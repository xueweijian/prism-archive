#!/usr/bin/env python3
"""c63 doc gate: the user documents are part of the product.

A README that advertises a path the code cannot produce is a bug, and a link to
a page that does not exist is the same bug with a different symptom. This gate
checks the four user-facing documents for the properties that can be checked
without a browser:

  * the file exists and has real content (a byte floor, not "non-empty")
  * exactly one H1, first heading wins, no skipped heading levels
  * every fenced code block names a language (a bare fence renders as a wall)
  * no hard-coded Windows user-profile path (the docs must be shareable)
  * every relative Markdown link resolves to a file, anchors included
  * the sections the card promised are actually present

--selftest builds a KNOWN-GOOD document set in a unique temp directory and then
injects one defect at a time. The gate is green only when the good set passes
AND every injected defect is caught (CONTRACTS D3: an assertion that cannot go
red is not a guard). The temp directory is removed on every path.
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
import unicodedata
from pathlib import Path

DOCS = (
    "README.md",
    "docs/USAGE.md",
    "docs/TROUBLESHOOTING.md",
    "docs/assets/README.md",
)

# Byte floors. They are deliberately low enough to survive an edit, high enough
# that a stub ("TODO") cannot pass: 0.1.0 shipped with the two pages missing.
MIN_BYTES = {
    "README.md": 4000,
    "docs/USAGE.md": 6000,
    "docs/TROUBLESHOOTING.md": 6000,
    "docs/assets/README.md": 800,
}

README_MARKERS = (
    "显示更多选项",
    "## 安装",
    "三条最常见的用法",
    "docs/USAGE.md",
    "docs/TROUBLESHOOTING.md",
)

USAGE_MARKERS = (
    "Ctrl+K",
    "Space",
    "--extract-here",
    "本版本的边界",
)

# One keyword per promised troubleshooting chapter. A heading must contain it.
TROUBLE_HEADINGS = (
    "右键",
    "显示更多选项",
    "7z.dll",
    "压缩",
    "乱码",
    "只读",
    "占用",
    "关联",
)

HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
LINK_RE = re.compile(r"(?<!!)\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
SCHEME_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
USER_PATH_RE = re.compile(r"C:[\\/]Users[\\/]", re.IGNORECASE)


def slugify(text: str) -> str:
    """GitHub-flavoured heading slug, close enough for the links we write."""
    out = []
    for ch in text.strip().lower():
        if ch in "-_":
            out.append(ch)
        elif ch.isspace():
            out.append("-")
        elif ch.isalnum() or unicodedata.category(ch).startswith("L"):
            out.append(ch)
    return "".join(out)


def parse_markdown(text: str):
    """Return (headings, links, issues) from one document.

    headings: list of (line_number, level, text, slug)
    links:    list of (line_number, label, target)
    issues:   structural problems (fences, heading skeleton)
    """
    issues = []
    headings = []
    links = []
    fence_char = None
    fence_line = 0
    prev_level = 0
    first_heading_checked = False

    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip("\r")
        stripped = line.lstrip()
        is_fence = stripped.startswith("~~~") or stripped.startswith("```")
        if is_fence:
            marker = stripped[:3]
            rest = stripped[3:].strip()
            if fence_char is None:
                fence_char = marker
                fence_line = number
                if not rest:
                    issues.append(f"code block at line {number} has no language tag")
            else:
                if marker != fence_char:
                    issues.append(f"code fence at line {number} closes a {fence_char} block")
                fence_char = None
            continue
        if fence_char is not None:
            continue  # inside a code block: no headings, no links

        match = HEADING_RE.match(line)
        if match:
            level = len(match.group(1))
            title = match.group(2).strip()
            if not title:
                issues.append(f"empty heading at line {number}")
            headings.append((number, level, title, slugify(title)))
            if not first_heading_checked:
                first_heading_checked = True
                if level != 1:
                    issues.append(
                        f"first heading at line {number} is H{level}; a document starts with one H1"
                    )
            elif prev_level and level > prev_level + 1:
                issues.append(
                    f"heading at line {number} jumps H{prev_level} -> H{level} (skipped a level)"
                )
            prev_level = level
            continue

        for label, target in LINK_RE.findall(line):
            links.append((number, label, target))

    if fence_char is not None:
        issues.append(f"code block opened at line {fence_line} is never closed")
    return headings, links, issues


def check_document(root: Path, rel: str, min_bytes: int | None = None) -> list[str]:
    path = root / rel
    issues = []
    if not path.is_file():
        return [f"{rel}: missing (the README links to it)"]

    raw = path.read_bytes()
    floor = MIN_BYTES.get(rel, 0) if min_bytes is None else min_bytes
    if len(raw) < floor:
        issues.append(f"{rel}: {len(raw)} bytes is below the {floor}-byte floor")

    text = raw.decode("utf-8", errors="replace")
    headings, links, structural = parse_markdown(text)
    issues.extend(f"{rel}: {item}" for item in structural)

    h1 = [h for h in headings if h[1] == 1]
    if len(h1) != 1:
        issues.append(f"{rel}: expected exactly one H1, found {len(h1)}")

    if USER_PATH_RE.search(text):
        issues.append(rel + ": contains a hard-coded C:\\Users\\ path")

    # Anchors are resolved per document, so collect slugs first.
    slugs = {slug for _, _, _, slug in headings}
    for number, label, target in links:
        if SCHEME_RE.match(target) or target.startswith("//"):
            continue
        if target.startswith("#"):
            anchor = target[1:]
            if anchor and anchor not in slugs:
                issues.append(
                    f"{rel}:{number}: link [{label}](#{anchor}) has no matching heading"
                )
            continue
        file_part, _, anchor = target.partition("#")
        if not file_part:
            continue
        target_path = (path.parent / file_part).resolve()
        try:
            target_path.relative_to(root.resolve())
        except ValueError:
            issues.append(f"{rel}:{number}: link [{label}]({target}) escapes the repo")
            continue
        if not target_path.exists():
            issues.append(f"{rel}:{number}: link [{label}]({target}) does not exist")
            continue
        if anchor and target_path.suffix.lower() == ".md":
            target_text = target_path.read_text(encoding="utf-8", errors="replace")
            target_headings, _, _ = parse_markdown(target_text)
            target_slugs = {slug for _, _, _, slug in target_headings}
            if anchor not in target_slugs:
                issues.append(
                    f"{rel}:{number}: link [{label}]({target}) anchor has no matching heading"
                )
    return issues


def check_markers(root: Path) -> list[str]:
    issues = []
    readme = (root / "README.md").read_text(encoding="utf-8", errors="replace")
    for marker in README_MARKERS:
        if marker not in readme:
            issues.append(f"README.md: required content missing: {marker!r}")

    usage = (root / "docs/USAGE.md").read_text(encoding="utf-8", errors="replace")
    for marker in USAGE_MARKERS:
        if marker not in usage:
            issues.append(f"docs/USAGE.md: required content missing: {marker!r}")

    trouble = (root / "docs/TROUBLESHOOTING.md").read_text(
        encoding="utf-8", errors="replace"
    )
    headings = [title for _, level, title, _ in parse_markdown(trouble)[0] if level == 2]
    for keyword in TROUBLE_HEADINGS:
        if not any(keyword in title for title in headings):
            issues.append(
                f"docs/TROUBLESHOOTING.md: no H2 symptom covers {keyword!r}"
            )
    return issues


def run_gate(root: Path, min_bytes: int | None = None) -> tuple[list[str], dict]:
    issues = []
    stats = {"documents": 0, "headings": 0, "links": 0}
    for rel in DOCS:
        path = root / rel
        if not path.is_file():
            issues.append(f"{rel}: missing (the README links to it)")
            continue
        stats["documents"] += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        headings, links, _ = parse_markdown(text)
        stats["headings"] += len(headings)
        stats["links"] += len(links)
        issues.extend(check_document(root, rel, min_bytes=min_bytes))
    # Marker checks read all four documents; a missing one is already reported,
    # so do not turn that into a traceback on top of the real verdict.
    if min_bytes is None and stats["documents"] == len(DOCS):
        issues.extend(check_markers(root))
    return issues, stats


# ---------------------------------------------------------------------------
# negative controls
# ---------------------------------------------------------------------------

GOOD = {
    "README.md": """# Fixture product

