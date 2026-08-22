// jpeg.c — C port of Source/render/decoders/jpeg.lua (JPEGDecoder).
//
// Baseline SOF0: full luma-only 8x8 IDCT decode; chroma coefficients are
// consumed for bitstream sync but never upsampled (the Lua original only
// renders Y). When the box filter is coarse (>=4x) or the source area is
// large, blocks render as their DC value only (AC still consumed for
// sync). Progressive SOF2 files are decoded from their DC scans alone;
// AC scans trigger rendering of what is available. Arithmetic coding
// (SOF9/10/11) is unsupported. Restart markers are consumed by the bit
// reader and returned as-is into the stream (faithful quirk). The decode
// loop yields cooperatively via tasks_yield_check().

#include "render/decoders/jpeg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_JPEG_PD 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define JPEG_NIL (-1)
#define IDCT_SCALE (4LL * 4096 * 4096)

/* zigzag position -> natural (row-major) coefficient index */
static const uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

static long long g_idctT[64];
static int g_tablesReady = 0;

static void ensure_tables(void) {
    if (g_tablesReady) return;
    const double c0 = 0.707106781186548;
    for (int k = 0; k < 8; k++) {
        double c = (k == 0) ? c0 : 1.0;
        for (int n = 0; n < 8; n++) {
            double v = 4096.0 * c * cos((2 * n + 1) * k * M_PI / 16);
            g_idctT[k * 8 + n] = (long long)floor(v + 0.5);
        }
    }
    g_tablesReady = 1;
}

static uint8_t clamp255ll(long long v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* guarded accessors: -1 == Lua nil */
static unsigned j_u16(const uint8_t* d, size_t len, size_t o) {
    if (o >= len || o + 1 >= len) return 0;
    return ((unsigned)d[o] << 8) | (unsigned)d[o + 1];  /* JPEG: big-endian */
}
static int j_byte(const uint8_t* d, size_t len, size_t o) {
    return o < len ? d[o] : -1;
}

typedef struct {
    const uint8_t* str;
    size_t len;
    size_t pos;      /* 0-based; Lua reader used 1-based */
    uint32_t bitBuf;
    int nbits;
} JReader;

static int jr_read_byte(JReader* r) {
    if (r->pos >= r->len) return JPEG_NIL;
    int b = r->str[r->pos];
    r->pos++;
    if (b != 0xFF) return b;
    if (r->pos >= r->len) return JPEG_NIL;
    int n = r->str[r->pos];
    if (n == 0x00) { r->pos++; return 0xFF; }
    if (n >= 0xD0 && n <= 0xD7) { r->pos++; return n; }
    r->pos--;   /* marker: rewind to the FF (Lua pos = pos - 1) */
    return JPEG_NIL;
}

static int jr_read_bits(JReader* r, int n) {
    while (r->nbits < n) {
        int b = jr_read_byte(r);
        if (b == JPEG_NIL) return JPEG_NIL;
        r->bitBuf = (r->bitBuf << 8) | (uint32_t)b;
        r->nbits += 8;
    }
    int shift = r->nbits - n;
    int v = (int)((r->bitBuf >> shift) & ((1u << n) - 1u));
    r->nbits -= n;
    r->bitBuf &= (r->nbits >= 32) ? 0xFFFFFFFFu : ((1u << r->nbits) - 1u);
    return v;
}

static int jr_expect_restart(JReader* r, int n) {
    r->nbits = 0;
    int b = jr_read_byte(r);
    if (b == JPEG_NIL) return 0;
    if (b >= 0xD0 && b <= 0xD7 && (b - 0xD0) == (n % 8)) return 1;
    return 0;
}

typedef struct {
    int mincode[17], maxcode[17], valptr[17]; /* levels 1..16 */
    uint8_t values[256];
    int nValues;
} JHuff;

static JHuff* build_huff(const int counts[17], const uint8_t* values, int total) {
    JHuff* t = (JHuff*)pluto_malloc(sizeof(JHuff));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    int code = 0, k = 1;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        int c = counts[l];
        t->maxcode[l] = (c == 0) ? (-1) : (code + c - 1);
        k += c;
        code = (code + c) << 1;
    }
    t->nValues = total;
    memcpy(t->values, values, (size_t)(total > 256 ? 256 : total));
    return t;
}

