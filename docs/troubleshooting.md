# Troubleshooting

Work top-down: driver first, then the probe, then Palm Desktop. Each layer is testable on
its own, so isolate before you debug.

```powershell
$env:PALMUSB_LOG = '1'    # then launch Hotsync.exe
# -> %LOCALAPPDATA%\RKPalmUSB\usbport.log
```

---

## `pnputil` rejects the driver package

**"The third-party INF does not contain digital signature information"** — the catalog is
unsigned. See step 3 of [installation.md](installation.md).

**"The system cannot find the file specified"** — `PalmWinUSB.cat` is missing. Generate it
with `Inf2Cat /driver:.\driver /os:10_X64`, or remove the `CatalogFile` line only if you
are using test signing.

**Installs, but the device keeps its old driver** — Windows ranked the existing package
higher. Remove it and replug:

```powershell
pnputil /enum-drivers                     # find the PalmUSBD / AceecaUSBD entry
pnputil /delete-driver oemNN.inf /uninstall /force
```

---

## `palm-usb-probe list` finds nothing

The device is not bound to `winusb.sys` under our interface GUID.

```powershell
.\scripts\Find-PalmDevice.ps1             # is it even enumerating?
```

- **Device not listed at all** — cable, port, or the handheld is asleep. Try a different
  cable; many Palm cradle cables are charge-only or simply worn out after 20 years.
- **Listed, `service` is not `WINUSB`** — the hardware ID in `PalmWinUSB.inf` does not
  match. Copy the exact ID from `Find-PalmDevice.ps1`.
- **Listed with `service: WINUSB` but the probe still finds nothing** — the
  `DeviceInterfaceGUIDs` registry value did not get written. Check:

  ```powershell
  Get-PnpDeviceProperty -InstanceId '<instance id>' -KeyName 'DEVPKEY_Device_Driver'
  # then look under HKLM\SYSTEM\CurrentControlSet\Control\Class\<driver key>\Device Parameters
  ```

  It must contain `{784126BF-4190-11D4-B5C2-00C04F687A67}`. Reinstalling the package with
  `/install` usually fixes it.

**The device appears only for a second after pressing HotSync** — that is normal for some
models, and it means the HotSync-mode PID differs from the idle one. Add both IDs to the
INF.

---

## `palm-usb-probe listen` prints only dots

Dots are read timeouts, which is the correct idle state. Bytes should appear when you
press the HotSync button.

- **Nothing ever arrives** — run `palm-usb-probe handshake`. If
  `GET_EXT_CONNECTION_INFO` reports a sync port on endpoints other than the ones
  `descriptors` selected, the device wants a different pair. The DLL follows the device's
  report automatically; the probe's `listen` uses the descriptor's first pair, so this is
  a case where the probe can fail while the DLL would work.
- **`ReadPipe failed: 31`** (`ERROR_GEN_FAILURE`) — usually a stalled endpoint or a device
  that reset. Unplug, replug, retry.
- **Data arrives but does not start `BE EF ED`** — you are reading the wrong endpoint, or
  a previous session left bytes queued. Replug and try again.

---

## HotSync Manager does not detect the handheld

**Check the DLL is actually being used.** With HotSync Manager running:

```powershell
# The Palm-directory copy should be loaded, not the SysWOW64 one.
Get-Process Hotsync | ForEach-Object { $_.Modules } | Where-Object ModuleName -eq 'USBPort.dll' |
    Select-Object FileName, FileVersionInfo
```

If it shows `C:\Windows\SysWOW64\USBPort.dll`, our copy is not in
`C:\Program Files (x86)\Palm\`. Re-run `.\scripts\Install-Dll.ps1`.

**HotSync Manager fails to start, or reports a missing module** — the export table does
not match. Run `.\scripts\Check-Abi.ps1`; all 12 exports must be present with ordinals
1–12 and machine `x86`.

**USB is disabled in HotSync Manager** — check its menu. This setting is Palm Desktop's
own and unrelated to the DLL.

**Log shows `device notification matched` but nothing after** — the arrival was seen but
`PalmUsbOpenPort` failed. The following line gives the status. Status `3` means the
device vanished between the notification and the open, which usually means the PID changed
when it entered HotSync mode: add that PID to the INF.

---

## Sync starts, then stalls or times out

**Palm Desktop reports a timeout (`0x2001`)** — `PalmUsbReceiveBytes` never produced the
10-byte SLP header inside the caller's deadline. Check the log for `ReadPipe failed`.

**Sync aborts immediately (`0x2005`)** — the DLL returned status `3` (disconnected).
Genuine unplug, or a read error being misclassified. `USBTransport.dll` treats status 3 as
fatal and never retries, so this is the one status that must be reported precisely; see
the status table in [usbport-abi.md](usbport-abi.md).

**First sync works, second one hangs** — a port was not closed. `PalmUsbClosePort` is
idempotent and aborts both pipes, so look for an `OpenPort` in the log with no matching
`ClosePort`. Restarting HotSync Manager clears it.

**Transfers are slow** — expected. These devices are USB 1.1 full speed (12 Mbit/s) and a
full backup of a well-used handheld legitimately takes minutes.

---

## Memory Integrity turned itself off

Nothing here should ever cause that; no kernel-mode code is installed. Check whether the
legacy driver got installed by accident:

```powershell
pnputil /enum-drivers | Select-String 'PalmUSBD|AceecaUSBD'
Get-CimInstance Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard |
    Select-Object SecurityServicesRunning, SecurityServicesConfigured
```

The files in `oldusbdriver/` are reference material and must never be installed. If one of
them is in the driver store, remove it with `pnputil /delete-driver … /uninstall /force`
and re-enable Memory Integrity in Windows Security → Device security → Core isolation.

---

## Collecting information for a bug report

```powershell
$env:PALMUSB_LOG = '1'
# reproduce, then gather:
Get-Content "$env:LOCALAPPDATA\RKPalmUSB\usbport.log" -Tail 200
.\scripts\Find-PalmDevice.ps1
.\build\x64\palm-usb-probe.exe descriptors
.\build\x64\palm-usb-probe.exe handshake
.\scripts\Check-Abi.ps1
```

Also useful: the HotSync log at `C:\Program Files (x86)\Palm\<username>\HotSync.log`.
Review it before sharing — it names your Palm user and the databases that synced.
