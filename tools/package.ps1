#requires -Version 5.1
<#
.SYNOPSIS
  Cut the user-installable Windows package: PrismArchive-<version>-win-x64.zip,
  the artifact a user unzips and installs with INSTALL.cmd (card C68).

.DESCRIPTION
  Card C24 answers "can a release engineer prove the bytes?"; this script answers
  the user's question: "I downloaded the zip, now what?"  The package therefore
  carries the payload AND the two entry points a human double-clicks:

    PrismArchive.exe, PrismShellExt.dll, PrismShellExtClassic.dll
    7z.dll                       the engine, so no separate 7-Zip install
    Assets/                      shaders + icon + language tables
    install.ps1, uninstall.ps1, register-static-verbs.ps1
    INSTALL.cmd, UNINSTALL.cmd
    LICENSE, THIRD-PARTY-NOTICES.txt, README.md
    package-manifest.json        size + sha256 of every shipped file

  Layout truth (CONTRACTS D3): the expected file set is the tree that
  "cmake --install" writes -- the same layout install.ps1 deploys -- plus the
  documented root additions.  A copy step that forgets a file is therefore a
  missing file in the manifest and a red build, not a thinner package.

  Version (card C68): read from Src/App/PrismArchive.rc VERSIONINFO.  Nothing is
  hard-coded.  The built exe is then asked for its OWN FileVersion and the two
  must agree, so a stale build cannot be shipped under a new name.  When
  tools/release/version.json exists it must agree as well (drift is red here
  instead of at the user's file properties dialog).

  Idempotency (card C68, item 3): -AssertIdempotent builds the package twice
  from the same inputs and compares a manifest content hash over
  "path|size|sha256" lines.  Timestamps are excluded by construction (the
  generated README carries version + commit, never a clock), so the assertion is
  exact, not "close enough".

  The finished zip is RE-OPENED and every shipped entry is re-hashed from the
  zip stream: the verdict describes the archive, not the staging directory.

.PARAMETER BuildDir
  CMake build directory to install from (must contain Src/PrismWin/PrismArchive.exe).

.PARAMETER OutDir
  Where the zip and its .sha256 sidecar land.  Default: <repo>/dist.

.PARAMETER SevenZipDll
  Explicit 7z.dll.  Default: PRISM_7Z_DLL, then the 7-Zip registry/PATH/program-dir
  probes install.ps1 already uses.  No engine -> explicit SKIP (exit 77), never a
  package that cannot extract anything.

.PARAMETER AssertIdempotent
  Build the package twice and assert the manifest content hash is identical.

.PARAMETER KeepStaging
  Keep the staging trees for inspection (default: removed even on failure).

.EXAMPLE
  . E:\wg\env.ps1 -Card c68
  pwsh -File tools\package.ps1 -BuildDir E:\build\wt-c68 -AssertIdempotent

.EXAMPLE
  pwsh -File tools\package.ps1 -BuildDir E:\build\wt-c68 -SevenZipDll E:\DevTools\7-Zip\7z.dll
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [string]$Config = '',
    [string]$SevenZipDll = '',
    [switch]$AssertIdempotent,
    [switch]$KeepStaging,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }

$script:Quiet = [bool]$Quiet
$script:TempRoot = $null
$script:Checks = 0
$script:LF = [string][char]10

function Say {
    param([string]$Text = '', [string]$Level = 'info')
    if ($script:Quiet -and ($Level -eq 'info' -or $Level -eq 'pass')) { return }  # skip/warn/fail/verdict always print
    $prefix = switch ($Level) {
        'pass' { 'PASS  ' }
        'fail' { 'FAIL  ' }
        'warn' { 'WARN  ' }
        'skip' { 'SKIP  ' }
        default { '' }
    }
    Write-Host ($prefix + $Text)
}

function Stop-Usage {
    param([string]$Text)
    Say ('PACKAGE: USAGE ERROR (' + $Text + ')') 'fail'
    exit 2
}

function Stop-Skip {
    param([string]$Reason)
    Say ('PACKAGE: SKIP (' + $Reason + ')') 'skip'
    exit 77
}

function Format-Bytes {
    param([long]$Bytes)
    return ('{0} bytes ({1:N2} MiB)' -f $Bytes, ($Bytes / 1MB))
}

