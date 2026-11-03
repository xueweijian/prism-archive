# PrismArchive 使用手册

适用版本 **0.1.0（开发版）**。安装、卸载与系统要求见 [README](../README.md)；
打不开、右键没有、乱码等问题见 [TROUBLESHOOTING](TROUBLESHOOTING.md)。

PrismArchive 用 7-Zip 的 `7z.dll` 做解码与编码，界面、导航、编辑与关联都是本仓库自己的实现。
本文只写**这一版真的能用**的路径；末尾「本版本的边界」把还没接线的入口一次说完，
省得你对着一个画出来的按钮怀疑人生。

## 1. 打开归档

四种入口，效果一样：

1. **双击**已关联的归档（安装器默认关联 22 个扩展名）。
2. **命令行**：`PrismArchive.exe "D:\Downloads\a.7z"`。
3. **从资源管理器拖进窗口**：当前没有打开归档时，拖入一个归档会打开它。
4. **不带参数启动**：恢复上一次会话（`session.json` 里记的归档与视图）。
   已经不存在的归档会被静默跳过（stderr 有一行计数），不会连着弹八个错误框；
   一个都恢复不了时才弹文件选择框。

窗口一次显示**一个**归档。会话文件会记住最近打开过的路径（上限见
[CONTRACTS](CONTRACTS.md)），但 0.1.0 还没有标签栏。

打开失败时弹窗包含应用给出的原因和引擎的打开轨迹（`openTrace`）。加密归档
不会走失败弹窗，而是先问口令（见第 10 节）。

## 2. 界面

| 区域 | 你能做什么 |
|---|---|
| 菜单栏 | **文件**：压缩为 .7z… / 退出；**编辑**：删除、重命名、新建文件夹；**Settings…** |
| 动作岛 | 八个按钮（打开 / 解压到… / 压缩 / 添加 / 删除 / 测试 / 查看 / 搜索）。**解压到…** 在有多个选区时只解压选中的条目；「添加」「搜索」的边界见文末 |
| 面包屑 | 显示当前归档内路径，**点击任意一级**直接跳过去 |
| 列表 / 网格 | 两种视图，`Ctrl+2` / `Ctrl+1` 切换；网格里没有缩略图的条目退化成一行，不画骨架卡 |
| 状态栏 | 选中数、加密状态、当前图标档位、上一次操作的结果（成功/失败原因） |
| 侧栏 | 收藏、最近、归档目录树 |
| HUD 胶囊 | 解压/压缩进行中与完成的进度 |

## 3. 键盘

修饰键是**严格匹配**的：`Ctrl+Shift+1` 不是 `Ctrl+1`，`AltGr`（Windows 上是
Ctrl+Alt）也不会误触视图切换。

| 按键 | 动作 |
|---|---|
| `↑` `↓` `←` `→` | 移动选择（网格里上下是一行） |
| `Home` / `End` | 第一项 / 最后一项 |
| `Enter` | 打开选中项：文件夹进入，文件开始解压（见第 4 节） |
| `Backspace` | 返回上一级（已在根目录时什么也不做） |
| `Alt+←` / `Alt+→` | 浏览历史后退 / 前进 |
| `Ctrl+1` / `Ctrl+2` | 网格视图 / 列表视图 |
| `Ctrl+Shift+P` | 图标档位 64 → 96 → 128 → 192 → 256 → 384 → 512 → 64 px，状态栏显示当前值 |
| `Space` | 打开 / 关闭 Quick Look 预览 |
| `Esc` | 关闭最上面那一层（面板 > 预览 > 列表） |
| `Ctrl+K` | 命令面板（再按一次关闭） |
| `Ctrl+E` | 智能解压（等同于双击文件，见第 4 节） |
| `Ctrl+A` | 全选当前文件夹 |
| `Alt+双击` | 解压到以归档名命名的子文件夹（等同于 `--extract-sub`，见第 4 节） |
| `Ctrl+单击` / `Shift+单击` | 切换单个条目 / 从锚点选一段（见第 4 节） |
| `Delete` | 删除选中条目（改归档，先确认） |
| `F2` | 重命名选中条目 |
| `Ctrl+Shift+N` | 在归档里新建文件夹 |

## 4. 选择、双击与解压

