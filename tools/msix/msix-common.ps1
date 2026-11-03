# PrismArchive -- shared helpers for the certificate-free MSIX paths (card C61).
#
# Dot-sourced by register-loose.ps1, unregister-loose.ps1 and
# selfsign-and-install.ps1. It exists so the three entry points cannot drift on
# the things that actually break:
#
#   * which manifest is being registered (the search order is data, not prose),
#   * what a manifest must contain before Windows will accept it,
#   * how a deployment HRESULT becomes an actionable sentence.
#
# Every reason string below was produced by a real deployment attempt on
# 2026-11-02 (Win11 26200, non-admin, Developer Mode on). The numbers are the
# HRESULTs Add-AppxPackage actually printed, not documentation guesses:
#
#   -Register without -ExternalLocation, sparse manifest  -> 0x80073CF9
#       "must be installed using an external location"
#   -Register -ExternalLocation, AllowExternalContent off -> 0x80073D2E
#       "must allow external location deployment"
#   malformed XML / missing Identity/@Publisher           -> 0x80080204
#       "the package manifest is invalid"
#
# NOTE on HResult: the PowerShell wrapper reports 0x80131620 / 0x80131500 and
# hides the deployment code inside the message text, so ConvertFrom-DeployError
# reads the message first and only falls back to Exception.HResult.
# Callers stay ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI,
# and this project builds on machines with a Chinese code page.

$script:MsixNamespaces = [ordered]@{
  a        = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'
  uap      = 'http://schemas.microsoft.com/appx/manifest/uap/windows10'
  uap10    = 'http://schemas.microsoft.com/appx/manifest/uap/windows10/10'
  com      = 'http://schemas.microsoft.com/appx/manifest/com/windows10'
  desktop4 = 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4'
  desktop5 = 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5'
  rescap   = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities'
}

function New-MsixNamespaceManager {
  param([Parameter(Mandatory = $true)] [xml] $Xml)

  $manager = [System.Xml.XmlNamespaceManager]::new($Xml.NameTable)
  foreach ($prefix in $script:MsixNamespaces.Keys) {
    $manager.AddNamespace($prefix, $script:MsixNamespaces[$prefix])
  }
  # -NoEnumerate is load-bearing: XmlNamespaceManager implements IEnumerable
  # (it enumerates namespace PREFIXES), so a plain "return $manager" hands the
  # caller the strings '', 'xmlns', 'xml' instead of the manager and every
  # SelectSingleNode(xpath, manager) call then dies with "no overload ... 2".
  Write-Output -NoEnumerate $manager
}

# One place that decides where a staged manifest may live. Returned as data so
# register-loose.ps1 can print every path it tried when nothing is found.
function Get-MsixManifestSearchPaths {
  param(
    [string] $StageDir = '',
    [string] $BuildDir = '',
    [string] $RepoRoot = ''
  )

  $candidates = New-Object System.Collections.Generic.List[string]
  if ($StageDir) { $candidates.Add((Join-Path $StageDir 'AppxManifest.xml')) }
  if ($BuildDir) {
    $candidates.Add((Join-Path $BuildDir 'msix\stage\AppxManifest.xml'))
    $candidates.Add((Join-Path $BuildDir 'out\msix\stage\AppxManifest.xml'))
  }
  if ($RepoRoot) {
    $candidates.Add((Join-Path $RepoRoot 'out\msix\stage\AppxManifest.xml'))
    $candidates.Add((Join-Path $RepoRoot 'build\msix\stage\AppxManifest.xml'))
    # Last resort: the checked-in blueprint. Registering it is only useful if the
    # DLLs sit next to it, which is exactly what build-sparse.ps1 puts there.
    $candidates.Add((Join-Path $RepoRoot 'Assets\Package\AppxManifest.xml'))
  }
  return $candidates
}

