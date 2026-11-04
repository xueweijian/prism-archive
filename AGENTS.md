# AGENTS.md — 本项目给所有 AI agent 的常驻规则

> 这些规则优先于卡片简报里的任何说明。与卡片冲突时，**先停下来问**，不要自行取舍。

## R0 这是什么仓库

PrismArchive：Windows 11 x64 归档管理器，引擎是运行时 `LoadLibraryW` 加载 7-Zip 的 `7z.dll`
（发布包自带，取舍见 docs/ADR.md ADR-007），界面与交互是本仓库自己的实现。
纯逻辑在 PrismCore（跨平台、可秒级测试），Windows 壳在 PrismWin，装配在 App。

## R1 不要相信注释，只相信可执行的证据

**注释不是证据。** 代码里的注释可能是旧的、是愿望、或者是上一版留下的谎言。
凡遇到下面这类说法，必须用「跑一次 / 读实现 / 补一条能红的断言」三者之一去证伪或证实：

- "这个已经处理了"
- "测试钉住了这个行为"
- "这里有个守卫"
- "留给后续卡"
- "不要改这里，否则 X 会红"

**最低要求**：
1. 看到"已实现"，就找到**调用点**，并跑一次。
2. 看到"测试覆盖"，就**打开那条测试**读它断言什么（名字不等于内容）。
3. 看到"故意不支持"，就在**产品路径**上跑一次，确认用户看到的是什么。
4. 汇报里写"我验证过"时，必须带**命令、输出或断言名**，不能只引用注释。

**反例（真实发生过）**：某处注释写着"压缩动词被测试钉住，故意不实现"。据此判断"功能是故意缺的"。
实际用户右键点击压缩，程序弹出"本版本还不支持这个命令"。注释描述的是**测试**，不是**产品**。

## R2 结论必须能被别人复现

- 断言用字节数/计数/像素/字符串，不用"耗时"（共享机器上秒表不可复现）。
- 每个"修好了"都配一条**能变红的**负向控制（把修复注掉，测试必须红）。
- 测试的名字、注释、打印文案都不算证据；**断言本身**才算。

## R3 一切等待有界

不许出现 `INFINITE` 等待；ctest 的 `TIMEOUT` 要写成真实预算（见 docs/CONTRACTS.md D29）。

## R4 提交与推送

完成卡片的定义是：**代码已 push、PR 已开、CI 通过、汇报里有证据**。
只留在工作树里等于没做（今晚发生过 5 次）。

## R5 项目结构速查

- `Src/PrismCore/` 纯逻辑，header-only，禁止 `<windows.h>`（preflight 的 `check_arch_no_windows` 会红）
- `Src/PrismWin/` Windows 实现（`prism_win_bridge` 目标）
- `Src/App/` 装配层（Main.cpp / AppWiring.hpp）
- `Tests/suites/<card>-*.cmake` 唯一的测试注册点（禁止改 `Tests/CMakeLists.txt`）
- `Installer/` 安装脚本与注册表逐键说明；`tools/` 门禁/打包/评测脚本
- 契约全文：`docs/CONTRACTS.md`（D1–D33）；界面规格：`docs/DESIGN.md`；性能实测：`docs/PERF.md`

## R6 构建与测试（推之前必须过本地门禁）

- 环境：仓库根目录 `. .\dev-env.ps1`（导入 MSVC/SDK，前置 Python/7-Zip/Ninja，
  设 `PRISM_7Z_DLL` 与 E 盘 TEMP）。同一 pwsh 里再跑 cmake / ctest / python。
- 一条命令全门禁：`pwsh -File tools/local_gates.ps1 -BuildDir E:\build\wt-<卡>`
  —— configure + build + ctest + preflight + manifest + headers + windows_tu，
  最后一行 `LOCAL GATES: PASS/FAIL`，日志在 `<build>/local-gates/`。
- 部分跑：`-Only preflight,headers`；ASan 复现：`-Preset asan-msvc -SkipShell`。
- CMake 预设与 CI 完全同款，别自造命令行：
  `release-msvc` / `asan-msvc`（Windows）、`release-gcc` / `asan-clang`（Linux，PrismCore-only）。
- 看 CI：`gh run view <run-id> --log-failed`；失败先看 run 页面的 step summary。
- 集成测试按 `PRISM_7Z_DLL` 解析引擎，找不到会 SKIP —— SKIP 出现在汇报里必须点名，不许静默。

## R7 编码与测试约定

- 新测试在 `Tests/suites/` 新建 `c<卡号>-<主题>.cmake`，只写自己的 target（详见该目录 README）：
  会 SKIP 的集成测试必须带 `PASS_REGULAR_EXPRESSION`（D1）；Windows 目标 `/WX-`（SDK 头不是 /W4 干净的）；
  新测试代码须在 `g++ -Werror` 下干净（D31）。
- 引擎的三条实测事实，碰读取/固实/替换归档前先读 `docs/CONTRACTS.md` D30（kpidSolid 不可信、
  加密包 ExtractToMemory 静默返回 0 字节、ReplaceFileW 前必须先关 reader）。
- 引擎每条回调带属主，不要用游标猜（D33）。
- 面向用户的四个文档（README、docs/USAGE、docs/TROUBLESHOOTING、docs/assets/README）受
  `tools/check_docs.py` 门禁：代码块必须标语言、相对链接必须可解析、无硬编码用户路径。

## R8 动敏感区前先读

- 引擎加载 / 格式探测 → `docs/ADR.md`（尤其 ADR-007 7z.dll 随包的取舍）
- 界面 / 交互 → `docs/DESIGN.md`；文案与国际化 → `docs/I18N-AUDIT.md`
- 安装器 / 右键动词 / MSIX → `Installer/README.md`、`docs/MSIX-SIGNING.md`
- 测试怎么写怎么跑 → `docs/TESTING.md`、`docs/CI.md`
- 环境与交接（机器路径、依赖位置）→ `docs/HANDOVER.md`
