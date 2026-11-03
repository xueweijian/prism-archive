#!/usr/bin/env python3
"""Assert the four README screenshots are real, correctly sized, and modest.

Card C73. The screenshots are part of the product's first impression, so they
are checked like any other artifact: present, decodable, exactly the documented
size, and small enough that cloning the repository stays cheap.

Every check prints one line, and the verdict line is the CTest marker -- a crash
after printing cannot pass (CONTRACTS D1).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "docs" / "assets"

# name -> (width, height, max bytes)
EXPECTED = {
    "screenshot-main.png": (1280, 800, 400 * 1024),
    "screenshot-tabs.png": (1280, 800, 400 * 1024),
    "screenshot-preview.png": (1280, 800, 400 * 1024),
    "screenshot-context-menu.png": (1280, 800, 400 * 1024),
}


def png_size(path: Path) -> tuple[int, int]:
    """Read the IHDR of a PNG without any third-party dependency."""
    with path.open("rb") as handle:
        head = handle.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    width, height = struct.unpack(">II", head[16:24])
    return width, height


def main() -> int:
    failures = 0
    for name, (want_w, want_h, max_bytes) in EXPECTED.items():
        path = ASSETS / name
        if not path.is_file():
            print(f"FAIL  missing {name}")
            failures += 1
            continue
        size = path.stat().st_size
        try:
            width, height = png_size(path)
        except ValueError as exc:
            print(f"FAIL  {name}: {exc}")
            failures += 1
            continue
        if (width, height) != (want_w, want_h):
            print(f"FAIL  {name}: {width}x{height}, expected {want_w}x{want_h}")
            failures += 1
        if size > max_bytes:
            print(f"FAIL  {name}: {size} bytes exceeds {max_bytes}")
            failures += 1
        print(f"PASS  {name}: {width}x{height}, {size} bytes")

    # The README must actually show them, otherwise the files are dead weight.
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for name in EXPECTED:
        if f"docs/assets/{name}" not in readme:
            print(f"FAIL  README does not reference {name}")
            failures += 1
    if "尚未拍" in readme:
        print("FAIL  README still says the screenshots have not been taken")
        failures += 1

    if failures:
        print(f"ASSETS GATE: FAIL ({failures} issue(s))")
        return 1
    print("ASSETS GATE: PASS (4 screenshots, sizes and references verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
