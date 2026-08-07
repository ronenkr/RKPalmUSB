<#
Compares the built USBPort.dll against the original's export table. A drop-in replacement
must match on machine type, export names and ordinals - a mismatch means HotSync Manager
will fail to load it, or worse, call the wrong function.

    .\scripts\Check-Abi.ps1

Exits non-zero on any difference.
#>
[CmdletBinding()]
param(
    [string]$Candidate = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\x86\USBPort.dll'),
    [string]$Reference = 'C:\Windows\SysWOW64\USBPort.dll'
)

$ErrorActionPreference = 'Stop'

$dumpbin = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' `
    -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) { throw 'dumpbin.exe not found' }

foreach ($path in @($Candidate, $Reference)) {
    if (-not (Test-Path $path)) { throw "Not found: $path" }
}

function Get-ExportTable {
    param([string]$Path)

    $lines = & $dumpbin /nologo /exports $Path
    $exports = @{}
    foreach ($line in $lines) {
        # "    1    0 00002400 PalmUsbClosePort"
        if ($line -match '^\s+(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)\s*$') {
            $exports[$Matches[2]] = [int]$Matches[1]
        }
    }
    return $exports
}

function Get-Machine {
    param([string]$Path)
    $line = & $dumpbin /nologo /headers $Path | Select-String -Pattern 'machine \(' | Select-Object -First 1
    if ($line -match 'machine \((\w+)\)') { return $Matches[1] }
    return 'unknown'
}

$failures = @()

$candidateMachine = Get-Machine $Candidate
$referenceMachine = Get-Machine $Reference
Write-Host "machine: candidate=$candidateMachine reference=$referenceMachine"
if ($candidateMachine -ne $referenceMachine) {
    $failures += "machine mismatch: $candidateMachine vs $referenceMachine"
}

$candidateExports = Get-ExportTable $Candidate
$referenceExports = Get-ExportTable $Reference

Write-Host "exports: candidate=$($candidateExports.Count) reference=$($referenceExports.Count)`n"

foreach ($name in ($referenceExports.Keys | Sort-Object)) {
    $expected = $referenceExports[$name]
    if (-not $candidateExports.ContainsKey($name)) {
        $failures += "MISSING export: $name (ordinal $expected)"
        Write-Host ("  {0,-36} MISSING" -f $name) -ForegroundColor Red
        continue
    }
    $actual = $candidateExports[$name]
    if ($actual -ne $expected) {
        $failures += "ordinal mismatch for ${name}: $actual, expected $expected"
        Write-Host ("  {0,-36} ordinal {1} != {2}" -f $name, $actual, $expected) -ForegroundColor Red
    } else {
        Write-Host ("  {0,-36} ordinal {1}  OK" -f $name, $actual) -ForegroundColor Green
    }
}

foreach ($name in ($candidateExports.Keys | Sort-Object)) {
    if (-not $referenceExports.ContainsKey($name)) {
        Write-Host ("  {0,-36} EXTRA (harmless)" -f $name) -ForegroundColor Yellow
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host 'ABI CHECK FAILED' -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

Write-Host 'ABI CHECK PASSED - export table matches the original.' -ForegroundColor Green
exit 0
