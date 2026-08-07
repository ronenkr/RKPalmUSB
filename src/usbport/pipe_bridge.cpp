// Named-pipe bridge between USBTransport.dll and WinUSB.
//
// See the comment block in palmusb.h for why this exists. In short: USBTransport.dll opens
// the names returned by PalmUsbGetFileNames itself and drives them with overlapped
// ReadFile/WriteFile, which a WinUSB device path cannot satisfy but a named pipe can.
//
// Layout, from USBTransport.dll's point of view:
//
//   CreateFileA(name_in,  GENERIC_READ,  FILE_SHARE_READ,  ...)  -> it reads  from us
//   CreateFileA(name_out, GENERIC_WRITE, FILE_SHARE_WRITE, ...)  -> it writes to us
//
// so our server ends are PIPE_ACCESS_OUTBOUND and PIPE_ACCESS_INBOUND respectively. The
// server handles are blocking and each is serviced by its own thread; the client's
// overlapped flag is a property of its handle and does not require ours to match.

#include "palmusb.h"

#include <cstdio>
#include <cstring>

namespace {

struct Bridge {
    HANDLE pipe_in = INVALID_HANDLE_VALUE;
    HANDLE pipe_out = INVALID_HANDLE_VALUE;
    HANDLE thread_in = nullptr;
    HANDLE thread_out = nullptr;
    PalmPort port;
    char device_path[kPalmUsbPathMax] = {};
    volatile LONG running = 0;
    // Set once the handheld has sent anything. This is what the hooked poll IOCTL reports
    // to CUSBTransport*::PollConnection as "a connection is pending".
    volatile LONG saw_device_data = 0;
    CRITICAL_SECTION lock;
    bool lock_ready = false;
};

Bridge g_bridge;

void EnsureLock() {
    // DllMain runs before any export, so this is not racing anything.
    if (!g_bridge.lock_ready) {
        InitializeCriticalSection(&g_bridge.lock);
        g_bridge.lock_ready = true;
    }
}

// device -> host. Pulls from the bulk IN pipe and pushes into the pipe USBTransport reads.
DWORD WINAPI DeviceToHost(LPVOID) {
    PALMLOG("bridge: device->host thread waiting for client");
    if (!ConnectNamedPipe(g_bridge.pipe_in, nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        PALMLOG("bridge: ConnectNamedPipe(in) failed, err %lu", GetLastError());
        return 0;
    }
    PALMLOG("bridge: device->host connected");

    while (InterlockedCompareExchange(&g_bridge.running, 1, 1) == 1) {
        const int status = PipeFill(g_bridge.port);
        if (status == kPalmUsbDisconnected) {
            PALMLOG("bridge: device gone (read)");
            break;
        }

        const size_t available = g_bridge.port.buffer_tail - g_bridge.port.buffer_head;
        if (available == 0) continue;  // idle poll, not an error
        InterlockedExchange(&g_bridge.saw_device_data, 1);

        DWORD written = 0;
        const BOOL ok = WriteFile(g_bridge.pipe_in,
                                  g_bridge.port.buffer + g_bridge.port.buffer_head,
                                  static_cast<DWORD>(available), &written, nullptr);
        if (!ok) {
            PALMLOG("bridge: WriteFile(in-pipe) failed, err %lu", GetLastError());
            break;
        }
        g_bridge.port.buffer_head += written;

        // WriteFile only proves the bytes reached the pipe's buffer, not that anyone read
        // them - which is exactly the question we cannot answer from the logs so far. On a
        // named-pipe SERVER, FlushFileBuffers blocks until the client has consumed
        // everything written, so its return is hard evidence that USBTransport.dll issued
        // a ReadFile. If it never returns, USBTransport is not reading at all and the
        // problem is upstream of the wire protocol.
        const DWORD started = GetTickCount();
        PALMLOG("bridge: dev->host %lu bytes queued, waiting for the host to read", written);
        if (!FlushFileBuffers(g_bridge.pipe_in)) {
            PALMLOG("bridge: dev->host flush failed after %lu ms, err %lu",
                    GetTickCount() - started, GetLastError());
            break;
        }
        PALMLOG("bridge: dev->host %lu bytes CONSUMED by host after %lu ms", written,
                GetTickCount() - started);
    }

    DisconnectNamedPipe(g_bridge.pipe_in);
    PALMLOG("bridge: device->host thread exiting");
    return 0;
}

// host -> device. Pulls from the pipe USBTransport writes and pushes to the bulk OUT pipe.
DWORD WINAPI HostToDevice(LPVOID) {
    PALMLOG("bridge: host->device thread waiting for client");
    if (!ConnectNamedPipe(g_bridge.pipe_out, nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        PALMLOG("bridge: ConnectNamedPipe(out) failed, err %lu", GetLastError());
        return 0;
    }
    PALMLOG("bridge: host->device connected");

    BYTE buffer[4096];
    while (InterlockedCompareExchange(&g_bridge.running, 1, 1) == 1) {
        DWORD read = 0;
        if (!ReadFile(g_bridge.pipe_out, buffer, sizeof(buffer), &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                PALMLOG("bridge: out-pipe closed by client");
            } else {
                PALMLOG("bridge: ReadFile(out-pipe) failed, err %lu", error);
            }
            break;
        }
        if (read == 0) continue;

        DWORD sent = 0;
        const int status = PipeWrite(g_bridge.port, buffer, read, &sent);
        PALMLOG("bridge: host->dev %lu bytes (sent %lu, status %d)", read, sent, status);
        if (status == kPalmUsbDisconnected) {
            PALMLOG("bridge: device gone (write)");
            break;
        }
    }

    DisconnectNamedPipe(g_bridge.pipe_out);
    PALMLOG("bridge: host->device thread exiting");
    return 0;
}

// Whether the bridge is actually serving a live session. Call with the lock held.
//
// `running` alone is not enough: when the handheld leaves the bus the pump threads exit on
// their own and nothing clears the flag. Nor is a matching device path - the handheld
// re-enumerates under the SAME path string on every HotSync press, so path equality says
// nothing about whether the previous session survived. Only the threads do.
bool BridgeAliveLocked() {
    if (InterlockedCompareExchange(&g_bridge.running, 1, 1) != 1) return false;
    if (g_bridge.port.disconnected) return false;

    const HANDLE threads[2] = {g_bridge.thread_in, g_bridge.thread_out};
    for (HANDLE thread : threads) {
        if (thread == nullptr) return false;
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) return false;  // exited
    }
    return true;
}

}  // namespace

void BridgeGetNames(char* name_in, size_t name_in_size, char* name_out,
                    size_t name_out_size) {
    // Per-process names so two Palm Desktop components cannot collide.
    const DWORD pid = GetCurrentProcessId();
    if (name_in != nullptr) {
        _snprintf_s(name_in, name_in_size, _TRUNCATE, "\\\\.\\pipe\\RKPalmUSB_%lu_IN", pid);
    }
    if (name_out != nullptr) {
        _snprintf_s(name_out, name_out_size, _TRUNCATE, "\\\\.\\pipe\\RKPalmUSB_%lu_OUT",
                    pid);
    }
}

bool BridgeConnectionPending() {
    if (!g_bridge.lock_ready) return false;
    EnterCriticalSection(&g_bridge.lock);
    // Aliveness matters as much as the data flag here: after a finished session both
    // `running` and `saw_device_data` are still set, and reporting "connection pending"
    // off a dead bridge would send HotSync into a session with nothing behind it.
    const bool pending =
        BridgeAliveLocked() &&
        InterlockedCompareExchange(&g_bridge.saw_device_data, 1, 1) == 1;
    LeaveCriticalSection(&g_bridge.lock);
    return pending;
}

int BridgeStart(const char* device_path) {
    if (device_path == nullptr || device_path[0] == '\0') return kPalmUsbInvalidParam;
    EnsureLock();

    EnterCriticalSection(&g_bridge.lock);

    if (g_bridge.running == 1) {
        if (_stricmp(g_bridge.device_path, device_path) == 0 && BridgeAliveLocked()) {
            LeaveCriticalSection(&g_bridge.lock);
            PALMLOG("bridge: already running for this device");
            return kPalmUsbOk;
        }
        // Either a different device, or the previous session has finished and left the
        // flag set. Tear it down: its pipes have no thread servicing them and its WinUSB
        // handle refers to a device that has left the bus.
        LeaveCriticalSection(&g_bridge.lock);
        PALMLOG("bridge: previous session is finished, restarting");
        BridgeStop();
        EnterCriticalSection(&g_bridge.lock);
    }

    char name_in[kPalmUsbPathMax];
    char name_out[kPalmUsbPathMax];
    BridgeGetNames(name_in, sizeof(name_in), name_out, sizeof(name_out));

    // The bridge's port never enters the port table (no PalmUsbOpenPort call reaches it),
    // so give it an id purely so the logs do not read "port -1".
    g_bridge.port.id = 0;
    const int status = PipeOpen(g_bridge.port, device_path);
    if (status != kPalmUsbOk) {
        PALMLOG("bridge: PipeOpen failed, status %d", status);
        LeaveCriticalSection(&g_bridge.lock);
        return status;
    }

    // USBTransport opens name_in for reading, so our end is outbound, and vice versa.
    g_bridge.pipe_in = CreateNamedPipeA(
        name_in, PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 8192, 8192, 0, nullptr);
    g_bridge.pipe_out = CreateNamedPipeA(
        name_out, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 8192, 8192, 0, nullptr);

    if (g_bridge.pipe_in == INVALID_HANDLE_VALUE ||
        g_bridge.pipe_out == INVALID_HANDLE_VALUE) {
        PALMLOG("bridge: CreateNamedPipe failed, err %lu", GetLastError());
        if (g_bridge.pipe_in != INVALID_HANDLE_VALUE) CloseHandle(g_bridge.pipe_in);
        if (g_bridge.pipe_out != INVALID_HANDLE_VALUE) CloseHandle(g_bridge.pipe_out);
        g_bridge.pipe_in = INVALID_HANDLE_VALUE;
        g_bridge.pipe_out = INVALID_HANDLE_VALUE;
        PipeClose(g_bridge.port);
        LeaveCriticalSection(&g_bridge.lock);
        return kPalmUsbInvalidParam;
    }

    strncpy_s(g_bridge.device_path, sizeof(g_bridge.device_path), device_path, _TRUNCATE);
    InterlockedExchange(&g_bridge.saw_device_data, 0);
    InterlockedExchange(&g_bridge.running, 1);

    g_bridge.thread_in = CreateThread(nullptr, 0, DeviceToHost, nullptr, 0, nullptr);
    g_bridge.thread_out = CreateThread(nullptr, 0, HostToDevice, nullptr, 0, nullptr);

    PALMLOG("bridge: started, in=%s out=%s (IN pipe 0x%02X, OUT pipe 0x%02X)", name_in,
            name_out, g_bridge.port.pipe_in, g_bridge.port.pipe_out);

    LeaveCriticalSection(&g_bridge.lock);
    return kPalmUsbOk;
}

void BridgeStop() {
    if (!g_bridge.lock_ready) return;
    EnterCriticalSection(&g_bridge.lock);

    if (g_bridge.running == 0) {
        LeaveCriticalSection(&g_bridge.lock);
        return;
    }
    InterlockedExchange(&g_bridge.running, 0);
    InterlockedExchange(&g_bridge.saw_device_data, 0);
    PALMLOG("bridge: stopping");

    // Closing the WinUSB handles aborts the blocked bulk read so the pump threads wake up.
    PipeClose(g_bridge.port);

    if (g_bridge.pipe_in != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_bridge.pipe_in);
        CloseHandle(g_bridge.pipe_in);
        g_bridge.pipe_in = INVALID_HANDLE_VALUE;
    }
    if (g_bridge.pipe_out != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_bridge.pipe_out);
        CloseHandle(g_bridge.pipe_out);
        g_bridge.pipe_out = INVALID_HANDLE_VALUE;
    }

    HANDLE threads[2] = {g_bridge.thread_in, g_bridge.thread_out};
    g_bridge.thread_in = nullptr;
    g_bridge.thread_out = nullptr;
    g_bridge.device_path[0] = '\0';
    LeaveCriticalSection(&g_bridge.lock);

    // Wait outside the lock: the threads log, and the logger takes its own lock.
    for (HANDLE thread : threads) {
        if (thread == nullptr) continue;
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    PALMLOG("bridge: stopped");
}
