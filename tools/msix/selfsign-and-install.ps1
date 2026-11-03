# PrismArchive -- self-signed certificate -> sign -> trust the root -> install the
# PACKAGED sparse MSIX (card C61, path 2 of docs/MSIX-SIGNING.md).
#
# This is the path that proves the real "MSIX installed" shape on your own
# machine. It is NOT the path for other people: a self-signed package only
# installs where that certificate is trusted (see path 3 in the doc).
#
#   .\selfsign-and-install.ps1 -BuildDir E:\build\wt-c61            # non-admin
#   .\selfsign-and-install.ps1 -BuildDir E:\build\wt-c61 -Machine   # needs admin
#   .\selfsign-and-install.ps1 -WhatIf                              # print the plan
#
# MEASURED (Win11 26200, non-admin, 2026-11-02):
#   * new-dev-cert.ps1 already trusts the certificate in CurrentUser\Root, which
#     is enough for Add-AppxPackage on this machine: no elevation needed.
#   * -Machine puts the root in LocalMachine\Root, which DOES need one elevated
#     run; when it is missing the deployment answers 0x800B0109.
#   * This box has no Windows SDK, so makeappx/signtool are reported as blockers
#     instead of a cryptic "not found". Install SDK 10.0.26100 to sign.
#
# The PFX password is generated when not supplied and is never printed (contract
# B7); the selftest asserts secret markers do not leak into stdout.
#
# Exit codes: 0 installed / planned, 1 failed with a printed REASON/FIX.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
  [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string] $BuildDir = '',
  [string] $ManifestPath = '',
  [string] $StageDir = '',
  [string] $ExternalLocation = '',
  [string] $PackagePath = '',
  [string] $ShellExtDll = '',
  [string] $CertDir = '',
  [string] $Subject = 'CN=PrismArchive Contributors',
  [string] $PfxPassword = '',
  [string] $PackageName = '',
  [switch] $Machine
)
$ErrorActionPreference = 'Stop'
$planOnly = [bool]$WhatIfPreference
$WhatIfPreference = $false
. (Join-Path $PSScriptRoot 'msix-common.ps1')

$admin = Test-MsixElevated
$dev = Get-MsixDevMode

if (-not $CertDir) {
  if ($BuildDir) { $CertDir = Join-Path $BuildDir 'certs' } else { $CertDir = Join-Path $RepoRoot 'out\certs' }
}
Write-Host ('SUBJECT ' + $Subject)
Write-Host ('CERT-DIR ' + $CertDir)
Write-Host ('ADMIN ' + $(if ($admin) { 'True' } else { 'False' }))
Write-Host ('DEV-MODE ' + $(if ($dev.On) { 'on' } else { 'off' }))
Write-Host 'PFX-PASSWORD <redacted>'

$secret = $PfxPassword
if (-not $secret) { $secret = [guid]::NewGuid().ToString('N') }

# --- 1. what are we installing? -------------------------------------------
$package = ''
if ($PackagePath) {
  if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    Write-Host ('FAIL install reason=package-missing path=' + $PackagePath)
    Write-Host 'FIX       drop -PackagePath to build+sign it here, or pass the .msix build-sparse.ps1 produced.'
    exit 1
  }
  $package = (Resolve-Path -LiteralPath $PackagePath).Path
}

$manifest = $null
try {
  $resolved = Resolve-MsixManifest -ManifestPath $ManifestPath -StageDir $StageDir -BuildDir $BuildDir -RepoRoot $RepoRoot
  $manifest = Read-MsixManifest -Path $resolved.Path
} catch {
  if (-not $package) {
    Write-Host 'FAIL install reason=manifest-not-found'
    Write-Host $_.Exception.Message
    exit 1
  }
}

