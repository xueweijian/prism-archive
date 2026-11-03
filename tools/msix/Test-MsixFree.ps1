# PrismArchive -- C61 self-test for the certificate-free MSIX paths.
#
# Runs the real scripts as CHILD processes and asserts on their exit codes and
# output, plus on the manifest, the compiled constants and the doc. It never
# registers, trusts or installs anything: every invocation is -WhatIf, and the
# certificate store is measured before/after to prove nothing was created.
#
#   pwsh -NoProfile -File tools\msix\Test-MsixFree.ps1
#   pwsh -NoProfile -File tools\msix\Test-MsixFree.ps1 -KeepFixtures
#
# Negative controls (CONTRACTS D3): -Mutate <name> copies the scripts to a temp
# directory, injects ONE defect, verifies the injection landed, re-runs the same
# assertions and prints
#   MSIX-FREE(MUTATION <name>): CAUGHT (<check that went red>)
# only when the EXPECTED check went red. Exit 0 means "the guard is sharp";
# exit 1 means "the injection did nothing" or "the assertion went blind" -- the
# two failure modes that make mutation testing self-deceiving.
#
# Conventions followed here:
#   D1  success marker "MSIX-FREE: PASS" is part of the verdict (ctest regex);
#       a failure exits 1 and prints every failed check.
#   D26 no WILL_FAIL anywhere: the mutation tests print their own CAUGHT marker.
#   D9  every fixture lives in its own temp directory, removed unless -KeepFixtures.
#   D29 no unbounded wait: every child process is bounded by the ctest TIMEOUT.
[CmdletBinding()]
param(
  [switch] $SelfTest,
  [switch] $SyntaxOnly,
  [string] $Mutate = '',
  [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [switch] $KeepFixtures
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'msix-common.ps1')

$script:RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$script:ToolsDir = $PSScriptRoot
$script:Pwsh = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
$script:Checks = New-Object System.Collections.Generic.List[object]
$script:TempRoots = New-Object System.Collections.Generic.List[string]
$script:DefaultPackageName = 'PrismArchive.ShellExt'
$script:MutationName = ''
$script:MutationExpected = ''

# The verdict for a mutation arm is printed AT THE FIRST red check that is the
# expected one (CONTRACTS D27): a crash, a hang or a later teardown can no longer
# swallow the marker and leave ctest with "required regular expression not
# found". "Red for an unrelated reason" is still NOT CAUGHT (D3), because only
# the expected check name can produce the marker.
function Add-Check {
  param([string] $Name, [bool] $Ok, [string] $Detail = '')
  $script:Checks.Add([pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail })
  $state = 'FAIL'
  if ($Ok) { $state = 'PASS' }
  if ($Detail) { Write-Host ('CHECK ' + $Name + ': ' + $state + ' ' + $Detail) }
  else { Write-Host ('CHECK ' + $Name + ': ' + $state) }

  if ($script:MutationName -and (-not $Ok) -and ($Name -eq $script:MutationExpected)) {
    $red = @($script:Checks | Where-Object { -not $_.Ok }).Count
    Write-Host ('MSIX-FREE(MUTATION ' + $script:MutationName + '): CAUGHT (' + $Name + ' went red, ' + $red + ' check(s) red at the first verdict)')
    exit 0
  }
}

# ASCII-only test files cannot paste Chinese headers; build them from UTF-16 code
# points instead of weakening the encoding check.
function ConvertFrom-CodePoints {
  param([int[]] $CodePoint)
  $builder = New-Object System.Text.StringBuilder
  foreach ($cp in $CodePoint) { [void]$builder.Append([char]$cp) }
  return $builder.ToString()
}

# Every .ps1 the card ships, discovered rather than listed: a new script that is
# not syntax-checked would be a silent hole, so the test enumerates the folder.
function Get-MsixScriptPaths {
  param([string] $ToolsDir)
  return @(Get-ChildItem -LiteralPath $ToolsDir -Filter '*.ps1' -File | Sort-Object Name)
}

function Assert-Match {
  param([string] $Name, [string] $Text, [string] $Pattern)
  $ok = [regex]::IsMatch($Text, $Pattern)
  $detail = 'missing /' + $Pattern + '/'
  if ($ok) { $detail = '' }
  Add-Check -Name $Name -Ok $ok -Detail $detail
}

function Assert-Absent {
  param([string] $Name, [string] $Text, [string] $Pattern)
  $ok = -not [regex]::IsMatch($Text, $Pattern)
  $detail = 'unexpected /' + $Pattern + '/'
  if ($ok) { $detail = '' }
  Add-Check -Name $Name -Ok $ok -Detail $detail
}

function New-TempRoot {
  param([string] $Tag)
  $root = Join-Path ([System.IO.Path]::GetTempPath()) ('prism-c61-' + $Tag + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Path $root -Force | Out-Null
  $script:TempRoots.Add($root)
  return $root
}

function Get-ParsedScript {
  param([string] $Path)
  $tokens = $null
  $errors = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
  return [pscustomobject]@{ Ast = $ast; Errors = $errors }
}

function Get-ParameterNames {
  param($Parsed)
  $names = @()
  if ($Parsed.Ast.ParamBlock) {
    foreach ($p in $Parsed.Ast.ParamBlock.Parameters) { $names += $p.Name.VariablePath.UserPath }
  }
  return $names
}

function Get-ParameterDefault {
  param($Parsed, [string] $Name)
  if (-not $Parsed.Ast.ParamBlock) { return $null }
  foreach ($p in $Parsed.Ast.ParamBlock.Parameters) {
    if ($p.Name.VariablePath.UserPath -eq $Name -and $p.DefaultValue) {
      return $p.DefaultValue.Extent.Text.Trim("'", '"')
    }
  }
  return $null
}

function Test-SupportsShouldProcess {
  param($Parsed)
  if (-not $Parsed.Ast.ParamBlock) { return $false }
  foreach ($attr in $Parsed.Ast.ParamBlock.Attributes) {
    if ($attr.TypeName.Name -ne 'CmdletBinding') { continue }
    foreach ($named in $attr.NamedArguments) {
      if ($named.ArgumentName -eq 'SupportsShouldProcess' -and $named.Argument.Extent.Text -match 'true') { return $true }
    }
  }
  return $false
}

# Windows PowerShell 5.1 (the shell enable-dev-mode-and-register.cmd uses) reads a
# BOM-less script as ANSI, so a single non-ASCII byte changes what it runs on a
# machine with a Chinese code page. Measured, not assumed: every file in
# tools\msix is pure ASCII today, and this check keeps it that way.
function Test-TextAscii {
  param([string] $Path)
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  $nonAscii = 0
  for ($i = 0; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -gt 127) { $nonAscii++ } }
  $bom = ($bytes.Length -ge 3) -and ($bytes[0] -eq 0xEF) -and ($bytes[1] -eq 0xBB) -and ($bytes[2] -eq 0xBF)
  return [pscustomobject]@{ Bytes = $bytes.Length; NonAscii = $nonAscii; Bom = $bom }
}

# Both parsers, because they are not the same gate: ParseFile tokenises the file,
# [scriptblock]::Create also compiles the parameter block and the body. A file can
# pass the first and still throw on the second.
function Test-ScriptSyntax {
  param([string] $Path)
  $tokens = $null
  $errors = $null
  $null = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
  $messages = New-Object System.Collections.Generic.List[string]
  foreach ($err in $errors) { $messages.Add('ParseFile: ' + $err.Message) }

  try {
    $null = [scriptblock]::Create((Get-Content -LiteralPath $Path -Raw))
  } catch {
    $messages.Add('scriptblock: ' + $_.Exception.Message)
  }
  return [pscustomobject]@{ Ok = ($messages.Count -eq 0); Errors = $messages }
}

# Bounded (5 attempts, 100 ms apart) so a locked fixture is reported as a red
# check instead of hanging past the ctest budget (CONTRACTS D29).
function Remove-Fixtures {
  $left = New-Object System.Collections.Generic.List[string]
  foreach ($root in $script:TempRoots) {
    for ($i = 0; $i -lt 5 -and (Test-Path -LiteralPath $root); $i++) {
      Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
      if (Test-Path -LiteralPath $root) { Start-Sleep -Milliseconds 100 }
    }
    if (Test-Path -LiteralPath $root) { $left.Add($root) }
  }
  return $left
}

# -SyntaxOnly is its own ctest entry (win.msix_free_syntax): it prints one line
# per script and a marker ctest can match, so a syntax regression is a named red
# test instead of a paragraph inside a larger one.
function Invoke-SyntaxChecks {
  param([string] $ToolsDir)
  $files = Get-MsixScriptPaths -ToolsDir $ToolsDir
  $bad = 0
  $bytes = 0
  foreach ($file in $files) {
    $info = Test-TextAscii -Path $file.FullName
    $bytes += $info.Bytes
    $syntax = Test-ScriptSyntax -Path $file.FullName
    if (-not $syntax.Ok) {
      $bad++
      Write-Host ('SYNTAX FAIL ' + $file.Name + ' :: ' + (($syntax.Errors) -join ' | '))
    } else {
      Write-Host ('SYNTAX ok ' + $file.Name + ' bytes=' + $info.Bytes + ' non-ascii=' + $info.NonAscii)
    }
  }
  if ($bad -gt 0) {
    Write-Host ('MSIX-SYNTAX: FAIL (' + $bad + ' of ' + $files.Count + ' scripts)')
    exit 1
  }
  Write-Host ('MSIX-SYNTAX: PASS (' + $files.Count + ' scripts, ' + $bytes + ' bytes, both parsers)')
  exit 0
}

# Runs one of the card's scripts in a child process and returns its output.
# stderr is merged on purpose: a parser error must be visible in the assertion
# message rather than swallowed.
function Invoke-CardScript {
  param([string] $ScriptName, [string[]] $Arguments = @())
  $path = Join-Path $script:ToolsDir $ScriptName
  $lines = @(& $script:Pwsh -NoProfile -ExecutionPolicy Bypass -File $path @Arguments 2>&1 | ForEach-Object { [string]$_ })
  return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($lines -join [Environment]::NewLine) }
}

