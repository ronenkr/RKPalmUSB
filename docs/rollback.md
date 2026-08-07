# Rollback

Nothing this project installs is destructive, and each piece comes out independently.
Palm Desktop's own files and your Palm user data are never modified.

## Remove the DLL only

Reverts Palm Desktop to the original transport. Close HotSync Manager first.

```powershell
.\scripts\Install-Dll.ps1 -Rollback
```

This deletes `C:\Program Files (x86)\Palm\USBPort.dll`. Palm Desktop then falls back to
`C:\Windows\SysWOW64\USBPort.dll`, which was never touched.

The script refuses to delete the file if its version resource says PalmSource — that would
mean it is the original, not our replacement, and deleting it would be a real loss.

Manual equivalent:

```powershell
Remove-Item 'C:\Program Files (x86)\Palm\USBPort.dll'
```

After this, USB HotSync stops working again on Windows 11 (the original DLL needs the
kernel driver that Memory Integrity blocks), but Palm Desktop itself is fully intact —
conduits, data and serial/network sync included.

## Remove the WinUSB driver package

```powershell
# Elevated. Find the package:
pnputil /enum-drivers | Select-String -Context 2,6 'PalmWinUSB'

# Remove it (substitute the oemNN.inf reported above):
pnputil /delete-driver oemNN.inf /uninstall /force
```

Unplug and replug the handheld. It reverts to whatever driver Windows picks by default —
usually none, showing as an unknown device, which is the state before this project.

## Remove the test certificate

Only if you used Option A in [installation.md](installation.md).

```powershell
# Elevated
Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
    Where-Object Subject -eq 'CN=RKPalmUSB Dev' |
    Remove-Item

Get-ChildItem Cert:\CurrentUser\My |
    Where-Object Subject -eq 'CN=RKPalmUSB Dev' |
    Remove-Item
```

## Turn test signing back off

Only if you used Option B.

```powershell
bcdedit /set testsigning off   # elevated, then reboot
```

## Remove the logs

```powershell
Remove-Item "$env:LOCALAPPDATA\RKPalmUSB" -Recurse
```

Diagnostic logs contain device paths and error codes. They should not contain personal
data, but treat them as potentially sensitive before attaching them to a bug report.

## Full removal checklist

- [ ] `.\scripts\Install-Dll.ps1 -Rollback`
- [ ] `pnputil /delete-driver oemNN.inf /uninstall /force`
- [ ] Remove the test certificate, or `bcdedit /set testsigning off` and reboot
- [ ] `Remove-Item "$env:LOCALAPPDATA\RKPalmUSB" -Recurse`
- [ ] Delete the project directory

Confirm the machine is back to its baseline:

```powershell
Test-Path 'C:\Program Files (x86)\Palm\USBPort.dll'     # should be False
Test-Path 'C:\Windows\SysWOW64\USBPort.dll'             # should be True (untouched)
(Get-CimInstance Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard).SecurityServicesRunning
```
