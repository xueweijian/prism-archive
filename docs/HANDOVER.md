# HANDOVER — 接手 PrismArchive（2026-11-01）

> 面向：在电脑上接手本项目的开发者（也可能是未来的我）。
> 读法：**第 1 节告诉你现在能不能跑、哪些是跑过的、哪些不是。** 然后第 3 节是环境配置，
> 第 4 节是这份代码的规矩（读之前不要改代码），第 6 节是接下来的活。
>
> 本文所有"现状"都是 2026-11-01 现查的，不是凭记忆。凡是我没验证的，都写了"未验证"。

---

## 1. 三十秒版：项目在哪、能不能用

**项目**：PrismArchive —— Windows 压缩包管理器。7-Zip 引擎（真 `7z.dll`）× Bandizip 交互 ×
iOS 26 Liquid Glass 视觉。MIT，公开仓库。

- 仓库：`https://github.com/xueweijian/prism-archive`
- 主分支：`main`
- 规模：21 个 PrismCore 头（header-only、纯 C++20）+ 19 个 CMake 测试目标 + 34 个测试源文件
- CI：三条 lane（Linux gcc / Linux asan+ubsan / Windows MSVC + 真 7z.dll + HLSL）

**能跑到什么程度（诚实版）**：

| 能力 | 状态 | 证据强度 |
|---|---|---|
| 引擎层（7z.dll 桥接、虚拟树、固实扫描、拖拽 seek） | 完成 | **强**：真 7z.dll 上 e2e，含变异负向控制 |
| 决策层（PrismCore 全部纯函数） | 完成 | **强**：Linux 上秒级全跑，变异构建必须变红 |
| 窗口层（D2D 绘制、导航、QuickLook、弹簧滚动） | 完成 | **中**：Windows lane 编译 + 集成测试 + PNG 快照 |
| `PrismArchive.exe` 真机双击可用 | **未验证** | **弱**：CI 只证明它**存在**（PE 头 x64 GUI），**从不启动它** |
| 安装/分发/多语言/设置持久化 | 未开始 | 无 |

**一句话**：引擎和决策层是硬的，产品化是空的。`可交付成品` 我按 35% 记，
决定这个数字的是 WinMain/安装器/真 explorer 验证 —— 跟引擎无关。

---

## 2. 立即上手（Windows 机器）

```powershell
git clone https://github.com/xueweijian/prism-archive
cd prism-archive
# 需要：VS 2022 (MSVC v143)、CMake ≥3.25、7-Zip 安装（提供 C:\Program Files\7-Zip\7z.dll）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build; ctest --output-on-failure
```

**第一次务必先跑这三条**（Linux/WSL 也能跑，10 秒内出结果）：

```bash
python3 tools/preflight.py      # 合约门禁 B1-B9 + ARCH + D1
sh tools/check_headers.sh       # 头文件自包含（21 个头）
sh tools/check_windows_tu.sh    # PrismWin 头用 stub 词汇表做类型检查
```

这三条是"CI 一轮二十分钟 vs 本地两秒"的差价所在。**改动前先让它们绿。**

跑 `PrismArchive.exe`：
```powershell
$env:PRISM_7Z_DLL = "C:\Program Files\7-Zip\7z.dll"
.\build\Src\PrismWin\PrismArchive.exe 你的归档.7z
```
不带参数会弹文件选择框。

---

## 3. 环境与依赖（踩过的坑都在这里）

### 7z.dll 解析顺序（`Src/App/Main.cpp`）
1. 环境变量 `PRISM_7Z_DLL`
2. 标准安装路径（逐条 `PathFileExistsW`）

### Windows 构建
- **CMake 找不到 VS 实例是常态**：CI 用 `ilammy/msvc-dev-cmd@v1` 先把 `cl.exe` 放进 PATH
  再用 Ninja。纯 MSVC 生成器也可以，但 CI 选的是 Ninja 路线。
- `PRISM_BUILD_WIN_LAYER=OFF` 可只构建 PrismCore（Linux 快验证用）。

### Linux/WSL 验证（推荐日常）
- 只能构建 PrismCore：`-DPRISM_BUILD_WIN_LAYER=OFF`
- **不能在 Linux 上跑 Windows 测试**，但 `tools/check_windows_tu.sh` 能用 stub 做类型检查
- ASAN/UBSAN lane 用的是 clang，`-fsanitize=address,undefined`

### 关键常量
- `kDefaultSeekLookbackBytes = 4 MiB`，`kDefaultRestartDecodeBudgetBytes = 16 MiB`
  （这两个数字有测量依据，`Tests/test_dnd_seek.cpp` 里有断言锁着，改之前看那节）
- 缩略图 `ThumbStore` 预算 64 MiB，装箱 256px

---

## 4. 代码规矩（**动代码之前必读**）

完整版在 `docs/CONTRACTS.md`（19 条 D + A/B/C 系列）。这里只列最容易违反、代价最大的：

### 分层铁律
- `Src/PrismCore/`：**header-only、纯 C++20、禁止 `<windows.h>`**。
  这一层在 Linux CI 秒级全跑 —— 是你的主战场。
