# PrismArchive

**7-Zip 的工业级压缩引擎 × Bandizip 的零摩擦交互 × Apple iOS 26 Liquid Glass 的光学美学。**

PrismArchive 是一个面向 Windows 11（x64）的开源归档管理器：用 7-Zip 的 C/C++ 算法内核做解压/压缩，
用 Bandizip 式的零摩擦交互组织界面，再用自定义 HLSL 管线绘制 Liquid Glass 材质。
当前版本 **0.1.0（开发版）**，MIT 许可；7-Zip 引擎不随包分发。

> **截图位（待人工补）**
> 在 Windows 11 上安装后截取主界面，保存为 `docs/assets/screenshot-main.png`，
> 再把本段替换为 `![PrismArchive 主界面](docs/assets/screenshot-main.png)`。
> 截图请包含侧边栏、归档列表和一张真实的 `.7z`；不要提交占位图或模拟图。

## 是什么

- **打开就看**：双击归档进入画廊，目录树、文件列表、缩略图一次加载；密码、乱码、套娃归档有明确的决策路径。
- **右键即用**：为 `.7z` / `.zip` / `.rar` 等 22 种格式注册每用户关联（`PrismArchive.<ext>`），
  卸载时逐键恢复安装前的处理器。
- **不碰管理员**：所有文件写入 `%LOCALAPPDATA%\PrismArchive`，所有注册表写入 `HKCU`；
  不需要 UAC，也不写机器级关联。
- **引擎外置**：`7z.dll` 是 LGPL，永不入库、永不进包；安装器只记录你机器上 7-Zip 的位置。

## 安装（每用户，免管理员）

从 [Releases](https://github.com/xueweijian/prism-archive/releases) 下载
`PrismArchive-0.1.0-win-x64.zip`，解压后：

    # 在解压目录打开 PowerShell
    pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1

安装器会复制载荷到 `%LOCALAPPDATA%\PrismArchive`、写 HKCU 关联和卸载项，并打印
`INSTALL: OK ...`。不想装到默认位置时加 `-InstallDir D:\Apps\PrismArchive`。
只想带走、不写注册表时加 `-Portable`。

### 7-Zip 依赖怎么满足

PrismArchive **不附带** `7z.dll`。任选其一：

1. 安装 [7-Zip](https://www.7-zip.org/) x64（默认路径即可，安装器会自动找到）；
2. 设置用户环境变量 `PRISM_7Z_DLL` 指向你的 `7z.dll`；
3. 把 `7z.dll` 放到 `PrismArchive.exe` 同目录（便携场景）。

安装器找不到引擎时会打印 `WARN No 7z.dll found ...`，但不会假装安装成功；
应用启动时会列出它尝试过的路径。

## 使用

- **双击** `.7z`：若 Windows 询问“如何打开”，选 **PrismArchive**；若已把 `.7z` 默认应用设为别的程序，
  到 **设置 → 应用 → 默认应用** 改一次即可（Windows 的 UserChoice 是哈希保护的，安装器不能替你改）。
- **右键 → 打开方式 → PrismArchive**：每用户安装会注册这个入口；Win11 的一级右键菜单由 MSIX 稀疏包提供，
  不在 `install.ps1` 的范围内。
- **无窗口校验**：`PrismArchive.exe --test D:\path\archive.7z`，成功时 stderr 打印
  `verify ok=1 ...` 并以 0 退出。
- **解压到当前目录**：`PrismArchive.exe --extract-here D:\path\archive.7z`。

## 卸载

- **设置 → 应用 → 已安装的应用 → PrismArchive → 卸载**；或
- `pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\PrismArchive\uninstall.ps1"`。

卸载会删除安装目录、卸载项和 `PrismArchive.*` ProgID，并把每个扩展名恢复成安装前的处理器
（逐键比对，见 `tools/verify-install.ps1`）。重复卸载会打印 `UNINSTALL: already clean`。

## 验证安装（开发 / 发布前）

    # 安全模式：只写唯一的 HKCU 临时键和 %TEMP% 目录，跑完自动清理
    pwsh -File tools\verify-install.ps1 -SourceDir build\Src\PrismWin -Sandbox

    # 真实用户视角：备份当前 HKCU 关联 -> 安装 -> --test 打开 -> 卸载 -> 逐键比对恢复
    pwsh -File tools\verify-install.ps1 -SourceDir build\Src\PrismWin

真实模式遇到已有安装会拒绝覆盖；先卸载，或加 `-CleanFirst` 让脚本先备份再卸载旧安装。
两个否定对照（`-Mutate UnquotedVerb` / `-Mutate NoRestore`）只允许在 `-Sandbox` 下运行。

## 从源码构建

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure
    pwsh -File tools\verify-install.ps1 -SourceDir build\Src\PrismWin -Sandbox

Windows 集成测试会真实加载 `7z.dll`；找不到时打印显式 `SKIP` 并以 77 退出，不会假装通过。

## 文档

- 发布步骤（构建 → 打包 → 校验 → 签名 → `gh release`）：[docs/RELEASE.md](docs/RELEASE.md)
- 架构蓝图：[docs/BLUEPRINT.md](docs/BLUEPRINT.md)
- 跨层契约：[docs/CONTRACTS.md](docs/CONTRACTS.md)
- 进度与路线：[docs/PROGRESS.md](docs/PROGRESS.md) · [ROADMAP.md](ROADMAP.md)
- 测试方法：[docs/TESTING.md](docs/TESTING.md)

## 许可

MIT（见 [LICENSE](LICENSE)）。7-Zip 引擎通过 `LoadLibraryW` 运行时加载，遵循其自身的 LGPL；
UI/交互层与引擎保持 MIT 干净隔离。
