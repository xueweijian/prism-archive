#!/usr/bin/env python3
"""Deterministic malformed-archive corpus generator (blueprint Phase 1).

Why a hand-built generator instead of libFuzzer/AFL (card C22 contract):

  * the Phase 1 exit criterion is *10 000 real malformed / nested / mojibake /
    encrypted samples*, i.e. a corpus with a known distribution per failure
    class -- not "as many random mutations as the clock allows". A coverage
    fuzzer spends its budget where the bug is, a corpus spends it where the
    *feature* is, and every class here maps to a defence that already exists in
    PrismCore (PathSafety / SmartExtractor / SecurityGuard);
  * reproducibility: same seed + same --total => byte-identical corpus on every
    machine, so a red run on CI is replayable locally with one command. A
    coverage fuzzer's crash is only reproducible with its exact seed AND its
    exact binary; with a header-only core rebuilt every commit that is not a
    reproducible artefact;
  * CI time: the whole 10 000-sample corpus is generated in a couple of seconds
    and consumed by Tests/test_fuzz_smoke.cpp in well under ten. A libFuzzer
    target can never be a per-push gate at that price (blueprint 7.2 wants the
    push lane at ~2.5 min), so it would become a nightly job nobody watches.

The corpus is written to a *temporary* directory: samples are build products,
never source. This generator refuses to write inside the repository.

Classes (11 malformation families, 50-200 distinct recipes each):

    truncated        archive cut at a structural landmark (header/EOCD/data)
    bad-crc          stored CRC or payload byte corrupted
    overlong-path    200 B .. 6 KB path components
    deep-nesting     3 .. 780 nested directory levels
    traversal        "../", absolute, drive, UNC, NUL, "....//" Zip-Slip shapes
    fake-size        declared 64 MiB+ unpack from a few packed bytes (bomb shape)
    bad-utf8         truncated / overlong / surrogate / legacy-CJK name bytes
    empty            zero-byte file, EOCD only, dangling central directory
    zero-byte        healthy archives whose entries all have size 0
    encrypted        ZipCrypto header + general-purpose bit 0, bogus payload
    zip64-lies       0xFFFFFFFF sizes without a zip64 extra field, fake zip64

Every family is instantiated --total/N times, so the 10 000-sample criterion is
met while the *recipe count* per family stays inside 50..200 (the recipes are
the intellectual content; the instances are the arithmetic).

Usage:
    python3 tools/fuzz/gen_corpus.py --out /tmp/prism-fuzz --total 10000
    python3 tools/fuzz/gen_corpus.py --check        # corpus contract self-test

Exit codes: 0 ok, 1 contract violation (--check), 2 usage/refused output path.
"""
import argparse
import os
import random
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# The 11 malformation families. Order is part of the corpus ABI: the C++ smoke
# test derives its per-sample expectation from the file-name prefix.
# ---------------------------------------------------------------------------
CLASSES = [
    "truncated",
    "bad-crc",
    "overlong-path",
    "deep-nesting",
    "traversal",
    "fake-size",
    "bad-utf8",
    "empty",
    "zero-byte",
    "encrypted",
    "zip64-lies",
]

RECIPES_MIN = 50
RECIPES_MAX = 200
DEFAULT_TOTAL = 10000
DEFAULT_SEED = 20261101

SIG_LOCAL = 0x04034B50
SIG_CENTRAL = 0x02014B50
SIG_EOCD = 0x06054B50


def le16(v):
    return struct.pack("<H", v & 0xFFFF)


def le32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


