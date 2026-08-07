<#
Generates PalmWinUSB.cat and signs it with a self-signed code-signing certificate.

    .\scripts\Sign-Driver.ps1                    # reuse the existing certificate
    .\scripts\Sign-Driver.ps1 -Renew             # issue a fresh one (see below)
    .\scripts\Sign-Driver.ps1 -Renew -Years 20
    .\scripts\Sign-Driver.ps1 -Trust             # also trust it here (needs elevation)

This keeps Secure Boot and Memory Integrity enabled - it does NOT touch test signing.

Re-run this after editing PalmWinUSB.inf: the catalog hashes the INF, so any edit
invalidates the signature and pnputil will reject the package.

CERTIFICATE LIFETIME
    A certificate's validity period is inside the data it signs, so it cannot be
    extended - changing -Years only takes effect together with -Renew, which issues a
    new certificate and re-signs the catalog with it. Machines that trusted the old
    certificate must trust the new one before they can install the package again;
    release\Install.ps1 does that, and driver packages already in the driver store keep
    working because Windows does not re-verify them.

TIMESTAMPING
    Without a timestamp the signature stops validating the day the certificate expires,
    rather than staying valid for code signed during its lifetime. Pass -TimestampUrl to
    add one, e.g. -TimestampUrl http://timestamp.digicert.com. This contacts that server,
    so it is off by default.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    # Resolved in the body, not here: $PSScriptRoot is empty in a param default when the
    # script is launched with `powershell -File`, which throws before anything runs.
    [string]$DriverDir,
    [string]$Subject = 'CN=RKPalmUSB Dev',

    # Where the signing key lives. CurrentUser needs no elevation; only -Trust does.
    [string]$CertStoreLocation = 'Cert:\CurrentUser\My',

    [int]$Years = 20,
    [switch]$Renew,
    [switch]$KeepSuperseded,
    [switch]$Trust,
    [string]$TimestampUrl
)

$ErrorActionPreference = 'Stop'

if (-not $DriverDir) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $DriverDir = Join-Path (Split-Path -Parent $scriptDir) 'driver'
}

$inf = Join-Path $DriverDir 'PalmWinUSB.inf'
if (-not (Test-Path $inf)) { throw "INF not found: $inf" }

$inf2cat = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $inf2cat)  { throw 'Inf2Cat.exe not found. Install the Windows Driver Kit.' }
if (-not $signtool) { throw 'signtool.exe not found. Install the Windows SDK signing tools.' }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($CertStoreLocation -like 'Cert:\LocalMachine\*' -and -not $elevated) {
    throw "Writing to $CertStoreLocation requires elevation. Run elevated, or keep the default Cert:\CurrentUser\My."
}
if ($Trust -and -not $elevated) { throw '-Trust writes to the LocalMachine trust stores and requires elevation.' }

Write-Host "Inf2Cat : $inf2cat"
Write-Host "SignTool: $signtool`n"

# --- 1. Certificate -------------------------------------------------------------------

# Search every store the key might live in, so an older certificate is found wherever a
# previous run put it. Newest expiry wins, which keeps selection deterministic when more
# than one exists.
$searchStores = @($CertStoreLocation, 'Cert:\CurrentUser\My', 'Cert:\LocalMachine\My') | Select-Object -Unique
$existing = foreach ($store in $searchStores) {
    Get-ChildItem $store -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey }
}
$existing = $existing | Sort-Object NotAfter -Descending
$superseded = @()

