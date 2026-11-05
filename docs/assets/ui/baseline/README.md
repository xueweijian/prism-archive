# C108 baseline frame — 1600 x 1000, simple.zip, Daylight

This is the **before** picture the whole visual wave is scored against. It is a
real window, not a mockup: the shipping `PrismArchive.exe` of this commit,
driven by `agent-desktop`, captured with no edits.

| file | what it shows |
|---|---|
| `prism-1600x1000-simple-zip.png` | 1600 x 1000 window (client 1580 x 980) with `simple.zip` open at its root, default Daylight appearance, repository language catalogs |

## What is wrong in this frame (the work list)

Every one of these is a row of [docs/UI-BASELINE.md](../../../UI-BASELINE.md):

- the eight action-island buttons still paint their Alt mnemonic letters
  (O/E/C/A/D/T/V/S) in the top-right corner, and the menu bar paints
  `设置(S)...` -- **9 ASCII letters in a Chinese UI** (C100, C106);
- the **compress** icon is four straight lines through its own centre: the
  centre pixel is ink, not the island surface (C100);
- the Day canvas is #F6F8FC and the floating island #FCFCFE -- **6 levels
  apart**, so the island has no body (C102);
- the sidebar selection is a hard-edged rectangle: its corner pixel is identical
  to its centre (C104);
- the info card carries a full-width 1 px hairline at y = 815 (C105);
- the compression-ratio strip is 6 px at full opacity in the file-kind colour
  (C101) -- it is not visible in this frame because the archive root is a single
  folder, so the matrix measures it from the source constants instead;
- `InitializeOptics` has zero call sites, so the glass chain never runs (C107).

## Sample points used by tools/visual_baseline.ps1

All coordinates are in the PNG (client area), not the window frame.

| what | pixel | value in this frame |
|---|---|---|
| canvas (empty content band) | (1200, 600) | #F6F8FC |
| action island (right pad, no glyph) | (1550, 160) | #FCFCFE |
| compress icon centre (3 x 3) | (981, 158) | #2A2A2C (ink) |
| menu bar plate | (400, 31) | #FFFFFF |
| window caption | (400, 10) | #FFFFFF |
| sidebar selection centre | (100, 260) | #DAECFF |
| sidebar selection inner corner | (25, 243) | #DAECFF (no rounding) |
| info-card hairline | y = 815 | #818186 |

## Reproduce

```pwsh
# prerequisites: the card's build dir configured and the shell built
cmake --preset release-msvc
cmake --build build/release-msvc --parallel 2 --target PrismArchive

$exeDir = 'build/release-msvc/Src/PrismWin'
Copy-Item -Recurse -Force Assets $exeDir            # otherwise A7 (mixed language)
Copy-Item -Force $env:PRISM_7Z_DLL (Join-Path $exeDir '7z.dll')

$fixture = 'E:\Temp\prism-adtest\fixtures\simple.zip'
$p = Start-Process (Join-Path $exeDir 'PrismArchive.exe') $fixture -WorkingDirectory $exeDir -PassThru
Start-Sleep -Seconds 5

# DPI-aware resize to exactly 1600 x 1000, then read the rectangle back
# (SetProcessDpiAwarenessContext(-4); SetWindowPos(hwnd, 0, 10, 10, 1600, 1000, SWP_NOZORDER))
# measured: GetWindowRect = 10,10..1610,1010

agent-desktop list-windows --app PrismArchive.exe
agent-desktop screenshot --window-id w-<hwnd> docs/assets/ui/baseline/prism-1600x1000-simple-zip.png
```

`agent-desktop` captures the CLIENT area, so the PNG is 1580 x 980 (the
window frame is 1600 x 1000: 10 px of border on each side and a 20 px caption).
If the window or the content changes, re-shoot **and** re-derive the sample
points above -- tools/visual_baseline.ps1 reads them literally.

## Measured, not eyeballed

```pwsh
pwsh -File tools/visual_baseline.ps1 -BuildDir <build-dir>
```

prints the whole matrix (16 rows in this revision) with each row's threshold and
owning card. `-Strict` exits non-zero while any row is still FAIL, which is
exactly the "is this card done" question.
