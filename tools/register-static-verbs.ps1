# Static right-click verbs for PrismArchive.
#
# Why static verbs and not only the shellex handler: on a machine where the
# archive association is pinned by the Windows user choice (hash protected),
# the shell does not necessarily load the extension DLL, but plain "shell\verb"
# entries are always shown. This is the entry point the user actually sees, so
# it is part of the install, not an afterthought.
param(
  [string] $ExePath = "$env:LOCALAPPDATA\PrismArchive\PrismArchive.exe",
  [switch] $Unregister,
  [switch] $Quiet
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $ExePath) -and -not $Unregister) { throw "exe not found: $ExePath" }

$exts = @(".7z",".zip",".rar",".tar",".gz",".tgz",".bz2",".tbz2",".xz",".txz",".zst",".cab",".iso",".wim",".arj",".lzh",".cpio",".001")
$verbs = @(
  @{ id = "PrismArchiveExtract"; label = "用 PrismArchive 解压到此处"; args = "--extract-here" },
  @{ id = "PrismArchiveOpen";    label = "用 PrismArchive 打开";       args = "" }
)

# Registry API instead of Set-ItemProperty: the default value of a key has the
# empty name, and Set-ItemProperty -Name "(default)" silently does nothing in
# PowerShell 7 (it worked in 5.1, which is why the first attempt looked fine).
$classes = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes", $true)
if (-not $classes) { throw "cannot open HKCU\Software\Classes" }
$written = 0
$removed = 0
function Set-Verb([string] $scope, [hashtable] $verb) {
  $path = "$scope\shell\$($verb.id)"
  if ($Unregister) {
    try { $classes.DeleteSubKeyTree($path, $false); $script:removed++ } catch { }
    return
  }
  $k = $classes.CreateSubKey($path)
  $k.SetValue("", $verb.label, [Microsoft.Win32.RegistryValueKind]::String)
  $k.SetValue("Icon", $ExePath, [Microsoft.Win32.RegistryValueKind]::String)
  $cmd = $k.CreateSubKey("command")
  $line = [char]34 + $ExePath + [char]34
  if ($verb.args) { $line = $line + " " + $verb.args }
  $line = $line + " " + [char]34 + "%1" + [char]34
  $cmd.SetValue("", $line, [Microsoft.Win32.RegistryValueKind]::String)
  $cmd.Close(); $k.Close()
  $script:written++
}

foreach ($ext in $exts) { foreach ($v in $verbs) { Set-Verb $ext $v } }
# One entry on every file, like 7-Zip does: the association may be pinned to
# another archiver, and the user should still find us.
Set-Verb "*" ($verbs[0])
$classes.Close()
if ($Unregister) { if (-not $Quiet) { Write-Host "STATIC VERBS REMOVED keys=$removed" } }
else { if (-not $Quiet) { Write-Host "STATIC VERBS REGISTERED exe=$ExePath keys=$written" } }