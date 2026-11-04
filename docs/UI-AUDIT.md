# UI 走查记录（用工具驱动，不靠肉眼印象）

本文件记录**实测截图 + UIA 探针**发现的界面缺陷。每条都要有：现象、证据（截图/探针输出）、根因、状态。
工具：`agent-desktop`（UIA 快照 + 窗口截图，headless，不抢焦点）。

---

## A1 客户端区域对 UIA 完全不可见（无障碍缺陷）

**证据**：`agent-desktop snapshot --app PrismArchive.exe -i --compact` → `ref_count=9`
（标题栏 3 个按钮 + 系统菜单 + 菜单栏 4 项）。

动作岛 8 个按钮、标签栏、列表的每一行、侧栏树、状态栏**一个都不在无障碍树里**。

**后果**：读屏软件用户无法使用；自动化测试也只能退回像素比对。
**根因**：全自绘，没有 `WM_GETOBJECT` / `IAccessible`。
**状态**：C80 处理中。

---

## A2 列表表头被渲染成"选中行"（灰底 + 焦点环 + 文字重影）

**证据**：窗口截图右上角，列头区域是一个**灰色圆角面板**，带**蓝色焦点环**，
且"原始大小"文字与下层重影叠在一起。

**应当是**：列头是**扁平文字** + 一条 1px 分隔线，没有面板、没有焦点环。
**根因（待确认）**：表头行与"选中行"共用同一套绘制路径（或表头被当作索引 0 的行）。
**状态**：✅ 已修（C81/#103）——表头扁平化 + 1px 分隔线，实测截图确认，变异臂 `header_selected` 会红。

---

## A3 压缩率在三个地方互相矛盾

**证据**（同一帧、同一个 15 B 文件）：
- 列表行：`100%`
- 左下信息卡：`压缩比 126.7%`
- 底部状态栏：`压缩率未知`

**后果**：用户不知道信哪个。三处必须来自**同一个计算**，或者明确标注各自口径。
**状态**：✅ 已修（C81/#103）——`CompressionRatioView` 单一事实来源，行/卡/状态栏一致（`>100% (126.7%)`），目录显示 `未知`。

---

## A4 标题栏 / 菜单栏是系统白色，与深色画布割裂

**证据**：截图顶部 64px 是系统标准白色标题栏与菜单栏，下面直接接深色画布。
**影响**：这是"不像同一个应用"的最大来源；Liquid Glass 要求窗口外壳与内容**连续**。
**状态**：✅ 已修（C83）。对比图：`docs/assets/ui/c83-shell-before.png` / `c83-shell-after.png`
（同一份 `Assets/Lang` + 同一台机器，before 是修复前的 exe，after 是修复后的 exe）。

**修法**
- **标题栏**（`Src/PrismWin/WindowChrome.hpp`）：`DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)`，
  属性号先试 **20**（Win10 20H1+/Win11）再退 **19**（Win10 1809–1909）；失败是返回值，不是崩溃。
  只开深色不够——Win11 的深色标题栏是 #202020，仍比画布亮 ~20 级——所以同时用
  `DWMWA_CAPTION_COLOR` 把标题栏画成 **`--surface-backdrop-reference`**（Win11 22000+；Win10 拒绝时
  记录 `captionApplied=false`，测试改用暗色标题栏的 40 级上限 + 绝对 <64），并用
  `DWMWA_BORDER_COLOR` 画 1px `--border-specular-rim`。
- **菜单栏**（`GalleryWindow::RebuildMenu` + `WM_MEASUREITEM`/`WM_DRAWITEM`）：`MFT_OWNERDRAW`，
  保留 HMENU、菜单循环与键盘；底色/文字/选中行/分隔线全部来自
  `prism::chrome::PaletteFor(variant)`（`Src/PrismCore/ChromeTheme.hpp`），
  `SetMenuInfo(MIM_BACKGROUND|MIM_APPLYTOSUBMENUS)` 让下拉面板的空隙也是深色。
- **无障碍（实测发现的坑）**：owner-draw 菜单项**没有 OS 文本**，系统菜单对象会把四项报成
  **无名**菜单项（agent-desktop 实测：`MENU-ITEMS:  |  |  |`）。窗口因此自己回答
  `OBJID_MENU`，用**同一批** `MenuDrawItem` 构造 `MenuBar/MenuItem` 树
  （`A11yTree::Role::MenuBar/MenuItem`）；修复后 snapshot 为
  `文件/menuitem | 编辑/menuitem | 设置(S).../menuitem | Help/menuitem`。
- **助记符**：同上，系统无法匹配不存在的文本，窗口在 `WM_SYSCHAR` 用**画出来的同一串字**匹配
  （`prism::chrome::MnemonicMatches`），命中后弹 TrackPopupMenu（方向键/Enter/Esc 仍是系统的）。
