#requires -Version 5.1
<#
.SYNOPSIS
  End-to-end verification of the per-user PrismArchive install: backup the real
  HKCU associations, install, assert .7z ProgID/command/icon/menu, run the
  INSTALLED exe headlessly against a real archive, uninstall, and prove the
  association tree is restored key by key.

.DESCRIPTION
  This is the "clean user" pass that C3's unit-level self-test cannot make:
  C3 proves the writer's own bookkeeping, this proves the machine's state.

  Default mode is the REAL user scope (HKCU\Software + %LOCALAPPDATA%\PrismArchive).
  It refuses to run over an existing PrismArchive install unless -CleanFirst is
  given; -CleanFirst runs tools/uninstall.ps1 first (after taking a backup), so
  the run still starts from a clean user state.

  -Sandbox installs into a unique scratch HKCU key and %TEMP% directory.  It is
  safe on a shared machine and is what ctest runs.  The scratch tree is removed
  even when an assertion fails; the real HKCU associations are never touched.

  Idempotency is asserted, not assumed:
    * uninstall before install  -> "UNINSTALL: already clean"
    * install twice             -> second run writes ZERO registry/file values
                                   and leaves a byte-identical snapshot
    * uninstall twice           -> second run is "already clean"

  Negative controls (CONTRACTS D3):
    -Mutate UnquotedVerb  forwards the installer's UnquotedVerb mutation and
                          verifies the malformed command is actually present
                          before expecting the guard to go red.
    -Mutate NoRestore     forwards the uninstaller's NoRestore mutation and
                          verifies the association was really left pointing at
                          the removed ProgID before expecting the restore check
                          to go red.
  The mutation verdict is printed at the FIRST failed assertion and the process
  leaves immediately (CONTRACTS D27). A marker printed after teardown can be
  lost to a throw, a hang or a crash in that teardown -- the failure mode that
  made a red C39 run report "required regular expression not found" (2026-11-02).
  Mutation mode is allowed only with -Sandbox: a deliberately broken uninstall
  must never be pointed at the real user's associations.

  Exit codes: 0 pass/caught, 1 failure/not-caught, 2 usage, 77 explicit SKIP
  (missing build output or 7z.dll).  The SKIP line names the missing input;
  a silent skip is never a pass (CONTRACTS D1).

.PARAMETER SourceDir
  Directory containing PrismArchive.exe (normally <build>\Src\PrismWin).  When
  omitted, PRISM_BUILD_DIR, the repo's build folder, and E:\build\wt-c39 are
  tried.  A missing folder is a SKIP; an explicitly wrong folder is a FAIL.

.PARAMETER InstallDir
  Install target.  Defaults to %LOCALAPPDATA%\PrismArchive in real mode and a
  unique %TEMP% directory in -Sandbox mode.

.PARAMETER ScopeRoot
  Registry base.  Defaults to HKCU:\Software in real mode and a unique scratch
  key in -Sandbox mode.  Any non-HKCU:\Software value is treated as scratch.

.PARAMETER SevenZipDll
  Path to 7z.dll.  Defaults to PRISM_7Z_DLL, the standard 7-Zip install paths,
  then E:\DevTools\7-Zip (the development image).

.PARAMETER Extensions
  Extensions to associate.  Default: .7z.  The restore proof compares the whole
  .7z key tree, so one extension is enough to prove the contract.

.PARAMETER Sandbox
  Use a unique scratch registry scope and install directory.

.PARAMETER CleanFirst
  If a real PrismArchive install already exists, uninstall it first (with a
  backup) and continue from the resulting clean state.

.PARAMETER KeepInstalled
  Do not uninstall at the end.  Restore assertions are then explicitly skipped.

.PARAMETER ShellLaunch
  Also try a real double-click (ShellExecute) and detect the installed process.
  This needs an interactive desktop and is NOT part of the ctest verdict; when
  Windows has a hash-protected UserChoice pinned elsewhere it reports SKIP with
  the ProgID instead of pretending.

.PARAMETER Mutate
  UnquotedVerb | NoRestore.  Requires -Sandbox.

.EXAMPLE
  pwsh -File tools\verify-install.ps1 -SourceDir E:\build\wt-c39\Src\PrismWin

.EXAMPLE
  pwsh -File tools\verify-install.ps1 -SourceDir E:\build\wt-c39\Src\PrismWin -Sandbox

.EXAMPLE
  pwsh -File tools\verify-install.ps1 -SourceDir E:\build\wt-c39\Src\PrismWin -Sandbox -Mutate UnquotedVerb
