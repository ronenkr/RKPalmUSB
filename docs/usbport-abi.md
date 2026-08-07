# `USBPort.dll` ABI

Recovered by static analysis of the shipped binaries. No guessing: every statement below
is backed by disassembly archived under [evidence/disasm/](../evidence/disasm/).

## Subjects

| File | Arch | Version | SHA-256 |
|---|---|---|---|
| `C:\Windows\SysWOW64\USBPort.dll` | x86 | 7.0.1 | `A940FBB5…` (primary — this is what HotSync loads) |
| `oldusbdriver\USBPort.dll` | x64 | 7.0.1 | `F6C18830…` (same source, second reference) |
| `C:\Program Files (x86)\Palm\USBTransport.dll` | x86 | 2008-01-04 | `717B00EF…` (primary consumer) |
| `C:\Program Files (x86)\Palm\Hotsync.exe` | x86 | 7.0.2 | `7A5B4BE4…` |

## Loading

`Hotsync.exe` does **not** reference `USBPort.dll`. It loads the transport plug-in
`USBTransport.dll`, which **statically imports** `USBPort.dll` by name (undecorated).
Three other modules also consume it:

| Module | Binding | Exports used |
|---|---|---|
| `USBTransport.dll` | static import | 8 (the sync data path) |
| `AutoDetect.dll` | static import | 10 |
| `DeviceMonitor.exe` | static import | 1 (`IsPalmOSDeviceNotification`) |
| `PalmUSBDirect.dll` | `LoadLibraryA("usbport.dll")` + `GetProcAddress` | 9, all optional |

Consequence: the replacement must satisfy all four, so **implement all 12 exports**.
`PalmUSBDirect.dll` tolerates missing exports (logs `USBPORT: <name> not found`), the
other three do not — a missing name is a hard load failure.

## Calling convention

**All 12 exports are `__cdecl`.** Every export ends in a bare `ret`; every call site cleans
its own stack (`add esp, N`). There is no name decoration and no `@N` suffix, so the `.def`
file needs no `NONAME` or alias trickery — plain `extern "C"` + `__cdecl` reproduces it
exactly. Ordinals 1–12 are assigned alphabetically by name.

## Constants

**Device interface GUID: `{784126BF-4190-11D4-B5C2-00C04F687A67}`**
Stored at RVA `0x813C` in the x86 DLL; used for `SetupDiGetClassDevsA`, for
`RegisterDeviceNotificationA`, and as the comparison key inside
`PalmUsbIsPalmOSDeviceNotification`. Note the old INF's *setup class* GUID is
`{784126C0-…}` — one greater, and a different thing.

Only `PalmDevC.dll` (the old class installer) and `PalmUSBDirect.dll` hardcode this GUID
elsewhere. `USBTransport.dll` does **not** — the GUID is fully encapsulated in `USBPort.dll`.

**Port tag `0x73796E63`** — FourCC `'sync'`, passed by callers as a port-selector argument.

**Pipe name suffixes `"IN"` and `"OUT"`** (RVA `0x8134`, `0x8130`). The original opened
*two* file handles per device: `<devicePath>\IN` for reads and `<devicePath>\OUT` for
writes, each via `CreateFileA`.

**Private IOCTLs** (FILE_DEVICE_UNKNOWN, METHOD_BUFFERED, FILE_ANY_ACCESS):

| Code | Function | Use |
|---|---|---|
| `0x222008` | 0x802 | get timeouts (8-byte buffer in/out) |
| `0x22200C` | 0x803 | set timeouts (8-byte buffer in/out) |
| `0x222010` | 0x804 | query IN/OUT file names |

These vanish in the WinUSB rewrite; they are recorded only to explain the semantics.

## Status codes

Exports return a `USB_STATUS` enum, not a Win32 error. `PalmUSBDirect.dll` contains a
17-entry jump table (`0..0x10`) translating it, which is how the value space was recovered:

| Status | Meaning | Evidence |
|---|---|---|
| `0` | Success | table → 0; callers treat 0 + `*pActual == 0` as "no data yet, keep polling" |
| `2` | → PalmDirect `-5` | |
| **`3`** | **Disconnected / port gone** | `USBTransport` bails out of its receive loop immediately on 3 (→ transport error `0x2005`); never retries |
| **`4`** | **Invalid parameter / unknown port** | returned directly by `ReceiveBytes` and the timeout helper when the port id is not in the port list, or a required pointer is NULL |
| `5`, `7`, `8`, `9`, `0x10` | mapped to `-7,-8,-9,-1,-10` | exact meanings not needed for the rewrite |
| `1`, `6`, `0xA`–`0xF` | unmapped → PalmDirect `-4` | treated as generic failure |