# ---------------------------------------------------------------------------
# Minimal ZIP writer. Deliberately hand-rolled (no zipfile): the point of the
# corpus is fields zipfile would refuse to write -- a 4 GiB size claim over 16
# bytes of payload, a 0xFFFFFFFF count without a zip64 record, a name that is
# not valid UTF-8. zipfile validates; a malformed-archive generator must not.
# ---------------------------------------------------------------------------
def build_zip(entries, comment=b"", eocd_override=None, central_count_override=None,
              cd_offset_override=None, drop_eocd=False):
    """Build a ZIP from entry dicts.

    entry = {
        name: bytes,                # raw, not validated, not normalised
        data: bytes,                # stored payload (method 0)
        flags: int,                 # general purpose bit flags
        method: int,                # 0 = stored (we never actually compress)
        crc: int | None,            # default: real zlib.crc32(data)
        comp_size: int | None,      # default: len(data)
        uncomp_size: int | None,    # default: len(data)
        extra_local: bytes,
        extra_central: bytes,
        external_attr: int,
        data_override: bytes | None,  # bytes actually written (default: data)
    }
    """
    local = []
    central = []
    offset = 0
    for e in entries:
        name = e["name"]
        data = e["data"]
        written = e.get("data_override")
        if written is None:
            written = data
        crc = e.get("crc")
        if crc is None:
            crc = zlib.crc32(written) & 0xFFFFFFFF
        comp = e.get("comp_size")
        if comp is None:
            comp = len(written)
        uncomp = e.get("uncomp_size")
        if uncomp is None:
            uncomp = len(written)
        flags = e.get("flags", 0)
        method = e.get("method", 0)
        extra_local = e.get("extra_local", b"")
        extra_central = e.get("extra_central", b"")
        ext_attr = e.get("external_attr", 0)

        header = (le32(SIG_LOCAL) + le16(20) + le16(flags) + le16(method) + le16(0) + le16(0)
                  + le32(crc) + le32(comp) + le32(uncomp) + le16(len(name))
                  + le16(len(extra_local)))
        local.append(header + name + extra_local + written)
        central.append(le32(SIG_CENTRAL) + le16(20) + le16(20) + le16(flags) + le16(method)
                       + le16(0) + le16(0) + le32(crc) + le32(comp) + le32(uncomp)
                       + le16(len(name)) + le16(len(extra_central)) + le16(0)  # comment
                       + le16(0) + le16(0) + le32(ext_attr) + le32(offset)
                       + name + extra_central)
        offset += len(local[-1])

    body = b"".join(local)
    cd = b"".join(central)
    count = len(entries) if central_count_override is None else central_count_override
    cd_offset = len(body) if cd_offset_override is None else cd_offset_override
    tail = b""
    if not drop_eocd:
        tail = (le32(SIG_EOCD) + le16(0) + le16(0) + le16(count) + le16(count)
                + le32(len(cd)) + le32(cd_offset) + le16(len(comment)) + comment)
    if eocd_override is not None:
        tail = eocd_override
    return body + cd + tail


def _clean_entries(n, rng):
    """n healthy entries with unremarkable names -- the base every malformation
    is applied to, so a sample differs from a valid archive in ONE dimension."""
    out = []
    for i in range(n):
        payload = ("payload-%d-%d\n" % (i, rng.randrange(1000))).encode("ascii")
        out.append({"name": ("docs/part_%02d.txt" % i).encode("ascii"), "data": payload})
    return out


# ---------------------------------------------------------------------------
# Recipe catalogues. Each function returns a list of 50..200 distinct recipes;
# render() turns (recipe, rng) into the sample bytes. rng makes two instances of
# the same recipe different, which is what scales 100 recipes to 10 000 samples.
# ---------------------------------------------------------------------------
def recipes_truncated():
    # Cut points derived from real structural landmarks of a 3-entry archive,
    # plus byte-exact offsets inside the local header / EOCD signature.
    cases = []
    # A 0-byte file is the "empty" family's job; truncation here starts at 1 so
    # every sample still carries a recognisable header prefix.
    for cut in [1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24]:
        cases.append({"cut": cut, "entries": 3})
    for n in range(1, 21):                    # cut inside the n-th local header
        cases.append({"cut_kind": "local", "index": n, "entries": 3})
    for n in range(1, 21):                    # cut inside the n-th payload
        cases.append({"cut_kind": "payload", "index": n, "entries": 3})
    for n in range(1, 31):                    # cut inside the central directory
        cases.append({"cut_kind": "cd", "index": n, "entries": 3})
    for n in range(1, 25):                    # cut inside the EOCD tail
        cases.append({"cut_kind": "tail", "index": n, "entries": 3})
    for n in range(1, 9):                     # over-truncation: signature only
        cases.append({"cut": n, "entries": 1})
    return cases[:RECIPES_MAX] or cases


def render_truncated(case, rng):
    entries = _clean_entries(case.get("entries", 3), rng)
    blob = build_zip(entries)
    cut = case.get("cut")
    if cut is None:
        kind = case["cut_kind"]
        idx = case["index"]
        if kind == "local":
            # Every local header is 30 bytes + name; aim inside the idx-th one.
            pos = 0
            for _ in range(idx):
                e = entries[rng.randrange(len(entries))]
                pos += 30 + len(e["name"]) + len(e["data"])
            cut = max(1, min(len(blob) - 1, pos + rng.randrange(1, 30)))
        elif kind == "payload":
            pos = 30 + len(entries[0]["name"])
            cut = max(1, min(len(blob) - 1, pos + idx * 3))
        elif kind == "cd":
            cut = max(1, len(blob) - 22 - idx * 4)
        else:
            cut = max(1, len(blob) - 25 + (idx % 22))
    cut = min(cut, len(blob))
    return blob[:cut]


