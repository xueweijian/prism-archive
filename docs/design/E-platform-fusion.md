# E · Windows 11 平台融合：「原生但更好」

> 题目范围：**平台约定、Fluent 2 材质、原生控件取舍、每帧预算、自绘风险**。
> 视觉工艺（对齐/对比/留白）见 C 文档，迁移心理见 B 文档，Bandizip 拆解见 A 文档；本文不重复。
> 方法：读实现 + 在真实进程/注册表上取数 + 像素采样。**所有数字都能复现，命令在第 9 节。**
> 纪律：没有改任何代码、没有提交。结论只来自跑出来的东西，不来自注释。

---

## 0. 一页结论（每条都带数字）

| # | 结论 | 证据 |
|---|---|---|
| 1 | **进程是 DPI-unaware（0）**，在你这台 125% 的机器上整窗被系统位图放大 1.25×。这是"字发糊"的第一根因，不是字体、不是 D2D。 | `GetProcessDpiAwareness = 0`（见 1.1 节）；exe 内嵌 manifest 只有 `asInvoker`，无 `dpiAware`/`dpiAwareness`/`supportedOS` |
| 2 | **"跟随系统"不存在**：`ShellVariantFor("system") == Midnight` 被单元测试钉死；而你的 Windows 是**浅色**（`AppsUseLightTheme=1`）。结果是"系统白、我们黑"。 | `ChromeTheme.hpp:239`、`Tests/test_chrome_theme.cpp:168`、注册表 |
| 3 | **系统关闭了透明效果**（`EnableTransparency=0`）。任何以 Mica/Acrylic 为唯一层级手段的方案，在这台机器上会退化成实色——必须带 solid fallback。 | 注册表 |
| 4 | **自绘 UI 的平台缺口是成片的**：没有滚动条、列表/树没有焦点环、没有 `NotifyWinEvent`、没有加速键表、没有 `WM_SETTINGCHANGE`、IME 不定位候选框。 | 第 2 节逐条给 grep 结果 |
| 5 | **"每帧抓背景折射"在我们的代码里其实不是每帧**（有 `BackdropKey` 缓存），但它仍然把**条目像素**画进了倒影，这才是彩色光晕的根因：间隙带 chroma 均值 **38.2** vs 关闭折射 **7.0**。 | `GalleryRenderer.hpp:538-576`；9.5 节实测 |
| 6 | 平台层最硬的一条：**应用拿不到桌面像素**（DWM 独立进程合成）。这不是性能取舍，是能力边界，ADR-001 已经写死。 | `docs/ADR.md:24-36` |

**一句话**：现在的界面有"漂亮的意图"，但缺"平台的地基"。地基四件事——**DPI 感知、跟随系统主题、系统级无障碍事件、原生输入路径**——一件都还没立。做完这四件，再谈玻璃。

---

## 1. 实测环境与坐标系（先对齐标尺，否则数字全错 25%）

### 1.1 机器事实

| 项 | 实测值 | 取法 |
|---|---|---|
| OS | Windows 11 **25H2 / build 26200** | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` |
| 显示缩放 | **125%**（`LOGPIXELSX = 120`） | `GetDeviceCaps(dc, LOGPIXELSX)` |
| 系统主题 | **浅色**（`AppsUseLightTheme=1`、`SystemUsesLightTheme=1`） | `HKCU\...\Themes\Personalize` |
| 透明效果 | **关**（`EnableTransparency=0`） | 同上 |
| 高对比度 | **关**（`Flags=126`，bit0 = 0） | `HKCU\Control Panel\Accessibility\HighContrast` |
| 系统标题栏高 | `SM_CYCAPTION = 29`（120 DPI） | `GetSystemMetrics(4)` |
| 系统菜单栏高 | `SM_CYMENU = 25`（120 DPI） | `GetSystemMetrics(15)` |
| **PrismArchive 进程 DPI 感知** | **0 = PROCESS_DPI_UNAWARE** | `GetProcessDpiAwareness`，挂起态进程，未创建窗口 |

> 探针做法：`CreateProcess(CREATE_SUSPENDED)` 起一个**永不运行**的实例，只查感知级别，然后 `TerminateProcess`。
> 没有任何窗口出现在你的桌面上。用挂起态是因为 DPI 感知在进程启动时就由 manifest 决定了，不需要跑起来。

### 1.2 两份截图各自的换算

| 截图 | 物理像素 | 换算 | 说明 |
|---|---|---|---|
| `audit-prism-open.png` | 1732 × 1011 | ÷1.25 = **1385.6 × 808.8 DIP** | 我们是 unaware：代码按 96 DPI 排版，系统把整窗放大 1.25× |
| `audit-bandizip.png` | 1007 × 742 | ÷1.25 = **805.6 × 593.6 DIP** | Bandizip 是 PMv2：按 120 DPI **原生**绘制 |

**这是本文最重要的一个对照**：同一块屏、同一个 125%，Bandizip 的 1 DIP 线真的落在 1.25 px 上并被 D2D/DWrite 正确抗锯齿；我们的 1 DIP 线先在 96 DPI 画成 1 px，再被系统**双线性重采样**成 1.25 px。
后果不是"稍微模糊"，而是：
- 每个字形边缘多一层 25% 的混合像素，ClearType 的子像素条纹被抹成彩色噪点（我们菜单文字区仍有 21.5% 的"通道差 >10"像素，就是被抹过的条纹）；
- 1 px 分隔线变成 1.25 px 的"脏边"，实测列表行分隔线占 **2 行**（y=548、549 同为 `#363739`），而不是设计上的 1 px。

### 1.3 我们的界面度量（设备 px @125%，括号内为 DIP）

| 部位 | 实测 | 采样/依据 |
|---|---|---|
| 窗口外框 | 1732 × 1011（1385.6 × 808.8） | 截图尺寸 |
| DWM 边框 | 1 px `#4F5053` @ y=0 | GetPixel(900,0) |
| 标题栏 | **39–40 px**（31–32 DIP），`#0B0C10` | y=1..38 |
| 标题栏/菜单栏分隔 | 1 px `#24252A` @ y=39 | GetPixel |
| 菜单栏 | **23 px**（18.4 DIP），`#0B0C10` | y=40..62 |
| 菜单栏下亮线 | 1 px **`#D8D8D8`**（luma 216），**贯穿全宽** | y=63，x=20/200/400/900/1400/1720 全为同值 |
| 画布 | `#0C0D11` | (900,600) |
| 标签 | 33 px 高（26.4 DIP），填充 `#353A49`，顶缘 `#6E737E` | x=60, y=107..139 |
| 活动标签下划线 | 4 px `#0A79EB` | y=136..139 |
| 动作岛 | 70 px（56 DIP），`#1A1D25` | x=1200, y=144..213 |
| 列表行距 | 40 px（32 DIP），行底 `#1B1C1F`，分隔 **2 px** `#363739` | x=900, y=508..549 |
| 侧栏宽 | 276 px（220.8 DIP） | 边界 x=275/276 |
| 状态栏 | 顶分隔 1 px `#2D2E31` @ y=975，底 `#0E0F14` | (30,975) |
| 选中带（树） | `#242834`，与画布对比 **1.28:1** | (60,340) vs (900,600) |
| 品牌强调色 | `#0A79EB`，与画布对比 **4.59:1** | 标签下划线 |

