# 晨间报告（2026-11-07）

> 你睡觉期间我按 TDD 推进：**先写会红的测试 → 实现 → 配变异臂**，全部以 GitHub CI 为判定。

## 1. 已合并的卡（main）

| 卡 | 内容 | 关键证据 |
|---|---|---|
| C100 | 图标曲线化 + 删助记符字母 | 助记符 glyphs=0；压缩图标中心像素距离=0（不再是大叉）；`GetStartCap=ROUND` |
| C101 | 压缩率条静音化 | 高 6→4、alpha 1.0→0.55、宽 66→56、饱和度 0.739→0.31 |
| C102 | Day 分层材质 | Day 画布 #F5F5F7；岛体与画布差 5→8 级；Night/HC 零改动（帧级 byte-identical） |
| C103 | 字体栈统一 | `Src/` 里 `Segoe UI` 字面量 21→0；实际解析 latin=Segoe UI Variable Display / cjk=Microsoft YaHei UI；换字体前后 6,677 像素不同（全在字形） |
| C104 | 侧栏 macOS 化 | 胶囊角像素差 30.7（方形臂 0.0）；文件夹离实心板 70.3 |
| C105 | 信息卡四组两行 + 状态栏去竖线 | 卡片内横线 0、标签 10px/值 12px、段间隙 24px |
| C106 | 菜单栏融合 + Alt 隐藏 | 三主题 caption==menu；Alt 使内容上移 25px |
| C107 | Liquid Glass 光学链接通 | `OpticsReady=true`；面板 vs 平涂 16.24 级；背景改红后板心 dR=111 |
| C108 | 视觉基线 + 验收矩阵 | 13 条主张、采集脚本、基准帧 |
| C110 | 网格倒影不吃图元 + 图块底色跟随主题 | 图块区折射差 34.56→0.70；Day 图块 1.86→12.33 级 |
| C111 | 图块文件夹图标重绘 | 正面宽高比 1.511→1.360；顶边高光 -6.14→+18.46；随 cell 缩放误差 28%→0.88% |
| C90 | 折射不吃选中行药丸 | 判据基线改为 `CanvasPresentedColor`，容差仍 4 |
| C93 | 面包屑可点 + 树根回根 | 三条断言 + 三条变异臂 |
| C92 / C94 | 双击+右键菜单 / 覆盖冲突一次问完 | `CS_DBLCLKS=1`；14 个冲突只弹 1 次 |

## 2. 你报的三个问题，现在各自的状态

### ① 两处白色模糊底色 → 已消除

根因分三层，都修了：
1. **行文字进 1/8 倒影**（C90）：13px 字在 1/8 图里只剩 1.6px，光栅化成实心板 → 白块。
2. **选中行药丸进倒影**（C90 跟进）：26% accent 在深色画布上是 3 倍亮度，融成横贯全行的亮带。
3. **网格图块进倒影**（C110）：图块 6% 白填充在 1/8 里变成 27px 实心块 → 灰涂抹。

实测：`px(480,400)` 从 46 降到 **12,13,17**（画布本色）。

### ② 主题只改了一部分 → 已全窗生效

根因：`Scene` 组好后 `sidebarTheme/actionTheme/tabTheme/glassTheme` 一个都没赋值，全走默认 Midnight。
现在 `Scene::theme` 单一入口，day→night 内容带像素变化 **0% → 99.91%**。

### ③ 和 Bandizip 的交互差异 → 四条 P0 全部落地

| 差异 | 修复 |
|---|---|
| 双击无反应 | `CS_DBLCLKS` + 双击语义（文件夹进入 / 文件打开） |
| 没有右键菜单 | 三套菜单（文件行/空白/树），带禁用理由与 Shift+F10 |
| 回不到归档根 | 面包屑每段可点 + 树根点击回根 |
| 覆盖逐文件弹窗 | 一次收集：全部覆盖/跳过/重命名 + 本操作记住 |

