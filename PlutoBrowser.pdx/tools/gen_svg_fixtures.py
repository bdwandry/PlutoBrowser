#!/usr/bin/env python3
"""Generates selftest_svg_fixtures.{h,c} for the P24 SVG port.

Each fixture is an SVG document; expected output grids come from an
independent byte-level Python replica of the C decoder spec (same tag
scanner, tokenizer, transforms, path flattening and the same deterministic
software rasterizer: Bresenham lines, midpoint circles/ellipses, quadrant-
arc rounded rects). Probes are auto-picked as the first black pixels."""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_H = os.path.join(ROOT, "src", "render", "decoders",
                     "selftest_svg_fixtures.h")
OUT_C = os.path.join(ROOT, "src", "render", "decoders",
                     "selftest_svg_fixtures.c")

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv_grid(grid):
    hsh = FNV_OFFSET
    for row in grid:
        for v in row:
            hsh ^= v & 0xFF
            hsh = (hsh * FNV_PRIME) & 0xFFFFFFFF
    return hsh


# ---------------------------------------------------------------------------
# Rasterizer replica (must mirror svg.c pixel-for-pixel).

class Canvas:
    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.rows = [[255] * w for _ in range(h)]

    def plot(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.rows[y][x] = 0

    def hline(self, x0, x1, y):
        lo, hi = (x0, x1) if x0 <= x1 else (x1, x0)
        for i in range(lo, hi + 1):
            self.plot(i, y)

    def vline(self, x, y0, y1):
        lo, hi = (y0, y1) if y0 <= y1 else (y1, y0)
        for i in range(lo, hi + 1):
            self.plot(x, i)

    def line(self, x0, y0, x1, y1):
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.plot(x0, y0)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def rect(self, x, y, w, h):
        if w <= 0 or h <= 0:
            return
        self.hline(x, x + w - 1, y)
        self.hline(x, x + w - 1, y + h - 1)
        self.vline(x, y, y + h - 1)
        self.vline(x + w - 1, y, y + h - 1)

    def circle(self, cx, cy, r, mask):
        if r <= 0:
            self.plot(cx, cy)
            return
        x, y = r, 0
        err = 1 - r
        while x >= y:
            if mask & 0x01:
                self.plot(cx + x, cy + y)
            if mask & 0x02:
                self.plot(cx + y, cy + x)
            if mask & 0x04:
                self.plot(cx - y, cy + x)
            if mask & 0x08:
                self.plot(cx - x, cy + y)
            if mask & 0x10:
                self.plot(cx - x, cy - y)
            if mask & 0x20:
                self.plot(cx - y, cy - x)
            if mask & 0x40:
                self.plot(cx + y, cy - x)
            if mask & 0x80:
                self.plot(cx + x, cy - y)
            y += 1
            if err < 0:
                err += 2 * y + 1
            else:
                x -= 1
                err += 2 * (y - x) + 1

    def round_rect(self, x, y, w, h, r):
        if w <= 0 or h <= 0:
            return
        if r < 0:
            r = 0
        if r > w // 2:
            r = w // 2
        if r > h // 2:
            r = h // 2
        if r == 0:
            self.rect(x, y, w, h)
            return
        x2 = x + w - 1
        y2 = y + h - 1
        self.hline(x + r, x2 - r, y)
        self.hline(x + r, x2 - r, y2)
        self.vline(x, y + r, y2 - r)
        self.vline(x2, y + r, y2 - r)
        self.circle(x + r, y + r, r, 0x30)      # TL (-x,-y)(-y,-x)
        self.circle(x2 - r, y + r, r, 0xC0)     # TR (+y,-x)(+x,-y)
        self.circle(x + r, y2 - r, r, 0x0C)     # BL (-y,+x)(-x,+y)
        self.circle(x2 - r, y2 - r, r, 0x03)    # BR (+x,+y)(+y,+x)

    def ellipse(self, bbx, bby, bbw, bbh):
        if bbw <= 0 or bbh <= 0:
            return
        rx = bbw // 2
        ry = bbh // 2
        cx = bbx + rx
        cy = bby + ry
        if rx <= 0 and ry <= 0:
            self.plot(cx, cy)
            return
        if rx <= 0:
            for i in range(cy - ry, cy + ry + 1):
                self.plot(cx, i)
            return
        if ry <= 0:
            for i in range(cx - rx, cx + rx + 1):
                self.plot(i, cy)
            return
        Rx2 = rx * rx
        Ry2 = ry * ry
        x, y = 0, ry
        while True:
            self.plot(cx + x, cy + y)
            self.plot(cx - x, cy + y)
            self.plot(cx + x, cy - y)
            self.plot(cx - x, cy - y)
            if 2 * Ry2 * x >= 2 * Rx2 * y or y <= 0:
                break
            f = 4 * Ry2 * (x + 1) * (x + 1) \
                + Rx2 * (2 * y - 1) * (2 * y - 1) - 4 * Rx2 * Ry2
            x += 1
            if f >= 0:
                y -= 1
        while y > 0:
            f = Ry2 * (2 * x + 1) * (2 * x + 1) \
                + 4 * Rx2 * (y - 1) * (y - 1) - 4 * Rx2 * Ry2
            y -= 1
            if f <= 0:
                x += 1
            self.plot(cx + x, cy + y)
            self.plot(cx - x, cy + y)
            self.plot(cx + x, cy - y)
            self.plot(cx - x, cy - y)


# ---------------------------------------------------------------------------
# Parser replica.

def is_namech(c):
    return c.isascii() and (c.isalnum() or c in "_:-")


def is_ws(c):
    return c in " \t\n\v\f\r"


def is_numch_viewbox(c):
    return c.isdigit() or c in ".eE-"


def is_numch_len(c):
    return c.isdigit() or c in ".eE"


def lua_number_strict(s):
    """Validates like Lua tonumber for decimal tokens; None when invalid."""
    i = 0
    L = len(s)
    mant = 0
    if L == 0 or L > 63:
        return None
    if s[0] in "+-":
        i = 1
    while i < L and "0" <= s[i] <= "9":
        i += 1
        mant += 1
    if i < L and s[i] == ".":
        i += 1
        while i < L and "0" <= s[i] <= "9":
            i += 1
            mant += 1
    if mant == 0:
        return None
    if i < L and s[i] in "eE":
        i += 1
        if i < L and s[i] in "+-":
            i += 1
        d0 = i
        while i < L and "0" <= s[i] <= "9":
            i += 1
        if i == d0:
            return None
    if i != L:
        return None
    return float(s)


def num_or0(s):
    if s is None:
        return 0.0
    v = lua_number_strict(s)
    return v if v is not None else 0.0


def tokenize_numbers(s):
    out = []
    i = 0
    n = len(s)
    stop = False
    while i < n and not stop:
        c = ord(s[i])
        if c in (0x20, 0x09, 0x0A, 0x0D, 0x2C):
            i += 1
            continue
        start = i
        has_digit = False
        if c == 0x2B or c == 0x2D:                 # sign
            i += 1
            if i >= n:
                stop = True
                continue
            c = ord(s[i])
        while 0x30 <= c <= 0x39:                   # integer digits
            has_digit = True
            i += 1
            if i >= n:
                break
            c = ord(s[i])
        if c == 0x2E:                              # fractional part
            if i + 1 < n and 0x30 <= ord(s[i + 1]) <= 0x39:
                i += 1
                c = ord(s[i])
                while 0x30 <= c <= 0x39:
                    has_digit = True
                    i += 1
                    if i >= n:
                        break
                    c = ord(s[i])
            elif not has_digit:                    # .xxx without integer
                i += 1
                if i >= n:
                    stop = True
                    continue
                c = ord(s[i])
                while 0x30 <= c <= 0x39:
                    has_digit = True
                    i += 1
                    if i >= n:
                        break
                    c = ord(s[i])
        if (c == 0x65 or c == 0x45) and has_digit:  # exponent
            i += 1
            if i < n:
                c = ord(s[i])
                if c == 0x2B or c == 0x2D:
                    i += 1
                while i < n and 0x30 <= ord(s[i]) <= 0x39:
                    i += 1
        if i > start and has_digit:
            v = lua_number_strict(s[start:i])
            if v is not None:
                out.append(v)
        if i == start:
            i = start + 1
    return out


def get_attrs(tag_str):
    """Double-quoted pass then single-quoted pass; later wins."""
    out = {}
    for q in ('"', "'"):
        i = 0
        n = len(tag_str)
        while i < n:
            while i < n and not is_namech(tag_str[i]):
                i += 1
            if i >= n:
                break
            k0 = i
            while i < n and is_namech(tag_str[i]):
                i += 1
            k1 = i
            p = k1
            while p < n and is_ws(tag_str[p]):
                p += 1
            if p >= n or tag_str[p] != "=":
                continue
            p += 1
            while p < n and is_ws(tag_str[p]):
                p += 1
            if p >= n or tag_str[p] != q:
                continue
            p += 1
            v0 = p
            while p < n and tag_str[p] != q:
                p += 1
            if p < n:
                out[tag_str[k0:k1]] = tag_str[v0:p]
                i = p + 1
            else:
                i = v0
    return out


def parse_style(style_str):
    out = {}
    if not style_str:
        return out
    for seg in style_str.split(";"):
        colon = seg.find(":")
        if colon < 0:
            continue
        k = seg[:colon].strip().lower()
        v = seg[colon + 1:].strip()
        if k:
            out[k] = v
    return out


class TagAttrs:
    def __init__(self, attr_str):
        self.xml = get_attrs(attr_str)
        self.style = parse_style(self.xml.get("style"))

    def get(self, key):
        v = self.style.get(key)
        if v is not None:
            return v
        return self.xml.get(key)

    def has(self, key):
        return self.get(key) is not None


def is_hidden(t):
    if t.get("display") == "none":
        return True
    if t.get("visibility") in ("hidden", "collapse"):
        return True
    return False


def has_ink(t):
    stroke = t.get("stroke")
    if stroke is not None and stroke != "none" and stroke != "":
        return True
    if t.get("fill") == "none":
        return False
    return True


def expand_uses(src):
    out = []
    last_pos = 0
    n = len(src)
    while True:
        s = -1
        e = -1
        p = last_pos
        while True:
            p = src.find("<", p)
            if p < 0:
                break
            if p + 3 < n and src[p + 1] in "uU" and src[p + 2] in "sS" \
                    and src[p + 3] in "eE":
                e = src.find(">", p + 4)
                if e < 0:
                    p += 1
                    continue
                s = p
                break
            p += 1
        if s < 0:
            out.append(src[last_pos:])
            break
        out.append(src[last_pos:s])
        ua = get_attrs(src[s + 4:e])
        href = ua.get("href")
        if href is None:
            href = ua.get("xlink:href")
        if href is not None and href.startswith("#") and len(href) > 1:
            ident = href[1:]
            replaced = False
            q = 0
            while not replaced:
                q = src.find("<", q)
                if q < 0:
                    break
                if q + 1 < n and is_namech(src[q + 1]):
                    name_end = q + 2
                    while name_end < n and is_namech(src[name_end]):
                        name_end += 1
                    te = src.find(">", name_end)
                    if te >= 0:
                        ea = get_attrs(src[name_end:te])
                        if ea.get("id") == ident:
                            out.append("<" + src[q + 1:name_end]
                                       + src[name_end:te] + ">")
                            replaced = True
                    q = te if te >= 0 else q + 1
                else:
                    q += 1
                q += 1
        last_pos = e + 1
    return "".join(out)


def find_viewbox(x):
    pos = 0
    while True:
        pos = x.find("viewBox", pos)
        if pos < 0:
            return None
        p = pos + 7
        n = len(x)
        while p < n and is_ws(x[p]):
            p += 1
        if p < n and x[p] == "=":
            p += 1
            while p < n and is_ws(x[p]):
                p += 1
            if p < n and x[p] in "\"'":
                p += 1
                while p < n and is_ws(x[p]):
                    p += 1
                nums = []
                ok = True
                for k in range(4):
                    n0 = p
                    while p < n and is_numch_viewbox(x[p]):
                        p += 1
                    v = lua_number_strict(x[n0:p])
                    if p == n0 or v is None:
                        ok = False
                        break
                    nums.append(v)
                    if k < 3:
                        ws0 = p
                        while p < n and is_ws(x[p]):
                            p += 1
                        if p == ws0:
                            ok = False
                            break
                if ok:
                    return tuple(nums)
        pos += 1


def find_len_attr(x, key):
    pos = 0
    n = len(x)
    while True:
        pos = x.find(key, pos)
        if pos < 0:
            return None
        p = pos + len(key)
        while p < n and is_ws(x[p]):
            p += 1
        if p < n and x[p] == "=":
            p += 1
            while p < n and is_ws(x[p]):
                p += 1
            if p < n and x[p] in "\"'":
                p += 1
                n0 = p
                while p < n and is_numch_len(x[p]):
                    p += 1
                if p > n0:
                    v = lua_number_strict(x[n0:p])
                    if v is not None:
                        return v
        pos += 1


CONTAINERS = {"svg", "g", "a", "symbol", "mask", "clipPath",
              "defs", "pattern", "marker", "switch"}
# NOTE: tag names are lowercased before membership checks, so "clipPath"
# never matches (faithful quirk of svg.lua).


class State:
    def __init__(self, cv, scale, min_x, min_y):
        self.cv = cv
        self.scale = scale
        self.min_x = min_x
        self.min_y = min_y
        self.skip_depth = 0
        self.stack = []
        self.drawn = 0

    def tx(self, x):
        return math.floor((x - self.min_x) * self.scale)

    def ty(self, y):
        return math.floor((y - self.min_y) * self.scale)


def draw_path(st, d):
    cv = st.cv
    cur_x = cur_y = 0.0
    start_x = start_y = 0.0
    last_cx = last_cy = 0.0
    has_point = False
    i = 0
    n = len(d)

    def is_letter(ch):
        return ("a" <= ch <= "z") or ("A" <= ch <= "Z")

    while i < n:
        while i < n and not is_letter(d[i]):
            i += 1
        if i >= n:
            break
        cmd = d[i]
        i += 1
        a0 = i
        while i < n and not is_letter(d[i]):
            i += 1
        coords = tokenize_numbers(d[a0:i])
        cn = len(coords)
        is_rel = cmd.islower()
        c_up = cmd.upper()

        def pt(ix):
            px = coords[ix] if ix < cn else 0.0
            py = coords[ix + 1] if ix + 1 < cn else 0.0
            if is_rel:
                return cur_x + px, cur_y + py
            return px, py

        if c_up == "M":
            for k in range(0, cn, 2):
                nx, ny = pt(k)
                if k == 0:
                    cur_x, cur_y = nx, ny
                    start_x, start_y = nx, ny
                    has_point = True
                else:
                    cv.line(st.tx(cur_x), st.ty(cur_y),
                            st.tx(nx), st.ty(ny))
                    cur_x, cur_y = nx, ny
            last_cx, last_cy = cur_x, cur_y
        elif c_up == "L":
            for k in range(0, cn, 2):
                nx, ny = pt(k)
                cv.line(st.tx(cur_x), st.ty(cur_y), st.tx(nx), st.ty(ny))
                cur_x, cur_y = nx, ny
            last_cx, last_cy = cur_x, cur_y
        elif c_up == "H":
            for k in range(cn):
                nx = cur_x + coords[k] if is_rel else coords[k]
                cv.line(st.tx(cur_x), st.ty(cur_y),
                        st.tx(nx), st.ty(cur_y))
                cur_x = nx
            last_cx, last_cy = cur_x, cur_y
        elif c_up == "V":
            for k in range(cn):
                ny = cur_y + coords[k] if is_rel else coords[k]
                cv.line(st.tx(cur_x), st.ty(cur_y),
                        st.tx(cur_x), st.ty(ny))
                cur_y = ny
            last_cx, last_cy = cur_x, cur_y
        elif c_up == "Z":
            if has_point:
                cv.line(st.tx(cur_x), st.ty(cur_y),
                        st.tx(start_x), st.ty(start_y))
                cur_x, cur_y = start_x, start_y
            last_cx, last_cy = cur_x, cur_y
        elif c_up == "C":
            for k in range(0, cn, 6):
                x1, y1 = pt(k)
                x2, y2 = pt(k + 2)
                x3, y3 = pt(k + 4)
                for t in range(1, 9):
                    u = t / 8.0
                    nx = ((1-u)*(1-u)*(1-u)*cur_x + 3*(1-u)*(1-u)*u*x1
                          + 3*(1-u)*u*u*x2 + u*u*u*x3)
                    ny = ((1-u)*(1-u)*(1-u)*cur_y + 3*(1-u)*(1-u)*u*y1
                          + 3*(1-u)*u*u*y2 + u*u*u*y3)
                    cv.line(st.tx(cur_x), st.ty(cur_y),
                            st.tx(nx), st.ty(ny))
                    cur_x, cur_y = nx, ny
                last_cx, last_cy = x2, y2
        elif c_up == "S":
            for k in range(0, cn, 4):
                sx1 = cur_x * 2 - last_cx
                sy1 = cur_y * 2 - last_cy
                x2, y2 = pt(k)
                x3, y3 = pt(k + 2)
                for t in range(1, 9):
                    u = t / 8.0
                    nx = ((1-u)*(1-u)*(1-u)*cur_x + 3*(1-u)*(1-u)*u*sx1
                          + 3*(1-u)*u*u*x2 + u*u*u*x3)
                    ny = ((1-u)*(1-u)*(1-u)*cur_y + 3*(1-u)*(1-u)*u*sy1
                          + 3*(1-u)*u*u*y2 + u*u*u*y3)
                    cv.line(st.tx(cur_x), st.ty(cur_y),
                            st.tx(nx), st.ty(ny))
                    cur_x, cur_y = nx, ny
                last_cx, last_cy = x2, y2
        elif c_up == "Q":
            for k in range(0, cn, 4):
                x1, y1 = pt(k)
                x2, y2 = pt(k + 2)
                for t in range(1, 7):
                    u = t / 6.0
                    nx = (1-u)*(1-u)*cur_x + 2*(1-u)*u*x1 + u*u*x2
                    ny = (1-u)*(1-u)*cur_y + 2*(1-u)*u*y1 + u*u*y2
                    cv.line(st.tx(cur_x), st.ty(cur_y),
                            st.tx(nx), st.ty(ny))
                    cur_x, cur_y = nx, ny
                last_cx, last_cy = x1, y1
        elif c_up == "T":
            for k in range(0, cn, 2):
                qx1 = cur_x * 2 - last_cx
                qy1 = cur_y * 2 - last_cy
                x2, y2 = pt(k)
                for t in range(1, 7):
                    u = t / 6.0
                    nx = (1-u)*(1-u)*cur_x + 2*(1-u)*u*qx1 + u*u*x2
                    ny = (1-u)*(1-u)*cur_y + 2*(1-u)*u*qy1 + u*u*y2
                    cv.line(st.tx(cur_x), st.ty(cur_y),
                            st.tx(nx), st.ty(ny))
                    cur_x, cur_y = nx, ny
                last_cx, last_cy = qx1, qy1
        elif c_up == "A":
            for k in range(0, cn, 7):
                ex, ey = pt(k + 5)
                cv.line(st.tx(cur_x), st.ty(cur_y),
                        st.tx(ex), st.ty(ey))
                cur_x, cur_y = ex, ey
            last_cx, last_cy = cur_x, cur_y


def shape_open(st, tag, t):
    cv = st.cv
    sc = st.scale
    if tag == "rect" and has_ink(t) and t.has("x") and t.has("y") \
            and t.has("width") and t.has("height"):
        x = st.tx(num_or0(t.get("x")))
        y = st.ty(num_or0(t.get("y")))
        w = max(1, math.floor(num_or0(t.get("width")) * sc))
        h = max(1, math.floor(num_or0(t.get("height")) * sc))
        if t.has("rx") or t.has("ry"):
            rr = num_or0(t.get("rx")) if t.has("rx") else 2.0
            r = max(1, min(4, math.floor(rr * sc)))
            cv.round_rect(x, y, w, h, r)
        else:
            cv.rect(x, y, w, h)
        st.drawn += 1
    elif tag == "circle" and has_ink(t) and t.has("cx") and t.has("cy") \
            and t.has("r"):
        r = max(1, math.floor(num_or0(t.get("r")) * sc))
        cv.circle(st.tx(num_or0(t.get("cx"))),
                  st.ty(num_or0(t.get("cy"))), r, 0xFF)
        st.drawn += 1
    elif tag == "ellipse" and has_ink(t) and t.has("cx") and t.has("cy") \
            and t.has("rx") and t.has("ry"):
        ecx = st.tx(num_or0(t.get("cx")))
        ecy = st.ty(num_or0(t.get("cy")))
        erx = max(1, math.floor(num_or0(t.get("rx")) * sc))
        ery = max(1, math.floor(num_or0(t.get("ry")) * sc))
        steps = max(2, min(5, math.floor(min(erx, ery) / 2)))
        for si in range(1, steps + 1):
            f = 1 - ((si - 1) / steps) * 0.6
            exv = ecx - math.floor(erx * f)
            eyv = ecy - math.floor(ery * f)
            ew = max(2, math.floor(erx * f * 2))
            eh = max(2, math.floor(ery * f * 2))
            cv.ellipse(exv, eyv, ew, eh)
        st.drawn += 1
    elif tag == "line" and has_ink(t) and t.has("x1") and t.has("y1") \
            and t.has("x2") and t.has("y2"):
        cv.line(st.tx(num_or0(t.get("x1"))), st.ty(num_or0(t.get("y1"))),
                st.tx(num_or0(t.get("x2"))), st.ty(num_or0(t.get("y2"))))
        st.drawn += 1
    elif tag in ("polygon", "polyline") and has_ink(t) and t.has("points"):
        pts = tokenize_numbers(t.get("points"))
        pn = len(pts)
        i = 0
        while i + 3 < pn:
            cv.line(st.tx(pts[i]), st.ty(pts[i + 1]),
                    st.tx(pts[i + 2]), st.ty(pts[i + 3]))
            i += 2
        if tag == "polygon" and pn >= 4:
            cv.line(st.tx(pts[pn - 2]), st.ty(pts[pn - 1]),
                    st.tx(pts[0]), st.ty(pts[1]))
        st.drawn += 1
    elif tag == "path" and has_ink(t) and t.has("d"):
        draw_path(st, t.get("d"))
        st.drawn += 1


def decode(xml, max_w=360, max_h=200):
    if xml is None or "<svg" not in xml:
        return None
    vb = find_viewbox(xml)
    wr = find_len_attr(xml, "width")
    hr = find_len_attr(xml, "height")

    src_w = vb[2] if vb else (wr if wr is not None else 100.0)
    src_h = vb[3] if vb else (hr if hr is not None else 100.0)
    min_x = vb[0] if vb else 0.0
    min_y = vb[1] if vb else 0.0
    if src_w <= 0 or src_h <= 0:
        return None

    scale = min(max_w / src_w, max_h / src_h)
    if scale > 2:
        scale = 2
    tw = max(20, math.floor(src_w * scale))
    th = max(20, math.floor(src_h * scale))

    body = expand_uses(xml)
    st = State(Canvas(tw, th), scale, min_x, min_y)

    pos = 0
    blen = len(body)
    while True:
        s = body.find("<", pos)
        if s < 0:
            break
        e = body.find(">", s)
        if e < 0:
            break
        inside = body[s + 1:e]
        pos = e + 1
        head = inside[:3]
        if head == "!--":
            ce = body.find("-->", e)
            if ce >= 0:
                pos = ce + 3
        elif head.startswith("!["):
            ce = body.find("]]>", e)
            if ce >= 0:
                pos = ce + 3
        elif head[:2] not in ("!D", "!d", "?x", "?X"):
            trimmed = inside.strip()
            if trimmed:
                is_close = trimmed.startswith("/")
                tag_body = trimmed[1:] if is_close else trimmed
                is_self_close = tag_body.endswith("/")
                if is_self_close:
                    tag_body = tag_body[:-1]
                m = ""
                for ch in tag_body:
                    if is_namech(ch):
                        m += ch
                    else:
                        break
                if m:
                    tag = m.lower()
                    attr_str = tag_body[len(m):]
                    if not is_close:
                        t = TagAttrs(attr_str)
                        own_hidden = is_hidden(t)
                        entering_skip = (tag == "defs") or own_hidden
                        if tag in CONTAINERS and not is_self_close:
                            st.stack.append(st.skip_depth)
                            if entering_skip:
                                st.skip_depth += 1
                        if not entering_skip and st.skip_depth == 0:
                            shape_open(st, tag, t)
                    else:
                        if tag in CONTAINERS:
                            if st.stack:
                                st.skip_depth = st.stack.pop()

    if st.drawn == 0:
        return None
    return tw, th, st.cv.rows, st.drawn


# ---------------------------------------------------------------------------
# Fixtures.

FIXTURES = {}
GUARDS = []

FIXTURES["rects"] = """<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">
<rect x="4" y="4" width="16" height="10"/>
<rect x="26" y="6" width="14" height="14" rx="3"/>
<rect x="6" y="22" width="12" height="8" fill="none"/>
<g display="none"><rect x="20" y="20" width="20" height="20"/></g>
</svg>"""

FIXTURES["shapes"] = ("<svg viewBox=\"0 0 40 40\">"
                      "<circle cx=\"10\" cy=\"10\" r=\"6\"/>"
                      "<ellipse cx=\"28\" cy=\"10\" rx=\"7\" ry=\"4\"/>"
                      "<line x1=\"4\" y1=\"34\" x2=\"18\" y2=\"24\"/>"
                      "<polyline points=\"22,36 30,30 36,32\"/>"
                      "<polygon points=\"6,14 12,18 4,20\"/></svg>")

FIXTURES["path_basic"] = (
    "<svg viewBox=\"0 0 32 32\">"
    "<path d=\"M4 4 L12 8 H20 V16 Z M22 6 l4 4 h4 v6\"/>"
    "<path d=\"M2 28 h8 v-6\"/></svg>")

FIXTURES["path_curves"] = (
    "<svg viewBox=\"0 0 64 32\">"
    "<path d=\"M2 28 C 2-8.264 12e0 4 20 8 S 30 2 38 10 "
    "Q 44 0 50 8 T 60 12\"/></svg>")

FIXTURES["arc"] = (
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M3 21 A 8 8 0 0 1 21 21 Z\"/>"
    "<path d=\"M2 4 a5 5 0 0 0 10 0\"/></svg>")

FIXTURES["use_defs"] = (
    "<svg viewBox=\"0 0 40 40\">"
    "<defs><circle id=\"dot\" cx=\"8\" cy=\"8\" r=\"5\"/></defs>"
    "<use href=\"#dot\"/>"
    "<use xlink:href=\"#missing\"/>"
    "<use href=\"#dot\"/>"
    "<g><use href=\"#dot\"/></g></svg>")

FIXTURES["nested_skip"] = (
    "<svg viewBox=\"0 0 40 40\">"
    "<g display=\"none\"><rect x=\"2\" y=\"2\" width=\"10\" height=\"10\"/>"
    "<g visibility=\"hidden\"><circle cx=\"30\" cy=\"10\" r=\"4\"/></g></g>"
    "<g><line x1=\"2\" y1=\"30\" x2=\"30\" y2=\"30\"/>"
    "<symbol><rect x=\"34\" y=\"2\" width=\"4\" height=\"4\"/></symbol></g>"
    "<switch><circle cx=\"10\" cy=\"10\" r=\"2\"/></switch></svg>")

FIXTURES["style_attr"] = (
    "<svg viewBox=\"0 0 40 40\">"
    "<rect x=\"2\" y=\"2\" width=\"12\" height=\"8\" style=\"fill:none\"/>"
    "<rect x=\"20\" y=\"2\" width=\"12\" height=\"8\" fill=\"none\" "
    "style=\"fill:black\"/>"
    "<circle cx=\"8\" cy=\"24\" r=\"5\" style=\"display:none\"/>"
    "<circle cx=\"24\" cy=\"24\" r=\"5\" style=\"stroke:gray\"/></svg>")

FIXTURES["viewbox_offset"] = (
    "<svg viewBox=\"10 5 40 40\" width=\"40\" height=\"40\">"
    "<rect x=\"12\" y=\"7\" width=\"16\" height=\"10\"/>"
    "<line x1=\"10\" y1=\"25\" x2=\"46\" y2=\"45\"/></svg>")

FIXTURES["comments"] = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" '
    '"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">'
    "<!-- <rect x=\"0\" y=\"0\" width=\"999\" height=\"999\"/> -->"
    "<![CDATA[ <rect x=\"1\" y=\"1\" width=\"50\" height=\"50\"/> ]]>"
    "<svg viewBox=\"0 0 20 20\">"
    "<rect x=\"2\" y=\"2\" width=\"10\" height=\"8\"/>"
    "<!-- inner comment -->"
    "<circle cx=\"15\" cy=\"15\" r=\"3\"/></svg>")

