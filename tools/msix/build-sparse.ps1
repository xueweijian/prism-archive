# PrismArchive -- build (and optionally sign) the sparse MSIX identity package.
#
# Card C4. The package carries identity + the Win11 context-menu registration
# only: uap10:AllowExternalContent means the manifest's Application/@Executable
# resolves against the install directory handed to
# AddPackageOptions.ExternalLocationUri at registration time. That is why
# makeappx runs with /nv -- the executable referenced by the manifest is
# deliberately NOT inside the package.
#
#   .\build-sparse.ps1 -ShellExtDll E:\build\wt-c4\Src\PrismWin\PrismShellExt.dll
#
# Registration itself is done by the real API (SparsePackageRegistrar.cpp) and
# exercised by Tests\win\test_sparse_registrar.cpp via .\test-sparse-e2e.ps1.
[CmdletBinding()]
param(
  [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [Parameter(Mandatory = $true)] [string] $ShellExtDll,
  [string] $OutDir = '',
  [string] $Pfx = '',
  [string] $PfxPassword = '',
  [switch] $NoSign
)
$ErrorActionPreference = 'Stop'

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'out\msix' }

function Find-SdkTool([string] $name) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $kits = @(
    'C:\Program Files (x86)\Windows Kits\10\bin',
    'C:\Program Files\Windows Kits\10\bin'
  ) | Where-Object { Test-Path $_ }
  foreach ($root in $kits) {
    $dirs = Get-ChildItem $root -Directory | Sort-Object Name -Descending
    foreach ($dir in $dirs) {
      $candidate = Join-Path $dir.FullName ('x64\' + $name)
      if (Test-Path $candidate) { return $candidate }
    }
  }
  throw "$name not found (Windows SDK 10.0.26100 is expected to provide it)"
}

$manifest = Join-Path $RepoRoot 'Assets\Package\AppxManifest.xml'
if (-not (Test-Path $manifest)) { throw "manifest missing: $manifest" }
if (-not (Test-Path $ShellExtDll)) { throw "PrismShellExt.dll missing: $ShellExtDll" }

$stage = Join-Path $OutDir 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# The package tree mirrors Assets/Package exactly (manifest + Assets\*.png).
Copy-Item $manifest (Join-Path $stage 'AppxManifest.xml') -Force
Copy-Item (Join-Path $RepoRoot 'Assets\Package\Assets') (Join-Path $stage 'Assets') -Recurse -Force
# The shell DLL DOES live in the package: Explorer loads com:Class/@Path from the
# package root, which is the only location a sparse manifest can resolve.
Copy-Item $ShellExtDll (Join-Path $stage 'PrismShellExt.dll') -Force

$package = Join-Path $OutDir 'PrismArchive.ShellExt.msix'
$makeappx = Find-SdkTool 'makeappx.exe'
& $makeappx pack /o /nv /d $stage /p $package
if ($LASTEXITCODE -ne 0) { throw "makeappx failed with $LASTEXITCODE" }

if (-not $NoSign) {
  if (-not $Pfx) { throw 'pass -Pfx <file.pfx> (see new-dev-cert.ps1) or -NoSign' }
  $signtool = Find-SdkTool 'signtool.exe'
  & $signtool sign /fd SHA256 /td SHA256 /f $Pfx /p $PfxPassword $package
  if ($LASTEXITCODE -ne 0) { throw "signtool failed with $LASTEXITCODE" }
}

$hash = (Get-FileHash $package -Algorithm SHA256).Hash
Write-Host "PACKAGE $package"
Write-Host "SHA256  $hash"
Write-Output $package
