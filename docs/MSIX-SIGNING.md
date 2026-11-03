# MSIX 免证书路径 —— 松散注册、自签安装、对外分发

> **一句话**：Win11 新式右键菜单只对**打包应用**开放，但"打包"不等于"买证书"。
> 本机自用可以零证书（松散注册），验证真实 MSIX 安装形态可以用自签证书（一次管理员信任根），
> 对外分发用 OSS 免费签名（如 SignPath）或让用户自行信任自签证书。
> 本项目**不依赖任何付费证书**，也不会把私钥放进仓库。

全部脚本都在 `tools/msix/` 下：

| 脚本 | 作用 |
|---|---|
| `register-loose.ps1` | 松散文件注册：不生成 MSIX、不签名、不需要管理员 |
| `unregister-loose.ps1` | 按包名移除，幂等（没装时打印 NO-OP 并成功退出） |
| `selfsign-and-install.ps1` | 自签证书 → 签名 → 信任根 → 安装（真 MSIX 形态） |
| `msix-common.ps1` | 上面三个脚本共用的清单解析、HRESULT 解释、SDK 工具查找 |
| `Test-MsixFree.ps1` | 自测：双解析器语法/编码/清单/计划/负向夹具/0x80073CFF 分支/HRESULT 表/变异；CI 走 `Tests/suites/c61-msix-free.cmake`（`-SyntaxOnly` 是独立 ctest） |
| `enable-dev-mode-and-register.cmd` | 开发者模式开关脚本（一次 UAC），然后调用 `register-loose.ps1`；松散注册失败时的 FIX 行会打印它的绝对路径 |

---

## 1. 路径取舍表（三条免证书路线 + 两条分发路线 + 一条付费对照）

| 路径 | 入口 | 费用 | 需要管理员 | 证书 / 签名 | 需要开发者模式 | 能否分发给别人 | 适用场景 |
|---|---|---|---|---|---|---|---|
| ① 松散注册（loose） | `register-loose.ps1` | **0 元** | **否** | **否** | **是** | 不能（只在本机、每个用户各自注册） | 自己机器上天天用；改完 DLL 重跑一次即可 |
| ② 自签 + 打包安装 | `selfsign-and-install.ps1` | **0 元**（自签） | 仅"信任根到 LocalMachine"时一次 | 自签证书 | 否 | 不能（对方必须先信任你的自签证书） | 验证"真正的 MSIX 安装"路径、演示、录屏 |
| ③ 对外分发（免费签名） | SignPath 等 OSS 签名服务 + 现有 `build-sparse.ps1` | **0 元**（OSS 计划） | 否（签名在云端/CI 做） | 免费 OSS 证书 | 否 | **能**（公共信任链） | 正式发布给陌生用户 |
| ④ 对外分发（自签 + 用户信任） | 发出 .msix + .cer + 一页说明 | **0 元** | 用户侧一次 | 自签证书 | 否 | 能，但每个用户都要手动信任根 | 小范围测试者、内部团队 |
| ⑤ 不打包，只装经典菜单 | `register-classic.ps1` | **0 元** | 否 | **否** | 否 | 能（.exe + 注册脚本） | 不要新式菜单、只要能用的右键菜单 |
| ⑥ 买商业代码签名证书 | 任何 CA（DigiCert / Sectigo / ...） | **每年 2000–6000 元起** | 否 | 公共信任的 CA 证书 | 否 | **能**（最省用户事） | **本项目不采用**：开源项目不把发布绑在个人钱包上 |

**结论**：想要新式菜单又要分发给陌生人 → 只有 ③（免费 OSS 签名）是可持续路线；
② 和 ④ 是"本机/小圈子"路线；① 是开发时最快的一条，连证书都不用；
⑥ 能用钱解决，但**不是必须的**——本卡的目标就是证明"免证书"这条路走得通。

---

## 2. 路径 ①：松散注册（零证书、零管理员）

前提：Windows 设置 → 系统 → 开发者选项 → **开发人员模式** 打开（或公司策略允许旁加载）。
松散布局没有签名，Windows 只在开发者模式下接受未签名布局的注册。

