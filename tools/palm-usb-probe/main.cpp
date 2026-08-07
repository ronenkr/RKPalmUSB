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

// After a session the handheld stays enumerated until its own ~60 s timeout expires, and
// it will not start a new conversation before then. Re-arming immediately just reopens a
// dead session, so wait for it to leave the bus first.
void WaitForDeviceGone(int timeout_seconds) {
    if (Enumerate().empty()) return;

    printf("Waiting for the handheld to drop off the bus (its own timeout, up to ~60 s)");
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_seconds) * 1000;
    while (!Enumerate().empty()) {
        if (GetTickCount() > deadline) {
            printf("\n  still present after %d s - continuing anyway\n", timeout_seconds);
            return;
        }
        printf("-");
        fflush(stdout);
        Sleep(1000);
    }
    printf("\n  gone.\n");
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

// --- NetSync -----------------------------------------------------------------
//
// The handheld opens with Palm's NET protocol: a 6-byte header
//
//     type(1) txid(1) size(4, big-endian)
//
// followed by `size` payload bytes. Observed on Palm 0830:0040 at connect:
//
//     01 FF 00 00 00 16                          type 1, txid 0xFF, 22 bytes
//     90 01 00 00 00 00 00 00 00 20 ...          exactly 22 bytes
//
// pilot-link's net_handshake answers the device's 0x90 message with an otherwise
// identical 0x12 message, and the device then replies 0x92. That is the default here;
// `netsync <hex>` overrides the reply payload for experimentation.

constexpr size_t kNetHeaderLen = 6;

// Bulk reads return whole packets, so buffer and hand out exact byte counts.
struct Reader {
    Device* device = nullptr;
    std::vector<UCHAR> buffer;
    size_t head = 0;

    bool Fill() {
        if (head > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + head);
            head = 0;
        }
        const size_t offset = buffer.size();
        buffer.resize(offset + device->max_packet_in);
        ULONG transferred = 0;
        const BOOL ok = WinUsb_ReadPipe(device->winusb, device->pipe_in,
                                        buffer.data() + offset,
                                        device->max_packet_in, &transferred, nullptr);
        buffer.resize(offset + (ok ? transferred : 0));
        if (!ok) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) return true;  // idle
            printf("  ReadPipe failed: %lu\n", error);
            return false;
        }
        return true;
    }

    // Blocks until `count` bytes are available or the deadline passes.
    bool Take(size_t count, UCHAR* out, int timeout_ms) {
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
        while (buffer.size() - head < count) {
            if (!Fill()) return false;
            if (GetTickCount() > deadline) return false;
        }
        memcpy(out, buffer.data() + head, count);
        head += count;
        return true;
    }
};

bool NetSend(Device& device, UCHAR type, UCHAR txid, const UCHAR* body, size_t length) {
    std::vector<UCHAR> frame(kNetHeaderLen + length);
    frame[0] = type;
    frame[1] = txid;
    frame[2] = static_cast<UCHAR>((length >> 24) & 0xFF);
    frame[3] = static_cast<UCHAR>((length >> 16) & 0xFF);
    frame[4] = static_cast<UCHAR>((length >> 8) & 0xFF);
    frame[5] = static_cast<UCHAR>(length & 0xFF);
    if (length > 0) memcpy(frame.data() + kNetHeaderLen, body, length);

    printf("  TX %zu bytes (type 0x%02X txid 0x%02X len %zu):\n", frame.size(), type, txid,
           length);
    HexDump(frame.data(), frame.size());

    ULONG sent = 0;
    if (!WinUsb_WritePipe(device.winusb, device.pipe_out, frame.data(),
                          static_cast<ULONG>(frame.size()), &sent, nullptr)) {
        printf("  WritePipe failed: %lu\n", GetLastError());
        return false;
    }
    return true;
}

const char* DlpErrorName(unsigned code) {
    switch (code) {
        case 0:  return "none";
        case 1:  return "system";
        case 2:  return "illegal request - the device rejected this command";
        case 3:  return "out of memory";
        case 4:  return "invalid parameter";
        case 5:  return "not found";
        case 6:  return "none open";
        case 7:  return "already open";
        case 8:  return "too many open";
        case 9:  return "already exists";
        case 10: return "cannot open";
        case 11: return "record deleted";
        case 12: return "record busy";
        case 13: return "unsupported operation";
        case 15: return "read only";
        default: return "unknown";
    }
}

