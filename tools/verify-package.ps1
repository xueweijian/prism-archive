#requires -Version 5.1
<#
.SYNOPSIS
  Verify the user-facing package the way a downloading user experiences it:
  unzip -> INSTALL.cmd -> files/settings/right-click verbs -> extract -> uninstall
  (card C68).

.DESCRIPTION
  tools/package.ps1 proves what is IN the zip.  This script proves what happens
  when someone runs it.  Every step is a real assertion with an exit code; there
  is no "looks fine" path:

    1. EXTRACT   the zip into a unique %TEMP% directory (GUID, asserted absent
                 before use), then every manifest entry must exist on disk with
                 the exact byte count and SHA-256 the manifest promises.
    2. INSTALL   INSTALL.cmd -Quiet -InstallDir <unique> -ScopeRoot <scratch>.
                 Exit code 0 AND the INSTALL.CMD: OK marker are both required.
                 The scratch scope keeps the file associations / marker /
                 uninstall entry of a real install out of harm's way.
    3. FILES     exe + both shell DLLs + 7z.dll are byte-identical to the payload;
                 the whole Assets tree is present with the same bytes.
    4. SETTINGS  %LOCALAPPDATA%\PrismArchive accepts a settings.json write (a
                 4096-byte probe is written, read back and compared byte for
                 byte); an existing settings.json must open read-write.
    5. VERBS     HKCU\Software\Classes\*\shell\PrismArchiveExtract / Open exist,
                 and their command lines are EXACTLY the installed exe + verb.
                 The pre-existing real menu is snapshotted first and restored in
                 the finally block.
    6. EXTRACT   the installed app compresses a 4096-byte fixture with
                 --compress-7z, deletes the source, then --extract-here must
                 produce a byte-identical file.  Both steps have a bounded
                 timeout (CONTRACTS D29) and a non-zero exit is a failure.
    7. UNINSTALL UNINSTALL.cmd removes both verb keys, the install directory, the
                 marker and the uninstall entry -- asserted key by key.

  Admin-dependent part: writing HKCU is per-user, but a policy-locked or
  service account cannot do it.  That case is an explicit SKIP (exit 77), which
  ctest reports as "Skipped" (CONTRACTS D1) -- never a green tick.

  Negative controls (CONTRACTS D3), -Mutate:
    DropShellDll  delete PrismShellExt.dll from the extracted package after the
                  injection is asserted to have happened; the manifest/bytes
                  assertion must go red.
    SkipVerbs     delete the PrismArchiveExtract key after install; the verb
                  assertion must go red.
  Exit 0 only when the guard went red AND the injection was observed; "NOT
  INJECTED" and "NOT CAUGHT" both exit 1.  The verdict is printed at the FIRST
  failed assertion (CONTRACTS D27), so a teardown crash cannot swallow it.

.PARAMETER ZipPath
  The zip to verify.

.PARAMETER ZipDir
  Directory holding exactly one PrismArchive-*-win-x64.zip (used by ctest, which
  does not know the version-derived file name).

.PARAMETER Mutate
  DropShellDll | SkipVerbs

.EXAMPLE
  . E:\wg\env.ps1 -Card c68
  pwsh -File tools\package.ps1 -BuildDir E:\build\wt-c68
  pwsh -File tools\verify-package.ps1 -ZipDir dist

.EXAMPLE
  pwsh -File tools\verify-package.ps1 -ZipDir dist -Mutate DropShellDll
