# CI — 哪条 lane 证明什么，本地该怎么跑

> 权威口径：**红绿只认 CI**。本地门禁（`tools/local_gates.ps1`）是为了在推之前
> 花 10 秒抓住确定性错误，不是为了替代 CI。本地只 Build-Only 自己改的 target，
> 全部测试交给 runner 的并发。

## 一、一条 run = 两个 job

| job | 证明什么 | 大概耗时 |
|---|---|---|
| `Gates + PrismCore (Linux)` | ① Release(gcc) 下 PrismCore 全量 ctest；② 同一份代码在 Debug(clang, ASan+UBSan) 下再跑一遍；③ preflight（B1-B9/ARCH/D1）、头文件自包含、Windows-TU 类型检查、MSIX manifest 四个门禁 | 2.5-4 min |
| `Windows (MSVC + ASan + 7z.dll + HLSL)` | 真 MSVC（`/fsanitize=address`）构建 7z.dll 桥接 + Shell 扩展 + exe；对 runner 自带真 `7z.dll` 跑 `win.*` 集成套件；`fxc` 编译 Liquid Glass HLSL；bench 数字进 step summary | 2.5-4 min |

两个 job 是刻意的：GitHub Free 公开仓约 20 并发 job，8 张并行卡同时推 =
8 条 run；三条 lane 就是 24 job 会全线排队，两条就是 16 job，刚好在额度内。

**触发面**：`push` 到 `main` 与 `feat/**`，以及指向 `main` 的 PR。
推分支就有一条 run，不必等开 PR。同一分支连推由 `concurrency` 取消旧一轮。

## 二、本地该跑什么

| 场景 | 命令 | 说明 |
|---|---|---|
| 改完代码，推之前 | `Invoke-Gates`（或 `. \E:\wg\env.ps1 -Card <卡>; Invoke-Gates`） | 抢构建锁 + configure + build + ctest + preflight |
| 要一条 PASS/FAIL 汇总 | `pwsh -File tools/local_gates.ps1 -BuildDir E:\build\wt-<卡>` | 8 步全跑，最后一行 `LOCAL GATES: PASS/FAIL`，日志在 `<build>/local-gates/` |
| 只跑两个 POSIX 门禁 | `pwsh -File tools/local_gates.ps1 -Only headers,windows_tu` | Windows 上自动走 WSL；`-NoShellMirror` 直接读工作树（慢 8 倍） |
| 只 Build-Only 自己的 target | `cmake --build E:\build\wt-<卡> --parallel 2 --target <target>` | 并行卡排队时最快的一条路 |
| ASan 复现 | `pwsh -File tools/local_gates.ps1 -Preset asan-msvc -BuildDir E:\build\asan-<卡> -SkipShell` | 与 CI windows lane 同一份 `CMakePresets.json` |

预置：`release-msvc` / `asan-msvc`（Windows）、`release-gcc` / `asan-clang`（Linux）。
CI 用的就是这四个，不是另写一套命令行 —— 所以它们不会腐烂。

## 三、用 gh 看结果

```bash
gh run list --branch feat/c2-gates                 # 这个分支最近几条 run
gh run list --branch feat/c2-gates --limit 1 --json status,conclusion,databaseId
gh run watch <run-id>                              # 盯到底（或 --exit-status 直接拿到退出码）
gh run view <run-id> --log-failed                  # 只看失败 step 的日志（最快的定位方式）
gh run view <run-id> --json jobs --jq '.jobs[] | "\(.name): \(.conclusion)"'
gh run download <run-id> -n ci-logs-windows        # 失败日志 artifact
```

red 时先看 run 页面的 **step summary**：失败测试名、ccache/sccache 命中率、
benchmark 数字都在那里，不用翻日志。仍看不出再 `--log-failed`。

## 断言「不是空帧」必须用 painted / digest，不能用「非黑像素数」（C113，2026-11-07）

C113 实测：`DrawFrame` 曾把 `target->EndDraw()` 的 HRESULT 丢掉，于是 D2D 以 `0x88990012`
（`D2DERR_WRONG_RESOURCE_DOMAIN`）整帧拒绝的离屏帧被当成成功帧。**这张帧不是黑的**——
它只剩画布底色，1264×741 下每像素 `B=19,G=14,R=11`、`maxChannel=18`，所以

