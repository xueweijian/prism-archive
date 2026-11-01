# TESTING.md — 把「测试很硬」变成数字（卡片 C23）

> 一句话结论（2026-11-01 本机实测）：
> **行覆盖 76.4%（10533/13781）、函数覆盖 79.2%（1174/1483），54 个模块**；
> **88 个条目 = 87 个变异体 + 1 个干净对照，分布在 23 个 suite 文件，全部可枚举、可校验**；
> 一轮自动负向控制里 **变异体全部 CAUGHT、对照全部 CONTROL-PASS**（注入证据 in-build + 套件红 + 干净对照绿）；
> 故意把宏拼错 → 工具报 **NOT-IN-EFFECT**，不是绿。

本卡只新增工具与注册，不改产品代码、不改 ci.yml、不改 PROGRESS/ROADMAP/CONTRACTS。

## 一、这张卡回答什么

1. **每个模块到底跑到多少行/函数？** —— `tools/cov/coverage.py`，一条命令自动选引擎，产出每模块行/函数覆盖率。
2. **负向控制有多少、还连着吗、跑起来是不是真红？** —— `tools/mutation/mutant_matrix.py` 枚举 + 校验，`tools/mutation/negctl.py` 自动跑一轮。
3. **「注入没生效」这种假绿怎么被机器抓住？** —— 见第四节，故意拼错宏的真实二进制，判定必须是 NOT-IN-EFFECT。

## 二、覆盖率

### 2.1 一条命令（自动选引擎）

```powershell
python tools/cov/coverage.py run --build-dir E:\build\cov-c23 `
    --exclude "bench|snapshot|mutation\.installer" `
    --json out/coverage.json --markdown out/coverage.md
```

引擎选择（`--engine auto`）：

| 平台 | 顺序 | 说明 |
|---|---|---|
| Windows | OpenCppCoverage → clang-cl `-fprofile-instr-generate -fcoverage-mapping` + llvm-profdata/llvm-cov | MSVC 没有行覆盖模式；clang-cl 与 VS 同装，编译同一份源码。OpenCppCoverage 装上时无需重建 |
| Linux | gcovr → lcov --capture → 裸 gcov 扫描 | 三者都读 `--coverage` 构建产物，只是读法不同 |

引擎缺失时**大声 SKIP 并返回 3**，绝不静默成功。`coverage.py selftest` 用固定 LCOV 样本校准合并/归属/策略（CONTRACTS D4：先校准工具，再相信数字）。

### 2.2 本机实测（clang-cl 22.1.8，Windows，50 个测试二进制）

命令：`--filter "^(core\.|win\.(write_archive|engine_bridge|volumes|com_qi_contract|settings_file|crash_dump|pwd_probe|mkdir_matrix|vtree_exec|tree_nav|app_lifecycle|motw_stamp|dragout|amsi_guard|dnd_seek|sidebar_view|sparse_registrar))"`。
`bench|snapshot|installer` 被排除（见 2.4 的读法）。