function Resolve-MsixManifest {
  param(
    [string] $ManifestPath = '',
    [string] $StageDir = '',
    [string] $BuildDir = '',
    [string] $RepoRoot = ''
  )

  if ($ManifestPath) {
    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ManifestPath)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
      throw (('AppxManifest.xml not found at the path you passed: ' + $resolved) + [Environment]::NewLine +
             'FIX       pass -ManifestPath <build dir>\msix\stage\AppxManifest.xml (build-sparse.ps1 writes it), or -StageDir <dir>.')
    }
    return [pscustomobject]@{ Path = $resolved; Source = 'explicit -ManifestPath' }
  }

  $tried = Get-MsixManifestSearchPaths -StageDir $StageDir -BuildDir $BuildDir -RepoRoot $RepoRoot
  foreach ($candidate in $tried) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return [pscustomobject]@{ Path = (Resolve-Path -LiteralPath $candidate).Path; Source = 'search' }
    }
  }

  $lines = @('AppxManifest.xml was not found. Tried:')
  foreach ($candidate in $tried) { $lines += ('  - ' + $candidate) }
  $lines += 'FIX       run tools\msix\build-sparse.ps1 -ShellExtDll <path> -NoSign first, then'
  $lines += '          pass -StageDir <out dir>\stage (or -ManifestPath <out dir>\stage\AppxManifest.xml).'
  throw ($lines -join [Environment]::NewLine)
}

