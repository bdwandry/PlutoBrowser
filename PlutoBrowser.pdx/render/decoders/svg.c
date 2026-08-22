// svg.c — C port of Source/render/decoders/svg.lua (SVGDecoder).
//
// Renders the web-icon subset of SVG into a 1-bit gray grid (255 white
// canvas, 0 ink) using a small deterministic software rasterizer:
// Bresenham lines, rect outlines, rounded rects (edges + quadrant arcs from
// the midpoint circle), midpoint circles/ellipses. Tag scanning, <use>
// resolution, style merging, hidden-subtree skipping and path flattening
// mirror svg.lua exactly, including its quirks (e.g. "clipPath" never
// matching after lowercasing, A-commands drawn as straight chords).

#include "render/decoders/svg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render/decoders/dither.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_SVG_PD 1
#endif

/* ── canvas + rasterizer primitives ──────────────────────────────────── */

typedef struct {
    uint8_t** rows;
    int w, h;
} Canvas;

static void cv_plot(Canvas* cv, int x, int y) {
    if (!cv || x < 0 || y < 0 || x >= cv->w || y >= cv->h) return;
    cv->rows[y][x] = 0;
}

/* Bresenham; plots both endpoints; any octant. */
static void cv_line(Canvas* cv, int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y0 > y1 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        int e2;
        cv_plot(cv, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void cv_rect(Canvas* cv, int x, int y, int w, int h) {
    int i;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < w; i++) {
        cv_plot(cv, x + i, y);
        cv_plot(cv, x + i, y + h - 1);
    }
    for (i = 0; i < h; i++) {
        cv_plot(cv, x, y + i);
        cv_plot(cv, x + w - 1, y + i);
    }
}

static void cv_hline(Canvas* cv, int x0, int x1, int y) {
    int i, lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
    for (i = lo; i <= hi; i++) cv_plot(cv, i, y);
}

static void cv_vline(Canvas* cv, int x, int y0, int y1) {
    int i, lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
    for (i = lo; i <= hi; i++) cv_plot(cv, x, i);
}

#define OCT_ALL      0xFFu
#define OCT_TL       0x30u   /* (-x,-y) and (-y,-x) */
#define OCT_TR       0xC0u   /* (+y,-x) and (+x,-y) */
#define OCT_BL       0x0Cu   /* (-y,+x) and (-x,+y) */
#define OCT_BR       0x03u   /* (+x,+y) and (+y,+x) */

/* Midpoint circle around (cx,cy); octmask selects plotted octants. */
static void cv_circle(Canvas* cv, int cx, int cy, int r, unsigned octmask) {
    int x, y;
    long long err;
    if (r <= 0) { cv_plot(cv, cx, cy); return; }
    x = r; y = 0;
    err = 1 - r;
    while (x >= y) {
        if (octmask & 0x01u) cv_plot(cv, cx + x, cy + y);
        if (octmask & 0x02u) cv_plot(cv, cx + y, cy + x);
        if (octmask & 0x04u) cv_plot(cv, cx - y, cy + x);
        if (octmask & 0x08u) cv_plot(cv, cx - x, cy + y);
        if (octmask & 0x10u) cv_plot(cv, cx - x, cy - y);
        if (octmask & 0x20u) cv_plot(cv, cx - y, cy - x);
        if (octmask & 0x40u) cv_plot(cv, cx + y, cy - x);
        if (octmask & 0x80u) cv_plot(cv, cx + x, cy - y);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void cv_round_rect(Canvas* cv, int x, int y, int w, int h, int r) {
    int x2, y2;
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r == 0) { cv_rect(cv, x, y, w, h); return; }
    x2 = x + w - 1;
    y2 = y + h - 1;
    cv_hline(cv, x + r, x2 - r, y);
    cv_hline(cv, x + r, x2 - r, y2);
    cv_vline(cv, x, y + r, y2 - r);
    cv_vline(cv, x2, y + r, y2 - r);
    cv_circle(cv, x + r, y + r, r, OCT_TL);
    cv_circle(cv, x2 - r, y + r, r, OCT_TR);
    cv_circle(cv, x + r, y2 - r, r, OCT_BL);
    cv_circle(cv, x2 - r, y2 - r, r, OCT_BR);
}

/* Midpoint ellipse inside bounding box (bbx,bby,bbw,bbh), pure integer. */
static void canvas_ellipse(Canvas* cv, int bbx, int bby, int bbw, int bbh) {
    int rx, ry, cx, cy, i;
    long long Rx2, Ry2, x, y, f;
    if (bbw <= 0 || bbh <= 0) return;
    rx = bbw / 2; ry = bbh / 2;
    cx = bbx + rx; cy = bby + ry;
    if (rx <= 0 && ry <= 0) { cv_plot(cv, cx, cy); return; }
    if (rx <= 0) { for (i = cy - ry; i <= cy + ry; i++) cv_plot(cv, cx, i); return; }
    if (ry <= 0) { for (i = cx - rx; i <= cx + rx; i++) cv_plot(cv, i, cy); return; }
    Rx2 = (long long)rx * rx;
    Ry2 = (long long)ry * ry;
    x = 0; y = ry;
    for (;;) {
        cv_plot(cv, cx + (int)x, cy + (int)y);
        cv_plot(cv, cx - (int)x, cy + (int)y);
        cv_plot(cv, cx + (int)x, cy - (int)y);
        cv_plot(cv, cx - (int)x, cy - (int)y);
        if (2 * Ry2 * x >= 2 * Rx2 * y || y <= 0) break;
        f = 4 * Ry2 * (x + 1) * (x + 1) + Rx2 * (2 * y - 1) * (2 * y - 1)
            - 4 * Rx2 * Ry2;
        x++;
        if (f >= 0) y--;
    }
    while (y > 0) {
        f = Ry2 * (2 * x + 1) * (2 * x + 1)
            + 4 * Rx2 * (y - 1) * (y - 1) - 4 * Rx2 * Ry2;
        y--;
        if (f <= 0) x++;
        cv_plot(cv, cx + (int)x, cy + (int)y);
        cv_plot(cv, cx - (int)x, cy + (int)y);
        cv_plot(cv, cx + (int)x, cy - (int)y);
        cv_plot(cv, cx - (int)x, cy - (int)y);
    }
}

/* ── small string helpers ────────────────────────────────────────────── */

static int find_ch(const char* s, int len, char c, int from) {
    int i;
    for (i = from; i < len; i++)
        if (s[i] == c) return i;
    return -1;
}

static int find_str(const char* s, int len, const char* needle, int nlen,
                    int from) {
    int i;
    if (nlen <= 0) return from <= len ? from : -1;
    for (i = from; i + nlen <= len; i++)
        if (memcmp(s + i, needle, (size_t)nlen) == 0) return i;
    return -1;
}

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int is_namech(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-';
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

static int is_numch_viewbox(char c) {
    return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
           c == '-';
}

static int is_numch_len(char c) {
    return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E';
}

/* Lua string.lower on ASCII. */
static char* dup_lower(const char* s, int len) {
    char* out = (char*)pluto_malloc((size_t)len + 1);
    int i;
    if (!out) return NULL;
    for (i = 0; i < len; i++) out[i] = lower_ch(s[i]);
    out[len] = '\0';
    return out;
}

static char* dup_range(const char* s, int len) {
    char* out = (char*)pluto_malloc((size_t)len + 1);
    if (!out) return NULL;
    if (len > 0) memcpy(out, s, (size_t)len);
    out[len] = '\0';
    return out;
}

/* ── attribute map ───────────────────────────────────────────────────── */

#define SVG_MAX_ATTRS 64

typedef struct {
    char* k[SVG_MAX_ATTRS];
    char* v[SVG_MAX_ATTRS];
    int n;
} AttrSet;

static void attrs_init(AttrSet* a) { a->n = 0; }

static void attrs_free(AttrSet* a) {
    int i;
    for (i = 0; i < a->n; i++) {
        pluto_free(a->k[i]);
        pluto_free(a->v[i]);
    }
    a->n = 0;
}

static void attr_set(AttrSet* a, const char* k, int klen,
                     const char* v, int vlen) {
    int i;
    for (i = 0; i < a->n; i++) {
        if ((int)strlen(a->k[i]) == klen && memcmp(a->k[i], k, (size_t)klen) == 0) {
            char* nv = dup_range(v, vlen);
            if (nv) {
                pluto_free(a->v[i]);
                a->v[i] = nv;
            }
            return;
        }
    }
    if (a->n >= SVG_MAX_ATTRS) return;
    a->k[a->n] = dup_range(k, klen);
    a->v[a->n] = dup_range(v, vlen);
    if (a->k[a->n] && a->v[a->n]) {
        a->n++;
    } else {
        pluto_free(a->k[a->n]);
        pluto_free(a->v[a->n]);
    }
}

/* getAttrs: double-quoted pass first, then single-quoted pass overriding. */
static void get_attrs(const char* s, int len, AttrSet* out) {
    int quotePass;
    attrs_init(out);
    for (quotePass = 0; quotePass < 2; quotePass++) {
        char q = quotePass == 0 ? '"' : '\'';
        int i = 0;
        while (i < len) {
            /* scan key run of name chars starting at next name char */
            while (i < len && !is_namech(s[i])) i++;
            if (i >= len) break;
            {
                int k0 = i;
                while (i < len && is_namech(s[i])) i++;
                int k1 = i;
                /* optional ws, '=', optional ws, quote */
                int p = k1;
                while (p < len && is_ws(s[p])) p++;
                if (p >= len || s[p] != '=') continue;
                p++;
                while (p < len && is_ws(s[p])) p++;
                if (p >= len || s[p] != q) continue;
                p++;
                {
                    int v0 = p;
                    while (p < len && s[p] != q) p++;
                    if (p > len) p = len;
                    if (p < len) {   /* closing quote found */
                        attr_set(out, s + k0, k1 - k0, s + v0, p - v0);
                        i = p + 1;
                    } else {
                        i = v0;      /* unterminated: skip past value start */
                    }
                }
            }
        }
    }
}

static const char* attr_get(const AttrSet* a, const char* key) {
    int i;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->k[i], key) == 0) return a->v[i];
    return NULL;
}

/* ── numbers ─────────────────────────────────────────────────────────── */

/* Strict decimal validator matching Lua tonumber semantics for tokens this
 * tokenizer can produce; strtod fills the value only when fully valid. */
static int lua_number_strict(const char* s, int len, double* out) {
    int i = 0, mantDigits = 0, expDigits = 0;
    if (len <= 0 || len > 63) return 0;
    if (s[i] == '+' || s[i] == '-') i++;
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; mantDigits++; }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; mantDigits++; }
    }
    if (mantDigits == 0) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int save = i;
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { i++; expDigits++; }
        if (expDigits == 0) return 0;   /* "5e"/"5e+" -> nil like Lua */
        (void)save;
    }
    if (i != len) return 0;
    {
        char buf[64];
        char* end = NULL;
        memcpy(buf, s, (size_t)len);
        buf[len] = '\0';
        *out = strtod(buf, &end);
        return end == buf + len;
    }
}

