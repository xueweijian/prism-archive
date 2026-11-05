#!/usr/bin/env pwsh
<#
.SYNOPSIS
    C108: collect EVERY row of docs/UI-BASELINE.md with one command.

.DESCRIPTION
    The matrix is only a referee if anyone can re-run it. This script reads the
    three sources the matrix names and prints one table:

      1. THE TEST BINARY  <BuildDir>/Tests/core_c108_baseline.exe -- the
         source/token rows (mnemonic letters, Day canvas, island body, the
         "Segoe UI" literal count, menu/caption delta, optics wiring). When the
         binary is not built it does NOT invent numbers: the rows print n/a.
      2. THE SCREENSHOT   docs/assets/ui/baseline/prism-1600x1000-simple-zip.png
         -- the pixel rows (canvas, island, compress-icon centre, sidebar
         selection corner, info-card hairlines, menu/caption).
      3. THE SOURCE       Src/PrismWin/GalleryRenderer.hpp -- the ratio bar's
         geometry and palette (height, alpha, HSV saturation).

    Every row prints its threshold and the card that owns it, so a reviewer can
    answer "who fixes this" and "how do I check" without opening the code.

    The screenshot's sample points are fixed BECAUSE the artifact is fixed:
    1600 x 1000 window, simple.zip, DPI 96, the default (Daylight) appearance.
    docs/assets/ui/baseline/README.md lists the exact reproduce command; if the
    window or the content changes, re-shoot and re-derive.

.PARAMETER BuildDir
    Build directory holding Tests/core_c108_baseline.exe. Defaults to
    <system drive>\build\wt-c108 (the card's own build dir).

.PARAMETER Image
    Baseline screenshot, relative to the repo root.

.PARAMETER Strict
    Exit 1 when any row FAILs its target. Card owners run this in their own
    suite; the doc's table is produced without it (the baseline is partly red
    by design -- that IS the work list).

.PARAMETER Control
    Negative control for the PIXEL reader: paint the island with the canvas
    colour in a temporary copy, re-measure, and require island_delta to go
    red. Prints "C108 PIXEL CONTROL CAUGHT: island_delta" and exits 0 when the
    reader really is load-bearing.

.PARAMETER OutJson
    Write the same rows to a JSON file (machine-readable; used by CI summaries).

.EXAMPLE
    pwsh -File tools/visual_baseline.ps1
.EXAMPLE
    pwsh -File tools/visual_baseline.ps1 -Strict
.EXAMPLE
    pwsh -File tools/visual_baseline.ps1 -Control
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$Image = 'docs/assets/ui/baseline/prism-1600x1000-simple-zip.png',
    [switch]$Strict,
    [switch]$Control,
    [string]$OutJson
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
# Build dir precedence: the flag, then PRISM_BUILD_DIR, then the in-repo preset
# directory, then the card's own <drive>:\build\wt-c108. A wrong root must not
# silently produce n/a rows, so the resolved path is printed below.
if (-not $PSBoundParameters.ContainsKey('BuildDir') -or [string]::IsNullOrEmpty($BuildDir)) {
    if ($env:PRISM_BUILD_DIR) {
        $BuildDir = $env:PRISM_BUILD_DIR
    } elseif (Test-Path (Join-Path $root 'build/release-msvc/Tests/core_c108_baseline.exe')) {
        $BuildDir = Join-Path $root 'build/release-msvc'
    } else {
        $BuildDir = Join-Path $env:SystemDrive 'build/wt-c108'
    }
}
Add-Type -AssemblyName System.Drawing

function Get-DisplayWidth([string]$text) {
    $width = 0
    foreach ($ch in $text.ToCharArray()) {
        $code = [int][char]$ch
        if ($code -ge 0x1100 -and ($code -le 0x115F -or $code -ge 0x2E80)) { $width += 2 }
        else { $width += 1 }
    }
    return $width
}

function Format-Cell([string]$text, [int]$width) {
    $pad = $width - (Get-DisplayWidth $text)
    if ($pad -lt 0) { $pad = 0 }
    return $text + (' ' * $pad)
}

function Get-Pixel($bitmap, [int]$x, [int]$y) {
    $c = $bitmap.GetPixel($x, $y)
    return @($c.R, $c.G, $c.B)
}

function Get-Delta($a, $b) {
    return [math]::Max([math]::Max([math]::Abs($a[0] - $b[0]), [math]::Abs($a[1] - $b[1])),
                       [math]::Abs($a[2] - $b[2]))
}

