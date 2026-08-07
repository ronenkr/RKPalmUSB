<#
Installs everything needed for Palm Desktop to sync over USB on Windows 11, without
disabling Memory Integrity or Secure Boot.

    Right-click PowerShell -> Run as administrator, then:

    cd <this folder>
    .\Install.ps1

    .\Install.ps1 -WhatIf              # show what would happen, change nothing
    .\Install.ps1 -Yes                 # no prompts (unattended)
    .\Install.ps1 -RemoveConflicting   # also remove the legacy Palm/Aceeca driver package

Three steps, each reversible with .\Uninstall.ps1:

  1. Trust the code-signing certificate that signs the driver package.
  2. Add the WinUSB device package, binding the handheld to Microsoft's winusb.sys.
  3. Copy USBPort.dll into the Palm Desktop program directory.

Step 1 is a real trust decision - read the warning it prints before accepting.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$PalmDir = 'C:\Program Files (x86)\Palm',
    [switch]$RemoveConflicting,
    [switch]$SkipCertificate,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$inf  = Join-Path $here 'driver\PalmWinUSB.inf'
$cat  = Join-Path $here 'driver\palmwinusb.cat'
$cer  = Join-Path $here 'driver\RKPalmUSB.cer'
$dll  = Join-Path $here 'USBPort.dll'

function Confirm-Step {
    param([string]$Question)
    if ($Yes) { return $true }
    $answer = Read-Host "$Question [y/N]"
    return ($answer -eq 'y' -or $answer -eq 'Y')
}

# --- Preflight -------------------------------------------------------------------------

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must run elevated. Right-click PowerShell and choose "Run as administrator".'
}

foreach ($file in @($inf, $cat, $cer, $dll)) {
    if (-not (Test-Path $file)) { throw "Missing from this release: $file" }
}
if (-not (Test-Path $PalmDir)) {
    throw "Palm Desktop not found at $PalmDir. Install Palm Desktop first, or pass -PalmDir."
}

$running = Get-Process -Name 'Hotsync', 'PalmDesktop', 'DeviceMonitor' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host 'Close these first - they hold USBPort.dll open:' -ForegroundColor Red
    $running | ForEach-Object { Write-Host "    $($_.ProcessName) (pid $($_.Id))" -ForegroundColor Red }
    throw 'Cannot install while Palm Desktop components are running.'
}

Write-Host ''
Write-Host 'RKPalmUSB - Palm Desktop USB support for Windows 11' -ForegroundColor Cyan
Write-Host '===================================================' -ForegroundColor Cyan