| 模块 | 行覆盖 | 函数覆盖 | 判定 | 说明 |
|---|---|---|---|---|
| `PrismCore/ArchiveCatalog` | 25.0% (11/44) | 33.3% (3/9) | LOW/WATCH | 目录读取：25%，A1/A3 PROPID 语义只能由真 7z 集成测试覆盖 |
| `PrismCore/ArchiveTreeView` | 95.2% (300/315) | 94.6% (35/37) | OK |  |
| `PrismCore/CharsetRepair` | 83.8% (217/259) | 82.3% (14/17) | OK |  |
| `PrismCore/CommandPalette` | 90.9% (209/230) | 85.7% (18/21) | OK |  |
| `PrismCore/CompressionPlan` | 90.7% (254/280) | 69.6% (16/23) | OK |  |
| `PrismCore/ContextMenuModel` | 90.5% (38/42) | 66.7% (2/3) | OK |  |
| `PrismCore/DragDrop` | 76.7% (520/678) | 78.6% (44/56) | OK | C5：拒绝必带 DenyReason；128 组合 |
| `PrismCore/ExtractScheduler` | 83.2% (227/273) | 72.5% (29/40) | OK |  |
| `PrismCore/FilterModel` | 100.0% (79/79) | 100.0% (5/5) | OK |  |
| `PrismCore/Gallery` | 90.6% (145/160) | 95.2% (40/42) | OK |  |
| `PrismCore/GlassIsland` | 97.1% (66/68) | 91.7% (11/12) | OK |  |
| `PrismCore/HistoryStack` | 100.0% (48/48) | 100.0% (13/13) | OK |  |
| `PrismCore/ListLayout` | 83.8% (67/80) | 91.7% (11/12) | OK |  |
| `PrismCore/ListSortModel` | 85.2% (271/318) | 82.3% (28/34) | OK |  |
| `PrismCore/Log` | 92.1% (420/456) | 83.3% (55/66) | OK | D25 脱敏 + 限流；变异体 no_redaction 必须红 |
| `PrismCore/NewFolder` | 84.4% (76/90) | 87.5% (7/8) | OK |  |
| `PrismCore/OpenFlow` | 76.7% (198/258) | 75.8% (25/33) | OK | 打开流程判定（WrongPassword vs NeedPassword），负向控制见 mutation_openflow.sh |
| `PrismCore/PasswordRing` | 78.7% (177/225) | 91.7% (33/36) | OK | 口令环：脱敏在格式化前（D3 第一次突变没生效就是在这里） |
| `PrismCore/PasswordVault` | 63.5% (66/104) | 50.0% (9/18) | LOW/WATCH | 口令库：63.5%，DPAPI 边界 |
| `PrismCore/PathSafety` | 96.3% (211/219) | 100.0% (21/21) | OK | Zip-Slip / UNC / 设备名边界；绕过即写入任意路径 |
| `PrismCore/Preview` | 82.5% (52/63) | 80.0% (4/5) | OK |  |
| `PrismCore/ProgressHud` | 85.0% (51/60) | 80.0% (4/5) | OK |  |
| `PrismCore/SecurityGuard` | 83.6% (239/286) | 73.0% (27/37) | OK | AMSI/MOTW 策略：拒绝码必须沿调用链回溯（D11） |
| `PrismCore/SettingsStore` | 90.1% (812/901) | 93.4% (71/76) | OK |  |
| `PrismCore/Shortcuts` | 66.7% (86/129) | 75.0% (9/12) | LOW/WATCH | 快捷键：66.7%，保留键/缩放钳制（C8 变异体） |
| `PrismCore/SidebarModel` | 89.5% (591/660) | 89.7% (61/68) | OK |  |
| `PrismCore/SmartExtractor` | 96.3% (131/136) | 100.0% (5/5) | OK |  |
| `PrismCore/SolidSweep` | 95.6% (43/45) | 100.0% (11/11) | OK |  |
| `PrismCore/SpringAnimator` | 97.4% (38/39) | 88.9% (8/9) | OK |  |
| `PrismCore/ThemeTokens` | 92.1% (197/214) | 87.9% (29/33) | OK |  |
| `PrismCore/ThumbStore` | 100.0% (36/36) | 100.0% (9/9) | OK |  |
| `PrismCore/VirtualTree` | 90.3% (176/195) | 84.2% (16/19) | OK |  |
| `PrismCore/VolumePlan` | 83.2% (283/340) | 84.2% (16/19) | OK |  |
| `PrismWin/ArchiveWriter` | 74.6% (520/697) | 90.0% (54/60) | LOW/WATCH | 写入路径：kpidIsDir/kpidSize/SetProperties 错一位就是错误归档（C9 两个变异体守着） |
| `PrismWin/CompanionWindow` | 10.6% (11/104) | 27.3% (3/11) | LOW/WATCH | 伴随窗口：10.6%，窗口生命周期（D20）相关分支未被本 run 触达 |
| `PrismWin/CrashDump` | 83.9% (234/279) | 86.4% (19/22) | OK | D25：脱敏必须发生在格式化之前，任何 sink 都不得见明文 |
| `PrismWin/DpapiVault` | 0.0% (0/88) | 0.0% (0/8) | LOW/WATCH | 凭据加密：本 run 0%（对应套件未纳入），任何一行未跑都是风险 |
| `PrismWin/DragOutSession` | 77.7% (143/184) | 60.0% (12/20) | OK |  |
| `PrismWin/GalleryRenderer` | 48.6% (217/446) | 62.5% (10/16) | LOW/WATCH | 渲染器：48.6%，像素级契约（A5/C1）只能由快照套件证明 |
| `PrismWin/GalleryWindow` | 82.3% (445/541) | 87.8% (65/74) | OK |  |
| `PrismWin/GlassIslandHost` | 0.2% (1/409) | 1.9% (1/52) | LOW/WATCH | Liquid Glass 宿主：本 run 只跑到 0.2%（win.gallery_snapshot 被排除），这块只能靠快照套件 |
| `PrismWin/MotwGuard` | 66.5% (131/197) | 73.7% (14/19) | OK |  |
| `PrismWin/PasswordProbe` | 100.0% (8/8) | 100.0% (1/1) | OK |  |
| `PrismWin/PrismDragDecisionBridge` | 71.2% (331/465) | 72.2% (39/54) | OK |  |
| `PrismWin/PrismEngineBridge` | 84.0% (630/750) | 92.4% (97/105) | LOW/WATCH | 7z 引擎唯一出口：Open/GetStream/UpdateItems 的错误路径静默返回时，产品会“成功”产出空归档；本 run 有 120 行未命中，错误分支是首要嫌疑 |
| `PrismWin/PrismVirtualDragDrop` | 33.3% (49/147) | 46.1% (12/26) | LOW/WATCH | 虚拟拖拽：33.3%，CFSTR_FILECONTENTS 路径 |
| `PrismWin/ReaderPool` | 64.6% (93/144) | 58.3% (14/24) | LOW/WATCH | D24 并发契约：峰值必须在 7z 回调内会合；本 run 只跑到 64.6% |
| `PrismWin/SettingsFile` | 67.4% (232/344) | 85.0% (17/20) | LOW/WATCH | 设置落盘：67.4%，并发读写（C14 变异体） |
| `PrismWin/SidebarView` | 84.7% (211/249) | 73.7% (14/19) | OK |  |
| `PrismWin/SolidSweeper` | 70.2% (111/158) | 87.0% (20/23) | OK |  |
| `PrismWin/SparsePackageRegistrar` | 51.7% (166/321) | 65.2% (15/23) | LOW/WATCH | MSIX 稀疏注册：51.7%，安装期关键路径 |
| `PrismWin/VirtualTreeExecutor` | 69.3% (305/440) | 86.2% (25/29) | LOW/WATCH | 虚拟树执行器：69.3%，写入/删除回滚路径 |
| `PrismWin/VolumeStream` | 75.5% (343/454) | 85.0% (51/60) | LOW/WATCH | 分卷流：75.5%，D23 卷回调错误路径 |
| `PrismWin/Win32Transcoder` | 65.4% (17/26) | 66.7% (2/3) | OK |  |
| **TOTAL** | **76.4%** (10533/13781) | **79.2%** (1174/1483) | | |



