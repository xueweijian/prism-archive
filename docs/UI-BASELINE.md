# UI-BASELINE.md — 视觉验收矩阵（C108）

> 这张卡不改产品代码。它把"看起来对不对"变成**可复现的数字**，给正在进行中的 9 张视觉卡
> （C100–C107、C109）当裁判：每一行 = 一条可测量的视觉主张 + 一个阈值 + 现在的实测值 +
> 谁负责把它改绿。

## 0. 一条命令看全部

```pwsh
pwsh -File tools/visual_baseline.ps1 -BuildDir <build-dir>
```

它会同时读三处并打印同一张表：

| 来源 | 给出哪些行 |
|---|---|
| 测试二进制 `<build-dir>/Tests/core_c108_baseline.exe` | 助记符字母数、Day 画布令牌值、岛体差、`Segoe UI` 字面量数、菜单/标题色差、光链路调用点 |
| 基准截图 `docs/assets/ui/baseline/prism-1600x1000-simple-zip.png` | 画布像素、岛体差（像素）、压缩图标中心、侧栏选中角、信息卡下划线、菜单/标题（像素） |
| 源码 `Src/PrismWin/GalleryRenderer.hpp` | 压缩率条高度 / alpha / HSV 饱和度 |

`-Strict` 在有 FAIL 时返回 1（卡片自查用）；`-OutJson <path>` 输出机器可读版本。
二进制没构建时对应行打印 `n/a`，**不会**编一个数字出来。

## 1. 基准工件（固定窗口 + 固定内容）

| 项 | 值 |
|---|---|
| 窗口外框 | **1600 × 1000**（DPI 96；SetWindowPos 精确设置并回读 `GetWindowRect`） |
| 截图 | 客户区 **1580 × 980**（`agent-desktop` 抓的是客户区；四边各 10 px 非客户区） |
| 内容 | `E:\Temp\prism-adtest\fixtures\simple.zip`（根：1 个文件夹 `payload_text`，全档 16 项） |
| 外观 | 默认（Daylight）；`%LOCALAPPDATA%\PrismArchive\settings.json` 不存在时的出厂状态 |
| 语言 | 仓库 `Assets/Lang` 随 exe 部署（否则命中 A7 的中英混排，不是视觉基线） |
| 主题 | 未手动切换（标题栏/菜单栏/画布同为浅色，正是 C106 要守的"一个外壳"） |

### 复现命令

```pwsh
# 1) 构建（预设与 CI 同款；env.ps1 见 docs/HANDOVER.md）
. E:\wg\env.ps1 -Card c108
cmake --preset release-msvc
cmake --build build/release-msvc --parallel 2 --target PrismArchive

# 2) 把语言包与引擎放到 exe 旁边（缺 Lang 就是 A7 的混排）
$exeDir = 'build/release-msvc/Src/PrismWin'
Copy-Item -Recurse -Force Assets $exeDir
Copy-Item -Force $env:PRISM_7Z_DLL (Join-Path $exeDir '7z.dll')

# 3) 以固定内容启动，再把窗口设成 1600x1000（必须先声明 DPI 感知，否则坐标被缩放）
$fixture = 'E:\Temp\prism-adtest\fixtures\simple.zip'
$p = Start-Process (Join-Path $exeDir 'PrismArchive.exe') $fixture -WorkingDirectory $exeDir -PassThru
Start-Sleep -Seconds 5
# Win32: SetProcessDpiAwarenessContext(-4) 后 SetWindowPos(hwnd, 0, 10, 10, 1600, 1000, SWP_NOZORDER)
# 本次实测：GetWindowRect = 10,10..1610,1010 => 1600x1000

# 4) 抓图（窗口 id 来自 list-windows，形如 w-<hwnd>）
agent-desktop list-windows --app PrismArchive.exe
agent-desktop screenshot --window-id w-<hwnd> docs/assets/ui/baseline/prism-1600x1000-simple-zip.png
```

## 2. 验收矩阵

