#!/usr/bin/env python3
"""Generates selftest_webp_fixtures.{h,c} for [P21]/[P22] webp.c selftests.

Builds small WebP images with Pillow, cross-validates every still file
against `dwebp -pam` (libwebp reference decoder), then emits byte arrays
plus full-resolution ARGB goldens (FNV-1a checksums + probe points).
Lossless roundtrip is exact, so goldens are bit-exact expectations;
for lossy fixtures the dwebp output IS the golden (quality 90).

[P22] adds lossy stills (VP8 / VP8+ALPH) and an animated file built from
lossless frames whose blended canvases are computed here using libwebp's
documented integer blend algorithm.
"""

import io
import os
import subprocess
import sys
import tempfile

from PIL import Image

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "src", "render", "decoders")
H_PATH = os.path.join(OUT_DIR, "selftest_webp_fixtures.h")
C_PATH = os.path.join(OUT_DIR, "selftest_webp_fixtures.c")

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv_argb(pixels):
    h = FNV_OFFSET
    for a, r, g, b in pixels:
        w = ((a << 24) | (r << 16) | (g << 8) | b) & 0xFFFFFFFF
        h ^= w
        h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h


def dwebp_rgba(data):
    """Decode with libwebp's reference decoder; returns (w, h, RGBA bytes)."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "in.webp")
        dst = os.path.join(td, "out.pam")
        with open(src, "wb") as f:
            f.write(data)
        res = subprocess.run(["dwebp", src, "-pam", "-o", dst],
                             capture_output=True)
        if res.returncode != 0:
            raise RuntimeError(f"dwebp failed: {res.stderr.decode()}")
        with open(dst, "rb") as f:
            blob = f.read()
    marker = b"ENDHDR\n"
    idx = blob.index(marker) + len(marker)
    header = blob[:idx].decode("ascii")
    fields = {}
    for line in header.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("WIDTH", "HEIGHT", "DEPTH"):
            fields[parts[0]] = int(parts[1])
    w, h, depth = fields["WIDTH"], fields["HEIGHT"], fields["DEPTH"]
    assert depth == 4, "expected RGBA PAM"
    raw = blob[idx:]
    assert len(raw) == w * h * 4, f"PAM size mismatch {len(raw)}"
    return w, h, raw


def build_images():
    imgs = []

    # flat: solid color
    im = Image.new("RGBA", (12, 9), (30, 144, 155, 255))
    imgs.append(("flat", im))

    # gradient: RGB gradient
    im = Image.new("RGBA", (48, 32))
    px = im.load()
    for y in range(32):
        for x in range(48):
            px[x, y] = (x * 5 % 256, y * 8 % 256, (x + y) * 2 % 256, 255)
    imgs.append(("gradient", im))

    # palette: few colors -> color-indexing transform territory
    im = Image.new("RGBA", (40, 28))
    px = im.load()
    pal = [(228, 40, 40, 255), (40, 200, 60, 255), (50, 60, 230, 255),
           (250, 240, 60, 255), (255, 255, 255, 255), (10, 10, 10, 255)]
    for y in range(28):
        for x in range(40):
            px[x, y] = pal[((x // 4) ^ (y // 3)) % len(pal)]
    imgs.append(("palette", im))

    # alpha: alpha ramp over varied RGB
    im = Image.new("RGBA", (24, 16))
    px = im.load()
    for y in range(16):
        for x in range(24):
            a = (x * 12) % 256
            px[x, y] = (220, 90, x * 9 % 256, a)
    imgs.append(("alpha", im))

    # predictor: smooth blobs -> predictor transform territory
    im = Image.new("RGBA", (64, 48))
    px = im.load()
    for y in range(48):
        for x in range(64):
            dx, dy = x - 24, y - 20
            d = min(255, int((dx * dx + dy * dy) ** 0.5))
            px[x, y] = (128 + d // 2, 200 - d // 2, 90 + d // 3, 255)
    imgs.append(("predictor", im))

    # cache: structured noise -> LZ77 / color cache activity
    rng = 12345
    im = Image.new("RGBA", (96, 72))
    px = im.load()
    for y in range(72):
        for x in range(96):
            rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
            v = (rng >> 16) & 0xFF
            px[x, y] = (v, (v * 3 + x) % 256, (v + y) % 256,
                        255 if (x + y) % 7 else 128)
    imgs.append(("cache", im))

    # meta: large multi-region image -> meta Huffman groups hoped for
    im = Image.new("RGBA", (300, 200))
    px = im.load()
    for y in range(200):
        for x in range(300):
            region = (x // 75 + y // 100) % 4
            if region == 0:
                c = ((x * 2) % 256, 40, (y * 2) % 256, 255)
            elif region == 1:
                c = (250, 250, 245, 255)
            elif region == 2:
                c = (20, (y + x) % 256, 180, 200)
            else:
                c = (90, 30, 200, 255)
            px[x, y] = c
    imgs.append(("meta", im))

    # pil_bench: photo-ish bench image
    im = Image.new("RGBA", (266, 200))
    px = im.load()
    for y in range(200):
        for x in range(266):
            r = (x * 19197 + y * 3) % 256
            g = (y * 23451 + x) % 256
            b = ((x ^ y) * 7) % 256
            px[x, y] = (r, g, b, 255)
    imgs.append(("pil_bench", im))

    # ---- P22: lossy stills (goldens = dwebp output, not source pixels)
    # lossy_flat: solid color
    im = Image.new("RGBA", (32, 24), (200, 30, 90, 255))
    imgs.append(("lossy_flat", im))

    # lossy_grad: RGB gradient
    im = Image.new("RGBA", (48, 32))
    px = im.load()
    for y in range(32):
        for x in range(48):
            px[x, y] = ((x * 11) % 256, (y * 13) % 256,
                        (x * 5 + y * 7) % 256, 255)
    imgs.append(("lossy_grad", im))

    # lossy_alpha: alpha ramp -> VP8 + ALPH chunk territory
    im = Image.new("RGBA", (24, 16))
    px = im.load()
    for y in range(16):
        for x in range(24):
            a = (x * 23 + y * 9) % 256
            px[x, y] = (30, (x * 7) % 256, (200 - y * 8) % 256, a)
    imgs.append(("lossy_alpha", im))

    # Per-image encoder options: everything defaults to lossless.
    save_kwargs = {
        "lossy_flat": dict(lossless=False, quality=90),
        "lossy_grad": dict(lossless=False, quality=90),
        "lossy_alpha": dict(lossless=False, quality=90),
    }

    encoded = []
    for name, im in imgs:
        buf = io.BytesIO()
        kw = save_kwargs.get(name, dict(lossless=True))
        im.save(buf, format="WEBP", **kw)
        data = buf.getvalue()

        # Pillow ground truth
        rgbaw, rgbah = im.size
        pil_bytes = im.tobytes()

        # libwebp reference decode. For lossless the roundtrip is exact
        # except RGB under A=0 (encoder non-exact mode zeroes it), so
        # diffs are allowed only on fully-transparent pixels. For lossy
        # the dwebp output simply becomes the golden.
        lossless = kw.get("lossless", True)
        dw, dh, draw = dwebp_rgba(data)
        if (dw, dh) != (rgbaw, rgbah):
            raise RuntimeError(f"{name}: dims differ PIL vs dwebp")
        if lossless:
            for i in range(0, len(draw), 4):
                if draw[i + 3] != 0 or pil_bytes[i + 3] != 0:
                    if draw[i:i + 4] != pil_bytes[i:i + 4]:
                        raise RuntimeError(
                            f"{name}: opaque pixel {i // 4} differs "
                            f"PIL={tuple(pil_bytes[i:i + 4])} "
                            f"dwebp={tuple(draw[i:i + 4])}")

        print(f"fx_w_{name}: {len(data)} bytes, {dw}x{dh}"
              f"{'' if lossless else ' (lossy)'}")
        encoded.append((name, data, dw, dh, draw))

    # guards
    name, data, *_ = encoded[1]
    bad = bytearray(b"RIFF\x00\x00\x00\x00WEBQxxxx")
    encoded.append(("badsig", bytes(bad), 0, 0, None))
    short = bytearray(b"RIFF\x24\x00\x00\x00WEBPVP8")
    encoded.append(("riff_short", bytes(short[:15]), 0, 0, None))
    trunc = bytearray(data)
    trunc = trunc[: len(trunc) // 2]
    encoded.append(("trunc", bytes(trunc), 0, 0, None))

    # truncated lossy file -> VP8 payload rejection via public API
    ldata = next(e[1] for e in encoded if e[0] == "lossy_grad")
    ltrunc = bytearray(ldata)[: len(ldata) // 3]
    encoded.append(("lossy_trunc", bytes(ltrunc), 0, 0, None))

    anim_data, anim_meta = build_anim_fixture()
    encoded.append(("anim", anim_data,
                    anim_meta["w"], anim_meta["h"], None))

    return encoded, anim_meta


# ---------------------------------------------------------------- P22 ----

def le24(v):
    return bytes([v & 255, (v >> 8) & 255, (v >> 16) & 255])


def le32(v):
    return bytes([v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255])


def riff_chunk(fourcc, payload):
    pad = b"\x00" if len(payload) & 1 else b""
    return fourcc + le32(len(payload)) + payload + pad


def encode_webp(im, **opts):
    buf = io.BytesIO()
    im.save(buf, format="WEBP", **opts)
    return buf.getvalue()


def blend_px(src, dst):
    """libwebp AnimDecoder integer blend (blend_func), ARGB words."""
    src_a = (src >> 24) & 255
    if src_a == 0:
        return dst
    dst_a = (dst >> 24) & 255
    dst_factor_a = (dst_a * (256 - src_a)) >> 8
    blend_a = src_a + dst_factor_a
    scale = 16777216 // blend_a
    out = [blend_a]
    for shift in (16, 8, 0):
        sc = (src >> shift) & 255
        dc = (dst >> shift) & 255
        c = ((sc * src_a + dc * dst_factor_a) * scale) >> 24
        if c > 255:
            c = 255
        out.append(c)
    return (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3]


def build_anim_fixture():
    """Hand-builds VP8X+ANIM+ANMF wrapping four lossless frames and
    computes the expected blended canvases with the documented integer
    algorithm (independent reimplementation of libwebp semantics)."""
    cw, chh = 24, 16
    bgcolor = (0x1E, 0xC8, 0x0A, 0x00)  # stored byte order: b, g, r, a
    loop_count = 3

    # Frame sources: (rect x, y, w, h, rgba, duration, dispose, noblend).
    # Spec stores x/y in units of 2 pixels, so both must be even.
    specs = [
        (0, 0, cw, chh, None, 100, 0, 0),      # full-canvas gradient
        (6, 4, 10, 10, (250, 40, 40, 128), 70, 0, 0),
        (2, 6, 8, 8, (30, 60, 190, 180), 50, 1, 0),
        (6, 8, 9, 7, (20, 170, 40, 160), 80, 0, 0),
    ]

    frames = []
    raw_frames = []
    for fx, fy, fw, fh, color, dur, disp, nob in specs:
        im = Image.new("RGBA", (fw, fh))
        px = im.load()
        for y in range(fh):
            for x in range(fw):
                if color is None:
                    px[x, y] = ((x * 37 + y * 11) % 256,
                                (y * 29 + x * 5) % 256,
                                (x * y * 3 + 17) % 256, 255)
                else:
                    px[x, y] = color
        data = encode_webp(im, lossless=True)
        # Extract the inner VP8L chunk from Pillow's simple container.
        pos = 12
        vpl = None
        while pos + 8 <= len(data):
            sz = int.from_bytes(data[pos + 4:pos + 8], "little")
            if data[pos:pos + 4] == b"VP8L":
                vpl = data[pos + 8:pos + 8 + sz]
                break
            pos += 8 + sz + (sz & 1)
        assert vpl is not None, "frame did not contain a VP8L chunk"
        raw_frames.append(im.tobytes())
        frames.append(dict(x=fx, y=fy, w=fw, h=fh, dur=dur, dispose=disp,
                           noblend=nob, vpl=vpl))

    body = b"WEBP"
    # Flags byte first (animation bit set), 3 reserved, canvas-1 LE24s.
    vp8x = bytes([0x02]) + b"\x00\x00\x00" + le24(cw - 1) + le24(chh - 1)
    body += riff_chunk(b"VP8X", vp8x)
    anim = bytes(bgcolor) + bytes([loop_count & 255,
                                   (loop_count >> 8) & 255]) + b"\x00\x00"
    body += riff_chunk(b"ANIM", anim)

    def has_alpha_px(rgba):
        return any(rgba[i + 3] != 255 for i in range(0, len(rgba), 4))

    prev_disposed = [[0] * cw for _ in range(chh)]
    prev_was_key = True
    canvases = []
    for i, fr in enumerate(frames):
        rgba = raw_frames[i]
        fa = has_alpha_px(rgba)
        if i == 0:
            key = True
        elif (not fa or fr["noblend"]) and fr["w"] == cw and fr["h"] == chh:
            key = True
        else:
            pf = frames[i - 1]
            key = bool(pf["dispose"]) and (pf["w"] == cw or prev_was_key)
        curr = ([[0] * cw for _ in range(chh)] if key
                else [row[:] for row in prev_disposed])
        for yy in range(fr["h"]):
            row = curr[fr["y"] + yy]
            base = (yy * fr["w"] + 0) * 4
            for xx in range(fr["w"]):
                j = base + xx * 4
                argb = ((rgba[j + 3] << 24) | (rgba[j] << 16) |
                        (rgba[j + 1] << 8) | rgba[j + 2])
                row[fr["x"] + xx] = argb
        if i > 0 and not fr["noblend"] and not key:
            p = frames[i - 1]

            def blend_range(y_, off, width_):
                for k in range(width_):
                    v = curr[y_][off + k]
                    if ((v >> 24) & 255) != 255:
                        curr[y_][off + k] = blend_px(v, prev_disposed[y_][off + k])

            for yy in range(fr["h"]):
                cy = fr["y"] + yy
                if not p["dispose"]:
                    blend_range(cy, fr["x"], fr["w"])
                else:
                    src_max_x = fr["x"] + fr["w"]
                    dst_max_x = p["x"] + p["w"]
                    dst_max_y = p["y"] + p["h"]
                    if (cy < p["y"] or cy >= dst_max_y or
                            fr["x"] >= dst_max_x or src_max_x <= p["x"]):
                        blend_range(cy, fr["x"], fr["w"])
                    else:
                        if fr["x"] < p["x"]:
                            blend_range(cy, fr["x"], p["x"] - fr["x"])
                        if src_max_x > dst_max_x:
                            blend_range(cy, dst_max_x, src_max_x - dst_max_x)
        canvases.append(curr)
        prev_disposed = [row[:] for row in curr]
        if fr["dispose"]:
            for yy in range(fr["h"]):
                for k in range(fr["w"]):
                    prev_disposed[fr["y"] + yy][fr["x"] + k] = 0
        prev_was_key = key

    anmf_blobs = []
    for fr in frames:
        flags = (1 if fr["dispose"] else 0) | \
                ((1 if fr["noblend"] else 0) << 1)
        head = (le24(fr["x"] // 2) + le24(fr["y"] // 2) +
                le24(fr["w"] - 1) + le24(fr["h"] - 1) +
                le24(fr["dur"]))
        anmf_payload = head + bytes([flags]) + \
            riff_chunk(b"VP8L", fr["vpl"])
        anmf_blobs.append(riff_chunk(b"ANMF", anmf_payload))

    body += b"".join(anmf_blobs)
    data = b"RIFF" + le32(len(body)) + body

    meta = {
        "w": cw, "h": chh,
        "loop": loop_count,
        "bg": (bgcolor[3] << 24) | (bgcolor[2] << 16) | \
              (bgcolor[1] << 8) | bgcolor[0],
        "nframes": len(frames),
        "durs": [f["dur"] for f in frames],
        "canvases": canvases,
    }
    print(f"fx_w_anim: {len(data)} bytes, {cw}x{chh}, "
          f"{len(frames)} frames (hand-built)")
    return data, meta


def frame_fnv(canvas):
    h = FNV_OFFSET
    for row in canvas:
        for w_ in row:
            h ^= w_
            h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h


def argb_pixels(w, h, rgba):
    out = []
    for i in range(w * h):
        r, g, b, a = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], \
            rgba[i * 4 + 3]
        out.append((a, r, g, b))
    return out


def gen_probes(idx, w, h, pixels):
    pts = []
    coords = [
        (0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
        (w // 2, h // 2), (w // 4, h // 4), (3 * w // 4, h // 4),
        (w // 4, 3 * h // 4), (3 * w // 4, 3 * h // 4),
    ]
    for k in range(3):
        coords.append(((k + 1) * w // 4, (2 * k + 1) * h // 3))
    seen = set()
    for x, y in coords:
        x = max(0, min(w - 1, x))
        y = max(0, min(h - 1, y))
        if (x, y) in seen:
            continue
        seen.add((x, y))
        a, r, g, b = pixels[y * w + x]
        pts.append((x, y, ((a << 24) | (r << 16) | (g << 8) | b)))
    return pts


def emit(fixtures, anim_meta):
    decodable = []
    for idx, (name, data, w, h, rgba) in enumerate(fixtures):
        if rgba is not None:
            decodable.append((idx, name, data, w, h, rgba))

    h_parts = []
    c_parts = []
    h_parts.append("// Generated by tools/gen_webp_fixtures.py — do not edit.\n\n")
    h_parts.append("#ifndef PLUTO_RENDER_DECODERS_SELFTEST_WEBP_FIXTURES_H\n")
    h_parts.append("#define PLUTO_RENDER_DECODERS_SELFTEST_WEBP_FIXTURES_H\n\n")
    h_parts.append("#include <stddef.h>\n\n")
    h_parts.append("typedef struct { int x, y; unsigned argb; } WebpProbePt;\n")
    h_parts.append("typedef struct { int img, nProbes; "
                   "const WebpProbePt* p; } WebpProbe;\n\n")

    names = []
    for idx, (name, data, w, h, rgba) in enumerate(fixtures):
        lines = []
        for i in range(0, len(data), 12):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 12])
            lines.append("    " + chunk + ",")
        body = "\n".join(lines)
        arr = f"fx_w_{name}"
        h_parts.append(f"static const unsigned char {arr}[] = {{\n{body}\n}};\n")
        h_parts.append(f"#define FX_W_{name.upper()}_LEN {len(data)}\n")
        h_parts.append(f"#define FX_W_{name.upper()}_W {w}\n")
        h_parts.append(f"#define FX_W_{name.upper()}_H {h}\n")
        if rgba is not None:
            ck = fnv_argb(argb_pixels(w, h, rgba))
            h_parts.append(f"#define FX_W_{name.upper()}_CK 0x{ck:08X}u\n")
        h_parts.append("\n")
        names.append(arr)

    probe_defs = []
    probe_tables = []
    for idx, name, data, w, h, rgba in decodable:
        pixels = argb_pixels(w, h, rgba)
        pts = gen_probes(idx, w, h, pixels)
        pname = f"wp_{name}"
        pt_items = ", ".join(f"{{{x},{y},0x{argb:08X}u}}" for x, y, argb in pts)
        probe_defs.append(
            f"static const WebpProbePt {pname}[] = {{{pt_items}}};\n")
        probe_tables.append(f"    {{{idx}, {len(pts)}, {pname}}},")

    h_parts.append("extern const WebpProbe webp_probes[];\n")
    h_parts.append("extern const int webp_probes_len;\n\n")

    c_parts.append("// Generated by tools/gen_webp_fixtures.py — do not edit.\n\n")
    c_parts.append('#include "render/decoders/selftest_webp_fixtures.h"\n\n')
    c_parts.append("#include <stddef.h>\n\n")
    for line in probe_defs:
        c_parts.append(line)
    c_parts.append("\nconst WebpProbe webp_probes[] = {\n")
    for line in probe_tables:
        c_parts.append("    " + line + "\n")
    c_parts.append("};\n\n")
    c_parts.append(f"const int webp_probes_len = {len(probe_tables)};\n")
    c_parts.append("\n")

    # ---- P22: animation metadata + per-frame probe tables ------------
    am = anim_meta
    h_parts.append(f"#define FX_W_ANIM_LOOP {am['loop']}\n")
    h_parts.append(f"#define FX_W_ANIM_BG 0x{am['bg']:08X}u\n")
    h_parts.append(f"#define FX_W_ANIM_NFRAMES {am['nframes']}\n")
    for k, d in enumerate(am["durs"]):
        h_parts.append(f"#define FX_W_ANIM_DUR{k} {d}\n")

    cw_, chh_ = am["w"], am["h"]
    anim_coords = [(0, 0), (cw_ - 1, 0), (0, chh_ - 1), (cw_ - 1, chh_ - 1),
                   (cw_ // 2, chh_ // 2), (6, 4), (18, 10), (9, 13),
                   (20, 2), (3, 6), (14, 12), (11, 8)]
    for k, canvas in enumerate(am["canvases"]):
        ckv = frame_fnv(canvas)
        h_parts.append(f"#define FX_W_ANIM_F{k}_CK 0x{ckv:08X}u\n")
        seen = set()
        pts = []
        for x, y in anim_coords:
            if (x, y) in seen:
                continue
            seen.add((x, y))
            pts.append((x, y, canvas[y][x]))
        pname = f"wp_anim_f{k}"
        pt_items = ", ".join(f"{{{x},{y},0x{argb:08X}u}}"
                             for x, y, argb in pts)
        c_parts.append(
            f"const WebpProbePt {pname}[] = {{{pt_items}}};\n")
        c_parts.append(f"const int wp_anim_f{k}_len = {len(pts)};\n\n")
        h_parts.append(f"extern const WebpProbePt {pname}[];\n")
        h_parts.append(f"extern const int wp_anim_f{k}_len;\n")
    h_parts.append("\n#endif\n")

    with open(H_PATH, "w") as f:
        f.write("".join(h_parts))
    with open(C_PATH, "w") as f:
        f.write("".join(c_parts))
    print(f"wrote {H_PATH}")
    print(f"wrote {C_PATH}")


def main():
    fixtures, anim_meta = build_images()
    emit(fixtures, anim_meta)


if __name__ == "__main__":
    main()
