#!/usr/bin/env python3
# Generates src/render/decoders/selftest_jpeg_fixtures.{h,c}.
#
# Hand-crafted minimal JPEG streams whose coefficient values are chosen
# exactly, so golden pixels can be computed analytically. A shadow of the
# decoder math (idctT basis, idct2d integer passes, DC shortcuts,
# progressive DC rendering, box downscale) mirrors CometBrowser's
# jpeg.lua/scale.lua bit-for-bit. Crafted streams are cross-checked by
# decoding them with Pillow before being emitted.

import math
import io

# ---------------------------------------------------------------- shadow math

C0 = 0.707106781186548
IDT = [[int(math.floor(4096.0 * (C0 if k == 0 else 1.0)
                      * math.cos((2 * n + 1) * k * math.pi / 16) + 0.5))
        for n in range(8)] for k in range(8)]
SCALE = 4 * 4096 * 4096


def clamp255(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def idct2d(block):
    """block: 64 ints natural order -> 64 ints (mirrors idct2d passes)."""
    tmp = [0] * 64
    for r in range(8):
        for n in range(8):
            s = 0
            for k in range(8):
                s += block[r * 8 + k] * IDT[k][n]
            tmp[r * 8 + n] = s
    out = [0] * 64
    for c in range(8):
        for n in range(8):
            s = 0
            for k in range(8):
                s += tmp[k * 8 + c] * IDT[k][n]
            out[n * 8 + c] = s
    return out


def sample_full(block_deq):
    out = idct2d(block_deq)
    return [[clamp255(int(math.floor(out[v * 8 + u] / SCALE + 0.5)) + 128)
             for u in range(8)] for v in range(8)]


def sample_dconly(dc, q0):
    return clamp255(int(math.floor(dc * q0 / 8.0 + 0.5)) + 128)


def box_sizes(w, h, maxW=360, maxH=200):
    bw = max(1, (w + maxW - 1) // maxW)
    bh = max(1, (h + maxH - 1) // maxH)
    return bw, bh, w // bw, h // bh


def box_scale(grid, w, h, bw, bh, tw, th):
    accum = [0] * tw
    filled = 0
    out = []

    def flush(div):
        out.append([int(math.floor(accum[i] / div + 0.5)) for i in range(tw)])

    for y in range(h):
        row = grid[y]
        for oc in range(tw):
            xs = oc * bw
            xe = min(xs + bw, w)
            accum[oc] += sum(row[xs:xe])
        filled += 1
        if filled >= bh:
            flush(bw * bh)
            accum = [0] * tw
            filled = 0
    if filled > 0:
        flush(bw * filled)
    return out


# ------------------------------------------------------------- huffman tables

def huff_codes(counts, vals):
    """Canonical assignment mirroring buildHuff's recurrence."""
    codes = {}
    code = 0
    k = 0
    for l in range(1, 17):
        c = counts[l] if l < len(counts) else 0
        start = code
        for _ in range(c):
            if k < len(vals):
                codes[vals[k]] = (l, start)
            start += 1
            k += 1
        code = (code + c) << 1
    return codes


# Tables must satisfy T.81 Fig C.2 / libjpeg's check: after each length,
# the next code must still FIT that length (no all-ones code ever assigned).
def assert_legal(counts):
    code = 0
    for l in range(1, 17):
        code += counts[l]
        if code >= (1 << l):
            raise AssertionError(f"all-ones/overflow at level {l}")
        code <<= 1


# 13 symbols (cats 0..12), slack at every level: [0,0,1,2,4,6]
DC_COUNTS = [0] * 17
DC_COUNTS[2] = 1
DC_COUNTS[3] = 2
DC_COUNTS[4] = 4
DC_COUNTS[5] = 6
DC_VALS = list(range(13))

# 22 symbols: EOB, ZRL, (0,1)..(0,13), (1,1)..(1,6), (1,7): shape below
AC_COUNTS = [0] * 17
for _l, _c in [(2, 1), (3, 2), (4, 3), (5, 3), (6, 3), (7, 3), (8, 3),
               (9, 3), (10, 1)]:
    AC_COUNTS[_l] = _c
AC_VALS = ([0x00, 0xF0] + list(range(0x01, 0x0E)) +
           list(range(0x11, 0x18)))
assert sum(AC_COUNTS) == len(AC_VALS)

assert_legal(DC_COUNTS)
assert_legal(AC_COUNTS)

DC_CODES = huff_codes(DC_COUNTS, DC_VALS)
AC_CODES = huff_codes(AC_COUNTS, AC_VALS)


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, val, nbits):
        for i in range(nbits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((val >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0x00)
                self.acc = 0
                self.n = 0

    def bytes(self):
        pad = (8 - self.n) % 8            # pad TO byte boundary with 1-bits
        if pad:
            self.put((1 << pad) - 1, pad)
        return bytes(self.out)


def mag_bits(v, s):
    return (v if v > 0 else v + (1 << s) - 1) & ((1 << s) - 1) if s else 0


def cat(v):
    return 0 if v == 0 else abs(v).bit_length()


def enc_dc(bw, diff):
    s = cat(diff)
    ln, cd = DC_CODES[s]
    bw.put(cd, ln)
    if s:
        bw.put(mag_bits(diff, s), s)


def enc_ac(bw, acs):
    """acs: dict zigzag_pos(1..63) -> coef. Zero coefs are skipped (a zero
    coef would encode as the EOB symbol and desync the stream)."""
    prev = 0
    for zz in sorted(acs):
        coef = acs[zz]
        if coef == 0:
            continue
        r = zz - prev - 1
        while r >= 16:
            ln, cd = AC_CODES[0xF0]
            bw.put(cd, ln)
            r -= 16
        s = cat(coef)
        sym = (r << 4) | s
        assert sym in AC_CODES, hex(sym)
        ln, cd = AC_CODES[sym]
        bw.put(cd, ln)
        if s:
            bw.put(mag_bits(coef, s), s)
        prev = zz
    ln, cd = AC_CODES[0x00]
    bw.put(cd, ln)


# ------------------------------------------------------------ stream building

def u16(v):
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def seg(marker, payload):
    return bytes([0xFF, marker]) + u16(len(payload) + 2) + payload


def dqt(vals):
    return seg(0xDB, bytes([0x00]) + bytes(vals))


def sof(marker, w, h, comps):
    p = bytes([8]) + u16(h) + u16(w) + bytes([len(comps)])
    for cid, sh, sv, qt in comps:
        p += bytes([cid, (sh << 4) | sv, qt])
    return seg(marker, p)


def dht(tc, counts, vals):
    payload = bytes([tc]) + bytes(counts[1:17]) + bytes(vals)
    return seg(0xC4, payload)


def dri(n):
    return seg(0xDD, u16(n))


def sos(ns, comps, ss=0, se=63, ah=0, al=0):
    p = bytes([ns])
    for cid, dcsel, acsel in comps:
        p += bytes([cid, (dcsel << 4) | acsel])
    p += bytes([ss, se, (ah << 4) | al])
    return seg(0xDA, p)


SOI = b"\xFF\xD8"
EOI = b"\xFF\xD9"


def rst(n):
    return bytes([0xFF, 0xD0 + (n % 8)])


QT_ONES = [1] * 64
QT_AC2 = [1] * 64
QT_AC2[0] = 1
QT_AC2[1] = 2          # quant indexed BY ZIGZAG position (pos 1 -> nat idx 1)

# ------------------------------------------------------------------- fixtures


def build_baseline(w, h, blocks, restart_interval=0, comps=None,
                   qt_vals=QT_ONES, progressive=False):
    """blocks: function(bx, by, ci) -> (dc, acs_dict) over 8px blocks.
    comps: None => single-component 1x1."""
    if comps is None:
        comps = [(1, 1, 1, 0)]
    nsamp_h = max(c[1] for c in comps)
    nsamp_v = max(c[2] for c in comps)
    mcu_w = nsamp_h * 8
    mcu_h = nsamp_v * 8
    mcu_cols = max(1, -(-w // mcu_w))
    mcu_rows = max(1, -(-h // mcu_h))

    buf = bytearray(SOI)
    buf += dqt(qt_vals)
    buf += dht(0x00, DC_COUNTS, DC_VALS)
    buf += dht(0x10, AC_COUNTS, AC_VALS)
    if restart_interval:
        buf += dri(restart_interval)
    buf += sof(0xC2 if progressive else 0xC0, w, h, comps)

    scan_comps = []
    for idx, (cid, _, _, _) in enumerate(comps):
        scan_comps.append((cid, 0, 0))
    buf += sos(len(scan_comps), scan_comps)

    bw = BitWriter()
    rst_counter = 0
    pred = [0] * len(comps)          # per-component DC prediction
    for my in range(mcu_rows):
        for mx in range(mcu_cols):
            if restart_interval and rst_counter and \
                    rst_counter % restart_interval == 0:
                buf += bw.bytes()
                bw = BitWriter()
                buf += rst(rst_counter)   # decoder checks D0+(mcuIndex % 8)
                pred = [0] * len(comps)   # prediction resets after RSTn
            for ci, (_, ch, cv, _) in enumerate(comps):
                for bj in range(cv):
                    for bi in range(ch):
                        bx = mx * ch + bi
                        by = my * cv + bj
                        dc, acs = blocks(bx, by, ci)
                        enc_dc(bw, dc - pred[ci])
                        pred[ci] = dc
                        if ci == 0:
                            enc_ac(bw, acs)
                        else:
                            pass   # chroma: DC + implicit EOB handled below
                        if ci != 0:
                            # chroma blocks: DC then EOB (sync only)
                            ln, cd = AC_CODES[0x00]
                            bw.put(cd, ln)
            rst_counter += 1
    buf += bw.bytes()
    buf += EOI
    return bytes(buf)


def assemble_full(w, h, blocks, qt_vals, comps=None, dc_only=False,
                  sMaxH=None, sMaxV=None):
    """Assemble source-gray grid exactly as the luma renderer would."""
    if comps is None:
        comps = [(1, 1, 1, 0)]
    nsamp_h = max(c[1] for c in comps)
    nsamp_v = max(c[2] for c in comps)
    mcu_w = nsamp_h * 8
    mcu_h = nsamp_v * 8
    mcu_cols = max(1, -(-w // mcu_w))
    mcu_rows = max(1, -(-h // mcu_h))
    grid = [[128] * w for _ in range(h)]

    def put(px, py, g):
        if 0 <= px < w and 0 <= py < h:
            grid[py][px] = g

    for my in range(mcu_rows):
        for mx in range(mcu_cols):
            for ci, (_, ch, cv, qtid) in enumerate(comps):
                if ci != 0:
                    continue
                for bj in range(cv):
                    for bi in range(ch):
                        bx = mx * ch + bi
                        by = my * cv + bj
                        dc, acs = blocks(bx, by, 0)
                        px = mx * mcu_w + bi * 8
                        py = my * mcu_h + bj * 8
                        if dc_only:
                            g = sample_dconly(dc, qt_vals[0])
                            for yy in range(8):
                                for xx in range(8):
                                    put(px + xx, py + yy, g)
                        else:
                            blk = [0] * 64
                            blk[0] = dc * qt_vals[0]
                            for zz, coef in acs.items():
                                NAT = [
                                    0, 1, 8, 16, 9, 2, 3, 10,
                                    17, 24, 32, 25, 18, 11, 4, 5,
                                    12, 19, 26, 33, 40, 48, 41, 34,
                                    27, 20, 13, 6, 7, 14, 21, 28,
                                    35, 42, 49, 56, 57, 50, 43, 36,
                                    29, 22, 15, 23, 30, 37, 44, 51,
                                    58, 59, 52, 45, 38, 31, 39, 46,
                                    53, 60, 61, 54, 47, 55, 62, 63]
                                blk[NAT[zz]] = coef * qt_vals[zz]
                            pix = sample_full(blk)
                            for yy in range(8):
                                for xx in range(8):
                                    put(px + xx, py + yy, pix[yy][xx])
    return grid


def build_progressive_dc_scan(w, h, dcs, comps=None, al=0, qt_idx=0):
    """Single interleaved DC scan carrying given per-block predictions."""
    if comps is None:
        comps = [(1, 1, 1, 0)]
    nsamp_h = max(c[1] for c in comps)
    nsamp_v = max(c[2] for c in comps)
    mcu_w = nsamp_h * 8
    mcu_h = nsamp_v * 8
    mcu_cols = max(1, -(-w // mcu_w))
    mcu_rows = max(1, -(-h // mcu_h))
    buf = sos(1, [(comps[0][0], 0, 0)], 0, 0, 0, al)
    bw = BitWriter()
    pred = 0
    for my in range(mcu_rows):
        for mx in range(mcu_cols):
            for bj in range(comps[0][2]):
                for bi in range(comps[0][1]):
                    dc = dcs(mx * comps[0][1] + bi, my * comps[0][2] + bj)
                    diff = dc - pred
                    enc_dc(bw, diff)
                    pred = dc
    buf += bw.bytes()
    return buf


FIXTURES = []


def add(name, data):
    FIXTURES.append((name, data))


# 1. flat: all-zero DC blocks through the full IDCT path -> flat mid gray.
FX_W, FX_H = 64, 48
add("fx_j_flat", build_baseline(
    FX_W, FX_H, lambda bx, by, ci: (0, {})))

# 2. dcOnly stripes: 480x480 area > 200000 forces the DC shortcut.
SW, SH = 480, 480
def stripe_blocks(bx, by, ci):
    dc = -1024 if (by // 2) % 2 == 0 else 1024
    return (dc, {})
add("fx_j_dcstripes", build_baseline(
    SW, SH, stripe_blocks))
GRID_STRIPES = assemble_full(
    SW, SH, stripe_blocks, QT_ONES,
    dc_only=(SW * SH) > 200000)

# 3. AC-bearing blocks, quant pos1 == 2, full IDCT path.
AW, AH = 32, 24
def ac_blocks(bx, by, ci):
    dc = ((bx + by) % 3 - 1) * 256
    acs = {}
    m = (bx * 7 + by * 5) % 4
    if m == 1:
        acs[1] = 64
    elif m == 2:
        acs[1] = -96
    elif m == 3:
        acs[17] = -32      # exercises ZRL (run exactly 16 -> r back to 0)
    return (dc, acs)
add("fx_j_acblocks", build_baseline(
    AW, AH, ac_blocks, qt_vals=QT_AC2))
GRID_AC = assemble_full(AW, AH, ac_blocks, QT_AC2)

# 4. restart intervals every 2 MCUs.
RW, RH = 160, 144
def rst_blocks(bx, by, ci):
    return (((bx % 4) * 128) - 192, {})
add("fx_j_restart", build_baseline(
    RW, RH, rst_blocks, restart_interval=2))
GRID_RST = assemble_full(RW, RH, rst_blocks, QT_ONES)

# 5. 4:2:0 subsampling: chroma present but ignored.
SUBW, SUBH = 128, 96
SUB_COMPS = [(1, 2, 2, 0), (2, 1, 1, 0), (3, 1, 1, 0)]
def sub_blocks(bx, by, ci):
    if ci == 0:
        dc = ((((bx ^ by) & 7) - 3) * 128)
        acs = {}
        if (bx + by) % 2:
            acs[1] = 80          # omit zero-amplitude entries entirely
        return (dc, acs)
    return (0, {})
add("fx_j_sub420", build_baseline(
    SUBW, SUBH, sub_blocks, comps=SUB_COMPS))
GRID_SUB = assemble_full(SUBW, SUBH, sub_blocks, QT_ONES, comps=SUB_COMPS)

# 6/7. progressive DC-only, rendered either at EOI (fallback) or at the
# first AC scan (early render) -- identical visible output.
PW, PH = 96, 80
def prog_dcs(bx, by):
    return by * 48 - 168
prog_body = build_progressive_dc_scan(PW, PH, prog_dcs)
_acbw = BitWriter()
for _ in range((PW // 8) * (PH // 8)):
    _ln, _cd = AC_CODES[0x00]
    _acbw.put(_cd, _ln)
add("fx_j_prog_dc",
    SOI + dqt(QT_ONES) + dht(0x00, DC_COUNTS, DC_VALS) +
    sof(0xC2, PW, PH, [(1, 1, 1, 0)]) + prog_body + EOI)
add("fx_j_prog_multi",
    SOI + dqt(QT_ONES) + dht(0x00, DC_COUNTS, DC_VALS) +
    dht(0x10, AC_COUNTS, AC_VALS) +
    sof(0xC2, PW, PH, [(1, 1, 1, 0)]) + prog_body +
    sos(1, [(1, 0, 0)], 1, 63, 0, 0) +
    _acbw.bytes() + EOI)

# 8. progressive DC + refinement scan (ah=1, al=1 adjusts by +-2).
RFW, RFH = 64, 48
def rf_base(bx, by):
    return ((bx + by) % 5 - 2) * 160
rf_buf = sos(1, [(1, 0, 0)], 0, 0, 0, 1)
_bw = BitWriter()
_pred = 0
for _my in range(RFH // 8):
    for _mx in range(RFW // 8):
        dc = rf_base(_mx, _my)
        enc_dc(_bw, dc - _pred)
        _pred = dc
rf_buf += _bw.bytes()

def rf_stored(bx, by):
    base = rf_base(bx, by) << 1
    corr = (2 if (bx + by) % 2 == 0 else -2)
    return base + corr
_rfbw = BitWriter()
for _my in range(RFH // 8):
    for _mx in range(RFW // 8):
        _rfbw.put(1 if (_mx + _my) % 2 == 0 else 0, 1)
add("fx_j_prog_refine",
    SOI + dqt(QT_ONES) + dht(0x00, DC_COUNTS, DC_VALS) +
    sof(0xC2, RFW, RFH, [(1, 1, 1, 0)]) + rf_buf +
    sos(1, [(1, 0, 0)], 0, 0, 1, 1) + _rfbw.bytes() + EOI)
GRID_REFINE = [[sample_dconly(rf_stored(x // 8, y // 8), 1)
                for x in range(RFW)] for y in range(RFH)]

# guards / misc
add("fx_j_sof9", SOI + seg(0xC9, bytes([8]) + u16(8) + u16(8) + b"\x01" +
                          b"\x01\x11\x00") + EOI)
add("fx_j_trunc_sof", SOI + bytes([0xFF, 0xC0, 0x00, 0x0F, 0x08, 0x00, 0x30,
                                   0x00, 0x40, 0x03, 0x01, 0x22, 0x00]))
add("fx_j_eoi_only", SOI + EOI)
add("fx_j_bad_sig", b"NOTAJPEG-not-a-jpeg-at-all........")

# real-world bench file via Pillow
from PIL import Image
bench_img = Image.new("RGB", (800, 600))
bp = bench_img.load()
for y in range(600):
    for x in range(800):
        bp[x, y] = ((x * 255) // 800, (y * 255) // 600,
                    ((x + y) * 255) // 1400)
bio = io.BytesIO()
bench_img.save(bio, "JPEG", quality=85)
add("fx_j_pil_bench", bio.getvalue())

# ------------------------------------------------------- pillow cross-check

from PIL import Image as PImage, ImageFile

# DC-scan-only progressive files never carry AC scans; libjpeg calls that
# incomplete. EOI-only is intentionally not an image.
EXPECTED_INCOMPLETE = {"fx_j_prog_refine"}

for name, data in FIXTURES:
    if name in ("fx_j_bad_sig", "fx_j_trunc_sof", "fx_j_sof9",
            "fx_j_eoi_only"):
        print(f"pillow skip (expect invalid): {name}")
        continue
    try:
        im = PImage.open(io.BytesIO(data))
        im.load()
        print(f"pillow ok: {name} {im.size} {im.mode}")
    except Exception as e:
        if name in EXPECTED_INCOMPLETE:
            ImageFile.LOAD_TRUNCATED_IMAGES = True
            im = PImage.open(io.BytesIO(data))
            print(f"pillow partial-ok (expected incomplete): "
                  f"{name} {im.size} [{e}]")
            ImageFile.LOAD_TRUNCATED_IMAGES = False
        else:
            raise SystemExit(f"ENCODER BUG in {name}: {e}")

# ------------------------------------------------------------------- goldens

def golden_for(idx):
    name = FIXTURES[idx][0]
    if name == "fx_j_flat":
        g = assemble_full(FX_W, FX_H, lambda bx, by, ci: (0, {}), QT_ONES)
        return g, FX_W, FX_H, False
    if name == "fx_j_dcstripes":
        return GRID_STRIPES, SW, SH, True
    if name == "fx_j_acblocks":
        return GRID_AC, AW, AH, False
    if name == "fx_j_restart":
        return GRID_RST, RW, RH, False
    if name == "fx_j_sub420":
        return GRID_SUB, SUBW, SUBH, False
    if name in ("fx_j_prog_dc", "fx_j_prog_multi"):
        g = [[clamp255(int(math.floor(prog_dcs(x // 8, y // 8) * 1 / 8.0
                                        + 0.5)) + 128) for x in range(PW)]
             for y in range(PH)]
        return g, PW, PH, False
    if name == "fx_j_prog_refine":
        return GRID_REFINE, RFW, RFH, False
    return None, 0, 0, False


PROBE_SPOTS = [(0, 0), (-1, -1), (2, 2), (4, 4), (2, 6), (6, 2),
               (3, 5), (7, 1)]


def gen_probes(idx, grid, w, h):
    pts = []
    spots = [(0, 0), (w - 1, h - 1), (w // 2, h // 2), (w // 4, h // 2),
             (3 * w // 4, h // 2), (w // 8, h // 8), (w - 2, h // 3),
             (w // 2, h - 3)]
    for x, y in spots:
        pts.append((x, y, grid[y][x]))
    return pts


# -------------------------------------------------------------------- emit

def emit():
    h_parts = []
    c_parts = []
    h_parts.append("// Generated by tools/gen_jpeg_fixtures.py — do not edit.\n\n")
    h_parts.append("#ifndef PLUTO_RENDER_DECODERS_SELFTEST_JPEG_FIXTURES_H\n")
    h_parts.append("#define PLUTO_RENDER_DECODERS_SELFTEST_JPEG_FIXTURES_H\n\n")
    h_parts.append("#include <stddef.h>\n\n")
    h_parts.append("typedef struct { int x, y, g; } JpegProbePt;\n")
    h_parts.append("typedef struct { int img, nProbes; "
                   "const JpegProbePt* p; } JpegProbe;\n\n")

    names = []
    for idx, (name, data) in enumerate(FIXTURES):
        lines = []
        for i in range(0, len(data), 12):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 12])
            lines.append("    " + chunk + ",")
        body = "\n".join(lines)
        h_parts.append(f"static const unsigned char {name}[] = {{\n{body}\n}};\n")
        h_parts.append(f"#define {name.upper()}_LEN {len(data)}\n\n")
        names.append(name)

    probe_defs = []
    probe_tables = []
    for idx, (name, data) in enumerate(FIXTURES):
        grid, gw, gh, _dc = golden_for(idx)
        if grid is None:
            continue
        bw_, bh_, tw_, th_ = box_sizes(gw, gh, 360, 200)
        outg = box_scale(grid, gw, gh, bw_, bh_, tw_, th_)
        pts = gen_probes(idx, outg, tw_, th_)
        pname = f"jp_{name[5:]}"
        pt_items = ", ".join(f"{{{x},{y},{g}}}" for x, y, g in pts)
        probe_defs.append(
            f"static const JpegProbePt {pname}[] = {{{pt_items}}};\n")
        probe_tables.append(f'    {{{idx}, {len(pts)}, {pname}}},')
        print(f"{name}: src {gw}x{gh} box {bw_}x{bh_} -> out {tw_}x{th_}")
        globals()[f"DIMS_{name[5:]}"] = (tw_, th_)

    c_parts = []
    c_parts.append("// Generated by tools/gen_jpeg_fixtures.py — do not edit.\n\n")
    c_parts.append('#include "render/decoders/selftest_jpeg_fixtures.h"\n\n')

    h_parts.append("typedef struct {\n"
                   "    int img;\n"
                   "} JpegDims;\n\n")

    # emit dims table as macros instead (simpler for static use)
    dim_lines = []
    for idx, (name, data) in enumerate(FIXTURES):
        key = name[5:].upper()
        if f"DIMS_{name[5:]}" in globals():
            tw_, th_ = globals()[f"DIMS_{name[5:]}"]
            dim_lines.append(f"#define FX_J_{key}_TW {tw_}")
            dim_lines.append(f"#define FX_J_{key}_TH {th_}")
    h_parts.append("\n".join(dim_lines) + "\n\n")

    h_parts.append("extern const JpegProbe jpeg_probes[];\n")
    h_parts.append("#define JPEG_PROBES_LEN "
                   f"{len(probe_tables)}\n\n")
    h_parts.append("#endif\n")

    for pd_ in probe_defs:
        c_parts.append(pd_)
    c_parts.append("\nconst JpegProbe jpeg_probes[] = {\n")
    for ptline in probe_tables:
        c_parts.append("    " + ptline.strip() + "\n")
    c_parts.append("};\n")

    with open("src/render/decoders/selftest_jpeg_fixtures.h", "w") as f:
        f.write("".join(h_parts))
    with open("src/render/decoders/selftest_jpeg_fixtures.c", "w") as f:
        f.write("".join(c_parts))
    print("emitted selftest_jpeg_fixtures.{h,c}")


emit()
