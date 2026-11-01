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

### 批次 A：装配出 PrismArchive.exe（UI 主线，解锁"能跑"）　**★ 1-5 全部落地（PR #3/#10/#11）**

> **PrismArchive.exe 第一次存在**：CI 构建 + 上传 artifact（`PrismArchive-exe`，PE 头验证 x64 GUI 子系统）。GalleryWindow 已改为纯宿主（全部像素走 GalleryRenderer::Scene，双绘制路径 drift 隐患清除）；缩略图经互斥指针交换从扫描线程交接（只有 UI 线程碰可见 store，LRU recency 单线程化 by construction）。装配契约在 Main.cpp 文件头注。
> **增量 2-4**：单击选择（网格/列表命中测试）+ 双击智能解压真接线（SmartExtractionPlanner → ExtractWithPlan → HUD 进度）+ Quick Look（空格开玻璃卡、Esc 关、流式徽标）+ 键盘导航（方向键按视口几何步进、Home/End、Enter 激活）+ ArchiveTreeView 文件夹导航决策件（6 例 TDD：目录子项/目录先序/面包屑链/UpFrom/名字前缀防误配）。
> **事故与恢复（CONTRACTS D6-D9）**：过时树推送丢 CMake 注册 + Main.cpp 索引手术切坏 + NowMs 私有访问，main 连红三轮；以远端 tarball 为底重建、B3 密码环头从分支找回、Main.cpp 整文件重写、preflight 新增 B7 门禁（负向控制双向验证）。教训全部入册。
1. `App/Main.cpp`：WinMain + 消息循环 + COM/WIC/D2D 初始化顺序（已有 GlassIslandHost 模式可复用）
2. GalleryWindow 换血：删旧 DrawTile，全量消费 GalleryRenderer（Scene 驱动）
3. 三事件源接线：catalog 载入 → 视口；sweep 进度 → ProgressHud；空格 → QuickLookSession
4. 鼠标：滚轮弹簧、命中测试选中、双击智能解压；键盘：Ctrl+1/2、方向键、空格/Esc
5. ~~ArchiveTreeView 接窗：当前文件夹状态 + 面包屑点击 + 双击目录钻入 + Backspace 上级~~ **✅ 已落地（2026-11-01，PR #10）**——`win.tree_nav` 进主线 CI；过程中修出两个真实产品缺陷（懒测量返回过期几何、点导航条空白会把选择清空）
**验收**：exe 双击打开 .7z/.zip → 网格/列表切换 → 真缩略图 → 弹簧滚动 → QuickLook 卡片 → 文件夹导航。CI 快照对比无回归。

### 批次 B：引擎执行层补全（并行线 1）　**★ 1-3 全部落地（PR #9/#13/#11）**
1. ~~VirtualTree 执行器：UpdateItems 真接线（诚实成本模型：全量拷贝语义，UI 必须显示进度）~~ **✅ 已落地（2026-11-01，PR #9）**——CommitTree 端到端：计划→备份→UpdateItems→重开验证→原子替换；矩阵 8 变体验证目录节点；`win.vtree_exec` / `win.mkdir_matrix` / `win.com_qi_contract` 三套件进主线 CI
2. ~~拖拽流随机访问：DelayedExtractStream 的 Seek 真实现（依赖 7z.dll 的块内随机解码）~~ **✅ 已落地（2026-11-01，PR #13）**——纯状态机 `DelayedSeekCursor`（Planner 纯函数：CursorOnly/LookbackHit/RestartDecode，双预算重启前拒绝）+ COM 包装 + 真 7z.exe 5 文件固实包 e2e；`core.test_dnd_seek`（22 例）+ 4 个变异构建 + `win.dnd_seek` 进主线 CI
3. ~~密码试错执行器：消费 PasswordRing 决策，并发探针 + "匹配中 3/12" 进度回调~~ **✅ 已落地（2026-11-01，PR #11）**——`OpenFlow`（Idle/Opening/NeedPassword/Probing/Opened/Exhausted）+ `pwd::Probe` 探针策略 + `PowerShellProbe`/`PasswordProbe.hpp`；36 用例 / 201 checks，明确避开 D16 陷阱
**验收**：包内重命名在真 7z.dll 上落盘成功且可再打开（✅）；Explorer 从虚拟流真拉到文件；AES 包试错出真实结果。

### 批次 C：Shell 真机验证 + 产品化（并行线 2）1. MSIX Sparse 真注册（CI runner 上 AddPackageByUriAsync 真跑 + 证据截图/日志）
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
- ~~`Src/App/Main.cpp` 的激活处理器用 detached 线程触碰 UI 线程状态~~ **已修（2026-11-01，PR #15）**：
  三处缺陷——① 在 detached 线程里调 `window.Hud().OnStart()`，而 `hud_` 无锁、由 WndProc 在 UI 线程喂
  ② `window` 是 `wWinMain` 栈对象，detached 线程按引用持有 → 解压中途关窗口 = UAF
  ③ `extracting` 永不复位 → 线程走不到完成路径就永久锁死后续双击。
  改法是 API 形状：`PostHudStart/PostStatus/PostProgress` 只 PostMessage 不碰成员，worker 改为 joinable 并 join。
  同时补上 `tools/win_stub/`（此前从未推上去，导致 `check_windows_tu.sh` 在 main 上一直坏）并把该门禁接进 CI，
  新增 preflight **B9**（静态锁死"worker 不得碰 UI 状态"，已做负向控制：注入原 bug 精确报三条）
