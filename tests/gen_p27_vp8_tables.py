#!/usr/bin/env python3
"""Transpile webp_vp8_data.lua (libwebp v1.6.0 VP8 tables) -> webp_vp8_data.c/.h.

Mechanical, byte-exact transcription. The script PARSES the Lua reference
(no hand copying), asserts the expected element counts per table, and emits
C arrays preserving the exact element order (Lua 1-based indexing -> C 0-based
does not reorder elements). Per-table checksums are printed so the battery
test (Source/main.c p27 battery) can assert the same sums at runtime.
"""
import re

REF = '/Users/bwandrych/Desktop/CometBrowser/Source/render/decoders/webp_vp8_data.lua'
OUT_C = 'Source/render/decoders/webp_vp8_data.c'
OUT_H = 'Source/render/decoders/webp_vp8_data.h'

# (lua name, C type, expected flat element count, C name)
TABLES = [
    ('kDcTable',         'uint8_t',  128,  'vp8_dc_table'),
    ('kAcTable',         'uint16_t', 128,  'vp8_ac_table'),       # values up to 284
    ('kZigzag',          'uint8_t',  16,   'vp8_zigzag'),
    ('kCat3456',         'uint8_t',  48,   'vp8_cat3456'),        # [4][12] zero-padded rows
    ('kBands',           'uint8_t',  17,   'vp8_bands'),
    ('kScan',            'uint16_t', 16,   'vp8_scan'),           # values up to 396
    ('kYModesIntra4',    'int8_t',   18,   'vp8_ymodes_intra4'),  # signed tree values
    ('kBModesProba',     'uint8_t',  900,  'vp8_bmodes_proba'),   # 100 rows x 9
    ('CoeffsProba0',     'uint8_t',  1056, 'vp8_coeffs_proba0'),  # 96 rows x 11
    ('CoeffsUpdateProba','uint8_t',  1056, 'vp8_coeffs_update_proba'),
]

src = open(REF).read()

def parse_flat(name):
    """Parse a flat Lua table { a, b, ... } into a python int list."""
    i = src.index(name + ' = {')
    i = src.index('{', i)
    depth = 0
    j = i
    while True:
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = src[i + 1:j]
    return [int(t) for t in re.findall(r'-?\d+', body)]

tables = {}
for name, ctype, count, cname in TABLES:
    if name == 'kCat3456':
        i = src.index(name + ' = {')
        seg = src[i:src.index('kBands = {')]
        rows = []
        for m in re.finditer(r'\{([^}]*)\}', seg):
            rows.append([int(t) for t in re.findall(r'-?\d+', m.group(1))])
        assert len(rows) == 4 and [len(r) for r in rows] == [4, 5, 6, 12], rows
        flat = []
        for r in rows:
            flat.extend(r)
            flat.extend([0] * (12 - len(r)))
        vals = flat
    else:
        vals = parse_flat(name)
    assert len(vals) == count, f"{name}: got {len(vals)} want {count}"
    tables[name] = (ctype, count, cname, vals)
    print(f"{name}: {count} elems, sum={sum(vals)}")

def fmt(vals, per_line, indent):
    return '\n'.join(
        indent + ', '.join(str(v) for v in vals[k:k + per_line]) + ','
        for k in range(0, len(vals), per_line))

h = []
h.append('/*')
h.append(' * PlutoBrowser — webp_vp8_data.h')
h.append(' * Transcribed from Source/render/decoders/webp_vp8_data.lua')
h.append(' * (generated from libwebp v1.6.0 decoder tables — DO NOT hand-edit;')
h.append(' *  regenerate with tests/gen_p27_vp8_tables.py).')
h.append(' *')
h.append(' * Lua -> C map (element order preserved; Lua 1-based -> C 0-based):')
for name, (ctype, count, cname, vals) in tables.items():
    h.append(f' *   VP8Tables.{name:20s} -> {cname}[{count}] ({ctype})')
h.append(' * kCat3456 rows are zero-padded to 12 entries as a [4][12] 2-D array; the')
h.append(' * decoder reads a row until the 0 terminator exactly like the Lua reference.')
h.append(' */')
h.append('#ifndef PLUTO_WEBP_VP8_DATA_H')
h.append('#define PLUTO_WEBP_VP8_DATA_H')
h.append('')
h.append('#include <stdint.h>')
h.append('')
h.append('#define VP8_CAT3456_ROW 12')
h.append('')
for name, (ctype, count, cname, vals) in tables.items():
    if name == 'kCat3456':
        h.append(f'extern const {ctype} {cname}[4][VP8_CAT3456_ROW];')
    else:
        h.append(f'extern const {ctype} {cname}[{count}];')
h.append('')
h.append('#endif /* PLUTO_WEBP_VP8_DATA_H */')
open(OUT_H, 'w').write('\n'.join(h) + '\n')

c = []
c.append('/*')
c.append(' * PlutoBrowser — webp_vp8_data.c')
c.append(' * VP8 decoder constant tables, transcribed 1:1 from the Lua reference')
c.append(' * (originally generated from libwebp v1.6.0). See webp_vp8_data.h.')
c.append(' */')
c.append('#include "webp_vp8_data.h"')
c.append('')
for name, (ctype, count, cname, vals) in tables.items():
    c.append(f'/* VP8Tables.{name} — {count} elements, Lua-order preserved */')
    if name == 'kCat3456':
        c.append(f'const {ctype} {cname}[4][VP8_CAT3456_ROW] = {{')
        for r in range(4):
            row = vals[r * 12:(r + 1) * 12]
            c.append('    { ' + ', '.join(str(v) for v in row) + ' },')
        c.append('};')
    else:
        c.append(f'const {ctype} {cname}[{count}] = {{')
        c.append(fmt(vals, 8 if count <= 32 else 16, '    '))
        c.append('};')
    c.append('')
open(OUT_C, 'w').write('\n'.join(c) + '\n')

print("\nChecksums for battery assertions:")
for name, (ctype, count, cname, vals) in tables.items():
    print(f"  {cname}: sum={sum(vals)}")
print("done")
