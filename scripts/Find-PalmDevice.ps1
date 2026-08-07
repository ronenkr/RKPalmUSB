<#
Reports every attached device whose VID matches a known Palm OS handheld vendor, so you
can copy the exact hardware ID into driver\PalmWinUSB.inf.

    .\scripts\Find-PalmDevice.ps1
    .\scripts\Find-PalmDevice.ps1 -All     # every USB device, when the VID is unknown

Some handhelds change PID between charge mode and HotSync mode. Run this once with the
device idle and again right after pressing the HotSync button, and add both IDs.
#>
[CmdletBinding()]
param([switch]$All)

# Vendor IDs from the legacy AceecaUSBDx64.inf.
$vendors = [ordered]@{
    'VID_0830' = 'Palm'
    'VID_082D' = 'Handspring'
    'VID_054C' = 'Sony CLIE'
    'VID_13E8' = 'Access/PalmSource'
    'VID_0C88' = 'Kyocera'
    'VID_081E' = 'AlphaSmart'
    'VID_0502' = 'Acer'
    'VID_0E7C' = 'Legend'
    'VID_04E8' = 'Samsung'
    'VID_091E' = 'Garmin'
    'VID_115E' = 'GSPDA'
    'VID_0E67' = 'Fossil'
    'VID_4766' = 'Aceeca'
}

$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'USB\*' }

if (-not $All) {
    $pattern = ($vendors.Keys -join '|')
    $devices = $devices | Where-Object { $_.InstanceId -match $pattern }
}

if (-not $devices) {
    Write-Host 'No matching USB devices found.' -ForegroundColor Yellow
    Write-Host 'Plug the handheld in, or re-run with -All to list every USB device.'
    return
}

foreach ($device in $devices) {
    $vid = if ($device.InstanceId -match '(VID_[0-9A-Fa-f]{4})') { $Matches[1].ToUpper() } else { '?' }
    $brand = if ($vendors.Contains($vid)) { $vendors[$vid] } else { 'unknown vendor' }

    $hardwareIds = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
    $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data

    Write-Host "`n$($device.FriendlyName)  [$brand]" -ForegroundColor Cyan
    Write-Host "  status      : $($device.Status)"
    Write-Host "  instance    : $($device.InstanceId)"
    Write-Host "  service     : $(if ($service) { $service } else { '(none - unbound)' })"
    Write-Host '  hardware ids:'
    foreach ($id in $hardwareIds) {
        Write-Host "    $id" -ForegroundColor Green
    }
    Write-Host '  -> INF line:'
    $primary = @($hardwareIds)[0]
    Write-Host "    %PalmGeneric.DeviceDesc% = WinUSB_Install, $primary" -ForegroundColor Yellow
}

Write-Host "`nCopy the yellow line(s) into the [Palm_WinUSB.NTamd64] section of driver\PalmWinUSB.inf."
