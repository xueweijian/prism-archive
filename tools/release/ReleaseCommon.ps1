#requires -Version 5.1
<#
.SYNOPSIS
  Shared primitives for the PrismArchive release pipeline (card C24).

.DESCRIPTION
  Dot-source this file; it defines no side effects beyond the constants below:

    . (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

  What lives here, and why it is ONE file:

  * version.json is the single source of truth for the shipped version.  The
    build still carries two copies of that number (CMakeLists.txt project
    VERSION and the VERSIONINFO block in Src/App/PrismArchive.rc); Get-VersionDrift
    compares all three and the packager REFUSES to cut a package when they
    disagree, so a release can never be named 0.1.0 while its VERSIONINFO says
    0.1.0.0 from an older tree.
  * The expected zip content is derived from the cmake --install TREE ON DISK,
    never from the packager's own copy list.  That is the whole sharpness of the
    completeness assertion: dropping a copy step leaves the expected list intact
    and goes red (CONTRACTS D3 -- a check that restates the writer is not a
    check).
  * SHA256SUMS.txt is GNU sha256sum format ("<hash>  <path>", two spaces,
    forward slashes), so a user can verify the package with coreutils on
    Linux/macOS or Get-FileHash on Windows without our code.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ReleaseCommonRoot = $PSScriptRoot

# Files the packager GENERATES (they exist in the zip, not in the install tree).
$script:GeneratedPackageFiles = @('PORTABLE.md', 'prism-package.json', 'SHA256SUMS.txt')

# Files a package may never ship without.  Hard-coded on purpose: an exclusion
# rule can never hide one of these (install-layout-derived expectations alone
# would happily 'pass' a build that stopped producing the shell DLLs).
$script:RequiredPayload = @(
    'PrismArchive.exe',
    'PrismShellExt.dll',
    'PrismShellExtClassic.dll',
    'install.ps1',
    'uninstall.ps1',
    'PORTABLE.md',
    'prism-package.json',
    'SHA256SUMS.txt',
    'Assets/Icon/prism.ico'
)

# Files cmake --install produces that the portable zip deliberately does NOT
# carry.  Every exclusion is recorded (path + reason) in prism-package.json, so
# "why is this file missing from the zip" is answerable from the artifact.
$script:PortableExclusions = @(
    [pscustomobject]@{
        Pattern = 'share/prism-archive/*'
        Reason  = 'duplicate shader copy from the top-level install(DIRECTORY Assets/Shaders DESTINATION share/prism-archive); the portable zip ships Assets/Shaders once'
    },
    [pscustomobject]@{
        Pattern = 'Assets/Package/*'
        Reason  = 'MSIX sparse-package payload (AppxManifest.xml + tile logos); the portable zip installs through install.ps1, not package identity'
    }
)

# ---------------------------------------------------------------------------
# version: one source, three readers
# ---------------------------------------------------------------------------

function Get-PrismVersionInfo {
    <#
      Read tools/release/version.json.  Returns an object whose ZipName is the
      artifact name, so there is exactly one place that decides what a package
      is called.
    #>
    param([string]$VersionFile = (Join-Path $script:ReleaseCommonRoot 'version.json'))

    if (-not (Test-Path -LiteralPath $VersionFile -PathType Leaf)) {
        throw "version source of truth not found: $VersionFile"
    }
    $json = Get-Content -LiteralPath $VersionFile -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($field in @('product', 'version', 'build', 'runtime')) {
        if (-not ($json.PSObject.Properties.Name -contains $field)) {
            throw "version.json is missing the '$field' field"
        }
    }
    $version = [string]$json.version
    if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') {
        throw "version.json version '$version' is not x.y.z[-suffix]"
    }
    $build = [int]$json.build
    $parts = $version.Split('-')[0].Split('.')
    $numeric = [string]::Join('.', @($parts[0], $parts[1], $parts[2], [string]$build))
    $product = [string]$json.product
    $runtime = [string]$json.runtime

    return [pscustomobject]@{
        Product        = $product
        Version        = $version
        Build          = $build
        NumericVersion = $numeric
        Runtime        = $runtime
        Channel        = [string]$json.channel
        Tag            = 'v' + $version
        ZipName        = $product + '-' + $version + '-' + $runtime + '.zip'
        VersionFile    = (Resolve-Path -LiteralPath $VersionFile).Path
    }
}

