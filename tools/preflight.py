#!/usr/bin/env python3
"""Preflight gate: intercept the repeat CI failure classes LOCALLY, in seconds.

Evidence (2026-10-31 session): 30/60 CI runs red; ~half were repeat classes --
unparenthesized std::min/max under the windows.h macro, transitive <cstdint>,
CFSTR_* TEXT macros in non-UNICODE builds, enums without operator<<. Each cost
a full CI round. Every check here maps to a CONTRACTS.md entry; the ID is in
the failure message so the ledger stays the single source of truth.

Checker discipline (v2): code scanning strips comments FIRST (prose mentions of
CFSTR_/<windows.h> in explanatory comments are not violations), and B6 detects
top-level CHECK_EQ arguments by paren depth, not substring -- a nested call
argument never gets printed, so it never needs operator<<.

Usage: python3 tools/preflight.py        (repo root, before every push)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "Src" / "PrismCore"
WIN = ROOT / "Src" / "PrismWin"
TESTS = ROOT / "Tests"
failures = []


def scan(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def code_lines(text: str):
    """Yield (lineno, line) with // line-comments stripped. Block comments are
    rare in this codebase's executable lines; // dominates our style."""
    for i, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        yield i, code


def top_level_args(call_text: str):
    """Split the inside of PRISM_CHECK_EQ(...) into top-level comma arguments."""
    args, depth, cur = [], 0, []
    for ch in call_text:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        args.append("".join(cur).strip())
    return args


def check_b1_macro_parens():
    """B1: PrismCore must parenthesize std::min/max/clamp -- a consumer TU may
    include windows.h without NOMINMAX and the macro eats the call (C2589)."""
    for f in sorted(CORE.glob("*.hpp")):
        for i, code in code_lines(scan(f)):
            # NOTE: std::max uses colons. The parenthesized form (std::max)(...)
            # cannot match std::(min|max|clamp)\( because a ')' follows 'max'.
            for m in re.finditer(r"(?<![\w.])std::(min|max|clamp)\s*\(", code):
                failures.append(f"B1 {f.relative_to(ROOT)}:{i} unparenthesized std::{m.group(1)} "
                                f"(write (std::max)(...))")


def check_arch_no_windows():
    """Architecture rule: PrismCore is pure C++20, never includes <windows.h>.
    (Prose mentions of windows.h in comments are fine -- that IS the rule text.)"""
    for f in sorted(CORE.glob("*.hpp")):
        for i, code in code_lines(scan(f)):
            if re.search(r"#\s*include\s*<windows\.h>", code):
                failures.append(f"ARCH {f.relative_to(ROOT)}:{i} <windows.h> in PrismCore")


def check_b2_fixed_width():
    """B2: headers using u?intN_t must include <cstdint> themselves (the local
    libstdc++ pulls it transitively; CI's does not)."""
    for f in sorted(CORE.glob("*.hpp")):
        text = scan(f)
        if re.search(r"\bu?int(8|16|32|64)_t\b", text) and "#include <cstdint>" not in text:
            failures.append(f"B2 {f.relative_to(ROOT)} uses fixed-width ints without "
                            f"#include <cstdint>")


def check_b7_app_qualified_statics():
    """B7: Src/App glue has no local MSVC compile -- the class-static
    references that bit us twice (GalleryWindow::NowMs / ::kProgressMessage)
    must be qualified or backed by a using-alias in the same TU."""
    known = ("GalleryWindow",)
    for f in sorted((ROOT / "Src" / "App").glob("*.cpp")):
        text = scan(f)
        has_alias = any(
            re.search(rf"using\s+{name}\s*=\s*prism", text) for name in known
        )
        for name in known:
            bare = re.search(rf"(?<![\w:]){name}::", text)
            if bare and not has_alias:
                failures.append(
                    f"B7 {f.relative_to(ROOT)} references {name}:: without a "
                    f"using-alias (C2653 on MSVC; the class lives in prism::win::gallery)")