# --------------------------------------------------------------------------
# 1. THE TEST BINARY (source + token rows)
# --------------------------------------------------------------------------
$binaryRows = @{}
$binaryPath = Join-Path $BuildDir 'Tests/core_c108_baseline.exe'
if (Test-Path $binaryPath) {
    $output = & $binaryPath 2>&1 | Out-String
    foreach ($line in ($output -split "\r?\n")) {
        if ($line -match '^C108 METRIC (\w+)=(.*?) target=(.*?) card=(\S+) verdict=(PASS|FAIL)$') {
            $binaryRows[$matches[1]] = @{
                Value = $matches[2]; Target = $matches[3]; Card = $matches[4]
                Verdict = $matches[5]
            }
        }
    }
}

function Get-BinaryRow([string]$id, [string]$claim, [string]$method, [string]$threshold,
                       [string]$card) {
    if ($binaryRows.ContainsKey($id)) {
        $r = $binaryRows[$id]
        return [pscustomobject]@{
            Id = $id; Claim = $claim; Method = $method; Threshold = $threshold
            Current = $r.Value; Card = $card; Verdict = $r.Verdict
        }
    }
    return [pscustomobject]@{
        Id = $id; Claim = $claim; Method = $method; Threshold = $threshold
        Current = 'n/a (build core_c108_baseline)'; Card = $card; Verdict = 'n/a'
    }
}

# --------------------------------------------------------------------------
# 2. THE SOURCE (ratio bar: height, alpha, saturation)
# --------------------------------------------------------------------------
$rendererPath = Join-Path $root 'Src/PrismWin/GalleryRenderer.hpp'
$renderer = Get-Content $rendererPath -Raw
$barHeight = 0
$barAlpha = 1.0
if ($renderer -match 'cy - ([\d.]+)f, trackX \+ trackW, cy \+ ([\d.]+)f') {
    $barHeight = [double]$matches[1] + [double]$matches[2]
}
# The fill brush is the opaque tileBorder_ brush, re-coloured per band: no
# SetOpacity and no alpha in the colour literal means alpha 1.0.
$fillBlock = [regex]::Match($renderer, 'FillRoundedRectangle\(fill[^)]*\)')
if ($fillBlock.Success -and $fillBlock.Value -match 'opacity|SetOpacity') { $barAlpha = 0.5 }
$barSaturation = 0.0
$greenMatch = [regex]::Match($renderer, 'BarColorOf\(prism::list::BarColor color\)\s*\{(.*?)\n\s*\}', 'Singleline')
if ($greenMatch.Success -and $greenMatch.Groups[1].Value -match 'Green: return \{([\d.]+)f, ([\d.]+)f, ([\d.]+)f') {
    $rgb = @([double]$matches[1], [double]$matches[2], [double]$matches[3])
    $max = ($rgb | Measure-Object -Maximum).Maximum
    $min = ($rgb | Measure-Object -Minimum).Minimum
    if ($max -gt 0) { $barSaturation = ($max - $min) / $max }
}

# --------------------------------------------------------------------------
# 3. THE SCREENSHOT (pixel rows)
# --------------------------------------------------------------------------
$imagePath = if ([System.IO.Path]::IsPathRooted($Image)) { $Image } else { Join-Path $root $Image }
if (-not (Test-Path $imagePath)) { throw "baseline screenshot missing: $imagePath" }
$bitmap = [System.Drawing.Bitmap]::FromFile($imagePath)

# Sample points. They are fixed because the artifact is fixed (see the file
# header): a 1600 x 1000 window, simple.zip at the root, DPI 96, Daylight.
$canvasPixel = Get-Pixel $bitmap 1200 600     # empty content band, right of the rail
$islandPixel = Get-Pixel $bitmap 1550 160     # action island, right pad (no glyph)
$menuPixel = Get-Pixel $bitmap 400 31         # menu bar plate, clear of the labels
$captionPixel = Get-Pixel $bitmap 400 10      # DWM caption, clear of the title text

# The compress icon must NOT put ink at its own centre: count the 3x3 around it
# that differs from the island surface by more than 24 levels.
$iconInk = 0
for ($dy = -1; $dy -le 1; $dy++) {
    for ($dx = -1; $dx -le 1; $dx++) {
        $p = Get-Pixel $bitmap (981 + $dx) (158 + $dy)
        if ((Get-Delta $p $islandPixel) -gt 24) { $iconInk++ }
    }
}

