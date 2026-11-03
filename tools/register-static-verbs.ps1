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

# The labels are the user-visible Chinese strings but they are built from code
# points, NOT written as literals. Reason, measured on the C68 clean-machine run:
# Windows PowerShell 5.1 reads a BOM-less script with the ANSI code page (1252
# on an English runner). UTF-8 Chinese bytes then decode to characters such as
# U+201D (a smart quote PowerShell treats as a string delimiter), so the file
# does not even PARSE -- the whole verb registration dies before writing a key,
# and the install used to fail with it. An ASCII-only file parses in every code
# page and still writes the correct labels.
$labelExtract = ([string][char]0x7528) + ' PrismArchive ' + ([string][char]0x89E3) + ([string][char]0x538B) + ([string][char]0x5230) + ([string][char]0x6B64) + ([string][char]0x5904)
$labelOpen = ([string][char]0x7528) + ' PrismArchive ' + ([string][char]0x6253) + ([string][char]0x5F00)
$verbs = @(
  @{ id = "PrismArchiveExtract"; label = $labelExtract; args = "--extract-here" },
  @{ id = "PrismArchiveOpen";    label = $labelOpen;    args = "" }
)

# CreateSubKey, not OpenSubKey: on a brand-new user profile (and on a clean CI
# runner) HKCU\Software\Classes does not exist yet, and OpenSubKey returns null
# there -- which used to abort the whole install with a null-reference throw.
# Legacy keys from before 0.1.0-C68. The early ad-hoc registration wrote
# PrismArchiveExtractAll / PrismArchiveOpenAll; leaving them behind shows the
# user TWO identical "解压到此处" lines in the shell menu forever, because an
# upgrade only rewrites the current names. Delete them on every run.
foreach ($legacy in @("PrismArchiveExtractAll", "PrismArchiveOpenAll", "PrismArchiveOpenAll2")) {
  try { $classes.DeleteSubKeyTree("*\shell\" + $legacy, $false) } catch { }
}

$classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("Software\Classes")
if (-not $classes) { throw "cannot create HKCU\Software\Classes" }
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