#!/usr/bin/env python3
"""Generates selftest_ico_fixtures.{h,c} for the P23 ICO port.

Hand-builds ICO containers (classic DIB entries at every supported bpp,
top-down variant, oversized downscale, PNG delegation, fallback and
selection guards) plus one Pillow-produced PNG payload. Expected output
grids come from an independent byte-level Python replica of the Lua
decoder algorithm operating on the encoded bytes."""

import io
import math
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_H = os.path.join(ROOT, "src", "render", "decoders",
                     "selftest_ico_fixtures.h")
OUT_C = os.path.join(ROOT, "src", "render", "decoders",
                     "selftest_ico_fixtures.c")

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv_grid(grid):
    hsh = FNV_OFFSET
    for row in grid:
        for v in row:
            hsh ^= v & 0xFF
            hsh = (hsh * FNV_PRIME) & 0xFFFFFFFF
    return hsh


def rgb_to_gray(r, g, b):
    return (306 * r + 601 * g + 117 * b) >> 10


def composite(gray, a):
    if a >= 255:
        return gray
    if a <= 0:
        return 255
    return math.floor((gray * a + 255 * (255 - a)) / 255 + 0.5)


class Blob:
    """Byte accessor mirroring the C/Lua helpers (missing -> defaults)."""

    def __init__(self, d):
        self.d = d

    def at(self, i, default=0):
        if i is None or i < 0 or i >= len(self.d):
            return default
        return self.d[i]

    def u16(self, i):
        if i >= len(self.d) or i + 1 >= len(self.d):
            return 0
        return self.d[i] | (self.d[i + 1] << 8)

    def u32(self, i):
        if i >= len(self.d) or i + 3 >= len(self.d):
            return 0
        return (self.d[i] | (self.d[i + 1] << 8) |
                (self.d[i + 2] << 16) | (self.d[i + 3] << 24))


def signed32(blob, i):
    v = blob.u32(i)
    return v - (1 << 32) if v >= (1 << 31) else v


# ---------------------------------------------------------------------------
# Reference decoder (independent reimplementation of ico.lua at the byte
# level; the same algorithm the C port must reproduce exactly).