- `Src/PrismWin/`：Windows 专属（7z.dll 桥接 / Shell 扩展 / D2D）。
- `Src/App/`：装配层。

**把决策推进 PrismCore，把系统调用留在 PrismWin** —— 这条决定了你一小时能迭代多少轮。

### 三条最容易踩的
- **D17 容量必须逐字节等于承诺**：任何"窗口/预算/容量"，实际可用量必须等于对外承诺。
  按整块淘汰的环形缓冲天然只兑现 `capacity - chunk`，而 Planner 按 `capacity` 授予命中权
  → 授予成功、读取时才失败。**有界容器必须断言 `residentBytes() == capacity()`。**
- **D18 断言"某路径必然失败"前先证明它够得着**：写过一条断言要求"重解码预算耗尽"，
  而它的计算从终点出发解析到越界，永远拿不到那条消息 —— 断言的是一个不存在的行为。
- **D19 本地模型与真实对象冲突时，先怀疑模型**：纯函数重放器能廉价验证算术，
  但必须复刻真实夹具的**状态前提**（冷/暖流、是否预热过缓冲）。我在这上面错过一次，CI 才纠正。

### 测试纪律
- **变异测试是正式的一等公民**：`Tests/CMakeLists.txt` 里 `WILL_FAIL TRUE` 的
  `mutation.*` 目标。为了证明"测试能抓住 bug"，要求它编译期注入变异体、跑测试、**必须失败**。
  比手改文件可靠。
- **负向控制是硬要求**：每加一条守卫，必须演示**把 bug 注回去它真的会红**。
  判断标准：注入 bug 后必须红，否则它是"重述算术"而不是守卫。
  （本项目删过一条这样的假绿断言，留过一条真的。）
- **`PASS_REGULAR_EXPRESSION` 锁 stdout**：静默 SKIP 永远不算过。
- **测试框架自研零依赖**：`Tests/mini_test.hpp`，断言返回 `bool`，
  前置失败早退写 `if (!PRISM_CHECK(x)) return;`。

### 别做的事
- **禁止对整文件做索引手术**（D7）。这条付过两次 CI 红：`Main.cpp` 被切坏，孤儿尾巴。
- **push 必须以远端为底**（D6）。曾用本地过时树推送，静默丢掉 CMake 注册。
- **subagent 不碰共享仓/共享 `/tmp` 的 git**（D9）。

---

## 5. 现在手上未完成的东西

### 未合并 PR（需要你处理）

**PR #15 `fix/b9-worker-ui-boundary`** —— 修 `Main.cpp` 里一个真 bug，已推到远端。

改的是什么（**这是本次唯一未合并的代码改动**）：
1. **数据竞争**：激活处理器在 **detached 线程**里调 `window.Hud().OnStart(...)`。
   而 `hud_` 由 WndProc 在 UI 线程喂，类里白纸黑字写着 "single-threaded by rule"，`Hud()` 没有锁。
   每次解压都在和读 `hud_` 的渲染器竞争。
2. **窗口 UAF**：`window` 是 `wWinMain` 的**栈对象**，`Run()` 返回即销毁。
   detached 线程按引用持有 `&window` → 解压中途关窗口 = 往已释放内存里写。
3. **`app->extracting` 永不复位**：只在线程自己的完成路径清 false，
   线程走不到完成路径就永久卡住后续所有双击。

改法（是 API 形状，不是纪律）：
- `GalleryWindow` 新增 `PostHudStart` / `PostStatus` / `PostProgress`：
  **不碰任何成员，只 PostMessage**。`kHudStart` / `kStatusText` 携带
  `new` 出来的字符串指针，WndProc 用 `unique_ptr` 按值接管；`PostMessage` 失败时在源头释放。
- `Main.cpp` 保留 `std::thread` 并在 `window` 出作用域前 **join**。

**CI 状态（必须知道）**：
- 第一次 Windows 红：`ProgressHud::OnStart` 收 `std::string`（UTF-8），我传了 `std::wstring`。
  已修（5623 之后那次推送），**但修复后的 CI 我没等到结果**就交给你了。
  → **接手第一件事：看 PR #15 的 Windows lane 是否绿。** 若还红，大概率还是编码/类型问题，
  因为那个文件在 Linux 上根本无法编译（见下）。

### 一个我引入但未验证的根因
`Src/App/Main.cpp` **没有任何本地门禁能编译它**。我试过用 `tools/win_stub` 覆盖它，
但它要 D2D/DWrite 的 COM 词汇表，**半吊子伪造 COM 表面会让门禁撒谎** —— 这恰好是
`tools/win_stub/` 自己头注释禁止的。所以我退回了，改为：

**preflight 规则 B9**（静态、不需要任何 SDK 词汇）：
- worker 线程体里调 `window.<非Post*成员>(` → 红
- 出现 `.detach()` → 红

**已做负向控制**：把原始 bug 注回去，B9 精确报出三条
（`window.Hud()` / `window.SetStatus()` / `detach`）。这条规则是真的守卫。

