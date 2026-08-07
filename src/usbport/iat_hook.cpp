// Answers USBTransport.dll's two private PalmUSBD.sys IOCTLs by swapping its imports.
//
// See the comment block in palmusb.h for why this is necessary, and
// docs/usbtransport-ioctls.md for the disassembly the codes were recovered from.
//
// Scope of the patch: two IAT slots inside USBTransport.dll, in the HotSync process, in
// user mode. Nothing is written to disk, no Palm binary is modified, and removing our DLL
// removes the hook. Both hooks forward every call that is not about our device to the
// original function, so unrelated file and device I/O in that process is untouched.

#include "palmusb.h"

#include <cstring>

namespace {

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                      DWORD, HANDLE);
using DeviceIoControlFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD,
                                        LPDWORD, LPOVERLAPPED);

// The two codes, recovered from DynTransCreate (666B1730) and the 666B1830 poll helper.
constexpr DWORD kIoctlIdentify = 0x00222004;
constexpr DWORD kIoctlPoll = 0x0022240C;

// DynTransCreate builds CUSBTransportPAD only for exactly this pair.
constexpr DWORD kTransportPadSignature[2] = {0x0000082D, 0x00000100};

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
bool g_installed = false;

CreateFileAFn g_real_create_file = nullptr;
DeviceIoControlFn g_real_ioctl = nullptr;

char g_device_path[kPalmUsbPathMax] = {};

// The handle our CreateFileA hook last returned on this thread, held in TLS.
//
// USBTransport's helper is strictly synchronous - CreateFileA, one DeviceIoControl, then
// CloseHandle, all on one thread with nothing interleaved - so one slot per thread
// identifies the shim exactly, and the slot is cleared the moment the IOCTL is answered.
//
// This replaced a fixed-size table of live shim handles, which was wrong twice over.
// Nothing ever removed entries, because the CLOSE is done by USBTransport and we never see
// it, so every PollConnection consumed a slot until the table filled and CreateFileA began
// refusing - a permanent failure after a few syncs. Worse, Windows recycles handle values,
// so a stale entry could match an unrelated handle and we would hijack its
// DeviceIoControl. A slot that lives only for the duration of one call has neither problem.
DWORD g_shim_tls = TLS_OUT_OF_INDEXES;

void EnsureLock() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
}

