# RELEASE — 一条命令产出可分发、可校验的产物（卡 C24）

> 验收标准不是"能打出 zip"，而是**打包时漏掉一个文件必须红**。
> 因此期望清单来自 `cmake --install» 落盘的目录树，而不是打包脚本自己走了哪几条复制语句
> （CONTRACTS D3：只复述写入方的检查不算检查）。

## 一、一条命令

    . E:\wg\env.ps1 -Card c24
    pwsh -File tools\release\New-PortablePackage.ps1 -BuildDir E:\build\wt-c24

产物：

    dist/PrismArchive-<ver>-win-x64.zip   可分发载荷（自带说明书 + 清单 + 校验和）
    dist/SHA256SUMS.txt                  整包哈希（下载即可校验）

脚本固定六步，任何一步不满足就非 0 退出、不留"半个包"：

1. 读 `tools/release/version.json»，与 `CMakeLists.txt» / `Src/App/PrismArchive.rc» 对拍，
   版本漂移直接 FAIL（不会产出 0.1.0 的文件名配一个旧 VERSIONINFO）。
2. `cmake --install <build> --prefix <tmp>» 得到安装布局——和 `install.ps1» 实际写的树同源。
3. 按布局复制到 stage（策略排除项见下，逐条记录原因）。
4. 生成 `PORTABLE.md»、`prism-package.json»、`SHA256SUMS.txt»。
5. 打 zip。
6. **重新打开 zip、逐条目重新哈希**，再做完整性断言；通过后才写 `dist/SHA256SUMS.txt»。

## 二、包里到底有什么

| 文件 | 来源 | 作用 |
|---|---|---|
| `PrismArchive.exe» | `cmake --install»（Src/PrismWin） | 主程序（VERSIONINFO 在此） |
| `PrismShellExt.dll» | 同上 | Win11 `IExplorerCommand» 右键菜单 |
| `PrismShellExtClassic.dll» | 同上 | Win10 `IContextMenu» 右键菜单 |
| `Assets/Shaders/*.hlsl» ×5 | 同上 | Liquid Glass 着色器（含 `.hlsli»） |
| `Assets/Icon/prism.ico» | 同上 | 窗口 / 文件关联图标 |
| `install.ps1» / `uninstall.ps1» | `tools/» | 每用户安装 / 卸载（免管理员） |
| `PORTABLE.md» | 生成 | 便携说明（三步上手 + 自校验命令） |
| `prism-package.json» | 生成 | 版本、git commit、每文件 size + sha256、排除项及原因 |
| `SHA256SUMS.txt» | 生成 | GNU `sha256sum» 格式，覆盖除自己以外的每个文件 |

**不含 `7z.dll»**：引擎是 LGPL，运行时从 `PRISM_7Z_DLL» 或标准安装路径发现。
包里出现任何 `7z.dll» → 断言直接红（`mutation.release.extra7zdll» 就是这条的负向控制）。

**有意排除**（写进 `prism-package.json» 的 `excludedFromPackage»，带原因，可审计）：

| 模式 | 原因 |
|---|---|
| `share/prism-archive/**» | 根 `CMakeLists.txt» 的 `install(DIRECTORY Assets/Shaders ...)» 复制的一份 shader 副本；便携包只要 `Assets/Shaders» 一份 |
| `Assets/Package/**» | MSIX 稀疏包载荷（`AppxManifest.xml» + 磁贴图）；便携包走 `install.ps1»，不需要包标识 |

## 三、校验分三层，三层都用同一套数据

1. **包内单文件**（用户）：解压后 `sha256sum -c SHA256SUMS.txt»；Windows 片段在 `PORTABLE.md» 里。
2. **整包**（下载者）：`dist/SHA256SUMS.txt» 里的哈希 = 你下载到的 zip 的哈希。
3. **完整性**（发布方 / CI）：把 zip 和 `cmake --install» 布局**精确对拍**（多一个、少一个、哈希不符都红）。

    pwsh -File tools\release\Test-ReleasePackage.ps1 -ZipPath dist\PrismArchive-0.1.0-win-x64.zip
    pwsh -File tools\release\Test-ReleasePackage.ps1 -ZipPath dist\PrismArchive-0.1.0-win-x64.zip -InstallRoot <prefix>

不带 `-InstallRoot» 时只能证明"包自洽"（清单与字节一致、必需文件齐、无 7z.dll）：
能发现篡改与清单缺项，但发现不了"整个包一致地少一个文件"——因为没有外部真值可比。
CI 的 `win.release_package» 就是那个外部真值。

## 四、版本号：单一真源

`tools/release/version.json» 是唯一真源（`product» / `version» / `build» / `runtime»）。
文件名、`prism-package.json»、`PORTABLE.md»、`SHA256SUMS.txt» 全部由它派生。

现状是**三份副本**（后两份是 C3 的文件，C24 不直接改）：

| 副本 | 位置 | 现状 |
|---|---|---|
| `project(PrismArchive VERSION 0.1.0)» | `CMakeLists.txt» | 由 `Get-VersionDrift» 对拍 |
| `PRISM_VERSION_*» 宏 + `VS_VERSION_INFO» | `Src/App/PrismArchive.rc» | 由 `Get-VersionDrift» 对拍 |

