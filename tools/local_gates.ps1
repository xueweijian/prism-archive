#Requires -Version 7.0
<#
.SYNOPSIS
    One command = PrismArchive's whole local acceptance line.

.DESCRIPTION
    Runs every gate this project owns and prints ONE verdict line at the end:

        LOCAL GATES: PASS  (7/7 steps, 94.2s, preset=release-msvc)

    Steps, in order. All of them are the same gates CI runs -- a local gate that
    CI does not run, or a CI gate no one runs locally, is exactly how this project
    lost days to "the script was broken and nobody called it" (CONTRACTS D1):

        presets          cmake --list-presets           (CMakePresets.json parses)
        configure        cmake --preset <preset>        (CMakePresets.json)
        build            cmake --build <dir> --parallel N
        ctest            ctest --output-on-failure --no-tests=error
        preflight        python tools/preflight.py      (contracts B1-B9, ARCH, D1)
        manifest         python tools/check_manifest.py (MSIX sparse manifest)
        headers          sh tools/check_headers.sh      (B2/B3; CXX-overridable)
        windows_tu       sh tools/check_windows_tu.sh   (MSVC-facing error class)

    The two shell gates are POSIX shell scripts. On Windows they are driven
    through WSL2 -- this replaces the ad-hoc E:\wg\posix-gates.sh prototype: same
    idea, but the working directory is passed with "wsl --cd", CXX is forwarded
    explicitly, and a missing shell shows up as SKIP in the verdict instead of
    vanishing. On Linux/macOS they run natively.

    Honesty rules baked in:
      * SKIP is never silent -- it is printed and it appears in the verdict line;
      * -Strict turns any SKIP into a FAIL (use it where a skip is unacceptable);
      * a missing compiler/python/WSL is a loud failure, never a quiet pass;
      * each step's full output is tee'd to <build>/local-gates/<step>.log.

    On Windows the shell gates run against a throwaway ext4 mirror of the
    worktree (Src + the gate scripts + tools/win_stub, tar'd byte-exact and
    rebuilt from scratch every run). 9p per-file opens made the header gate take
    205s there versus 25s on ext4 -- a gate that costs four minutes gets skipped,
    and a skipped gate is the failure mode this whole card exists to remove.
    -NoShellMirror reads the worktree in place if you want to see both paths.

.PARAMETER Worktree
    Repo root. Defaults to the parent of this script's directory.

.PARAMETER BuildDir
    Binary dir. Defaults to <Worktree>/build/<preset> (gitignored).

.PARAMETER Preset
    CMakePresets.json configure preset. "auto" = release-msvc on Windows,
    release-gcc elsewhere.

.PARAMETER Only
    Comma-separated subset, e.g. -Only preflight,manifest -- or
    -Only headers,windows_tu for the old posix-gates.sh behaviour.

.PARAMETER ShellRunner
    auto | wsl | native | none. "none" is for debugging only: it produces SKIPs.

.PARAMETER Strict
    Any SKIP becomes a FAIL.

.EXAMPLE
    tools/local_gates.ps1
    configure -> build -> ctest -> preflight -> manifest -> shell gates, one line.

.EXAMPLE
    tools/local_gates.ps1 -Preset asan-msvc -BuildDir E:\build\asan-c2 -SkipShell
    The Windows ASan lane (CONTRACTS D14 documents the 7z.dll SetCodecs noise).

.EXAMPLE
    tools/local_gates.ps1 -Only headers,windows_tu
    Just the two POSIX gates, from Windows, through WSL.