// Ownership of a shim handle passes to the caller, which always closes it. We must never
// close one ourselves: by the time we could, the value may name someone else's object.
bool IsShim(HANDLE handle) {
    if (g_shim_tls == TLS_OUT_OF_INDEXES || handle == nullptr ||
        handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return TlsGetValue(g_shim_tls) == handle;
}

// Matches the path USBTransport was given. Falls back to a vendor-id test so a stale or
// missing note still recognises a Palm interface path rather than silently declining.
bool IsOurDevicePath(const char* path) {
    if (path == nullptr || path[0] == '\0') return false;
    if (g_device_path[0] != '\0' && _stricmp(path, g_device_path) == 0) return true;

    char lowered[kPalmUsbPathMax] = {};
    strncpy_s(lowered, sizeof(lowered), path, _TRUNCATE);
    _strlwr_s(lowered, sizeof(lowered));
    return strstr(lowered, "vid_0830") != nullptr;
}

// Selects which transport class DynTransCreate builds. HTAL is the default: its data path
// is the named-pipe bridge, which is already known to carry device bytes. PAD routes I/O
// through PalmUsbOpenPort/SendBytes/ReceiveBytes instead - also implemented here, but its
// framing is unverified against this handheld.
bool WantPadTransport() {
    char value[16] = {};
    if (GetEnvironmentVariableA("PALMUSB_TRANSPORT", value, sizeof(value)) == 0) {
        return false;
    }
    return _stricmp(value, "pad") == 0;
}

HANDLE WINAPI HookedCreateFileA(LPCSTR file_name, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD creation,
                                DWORD flags, HANDLE template_file) {
    if (!IsOurDevicePath(file_name)) {
        return g_real_create_file(file_name, access, share, security, creation, flags,
                                  template_file);
    }

    if (g_shim_tls == TLS_OUT_OF_INDEXES) {
        return g_real_create_file(file_name, access, share, security, creation, flags,
                                  template_file);
    }

    // Do NOT open the real WinUSB device here. This handle exists only to carry the two
    // IOCTLs below, and opening the device a second time would contend with the handle the
    // bridge is already using. Any waitable object serves; an event keeps the file system
    // out of it entirely, and CloseHandle - which the caller will call - accepts one.
    HANDLE shim = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (shim == nullptr) return INVALID_HANDLE_VALUE;

    // A leftover value here means a previous call never reached its DeviceIoControl. The
    // caller closed that handle regardless, so overwrite the slot and close nothing.
    TlsSetValue(g_shim_tls, shim);

    SetLastError(ERROR_SUCCESS);
    return shim;
}

BOOL WINAPI HookedDeviceIoControl(HANDLE handle, DWORD code, LPVOID in_buffer,
                                  DWORD in_size, LPVOID out_buffer, DWORD out_size,
                                  LPDWORD returned, LPOVERLAPPED overlapped) {
    if (!IsShim(handle)) {
        return g_real_ioctl(handle, code, in_buffer, in_size, out_buffer, out_size,
                            returned, overlapped);
    }

    // One IOCTL per open, so retire the slot now. Leaving it set would let a later,
    // unrelated handle that reuses this value be mistaken for ours.
    TlsSetValue(g_shim_tls, nullptr);

    // The caller reads GetLastError(), not the BOOL, and treats non-zero as failure - so
    // clearing the error is what actually signals success here.
    if (code == kIoctlIdentify) {
        const bool pad = WantPadTransport();
        if (out_buffer != nullptr && out_size >= sizeof(kTransportPadSignature)) {
            if (pad) {
                memcpy(out_buffer, kTransportPadSignature,
                       sizeof(kTransportPadSignature));
            } else {
                memset(out_buffer, 0, sizeof(kTransportPadSignature));
            }
        }
        if (returned != nullptr) *returned = sizeof(kTransportPadSignature);
        PALMLOG("hook: IOCTL identify -> %s", pad ? "CUSBTransportPAD" : "CUSBTransportHTAL");
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    if (code == kIoctlPoll) {
        const USHORT pending = BridgeConnectionPending() ? 1 : 0;
        if (out_buffer != nullptr && out_size >= sizeof(pending)) {
            memcpy(out_buffer, &pending, sizeof(pending));
        }
        if (returned != nullptr) *returned = sizeof(pending);
        if (pending != 0) PALMLOG("hook: IOCTL poll -> connection pending");
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    PALMLOG("hook: unhandled IOCTL 0x%08lX on our device", code);
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

// Swaps one import slot. Matches on the imported function's name where the name table is
// present, and otherwise on the slot's current value - bound imports keep no names.
bool PatchImport(HMODULE module, const char* function, void* replacement,
                 void** original) {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return false;

    void* known = GetProcAddress(GetModuleHandleA("kernel32.dll"), function);

    auto* import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; import->Name != 0; ++import) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
        IMAGE_THUNK_DATA* names =
            (import->OriginalFirstThunk != 0)
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk)
                : nullptr;

        for (; thunk->u1.Function != 0; ++thunk, names = (names != nullptr) ? names + 1
                                                                            : nullptr) {
            bool match = false;
            if (names != nullptr && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                auto* by_name =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                match = (strcmp(reinterpret_cast<const char*>(by_name->Name), function) == 0);
            }
            if (!match && known != nullptr) {
                match = (reinterpret_cast<void*>(thunk->u1.Function) == known);
            }
            if (!match) continue;

            DWORD old_protect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                                PAGE_READWRITE, &old_protect)) {
                return false;
            }
            *original = reinterpret_cast<void*>(thunk->u1.Function);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old_protect,
                           &old_protect);
            return true;
        }
    }
    return false;
}

}  // namespace