### 1.4 文字对比度（WCAG 2.1，取字形最亮像素 vs 底色）

| 位置 | 前景 | 底色 | 对比度 | 判定 |
|---|---|---|---|---|
| 菜单文字 | `#F5F5F7` | `#0B0C10` | **17.95:1** | 远超 4.5 |
| 列表文件名 | `#F4F4F6` | `#1B1C1F` | **15.51:1** | 远超 |
| 标签文字 | `#F4F4F6` | `#353A49` | **10.32:1** | 合格 |
| 动作条标签 | `#EBEBED` | `#1A1D25` | **14.15:1** | 合格 |
| 侧栏分组标题 | `#9F9FA4` | `#0C0D11` | **7.37:1** | 合格 |

> 顺带一个"我们其实更好"的数字：Bandizip 工具栏最暗端 `#077BD7` 上的白字是 **4.32:1**，**低于 4.5**。
> 也就是说：动作条的对比度我们已经赢它，不必为了"一致"往回调。

---

## 2. 必须遵守的平台约定（逐条：现状 / 风险 / 规则）

### 2.1 标题栏与系统菜单

**现状（正确的那一半）**：我们用的是**系统 caption**，只通过 `DwmSetWindowAttribute` 改颜色（`WindowChrome.hpp:94-127`：immersive dark → caption color → border color），没有 `WM_NCCALCSIZE`、没有 `HTMAXBUTTON` 自绘按钮（`GalleryWindow`/`WindowFrame` 全文件都没有这两个消息）。
这意味着 **Snap Layouts 悬停、双击最大化、Alt+Space 系统菜单、Aero Snap 拖边、Win+方向键** 全部由系统提供——这是对的选择，**不要改**。

**风险**：
1. 标题文本是 `PrismArchive -- fixture.zip`（应用在前、双连字符）；Windows 约定是 **"文档 — 应用"**，Bandizip 是 `fixture.zip - Bandizip 6.29`。任务栏、Alt+Tab、窗口列表排序都按标题文本走，用户扫视时先看到"应用名"是反的。
2. `DWMWA_CAPTION_COLOR` 只有 Win11 22000+ 接受；Win10 上 `E_INVALIDARG`，标题栏退回深色模式的 `#202020`（luma 32），与画布 `#0C0D11`（luma 12.9）差 **19 级**，接缝会回来。
3. 菜单栏只有 18.4 DIP 高，比系统 `SM_CYMENU`（20 DIP，本机 25 px）**矮 1.6 DIP**；标题栏下沿到菜单栏文字基线的距离因此比任何一个 Windows 原生菜单都紧，键盘用户按 Alt 时的高亮框会显得挤。

**规则**：
- 标题模板固定为 `<文档名> — PrismArchive`（无文档时 `PrismArchive`）。分隔符用 **U+2014 全角破折号两侧各一空格**，长度对齐 Bandizip 的 ` - ` 视觉。
- 保留系统 caption。**禁止**为了"把标签画进标题栏"而改成 `WM_NCCALCSIZE` 自绘——那会一次性丢掉 Snap Layouts、系统菜单、平板模式的触摸目标，以及无障碍的窗口按钮。
- Win10 fallback：caption 拒绝上色时，把菜单栏底色改成与深色 caption 相同的 `#202020`，并把窗口 1 px 边框画成 `#202020`，让"系统色—菜单色"连续，而不是"系统色—画布色"硬切。
- 菜单栏高度改为**系统值**：`SM_CYMENU`（120 DPI 下 25 px），不要用固定 23 px。

**验收**：120 DPI 下 `SM_CYMENU == 25` 且菜单栏实测 25 px ±0；标题字符串匹配 `^.+ — PrismArchive$`。

---

### 2.2 DPI（96 / 125% / 150%）

**现状（P0 缺陷，已用进程探针钉死）**：

| 检查 | 结果 |
|---|---|
| exe 内嵌 manifest | 只有 `<trustInfo><requestedExecutionLevel level='asInvoker'/>`。**没有 `<dpiAware>`、没有 `<dpiAwareness>`、没有 `<compatibility>/<supportedOS>`** |
| 源码里的 API | `SetProcessDpiAwarenessContext` / `SetProcessDPIAware` **0 处**（`Src/` 全树 grep，命中只在 `Tests/win/`） |
| 运行时 | `GetProcessDpiAwareness = 0`（UNAWARE） |
| 推论 | `GetDpiForWindow` 永远返回 96；各对话框里的 `WM_DPICHANGED` 分支（`CompressDialog`、`SettingsDialog`、`AboutDialog`、`VaultDialog`）**是死代码** |

**后果（可量化）**：
- 125% 下所有文字被 1.25× 双线性重采样：一个 12 px 字号的 9 pt 文本，实际是 12 px 位图放大到 15 px，笔画边缘多出 25% 的混合像素。
- 1 DIP 的线（行分隔、卡片描边、图标描边）在物理上变成 1.25 px，落在 2 个像素行上（实测行分隔 2 px）。**在任何非整数缩放下，1 px 线条必然发虚**——这是几何问题，不是渲染质量问题。
- 150% 时是 1.5×，200% 时是 2×（整数，反而清晰）。所以**你的机器（125%）恰好是最坏的那一档**。
- 多显示器：把窗口从 125% 屏拖到 100% 屏，unaware 进程不会重排，系统换一个缩放系数继续拉伸——窗口尺寸"跳一下"，内容不重排。

**规则（按优先级）**：
1. exe manifest 加 `<dpiAwareness>PerMonitorV2</dpiAwareness>`，同时加 `<supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>`（Win10/11 兼容性声明；不加的话 `GetVersionEx` 会谎报 6.2，某些系统行为按 Win8 走）。
2. 所有几何走 **DIP → px 的单一换算函数**，缩放系数来自 `GetDpiForWindow(hwnd)`，不要用进程级常量。
3. `WM_DPICHANGED`：用 `lParam` 给的建议矩形 `SetWindowPos`，然后**重算**布局、重建字体（DWrite 的字号是 DIP，但 `SetDpi` 要重设）、重建 1/8 倒影 target。
4. 1 px 的发丝线改为**按物理像素对齐**：`lineWidth = 1.0f / scale` 且坐标取 `floor(x*scale)/scale`，这样 1 DIP 线在 125% 下正好 1 px、在 150% 下正好 1.5→2 px 且两端对齐。
5. 最小尺寸 **880 × 560 DIP**（保证 125% 下窗口不被系统裁切时仍有 4 列可见）；默认 **1100 × 700 DIP**。

**验收（可写进 ctest）**：
- `GetProcessDpiAwareness(hProcess) == 2`；
- 125% 下标题栏总高 **39 px ±1**、菜单栏 **25 px ±0**；
- 一条 1 DIP 分隔线在 125%/150%/200% 三档下，连续像素的 luma 极差 **≤ 2**（今天 125% 下是 12，因为跨了 2 行且一行是混合色）。

---

### 2.3 深色模式（以及"跟随系统"到底存不存在）