#>
[CmdletBinding()]
param(
    [string]$Worktree = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$BuildDir,
    [string]$Preset = 'auto',
    [string]$Only,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$SkipShell,
    [switch]$SkipPreflight,
    [switch]$SkipManifest,
    [ValidateSet('auto', 'wsl', 'native', 'none')]
    [string]$ShellRunner = 'auto',
    # Read the worktree in place from WSL instead of an ext4 mirror (~8x slower).
    [switch]$NoShellMirror,
    [switch]$KeepShellStage,
    [string]$Distro = $env:PRISM_WSL_DISTRO,
    [string]$Cxx,
    [string]$Python,
    [string]$SevenZipDll = $env:PRISM_7Z_DLL,
    [int]$Parallel = 2,
    [switch]$NoLock,
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'
$nl = [Environment]::NewLine

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
$script:Results = [System.Collections.Generic.List[object]]::new()
$script:StartedAt = Get-Date

function Write-Info([string]$text) { Write-Host $text }
function Write-Section([string]$text) { Write-Host ''; Write-Host "== $text ==" -ForegroundColor Cyan }
function Write-Warn([string]$text) { Write-Host "WARNING: $text" -ForegroundColor Yellow }

function Add-Result {
    param(
        [string]$Name,
        [ValidateSet('PASS', 'FAIL', 'SKIP')][string]$Status,
        [double]$Seconds = 0,
        [string]$Detail = ''
    )
    $script:Results.Add([pscustomobject]@{
            Name    = $Name
            Status  = $Status
            Seconds = [math]::Round($Seconds, 1)
            Detail  = $Detail
        })
}

# ---------------------------------------------------------------------------
# Environment discovery -- must work on a box where nobody imported env.ps1, and
# must NOT quietly use a different toolchain than CI's.
# ---------------------------------------------------------------------------
function Join-Maybe([string]$root, [string]$child) {
    # $env:ProgramFiles / $env:ProgramFiles(x86) are EMPTY on Linux, and
    # "Join-Path $null ..." throws "Cannot bind argument to parameter 'Path'
    # because it is null" -- which is exactly how the first CI run of this
    # script died on the ubuntu runner, before any gate ran. Every machine-path
    # lookup goes through here.
    if ([string]::IsNullOrEmpty($root)) { return $null }
    return (Join-Path $root $child)
}

function Resolve-VcVars {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return $null }
    if ($env:PRISM_VCVARS -and (Test-Path $env:PRISM_VCVARS)) { return $env:PRISM_VCVARS }

    $candidates = @()
    $vswhere = Join-Maybe ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if ($vswhere -and (Test-Path $vswhere)) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($install) { $candidates += $install.Trim() }
    }
    foreach ($edition in 'BuildTools', 'Community', 'Professional', 'Enterprise') {
        $candidates += (Join-Maybe $env:ProgramFiles "Microsoft Visual Studio\2022\$edition")
        $candidates += (Join-Maybe ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\$edition")
    }
    foreach ($root in $candidates) {
        if (-not $root) { continue }
        $bat = Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path $bat) { return $bat }
    }
    return $null
}

function Import-MsvcEnvironment {
    # CMake cannot find an MSVC instance by itself for the Ninja generator;
    # cl.exe has to be on PATH. That is what CI does with ilammy/msvc-dev-cmd.
    $vcvars = Resolve-VcVars
    $vsRoot = $null
    if ($vcvars) {
        $vsRoot = (Resolve-Path (Join-Path (Split-Path $vcvars -Parent) '..\..\..')).Path
        $cmdLine = '"' + $vcvars + '" >nul 2>&1 && set'
        cmd /c $cmdLine | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
        }
    }

    # Ninja ships inside the VS install; use it when the box has no standalone
    # copy (the CI runner has the same shape).
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and $vsRoot) {
        $d = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
        if (Test-Path (Join-Path $d 'ninja.exe')) {
            $env:PATH = "$d;$env:PATH"
            Write-Info "[env] ninja from $d"
        }
    }
    if ($vcvars) { Write-Info "[env] vcvars: $vcvars" }
}

function Resolve-Python {
    # The WindowsApps python.exe stub prints a banner and fails; validating each
    # candidate's actual version is what keeps this from "finding" a fake python.
    $candidates = [System.Collections.Generic.List[object]]::new()
    if ($Python) { $candidates.Add([pscustomobject]@{ Exe = $Python; Prefix = @() }) }
    if ($env:PRISM_PYTHON) { $candidates.Add([pscustomobject]@{ Exe = $env:PRISM_PYTHON; Prefix = @() }) }
    $candidates.Add([pscustomobject]@{ Exe = 'python'; Prefix = @() })
    $candidates.Add([pscustomobject]@{ Exe = 'python3'; Prefix = @() })
    $candidates.Add([pscustomobject]@{ Exe = 'py'; Prefix = @('-3') })

    foreach ($c in $candidates) {
        if (-not (Get-Command $c.Exe -ErrorAction SilentlyContinue)) { continue }
        try {
            $probe = & $c.Exe @($c.Prefix) -c 'import sys; print(sys.version_info[0], sys.version_info[1])' 2>$null
        }
        catch { continue }
        if ($LASTEXITCODE -ne 0 -or -not $probe) { continue }
        $parts = "$probe".Trim() -split '\s+'
        if ($parts.Count -ge 2 -and [int]$parts[0] -ge 3 -and [int]$parts[1] -ge 8) {
            return [pscustomobject]@{
                Exe     = (Get-Command $c.Exe).Source
                Prefix  = $c.Prefix
                Version = "$($parts[0]).$($parts[1])"
            }
        }
    }
    return $null
}

