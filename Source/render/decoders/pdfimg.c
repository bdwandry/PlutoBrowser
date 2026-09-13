/*
 * PlutoBrowser — pdfimg.c
 * PDF raster-image extractor (see pdfimg.h). Scans the raw bytes for image
 * XObject dictionaries ("<< ... /Subtype /Image ... >> stream"), reads /Width,
 * /Height, /ColorSpace, /Filter and the following stream's data, inflates it
 * when FlateDecode (reusing the PNG decoder's zlib inflate), and maps
 * DeviceRGB/DeviceGray bytes to gray. DCTDecode (JPEG inside PDF) is handed
 * to jpeg_decode. Vector-only PDFs yield NULL (caller falls back to the
 * unsupported-image path).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pdfimg.h"
#include "dither.h"
#include "scale.h"
#include "jpeg.h"
#include "inflate.h"
#include "../../core/logger.h"


static const void *memstr_local(const void *hay, size_t hayLen,
                                const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || hayLen < nl) return NULL;
    const uint8_t *h = (const uint8_t *)hay;
    for (size_t i = 0; i + nl <= hayLen; i++)
    {
        if (memcmp(h + i, needle, nl) == 0) return h + i;
    }
    return NULL;
}

typedef struct
{
    const uint8_t *data;
    size_t len;
    int scaleNum, scaleDen;
    int srcW, srcH, srcComponents;
    const uint8_t *gray; /* srcW*srcH gray plane (owned) */
} PdfPix;

static uint8_t pdf_pixel_gray(void *ud, int outX, int outY)
{
    PdfPix *p = (PdfPix *)ud;
    int srcX = outX * p->scaleNum / p->scaleDen;
    int srcY = outY * p->scaleNum / p->scaleDen;
    if (srcX > p->srcW - 1) srcX = p->srcW - 1;
    if (srcY > p->srcH - 1) srcY = p->srcH - 1;
    return p->gray[(size_t)srcY * (size_t)p->srcW + (size_t)srcX];
}

static int pdf_dict_value(const char *seg, size_t segLen, const char *key,
                          int *intOut)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl + 1 < segLen; i++)
    {
        if (strncmp(seg + i, key, kl) == 0 && seg[i + kl] == ' ')
        {
            size_t j = i + kl + 1;
            while (j < segLen && seg[j] == ' ') j++;
            if (j < segLen && seg[j] >= '0' && seg[j] <= '9')
            {
                *intOut = atoi(seg + j);
                return 1;
            }
        }
    }
    return 0;
}

/* Locate the raw stream bytes following "stream\r?\n" at/after `from`. */
static const uint8_t *pdf_stream_data(const uint8_t *data, size_t len,
                                      size_t from, size_t *outLen)
{
    for (size_t i = from; i + 6 < len; i++)
    {
        if (memcmp(data + i, "stream", 6) == 0)
        {
            /* not "endstream" */
            if (i >= 3 && memcmp(data + i - 3, "end", 3) == 0) continue;
            size_t j = i + 6;
            if (j < len && data[j] == '\r') j++;
            if (j < len && data[j] == '\n') j++;
            const uint8_t *stop =
                (const uint8_t *)memstr_local(data + j, len - j, "endstream");
            if (!stop) return NULL;
            size_t n = (size_t)(stop - (data + j));
            while (n > 0)
            {
                uint8_t c = data[j + n - 1];
                if (c == '\n' || c == '\r')
                    n--;
                else
                    break;
            }
            *outLen = n;
            return data + j;
        }
    }
    return NULL;
}

/* ── Mini vector content-stream renderer ───────────────────────────────
 * Rasterizes a page-1 content stream (q/Q/cm/rg/RG/g/G/w/re/m/l/c/h/S/s,
 * fill/stroke paint, clip and shading ops) into a gray raster, then
 * Bayer-dithers exactly like every other decoder. Covers the vector PDFs
 * in the wild (e.g. the benchmark corpus's test_pattern.pdf: rects,
 * polygons, grid lines, and axial shadings). No external deps; fixed-size
 * path/edge buffers. */

typedef struct { float a, b, c, d, e, f; } PdfM;

/* Page space → device: flip Y, fit MediaBox into W×H. */
static PdfM pdfv_page_matrix(float mx0, float my0, float mx1, float my1,
                             int W, int H)
{
    PdfM m;
    float sx = (float)W / (mx1 - mx0);
    float sy = (float)H / (my1 - my0);
    m.a = sx;
    m.b = 0;
    m.c = 0;
    m.d = -sy;
    m.e = -mx0 * sx;
    m.f = my1 * sy;
    return m;
}

/* Row-vector composition (PDF semantics): p × (m × g) = (p × m) × g. */
static PdfM pdfv_mul(PdfM m, PdfM g)
{
    PdfM r;
    r.a = m.a * g.a + m.b * g.c;
    r.b = m.a * g.b + m.b * g.d;
    r.c = m.c * g.a + m.d * g.c;
    r.d = m.c * g.b + m.d * g.d;
    r.e = m.e * g.a + m.f * g.c + g.e;
    r.f = m.e * g.b + m.f * g.d + g.f;
    return r;
}