**现状**：
- 外壳：DWM 深色 caption + `DWMWA_CAPTION_COLOR` 上色（`WindowChrome.hpp`），菜单栏 `MFT_OWNERDRAW` 用同一 token（`#0B0C10`）→ 这一层做得对，实测标题栏 luma 12.08、画布 12.86，差 0.78。
- 内容：C91 之后 `scene.theme = chromeVariant_`（`GalleryWindow.hpp:4854`），画布不再是字面量。
- **但是**："跟随系统"是假的。`ChromeTheme.hpp:239` 的 `ShellVariantFor` 对 `"system"` 返回 `Variant::Midnight`，而且 **`Tests/test_chrome_theme.cpp:168` 把这条断言钉住了**：
  `PRISM_CHECK(prism::chrome::ShellVariantFor("system", false) == Variant::Midnight);`
  也就是说：这不是 bug，是**当前的设计决定**（"白菜单盖黑画布"修好之前不敢跟浅色）。
- **而你的机器是浅色**（`AppsUseLightTheme=1`）。Bandizip 因此是白底（`#FFFFFF` 列表、`#F3F3F3` 窗口底），我们是黑底。首屏第一眼就是"这不是 Windows 的应用"。

**风险**：
1. 迁移语境下这是最贵的一条：用户从白底 Bandizip 过来，落进一个全黑界面，第一反应是"我装错了/这是另一个东西"。
2. 系统切换深浅色时应用**完全无感**：全树没有 `WM_SETTINGCHANGE` 处理器、没有 `AppsUseLightTheme` 读取、没有 `UISettings`（grep 0 命中）。用户改了系统主题，要重启应用才可能变（而现在根本不变）。
3. 浅色模式下"玻璃"是灾难：C 文档已经指出浅色画布平铺大白 + 岛变成"脏塑料"。所以**不做浅色是有理由的**——但正确做法是把浅色做对，而不是把 system 定义成 dark。

**规则**：
- `system` 必须真的跟随：读 `AppsUseLightTheme`（或 `UISettings::GetColorValue(UIColorType::Background)`），并在 `WM_SETTINGCHANGE` 且 `lParam == L"ImmersiveColorSet"` 时重算 variant + 重绘（≤ 1 帧，不做动画）。
- 浅色画布**不要用纯白**：用 **`#F3F3F3`**（243）——Bandizip 的窗口底就是它，列表区才是 `#FFFFFF`。这一条"和 Bandizip 一致"与"Fluent 正确"是同一条，没有取舍。
- 深色画布保持 `#0C0D11`（luma 13.7），不要提到 `#202020`（Win11 的窗口底）——深色下我们的画布比系统更暗是刻意的，实测文字对比 17.95:1，比 Explorer 深色（约 15:1）还好。
- 如果产品坚持"深色是品牌"，就把选项写成**浅色 / 深色 / 跟随系统**三选一，默认"跟随系统"，并把 `ShellVariantFor("system")` 的断言改成"等于当前系统变体"。**不要**保留"system = dark"还叫它 system。

**验收**：
- `AppsUseLightTheme=1` 时启动 → 画布 luma ≥ 230；
- 运行中发 `WM_SETTINGCHANGE(0, L"ImmersiveColorSet")` → 500 ms 内画布 luma 变化 ≥ 100；
- 浅色下正文对比度 ≥ 4.5:1（`#1A1A1A` on `#F3F3F3` = 15.9:1）。

---

### 2.4 高对比度（HC）

**现状**：
- 有 `Variant::HighContrast` 令牌集，也有设置项 `appearance.highContrast`（`PrismCore/Appearance.hpp`、`SettingsDialog`），测试覆盖到 `c83`/`c91`/`c104`/`c78` 各主题帧。
- **但它是手动开关，不是系统状态的映射**：全树没有 `SPI_GETHIGHCONTRAST`、没有 `HIGHCONTRAST` 结构、没有 `WM_SETTINGCHANGE("HighContrast")`（grep 只在 `PasswordDialog`/`GalleryWindow` 命中 `SystemParametersInfoW` 的 `SPI_GETWORKAREA`/`SPI_GETNONCLIENTMETRICS`，与 HC 无关）。
- 你机器上 HC 是**关**的（`Flags=126`，bit0=0），所以这条今天不会咬人——但它是企业/老年用户的第一道门。

**风险**：
1. 用户在设置里打开"高对比度"后，应用外观**不变**，仍是深色玻璃 + 低对比选中带（1.28:1）。对低视力用户等于没生效。
2. HC 下玻璃/阴影/发光必须关掉（它们把对比度吃掉了）；我们的折射层是常量开启的路径（`appearance.backdropRefraction` 默认值在 `Appearance.hpp`），HC 变体若只换颜色而不改材质，仍会糊。
3. 自绘控件的 HC 颜色必须来自 `GetSysColor`/`GetSystemColor`，不能来自品牌 token——用户选的可能不是黑底白字，而是"高对比 白底黑字""高对比 黑色"等 4 套。

**规则**：
- 启动 + `WM_SETTINGCHANGE` 时读 `SPI_GETHIGHCONTRAST`；`HCF_HIGHCONTRASTON` 置位 → 强制 `Variant::HighContrast`，**并且无视用户的主题选择**（系统设置优先）。
- HC 下：`backdropRefraction = off`、glass alpha = 1.0（实色）、阴影 = 无、圆角 = 0（系统 HC 主题通常是直角）、焦点矩形 = **2 px 系统色**（`COLOR_HIGHLIGHT`）、禁用文字 = `COLOR_GRAYTEXT`（不要用 alpha 表达禁用）。
- 图标：HC 下切换到单色掩膜（当前图标是彩色位图/矢量填充，HC 下会糊成一团）。
- 菜单：HC 下**不要自绘**，交回系统（`MFT_OWNERDRAW` 摘掉）——系统菜单在 HC 下的配色、焦点、边框是经过验证的。

**验收**：HC 开 → 画布 == `GetSysColor(COLOR_WINDOW)` 逐通道 ±0；正文对比 ≥ 7:1；焦点矩形 2 px 且对比 ≥ 3:1；折射层绘制计数 == 0。

---

### 2.5 UIA 无障碍

**现状（比多数自绘应用好，但停在一半）**：
- `WM_GETOBJECT` 答 `OBJID_CLIENT`（`Accessible.hpp:672`）→ 一棵 MSAA `IAccessible` 树；菜单另答 `OBJID_MENU`。审计实测元素数 **9 → 33**（`docs/UI-AUDIT.md` 里程碑段）。
- 搜索框可写（`put_accValue`，A6 已修）。
- **缺口**：
  1. **没有一条 `NotifyWinEvent` / `UiaRaiseAutomationEvent`**（grep 0 命中）。选中行变了、焦点走了、状态栏数字变了——读屏**不会被告知**。它只能靠轮询。
  2. 没有 UIA provider，只有 MSAA 桥。`IGridProvider`/`ISelectionProvider`/`IScrollProvider` 全部缺失；10 万行时 MSAA 的 `accChildCount` 全量枚举会卡死客户端。
  3. 焦点是"模型内部焦点"（`ActionBarModel::SetFocusIndex`、`SidebarModel::SetFocus`），窗口本身没有子 HWND，`GetFocus()` 恒等于主窗口——UIA 的 `HasKeyboardFocus` 只能靠我们自报。

**风险**：读屏用户"看得见、点得动、但不知道发生了什么"。这在归档工具里尤其致命——解压进度、冲突对话框、覆盖确认全靠状态变化传达。