**Only `0`, `3` and `4` are load-bearing for HotSync.** The replacement returns those three
and nothing else.

## Receive semantics (the important one)

`PalmUsbReceiveBytes` is a **buffered byte-stream read**, not a raw USB transfer:

- The port object holds an internal read buffer (`+0x10C` base, `+0x110` read cursor,
  `+0x114` bytes available). The export first `memcpy`s `min(len, available)` out of it and
  only touches the device when the buffer is dry.
- It writes `*pActual = 0` before doing anything else.
- Returning `0` with `*pActual == 0` is normal and means "nothing available yet".

`USBTransport` relies on this exactly: it asks for **10 bytes** (the SLP header), and if it
gets fewer it re-polls in a `GetTickCount` loop until its own deadline, then fails with
`0x2001`. It validates the header magic **`BE EF ED`** — Palm SLP framing. So all Palm
protocol framing lives *above* `USBPort.dll`; the DLL is a dumb byte pipe.

Since WinUSB bulk reads return whole packets, **the replacement must reproduce this
internal buffering** — read a full packet from the IN pipe, hand out only what was asked
for, keep the remainder.

## Thread safety

`ReceiveBytes`, `SendBytes` and `ClosePort` bracket their work in
`EnterCriticalSection`/`LeaveCriticalSection` on a single DLL-global critical section
(RVA `0xA904`), initialized in `DllMain`. Ports live in a singly linked list (head at RVA
`0xA920`, `next` at node `+0x118`, port id at `+0x00`, handle at `+0x04`).

## Export table

`H` = confidence high (arg count and role proven from both the callee prologue and at least
one call site). `M` = medium (arg count proven, role inferred).

| # | Export | Signature | Conf |
|---|---|---|---|
| 1 | `PalmUsbClosePort` | `int (int port)` | H |
| 2 | `PalmUsbGetAttachedDevices` | `int (void* a, void* b, void* c)` — 3 args, forwards to internal enumerator with the GUID | M |
| 3 | `PalmUsbGetDeviceFriendlyName` | `int (void* a, void* b)` — 2 args, forwards with the GUID | M |
| 4 | `PalmUsbGetFileNames` | `int (const char* devPath, DWORD tag, char* inName, DWORD* inLen, char* outName, DWORD* outLen)` | H |
| 5 | `PalmUsbGetTimeouts` | `int (int port, PALM_USB_TIMEOUTS* t)` — 8-byte struct | H |
| 6 | `PalmUsbIsPalmOSDeviceNotification` | `BOOL (DEV_BROADCAST_HDR* hdr, DWORD tag, char* outPath /*256*/, GUID* outClassGuid /*optional*/)` | H |
| 7 | `PalmUsbOpenPort` | `int (const char* devPath, DWORD tag)` → port id, `-1` on failure | H |
| 8 | `PalmUsbReceiveBytes` | `int (int port, void* buf, DWORD len, DWORD* pActual)` | H |
| 9 | `PalmUsbRegisterDeviceInterface` | `int (HWND hwnd)` → non-zero on success | H |
| 10 | `PalmUsbSendBytes` | `int (int port, const void* buf, DWORD len, DWORD* pActual)` | H |
| 11 | `PalmUsbSetTimeouts` | `int (int port, const PALM_USB_TIMEOUTS* t)` | H |
| 12 | `PalmUsbUnRegisterDeviceInterface` | `int (HDEVNOTIFY h)` | H |

### `PalmUsbIsPalmOSDeviceNotification` detail

Proven from the callee body at `66561F20`:

1. Rejects `hdr == NULL`.
2. Requires `hdr->dbch_devicetype == 5` (`DBT_DEVTYP_DEVICEINTERFACE`).
3. Reads `dbcc_classguid` at offset `0x0C` and `dbcc_name` at offset `0x1C`.
4. If `outClassGuid != NULL`, stores the 16-byte class GUID there — *before* the match test,
   so it is filled even on a non-match.
5. Compares the class GUID against the DLL's own; mismatch → return `0`.
6. On match, resolves the tag and copies the device path into `outPath`, returns `1`.

`DeviceMonitor.exe` and `USBTransport.dll` both call it from a `WM_DEVICECHANGE` handler,
for `wParam == 0x8000` (`DBT_DEVICEARRIVAL`) and `0x8004` (`DBT_DEVICEREMOVECOMPLETE`).
`USBTransport` passes a **256-byte** global as `outPath` (it clears it with
`rep stos` of `0x40` dwords), so 256 bytes is the buffer size contract.