if ($manifest) {
  Write-Host ('MANIFEST ' + $manifest.Path)
  if ($manifest.Identity) {
    Write-Host ('IDENTITY name=' + $manifest.Identity.Name + ' publisher=' + $manifest.Identity.Publisher + ' version=' + $manifest.Identity.Version)
  }
  foreach ($warning in $manifest.Warnings) { Write-Host ('WARN ' + $warning) }
  if ($manifest.Errors.Count -gt 0) {
    foreach ($entry in $manifest.Errors) { Write-Host ('ERROR ' + $entry) }
    Write-Host ('FAIL install reason=manifest-invalid errors=' + $manifest.Errors.Count)
    Write-Host 'FIX       re-stage with tools\msix\build-sparse.ps1; a signed package with an invalid manifest answers 0x80080204.'
    exit 1
  }
  if ($PackageName -and $PackageName -ne $manifest.Identity.Name) {
    Write-Host ('FAIL install reason=package-name-mismatch expected=' + $manifest.Identity.Name + ' got=' + $PackageName)
    exit 1
  }
  $PackageName = $manifest.Identity.Name
} elseif (-not $PackageName) {
  Write-Host 'FAIL install reason=package-name-unknown'
  Write-Host 'FIX       pass -PackageName together with -PackagePath (no manifest is reachable).'
  exit 1
}
Write-Host ('PACKAGE-NAME ' + $PackageName)

if (-not $ShellExtDll) {
  if ($BuildDir) { $ShellExtDll = Join-Path $BuildDir 'Src\PrismWin\PrismShellExt.dll' }
}
if (-not $package) {
  if (-not $ShellExtDll -or -not (Test-Path -LiteralPath $ShellExtDll -PathType Leaf)) {
    if ($planOnly) {
      # A plan must be printable on a machine with no build tree (CI); the
      # blocker says what a real run would refuse with.
      Write-Host ('BLOCKER shell-ext-dll-missing path=' + $ShellExtDll)
      Write-Host 'FIX       build PrismShellExt first, or pass -ShellExtDll <path> / -PackagePath <existing .msix>.'
    } else {
      Write-Host ('FAIL sign reason=shell-ext-dll-missing path=' + $ShellExtDll)
      Write-Host 'FIX       build PrismShellExt first, or pass -ShellExtDll <path> / -PackagePath <existing .msix>.'
      exit 1
    }
  } else {
    Write-Host ('SHELL-EXT ' + $ShellExtDll)
  }
}

# --- 2. plan ---------------------------------------------------------------
if (-not $Machine -and -not $admin) {
  Write-Host 'TRUST-STORE Cert:\CurrentUser\Root (no elevation; the per-user root is enough for this user)'
} elseif ($Machine -and -not $admin) {
  Write-Host 'TRUST-STORE Cert:\LocalMachine\Root'
  Write-Host 'BLOCKER admin-required: -Machine writes LocalMachine\Root, which only an elevated shell can do'
  $elevate = 'Start-Process -FilePath pwsh -Verb RunAs -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-File","' +
             (Join-Path $PSScriptRoot 'selfsign-and-install.ps1') + '","-Machine"'
  Write-Host ('FIX       ' + $elevate)
} else {
  Write-Host 'TRUST-STORE Cert:\LocalMachine\Root'
}

Write-Host ('PLAN cert subject=' + $Subject + ' store=Cert:\CurrentUser\My out=' + $CertDir + ' via=new-dev-cert.ps1')
if ($package) {
  Write-Host ('PLAN install package=' + $package + ' (already signed; skipping build)')
} else {
  $makeappx = Find-MsixSdkTool 'makeappx.exe'
  $signtool = Find-MsixSdkTool 'signtool.exe'
  if (-not $makeappx) {
    Write-Host 'BLOCKER tool-missing tool=makeappx'
    Write-Host 'FIX       install Windows SDK 10.0.26100 (makeappx.exe lives in C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64).'
  }
  if (-not $signtool) {
    Write-Host 'BLOCKER tool-missing tool=signtool'
    Write-Host 'FIX       install Windows SDK 10.0.26100 (signtool.exe lives in the same folder as makeappx.exe).'
  }
  Write-Host ('PLAN build package=' + (Join-Path ($(if ($BuildDir) { $BuildDir } else { $RepoRoot }) ) 'msix\PrismArchive.ShellExt.msix') + ' via=build-sparse.ps1')
  Write-Host ('PLAN sign pfx=' + (Join-Path $CertDir 'PrismArchive-dev.pfx') + ' signer=signtool')
}
Write-Host 'PLAN trust import=PrismArchive-dev.cer'
Write-Host ('PLAN install Add-AppxPackage -Path <signed .msix> name=' + $PackageName)

