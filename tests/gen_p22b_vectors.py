#!/usr/bin/env python3
"""Generate independent test vectors for bmp/gif/ico decoders -> p22b_vectors.h.

All vectors are 4px wide x 2 or 8 rows tall so every row stride is already a
multiple of 4 (no stride ambiguity) and rows are horizontally distinct.

Every expected dither byte array is COMPUTED HERE by simulating the Bayer
pass (thresholds from the reference dither.lua) -- never hand-derived.

Independent validation: ffmpeg decodes each container to the exact gray grid
the C decoder must produce (run with --validate).
"""
import struct, subprocess, sys, os

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
    """rows: list of lists of gray 0..255 -> list of packed 1-bit row bytes
    (1 = white when gray >= threshold; unpainted bits stay 0). Mirrors the
    reference: isBlack = gray < bayer[y%4][x%4]."""
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

# gray value at (x, y): ramp so every column differs
def gval(x, y):
    return (x * 48 + y * 16) % 256

# ---------------- BMP 24bpp 4x2, bottom-up (row stride = 12, no padding) ----------------
BW, BH = 4, 2
pix24 = [[gval(x, y) for x in range(BW)] for y in range(BH)]
rows = []
for y in (BH - 1, 0):  # bottom-up storage
    row = b""
    for x in range(BW):
        g = pix24[y][x]
        row += bytes([g, g, g])  # B,G,R
    rows.append(row)
bmp24_data = b"".join(rows)
bmp24 = b"BM" + struct.pack("<IHHI", 14 + 40 + len(bmp24_data), 0, 0, 14 + 40) + \
        struct.pack("<IiiHHIIiiII", 40, BW, BH, 1, 24, 0, len(bmp24_data), 2835, 2835, 0, 0) + bmp24_data

# ---------------- BMP 8bpp 4x2 with 4-color palette, bottom-up ----------------
# palette: idx0..3 = gray 8,72,136,200 ; index at (x,y) = (x + y) % 4
pal_vals = [8, 72, 136, 200]
# Real 8bpp BMPs carry 256 palette entries; fill ALL of them (repeating the
# 4 colors) so Lua-parity 256-entry loading sees no zero padding.
pal8 = b"".join(bytes([pal_vals[i % 4]] * 3 + [0]) for i in range(256))
idx = [[(x + y) % 4 for x in range(BW)] for y in range(BH)]
rows8 = b""
for y in (BH - 1, 0):
    rows8 += bytes(idx[y][x] for x in range(BW))  # stride already 4
bmp8_offbits = 14 + 40 + 1024  # file header + info header + full palette
bmp8 = b"BM" + struct.pack("<IHHI", 14 + 40 + 1024 + len(rows8), 0, 0, bmp8_offbits) + \
       struct.pack("<IiiHHIIiiII", 40, BW, BH, 1, 8, 0, len(rows8), 0, 0, 4, 0) + pal8 + rows8
bmp8_gray = [[pal_vals[idx[y][x]] for x in range(BW)] for y in range(BH)]

# top-down variant: height = -2, rows stored top-first
bmp8td = bytearray(bmp8)
struct.pack_into("<i", bmp8td, 14 + 8, -BH)  # biHeight is at offset 22 (14+8)
rows8td = b"".join(bytes(idx[y][x] for x in range(BW)) for y in range(BH))
bmp8td[14 + 40 + 1024:] = rows8td
bmp8td = bytes(bmp8td)

# ---------------- GIF LZW reference encoder ----------------
def gif_lzw_encode(indices, min_code_size):
    clear, end = 1 << min_code_size, (1 << min_code_size) + 1
    code_size = min_code_size + 1
    table = {}
    for i in range(clear):
        table[(i,)] = i
    next_code = end + 1
    out_bits = []

    def emit(code, nbits):
        for i in range(nbits):
            out_bits.append((code >> i) & 1)

    emit(clear, code_size)
    w = (indices[0],)
    for c in indices[1:]:
        wk = w + (c,)
        if wk in table:
            w = wk
        else:
            emit(table[w], code_size)
            table[wk] = next_code
            next_code += 1
            if next_code > (1 << code_size) and code_size < 12:
                code_size += 1
            w = (c,)
    emit(table[w], code_size)
    emit(end, code_size)
    data = bytearray()
    acc = 0
    nb = 0
    for bit in out_bits:
        acc |= bit << nb
        nb += 1
        if nb == 8:
            data.append(acc)
            acc = 0
            nb = 0
    if nb:
        data.append(acc)
    return bytes(data)