# ---------------------------------------------------------------------------
# hashing / file helpers
# ---------------------------------------------------------------------------

function Get-Sha256Hex {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-FileSha256 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Get-StreamSha256 {
    param($Stream)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Stream)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Write-TextFileStable {
    # UTF-8 without BOM and LF line endings: identical bytes on every run, so the
    # manifest content hash can be compared between two package builds.
    param([string]$Path, [string]$Text)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-CmdFile {
    # cmd.exe is happiest with CRLF; ASCII only, so the batch parser cannot trip
    # over the code page.
    param([string]$Path, [string[]]$Lines)
    $text = ($Lines -join ([string][char]13 + [string][char]10)) + [string][char]13 + [string][char]10
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.ASCIIEncoding))
}

# ---------------------------------------------------------------------------
# version: Src/App/PrismArchive.rc is the authority (card C68 says so)
# ---------------------------------------------------------------------------

function Get-RcVersion {
    param([string]$RcPath)

    if (-not (Test-Path -LiteralPath $RcPath -PathType Leaf)) {
        throw ('VERSIONINFO source missing: ' + $RcPath)
    }
    $text = [System.IO.File]::ReadAllText($RcPath)
    $values = @{}
    foreach ($name in @('MAJOR', 'MINOR', 'PATCH', 'BUILD')) {
        $m = [regex]::Match($text, '#define\s+PRISM_VERSION_' + $name + '\s+(\d+)')
        if (-not $m.Success) { throw ('VERSIONINFO is missing PRISM_VERSION_' + $name + ' in ' + $RcPath) }
        $values[$name] = [int]$m.Groups[1].Value
    }
    $stringMatch = [regex]::Match($text, '#define\s+PRISM_VERSION_STRING\s+"([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)"')
    if (-not $stringMatch.Success) {
        throw ('VERSIONINFO is missing a well-formed PRISM_VERSION_STRING in ' + $RcPath)
    }
    $quad = ('{0}.{1}.{2}.{3}' -f $values['MAJOR'], $values['MINOR'], $values['PATCH'], $values['BUILD'])
    $stringQuad = ('{0}.{1}.{2}.{3}' -f $stringMatch.Groups[1].Value, $stringMatch.Groups[2].Value,
                                        $stringMatch.Groups[3].Value, $stringMatch.Groups[4].Value)
    if ($quad -ne $stringQuad) {
        throw ('VERSIONINFO disagrees with itself: macros=' + $quad + ' PRISM_VERSION_STRING=' + $stringQuad)
    }
    return [pscustomobject]@{
        Version = ('{0}.{1}.{2}' -f $values['MAJOR'], $values['MINOR'], $values['PATCH'])
        Quad    = $quad
        Source  = $RcPath
    }
}

function Assert-ExeVersion {
    param([string]$ExePath, $VersionInfo)
    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) { throw ('payload exe missing: ' + $ExePath) }
    $info = (Get-Item -LiteralPath $ExePath).VersionInfo
    $fileVersion = [string]$info.FileVersion
    $productVersion = [string]$info.ProductVersion
    if ($fileVersion -ne $VersionInfo.Quad) {
        throw ('stale build: ' + $ExePath + ' FileVersion [' + $fileVersion + '] != VERSIONINFO [' + $VersionInfo.Quad + ']; rebuild before packaging')
    }
    if ($productVersion -ne $VersionInfo.Quad) {
        throw ('stale build: ' + $ExePath + ' ProductVersion [' + $productVersion + '] != VERSIONINFO [' + $VersionInfo.Quad + ']')
    }
    Say ('version: ' + $VersionInfo.Version + ' (' + $VersionInfo.Quad + ') from ' + (Split-Path -Leaf $VersionInfo.Source) + ', confirmed by the exe VERSIONINFO') 'pass'
}

# ---------------------------------------------------------------------------
# engine discovery (the zip must carry 7z.dll: "unzip and it works")
# ---------------------------------------------------------------------------

