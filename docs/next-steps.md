# Where the project stands, and the decision to make

## What works

| Piece | State |
|---|---|
| ABI of the original `USBPort.dll` | fully recovered, all 12 exports, `__cdecl`, ordinals 1–12 |
| Replacement DLL | builds 32-bit, CFG/ASLR/DEP, export table matches the original exactly |
| WinUSB device package | installed as `oem26.inf`; Palm `0830:0040` binds to `winusb.sys` |
| Memory Integrity / Secure Boot | never disabled at any point |
| USB communication | **works** — `palm-usb-probe` reads real data from the handheld |
| Device notification path | works — HotSync loads our DLL and it identifies the Palm correctly |

USB is not the problem. The handheld talks to us over WinUSB on endpoints `0x82`/`0x02`.

## What blocks it

`USBTransport.dll` calls `PalmUsbGetFileNames`, then **opens the two returned names itself**
with `CreateFileA` and drives them with raw overlapped `ReadFile`/`WriteFile`/
`DeviceIoControl`. On that path it never calls `PalmUsbOpenPort`, `PalmUsbSendBytes` or
`PalmUsbReceiveBytes`, so the WinUSB backend inside our DLL is never reached.

Confirmed by experiment: making `GetFileNames` fail does **not** make HotSync fall back to
the transport class that would use our DLL. It simply gives up on the device.

A WinUSB interface path cannot substitute for those names — WinUSB does not support raw
`ReadFile`/`WriteFile`, and the two opens request conflicting share modes.

**No change confined to `USBPort.dll` can fix this.** The capability HotSync needs is
supplied by a driver, not by a user-mode DLL.

## Options

### A. UMDF v2 driver (recommended if the goal is a working HotSync)

Write a user-mode driver that claims the Palm and exposes a device object accepting the
relative filenames `IN` and `OUT`, mapping reads to the bulk IN pipe and writes to the bulk
OUT pipe. `PalmUsbGetFileNames` then returns `"<devicePath>\IN"` and `"<devicePath>\OUT"`
and the existing `USBTransport.dll` code works unmodified.

- **Keeps Memory Integrity and Secure Boot enabled** — UMDF drivers run in user mode under
  Microsoft's in-box, signed `WUDFRd.sys`. No third-party kernel code.
- Reuses most of what exists: the INF, the signing setup, the endpoint discovery, and the
  buffering logic in `winusb_pipe.cpp` all carry over.
- Cost: a WDK UMDF USB driver project, an INF rewrite from WinUSB to UMDF, and the
  `IRP_MJ_CREATE` filename routing. Meaningfully more work than everything so far.
- Gives up the project's "no new driver" goal, but keeps its security goal, which was the
  actual reason that goal existed.

### B. Bypass Palm Desktop's transport entirely

Write a standalone sync tool over the working WinUSB path, using pilot-link's protocol
implementation as reference. `palm-usb-probe` is already most of the transport layer.

- No driver work; the current WinUSB package is sufficient.
- Loses Palm Desktop, its conduits, and the HotSync UI. Backup and PRC/PDB install would
  have to be reimplemented or taken from pilot-link.

### C. Try a different Palm Desktop version

The transport plug-in differs between releases; an older one may use the
`PalmUsbOpenPort` path that our DLL already implements. Cheap to test if you have another
installer, pure speculation otherwise.

### D. Stop here

The reverse-engineering results, the WinUSB package and the probe stand on their own and
are documented. `scripts\Install-Dll.ps1 -Rollback` and `docs\rollback.md` return the
machine to its original state.

## Recommendation

If a working HotSync matters, **option A**. It is the only route that reaches the goal
without disabling the protections the project exists to preserve, and the analysis needed
to write it is already done — the `IN`/`OUT` contract is recovered and documented.

If the goal was mainly to see whether the drop-in approach could work, the answer is now
established with evidence, and **option D** is a legitimate place to stop.