# Same set as Get-ChildItem Cert:\CurrentUser\My, ~20x cheaper on this box
# (measured 0.4 s vs 8.5 s: the PowerShell certificate provider enumerates every
# store before filtering). The count assertion is unchanged -- only the way the
# store is read.
function Get-CertCount {
  param([string] $Subject, [string] $Store = 'Cert:\CurrentUser\My')
  $split = $Store.Split('\')
  $name = $split[-1]
  $location = $split[-2]
  $x509 = [System.Security.Cryptography.X509Certificates.X509Store]::new($name, $location)
  try {
    $x509.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
    return @($x509.Certificates | Where-Object { $_.Subject -eq $Subject }).Count
  } finally {
    $x509.Close()
  }
}

# A staged loose layout: manifest + Assets + a file standing in for the shell
# DLL, with a UNIQUE identity so a bug that does register something can be seen
# (and removed) instead of silently touching the real package.
function New-StagedLayout {
  param([string] $Tag)
  $root = New-TempRoot -Tag $Tag
  $manifest = Join-Path $root 'AppxManifest.xml'
  Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'Assets\Package\AppxManifest.xml') -Destination $manifest
  Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'Assets\Package\Assets') -Destination (Join-Path $root 'Assets') -Recurse
  Set-Content -LiteralPath (Join-Path $root 'PrismShellExt.dll') -Value 'C61 probe: presence is all registration checks' -NoNewline

  $xml = New-Object System.Xml.XmlDocument
  $xml.Load($manifest)
  $name = 'PrismArchive.ShellExt.C61' + $Tag + [guid]::NewGuid().ToString('N').Substring(0, 6)
  $xml.Package.Identity.Name = $name
  $xml.Save($manifest)

  return [pscustomobject]@{
    Root        = $root
    Manifest    = $manifest
    PackageName = $name
    Output      = (Join-Path $root 'out')
    Build       = (Join-Path $root 'build')
  }
}

# Rewrites a manifest copy through a mutation callback and returns the path.
function New-ManifestVariant {
  param([string] $SourceManifest, [string] $Tag, [scriptblock] $MutateXml)
  $root = New-TempRoot -Tag $Tag
  $manifest = Join-Path $root 'AppxManifest.xml'
  Copy-Item -LiteralPath $SourceManifest -Destination $manifest
  Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $SourceManifest) 'Assets') -Destination (Join-Path $root 'Assets') -Recurse
  Set-Content -LiteralPath (Join-Path $root 'PrismShellExt.dll') -Value 'probe' -NoNewline

  $xml = New-Object System.Xml.XmlDocument
  $xml.Load($manifest)
  & $MutateXml $xml
  $xml.Save($manifest)
  return [pscustomobject]@{ Root = $root; Manifest = $manifest }
}