### 2.3 低覆盖但重要（watchlist）

策略文件 `tools/cov/coverage_policy.json` 分两类：
- `critical + min_line`：低于门限 → **LOW/CRITICAL**（阻断干净结论）。
- `watch + watch_max`：失败模式静默（错误路径 / 安全 / 并发），门槛高于全局 → **LOW/WATCH**（报告，不阻断）。

| 模块 | 行覆盖 | 未命中行 | 说明 |
|---|---|---|---|
| **PrismCore/ArchiveCatalog** | 25.0% (11/44) | 33 | 目录读取：25%，A1/A3 PROPID 语义只能由真 7z 集成测试覆盖 |
| **PrismCore/PasswordVault** | 63.5% (66/104) | 38 | 口令库：63.5%，DPAPI 边界 |
| **PrismCore/Shortcuts** | 66.7% (86/129) | 43 | 快捷键：66.7%，保留键/缩放钳制（C8 变异体） |
| **PrismWin/ArchiveWriter** | 74.6% (520/697) | 177 | 写入路径：kpidIsDir/kpidSize/SetProperties 错一位就是错误归档（C9 两个变异体守着） |
| **PrismWin/CompanionWindow** | 10.6% (11/104) | 93 | 伴随窗口：10.6%，窗口生命周期（D20）相关分支未被本 run 触达 |
| **PrismWin/DpapiVault** | 0.0% (0/88) | 88 | 凭据加密：本 run 0%（对应套件未纳入），任何一行未跑都是风险 |
| **PrismWin/GalleryRenderer** | 48.6% (217/446) | 229 | 渲染器：48.6%，像素级契约（A5/C1）只能由快照套件证明 |
| **PrismWin/GlassIslandHost** | 0.2% (1/409) | 408 | Liquid Glass 宿主：本 run 只跑到 0.2%（win.gallery_snapshot 被排除），这块只能靠快照套件 |
| **PrismWin/PrismEngineBridge** | 84.0% (630/750) | 120 | 7z 引擎唯一出口：Open/GetStream/UpdateItems 的错误路径静默返回时，产品会“成功”产出空归档；本 run 有 120 行未命中，错误分支是首要嫌疑 |
| **PrismWin/PrismVirtualDragDrop** | 33.3% (49/147) | 98 | 虚拟拖拽：33.3%，CFSTR_FILECONTENTS 路径 |
| **PrismWin/ReaderPool** | 64.6% (93/144) | 51 | D24 并发契约：峰值必须在 7z 回调内会合；本 run 只跑到 64.6% |
| **PrismWin/SettingsFile** | 67.4% (232/344) | 112 | 设置落盘：67.4%，并发读写（C14 变异体） |
| **PrismWin/SparsePackageRegistrar** | 51.7% (166/321) | 155 | MSIX 稀疏注册：51.7%，安装期关键路径 |
| **PrismWin/VirtualTreeExecutor** | 69.3% (305/440) | 135 | 虚拟树执行器：69.3%，写入/删除回滚路径 |
| **PrismWin/VolumeStream** | 75.5% (343/454) | 111 | 分卷流：75.5%，D23 卷回调错误路径 |



**PrismEngineBridge 为什么单独点名**：整体 84.0% 看着不差，但 750 行里 **120 行没跑过**，而它的失败模式是「返回 S_OK 但什么都没做」——外面看不出来。错误分支（UpdateItems/GetStream/QI/卷回调失败）是首要嫌疑。这条不进阻断门限（会把整条 lane 变成红），但每次覆盖率报告都会把它列出来，下一步补测试有明确靶子。

同理：`PrismWin/DpapiVault 0%`（凭据加密，对应套件未纳入本次 run）、`PrismCore/ArchiveCatalog 25%`（A1/A3 的 PROPID 语义只能靠真 7z 集成测试）、`PrismWin/ReaderPool 64.6%`（D24 并发契约）。

### 2.4 读法（不要过度解读）

