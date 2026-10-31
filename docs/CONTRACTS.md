# CONTRACTS.md — 跨边界契约台账

> 规则：**每一条跨边界契约必须有四要素**——①陈述 ②权威来源 ③强制门禁（能自动化的绝不靠约定）
> ④违反案例（教会我们这条规则的那个 bug）。没有门禁的契约只是一条注释。
> 本台账是 CONTRACT-FIRST TDD 的输入：写新模块前，先在这里登记它触到的契约。

## A. 外部 ABI 契约（不可协商，权威在外部源码）

| # | 契约 | 权威 | 门禁 | 违反案例 |
|---|---|---|---|---|
| A1 | 7-Zip handler `Open()`：`S_OK`=接受，`S_FALSE`="不是我的格式，试下一个"；**`SUCCEEDED(S_FALSE)` 为真** | 7-Zip 源码 ArchiveOpenCallback 语义 | 真 .7z fixture 集成测试（批 2 引入） | ZIP handler 绑上 .7z → catalog 全空（M1 的手写 ZIP 夹具永远测不出） |
| A2 | 候选 handler 失败的 Open 会消费流位置；每个候选 Open 前必须 `Seek(0)` | IInStream COM 语义 | 同上（openTrace 断言两个 handler 都能试） | trace 全 0x00000001，真 7z 也打不开 |
| A3 | PROPID 是**权威序数**（kpidPath=3…kpidBlock=27, kpidMethod=22）；标准属性在 GetPropertyInfo 里 name=NULL → 按名字发现必然失败 | 7-Zip PropID.h（逐条数过） | 集成测试断言 item.block 非零（固实包） | `PropertyId("Block", 0)` fallback=0 恒返回 0，固实分组静默塌缩 |
| A4 | IID 结构 `{23170F69-40C1-278A-0000-00yy00xx0000}`（yy=头文件族，xx=成员；Data4[5] 是 0x60 不是 0x06） | 7-Zip Guid.txt | CreateArchiver 冒烟（M1 起） | 错一位 = E_NOINTERFACE 且发生在 CLSID 检查前，极难定位 |
| A5 | WIC：`CopyPixels` 是**从位图读出**；写入走 `Lock`+`GetDataPointer`。方向反了不报错，静默产出全透明 PNG | WIC 头文件签名 | 夹具颜色断言（RGB 三元组集合相等） | WritePng 用 CopyPixels → 全管线"解码成功"但像素全零 |
| A6 | HLSL cbuffer 打包规则：float2×4→2 寄存器、float×4→1、float4 各占 1（5 寄存器 80B） | d2d1 effect 文档 + fxc | GlassIsland::WriteCbuffer 逐位断言（TDD） | 未违反（先测后写） |
| A7 | 7z 固实模式**重排条目**（相似文件分组进同一字典）：解压顺序 ≠ 打包顺序 | 7-Zip 压缩器行为 | 测试断言**尺寸集合**而非顺序 | dims[1] 期望 photo 实为 large，断言层就红了 |

## B. 跨层契约（PrismCore ↔ PrismWin ↔ CI）

