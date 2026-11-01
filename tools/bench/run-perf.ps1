# PrismArchive -- the blueprint ch. 7.2 benchmark lane (card c7).
#
# Runs the three Windows benchmarks that answer the performance matrix cells,
# collects their machine-readable BENCH_* lines, and prints a markdown block
# ready to paste into docs/PERF.md. Nothing here is a merge gate: the numbers are
# REPORT-ONLY (a shared 4-core box cannot gate a stopwatch), which is why the
# benchmarks themselves only assert SHAPE (calibrated timer, peak-memory
# invariant, zero-%TEMP%, virtualization bound) and print the digits.
#
# Usage:
#   tools/bench/run-perf.ps1                 # build + run all three
#   tools/bench/run-perf.ps1 -SkipBuild      # run what is already built
#   tools/bench/run-perf.ps1 -Only startup   # one lane
#   tools/bench/run-perf.ps1 -ReportPath perf.md
#
# Requirements: a Windows host with 7z.dll/7z.exe (PRISM_7Z_DLL) and the build
# tree from README (cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release).
# The close-latency cell needs the real PrismArchive.exe beside the tests; when
# it is missing that ONE cell reports "not measured + reason" (never a skip).
param(
  [switch]$SkipBuild,
  [ValidateSet('all', 'startup', 'memory', 'scale')]
  [string]$Only = 'all',
  [string]$BuildDir = 'E:\build\wt-c7',
  [string]$EnvScript = 'E:\wg\env.ps1',
  [string]$Card = 'c7',
  [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Push-Location $repoRoot
try {
  if (Test-Path $EnvScript) {
    . $EnvScript -Card $Card
  } else {
    Write-Host "[perf] no env script at $EnvScript -- assuming cl.exe/7z are already on PATH" -ForegroundColor DarkGray
  }
  if (-not $env:PRISM_7Z_DLL) {
    foreach ($c in @('C:\Program Files\7-Zip\7z.dll', 'C:\Program Files (x86)\7-Zip\7z.dll')) {
      if (Test-Path $c) { $env:PRISM_7Z_DLL = $c; break }
    }
  }
  if (-not $env:PRISM_7Z_DLL -or -not (Test-Path $env:PRISM_7Z_DLL)) {
    throw '7z.dll not found: set PRISM_7Z_DLL (the benchmarks FAIL rather than skip)'
  }

  $targets = @{
    startup = @{ Exe = 'win_bench_startup'; Marker = 'ALL STARTUP BENCH CHECKS PASSED' }
    memory  = @{ Exe = 'win_bench_memory';  Marker = 'ALL MEMORY BENCH CHECKS PASSED' }
    scale   = @{ Exe = 'win_bench_scale';   Marker = 'ALL SCALE BENCH CHECKS PASSED' }
  }
  $selected = if ($Only -eq 'all') { @('startup', 'memory', 'scale') } else { @($Only) }

  if (-not $SkipBuild) {
    if (Get-Command Enter-BuildLock -ErrorAction SilentlyContinue) { Enter-BuildLock }
    try {
      Write-Host '== configure ==' -ForegroundColor Cyan
      cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
      if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
      Write-Host '== build ==' -ForegroundColor Cyan
      $exeArgs = @()
      foreach ($k in $selected) { $exeArgs += $targets[$k].Exe }
      cmake --build $BuildDir --parallel 2 --target @exeArgs
      if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    } finally {
      if (Get-Command Exit-BuildLock -ErrorAction SilentlyContinue) { Exit-BuildLock }
    }
  }

  $rows = @()
  $failed = @()
  foreach ($k in $selected) {
    $spec = $targets[$k]
    $exe = Join-Path $BuildDir ("Tests\{0}.exe" -f $spec.Exe)
    if (-not (Test-Path $exe)) { throw "missing $exe (build it first)" }
    Write-Host ''
    Write-Host ('== {0} ==' -f $k) -ForegroundColor Cyan
    $output = & $exe 2>&1
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    $bench = $output | Where-Object { $_ -match '^BENCH_' } | Select-Object -Last 1
    $marker = @($output | Where-Object { $_ -match [regex]::Escape($spec.Marker) }).Count -gt 0
    if ($code -ne 0 -or -not $marker -or -not $bench) {
      $failed += ('{0}: exit={1} marker={2} bench_line={3}' -f $k, $code, $marker, [bool]$bench)
      continue
    }
    $json = $bench.Substring($bench.IndexOf(' ')).Trim() | ConvertFrom-Json
    $rows += [pscustomobject]@{ Lane = $k; Line = $bench.Trim(); Data = $json }
  }

  Write-Host ''
  Write-Host '== machine-readable lines ==' -ForegroundColor Cyan
  foreach ($row in $rows) { Write-Host ('- {0}: {1}' -f $row.Lane, $row.Line) }
  if ($ReportPath) {
    $md = @('# PrismArchive perf run', '', ('Generated: {0}' -f (Get-Date -Format s)), '')
    foreach ($row in $rows) {
      $md += ('## {0}' -f $row.Lane)
      $md += '    ' + $row.Line
      $md += ''
    }
    $md -join [Environment]::NewLine | Set-Content -Path $ReportPath -Encoding utf8
    Write-Host "[perf] report written to $ReportPath" -ForegroundColor DarkGray
  }
  if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host 'PERF LANE: FAILED' -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
  }
  Write-Host ''
  Write-Host 'PERF LANE: PASS (BENCH lines present, shape checks green)' -ForegroundColor Green
} finally {
  Pop-Location
}
