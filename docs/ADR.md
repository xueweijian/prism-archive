# 架构决策记录（ADR）与外部审阅勘误

本文件记录**已经做过的架构裁决**，以及一次外部深度审阅（2026-10-30）指出、
经我们逐条核查后的处理结论。目的只有一个：同一批坑不要踩第二次。

图例：**[已采纳]** 改代码 + TDD · **[不适用]** 核查后我们的实现本来就没这问题 ·
**[已纠正]** 原设计/注释有错，已改正 · **[降级]** 采纳审阅建议砍掉或改写承诺。

---

## ADR-001 渲染栈：纯 Win32 + DirectComposition + Direct2D，**不引入 WinUI 3 / XAML**

**结论**：`PrismArchive.exe` 的主界面使用 `IDCompositionDevice` +
每个悬浮岛一条 `CreateSwapChainForComposition`（`DXGI_ALPHA_MODE_PREMULTIPLIED`）
+ `ID2D1DeviceContext` 绘制，光学效果来自 `Assets/Shaders/LiquidGlassEffect.hlsl`
（`ps_5_0`）。**不引入 Windows App SDK / Microsoft.UI.Xaml**。

**为什么（这是审阅第 1 条的核心，也是最容易犯的错）**：

1. `Windows.UI.Composition` 的 `CompositionEffectBrush` **不支持自定义 HLSL**。
   微软自己的文档在 Win2D `PixelShaderEffect` 页明确写着
   *"Supported by Win2D but not Windows.UI.Composition"*。所以「WinUI 3 + 自定义
   HLSL 折射画刷」这个组合在物理上不存在，蓝图原稿的这一段是错的。
2. 应用**无法采样窗口背后的桌面像素**。DWM 在独立进程合成，窗口拿不到合成后的
   桌面纹理，任何"折射真实桌面壁纸"的承诺都是空的。
3. 只要拉起 `Microsoft.UI.Xaml.dll`，XAML 框架初始化本身就要 120–250 ms 和
   45–70 MB。蓝图写的"主界面冷启动 ≤25ms"在 WinUI 路线上永远不可达。

**我们因此得到的真实能力边界（不要对外夸大）**：

- ✅ 可以对**窗口内部**的内容做真实折射/色散：岛屿下方的列表、缩略图网格是我们
  自己画的，纹理就在手上。`GlassIslandHost` 的每个岛有自己的合成表面，岛内容
  由我们合成 —— 这是能做出 iOS 26 那种"水晶镇纸"观感的部分。
- ❌ 不能折射**桌面壁纸或别的应用窗口**。需要那层观感时，用系统
  `Mica Alt / DesktopAcrylicBackdrop`（窗口基底），不要把自定义 shader 往那儿塞。
- ⚠️ 跨进程读取 DWM 合成缓冲属于未文档化行为，不进产品。

**于是史诗级的性能承诺都是错的 [降级]**：

| 原承诺 | 实际情况 | 现在怎么写 |
|---|---|---|
| 主界面冷启动 ≤ 25 ms | 纯 Win32/D2D 空壳可到 20 ms；一旦加 XAML 就 120–250 ms | 目标：**主窗口首帧 ≤ 60 ms**（纯 Win32 路线），作为待测目标而非承诺 |
| 常驻内存 ≤ 24 MB | D2D + DComp + 7z.dll 实际基线更高 | 改为**实测值 + 回归上限**，见 `docs/PERF.md` |
| 折射真实桌面 | 平台不允许 | 只承诺折射窗口内内容 + 系统 Mica 基底 |

**落地校验**：`Src/PrismWin/GlassIslandHost.hpp` 只依赖
`d3d11 / dxgi1_3 / dcomp / d2d1_3`，CI 的 windows lane 链接这四个库即证明没有
偷偷引入 XAML 运行时。

---

## ADR-002 已采纳的代码级勘误（全部 TDD + 负向控制）