#>
[CmdletBinding()]
param(
    [string]$SourceDir = '',
    [string]$InstallDir = '',
    [string]$ScopeRoot = '',
    [string]$SevenZipDll = '',
    [string[]]$Extensions = @('.7z'),
    [switch]$Sandbox,
    [switch]$CleanFirst,
    [switch]$KeepInstalled,
    [switch]$ShellLaunch,
    [string]$Mutate = '',
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# state + output
# ---------------------------------------------------------------------------

$script:Quiet = [bool]$Quiet
$script:Checks = 0
$script:Failures = New-Object System.Collections.Generic.List[string]
$script:Skips = New-Object System.Collections.Generic.List[string]
$script:Mutation = $Mutate
$script:MutationObserved = $false
$script:Installed = $false
$script:Uninstalled = $false
$script:WorkDir = ''
$script:BackupJson = ''
$script:BackupReg = ''
$script:IsSandbox = [bool]$Sandbox
$script:RealScope = $false

function Say {
    param([string]$Text = '', [string]$Level = 'info')
    if ($script:Quiet -and ($Level -eq 'info' -or $Level -eq 'pass' -or $Level -eq 'skip')) { return }
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
    # CONTRACTS D27: the verdict belongs to the FIRST failed assertion, not to
    # the end of the run. Print it here and leave before the catch/finally
    # teardown (cleanup uninstall, scratch-registry removal) can throw or hang.
    # Exit 0 on purpose: the ctest entry decides with PASS_REGULAR_EXPRESSION
    # and already printed its FAIL line above (CONTRACTS D26).
    if ($script:Mutation -and $script:MutationObserved) {
        Say ('VERIFY-INSTALL(MUTATION ' + $script:Mutation + '): CAUGHT (' + $script:Failures.Count + ' assertion(s) went red; injection observed; verdict printed at the first failure)')
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
    Say ('VERIFY-INSTALL: USAGE ERROR (' + $Text + ')') 'error'
    exit 2
}

function Stop-Skip {
    param([string]$Reason)
    Say ('VERIFY-INSTALL: SKIP (' + $Reason + ')')
    exit 77
}

function Join-Output {
    param($Lines)
    return [string]::Join([Environment]::NewLine, [string[]]$Lines)
}

# ---------------------------------------------------------------------------
# registry primitives (independent of tools/install.ps1: this file must be
# able to disagree with the writer, otherwise it is just a restatement)
# ---------------------------------------------------------------------------

function Get-Hive {
    param([string]$Path)
    if ($Path -like 'HKCU:\*') {
        return @([Microsoft.Win32.Registry]::CurrentUser, $Path.Substring(6))
    }
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

function Remove-RegTree {
    param([string]$Path)
    if (-not (Test-RegKey $Path)) { return }
    $h = Get-Hive $Path
    $h[0].DeleteSubKeyTree($h[1], $false)
}

function Format-RegValue {
    param($Value, [string]$Kind)
    if ($null -eq $Value) { return '<null>' }
    if ($Value -is [byte[]]) { return 'hex:' + [Convert]::ToBase64String($Value) }
    if ($Value -is [string[]]) { return 'multi:' + (($Value | ForEach-Object { [string]$_ }) -join ([string][char]31)) }
    return [string]$Value
}

function Get-RegLines {
    <# Canonical "path|name|kind|value" lines. KEY and MISSING markers make key
       existence itself part of the comparison: deleting every value but leaving
       an empty key is NOT a restore. #>
    param([string]$Path, [string[]]$OnlyNames = $null)

    $h = Get-Hive $Path
    $key = $h[0].OpenSubKey($h[1])
    if ($null -eq $key) { return @($Path + '|@MISSING||') }

    $lines = New-Object System.Collections.Generic.List[string]
    try {
        $lines.Add($Path + '|@KEY||')
        if ($OnlyNames) {
            $names = @($key.GetValueNames())
            foreach ($n in $OnlyNames) {
                if ($names -contains $n) {
                    $v = $key.GetValue($n, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $kind = [string]$key.GetValueKind($n)
                    $lines.Add($Path + '|' + $n + '|' + $kind + '|' + (Format-RegValue $v $kind))
                }
            }
        } else {
            $walk = $null
            $walk = {
                param($node, [string]$rel)
                foreach ($n in @($node.GetValueNames() | Sort-Object)) {
                    $v = $node.GetValue($n, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $kind = [string]$node.GetValueKind($n)
                    $lines.Add($Path + $rel + '|' + $n + '|' + $kind + '|' + (Format-RegValue $v $kind))
                }
                foreach ($sub in @($node.GetSubKeyNames() | Sort-Object)) {
                    $lines.Add($Path + $rel + '\' + $sub + '|@KEY||')
                    $child = $node.OpenSubKey($sub)
                    & $walk $child ($rel + '\' + $sub)
                    $child.Close()
                }
            }
            & $walk $key ''
        }
    } finally { $key.Close() }

    return @($lines | Sort-Object)
}

function Get-Snapshot {
    $lines = New-Object System.Collections.Generic.List[string]
    # Parenthesize every element: the comma operator binds tighter than '+', so
    # an unparenthesized "$a + '\' + $b," list collapses into ONE array that the
    # [string] parameter then joins with spaces (C39 found this the hard way).
    $roots = @(
        ($script:ClassesRoot + '\' + $script:ProbeExt),
        ($script:ClassesRoot + '\' + (Get-ProgId $script:ProbeExt)),
        ($script:ClassesRoot + '\Applications\PrismArchive.exe'),
        $script:AppRoot,
        $script:UninstallKey
    )
    foreach ($r in $roots) { $lines.AddRange([string[]]@(Get-RegLines $r)) }
    $lines.AddRange([string[]]@(Get-RegLines $script:EnvRoot @('PRISM_7Z_DLL')))

    # Parent keys must not survive as empty leftovers.  Only existence is
    # recorded for Applications, not its (huge) real-machine subtree.
    foreach ($p in @(($script:ClassesRoot + '\Applications'), $script:ClassesRoot, $script:UninstallRoot)) {
        $lines.Add($p + '|@PARENT|' + [int](Test-RegKey $p) + '|')
    }

    # The OS-owned user choice is not written by the installer; recording it is
    # how we prove the installer/uninstaller left it alone.
    $fileExt = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\' + $script:ProbeExt
    $lines.AddRange([string[]]@(Get-RegLines $fileExt))

    return [string[]]($lines | Sort-Object)
}

function Compare-Snapshots {
    param([string]$What, [string[]]$Before, [string[]]$After)
    $diff = @(Compare-Object -ReferenceObject $Before -DifferenceObject $After -CaseSensitive)
    if ($diff.Count -eq 0) {
        Say ($What + ' (' + $Before.Count + ' snapshot lines byte-identical)') 'pass'
        Add-Check
        return
    }
    $line = $What + ' -- ' + $diff.Count + ' differing snapshot line(s)'
    Add-Check
    Say $line 'fail'
    Add-Failure $line
    foreach ($d in @($diff | Select-Object -First 30)) {
        $side = if ([string]$d.SideIndicator -eq '=>') { 'extra now' } else { 'missing now' }
        Say ('  ' + $side + ': ' + [string]$d.InputObject) 'fail'
    }
}

function Get-ProgId {
    param([string]$Ext)
    return 'PrismArchive' + $Ext
}

function Get-UserChoice {
    param([string]$Ext)
    $base = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\' + $Ext
    $latest = Get-RegValue ($base + '\UserChoiceLatest\ProgId') 'ProgId'
    if ($latest) { return [pscustomobject]@{ Pinned = $true; ProgId = [string]$latest; Source = 'UserChoiceLatest' } }
    $legacy = Get-RegValue ($base + '\UserChoice') 'ProgId'
    if ($legacy) { return [pscustomobject]@{ Pinned = $true; ProgId = [string]$legacy; Source = 'UserChoice' } }
    return [pscustomobject]@{ Pinned = $false; ProgId = ''; Source = '' }
}

# ---------------------------------------------------------------------------
# paths / inputs
# ---------------------------------------------------------------------------

function Resolve-Source {
    param([string]$Explicit)
    if ($Explicit) {
        $p = [System.IO.Path]::GetFullPath($Explicit)
        if (-not (Test-Path -LiteralPath (Join-Path $p 'PrismArchive.exe') -PathType Leaf)) {
            Say ('VERIFY-INSTALL: FAIL -SourceDir has no PrismArchive.exe: ' + $p) 'error'
            exit 1
        }
        return $p
    }
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:PRISM_BUILD_DIR) { $candidates.Add((Join-Path $env:PRISM_BUILD_DIR 'Src\PrismWin')) }
    $repo = Split-Path -Parent $PSScriptRoot
    $candidates.Add((Join-Path $repo 'build\Src\PrismWin'))
    $candidates.Add('E:\build\wt-c39\Src\PrismWin')
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'PrismArchive.exe') -PathType Leaf)) {
            return [System.IO.Path]::GetFullPath($c)
        }
    }
    Stop-Skip ('PrismArchive.exe not found; build the PrismArchive target or pass -SourceDir. Tried: ' + ($candidates -join '; '))
}

function Resolve-Engine {
    param([string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit -PathType Leaf)) {
            Say ('VERIFY-INSTALL: FAIL -SevenZipDll does not exist: ' + $Explicit) 'error'
            exit 1
        }
        return [System.IO.Path]::GetFullPath($Explicit)
    }
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:PRISM_7Z_DLL) { $candidates.Add($env:PRISM_7Z_DLL) }
    $candidates.Add((Join-Path $env:ProgramFiles '7-Zip\7z.dll'))
    $x86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($x86) { $candidates.Add((Join-Path $x86 '7-Zip\7z.dll')) }
    $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\7-Zip\7z.dll'))
    $candidates.Add('E:\DevTools\7-Zip\7z.dll')
    $cmd = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add((Join-Path (Split-Path -Parent $cmd.Source) '7z.dll')) }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return [System.IO.Path]::GetFullPath($c) }
    }
    Stop-Skip ('no 7z.dll found; install 7-Zip x64 or set PRISM_7Z_DLL. The installed app cannot verify an archive without its engine (it is never bundled: LGPL)')
}