The path this writes is later handed verbatim to `PalmUsbOpenPort`. Since the replacement
owns both ends, the path format is a private contract — it just has to round-trip.

### `PalmUsbGetFileNames` detail — and why it breaks the WinUSB approach

Two-pass sizing protocol, as used by `USBTransport.dll` at `666B1201`:

1. Call with `inName = outName = NULL`; the lengths come back in `*inLen` / `*outLen`.
2. Allocate, call again with real buffers.
3. Returns `0` on success (`test eax,eax; jne <error>`).

**Correction (found on hardware, 2026-08-07).** An earlier revision of this document
claimed the returned names were opaque and only had to round-trip back to
`PalmUsbOpenPort`. That is wrong, and it is the central problem with the drop-in approach.

`USBTransport.dll` opens the two names **itself**, immediately after the second call
(`666B1263`–`666B1292`):

```
CreateFileA(name1, GENERIC_READ,  FILE_SHARE_READ,  NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL)
CreateFileA(name2, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL)
; either handle invalid -> CloseHandle both, store -1, abandon the device
; both valid          -> keep them in [esi+4] (read) and [esi+8] (write)
```

It then drives those handles with its own `ReadFile`, `WriteFile`, `DeviceIoControl`,
`GetOverlappedResult` and `CancelIo` — all of which it imports directly. On this path
`PalmUsbOpenPort`, `PalmUsbSendBytes` and `PalmUsbReceiveBytes` are **never called**.

That is what the original driver's `"IN"` / `"OUT"` suffixes were for: two genuine kernel
device objects, one opened read-only and one write-only. A WinUSB device-interface path
cannot substitute:

- it does not support raw `ReadFile`/`WriteFile` (WinUSB requires `WinUsb_ReadPipe` or its
  own IOCTLs), and
- the two opens request conflicting share modes against a single interface.

**Consequence:** a user-mode `USBPort.dll` shim cannot serve this transport, because the
I/O never passes through it. The DLL can still serve the *other* transport class —
`PalmUsbOpenPort` is called from `CUSBTransportPAD::Reset` at `666B2760`, and that class
does route reads and writes through `PalmUsbReceiveBytes`/`PalmUsbSendBytes`.

**Tested, 2026-08-07: there is no fallback.** With `PALMUSB_NO_FILENAMES=1` forcing
`PalmUsbGetFileNames` to fail, HotSync Manager abandoned the device outright — no
`PalmUsbOpenPort` call, no session, nothing further in the log:

```
device notification matched (out_path=filled) -> TRUE
-> GetFileNames(..., pass=size-query)
GetFileNames: refusing (PALMUSB_NO_FILENAMES=1) ...
-> GetFileNames(..., pass=fetch)
GetFileNames: refusing (PALMUSB_NO_FILENAMES=1) ...
<end>
```

`CUSBTransportPAD` is never instantiated for this device, so the `PalmUsbOpenPort` /
`SendBytes` / `ReceiveBytes` path is unreachable in practice on Palm Desktop 6.2.2 with
`0830:0040`.

### Conclusion: a pure user-mode WinUSB shim cannot work here

`USBTransport.dll` requires two device paths that support `CreateFileA` plus overlapped
`ReadFile`/`WriteFile`/`DeviceIoControl`. That is a driver-provided capability, and no
amount of work inside `USBPort.dll` can synthesise it.

The `"IN"` / `"OUT"` strings recovered from the original DLL (RVA `0x8130`, `0x8134`) show
how the original satisfied this: it opened `"<devicePath>\IN"` and `"<devicePath>\OUT"`.
The trailing component is a **filename relative to the device object**, delivered to the
driver in `IRP_MJ_CREATE`, so a single device can serve both by inspecting it and routing
reads to the bulk IN pipe and writes to the bulk OUT pipe.

Reproducing that needs a driver, but **not necessarily a kernel-mode one**: a UMDF v2
driver runs in user mode under Microsoft's in-box, signed `WUDFRd.sys`, so Memory
Integrity and Secure Boot stay enabled. That preserves the project's actual security goal
while giving up its "no new driver" goal. See `docs/next-steps.md`.

## Open questions

- Exact meaning of status codes other than `0`, `3`, `4`. Not required: HotSync only
  branches on those three.
- `PalmUsbGetAttachedDevices` / `GetDeviceFriendlyName` parameter roles. Used only by
  `AutoDetect.dll` (the setup-time device wizard), not on the sync path. The replacement
  implements them conservatively — see [`palmusb_api.cpp`](../src/usbport/palmusb_api.cpp).
