# PrismArchive 故障排查

按**症状**查。每条都写成三段：症状（你会看到什么）→ 原因（代码里是哪一步）→ 解决（按顺序做）。
命令都在 Windows PowerShell 7（`pwsh`）里跑；没有特别说明时不需要管理员权限。

先做一次 30 秒自检，很多问题一眼就能定位：

```pwsh
# 1. 程序在不在
Test-Path "$env:LOCALAPPDATA\PrismArchive\PrismArchive.exe"
# 2. 右键动词在不在（两个都应为 True）
$cls = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes')
'open    = ' + [bool]$cls.OpenSubKey('*\shell\PrismArchiveOpen')
'extract = ' + [bool]$cls.OpenSubKey('*\shell\PrismArchiveExtract')
# 3. 引擎路径（空 = 没设）
[Environment]::GetEnvironmentVariable('PRISM_7Z_DLL', 'User')
```

## 窗口一片空白（标题栏和菜单栏在，内容区全白）

**症状**：打开归档后窗口只有标题栏和菜单栏，内容区一片白。用屏幕阅读器或 UI 自动化
（例如 `agent-desktop snapshot`）却能看到**完整的**控件树：按钮、标签页、列表、
状态栏的每个字段都在。

**原因**：先别急着找「少了一次重绘」。PrismArchive 的内容区由 Direct2D 画在一个
HWND 渲染目标上，**画完还要由系统把这一帧呈现到屏幕上**。这台机器如果丢掉了
Direct2D 的窗口呈现，代码照样一路成功（`EndDraw()` 返回 `S_OK`），窗口却还是白的。
用下面这个探针 30 秒判定——它**不含 PrismArchive 的任何代码**，只用 Direct2D 画一个
纯红窗口，再用 GDI 在同一窗口画一块绿色做对照。

**解决**：

1. 跑探针（需要有 VS 2022 / Build Tools 的 C++ 工具链；脚本自己找，不需要先配 PATH）：

   ```pwsh
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools\d2d_present_probe.ps1
   ```

2. 看最后一行：

   ```text
   D2D_PRESENT {"enddraw":"0x00000000","paints":6,"d2d_pixel":"ff0000","gdi_pixel":"00ff00","verdict":"D2D_PRESENT_OK"}
   D2D_PRESENT {"enddraw":"0x00000000","paints":6,"d2d_pixel":"ffffff","gdi_pixel":"00ff00","verdict":"D2D_PRESENT_BROKEN"}
   ```

3. 按 verdict 处理：

   | verdict | 含义 | 下一步 |
   |---|---|---|
   | `D2D_PRESENT_OK` | 两条呈现路径都正常 | 白屏是应用的问题：带上 `%LOCALAPPDATA%\PrismArchive\logs` 里当天的日志开 issue |
   | `D2D_PRESENT_BROKEN` | GDI 能看见、Direct2D 看不见 | **应用是好的，这台机器丢了 D2D 呈现**：更新/回滚显卡驱动；远程桌面、向日葵/ToDesk 之类的虚拟显示驱动（IDD）、双显卡外接屏切换是常见原因；换一个会话/显示器再试 |
   | `D2D_AND_GDI_BROKEN` | 两条都看不见 | 先查显示驱动和桌面会话本身，与应用无关 |
   | `D2D_PROBE_FAILED` | 连渲染目标都建不出来 | 显卡驱动未装好，或会话没有可用的显示设备 |

**为什么截图不可靠**：DirectComposition 的内容（例如导航药丸）能被普通屏幕截图截到，
Direct2D 的 HWND 呈现被丢掉时截出来的就是白色——所以「截图是白的」既可能是应用没画，
也可能是机器没显示。探针用「D2D 红 + GDI 绿」这一对像素把两种情况分开；只看截图会把
机器问题误判成应用 bug。

## 右键菜单里没有 PrismArchive

