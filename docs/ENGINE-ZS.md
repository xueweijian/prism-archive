# ENGINE-ZS — 7-Zip ZS（Zstd / LZ4 / Brotli）引擎扩展 spike

> 卡片：C17（蓝图 ch.2「把 7-Zip ZS 的 Zstd/LZ4/Brotli 合进来」）。
> 结论先行：**A 档——ABI 兼容，可以换 DLL；但比蓝图假设的少一半收益，而且真正的
> 改造面在我们自己的 handler 选择表上，不在 DLL 里。**
> 本文每个数字都是 2026-11-01 在本机实测（E:\DevTools\7-Zip / E:\DevTools\7zzs），
> 没测的写在「未验证」里。

---

## 0. 结论（30 秒）

| 问题 | 答案 | 证据 |
|---|---|---|
| 官方 7-Zip 25.01 的 7z.dll 有 Zstd/LZ4/Brotli 吗？ | **有 Zstd 解码器（仅解 .zst 单流），没有 Zstd 方法、没有编码器、没有 LZ4/Brotli** | §1：format 表 60 项含 zstd；method 表 25 项不含 ZSTD |
| 换成 7-Zip ZS 的 7z.dll 是否 ABI 兼容？ | **是。14 个导出名与序号完全一致，ZS 只多一个 SetLargePageMode2（纯新增）** | §2：dumpbin /exports 全量 diff |
| 二进制层面能跑通吗？ | **能。zstd 压缩的 .7z 走我们未改一行 的 ArchiveReader 解出 2270 B 原文；裸 .zst/.lz4/.br 用对应 handler CLSID 各解出 2270 B** | §3：win.codec_probe 87 个断言全绿 |
| 需要构建源码吗？ | **不需要（不需要 C 档）。** 除非要「产品内置 Zstd 压缩」而不是「支持用户指定的引擎」 | §6 |
| 许可还满足「引擎不入库」吗？ | **满足。仍然是运行时动态加载 LGPL DLL，仓库零引擎二进制**；额外 codec 的许可是附加条件，不是冲突 | §5 |

**一句话**：不要做「合并 ZS 源码」这件事（蓝图 ch.2 的原文）。要做的是
**把 handler 选择表从硬编码 2 个 CLSID 改成「表驱动 + 能力探测」**，顺手让 stock
引擎白得 .zst 解压，并让 ZS DLL 一插即得 zstd 编码 / lz4 / brotli。

---

## 1. 官方 7-Zip 25.01 有什么（实测）

E:\DevTools\7-Zip\7z.dll：1,906,176 B，VERSIONINFO 25.1.0.0。

用 GetNumberOfFormats / GetHandlerProperty2(kName=0) 与
GetNumberOfMethods / GetMethodProperty(kName=1) 直接读 DLL（**不要**用 7z i
的文本输出：它的 Formats 段有 60 行，我第一版只截了前 50 行，于是「没有 zstd」这个
结论是截断造成的假象——同样的坑 CONTRACTS D12 记过一次）：

| 表 | 数量 | 关键内容 |
|---|---|---|
| handler（format） | **60** | 含 zstd、Z、xz、lzma、tar、wim…… |
| method（codec） | **25** | LZMA2、Deflate、BZip2、PPMD、Rar1..5、7zAES、AES256CBC……**无 ZSTD** |

所以官方 25.01：

- **能**解裸 .zst（单流解压，License.txt 里明确写着 7z.dll 内的 ZSTD 解压代码是
  BSD-3 参考实现，Copyright (c) Facebook, Inc.）；
- **不能**解 zstd 压缩的 .7z（没有 method id）；
- **不能**压 zstd，**没有** LZ4 / Brotli。

> 这条直接改写了蓝图的假设：**Zstd 解压我们本来就免费拥有**（只要 Open 愿意试那个
> handler），要买的其实是「Zstd 编码 + 7z 容器里的 Zstd + LZ4 + Brotli」。

## 2. 换成 ZS 的 7z.dll：ABI 兼容性（实测）