function Resolve-SevenZip {
    # Integration suites SKIP without a real 7z.dll, and a silent SKIP is not a
    # pass (CONTRACTS D1) -- so find one and say where it came from.
    if ($SevenZipDll -and (Test-Path $SevenZipDll)) { return $SevenZipDll }
    $candidates = @(
        (Join-Maybe $env:ProgramFiles '7-Zip\7z.dll'),
        (Join-Maybe ${env:ProgramFiles(x86)} '7-Zip\7z.dll'),
        (Join-Maybe $env:LOCALAPPDATA 'Programs\7-Zip\7z.dll')
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Resolve-ShellRunner {
    if ($ShellRunner -ne 'auto') { return $ShellRunner }
    if ($IsWindows) {
        if (Get-Command wsl.exe -ErrorAction SilentlyContinue) { return 'wsl' }
        return 'none'
    }
    return 'native'
}

# ---------------------------------------------------------------------------
# Build lock -- serialize with the other parallel cards. If env.ps1 was
# dot-sourced its functions win, so both mechanisms share one slot directory.
# ---------------------------------------------------------------------------
function Enter-GateLock {
    if ($NoLock) { return }
    if (Get-Command Enter-BuildLock -ErrorAction SilentlyContinue) {
        Enter-BuildLock
        $script:UsedAmbientLock = $true
        return
    }
    $root = $env:PRISM_LOCK_ROOT
    if (-not $root) {
        $parent = Split-Path -Parent $BuildDir
        if (-not $parent) { $parent = (Get-Location).Path }
        $root = Join-Path $parent 'locks'
    }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $deadline = (Get-Date).AddMinutes(45)
    while ((Get-Date) -lt $deadline) {
        foreach ($i in 1..3) {
            $slot = Join-Path $root "slot$i"
            if ((Test-Path $slot) -and ((Get-Item $slot).LastWriteTime -lt (Get-Date).AddMinutes(-40))) {
                Remove-Item -Recurse -Force $slot -ErrorAction SilentlyContinue
            }
            try {
                New-Item -ItemType Directory -Path $slot -ErrorAction Stop | Out-Null
                Set-Content -Path (Join-Path $slot 'owner.txt') -Value "local_gates pid=$PID"
                $script:LockSlot = $slot
                Write-Info "[lock] $slot acquired"
                return
            }
            catch { }
        }
        Start-Sleep -Seconds 5
    }
    throw "build lock timeout: $root (another card is compiling)"
}

function Exit-GateLock {
    if ($script:UsedAmbientLock) {
        Exit-BuildLock
        $script:UsedAmbientLock = $false
        return
    }
    if ($script:LockSlot) {
        Remove-Item -Recurse -Force $script:LockSlot -ErrorAction SilentlyContinue
        Write-Info "[lock] released"
        $script:LockSlot = $null
    }
}

# ---------------------------------------------------------------------------
# Step driver
# ---------------------------------------------------------------------------
function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action,
        [switch]$NeedsLock
    )
    Write-Section $Name
    $log = Join-Path $script:LogDir "$Name.log"
    $before = Get-Date
    $exit = 0
    try {
        if ($NeedsLock) { Enter-GateLock }
        $global:LASTEXITCODE = 0
        & $Action *>&1 | Tee-Object -FilePath $log | Out-Host
        $exit = $LASTEXITCODE
        if ($null -eq $exit) { $exit = 0 }
    }
    catch {
        Write-Host "STEP ERROR: $_" -ForegroundColor Red
        $exit = 1
    }
    finally {
        if ($NeedsLock) { Exit-GateLock }
    }
    $seconds = ((Get-Date) - $before).TotalSeconds
    if ($exit -eq 0) {
        Add-Result -Name $Name -Status PASS -Seconds $seconds
    }
    else {
        Add-Result -Name $Name -Status FAIL -Seconds $seconds -Detail "exit=$exit (log: $log)"
        Write-Host "---- last 15 lines of $Name ----" -ForegroundColor Red
        if (Test-Path $log) { Get-Content $log -Tail 15 | ForEach-Object { Write-Host "    $_" } }
    }
}

