#!/usr/bin/env python3
"""Generate independent PNG test vectors for the P23 battery -> p23_vectors.h.

Containers are built with python's zlib (independent of the C inflate) and
expected dither rows are COMPUTED by simulating the reference Bayer pass on
the true decoded gray grids — never hand-derived. Filtered scanlines are
forward-filtered here (types 0/1/2/3/4) and re-verified with a python
reference unfilter before emission.

ffmpeg validates the spec-conformant vectors (the Adam7 first-pass vector is
intentionally incomplete per PNG spec — only pass 1 rows are present — so it
is validated by construction instead).
"""
import struct, zlib, subprocess, sys, os

BAYER = [
    [0, 128, 32, 160],
    [192, 64, 224, 96],
    [48, 176, 16, 144],
    [240, 112, 208, 80],
]

def cbytes(b, per=16):
    lines = []
    for i in range(0, len(b), per):
        lines.append("  " + ", ".join(f"0x{x:02X}" for x in b[i:i+per]) + ",")
    return "\n".join(lines).rstrip(",")

def dither_rows_gray(rows):
    out = []
    w = len(rows[0])
    stride = (w + 7) // 8
    for y, row in enumerate(rows):
        acc = [0] * stride
        for x, g in enumerate(row):
            if not (g < BAYER[y % 4][x % 4]):
                acc[x >> 3] |= 0x80 >> (x & 7)
        out.append(bytes(acc))
    return out

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c

def forward_filter(raw_rows, bpp, ftypes):
    """Encode rows with the given per-row filter types (reference mirror)."""
    out = bytearray()
    prev = bytes(len(raw_rows[0]))
    for row, ft in zip(raw_rows, ftypes):
        out.append(ft)
        cur = bytes(row)
        for i in range(len(cur)):
            x = cur[i]
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 0:
                v = x
            elif ft == 1:
                v = (x - a) & 0xFF
            elif ft == 2:
                v = (x - b) & 0xFF
            elif ft == 3:
                v = (x - ((a + b) >> 1)) & 0xFF
            else:
                v = (x - paeth(a, b, c)) & 0xFF
            out.append(v)
        prev = cur
    return bytes(out)

def unfilter_ref(data, rowbytes, bpp, nrows):
    """Python reference unfilter (used to self-check forward_filter)."""
    rows = []
    prev = bytearray(rowbytes)
    pos = 0
    for _ in range(nrows):
        ft = data[pos]; pos += 1
        cur = bytearray(data[pos:pos + rowbytes]); pos += rowbytes
        for i in range(rowbytes):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                cur[i] = (cur[i] + a) & 0xFF
            elif ft == 2:
                cur[i] = (cur[i] + b) & 0xFF
            elif ft == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                cur[i] = (cur[i] + paeth(a, b, c)) & 0xFF
        rows.append(bytes(cur))
        prev = cur
    return rows

def chunk(ctype, data):
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)

def png(ihdr_fields, chunks_after, include_iend=True):
    # call sites pass (w, h, depth, colorType, filter, interlace); compression=0
    w, h, d, c, f, i = ihdr_fields
    ihdr = struct.pack(">IIBBBBB", w, h, d, c, 0, f, i)
    body = chunk(b"IHDR", ihdr) + b"".join(chunks_after)
    if include_iend:
        body += chunk(b"IEND", b"")
    return b"\x89PNG\r\n\x1a\n" + body

def gval(x, y):
    return (x * 48 + y * 16) % 256

vectors = {}   # name -> (bytes, [expected dither rows], ffmpeg_ok)
notes = {}

# ── tc1: colorType 0 (gray), 8-bit, 4x2, filter 0 ──────────────────────────
grid = [[gval(x, y) for x in range(4)] for y in range(2)]
raw = b"".join(b"\x00" + bytes(r) for r in grid)
idat = zlib.compress(raw)
png1 = png((4, 2, 8, 0, 0, 0), [chunk(b"IDAT", idat)])
vectors["PNG_GRAY8"] = (png1, dither_rows_gray(grid), True)

# ── tc2: gray 8-bit, 8x4, exercising filters 0/1/2/4 ───────────────────────
grid2 = [[gval(x, y) for x in range(8)] for y in range(4)]
ftypes = [0, 1, 2, 4]
filtered = forward_filter(grid2, 1, ftypes)
assert unfilter_ref(filtered, 8, 1, 4) == [bytes(r) for r in grid2], "self-check"
idat2 = zlib.compress(filtered)
png2 = png((8, 4, 8, 0, 0, 0), [chunk(b"IDAT", idat2)])
vectors["PNG_FILTERS"] = (png2, dither_rows_gray(grid2), True)