E:\DevTools\7zzs\7z.dll：5,441,024 B，VERSIONINFO 26.2.0.0
（安装包 7z26.02-zstd-x64.exe → 7z 26.02 ZS v1.5.7 R1，见 §7 复现）。

### 2.1 导出表 diff（dumpbin /exports，全量）

| 序号 | 官方 25.01 | ZS 26.02 |
|---|---|---|
| 1..14 | CreateDecoder CreateEncoder CreateObject GetHandlerProperty GetHandlerProperty2 GetHashers GetIsArc GetMethodProperty GetModuleProp GetNumberOfFormats GetNumberOfMethods SetCaseSensitive SetCodecs SetLargePageMode | 同名同序号，**一个不少** |
| 15 | — | SetLargePageMode2（新增，纯加法） |

SevenZipLibrary::Load 只依赖 CreateObject / GetNumberOfFormats /
GetHandlerProperty2 / SetLargePageMode——**四个全在，序号相同，所以
GetProcAddress 的语义完全不变；老 DLL 练出来的代码路径一行不改。**

### 2.2 IID / 结构体

IID 不是 DLL 提供的，而是 7-Zip 家族固定的 {23170F69-40C1-278A-…} 命名空间。
实测：ZS DLL 的 IInArchive（7z 容器 handler 0x07、zstd handler 0x0E）在
CreateObject 上返回 S_OK，Open/GetNumberOfItems/Extract 全套走通，
IArchiveExtractCallback 的 vtable 顺序（IProgress 三槽 + GetStream/
PrepareOperation/SetOperationResult）与 SevenZipAbi.hpp 一致——否则
win.codec_probe 里那三次 opResult=0 的提取不可能成功。

**结论：ABI 不兼容项 = 0。** §8 列的是「不兼容会怎样」的备查项，本轮没有触发。

### 2.3 两个引擎的表差异

| | 官方 25.01 | ZS 26.02 |
|---|---|---|
| handler | 60 | **65**（+brotli +lzip +lz4 +lz5 +lizard） |
| method | 25 | **31**（+BROTLI LZ4 LIZARD LZ5 ZSTD FLZMA2） |
| method id（NMethodPropID::kID） | — | ZSTD=0x4F71101、BROTLI=0x4F71102、LZ4=0x4F71104 |

## 3. 二进制兼容性：跑通了什么（实测）

win.codec_probe（Tests/win/test_codec_probe.cpp）7 个用例 / 87 个断言全绿：

    codec-probe: 26.2.0.0 formats=65 methods=31 zstd=handler+method lz4=handler+method brotli=handler+method
    method ids: lzma2=0x21 brotli=0x4F71102 lz4=0x4F71104 zstd=0x4F71101
    zstd-compressed .7z extracts through the unchanged 7z handler          [OK]
    bare .zst/.lz4/.br decode through their handler CLSIDs
      zstd:   CreateObject(id=0x0E) hr=0x00000000; Extract hr=0x00000000 opResult=0 bytes=2270
      lz4:    CreateObject(id=0x0F) hr=0x00000000; Extract hr=0x00000000 opResult=0 bytes=2270
      brotli: CreateObject(id=0x1F) hr=0x00000000; Extract hr=0x00000000 opResult=0 bytes=2270
    stock 7z.dll: same .zst CLSID works, zstd .7z has no payload
      stock bare .zst:  CreateObject(id=0x0E) … opResult=0 bytes=2270
      stock zstd .7z:   CreateObject(id=0x07) … Extract hr=0x00000000 opResult=1 bytes=0
      stock ArchiveReader open=accepted trace: Zip…S_FALSE; 7z…S_OK; catalog=1 bytes=0
      ArchiveReader(.zst) opened=no trace: Zip hr=0x00000000; Zip.Open hr=0x00000001; 7z…S_FALSE;


读法（每一条都是证据，不是装饰）：

1. **zstd 压缩的 .7z，用我们现有的 ArchiveReader（Zip→7z 两个候选 CLSID）就能解**，
   解出 2270 B，与测试内建的 40 行原文逐字节相等。**产品代码零改动**即可读 ZS
   写出的 zstd 7z。
