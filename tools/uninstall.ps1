#requires -Version 5.1
<#
.SYNOPSIS
  Remove everything tools/install.ps1 wrote: associations (restoring whatever
  they pointed at before), the ProgID/Application entries, the uninstall entry,
  the user engine override, and the install directory.

.DESCRIPTION
  This script is copied into the install directory by the installer and is also
  the UninstallString in the registry, so it must be able to delete the folder it
  is running from. It re-launches itself from %TEMP% once and turns the original
  process into a thin waiter -- on Windows a script cannot reliably delete its own
  directory while the interpreter still has it open.

  Every step is idempotent and tolerant of half-installed state: whatever exists
  is removed, whatever does not is skipped, and a second run prints
  "UNINSTALL: already clean".

.PARAMETER ScopeRoot
  Registry base, defaults to HKCU:\Software (the installer's default). The
  self-test passes a scratch key.

.PARAMETER DryRun
  Print what would be removed, change nothing.

.EXAMPLE
  pwsh -File tools\uninstall.ps1 -DryRun
  pwsh -File tools\uninstall.ps1
#>
[CmdletBinding()]
param(
    [string]$ScopeRoot = 'HKCU:\Software',
    [string]$InstallDir = '',
    [switch]$DryRun,
    [switch]$Verify,
    [switch]$Quiet,
    [switch]$ReLaunched,
    [string]$Mutate = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:DryRun = [bool]$DryRun
$script:Quiet = [bool]$Quiet
$script:Removed = 0
$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:ExitCode = 0

function Say {
    param([string]$Text, [string]$Level = 'info')
    if ($script:Quiet -and $Level -eq 'info') { return }
    $prefix = switch ($Level) {
        'warn' { 'WARN  ' }
        'error' { 'ERROR ' }
        'plan' { '[dry-run] ' }
        'done' { '[removed] ' }
        'same' { '[same] ' }
        default { '' }
    }
    Write-Host ($prefix + $Text)
}

function Warn {
    param([string]$Text)
    $script:Warnings.Add($Text)
    Say $Text 'warn'
}

# ---------------------------------------------------------------------------
# registry primitives (self-contained: this file is deployed alone)
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
    param([string]$Path, [switch]$Create)
    $h = Get-Hive $Path
    if ($Create) { return $h[0].CreateSubKey($h[1], $true) }
    return $h[0].OpenSubKey($h[1], $true)
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
    if ($script:DryRun) { Say ('delete key tree ' + $Path) 'plan'; return }
    $h = Get-Hive $Path
    $h[0].DeleteSubKeyTree($h[1], $false)
    $script:Removed++
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
        $script:Removed++
        Say ('deleted value ' + $Path + ' [' + $Name + ']') 'done'
    } finally { $k.Close() }
}

