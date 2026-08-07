// WinUSB transport. Replaces the private IOCTL interface of the obsolete Palm/Aceeca
// kernel driver with Microsoft's in-box winusb.sys.

#include "palmusb.h"

#include <setupapi.h>

#include <cstdlib>
#include <cstring>
#include <new>

extern "C" const GUID GUID_DEVINTERFACE_PALM_USB = {
    0x784126bf, 0x4190, 0x11d4, {0xb5, 0xc2, 0x00, 0xc0, 0x4f, 0x68, 0x7a, 0x67}};

namespace {

// Palm vendor requests, per pilot-link libpisock/usb.c. Devices report which bulk
// endpoints to use for HotSync rather than always using the first pair in the
// interface descriptor.
constexpr BYTE kRequestGetConnectionInfo = 0x01;
constexpr BYTE kRequestGetExtConnectionInfo = 0x04;
constexpr BYTE kPalmPortTypeSync = 0x00;

#pragma pack(push, 1)
struct PalmExtConnectionEntry {
    char port_name[4];
    BYTE port_type;
    BYTE endpoint_info;  // high nibble = IN endpoint, low nibble = OUT endpoint
    WORD reserved;
};

struct PalmExtConnectionInfo {
    BYTE port_count;
    BYTE endpoint_numbers_different;
    WORD reserved;
    PalmExtConnectionEntry entries[20];
};
#pragma pack(pop)

// Looks up the max packet size of one pipe. Must be called whenever pipe_in changes:
// RAW_IO requires every read buffer to be a whole multiple of this value, so a stale
// size against a different endpoint makes ReadPipe fail outright.
bool QueryPipeMaxPacket(PalmPort& port, UCHAR pipe_id, ULONG& max_packet) {
    USB_INTERFACE_DESCRIPTOR interface_desc = {};
    if (!WinUsb_QueryInterfaceSettings(port.winusb, 0, &interface_desc)) return false;

    for (UCHAR i = 0; i < interface_desc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(port.winusb, 0, i, &pipe)) continue;
        if (pipe.PipeId != pipe_id) continue;
        max_packet = pipe.MaximumPacketSize;
        return true;
    }
    return false;
}

// Picks a bulk IN/OUT pair from the interface descriptor, used when the device does not
// answer the vendor requests below.
//
// Palm handhelds commonly expose more than one bulk pair - a Palm m-series reports a
// 16-byte pair on endpoint 1 and a 64-byte pair on endpoint 2, and only the latter
// carries HotSync. Taking the *first* pair therefore picks the wrong one and every read
// stalls with ERROR_GEN_FAILURE, so prefer the largest max packet size instead.
bool DiscoverEndpointsFromDescriptor(PalmPort& port) {
    USB_INTERFACE_DESCRIPTOR interface_desc = {};
    if (!WinUsb_QueryInterfaceSettings(port.winusb, 0, &interface_desc)) return false;

    ULONG best_in = 0;
    ULONG best_out = 0;
    bool have_in = false;
    bool have_out = false;

    for (UCHAR i = 0; i < interface_desc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(port.winusb, 0, i, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;

        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            if (!have_in || pipe.MaximumPacketSize > best_in) {
                port.pipe_in = pipe.PipeId;
                best_in = pipe.MaximumPacketSize;
                port.max_packet_in = pipe.MaximumPacketSize;
                have_in = true;
            }
        } else {
            if (!have_out || pipe.MaximumPacketSize > best_out) {
                port.pipe_out = pipe.PipeId;
                best_out = pipe.MaximumPacketSize;
                have_out = true;
            }
        }
    }
    PALMLOG("descriptor endpoints in=0x%02X out=0x%02X maxpacket=%lu", port.pipe_in,
            port.pipe_out, port.max_packet_in);
    return have_in && have_out;
}

