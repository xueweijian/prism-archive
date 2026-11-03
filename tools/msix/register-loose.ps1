# PrismArchive -- register the sparse package from a LOOSE folder (card C61).
#
# No certificate, no MSIX file, no elevation: this is path 1 of
# docs/MSIX-SIGNING.md. Windows accepts a loose layout only when Developer Mode
# (or the sideload policy) is on, which is why the dev-mode state is measured and
# printed instead of assumed.
#
#   .\register-loose.ps1 -BuildDir E:\build\wt-c61
#   .\register-loose.ps1 -StageDir E:\build\wt-c61\msix\stage -WhatIf
#
# TWO THINGS THAT WERE MEASURED, NOT ASSUMED (Win11 26200, non-admin, 2026-11-02):
#
#   1. Add-AppxPackage -Register <manifest> ALONE FAILS for this manifest:
#      0x80073CF9 "the package must be installed using an external location".
#      The manifest declares uap10:AllowExternalContent, so the deployment API
#      needs -ExternalLocation. This script always passes it; the selftest
#      asserts the argument is present in the call (card C61 mutation
#      DropExternalLocation).
#   2. A missing PrismShellExt.dll does NOT fail registration (count 0 -> 1) and
#      the menu is dead afterwards, because Explorer loads the COM class from the
#      package root. That is a WARN, not a FAIL: refusing here would block the
#      register-then-build loop for a defect registration cannot detect.
#
# -WhatIf is a real plan: it parses the manifest, resolves the external location,
# prints every blocker and changes nothing. It exits 0 even when a blocker is
# printed, so CI can assert the plan on a machine without Developer Mode.
#
# Exit codes: 0 registered (or planned), 1 failed with a printed REASON/FIX.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
  [string] $ManifestPath = '',
  [string] $StageDir = '',
  [string] $BuildDir = '',
  [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string] $ExternalLocation = '',
  [string] $PackageName = ''
)
$ErrorActionPreference = 'Stop'
# -WhatIf must not leak into cmdlets: importing the Appx module under it prints
# dozens of "Set Alias" lines and hides the plan. Capture the intent, clear the
# preference, branch on the captured flag.
$planOnly = [bool]$WhatIfPreference
$WhatIfPreference = $false
. (Join-Path $PSScriptRoot 'msix-common.ps1')

try {
  $resolved = Resolve-MsixManifest -ManifestPath $ManifestPath -StageDir $StageDir -BuildDir $BuildDir -RepoRoot $RepoRoot
} catch {
  Write-Host 'FAIL register reason=manifest-not-found'
  Write-Host $_.Exception.Message
  exit 1
}

$manifest = Read-MsixManifest -Path $resolved.Path
Write-Host ('MANIFEST ' + $manifest.Path + ' source=' + $resolved.Source)

if ($manifest.Identity) {
  Write-Host ('IDENTITY name=' + $manifest.Identity.Name + ' publisher=' + $manifest.Identity.Publisher +
              ' version=' + $manifest.Identity.Version + ' arch=' + $manifest.Identity.Architecture)
}

foreach ($warning in $manifest.Warnings) { Write-Host ('WARN ' + $warning) }

if ($manifest.Errors.Count -gt 0) {
  foreach ($entry in $manifest.Errors) { Write-Host ('ERROR ' + $entry) }
  Write-Host ('FAIL register reason=manifest-invalid errors=' + $manifest.Errors.Count)
  Write-Host 'FIX       re-stage with tools\msix\build-sparse.ps1 (it copies Assets\Package\AppxManifest.xml verbatim), or fix the manifest.'
  exit 1
}

if (-not $PackageName) {
  $PackageName = $manifest.Identity.Name
} elseif ($PackageName -ne $manifest.Identity.Name) {
  Write-Host ('FAIL register reason=package-name-mismatch expected=' + $manifest.Identity.Name + ' got=' + $PackageName)
  Write-Host 'FIX       the manifest is the authority: drop -PackageName, or stage the package you meant.'
  exit 1
}
Write-Host ('PACKAGE-NAME ' + $PackageName)

