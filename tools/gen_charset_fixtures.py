#!/usr/bin/env python3
"""Regenerate the byte fixtures used by Tests/test_charset_repair.cpp.

The C++ detector is table-free (structural validity + common-band plausibility),
so its fixtures must come from an authoritative encoder -- Python's codecs here,
never hand-written bytes.

    python tools/gen_charset_fixtures.py
"""
from __future__ import annotations

FIXTURES = [
    ("GBK (Simplified Chinese)", "文档资料.docx", "gbk"),
    ("Big5 (Traditional Chinese)", "檔案資料.docx", "big5"),
    ("Shift-JIS (Japanese)", "資料フォルダ.txt", "shift_jis"),
    ("EUC-KR (Korean)", "문서자료.txt", "euc_kr"),
    ("GBK with a directory", "项目备份/说明.txt", "gbk"),
]


def main() -> int:
    for label, text, encoding in FIXTURES:
        raw = text.encode(encoding)
        print(f"// {label}: {text}")
        print(f'   hex      = "{raw.hex()}"')
        print(f'   utf8     = "{text.encode("utf-8").hex()}"')
        print(f"   as latin1= {raw.decode('latin-1')!r}   <-- the mojibake users see")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