function Add-Skip {
    param([string]$Name, [string]$Reason)
    Write-Section $Name
    Write-Warn "SKIPPED -- $Reason"
    Add-Result -Name $Name -Status SKIP -Detail $Reason
}

# ---------------------------------------------------------------------------
# Resolve context
# ---------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $Worktree 'CMakeLists.txt'))) {
    throw "not a PrismArchive worktree: $Worktree"
}
$Worktree = (Resolve-Path $Worktree).Path

if ($Preset -eq 'auto') {
    $Preset = if ($IsWindows) { 'release-msvc' } else { 'release-gcc' }
}
if (-not $BuildDir) { $BuildDir = Join-Path $Worktree (Join-Path 'build' $Preset) }
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Worktree $BuildDir }
$script:LogDir = Join-Path $BuildDir 'local-gates'

$runner = Resolve-ShellRunner
$py = Resolve-Python
$sevenZip = Resolve-SevenZip

if ($Preset -like '*msvc*') { Import-MsvcEnvironment }

if ($Cxx) {
    $env:CXX = $Cxx
    Write-Info "[env] CXX=$Cxx"
}
elseif ($IsWindows -and $runner -eq 'wsl') {
    # WSL has g++; make sure a Windows-side CXX (e.g. cl.exe) is not forwarded.
    $env:CXX = $null
}
if ($sevenZip) {
    $env:PRISM_7Z_DLL = $sevenZip
    Write-Info "[env] PRISM_7Z_DLL=$sevenZip"
}
else {
    Write-Warn 'no 7z.dll found -- win.* integration suites will SKIP (and ctest treats a silent SKIP as red)'
}

Write-Info "[gates] worktree = $Worktree"
Write-Info "[gates] build    = $BuildDir"
Write-Info "[gates] preset   = $Preset"
Write-Info "[gates] shell    = $runner$(if ($Distro) { " ($Distro)" })"
Write-Info "[gates] python   = $(if ($py) { "$($py.Exe) [$($py.Version)]" } else { 'NOT FOUND' })"

# ---------------------------------------------------------------------------
# Step implementations
# ---------------------------------------------------------------------------
$steps = [ordered]@{}

$steps['presets'] = {
    # CMakePresets.json is an input to every build in this project now, so a
    # typo in it must not surface five minutes into a configure. Asserting the
    # four preset NAMES (not just "it parses") is what makes this a guard and
    # not a restatement: rename or drop one and this goes red.
    Push-Location $Worktree
    try {
        $required = @('release-msvc', 'asan-msvc', 'release-gcc', 'asan-clang')
        $listed = (& cmake --list-presets 2>&1) -join $nl
        if ($LASTEXITCODE -ne 0) {
            throw "cmake --list-presets failed -- CMakePresets.json does not parse:$nl$listed"
        }
        $missing = @($required | Where-Object { $listed -notmatch ('"' + [regex]::Escape($_) + '"') })
        if ($missing.Count -gt 0) {
            throw "CMakePresets.json is missing preset(s): $($missing -join ', ')"
        }
        foreach ($p in $required) { Write-Host "  preset ok: $p" }
    }
    finally { Pop-Location }
}

$steps['configure'] = {
    Push-Location $Worktree
    try {
        & cmake --preset $Preset -B $BuildDir
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0) { throw "cmake --preset $Preset failed (exit $code)" }
}

$steps['build'] = {
    & cmake --build $BuildDir --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake --build failed (exit $LASTEXITCODE)" }
}

$steps['ctest'] = {
    & ctest --test-dir $BuildDir --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
}