# Resolve the external location the way the product does: the folder that holds
# PrismArchive.exe. build-sparse.ps1 stages only identity + the shell DLL, so the
# build tree's Src\PrismWin is the usual answer.
$external = ''
if ($ExternalLocation) {
  $candidate = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExternalLocation)
  if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
    Write-Host ('FAIL register reason=external-location-missing path=' + $candidate)
    Write-Host 'FIX       pass -ExternalLocation <folder that contains PrismArchive.exe>; it must already exist.'
    exit 1
  }
  $external = (Resolve-Path -LiteralPath $candidate).Path
} elseif ($BuildDir -and (Test-Path -LiteralPath (Join-Path $BuildDir 'Src\PrismWin') -PathType Container)) {
  $external = (Resolve-Path -LiteralPath (Join-Path $BuildDir 'Src\PrismWin')).Path
} else {
  $external = $manifest.ManifestDir
}
Write-Host ('EXTERNAL-LOCATION ' + $external)

if ($manifest.Executable) {
  $companion = Join-Path $external $manifest.Executable
  if (-not (Test-Path -LiteralPath $companion -PathType Leaf)) {
    Write-Host ('WARN companion-exe-missing ' + $companion + ' (the menu verb will launch nothing; pass -ExternalLocation <folder with PrismArchive.exe>)')
  }
}

$dev = Get-MsixDevMode
$devValue = '<absent>'
if ($null -ne $dev.Value) { $devValue = [string]$dev.Value }
Write-Host ('DEV-MODE ' + $(if ($dev.On) { 'on' } else { 'off' }) + ' ' + $dev.Source + '=' + $devValue)
if ($dev.Override) {
  Write-Host ('DEV-MODE-OVERRIDE ' + $dev.Override + ' (PRISM_MSIX_DEV_MODE test seam: it changes the branch, not the registry)')
}

$existing = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
  $names = @($existing | ForEach-Object { $_.PackageFullName })
  Write-Host ('EXISTING ' + ($names -join ',') + ' (re-registering refreshes the loose layout in place)')
}

Write-Host ('PLAN Add-AppxPackage -Register ' + $manifest.Path + ' -ExternalLocation ' + $external)

if (-not $dev.On) {
  Write-Host 'BLOCKER dev-mode-off: an unsigned loose layout needs Developer Mode (Windows answers 0x80073CFF otherwise)'
  # The FIX carries the absolute path of the one-shot helper: "turn on Developer
  # Mode" is not actionable when the reader does not know which switch, and the
  # helper also finishes the registration afterwards.
  foreach ($line in (Get-MsixDevModeFix -DevMode $dev)) { Write-Host ('FIX       ' + $line) }
}

if ($planOnly) {
  Write-Host 'PLAN exit=0 nothing was registered (-WhatIf)'
  exit 0
}

if (-not $dev.On) {
  Write-Host 'FAIL register reason=dev-mode-off'
  exit 1
}

if (-not $PSCmdlet.ShouldProcess($manifest.Path, 'Add-AppxPackage -Register')) {
  Write-Host 'CANCELLED register (ShouldProcess declined)'
  exit 1
}

try {
  Add-AppxPackage -Register $manifest.Path -ExternalLocation $external -ErrorAction Stop
} catch {
  $failure = ConvertFrom-DeployError -ErrorRecord $_
  Write-Host ('FAIL register hresult=' + $failure.Text + ' message=' + $failure.Message)
  Write-Host ('REASON    ' + $failure.Reason)
  Write-Host ('FIX       ' + $failure.Fix)
  exit 1
}

# The same guard the C++ registrar has (DeployVerdict::Unverified): a deploy that
# reports success without leaving a package behind is a failure, not a success.
$after = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)
if ($after.Count -eq 0) {
  Write-Host 'FAIL register reason=unverified'
  Write-Host 'REASON    Add-AppxPackage returned without an error but the package is not registered.'
  Write-Host 'FIX       run Get-AppxPackage -Name ' + $PackageName + ' and Get-AppxPackageLog for the deployment detail.'
  exit 1
}

Write-Host ('REGISTERED name=' + $PackageName + ' family=' + $after[0].PackageFamilyName +
            ' full=' + $after[0].PackageFullName + ' external=' + $external)
exit 0