static void pdfv_apply(PdfM m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

#define PDFV_MAX_SUBS 96
#define PDFV_MAX_PTS 160

typedef struct
{
    float px[PDFV_MAX_PTS];
    float py[PDFV_MAX_PTS];
    int n;
    int closed;
} PdfSub;

typedef struct
{
    PdfM ctm;
    float fr, fg, fb; /* fill color 0..1 */
    float sr, sg, sb; /* stroke color 0..1 */
    float lw;         /* line width, user units */
    int cx0, cy0, cx1, cy1; /* clip rect, device px (inclusive-exclusive) */
    int hasClip;
} PdfGs;

typedef struct
{
    uint8_t *gray;
    int W, H;
    PdfGs *gs;
    PdfSub *subs;
    int nSubs;
    int fillGray;
} PdfFillCtx;

typedef struct { float x1, y1, x2, y2; } PdfEdge;

static float pdfv_sqrt(float v);

/* Nonzero-winding scanline fill of all subpaths (device space). */
static void pdfv_fill(uint8_t *gray, int W, int H, const PdfSub *subs,
                      int nSubs, uint8_t val, const PdfGs *gs)
{
    int cap = 0, i;
    PdfEdge *edges;
    float ymin = 1e30f, ymax = -1e30f;
    int x0 = 0, x1 = W, y0 = 0, y1 = H;

    if (gs->hasClip)
    {
        x0 = gs->cx0;
        y0 = gs->cy0;
        x1 = gs->cx1;
        y1 = gs->cy1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
    }
    for (i = 0; i < nSubs; i++) cap += subs[i].n;
    if (cap < 3) return;
    edges = (PdfEdge *)malloc(sizeof(PdfEdge) * (size_t)cap);
    if (!edges) return;
    int nE = 0;
    for (i = 0; i < nSubs; i++)
    {
        const PdfSub *s = &subs[i];
        for (int j = 0; j < s->n; j++)
        {
            int k = (j + 1 < s->n) ? j + 1 : 0; /* fill: implicit close */
            float ax = s->px[j], ay = s->py[j];
            float bx = s->px[k], by = s->py[k];
            if (ay == by) continue;
            edges[nE].x1 = ax;
            edges[nE].y1 = ay;
            edges[nE].x2 = bx;
            edges[nE].y2 = by;
            nE++;
            if (ay < ymin) ymin = ay;
            if (ay > ymax) ymax = ay;
            if (by < ymin) ymin = by;
            if (by > ymax) ymax = by;
        }
    }
    if (nE == 0)
    {
        free(edges);
        return;
    }
    int sy = (int)ymin;
    if (sy < y0) sy = y0;
    int ey = (int)ymax + 1;
    if (ey > y1) ey = y1;

    float *xs = (float *)malloc(sizeof(float) * (size_t)nE);
    int *ds = (int *)malloc(sizeof(int) * (size_t)nE);
    if (xs && ds)
    {
        for (int y = sy; y < ey; y++)
        {
            float yc = (float)y + 0.5f;
            int nX = 0;
            for (int e = 0; e < nE; e++)
            {
                float ay = edges[e].y1, by = edges[e].y2;
                if ((ay <= yc && by > yc) || (by <= yc && ay > yc))
                {
                    float t = (yc - ay) / (by - ay);
                    xs[nX] = edges[e].x1 + t * (edges[e].x2 - edges[e].x1);
                    ds[nX] = (by > ay) ? 1 : -1;
                    nX++;
                }
            }
            /* insertion sort by x */
            for (int a = 1; a < nX; a++)
            {
                float fx = xs[a];
                int fd = ds[a];
                int b = a - 1;
                while (b >= 0 && xs[b] > fx)
                {
                    xs[b + 1] = xs[b];
                    ds[b + 1] = ds[b];
                    b--;
                }
                xs[b + 1] = fx;
                ds[b + 1] = fd;
            }
            uint8_t *row = gray + (size_t)y * (size_t)W;
            int wind = 0;
            int spanStart = 0;
            for (int a = 0; a < nX; a++)
            {
                int before = wind;
                wind += ds[a];
                if (before == 0 && wind != 0)
                    spanStart = a;
                else if (before != 0 && wind == 0)
                {
                    float fa = xs[spanStart], fb2 = xs[a];
                    int px0 = (int)(fa - 0.5f);
                    if ((float)px0 < fa - 0.5f) px0++;
                    if (px0 < x0) px0 = x0;
                    int px1 = (int)(fb2 - 0.5f);
                    if ((float)px1 < fb2 - 0.5f) px1++;
                    px1 -= 1;
                    if (px1 > x1 - 1) px1 = x1 - 1;
                    for (int px = px0; px <= px1; px++)
                        row[px] = val;
                }
            }
        }
    }
    free(xs);
    free(ds);
    free(edges);
}

static void pdfv_dot(uint8_t *gray, int W, int H, int x, int y, uint8_t val)
{
    if (x >= 0 && y >= 0 && x < W && y < H)
        gray[(size_t)y * (size_t)W + (size_t)x] = val;
}

static void pdfv_line(uint8_t *gray, int W, int H, int x0, int y0, int x1,
                      int y1, uint8_t val)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    int err = dx - dy;
    for (;;)
    {
        pdfv_dot(gray, W, H, x0, y0, val);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

static float pdfv_stroke_width(const PdfGs *gs);
static void pdfv_seg(uint8_t *gray, int W, int H, const PdfSub *s, int j,
                     float wEff, uint8_t val);
static void pdfv_seg_wrap(uint8_t *gray, int W, int H, const PdfSub *s,
                          float wEff, uint8_t val);

/* Stroke all subpaths. Thin lines (< ~1.6px effective) get a plain
 * Bresenham pass; thicker widths stamp small squares along each segment. */
static void pdfv_stroke(uint8_t *gray, int W, int H, const PdfSub *subs,
                        int nSubs, uint8_t val, const PdfGs *gs)
{
    float wEff = pdfv_stroke_width(gs);

    for (int i = 0; i < nSubs; i++)
    {
        const PdfSub *s = &subs[i];
        if (s->n == 0) continue;
        for (int j = 0; j + 1 < s->n; j++)
            pdfv_seg(gray, W, H, s, j, wEff, val);
        if (s->closed && s->n > 2)
            pdfv_seg_wrap(gray, W, H, s, wEff, val);
    }
}

static float pdfv_stroke_width(const PdfGs *gs)
{
    float det = gs->ctm.a * gs->ctm.d - gs->ctm.b * gs->ctm.c;
    float scale = det < 0 ? -det : det;
    if (scale < 0.0001f) scale = 1.0f;
    return gs->lw * pdfv_sqrt(scale);
}

static void pdfv_seg(uint8_t *gray, int W, int H, const PdfSub *s, int j,
                     float wEff, uint8_t val)
{
    int ax = (int)(s->px[j] + 0.5f), ay = (int)(s->py[j] + 0.5f);
    int bx = (int)(s->px[j + 1] + 0.5f), by = (int)(s->py[j + 1] + 0.5f);
    int thick = wEff >= 1.6f;
    if (!thick)
    {
        pdfv_line(gray, W, H, ax, ay, bx, by, val);
        return;
    }
    float dx = s->px[j + 1] - s->px[j], dy = s->py[j + 1] - s->py[j];
    float len = pdfv_sqrt(dx * dx + dy * dy);
    int steps = (int)(len * 2.0f) + 1;
    if (steps < 1) steps = 1;
    int r = (int)(wEff * 0.5f + 0.5f);
    if (r < 1) r = 1;
    for (int k = 0; k <= steps; k++)
    {
        float t = (float)k / (float)steps;
        float fx = s->px[j] + t * dx, fy = s->py[j] + t * dy;
        int cx = (int)(fx + 0.5f), cy = (int)(fy + 0.5f);
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++)
                pdfv_dot(gray, W, H, cx + ox, cy + oy, val);
    }
}

static void pdfv_seg_wrap(uint8_t *gray, int W, int H, const PdfSub *s,
                          float wEff, uint8_t val)
{
    /* closing segment of a closed subpath */
    int ax = (int)(s->px[s->n - 1] + 0.5f), ay = (int)(s->py[s->n - 1] + 0.5f);
    int bx = (int)(s->px[0] + 0.5f), by = (int)(s->py[0] + 0.5f);
    if (wEff >= 1.6f)
    {
        /* thickness handled by pdfv_seg-style stamping */
        PdfSub one;
        one.n = 2;
        one.closed = 0;
        one.px[0] = s->px[s->n - 1];
        one.py[0] = s->py[s->n - 1];
        one.px[1] = s->px[0];
        one.py[1] = s->py[0];
        pdfv_seg(gray, W, H, &one, 0, wEff, val);
        return;
    }
    pdfv_line(gray, W, H, ax, ay, bx, by, val);
}

static float pdfv_sqrt(float v)
{
    if (v <= 0.0f) return 0.0f;
    float x = v;
    for (int i = 0; i < 12; i++)
        x = 0.5f * (x + v / x);
    return x;
}

/* ── PDF dict text helpers ────────────────────────────────────────────── */

static int pdfv_key_pos(const char *seg, size_t segLen, const char *key,
                        size_t *valPos)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl < segLen; i++)
    {
        if (seg[i] == '/' && strncmp(seg + i + 1, key, kl) == 0)
        {
            size_t j = i + 1 + kl;
            while (j < segLen && (seg[j] == ' ' || seg[j] == '\n' ||
                                  seg[j] == '\r' || seg[j] == '\t'))
                j++;
            *valPos = j;
            return 1;
        }
    }
    return 0;
}