- **`sweep` 与 `extract` 两个工作线程共用同一个 `ArchiveReader`**（PR #15 查出，未修）：7z.dll 的 handler 不是线程安全的，7-Zip 自己的规则是"每个并发操作一个 handler"。两个线程现在都已 join，**生命周期**问题解决了，但如果扫描还在跑（大归档要数秒）时用户双击解压，就是真实交叠。修法需要第二个 reader/handle，是实打实的一块工作，不塞进本次
- PrismVirtualDragDrop 的 GetDataHere 保守拒绝
- 部分蓝图性能承诺已按 ADR.md 降级（25ms 冷启动等），ch7.2 验收以 ADR 为准
- ROADMAP 部分 checkbox 滞后于实际（以本文档为准，分批同步）

---

## 六、2026-11-01 深夜：24 卡并行冲刺（14 张已合入 main）

> 本节由集成者（父 agent）写。口径不变：**写了 ≠ 能用**；下面每一条都指向一个已合并的 PR 或一份实测数字。

### 6.1 已合入的能力（按用户能感知的顺序）

| 能力 | 卡 / PR | 证据 |
|---|---|---|
| 开窗即渲染、关窗即退出 | C1 #21 | `win.app_lifecycle` 20 checks；关窗 85ms、退出码 0；根因是 `GalleryRenderer::Initialize()` 从未被调用，首帧走空 vtable → UI 线程死在 `DispatchMessageW`。**此前 exe 从未成功渲染过一帧** |
| 7z.dll 跨盘解析 | C1 #21 | env → exe 目录 → 注册表 `InstallDir`（HKLM/HKCU）→ 两个 Program Files；本机 E 盘安装从「弹找不到」变可用 |
| 分卷读取/生成 | C10 #27 | 四族卷名、缺卷/乱序在**引擎之前**拒绝；7z.exe 造 3 卷 → 逐项 sha256 全等；顺手修了 `GetStream` 返回 `S_OK`+空流指针的真 bug |
| 并发解压 | C11 #26 | `ReaderPool` + `ExtractScheduler`；同包并发 sweep+extract 不串数据（173 checks，`peak==4` 在 7z 回调内部会合）；4 个变异全红 |
| 右键双栈 | C4 #18 | Win10 经典菜单在真 shell 可见（5 项与模型逐字一致，注销即消失）；Win11 MSIX 真跑到 `AddPackageByUriAsync`（本机缺 ROOT 信任证书，按 D1 显式 SKIP） |
| 安装与关联 | C3 #23/#25 | 免管理员 HKCU 安装器、`.7z/.zip/.rar` 关联、多尺寸图标、`cmake --install` 扁平布局 |
| 安全链路 | C5 #17 | MoTW 逐文件继承（ADS 写读回）、AMSI 三态（Unavailable 不静默放行）、炸弹熔断双条件；5 个编译期变异全红 |
| 液态玻璃 | C6 #24 | 真 `ID2D1Effect` 效果图 + DComp 每岛合成；`bandDiff 0.86`，注入 `u_iorStrength=0` 后 3 条断言全红 |
| 侧边栏 / 排序过滤 / 快捷键 | C12 #29、C13 #28、C8 #20 | 942 行侧栏模型、6 键多键排序 + 精确压缩率 + 6 个变异、蓝图 9.1 六组快捷键 + 13 条旅程 |
| 门禁与 CI | C2 #22 | `local_gates.ps1` 一条命令 8 步；CI 压到 2 job/run（Linux 64s / Windows 133s） |

### 6.2 实测性能（`docs/PERF.md`，卡片 C7 #19）

| 指标 | 实测 | 判定 |
|---|---|---|
| 首帧 | **22.7 ms** | 达标（ADR-001 目标 60ms） |
| 进程→main | 313.6 ms | 未达标（ADR 已把 25ms 降级） |
| 常驻内存 | 58.4 MB | 未达标（目标 24–25MB；玻璃层 +11MB） |
| 关闭延迟 | 153 ms（空闲）/ 517 ms（编译风暴） | C1 的 500ms 预算：空闲达标、满载超线 |
| 1 万条目 Open | 7.5 ms | — |
| 4 GiB Open | **78.5 s** | 未达标；约等于整文件顺序扫描（ZIP 候选先试），魔数前置筛选是最可能的解 |
| 10 万行滚动 | 7.9 ms/帧，p95 15.6 ms ≈ 126 FPS | 120FPS 在 p95 未达标 |

### 6.3 进度重估（本节口径）