function Get-VersionDrift {
    <#
      Compare version.json against the copies that still exist in the build.
      Returns { Errors, Notes }: Errors are fatal for a package build, Notes are
      the copies that were proven to agree.
    #>
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)]$VersionInfo
    )
    $errors = New-Object System.Collections.Generic.List[string]
    $notes = New-Object System.Collections.Generic.List[string]

    $cmakePath = Join-Path $RepoRoot 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        $errors.Add('CMakeLists.txt not found; cannot check the project VERSION')
    } else {
        $text = Get-Content -LiteralPath $cmakePath -Raw
        if ($text -match 'project\s*\(\s*PrismArchive\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
            $declared = $Matches[1]
            if ($declared -ne $VersionInfo.Version) {
                $errors.Add("CMakeLists.txt project VERSION $declared != version.json $($VersionInfo.Version)")
            } else {
                $notes.Add("CMakeLists.txt project VERSION $declared")
            }
        } else {
            $errors.Add('CMakeLists.txt: no "project(PrismArchive VERSION x.y.z)" declaration found')
        }
    }

    $rcPath = Join-Path $RepoRoot 'Src/App/PrismArchive.rc'
    if (-not (Test-Path -LiteralPath $rcPath -PathType Leaf)) {
        $errors.Add('Src/App/PrismArchive.rc not found; cannot check the VERSIONINFO')
    } else {
        $rc = Get-Content -LiteralPath $rcPath -Raw
        if ($rc -match '#define\s+PRISM_VERSION_MAJOR\s+([0-9]+)') {
            $want = $VersionInfo.NumericVersion.Split('.')
            $names = @('MAJOR', 'MINOR', 'PATCH', 'BUILD')
            for ($i = 0; $i -lt $names.Count; $i++) {
                $macro = 'PRISM_VERSION_' + $names[$i]
                if ($rc -notmatch ("#define\s+" + $macro + "\s+([0-9]+)")) {
                    $errors.Add("PrismArchive.rc: $macro is missing")
                    continue
                }
                if ($Matches[1] -ne $want[$i]) {
                    $errors.Add("PrismArchive.rc: $macro $($Matches[1]) != $($want[$i]) (version.json $($VersionInfo.Version) build $($VersionInfo.Build))")
                }
            }
            if ($rc -match '#define\s+PRISM_VERSION_STRING\s+"([^"]*)"') {
                if ($Matches[1] -ne $VersionInfo.NumericVersion) {
                    $errors.Add("PrismArchive.rc: PRISM_VERSION_STRING '$($Matches[1])' != '$($VersionInfo.NumericVersion)'")
                }
            } else {
                $errors.Add('PrismArchive.rc: PRISM_VERSION_STRING is missing')
            }
            if ($errors.Count -eq 0) {
                $notes.Add("PrismArchive.rc VERSIONINFO $($VersionInfo.NumericVersion)")
            }
        } elseif ($rc -match 'prism_version\.h') {
            $notes.Add('PrismArchive.rc VERSIONINFO is generated from version.json (C24 patch applied)')
        } else {
            $errors.Add('PrismArchive.rc: neither PRISM_VERSION_MAJOR nor a generated prism_version.h include was found')
        }
    }

    return [pscustomobject]@{ Errors = $errors; Notes = $notes }
}

# ---------------------------------------------------------------------------
# small utilities
# ---------------------------------------------------------------------------

function Get-OrdinalSorted {
    <# Culture-independent sort: the zip listing must not depend on the machine locale. #>
    param([string[]]$Items)
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($i in $Items) { if ($null -ne $i) { $list.Add([string]$i) } }
    $list.Sort([System.StringComparer]::Ordinal)
    return , $list.ToArray()
}

