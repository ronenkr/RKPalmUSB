<#
Prints the disassembly of one function from a dumpbin /disasm listing.

    .\Get-FuncBody.ps1 -Disasm evidence\disasm\USBPort_x86.disasm.txt -Address 66562350 -Lines 60

Used during ABI recovery to read individual USBPort.dll exports.
#>
param(
    [Parameter(Mandatory)][string]$Disasm,
    [Parameter(Mandatory)][string]$Address,
    [int]$Lines = 60
)

$listing = Get-Content $Disasm
$pattern = '^\s*' + $Address + ':'
$match = $listing | Select-String -Pattern $pattern -SimpleMatch:$false | Select-Object -First 1

if (-not $match) {
    Write-Error "Address $Address not found in $Disasm"
    exit 1
}

$start = $match.LineNumber - 1
$end = [Math]::Min($listing.Count, $start + $Lines)
for ($i = $start; $i -lt $end; $i++) { $listing[$i] }