- 覆盖率是**本次选中的套件**跑出来的并集。被排除的 bench/snapshot 不会拉低数字，但也意味着 **0% 可能是「没跑」而不是「没测」**；watchlist 的备注已标明这一点。
- 仪器化构建用 `-DPRISM_WARNINGS_AS_ERRORS=OFF`：clang 特有的告警（如 COM 测试里的 `-Wmicrosoft-exception-spec`）不是产品缺陷，而门禁构建仍然是 `/WX`。**测量构建不是门禁构建**，两者不能混谈。
- Linux 的 gcov/gcovr/lcov 路径已实现并用 selftest 校准解析器；本机没有 Linux，**未做端到端实测**，接 CI 前先在 runner 上跑一次。

## 三、变异矩阵

### 3.1 枚举 + 校验 + 一轮自动化

```powershell
# 1) 枚举 + 校验（D26 违规、宏/源/对照缺失都会红）
python tools/mutation/mutant_matrix.py --check

# 2) 一轮：注入证明 -> 跑 -> 断言红 -> 干净对照必须绿
python tools/mutation/negctl.py run --build-dir E:\build\wt-c23 --json-out out/negctl.json

# 3) 把实测状态回填进表（给 docs/PR 用）
python tools/mutation/mutant_matrix.py --markdown out/matrix.md --results out/negctl.json
```

判定不是「ctest 绿了」：
- **注入证据**：宏必须出现在该 target 的编译命令行（`ninja -t commands`）**或**二进制打印自己的 `MUTANT ACTIVE` 横幅；
- **必须红**：`WILL_FAIL` 形态要求退出码非 0；`MARKER` 形态（D26 安全形状）要求合取 marker（红 + 注入计数 > 0）；
- **干净对照**：同一 target 不带宏必须绿（否则永久红的套件会让所有 WILL_FAIL 看起来都「被抓住」）。

### 3.1.1 对照条目（C22 教训）

C22 的 `mutation.fuzz.clean_build_unarmed` 是**故意的反例**：同一个源文件、**不注入任何宏**，用来证明「没注入时不会出现 MUTANT ACTIVE 横幅」。矩阵因此区分两种角色：

| 角色 | 判定规则 |
|---|---|
| MUTANT | 必须有注入宏；否则 PROBLEM（保持严格） |
| CONTROL（测试名/目标名含 `clean` / `unarmed` / `control` 词元） | 允许无宏，并自动与**同 source 的变异体**配对（`paired_with`）；**带宏反而报错**（既当运动员又当裁判） |

这条规则本身有回归自检：`mutant_matrix.py --selftest` 在合成树上跑三个场景——合法对照（允许且配对）、**自称对照但实际带宏（必须报错）**、非对照缺宏（必须报错）。已注册为 `c23.mutation.matrix.selftest`。

### 3.2 全部 88 个条目（87 变异体 + 1 对照；状态为实测）

