#!/usr/bin/env python3
"""C23 -- mutation matrix: enumerate and validate every negative control.

Each mutant is the same test source compiled with one rule switched off by a
compile definition (PRISM_*_MUTANT_*). They live in 16 suite files plus the
legacy block in Tests/CMakeLists.txt. This tool rebuilds the wires:

  test name -> target -> source -> injection macro -> clean control -> marker.

It also enforces the CTest-property contracts (CONTRACTS D26):
  * WILL_FAIL + PASS_REGULAR_EXPRESSION on one test = always red;
  * WILL_FAIL + SKIP_RETURN_CODE = a SKIP turned into a fake green (D1).

Usage:
  python tools/mutation/mutant_matrix.py --check
  python tools/mutation/mutant_matrix.py --json out/matrix.json
  python tools/mutation/mutant_matrix.py --markdown out/matrix.md --results out/negctl.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
COMMAND_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def strip_cmake_comments(text):
    out = []
    i = 0
    n = len(text)
    quote = None
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c == '"':
            quote = c
            out.append(c)
        elif c == "#":
            j = text.find("\n", i)
            if j < 0:
                break
            i = j
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def parse_commands(text):
    cmds = []
    i = 0
    n = len(text)
    while True:
        m = COMMAND_RE.search(text, i)
        if not m:
            break
        j = m.end()
        depth = 1
        quote = None
        while j < n and depth:
            c = text[j]
            if quote:
                if c == "\\":
                    j += 2
                    continue
                if c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        cmds.append((m.group(1), text[m.end() : j - 1]))
        i = j
    return cmds


def split_args(s):
    args, cur, depth, quote, i, n = [], [], 0, None, 0, len(s)
    while i < n:
        c = s[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < n:
                cur.append(s[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c == "(":
            depth += 1
            cur.append(c)
        elif c == ")":
            depth -= 1
            cur.append(c)
        elif c.isspace() and depth == 0:
            if cur:
                args.append("".join(cur))
                cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        args.append("".join(cur))
    return args


def unquote(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'":
        return s[1:-1]
    return s


def expand(body, env, depth=0):
    # Values can reference other values (set(mutant_exe "...${mutant_lc}")), so
    # substitute until stable instead of one pass.
    if depth > 5:
        return body
    new = re.sub(
        r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}",
        lambda m: env.get(m.group(1), m.group(0)),
        body,
    )
    return new if new == body else expand(new, env, depth + 1)


def eval_commands(cmds, env=None, out=None):
    env = env if env is not None else {}
    out = out if out is not None else []
    i = 0
    while i < len(cmds):
        name, body = cmds[i]
        if name == "set":
            args = split_args(body)
            if len(args) >= 2:
                env[args[0]] = " ".join(unquote(a) for a in args[1:])
            i += 1
            continue
        if name == "string":
            args = [unquote(a) for a in split_args(body)]
            if len(args) >= 5 and args[0].upper() == "REPLACE":
                # string(REPLACE <match> <replace> <out-var> <input>)
                before, after, outvar, value = args[1], args[2], args[3], args[4]
                env[outvar] = expand(value, env).replace(expand(before, env), expand(after, env))
            i += 1
            continue
        if name == "foreach":
            m = re.match(r"\s*([A-Za-z_]\w*)\s+IN\s+(ITEMS|LISTS)\s+(.*)", body, re.S)
            if not m:
                i += 1
                continue
            var = m.group(1)
            items = [unquote(a) for a in split_args(m.group(3))]
            depth = 1
            k = i + 1
            while k < len(cmds) and depth:
                depth += 1 if cmds[k][0] == "foreach" else (-1 if cmds[k][0] == "endforeach" else 0)
                k += 1
            for item in items:
                e = dict(env)
                e[var] = item
                e[var + "_lc"] = item.lower()
                eval_commands(cmds[i + 1 : k - 1], e, out)
            i = k
            continue
        out.append((name, expand(body, env)))
        i += 1
    return out


def load_events(root):
    files = [root / "Tests" / "CMakeLists.txt"]
    suites = root / "Tests" / "suites"
    if suites.is_dir():
        files += sorted(suites.glob("*.cmake"))
    events = []
    for f in files:
        if f.is_file():
            for name, body in eval_commands(
                parse_commands(strip_cmake_comments(f.read_text(encoding="utf-8")))
            ):
                events.append((f, name, body))
    return events


def parse_model(root):
    model = {"targets": {}, "tests": {}, "props": {}, "order": [], "links": {}}

    def target(name):
        return model["targets"].setdefault(name, {"sources": [], "defs": [], "wx_minus": False})

    for f, name, body in load_events(root):
        args = [unquote(a) for a in split_args(body)]
        if not args:
            continue
        if name == "add_executable" and len(args) >= 2:
            target(args[0])["sources"] = [
                a for a in args[1:] if a not in ("WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL")
            ]
        elif name == "target_compile_definitions" and len(args) >= 2:
            target(args[0])["defs"].extend(
                a for a in args[1:] if a not in ("PRIVATE", "PUBLIC", "INTERFACE")
            )
        elif name == "target_link_libraries" and len(args) >= 2:
            model["links"].setdefault(args[0], []).extend(
                a for a in args[1:] if a not in ("PRIVATE", "PUBLIC", "INTERFACE")
            )
        elif name == "target_compile_options" and args and "/WX-" in args:
            target(args[0])["wx_minus"] = True
        elif name == "add_test" and len(args) >= 2 and args[0] == "NAME":
            test = args[1]
            model["tests"][test] = {
                "command": args[args.index("COMMAND") + 1 :] if "COMMAND" in args else [],
                "file": f,
            }
            model["order"].append(test)
        elif name == "set_tests_properties" and "PROPERTIES" in args:
            idx = args.index("PROPERTIES")
            for i in range(0, len(args) - idx - 2, 2):
                for t in args[:idx]:
                    model["props"].setdefault(t, {})[args[idx + 1 + i]] = args[idx + 2 + i]
    return model


def rel(root, p):
    try:
        return str(p.resolve().relative_to(root.resolve())).replace("\\", "/")
    except Exception:
        return str(p).replace("\\", "/")


def resolve_source(root, source):
    """add_executable() sources are relative to Tests/, not to the repo root."""
    if not source:
        return None
    for base in (root / "Tests", root):
        p = base / source
        if p.is_file():
            return p
    return None


def target_defs(model, target, depth=0):
    """The mutant macro may sit on a linked static library (C4 shell contract)."""
    if depth > 2 or target not in model["targets"]:
        return []
    defs = [d for d in model["targets"][target]["defs"] if "_MUTANT" in d]
    if defs:
        return defs
    for lib in model["links"].get(target, []):
        defs += target_defs(model, lib, depth + 1)
    return defs


def find_banner(text, macro):
    if not text or not macro:
        return None
    pats = [
        r"#\s*if\s+defined\s*\(\s*" + re.escape(macro) + r"\s*\)",
        r"#\s*ifdef\s+" + re.escape(macro) + r"\b",
        r"#\s*if\s+" + re.escape(macro) + r"\b",
    ]
    for pat in pats:
        for m in re.finditer(pat, text):
            lit = re.search(r'"([^"\n]*MUTANT ACTIVE[^"\n]*)"', text[m.end() : m.end() + 800])
            if lit:
                return lit.group(1)
    for m in re.finditer(re.escape(macro), text):
        lit = re.search(r'"([^"\n]*MUTANT ACTIVE[^"\n]*)"', text[m.end() : m.end() + 400])
        if lit:
            return lit.group(1)
    return None


def find_expected_red(text, macro):
    if not text or not macro:
        return None
    head = "\n".join(text.splitlines()[:80])
    m = re.search(r"^\s*//\s*" + re.escape(macro) + r"\s*(?:->|=>|:)\s*(.+)$", head, re.M)
    if m:
        return m.group(1).strip()
    # No reliable per-mutant PRISM_TEST mapping: the macro may be consumed in a
    # product header, so the source cannot say which assertion goes red. The
    # measured first red (negctl --json-out, folded in with --results) fills it.
    return None


# A control entry proves the ABSENCE of an injection: C22 registers the clean
# binary itself as "mutation.fuzz.clean_build_unarmed" so that "no banner when
# nothing is injected" is asserted, not assumed. Such an entry MUST be allowed
# to have no macro -- and must never carry one (a control that is also a mutant
# is neither).
CONTROL_RE = re.compile(r"(?:^|[._-])(clean|unarmed|control)(?:$|[._-])", re.I)


def is_control_entry(test, target):
    return bool(CONTROL_RE.search(test) or (target and CONTROL_RE.search(target)))


def build_matrix(root, results=None):
    model = parse_model(root)
    mutants, problems, warnings = [], [], []

    for test in model["order"]:
        if not test.startswith("mutation."):
            continue
        command = model["tests"][test]["command"]
        props = model["props"].get(test, {})
        target = command[0] if command else ""
        macro = source = runtime = None
        control = is_control_entry(test, target)

        source_path = None
        if target in model["targets"]:
            defs = target_defs(model, target)
            if len(defs) == 1:
                macro = defs[0].split("=")[0]
            elif defs:
                suffix = re.sub(r"^.*_mutant_", "", target)
                pick = [d for d in defs if suffix.upper() in d.upper()]
                macro = (pick[0] if pick else defs[0]).split("=")[0]
            sources = model["targets"][target]["sources"]
            source = sources[0] if sources else None
            source_path = resolve_source(root, source)
            if control:
                # Allowed to be unarmed. Armed => contradiction, which is a
                # structural problem, not a "mutant without a clean control".
                if macro:
                    problems.append(
                        "%s: control entry carries injection macro %s -- "
                        "a control must stay unarmed" % (test, macro)
                    )
            elif not macro:
                problems.append("%s: no injection macro on target %s" % (test, target))
            if source and not source_path:
                problems.append("%s: source missing: %s" % (test, source))
            if source and source.startswith("win/") and not model["targets"][target]["wx_minus"]:
                warnings.append("%s: Windows target without /WX-" % target)
        else:
            try:
                runtime = "-Mutate " + command[command.index("-Mutate") + 1]
            except (ValueError, IndexError):
                problems.append("%s: command is neither a target nor -Mutate" % test)
            source = next((a for a in command if a.endswith((".ps1", ".py"))), None)
            source_path = resolve_source(root, source)

        # A test name that still contains a CMake variable never registered
        # under the name the suite author intended (C19's string(REPLACE) used to
        # land here) -- that is a wiring bug, not a runtime mutator.
        if "${" in test:
            problems.append("%s: unexpanded CMake variable in the test name" % test)

        will_fail = str(props.get("WILL_FAIL", "")).upper() == "TRUE"
        pass_regex = props.get("PASS_REGULAR_EXPRESSION")
        skip_code = props.get("SKIP_RETURN_CODE")
        if will_fail and pass_regex:
            problems.append("%s: D26 -- WILL_FAIL + PASS_REGULAR_EXPRESSION is always RED" % test)
        if will_fail and skip_code:
            problems.append("%s: D26/D1 -- WILL_FAIL turns SKIP %s into fake green" % (test, skip_code))

        mode = "MARKER" if (pass_regex and not will_fail) else ("WILL_FAIL" if will_fail else "PLAIN")
        if control and will_fail:
            problems.append("%s: control registered WILL_FAIL (a control must be green)" % test)
        clean = None
        if not control and source_path:
            for t in model["order"]:
                if t.startswith("mutation."):
                    continue
                cmd = model["tests"][t]["command"]
                if cmd and cmd[0] in model["targets"]:
                    other = model["targets"][cmd[0]]["sources"]
                    if other and resolve_source(root, other[0]) == source_path:
                        clean = t
                        break
        if not control and not clean:
            warnings.append("%s: no clean control for target %s" % (test, target))
        pass_marker = model["props"].get(clean, {}).get("PASS_REGULAR_EXPRESSION") if clean else None

        text = source_path.read_text(encoding="utf-8", errors="replace") if source_path else None
        banner = find_banner(text, macro) if (text and macro) else None
        expected = find_expected_red(text, macro) if (text and macro) else None
        if not control and mode == "WILL_FAIL" and not banner:
            warnings.append("%s: no MUTANT ACTIVE banner (weaker injection proof)" % test)

        mutants.append({
            "test": test,
            "target": target if target in model["targets"] else "",
            "source": rel(root, source_path) if source_path else "",
            "macro": macro or "",
            "runtime_injection": runtime or "",
            "role": "CONTROL" if control else "MUTANT",
            "paired_with": [],
            "mode": mode,
            "will_fail": will_fail,
            "pass_regex": pass_regex or "",
            "skip_return_code": skip_code or "",
            "clean_test": clean or "",
            "pass_marker": pass_marker or "",
            "banner": banner or "",
            "expected_red": expected or "",
            "suite_file": rel(root, model["tests"][test]["file"]),
            "verdict": "",
            "first_red": "",
        })

    if results:
        by_test = {r.get("test"): r for r in results.get("results", [])}
        for m in mutants:
            hit = by_test.get(m["test"])
            if hit:
                m["verdict"] = hit.get("verdict", "")
                m["first_red"] = hit.get("first_red", "")

    # Pair each control with the mutants that share its source file. A control
    # with no mutant counterpart is still legal, but it proves nothing about a
    # mutation, so say so out loud.
    for c in [m for m in mutants if m["role"] == "CONTROL"]:
        c["paired_with"] = sorted(
            m["test"] for m in mutants
            if m["role"] == "MUTANT" and c["source"] and m["source"] == c["source"]
        )
        if c["source"] and not c["paired_with"]:
            warnings.append("%s: control shares no source with any mutant" % c["test"])

    mutants.sort(key=lambda m: (m["suite_file"], m["test"]))
    controls = [m for m in mutants if m["role"] == "CONTROL"]
    return {
        "root": str(root),
        "generated_by": "tools/mutation/mutant_matrix.py",
        "count": len(mutants),
        "mutants_count": len(mutants) - len(controls),
        "controls_count": len(controls),
        "files": sorted({m["suite_file"] for m in mutants}),
        "mutants": mutants,
        "problems": problems,
        "warnings": warnings,
    }


def markdown(matrix):
    lines = ["| 测试 | 注入宏 / 注入点 | 预期红断言 | 注册方式 | 状态 |", "|---|---|---|---|---|"]
    for m in matrix["mutants"]:
        inject = m["macro"] or m["runtime_injection"] or (
            "(none: control)" if m["role"] == "CONTROL" else "")
        red = m["expected_red"] or (
            ("实测首红: `%s`" % m["first_red"]) if m.get("first_red") else "—"
        )
        red = red.replace("|", "\\|")
        reg = "CONTROL(no injection)" if m["role"] == "CONTROL" else m["mode"]
        lines.append(
            "| `%s` | `%s` | %s | %s | %s |"
            % (m["test"], inject, red, reg, m["verdict"] or "not-run")
        )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# selftest -- calibrate the RULES themselves on a synthetic tree (D4). The
# control rule is the one that just produced a false positive on C22, so it
# gets three cases: legal control, control that is secretly armed (must be
# rejected), and a plain mutant with no macro (must stay rejected).
# ---------------------------------------------------------------------------
SELFTEST_MUTANT = """add_executable(test_mut demo.cpp)
target_compile_definitions(test_mut PRIVATE PRISM_DEMO_MUTANT_ON=1)
add_test(NAME mutation.demo.injected COMMAND test_mut)
set_tests_properties(mutation.demo.injected PROPERTIES WILL_FAIL TRUE)"""

SELFTEST_CLEAN = """add_executable(test_clean demo.cpp)
add_test(NAME mutation.demo.clean_build_unarmed COMMAND test_clean)
set_tests_properties(mutation.demo.clean_build_unarmed PROPERTIES PASS_REGULAR_EXPRESSION "ALL DEMO CHECKS PASSED")"""

SELFTEST_ARMED_CONTROL = """add_executable(test_sneaky demo.cpp)
target_compile_definitions(test_sneaky PRIVATE PRISM_DEMO_MUTANT_SNEAK=1)
add_test(NAME mutation.demo.control_but_injected COMMAND test_sneaky)
set_tests_properties(mutation.demo.control_but_injected PROPERTIES PASS_REGULAR_EXPRESSION "ALL DEMO CHECKS PASSED")"""

SELFTEST_ORPHAN = """add_executable(test_orphan demo.cpp)
add_test(NAME mutation.demo.orphan COMMAND test_orphan)"""

SELFTEST_MUTANT_VARS = """add_executable(test_vars demo.cpp)
target_compile_definitions(test_vars PRIVATE PRISM_DEMO_MUTANT_VARS=1)
set(demo_exe "test_vars")
string(REPLACE "vars" "vars_id" demo_id "vars")
add_test(NAME "mutation.demo.${demo_id}" COMMAND "${demo_exe}")
set_tests_properties("mutation.demo.${demo_id}" PROPERTIES WILL_FAIL TRUE)"""



def _selftest_matrix(blocks):
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "Tests").mkdir()
        (root / "Tests" / "demo.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        (root / "Tests" / "CMakeLists.txt").write_text(
            "\n\n".join(blocks) + "\n", encoding="utf-8"
        )
        return build_matrix(root)


def selftest():
    legal = _selftest_matrix([SELFTEST_MUTANT, SELFTEST_CLEAN])
    armed = _selftest_matrix([SELFTEST_MUTANT, SELFTEST_ARMED_CONTROL])
    orphan = _selftest_matrix([SELFTEST_MUTANT, SELFTEST_ORPHAN])
    variables = _selftest_matrix([SELFTEST_MUTANT_VARS, SELFTEST_CLEAN])

    ctrl = next((m for m in legal["mutants"] if m["role"] == "CONTROL"), None)
    ok = True
    if legal["problems"]:
        ok = False
    if not ctrl or ctrl["paired_with"] != ["mutation.demo.injected"]:
        ok = False
    if len(armed["problems"]) != 1 or "carries injection macro" not in armed["problems"][0]:
        ok = False
    if any("no injection macro" in p for p in armed["problems"]):
        ok = False
    if len(orphan["problems"]) != 1 or "no injection macro" not in orphan["problems"][0]:
        ok = False
    # set()/string(REPLACE) indirection must resolve to a real test name, not
    # leave a literal CMake variable that silently never registers.
    if variables["problems"] or not any(
        m["test"] == "mutation.demo.vars_id" for m in variables["mutants"]
    ):
        ok = False

    print("C23 MUTATION MATRIX SELFTEST: %s (clean control allowed+paired, "
          "armed control rejected, unarmed mutant rejected)"
          % ("OK" if ok else "FAILED"))
    if not ok:
        for label, mat in (("legal", legal), ("armed-control", armed), ("orphan", orphan)):
            for p in mat["problems"]:
                print("  %s PROBLEM: %s" % (label, p))
        print("  paired_with: %s" % (ctrl["paired_with"] if ctrl else None))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Enumerate/validate the negative-control matrix (C23).")
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--results")
    ap.add_argument("--selftest", action="store_true",
                    help="calibrate the matrix rules on a synthetic tree (D4)")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()

    root = Path(args.root).resolve()
    results = json.loads(Path(args.results).read_text(encoding="utf-8")) if args.results else None
    matrix = build_matrix(root, results)

    if args.json:
        p = Path(args.json)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(matrix, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.markdown:
        p = Path(args.markdown)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(markdown(matrix) + "\n", encoding="utf-8")
    if not (args.json or args.markdown or args.check):
        print(markdown(matrix))

    if args.check:
        for w in matrix["warnings"]:
            print("WARN  " + w)
        for pr in matrix["problems"]:
            print("PROBLEM  " + pr)
        if matrix["problems"]:
            print("C23 MUTATION MATRIX: FAILED (%d problems)" % len(matrix["problems"]))
            return 1
        print(
            "C23 MUTATION MATRIX: OK (%d entries = %d mutants + %d controls across "
            "%d suite files, 0 problems)"
            % (matrix["count"], matrix["mutants_count"], matrix["controls_count"],
               len(matrix["files"]))
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