static int pdfv_int_key(const char *seg, size_t segLen, const char *key,
                        int *out)
{
    size_t p;
    if (!pdfv_key_pos(seg, segLen, key, &p)) return 0;
    char *end = NULL;
    long v = strtol(seg + p, &end, 10);
    if (end == seg + p) return 0;
    *out = (int)v;
    return 1;
}

static int pdfv_floats_key(const char *seg, size_t segLen, const char *key,
                           float *out, int maxN)
{
    size_t p;
    if (!pdfv_key_pos(seg, segLen, key, &p)) return 0;
    /* skip to '[' if present */
    while (p < segLen && (seg[p] == ' ' || seg[p] == '\n' || seg[p] == '\r'))
        p++;
    if (p < segLen && seg[p] == '[') p++;
    int n = 0;
    while (n < maxN && p < segLen)
    {
        char *end = NULL;
        double v = strtod(seg + p, &end);
        if (end == seg + p) break;
        out[n++] = (float)v;
        p = (size_t)(end - seg);
    }
    return n;
}

/* ── Shading paint (axial type 2; radial falls back to mean color) ────── */

static void pdfv_paint_shading(uint8_t *gray, int W, int H, const PdfGs *gs,
                               const uint8_t *data, size_t len,
                               const char *name)
{
    /* name → object ref: "/sh5 5 0 R" in the Resources dict */
    size_t nl = strlen(name);
    int objNum = -1;
    for (size_t i = 0; i + nl + 8 < len; i++)
    {
        if (memcmp(data + i, name, nl) == 0 && data[i + nl] == ' ')
        {
            char *end = NULL;
            long n = strtol((const char *)data + i + nl + 1, &end, 10);
            if (end && end != (const char *)data + i + nl + 1)
            {
                const char *q = end;
                while (*q == ' ') q++;
                if (q[0] == '0' && q[1] == ' ' && q[2] == 'R')
                {
                    objNum = (int)n;
                    break;
                }
            }
        }
    }
    if (objNum < 0) return;

    /* find "<N> 0 obj" → dict segment until endobj/stream */
    char pat[32];
    int pl = 0;
    pl += snprintf(pat, sizeof pat, "%d 0 obj", objNum);
    const void *hit = memstr_local(data, len, pat);
    if (!hit) return;
    size_t off = (size_t)((const uint8_t *)hit - data) + (size_t)pl;
    size_t segEnd = off;
    while (segEnd + 6 < len && memcmp(data + segEnd, "endobj", 6) != 0 &&
           memcmp(data + segEnd, "stream", 6) != 0)
        segEnd++;
    if (segEnd <= off) return;
    const char *seg = (const char *)data + off;
    size_t segLen = segEnd - off;

    int stype = 2;
    pdfv_int_key(seg, segLen, "ShadingType", &stype);
    float coords[6] = {0, 0, 0, 0, 0, 0};
    int nCoords = pdfv_floats_key(seg, segLen, "Coords", coords, 6);
    float c0[3] = {0, 0, 0}, c1[3] = {1, 1, 1};
    pdfv_floats_key(seg, segLen, "C0", c0, 3);
    pdfv_floats_key(seg, segLen, "C1", c1, 3);
    float nExp = 1.0f;
    size_t fp;
    if (pdfv_key_pos(seg, segLen, "N", &fp))
    {
        char *end = NULL;
        double v = strtod(seg + fp, &end);
        if (end != seg + fp) nExp = (float)v;
    }
    (void)nCoords;

    /* inverse CTM: device px → user space at sh time */
    PdfM m = gs->ctm;
    float det = m.a * m.d - m.b * m.c;
    if (det < 0.0001f && det > -0.0001f) return;
    PdfM inv;
    inv.a = m.d / det;
    inv.b = -m.b / det;
    inv.c = -m.c / det;
    inv.d = m.a / det;
    inv.e = (m.c * m.f - m.d * m.e) / det;
    inv.f = (m.b * m.e - m.a * m.f) / det;

    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (gs->hasClip)
    {
        x0 = gs->cx0; y0 = gs->cy0; x1 = gs->cx1; y1 = gs->cy1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
    }
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            float ux, uy;
            pdfv_apply(inv, (float)x + 0.5f, (float)y + 0.5f, &ux, &uy);
            float t;
            if (stype == 2 && nCoords >= 4)
            {
                float dx = coords[2] - coords[0], dy = coords[3] - coords[1];
                float den = dx * dx + dy * dy;
                if (den <= 0.0001f) continue;
                t = ((ux - coords[0]) * dx + (uy - coords[1]) * dy) / den;
            }
            else
            {
                continue; /* only axial implemented */
            }
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float tp = 1.0f;
            int ipow = (int)nExp;
            if (ipow < 0) ipow = 0;
            if (ipow > 8) ipow = 8;
            for (int k = 0; k < ipow; k++) tp *= t;
            float r = c0[0] + tp * (c1[0] - c0[0]);
            float g = c0[1] + tp * (c1[1] - c0[1]);
            float b = c0[2] + tp * (c1[2] - c0[2]);
            int rr = (int)(r * 255.0f + 0.5f);
            int gg = (int)(g * 255.0f + 0.5f);
            int bb = (int)(b * 255.0f + 0.5f);
            if (rr < 0) rr = 0;
            if (rr > 255) rr = 255;
            if (gg < 0) gg = 0;
            if (gg > 255) gg = 255;
            if (bb < 0) bb = 0;
            if (bb > 255) bb = 255;
            gray[(size_t)y * (size_t)W + (size_t)x] =
                (uint8_t)dither_rgb_to_gray(rr, gg, bb);
        }
    }
}

