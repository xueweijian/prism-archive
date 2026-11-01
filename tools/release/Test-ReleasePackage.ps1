#requires -Version 5.1
<#
.SYNOPSIS
  Verify a PrismArchive package the way a user (or a release gate) would.

.DESCRIPTION
  Two modes, deliberately different in strength:

  * with -InstallRoot: the zip must match the cmake --install layout EXACTLY
    (missing file, extra file, wrong hash, wrong size, forbidden engine DLL all
    go red).  This is what CI runs, because only the build knows what the
    package was supposed to contain.
  * without -InstallRoot: the zip must be INTERNALLY consistent (SHA256SUMS.txt
    covers every file, prism-package.json agrees with the bytes, the required
    files are present, no 7z.dll).  This is what a user can run after
    downloading.  It cannot detect a consistently thin package -- there is
    nothing to compare against -- which is exactly why the CI lane passes
    -InstallRoot.

  If a SHA256SUMS.txt sits next to the zip (the dist layout) and names it, the
  archive's own hash is checked too: the download is verified, not just its
  contents.

.EXAMPLE
  pwsh -File tools\release\Test-ReleasePackage.ps1 -ZipPath dist\PrismArchive-0.1.0-win-x64.zip

.EXAMPLE
  pwsh -File tools\release\Test-ReleasePackage.ps1 -ZipPath dist\PrismArchive-0.1.0-win-x64.zip -InstallRoot E:\build\install
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ZipPath,
    [string]$InstallRoot = '',
    [string]$VersionFile = '',
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

$script:Quiet = [bool]$Quiet

function Say {
    param([string]$Text, [string]$Level = 'info')
    if ($script:Quiet -and $Level -eq 'info') { return }
    $prefix = switch ($Level) { 'warn' { 'WARN  ' } 'error' { 'FAIL  ' } default { '' } }
    Write-Host ($prefix + $Text)
}

$exitCode = 1
try {
    $versionFileArg = $VersionFile
    if (-not $versionFileArg) { $versionFileArg = Join-Path $PSScriptRoot 'version.json' }
    $v = Get-PrismVersionInfo -VersionFile $versionFileArg

    $resolved = (Resolve-Path -LiteralPath $ZipPath).Path
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        # CI knows the output directory, not necessarily the versioned file name
        # (which belongs to version.json). Exactly one archive must live there.
        $candidates = @(Get-ChildItem -LiteralPath $resolved -File -Filter 'PrismArchive-*-win-x64.zip')
        if ($candidates.Count -ne 1) {
            throw ("expected exactly one PrismArchive-*-win-x64.zip in " + $resolved + ", found " + $candidates.Count)
        }
        $resolved = $candidates[0].FullName
    }
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw ("package not found: " + $resolved)
    }
    $full = $resolved
    Say ("VERIFY: " + $full)
    Say ("VERSION: " + $v.Version + " (expected archive name " + $v.ZipName + ")")

    if ($InstallRoot) {
        if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
            throw "install root not found: $InstallRoot"
        }
        $payload = Get-InstalledPayload -InstallRoot $InstallRoot
        $expected = New-Object System.Collections.Generic.List[string]
        foreach ($rel in $payload.Files) { $expected.Add($rel) }
        foreach ($gen in $script:GeneratedPackageFiles) { $expected.Add($gen) }
        $expectedArray = Get-OrdinalSorted $expected.ToArray()
        $mode = 'exact against cmake --install (' + $payload.Files.Count + ' installed file(s) + 3 generated)'
    } else {
        $entries = Get-ZipEntries -ZipPath $full
        if (-not $entries.ByName.ContainsKey('prism-package.json')) {
            throw 'prism-package.json is missing, so the package cannot describe itself; pass -InstallRoot for a full check'
        }
        $zipForText = $entries | Add-Member -NotePropertyName Path -NotePropertyValue $full -Force -PassThru
        $pkg = Get-ZipEntryText -Zip $zipForText -Name 'prism-package.json' | ConvertFrom-Json
        $expected = New-Object System.Collections.Generic.List[string]
        foreach ($entry in @($pkg.files)) { $expected.Add([string]$entry.path) }
        $expected.Add('prism-package.json')
        $expected.Add('SHA256SUMS.txt')
        $expectedArray = Get-OrdinalSorted $expected.ToArray()
        $mode = 'self-consistency from prism-package.json (' + (@($pkg.files).Count) + ' claimed file(s)); pass -InstallRoot to prove completeness'
    }
    Say ("MODE:    " + $mode)

    $result = Test-PackageContents -ZipPath $full -ExpectedFiles $expectedArray -VersionInfo $v
    foreach ($e in $result.Errors) { Say $e 'error' }

    # dist/ sidecar: the hash of the archive itself, not of its contents.
    $sidecar = Join-Path (Split-Path -Parent $full) 'SHA256SUMS.txt'
    if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
        $listed = $null
        foreach ($line in [System.IO.File]::ReadAllText($sidecar).Split([char]10)) {
            if ($line -match '^([0-9a-f]{64})  (.+)$' -and $Matches[2] -eq $v.ZipName) { $listed = $Matches[1] }
        }
        if ($listed) {
            if ($listed -eq $result.ZipSha256) {
                Say ("ARCHIVE: " + $result.ZipSha256 + " matches " + $sidecar)
            } else {
                $result.Errors.Add("archive hash " + $result.ZipSha256 + " != " + $listed + " listed in " + $sidecar)
                Say ("archive hash " + $result.ZipSha256 + " != " + $listed + " listed in " + $sidecar) 'error'
            }
        } else {
            Say ("ARCHIVE: " + $result.ZipSha256 + " (" + $sidecar + " does not name " + $v.ZipName + ")")
        }
    } else {
        Say ("ARCHIVE: " + $result.ZipSha256 + " (no sidecar SHA256SUMS.txt next to the zip)")
    }

    if ($result.Errors.Count -gt 0) {
        Say ("VERIFY: FAIL (" + $result.Errors.Count + " assertion(s))") 'error'
        $exitCode = 1
    } else {
        Say ("VERIFY: PASS (" + $result.Assertions + " assertions, " + $result.EntryCount + " entries, engine not bundled)")
        $exitCode = 0
    }
} catch {
    Say ($_.Exception.Message) 'error'
    Say 'VERIFY: FAIL (unhandled error)' 'error'
    $exitCode = 1
}
exit $exitCode
