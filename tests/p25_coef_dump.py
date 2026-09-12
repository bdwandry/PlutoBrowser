#!/usr/bin/env python3
"""Mini baseline-JPEG coefficient decoder for single-MCU gray test files.
Prints the dequantized zigzag coefficients of block 0 — per-coefficient truth
to diff against the C decoder's block[] dump."""
import sys

def main(path):
    data = open(path, 'rb').read()
    i = 2
    dqt = {}
    huff = {0: {}, 1: {}}  # class -> id -> (counts, values)
    frame = scan = None
    while i < len(data) - 1:
        assert data[i] == 0xFF, hex(i)
        m = data[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        ln = (data[i + 2] << 8) | data[i + 3]
        body = data[i + 4:i + 2 + ln]
        if m == 0xDB:
            dqt[body[0] & 0x0F] = list(body[1:])
        elif m == 0xC4:
            p = 0
            while p < len(body):
                cls, cid = body[p] >> 4, body[p] & 0x0F
                counts = list(body[p + 1:p + 17])
                total = sum(counts)
                vals = list(body[p + 17:p + 17 + total])
                huff[cls][cid] = (counts, vals)
                p += 17 + total
        elif m in (0xC0, 0xC1):
            frame = {'prec': body[0], 'h': (body[1] << 8) | body[2],
                     'w': (body[3] << 8) | body[4], 'n': body[5],
                     'comps': [(body[6 + 3 * k], body[7 + 3 * k] >> 4,
                                body[7 + 3 * k] & 15, body[8 + 3 * k])
                               for k in range(body[5])]}
        elif m == 0xDA:
            ns = body[0]
            scomps = [(body[1 + 2 * k], body[2 + 2 * k] >> 4, body[2 + 2 * k] & 15)
                      for k in range(ns)]
            scan = {'comps': scomps, 'ss': body[1 + 2 * ns],
                    'se': body[2 + 2 * ns], 'ahal': body[3 + 2 * ns]}
            i += 2 + ln
            break
        i += 2 + ln

    entropy = data[i:]

    class BR:
        def __init__(self, b): self.b = b; self.p = 0; self.bits = ''
        def bit(self):
            while not self.bits:
                if self.p >= len(self.b): raise EOFError
                x = self.b[self.p]; self.p += 1
                if x == 0xFF:
                    assert self.b[self.p] == 0x00, hex(self.b[self.p])
                    self.p += 1
                self.bits = format(x, '08b')
            v = self.bits[0]; self.bits = self.bits[1:]
            return int(v)
        def n(self, cnt):
            v = 0
            for _ in range(cnt): v = (v << 1) | self.bit()
            return v

    def build(counts, vals):
        codes = {}
        code = 0; k = 0
        for l in range(1, 17):
            for _ in range(counts[l - 1]):
                codes[(l, code)] = vals[k]; code += 1; k += 1
            code <<= 1
        return codes

    br = BR(entropy)
    def sym(tbl):
        code = 0
        for l in range(1, 17):
            code = (code << 1) | br.bit()
            if (l, code) in tbl: return tbl[(l, code)]
        raise ValueError('bad code')

    def extend(v, s):
        return v - (1 << s) + 1 if v < (1 << (s - 1)) else v

    zz = [0] * 64  # zigzag order index -> coefficient
    comp = scan['comps'][0]
    dcs, acs = build(*huff[0][comp[1]]), build(*huff[1][comp[2]])
    s = sym(dcs)
    diff = 0 if s == 0 else extend(br.n(s), s)
    dc = diff
    qtz = dqt[frame['comps'][0][3]]
    zz[0] = dc * qtz[0]
    k = 1
    while k <= 63:
        rs = sym(acs)
        r, cs = rs >> 4, rs & 15
        if cs == 0:
            if r == 15: k += 16; continue
            break
        k += r
        v = extend(br.n(cs), cs)
        ZIGZAG = [0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,
                  41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,
                  15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,
                  55,62,63]
        zz[ZIGZAG[k]] = v * qtz[k]
        k += 1
    print('DC diff:', diff, 'DC:', dc, 'q:', qtz[0])
    print('coeffs (natural order, dequantized):')
    for r in range(8):
        print(' '.join(f'{zz[r*8+c]:6d}' for c in range(8)))

if __name__ == '__main__':
    main(sys.argv[1])
