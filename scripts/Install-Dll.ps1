<#
Installs (or removes) the replacement USBPort.dll for Palm Desktop.

    .\scripts\Install-Dll.ps1
    .\scripts\Install-Dll.ps1 -Rollback

The DLL is copied into the Palm Desktop program directory, NOT over the system copy in
C:\Windows\SysWOW64. Windows searches the executable's own directory before the system
directories, so Hotsync.exe picks up ours while the original stays untouched - which makes
rollback a file deletion and nothing more.

Close HotSync Manager first; the DLL cannot be replaced while it is loaded.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$PalmDir = 'C:\Program Files (x86)\Palm',
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\x86\USBPort.dll'),
    [switch]$Rollback
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $PalmDir)) { throw "Palm Desktop directory not found: $PalmDir" }
$target = Join-Path $PalmDir 'USBPort.dll'

$running = Get-Process -Name 'Hotsync', 'PalmDesktop', 'DeviceMonitor' -ErrorAction SilentlyContinue
if ($running) {
    Write-Warning 'These processes hold the DLL open; close them first:'
    $running | ForEach-Object { Write-Warning "  $($_.ProcessName) (pid $($_.Id))" }
    throw 'Cannot modify USBPort.dll while it is loaded.'
}

if ($Rollback) {
    if (-not (Test-Path $target)) {
        Write-Host 'Nothing to roll back - no USBPort.dll in the Palm directory.' -ForegroundColor Green
        Write-Host 'Palm Desktop is already using the original from SysWOW64.'
        return
    }

    # Refuse to delete a file we did not put there.
    $info = (Get-Item $target).VersionInfo
    if ($info.CompanyName -like '*PalmSource*') {
        throw "$target looks like the ORIGINAL PalmSource DLL (CompanyName '$($info.CompanyName)'), not our replacement. Refusing to delete it. Move it aside manually if that is really what you want."
    }

    if ($PSCmdlet.ShouldProcess($target, 'Remove replacement DLL')) {
        Remove-Item $target -Force
        Write-Host "Removed $target" -ForegroundColor Green
        Write-Host 'Palm Desktop will fall back to C:\Windows\SysWOW64\USBPort.dll (the original).'
    }
    return
}

if (-not (Test-Path $Source)) { throw "Built DLL not found: $Source. Run .\scripts\build.ps1 first." }

if (Test-Path $target) {
    $existing = (Get-Item $target).VersionInfo
    if ($existing.CompanyName -like '*PalmSource*') {
        $backup = Join-Path $PalmDir 'USBPort.dll.original'
        if (-not (Test-Path $backup)) {
            Write-Host "Backing up the original DLL found in the Palm directory -> $backup" -ForegroundColor Yellow
            Copy-Item $target $backup
        }
    }
}

if ($PSCmdlet.ShouldProcess($target, "Copy $Source")) {
    Copy-Item $Source $target -Force
    Write-Host "Installed $target" -ForegroundColor Green
    Write-Host ''
    Write-Host 'Start HotSync Manager, make sure USB is enabled in its menu, then press the'
    Write-Host 'HotSync button on the handheld.'
    Write-Host ''
    Write-Host 'For a diagnostic log, set PALMUSB_LOG=1 before launching HotSync Manager;'
    Write-Host 'output goes to %LOCALAPPDATA%\RKPalmUSB\usbport.log (metadata only, no payloads).'
}