function Remove-NodeByLocalName {
  param($Xml, [string] $LocalName)
  foreach ($node in @($Xml.SelectNodes('//*'))) {
    if ($node.LocalName -eq $LocalName) { [void]$node.ParentNode.RemoveChild($node); return $true }
  }
  return $false
}

# ---------------------------------------------------------------------------
# The whole assertion set. Parameterised by -ToolsDir so -Mutate can run the
# SAME assertions against mutated copies.
# ---------------------------------------------------------------------------
function Invoke-MsixFreeChecks {
  param([string] $ToolsDir)

  $script:Checks.Clear()
  $script:ToolsDir = $ToolsDir

  # Re-dot-source the folder under test. The test loads msix-common.ps1 from its
  # own directory at startup; without this line every IN-PROCESS assertion (the
  # HRESULT table, the dev-mode reader) would keep calling the unmutated copy and
  # a mutation in msix-common could never be caught (measured: HexCaseCollapse
  # stayed green for exactly this reason).
  . (Join-Path $ToolsDir 'msix-common.ps1')

  # --- 1. every script parses twice, is ASCII, declares its parameters, is -WhatIf capable ---
  $scriptFiles = Get-MsixScriptPaths -ToolsDir $ToolsDir
  $scriptCount = $scriptFiles.Count
  Add-Check -Name 'script-count' -Ok ($scriptCount -ge 9) -Detail ('ps1 files in tools\msix=' + $scriptCount)

  $totalBytes = 0
  foreach ($file in $scriptFiles) {
    $encoding = Test-TextAscii -Path $file.FullName
    $totalBytes += $encoding.Bytes
    Add-Check -Name ('script-encoding:' + $file.Name) -Ok ($encoding.NonAscii -eq 0 -and -not $encoding.Bom) -Detail ('bytes=' + $encoding.Bytes + ' non-ascii=' + $encoding.NonAscii + ' bom=' + $encoding.Bom)

    $syntax = Test-ScriptSyntax -Path $file.FullName
    Add-Check -Name ('script-parse:' + $file.Name) -Ok ($syntax.Ok) -Detail (($syntax.Errors) -join '; ')
    Add-Check -Name ('script-syntax:' + $file.Name) -Ok ($syntax.Ok) -Detail (($syntax.Errors) -join '; ')
  }
  Add-Check -Name 'script-bytes' -Ok ($totalBytes -gt 50000) -Detail ('total=' + $totalBytes)

  $parsed = @{}
  $scripts = @('msix-common.ps1', 'register-loose.ps1', 'unregister-loose.ps1', 'selfsign-and-install.ps1')
  foreach ($name in $scripts) {
    $parsed[$name] = Get-ParsedScript -Path (Join-Path $ToolsDir $name)
  }

  $entryScripts = @('register-loose.ps1', 'unregister-loose.ps1', 'selfsign-and-install.ps1')
  foreach ($name in $entryScripts) {
    Add-Check -Name ('script-whatif:' + $name) -Ok (Test-SupportsShouldProcess -Parsed $parsed[$name])
  }

  $expectedParameters = @{
    'register-loose.ps1'         = @('ManifestPath', 'StageDir', 'BuildDir', 'RepoRoot', 'ExternalLocation', 'PackageName')
    'unregister-loose.ps1'       = @('PackageName')
    'selfsign-and-install.ps1'   = @('ManifestPath', 'ExternalLocation', 'PackagePath', 'ShellExtDll', 'CertDir', 'Subject', 'PfxPassword', 'Machine')
  }
  foreach ($name in $expectedParameters.Keys) {
    $actual = Get-ParameterNames -Parsed $parsed[$name]
    $missing = @($expectedParameters[$name] | Where-Object { $_ -notin $actual })
    Add-Check -Name ('script-params:' + $name) -Ok ($missing.Count -eq 0) -Detail ('missing: ' + ($missing -join ','))
  }

  # --- 2. the ONE measured call shape: -Register needs -ExternalLocation ---
  $calls = @($parsed['register-loose.ps1'].Ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Add-AppxPackage'
      }, $true))
  $callParameters = @()
  foreach ($call in $calls) {
    foreach ($element in $call.CommandElements) {
      if ($element -is [System.Management.Automation.Language.CommandParameterAst]) { $callParameters += $element.ParameterName }
    }
  }
  $shapeOk = ($calls.Count -ge 1) -and ('Register' -in $callParameters) -and ('ExternalLocation' -in $callParameters)
  Add-Check -Name 'script-call:register-loose' -Ok $shapeOk -Detail ('Add-AppxPackage calls=' + $calls.Count + ' params=' + (($callParameters | Select-Object -Unique) -join ','))

  # --- 3. the classic menu path must stay certificate-free ----------------
  # Read from $ToolsDir, not the repo: the DropClassicCertFree mutation must be
  # able to prove that this lexical guard is what keeps the path certificate-free.
  $classicPath = Join-Path $ToolsDir 'register-classic.ps1'
  $classicText = Get-Content -LiteralPath $classicPath -Raw
  $signingTokens = @('Add-AppxPackage', 'makeappx', 'signtool', 'New-SelfSignedCertificate', 'Import-Certificate')
  $hits = 0
  foreach ($token in $signingTokens) {
    $hits += ([regex]::Matches($classicText, [regex]::Escape($token), 'IgnoreCase')).Count
  }
  Add-Check -Name 'classic-path:cert-free' -Ok ($hits -eq 0) -Detail ('signing/package tokens found=' + $hits)
  Add-Check -Name 'classic-path:hkcu' -Ok ($classicText -match 'HKCU:') -Detail ''

  # --- 4. the checked-in manifest, against the compiled constants ----------
  $manifestPath = Join-Path $script:RepoRoot 'Assets\Package\AppxManifest.xml'
  $manifest = Read-MsixManifest -Path $manifestPath
  Add-Check -Name 'manifest:valid' -Ok ($manifest.Errors.Count -eq 0) -Detail (($manifest.Errors) -join '; ')

  $header = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'Src\PrismWin\SparsePackageRegistrar.hpp') -Raw
  $nameMatches = [regex]::Matches($header, 'kPackageName\[\]\s*=\s*L"([^"]+)"')
  Add-Check -Name 'manifest:name-vs-compiled' -Ok ($nameMatches.Count -eq 1 -and $manifest.Identity.Name -eq $nameMatches[0].Groups[1].Value) -Detail ('manifest=' + $manifest.Identity.Name + ' matches=' + $nameMatches.Count)

  $publisherMatches = [regex]::Matches($header, 'kPackagePublisher\[\]\s*=\s*L"([^"]+)"')
  Add-Check -Name 'manifest:publisher-vs-compiled' -Ok ($publisherMatches.Count -eq 1 -and $manifest.Identity.Publisher -eq $publisherMatches[0].Groups[1].Value) -Detail ('manifest=' + $manifest.Identity.Publisher)

  $clsid = $manifest.ComClassId.Replace('{', '').Replace('}', '').ToUpperInvariant()
  Add-Check -Name 'manifest:clsid-vs-compiled' -Ok ($header.ToUpperInvariant().Contains($clsid)) -Detail ('clsid=' + $clsid)

  $minimum = [version]'0.0.0.0'
  $parsedMin = [version]::TryParse($manifest.MinVersion, [ref]$minimum)
  Add-Check -Name 'manifest:min-version' -Ok ($parsedMin -and $minimum -ge [version]'10.0.19041.0') -Detail ('MinVersion=' + $manifest.MinVersion)

  $unregisterDefault = Get-ParameterDefault -Parsed $parsed['unregister-loose.ps1'] -Name 'PackageName'
  Add-Check -Name 'manifest:name-vs-unregister-default' -Ok ($unregisterDefault -eq $manifest.Identity.Name) -Detail ('default=' + $unregisterDefault)

  # --- 5. register-loose plan on a real staged layout ---------------------
  $fixture = New-StagedLayout -Tag 'plan'
  $dev = Get-MsixDevMode
  $certSubject = 'CN=PrismArchive Contributors'
  $beforeCerts = Get-CertCount -Subject $certSubject
  $beforePackages = @(Get-AppxPackage -Name $fixture.PackageName -ErrorAction SilentlyContinue).Count

  $plan = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @(
    '-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root, '-WhatIf')
  Add-Check -Name 'register-plan:exit' -Ok ($plan.ExitCode -eq 0) -Detail ('exit=' + $plan.ExitCode)
  Assert-Match -Name 'register-plan:manifest' -Text $plan.Output -Pattern ('MANIFEST ' + [regex]::Escape($fixture.Manifest))
  Assert-Match -Name 'register-plan:identity' -Text $plan.Output -Pattern ('IDENTITY name=' + [regex]::Escape($fixture.PackageName))
  Assert-Match -Name 'register-plan:external' -Text $plan.Output -Pattern ('EXTERNAL-LOCATION ' + [regex]::Escape($fixture.Root))
  Assert-Match -Name 'register-plan:plan-line' -Text $plan.Output -Pattern 'PLAN Add-AppxPackage -Register '
  $devValue = '<absent>'
  if ($null -ne $dev.Value) { $devValue = [string]$dev.Value }
  $devPattern = 'DEV-MODE (on|off) HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock\\AllowDevelopmentWithoutDevLicense=' + [regex]::Escape($devValue)
  Assert-Match -Name 'register-plan:devmode' -Text $plan.Output -Pattern $devPattern
  $afterPackages = @(Get-AppxPackage -Name $fixture.PackageName -ErrorAction SilentlyContinue).Count
  Add-Check -Name 'register-plan:no-side-effect' -Ok ($beforePackages -eq 0 -and $afterPackages -eq 0) -Detail ('before=' + $beforePackages + ' after=' + $afterPackages)

  # --- 5b. the 0x80073CFF branch: Developer Mode off ---------------------
  # Measured by the user (2026-11-02): with Developer Mode off, Windows refuses a
  # loose layout with 0x80073CFF. Reproducing that state for real needs an HKLM
  # write and admin, so the scripts carry a TEST SEAM: PRISM_MSIX_DEV_MODE=off
  # makes them believe the switch is off. The branch, its exit code, its FIX text
  # and the fact that nothing gets deployed are asserted; the registry is never
  # touched (this box has Developer Mode ON, value=1).
  $devCmd = Join-Path $ToolsDir 'enable-dev-mode-and-register.cmd'
  Add-Check -Name 'dev-mode-script:exists' -Ok (Test-Path -LiteralPath $devCmd -PathType Leaf) -Detail $devCmd
  if (Test-Path -LiteralPath $devCmd -PathType Leaf) {
    $cmdText = Get-Content -LiteralPath $devCmd -Raw
    $cmdEncoding = Test-TextAscii -Path $devCmd
    $regAdds = ([regex]::Matches($cmdText, '(?m)^reg add ')).Count
    Add-Check -Name 'dev-mode-script:registry-writes' -Ok ($regAdds -eq 2) -Detail ('reg add lines=' + $regAdds)
    Add-Check -Name 'dev-mode-script:values' -Ok ($cmdText.Contains('AllowDevelopmentWithoutDevLicense') -and $cmdText.Contains('AllowAllTrustedApps')) -Detail ''
    Add-Check -Name 'dev-mode-script:elevates' -Ok ($cmdText -match 'RunAs') -Detail ''
    Add-Check -Name 'dev-mode-script:calls-register' -Ok ($cmdText -match 'register-loose\.ps1') -Detail ''
    Add-Check -Name 'dev-mode-script:ascii' -Ok ($cmdEncoding.NonAscii -eq 0 -and -not $cmdEncoding.Bom) -Detail ('bytes=' + $cmdEncoding.Bytes + ' non-ascii=' + $cmdEncoding.NonAscii)
  }

  $env:PRISM_MSIX_DEV_MODE = 'off'
  try {
    $offPlan = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root, '-WhatIf')
    Add-Check -Name 'register-devmode-off:plan-exit' -Ok ($offPlan.ExitCode -eq 0) -Detail ('exit=' + $offPlan.ExitCode)
    Assert-Match -Name 'register-devmode-off:blocker' -Text $offPlan.Output -Pattern 'BLOCKER dev-mode-off'
    Assert-Match -Name 'register-devmode-off:override' -Text $offPlan.Output -Pattern 'DEV-MODE-OVERRIDE off'
    Assert-Match -Name 'register-devmode-off:which-script' -Text $offPlan.Output -Pattern ([regex]::Escape($devCmd))

    # The real (non -WhatIf) run must refuse BEFORE Add-AppxPackage. A deploy here
    # would pollute the user's package database, so "refused" is the assertion.
    $offRun = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root)
    Add-Check -Name 'register-devmode-off:real-exit' -Ok ($offRun.ExitCode -eq 1) -Detail ('exit=' + $offRun.ExitCode)
    Assert-Match -Name 'register-devmode-off:real-reason' -Text $offRun.Output -Pattern 'FAIL register reason=dev-mode-off'
    Assert-Match -Name 'register-devmode-off:real-fix' -Text $offRun.Output -Pattern ([regex]::Escape($devCmd))
    Add-Check -Name 'register-devmode-off:no-deploy' -Ok ((@(Get-AppxPackage -Name $fixture.PackageName -ErrorAction SilentlyContinue)).Count -eq 0) -Detail ''
  } finally {
    Remove-Item Env:PRISM_MSIX_DEV_MODE -ErrorAction SilentlyContinue
  }

  $env:PRISM_MSIX_DEV_MODE = 'on'
  try {
    $onPlan = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root, '-WhatIf')
    Add-Check -Name 'register-devmode-on:plan-exit' -Ok ($onPlan.ExitCode -eq 0) -Detail ('exit=' + $onPlan.ExitCode)
    Assert-Absent -Name 'register-devmode-on:no-blocker' -Text $onPlan.Output -Pattern 'BLOCKER dev-mode-off'
    Assert-Match -Name 'register-devmode-on:override' -Text $onPlan.Output -Pattern 'DEV-MODE-OVERRIDE on'
  } finally {
    Remove-Item Env:PRISM_MSIX_DEV_MODE -ErrorAction SilentlyContinue
  }

  # --- 5c. the HRESULT table is live, not comments ------------------------
  # Each row is fed the exact message shape Add-AppxPackage produces. This is the
  # assertion that caught the signedness bug: "switch ([uint32]$code) { 0x80073CFF
  # { ... } }" compares a uint32 against an Int32 NEGATIVE literal, so 0x80073CFF,
  # 0x800B0109 and 0x800B010F were dead rows (the Fix text never appeared).
  $mappingCases = @(
    @{ Hex = '0x80073CF9'; Expect = 'external location' },
    @{ Hex = '0x80073D2E'; Expect = 'does not allow it' },
    @{ Hex = '0x80080204'; Expect = 'manifest is invalid' },
    @{ Hex = '0x80073CFF'; Expect = 'Developer Mode' },
    @{ Hex = '0x80073CF0'; Expect = 'could not be opened' },
    @{ Hex = '0x80073CF3'; Expect = 'dependency or conflict' },
    @{ Hex = '0x80073CFD'; Expect = 'prerequisite is missing' },
    @{ Hex = '0x80073D06'; Expect = 'higher version' },
    @{ Hex = '0x80073D02'; Expect = 'in use' },
    @{ Hex = '0x800B0109'; Expect = 'untrusted root' },
    @{ Hex = '0x800B010F'; Expect = 'signer subject' },
    @{ Hex = '0x80073CFB'; Expect = 'different content' }
  )
  $mapped = 0
  foreach ($case in $mappingCases) {
    $record = [System.Management.Automation.ErrorRecord]::new(
      [System.Exception]::new('Deployment failed with HRESULT: ' + $case.Hex + '.'),
      'msix-hresult', [System.Management.Automation.ErrorCategory]::OperationStopped, $null)
    $failure = ConvertFrom-DeployError -ErrorRecord $record
    $ok = ($failure.Text -eq $case.Hex) -and ($failure.Reason -match [regex]::Escape($case.Expect))
    if ($ok) { $mapped++ }
    Add-Check -Name ('hresult:' + $case.Hex) -Ok $ok -Detail ('text=' + $failure.Text + ' reason=' + $failure.Reason)
  }
  Add-Check -Name 'hresult:table-rows' -Ok ($mapped -eq $mappingCases.Count) -Detail ($mapped.ToString() + '/' + $mappingCases.Count)

  $cffRecord = [System.Management.Automation.ErrorRecord]::new(
    [System.Exception]::new('Deployment failed with HRESULT: 0x80073CFF.'),
    'msix-hresult', [System.Management.Automation.ErrorCategory]::OperationStopped, $null)
  $cffFailure = ConvertFrom-DeployError -ErrorRecord $cffRecord
  Assert-Match -Name 'hresult:cff-fix' -Text $cffFailure.Fix -Pattern ([regex]::Escape($devCmd))

  $unknownRecord = [System.Management.Automation.ErrorRecord]::new(
    [System.Exception]::new('Deployment failed with HRESULT: 0x80070005.'),
    'msix-hresult', [System.Management.Automation.ErrorCategory]::OperationStopped, $null)
  $unknown = ConvertFrom-DeployError -ErrorRecord $unknownRecord
  Add-Check -Name 'hresult:unknown-default' -Ok ($unknown.Reason -match 'only stable identifier' -and $unknown.Fix.Length -gt 20 -and $unknown.Text -eq '0x80070005') -Detail ('reason=' + $unknown.Reason + ' fixlen=' + $unknown.Fix.Length)

  # --- 6. negative fixtures: each MUST be rejected with its own reason ----
  $noExternal = New-ManifestVariant -SourceManifest $fixture.Manifest -Tag 'noexternal' -MutateXml {
    param($xml)
    $removed = Remove-NodeByLocalName -Xml $xml -LocalName 'AllowExternalContent'
    if (-not $removed) { throw 'injection missed: AllowExternalContent not found' }
  }
  $result = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $noExternal.Manifest, '-ExternalLocation', $noExternal.Root, '-WhatIf')
  Add-Check -Name 'register-neg:external-content' -Ok ($result.ExitCode -ne 0) -Detail ('exit=' + $result.ExitCode)
  Assert-Match -Name 'register-neg:external-content-reason' -Text $result.Output -Pattern 'allow-external-content-missing'

  $noPublisher = New-ManifestVariant -SourceManifest $fixture.Manifest -Tag 'nopublisher' -MutateXml {
    param($xml)
    $identity = $xml.SelectSingleNode("//*[local-name()='Identity']")
    [void]$identity.Attributes.Remove($identity.Attributes['Publisher'])
  }
  $result = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $noPublisher.Manifest, '-ExternalLocation', $noPublisher.Root, '-WhatIf')
  Add-Check -Name 'register-neg:publisher' -Ok ($result.ExitCode -ne 0) -Detail ('exit=' + $result.ExitCode)
  Assert-Match -Name 'register-neg:publisher-reason' -Text $result.Output -Pattern 'identity-publisher-empty'

  $badXmlRoot = New-TempRoot -Tag 'badxml'
  $badXml = Join-Path $badXmlRoot 'AppxManifest.xml'
  Set-Content -LiteralPath $badXml -Value '<Package><Identity Name="X"' -NoNewline
  $result = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', $badXml, '-ExternalLocation', $badXmlRoot, '-WhatIf')
  Add-Check -Name 'register-neg:xml' -Ok ($result.ExitCode -ne 0) -Detail ('exit=' + $result.ExitCode)
  Assert-Match -Name 'register-neg:xml-reason' -Text $result.Output -Pattern 'manifest-xml-invalid'

  # A missing DLL is a WARNING (measured: registration succeeds without it and
  # the menu is dead). If this ever becomes a hard failure, the doc's advice
  # "registration works, rebuild to make the menu appear" would be wrong.
  $noDll = New-TempRoot -Tag 'nodll'
  Copy-Item -LiteralPath $fixture.Manifest -Destination (Join-Path $noDll 'AppxManifest.xml')
  $result = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-ManifestPath', (Join-Path $noDll 'AppxManifest.xml'), '-ExternalLocation', $noDll, '-WhatIf')
  Add-Check -Name 'register-warn:dll-missing' -Ok ($result.ExitCode -eq 0) -Detail ('exit=' + $result.ExitCode)
  Assert-Match -Name 'register-warn:dll-missing-reason' -Text $result.Output -Pattern 'shell-dll-missing'

  # No manifest anywhere: the resolver must list every path it tried.
  $emptyRoot = New-TempRoot -Tag 'emptyrepo'
  $result = Invoke-CardScript -ScriptName 'register-loose.ps1' -Arguments @('-RepoRoot', $emptyRoot, '-WhatIf')
  Add-Check -Name 'register-neg:no-manifest' -Ok ($result.ExitCode -ne 0) -Detail ('exit=' + $result.ExitCode)
  Assert-Match -Name 'register-neg:no-manifest-reason' -Text $result.Output -Pattern 'AppxManifest.xml was not found'
  $tried = ([regex]::Matches($result.Output, '(?m)^  - ')).Count
  Add-Check -Name 'register-neg:no-manifest-search-count' -Ok ($tried -ge 3) -Detail ('listed=' + $tried)

  # --- 7. unregister is idempotent ---------------------------------------
  $absentName = $fixture.PackageName + '.Absent'
  $first = Invoke-CardScript -ScriptName 'unregister-loose.ps1' -Arguments @('-PackageName', $absentName, '-WhatIf')
  Add-Check -Name 'unregister-noop:exit' -Ok ($first.ExitCode -eq 0) -Detail ('exit=' + $first.ExitCode)
  Assert-Match -Name 'unregister-noop:marker' -Text $first.Output -Pattern 'NO-OP nothing to remove'
  $second = Invoke-CardScript -ScriptName 'unregister-loose.ps1' -Arguments @('-PackageName', $absentName, '-WhatIf')
  Add-Check -Name 'unregister-noop:idempotent' -Ok ($second.ExitCode -eq 0) -Detail ('exit=' + $second.ExitCode)
  Add-Check -Name 'unregister-noop:still-absent' -Ok ((@(Get-AppxPackage -Name $absentName -ErrorAction SilentlyContinue)).Count -eq 0) -Detail ''

  # --- 8. selfsign plan: no cert, no secret, honest admin/dev-mode -------
  $selfPlan = Invoke-CardScript -ScriptName 'selfsign-and-install.ps1' -Arguments @(
    '-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root,
    '-ShellExtDll', (Join-Path $fixture.Root 'PrismShellExt.dll'),
    '-BuildDir', $fixture.Build, '-CertDir', (Join-Path $fixture.Build 'certs'), '-WhatIf')
  Add-Check -Name 'selfsign-plan:exit' -Ok ($selfPlan.ExitCode -eq 0) -Detail ('exit=' + $selfPlan.ExitCode)
  Assert-Match -Name 'selfsign-plan:cert' -Text $selfPlan.Output -Pattern 'PLAN cert subject='
  Assert-Match -Name 'selfsign-plan:sign' -Text $selfPlan.Output -Pattern 'PLAN sign pfx='
  Assert-Match -Name 'selfsign-plan:trust' -Text $selfPlan.Output -Pattern 'PLAN trust import='
  Assert-Match -Name 'selfsign-plan:install' -Text $selfPlan.Output -Pattern 'PLAN install Add-AppxPackage -Path'
  Add-Check -Name 'selfsign-plan:no-package-built' -Ok (-not (Test-Path -LiteralPath (Join-Path $fixture.Build 'msix'))) -Detail (Join-Path $fixture.Build 'msix')

  $afterCerts = Get-CertCount -Subject $certSubject
  Add-Check -Name 'selfsign-plan:no-cert-created' -Ok ($beforeCerts -eq $afterCerts) -Detail ('before=' + $beforeCerts + ' after=' + $afterCerts)

  $secretMarker = 'C61-SECRET-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
  $secretRun = Invoke-CardScript -ScriptName 'selfsign-and-install.ps1' -Arguments @(
    '-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root,
    '-ShellExtDll', (Join-Path $fixture.Root 'PrismShellExt.dll'),
    '-BuildDir', $fixture.Build, '-CertDir', (Join-Path $fixture.Build 'certs'),
    '-PfxPassword', $secretMarker, '-WhatIf')
  Assert-Absent -Name 'selfsign-plan:secret-not-printed' -Text $secretRun.Output -Pattern ([regex]::Escape($secretMarker))
  Assert-Match -Name 'selfsign-plan:secret-redacted' -Text $secretRun.Output -Pattern 'PFX-PASSWORD <redacted>'

  $admin = Test-MsixElevated
  $adminText = 'ADMIN False'
  if ($admin) { $adminText = 'ADMIN True' }
  Assert-Match -Name 'selfsign-plan:admin' -Text $selfPlan.Output -Pattern $adminText

  $machineRun = Invoke-CardScript -ScriptName 'selfsign-and-install.ps1' -Arguments @(
    '-ManifestPath', $fixture.Manifest, '-ExternalLocation', $fixture.Root,
    '-ShellExtDll', (Join-Path $fixture.Root 'PrismShellExt.dll'),
    '-BuildDir', $fixture.Build, '-CertDir', (Join-Path $fixture.Build 'certs'), '-Machine', '-WhatIf')
  Add-Check -Name 'selfsign-machine:exit' -Ok ($machineRun.ExitCode -eq 0) -Detail ('exit=' + $machineRun.ExitCode)
  Assert-Match -Name 'selfsign-machine:localmachine-store' -Text $machineRun.Output -Pattern 'TRUST-STORE Cert:\\LocalMachine\\Root'
  if (-not $admin) {
    Assert-Match -Name 'selfsign-machine:admin-blocker' -Text $machineRun.Output -Pattern 'BLOCKER admin-required'
    Assert-Match -Name 'selfsign-machine:runas-hint' -Text $machineRun.Output -Pattern 'Start-Process -FilePath pwsh -Verb RunAs'
  } else {
    Assert-Absent -Name 'selfsign-machine:admin-blocker' -Text $machineRun.Output -Pattern 'BLOCKER admin-required'
  }

  # Nothing above may have touched the real package or the certificate store.
  $stillCerts = Get-CertCount -Subject $certSubject
  Add-Check -Name 'selftest:store-untouched' -Ok ($stillCerts -eq $beforeCerts) -Detail ('before=' + $beforeCerts + ' after=' + $stillCerts)
  Add-Check -Name 'selftest:fixture-not-installed' -Ok ((@(Get-AppxPackage -Name $fixture.PackageName -ErrorAction SilentlyContinue)).Count -eq 0) -Detail ''

  # --- 9. the doc must state the measured facts --------------------------
  $docPath = Join-Path $script:RepoRoot 'docs\MSIX-SIGNING.md'
  Add-Check -Name 'docs:exists' -Ok (Test-Path -LiteralPath $docPath -PathType Leaf) -Detail $docPath
  if (Test-Path -LiteralPath $docPath) {
    $doc = Get-Content -LiteralPath $docPath -Raw
    $tableRows = ([regex]::Matches($doc, '(?m)^\|')).Count
    Add-Check -Name 'docs:tradeoff-table' -Ok ($tableRows -ge 8) -Detail ('table rows=' + $tableRows)
    foreach ($token in @('register-loose.ps1', 'unregister-loose.ps1', 'selfsign-and-install.ps1',
                         'Add-AppxPackage -Register', '-ExternalLocation', 'SignPath',
                         'ms-settings:developers', 'Cert:\CurrentUser\Root', 'Cert:\LocalMachine\Root',
                         '0x80073CF9', '0x80073D2E', '0x80073CFF', '0x800B0109')) {
      Assert-Match -Name ('docs:has:' + $token) -Text $doc -Pattern ([regex]::Escape($token))
    }
    # The classic menu is the free path; the doc must say so without relying on
    # the reader opening the script.
    $classicMention = ([regex]::Matches($doc, 'classic|IContextMenu', 'IgnoreCase')).Count
    Add-Check -Name 'docs:classic-path-explained' -Ok ($classicMention -ge 2) -Detail ('mentions=' + $classicMention)

    # Certificate-free is a claim about cost, admin rights and distribution. A
    # trade-off table without those columns has not made the trade-off, so they are
    # asserted as table headers rather than inferred from prose. The headers are
    # built from code points because this test file is ASCII-only on purpose (see
    # the encoding checks): Windows PowerShell 5.1 reads BOM-less files as ANSI.
    Add-Check -Name 'docs:cost-column' -Ok ($doc.Contains('| ' + (ConvertFrom-CodePoints @(0x8D39, 0x7528)) + ' |')) -Detail 'cost column header missing'
    Add-Check -Name 'docs:admin-column' -Ok ($doc.Contains('| ' + (ConvertFrom-CodePoints @(0x9700, 0x8981, 0x7BA1, 0x7406, 0x5458)) + ' |')) -Detail 'admin column header missing'
    Add-Check -Name 'docs:distribution-column' -Ok ($doc.Contains('| ' + (ConvertFrom-CodePoints @(0x80FD, 0x5426, 0x5206, 0x53D1, 0x7ED9, 0x522B, 0x4EBA)) + ' |')) -Detail 'distribution column header missing'
    Add-Check -Name 'docs:classic-why-free' -Ok ($doc.Contains((ConvertFrom-CodePoints @(0x4E3A, 0x4EC0, 0x4E48, 0x7ECF, 0x5178, 0x83DC, 0x5355, 0x4E0D, 0x9700, 0x8981, 0x8BC1, 0x4E66)))) -Detail 'classic-menu explanation heading missing'

    # Every path must be runnable from the doc alone: one command block per path.
    $codeBlocks = ([regex]::Matches($doc, '(?m)^```powershell')).Count
    Add-Check -Name 'docs:command-blocks' -Ok ($codeBlocks -ge 5) -Detail ('powershell blocks=' + $codeBlocks)
    foreach ($command in @('register-loose.ps1 -BuildDir', 'unregister-loose.ps1', 'selfsign-and-install.ps1 -BuildDir',
                           'build-sparse.ps1', 'register-classic.ps1 -DllPath', 'Add-AppxPackage -Path', '-Pfx')) {
      Assert-Match -Name ('docs:cmd:' + $command) -Text $doc -Pattern ([regex]::Escape($command))
    }
  }

  # --- 10. fixtures are gone (CONTRACTS D9) ------------------------------
  $left = Remove-Fixtures
  Add-Check -Name 'fixtures:cleanup' -Ok ($left.Count -eq 0) -Detail ('left=' + $left.Count + ' of ' + $script:TempRoots.Count + ' fixture root(s)')
}

