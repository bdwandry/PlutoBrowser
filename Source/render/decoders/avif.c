/*
 * PlutoBrowser — avif.c
 * AVIF still decoder over vendored dav1d 1.5.1 (see avif.h).
 *
 * Pipeline:
 *   ftyp brand check → meta walk (pitm primary id, iinf av01 id, iloc
 *   extents for the primary item) → concat extents → dav1d (n_threads=1,
 *   max_frame_delay=1, grain off, frame_size_limit cap) → YUV→gray rows →
 *   dither_to_bitmap (360x200 convention).
 *
 * dav1d diagnostics are silenced (noop logger); all outcomes are logged
 * by us as [avif] lines. dav1d owns nothing: the payload buffer is ours
 * (noop free callback), the picture is unref'd before return.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "avif.h"
#include "dither.h"
#include "dav1d/include/dav1d/dav1d.h"
#include "../../core/logger.h"

/* Decoded frames are capped here (pixels): 1600x1600 YUV420 peaks around
 * 4MB transient inside dav1d — the most a single-flight decode may claim
 * before the device envelope is at risk. Oversize → clean dav1d refusal. */
#define AVIF_MAX_PIXELS (1600U * 1600U)
/* Container sanity: refuse absurd box/item counts from hostile files. */
#define AVIF_MAX_EXTENTS 16
#define AVIF_MAX_ITEMS 64

/* ── bit readers ─────────────────────────────────────────────────────── */

