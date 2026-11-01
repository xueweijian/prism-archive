#!/usr/bin/env python3
r"""Generate Assets/Icon/prism.ico -- the multi-size application icon.

Deliberately dependency-free: this file is the *source* of the committed .ico, so
a reviewer can regenerate it byte-for-byte (Pillow's output changes between
versions and would make the binary unreviewable). Rasterising is plain
supersampling: shapes are tested on an SS-times-finer grid, then box-downsampled,
which is the same antialiasing Win2D/D2D would give us.

Two compositions:
  * "detailed" (>=48 px): glass tile, ice triangle, white beam, six-band fan.
  * "compact"  (<=40 px): thicker strokes, four bands, no rim gloss -- 16 px is
    the size Explorer's list view actually uses, and a shrunken detailed drawing
    turns into mush there.

Usage:
  python Assets/Icon/make_prism_icon.py                        # write prism.ico
  python Assets/Icon/make_prism_icon.py --check                # validate current
  python Assets/Icon/make_prism_icon.py --check --mutate drop-256
  python Assets/Icon/make_prism_icon.py --preview-dir E:\Temp\c3\icon

The --check mode is the guard; --mutate is its negative control (D3): e.g.
drop-256 must make --check exit non-zero, proving the size assertion is real and
not a restatement of the writer.
"""
from __future__ import annotations

import argparse
import binascii
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ICO_PATH = HERE / "prism.ico"

# Every size Explorer may ask for; 16/32/48/256 are the contract (the task and
# the Windows icon guidelines agree on those four).
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
REQUIRED = (16, 32, 48, 256)


# ---------------------------------------------------------------------------
# canvas: straight-alpha RGBA floats, painter's algorithm
# ---------------------------------------------------------------------------


class Canvas:
    def __init__(self, size, scale):
        self.n = size * scale          # raster grid
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(self.n * self.n)]

    def paint(self, inside, color, alpha=1.0, bbox=None):
        """Composite color over every raster sample where inside(x, y) is true."""
        n = self.n
        x0, y0, x1, y1 = bbox if bbox else (0.0, 0.0, 1.0, 1.0)
        i0, i1 = max(0, int(x0 * n)), min(n, int(x1 * n) + 2)
        j0, j1 = max(0, int(y0 * n)), min(n, int(y1 * n) + 2)
        inv = 1.0 / n
        for j in range(j0, j1):
            y = (j + 0.5) * inv
            row = j * n
            for i in range(i0, i1):
                x = (i + 0.5) * inv
                if not inside(x, y):
                    continue
                c = color(x, y) if callable(color) else color
                sa = (c[3] if len(c) > 3 else 1.0) * alpha
                if sa <= 0.0:
                    continue
                d = self.px[row + i]
                da = d[3]
                oa = sa + da * (1.0 - sa)
                if oa <= 0.0:
                    continue
                k = da * (1.0 - sa)
                d[0] = (c[0] * sa + d[0] * k) / oa
                d[1] = (c[1] * sa + d[1] * k) / oa
                d[2] = (c[2] * sa + d[2] * k) / oa
                d[3] = oa

    def resolve(self, size):
        n, s = self.n, self.n // size
        out = bytearray(size * size * 4)
        acc = s * s
        for y in range(size):
            for x in range(size):
                r = g = b = a = 0.0
                for j in range(s):
                    row = (y * s + j) * n
                    for i in range(s):
                        p = self.px[row + x * s + i]
                        pa = p[3]
                        r += p[0] * pa
                        g += p[1] * pa
                        b += p[2] * pa
                        a += pa
                o = (y * size + x) * 4
                if a <= 0.0:
                    continue
                out[o + 0] = max(0, min(255, round(r / a * 255.0)))
                out[o + 1] = max(0, min(255, round(g / a * 255.0)))
                out[o + 2] = max(0, min(255, round(b / a * 255.0)))
                out[o + 3] = max(0, min(255, round(a / acc * 255.0)))
        return bytes(out)


# ---------------------------------------------------------------------------
# signed-distance shapes (normalised 0..1 coordinates)
# ---------------------------------------------------------------------------


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    dot = vx * vx + vy * vy
    t = 0.0 if dot == 0.0 else max(0.0, min(1.0, (wx * vx + wy * vy) / dot))
    dx, dy = wx - t * vx, wy - t * vy
    return (dx * dx + dy * dy) ** 0.5


def rounded_rect(x0, y0, x1, y1, r):
    def inside(x, y):
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        dx, dy = x - cx, y - cy
        return dx * dx + dy * dy <= r * r
    return inside, (x0, y0, x1, y1)


def triangle(p0, p1, p2):
    (ax, ay), (bx, by), (cx, cy) = p0, p1, p2
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)

    def inside(x, y):
        l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
        l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
        l3 = 1.0 - l1 - l2
        return l1 >= 0.0 and l2 >= 0.0 and l3 >= 0.0
    xs = (ax, bx, cx)
    ys = (ay, by, cy)
    return inside, (min(xs), min(ys), max(xs), max(ys))