- `non_black`（任一通道非 0）**= 936624 / 936624**，全中；
- 「至少 N 个像素非黑」这类断言**永远抓不到它**；
- `painted`（最亮通道 >= 32）**= 0**，一次就抓到；digest 也会变（该帧是 `0af0b75bc5b12383`）。

**规则**：门禁里写「帧里真的有东西」时，用 `painted`（阈值化亮度）或**记录的 digest**，
不要用「非黑/非零像素数」——被拒绝的帧比黑帧亮。共享读取器是
`Tests/win/BenchFrameCapture.hpp`：它记录 `EndDraw` 的 HRESULT，失败时打印
`RENDER FAILED hr=0x%08X` 并让 `rendered == false`，且 `painted == 0`。
## 四、两条已知噪音（不是我们的 bug）

- **D14**：runner 镜像自带的 `7z.dll` 是 ASan 构建，`SetCodecs` 会在它自己内部
  报 use-after-free（`#0/#1` 都在 `7z.dll`，我方栈帧为 0）。豁免只绑这一条签名。
- **sccache + ASan**：sccache 不缓存 MSVC 的 `/fsanitize=address` 编译，所以
  windows lane 故意不挂 launcher（挂了也只是多一跳进程）。Linux 的 asan lane 用 ccache。

## ASan 报告必须可读（PDB）

Windows 的 ASan lane 必须带 `/DEBUG`（生成 PDB）。没有它，报告里每一帧都只有 `PrismArchive.exe+0x...`，
内联帧和变量名一个都看不到——2026-11-05 的一次 `stack-use-after-scope` 因此花了 **4 轮 CI 红**才定位。
`asan-msvc` preset 现在带 `/DEBUG /OPT:REF`；改动这个 preset 时**不要**把 `/DEBUG` 去掉。

## 变异/回退脚本要锚 token id，不要锚颜色值（C102，2026-11-05）

C102 要做"回退这四条 Daylight token 再重渲染"的差异统计，第一版按**颜色字面量** `(245,245,247)` 去匹配源码，
结果**同时命中了 `--text-primary@Midnight`**（同一个 RGB 三元组），于是读到"Night 也变了 ≤5 级、集中在 chrome 字形"的**假结论**。
改成按 `{TokenId::X, Variant::Daylight, ...}` 锚定后，Night 精确归 0（15/17 帧 byte-identical）。

**规则**：变异注入与回退脚本必须以**标识符**（token id / 枚举 / 符号名）为锚点；颜色、尺寸这类**会漂移的值**不是锚点。
这与上面"门禁阈值不得依赖可调材质参数"是同一类错误：**拿一个会变的东西当参照**。
## 门禁阈值不得依赖可调材质参数（C102）

刻意设计成"能变红"的断言，阈值只能由**被测的行为**决定，不能由某个可调的设计令牌决定。
否则调令牌就得改门禁，而下一个改门禁的人看不到当初的推导，门禁就退化成"第二份真相"。

这一条来自 C102：Daylight 岛体 alpha 从 .58 调到 .82 后，`win.c33.glass_memory` 的
`bandDiff` 从 **0.86** 掉到 **0.35**——因为折射带的可见度正比于材质透过率 `(1 - alpha)`
（0.35 / 0.86 = 0.41 ≈ (1-.82)/(1-.58)）。这不是链路坏了，是材质更不透了。

两条做法，二选一，并在断言旁边写清楚理由：

1. **归一化**（首选）：把测量值除以材质透过的比例，让判据与不透明度解耦。C33 现在是
   `bandDiff / max(0.05, 1 - tintAlpha) >= 1.50`——.58 时 **2.05**、.82 时 **1.94**，
   光学链路没接上的空跑仍然是 **0.00**（照样红）；
2. **显式声明**：如果这个数字确实是产品常量（不是令牌），就在断言上注明它绑的是哪个常量、
   谁改它必须同步改门禁。

反例：把 0.4 直接降到 0.30。判据被削弱，而且下一次再调 alpha 还会再红一次。

