#!/usr/bin/env python3
"""Negative controls for the navigation module (CONTRACTS D3/D5).

For every mutation: (1) prove the substitution MATCHED in the source -- a
replace that matches nothing is a silent no-op and would fake a "sharp test"
verdict, which is exactly how the password-ring mutant lied; (2) rebuild and
run the suite; (3) require it to go RED.

Usage: python3 tools/mutate_tree.py
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "Src" / "PrismCore" / "ArchiveTreeView.hpp"
TEST = ROOT / "Tests" / "test_archive_tree.cpp"

MUTATIONS = [
    ("hit boundary becomes exclusive-left",
     "if (x >= span.left && x < span.right) return span.level;",
     "if (x > span.left && x < span.right) return span.level;",
     "breadcrumb geometry"),
    ("hit ignores clipping (span clamp removed)",
     "    span.left = (std::max)(span.left, clipLeft);\n"
     "    span.right = (std::min)(span.right, clipRight);",
     "    span.left = span.left;\n"
     "    span.right = span.right;",
     "breadcrumb clipping"),
    ("separators become clickable levels",
     "    segments.push_back({std::string(separator), kNoLevel});",
     "    segments.push_back({std::string(separator), static_cast<int>(i)});",
     "breadcrumb segments"),
    ("pseudo-name filter removed",
     "  return name == \".\" || name == \"..\";",
     "  return false;",
     "pseudo children"),
    ("digit runs compared lexically (naive sort)",
     "      if (lenA != lenB) return lenA < lenB ? -1 : 1;",
     "      if (lenA != lenB) return a.compare(startA, lenA, b, startB, lenB) < 0 ? -1 : 1;",
     "ordering follows 7-Zip"),
    ("Back at the root fabricates a path instead of doing nothing",
     "    if (folder_.empty()) return Unchanged();",
     "    if (folder_.empty()) return GoTo(\"Assets/Ghost/Deep\");",
     "Backspace at the root"),
    ("a no-op transition drops the live selection",
     "  NavResult Unchanged() const {\n    NavResult result;\n    result.selection = selection_;",
     "  NavResult Unchanged() const {\n    NavResult result;\n    result.selection = -1;",
     "Backspace at the root"),
    ("selection restore dropped",
     "    if (!childName.empty()) {",
     "    if (false && !childName.empty()) {",
     "drill into a folder"),
    ("synthetic folders steal a child index",
     "it == explicitDirs.end() ? kSyntheticIndex\n                                                         : it->second",
     "it == explicitDirs.end() ? item.index\n                                                         : it->second",
     "steal a child's index"),
    ("implicit folders no longer discovered",
     "      if (detail::IsPseudoName(childName)) continue;",
     "      if (true) continue;",
     "Assets/Icons"),
    ("file activation navigates instead of activating",
     "    if (!entry.isDir) {",
     "    if (false) {",
     "keeps the file action"),
    ("Back jumps to the root instead of one level",
     "    return GoTo(UpOne(folder_));",
     "    return GoTo(\"\");",
     "deeper nesting"),
    ("centred origin ignores the island box",
     "  int origin = boxLeft + (boxWidth - total) / 2;",
     "  int origin = boxLeft;",
     "centred in the island"),
    ("centred layout does not clip to the box",
     "  return LayoutBreadcrumb(segments, origin, width, boxLeft, (std::max)(boxLeft, boxRight));",
     "  return LayoutBreadcrumb(segments, origin, width);",
     "centred in the island"),
    ("Hostile paths keep their '..' segments",
     "  return prism::SanitizeRelativePath(folder);",
     "  return prism::NormalizeArchivePath(folder);",
     "hostile folder"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    original = HEADER.read_text(encoding="utf-8")
    failures = []
    for name, old, new, expect_case in MUTATIONS:
        if original.count(old) != 1:
            failures.append(f"INJECTION DID NOT LAND ({original.count(old)} matches): {name}")
            continue
        mutated = original.replace(old, new)
        assert mutated != original, name
        # Prove the injected text is really in the mutated source (double check
        # the replacement, not just the search string).
        assert new in mutated, name
        with tempfile.TemporaryDirectory() as tmp:
            tmp_header = Path(tmp) / "ArchiveTreeView.hpp"
            tmp_header.write_text(mutated, encoding="utf-8")
            # Point the test at the mutated header by shadowing the include dir.
            src = TEST.read_text(encoding="utf-8").replace(
                '"../Src/PrismCore/ArchiveTreeView.hpp"', f'"{tmp_header.as_posix()}"')
            probe = Path(tmp) / "mut.cpp"
            probe.write_text(src, encoding="utf-8")
            exe = Path(tmp) / "mut"
            build = run(["g++", "-std=c++20", "-w", "-I", str(ROOT / "Src" / "PrismCore"),
                         "-I", str(ROOT / "Tests"), "-o", str(exe), str(probe)])
            if build.returncode != 0:
                failures.append(f"DID NOT COMPILE: {name}: {build.stderr.splitlines()[:3]}")
                continue
            result = run([str(exe)])
            red = result.returncode != 0
            if red:
                caught = [l.strip() for l in result.stdout.splitlines() if l.strip().startswith("FAIL")]
                hit_expect = any(expect_case in l for l in result.stdout.splitlines() if "[FAILED]" in l)
                print(f"  RED   {name}")
                print(f"        first assertion: {caught[0] if caught else '(none)'}")
                if not hit_expect:
                    print(f"        NOTE: expected case containing '{expect_case}' did not fail")
            else:
                failures.append(f"STILL GREEN WITH BUG: {name}")
                print(f"  GREEN(hole) {name}")
    if failures:
        print("\nMUTATION GATE: FAILED")
        for f in failures:
            print("  " + f)
        return 1
    print(f"\nMUTATION GATE: PASSED ({len(MUTATIONS)} bugs injected, all caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
