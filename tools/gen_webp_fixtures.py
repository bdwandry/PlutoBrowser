#!/usr/bin/env python3
"""Generates selftest_webp_fixtures.{h,c} for [P21] webp.c selftests.

Builds small lossless WebP images with Pillow, cross-validates every file
against `dwebp -pam` (libwebp reference decoder), then emits byte arrays
plus full-resolution ARGB goldens (FNV-1a checksums + probe points).
Lossless roundtrip is exact, so goldens are bit-exact expectations.
"""

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

    encoded = []
    for name, im in imgs:
        import io
        buf = io.BytesIO()
        im.save(buf, format="WEBP", lossless=True)
        data = buf.getvalue()

        # Pillow ground truth
        rgbaw, rgbah = im.size
        pil_bytes = im.tobytes()

        # libwebp reference decode + cross-check. Lossless roundtrip is
        # exact except RGB under A=0 (encoder non-exact mode zeroes it),
        # so diffs are allowed only on fully-transparent pixels.
        dw, dh, draw = dwebp_rgba(data)
        if (dw, dh) != (rgbaw, rgbah):
            raise RuntimeError(f"{name}: dims differ PIL vs dwebp")
        for i in range(0, len(draw), 4):
            if draw[i + 3] != 0 or pil_bytes[i + 3] != 0:
                if draw[i:i + 4] != pil_bytes[i:i + 4]:
                    raise RuntimeError(
                        f"{name}: opaque pixel {i // 4} differs "
                        f"PIL={tuple(pil_bytes[i:i + 4])} "
                        f"dwebp={tuple(draw[i:i + 4])}")

        print(f"fx_w_{name}: {len(data)} bytes, {dw}x{dh}")
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

    return encoded


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


def emit(fixtures):
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
    h_parts.append("#endif\n")

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

    with open(H_PATH, "w") as f:
        f.write("".join(h_parts))
    with open(C_PATH, "w") as f:
        f.write("".join(c_parts))
    print(f"wrote {H_PATH}")
    print(f"wrote {C_PATH}")


def main():
    fixtures = build_images()
    emit(fixtures)


if __name__ == "__main__":
    main()