**多选**：`Ctrl+单击` 切换单个条目，`Shift+单击` 从锚点选一段，`Ctrl+A`
全选当前文件夹，点空白处清空。选区是**有序**的（按行号升序），动作岛的
「删除 / 查看」等按钮跟着"有没有选中"变化。

**双击一个文件**走「智能解压」：把整个归档按规则铺到归档所在目录。
**`Alt+双击`** 走 `--extract-sub`：整棵树放进归档旁以归档名命名的子文件夹。
两者是同一套计划（`--extract-sub` 与菜单/命令行同名），文件夹双击仍然是进入，
不写磁盘。

**只解压选中的条目**：选中多个条目后点动作岛的「解压到…」，只解压这些条目
（选中的文件夹会带上它的子树），落点是当前智能解压计划的目标目录。

**拖出**：把条目从窗口拖到资源管理器（或任何接受 `CF_HDROP` 的目标）会复制
文件。为了不产生"拖一下就落一堆临时文件"，只有在目标真的开始复制时才会解压
到唯一的临时目录；拖放结束（放下 / 按 Esc 取消 / 出错）后该目录被删除。

## 5. 解压

三种落点，由入口决定（命令行与右键动词同名）：

| 入口 | 落点 | 规则 |
|---|---|---|
| 智能解压（双击 / `Ctrl+E` / `--smart-extract`） | 归档所在目录 | 去掉多余的嵌套根；散落的顶层文件包进一个以归档命名的文件夹；跳过推广类噪声文件 |
| 解压到此处（`--extract-here`） | 归档所在目录 | 7-Zip 的 Extract Here：归档里的树原样铺开，不剥根、不过滤 |
| 解压到子文件夹（`--extract-sub`） | 归档旁的 `<归档名>\` | 整棵树放进以归档命名的目录；目录已存在时自动取一个不冲突的名字 |

同一批动词还有：

```pwsh
PrismArchive.exe --extract-here "D:\Downloads\a.7z"
PrismArchive.exe --extract-sub  "D:\Downloads\a.7z"
PrismArchive.exe --test         "D:\Downloads\a.7z"   # 只校验，不开窗
```

`--test` 解压每个条目但不写文件，向 stderr 打印一行：

```text
verify ok=1 items=42 bytes=1048576 failedIndex=-1
```

**安全后处理**：解压成功后，归档的 `:Zone.Identifier`（「来自 Internet」标记）会继承到
写出的文件上；可执行/脚本类载荷会先过一遍 AMSI。被拦下或无法扫描时会弹一个警告框
（不是静默成功）。

## 6. Quick Look

选中一条，按 `Space`。能预览的类型：

- 图片：png、jpg/jpeg、gif、webp、bmp、avif、heic/heif、svg、psd、tif/tiff、cr2、nef、arw、dng
- 文本：txt、md、cpp/h/hpp、c/cc、py、java、js、ts、json、xml、yaml/yml、ini、cfg、log、csv、html、css、url
- 文档：pdf
- 媒体：mp4、mov、mkv、avi、webm、mp3、wav、flac、ogg、m4a

单条目超过 **32 MiB** 不做预览（防止解压炸弹）；其它类型提示「没有预览」。
`Esc` 或再按 `Space` 关闭。预览是覆盖层，不会把列表顶走。

## 7. 命令面板

`Ctrl+K` 打开。它同时搜**归档内路径**和**命令**：

| 命令 | 快捷键 |
|---|---|
| Extract Smart | `Ctrl+E` |
| New Folder | `Ctrl+Shift+N` |
| Grid View | `Ctrl+1` |
| List View | `Ctrl+2` |
| Zoom Next | `Ctrl+Shift+P` |
| Back | `Alt+Left` |
| Forward | `Alt+Right` |
| Quick Look | `Space` |

- 输入过滤，`↑`/`↓` 选择，`Enter` 执行。
- `Tab` 在 **All / Commands / Files** 之间切换（`Shift+Tab` 反向）。
- 选中一个**文件**时，Enter 只是跳到它所在的文件夹并选中它，**不会解压**。
- `Esc` 或再按 `Ctrl+K` 关闭。面板打开时，字母和空格归输入框，不会触发快捷键。

## 8. 压缩

### 应用内

**文件 → 压缩为 .7z…** 打开压缩对话框。它压缩的是**当前归档里的内容**（重新打包），
默认目标放在归档旁边，文件名是 `<归档名>.7z`。对话框里可以改：

- 格式（7z / zip / tar）
- 压缩级别 1–9
- 固实（solid）
- 线程数（Auto 或 1–64）
- 口令、加密文件名（仅 7z）

确认后，内容先解到 `%TEMP%\PrismArchive\repack-<pid>-<ms>`，压缩完立即删除。
成功时状态栏显示 `已压缩 <新归档路径>` 并切到新归档；失败时显示
`压缩失败 <目标路径>` 加引擎给的原因。

会被明确拒绝的组合（不会生成一个「看起来成功」的包）：

- tar + 口令（tar 没有加密）
- 非 7z + 加密文件名（只有 7z 能加密文件头）
- 加密文件名但没有口令

### 命令行

```pwsh
PrismArchive.exe --compress-7z  "D:\work\report.docx" "D:\work\data"
PrismArchive.exe --compress-zip "D:\work\report.docx"
PrismArchive.exe --compress-here "D:\work\report.docx"   # 等价于 --compress-7z
```

目标文件名由第一个输入决定（`report.7z`，放在输入旁边）。stderr 最后一行是裁决：

```text
compress ok=1 items=2 bytes=20480 verified=2 ms=37 target=D:\work\report.7z error=
```

退出码 0 表示**有可用的包落盘**；1 表示什么都没写出来（上一行会说原因，
例如 `compress failed: nothing to archive`）；2 表示参数不对。

## 9. 编辑归档内的条目

- `Delete` 删除选中项，`F2` 重命名，`Ctrl+Shift+N` 新建文件夹。
- 每次都先弹确认框，里面写着这次重写会读写多少条目/字节。
- 编辑 = **整包重写**（copy-on-write：改的是临时副本，校验通过后才替换原文件）。
  中途失败时原归档不动。

编辑入口会先做体检，不能编辑时菜单项是灰的，状态栏/确认框给出原因：

| 原因（原文） | 怎么办 |
|---|---|
| 归档是只读的：编辑会替换整个文件，请先清除只读属性 | `attrib -R <归档>` 后再试 |
| 归档被其他程序独占：请关闭占用它的程序后再编辑 | 关掉解压软件/杀软扫描/另一个 PrismArchive 窗口 |
| 归档是分卷集（.001/.002）：重写只会产生一个文件，无法替换分卷 | 分卷不支持编辑，解压后重新打包 |
| 这个容器格式不支持编辑（只支持 .7z/.zip/.tar） | 换成这三种容器 |

解压或扫描正在进行时，第二个会写文件的操作会被拒绝并说明原因
（`正在解压或扫描，稍后再编辑`），不会两个写者抢同一个文件。

## 10. 加密归档与口令

打开加密归档时会先弹口令框，而不是直接报「打开失败」：

- 输错会重试，对话框显示尝试次数与诊断。
- 勾选「记住」后，口令经 DPAPI 加密存进 `%LOCALAPPDATA%\PrismArchive\passwords.vault`，
  下次自动带出。不勾选则只在本次进程内使用。
- 打开时验证通过的口令会交给后续解压句柄，所以「能列出」的加密包也能解压。
- 口令永远不写日志、不进崩溃转储。

## 11. 拖放

- **拖入归档**（当前没有打开归档）：打开它。
- **拖入文件/文件夹**（已经打开归档）：模型会给出「添加到归档」的计划，但这个版本
  **没有增量追加能力**，状态栏会明确说「暂不支持追加 (AddToArchive)：拖入的 N 个文件
  没有被写入」，并且真的一个字节都不写。
- **拖入非归档**（没有打开归档）：拒绝并说明原因。
- **拖出到资源管理器**：0.1.0 还没有接线，请用解压。

## 12. 设置

**Settings…**（菜单栏）打开。可改：

| 项 | 说明 |
|---|---|
| Language | `auto`（跟随 Windows）/ `zh-CN` / `en-US`，界面语言立即生效 |
| Theme | 日间 / 夜间 / 高对比 |
| Default extraction folder | 解压时的默认位置 |
| Thumbnail cache | 缩略图内存预算（MB） |
| Log level | Off / Error / Warnings / Info / Debug |
| MOTW | 是否为解压出的文件继承「来自 Internet」标记 |

配置在 `%LOCALAPPDATA%\PrismArchive\settings.json`。文件损坏时会被备份成
`settings.json.corrupt-<时间>.bak` 再用默认值启动；更高版本写的配置会被当成
**只读**（保存被拒绝，而不是悄悄降级覆盖）。

## 13. 命令行参考

| 命令 | 作用 | 退出码 |
|---|---|---|
| `PrismArchive.exe "<归档>"` | 打开界面 | 0 |
| `--smart-extract "<归档>"` | 智能解压到归档所在目录 | 0 成功 / 1 失败 / 2 缺参数 |
| `--extract-here "<归档>"` | 解压到归档所在目录 | 同上 |
| `--extract-sub "<归档>"` | 解压到以归档名命名的子目录 | 同上 |
| `--test "<归档>"` | 只校验，不开窗；stderr 打印 `verify ok=...` | 0 通过 / 1 失败 |
| `--compress-7z <文件或目录>...` | 压缩成 .7z | 0 成功 / 1 失败 / 2 缺参数 |
| `--compress-zip <文件或目录>...` | 压缩成 .zip | 同上 |
| `--compress-here <文件或目录>...` | 同 `--compress-7z` | 同上 |
| `--smart-extract-each "<归档>"...` | 每个归档各自解压到独立文件夹 | 0 / 1 / 2 |

不认识的参数不会弹框卡住调用方（Explorer 可能一次调一百个），而是向 stderr 说明并退出 2：

```text
verb refused: unsupported flag --nope; supported: --smart-extract --extract-here --extract-sub --test --compress-here --compress-7z --compress-zip --smart-extract-each
```

脚本里判断成败请只看退出码；stderr 的 `extract ok=...` / `verify ok=...` /
`compress ok=...` 是给人看的证据。

## 14. 卸载

```pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\PrismArchive\uninstall.ps1"
```

或者 **设置 → 应用 → 已安装的应用 → PrismArchive → 卸载**。卸载会删除安装目录、
卸载项、`PrismArchive.*` ProgID 与右键动词，并把每个扩展恢复成安装前的处理器
（逐键比对）。重复卸载打印 `UNINSTALL: already clean`，不是错误。

## 15. 文件在哪

| 内容 | 路径 |
|---|---|
| 程序与 shell 扩展 | `%LOCALAPPDATA%\PrismArchive\` |
| 配置 | `%LOCALAPPDATA%\PrismArchive\settings.json` |
| 会话（上次打开的归档/视图） | `%LOCALAPPDATA%\PrismArchive\session.json` |
| 记住的口令（DPAPI 加密） | `%LOCALAPPDATA%\PrismArchive\passwords.vault` |
| 引擎 | **不在包里**：`7z.dll` 由 `PRISM_7Z_DLL` 或本机 7-Zip 提供 |

## 16. 无障碍：屏幕阅读器与自动化（C80）

PrismArchive 的客户端区域（动作岛、标签栏、列表、侧栏、状态栏）全部是自绘的，
所以它们**不是**子窗口控件。在 C80 之前，无障碍客户端（UI Automation）抓这个窗口
只能看到 **9 个元素**：标题栏三个按钮 + 系统菜单 + 菜单栏四项。读屏用户无法使用
软件，自动化也只能做像素断言。

现在窗口实现了 `WM_GETOBJECT(OBJID_CLIENT)`，返回一棵自定义的 **MSAA
（IAccessible）** 树；Windows 的 MSAA 代理会把它翻译成 UI Automation 元素
（名称、角色、可用/禁用、屏幕矩形、Invoke / SelectionItem）。**绘制方式没有改变**
——这是桥，不是把界面换成标准控件。

### 能被读到的对象

| 区域 | MSAA 角色 | 名称 | 状态 |
|---|---|---|---|
| 窗口本身 | `ROLE_SYSTEM_CLIENT` | 窗口标题（`PrismArchive -- 归档名`） | 焦点在它上面时 `FOCUSED` |
| 动作岛 8 个按钮 | `ROLE_SYSTEM_PUSHBUTTON` | **当前语言**的按钮文字（打开 / 解压到… / 压缩 / 添加 / 删除 / 测试 / 查看 / 搜索） | 可用/禁用跟随按钮启用规则（无选中时「添加」「删除」「查看」为禁用）；`Alt+O/E/C/A/D/T/V/S` 作为快捷键报告 |
| 标签栏每个标签 | `ROLE_SYSTEM_PAGETAB` | 归档文件名 | 当前标签 `SELECTED`；描述里写「可关闭」/「已修改，可关闭」 |
| 列表容器 | `ROLE_SYSTEM_LIST` | 当前路径（面包屑文本） | — |
| 列表每一行 | `ROLE_SYSTEM_LISTITEM` | 文件名；值 = 字节数 | 选中行 `SELECTED`；描述「文件」/「文件夹」 |
| 侧栏归档树 | `ROLE_SYSTEM_OUTLINE` / `ROLE_SYSTEM_OUTLINEITEM` | 归档名 / 文件夹名 | 当前文件夹 `SELECTED`；可展开项 `EXPANDED`/`COLLAPSED` |
| 状态栏 | `ROLE_SYSTEM_STATUSBAR` | 状态文字（如「共 12 项」） | 值 = 路径栏文本 |
| 搜索框（打开时） | `ROLE_SYSTEM_TEXT` | 「搜索框」 | 值 = 当前输入（含输入法预览）；**可写**（见下） |

### 能做的动作

- **按下动作按钮**：`accDoDefaultAction`（UIA 的 InvokePattern）走的是**和鼠标点击同一条路径**
  （`ActionBarClick` → `FireAction`），所以「搜索」按下去真的会打开搜索框，
  禁用按钮按下去什么也不会发生。
- **选中列表行**：`accSelect(SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION)`（UIA 的
  SelectionItem.Select）会**真的改变窗口的选中状态**（选中集合、动作岛的启用规则、
  下一次解压的目标都跟着变）。
- **切换标签**：对标签调用默认动作或 `accSelect` 等于点击该标签（会去加载那个归档）。
- **进入侧栏文件夹**：对树项调用默认动作等于在侧栏里点该文件夹。
- **往搜索框里写字**：`put_accValue`（UIA 的 `ValuePattern.SetValue`）把整串文字写进
  查询，走的是**和键盘输入同一条路径**（同一个 model 的 `SetQuery`/`InsertText` +
  同一个 `ApplySearchFilter`），所以列表会立刻按查询过滤，写成空串则恢复全部行。
  读屏/自动化因此可以「输入查询」，不只是「看见搜索框」。
  对搜索框调用默认动作（或 `accSelect(TAKEFOCUS)`）把键盘焦点放进字段。
- **焦点**：`accFocus` 回答当前焦点元素——顺序与键盘路由一致：搜索框（打开时）→
  动作岛焦点环 → 选中的行 → 窗口本身。
- **命中测试**：`accHitTest` 用 `accLocation` 报告的屏幕矩形做最深匹配。

### 已知边界（如实写出来，不假装支持）

- 侧栏树项的展开/折叠只**报告状态**，不提供 ExpandCollapse 动作（用键盘或点击箭头）。
- 驱动工具里的 `type`（语义输入）在默认无头模式下要求原生 UIA 的 TextPattern，而 MSAA
  代理只提供 ValuePattern：用 `set-value` 写字，或给 `type` 加 `--headed`（物理按键，
  等价于读屏用户真的在打字）。两条路都实测会改查询并过滤列表。
- 多选（Ctrl/Shift）通过每一行的 `SELECTED` 状态暴露；`accSelection` 只返回一个元素。
- UIA 看到的是 **MSAA 代理元素**（LegacyIAccessible），不是原生 UIA Provider：
  Windows 讲述人、NVDA、Inspect.exe 都能用，但个别只认原生 UIA 属性的工具会退化。

### 自己验证（不需要看屏幕）

```powershell
# 元素数（修复前是 9；现在打开的归档有 20+ 个元素）
agent-desktop snapshot --app PrismArchive.exe -i --compact