2. **裸 .zst/.lz4/.br 走对应 handler CLSID 也能解**，而且**同一个 CLSID 0x0E 在官方
   DLL 上也解得出**——这证明两件事：handler id 是跨构建稳定的常量；DLL 的 COM
   表面没变，变的只是表里有什么。
3. **官方 DLL 对 zstd .7z 的返回值是最容易被误读的一种**：Extract 返回
   hr=S_OK，SetOperationResult 才给出 opResult=1（unsupported method），
   字节数 0。**只看 HRESULT 的代码会把「不支持」当成「成功解出 0 字节」**。
   这条对产品是硬缺陷（见 §4.2），且与要不要 ZS 无关。

Tests/fixtures/zs/（提交进仓的只有 5 个归档，合计 ≈1.1 KB；**原文不提交**，
由测试里的 ExpectedPayload() 逐字节重建——提交一个 .txt 会被 core.autocrlf 在
checkout 时改成 CRLF，而归档里装的是 LF，比较会在干净克隆上无故变红）：

| 文件 | 大小 | 说明 |
|---|---|---|
| lzma2-baseline.7z | 286 B | 阴性对照的「引擎是健康的」基线 |
| zstd-method.7z | 264 B | Method = ZSTD:v1.5,l5，Packed 142 B |
| note.txt.zst | 146 B | 裸 zstd 单流 |
| note.txt.lz4 | 287 B | 裸 lz4 |
| note.txt.br | 123 B | 裸 brotli |

## 4. 如果要做，改造面在哪（按性价比排序）

### 4.1 裸 .zst/.lz4/.br 打不开 —— 是我们的表，不是 DLL

ArchiveReader::Open 目前**只**尝试 CLSID_CFormatZip 与 CLSID_CFormat7z
（PrismEngineBridge.hpp 里那个两元素 for 循环）。实测：ZS DLL 在手、
note.txt.zst 就在旁边，ArchiveReader::Open 依然返回 false（trace 只有 Zip/7z
两次尝试）。最小改动：

- 在 SevenZipAbi.hpp 旁新增 HandlerClsid(id)（本卡已在 CodecProbe.hpp 提供）＋
  按扩展名排一张候选表：.zst→0x0E、.lz4→0x0F、.br→0x1F、.lz5→0x10、
  .liz→0x11、.7z→0x07、.zip→0x01；
- Open 里「按扩展名先试、再试 Zip/7z」——**顺序不能反**，Zip handler 对任意
  字节都先回 S_FALSE 再回 S_OK 的老坑（openTrace 里那段
  Zip.Open hr=0x00000001）已经踩过一次。

成本 ≈ 半天 + 一个 .zst 的集成用例（可复用本卡 ExtractSingleStream 的形状）。
收益：**官方引擎白得 .zst 解压**，ZS 引擎白得 .zst/.lz4/.br。

### 4.2 opResult=1 必须变成用户看得见的话

现在 SmartExtractCallback::SetOperationResult 只数 filesWritten，丢掉
operationResult；于是「引擎不支持这个 codec」表现为**成功但 0 字节**。
建议：把 operationResult 冒到 Result（unsupportedMethods 计数），UI 文案
「这个压缩包用了 Zstd 方法，当前 7z.dll 不支持」+ 指向引擎设置。
成本 ≈ 2 小时，**不依赖 ZS**，属于修 bug 而不是加特性。

### 4.3 允许用户指向 ZS 的 7z.dll —— 已经支持，只差文档

Resolve7zDll() 的顺序是 PRISM_7Z_DLL → exe 目录 → 安装目录 → Program Files。
所以「用户自己装 ZS，把 7z.dll 放到程序旁 / 设环境变量」**现在就生效**，
win.codec_probe 就是这样把两个 DLL 分别加载验证的。产品侧零代码。
要不要**随包分发** ZS DLL 是许可/体积决策（§5），不是工程决策。

### 4.4 不推荐：外挂 Codecs\*.dll 路线