static double num_or0(const char* s) {
    double v;
    if (!s) return 0.0;
    if (lua_number_strict(s, (int)strlen(s), &v)) return v;
    return 0.0;
}

typedef struct {
    double* v;
    int n, cap;
} Nums;

static void nums_free(Nums* ns) {
    if (ns->v) pluto_free(ns->v);
    ns->v = NULL;
    ns->n = ns->cap = 0;
}

static void nums_push(Nums* ns, double d) {
    if (ns->n >= ns->cap) {
        int ncap = ns->cap ? ns->cap * 2 : 16;
        double* nv = (double*)pluto_malloc(sizeof(double) * (size_t)ncap);
        if (!nv) return;
        if (ns->v) {
            memcpy(nv, ns->v, sizeof(double) * (size_t)ns->n);
            pluto_free(ns->v);
        }
        ns->v = nv;
        ns->cap = ncap;
    }
    ns->v[ns->n++] = d;
}

/* Character-level tokenizer — literal port of svg.lua tokenizePathNumbers,
 * which correctly splits adjacent numbers like "0-8.264". */
static void tokenize_numbers(const char* s, int len, Nums* out) {
    int i = 0;
    out->v = NULL; out->n = 0; out->cap = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int start, hasDigit;
        if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x2C) {
            i++;
            continue;
        }
        start = i;
        hasDigit = 0;
        if (c == 0x2B || c == 0x2D) {           /* sign */
            i++;
            if (i >= len) break;                /* exits outer loop (Lua) */
            c = (unsigned char)s[i];
        }
        while (c >= 0x30 && c <= 0x39) {        /* integer digits */
            hasDigit = 1;
            i++;
            if (i >= len) break;
            c = (unsigned char)s[i];
        }
        if (c == 0x2E) {                        /* fractional part */
            if (i + 1 < len && s[i + 1] >= 0x30 && s[i + 1] <= 0x39) {
                i++;
                c = (unsigned char)s[i];
                while (c >= 0x30 && c <= 0x39) {
                    hasDigit = 1;
                    i++;
                    if (i >= len) break;
                    c = (unsigned char)s[i];
                }
            } else if (!hasDigit) {             /* .xxx without integer part */
                i++;
                if (i >= len) break;
                c = (unsigned char)s[i];
                while (c >= 0x30 && c <= 0x39) {
                    hasDigit = 1;
                    i++;
                    if (i >= len) break;
                    c = (unsigned char)s[i];
                }
            }
            /* else: digits followed by bare dot -> number ends before dot */
        }
        if ((c == 0x65 || c == 0x45) && hasDigit) {   /* exponent */
            i++;
            if (i < len) {
                c = (unsigned char)s[i];
                if (c == 0x2B || c == 0x2D) i++;
                while (i < len && s[i] >= 0x30 && s[i] <= 0x39) i++;
            }
        }
        if (i > start && hasDigit) {
            double num;
            if (lua_number_strict(s + start, i - start, &num))
                nums_push(out, num);
        }
        if (i == start) i = start + 1;          /* skip unknown char */
    }
}