#>
[CmdletBinding()]
param(
    [string]$ZipPath = '',
    [string]$ZipDir = '',
    [ValidateSet('', 'DropShellDll', 'SkipVerbs')]
    [string]$Mutate = '',
    [int]$ExeTimeoutSeconds = 120,
    [switch]$KeepTemp,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:Quiet = [bool]$Quiet
$script:Checks = 0
$script:Failures = New-Object System.Collections.Generic.List[string]
$script:Skips = New-Object System.Collections.Generic.List[string]
$script:Mutation = $Mutate
$script:MutationObserved = $false
$script:Installed = $false
$script:Uninstalled = $false
$script:WorkDir = ''
$script:PackageRoot = ''
$script:InstallDir = ''
$script:ScopeRoot = ''
$script:VerbNames = @('PrismArchiveExtract', 'PrismArchiveOpen')
$script:VerbRoot = 'HKCU:\Software\Classes\*\shell'
$script:VerbSnapshot = @{}
$script:LF = [string][char]10

function Say {
    param([string]$Text = '', [string]$Level = 'info')
    if ($script:Quiet -and ($Level -eq 'info' -or $Level -eq 'pass')) { return }  # skip/warn/error/verdict always print
    $prefix = switch ($Level) {
        'pass' { 'PASS  ' }
        'fail' { 'FAIL  ' }
        'skip' { 'SKIP  ' }
        'warn' { 'WARN  ' }
        'error' { 'ERROR ' }
        default { '' }
    }
    Write-Host ($prefix + $Text)
}

function Add-Check { $script:Checks++ }

function Add-Failure {
    param([string]$Line)
    $script:Failures.Add($Line)
    # CONTRACTS D27: the verdict belongs to the FIRST failed assertion. Print it
    # here and leave; the finally block still runs the cleanup (uninstall,
    # verb restore, temp removal). Exit 0 because the ctest entry decides with
    # PASS_REGULAR_EXPRESSION and the FAIL line above is already on stdout.
    if ($script:Mutation -and $script:MutationObserved) {
        Say ('PACKAGE VERIFY(MUTATION ' + $script:Mutation + '): CAUGHT (' + $script:Failures.Count + ' assertion(s) went red; injection observed; verdict printed at the first failure)') 'verdict'
        exit 0
    }
}

function Assert-True {
    param([string]$What, [bool]$Condition, [string]$Detail = '')
    Add-Check
    if ($Condition) {
        Say $What 'pass'
    } else {
        $line = $What
        if ($Detail) { $line += ' (' + $Detail + ')' }
        Say $line 'fail'
        Add-Failure $line
    }
}

function Assert-Equal {
    param([string]$What, $Expected, $Actual)
    Add-Check
    if ([string]$Expected -eq [string]$Actual) {
        Say $What 'pass'
    } else {
        $line = $What + ' -- expected [' + [string]$Expected + '], got [' + [string]$Actual + ']'
        Say $line 'fail'
        Add-Failure $line
    }
}

function Skip-Check {
    param([string]$What, [string]$Reason)
    Add-Check
    $line = $What + ' -- ' + $Reason
    $script:Skips.Add($line)
    Say $line 'skip'
}

function Stop-Usage {
    param([string]$Text)
    Say ('PACKAGE VERIFY: USAGE ERROR (' + $Text + ')') 'error'
    exit 2
}

function Stop-Skip {
    param([string]$Reason)
    Say ('PACKAGE VERIFY: SKIP (' + $Reason + ')') 'skip'
    exit 77
}

# ---------------------------------------------------------------------------
# hashing
# ---------------------------------------------------------------------------

function Get-FileSha256 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Get-BytesSha256 {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function New-ProbeBytes {
    # Deterministic 4096-byte fixture: the comparison is over bytes, not "a file
    # appeared" (CONTRACTS D2).
    param([int]$Count = 4096)
    $bytes = [byte[]]::new($Count)
    for ($i = 0; $i -lt $Count; $i++) { $bytes[$i] = [byte](65 + ($i % 26)) }
    return $bytes
}

# ---------------------------------------------------------------------------
# registry primitives (independent of install.ps1/uninstall.ps1 on purpose:
# this file must be able to disagree with the writers)
# ---------------------------------------------------------------------------

function Get-Hive {
    param([string]$Path)
    $idx = $Path.IndexOf('\')
    if ($idx -lt 1) { throw ('not a registry path: ' + $Path) }
    $root = $Path.Substring(0, $idx).ToUpperInvariant()
    $sub = $Path.Substring($idx + 1)
    switch ($root) {
        'HKCU:' { return @([Microsoft.Win32.Registry]::CurrentUser, $sub) }
        'HKLM:' { return @([Microsoft.Win32.Registry]::LocalMachine, $sub) }
        default { throw ('unsupported registry hive: ' + $root) }
    }
}

function Test-RegKey {
    param([string]$Path)
    try {
        $h = Get-Hive $Path
        $k = $h[0].OpenSubKey($h[1])
        if ($null -eq $k) { return $false }
        $k.Close()
        return $true
    } catch { return $false }
}

function Get-RegValue {
    param([string]$Path, [string]$Name = '')
    try {
        $h = Get-Hive $Path
        $k = $h[0].OpenSubKey($h[1])
        if ($null -eq $k) { return $null }
        try { return $k.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        finally { $k.Close() }
    } catch { return $null }
}

function Set-RegString {
    param([string]$Path, [string]$Name, [string]$Value)
    $h = Get-Hive $Path
    $k = $h[0].CreateSubKey($h[1])
    try { $k.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String) } finally { $k.Close() }
}

function Remove-RegTree {
    param([string]$Path)
    if (-not (Test-RegKey $Path)) { return }
    $h = Get-Hive $Path
    $h[0].DeleteSubKeyTree($h[1], $false)
}

# ---------------------------------------------------------------------------
# current right-click menu: snapshot before, restore after.  The verbs live at
# the REAL HKCU path (Explorer has no scratch equivalent), so a verification run
# that installs to a temp folder would otherwise clobber a real installation's
# menu entries for good.
# ---------------------------------------------------------------------------

function Get-VerbState {
    param([string]$Verb)
    $key = $script:VerbRoot + '\' + $Verb
    if (-not (Test-RegKey $key)) { return $null }
    return [pscustomobject]@{
        Label   = [string](Get-RegValue $key '')
        Icon    = [string](Get-RegValue $key 'Icon')
        Command = [string](Get-RegValue ($key + '\command') '')
    }
}

function Save-VerbState {
    foreach ($verb in $script:VerbNames) { $script:VerbSnapshot[$verb] = Get-VerbState $verb }
}

function Restore-VerbState {
    foreach ($verb in $script:VerbNames) {
        $key = $script:VerbRoot + '\' + $verb
        if (-not $script:VerbSnapshot.ContainsKey($verb)) { continue }
        $state = $script:VerbSnapshot[$verb]
        Remove-RegTree $key
        if ($null -eq $state) { continue }
        if ($state.Label) { Set-RegString $key '' $state.Label }
        if ($state.Icon) { Set-RegString $key 'Icon' $state.Icon }
        if ($state.Command) { Set-RegString ($key + '\command') '' $state.Command }
    }
}

# ---------------------------------------------------------------------------
# bounded child processes (CONTRACTS D29: no INFINITE wait)
# ---------------------------------------------------------------------------

function Invoke-BoundedProcess {
    param(
        [string]$FilePath,
        [string]$Arguments,
        [int]$TimeoutSeconds,
        [string]$Tag,
        [string]$WorkingDirectory = ''
    )
    $out = Join-Path $script:WorkDir ('io-' + $Tag + '.out.txt')
    $err = Join-Path $script:WorkDir ('io-' + $Tag + '.err.txt')
    $startArgs = @{
        FilePath               = $FilePath
        ArgumentList           = $Arguments
        PassThru               = $true
        WindowStyle            = 'Hidden'
        RedirectStandardOutput = $out
        RedirectStandardError  = $err
    }
    if ($WorkingDirectory) { $startArgs['WorkingDirectory'] = $WorkingDirectory }
    $proc = Start-Process @startArgs
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch { }
        throw ('process timed out after ' + $TimeoutSeconds + 's (bounded wait): ' + $FilePath + ' ' + $Arguments)
    }
    $text = ''
    foreach ($f in @($out, $err)) {
        if (Test-Path -LiteralPath $f) { $text += [System.IO.File]::ReadAllText($f) + $script:LF }
    }
    return [pscustomobject]@{ ExitCode = $proc.ExitCode; Output = $text; Tag = $Tag }
}

function Invoke-PackageCmd {
    # Runs INSTALL.cmd / UNINSTALL.cmd from the package root with cmd.exe,
    # forwarding arguments.  The working directory is the package root, so the
    # batch name never has to be quoted.
    param([string]$CmdName, [string[]]$Arguments, [string]$Tag)
    $argText = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $line = '/d /c "' + $CmdName + ' ' + $argText + '"'
    return Invoke-BoundedProcess -FilePath $env:ComSpec -Arguments $line -TimeoutSeconds $ExeTimeoutSeconds -Tag $Tag -WorkingDirectory $script:PackageRoot
}

# ---------------------------------------------------------------------------
# resolve the archive
# ---------------------------------------------------------------------------

Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue

if (-not $ZipPath -and -not $ZipDir) { Stop-Usage 'pass -ZipPath <zip> or -ZipDir <folder>' }
if ($ZipPath -and $ZipDir) { Stop-Usage '-ZipPath and -ZipDir are mutually exclusive' }
if ($ZipDir) {
    if (-not (Test-Path -LiteralPath $ZipDir -PathType Container)) { Stop-Usage ('-ZipDir does not exist: ' + $ZipDir) }
    $found = @(Get-ChildItem -LiteralPath $ZipDir -Filter 'PrismArchive-*-win-x64.zip' -File)
    if ($found.Count -eq 0) {
        Stop-Skip ('no PrismArchive-*-win-x64.zip in ' + $ZipDir + ' -- the packaging step did not produce one (it skips when no 7z.dll is available)')
    }
    if ($found.Count -gt 1) {
        Stop-Usage ('expected exactly one PrismArchive-*-win-x64.zip in ' + $ZipDir + ', found ' + $found.Count)
    }
    $ZipPath = $found[0].FullName
}
if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) { Stop-Usage ('zip does not exist: ' + $ZipPath) }
$ZipPath = (Resolve-Path -LiteralPath $ZipPath).Path

# Admin-dependent half: per-user registry writes are the only privileged-looking
# step, and a locked-down account cannot do them. Say so and exit 77 (D1).
$classes = $null
try { $classes = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes', $true) } catch { $classes = $null }
if ($null -eq $classes) {
    Stop-Skip 'HKCU\Software\Classes is not writable by this account (policy or service account); the right-click verb half needs per-user registry access'
}
$classes.Close()

$script:WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ('prism-c68-verify-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
if (Test-Path -LiteralPath $script:WorkDir) { Stop-Usage ('unique temp directory already exists: ' + $script:WorkDir) }
New-Item -ItemType Directory -Force -Path $script:WorkDir | Out-Null
$script:InstallDir = Join-Path $script:WorkDir 'app'
$script:ScopeRoot = 'HKCU:\Software\PrismArchiveC68\' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$extractRoot = Join-Path $script:WorkDir 'unzipped'

$zipBytes = (Get-Item -LiteralPath $ZipPath).Length
$zipSha = Get-FileSha256 $ZipPath
Say ('archive: ' + $ZipPath + ' (' + $zipBytes + ' bytes, sha256 ' + $zipSha + ')')
$sidecar = $ZipPath + '.sha256'
if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
    $expectedSide = (@(([System.IO.File]::ReadAllText($sidecar)).Trim() -split '\s+')[0]).ToLowerInvariant()
    Assert-Equal 'archive sha256 matches its .sha256 sidecar (downloaded bytes)' $expectedSide $zipSha
}
Say ('work directory: ' + $script:WorkDir)
Say ('scratch registry scope: ' + $script:ScopeRoot)

try {
    # -----------------------------------------------------------------------
    # 1. extract + manifest bytes
    # -----------------------------------------------------------------------
    Save-VerbState
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $extractRoot)
    $roots = @(Get-ChildItem -LiteralPath $extractRoot -Directory)
    Assert-Equal 'zip expands to exactly one root folder' 1 $roots.Count
    if ($roots.Count -ne 1) { throw 'cannot continue without a single package root' }
    $script:PackageRoot = $roots[0].FullName
    Assert-True ('package root is named PrismArchive-<version>-win-x64 (' + $roots[0].Name + ')') ($roots[0].Name -match '^PrismArchive-[0-9]+\.[0-9]+\.[0-9]+-win-x64$')

    $manifestPath = Join-Path $script:PackageRoot 'package-manifest.json'
    Assert-True 'package-manifest.json exists' (Test-Path -LiteralPath $manifestPath -PathType Leaf)
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'package-manifest.json is missing' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-True ('manifest version matches the root folder (' + $manifest.version + ')') ($roots[0].Name.StartsWith('PrismArchive-' + $manifest.version + '-win-x64'))
    $files = @($manifest.files)
    Assert-Equal 'manifest fileCount matches its file list' ([int]$manifest.fileCount) $files.Count

    $diskFiles = @(Get-ChildItem -LiteralPath $script:PackageRoot -Recurse -File)
    Assert-Equal 'package contains the manifest plus every listed file (count)' ($files.Count + 1) $diskFiles.Count

    $required = @('PrismArchive.exe', 'PrismShellExt.dll', 'PrismShellExtClassic.dll', '7z.dll',
                  'install.ps1', 'uninstall.ps1', 'register-static-verbs.ps1',
                  'INSTALL.cmd', 'UNINSTALL.cmd', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.txt')
    $missing = @()
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $script:PackageRoot $name) -PathType Leaf)) { $missing += $name }
    }
    Assert-True ('package root has all ' + $required.Count + ' required files') ($missing.Count -eq 0) ($missing -join ', ')

    if ($Mutate -eq 'DropShellDll') {
        $victim = Join-Path $script:PackageRoot 'PrismShellExt.dll'
        Remove-Item -LiteralPath $victim -Force
        $script:MutationObserved = -not (Test-Path -LiteralPath $victim)
        if ($script:MutationObserved) { Say ('MUTATION FIXTURE: removed ' + $victim + ' after extraction; the manifest still lists it') 'warn' }
    }

    $bad = New-Object System.Collections.Generic.List[string]
    $total = [long]0
    foreach ($f in $files) {
        $full = Join-Path $script:PackageRoot ($f.path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { $bad.Add($f.path + ': missing'); continue }
        $size = (Get-Item -LiteralPath $full).Length
        if ($size -ne [long]$f.size) { $bad.Add($f.path + ': size ' + $size + ' != ' + $f.size); continue }
        $hash = Get-FileSha256 $full
        if ($hash -ne $f.sha256) { $bad.Add($f.path + ': sha256 ' + $hash + ' != ' + $f.sha256); continue }
        $total += $size
    }
    Assert-True ('every manifest entry matches bytes on disk (' + $files.Count + ' files, ' + $total + ' bytes)') ($bad.Count -eq 0) ($bad -join '; ')
    Assert-True '7z.dll is a real engine payload (> 500000 bytes)' ((Get-Item -LiteralPath (Join-Path $script:PackageRoot '7z.dll')).Length -gt 500000)

    # -----------------------------------------------------------------------
    # 2. install (quiet, scratch scope, unique install folder)
    # -----------------------------------------------------------------------
    $installArgs = @('-Quiet', '-InstallDir', $script:InstallDir, '-ScopeRoot', $script:ScopeRoot)
    $inst = Invoke-PackageCmd -CmdName 'INSTALL.cmd' -Arguments $installArgs -Tag 'install'
    if ($inst.ExitCode -ne 0 -or $inst.Output -notmatch 'INSTALL\.CMD: OK') {
        # The captured output is the only record of WHY install.ps1 failed; print
        # it on the failure path so the CI log carries the reason, not just the
        # exit code.
        Say ('INSTALL.cmd output (exit ' + $inst.ExitCode + '):' + $script:LF + $inst.Output) 'error'
    }
    Assert-Equal 'INSTALL.cmd -Quiet exits 0' 0 $inst.ExitCode
    Assert-True 'INSTALL.cmd prints its success marker (INSTALL.CMD: OK)' ($inst.Output -match 'INSTALL\.CMD: OK')
    $script:Installed = $true

    # -----------------------------------------------------------------------
    # 3. payload files are byte-identical
    # -----------------------------------------------------------------------
    foreach ($name in @('PrismArchive.exe', 'PrismShellExt.dll', 'PrismShellExtClassic.dll', '7z.dll')) {
        $pkgFile = Join-Path $script:PackageRoot $name
        $instFile = Join-Path $script:InstallDir $name
        $exists = Test-Path -LiteralPath $instFile -PathType Leaf
        Assert-True ('installed ' + $name + ' exists') $exists
        if (-not $exists) { continue }
        Assert-Equal ('installed ' + $name + ' byte size') ((Get-Item -LiteralPath $pkgFile).Length) ((Get-Item -LiteralPath $instFile).Length)
        Assert-Equal ('installed ' + $name + ' sha256') (Get-FileSha256 $pkgFile) (Get-FileSha256 $instFile)
    }

    # Enumerate with System.IO, not Get-ChildItem: under a short (8.3) TEMP path
    # Get-ChildItem returns long-form FullNames, so Substring(base.Length) slices
    # the relative path in the wrong place (the C63 defect).  Directory
    # enumeration builds paths from the base string, keeping the two forms equal.
    $pkgAssetsRoot = Join-Path $script:PackageRoot 'Assets'
    $instAssetsRoot = Join-Path $script:InstallDir 'Assets'
    $pkgAssetFiles = @([System.IO.Directory]::EnumerateFiles($pkgAssetsRoot, '*', [System.IO.SearchOption]::AllDirectories))
    $instAssetFiles = @([System.IO.Directory]::EnumerateFiles($instAssetsRoot, '*', [System.IO.SearchOption]::AllDirectories))
    Assert-Equal 'installed Assets file count matches the package' $pkgAssetFiles.Count $instAssetFiles.Count
    $assetDiff = @()
    foreach ($full in $pkgAssetFiles) {
        $rel = $full.Substring($pkgAssetsRoot.Length).TrimStart('\', '/')
        $instAsset = Join-Path $instAssetsRoot $rel
        if (-not (Test-Path -LiteralPath $instAsset -PathType Leaf)) { $assetDiff += ($rel + ': missing'); continue }
        if ((Get-FileSha256 $full) -ne (Get-FileSha256 $instAsset)) { $assetDiff += ($rel + ': bytes differ') }
    }
    Assert-True ('installed Assets bytes match the package (' + $pkgAssetFiles.Count + ' files)') ($assetDiff.Count -eq 0) ($assetDiff -join '; ')
    Assert-True 'install marker records the install folder in the scratch scope' ((Test-RegKey ($script:ScopeRoot + '\PrismArchive')) -and ([string](Get-RegValue ($script:ScopeRoot + '\PrismArchive') 'InstallDir') -eq $script:InstallDir))

    # -----------------------------------------------------------------------
    # 4. settings.json is writable
    # -----------------------------------------------------------------------
    $settingsDir = Join-Path $env:LOCALAPPDATA 'PrismArchive'
    $createdSettingsDir = -not (Test-Path -LiteralPath $settingsDir)
    if ($createdSettingsDir) { New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null }
    $probe = Join-Path $settingsDir ('settings.json.c68probe-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $probeBytes = New-ProbeBytes 4096
    [System.IO.File]::WriteAllBytes($probe, $probeBytes)
    $readBack = [System.IO.File]::ReadAllBytes($probe)
    Assert-Equal 'settings directory accepts a 4096-byte settings.json write (read back)' 4096 $readBack.Length
    Assert-Equal 'settings write probe bytes round-trip' (Get-BytesSha256 $probeBytes) (Get-FileSha256 $probe)
    Remove-Item -LiteralPath $probe -Force
    $settingsFile = Join-Path $settingsDir 'settings.json'
    if (Test-Path -LiteralPath $settingsFile -PathType Leaf) {
        $stream = [System.IO.File]::Open($settingsFile, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
        try { $len = $stream.Length } finally { $stream.Dispose() }
        Add-Check
        Say ('existing settings.json opens read-write (' + $len + ' bytes)') 'pass'
    } else {
        # No settings.json yet: prove the file itself can be created and read
        # back at the exact product path, then remove it (the app would create it
        # on its first normal exit; this asserts the capability, not the timing).
        [System.IO.File]::WriteAllBytes($settingsFile, $probeBytes)
        $fileReadBack = [System.IO.File]::ReadAllBytes($settingsFile)
        Assert-Equal 'settings.json (absent before) is created and reads back 4096 bytes' 4096 $fileReadBack.Length
        Assert-Equal 'settings.json bytes round-trip' (Get-BytesSha256 $probeBytes) (Get-FileSha256 $settingsFile)
        Remove-Item -LiteralPath $settingsFile -Force
    }
    if ($createdSettingsDir) {
        try { if (@(Get-ChildItem -LiteralPath $settingsDir -Force).Count -eq 0) { Remove-Item -LiteralPath $settingsDir -Force } } catch { }
    }

    # -----------------------------------------------------------------------
    # 5. right-click verbs point at the installed exe
    # -----------------------------------------------------------------------
    $exe = Join-Path $script:InstallDir 'PrismArchive.exe'
    $extractKey = $script:VerbRoot + '\PrismArchiveExtract'
    $openKey = $script:VerbRoot + '\PrismArchiveOpen'

    if ($Mutate -eq 'SkipVerbs') {
        Remove-RegTree $extractKey
        $script:MutationObserved = -not (Test-RegKey $extractKey)
        if ($script:MutationObserved) { Say ('MUTATION FIXTURE: removed ' + $extractKey + ' after the install registered it') 'warn' }
    }

    Assert-True 'right-click key PrismArchiveExtract exists' (Test-RegKey $extractKey)
    Assert-True 'right-click key PrismArchiveOpen exists' (Test-RegKey $openKey)
    if (Test-RegKey $extractKey) {
        $expectedExtract = '"' + $exe + '" --extract-here "%1"'
        Assert-Equal 'extract verb command is the installed exe + --extract-here' $expectedExtract ([string](Get-RegValue ($extractKey + '\command') ''))
        $label = [string](Get-RegValue $extractKey '')
        Assert-True 'extract verb has a user-visible label' ($label.Length -gt 0 -and $label.Contains('PrismArchive'))
        Assert-Equal 'extract verb icon is the installed exe' $exe ([string](Get-RegValue $extractKey 'Icon'))
    }
    if (Test-RegKey $openKey) {
        $expectedOpen = '"' + $exe + '" "%1"'
        Assert-Equal 'open verb command is the installed exe + "%1"' $expectedOpen ([string](Get-RegValue ($openKey + '\command') ''))
        $openLabel = [string](Get-RegValue $openKey '')
        Assert-True 'open verb has a user-visible label' ($openLabel.Length -gt 0 -and $openLabel.Contains('PrismArchive'))
    }

    # -----------------------------------------------------------------------
    # 6. the installed app extracts a real archive, with its bundled engine
    # -----------------------------------------------------------------------
    $probeDir = Join-Path $script:WorkDir 'extract-probe'
    New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
    $sourceFile = Join-Path $probeDir 'payload.txt'
    [System.IO.File]::WriteAllBytes($sourceFile, (New-ProbeBytes 4096))
    $sourceHash = Get-FileSha256 $sourceFile

    $savedEngineEnv = [string]$env:PRISM_7Z_DLL
    try {
        # Clear the override so the engine has to come from beside the installed
        # exe -- i.e. the 7z.dll this package shipped.
        $env:PRISM_7Z_DLL = ''
        $compressed = Invoke-BoundedProcess -FilePath $exe -Arguments ('--compress-7z "' + $sourceFile + '"') -TimeoutSeconds $ExeTimeoutSeconds -Tag 'compress'
        Assert-Equal 'installed app --compress-7z exits 0' 0 $compressed.ExitCode
        $archive = Join-Path $probeDir 'payload.7z'
        Assert-True '--compress-7z produced payload.7z (bytes > 0)' ((Test-Path -LiteralPath $archive -PathType Leaf) -and ((Get-Item -LiteralPath $archive).Length -gt 0))
        Remove-Item -LiteralPath $sourceFile -Force
        $extracted = Invoke-BoundedProcess -FilePath $exe -Arguments ('--extract-here "' + $archive + '"') -TimeoutSeconds $ExeTimeoutSeconds -Tag 'extract'
        Assert-Equal 'installed app --extract-here exits 0' 0 $extracted.ExitCode
        $restored = Test-Path -LiteralPath $sourceFile -PathType Leaf
        Assert-True 'extracted payload.txt exists' $restored
        if ($restored) {
            Assert-Equal 'extracted payload byte count' 4096 (Get-Item -LiteralPath $sourceFile).Length
            Assert-Equal 'extracted payload sha256' $sourceHash (Get-FileSha256 $sourceFile)
        }
    } finally {
        $env:PRISM_7Z_DLL = $savedEngineEnv
    }

    # -----------------------------------------------------------------------
    # 7. uninstall removes verbs, files, marker and the uninstall entry
    # -----------------------------------------------------------------------
    $unArgs = @('-Quiet', '-InstallDir', $script:InstallDir, '-ScopeRoot', $script:ScopeRoot)
    $un = Invoke-PackageCmd -CmdName 'UNINSTALL.cmd' -Arguments $unArgs -Tag 'uninstall'
    Assert-Equal 'UNINSTALL.cmd -Quiet exits 0' 0 $un.ExitCode
    Assert-True 'UNINSTALL.cmd prints its success marker (UNINSTALL.CMD: OK)' ($un.Output -match 'UNINSTALL\.CMD: OK')
    $script:Uninstalled = $true

    Assert-True 'PrismArchiveExtract key is gone after uninstall' (-not (Test-RegKey $extractKey))
    Assert-True 'PrismArchiveOpen key is gone after uninstall' (-not (Test-RegKey $openKey))
    Assert-True 'install directory is gone after uninstall' (-not (Test-Path -LiteralPath $script:InstallDir))
    Assert-True 'install marker is gone after uninstall' (-not (Test-RegKey ($script:ScopeRoot + '\PrismArchive')))
    Assert-True 'uninstall entry is gone after uninstall' (-not (Test-RegKey ($script:ScopeRoot + '\Microsoft\Windows\CurrentVersion\Uninstall\PrismArchive')))
} catch {
    Say ('harness threw: ' + $_.Exception.Message) 'error'
    Say ([string]$_.ScriptStackTrace) 'error'
    Add-Failure ('harness threw: ' + $_.Exception.Message)
} finally {
    if ($script:Installed -and -not $script:Uninstalled) {
        Say 'CLEANUP: uninstall after an aborted run ...' 'warn'
        try {
            $cleanup = Invoke-PackageCmd -CmdName 'UNINSTALL.cmd' -Arguments @('-Quiet', '-InstallDir', $script:InstallDir, '-ScopeRoot', $script:ScopeRoot) -Tag 'cleanup-uninstall'
            if ($cleanup.ExitCode -eq 0) { $script:Uninstalled = $true }
            else { Say ('CLEANUP: uninstall exited ' + $cleanup.ExitCode) 'warn' }
        } catch {
            Say ('CLEANUP: uninstall threw: ' + $_.Exception.Message) 'error'
        }
    }
    try { Restore-VerbState } catch { Say ('CLEANUP: verb restore threw: ' + $_.Exception.Message) 'error' }
    try { if (Test-RegKey $script:ScopeRoot) { Remove-RegTree $script:ScopeRoot } } catch { }
    if ($KeepTemp) {
        Say ('temp kept: ' + $script:WorkDir) 'warn'
    } else {
        try { Remove-Item -LiteralPath $script:WorkDir -Recurse -Force -ErrorAction SilentlyContinue } catch { }
    }
}

# ---------------------------------------------------------------------------
# verdict
# ---------------------------------------------------------------------------

if ($script:Mutation) {
    if (-not $script:MutationObserved) {
        Say ('PACKAGE VERIFY(MUTATION ' + $script:Mutation + '): NOT INJECTED -- the state did not change; a red result would be meaningless') 'verdict'
        exit 1
    }
    if ($script:Failures.Count -gt 0) {
        Say ('PACKAGE VERIFY(MUTATION ' + $script:Mutation + '): CAUGHT (' + $script:Failures.Count + ' assertion(s) went red; injection observed)') 'verdict'
        exit 0
    }
    Say ('PACKAGE VERIFY(MUTATION ' + $script:Mutation + '): NOT CAUGHT -- the guard is blind to the injected defect') 'verdict'
    exit 1
}

if ($script:Failures.Count -gt 0) {
    Say ('PACKAGE VERIFY: FAIL (' + $script:Failures.Count + ' of ' + $script:Checks + ' checks; ' + $script:Skips.Count + ' explicit skip(s))') 'error'
    exit 1
}

Say ('PACKAGE VERIFY: PASS (' + $script:Checks + ' checks, ' + $script:Skips.Count + ' explicit skip(s); archive ' + (Split-Path -Leaf $ZipPath) + ' sha256 ' + $zipSha + ')') 'verdict'
exit 0