ZS 另发 Codecs-x64.7z（zstd.dll 761 KB、brotli.dll 1.0 MB、lz4.dll 272 KB…），
那是给**官方 7z.exe** 用的外部 codec 模块，走 SetCodecs + ICompressCodecsInfo。
我们的桥**从未调用 SetCodecs**，要用这条路得手写 ICompressCodecsInfo 并处理
g_ExternalCodecs 的生命周期（SevenZipLibrary 的析构注释里记着那条
use-after-free）。**投入远大于收益，本轮不做，也不建议做。**

## 5. 许可影响（逐条对本机 License.txt）

读的是包内原文，不是记忆：

- E:\DevTools\7zzs\License.txt（随 7z.dll 分发）：7z.dll 主体 **LGPL**，
  RAR 解压部分带 **unRAR 限制**，LZFSE 与 **ZSTD 解压**为 **BSD-3**，
  XXH64 为 **BSD-2**。
- E:\DevTools\7zzs-codecs\LICENSE：Tino Reichardt 的 codec 代码声明 **LGPL 2.1+**。

结论：

1. **「引擎不入库」原则不受影响。** 仍然是运行时 LoadLibraryW 一个外部 DLL，
   仓库里没有引擎源码、没有 codec 源码、没有二进制（本卡的 fixtures 是 1 KB
   归档样本，不是引擎）。
2. **LGPL 义务照旧**（今天加载官方 7z.dll 也一样）：分发时必须随附许可文本、
   声明 7-Zip/ZS 版本与来源、并提供 LGPL 源码获取途径（7-Zip 官方做法是给出
   源码下载地址/书面要约）。
3. **ZS 特有**：SetLargePageMode2 是纯新增导出，不改变链接方式；ZS 自己新增的
   codec 在其包里按 LGPL 声明，上游 zstd/brotli/lz4 各自是 BSD-3/MIT/BSD-2
   （**本轮未逐字校验上游 LICENSE 文本，若真要随包分发，法务需要复核**）。
4. **unRAR 限制**今天已经适用（stock DLL 同样含 RAR 解压），ZS 不新增限制。
5. 蓝图 ch.2 的「合并 ZS 源码进 7zip-core/ 子模块」（= C 档）会把 LGPL 代码
   变成**我们构建产物的一部分**，那时才需要讨论「动态链接豁免是否还成立」。
   **本轮结论是不走那条路，所以这个风险不成立。**

## 6. C 档（自建源码）要不要做：不必

- 实测：换 DLL 即可获得全部能力，零源码、零构建时间。
- 自建需要：VS + zstd/brotli/lz4 源码 + 7-Zip ZS 的 nasm 构建链，产物 ~5.4 MB，
  构建时间与维护成本都落在「每季度跟一次上游」上。
- **只有当「必须随包内置 Zstd 编码、且不接受外部 DLL」时**才值得做；现阶段
  产品连安装器都还没有，优先事项明显不是它。

## 7. 复现（从零到绿）

    # 1) 取 ZS 引擎（走 socks5 代理；release v26.02-v1.5.7-R2）
    curl.exe -sL --proxy socks5h://127.0.0.1:10808 -o 7z26.02-zstd-x64.exe https://github.com/mcmilk/7-Zip-zstd/releases/download/v26.02-v1.5.7-R2/7z26.02-zstd-x64.exe
    & 'E:\DevTools\7-Zip\7z.exe' x 7z26.02-zstd-x64.exe -oE:\DevTools\7zzs -y

    # 2) 生成 fixtures（归档已提交在 Tests/fixtures/zs/；原文只在生成时用到，
    #    测试里的 ExpectedPayload() 是同一段文本）
    $src='E:\Temp\prism-fixture'; New-Item -ItemType Directory -Force $src | Out-Null
    1..40 | % { "PrismArchive codec probe line $_ / zstd-lz4-brotli spike" } |
      Set-Content "$src\note.txt" -NoNewline -Encoding ascii
    $sz='E:\DevTools\7zzs\7z.exe'; $fx='Tests\fixtures\zs'
    & $sz a -t7z -m0=lzma2 "$fx\lzma2-baseline.7z" "$src\note.txt"
    & $sz a -t7z -m0=zstd  "$fx\zstd-method.7z"    "$src\note.txt"
    & $sz a -tZSTD  "$fx\note.txt.zst" "$src\note.txt"
    & $sz a -tLZ4   "$fx\note.txt.lz4" "$src\note.txt"
    & $sz a -tBROTLI "$fx\note.txt.br" "$src\note.txt"

    # 3) 构建 + 运行（ZS DLL 用环境变量指过去）
    . E:\wg\env.ps1 -Card c17
    Build-Only -Target win_codec_probe
    $env:PRISM_7Z_ZS_DLL='E:\DevTools\7zzs\7z.dll'
    $env:PRISM_7Z_DLL='E:\DevTools\7-Zip\7z.dll'
    ctest --test-dir E:\build\wt-c17 -R win.codec_probe --output-on-failure

