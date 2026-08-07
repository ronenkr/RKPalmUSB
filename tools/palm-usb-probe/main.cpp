// palm-usb-probe - talks to a Palm device through WinUSB without Palm Desktop in the way.
//
//   palm-usb-probe list          enumerate bound devices
//   palm-usb-probe descriptors   device/config/string descriptors and endpoints
//   palm-usb-probe handshake     the Palm vendor requests that report the sync endpoints
//   palm-usb-probe listen        poll the IN pipe and hexdump whatever arrives
//
// `listen` is the acceptance test for the WinUSB package: press the HotSync button and
// bytes starting BE EF ED (Palm SLP framing) should appear.

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Same interface GUID the replacement USBPort.dll and PalmWinUSB.inf use.
const GUID kPalmInterface = {
    0x784126bf, 0x4190, 0x11d4, {0xb5, 0xc2, 0x00, 0xc0, 0x4f, 0x68, 0x7a, 0x67}};

struct Device {
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    UCHAR pipe_in = 0;
    UCHAR pipe_out = 0;
    ULONG max_packet_in = 64;
};

std::vector<std::string> Enumerate() {
    std::vector<std::string> paths;
    HDEVINFO set = SetupDiGetClassDevsA(&kPalmInterface, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;

    SP_DEVICE_INTERFACE_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &kPalmInterface, i, &data); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &data, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<char> storage(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(set, &data, detail, needed, nullptr, nullptr)) {
            paths.emplace_back(detail->DevicePath);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

bool QueryPipeMaxPacket(Device& device, UCHAR pipe_id, ULONG& max_packet) {
    USB_INTERFACE_DESCRIPTOR descriptor = {};
    if (!WinUsb_QueryInterfaceSettings(device.winusb, 0, &descriptor)) return false;
    for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(device.winusb, 0, i, &pipe)) continue;
        if (pipe.PipeId != pipe_id) continue;
        max_packet = pipe.MaximumPacketSize;
        return true;
    }
    return false;
}

// Asks the device which bulk pair carries HotSync. Palm handhelds often expose two pairs
// (e.g. 16-byte on endpoint 1, 64-byte on endpoint 2) and only one of them works, so this
// answer beats anything inferred from the descriptor.
bool RefineEndpoints(Device& device, bool verbose) {
    BYTE buffer[256] = {};
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = 0xC0;  // device-to-host, vendor, device
    setup.Request = 0x04;      // GET_EXT_CONNECTION_INFO
    setup.Length = sizeof(buffer);

    ULONG transferred = 0;
    if (!WinUsb_ControlTransfer(device.winusb, setup, buffer, sizeof(buffer), &transferred,
                                nullptr) ||
        transferred < 4) {
        if (verbose) {
            printf("  GET_EXT_CONNECTION_INFO unsupported (err %lu) - using descriptor\n",
                   GetLastError());
        }
        return false;
    }

    const BYTE count = buffer[0];
    const BYTE differ = buffer[1];
    for (BYTE i = 0; i < count && 4u + (i + 1u) * 8u <= transferred; ++i) {
        const BYTE* entry = buffer + 4 + i * 8;
        if (entry[4] != 0x00) continue;  // port type 0 == sync

        const UCHAR in_number = (entry[5] >> 4) & 0x0F;
        const UCHAR out_number = entry[5] & 0x0F;
        if (in_number == 0 && out_number == 0) continue;

        device.pipe_in = static_cast<UCHAR>(0x80 | in_number);
        device.pipe_out = differ ? out_number : in_number;

        ULONG max_packet = 0;
        if (QueryPipeMaxPacket(device, device.pipe_in, max_packet) && max_packet > 0) {
            device.max_packet_in = max_packet;
        }
        if (verbose) {
            printf("  device reports sync port: IN 0x%02X, OUT 0x%02X (maxpacket %lu)\n",
                   device.pipe_in, device.pipe_out, device.max_packet_in);
        }
        return true;
    }
    return false;
}

bool Open(Device& device, const std::string& path, bool verbose = false) {
    device.file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (device.file == INVALID_HANDLE_VALUE) {
        printf("  CreateFile failed: %lu\n", GetLastError());
        return false;
    }
    if (!WinUsb_Initialize(device.file, &device.winusb)) {
        printf("  WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(device.file);
        device.file = INVALID_HANDLE_VALUE;
        return false;
    }

    // Prefer the largest bulk pair: on multi-pair Palm devices the small pair is not the
    // sync port, and picking it makes every read stall with ERROR_GEN_FAILURE.
    USB_INTERFACE_DESCRIPTOR descriptor = {};
    WinUsb_QueryInterfaceSettings(device.winusb, 0, &descriptor);
    ULONG best_in = 0;
    ULONG best_out = 0;
    for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(device.winusb, 0, i, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            if (pipe.MaximumPacketSize > best_in) {
                device.pipe_in = pipe.PipeId;
                device.max_packet_in = pipe.MaximumPacketSize;
                best_in = pipe.MaximumPacketSize;
            }
        } else if (pipe.MaximumPacketSize > best_out) {
            device.pipe_out = pipe.PipeId;
            best_out = pipe.MaximumPacketSize;
        }
    }

    RefineEndpoints(device, verbose);
    return true;
}