| 测试 | 注入宏 / 注入点 | 预期红断言 | 注册方式 | 状态 |
|---|---|---|---|---|
| `mutation.dnd_seek.forward_only` | `PRISM_DND_MUTANT_SEEK_FORWARD_ONLY` | 实测首红: `small` | WILL_FAIL | CAUGHT |
| `mutation.dnd_seek.ignore_budget` | `PRISM_DND_MUTANT_SEEK_IGNORE_BUDGET` | 实测首红: `restart` | WILL_FAIL | CAUGHT |
| `mutation.dnd_seek.lookback_off_by_one` | `PRISM_DND_MUTANT_SEEK_LOOKBACK_OFF_BY_ONE` | 实测首红: `rewind` | WILL_FAIL | CAUGHT |
| `mutation.dnd_seek.ring_keep_skipped` | `PRISM_DND_MUTANT_RING_KEEP_SKIPPED` | 实测首红: `cursor:` | WILL_FAIL | CAUGHT |
| `mutation.dnd_seek.serve_unresident` | `PRISM_DND_MUTANT_SERVE_UNRESIDENT` | 实测首红: `cursor:` | WILL_FAIL | CAUGHT |
| `mutation.drag_drop.ignore_space` | `PRISM_DND_MUTANT_IGNORE_SPACE` | 实测首红: `not` | WILL_FAIL | CAUGHT |
| `mutation.drag_drop.trust_all` | `PRISM_DND_MUTANT_TRUST_ALL` | 实测首红: `UNC` | WILL_FAIL | CAUGHT |
| `mutation.app_lifecycle.no_scan_abort` | `PRISM_MUTANT_SCAN_ABORT_IGNORED` | 实测首红: `closing` | WILL_FAIL | CAUGHT |
| `mutation.volume_plan.accept_unsorted` | `PRISM_VOLUME_MUTANT_ACCEPT_UNSORTED` | 实测首红: `explicit` | WILL_FAIL | CAUGHT |
| `mutation.volume_plan.ignore_gaps` | `PRISM_VOLUME_MUTANT_IGNORE_GAPS` | 实测首红: `numeric` | WILL_FAIL | CAUGHT |
| `mutation.volume_plan.last_volume_optional` | `PRISM_VOLUME_MUTANT_LAST_VOLUME_OPTIONAL` | 实测首红: `split` | WILL_FAIL | CAUGHT |
| `mutation.extract_scheduler.ignore_priority` | `PRISM_C11_MUTANT_IGNORE_PRIORITY` | priority test goes red | WILL_FAIL | CAUGHT |
| `mutation.extract_scheduler.share_slot` | `PRISM_C11_MUTANT_SHARE_SLOT` | slot-invariance test goes red | WILL_FAIL | CAUGHT |
| `mutation.extract_scheduler.unbounded_backlog` | `PRISM_C11_MUTANT_UNBOUNDED_BACKLOG` | backpressure test goes red | WILL_FAIL | CAUGHT |
| `mutation.reader_pool.shared_handler` | `PRISM_C11_MUTANT_SHARED_HANDLER` | 实测首红: `reader_pool.leases_own_distinct_handlers` | WILL_FAIL | CAUGHT |
| `mutation.sidebar.no_focus_wrap` | `PRISM_SIDEBAR_MUTANT_NO_FOCUS_WRAP` | 实测首红: `TestFocusTraversesSectionsAndCycles` | WILL_FAIL | CAUGHT |
| `mutation.sidebar.no_recent_cap` | `PRISM_SIDEBAR_MUTANT_NO_RECENT_CAP` | 实测首红: `TestRecentCapacityEvictsTheOldest` | WILL_FAIL | CAUGHT |
| `mutation.sidebar.no_recent_dedupe` | `PRISM_SIDEBAR_MUTANT_NO_RECENT_DEDUPE` | 实测首红: `TestRecentDeduplicatesAcrossSeparatorsAndCase` | WILL_FAIL | CAUGHT |
| `mutation.sidebar.view_no_cull` | `PRISM_SIDEBAR_MUTANT_VIEW_NO_CULL` | 实测首红: `TestScrollCullsRowsButKeepsContentHeight` | WILL_FAIL | CAUGHT |
| `mutation.sidebar.view_no_indent` | `PRISM_SIDEBAR_MUTANT_VIEW_NO_INDENT` | 实测首红: `TestSectionStackAndRowBoxes` | WILL_FAIL | CAUGHT |
| `mutation.filter_model.glob_star_stops_at_slash` | `PRISM_C13_MUTANT_GLOB_STAR_STOPS_AT_SLASH` | '*' crossing a separator goes red | WILL_FAIL | CAUGHT |
| `mutation.filter_model.selection_by_position` | `PRISM_C13_MUTANT_SELECTION_BY_POSITION` | selection-by-index goes red | WILL_FAIL | CAUGHT |
| `mutation.list_sort.dirs_interleaved` | `PRISM_C13_MUTANT_DIRS_INTERLEAVED` | folders-first goes red | WILL_FAIL | CAUGHT |
| `mutation.list_sort.lexicographic_names` | `PRISM_C13_MUTANT_LEXICOGRAPHIC_NAMES` | natural order goes red (file10 < file2) | WILL_FAIL | CAUGHT |
| `mutation.list_sort.unstable_sort` | `PRISM_C13_MUTANT_UNSTABLE_SORT` | stability property goes red | WILL_FAIL | CAUGHT |
| `mutation.list_sort.width_bypasses_min` | `PRISM_C13_MUTANT_WIDTH_BYPASSES_MIN` | column minimum width goes red | WILL_FAIL | CAUGHT |
| `mutation.settings_file.inplace_save` | `PRISM_C14_MUTANT_NONATOMIC_SAVE` | 实测首红: `atomic` | WILL_FAIL | CAUGHT |
| `mutation.settings_store.drop_unknown_keys` | `PRISM_C14_MUTANT_DROP_UNKNOWN_KEYS` | 实测首红: `unknown` | WILL_FAIL | CAUGHT |
| `mutation.settings_store.ignore_validation` | `PRISM_C14_MUTANT_IGNORE_VALIDATION` | 实测首红: `unknown` | WILL_FAIL | CAUGHT |
| `mutation.settings_store.skip_migration` | `PRISM_C14_MUTANT_SKIP_MIGRATION` | 实测首红: `migration:` | WILL_FAIL | CAUGHT |
| `mutation.theme.circle_exponent` | `PRISM_C15_MUTANT_CIRCLE_EXPONENT` | 实测首红: `squircle:` | WILL_FAIL | CAUGHT |
| `mutation.theme.linear_raw` | `PRISM_C15_MUTANT_LINEAR_RAW` | 实测首红: `WCAG:` | WILL_FAIL | CAUGHT |
| `mutation.theme.no_fallback` | `PRISM_C15_MUTANT_NO_FALLBACK` | 实测首红: `3.3:` | WILL_FAIL | CAUGHT |
| `mutation.theme.swap_theme` | `PRISM_C15_MUTANT_SWAP_THEME` | 实测首红: `3.3:` | WILL_FAIL | CAUGHT |
| `mutation.crash_dump.no_redaction` | `PRISM_C16_MUTANT_NO_REDACTION` | 实测首红: `crash.writer_emits_a_real_minidump_and_a_redacted_tail` | WILL_FAIL | CAUGHT |
| `mutation.log.no_redaction` | `PRISM_C16_MUTANT_NO_REDACTION` | 实测首红: `log.wire_format_is_byte_stable_and_redacted` | WILL_FAIL | CAUGHT |
| `mutation.compress_dialog.tar_encrypt_allowed` | `PRISM_CD_MUTANT_TAR_ENCRYPT_ALLOWED` | 实测首红: `state:` | WILL_FAIL | CAUGHT |
| `mutation.compress_dialog.zip_solid` | `PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED` | 实测首红: `state:` | MARKER | CAUGHT |
| `mutation.compress_dialog.zip_solid_allowed` | `PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED` | 实测首红: `state:` | WILL_FAIL | CAUGHT |
| `mutation.i18n.no-fallback` | `PRISM_C19_MUTANT_NO_FALLBACK` | 实测首红: `lookup:` | WILL_FAIL | CAUGHT |
| `mutation.i18n.no-fallback.injected` | `PRISM_C19_MUTANT_NO_FALLBACK` | — | MARKER | CAUGHT |
| `mutation.i18n.no-missing-tracking` | `PRISM_C19_MUTANT_NO_MISSING_TRACKING` | 实测首红: `lookup:` | WILL_FAIL | CAUGHT |
| `mutation.i18n.no-missing-tracking.injected` | `PRISM_C19_MUTANT_NO_MISSING_TRACKING` | — | MARKER | CAUGHT |
| `mutation.i18n.no-placeholder` | `PRISM_C19_MUTANT_NO_PLACEHOLDER` | 实测首红: `catalog:` | WILL_FAIL | CAUGHT |
| `mutation.i18n.no-placeholder.injected` | `PRISM_C19_MUTANT_NO_PLACEHOLDER` | — | MARKER | CAUGHT |
| `mutation.dragout.getdata_notimpl` | `PRISM_C20_MUTANT_GETDATA_NOTIMPL` | 实测首红: `expected true: hr == S_OK` | WILL_FAIL | CAUGHT |
| `mutation.dragout.size_lie` | `PRISM_C20_MUTANT_SIZE_LIE` | 实测首红: `expected true: stat.cbSize.QuadPart == expected[lindex].size` | WILL_FAIL | CAUGHT |
| `mutation.preview_plan.keep_long_lines` | `PRISM_PREVIEW_MUTANT_KEEP_LONG_LINES` | 实测首红: `text` | WILL_FAIL | CAUGHT |
| `mutation.preview_plan.utf8_always` | `PRISM_PREVIEW_MUTANT_UTF8_ALWAYS` | 实测首红: `magic` | WILL_FAIL | CAUGHT |
| `mutation.fuzz.accept_all` | `PRISM_FUZZ_MUTANT_ACCEPT_ALL` | 实测首红: `invariants:` | WILL_FAIL | CAUGHT |
| `mutation.fuzz.clean_build_unarmed` | （无：对照，故意不注入） | — | CONTROL(no injection) | CONTROL-PASS |
| `mutation.fuzz.injection_armed` | `PRISM_FUZZ_MUTANT_ACCEPT_ALL` | — | MARKER | CAUGHT |
| `mutation.fuzz.valid_rejected` | `PRISM_FUZZ_MUTANT_VALID_REJECTED` | 实测首红: `negative` | WILL_FAIL | CAUGHT |
| `mutation.release.dropshellextclassic` | `-Mutate DropShellExtClassic` | — | MARKER | CAUGHT |
| `mutation.release.extra7zdll` | `-Mutate Extra7zDll` | — | MARKER | CAUGHT |
| `mutation.release.sumgap` | `-Mutate SumGap` | — | MARKER | CAUGHT |
| `mutation.app_wiring.no_motw` | `PRISM_C25_MUTANT_NO_MOTW` | 实测首红: `wiring.extracted_files_inherit_the_zone_identifier_verbatim` | MARKER | CAUGHT |
| `mutation.app_wiring.shared_reader` | `PRISM_C25_MUTANT_SHARED_READER` | 实测首红: `wiring.sweep_and_extract_own_distinct_handlers` | MARKER | CAUGHT |
| `mutation.extract_failure.ignore_opresult` | `PRISM_C26_MUTANT_IGNORE_OPRESULT` | 实测首红: `unsupported` | MARKER | CAUGHT |
| `mutation.installer.envundersoftware` | `-Mutate EnvUnderSoftware` | — | MARKER | not-run |
| `mutation.installer.nobackup` | `-Mutate NoBackup` | — | MARKER | not-run |
| `mutation.installer.norestore` | `-Mutate NoRestore` | — | MARKER | not-run |
| `mutation.installer.rebackup` | `-Mutate Rebackup` | — | MARKER | not-run |
| `mutation.installer.skipdefaulticon` | `-Mutate SkipDefaultIcon` | — | MARKER | not-run |
| `mutation.installer.unquotedverb` | `-Mutate UnquotedVerb` | — | MARKER | not-run |
| `mutation.sparse_registrar.accept_unverified` | `PRISM_MUTANT_ACCEPT_UNVERIFIED` | 实测首红: `a` | WILL_FAIL | CAUGHT |
| `mutation.sparse_registrar.manifest_clsid` | `PRISM_MUTANT_MANIFEST_WRONG_CLSID` | 实测首红: `package` | WILL_FAIL | CAUGHT |
| `mutation.sparse_registrar.manifest_external_content` | `PRISM_MUTANT_MANIFEST_DROP_EXTERNAL_CONTENT` | 实测首红: `package` | WILL_FAIL | CAUGHT |
| `mutation.security.ignore_chunk_overlap` | `PRISM_SEC_MUTANT_IGNORE_CHUNK_OVERLAP` | 实测首红: `amsi:` | WILL_FAIL | CAUGHT |
| `mutation.security.ignore_disk_fraction` | `PRISM_SEC_MUTANT_IGNORE_DISK_FRACTION` | 实测首红: `bomb` | WILL_FAIL | CAUGHT |
| `mutation.security.ignore_per_file_ratio` | `PRISM_SEC_MUTANT_IGNORE_PER_FILE_RATIO` | 实测首红: `bomb` | WILL_FAIL | CAUGHT |
| `mutation.security.never_stamp` | `PRISM_SEC_MUTANT_NEVER_STAMP` | 实测首红: `motw:` | WILL_FAIL | CAUGHT |
| `mutation.security.unavailable_is_allow` | `PRISM_SEC_MUTANT_UNAVAILABLE_IS_ALLOW` | 实测首红: `amsi:` | WILL_FAIL | CAUGHT |
| `mutation.bench_memory.block_buffer` | `PRISM_BENCH_MUTANT_BLOCK_BUFFER` | — | WILL_FAIL | not-run |
| `mutation.bench_scale.temp_write` | `PRISM_BENCH_MUTANT_TEMP_WRITE` | — | WILL_FAIL | not-run |
| `mutation.bench_startup.timer_scale` | `PRISM_BENCH_MUTANT_TIMER_SCALE` | — | WILL_FAIL | not-run |
| `mutation.history.no_dedupe` | `PRISM_HISTORY_MUTANT_NO_DEDUPE` | 实测首红: `history:` | WILL_FAIL | CAUGHT |
| `mutation.history.off_by_one` | `PRISM_HISTORY_MUTANT_OFF_BY_ONE` | 实测首红: `history:` | WILL_FAIL | CAUGHT |
| `mutation.mkdir.allow_reserved` | `PRISM_NEWFOLDER_MUTANT_ALLOW_RESERVED` | 实测首红: `mkdir:` | WILL_FAIL | CAUGHT |
| `mutation.palette.longer_first` | `PRISM_PALETTE_MUTANT_LONGER_FIRST` | 实测首红: `palette:` | WILL_FAIL | CAUGHT |
| `mutation.palette.no_boundary` | `PRISM_PALETTE_MUTANT_NO_BOUNDARY` | 实测首红: `palette:` | WILL_FAIL | CAUGHT |
| `mutation.zoom.no_clamp` | `PRISM_ZOOM_MUTANT_NO_CLAMP` | 实测首红: `zoom:` | WILL_FAIL | CAUGHT |
| `mutation.compress_plan.no_clamp` | `PRISM_CP_MUTANT_NO_CLAMP` | 实测首红: `level` | WILL_FAIL | CAUGHT |
| `mutation.compress_plan.no_input_clamp` | `PRISM_CP_MUTANT_NO_INPUT_CLAMP` | 实测首红: `dictionary` | WILL_FAIL | CAUGHT |
| `mutation.compress_plan.no_thread_clamp` | `PRISM_CP_MUTANT_NO_THREAD_CLAMP` | 实测首红: `threads:` | WILL_FAIL | CAUGHT |
| `mutation.compress_plan.solid_any_format` | `PRISM_CP_MUTANT_SOLID_ANY_FORMAT` | 实测首红: `solid:` | WILL_FAIL | CAUGHT |
| `mutation.write_archive.dir_as_file` | `PRISM_CW_MUTANT_DIR_AS_FILE` | the callback tells 7z folders are files | MARKER | CAUGHT |
| `mutation.write_archive.no_size` | `PRISM_CW_MUTANT_NO_SIZE` | the callback refuses to answer kpidSize | MARKER | CAUGHT |