**规则**：
1. 每次选中集合变化 → `NotifyWinEvent(EVENT_OBJECT_SELECTION, hwnd, OBJID_CLIENT, childId)` + `EVENT_OBJECT_FOCUS`（焦点行）。
2. 状态栏文本变化 → `EVENT_OBJECT_NAMECHANGE`（限流：≥ 100 ms 一次，避免解压时刷屏）。
3. 进度 HUD → `EVENT_OBJECT_VALUECHANGE`，并暴露 `IAccessibleValue`（当前/最大）。
4. 行数 > 200 时，`accChildCount` 报**真实总数**，`accChild(i)` 按需生成（MSAA 的 virtualized item 约定）；元素必须有稳定 `childId`（用行索引会随排序漂移，应绑 `entryId`）。
5. Tab 环元素 ≤ 12 个；每个元素 Name 非空。

**验收**：选中第 2 行 → 200 ms 内 `WaitForSingleObject` 收到 `EVENT_OBJECT_SELECTION`（用 `SetWinEventHook` 在自己进程里监听）；UIA 客户端读 `SelectionPattern` 非空；10 万行场景 `accChildCount == 100000` 且单次 `accChild` 调用 ≤ 1 ms。

---

### 2.6 键盘导航（Tab 顺序 / 方向键 / Enter / Space / 助记符 / F10）

**现状**：

| 项 | 现状 | 证据 |
|---|---|---|
| 加速键表 | **没有** `CreateAcceleratorTable` / `TranslateAccelerator`（grep 0） | 全部快捷键走 `WM_KEYDOWN` + `PrismCore` 的 keys 表 |
| 菜单助记符 | 只有 `设置(S)...` 有；`文件`/`编辑`/`帮助` **没有** (F)(E)(H) | 截图 y=40..62 的文本 |
| Alt 进入菜单模式 | 只有 `WM_SYSCHAR` + `MnemonicMatches` 自绘匹配后弹 `TrackPopupMenu` | `docs/UI-AUDIT.md:67-68` |
| F10 / 单按 Alt | 未处理（F10 只在 Shift+F10 时被截获） | `GalleryWindow.hpp:2398` |
| Tab 环 | 动作条有（`HandleTabKey`），列表/树的行焦点有模型无环 | `GalleryWindow.hpp:3045` |
| 焦点环 | 动作条 2 DIP / 内缩 2 / 圆角 10（`ActionBar.hpp:111-113`）；**列表行、网格图块、树节点没有焦点环** | grep 无 |
| 标签页 | Ctrl+Tab / Ctrl+Shift+Tab 已修（C85） | `docs/PROGRESS.md:384` |

**风险**：
1. 键盘用户按 **F10 / Alt** 进不了菜单栏（Windows 的通用约定），必须先知道 `Alt+F`。而 `Alt+F` 又因为 `文件` 没有助记符而只能靠硬编码的 `WM_SYSCHAR` 匹配。
2. **列表/网格没有焦点环** = WCAG 2.4.7 不达标，也是"用键盘时不知道自己在哪"的直接原因。动作条有环、列表没有，用户会觉得"键盘只在一半的界面上有用"。
3. 没有加速键表意味着 `Ctrl+O`/`Ctrl+E` 等菜单项的快捷键提示与实现是两份真相；Bandizip 的菜单右侧会显示 `Ctrl+O` 之类的提示，我们显示不了（除非自绘右列）。

**规则**：
- 恢复全部助记符：`文件(&F)`、`编辑(&E)`、`查看(&V)`、`帮助(&H)`（Bandizip 是 `文件(F) 编辑(E) 查找(I) 选项(O) 视图(V) 帮助(H)`；我们少 `查找/选项/视图`，但**助记符字母要避开已占用**，见 B 文档的键位表，不要与它冲突）。
- 实现 **F10 / Alt 进入菜单栏模式**：进入后方向键在顶级项间移动、↓ 展开、Esc 退出、再次 Alt 退出。
- Tab 环顺序固定为：标签条 → 面包屑 → 动作条（8 个）→ 搜索 → 侧栏树 → 列表/网格 → 信息卡 → 状态栏（不可聚焦则跳过）。
- 列表键盘：↑↓ 移动、Home/End、PageUp/PageDown、Ctrl+A、Space 切换选中、Enter 激活、Backspace 返回上级、Esc 清空选中。
- **焦点环统一 2 DIP、内缩 2 DIP、对比 ≥ 3:1**（动作条已经是这个规格，把它变成全应用常量）。选中带（1.28:1）不能替代焦点环——两者必须能同时看见。
- 菜单项快捷键提示用右对齐的 12 DIP 文本，与 `HMENU` 的 `MFT_RIGHTJUSTIFY` 一致。

**验收**：注入 `VK_F10` → 菜单栏获得焦点（可读的 `EVENT_OBJECT_FOCUS` + 像素上的高亮项）；`Tab` 连续按 12 次回到起点且顺序与上表一致；每一行获得键盘焦点时，该行矩形内存在 2 DIP 的环且环像素与行底对比 ≥ 3:1。

---

### 2.7 IME（中文输入）

**现状（做得比多数自绘应用多，但缺最关键的一步）**：
- `WM_IME_STARTCOMPOSITION` / `WM_IME_COMPOSITION`（读 `GCS_COMPSTR` 预览、`GCS_RESULTSTR` 提交）/ `WM_IME_ENDCOMPOSITION` / `WM_IME_CHAR`（去重）都处理了（`GalleryWindow.hpp:2349-2392`）。
- **没有** `ImmSetCandidateWindow`、**没有** `ImmSetCompositionWindow`、**没有** `WM_IME_SETCONTEXT`（grep 0）。

**风险**：
1. **候选框位置由系统兜底**：没有 `CFS_CANDIDATEPOS` 时，IME 会把候选列表放在"它自己认为的位置"（常见是窗口左上或上次位置）。用户点开搜索框、打拼音，候选框可能出现在**窗口另一头**。这是中文用户最容易被劝退的细节，而且 Bandizip（原生 EDIT 控件）永远是对的。
2. 我们自己画了 `COMPSTR` 预览，但**没有抑制系统的 composition window**（`ISC_SHOWUICOMPOSITIONWINDOW` 未清），可能出现"自绘预览 + 系统预览"双份。
3. 插入符（caret）是自绘的，不调用 `CreateCaret`/`SetCaretPos`，所以系统完全不知道文字插入点在哪——`ImmSetCandidateWindow` 必须由我们显式给出坐标。

**规则**：
- `WM_IME_STARTCOMPOSITION` / 每次 `WM_IME_COMPOSITION`：
  `HIMC hc = ImmGetContext(hwnd)` → `ImmSetCompositionWindow(hc, {CFS_POINT, caretScreen.x, caretScreen.y})` → `ImmSetCandidateWindow(hc, {CFS_CANDIDATEPOS, caretScreen.x, caretScreen.y + caretH + 4})` → `ImmReleaseContext`。
- `WM_IME_SETCONTEXT` 里 `lParam &= ~ISC_SHOWUICOMPOSITIONWINDOW`（我们自绘预览时）。
- 候选框不得超出工作区：靠近屏幕底部时 `y = caretTop - candidateH - 4`（上翻）。
- 组合串的字体与正文一致（DWrite，字号同正文），不要用系统 IME 字体渲染预览。

**验收**：`ImmSetCompositionStringW` 注入一个组合串 → 读回 `ImmGetCandidateWindow` 的 `ptCurrentPos`：`x ∈ [搜索框左-8, 右+8]`、`y ≥ 搜索框底`；在 4 个窗口位置（贴顶/贴底/贴左/贴右）各测一次都不越界。