# Keeping these on is the entire point of the project, so report the state up front.
$guard = Get-CimInstance -ClassName Win32_DeviceGuard `
    -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
if ($guard) {
    $hvci = $guard.SecurityServicesRunning -contains 2
    Write-Host "  Memory Integrity (HVCI) running : $hvci" -ForegroundColor $(if ($hvci) { 'Green' } else { 'Gray' })
}
$secureBoot = try { Confirm-SecureBootUEFI } catch { $null }
if ($null -ne $secureBoot) {
    Write-Host "  Secure Boot enabled             : $secureBoot" -ForegroundColor $(if ($secureBoot) { 'Green' } else { 'Gray' })
}
Write-Host '  Neither is changed by this installer.' -ForegroundColor Gray
Write-Host ''

# --- 1. Certificate --------------------------------------------------------------------

if (-not $SkipCertificate) {
    $signature = Get-AuthenticodeSignature $cat
    $subject   = $signature.SignerCertificate.Subject
    $thumb     = $signature.SignerCertificate.Thumbprint
    $expires   = $signature.SignerCertificate.NotAfter

    $alreadyTrusted = Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $thumb }

    if ($alreadyTrusted) {
        Write-Host "[1/3] Certificate already trusted ($subject)." -ForegroundColor Green
    } else {
        Write-Host '[1/3] Trust the driver signing certificate' -ForegroundColor Cyan
        Write-Host ''
        Write-Host '  Windows will not add a driver package to the driver store unless its' -ForegroundColor Gray
        Write-Host '  catalog is signed by a trusted publisher. This package is signed with a' -ForegroundColor Gray
        Write-Host '  SELF-SIGNED certificate, not one issued by a public authority:' -ForegroundColor Gray
        Write-Host ''
        Write-Host "      subject    : $subject"
        Write-Host "      thumbprint : $thumb"
        Write-Host "      expires    : $expires"
        Write-Host ''
        Write-Host '  READ THIS: installing it into Trusted Root means your machine will accept' -ForegroundColor Yellow
        Write-Host '  ANY driver package or program signed by whoever holds the matching private' -ForegroundColor Yellow
        Write-Host '  key - not just this one. Accept it only if you trust the source of this' -ForegroundColor Yellow
        Write-Host '  release. Uninstall.ps1 removes it again.' -ForegroundColor Yellow
        Write-Host ''
        Write-Host '  Alternative: sign the package with your own certificate instead. See' -ForegroundColor Gray
        Write-Host '  README.md, "Signing it yourself", then re-run with -SkipCertificate.' -ForegroundColor Gray
        Write-Host ''

        if (-not (Confirm-Step '  Install this certificate as a trusted root?')) {
            throw 'Declined. Nothing was changed.'
        }
        if ($PSCmdlet.ShouldProcess($subject, 'Import to LocalMachine Root and TrustedPublisher')) {
            Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
            Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
            Write-Host '  Certificate trusted.' -ForegroundColor Green
        }
    }
} else {
    Write-Host '[1/3] Skipping certificate (-SkipCertificate).' -ForegroundColor Gray
}
Write-Host ''

# --- 2. Driver package -----------------------------------------------------------------

Write-Host '[2/3] Install the WinUSB device package' -ForegroundColor Cyan

# The legacy PalmUSBD/AceecaUSBD packages claim the same hardware IDs. If one is still in
# the driver store Windows may keep binding the handheld to a kernel driver that cannot
# load while Memory Integrity is on, and the sync will fail for a reason that looks
# nothing like this.
$records = @()
$current = @{}
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match '^\s*Published Name:\s*(\S+)') {
        if ($current.Count -gt 0) { $records += [pscustomobject]$current }
        $current = @{ Published = $Matches[1] }
    } elseif ($line -match '^\s*Original Name:\s*(\S+)') {
        $current['Original'] = $Matches[1]
    }
}
if ($current.Count -gt 0) { $records += [pscustomobject]$current }
$conflicting = $records | Where-Object { $_.Original -match 'palmusbd|aceecausbd' }

if ($conflicting) {
    Write-Host '  Legacy Palm driver package(s) found:' -ForegroundColor Yellow
    $conflicting | ForEach-Object { Write-Host "      $($_.Published)  <- $($_.Original)" -ForegroundColor Yellow }
    if ($RemoveConflicting) {
        foreach ($record in $conflicting) {
            if ($PSCmdlet.ShouldProcess($record.Published, 'pnputil /delete-driver /uninstall /force')) {
                pnputil /delete-driver $record.Published /uninstall /force | Out-Null
                Write-Host "      removed $($record.Published)" -ForegroundColor Green
            }
        }
    } else {
        Write-Host '  Re-run with -RemoveConflicting if the handheld does not bind to WinUSB below.' -ForegroundColor Yellow
    }
}

if ($PSCmdlet.ShouldProcess($inf, 'pnputil /add-driver /install')) {
    pnputil /add-driver $inf /install
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil returned $LASTEXITCODE. See README.md, section 'If the driver will not install'."
        exit $LASTEXITCODE
    }
}
Write-Host ''

# --- 3. The DLL ------------------------------------------------------------------------

Write-Host '[3/3] Install USBPort.dll' -ForegroundColor Cyan

$target = Join-Path $PalmDir 'USBPort.dll'

# Windows searches the executable's own directory before SysWOW64, so a copy here wins
# while C:\Windows\SysWOW64\USBPort.dll - the original - is never touched. That makes
# uninstalling a file deletion. If a real PalmSource DLL is somehow already sitting in the
# program directory, keep a copy before overwriting it.
if (Test-Path $target) {
    $existing = (Get-Item $target).VersionInfo
    if ($existing.CompanyName -like '*PalmSource*') {
        $backup = Join-Path $PalmDir 'USBPort.dll.original'
        if (-not (Test-Path $backup)) {
            Write-Host "  Backing up the original -> $backup" -ForegroundColor Yellow
            Copy-Item $target $backup
        }
    }
}

if ($PSCmdlet.ShouldProcess($target, "Copy USBPort.dll")) {
    Copy-Item $dll $target -Force
    Write-Host "  Installed $target" -ForegroundColor Green
}
Write-Host ''

# --- Verify ----------------------------------------------------------------------------

Write-Host 'Verifying' -ForegroundColor Cyan

if ($guard) {
    $hvci = (Get-CimInstance -ClassName Win32_DeviceGuard `
        -Namespace root\Microsoft\Windows\DeviceGuard).SecurityServicesRunning -contains 2
    Write-Host "  Memory Integrity still running : $hvci" -ForegroundColor $(if ($hvci) { 'Green' } else { 'Red' })
}

$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'VID_0830|VID_082D|VID_054C|VID_4766|VID_13E8' }

if (-not $devices) {
    Write-Host '  No handheld attached right now - that is fine.' -ForegroundColor Gray
    Write-Host '  It only appears on the bus while a HotSync is in progress.' -ForegroundColor Gray
} else {
    foreach ($device in $devices) {
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $ok = ($service -eq 'WINUSB')
        Write-Host "  $($device.FriendlyName) -> service '$service'" `
            -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
        if (-not $ok) {
            Write-Host '    Expected WINUSB. Unplug and replug the handheld, or re-run with -RemoveConflicting.' -ForegroundColor Yellow
        }
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host ''
Write-Host 'Next:' -ForegroundColor Cyan
Write-Host '  1. Start HotSync Manager:  ' -NoNewline; Write-Host "$PalmDir\Hotsync.exe"
Write-Host '  2. Check USB is ticked in its tray menu.'
Write-Host '  3. Press the HotSync button on the handheld.'
Write-Host ''
Write-Host 'To undo everything:  .\Uninstall.ps1' -ForegroundColor Gray