未跑的 9 个：6 个 installer 运行时变异体（`-Mutate` 走 pwsh 安装脚本）和 3 个 bench 变异体（长基准，见 2.4）。它们的注册仍由 `matrix --check` 静态校验；其余 **78 个变异体 + 1 个对照全部实测**。

### 3.3 状态含义

| 状态 | 含义 | 处置 |
|---|---|---|
| CAUGHT | 注入证明 + 套件红 + 对照绿 | 期望状态 |
| NOT-IN-EFFECT | 宏没进编译（拼错/改名）或没 banner 且套件绿 | **假绿**，必须修 |
| GREEN-HOLE | 注入生效但套件没红 | 测试不锋利 |
| RED-WITHOUT-INJECTION | 红了但没有注入证据（可能是无关崩溃） | 不能算抓住 |
| SKIPPED | 返回 SKIP_RETURN_CODE | 可见的跳过，不算通过 |
| CONTROL-PASS / CONTROL-SKIP / CONTROL-FAIL | 干净对照的三种结局 | FAIL 才阻断 |

## 四、D3 的自动化：注入没生效必须是 NOT-IN-EFFECT

`Tests/c23_mutation_probe.cpp` 一份源码编三次：

| 二进制 | 宏 | 预期 |
|---|---|---|
| `c23_mutant_probe_clean` | 无 | 绿，无横幅 |
| `c23_mutant_probe_active` | `PRISM_C23_MUTANT_PROBE=1` | 横幅 + 一条断言红 |
| `c23_mutant_probe_typo` | `PRISM_C23_MUTANT_PROBE_TYPO=1`（**故意拼错**） | 行为等同干净版 —— 必须被判为「未生效」 |