---

### 2.8 右键菜单与 Shift+F10

**现状（这条是全篇做得最对的）**：`WM_CONTEXTMENU` 建三种菜单（行 / 空白 / 侧栏），`Shift+F10` 与 Menu 键在 `WM_KEYDOWN` 里显式转同一个入口（`GalleryWindow.hpp:2394-2402`），最终 `TrackPopupMenu`（`:4153`）。`(-1,-1)` 的键盘路径也处理了。这就是 Win32 的标准答案。

**风险**：
1. `TrackPopupMenu` 未带 `TPM_WORKAREA` 时，贴任务栏弹出的菜单会被任务栏盖住一截。
2. ownerdraw 菜单项在 HC 下如果继续用品牌 token，会破坏系统对比度（见 2.4）。
3. 菜单字体没跟系统 `NONCLIENTMETRICS.lfMenuFont`：120 DPI 下系统菜单是 9 pt→15 px，我们若用 12 px DIP 常量，会比系统菜单**小一号**，且与本机其它应用的菜单不一致。

**规则**：
- `TrackPopupMenu(TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_WORKAREA, x, y, ...)`；
- 菜单字体取 `SystemParametersInfoW(SPI_GETNONCLIENTMETRICS)` 的 `lfMenuFont`，用 DWrite 按该字号建格式（`GalleryWindow.hpp:4438` 已经有取 nonclient metrics 的地方，复用它）；
- 菜单项高度 **≥ 24 DIP**，左右内边距 **12 / 8 DIP**，图标 16 DIP，禁用项用 `COLOR_GRAYTEXT`；
- **不做自研菜单动画**：系统菜单的展开/收起由 `MFT_OWNERDRAW` 之外的菜单窗口负责，加了反而抖。

**验收**：贴任务栏（工作区底边）弹出 → 菜单矩形完全落在 `SPI_GETWORKAREA` 内；HC 下菜单项像素 == `GetSysColor(COLOR_MENU)` ±2；菜单字高 == 系统 `lfMenuFont` 的 `lfHeight`（120 DPI 下 -15）。

---

### 2.9 滚动条（平台约定里最容易被忘的一条）

**现状**：**没有滚动条**。列表/网格是纯自绘，`GalleryRenderer` 里没有任何 thumb/track 绘制（grep `scrollThumb`/`DrawScroll` 0 命中），也没有子 HWND；`WM_MOUSEWHEEL` 有处理（`:2447`），但没有"我在哪/还有多少"的空间指示。截图 5 个条目时不显示，是因为根本没有。

**风险**：
1. 1000 个条目时用户不知道列表还有多长，也不知道拖到哪了——Bandizip 有（审计里明确写了"列表底部有水平滚动条"）。
2. UIA 的 `ScrollPattern` 无处可挂。
3. Win11 的细滚动条（静止 2 px、悬停 6 px、圆角、2 s 淡出）是**用户肌肉记忆**，缺了会觉得"这不是 Windows 控件"。
4. 触控板/触屏的惯性滚动、`WM_GESTURE`、中键自动滚动全部要自己实现。

**规则（二选一，别混）**：
- **A（推荐，风险最低）**：挂系统滚动条 `WS_VSCROLL` + `SetScrollInfo`，让它画。代价是外观与 Fluent 不完全一致（系统滚动条在 Win11 已是细的），收益是零风险。
- **B（自绘）**：宽度 **12 DIP 命中区**、可视 thumb **3 DIP**（hover 6 DIP）、圆角 = 宽/2、颜色 `#FFFFFF` 12%（hover 24%）、2 s 无操作淡至 40%、点击轨道按页滚动、Shift+滚轮水平。
- 无论 A/B：行数 ≤ 视口时 thumb 高度 = 100%（不隐藏滚动条槽位，避免内容宽度跳动）。

**验收**：1000 行时 thumb 高度 ≈ 视口/总数（±2 px）；点击轨道 → 滚动一页；UIA `ScrollPattern` 可读写 `VerticalScrollPercent`。

---

### 2.10 另外三条平台约定（容易被忽略，但都有实际用户影响）

| 项 | 现状 | 影响 | 规则 |
|---|---|---|---|
| **AppUserModelID** | 没有 `SetCurrentProcessExplicitAppUserModelID`（grep 0） | 任务栏不能正确分组/固定，JumpList 不可用，通知无法归属 | 启动第一行设 `PrismArchive.ArchiveManager`；安装器写同名 `HKCU\...\AppUserModelId` |
| **窗口位置持久化** | 只存 width/height（`docs/UI-AUDIT.md` S3-2） | 换显示器/改缩放后窗口跑到屏外 | 存 `GetWindowPlacement` 的 `rcNormalPosition`（虚拟屏坐标）+ 显示器 ID；恢复时若不在任何 `MonitorFromRect` 内，回落到主屏居中 |
| **触控/笔** | 没有 `WM_POINTER` 处理 | 平板模式下列表无法拖动滚动 | 所有可点目标 ≥ **32×32 DIP**（当前动作条按钮 56 DIP 高，达标；标签关闭按钮需查）；至少把 `WM_POINTERDOWN` 映射到鼠标路径 |

---

## 3. Fluent 2 与 Mica/Acrylic 的正确用法

### 3.1 什么该用、什么不该用

| 材质 | API / 值 | 该用在哪 | **不该**用在哪 | 前提 |
|---|---|---|---|---|
| **Mica** | `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW (2)` | 窗口**基底**：标题栏、侧栏、画布的最底层 | 卡片、菜单、浮层（Fluent 明文规定 Mica 只做窗口基底） | 窗口内容区必须**真的有透明像素**，否则 Mica 被完全遮住，等于没设 |
| **Mica Alt** | `DWMSBT_TABBEDWINDOW (4)` | 标签式窗口（我们要用标签，理应是这个） | 单文档窗口 | 同上；对比度比 Mica 高，适合有标签条的结构 |
| **Acrylic** | `DWMSBT_TRANSIENTWINDOW (3)` / `DesktopAcrylicBackdrop` | **临时**浮层：右键菜单、flyout、tooltip | 主窗口（耗电、掉帧；微软自己的文档也劝退） | 系统透明开启 |
| **实色** | 令牌色 | 以上全部失效时 | —— | HC / 远程桌面 / 省电模式 / 透明关闭 |

**本机的现实**：`EnableTransparency = 0`。也就是说 **Mica 在你这台机器上会被系统替换成实色**。所以：
- Mica 只能是"锦上添花"，**不能是层级设计的唯一手段**。实色画布必须先成立（我们的 `#0C0D11` 成立）。
- 设计稿必须同时给"Mica 版"和"实色版"两套截图，二者都要能通过对比度门槛。

**Mica 与 DComp 的硬前提**（不做这个，设了也没用）：
- 窗口用 `DwmExtendFrameIntoClientArea(hwnd, {-1,-1,-1,-1})` 把客户区并入扩展框架；
- D2D/DComp 交换链必须 `DXGI_ALPHA_MODE_PREMULTIPLIED`，并且在要透出 Mica 的区域 `Clear(0,0,0,0)`；
- 我们现在的画布是**不透明填充**（`Clear(#0C0D11)`），所以今天即使调用 `DWMWA_SYSTEMBACKDROP_TYPE` 也看不见任何东西。