| # | 契约 | 权威 | 门禁 | 违反案例 |
|---|---|---|---|---|
| B1 | PrismCore 头对 `std::min/max/clamp` 必须写**括号形式** `(std::max)(...)`：消费方 TU 可能含未定义 NOMINMAX 的 windows.h | MSVC 宏行为 | **preflight grep**（tools/preflight.sh，待建）+ CI | C2589：本会话至少两轮（M3 批次 1、密码环） |
| B2 | 头文件自洽：用到定宽整数（u?intN_t）必须**自己** `#include <cstdint>`；本地 libstdc++ 的传递引入在 CI 不存在 | libstdc++ 版本差异（已实证） | tools/check_headers.sh **两级**：独立编译 + 词法卫生 | PasswordVault 用 uint8_t 靠 `<regex>` 传递引入 → CI 才炸；词法门禁当场再抓出 SmartExtractor 同款 |
| B3 | 词法门禁优先于行为门禁：本地≠CI 时，只有**确定性检查**可复现 | 本会话实证 | check_headers LEVEL2 | 编译探针抓不到（本地也绿），词法才抓住 |
| B4 | PrismCore 全用 `'/'`；过 Win32 层前 `'/'→'\\'` 再加 `\\?\`（前缀禁用归一化，不转就静默失败） | Win32 路径语义 | M1 集成测试 | M0 时期踩过（见 ADR-004） |
| B5 | 项目非 UNICODE 构建：`CFSTR_*` 等 TEXT 宏展开成窄串，必须显式 Wide 形式 | 工程 .vcx 默认 | CI 编译（m3_compile_check） | dnd agent 一轮：CFSTR_FILEDESCRIPTORW 窄串 |
| B6 | 新枚举被 PRISM_CHECK_EQ 打印必须有 `operator<<` | mini_test 契约 | preflight 启发式（enum class 与 operator<< 同头文件共现，待建） | DragDrop Channel 枚举一轮 |
| B7 | 环境变量/env 的凭据永远不打印；工具必须打印**可回读的远端证据**（remote sha）而非本地回显 | 本会话教训 | push_api --force + 回读远端 ref | push_api 谎报"pushed"（非快进被拒却打印本地 sha）→ CI 证据失效一轮 |

## C. 数据与所有权契约（模块间）

| # | 契约 | 权威 | 门禁 | 违反案例 |
|---|---|---|---|---|
| C1 | `DecodedThumb.pixels` = BGRA、premultiplied、top-down、stride=w*4 | 渲染器 CreateBitmap 参数 | 快照像素断言（四 fixture 色） | 夹具方向反（A5）曾让全链路零像素 |
| C2 | 固实扫描的**零拷贝契约**：scratch 指针经 OnItemEnd 传递，峰值内存=scratch+缩略图，绝不缓存块 | SolidSweep.hpp 头注 | 基准不变量测试（PR #4） | —（先测后写） |
| C3 | ThumbStore.Version() 单调：渲染器位图缓存只在 Version 变化时失效 | ThumbStore.hpp | TDD（插入/驱逐 bump，Lookup 不 bump） | — |
| C4 | 排序的确定性是契约：RankCandidates 同输入同序（权重→历史→来源→字典序） | PasswordRing.hpp | 断言**精确顺序**（不只"排过序了"） | — |
| C5 | `TempCopyFallback` 拒绝必带 DenyReason 枚举，HasExplicitReason() 恒真 | DragDrop.hpp | 128 组合全枚举扫描 | —（dnd agent 补的防线） |
| C6 | 测试断言在**正确的层**：TransformItemPath 是纯变换（不管碰撞），碰撞归 CollisionResolver（碰文件系统的那层） | SmartExtractor 分层 | 测试评审时的提问："被断言的函数是否拥有这个职责？" | 我把文件改名断言放在纯函数上，测试错了不是代码错了 |

## D. 验证契约（测试基建自身的规则）

| # | 契约 | 门禁 | 违反案例 |
|---|---|---|---|
| D1 | 集成测试 stdout 成功标记是**判定的一部分**：ctest `PASS_REGULAR_EXPRESSION "ALL … CHECKS PASSED"`；SKIP 必须 return 非 0 | CMake 属性 | 0.15s 的"Passed"实为 SKIP 假绿（批 2 前夜） |
| D2 | 断言**内容**不只存在性：像素要断颜色，payload 要断数值 | 夹具三元组集合断言 | `!pixels.empty()` 放过了全零像素（A5 的帮凶） |
| D3 | 负向控制的负向控制：注入 bug 后套件必须红；**注入本身要验证发生了**（替换串没匹配上=静默无操作） | 突变双编译 + 注入后 grep | 密码环 Redact 突变第一次没生效（regex 没匹配），差点误报"测试不锋利" |
| D4 | 验证工具自身要先被校准：我的 PNG 解析器先对已知底色验证过才可信 | 会话实录 | 未校准的解析器会报假 0% |
| D5 | 突变要打在**能测出的位置**：部分泄漏循环会顺带消掉长密码全匹配 → 突变对短密码才可观测 | 密码环短密码用例 | 突变二起初全绿（不是测试锋利，是突变无效） |
| D6 | **push 必须以远端为底**：--tree 整树推送前先拉远端 tarball 同步；整树只增不删是保命特性（base_tree 继承） | push_api --tree + 同步步骤 | 过时树推送静默丢 B3 的 CMake 注册，main 连红两次（2026-10-31） |
| D7 | **禁止对整文件做索引手术**：python 按 old_start/old_end 切片段必然切错位，留下新旧混杂的孤儿尾巴 | 整文件重写（file_write） | inc-2 的 Main.cpp 切坏，CI 红两轮才修（2026-10-31） |
| D8 | **Windows TU 无本地编译 → 每类 MSVC 错只犯一次**：犯过的错误形态进 preflight 规则（B7=未限定类静态） | tools/preflight.py B7 | GalleryWindow:: 静态未限定（C2653）→ NowMs 私有（C2248）连咬两轮 |
| D9 | **subagent 一律用自己的 clone 工作树**，禁止在共享仓 git 操作：共享 /tmp 工作树会被并发 agent 覆盖，未跟踪文件会被清掉 | 任务简报明示 clone 路径 | m4b3 工作树 CMakeLists 被毁 + 本地未跟踪 PasswordRing.hpp 消失（2026-10-31） |

## E. 待登记（下批次会触碰）

- IOutArchive::UpdateItems 的真实成本语义（ADR 已记，执行器落地时加集成测试：改名后重新 Open 验证 + 耗时上界断言）
- FILECONTENTS 流的 Seek 语义（Explorer 乱序/重复 GetData 的容忍度）——**POC 先行**（见 METHODOLOGY）
- MSIX 注册在 runner 上的可重现证据格式

> 使用方式：改动跨边界的代码前，把触到的契约编号写进 PR 描述。新增契约走 PR 改本文件。
