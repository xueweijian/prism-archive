#requires -Version 5.1
<#
.SYNOPSIS
  Cut the distributable Windows payload: PrismArchive-<version>-win-x64.zip,
  self-describing and self-verifying.

.DESCRIPTION
  One command, four guarantees:

    1. cmake --install produces the layout (the same tree install.ps1 deploys),
       so the package cannot quietly disagree with what a real install writes.
    2. The expected zip content is DERIVED FROM THAT LAYOUT, not from the copy
       list this script walks.  Dropping a file from the copy step is therefore
       a red build, not a silent thin package (CONTRACTS D3).
    3. The zip carries prism-package.json (version, git commit, size + sha256 of
       every file, and every file excluded by policy WITH ITS REASON) and
       SHA256SUMS.txt (GNU sha256sum format, covers every file but itself).
    4. The finished archive is re-opened and re-hashed from the zip stream: the
       verdict describes the bytes that ship, not the staging directory.

  The version comes from tools/release/version.json only.  Drift against
  CMakeLists.txt / Src/App/PrismArchive.rc aborts the build before any artifact
  is written (a package named 0.1.0 whose VERSIONINFO says something else is
  worse than no package).

  7z.dll is never packaged: the engine is LGPL and discovered at runtime.  The
  verifier fails the build if any 7z.dll ever appears in the archive.

.PARAMETER BuildDir
  CMake build directory to install from.  Ignored when -InstallRoot is given.

.PARAMETER InstallRoot
  Reuse an existing cmake --install tree (for re-packaging without rebuilding).

.PARAMETER OutDir
  Where the zip + dist SHA256SUMS.txt land.  Default: <repo>/dist (or a temp
  directory for mutation runs, so a deliberately broken package is never kept).

.PARAMETER Mutate
  Negative control (CONTRACTS D3).  One of:
    DropShellExtClassic | Extra7zDll | SumGap
  Each injects exactly one defect, VERIFIES THE INJECTION HAPPENED, and then
  exits 0 only when the package verification went red.  "NOT INJECTED" and
  "NOT CAUGHT" both exit 1: a mutation that silently did nothing would otherwise
  look like a sharp guard.

.EXAMPLE
  . E:\wg\env.ps1 -Card c24
  pwsh -File tools\release\New-PortablePackage.ps1 -BuildDir E:\build\wt-c24

.EXAMPLE
  pwsh -File tools\release\New-PortablePackage.ps1 -BuildDir E:\build\wt-c24 -Mutate DropShellExtClassic
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$BuildDir = '',
    [string]$InstallRoot = '',
    [string]$OutDir = '',
    [string]$Config = '',
    [string]$VersionFile = '',
    [string]$Mutate = '',
    [switch]$KeepStaging,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

$script:Quiet = [bool]$Quiet
$script:TempRoot = $null
$script:InjectionApplied = $false
$script:InjectionDetail = ''

function Say {
    param([string]$Text, [string]$Level = 'info')
    if ($script:Quiet -and $Level -eq 'info') { return }
    $prefix = switch ($Level) {
        'warn' { 'WARN  ' }
        'error' { 'FAIL  ' }
        default { '' }
    }
    Write-Host ($prefix + $Text)
}

function Get-GitCommit {
    param([string]$Root)
    if ($env:PRISM_GIT_COMMIT) { return [string]$env:PRISM_GIT_COMMIT }
    try {
        $commit = & git -C $Root rev-parse --short=12 2>$null
        if ($LASTEXITCODE -eq 0 -and $commit) { return ([string]$commit).Trim() }
    } catch { }
    return 'unknown'
}

