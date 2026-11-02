#requires -Version 5.1
<#
.SYNOPSIS
  PrismArchive per-user installer: files, file associations, uninstall entry,
  engine discovery. No administrator rights, no machine-wide writes.

.DESCRIPTION
  Everything this script writes lives under HKCU and %LOCALAPPDATA%:
    <ScopeRoot>\Classes                            file associations (HKCU\Software\Classes)
    <ScopeRoot>\PrismArchive                       install marker + association backup
    <ScopeRoot>\Microsoft\Windows\CurrentVersion\Uninstall\PrismArchive
    <ScopeRoot>\Environment                        PRISM_7Z_DLL (user environment)
  ScopeRoot defaults to HKCU:\Software. The self-test overrides it with a scratch
  key so the round trip never touches real associations.

  We do NOT bundle 7z.dll: the 7-Zip engine is LGPL and the blueprint (ch.2) keeps
  it out of the tree. The installer finds the user's copy and records its path in
  the per-user environment variable the engine already honours (Src/App/Main.cpp
  resolution order: PRISM_7Z_DLL, then the standard install paths).

.PARAMETER DryRun
  Print every registry/file operation that would happen, change nothing.

.PARAMETER Portable
  Copy the payload next to any directory you like, write no registry key at all,
  and leave a marker file with the engine path. Nothing to uninstall.

.PARAMETER SelfTest
  Full install/idempotency/uninstall round trip inside a scratch registry key and
  a scratch directory, using a synthetic payload, so it runs before any build.
  Prints SELFTEST: PASS or SELFTEST: FAIL and exits non-zero on failure.

.PARAMETER Mutate
  Negative control for the self-test (CONTRACTS D3). Values:
    SkipDefaultIcon | NoBackup | NoRestore | UnquotedVerb | Rebackup
  Each one injects a real defect into the code path, and the self-test must then
  FAIL. A mutation that the suite still passes means the suite is not a guard.

.EXAMPLE
  . E:\wg\env.ps1 -Card c3
  pwsh -File tools\install.ps1 -DryRun
  pwsh -File tools\install.ps1
  pwsh -File tools\install.ps1 -Verify
  pwsh -File tools\install.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [string]$SourceDir = '',
    [string]$InstallDir = '',
    [string]$SevenZipDll = '',
    [string[]]$Extensions = @(),
    [string]$ScopeRoot = 'HKCU:\Software',
    [switch]$DryRun,
    [switch]$Portable,
    [switch]$NoAssociations,
    [switch]$SelfTest,
    [string]$Mutate = '',
    [switch]$Verify,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# state + logging
# ---------------------------------------------------------------------------

$script:DryRun = [bool]$DryRun
$script:Quiet = [bool]$Quiet
$script:Mutate = $Mutate
$script:Wrote = 0
$script:Kept = 0
$script:Planned = 0
$script:Warnings = New-Object System.Collections.Generic.List[string]

function Say {
    param([string]$Text, [string]$Level = 'info')
    if ($script:Quiet -and ($Level -eq 'info' -or $Level -eq 'done' -or $Level -eq 'same')) { return }
    $prefix = switch ($Level) {
        'warn' { 'WARN  ' }
        'error' { 'ERROR ' }
        'plan' { if ($script:DryRun) { '[dry-run] ' } else { '' } }
        'same' { '[same] ' }
        'done' { '[write] ' }
        default { '' }
    }
    Write-Host ($prefix + $Text)
}

function Warn {
    param([string]$Text)
    $script:Warnings.Add($Text)
    Say $Text 'warn'
}

$script:ExitCode = 0
function Fail {
    param([string]$Text)
    Say $Text 'error'
    $script:ExitCode = 1
}

$ValidMutations = @('SkipDefaultIcon', 'NoBackup', 'NoRestore', 'UnquotedVerb', 'Rebackup', 'EnvUnderSoftware')
if ($script:Mutate -and ($ValidMutations -notcontains $script:Mutate)) {
    Write-Host ('ERROR unknown mutation: ' + $script:Mutate + ' (valid: ' + ($ValidMutations -join ', ') + ')')
    exit 2
}
if ($script:Mutate) { Say ('MUTATION ACTIVE: ' + $script:Mutate) 'warn' }

# ---------------------------------------------------------------------------
# registry primitives (.NET API: exact value kinds, no provider translation)
# ---------------------------------------------------------------------------

function Get-Hive {
    param([string]$Path)
    if ($Path -like 'HKCU:\*') { return @([Microsoft.Win32.Registry]::CurrentUser, $Path.Substring(6)) }
    if ($Path -like 'HKLM:\*') { return @([Microsoft.Win32.Registry]::LocalMachine, $Path.Substring(6)) }
    throw ('unsupported registry path: ' + $Path)
}

function Test-RegKey {
    param([string]$Path)
    $h = Get-Hive $Path
    $k = $h[0].OpenSubKey($h[1])
    if ($null -eq $k) { return $false }
    $k.Close()
    return $true
}

