#!/usr/bin/env python3
"""PrismArchive -- Kaggle 算力农场基准矩阵 (blueprint ch.7.2 性能验证面).

在 Kaggle 4 核 CPU kernel 上回答: 7z 固实块 (solid) 的"单趟解压一次"
相对"逐项解压"的墙钟优势, 随 文件数 N / 单文件大小 / 压缩等级 mx / 固实开关
如何变化. GitHub CI 只有 2 核, 这里给出第一批跨规模真实数字.

输出
  /kaggle/working/bench_results.jsonl   每行一个 JSON 对象 (config/pruned/error/meta)
  /kaggle/working/bench_summary.txt     人类可读汇总表

设计约束
  * 总时长 <= 40 min: 超预算的配置按估算代价剪枝, 记 pruned 行;
  * 任何配置崩溃记录 {"type":"error",...} 行, 绝不中断整个 kernel;
  * 不依赖本仓库 C++ 代码, 只测系统 7z (p7zip) -- 产品依赖的归档管线本身;
  * 测量口径: 单趟 = `7z x` 全量解到磁盘; 逐项 = 对每个文件单独
    `7z e -so` (stdout 丢弃, 即逐项的"最快可能"口径 => speedup 是下界).
"""
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time

# ---------------------------------------------------------------- 实验矩阵
NS = [10, 50, 200, 1000]
SIZES = [(8 * 1024, "8KB"), (256 * 1024, "256KB"), (4 * 1024 * 1024, "4MB")]
MXS = [1, 5, 9]
SOLIDS = [(False, "off"), (True, "on")]   # 非固实先跑(便宜), 固实后跑
REPS = 3                                   # 每配置测量 3 次取中位数

BUDGET_S = float(os.environ.get("BENCH_BUDGET_S", 40 * 60))
SOFT_PRUNE = 0.55    # 已耗时超过预算 55% => 不再开新配置 (先小 N, 时间过半跳大 N)
HARD_GATE = 0.92     # 已耗时 + 估算代价 > 预算 92% => 该配置剪枝
PER_CFG_TIMEOUT = 900   # 单配置硬上限 (秒), 防止估算失准拖死整个 kernel
ITEM_TIMEOUT = 300      # 单个 7z 进程硬上限

T7Z = None

# ------------------------------------------------------- 数据: 伪文本+随机混合
# 35% 不可压随机段 + 65% 词表伪文本: 给 LZMA 真实的压缩工作, 固定种子可复现.
# 每个尺寸类只生成 8 个模板文件循环复用 => 生成成本与 N 无关.
_WORDS = ["w%04d" % i for i in range(4096)]
_TPL = {}


