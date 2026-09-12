#!/usr/bin/env python3
"""Generate independent test vectors for the JPEG decoder -> p25_vectors.h.

Oracles:
  1. ffmpeg builds every baseline JPEG from raw gray pixels (independent
     encoder) and decodes it back (independent decoder); expected gray grids
     are computed from ffmpeg's own decoded pixels by simulating scale.lua's
     box filter EXACTLY (int box = ceil(src/max), half-up rounding, trailing
     partial row block divisor boxW*filled).
  2. The progressive vector is hand-built (SOF2 + DC-only scan, standard
     tables) so no external progressive encoder is needed; its expected grid
     is derived from the constructed DC values directly.
  3. The parity anchor: JPEGDecoder.decode feeds the SAME Scale + Dither
     modules already ported and verified 1:1 — jpeg_decode_gray must
     reproduce the box-filter output of ffmpeg's decoded pixels.
"""
import subprocess, sys, os, math

OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Source", "render", "decoders", "p25_vectors.h"))
TMP = "/tmp/p25vec"
os.makedirs(TMP, exist_ok=True)
FF = "/opt/homebrew/bin/ffmpeg"
if not os.path.exists(FF):
    FF = "ffmpeg"

def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stderr.decode()[:800]}")
    return r.stdout

def sim_scale(src, w, h, maxW=360, maxH=200):
    """scale.lua box filter, verbatim semantics. src = list of rows (bytes)."""
    boxW = max(1, math.ceil(w / max(1, maxW)))
    boxH = max(1, math.ceil(h / max(1, maxH)))
    targetW = max(1, w // boxW)
    targetH = max(1, h // boxH)
    out = []
    filled = 0
    accum = [0] * targetW
    def emit(div):
        nonlocal filled, accum
        row = []
        for i in range(targetW):
            row.append(min(255, max(0, math.floor(accum[i] / div + 0.5))))
        out.append(row)
        filled = 0
        accum = [0] * targetW
    for y in range(h):
        row = src[y]
        if boxW == 1:
            for i in range(targetW):
                v = row[i] if i < w else 0
                accum[i] += v
        else:
            x = 0
            for oc in range(targetW):
                xEnd = min(w, x + boxW)
                s = 0
                for k in range(x, xEnd):
                    s += row[k] if k < w else 0
                accum[oc] += s
                x += boxW
        filled += 1
        if filled >= boxH:
            emit(boxW * boxH)
    if filled > 0:
        emit(boxW * filled)
    return out, targetW, targetH

def ff_decode_gray(path):
    return run([FF, "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "gray", "-"])

def ff_grid(raw, w, h):
    return [list(raw[y * w:(y + 1) * w]) for y in range(h)]

def encode_gray(rows, w, h):
    p = f"{TMP}/src.pgm"
    with open(p, "wb") as f:
        f.write(f"P5 {w} {h} 255\n".encode())
        for r in rows:
            f.write(bytes(r))
    out = f"{TMP}/src.jpg"
    run([FF, "-v", "error", "-y", "-i", p, "-pix_fmt", "gray", "-c:v", "mjpeg", out])
    return open(out, "rb").read()

vecs = []
def add(name, data):
    vecs.append((name, data))
    print(f"  {name}: {len(data)} bytes")

# Standard JPEG tables (Annex K luminance).
STD_DC_COUNTS = [0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
                 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
STD_DC_VALUES = list(range(0x00, 0x0C))
STD_AC_COUNTS = [0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
                 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D]
STD_AC_VALUES = [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
]

def seg(marker, payload):
    ln = len(payload) + 2
    return bytes([0xFF, marker, ln >> 8, ln & 0xFF]) + bytes(payload)

def dqt(id_, qvals):
    return seg(0xDB, [id_ & 0x0F] + list(qvals))

def dht(cls, id_, counts, values):
    return seg(0xC4, [(cls << 4) | id_] + list(counts) + list(values))

def sof(marker, w, h, comps):
    # comps: list of (id, h, v, qt)
    p = bytearray([8, h >> 8, h & 0xFF, w >> 8, w & 0xFF, len(comps)])
    for cid, hh, vv, q in comps:
        p += bytes([cid, (hh << 4) | vv, q])
    return seg(marker, p)

def sos(comps, ss=0, se=0, ah=0, al=0):
    # comps: list of (cid, dcTbl, acTbl)
    p = bytearray([len(comps)])
    for cid, dct, act in comps:
        p += bytes([cid, (dct << 4) | act])
    p += bytes([ss, se, (ah << 4) | al])
    return seg(0xDA, p)

class BitWriter:
    def __init__(self):
        self.bits = []
    def put(self, val, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((val >> i) & 1)
    def flush_pad(self):
        while len(self.bits) % 8:
            self.bits.append(1)
    def entropy_bytes(self):
        self.flush_pad()
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for b in self.bits[i:i + 8]:
                byte = (byte << 1) | b
            if byte == 0xFF:
                out += b"\xFF\x00"
            else:
                out.append(byte)
        return bytes(out)
    def raw_bit_length(self):
        return len(self.bits)

def huff_codes(counts, values):
    codes = {}
    code = 0
    k = 0
    for l in range(1, 17):
        for _ in range(counts[l - 1]):
            codes[values[k]] = (code, l)
            code += 1
            k += 1
        code <<= 1
    return codes

DC_CODES = huff_codes(STD_DC_COUNTS, STD_DC_VALUES)
AC_CODES = huff_codes(STD_AC_COUNTS, STD_AC_VALUES)

# ── tc1: 8x8 baseline gradient (full IDCT path, box=1) ────────────────────
w, h = 8, 8
rows = [[(x * 28 + y * 3) % 256 for x in range(w)] for y in range(h)]
jpg = encode_gray(rows, w, h)
grid = ff_grid(ff_decode_gray("/tmp/p25vec/src.jpg"), w, h)
exp, tw, th = sim_scale(grid, w, h)
add("JPEG1", jpg)
EXP1 = [v for r in exp for v in r]
TW1, TH1 = tw, th

# ── tc2: 16x16 gradient (full IDCT) ───────────────────────────────────────
w, h = 16, 16
rows = [[(x * 15 + y * 9) % 256 for x in range(w)] for y in range(h)]
jpg = encode_gray(rows, w, h)
grid = ff_grid(ff_decode_gray("/tmp/p25vec/src.jpg"), w, h)
exp, tw, th = sim_scale(grid, w, h)
add("JPEG2", jpg)
EXP2 = [v for r in exp for v in r]
TW2, TH2 = tw, th

# ── tc3: 64x64 (battery calls with maxW=16 → boxW=4 → DC-only path) ───────
w, h = 64, 64
rows = [[(x * 3 + y * 5) % 256 for x in range(w)] for y in range(h)]
jpg = encode_gray(rows, w, h)
grid = ff_grid(ff_decode_gray("/tmp/p25vec/src.jpg"), w, h)
exp, tw, th = sim_scale(grid, w, h, maxW=16, maxH=12)
add("JPEG3", jpg)
EXP3 = [v for r in exp for v in r]
TW3, TH3 = tw, th

# ── tc4: 24x24 grayscale 1-component ──────────────────────────────────────
w, h = 24, 24
rows = [[(x * 10 + y * 7) % 256 for x in range(w)] for y in range(h)]
jpg = encode_gray(rows, w, h)
grid = ff_grid(ff_decode_gray("/tmp/p25vec/src.jpg"), w, h)
exp, tw, th = sim_scale(grid, w, h)
add("JPEG4", jpg)
EXP4 = [v for r in exp for v in r]
TW4, TH4 = tw, th

# ── tc5: 2x2 tiny (minimum size) ──────────────────────────────────────────
w, h = 2, 2
rows = [[200, 60], [90, 170]]
jpg = encode_gray(rows, w, h)
grid = ff_grid(ff_decode_gray("/tmp/p25vec/src.jpg"), w, h)
exp, tw, th = sim_scale(grid, w, h)
add("JPEG5", jpg)
EXP5 = [v for r in exp for v in r]
TW5, TH5 = tw, th

# ── tc6: hand-built PROGRESSIVE DC-only (SOF2, 32x32, all DC = +40) ───────
# Frame: 1 component (id 1, 1x1, qtable 0). DC scan (Ss=0, Se=0, Ah=0, Al=0)
# emitting 4x4 = 16 blocks. DC diff chain: first block +40, rest 0.
w, h = 32, 32
n_blocks = 16
bw = BitWriter()
for i in range(n_blocks):
    s, ln = DC_CODES[0 if i else 4]  # i==0: category 4 (val in 8..15); else cat 0
    bw.put(s, ln)
    if i == 0:
        bw.put(40, 4)  # extra bits for +40 (>= 8, so val as-is)
entropy = bw.entropy_bytes()

prog = bytearray(b"\xFF\xD8")
prog += dqt(0, [1] * 64)          # qtable all 1 → dc*q/8 = dc/8? NOTE: qdc=1
prog += dht(0, 0, STD_DC_COUNTS, STD_DC_VALUES)
prog += dht(1, 0, STD_AC_COUNTS, STD_AC_VALUES)
prog += sof(0xC2, w, h, [(1, 1, 1, 0)])
prog += sos([(1, 0, 0)], ss=0, se=0, ah=0, al=0)
prog += entropy
prog += b"\xFF\xD9"
prog = bytes(prog)

# Expected grid: g = clamp(dc*q/8 + 128) with q = qtz[1] = 1, dc = 40.
# g = clamp(floor(40*1/8 + 0.5) + 128) = clamp(5 + 128) = 133 flat.
G = 133
# sim_scale of a flat-133 32x32 source = flat 133 everywhere (any box).
flat = [[G] * w for _ in range(h)]
exp, tw, th = sim_scale(flat, w, h)
add("JPEG6_PROG", prog)
EXP6 = [v for r in exp for v in r]
TW6, TH6 = tw, th
# record the DC expectation independently: all pixels 133
assert all(v == G for v in EXP6), "flat expectation self-check"

# ── tc7: truncated baseline (valid header/two MCU rows, entropy cut) ──────
# Reuse JPEG2's container, cut the entropy stream mid-way (keep 60% of bytes
# after SOS). Expected: decode stops at EOF; rows completed so far are those
# fully fed MCU rows; grid rows beyond are 255 (missing). The generator
# cannot know which rows the C decoder will have fed (bit-level), so the
# battery asserts only structural properties: img != NULL, and rows 0..k
# equal the full decode's rows for SOME k >= 1, with trailing rows 0x00
# (unpainted bits) — deterministic assertions in C, not here.
w, h = 16, 16
rows = [[(x * 15 + y * 9) % 256 for x in range(w)] for y in range(h)]
jpg2 = encode_gray(rows, w, h)
sos_i = jpg2.index(b"\xFF\xDA")
seglen = (jpg2[sos_i + 2] << 8) | jpg2[sos_i + 3]
ent_start = sos_i + 2 + seglen
cut = ent_start + (len(jpg2) - ent_start) * 3 // 5
trunc = jpg2[:cut]  # no EOI
add("JPEG7_TRUNC", trunc)

# ── tc8: CMYK 4-component (unsupported → NULL). Hand-built SOF0 with 4
#     components + SOS with 4 comps; the reference's luma-only path actually
#     DECODES it (ci==1 luma) — so expectation: valid image, not NULL! The
#     "unsupported" case in the reference is arithmetic coding (SOF9), which
#     hits the default skip path → no SOS decode → acc stays NULL → nil.
#     Build an SOF9 container (no entropy decode) → expect NULL.
w, h = 8, 8
arf = bytearray(b"\xFF\xD8")
arf += dqt(0, [1] * 64)
arf += sof(0xC9, w, h, [(1, 1, 1, 0)])  # SOF9 = arithmetic
arf += sos([(1, 0, 0)], ss=1, se=63, ah=0, al=0)
arf += b"\x00" * 8
arf += b"\xFF\xD9"
add("JPEG8_ARITH", bytes(arf))

# ── Emit header ────────────────────────────────────────────────────────────
def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"

def c_ints(name, values):
    lines = []
    for i in range(0, len(values), 16):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + 16]) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"

hdr = """/* Generated by tests/gen_p25_vectors.py — DO NOT EDIT BY HAND.
 * JPEG test vectors with ffmpeg-validated containers and generator-computed
 * expected grayscale grids (scale.lua box filter simulated exactly). */
#ifndef PLUTO_P25_VECTORS_H
#define PLUTO_P25_VECTORS_H

"""
for name, data in vecs:
    hdr += c_array(name, data)
hdr += c_ints("JPEG1_EXP", EXP1)
hdr += c_ints("JPEG2_EXP", EXP2)
hdr += c_ints("JPEG3_EXP", EXP3)
hdr += c_ints("JPEG4_EXP", EXP4)
hdr += c_ints("JPEG5_EXP", EXP5)
hdr += c_ints("JPEG6_EXP", EXP6)
hdr += f"#define JPEG1_TW {TW1}\n#define JPEG1_TH {TH1}\n"
hdr += f"#define JPEG2_TW {TW2}\n#define JPEG2_TH {TH2}\n"
hdr += f"#define JPEG3_TW {TW3}\n#define JPEG3_TH {TH3}\n"
hdr += f"#define JPEG4_TW {TW4}\n#define JPEG4_TH {TH4}\n"
hdr += f"#define JPEG5_TW {TW5}\n#define JPEG5_TH {TH5}\n"
hdr += f"#define JPEG6_TW {TW6}\n#define JPEG6_TH {TH6}\n"
hdr += "\n#endif /* PLUTO_P25_VECTORS_H */\n"

with open(OUT, "w") as f:
    f.write(hdr)
print(f"wrote {OUT} ({len(hdr)} bytes)")