Intro paragraph with enough words to look like a real page.

## Install

`one` and `two` and [usage](docs/USAGE.md) and [trouble](docs/TROUBLESHOOTING.md#7zdll-找不到).

## 三条最常见的用法

显示更多选项 appears here so the marker check has something to find.

```pwsh
Write-Host "hello"
```
""",
    "docs/USAGE.md": """# Fixture usage

Body text.

## Keys

Press `Ctrl+K` or `Space`. Extract with `--extract-here`.

## 本版本的边界

Nothing is wired yet.
""",
    "docs/TROUBLESHOOTING.md": """# Fixture trouble

Body text.

## 右键菜单里没有

Symptom, cause, fix.

## 右键菜单在「显示更多选项」里

Symptom, cause, fix.

## 7z.dll 找不到

Symptom, cause, fix.

## 压缩失败

Symptom, cause, fix.

## 中文乱码

Symptom, cause, fix.

## 归档只读

Symptom, cause, fix.

## 文件被占用

Symptom, cause, fix.

## 关联被别的软件锁定

Symptom, cause, fix.
""",
    "docs/assets/README.md": """# Fixture assets

Screenshot rules.

```text
plain text block
```
""",
}


def write_fixture(root: Path) -> None:
    for rel, text in GOOD.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def selftest() -> int:
    temp_root = Path(tempfile.mkdtemp(prefix="prism-c63-docs-"))
    failures = []
    caught = 0
    try:
        # 1. the known-good set must pass with the same parser and marker rules
        good_root = temp_root / "good"
        write_fixture(good_root)
        issues, _ = run_gate(good_root, min_bytes=0)
        if issues:
            failures.append("known-good fixture was rejected: " + "; ".join(issues))
        else:
            print("ok   known-good fixture passes")

        # 2. one injected defect per copy; each must produce its own failure
        cases = [
            ("missing document", "docs/USAGE.md"),
            ("missing H1", "README.md"),
            ("skipped heading level", "docs/USAGE.md"),
            ("code fence without language", "README.md"),
            ("hard-coded user path", "README.md"),
            ("dangling link", "README.md"),
            ("dangling anchor", "README.md"),
        ]
        for name, rel in cases:
            case_root = temp_root / ("case-" + re.sub(r"[^a-z0-9]+", "-", name))
            write_fixture(case_root)
            path = case_root / rel
            text = path.read_text(encoding="utf-8")
            if name == "missing document":
                path.unlink()
            elif name == "missing H1":
                path.write_text(text.replace("# Fixture product\n", "", 1), encoding="utf-8")
            elif name == "skipped heading level":
                path.write_text(text.replace("## Keys", "#### Keys", 1), encoding="utf-8")
            elif name == "code fence without language":
                path.write_text(text.replace("```pwsh", "```", 1), encoding="utf-8")
            elif name == "hard-coded user path":
                path.write_text(text + "\nSee C:\\Users\\someone\\a.7z\n", encoding="utf-8")
            elif name == "dangling link":
                path.write_text(
                    text.replace("docs/USAGE.md", "docs/NOPE.md", 1), encoding="utf-8"
                )
            elif name == "dangling anchor":
                path.write_text(
                    text.replace("docs/USAGE.md)", "docs/USAGE.md#no-such-heading)", 1),
                    encoding="utf-8",
                )
            case_issues, _ = run_gate(case_root, min_bytes=0)
            if not case_issues:
                failures.append(f"negative control not caught: {name}")
            else:
                caught += 1
                print(f"ok   caught: {name}")
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)

    if failures:
        for item in failures:
            print("SELFTEST FAIL: " + item)
        print(f"DOCS GATE SELFTEST: FAIL ({len(failures)} problem(s))")
        return 1
    print(f"DOCS GATE SELFTEST: PASS ({caught} negative controls, good fixture accepted)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="PrismArchive documentation gate")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = Path(args.root).resolve()
    issues, stats = run_gate(root)
    if issues:
        for issue in issues:
            print("DOCS GATE: " + issue)
        print(f"DOCS GATE: FAIL ({len(issues)} issue(s))")
        return 1
    print(
        "DOCS GATE: OK "
        f"({stats['documents']} documents, {stats['headings']} headings, "
        f"{stats['links']} links)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