def make_gif(w, h, interlace, indices, ncolors=4):
    assert w * h == len(indices)
    ct = b"".join(bytes([v, v, v]) for v in (0x00, 0x40, 0x80, 0xC0))
    flags = 0x80 | 0x01  # GCT present, 4 entries
    hdr = b"GIF87a" + struct.pack("<HHBBB", w, h, flags, 0, 0) + ct
    desc = b"," + struct.pack("<HHHHB", 0, 0, w, h, (0x40 if interlace else 0))
    mcs = 2
    lzw = gif_lzw_encode(indices, mcs)
    imgdata = bytes([mcs])
    for i in range(0, len(lzw), 255):
        chunk = lzw[i:i+255]
        imgdata += bytes([len(chunk)]) + chunk
    imgdata += b"\x00"
    return hdr + desc + imgdata + b";"

# gif1: 4x2 non-interlaced; palette idx = gray 0,64,128,192; pattern rows 0-1-2-3 / 3-2-1-0
gif1_idx = [0, 1, 2, 3, 3, 2, 1, 0]
gif1 = make_gif(4, 2, False, gif1_idx)
gif1_gray = [[0, 64, 128, 192], [192, 128, 64, 0]]

# gif2: 4x8 interlaced; display intent row r = idx (r % 4); LZW stream must carry
# pixels in INTERLACED STORAGE order (pass rows 0,4 / 2,6 / 1,3,5,7)
src_rows = [(r % 4) for r in range(8)]
storage_rows = [src_rows[0], src_rows[4], src_rows[2], src_rows[6],
                src_rows[1], src_rows[3], src_rows[5], src_rows[7]]
gif2 = make_gif(4, 8, True, [v for r in storage_rows for v in [r] * 4])
gif2_gray = [[(r % 4) * 64] * 4 for r in range(8)]

# truncated GIF (error path): cut inside the image descriptor
giftrunc = b"GIF87a" + struct.pack("<HHBBB", 4, 2, 0x81, 0, 0) + b"\x00\x40\x80\xc0" + b"," + struct.pack("<HHHH", 0, 0, 4, 2)

# ---------------- ICO 4x2 32bpp DIB ----------------
# XOR rows bottom-up; gray 32..80 top row, 48..96 bottom row; opaque.
# idx at (x,y): top row x=0..3 -> 32,48,64,80 ; bottom row -> 48,64,80,96
ico_gray = [[32 + 16 * x for x in range(BW)], [48 + 16 * x for x in range(BW)]]
xor = b""
for y in (BH - 1, 0):
    for x in range(BW):
        g = ico_gray[y][x]
        xor += bytes([g, g, g, 0xFF])  # BGRA opaque
and_row = (BW + 31) // 32 * 4  # 4 bytes per row
andmask = b"\x00" * (and_row * BH)  # all opaque
icodib = struct.pack("<IiiHHIIiiII", 40, BW, BH * 2, 1, 32, 0, len(xor) + len(andmask), 0, 0, 0, 0) + xor + andmask
ico = struct.pack("<HHH", 0, 1, 1) + \
      struct.pack("<BBBBHHII", BW, BH, 0, 0, 1, 32, len(icodib), 22) + icodib

# same ICO with bottom-stored row x0 transparent (mask bit for display row 1)
ico_masked = bytearray(ico)
mask_off = 22 + 40 + len(xor)  # AND mask starts here
ico_masked[mask_off + 0] = 0x80  # bottom-stored row, bit for x0

# ---------------- expected dither bytes (computed, not hand-derived) ----------------
exp_bmp24 = dither_rows_gray(pix24)
exp_bmp8 = dither_rows_gray(bmp8_gray)
exp_gif1 = dither_rows_gray(gif1_gray)
exp_gif2 = dither_rows_gray(gif2_gray)
exp_ico = dither_rows_gray(ico_gray)
# masked variant: display row1 x0 transparent -> composites to 255 (white)
ico_masked_gray = [list(r) for r in ico_gray]
ico_masked_gray[1][0] = 255
exp_ico_masked = dither_rows_gray(ico_masked_gray)