打包前对拍不通过 → 不产出任何产物。这样"改版本忘了改 VERSIONINFO"在**打包时**就红，
而不是等用户在文件属性里看到一个旧版本。

### 消除第三份副本的补丁（交给 C3 应用）

补丁文件：`tools/release/patches/0001-version-from-json.patch»

    git apply --check tools/release/patches/0001-version-from-json.patch
    git apply         tools/release/patches/0001-version-from-json.patch

它做两件事：

1. 新增 `tools/release/prism_version.h.in» 模板，由 `configure_file()» 从 `version.json»
   生成 `prism_version.h»（放在 target 的二进制目录）。
2. `Src/App/PrismArchive.rc» 的六行宏定义换成 `#include "prism_version.h"»；
   `Src/PrismWin/CMakeLists.txt» 增加若干行：读 `version.json»、拆三段版本号、
   `configure_file»、把生成目录加进 `PrismArchive» 的 include。

应用后 `Get-VersionDrift» 会自动识别"VERSIONINFO 来自生成头文件"这条形态
（`PrismArchive.rc VERSIONINFO is generated from version.json (C24 patch applied)»），
不再要求 `.rc» 里出现字面量。补丁已验证 `git apply» 干净、在独立 build 目录构建通过，
且 `PrismArchive.exe» 的文件版本与 `version.json» 一致（证据见 PR 描述）。

## 五、Authenticode 签名：有证书就签，没有就明确 SKIP

**顺序必须是"先签名、后打包"**：签名改写 PE 字节，打包后再签会让 `SHA256SUMS.txt»
和整包哈希同时失效。

    # 1) 签载荷（原地）
    pwsh -File tools\release\Sign-ReleaseArtifacts.ps1 -Path E:\build\wt-c24\Src\PrismWin
    # 2) 再打包（哈希覆盖已签名的字节）
    pwsh -File tools\release\New-PortablePackage.ps1 -BuildDir E:\build\wt-c24

证书来源两种，都不入库：

- `-CertificateThumbprint <sha1>»：已在 `Cert:\CurrentUser\My» / `Cert:\LocalMachine\My» 里；
- `PRISM_SIGN_PFX» + `PRISM_SIGN_PFX_PASSWORD»：PFX 文件 + 口令（CI secret 用这个）。

两种都没有时脚本打印**缺什么**并 `exit 77»：

    SIGN: SKIP (no code-signing certificate configured: ...)
    SIGN: SKIP reason -- a public release needs an OV/EV Authenticode certificate issued to the publisher ...

`Tests/suites/c24-release.cmake» 给 `win.release_sign» 设了 `SKIP_RETURN_CODE 77»，
所以 ctest 报 **Skipped** 而不是绿色通过（CONTRACTS D1：静默跳过不能算过）。

公开分发需要**签给发布主体**的 OV/EV Authenticode 证书（含时间戳服务）；
个人自签证书只能内部验证——脚本不拦，但发布页不该放自签产物。

## 六、发布步骤（`gh release create --draft»）

