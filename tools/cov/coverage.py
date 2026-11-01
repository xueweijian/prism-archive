#!/usr/bin/env python3
r"""C23 -- measurable coverage for PrismArchive, one command, auto-selected engine.

The project's claim is "the tests are hard". This turns it into numbers:
per-module line/function coverage, measured after a real CTest run, with the
low-coverage-but-important modules called out explicitly.

Engine selection (auto):
  Windows  OpenCppCoverage (if installed) -> clang-cl -fprofile-instr-generate
           + llvm-profdata/llvm-cov. MSVC has no line-coverage mode; clang-cl is
           the instrumenting compiler that ships with the VS Build Tools.
  Linux    gcovr -> lcov --capture -> raw gcov scan (all three read a
           --coverage build; they are readers, not different measurements).

Usage:
  python tools/cov/coverage.py run --build-dir E:\build\cov-c23 --engine auto \
      --json out/coverage.json --markdown out/coverage.md
  python tools/cov/coverage.py selftest

Exit code: 0 when the measurement produced numbers and every critical module in
the policy was measured; 3 when no engine could measure (a loud SKIP, never a
silent success).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_POLICY = Path(__file__).resolve().parent / "coverage_policy.json"


def which(name, extra_dirs=()):
    p = shutil.which(name)
    if p:
        return p
    for d in extra_dirs:
        for ext in (".exe", ""):
            cand = Path(d) / (name + ext)
            if cand.is_file():
                return str(cand)
    return None


def llvm_tools():
    dirs = []
    if os.environ.get("LLVM_BIN"):
        dirs.append(os.environ["LLVM_BIN"])
    dirs += [r"C:\Program Files\LLVM\bin", r"C:\Program Files (x86)\LLVM\bin"]
    return which("clang-cl", dirs), which("llvm-profdata", dirs), which("llvm-cov", dirs)


def run(cmd, timeout=3600, cwd=None, env=None, log=None):
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout,
                       cwd=str(cwd) if cwd else None, env=env)
    out = (p.stdout or "") + (p.stderr or "")
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        Path(log).write_text(out, encoding="utf-8", errors="replace")
    return p.returncode, out


def normalize_path(p):
    p = p.replace("\\", "/")
    if re.match(r"^[a-z]:/", p):
        p = p[0].upper() + p[1:]
    return p


def module_of(path):
    p = normalize_path(path)
    m = re.search(r"/Src/(PrismCore|PrismWin|App)/(.+)", p, re.I)
    if not m:
        return ""
    area = {"prismcore": "PrismCore", "prismwin": "PrismWin", "app": "App"}[m.group(1).lower()]
    return "%s/%s" % (area, Path(m.group(2)).stem)


class Lcov:
    def __init__(self):
        self.files = {}

    def _file(self, path):
        return self.files.setdefault(normalize_path(path), {"lines": {}, "functions": {}})

    def add_record(self, path, lines, functions):
        f = self._file(path)
        for n, c in lines.items():
            f["lines"][n] = max(f["lines"].get(n, 0), c)
        for n, c in functions.items():
            f["functions"][n] = max(f["functions"].get(n, 0), c)

    def merge_text(self, text):
        cur = None
        for line in text.splitlines():
            if line.startswith("SF:"):
                cur = self._file(line[3:].strip())
            elif line.startswith("DA:") and cur is not None:
                parts = line[3:].split(",")
                if len(parts) >= 2:
                    try:
                        n, c = int(parts[0]), int(float(parts[1]))
                    except ValueError:
                        continue
                    cur["lines"][n] = max(cur["lines"].get(n, 0), c)
            elif line.startswith("FNDA:") and cur is not None:
                parts = line[5:].split(",", 1)
                if len(parts) == 2:
                    try:
                        c = int(float(parts[0]))
                    except ValueError:
                        continue
                    name = parts[1].strip()
                    cur["functions"][name] = max(cur["functions"].get(name, 0), c)
            elif line.strip() == "end_of_record":
                cur = None

    def summary(self):
        out = {}
        for path, f in self.files.items():
            mod = module_of(path)
            if not mod:
                continue
            s = out.setdefault(mod, {"lines_total": 0, "lines_covered": 0,
                                     "fn_total": 0, "fn_covered": 0, "files": []})
            lt, lc = len(f["lines"]), sum(1 for c in f["lines"].values() if c > 0)
            ft, fc = len(f["functions"]), sum(1 for c in f["functions"].values() if c > 0)
            s["lines_total"] += lt
            s["lines_covered"] += lc
            s["fn_total"] += ft
            s["fn_covered"] += fc
            s["files"].append({"file": path, "lines_total": lt, "lines_covered": lc})
        for s in out.values():
            s["line_pct"] = round(100.0 * s["lines_covered"] / s["lines_total"], 2) if s["lines_total"] else 0.0
            s["fn_pct"] = round(100.0 * s["fn_covered"] / s["fn_total"], 2) if s["fn_total"] else 0.0
        return out


# ---------------------------------------------------------------------------
# engines -- each returns (Lcov | None, detail)
# ---------------------------------------------------------------------------
def configure_clang(build_dir, source_dir, reconfigure):
    clang_cl, profdata, cov = llvm_tools()
    if not (clang_cl and profdata and cov):
        return False, "clang-cl/llvm-profdata/llvm-cov not all found (set LLVM_BIN)"
    if (Path(build_dir) / "CMakeCache.txt").is_file() and not reconfigure:
        return True, "reused existing clang build tree"
    cmd = [
        "cmake", "-S", str(source_dir), "-B", str(build_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=" + clang_cl,
        "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping /EHsc",
        "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate",
        "-DPRISM_BUILD_WIN_LAYER=ON",
        "-DPRISM_WARNINGS_AS_ERRORS=OFF",
    ]
    rc, out = run(cmd, timeout=600)
    return rc == 0, out[-800:]


def ctest_run(build_dir, regex, exclude, junit, env):
    # A default timeout bounds tests that never declared one: a hung test must
    # not eat the whole measurement window (per-test TIMEOUT still wins).
    cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure",
           "--timeout", "180", "--output-junit", str(junit)]
    if regex:
        cmd += ["-R", regex]
    if exclude:
        cmd += ["-E", exclude]
    return run(cmd, timeout=7200, env=env)


def junit_tests(path):
    out = {}
    try:
        root = ET.parse(str(path)).getroot()
    except Exception:
        return out
    for tc in root.iter("testcase"):
        out[tc.get("name", "")] = tc.find("skipped") is not None
    return out


def lasttest_ran(bd):
    """Names of tests that completed in the most recent ctest run (killed runs too)."""
    path = Path(bd) / "Testing" / "Temporary" / "LastTest.log"
    names = set()
    if not path.is_file():
        return names
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*\d+/\d+ Test\s+#\d+:\s+(\S+)", line)
        if m:
            names.add(m.group(1))
        m = re.match(r"\s*Test\s+#\d+:\s+(\S+)", line)
        if m:
            names.add(m.group(1))
    return names


def ctest_json(build_dir):
    rc, out = run(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"], timeout=120)
    if rc != 0:
        return {}
    data = json.loads(out)
    return {t.get("name"): {"command": t.get("command", [])} for t in data.get("tests", [])}


def engine_clang(build_dir, source_dir, args):
    clang_cl, profdata, cov = llvm_tools()
    if not (clang_cl and profdata and cov):
        return None, "clang-cl/llvm tools missing"
    bd = Path(build_dir).resolve()
    prof = bd / "prof"
    if args.skip_ctest:
        # Reuse the profile data already on disk (phase runs); do not pretend a
        # fresh ctest happened. Only tests that actually finished are exported.
        ran = junit_tests(args.ran_from) if args.ran_from else lasttest_ran(bd)
        print("  ctest: reused existing profile (%d tests in LastTest.log)" % len(ran))
    else:
        if prof.is_dir():
            shutil.rmtree(prof)
        prof.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        env["LLVM_PROFILE_FILE"] = str(prof / "cov-%p-%m.profraw")
        junit = bd / "cov-junit.xml"
        rc, out = ctest_run(bd, args.filter, args.exclude, junit, env)
        ran = junit_tests(junit)
        summary = [l.strip() for l in out.splitlines() if "tests passed" in l or "tests failed" in l]
        print("  ctest: %s (%s)" % ("ok" if rc == 0 else "FAILED", summary[-1] if summary else "no summary"))

    raws = sorted(prof.glob("*.profraw"))
    if not raws:
        return None, "no .profraw produced (did ctest actually run?)"
    merged = bd / "coverage.profdata"
    rc, out = run([profdata, "merge", "-sparse"] + [str(p) for p in raws] + ["-o", str(merged)], timeout=600)
    if rc != 0:
        return None, "llvm-profdata merge failed: %s" % out[-500:]

    tests = ctest_json(bd)
    lcov = Lcov()
    exported = 0
    for name, t in tests.items():
        if ran.get(name) is None or ran.get(name) is True:
            continue  # not selected by -R, or the test SKIPPED: its binary has no profile
        exe = Path(t["command"][0]) if t.get("command") else None
        if not exe or not exe.is_file() or exe.suffix.lower() != ".exe":
            continue
        rc, out = run([cov, "export", "--format=lcov", str(exe),
                       "-instr-profile", str(merged)], timeout=300)
        if rc == 0 and out.strip():
            lcov.merge_text(out)
            exported += 1
    if not exported:
        return None, "llvm-cov export produced nothing (no instrumented test binaries?)"
    print("  export: %d instrumented binaries, %d source files" % (exported, len(lcov.files)))
    return lcov, "%d binaries" % exported


def parse_cobertura(xml, bd):
    lcov = Lcov()
    for cls in ET.parse(str(xml)).getroot().iter("class"):
        filename = cls.get("filename", "")
        if not filename:
            continue
        p = Path(filename)
        if not p.is_absolute():
            p = bd / filename
        lines, funcs = {}, {}
        for ln in cls.iter("line"):
            try:
                n, hits = int(ln.get("number")), int(ln.get("hits", "0"))
            except (TypeError, ValueError):
                continue
            lines[n] = hits
            if ln.get("type") == "function" or ln.get("name"):
                funcs[ln.get("name") or ("line%d" % n)] = hits
        lcov.add_record(str(p), lines, funcs)
    return lcov


def engine_opencpp(build_dir, source_dir, args):
    occ = which("OpenCppCoverage", [r"C:\Program Files\OpenCppCoverage"])
    if not occ:
        return None, "OpenCppCoverage not installed"
    bd = Path(build_dir).resolve()
    xml = bd / "opencpp-cobertura.xml"
    cmd = [occ, "--sources", str(Path(source_dir) / "Src"),
           "--export_type", "cobertura:" + str(xml), "--",
           "ctest", "--test-dir", str(bd), "--output-on-failure"]
    if args.filter:
        cmd += ["-R", args.filter]
    if args.exclude:
        cmd += ["-E", args.exclude]
    rc, out = run(cmd, timeout=7200)
    if not xml.is_file():
        return None, "OpenCppCoverage run failed: %s" % out[-400:]
    return parse_cobertura(xml, bd), "OpenCppCoverage cobertura"


def engine_gcov(build_dir, source_dir, args, reader):
    bd = Path(build_dir).resolve()
    if reader == "gcovr":
        gcovr = which("gcovr")
        if not gcovr:
            return None, "gcovr not installed"
        js = bd / "gcovr.json"
        rc, out = run([gcovr, "--root", str(source_dir), "--json", str(js), str(bd)], timeout=1800)
        if rc != 0:
            return None, "gcovr failed: %s" % out[-400:]
        data = json.loads(Path(js).read_text(encoding="utf-8"))
        lcov = Lcov()
        for f in data.get("files", []):
            lt = int(f.get("line_total", 0))
            lc = int(f.get("line_covered", 0))
            lines = {i: 1 for i in range(1, lc + 1)}
            lines.update({i: 0 for i in range(lc + 1, lt + 1)})
            ft = int(f.get("function_total", 0) or 0)
            fc = int(f.get("function_covered", 0) or 0)
            funcs = {("fn%d" % i): 1 for i in range(fc)}
            funcs.update({("fn%d" % i): 0 for i in range(fc, ft)})
            lcov.add_record(f.get("filename") or f.get("file") or "", lines, funcs)
        return lcov, "gcovr"
    if reader == "lcov":
        lcov_bin = which("lcov") or which("lcov.perl")
        if not lcov_bin:
            return None, "lcov not installed"
        info = bd / "coverage.info"
        rc, out = run([lcov_bin, "--capture", "--directory", str(bd),
                       "--output-file", str(info), "--rc", "branch_coverage=0"], timeout=1800)
        if rc != 0 or not info.is_file():
            return None, "lcov --capture failed: %s" % out[-400:]
        lcov = Lcov()
        lcov.merge_text(info.read_text(encoding="utf-8", errors="replace"))
        return lcov, "lcov --capture"
    gcov = which("gcov")
    if not gcov:
        return None, "gcov not installed"
    lcov = Lcov()
    count = 0
    for gcda in bd.rglob("*.gcda"):
        rc, out = run([gcov, "-p", "-b", "-c", str(gcda)], cwd=gcda.parent, timeout=120)
        if rc != 0:
            continue
        for m in re.finditer(r"^File '(.+)'\nLines executed:([\d.]+)% of (\d+)", out, re.M):
            total, pct = int(m.group(3)), float(m.group(2))
            covered = int(round(total * pct / 100.0))
            lines = {i: 1 for i in range(1, covered + 1)}
            lines.update({i: 0 for i in range(covered + 1, total + 1)})
            lcov.add_record(m.group(1), lines, {})
            count += 1
    if not count:
        return None, "gcov produced no file records"
    return lcov, "raw gcov"


# ---------------------------------------------------------------------------
# policy / reporting
# ---------------------------------------------------------------------------
def load_policy(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if "modules" not in data:
        raise ValueError("policy has no 'modules' map")
    return data


def evaluate(summary, policy):
    notes = policy.get("modules", {})
    rows, alerts, watch = [], [], []
    for mod, s in sorted(summary.items()):
        pol = notes.get(mod, {})
        status = "OK"
        if pol.get("critical") and s["line_pct"] < pol.get("min_line", 80):
            status = "LOW/CRITICAL"
            alerts.append((mod, s, pol))
        # Watchlist: modules whose failure modes are silent or security-relevant
        # even when the global total looks fine (e.g. PrismEngineBridge's error
        # paths). Below watch_max is reported, but it does not block the run.
        if pol.get("watch") and s["line_pct"] < pol.get("watch_max", 90):
            if status == "OK":
                status = "LOW/WATCH"
            watch.append((mod, s, pol))
        rows.append((mod, s, pol, status))
    unmeasured = [(m, p) for m, p in notes.items() if p.get("critical") and m not in summary]
    return rows, alerts, unmeasured, watch


def report_markdown(summary, policy, meta):
    rows, alerts, unmeasured, watch = evaluate(summary, policy)
    lines = [
        "# 覆盖率摘要（C23 自动生成）",
        "",
        "- 引擎：`%s`" % meta.get("engine", "?"),
        "- 构建树：`%s`" % meta.get("build_dir", "?"),
        "- ctest：`%s`（%s）" % (meta.get("ctest") or "(all)", meta.get("ran", "?")),
        "- 生成时间：%s" % meta.get("timestamp", ""),
        "",
        "## 每模块行/函数覆盖率",
        "",
        "| 模块 | 行覆盖 | 函数覆盖 | 判定 | 说明 |",
        "|---|---|---|---|---|",
    ]
    total_l = total_lc = total_f = total_fc = 0
    for mod, s, pol, status in rows:
        total_l += s["lines_total"]
        total_lc += s["lines_covered"]
        total_f += s["fn_total"]
        total_fc += s["fn_covered"]
        note = (pol.get("note") or "").replace("|", "/")
        lines.append(
            "| `%s` | %.1f%% (%d/%d) | %.1f%% (%d/%d) | %s | %s |"
            % (mod, s["line_pct"], s["lines_covered"], s["lines_total"],
               s["fn_pct"], s["fn_covered"], s["fn_total"], status, note)
        )
    pct = 100.0 * total_lc / total_l if total_l else 0.0
    fpct = 100.0 * total_fc / total_f if total_f else 0.0
    lines.append("| **TOTAL** | **%.1f%%** (%d/%d) | **%.1f%%** (%d/%d) | | |"
                 % (pct, total_lc, total_l, fpct, total_fc, total_f))
    lines += ["", "## 覆盖率低但重要（策略门限）", ""]
    if alerts:
        for mod, s, pol in alerts:
            lines.append("- **%s**：行覆盖 %.1f%% < 门限 %s%% —— %s"
                         % (mod, s["line_pct"], pol.get("min_line", 80),
                            pol.get("note", "关键路径")))
    else:
        lines.append("- 无：策略里的关键模块全部达到门限。")
    if unmeasured:
        lines += ["", "未测量的关键模块（**不能当作通过**）："]
        for mod, pol in unmeasured:
            lines.append("- **%s**：%s" % (mod, pol.get("note", "关键模块")))
    lines += ["", "## 低覆盖但重要（watchlist，不阻断）", ""]
    if watch:
        lines.append("这些模块的失败模式是静默的（错误路径 / 安全 / 并发），")
        lines.append("所以门槛比全局高；低于门槛不等于测试没价值，而是下一条该补的测试在这里。")
        lines.append("")
        lines.append("| 模块 | 行覆盖 | 未命中行 | 说明 |")
        lines.append("|---|---|---|---|")
        for mod, s, pol in watch:
            miss = s["lines_total"] - s["lines_covered"]
            lines.append("| **%s** | %.1f%% (%d/%d) | %d | %s |"
                         % (mod, s["line_pct"], s["lines_covered"], s["lines_total"],
                            miss, (pol.get("note") or "").replace("|", "/")))
    else:
        lines.append("- 无：watchlist 全部达标。")
    lines += ["", "## 全部文件", "", "| 文件 | 行覆盖 |", "|---|---|"]
    for mod, s in sorted(summary.items()):
        for f in sorted(s["files"], key=lambda x: x["file"]):
            p = 100.0 * f["lines_covered"] / f["lines_total"] if f["lines_total"] else 0.0
            lines.append("| `%s` | %.1f%% (%d/%d) |"
                         % (f["file"], p, f["lines_covered"], f["lines_total"]))
    return "\n".join(lines) + "\n", alerts, unmeasured


# ---------------------------------------------------------------------------
# selftest -- calibrate the parser before trusting its numbers (CONTRACTS D4)
# ---------------------------------------------------------------------------
SELFTEST_LCOV = """SF:/x/Src/PrismCore/Alpha.hpp
FN:1,a
FNDA:3,a
DA:1,3
DA:2,0
end_of_record
SF:/x/Src/PrismWin/Beta.hpp
FN:5,b
FNDA:0,b
DA:5,0
end_of_record
"""


def selftest():
    lc = Lcov()
    lc.merge_text(SELFTEST_LCOV)
    lc.merge_text("""SF:/x/Src/PrismCore/Alpha.hpp