# Reads AND validates. Errors are returned, never thrown, so the caller decides
# whether a plan run may continue (it may: -WhatIf must be able to explain a
# broken layout) while a real run refuses.
function Read-MsixManifest {
  param([Parameter(Mandatory = $true)] [string] $Path)

  $errors = New-Object System.Collections.Generic.List[string]
  $warnings = New-Object System.Collections.Generic.List[string]

  $xml = $null
  try {
    $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    $xml = New-Object System.Xml.XmlDocument
    $xml.LoadXml($raw)
  } catch {
    $message = $_.Exception.Message
    $errors.Add('manifest-xml-invalid: ' + $message)
    return [pscustomobject]@{
      Path                 = $Path
      ManifestDir          = (Split-Path -Parent $Path)
      Identity             = $null
      Executable           = ''
      AllowExternalContent = $false
      ComClassId           = ''
      ComClassPath         = ''
      VerbId               = ''
      VerbClsid            = ''
      MinVersion           = ''
      Errors               = $errors
      Warnings             = $warnings
    }
  }

  $ns = New-MsixNamespaceManager -Xml $xml

  $identityNode = $xml.SelectSingleNode('/a:Package/a:Identity', $ns)
  $identity = $null
  if ($null -eq $identityNode) {
    $errors.Add('manifest-identity-missing: <Identity> is required by the AppX schema (Windows answers 0x80080204).')
  } else {
    $identity = [pscustomobject]@{
      Name         = $identityNode.GetAttribute('Name')
      Publisher    = $identityNode.GetAttribute('Publisher')
      Version      = $identityNode.GetAttribute('Version')
      Architecture = $identityNode.GetAttribute('ProcessorArchitecture')
    }
    if (-not $identity.Name) {
      $errors.Add('identity-name-empty: Identity/@Name is what the package is registered under (measured: Windows rejects a manifest without it with 0x80080204).')
    }
    if (-not $identity.Publisher) {
      $errors.Add('identity-publisher-empty: Identity/@Publisher is mandatory; a signed package also needs a signer subject equal to it (0x800B010F otherwise).')
    }
    if (-not $identity.Version) {
      $errors.Add('identity-version-empty: Identity/@Version must be a four-part version such as 0.3.0.0.')
    } elseif ($identity.Version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
      $errors.Add('identity-version-invalid: "' + $identity.Version + '" is not a four-part version (Windows rejects it with 0x80080204).')
    }
  }

  $applications = @($xml.SelectNodes('/a:Package/a:Applications/a:Application', $ns))
  $executable = ''
  if ($applications.Count -ne 1) {
    $errors.Add('application-count: expected exactly 1 <Application>, found ' + $applications.Count + '.')
  } else {
    $executable = $applications[0].GetAttribute('Executable')
    if (-not $executable) {
      $errors.Add('application-executable-empty: Application/@Executable is required; with uap10:AllowExternalContent it resolves against -ExternalLocation.')
    }
  }

  $externalNode = $xml.SelectSingleNode('/a:Package/a:Properties/uap10:AllowExternalContent', $ns)
  $external = ($null -ne $externalNode) -and ($externalNode.InnerText.Trim() -ieq 'true')
  if (-not $external) {
    $errors.Add('allow-external-content-missing: Properties/uap10:AllowExternalContent must be true for this sparse layout (measured: without it Add-AppxPackage answers 0x80073D2E, must allow external location deployment).')
  }

  $comClass = $xml.SelectSingleNode('/a:Package/a:Applications/a:Application/a:Extensions/com:Extension/com:ComServer/com:SurrogateServer/com:Class', $ns)
  $verb = $xml.SelectSingleNode('/a:Package/a:Applications/a:Application/a:Extensions/desktop4:Extension/desktop4:FileExplorerContextMenus/desktop5:ItemType/desktop5:Verb', $ns)
  $comClassId = ''
  $comClassPath = ''
  $verbId = ''
  $verbClsid = ''
  if ($null -eq $comClass) {
    $errors.Add('com-class-missing: no com:Class in the manifest; Explorer would have nothing to activate.')
  } else {
    $comClassId = $comClass.GetAttribute('Id')
    $comClassPath = $comClass.GetAttribute('Path')
    if (-not $comClassId) { $errors.Add('com-class-id-empty: com:Class/@Id must be the CLSID PrismShellExt.dll answers to.') }
    if (-not $comClassPath) { $errors.Add('com-class-path-empty: com:Class/@Path is the DLL Explorer loads (build-sparse.ps1 stages PrismShellExt.dll).') }
  }
  if ($null -eq $verb) {
    $errors.Add('verb-missing: no desktop5:Verb; the Win11 menu would show nothing.')
  } else {
    $verbId = $verb.GetAttribute('Id')
    $verbClsid = $verb.GetAttribute('Clsid')
    if ($verbClsid -and $comClassId) {
      $left = $comClassId.Replace('{', '').Replace('}', '').ToUpperInvariant()
      $right = $verbClsid.Replace('{', '').Replace('}', '').ToUpperInvariant()
      if ($left -ne $right) {
        $errors.Add('clsid-mismatch: com:Class/@Id (' + $comClassId + ') != desktop5:Verb/@Clsid (' + $verbClsid + '); the verb silently never appears at runtime.')
      }
    }
  }

  $family = $xml.SelectSingleNode('/a:Package/a:Dependencies/a:TargetDeviceFamily', $ns)
  $minVersion = ''
  if ($null -eq $family) {
    $errors.Add('target-device-family-missing: Dependencies/TargetDeviceFamily is required (0x80073CFD otherwise).')
  } else {
    $minVersion = $family.GetAttribute('MinVersion')
  }

  $manifestDir = Split-Path -Parent $Path
  if ($comClassPath) {
    $dll = Join-Path $manifestDir $comClassPath
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
      # Measured: registration SUCCEEDS without this file (count went 0 -> 1) and
      # the menu is dead afterwards, because Explorer activates the COM class from
      # the package root. Warning, not failure: failing here would block a
      # register-then-build loop for no reason.
      $warnings.Add('shell-dll-missing: ' + $dll + ' does not exist; registration still succeeds but the context menu cannot load. Re-run build-sparse.ps1 to stage it.')
    }
  }

  return [pscustomobject]@{
    Path                 = $Path
    ManifestDir          = $manifestDir
    Identity             = $identity
    Executable           = $executable
    AllowExternalContent = $external
    ComClassId           = $comClassId
    ComClassPath         = $comClassPath
    VerbId               = $verbId
    VerbClsid            = $verbClsid
    MinVersion           = $minVersion
    Errors               = $errors
    Warnings             = $warnings
  }
}

# The one-shot elevation helper that turns Developer Mode on and then calls
# register-loose.ps1. Returned as a PATH so every dev-mode-off FIX can print
# something the reader can paste, instead of naming a setting and hoping.
function Get-MsixDevModeScript {
  $candidate = Join-Path $PSScriptRoot 'enable-dev-mode-and-register.cmd'
  if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
  return ''
}