## 3. 你分析的 UI 五条，逐条落地情况

| 你的判断 | 落地 |
|---|---|
| 单字母牛皮癣 | 已删（Alt 快捷键不依赖绘制，已用 `PressMnemonic` 验证） |
| 简笔画图标、压缩画成大叉 | 已改曲线；压缩=打包箱+束带 |
| 浅色模式惨白无层次 | Day #F5F5F7 + 图块/岛体都有身体 |
| 字体全量硬编码 Segoe UI | 21 处清零，中文走 Microsoft YaHei UI fallback |
| 压缩率高饱和粗条 | 4px / alpha 0.55 / 短条 / 饱和度 0.31 |

## 4. 我踩的坑（都记进契约/CI 文档了）

| 坑 | 后果 | 已固化的规则 |
|---|---|---|
| 构建目录绑到了别人的工作树 | 我据此下了**错误的 P0 结论**并派单 | **D36**：相信产物前核对 `build.ninja` 的源码根 |
| 手工解 rebase 冲突只看"标记清零" | 推上去的 HEAD 编译不过（`right undeclared`） | 解冲突后**至少跑一次 `clang++ -fsyntax-only`** |
| 巡检没写跳过表 | 误合了 C107（会把面包屑文字糊掉） | 意图必须写进脚本，**机器只认代码** |
| 两次把用户文件 stash 走 | 已恢复 | 在**正确的工作树**里操作，先 `pwd` 再动手 |

## 5. 还在跑

- **C112**：让 island 拥有 pill 文字（修 C107 的"玻璃盖住自己的背景文字"），核心断言是**文字锐度保留率 ≥90%**。
- **C109**：列表排版（等宽数字 + 36px 行高），探针误报已修。
- **C105 比例条**：条移到值下方，不相交断言 + 变异臂。

## 6. 已知未做（不粉饰）

1. **`Help` 菜单在中文下仍是英文** —— 修复在分支上（`feat/c93b-help-label`），等合并。
2. **Liquid Glass 目前只覆盖面包屑岛**，动作岛/信息卡还没接。
3. **每帧 GDI 抓背景的开销没测**（C107 遗留），FPS 若有问题要回头。
4. 启动耗时、最小化内存这两项今晚没碰。

## 7. 【重要】C113 发现：一个"已合并卡"的结论是建立在空帧上的

这是今晚最严重的发现，比任何 UI bug 都严重：

> **C112 把 C76 的像素基线重录到了一张被 D2D 拒绝的空帧上。**

实证链（C113 在本机跑出来的）：

```
bench_c76_startup:  frame1=frame2=0af0b75bc5b12383, painted=0/936624, maxChannel=18
C113 H1 复现:       RenderSceneInto 失败, LastRenderError=0x88990012, 该帧 digest = 0af0b75bc5b12383
```

三个连锁事实：

1. **`DrawFrame` 丢弃 `EndDraw` 的 HRESULT 并 `return true`** → `if (!RenderSceneInto(...))` 是死代码，
   `frame.rendered` 恒为 true，**失败帧被当成成功帧**。
2. **同一窗口的第二次离屏渲染也会被拒**（`0x88990012`）→ C87 一窗口渲 8 帧，其中 **7 帧读到空帧**，而 C87 一直是绿的。
3. **`ci.yml` 的 `PRISM_CTEST_EXCLUDE="bench|fuzz"` 让 `c76.bench_startup` 从未在 CI 跑过** —— 这条像素门在 CI 里一直是死的。

### 这意味着什么

- C112 的 **"digest 重录"依据是错的**：不是"pill 搬到 island"，而是整帧（文字/缩略图/侧栏/动作栏）都没画进去。
- C112 的 **S1/S2 锐度断言**若走的是 `PresentGlassFrame` 产品路径，仍然有效；但**任何跨帧像素比较都必须重做**。
- 我已把 #129 按住（它在 CI 上也红了：`win.c112.glass_owns_text`），等 C113 合并后让它 rebase 重做。