function Open-RegKey {
    <# Never let a read path depend on write access: on some runtimes
       OpenSubKey(name, writable:true) throws SecurityException ("requested
       registry access is not allowed") for a key that does not exist yet, which
       is exactly the state a first install reads from. CI found this on the
       Windows runner while the dev box silently returned $null. #>
    param([string]$Path, [switch]$Create)
    $h = Get-Hive $Path
    if ($Create) { return $h[0].CreateSubKey($h[1], $true) }
    try { return $h[0].OpenSubKey($h[1], $true) } catch { }
    try { return $h[0].OpenSubKey($h[1]) } catch { }
    return $null
}

function Get-RegValue {
    param([string]$Path, [string]$Name = '')
    $k = Open-RegKey $Path
    if ($null -eq $k) { return $null }
    try {
        return $k.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally { $k.Close() }
}

function Get-RegKind {
    param([string]$Path, [string]$Name = '')
    $k = Open-RegKey $Path
    if ($null -eq $k) { return $null }
    try {
        if (-not ($k.GetValueNames() -contains $Name)) { return $null }
        return $k.GetValueKind($Name)
    } finally { $k.Close() }
}

function Test-SameValue {
    param($Current, $Desired, [string]$Kind)
    if ($Kind -eq 'None') {
        # An empty REG_NONE comes back as $null in one code path and byte[0] in
        # another (observed on pwsh 7.6); both mean "already the value we write".
        if ($null -eq $Current) { return $true }
        if ($Current -is [byte[]]) { return ($Current.Length -eq 0) }
        return $false
    }
    if ($null -eq $Current) { return $false }
    if ($Kind -eq 'DWord' -or $Kind -eq 'QWord') { return ([int64]$Current -eq [int64]$Desired) }
    return ([string]$Current -eq [string]$Desired)
}

function Set-RegValue {
    param([string]$Path, [string]$Name = '', $Value, [string]$Kind = 'String')
    $current = Get-RegValue $Path $Name
    $currentKind = Get-RegKind $Path $Name
    $exists = ($null -ne $currentKind)
    if ($exists -and $currentKind -eq $Kind -and (Test-SameValue $current $Value $Kind)) {
        $script:Kept++
        Say ('value already correct: ' + $Path + ' [' + $Name + ']') 'same'
        return 'same'
    }
    $shown = if ($Kind -eq 'None') { '<none>' } else { [string]$Value }
    $label = if ($Name -eq '') { 'Default' } else { $Name }
    $what = 'set ' + $Path + ' [' + $label + '] = ' + $shown + ' (' + $Kind + ')'
    if ($script:DryRun) {
        $script:Planned++
        Say $what 'plan'
        return 'would'
    }
    $k = Open-RegKey $Path -Create
    if ($null -eq $k) { throw ('cannot create key ' + $Path) }
    try {
        switch ($Kind) {
            'None' { $k.SetValue($Name, (New-Object byte[] 0), [Microsoft.Win32.RegistryValueKind]::None) }
            'DWord' { $k.SetValue($Name, [int]$Value, [Microsoft.Win32.RegistryValueKind]::DWord) }
            default { $k.SetValue($Name, [string]$Value, [Microsoft.Win32.RegistryValueKind]::String) }
        }
    } finally { $k.Close() }
    $script:Wrote++
    Say $what 'done'
    return 'changed'
}

function Remove-RegTree {
    param([string]$Path)
    if (-not (Test-RegKey $Path)) { return }
    if ($script:DryRun) { Say ('delete key tree ' + $Path) 'plan'; return }
    $h = Get-Hive $Path
    $h[0].DeleteSubKeyTree($h[1], $false)
    $script:Wrote++
    Say ('deleted key tree ' + $Path) 'done'
}

function Remove-RegValue {
    param([string]$Path, [string]$Name = '')
    if (-not (Test-RegKey $Path)) { return }
    $k = Open-RegKey $Path
    try {
        if (-not ($k.GetValueNames() -contains $Name)) { return }
        if ($script:DryRun) { Say ('delete value ' + $Path + ' [' + $Name + ']') 'plan'; return }
        $k.DeleteValue($Name, $false)
        $script:Wrote++
        Say ('deleted value ' + $Path + ' [' + $Name + ']') 'done'
    } finally { $k.Close() }
}

function Get-RegTree {
    <# Sorted "path|name|kind|value" lines for a subtree: the idempotency snapshot. #>
    param([string]$Root)
    $lines = New-Object System.Collections.Generic.List[string]
    $h = Get-Hive $Root
    $key = $h[0].OpenSubKey($h[1])
    if ($null -eq $key) { return @() }
    $walk = $null
    $walk = {
        param($k, [string]$rel)
        foreach ($n in @($k.GetValueNames() | Sort-Object)) {
            $v = $k.GetValue($n, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $kind = [string]$k.GetValueKind($n)
            $text = if ($null -eq $v) { '<null>' } elseif ($v -is [byte[]]) { 'hex:' + [Convert]::ToBase64String($v) } else { [string]$v }
            $lines.Add($rel + '|' + $n + '|' + $kind + '|' + $text)
        }
        foreach ($sub in @($k.GetSubKeyNames() | Sort-Object)) {
            $lines.Add($rel + '\' + $sub + '|KEY||')
            $child = $k.OpenSubKey($sub)
            & $walk $child ($rel + '\' + $sub)
            $child.Close()
        }
    }
    & $walk $key ''
    $key.Close()
    return $lines.ToArray()
}

# ---------------------------------------------------------------------------
# paths derived from ScopeRoot
# ---------------------------------------------------------------------------

$script:ScopeRoot = $ScopeRoot
$script:ClassesRoot = $ScopeRoot + '\Classes'
$script:AppRoot = $ScopeRoot + '\PrismArchive'
$script:UninstallRoot = $ScopeRoot + '\Microsoft\Windows\CurrentVersion\Uninstall'
$script:UninstallKey = $script:UninstallRoot + '\PrismArchive'
function Get-EnvRootPath {
    <# The user environment is a HIVE-ROOT key: HKCU\Environment, not
       HKCU\Software\Environment. A scratch scope (self-test) keeps its own copy
       inside the scope so the round trip never touches the real value.
       This cost a real bug once: the engine override was written to
       HKCU\Software\Environment, where nothing reads it, and the self-test could
       not see it because it only exercised its own scratch mapping. The self-test
       now asserts the real-scope mapping directly, and mutation
       EnvUnderSoftware is its negative control. #>
    param([string]$Scope)
    if ($script:Mutate -eq 'EnvUnderSoftware') { return $Scope + '\Environment' }
    if ($Scope -eq 'HKCU:\Software') { return 'HKCU:\Environment' }
    return $Scope + '\Environment'
}

$script:EnvRoot = Get-EnvRootPath $ScopeRoot
$script:BackupRoot = $script:AppRoot + '\AssocBackup'

$script:DefaultExtensions = @(
    '.7z', '.zip', '.rar', '.tar', '.gz', '.tgz', '.bz2', '.tbz2', '.xz', '.txz',
    '.zst', '.lz', '.cab', '.iso', '.wim', '.arj', '.lzh', '.cpio', '.rpm', '.deb',
    '.001', '.zipx'
)

$script:Friendly = @{
    '.7z' = '7z Archive'
    '.zip' = 'ZIP Archive'
    '.rar' = 'RAR Archive'
    '.tar' = 'TAR Archive'
    '.gz' = 'Gzip Archive'
    '.tgz' = 'Gzip Archive'
    '.bz2' = 'Bzip2 Archive'
    '.tbz2' = 'Bzip2 Archive'
    '.xz' = 'XZ Archive'
    '.txz' = 'XZ Archive'
    '.zst' = 'Zstandard Archive'
    '.lz' = 'Lzip Archive'
    '.cab' = 'Cabinet Archive'
    '.iso' = 'Disc Image'
    '.wim' = 'Windows Imaging Format'
    '.arj' = 'ARJ Archive'
    '.lzh' = 'LHA Archive'
    '.cpio' = 'CPIO Archive'
    '.rpm' = 'RPM Package'
    '.deb' = 'Debian Package'
    '.001' = 'Split Archive'
    '.zipx' = 'ZIPX Archive'
}

function Get-ProgId {
    param([string]$Ext)
    return 'PrismArchive' + $Ext
}

function Get-AssocDescription {
    param([string]$Ext)
    if ($script:Friendly.ContainsKey($Ext)) { return $script:Friendly[$Ext] }
    return ($Ext.TrimStart('.').ToUpperInvariant() + ' Archive')
}

# ---------------------------------------------------------------------------
# payload discovery
# ---------------------------------------------------------------------------

function Find-Engine {
    <# 7z.dll is never shipped: find the user's 7-Zip, in the order the engine
       itself would (PRISM_7Z_DLL first, then real installs, then PATH). #>
    param([string]$Explicit, [string]$UserEnvValue)

    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit -PathType Leaf) { return (Resolve-Path -LiteralPath $Explicit).Path }
        Warn ('explicit -SevenZipDll does not exist: ' + $Explicit)
    }
    if ($UserEnvValue -and (Test-Path -LiteralPath $UserEnvValue -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $UserEnvValue).Path
    }
    $candidates = New-Object System.Collections.Generic.List[string]
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
    $pf = $env:ProgramFiles
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $pw64 = [Environment]::GetEnvironmentVariable('ProgramW6432')
    foreach ($dir in @($pf, $pf86, $pw64, (Join-Path $env:LOCALAPPDATA 'Programs\7-Zip'), 'E:\DevTools\7-Zip')) {
        if ($dir) { $candidates.Add((Join-Path $dir '7z.dll')) }
    }
    $cmd = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add((Join-Path (Split-Path -Parent $cmd.Source) '7z.dll')) }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return ''
}

function Find-SourceDir {
    param([string]$Explicit)
    $tried = New-Object System.Collections.Generic.List[string]
    if ($Explicit) {
        # A trailing separator before the closing quote escapes that quote at the
        # cmd/PowerShell boundary ("...\" -SevenZipDll ..."), so the rest of the
        # command line ends up inside the path and Test-Path throws
        # "Illegal characters in path". Trim quotes and trailing separators here
        # rather than trusting every caller to quote correctly.
        $p = $Explicit.Trim().Trim('"')
        while ($p.Length -gt 3 -and ($p.EndsWith('\') -or $p.EndsWith('/'))) {
            $p = $p.Substring(0, $p.Length - 1)
        }
        if (Test-Path -LiteralPath $p -PathType Leaf) { $p = Split-Path -Parent $p }
        $exe = Join-Path $p 'PrismArchive.exe'
        if (Test-Path -LiteralPath $exe -PathType Leaf) { return (Resolve-Path -LiteralPath $p).Path }
        throw ('PrismArchive.exe not found in -SourceDir ' + $p)
    }
    $repo = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        $env:PRISM_BUILD_DIR,
        'E:\build\wt-c3\Src\PrismWin',
        (Join-Path $repo 'build\Src\PrismWin'),
        (Join-Path $repo 'build'),
        (Join-Path $repo 'out\Src\PrismWin')
    )
    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $tried.Add($c)
        if (Test-Path -LiteralPath (Join-Path $c 'PrismArchive.exe') -PathType Leaf) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    throw ('PrismArchive.exe not found. Build first (cmake --build <dir>) or pass -SourceDir. Tried: ' + ($tried -join '; '))
}

function Find-AssetsDir {
    param([string]$SourceDir)
    $candidates = @(
        (Join-Path $SourceDir 'Assets'),
        (Join-Path $SourceDir '..\..\Assets'),
        (Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Container) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return ''
}

# ---------------------------------------------------------------------------
# file deployment
# ---------------------------------------------------------------------------

function Copy-FileStable {
    param([string]$Source, [string]$Destination, [switch]$Required)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        if ($Required) { throw ('missing payload file: ' + $Source) }
        Warn ('payload file missing, skipped: ' + $Source)
        return
    }
    if ($script:DryRun) {
        $script:Planned++
        Say ('copy ' + $Source + ' -> ' + $Destination) 'plan'
        return
    }
    $dir = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    if (Test-Path -LiteralPath $Destination) {
        $same = ((Get-Item -LiteralPath $Destination).Length -eq (Get-Item -LiteralPath $Source).Length) -and
                ((Get-FileHash -LiteralPath $Destination).Hash -eq (Get-FileHash -LiteralPath $Source).Hash)
        if ($same) {
            $script:Kept++
            Say ('unchanged ' + $Destination) 'same'
            return
        }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $script:Wrote++
    Say ('copied ' + $Destination) 'done'
}

function Deploy-Payload {
    param([string]$SourceDir, [string]$TargetDir, [string]$AssetsDir)

    Copy-FileStable (Join-Path $SourceDir 'PrismArchive.exe') (Join-Path $TargetDir 'PrismArchive.exe') -Required
    foreach ($dll in @('PrismShellExt.dll', 'PrismShellExtClassic.dll')) {
        Copy-FileStable (Join-Path $SourceDir $dll) (Join-Path $TargetDir $dll)
    }
    if ($AssetsDir) {
        # Enumerate with System.IO instead of Get-ChildItem: on CI the payload sits
        # under a short (8.3) TEMP path, and Get-ChildItem returns long-form
        # FullNames, so Substring(base.Length) sliced "Assets\Icon\prism.ico" into
        # "Assets\ets\Icon\prism.ico". Directory.EnumerateFiles returns paths built
        # from the base string, which keeps the two forms identical by construction.
        $assetFiles = [System.IO.Directory]::EnumerateFiles(
            $AssetsDir, '*', [System.IO.SearchOption]::AllDirectories)
        foreach ($full in $assetFiles) {
            if ([System.IO.Path]::GetExtension($full) -eq '.py') { continue }
            $rel = $full.Substring($AssetsDir.Length).TrimStart('\', '/')
            Copy-FileStable $full (Join-Path (Join-Path $TargetDir 'Assets') $rel)
        }
    } else {
        Warn 'Assets directory not found; the association icon still resolves because the .ico is compiled into the exe'
    }
    Copy-FileStable (Join-Path $PSScriptRoot 'uninstall.ps1') (Join-Path $TargetDir 'uninstall.ps1') -Required
}

# ---------------------------------------------------------------------------
# file associations
# ---------------------------------------------------------------------------

function Install-Associations {
    param([string]$ExePath, [string]$IconPath, [string[]]$Exts)

    $exeName = Split-Path -Leaf $ExePath
    $command = '"' + $ExePath + '" '
    if ($script:Mutate -eq 'UnquotedVerb') {
        $command += '%1'
    } else {
        $command += '"%1"'
    }
    $icon = $IconPath + ',0'

    # Application registration: one entry, every supported type. This is what
    # the Open-with list and the Default-apps page read.
    $appKey = $script:ClassesRoot + '\Applications\' + $exeName
    Set-RegValue $appKey '' 'PrismArchive' | Out-Null
    Set-RegValue $appKey 'FriendlyAppName' 'PrismArchive' | Out-Null
    Set-RegValue $appKey 'ApplicationCompany' 'PrismArchive contributors' | Out-Null
    if ($script:Mutate -ne 'SkipDefaultIcon') {
        Set-RegValue ($appKey + '\DefaultIcon') '' $icon | Out-Null
    }
    Set-RegValue ($appKey + '\shell\open\command') '' $command | Out-Null

    foreach ($ext in $Exts) {
        $progId = Get-ProgId $ext
        $extKey = $script:ClassesRoot + '\' + $ext
        $progKey = $script:ClassesRoot + '\' + $progId

        # Capture whatever answered for this extension BEFORE we take it over, so
        # uninstall can put it back. Capture once: re-capturing on every install
        # would overwrite the original with our own ProgID (that is the Rebackup
        # mutation, and the self-test must catch it).
        $backupKey = $script:BackupRoot + '\' + $ext
        if ($script:Mutate -ne 'NoBackup') {
            $recapture = ($script:Mutate -eq 'Rebackup') -or (-not (Test-RegKey $backupKey))
            if ($recapture) {
                $previous = Get-RegValue $extKey ''
                $hadValue = ($null -ne (Get-RegKind $extKey ''))
                Set-RegValue $backupKey 'Present' ([int]($(if ($hadValue) { 1 } else { 0 }))) 'DWord' | Out-Null
                Set-RegValue $backupKey 'Previous' ([string]$previous) | Out-Null
            }
        }

        Set-RegValue $extKey '' $progId | Out-Null
        Set-RegValue ($extKey + '\OpenWithProgids') $progId $null 'None' | Out-Null
        Set-RegValue ($appKey + '\SupportedTypes') $ext '' | Out-Null

        Set-RegValue $progKey '' (Get-AssocDescription $ext) | Out-Null
        if ($script:Mutate -ne 'SkipDefaultIcon') {
            Set-RegValue ($progKey + '\DefaultIcon') '' $icon | Out-Null
        }
        Set-RegValue ($progKey + '\shell\open\command') '' $command | Out-Null
        Set-RegValue ($progKey + '\shell\open') 'MultiSelectModel' 'Player' | Out-Null
        Set-RegValue ($progKey + '\shell\open') 'FriendlyAppName' 'PrismArchive' | Out-Null
    }
}

function Get-UserChoice {
    <# Windows protects the user's default handler for an extension with a hashed
       UserChoice/UserChoiceLatest record. That record outranks HKCU\Software\Classes
       and cannot be rewritten by a non-elevated installer -- by design. Both shapes
       must be read: on current Win11 the ProgID lives in a *subkey*
       (UserChoiceLatest\ProgId\ProgId), on older builds in UserChoice as a value. #>
    param([string]$Ext)
    $base = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\' + $Ext
    $latest = Get-RegValue ($base + '\UserChoiceLatest\ProgId') 'ProgId'
    if ($latest) { return [pscustomobject]@{ Pinned = $true; ProgId = [string]$latest; Source = 'UserChoiceLatest' } }
    $legacy = Get-RegValue ($base + '\UserChoice') 'ProgId'
    if ($legacy) { return [pscustomobject]@{ Pinned = $true; ProgId = [string]$legacy; Source = 'UserChoice' } }
    return [pscustomobject]@{ Pinned = $false; ProgId = ''; Source = '' }
}

function Report-UserChoice {
    param([string[]]$Exts)
    foreach ($ext in $Exts) {
        $choice = Get-UserChoice $ext
        if ($choice.Pinned) {
            Warn ($ext + ' is pinned by the Windows user choice (' + $choice.ProgId + ', ' + $choice.Source + '). A hash-protected default cannot be overridden by HKCU\Software\Classes; finish the switch in Settings > Default apps, or use Open with -> PrismArchive once. Our ProgID and command are registered either way.')
        }
    }
}

# ---------------------------------------------------------------------------
# engine + uninstall entry
# ---------------------------------------------------------------------------

function Install-EngineEnv {
    param([string]$Dll)
    if (-not $Dll) {
        Warn 'No 7z.dll found. PrismArchive needs 7-Zip installed or PRISM_7Z_DLL pointing at 7z.dll. Install 7-Zip, then re-run this installer (it does not ship the engine: LGPL, blueprint ch.2).'
        Set-RegValue $script:AppRoot 'Engine' '' | Out-Null
        return
    }
    $previous = Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL'
    $hadPrevious = ($null -ne (Get-RegKind $script:EnvRoot 'PRISM_7Z_DLL'))
    if ([string]$previous -ne $Dll) {
        Set-RegValue $script:AppRoot 'EnginePrevious' ([string]$previous) | Out-Null
        Set-RegValue $script:AppRoot 'EngineHadPrevious' ([int]($(if ($hadPrevious) { 1 } else { 0 }))) 'DWord' | Out-Null
    }
    Set-RegValue $script:AppRoot 'Engine' $Dll | Out-Null
    Set-RegValue $script:EnvRoot 'PRISM_7Z_DLL' $Dll | Out-Null
    Say ('engine: ' + $Dll) 'info'
}

function Get-ExeVersion {
    param([string]$ExePath)
    $version = '0.1.0.0'
    try {
        $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($ExePath)
        if ($info.FileVersion) { $version = $info.FileVersion.Trim() }
    } catch { }
    return $version
}

function Install-UninstallEntry {
    param([string]$InstallDir, [string]$ExePath)
    $version = Get-ExeVersion $ExePath
    $sizeKb = 0
    Get-ChildItem -LiteralPath $InstallDir -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object { $sizeKb += [math]::Ceiling($_.Length / 1024.0) }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $uninstallString = '"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $InstallDir 'uninstall.ps1') + '"'
    Set-RegValue $script:UninstallKey 'DisplayName' 'PrismArchive' | Out-Null
    Set-RegValue $script:UninstallKey 'DisplayVersion' $version | Out-Null
    Set-RegValue $script:UninstallKey 'Publisher' 'PrismArchive contributors' | Out-Null
    Set-RegValue $script:UninstallKey 'DisplayIcon' ($ExePath + ',0') | Out-Null
    Set-RegValue $script:UninstallKey 'InstallLocation' $InstallDir | Out-Null
    Set-RegValue $script:UninstallKey 'UninstallString' $uninstallString | Out-Null
    Set-RegValue $script:UninstallKey 'QuietUninstallString' ($uninstallString + ' -Quiet') | Out-Null
    Set-RegValue $script:UninstallKey 'URLInfoAbout' 'https://github.com/xueweijian/prism-archive' | Out-Null
    Set-RegValue $script:UninstallKey 'InstallDate' (Get-Date -Format 'yyyyMMdd') | Out-Null
    Set-RegValue $script:UninstallKey 'EstimatedSize' ([int]$sizeKb) 'DWord' | Out-Null
    Set-RegValue $script:UninstallKey 'NoModify' 1 'DWord' | Out-Null
    Set-RegValue $script:UninstallKey 'NoRepair' 1 'DWord' | Out-Null
}

function Install-Marker {
    param([string]$InstallDir, [string]$ExePath, [string[]]$Exts, [string]$Mode)
    Set-RegValue $script:AppRoot 'InstallDir' $InstallDir | Out-Null
    Set-RegValue $script:AppRoot 'ExePath' $ExePath | Out-Null
    Set-RegValue $script:AppRoot 'ScopeRoot' $script:ScopeRoot | Out-Null
    Set-RegValue $script:AppRoot 'Mode' $Mode | Out-Null
    Set-RegValue $script:AppRoot 'Extensions' ($Exts -join ';') | Out-Null
    Set-RegValue $script:AppRoot 'Version' (Get-ExeVersion $ExePath) | Out-Null
    # First install only: a timestamp refreshed on every run would make the
    # second run look like a change and defeat the idempotency check.
    if ($null -eq (Get-RegKind $script:AppRoot 'InstalledAt')) {
        Set-RegValue $script:AppRoot 'InstalledAt' (Get-Date -Format 'o') | Out-Null
    }
}

function Publish-ShellChange {
    <# Two broadcasts, both required for "install then double-click works now":
         WM_SETTINGCHANGE('Environment') -- new processes inherit PRISM_7Z_DLL;
         SHCNE_ASSOCCHANGED              -- Explorer drops its cached handler. #>
    if ($script:DryRun) { return }
    try {
        Add-Type -Namespace Prism -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.IntPtr wParam, string lParam, uint flags, uint timeout, out System.IntPtr result);
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@ -ErrorAction Stop
        $result = [IntPtr]::Zero
        [void][Prism.Native]::SendMessageTimeout([IntPtr]0xffff, 0x001A, [IntPtr]::Zero, 'Environment', 2, 2000, [ref]$result)
        [Prism.Native]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
    } catch {
        Warn ('could not broadcast the shell change: ' + $_.Exception.Message)
    }
}

# ---------------------------------------------------------------------------
# install / portable / verify
# ---------------------------------------------------------------------------

function Invoke-Install {
    param(
        [string]$SourceDir,
        [string]$TargetDir,
        [string]$EngineDll,
        [string[]]$Exts,
        [switch]$PortableMode,
        [switch]$SkipAssoc
    )

    $exe = Join-Path $TargetDir 'PrismArchive.exe'
    Say ('payload: ' + $SourceDir)
    Say ('target:  ' + $TargetDir)
    $assets = Find-AssetsDir -SourceDir $SourceDir
    if (-not $assets) { Warn 'Assets directory not found next to the payload' }
    Deploy-Payload -SourceDir $SourceDir -TargetDir $TargetDir -AssetsDir $assets

    if ($PortableMode) {
        $marker = Join-Path $TargetDir 'PrismArchive.portable'
        $body = @(
            '# PrismArchive portable deployment (no registry, no uninstall entry).',
            '# Engine: 7z.dll is not shipped (LGPL, blueprint ch.2). Point the app at',
            '# your 7-Zip copy before launching, for example in the same shell:',
            'engine=' + $EngineDll
        )
        if ($script:DryRun) {
            $script:Planned++
            Say ('write ' + $marker) 'plan'
        } else {
            Set-Content -LiteralPath $marker -Value $body -Encoding utf8
            $script:Wrote++
            Say ('wrote ' + $marker) 'done'
        }
        return
    }

    if (-not $SkipAssoc) {
        Install-Associations -ExePath $exe -IconPath $exe -Exts $Exts
        Report-UserChoice -Exts $Exts
        # Static verbs are the entry point the user actually sees. On a machine
        # where the archive association is pinned by the Windows user choice
        # (hash protected, as on this dev box), the shell does not necessarily
        # load the shellex DLL -- but "shell\verb" entries are always shown.
        # Without this step "installed" and "visible in the right-click menu"
        # were two different things, which is exactly what the user reported.
        $verbsScript = Join-Path $PSScriptRoot 'register-static-verbs.ps1'
        if (Test-Path -LiteralPath $verbsScript) {
            & $verbsScript -ExePath $exe -Quiet
        } else {
            Write-Warn ('static verbs script not found next to install.ps1: ' + $verbsScript)
        }
    }
    Install-EngineEnv -Dll $EngineDll
    Install-Marker -InstallDir $TargetDir -ExePath $exe -Exts $Exts -Mode 'install'
    Install-UninstallEntry -InstallDir $TargetDir -ExePath $exe

    if ($script:DryRun) { return }
    Publish-ShellChange
}

function Invoke-Verify {
    <# Proves the two things that actually matter on this machine:
         1. the shell resolver (AssocQueryStringW) answers with our command, and
         2. handing an archive to ShellExecute (what a double click does) starts
            the installed PrismArchive.exe.
       Reading the registry back is table stakes; asking the shell is the test. #>
    param([string]$TargetDir, [string[]]$Exts, [string]$EngineDll)

    $failed = 0
    $exe = Join-Path $TargetDir 'PrismArchive.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Say ('VERIFY: FAIL installed exe missing: ' + $exe) 'error'
        return 1
    }

    # Pick an extension the platform will actually let us own. A hash-protected
    # user choice cannot be overridden by an installer; if that is the case for
    # every extension, verification genuinely cannot run and must say so instead
    # of pretending (D1: a silent skip is not a pass).
    # Formats 7z.exe can actually create: the launch half of the check needs a real
    # file of the probed type, so a probe we cannot build is not a probe.
    $creatable = @('.tar', '.gz', '.xz', '.bz2', '.cpio', '.wim')
    $free = New-Object System.Collections.Generic.List[string]
    foreach ($ext in $Exts) {
        $choice = Get-UserChoice $ext
        if ($choice.Pinned) {
            if ($choice.ProgId -notlike 'PrismArchive.*') {
                Say ('VERIFY: SKIP ' + $ext + ' -- pinned by Windows user choice (' + $choice.ProgId + '); switch it in Settings > Default apps') 'warn'
            }
        } else {
            $free.Add($ext)
        }
    }
    $probe = $null
    foreach ($c in $creatable) { if (-not $probe -and $free.Contains($c)) { $probe = $c } }
    if (-not $probe -and $free.Count -gt 0) { $probe = $free[0] }
    if (-not $probe) {
        Say 'VERIFY: FAIL every extension is pinned by a Windows user choice -- nothing can be verified without the user changing defaults' 'error'
        return 1
    }
    Say ('VERIFY: probing ' + $probe + ' (no user choice pinned)')
    $default = Get-RegValue ($script:ClassesRoot + '\' + $probe) ''
    if ($default -eq (Get-ProgId $probe)) {
        Say ('VERIFY: PASS registry readback ' + $probe + ' -> ' + $default)
    } else {
        Say ('VERIFY: FAIL registry readback ' + $probe + ' -> ' + ([string]$default)) 'error'
        $failed++
    }
    $cmd = Get-RegValue ($script:ClassesRoot + '\' + (Get-ProgId $probe) + '\shell\open\command') ''
    if ($cmd -and $cmd.Contains($exe) -and $cmd.Contains('"%1"')) {
        Say ('VERIFY: PASS open command ' + $cmd)
    } else {
        Say ('VERIFY: FAIL open command ' + ([string]$cmd)) 'error'
        $failed++
    }

    Add-Type -Namespace Prism -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shlwapi.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, ExactSpelling = true)]
public static extern int AssocQueryStringW(uint flags, uint str, string assoc, string extra, System.Text.StringBuilder outBuf, ref uint outLen);
'@ -ErrorAction Stop
    $probeResolver = {
        param([string]$ext)
        $sb = New-Object System.Text.StringBuilder 1024
        $len = [uint32]1024
        $hr = [Prism.Shell]::AssocQueryStringW(0, 1, $ext, $null, $sb, [ref]$len)
        return [pscustomobject]@{ Hr = $hr; Text = $sb.ToString() }
    }
    # D4: calibrate the instrument before trusting it. .txt is associated on every
    # Windows install; if it does not resolve here, the resolver is unavailable in
    # this session and its silence says nothing about our registration.
    $calibration = & $probeResolver '.txt'
    if ($calibration.Hr -ne 0) {
        Say ('VERIFY: SKIP shell resolver -- it cannot resolve even .txt (hr=0x' + ('{0:X8}' -f $calibration.Hr) + '), so this probe is uncalibrated in this session') 'warn'
    } else {
        $resolved = & $probeResolver $probe
        if ($resolved.Hr -eq 0 -and $resolved.Text.Contains('PrismArchive')) {
            Say ('VERIFY: PASS shell resolver ASSOCSTR_COMMAND -> ' + $resolved.Text)
        } else {
            Say ('VERIFY: FAIL shell resolver ' + $probe + ' hr=0x' + ('{0:X8}' -f $resolved.Hr) + ' -> ' + $resolved.Text) 'error'
            $failed++
        }
    }

    # double-click equivalence: ShellExecute, not CreateProcess
    $sevenZip = ''
    if ($EngineDll) { $sevenZip = Join-Path (Split-Path -Parent $EngineDll) '7z.exe' }
    if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
        $c = Get-Command '7z.exe' -ErrorAction SilentlyContinue
        if ($c) { $sevenZip = $c.Source }
    }
    if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
        Say 'VERIFY: FAIL no 7z.exe to build a real .7z sample with -- cannot test the launch path' 'error'
        return 1
    }
    $sampleDir = Join-Path $env:TEMP ('prism-verify-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $sampleDir | Out-Null
    # The sample must carry the probed extension: launching a .7z would exercise
    # whatever the user pinned .7z to, not the association we are verifying.
    $sample = Join-Path $sampleDir ('verify-sample' + $probe)
    $format = $probe.TrimStart('.')
    Set-Content -LiteralPath (Join-Path $sampleDir 'payload.txt') -Value 'prism-archive association probe' -Encoding utf8
    & $sevenZip a ("-t" + $format) $sample (Join-Path $sampleDir 'payload.txt') | Out-Null
    if (-not (Test-Path -LiteralPath $sample)) {
        Say ('VERIFY: FAIL could not create a ' + $probe + ' sample with 7z.exe') 'error'
        return 1
    }

    $before = @(Get-Process -Name 'PrismArchive' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    $adoption = $null
    try {
        # (a) our own contract: the installed exe accepts the archive path as argv[1].
        # This is what the "%1" in the command string promises, and it is testable
        # without involving the shell at all.
        $direct = Start-Process -FilePath $exe -ArgumentList @($sample) -PassThru
        Start-Sleep -Seconds 3
        $direct.Refresh()
        if (-not $direct.HasExited) {
            Say ('VERIFY: PASS installed exe opens an archive argument directly (pid ' + $direct.Id + ', window "' + $direct.MainWindowTitle + '")')
            Stop-Process -Id $direct.Id -Force -ErrorAction SilentlyContinue
        } else {
            Say ('VERIFY: FAIL installed exe exited immediately with code ' + $direct.ExitCode + ' when given an archive') 'error'
            $failed++
        }

        # (b) OS adoption: what a double click would actually do. Kept separate on
        # purpose -- on current Windows a registered ProgID is not a default until
        # the user picks it, and reporting that as "our installer failed" would be
        # as dishonest as reporting it as a pass.
        Start-Process -FilePath $sample | Out-Null
        $deadline = (Get-Date).AddSeconds(15)
        $launched = $null
        while ((Get-Date) -lt $deadline) {
            $launched = Get-Process -Name 'PrismArchive' -ErrorAction SilentlyContinue |
                Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
            if ($launched) { break }
            Start-Sleep -Milliseconds 250
        }
        if ($launched) {
            $path = $launched.Path
            if ($path -and $path.Equals($exe, [System.StringComparison]::OrdinalIgnoreCase)) {
                Say ('VERIFY: PASS double-click ' + $probe + ' started ' + $path + ' (pid ' + $launched.Id + ')')
                $adoption = $true
            } else {
                Say ('VERIFY: FAIL double-click ' + $probe + ' started ' + ([string]$path) + ' instead of ' + $exe) 'error'
                $failed++
                $adoption = $true
            }
            Stop-Process -Id $launched.Id -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Say ('VERIFY: FAIL launch check threw: ' + $_.Exception.Message) 'error'
        $failed++
    } finally {
        Remove-Item -LiteralPath $sampleDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($failed -gt 0) {
        Say ('VERIFY: FAILED (' + $failed + ' checks)') 'error'
        return 1
    }
    if ($adoption -eq $true) {
        Say 'VERIFY: PASSED (registry + installed exe + OS adopted the association)'
        return 0
    }
    # Everything we control is correct; the OS will not hand a newly registered
    # ProgID the default. Documented platform behaviour (hash-protected user
    # choice), with the exact user action printed above.
    Say ('VERIFY: OS-ADOPTION-PENDING -- registry and exe verified, but this Windows build did not route ' + $probe + ' to PrismArchive. Finish it once in Settings > Default apps or via Open with -> PrismArchive; no installer can write a hash-protected default.')
    return 3
}

# ---------------------------------------------------------------------------
# self-test: the guard, and its negative control
# ---------------------------------------------------------------------------

$script:TestFailures = New-Object System.Collections.Generic.List[string]
$script:Planned = 0

$script:Checked = 0

function Assert-True {
    param([string]$What, [bool]$Condition)
    $script:Checked++
    if ($Condition) {
        Say ('ok   ' + $What)
    } else {
        $script:TestFailures.Add($What)
        Say ('SELFTEST FAIL: ' + $What) 'error'
    }
}

function Assert-Equal {
    param([string]$What, $Expected, $Actual)
    $script:Checked++
    if ([string]$Expected -eq [string]$Actual) {
        Say ('ok   ' + $What)
    } else {
        $script:TestFailures.Add($What)
        Say ('SELFTEST FAIL: ' + $What + ' (expected ' + [string]$Expected + ', got ' + [string]$Actual + ')') 'error'
    }
}

function New-FakePayload {
    param([string]$Dir)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $stamp = [System.Text.Encoding]::UTF8.GetBytes('prism-selftest-payload')
    foreach ($name in @('PrismArchive.exe', 'PrismShellExt.dll', 'PrismShellExtClassic.dll', '7z.dll')) {
        [System.IO.File]::WriteAllBytes((Join-Path $Dir $name), $stamp)
    }
    $assetsIcon = Join-Path $Dir 'Assets\Icon'
    New-Item -ItemType Directory -Force -Path $assetsIcon | Out-Null
    $realIcon = Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets\Icon\prism.ico'
    if (Test-Path -LiteralPath $realIcon -PathType Leaf) {
        Copy-Item -LiteralPath $realIcon -Destination (Join-Path $assetsIcon 'prism.ico') -Force
    } else {
        [System.IO.File]::WriteAllBytes((Join-Path $assetsIcon 'prism.ico'), $stamp)
    }
    $shaders = Join-Path $Dir 'Assets\Shaders'
    New-Item -ItemType Directory -Force -Path $shaders | Out-Null
    Set-Content -LiteralPath (Join-Path $shaders 'LiquidGlassEffect.hlsl') -Value '// fixture' -Encoding utf8
    # the generator is source, not payload: it must not be deployed
    Set-Content -LiteralPath (Join-Path $assetsIcon 'make_prism_icon.py') -Value '# fixture' -Encoding utf8
}

function Invoke-SelfTest {
    $root = 'HKCU:\Software\PrismArchiveSelfTest'
    $tmp = Join-Path $env:TEMP ('prism-selftest-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $src = Join-Path $tmp 'payload'
    $dst = Join-Path $tmp 'install'
    $portableDir = Join-Path $tmp 'portable'
    $fakeEngine = Join-Path $src '7z.dll'
    $exts = @('.7z', '.zip', '.rar', '.gz')

    Say ('selftest scratch scope: ' + $root)
    Say ('selftest scratch dir:   ' + $tmp)

    $script:ScopeRoot = $root
    $script:ClassesRoot = $root + '\Classes'
    $script:AppRoot = $root + '\PrismArchive'
    $script:UninstallRoot = $root + '\Microsoft\Windows\CurrentVersion\Uninstall'
    $script:UninstallKey = $script:UninstallRoot + '\PrismArchive'
    $script:EnvRoot = $root + '\Environment'
    $script:BackupRoot = $script:AppRoot + '\AssocBackup'

    Remove-RegTree $root
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-FakePayload $src

    # Pre-existing state must survive the round trip: a live association (.zip),
    # an extension with no default (.rar), and a user engine override.
    Set-RegValue ($script:ClassesRoot + '\.zip') '' '7-Zip.zip' | Out-Null
    Set-RegValue ($script:EnvRoot) 'PRISM_7Z_DLL' 'C:\stale\7z.dll' | Out-Null
    $seeded = Get-RegTree $root

    # -- 0. the mapping itself (this is the bug the first self-test missed) ---
    $script:EnvRoot = Get-EnvRootPath $root
    Assert-Equal 'real scope places the engine override at HKCU\Environment' 'HKCU:\Environment' (Get-EnvRootPath 'HKCU:\Software')
    Assert-Equal 'scratch scope keeps its override inside the scope' ($root + '\Environment') (Get-EnvRootPath $root)

    # -- 0b. read paths must tolerate missing keys --------------------------
    # CI's Windows runner throws SecurityException for OpenSubKey(name, writable)
    # on a key that does not exist, while the dev box returns $null. A first
    # install reads nothing but missing keys, so this class must be asserted, not
    # assumed (CONTRACTS B3: when local != CI the CI behaviour is the truth).
    Assert-Equal 'reading a missing key returns null, not an error' $null (Get-RegValue ($root + '\NoSuchKey\Deeper') 'Value')
    Assert-Equal 'a missing key is reported as missing' $false (Test-RegKey ($root + '\NoSuchKey'))
    Remove-RegValue ($root + '\NoSuchKey') 'Value'
    Assert-True 'deleting from a missing key is a no-op' (-not (Test-RegKey ($root + '\NoSuchKey')))

    # -- 1. dry run changes nothing ----------------------------------------
    $script:Wrote = 0
    $script:Planned = 0
    $script:DryRun = $true
    Invoke-Install -SourceDir $src -TargetDir $dst -EngineDll $fakeEngine -Exts $exts
    $script:DryRun = $false
    $afterDry = Get-RegTree $root
    Assert-Equal 'dry run wrote no registry value' 0 $script:Wrote
    Assert-True 'dry run printed planned operations' ($script:Planned -gt 20)
    Assert-True 'dry run left the registry byte-identical' ((($seeded -join '|') -eq ($afterDry -join '|')))
    Assert-True 'dry run created no install directory' (-not (Test-Path -LiteralPath $dst))

    # -- 2. first install ---------------------------------------------------
    Invoke-Install -SourceDir $src -TargetDir $dst -EngineDll $fakeEngine -Exts $exts
    $installed = Get-RegTree $root
    Assert-Equal '.7z default ProgID' 'PrismArchive.7z' (Get-RegValue ($script:ClassesRoot + '\.7z') '')
    Assert-Equal '.gz default ProgID' 'PrismArchive.gz' (Get-RegValue ($script:ClassesRoot + '\.gz') '')
    $openCmd = Get-RegValue ($script:ClassesRoot + '\PrismArchive.7z\shell\open\command') ''
    Assert-True 'open command quotes the exe and %1' ($openCmd.Contains('"' + (Join-Path $dst 'PrismArchive.exe') + '"') -and $openCmd.Contains('"%1"'))
    Assert-Equal 'DefaultIcon points at the exe' ((Join-Path $dst 'PrismArchive.exe') + ',0') (Get-RegValue ($script:ClassesRoot + '\PrismArchive.7z\DefaultIcon') '')
    Assert-Equal 'Applications SupportedTypes has .7z' '' (Get-RegValue ($script:ClassesRoot + '\Applications\PrismArchive.exe\SupportedTypes') '.7z')
    Assert-Equal 'uninstall entry name' 'PrismArchive' (Get-RegValue $script:UninstallKey 'DisplayName')
    Assert-Equal 'marker install dir' $dst (Get-RegValue $script:AppRoot 'InstallDir')
    Assert-Equal 'engine recorded' $fakeEngine (Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL')
    Assert-Equal 'engine previous value preserved' 'C:\stale\7z.dll' (Get-RegValue $script:AppRoot 'EnginePrevious')
    Assert-Equal 'backup captured the previous .zip handler' '7-Zip.zip' (Get-RegValue ($script:BackupRoot + '\.zip') 'Previous')
    Assert-Equal 'backup knows .rar had no handler' 0 (Get-RegValue ($script:BackupRoot + '\.rar') 'Present')
    Assert-True 'exe deployed' (Test-Path -LiteralPath (Join-Path $dst 'PrismArchive.exe'))
    Assert-True 'shell ext deployed' (Test-Path -LiteralPath (Join-Path $dst 'PrismShellExt.dll'))
    Assert-True 'classic shell ext deployed' (Test-Path -LiteralPath (Join-Path $dst 'PrismShellExtClassic.dll'))
    Assert-True 'assets deployed' (Test-Path -LiteralPath (Join-Path $dst 'Assets\Icon\prism.ico'))
    Assert-True 'asset generator is not deployed' (-not (Test-Path -LiteralPath (Join-Path $dst 'Assets\Icon\make_prism_icon.py')))
    Assert-True 'uninstaller deployed' (Test-Path -LiteralPath (Join-Path $dst 'uninstall.ps1'))
    Assert-True 'engine DLL is never shipped' (-not (Test-Path -LiteralPath (Join-Path $dst '7z.dll')))

    # -- 3. idempotency: a second install is a no-op ------------------------
    Say '--- second install (idempotency) ---'
    $script:Wrote = 0
    Invoke-Install -SourceDir $src -TargetDir $dst -EngineDll $fakeEngine -Exts $exts
    $second = Get-RegTree $root
    Assert-Equal 'second install wrote nothing' 0 $script:Wrote
    Assert-True 'second install left the registry byte-identical' ((($installed -join '|') -eq ($second -join '|')))

    # -- 4. uninstall restores what was there ------------------------------
    $psExe = (Get-Process -Id $PID).Path
    $uninstall = Join-Path $PSScriptRoot 'uninstall.ps1'
    $uninstallArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $uninstall, '-ScopeRoot', $root, '-InstallDir', $dst, '-Quiet')
    if ($script:Mutate) { $uninstallArgs += @('-Mutate', $script:Mutate) }
    $out = & $psExe @uninstallArgs *>&1
    Assert-Equal 'uninstall exit code' 0 $LASTEXITCODE
    Assert-Equal 'uninstall restored the .zip handler' '7-Zip.zip' (Get-RegValue ($script:ClassesRoot + '\.zip') '')
    Assert-Equal 'uninstall removed our .7z handler' $null (Get-RegKind ($script:ClassesRoot + '\.7z') '')
    Assert-Equal 'uninstall removed the ProgID tree' $false (Test-RegKey ($script:ClassesRoot + '\PrismArchive.7z'))
    Assert-Equal 'uninstall removed the Applications entry' $false (Test-RegKey ($script:ClassesRoot + '\Applications\PrismArchive.exe'))
    Assert-Equal 'uninstall removed the uninstall entry' $false (Test-RegKey $script:UninstallKey)
    Assert-Equal 'uninstall removed the marker' $false (Test-RegKey $script:AppRoot)
    Assert-Equal 'uninstall restored the previous engine override' 'C:\stale\7z.dll' (Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL')
    Assert-True 'uninstall removed the install directory' (-not (Test-Path -LiteralPath $dst))
    $afterUninstall = Get-RegTree $root
    Assert-True 'uninstall restored the exact pre-install registry state' ((($seeded -join '|') -eq ($afterUninstall -join '|')))
    if (($seeded -join '|') -ne ($afterUninstall -join '|')) {
        foreach ($line in $afterUninstall) { if ($seeded -notcontains $line) { Say ('  extra after uninstall: ' + $line) 'error' } }
        foreach ($line in $seeded) { if ($afterUninstall -notcontains $line) { Say ('  missing after uninstall: ' + $line) 'error' } }
    }

    # -- 5. uninstall is idempotent too ------------------------------------
    $out2 = & $psExe @uninstallArgs *>&1
    Assert-Equal 'second uninstall exit code' 0 $LASTEXITCODE
    Assert-True 'second uninstall says already clean' (($out2 -join ';') -match 'already clean')

    # -- 6. portable writes no registry at all -----------------------------
    $beforePortable = Get-RegTree $root
    Invoke-Install -SourceDir $src -TargetDir $portableDir -EngineDll $fakeEngine -Exts $exts -PortableMode
    $afterPortable = Get-RegTree $root
    Assert-True 'portable wrote no registry key' ((($beforePortable -join '|') -eq ($afterPortable -join '|')))
    Assert-True 'portable deployed the exe' (Test-Path -LiteralPath (Join-Path $portableDir 'PrismArchive.exe'))
    Assert-True 'portable wrote its marker' (Test-Path -LiteralPath (Join-Path $portableDir 'PrismArchive.portable'))
    Assert-True 'portable marker records the engine' ((Get-Content -LiteralPath (Join-Path $portableDir 'PrismArchive.portable') -Raw).Contains($fakeEngine))

    # -- 7. cleanup --------------------------------------------------------
    Remove-RegTree $root
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue

    if ($script:Mutate) {
        # CONTRACTS D3: the injected defect must be observable. CAUGHT is the
        # healthy outcome and exits 0; NOT CAUGHT exits 1 and means the suite is
        # a restatement of the writer, not a guard.
        if ($script:TestFailures.Count -gt 0) {
            Write-Host ('SELFTEST(MUTATION ' + $script:Mutate + '): CAUGHT (' + $script:TestFailures.Count + ' assertions went red)')
            return 0
        }
        Say ('SELFTEST(MUTATION ' + $script:Mutate + '): NOT CAUGHT -- this suite is not a guard') 'error'
        return 1
    }
    if ($script:TestFailures.Count -gt 0) {
        Say ('SELFTEST: FAIL (' + $script:TestFailures.Count + ' assertions)') 'error'
        return 1
    }
    Write-Host ('SELFTEST: PASS (' + $script:Checked + ' assertions: dry-run purity / install / idempotent reinstall / association restore / clean uninstall / idempotent uninstall / portable isolation)')
    return 0
}

# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------

try {
    if ($SelfTest) { exit (Invoke-SelfTest) }

    if ($Verify -and $ScopeRoot -ne 'HKCU:\Software') {
        throw '-Verify needs the real scope (HKCU:\Software); it asks the shell resolver, which only sees real associations'
    }

    # A one-element -Extensions reaches this file as a scalar string, and an
    # if-expression UNROLLS a one-element array back to a scalar. Assign first,
    # then fill the default: $exts stays an array so $exts.Count is valid
    # under Set-StrictMode (C39 found this with -Extensions .7z).
    $exts = @($Extensions)
    if ($exts.Count -eq 0) { $exts = @($script:DefaultExtensions) }
    foreach ($e in $exts) {
        if (-not $e.StartsWith('.')) { throw ('extension must start with a dot: ' + $e) }
    }
    $target = if ($InstallDir) { $InstallDir } else { Join-Path $env:LOCALAPPDATA 'PrismArchive' }
    $src = Find-SourceDir -Explicit $SourceDir
    $engine = Find-Engine -Explicit $SevenZipDll -UserEnvValue (Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL')

    $installArgs = @{
        SourceDir = $src
        TargetDir = $target
        EngineDll = $engine
        Exts = $exts
        PortableMode = [bool]$Portable
        SkipAssoc = [bool]$NoAssociations
    }
    Invoke-Install @installArgs

    if ($Portable) {
        if ($script:DryRun) {
            Say 'DRYRUN: OK portable deployment plan printed, nothing written'
        } else {
            Say ('PORTABLE: OK ' + $target)
        }
    } elseif ($script:DryRun) {
        Say ('DRYRUN: OK would install ' + $target + ' with ' + $exts.Count + ' associations (' + $script:Planned + ' planned operations)')
    } else {
        if ($script:Warnings.Count -gt 0) {
            Say ('install finished with ' + $script:Warnings.Count + ' warning(s); see WARN lines above') 'warn'
        }
        Say ('INSTALL: OK ' + $target + ' (' + $exts.Count + ' associations, engine=' + $(if ($engine) { $engine } else { 'NOT FOUND' }) + ')')
    }

    if ($Verify) {
        exit (Invoke-Verify -TargetDir $target -Exts $exts -EngineDll $engine)
    }
} catch {
    Fail $_.Exception.Message
    Fail ($_.ScriptStackTrace)
}

exit $script:ExitCode
