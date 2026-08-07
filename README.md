# RKPalmUSB — Palm Desktop WinUSB drop-in transport

A drop-in replacement for Palm Desktop's `USBPort.dll` that talks to Palm OS handhelds
through Microsoft's in-box `winusb.sys` instead of the obsolete Palm/Aceeca kernel driver.
HotSync works on Windows 11 with **Memory Integrity and Secure Boot left enabled**.

Palm Desktop binaries are not modified. The only kernel-mode USB driver involved is
Microsoft's signed `winusb.sys`.

```
Hotsync.exe (32-bit)
   -> USBTransport.dll        Palm SLP/PADP framing, unchanged
      -> USBPort.dll          <- this project (32-bit, WinUSB-backed)
         -> winusb.sys        Microsoft, signed, in-box
            -> Palm handheld
```

## Status

| Milestone | State |
|---|---|
| ABI of the original `USBPort.dll` recovered | done — [docs/usbport-abi.md](docs/usbport-abi.md) |
| Replacement DLL builds, 32-bit, CFG/ASLR/DEP | done |
| Export table matches the original exactly | done — `scripts\Check-Abi.ps1` passes 12/12 |
| Loads in a 32-bit process, exports callable | done — `scripts\Test-Load.ps1` passes |
| WinUSB INF + probe tool | written, **awaiting hardware** |
| End-to-end HotSync on a real device | **not yet tested — no handheld attached** |

Everything that can be verified without a Palm has been verified. The device-dependent
parts — hardware IDs, endpoint discovery, the handshake — are written from the
[pilot-link](https://github.com/desrod/pilot-link) protocol reference and need a real
device to confirm.

## Quick start

```powershell
.\scripts\build.ps1            # build USBPort.dll (x86) and palm-usb-probe.exe (x64)
.\scripts\Check-Abi.ps1        # export table must match the original
.\scripts\Test-Load.ps1        # load the DLL in a 32-bit process and call exports

# --- with the handheld plugged in ---
.\scripts\Find-PalmDevice.ps1  # read the hardware ID, paste it into driver\PalmWinUSB.inf
.\scripts\Install-Driver.ps1   # bind the device to winusb.sys (elevated)
.\build\x64\palm-usb-probe.exe list
.\build\x64\palm-usb-probe.exe descriptors
.\build\x64\palm-usb-probe.exe listen     # press HotSync; expect bytes starting BE EF ED

# --- then hand it to Palm Desktop ---
.\scripts\Install-Dll.ps1      # copies into C:\Program Files (x86)\Palm\
.\scripts\Install-Dll.ps1 -Rollback
```

See [docs/installation.md](docs/installation.md) for the driver-signing step, which is the
one part that needs a decision from you.

## How it works

`USBPort.dll` sits below Palm's framing layers, so it never needs to understand HotSync.
It is a byte pipe with eight meaningful operations: register for device notifications,
recognise an arriving Palm, open a port, read, write, set timeouts, close, unregister.

Three details from the reverse engineering drive the implementation:

- **Everything is `__cdecl`** with undecorated names and fixed ordinals 1–12.
- **`PalmUsbReceiveBytes` is a buffered stream read.** Callers ask for exactly 10 bytes
  (the SLP header) and poll. WinUSB bulk reads return whole packets, so the DLL buffers a
  packet internally and hands out byte counts on demand.
- **Status `3` means disconnected and only that.** `USBTransport.dll` abandons a session
  immediately on 3 but retries indefinitely on any other non-zero status, so misreporting
  a transient error as 3 kills the sync, and misreporting a disconnect as anything else
  hangs it.

## Layout

```
docs/usbport-abi.md      the recovered ABI, with evidence for every claim
docs/installation.md     driver signing, install, first sync
docs/rollback.md         complete removal
docs/troubleshooting.md  symptom -> cause
evidence/                hashes and dumpbin/disassembly output for the original binaries
src/usbport/             the replacement DLL
tools/palm-usb-probe/    standalone WinUSB test tool
driver/PalmWinUSB.inf    binds Palm hardware IDs to winusb.sys
scripts/                 build, verify, install, roll back
oldusbdriver/            the legacy Aceeca driver package (reference only, never installed)
```

## Scope

Targets Palm Desktop 6.2.2 / HotSync Manager 7.0.2 with a Palm-branded (VID `0x0830`)
handheld on Windows 11 x64. Other vendors from the legacy INF are listed in
[driver/hardware-ids.md](driver/hardware-ids.md) and can be added one at a time after
testing.

Not included: an installer executable, CI, replay/fault-injection tests, or a
distribution-signed driver package.

## Legal

Contains no Palm, PalmSource, Access or Aceeca code. The replacement DLL is an original
implementation written against an ABI recovered from binaries already present on the
machine, for interoperability. `oldusbdriver/` holds the user's own copy of the legacy
package for reference and is never installed or redistributed.