**症状**：任意文件上右键，无论一级菜单还是「显示更多选项」里，都找不到
「用 PrismArchive 打开」和「用 PrismArchive 解压到此处」；但双击归档能打开，
或者在命令行里跑 `PrismArchive.exe a.7z` 一切正常。

**原因**：右键动词不是文件关联。文件关联只负责「双击时用谁打开」；右键菜单里的两条
是**静态动词**，写在：

```text
HKCU\Software\Classes\*\shell\PrismArchiveOpen\command
HKCU\Software\Classes\*\shell\PrismArchiveExtract\command
```

它们由 `install.ps1` 调用**同目录的** `register-static-verbs.ps1` 写入
（注册在 `*` 上，所以对所有文件都出现，不依赖扩展名关联）。出现这条症状通常是：

1. 安装包/解压目录里没有 `register-static-verbs.ps1`，安装器跳过了这一步（旧包）；
2. 从源码树跑 `install.ps1`，脚本同级有它，但安装过程报了警告；
3. 动词写进去了，但 Explorer 还缓存着旧的处理器列表；
4. 命令指向的 exe 已经被删掉/移动。

**解决**：

1. 跑上面的自检第 2 条。两个都是 `False` → 继续第 2 步；有一个是 `True` → 跳到第 3 步。
2. 在 `install.ps1` 所在目录重跑一次（zip 包就是解压目录）：

   ```pwsh
   pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```

   正常的最后一行是 `INSTALL: OK <安装目录> (22 associations, engine=<7z.dll 路径>)`。
   若打印 `WARN  static verbs skipped: ...`，说明同目录缺 `register-static-verbs.ps1`，
   到仓库的 `tools\` 目录把它复制过来再跑一次。
3. 重启资源管理器（右键菜单处理器按进程缓存）：任务管理器 → Windows 资源管理器 →
   重新启动；或者注销再登录。
4. 确认注册表里的命令指向真实存在的 exe：

   ```pwsh
   (Get-ItemProperty 'HKCU:\Software\Classes\*\shell\PrismArchiveExtract\command').'(default)'
   ```

5. 修不好也不影响干活：解压用 `PrismArchive.exe --extract-here "<归档>"`，压缩用
   `--compress-7z` / `--compress-zip`。

## 右键菜单在「显示更多选项」里

**症状**：右键一级菜单里没有，点开最下面的「显示更多选项」才看到两条 PrismArchive 动词。
或者按 `Shift+F10` 直接看到。

**原因**：Windows 11 把右键菜单拆成两层。一级菜单只收**有包标识**（已签名 MSIX 稀疏包）
的 `IExplorerCommand` 处理器；绿色 zip/`install.ps1` 这条安装线没有包标识、没有签名，
进不去一级菜单。经典菜单（「显示更多选项」）收的才是
`HKCU\Software\Classes\*\shell\...` 这类传统动词——这正是安装器写的那两条。

**解决**：这不是安装失败，就在「显示更多选项」里用；键盘用户按 `Shift+F10`。
想把入口搬进一级菜单，必须自己构建并签名 MSIX 稀疏包
（`tools\msix\build-sparse.ps1`，证书主体须为 `CN=PrismArchive Contributors`），
属于开发路径，不在绿色包承诺范围内。

## 7z.dll 找不到

**症状**：启动或执行任何动词时弹窗：

```text
未找到 7z.dll，PrismArchive 需要 7-Zip 引擎才能打开归档。
已依次尝试以下路径：
  [env] ...
  [app-dir] ...
  [registry:HKLM] ...
  [path] ...
  [probe] C:\Program Files\7-Zip\7z.dll