if ($planOnly) {
  if ($Machine -and -not $admin) {
    Write-Host 'PLAN exit=0 blockers printed, nothing changed (-WhatIf)'
    exit 0
  }
  Write-Host 'PLAN exit=0 nothing was created, trusted or installed (-WhatIf)'
  exit 0
}

if ($Machine -and -not $admin) {
  Write-Host 'FAIL install reason=admin-required'
  Write-Host 'FIX       re-run from an elevated shell with -Machine (see the Start-Process line above), or drop -Machine to trust the root for this user only.'
  exit 1
}

# --- 3. certificate --------------------------------------------------------
$newCert = Join-Path $PSScriptRoot 'new-dev-cert.ps1'
if (-not (Test-Path -LiteralPath $newCert -PathType Leaf)) {
  Write-Host ('FAIL cert reason=new-dev-cert-missing path=' + $newCert)
  exit 1
}

$certArgs = @{ OutDir = $CertDir; Password = $secret; Subject = $Subject }
if ($Machine) { $certArgs['Machine'] = $true }
$certOutput = @()
try {
  $certOutput = @(& $newCert @certArgs 6>&1 | ForEach-Object { [string]$_ })
} catch {
  Write-Host ('FAIL cert reason=new-dev-cert-threw message=' + $_.Exception.Message)
  Write-Host 'FIX       run tools\msix\new-dev-cert.ps1 by hand to see the failing step (Root-store import can ask for consent).'
  exit 1
}
foreach ($line in $certOutput) { Write-Host ('CERT ' + $line) }

$pfx = Join-Path $CertDir 'PrismArchive-dev.pfx'
$cer = Join-Path $CertDir 'PrismArchive-dev.cer'
$missing = @($pfx, $cer) | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
if ($missing.Count -gt 0) {
  Write-Host ('FAIL cert reason=export-missing files=' + ($missing -join ','))
  Write-Host 'FIX       the certificate was created but not exported; check the CERT lines above (a locked .pfx also fails here).'
  exit 1
}
Write-Host ('CERT-EXPORTED pfx=' + $pfx + ' cer=' + $cer)

$thumbprint = ''
foreach ($line in $certOutput) {
  $match = [regex]::Match($line, 'certificate ([0-9A-Fa-f]{40})')
  if ($match.Success) { $thumbprint = $match.Groups[1].Value.ToUpperInvariant() }
}
if (-not $thumbprint) {
  $found = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey } |
           Sort-Object NotAfter -Descending | Select-Object -First 1
  if ($found) { $thumbprint = $found.Thumbprint }
}
if (-not $thumbprint) {
  Write-Host 'FAIL cert reason=thumbprint-unknown'
  Write-Host 'FIX       Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq CN=PrismArchive Contributors'
  exit 1
}
Write-Host ('THUMBPRINT ' + $thumbprint)

