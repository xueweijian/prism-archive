#!/usr/bin/env python3
"""Check that the MSIX sparse-package manifest is well-formed and complete.

The sparse package is what gives PrismShellExt.dll package identity so the
Win11 IExplorerCommand context menu can register WITHOUT touching the registry
(blueprint ch. 4.1 / M2). A malformed manifest would only explode at install
time on a user machine -- so CI parses it on every push instead.
"""
from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REQUIRED_NAMESPACES = {
    "rescap": "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities",
}
REQUIRED_ELEMENTS = [
    "./Identity",
    "./Properties/DisplayName",
    "./Properties/PublisherDisplayName",
    "./Applications/Application",
]
REQUIRED_CAPABILITIES = ["runFullTrust"]


def main() -> int:
    manifest = Path(sys.argv[1] if len(sys.argv) > 1 else "Assets/Package/AppxManifest.xml")
    if not manifest.exists():
        print(f"FAIL: {manifest} missing")
        return 1
    try:
        root = ET.parse(manifest).getroot()
    except ET.ParseError as exc:
        print(f"FAIL: {manifest} is not well-formed XML: {exc}")
        return 1

    # Strip xml namespaces so find("./Identity") works regardless of xmlns.
    for element in root.iter():
        if "}" in element.tag:
            element.tag = element.tag.split("}", 1)[1]

    failures = []
    for element in REQUIRED_ELEMENTS:
        if root.find(element) is None:
            failures.append(f"missing element {element}")

    cap_root = root.find("Capabilities")
    caps = {cap.get("Name") for cap in cap_root} if cap_root is not None else set()
    for name in REQUIRED_CAPABILITIES:
        if name not in caps:
            failures.append(f"missing capability {name}")

    identity = root.find("Identity")
    if identity is not None:
        for attr in ("Name", "Publisher", "Version"):
            if not identity.get(attr):
                failures.append(f"Identity/@{attr} empty")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"PASS: {manifest} well-formed and complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