function Remove-EmptyKeys {
    <# Walk up from Path, deleting keys that WE left empty, stopping at StopAt.
       "Clean uninstall" means the pre-install registry state, and Windows has no
       notion of an empty key: a key we created must not survive the uninstall. #>
    param([string]$Path, [string]$StopAt)
    $stop = $StopAt.TrimEnd('\')
    $p = $Path.TrimEnd('\')
    while ($p -and $p.Length -gt $stop.Length -and $p.StartsWith($stop + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        if (-not (Test-RegKey $p)) { $p = Split-Path -Parent $p; continue }
        $k = Open-RegKey $p
        if ($null -eq $k) { $p = Split-Path -Parent $p; continue }
        $empty = ($k.SubKeyCount -eq 0) -and ($k.ValueCount -eq 0)
        $k.Close()
        if (-not $empty) { break }
        if ($script:DryRun) {
            Say ('delete empty key ' + $p) 'plan'
        } else {
            $h = Get-Hive $p
            $h[0].DeleteSubKeyTree($h[1], $false)
            $script:Removed++
            Say ('deleted empty key ' + $p) 'done'
        }
        $p = Split-Path -Parent $p
    }
}

function Set-RegString {
    param([string]$Path, [string]$Name = '', [string]$Value)
    if ($script:DryRun) { Say ('restore ' + $Path + ' [' + $Name + '] = ' + $Value) 'plan'; return }
    $k = Open-RegKey $Path -Create
    try { $k.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String) } finally { $k.Close() }
    $script:Removed++
    Say ('restored ' + $Path + ' [' + $Name + '] = ' + $Value) 'done'
}

# ---------------------------------------------------------------------------
# scope
# ---------------------------------------------------------------------------

$script:ScopeRoot = $ScopeRoot
$script:ClassesRoot = $ScopeRoot + '\Classes'
$script:AppRoot = $ScopeRoot + '\PrismArchive'
$script:UninstallKey = $ScopeRoot + '\Microsoft\Windows\CurrentVersion\Uninstall\PrismArchive'
function Get-EnvRootPath {
    <# Same mapping as the installer: HKCU\Environment is hive-root, a scratch
       scope keeps its own copy so the self-test round trip cannot touch the real
       value. Kept in step with tools/install.ps1 (its self-test asserts the
       mapping; the two files are deployed separately and cannot share code). #>
    param([string]$Scope, [string]$Mutate = '')
    if ($Mutate -eq 'EnvUnderSoftware') { return $Scope + '\Environment' }
    if ($Scope -eq 'HKCU:\Software') { return 'HKCU:\Environment' }
    return $Scope + '\Environment'
}

$script:EnvRoot = Get-EnvRootPath $ScopeRoot $Mutate
$script:BackupRoot = $script:AppRoot + '\AssocBackup'

function Get-ProgId {
    param([string]$Ext)
    return 'PrismArchive' + $Ext
}

# ---------------------------------------------------------------------------
# entry: relocate out of the directory we are about to delete
# ---------------------------------------------------------------------------

$markerDir = Get-RegValue $script:AppRoot 'InstallDir'
$targetDir = if ($InstallDir) { $InstallDir } elseif ($markerDir) { [string]$markerDir } else { '' }
$selfPath = $PSCommandPath

if (-not $ReLaunched -and $selfPath -and $targetDir) {
    $normalized = [System.IO.Path]::GetFullPath($targetDir).TrimEnd('\')
    if ($selfPath.StartsWith($normalized + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $tempCopy = Join-Path $env:TEMP ('prism-uninstall-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.ps1')
        Copy-Item -LiteralPath $selfPath -Destination $tempCopy -Force
        $psExe = (Get-Process -Id $PID).Path
        $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $tempCopy,
                  '-ScopeRoot', $script:ScopeRoot, '-ReLaunched')
        if ($targetDir) { $argv += @('-InstallDir', $targetDir) }
        if ($script:DryRun) { $argv += '-DryRun' }
        if ($script:Quiet) { $argv += '-Quiet' }
        if ($Verify) { $argv += '-Verify' }
        if ($Mutate) { $argv += @('-Mutate', $Mutate) }
        & $psExe @argv
        $code = $LASTEXITCODE
        Remove-Item -LiteralPath $tempCopy -Force -ErrorAction SilentlyContinue
        exit $code
    }
}

# ---------------------------------------------------------------------------
# uninstall
# ---------------------------------------------------------------------------

try {
    if (-not (Test-RegKey $script:AppRoot) -and -not ($targetDir -and (Test-Path -LiteralPath $targetDir))) {
        Write-Host 'UNINSTALL: already clean (no PrismArchive marker, no install directory)'
        exit 0
    }

    $extensions = @()
    $recorded = Get-RegValue $script:AppRoot 'Extensions'
    if ($recorded) { $extensions = @(([string]$recorded) -split ';' | Where-Object { $_ }) }
    if ($extensions.Count -eq 0) {
        # Half-installed state: recover the ProgIDs we created from the backup
        # subtree instead of guessing the extension list.
        if (Test-RegKey $script:BackupRoot) {
            $h = Get-Hive $script:BackupRoot
            $k = $h[0].OpenSubKey($h[1])
            if ($k) {
                $extensions = @($k.GetSubKeyNames())
                $k.Close()
            }
        }
    }

    $engine = [string](Get-RegValue $script:AppRoot 'Engine')
    $enginePrevious = [string](Get-RegValue $script:AppRoot 'EnginePrevious')
    $engineHadPrevious = Get-RegValue $script:AppRoot 'EngineHadPrevious'

    # 1. associations: restore the handler that was there before, or drop ours
    foreach ($ext in $extensions) {
        $progId = Get-ProgId $ext
        $extKey = $script:ClassesRoot + '\' + $ext
        $current = Get-RegValue $extKey ''
        $backupKey = $script:BackupRoot + '\' + $ext
        $present = Get-RegValue $backupKey 'Present'
        $previous = Get-RegValue $backupKey 'Previous'

        if ([string]$current -eq $progId) {
            if ($Mutate -eq 'NoRestore') {
                # negative control (CONTRACTS D3): leave the association pointing
                # at us; the self-test must notice.
                Say ('mutation NoRestore: leaving ' + $ext + ' pointing at ' + $progId) 'warn'
            } elseif ($null -ne $present -and [int]$present -eq 1) {
                Set-RegString $extKey '' ([string]$previous)
            } else {
                Remove-RegValue $extKey ''
            }
        } else {
            Say ('left ' + $ext + ' alone: it now points at ' + [string]$current + ', not at us') 'same'
        }
        Remove-RegValue ($extKey + '\OpenWithProgids') $progId
        Remove-RegTree ($script:ClassesRoot + '\' + $progId)
        Remove-EmptyKeys ($extKey + '\OpenWithProgids') $script:ScopeRoot
        Remove-EmptyKeys $extKey $script:ScopeRoot
    }

    # 2. application registration, marker, uninstall entry
    Remove-RegTree ($script:ClassesRoot + '\Applications\PrismArchive.exe')
    Remove-EmptyKeys ($script:ClassesRoot + '\Applications') $script:ScopeRoot
    Remove-RegTree $script:AppRoot
    Remove-RegTree $script:UninstallKey
    Remove-EmptyKeys (Split-Path -Parent $script:UninstallKey) $script:ScopeRoot

    # 3. engine override: put back what the user had
    $currentEngine = Get-RegValue $script:EnvRoot 'PRISM_7Z_DLL'
    if ($engine -and [string]$currentEngine -eq $engine) {
        if ($null -ne $engineHadPrevious -and [int]$engineHadPrevious -eq 1) {
            Set-RegString $script:EnvRoot 'PRISM_7Z_DLL' $enginePrevious
        } else {
            Remove-RegValue $script:EnvRoot 'PRISM_7Z_DLL'
        }
        Remove-EmptyKeys $script:EnvRoot $script:ScopeRoot
    } elseif ($engine) {
        Say ('left PRISM_7Z_DLL alone: it now points at ' + [string]$currentEngine) 'same'
    }

    # 4. files
    if ($targetDir -and (Test-Path -LiteralPath $targetDir)) {
        if ($script:DryRun) {
            Say ('delete directory ' + $targetDir) 'plan'
        } else {
            Get-ChildItem -LiteralPath $targetDir -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $targetDir -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $targetDir) {
                Warn ('could not fully remove ' + $targetDir + ' (a file is in use); remove it after closing PrismArchive')
            } else {
                $script:Removed++
                Say ('deleted directory ' + $targetDir) 'done'
            }
        }
    }

    if ($script:DryRun) {
        Say ('DRYRUN: OK would remove PrismArchive from ' + $script:ScopeRoot)
    } else {
        Write-Host ('UNINSTALL: OK (' + $script:Removed + ' operations' + $(if ($script:Warnings.Count -gt 0) { ', ' + $script:Warnings.Count + ' warning(s)' } else { '' }) + ')')
    }

    if ($Verify -and -not $script:DryRun) {
        Add-Type -Namespace Prism -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shlwapi.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, ExactSpelling = true)]
public static extern int AssocQueryStringW(uint flags, uint str, string assoc, string extra, System.Text.StringBuilder outBuf, ref uint outLen);
'@ -ErrorAction Stop
        $failed = 0
        $probe = '.7z'
        $sb = New-Object System.Text.StringBuilder 1024
        $len = [uint32]1024
        $hr = [Prism.Shell]::AssocQueryStringW(0, 1, $probe, $null, $sb, [ref]$len)
        if ($hr -ne 0 -or -not $sb.ToString().Contains('PrismArchive')) {
            Say ('VERIFY: PASS shell resolver no longer resolves ' + $probe + ' to PrismArchive')
        } else {
            Say ('VERIFY: FAIL shell resolver still returns ' + $sb.ToString()) 'error'
            $failed++
        }
        $left = Get-RegValue ($script:ClassesRoot + '\' + $probe) ''
        if ([string]$left -ne 'PrismArchive.7z') {
            Say ('VERIFY: PASS registry readback ' + $probe + ' -> ' + [string]$left)
        } else {
            Say 'VERIFY: FAIL registry still points at PrismArchive.7z' 'error'
            $failed++
        }
        if ($failed -eq 0) { Say 'VERIFY: PASSED (associations removed)' } else { $script:ExitCode = 1 }
    }
} catch {
    Say $_.Exception.Message 'error'
    Say $_.ScriptStackTrace 'error'
    $script:ExitCode = 1
}

exit $script:ExitCode