/* ── Content-stream interpreter ───────────────────────────────────────── */

typedef struct
{
    uint8_t *gray;
    int W, H;
    PdfGs *gs;
    PdfSub *subs;
    int nSubs;
    int cur; /* index of open subpath, -1 = none */
    float lastX, lastY; /* current point, user space */
    int pendingClip; /* W* seen; commit on n */
} PdfInterp;

static void pdfv_clear_path(PdfInterp *st)
{
    st->nSubs = 0;
    st->cur = -1;
    st->pendingClip = 0;
}

static void pdfv_add_pt(PdfInterp *st, float x, float y)
{
    if (st->cur < 0)
    {
        if (st->nSubs >= PDFV_MAX_SUBS) return;
        st->cur = st->nSubs++;
        st->subs[st->cur].n = 0;
        st->subs[st->cur].closed = 0;
    }
    PdfSub *s = &st->subs[st->cur];
    if (s->n >= PDFV_MAX_PTS) return;
    float dx, dy;
    pdfv_apply(st->gs->ctm, x, y, &dx, &dy);
    s->px[s->n] = dx;
    s->py[s->n] = dy;
    s->n++;
}

static void pdfv_path_bbox(const PdfSub *subs, int nSubs, int *x0, int *y0,
                           int *x1, int *y1)
{
    float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
    for (int i = 0; i < nSubs; i++)
        for (int j = 0; j < subs[i].n; j++)
        {
            if (subs[i].px[j] < xmin) xmin = subs[i].px[j];
            if (subs[i].px[j] > xmax) xmax = subs[i].px[j];
            if (subs[i].py[j] < ymin) ymin = subs[i].py[j];
            if (subs[i].py[j] > ymax) ymax = subs[i].py[j];
        }
    if (xmin > xmax)
    {
        *x0 = *y0 = *x1 = *y1 = 0;
        return;
    }
    *x0 = (int)xmin;
    *y0 = (int)ymin;
    *x1 = (int)xmax + 1;
    *y1 = (int)ymax + 1;
}