static int decode_symbol(JReader* r, const JHuff* t) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        int b = jr_read_bits(r, 1);
        if (b == JPEG_NIL) return JPEG_NIL;
        code = (code << 1) | b;
        if (t->mincode[l] >= 0 && code <= t->maxcode[l]) {
            int idx = t->valptr[l] - 1 + (code - t->mincode[l]);
            if (idx < 0 || idx >= t->nValues || idx >= 256) return JPEG_NIL;
            return t->values[idx];
        }
    }
    return JPEG_NIL;
}

static int jpg_extend(int v, int s) {
    if (s == 0) return 0;
    return (v < (1 << (s - 1))) ? (v - (1 << s) + 1) : v;
}

static void idct2d(long long b[64]) {
    long long tmp[64];
    for (int rr = 0; rr < 8; rr++)
        for (int n = 0; n < 8; n++) {
            long long s = 0;
            for (int k = 0; k < 8; k++) s += b[rr * 8 + k] * g_idctT[k * 8 + n];
            tmp[rr * 8 + n] = s;
        }
    for (int c = 0; c < 8; c++)
        for (int n = 0; n < 8; n++) {
            long long s = 0;
            for (int k = 0; k < 8; k++) s += tmp[k * 8 + c] * g_idctT[k * 8 + n];
            b[n * 8 + c] = s;
        }
}

typedef struct { int id, h, v, qt; } JComp;

typedef struct {
    int width, height, precision, ncomp;
    JComp* comps;
} JFrame;

typedef struct {
    int ns;
    int ci[256];      /* frame component indices (1-based), fallback = scan pos */
    int dcSel[256], acSel[256];
    int ss, se, ah, al;
} JScan;

/* Progressive state: persists across scans within one decode call.
 * Keys are 1-based component indices (0 unused), so all arrays span [256]. */
typedef struct {
    long long dcPred[256];   /* keyed by frame comp index */
    long long* blockDC[256];
    int cap[256];
    long long blkIdx[256];
} JPState;

static void jpstate_reset(JFrame* f, JPState* st) {
    int sMaxH = 1, sMaxV = 1;
    for (int i = 0; i < f->ncomp; i++) {
        if (f->comps[i].h > sMaxH) sMaxH = f->comps[i].h;
        if (f->comps[i].v > sMaxV) sMaxV = f->comps[i].v;
    }
    int bw = sMaxH * 8, bh = sMaxV * 8;
    long long mcuColsF = (f->width + bw - 1) / bw; if (mcuColsF < 1) mcuColsF = 1;
    long long mcuRowsF = (f->height + bh - 1) / bh; if (mcuRowsF < 1) mcuRowsF = 1;
    for (int c = 1; c < 256; c++) {
        st->dcPred[c] = 0;
        st->blkIdx[c] = 0;
    }
    for (int i = 1; i <= f->ncomp && i < 256; i++) {
        if (st->blockDC[i]) pluto_free(st->blockDC[i]);
        st->blockDC[i] = NULL;
        long long cap = mcuColsF * f->comps[i - 1].h * mcuRowsF * f->comps[i - 1].v;
        if (cap < 1) cap = 1;
        st->cap[i] = (cap > 0x7FFFFFF0LL) ? 0x7FFFFFF0 : (int)cap;
        st->blockDC[i] = (long long*)pluto_calloc((size_t)st->cap[i], sizeof(long long));
        if (st->blockDC[i] == NULL) st->cap[i] = 0;
    }
    for (int i = f->ncomp + 1; i < 256; i++) {
        if (st->blockDC[i]) pluto_free(st->blockDC[i]);
        st->blockDC[i] = NULL;
        st->cap[i] = 0;
    }
}

static void jpstate_free(JPState* st) {
    for (int i = 0; i < 256; i++) {
        if (st->blockDC[i]) pluto_free(st->blockDC[i]);
        st->blockDC[i] = NULL;
        st->cap[i] = 0;
    }
}