真实输出（本机）：

```
C23 NEGCTL PROBE: NOT-IN-EFFECT  exe=c23_mutant_probe_typo.exe macro=PRISM_C23_MUTANT_PROBE injection=ABSENT rc=0
  evidence: no injection evidence and the suite stayed green
  expectation: NOT-IN-EFFECT -> MET

C23 NEGCTL PROBE: CAUGHT  exe=c23_mutant_probe_active.exe macro=PRISM_C23_MUTANT_PROBE injection=in-build rc=1
  evidence: banner present and exit code 1
  expectation: CAUGHT -> MET
```

三条 ctest 把这件事钉住（`Tests/suites/c23-coverage-mutation.cmake`）：
`c23.mutation.negctl.control` / `.caught` / `.dead_injection`。
它们**只设 PASS_REGULAR_EXPRESSION、不设 WILL_FAIL**——正是 CONTRACTS D26 要求的形状：marker 是「判定结果」本身，退出码仍是 0，二者合取。

另外 `matrix --check` 静态抓 D26 的两种事故：`WILL_FAIL + PASS_REGULAR_EXPRESSION`（永远红）、`WILL_FAIL + SKIP_RETURN_CODE`（SKIP 变假绿）。当前 88 个条目 0 违规（这条检查本身已进 ctest）。