std::vector<UCHAR> ParseHex(const char* text) {
    std::vector<UCHAR> bytes;
    if (text == nullptr) return bytes;
    const char* p = text;
    while (*p && p[1]) {
        if (*p == ' ' || *p == ',') { ++p; continue; }
        bytes.push_back(static_cast<UCHAR>(strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return bytes;
}

bool NetRxHandshake(Device& device, Reader& reader, bool verbose);
bool DlpExchange(Device& device, Reader& reader, UCHAR txid,
                 const std::vector<UCHAR>& request, std::vector<UCHAR>& response);

// One button press: wait for the device, handshake, issue the DLP request, report.
// Returns false when the caller should stop looping.
bool RunNetSyncSession(const std::vector<UCHAR>& request) {
    const std::string path = WaitForDevice(120);
    if (path.empty()) return false;

    Device device;
    if (!Open(device, path, true)) return true;

    ULONG timeout = 1000;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(device.winusb, device.pipe_out, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    UCHAR auto_clear = TRUE;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, AUTO_CLEAR_STALL,
                         sizeof(auto_clear), &auto_clear);

    Reader reader;
    reader.device = &device;

    printf("\nNetSync on IN 0x%02X / OUT 0x%02X.\n\n", device.pipe_in, device.pipe_out);

    if (!NetRxHandshake(device, reader, true)) {
        Close(device);
        return true;
    }

    std::vector<UCHAR> response;
    if (!DlpExchange(device, reader, 0x01, request, response)) {
        printf("  no DLP response.\n");
        Close(device);
        return true;
    }
    if (response.size() >= 4) {
        const unsigned error = (static_cast<unsigned>(response[2]) << 8) | response[3];
        printf("  DLP 0x%02X argc %u err %u (%s)\n", response[0], response[1], error,
               DlpErrorName(error));
        HexDump(response.data(), response.size());
        if (error == 0) printf("\n  *** DLP ACCEPTED THE REQUEST ***\n");
    }
    Close(device);
    return true;
}

// The NET handshake is asymmetric. The handheld runs net_tx_handshake(), so the host must
// run net_rx_handshake() (pilot-link libpisock/net.c):
//
//   device: TX 22 -> RX 50 -> TX 50 -> RX 46 -> TX 8
//   host:   RX    -> TX 50 -> RX 50 -> TX 46 -> RX 8
//
// Replying with a 22-byte echo instead of the 50-byte message, and never sending the
// 46-byte follow-up, makes every later DLP call fail with "invalid parameter". The
// embedded 0xC0A8A51F / 0x0427 are an IP address and port left over from NetSync's TCP
// origins; pilot-link sends them unchanged over USB and the device accepts them.
const UCHAR kNetRxMsg1[50] = {
    0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x24, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x00,
    0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc0, 0xa8, 0xa5, 0x1f, 0x04, 0x27, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const UCHAR kNetRxMsg2[46] = {
    0x13, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x20, 0xff, 0xff, 0xff, 0xff, 0x00, 0x3c,
    0x00, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Reads one NET frame into `body`, returning the header's txid, or -1 on failure.
int NetReceive(Reader& reader, std::vector<UCHAR>& body, int timeout_ms) {
    UCHAR header[kNetHeaderLen] = {};
    if (!reader.Take(kNetHeaderLen, header, timeout_ms)) return -1;
    const size_t size = (static_cast<size_t>(header[2]) << 24) |
                        (static_cast<size_t>(header[3]) << 16) |
                        (static_cast<size_t>(header[4]) << 8) |
                        static_cast<size_t>(header[5]);
    if (size > 64 * 1024) return -1;
    body.assign(size, 0);
    if (size > 0 && !reader.Take(size, body.data(), timeout_ms)) return -1;
    return header[1];
}

// Performs the receiving side of the NET handshake. Returns false if it did not complete.
bool NetRxHandshake(Device& device, Reader& reader, bool verbose) {
    std::vector<UCHAR> body;

    const int txid = NetReceive(reader, body, 30000);
    if (txid < 0) {
        printf("  no greeting from the handheld\n");
        return false;
    }
    if (verbose) {
        printf("  RX greeting: %zu bytes, first byte 0x%02X\n", body.size(),
               body.empty() ? 0 : body[0]);
    }

    if (verbose) printf("  TX handshake msg1 (50 bytes)\n");
    ULONG sent = 0;
    std::vector<UCHAR> frame(kNetHeaderLen + sizeof(kNetRxMsg1));
    frame[0] = 0x01;
    frame[1] = static_cast<UCHAR>(txid);
    frame[2] = 0; frame[3] = 0; frame[4] = 0;
    frame[5] = static_cast<UCHAR>(sizeof(kNetRxMsg1));
    memcpy(frame.data() + kNetHeaderLen, kNetRxMsg1, sizeof(kNetRxMsg1));
    if (!WinUsb_WritePipe(device.winusb, device.pipe_out, frame.data(),
                          static_cast<ULONG>(frame.size()), &sent, nullptr)) {
        printf("  write failed: %lu\n", GetLastError());
        return false;
    }

    if (NetReceive(reader, body, 5000) < 0) {
        printf("  no response to handshake msg1\n");
        return false;
    }
    if (verbose) {
        printf("  RX %zu bytes, first byte 0x%02X\n", body.size(),
               body.empty() ? 0 : body[0]);
    }

    if (verbose) printf("  TX handshake msg2 (46 bytes)\n");
    frame.assign(kNetHeaderLen + sizeof(kNetRxMsg2), 0);
    frame[0] = 0x01;
    frame[1] = static_cast<UCHAR>(txid);
    frame[5] = static_cast<UCHAR>(sizeof(kNetRxMsg2));
    memcpy(frame.data() + kNetHeaderLen, kNetRxMsg2, sizeof(kNetRxMsg2));
    if (!WinUsb_WritePipe(device.winusb, device.pipe_out, frame.data(),
                          static_cast<ULONG>(frame.size()), &sent, nullptr)) {
        printf("  write failed: %lu\n", GetLastError());
        return false;
    }

    if (NetReceive(reader, body, 5000) < 0) {
        printf("  no response to handshake msg2\n");
        return false;
    }
    if (verbose) {
        printf("  RX %zu bytes, first byte 0x%02X\n", body.size(),
               body.empty() ? 0 : body[0]);
    }

    printf("  handshake complete.\n\n");
    return true;
}

// Sends one DLP request and reads its response. Returns false if the link died.
bool DlpExchange(Device& device, Reader& reader, UCHAR txid,
                 const std::vector<UCHAR>& request, std::vector<UCHAR>& response) {
    std::vector<UCHAR> frame(kNetHeaderLen + request.size());
    frame[0] = 0x01;
    frame[1] = txid;
    frame[2] = 0;
    frame[3] = 0;
    frame[4] = static_cast<UCHAR>((request.size() >> 8) & 0xFF);
    frame[5] = static_cast<UCHAR>(request.size() & 0xFF);
    memcpy(frame.data() + kNetHeaderLen, request.data(), request.size());

    ULONG sent = 0;
    if (!WinUsb_WritePipe(device.winusb, device.pipe_out, frame.data(),
                          static_cast<ULONG>(frame.size()), &sent, nullptr)) {
        return false;
    }

    UCHAR header[kNetHeaderLen] = {};
    if (!reader.Take(kNetHeaderLen, header, 4000)) return false;
    const size_t size = (static_cast<size_t>(header[2]) << 24) |
                        (static_cast<size_t>(header[3]) << 16) |
                        (static_cast<size_t>(header[4]) << 8) |
                        static_cast<size_t>(header[5]);
    if (size > 64 * 1024) return false;

    response.assign(size, 0);
    if (size > 0 && !reader.Take(size, response.data(), 4000)) return false;
    return true;
}

// Walks the tiny/short/long argument encoding of a DLP response body, calling `visit`
// with each argument's ordinal position, length and data. Layout per pilot-link
// libpisock/dlp.c.
//
// Dispatch on the POSITION, never on the id. Ids start at PI_DLP_ARG_FIRST_ID (0x20), and
// pilot-link's own accessor - DLP_RESPONSE_DATA(res, index, offset) - indexes by position
// too. Matching against id 0x00/0x01 silently decodes nothing, which is exactly what the
// first version of this did.
template <typename Visit>
void ForEachDlpArg(const std::vector<UCHAR>& body, Visit visit) {
    if (body.size() < 4) return;
    size_t offset = 4;
    for (UCHAR i = 0; i < body[1] && offset + 2 <= body.size(); ++i) {
        const UCHAR raw_id = body[offset];
        size_t len = 0;
        size_t data_at = 0;
        if (raw_id & 0x80) {          // short
            if (offset + 4 > body.size()) return;
            len = (static_cast<size_t>(body[offset + 2]) << 8) | body[offset + 3];
            data_at = offset + 4;
        } else if (raw_id & 0x40) {   // long
            if (offset + 6 > body.size()) return;
            len = (static_cast<size_t>(body[offset + 2]) << 24) |
                  (static_cast<size_t>(body[offset + 3]) << 16) |
                  (static_cast<size_t>(body[offset + 4]) << 8) | body[offset + 5];
            data_at = offset + 6;
        } else {                      // tiny
            len = body[offset + 1];
            data_at = offset + 2;
        }
        if (data_at + len > body.size()) return;
        visit(i, body.data() + data_at, len);
        offset = data_at + len;
    }
}

unsigned Be16(const UCHAR* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
unsigned long Be32(const UCHAR* p) {
    return (static_cast<unsigned long>(p[0]) << 24) |
           (static_cast<unsigned long>(p[1]) << 16) |
           (static_cast<unsigned long>(p[2]) << 8) | p[3];
}

// Reports why a DLP call produced nothing, so "no output" always has a printed reason.
bool DlpOk(bool exchanged, const std::vector<UCHAR>& response, const char* what) {
    if (!exchanged) {
        printf("  %s: no response\n", what);
        return false;
    }
    if (response.size() < 4) {
        printf("  %s: short response (%zu bytes)\n", what, response.size());
        return false;
    }
    const unsigned error = Be16(response.data() + 2);
    if (error != 0) {
        printf("  %s: error %u (%s)\n", what, error, DlpErrorName(error));
        return false;
    }
    return true;
}

// A successful call whose arguments we failed to interpret is a bug in the decoder, not an
// empty device. Print the bytes rather than nothing.
void DumpUndecoded(const std::vector<UCHAR>& response) {
    printf("  (argc %u, no argument decoded - raw response follows)\n", response[1]);
    HexDump(response.data(), response.size());
}

// Palm date-time: year(2) month day hour minute second pad
void PrintPalmTime(const char* label, const UCHAR* p) {
    printf("  %-18s %u-%02u-%02u %02u:%02u:%02u\n", label, Be16(p), p[2], p[3], p[4], p[5],
           p[6]);
}

// Everything the handheld is asked lives in ONE connection. It only stays on the bus for
// a few seconds per HotSync press and then takes ~60 s to time out, so probing one
// payload per session is unusable - sweep the whole space while the link is up.
int CommandSweep() {
    printf("Open the connection, then press the HotSync button once.\n"
           "Every probe below runs inside that single session.\n\n");

    const std::string path = WaitForDevice(180);
    if (path.empty()) return 1;

    Device device;
    if (!Open(device, path, true)) return 1;

    ULONG timeout = 800;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(device.winusb, device.pipe_out, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    UCHAR auto_clear = TRUE;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, AUTO_CLEAR_STALL,
                         sizeof(auto_clear), &auto_clear);

    Reader reader;
    reader.device = &device;

    if (!NetRxHandshake(device, reader, true)) {
        Close(device);
        return 1;
    }

    // --- build the probe list ---
    struct Probe {
        std::string name;
        std::vector<UCHAR> payload;
    };
    std::vector<Probe> probes;

    // ReadSysInfo FIRST: it negotiates the DLP version, and the device rejects other
    // commands until that has happened.
    //
    // dlpFuncReadSysInfo is 0x12 (pilot-link include/pi-dlp.h; the enum starts at
    // dlpReservedFunc = 0x0F). Argument id is PI_DLP_ARG_FIRST_ID 0x20 with the TINY
    // flag 0x00 - the argument id is emphatically not the command, which is the mistake
    // that made every earlier probe fail.
    for (UCHAR minor = 0; minor <= 4; ++minor) {
        char name[64];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "ReadSysInfo v1.%u", minor);
        probes.push_back({name, {0x12, 0x01, 0x20, 0x04, 0x00, 0x01, 0x00, minor}});
    }
    probes.push_back({"ReadSysInfo v2.0",
                      {0x12, 0x01, 0x20, 0x04, 0x00, 0x02, 0x00, 0x00}});
    probes.push_back({"ReadSysInfo no-arg", {0x12, 0x00}});

    // Then ReadUserInfo, which takes no arguments.
    probes.push_back({"ReadUserInfo", {0x10, 0x00}});

    // Then a few more opcodes with no arguments. Kept short: the handheld drops off the
    // bus partway through a long sweep, so spend the session on the calls that matter.
    for (UCHAR cmd = 0x13; cmd <= 0x20; ++cmd) {
        char name[64];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "cmd 0x%02X argc=0", cmd);
        probes.push_back({name, {cmd, 0x00}});
    }

    printf("Running %zu probes...\n\n", probes.size());

    std::vector<std::string> interesting;
    UCHAR txid = 0x00;

    for (const Probe& probe : probes) {
        std::vector<UCHAR> response;
        if (!DlpExchange(device, reader, ++txid, probe.payload, response)) {
            printf("  %-24s <no response - link lost, stopping>\n", probe.name.c_str());
            break;
        }
        if (response.size() < 4) {
            printf("  %-24s short response (%zu bytes)\n", probe.name.c_str(),
                   response.size());
            continue;
        }

        const unsigned error = (static_cast<unsigned>(response[2]) << 8) | response[3];
        printf("  %-24s -> 0x%02X argc %u err %u (%s)%s\n", probe.name.c_str(),
               response[0], response[1], error, DlpErrorName(error),
               response.size() > 4 ? "  [+data]" : "");

        if (error != 2) {
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%s -> err %u (%s), %zu bytes", probe.name.c_str(), error,
                        DlpErrorName(error), response.size());
            interesting.push_back(line);
            if (response.size() > 4) HexDump(response.data(), response.size());
        }
    }

    printf("\n================ probes that were NOT 'illegal request' ================\n");
    if (interesting.empty()) {
        printf("  none - every command was rejected the same way, so the request\n"
               "  framing itself is wrong, not the choice of command.\n");
    } else {
        for (const std::string& line : interesting) printf("  %s\n", line.c_str());
    }

    Close(device);
    return 0;
}

// Reads and decodes the handheld's identity: OS version, DLP version, user name and sync
// history. This is the end-to-end proof that WinUSB + NET + DLP all work.
int CommandInfo() {
    printf("Press the HotSync button once.\n\n");

    const std::string path = WaitForDevice(180);
    if (path.empty()) return 1;

    Device device;
    if (!Open(device, path, false)) return 1;

    ULONG timeout = 2000;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(device.winusb, device.pipe_out, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    UCHAR auto_clear = TRUE;
    WinUsb_SetPipePolicy(device.winusb, device.pipe_in, AUTO_CLEAR_STALL,
                         sizeof(auto_clear), &auto_clear);

    Reader reader;
    reader.device = &device;
    if (!NetRxHandshake(device, reader, false)) {
        Close(device);
        return 1;
    }

    UCHAR txid = 0;
    std::vector<UCHAR> response;

    printf("--- system ---\n");
    if (DlpOk(DlpExchange(device, reader, ++txid,
                          {0x12, 0x01, 0x20, 0x04, 0x00, 0x01, 0x00, 0x04}, response),
              response, "ReadSysInfo")) {
        int decoded = 0;
        ForEachDlpArg(response, [&](UCHAR index, const UCHAR* d, size_t len) {
            // arg 0: romVersion(4) locale(4) pad(1) prodIDLength(1) prodID...
            if (index == 0 && len >= 10) {
                const unsigned long rom = Be32(d);
                printf("  %-18s %lu.%lu.%lu (0x%08lX)\n", "Palm OS", (rom >> 24) & 0xFF,
                       (rom >> 20) & 0x0F, (rom >> 16) & 0x0F, rom);
                printf("  %-18s 0x%08lX\n", "locale", Be32(d + 4));
                const UCHAR id_len = d[9];
                if (id_len > 0 && 10 + id_len <= len) {
                    printf("  %-18s \"%.*s\"\n", "product id", id_len,
                           reinterpret_cast<const char*>(d + 10));
                }
                ++decoded;
            // arg 1: dlpVer(2,2) compatVer(2,2) maxRecSize(4)
            } else if (index == 1 && len >= 8) {
                printf("  %-18s %u.%u\n", "device DLP", Be16(d), Be16(d + 2));
                printf("  %-18s %u.%u\n", "compat DLP", Be16(d + 4), Be16(d + 6));
                if (len >= 12) printf("  %-18s %lu\n", "max record", Be32(d + 8));
                ++decoded;
            }
        });
        if (decoded == 0) DumpUndecoded(response);
    }

    printf("\n--- user ---\n");
    if (DlpOk(DlpExchange(device, reader, ++txid, {0x10, 0x00}, response), response,
              "ReadUserInfo")) {
        int decoded = 0;
        ForEachDlpArg(response, [&](UCHAR index, const UCHAR* d, size_t len) {
            if (index != 0 || len < 30) return;
            // userID(4) viewerID(4) lastSyncPC(4) succSyncDate(8) lastSyncDate(8)
            // userNameLen(1) passwordLen(1) userName...
            printf("  %-18s %lu\n", "user id", Be32(d));
            printf("  %-18s 0x%08lX\n", "last sync PC", Be32(d + 8));
            PrintPalmTime("last successful", d + 12);
            PrintPalmTime("last sync", d + 20);
            const UCHAR name_len = d[28];   // includes the NUL
            if (name_len > 1 && 30 + name_len <= len) {
                printf("  %-18s \"%.*s\"\n", "user name", name_len - 1,
                       reinterpret_cast<const char*>(d + 30));
            }
            ++decoded;
        });
        if (decoded == 0) DumpUndecoded(response);
    }

    printf("\n--- clock ---\n");
    if (DlpOk(DlpExchange(device, reader, ++txid, {0x13, 0x00}, response), response,
              "ReadSysDateTime")) {
        int decoded = 0;
        ForEachDlpArg(response, [&](UCHAR index, const UCHAR* d, size_t len) {
            if (index == 0 && len >= 7) {
                PrintPalmTime("device time", d);
                ++decoded;
            }
        });
        if (decoded == 0) DumpUndecoded(response);
    }

    Close(device);
    printf("\nWinUSB -> NET -> DLP all working.\n");
    return 0;
}

// `request_hex` is the DLP request payload to send once the handshake completes. The
// handshake itself is no longer overridable - it is the fixed two-message pilot-link
// exchange in NetRxHandshake, and every variation tried against this handheld failed.
//
// Loops until Ctrl+C, re-arming after each session. The handheld drops off the bus
// seconds after the button press and takes ~60 s to time out, so re-running the whole
// command per attempt is painfully slow; this way one invocation serves many presses.
int CommandNetSync(const char* request_hex) {
    // dlp_ReadSysInfo, exactly as pilot-link builds it (libpisock/dlp.c). Note the
    // command is 0x12, NOT 0x20 - 0x20 is PI_DLP_ARG_FIRST_ID, the argument id, which is
    // easy to confuse with the opcode and produces "illegal request" forever.
    //   12    dlpFuncReadSysInfo
    //   01    argc
    //   20    arg id 0x20 | PI_DLP_ARG_FLAG_TINY (0x00)
    //   04    arg length
    //   0001  PI_DLP_VERSION_MAJOR = 1
    //   0004  PI_DLP_VERSION_MINOR = 4
    std::vector<UCHAR> request = ParseHex(request_hex);
    if (request.empty()) {
        request = {0x12, 0x01, 0x20, 0x04, 0x00, 0x01, 0x00, 0x04};
    }

    printf("DLP request : ");
    for (UCHAR byte : request) printf("%02X ", byte);
    printf("\n\nCtrl+C to stop. Press the HotSync button for each attempt.\n");

    for (;;) {
        if (!RunNetSyncSession(request)) break;
        printf("\n------------------------- next attempt -------------------------\n\n");
        WaitForDeviceGone(90);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: palm-usb-probe <command>\n\n"
               "  list                enumerate bound devices\n"
               "  descriptors         descriptors and endpoints\n"
               "  handshake           ask the device which endpoints carry HotSync\n"
               "  listen [endpoint]   wait for arrival, then dump incoming bytes\n"
               "                      optional endpoint forces an IN pipe, e.g. 0x81\n"
               "  info                ONE session: handshake, then decode the device's\n"
               "                      system info, user info and clock. Press HotSync once.\n"
               "  sweep               ONE session: handshake, then try every DLP opcode\n"
               "                      and several request encodings, reporting what the\n"
               "                      device accepts. Press HotSync once.\n"
               "  netsync [dlp]       speak Palm NET, then issue one raw DLP request.\n"
               "                      Loops: press the HotSync button per attempt.\n"
               "                        dlp  request hex, default 1201200400010004\n"
               "                             (dlpFuncReadSysInfo)\n"
               "                      e.g. netsync 1000        (dlpFuncReadUserInfo)\n");
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
    if (command == "netsync") return CommandNetSync(argc >= 3 ? argv[2] : nullptr);
    if (command == "sweep") return CommandSweep();
    if (command == "info") return CommandInfo();

    printf("unknown command: %s\n", command.c_str());
    return 2;
}
