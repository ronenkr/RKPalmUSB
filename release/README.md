# RKPalmUSB — Palm Desktop USB sync on Windows 11

Makes Palm Desktop / HotSync Manager sync with a Palm OS handheld over USB on Windows 11,
**without disabling Memory Integrity or Secure Boot**.

The original Palm driver (`PalmUSBD.sys` / `AceecaUSBDx64.sys`) is unsigned-era kernel code
that will not load while Memory Integrity is on. This replaces it with Microsoft's in-box,
signed `winusb.sys` plus a user-mode drop-in replacement for `USBPort.dll`. **No
third-party kernel driver is installed.**

## What you need

- Windows 11 x64
- Palm Desktop already installed at `C:\Program Files (x86)\Palm`
- Administrator access
- The handheld and its USB cable or cradle

## Install

1. Close Palm Desktop, HotSync Manager and DeviceMonitor.
2. Right-click PowerShell → **Run as administrator**.
3. ```powershell
   cd <this folder>
   .\Install.ps1
   ```

Preview it first with `.\Install.ps1 -WhatIf` — that changes nothing.

**If you downloaded this as a zip**, Windows tags the extracted files as coming from the
internet and refuses to run the scripts. Clear that first:

```powershell
Get-ChildItem -Recurse | Unblock-File
```

You may also need `Set-ExecutionPolicy -Scope Process Bypass` for the current window.

The installer does three things, each undone by `Uninstall.ps1`:

| Step | What | Why |
|---|---|---|
| 1 | Trusts `driver\RKPalmUSB.cer` | Windows rejects unsigned driver packages. **See the warning below.** |
| 2 | `pnputil /add-driver PalmWinUSB.inf /install` | Binds the handheld to `winusb.sys` |
| 3 | Copies `USBPort.dll` into the Palm directory | The replacement transport |

Then:

1. Start `C:\Program Files (x86)\Palm\Hotsync.exe`
2. Confirm **USB** is ticked in its tray menu
3. Press the HotSync button on the handheld

### Read this before accepting the certificate

The driver package is signed with a **self-signed** certificate, not one from a public
authority. Installing it into Trusted Root means your machine will accept any driver
package or program signed by whoever holds the matching private key — not only this one.

Accept it only if you trust where this release came from. `Uninstall.ps1` removes it again.
If you would rather not, sign the package with your own certificate — see
*Signing it yourself* below — and run `.\Install.ps1 -SkipCertificate`.

## Uninstall

```powershell
.\Uninstall.ps1
```

Removes the DLL, the driver package and the certificate. `C:\Windows\SysWOW64\USBPort.dll`
— the original — is **never modified at any point**, so Palm Desktop simply falls back to
it. That is the whole rollback story: Windows searches the program directory before
SysWOW64, so our copy wins while it is present and stops mattering when it is deleted.

## Checking USB without Palm Desktop

`tools\palm-usb-probe.exe` talks to the handheld directly. Use it to tell a USB problem
apart from a Palm Desktop problem.

```powershell
.\tools\palm-usb-probe.exe list          # is the device bound to WinUSB?
.\tools\palm-usb-probe.exe descriptors   # endpoints
.\tools\palm-usb-probe.exe info          # then press HotSync once
```

`info` runs a complete handshake and prints the device's OS version, user name and clock.
If that works, USB is fine and any remaining fault is above the transport.

The handheld only appears on the USB bus **while a HotSync is in progress** and drops off
about 60 seconds later. An empty `list` with nothing syncing is normal.

## Logging

```powershell
$env:PALMUSB_LOG = '1'
& 'C:\Program Files (x86)\Palm\Hotsync.exe'
# -> %LOCALAPPDATA%\RKPalmUSB\usbport.log
```

The log records state transitions, transfer sizes and error codes. It **never records
payload bytes** — a HotSync stream carries your contacts, calendar and memos.

## If something goes wrong