- **主题**：`appearance.theme`（light/dark/system）+ `appearance.highContrast` 经
  `prism::chrome::ShellVariantFor` 映射到唯一 Variant；启动时与保存设置后都重画外壳（`Main.cpp`）。

**断言（可复现）**
- `win.c83_chrome`（真实窗口 + 屏幕像素）：DWM 记录 `dark=1 attribute=20 caption=1 border=1`；
  画布 luma **12.86**、标题栏 **12.08**（差 −0.78 < 12）；菜单栏众数色 **#0B0C10** ==
  `--surface-backdrop-reference`（Midnight）；下拉行 `WM_DRAWITEM` 像素 **#0B1F36** ==
  `--selection-tint` over 菜单底；四个顶级项与 `kMenuSettingsId` 仍在 HMENU 上。
- `core.chrome_theme`（Linux lane）：三主题下文字对比度 **16.83 / 17.95 / 21.00**
  （选中行 14.03 / 15.24 / 14.45，均 ≥ 4.5）；菜单底逐通道等于令牌；暗色外壳 luma 12.08 vs 画布 13.74；
  助记符匹配（`&File`、`设置(&S)...`、`&&` 字面量、无 & 不匹配）。
- **负向控制**：`PRISM_C83_MUTANT_NO_DWM` → 标题栏 luma **243.30**，**像素断言**红
  （`C83 MUTATION CAUGHT: no_dwm`）；`PRISM_C83_MUTANT_NO_OWNERDRAW` → 菜单栏 **#FFFFFF**，背景断言红
  （`no_owner_draw`）；`PRISM_C83_MUTANT_MENU_SYSTEM_LIGHT` → 白底白字，对比度断言红
  （`menu_system_light`）。

**已知边界**：画布本身仍是 `GalleryRenderer::kBackdrop` 的固定 #0B0E13（Day/Night 的画布切换是另一张卡）；
`system` 因此映射到 Midnight —— 在"白菜单盖黑画布"修好之前跟随浅色系统桌面只会重现 A4。

---

## A5 活动标签与非活动标签区分度不够

**证据**：活动标签只有一圈细边框 + 关闭按钮，非活动标签仅文字 + 关闭按钮。
**应当是**：活动标签明显"抬起"（更亮的填充 + 底部 2px 强调线），非活动标签关闭按钮仅在 hover 出现。
**状态**：C79 处理中。

---

## 说明

- 截图与 UIA 原始 JSON 含本机窗口标题，**不提交到仓库**；这里只记录结论。
- 每条修复都必须附**前后截图**与**像素断言**。

---

## A6 搜索框对 UIA 只读（读屏能看见、输不进去）

**证据**（C80 合并后，真实工具驱动）：
```
snapshot → 找到「搜索框」 → agent-desktop type <ref> "zzz"
→ POLICY_DENIED: supported_action 不满足
   visible pass / enabled pass / editable pass / supported_action fail
```

**后果**：读屏或自动化能点开搜索框，但**无法把查询串写进去**。
**修法**：搜索框元素暴露可写的 `accValue`（MSAA → UIA 的 ValuePattern），写入后走与 `WM_CHAR` 同一条 model 更新路径；加"写入后列表被过滤 / 写空串恢复"的断言。
**状态**：已修复（C80 后续提交）。`put_accValue` 写入整串查询，走与 `WM_CHAR` 同一条
model 路径（`SetQuery`/`InsertText` + 同一个 `ApplySearchFilter`）；字段带
`STATE_SYSTEM_FOCUSABLE`、不带 `STATE_SYSTEM_READONLY`，默认动作把键盘焦点放进字段。
断言：`c80.a11y`（写入 → 查询/行数/回读/清空恢复）、`c80.a11y_uia`（ValuePattern.SetValue
12 行 → 1 行 → 清空 → 12 行）、负向控制 `c80.mutation.readonly_search` 与
`c80.mutation.readonly_search_uia`（`PRISM_C80_MUTANT_READONLY_SEARCH` 下两条都必须红）。
顺带修正：无障碍列表现在镜像**画出来的**行（过滤+排序后的 `listRowToFolder_`），
此前查询把列表缩到 1 行时，读屏仍被告知有 12 行。

---

## 里程碑：UIA 从 9 个元素 → 33 个（C80）

修复后同一个探针抓到：
```
打开 / 解压到… / 压缩 / 添加 / 删除 / 测试 / 查看 / 搜索
Photos.7z / Backup.7z        （标签）
one.txt                      （列表行）
Photos.7z › 共 1 项           （面包屑）
共 1 项 / 未加密 / …          （状态栏）
```
并且 `click` 动作岛上的「搜索」ref 后，树里出现「搜索框」——**工具驱动的端到端测试从此可用**。

---

## A7 语言包缺失时降级成"中英混杂"（低优先级）

