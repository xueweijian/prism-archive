<#
.SYNOPSIS
  Is the white window the application's bug, or does this machine drop Direct2D
  HWND presents? Builds tools\d2d_present_probe.cpp with the installed MSVC
  toolchain, runs it and prints the one-line verdict.

.DESCRIPTION
  Card C116: a user saw a caption + menu bar over a blank client area while the
  accessibility tree listed the whole model. The app's D2D frame was complete
  and EndDraw() returned S_OK -- the machine simply never presented it. This
  probe reproduces that experiment with NO PrismArchive code:

    CreateHwndRenderTarget -> Clear(RED) -> EndDraw() x3
    GetPixel(window DC)          -> d2d_pixel
    FillRect(GREEN) with GDI     -> gdi_pixel

  Last line of output:
    D2D_PRESENT {"enddraw":"0x00000000","d2d_pixel":"ff0000","gdi_pixel":"00ff00","verdict":"D2D_PRESENT_OK"}

  verdict:
    D2D_PRESENT_OK        both paths work -> a white window is the app's bug
    D2D_PRESENT_BROKEN    GDI shows, D2D does not -> the machine drops D2D
    D2D_AND_GDI_BROKEN    neither shows -> display/session problem
    D2D_PROBE_FAILED      no render target at all

  CONTRACTS D1: this is a diagnostic, not a test -- it depends on the machine's
  display stack, so it is deliberately NOT registered with CTest.

.EXAMPLE
  pwsh -NoProfile -ExecutionPolicy Bypass -File tools\d2d_present_probe.ps1
#>
[CmdletBinding()]
param(
    # Keep the built exe (default: remove it from %TEMP% afterwards).
    [switch]$KeepExe
)

$ErrorActionPreference = 'Stop'

if (-not $IsWindows) {
    Write-Error 'd2d_present_probe is a Windows diagnostic (Direct2D is a Windows API).'
    exit 2
}

$source = Join-Path $PSScriptRoot 'd2d_present_probe.cpp'
if (-not (Test-Path -LiteralPath $source)) {
    Write-Error "probe source missing: $source"
    exit 2
}

# --- the C++ toolchain -------------------------------------------------------
# Same discovery order as local_gates.ps1: vswhere first, then the Build Tools
# path this project's env script uses. No PATH required from the caller.
function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $inst = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($inst) {
            $candidate = Join-Path $inst.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    $fallback = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) {
    Write-Error 'vcvars64.bat not found: install Visual Studio 2022 (or Build Tools) with the C++ workload.'
    exit 2
}

$envLines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

# --- build + run -------------------------------------------------------------
$outDir = Join-Path ([System.IO.Path]::GetTempPath()) 'prism-d2d-probe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'd2d_present_probe.exe'

Push-Location $outDir
try {
    & cl.exe /nologo /EHsc /std:c++20 /W4 /DUNICODE /D_UNICODE $source `
        "/Fe:$exe" "/Fo:$outDir\" `
        /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib d2d1.lib dwmapi.lib dxgi.lib
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        Write-Error 'the probe did not compile (see cl output above)'
        exit 3
    }
    & $exe
}
finally {
    Pop-Location
    if (-not $KeepExe) {
        Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
    }
}
