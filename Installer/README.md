# Installer / 分发（卡片 C3）

PrismArchive 有两条分发路径，**注册表内容逐字节一致**，区别只在于谁来执行：

| 路径 | 场景 | 权限 | 关联扩展 | 可测性 |
|---|---|---|---|---|
| `tools/install.ps1` | 开发机、免管理员、绿色部署 | HKCU / %LOCALAPPDATA% | 22 个 | `-DryRun` / `-SelfTest` / 5 个变异体 / `-Verify` |
| `Installer/prism.iss` (Inno Setup 6) | 正式发布单个 setup.exe | `PrivilegesRequired=lowest` | 同 22 个 | ISCC 编译 + 静默安装/卸载冒烟 |

## 快速开始（免管理员）

    . E:\wg\env.ps1 -Card c3                 # 本仓库的开发环境脚本
    pwsh -File tools\install.ps1 -DryRun     # 先看它打算写什么
    pwsh -File tools\install.ps1             # 真装
    pwsh -File tools\install.ps1 -Verify     # 读回注册表 + 真启动
    pwsh -File tools\uninstall.ps1           # 卸干净（还原安装前的 handler）
    pwsh -File tools\install.ps1 -Portable -InstallDir D:\Apps\PrismArchive   # 便携：零注册表

安装位置：`%LOCALAPPDATA%\PrismArchive`（exe + 两个 shell 扩展 DLL + `Assets\` + `uninstall.ps1`）。

写入的东西全部在 HKCU：

    HKCU\Software\Classes\<ext>                      默认值 = PrismArchive.<ext>
    HKCU\Software\Classes\<ext>\OpenWithProgids      REG_NONE 标记
    HKCU\Software\Classes\PrismArchive.<ext>         "" / DefaultIcon / shell\open\command
    HKCU\Software\Classes\Applications\PrismArchive.exe\...  shell\open\command / SupportedTypes / DefaultIcon
    HKCU\Software\PrismArchive                       安装标记 + AssocBackup\<ext>（原 handler 备份）
    HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\PrismArchive
    HKCU\Environment                                 PRISM_7Z_DLL（找到 7-Zip 时写，保留原值以便还原）

**7z.dll 不入库**（LGPL，蓝图 ch.2）。安装器按引擎自己的解析顺序找：`PRISM_7Z_DLL` → 7-Zip 注册表
`Path/Path64` → 标准安装目录 → `PATH`。找不到时照常装好，只告警。

## 自测与负向控制（D1 / D3）

    pwsh -File tools\install.ps1 -SelfTest -Quiet        # SELFTEST: PASS (n assertions)
    pwsh -File tools\install.ps1 -SelfTest -Quiet -Mutate SkipDefaultIcon
    pwsh -File tools\install.ps1 -SelfTest -Quiet -Mutate NoBackup
    pwsh -File tools\install.ps1 -SelfTest -Quiet -Mutate NoRestore
    pwsh -File tools\install.ps1 -SelfTest -Quiet -Mutate UnquotedVerb
    pwsh -File tools\install.ps1 -SelfTest -Quiet -Mutate Rebackup

自测在一个丢弃式注册表根（`HKCU\Software\PrismArchiveSelfTest`）和临时目录里跑完整回合：
DryRun 纯净性 → 安装 → 幂等重装（逐字节比对整棵注册表子树）→ 还原关联 → 完全卸载（与安装前逐字节一致）
→ 幂等卸载 → 便携零注册表。变异体必须让断言变红并打印 `SELFTEST(MUTATION X): CAUGHT (n ...)`；
若注入没有生效，打印 `NOT CAUGHT` 并 exit 1。这是"注入 bug 必须变红"的守卫，不是跑通就行。

## `-Verify` 的三种结局（不要把它当成一把绿锁）

| 退出码 | 含义 |
|---|---|
| 0 | 注册表 + 安装后的 exe + **系统把该扩展交给我们**（ShellExecute 真启动了 PrismArchive） |
| 3 | 注册表与 exe 都验证通过，但**系统没有采纳**这个新 ProgID 为默认（见下） |
| 1 | 真失败（键写错、exe 不响应参数、启动的是别的应用） |

`-Verify` 会先**校准** shell 解析器：若 `AssocQueryString` 连 `.txt` 都解析不出来，就明确报
`VERIFY: SKIP shell resolver ... uncalibrated`（CONTRACTS D4：没校准的解析器不许当证据），
而不是把自己的解析失败说成"注册失败"。

### 为什么退出码 3 是常态（2026 年的 Windows）

当前 Windows 把"某个扩展的默认程序"存成**带哈希保护的用户选择**
（`FileExts\<ext>\UserChoiceLatest\ProgId`，旧版是 `UserChoice`，值名还嵌在子键里）。
该记录优先级高于 `HKCU\Software\Classes`，且**非提权安装器不得改写**。因此：

- 我们能保证：ProgID / 图标 / open 命令 / Applications 注册全部正确，HKCR 合并视图可见，
  "打开方式"列表出现 PrismArchive；
- 我们不能保证（也不该假装）：把一个已有默认程序的扩展**静默**改成我们。

实测本机：`.7z` 被 Bandizip 的 `UserChoiceLatest` 钉住 → 双击仍是 Bandizip；
`.zip` 被 Explorer `CompressedFolder` 钉住；`.cab` / `.rpm` 同理。安装器会逐条打印
`WARN  <ext> is pinned by the Windows user choice (...)` 并给出动作：**设置 → 默认应用**，
或对文件右键 **打开方式 → PrismArchive**（一次即可）。这条限制对所有安装器（含 Inno/MSIX）一样成立。

## Inno Setup（正式发布）

Inno Setup 6.7.3 已用 winget 装到 `E:\DevTools\InnoSetup`（**不进 C 盘**）：

    winget install --id JRSoftware.InnoSetup --exact --silent --location E:\DevTools\InnoSetup

编译（已实测通过，产物 `Installer\dist\PrismArchive-0.1.0-setup.exe`，约 2.3 MB）：

    pwsh -File Installer\build-installer.ps1 -PayloadDir E:\build\wt-c3\Src\PrismWin

或直接：

    E:\DevTools\InnoSetup\ISCC.exe Installer\prism.iss /DPayloadDir=E:\build\wt-c3\Src\PrismWin

静默安装/卸载冒烟（也实测过，装到临时目录、不动关联）：

    PrismArchive-0.1.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=E:\Temp\prism-inno /NOICONS /TASKS=""
    E:\Temp\prism-inno\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

**未做**：代码签名（证书不在本机）。setup.exe 与 exe 都未签名，发布前必须签，否则 SmartScreen 会拦。

## 与 `cmake --install` 的关系

`Src/PrismWin/CMakeLists.txt` 的 install 规则产出**与两个安装器完全相同的树**：

    cmake --install E:\build\wt-c3 --prefix E:\Temp\prism-stage

这让 payload 可以在不运行任何安装器的情况下被审阅/对比。