FIXTURES["bench"] = (
    "<svg viewBox=\"0 0 96 96\">"
    "<rect x=\"4\" y=\"4\" width=\"88\" height=\"88\" rx=\"6\"/>"
    "<circle cx=\"30\" cy=\"34\" r=\"14\"/>"
    "<ellipse cx=\"66\" cy=\"34\" rx=\"16\" ry=\"9\"/>"
    "<path d=\"M12 70 C 24 58 36 82 48 70 S 72 58 84 70\"/>"
    "<polygon points=\"20,88 40,78 60,86 76,80\"/>"
    "<line x1=\"4\" y1=\"52\" x2=\"92\" y2=\"52\"/>"
    "</svg>")

GUARDS.append(("no_svg", "<div>hello</div>"))
GUARDS.append(("zero_drawn",
               "<svg viewBox=\"0 0 20 20\"><defs>"
               "<rect x=\"2\" y=\"2\" width=\"8\" height=\"8\"/>"
               "</defs></svg>"))
GUARDS.append(("bad_dims",
               "<svg width=\"0\" height=\"10\">"
               "<rect x=\"1\" y=\"1\" width=\"5\" height=\"5\"/></svg>"))
GUARDS.append(("trunc_tag",
               "<svg viewBox=\"0 0 10 10\"><rect x=\"1\" y=\"1\" wid"))