...
```

进程退出码 `1`。

**原因**：PrismArchive **不附带** `7z.dll`（LGPL）。启动时按下面的顺序找，第一个存在的赢：

| 顺序 | 来源标签 | 路径 |
|---|---|---|
| 1 | `[env]` | 环境变量 `PRISM_7Z_DLL` 指向的完整路径 |
| 2 | `[app-dir]` | `PrismArchive.exe` 同目录的 `7z.dll` |
| 3 | `[registry:*]` | 7-Zip 自己写的 `InstallDir`：HKLM、HKLM\WOW6432Node、HKCU 三处 |
| 4 | `[path]` | `PATH` 里的每个目录（会去重） |
| 5 | `[common:*]` | `%LOCALAPPDATA%\Programs\7-Zip`、scoop 的 7zip |
| 6 | `[probe]` | 最后兜底探测两个 `Program Files\7-Zip` |

路径里的 `7-Zip` 大小写、x86/x64 都不重要；弹窗列出的是**这次真的试过的每一条**，
包括来源标签——这就是诊断信息。

**诊断信息怎么复制**：弹窗是标准 Windows 消息框，**按 `Ctrl+C` 会把整个框里的文字
（含全部候选路径）复制到剪贴板**，直接粘进 issue/聊天即可。不要截图，路径很长会截不全。
若你已经能进主界面，也可以在命令行复现同一个判定：

```pwsh
$env:PRISM_7Z_DLL   # 当前进程看到的覆盖值（可能和「用户环境变量」不同）
& "$env:LOCALAPPDATA\PrismArchive\PrismArchive.exe" --test "D:\Downloads\a.7z"
echo "exit=$LASTEXITCODE"
```

**解决**（任选一条，改完**重启 PrismArchive**——它在启动时读一次）：

1. 安装 7-Zip x64（https://www.7-zip.org/），默认路径即可；重跑 `install.ps1`，
   它会把找到的路径写进用户环境变量 `PRISM_7Z_DLL`。
2. 手动设用户变量（新开的进程才看得到）：

   ```pwsh
   [Environment]::SetEnvironmentVariable('PRISM_7Z_DLL', 'D:\Tools\7-Zip\7z.dll', 'User')
   Test-Path ([Environment]::GetEnvironmentVariable('PRISM_7Z_DLL', 'User'))
   ```

3. 把 `7z.dll` 复制到 `PrismArchive.exe` 同目录（便携场景，路径必须与 exe 同级）。
4. 把 7-Zip 目录加进 `PATH`。

验证：`Test-Path` 为 `True` 后重新打开应用；还弹同一个框，就把 `Ctrl+C` 的内容
和上面 `Test-Path` 的结果一起发出来。

## 压缩失败

**症状**：应用内状态栏 `压缩失败 <目标路径>` 加一行原因；命令行退出码 `1`，stderr 里有
`compress failed: ...` 或 `compress ok=0 ... error=...`。

**原因**：常见是计划阶段就拒绝了（不会留下半个包）：

| stderr / 原因 | 含义 |
|---|---|
| `nothing to archive` | 输入是空目录/空选择 |
| `invalid entry path (...)` | 条目路径非法（绝对路径、`..` 越界等） |
| `tar cannot encrypt: no cipher exists in the tar container` | tar + 口令，没得商量 |
| `... cannot encrypt file names: only 7z encrypts its header` | zip/tar + 加密文件名 |
| `encryptFileNames requires encrypt ...` | 勾了加密文件名但没给口令 |

也可能是文件系统层面：目标目录只读、目标被别的程序占用、磁盘满、路径过长。

**解决**：

1. **先看默认目标名**。应用内压缩的默认目标是 `<归档名>.7z`，放在归档旁边；
   如果你打开的本来就是 `a.7z`，默认目标会和源归档同名。改一个目标名再压。
2. 换一个可写目录作为目标，确认磁盘有空间：

   ```pwsh
   Get-PSDrive -PSProvider FileSystem | Select-Object Name, Free
   ```

3. 关掉占用目标文件的程序（预览器、同步盘、杀软实时扫描）后重试。
4. 用命令行拿到确定的一行原因（GUI 的 HUD 可能只显示第一行）：

   ```pwsh
   & "$env:LOCALAPPDATA\PrismArchive\PrismArchive.exe" --compress-7z "D:\work\a.txt"
   echo "exit=$LASTEXITCODE"
   ```

5. tar + 口令、非 7z + 加密文件名是**故意拒绝**，换成 7z 或去掉对应选项。

## 中文文件名或预览文本乱码

**症状**：解压出来的文件名是 `ÐÂ½¨ÎÄ¼þ¼Ð` 这类乱码；或者 Quick Look 预览一段中文
文本，满屏问号/方块。

**原因**：

- **文件名**由 `7z.dll` 解码后以 UTF-16 交给界面，PrismArchive **不会二次改写**名字
  （改写会把本来正确的中文名改坏）。ZIP 条目名是否带 UTF-8 标志由**打包方**决定；
  没有标志时按「打包机器的代码页」解释，在另一台机器上就可能显示成乱码。
- **预览文本**按 BOM → UTF-8 → GBK/GB18030 的顺序判定，再交给 Windows NLS 转码。
  两者都不像时按 UTF-8 显示，于是就成了乱码。

**解决**：

1. 用**同版本 7-Zip** 打开同一个包对照。7-Zip 也乱 → 是包的问题，不是 PrismArchive：
   用会写 UTF-8 名称的工具重新打包（7-Zip 的「压缩」对话框里有相关选项），
   或在系统「区域设置 → 非 Unicode 程序的语言」里选对代码页后重新解压。
2. 7-Zip 正常、PrismArchive 乱 → 是我们的解码差异，请把 `--test` 的输出和包的一小块
   样本发出来（`--test` 不写文件，安全）。
3. 预览乱码时先确认文件本身是否是 UTF-8：`Get-Content -Encoding Byte` 看前几个字节；
   纯 GBK 文本在转码器不可用的精简系统上会直接报「没有转码器」而不是硬猜。
4. 不要在路径里用「看起来像 UTF-8 其实是 GBK」的混合名做实验——重名/覆盖问题会掩盖乱码问题。

## 归档只读或被占用

**症状**：编辑菜单项是灰的；或者按 `Delete`/`F2` 后弹窗/状态栏给出一句原因，归档没变。

**原因**：编辑 = **整包重写**（copy-on-write）：先写临时副本，校验通过后再替换原文件。
所以下面四种情况在动手前就会被拒绝，并给出原文：

| 原因（原文） | 触发条件 |
|---|---|
| 归档是只读的：编辑会替换整个文件，请先清除只读属性 | 文件带 `FILE_ATTRIBUTE_READONLY` |
| 归档被其他程序独占：请关闭占用它的程序后再编辑 | 别的进程以不共享的方式打开了它 |
| 归档是分卷集（.001/.002）：重写只会产生一个文件，无法替换分卷 | `.001/.002` 分卷 |
| 这个容器格式不支持编辑（只支持 .7z/.zip/.tar） | rar/gz/iso 等容器 |

另外，解压或扫描正在进行时，第二个写操作会被拒绝：
`正在解压或扫描，稍后再编辑`。

**解决**：

1. 清只读：

   ```pwsh
   attrib -R "D:\Downloads\a.7z"
   ```

2. 关掉可能持有它的程序：另一个 PrismArchive 窗口、7-Zip、同步盘、杀软扫描、
   资源管理器预览窗格。用「资源监视器 → CPU → 关联的句柄」搜文件名可以确认是谁。
3. 只读介质（光盘、挂载的 ISO、受控目录）上的归档：先复制到本地磁盘再编辑。
4. 分卷/不支持的容器：解压后重新打包成 `.7z`。
5. 只是**解压**被拒（`拒绝访问：<path>`）：目标目录需要写权限，换目录或用
   `--extract-sub` 落到归档旁边。

## 关联被别的软件锁定（双击还是打开别的程序）

**症状**：安装成功，但双击 `.7z` 仍然打开 Bandizip/7-Zip/资源管理器自带解压；
`install.ps1` 的输出里有：

```text
WARN  .7z is pinned by the Windows user choice (Bandizip.7z, UserChoiceLatest).
```

`install.ps1 -Verify` 退出码是 `3`，并打印 `VERIFY: OS-ADOPTION-PENDING`。

**原因**：Windows 把「某个扩展的默认程序」存成**带哈希保护的用户选择**：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.7z\UserChoiceLatest\ProgId
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.7z\UserChoice      (旧版)
```