# ---------------------------------------------------------------------------
# Mutation glue: exact textual injections, each verified to have landed.
# ---------------------------------------------------------------------------
$script:MutationSpecs = @{
  DropExternalLocation = @{
    File     = 'register-loose.ps1'
    Before   = 'Add-AppxPackage -Register $manifest.Path -ExternalLocation $external -ErrorAction Stop'
    After    = 'Add-AppxPackage -Register $manifest.Path -ErrorAction Stop'
    Expected = 'script-call:register-loose'
  }
  DropManifestGate = @{
    File     = 'msix-common.ps1'
    Before   = 'if (-not $external) {'
    After    = 'if ($false) {'
    Expected = 'register-neg:external-content'
  }
  DropPublisherGate = @{
    File     = 'msix-common.ps1'
    Before   = 'if (-not $identity.Publisher) {'
    After    = 'if ($false) {'
    Expected = 'register-neg:publisher'
  }
  DropDevModeReport = @{
    File     = 'register-loose.ps1'
    Before   = '$dev.Source + ''='' + $devValue'
    After    = '''unknown'''
    Expected = 'register-plan:devmode'
  }
  LeakPassword = @{
    File     = 'selfsign-and-install.ps1'
    Before   = 'if (-not $secret) { $secret = [guid]::NewGuid().ToString(''N'') }'
    After    = 'if (-not $secret) { $secret = [guid]::NewGuid().ToString(''N'') }; Write-Host (''SECRET-LEAK '' + $secret)'
    Expected = 'selfsign-plan:secret-not-printed'
  }
  BreakUnregisterNoOp = @{
    File     = 'unregister-loose.ps1'
    Before   = 'if ($existing.Count -eq 0) {'
    After    = 'if ($false) {'
    Expected = 'unregister-noop:marker'
  }
  # The syntax gate must bite on a file that tokenises but no longer compiles.
  BreakSyntax = @{
    File     = 'unregister-loose.ps1'
    Before   = 'if (-not $PackageName) {'
    After    = 'if (-not $PackageName {'
    Expected = 'script-syntax:unregister-loose.ps1'
  }
  # The dev-mode-off FIX must carry the real script path, not a sentence.
  DropDevModeFix = @{
    File     = 'register-loose.ps1'
    Before   = 'foreach ($line in (Get-MsixDevModeFix -DevMode $dev)) { Write-Host (''FIX       '' + $line) }'
    After    = 'Write-Host ''FIX       enable Developer Mode in Settings'''
    Expected = 'register-devmode-off:which-script'
  }
  # Inserting a packaging/signing token into the classic path must turn the
  # certificate-free assertion red.
  DropClassicCertFree = @{
    File     = 'register-classic.ps1'
    Before   = '$ErrorActionPreference = ''Stop'''
    After    = '$ErrorActionPreference = ''Stop''; $null = Add-AppxPackage'
    Expected = 'classic-path:cert-free'
  }
  # A table row that keys the WRONG code is a dead row: the mapping check that
  # pins 0x80073CFF must go red. (Changing only the case would NOT be a defect:
  # PowerShell's switch compares case-insensitively -- measured, and the first
  # version of this mutation stayed green because of it.)
  ShiftHresultKey = @{
    File     = 'msix-common.ps1'
    Before   = "    '80073CFF' {"
    After    = "    '80073CFE' {"
    Expected = 'hresult:0x80073CFF'
  }
}