草稿是"人工过一遍"的位置：CI 不自动推资产，发布是显式动作。

    # 1. 打 tag（版本必须等于 tools/release/version.json）
    git tag -a v0.1.0 -m "PrismArchive 0.1.0"
    git push origin v0.1.0

    # 2. 先建草稿并上传资产（PowerShell 下换行用反引号续行）
    gh release create v0.1.0 dist/PrismArchive-0.1.0-win-x64.zip dist/SHA256SUMS.txt --draft --title "PrismArchive 0.1.0" --notes-file docs/RELEASE-NOTES-0.1.0.md

    # 3. 从远端把资产抓回来校验（信远端字节，不信本地目录）
    gh release download v0.1.0 --pattern "*.zip" -D %TEMP%\rel-check
    gh release download v0.1.0 --pattern "SHA256SUMS.txt" -D %TEMP%\rel-check
    pwsh -File tools\release\Test-ReleasePackage.ps1 -ZipPath %TEMP%\rel-check\PrismArchive-0.1.0-win-x64.zip

    # 4. 校验通过后转正
    gh release view v0.1.0 --json assets --jq '.assets[] | "\(.name) \(.size)"'
    gh release edit v0.1.0 --draft=false

`gh release create» 不会替你校验、也不会替你签名：草稿 + 第 3 步是唯一的自校验关口。

## 七、负向控制（CONTRACTS D3）

每个 mutant 只注入一处缺陷，**先断言注入真的发生了**，再看断言是否红：

| mutant | 注入点 | 期望红在哪 | 实测 |
|---|---|---|---|
| `DropShellExtClassic» | 复制阶段少拷 `PrismShellExtClassic.dll» | MISSING + REQUIRED + SHA256SUMS 未覆盖 + manifest 少一项（5 条） | CAUGHT |
| `Extra7zDll» | stage 里塞一个 4 字节 `7z.dll» | UNEXPECTED + FORBIDDEN + manifest 数量不符（3 条） | CAUGHT |
| `SumGap» | 写完 `SHA256SUMS.txt» 后删掉 `PrismArchive.exe» 那行 | "SHA256SUMS.txt does not cover PrismArchive.exe" | CAUGHT |

退出码语义（与 `tools/install.ps1 -SelfTest -Mutate» 一致）：

- `MUTATION <name>: CAUGHT (n assertion(s) went red; injection: ...)» → 0，守卫是锋利的；
- `MUTATION <name>: NOT INJECTED (...)» → 1，注入本身失效（最容易自欺的一种失败）；
- `MUTATION <name>: NOT CAUGHT» → 1，包带着缺陷却验过了，守卫是瞎的。

## 八、CI 注册（`Tests/suites/c24-release.cmake»）

| 测试 | 证明什么 | 超时 |
|---|---|---|
| `win.release_package» | 端到端一条命令：install → 清单 → 校验和 → zip → 拆包复核 | 600s |
| `win.release_verify» | 独立入口按"下载用户"的方式再验一次（含整包哈希对 sidecar） | 300s |
| `mutation.release.dropshellextclassic» | 少一个 DLL 必须红 | 600s |
| `mutation.release.extra7zdll» | 引擎入包必须红 | 600s |
| `mutation.release.sumgap» | 校验和漏一行必须红 | 600s |
| `win.release_sign» | 有证书就签、没有就 SKIP（exit 77） | 300s |

打包类测试各自跑一次 `cmake --install»，都会写同一个 `install_manifest.txt»，
所以共用 `RESOURCE_LOCK "prism-release-install"»（`ctest -j» 下也不会互相踩）。

## 九、边界与未做项

- **不做 MSIX/AppX 打包**：那是稀疏包分发线（C4 + `tools/msix/»），便携包只走 `install.ps1»。
- **不签名**：仓库里没有证书，也不该有；只留骨架 + 明确的 SKIP 契约。
- **不自动发布**：`ci.yml» 不在本卡所有权内；CI 只跑测试，不 `gh release»。
- **不做差分/增量更新、不做 winget/Chocolatey 清单**：属于后续分发卡。
- **zip 不是逐字节可复现**（条目时间戳来自文件 mtime）；内容哈希稳定，
  `SOURCE_DATE_EPOCH» 可固定 `prism-package.json» 的 `builtAt»。

## 十、文件清单

    tools/release/version.json                 版本唯一真源
    tools/release/ReleaseCommon.ps1            版本对拍 / 布局枚举 / zip 复核（纯函数）
    tools/release/New-PortablePackage.ps1      一条命令打包 + 自校验 + 负向控制
    tools/release/Test-ReleasePackage.ps1      独立校验入口（用户 / CI 同一套断言）
    tools/release/Sign-ReleaseArtifacts.ps1    Authenticode 骨架（无证书 → SKIP + 77）
    tools/release/prism_version.h.in           版本头模板（补丁的一部分）
    tools/release/patches/0001-version-from-json.patch
    Tests/suites/c24-release.cmake             ctest 注册