function Find-EngineDll {
    param([string]$Explicit)

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Explicit) { $candidates.Add($Explicit) }
    if ($env:PRISM_7Z_DLL) { $candidates.Add([string]$env:PRISM_7Z_DLL) }
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
            foreach ($sub in @('SOFTWARE\7-Zip', 'SOFTWARE\WOW6432Node\7-Zip')) {
                $k = $base.OpenSubKey($sub)
                if ($null -eq $k) { continue }
                foreach ($name in @('Path64', 'Path')) {
                    $v = $k.GetValue($name)
                    if ($v) { $candidates.Add((Join-Path ([string]$v) '7z.dll')) }
                }
                $k.Close()
            }
            $base.Close()
        } catch { }
    }
    foreach ($dir in @($env:ProgramFiles,
                       [Environment]::GetEnvironmentVariable('ProgramFiles(x86)'),
                       [Environment]::GetEnvironmentVariable('ProgramW6432'),
                       (Join-Path $env:LOCALAPPDATA 'Programs\7-Zip'))) {
        if ($dir) { $candidates.Add((Join-Path $dir '7z.dll')) }
    }
    $cmd = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add((Join-Path (Split-Path -Parent $cmd.Source) '7z.dll')) }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return ''
}

# ---------------------------------------------------------------------------
# staged content
# ---------------------------------------------------------------------------

function Get-ExclusionReason {
    param([string]$RelativePath)
    $rel = $RelativePath -replace '/', '\'
    if ($rel -like 'share\*') {
        return 'duplicate shader tree installed by the root CMakeLists; the app loads Assets\Shaders only'
    }
    if ($rel -like 'Assets\Package\*') {
        return 'MSIX sparse-package payload (C61); a per-user INSTALL.cmd install creates no package identity'
    }
    return ''
}

function New-GeneratedReadme {
    param($VersionInfo, [string]$Commit, [string]$EngineName)
    $nl = [string][char]10
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('# PrismArchive ' + $VersionInfo.Version + ' (win-x64) - unzip, then double-click INSTALL.cmd')
    $out.Add('')
    $out.Add('Windows 10 1809+ / Windows 11 x64. No administrator rights: the install writes')
    $out.Add('only under %LOCALAPPDATA% and HKCU.')
    $out.Add('')
    $out.Add('## Install (three steps)')
    $out.Add('')
    $out.Add('1. Extract this zip to any folder (do not run it from inside the zip preview).')
    $out.Add('2. Double-click INSTALL.cmd (scripts: INSTALL.cmd -Quiet).')
    $out.Add('3. Right-click any file: the menu now has "Extract here with PrismArchive" and')
    $out.Add('   "Open with PrismArchive".')
    $out.Add('')
    $out.Add('Default install folder: %LOCALAPPDATA%\PrismArchive. The zip ships 7z.dll')
    $out.Add('(' + $EngineName + '); INSTALL.cmd puts it beside PrismArchive.exe, so a')
    $out.Add('machine without 7-Zip still extracts.')
    $out.Add('')
    $out.Add('## Uninstall')
    $out.Add('')
    $out.Add('Double-click UNINSTALL.cmd: it removes the install folder, the right-click verbs,')
    $out.Add('the file associations, the uninstall entry and the engine environment variable.')
    $out.Add('Quiet form: UNINSTALL.cmd -Quiet -InstallDir <install folder>.')
    $out.Add('')
    $out.Add('## Verify')
    $out.Add('')
    $out.Add('package-manifest.json lists the byte size and SHA-256 of every shipped file.')
    $out.Add('The publisher verifies the whole archive with')
    $out.Add('pwsh -File tools/verify-package.ps1 -ZipDir dist')
    $out.Add('(install -> settings -> right-click verbs -> extract -> uninstall).')
    $out.Add('')
    $out.Add('## Contents')
    $out.Add('')
    $out.Add('- PrismArchive.exe / PrismShellExt.dll / PrismShellExtClassic.dll')
    $out.Add('- 7z.dll (7-Zip engine; licence in THIRD-PARTY-NOTICES.txt)')
    $out.Add('- Assets/ (shaders, icon, language tables)')
    $out.Add('- install.ps1 / uninstall.ps1 / register-static-verbs.ps1')
    $out.Add('- INSTALL.cmd / UNINSTALL.cmd / LICENSE / package-manifest.json')
    $out.Add('')
    $out.Add('Version ' + $VersionInfo.Version + ' (VERSIONINFO ' + $VersionInfo.Quad + '), commit ' + $Commit + '.')
    return (($out -join $nl) + $nl)
}

