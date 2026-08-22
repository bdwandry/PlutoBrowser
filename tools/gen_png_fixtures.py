#!/usr/bin/env python3
"""Generates selftest_png_fixtures.h — PNG test vectors with analytic
golden values for the P17 decoder port. Regenerate rather than edit."""
import zlib, struct

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + \
           struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

def png(ct, bd, w, h, idat_raw, plte=None, trns=None, interlace=False):
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bd, ct, 0, 0,
                                      1 if interlace else 0))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    out += chunk(b"IDAT", zlib.compress(idat_raw, 6))
    return out + chunk(b"IEND", b"")

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
    if pa <= pb and pa <= pc: return a
    if pb <= pc: return b
    return c

def filter_rows(rows, bpp):
    prev = bytearray(len(rows[0]))
    out = bytearray(); fi = 0
    for cur0 in rows:
        f = fi % 5; fi += 1
        cur = bytes(cur0); enc = bytearray(len(cur))
        for x in range(len(cur)):
            xv = cur[x]
            a = cur[x-bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x >= bpp else 0
            if f == 0: enc[x] = xv
            elif f == 1: enc[x] = (xv - a) & 0xFF
            elif f == 2: enc[x] = (xv - b) & 0xFF
            elif f == 3: enc[x] = (xv - ((a+b) >> 1)) & 0xFF
            else: enc[x] = (xv - paeth(a, b, c)) & 0xFF
        out.append(f); out += enc
        prev = bytearray(cur)
    return bytes(out)

def comp(g, a):
    if a >= 255: return g
    if a <= 0: return 255
    return int((g*a + 255*(255-a)) // 255 + 0.5)

def rgbgray(r, g, b): return (r*306 + g*601 + b*117) >> 10

FX = []      # (name, png_bytes)
PROBES = []  # (img_index, w, h, [(x, y, gray)])

def add(name, data, w, h, probes):
    FX.append((name, data))
    PROBES.append((len(FX)-1, w, h, probes))

# F_red: RGB solid red 64x48, filters cycle -> all outputs rgbToGray(255,0,0)=76
w, h = 64, 48
row = bytes([255, 0, 0]) * w
add("fx_red", png(2, 8, w, h, filter_rows([row]*h, 3)), w, h,
    [(0,0,76),(63,47,76),(31,23,76)])

# F_rgba: RGBA 40x30 black left half opaque, right half alpha 0
w, h = 40, 30
rows = []
for y in range(h):
    r = bytearray()
    for x in range(w):
        a = 255 if x < 20 else 0
        r += bytes([10, 200, 30, a])
    rows.append(bytes(r))
add("fx_rgba", png(6, 8, w, h, filter_rows(rows, 4)), w, h,
    [(5,15,rgbgray(10,200,30)),(19,29,rgbgray(10,200,30)),
     (20,15,255),(39,0,255)])

# F_pal: palette ct3 8bit 32x16, indices cycling 0..3 + tRNS [255,0,128,255]
plte = bytes([0,0,0, 255,255,255, 255,0,0, 0,255,0])
trns = bytes([255, 0, 128, 255])
exp = [comp(0,255), comp(255,0), comp(rgbgray(255,0,0),128),
       comp(0+255,255) and comp(149,255)]
exp = [0, 255, comp(76,128), comp(rgbgray(0,255,0),255)]
w, h = 32, 16
rows = []
for y in range(h):
    rows.append(bytes([(y+x) % 4 for x in range(w)]))
probes = []
for k, (px, py) in enumerate([(0,0),(1,1),(2,2),(3,3)]):
    probes.append((px, py, exp[(py+px) % 4]))
add("fx_pal", png(3, 8, w, h, filter_rows(rows, 1), plte=plte, trns=trns),
    w, h, probes)

# F_gray8: ct0 bd8 ramp 100x10, no downscale
w, h = 100, 10
rows = [bytes([x % 256 for x in range(w)]) for _ in range(h)]
add("fx_gray8", png(0, 8, w, h, filter_rows(rows, 1)), w, h,
    [(0,5,0),(50,5,50),(99,9,99)])

# F_gray1: ct0 bd1 checker 32x8 byte 0xAA -> bits MSB-first 1,0.. *255
w, h = 32, 8
rowbytes = 4
rows = [bytes(([0xAA] if (y % 2 == 0) else [0x55])*rowbytes) for y in range(h)]
# bit1 (MSB first) scales by 255 via integer div 255//1
pr = []
for yy in (0, 1):
    base = 0xAA if yy % 2 == 0 else 0x55
    for bx in range(rowbytes):
        for bit in range(8):
            v = ((base >> (7-bit)) & 1) * 255
            pr.append((yy*rowbytes*8//h if False else bx*8+bit, yy, v))
add("fx_gray1", png(0, 1, w, h, filter_rows(rows, 1)), w, h, pr[:8])

# F_ga: ct4 bd8 24x12 gray=x%256 alpha alternating 255/64 per column pair
w, h = 24, 12
rows = []
expGA = {}
for y in range(h):
    r = bytearray()
    for x in range(w):
        g = (x * 9) % 256
        a = 255 if x % 2 == 0 else 64
        expGA[(x,y)] = comp(g, a)
        r.append(g); r.append(a)
    rows.append(bytes(r))
add("fx_ga", png(4, 8, w, h, filter_rows(rows, 2)), w, h,
    [(0,0,expGA[(0,0)]),(1,11,expGA[(1,11)]),(22,5,expGA[(22,5)])])

# F_gray16: ct0 bd16 high-byte sampling 16x4
w, h = 16, 4
rows = []
for y in range(h):
    r = bytearray()
    for x in range(w):
        hi = (x*13+y*7) % 256
        r.append(hi); r.append(0xAB)
    rows.append(bytes(r))
add("fx_gray16", png(0, 16, w, h, filter_rows(rows, 2)), w, h,
    [(3,1,(3*13+7)%256),(15,3,(15*13+21)%256)])

# F_adam7: interlaced ct0 bd8 33x17 -> first pass grid ceil(33/8)=5 x ceil(17/8)=3
W, H = 33, 17
passes = [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
stream = bytearray()
for (x0,y0,dx,dy) in passes:
    pw = (W - x0 + dx - 1)//dx
    ph = (H - y0 + dy - 1)//dy
    if pw <= 0 or ph <= 0: continue
    rows = []
    for j in range(ph):
        rows.append(bytes([77]*pw))
    stream += filter_rows(rows, 1) if (x0,y0)==(0,0) else \
              (b"\x00" + bytes([77])*pw)*ph
first_vals = {}
for j, gy in enumerate(range(0, H, 8)):
    for i, gx in enumerate(range(0, W, 8)):
        first_vals[(i,j)] = (gx*3) % 256
rows = []
for j in range(len(range(0, H, 8))):
    rows.append(bytes(first_vals[(i,j)] for i in range(len(range(0, W, 8)))))
stream = filter_rows(rows, 1)
for (x0,y0,dx,dy) in passes[1:]:
    pw = (W - x0 + dx - 1)//dx
    ph = (H - y0 + dy - 1)//dy
    if pw <= 0 or ph <= 0: continue
    stream += (b"\x00" + bytes([66])*pw)*ph
sw, sh = (W+7)//8, (H+7)//8
add("fx_adam7", png(0, 8, W, H, bytes(stream), interlace=True), sw, sh,
    [(0,0,first_vals[(0,0)]),(4,2,first_vals[(4,2)]),(2,1,first_vals[(2,1)])])

# F_bench: 800x600 RGB deterministic gradient (benchmark + downscale dims)
W, H = 800, 600
rows = []
for y in range(H):
    r = bytearray()
    for x in range(W):
        r.append(x % 256); r.append(y % 256); r.append((x+y) % 256)
    rows.append(bytes(r))
data = png(2, 8, W, H, filter_rows(rows, 3))
FX.append(("fx_bench", data))
PROBES.append((len(FX)-1, 266, 200, []))   # dims only: box 3x3 -> floor(800/3)=266

# F_down: 100x80 solid 200 with maxW/maxH 50/40 -> box 2x2 all-200 output
w, h = 100, 80
rows = [bytes([200])*w for _ in range(h)]
FX.append(("fx_down", png(0, 8, w, h, filter_rows(rows, 1))))
PROBES.append((len(FX)-1, 50, 40, [(25,20,200)]))

# ── emit header ──────────────────────────────────────────────────────────
out = ["// selftest_png_fixtures.h -- generated PNG vectors + golden probes",
       "// (regenerate via tools/gen_png_fixtures.py; do not hand-edit).",
       "#ifndef PLUTO_SELFTEST_PNG_FIXTURES_H",
       "#define PLUTO_SELFTEST_PNG_FIXTURES_H", ""]
def arr(name, blob):
    L = [f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(blob), 20):
        L.append("    " + ",".join(str(b) for b in blob[i:i+20]) + ",")
    L.append("};")
    L.append(f"#define {name.upper()}_LEN {len(blob)}")
    return L
for name, blob in FX:
    out += arr(name, blob)
    print(f"{name}: {len(blob)} bytes")
out.append("")
out.append("typedef struct { int img, tw, th; struct { int x, y, g; } p[8]; "
           "int nProbes; } PngProbe;")
out.append("")
out.append("static const PngProbe png_probes[] = {")
for idx, tw, th, ps in PROBES:
    items = ", ".join(f"{{{x},{y},{g}}}" for x, y, g in ps[:8])
    out.append(f"    {{{idx}, {tw}, {th}, {{{items}}}, {len(ps[:8])}}},")
out.append("};")
out.append("#define PNG_PROBES_LEN " +
           str(sum(1 for _ in PROBES)))
out += ["", "#endif"]
open("src/render/decoders/selftest_png_fixtures.h", "w").write("\n".join(out) + "\n")
print("header written,", len(PROBES), "probe sets")