CI 语义（诚实版）：runner 上没有 ZS DLL，所以 win.codec_probe 在 CI 上**是
Skipped**（SKIP_RETURN_CODE 77 + 必填 PASS_REGULAR_EXPRESSION
"ALL CODEC-PROBE CHECKS PASSED"，CONTRACTS D1）。**CI 绿 ≠ ZS 路径被跑过**；
纯分类那三个用例在 CI 上是真跑的（不依赖 DLL），ZS 相关的 4 个用例只在本地/
装好 ZS 的机器上跑。

## 8. 备查：如果 ABI 不兼容会怎样（本轮未触发）

留一张清单给将来换别的 ZS 构建时用：

| 维度 | 检查方法 | 本轮结果 |
|---|---|---|
| 导出名/序号 | dumpbin /exports 7z.dll 全量 diff | 14/14 同名同序号，+1 新增 |
| IID 集合 | 对目标 CLSID 试 CreateObject + QueryInterface | 0x07/0x0E/0x0F/0x1F 全部 S_OK |
| vtable 布局 | 真提取一次、比对 opResult/字节数 | 三次 opResult=0、字节数 2270 |
| 结构体布局（PROPVARIANT/PROPID） | 读 handler/method 表并核对计数 | 60/25 与 65/31，逐项对得上 |
| 线程/大页 | SetLargePageMode 存在性 | 存在；ZS 另加 SetLargePageMode2 |

## 9. 未验证 / 局限

- **未验证**：ZS 的 lz5/lizard/flzma2 实际往返（只读了表，没造样本）。
- **未验证**：ZS 的 7z.dll 在 ASan 下的稳定性（本卡只在 Release 下跑；
  CI 的 Windows lane 用 ASan，但那里没有 ZS DLL）。
- **未验证**：ZS 26.02 与官方 25.01 的 7zAES 是否有格式兼容差异（没做交叉往返）。
- 提取证据只覆盖 2270 B 小样本；**大文件/多线程/4 GB 以上**没测。
- 蓝图 ch.2 还提到「AVX2/AVX-512 向量化加速」——ZS 的 DLL 内部用哪套指令集
  **未验证**（不改变结论，但影响性能预期）。

## 10. 建议下一步（给排期用）

1. **本卡已产出**：Src/PrismWin/CodecProbe.hpp（纯查询 + 分类，可单测）、
   Tests/win/test_codec_probe.cpp、Tests/suites/c17-zstd.cmake、
   Tests/fixtures/zs/*、本文。
2. **下一张卡（推荐 P0，与 ZS 无关）**：opResult 冒到 ExtractCallback::Result，
   unsupported method 给出可操作文案（§4.2）。
3. **下一张卡（推荐 P1）**：ArchiveReader::Open 改成扩展名→CLSID 表驱动，
   官方引擎即得 .zst（§4.1）。
4. **再之后（P2，纯文档）**：README/设置里写清「可选 ZS 引擎 = 更好格式支持」，
   并把许可文本随包（§5）。**不要**做 ch.2 的源码合并（§6）。