function Invoke-PythonTool([string]$relative) {
    if (-not $py) { throw 'python 3.8+ not found (set PRISM_PYTHON or install python)' }
    Push-Location $Worktree
    try {
        & $py.Exe @($py.Prefix) $relative
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0) { throw "$relative failed (exit $code)" }
}

$steps['preflight'] = { Invoke-PythonTool 'tools/preflight.py' }
$steps['manifest'] = { Invoke-PythonTool 'tools/check_manifest.py' }

function Initialize-ShellStage {
    # WSL2 reaches this repo through the 9p mount, where a per-file open costs
    # ~50x what it costs on ext4. Measured on the header gate: 205.8s reading the
    # worktree directly vs 25s from an ext4 mirror of the SAME bytes (one compile
    # of Gallery.hpp: 51s over 9p, 3.9s on ext4 -- and only 10% CPU, i.e. all of
    # it was I/O wait). tar is byte-exact by construction, the mirror is rebuilt
    # from scratch on every run so it can never be stale, and any input a gate
    # needs that is NOT staged fails loudly instead of passing on old data.
    # Skip it with -NoShellMirror (slow, but reads the worktree in place).
    if ($script:ShellStage) { return $script:ShellStage }
    $stage = "/tmp/prism-local-gates-$PID"
    $inputs = 'Src tools/check_headers.sh tools/check_windows_tu.sh tools/win_stub'
    # The sed is not cosmetic: on a core.autocrlf=true checkout the .sh files
    # arrive with CRLF and dash dies on its 'set -eu' line ("Illegal option").
    # .gitattributes pins eol=lf for *.sh, but a worktree checked out before
    # that commit keeps its CRLF, so normalise the mirror too.
    $shCommand = 'set -e; rm -rf ' + $stage + ' ' + $stage + '.tar; mkdir -p ' + $stage +
                 '; tar -cf ' + $stage + '.tar ' + $inputs +
                 '; tar -C ' + $stage + ' -xf ' + $stage + '.tar' +
                 '; find ' + $stage + ' -name "*.sh" -exec sed -i "s/\r$//" {} +'
    $wslArgs = @()
    if ($Distro) { $wslArgs += @('--distribution', $Distro) }
    $wslArgs += @('--cd', $Worktree, '--exec', 'sh', '-c', $shCommand)
    & wsl.exe @wslArgs
    if ($LASTEXITCODE -ne 0) { throw "could not stage the worktree into the WSL filesystem (exit $LASTEXITCODE)" }
    $script:ShellStage = $stage
    Write-Info "[shell] staged [$inputs] -> $stage (WSL ext4 mirror of the worktree)"
    return $stage
}

function Invoke-ShellGate([string]$relative) {
    if ($runner -eq 'native') {
        Push-Location $Worktree
        try {
            if ($Cxx) { $env:CXX = $Cxx }
            & sh $relative
            $code = $LASTEXITCODE
        }
        finally { Pop-Location }
    }
    else {
        $gateRoot = $Worktree
        if (-not $NoShellMirror) { $gateRoot = Initialize-ShellStage }
        $wslArgs = @()
        if ($Distro) { $wslArgs += @('--distribution', $Distro) }
        # --cd accepts a Windows path (converted) and an absolute Linux path (used
        # as is), so the same call works for the worktree and for the mirror.
        $wslArgs += @('--cd', $gateRoot, '--exec', 'env', "CXX=$($env:CXX)", "JOBS=$Parallel", 'sh', $relative)
        & wsl.exe @wslArgs
        $code = $LASTEXITCODE
    }
    if ($code -ne 0) { throw "$relative failed (exit $code)" }
}

$steps['headers'] = { Invoke-ShellGate 'tools/check_headers.sh' }
$steps['windows_tu'] = { Invoke-ShellGate 'tools/check_windows_tu.sh' }

# Flags that turn a step off, with the reason recorded so the verdict cannot
# hide it.
$disabled = [ordered]@{}
if ($SkipBuild) { $disabled['configure'] = '-SkipBuild'; $disabled['build'] = '-SkipBuild' }
if ($SkipTests) { $disabled['ctest'] = '-SkipTests' }
if ($SkipPreflight) { $disabled['preflight'] = '-SkipPreflight' }
if ($SkipManifest) { $disabled['manifest'] = '-SkipManifest' }
if ($SkipShell) { $disabled['headers'] = '-SkipShell'; $disabled['windows_tu'] = '-SkipShell' }
if ($runner -eq 'none') {
    $disabled['headers'] = 'no POSIX shell (pass -ShellRunner wsl|native)'
    $disabled['windows_tu'] = 'no POSIX shell (pass -ShellRunner wsl|native)'
}

