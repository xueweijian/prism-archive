# tools/fuzz — 畸形归档语料（C22）

对应蓝图 Phase 1 exit criteria：**10 000 个真实畸形 / 嵌套 / 乱码 / 加密归档样本**。

## 组成

| 文件 | 角色 |
|---|---|
| `gen_corpus.py` | 确定性语料生成器（纯 stdlib，无第三方依赖） |
| `Tests/test_fuzz_smoke.cpp` | 把语料喂给 PrismCore 决策层（PathSafety / SmartExtractor / SecurityGuard），全平台可跑 |
| `Tests/suites/c22-fuzz.cmake` | 唯一注册点（一卡一文件） |

## 为什么是「确定性语料」而不是接 libFuzzer / AFL

三条理由，都是本项目的硬约束，不是口味问题：

1. **判据是分布，不是随机。** Phase 1 要的是「10 000 个真实畸形样本」，即每类防御
   （Zip Slip、超长路径、深嵌套、伪造解压尺寸、乱码文件名、加密、截断头）都有
   已知数量的样本。覆盖率 fuzzer 会把预算花在「最容易触发新边」的地方，语料把预算
   花在**每个功能**上；判据要对得上功能清单，就得自己造。
2. **可复现性。** `--seed` + `--total` 决定字节，同一命令在任何机器上产出同一语料
   （容器 FNV-1a 会打印在 CI 日志里）。覆盖率 fuzzer 的崩溃只对「同一二进制 + 同一
   种子」可复现，而 PrismCore 是 header-only、每次提交都重编——那不是可复现产物。
   本卡的语料还有个额外好处：CI 红了，本地一条命令就能重放同一批样本。
3. **CI 时间。** 全量 10 000 样本生成 ~1 s、消费 ~1 s，所以它能进 push lane。
   libFuzzer 目标永远做不到这个价位，只能变成没人看的 nightly 任务（蓝图 7.2 要求
   push lane ≈2.5 min）。

## 用法

```bash
# 契约自检：每类 50..200 个 recipe、确定性、对照样本合法、容量 ≥10000
python3 tools/fuzz/gen_corpus.py --check --total 10000

# 生成（CI 用法）：一个 packed 容器 + 24 个真实文件（给 7z.dll 直开 / 人工看）
python3 tools/fuzz/gen_corpus.py --out /tmp/prism-fuzz --total 10000 --pack --loose 24

# 生成（人工排查用法）：10 000 个独立 .zip，便于直接丢给 7z / 资源管理器
python3 tools/fuzz/gen_corpus.py --out /tmp/prism-fuzz-loose --total 2000

# 冒烟回归（第二个参数是期望样本数，0 = 不检查）
./build/Tests/test_fuzz_smoke /tmp/prism-fuzz 10000
```

语料**永不入仓**：生成器拒绝写到仓库目录内（`--out` 必须在外），样本是构建产物。

## 11 类畸形（每类 50–200 个 recipe，按 `--total` 实例化为 10 000 个样本）

| 类 | 畸形 | 对应防御 |
|---|---|---|
| `truncated` | 在头部/数据/中央目录/EOCD 的任意结构地标处截断 | 打开失败路径必须干净、有理由 |
| `bad-crc` | 存储 CRC 或载荷字节损坏 | CRC 检测（并用合法样本校准，D4） |
| `overlong-path` | 200 B–45 KB 路径、>255 B 单段、边界 254/255/256 | 路径预算（`PathSafety` + 层策略） |
| `deep-nesting` | 3–780 层嵌套目录 | 深度预算，迭代不递归 |
| `traversal` | `../`、绝对路径、盘符、UNC、NUL、`....//` 等 | `SanitizeRelativePath` + `WriteGuard` |
| `fake-size` | 16 B 打包却声明 64 MiB–1 GiB 解压 | `BombFuse`（比值 + 磁盘双条件） |
| `bad-utf8` | 截断/过长/代理对/GBK/Shift-JIS/Big5/EUC-KR 文件名 | `IsUtf8Valid` + `RepairArchiveName` |
| `empty` | 0 字节、只有 EOCD、悬空中央目录、只剩目录项 | `EmptyArchive` 分支 |
| `zero-byte` | 结构合法但条目全为 0 字节 | 计划计数 / CRC=0 语义 |
| `encrypted` | ZipCrypto 位 + 伪造加密头、AES extra field | 必须先要密码，绝不静默规划 |
| `zip64-lies` | 无 zip64 extra 的 0xFFFFFFFF、伪 zip64、0xFFFF 计数 | 尺寸占位符不得让计划溢出 |

## 语料 ABI（容器格式）

```text
magic   "PRISMFZ1\n"
classes u16 n, then n x { u8 len, len bytes of ASCII class name }
samples repeated to EOF: u32 class_index, u32 size, size bytes of archive
```

类表写进容器不是装饰：消费端逐项比对类名与顺序，任一侧改名就是硬错误，
而不是「这一类 0 个样本」的静默通过。

## 契约断言（两面）

拒绝断言很容易写成空转，所以冒烟测试同时钉住两边：

- **敌意必拒**：traversal / 超长 / 超深 / 乱码 / 加密条目永不被 `Accept`，且每条
  `Reject`/`Rewrite`/`NeedsPassword` 都带非空、具体的理由；
- **干净必收**：完全合规的相对路径必须 `Accept`——「全都拒绝」也会被判红，
  否则拒绝断言毫无意义；
- 计划不变量：`reason` 非空、目标目录在输出根内、`fileCount`/`totalBytes` 与目录一致
  （含 64 位溢出检查）、`TransformItemPath` 结果永远落在计划目标内（Zip Slip）、
  碰撞解析要么给出唯一新名，要么设置错误。

## 负向控制（CONTRACTS D3）

同一份 `test_fuzz_smoke.cpp` 编两次：

| 目标 | 注入 | 期望 |
|---|---|---|
| `mutation.fuzz.accept_all` | 守卫全部放行（`PRISM_FUZZ_MUTANT_ACCEPT_ALL`） | 套件因「敌意条目被接受」变红（WILL_FAIL） |
| `mutation.fuzz.valid_rejected` | 对照样本的期望被反转（`PRISM_FUZZ_MUTANT_VALID_REJECTED`） | 套件因「预期被拒但通过了」变红（WILL_FAIL），失败行带 `CONTROL-VIOLATION` |
| `mutation.fuzz.injection_armed` | 无需注入，跑突变二进制 | 必须打印 `FUZZ MUTATION ARMED: accept-all` 且退出 0——编译定义拼错=静默无操作，这条把它变成红 |
| `mutation.fuzz.clean_build_unarmed` | 同上，跑干净二进制 | 必须打印 `NOT armed`，保证干净构建里没有残留突变 |

## 已知边界

- 这是**确定性语料 + 冒烟**，不是模糊测试引擎：不做覆盖率引导、不做变异搜索。
  它的价值是「每个防御都有已知数量的真实样本 + 可复现 + 能进 push lane」。
- 7z.dll 真开样本属于 Windows 侧可选增量（`--loose` 产出的真实文件就是给它的入口）；
  本卡的判据在策略层，Linux/Windows 同一套断言。