def recipes_bad_crc():
    cases = []
    for bit in range(0, 32):                  # one flipped CRC bit per recipe
        cases.append({"crc_flip": 1 << bit})
    for delta in range(1, 33):                # off-by-delta CRC (1..32)
        cases.append({"crc_delta": delta})
    for n in range(1, 33):                    # payload byte corrupted, CRC stale
        cases.append({"data_flip": n})
    cases.append({"crc_zero": True})
    cases.append({"crc_all_ones": True})
    return cases[:RECIPES_MAX]


def render_bad_crc(case, rng):
    entries = _clean_entries(3, rng)
    target = rng.randrange(len(entries))
    e = entries[target]
    if "crc_flip" in case:
        good = zlib.crc32(e["data"]) & 0xFFFFFFFF
        e["crc"] = good ^ case["crc_flip"]
    elif "crc_delta" in case:
        good = zlib.crc32(e["data"]) & 0xFFFFFFFF
        e["crc"] = (good + case["crc_delta"]) & 0xFFFFFFFF
    elif "data_flip" in case:
        data = bytearray(e["data"])
        if not data:
            data = bytearray(b"x")
        idx = case["data_flip"] % len(data)
        data[idx] ^= 0xFF
        e["data"] = bytes(data)              # keeps the ORIGINAL crc -> mismatch
    else:
        e["crc"] = 0 if case.get("crc_zero") else 0xFFFFFFFF
    return build_zip(entries)


def recipes_overlong_path():
    cases = []
    for i in range(60):                       # single 200..3863 byte component
        cases.append({"len": 200 + i * 37})
    for i in range(30):                       # component above the 255 limit
        cases.append({"len": 256 + i * 5})
    for i in range(40):                       # 340..732 components, ~20..45 KB
        cases.append({"per": 60, "parts": 340 + i * 10})
    for i in range(20):                       # exactly at / just past the limit
        cases.append({"len": 254 + i})
    return cases[:RECIPES_MAX]


def render_overlong_path(case, rng):
    unit = "p%03d" % rng.randrange(1000)
    if "len" in case:
        comps = [unit + "x" * max(0, case["len"] - len(unit))]
    else:
        comps = [("c%03d" % i) + "x" * max(0, case["per"] - 4) for i in range(case["parts"])]
    name = "/".join(comps)
    entries = [{"name": name.encode("ascii"), "data": b"x" * 8}]
    return build_zip(entries)


def recipes_deep_nesting():
    cases = []
    for i in range(60):                       # 3..62 levels
        cases.append({"depth": 3 + i})
    for i in range(40):                       # 64..256 levels
        cases.append({"depth": 64 + i * 5})
    for i in range(40):                       # 260..780 levels
        cases.append({"depth": 260 + i * 13})
    return cases[:RECIPES_MAX]


def render_deep_nesting(case, rng):
    depth = case["depth"]
    comps = ["d%03d" % (i % 1000) for i in range(depth)]
    name = "/".join(comps) + "/leaf.txt"
    entries = [{"name": name.encode("ascii"), "data": b"leaf"}]
    return build_zip(entries)


TRAVERSAL_PATTERNS = [
    b"../evil.txt",
    b"..\\evil.txt",
    b"../../evil.txt",
    b"..\\..\\evil.dll",
    b"/etc/passwd",
    b"//server/share/x.txt",
    b"\\\\server\\share\\x.txt",
    b"C:\\Windows\\evil.dll",
    b"C:evil.txt",
    b"\\\\?\\C:\\Windows\\evil.dll",
    b"a/../../b.txt",
    b"a/b/../../../../c.txt",
    b"....//....//evil.txt",
    b"..../....//evil.txt",
    b"..%2f..%2fevil.txt",
    b"..;/evil.txt",
    b"dir/..",
    b"..",
    b"../",
    b"./../evil.txt",
    b"a/./b/../.././c.txt",
    b".. /\x20/evil.txt",
    b"a\x00b/../../evil.txt",
    b"a.txt:hidden",
    b"con",
    b"CON.txt",
    b"aux/../../../evil.txt",
    b"sub/../../../../../../x",
    b"~root/.ssh/authorized_keys",
    b"$Recycle.Bin/../../x",
    b"..\\..\\..\\..\\windows\\system32\\drivers\\etc\\hosts",
    b"\xef\xbb\xbf../evil.txt",
    b"dir/sub/../../../../../../../evil.sh",
    b".",
    b"./",
    b"a//..//..//evil",
    b"%2e%2e/%2e%2e/evil",
    b"..\\..\\..\\evil",
    b"c:/windows/evil.dll",
    b"\\\\?\\UNC\\srv\\share\\evil.dll",
]