static long long* jp_block_slot(JPState* st, int c, long long idx) {
    if (c < 1 || c > 255) return NULL;
    if (idx < 0 || idx >= st->cap[c]) return NULL;
    return &st->blockDC[c][idx];
}

/* Lazy row buffer over the source canvas (rows calloc'd zero-filled where
 * Lua left untouched cells as nil). */
static uint8_t** rowbuf_new(int h) {
    return (uint8_t**)pluto_calloc((size_t)(h > 0 ? h : 1), sizeof(uint8_t*));
}

static void rowbuf_put(uint8_t** rb, int width, int h,
                       int x, int y, uint8_t g) {
    /* Lua wrote into a sparse canvas: off-canvas cells were silently
     * dropped. MCU padding blocks land past height/width, so clip here. */
    if (x < 0 || y < 0 || x >= width || y >= h || !rb) return;
    if (!rb[y]) rb[y] = (uint8_t*)pluto_calloc((size_t)width, 1);
    if (rb[y]) rb[y][x] = g;
}

static void rowbuf_fill_block(uint8_t** rb, int width, int h,
                              int px, int py, uint8_t g) {
    for (int ri = 0; ri < 8; ri++)
        for (int cx = 0; cx < 8; cx++)
            rowbuf_put(rb, width, h, px + cx, py + ri, g);
}

static void rowbuf_free(uint8_t** rb, int h) {
    if (!rb) return;
    for (int y = 0; y < h; y++) pluto_free(rb[y]);
    pluto_free(rb);
}

/*-----------------------------------------------------------------------------
 * Baseline scan decoding
 *---------------------------------------------------------------------------*/

static int decode_baseline(const uint8_t* data, size_t len, size_t pos,
                           const JFrame* f, const JScan* sc,
                           JHuff* const* dcTables, JHuff* const* acTables,
                           int* const* qt,
                           int restartInterval, int maxW, int maxH,
                           ScaleAccum** outAcc, size_t* outPos);

/*-----------------------------------------------------------------------------
 * Progressive DC scans
 *---------------------------------------------------------------------------*/

static int decode_progressive_dc(const uint8_t* data, size_t len, size_t pos,
                                 const JFrame* f, const JScan* sc,
                                 JHuff* const* dcTables,
                                 int restartInterval, JPState* st,
                                 size_t* outPos);

static ScaleAccum* render_progressive_dc(const JFrame* f, JPState* st,
                                         int maxW, int maxH, int* const* qt);

static int decode_ac(JReader* rd, const JHuff* tbl, long long block[64],
                     const int* qt) {
    tasks_yield_check();
    int k = 1;
    while (k <= 63) {
        int s = decode_symbol(rd, tbl);
        if (s == JPEG_NIL) return 0;
        int rr = s >> 4, cs = s & 15;
        if (cs == 0) {
            if (rr == 15) { k += 16; continue; }  /* ZRL */
            return 1;                             /* EOB */
        }
        k += rr;
        if (k > 63) return 1;                     /* overrun quirk: no bits consumed */
        int v = jr_read_bits(rd, cs);
        if (v == JPEG_NIL) return 0;
        block[kZigzag[k]] = (long long)jpg_extend(v, cs) * qt[k];
        k += 1;
    }
    return 1;
}

static int decode_dc_sym(JReader* rd, const JHuff* tbl, int* ok) {
    int s = decode_symbol(rd, tbl);
    *ok = 1;
    if (s == JPEG_NIL) { *ok = 0; return 0; }
    if (s == 0) return 0;
    int v = jr_read_bits(rd, s);
    if (v == JPEG_NIL) { *ok = 0; return 0; }
    return jpg_extend(v, s);
}

