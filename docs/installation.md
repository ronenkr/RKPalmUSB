# Installation

Two independent pieces, in this order:

1. **The WinUSB device package** — tells Windows to drive the handheld with `winusb.sys`.
   One-time, per machine.
2. **The replacement `USBPort.dll`** — a file copy into the Palm Desktop directory.

Do the driver first and confirm with `palm-usb-probe` before touching Palm Desktop. If USB
communication is broken, you want to find out from the probe, not from HotSync Manager.

## Prerequisites

- Windows 11 x64, Palm Desktop installed at `C:\Program Files (x86)\Palm`
- Visual Studio 2022 Build Tools with the C++ desktop workload
- Administrator access
- The handheld and its cable

Memory Integrity and Secure Boot should stay **on**. Check:

```powershell
(Get-CimInstance Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard).SecurityServicesRunning
# 2 in the output = Memory Integrity running
```

## 1. Build and verify

```powershell
cd C:\Projects\RKPalmUSB
.\scripts\build.ps1
.\scripts\Check-Abi.ps1     # 12/12 exports, machine x86
.\scripts\Test-Load.ps1     # loads in a 32-bit process, exports callable
```

All three must pass before going further. `Check-Abi.ps1` compares against the original
`C:\Windows\SysWOW64\USBPort.dll`; any difference in name, ordinal or architecture means
HotSync Manager will not load the replacement.

## 2. Identify the device

Plug the handheld in and run:

```powershell
.\scripts\Find-PalmDevice.ps1
```

It prints an INF line ready to paste. Add it to the `[Palm_WinUSB.NTamd64]` section of
[`driver/PalmWinUSB.inf`](../driver/PalmWinUSB.inf).

Some models present a **different PID in HotSync mode than when idle**. Run the script
once idle and again immediately after pressing the HotSync button, and add both IDs. If
the device only appears for a moment, that second ID is the one that matters.

## 3. Sign the driver package

Windows will not add an unsigned package to the driver store. Pick one:

### Option A — self-signed test certificate (recommended)

Keeps Secure Boot and Memory Integrity on. The certificate is trusted only on this
machine.

```powershell
# Elevated. Creates the cert, signs the catalog, trusts it locally.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=RKPalmUSB Dev' `
    -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3)

$pwd = ConvertTo-SecureString 'rkpalm' -AsPlainText -Force
Export-PfxCertificate -Cert $cert -FilePath .\driver\RKPalmUSB.pfx -Password $pwd
Export-Certificate  -Cert $cert -FilePath .\driver\RKPalmUSB.cer

# Trust it for driver installation.
Import-Certificate -FilePath .\driver\RKPalmUSB.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\driver\RKPalmUSB.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

Then generate and sign the catalog with the WDK's `Inf2Cat` and `SignTool`:

```powershell
Inf2Cat /driver:.\driver /os:10_X64
SignTool sign /fd SHA256 /f .\driver\RKPalmUSB.pfx /p rkpalm .\driver\PalmWinUSB.cat
```

`Inf2Cat` ships with the Windows Driver Kit. Without the WDK, use Option B.

### Option B — test signing

Simpler, but weakens the machine's driver policy and puts a watermark on the desktop.

```powershell
bcdedit /set testsigning on   # elevated, then reboot
```

Turn it back off (`bcdedit /set testsigning off`) when you are done. This does **not**
disable Memory Integrity, so the project's goal still holds — but Option A is preferable.

## 4. Install the driver

```powershell
# Elevated
.\scripts\Install-Driver.ps1
```

It runs `pnputil /add-driver … /install`, then reports each Palm device's function driver
and re-checks Memory Integrity. You want `service: WINUSB`.

If the device still shows the old driver, Windows preferred the existing package. Remove
it:

```powershell
pnputil /enum-drivers                       # find the oemNN.inf for PalmUSBD/AceecaUSBD
pnputil /delete-driver oemNN.inf /uninstall /force
```

Then unplug and replug the handheld.

## 5. Prove USB works, without Palm Desktop

```powershell
.\build\x64\palm-usb-probe.exe list
.\build\x64\palm-usb-probe.exe descriptors
.\build\x64\palm-usb-probe.exe handshake
.\build\x64\palm-usb-probe.exe listen
```

With `listen` running, press the HotSync button. Expect a burst of bytes beginning
**`BE EF ED`** — Palm's SLP framing, and proof the endpoints are right.

`descriptors` and `handshake` tell you which bulk endpoints the device wants. If
`GET_EXT_CONNECTION_INFO` reports a pair different from the descriptor's first pair, the
DLL follows the device's choice automatically.

Do not continue until `listen` shows data. Everything above this line is independent of
Palm Desktop; everything below assumes USB already works.

## 6. Install the DLL

Close HotSync Manager, Palm Desktop and DeviceMonitor first.

```powershell
.\scripts\Install-Dll.ps1
```

This copies the DLL to `C:\Program Files (x86)\Palm\USBPort.dll`. It does **not** touch
`C:\Windows\SysWOW64\USBPort.dll` — Windows searches the executable's directory first, so
ours wins while the original stays in place as the fallback. Rollback is a file deletion.

## 7. First sync

1. Start HotSync Manager (`C:\Program Files (x86)\Palm\Hotsync.exe`).
2. Confirm **USB** is enabled in its menu.
3. Press the HotSync button on the handheld.

For a log, set the environment variable before launching:

```powershell
$env:PALMUSB_LOG = '1'
& 'C:\Program Files (x86)\Palm\Hotsync.exe'
# -> %LOCALAPPDATA%\RKPalmUSB\usbport.log
```

The log records state transitions, transfer sizes and error codes — never payload bytes,
because a HotSync stream carries contacts, calendar entries and memos.

Use a **disposable Palm user profile** for the first sync. Back up
`C:\Program Files (x86)\Palm\<username>` before syncing anything you care about.

## If something goes wrong

See [troubleshooting.md](troubleshooting.md). To undo everything, see
[rollback.md](rollback.md).