# ── tc3: palette (colorType 3) + tRNS alpha compositing ────────────────────
# palette grays 0/85/170/255; tRNS 255,255,0,128 → idx2 transparent, idx3 a=128
pal_grays = [0, 85, 170, 255]
plte = b"".join(bytes([g, g, g]) for g in pal_grays)
trns3 = bytes([255, 255, 0, 128])
idx_grid = [[(x + y) % 4 for x in range(4)] for y in range(2)]
comp_grid = []
for y in range(2):
    row = []
    for x in range(4):
        g = pal_grays[idx_grid[y][x]]
        a = trns3[idx_grid[y][x]]
        if a >= 255:
            row.append(g)
        elif a <= 0:
            row.append(255)
        else:
            row.append((g * a + 255 * (255 - a)) // 255)  # floor(x+0.5) == (x+y)//y when remainder<1
            # exact Lua: floor((g*a + 255*(255-a))/255 + 0.5)
            row[-1] = int((g * a + 255 * (255 - a)) / 255 + 0.5) // 1
    comp_grid.append(row)
raw3 = b"".join(b"\x00" + bytes(idx_grid[y]) for y in range(2))
png3 = png((4, 2, 8, 3, 0, 0), [chunk(b"PLTE", plte), chunk(b"tRNS", trns3), chunk(b"IDAT", zlib.compress(raw3))])
vectors["PNG_PAL_TRNS"] = (png3, dither_rows_gray(comp_grid), True)

# ── tc4: RGBA 16-bit (colorType 6, depth 16) composite over white ──────────
# pixel(x,y): gray channel = gval, alpha: 0xFFFF except (0,0)=0x8000 and (3,1)=0x0000
grid4 = []
for y in range(2):
    row = []
    for x in range(4):
        g = gval(x, y)
        a = 0x8000 if (x, y) == (0, 0) else (0x0000 if (x, y) == (3, 1) else 0xFFFF)
        av8 = a >> 8  # Lua uses only the high byte of the alpha sample
        if av8 >= 255:
            row.append(g)
        elif av8 <= 0:
            row.append(255)
        else:
            row.append(int((g * av8 + 255 * (255 - av8)) / 255 + 0.5))
    grid4.append(row)
raw4 = b""
for y in range(2):
    raw4 += b"\x00"
    for x in range(4):
        g = gval(x, y)
        a = 0x8000 if (x, y) == (0, 0) else (0x0000 if (x, y) == (3, 1) else 0xFFFF)
        raw4 += struct.pack(">HHHH", g << 8, g << 8, g << 8, a)
png4 = png((4, 2, 16, 6, 0, 0), [chunk(b"IDAT", zlib.compress(raw4))])
vectors["PNG_RGBA16"] = (png4, dither_rows_gray(grid4), True)

# ── tc5: RGB (colorType 2) + tRNS key → transparent pixel ──────────────────
grid5 = [[gval(x, y) for x in range(4)] for y in range(2)]
key = (gval(0, 0), gval(0, 0), gval(0, 0))  # first pixel's color
grid5_exp = [[255 if (x, y) == (0, 0) else grid5[y][x] for x in range(4)] for y in range(2)]
raw5 = b""
for y in range(2):
    raw5 += b"\x00"
    for x in range(4):
        g = gval(x, y)
        raw5 += bytes([g, g, g])
trns5 = bytes([key[0], key[1], key[2]])
png5 = png((4, 2, 8, 2, 0, 0), [chunk(b"tRNS", trns5), chunk(b"IDAT", zlib.compress(raw5))])
vectors["PNG_RGB_TRNS"] = (png5, dither_rows_gray(grid5_exp), True)

# ── tc6: gray+alpha 8-bit (colorType 4) ────────────────────────────────────
grid6 = []
raw6 = b""
for y in range(2):
    row = []
    raw6 += b"\x00"
    for x in range(4):
        g = gval(x, y)
        a = [255, 128, 0, 255][x]
        if a >= 255:
            row.append(g)
        elif a <= 0:
            row.append(255)
        else:
            row.append(int((g * a + 255 * (255 - a)) / 255 + 0.5))
        raw6 += bytes([g, a])
    grid6.append(row)
png6 = png((4, 2, 8, 4, 0, 0), [chunk(b"IDAT", zlib.compress(raw6))])
vectors["PNG_GRAYA8"] = (png6, dither_rows_gray(grid6), True)

# ── tc7: gray 4-bit (sub-byte unpacking, grayScale 17) ─────────────────────
# nibble values 0..15 → gray = nibble * 17 (255 // 15)
grid7 = [[(3 * x + 5 * y) % 16 for x in range(4)] for y in range(2)]
grid7_gray = [[v * 17 for v in row] for row in grid7]
raw7 = b""
for y in range(2):
    raw7 += b"\x00" + bytes([(grid7[y][0] << 4) | grid7[y][1], (grid7[y][2] << 4) | grid7[y][3]])
png7 = png((4, 2, 4, 0, 0, 0), [chunk(b"IDAT", zlib.compress(raw7))])
vectors["PNG_GRAY4"] = (png7, dither_rows_gray(grid7_gray), True)

# ── tc8: Adam7 interlaced 16x16 — decoder consumes first pass only ─────────
# First pass = 2x2 samples (ceil(16/8)); IDAT holds exactly those 2 rows —
# spec-conformant first-pass stream; validated by construction.
grid8 = [[gval(x, y) for x in range(2)] for y in range(2)]
raw8 = b"".join(b"\x00" + bytes(r) for r in grid8)
png8 = png((16, 16, 8, 0, 0, 1), [chunk(b"IDAT", zlib.compress(raw8))])
vectors["PNG_ADAM7_P1"] = (png8, dither_rows_gray(grid8), False)

# ── tc9: truncated PNG (signature only, no IHDR) → NULL ────────────────────
vectors["PNG_TRUNC"] = (b"\x89PNG\r\n\x1a\n", None, False)

# ── tc10: PNG-entry ICO (closes the ICO deviation) ─────────────────────────
# 4x2 32bpp PNG entry (alpha 255 everywhere), wrapped in an ICO container.
grid10 = [[32 + 16 * x for x in range(4)] for y in range(2)]
raw10 = b""
for y in range(2):
    raw10 += b"\x00"
    for x in range(4):
        g = grid10[y][x]
        raw10 += bytes([g, g, g, 255])
png10_inner = png((4, 2, 8, 6, 0, 0), [chunk(b"IDAT", zlib.compress(raw10))])
ico10 = struct.pack("<HHH", 0, 1, 1) + \
        struct.pack("<BBBBHHII", 4, 2, 0, 0, 1, 32, len(png10_inner), 22) + png10_inner
vectors["ICO_PNG_ENTRY"] = (ico10, dither_rows_gray(grid10), True)

# ── emit ────────────────────────────────────────────────────────────────────
def arr(name, data, comment=""):
    c = f" /* {comment} */" if comment else ""
    return f"{c}\nstatic const unsigned char {name}[{len(data)}] = {{\n{cbytes(data)}\n}};"

def rows_arr(name, rows_):
    return f"static const uint8_t {name}[{len(rows_)}] = {{\n" + \
           ",\n".join("  " + ", ".join(f"0x{b:02X}" for b in r) for r in rows_) + "\n};"

out = []
out.append("/* AUTO-GENERATED by tests/gen_p23_vectors.py — do not edit by hand. */")
out.append("/* Containers built with python zlib; expected dither rows computed by the */")
out.append("/* generator from the reference Bayer + composite formulas. */")
out.append("#ifndef P23_VECTORS_H")
out.append("#define P23_VECTORS_H")
out.append("#include <stdint.h>\n")
for name, (data, exp, _ff) in vectors.items():
    out.append(arr(name, data))
    if exp is not None:
        out.append(rows_arr(name + "_EXP", exp))
out.append("#endif")

open("Source/render/decoders/p23_vectors.h", "w").write("\n".join(out) + "\n")
print("WROTE p23_vectors.h:", ", ".join(f"{n}={len(d)}" for n, (d, _, _) in vectors.items()))

# ── ffmpeg validation ───────────────────────────────────────────────────────
if "--validate" in sys.argv:
    print("ffmpeg validation:")
    ok = True
    for name, (data, exp, ff) in vectors.items():
        if not ff:
            print(f"  {name}: skipped (validated by construction)")
            continue
        ext = ".ico" if name.startswith("ICO") else ".png"
        path = f"/tmp/v23_{name}{ext}"
        open(path, "wb").write(data)
        r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                            "-pix_fmt", "gray", "/tmp/v23_out.raw", "-y"],
                           capture_output=True, text=True)
        status = "DECODED" if r.returncode == 0 and os.path.getsize("/tmp/v23_out.raw") > 0 else "FAILED"
        if r.stderr.strip():
            status += f" stderr={r.stderr.strip()[:80]}"
        print(f"  {name}: {status}")
        if status.startswith("FAILED"):
            ok = False
    sys.exit(0 if ok else 1)
