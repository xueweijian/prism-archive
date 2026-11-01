# PrismArchive -- end-to-end sparse-package round trip (card C4).
#
# Builds the package, signs it with a dev certificate, then runs the C++
# registrar test which performs the REAL deploy / verify / unregister cycle
# through PackageManager.AddPackageByUriAsync.
#
#   .\test-sparse-e2e.ps1 -BuildDir E:\build\wt-c4 -CertPassword 'prism-dev'
#
# Exit codes mirror the test: 0 pass, 77 skipped (capability missing), 1 failed.
[CmdletBinding()]
param(
  [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [Parameter(Mandatory = $true)] [string] $BuildDir,
  [string] $CertDir = '',
  [string] $CertPassword = 'prism-dev',
  [switch] $SkipCert
)
$ErrorActionPreference = 'Stop'

if (-not $CertDir) { $CertDir = Join-Path $BuildDir 'certs' }
$outDir = Join-Path $BuildDir 'msix'

$shellExt = Join-Path $BuildDir 'Src\PrismWin\PrismShellExt.dll'
$testExe = Join-Path $BuildDir 'Tests\win_sparse_registrar_test.exe'
$external = Join-Path $BuildDir 'Src\PrismWin'
if (-not (Test-Path $testExe)) { throw "test binary missing: $testExe (build first)" }

$pfx = Join-Path $CertDir 'PrismArchive-dev.pfx'
if (-not $SkipCert) {
  & (Join-Path $PSScriptRoot 'new-dev-cert.ps1') -OutDir $CertDir -Password $CertPassword | Write-Host
}

$package = & (Join-Path $PSScriptRoot 'build-sparse.ps1') -RepoRoot $RepoRoot -ShellExtDll $shellExt -OutDir $outDir -Pfx $pfx -PfxPassword $CertPassword | Select-Object -Last 1

$env:PRISM_SPARSE_MSIX = $package
$env:PRISM_SPARSE_EXTERNAL = $external
Write-Host "RUN $testExe"
& $testExe
$code = $LASTEXITCODE
Write-Host "EXIT  $code"
exit $code