### 3.2 为什么"每帧抓背景做折射"在桌面应用里是高风险选择

先把事实说清楚，免得打稻草人：

**我们代码里实际做的不是"每帧"**。`GalleryRenderer::DrawBackdrop` 有缓存键（`:538-548` 的 `BackdropKey`：宽高/count/items 指针/指纹/revision/listMode），键不变就复用（`:560`）。真正重建的是 `BuildBackdrop`：1/8 目标 + 15 taps × 2 次可分离模糊（`:578-660`）。所以"每帧全屏模糊"这个说法对我们不成立——**但它仍然是错的方向**，原因有三层：

**第一层（语义错）**：倒影里画的是**条目像素**（照片缩略图），不是"层背后的背景"。物理上，玻璃折射的是它**下面**的东西；而照片是**玻璃上面**的内容。把内容画进倒影，等于"把纸上的画印到玻璃上再糊一遍"。
实测：红卡与蓝卡之间的 11 px 间隙，开折射时 chroma 均值 **38.2 / 最大 53**，关折射时 **7.0 / 最大 7**（2651 个像素，没有一个 > 8）。间隙里出现 `#4F1D1C`（暗红）这种颜色，物理上不可能来自背景。

**第二层（缓存键错）**：`BackdropKey` 里**没有 `scrollOffset`，也没有 `theme`**。后果：
- 滚动列表时倒影像"贴"在屏幕上不动（背景不跟着内容走）；
- 切主题时倒影不换色（深色倒影盖在浅色画布上）。
这两条都是"看起来像 bug 的观感"，而且测试抓不到（现有 c77 只渲染网格、不改 scroll/theme）。

**第三层（平台能力错）**：如果真按字面去"抓背景"，应用**根本抓不到**：
1. DWM 在独立进程合成，窗口拿不到合成后的桌面纹理（`docs/ADR.md:24-25` 已写死这条边界）；
2. 退而求其次用 `Windows.Graphics.Capture`/`BitBlt`：1080p BGRA 一帧 **8.29 MB**，60 fps = **497 MB/s** 的拷贝，4K 是 **1.99 GB/s**；还要 GPU→CPU 回读（一次 PCIe 往返 + 管线 stall，毫秒级）；
3. 抓到的永远是**过去的帧**：DWM 合成 → 捕获 → 回读 → 再合成，滞后 **1–3 帧（16–50 ms）**。拖动窗口时玻璃里的世界会"拖影"；
4. 遮挡、最小化、锁屏、DRM 内容抓不到 → 折射层变黑/花屏；
5. Win11 的屏幕捕获会点亮**隐私指示器**，企业策略可能直接拒绝；
6. 持续 60 fps 回读让 GPU 无法进低功耗态——笔记本上这是能听见的风扇。

**结论**：折射只做**窗口内、岛屿正下方**的内容，且**只允许背景类图元**（画布 + 面板 + 分隔线），**禁止条目像素**（缩略图、图标、文字）。这和 C 文档的 #1 建议是同一条，且能用一个断言钉死。

### 3.3 正确的分层（我们能做、且做得起的）

| 层 | 内容 | 谁画 | 成本 | 失效时 |
|---|---|---|---|---|
| L0 | Mica Alt 窗口基底 | DWM | 0 | 实色 `#0C0D11` |
| L1 | 画布 + 面板（实色/半透明） | 我们 | 1 次填充 | —— |
| L2 | **岛屿下方内容的 1/8 折射**（只含 L1 的图元） | 我们，缓存 | 重建 ≤ 4 ms | 直接画 L1 的色 |
| L3 | 高光边（1 px rim）+ 内侧 1 px 暗边 | 我们 | 可忽略 | 去掉 |

L2 的具体参数（可断言）：
- 采样率 **1/8**（4K 下 480×270，可接受）；模糊半径按 **DIP 定义 24 DIP**（125% 下 30 px，现在写死 28 px）；
- 重建触发键必须包含 `{width, height, itemCount, scrollOffset, theme, listMode}`（今天缺后两个）；
- 重建预算 **≤ 4 ms**（1/8 + 15 taps×2 在全屏尺寸上约 2–3 ms；一旦超了就是键设计错了）。

---

## 4. 原生控件的取舍（哪些必须交给系统）

判据只有三条：**① 系统能给"输入法 + 无障碍 + 高对比 + 键盘"四件套的，交出去；② 需要虚拟化到 10 万行或需要逐像素品牌的，自己画；③ 出错代价高的（文件路径、覆盖确认），交出去。**

| 部位 | 结论 | 理由（可验收） |
|---|---|---|
| 标题栏 | **系统**（现状正确） | Snap Layouts / 系统菜单 / 平板触摸目标，自绘要重写 4 套行为 |
| 菜单栏与下拉 | **系统 HMENU + `MFT_OWNERDRAW`**（现状正确） | 键盘/助记符/IME/HC 由系统给；但 HC 下要退回纯系统绘制 |
| 文件/文件夹选择 | **系统 `IFileOpenDialog`** | 路径联想、OneDrive/网络位置、最近位置、UAC 虚拟化 |
| 覆盖/冲突确认 | **系统 `TaskDialog`**（不是 `MessageBox`） | 命令链接、按钮顺序、HC、读屏、键盘默认焦点 |
| 消息框/错误 | **`TaskDialog`** | 同上；`MessageBox` 在 Win11 上是"旧皮肤"，一眼 2007 |
| 滚动条 | **系统**（推荐）或严格复刻 Win11 尺寸 | 见 2.9 |
| 文本框（搜索、密码、路径） | **系统 `EDIT` 子类化**（推荐）或自绘 + 完整 IME 定位 | IME、选择、剪贴板、无障碍、HC 免费；自绘要自己实现 6 件事 |
| 复选框/单选/按钮（对话框内） | **系统控件**（现状：`SettingsDialog`/`VaultDialog` 已是子 HWND，正确） | 同上 |
| **列表/网格** | **必须自绘** | 10 万行虚拟化（实测每帧只画 25 行）、缩略图流水线、列宽拖拽、自定义排序——系统 `ListView` 在这个量级会掉帧且无法做 1/8 折射 |
| **工具栏/动作条** | **自绘** | 品牌与密度；但图标语言用 `Segoe Fluent Icons`（见 C 文档） |
| **标签栏** | **自绘**，形状按 Win11 | 没有系统 tab 控件可嵌入非 XAML 应用；但几何要像 Explorer 的标签（32 DIP 高、上圆角 8、下直角、选中页与内容同色） |
| 进度条 | 自绘（品牌）+ UIA `RangeValuePattern` | 系统进度条在深色下与画布不搭；但无障碍必须补 |

**一句话**：**输入类交出去，呈现类自己画。** 我们现在的错误恰好是反的——输入（搜索框、滚动）自绘且缺 IME/滚动条，而呈现（列表）该自绘的地方反而没做四列。

---

## 5. 性能与观感的平衡（每帧预算）

### 5.1 现有实测（`docs/PERF.md:360-371`）

| 100k 行列表帧（1200×800 软件目标） | 中位 | p95 |
|---|---|---|
| 记录 `Draw` | 4.149 ms | 11.345 ms |
| 栅格化/提交 `EndDraw` | 5.211 ms | 14.335 ms |
| **合计** | **11.565 ms（86.5 FPS）** | **20.081 ms（49.8 FPS）** |

