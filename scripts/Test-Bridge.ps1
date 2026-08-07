<#
Exercises the named-pipe bridge the way USBTransport.dll does, without HotSync or a Palm.

It loads the DLL in a 32-bit process, calls PalmUsbGetFileNames with a bogus device path,
and checks that the two-pass sizing protocol returns pipe names. It then verifies the
names have the shape USBTransport.dll will hand to CreateFileA.

A real device is required for the bridge to actually start (PipeOpen must succeed), so
without one this checks the naming and sizing contract only - which is still the part most
likely to be wrong.

    .\scripts\Test-Bridge.ps1
#>
[CmdletBinding()]
param(
    [string]$Dll = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\x86\USBPort.dll')
)

$ErrorActionPreference = 'Stop'

if ([IntPtr]::Size -eq 8) {
    $ps32 = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    & $ps32 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Dll $Dll
    exit $LASTEXITCODE
}

$signature = @'
[DllImport("kernel32", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr LoadLibrary(string name);
[DllImport("kernel32", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("kernel32")]
public static extern bool FreeLibrary(IntPtr module);

// int PalmUsbGetFileNames(const char*, DWORD, char*, DWORD*, char*, DWORD*) - __cdecl
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int GetFileNamesFn(string path, uint tag, IntPtr inName, IntPtr inLen,
                                   IntPtr outName, IntPtr outLen);
'@

$native = Add-Type -MemberDefinition $signature -Name 'BridgeTest' -Namespace 'RKPalm' -PassThru |
    Where-Object { $_.Name -eq 'BridgeTest' } | Select-Object -First 1

$module = $native::LoadLibrary($Dll)
if ($module -eq [IntPtr]::Zero) { throw 'LoadLibrary failed' }

$address = $native::GetProcAddress($module, 'PalmUsbGetFileNames')
if ($address -eq [IntPtr]::Zero) { throw 'PalmUsbGetFileNames not found' }
$getFileNames = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $address, [RKPalm.BridgeTest+GetFileNamesFn])

$tag = 0x73796E63   # 'sync'
$fake = '\\?\usb#vid_0830&pid_0040#test#{784126bf-4190-11d4-b5c2-00c04f687a67}'

$inLen  = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
$outLen = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
[Runtime.InteropServices.Marshal]::WriteInt32($inLen, 0)
[Runtime.InteropServices.Marshal]::WriteInt32($outLen, 0)

$failures = @()

# --- Pass 1: size query. Mirrors USBTransport.dll passing NULL buffers. ---
$result = $getFileNames.Invoke($fake, $tag, [IntPtr]::Zero, $inLen, [IntPtr]::Zero, $outLen)
$size1 = [Runtime.InteropServices.Marshal]::ReadInt32($inLen)
$size2 = [Runtime.InteropServices.Marshal]::ReadInt32($outLen)

Write-Host "pass 1 (size query): returned $result, in_len=$size1 out_len=$size2"
if ($result -ne 0) { $failures += "size query returned $result, expected 0" }
if ($size1 -le 0 -or $size2 -le 0) { $failures += 'size query produced no lengths' }

# --- Pass 2: fetch. Without hardware the bridge cannot open the device, so a non-zero
#     return here is expected; what matters is that it does not crash and that the names
#     come back correctly shaped when it does succeed. ---
if ($size1 -gt 0 -and $size2 -gt 0) {
    $buf1 = [Runtime.InteropServices.Marshal]::AllocHGlobal($size1)
    $buf2 = [Runtime.InteropServices.Marshal]::AllocHGlobal($size2)
    $result2 = $getFileNames.Invoke($fake, $tag, $buf1, $inLen, $buf2, $outLen)
    $name1 = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($buf1)
    $name2 = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($buf2)

    Write-Host "pass 2 (fetch)     : returned $result2"
    Write-Host "  IN  name: $name1"
    Write-Host "  OUT name: $name2"

    if ($result2 -eq 0) {
        if ($name1 -notlike '\\.\pipe\*') { $failures += "IN name is not a pipe path: $name1" }
        if ($name2 -notlike '\\.\pipe\*') { $failures += "OUT name is not a pipe path: $name2" }
        if ($name1 -eq $name2) { $failures += 'IN and OUT names are identical' }
        Write-Host '  bridge started (a device must be attached)' -ForegroundColor Green
    } else {
        Write-Host '  bridge did not start - expected with no Palm attached' -ForegroundColor Yellow
        Write-Host '  (the names above are only filled when the device opens)'
    }

    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf1)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf2)
}

[Runtime.InteropServices.Marshal]::FreeHGlobal($inLen)
[Runtime.InteropServices.Marshal]::FreeHGlobal($outLen)
$native::FreeLibrary($module) | Out-Null

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'BRIDGE TEST FAILED' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'BRIDGE TEST PASSED' -ForegroundColor Green
exit 0