// Asks the device which endpoints carry the HotSync port. Failure is not fatal - the
// descriptor-derived pair stays in effect. Returns false if the device did not answer.
//
// Confirmed unsupported on Palm 0830:0040, which stalls both vendor requests and works
// fine on the descriptor's largest bulk pair.
bool RefineEndpointsFromDevice(PalmPort& port) {
    PalmExtConnectionInfo info = {};
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = 0xC0;  // device-to-host, vendor, device
    setup.Request = kRequestGetExtConnectionInfo;
    setup.Length = sizeof(info);

    ULONG transferred = 0;
    if (!WinUsb_ControlTransfer(port.winusb, setup, reinterpret_cast<PUCHAR>(&info),
                                sizeof(info), &transferred, nullptr)) {
        PALMLOG("GetExtConnectionInfo unsupported (err %lu), keeping descriptor endpoints",
                GetLastError());
        return false;
    }

    const BYTE count = info.port_count > 20 ? 20 : info.port_count;
    for (BYTE i = 0; i < count; ++i) {
        const PalmExtConnectionEntry& entry = info.entries[i];
        if (entry.port_type != kPalmPortTypeSync) continue;

        const UCHAR in_number = (entry.endpoint_info >> 4) & 0x0F;
        const UCHAR out_number = entry.endpoint_info & 0x0F;
        if (in_number == 0 && out_number == 0) continue;

        // endpoint_numbers_different == 0 means the device reports one number for both.
        port.pipe_in = static_cast<UCHAR>(0x80 | in_number);
        port.pipe_out = info.endpoint_numbers_different ? out_number : in_number;

        // The chosen endpoint may have a different max packet size than the one the
        // descriptor scan settled on; RAW_IO reads fail if this is not refreshed.
        ULONG max_packet = 0;
        if (QueryPipeMaxPacket(port, port.pipe_in, max_packet) && max_packet > 0) {
            port.max_packet_in = max_packet;
        }

        PALMLOG("device reports sync port on in=0x%02X out=0x%02X maxpacket=%lu",
                port.pipe_in, port.pipe_out, port.max_packet_in);
        return true;
    }
    return false;
}

// Some Palm models need this before they will talk. Harmless where unsupported.
void SendGetConnectionInfo(PalmPort& port) {
    BYTE scratch[64] = {};
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = 0xC0;
    setup.Request = kRequestGetConnectionInfo;
    setup.Length = sizeof(scratch);

    ULONG transferred = 0;
    WinUsb_ControlTransfer(port.winusb, setup, scratch, sizeof(scratch), &transferred,
                           nullptr);
}

void ApplyPipePolicy(PalmPort& port) {
    UCHAR raw_io = TRUE;
    UCHAR auto_clear = TRUE;

    // RAW_IO requires reads to be a multiple of the max packet size, which the stream
    // buffer already guarantees, and avoids winusb.sys buffering packets on our behalf.
    WinUsb_SetPipePolicy(port.winusb, port.pipe_in, RAW_IO, sizeof(raw_io), &raw_io);
    WinUsb_SetPipePolicy(port.winusb, port.pipe_in, AUTO_CLEAR_STALL, sizeof(auto_clear),
                         &auto_clear);
    WinUsb_SetPipePolicy(port.winusb, port.pipe_out, AUTO_CLEAR_STALL, sizeof(auto_clear),
                         &auto_clear);

    ULONG timeout = port.timeouts.read_ms;
    WinUsb_SetPipePolicy(port.winusb, port.pipe_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                         &timeout);
    timeout = port.timeouts.write_ms;
    WinUsb_SetPipePolicy(port.winusb, port.pipe_out, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
}

// A disconnect must be reported as kPalmUsbDisconnected: USBTransport.dll abandons the
// session immediately on status 3 but retries forever on any other non-zero status.
bool IsDisconnectError(DWORD error) {
    return error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_GEN_FAILURE ||
           error == ERROR_NO_SUCH_DEVICE || error == ERROR_FILE_NOT_FOUND ||
           error == ERROR_OPERATION_ABORTED || error == ERROR_BAD_COMMAND ||
           error == ERROR_INVALID_HANDLE;
}

}  // namespace

int PipeOpen(PalmPort& port, const char* device_path) {
    if (device_path == nullptr || device_path[0] == '\0') return kPalmUsbInvalidParam;

    port.file = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (port.file == INVALID_HANDLE_VALUE) {
        PALMLOG("CreateFile failed, err %lu", GetLastError());
        return kPalmUsbDisconnected;
    }

    if (!WinUsb_Initialize(port.file, &port.winusb)) {
        PALMLOG("WinUsb_Initialize failed, err %lu", GetLastError());
        CloseHandle(port.file);
        port.file = INVALID_HANDLE_VALUE;
        return kPalmUsbDisconnected;
    }

    if (!DiscoverEndpointsFromDescriptor(port)) {
        PALMLOG("no bulk endpoint pair found");
        PipeClose(port);
        return kPalmUsbDisconnected;
    }

    // Try the informative request first. Only fall back to the older one if it failed:
    // the handheld stays enumerated for only a few seconds after the HotSync button, so
    // avoid spending that window on control transfers we already know will not answer.
    if (!RefineEndpointsFromDevice(port)) {
        SendGetConnectionInfo(port);
    }
    ApplyPipePolicy(port);

    // One max-packet read must always fit, plus room for what a caller left behind.
    port.buffer_capacity = port.max_packet_in * 4;
    port.buffer = new (std::nothrow) std::uint8_t[port.buffer_capacity];
    if (port.buffer == nullptr) {
        PipeClose(port);
        return kPalmUsbInvalidParam;
    }
    port.buffer_head = 0;
    port.buffer_tail = 0;
    port.disconnected = false;

    strncpy_s(port.path, sizeof(port.path), device_path, _TRUNCATE);
    PALMLOG("port %d opened", port.id);
    return kPalmUsbOk;
}

