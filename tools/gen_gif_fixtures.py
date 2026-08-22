#!/usr/bin/env python3
"""Generate GIF test fixtures + golden probes for selftest_gif.c.

Includes a small GIF LZW encoder (LSB-first packing, dictionary growth
mirroring the decoder: widen when nextCode >= maxCode after adding).
"""

import sys

def gray(r, g, b):
    return (r * 306 + g * 601 + b * 117) >> 10

def u16(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF))

# ---------------------------------------------------------------- LZW

class BitPacker:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, code, size):
        self.acc |= code << self.n
        self.n += size
        while self.n >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def bytes(self):
        if self.n > 0:
            self.buf.append(self.acc & 0xFF)
        return bytes(self.buf)

def lzw_encode(pixels, min_code):
    """GIF LZW encode. Code-width changes shadow the DECODER's schedule
    exactly: the decoder adds one dictionary entry per data code read,
    starting with the SECOND, and widens right after adding when the
    table fills -- so the encoder widens after emitting each data code
    except the first."""
    clear = 1 << min_code
    end = clear + 1

    seqs = {(i,): i for i in range(clear)}   # real dictionary
    nextc = end + 1                           # real next entry id
    cs = min_code + 1                         # width used NOW
    shadow_next = end + 1                     # decoder-side mirror
    shadow_max = 1 << cs
    bp = BitPacker()

    bp.write(clear, cs)
    cur = (pixels[0],)
    first_data = True
    for px in pixels[1:]:
        cand = cur + (px,)
        hit = cand in seqs and len(cand) <= 64
        if hit:
            cur = cand
            continue
        bp.write(seqs[cur], cs)
        # shadow advance: one entry per data code except the first
        if not first_data:
            shadow_next += 1
            if shadow_next >= shadow_max and cs < 12:
                cs += 1
                shadow_max = 1 << cs
        else:
            first_data = False
        # real dictionary growth (may lag shadow harmlessly)
        if nextc < 4096:
            seqs[cand] = nextc
            nextc += 1
        cur = (px,)
    bp.write(seqs[cur], cs)
    bp.write(end, cs)
    return bp.bytes()

def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)

# ---------------------------------------------------------------- file

def pal_bytes(colors):
    """pad to power-of-two >= len(colors); entries BGR triples"""
    n = 2
    while n < len(colors):
        n *= 2
    s = 0
    while (1 << (s + 1)) < n:
        s += 1
    out = bytearray()
    for i in range(n):
        r, g, b = colors[i] if i < len(colors) else (0, 0, 0)
        out += bytes((r, g, b))
    return bytes(out), s   # packed&7 value: entry count == 1 << (s+1)

def frame(left, top, w, h, indices, min_code,
          lct=None, interlaced=False, emit_order=None):
    """indices: rows top-down. emit_order: row list to feed encoder
    (interlace pass order). Returns image block bytes."""
    img_packed = 0
    lct_b = b""
    if lct is not None:
        lct_b, sbits = pal_bytes(lct)
        img_packed |= 0x80 | sbits
    if interlaced:
        img_packed |= 0x40
    blk = bytearray()
    blk.append(0x2C)
    blk += u16(left) + u16(top) + u16(w) + u16(h)
    blk.append(img_packed)
    blk += lct_b
    blk.append(min_code)
    order = emit_order if emit_order is not None else range(h)
    stream = []
    for r in order:
        stream.extend(indices[r])
    blk += sub_blocks(lzw_encode(stream, min_code))
    return bytes(blk)

def gif_file(sig, w, h, gct=None, blocks=b""):
    out = bytearray(sig)
    packed = 0
    gct_b = b""
    if gct is not None:
        gct_b, sbits = pal_bytes(gct)
        packed |= 0x80 | sbits
    out += u16(w) + u16(h) + bytes((packed, 0, 0))
    out += gct_b
    out += blocks
    out.append(0x3B)
    return bytes(out)

def gce(transparent_index=None):
    if transparent_index is None:
        return b""
    return bytes((0x21, 0xF9, 4, 0x01, 0, 0, transparent_index, 0))

# ---------------------------------------------------------------- fixtures

fixtures = {}
probes = []

def add(name, data, idx, tw, th, points):
    fixtures[name] = data
    probes.append((name, idx, tw, th, points))

# 0: fx_g_basic — 40x30 screen, global palette 4, adaptive LZW.
PAL4 = [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 0, 255)]
G4 = [gray(*c) for c in PAL4]
W, H = 40, 30
rows = []
for y in range(H):
    r = []
    for x in range(W):
        if y == 0:
            r.append(1)          # white top
        elif y == H - 1:
            r.append(0)          # black bottom
        elif x < W // 2:
            r.append(2)          # red
        else:
            r.append(3)          # blue
    rows.append(r)
exp = lambda x, y: G4[rows[y][x]]
blk = frame(0, 0, W, H, rows, 2)
add("g_basic", gif_file(b"GIF87a", W, H, gct=PAL4, blocks=blk),
    0, W, H,
    [(0, 0, exp(0, 0)), (39, 0, exp(39, 0)), (10, 15, exp(10, 15)),
     (30, 15, exp(30, 15)), (5, 29, exp(5, 29)), (39, 29, exp(39, 29))])

