# Why HotSync never started a session

Recovered by disassembling `C:\Program Files (x86)\Palm\USBTransport.dll` (x86, 42,496
bytes, 2008-01-04). Image base `0x666B0000`; addresses below are as loaded. Regenerate the
listing with:

```powershell
dumpbin /disasm /out:evidence\disasm\USBTransport_x86.disasm.txt "C:\Program Files (x86)\Palm\USBTransport.dll"
```

## Summary

`USBTransport.dll` gates its entire session on **two private `PalmUSBD.sys` IOCTLs issued
against the device path**. `winusb.sys` answers neither, and neither does a named pipe.
`PollConnection` therefore returns `0x2005` ("no connection") forever, and HotSync never
progresses to reading or writing — which is why the named-pipe bridge could deliver the
handheld's greeting and still see no reply.

The wire protocol, the framing and the pipes were never the problem.

## How the device path gets there

`666BB2A8` is a 256-byte global. It is filled by **our own export**, at `666B1711`:

```
666B1711: mov  ecx,[esp+8]
666B1715: push 0                  ; out_class_guid
666B1717: push 666BB2A8h          ; out_path  <- the global
666B171C: push 73796E63h          ; tag 'sync'
666B1721: push ecx                ; DEV_BROADCAST_HDR*
666B1722: call ds:[666B813Ch]     ; PalmUsbIsPalmOSDeviceNotification
```

It is then used three ways:

| site | use |
|---|---|
| `666B19EB`, `666B1A23` | `PalmUsbGetFileNames(path,'sync',…)`, then `CreateFileA` on each returned name → handles at `this+15Ch` / `this+160h` |
| `666B2782` | `PalmUsbOpenPort(path,'sync')` → port id at `this+18Ch` |
| `666B1761`, `666B2398`, `666B3689` | `CreateFileA(path)` + `DeviceIoControl(…)` — **the blocker** |

## The IOCTL helper — `666B17B0`

```c
DWORD ioctl_on_path(const char *path, DWORD code,
                    void *in, DWORD in_len, void *out, DWORD out_len)
{
    if (strlen(path) == 0) return (DWORD)-1;
    HANDLE h = CreateFileA(path, GENERIC_READ|GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return (DWORD)-1;
    DeviceIoControl(h, code, in, in_len, out, out_len, &returned, NULL);
    DWORD err = GetLastError();      /* <-- the BOOL is ignored */
    CloseHandle(h);
    return err;                      /* 0 == success */
}
```

**Success is signalled by `GetLastError() == 0`, not by `DeviceIoControl`'s return value.**
Anything hooking this must call `SetLastError(ERROR_SUCCESS)`.

## Code 1 — `0x222004`, transport selection (`DynTransCreate`, `666B1730`)

```c
void *DynTransCreate(void)
{
    if (strlen(g_devicePath) == 0) return NULL;
    DWORD info[2] = {0, 0};
    ioctl_on_path(g_devicePath, 0x222004, info, 8, info, 8);
    if (info[0] == 0x82D && info[1] == 0x100)
        return new CUSBTransportPAD();      /* 666B2FE0, object size 0x190 */
    return new CUSBTransportHTAL();         /* 666B1560, object size 0x1B8 */
}
```

The result **chooses the transport class**, and the two classes take different data paths:

| class | data path |
|---|---|
| `CUSBTransportHTAL` | the two `CreateFileA` handles from `PalmUsbGetFileNames` — raw overlapped `ReadFile`/`WriteFile` |
| `CUSBTransportPAD` | `PalmUsbOpenPort` / `PalmUsbSendBytes` / `PalmUsbReceiveBytes` — i.e. straight through this DLL |

A failing IOCTL leaves `{0,0}`, so before the hook we always got HTAL.

## Code 2 — `0x22240C`, connection polling (`666B1830`)

```c
BOOL poll(const char *path)
{
    USHORT flag = 0;
    DWORD err = ioctl_on_path(path, 0x22240C, &flag, 2, &flag, 2);
    return err == 0 && flag != 0;
}
```

`CUSBTransportHTAL::PollConnection` (`666B2380`):

```
if (this->handle_in  == INVALID_HANDLE_VALUE) return 0x2005;
if (this->handle_out == INVALID_HANDLE_VALUE) return 0x2005;
if (!poll(g_devicePath))                      return 0x2005;
goto 666B2190;                              /* the real session */
```

`CUSBTransportPAD::PollConnection` (`666B3640`) is the same shape but checks the
`PalmUsbOpenPort` id at `this+18Ch` instead of the two handles. **Both classes gate on this
IOCTL**, so no choice of transport avoids it.

`CUSBTransportHTAL::StartListen` (`666B1140`) is `xor eax,eax / ret` — it arms nothing.
All connection detection is the poll above.

## The fix: two IAT slots

`CreateFileA` and `DeviceIoControl` are called indirectly through USBTransport.dll's
import table:

| slot | function | how it was identified |
|---|---|---|
| `666B8030` | `CreateFileA` | 7 pushed args at `666B17DF`, `0xC0000000`/`OPEN_EXISTING` |
| `666B8048` | `DeviceIoControl` | 8 pushed args at `666B180D` |
| `666B8040` | `GetLastError` | no args, result becomes the return value |
| `666B802C` | `CloseHandle` | 1 arg, 16 call sites |

`src/usbport/iat_hook.cpp` swaps the first two **in memory, in the HotSync process only**.
Nothing on disk changes and no Palm binary is modified; removing our DLL removes the hook.
`PALMUSB_NO_HOOK=1` disables it.

The hook resolves the slots **by imported function name**, not by the addresses above, so
it survives a different `USBTransport.dll` build. The addresses are recorded here as
evidence, not used as constants.

### What the hook answers

| code | answer |
|---|---|
| `0x222004` | `{0x82D, 0x100}` when `PALMUSB_TRANSPORT=pad`, otherwise zeros (HTAL, the default) |
| `0x22240C` | `1` once the handheld has sent bytes since the bridge started, else `0` |

`CreateFileA` on the device path returns a shim event handle rather than opening the
WinUSB device a second time — that handle only ever carries these two IOCTLs, and a second
open would contend with the bridge's.

## Status

**Confirmed on hardware, 2026-08-07.** With the hook installed, a Palm m125 completed a
full HotSync — four conduits plus a 13-file backup — over WinUSB with Memory Integrity and
Secure Boot enabled. The log goes straight from

```
hook: IOCTL identify -> CUSBTransportHTAL
hook: IOCTL poll -> connection pending
```

to bidirectional traffic, where every previous run stopped dead after the handheld's
greeting.

`PALMUSB_TRANSPORT=pad` was never needed: HTAL, driven by the named-pipe bridge, carries
this device's NET framing correctly. The PAD path stays available but is unverified.