function Get-RelativePathForward {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Path
    )
    $rootFull = (Resolve-Path -LiteralPath $Root).Path.TrimEnd([char]92, [char]47)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path '$full' is outside '$rootFull'"
    }
    return $full.Substring($rootFull.Length).TrimStart([char]92, [char]47).Replace([char]92, [char]47)
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Utf8File {
    <# UTF-8 without BOM, LF line endings: stable bytes on every platform, and
       the manifest/checksums stay diffable and byte-identical between runs. #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [AllowEmptyString()][string]$Text = ''
    )
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $lf = [string][char]10
    $cr = [string][char]13
    $normalized = $Text.Replace($cr + $lf, $lf)
    if (-not $normalized.EndsWith($lf)) { $normalized += $lf }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $normalized, $encoding)
}

function Format-Size {
    param([long]$Bytes)
    if ($Bytes -ge 1MB) { return ('{0:N2} MiB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KiB' -f ($Bytes / 1KB)) }
    return "$Bytes B"
}

# ---------------------------------------------------------------------------
# install layout -> expected zip content
# ---------------------------------------------------------------------------

function Get-InstalledPayload {
    <#
      Enumerate the cmake --install prefix and apply the documented exclusions.
      Installed = everything the install tree holds (truth for "the build
      produced it"), Files = what the portable zip must contain.
    #>
    param([Parameter(Mandatory)][string]$InstallRoot)

    if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
        throw "install root not found: $InstallRoot"
    }
    $all = New-Object System.Collections.Generic.List[string]
    foreach ($f in Get-ChildItem -LiteralPath $InstallRoot -Recurse -File -Force) {
        $all.Add((Get-RelativePathForward -Root $InstallRoot -Path $f.FullName))
    }
    $installed = Get-OrdinalSorted $all.ToArray()

    $excluded = New-Object System.Collections.Generic.List[object]
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($rel in $installed) {
        $rule = $null
        foreach ($r in $script:PortableExclusions) {
            if ($rel -like $r.Pattern) { $rule = $r; break }
        }
        if ($rule) {
            $excluded.Add([pscustomobject]@{ Path = $rel; Reason = $rule.Reason })
        } else {
            $kept.Add($rel)
        }
    }

    return [pscustomobject]@{
        Files     = $kept.ToArray()
        Excluded  = $excluded.ToArray()
        Installed = $installed
    }
}

function Get-StagedFiles {
    param([Parameter(Mandatory)][string]$Stage)
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($f in Get-ChildItem -LiteralPath $Stage -Recurse -File -Force) {
        $list.Add((Get-RelativePathForward -Root $Stage -Path $f.FullName))
    }
    return , (Get-OrdinalSorted $list.ToArray())
}

# ---------------------------------------------------------------------------
# zip reading + verification
# ---------------------------------------------------------------------------

function Initialize-ZipApi {
    if (-not ('System.IO.Compression.ZipFile' -as [type])) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
    }
}

function Get-ZipEntries {
    <#
      Read the zip and hash every entry, straight from the archive stream: the
      verdict then describes the FILE that ships, not the staging directory that
      produced it.
    #>
    param([Parameter(Mandatory)][string]$ZipPath)

    Initialize-ZipApi
    $byName = @{}
    $names = New-Object System.Collections.Generic.List[string]
    $duplicates = New-Object System.Collections.Generic.List[string]
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            $name = $entry.FullName.Replace([char]92, [char]47)
            if ($byName.ContainsKey($name)) { $duplicates.Add($name); continue }
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $stream = $entry.Open()
                try { $digest = $sha.ComputeHash($stream) } finally { $stream.Dispose() }
            } finally { $sha.Dispose() }
            $hex = -join ($digest | ForEach-Object { $_.ToString('x2') })
            $byName[$name] = [pscustomobject]@{ Name = $name; Size = [long]$entry.Length; Hash = $hex }
            $names.Add($name)
        }
    } finally { $zip.Dispose() }

    return [pscustomobject]@{
        ByName     = $byName
        EntryNames = (Get-OrdinalSorted $names.ToArray())
        Duplicates = $duplicates.ToArray()
        Count      = $byName.Count
    }
}