/* ── style / visibility / ink helpers ───────────────────────────────── */

typedef struct {
    AttrSet xml;    /* from tag attributes */
    AttrSet style;  /* parsed from style="" (wins over xml) */
} TagAttrs;

static const char* ta_get(const TagAttrs* t, const char* key) {
    const char* v = attr_get(&t->style, key);
    if (v) return v;
    return attr_get(&t->xml, key);
}

static int ta_has(const TagAttrs* t, const char* key) {
    return ta_get(t, key) != NULL;
}

/* parseStyle: split ';', first ':', trim both, lowercase key. */
static void parse_style(const char* s, int len, AttrSet* out) {
    int i = 0;
    attrs_init(out);
    while (i < len) {
        int seg0 = i;
        int seg1 = find_ch(s, len, ';', i);
        if (seg1 < 0) seg1 = len;
        {
            int colon = find_ch(s, seg1, ':', seg0);
            if (colon >= 0) {
                int k0 = seg0, k1 = colon;
                int v0 = colon + 1, v1 = seg1;
                while (k0 < k1 && is_ws(s[k0])) k0++;
                while (k1 > k0 && is_ws(s[k1 - 1])) k1--;
                while (v0 < v1 && is_ws(s[v0])) v0++;
                while (v1 > v0 && is_ws(s[v1 - 1])) v1--;
                if (k1 > k0) {
                    int j;
                    char keybuf[48];
                    int klen = k1 - k0;
                    if (klen > 47) klen = 47;
                    for (j = 0; j < klen; j++)
                        keybuf[j] = lower_ch(s[k0 + j]);
                    keybuf[j] = '\0';
                    attr_set(out, keybuf, klen, s + v0, v1 - v0);
                }
            }
        }
        i = seg1 + 1;
    }
}

static void tagattrs_load(TagAttrs* t, const char* attrStr, int alen) {
    const char* styleStr;
    get_attrs(attrStr, alen, &t->xml);
    styleStr = attr_get(&t->xml, "style");
    parse_style(styleStr ? styleStr : "", styleStr ? (int)strlen(styleStr) : 0,
                &t->style);
}

static void tagattrs_free(TagAttrs* t) {
    attrs_free(&t->xml);
    attrs_free(&t->style);
}

