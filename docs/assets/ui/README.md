# 动作岛按钮 before/after（C78）

这八张图由仓库自己的渲染测试从**真实 Direct2D 软件渲染目标**输出（不是设计稿、
不是截图拼接）。每张裁的是 1280 × 800 窗口的 y = 20..120 条带：上面 12 px 是标题
栏底边，中间 56 px 是动作岛，下面留一点内容背景，方便看岛缘与按钮的相对位置。

| 文件 | 状态 |
|---|---|
| `c78-before-rest.png` / `c78-after-rest.png` | 常态（无 hover） |
| `c78-before-hover.png` / `c78-after-hover.png` | 悬停「解压到…」（有下拉的动作） |
| `c78-before-pressed.png` / `c78-after-pressed.png` | 按下「解压到…」 |
| `c78-before-focus.png` / `c78-after-focus.png` | 键盘焦点环（Alt / Tab 导航） |

before 来自分支起点的代码（`git stash` 后重新构建），after 来自本卡最终代码。
两边使用同一场景、同一 1280 × 800、同一 Midnight 主题。

## 复现

```pwsh
. E:\wg\env.ps1 -Card c78
cmake --build E:\build\wt-c78 --target win_c78_action_island
E:\build\wt-c78\Tests\win_c78_action_island.exe <out-dir>
```

不给 `<out-dir>` 时测试只跑断言、不写文件（CI 走的就是这条路径）。
`win.c78_action_island` 同时断言：hover 提亮落在 3–8、按压时图标与文字的
墨迹质心各下移 1.0 px、三条分组分隔线恢复出的 alpha 在 0.06–0.10、禁用标签
对比度 < 3:1、焦点环颜色与强调色差 < 6、下拉箭头只在一个被悬停的菜单动作上出现。
# C79 chrome evidence -- sidebar / tab bar / status bar

These PNGs are rendered by the REAL renderer (`Tests/win/gallery_snapshot.cpp` ->
`GalleryRenderer` -> a headless WIC target). They are not mockups: the same code
path paints the shipping window.

## Images

| file | what it shows |
| --- | --- |
| `before/prism_chrome.png` | origin/main chrome: 20 px tab plates, close x on every tab, 32% selection wash, 11 px four-segment status bar |
| `after/prism_chrome.png` | C79: 24 px raised active plate + 2 px accent rule, close x only on the active/hovered tab, 14% selection wash + 3 px accent bar, two-cluster 12 px status bar |
| `before/prism_sidebar.png`, `after/prism_sidebar.png` | the C29 sidebar frame (selection, headings, capsule) |
| `before/prism_list.png`, `after/prism_list.png` | list mode with the status band |
| `after/prism_chrome_hover.png` | pointer over tab 1: its close x appears, tab 2 stays text-only |
| `after/prism_chrome_daylight.png`, `after/prism_chrome_highcontrast.png` | the same chrome in the other two theme variants (all C79 tokens have a row in each) |
| `c79-chrome-before-after.png` | full-frame before/after |
| `c79-chrome-details-before-after.png` | tab bar / sidebar selection / status bar, before vs after |

The Daylight frame's backdrop still comes from the hardcoded dark band fill
(`GalleryRenderer::kBackdrop`); making the background layer theme-aware is C77's
card, not this one. The C79 surfaces themselves (panel, selection, tabs, status
divider) resolve the Daylight tokens correctly.

## Pixel assertions (all measured on the rendered frame)

- **Tab brightness**: at the same coordinate of two plates in one frame,
  `active=61`, `inactive=19`, delta `42 >= 6`
  (`Tests/win/test_tab_bar.cpp`, and the same probe in the snapshot).
- **Resting close button**: the inactive tab's close-box centre is
  `FF101318`, bit-identical to an empty point in the same tab -> the x is not
  painted (`test_tab_bar.cpp`).
- **Active tab rule**: exactly `2 px` of opaque accent at the bottom edge; the
  top edge is a specular highlight (`topEdge=364 > plate=185`) with an ambient
  shadow below (`shadowBelow=35 < canvas=58`).
- **Sidebar selection rule**: `3 px` accent at the row's left edge, in
  Midnight, Daylight and HighContrast (the run is measured, not inferred).
- **Status bar clusters**: `leftEnd=444`, `rightStart=1232`, gap `788 px =
  49.2% >= 40%`, and `0` ink pixels in the empty middle except the 1 px
  `--divider-subtle` rule.
- **Three-theme tokens**: every C79 token
  (`--selection-tint`, `--surface-tab-active`, `--surface-tab-hover`,
  `--divider-subtle`, `--shadow-ambient`) resolves with
  `known && !fellBack` in Daylight, Midnight and HighContrast.

## Reproduce

```powershell
. E:\wg\env.ps1 -Card c79
Build-Only -Target gallery_snapshot
ctest --test-dir E:\build\wt-c79 -R "win.gallery_snapshot|c60.tab_bar|win.sidebar_view|win.visual_polish" --output-on-failure
& E:\build\wt-c79\Tests\gallery_snapshot.exe docs\assets\ui\after
```

The `before/` set was produced from the same test scene with the five production
headers stashed back to `origin/main`, so the two sets differ only by this card.

# C83 shell evidence -- dark title bar + dark menu bar

These two PNGs are window screenshots taken with `agent-desktop screenshot` against
the SHIPPED exe staged with the repo's `Assets/Lang` catalogs and `7z.dll` (the
build-dir exe has no catalogs, which is the A7 mixed-language trap, not a shell
regression). Same machine, same fixture (`simple.7z`), same 1920x600 window.

