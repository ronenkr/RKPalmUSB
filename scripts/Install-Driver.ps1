<#
Installs the WinUSB device package and binds the attached Palm to winusb.sys.
Run elevated.

    .\scripts\Install-Driver.ps1 -WhatIf     # show what would happen
    .\scripts\Install-Driver.ps1

Before first use, add your device's exact hardware ID to driver\PalmWinUSB.inf. Find it
with .\scripts\Find-PalmDevice.ps1.

Windows requires a signed catalog to add a driver package to the driver store. For local
development that means either a self-signed test certificate in the machine's
Trusted Root + Trusted Publishers stores, or temporarily enabling test signing. See
docs\installation.md - this script does not change signing policy for you.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$InfPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'driver\PalmWinUSB.inf'),

    # Removes any legacy PalmUSBD/AceecaUSBD package from the driver store first. Those
    # claim the same hardware IDs, so leaving one installed lets Windows bind the Palm to
    # a kernel driver that cannot load under Memory Integrity.
    [switch]$RemoveConflicting
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must run elevated.'
}
if (-not (Test-Path $InfPath)) { throw "INF not found: $InfPath" }

$catalog = [IO.Path]::ChangeExtension($InfPath, '.cat')
if (-not (Test-Path $catalog)) {
    Write-Warning "No catalog at $catalog - pnputil will reject an unsigned package."
    Write-Warning 'Run .\scripts\Sign-Driver.ps1 first.'
}

# --- Legacy package check -------------------------------------------------------------
# Parse pnputil's record blocks: "Published Name: oemNN.inf" followed by "Original Name".
$records = @()
$current = @{}
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match '^\s*Published Name:\s*(\S+)') {
        if ($current.Count -gt 0) { $records += [pscustomobject]$current }
        $current = @{ Published = $Matches[1] }
    } elseif ($line -match '^\s*Original Name:\s*(\S+)') {
        $current['Original'] = $Matches[1]
    } elseif ($line -match '^\s*Provider Name:\s*(.+?)\s*$') {
        $current['Provider'] = $Matches[1]
    }
}
if ($current.Count -gt 0) { $records += [pscustomobject]$current }

$conflicting = $records | Where-Object { $_.Original -match 'palmusbd|aceecausbd' }

if ($conflicting) {
    Write-Host 'Legacy Palm driver package(s) still in the driver store:' -ForegroundColor Yellow
    foreach ($record in $conflicting) {
        Write-Host "  $($record.Published)  <- $($record.Original)  [$($record.Provider)]" -ForegroundColor Yellow
    }

    if (-not $RemoveConflicting) {
        Write-Host ''
        Write-Host 'These claim the same hardware IDs as PalmWinUSB.inf. Windows may keep binding' -ForegroundColor Yellow
        Write-Host 'the Palm to the old kernel driver, which cannot load under Memory Integrity.' -ForegroundColor Yellow
        Write-Host 'Re-run with -RemoveConflicting to remove them, or do it manually:' -ForegroundColor Yellow
        foreach ($record in $conflicting) {
            Write-Host "  pnputil /delete-driver $($record.Published) /uninstall /force" -ForegroundColor Cyan
        }
        Write-Host ''
    } else {
        foreach ($record in $conflicting) {
            if ($PSCmdlet.ShouldProcess($record.Published, 'pnputil /delete-driver /uninstall /force')) {
                Write-Host "Removing $($record.Published) ($($record.Original))..." -ForegroundColor Cyan
                pnputil /delete-driver $record.Published /uninstall /force
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning "  pnputil returned $LASTEXITCODE for $($record.Published)"
                }
            }
        }
        # The original INF stays at C:\Program Files (x86)\Palm\USB_Driver\ if it is ever
        # needed again, so this is reversible.
        Write-Host 'Legacy package(s) removed. The original INF remains in the Palm install directory.' -ForegroundColor Green
        Write-Host ''
    }
}

Write-Host "Installing $InfPath" -ForegroundColor Cyan

if ($PSCmdlet.ShouldProcess($InfPath, 'pnputil /add-driver /install')) {
    pnputil /add-driver $InfPath /install
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil returned $LASTEXITCODE. A signing failure is the usual cause - see docs\installation.md."
        exit $LASTEXITCODE
    }
}

Write-Host "`nVerifying..." -ForegroundColor Cyan

# Confirm Memory Integrity is still on. Keeping it enabled is the whole point of the
# project, so a regression here is a hard failure, not a warning.
$guard = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
if ($guard) {
    $hvciOn = $guard.SecurityServicesRunning -contains 2
    $color = if ($hvciOn) { 'Green' } else { 'Yellow' }
    Write-Host "  Memory Integrity (HVCI) running: $hvciOn" -ForegroundColor $color
}

Write-Host '  Palm devices and their drivers:'
$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'VID_0830|VID_082D|VID_054C|VID_4766|VID_13E8' }

if (-not $devices) {
    Write-Host '    none present - plug the handheld in and re-run' -ForegroundColor Yellow
} else {
    foreach ($device in $devices) {
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $ok = ($service -eq 'WINUSB')
        $color = if ($ok) { 'Green' } else { 'Yellow' }
        Write-Host "    $($device.FriendlyName)" -ForegroundColor $color
        Write-Host "      $($device.InstanceId)"
        Write-Host "      service: $service $(if ($ok) { '(WinUSB - correct)' } else { '(expected WINUSB)' })" -ForegroundColor $color
    }
}

Write-Host "`nNext: build\x64\palm-usb-probe.exe list" -ForegroundColor Cyan