**The driver will not install.** Almost always the certificate. Confirm it is trusted:

```powershell
Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Subject -like '*RKPalmUSB*' }
```

If the INF has been edited at all, the catalog no longer matches it and installation fails
— see *Adding another Palm model*.

**The handheld shows up under the old driver.** The legacy package is still in the driver
store and claims the same hardware IDs:

```powershell
.\Install.ps1 -RemoveConflicting
```

Then unplug and replug the handheld.

**HotSync never notices the handheld.** Check `USB` is ticked in the HotSync tray menu,
then confirm the device binds correctly:

```powershell
.\tools\palm-usb-probe.exe list
```

**A sync starts and then fails.** Enable the log above and look at the tail. Also read
HotSync's own log, which names the conduit that failed — it is under
`Documents\Palm OS Desktop\<user>\HotSyncLog.htm`.

## Adding another Palm model

`driver\PalmWinUSB.inf` lists these hardware IDs:

```
USB\VID_0830&PID_0040    USB\VID_0830&PID_0050    USB\VID_0830&PID_0060
USB\VID_0830&PID_0061    USB\VID_0830&PID_0062
```

Find yours with:

```powershell
pnputil /enum-devices /connected /deviceids
```

Some models report a **different PID in HotSync mode than when idle**, so check both — the
HotSync-mode one is what matters. `driver\hardware-ids.md` lists the IDs harvested from the
legacy Aceeca INF as candidates.

**Editing the INF invalidates the signed catalog**, and Windows will then refuse the
package. After editing you must re-sign it yourself.

## Signing it yourself

Needs the Windows Driver Kit for `Inf2Cat`.

```powershell
# Elevated. Creates a certificate, signs the catalog, trusts it on this machine only.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=My Palm Driver' `
    -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(20) `
    -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
Export-Certificate -Cert $cert -FilePath .\driver\My.cer

Import-Certificate -FilePath .\driver\My.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\driver\My.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

Inf2Cat /driver:.\driver /os:10_X64
SignTool sign /fd SHA256 /sha1 $cert.Thumbprint .\driver\palmwinusb.cat
```

Signing by thumbprint straight from the certificate store avoids ever writing a `.pfx` to
disk. If you do export one, keep it private — anyone holding it can sign code your machine
will trust. Then `.\Install.ps1 -SkipCertificate`.

A certificate's validity period is part of what it signs, so it cannot be extended later —
issuing a new one and re-signing is the only way to change it. In this repo that is
`scripts\Sign-Driver.ps1 -Renew -Years 20`.

## What this actually does to your machine

Worth knowing before you install it:

- Adds a WinUSB device package. The only kernel driver involved is Microsoft's `winusb.sys`.
- Adds one DLL to the Palm program directory. No system file is replaced.
- Adds one certificate to the machine's trust stores (step 1).
- **At runtime, inside the HotSync process only,** the DLL patches two entries in
  `USBTransport.dll`'s import table (`CreateFileA` and `DeviceIoControl`). Palm's transport
  refuses to start a session unless two private IOCTLs from the old kernel driver are
  answered, and this is how they get answered without one. Nothing is written to disk, no
  Palm binary is modified, and unrelated I/O is passed straight through. Set
  `PALMUSB_NO_HOOK=1` to disable it — HotSync will then detect the handheld but never sync.

## Scope and limits

Verified on a **Palm m125** with **Palm Desktop 6.2.2 / HotSync Manager 7.0.2** on Windows
11, with Memory Integrity and Secure Boot enabled: four conduits plus a 13-file backup, and
consecutive syncs without restarting HotSync Manager.

Not tested, and reasonably likely to need work:

- Other Palm models, and other Palm Desktop versions
- Unplugging during a transfer, or cancelling a sync from the desktop
- Two different handhelds in one HotSync Manager run

The import-table patch resolves its targets by function name rather than by fixed
addresses, so a different `USBTransport.dll` build should still work — but that has not
been tested against one.