它的优先级高于 `HKCU\Software\Classes`，且**非提权安装器不得改写**（这是 Windows 的
防劫持设计，对所有安装器一视同仁）。安装器能保证的是 ProgID、图标、open 命令、
「打开方式」列表都正确；不能保证把一个已有默认程序静默换掉。

**解决**（一次即可）：

1. 右键那个 `.7z` → **打开方式** → **选择其他应用** → PrismArchive → 勾「始终使用」。
2. 或 **设置 → 应用 → 默认应用** → 按文件类型选 `.7z` → PrismArchive。
3. 验证当前钉住的是谁：

   ```pwsh
   $base = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.7z'
   Get-ItemProperty "$base\UserChoiceLatest\ProgId" -ErrorAction SilentlyContinue
   Get-ItemProperty "$base\UserChoice" -ErrorAction SilentlyContinue
   ```

   看到 `PrismArchive.7z` 就成了；还是别的 ProgID，说明用户选择还没改。
4. 想一次看全：`install.ps1 -Verify`。退出码 0 = 注册表 + exe + 系统都通过；
   3 = 我们这边全对、系统没采纳（就是本条）；1 = 真失败。

## 安装脚本说找不到 PrismArchive.exe

**症状**：在解压目录里跑 `.\install.ps1`，报：

