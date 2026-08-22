#!/usr/bin/env python3
"""Generate BMP test fixtures + golden probes for selftest_bmp.c.

Mirrors the decoder contract of Source/render/decoders/bmp.lua:
nearest-neighbor sampling with float scale, bottom-up default,
top-down via negative height, short-palette zero-fill quirk.
"""

import sys

def gray(r, g, b):
    return (r * 306 + g * 601 + b * 117) >> 10

# ---------------------------------------------------------------- writers

def u16(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF))

def u32(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF,
                  (v >> 24) & 0xFF))

def s32(v):
    return u32(v & 0xFFFFFFFF)

def bmp(w, h, bpp, pix_rows, palette=None, top_down=False,
        compression=0):
    """pix_rows[y][x] already packed as ints (palette idx or 0xBBGGRR
    for truecolor). Rows are given in TOP-DOWN order."""
    pal = bytearray()
    if palette:
        for (r, g, b) in palette:
            pal += bytes((b, g, r, 0))
    pixel_offset = 14 + 40 + len(pal)
    row_bytes = ((bpp * w + 31) // 32) * 4
    body = bytearray()
    order = range(h) if top_down else range(h - 1, -1, -1)
    for y in order:
        row = pix_rows[y]
        buf = bytearray(row_bytes)
        if bpp == 24:
            for x in range(w):
                p = row[x]
                buf[x * 3] = p & 0xFF
                buf[x * 3 + 1] = (p >> 8) & 0xFF
                buf[x * 3 + 2] = (p >> 16) & 0xFF
        elif bpp == 32:
            for x in range(w):
                p = row[x]
                buf[x * 4] = p & 0xFF
                buf[x * 4 + 1] = (p >> 8) & 0xFF
                buf[x * 4 + 2] = (p >> 16) & 0xFF
                buf[x * 4 + 3] = 0xAA          # alpha ignored by decoder
        else:
            for x in range(w):
                buf[x * bpp // 8] |= row[x] << (8 - bpp - (x * bpp) % 8)
        body += buf
    raw_h = -h if top_down else h
    out = bytearray()
    out += b"BM"
    out += u32(pixel_offset + len(body))
    out += u16(0) + u16(0)
    out += u32(pixel_offset)
    out += u32(40)
    out += s32(w) + s32(raw_h)
    out += u16(1) + u16(bpp)
    out += u32(compression)
    out += u32(len(body)) + u32(0) + u32(0) + u32(0) + u32(0)
    assert len(out) == 54
    out += pal
    out += body
    return bytes(out)

# ---------------------------------------------------------------- fixtures

fixtures = {}   # name -> bytes
probes = []     # (img_idx, tw, th, [(x, y, g), ...])

def add(name, data, img_idx, tw, th, points):
    fixtures[name] = data
    probes.append((name, img_idx, tw, th, points))

# 0: fx_b24 — 24bpp bottom-up gradient
W, H = 64, 48
rows = [[((x * 4) % 256) << 16 | 64 << 8 | 32 for x in range(W)]
        for y in range(H)]
g_of = lambda x, y: gray((x * 4) % 256, 64, 32)
add("b24", bmp(W, H, 24, rows), 0, W, H,
    [(0, 0, g_of(0, 0)), (W - 1, 0, g_of(W - 1, 0)),
     (0, H - 1, g_of(0, H - 1)), (W - 1, H - 1, g_of(W - 1, H - 1)),
     (W // 2, H // 2, g_of(W // 2, H // 2))])

# 1: fx_b32 — 32bpp top-down (negative height), alpha byte ignored
W, H = 20, 10
solid = gray(10, 200, 90)
rows = [[10 << 16 | 200 << 8 | 90 for _ in range(W)] for _ in range(H)]
add("b32", bmp(W, H, 32, rows, top_down=True), 1, W, H,
    [(0, 0, solid), (W - 1, H - 1, solid), (5, 5, solid)])

# 2: fx_b8 — 8bpp with SHORT palette (only 4 entries on disk);
#    decoder builds all 256 entries, indices beyond -> 0 (quirk)
W, H = 16, 16
pal4 = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 0, 255)]
pal_gray = [gray(*c) for c in pal4]
pattern = []
for y in range(H):
    r = []
    for x in range(W):
        r.append((x + y) % 4)
    pattern.append(r)
pattern[3][7] = 200                       # far beyond palette -> 0
exp8 = lambda i: pal_gray[i] if i < 4 else 0
pts = [(0, 0, exp8(pattern[0][0])), (15, 15, exp8(pattern[15][15])),
       (7, 3, exp8(pattern[3][7]))]
add("b8", bmp(W, H, 8, pattern, palette=pal4), 2, W, H, pts)

# 3: fx_b4 — 4bpp nibbles, full palette where entry k has gray k*17
W, H = 12, 8
pal16 = [(k * 17, k * 17, k * 17) for k in range(16)]
rows = [[(x * 13 + y * 5) % 16 for x in range(W)] for y in range(H)]
gb4 = lambda x, y: ((rows[y][x]) * 17)
add("b4", bmp(W, H, 4, rows, palette=pal16), 3, W, H,
    [(0, 0, gb4(0, 0)), (11, 7, gb4(11, 7)), (6, 3, gb4(6, 3)),
     (1, 0, gb4(1, 0)), (2, 0, gb4(2, 0))])

# 4: fx_b1 — 1bpp checker, MSB-first
W, H = 24, 6
rows = [[(1 if (x % 2 == 0) else 0) for x in range(W)] for y in range(H)]
pal2 = [(0, 0, 0), (255, 255, 255)]
g1 = lambda x: 255 if x % 2 == 0 else 0
add("b1", bmp(W, H, 1, rows, palette=pal2), 4, W, H,
    [(0, 0, g1(0)), (1, 0, g1(1)), (23, 5, g1(23)), (22, 5, g1(22))])

# 5: fx_wide — 400x100 forces scale = 400/360 -> 360x90
W, H = 400, 100
gsolid = gray(200, 200, 200)
rows = [[200 << 16 | 200 << 8 | 200 for _ in range(W)] for _ in range(H)]
add("wide", bmp(W, H, 24, rows), 5, 360, 90,
    [(0, 0, gsolid), (359, 89, gsolid), (100, 50, gsolid)])

# 6: fx_bench — 600x400 gradient -> scale 2 -> 300x200
W, H = 600, 400
rows = [[(x % 256) << 16 | (y % 256) << 8 | ((x + y) % 256)
         for x in range(W)] for y in range(H)]
gbench = lambda x, y: gray(x % 256, y % 256, (x + y) % 256)
add("bmp_bench", bmp(W, H, 24, rows), 6, 300, 200,
    [(0, 0, gbench(0, 0)), (299, 199, gbench(598, 398)),
     (150, 100, gbench(300, 200)), (10, 20, gbench(20, 40))])

# ---------------------------------------------------------------- emit

out = []
out.append("// Generated by tools/gen_bmp_fixtures.py — do not edit.\n")
for name, data in fixtures.items():
    out.append(f"static const unsigned char fx_{name}[] = {{")
    line = "    "
    for b in data:
        tok = f"0x{b:02X},"
        if len(line) + len(tok) > 78:
            out.append(line)
            line = "    "
        line += tok
    if line.strip():
        out.append(line)
    out.append("};")
    out.append(f"#define FX_{name.upper()}_LEN {len(data)}\n")

out.append("typedef struct { int x, y, g; } BmpPoint;")
out.append("typedef struct { int img, tw, th; "
           "BmpPoint p[8]; int nProbes; } BmpProbe;\n")
out.append(f"#define BMP_PROBES_LEN {len(probes)}")
out.append("static const BmpProbe bmp_probes[] = {")
for (name, idx, tw, th, pts) in probes:
    pp = ", ".join(f"{{{x}, {y}, {g}}}" for (x, y, g) in pts)
    out.append(f"    {{{idx}, {tw}, {th}, {{{pp}}}, {len(pts)}}},"
               f" /* {name} */")
out.append("};\n")

dest = sys.argv[1] if len(sys.argv) > 1 else \
    "src/render/decoders/selftest_bmp_fixtures.h"
with open(dest, "w") as f:
    f.write("\n".join(out))
print(f"wrote {dest}: {len(fixtures)} fixtures, "
      f"{sum(len(d) for d in fixtures.values())} bytes total")