# Developer Mode / sideload policy. Read-only, no elevation, no exception when the
# key is absent (a stock Windows install does not create it until you flip it).
#
# TEST SEAM: on this box Developer Mode is ON (measured 2026-11-02, value=1), and
# CI must never write HKLM. PRISM_MSIX_DEV_MODE=on|off changes what the scripts
# BELIEVE about the machine -- and therefore which branch runs -- without touching
# the registry. It does not change what any script writes; only the branch taken.
function Get-MsixDevMode {
  $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
  $value = $null
  try {
    $value = (Get-ItemProperty -LiteralPath $key -Name 'AllowDevelopmentWithoutDevLicense' -ErrorAction Stop).AllowDevelopmentWithoutDevLicense
  } catch {
    $value = $null
  }
  $on = ($null -ne $value) -and ([int]$value -eq 1)

  $override = [string]$env:PRISM_MSIX_DEV_MODE
  $forced = ($override -eq 'on') -or ($override -eq 'off')
  if ($forced) { $on = ($override -eq 'on') }

  return [pscustomobject]@{
    On           = $on
    Value        = $value
    Key          = $key
    Override     = $(if ($forced) { $override } else { '' })
    Source       = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock\AllowDevelopmentWithoutDevLicense'
    EnableScript = (Get-MsixDevModeScript)
    SettingsUri  = 'ms-settings:developers'
  }
}

# What to DO when the machine refuses an unsigned loose layout (0x80073CFF).
# Returned as lines so register-loose.ps1 and the HRESULT table cannot drift:
# the selftest asserts the printed FIX carries the absolute path of
# enable-dev-mode-and-register.cmd, and the mutation DropDevModeFix proves that
# assertion is the thing holding the path in place.
function Get-MsixDevModeFix {
  param($DevMode)

  $script = ''
  if ($DevMode -and $DevMode.EnableScript) { $script = $DevMode.EnableScript }

  $lines = New-Object System.Collections.Generic.List[string]
  if ($script) {
    $lines.Add('run ' + $script + '  (one UAC prompt: sets AllowDevelopmentWithoutDevLicense + AllowAllTrustedApps, then registers this package)')
  } else {
    $lines.Add('enable Developer Mode: start ms-settings:developers')
  }
  $lines.Add('or, elevated: Set-ItemProperty HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock -Name AllowDevelopmentWithoutDevLicense -Value 1 (AllowAllTrustedApps=1 too)')
  return $lines
}