function Get-BuildTimestamp {
    <# SOURCE_DATE_EPOCH is honoured so a reproducible pipeline can pin the
       manifest timestamp; otherwise UTC now. #>
    if ($env:SOURCE_DATE_EPOCH) {
        return [DateTimeOffset]::FromUnixTimeSeconds([long]$env:SOURCE_DATE_EPOCH).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    return [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
}

function New-PortableReadme {
    param(
        [Parameter(Mandatory)]$VersionInfo,
        [string]$Commit = 'unknown',
        [string]$BuiltAt = ''
    )
    $template = @'
# PrismArchive {{VERSION}} portable ({{RUNTIME}})

Windows 10 1809+ / Windows 11, x64. 解压即用：不写注册表，不写 %LOCALAPPDATA%。

构建：commit {{COMMIT}}，打包于 {{BUILT_AT}}（UTC）。
完整包清单与每个文件的 SHA256 见 prism-package.json 与 SHA256SUMS.txt。

## 三步开始用

1. 解压到任意目录（例如 D:\Apps\PrismArchive）。
2. 引擎不入包：PrismArchive 不携带 7z.dll（LGPL，见发布页说明）。安装 7-Zip，
   或把 PRISM_7Z_DLL 指向你的 7z.dll。解析顺序：PRISM_7Z_DLL → 标准安装路径。
3. 双击 PrismArchive.exe。

## 可选：文件关联（仍然每用户、免管理员）

    pwsh -File install.ps1 -SevenZipDll "C:\Program Files\7-Zip\7z.dll"
    pwsh -File uninstall.ps1

只要一个"记住引擎路径"的便携标记、完全不碰注册表：

    pwsh -File install.ps1 -Portable -SourceDir . -InstallDir . -SevenZipDll "C:\Program Files\7-Zip\7z.dll"

## 校验这个包

包内 SHA256SUMS.txt 覆盖除它自己以外的每个文件，格式与 GNU sha256sum 一致。

Linux / macOS（解压后）：

    sha256sum -c SHA256SUMS.txt

Windows PowerShell（解压后）：

    $bad = 0
    Get-Content SHA256SUMS.txt | ForEach-Object {
        $hash, $path = $_ -split '  ', 2
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $hash) { $bad++; Write-Host "MISMATCH $path" }
    }
    if ($bad) { throw "$bad file(s) do not match SHA256SUMS.txt" } else { 'OK: every file matches SHA256SUMS.txt' }

整包哈希在发布页的 SHA256SUMS.txt（与本 zip 同级）。

## 包里有什么 / 没有什么

有：PrismArchive.exe、PrismShellExt.dll、PrismShellExtClassic.dll、
    Assets/Shaders/*.hlsl（Liquid Glass 着色器）、Assets/Icon/prism.ico、
    install.ps1、uninstall.ps1、本说明、prism-package.json、SHA256SUMS.txt。

没有：7z.dll（LGPL 引擎，运行时发现）；MSIX 稀疏包载荷（Assets/Package/*，
    走 MSIX 分发线）；share/prism-archive/*（与 Assets/Shaders 重复的一份副本）。
    两份排除都在 prism-package.json 的 excludedFromPackage 里写明原因。
'@
    $text = $template
    $text = $text.Replace('{{VERSION}}', $VersionInfo.Version)
    $text = $text.Replace('{{RUNTIME}}', $VersionInfo.Runtime)
    $text = $text.Replace('{{COMMIT}}', $Commit)
    $text = $text.Replace('{{BUILT_AT}}', $BuiltAt)
    return $text
}

function Invoke-ReleaseBuild {
    <# Returns the process exit code.  Split out of the script body so the temp
       directory can be cleaned in a finally block without swallowing the
       verdict. #>
    param()

    # ---- 0. where are we ------------------------------------------------
    $root = $RepoRoot
    if (-not $root) { $root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path }
    $versionFileArg = $VersionFile
    if (-not $versionFileArg) { $versionFileArg = Join-Path $PSScriptRoot 'version.json' }

    $v = Get-PrismVersionInfo -VersionFile $versionFileArg
    Say ("VERSION: " + $v.Version + " (build " + $v.Build + ") from " + (Get-RelativePathForward -Root $root -Path $v.VersionFile))

    # ---- 1. one source of truth, three readers ---------------------------
    $drift = Get-VersionDrift -RepoRoot $root -VersionInfo $v
    foreach ($note in $drift.Notes) { Say ("VERSION COPY: " + $note) }
    if ($drift.Errors.Count -gt 0) {
        foreach ($e in $drift.Errors) { Say $e 'error' }
        Say "RELEASE PACKAGE: FAIL (version drift -- fix version.json or the copies it owns before cutting a package)" 'error'
        return 1
    }
    Say ("VERSION DRIFT: none (" + ($drift.Notes -join '; ') + ")")

    # ---- 2. install layout ----------------------------------------------
    $script:TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('prism-release-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $script:TempRoot | Out-Null

    $installRoot = $InstallRoot
    if (-not $installRoot) {
        if (-not $BuildDir) {
            Say 'RELEASE PACKAGE: FAIL (-BuildDir or -InstallRoot is required)' 'error'
            return 1
        }
        $installRoot = Join-Path $script:TempRoot 'install'
        $installArgs = @('--install', $BuildDir, '--prefix', $installRoot)
        if ($Config) { $installArgs += @('--config', $Config) }
        $installOutput = & cmake @installArgs 2>&1
        $installCode = $LASTEXITCODE
        if ($installCode -ne 0) {
            foreach ($line in @($installOutput | Select-Object -Last 20)) { Say ([string]$line) 'error' }
            Say ("RELEASE PACKAGE: FAIL (cmake --install " + $BuildDir + " exited " + $installCode + ")") 'error'
            return 1
        }
        Say ("INSTALL: cmake --install " + $BuildDir + " -> " + $installRoot)
    } else {
        Say ("INSTALL: reusing " + $installRoot)
    }

    $payload = Get-InstalledPayload -InstallRoot $installRoot
    Say ("LAYOUT: " + $payload.Installed.Count + " file(s) installed, " + $payload.Files.Count + " packaged, " + $payload.Excluded.Count + " excluded by policy")
    foreach ($group in @($payload.Excluded | Group-Object Reason)) {
        Say ("  excluded " + $group.Count + " file(s): " + $group.Name)
    }

    # A build that stopped producing a shell DLL would shrink BOTH lists and
    # still verify -- the hard-coded required set closes that hole.
    $missingFromLayout = 0
    foreach ($req in $script:RequiredPayload) {
        if ($script:GeneratedPackageFiles -contains $req) { continue }
        if ($payload.Installed -notcontains $req) {
            Say ("install layout is missing " + $req + " -- the build or the CMake install rule is incomplete") 'error'
            $missingFromLayout++
        }
    }
    if ($missingFromLayout -gt 0) {
        Say "RELEASE PACKAGE: FAIL (" + $missingFromLayout + " required file(s) never reached the install tree)" 'error'
        return 1
    }

    # ---- 3. stage the payload (and, for a mutation run, break it once) ---
    $stage = Join-Path $script:TempRoot 'stage'
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $skip = ''
    switch ($Mutate) {
        '' { }
        'DropShellExtClassic' { $skip = 'PrismShellExtClassic.dll' }
        'Extra7zDll' { }
        'SumGap' { }
        default {
            Say ("RELEASE PACKAGE: FAIL (unknown mutation '" + $Mutate + "'; known: DropShellExtClassic, Extra7zDll, SumGap)") 'error'
            return 1
        }
    }
    if ($skip) {
        Say ("MUTATION: deliberately dropping " + $skip + " from the copy step") 'warn'
    }

    foreach ($rel in $payload.Files) {
        if ($skip -and $rel -eq $skip) { continue }
        $destination = Join-Path $stage $rel
        $destinationDir = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $destinationDir)) {
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
        }
        Copy-Item -LiteralPath (Join-Path $installRoot $rel) -Destination $destination -Force
    }

    # CONTRACTS D3: prove the injection actually happened.  A regex/skip that
    # matched nothing would otherwise be reported as "the guard is sharp".
    if ($Mutate -eq 'DropShellExtClassic') {
        if (Test-Path -LiteralPath (Join-Path $stage 'PrismShellExtClassic.dll')) {
            $script:InjectionDetail = 'the DLL is still in staging: the drop never happened'
        } else {
            $script:InjectionApplied = $true
            $script:InjectionDetail = 'PrismShellExtClassic.dll absent from staging'
        }
    } elseif ($Mutate -eq 'Extra7zDll') {
        $fakeEngine = Join-Path $stage '7z.dll'
        [System.IO.File]::WriteAllBytes($fakeEngine, [byte[]](0x4D, 0x5A, 0x00, 0x00))
        if (Test-Path -LiteralPath $fakeEngine) {
            $script:InjectionApplied = $true
            $script:InjectionDetail = 'a 4-byte 7z.dll sits in staging'
        } else {
            $script:InjectionDetail = 'the fake engine DLL was not written'
        }
    }

    # SumGap injects later (it needs the checksum file to exist first).
    if ($Mutate -and $Mutate -ne 'SumGap' -and -not $script:InjectionApplied) {
        Say ("MUTATION " + $Mutate + ": NOT INJECTED (" + $script:InjectionDetail + ") -- the control itself is broken") 'error'
        return 1
    }

    # ---- 4. generated documents -----------------------------------------
    $commit = Get-GitCommit -Root $root
    $builtAt = Get-BuildTimestamp

    Write-Utf8File -Path (Join-Path $stage 'PORTABLE.md') -Text (New-PortableReadme -VersionInfo $v -Commit $commit -BuiltAt $builtAt)

    $manifestFiles = New-Object System.Collections.Generic.List[object]
    foreach ($rel in (Get-StagedFiles -Stage $stage)) {
        $full = Join-Path $stage $rel
        $manifestFiles.Add([ordered]@{
            path   = $rel
            size   = (Get-Item -LiteralPath $full).Length
            sha256 = (Get-Sha256 -Path $full)
        })
    }
    $excludedManifest = New-Object System.Collections.Generic.List[object]
    foreach ($ex in $payload.Excluded) {
        $excludedManifest.Add([ordered]@{ path = $ex.Path; reason = $ex.Reason })
    }
    $manifest = [ordered]@{
        product             = $v.Product
        version             = $v.Version
        numericVersion      = $v.NumericVersion
        channel             = $v.Channel
        runtime             = $v.Runtime
        zipName             = $v.ZipName
        builtAt             = $builtAt
        gitCommit           = $commit
        engine              = 'external'
        engineNote          = '7z.dll is LGPL and is never shipped; resolved at runtime from PRISM_7Z_DLL or the standard 7-Zip install paths'
        checksumFile        = 'SHA256SUMS.txt'
        checksumCoverage    = 'every file in the archive except SHA256SUMS.txt itself'
        files               = $manifestFiles.ToArray()
        excludedFromPackage = $excludedManifest.ToArray()
    }
    Write-Utf8File -Path (Join-Path $stage 'prism-package.json') -Text ($manifest | ConvertTo-Json -Depth 6)

    $sums = New-Object System.Collections.Generic.List[string]
    foreach ($rel in (Get-StagedFiles -Stage $stage)) {
        $sums.Add((Get-Sha256 -Path (Join-Path $stage $rel)) + '  ' + $rel)
    }
    Write-Utf8File -Path (Join-Path $stage 'SHA256SUMS.txt') -Text ([string]::Join([string][char]10, $sums.ToArray()))
    Say ("CHECKSUMS: " + $sums.Count + " file(s) listed in SHA256SUMS.txt")

    if ($Mutate -eq 'SumGap') {
        $sumPath = Join-Path $stage 'SHA256SUMS.txt'
        $kept = @()
        foreach ($line in [System.IO.File]::ReadAllText($sumPath).Split([char]10)) {
            if ($line -and ($line -notmatch '  PrismArchive\.exe$')) { $kept += $line }
        }
        [System.IO.File]::WriteAllText($sumPath, ([string]::Join([string][char]10, $kept) + [string][char]10), (New-Object System.Text.UTF8Encoding($false)))
        if ([System.IO.File]::ReadAllText($sumPath) -match '  PrismArchive\.exe') {
            $script:InjectionDetail = 'the PrismArchive.exe line is still listed'
        } else {
            $script:InjectionApplied = $true
            $script:InjectionDetail = 'SHA256SUMS.txt no longer covers PrismArchive.exe'
        }
        if (-not $script:InjectionApplied) {
            Say ("MUTATION SumGap: NOT INJECTED (" + $script:InjectionDetail + ")") 'error'
            return 1
        }
    }

    # ---- 5. zip ----------------------------------------------------------
    $targetDir = $OutDir
    if (-not $targetDir) {
        if ($Mutate) { $targetDir = Join-Path $script:TempRoot 'dist' } else { $targetDir = Join-Path $root 'dist' }
    }
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    $zipPath = Join-Path $targetDir $v.ZipName
    # A stale archive from a previous version would make "which zip is the
    # release" ambiguous (and Test-ReleasePackage refuses to guess).  Artifacts
    # in OutDir are regenerable, so the pipeline owns them.
    foreach ($stale in @(Get-ChildItem -LiteralPath $targetDir -File -Filter 'PrismArchive-*-win-x64.zip' -ErrorAction SilentlyContinue)) {
        if ($stale.FullName -ne $zipPath) {
            Remove-Item -LiteralPath $stale.FullName -Force
            Say ("PRUNE: removed stale " + $stale.Name)
        }
    }
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

    Initialize-ZipApi
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    # ---- 6. verify the bytes that will ship ------------------------------
    $expected = New-Object System.Collections.Generic.List[string]
    foreach ($rel in $payload.Files) { $expected.Add($rel) }
    foreach ($gen in $script:GeneratedPackageFiles) { $expected.Add($gen) }
    $expectedArray = Get-OrdinalSorted $expected.ToArray()

    $result = Test-PackageContents -ZipPath $zipPath -ExpectedFiles $expectedArray -VersionInfo $v

    if ($Mutate) {
        foreach ($e in $result.Errors) { Say ("  " + $e) 'warn' }
        if ($result.Errors.Count -gt 0) {
            Write-Host ("MUTATION " + $Mutate + ": CAUGHT (" + $result.Errors.Count + " assertion(s) went red; injection: " + $script:InjectionDetail + ")")
            return 0
        }
        Say ("MUTATION " + $Mutate + ": NOT CAUGHT -- the package verification is not a guard") 'error'
        return 1
    }

    if ($result.Errors.Count -gt 0) {
        foreach ($e in $result.Errors) { Say $e 'error' }
        Say ("RELEASE PACKAGE: FAIL (" + $result.Errors.Count + " assertion(s) failed)") 'error'
        return 1
    }

    # ---- 7. dist checksum, so the download itself is verifiable ----------
    Write-Utf8File -Path (Join-Path $targetDir 'SHA256SUMS.txt') -Text ($result.ZipSha256 + '  ' + $v.ZipName)

    Say ("PACKAGE: " + $zipPath)
    Say ("SIZE:    " + (Format-Size ((Get-Item -LiteralPath $zipPath).Length)) + " (" + $result.EntryCount + " entries)")
    Say ("SHA256:  " + $result.ZipSha256)
    Say ("DIST:    " + (Join-Path $targetDir 'SHA256SUMS.txt'))
    Say ("ASSERT:  " + $result.Assertions + " assertions, " + $expectedArray.Count + " expected entries, 0 forbidden engine DLLs")
    Say "RELEASE PACKAGE: PASS"
    return 0
}

$exitCode = 1
try {
    $exitCode = Invoke-ReleaseBuild
} catch {
    Write-Host ('FAIL  ' + $_.Exception.Message)
    Write-Host ('FAIL  ' + $_.ScriptStackTrace)
    Write-Host 'RELEASE PACKAGE: FAIL (unhandled error)'
    $exitCode = 1
} finally {
    if ($script:TempRoot -and -not $KeepStaging) {
        Remove-Item -LiteralPath $script:TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($script:TempRoot -and $KeepStaging) {
        Write-Host ("STAGING KEPT: " + $script:TempRoot)
    }
}
exit $exitCode
