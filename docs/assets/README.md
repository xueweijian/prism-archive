# 截图素材规范

README 里的四张图是**发布素材**，全部来自真实运行的本程序，不接受模拟图、占位图、
设计稿或从别家软件截来的图。每张图的拍摄归档、按键、窗口尺寸与裁切规则都写在下面，
照着做能得到同一画面。

## 拍摄条件

| 项 | 要求 |
|---|---|
| 系统 | Windows 11，1920 × 1080，显示缩放 125%（物理像素） |
| 窗口 | **可见帧 1280 × 800**（DWM 扩展边框，含标题栏与菜单栏），深色主题（Midnight），侧栏展开 |
| 内容 | 示例归档 `demo.7z`：18 个条目、4 个目录、3 层目录、8 种类型（png/svg/md/txt/csv/json/bin/pdf） |
| 语言 | 简体中文界面 |
| 格式 | PNG，单张 ≤ 400 KB，不锐化、不滤镜、不拼接 |

“可见帧”指 `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` 报出的矩形，
不是 `GetWindowRect`：Win11 的窗口每边有约 9 px 不可见的拖拽边框，
按 `GetWindowRect` 裁会把窗口后面的桌面/其它窗口带进图片底部一两行。

## 四张图

| 文件 | 画面必须出现 | 要证明的事 |
|---|---|---|
| `screenshot-main.png` | 标题栏 `demo.7z`、动作岛 8 个按钮、侧栏目录树、列表（4 个目录 + 5 个文件）、状态栏 4 段 | 打开归档后一屏同时看到目录、列表与状态 |
| `screenshot-tabs.png` | 两个标签（`demo.7z` 高亮为活动标签、`示例素材.7z` 非活动） | 多个归档各自成标签，活动标签有明显高亮 |
| `screenshot-preview.png` | 选中 `01-设计稿/首屏.png` 后按空格出现的 Quick Look 浮层：名称、字节数、关闭提示 | 预览是浮层，列表仍在原位、没有被顶走 |
| `screenshot-context-menu.png` | 资源管理器右键（经典菜单）里的 `PrismArchive` 级联，展开后 4 条动词 | 动词注册在 HKCU 的 `*` 经典菜单层，文案完整 |

## 生成方法

四张图都从当前 Release 构建的 `PrismArchive.exe` 拍出。窗口用 `SetWindowPos` 放到
屏幕左上角、尺寸调到“可见帧 1280 × 800”，再用 `PrintWindow` 之外的
**屏幕抓取**（`Graphics.CopyFromScreen`）只截这个矩形；截图前把其它窗口留在背景、
关掉通知，避免弹窗和桌面图标入镜。

### screenshot-main.png

1. 启动：`PrismArchive.exe demo.7z`（单归档，标题栏即 `PrismArchive -- demo.7z`）。
2. `Ctrl+2` 切到列表视图；`Home` 选中第一行 `01-设计稿`。
3. 窗口可见帧放到 (0,0)，宽高 1280 × 800；屏幕抓取同一矩形。

### screenshot-tabs.png

1. 启动：`PrismArchive.exe demo.7z 示例素材.7z`，两个归档各成一个标签。
2. `Ctrl+2` 列表视图；不切换标签（第一个保持活动，蓝色高亮）。
3. 同样的 1280 × 800 可见帧与裁切。

### screenshot-preview.png

1. 启动 `demo.7z`，`Ctrl+2` 列表视图，`Home` + `Enter` 进入 `01-设计稿`。
2. `Home` 后按两次 `↓` 选中 `首屏.png`，按 `Space` 打开 Quick Look。
3. 浮层出现在列表右侧，列表本身不动；同样按 1280 × 800 抓取。

**这张图证明的是浮层的位置、标题与元数据，不是图片像素。** 读实现可知
`GalleryRenderer::DrawQuickLookCard` 目前画的是“decoded texture 的渐变替身”，
图片/文本的解码结果还没有接进这块卡片；截图里看到的渐变就是当前构建的真实输出。

### screenshot-context-menu.png

1. 桌面上准备一个含 `demo.7z` 的示例目录（本图用 `%TEMP%\c73-shots\fixtures`）。
2. `explorer.exe /select,"…\demo.7z"` 打开资源管理器并选中该文件，窗口放大到
   1520 × 800（比成品宽 240 px，多出的部分留给裁切）。
3. `Shift+F10` 打开经典右键菜单；用键盘下移到 `PrismArchive` 再按 `→` 展开级联。
4. 从屏幕抓取里裁 `x = 240` 起的 1280 × 800：左边 240 px 正好是导航窗格，
   裁掉它是为了不带进收藏夹里的本机个人目录名；右侧保留完整菜单与级联。

级联里的动词来自 `HKCU\Software\Classes\*\shell\PrismArchive*`（安装器写入）。
本机这份注册展开后是四条：智能解压到此处、解压到此处、解压到 "demo"、测试完整性；
`用 PrismArchive 打开` 是 `.7z` 的默认双击动作，由默认打开命令承担，
不再重复出现在级联里。

## 脱敏（硬要求）

- 不出现真实用户名、家目录、邮箱、机器名、许可证/序列号、内网地址。
- 标题栏与地址栏只用示例路径（`…\Temp\c73-shots\fixtures`、`demo.7z`）。
- 导航窗格、状态栏、预览标题里的文件名同样要检查：一个真实的客户名比一个可见路径更糟。
- 截图前关闭通知与输入法候选窗，不放鼠标指针。

## 复现

示例归档由 `7z.exe` 现场生成，路径都落在 `%TEMP%` 下，不依赖任何个人目录：

```pwsh
$demo = "$env:TEMP\c73-shots\demo"
New-Item -ItemType Directory -Force "$demo\01-设计稿\界面", "$demo\02-文档", "$demo\03-数据" | Out-Null
# …写入 9 个文件（png/svg/md/txt/csv/json/bin/pdf），再打包：
Push-Location $demo
7z a -t7z "$env:TEMP\c73-shots\demo.7z" *
Pop-Location
```

抓图脚本只做三件事：`SetWindowPos` 把可见帧调成 1280 × 800、`SetForegroundWindow`
把窗口提到最前、`CopyFromScreen` 截该矩形。**不要**对截图做缩放、锐化或拼接。

## 提交前自查

1. 四张图都是 1280 × 800，字节数都 ≤ 400 KB（`Tests/suites/c73-assets.cmake` 会断言）。
2. 每张图都能在未修改的发布构建上复现同一画面（不是某个本地分支的临时 UI）。
3. 图片里的版本号与 README 写的版本一致。
4. 没有第二台显示器的边缘、鼠标指针、通知弹窗、桌面图标。
