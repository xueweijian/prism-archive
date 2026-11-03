# PrismArchive -- remove the loose/registered sparse package (card C61).
#
# Idempotent on purpose: this runs from installers and from the card's selftest,
# where "the package was never registered" is a normal outcome, not an error.
#
#   .\unregister-loose.ps1                      # default package name
#   .\unregister-loose.ps1 -WhatIf              # show what would be removed
#   .\unregister-loose.ps1 -PackageName PrismArchive.ShellExt.C61Probe
#
# Exit codes: 0 removed / already absent, 1 removal failed or was not verified.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
  [string] $PackageName = 'PrismArchive.ShellExt'
)
$ErrorActionPreference = 'Stop'
$planOnly = [bool]$WhatIfPreference
$WhatIfPreference = $false
. (Join-Path $PSScriptRoot 'msix-common.ps1')

if (-not $PackageName) {
  Write-Host 'FAIL unregister reason=package-name-empty'
  Write-Host 'FIX       pass -PackageName PrismArchive.ShellExt (the manifest Identity/@Name).'
  exit 1
}

Write-Host ('PACKAGE-NAME ' + $PackageName)
$existing = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)

if ($existing.Count -eq 0) {
  Write-Host ('NO-OP nothing to remove: ' + $PackageName + ' is not registered for this user')
  exit 0
}

foreach ($package in $existing) {
  Write-Host ('FOUND ' + $package.PackageFullName)
  Write-Host ('PLAN Remove-AppxPackage -Package ' + $package.PackageFullName)
}

if ($planOnly) {
  Write-Host ('PLAN exit=0 ' + $existing.Count + ' package(s) would be removed (-WhatIf)')
  exit 0
}

$removed = 0
foreach ($package in $existing) {
  if (-not $PSCmdlet.ShouldProcess($package.PackageFullName, 'Remove-AppxPackage')) { continue }
  try {
    Remove-AppxPackage -Package $package.PackageFullName -ErrorAction Stop
    $removed++
  } catch {
    $failure = ConvertFrom-DeployError -ErrorRecord $_
    Write-Host ('FAIL unregister hresult=' + $failure.Text + ' package=' + $package.PackageFullName + ' message=' + $failure.Message)
    Write-Host ('REASON    ' + $failure.Reason)
    Write-Host ('FIX       ' + $failure.Fix)
    exit 1
  }
}

# A delete that silently did nothing is the same bug as a deploy that silently
# did nothing (SparsePackageRegistrar::Unregister checks this too).
$remaining = @(Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)
if ($remaining.Count -gt 0) {
  Write-Host ('FAIL unregister reason=unverified remaining=' + $remaining.Count)
  Write-Host ('REASON    Remove-AppxPackage returned without an error but ' + $remaining[0].PackageFullName + ' is still registered.')
  Write-Host 'FIX       close Explorer windows / the app (a loaded PrismShellExt.dll keeps the package in use), then retry.'
  exit 1
}

Write-Host ('UNREGISTERED count=' + $removed + ' package=' + $PackageName)
exit 0