DA:2,7
end_of_record
""")
    s = lc.summary()
    ok = True
    if s.get("PrismCore/Alpha", {}).get("line_pct") != 100.0:
        ok = False
    if s.get("PrismCore/Alpha", {}).get("lines_covered") != 2:
        ok = False
    if s.get("PrismWin/Beta", {}).get("fn_covered") != 0:
        ok = False
    if module_of("/x/Tests/test_foo.cpp") != "":
        ok = False
    policy = load_policy(DEFAULT_POLICY)
    _, alerts, _, _ = evaluate(
        s, {"modules": {"PrismWin/Beta": {"critical": True, "min_line": 80, "note": "x"}}}
    )
    if not alerts or not any(a[0] == "PrismWin/Beta" for a in alerts):
        ok = False
    alpha = s.get("PrismCore/Alpha", {"line_pct": 0, "fn_pct": 0})
    print("C23 COVERAGE SELFTEST: %s (alpha=%.1f%% beta=%.1f%% alerts=%d)"
          % ("OK" if ok else "FAILED", alpha["line_pct"],
             s.get("PrismWin/Beta", {}).get("line_pct", 0), len(alerts)))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="C23 coverage driver (auto engine).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run")
    r.add_argument("--build-dir", required=True)
    r.add_argument("--source-dir", default=str(ROOT))
    r.add_argument("--engine", default="auto",
                   choices=["auto", "clang", "opencpp", "gcovr", "lcov", "gcov"])
    r.add_argument("--filter", default="")
    r.add_argument("--exclude", default="")
    r.add_argument("--configure", action="store_true")
    r.add_argument("--reconfigure", action="store_true")
    r.add_argument("--no-build", action="store_true")
    r.add_argument("--skip-ctest", action="store_true",
                   help="reuse profile data on disk (phase runs); never re-runs tests")
    r.add_argument("--ran-from", default="",
                   help="JUnit XML listing the tests that produced the profile data")
    r.add_argument("--json", default="")
    r.add_argument("--markdown", default="")
    r.add_argument("--policy", default=str(DEFAULT_POLICY))
    r.add_argument("--parallel", type=int, default=4)
    sub.add_parser("selftest")

    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()

    src = Path(args.source_dir).resolve()
    bd = Path(args.build_dir).resolve()
    engine = args.engine
    if engine == "auto":
        if which("OpenCppCoverage", [r"C:\Program Files\OpenCppCoverage"]):
            engine = "opencpp"
        elif all(llvm_tools()):
            engine = "clang"
        elif os.name != "nt":
            engine = "gcovr" if which("gcovr") else ("lcov" if which("lcov") else "gcov")
        else:
            engine = "gcovr"

    print("C23 COVERAGE: engine=%s build=%s" % (engine, bd))
    started = time.time()
    if not (bd / "CMakeCache.txt").is_file() or args.configure or args.reconfigure:
        if engine == "clang":
            ok, detail = configure_clang(str(bd), str(src), args.reconfigure)
        else:
            cmd = ["cmake", "-S", str(src), "-B", str(bd), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS=--coverage",
                   "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
                   "-DPRISM_BUILD_WIN_LAYER=OFF", "-DPRISM_WARNINGS_AS_ERRORS=ON"]
            rc, out = run(cmd, timeout=600)
            ok, detail = rc == 0, out[-800:]
        print("  configure: %s" % ("ok" if ok else "FAILED"))
        if not ok:
            print(detail)
            return 3
    if not args.no_build:
        rc, out = run(["cmake", "--build", str(bd), "--parallel", str(args.parallel)], timeout=3600)
        print("  build: %s" % ("ok" if rc == 0 else "FAILED"))
        if rc != 0:
            print(out[-1500:])
            return 3

    if engine == "clang":
        lcov, detail = engine_clang(bd, src, args)
    elif engine == "opencpp":
        lcov, detail = engine_opencpp(bd, src, args)
    elif engine in ("gcovr", "lcov", "gcov"):
        lcov, detail = engine_gcov(bd, src, args, engine)
    else:
        lcov, detail = None, "unknown engine"

    if lcov is None:
        print("C23 COVERAGE: SKIP -- %s" % detail)
        return 3

    summary = lcov.summary()
    if not summary:
        print("C23 COVERAGE: FAILED -- no Src/ file instrumented (wrong filter/build tree?)")
        return 3
    policy = load_policy(args.policy)
    md, alerts, unmeasured, watch = report_markdown(
        summary, policy,
        {"engine": engine, "build_dir": str(bd), "ctest": args.filter or "(all)",
         "ran": detail, "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")},
    )
    if args.markdown:
        p = Path(args.markdown)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(md, encoding="utf-8")
    if args.json:
        p = Path(args.json)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps({
            "engine": engine, "build_dir": str(bd), "detail": detail,
            "generated_by": "tools/cov/coverage.py",
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "modules": summary,
            "alerts": [m for m, _, _ in alerts],
            "unmeasured_critical": [m for m, _ in unmeasured],
        }, indent=2) + "\n", encoding="utf-8")
    print(md.split("## 覆盖率低但重要")[0].strip())
    print("")
    print("## 覆盖率低但重要（策略门限）")
    if alerts:
        for mod, s, pol in alerts:
            print("- %s: %.1f%% < %s%% -- %s"
                  % (mod, s["line_pct"], pol.get("min_line", 80), pol.get("note", "")))
    else:
        print("- 无")
    if unmeasured:
        print("未测量关键模块: " + ", ".join(m for m, _ in unmeasured))
    if watch:
        print("")
        print("低覆盖但重要（watchlist）：")
        for mod, s, pol in watch:
            print("- %s: %.1f%% (%d/%d), 未命中 %d 行"
                  % (mod, s["line_pct"], s["lines_covered"], s["lines_total"],
                     s["lines_total"] - s["lines_covered"]))
    print("")
    print("C23 COVERAGE: %s (%d modules, %.1fs)"
          % ("OK" if not alerts and not unmeasured else "ATTENTION", len(summary), time.time() - started))
    return 0


if __name__ == "__main__":
    sys.exit(main())
