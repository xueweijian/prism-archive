# PrismArchive -- tools
# C80: the PROCESS-LEVEL accessibility assertion.
#
# WHY THIS EXISTS. The unit suite proves the window answers WM_GETOBJECT with an
# IAccessible tree. This script proves the thing the card was measured with: a
# real UIA client (agent-desktop) sees the client area -- the eight action
# buttons, the tab, the list rows, the sidebar tree and the status line -- and
# can PRESS a button through the accessibility API and observe what the product
# did.
#
# The "before" number is documented here so the assertion has a baseline: the
# shipped window answered 9 elements (title bar + menu bar). The assertion below
# requires at least 20, names three of the eight buttons, the archive's tab and
# a list row, then clicks 搜索 and requires the search FIELD to appear.
#
# Verdict shape (CONTRACTS D1): the stdout marker "C80 UIA CHECKS PASSED" is a
# conjunction with exit 0. A machine without the driver SKIPS (exit 77, mapped
# to SKIP_RETURN_CODE), never a fake green. Failures are printed one per line as
# "FAIL ..." at the moment they are found.
param(
    [string]$Exe = $env:PRISM_C80_EXE
)

$ErrorActionPreference = 'Stop'

$script:failures = New-Object System.Collections.Generic.List[string]

function Add-Failure([string]$text) {
    $script:failures.Add($text) | Out-Null
    Write-Output ("FAIL " + $text)
}

function Exit-Skip([string]$why) {
    Write-Output ("SKIP: " + $why)
    exit 77
}

if (-not $Exe -or -not (Test-Path -LiteralPath $Exe)) {
    Exit-Skip "no executable under test (pass -Exe)"
}
if (-not (Get-Command agent-desktop -ErrorAction SilentlyContinue)) {
    Exit-Skip "agent-desktop (the UIA driver) is not installed on this machine"
}
if (-not (Get-Command 7z.exe -ErrorAction SilentlyContinue)) {
    Exit-Skip "7z.exe is not on PATH (the fixture archive cannot be built)"
}

# ---------------------------------------------------------------- fixture
$temp = Join-Path $env:TEMP ("prism-c80-uia-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$archive = Join-Path $temp 'uia.7z'
$payload = Join-Path $temp 'payload'
New-Item -ItemType Directory -Force -Path $payload | Out-Null
1..12 | ForEach-Object {
    Set-Content -LiteralPath (Join-Path $payload ("item{0:D2}.txt" -f $_)) -Value ("payload " + $_)
}
Push-Location $payload
try {
    & 7z.exe a -t7z $archive '.\*' | Out-Null
    if ($LASTEXITCODE -ne 0) { Exit-Skip "7z.exe could not create the fixture archive" }
} finally {
    Pop-Location
}

function Get-TreeElements($node) {
    $all = New-Object System.Collections.Generic.List[object]
    $stack = New-Object System.Collections.Stack
    $stack.Push($node)
    while ($stack.Count -gt 0) {
        $n = $stack.Pop()
        $all.Add($n)
        if ($n.children) { foreach ($c in $n.children) { $stack.Push($c) } }
    }
    return $all
}

$process = $null
try {
    $process = Start-Process -FilePath $Exe -ArgumentList $archive -PassThru

    # BOUNDED wait (CONTRACTS D29) for OUR window: list-windows is filtered by
    # the pid we started, so a second PrismArchive on the machine cannot be
    # mistaken for this one.
    $windowId = $null
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            Add-Failure ("the app exited before showing a window (exit " + $process.ExitCode + ")")
            break
        }
        $listed = (agent-desktop list-windows --app PrismArchive.exe 2>&1 | Out-String) | ConvertFrom-Json
        $mine = $listed.data | Where-Object { $_.pid -eq $process.Id } | Select-Object -First 1
        if ($mine) { $windowId = $mine.id; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $windowId) { Add-Failure "no PrismArchive window for the launched process within 25 s" }

    if ($windowId) {
        $snapshotText = (agent-desktop snapshot --window-id $windowId -i --compact 2>&1 | Out-String)
        $snapshot = $snapshotText | ConvertFrom-Json
        if (-not $snapshot.ok) { Add-Failure ("snapshot failed: " + $snapshotText) }

        $elements = Get-TreeElements $snapshot.data.tree
        $count = [int]$snapshot.data.ref_count
        Write-Output ("UIA elements=" + $count)

        # THE CARD'S NUMBER. Nine was the defect (title bar + menu bar only).
        if ($count -lt 20) {
            Add-Failure ("element count " + $count + " < 20: the client area is not visible to UIA")
        }

        foreach ($wanted in @('打开', '解压到…', '搜索')) {
            $hit = $elements | Where-Object { $_.role -eq 'button' -and $_.name -eq $wanted } | Select-Object -First 1
            if (-not $hit) { Add-Failure ("button not found by name: " + $wanted) }
        }
        $tab = $elements | Where-Object { $_.role -eq 'tab' -and $_.name -eq 'uia.7z' } | Select-Object -First 1
        if (-not $tab) { Add-Failure "the archive's tab (uia.7z) is not exposed as role=tab" }
        $rows = @($elements | Where-Object { $_.role -eq 'option' })
        if ($rows.Count -lt 12) { Add-Failure ("list rows exposed=" + $rows.Count + " < 12") }
        $row = $rows | Where-Object { $_.name -eq 'item01.txt' } | Select-Object -First 1
        if (-not $row) { Add-Failure "list row item01.txt not found by name" }
        $outline = $elements | Where-Object { $_.role -eq 'outline' } | Select-Object -First 1
        if (-not $outline) { Add-Failure "the sidebar tree is not exposed as role=outline" }
        $status = $elements | Where-Object { $_.role -eq 'status' } | Select-Object -First 1
        if (-not $status) { Add-Failure "the status line is not exposed as role=status" }

        # PRESS a button through the accessibility API and require the PRODUCT
        # to change: InvokePattern (MSAA accDoDefaultAction) opens the search
        # field, which then appears as a text field in the tree.
        $searchButton = $elements | Where-Object { $_.role -eq 'button' -and $_.name -eq '搜索' } | Select-Object -First 1
        if ($searchButton) {
            $click = (agent-desktop click $searchButton.ref_id --timeout-ms 4000 2>&1 | Out-String)
            $clicked = $click | ConvertFrom-Json
            if (-not $clicked.ok) { Add-Failure ("click on 搜索 failed: " + $click) }
            Start-Sleep -Milliseconds 500
            $after = ((agent-desktop snapshot --window-id $windowId -i --compact 2>&1 | Out-String)) | ConvertFrom-Json
            $afterElements = Get-TreeElements $after.data.tree
            $field = $afterElements | Where-Object { $_.role -eq 'textfield' -and $_.name -eq '搜索框' } | Select-Object -First 1
            if (-not $field) { Add-Failure "the search field did not appear after pressing 搜索 through UIA" }
        }
    }
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures.Count -gt 0) {
    Write-Output ("C80 UIA CHECKS FAILED (" + $script:failures.Count + " failures)")
    exit 1
}
Write-Output "C80 UIA CHECKS PASSED"
exit 0