static int decode_baseline(const uint8_t* data, size_t len, size_t pos,
                           const JFrame* f, const JScan* sc,
                           JHuff* const* dcTables, JHuff* const* acTables,
                           int* const* qt,
                           int restartInterval, int maxW, int maxH,
                           ScaleAccum** outAcc, size_t* outPos) {
    *outAcc = NULL;
    int ns = sc->ns;

    int sMaxH = 1, sMaxV = 1;
    for (int ci = 1; ci <= ns; ci++) {
        const JComp* fc = &f->comps[sc->ci[ci] - 1];
        if (fc->h > sMaxH) sMaxH = fc->h;
        if (fc->v > sMaxV) sMaxV = fc->v;
    }
    if (ns == 1) { sMaxH = 1; sMaxV = 1; }

    int bw = sMaxH * 8, bh = sMaxV * 8;
    int mcuCols = (f->width + bw - 1) / bw; if (mcuCols < 1) mcuCols = 1;
    int mcuRows = (f->height + bh - 1) / bh; if (mcuRows < 1) mcuRows = 1;

    ScaleAccum* acc = scale_accum_new(f->width, f->height, maxW, maxH);
    if (!acc) { *outPos = pos; return 0; }
    uint8_t** rowBuf = rowbuf_new(f->height);

    int boxW = 1, boxH = 1, twIgn = 0, thIgn = 0;
    scale_box_sizes(f->width, f->height, maxW, maxH, &boxW, &boxH, &twIgn, &thIgn);
    int dcOnly = (boxW >= 4) || (boxH >= 4) ||
                 ((long long)f->width * (long long)f->height > 200000LL);

    JReader rd = { data, len, pos, 0, 0 };
    int dcPred[256] = {0};   /* keyed by scan component position 1..ns */
    long long block[64];
    long long mcuIndex = 0;
    int ended = 0;
    int zeroQt[64] = {0};

    for (int mcuY = 0; mcuY < mcuRows; mcuY++) {
        tasks_yield_check();
        for (int mcuX = 0; mcuX < mcuCols; mcuX++) {
            if (!ended && restartInterval > 0 && mcuIndex > 0 &&
                (mcuIndex % restartInterval) == 0) {
                if (!jr_expect_restart(&rd, (int)(mcuIndex % 8))) {
                    ended = 1;
                } else {
                    for (int i = 0; i <= ns; i++) dcPred[i] = 0;
                }
            }
            for (int ci = 1; ci <= ns && !ended; ci++) {
                const JComp* fc = &f->comps[sc->ci[ci] - 1];
                const JHuff* dcTbl = dcTables[sc->dcSel[ci]];
                const JHuff* acTbl = acTables[sc->acSel[ci]];
                const int* qtz = qt[fc->qt];
                if (!qtz) qtz = zeroQt;   /* missing table: zeros (Lua would error) */

                for (int bj = 0; bj < fc->v && !ended; bj++) {
                    for (int bi = 0; bi < fc->h && !ended; bi++) {
                        int ok;
                        int diff = decode_dc_sym(&rd, dcTbl, &ok);
                        if (!ok) { ended = 1; break; }
                        dcPred[ci] += diff;
                        int dc = dcPred[ci];
                        int px = mcuX * bw + bi * 8;
                        int py = mcuY * bh + bj * 8;

                        if (ci == 1) {
                            if (dcOnly) {
                                decode_ac(&rd, acTbl, block, qtz); /* sync only */
                                int g = clamp255ll(
                                    (long long)floor((double)dc * (double)qtz[0] / 8.0 + 0.5) + 128);
                                rowbuf_fill_block(rowBuf, f->width, f->height, px, py, g);
                            } else {
                                block[0] = (long long)dc * qtz[0];
                                for (int i = 1; i < 64; i++) block[i] = 0;
                                if (!decode_ac(&rd, acTbl, block, qtz)) { ended = 1; break; }
                                idct2d(block);
                                for (int vy = 0; vy < 8; vy++) {
                                    for (int vx = 0; vx < 8; vx++) {
                                        double dv = (double)block[vy * 8 + vx] /
                                                    (double)IDCT_SCALE;
                                        int g = clamp255ll(
                                            (long long)floor(dv + 0.5) + 128);
                                        rowbuf_put(rowBuf, f->width, f->height, px + vx, py + vy,
                                                   (uint8_t)g);
                                    }
                                }
                            }
                        } else {
                            /* Chroma: consumed for sync only, results discarded */
                            block[0] = 0;
                            decode_ac(&rd, acTbl, block, qtz);
                        }
                    }
                }
            }
            mcuIndex++;
        }
        /* Feed this MCU band even when the scan ended mid-row. */
        int base = mcuY * bh;
        for (int ri = 0; ri < bh; ri++) {
            int y = base + ri;
            if (y < f->height) scale_accum_add_row(acc, rowBuf[y]);
        }
        if (ended) break;
    }

    rowbuf_free(rowBuf, f->height);
    *outPos = rd.pos;
    *outAcc = acc;
    return ended;
}