# 按名称点「搜索」，然后确认搜索框出现（窗口状态变了，而不是只有像素变了）
agent-desktop click <snapshot 里「搜索」按钮的 ref>
agent-desktop snapshot --app PrismArchive.exe -i --compact

# 再用 ValuePattern 真的「打字」：写 item01 → 列表只剩一行；清空 → 全部行回来
agent-desktop set-value <snapshot 里「搜索框」的 ref> item01
agent-desktop snapshot --app PrismArchive.exe -i --compact
agent-desktop clear <snapshot 里「搜索框」的 ref>
```

自动化测试在仓库里：

```text
ctest -R "a11y"                  # core.a11y_tree / c80.a11y / c80.a11y_uia
ctest -R no_getobject            # 负向控制：把 WM_GETOBJECT 注掉后必须红
ctest -R readonly_search         # 负向控制：把 put_accValue 注掉（只读搜索框）后两条测试必须红
```

## 本版本的边界

这些入口**画出来了但还没接线**，或者只完成了一部分。写在这里是为了让「点了没反应」
不被误解成安装坏了：

- 顶部动作岛的「添加」：`ArchiveWriter` 没有增量写入，点击给出「追加暂不支持」并记录日志，**不会改归档**。
  想追加请先解压、再重新压缩（应用内「压缩」或 `--compress-7z`）。
- `Ctrl+Shift+P`：只切换并显示图标档位文本，缩略图仍按固定尺寸绘制。

以下三条**已经接好了**（曾经列在这里，2026-11-03 实测确认）：

- 顶部动作岛的「搜索」与 `Ctrl+F` 打开**同一个**搜索框；
- 标签页：多个归档各有标签，点击真的切换内容，`Ctrl+Tab` / `Ctrl+W` / 中键关闭 / 拖拽重排都可用；
- Help → About：打开诊断窗口，可一键复制/保存环境信息（版本、7z.dll 路径、注册表状态）。
### 设置里「改了以后真的会变」的项（C69 + C71 实测）

C69 用真对话框 + 真窗口 + 像素/菜单字符串断言逐项测过。**会变的**：

| 设置 | 可观察的差异 | 断言 |
|---|---|---|
| 主题（Day / Night / High contrast） | 整个设置窗口按新令牌重画：同一坐标的 RGB 从 (30,138,255) → (13,114,216) → (102,179,255) | win.c69_settings（采样点 = 主按钮填充，即主题强调色令牌） |
| 语言（auto / zh-CN / en-US） | 菜单栏文字与对话框六行标签立刻换语言：Compress to a .7z archive → 压缩为 .7z 归档，Language → 语言 | win.c69_settings（GetMenuStringW + 控件文本） |
| 列表/网格（listMode） | 窗口模型切换，渲染器绘制的行/瓦片布局随之改变 | win.c69_settings checklist 用例 |
| 排序（sort） | 排序键与方向落到窗口的多键排序规格上，行序随之改变 | 同上 |

**C71 接线后也「改了真的会变」的四项**（对话框里没有对应控件，改的是
`%LOCALAPPDATA%\PrismArchive\settings.json`；保存后下一次绘制/解压生效）：

| 设置 | 可观察的差异 | 断言 |
|---|---|---|
| appearance.accentColor | 进度条填充、动作岛焦点环、列表选中行改用保存的颜色（默认值 #4F8CFF 是「跟随主题令牌」的哨兵） | win.c71_livesettings（同一坐标像素 = 解析出的 RGB，且与令牌像素不同） |
| appearance.glassIntensity | 玻璃不透明度按 0.72 为基准缩放：0.30 与 0.90 在同一玻璃坐标得到不同像素，且等于 CompositeOver(令牌×强度, 背景) | win.c71_livesettings（两档像素 + 合成值比对） |
| extract.overwrite | 目标已存在时 replace 覆盖、skip 保留原文件、rename 另存为 `note (1).txt`；ask 在窗口里弹确认，命令行动词（无窗口）按 skip 处理并在 `skipped=` 里报数 | win.c71_livesettings（磁盘字节 + 回调计数；ask 断言提示次数，不弹真框） |
| extract.verifyAfterExtract | 开：解压后按 C13 Verify 逐条重解，损坏条目让本次操作报告失败；关：同一计划成功 | win.c71_livesettings（同一损坏包、同一计划，off ok / on verifyFailed） |

C74 之后，动作岛的八个按钮、侧栏标题、信息卡（未加密 / 未知 | 字典 --
/ 压缩比）以及设置对话框的摘要行与就地提示都从资源表读取：
`ActionBarModel` 只保留键（`actionbar.*`），语言值由窗口注入；
侧栏的信息卡用 `status.*`，设置诊断用 `settings.issue.*` 与
`settings.summary.*`。断言见 `core.actionbar_labels` 与 `win.i18n_switch`
（真窗口切语言后标签文本与像素都变）。