```text
PrismArchive.exe not found. Build first (cmake --build <dir>) or pass -SourceDir. Tried: ...
```

**原因**：旧版脚本只在**构建目录**里找 payload，不看脚本自己所在的目录；从 zip 解压出来
的布局（exe 与脚本同级）因此找不到。这一版已改为**先看脚本目录**，再退回构建目录。

**解决**：

1. 确认 `PrismArchive.exe` 与 `install.ps1` 在同一个目录（zip 包解压后就是这样）。
2. 显式指定来源目录（最稳）：

   ```pwsh
   pwsh -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -SourceDir .
   ```

3. 从源码树安装时先构建，或设 `PRISM_BUILD_DIR` 指向含 `PrismArchive.exe` 的目录。
4. 先空跑看它打算做什么：加 `-DryRun`（不落盘、不写注册表）。

## 卸载后右键还留着 PrismArchive

**症状**：卸载完成，右键菜单里仍有两条动词，点了提示找不到程序。

**原因**：旧版卸载只清文件关联与 ProgID，没有删静态动词那两个键
（`HKCU\Software\Classes\*\shell\PrismArchive*`）。这一版卸载会一起清掉。

**解决**：

```pwsh
foreach ($id in 'PrismArchiveExtract','PrismArchiveOpen') {
  $p = "HKCU:\Software\Classes\*\shell\$id"
  if (Test-Path $p) { Remove-Item -Path $p -Recurse -Force; "removed $p" }
}
```

然后重启资源管理器。重装一次再卸载也可以（新卸载器会清理）。

## 还是不行：请给我这些

发 issue 时附上下面四样，能省掉大部分来回：

1. **版本**：`PrismArchive.exe` 属性 → 详细信息 → 文件版本（例如 `0.1.0.0`）。
2. **原文**：错误弹窗按 `Ctrl+C` 复制全文（不要截图）；或命令行的完整 stderr。
3. **复现命令与退出码**：`... ; echo "exit=$LASTEXITCODE"`。
4. **环境**：Windows 版本、7-Zip 版本与 `PRISM_7Z_DLL` 的值（路径可以打码，
   但保留盘符与目录层级）。

排查时不要删 `%LOCALAPPDATA%\PrismArchive\settings.json` 和 `session.json` 来「试试」——
它们会被自动备份成 `*.bak`，但删掉会丢掉现场。
