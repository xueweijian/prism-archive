# Static right-click verbs for PrismArchive.
#
# Registered on "*" (every file), like 7-Zip does, and NOT per extension.
# Reason, measured on this machine: when the archive association is pinned by
# the Windows user choice (hash protected), the shell ignores verbs that live
# on the extension key (.7z\shell\...) -- they do not appear at all -- while
# the "*" entry does. Registering both also produced two identical menu lines.
# So: one place, the one that works everywhere.
param(
  [string] $ExePath = "$env:LOCALAPPDATA\PrismArchive\PrismArchive.exe",
  [switch] $Unregister,
  [switch] $Quiet
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $ExePath) -and -not $Unregister) { throw "exe not found: $ExePath" }

$verbs = @(
  @{ id = "PrismArchiveExtract"; label = "用 PrismArchive 解压到此处"; args = "--extract-here" },
  @{ id = "PrismArchiveOpen";    label = "用 PrismArchive 打开";       args = "" }
)

$classes = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes", $true)
if (-not $classes) { throw "cannot open HKCU\Software\Classes" }
$written = 0; $removed = 0
foreach ($v in $verbs) {
  $path = "*\shell\" + $v.id
  if ($Unregister) {
    try { $classes.DeleteSubKeyTree($path, $false); $removed++ } catch { }
    continue
  }
  $k = $classes.CreateSubKey($path)
  $k.SetValue("", $v.label, [Microsoft.Win32.RegistryValueKind]::String)
  $k.SetValue("Icon", $ExePath, [Microsoft.Win32.RegistryValueKind]::String)
  $cmd = $k.CreateSubKey("command")
  $line = [char]34 + $ExePath + [char]34
  if ($v.args) { $line = $line + " " + $v.args }
  $line = $line + " " + [char]34 + "%1" + [char]34
  $cmd.SetValue("", $line, [Microsoft.Win32.RegistryValueKind]::String)
  $cmd.Close(); $k.Close()
  $written++
}
$classes.Close()
if ($Unregister) { if (-not $Quiet) { Write-Host "STATIC VERBS REMOVED keys=$removed" } }
else { if (-not $Quiet) { Write-Host "STATIC VERBS REGISTERED exe=$ExePath keys=$written" } }