def recipes_traversal():
    cases = []
    for i, pat in enumerate(TRAVERSAL_PATTERNS):
        cases.append({"pattern": pat, "extra": i})
    # Two-segment and three-segment escalations built from the table.
    for i in range(len(TRAVERSAL_PATTERNS), RECIPES_MAX // 2):
        base = TRAVERSAL_PATTERNS[i % len(TRAVERSAL_PATTERNS)]
        cases.append({"pattern": b"../" * (1 + i % 6) + base.lstrip(b"/\\"), "extra": i})
    cases.append({"pattern": b"\x00" * 8, "extra": 999})
    cases.append({"pattern": b"..\\\x00/../evil", "extra": 1000})
    return cases[:RECIPES_MAX]


def render_traversal(case, rng):
    hostile = case["pattern"]
    entries = [
        {"name": b"docs/readme.txt", "data": b"harmless\n"},
        {"name": hostile, "data": b"P" * 16},
        {"name": bytes(rng.randrange(97, 123) for _ in range(6)) + b".txt", "data": b"ok"},
    ]
    return build_zip(entries)


def recipes_fake_size():
    cases = []
    for i in range(60):                       # 64 MiB .. ~1 GiB declared
        cases.append({"uncomp": (64 << 20) + i * (1 << 20), "comp": 16 + i})
    for i in range(40):                       # ratio extremes, tiny packed side
        cases.append({"uncomp": (1 << 30) + i * (1 << 20), "comp": 1 + i % 8})
    for i in range(40):                       # 2^32-1 style lies (no zip64)
        cases.append({"uncomp": 0xFFFFFFFF, "comp": 32 + i})
    return cases[:RECIPES_MAX]


def render_fake_size(case, rng):
    payload = b"\x00" * 32
    entries = [
        {"name": b"bomb.bin", "data": payload, "comp_size": case["comp"],
         "uncomp_size": case["uncomp"]},
        {"name": b"readme.txt", "data": b"bomb-shaped sample\n"},
    ]
    return build_zip(entries)


BAD_UTF8_PATTERNS = [
    b"\x80name.txt",                          # lone continuation
    b"\xbf\xbf.txt",                          # only continuations
    b"\xc0\xaf.txt",                          # overlong '/'
    b"\xc1\xbf.txt",                          # overlong
    b"\xe0\x80\xaf.txt",                      # overlong 3-byte
    b"\xed\xa0\x80.txt",                      # UTF-16 surrogate D800
    b"\xed\xbf\xbf.txt",                      # surrogate DFFF
    b"\xf4\x90\x80\x80.txt",                  # beyond U+10FFFF
    b"\xf8\x88\x80\x80\x80.txt",              # 5-byte sequence
    b"\xfe\xff.txt",                          # invalid lead bytes
    b"\xff\xfe.txt",
    b"\xc3.txt",                              # truncated 2-byte seq at end
    b"\xe6\x96.txt",                          # truncated 3-byte seq
    b"\xf0\x9f\x92.txt",                      # truncated 4-byte seq
    b"\xd6\xd0\xce\xc4.txt",                  # GBK "中文", utf8 flag OFF
    b"\x82\xa0\x82\xa2.txt",                  # Shift-JIS, utf8 flag OFF
    b"\xa4\xa4\xa4\xe5.txt",                  # Big5, utf8 flag OFF
    b"\xb0\xa1\xb3\xaa.txt",                  # EUC-KR, utf8 flag OFF
    b"\xef\xbb\xbfname.txt",                  # UTF-8 BOM kept in the name
    b"a\x00b.txt",                            # embedded NUL
    b"\xc0\x80.txt",                          # CESU/overlong NUL
    b"\xe6\x96\x87\xff.txt",                  # valid CJK + invalid tail
    b"\xcc\x81\xcc\x82.txt",                  # combining marks (valid but odd)
    b"\xf0\x9f\x92\xa9.txt",                  # valid emoji (control: must pass)
    b"\xe8\x8b\xa5\xe6\x97\xa5.txt",          # valid Japanese (control)
]


def recipes_bad_utf8():
    cases = []
    for i, pat in enumerate(BAD_UTF8_PATTERNS):
        cases.append({"pattern": pat, "utf8_flag": (i % 3 != 1)})
    for i in range(len(BAD_UTF8_PATTERNS), RECIPES_MAX // 2):
        pat = BAD_UTF8_PATTERNS[i % len(BAD_UTF8_PATTERNS)]
        cases.append({"pattern": pat * (1 + i % 4), "utf8_flag": (i % 2 == 0)})
    for i in range(RECIPES_MAX // 2, 90):     # random high-byte noise
        cases.append({"random_high": i, "utf8_flag": (i % 2 == 0)})
    return cases[:RECIPES_MAX]


def render_bad_utf8(case, rng):
    if "random_high" in case:
        n = 3 + case["random_high"] % 12
        name = bytes(rng.randrange(0x80, 0x100) for _ in range(n)) + b".txt"
    else:
        name = case["pattern"]
    entries = [
        {"name": name, "data": b"mojibake sample\n",
         "flags": 0x0800 if case.get("utf8_flag") else 0},
        {"name": b"docs/readme.txt", "data": b"ok\n"},
    ]
    return build_zip(entries)


def recipes_empty():
    cases = [{"kind": "zero-bytes"}]
    for i in range(30):                       # EOCD with a variable comment
        cases.append({"kind": "eocd-comment", "n": i})
    for i in range(30):                       # EOCD with a bogus entry count
        cases.append({"kind": "eocd-count", "n": i})
    for i in range(30):                       # dangling central directory
        cases.append({"kind": "dangling-cd", "n": i})
    for i in range(10):                       # signature-only truncations
        cases.append({"kind": "sig", "n": i})
    cases.append({"kind": "dir-only"})
    return cases[:RECIPES_MAX]


def render_empty(case, rng):
    kind = case["kind"]
    if kind == "zero-bytes":
        return b""
    if kind == "eocd-comment":
        n = case["n"]
        return build_zip([], comment=bytes(rng.randrange(65, 90) for _ in range(n)))
    if kind == "eocd-count":
        n = case["n"]
        return build_zip([], central_count_override=n)
    if kind == "dangling-cd":
        n = case["n"]
        return build_zip([], cd_offset_override=1000 + n)
    if kind == "sig":
        return (le32(SIG_EOCD) + le16(0) + le16(0))[: 2 + case["n"] % 6]
    entries = [{"name": b"empty-dir/", "data": b"", "external_attr": 0x10}]
    return build_zip(entries)


def recipes_zero_byte():
    cases = []
    for i in range(60):
        cases.append({"files": 1 + i % 6, "dirs": i % 3, "names": i})
    for i in range(40):
        cases.append({"files": 1, "dirs": 0, "deep": i})
    return cases[:RECIPES_MAX]


def render_zero_byte(case, rng):
    entries = []
    for d in range(case.get("dirs", 0)):
        entries.append({"name": ("dir%d/" % d).encode("ascii"), "data": b"",
                        "external_attr": 0x10})
    deep = case.get("deep", 0)
    prefix = ("nested/" * (deep % 20)) if deep else ""
    for f in range(case.get("files", 1)):
        entries.append({"name": (prefix + "empty_%02d.dat" % f).encode("ascii"),
                        "data": b""})
    rng.shuffle(entries)
    return build_zip(entries)


def recipes_encrypted():
    cases = []
    for i in range(50):                       # ZipCrypto bit + bogus 12-byte header
        cases.append({"kind": "zipcrypto", "n": i})
    for i in range(30):                       # AES-extra-field shape (0x9901)
        cases.append({"kind": "aes-extra", "n": i})
    for i in range(30):                       # "encrypted" bit with empty payload
        cases.append({"kind": "empty-enc", "n": i})
    return cases[:RECIPES_MAX]


def render_encrypted(case, rng):
    kind = case["kind"]
    n = case["n"]
    if kind == "zipcrypto":
        payload = bytes(rng.randrange(0, 256) for _ in range(12 + n % 9))
        entries = [
            {"name": b"secret/report.pdf", "data": b"\x01" * (16 + n),
             "flags": 0x0001, "data_override": payload,
             "comp_size": len(payload)},
            {"name": b"secret/readme.txt", "data": b"ok\n"},
        ]
    elif kind == "aes-extra":
        aes = b"\x01\x99" + le16(7) + le16(2) + b"AES" + b"\x03" + b"\x00"
        payload = bytes(rng.randrange(0, 256) for _ in range(16 + n))
        entries = [
            {"name": b"secret/aes.docx", "data": b"\x02" * 32, "flags": 0x0001,
             "extra_local": aes, "extra_central": aes,
             "method": 99, "data_override": payload, "comp_size": len(payload)},
        ]
    else:
        entries = [
            {"name": b"secret/empty.txt", "data": b"", "flags": 0x0001,
             "comp_size": 0, "uncomp_size": 0},
        ]
    return build_zip(entries)


def recipes_zip64_lies():
    cases = []
    for i in range(50):                       # 0xFFFFFFFF sizes, no zip64 extra
        cases.append({"kind": "no-extra", "n": i})
    for i in range(30):                       # zip64 extra with garbage length
        cases.append({"kind": "bad-extra", "n": i})
    for i in range(20):                       # EOCD count 0xFFFF, no locator
        cases.append({"kind": "eocd-count", "n": i})
    return cases[:RECIPES_MAX]


def render_zip64_lies(case, rng):
    kind = case["kind"]
    n = case["n"]
    if kind == "no-extra":
        entries = [
            {"name": b"huge.bin", "data": b"\x00" * 8,
             "comp_size": 0xFFFFFFFF, "uncomp_size": 0xFFFFFFFF},
            {"name": b"readme.txt", "data": b"zip64 lie\n"},
        ]
        return build_zip(entries)
    if kind == "bad-extra":
        extra = b"\x01\x00" + le16(4) + le32(1 << 20)  # says 16 bytes, has 4
        entries = [
            {"name": b"huge.bin", "data": b"\x00" * 8,
             "comp_size": 0xFFFFFFFF, "uncomp_size": 0xFFFFFFFF,
             "extra_local": extra, "extra_central": extra},
        ]
        return build_zip(entries)
    entries = _clean_entries(2, rng)
    return build_zip(entries, central_count_override=0xFFFF)


RECIPE_FN = {
    "truncated": recipes_truncated,
    "bad-crc": recipes_bad_crc,
    "overlong-path": recipes_overlong_path,
    "deep-nesting": recipes_deep_nesting,
    "traversal": recipes_traversal,
    "fake-size": recipes_fake_size,
    "bad-utf8": recipes_bad_utf8,
    "empty": recipes_empty,
    "zero-byte": recipes_zero_byte,
    "encrypted": recipes_encrypted,
    "zip64-lies": recipes_zip64_lies,
}

RENDER_FN = {
    "truncated": render_truncated,
    "bad-crc": render_bad_crc,
    "overlong-path": render_overlong_path,
    "deep-nesting": render_deep_nesting,
    "traversal": render_traversal,
    "fake-size": render_fake_size,
    "bad-utf8": render_bad_utf8,
    "empty": render_empty,
    "zero-byte": render_zero_byte,
    "encrypted": render_encrypted,
    "zip64-lies": render_zip64_lies,
}


def recipe_count(cls):
    return len(RECIPE_FN[cls]())


def render_sample(cls, instance, seed):
    """Sample bytes for (class, instance). Pure function of (cls, instance, seed)."""
    cases = RECIPE_FN[cls]()
    case = cases[instance % len(cases)]
    rng = random.Random(seed * 1000003 + CLASSES.index(cls) * 7919 + instance)
    return RENDER_FN[cls](case, rng)


# ---------------------------------------------------------------------------
# The negative control: ONE genuinely healthy archive. The smoke test requires
# it to be ACCEPTED on every path; a build whose rejection machinery is inverted
# must therefore go red (CONTRACTS D3 -- see the two mutant targets in
# Tests/suites/c22-fuzz.cmake).
# ---------------------------------------------------------------------------
def render_control(kind="valid"):
    if kind == "valid":
        entries = [
            {"name": b"docs/readme.txt", "data": b"PrismArchive control sample\n"},
            {"name": b"docs/img/photo.bin", "data": bytes(range(0, 64)) * 4},
            {"name": b"src/main.cpp", "data": b"int main() { return 0; }\n"},
        ]
        return build_zip(entries)
    if kind == "nested-doll":
        entries = [
            {"name": b"Photo/Photo/a.jpg", "data": b"\xff\xd8\xff" + bytes(32)},
            {"name": b"Photo/Photo/b.jpg", "data": b"\xff\xd8\xff" + bytes(64)},
        ]
        return build_zip(entries)
    entries = [
        {"name": b"WinRAR/download_this_setup.url", "data": b"[InternetShortcut]\n"},
        {"name": b"App/app.exe", "data": b"MZ" + bytes(16)},
    ]
    return build_zip(entries)


CONTROL_SAMPLES = ["valid", "nested-doll", "promo-noise"]


# ---------------------------------------------------------------------------
# Self-validation (CONTRACTS D4: the checker must itself be calibrated).
# A deliberately small central-directory reader, used ONLY to prove the control
# sample is well formed and the corrupt ones are not.
# ---------------------------------------------------------------------------
def read_central_dir(blob):
    """-> (entries, error). Entries: dict(name, crc, comp, uncomp, offset, flags)."""
    if len(blob) < 22:
        return [], "file shorter than an EOCD record"
    eocd = blob.rfind(struct.pack("<I", SIG_EOCD), max(0, len(blob) - 65558))
    if eocd < 0:
        return [], "no EOCD signature"
    if eocd + 22 > len(blob):
        return [], "EOCD truncated"
    (count, cd_size, cd_off) = struct.unpack("<HII", blob[eocd + 10: eocd + 20])
    if cd_off + cd_size > len(blob):
        return [], "central directory past EOF"
    entries = []
    pos = cd_off
    for _ in range(count):
        if pos + 46 > len(blob):
            return entries, "central entry truncated"
        fields = struct.unpack("<IHHHHHHIIIHHHHHII", blob[pos: pos + 46])
        if fields[0] != SIG_CENTRAL:
            return entries, "bad central signature"
        flags, crc, comp, uncomp = fields[3], fields[7], fields[8], fields[9]
        nlen, elen = fields[10], fields[11]
        name = blob[pos + 46: pos + 46 + nlen]
        entries.append({"name": name, "flags": flags, "crc": crc, "comp": comp,
                        "uncomp": uncomp})
        pos += 46 + nlen + elen + fields[12]
    return entries, ""


def check_corpus_contract(total):
    problems = []
    for cls in CLASSES:
        n = recipe_count(cls)
        if n < RECIPES_MIN or n > RECIPES_MAX:
            problems.append("%s: %d recipes, outside %d..%d" % (cls, n, RECIPES_MIN, RECIPES_MAX))
    if total < 10000:
        problems.append("--total %d is below the blueprint's 10 000-sample criterion" % total)
    # Determinism: identical (cls, instance, seed) must yield identical bytes.
    for cls in CLASSES:
        for instance in (0, 1, 7, 99):
            a = render_sample(cls, instance, DEFAULT_SEED)
            b = render_sample(cls, instance, DEFAULT_SEED)
            if a != b:
                problems.append("%s[%d]: not deterministic" % (cls, instance))
            if not a and cls != "empty":
                problems.append("%s[%d]: empty sample outside the 'empty' class" % (cls, instance))
    # The control must be a valid archive, and the bad-crc family must not be.
    for kind in CONTROL_SAMPLES:
        blob = render_control(kind)
        entries, err = read_central_dir(blob)
        if err:
            problems.append("control/%s: %s" % (kind, err))
        if not entries:
            problems.append("control/%s: no entries" % kind)
        for e in entries:
            if e["comp"] != e["uncomp"] and kind == "valid":
                problems.append("control/%s: size mismatch" % kind)
    good = render_control("valid")
    bad = render_sample("bad-crc", 0, DEFAULT_SEED)
    bad_entries, bad_err = read_central_dir(bad)
    if not bad_err and bad_entries:
        good_entries, _ = read_central_dir(good)
        if bad_entries[0]["crc"] == good_entries[0]["crc"] and \
                bad_entries[0]["name"] == good_entries[0]["name"]:
            problems.append("bad-crc[0]: CRC identical to the control")
    # A traversal sample must really contain an escaping name.
    trav = render_sample("traversal", 0, DEFAULT_SEED)
    trav_entries, _ = read_central_dir(trav)
    if not any(b".." in e["name"] or e["name"].startswith((b"/", b"\\", b"C:"))
               for e in trav_entries):
        problems.append("traversal[0]: no escaping name in the sample")
    return problems


BUILD_ROOT_NAMES = ("build", "out", "cmake-build")
BUILD_ROOT_PREFIXES = ("build-", "cmake-build-")


def is_build_artifact_dir(out, repo):
    """True when the path sits under a top-level build artefact directory.

    .gitignore lists build/ and out/; CI additionally builds into build-asan for
    the sanitizer lane. A corpus written there is a build product, so those names
    are allowed explicitly instead of trusting git's ignore database -- an
    earlier version shelled out to "git check-ignore", which turned the fixture
    red on CI (build-asan is not ignored) and could block on a repository lock.

    Deliberately conventional and dependency-free: the ONE rule is "sample trees
    live in build artefacts, never in the source tree".
    """
    parts = [p.lower() for p in out.relative_to(repo).parts]
    if not parts:
        return False
    top = parts[0]
    return (top in BUILD_ROOT_NAMES or
            any(top.startswith(prefix) for prefix in BUILD_ROOT_PREFIXES))


def assert_writable_corpus_dir(out_dir):
    """A corpus may live outside the checkout or inside a build tree; never in source."""
    repo = Path(__file__).resolve().parent.parent.parent
    try:
        out = Path(out_dir).resolve()
    except OSError:
        return True
    try:
        out.relative_to(repo)
    except ValueError:
        return True  # outside the checkout: nothing to protect
    return is_build_artifact_dir(out, repo)


def distribute(total):
    """Exact per-class instance counts summing to total (>= 50 each).

    NOTE: an earlier version "trimmed" the tail when total < 50*n, and because
    every count already sat at the 50 floor that loop never terminated -- a
    --total 30 smoke run hung instead of failing. Small requests are now simply
    raised to the floor, which is the only answer that keeps the per-class
    contract intact.
    """
    n = len(CLASSES)
    floor = RECIPES_MIN * n
    if total <= floor:
        return [RECIPES_MIN] * n
    base, remainder = divmod(total, n)
    counts = [base] * n
    for i in range(remainder):
        counts[i] += 1
    return counts


# ---------------------------------------------------------------------------
# Packed container: one file instead of N tiny files.
#
# Why: on Windows the push lane paid 31 s just *creating* 10 000 small files
# (real-time AV opens every new file). The packed form costs one 5 MB write and
# keeps the sample count identical, so the gate stays about the corpus and not
# about the filesystem. --loose N still materialises N real archive files,
# which is what the optional 7z.dll pass and a human reviewer need.
#
# Layout (little endian, all offsets 0-based):
#   magic   "PRISMFZ1\n"
#   classes u16 n, then n x { u8 len, len bytes of ASCII class name }
#   samples repeated to EOF: u32 class_index, u32 size, size bytes of archive
# The class table is written so the consumer can prove it is reading the SAME
# family list -- a renamed class on either side is a hard error, not a silent
# "0 samples in that class" pass.
# ---------------------------------------------------------------------------
PACK_MAGIC = b"PRISMFZ1\n"


def pack_corpus(blobs):
    """blobs: list of (class_index, bytes) -> packed container bytes."""
    out = bytearray(PACK_MAGIC)
    out += le16(len(CLASSES))
    for cls in CLASSES:
        raw = cls.encode("ascii")
        out += bytes([len(raw)]) + raw
    for class_index, blob in blobs:
        out += le32(class_index) + le32(len(blob)) + blob
    return bytes(out)


def main(argv):
    ap = argparse.ArgumentParser(description="Deterministic malformed-archive corpus")
    ap.add_argument("--out", help="output directory (must be outside the repository)")
    ap.add_argument("--total", type=int, default=DEFAULT_TOTAL,
                    help="malformed samples to emit (default %d)" % DEFAULT_TOTAL)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--check", action="store_true",
                    help="validate the recipe catalogue + determinism; writes nothing")
    ap.add_argument("--pack", action="store_true",
                    help="write one packed corpus.bin instead of N loose files")
    ap.add_argument("--loose", type=int, default=0,
                    help="materialise N samples as real archive files (with --pack)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.check:
        problems = check_corpus_contract(args.total)
        if problems:
            print("FUZZ CORPUS CONTRACT: FAILED")
            for p in problems:
                print("  " + p)
            return 1
        print("FUZZ CORPUS CONTRACT OK: %d classes, recipes %d..%d, deterministic, "
              "control valid, total=%d" % (len(CLASSES), RECIPES_MIN, RECIPES_MAX, args.total))
        return 0

    if not args.out:
        print("usage: gen_corpus.py --out DIR [--total N]   (or --check)", file=sys.stderr)
        return 2
    if not assert_writable_corpus_dir(args.out):
        print("refusing to write the corpus into the source tree: %s "
              "(use an artefact directory: build/, build-* or out/)" % args.out,
              file=sys.stderr)
        return 2

    out = Path(args.out)
    if out.exists():
        # A stale corpus from an older seed would make the run irreproducible.
        for child in sorted(out.rglob("*"), reverse=True):
            if child.is_file() or child.is_symlink():
                child.unlink()
            elif child.is_dir():
                child.rmdir()
    out.mkdir(parents=True, exist_ok=True)
    (out / "control").mkdir(parents=True, exist_ok=True)

    counts = distribute(args.total)
    written = 0
    per_class = {}
    blobs = []
    for class_index, (cls, count) in enumerate(zip(CLASSES, counts)):
        for instance in range(count):
            blob = render_sample(cls, instance, args.seed)
            blobs.append((class_index, blob))
            if not args.pack:
                (out / ("%s-%04d.zip" % (cls, instance))).write_bytes(blob)
            written += 1
        per_class[cls] = count

    if args.pack:
        (out / "corpus.bin").write_bytes(pack_corpus(blobs))
        if args.loose > 0:
            loose_dir = out / "loose"
            loose_dir.mkdir(exist_ok=True)
            stride = max(1, written // max(1, args.loose))
            picks = blobs[::stride][: args.loose]
            for pick_no, (class_index, blob) in enumerate(picks):
                (loose_dir / ("%s-%03d.zip" % (CLASSES[class_index], pick_no))).write_bytes(blob)

    for kind in CONTROL_SAMPLES:
        (out / "control" / ("%s.zip" % kind)).write_bytes(render_control(kind))

    if not args.quiet:
        print("FUZZ CORPUS: seed=%d total=%d (+%d control) mode=%s" %
              (args.seed, written, len(CONTROL_SAMPLES),
               "packed" if args.pack else "loose-files"))
        for cls in CLASSES:
            print("  %-14s %5d samples / %3d recipes" % (cls, per_class[cls], recipe_count(cls)))
    print("FUZZ CORPUS READY: %d samples in %s" % (written, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))