static int decode_progressive_dc(const uint8_t* data, size_t len, size_t pos,
                                 const JFrame* f, const JScan* sc,
                                 JHuff* const* dcTables,
                                 int restartInterval, JPState* st,
                                 size_t* outPos) {
    int ns = sc->ns;

    int maxHf = 1, maxVf = 1;
    for (int i = 0; i < f->ncomp; i++) {
        if (f->comps[i].h > maxHf) maxHf = f->comps[i].h;
        if (f->comps[i].v > maxVf) maxVf = f->comps[i].v;
    }
    int bw = maxHf * 8, bh = maxVf * 8;
    int mcuColsF = (f->width + bw - 1) / bw; if (mcuColsF < 1) mcuColsF = 1;
    int mcuRowsF = (f->height + bh - 1) / bh; if (mcuRowsF < 1) mcuRowsF = 1;

    int mcuCols, mcuRows;
    if (ns == 1) {
        const JComp* fc = &f->comps[sc->ci[1] - 1];
        mcuCols = mcuColsF * fc->h;
        mcuRows = mcuRowsF * fc->v;
    } else {
        mcuCols = mcuColsF;
        mcuRows = mcuRowsF;
    }

    JReader rd = { data, len, pos, 0, 0 };
    int refinement = (sc->ah != 0);
    long long mcuIndex = 0;
    int ended = 0;

    for (int mcuY = 0; mcuY < mcuRows && !ended; mcuY++) {
        tasks_yield_check();
        for (int mcuX = 0; mcuX < mcuCols && !ended; mcuX++) {
            if (restartInterval > 0 && mcuIndex > 0 &&
                (mcuIndex % restartInterval) == 0) {
                if (!jr_expect_restart(&rd, (int)(mcuIndex % 8))) {
                    ended = 1;
                    break;
                }
                for (int i = 1; i <= ns; i++)
                    st->dcPred[sc->ci[i]] = 0;
            }

            for (int cix = 1; cix <= ns && !ended; cix++) {
                int c = sc->ci[cix];
                const JHuff* dcTbl = dcTables[sc->dcSel[cix]];
                int blocks = (ns == 1) ? 1
                    : f->comps[c - 1].h * f->comps[c - 1].v;
                for (int bi = 0; bi < blocks && !ended; bi++) {
                    tasks_yield_check();
                    long long idx = st->blkIdx[c];
                    st->blkIdx[c] = idx + 1;
                    long long* slot = jp_block_slot(st, c, idx);
                    if (refinement) {
                        int bit = jr_read_bits(&rd, 1);
                        if (bit == JPEG_NIL) { ended = 1; break; }
                        long long dcv = slot ? *slot : 0;
                        if (slot) {
                            if (bit != 0) *slot = dcv + (1LL << sc->al);
                            else          *slot = dcv - (1LL << sc->al);
                        }
                    } else {
                        int ok;
                        int diff = decode_dc_sym(&rd, dcTbl, &ok);
                        if (!ok) { ended = 1; break; }
                        st->dcPred[c] += diff;
                        if (slot) *slot = st->dcPred[c] << sc->al;
                    }
                }
            }
            mcuIndex++;
        }
    }

    *outPos = rd.pos;
    return !ended;
}