function New-InstallCmd {
    param([string]$Version)
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('@echo off')
    $out.Add('rem PrismArchive ' + $Version + ' package installer (card C68).')
    $out.Add('rem 1) runs install.ps1 from this folder with every argument forwarded;')
    $out.Add('rem 2) copies the bundled 7z.dll beside the installed exe, so the app finds')
    $out.Add('rem    the engine through its "next to the exe" candidate and a machine')
    $out.Add('rem    without 7-Zip still works.')
    $out.Add('setlocal EnableExtensions')
    $out.Add('set "ROOT=%~dp0"')
    $out.Add('set "TARGET=%LOCALAPPDATA%\PrismArchive"')
    $out.Add('call :scanargs %*')
    $out.Add('powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%install.ps1" %*')
    $out.Add('set "RC=%ERRORLEVEL%"')
    $out.Add('if not "%RC%"=="0" (')
    $out.Add('  echo INSTALL.CMD: FAIL install.ps1 exited %RC%')
    $out.Add('  exit /b %RC%')
    $out.Add(')')
    $out.Add('if exist "%ROOT%7z.dll" (')
    $out.Add('  if not exist "%TARGET%" mkdir "%TARGET%" >nul 2>&1')
    $out.Add('  copy /y "%ROOT%7z.dll" "%TARGET%\7z.dll" >nul')
    $out.Add('  if not exist "%TARGET%\7z.dll" (')
    $out.Add('    echo INSTALL.CMD: FAIL could not deploy 7z.dll to "%TARGET%"')
    $out.Add('    exit /b 1')
    $out.Add('  )')
    $out.Add(')')
    $out.Add('echo INSTALL.CMD: OK target=%TARGET%')
    $out.Add('exit /b 0')
    $out.Add(':scanargs')
    $out.Add('if "%~1"=="" goto :eof')
    $out.Add('if /i "%~1"=="-InstallDir" (')
    $out.Add('  set "TARGET=%~2"')
    $out.Add('  shift')
    $out.Add('  shift')
    $out.Add('  goto scanargs')
    $out.Add(')')
    $out.Add('shift')
    $out.Add('goto scanargs')
    return $out.ToArray()
}

function New-UninstallCmd {
    param([string]$Version)
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('@echo off')
    $out.Add('rem PrismArchive ' + $Version + ' package uninstaller (card C68).')
    $out.Add('rem Forwards every argument to uninstall.ps1 (which reads the install marker')
    $out.Add('rem when -InstallDir is not given) and reports the exit code it got.')
    $out.Add('setlocal EnableExtensions')
    $out.Add('set "ROOT=%~dp0"')
    $out.Add('powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%uninstall.ps1" %*')
    $out.Add('set "RC=%ERRORLEVEL%"')
    $out.Add('if not "%RC%"=="0" (')
    $out.Add('  echo UNINSTALL.CMD: FAIL uninstall.ps1 exited %RC%')
    $out.Add('  exit /b %RC%')
    $out.Add(')')
    $out.Add('echo UNINSTALL.CMD: OK')
    $out.Add('exit /b 0')
    return $out.ToArray()
}

function New-ThirdPartyNotices {
    param([string]$EngineDll)
    $nl = [string][char]10
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add('PrismArchive ships the 7-Zip engine (7z.dll) so the package works without a')
    $out.Add('separate 7-Zip installation.')
    $out.Add('')
    $out.Add('7-Zip is licensed under the GNU LGPL 2.1 or later, with the unRAR restriction.')
    $out.Add('Source: https://www.7-zip.org/download.html')
    $out.Add('')
    $out.Add('This package contains: ' + (Split-Path -Leaf $EngineDll))
    $out.Add('')
    $licensePath = Join-Path (Split-Path -Parent $EngineDll) 'License.txt'
    if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
        $out.Add([System.IO.File]::ReadAllText($licensePath))
    } else {
        $out.Add('The license text was not found next to the engine on this machine.')
        $out.Add('See https://www.7-zip.org/license.txt for the authoritative text.')
    }
    return (($out -join $nl) + $nl)
}

# ---------------------------------------------------------------------------
# manifest: the file list, sizes and hashes that BOTH the idempotency check and
# the post-zip verification compare against
# ---------------------------------------------------------------------------