判定符号：**FAIL** = 现在没达标（就是待办）；**PASS** = 现在达标，改动不得回退。
下表 13 条主张；工具把它展开成 16 行（画布、岛体、菜单/标题各多一份"截图读数"），当前是 2 PASS / 14 FAIL。

| 主张 | 测量方法 | 阈值 | 当前实测 | 目标卡 |
|---|---|---|---|---|
| 助记符字母数 | `kActions[]` 中 `mnemonic != 0` 的数量 + `Assets/Lang/zh-CN.json` 里 `(&X)` 标记数 | **0** | 8 + 1 = **9**（动作岛 O/E/C/A/D/T/V/S；菜单 `设置(S)...`） | C100, C106 |
| 压缩图标中心像素 = 岛底色 | 截图中「压缩」按钮图标盒中心 3×3 与岛底色通道差 > 24 的像素数 | **0 个墨点** | **9 / 9**（四条直线在中心交汇成 X） | C100 |
| 压缩率条高度 | `GalleryRenderer` 中 `cy ± N`（胶囊条上下沿） | **4 px** | **6 px** | C101 |
| 压缩率条 alpha | 填充笔刷不透明度（源码常量；无 `SetOpacity` 即 1.0） | **≤ 0.55** | **1.00** | C101 |
| 压缩率条饱和度 | `BarColorOf` 默认带的 HSV S | **≤ 0.45** | **0.738**（#34C759） | C101 |
| Day 画布色值 | `CanvasPresentedColor(Daylight)`；截图空白区 (1200,600) | **#F5F5F7 ±2/通道** | 令牌 **#F7F9FC**；截图 **#F6F8FC** | C102 |
| 岛体与画布差 | `island - canvas` 最大通道差；截图 (1550,160) vs (1200,600) | **≥ 8** | 令牌 **5**；截图 **6** | C102 |
| `Segoe UI` 字面量计数 | `Src/` 递归扫描（本次计入 115 个文件） | **0** | **21** | C103 |
| 侧栏选中角像素差 | 截图中选中胶囊内角像素 vs 中心像素 | **> 0** | **0**（硬边矩形） | C104 |
| 信息卡下划线数 | 截图 y∈[740,860] 的全宽 1 px 横线数 | **0** | **1**（y=815） | C105 |
| 菜单栏与标题栏色差 | `PaletteFor(v).background` vs 标题栏令牌；截图 (400,31) vs (400,10) | **≤ 2/通道** | **0**（令牌）/ **0**（像素） | C106 |
| `OpticsReady()` | `Src/` 中 `InitializeOptics(` 成员调用点数（0 = 链路从未运行） | **≥ 1 且运行时 true** | **0** | C107 |
| 面板随背景变色 | 背景由红变蓝时面板的 ΔR（链路未接时为平面填充） | **≥ 4** | **0**（chain off） | C107 |

## 3. 阈值为什么是这些数

- **助记符 0**：中文标签里出现一个 ASCII 字母就是第二套文字系统；全产品只有动作岛与菜单栏两处会画。
  0 是唯一可计数的目标：C100 断言角标框是"裸岛色"、C106 断言菜单项没有 `&` 或裸字母。
- **图标中心 0 墨点**：空心图标按构造不过中心；修复前的 X 是四条线穿过中心，因此 9/9。
  0 是精确值，不是容差。
- **条高 4 px**：`ui-redesign-plan §2.2` 的 4 px + 半径 2；96 DPI 下按整数 DIP 绘制，所以 ±0。
- **alpha ≤ 0.55**：计划值就是 0.55（"静音条"）。上限允许计划值、拒绝旧的 1.0，不留模糊地带。
- **饱和度 ≤ 0.45**：语义色相仍要可辨（±20°），但不能再是荧光色；#34C759 的 S=0.738 正是用户说的
  "刺眼荧光绿厚条"。这个数字由 C101 的纯逻辑套件拥有。