function Test-MsixElevated {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Turns a failed Add-AppxPackage / Remove-AppxPackage into (hresult, reason, fix).
# The code is read out of the message text first -- see the header note.
function ConvertFrom-DeployError {
  param([Parameter(Mandatory = $true)] $ErrorRecord)

  $exception = $ErrorRecord.Exception
  $message = ''
  if ($exception) { $message = $exception.Message }
  $match = [regex]::Match([string]$message, '0x([0-9A-Fa-f]{8})')
  $code = 0
  if ($match.Success) {
    $code = [int64][Convert]::ToUInt32($match.Groups[1].Value, 16)
  } elseif ($exception) {
    $code = [int64][uint32]$exception.HResult
  }

  $reason = 'deployment failed; the HRESULT below is the only stable identifier (the message text is localised).'
  $fix = 'run Get-AppxPackageLog -ActivityID <id from the message> for the component-level failure, and re-run with -Verbose.'

  # Keyed by the 8-digit UPPERCASE hex TEXT, never by the numeric literal:
  # in PowerShell 0x80073CFF is an Int32 NEGATIVE value (-2147009281), so
  # "switch ([uint32]$code) { 0x80073CFF { ... } }" never matched. 0x80073CFF,
  # 0x800B0109 and 0x800B010F (all above 0x7FFFFFFF) were dead rows -- the
  # selftest now pins one check per row plus the default, so a dead row is a red
  # check and not a plausible-looking comment.
  $hex = ('{0:X8}' -f [uint32]$code)
  switch ($hex) {
    '80073CF9' {
      $reason = 'the package layout requires an external location, but none was supplied.'
      $fix = 'pass -ExternalLocation <folder with PrismArchive.exe>; a sparse package (uap10:AllowExternalContent) cannot be registered with -Register alone.'
    }
    '80073D2E' {
      $reason = 'the deployment API was told to use an external location, but the manifest does not allow it.'
      $fix = 'restore <uap10:AllowExternalContent>true</uap10:AllowExternalContent> in Properties (build-sparse.ps1 stages Assets\Package\AppxManifest.xml verbatim).'
    }
    '80080204' {
      $reason = 'the manifest is invalid (schema violation, missing Identity/@Publisher, bad version, ...).'
      $fix = 'run python tools\check_manifest.py <manifest>, then re-read the manifest errors this script printed above.'
    }
    '80073CFF' {
      $reason = 'this machine refuses unsigned/sideloaded layouts (0x80073CFF): Developer Mode or the sideloading policy is off.'
      $fix = ((Get-MsixDevModeFix -DevMode (Get-MsixDevMode)) -join '; ')
    }
    '80073CF0' {
      $reason = 'the package could not be opened (a file the manifest references is missing or unreadable).'
      $fix = 're-run tools\msix\build-sparse.ps1 and register its stage folder again.'
    }
    '80073CF3' {
      $reason = 'dependency or conflict validation failed.'
      $fix = 'another package with this identity/version is installed, or a dependency is missing: Remove-AppxPackage the existing one, then retry.'
    }
    '80073CFD' {
      $reason = 'a prerequisite is missing (unsupported OS build or missing dependency package).'
      $fix = 'compare Dependencies/TargetDeviceFamily MinVersion with [Environment]::OSVersion.Version (Win11 context menus need 22000+).'
    }
    '80073D06' {
      $reason = 'a higher version of this package is already installed.'
      $fix = 'bump Identity/@Version, or uninstall the newer package first.'
    }
    '80073D02' {
      $reason = 'the package is in use (a loaded DLL).'
      $fix = 'close Explorer windows / the app, then retry.'
    }
    '800B0109' {
      $reason = 'the signing certificate chains to an untrusted root (CERT_E_UNTRUSTEDROOT).'
      $fix = 'trust the .cer: Import-Certificate -FilePath <cert>.cer -CertStoreLocation Cert:\CurrentUser\Root (see selfsign-and-install.ps1), or use the loose path that needs no signature.'
    }
    '800B010F' {
      $reason = 'the signer subject differs from Identity/@Publisher (CERT_E_WRONG_PRINCIPAL).'
      $fix = 'sign with a certificate whose subject is exactly the Publisher string (new-dev-cert.ps1 defaults to CN=PrismArchive Contributors).'
    }
    '80073CFB' {
      $reason = 'the same version is already installed with different content.'
      $fix = 'bump Identity/@Version, or remove the installed package first (unregister-loose.ps1).'
    }
    default { }
  }

  return [pscustomobject]@{
    Code    = [uint32]$code
    Text    = ('0x{0:X8}' -f [uint32]$code)
    Message = ([string]$message -split '\r?\n')[0]
    Reason  = $reason
    Fix     = $fix
  }
}

# Same lookup build-sparse.ps1 uses, kept here so selfsign-and-install.ps1 can
# run its own preflight before it calls anything heavy.
function Find-MsixSdkTool {
  param([Parameter(Mandatory = $true)] [string] $Name)

  $command = Get-Command $Name -ErrorAction SilentlyContinue
  if ($command) { return $command.Source }

  $kits = @(
    'C:\Program Files (x86)\Windows Kits\10\bin',
    'C:\Program Files\Windows Kits\10\bin'
  ) | Where-Object { Test-Path -LiteralPath $_ }

  foreach ($root in $kits) {
    foreach ($dir in (Get-ChildItem -LiteralPath $root -Directory | Sort-Object Name -Descending)) {
      $candidate = Join-Path $dir.FullName ('x64\' + $Name)
      if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
  }
  return $null
}