def main():
    probe_target = 12

    h_parts = [
        "// GENERATED by tools/gen_svg_fixtures.py - do not edit.\n",
        "#ifndef PLUTO_SELFTEST_SVG_FIXTURES_H\n",
        "#define PLUTO_SELFTEST_SVG_FIXTURES_H\n\n",
        "#include <stddef.h>\n#include <stdint.h>\n\n",
        "typedef struct { int x, y, v; } SvgProbe;\n\n",
    ]
    c_parts = [
        "// GENERATED by tools/gen_svg_fixtures.py - do not edit.\n",
        '#include "render/decoders/selftest_svg_fixtures.h"\n\n',
    ]

    for name in sorted(FIXTURES):
        xml = FIXTURES[name]
        res = decode(xml)
        assert res is not None, "fixture %s unexpectedly rejected" % name
        gw, gh, grid, drawn = res
        up = name.upper()
        data = xml.encode("utf-8")
        arr = ", ".join("0x%02X" % b for b in data)
        esc = xml.replace("\\", "\\\\").replace('"', '\\"')
        esc = esc.replace("\n", "\\n")

        c_parts.append('const char fx_s_%s[] = "%s";\n' % (name, esc))
        c_parts.append("const int fx_s_%s_len = %d;\n" % (name, len(data)))
        h_parts.append("extern const char fx_s_%s[];\n" % name)
        h_parts.append("#define FX_S_%s_LEN %d\n" % (up, len(data)))
        h_parts.append("#define FX_S_%s_W %d\n" % (up, gw))
        h_parts.append("#define FX_S_%s_H %d\n" % (up, gh))
        h_parts.append("#define FX_S_%s_CK 0x%08Xu\n" % (up, fnv_grid(grid)))
        h_parts.append("#define FX_S_%s_DRAWN %d\n" % (up, drawn))

        pts = []
        for y in range(gh):
            for x in range(gw):
                if grid[y][x] == 0:
                    pts.append((x, y, 0))
                    if len(pts) >= probe_target:
                        break
            if len(pts) >= probe_target:
                break
        pname = "fx_sp_" + name
        items = ", ".join("{%d,%d,%d}" % p for p in pts)
        c_parts.append("const SvgProbe %s[] = {%s};\n\n" % (pname, items))
        h_parts.append("extern const SvgProbe %s[];\n" % pname)
        h_parts.append("#define FX_SP_%s_LEN %d\n"
                       % (name.upper(), len(pts)))

    for name, blob in GUARDS:
        up = name.upper()
        data = blob.encode("utf-8")
        arr = ", ".join("0x%02X" % b for b in data)
        esc = blob.replace("\\", "\\\\").replace('"', '\\"')
        esc = esc.replace("\n", "\\n")
        c_parts.append('const char fx_sg_%s[] = "%s";\n' % (name, esc))
        c_parts.append("const int fx_sg_%s_len = %d;\n\n"
                       % (name, len(data)))
        h_parts.append("extern const char fx_sg_%s[];\n" % name)
        h_parts.append("#define FX_SG_%s_LEN %d\n" % (up, len(data)))

    h_parts.append("\n#endif\n")

    with open(OUT_H, "w") as f:
        f.write("".join(h_parts))
    with open(OUT_C, "w") as f:
        f.write("".join(c_parts))
    print("wrote", OUT_H)
    print("wrote", OUT_C)


if __name__ == "__main__":
    main()