省事做法：**没开开发者模式时脚本会把开关脚本的完整路径直接打给你**——
`tools/msix/enable-dev-mode-and-register.cmd`。它做两件事：提权一次写
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock` 的两个值，然后回来调用
`register-loose.ps1`。这条提示不是文案：`Test-MsixFree.ps1` 断言打印出来的 FIX 行里含该
`.cmd` 的**绝对路径**（`register-devmode-off:which-script`），并断言这个 `.cmd` 里恰好有两条
`reg add`、含 `AllowDevelopmentWithoutDevLicense` 与 `AllowAllTrustedApps`、且会自我提权
（`RunAs`）。

```powershell
# 1) 先有 stage 目录：build-sparse.ps1 会把清单 + 图标 + PrismShellExt.dll 铺进去
.\tools\msix\build-sparse.ps1 -RepoRoot . -ShellExtDll .\build\Src\PrismWin\PrismShellExt.dll -OutDir .\build\msix -NoSign

# 2) 注册（-ExternalLocation 指向 build\Src\PrismWin，即 PrismArchive.exe 所在目录）
.\tools\msix\register-loose.ps1 -BuildDir .\build

# 3) 先看计划，不动系统
.\tools\msix\register-loose.ps1 -BuildDir .\build -WhatIf

# 4) 不想看了：按包名移除（幂等）
.\tools\msix\unregister-loose.ps1
```

**它到底做了什么**（实测，不是文档抄的）：`Add-AppxPackage -Register <stage>\AppxManifest.xml -ExternalLocation <app dir>`。
少一个 `-ExternalLocation` 就会失败——见下面第 6 节的 0x80073CF9。

**失败时脚本会给出可直接执行的原因**：

| HRESULT | 脚本给出的 REASON | 你要做的事 |
|---|---|---|
| `0x80073CF9` | 布局需要外部位置但没给 | 传 `-ExternalLocation <PrismArchive.exe 所在目录>`（脚本已默认传，出现即说明 build 目录布局不对） |
| `0x80073D2E` | 清单不允许外部内容 | 恢复 `<uap10:AllowExternalContent>true</uap10:AllowExternalContent>`，重新 `build-sparse.ps1` |
| `0x80080204` | 清单无效（缺 Identity/@Publisher、版本不是四段等） | 脚本上方已逐条打印清单错误；`python tools/check_manifest.py <清单>` 再核一遍 |
| `0x80073CFF` | 开发者模式/旁加载策略关闭 | 跑 `tools\msix\enable-dev-mode-and-register.cmd`（脚本会把它的绝对路径打出来），或 `start ms-settings:developers` |
| `0x800B010F` | 签名主体 ≠ `Identity/@Publisher` | 用 `new-dev-cert.ps1` 默认的 `CN=PrismArchive Contributors` 签名 |
| `0x800B0109` | 证书根不受信任（只可能出现在打包路径） | 用 `selfsign-and-install.ps1` 或走路径 ① |

清单里缺 `PrismShellExt.dll` 时脚本只 **WARN**：实测注册照样成功（包计数 0→1），
只是菜单点不出来——因为 Explorer 从包根目录加载 COM 类。重跑 `build-sparse.ps1` 即可。

### 2.1 开发者模式没开时（0x80073CFF）

用户实测（2026-11-02，Win11 26200）：关掉开发者模式后，`Add-AppxPackage -Register` 返回
`0x80073CFF`（"此应用包无法安装，因为未启用旁加载/开发者模式"）。脚本在这里**不会**抛一句
HRESULT 了事，而是先跑分支、再给出带路径的动作：

```powershell
# 关掉开发者模式时，register-loose.ps1 打印（真实输出，非示意）：
#   BLOCKER dev-mode-off: an unsigned loose layout needs Developer Mode (Windows answers 0x80073CFF otherwise)
#   FIX       run <仓库>\tools\msix\enable-dev-mode-and-register.cmd  (one UAC prompt: ...)
#   FIX       or, elevated: Set-ItemProperty HKLM:\...\AppModelUnlock -Name AllowDevelopmentWithoutDevLicense -Value 1
# 直接执行它即可（弹一次 UAC，设两个值，然后回来调用 register-loose.ps1）：
.\tools\msix\enable-dev-mode-and-register.cmd