def capsule(a, b, r):
    def inside(x, y):
        return seg_dist(x, y, a[0], a[1], b[0], b[1]) <= r
    xs = (a[0], b[0])
    ys = (a[1], b[1])
    return inside, (min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)


def lerp(c0, c1, t):
    t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
    return tuple(c0[k] + (c1[k] - c0[k]) * t for k in range(len(c0)))


# ---------------------------------------------------------------------------
# the mark
# ---------------------------------------------------------------------------

TILE_TOP = (0.086, 0.106, 0.196)       # #161b32
TILE_BOTTOM = (0.212, 0.086, 0.345)    # #361658
ICE_HI = (0.949, 0.961, 1.0)           # #f2f5ff
ICE_LO = (0.741, 0.780, 0.898)         # #bdc7e5
SPECTRUM = (
    (1.000, 0.302, 0.427),   # red
    (1.000, 0.588, 0.314),   # orange
    (1.000, 0.839, 0.400),   # amber
    (0.431, 0.906, 0.718),   # green
    (0.376, 0.647, 0.980),   # blue
    (0.655, 0.545, 0.980),   # violet
)


def draw(size, mutate=""):
    compact = size <= 40
    # Supersampling factor: kept low for the big frames (a 256px tile is already
    # large enough that 2x2 samples per pixel is invisible), high for the tiny
    # ones where a half-pixel of stroke width decides legibility.
    scale = 8 if size <= 40 else (4 if size <= 64 else 2)
    c = Canvas(size, scale)

    tile, bbox = rounded_rect(0.02, 0.02, 0.98, 0.98, 0.23)
    c.paint(tile, lambda x, y: lerp(TILE_TOP, TILE_BOTTOM, y), bbox=bbox)
    if not compact:
        # top inner rim: the "pane of glass" read, only worth the pixels above 40
        rim, rbox = rounded_rect(0.055, 0.045, 0.945, 0.42, 0.19)
        c.paint(rim, lambda x, y: (1.0, 1.0, 1.0, 0.10 * (1.0 - (y - 0.045) / 0.5)), bbox=rbox)

    if compact:
        apex, bl, br = (0.40, 0.16), (0.13, 0.84), (0.67, 0.84)
        beam_a, beam_b = (0.02, 0.52), (0.30, 0.52)
        beam_r = 0.055
        fan_origin = (0.565, 0.585)
        fan_ys = (0.20, 0.42, 0.64, 0.86)
        fan_w = 0.070
        fan_x = 0.95
        stroke = 0.0
    else:
        apex, bl, br = (0.40, 0.13), (0.15, 0.83), (0.65, 0.83)
        beam_a, beam_b = (0.03, 0.52), (0.295, 0.52)
        beam_r = 0.032
        fan_origin = (0.556, 0.575)
        fan_ys = (0.165, 0.315, 0.465, 0.615, 0.765, 0.915)
        fan_w = 0.030
        fan_x = 0.955
        stroke = 0.012

    # fan first, so the prism body clips it: rays appear to leave the face
    for k, yend in enumerate(fan_ys):
        seg, sbox = capsule(fan_origin, (fan_x, yend), fan_w)
        c.paint(seg, SPECTRUM[k % len(SPECTRUM)], bbox=sbox)

    beam, bbox2 = capsule(beam_a, beam_b, beam_r)
    c.paint(beam, (1.0, 1.0, 1.0), bbox=bbox2)

    body, tbox = triangle(apex, bl, br)
    c.paint(body, lambda x, y: lerp(ICE_HI, ICE_LO, (y - apex[1]) / (bl[1] - apex[1])), bbox=tbox)
    if stroke:
        for a, b in ((apex, bl), (bl, br), (br, apex)):
            edge, ebox = capsule(a, b, stroke)
            c.paint(edge, (1.0, 1.0, 1.0, 0.55), bbox=ebox)

    data = c.resolve(size)
    if mutate == "blank-16" and size == 16:
        data = bytes(size * size * 4)          # silent blank frame
    return data


# ---------------------------------------------------------------------------
# encoders
# ---------------------------------------------------------------------------