static uint8_t pdfv_fill_val(const PdfGs *gs)
{
    int r = (int)(gs->fr * 255.0f + 0.5f);
    int g = (int)(gs->fg * 255.0f + 0.5f);
    int b = (int)(gs->fb * 255.0f + 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint8_t)dither_rgb_to_gray(r, g, b);
}

static uint8_t pdfv_stroke_val(const PdfGs *gs)
{
    PdfGs tmp = *gs;
    tmp.fr = gs->sr;
    tmp.fg = gs->sg;
    tmp.fb = gs->sb;
    return pdfv_fill_val(&tmp);
}

static void pdfv_run(const uint8_t *cs, size_t csLen, uint8_t *gray, int W,
                     int H, PdfM pageM, const uint8_t *data, size_t len)
{
    PdfGs stack[16];
    int sp = 0;
    PdfGs gs;
    gs.ctm = pageM;
    gs.fr = gs.fg = gs.fb = 0.0f;
    gs.sr = gs.sg = gs.sb = 0.0f;
    gs.lw = 1.0f;
    gs.cx0 = gs.cy0 = 0;
    gs.cx1 = W;
    gs.cy1 = H;
    gs.hasClip = 0;

    static PdfSub subs[PDFV_MAX_SUBS];
    PdfInterp st;
    st.gray = gray;
    st.W = W;
    st.H = H;
    st.gs = &gs;
    st.subs = subs;
    st.nSubs = 0;
    st.cur = -1;
    st.lastX = st.lastY = 0.0f;
    st.pendingClip = 0;

    float nums[8];
    int nn = 0;
    char lastName[32] = "";
    char tok[64];
    size_t i = 0;

    while (i < csLen)
    {
        /* skip whitespace + comments */
        while (i < csLen)
        {
            char c = (char)cs[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == 0)
                i++;
            else if (c == '%')
                while (i < csLen && cs[i] != '\n') i++;
            else
                break;
        }
        if (i >= csLen) break;

        /* token */
        size_t tl = 0;
        char first = (char)cs[i];
        if (first == '(')
        {
            i++; /* skip string literal */
            int depth = 1;
            while (i < csLen && depth > 0)
            {
                if (cs[i] == '\\' && i + 1 < csLen)
                    i += 2;
                else
                {
                    if (cs[i] == '(') depth++;
                    else if (cs[i] == ')') depth--;
                    i++;
                }
            }
            continue;
        }
        if (first == '[' || first == ']')
        {
            i++;
            continue;
        }
        if (first == '<')
        {
            if (i + 1 < csLen && cs[i + 1] == '<')
                i += 2;
            else /* hex string */
            {
                i++;
                while (i < csLen && cs[i] != '>') i++;
                if (i < csLen) i++;
            }
            continue;
        }
        while (i < csLen && tl + 1 < (int)sizeof(tok))
        {
            char c = (char)cs[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
            tok[tl++] = c;
            i++;
        }
        tok[tl] = 0;
        if (tl == 0)
        {
            i++;
            continue;
        }

        /* number operand? */
        {
            char *end = NULL;
            double v = strtod(tok, &end);
            if (end && end != tok && *end == 0)
            {
                if (nn < 8)
                    nums[nn++] = (float)v;
                else
                {
                    nums[0] = nums[1];
                    nums[1] = nums[2];
                    nums[2] = nums[3];
                    nums[3] = nums[4];
                    nums[4] = nums[5];
                    nums[5] = nums[6];
                    nums[6] = nums[7];
                    nums[7] = (float)v;
                }
                continue;
            }
        }

        if (tok[0] == '/')
        {
            strncpy(lastName, tok, sizeof(lastName) - 1);
            lastName[sizeof(lastName) - 1] = 0;
            continue;
        }

        /* operator dispatch */
        if (strcmp(tok, "q") == 0)
        {
            if (sp < 16) stack[sp++] = gs;
        }
        else if (strcmp(tok, "Q") == 0)
        {
            if (sp > 0) gs = stack[--sp];
        }
        else if (strcmp(tok, "cm") == 0 && nn >= 6)
        {
            PdfM m;
            m.a = nums[nn - 6];
            m.b = nums[nn - 5];
            m.c = nums[nn - 4];
            m.d = nums[nn - 3];
            m.e = nums[nn - 2];
            m.f = nums[nn - 1];
            gs.ctm = pdfv_mul(m, gs.ctm);
        }
        else if (strcmp(tok, "rg") == 0 && nn >= 3)
        {
            gs.fr = nums[nn - 3];
            gs.fg = nums[nn - 2];
            gs.fb = nums[nn - 1];
        }
        else if (strcmp(tok, "RG") == 0 && nn >= 3)
        {
            gs.sr = nums[nn - 3];
            gs.sg = nums[nn - 2];
            gs.sb = nums[nn - 1];
        }
        else if (strcmp(tok, "g") == 0 && nn >= 1)
            gs.fr = gs.fg = gs.fb = nums[nn - 1];
        else if (strcmp(tok, "G") == 0 && nn >= 1)
            gs.sr = gs.sg = gs.sb = nums[nn - 1];
        else if (strcmp(tok, "w") == 0 && nn >= 1)
            gs.lw = nums[nn - 1];
        else if (strcmp(tok, "m") == 0 && nn >= 2)
        {
            st.cur = -1; /* start a new subpath */
            pdfv_add_pt(&st, nums[nn - 2], nums[nn - 1]);
            st.lastX = nums[nn - 2];
            st.lastY = nums[nn - 1];
        }
        else if (strcmp(tok, "l") == 0 && nn >= 2)
        {
            pdfv_add_pt(&st, nums[nn - 2], nums[nn - 1]);
            st.lastX = nums[nn - 2];
            st.lastY = nums[nn - 1];
        }
        else if (strcmp(tok, "c") == 0 && nn >= 6 && st.cur >= 0)
        {
            /* cubic bezier from (lastX,lastY) via c1 c2 to (x3,y3),
             * flattened in device space (affine-safe) */
            float lx, ly;
            pdfv_apply(gs.ctm, st.lastX, st.lastY, &lx, &ly);
            float x1, y1, x2, y2, x3, y3;
            pdfv_apply(gs.ctm, nums[nn - 6], nums[nn - 5], &x1, &y1);
            pdfv_apply(gs.ctm, nums[nn - 4], nums[nn - 3], &x2, &y2);
            pdfv_apply(gs.ctm, nums[nn - 2], nums[nn - 1], &x3, &y3);
            PdfSub *s = &st.subs[st.cur];
            const int SEG = 8;
            for (int k = 1; k <= SEG; k++)
            {
                float t = (float)k / (float)SEG;
                float mt = 1.0f - t;
                float bx = mt * mt * mt * lx + 3 * mt * mt * t * x1 +
                           3 * mt * t * t * x2 + t * t * t * x3;
                float by = mt * mt * mt * ly + 3 * mt * mt * t * y1 +
                           3 * mt * t * t * y2 + t * t * t * y3;
                if (s->n < PDFV_MAX_PTS)
                {
                    s->px[s->n] = bx;
                    s->py[s->n] = by;
                    s->n++;
                }
            }
            st.lastX = nums[nn - 2];
            st.lastY = nums[nn - 1];
        }
        else if (strcmp(tok, "v") == 0 && nn >= 4 && st.cur >= 0)
        {
            /* cubic with first control = current point */
            st.lastX = nums[nn - 2];
            st.lastY = nums[nn - 1];
            pdfv_add_pt(&st, nums[nn - 2], nums[nn - 1]);
        }
        else if (strcmp(tok, "y") == 0 && nn >= 4 && st.cur >= 0)
        {
            st.lastX = nums[nn - 2];
            st.lastY = nums[nn - 1];
            pdfv_add_pt(&st, nums[nn - 2], nums[nn - 1]);
        }
        else if (strcmp(tok, "h") == 0)
        {
            if (st.cur >= 0) st.subs[st.cur].closed = 1;
        }
        else if (strcmp(tok, "re") == 0 && nn >= 4)
        {
            float x = nums[nn - 4], y = nums[nn - 3];
            float rw = nums[nn - 2], rh = nums[nn - 1];
            if (rw < 0) { x += rw; rw = -rw; }
            if (rh < 0) { y += rh; rh = -rh; }
            st.cur = -1;
            pdfv_add_pt(&st, x, y);
            pdfv_add_pt(&st, x + rw, y);
            pdfv_add_pt(&st, x + rw, y + rh);
            pdfv_add_pt(&st, x, y + rh);
            if (st.cur >= 0) st.subs[st.cur].closed = 1;
        }
        else if (strcmp(tok, "W") == 0 || strcmp(tok, "W*") == 0)
        {
            st.pendingClip = 1;
        }
        else if (strcmp(tok, "n") == 0)
        {
            if (st.pendingClip && st.nSubs > 0)
            {
                int bx0, by0, bx1, by1;
                pdfv_path_bbox(st.subs, st.nSubs, &bx0, &by0, &bx1, &by1);
                gs.cx0 = bx0;
                gs.cy0 = by0;
                gs.cx1 = bx1;
                gs.cy1 = by1;
                gs.hasClip = 1;
            }
            pdfv_clear_path(&st);
        }
        else if (strcmp(tok, "f") == 0 || strcmp(tok, "F") == 0 ||
                 strcmp(tok, "f*") == 0 || strcmp(tok, "f*1") == 0)
        {
            pdfv_fill(gray, W, H, st.subs, st.nSubs, pdfv_fill_val(&gs), &gs);
            pdfv_clear_path(&st);
        }
        else if (strcmp(tok, "B") == 0 || strcmp(tok, "B*") == 0 ||
                 strcmp(tok, "b") == 0 || strcmp(tok, "b*") == 0)
        {
            pdfv_fill(gray, W, H, st.subs, st.nSubs, pdfv_fill_val(&gs), &gs);
            pdfv_stroke(gray, W, H, st.subs, st.nSubs,
                        pdfv_stroke_val(&gs), &gs);
            pdfv_clear_path(&st);
        }
        else if (strcmp(tok, "S") == 0)
        {
            pdfv_stroke(gray, W, H, st.subs, st.nSubs,
                        pdfv_stroke_val(&gs), &gs);
            pdfv_clear_path(&st);
        }
        else if (strcmp(tok, "s") == 0)
        {
            if (st.cur >= 0) st.subs[st.cur].closed = 1;
            pdfv_stroke(gray, W, H, st.subs, st.nSubs,
                        pdfv_stroke_val(&gs), &gs);
            pdfv_clear_path(&st);
        }
        else if (strcmp(tok, "sh") == 0 && lastName[0])
        {
            pdfv_paint_shading(gray, W, H, &gs, data, len, lastName);
        }
        /* gs, d, J, j, M, ri, i, Tr…: no observable effect on our 1-bit
         * rendering — silently ignored. */

        nn = 0;
    }
}

/* ── Vector entry: MediaBox + content stream discovery → gray → bitmap ── */

typedef struct
{
    int W, H;
    const uint8_t *gray;
} PdfVecPix;

static uint8_t pdfv_pixel_cb(void *ud, int x, int y)
{
    PdfVecPix *p = (PdfVecPix *)ud;
    return p->gray[(size_t)y * (size_t)p->W + (size_t)x];
}

static LCDBitmap *pdfvec_render(const uint8_t *data, size_t len)
{
    /* MediaBox (default letter) */
    float mb[4] = {0, 0, 612, 792};
    {
        const void *h = memstr_local(data, len, "/MediaBox");
        if (h)
        {
            size_t off = (size_t)((const uint8_t *)h - data);
            size_t segLen = len - off;
            if (segLen > 96) segLen = 96;
            pdfv_floats_key((const char *)h, segLen, "MediaBox", mb, 4);
        }
    }
    float pw = mb[2] - mb[0], ph = mb[3] - mb[1];
    if (pw <= 0 || ph <= 0) return NULL;

    /* find the page content stream: first stream that inflates (or reads)
     * as text containing path operators */
    uint8_t *cs = NULL;
    int csOwned = 0;
    size_t csLen = 0, from = 0;
    while (!cs)
    {
        size_t sLen = 0;
        const uint8_t *sd = pdf_stream_data(data, len, from, &sLen);
        if (!sd) break;
        from = (size_t)(sd - data) + sLen + 9;
        uint8_t *inf = NULL;
        size_t infLen = 0;
        if (sLen >= 2 && sd[0] == 0x78)
            inf = inflate_decompress(sd, sLen, &infLen);
        const uint8_t *cand = inf ? inf : sd;
        size_t candLen = inf ? infLen : sLen;
        if (candLen > 16 &&
            (memstr_local(cand, candLen, " re") ||
             memstr_local(cand, candLen, " f*") ||
             memstr_local(cand, candLen, " S")))
        {
            if (inf)
            {
                cs = inf;
                csOwned = 1;
            }
            else
            {
                cs = (uint8_t *)malloc(candLen);
                if (cs)
                {
                    memcpy(cs, cand, candLen);
                    csOwned = 1;
                }
            }
            csLen = candLen;
        }
        else
        {
            free(inf);
        }
    }
    if (!cs) return NULL;

    /* fit into 360x200, never upscale */
    float scale = 360.0f / pw;
    if (200.0f / ph < scale) scale = 200.0f / ph;
    if (scale > 1.0f) scale = 1.0f;
    int W = (int)(pw * scale);
    int H = (int)(ph * scale);
    if (W < 1) W = 1;
    if (H < 1) H = 1;

    uint8_t *gray = (uint8_t *)malloc((size_t)W * (size_t)H);
    if (!gray)
    {
        if (csOwned) free(cs);
        return NULL;
    }
    memset(gray, 255, (size_t)W * (size_t)H);

    PdfM pageM = pdfv_page_matrix(mb[0], mb[1], mb[2], mb[3], W, H);
    pdfv_run(cs, csLen, gray, W, H, pageM, data, len);
    if (csOwned) free(cs);

    PdfVecPix pp;
    pp.W = W;
    pp.H = H;
    pp.gray = gray;
    LCDBitmap *img = dither_to_bitmap(W, H, pdfv_pixel_cb, &pp);
    free(gray);
    if (img) logger_log("PDF ok (vector) %dx%d", W, H);
    return img;
}

LCDBitmap *pdfimg_decode(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 64) return NULL;
    if (memcmp(data, "%PDF-", 5) != 0) return NULL;

    /* Find every "/Subtype /Image" occurrence (raw scan; PDF may not be
     * fully parseable without a real xref implementation). */
    for (size_t i = 0; i + 15 < len; i++)
    {
        if (memcmp(data + i, "/Subtype", 8) == 0)
        {
            size_t j = i + 8;
            while (j < len && (data[j] == ' ' || data[j] == '\n' ||
                               data[j] == '\r'))
                j++;
            if (j + 5 >= len || memcmp(data + j, "/Image", 6) != 0)
                continue;

            /* Dict extends backwards to "<<" and forwards to ">>". */
            size_t dictStart = i;
            int depth = 0;
            while (dictStart > 0)
            {
                if (data[dictStart] == '>' && data[dictStart - 1] == '>')
                    depth++;
                if (data[dictStart] == '<' && data[dictStart - 1] == '<')
                {
                    if (depth == 0) break;
                    depth--;
                }
                dictStart--;
            }
            size_t dictEnd = i;
            while (dictEnd + 1 < len &&
                   !(data[dictEnd] == '>' && data[dictEnd + 1] == '>'))
                dictEnd++;
            dictEnd += 2;
            if (dictEnd > len) dictEnd = len;
            size_t segLen = dictEnd - dictStart;
            const char *seg = (const char *)data + dictStart;

            int w = 0, h = 0;
            if (!pdf_dict_value(seg, segLen, "/Width", &w)) continue;
            if (!pdf_dict_value(seg, segLen, "/Height", &h)) continue;
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;

            int comps = 3;
            if (memstr_local(seg, segLen, "/DeviceGray"))
                comps = 1;

            size_t sLen = 0;
            const uint8_t *sdata =
                pdf_stream_data(data, len, dictEnd, &sLen);
            if (!sdata || sLen == 0) continue;

            int isDCT = memstr_local(seg, segLen, "/DCTDecode") != NULL;
            int isFlate = memstr_local(seg, segLen, "/FlateDecode") != NULL;
            int isAscii = memstr_local(seg, segLen, "/ASCIIHexDecode") != NULL ||
                          memstr_local(seg, segLen, "/ASCII85Decode") != NULL;

            uint8_t *gray = (uint8_t *)malloc((size_t)w * (size_t)h);
            if (!gray) return NULL;
            LCDBitmap *img = NULL;

            if (isDCT)
            {
                /* JPEG bytes inside the stream (possible filter chain
                 * DCTDecode alone). Decode with jpeg_decode then pull the
                 * luminance via getBitmapData (1-bit) — better: use our own
                 * jpeg_decode_gray seam if available through the bitmap. */
                LCDBitmap *b = jpeg_decode(sdata, sLen, 360, 200);
                if (b)
                {
                    /* Repack through the dithered bitmap directly. */
                    free(gray);
                    logger_log("PDF(jpeg) ok");
                    return b;
                }
                free(gray);
                continue;
            }

            const uint8_t *pixBytes = sdata;
            size_t pixLen = sLen;
            uint8_t *inflated = NULL;
            if (isFlate)
            {
                inflated = inflate_decompress(sdata, sLen, &pixLen);
                if (inflated && pixLen >= (size_t)w * (size_t)h * (size_t)comps)
                {
                    pixBytes = inflated;
                }
                else
                {
                    free(inflated);
                    free(gray);
                    continue;
                }
            }
            else if (isAscii)
            {
                free(gray);
                continue; /* rare; unsupported filter chain */
            }

            size_t need = (size_t)w * (size_t)h * (size_t)comps;
            if (pixLen < need)
            {
                free(inflated);
                free(gray);
                continue; /* truncated image data */
            }
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    size_t idx = (size_t)y * (size_t)w + (size_t)x;
                    const uint8_t *p = pixBytes + idx * (size_t)comps;
                    gray[idx] = comps == 1
                                    ? p[0]
                                    : (uint8_t)dither_rgb_to_gray(p[0], p[1],
                                                                  p[2]);
                }
            }
            free(inflated);

            int scaleNum = 1, scaleDen = 1;
            int targetW = w, targetH = h;
            if (w > 360 || h > 200)
            {
                if ((long long)w * 200 >= (long long)h * 360)
                {
                    scaleNum = w; scaleDen = 360;
                }
                else
                {
                    scaleNum = h; scaleDen = 200;
                }
                targetW = (int)((long long)w * scaleDen / scaleNum);
                if (targetW < 1) targetW = 1;
                targetH = (int)((long long)h * scaleDen / scaleNum);
                if (targetH < 1) targetH = 1;
            }

            PdfPix pp;
            pp.data = data;
            pp.len = len;
            pp.scaleNum = scaleNum;
            pp.scaleDen = scaleDen;
            pp.srcW = w;
            pp.srcH = h;
            pp.srcComponents = comps;
            pp.gray = gray;
            img = dither_to_bitmap(targetW, targetH, pdf_pixel_gray, &pp);
            free(gray);
            if (img)
            {
                int iw = 0, ih = 0;
                logger_log("PDF ok (embedded raster) %dx%d", w, h);
            }
            return img;
        }
    }
    /* No embedded raster — render as vector if it has a content stream. */
    return pdfvec_render(data, len);
}