# 只看计划、不动系统（没有开发者模式时也退出 0，方便 CI 断言整段计划）：
.\tools\msix\register-loose.ps1 -BuildDir .\build -WhatIf
```

CI 里怎么证明这条分支没被删掉？不写 HKLM（runner 没有管理员，也不该动用户态设置），
而是用**测试缝** `PRISM_MSIX_DEV_MODE=off|on`：它只改变脚本"认为"的开关状态，不改变任何写入。
`register-devmode-off:*` 六条断言覆盖：计划退出 0、打印 BLOCKER、打印 `.cmd` 绝对路径、
真跑退出 1 且 reason=dev-mode-off、FIX 里仍有该路径、以及包计数仍为 0（**没有真的部署**）。
反向 `register-devmode-on:no-blocker` 证明这个缝的另一侧也有效。

---

## 3. 路径 ②：自签证书 + 打包安装（真 MSIX 形态）

这条路径验证的是"用户从 .msix 安装"的完整链路。证书免费（自签），但**信任链**要落到本机存储：

- `Cert:\CurrentUser\Root`：**不需要管理员**，只对当前用户生效（脚本默认走这条）；
- `Cert:\LocalMachine\Root`：需要**一次管理员**（`-Machine` 开关），对整机生效。

```powershell
# 非管理员：证书进 CurrentUser，打包 + 签名 + 安装一条龙
.\tools\msix\selfsign-and-install.ps1 -BuildDir .\build

# 需要整机信任（或给别的账户用）：一次提权
Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','.\tools\msix\selfsign-and-install.ps1','-Machine'

# 先看计划：不建证书、不导入、不安装
.\tools\msix\selfsign-and-install.ps1 -BuildDir .\build -WhatIf
```

脚本逐步打印 `CERT` / `SIGN` / `TRUSTED` / `INSTALLED`，每一步失败都有 `REASON` + `FIX`：

1. **证书**：调用已有的 `new-dev-cert.ps1`（主体必须等于清单 `Identity/@Publisher`，否则签名安装报 `0x800B010F`）。
2. **签名**：调用 `build-sparse.ps1`（makeappx 打包 + signtool 签名），找不到 SDK 工具时明确报
   `BLOCKER tool-missing tool=makeappx` 并给出 SDK 路径，而不是一句 "not found"。
3. **信任根**：自签证书**自己就是根**，只导入 `TrustedPeople` 不够（`CurrentUser\Root` 里没有它时，
   部署会报 `0x800B0109`）。脚本先查根存储里有没有该指纹，没有才导入。
4. **安装**：`Add-AppxPackage -Path <signed .msix>`，装完再用 `Get-AppxPackage` 复核（报成功但没装上 = FAIL）。
5. 卸载命令脚本会直接打印：`Remove-AppxPackage -Package <full name>`。

**PFX 口令**：不传就随机生成，任何情况下都只打印 `PFX-PASSWORD <redacted>`，不会写进日志（契约 B7）。

**关于弹出的确认框**：往 `CurrentUser\Root` 导入证书时 Windows 可能弹一次"是否信任"对话框
（`LocalMachine\Root` 不会有，但需要提权）。脚本没有也不该有"无限等待"：看到对话框点"是"，
或者在无人值守环境里先手动导入：

```powershell
Import-Certificate -FilePath .\build\certs\PrismArchive-dev.cer -CertStoreLocation Cert:\CurrentUser\Root
```

---

## 4. 路径 ③：对外分发（不花钱）

三条免费路线，按"用户麻烦程度"排序：

1. **OSS 免费签名（推荐）**：SignPath.io 的 open-source plan 对公开仓库免费签发，
   证书链是公共信任的，用户双击 .msix 即可安装。把 `build-sparse.ps1 -Pfx <CI 里拿到的 .pfx>` 接到流水线即可，
   仓库里不放私钥。同类还有部分 CA 的 OSS 计划；**本项目不引入任何付费依赖**。
2. **自签 + 用户自行信任**：发布 .msix + PrismArchive-dev.cer + 两行命令。
   用户信任你的根之后才能装；证书轮换/泄露都要重新信任，只适合小范围。
3. **不打包，只发经典菜单**：`install.ps1`（或 `register-classic.ps1`）+ 便携包。
   没有新式菜单，但零证书、零签名、零管理员。

```powershell
# ③a OSS 签名（推荐）：.pfx 由 CI 的签名服务提供，仓库里不放私钥
.\tools\msix\build-sparse.ps1 -RepoRoot . -ShellExtDll .\build\Src\PrismWin\PrismShellExt.dll `
    -OutDir .\build\msix -Pfx $env:PRISM_MSIX_PFX -PfxPassword $env:PRISM_MSIX_PFX_PASSWORD

# ③b 自签 + 用户自行信任（小范围）：发布 .msix 与 .cer，用户在自己机器上信任根
.\tools\msix\selfsign-and-install.ps1 -BuildDir .\build
Import-Certificate -FilePath .\build\certs\PrismArchive-dev.cer -CertStoreLocation Cert:\CurrentUser\Root
```