结论已经写在文档里：**120 FPS 未达标（p95）**；flush 占 p95 的 45–71%。

### 5.2 预算表（建议写进 DESIGN.md，作为门禁）

| 阶段 | 预算 | 依据 |
|---|---|---|
| 输入 → 首个视觉反馈 | **≤ 50 ms** | Fluent 的响应阈值；超过就"粘手" |
| 按压/悬停状态切换 | **1 帧内开始**，过渡 100–120 ms | 小于 100 ms 察觉不到"动画"，大于 160 ms 觉得慢 |
| 面板/浮层展开 | **180–250 ms**，`cubic-bezier(0.2,0,0,1)` | Fluent motion 的大表面时长 |
| 每帧 CPU 记录 | **≤ 6 ms**（今天 4.1 中位） | 给提交留 10 ms |
| `EndDraw`/提交 | **≤ 8 ms**（今天 5.2 中位 / 14.3 p95） | 这是唯一 GPU 目标能改善的一半 |
| **整帧 p95** | **≤ 16.7 ms**（今天 20.08） | 60 Hz 的硬线；120 Hz 是 8.3 ms，先别承诺 |
| 折射/模糊重建 | **≤ 4 ms**，且**只在键变化时** | 1/8 + 15 taps×2 |
| 空闲 CPU | **≤ 1%**，0 个高频定时器 | 现在有 `kIdleTimerId`，确保最小化时停 |
| 窗口最小化/遮挡 | **停止一切**动画与重建 | `WM_SIZE(SIZE_MINIMIZED)` / `DwmGetWindowAttribute(DWMWA_CLOAKED)` |

### 5.3 三条"每帧"纪律

1. **模糊只对静态背景做一次**，缓存键必须含 `scrollOffset` 和 `theme`（见 3.2 第二层）。滚动时**不要**重建倒影——滚动是"内容在动、背景没动"，重建只会让倒影跟着抖。正确做法是把倒影当作屏幕空间的一层，滚动只移动内容层。
2. **动画不驱动重绘，重绘驱动动画**：所有过渡挂在 `CompositionTarget::Rendering`（或 `DwmFlush` 后的 `InvalidateRect`），不要在 `WM_TIMER` 里按 16 ms 自转；且动画只改 `transform/opacity`，不改布局（布局动画会触发全窗重排 + 倒影重建）。
3. **降级路径必须存在**：`DwmIsCompositionEnabled` 为假、远程桌面、HC、省电模式 → 折射层 `return`（走实色），且这条路径要有一条测试（今天的 `PRISM_C77_MUTANT_NO_BACKDROP` 就是现成的注入点，复用它做"降级渲染"的正向断言）。

---

## 6. 风险清单（自绘 UI 在 Windows 上的 12 个坑）

| # | 坑 | 症状 | 触发条件 | 检测 | 修法 |
|---|---|---|---|---|---|
| R1 | **DPI 模糊** | 全文发虚、1 px 线变 2 px | 任何非 100%/200% 缩放 | `GetProcessDpiAwareness != 2` | 2.2 |
| R2 | 非整数缩放下发丝线脏边 | 分隔线一半深一半浅 | 125%/150% | 连续像素 luma 极差 > 2 | `1/scale` 对齐 |
| R3 | **焦点环缺失** | 键盘走到列表不知道在哪 | 列表/网格/树 | 无 2 DIP 环像素 | 统一 2 DIP 环 |
| R4 | **读屏收不到事件** | 选中变了读屏不吭声 | 任何选择变化 | `SetWinEventHook` 收不到 | `NotifyWinEvent` |
| R5 | **IME 候选框错位** | 候选列表在窗口另一头 | 中文输入 | `ImmGetCandidateWindow` 坐标 | `ImmSetCandidateWindow` |
| R6 | 高对比失效 | 开 HC 后界面没变 | 系统 HC 开 | 画布 ≠ `COLOR_WINDOW` | 2.4 |
| R7 | 多显示器/缩放变更 | 窗口跑屏外、内容不重排 | 拖到另一块屏 | `GetWindowPlacement` 不在任何 monitor 内 | 2.10 |
| R8 | ownerdraw 菜单在 HC/远程桌面 | 白底白字或对比不足 | HC / RDP | 菜单像素 ≠ `COLOR_MENU` | HC 退回系统绘制 |
| R9 | 最大化后 DWM 边框消失 | 1 px 外框突然不见/多一条 | 最大化/贴边 | 边框像素带变化 | 用 `DWMWA_BORDER_COLOR` 而非自绘边框 |
| R10 | 触控无目标 | 手指点不中关闭按钮 | 平板模式 | 命中矩形 < 32 DIP | 32 DIP 最小目标 |
| R11 | 全屏/独占时倒影错位 | 玻璃里的内容与真实内容错半帧 | 滚动/拖拽 | 倒影与内容偏移 > 1 px | 屏幕空间层（5.3） |
| R12 | 10 万行无障碍枚举 | 读屏卡死 | `accChildCount` 全量 | 枚举耗时 > 1 s | 虚拟化 item |

**另外三条"进程级"的坑**（不常被提起，但会直接毁掉"原生感"）：
- **没有 `WM_SETTINGCHANGE`**：系统主题、字体大小、无障碍开关改了，应用全无反应（我们 0 处）。
- **没有 `WM_DISPLAYCHANGE`/`WM_THEMECHANGED`**：换显示器、换主题时不重建 DWrite 文本格式与位图缓存。
- **没有 `WM_QUERYENDSESSION`**：关机/注销时可能被当成"阻止关机"的应用。

---

## 7. 与 Bandizip：哪些必须一致（迁移成本）、哪些应当超越（平台层）

> 只列**平台/运行时**相关的；视觉层的分割线见 C 文档第 6 节，键位见 B 文档第 3 节。

### 7.1 必须一致（不一致 = 用户要重新学）

| 项 | Bandizip | 我们必须做到 | 为什么是迁移成本 |
|---|---|---|---|
| 标题栏文本格式 | `fixture.zip - Bandizip 6.29` | `fixture.zip — PrismArchive` | 用户扫标题栏找文件名，位置必须在前 |
| 菜单助记符 | `(F)(E)(I)(O)(V)(H)` | 每个顶级项都有 | 键盘用户的第一反射 |
| Alt/F10 进入菜单 | 系统行为 | 必须有 | 同上 |
| 右键菜单动词顺序 | 解压到/解压到当前/打开/… | 同序 | 位置记忆 |
| 文件/文件夹选择 | 系统对话框 | 系统对话框 | 路径联想 |
| 覆盖确认 | 系统对话框 + 记住选择 | 系统 TaskDialog | 危险操作的一致性 |
| 滚轮/PageDown/Home/End | 标准 | 标准 | 肌肉记忆 |
| 深色模式跟随系统 | 是（截图白底=系统浅色） | 必须 | 第一眼"这是不是一个 Windows 应用" |
| 双击文件夹进入、Backspace 返回 | 是 | 是 | 导航模型 |

### 7.2 应当超越（差异化，且每条都能验收）