| 维度 | §一 旧值 | 现在 | 依据 |
|---|---|---|---|
| 可交付成品 | ~35% | **~55%** | exe 能开能关、能装能关联、右键可见；缺压缩与装配 |
| 加权主数字 | 58% | **~68%** | Shell 集成 50→70（真机可见）、UI 35→55（侧栏/排序/快捷键决策层 + 玻璃真渲染）、产品化 10→40（安装器/关联/图标） |

### 6.4 新增债务（当期记账，不掩盖）

- **4 GiB 归档 Open 78.5s**：魔数前置筛选未做，是「右键 12ms」与「大包可用性」的共同拦路虎。
- **常驻内存 58.4MB**：玻璃层（DComp 岛 + 效果图）贡献 +11MB，与 ADR 的 25MB 目标冲突，需在 ADR 里重新裁决或做分层惰性创建。
- **`Log.hpp` 非线程安全**：与 `ExtractScheduler` 同约定，接多线程前必须由 owner 串行化或加锁。
- **`ReaderPool` / `MoTW` / `AMSI` / `Shortcuts` / 侧栏 / 排序都还没接进 `Main.cpp`**：这正是波 2 装配卡（C25）在做的事。
- `GlassIslandHost` 的 API 变动会打断直接引用它的外部 TU（C7 的 bench 已解耦，其他测试同样不要绑）。

### 6.5 并行工作模式（已实证）

24 张卡 / 8 个并发槽位 / 每卡独立 worktree + 分支；构建走 3 槽位编译锁，编译与测试以 GitHub Actions 为权威（2 job、约 2.5 分钟），本地只 `Build-Only` 自己的 target。
合入门禁：每个 PR 必须 CI 绿 + preflight PASS + 新守卫有负向控制（D3）+ 会 SKIP 的测试钉 `PASS_REGULAR_EXPRESSION`（D1）。

---

## 七、夜间冲刺（2026-11-01 13:00 → 11-02 凌晨）

**规模**：46 个 PR 合入 main，**+81,618 / −1,546 行**；测试 80 个文件 / 31 个 suite；累计合并 55 个 PR。
全程 8 个 subagent 并发、一卡一 worktree、CI 为权威。

### 7.1 这一夜修掉的产品缺陷（不是新功能）

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | `SetOperationResult` 的 opResult 被忽略 | 不支持的方法被当成"成功解出 0 字节" | 记录 opResult + 可执行文案 + 扩展名→handler 表 |
| 2 | 7z/tar 在签名失配时扫全文件 | 4 GiB 打开 78.5 s | 魔数嗅探路由：**读字节 4.3 GB → 1.6 KB**（按字节断言） |
| 3 | 共享 handler 下并行抽取静默丢数据 | 可能解出空文件 | 原子 CAS 守卫 + `filesWritten==0` 不算成功 |
| 4 | 夹具固定目录（干净/变异二进制共用） | CI 偶发空字节与崩溃 | 唯一 RAII 目录 `prism_<card>_p<pid>_<n>` |
| 5 | `Tr::Or` 不记 miss | 缺 key 回退英文，`MissingKeys()` 却是空的 | 补 11 key + 源码级 key 审计 |
| 6 | fallback 双重转义 | 用户看到字面 `\u5DF2` | 句子搬进可测函数 + 单转义 UCN |
| 7 | 变异裁决在 teardown 后打印 | 崩溃吞掉 CAUGHT，CI 报"正则未找到" | **D27**：首红即打印并退出 |
| 8 | 岛重叠 / 文本画骨架 / 不重排 | 界面像半渲染的 mock | `WindowLayout` 纯几何 + `CellKindFor` + 33828 条断言 |

### 7.2 可测量而非可感觉的验收方式

- **按字节**：4 GiB 打开读 1.6 KB；cpio 2052 B、cab 1649 B、chm 17928 B（< 64 KiB 门禁）
- **按会计**：内存用活 COM 对象推导，不用工作集（C33）
- **按矩形**：同级矩形不相交（容差 0）、条目必须落在内容区（V1）
- **按像素**：骨架像素 0 vs 对照 3 单元 / 24034 像素（检测器先自校准）

### 7.3 被数据推翻的三个断言（我自己说错的）

1. "4 GiB 慢是因为先试 ZIP handler" → 错。Zip 只读尾部 73–183 ms；真因是 7z 签名失配 + tar 逐块走完。
2. "套件按构造独立，可 `ctest -j 8`" → 错。夹具共用；修完还有两个"用经过时间当预算"的用例在忙时必红。
3. "25 MB 常驻可以做到" → 错。玻璃图活体单项 +40.33 MB；ADR-006 改为 96/40 MB 分档，观感优先。

### 7.4 状态与剩余

- **已合**：引擎（+9 格式）、安全、i18n、设置、侧栏、嗅探、安装器、报表 lane、密码 UI、对话框玻璃壳
- **在途**：动作岛（V2）、会话恢复（C46）、缩略图流水线（C45）、变异裁决谓词化（C44 后续）、布局 P0（V1 待合）
- **需你本人**：MSIX 证书信任、真机右键浮层验收、截图入 `docs/assets/`
- **已知不达标**：120 FPS（WARP 上限）、4K 多显示器 DPI、MSIX 真注册