# 1: fx_g_offset — small frame at left offset inside larger screen;
#    NOTE source composites rows starting at canvas row 0 (imgTop unused)
W, H = 30, 20
FW, FH = 8, 8
frows = [[(x + y) % 2 for x in range(FW)] for y in range(FH)]
LPAL2 = [(200, 100, 50), (50, 100, 200)]
LG = [gray(*c) for c in LPAL2]
expx = lambda x, y: LG[frows[y][x - 12]] if (12 <= x < 20 and y < FH) else 255
blk = frame(12, 6, FW, FH, frows, 1, lct=LPAL2)
# NOTE: source streams only imgH frame rows into the downscaler (created
# with logical-screen dims), so output height == FH, not H.
add("g_offset", gif_file(b"GIF87a", W, H, gct=[(0, 0, 0)],
                         blocks=blk),
    1, W, FH,
    [(0, 0, 255), (29, 7, 255), (12, 3, expx(12, 3)),
     (19, 7, expx(19, 7)), (11, 5, 255)])

# 2: fx_g_trans — transparency index renders as white
W, H = 20, 10
rows = [[(x % 2) for x in range(W)] for _ in range(H)]  # idx1=transparent
TPAL = [(0, 0, 0), (255, 0, 0)]
TG = gray(*TPAL[0])
expt = lambda x: TG if x % 2 == 0 else 255
blk = gce(1) + frame(0, 0, W, H, rows, 2)
add("g_trans", gif_file(b"GIF89a", W, H, gct=TPAL, blocks=blk),
    2, W, H,
    [(0, 5, expt(0)), (1, 5, expt(1)), (19, 9, expt(19)),
     (18, 0, expt(18))])

# 3: fx_g_inter — interlaced 16x16; encoder feeds rows in pass order
W, H = 16, 16
irows = [[(x * 3 + y * 5) % 2 for x in range(W)] for y in range(H)]
order = []
for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)):
    for r in range(start, H, step):
        order.append(r)
IPAL = [(0, 255, 0), (255, 255, 0)]
IG = [gray(*c) for c in IPAL]
expi = lambda x, y: IG[irows[y][x]]
pts = [(0, 0, expi(0, 0)), (15, 0, expi(15, 0)), (0, 4, expi(0, 4)),
       (7, 7, expi(7, 7)), (3, 13, expi(3, 13)), (15, 15, expi(15, 15))]
blk = frame(0, 0, W, H, irows, 1, interlaced=True, emit_order=order)
add("g_inter", gif_file(b"GIF87a", W, H, gct=IPAL, blocks=blk),
    3, W, H, pts)

# 4: fx_g_multi — two frames; only the first decodes
W, H = 24, 12
r1 = [[1 if x < W // 2 else 0 for x in range(W)] for _ in range(H)]
r2 = [[0] * W for _ in range(H)]           # would be all-black
MPAL = [(0, 0, 0), (255, 255, 255)]
MG = gray(*MPAL[1])
expm = lambda x: MG if x < W // 2 else 0
blk = frame(0, 0, W, H, r1, 2) + frame(0, 0, W, H, r2, 2)
add("g_multi", gif_file(b"GIF89a", W, H, gct=MPAL, blocks=blk),
    4, W, H,
    [(0, 6, expm(0)), (11, 6, expm(11)), (12, 6, expm(12)),
     (23, 11, expm(23))])

# 5: fx_g_bench — 600x400 screen, 256-gray palette, downscale to 300x200
W, H = 600, 400
BPAL = [(k, k, k) for k in range(256)]     # gray(k,k,k) == k exactly
rows = [[((x // 7) + (y // 5)) % 256 for x in range(W)]
        for y in range(H)]
def expb(ox, oy):
    # box-filtered 2x2 average, floor(mean + 0.5) like ScaleAccum
    tot = 0
    for sy in (oy * 2, oy * 2 + 1):
        for sx in (ox * 2, ox * 2 + 1):
            tot += ((sx // 7) + (sy // 5)) % 256
    return int(tot / 4 + 0.5)   # tot%4==0 or 1 here; matches C float math
blk = frame(0, 0, W, H, rows, 8)
add("g_bench", gif_file(b"GIF89a", W, H, gct=BPAL, blocks=blk),
    5, 300, 200,
    [(0, 0, expb(0, 0)), (299, 199, expb(299, 199)),
     (150, 100, expb(150, 100)), (77, 33, expb(77, 33))])

# ---------------------------------------------------------------- emit

out = []
out.append("// Generated by tools/gen_gif_fixtures.py — do not edit.\n")
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

out.append("typedef struct { int x, y, g; } GifPoint;")
out.append("typedef struct { int img, tw, th; "
           "GifPoint p[8]; int nProbes; } GifProbe;\n")
out.append(f"#define GIF_PROBES_LEN {len(probes)}")
out.append("static const GifProbe gif_probes[] = {")
for (name, idx, tw, th, pts) in probes:
    pp = ", ".join(f"{{{x}, {y}, {g}}}" for (x, y, g) in pts)
    out.append(f"    {{{idx}, {tw}, {th}, {{{pp}}}, {len(pts)}}},"
               f" /* {name} */")
out.append("};\n")

dest = sys.argv[1] if len(sys.argv) > 1 else \
    "src/render/decoders/selftest_gif_fixtures.h"
with open(dest, "w") as f:
    f.write("\n".join(out))
print(f"wrote {dest}: {len(fixtures)} fixtures, "
      f"{sum(len(d) for d in fixtures.values())} bytes total")
