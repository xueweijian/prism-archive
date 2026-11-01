# PrismArchive 总进度与作战计划

> 更新：2026-11-01（B1 引擎执行器落地）。本文档是**进度的唯一权威口径**（ROADMAP 的 checkbox 是按批次的执行清单，可能滞后）。
> 口径原则：**写了 ≠ 能用；CI 编译过 ≠ 真机可用**。每项进度都按"可交付成品"的标尺打分。

## 一、总进度：≈58%

| 口径 | 进度 | 说明 |
|---|---|---|
| 代码模块覆盖 | ~75% | 蓝图 11 章核心模块均有实现 |
| 验证深度 | ~74% | 决策层极深（20 套件/566+ 检查/突变负向控制）；执行层因 B1 落地（`win.vtree_exec`/`win.mkdir_matrix`/`win.com_qi_contract` 对真 7z.dll）显著加强 |
| **可交付成品** | ~35% | 无 exe、无安装器、未在真 explorer 验证 |

加权主数字 **58%**（决策 20% · 引擎 20% · Shell 15% · UI 30% · 产品化 15%）。

> 2026-11-01：批次 B1（VirtualTree 执行器）合入 main（PR #9），引擎桥接层 65% → 78%。可交付成品仍是 35% —— 决定它的是 WinMain/安装器/真 explorer 验证，跟引擎无关。

## 二、分维度明细

### 1. 决策核心层 PrismCore —— 85%
**已交付**（全部 TDD + 负向控制）：ArchiveCatalog · CharsetRepair（GBK/Big5/SJIS/EUC-KR）· ContextMenuModel · Gallery（视口虚拟化/LRU/固实规划/QuickLook 会话）· GlassIsland（五寄存器 cbuffer 契约）· ListLayout（四列+语义胶囊条）· PasswordVault+PasswordRing（确定性排序/探针/预算/脱敏）· PathSafety（ZipSlip/碰撞/Windows 名净化）· Preview · ProgressHud · SecurityGuard · SmartExtractor（五分支，两处真 bug 已修）· SolidSweep（零拷贝契约）· SpringAnimator（位级确定性）· ThumbStore · VirtualTree（CoW 计划）· DragDrop（三通道+理由枚举）
**缺**：IORING 落盘调度决策、MoTW/AMSI 决策接主流程、密码试错的进度反馈模型细化

### 2. 引擎桥接层 —— 78%
**已交付**：Open（S_OK 纪律+候选回绕）· 目录（PROPID 集合发现，kpidBlock=27）· ExtractToMemory · 智能解压落盘 · 固实单趟扫描（ExtractBatch）· GBK 修复集成 · **`IOutArchive::UpdateItems` 真接线**（`VirtualTreeExecutor.hpp`：CommitTree / 目录提交 / 包内重命名 / 诚实成本模型 / 重开验证闸门）
**缺**：压缩/打包方向 · 大页

> **B1 落地（2026-11-01，PR #9）**：真因是 `DiskOutStream` 继承了 `IOutStream`、实现了 `SetSize`，却没在 `QueryInterface` 里应答 `IID_IOutStream` —— 7-Zip 只用 QI 拿可选接口（`7zUpdate.cpp` `Z7_DECL_CMyComPtr_QI_FROM(IOutStream, ...)`，失败即 `E_NOTIMPL`），对象在 C++ 层面完美、在 COM 层面自称"我不是"。**18 轮 CI 全打在这上面**（CONTRACTS D10-D13）。
> 顺带挖出三个产品级 bug：`PropertyId` 用属性发现集合做门 → **所有 7z 目录被读成 0 字节文件**（D16）；`~SevenZipLibrary` 卸 7z.dll 毒化下次加载；`SmartExtractCallback::GetStream` 一个引用给两个主人 → heap-use-after-free（D15）。
> 空目录提交按 CONTRACTS A10 虚拟化（shipped 7z.dll 对新建空目录节点行为不定，目录结构由子路径隐含）。Windows lane 现带 `/fsanitize=address`。

### 3. Shell 集成 —— 50%
**已交付**：双栈 DLL（Win11 IExplorerCommand / Win10 IContextMenu）编译链接+CI、菜单模型 TDD、manifest 由 check_manifest.py 每次校验、SparsePackageRegistrar 代码在
**缺**：**从未在真 explorer 注册运行过**、菜单延迟实测、伴生预览窗真跑

### 4. UI 层 —— 35%
**已交付**：GalleryRenderer（网格/列表/文档占位/岛模型/真缩略图，像素级验证）· 弹簧 · GalleryWindow 骨架+Ctrl+1/2 状态 · GlassIslandHost（编译级）· **真缩略图管线端到端**（真固实 7z → 单趟 → WIC → 瓦片）
**缺**：**WinMain（整个 Src/ 无入口）**、窗口消费渲染器（仍是旧 DrawTile）、鼠标/键盘交互、QuickLook 真窗口、DComp/HLSL 真渲染

### 5. 产品化 —— 10%
**缺**：安装器/签名分发、多语言、图标、设置持久化、无障碍回退、性能达标数字（现仅 1 条：20 项 sweep 1.12x / 13.1MB 峰值）、4GB 零 %TEMP% 实测、万项目录实测

## 三、剩余作战计划（四批次，每批有硬验收）

### 批次 A：装配出 PrismArchive.exe（UI 主线，解锁"能跑"）　**★ 增量 1-4 已落地；事故恢复完成（2026-10-31 凌晨）**

