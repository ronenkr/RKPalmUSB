# Where the project stands

## It works — 2026-08-07

A full HotSync completed against a Palm m125 over WinUSB, with **Memory Integrity and
Secure Boot enabled throughout** and no third-party kernel driver loaded:

```
Date Book synchronized successfully
Address Book synchronized successfully
To Do List synchronized successfully
Memo Pad synchronized successfully
Backed up 13 file(s) successfully
HotSync session completed successfully on 08/07/26 15:56:02
```

The only kernel driver in the path is Microsoft's in-box, signed `winusb.sys`.

## What it took

| Piece | Result |
|---|---|
| ABI of the original `USBPort.dll` | fully recovered — 12 exports, `__cdecl`, ordinals 1–12 ([usbport-abi.md](usbport-abi.md)) |
| Replacement DLL | x86, CFG/ASLR/DEP, export table byte-identical in shape to the original |
| WinUSB device package | `oem26.inf`; Palm `0830:0040` binds to `winusb.sys` |
| Named-pipe bridge | serves the two names `PalmUsbGetFileNames` returns, which `USBTransport.dll` opens itself |
| IAT hook | answers the two private `PalmUSBD.sys` IOCTLs `USBTransport.dll` gates its session on ([usbtransport-ioctls.md](usbtransport-ioctls.md)) |
| Wire protocol | NET + DLP, verified independently by `palm-usb-probe` ([protocol-notes.md](protocol-notes.md)) |

Deployment is still one file in `C:\Program Files (x86)\Palm\`. The system copy in
`SysWOW64` is never touched, so rollback is a deletion:
`scripts\Install-Dll.ps1 -Rollback`.

## The two blockers, and what they actually were

**1. HotSync never started a session.** Not framing, not the pipes, not the protocol.
`CUSBTransport*::PollConnection` refuses to proceed unless a private IOCTL (`0x22240C`)
on the device path answers, and `winusb.sys` implements no such code. It returned
`0x2005` ("no connection") on every poll, forever. `StartListen` is a stub — that poll is
the entire connection-detection mechanism. Answering the IOCTL from an IAT hook inside our
own DLL fixed it. See [usbtransport-ioctls.md](usbtransport-ioctls.md) for the
disassembly.

**2. The backup conduit aborted with `Protocol Error (6410)`.** This one was our bug.
`WinUsb_ReadPipe` fills `transferred` *even when it fails with `ERROR_SEM_TIMEOUT`*, and
`PipeFill` returned early without banking those bytes — silently discarding data that was
already off the wire. Small conduits never straddled the timeout and passed; the one large
database did, lost a chunk, and HotSync then waited forever for a NET message that could
never complete. Confirmed after the fix by the log line
`read timed out with 256 bytes salvaged` — bytes the previous build dropped.

The lesson worth keeping: a silent truncation is indistinguishable from a peer protocol
fault. It cost a full round of misdiagnosis pointed at framing.

## Superseded conclusions

Earlier revisions of this file recommended a UMDF driver (option A) or abandoning Palm
Desktop (option B), on the reasoning that **"no change confined to `USBPort.dll` can fix
this."** That was wrong. It was true that the *documented exports* could not, but the IAT
hook ships inside the same DLL and needs no driver, so the deployment story never changed.
Options A–D are closed.

## Lifecycle

**Repeat syncs work** — verified 2026-08-07. Two consecutive syncs in one HotSync Manager
run, second one `completed successfully` in 4.0 s.

That needed a third fix. `BridgeStart` treated a matching device path as proof the bridge
was still live and short-circuited with `already running for this device`, handing HotSync
pipe names that no thread was servicing and a WinUSB handle for a device that had left the
bus. The handheld **re-enumerates under an identical path string** on every press, so path
equality proves nothing about session liveness. `BridgeAliveLocked()` now checks the pump
threads with `WaitForSingleObject(thread, 0)` and the port's disconnected flag instead;
`BridgeConnectionPending()` is gated on the same test, so the hooked poll IOCTL cannot
report a connection from a dead bridge.

## Remaining work

Nothing is blocking. Untested:

- Unplug during transfer, and cancel from the desktop. The pump threads exit the same way
  they do at a clean session end, so this *should* be covered, but it has not been run.
- Two different handhelds in one HotSync run — now takes the restart branch, unverified.
- Two-way record edits and a PRC install, to exercise paths a backup does not.
- Harvest other brands' PIDs from `AceecaUSBDx64.inf` if non-Palm devices matter.
- `docs/installation.md` and `docs/troubleshooting.md` still assume the pre-hook state.