> 为什么不买证书：代码签名证书是**付费**的（商业 CA 每年数千元），开源项目不该把发布流程
> 绑在个人钱包上。需要公共信任时用 OSS 计划，需要本机自用时用自签/松绑，三条都 0 元。

---

## 5. 为什么经典菜单不需要证书

两条右键菜单的加载机制根本不是一回事：

| | Win10 经典菜单（`IContextMenu`） | Win11 新式菜单（`IExplorerCommand`） |
|---|---|---|
| 入口 | `HKCU\Software\Classes\<ext>\shellex\ContextMenuHandlers\...` | `AppxManifest.xml` 的 `desktop4:Extension` / `desktop5:Verb` |
| 谁加载 | Explorer 直接 `LoadLibrary` 注册表里写的 DLL 路径 | AppX 部署系统先认包身份，再由包里的 COM Surrogate 激活 |
| 需要包身份 | **不需要** | **需要**（稀疏包就是为了给纯 Win32 程序一个身份） |
| 需要签名 | **不需要**（注册表写入即可，键是用户自己写的） | 打包安装需要（证书主体必须等于 `Identity/@Publisher`） |
| 权限 | 写 `HKCU` 即可，无需管理员 | 松散注册需开发者模式；签名安装需信任根 |

所以 `register-classic.ps1` 里**一个签名/打包 API 都没有**：它只写 `HKCU` 的 CLSID 与
`shellex\ContextMenuHandlers` 键。这条性质不是注释，是断言：
`Test-MsixFree.ps1` 的 `classic-path:cert-free` 统计 `Add-AppxPackage|makeappx|signtool|New-SelfSignedCertificate|Import-Certificate`
在该脚本里出现 **0** 次。谁哪天给经典路径塞进签名步骤，这条断言会直接变红——
负向控制 `-Mutate DropClassicCertFree` 证明它真的会红（往脚本里塞一个 `Add-AppxPackage` 记号即可）。

```powershell
# ⑤ 只装经典菜单：零证书、零签名、零管理员（只写 HKCU）
.\tools\msix\register-classic.ps1 -DllPath .\build\Src\PrismWin\PrismShellExtClassic.dll

# 卸载（同样只动 HKCU）
.\tools\msix\register-classic.ps1 -Unregister
```

---

## 6. 实测记录（2026-11-02，Win11 26200，非管理员，开发者模式=1）

都在同一台开发机上用 `pwsh 7.6` 跑出来，命令与结果都可复现：

| 实验 | 命令 | 结果 |
|---|---|---|
| 只 `-Register` | `Add-AppxPackage -Register <stage>\AppxManifest.xml` | **FAIL 0x80073CF9**："必须使用外部位置安装它"（包计数 0） |
| 加 `-ExternalLocation` | `Add-AppxPackage -Register <stage>\AppxManifest.xml -ExternalLocation <stage>` | **成功**，`Get-AppxPackage` 计数 1，`Remove-AppxPackage` 后回到 0 |
| 去掉 `AllowExternalContent` | 同上 | **FAIL 0x80073D2E**："必须允许使用外部位置部署外部内容" |
| 清单 XML 截断 | 同上 | **FAIL 0x80080204**："Appx 包的清单无效" |
| 删掉 `Identity/@Publisher` | 同上 | **FAIL 0x80080204**（清单无效） |
| 删掉包里引用的 `PrismShellExt.dll` | 同上 | **成功**（计数 1）——所以缺 DLL 只 WARN，菜单才是死的 |
| 非管理员 + 开发者模式=1 | 上述成功实验 | 无需提权，无需证书 |
| **开发者模式=0** | 同上 | **FAIL 0x80073CFF**（用户实测；切换开关要管理员，所以 CI 用 `PRISM_MSIX_DEV_MODE=off` 复现同一分支） |

> 注意：`Add-AppxPackage` 的 `Exception.HResult` 是 `0x80131620`/`0x80131500`（PowerShell 包装层），
> 真正的部署码在**消息文本**里（`Deployment failed with HRESULT: 0x80073...`）。
> `msix-common.ps1` 的 `ConvertFrom-DeployError` 先正则取消息里的码，再回退到 `HResult`——只取 `HResult` 会映射错。