### 有效的观测量（C113 提供，已要求写进 docs/CI.md）

> 断言"不是空帧"必须用 **`painted`（最亮通道 ≥ 32）** 或 digest，
> **不能用"非黑像素数"** —— 被拒帧每像素 `B=19,G=14,R=11`，比黑还亮。

### 修复（PR #131）

- 新增 `RenderSceneForCapture`（绕开设备域错误），`DrawFrame` 传播 HRESULT；
- C87 改用它 → 55 checks / 0 failures；
- C76 digest 重录为**真实帧** `e735eb87a09d4f84`（三次稳定）；
- 新套件 `c113.render_honesty`（名字不含 `bench`，所以 **CI 真的会跑它**）+ 两个变异臂。

### 我要求它补的一条

把 C76 的像素结论钉进一个 **CI 可见**的测试（`c113.render_digest`），断言 `painted ≥ 阈值` 且 digest 等于真实帧值；
变异臂：把 `DrawFrame` 改回丢弃 HRESULT → 该测试必须红。**这样这条门才真正活起来。**

## 8. 今晚合并清单（截至 04:45）

| # | 卡 | 一句话 |
|---|---|---|
| 1 | C100 | 图标曲线化 + 删助记符字母 |
| 2 | C101 | 压缩率条静音化（4px / alpha .55 / 短条） |
| 3 | C102 | Day 分层材质（画布 #F5F5F7、岛体 8 级差） |
| 4 | C103 | 字体栈统一（21 处字面量清零） |
| 5 | C104 | 侧栏 macOS 化（胶囊选中 + 柔和文件夹） |
| 6 | C105 | 信息卡四组两行 + 状态栏去竖线 + 比例条归位 |
| 7 | C106 | 菜单栏融合 + Alt 隐藏 |
| 8 | C107 | Liquid Glass 光学链接通（后被 C112 修正归属） |
| 9 | C108 | 视觉基线 + 验收矩阵 |
| 10 | C110 | 网格倒影不吃图元 + 图块底色跟随主题 |
| 11 | C111 | 图块文件夹图标重绘（贝塞尔 squircle） |
| 12 | C90 | 折射不吃选中行药丸 |
| 13 | C93 | 面包屑可点 + 树根回根 |
| 14 | C92 | 双击 + 右键菜单 |
| 15 | C94 | 覆盖冲突一次问完 |
| 16 | C109 | 列表排版（等宽数字 + 36px 行高） |
| 17 | C113 | 基准诚实性（帧真的画出来了才允许比 digest） |
| 18 | C114 | pool_lease 从"调度运气"改为确定性断言 |

## 9. 三张卡改变了这个项目的可信度（比任何 UI 改进都重要）

| 卡 | 它证明了什么 |
|---|---|
| **C113** | 一个已合并卡（C112）把基线重录在**被 D2D 拒绝的空帧**上；`DrawFrame` 一直在丢弃 `EndDraw` 的 HRESULT；`bench` 被 CI 排除所以从没跑过 |
| **C114** | 一条断言（`distinctSlotsUsed == capacity`）测的是**调度器行为**，不是契约；它挡了两次合并 |
| **C110** | 我建议"只改采样带"，它用实测否证（gap 上未改代码也只有 2.0 级），改成量**阴影有效 alpha**（0.109 → 0.106，与背景解耦） |

## 10. 我的错误清单（今晚共 6 次，都记下来）

| # | 错误 | 教训 |
|---|---|---|
| 1 | 相信 `E:\build\prism` 的输出，没核对它绑哪个树 | **D36**：先看 `build.ninja` 的源码根 |
| 2 | 解 rebase 冲突后只确认"标记清零"就推 | 至少跑一次 `clang++ -fsyntax-only` |
| 3 | 把两个探针函数嵌套（`C2601`） | 同上 |
| 4 | `git add -A` 把 16 张快照 png 扫进提交 | 提交前看 `git show --stat` |
| 5 | 套件文件漏 `if(WIN32 ...)` → Linux 撞 `windows.h` | **每条 lane 都要想一遍** |
| 6 | 用 `git grep origin/main` 判断"某测试不存在"，而它在未合并的分支上 | 查之前先问"它在哪个 ref 上" |