### [已采纳] kpidBlock 恒为 0
`PropertyId("Block", 0)` 把自己要查的 key 当作 fallback 传了进去，于是**永远返回
0** —— 每个条目都像 block 0，固实块分组静默塌陷，单趟扫描的调度基础就是错的。
标准属性在 `GetPropertyInfo` 里 `name = NULL`，所以 fallback 必须是权威 ordinal。
已按 7-Zip `PropID.h` 补 `kpidBlock = 27` / `kpidMethod = 22`。

### [已采纳] 单文件重名变成目录
单个松散文件与磁盘上已有文件重名时，计划把它改名成了
`C:/dl/pack.txt (2)/pack.txt` —— 拿文件名当目录建。现在文件保持平铺，由
`CollisionResolver` 在同级改名（`pack (2).txt`）；**真目录**仍然得到 `(2)` 后缀。

### [已采纳] Windows 文件名净化缺失
`SanitizeWindowsName` 现在处理：保留设备名（`CON/PRN/AUX/NUL/COM1-9/LPT1-9`，
带扩展名也算）、文件系统会静默吃掉的**尾部点与空格**、`:`（否则
`a.txt:hidden` 会开出 NTFS 备用数据流）、以及 `< > | ? *` 与控制字符。

### [已采纳] 密码候选列表不区分 KDF
原先所有格式都试 64 个候选。7zAES / RAR5 的密钥派生默认 `2^19 = 524,288` 轮
SHA-256，单次猜测 15–40 ms，64 个即使完美并行也要 0.5–2.5 s。现在
`BuildCandidateList` 按格式给预算：**ZipCrypto = 64**（微秒级）、
**AES-KDF = 12**；要更深必须显式传数字并显示进度。

### [已纠正] "10 GB 包内重命名 0.05 秒"
`VirtualTree.hpp` 曾写"只重写中央目录，10GB 重命名毫秒级"。**这是错的**：标准
`IOutArchive::UpdateItems()` 即使 `newData = 0 / newProps = 1`，7-Zip 也会把每个
未修改条目的**压缩流完整拷贝**进一个新归档再替换原文件。代价是一次全量顺序拷贝
（NVMe 上 2–4 GB/s，10 GB 就是几秒）。原地尾部补丁（ZIP 的 EOCD / 7z 的
`NextHeader`）理论可行，但属于高风险格式特化特性，**明确排除在 MVP 之外**。

### [已采纳] 密码正则吞掉右方括号
字符类里带了 `[` `]`，导致 `[密码:abc123]` 把结尾的 `]` 当成密码字符捕获。

---

## ADR-003 核查后**不适用**于本实现的审阅意见

### [不适用] `ISequentialOutStream` 的 IID 末位
审阅说应为 `...-00020000` 而我们可能写成了 `00010000`。**核查：我们的值本来就是
对的** —— `IID_ISequentialOutStream` = `{23170F69-40C1-278A-0000-000300020000}`，
`IID_ISequentialInStream` = `{...000300010000}`（`7-Zip CPP/7zip/Guid.txt`：
family `03`，成员 `02` = 出、`01` = 入）。而且我们连 IID 都没手抄错
——`IInArchive` 的 `Data4[5] = 0x60` 这个坑在 M1 就付过学费。
教训仍然是：**不要手推 GUID**，要么查 `Guid.txt`，要么直接 include 7-Zip 官方头。

### [不适用] 右键菜单"悬停"事件假设
我们的实现从来没依赖"悬停触发 `GetState`"。`ContextMenuModel` 是在
**`GetState(selection)`** 里构建并缓存菜单模型（`EnumSubCommands()` 只渲染缓存），
`GetState` 本来就是建菜单时调用的，这与审阅指出的事实一致，我们踩的是正确的路。
蓝图原稿里"悬停自动贴边浮窗"的交互 **不采用** —— Win11 的右键菜单跑在独立 XAML
宿主里，Win32 侧追坐标不可靠。改为显式菜单项「👁️ 快速预览」，点击后弹独立窗口。