# --- 4. sign ---------------------------------------------------------------
if (-not $package) {
  $outDir = Join-Path ($(if ($BuildDir) { $BuildDir } else { $RepoRoot })) 'msix'
  $build = Join-Path $PSScriptRoot 'build-sparse.ps1'
  $buildArgs = @{
    RepoRoot    = $RepoRoot
    ShellExtDll = $ShellExtDll
    OutDir      = $outDir
    Pfx         = $pfx
    PfxPassword = $secret
  }
  try {
    $buildOutput = @(& $build @buildArgs 6>&1 | ForEach-Object { [string]$_ })
  } catch {
    Write-Host ('FAIL sign reason=build-sparse-threw message=' + $_.Exception.Message)
    Write-Host 'FIX       run tools\msix\build-sparse.ps1 by hand with the same -Pfx; the makeappx/signtool exit code is in its error.'
    exit 1
  }
  foreach ($line in $buildOutput) { Write-Host ('SIGN ' + $line) }
  $package = @($buildOutput | Where-Object { $_ -like '*.msix' }) | Select-Object -Last 1
  if (-not $package -or -not (Test-Path -LiteralPath $package -PathType Leaf)) {
    Write-Host 'FAIL sign reason=package-not-produced'
    Write-Host 'FIX       build-sparse.ps1 prints PACKAGE <path> on success; the SIGN lines above show where it stopped.'
    exit 1
  }
  $package = (Resolve-Path -LiteralPath $package).Path
}
$size = (Get-Item -LiteralPath $package).Length
if ($size -le 0) {
  Write-Host ('FAIL sign reason=package-empty path=' + $package)
  exit 1
}
Write-Host ('SIGNED package=' + $package + ' bytes=' + $size)

# --- 5. trust the root -----------------------------------------------------
$rootStore = 'Cert:\CurrentUser\Root'
if ($Machine) { $rootStore = 'Cert:\LocalMachine\Root' }
$trusted = @(Get-ChildItem $rootStore -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $thumbprint })
if ($trusted.Count -eq 0) {
  try {
    Import-Certificate -FilePath $cer -CertStoreLocation $rootStore -ErrorAction Stop | Out-Null
    Write-Host ('TRUSTED ' + $rootStore + ' thumbprint=' + $thumbprint)
  } catch {
    Write-Host ('FAIL trust reason=import-refused store=' + $rootStore + ' message=' + $_.Exception.Message)
    Write-Host 'FIX       import it by hand: Import-Certificate -FilePath ' + $cer + ' -CertStoreLocation ' + $rootStore
    Write-Host '          (a self-signed package certificate is its own root: without the root, deployment answers 0x800B0109).'
    exit 1
  }
} else {
  Write-Host ('TRUSTED ' + $rootStore + ' thumbprint=' + $thumbprint + ' (already present)')
}

# --- 6. install ------------------------------------------------------------
if (-not $PSCmdlet.ShouldProcess($package, 'Add-AppxPackage -Path')) {
  Write-Host 'CANCELLED install (ShouldProcess declined)'
  exit 1
}
try {
  Add-AppxPackage -Path $package -ErrorAction Stop
} catch {
  $failure = ConvertFrom-DeployError -ErrorRecord $_
  Write-Host ('FAIL install hresult=' + $failure.Text + ' message=' + $failure.Message)
  Write-Host ('REASON    ' + $failure.Reason)
  Write-Host ('FIX       ' + $failure.Fix)
  if ($rootStore -eq 'Cert:\CurrentUser\Root') {
    Write-Host 'HINT      if the HRESULT is 0x800B0109, trust the root for every account that installs it, or run this script with -Machine from an elevated shell.'
  }
  exit 1
}

$after = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)
if ($after.Count -eq 0) {
  Write-Host 'FAIL install reason=unverified'
  Write-Host 'REASON    Add-AppxPackage returned without an error but the package is not registered.'
  Write-Host ('FIX       Get-AppxPackage -Name ' + $PackageName + '; Get-AppxPackageLog for the deployment detail.')
  exit 1
}

Write-Host ('INSTALLED name=' + $PackageName + ' family=' + $after[0].PackageFamilyName + ' full=' + $after[0].PackageFullName)
Write-Host ('UNINSTALL Remove-AppxPackage -Package ' + $after[0].PackageFullName)
exit 0