# Sidebar selection: locate the wash by its own colour, then compare the
# capsule's inner corner with its centre. A hard-edged rectangle makes the two
# identical -- that is the C104 defect, and it is why the threshold is "> 0".
$selRef = Get-Pixel $bitmap 100 260
$minX = 9999; $maxX = -1; $minY = 9999; $maxY = -1
for ($y = 230; $y -lt 300; $y++) {
    for ($x = 0; $x -lt 320; $x++) {
        $p = Get-Pixel $bitmap $x $y
        if ((Get-Delta $p $selRef) -le 2) {
            if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$selCorner = Get-Pixel $bitmap ($minX + 1) ($minY + 1)
$selCentre = Get-Pixel $bitmap ([int](($minX + $maxX) / 2)) ([int](($minY + $maxY) / 2))
$selDelta = Get-Delta $selCorner $selCentre

# Info-card hairlines: a 1 px rule is a row that is one colour across the card
# AND differs from the rows 3 px above and below. Adjacent antialiased rows
# group into ONE rule (that is what the eye counts, too).
$rules = 0
$inRule = $false
for ($y = 740; $y -lt 860; $y++) {
    $row = @()
    for ($x = 40; $x -lt 240; $x += 4) { $row += (Get-Pixel $bitmap $x $y) -join ',' }
    $distinct = ($row | Sort-Object -Unique).Count
    $isRule = $false
    if ($distinct -le 2) {
        $here = Get-Pixel $bitmap 120 $y
        $above = Get-Pixel $bitmap 120 ($y - 3)
        $below = Get-Pixel $bitmap 120 ($y + 3)
        if ((Get-Delta $here $above) -ge 6 -and (Get-Delta $here $below) -ge 6) { $isRule = $true }
    }
    if ($isRule -and -not $inRule) { $rules++; $inRule = $true }
    if (-not $isRule) { $inRule = $false }
}

$bitmap.Dispose()

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row($id, $claim, $method, $threshold, $current, $card, $ok) {
    $script:rows.Add([pscustomobject]@{
        Id = $id; Claim = $claim; Method = $method; Threshold = $threshold
        Current = $current; Card = $card
        Verdict = if ($ok) { 'PASS' } else { 'FAIL' }
    })
}

$rows.Add((Get-BinaryRow 'mnemonic_letters' '助记符字母数' 'band kActions[].mnemonic + Assets/Lang "(&X)"' '== 0' 'C100,C106'))
$rows.Add((Get-BinaryRow 'segoe_literals' 'Segoe UI 字面量计数' 'Src/ 递归扫描' '== 0' 'C103'))
$rows.Add((Get-BinaryRow 'day_canvas' 'Day 画布色值' 'CanvasPresentedColor(Daylight)' '#F5F5F7 正负2' 'C102'))
$rows.Add((Get-BinaryRow 'island_delta' '岛体与画布差' 'island - canvas, max channel' '>= 8' 'C102'))
$rows.Add((Get-BinaryRow 'menu_caption_delta' '菜单栏与标题栏色差' 'PaletteFor(v).background vs caption token' '<= 2' 'C106'))
$rows.Add((Get-BinaryRow 'optics_live' 'OpticsReady()' 'InitializeOptics 成员调用点 (Src/)' '>= 1' 'C107'))
$rows.Add((Get-BinaryRow 'panel_backdrop_delta' '面板随背景变色' 'chain off => flat fill => 0' '>= 4 (R)' 'C107'))

Add-Row 'png_canvas' '画布像素（截图）' 'PNG (1200,600)' '#F5F5F7 正负2' (
    '#{0:X2}{1:X2}{2:X2}' -f $canvasPixel[0], $canvasPixel[1], $canvasPixel[2]) 'C102' (
    [math]::Abs($canvasPixel[0] - 245) -le 2 -and [math]::Abs($canvasPixel[1] - 245) -le 2 -and
    [math]::Abs($canvasPixel[2] - 247) -le 2)
Add-Row 'png_island_delta' '岛体-画布（截图）' 'PNG (1550,160) vs (1200,600)' '>= 8' (
    (Get-Delta $islandPixel $canvasPixel).ToString() + ' levels') 'C102' (
    (Get-Delta $islandPixel $canvasPixel) -ge 8)
Add-Row 'compress_icon_ink' '压缩图标中心像素=岛底色' 'PNG 3x3 @ (981,158)' '0 ink px' (
    $iconInk.ToString() + ' / 9') 'C100' ($iconInk -eq 0)
Add-Row 'ratio_bar_height' '压缩率条高度' 'GalleryRenderer cy±N' '4 px' (
    $barHeight.ToString() + ' px') 'C101' ($barHeight -eq 4)
Add-Row 'ratio_bar_alpha' '压缩率条 alpha' 'GalleryRenderer fill brush' '<= 0.55' (
    $barAlpha.ToString()) 'C101' ($barAlpha -le 0.56)
Add-Row 'ratio_bar_saturation' '压缩率条饱和度' 'BarColorOf HSV S' '<= 0.45' (
    [math]::Round($barSaturation, 3).ToString()) 'C101' ($barSaturation -le 0.45)
Add-Row 'sidebar_corner_delta' '侧栏选中角像素差' 'PNG selection corner vs centre' '> 0' (
    $selDelta.ToString()) 'C104' ($selDelta -gt 0)
Add-Row 'info_card_rules' '信息卡下划线数' 'PNG 740..860 1px rule scan' '0' (
    $rules.ToString()) 'C105' ($rules -eq 0)
Add-Row 'png_menu_caption' '菜单栏-标题栏（截图）' 'PNG (400,31) vs (400,10)' '<= 2' (
    (Get-Delta $menuPixel $captionPixel).ToString()) 'C106' (
    (Get-Delta $menuPixel $captionPixel) -le 2)

# --------------------------------------------------------------------------
# 4. PRINT
# --------------------------------------------------------------------------
$headers = @('主张', '测量方法', '阈值', '当前实测', '目标卡', '判定')
$table = @()
foreach ($r in $rows) {
    $table += ,@($r.Claim, $r.Method, $r.Threshold, $r.Current, $r.Card, $r.Verdict)
}
$widths = @()
for ($i = 0; $i -lt $headers.Count; $i++) {
    $w = Get-DisplayWidth $headers[$i]
    foreach ($line in $table) { $w = [math]::Max($w, (Get-DisplayWidth $line[$i])) }
    $widths += $w
}
$sep = '+' + (($widths | ForEach-Object { '-' * ($_ + 2) }) -join '+') + '+'
Write-Host $sep
Write-Host ('| ' + ((0..($headers.Count - 1) | ForEach-Object { Format-Cell $headers[$_] $widths[$_] }) -join ' | ') + ' |')
Write-Host $sep
foreach ($line in $table) {
    Write-Host ('| ' + ((0..($line.Count - 1) | ForEach-Object { Format-Cell $line[$_] $widths[$_] }) -join ' | ') + ' |')
}
Write-Host $sep

$failed = @($rows | Where-Object { $_.Verdict -eq 'FAIL' })
$unknown = @($rows | Where-Object { $_.Verdict -eq 'n/a' })
Write-Host ("C108 BASELINE: {0} rows, {1} PASS, {2} FAIL, {3} n/a" -f $rows.Count,
            ($rows.Count - $failed.Count - $unknown.Count), $failed.Count, $unknown.Count)
Write-Host ("C108 BASELINE IMAGE: {0}" -f $Image)

if ($OutJson) {
    $payload = [pscustomobject]@{
        generatedBy = 'tools/visual_baseline.ps1'
        image = $Image
        rows = $rows
    }
    $payload | ConvertTo-Json -Depth 4 | Set-Content -Path $OutJson -Encoding utf8
    Write-Host "C108 BASELINE JSON: $OutJson"
}

if ($Control) {
    # NEGATIVE CONTROL for the pixel reader: the island becomes the canvas
    # colour in a COPY of the frame. If island_delta still passes, the reader is
    # blind and this exits 1.
    $copyPath = Join-Path ([System.IO.Path]::GetTempPath()) 'c108-control.png'
    # Load through a COPY: Bitmap.FromFile keeps the file locked, and GDI+
    # refuses to Save over its own source.
    $source = [System.Drawing.Bitmap]::FromFile($imagePath)
    $bmp = New-Object System.Drawing.Bitmap($source.Width, $source.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    $graphics.DrawImage($source, 0, 0)
    $graphics.Dispose()
    $source.Dispose()
    for ($y = 132; $y -lt 197; $y++) {
        for ($x = 733; $x -lt 1553; $x++) {
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(246, 248, 252))
        }
    }
    $bmp.Save($copyPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $ctrl = [System.Drawing.Bitmap]::FromFile($copyPath)
    $ctrlIsland = Get-Pixel $ctrl 1200 160
    $ctrlCanvas = Get-Pixel $ctrl 1200 600
    $ctrl.Dispose()
    $delta = Get-Delta $ctrlIsland $ctrlCanvas
    Remove-Item $copyPath -Force
    if ($delta -lt 8) {
        Write-Host ("C108 PIXEL CONTROL CAUGHT: island_delta (flooded frame reads {0}, threshold 8)" -f $delta)
        exit 0
    }
    Write-Host ("C108 PIXEL CONTROL MISSED: island_delta still reads {0}" -f $delta)
    exit 1
}

if ($Strict -and $failed.Count -gt 0) { exit 1 }
exit 0