def check_b5_cfstr():
    """B5: CFSTR_* is TEXT-based -- it expands to a NARROW string in this
    project's non-UNICODE build, failing RegisterClipboardFormatW. Clipboard
    format names must be explicit wide literals in code (comments are fine)."""
    for f in sorted(list(WIN.glob("*.hpp")) + list(WIN.glob("*.cpp"))):
        for i, code in code_lines(scan(f)):
            if "CFSTR_" in code:
                failures.append(f"B5 {f.relative_to(ROOT)}:{i} CFSTR_* expands narrow in "
                                f"non-UNICODE builds -- use explicit L\"...\" format name")


def check_b6_enum_print():
    """B6: an enum CONSTANT passed as a TOP-LEVEL PRISM_CHECK_EQ argument gets
    printed on failure, so its type needs operator<< in the header. An enum
    nested inside a call (e.g. Budget(SomeEnum::X)) evaluates to something else
    and is never printed -- flagging it would be a false positive."""
    # enum name -> defining header
    enums = {}
    for f in sorted(CORE.glob("*.hpp")):
        for enum in re.findall(r"enum class (\w+)", scan(f)):
            enums.setdefault(enum, f)
    for t in sorted(TESTS.rglob("test_*.cpp")):
        text = scan(t)
        for m in re.finditer(r"PRISM_CHECK_EQ\(", text):
            start = m.end()
            depth = 1
            i = start
            while i < len(text) and depth > 0:
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                i += 1
            inner = text[start: i - 1]
            for arg in top_level_args(inner):
                am = re.search(r"(\w+)::(\w+)\s*$", arg)
                if am and am.group(1) in enums:
                    header = enums[am.group(1)]
                    if "operator<<" not in scan(header):
                        failures.append(f"B6 {t.relative_to(ROOT)}: enum {am.group(1)} "
                                        f"({header.relative_to(ROOT)}) is a top-level "
                                        f"PRISM_CHECK_EQ argument but has no operator<<")


def exe_source_map():
    """Map every test executable to the sources it compiles (from add_executable)."""
    mapping = {}
    cmake = scan(ROOT / "Tests" / "CMakeLists.txt")
    for m in re.finditer(r"add_executable\((\w+)\s+([^)]+)\)", cmake):
        exe, sources = m.group(1), m.group(2)
        for src in re.findall(r"[\w/.]+\.cpp", sources):
            mapping.setdefault(Path(src).name, exe)
    return mapping


def check_d1_skip_gates():
    """D1: any Windows test that can SKIP must have its stdout marker REQUIRED
    via PASS_REGULAR_EXPRESSION, or a silent skip counts as a pass."""
    cmake = scan(ROOT / "Tests" / "CMakeLists.txt")
    src_to_exe = exe_source_map()
    for f in sorted((TESTS / "win").glob("test_*.cpp")):
        if "SKIP" not in scan(f):
            continue
        exe = src_to_exe.get(f.name)
        if not exe:
            failures.append(f"D1 {f.relative_to(ROOT)} can SKIP but is not in any "
                            f"add_executable")
            continue
        m = re.search(rf'add_test\(NAME "([^"]+)" COMMAND {exe}\)', cmake)
        if not m:
            failures.append(f"D1 {f.relative_to(ROOT)} can SKIP but has no add_test "
                            f"registration (exe {exe})")
            continue
        tail = cmake[m.end(): m.end() + 400]
        if "PASS_REGULAR_EXPRESSION" not in tail:
            failures.append(f"D1 test '{m.group(1)}' can SKIP but PASS_REGULAR_EXPRESSION "
                            f"is not pinned -- a silent skip would count as a pass")


def main():
    check_b1_macro_parens()
    check_arch_no_windows()
    check_b2_fixed_width()
    check_b5_cfstr()
    check_b7_app_qualified_statics()
    check_b6_enum_print()
    check_d1_skip_gates()
    if failures:
        print("PREFLIGHT: FAILED")
        for f in failures:
            print("  " + f)
        print("\nEach ID maps to docs/CONTRACTS.md -- fix the code or amend the ledger.")
        return 1
    print("PREFLIGHT: PASSED (B1 parens / ARCH windows.h / B2 cstdint / B5 CFSTR / "
          "B6 enum print / D1 skip gates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