function Get-ZipEntryText {
    param(
        [Parameter(Mandatory)]$Zip,
        [Parameter(Mandatory)][string]$Name
    )
    Initialize-ZipApi
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip.Path)
    try {
        $entry = $archive.GetEntry($Name)
        if ($null -eq $entry) { throw "zip entry not found: $Name" }
        $stream = $entry.Open()
        try {
            $reader = New-Object System.IO.StreamReader($stream, (New-Object System.Text.UTF8Encoding($false)), $true)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}

function Test-PackageContents {
    <#
      The package verdict.  Every failure is named, so a red build says WHICH
      property broke (missing file / unexpected file / hash mismatch / engine
      DLL present / manifest disagrees).

      ExpectedFiles must come from the install tree (plus the generated
      documents), never from the packager's copy list.
    #>
    param(
        [Parameter(Mandatory)][string]$ZipPath,
        [Parameter(Mandatory)][string[]]$ExpectedFiles,
        [Parameter(Mandatory)]$VersionInfo,
        [string[]]$Required = $script:RequiredPayload
    )
    $errors = New-Object System.Collections.Generic.List[string]
    $checks = 0

    if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
        $errors.Add("package not found: $ZipPath")
        return [pscustomobject]@{ Errors = $errors; Assertions = 0; EntryCount = 0; ZipSha256 = ''; Zip = $null }
    }

    $zip = Get-ZipEntries -ZipPath $ZipPath
    $zip | Add-Member -NotePropertyName Path -NotePropertyValue ((Resolve-Path -LiteralPath $ZipPath).Path) -Force

    foreach ($dup in $zip.Duplicates) { $errors.Add("duplicate zip entry: $dup") }

    $leaf = Split-Path -Leaf $ZipPath
    if ($leaf -ne $VersionInfo.ZipName) {
        $errors.Add("package file name '$leaf' != '$($VersionInfo.ZipName)' from version.json")
    }
    $checks++

    # 1. exact set equality against the install layout -----------------------
    foreach ($want in $ExpectedFiles) {
        if (-not $zip.ByName.ContainsKey($want)) {
            $errors.Add("MISSING from package: $want")
        }
    }
    foreach ($got in $zip.EntryNames) {
        if ($ExpectedFiles -notcontains $got) {
            $errors.Add("UNEXPECTED in package: $got (absent from the cmake --install layout or dropped by a policy exclusion)")
        }
    }
    $checks += $ExpectedFiles.Count

    # 2. the LGPL engine never ships ----------------------------------------
    foreach ($got in $zip.EntryNames) {
        if ($got -match '(^|/)7z\.dll$') {
            $errors.Add("FORBIDDEN in package: $got (7-Zip is LGPL and is discovered at runtime; the engine must never be bundled)")
        }
    }
    $checks++

    # 3. files a package may never go without --------------------------------
    foreach ($req in $Required) {
        if (-not $zip.ByName.ContainsKey($req)) {
            $errors.Add("REQUIRED file missing from package: $req")
        }
        $checks++
    }
    $expectedShaders = @($ExpectedFiles | Where-Object { $_ -like 'Assets/Shaders/*.hlsl' })
    if ($expectedShaders.Count -eq 0) {
        $errors.Add('the install layout has no Assets/Shaders/*.hlsl -- the Liquid Glass shaders would not ship')
    }
    $checks++

    # 4. SHA256SUMS.txt covers every file but itself -------------------------
    $sumsRel = 'SHA256SUMS.txt'
    if (-not $zip.ByName.ContainsKey($sumsRel)) {
        $errors.Add("REQUIRED file missing from package: $sumsRel")
    } else {
        $sumText = Get-ZipEntryText -Zip $zip -Name $sumsRel
        $covered = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($rawLine in $sumText.Split([char]10)) {
            $line = $rawLine.TrimEnd([char]13)
            if ($line.Trim().Length -eq 0) { continue }
            if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
                $errors.Add("SHA256SUMS.txt: malformed line '$line' (expected '<64 hex>  <path>')")
                continue
            }
            $hash = $Matches[1]
            $rel = $Matches[2]
            $checks++
            if (-not $zip.ByName.ContainsKey($rel)) {
                $errors.Add("SHA256SUMS.txt names a file that is not in the zip: $rel")
                continue
            }
            if (-not $covered.Add($rel)) {
                $errors.Add("SHA256SUMS.txt lists $rel twice")
                continue
            }
            if ($zip.ByName[$rel].Hash -ne $hash) {
                $errors.Add(("SHA256SUMS.txt hash mismatch for " + $rel + ": listed " + $hash + ", actual " + $zip.ByName[$rel].Hash))
            }
        }
        foreach ($want in $ExpectedFiles) {
            if ($want -eq $sumsRel) { continue }
            if (-not $covered.Contains($want)) {
                $errors.Add("SHA256SUMS.txt does not cover $want (an unlisted file cannot be verified by the user)")
            }
            $checks++
        }
    }

    # 5. prism-package.json: same payload, same bytes -------------------------
    $pkgRel = 'prism-package.json'
    if (-not $zip.ByName.ContainsKey($pkgRel)) {
        $errors.Add("REQUIRED file missing from package: $pkgRel")
    } else {
        $pkg = $null
        try { $pkg = Get-ZipEntryText -Zip $zip -Name $pkgRel | ConvertFrom-Json }
        catch { $errors.Add("prism-package.json does not parse: $($_.Exception.Message)") }
        if ($null -ne $pkg) {
            $checks++
            if ([string]$pkg.version -ne $VersionInfo.Version) {
                $errors.Add("prism-package.json version '$($pkg.version)' != '$($VersionInfo.Version)'")
            }
            if ([string]$pkg.zipName -ne $VersionInfo.ZipName) {
                $errors.Add("prism-package.json zipName '$($pkg.zipName)' != '$($VersionInfo.ZipName)'")
            }
            if ([string]$pkg.engine -ne 'external') {
                $errors.Add("prism-package.json must record the 7-Zip engine as 'external' (got '$($pkg.engine)')")
            }
            $claimed = @($pkg.files)
            $shouldList = @($ExpectedFiles | Where-Object { $_ -ne $pkgRel -and $_ -ne $sumsRel })
            if ($claimed.Count -ne $shouldList.Count) {
                $errors.Add("prism-package.json lists $($claimed.Count) file(s), the package ships $($shouldList.Count) payload file(s)")
            }
            $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
            foreach ($entry in $claimed) {
                $rel = [string]$entry.path
                $checks++
                if (-not $listed.Add($rel)) { $errors.Add("prism-package.json lists $rel twice"); continue }
                if (-not $zip.ByName.ContainsKey($rel)) {
                    $errors.Add("prism-package.json lists $rel which is not in the zip")
                    continue
                }
                if ([string]$entry.sha256 -ne $zip.ByName[$rel].Hash) {
                    $errors.Add("prism-package.json sha256 mismatch for $rel")
                }
                if ([long]$entry.size -ne [long]$zip.ByName[$rel].Size) {
                    $errors.Add("prism-package.json size mismatch for $rel ($($entry.size) != $($zip.ByName[$rel].Size))")
                }
            }
            foreach ($rel in $shouldList) {
                if (-not $listed.Contains($rel)) { $errors.Add("prism-package.json omits $rel") }
            }
        }
    }

    return [pscustomobject]@{
        Errors     = $errors
        Assertions = $checks
        EntryCount = $zip.Count
        ZipSha256  = (Get-Sha256 -Path $ZipPath)
        Zip        = $zip
    }
}
