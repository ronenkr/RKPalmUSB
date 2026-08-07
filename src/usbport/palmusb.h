// Shared declarations for the WinUSB-backed USBPort.dll replacement.
// The ABI reproduced here is documented in docs/usbport-abi.md.

#pragma once

#include <windows.h>
#include <dbt.h>
#include <winusb.h>

#include <cstdint>

// {784126BF-4190-11D4-B5C2-00C04F687A67} - taken from the original USBPort.dll (RVA 0x813C)
// and reused by driver/PalmWinUSB.inf so that AutoDetect.dll, DeviceMonitor.exe and
// PalmUSBDirect.dll, which hardcode it, keep working.
extern "C" const GUID GUID_DEVINTERFACE_PALM_USB;

// Status values returned by the exports. Only these three are load-bearing for HotSync;
// see the status table in docs/usbport-abi.md.
enum PalmUsbStatus : int {
    kPalmUsbOk = 0,
    kPalmUsbDisconnected = 3,
    kPalmUsbInvalidParam = 4,
};

constexpr int kPalmUsbInvalidPort = -1;

// Callers pass this FourCC ('sync') to select the HotSync port.
constexpr DWORD kPalmUsbSyncTag = 0x73796E63;

// PalmUsbIsPalmOSDeviceNotification's callers supply a 256-byte path buffer.
constexpr size_t kPalmUsbPathMax = 256;

// The 8-byte blob exchanged by PalmUsbGet/SetTimeouts.
struct PalmUsbTimeouts {
    DWORD read_ms;
    DWORD write_ms;
};

// One open Palm device: the WinUSB handle plus the stream buffer that lets
// PalmUsbReceiveBytes hand out arbitrary byte counts from packet-sized bulk reads.
struct PalmPort {
    int id = kPalmUsbInvalidPort;
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    UCHAR pipe_in = 0;
    UCHAR pipe_out = 0;
    ULONG max_packet_in = 64;
    PalmUsbTimeouts timeouts{ 1000, 5000 };

    // Leftovers from the last bulk read that the caller has not consumed yet.
    std::uint8_t* buffer = nullptr;
    size_t buffer_capacity = 0;
    size_t buffer_head = 0;
    size_t buffer_tail = 0;

    char path[kPalmUsbPathMax] = {};
    bool disconnected = false;
};

// --- winusb_pipe.cpp ---------------------------------------------------------

// Opens the device at `device_path` and fills in the WinUSB handle and endpoints.
// Returns a PalmUsbStatus.
int PipeOpen(PalmPort& port, const char* device_path);
void PipeClose(PalmPort& port);

// Refills port.buffer with one bulk transfer. Returns kPalmUsbOk even when nothing
// arrived before the read timeout - an empty read is a normal poll result.
int PipeFill(PalmPort& port);
int PipeWrite(PalmPort& port, const void* data, DWORD length, DWORD* transferred);

// Enumerates present Palm interfaces. Returns the count; writes up to `capacity`
// NUL-terminated paths into `paths` laid out as [capacity][kPalmUsbPathMax].
int PipeEnumerate(char (*paths)[kPalmUsbPathMax], int capacity);

// --- port_table.cpp ----------------------------------------------------------

// The port list and its lock. Mirrors the original's single DLL-global critical section.
PalmPort* PortFind(int id);
PalmPort* PortCreate();
void PortDestroy(int id);
void PortLock();
void PortUnlock();

// --- log.cpp -----------------------------------------------------------------

void LogInit();
void LogShutdown();
void LogPrintf(const char* format, ...);

#define PALMLOG(...) LogPrintf(__VA_ARGS__)