def ref_decode_dib(data, max_w, max_h):
    if data is None or len(data) < 40:
        return None
    D = Blob(data)
    header_size = D.u32(0)
    width = signed32(D, 4)
    raw_height = signed32(D, 8)
    bpp = D.u16(14)
    compression = D.u32(16)

    if header_size < 40 or width <= 0 or raw_height == 0 or compression != 0:
        return None
    if bpp not in (1, 4, 8, 24, 32):
        return None

    top_down = raw_height < 0
    height = abs(raw_height) // 2
    if height <= 0:
        return None

    palette = {}
    if bpp <= 8:
        n = 1 << bpp
        pal_off = header_size
        for i in range(n):
            p = pal_off + i * 4
            if p < len(data) and p + 2 < len(data):
                palette[i] = rgb_to_gray(data[p + 2], data[p + 1], data[p])
            else:
                palette[i] = 0

    row_bytes = ((bpp * width + 31) // 32) * 4
    and_row_bytes = ((width + 31) // 32) * 4
    pix_off = header_size + ((1 << bpp) * 4 if bpp <= 8 else 0)
    mask_off = pix_off + row_bytes * height
    has_mask = (mask_off + and_row_bytes * height) <= len(data)

    scale = 1.0
    if width > max_w or height > max_h:
        scale = max(width / max_w, height / max_h)
    tw = max(1, math.floor(width / scale))
    th = max(1, math.floor(height / scale))

    grid = [[255] * tw for _ in range(th)]
    for oy in range(th):
        for ox in range(tw):
            sx = min(width - 1, math.floor(ox * scale))
            sy = min(height - 1, math.floor(oy * scale))
            by = sy if top_down else height - 1 - sy
            row_start = pix_off + by * row_bytes

            if bpp == 32:
                p = row_start + sx * 4
                gray = rgb_to_gray(D.at(p + 2), D.at(p + 1), D.at(p))
                alpha = D.at(p + 3, 255)
            elif bpp == 24:
                p = row_start + sx * 3
                gray = rgb_to_gray(D.at(p + 2), D.at(p + 1), D.at(p))
                alpha = 255
            elif bpp == 8:
                idx = D.at(row_start + sx)
                gray = palette.get(idx, 0)
                alpha = 255
            elif bpp == 4:
                b = D.at(row_start + (sx >> 1))
                idx = ((b >> 4) & 0x0F) if sx % 2 == 0 else (b & 0x0F)
                gray = palette.get(idx, 0)
                alpha = 255
            else:
                b = D.at(row_start + (sx >> 3))
                idx = (b >> (7 - (sx % 8))) & 1
                gray = palette.get(idx, 0)
                alpha = 255

            if has_mask:
                m_row = mask_off + by * and_row_bytes
                mb = D.at(m_row + (sx >> 3))
                if ((mb >> (7 - (sx % 8))) & 1) == 1:
                    grid[oy][ox] = 255
                    continue

            if alpha < 255:
                gray = composite(gray, alpha)
            grid[oy][ox] = gray
    return tw, th, grid


def ref_decode_ico(data, max_w, max_h, png_golden=None):
    if data is None or len(data) < 22:
        return None
    D = Blob(data)
    reserved = D.u16(0)
    ftype = D.u16(2)
    count = D.u16(4)
    if reserved != 0 or ftype not in (1, 2) or count == 0:
        return None

    entries = []
    for i in range(count):
        base = 6 + i * 16
        w = D.at(base)
        h = D.at(base + 1)
        if w == 0:
            w = 256
        if h == 0:
            h = 256
        bits = D.u16(base + 6) if ftype == 1 else 0
        size = D.u32(base + 8)
        off = D.u32(base + 12)
        if off > 0 and size > 0 and off + size <= len(data):
            entries.append((w, h, bits, off, size))
    if not entries:
        return None

    entries.sort(key=lambda e: (-(e[0] * e[1]), -e[2]))
    for _, _, _, off, size in entries:
        chunk = data[off:off + size]
        if chunk[0:4] == b"\x89PNG":
            if png_golden is not None:
                got = png_golden(chunk, max_w, max_h)
                if got is not None:
                    return got
        else:
            got = ref_decode_dib(chunk, max_w, max_h)
            if got is not None:
                return got
    return None


# ---------------------------------------------------------------------------
# ICO/DIB builders.

def le16(v):
    return struct.pack("<H", v & 0xFFFF)


def le32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def dib_header(width, height_signed, bpp):
    return (le32(40) + le32(width) + le32(height_signed & 0xFFFFFFFF) +
            le16(1) + le16(bpp) + le32(0) + le32(0) + le32(0) +
            le32(0) + le32(0))


def build_dib(width, height_signed, bpp, pix_fn, mask_bits=frozenset(),
              palette_fn=None, compression=0, omit_mask=False):
    """pix_fn(x,y) -> (r,g,b,a) truecolor or palette index when indexed.
    Positive heights store rows bottom-up; negative heights (raw value as
    stored) use the BMP top-down convention."""
    row_bytes = ((bpp * width + 31) // 32) * 4
    and_row_bytes = ((width + 31) // 32) * 4
    top_down = height_signed < 0
    h_abs = abs(height_signed)

    body = bytearray()
    body += dib_header(width, height_signed, bpp)
    if compression != 0:
        body[16:20] = le32(compression)
    if bpp <= 8:
        n = 1 << bpp
        for i in range(n):
            r, g, b = palette_fn(i) if palette_fn else (0, 0, 0)
            body += bytes([b, g, r, 0])

    for row in range(h_abs):
        y = row if top_down else (h_abs - 1 - row)
        line = bytearray(row_bytes)
        if bpp == 32:
            for x in range(width):
                r, g, b, a = pix_fn(x, y)
                line[x * 4:x * 4 + 4] = bytes([b, g, r, a])
        elif bpp == 24:
            for x in range(width):
                r, g, b, _ = pix_fn(x, y)
                line[x * 3:x * 3 + 3] = bytes([b, g, r])
        elif bpp == 8:
            for x in range(width):
                line[x] = pix_fn(x, y) & 0xFF
        elif bpp == 4:
            for x in range(width):
                idx = pix_fn(x, y) & 0x0F
                if x % 2 == 0:
                    line[x >> 1] |= idx << 4
                else:
                    line[x >> 1] |= idx
        else:
            for x in range(width):
                if pix_fn(x, y) & 1:
                    line[x >> 3] |= 1 << (7 - (x % 8))
        body += line

    if not omit_mask:
        for row in range(h_abs):
            y = row if top_down else (h_abs - 1 - row)
            line = bytearray(and_row_bytes)
            for x in range(width):
                if (x, y) in mask_bits:
                    line[x >> 3] |= 1 << (7 - (x % 8))
            body += line
    return bytes(body)


def build_ico(entries, ftype=1, reserved=0, count_override=None):
    """entries: list of dicts {blob, w, h, bits}."""
    count = len(entries) if count_override is None else count_override
    out = bytearray(le16(reserved) + le16(ftype) + le16(count))
    offset = 6 + 16 * len(entries)
    for e in entries:
        out += bytes([e["w"] % 256, e["h"] % 256, 0, 0])
        out += le16(1) + le16(e["bits"])
        out += le32(len(e["blob"])) + le32(offset)
        offset += len(e["blob"])
    for e in entries:
        out += e["blob"]
    return bytes(out)


FIXTURES = {}   # name -> dict(bytes=..., golden=(w,h,grid))
GUARDS = []     # (name, blob) expected to be rejected


def solid_png(width, height, color):
    im = Image.new("RGB", (width, height), color)
    bio = io.BytesIO()
    im.save(bio, format="PNG")
    return bio.getvalue()


def main():
    # --- fx_i_i_32bpp naming below keeps FX_I_ prefix consistent.

    # 16x16 truecolor, varying alpha incl. A=0, zeroed AND mask present.
    w, h = 16, 16

    def px32(x, y):
        return ((x * 17) % 256, (y * 13 + 40) % 256,
                (x * y * 7 + x) % 256, (x * 23 + y * 9) % 256)

    blob32 = build_dib(w, 2 * h, 32, px32)
    add_name = "i_32bpp"
    FIXTURES[add_name] = {
        "bytes": build_ico([{"blob": blob32, "w": w, "h": h, "bits": 32}]),
        "golden": ref_decode_dib(blob32, 360, 200)}

    # 12x10 truecolor with AND-mask holes at known coords.
    w, h = 12, 10
    mask = {(2, 1), (5, 4), (8, 7)}

    def px24(x, y):
        return ((x * 29 + 10) % 256, (y * 37) % 256,
                (x * y * 11 + 90) % 256, 255)

    blob24 = build_dib(w, 2 * h, 24, px24, mask_bits=mask)
    FIXTURES["i_24bpp_masked"] = {
        "bytes": build_ico([{"blob": blob24, "w": w, "h": h, "bits": 24}]),
        "golden": ref_decode_dib(blob24, 360, 200)}

    # 20x14 palette-indexed WITHOUT trailing mask rows.
    w, h = 20, 14
    blob8n = build_dib(w, 2 * h, 8, lambda x, y: (x * 3 + y * 5) % 64,
                       palette_fn=lambda i: ((i * 41) % 256,
                                             (i * 97 + 30) % 256,
                                             (i * 13) % 256),
                       omit_mask=True)
    FIXTURES["i_8bpp_nomask"] = {
        "bytes": build_ico([{"blob": blob8n, "w": w, "h": h, "bits": 8}]),
        "golden": ref_decode_dib(blob8n, 360, 200)}

    # 9x7 odd width nibble packing.
    w, h = 9, 7
    blob4 = build_dib(w, 2 * h, 4, lambda x, y: (x * 5 + y * 3) % 16,
                      palette_fn=lambda i: ((i * 61) % 256,
                                            (i * 23) % 256,
                                            (200 - i * 15) % 256))
    FIXTURES["i_4bpp"] = {
        "bytes": build_ico([{"blob": blob4, "w": w, "h": h, "bits": 4}]),
        "golden": ref_decode_dib(blob4, 360, 200)}

    # 11x5 monochrome checkerboard.
    w, h = 11, 5
    blob1 = build_dib(w, 2 * h, 1, lambda x, y: (x + y) % 2,
                      palette_fn=lambda i: (255, 255, 255) if i
                      else (10, 20, 30))
    FIXTURES["i_1bpp"] = {
        "bytes": build_ico([{"blob": blob1, "w": w, "h": h, "bits": 1}]),
        "golden": ref_decode_dib(blob1, 360, 200)}

    # Negative height stores rows top-down.
    w, h = 10, 8
    blobtop = build_dib(w, -2 * h, 32,
                        lambda x, y: ((x * 19) % 256, (y * 53) % 256,
                                      (x + y * 7) % 256, 255))
    FIXTURES["i_topdown"] = {
        "bytes": build_ico([{"blob": blobtop, "w": w, "h": h,
                             "bits": 32}]),
        "golden": ref_decode_dib(blobtop, 360, 200)}

    # 400x300 downscales through nearest sampling to 266x200.
    blobscaled = build_dib(
        400, 600, 8, lambda x, y: (x * 7 + y * 3) % 256,
        palette_fn=lambda i: (i & 0xFF, (255 - i) & 0xFF,
                              (i * 3) % 256))
    FIXTURES["i_scaled"] = {
        "bytes": build_ico([{"blob": blobscaled, "w": 144, "h": 44,
                             "bits": 8}]),
        "golden": ref_decode_dib(blobscaled, 360, 200)}

    # Real PNG payload wins over smaller DIB decoy.
    png_blob = solid_png(20, 20, (80, 160, 240))
    decoy = build_dib(8, 16, 32, lambda x, y: (250, 250, 250, 255))

    def png_golden_const(chunk, max_w, max_h):
        g = rgb_to_gray(80, 160, 240)
        return 20, 20, [[g] * 20 for _ in range(20)]

    FIXTURES["i_png_entry"] = {
        "bytes": build_ico([
            {"blob": decoy, "w": 8, "h": 8, "bits": 32},
            {"blob": png_blob, "w": 20, "h": 20, "bits": 32}]),
        "golden": ref_decode_ico(
            build_ico([
                {"blob": decoy, "w": 8, "h": 8, "bits": 32},
                {"blob": png_blob, "w": 20, "h": 20, "bits": 32}]),
            360, 200, png_golden=png_golden_const)}

    # Broken PNG body first, good DIB second: entry loop falls through.
    badpng = b"\x89PNG\r\n\x1a\n" + b"\xde\xad\xbe" * 9
    w, h = 12, 9
    blobfb = build_dib(w, 2 * h, 24,
                       lambda x, y: ((x * 13 + 5) % 256, (y * 71) % 256,
                                     (x * y * 3) % 256, 255))
    FIXTURES["i_png_fallback"] = {
        "bytes": build_ico([
            {"blob": badpng, "w": 32, "h": 32, "bits": 32},
            {"blob": blobfb, "w": w, "h": h, "bits": 24}]),
        "golden": ref_decode_dib(blobfb, 360, 200)}

    # Area tie resolved by bpp desc: 32bpp beats 4bpp beats 8bpp.
    w, h = 16, 16
    blobm32 = build_dib(w, 2 * h, 32,
                        lambda x, y: ((x * 31 + y) % 256, (y * 29) % 256,
                                      (x ^ y) % 256, 255))
    FIXTURES["i_multi"] = {
        "bytes": build_ico([
            {"blob": build_dib(8, 16, 8, lambda x, y: (x + y) % 64,
                               palette_fn=lambda i: ((i * 4) % 256,) * 3),
             "w": 8, "h": 8, "bits": 8},
            {"blob": build_dib(w, 2 * h, 4, lambda x, y: (x * y) % 16,
                               palette_fn=lambda i:
                                   ((i * 17) % 256,) * 3),
             "w": w, "h": h, "bits": 4},
            {"blob": blobm32, "w": w, "h": h, "bits": 32}]),
        "golden": ref_decode_dib(blobm32, 360, 200)}

    # fileType 2 (cursor) accepted; directory bit depth ignored.
    w, h = 7, 5
    blobcur = build_dib(w, 2 * h, 24,
                        lambda x, y: ((x * 91) % 256,
                                      (y * 33 + 7) % 256,
                                      (x * y * 5 + 3) % 256, 255))
    FIXTURES["i_cursor"] = {
        "bytes": build_ico([{"blob": blobcur, "w": w, "h": h,
                             "bits": 0}], ftype=2),
        "golden": ref_decode_dib(blobcur, 360, 200)}

    # --- guards: containers that must be rejected outright.
    good4 = build_dib(4, 8, 32, lambda x, y: (1, 2, 3, 255))
    GUARDS.append(("bad_reserved",
                   build_ico([{"blob": good4, "w": 4, "h": 4, "bits": 32}],
                             reserved=1)))
    GUARDS.append(("bad_type",
                   build_ico([{"blob": good4, "w": 4, "h": 4, "bits": 32}],
                             ftype=3)))
    GUARDS.append(("count_zero",
                   build_ico([], ftype=1, count_override=0) + good4))
    full = build_ico([{"blob": good4, "w": 4, "h": 4, "bits": 32}])
    GUARDS.append(("no_entries", full[:len(full) - len(good4)]))
    GUARDS.append(("all_fail_dib",
                   build_ico([{"blob":
                               build_dib(6, 12, 8, lambda x, y: 0,
                                         palette_fn=lambda i: (9, 9, 9),
                                         compression=1),
                               "w": 6, "h": 6, "bits": 8}])))

    # ------------------------------------------------------------------
    probe_coords = [(0, 0), (1, 0), (0, 1), (2, 2), (5, 4), (7, 3),
                    (6, 5), (9, 2), (2, 1), (8, 7), (10, 8), (3, 6)]

    h_parts = []
    c_parts = []
    h_parts.append("// GENERATED by tools/gen_ico_fixtures.py - do not edit.\n")
    h_parts.append("#ifndef PLUTO_SELFTEST_ICO_FIXTURES_H\n")
    h_parts.append("#define PLUTO_SELFTEST_ICO_FIXTURES_H\n\n")
    h_parts.append("#include <stddef.h>\n#include <stdint.h>\n\n")
    h_parts.append("typedef struct { int x, y, v; } IcoProbe;\n\n")
    c_parts.append("// GENERATED by tools/gen_ico_fixtures.py - do not edit.\n")
    c_parts.append('#include "render/decoders/selftest_ico_fixtures.h"\n\n')

    for name in sorted(FIXTURES):
        fx = FIXTURES[name]
        blob = fx["bytes"]
        gw, gh, grid = fx["golden"]
        up = name.upper()
        arr = ", ".join("0x%02X" % b for b in blob)
        c_parts.append("const unsigned char fx_%s[] = {%s};\n"
                       % (name, arr))
        c_parts.append("const int fx_%s_len = %d;\n" % (name, len(blob)))
        h_parts.append("extern const unsigned char fx_%s[];\n" % name)
        h_parts.append("#define FX_I_%s_LEN %d\n" % (up, len(blob)))
        h_parts.append("#define FX_I_%s_W %d\n" % (up, gw))
        h_parts.append("#define FX_I_%s_H %d\n" % (up, gh))
        h_parts.append("#define FX_I_%s_CK 0x%08Xu\n"
                       % (up, fnv_grid(grid)))

        pts = []
        seen = set()
        for x, y in probe_coords:
            if x < gw and y < gh and (x, y) not in seen:
                seen.add((x, y))
                pts.append((x, y, grid[y][x]))
        pname = "fx_ip_" + name
        items = ", ".join("{%d,%d,%d}" % p for p in pts)
        c_parts.append("const IcoProbe %s[] = {%s};\n" % (pname, items))
        c_parts.append("const int %s_len = %d;\n\n" % (pname, len(pts)))
        h_parts.append("extern const IcoProbe %s[];\n" % pname)
        h_parts.append("extern const int %s_len;\n" % pname)

    for name, blob in GUARDS:
        up = name.upper()
        arr = ", ".join("0x%02X" % b for b in blob)
        c_parts.append("const unsigned char fx_ig_%s[] = {%s};\n"
                       % (name, arr))
        c_parts.append("const int fx_ig_%s_len = %d;\n\n"
                       % (name, len(blob)))
        h_parts.append("extern const unsigned char fx_ig_%s[];\n" % name)
        h_parts.append("#define FX_IG_%s_LEN %d\n" % (up, len(blob)))

    h_parts.append("\n#endif\n")

    with open(OUT_H, "w") as f:
        f.write("".join(h_parts))
    with open(OUT_C, "w") as f:
        f.write("".join(c_parts))
    print("wrote", OUT_H)
    print("wrote", OUT_C)


if __name__ == "__main__":
    main()
