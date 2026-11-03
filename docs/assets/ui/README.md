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