void HookNoteDevicePath(const char* device_path) {
    if (device_path == nullptr || device_path[0] == '\0') return;
    EnsureLock();
    EnterCriticalSection(&g_lock);
    strncpy_s(g_device_path, sizeof(g_device_path), device_path, _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}

void HookInstall() {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_installed) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    // Only USBTransport.dll issues these IOCTLs, so only its table is touched. Absent
    // module means we were loaded by AutoDetect.dll or DeviceMonitor.exe instead, where
    // there is nothing to patch.
    HMODULE transport = GetModuleHandleA("USBTransport.dll");
    if (transport == nullptr) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    if (g_shim_tls == TLS_OUT_OF_INDEXES) {
        g_shim_tls = TlsAlloc();
        if (g_shim_tls == TLS_OUT_OF_INDEXES) {
            LeaveCriticalSection(&g_lock);
            PALMLOG("hook: TlsAlloc failed - NOT installed");
            return;
        }
    }

    char opt[8] = {};
    if (GetEnvironmentVariableA("PALMUSB_NO_HOOK", opt, sizeof(opt)) > 0 && opt[0] == '1') {
        g_installed = true;  // do not retry
        LeaveCriticalSection(&g_lock);
        PALMLOG("hook: disabled by PALMUSB_NO_HOOK=1");
        return;
    }

    const bool ioctl_ok = PatchImport(transport, "DeviceIoControl",
                                      reinterpret_cast<void*>(&HookedDeviceIoControl),
                                      reinterpret_cast<void**>(&g_real_ioctl));
    const bool create_ok = PatchImport(transport, "CreateFileA",
                                       reinterpret_cast<void*>(&HookedCreateFileA),
                                       reinterpret_cast<void**>(&g_real_create_file));

    // A half-installed hook is worse than none: CreateFileA would hand out shim handles
    // that the unhooked DeviceIoControl cannot answer. Back the one that took out.
    if (ioctl_ok != create_ok) {
        PALMLOG("hook: partial install (ioctl=%d create=%d), reverting", ioctl_ok,
                create_ok);
        if (ioctl_ok) {
            PatchImport(transport, "DeviceIoControl",
                        reinterpret_cast<void*>(g_real_ioctl), reinterpret_cast<void**>(
                            &g_real_ioctl));
        }
        if (create_ok) {
            PatchImport(transport, "CreateFileA",
                        reinterpret_cast<void*>(g_real_create_file),
                        reinterpret_cast<void**>(&g_real_create_file));
        }
        LeaveCriticalSection(&g_lock);
        return;
    }

    g_installed = ioctl_ok && create_ok;
    LeaveCriticalSection(&g_lock);
    PALMLOG("hook: %s", g_installed ? "installed into USBTransport.dll"
                                    : "import slots not found - NOT installed");
}

void HookUninstall() {
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);

    // Put the original imports back before we can be unloaded. USBTransport.dll holds a
    // static import on this DLL so it should always unload first, but if that order ever
    // reversed its IAT would point into freed code and the next CreateFileA would crash
    // the HotSync process.
    if (g_installed) {
        HMODULE transport = GetModuleHandleA("USBTransport.dll");
        if (transport != nullptr) {
            void* discard = nullptr;
            if (g_real_ioctl != nullptr) {
                PatchImport(transport, "DeviceIoControl",
                            reinterpret_cast<void*>(g_real_ioctl), &discard);
            }
            if (g_real_create_file != nullptr) {
                PatchImport(transport, "CreateFileA",
                            reinterpret_cast<void*>(g_real_create_file), &discard);
            }
            PALMLOG("hook: original imports restored");
        }
        g_installed = false;
    }

    // Only the TLS index is ours to release. Shim handles belong to USBTransport, which
    // closes each one immediately after its IOCTL - closing them here would be a double
    // close, and by then the value may well name an unrelated object.
    if (g_shim_tls != TLS_OUT_OF_INDEXES) {
        TlsFree(g_shim_tls);
        g_shim_tls = TLS_OUT_OF_INDEXES;
    }

    LeaveCriticalSection(&g_lock);
}
