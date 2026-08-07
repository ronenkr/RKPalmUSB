<#
Generates PalmWinUSB.cat and signs it with a self-signed development certificate, then
trusts that certificate on this machine so pnputil will accept the package.

    .\scripts\Sign-Driver.ps1        # elevated

This keeps Secure Boot and Memory Integrity enabled - it does NOT touch test signing. The
certificate is trusted only on this machine and is removed by scripts\Uninstall-All.ps1.

Re-run this after editing PalmWinUSB.inf: the catalog hashes the INF, so any edit
invalidates the signature.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$DriverDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'driver'),
    [string]$Subject = 'CN=RKPalmUSB Dev',
    [string]$PfxPassword = 'rkpalm'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must run elevated (certificates go into the LocalMachine store).'
}

$inf = Join-Path $DriverDir 'PalmWinUSB.inf'
if (-not (Test-Path $inf)) { throw "INF not found: $inf" }

$inf2cat = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $inf2cat)  { throw 'Inf2Cat.exe not found. Install the Windows Driver Kit, or use test signing (docs\installation.md option B).' }
if (-not $signtool) { throw 'signtool.exe not found. Install the Windows SDK signing tools.' }

Write-Host "Inf2Cat : $inf2cat"
Write-Host "SignTool: $signtool`n"

# --- 1. Certificate -------------------------------------------------------------------

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1

if ($cert) {
    Write-Host "Reusing existing certificate $($cert.Thumbprint)" -ForegroundColor Green
} elseif ($PSCmdlet.ShouldProcess($Subject, 'Create self-signed code-signing certificate')) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(3) `
        -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
    Write-Host "Created certificate $($cert.Thumbprint)" -ForegroundColor Green
}

# A driver package is only accepted if its signer chains to a trusted root AND the
# publisher is trusted. A self-signed cert must therefore be placed in both stores.
$cerPath = Join-Path $DriverDir 'RKPalmUSB.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

foreach ($store in 'Root', 'TrustedPublisher') {
    $already = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if ($already) {
        Write-Host "  already trusted in LocalMachine\$store" -ForegroundColor DarkGray
        continue
    }
    if ($PSCmdlet.ShouldProcess("LocalMachine\$store", 'Import certificate')) {
        Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Write-Host "  trusted in LocalMachine\$store" -ForegroundColor Green
    }
}

# --- 2. Catalog -----------------------------------------------------------------------

Write-Host "`nGenerating catalog..." -ForegroundColor Cyan
$cat = Join-Path $DriverDir 'PalmWinUSB.cat'
Remove-Item $cat -ErrorAction SilentlyContinue

if ($PSCmdlet.ShouldProcess($DriverDir, 'Inf2Cat')) {
    # 10_X64 covers Windows 10 and 11 x64.
    & $inf2cat /driver:$DriverDir /os:10_X64 /verbose
    if ($LASTEXITCODE -ne 0) {
        throw "Inf2Cat failed ($LASTEXITCODE). The INF has a syntax or section error - read the output above."
    }
    Write-Host "  -> $cat" -ForegroundColor Green
}

# --- 3. Signature ---------------------------------------------------------------------

Write-Host "`nSigning catalog..." -ForegroundColor Cyan
if ($PSCmdlet.ShouldProcess($cat, 'SignTool sign')) {
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My /sm $cat
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)" }

    & $signtool verify /pa /v $cat
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'Catalog signed but verification failed. pnputil will probably reject it.'
    }
}

Write-Host "`nSigned. Next: .\scripts\Install-Driver.ps1" -ForegroundColor Green