| file | what it shows |
| --- | --- |
| `c83-shell-before.png` | origin/main: white system caption + light Win32 menu bar over the #0B0E13 canvas (UI-AUDIT A4) |
| `c83-shell-after.png` | C83: caption, menu bar and canvas are one surface; the four items are still 文件 / 编辑 / 设置(S)... / Help |

## Pixel assertions (measured, not eyeballed)

- **Caption vs canvas**: canvas luma `12.86`, caption luma `12.08` (delta `-0.78`,
  rule is `< canvas + 12`). DWM record: `attribute=20, caption=1, border=1`.
- **Menu plate**: modal colour `#0B0C10` == `--surface-backdrop-reference`
  (Midnight), each channel within 3.
- **Dropdown row**: `WM_DRAWITEM` driven with the menu's own item data paints
  `#0B1F36` == `--selection-tint` over the plate.
- **Contrast**: item text `16.83 / 17.95 / 21.00` (Day / Night / High contrast),
  selected row `14.03 / 15.24 / 14.45` -- all `>= 4.5:1`.
- **Negative controls**: no DWM call -> caption luma `243.30` (red);
  `MF_STRING` bar -> plate `#FFFFFF` (red); system-light palette -> white on
  white (red).

## Reproduce

```pwsh
. E:\wg\env.ps1 -Card c83
cmake --build E:\build\wt-c83 --parallel 2 --target win_c83_chrome_test test_chrome_theme
ctest --test-dir E:\build\wt-c83 -R "c83" --output-on-failure
```

# C90 list backdrop evidence -- the white plate over the rows

Two frames of the SAME scene (1920 x 1080, list mode, 14 rows, Midnight, rail
shown), rendered by the REAL renderer (`Tests/win/test_c90_backdrop_text.cpp`
-> `GalleryRenderer` -> a headless WIC target). They differ by exactly one
thing: whether the C77 refraction source contains the row GLYPHS.

| file | what it shows |
| --- | --- |
| `c90-list-backdrop-before.png` | shipped code: the name column's 13 px labels are rasterized into the 1/8 backdrop, fused by the 1.75 mini px blur and returned at 35% -- the white rectangles the user circled. The 原始大小 / 压缩后 / 压缩率 columns smear the same way. |
| `c90-list-backdrop-after.png` | C90: glyphs are out of the refraction source; the folder icon and the ratio capsules stay, so the layer still refracts something (the green haloes around the capsules). |

## Pixel assertions (measured on the rendered frame)

- **Name band** (the audit's x 400..1400, rows band y 136..584), over the pixels
  that are canvas in the backdrop-OFF reference: `onMaxLum = 88` before,
  `13` after, canvas `14`, limit `canvas + 24 = 38`. Plate area
  `55 663 px` -> `0 px`.
- **Positive control** (ratio column x 1730..1830): `23 270` pixels still differ
  by `>= 8` levels (max channel delta `58`), i.e. the layer was not switched
  off along with the glyphs.
- **Negative control**: `PRISM_C77_MUTANT_TEXT_IN_BACKDROP` restores the shipped
  `drawGlyphs`; its frame is byte-identical to
  `c90-list-backdrop-before.png` (same sha256) and it prints
  `C77 MUTATION CAUGHT: text-in-backdrop`.
- **Rebuild policy**: one backdrop build per content change, zero on a scroll.

## Reproduce

```pwsh
. E:\wg\env.ps1 -Card c90
cmake --build E:\build\wt-c90 --target win_c90_backdrop_text
ctest --test-dir E:\build\wt-c90 -R "c90" --output-on-failure
& E:\build\wt-c90\Tests\win_c90_backdrop_text.exe <out-dir>
```

No `<out-dir>` = assertions only, no files (the CI path).

## C90 follow-up -- the band that survived the text fix (the selection pill)

The user's second red circle: ONE file open, the first row selected, and a
full-width light band around that row -- wider than the row, still there with
the pointer parked elsewhere. Same scene as above (1920 x 1080, list mode, 14
rows, Midnight), selection ON (`scene.selectedRow = 0`, which is what the
window does after opening an archive).

| file | what it shows |
| --- | --- |
| `c90-list-band-before.png` | the 32 px accent pill is 4 mini px of solid accent in the 1/8 source; blurred, it becomes a band that reaches ~40 px above and below the row. Its `BackdropKey` never carried `selectedRow`, so it also survived the selection moving on. |
| `c90-list-band-after.png` | the pill is STATE, not content: out of the refraction source (same rule Draw() states for hover/focus pills). The row keeps its crisp pill; the ghost keeps the icons and the ratio capsules. |

Measured at the same coordinate (x 400, 8 px below the selected row -- canvas in
the backdrop-OFF frame), `maxChannelDelta`:

```
shipped / mutant : 74   (the band)
this change      :  2   (rounding on the theme's presented canvas; limit is 4)
```

The scan is the reporter's: x 400..1400 every 100 px, every y of the rows band,
4531 reference-canvas probes: `violations = 512 -> 0`, `worst = 74 -> 2`. It
runs in BOTH states (no selection: 4839 probes, same 0 violations), so the
assertion is a property of the layer and not of one selection.

**The baseline is the theme's presented canvas, not the layer-OFF frame** (C91's
`prism::chrome::CanvasPresentedColor`: the veil token composited over the
canvas). The veil's own wash is 5 levels on the night canvas's blue channel --
(16,17,23) -> (13,14,18) -- so a "within 4 of the OFF frame" rule counted every
empty pixel as a violation and turned the Windows lane red with no band in the
frame at all (PR #114). Against the canvas the pixel should be showing, the same
scan reads 2 with the fix and 74 with the pill back in the source.

`PRISM_C77_MUTANT_STATE_IN_BACKDROP` restores the pill in the 1/8 source and
prints `C77 MUTATION CAUGHT: state-in-backdrop` (unselected: 0 violations,
selected: 512).