static ScaleAccum* render_progressive_dc(const JFrame* f, JPState* st,
                                         int maxW, int maxH, int* const* qt) {
    int yc = 1;   /* luma assumed first component */

    int sMaxH = 1, sMaxV = 1;
    for (int i = 0; i < f->ncomp; i++) {
        if (f->comps[i].h > sMaxH) sMaxH = f->comps[i].h;
        if (f->comps[i].v > sMaxV) sMaxV = f->comps[i].v;
    }
    int bw = sMaxH * 8, bh = sMaxV * 8;
    int mcuCols = (f->width + bw - 1) / bw; if (mcuCols < 1) mcuCols = 1;
    int mcuRows = (f->height + bh - 1) / bh; if (mcuRows < 1) mcuRows = 1;

    ScaleAccum* acc = scale_accum_new(f->width, f->height, maxW, maxH);
    if (!acc) return NULL;
    uint8_t** rowBuf = rowbuf_new(f->height);

    const JComp* yfc = &f->comps[yc - 1];
    const int* qtz = qt[yfc->qt];
    long long qdc = qtz ? qtz[0] : 1;
    long long idx = 0;

    for (int mcuY = 0; mcuY < mcuRows; mcuY++) {
        tasks_yield_check();
        for (int mcuX = 0; mcuX < mcuCols; mcuX++) {
            for (int bj = 0; bj < yfc->v; bj++) {
                for (int bi = 0; bi < yfc->h; bi++) {
                    tasks_yield_check();
                    long long dc = 0;
                    long long* slot = jp_block_slot(st, yc, idx);
                    if (slot) dc = *slot;
                    idx++;
                    int px = mcuX * sMaxH * 8 + bi * 8;
                    int py = mcuY * sMaxV * 8 + bj * 8;
                    int g = clamp255ll(
                        (long long)floor((double)dc * (double)qdc / 8.0 + 0.5) + 128);
                    rowbuf_fill_block(rowBuf, f->width, f->height, px, py, (uint8_t)g);
                }
            }
        }
        int base = mcuY * sMaxV * 8;
        for (int ri = 0; ri < sMaxV * 8; ri++) {
            int y = base + ri;
            if (y < f->height) scale_accum_add_row(acc, rowBuf[y]);
        }
    }

    rowbuf_free(rowBuf, f->height);
    return acc;
}

/*-----------------------------------------------------------------------------
 * Main decoder
 *---------------------------------------------------------------------------*/

static void frame_free(JFrame* f) {
    if (!f) return;
    pluto_free(f->comps);
    pluto_free(f);
}