## 11. 还剩什么（不粉饰）

1. **#129 C112** 待合并：island 拥有文字的修法是对的，但它必须先把 `bench_c76_startup` 与 `c113.render_digest`
   **两个**常数一起更新到 C112+C113 树的值，并写清"基线为什么正当移动"。
2. **C115 窄窗口布局**：1028 px 下动作岛与面包屑重叠 144 px（已实测），卡在跑。
3. **Liquid Glass 只覆盖面包屑岛**，动作岛/信息卡还没接。
4. **每帧 GDI 抓背景的开销没测**（C107 遗留）。
5. 启动耗时、最小化内存两项今晚没碰。
## 12. 【未解决】装好的程序打开归档后内容区全白

我在 04:53 把 main 装到本机后截图，发现窗口**只有标题栏和菜单栏**，内容区是 **Day 画布色 #F5F5F7**。

### 已排除（每条都有证据）

| 假设 | 排除方式 |
|---|---|
| 渲染器坏了 | 同一棵树跑 headless `gallery_snapshot` → `ALL SNAPSHOT CHECKS PASSED`，产出的 PNG 有完整网格/列表/状态栏 |
| C107 玻璃层盖住 | 回退 C107 重建 → 窗口截图像素**字节完全相同**（sha256 `f6247999…`） |
| 截图路径假象 | `--screen 0` 抓**合成后的桌面**：VS Code / 任务栏 / 桌面图标全正常，**只有该窗口矩形是白的** |
| 合成岛铺满窗口 | C115 三条证明：首帧 `NavPillRect` 为空 → `GlassPanelRect` 0x0 → `PresentGlassFrame` 在 `ResizeIsland` 前 return；panel 恒等于 pill；11 个宽度实测 panel ≠ client |
| 数据没加载 | 无障碍树列出全部控件（动作按钮、tab、listbox 的 `docs`/`readme.txt`、树、状态栏） |
| 强制重绘能救 | `ShowWindow(SW_RESTORE)` + `MoveWindow` + `RedrawWindow(INVALIDATE\|UPDATENOW\|ALLCHILDREN)` 后全屏截图**仍然白** |

### 白屏的形态

- 白是 **Day 画布色 #F5F5F7**，不是纯白；
- **菜单栏画出来了**（同一个 D2D 目标）⇒ `Clear` 和部分绘制执行了；
- **内容带（侧栏/列表/动作岛/状态栏）一个都没画**；
- 无障碍树完整 ⇒ **模型有数据，Scene 没有**（或内容带的矩形为空）。

### 当前假设（C116 在验证）

窗口在**扫描完成之前**画了首帧（`scene.items == nullptr`），之后**没有重建 Scene / 没有重绘**；
强制重绘也画空帧 ⇒ 重绘读到的 Scene 仍是空的，或内容带矩形高度为 0。

### 一个必须记住的验证陷阱

`agent-desktop screenshot --window-id` 对 **DirectComposition 合成的窗口会返回空的重定向表面**。
本仓库 `GlassIslandHost.hpp:971-990` 早就记了这件事（GDI 拿到 blank，要 `PrintWindow(PW_RENDERFULLCONTENT)`）。
**验证手段本身也要被验证**：判断 UI 好坏必须用**桌面截图**（`--screen 0`）或 headless 渲染，不能用窗口截图。
## 13. 一个还没查完的隐患：还有测试可能在读"空帧"