### [部分采纳] 拖拽通道 A（`WindowFromPoint` + `IShellWindows` 反查路径）
审阅建议砍掉。**采纳其结论的一半**：坐标嗅探在 Win11 多标签、桌面 `WorkerW`、
UAC 提权不一致的场景下拿错路径的概率高，**不作为主路径**。但"直接解压到目标目录"
这个能力本身有价值，保留为**受控路径**：只有当目标窗口能通过
`IShellWindows → IShellBrowser → IFolderView → IPersistFolder2::GetCurFolder`
**确定性地**解析出物理路径、且磁盘空间足够时才启用；任何一步不确定就回退到标准的
`CFSTR_FILEDESCRIPTORW` + `CFSTR_FILECONTENTS` 内存流。
审阅提到的"固实包乱序/多次 `GetData` 导致反复从头解压"是真实风险 —— 固实块的
`IStream` 供给必须走我们已有的单趟 sweep 缓存（`SolidSweepSession` + `ThumbStore`
的同一套块内顺序解压），不能每条 `Read` 都重新开块。

---

## ADR-004 需要保留的老坑（前序里程碑付过的学费）

- 7-Zip handler 的 `Open` 返回 **`S_OK` 才算打开**；`S_FALSE` = "不是我的格式，
  试下一个"。`SUCCEEDED(S_FALSE)` 是 TRUE —— 曾因此让 ZIP handler 霸占 `.7z`
  文件，catalog 静默为空。
- 候选 handler 之间必须 `Seek(0)` 归位：失败的 `Open` 会留在文件中间。
- 7z **固实模式会重排条目**（相似文件分组进同一字典），解压顺序 ≠ 打包顺序，
  断言必须按集合而非顺序。
- `IWICBitmap` **没有 `SetPixels`**；`CopyPixels` 是**往外读**。要写像素得走
  `Lock(WICBitmapLockWrite)` + 直接写缓冲。曾经因为反了方向，夹具 PNG 全透明，
  整条"解码"管线交出全零缩略图，而所有计数都对。
- ctest 必须用 `PASS_REGULAR_EXPRESSION` 钉住成功标记 —— 静默 SKIP 不能算过。
---

## ADR-005 ASan 覆盖范围：**已评估，暂不采纳收窄**（2026-11-01）

**提案**：Windows 门禁 lane 是全仓库 ASan 构建（476 秒，其中约 300 秒是给 110 个 target
加 sanitizer 编译）。曾提议把 ASan 收窄到高风险模块（引擎桥接 / 流 / 安全 / 并发），
其余用 Release 编译走 sccache 缓存，预计门禁从 476 秒降到约 200 秒、合并吞吐翻倍。

**裁决：不采纳**（用户明确决定，2026-11-01）。

**理由**：
1. ASan 在一个晚上救过三次，而且三次都不在"你事先以为高风险"的模块里：
   C14 的 heap-use-after-free（设置序列化顺序）、C5 的 AMSI 缓冲越界、
   C11 的并发交叠。按模块划分覆盖范围，等于赌"下一个 bug 会出现在我猜的地方"。
2. 门禁的价值在于**无差别**：一旦按模块豁免，任何一次"这次应该没事"的判断都变成
   没有兜底的判断。
3. 省下的时间可以用零风险手段补回来（并行 ctest、文档改动跳过、批量合并），
   这些已经在 2026-11-01 落地（`5edd80f`）。

**代价（记录在案）**：合并门禁每轮约 8 分钟，吞吐约 4–5 个 PR/小时。
如果将来要提速，优先级顺序是：**路径过滤构建 > 批量合并 > 拆分慢测试**，
**不要**再回到"收窄 sanitizer"这条路，除非有数据证明某模块的 ASan 收益为零。
