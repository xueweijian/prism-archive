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

## 四、两条已知噪音（不是我们的 bug）

- **D14**：runner 镜像自带的 `7z.dll` 是 ASan 构建，`SetCodecs` 会在它自己内部
  报 use-after-free（`#0/#1` 都在 `7z.dll`，我方栈帧为 0）。豁免只绑这一条签名。
- **sccache + ASan**：sccache 不缓存 MSVC 的 `/fsanitize=address` 编译，所以
  windows lane 故意不挂 launcher（挂了也只是多一跳进程）。Linux 的 asan lane 用 ccache。