int jpeg_decode_gray(const uint8_t* data, size_t len,
                     int maxW, int maxH,
                     uint8_t*** outRows, int* outW, int* outH) {
    ensure_tables();
    *outRows = NULL; *outW = 0; *outH = 0;
    if (!data || len < 4) return -1;
    if (data[0] != 0xFF || data[1] != 0xD8) return -1;
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    size_t pos0 = 2;                 /* Lua pos = 3 */
    JFrame* frame = NULL;
    int* qt[16] = {0};
    JHuff* dcTables[16] = {0};
    JHuff* acTables[16] = {0};
    int restartInterval = 0;
    int progressive = 0;
    JPState* st = (JPState*)pluto_calloc(1, sizeof(JPState));
    if (st == NULL) goto cleanup;
    ScaleAccum* acc = NULL;
    int rc = -1;
    uint8_t** grid = NULL;

    while (pos0 + 1 < len) {         /* Lua: pos <= #data - 1 */
        if (data[pos0] != 0xFF) break;
        int m = data[pos0 + 1];
        pos0 += 2;

        if (m == 0xD9) break;        /* EOI */

        else if (m == 0xDB) {        /* DQT */
            size_t p = pos0;
            size_t segEnd = p + j_u16(data, len, p);
            p += 2;
            while (p < segEnd) {
                int pq = j_byte(data, len, p);
                p++;
                if (pq == JPEG_NIL) break;
                int id = pq & 0x0F;
                int* t = (int*)pluto_calloc(64, sizeof(int));
                if (!t) goto cleanup;
                pluto_free(qt[id]);
                qt[id] = t;
                for (int i = 0; i < 64; i++) {
                    if ((pq >> 4) == 0) {
                        int b = j_byte(data, len, p);
                        t[i] = (b == JPEG_NIL) ? 1 : b;
                        p++;
                    } else {
                        t[i] = (int)j_u16(data, len, p);
                        p += 2;
                    }
                }
            }
            pos0 = segEnd;
        }

        else if (m == 0xC4) {        /* DHT */
            size_t p = pos0;
            size_t segEnd = p + j_u16(data, len, p);
            p += 2;
            while (p < segEnd) {
                int tc = j_byte(data, len, p);
                p++;
                if (tc == JPEG_NIL) break;
                int counts[17] = {0};
                int total = 0;
                for (int i = 1; i <= 16; i++) {
                    int c = j_byte(data, len, p);
                    p++;
                    counts[i] = (c == JPEG_NIL) ? 0 : c;
                    total += counts[i];
                }
                if (total > 256) total = 256;
                uint8_t values[256];
                int rawTotal = 0;
                for (int i = 1; i <= 16; i++) rawTotal += counts[i];
                for (int i = 0; i < rawTotal; i++) {
                    int b = j_byte(data, len, p);
                    p++;
                    if (i < 256) values[i] = (uint8_t)((b == JPEG_NIL) ? 0 : b);
                }
                JHuff* t = build_huff(counts, values, total);
                if (!t) goto cleanup;
                int cls = (tc >> 4) & 1;
                int id = tc & 0x0F;
                if (cls == 0) {
                    pluto_free(dcTables[id]);
                    dcTables[id] = t;
                } else {
                    pluto_free(acTables[id]);
                    acTables[id] = t;
                }
            }
            pos0 = segEnd;
        }

        else if (m == 0xC0 || m == 0xC2) {   /* SOF0 / SOF2 */
            size_t p = pos0 + 2;             /* skip length */
            int precision = j_byte(data, len, p); if (precision == JPEG_NIL) precision = 8;
            int height = (int)j_u16(data, len, p + 1);
            int width  = (int)j_u16(data, len, p + 3);
            int ncomp  = j_byte(data, len, p + 5); if (ncomp == JPEG_NIL) ncomp = 0;
            if (ncomp < 0 || ncomp > 255 || width <= 0 || height <= 0 ||
                p + 6 + (size_t)ncomp * 3 > len) goto cleanup;
            JComp* comps = (JComp*)pluto_calloc((size_t)ncomp, sizeof(JComp));
            if (!comps) goto cleanup;
            p += 6;
            for (int i = 0; i < ncomp; i++) {
                comps[i].id = j_byte(data, len, p);
                int samp = j_byte(data, len, p + 1);
                if (samp == JPEG_NIL) samp = 0x11;
                comps[i].h = samp >> 4;
                comps[i].v = samp & 0x0F;
                int qtid = j_byte(data, len, p + 2);
                comps[i].qt = (qtid == JPEG_NIL) ? 0 : (qtid & 0x0F);
                p += 3;
            }
            frame_free(frame);
            frame = (JFrame*)pluto_malloc(sizeof(JFrame));
            if (!frame) { pluto_free(comps); goto cleanup; }
            frame->width = width;
            frame->height = height;
            frame->precision = precision;
            frame->ncomp = ncomp;
            frame->comps = comps;
            progressive = (m == 0xC2);
            jpstate_reset(frame, st);
            pos0 += j_u16(data, len, pos0);
        }

        else if (m == 0xDD) {        /* DRI */
            restartInterval = (int)j_u16(data, len, pos0 + 2);
            pos0 += j_u16(data, len, pos0);
        }

        else if (m == 0xDA) {        /* SOS */
            if (!frame) break;
            size_t lp = pos0;        /* length field start */
            int segLen = (int)j_u16(data, len, lp);
            int ns = j_byte(data, len, lp + 2); if (ns == JPEG_NIL) ns = 0;
            if (ns < 1 || ns > 255) { if (segLen > 0) pos0 += segLen; else break; continue; }
            JScan sc;
            memset(&sc, 0, sizeof(sc));
            sc.ns = ns;
            size_t p = lp + 3;
            for (int i = 1; i <= ns; i++) {
                int cid = j_byte(data, len, p);
                int tbls = j_byte(data, len, p + 1);
                if (tbls == JPEG_NIL) tbls = 0;
                int ci = 0;
                for (int jj = 1; jj <= frame->ncomp; jj++) {
                    if (frame->comps[jj - 1].id == cid) { ci = jj; break; }
                }
                if (ci == 0) ci = i;
                sc.ci[i] = ci;
                sc.dcSel[i] = tbls >> 4;
                sc.acSel[i] = tbls & 0x0F;
                p += 2;
            }
            int ss = j_byte(data, len, p); if (ss == JPEG_NIL) ss = 0;
            int se = j_byte(data, len, p + 1); if (se == JPEG_NIL) se = 63;
            int ahal = j_byte(data, len, p + 2); if (ahal == JPEG_NIL) ahal = 0;
            sc.ss = ss;
            sc.se = se;
            sc.ah = ahal >> 4;
            sc.al = ahal & 0x0F;

            size_t entropyPos = lp + (size_t)segLen;

            if (progressive) {
                if (sc.ss == 0 && sc.se == 0) {
                    size_t nextPos = entropyPos;
                    int ok = decode_progressive_dc(data, len, entropyPos, frame, &sc,
                                                   dcTables, restartInterval, st,
                                                   &nextPos);
                    pos0 = nextPos;
                    if (!ok) break;
                } else {
                    /* First AC scan reached: DCs are final enough -- render */
                    if (acc == NULL)
                        acc = render_progressive_dc(frame, st, maxW, maxH, qt);
                    break;
                }
            } else {
                ScaleAccum* res = NULL;
                size_t nextPos = entropyPos;
                int ended = decode_baseline(data, len, entropyPos, frame, &sc,
                                            dcTables, acTables, (int* const*)qt,
                                            restartInterval, maxW, maxH,
                                            &res, &nextPos);
                acc = res;
                pos0 = nextPos;
                if (ended) break;
            }
        }

        else {
            /* Other segment: skip by length */
            unsigned slen = j_u16(data, len, pos0);
            if (slen == 0) break;   /* hang guard (Lua would spin here) */
            pos0 += slen;
        }
    }

    if (acc == NULL && frame)
        acc = render_progressive_dc(frame, st, maxW, maxH, qt);

    if (acc != NULL && acc->count > 0) {
        int tw = 0, th = 0;
        int count = scale_accum_finish(acc, &tw, &th);
        if (count > 0 && tw > 0) {
            grid = (uint8_t**)rowbuf_new(count);
            int okAlloc = 1;
            for (int y = 0; y < count && okAlloc; y++) {
                grid[y] = (uint8_t*)pluto_malloc((size_t)tw);
                if (!grid[y]) { okAlloc = 0; break; }
                for (int x = 0; x < tw; x++)
                    grid[y][x] = clamp255ll(acc->out[y][x]);
            }
            if (okAlloc) {
                *outRows = grid;
                *outW = tw;
                *outH = count;
                grid = NULL;
                rc = 0;
            }
        }
    }

cleanup:
    if (grid) rowbuf_free(grid, *outH);
    if (acc) scale_accum_free(acc);
    if (st != NULL) {
        jpstate_free(st);
        pluto_free(st);
    }
    frame_free(frame);
    for (int i = 0; i < 16; i++) {
        pluto_free(qt[i]);
        pluto_free(dcTables[i]);
        pluto_free(acTables[i]);
    }
    return rc;
}

void jpeg_free_rows(uint8_t** rows, int h) {
    if (!rows) return;
    for (int y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

#if defined(PLUTO_JPEG_PD)
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} JpegPixCtx;

static int jpeg_pix(void* ud, int x, int y) {
    JpegPixCtx* c = (JpegPixCtx*)ud;
    if (y >= c->h || c->rows[y] == NULL) return 255;
    return x < c->w ? c->rows[y][x] : 255;
}

struct LCDBitmap* jpeg_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH) {
    uint8_t** rows = NULL;
    int tw = 0, th = 0;
    if (jpeg_decode_gray(data, len, maxW, maxH, &rows, &tw, &th) != 0)
        return NULL;
    JpegPixCtx ctx = { rows, tw, th };
    struct LCDBitmap* img =
        dither_to_image(pd, jpeg_pix, &ctx, tw, th);
    jpeg_free_rows(rows, th);
    return img;
}
#else
struct LCDBitmap* jpeg_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH) {
    (void)pd; (void)data; (void)len; (void)maxW; (void)maxH;
    return NULL;
}
#endif
