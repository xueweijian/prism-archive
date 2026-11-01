#!/usr/bin/env python3
"""C23 -- automated negative-control round: inject, run, require RED, restore.

A mutant is a test binary compiled with one rule broken by a compile definition.
"Green" is never evidence on its own, so this harness decides with three
independent facts per mutant:

  1. INJECTION HAPPENED. The macro must appear in the build's own command line
     for the mutant target (ninja -t commands) AND/OR the binary must print its
     "MUTANT ACTIVE" banner. A misspelled macro is the classic silent no-op:
     the mutant then behaves exactly like the clean build. That case must be
     reported as MUTATION NOT IN EFFECT -- it is NEVER "caught" and never a pass.
  2. THE SUITE WENT RED. Exit code / [FAILED] lines for WILL_FAIL mutants; the
     conjunction marker ("... MUTANT CAUGHT") for the D26-safe marker form.
  3. THE CLEAN CONTROL IS GREEN. Running the unmutated target from the same tree
     proves the assertions are not permanently red (a permanently red suite
     would make every WILL_FAIL registration look "caught").

Modes
  probe  classify ONE binary (used by the C23 self-test with a deliberately
         misspelled macro: the verdict must be NOT-IN-EFFECT, not CAUGHT).
  run    walk real mutant tests from a build tree's ctest JSON.
  list   print the matrix discovered from the repo.

Exit codes: 0 = every expectation met, 1 = verdict problem, 2 = environment.
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
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import mutant_matrix  # noqa: E402

CAUGHT = "CAUGHT"
NOT_IN_EFFECT = "NOT-IN-EFFECT"
GREEN_HOLE = "GREEN-HOLE"
RED_NO_INJECTION = "RED-WITHOUT-INJECTION"
CLEAN = "CLEAN"
SKIPPED = "SKIPPED"
TIMEOUT = "TIMEOUT"
NOT_BUILT = "NOT-BUILT"
ERROR = "ERROR"
CONTROL_PASS = "CONTROL-PASS"
CONTROL_FAIL = "CONTROL-FAIL"

FAIL_VERDICTS = {NOT_IN_EFFECT, GREEN_HOLE, RED_NO_INJECTION, ERROR, TIMEOUT, CONTROL_FAIL}


def run_process(cmd, timeout, cwd=None, env=None, input_text=None):
    try:
        p = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            cwd=str(cwd) if cwd else None,
            env=env,
        )
        return p.returncode, (p.stdout or "") + (p.stderr or ""), False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return None, out, True
    except OSError as e:
        return None, "process failed to start: %s" % e, False


def injection_in_build(build_dir, target, macro):
    """D3: prove the macro reached the compiler command line for this target."""
    if not macro or not target:
        return None
    ninja = shutil.which("ninja")
    if not ninja:
        return None
    rc, out, _ = run_process([ninja, "-C", str(build_dir), "-t", "commands", target], 60)
    if rc != 0:
        return None
    # Exact token: a misspelled macro (..._PROBE_X) must NOT count as proof that
    # the real one (..._PROBE) was injected. Match the compiler flag prefix
    # explicitly (the character before the macro is 'D' of -D/ /D), then fall
    # back to a plain token boundary for other evidence.
    flag = re.search(r"(?:-D|/D)" + re.escape(macro) + r"(?![A-Za-z0-9_])", out)
    token = re.search(r"(?<![A-Za-z0-9_])" + re.escape(macro) + r"(?![A-Za-z0-9_])", out)
    return bool(flag or token)


def first_red(out):
    m = re.search(r"\[FAILED\]\s+(\S+)", out)
    if m:
        return m.group(1)
    m = re.search(r"^\s+FAIL\s+\S+:\d+\s+(.+)$", out, re.M)
    if m:
        return m.group(1).strip()[:120]
    return ""


def classify(rc, out, mode, banner, marker, injected=None):
    """Pure decision function -- the heart of D3. Returns (verdict, evidence).

    injected=True means the macro was found on the target's compile command line.
    That is independent proof of injection for mutants whose banner is computed
    at runtime (e.g. PrismCore/ThemeTokens exposes ActiveMutation() as a string).
    """
    banner_hit = bool(banner) and banner in out
    marker_hit = bool(marker) and re.search(marker, out) is not None
    proof = banner_hit or injected is True
    if mode == "marker":
        if marker_hit:
            return CAUGHT, "conjunction marker matched (D26-safe form)"
        if rc == 0:
            return NOT_IN_EFFECT, "marker absent and the mutant exited 0"
        return RED_NO_INJECTION, "red but the marker that proves injection never printed"
    if mode == "will-fail":
        if proof and rc not in (0, None):
            why = "banner present" if banner_hit else "macro present in build flags"
            return CAUGHT, "%s and exit code %s" % (why, rc)
        if proof and rc == 0:
            return GREEN_HOLE, "injection fired but the suite stayed green"
        if not proof and rc == 0:
            return NOT_IN_EFFECT, "no injection evidence and the suite stayed green"
        return RED_NO_INJECTION, "red without banner or build-flag proof of injection"
    # plain mutant: the only evidence is the exit code
    if rc not in (0, None):
        return CAUGHT, "exit code %s" % rc
    return NOT_IN_EFFECT, "plain mutant stayed green"


def probe(args):
    exe = Path(args.exe)
    if not exe.is_file():
        print("NEGCTL: ERROR -- executable not found: %s" % exe)
        return 2
    in_build = injection_in_build(args.build_dir, args.target, args.macro) if args.build_dir else None
    rc, out, timed = run_process([str(exe)], args.timeout)
    if timed:
        verdict, why = TIMEOUT, "exceeded %ss" % args.timeout
    else:
        verdict, why = classify(rc, out, args.mode, args.marker, args.pass_marker, in_build)
        if not args.macro and verdict == NOT_IN_EFFECT and rc == 0:
            # No mutation was requested: a green run with no banner IS the clean
            # control, not a dead injection.
            verdict, why = CLEAN, "no mutation requested and the control stayed green"
        if in_build is False and verdict in (CAUGHT,):
            # The binary printed a banner but the *current* build flags no longer
            # carry the macro: the artifact is stale, which is not evidence.
            verdict, why = NOT_IN_EFFECT, "macro absent from the build command line (stale artifact?)"
        elif in_build is False and verdict == CLEAN:
            verdict, why = NOT_IN_EFFECT, "macro absent from the build command line for %s" % args.target
    inj = "n/a" if in_build is None else ("in-build" if in_build else "ABSENT")
    print(
        "C23 NEGCTL PROBE: %s  exe=%s macro=%s injection=%s rc=%s"
        % (verdict, exe.name, args.macro or "-", inj, rc)
    )
    print("  evidence: %s" % why)
    if args.expect:
        ok = verdict.upper() == args.expect.upper()
        print("  expectation: %s -> %s" % (args.expect, "MET" if ok else "NOT MET"))
        return 0 if ok else 1
    return 0 if verdict in (CAUGHT, CLEAN) else 1


def ctest_tests(build_dir):
    rc, out, _ = run_process(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"], 120
    )
    if rc != 0:
        raise RuntimeError("ctest --show-only failed: %s" % out[-400:])
    data = json.loads(out)
    tests = {}
    for t in data.get("tests", []):
        props = {}
        for p in t.get("properties", []):
            props[p.get("name")] = p.get("value")
        tests[t.get("name")] = {"command": t.get("command", []), "props": props}
    return tests


def props_value(props, name):
    v = props.get(name)
    if isinstance(v, list):
        return v[0] if v else None
    return v


def test_env(props):
    env = dict(os.environ)
    raw = props.get("ENVIRONMENT")
    items = []
    if isinstance(raw, list):
        items = raw
    elif isinstance(raw, str):
        items = [p for p in raw.split(";") if p]
    for item in items:
        if "=" in item:
            k, v = item.split("=", 1)
            env[k] = v
    return env


def run_one(entry, tests, build_dir, do_build, timeout_default):
    name = entry["test"]
    if name not in tests:
        return {"test": name, "verdict": NOT_BUILT, "evidence": "not registered in this build tree"}
    t = tests[name]
    props = t["props"]
    command = t["command"]
    if not command:
        return {"test": name, "verdict": ERROR, "evidence": "ctest has no command"}
    timeout = int(props_value(props, "TIMEOUT") or timeout_default)
    env = test_env(props)
    cwd = Path(build_dir) / "Tests"
    if not cwd.is_dir():
        cwd = Path(build_dir)

    if entry.get("role") == "CONTROL":
        # A control must be GREEN and unarmed. "Caught" would be nonsense here:
        # it proves the opposite direction (no injection => no banner, checks pass).
        if do_build and entry["target"]:
            rc, out, timed = run_process(
                ["cmake", "--build", str(build_dir), "--target", entry["target"]], 1800
            )
            if timed or rc != 0:
                return {"test": name, "verdict": ERROR,
                        "evidence": "control build failed: %s" % out[-300:]}
        rc, out, timed = run_process(command, timeout, cwd=cwd, env=env)
        if timed:
            return {"test": name, "verdict": TIMEOUT,
                    "evidence": "exceeded %ss" % timeout, "rc": rc}
        marker = entry.get("pass_regex") or ""
        marker_hit = bool(marker) and re.search(marker, out) is not None
        armed = "MUTANT ACTIVE" in out
        ok = rc == 0 and (not marker or marker_hit) and not armed
        return {
            "test": name,
            "verdict": CONTROL_PASS if ok else CONTROL_FAIL,
            "evidence": "rc=%s marker=%s armed=%s" % (
                rc, "present" if marker_hit else ("missing" if marker else "n/a"),
                "yes" if armed else "no"),
            "injection": "n/a",
            "rc": rc,
            "first_red": "",
            "expected_hit": "",
            "mode": "CONTROL",
        }

    if do_build and entry["target"]:
        targets = [entry["target"]]
        if entry["clean_test"] and entry["clean_test"] in tests:
            clean_cmd = tests[entry["clean_test"]]["command"]
            if clean_cmd and clean_cmd[0] != entry["target"]:
                targets.append(clean_cmd[0])
        rc, out, timed = run_process(
            ["cmake", "--build", str(build_dir), "--target"] + targets, 1800
        )
        if timed or rc != 0:
            return {
                "test": name,
                "verdict": ERROR,
                "evidence": "build failed (inject/restore step): %s" % out[-300:],
            }

    mode = "marker" if (entry["mode"] == "MARKER") else (
        "will-fail" if entry["will_fail"] else "plain"
    )
    in_build = injection_in_build(build_dir, entry["target"], entry["macro"])
    rc, out, timed = run_process(command, timeout, cwd=cwd, env=env)
    if timed:
        return {"test": name, "verdict": TIMEOUT, "evidence": "exceeded %ss" % timeout, "rc": rc}
    skip_code = props_value(props, "SKIP_RETURN_CODE")
    if skip_code is not None and rc is not None and str(rc) == str(skip_code):
        return {
            "test": name,
            "verdict": SKIPPED,
            "evidence": "returned SKIP code %s (a visible skip, never a pass)" % skip_code,
            "rc": rc,
        }
    verdict, why = classify(rc, out, mode, entry["banner"], entry["pass_regex"], in_build)
    if verdict == CAUGHT and in_build is False:
        verdict, why = NOT_IN_EFFECT, "macro absent from the build command line for %s" % entry["target"]
    if entry["mode"] == "MARKER" and verdict == SKIPPED:
        verdict, why = SKIPPED, why
    red = first_red(out)
    expected = entry.get("expected_red", "")
    hit = ""
    if red and expected.startswith("test: "):
        hit = "matches expected" if red == expected[6:].strip() else "expected %r" % expected[6:]
    return {
        "test": name,
        "verdict": verdict,
        "evidence": why,
        "injection": "n/a" if in_build is None else ("in-build" if in_build else "ABSENT"),
        "rc": rc,
        "first_red": red,
        "expected_hit": hit,
        "mode": entry["mode"],
    }


def control_checks(entries, tests, build_dir, timeout_default):
    """Restore/control: the unmutated suite must be green. Same source, no macro."""
    seen = {}
    results = []
    for entry in entries:
        clean = entry.get("clean_test")
        if not clean or clean in seen or clean not in tests:
            continue
        seen[clean] = True
        t = tests[clean]
        timeout = int(props_value(t["props"], "TIMEOUT") or timeout_default)
        rc, out, timed = run_process(
            t["command"], timeout, cwd=Path(build_dir) / "Tests", env=test_env(t["props"])
        )
        marker = entry.get("pass_marker") or ""
        skip = props_value(t["props"], "SKIP_RETURN_CODE")
        if skip is not None and rc is not None and str(rc) == str(skip):
            results.append({
                "test": clean, "verdict": "CONTROL-SKIP",
                "evidence": "returned SKIP code %s (visible skip, not a pass)" % skip, "rc": rc,
            })
            continue
        ok = (not timed) and rc == 0 and (not marker or marker in out)
        results.append(
            {
                "test": clean,
                "verdict": "CONTROL-PASS" if ok else "CONTROL-FAIL",
                "evidence": "rc=%s marker=%s" % (rc, "present" if marker and marker in out else "missing"),
                "rc": rc,
            }
        )
    return results


def run_matrix(args):
    build_dir = Path(args.build_dir).resolve()
    if not (build_dir / "CTestTestfile.cmake").is_file():
        print("NEGCTL: ERROR -- %s is not a configured build tree" % build_dir)
        return 2
    matrix = mutant_matrix.build_matrix(Path(args.root).resolve()) if args.root else mutant_matrix.build_matrix(ROOT)
    entries = matrix["mutants"]
    if args.only:
        rx = re.compile(args.only)
        entries = [e for e in entries if rx.search(e["test"])]
    if not entries:
        print("NEGCTL: ERROR -- no mutant matched %r" % args.only)
        return 2

    tests = ctest_tests(build_dir)
    results = [run_one(e, tests, build_dir, args.build, args.timeout) for e in entries]
    controls = control_checks(entries, tests, build_dir, args.timeout) if args.control else []

    summary = {}
    for r in results:
        summary[r["verdict"]] = summary.get(r["verdict"], 0) + 1
    print("")
    print("== C23 negative-control round (%s, %d mutants) ==" % (build_dir.name, len(results)))
    for r in results:
        line = "  %-14s %s" % (r["verdict"], r["test"])
        if r.get("first_red"):
            line += "  [%s]" % r["first_red"]
        if r.get("injection") not in (None, "n/a"):
            line += "  injection=%s" % r["injection"]
        print(line)
        if r["verdict"] in FAIL_VERDICTS:
            print("      > %s" % r.get("evidence", ""))
    for c in controls:
        print("  %-14s %s (%s)" % (c["verdict"], c["test"], c["evidence"]))
    print("  summary: " + ", ".join("%s=%d" % kv for kv in sorted(summary.items())))

    payload = {
        "generated_by": "tools/mutation/negctl.py",
        "build_dir": str(build_dir),
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "results": results,
        "controls": controls,
        "summary": summary,
    }
    if args.json_out:
        p = Path(args.json_out)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("  json: %s" % p)

    bad = [r for r in results if r["verdict"] in FAIL_VERDICTS]
    bad += [c for c in controls if c["verdict"] == "CONTROL-FAIL"]
    if bad:
        print("C23 NEGCTL: FAILED (%d verdicts not acceptable)" % len(bad))
        return 1
    measured = [r for r in results if r["verdict"] == CAUGHT]
    print("C23 NEGCTL: OK (%d mutants caught, %d skipped-by-build)" % (len(measured), summary.get(NOT_BUILT, 0)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="C23 negative-control harness (CONTRACTS D3).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="classify one binary")
    p.add_argument("--exe", required=True)
    p.add_argument("--macro", default="")
    p.add_argument("--build-dir", default="")
    p.add_argument("--target", default="")
    p.add_argument("--mode", choices=["will-fail", "marker", "plain"], default="will-fail")
    p.add_argument("--marker", default="", help="banner that proves the injection compiled in")
    p.add_argument("--pass-marker", default="", help="marker for the marker-conjunction form")
    p.add_argument("--expect", default="", help="CAUGHT | NOT-IN-EFFECT | CLEAN ...")
    p.add_argument("--timeout", type=int, default=180)

    r = sub.add_parser("run", help="run real mutants from a build tree")
    r.add_argument("--build-dir", required=True)
    r.add_argument("--root", default="")
    r.add_argument("--only", default="")
    r.add_argument("--build", action="store_true", help="rebuild mutant + clean target first")
    r.add_argument("--control", action="store_true", default=True)
    r.add_argument("--no-control", dest="control", action="store_false")
    r.add_argument("--json-out", default="")
    r.add_argument("--timeout", type=int, default=300)

    l = sub.add_parser("list", help="print the discovered matrix")
    l.add_argument("--json-out", default="")

    args = ap.parse_args(argv)
    if args.cmd == "probe":
        return probe(args)
    if args.cmd == "run":
        return run_matrix(args)
    matrix = mutant_matrix.build_matrix(Path(args.root).resolve() if getattr(args, "root", "") else ROOT)
    print(mutant_matrix.markdown(matrix))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(matrix, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
