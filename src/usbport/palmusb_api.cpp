// The 12 exports. Signatures, calling convention and return values are those recovered
// in docs/usbport-abi.md - do not change one without updating that document.
//
// Every export is __cdecl and every one is reachable from a different Palm Desktop
// module, so none may be omitted.

#include "palmusb.h"

#include <cstring>
#include <new>

namespace {

// Set by PalmUsbRegisterDeviceInterface, released by PalmUsbUnRegisterDeviceInterface.
HDEVNOTIFY g_notify = nullptr;

// Copies a NUL-terminated string out under the two-pass sizing protocol that
// PalmUsbGetFileNames uses: a NULL buffer means "just tell me the length".
bool EmitSizedString(const char* value, char* buffer, DWORD* length) {
    if (length == nullptr) return false;
    const DWORD needed = static_cast<DWORD>(strlen(value)) + 1;

    if (buffer == nullptr) {
        *length = needed;
        return true;
    }
    if (*length < needed) {
        *length = needed;
        return false;
    }
    memcpy(buffer, value, needed);
    *length = needed;
    return true;
}

}  // namespace

extern "C" {

// Returns a port id, or -1. The device path comes from PalmUsbIsPalmOSDeviceNotification
// or PalmUsbGetAttachedDevices; its format is private to this DLL.
int __cdecl PalmUsbOpenPort(const char* device_path, DWORD tag) {
    PALMLOG("-> OpenPort(path=%s, tag=0x%08lX)",
            device_path ? device_path : "(null)", tag);
    if (device_path == nullptr || device_path[0] == '\0') return kPalmUsbInvalidPort;
    if (tag != kPalmUsbSyncTag) {
        PALMLOG("OpenPort: unexpected tag 0x%08lX", tag);
    }

    PortLock();
    PalmPort* port = PortCreate();
    if (port == nullptr) {
        PortUnlock();
        return kPalmUsbInvalidPort;
    }

    const int id = port->id;
    const int status = PipeOpen(*port, device_path);
    if (status != kPalmUsbOk) {
        PortDestroy(id);
        PortUnlock();
        PALMLOG("OpenPort failed, status %d", status);
        return kPalmUsbInvalidPort;
    }

    PortUnlock();
    return id;
}

int __cdecl PalmUsbClosePort(int port_id) {
    PortLock();
    PortDestroy(port_id);
    PortUnlock();
    PALMLOG("ClosePort %d", port_id);
    return kPalmUsbOk;
}

// Reads up to `length` bytes. Returning kPalmUsbOk with *transferred == 0 is the normal
// "nothing yet" answer - USBTransport.dll polls on that and only treats
// kPalmUsbDisconnected as fatal.
int __cdecl PalmUsbReceiveBytes(int port_id, void* buffer, DWORD length,
                                DWORD* transferred) {
    if (transferred != nullptr) *transferred = 0;
    if (buffer == nullptr || transferred == nullptr) return kPalmUsbInvalidParam;

    PortLock();
    PalmPort* port = PortFind(port_id);
    if (port == nullptr) {
        PortUnlock();
        return kPalmUsbInvalidParam;
    }
    if (port->disconnected) {
        PortUnlock();
        return kPalmUsbDisconnected;
    }

    int status = kPalmUsbOk;
    if (port->buffer_head == port->buffer_tail) {
        status = PipeFill(*port);
        if (status != kPalmUsbOk) {
            PortUnlock();
            return status;
        }
    }

    const size_t available = port->buffer_tail - port->buffer_head;
    const size_t take = available < length ? available : length;
    if (take > 0) {
        memcpy(buffer, port->buffer + port->buffer_head, take);
        port->buffer_head += take;
        *transferred = static_cast<DWORD>(take);
    }

    PortUnlock();
    return kPalmUsbOk;
}

int __cdecl PalmUsbSendBytes(int port_id, const void* buffer, DWORD length,
                             DWORD* transferred) {
    if (transferred != nullptr) *transferred = 0;
    if (buffer == nullptr) return kPalmUsbInvalidParam;

    PortLock();
    PalmPort* port = PortFind(port_id);
    if (port == nullptr) {
        PortUnlock();
        return kPalmUsbInvalidParam;
    }
    if (port->disconnected) {
        PortUnlock();
        return kPalmUsbDisconnected;
    }

    const int status = PipeWrite(*port, buffer, length, transferred);
    PortUnlock();
    return status;
}

int __cdecl PalmUsbSetTimeouts(int port_id, const PalmUsbTimeouts* timeouts) {
    if (timeouts == nullptr) return kPalmUsbInvalidParam;

    PortLock();
    PalmPort* port = PortFind(port_id);
    if (port == nullptr) {
        PortUnlock();
        return kPalmUsbInvalidParam;
    }

    port->timeouts = *timeouts;
    if (port->winusb != nullptr) {
        ULONG value = port->timeouts.read_ms;
        WinUsb_SetPipePolicy(port->winusb, port->pipe_in, PIPE_TRANSFER_TIMEOUT,
                             sizeof(value), &value);
        value = port->timeouts.write_ms;
        WinUsb_SetPipePolicy(port->winusb, port->pipe_out, PIPE_TRANSFER_TIMEOUT,
                             sizeof(value), &value);
    }
    PortUnlock();
    return kPalmUsbOk;
}

int __cdecl PalmUsbGetTimeouts(int port_id, PalmUsbTimeouts* timeouts) {
    if (timeouts == nullptr) return kPalmUsbInvalidParam;

    PortLock();
    PalmPort* port = PortFind(port_id);
    if (port == nullptr) {
        PortUnlock();
        return kPalmUsbInvalidParam;
    }
    *timeouts = port->timeouts;
    PortUnlock();
    return kPalmUsbOk;
}

// Called from a WM_DEVICECHANGE handler. Fills `out_path` (callers supply 256 bytes) with
// the arriving device's interface path and returns TRUE when it is one of ours.
int __cdecl PalmUsbIsPalmOSDeviceNotification(DEV_BROADCAST_HDR* header, DWORD tag,
                                              char* out_path, GUID* out_class_guid) {
    if (header == nullptr) return FALSE;
    if (header->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) return FALSE;

    auto* iface = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_A*>(header);

    // The original fills the caller's GUID even when the interface does not match.
    if (out_class_guid != nullptr) *out_class_guid = iface->dbcc_classguid;

    if (!IsEqualGUID(iface->dbcc_classguid, GUID_DEVINTERFACE_PALM_USB)) return FALSE;
    if (tag != kPalmUsbSyncTag) {
        PALMLOG("notification: our device, but unexpected tag 0x%08lX", tag);
        return FALSE;
    }

    if (out_path != nullptr) {
        strncpy_s(out_path, kPalmUsbPathMax, iface->dbcc_name, _TRUNCATE);
    }
    PALMLOG("device notification matched (out_path=%s) -> TRUE",
            (out_path != nullptr) ? "filled" : "not requested");
    return TRUE;
}

int __cdecl PalmUsbRegisterDeviceInterface(HWND window) {
    if (window == nullptr) return FALSE;
    if (g_notify != nullptr) return TRUE;

    DEV_BROADCAST_DEVICEINTERFACE_A filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_PALM_USB;

    g_notify = RegisterDeviceNotificationA(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    PALMLOG("RegisterDeviceInterface -> %p", g_notify);
    return g_notify != nullptr;
}

int __cdecl PalmUsbUnRegisterDeviceInterface(HDEVNOTIFY handle) {
    HDEVNOTIFY target = (handle != nullptr) ? handle : g_notify;
    if (target == nullptr) return kPalmUsbOk;

    UnregisterDeviceNotification(target);
    if (target == g_notify) g_notify = nullptr;
    return kPalmUsbOk;
}

// Two-pass sizing query. The original returned the "\IN" and "\OUT" sub-device names that
// its kernel driver exposed; WinUSB has a single interface, so both names are the device
// path itself. Only round-trip consistency with PalmUsbOpenPort matters - USBTransport.dll
// passes the results straight back and never parses them.
int __cdecl PalmUsbGetFileNames(const char* device_path, DWORD tag, char* in_name,
                                DWORD* in_length, char* out_name, DWORD* out_length) {
    PALMLOG("-> GetFileNames(path=%s, tag=0x%08lX, pass=%s)",
            device_path ? device_path : "(null)", tag,
            (in_name == nullptr) ? "size-query" : "fetch");
    if (device_path == nullptr || in_length == nullptr || out_length == nullptr) {
        return kPalmUsbInvalidParam;
    }
    if (tag != kPalmUsbSyncTag) return kPalmUsbInvalidParam;

    // USBTransport.dll does NOT hand these names back to us. It opens them itself with
    // CreateFileA (read-only and write-only respectively) and then drives them with raw
    // ReadFile/WriteFile - bypassing PalmUsbOpenPort/SendBytes/ReceiveBytes entirely.
    // The original kernel driver exposed two real device objects, "<path>\IN" and
    // "<path>\OUT", for exactly this. A WinUSB interface path cannot stand in for them:
    // it does not support raw ReadFile/WriteFile.
    //
    // Failing here makes that code path abandon the device, which is how we test whether
    // HotSync falls back to the transport class that *does* route I/O through this DLL
    // (PalmUsbOpenPort is called from CUSBTransportPAD::Reset).
    //
    // Set PALMUSB_NO_FILENAMES=1 to take that fallback route.
    char opt[8] = {};
    if (GetEnvironmentVariableA("PALMUSB_NO_FILENAMES", opt, sizeof(opt)) > 0 &&
        opt[0] == '1') {
        PALMLOG("GetFileNames: refusing (PALMUSB_NO_FILENAMES=1) to force the "
                "OpenPort-based transport");
        return kPalmUsbInvalidParam;
    }

    const bool in_ok = EmitSizedString(device_path, in_name, in_length);
    const bool out_ok = EmitSizedString(device_path, out_name, out_length);
    const int status = (in_ok && out_ok) ? kPalmUsbOk : kPalmUsbInvalidParam;
    PALMLOG("GetFileNames -> status %d, in_len=%lu out_len=%lu", status,
            in_length ? *in_length : 0, out_length ? *out_length : 0);
    return status;
}

// Used by AutoDetect.dll during the setup wizard, not on the sync path. Reports how many
// Palm interfaces are present and, when given a buffer, their paths.
int __cdecl PalmUsbGetAttachedDevices(char* paths, DWORD* count, DWORD* path_stride) {
    const int present = PipeEnumerate(nullptr, 0);
    PALMLOG("-> GetAttachedDevices(pass=%s) -> %d present",
            (paths == nullptr) ? "count-query" : "fetch", present);

    if (path_stride != nullptr) *path_stride = kPalmUsbPathMax;
    if (count == nullptr) return kPalmUsbInvalidParam;

    if (paths == nullptr) {
        *count = static_cast<DWORD>(present);
        return kPalmUsbOk;
    }

    const int capacity = static_cast<int>(*count);
    const int written = PipeEnumerate(reinterpret_cast<char(*)[kPalmUsbPathMax]>(paths),
                                      capacity);
    *count = static_cast<DWORD>(written < capacity ? written : capacity);
    return kPalmUsbOk;
}

// Also AutoDetect-only. The device's product string is the closest equivalent to what the
// original read out of the driver's registry key.
int __cdecl PalmUsbGetDeviceFriendlyName(const char* device_path, char* name) {
    if (device_path == nullptr || name == nullptr) return kPalmUsbInvalidParam;

    PalmPort probe;
    if (PipeOpen(probe, device_path) != kPalmUsbOk) return kPalmUsbDisconnected;

    USB_DEVICE_DESCRIPTOR descriptor = {};
    ULONG transferred = 0;
    int status = kPalmUsbInvalidParam;

    if (WinUsb_GetDescriptor(probe.winusb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                             reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor),
                             &transferred) &&
        descriptor.iProduct != 0) {
        // String descriptors are UTF-16 with a 2-byte header.
        UCHAR raw[256] = {};
        if (WinUsb_GetDescriptor(probe.winusb, USB_STRING_DESCRIPTOR_TYPE,
                                 descriptor.iProduct, 0x0409, raw, sizeof(raw),
                                 &transferred) &&
            transferred > 2) {
            const int chars = static_cast<int>((transferred - 2) / sizeof(wchar_t));
            const int written = WideCharToMultiByte(
                CP_ACP, 0, reinterpret_cast<wchar_t*>(raw + 2), chars, name,
                static_cast<int>(kPalmUsbPathMax) - 1, nullptr, nullptr);
            name[written > 0 ? written : 0] = '\0';
            status = kPalmUsbOk;
        }
    }

    PipeClose(probe);
    return status;
}

}  // extern "C"
