# PrismArchive -- per-user registration for the Win10 classic context menu.
#
# Card C4 owns this fragment; card C3's installer (install.ps1) calls it. It does
# NOT install anything: it writes exactly the keys a classic shell extension
# needs, and nothing else.
#
#   .\register-classic.ps1 -DllPath E:\build\wt-c4\Src\PrismWin\PrismShellExtClassic.dll
#   .\register-classic.ps1 -Unregister
#
# Why SystemFileAssociations and not HKCU\Software\Classes\*:
#   a "*" handler is loaded for EVERY right-click on the machine, including
#   folders and text files where our model has nothing to say. Registering per
#   archive extension loads us only where the model produces verbs.
#
# The CLSID must equal prism::win::shell_ids::kClassicMenuClsid
# (Src/PrismWin/SparsePackageRegistrar.hpp); DllGetClassObject rejects anything
# else with CLASS_E_CLASSNOTAVAILABLE.
[CmdletBinding()]
param(
  [string] $DllPath = '',
  [ValidateSet('CurrentUser', 'LocalMachine')] [string] $Scope = 'CurrentUser',
  [switch] $Unregister
)
$ErrorActionPreference = 'Stop'

$Clsid = '{8C1B7E52-1F3A-4B7E-9C41-5B3C0000C100}'
$Extensions = @('.7z', '.zip', '.rar', '.tar', '.gz', '.xz', '.zst', '.bz2', '.cab', '.iso', '.tgz')

$root = 'HKCU:'
if ($Scope -eq 'LocalMachine') { $root = 'HKLM:' }
if ($Scope -eq 'LocalMachine') {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'LocalMachine scope requires an elevated shell'
  }
}

function Get-HandlerKeys([string] $ext) {
  # Every place Explorer has been observed to look, in the order it consults them.
  $plain = $ext.TrimStart('.')
  return @(
    "$root\Software\Classes\$ext\shellex\ContextMenuHandlers\PrismArchive",
    "$root\Software\Classes\PrismArchive.$plain\shellex\ContextMenuHandlers\PrismArchive",
    "$root\Software\Classes\SystemFileAssociations\$ext\shellex\ContextMenuHandlers\PrismArchive"
  )
}

$clsidKey = "$root\Software\Classes\CLSID\$Clsid"
$removed = 0
$written = 0

if ($Unregister) {
  if (Test-Path $clsidKey) { Remove-Item $clsidKey -Recurse -Force; $removed++ }
  foreach ($ext in $Extensions) {
    foreach ($h in (Get-HandlerKeys $ext)) {
      if (Test-Path $h) { Remove-Item $h -Recurse -Force; $removed++ }
    }
  }
  Write-Host "UNREGISTERED scope=$Scope keys=$removed"
  return
}

if (-not $DllPath) { throw '-DllPath is required unless -Unregister is used' }
if (-not (Test-Path $DllPath)) { throw "DLL not found: $DllPath" }
$DllPath = (Resolve-Path $DllPath).Path

# The DLL bitness must match the shell that loads it. HKCU\Software\Classes is
# not redirected on x64 Windows, so a 32-bit Explorer would still see this key --
# we record the bitness instead of guessing.
$bitness = 'unknown'
try {
  $bytes = [IO.File]::ReadAllBytes($DllPath)
  $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
  $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
  if ($machine -eq 0x8664) { $bitness = 'x64' }
  if ($machine -eq 0x14c) { $bitness = 'x86' }
  if ($machine -eq 0xAA64) { $bitness = 'arm64' }
} catch { $bitness = 'unknown' }

New-Item -Path $clsidKey -Force | Out-Null
New-ItemProperty -Path $clsidKey -Name '(default)' -Value 'PrismArchive classic context menu' -PropertyType String -Force | Out-Null

$inproc = "$clsidKey\InprocServer32"
New-Item -Path $inproc -Force | Out-Null
New-ItemProperty -Path $inproc -Name '(default)' -Value $DllPath -PropertyType String -Force | Out-Null
New-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Apartment' -PropertyType String -Force | Out-Null
$written++

# Explorer resolves context-menu handlers down the association chain:
#   extension key  ->  ProgID                (SystemFileAssociations is an
#   extra hint that only some file types honour). On Windows 11 the classic
#   menu reads the extension key and the ProgID; registering ONLY under
#   SystemFileAssociations left the entry invisible in practice, which is
#   exactly what the user reported ("7-Zip shows, PrismArchive does not").
foreach ($ext in $Extensions) {
  foreach ($handler in (Get-HandlerKeys $ext)) {
    New-Item -Path $handler -Force | Out-Null
    New-ItemProperty -Path $handler -Name '(default)' -Value $Clsid -PropertyType String -Force | Out-Null
    $written++
  }
}

Write-Host "REGISTERED scope=$Scope dll=$DllPath bitness=$bitness keys=$written"
Write-Host 'Restart Explorer (or sign out/in) -- the shell caches context-menu handlers per process.'