> **PrismArchive.exe 第一次存在**：CI 构建 + 上传 artifact（`PrismArchive-exe`，PE 头验证 x64 GUI 子系统）。GalleryWindow 已改为纯宿主（全部像素走 GalleryRenderer::Scene，双绘制路径 drift 隐患清除）；缩略图经互斥指针交换从扫描线程交接（只有 UI 线程碰可见 store，LRU recency 单线程化 by construction）。装配契约在 Main.cpp 文件头注。
> **增量 2-4**：单击选择（网格/列表命中测试）+ 双击智能解压真接线（SmartExtractionPlanner → ExtractWithPlan → HUD 进度）+ Quick Look（空格开玻璃卡、Esc 关、流式徽标）+ 键盘导航（方向键按视口几何步进、Home/End、Enter 激活）+ ArchiveTreeView 文件夹导航决策件（6 例 TDD：目录子项/目录先序/面包屑链/UpFrom/名字前缀防误配）。
> **事故与恢复（CONTRACTS D6-D9）**：过时树推送丢 CMake 注册 + Main.cpp 索引手术切坏 + NowMs 私有访问，main 连红三轮；以远端 tarball 为底重建、B3 密码环头从分支找回、Main.cpp 整文件重写、preflight 新增 B7 门禁（负向控制双向验证）。教训全部入册。
1. `App/Main.cpp`：WinMain + 消息循环 + COM/WIC/D2D 初始化顺序（已有 GlassIslandHost 模式可复用）
2. GalleryWindow 换血：删旧 DrawTile，全量消费 GalleryRenderer（Scene 驱动）
3. 三事件源接线：catalog 载入 → 视口；sweep 进度 → ProgressHud；空格 → QuickLookSession
4. 鼠标：滚轮弹簧、命中测试选中、双击智能解压；键盘：Ctrl+1/2、方向键、空格/Esc
5. （进行中）ArchiveTreeView 接窗：当前文件夹状态 + 面包屑点击 + 双击目录钻入 + Backspace 上级
**验收**：exe 双击打开 .7z/.zip → 网格/列表切换 → 真缩略图 → 弹簧滚动 → QuickLook 卡片 → 文件夹导航。CI 快照对比无回归。

### 批次 B：引擎执行层补全（并行线 1）
1. ~~VirtualTree 执行器：UpdateItems 真接线（诚实成本模型：全量拷贝语义，UI 必须显示进度）~~ **✅ 已落地（2026-11-01，PR #9）**——CommitTree 端到端：计划→备份→UpdateItems→重开验证→原子替换；矩阵 8 变体验证目录节点；`win.vtree_exec` / `win.mkdir_matrix` / `win.com_qi_contract` 三套件进主线 CI
2. ~~拖拽流随机访问：DelayedExtractStream 的 Seek 真实现（依赖 7z.dll 的块内随机解码）~~ **✅ 已落地（2026-11-01，PR #13）**——纯状态机 `DelayedSeekCursor`（Planner 纯函数：CursorOnly/LookbackHit/RestartDecode，双预算重启前拒绝）+ COM 包装 + 真 7z.exe 5 文件固实包 e2e；`core.test_dnd_seek`（22 例）+ 4 个变异构建 + `win.dnd_seek` 进主线 CI
3. 密码试错执行器：消费 PasswordRing 决策，并发探针 + "匹配中 3/12" 进度回调
**验收**：包内重命名在真 7z.dll 上落盘成功且可再打开（✅）；Explorer 从虚拟流真拉到文件；AES 包试错出真实结果。

### 批次 C：Shell 真机验证 + 产品化（并行线 2）
1. MSIX Sparse 真注册（CI runner 上 AddPackageByUriAsync 真跑 + 证据截图/日志）
2. 右键延迟实测 lane（< 12ms 目标，出数字）
3. MoTW 继承 + AMSI 预检接主流程（SecurityGuard 已有守卫）
4. 图标资源、设置持久化、en/zh 资源表、安装器（先进 Inno Setup 或 MSIX 直发）
**验收**：真 Win11 explorer 右键菜单可见可用；带 MoTW 的包解压有告警。

### 批次 D：性能达标 + 发布（最后）
1. 基准矩阵：大固实块（256MB）/ 万项目录 / 4GB 零 %TEMP% / 冷启动 / 空闲内存
2. 畸形语料 10 000 回归（CI 下载不入库）
3. 蓝图 ch7.2 表格逐项填数字；v1.0 tag + 发布页
**验收**：ch7.2 每一格要么有实测数字要么标注"未达标+原因"。

## 四、并行工作模式

主线（我）+ ≤3 subagent。批次 A 期间：subagent 1 承接 B1（VirtualTree 执行器）、subagent 2 承接 B3（密码执行器）、subagent 3 承接 C2（延迟实测 lane）。批次边界重新分派。

## 五、已知债务（不挡路，但要记账）

- ~~DelayedExtractStream::Seek 只挪游标（拖拽真拉取前必须补）~~ **已补（2026-11-01，PR #13）**：真 e2e 在 7z.dll 上验证冷/热流、窗口边界、双预算拒绝
- PrismVirtualDragDrop 的 GetDataHere 保守拒绝
- 部分蓝图性能承诺已按 ADR.md 降级（25ms 冷启动等），ch7.2 验收以 ADR 为准
- ROADMAP 部分 checkbox 滞后于实际（以本文档为准，分批同步）