def _template(n_bytes, seed):
    rng = random.Random(seed)
    parts, got = [], 0
    while got < n_bytes:
        take = min(65536, n_bytes - got)
        if rng.random() < 0.35:
            parts.append(os.urandom(take))
        else:
            txt = " ".join(rng.choice(_WORDS) for _ in range(take // 7 + 8))
            parts.append(txt.encode()[:take])
        got += take
    return b"".join(parts)[:n_bytes]


def template(size, idx):
    key = (size, idx % 8)
    if key not in _TPL:
        _TPL[key] = _template(size, 1000 + key[1])
    return _TPL[key]


def build_input(d_in, n, size):
    os.makedirs(d_in, exist_ok=True)
    for j in range(n):
        with open(os.path.join(d_in, "f_%05d.bin" % j), "wb") as fh:
            fh.write(template(size, j))


# ----------------------------------------------------------------- 机器规格
def _cpu_model():
    try:
        with open("/proc/cpuinfo") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def machine():
    mem_gb = 0
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith("MemTotal"):
                    mem_gb = round(int(line.split()[1]) / 1048576, 1)
                    break
    except OSError:
        pass
    r = subprocess.run([T7Z], capture_output=True, text=True)
    banner = ((r.stdout or "") + (r.stderr or "")).splitlines()
    ver = banner[0].strip() if banner else "7z(?)"
    return {
        "cpu_cores": os.cpu_count(),
        "cpu": _cpu_model(),
        "mem_gb": mem_gb,
        "kernel": platform.release(),
        "seven_zip": ver,
    }


# ------------------------------------------------------------------ 工具函数
def find_7z():
    for name in ("7z", "7za", "7zr"):
        p = shutil.which(name)
        if p:
            return p
    if os.path.isdir("/kaggle"):   # Kaggle 镜像默认没有 7z, apt 安装
        subprocess.run(["apt-get", "update", "-qq"], capture_output=True)
        subprocess.run(["apt-get", "install", "-y", "-qq", "p7zip-full"],
                       capture_output=True)
        for name in ("7z", "7za", "7zr"):
            p = shutil.which(name)
            if p:
                return p
    raise SystemExit("no 7z binary found (tried 7z/7za/7zr, apt p7zip-full)")


def run(cmd, timeout=ITEM_TIMEOUT, cwd=None):
    """跑一条命令, 返回墙钟秒数; 非零退出/超时抛 RuntimeError.

    cwd 不是可选的细节: `7z a ... .` 里的 "." 由进程工作目录决定, 不传 cwd
    就会把调用者的整个目录打包进去 (2026-11-01 实测: 50 个 256KB 文件本该
    产出 3.3MB, 实际产出 70.9MB).
    """
    t0 = time.monotonic()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       timeout=timeout, cwd=cwd)
    dt = time.monotonic() - t0
    if r.returncode != 0:
        tail = r.stderr[-240:].decode("utf-8", "replace").replace("\n", " ")
        raise RuntimeError("rc=%d %s | %s" % (r.returncode, " ".join(cmd[:5]), tail))
    return dt


# ------------------------------------------------------- 代价估算 (剪枝依据)
# 保守取慢速度: 宁可多剪一个配置, 不可拖爆 40min 预算.
_PACK_MBPS = {1: 60.0, 5: 20.0, 9: 8.0}
_DEC_MBPS = 150.0
_SPAWN_S = 0.02


def estimate_cost(n, size, mx, solid):
    data_mb = n * size / 1e6
    if solid:   # 逐项要回放固实前缀: 总解码量 ≈ data × (N-1)/2
        per_rep = (data_mb * (n - 1) / 2) / _DEC_MBPS + n * _SPAWN_S
    else:       # 非固实: 逐项只解该项本身
        per_rep = data_mb / _DEC_MBPS + n * _SPAWN_S
    return data_mb / _PACK_MBPS[mx] + REPS * per_rep


def matrix():
    for n in NS:
        for size, lbl in SIZES:
            for mx in MXS:
                for solid, so in SOLIDS:
                    yield n, size, lbl, mx, solid, so

# ------------------------------------------------------------------ 测一个配置
def archive_names(arc):
    """`7z l -slt` 解析归档内条目名 (避免猜测打包时的路径前缀).

    坑 1 (2026-11-01 实测): 7z 的列表输出里 `Path = ` 也包含归档自身那一行,
    直接取第一条会把归档当成一个条目 -> 逐项循环多跑一次 `7z e` 打开自己.
    自检: 名字等于归档路径本身, 或以 .7z/.zip 结尾且等于 arc 的 basename.
    坑 2: `7z l` 相对路径会按进程 CWD 解析, 所以统一传绝对路径 + cwd.
    """
    r = subprocess.run([T7Z, "l", "-slt", arc], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("listing failed rc=%d" % r.returncode)
    names = [ln.split("=", 1)[1].strip() for ln in r.stdout.splitlines()
             if ln.startswith("Path = ")]
    arc_abs = os.path.abspath(arc)
    arc_base = os.path.basename(arc_abs)
    names = [n for n in names
             if os.path.abspath(n) != arc_abs and os.path.basename(n) != arc_base]
    if not names:
        raise RuntimeError("archive has no entries (after dropping the self row)")
    return names


def run_config(work, n, size, lbl, mx, solid, so, mach, elapsed0):
    d_in = os.path.join(work, "in")
    d_out = os.path.join(work, "out")
    arc = os.path.join(work, "bench.7z")
    for d in (d_in, d_out):
        shutil.rmtree(d, ignore_errors=True)

    build_input(d_in, n, size)
    # cwd=d_in 是必需的: 不加 cwd 时 "." 指的是本进程的工作目录 (仓库根),
    # 会把整个仓库打进去 -- 实测同一个 50 文件配置产出 70.9MB 而不是 3.3MB,
    # 此后所有 sweep/peritem 数字都毫无意义 (2026-11-01 实测发现).
    pack_s = run([T7Z, "a", "-t7z", "-mx=%d" % mx, "-ms=%s" % so, "-y",
                  os.path.abspath(arc), "."], cwd=d_in, timeout=PER_CFG_TIMEOUT)
    arc_mb = os.path.getsize(arc) / 1e6
    names = archive_names(arc)

    # 单趟: 7z x 全量解到磁盘, 每趟之间清空输出目录 (口径一致)
    sweep_runs = []
    for _ in range(REPS):
        shutil.rmtree(d_out, ignore_errors=True)
        sweep_runs.append(run([T7Z, "x", "-y", "-o" + d_out, arc],
                              timeout=PER_CFG_TIMEOUT))

    # 逐项: 每个文件单独 7z e -so (stdout 丢弃 => 逐项的最快可能口径)
    per_runs = []
    for _ in range(REPS):
        t = 0.0
        for name in names:
            t += run([T7Z, "e", "-y", "-so", arc, name], timeout=ITEM_TIMEOUT)
        per_runs.append(t)

    sweep_med = statistics.median(sweep_runs)
    per_med = statistics.median(per_runs)
    row = {
        "type": "config", "N": n, "file_size": lbl, "file_size_bytes": size,
        "mx": mx, "solid": solid, "status": "ok",
        "pack_s": round(pack_s, 3), "archive_mb": round(arc_mb, 2),
        "sweep_s": [round(x, 3) for x in sweep_runs],
        "sweep_s_med": round(sweep_med, 3),
        "peritem_s": [round(x, 3) for x in per_runs],
        "peritem_s_med": round(per_med, 3),
        "speedup": round(per_med / sweep_med, 2) if sweep_med > 0 else None,
        "cpu": "%dx %s" % (mach["cpu_cores"], mach["cpu"]),
        "mem_gb": mach["mem_gb"], "seven_zip": mach["seven_zip"],
        "elapsed_s": round(time.monotonic() - elapsed0, 1),
    }
    shutil.rmtree(work, ignore_errors=True)
    return row


# --------------------------------------------------------------------- 汇总表
def summary_table(rows):
    hdr = ("N", "size", "mx", "solid", "pack_s", "sweep_s", "peritem_s",
           "speedup", "status")
    lines = ["%-6s %-7s %-4s %-6s %-9s %-9s %-10s %-8s %s" % hdr,
             "-" * 72]
    for r in rows:
        if r.get("type") != "config":
            continue
        lines.append("%-6d %-7s %-4d %-6s %-9.2f %-9.3f %-10.2f %-8s %s" % (
            r["N"], r["file_size"], r["mx"], ("on" if r["solid"] else "off"),
            r["pack_s"], r["sweep_s_med"], r["peritem_s_med"],
            ("%.2fx" % r["speedup"]) if r.get("speedup") else "n/a",
            r["status"]))
    ok = [r for r in rows if r.get("type") == "config"]
    pr = [r for r in rows if r.get("type") == "pruned"]
    er = [r for r in rows if r.get("type") == "error"]
    lines.append("-" * 72)
    lines.append("configs: %d ok, %d pruned, %d error  (reps=%d, "
                 "speedup = peritem/sweep, 逐项口径已含 -so 丢弃输出的下界偏差)"
                 % (len(ok), len(pr), len(er), REPS))
    return "\n".join(lines)


# ------------------------------------------------------------------------ 主流程
def main():
    global NS, SIZES, MXS, REPS, BUDGET_S
    args = sys.argv[1:]
    if "--quick" in args:   # 本地小规模预跑
        NS, MXS, REPS, BUDGET_S = [10, 50], [1, 5], 1, 600.0
        SIZES = SIZES[:2]
    if "--summary" in args:  # 只打印已有结果的汇总表
        path = args[args.index("--summary") + 1]
        rows = [json.loads(l) for l in open(path) if l.strip()]
        print(summary_table(rows))
        return

    t0 = time.monotonic()
    out_dir = "/kaggle/working" if os.path.isdir("/kaggle/working") else "."
    res_path = os.path.join(out_dir, "bench_results.jsonl")
    sum_path = os.path.join(out_dir, "bench_summary.txt")

    global T7Z
    T7Z = find_7z()
    mach = machine()
    print("[bench] 7z=%s | %d cores | %s | %.1f GB | budget=%.0fs"
          % (mach["seven_zip"], mach["cpu_cores"], mach["cpu"],
             mach["mem_gb"], BUDGET_S), flush=True)

    rows = []
    fh = open(res_path, "a")

    def emit(row):
        rows.append(row)
        fh.write(json.dumps(row, ensure_ascii=False) + "\n")
        fh.flush()

    emit({"type": "meta", "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
          "machine": mach, "budget_s": BUDGET_S, "reps": REPS,
          "matrix_planned": len(list(matrix())),
          "python": platform.python_version()})

    n_ok = n_pruned = n_err = 0
    for n, size, lbl, mx, solid, so in matrix():
        elapsed = time.monotonic() - t0
        base = {"N": n, "file_size": lbl, "mx": mx, "solid": solid}
        if elapsed > SOFT_PRUNE * BUDGET_S:
            emit({**base, "type": "pruned", "reason": "time_budget",
                  "elapsed_s": round(elapsed, 1)})
            n_pruned += 1
            continue
        est = estimate_cost(n, size, mx, solid)
        if elapsed + est > HARD_GATE * BUDGET_S:
            emit({**base, "type": "pruned", "reason": "estimate_overrun",
                  "est_s": round(est, 1), "elapsed_s": round(elapsed, 1)})
            n_pruned += 1
            continue
        tag = "N=%-4d %-5s mx=%d ms=%s" % (n, lbl, mx, so)
        print("[bench][%5.0fs] %-26s est=%5.0fs run ..." % (elapsed, tag, est),
              flush=True)
        try:
            row = run_config("/kaggle/tmp/bench" if os.path.isdir("/kaggle/tmp")
                             else os.path.join(os.environ.get("TMPDIR", "/tmp"),
                                               "bench"),
                             n, size, lbl, mx, solid, so, mach, t0)
            emit(row)
            n_ok += 1
            print("[bench][%5.0fs] %-26s pack=%6.2fs sweep=%7.3fs "
                  "peritem=%8.2fs speedup=%s"
                  % (time.monotonic() - t0, tag, row["pack_s"],
                     row["sweep_s_med"], row["peritem_s_med"],
                     row["speedup"]), flush=True)
        except Exception as exc:   # 单配置崩溃 => error 行, kernel 继续
            emit({**base, "type": "error", "error": str(exc)[:400],
                  "elapsed_s": round(time.monotonic() - t0, 1)})
            n_err += 1
            print("[bench][%5.0fs] %-26s ERROR %s"
                  % (time.monotonic() - t0, tag, exc), flush=True)
            shutil.rmtree("/kaggle/tmp/bench", ignore_errors=True)

    table = summary_table(rows)
    print(table, flush=True)
    with open(sum_path, "w") as fh2:
        fh2.write(table + "\n")
    emit({"type": "done", "ok": n_ok, "pruned": n_pruned, "error": n_err,
          "total_s": round(time.monotonic() - t0, 1)})
    fh.close()
    print("[bench] DONE ok=%d pruned=%d error=%d total=%.0fs"
          % (n_ok, n_pruned, n_err, time.monotonic() - t0), flush=True)


if __name__ == "__main__":
    main()