static uint32_t avif_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t avif_rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint64_t avif_rdN(const uint8_t *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

/* ── meta box walk ───────────────────────────────────────────────────── */

typedef struct
{
    uint32_t primary_id; /* pitm */
    uint32_t av01_id;    /* first 'av01' item in iinf */
    int av01_count;      /* grids have several: refuse */
    size_t ext_off[AVIF_MAX_EXTENTS];
    size_t ext_len[AVIF_MAX_EXTENTS];
    int ext_count;
    uint32_t want_id;    /* item whose extents iloc collects */
    int collecting;
} AvifMeta;

typedef void (*AvifBoxFn)(uint32_t type, const uint8_t *body, size_t bodyLen,
                          void *ctx);

static void avif_walk(const uint8_t *base, size_t len, AvifBoxFn fn, void *ctx)
{
    size_t pos = 0;
    while (pos + 8 <= len)
    {
        uint64_t blen = avif_rd32(base + pos);
        uint32_t type;
        size_t hdr = 8;
        if (blen == 1)
        {
            if (pos + 16 > len)
                return;
            blen = ((uint64_t)avif_rd32(base + pos + 8) << 32) |
                   avif_rd32(base + pos + 12);
            hdr = 16;
        }
        else if (blen == 0)
            blen = len - pos;
        type = avif_rd32(base + pos + 4);
        if (blen < hdr || pos + (size_t)blen > len)
            return;
        fn(type, base + pos + hdr, (size_t)blen - hdr, ctx);
        pos += (size_t)blen;
        if (pos > len)
            return;
    }
}

static void avif_meta_child(uint32_t type, const uint8_t *b, size_t n,
                            void *ctx)
{
    AvifMeta *m = (AvifMeta *)ctx;
    if (type == 0x7069746d) /* pitm */
    {
        int ver;
        if (n < 6)
            return;
        ver = b[0];
        m->primary_id = (ver == 0) ? avif_rd16(b + 4) : avif_rd32(b + 4);
    }
    else if (type == 0x69696e66) /* iinf */
    {
        int ver, i;
        const uint8_t *p;
        uint32_t count;
        /* Pass 2 re-walks for iloc extents only — count items once. */
        if (m->collecting || n < 6)
            return;
        ver = b[0];
        p = b + 4;
        count = (ver == 0) ? avif_rd16(p) : avif_rd32(p);
        p += (ver == 0) ? 2 : 4;
        if (count > AVIF_MAX_ITEMS)
            return;
        for (i = 0; (uint32_t)i < count; i++)
        {
            uint32_t ebl, id = 0;
            int ever;
            const uint8_t *q;
            if (p + 12 > b + n)
                return;
            ebl = avif_rd32(p);
            ever = p[8];
            q = p + 12;
            if (ever == 2 || ever == 3)
            {
                if (q + 4 > b + n)
                    return;
                id = avif_rd16(q);
                q += 4; /* id + protection index */
            }
            if (q + 4 <= b + n && memcmp(q, "av01", 4) == 0)
            {
                m->av01_count++;
                if (m->av01_id == 0)
                    m->av01_id = id;
            }
            if (ebl < 12 || p + ebl > b + n)
                return;
            p += ebl;
        }
    }
    else if (type == 0x696c6f63) /* iloc */
    {
        int ver, off_sz, len_sz, base_sz, idx_sz = 0, e;
        const uint8_t *p;
        uint32_t count, i;
        if (!m->collecting || n < 8)
            return;
        ver = b[0];
        p = b + 4;
        off_sz = (*p >> 4) & 0xF;
        len_sz = *p & 0xF;
        base_sz = (*(p + 1) >> 4) & 0xF;
        p += 2;
        if (ver == 1 || ver == 2)
        {
            idx_sz = *p & 0xF;
            p += 2;
        }
        if (off_sz > 8 || len_sz > 8 || base_sz > 8 || idx_sz > 8)
            return;
        count = (ver < 2) ? avif_rd16(p) : avif_rd32(p);
        p += (ver < 2) ? 2 : 4;
        if (count > AVIF_MAX_ITEMS)
            return;
        for (i = 0; i < count; i++)
        {
            uint32_t id;
            uint16_t method = 0, exts;
            uint64_t base;
            if (ver < 2)
            {
                if (p + 8 > b + n)
                    return;
                id = avif_rd16(p);
                p += 2;
            }
            else
            {
                if (p + 12 > b + n)
                    return;
                id = avif_rd32(p);
                p += 4;
                method = avif_rd16(p); /* construction_method */
                p += 2;
            }
            if (p + 2 > b + n)
                return;
            p += 2; /* data_reference_index */
            if (p + base_sz + 2 > b + n)
                return;
            base = avif_rdN(p, base_sz);
            p += base_sz;
            exts = avif_rd16(p);
            p += 2;
            for (e = 0; e < exts; e++)
            {
                uint64_t eo, el;
                if (p + idx_sz + off_sz + len_sz > b + n)
                    return;
                p += idx_sz;
                eo = avif_rdN(p, off_sz);
                p += off_sz;
                el = avif_rdN(p, len_sz);
                p += len_sz;
                /* method 0 = file offsets. idat (1) and item (2)
                 * offsets are a different address space: refuse. */
                if (id == m->want_id && method == 0 &&
                    m->ext_count < AVIF_MAX_EXTENTS)
                {
                    m->ext_off[m->ext_count] = (size_t)(base + eo);
                    m->ext_len[m->ext_count] = (size_t)el;
                    m->ext_count++;
                }
            }
        }
    }
}

static void avif_top_box(uint32_t type, const uint8_t *b, size_t n, void *ctx)
{
    AvifMeta *m = (AvifMeta *)ctx;
    if (type == 0x6d657461) /* meta: skip 4B fullbox header, two passes */
    {
        if (n < 4)
            return;
        avif_walk(b + 4, n - 4, avif_meta_child, ctx); /* ids */
        m->want_id = m->primary_id ? m->primary_id : m->av01_id;
        m->collecting = 1;
        avif_walk(b + 4, n - 4, avif_meta_child, ctx); /* extents */
    }
}

/* ── dav1d glue ──────────────────────────────────────────────────────── */

static void avif_noop_free(const uint8_t *data, void *cookie)
{
    (void)data;
    (void)cookie;
}

/* ── Picture bump arena ───────────────────────────────────────────────
 * dav1d's default picture allocator mallocs multi-hundred-KB frame
 * buffers per picture; under device heap pressure (multi-MB pages) those
 * large first-fit allocs stall for seconds (measured: same 640px AVIF
 * decodes in ~6s lean vs 10s+ pressured, tripping the run-loop
 * watchdog). This arena serves picture buffers O(1) from one chunk,
 * reset per decode (single-flight queue: at most one decode owns it).
 * Layout math mirrors dav1d_default_picture_alloc exactly (128-px
 * alignment, 1024-stride padding, shared chroma stride). Release is a
 * noop; the arena resets at each decode start. malloc failure (or an
 * oversize picture) returns ENOMEM: clean failure → placeholder card,
 * never a stall. Upstream dav1d untouched. */
#define AVIF_ARENA_SIZE (4 * 1024 * 1024)
static uint8_t *g_picArena = NULL;
static size_t g_picArenaOff = 0;

static int avif_pic_alloc(Dav1dPicture *pic, void *cookie)
{
    int hbd, aligned_w, aligned_h, has_chroma, ss_ver, ss_hor;
    ptrdiff_t y_stride, uv_stride;
    size_t y_sz, uv_sz, need, off;
    uint8_t *base;
    (void)cookie;
    if (!pic)
    {
        return DAV1D_ERR(EINVAL);
    }
    hbd = pic->p.bpc > 8;
    aligned_w = (pic->p.w + 127) & ~127;
    aligned_h = (pic->p.h + 127) & ~127;
    has_chroma = pic->p.layout != DAV1D_PIXEL_LAYOUT_I400;
    ss_ver = pic->p.layout == DAV1D_PIXEL_LAYOUT_I420;
    ss_hor = pic->p.layout != DAV1D_PIXEL_LAYOUT_I444;
    y_stride = aligned_w << hbd;
    uv_stride = has_chroma ? y_stride >> ss_hor : 0;
    if (!(y_stride & 1023))
    {
        y_stride += DAV1D_PICTURE_ALIGNMENT;
    }
    if (!(uv_stride & 1023) && has_chroma)
    {
        uv_stride += DAV1D_PICTURE_ALIGNMENT;
    }
    /* Overflow-safe size math (hostile headers reach this callback). */
    y_sz = (size_t)y_stride * (size_t)aligned_h;
    uv_sz = (size_t)uv_stride * (size_t)(aligned_h >> ss_ver);
    if (y_stride <= 0 || (has_chroma && uv_stride <= 0) ||
        y_sz / (size_t)(aligned_h ? aligned_h : 1) != (size_t)y_stride ||
        (has_chroma &&
         uv_sz / (size_t)((aligned_h >> ss_ver) ? (aligned_h >> ss_ver) : 1) !=
             (size_t)uv_stride))
    {
        return DAV1D_ERR(EINVAL);
    }
    need = y_sz + 2 * uv_sz;
    if (need < y_sz || need + DAV1D_PICTURE_ALIGNMENT < need)
    {
        return DAV1D_ERR(EINVAL);
    }
    need += DAV1D_PICTURE_ALIGNMENT;
    if (!g_picArena || g_picArenaOff + need < g_picArenaOff ||
        g_picArenaOff + need > AVIF_ARENA_SIZE)
    {
        return DAV1D_ERR(ENOMEM);
    }
    off = (g_picArenaOff + DAV1D_PICTURE_ALIGNMENT - 1) &
          ~(size_t)(DAV1D_PICTURE_ALIGNMENT - 1);
    /* TEMP: prove the arena path runs on device (else the default
     * allocator's fragmentation drag remains). */
    logger_log("[avif] arena alloc %zu bytes", need);
    if (off + need < off || off + need > AVIF_ARENA_SIZE)
    {
        return DAV1D_ERR(ENOMEM);
    }
    base = g_picArena + off;
    g_picArenaOff = off + need;
    pic->stride[0] = y_stride;
    pic->stride[1] = uv_stride;
    pic->data[0] = base;
    pic->data[1] = has_chroma ? base + y_sz : NULL;
    pic->data[2] = has_chroma ? base + y_sz + uv_sz : NULL;
    pic->allocator_data = NULL;
    return 0;
}

static void avif_pic_release(Dav1dPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    /* No-op: the arena resets per decode (single-flight). */
}

/* BT.601 full-range YUV→gray via integer math (matches dither's style:
 * (r*306 + g*601 + b*117) >> 10). Subsampled chroma is nearest-held. */
typedef struct
{
    uint8_t *rows;
    int width, height;
    int scaleNum, scaleDen; /* nearest-sample to 360x200 (tif.c style) */
} AvifPix;

static uint8_t avif_pixel_gray(void *userdata, int x, int y)
{
    AvifPix *p = (AvifPix *)userdata;
    int sx = (int)((long long)x * p->scaleNum / p->scaleDen);
    int sy = (int)((long long)y * p->scaleNum / p->scaleDen);
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if (sx >= p->width)
        sx = p->width - 1;
    if (sy >= p->height)
        sy = p->height - 1;
    return p->rows[(size_t)sy * p->width + sx];
}

#if defined(TARGET_PLAYDATE) && defined(__arm__)
/* ── Heap-backed scratch stack (fiber) ────────────────────────────────
 * The 61.8KB game task cannot hold dav1d's worst frames (24KB init,
 * 34KB restoration, 16KB transforms + call chain), and moving those
 * temps to static storage CORRUPTS pixels (bisected file-by-file: the
 * scratch carries state that must not persist). So the whole decode runs
 * on 64KB of heap with EXACT stack semantics (SP reset per call: nothing
 * persists, exactly like a bigger task stack). Upstream dav1d stays 100%
 * pristine. Simulator/host builds take the direct path (8MB stacks). */
#define AVIF_FIBER_STACK (64 * 1024)
#define AVIF_FIBER_CANARY 0xA5F1C3E7u

__asm__(
    ".syntax unified\n"
    ".thumb\n"
    ".global avif_fiber_call\n"
    ".type avif_fiber_call, %function\n"
    "avif_fiber_call:\n"
    "    push {r4-r6, lr}\n"
    "    mov r4, sp\n"
    "    mov r5, r1\n"
    "    mov sp, r0\n"
    "    mov r0, r2\n"
    "    mov r1, r3\n"
    "    blx r5\n"
    "    mov r6, r0\n"
    "    mov sp, r4\n"
    "    mov r0, r6\n"
    "    pop {r4-r6, pc}\n");

static void *avif_fiber_call(void *stackTop,
                             void *(*fn)(const uint8_t *, size_t),
                             const uint8_t *data, size_t len);
static uint8_t *g_fiberStack = NULL;
#endif

static LCDBitmap *avif_decode_inner(const uint8_t *data, size_t len)
{
    AvifMeta m;
    uint8_t *obu = NULL;
    size_t total = 0, w = 0;
    int i;
    Dav1dSettings s;
    Dav1dContext *c = NULL;
    Dav1dData dd;
    Dav1dPicture pic;
    uint8_t *rows = NULL;
    LCDBitmap *img = NULL;
    int have_pic = 0;

    logger_stack_touch();
    if (!data || len < 16)
        return NULL;
    logger_log("[avif] enter %zu bytes (peak=%u)", len, logger_stack_peak());
    /* ftyp box branded avif/avis (mif1 alone may be HEIC: not ours). */
    if (memcmp(data + 4, "ftyp", 4) != 0 ||
        (memcmp(data + 8, "avif", 4) != 0 &&
         memcmp(data + 8, "avis", 4) != 0))
        return NULL;

    memset(&m, 0, sizeof m);
    avif_walk(data, len, avif_top_box, &m);
    /* want_id = pitm primary (a grid primary has no mdat extents, so
     * grids refuse below via ext_count == 0 — no special-casing). */
    if (m.ext_count == 0 || m.want_id == 0)
    {
        logger_log("[avif] no primary av01 extents (items=%d)",
                   m.av01_count);
        return NULL;
    }
    for (i = 0; i < m.ext_count; i++)
    {
        if (m.ext_off[i] + m.ext_len[i] < m.ext_off[i] ||
            m.ext_off[i] + m.ext_len[i] > len || m.ext_len[i] == 0)
        {
            logger_log("[avif] hostile extent %d", i);
            return NULL;
        }
        total += m.ext_len[i];
    }
    obu = (uint8_t *)malloc(total);
    if (!obu)
        return NULL;
    for (i = 0; i < m.ext_count; i++)
    {
        memcpy(obu + w, data + m.ext_off[i], m.ext_len[i]);
        w += m.ext_len[i];
    }

    dav1d_default_settings(&s);
    s.n_threads = 1;        /* every thread primitive is gated on n_tc > 1 */
    s.max_frame_delay = 1;  /* stills: no frame threading either */
    s.apply_grain = 0;      /* skip film-grain synthesis (1-bit output) */
    s.frame_size_limit = AVIF_MAX_PIXELS;
    /* Watchdog survival: full AV1 decode of a 546K-px still exceeds the
     * 10s run-loop budget on the M7 (sampled in loopfilter, decode_b,
     * and idct across runs). Post-filters are ~1/3 of the work and the
     * least visible at thumbnail scale, so big inputs keep edge
     * deblocking only and skip CDEF + restoration. Small inputs (fast
     * either way) keep ALL for pixel-exactness. Threshold is input
     * bytes (proxy for frame area; dims aren't known pre-open). */
    s.inloop_filters = (total > 8192) ? DAV1D_INLOOPFILTER_DEBLOCK
                                      : DAV1D_INLOOPFILTER_ALL;
    logger_log("[avif] filters=%s (%zu bytes)",
               (total > 8192) ? "deblock-only" : "all", total);
    s.logger.callback = NULL; /* dav1d logging off; we log our own lines */
    /* Picture arena (see above): one malloc reused across decodes; on
     * failure the default allocator stays (slower under pressure, but
     * functional). Reset per decode: single-flight owns it. */
    if (!g_picArena)
    {
        g_picArena = (uint8_t *)malloc(AVIF_ARENA_SIZE);
        if (!g_picArena)
        {
            logger_log("[avif] no arena (using default allocator)");
        }
    }
    if (g_picArena)
    {
        g_picArenaOff = 0;
        s.allocator.cookie = NULL;
        s.allocator.alloc_picture_callback = avif_pic_alloc;
        s.allocator.release_picture_callback = avif_pic_release;
    }
    logger_log("[avif] container ok (peak=%u)", logger_stack_peak());
    if (dav1d_open(&c, &s) < 0)
    {
        logger_log("[avif] dav1d_open failed");
        goto done;
    }
    logger_log("[avif] open ok (peak=%u)", logger_stack_peak());
    if (dav1d_data_wrap(&dd, obu, total, avif_noop_free, NULL) < 0)
    {
        logger_log("[avif] data_wrap failed");
        goto done;
    }
    {
        int r = dav1d_send_data(c, &dd);
        if (r < 0 && r != DAV1D_ERR(EAGAIN))
        {
            logger_log("[avif] send_data %d", r);
            goto done;
        }
    }
    memset(&pic, 0, sizeof pic);
    {
        int r = dav1d_get_picture(c, &pic);
        if (r < 0)
        {
            logger_log("[avif] get_picture %d", r);
            goto done;
        }
    }
    have_pic = 1;
    logger_log("[avif] decoded %dx%d (peak=%u)", pic.p.w, pic.p.h,
               logger_stack_peak());
    if (pic.p.bpc != 8)
    {
        /* 8-bit-only DSP build: dav1d should have refused already; belt
         * and suspenders. */
        logger_log("[avif] refuse: bpc=%d", pic.p.bpc);
        goto done;
    }
    {
        int W = (int)pic.p.w, H = (int)pic.p.h;
        int mono = (pic.p.layout == DAV1D_PIXEL_LAYOUT_I400);
        int cw = mono ? W : (W >> (pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 ||
                                   pic.p.layout == DAV1D_PIXEL_LAYOUT_I422));
        int ch = mono ? H : (H >> (pic.p.layout == DAV1D_PIXEL_LAYOUT_I420));
        const uint8_t *Y = pic.data[0];
        const uint8_t *U = mono ? NULL : pic.data[1];
        const uint8_t *V = mono ? NULL : pic.data[2];
        ptrdiff_t ys = pic.stride[0], us = pic.stride[1];
        int scaleNum = 1, scaleDen = 1;
        int targetW = W, targetH = H;
        AvifPix p;
        if (W < 1 || H < 1 || W > 1600 || H > 1600 || !Y ||
            (!mono && (!U || !V || cw < 1 || ch < 1)))
        {
            logger_log("[avif] hostile dims %dx%d", W, H);
            goto done;
        }
        rows = (uint8_t *)malloc((size_t)W * H);
        if (!rows)
            goto done;
        /* Coordinate maps (exact same math as the inline version below,
         * hoisted out of the hot loop: integer division on the M7 has no
         * hardware support, and 2 divs × 546K pixels dominated conversion
         * time). NULL map = fall back to inline math (always correct). */
        {
            int *xmap = (int *)malloc((size_t)W * sizeof(int));
            int *ymap = (int *)malloc((size_t)H * sizeof(int));
            int useMap = (xmap && ymap);
            int xi, yi;
            if (useMap)
            {
                for (xi = 0; xi < W; xi++)
                    xmap[xi] = (int)((long long)xi * cw / W);
                for (yi = 0; yi < H; yi++)
                    ymap[yi] = (int)((long long)yi * ch / H);
            }
            for (int y = 0; y < H; y++)
            {
                int cy = useMap ? ymap[y] : (int)((long long)y * ch / H);
                for (int x = 0; x < W; x++)
                {
                    int yy = Y[(size_t)y * ys + x];
                    int g;
                    if (mono)
                    {
                        g = yy;
                    }
                    else
                    {
                        int cx = useMap ? xmap[x]
                                        : (int)((long long)x * cw / W);
                        int u = U[(size_t)cy * us + cx] - 128;
                    int v = V[(size_t)cy * us + cx] - 128;
                    int r = yy + ((359 * v) >> 8);
                    int gg = yy - ((88 * u + 183 * v) >> 8);
                    int b = yy + ((454 * u) >> 8);
                    if (r < 0)
                        r = 0;
                    if (r > 255)
                        r = 255;
                    if (gg < 0)
                        gg = 0;
                    if (gg > 255)
                        gg = 255;
                    if (b < 0)
                        b = 0;
                    if (b > 255)
                        b = 255;
                    g = (r * 306 + gg * 601 + b * 117) >> 10;
                }
                rows[(size_t)y * W + x] = (uint8_t)g;
                }
            }
            if (xmap)
                free(xmap);
            if (ymap)
                free(ymap);
        }
        if (W > 360 || H > 200)
        {
            if ((long long)W * 200 >= (long long)H * 360)
            {
                scaleNum = W;
                scaleDen = 360;
            }
            else
            {
                scaleNum = H;
                scaleDen = 200;
            }
            targetW = (int)((long long)W * scaleDen / scaleNum);
            if (targetW < 1)
                targetW = 1;
            targetH = (int)((long long)H * scaleDen / scaleNum);
            if (targetH < 1)
                targetH = 1;
        }
        p.rows = rows;
        p.width = W;
        p.height = H;
        p.scaleNum = scaleNum;
        p.scaleDen = scaleDen;
        img = dither_to_bitmap(targetW, targetH, avif_pixel_gray, &p);
        if (img)
            logger_log("[avif] ok %dx%d", W, H);
    }

done:
    if (rows)
        free(rows);
    if (have_pic)
        dav1d_picture_unref(&pic);
    if (c)
        dav1d_close(&c);
    if (obu)
        free(obu);
    return img;
}

/* Public entry (avif.h): fiber on device, direct everywhere else. */
LCDBitmap *avif_decode(const uint8_t *data, size_t len)
{
#if defined(TARGET_PLAYDATE) && defined(__arm__)
    uint32_t *canary;
    LCDBitmap *img;
    int smashed;
    if (!g_fiberStack)
    {
        g_fiberStack = (uint8_t *)malloc(AVIF_FIBER_STACK);
        if (!g_fiberStack)
        {
            return NULL;
        }
    }
    canary = (uint32_t *)g_fiberStack;
    canary[0] = canary[1] = canary[2] = canary[3] = AVIF_FIBER_CANARY;
    img = (LCDBitmap *)avif_fiber_call(g_fiberStack + AVIF_FIBER_STACK,
                                      (void *(*)(const uint8_t *, size_t))avif_decode_inner,
                                      data, len);
    smashed = (canary[0] != AVIF_FIBER_CANARY ||
               canary[1] != AVIF_FIBER_CANARY ||
               canary[2] != AVIF_FIBER_CANARY ||
               canary[3] != AVIF_FIBER_CANARY);
    if (smashed)
    {
        logger_log("[avif] FIBER STACK OVERFLOW (64KB exceeded)");
    }
    return img;
#else
    return avif_decode_inner(data, len);
#endif
}
