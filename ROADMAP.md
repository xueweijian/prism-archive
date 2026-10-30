# PrismArchive 研发路线（对应蓝图第 7.3 节 16 周 / 四阶段）

拆解原则：**每个里程碑都必须有一条能在 CI 上跑出来的判据**（数字或红灯），
没有判据的条目不进主线。

## M0 · 仓库与验证骨架（第 1 周）— 已完成

- [x] 公开仓库、MIT 许可、CMake 3.20+ / C++20 工程骨架
- [x] 双轨 CI：Linux 编 PrismCore + ctest（含 asan/ubsan）；Windows MSVC 编 Windows 层
- [x] Windows 集成测试真加载 `7z.dll`（找不到就显式 SKIP，不假绿）
- [x] HLSL 由 `fxc.exe` 编译校验（`tools/hlsl/compile_hlsl.py`）
- [x] 零依赖测试框架 `Tests/mini_test.hpp`（不引 GoogleTest，手机端也能跑）

判据：CI 两个 lane 全绿 + Windows lane 打印出 7z.dll 的格式表数量。

## M1 · PrismCore 决策逻辑（第 2–4 周，对应蓝图 Phase 1）

- [x] 智能解压五分支决策树（A/B/B+/D/E）+ 零拷贝路径重写
- [x] Zip Slip 两阶段守卫、压缩炸弹滑动窗口熔断
- [x] CJK 历史乱码检测与修复（GB18030 / Big5 / Shift-JIS / EUC-KR）
- [x] 免解压 CoW 虚拟目录树 → `IOutArchive::UpdateItems` 元数据计划
- [x] 密码环嗅探策略 + 保险库接口（Windows 侧 DPAPI 已实现）
- [ ] 畸形归档语料回归：10 000 个真实样本（走 CI，下载到 runner，不入库）
- [ ] 密码环并发试错器（15 ms / 64 候选，需实测数字）

判据：每个规则都有正例 + 反例（负向控制）单测；畸形样本语料 lane 红灯可复现。

**已知限制（不掩盖）**：韩文 EUC-KR 与 GB2312 常用区高度重叠，缺少频率表时会出现并列，
此时 `RepairResult.ambiguous = true`，UI 必须提供编码切换器（蓝图第 7.1 节正是这么要求的）。
后续接入 `uchardet` / ICU 可消除并列。

## M2 · 双栈右键引擎与内联预览（第 5–8 周，对应 Phase 2）— 进行中

已完成：MSIX sparse manifest（XML 由 CI 校验）、`PrismShellExtClassic.dll`（Win10 IContextMenu，渲染 ContextMenuModel）、ContextMenuModel 跨平台菜单模型。
待做：SparsePackageRegistrar（PackageManager API）、伴生预览窗（CompanionWindow）、IExplorerCommand 的阴影菜单渲染对齐新模型。

- [x] `IExplorerCommand` + `IEnumExplorerCommand` 级联菜单（编译通过）
- [ ] MSIX Sparse Package 清单与签名注册（`AppxManifest.xml`）
- [ ] 伴生无焦点 Liquid Glass 内联预览窗（DirectComposition，`WS_EX_NOACTIVATE`）
- [ ] Win10 经典 `IContextMenu` 兼容层
- [ ] 右键呼出延迟实测 < 12 ms（CI 上跑 benchmark lane，出数字）

## M3 · 主界面与流式画廊（第 9–13 周，对应 Phase 3）— 进行中

已完成（决策层全部 TDD，三 lane 全绿）：
- [x] 三阶段流体进度 HUD（M2 已交付 `ProgressHud`，TDD 8 例）
- [x] 画廊决策核心 `Gallery.hpp`：ViewportWindow（钳位/单调虚拟化 + 命中测试）、ThumbnailLru（字节预算真 LRU、超预算响亮拒绝）、固实块扫描规划（≤256MB 单趟扫描、边界含、focus 优先确定性顺序）、QuickLookSession（空格切换/Esc 关闭/跳过不可预览项/超限标记 Streaming）——TDD 12 例
- [x] 弹簧物理 `SpringAnimator.hpp`：蓝图两档预设（k=420/c=28 ζ≈0.68 灵动回弹；k=280/c=34 ζ≈1.0 无过冲），固定子步辛积分器——同 tick 序列位级一致；settle 吸附让 UI 定时器可休眠；中途换目标保留动量——TDD 6 例
- [x] `GalleryWindow.hpp`：D2D/DirectWrite 渲染器只消费上述决策（网格瓦片 + 标题先行；环形缓冲缩略图下一增量）；m3_compile_check 拉进 Windows 测试二进制

待做：引擎侧固实块单趟扫描执行器（7z.dll → 环形缓冲 → WIC 解码管线）、主窗口文件列表模式、零拷贝拖拽通道决策、4GB 零 %TEMP% 实测

## M4 · 安全、性能与发布（第 14–16 周，对应 Phase 4）

- [ ] MoTW 全链路继承 + AMSI 预检（守卫代码已就绪，待接入主流程）
- [ ] `IORING` 小文件并发落盘（目标：8 万小文件 42 s → 6.8 s）
- [ ] 大页内存（`SetLargePageMode`）+ 长路径 `\\?\` 全链路
- [ ] 冷启动 ≤ 25 ms、空闲内存 ≤ 25 MB 的 benchmark lane
- [ ] 便携版与安装程序、多语言本地化