// These handhelds only enumerate while the HotSync button is held/active, and drop off
// again after a few seconds. Polling for arrival lets you start the tool first and press
// the button second, which is the only workable order.
std::string WaitForDevice(int timeout_seconds) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_seconds) * 1000;
    bool announced = false;
    for (;;) {
        const auto paths = Enumerate();
        if (!paths.empty()) {
            if (announced) printf("\n");
            return paths[0];
        }
        if (!announced) {
            printf("Waiting for the device. Press the HotSync button now");
            announced = true;
        }
        if (GetTickCount() > deadline) {
            printf("\nTimed out after %d s.\n", timeout_seconds);
            return std::string();
        }
        printf("+");
        fflush(stdout);
        Sleep(250);
    }
}

void Close(Device& device) {
    if (device.winusb) WinUsb_Free(device.winusb);
    if (device.file != INVALID_HANDLE_VALUE) CloseHandle(device.file);
    device = Device{};
}

void HexDump(const unsigned char* data, size_t length) {
    for (size_t offset = 0; offset < length; offset += 16) {
        printf("    %04zX  ", offset);
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < length) printf("%02X ", data[offset + i]);
            else printf("   ");
        }
        printf(" |");
        for (size_t i = 0; i < 16 && offset + i < length; ++i) {
            const unsigned char c = data[offset + i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

int CommandList() {
    const auto paths = Enumerate();
    if (paths.empty()) {
        printf("No Palm interfaces present.\n");
        printf("Is the device plugged in and bound to winusb.sys?\n");
        printf("Check with: pnputil /enum-devices /connected /drivers\n");
        return 1;
    }
    printf("%zu device(s):\n", paths.size());
    for (const auto& path : paths) printf("  %s\n", path.c_str());
    return 0;
}

int CommandDescriptors() {
    const std::string path = WaitForDevice(60);
    if (path.empty()) return 1;

    Device device;
    if (!Open(device, path)) return 1;

    USB_DEVICE_DESCRIPTOR descriptor = {};
    ULONG transferred = 0;
    if (WinUsb_GetDescriptor(device.winusb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor),
                             &transferred)) {
        printf("VID:PID    %04X:%04X\n", descriptor.idVendor, descriptor.idProduct);
        printf("bcdDevice  %04X\n", descriptor.bcdDevice);
        printf("bcdUSB     %04X\n", descriptor.bcdUSB);
        printf("class      %02X/%02X/%02X\n", descriptor.bDeviceClass,
               descriptor.bDeviceSubClass, descriptor.bDeviceProtocol);
        printf("configs    %u\n", descriptor.bNumConfigurations);

        const UCHAR strings[] = {descriptor.iManufacturer, descriptor.iProduct,
                                 descriptor.iSerialNumber};
        const char* labels[] = {"manufacturer", "product", "serial"};
        for (int i = 0; i < 3; ++i) {
            if (strings[i] == 0) continue;
            UCHAR raw[256] = {};
            if (WinUsb_GetDescriptor(device.winusb, USB_STRING_DESCRIPTOR_TYPE, strings[i],
                                     0x0409, raw, sizeof(raw), &transferred) &&
                transferred > 2) {
                printf("%-11s %S\n", labels[i], reinterpret_cast<wchar_t*>(raw + 2));
            }
        }
    }

    USB_INTERFACE_DESCRIPTOR interface_desc = {};
    WinUsb_QueryInterfaceSettings(device.winusb, 0, &interface_desc);
    printf("\nendpoints (%u):\n", interface_desc.bNumEndpoints);
    for (UCHAR i = 0; i < interface_desc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(device.winusb, 0, i, &pipe)) continue;
        const char* type = pipe.PipeType == UsbdPipeTypeBulk      ? "bulk"
                           : pipe.PipeType == UsbdPipeTypeInterrupt ? "interrupt"
                           : pipe.PipeType == UsbdPipeTypeIsochronous ? "isoc"
                                                                    : "control";
        printf("  0x%02X  %-9s  %s  maxpacket %u\n", pipe.PipeId, type,
               USB_ENDPOINT_DIRECTION_IN(pipe.PipeId) ? "IN " : "OUT",
               pipe.MaximumPacketSize);
    }
    printf("\nselected: IN 0x%02X, OUT 0x%02X\n", device.pipe_in, device.pipe_out);

    Close(device);
    return 0;
}