$selected = @($steps.Keys)
if ($Only) {
    $wanted = @($Only -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $unknown = @($wanted | Where-Object { $steps.Keys -notcontains $_ })
    if ($unknown.Count -gt 0) {
        throw "-Only names unknown step(s): $($unknown -join ', '). Known: $($steps.Keys -join ', ')"
    }
    $selected = @($steps.Keys | Where-Object { $wanted -contains $_ })
}

New-Item -ItemType Directory -Force -Path $script:LogDir | Out-Null
foreach ($name in $selected) {
    if ($disabled.Contains($name)) { Add-Skip -Name $name -Reason $disabled[$name]; continue }
    $needsLock = ($name -eq 'configure' -or $name -eq 'build')
    Invoke-Step -Name $name -Action $steps[$name] -NeedsLock:$needsLock
}

# Drop the ext4 mirror (best effort): the gates already ran against it.
if ($script:ShellStage -and -not $KeepShellStage) {
    $wslArgs = @()
    if ($Distro) { $wslArgs += @('--distribution', $Distro) }
    $wslArgs += @('--exec', 'sh', '-c', ('rm -rf ' + $script:ShellStage + ' ' + $script:ShellStage + '.tar'))
    & wsl.exe @wslArgs 2>$null | Out-Null
}

# ---------------------------------------------------------------------------
# Verdict: exactly one PASS/FAIL line.
# ---------------------------------------------------------------------------
$total = $script:Results.Count
$passed = @($script:Results | Where-Object Status -eq 'PASS').Count
$skipped = @($script:Results | Where-Object Status -eq 'SKIP')
$failed = @($script:Results | Where-Object Status -eq 'FAIL')
$elapsed = [math]::Round(((Get-Date) - $script:StartedAt).TotalSeconds, 1)
$strictFail = $Strict -and $skipped.Count -gt 0 -and $failed.Count -eq 0

$rule = '-' * 75
$table = @()
$table += $rule
foreach ($r in $script:Results) {
    $table += ('  {0,-6} {1,-14} {2,7:N1}s  {3}' -f $r.Status, $r.Name, $r.Seconds, $r.Detail)
}
$table += $rule
foreach ($line in $table) {
    $colour = if ($line -match '\bFAIL\b') { 'Red' } elseif ($line -match '\bSKIP\b') { 'Yellow' } else { 'Gray' }
    Write-Host $line -ForegroundColor $colour
}

$tail = "preset=$Preset build=$BuildDir"
if ($failed.Count -gt 0) {
    $verdict = "LOCAL GATES: FAIL  ($($total - $failed.Count)/$total steps ok; failed: $(($failed | ForEach-Object Name) -join ', '); $elapsed s; $tail)"
    $verdict += "  logs: $script:LogDir"
}
elseif ($strictFail) {
    $verdict = "LOCAL GATES: FAIL  ($passed/$total steps ok but $($skipped.Count) SKIPPED and -Strict is set: $(($skipped | ForEach-Object Name) -join ', '); $elapsed s; $tail)"
}
elseif ($skipped.Count -gt 0) {
    $verdict = "LOCAL GATES: PASS  ($passed/$total steps ok, $($skipped.Count) SKIPPED: $(($skipped | ForEach-Object Name) -join ', '); $elapsed s; $tail)"
}
else {
    $verdict = "LOCAL GATES: PASS  ($passed/$total steps ok; $elapsed s; $tail)"
}
Write-Host ''
Write-Host $verdict -ForegroundColor $(if ($verdict -match 'PASS') { 'Green' } else { 'Red' })

if ($env:GITHUB_STEP_SUMMARY) {
    $summaryLines = @('### Local gates', '', '~~~', $verdict, '')
    $summaryLines += $table
    $summaryLines += '~~~'
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ($summaryLines -join $nl)
}

if ($verdict -match 'FAIL') { exit 1 }
exit 0