## 五、CI 接入建议（由集成者决定，本卡不改 ci.yml）

ctest 已经自带 7 条 `c23.*` 自检（matrix / matrix selftest / 三个 negctl / 快测 round / coverage selftest），**推上去就生效，不需要改 workflow**。若要采集数字：

Linux lane（2 步，~1 分钟）：
```yaml
- name: coverage (gcovr)
  run: |
    python tools/cov/coverage.py run --build-dir build/cov --configure \
      --engine gcovr --json coverage.json --markdown coverage.md
- uses: actions/upload-artifact@v4
  with: { name: coverage, path: coverage.* }
```

Windows lane（仪器化单独构建，~1.5 分钟）：
```yaml
- name: coverage (clang-cl + llvm-cov)
  run: |
    python tools/cov/coverage.py run --build-dir E:\build\cov --engine clang \
      --exclude "bench|snapshot|mutation\.installer" --markdown coverage.md --json coverage.json
```

稳定性提醒：覆盖率数字会随「本次跑了哪些套件」漂移，所以策略里 critical 门槛不要卡到 0.1%；watchlist 只报告不阻断。真正阻断的应该是 `matrix --check` 与 negctl 的 FAIL_VERDICTS。

## 六、契约对照

| 契约 | 这里怎么满足 |
|---|---|
| D1 成功标记是判定的一部分 | 所有 c23 测试都是「退出码 0 + PASS_REGULAR_EXPRESSION」；SKIP 不被当作通过 |
| D3 负向控制且验证注入真生效 | negctl 的注入证据（编译命令行 + 横幅）+ 拼错宏的 NOT-IN-EFFECT 自检 |
| D4 工具先校准 | `coverage.py selftest` 用固定 LCOV 样本；negctl 用三个真实二进制 |
| D26 WILL_FAIL 与正则不叠加 | matrix 静态检查 + 本卡所有注册只用 marker 合取 |
| D7 不做索引手术 | 全部工具为整文件生成/JSON 输出，不改产品源码 |
| D9 独立工作树 | 全程在 `E:\wg\c23` |
| B9 /WX- | 探针目标在 MSVC 下显式 `/WX-` |
| 注册规则 | 只新增 `Tests/suites/c23-coverage-mutation.cmake`，未改 Tests/CMakeLists.txt |

## 七、文件

- `tools/cov/coverage.py` —— 自动选引擎、跑 ctest、合并 LCOV、生成报告
- `tools/cov/coverage_policy.json` —— critical / watch 门槛与「为什么重要」
- `tools/mutation/mutant_matrix.py` —— 枚举 + 校验 + 表格
- `tools/mutation/negctl.py` —— 一轮负向控制（注入 → 跑 → 红 → 对照）
- `Tests/suites/c23-coverage-mutation.cmake` —— 6 条 ctest 自检
- `Tests/c23_mutation_probe.cpp` —— D3 校准夹具（clean / active / typo）