**一个由断言抓出来的真 bug（2026-11-03）**：这张映射表原先写成
`switch ([uint32]$code) { 0x80073CFF { ... } }`。PowerShell 里 `0x80073CFF` 是**负的 Int32**
（`-2147009281`，因为 `0x80073CFF > 0x7FFFFFFF`），于是 `0x80073CFF`、`0x800B0109`、`0x800B010F`
三行是**死代码**：脚本永远打印不出对应的 REASON/FIX，而注释和文档都"看起来"覆盖了它们。
改成按 8 位大写十六进制**文本**查表后，`hresult:0x80073CFF` / `hresult:0x800B0109` /
`hresult:0x800B010F` 每行一条断言；负向控制 `-Mutate ShiftHresultKey`（把一个键挪到别的码）必须红。
> 顺带一条实测：PowerShell 的 `switch` **默认不区分大小写**，所以"把键改成小写"根本不构成缺陷
> （这一版负向控制第一跑是绿的），最终的控制是改键值。

---

## 7. 自测（CI 上跑什么）

```powershell
# 全部脚本的语法（AST 解析 + [scriptblock]::Create 双解析）——独立 ctest：win.msix_free_syntax
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -SyntaxOnly

# 全量断言（不注册、不导入、不安装；证书存储前后计数相等）
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1

# 负向控制：把某个守卫注掉，套件必须变红才算 CAUGHT
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropExternalLocation
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropManifestGate
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropPublisherGate
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropDevModeReport
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate LeakPassword
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate BreakUnregisterNoOp
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate BreakSyntax
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropDevModeFix
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate DropClassicCertFree
pwsh -NoProfile -File .\tools\msix\Test-MsixFree.ps1 -Mutate ShiftHresultKey
```

ctest 名字：`win.msix_free_syntax`（语法，120s 预算）、`win.msix_free_selftest`（全量断言）、
`mutation.c61.msix_free.<name>`（十条负向控制，每条与全量同样的 600s 真实预算；输出 `MSIX-FREE(MUTATION <name>): CAUGHT` 才算数，
且是在**第一次**红的就是期望那条时立刻打印——CONTRACTS D27）。

断言覆盖：`tools\msix` 下**全部 9 个 `.ps1`** 双解析器无语法错误、纯 ASCII 无 BOM（Windows PowerShell 5.1
按 ANSI 读无 BOM 文件）、`-WhatIf` 能力、参数表；清单能被解析且与 `SparsePackageRegistrar.hpp` 里的
包名/发布者/CLSID 一致；`Add-AppxPackage` 调用带 `-ExternalLocation`；松散注册计划不产生包；
四个坏清单各自被拒绝并给出对应 reason；`0x80073CFF` 分支（计划/FIX 路径/真跑拒绝/包计数 0）；
12 行 HRESULT 映射逐行命中 + 未知码走 default；未安装时注销幂等；自签计划不建证书、不泄露 PFX 口令、
`-Machine` 在非管理员下给可执行的提权命令；文档包含取舍表（含费用/管理员/分发三列）、
每条路径的完整命令与实测 HRESULT；夹具目录用完即删（D9）。

CI 上**不做真实注册/安装**（runner 没有交互桌面，且不该污染用户态包数据库）。
唯一的"真跑"是 `PRISM_MSIX_DEV_MODE=off` 下的 `register-loose.ps1`：它在 `Add-AppxPackage` 之前就拒绝，
断言恰好证明"没部署"（包计数 0）。真安装由人在这台机器上按第 2、3 节跑。

---

## 8. 已知限制

- **`0x80073CFF` 由用户在本机实测**（关掉开发者模式后松散注册失败），但**切换开关需要管理员**，
  所以 CI 不能把机器真的切到那个状态：套件用 `PRISM_MSIX_DEV_MODE=off` 复现**分支**，并用
  `hresult:0x80073CFF` 钉住**映射**。也就是说"Windows 会返回这个码"来自实测，
  "脚本对这个码的反应"来自断言。
- **自签证书不是分发方案**：对方没信任你的根就装不上，这是证书链的性质，不是脚本的缺陷。
- **证书口令**只在内存里，不落盘、不打印；PFX 本身在 `<BuildDir>\certs`，别提交进仓库。
- **`-Machine` 需要一次提权**，脚本只能检测并打印 `Start-Process -Verb RunAs` 命令行，不能自我提权。
