# PrismArchive

**7-Zip 的工业级压缩引擎 × Bandizip 的零摩擦交互 × Apple iOS 26 Liquid Glass 的光学美学。**

PrismArchive 是一个公开的开源归档管理器工程。目标不是再写一个 7-Zip 皮肤，而是把三件事
真正合到一起：把 7-Zip 的 C/C++ 算法内核当作**无状态服务**通过非注册表 COM 驱动，
1:1 复刻并超越 [Bandizip](https://en.bandisoft.com/bandizip/) 的五大灵魂交互，
并在视觉层用自定义 HLSL 着色器管线实现 Apple 的 [Liquid Glass](https://developer.apple.com/documentation/technologyoverviews/liquid-glass) 数字光学材质。

> 本仓库对应一份 11 章架构蓝图（战略对标 / 7-Zip 裁剪矩阵 / 五层光学管线 / 五大交互 /
> 生产级源码 / 安全与性能 / 16 周路线）。代码按该蓝图分模块落地，每个模块都挂着自己的 CI 判据。

---

## 为什么这样分层（手机端开发约束下的工程选择）

开发机只有手机，重活全部交给 GitHub Actions。因此架构的第一原则是：

**凡是能脱离 Windows 判定的逻辑，一律不许碰 `<windows.h>`。**

```
Src/PrismCore/    纯 C++20、header-only、跨平台 —— 产品的"脑子"
                  （智能解压决策树 / Zip Slip / 压缩炸弹熔断 / CJK 乱码修复 / CoW 虚拟树 / 密码环策略）
      |  Linux CI 编译 + 单测（秒级，含 asan/ubsan）
      v
Src/PrismWin/     Windows 专属 —— 7z.dll 桥接 / IExplorerCommand 右键 / 零拷贝拖拽 / DPAPI / MoTW
      |  windows-latest CI 用 MSVC 真编译、真加载 7z.dll、真解压
      v
Assets/Shaders/   Liquid Glass 五层光学管线（HLSL，CI 用 fxc.exe 编译校验）
tools/glass-preview/  同一套数学的 WebGL2 参考实现，浏览器里可直接看效果
```

好处：蓝图中"最容易做错的决策逻辑"（智能解压五分支、路径穿越、炸弹熔断、乱码判定）
在 Linux 上就有完整回归；而"只有 Windows 能证伪的"（COM vtable 顺序、7z.dll ABI、
HLSL 编译）交给 windows-latest 真机验证。

**设计蓝本**：[docs/BLUEPRINT.md](docs/BLUEPRINT.md) —— 11 章完整架构蓝图（7-Zip 引擎 × Bandizip 交互 × iOS 26 Liquid Glass 光学），本项目即按此从零实现。

架构决策记录与外部审阅勘误（**改代码前先读**）：[docs/ADR.md](docs/ADR.md)
总进度与作战计划（进度唯一权威口径）：[docs/PROGRESS.md](docs/PROGRESS.md)

## 已落地模块

**三 lane CI 全绿**（gcc-release / clang asan+ubsan / windows MSVC）。

- **M0/M1 完成**：PrismCore 决策脑（智能解压五分支、ZipSlip、炸弹熔断、CJK 修复、CoW 虚拟树、密码环）+ 7z.dll 真桥接（Windows 集成测试加载运行器上的真实 `7z.dll`：目录扫描 / 内存解压 / 智能解压落盘 / 套娃剥离 / ZipSlip 拦截 / GBK 修复全过）
- **M2 进行中**：MSIX Sparse Package manifest（Linux lane 校验 XML 完整性）✓ · Win10 IContextMenu 兼容层 `PrismShellExtClassic.dll` ✓ · 稀疏包注册器待做
- **TDD 增量**：CollisionResolver（计划集唯一性 + 有界后缀链）、QuickLookPlanner、SolidBlockPrefetcher、ContextMenuModel（一份模型，三处渲染：Win11 IExplorerCommand / Win10 IContextMenu / 应用内菜单）
- **Liquid Glass**：HLSL 两个 ps_5_0 过 fxc；WebGL2 参照实现修复了"先模糊后折射导致折射不可见"的管线缺陷 —— 折射带采样锐利背景、内部采样模糊背景，blur=24 下边缘弯曲肉眼可见

| 模块 | 文件 | 判据 |
| :--- | :--- | :--- |
| 归档目录模型 | `Src/PrismCore/ArchiveCatalog.hpp` | 固实块分组、路径索引 |
| 路径安全 | `Src/PrismCore/PathSafety.hpp` | Zip Slip 穿越、盘符注入、幽灵文件、推广文件识别 |
| **智能解压五分支决策树** | `Src/PrismCore/SmartExtractor.hpp` | A/B/B+/D/E 全分支 + 套娃剥离 + 碰撞改名，单测 12 例 |
| 压缩炸弹熔断 + 写守卫 | `Src/PrismCore/SecurityGuard.hpp` | 滑动窗口膨胀率、磁盘占用、符号链接延迟创建 |
| CJK 乱码检测与修复 | `Src/PrismCore/CharsetRepair.hpp` | GB18030/Big5/Shift-JIS/EUC-KR 结构打分 + 平台转码 |
| 免解压 CoW 虚拟树 | `Src/PrismCore/VirtualTree.hpp` | 重命名/移动/新建目录 → 仅重写中央目录的更新计划 |
| 密码环策略 | `Src/PrismCore/PasswordVault.hpp` | 文件名/注释/同级文本嗅探 + 保险库接口 |
| 7z.dll 桥接 | `Src/PrismWin/PrismEngineBridge.hpp` | windows-latest 上真加载 7z.dll、真扫描、真解压、真落盘 |
| Win11 右键扩展 | `Src/PrismWin/PrismShellCommand.cpp` | MSVC 编译 `IExplorerCommand` + `IEnumExplorerCommand` |
| 零拷贝拖拽 | `Src/PrismWin/PrismVirtualDragDrop.hpp` | `IShellWindows` 路径嗅探 + 延迟 `IStream` |
| DPAPI 密码环 / MoTW | `Src/PrismWin/DpapiVault.hpp` | `CryptProtectData`、`:Zone.Identifier` 继承 |
| 液态玻璃光学 | `Assets/Shaders/*.hlsl` | fxc `ps_5_0` 编译通过 |

## 本地构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

Windows（需要 MSVC + Windows SDK）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest -C Release --output-on-failure
```

Windows 集成测试会**真实加载 `7z.dll`**（优先读环境变量 `PRISM_7Z_DLL`，否则在常见安装
路径里找）。找不到时它会打印 `SKIP` 而不是假装通过 —— 没跑过的测试不算绿。

## Liquid Glass 可视原型

`tools/glass-preview/index.html` 是光学管线的 WebGL2 参照实现（多 pass：背景 FBO →
Kawase 双通降采样模糊 → SDF 圆角遮罩 + RGB 三通道色散折射 + Vibrancy + 菲涅尔追光 +
蓝噪点）。直接用浏览器打开即可，参数面板可实时调模糊半径、折射带、饱和度、圆角与日/暗夜
token，用来肉眼验证折射是否真的把背景扭曲了。

## 路线

见 [ROADMAP.md](ROADMAP.md)（对应蓝图第 7.3 节的 16 周四阶段，拆成 M0–M4 五个可验收里程碑）。

## 许可

MIT（见 [LICENSE](LICENSE)）。7-Zip 引擎**不入库**：运行时通过 `LoadLibraryW` 动态加载
`7z.dll`，引擎侧仍遵循其自身的 LGPL 许可，UI/交互层保持 MIT 干净隔离（蓝图第 2 章的解耦原则）。
