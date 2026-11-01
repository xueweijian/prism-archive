#requires -Version 5.1
<#
.SYNOPSIS
  Compile Installer/prism.iss into a release setup executable.

.DESCRIPTION
  Wraps ISCC.exe so the build is one command and the result is checked rather than
  assumed: it refuses to report success without the setup file on disk, and prints
  its size and SHA-256 (release evidence, not a log line).

  ISCC comes from Inno Setup 6, installed outside the C: drive on purpose:
      winget install --id JRSoftware.InnoSetup --exact --silent `
          --location E:\DevTools\InnoSetup
#>
[CmdletBinding()]
param(
    [string]$PayloadDir = '',
    [string]$Iscc = '',
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$iss = Join-Path $PSScriptRoot 'prism.iss'
if (-not (Test-Path -LiteralPath $iss)) { throw ('missing script: ' + $iss) }

if (-not $PayloadDir) {
    foreach ($candidate in @($env:PRISM_BUILD_DIR, 'E:\build\wt-c3\Src\PrismWin',
                             (Join-Path $repo 'build\Src\PrismWin'))) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'PrismArchive.exe'))) {
            $PayloadDir = $candidate
            break
        }
    }
}
if (-not $PayloadDir -or -not (Test-Path -LiteralPath (Join-Path $PayloadDir 'PrismArchive.exe'))) {
    throw ('no payload with PrismArchive.exe; build first or pass -PayloadDir (tried: ' + [string]$PayloadDir + ')')
}

if (-not $Iscc) {
    foreach ($candidate in @($env:PRISM_ISCC, 'E:\DevTools\InnoSetup\ISCC.exe',
                             'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
                             'C:\Program Files\Inno Setup 6\ISCC.exe')) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { $Iscc = $candidate; break }
    }
}
if (-not $Iscc) {
    throw 'ISCC.exe not found. Install Inno Setup 6 (winget install --id JRSoftware.InnoSetup --location E:\DevTools\InnoSetup) or pass -Iscc.'
}

$output = Join-Path $PSScriptRoot 'dist'
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }

Write-Host ('payload : ' + $PayloadDir)
Write-Host ('iscc    : ' + $Iscc)
$isccArgs = @($iss, ('/DPayloadDir=' + $PayloadDir), ('/O' + $output))
if ($Quiet) { $isccArgs += '/Qp' }
$proc = Start-Process -FilePath $Iscc -ArgumentList $isccArgs -Wait -PassThru -NoNewWindow
if ($proc.ExitCode -ne 0) { throw ('ISCC failed with exit code ' + $proc.ExitCode) }

$setup = Get-ChildItem -LiteralPath $output -Filter '*.exe' | Select-Object -First 1
if (-not $setup) { throw ('ISCC reported success but produced no setup in ' + $output) }

$hash = (Get-FileHash -LiteralPath $setup.FullName -Algorithm SHA256).Hash
Write-Host ''
Write-Host ('SETUP: ' + $setup.FullName)
Write-Host ('SIZE:  ' + [math]::Round($setup.Length / 1MB, 2) + ' MB')
Write-Host ('SHA256: ' + $hash)
Write-Host 'NOTE: unsigned. Sign before publishing (signtool + EV certificate).'