C113 查实：窗口真实绘制过之后再 `RenderSceneInto(WIC)`，D2D 会以 `0x88990012` **整帧拒绝**，
而 `DrawFrame` 曾经吞掉 `EndDraw` 的 HRESULT → 测试拿到的是"只有画布底色"的帧。

C113 修了 `bench_c76_startup` 与 `test_c87_hud_lifecycle`（改用 `RenderSceneForCapture`），
但我核到 **`Tests/win/test_c74_i18n_switch.cpp` 仍有自己的 `RenderWindow` helper**，
没有看到它走 capture seam。如果它也读的是被拒帧，那它的"英文/中文像素不同"断言可能是**空帧对空帧**。

**审计结果（2026-11-07 05:55）**：全仓 `Tests/` 里引用 `RenderSceneInto` 的只有 3 个文件
（`BenchFrameCapture.hpp`、`bench_c76_startup.cpp`、`c113-bench-honesty.cmake`），都已被 C113 处理。
`test_c74_i18n_switch.cpp` 有自己的 `RenderWindow`，但它是**自建 `GalleryRenderer` + 在 WIC target 上 `CreateDeviceResources`**，
独立实例、不跨设备，**没有空帧风险**。所以这个隐患的范围是封闭的。

**仍待办（下一张卡，非阻塞）**：
1. 全仓扫 `RenderSceneInto` 在 `Tests/` 里的使用点；
2. 每个使用点加**前置断言** `rendered == true && hr == S_OK && painted >= 5%`；
3. 任何读空帧的测试，其历史结论都要重测（像 C76 那样列"作废/待重测"清单）。

## 14. 白屏结案：**是这台机器的 D2D present 失效，不是应用**

C116 用证据推翻了我上面的假设（第 12 节），三条：

1. **首帧不在数据之前**：真实 exe + `PRISM_C76_TRACE=1` 的 trace 里 `set_items=265.2ms`、首次 `WM_PAINT=373.7ms`，
   首帧内容带非空（`band width=1184 buttons=8`，`renderer_draw 21.97ms`），`EndDraw=S_OK`。
2. **零依赖探针复现**：100 行、零 PrismArchive 代码（`CreateHwndRenderTarget → Clear(红) → EndDraw`），
   连做 7 次 `EndDraw` 都 `S_OK`，窗口仍白；硬件/软件目标、两种 present、DPI-unaware/PMv2 **全白**。
3. **同一窗口 GDI `FillRect` 红块可见**（截图 + `GetPixel=ff0000`）。

环境：`DwmIsCompositionEnabled=1`、窗口 `ex=0x110`（非 layered/非 NOREDIRECTIONBITMAP）、`cloaked=0`；
本机 DXGI 有 **Intel UHD 620 / Radeon 530 / MS Basic Render Driver / OrayIddDriver** 四个适配器——
怀疑与虚拟显示器/混合显卡有关。

### 这条发现的方法论价值

> **"用全屏截图判断画没画出来"在本机不可靠**：DComp 药丸能截到，D2D HWND present 完全截不到。

也就是说：**用户看到的白屏与截图白屏无法区分**。我先前的"桌面截图可见其真实颜色"这条推理，前提不成立。

### 处置

- C116 交付了真实的产品改进（`Invalidate()` 成为具名重绘门，`RebuildFolderView` 经它请求帧）+ 测试 + 变异臂；
- 我要求它把零依赖探针落成 `tools/d2d_present_probe`，并在 `docs/TROUBLESHOOTING.md` 加一节"窗口全白"：
  `verdict=D2D_PRESENT_BROKEN` ⇒ 指向显示驱动/虚拟显示器，不是应用；
- **整窗改走 DirectComposition** 作为独立卡评估（量级大，CI 无法验证差异）。

### 给用户的建议

若在本机看到窗口全白：**先重启一次**（清掉虚拟显示器/驱动状态），或拔掉远程桌面/投屏会话；
再用 `tools/d2d_present_probe` 判定。应用本身在 headless 渲染与无障碍树上都是完整的。