if ($existing -and -not $Renew) {
    $cert = $existing | Select-Object -First 1
    Write-Host "Reusing certificate $($cert.Thumbprint)" -ForegroundColor Green
    Write-Host "  expires $($cert.NotAfter)" -ForegroundColor DarkGray
    if ($cert.NotAfter -lt (Get-Date).AddYears(1)) {
        Write-Warning 'That certificate expires within a year. Re-run with -Renew to issue a fresh one.'
    }
} else {
    if ($existing) {
        $superseded = $existing
        Write-Host 'Superseding:' -ForegroundColor Yellow
        $superseded | ForEach-Object { Write-Host "  $($_.Thumbprint)  expires $($_.NotAfter)" -ForegroundColor Yellow }
    }
    if (-not $PSCmdlet.ShouldProcess($Subject, "Create self-signed code-signing certificate valid $Years years")) { return }

    # KeyUsage DigitalSignature with no CA basic-constraint: this key can sign code, but
    # cannot issue further certificates. That bounds what trusting it as a root means.
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -CertStoreLocation $CertStoreLocation -NotAfter (Get-Date).AddYears($Years) `
        -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
    Write-Host "Created certificate $($cert.Thumbprint)" -ForegroundColor Green
    Write-Host "  valid $($cert.NotBefore) -> $($cert.NotAfter)" -ForegroundColor Green
}

$cerPath = Join-Path $DriverDir 'RKPalmUSB.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
Write-Host "  public certificate -> $cerPath" -ForegroundColor DarkGray

# --- 2. Trust (optional, elevated) ------------------------------------------------------

if ($Trust) {
    # A driver package is accepted only if the signer chains to a trusted root AND the
    # publisher is trusted, so a self-signed certificate goes into both stores.
    foreach ($store in 'Root', 'TrustedPublisher') {
        $already = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
        if ($already) {
            Write-Host "  already trusted in LocalMachine\$store" -ForegroundColor DarkGray
        } elseif ($PSCmdlet.ShouldProcess("LocalMachine\$store", 'Import certificate')) {
            Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            Write-Host "  trusted in LocalMachine\$store" -ForegroundColor Green
        }
    }

    if ($superseded -and -not $KeepSuperseded) {
        # Leaving a retired signing certificate in the trust stores widens what the machine
        # accepts for no benefit.
        foreach ($old in $superseded) {
            foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher', 'Cert:\CurrentUser\Root') {
                Get-ChildItem $store -ErrorAction SilentlyContinue |
                    Where-Object { $_.Thumbprint -eq $old.Thumbprint } |
                    ForEach-Object {
                        if ($PSCmdlet.ShouldProcess("$store\$($old.Thumbprint)", 'Remove superseded certificate')) {
                            Remove-Item $_.PSPath -Force
                            Write-Host "  removed superseded cert from $store" -ForegroundColor Green
                        }
                    }
            }
        }
    }
} elseif ($superseded) {
    Write-Host ''
    Write-Warning 'The superseded certificate is still trusted on this machine. Re-run elevated with -Trust to swap it out.'
}

# --- 3. Catalog -------------------------------------------------------------------------

Write-Host "`nGenerating catalog..." -ForegroundColor Cyan
$cat = Join-Path $DriverDir 'PalmWinUSB.cat'
Remove-Item $cat -ErrorAction SilentlyContinue

if ($PSCmdlet.ShouldProcess($DriverDir, 'Inf2Cat')) {
    # 10_X64 covers Windows 10 and 11 x64.
    & $inf2cat /driver:$DriverDir /os:10_X64
    if ($LASTEXITCODE -ne 0) {
        throw "Inf2Cat failed ($LASTEXITCODE). The INF has a syntax or section error - read the output above."
    }
    Write-Host "  -> $cat" -ForegroundColor Green
}

# --- 4. Signature -----------------------------------------------------------------------

Write-Host "`nSigning catalog..." -ForegroundColor Cyan
if ($PSCmdlet.ShouldProcess($cat, 'SignTool sign')) {
    $signArgs = @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint)
    if ($TimestampUrl) { $signArgs += @('/tr', $TimestampUrl, '/td', 'SHA256') }
    $signArgs += $cat

    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)" }

    $signature = Get-AuthenticodeSignature $cat
    Write-Host ''
    Write-Host "  status     : $($signature.Status)" -ForegroundColor $(if ($signature.Status -eq 'Valid') { 'Green' } else { 'Yellow' })
    Write-Host "  signer     : $($signature.SignerCertificate.Subject)"
    Write-Host "  expires    : $($signature.SignerCertificate.NotAfter)"
    Write-Host "  timestamped: $(if ($signature.TimeStamperCertificate) { 'yes' } else { 'no  (signature stops validating when the certificate expires)' })"

    # A freshly issued self-signed certificate is not trusted anywhere yet, so the
    # signature reports UnknownError/UntrustedRoot until it is imported. That is expected
    # and says nothing about whether the signing itself worked - spell it out rather than
    # leave a scary status code sitting there.
    if ($signature.Status -ne 'Valid') {
        $trusted = Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
        if (-not $trusted) {
            Write-Host ''
            Write-Host '  The catalog is signed correctly; this certificate is simply not trusted on' -ForegroundColor Gray
            Write-Host '  this machine yet, so the chain cannot be validated. It becomes Valid once' -ForegroundColor Gray
            Write-Host '  release\Install.ps1 imports it, or after re-running this elevated with -Trust.' -ForegroundColor Gray
        }
    }
}

Write-Host ''
Write-Host 'Done. Copy driver\PalmWinUSB.cat and driver\RKPalmUSB.cer into release\driver\.' -ForegroundColor Green