function New-MutatedToolsDir {
  param([string] $Mutation)
  $spec = $script:MutationSpecs[$Mutation]
  $root = New-TempRoot -Tag ('mutation-' + $Mutation)
  # Copy the WHOLE tools\msix folder: the syntax/encoding checks enumerate it, and
  # a missing card file would make those checks red for a reason unrelated to the
  # injected defect (D3: red for another reason is not CAUGHT).
  foreach ($file in @(Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object { $_.Extension -in @('.ps1', '.cmd') })) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $root $file.Name)
  }
  $target = Join-Path $root $spec.File
  $text = Get-Content -LiteralPath $target -Raw
  if (-not $text.Contains($spec.Before)) { return $null }   # injection missed
  $text = $text.Replace($spec.Before, $spec.After)
  if (-not $text.Contains($spec.After)) { return $null }
  Set-Content -LiteralPath $target -Value $text -NoNewline
  return $root
}

if ($SyntaxOnly) { Invoke-SyntaxChecks -ToolsDir $PSScriptRoot }

try {
  if ($Mutate) {
    if (-not $script:MutationSpecs.ContainsKey($Mutate)) {
      Write-Host ('MSIX-FREE(MUTATION ' + $Mutate + '): UNKNOWN (known: ' + (($script:MutationSpecs.Keys | Sort-Object) -join ',') + ')')
      exit 1
    }
    $tools = New-MutatedToolsDir -Mutation $Mutate
    if (-not $tools) {
      Write-Host ('MSIX-FREE(MUTATION ' + $Mutate + '): NOT INJECTED (the source no longer contains the target text)')
      exit 1
    }
    $script:MutationName = $Mutate
    $script:MutationExpected = $script:MutationSpecs[$Mutate].Expected
    Invoke-MsixFreeChecks -ToolsDir $tools
    # Reaching this line means the expected guard never went red: the assertion is
    # blind to this defect, which is exactly what a negative control must report.
    $failed = @($script:Checks | Where-Object { -not $_.Ok })
    Write-Host ('MSIX-FREE(MUTATION ' + $Mutate + '): NOT CAUGHT (' + $script:MutationExpected + ' stayed green; ' + $failed.Count + ' unrelated check(s) red)')
    foreach ($f in $failed) { Write-Host ('  red: ' + $f.Name + ' ' + $f.Detail) }
    exit 1
  }

  Invoke-MsixFreeChecks -ToolsDir $PSScriptRoot
  $failed = @($script:Checks | Where-Object { -not $_.Ok })
  if ($failed.Count -eq 0) {
    Write-Host ('MSIX-FREE: PASS (' + $script:Checks.Count + ' checks, ' + $script:TempRoots.Count + ' fixtures removed: all-script syntax/encoding, manifest vs compiled constants, loose-register plan, 4 negative fixtures, dev-mode-off 0x80073CFF branch + HRESULT table, idempotent unregister, self-sign plan without cert/secret, doc facts)')
    exit 0
  }
  foreach ($f in $failed) { Write-Host ('RED ' + $f.Name + ' ' + $f.Detail) }
  Write-Host ('MSIX-FREE: FAIL (' + $failed.Count + ' of ' + $script:Checks.Count + ' checks)')
  exit 1
} finally {
  if (-not $KeepFixtures) {
    foreach ($root in $script:TempRoots) {
      Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
  } else {
    foreach ($root in $script:TempRoots) { Write-Host ('KEPT ' + $root) }
  }
}