function New-Manifest {
    param([string]$PackageRoot, $VersionInfo, [string]$Commit, [string]$BuiltAt, $Excluded)

    $root = (Resolve-Path -LiteralPath $PackageRoot).Path
    $all = @([System.IO.Directory]::EnumerateFiles($root, '*', [System.IO.SearchOption]::AllDirectories))
    $rels = New-Object System.Collections.Generic.List[string]
    foreach ($full in $all) {
        $rels.Add($full.Substring($root.Length).TrimStart('\', '/'))
    }
    $sorted = $rels.ToArray()
    [Array]::Sort($sorted, [System.StringComparer]::Ordinal)

    $entries = New-Object System.Collections.Generic.List[object]
    $lines = New-Object System.Collections.Generic.List[string]
    $total = [long]0
    foreach ($rel in $sorted) {
        if ($rel -eq 'package-manifest.json') { continue }
        $full = Join-Path $root $rel
        $size = (Get-Item -LiteralPath $full).Length
        $hash = Get-FileSha256 $full
        $total += $size
        $entries.Add([ordered]@{
            path   = ($rel -replace '\\', '/')
            size   = [long]$size
            sha256 = $hash
        })
        $lines.Add(($rel -replace '\\', '/') + '|' + $size + '|' + $hash)
    }
    $contentHash = Get-Sha256Hex ([System.Text.Encoding]::UTF8.GetBytes(($lines -join $script:LF) + $script:LF))

    $manifest = [ordered]@{
        product             = 'PrismArchive'
        version             = $VersionInfo.Version
        versionInfo         = $VersionInfo.Quad
        runtime             = 'win-x64'
        gitCommit           = $Commit
        builtAt             = $BuiltAt
        fileCount           = $entries.Count
        totalBytes          = $total
        contentHash         = $contentHash
        manifestCovers      = 'every file in this zip except package-manifest.json itself'
        excludedFromPackage = $Excluded
        files               = $entries.ToArray()
    }
    $json = ($manifest | ConvertTo-Json -Depth 6) + $script:LF
    Write-TextFileStable (Join-Path $root 'package-manifest.json') $json
    return [pscustomobject]@{
        FileCount   = $entries.Count
        TotalBytes  = $total
        ContentHash = $contentHash
        Files       = $entries.ToArray()
        Json        = $json
    }
}

# ---------------------------------------------------------------------------
# one packaging run
# ---------------------------------------------------------------------------

function New-PackageStage {
    param(
        [string]$StageRoot,
        [string]$PackageName,
        $VersionInfo,
        [string]$Commit,
        [string]$BuildDir,
        [string]$Config,
        [string]$EngineDll,
        [string]$RepoRoot,
        [string]$BuiltAt
    )

    $installRoot = Join-Path $StageRoot '.install'
    New-Item -ItemType Directory -Force -Path $installRoot | Out-Null
    $cmakeArgs = @('--install', $BuildDir, '--prefix', $installRoot)
    if ($Config) { $cmakeArgs += @('--config', $Config) }
    $installLog = & cmake @cmakeArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ('cmake --install failed (exit ' + $LASTEXITCODE + '): ' + (($installLog | Out-String).Trim()))
    }

    $pkg = Join-Path $StageRoot $PackageName
    New-Item -ItemType Directory -Force -Path $pkg | Out-Null
    $excluded = New-Object System.Collections.Generic.List[object]
    $excludedSeen = New-Object System.Collections.Generic.HashSet[string]

    $installed = @([System.IO.Directory]::EnumerateFiles($installRoot, '*', [System.IO.SearchOption]::AllDirectories))
    foreach ($full in $installed) {
        $rel = $full.Substring($installRoot.Length).TrimStart('\', '/')
        $reason = Get-ExclusionReason $rel
        if ($reason) {
            $norm = $rel -replace '/', '\'
            $pattern = if ($norm -like 'Assets\Package\*') { 'Assets/Package/**' } else { $norm.Split('\')[0] + '/**' }
            if ($excludedSeen.Add($pattern)) {
                $excluded.Add([ordered]@{ pattern = $pattern; reason = $reason })
            }
            continue
        }
        $dest = Join-Path $pkg $rel
        $destDir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
        [System.IO.File]::Copy($full, $dest, $true)
    }

    # The engine: required by the card and by "the zip works on a clean machine".
    $engineDest = Join-Path $pkg '7z.dll'
    [System.IO.File]::Copy($EngineDll, $engineDest, $true)
    if ((Get-Item -LiteralPath $engineDest).Length -ne (Get-Item -LiteralPath $EngineDll).Length) {
        throw ('7z.dll copy is not byte-equal: ' + $engineDest)
    }

    [System.IO.File]::Copy((Join-Path $RepoRoot 'LICENSE'), (Join-Path $pkg 'LICENSE'), $true)
    Write-TextFileStable (Join-Path $pkg 'README.md') (New-GeneratedReadme $VersionInfo $Commit (Split-Path -Leaf $EngineDll))
    Write-TextFileStable (Join-Path $pkg 'THIRD-PARTY-NOTICES.txt') (New-ThirdPartyNotices $EngineDll)
    Write-CmdFile (Join-Path $pkg 'INSTALL.cmd') (New-InstallCmd $VersionInfo.Version)
    Write-CmdFile (Join-Path $pkg 'UNINSTALL.cmd') (New-UninstallCmd $VersionInfo.Version)

    return [pscustomobject]@{ PackageRoot = $pkg; Excluded = $excluded.ToArray() }
}

function Assert-RequiredSet {
    param([string]$PackageRoot, $VersionInfo)
    $required = @(
        'PrismArchive.exe',
        'PrismShellExt.dll',
        'PrismShellExtClassic.dll',
        '7z.dll',
        'install.ps1',
        'uninstall.ps1',
        'register-static-verbs.ps1',
        'INSTALL.cmd',
        'UNINSTALL.cmd',
        'LICENSE',
        'README.md',
        'THIRD-PARTY-NOTICES.txt'
    )
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $name) -PathType Leaf)) { $missing.Add($name) }
    }
    if ($missing.Count -gt 0) {
        throw ('package is missing required file(s): ' + ($missing -join ', '))
    }
    $script:Checks++
    Say ('package carries all ' + $required.Count + ' required root files (payload + entry points + licence)') 'pass'

    $shaders = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot 'Assets\Shaders') -Filter '*.hlsl' -File)
    $hlsli = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot 'Assets\Shaders') -Filter '*.hlsli' -File)
    if (($shaders.Count + $hlsli.Count) -lt 5) {
        throw ('Assets\Shaders has only ' + ($shaders.Count + $hlsli.Count) + ' shader files; the install layout ships 5')
    }
    $script:Checks++
    Say ('Assets\Shaders: ' + $shaders.Count + ' .hlsl + ' + $hlsli.Count + ' .hlsli') 'pass'
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot 'Assets\Icon\prism.ico') -PathType Leaf)) {
        throw 'Assets\Icon\prism.ico is missing'
    }
    $langs = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot 'Assets\Lang') -Filter '*.json' -File)
    if ($langs.Count -lt 2) { throw ('Assets\Lang has only ' + $langs.Count + ' language table(s)') }
    $script:Checks++
    Say ('Assets: icon present, ' + $langs.Count + ' language tables, version ' + $VersionInfo.Version) 'pass'
}

function New-PackageZip {
    param([string]$PackageRoot, [string]$ZipPath)
    $parent = Split-Path -Parent $ZipPath
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $PackageRoot, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    return (Get-Item -LiteralPath $ZipPath).Length
}

function Assert-ZipMatchesManifest {
    param([string]$ZipPath, $Manifest, [string]$PackageName)

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $map = @{}
        $rootPrefix = $PackageName + '/'
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) { continue }  # directory entry
            $name = $entry.FullName -replace '\\', '/'
            if (-not $name.StartsWith($rootPrefix, [System.StringComparison]::Ordinal)) {
                throw ('zip entry is outside the single root folder ' + $rootPrefix + ': ' + $name)
            }
            $name = $name.Substring($rootPrefix.Length)
            if ($map.ContainsKey($name)) { throw ('zip contains a duplicate entry: ' + $name) }
            $map[$name] = $entry
        }
        $expected = @{}
        foreach ($f in $Manifest.Files) { $expected[$f.path] = $f }
        $expected['package-manifest.json'] = $null  # present, not self-hashed

        foreach ($name in $expected.Keys) {
            if (-not $map.ContainsKey($name)) { throw ('zip is missing manifest entry: ' + $name) }
        }
        if ($map.Count -ne $expected.Count) {
            throw ('zip has ' + $map.Count + ' file entries but the manifest describes ' + $expected.Count)
        }
        $checked = 0
        $bytes = [long]0
        foreach ($f in $Manifest.Files) {
            $entry = $map[$f.path]
            $stream = $entry.Open()
            try { $hash = Get-StreamSha256 $stream } finally { $stream.Dispose() }
            if ($hash -ne $f.sha256) { throw ('zip bytes changed for ' + $f.path + ': ' + $hash + ' != ' + $f.sha256) }
            if ($entry.Length -ne $f.size) { throw ('zip entry size changed for ' + $f.path) }
            $checked++
            $bytes += $entry.Length
        }
        $manifestEntry = $map['package-manifest.json']
        if ($manifestEntry.Length -le 0) { throw 'package-manifest.json is empty in the zip' }
        $script:Checks++
        Say ('re-opened the zip: ' + $checked + ' entries re-hashed from the archive, ' + (Format-Bytes $bytes) + ', every sha256 matches the manifest') 'pass'
        return [pscustomobject]@{ Entries = $checked; Bytes = $bytes }
    } finally {
        $archive.Dispose()
    }
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

try {
    if (-not $BuildDir) { Stop-Usage '-BuildDir is required (or run through ctest, which passes it)' }
    if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { Stop-Usage ('-BuildDir does not exist: ' + $BuildDir) }
    $exe = Join-Path $BuildDir 'Src\PrismWin\PrismArchive.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Stop-Usage ('PrismArchive.exe is not built in ' + $BuildDir + ' (build the PrismArchive target first)')
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue

    $rc = Join-Path $RepoRoot 'Src\App\PrismArchive.rc'
    $versionInfo = Get-RcVersion $rc
    if ($versionInfo.Version -notmatch '^\d+\.\d+\.\d+$') { throw ('unexpected version string: ' + $versionInfo.Version) }
    Assert-ExeVersion $exe $versionInfo

    $releaseJson = Join-Path $RepoRoot 'tools\release\version.json'
    if (Test-Path -LiteralPath $releaseJson -PathType Leaf) {
        $release = Get-Content -LiteralPath $releaseJson -Raw | ConvertFrom-Json
        if ([string]$release.version -ne $versionInfo.Version) {
            throw ('version drift: tools/release/version.json says [' + $release.version + '], Src/App/PrismArchive.rc VERSIONINFO says [' + $versionInfo.Version + ']')
        }
        $script:Checks++
        Say ('version.json agrees with the VERSIONINFO: ' + $versionInfo.Version) 'pass'
    }

    $engine = Find-EngineDll -Explicit $SevenZipDll
    if (-not $engine) {
        Stop-Skip 'no 7z.dll found (pass -SevenZipDll, set PRISM_7Z_DLL, or install 7-Zip); the package must ship the engine it extracts with'
    }
    $script:Checks++
    Say ('engine: ' + $engine + ' (' + (Format-Bytes (Get-Item -LiteralPath $engine).Length) + ')') 'pass'

    $commit = 'unknown'
    try {
        $c = & git -C $RepoRoot rev-parse --short=12 HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $c) { $commit = ([string]$c).Trim() }
    } catch { }

    $packageName = 'PrismArchive-' + $versionInfo.Version + '-win-x64'
    $zipPath = Join-Path $OutDir ($packageName + '.zip')
    $builtAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')

    $script:TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('prism-c68-package-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $script:TempRoot | Out-Null
    Say ('staging: ' + $script:TempRoot)

    $stageA = Join-Path $script:TempRoot 'a'
    New-Item -ItemType Directory -Force -Path $stageA | Out-Null
    $stageParams = @{
        StageRoot = $stageA; PackageName = $packageName; VersionInfo = $versionInfo
        Commit = $commit; BuildDir = $BuildDir; Config = $Config
        EngineDll = $engine; RepoRoot = $RepoRoot; BuiltAt = $builtAt
    }
    $stageAResult = New-PackageStage @stageParams
    Assert-RequiredSet -PackageRoot $stageAResult.PackageRoot -VersionInfo $versionInfo
    $manifestParams = @{
        PackageRoot = $stageAResult.PackageRoot; VersionInfo = $versionInfo
        Commit = $commit; BuiltAt = $builtAt; Excluded = $stageAResult.Excluded
    }
    $manifest = New-Manifest @manifestParams
    $script:Checks++
    Say ('manifest: ' + $manifest.FileCount + ' files, ' + (Format-Bytes $manifest.TotalBytes) + ', contentHash ' + $manifest.ContentHash) 'pass'

    $zipBytes = New-PackageZip -PackageRoot $stageAResult.PackageRoot -ZipPath $zipPath
    $zipInfo = Assert-ZipMatchesManifest -ZipPath $zipPath -Manifest $manifest -PackageName $packageName
    $zipSha = Get-FileSha256 $zipPath
    Write-TextFileStable ($zipPath + '.sha256') ($zipSha + '  ' + $packageName + '.zip' + $script:LF)
    $script:Checks++
    Say ('zip: ' + $zipPath + ' ' + (Format-Bytes $zipBytes) + ' sha256=' + $zipSha) 'pass'

    $idempotent = 'NOT-CHECKED'
    if ($AssertIdempotent) {
        $stageB = Join-Path $script:TempRoot 'b'
        New-Item -ItemType Directory -Force -Path $stageB | Out-Null
        $stageParamsB = @{
            StageRoot = $stageB; PackageName = $packageName; VersionInfo = $versionInfo
            Commit = $commit; BuildDir = $BuildDir; Config = $Config
            EngineDll = $engine; RepoRoot = $RepoRoot; BuiltAt = $builtAt
        }
        $stageBResult = New-PackageStage @stageParamsB
        Assert-RequiredSet -PackageRoot $stageBResult.PackageRoot -VersionInfo $versionInfo
        $manifestParamsB = @{
            PackageRoot = $stageBResult.PackageRoot; VersionInfo = $versionInfo
            Commit = $commit; BuiltAt = $builtAt; Excluded = $stageBResult.Excluded
        }
        $manifestB = New-Manifest @manifestParamsB
        if ($manifestB.ContentHash -ne $manifest.ContentHash) {
            throw ('package is not idempotent: contentHash ' + $manifest.ContentHash + ' != ' + $manifestB.ContentHash)
        }
        if ($manifestB.FileCount -ne $manifest.FileCount -or $manifestB.TotalBytes -ne $manifest.TotalBytes) {
            throw ('package is not idempotent: ' + $manifestB.FileCount + '/' + $manifestB.TotalBytes +
                   ' != ' + $manifest.FileCount + '/' + $manifest.TotalBytes)
        }
        for ($i = 0; $i -lt $manifest.Files.Count; $i++) {
            $a = $manifest.Files[$i]
            $b = $manifestB.Files[$i]
            if ($a.path -ne $b.path -or $a.size -ne $b.size -or $a.sha256 -ne $b.sha256) {
                throw ('package is not idempotent at entry ' + $i + ': [' + $a.path + '/' + $a.size + '/' + $a.sha256 +
                       '] != [' + $b.path + '/' + $b.size + '/' + $b.sha256 + ']')
            }
        }
        $idempotent = 'YES'
        $script:Checks++
        Say ('idempotent: a second build from the same inputs produced the identical ' + $manifest.FileCount +
             '-file manifest (contentHash ' + $manifest.ContentHash + ')') 'pass'
    }

    Say (('PACKAGE: PASS version=' + $versionInfo.Version + ' files=' + $manifest.FileCount +
         ' bytes=' + $manifest.TotalBytes + ' idempotent=' + $idempotent +
         ' zip=' + $packageName + '.zip sha256=' + $zipSha)) 'verdict'
    exit 0
} catch {
    Say ('PACKAGE: FAIL (' + $_.Exception.Message + ')') 'fail'
    Say ([string]$_.ScriptStackTrace) 'fail'
    exit 1
} finally {
    if ($script:TempRoot -and (Test-Path -LiteralPath $script:TempRoot)) {
        if ($KeepStaging) {
            Say ('staging kept: ' + $script:TempRoot) 'warn'
        } else {
            Remove-Item -LiteralPath $script:TempRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