def bmp_entry(rgba, size):
    """BITMAPINFOHEADER + 32bpp bottom-up BGRA + 1bpp AND mask."""
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                      size * size * 4, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        row = y * size * 4
        for x in range(size):
            o = row + x * 4
            xor += bytes((rgba[o + 2], rgba[o + 1], rgba[o + 0], rgba[o + 3]))
    stride = ((size + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        and_mask += bits
    return hdr + bytes(xor) + bytes(and_mask)


def png_entry(rgba, size):
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter: none
        raw += rgba[y * size * 4:(y + 1) * size * 4]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", binascii.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def build_ico(mutate=""):
    frames = []
    sizes = list(SIZES)
    if mutate == "drop-256":
        sizes.remove(256)
    for s in sizes:
        rgba = draw(s, mutate)
        frames.append((s, png_entry(rgba, s) if s == 256 else bmp_entry(rgba, s)))
    out = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    body = bytearray()
    for s, blob in frames:
        out += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s,
                           0, 0, 1, 32, len(blob), offset + len(body))
        body += blob
    return out + bytes(body)


# ---------------------------------------------------------------------------
# reader + guard
# ---------------------------------------------------------------------------


def read_ico(blob):
    reserved, kind, count = struct.unpack_from("<HHH", blob, 0)
    if reserved != 0 or kind != 1:
        raise ValueError("not an ICO (header %r)" % ((reserved, kind),))
    entries = []
    for i in range(count):
        w, h, colors, res, planes, bpp, nbytes, off = struct.unpack_from("<BBBBHHII", blob, 6 + 16 * i)
        entries.append({"size": 256 if w == 0 else w, "bpp": bpp,
                        "bytes": nbytes, "offset": off})
    return entries


def decode_entry(blob, entry):
    """Return RGBA rows top-down for one frame (PNG or 32bpp DIB)."""
    data = blob[entry["offset"]:entry["offset"] + entry["bytes"]]
    size = entry["size"]
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        pos, idat = 8, b""
        w = h = 0
        while pos < len(data):
            (n,) = struct.unpack_from(">I", data, pos)
            tag = data[pos + 4:pos + 8]
            payload = data[pos + 8:pos + 8 + n]
            pos += 12 + n
            if tag == b"IHDR":
                w, h = struct.unpack_from(">II", payload, 0)
            elif tag == b"IDAT":
                idat += payload
            elif tag == b"IEND":
                break
        if (w, h) != (size, size):
            raise ValueError("PNG frame %dx%d disagrees with directory size %d" % (w, h, size))
        raw = zlib.decompress(idat)
        stride = size * 4
        out = bytearray()
        for y in range(size):
            if raw[y * (stride + 1)] != 0:
                raise ValueError("unexpected PNG filter")
            out += raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        return bytes(out)
    bih = struct.unpack_from("<IiiHHIIiiII", data, 0)
    height = bih[2]
    if height != size * 2:
        raise ValueError("DIB height %d != 2*%d" % (height, size))
    xor_off = 40
    out = bytearray(size * size * 4)
    for y in range(size):
        src = xor_off + (size - 1 - y) * size * 4
        for x in range(size):
            b, g, r, a = data[src + x * 4: src + x * 4 + 4]
            o = (y * size + x) * 4
            out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)


def check(blob, preview_dir):
    problems = []
    try:
        entries = read_ico(blob)
    except ValueError as exc:
        print("ICON CHECK FAILED: %s" % exc)
        return 1
    sizes = [e["size"] for e in entries]
    print("frames: " + ", ".join("%dpx/%dbpp/%dB" % (e["size"], e["bpp"], e["bytes"]) for e in entries))
    for need in REQUIRED:
        if need not in sizes:
            problems.append("missing required size %dpx" % need)
    if sizes != sorted(sizes) or len(set(sizes)) != len(sizes):
        problems.append("frame table is not sorted/unique: %r" % (sizes,))
    for e in entries:
        if e["bpp"] != 32:
            problems.append("%dpx advertises %dbpp" % (e["size"], e["bpp"]))
        try:
            rgba = decode_entry(blob, e)
        except ValueError as exc:
            problems.append("%dpx does not decode: %s" % (e["size"], exc))
            continue
        opaque = sum(1 for i in range(3, len(rgba), 4) if rgba[i] > 200)
        coverage = opaque / (e["size"] * e["size"])
        if coverage < 0.45:
            problems.append("%dpx is %d%% opaque -- the tile would not read as a tile"
                            % (e["size"], round(coverage * 100)))
        chroma = sum(1 for i in range(0, len(rgba), 4)
                     if max(rgba[i:i + 3]) - min(rgba[i:i + 3]) > 60 and rgba[i + 3] > 200)
        if chroma < max(6, e["size"] * e["size"] // 200):
            problems.append("%dpx carries almost no spectrum (%d chromatic px)"
                            % (e["size"], chroma))
        if preview_dir:
            preview_dir.mkdir(parents=True, exist_ok=True)
            (preview_dir / ("prism-%d.png" % e["size"])).write_bytes(png_entry(rgba, e["size"]))
    if problems:
        print("ICON CHECK FAILED")
        for p in problems:
            print("  " + p)
        return 1
    print("ICON CHECK PASSED (%d frames, 16/32/48/256 present, all decodable, "
          "tile+spectrum coverage verified)" % len(entries))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="write prism.ico (default if no --check)")
    ap.add_argument("--check", action="store_true", help="validate the committed prism.ico")
    ap.add_argument("--preview-dir", type=Path, default=None)
    ap.add_argument("--mutate", default="",
                    help="negative control: drop-256 | blank-16 (must make --check fail)")
    args = ap.parse_args()
    if args.mutate:
        print("MUTATION ACTIVE: %s" % args.mutate)
        if args.mutate not in ("drop-256", "blank-16"):
            print("unknown mutation")
            return 2
    if args.check:
        blob = ICO_PATH.read_bytes()
        if args.mutate:
            blob = build_ico(args.mutate)
        return check(blob, args.preview_dir)
    blob = build_ico(args.mutate)
    ICO_PATH.write_bytes(blob)
    print("wrote %s (%d bytes)" % (ICO_PATH, len(blob)))
    return check(blob, args.preview_dir)


if __name__ == "__main__":
    sys.exit(main())
