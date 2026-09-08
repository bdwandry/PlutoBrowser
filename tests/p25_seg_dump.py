#!/usr/bin/env python3
"""Dump every JPEG segment (DQT/DHT fully expanded) for vector forensics."""
import sys

def main(path):
    data = open(path, 'rb').read()
    i = 2
    while i < len(data) - 1:
        if data[i] != 0xFF:
            print(f'{i:#x}: NON-MARKER {data[i]:#x} — stopping')
            break
        m = data[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            print(f'{i:#x}: marker {m:#04x}')
            i += 2
            if m == 0xD9:
                break
            continue
        ln = (data[i + 2] << 8) | data[i + 3]
        body = data[i + 4:i + 2 + ln]
        print(f'{i:#x}: marker {m:#04x} len {ln}')
        if m == 0xDB:
            tid = body[0] & 0x0F
            prec = body[0] >> 4
            print(f'  DQT id={tid} prec={prec} q[0..7]={list(body[1:9])}')
        elif m == 0xC4:
            p = 0
            while p < len(body):
                tch = body[p]
                cls, tid = tch >> 4, tch & 0x0F
                counts = list(body[p + 1:p + 17])
                total = sum(counts)
                vals = list(body[p + 17:p + 17 + total])
                avail = len(body) - (p + 17)
                print(f'  DHT class={cls} id={tid} counts={counts} '
                      f'sum={total} vals_available={avail} vals={vals}')
                p += 17 + total
        elif m in (0xC0, 0xC1, 0xC2):
            print(f'  SOF prec={body[0]} h={(body[1]<<8)|body[2]} '
                  f'w={(body[3]<<8)|body[4]} n={body[5]} '
                  f'comps={[(body[6+3*k], body[7+3*k]>>4, body[7+3*k]&15, body[8+3*k]) for k in range(body[5])]}')
        elif m == 0xDA:
            ns = body[0]
            print(f'  SOS comps={[(body[1+2*k], body[2+2*k]>>4, body[2+2*k]&15) for k in range(ns)]} '
                  f'ss={body[1+2*ns]} se={body[2+2*ns]} ah={body[3+2*ns]>>4} al={body[3+2*ns]&15}')
            print(f'  entropy[{len(data)-i-2-ln}:] = {data[i+2+ln:i+2+ln+24].hex()}...')
            break
        i += 2 + ln

main(sys.argv[1])