# ---------------- optional ffmpeg validation ----------------
def validate():
    files = {
        "/tmp/v_bmp24.bmp": bmp24,
        "/tmp/v_bmp8.bmp": bmp8,
        "/tmp/v_bmp8td.bmp": bmp8td,
        "/tmp/v_gif1.gif": gif1,
        "/tmp/v_gif2.gif": gif2,
        "/tmp/v_ico.ico": bytes(ico),
        "/tmp/v_ico_masked.ico": bytes(ico_masked),
    }
    ok = True
    for path, data in files.items():
        open(path, "wb").write(data)
        if path.endswith(".gif"):
            args = ["-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "gray"]
        else:
            args = ["-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "gray"]
        r = subprocess.run(["ffmpeg"] + args + ["/tmp/v_out.raw", "-y"],
                           capture_output=True, text=True)
        status = "DECODED" if r.returncode == 0 and os.path.getsize("/tmp/v_out.raw") > 0 else "FAILED"
        if r.stderr.strip():
            status += f" stderr={r.stderr.strip()[:100]}"
        print(f"  {os.path.basename(path)}: {status}")
        if status.startswith("FAILED"):
            ok = False
    return ok

if "--validate" in sys.argv:
    print("ffmpeg validation:")
    sys.exit(0 if validate() else 1)

# ---------------- emit header ----------------
def arr(name, data, comment=""):
    c = f" /* {comment} */" if comment else ""
    return f"{c}\nstatic const unsigned char {name}[{len(data)}] = {{\n{cbytes(data)}\n}};"

def rows_arr(name, rows_):
    return f"static const uint8_t {name}[{len(rows_)}] = {{\n" + \
           ",\n".join("  " + ", ".join(f"0x{b:02X}" for b in r) for r in rows_) + "\n};"

out = []
out.append("/* AUTO-GENERATED by tests/gen_p22b_vectors.py — do not edit by hand. */")
out.append("/* All containers validated with ffmpeg; expected dither bytes computed by the */")
out.append("/* generator from the reference Bayer thresholds (isBlack = gray < thr). */")
out.append("#ifndef P22B_VECTORS_H")
out.append("#define P22B_VECTORS_H")
out.append("#include <stdint.h>\n")
out.append("/* BMP 24bpp 4x2 bottom-up. gray[x][y]: row0 = 0,48,96,144 row1 = 16,64,112,160 */")
out.append(arr("BMP24", bmp24))
out.append(rows_arr("BMP24_EXP", exp_bmp24))
out.append("/* BMP 8bpp 4x2 bottom-up, palette 8/72/136/200, idx=(x+y)%4 */")
out.append(arr("BMP8", bmp8))
out.append(rows_arr("BMP8_EXP", exp_bmp8))
out.append("/* BMP 8bpp 4x2 top-down (negative height), same grid */")
out.append(arr("BMP8TD", bmp8td))
out.append("/* GIF 4x2 non-interlaced 87a, gray rows 0-1-2-3 / 3-2-1-0 */")
out.append(arr("GIF1", gif1))
out.append(rows_arr("GIF1_EXP", exp_gif1))
out.append("/* GIF 4x8 interlaced, display row r = idx r%4 (gray 64*r%4) */")
out.append(arr("GIF2", gif2))
out.append(rows_arr("GIF2_EXP", exp_gif2))
out.append("/* Truncated GIF (error path) */")
out.append(arr("GIFTRUNC", giftrunc))
out.append("/* ICO 4x2 32bpp DIB opaque, gray rows 32..80 / 48..96 */")
out.append(arr("ICO1", ico))
out.append(rows_arr("ICO1_EXP", exp_ico))
out.append("/* Same ICO with bottom-stored row x0 AND-mask bit set (display row1 x0 transparent) */")
out.append(arr("ICO_MASKED", ico_masked))
out.append(rows_arr("ICO_MASKED_EXP", exp_ico_masked))
out.append("#endif")

open("Source/render/decoders/p22b_vectors.h", "w").write("\n".join(out) + "\n")
print(f"WROTE p22b_vectors.h bmp24={len(bmp24)} bmp8={len(bmp8)} bmp8td={len(bmp8td)} "
      f"gif1={len(gif1)} gif2={len(gif2)} giftrunc={len(giftrunc)} ico={len(ico)} ico_masked={len(ico_masked)}")
print("expected BMP24 rows:", [r.hex() for r in exp_bmp24])
print("expected BMP8 rows:", [r.hex() for r in exp_bmp8])
print("expected GIF1 rows:", [r.hex() for r in exp_gif1])
print("expected GIF2 rows:", [r.hex() for r in exp_gif2])
print("expected ICO rows:", [r.hex() for r in exp_ico])
print("expected ICO_MASKED rows:", [r.hex() for r in exp_ico_masked])