static int is_hidden(const TagAttrs* t) {
    const char* v;
    if ((v = ta_get(t, "display")) && strcmp(v, "none") == 0) return 1;
    if ((v = ta_get(t, "visibility")) &&
        (strcmp(v, "hidden") == 0 || strcmp(v, "collapse") == 0))
        return 1;
    return 0;
}

static int has_ink(const TagAttrs* t) {
    const char* fill = ta_get(t, "fill");
    const char* stroke = ta_get(t, "stroke");
    if (stroke && strcmp(stroke, "none") != 0 && strcmp(stroke, "") != 0)
        return 1;
    if (fill && strcmp(fill, "none") == 0) return 0;
    return 1;
}

/* ── <use> expansion ─────────────────────────────────────────────────── */

typedef struct {
    char* data;
    int len, cap;
} StrBuf;

static void sb_init(StrBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void sb_append(StrBuf* b, const char* s, int len) {
    if (len <= 0) return;
    if (b->len + len > b->cap) {
        int ncap = b->cap ? b->cap : 256;
        char* nd;
        while (ncap < b->len + len) ncap *= 2;
        nd = (char*)pluto_malloc((size_t)ncap);
        if (!nd) return;
        if (b->data) {
            memcpy(nd, b->data, (size_t)b->len);
            pluto_free(b->data);
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, (size_t)len);
    b->len += len;
}

/* expandUses: replace each <use href="#id"/> with the element whose id
 * matches (reconstructed "<tag attrs>"), or drop it. Literal port. */
static char* expand_uses(const char* src, int slen, int* outLen) {
    StrBuf out;
    int lastPos = 0;
    sb_init(&out);
    while (1) {
        int s = -1, e = -1, p = lastPos;
        while ((p = find_ch(src, slen, '<', p)) >= 0) {
            if (p + 3 < slen &&
                lower_ch(src[p + 1]) == 'u' &&
                lower_ch(src[p + 2]) == 's' &&
                lower_ch(src[p + 3]) == 'e') {
                e = find_ch(src, slen, '>', p + 4);
                if (e < 0) { p++; continue; }
                s = p;
                break;
            }
            p++;
        }
        if (s < 0) {
            sb_append(&out, src + lastPos, slen - lastPos);
            break;
        }
        sb_append(&out, src + lastPos, s - lastPos);
        {
            AttrSet ua;
            const char* hrefRaw;
            get_attrs(src + s + 4, e - (s + 4), &ua);
            hrefRaw = attr_get(&ua, "href");
            if (!hrefRaw) hrefRaw = attr_get(&ua, "xlink:href");
            if (hrefRaw && hrefRaw[0] == '#' && hrefRaw[1] != '\0') {
                const char* id = hrefRaw + 1;
                int idLen = (int)strlen(id);
                int q = 0;
                int replaced = 0;
                while (!replaced && (q = find_ch(src, slen, '<', q)) >= 0) {
                    if (q + 1 < slen && is_namech(src[q + 1])) {
                        int nameEnd = q + 2;
                        int te;
                        while (nameEnd < slen && is_namech(src[nameEnd]))
                            nameEnd++;
                        te = find_ch(src, slen, '>', nameEnd);
                        if (te >= 0) {
                            AttrSet ea;
                            get_attrs(src + nameEnd, te - nameEnd, &ea);
                            {
                                const char* elId = attr_get(&ea, "id");
                                if (elId && (int)strlen(elId) == idLen &&
                                    memcmp(elId, id, (size_t)idLen) == 0) {
                                    sb_append(&out, "<", 1);
                                    sb_append(&out, src + q + 1,
                                              nameEnd - (q + 1));
                                    sb_append(&out, src + nameEnd,
                                              te - nameEnd);
                                    sb_append(&out, ">", 1);
                                    replaced = 1;
                                }
                            }
                            attrs_free(&ea);
                        }
                        q = te >= 0 ? te : q + 1;
                    } else {
                        q++;
                    }
                    q++;
                }
            }
            attrs_free(&ua);
        }
        lastPos = e + 1;
    }
    *outLen = out.len;
    if (!out.data) {
        out.data = (char*)pluto_malloc(1);
        if (out.data) out.data[0] = '\0';
    }
    return out.data;
}

/* ── viewBox / width / height scanners ──────────────────────────────── */

static int parse_viewbox(const char* x, int len,
                         double* mx, double* my, double* vw, double* vh) {
    int pos = 0;
    while ((pos = find_str(x, len, "viewBox", 7, pos)) >= 0) {
        int p = pos + 7;
        while (p < len && is_ws(x[p])) p++;
        if (p < len && x[p] == '=') {
            double nums[4];
            int ok = 1, k;
            p++;
            while (p < len && is_ws(x[p])) p++;
            if (p < len && (x[p] == '"' || x[p] == '\'')) {
                p++;
                while (p < len && is_ws(x[p])) p++;
                for (k = 0; k < 4 && ok; k++) {
                    int n0 = p;
                    while (p < len && is_numch_viewbox(x[p])) p++;
                    if (p == n0 ||
                        !lua_number_strict(x + n0, p - n0, &nums[k])) {
                        ok = 0;
                        break;
                    }
                    if (k < 3) {         /* %s+ separator required */
                        int ws0 = p;
                        while (p < len && is_ws(x[p])) p++;
                        if (p == ws0) ok = 0;
                    }
                }
                if (ok) {
                    *mx = nums[0]; *my = nums[1];
                    *vw = nums[2]; *vh = nums[3];
                    return 1;
                }
            }
        }
        pos = pos + 1;
    }
    return 0;
}

/* First occurrence anywhere whose tail matches key ws '=' ws quote NUMS. */
static int parse_len_attr(const char* x, int len, const char* key,
                          double* out) {
    int klen = (int)strlen(key);
    int pos = 0;
    while ((pos = find_str(x, len, key, klen, pos)) >= 0) {
        int p = pos + klen;
        while (p < len && is_ws(x[p])) p++;
        if (p < len && x[p] == '=') {
            p++;
            while (p < len && is_ws(x[p])) p++;
            if (p < len && (x[p] == '"' || x[p] == '\'')) {
                int n0 = ++p;
                while (p < len && is_numch_len(x[p])) p++;
                if (p > n0 && lua_number_strict(x + n0, p - n0, out))
                    return 1;
            }
        }
        pos = pos + 1;
    }
    return 0;
}

/* ── tag scanner ─────────────────────────────────────────────────────── */

static int is_container_name(const char* tag) {
    /* NOTE: kept verbatim from svg.lua; "clipPath" never matches because
     * tag names are lowercased before this check (faithful quirk). */
    return strcmp(tag, "svg") == 0 || strcmp(tag, "g") == 0 ||
           strcmp(tag, "a") == 0 || strcmp(tag, "symbol") == 0 ||
           strcmp(tag, "mask") == 0 || strcmp(tag, "clipPath") == 0 ||
           strcmp(tag, "defs") == 0 || strcmp(tag, "pattern") == 0 ||
           strcmp(tag, "marker") == 0 || strcmp(tag, "switch") == 0;
}

typedef struct {
    Canvas cv;
    double scale, minX, minY;
    int skipDepth;
    int* stack;
    int stackN, stackCap;
    int drawn;
} SvgState;

static void state_push(SvgState* st, int v) {
    if (st->stackN >= st->stackCap) {
        int ncap = st->stackCap ? st->stackCap * 2 : 16;
        int* ns = (int*)pluto_malloc(sizeof(int) * (size_t)ncap);
        if (!ns) return;
        if (st->stack) {
            memcpy(ns, st->stack, sizeof(int) * (size_t)st->stackN);
            pluto_free(st->stack);
        }
        st->stack = ns;
        st->stackCap = ncap;
    }
    st->stack[st->stackN++] = v;
}

static int tx(SvgState* st, double x) {
    return (int)floor((x - st->minX) * st->scale);
}

static int ty(SvgState* st, double y) {
    return (int)floor((y - st->minY) * st->scale);
}

/* ── path flattening ─────────────────────────────────────────────────── */

static void draw_path(SvgState* st, const char* d, int dlen) {
    Canvas* cv = &st->cv;
    double curX = 0, curY = 0, startX = 0, startY = 0;
    double lastCtrlX = 0, lastCtrlY = 0;
    int hasPoint = 0;
    int i = 0;

    while (i < dlen) {
        char cmd;
        int a0, a1;
        Nums coords;
        int isRel;
        char cUp;

        while (i < dlen && !((d[i] >= 'a' && d[i] <= 'z') ||
                             (d[i] >= 'A' && d[i] <= 'Z')))
            i++;
        if (i >= dlen) break;
        cmd = d[i++];
        a0 = i;
        while (i < dlen && !((d[i] >= 'a' && d[i] <= 'z') ||
                             (d[i] >= 'A' && d[i] <= 'Z')))
            i++;
        a1 = i;

        tokenize_numbers(d + a0, a1 - a0, &coords);
        isRel = (cmd >= 'a' && cmd <= 'z');
        cUp = (char)(isRel ? cmd - 32 : cmd);

#define PT(ix, ox, oy)                                            \
    do {                                                          \
        double px_ = (ix) < coords.n ? coords.v[(ix)] : 0.0;      \
        double py_ = (ix) + 1 < coords.n ? coords.v[(ix) + 1] : 0.0; \
        if (isRel) { (ox) = curX + px_; (oy) = curY + py_; }      \
        else { (ox) = px_; (oy) = py_; }                          \
    } while (0)

        switch (cUp) {
            case 'M': {
                int k;
                for (k = 0; k < coords.n; k += 2) {
                    double nx, ny;
                    PT(k, nx, ny);
                    if (k == 0) {
                        curX = nx; curY = ny;
                        startX = nx; startY = ny;
                        hasPoint = 1;
                    } else {
                        cv_line(cv, tx(st, curX), ty(st, curY),
                                tx(st, nx), ty(st, ny));
                        curX = nx; curY = ny;
                    }
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'L': {
                int k;
                for (k = 0; k < coords.n; k += 2) {
                    double nx, ny;
                    PT(k, nx, ny);
                    cv_line(cv, tx(st, curX), ty(st, curY),
                            tx(st, nx), ty(st, ny));
                    curX = nx; curY = ny;
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'H': {
                int k;
                for (k = 0; k < coords.n; k++) {
                    double nx = isRel ? curX + coords.v[k] : coords.v[k];
                    cv_line(cv, tx(st, curX), ty(st, curY),
                            tx(st, nx), ty(st, curY));
                    curX = nx;
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'V': {
                int k;
                for (k = 0; k < coords.n; k++) {
                    double ny = isRel ? curY + coords.v[k] : coords.v[k];
                    cv_line(cv, tx(st, curX), ty(st, curY),
                            tx(st, curX), ty(st, ny));
                    curY = ny;
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'Z': {
                if (hasPoint) {
                    cv_line(cv, tx(st, curX), ty(st, curY),
                            tx(st, startX), ty(st, startY));
                    curX = startX; curY = startY;
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'C': {
                int k;
                for (k = 0; k < coords.n; k += 6) {
                    double x1, y1, x2, y2, x3, y3;
                    int t;
                    PT(k, x1, y1);
                    PT(k + 2, x2, y2);
                    PT(k + 4, x3, y3);
                    for (t = 1; t <= 8; t++) {
                        double u = t / 8.0;
                        double nx, ny;
                        nx = (1-u)*(1-u)*(1-u)*curX
                             + 3*(1-u)*(1-u)*u*x1
                             + 3*(1-u)*u*u*x2 + u*u*u*x3;
                        ny = (1-u)*(1-u)*(1-u)*curY
                             + 3*(1-u)*(1-u)*u*y1
                             + 3*(1-u)*u*u*y2 + u*u*u*y3;
                        cv_line(cv, tx(st, curX), ty(st, curY),
                                tx(st, nx), ty(st, ny));
                        curX = nx; curY = ny;
                    }
                    lastCtrlX = x2; lastCtrlY = y2;
                }
                break;
            }
            case 'S': {
                int k;
                for (k = 0; k < coords.n; k += 4) {
                    double sx1 = curX * 2 - lastCtrlX;
                    double sy1 = curY * 2 - lastCtrlY;
                    double x2, y2, x3, y3;
                    int t;
                    PT(k, x2, y2);
                    PT(k + 2, x3, y3);
                    for (t = 1; t <= 8; t++) {
                        double u = t / 8.0;
                        double nx, ny;
                        nx = (1-u)*(1-u)*(1-u)*curX
                             + 3*(1-u)*(1-u)*u*sx1
                             + 3*(1-u)*u*u*x2 + u*u*u*x3;
                        ny = (1-u)*(1-u)*(1-u)*curY
                             + 3*(1-u)*(1-u)*u*sy1
                             + 3*(1-u)*u*u*y2 + u*u*u*y3;
                        cv_line(cv, tx(st, curX), ty(st, curY),
                                tx(st, nx), ty(st, ny));
                        curX = nx; curY = ny;
                    }
                    lastCtrlX = x2; lastCtrlY = y2;
                }
                break;
            }
            case 'Q': {
                int k;
                for (k = 0; k < coords.n; k += 4) {
                    double x1, y1, x2, y2;
                    int t;
                    PT(k, x1, y1);
                    PT(k + 2, x2, y2);
                    for (t = 1; t <= 6; t++) {
                        double u = t / 6.0;
                        double nx = (1-u)*(1-u)*curX
                                    + 2*(1-u)*u*x1 + u*u*x2;
                        double ny = (1-u)*(1-u)*curY
                                    + 2*(1-u)*u*y1 + u*u*y2;
                        cv_line(cv, tx(st, curX), ty(st, curY),
                                tx(st, nx), ty(st, ny));
                        curX = nx; curY = ny;
                    }
                    lastCtrlX = x1; lastCtrlY = y1;
                }
                break;
            }
            case 'T': {
                int k;
                for (k = 0; k < coords.n; k += 2) {
                    double qx1 = curX * 2 - lastCtrlX;
                    double qy1 = curY * 2 - lastCtrlY;
                    double x2, y2;
                    int t;
                    PT(k, x2, y2);
                    for (t = 1; t <= 6; t++) {
                        double u = t / 6.0;
                        double nx = (1-u)*(1-u)*curX
                                    + 2*(1-u)*u*qx1 + u*u*x2;
                        double ny = (1-u)*(1-u)*curY
                                    + 2*(1-u)*u*qy1 + u*u*y2;
                        cv_line(cv, tx(st, curX), ty(st, curY),
                                tx(st, nx), ty(st, ny));
                        curX = nx; curY = ny;
                    }
                    lastCtrlX = qx1; lastCtrlY = qy1;
                }
                break;
            }
            case 'A': {
                int k;
                for (k = 0; k < coords.n; k += 7) {
                    double ex, ey;
                    PT(k + 5, ex, ey);
                    cv_line(cv, tx(st, curX), ty(st, curY),
                            tx(st, ex), ty(st, ey));
                    curX = ex; curY = ey;
                }
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            default:
                break;
        }
#undef PT
        nums_free(&coords);
    }
}

/* ── shape dispatch for an opening tag ──────────────────────────────── */

/* tonumber(x) or default — used where svg.lua writes (tonumber(a[k]) or d). */
static double num_attr_or(const TagAttrs* t, const char* key, double def) {
    const char* s = ta_get(t, key);
    double v;
    if (!s) return def;
    if (lua_number_strict(s, (int)strlen(s), &v)) return v;
    return def;
}

static void shape_open(SvgState* st, const char* tag, TagAttrs* t) {
    Canvas* cv = &st->cv;
    double sc = st->scale;

    if (strcmp(tag, "rect") == 0 && has_ink(t) &&
        ta_has(t, "x") && ta_has(t, "y") &&
        ta_has(t, "width") && ta_has(t, "height")) {
        int x = tx(st, num_or0(ta_get(t, "x")));
        int y = ty(st, num_or0(ta_get(t, "y")));
        int w = (int)floor(num_or0(ta_get(t, "width")) * sc);
        int h = (int)floor(num_or0(ta_get(t, "height")) * sc);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (ta_has(t, "rx") || ta_has(t, "ry")) {
            double rraw = num_attr_or(t, "rx", 2.0);
            int r = (int)floor(rraw * sc);
            if (r < 1) r = 1;
            if (r > 4) r = 4;
            cv_round_rect(cv, x, y, w, h, r);
        } else {
            cv_rect(cv, x, y, w, h);
        }
        st->drawn++;
    } else if (strcmp(tag, "circle") == 0 && has_ink(t) &&
               ta_has(t, "cx") && ta_has(t, "cy") && ta_has(t, "r")) {
        int r = (int)floor(num_attr_or(t, "r", 1.0) * sc);
        if (r < 1) r = 1;
        cv_circle(cv, tx(st, num_or0(ta_get(t, "cx"))),
                  ty(st, num_or0(ta_get(t, "cy"))), r, OCT_ALL);
        st->drawn++;
    } else if (strcmp(tag, "ellipse") == 0 && has_ink(t) &&
               ta_has(t, "cx") && ta_has(t, "cy") &&
               ta_has(t, "rx") && ta_has(t, "ry")) {
        int ecx = tx(st, num_or0(ta_get(t, "cx")));
        int ecy = ty(st, num_or0(ta_get(t, "cy")));
        int erx = (int)floor(num_attr_or(t, "rx", 1.0) * sc);
        int ery = (int)floor(num_attr_or(t, "ry", 1.0) * sc);
        double stepsD = floor((erx < ery ? erx : ery) / 2.0);
        int steps = (int)stepsD;
        int si;
        if (erx < 1) erx = 1;
        if (ery < 1) ery = 1;
        if (steps > 5) steps = 5;
        if (steps < 2) steps = 2;
        for (si = 1; si <= steps; si++) {
            double f = 1 - ((si - 1) / (double)steps) * 0.6;
            int exv = ecx - (int)floor(erx * f);
            int eyv = ecy - (int)floor(ery * f);
            int ew = (int)floor(erx * f * 2);
            int eh = (int)floor(ery * f * 2);
            if (ew < 2) ew = 2;
            if (eh < 2) eh = 2;
            canvas_ellipse(cv, exv, eyv, ew, eh);
        }
        st->drawn++;
    } else if (strcmp(tag, "line") == 0 && has_ink(t) &&
               ta_has(t, "x1") && ta_has(t, "y1") &&
               ta_has(t, "x2") && ta_has(t, "y2")) {
        cv_line(cv, tx(st, num_or0(ta_get(t, "x1"))),
                ty(st, num_or0(ta_get(t, "y1"))),
                tx(st, num_or0(ta_get(t, "x2"))),
                ty(st, num_or0(ta_get(t, "y2"))));
        st->drawn++;
    } else if ((strcmp(tag, "polygon") == 0 || strcmp(tag, "polyline") == 0)
               && has_ink(t) && ta_has(t, "points")) {
        Nums pts;
        int i;
        tokenize_numbers(ta_get(t, "points"),
                         (int)strlen(ta_get(t, "points")), &pts);
        for (i = 0; i + 3 < pts.n; i += 2) {
            cv_line(cv, tx(st, pts.v[i]), ty(st, pts.v[i + 1]),
                    tx(st, pts.v[i + 2]), ty(st, pts.v[i + 3]));
        }
        if (strcmp(tag, "polygon") == 0 && pts.n >= 4) {
            cv_line(cv, tx(st, pts.v[pts.n - 2]), ty(st, pts.v[pts.n - 1]),
                    tx(st, pts.v[0]), ty(st, pts.v[1]));
        }
        nums_free(&pts);
        st->drawn++;
    } else if (strcmp(tag, "path") == 0 && has_ink(t) && ta_has(t, "d")) {
        const char* d = ta_get(t, "d");
        draw_path(st, d, (int)strlen(d));
        st->drawn++;
    }
}

/* ── main decode ─────────────────────────────────────────────────────── */

int svg_decode_gray(const char* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH,
                    int* outDrawn) {
    double vbMinX = 0, vbMinY = 0, vbW = 0, vbH = 0;
    double wRaw = 0, hRaw = 0;
    int hasVb, hasW, hasH;
    double srcW, srcH, minX, minY, scale;
    int targetW, targetH, yi;
    char* body = NULL;
    int bodyLen = 0;
    SvgState st;

    if (outRows == NULL || outW == NULL || outH == NULL) return -1;
    *outRows = NULL;
    *outW = *outH = 0;
    if (outDrawn) *outDrawn = 0;
    if (data == NULL || len <= 0 || len > 0x7FFFFFF0) return -1;

    if (find_str(data, (int)len, "<svg", 4, 0) < 0) return -1;

    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    hasVb = parse_viewbox(data, (int)len, &vbMinX, &vbMinY, &vbW, &vbH);
    hasW = parse_len_attr(data, (int)len, "width", &wRaw);
    hasH = parse_len_attr(data, (int)len, "height", &hRaw);

    srcW = hasVb ? vbW : (hasW ? wRaw : 100.0);
    srcH = hasVb ? vbH : (hasH ? hRaw : 100.0);
    minX = hasVb ? vbMinX : 0.0;
    minY = hasVb ? vbMinY : 0.0;

    if (srcW <= 0 || srcH <= 0) return -1;

    scale = maxW / srcW;
    if (maxH / srcH < scale) scale = maxH / srcH;
    if (scale > 2) scale = 2;
    targetW = (int)floor(srcW * scale);
    targetH = (int)floor(srcH * scale);
    if (targetW < 20) targetW = 20;
    if (targetH < 20) targetH = 20;

    memset(&st, 0, sizeof(st));
    st.cv.w = targetW;
    st.cv.h = targetH;
    st.cv.rows = (uint8_t**)pluto_malloc(sizeof(uint8_t*) * (size_t)targetH);
    if (!st.cv.rows) return -1;
    for (yi = 0; yi < targetH; yi++) {
        st.cv.rows[yi] = (uint8_t*)pluto_malloc((size_t)targetW);
        if (!st.cv.rows[yi]) {
            for (; yi > 0; yi--) pluto_free(st.cv.rows[yi - 1]);
            pluto_free(st.cv.rows);
            return -1;
        }
        memset(st.cv.rows[yi], 255, (size_t)targetW);
    }
    st.scale = scale;
    st.minX = minX;
    st.minY = minY;
    st.skipDepth = 0;
    st.drawn = 0;

    body = expand_uses(data, (int)len, &bodyLen);

    /* scan tags and draw (errors cannot throw in C; bounds are guarded) */
    {
        int pos = 0;
        int blen = bodyLen;
        while (1) {
            int s = find_ch(body, blen, '<', pos);
            int e, inLen;
            const char* in;
            char h0, h1, h2;
            if (s < 0) break;
            e = find_ch(body, blen, '>', s);
            if (e < 0) break;
            in = body + s + 1;
            inLen = e - s - 1;
            pos = e + 1;
            h0 = inLen > 0 ? in[0] : 0;
            h1 = inLen > 1 ? in[1] : 0;
            h2 = inLen > 2 ? in[2] : 0;
            if (h0 == '!' && h1 == '-' && h2 == '-') {
                int ce = find_str(body, blen, "-->", 3, e);
                if (ce >= 0) pos = ce + 3;
            } else if (h0 == '!' && h1 == '[') {
                int ce = find_str(body, blen, "]]>", 3, e);
                if (ce >= 0) pos = ce + 3;
            } else if (!((h0 == '!' && h1 == 'D') ||
                         (h0 == '!' && h1 == 'd') ||
                         (h0 == '?' && h1 == 'x') ||
                         (h0 == '?' && h1 == 'X'))) {
                int t0 = 0, t1 = inLen;
                while (t0 < t1 && is_ws(in[t0])) t0++;
                while (t1 > t0 && is_ws(in[t1 - 1])) t1--;
                if (t1 > t0) {
                    int isClose = in[t0] == '/';
                    int b0 = isClose ? t0 + 1 : t0;
                    int b1 = t1;
                    int isSelfClose;
                    int nameEnd;
                    char* tagName;
                    if (in[b1 - 1] == '/') b1--;
                    isSelfClose = t1 > t0 && in[t1 - 1] == '/';
                    if (b1 > b0 && is_namech(in[b0])) {
                        nameEnd = b0 + 1;
                        while (nameEnd < b1 && is_namech(in[nameEnd]))
                            nameEnd++;
                        tagName = dup_lower(in + b0, nameEnd - b0);
                        if (tagName) {
                            if (!isClose) {
                                TagAttrs t;
                                tagattrs_load(&t, in + nameEnd,
                                              b1 - nameEnd);
                                {
                                    int ownHidden = is_hidden(&t);
                                    int enteringSkip =
                                        strcmp(tagName, "defs") == 0 ||
                                        ownHidden;
                                    if (is_container_name(tagName) &&
                                        !isSelfClose) {
                                        state_push(&st, st.skipDepth);
                                        if (enteringSkip) st.skipDepth++;
                                    }
                                    if (!enteringSkip && st.skipDepth == 0) {
                                        shape_open(&st, tagName, &t);
                                    }
                                }
                                tagattrs_free(&t);
                            } else {
                                if (is_container_name(tagName)) {
                                    if (st.stackN > 0)
                                        st.skipDepth =
                                            st.stack[--st.stackN];
                                }
                            }
                            pluto_free(tagName);
                        }
                    }
                }
            }
        }
    }

    pluto_free(body);
    pluto_free(st.stack);

    if (st.drawn == 0) {
        svg_free_rows(st.cv.rows, targetH);
        return -1;
    }
    *outRows = st.cv.rows;
    *outW = targetW;
    *outH = targetH;
    if (outDrawn) *outDrawn = st.drawn;
    return 0;
}

void svg_free_rows(uint8_t** rows, int h) {
    if (rows == NULL) return;
    for (; h > 0; h--) pluto_free(rows[h - 1]);
    pluto_free(rows);
}

/* ── device/simulator bitmap wrapper ─────────────────────────────────── */

#ifdef PLUTO_SVG_PD
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} SvgCtx;

static int svg_pixel(void* ud, int x, int y) {
    SvgCtx* c = (SvgCtx*)ud;
    if (!c || !c->rows || y < 0 || y >= c->h || x < 0 || x >= c->w)
        return 255;
    return c->rows[y][x];
}

struct LCDBitmap* svg_decode(struct PlaydateAPI* pd, const char* data,
                             size_t len, int maxW, int maxH) {
    uint8_t** rows = NULL;
    int w = 0, h = 0, drawn = 0;
    struct LCDBitmap* img;
    SvgCtx ctx;
    if (!pd) return NULL;
    if (svg_decode_gray(data, len, maxW, maxH,
                        &rows, &w, &h, &drawn) != 0)
        return NULL;
    ctx.rows = rows;
    ctx.w = w;
    ctx.h = h;
    img = dither_to_image(pd, svg_pixel, &ctx, w, h);
    svg_free_rows(rows, h);
    return img;
}
#endif