void PipeClose(PalmPort& port) {
    if (port.winusb != nullptr) {
        // Cancels any transfer blocked in another thread, which is the original DLL's
        // only cancellation path.
        WinUsb_AbortPipe(port.winusb, port.pipe_in);
        WinUsb_AbortPipe(port.winusb, port.pipe_out);
        WinUsb_Free(port.winusb);
        port.winusb = nullptr;
    }
    if (port.file != INVALID_HANDLE_VALUE) {
        CloseHandle(port.file);
        port.file = INVALID_HANDLE_VALUE;
    }
    delete[] port.buffer;
    port.buffer = nullptr;
    port.buffer_capacity = 0;
    port.buffer_head = 0;
    port.buffer_tail = 0;
}

int PipeFill(PalmPort& port) {
    if (port.winusb == nullptr) return kPalmUsbDisconnected;

    // Reclaim space by shifting the unread tail down before reading more.
    if (port.buffer_head > 0) {
        const size_t remaining = port.buffer_tail - port.buffer_head;
        if (remaining > 0) memmove(port.buffer, port.buffer + port.buffer_head, remaining);
        port.buffer_head = 0;
        port.buffer_tail = remaining;
    }

    const size_t space = port.buffer_capacity - port.buffer_tail;
    if (space < port.max_packet_in) return kPalmUsbOk;  // caller has not drained us yet

    // RAW_IO requires whole multiples of the max packet size.
    const ULONG request = static_cast<ULONG>((space / port.max_packet_in) *
                                             port.max_packet_in);

    ULONG transferred = 0;
    if (!WinUsb_ReadPipe(port.winusb, port.pipe_in, port.buffer + port.buffer_tail,
                         request, &transferred, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_SEM_TIMEOUT) return kPalmUsbOk;  // idle, not an error
        PALMLOG("ReadPipe failed, err %lu", error);
        if (IsDisconnectError(error)) {
            port.disconnected = true;
            return kPalmUsbDisconnected;
        }
        return kPalmUsbOk;
    }

    port.buffer_tail += transferred;
    return kPalmUsbOk;
}

int PipeWrite(PalmPort& port, const void* data, DWORD length, DWORD* transferred) {
    if (port.winusb == nullptr) return kPalmUsbDisconnected;

    ULONG sent = 0;
    if (!WinUsb_WritePipe(port.winusb, port.pipe_out,
                          static_cast<PUCHAR>(const_cast<void*>(data)), length, &sent,
                          nullptr)) {
        const DWORD error = GetLastError();
        PALMLOG("WritePipe failed, err %lu", error);
        if (IsDisconnectError(error)) {
            port.disconnected = true;
            return kPalmUsbDisconnected;
        }
        return kPalmUsbInvalidParam;
    }

    if (transferred != nullptr) *transferred = sent;
    return kPalmUsbOk;
}

int PipeEnumerate(char (*paths)[kPalmUsbPathMax], int capacity) {
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_PALM_USB, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    int found = 0;
    SP_DEVICE_INTERFACE_DATA interface_data = {};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr,
                                                      &GUID_DEVINTERFACE_PALM_USB, index,
                                                      &interface_data);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &interface_data, nullptr, 0, &required,
                                         nullptr);
        if (required == 0 || required > 4096) continue;

        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(
            calloc(1, required));
        if (detail == nullptr) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (SetupDiGetDeviceInterfaceDetailA(set, &interface_data, detail, required,
                                             nullptr, nullptr)) {
            if (paths != nullptr && found < capacity) {
                strncpy_s(paths[found], kPalmUsbPathMax, detail->DevicePath, _TRUNCATE);
            }
            ++found;
        }
        free(detail);

        if (paths != nullptr && found >= capacity) break;
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}
