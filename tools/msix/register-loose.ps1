# Register PrismArchive by loose-file registration: no certificate, no MSIX.
#
#   .\register-loose.ps1 [-InstallDir <dir>] [-Unregister]
#
# Requires Developer Mode (see enable-dev-mode-and-register.cmd) -- Windows
# refuses unsigned packages otherwise (HRESULT 0x80073CFF).
param(
  [string] $InstallDir = "$env:LOCALAPPDATA\PrismArchive",
  [switch] $Unregister
)
$ErrorActionPreference = 'Stop'
$manifest = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'Assets\Package\AppxManifest.xml'
if (-not (Test-Path -LiteralPath $manifest)) { throw "manifest missing: $manifest" }

if ($Unregister) {
  $pkg = Get-AppxPackage -Name '*PrismArchive*'
  if ($pkg) { $pkg | Remove-AppxPackage; Write-Host "UNREGISTERED $($pkg.PackageFullName)" }
  else { Write-Host 'nothing to unregister' }
  return
}

if (-not (Test-Path -LiteralPath $InstallDir)) { throw "install dir missing: $InstallDir (run INSTALL.cmd first)" }
try {
  Add-AppxPackage -Register $manifest -ExternalLocation $InstallDir -ErrorAction Stop
} catch {
  $msg = $_.Exception.Message
  if ($msg -match '0x80073CFF') {
    throw ("Developer Mode / sideloading is off. Run enable-dev-mode-and-register.cmd " +
           "(it asks for the UAC prompt once) and try again.")
  }
  throw
}
Get-AppxPackage -Name '*PrismArchive*' | Select-Object Name, PackageFullName, InstallLocation | Format-List