**证据**（从构建目录直接跑 exe，没有 Assets/Lang）：
```
INFO  i18n: language resolved tag="zh-CN" catalogs="0"
WARN  i18n: language table unavailable: ...\Assets\Lang\en-US.json
```
结果：菜单显示中文（因为 `tr.Or(key, 中文回退)`），但侧栏标题与信息卡显示英文（因为
`LabelsFrom` 的默认值是英文）——**同一窗口两种语言**。

**触发条件**：只有 Assets 缺失时（损坏的安装/从构建目录裸跑）。正常安装不会。
**建议**：让 `Labels` 的默认值与菜单的回退同源（都中文），或语言包加载失败时统一走一个
"fallback language" 常量；断言：catalogs=0 时窗口内不出现两种语言。
**状态**：记录，未修（低优先级）。

---

## A8 用户 6-lane 实测报告（`agent-desktop-tests/02-问题汇总.md`）复测结论

用户报告基于 `E:\build\adtest-main`（08:06 构建），比 C82 合并早 3 小时。**逐条复测**结果：

| 报告编号 | 结论 | 我的证据 |
|---|---|---|
| S1-1 CLI 加密挂死 | **旧构建，已修** | `--test enc.7z` → exit=2 + `encrypted: password required`；`--password` → exit=0, verify ok=1 |
| S1-1 GUI 密码框不出现 | **真（P0）** | 隔离 profile 启动 enc.7z → 进程活着、`list-windows` 返回空；根因 `PasswordDialog` 不 Show |
| S1-2 多卷被拒 | **旧构建，已修** | `--smart-extract-each vols.7z.001` → exit=0, 25000 B |
| S1-3 设置一击僵尸 | **真（P0）** | `WM_COMMAND 0x4B31` → `IsWindowEnabled` True→False，无可见窗口；根因 `CreateWindowExW` 无 `WS_VISIBLE` |
| S2-2 无确认解压 | **部分真** | 列表行激活确实整档解压（C34 既定设计，落新目录、有状态反馈）；**侧栏树根点击不写盘**（计数 1→1） |
| S2-4 HUD UIA 不可见 | **真** | 30 MB 解压全程 `progress=0` |
| S2-5 压缩按钮无响应 | **真** | 同 S1-3 根因（`CompressDialog` 不 Show） |
| S2-6 A2/A3 仍在 | **旧构建，已修** | 表头已扁平、压缩率三处一致（C81/#103，我跑其 exe 截图确认） |
| S3-1 日志不落盘/1970 | **真** | LOCALAPPDATA 下无 `.log`；stderr 前缀 `1970-01-01T00:00:00` |
| S3-2 窗口位置不持久化 | **真** | `settings.json` 的 `window` 段只有 width/height |
| S3-3 错误文案为空 | **真** | 不存在的路径启动 → 弹窗只有 确定/关闭，无文本 |
| S3-6 Ctrl+Tab/Ctrl+W 无效 | **真** | 真按键（`SetForegroundWindow`+`keybd_event`）标题不变 ×3；根因 `HandleActionBarKey` 无条件吞 `VK_TAB` |
| S3-11 状态栏分段 UIA 不可达 | **真** | 树里只有 `status | 共 N 项` |

教训（本会话第三次同型）：**报告与构建必须配对**。用户测的是旧构建，三条"严重度 1"里两条早已修好；
而真正的新 P0（密码框/设置框不可见）恰好是同一条 `WS_VISIBLE` 根因——**一个根因解释了三个缺陷**。
---

## A9 密码对话框：Win32 原味浅灰 + 全英文（C84 修好可见性后暴露）

**证据**：`E:\Temp\uicheck\pwd-dialog.png`（C84 构建，打开 enc.7z）。
现在对话框**可见了**（C84 修好 `PasswordDialog` 不 Show 的 P0），但内容一眼不对：

- **语言**：`This archive needs a password.` / `Password` / `Show password` / `Remember in vault` /
  `This time only` / `Ring (masked)` / `Stored passwords (masked)` / `Use selected` / `Open` / `Cancel`
  ——全是英文，而窗口标题、菜单、状态栏都是中文。
- **视觉**：浅灰系统背景条、系统 group 框、系统按钮——与应用主体的深色玻璃完全不是一套语言。
  这是 A4（系统白与深色画布割裂）在对话框上的同一类问题，而且更严重：整块是系统控件。

**影响**：加密归档的用户第一步看到的就是这个框，它决定"这软件做得认真吗"。
**要求**：①所有字串走 i18n 表（`password.*` 键，中英双语）；②按 `DESIGN.md` 的玻璃卡样式重绘
（圆角 14、`GlassPanel` 三原语、深色背景、主按钮强调色）；③键盘与 UIA 不退化（现有 textfield/按钮可达）。
**状态**：待修（记入队列）。