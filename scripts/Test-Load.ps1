<#
Loads the 32-bit replacement DLL in a 32-bit process, resolves every export, and calls the
ones that are safe to invoke without hardware. Catches missing imports, a wrong PE
architecture, and DllMain crashes before HotSync Manager ever sees the DLL.

    .\scripts\Test-Load.ps1

Re-launches itself under the 32-bit PowerShell when started from the 64-bit one.
#>
[CmdletBinding()]
param(
    [string]$Dll = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\x86\USBPort.dll')
)

$ErrorActionPreference = 'Stop'

# A 32-bit DLL can only be loaded by a 32-bit process.
if ([IntPtr]::Size -eq 8) {
    $ps32 = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path $ps32)) { throw '32-bit PowerShell not found' }
    & $ps32 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Dll $Dll
    exit $LASTEXITCODE
}

if (-not (Test-Path $Dll)) { throw "Not found: $Dll. Run .\scripts\build.ps1 first." }
Write-Host "Loading $Dll in a $(if ([IntPtr]::Size -eq 4) { '32' } else { '64' })-bit process...`n"

$signature = @'
[DllImport("kernel32", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr LoadLibrary(string name);
[DllImport("kernel32", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("kernel32", SetLastError=true)]
public static extern bool FreeLibrary(IntPtr module);

// PalmUsbIsPalmOSDeviceNotification(hdr, tag, outPath, outGuid) - __cdecl.
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int IsNotifyFn(IntPtr hdr, uint tag, IntPtr outPath, IntPtr outGuid);

// PalmUsbOpenPort(devicePath, tag) - __cdecl.
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int OpenPortFn(string path, uint tag);

// PalmUsbGetTimeouts(port, timeouts) - __cdecl.
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate int GetTimeoutsFn(int port, IntPtr timeouts);
'@

# -PassThru yields the generated type plus its nested delegate types; take the container.
$native = Add-Type -MemberDefinition $signature -Name 'PalmLoader' -Namespace 'RKPalm' -PassThru |
    Where-Object { $_.Name -eq 'PalmLoader' } | Select-Object -First 1

$module = $native::LoadLibrary($Dll)
if ($module -eq [IntPtr]::Zero) {
    throw "LoadLibrary failed with Win32 error $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
}
Write-Host "LoadLibrary OK (handle 0x$($module.ToString('X')))" -ForegroundColor Green

$exports = @(
    'PalmUsbClosePort', 'PalmUsbGetAttachedDevices', 'PalmUsbGetDeviceFriendlyName',
    'PalmUsbGetFileNames', 'PalmUsbGetTimeouts', 'PalmUsbIsPalmOSDeviceNotification',
    'PalmUsbOpenPort', 'PalmUsbReceiveBytes', 'PalmUsbRegisterDeviceInterface',
    'PalmUsbSendBytes', 'PalmUsbSetTimeouts', 'PalmUsbUnRegisterDeviceInterface'
)

$missing = @()
$addresses = @{}
Write-Host "`nResolving exports:"
foreach ($name in $exports) {
    $address = $native::GetProcAddress($module, $name)
    if ($address -eq [IntPtr]::Zero) {
        Write-Host ("  {0,-36} MISSING" -f $name) -ForegroundColor Red
        $missing += $name
    } else {
        Write-Host ("  {0,-36} 0x{1:X}" -f $name, $address.ToInt64()) -ForegroundColor Green
        $addresses[$name] = $address
    }
}

$failures = @()
if ($missing.Count -gt 0) { $failures += "$($missing.Count) export(s) missing" }

Write-Host "`nCalling the exports that are safe without hardware:"

# Invalid arguments must be rejected, not crash. Stack imbalance from a wrong calling
# convention would corrupt this process and show up here immediately.
$fn = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $addresses['PalmUsbIsPalmOSDeviceNotification'], [RKPalm.PalmLoader+IsNotifyFn])
$result = $fn.Invoke([IntPtr]::Zero, 0x73796E63, [IntPtr]::Zero, [IntPtr]::Zero)
if ($result -eq 0) {
    Write-Host '  IsPalmOSDeviceNotification(NULL) -> FALSE  OK' -ForegroundColor Green
} else {
    Write-Host "  IsPalmOSDeviceNotification(NULL) -> $result  EXPECTED 0" -ForegroundColor Red
    $failures += 'IsPalmOSDeviceNotification did not reject a NULL header'
}

$fn = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $addresses['PalmUsbOpenPort'], [RKPalm.PalmLoader+OpenPortFn])
$result = $fn.Invoke('', 0x73796E63)
if ($result -eq -1) {
    Write-Host '  OpenPort("") -> -1  OK' -ForegroundColor Green
} else {
    Write-Host "  OpenPort('') -> $result  EXPECTED -1" -ForegroundColor Red
    $failures += 'OpenPort accepted an empty path'
}

# Status 4 == invalid parameter / unknown port (docs\usbport-abi.md).
$fn = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $addresses['PalmUsbGetTimeouts'], [RKPalm.PalmLoader+GetTimeoutsFn])
$buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal(8)
$result = $fn.Invoke(999, $buffer)
[Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
if ($result -eq 4) {
    Write-Host '  GetTimeouts(bogus port) -> 4 (invalid param)  OK' -ForegroundColor Green
} else {
    Write-Host "  GetTimeouts(bogus port) -> $result  EXPECTED 4" -ForegroundColor Red
    $failures += 'GetTimeouts returned the wrong status for an unknown port'
}

$native::FreeLibrary($module) | Out-Null
Write-Host "`nFreeLibrary OK - DllMain(PROCESS_DETACH) did not fault." -ForegroundColor Green

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'LOAD TEST FAILED' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'LOAD TEST PASSED' -ForegroundColor Green
exit 0