int CommandHandshake() {
    const std::string path = WaitForDevice(60);
    if (path.empty()) return 1;

    Device device;
    if (!Open(device, path)) return 1;

    struct { BYTE request; const char* name; } requests[] = {
        {0x01, "GET_CONNECTION_INFO"},
        {0x04, "GET_EXT_CONNECTION_INFO"},
    };

    for (const auto& request : requests) {
        BYTE buffer[256] = {};
        WINUSB_SETUP_PACKET setup = {};
        setup.RequestType = 0xC0;  // device-to-host, vendor, device
        setup.Request = request.request;
        setup.Length = sizeof(buffer);

        ULONG transferred = 0;
        printf("%s (0x%02X): ", request.name, request.request);
        if (WinUsb_ControlTransfer(device.winusb, setup, buffer, sizeof(buffer),
                                   &transferred, nullptr)) {
            printf("%lu bytes\n", transferred);
            HexDump(buffer, transferred);

            if (request.request == 0x04 && transferred >= 4) {
                const BYTE count = buffer[0];
                const BYTE differ = buffer[1];
                printf("  ports: %u, endpoints differ: %u\n", count, differ);
                for (BYTE i = 0; i < count && 4u + (i + 1u) * 8u <= transferred; ++i) {
                    const BYTE* entry = buffer + 4 + i * 8;
                    printf("    '%.4s' type %u  IN 0x%02X  OUT 0x%02X\n", entry,
                           entry[4], 0x80 | (entry[5] >> 4), entry[5] & 0x0F);
                }
            }
        } else {
            printf("not supported (err %lu)\n", GetLastError());
        }
    }

    Close(device);
    return 0;
}

// `endpoint_override` forces a specific IN pipe id (e.g. 0x81) for experimentation.
int CommandListen(UCHAR endpoint_override) {
    printf("Ctrl+C to stop. The handheld only stays enumerated for a few seconds after\n"
           "the HotSync button, so this reconnects automatically.\n\n");

    for (;;) {
        const std::string path = WaitForDevice(120);
        if (path.empty()) return 1;

        Device device;
        if (!Open(device, path, true)) {
            Sleep(500);
            continue;
        }

        if (endpoint_override != 0) {
            device.pipe_in = endpoint_override;
            ULONG max_packet = 0;
            if (QueryPipeMaxPacket(device, device.pipe_in, max_packet) && max_packet > 0) {
                device.max_packet_in = max_packet;
            }
            printf("  override: IN 0x%02X (maxpacket %lu)\n", device.pipe_in,
                   device.max_packet_in);
        }

        ULONG timeout = 500;
        WinUsb_SetPipePolicy(device.winusb, device.pipe_in, PIPE_TRANSFER_TIMEOUT,
                             sizeof(timeout), &timeout);
        UCHAR auto_clear = TRUE;
        WinUsb_SetPipePolicy(device.winusb, device.pipe_in, AUTO_CLEAR_STALL,
                             sizeof(auto_clear), &auto_clear);

        printf("Listening on IN 0x%02X (maxpacket %lu)...\n", device.pipe_in,
               device.max_packet_in);

        std::vector<UCHAR> buffer(device.max_packet_in);
        int stalls = 0;
        for (;;) {
            ULONG transferred = 0;
            if (WinUsb_ReadPipe(device.winusb, device.pipe_in, buffer.data(),
                                static_cast<ULONG>(buffer.size()), &transferred,
                                nullptr)) {
                stalls = 0;
                if (transferred > 0) {
                    printf("\n%lu bytes:\n", transferred);
                    HexDump(buffer.data(), transferred);
                    if (transferred >= 3 && buffer[0] == 0xBE && buffer[1] == 0xEF &&
                        buffer[2] == 0xED) {
                        printf("  ^ Palm SLP framing - THIS IS THE RIGHT ENDPOINT.\n");
                    }
                }
                continue;
            }

            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) {
                printf(".");
                fflush(stdout);
                continue;
            }

            printf("\nReadPipe failed: %lu", error);
            if (error == ERROR_GEN_FAILURE) {
                // Either the endpoint stalled (wrong pipe) or the device went away. They
                // are indistinguishable from the error code alone, so try clearing the
                // stall a couple of times before giving up on the connection.
                printf(" (ERROR_GEN_FAILURE - stalled pipe or device gone)\n");
                if (++stalls <= 2) {
                    WinUsb_ResetPipe(device.winusb, device.pipe_in);
                    Sleep(200);
                    continue;
                }
                printf("Giving up on this connection.\n");
                if (endpoint_override == 0) {
                    printf("Hint: run 'palm-usb-probe handshake' to see which endpoints\n"
                           "the device wants, or force one with 'listen 0x81'.\n");
                }
            } else {
                printf("\n");
            }
            break;
        }

        Close(device);
        printf("\nDevice gone - waiting for it to come back.\n\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: palm-usb-probe <command>\n\n"
               "  list                enumerate bound devices\n"
               "  descriptors         descriptors and endpoints\n"
               "  handshake           ask the device which endpoints carry HotSync\n"
               "  listen [endpoint]   wait for arrival, then dump incoming bytes\n"
               "                      optional endpoint forces an IN pipe, e.g. 0x81\n");
        return 2;
    }

    const std::string command = argv[1];
    if (command == "list") return CommandList();
    if (command == "descriptors") return CommandDescriptors();
    if (command == "handshake") return CommandHandshake();
    if (command == "listen") {
        UCHAR endpoint = 0;
        if (argc >= 3) {
            endpoint = static_cast<UCHAR>(strtoul(argv[2], nullptr, 0));
        }
        return CommandListen(endpoint);
    }

    printf("unknown command: %s\n", command.c_str());
    return 2;
}