function PsQuote {
    param([string]$Text)
    return "'" + $Text.Replace("'", "''") + "'"
}

function New-Wrapper {
    param([string]$Name, [string]$Body)
    $p = Join-Path $script:WorkDir $Name
    Set-Content -LiteralPath $p -Value $Body -Encoding UTF8
    return $p
}

function Invoke-Wrapper {
    param([string]$Wrapper, [string]$Label)
    Say ('-- ' + $Label + ' --')
    $psExe = (Get-Process -Id $PID).Path
    $lines = New-Object System.Collections.Generic.List[string]
    & $psExe -NoProfile -ExecutionPolicy Bypass -File $Wrapper *>&1 | ForEach-Object {
        $s = [string]$_
        $lines.Add($s)
        Say ('  ' + $s)
    }
    $code = 0
    if ($null -ne $LASTEXITCODE) { $code = [int]$LASTEXITCODE }
    return [pscustomobject]@{ ExitCode = $code; Output = $lines.ToArray() }
}

# ---------------------------------------------------------------------------
# install/uninstall wrappers (the real scripts, run as child processes)
# ---------------------------------------------------------------------------

$script:ScriptRoot = $PSScriptRoot

function Invoke-InstallStep {
    param([string]$Label)
    $extExpr = '@(' + (($Extensions | ForEach-Object { PsQuote ([string]$_) }) -join ',') + ')'
    $line = '& ' + (PsQuote (Join-Path $script:ScriptRoot 'install.ps1')) +
            ' -SourceDir ' + (PsQuote $script:SourceDir) +
            ' -InstallDir ' + (PsQuote $script:InstallDir) +
            ' -ScopeRoot ' + (PsQuote $script:ScopeRoot) +
            ' -SevenZipDll ' + (PsQuote $script:EngineDll) +
            ' -Extensions ' + $extExpr
    if ($script:Mutation -eq 'UnquotedVerb') { $line += ' -Mutate UnquotedVerb' }
    $body = $line + [Environment]::NewLine + 'exit $LASTEXITCODE' + [Environment]::NewLine
    $wrapper = New-Wrapper ('install-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.ps1') $body
    return Invoke-Wrapper $wrapper $Label
}