| 项 | 我们的做法 | 验收数字 |
|---|---|---|
| **真正的 Per-Monitor V2** | manifest + `WM_DPICHANGED` 全链路 | 125%/150% 下 1 DIP 线 luma 极差 ≤ 2；Bandizip 在这一点上也只是"不糊"，我们能做"不糊且线宽对齐" |
| **系统级无障碍事件** | `NotifyWinEvent` + 虚拟化 item | 选中后 200 ms 内收到事件；10 万行可读 |
| **IME 候选框精确跟随插入符** | `ImmSetCandidateWindow` | 候选点与插入符偏差 ≤ 8 DIP |
| **Mica Alt 基底 + 内容折射** | L0–L3 分层 | 透明关闭时自动退化且对比度不变 |
| **可键盘完成全流程** | F10 + 全量助记符 + Tab 环 + 焦点环 | Tab 12 步闭环；无鼠标可完成"打开→解压→确认" |
| **动作条对比度** | 现 14.15:1 | Bandizip 4.32:1 —— 我们已经在这一点上赢了，别再往回调 |
| **深浅色双语种主题** | 4 套变体（Day/Night/HC/……） | 三主题全窗差分 ≥ 90%（今天 `UI-AUDIT` A12 实测 7.96%） |

### 7.3 明确不要为了"原生"而做的事

- **不要**为了像 Explorer 而换成 `ListView`/`TreeView`：我们会失去 10 万行虚拟化与缩略图流水线，且无法做折射。
- **不要**为了像 Bandizip 而把菜单栏做成蓝色渐变条（那是 2007 的语言）。
- **不要**自绘标题栏按钮（丢掉 Snap Layouts）。
- **不要**在 HC 下坚持品牌色。

---

## 8. 落地顺序（按"修一条消失一类问题"排）

| 序 | 动作 | 为什么排这里 | 验收 |
|---|---|---|---|
| 1 | manifest 加 `PerMonitorV2` + `supportedOS` | 一行配置，消掉全部"糊" | `GetProcessDpiAwareness == 2` |
| 2 | `WM_SETTINGCHANGE` 监听 + `system` 真跟随 | 让应用"住在 Windows 里" | 改系统主题 500 ms 内变色 |
| 3 | HC 自动跟随 + 折射降级 | 无障碍底线 | HC 下折射计数 0 |
| 4 | `NotifyWinEvent` + 焦点环 | 读屏与键盘可用 | 事件 + 2 DIP 环 |
| 5 | `ImmSetCandidateWindow` | 中文用户第一体验 | 候选点跟随插入符 |
| 6 | 滚动条（系统或自绘） | 空间感 | thumb 比例正确 |
| 7 | 倒影只含背景 + 缓存键补 `scroll/theme` | 消掉光晕（C 文档 #1） | 间隙 chroma ≤ 8 |
| 8 | Mica Alt + 实色 fallback | 材质升级 | 透明开关两态都达标 |

---

## 9. 证据与复现

### 9.1 进程 DPI 感知（决定性证据）

~~~powershell
# 挂起态启动，只查感知级别，不创建窗口
$si = New-Object DpiProbe+STARTUPINFO; $si.cb = [Runtime.InteropServices.Marshal]::SizeOf($si)
$pi = New-Object DpiProbe+PROCESS_INFORMATION
[DpiProbe]::CreateProcess($exe,$null,[IntPtr]::Zero,[IntPtr]::Zero,$false,0x4,[IntPtr]::Zero,$dir,[ref]$si,[ref]$pi)
$v = 0; [DpiProbe]::GetProcessDpiAwareness($pi.hProcess, [ref]$v)
# 输出: hr=0 value=0   (0=UNAWARE 1=SYSTEM 2=PER_MONITOR)
~~~

### 9.2 exe 内嵌 manifest

~~~powershell
$txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
$txt.IndexOf('dpiAware')        # -1
[regex]::Matches($txt,'<assembly[sS]{0,3000}?</assembly>').Value
# <assembly ...><trustInfo><security><requestedPrivileges>
#   <requestedExecutionLevel level='asInvoker' uiAccess='false' /></requestedPrivileges>
# </security></trustInfo></assembly>      ← 只有这些，没有 dpiAware / supportedOS
~~~

### 9.3 系统状态

~~~powershell
Get-ItemProperty 'HKCU:SoftwareMicrosoftWindowsCurrentVersionThemesPersonalize'
#   AppsUseLightTheme    : 1        ← 系统是浅色
#   EnableTransparency   : 0        ← 透明效果关闭（Mica 会退化为实色）
Get-ItemProperty 'HKCU:Control PanelAccessibilityHighContrast' | Select Flags
#   Flags : 126                     ← bit0 = 0，高对比关闭
~~~

### 9.4 像素（`System.Drawing.GetPixel`）

~~~powershell
$b = [Drawing.Bitmap]::new('...audit-prism-open.png')
$b.GetPixel(900,0)    # 4F5053   DWM 边框
$b.GetPixel(900,63)   # D8D8D8   菜单栏下 1 px 亮线（贯穿全宽）
$b.GetPixel(900,600)  # 0C0D11   画布
$b.GetPixel(60,137)   # 0A79EB   标签下划线（4 px）
$b.GetPixel(900,548)  # 363739   行分隔（占 2 px：548–549）
~~~

### 9.5 光晕量化（A/B，同一场景只切 `backdropRefraction`）

| 区域 | 开折射 | 关折射 |
|---|---|---|
| 红/蓝卡间隙 x640–650, y240–480（2651 px） | 均值 **38.2**，最大 **53**，>8 的像素 **2651/2651** | 均值 **7.0**，最大 **7**，>8 的像素 **0** |
| 最后一张卡右侧 x990–1120（31571 px） | 均值 16.4，>8 的 **11564** | 均值 9.2，>8 的 **743** |
| 单点 (650,330) | `#4F1D1C`（暗红雾） | `#101117`（画布） |

---

## 10. 我没验证的（不要当成"已经没问题"）

1. **150% / 200% 下的实际渲染**：本文的 DPI 结论来自 125% + 进程探针，另外两档没跑。
2. **HC 下玻璃是否真的关掉**：我只看了令牌与设置项，没跑 HC 帧。
3. **UIA 事件**：我确认了"没有 `NotifyWinEvent`"，但没跑 NVDA/Narrator 端到端（`tools/c80-a11y-uia.ps1` 可复用）。
4. **IME 候选框的实际错位**：我确认了"没有 `ImmSetCandidateWindow`"，但没有用真输入法录一次候选框坐标（需要一台能切中文输入法的交互会话）。
5. **Bandizip 的 DPI 感知级别**：我从截图尺寸反推它是 PMv2（1007×742 @125%），没有对它跑 `GetProcessDpiAwareness`——它可能装在受保护目录、探针需要管理员。
6. **Mica 实际观感**：本机 `EnableTransparency=0`，设了也看不见；要评估必须开透明后另测。
7. **10 万行下的 MSAA 枚举耗时**：没实测。

---

## 11. 给实现者的一句话

> 先把**地基**打完——`PerMonitorV2` + 跟随系统主题 + HC 自动降级 + 一条 `NotifyWinEvent` + 一个 IME 候选点——这五件事加起来大概是 3 天的量，却能让界面从"一个画得很用心的窗口"变成"一个 Windows 应用"。
> 玻璃与折射留到最后做，而且**只折射背景、不折射内容**；否则你花在 1/8 模糊上的每一毫秒，都会被 11 px 间隙里那圈 `#4F1D1C` 的雾抵消掉。
