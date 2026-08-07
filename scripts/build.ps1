<#
Builds the x86 USBPort.dll replacement and the x64 palm-usb-probe tool using the
VS 2022 Build Tools installed at
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools.

    .\scripts\build.ps1                       # build everything
    .\scripts\build.ps1 -Target dll           # just the DLL
    .\scripts\build.ps1 -Configuration Debug

Output lands in build\x86\ and build\x64\.
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'dll', 'probe')][string]$Target = 'all',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vcvarsall = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'

if (-not (Test-Path $vcvarsall)) {
    throw "vcvarsall.bat not found at $vcvarsall"
}

$isDebug = ($Configuration -eq 'Debug')
$optimize = if ($isDebug) { '/Od /Zi /MTd' } else { '/O2 /MT /GL' }
$link = if ($isDebug) { '/DEBUG' } else { '/LTCG /OPT:REF /OPT:ICF' }

# /guard:cf, /DYNAMICBASE and /NXCOMPAT keep the DLL loadable in a hardened process.
$common = "/nologo /W4 /WX /EHsc /GS /guard:cf /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS $optimize"
$commonLink = "/nologo /DYNAMICBASE /NXCOMPAT $link"

function Invoke-Cl {
    param([string]$Arch, [string]$CommandLine)

    # vcvarsall must run in the same cmd.exe invocation as cl for its environment to
    # apply. Its stderr carries a harmless vswhere warning on this machine, so route
    # everything through a file rather than PowerShell's stderr merge - in 5.1 that
    # merge turns native stderr into terminating errors.
    $log = [System.IO.Path]::GetTempFileName()
    $script = "call `"$vcvarsall`" $Arch >nul 2>nul && $CommandLine"
    cmd.exe /c $script > $log 2>&1
    $exit = $LASTEXITCODE

    Get-Content $log | Where-Object { $_ -notmatch '^Microsoft \(R\)|^Copyright \(C\)|^\s*$' }
    Remove-Item $log -ErrorAction SilentlyContinue

    if ($exit -ne 0) {
        throw "Build failed for $Arch (exit $exit)"
    }
}

if ($Target -in 'all', 'dll') {
    Write-Host 'Building USBPort.dll (x86)...' -ForegroundColor Cyan
    $out = Join-Path $root 'build\x86'
    New-Item -ItemType Directory -Force $out | Out-Null

    $sources = @(
        'src\usbport\dllmain.cpp'
        'src\usbport\palmusb_api.cpp'
        'src\usbport\winusb_pipe.cpp'
        'src\usbport\port_table.cpp'
        'src\usbport\log.cpp'
    ) | ForEach-Object { "`"$(Join-Path $root $_)`"" }

    $cmd = "cl $common /LD /Fe:`"$out\USBPort.dll`" /Fo:`"$out\\`" /Fd:`"$out\USBPort.pdb`" " +
           "$($sources -join ' ') /link $commonLink /DEF:`"$root\src\usbport\exports.def`" " +
           'winusb.lib setupapi.lib user32.lib shell32.lib'

    # HotSync Manager is a 32-bit process, so this DLL MUST be 32-bit (PE32, machine 14C).
    # 'amd64_x86' selects the 64-bit *host* compiler with a 32-bit *target* - the output is
    # identical to vcvars32.bat's, but the compiler itself is not capped at a 2 GB address
    # space. Use 'x86' here instead if you want the fully 32-bit toolchain.
    # scripts\Check-Abi.ps1 fails the build if the target arch ever drifts.
    Invoke-Cl -Arch 'amd64_x86' -CommandLine $cmd
    Write-Host "  -> $out\USBPort.dll" -ForegroundColor Green
}

if ($Target -in 'all', 'probe') {
    Write-Host 'Building palm-usb-probe.exe (x64)...' -ForegroundColor Cyan
    $out = Join-Path $root 'build\x64'
    New-Item -ItemType Directory -Force $out | Out-Null

    $cmd = "cl $common /Fe:`"$out\palm-usb-probe.exe`" /Fo:`"$out\\`" /Fd:`"$out\probe.pdb`" " +
           "`"$root\tools\palm-usb-probe\main.cpp`" /link $commonLink " +
           'winusb.lib setupapi.lib'

    Invoke-Cl -Arch 'amd64' -CommandLine $cmd
    Write-Host "  -> $out\palm-usb-probe.exe" -ForegroundColor Green
}

Write-Host 'Build complete.' -ForegroundColor Green