- **Day 画布 #F5F5F7 ±2**：三层材质（画布 / 内容板 / 悬浮岛）必须互相可辨；±2 是 8 位合成链
  （base 叠 reference，再叠 veil）的舍入预算。
- **岛体 ≥ 8**：这些亮度下 1 个 JND 约 2–3 级；取 8 才能在 100% 与 150% DPI 下都保住"岛有厚度"。
  现在是 5（令牌）/ 6（像素），所以是"塑料药丸贴在惨白纸面"。
- **`Segoe UI` 0**：全产品一套字体栈；字面量是第二个字族溜进来的唯一入口。扫描同时断言
  "走过 ≥50 个文件"，防止路径写错时得到一个舒服的 0。
- **选中角 > 0**：任何圆角胶囊在抗锯齿后角点都会与中心差至少 1 级；等于 0 就说明画的是矩形。
- **下划线 0**：卡片用留白分隔，不画表格线；截图里那条 1 px 线就是用户报告的"硬下划线"。
- **菜单/标题 ≤ 2**：两块 OS 表面由同一令牌绘制，中间只隔 1 px 边框；≤2 覆盖抗锯齿，同时拒绝
  "白条"（差 200+）。
- **`OpticsReady()` ≥ 1 调用点**：不是"有没有实现"，而是"有没有接上"。0 个调用点 = 两个 shader 从未运行。
- **面板 ΔR ≥ 4**：veil 后只剩 35% 的采样权重；红→蓝在 veil 前会移动 R 100+ 级。4 是"能归因于背景"
  的最小可测变化，低于它就是在测噪声。

## 4. 谁怎么用这张表

1. **卡片自查**：改完自己的行，跑 `pwsh -File tools/visual_baseline.ps1 -Strict`，自己那行必须 PASS。
2. **改表**：只改自己行的「当前实测」一列，并附新截图；阈值与目标卡列由本卡维护，改阈值要在
   PR 描述里说明"为什么这个数变了"。
3. **负向控制**：本卡的 `core_c108_baseline` 三条臂（`mutation.c108.scan_root|flat_day|mnemonic`）
   证明这些判定**能变红**；像素读取臂由 `tools/visual_baseline.ps1 -Control` 证明（把岛涂成画布色，
   `island_delta` 必须掉到 0 并打印 `C108 PIXEL CONTROL CAUGHT`）。
4. **红绿只认 CI**：Clean 目标只断言"测量有效"，不把未达标的行写成红测试——否则 main 永远红，
   所有人都会学会无视它。

## 5. 已知边界

- **压缩率条三行来自源码常量**，不是像素：simple.zip 根只有 1 个文件夹（压缩率 `未知`），
  列表里没有胶囊条可测。C101 自己的 Windows 套件在有文件行的帧上测像素。
- **菜单/标题在默认（浅色）外观下都是 #FFFFFF**，像素差天然为 0；C106 的窗口套件在三种主题下
  各比一次，本表只记录当前外观这一帧。
- **`OpticsReady()` 行是静态证据**（调用点数）。运行时布尔值由 C107 的 `win.c107.optics` 断言；
  链路接上之前，任何"面板随背景变色"的像素读数都只能是 0。
- **截图是客户区**：窗口外框 1600×1000，PNG 1580×980。所有像素坐标以 PNG 为准。
- **基准是"改造前"的真相**：14 行 FAIL 不是缺陷报告，是分工表。

## 6. 相关

- 基准图片与逐点坐标：[docs/assets/ui/baseline/README.md](assets/ui/baseline/README.md)
- 采集脚本：[tools/visual_baseline.ps1](../tools/visual_baseline.ps1)
- 判分二进制：[Tests/test_c108_baseline.cpp](../Tests/test_c108_baseline.cpp)、
  [Tests/suites/c108-visual-baseline.cmake](../Tests/suites/c108-visual-baseline.cmake)
- 界面规格：[docs/DESIGN.md](DESIGN.md)；走查记录：[docs/UI-AUDIT.md](UI-AUDIT.md)
