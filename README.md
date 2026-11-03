# PrismArchive

**把 7-Zip 的引擎装进一个 Windows 11 的外壳：双击就看，右键就解，键盘优先。**

PrismArchive 是一个 Windows 11 x64 归档管理器：引擎用 7-Zip 的 `7z.dll`（运行时加载，不随包分发），
界面、交互与文件关联都是本仓库自己的实现。当前版本 **0.1.0（开发版）**，MIT 许可。
已经画出来但还没接线的入口，统一列在 [docs/USAGE.md](docs/USAGE.md#本版本的边界)。

装完最常见的问题是「右键没有菜单」。它不是安装失败，是 Windows 11 的菜单分层，
见下文 [为什么右键在「显示更多选项」里](#为什么右键在显示更多选项里)。

## 截图

四张图是本项目的发布素材，**尚未拍**（不提交模拟图或占位图，规范见
[docs/assets/README.md](docs/assets/README.md)）。下表就是四张图各自要证明的东西：

| 文件 | 拍什么 | 要证明的事 |
|---|---|---|
| `docs/assets/screenshot-gallery.png` | 主界面打开一个真实 `.7z`：标题栏、动作岛、侧边栏、网格缩略图、状态栏 | 打开归档后一屏能看到目录、缩略图与状态 |
| `docs/assets/screenshot-context-menu.png` | 资源管理器右键 →「显示更多选项」里的「用 PrismArchive 打开」「用 PrismArchive 解压到此处」 | 右键动词确实注册到了 HKCU，且位于经典菜单 |
| `docs/assets/screenshot-preview.png` | 选中一行按空格后的 Quick Look 预览层 | 空格预览不离开列表 |
| `docs/assets/screenshot-palette.png` | Ctrl+K 命令面板，输入 `png` 后的过滤结果 | 面板同时搜归档内路径和命令 |

拍完后把上面四张图换成 Markdown 图片引用即可；`docs/assets/README.md` 里写明了每张图的
窗口尺寸、必须出现的元素和禁止出现的内容（真实路径、真实用户名）。

## 安装（三步，免管理员）

1. 从 [Releases](https://github.com/xueweijian/prism-archive/releases) 下载
   `PrismArchive-0.1.0-win-x64.zip`，解压到任意目录（例如 `D:\Apps\PrismArchive`）。
2. 装 [7-Zip](https://www.7-zip.org/) x64。**引擎不入包**（LGPL，见下文）。
3. 在解压目录执行安装脚本：

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

成功的最后一行是这样（`engine=` 后面就是你机器上的 `7z.dll`）：

```text
INSTALL: OK C:\...\PrismArchive (22 associations, engine=E:\DevTools\7-Zip\7z.dll)
```

脚本从**自己所在目录**取 payload（zip 解压后 `PrismArchive.exe`、`install.ps1`、`uninstall.ps1`、
`register-static-verbs.ps1` 同级），再退回构建目录；所以解压到哪都能直接跑。

安装器只写 `HKCU` 和 `%LOCALAPPDATA%\PrismArchive`，不弹 UAC、不写机器级关联。
不想装到默认位置加 `-InstallDir`；只想带走、不写注册表加 `-Portable`：

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -InstallDir D:\Apps\PrismArchive
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -Portable -SourceDir . -InstallDir .
```

先看它打算写什么，再真装（`-DryRun` 不落盘、不写注册表）：

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -DryRun
```

## 三条最常见的用法

**1. 双击打开。** 安装器把 22 个扩展指到 PrismArchive。如果双击后打开的是别的程序，
说明这个扩展被 Windows 的「用户选择」钉住了——那不是安装器能改的，见
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#关联被别的软件锁定双击还是打开别的程序)。

**2. 右键解压。** 资源管理器里右键归档 →「显示更多选项」→「用 PrismArchive 解压到此处」。
这条动词由安装器写进注册表（`HKCU\Software\Classes\*\shell\PrismArchiveExtract`），
命令行是 `PrismArchive.exe --extract-here "<归档路径>"`。

**3. 右键压缩。** 这一步**安装器不做**：压缩类动词来自经典右键菜单处理器
（`PrismShellExtClassic.dll`），需要单独注册一次（仍然免管理员）：

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\msix\register-classic.ps1 -DllPath "$env:LOCALAPPDATA\PrismArchive\PrismShellExtClassic.dll"
```

注册后右键非归档文件 →「显示更多选项」里会出现「压缩为 .7z 归档」「压缩为 .zip 归档」。
不注册也有等价入口：应用内 **文件 → 压缩为 .7z…**，或命令行 `--compress-7z`。

## 为什么右键在「显示更多选项」里

Windows 11 把右键菜单拆成两层：

- **一级菜单**只收「有包标识」的处理器：`IExplorerCommand` + 已签名的 MSIX 稀疏包。
  绿色 zip 发布物没有包标识、也没有签名，所以进不去。
- **经典菜单**（一级菜单底部的「显示更多选项」，或直接按 `Shift+F10`）收的是
  `HKCU\Software\Classes\*\shell\...` 这类传统动词——这正是 `install.ps1` 写的那两条。

所以「右键没有」通常是找错层了。一条命令确认动词在不在：

```pwsh
$cls = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes')
'open    = ' + [bool]$cls.OpenSubKey('*\shell\PrismArchiveOpen')
'extract = ' + [bool]$cls.OpenSubKey('*\shell\PrismArchiveExtract')
```

两行都是 `True` 就说明注册好了，只是要去「显示更多选项」里找。想把它搬进一级菜单，
必须自己构建并签名 MSIX 稀疏包（`tools/msix/build-sparse.ps1`，证书主体须为
`CN=PrismArchive Contributors`）；这是开发路径，不在绿色包范围内。

## 7z.dll 依赖

PrismArchive **不附带** `7z.dll`。任选其一：

1. 安装 7-Zip x64（默认路径即可，安装器会自动找到并写进 `PRISM_7Z_DLL`）；
2. 自己设用户环境变量 `PRISM_7Z_DLL` 指向 `7z.dll` 的**完整路径**；
3. 把 `7z.dll` 放到 `PrismArchive.exe` 同目录（便携场景）。

解析顺序（每条都带来源标签，弹窗里原样列出）：

| 顺序 | 来源 | 路径 |
|---|---|---|
| 1 | `[env]` | 环境变量 `PRISM_7Z_DLL` |
| 2 | `[app-dir]` | `PrismArchive.exe` 同目录 |
| 3 | `[registry:*]` | 7-Zip 自己的 `InstallDir`（HKLM / WOW6432Node / HKCU） |
| 4 | `[path]` | `PATH` 里每个目录（去重） |
| 5 | `[common:*]` | `%LOCALAPPDATA%\Programs\7-Zip`、scoop |
| 6 | `[probe]` | 两个 `Program Files\7-Zip` 兜底 |

找不到时应用不会假装成功：弹窗列出每一条尝试过的路径及其来源，按 `Ctrl+C` 即可复制全文。
更多诊断见 [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#7zdll-找不到)。

## 命令行

无窗口动词（右键菜单调用的就是这些），退出码即裁决：

| 命令 | 作用 | 退出码 |
|---|---|---|
| `PrismArchive.exe "<归档>"` | 打开界面 | 0 |
| `--extract-here "<归档>"` | 解压到归档所在目录 | 0 成功 / 1 失败 / 2 缺参数 |
| `--extract-sub "<归档>"` | 解压到以归档名命名的子目录 | 同上 |
| `--test "<归档>"` | 只校验、不开窗；stderr 打印 `verify ok=...` | 0 通过 / 1 失败 |
| `--compress-7z "<文件或目录>" ...` | 压缩成 `.7z` | 0 成功 / 1 失败 / 2 缺参数 |
| `--compress-zip "<文件或目录>" ...` | 压缩成 `.zip` | 同上 |
| `--smart-extract` / `--smart-extract-each` | 智能解压（单个 / 各归各的目录） | 同上 |

不认识的参数不会开对话框卡住调用方，而是向 stderr 说明并退出 2：

```text
verb refused: unsupported flag --nope; supported: --smart-extract --extract-here --extract-sub --test --compress-here --compress-7z --compress-zip --smart-extract-each
```

加密归档的动词会先弹密码框（每次调用都要人工输入一次），批处理脚本里请注意这一点。

## 卸载

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\PrismArchive\uninstall.ps1"
```

或者 **设置 → 应用 → 已安装的应用 → PrismArchive → 卸载**。卸载会删除安装目录、卸载项、
`PrismArchive.*` ProgID 与右键动词，并把每个扩展恢复成安装前的处理器（逐键比对）。
重复卸载打印 `UNINSTALL: already clean`，不是错误。

## 文档

- 装完之后怎么用（动作岛、快捷键、拖放、密码、归档内编辑）：[docs/USAGE.md](docs/USAGE.md)
- 出问题先查这里（右键没有、7z.dll 找不到、乱码、被占用、关联被锁）：[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- 截图规范与拍摄清单：[docs/assets/README.md](docs/assets/README.md)
- 从源码构建 / 打包 / 校验 / 发布：[docs/RELEASE.md](docs/RELEASE.md)
- 界面规格：[docs/DESIGN.md](docs/DESIGN.md) · 跨层契约：[docs/CONTRACTS.md](docs/CONTRACTS.md)
- 安装器细节（注册表逐键、`-Verify` 的三种结局）：[Installer/README.md](Installer/README.md)
- 进度与路线：[docs/PROGRESS.md](docs/PROGRESS.md) · [ROADMAP.md](ROADMAP.md)

## 许可

MIT（见 [LICENSE](LICENSE)）。7-Zip 引擎通过 `LoadLibraryW` 运行时加载，遵循其自身的 LGPL；
UI/交互层与引擎保持 MIT 干净隔离。
