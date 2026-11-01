# PrismArchive -- self-signed development certificate for the sparse package.
#
# Card C4. The publisher in Assets\Package\AppxManifest.xml is
# "CN=PrismArchive Contributors"; MSIX refuses a package whose signing subject
# differs from Identity/@Publisher (CERT_E_WRONG_PRINCIPAL, 0x800B010F).
#
# Per-user trust needs no elevation -- this machine has no admin rights and
# still registers the package this way:
#   .\new-dev-cert.ps1 -OutDir E:\build\c4-certs -Password 'prism-dev'
#
# It exports PrismArchive-dev.pfx (private key, never commit it) and
# PrismArchive-dev.cer, then imports the .cer into CurrentUser\TrustedPeople.
# Microsoft documents exactly this per-user flow for sparse packages; without it
# registration fails with CERT_E_UNTRUSTEDROOT (0x800B0109).
[CmdletBinding()]
param(
  [string] $Subject = 'CN=PrismArchive Contributors',
  [Parameter(Mandatory = $true)] [string] $OutDir,
  [Parameter(Mandatory = $true)] [string] $Password,
  [switch] $Machine
)
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$pfx = Join-Path $OutDir 'PrismArchive-dev.pfx'
$cer = Join-Path $OutDir 'PrismArchive-dev.cer'

$existing = Get-ChildItem Cert:\CurrentUser\My |
  Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey } |
  Sort-Object NotAfter -Descending |
  Select-Object -First 1

if ($existing) {
  $cert = $existing
  Write-Host "REUSE certificate $($cert.Thumbprint)"
} else {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -FriendlyName 'PrismArchive sparse package (dev)' -KeyUsage DigitalSignature -KeyLength 2048 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3) -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
  Write-Host "CREATE certificate $($cert.Thumbprint)"
}

$secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secure | Out-Null
Export-Certificate -Cert $cert -FilePath $cer | Out-Null

$store = 'Cert:\CurrentUser\TrustedPeople'
if ($Machine) { $store = 'Cert:\LocalMachine\TrustedPeople' }
Import-Certificate -FilePath $cer -CertStoreLocation $store | Out-Null

Write-Host "PFX     $pfx"
Write-Host "CER     $cer"
Write-Host "TRUSTED $store"
Write-Host "SUBJECT $($cert.Subject)"
