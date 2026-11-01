#requires -Version 5.1
<#
.SYNOPSIS
  Authenticode signing step for the release pipeline -- with an explicit SKIP
  when no certificate is configured.

.DESCRIPTION
  ORDER MATTERS: sign the payload (PrismArchive.exe, PrismShellExt.dll,
  PrismShellExtClassic.dll) BEFORE New-PortablePackage.ps1 runs.  Signing
  rewrites the PE bytes, so a signature applied after packaging would invalidate
  both SHA256SUMS.txt and the dist archive hash.  docs/RELEASE.md spells out the
  order.

  Credentials are never committed and never prompted for here:

    * -CertificateThumbprint <sha1>   a cert already in Cert:\CurrentUser\My
    * $env:PRISM_SIGN_PFX + $env:PRISM_SIGN_PFX_PASSWORD
                                      a PFX file (release automation)

  With neither, this prints exactly what is missing and exits 77.  77 is the
  project's SKIP sentinel: Tests/suites/c24-release.cmake sets
  SKIP_RETURN_CODE 77, so ctest reports "Skipped" -- never a green pass that
  hides an unsigned artifact (CONTRACTS D1).

.EXAMPLE
  pwsh -File tools\release\Sign-ReleaseArtifacts.ps1 -Path E:\build\install

.EXAMPLE
  pwsh -File tools\release\Sign-ReleaseArtifacts.ps1 -Path E:\build\install -CertificateThumbprint A1B2C3...
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [string]$CertificateThumbprint = '',
    [string]$TimestampServer = 'http://timestamp.digicert.com',
    [string]$HashAlgorithm = 'SHA256',
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say {
    param([string]$Text, [string]$Level = 'info')
    if ($Quiet -and $Level -eq 'info') { return }
    $prefix = switch ($Level) { 'warn' { 'WARN  ' } 'error' { 'FAIL  ' } default { '' } }
    Write-Host ($prefix + $Text)
}

function Get-SignableFiles {
    param([string]$Target)
    $files = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $files.Add((Resolve-Path -LiteralPath $Target).Path)
    } elseif (Test-Path -LiteralPath $Target -PathType Container) {
        foreach ($f in Get-ChildItem -LiteralPath $Target -Recurse -File -Force) {
            if ($f.Extension -in @('.exe', '.dll')) { $files.Add($f.FullName) }
        }
    }
    $sorted = $files.ToArray()
    [array]::Sort($sorted, [System.StringComparer]::Ordinal)
    return $sorted
}

function Resolve-SigningCertificate {
    param([string]$Thumbprint)

    if ($Thumbprint) {
        $clean = $Thumbprint.Replace(' ', '').ToUpperInvariant()
        foreach ($store in @('Cert:\CurrentUser\My', 'Cert:\LocalMachine\My')) {
            $found = Get-ChildItem -Path $store -ErrorAction SilentlyContinue |
                Where-Object { $_.Thumbprint -eq $clean } |
                Select-Object -First 1
            if ($found) { return $found }
        }
        return $null
    }

    if ($env:PRISM_SIGN_PFX) {
        if (-not (Test-Path -LiteralPath $env:PRISM_SIGN_PFX -PathType Leaf)) { return $null }
        $password = ''
        if ($env:PRISM_SIGN_PFX_PASSWORD) { $password = $env:PRISM_SIGN_PFX_PASSWORD }
        $secure = ConvertTo-SecureString -String $password -AsPlainText -Force
        return New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
            (Resolve-Path -LiteralPath $env:PRISM_SIGN_PFX).Path, $secure)
    }

    return $null
}

$exitCode = 1
try {
    $targets = Get-SignableFiles -Target $Path
    if ($targets.Count -eq 0) {
        Say ("SIGN: FAIL (no .exe/.dll under " + $Path + ")") 'error'
        exit 1
    }

    $cert = Resolve-SigningCertificate -Thumbprint $CertificateThumbprint
    if ($null -eq $cert) {
        Say "SIGN: SKIP (no code-signing certificate configured: pass -CertificateThumbprint <sha1> of a cert in Cert:\CurrentUser\My, or set PRISM_SIGN_PFX + PRISM_SIGN_PFX_PASSWORD)"
        Say "SIGN: SKIP reason -- a public release needs an OV/EV Authenticode certificate issued to the publisher; the pipeline never invents one and never reports an unsigned artifact as signed"
        Say ("SIGN: SKIP target(s) would be: " + [string]::Join(', ', $targets))
        exit 77
    }

    Say ("SIGN: certificate " + $cert.Subject + " (thumbprint " + $cert.Thumbprint + ")")
    $failed = 0
    foreach ($file in $targets) {
        $signature = Set-AuthenticodeSignature -FilePath $file -Certificate $cert -TimestampServer $TimestampServer -HashAlgorithm $HashAlgorithm
        $check = Get-AuthenticodeSignature -FilePath $file
        if ($check.Status -ne 'Valid') {
            Say ("SIGN: FAIL (" + $file + ": " + $check.Status + " " + $check.StatusMessage + ")") 'error'
            $failed++
        } elseif ($null -eq $check.TimeStamperCertificate) {
            # Unsigned-by-timestamp signatures expire with the certificate; that
            # is a release defect, not a warning.
            Say ("SIGN: FAIL (" + $file + ": no timestamp -- the signature dies with the certificate)") 'error'
            $failed++
        } else {
            Say ("SIGN: OK " + (Split-Path -Leaf $file) + " (" + $check.SignerCertificate.Subject + ", timestamped)")
        }
    }

    if ($failed -gt 0) {
        Say ("SIGN: FAIL (" + $failed + "/" + $targets.Count + " file(s) did not produce a valid timestamped signature)") 'error'
        $exitCode = 1
    } else {
        Say ("SIGN: PASS (" + $targets.Count + " file(s) signed and timestamped by " + $TimestampServer + ")")
        $exitCode = 0
    }
} catch {
    Say ($_.Exception.Message) 'error'
    Say 'SIGN: FAIL (unhandled error)' 'error'
    $exitCode = 1
}
exit $exitCode
