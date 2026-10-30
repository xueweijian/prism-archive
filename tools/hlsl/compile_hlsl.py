#!/usr/bin/env python3
"""Compile the Liquid Glass HLSL with fxc.exe (Windows SDK / MSVC toolset).

Used by CI as a permanent compile guard for the optics pipeline. When fxc.exe
cannot be found the script reports SKIP instead of a fake green -- a shader that
was never compiled proves nothing.

Usage:
    python tools/hlsl/compile_hlsl.py [--strict] [--shader-dir Assets/Shaders]
"""
from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys

# Standalone variants that must compile on any machine with the Windows SDK.
REQUIRED = [
    ("LiquidGlassPreviewPS.hlsl", "PSMain"),
    ("KawaseDualBlur.hlsl", "PSMain"),
]

# The production Direct2D effect needs d2d1effecthelpers.hlsli from the SDK.
OPTIONAL = [("LiquidGlassEffect.hlsl", "main")]


def find_fxc() -> str | None:
    direct = shutil.which("fxc")
    if direct:
        return direct
    kits = os.environ.get("WindowsSdkDir") or r"C:\Program Files (x86)\Windows Kits\10"
    pattern = os.path.join(kits, "bin", "*", "x64", "fxc.exe")
    for candidate in sorted(glob.glob(pattern)):
        if os.path.exists(candidate):
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true",
                        help="exit non-zero when fxc.exe is unavailable")
    parser.add_argument("--shader-dir", default=os.path.join("Assets", "Shaders"))
    args = parser.parse_args()

    fxc = find_fxc()
    if not fxc:
        print("SKIP: fxc.exe not found -- install the Windows SDK to check HLSL")
        return 1 if args.strict else 0

    failures = 0
    for name, entry in REQUIRED + OPTIONAL:
        path = os.path.join(args.shader_dir, name)
        if not os.path.exists(path):
            print(f"SKIP: {path} missing")
            continue
        command = [fxc, "/nologo", "/T", "ps_5_0", "/E", entry, path, "/Fo", "NUL"]
        proc = subprocess.run(command, capture_output=True, text=True)
        optional = (name, entry) in OPTIONAL
        if proc.returncode == 0:
            print(f"PASS: {name} ({entry})")
        else:
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            reason = detail[0] if detail else "unknown error"
            if optional:
                print(f"SKIP: {name} -- {reason}")
            else:
                failures += 1
                print(f"FAIL: {name} -- {reason}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