function Invoke-UninstallStep {
    param([string]$Label, [string]$Mutation = '')
    $line = '& ' + (PsQuote (Join-Path $script:ScriptRoot 'uninstall.ps1')) +
            ' -ScopeRoot ' + (PsQuote $script:ScopeRoot) +
            ' -InstallDir ' + (PsQuote $script:InstallDir)
    if ($Mutation) { $line += ' -Mutate ' + (PsQuote $Mutation) }
    $body = $line + [Environment]::NewLine + 'exit $LASTEXITCODE' + [Environment]::NewLine
    $wrapper = New-Wrapper ('uninstall-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.ps1') $body
    return Invoke-Wrapper $wrapper $Label
}

# ---------------------------------------------------------------------------
# assertions
# ---------------------------------------------------------------------------

function Test-ExeHasIcon {
    param([string]$ExePath)
    try {
        if (-not ('PrismC39.IconApi' -as [type])) {
            Add-Type -Namespace PrismC39 -Name IconApi -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint ExtractIconExW(string file, int index, System.IntPtr[] large, System.IntPtr[] small, uint count);
'@
        }
        return ([int][PrismC39.IconApi]::ExtractIconExW($ExePath, -1, $null, $null, 0) -gt 0)
    } catch {
        return $null
    }
}

function Assert-InstalledState {
    $exe = Join-Path $script:InstallDir 'PrismArchive.exe'
    $prog = $script:ClassesRoot + '\' + (Get-ProgId $script:ProbeExt)
    $app = $script:ClassesRoot + '\Applications\PrismArchive.exe'
    $expectedCommand = '"' + $exe + '" "%1"'
    $expectedIcon = $exe + ',0'

    Assert-Equal ('.7z default ProgID') (Get-ProgId $script:ProbeExt) (Get-RegValue ($script:ClassesRoot + '\' + $script:ProbeExt) '')
    Assert-Equal ('ProgID open command quotes the installed exe and %1') $expectedCommand (Get-RegValue ($prog + '\shell\open\command') '')
    Assert-Equal ('ProgID DefaultIcon points at the installed exe') $expectedIcon (Get-RegValue ($prog + '\DefaultIcon') '')
    Assert-Equal ('ProgID description') '7z Archive' (Get-RegValue $prog '')
    Assert-Equal ('ProgID MultiSelectModel') 'Player' (Get-RegValue ($prog + '\shell\open') 'MultiSelectModel')
    Assert-Equal ('ProgID FriendlyAppName') 'PrismArchive' (Get-RegValue ($prog + '\shell\open') 'FriendlyAppName')

    Assert-Equal ('Applications open command quotes the installed exe and %1') $expectedCommand (Get-RegValue ($app + '\shell\open\command') '')
    Assert-Equal ('Applications DefaultIcon points at the installed exe') $expectedIcon (Get-RegValue ($app + '\DefaultIcon') '')
    Assert-Equal ('Applications FriendlyAppName') 'PrismArchive' (Get-RegValue $app 'FriendlyAppName')
    Assert-Equal ('Applications SupportedTypes .7z is registered as an empty value') 'String' ([string](Get-RegKind ($app + '\SupportedTypes') $script:ProbeExt))
    Assert-Equal ('Applications SupportedTypes .7z has empty data') '' (Get-RegValue ($app + '\SupportedTypes') $script:ProbeExt)
    Assert-Equal ('.7z OpenWithProgids advertises the ProgID as REG_NONE') 'None' ([string](Get-RegKind ($script:ClassesRoot + '\' + $script:ProbeExt + '\OpenWithProgids') (Get-ProgId $script:ProbeExt)))

    Assert-Equal ('marker InstallDir') $script:InstallDir (Get-RegValue $script:AppRoot 'InstallDir')
    Assert-Equal ('marker ExePath') $exe (Get-RegValue $script:AppRoot 'ExePath')
    Assert-Equal ('marker Mode') 'install' (Get-RegValue $script:AppRoot 'Mode')
    Assert-True ('marker Extensions contains .7z') (([string](Get-RegValue $script:AppRoot 'Extensions')).Split(';') -contains $script:ProbeExt)
    Assert-True ('marker InstalledAt is recorded once') ((Get-RegKind $script:AppRoot 'InstalledAt') -ne $null)

    Assert-Equal ('uninstall DisplayName') 'PrismArchive' (Get-RegValue $script:UninstallKey 'DisplayName')
    Assert-Equal ('uninstall DisplayIcon') $expectedIcon (Get-RegValue $script:UninstallKey 'DisplayIcon')
    Assert-Equal ('uninstall InstallLocation') $script:InstallDir (Get-RegValue $script:UninstallKey 'InstallLocation')
    $uninstallString = [string](Get-RegValue $script:UninstallKey 'UninstallString')
    Assert-True ('uninstall string runs the installed uninstall.ps1') ($uninstallString.Contains((Join-Path $script:InstallDir 'uninstall.ps1')) -and $uninstallString.Contains('-File'))
    Assert-True ('quiet uninstall string adds -Quiet') ([string](Get-RegValue $script:UninstallKey 'QuietUninstallString')).Contains('-Quiet')

    $installedEngine = [string](Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL')
    Assert-Equal ('engine override recorded in the effective environment key') $script:EngineDll $installedEngine

    Assert-True ('installed exe exists') (Test-Path -LiteralPath $exe -PathType Leaf)
    Assert-True ('installed uninstaller exists') (Test-Path -LiteralPath (Join-Path $script:InstallDir 'uninstall.ps1') -PathType Leaf)
    $sourceShell = @('PrismShellExt.dll', 'PrismShellExtClassic.dll')
    foreach ($name in $sourceShell) {
        if (Test-Path -LiteralPath (Join-Path $script:SourceDir $name) -PathType Leaf) {
            Assert-True ('installed payload: ' + $name) (Test-Path -LiteralPath (Join-Path $script:InstallDir $name) -PathType Leaf)
        } else {
            Skip-Check ('installed payload: ' + $name) 'not built next to PrismArchive.exe'
        }
    }

    $iconCount = Test-ExeHasIcon $exe
    if ($null -eq $iconCount) {
        Skip-Check 'installed exe carries an icon resource' 'ExtractIconExW unavailable in this PowerShell session'
    } else {
        Assert-True ('installed exe carries an icon resource (DefaultIcon target)') ([bool]$iconCount)
    }

    # "Right-click / Open with" entry: the per-user install owns these keys.
    # The Win11 IExplorerCommand pack is MSIX-owned (C4) and deliberately out of
    # this path; claiming otherwise would make this assertion a lie.
    $menuOk = (Test-RegKey ($app + '\shell\open\command')) -and
               (Test-RegKey ($app + '\SupportedTypes')) -and
               (Test-RegKey ($prog + '\shell\open\command')) -and
               ((Get-RegKind ($script:ClassesRoot + '\' + $script:ProbeExt + '\OpenWithProgids') (Get-ProgId $script:ProbeExt)) -ne $null)
    Assert-True ('shell menu entry (Open with -> PrismArchive, verb=open) is registered for .7z') $menuOk
}

function Assert-UninstalledState {
    $prog = $script:ClassesRoot + '\' + (Get-ProgId $script:ProbeExt)
    Assert-True ('ProgID tree removed') (-not (Test-RegKey $prog))
    Assert-True ('Applications entry removed') (-not (Test-RegKey ($script:ClassesRoot + '\Applications\PrismArchive.exe')))
    Assert-True ('marker removed') (-not (Test-RegKey $script:AppRoot))
    Assert-True ('uninstall entry removed') (-not (Test-RegKey $script:UninstallKey))
    Assert-True ('install directory removed') (-not (Test-Path -LiteralPath $script:InstallDir))
}

function Invoke-ShellResolverCheck {
    if ($script:ScopeRoot -ne 'HKCU:\Software') {
        Skip-Check ('shell resolver asks Windows what .7z opens') 'scratch scope is invisible to the shell by design; the real run checks this'
        return
    }
    $choice = Get-UserChoice $script:ProbeExt
    if ($choice.Pinned -and ($choice.ProgId -notlike 'PrismArchive.*')) {
        Skip-Check ('shell resolver asks Windows what .7z opens') ('Windows user choice is pinned to ' + $choice.ProgId + ' (' + $choice.Source + '); finish the switch in Settings > Default apps. No non-elevated installer can rewrite a hash-protected choice')
        return
    }
    try {
        if (-not ('PrismC39.ShellApi' -as [type])) {
            Add-Type -Namespace PrismC39 -Name ShellApi -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shlwapi.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, ExactSpelling = true)]
public static extern int AssocQueryStringW(uint flags, uint str, string assoc, string extra, System.Text.StringBuilder outBuf, ref uint outLen);
'@
        }
    } catch {
        Skip-Check 'shell resolver asks Windows what .7z opens' ('Add-Type failed: ' + $_.Exception.Message)
        return
    }
    $resolve = {
        param([string]$ext)
        $sb = New-Object System.Text.StringBuilder 1024
        $len = [uint32]1024
        $hr = [PrismC39.ShellApi]::AssocQueryStringW(0, 1, $ext, $null, $sb, [ref]$len)
        return [pscustomobject]@{ Hr = $hr; Text = $sb.ToString() }
    }
    $cal = & $resolve '.txt'
    if ($cal.Hr -ne 0) {
        Skip-Check 'shell resolver asks Windows what .7z opens' ('resolver cannot resolve even .txt (hr=0x' + ('{0:X8}' -f $cal.Hr) + '); the probe is uncalibrated in this session')
        return
    }
    $resolved = & $resolve $script:ProbeExt
    if ($resolved.Hr -eq 0 -and $resolved.Text.Contains('PrismArchive')) {
        Assert-True ('shell resolver returns our installed command') ($resolved.Text.Contains($script:InstallDir))
    } else {
        Skip-Check 'shell resolver asks Windows what .7z opens' ('still routes to [' + $resolved.Text + '] (hr=0x' + ('{0:X8}' -f $resolved.Hr) + '), not to our command; registry is correct, OS default is a user setting')
    }
}

function Invoke-HeadlessProof {
    param([string]$Exe, [string]$Archive, [string]$Engine)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = '--test "' + $Archive + '"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    $psi.EnvironmentVariables['PRISM_7Z_DLL'] = $Engine

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $exited = $p.WaitForExit(30000)
    if (-not $exited) {
        try { $p.Kill() } catch { }
        try { [void]$p.WaitForExit(5000) } catch { }
    }
    $stderrText = ''
    $stdoutText = ''
    try { $stderrText = $errTask.Result } catch { }
    try { $stdoutText = $outTask.Result } catch { }
    return [pscustomobject]@{
        Exited = $exited
        ExitCode = $(if ($exited) { $p.ExitCode } else { -1 })
        StdErr = $stderrText
        StdOut = $stdoutText
    }
}

# ---------------------------------------------------------------------------
# resolve inputs / scope
# ---------------------------------------------------------------------------

$script:SourceDir = Resolve-Source $SourceDir
$script:EngineDll = Resolve-Engine $SevenZipDll
$script:ProbeExt = if ($Extensions.Count -gt 0) { [string]$Extensions[0] } else { '.7z' }
if (-not $script:ProbeExt.StartsWith('.')) { Stop-Usage ('extension must start with a dot: ' + $script:ProbeExt) }
foreach ($e in $Extensions) {
    if (-not ([string]$e).StartsWith('.')) { Stop-Usage ('extension must start with a dot: ' + [string]$e) }
}

$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$script:WorkDir = Join-Path $env:TEMP ('prism-c39-verify-' + $PID + '-' + $tag)
New-Item -ItemType Directory -Force -Path $script:WorkDir | Out-Null

$realScope = $false
if ($ScopeRoot) {
    if ($ScopeRoot -eq 'HKCU:\Software') { $realScope = $true }
} elseif ($Sandbox) {
    $ScopeRoot = 'HKCU:\Software\PrismArchiveVerify\c39-' + $PID + '-' + $tag
} else {
    $ScopeRoot = 'HKCU:\Software'
    $realScope = $true
}
if ($Sandbox -and $realScope) { Stop-Usage '-Sandbox cannot be combined with the real scope HKCU:\Software' }
if ($ScopeRoot -eq 'HKCU:\Software') { $realScope = $true }
$script:RealScope = $realScope

if ($Mutate -and ($Mutate -ne 'UnquotedVerb') -and ($Mutate -ne 'NoRestore')) {
    Stop-Usage ('unknown -Mutate ' + $Mutate + ' (valid: UnquotedVerb, NoRestore)')
}
if ($Mutate -and $realScope) {
    Stop-Usage '-Mutate requires -Sandbox: a deliberately broken install/uninstall must never touch the real user associations'
}

if ($InstallDir) {
    $script:InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
} elseif ($Sandbox) {
    $script:InstallDir = Join-Path $env:TEMP ('prism-c39-install-' + $PID + '-' + $tag)
} else {
    $script:InstallDir = Join-Path $env:LOCALAPPDATA 'PrismArchive'
}

$script:ScopeRoot = $ScopeRoot
$script:ClassesRoot = $ScopeRoot + '\Classes'
$script:AppRoot = $ScopeRoot + '\PrismArchive'
$script:UninstallRoot = $ScopeRoot + '\Microsoft\Windows\CurrentVersion\Uninstall'
$script:UninstallKey = $script:UninstallRoot + '\PrismArchive'
if ($realScope) { $script:EnvRoot = 'HKCU:\Environment' } else { $script:EnvRoot = $ScopeRoot + '\Environment' }

Say '== PrismArchive install end-to-end (C39) =='
Say ('scope:      ' + $script:ScopeRoot + $(if ($realScope) { ' (real HKCU)' } else { ' (scratch)' }))
Say ('install:    ' + $script:InstallDir)
Say ('payload:    ' + $script:SourceDir)
Say ('engine:     ' + $script:EngineDll)
Say ('extension:  ' + $script:ProbeExt)
Say ('work:       ' + $script:WorkDir)
if ($script:Mutation) { Say ('mutation:   ' + $script:Mutation + ' (negative control)') 'warn' }

# Refuse to overwrite a real install we did not create.
if ($realScope -and (Test-RegKey $script:AppRoot)) {
    if (-not $CleanFirst) {
        Say 'VERIFY-INSTALL: FAIL (an existing PrismArchive install is present; run tools\uninstall.ps1 first, or pass -CleanFirst)' 'error'
        exit 1
    }
    Say 'PRE-EXISTING: uninstalling the old install first (after backup) ...' 'warn'
    $preLines = Get-Snapshot
    $script:BackupJson = Join-Path $script:WorkDir 'assoc-backup-preclean.json'
    @{ capturedAt = (Get-Date).ToString('o'); scope = $script:ScopeRoot; lines = $preLines } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $script:BackupJson -Encoding UTF8
    $pre = Invoke-UninstallStep 'pre-existing uninstall'
    if ($pre.ExitCode -ne 0 -or (Test-RegKey $script:AppRoot)) {
        Say 'VERIFY-INSTALL: FAIL (could not remove the pre-existing install; refusing to continue)' 'error'
        exit 1
    }
    Say 'PRE-EXISTING: clean' 'pass'
}
if ($realScope -and (Test-Path -LiteralPath $script:InstallDir) -and -not (Test-RegKey $script:AppRoot)) {
    if ($CleanFirst) {
        # A previous uninstall can leave settings.json behind when the app's
        # handle outlives the delete.  Preserve it, do not silently destroy it.
        $leftover = Join-Path $script:WorkDir 'leftover-install-dir'
        Move-Item -LiteralPath $script:InstallDir -Destination $leftover -Force
        Say ('PRE-EXISTING: moved leftover install directory (no marker) to ' + $leftover) 'warn'
    } else {
        Say ('VERIFY-INSTALL: FAIL (install directory exists without a PrismArchive marker: ' + $script:InstallDir + '; refusing to delete unknown files; rerun with -CleanFirst to preserve and continue)') 'error'
        exit 1
    }
}
if (-not $realScope -and (Test-RegKey $script:AppRoot)) { Remove-RegTree $script:ScopeRoot }
if (-not $realScope -and (Test-Path -LiteralPath $script:InstallDir)) { Remove-Item -LiteralPath $script:InstallDir -Recurse -Force -ErrorAction SilentlyContinue }

# ---------------------------------------------------------------------------
# backup
# ---------------------------------------------------------------------------

$before = Get-Snapshot
$script:BackupJson = Join-Path $script:WorkDir 'assoc-backup.json'
@{
    capturedAt = (Get-Date).ToString('o')
    scope = $script:ScopeRoot
    realScope = $realScope
    lines = $before
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $script:BackupJson -Encoding UTF8

$script:BackupReg = Join-Path $script:WorkDir 'assoc-.7z.reg'
if (Test-RegKey ($script:ClassesRoot + '\' + $script:ProbeExt)) {
    $native = ($script:ClassesRoot + '\' + $script:ProbeExt).Replace('HKCU:\', 'HKCU\')
    & reg.exe export $native $script:BackupReg /y *> $null
    if ($LASTEXITCODE -ne 0) { Say ('WARN  reg.exe export failed for ' + $native + ' (JSON snapshot still captured)') 'warn' }
}
Say ('backup:     ' + $script:BackupJson)
if (Test-Path -LiteralPath $script:BackupReg) { Say ('backup:     ' + $script:BackupReg + '  (recover: reg import "' + $script:BackupReg + '")') }
Say ('before:     ' + $before.Count + ' registry snapshot lines')

# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

try {
    # 0. uninstall when nothing is installed must be a safe no-op.
    $clean0 = Invoke-UninstallStep 'uninstall before install (must be a no-op)'
    Assert-Equal 'uninstall before install exits 0' 0 $clean0.ExitCode
    Assert-True 'uninstall before install says already clean' ((Join-Output $clean0.Output) -match 'UNINSTALL: already clean')
    Compare-Snapshots 'uninstall before install left the registry unchanged' $before (Get-Snapshot)

    # 1. install
    $first = Invoke-InstallStep 'install #1'
    $script:Installed = $true
    Assert-Equal 'install #1 exits 0' 0 $first.ExitCode
    Assert-True 'install #1 prints INSTALL: OK' ((Join-Output $first.Output) -match 'INSTALL: OK')
    $firstWrites = @($first.Output | Where-Object { ([string]$_).StartsWith('[write]') }).Count
    Assert-True 'install #1 actually wrote values (accounting, not a stopwatch)' ($firstWrites -gt 10)
    if ($first.ExitCode -ne 0) { throw 'install #1 failed; dependent assertions would only produce noise' }

    $installedSnapshot = Get-Snapshot

    # CONTRACTS D27: prove the injection was observed BEFORE the assertion it
    # breaks. If this ran after Assert-InstalledState, the first failed assertion
    # could not be attributed to the mutation and the verdict would have to wait
    # for the tail of the run -- exactly the shape D27 forbids.
    if ($script:Mutation -eq 'UnquotedVerb') {
        $cmd = [string](Get-RegValue ($script:ClassesRoot + '\' + (Get-ProgId $script:ProbeExt) + '\shell\open\command') '')
        $script:MutationObserved = (-not $cmd.Contains('"%1"')) -and $cmd.Contains('%1')
        if ($script:MutationObserved) {
            Say ('MUTATION FIXTURE: installed command is malformed as injected: ' + $cmd) 'warn'
        }
    }

    Assert-InstalledState

    # 2. install again BEFORE anything writes user data (the app stores
    #    settings.json inside the install directory, which legitimately changes
    #    EstimatedSize; asserting the no-op before that is the real contract).
    $second = Invoke-InstallStep 'install #2 (idempotency)'
    Assert-Equal 'install #2 exits 0' 0 $second.ExitCode
    $secondWrites = @($second.Output | Where-Object { ([string]$_).StartsWith('[write]') }).Count
    Assert-Equal 'install #2 wrote no registry/file values' 0 $secondWrites
    Compare-Snapshots 'install #2 left the registry byte-identical' $installedSnapshot (Get-Snapshot)

    # 3. the installed exe must open a real archive headlessly.
    $fixture = Join-Path $script:WorkDir 'c39-fixture.7z'
    $fixtureB64 = 'N3q8ryccAAQrLwjRKAAAAAAAAABaAAAAAAAAAOh4jacBACNwcmlzbS1hcmNoaXZlIGMzOSBlbmQtdG8tZW5kIGZpeHR1cmUAAQQGAAEJKAAHCwEAASEhAQAMJAAICgHxAhBUAAAFARkMAAAAAAAAAAAAAAAAERUAaABlAGwAbABvAC4AdAB4AHQAAAAUCgEAnuWX6BZq3QEVBgEAIAAAAAAA'
    [System.IO.File]::WriteAllBytes($fixture, [Convert]::FromBase64String($fixtureB64))
    Assert-True 'embedded .7z fixture materialized in a unique temp directory' ((Test-Path -LiteralPath $fixture -PathType Leaf) -and ((Get-Item -LiteralPath $fixture).Length -gt 0))
    $installedExe = Join-Path $script:InstallDir 'PrismArchive.exe'
    $proof = Invoke-HeadlessProof $installedExe $fixture $script:EngineDll
    Assert-True 'installed exe --test exits (no message box, no window)' ([bool]$proof.Exited)
    Assert-Equal 'installed exe --test exit code' 0 $proof.ExitCode
    Assert-True 'installed exe --test prints verify ok=1 items=1' (($proof.StdErr -match 'verify ok=1 items=1') -or ($proof.StdOut -match 'verify ok=1 items=1'))
    if ($proof.StdErr) { Say ('headless stderr: ' + $proof.StdErr.Trim()) }
    if ($proof.StdOut) { Say ('headless stdout: ' + $proof.StdOut.Trim()) }

    Invoke-ShellResolverCheck

    if ($ShellLaunch) {
        if ($script:IsSandbox -or -not $realScope) {
            Skip-Check 'double-click .7z via ShellExecute' 'scratch scope is invisible to the shell'
        } else {
            $choice = Get-UserChoice $script:ProbeExt
            if ($choice.Pinned -and ($choice.ProgId -notlike 'PrismArchive.*')) {
                Skip-Check 'double-click .7z via ShellExecute' ('Windows user choice is pinned to ' + $choice.ProgId + '; use Open with -> PrismArchive or Settings > Default apps')
            } else {
                $beforePids = @(Get-Process -Name 'PrismArchive' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
                try {
                    Start-Process -FilePath $fixture | Out-Null
                    $deadline = (Get-Date).AddSeconds(10)
                    $launched = $null
                    while ((Get-Date) -lt $deadline) {
                        $launched = Get-Process -Name 'PrismArchive' -ErrorAction SilentlyContinue |
                            Where-Object { $beforePids -notcontains $_.Id } | Select-Object -First 1
                        if ($launched) { break }
                        Start-Sleep -Milliseconds 250
                    }
                    if ($launched) {
                        Assert-True 'double-click .7z started the installed exe' (([string]$launched.Path) -eq $installedExe)
                        Stop-Process -Id $launched.Id -Force -ErrorAction SilentlyContinue
                    } else {
                        Skip-Check 'double-click .7z via ShellExecute' 'no new PrismArchive process within 10s (shell routing is a user setting; --test already proved the payload)'
                    }
                } catch {
                    Skip-Check 'double-click .7z via ShellExecute' ('ShellExecute threw: ' + $_.Exception.Message)
                }
            }
        }
    }

    if ($KeepInstalled) {
        Skip-Check 'uninstall restores the association tree' '-KeepInstalled was requested'
        Skip-Check 'uninstall removes our keys and files' '-KeepInstalled was requested'
        Skip-Check 'second uninstall is a safe no-op' '-KeepInstalled was requested'
    } else {
        # 4. uninstall and prove key-by-key restoration.
        $unMutation = ''
        if ($script:Mutation -eq 'NoRestore') { $unMutation = 'NoRestore' }
        $un = Invoke-UninstallStep 'uninstall #1' $unMutation
        $script:Uninstalled = ($un.ExitCode -eq 0)
        Assert-Equal 'uninstall #1 exits 0' 0 $un.ExitCode
        Assert-True 'uninstall #1 prints UNINSTALL: OK' ((Join-Output $un.Output) -match 'UNINSTALL: OK')
        if ($script:Mutation -eq 'NoRestore') {
            $left = [string](Get-RegValue ($script:ClassesRoot + '\' + $script:ProbeExt) '')
            $injection = ((Join-Output $un.Output) -match 'mutation NoRestore')
            $script:MutationObserved = $injection -and ($left -eq (Get-ProgId $script:ProbeExt))
            if ($script:MutationObserved) {
                Say ('MUTATION FIXTURE: .7z was left pointing at ' + $left + ' as injected') 'warn'
            }
        }
        Assert-UninstalledState
        $after = Get-Snapshot
        Compare-Snapshots 'uninstall restored the exact pre-install registry state' $before $after

        # 5. uninstall again: safe no-op, no further changes.
        $un2 = Invoke-UninstallStep 'uninstall #2 (must be a no-op)'
        Assert-Equal 'second uninstall exits 0' 0 $un2.ExitCode
        Assert-True 'second uninstall says already clean' ((Join-Output $un2.Output) -match 'UNINSTALL: already clean')
        Compare-Snapshots 'second uninstall left the registry unchanged' $after (Get-Snapshot)
    }
} catch {
    Say ('VERIFY-INSTALL: FAIL (harness threw: ' + $_.Exception.Message + ')') 'error'
    Say ($_.ScriptStackTrace) 'error'
    Add-Failure ('harness threw: ' + $_.Exception.Message)
} finally {
    if ($script:Installed -and -not $script:Uninstalled -and -not $KeepInstalled) {
        Say 'CLEANUP: uninstall after an aborted run ...' 'warn'
        try {
            $cleanup = Invoke-UninstallStep 'cleanup uninstall'
            if ($cleanup.ExitCode -eq 0) { $script:Uninstalled = $true }
        } catch {
            Say ('CLEANUP: uninstall threw: ' + $_.Exception.Message) 'error'
        }
    }
    if (-not $realScope) {
        # Scratch scope is harness state, not product state: remove it even if an
        # assertion failed, so the next run starts clean.
        try { if (Test-RegKey $script:ScopeRoot) { Remove-RegTree $script:ScopeRoot } } catch { }
        try { if (Test-Path -LiteralPath $script:InstallDir) { Remove-Item -LiteralPath $script:InstallDir -Recurse -Force -ErrorAction SilentlyContinue } } catch { }
    }
}

# ---------------------------------------------------------------------------
# verdict
# ---------------------------------------------------------------------------

if ($script:Mutation) {
    if (-not $script:MutationObserved) {
        Say ('VERIFY-INSTALL(MUTATION ' + $script:Mutation + '): NOT INJECTED -- the code path did not change; a red result would be meaningless') 'error'
        exit 1
    }
    # Reached only if a failure never went through Add-Failure (there is none
    # today): the D27 verdict was printed and the process left at the first one.
    if ($script:Failures.Count -gt 0) {
        Say ('VERIFY-INSTALL(MUTATION ' + $script:Mutation + '): CAUGHT (' + $script:Failures.Count + ' assertion(s) went red; injection observed)')
        exit 0
    }
    Say ('VERIFY-INSTALL(MUTATION ' + $script:Mutation + '): NOT CAUGHT -- the guard is blind to the injected defect') 'error'
    exit 1
}

if ($script:Failures.Count -gt 0) {
    Say ('VERIFY-INSTALL: FAIL (' + $script:Failures.Count + ' of ' + $script:Checks + ' checks; ' + $script:Skips.Count + ' explicit skip(s))') 'error'
    exit 1
}

Say ('VERIFY-INSTALL: PASS (' + $script:Checks + ' checks, ' + $script:Skips.Count + ' explicit skip(s); backup: ' + $script:BackupJson + ')')
exit 0
