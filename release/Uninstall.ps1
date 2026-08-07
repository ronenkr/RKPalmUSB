<#
Reverses everything Install.ps1 did, returning the machine to stock Palm Desktop.

    Right-click PowerShell -> Run as administrator, then:

    cd <this folder>
    .\Uninstall.ps1

    .\Uninstall.ps1 -WhatIf          # show what would happen, change nothing
    .\Uninstall.ps1 -KeepDriver      # leave the WinUSB package installed
    .\Uninstall.ps1 -KeepCertificate # leave the signing certificate trusted

Palm Desktop falls back to the original C:\Windows\SysWOW64\USBPort.dll, which this
project never modified.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$PalmDir = 'C:\Program Files (x86)\Palm',
    [switch]$KeepDriver,
    [switch]$KeepCertificate
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must run elevated.'
}

$running = Get-Process -Name 'Hotsync', 'PalmDesktop', 'DeviceMonitor' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host 'Close these first - they hold USBPort.dll open:' -ForegroundColor Red
    $running | ForEach-Object { Write-Host "    $($_.ProcessName) (pid $($_.Id))" -ForegroundColor Red }
    throw 'Cannot uninstall while Palm Desktop components are running.'
}

Write-Host ''
Write-Host 'RKPalmUSB - uninstall' -ForegroundColor Cyan
Write-Host ''

# --- 1. The DLL ------------------------------------------------------------------------

$target = Join-Path $PalmDir 'USBPort.dll'
if (-not (Test-Path $target)) {
    Write-Host '[1/3] No USBPort.dll in the Palm directory - already using the original.' -ForegroundColor Green
} else {
    # Never delete the genuine PalmSource DLL. If someone copied the original into the
    # program directory by hand, removing it would break Palm Desktop rather than restore it.
    $info = (Get-Item $target).VersionInfo
    if ($info.CompanyName -like '*PalmSource*') {
        Write-Host "[1/3] $target reports CompanyName '$($info.CompanyName)'." -ForegroundColor Yellow
        Write-Host '      That is the ORIGINAL PalmSource DLL, not ours. Leaving it alone.' -ForegroundColor Yellow
    } elseif ($PSCmdlet.ShouldProcess($target, 'Remove replacement DLL')) {
        Remove-Item $target -Force
        Write-Host "[1/3] Removed $target" -ForegroundColor Green
        Write-Host '      Palm Desktop now falls back to C:\Windows\SysWOW64\USBPort.dll.' -ForegroundColor Gray
    }
}

# --- 2. Driver package -----------------------------------------------------------------

if ($KeepDriver) {
    Write-Host '[2/3] Leaving the WinUSB package installed (-KeepDriver).' -ForegroundColor Gray
} else {
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

    $ours = $records | Where-Object { $_.Original -match 'palmwinusb' -or $_.Provider -eq 'RKPalmUSB' }
    if (-not $ours) {
        Write-Host '[2/3] No RKPalmUSB driver package in the store.' -ForegroundColor Green
    } else {
        foreach ($record in $ours) {
            if ($PSCmdlet.ShouldProcess($record.Published, 'pnputil /delete-driver /uninstall /force')) {
                pnputil /delete-driver $record.Published /uninstall /force | Out-Null
                Write-Host "[2/3] Removed $($record.Published) ($($record.Original))" -ForegroundColor Green
            }
        }
        Write-Host '      Windows will re-detect the handheld as an unknown device next time.' -ForegroundColor Gray
    }
}

# --- 3. Certificate --------------------------------------------------------------------

if ($KeepCertificate) {
    Write-Host '[3/3] Leaving the signing certificate trusted (-KeepCertificate).' -ForegroundColor Gray
} else {
    $cer = Join-Path $here 'driver\RKPalmUSB.cer'
    if (-not (Test-Path $cer)) {
        Write-Host '[3/3] driver\RKPalmUSB.cer not found - cannot identify the certificate to remove.' -ForegroundColor Yellow
    } else {
        # Match on SUBJECT, not just this release's thumbprint. A certificate cannot be
        # renewed in place, so an earlier release may have left a superseded certificate
        # with the same subject trusted here. Removing only the current thumbprint would
        # orphan those - each one an extra key the machine accepts code from.
        $subject = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Subject
        $removed = 0
        foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\Root') {
            Get-ChildItem $store -ErrorAction SilentlyContinue |
                Where-Object { $_.Subject -eq $subject } |
                ForEach-Object {
                    if ($PSCmdlet.ShouldProcess("$store\$($_.Thumbprint)", 'Remove certificate')) {
                        Write-Host "      $($_.Thumbprint) (expires $($_.NotAfter)) from $store" -ForegroundColor DarkGray
                        Remove-Item $_.PSPath -Force
                        $removed++
                    }
                }
        }
        if ($removed -gt 0) {
            Write-Host "[3/3] Removed $removed certificate entr(ies) for $subject." -ForegroundColor Green
        } else {
            Write-Host '[3/3] Signing certificate was not present.' -ForegroundColor Green
        }
    }
}

Write-Host ''
Write-Host 'Uninstall complete.' -ForegroundColor Green
Write-Host 'Nothing this project installed remains. C:\Windows\SysWOW64\USBPort.dll was' -ForegroundColor Gray
Write-Host 'never modified at any point.' -ForegroundColor Gray