**顺带修的第二件事**：`tools/check_windows_tu.sh` 在 `main` 上是**坏的** ——
它依赖的 `tools/win_stub/` 目录从来没被推上去，脚本第一行 `cp` 就失败。
而且**没有任何 CI 步骤调用它**，所以坏了没人知道。
PR #15 补上 stub 目录 **并把门禁接进了 CI**（Linux lane）。这就是"没人跑的守卫不是守卫"。

---

## 6. 路线图：下一步该做什么（按价值排序）

### 第一优先：`PrismArchive.exe` 真机跑一次
CI 只证明 exe **存在**。验收标准（`docs/PROGRESS.md` 批次 A）写的是：
> 双击打开 .7z/.zip → 网格/列表切换 → 真缩略图 → 弹簧滚动 → QuickLook 卡片 → 文件夹导航

**这件事没有捷径，只能人坐在 Windows 上点一遍。** 跑完把结论写回 `docs/PROGRESS.md`，
并把任何红写进 `docs/CONTRACTS.md`（新的坑）。
建议同时开 ASAN 构建跑一遍（CI 的 Windows lane 已带 `/fsanitize=address`）。

### 第二优先：修共享 `ArchiveReader`（已知债务，我给 PR #15 留下了）
`sweep` 和 `extract` 两个线程**共用同一个 `ArchiveReader`**，而 7z.dll handler 不是线程安全的
（7-Zip 自己的规则：每个并发操作一个 handler）。
两边现在都 join 了，**生命周期**问题解决了，但"扫描还在跑时用户双击解压"是**真实交叠**。
修法：第二个 reader/handle。这是实打实一块活，别塞进小 PR。

### 第三优先：批次 C（产品化）
1. MSIX Sparse 真注册（CI runner 上 `AddPackageByUriAsync` 真跑 + 证据）
2. 右键延迟实测 lane（< 12ms 目标，**出数字**）
3. MoTW 继承接主流程：`DpapiVault.hpp` 里 `MoTWPropagator` **已实现且完整**，
   但**没有接进解压主流程** —— 接线即可。AMSI 预检**完全没有**。
4. 图标资源、设置持久化、en/zh 资源表、安装器（Inno Setup 或 MSIX 直发）

### 第四优先：批次 D（性能达标 + 发布）
`docs/PERF.md` 已有第一份跨规模矩阵（44 配置 Kaggle 实跑），
但蓝图 ch7.2 表还没逐格填数字。

---

## 7. 工具链与我的操作习惯（能省你时间）

### 验证比收集证据便宜
- **别串行等 CI**：本地 `g++ -fsyntax-only` / `ast.parse` / 小 fixture 先过一遍
- 卡住三轮以上 → **转"权威对拍"**（D13）：找官方同功能客户端源码逐行对拍。
  本项目多次靠 `7z2600/CPP/7zip/UI/Client7z/Client7z.cpp` 一行定位真因。
  18 轮单变量试错全部打转，一行对拍解决。

### 图表/日志读数
- profile 记**绝对 ms**，不记百分比（占比陷阱：CUDA 版 stem 0.68%→75% 的教训）

### Kaggle 算力农场（可选，用于大规模基准）
`kaggle/bench_solid.py` + `kaggle/kernel-metadata.json`，CLI 2.2.4，
kernel `weijianxue/prism-bench-matrix`。**已知结论**：固实块让逐项解压**慢** 1.2–21.4 倍
（重放前缀的代价），且倍数随文件大小增长 —— 这条推翻了旧 PR #7 的叙事。

---

## 8. 文档地图（按需要读）

| 文件 | 什么时候读 |
|---|---|
| `docs/CONTRACTS.md` | **改任何跨边界代码前**。19 条 D + A/B/C，每条都是付过代价的 |
| `docs/METHODOLOGY.md` | 想知道为什么这套验证流程长这样 |
| `docs/PROGRESS.md` | 想知道"现在到哪了"、批次划分、已知债务 |
| `docs/ADR.md` | 想知道某个设计为什么被降级/放弃（如 25ms 冷启动） |
| `docs/PERF.md` | 性能数字，含推翻了旧结论的那份矩阵 |
| `docs/BLUEPRINT.md` | 11 章产品蓝图（1276 行，原始需求） |

---

## 9. 这个项目最值钱的三个认知

1. **决策与系统调用必须分层**。PrismCore 在 Linux 上秒级全跑，所以一天能迭代几十轮；
   PrismWin 每改一次要等二十分钟 CI。**把判断塞进 PrismCore，是产能的全部来源。**

2. **负向控制不是可选步骤**。每一条守卫都要演示"把 bug 注回去它真的会红"。
   本项目删过一条注入 bug 后仍绿的假绿断言 —— 那不是守卫，是重述算术。
   变异测试目标（`WILL_FAIL TRUE`）是把这件事自动化的正确做法。

3. **本地模型骗过我一次，值得记住**。我的纯函数重放器把上一块的预热状态带进了下一块，
   于是预测"免费命中"，而真实冷流应该重启。CI 纠正了我。
   模型与真实对象冲突时，**默认模型错了**（D19）。

---

*最后更新：2026-11-01。PR #15 的 CI 结果未确认，见第 5 节。*
