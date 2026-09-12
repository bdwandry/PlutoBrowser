/*
 * PlutoBrowser — jpeg.c
 * Port of Source/render/decoders/jpeg.lua (reference, 670 lines).
 * See jpeg.h for the Lua→C function map and preserved semantics.
 *
 * Stack discipline (P22 rule): all large buffers live in BSS/static or heap;
 * the public entry points stay shallow. The decoder is re-entrant across
 * sequential calls because every table/state lives in the JpegState struct.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "pd_api.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/scale.h"
#include "render/decoders/dither.h"
#include "core/tasks.h"
#include "core/logger.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_pd()->system->realloc((p), 0)

/* ── Zigzag: zigzag[zz] = natural (row-major) index, zz = 0..63 (verbatim) ─ */
static const uint8_t JPEG_ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* Fixed-point IDCT basis (verbatim):
 * idctT[k*8+n] = floor(4096 * C(k) * cos((2n+1)k*pi/16) + 0.5) */
static int JPEG_IDCT_T[64];
static int jpeg_idct_ready = 0;

static void jpeg_idct_init(void)
{
    if (jpeg_idct_ready)
    {
        return;
    }
    const double c0 = 0.707106781186548;
    for (int k = 0; k < 8; k++)
    {
        double c = (k == 0) ? c0 : 1.0;
        for (int n = 0; n < 8; n++)
        {
            double v = 4096.0 * c * cos((2 * n + 1) * k * 3.14159265358979323846 / 16.0);
            JPEG_IDCT_T[k * 8 + n] = (int)floor(v + 0.5);
        }
    }
    jpeg_idct_ready = 1;
}

#define JPEG_IDCT_SCALE (4 * 4096 * 4096)

/* ── Bit reader (MSB-first, JPEG byte stuffing, restart/marker detection) ── */
typedef struct
{
    const uint8_t *str;
    size_t len;
    size_t pos;      /* 0-based index of the NEXT byte (Lua pos is 1-based) */
    unsigned int bitBuf;
    int nbits;
    int eof;         /* a real marker stopped the stream */
} JpegReader;

/* Next byte with stuffing removed; RST markers returned as-is; -1 at a real
 * marker (the reader rewinds so the marker starts at the FF byte — the
 * reference's `pos = pos - 1`). */
static int jpeg_rd_byte(JpegReader *r)
{
    if (r->eof)
    {
        return -1;
    }
    if (r->pos >= r->len)
    {
        r->eof = 1;
        return -1;
    }
    uint8_t b = r->str[r->pos];
    r->pos++;
    if (b == 0xFF)
    {
        if (r->pos >= r->len)
        {
            r->eof = 1;
            return -1;
        }
        uint8_t n = r->str[r->pos];
        if (n == 0x00)
        {
            r->pos++;
            return 0xFF;
        }
        if (n >= 0xD0 && n <= 0xD7)
        {
            r->pos++;
            return n; /* restart marker, part of the entropy stream */
        }
        r->pos--; /* rewind so the marker starts at the FF byte */
        r->eof = 1;
        return -1;
    }
    return b;
}

static int jpeg_rd_bits(JpegReader *r, int n)
{
    if (n == 0)
    {
        return 0;
    }
    while (r->nbits < n)
    {
        int b = jpeg_rd_byte(r);
        if (b < 0)
        {
            return -1;
        }
        r->bitBuf = (r->bitBuf << 8) | (unsigned)b;
        r->nbits += 8;
    }
    int v = (int)((r->bitBuf >> (r->nbits - n)) & ((1u << n) - 1));
    r->nbits -= n;
    r->bitBuf &= (r->nbits >= 32 || r->nbits < 0) ? 0 : ((1u << r->nbits) - 1);
    return v;
}

/* Reset bit buffer; require exact RSTn (n = mcuIndex % 8). */
static int jpeg_expect_restart(JpegReader *r, int mcuIndex)
{
    r->nbits = 0;
    r->bitBuf = 0;
    int b = jpeg_rd_byte(r);
    if (b < 0)
    {
        return 0;
    }
    if (b >= 0xD0 && b <= 0xD7 && (b - 0xD0) == (mcuIndex % 8))
    {
        return 1;
    }
    return 0;
}

/* ── Canonical MSB-first Huffman tables ────────────────────────────────────── */
#define HUFF_MAXCODE_LEN 17
typedef struct
{
    int mincode[HUFF_MAXCODE_LEN];  /* [1..16] */
    int maxcode[HUFF_MAXCODE_LEN];  /* [1..16] */
    int valptr[HUFF_MAXCODE_LEN];   /* [1..16] */
    uint8_t values[256];
    int valueCount;
} JpegHuff;

static void jpeg_build_huff(JpegHuff *h, const uint8_t counts[16], const uint8_t *values, int total)
{
    memset(h, 0, sizeof(*h));
    h->valueCount = total;
    for (int i = 0; i < total && i < 256; i++)
    {
        h->values[i] = values[i];
    }
    int code = 0;
    int k = 1; /* 1-based like the reference */
    for (int l = 1; l <= 16; l++)
    {
        int c = counts[l - 1];
        if (c != 0)
        {
            h->mincode[l] = code;
            h->maxcode[l] = code + c - 1;
            h->valptr[l] = k;
            k += c;
        }
        else
        {
            h->mincode[l] = -1;
            h->maxcode[l] = -1;
            h->valptr[l] = k;
        }
        code = (code + c) << 1;
    }
}

static int jpeg_decode_symbol(JpegReader *r, const JpegHuff *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++)
    {
        int bit = jpeg_rd_bits(r, 1);
        if (bit < 0)
        {
            return -1;
        }
        code = (code << 1) | bit;
        int mn = h->mincode[l];
        if (mn >= 0 && code >= mn && code <= h->maxcode[l])
        {
            int idx = h->valptr[l] + code - mn; /* 1-based */
            if (idx < 1 || idx > h->valueCount)
            {
                return -1;
            }
            return h->values[idx - 1];
        }
    }
    return -1;
}

static int jpeg_extend(int val, int s)
{
    if (s == 0)
    {
        return 0;
    }
    if (val < (1 << (s - 1)))
    {
        return val - (1 << s) + 1;
    }
    return val;
}

/* DC difference: signed diff. End-of-stream is signalled by r->eof (set by
 * the reader on every failure path) — no sentinel value, so a legitimate
 * -32768 DC difference (s=16) cannot be misread. */
static int jpeg_decode_dc(JpegReader *r, const JpegHuff *tbl)
{
    int s = jpeg_decode_symbol(r, tbl);
    if (s < 0)
    {
        return 0;
    }
    if (s == 0)
    {
        return 0;
    }
    int v = jpeg_rd_bits(r, s);
    if (v < 0)
    {
        return 0;
    }
    return jpeg_extend(v, s);
}

/* Two-pass separable fixed-point IDCT (verbatim algorithm). */
/* 64-bit accumulators throughout: the Lua reference computes in doubles and
 * intermediate sums (dequantized coefficients x 4096-scale basis, two passes)
 * overflow int32 — the exact bug the battery caught. Final spatial values
 * final spatial samples (pixel 255 -> (255-128)*4*4096*4096 ~ 8.5e9) do NOT
 * fit int32 either — the Lua reference holds them in doubles. So the output
 * is int64 as well and the caller divides + rounds in 64-bit. */
static void jpeg_idct2d(int64_t *block, int64_t *tmp)
{
    for (int r = 0; r < 8; r++)
    {
        int off = r * 8;
        for (int n = 0; n < 8; n++)
        {
            int64_t s = 0;
            for (int k = 0; k < 8; k++)
            {
                s += (int64_t)block[off + k] * JPEG_IDCT_T[k * 8 + n];
            }
            tmp[off + n] = s;
        }
    }
    for (int c = 0; c < 8; c++)
    {
        for (int n = 0; n < 8; n++)
        {
            int64_t s = 0;
            for (int k = 0; k < 8; k++)
            {
                s += tmp[k * 8 + c] * JPEG_IDCT_T[k * 8 + n];
            }
            block[n * 8 + c] = s;
        }
    }
}

static int jpeg_clamp255(int v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > 255)
    {
        return 255;
    }
    return v;
}

/* ── AC coefficients (natural order, dequantized) ─────────────────────────── */
/* Returns 1 ok, 0 end-of-scan (marker/EOB-exhausted) — the Lua boolean. */
static int jpeg_decode_ac(JpegReader *r, const JpegHuff *tbl, int64_t *block, const uint16_t *qt)
{
    int k = 1; /* zigzag position, 1..63 (Lua k) */
    while (k <= 63)
    {
        int s = jpeg_decode_symbol(r, tbl);
        if (s < 0)
        {
            return 0;
        }
        int run = s >> 4;
        int cs = s & 15;
        if (cs == 0)
        {
            if (run == 15)
            {
                k += 16; /* ZRL */
            }
            else
            {
                return 1; /* EOB */
            }
        }
        else
        {
            k += run;
            if (k > 63)
            {
                return 1;
            }
            int v = jpeg_rd_bits(r, cs);
            if (v < 0)
            {
                return 0;
            }
            block[JPEG_ZIGZAG[k]] = (int64_t)jpeg_extend(v, cs) * qt[k + 1];
            k += 1;
        }
    }
    return 1;
}

/* ── Frame/scan segment structs ───────────────────────────────────────────── */
typedef struct
{
    int id;
    int h, v;   /* sampling factors */
    int qt;     /* quant table id */
} JpegComp;

typedef struct
{
    int width, height, precision;
    JpegComp comps[4];
    int ncomp;
} JpegFrame;

typedef struct
{
    int comps[5];   /* frame component indices (Lua 1-based: [1..4]) */
    int dcTbl[4];
    int acTbl[4];
    int ns;
    int ss, se, ah, al;
} JpegScan;

/* Progressive DC state. blockDC[c] is HEAP, sized to the component's exact
 * block count on first touch (Lua tables grow unbounded; a fixed static cap
 * would silently truncate large photos). Keyed by 1-based frame component
 * index c → slot 0 unused. */
typedef struct
{
    int64_t dcPred[5];
    int64_t *blockDC[5];
    int blockDCcap[5];
    int blkIdx[5];
} JpegProgState;

/* Ensure blockDC[c] holds >= n int64 DC coefficients (zero-filled on first
 * alloc). DC values live in Lua doubles — must stay 64-bit in C. */
static int jpeg_prog_ensure(JpegProgState *st, int c, int n)
{
    if (c < 1 || c > 4)
    {
        return 0;
    }
    if (st->blockDC[c] && st->blockDCcap[c] >= n)
    {
        return 1;
    }
    int newCap = st->blockDCcap[c] ? st->blockDCcap[c] : 256;
    while (newCap < n)
    {
        newCap *= 2;
    }
    int64_t *p = (int64_t *)PLUTO_MALLOC(sizeof(int64_t) * (size_t)newCap);
    if (!p)
    {
        return 0;
    }
    memset(p, 0, sizeof(int64_t) * (size_t)newCap);
    if (st->blockDC[c])
    {
        memcpy(p, st->blockDC[c], sizeof(int64_t) * (size_t)st->blockDCcap[c]);
        PLUTO_FREE(st->blockDC[c]);
    }
    st->blockDC[c] = p;
    st->blockDCcap[c] = newCap;
    return 1;
}

static void jpeg_prog_free(JpegProgState *st)
{
    for (int c = 1; c <= 4; c++)
    {
        if (st->blockDC[c])
        {
            PLUTO_FREE(st->blockDC[c]);
            st->blockDC[c] = NULL;
        }
        st->blockDCcap[c] = 0;
    }
}

/* Quantization tables (Lua parity): t[id][i+1] = i-th byte of the DQT payload
 * (i = 0-based zigzag position). [1] = DC, [zz+1] = quant for 0-based zz.
 * 65 slots so index 64 exists (Lua tables have no upper bound issue). */
typedef struct
{
    uint16_t t[4][65];
} JpegQt;

/* ── Baseline scan decode (streaming, block rows → downscaler) ───────────── */
typedef struct
{
    ScaleAccum *acc;
    int ended;
    size_t pos;
} JpegBaseResult;

static void jpeg_baseline_store_row_rowbuf(uint8_t **rowBuf, int maxRows, int y, int px,
                                           const int *vals8, int flat)
{
    /* rowBuf[y] lazily allocated (Lua rowBuf[py+ri] lazy tables), 0-based y. */
    if (y < 0 || y >= maxRows)
    {
        return;
    }
    uint8_t *rb = rowBuf[y];
    if (!rb)
    {
        rb = (uint8_t *)PLUTO_MALLOC(4096); /* one row: up to 4096 px wide */
        if (!rb)
        {
            return;
        }
        memset(rb, 0, 4096);
        rowBuf[y] = rb;
    }
    if (flat >= 0)
    {
        uint8_t g = (uint8_t)flat;
        memset(rb + px, g, 8);
    }
    else
    {
        for (int cx = 0; cx < 8; cx++)
        {
            rb[px + cx] = (uint8_t)vals8[cx];
        }
    }
}

static JpegBaseResult jpeg_decode_baseline(const uint8_t *data, size_t len, size_t entropyPos,
                                           const JpegFrame *frame, const JpegScan *scan,
                                           const JpegHuff *dcTables, const JpegHuff *acTables,
                                           const JpegQt *qt, int restartInterval,
                                           int maxW, int maxH)
{
    JpegBaseResult res = { NULL, 0, entropyPos };
    int width = frame->width, height = frame->height;
    int ns = scan->ns;

    /* Sampling factors for this scan. scan->comps is Lua-1-based ([1..ns],
     * slot 0 unused) — iterating it 0-based read garbage (frame->comps[-1])
     * and collapsed every interleaved image to one MCU. */
    int sMaxH = 1, sMaxV = 1;
    for (int i = 1; i <= ns; i++)
    {
        const JpegComp *fc = &frame->comps[scan->comps[i] - 1];
        if (fc->h > sMaxH) sMaxH = fc->h;
        if (fc->v > sMaxV) sMaxV = fc->v;
    }
    if (ns == 1) { sMaxH = 1; sMaxV = 1; } /* non-interleaved */

    int mcuCols = (width + sMaxH * 8 - 1) / (sMaxH * 8);
    int mcuRows = (height + sMaxV * 8 - 1) / (sMaxV * 8);
    if (mcuCols < 1) mcuCols = 1;
    if (mcuRows < 1) mcuRows = 1;

    JpegReader rd;
    memset(&rd, 0, sizeof(rd));
    rd.str = data;
    rd.len = len;
    rd.pos = entropyPos;

#ifdef P25_DEBUG_DUMP
    fprintf(stderr, "JPEGDBG GEOM w=%d h=%d ns=%d sMaxH=%d sMaxV=%d mcuCols=%d mcuRows=%d dcOnly=%d\n",
            width, height, ns, sMaxH, sMaxV, mcuCols, mcuRows,
            (int)((long)width * height > 200000));
#endif

    res.acc = scale_accum_new(width, height, maxW, maxH);
    if (!res.acc)
    {
        return res;
    }

    /* rowBuf: one pointer per source row (Lua lazy tables) */
    int maxRows = height;
    uint8_t **rowBuf = (uint8_t **)PLUTO_MALLOC(sizeof(uint8_t *) * (size_t)maxRows);
    if (!rowBuf)
    {
        scale_accum_free(res.acc);
        res.acc = NULL;
        return res;
    }
    memset(rowBuf, 0, sizeof(uint8_t *) * (size_t)maxRows);

    int64_t block[64];
    int64_t tmp[64];
    int64_t dcPred[4];
    for (int i = 0; i < 4; i++) dcPred[i] = 0;

    int boxW = 1, boxH = 1, tw = 1, th = 1;
    scale_box_sizes(width, height, maxW, maxH, &boxW, &boxH, &tw, &th);
    int dcOnly = (boxW >= 4) || (boxH >= 4) || ((long)width * height > 200000);

    int mcuIndex = 0;
    int ended = 0;

    for (int mcuY = 0; mcuY < mcuRows && !ended; mcuY++)
    {
        for (int mcuX = 0; mcuX < mcuCols; mcuX++)
        {
            if (restartInterval > 0 && mcuIndex > 0 && (mcuIndex % restartInterval) == 0)
            {
                if (!jpeg_expect_restart(&rd, mcuIndex))
                {
                    ended = 1;
                    break;
                }
                for (int i = 0; i < 4; i++) dcPred[i] = 0;
            }

            for (int ci = 1; ci <= ns && !ended; ci++)
            {
                /* scan->comps is Lua-1-based ([1..ns], slot 0 unused);
                 * dcTbl/acTbl are stored 0-based. */
                const JpegComp *fc = &frame->comps[scan->comps[ci] - 1];
                const JpegHuff *dcTbl = &dcTables[scan->dcTbl[ci - 1] & 0x03];
                const JpegHuff *acTbl = &acTables[scan->acTbl[ci - 1] & 0x03];
                const uint16_t *qtz = qt->t[fc->qt & 0x03];
                for (int bj = 0; bj < fc->v && !ended; bj++)
                {
                    for (int bi = 0; bi < fc->h && !ended; bi++)
                    {
                        int diff = jpeg_decode_dc(&rd, dcTbl);
                        if (rd.eof)
                        {
#ifdef P25_DEBUG_DUMP
                            fprintf(stderr, "JPEGDBG END dc-eof mcuX=%d ci=%d bi=%d bj=%d pos=%zu\n", mcuX, ci, bi, bj, rd.pos);
#endif
                            ended = 1;
                            break;
                        }
                        dcPred[ci - 1] += diff;
                        int dc = dcPred[ci - 1];
                        int px = mcuX * sMaxH * 8 + bi * 8;
                        int py = mcuY * sMaxV * 8 + bj * 8;

                        if (ci == 1)
                        {
                            /* Luma (Y): fill row buffer */
                            if (dcOnly)
                            {
                                /* consume AC symbols for stream sync, DC only */
                                memset(block, 0, 64 * sizeof(int64_t));
                                if (!jpeg_decode_ac(&rd, acTbl, block, qtz))
                                {
#ifdef P25_DEBUG_DUMP
                                    fprintf(stderr, "JPEGDBG END dconly-ac mcuX=%d ci=%d bi=%d bj=%d pos=%zu eof=%d\n", mcuX, ci, bi, bj, rd.pos, rd.eof);
#endif
                                    ended = 1;
                                    break;
                                }
                                int g = jpeg_clamp255((int)floor((double)dc * qtz[1] / 8.0 + 0.5) + 128);
                                for (int ri = 0; ri < 8; ri++)
                                {
                                    jpeg_baseline_store_row_rowbuf(rowBuf, maxRows, py + ri, px, NULL, g);
                                }
                            }
                            else
                            {
                                block[0] = dc * (int64_t)qtz[1];
                                for (int i = 1; i < 64; i++) block[i] = 0;
                                if (!jpeg_decode_ac(&rd, acTbl, block, qtz))
                                {
#ifdef P25_DEBUG_DUMP
                                    fprintf(stderr, "JPEGDBG END full-ac mcuX=%d ci=%d bi=%d bj=%d pos=%zu eof=%d\n", mcuX, ci, bi, bj, rd.pos, rd.eof);
#endif
                                    ended = 1;
                                    break;
                                }
#ifdef P25_DEBUG_DUMP
                                fprintf(stderr, "JPEGDBG block mcuX=%d bi=%d bj=%d ci=%d:", mcuX, bi, bj, ci);
                                for (int zz = 0; zz < 64; zz++) fprintf(stderr, " %lld", (long long)block[zz]);
                                fprintf(stderr, "\n");
#endif
                                jpeg_idct2d(block, tmp);
                                for (int ri = 0; ri < 8; ri++)
                                {
                                    int vals8[8];
                                    for (int cx = 0; cx < 8; cx++)
                                    {
                                        vals8[cx] = jpeg_clamp255(
                                            (int)floor((double)block[ri * 8 + cx] / JPEG_IDCT_SCALE + 0.5) + 128);
                                    }
                                    jpeg_baseline_store_row_rowbuf(rowBuf, maxRows, py + ri, px, vals8, -1);
                                }
                            }
                        }
                        else
                        {
                            /* Chroma: consume bits for sync, discard */
                            memset(block, 0, 64 * sizeof(int64_t));
                            if (!jpeg_decode_ac(&rd, acTbl, block, qtz))
                            {
#ifdef P25_DEBUG_DUMP
                                fprintf(stderr, "JPEGDBG END chroma-ac mcuX=%d ci=%d bi=%d bj=%d pos=%zu eof=%d\n", mcuX, ci, bi, bj, rd.pos, rd.eof);
#endif
                                ended = 1;
                                break;
                            }
                        }
                    }
                }
            }

            mcuIndex++;
        }

        /* Feed completed block rows to the downscaler (also when ended, so
         * the final MCU row's Y rows are not lost). */
        int base = mcuY * sMaxV * 8;
        for (int ri = 0; ri < sMaxV * 8; ri++)
        {
            int y = base + ri;
            if (y < height)
            {
                scale_accum_add_row(res.acc, rowBuf[y]); /* NULL row = no-op */
            }
        }
    }

    for (int y = 0; y < maxRows; y++)
    {
        if (rowBuf[y])
        {
            PLUTO_FREE(rowBuf[y]);
        }
    }
    PLUTO_FREE(rowBuf);

    res.ended = ended;
    res.pos = rd.pos;
    return res;
}

/* ── Progressive: decode DC scans only (memory-safe, block averages) ─────── */
/* Returns 1 on success, 0 on truncation. */
static int jpeg_decode_prog_dc(const uint8_t *data, size_t len, size_t entropyPos,
                               const JpegFrame *frame, const JpegScan *scan,
                               const JpegHuff *dcTables, const JpegQt *qt,
                               int restartInterval, JpegProgState *state)
{
    (void)qt;
    int width = frame->width, height = frame->height;
    int ns = scan->ns;

    /* Frame-level sampling factors define every component's block grid. */
    int maxHf = 1, maxVf = 1;
    for (int i = 0; i < frame->ncomp; i++)
    {
        if (frame->comps[i].h > maxHf) maxHf = frame->comps[i].h;
        if (frame->comps[i].v > maxVf) maxVf = frame->comps[i].v;
    }
    int mcuColsF = (width + maxHf * 8 - 1) / (maxHf * 8);
    int mcuRowsF = (height + maxVf * 8 - 1) / (maxVf * 8);
    if (mcuColsF < 1) mcuColsF = 1;
    if (mcuRowsF < 1) mcuRowsF = 1;

    /* Non-interleaved scan: each block is its own MCU. */
    int mcuCols, mcuRows;
    if (ns == 1)
    {
        const JpegComp *fc = &frame->comps[scan->comps[0] - 1];
        mcuCols = mcuColsF * fc->h;
        mcuRows = mcuRowsF * fc->v;
    }
    else
    {
        mcuCols = mcuColsF;
        mcuRows = mcuRowsF;
    }

    JpegReader rd;
    memset(&rd, 0, sizeof(rd));
    rd.str = data;
    rd.len = len;
    rd.pos = entropyPos;

    int refinement = (scan->ah != 0);
    int mcuIndex = 0;
    int ended = 0;

    for (int mcuY = 0; mcuY < mcuRows && !ended; mcuY++)
    {
        for (int mcuX = 0; mcuX < mcuCols && !ended; mcuX++)
        {
            if (restartInterval > 0 && mcuIndex > 0 && (mcuIndex % restartInterval) == 0)
            {
                if (!jpeg_expect_restart(&rd, mcuIndex))
                {
                    ended = 1;
                    break;
                }
                for (int i = 1; i <= ns; i++)
                {
                    state->dcPred[scan->comps[i]] = 0;
                }
            }

            for (int ci = 1; ci <= ns && !ended; ci++)
            {
                int c = scan->comps[ci];
                const JpegComp *fc = &frame->comps[c - 1];
                const JpegHuff *dcTbl = &dcTables[scan->dcTbl[ci - 1] & 0x03];

                int blocks = (ns == 1) ? 1 : fc->h * fc->v;
                for (int bi = 0; bi < blocks; bi++)
                {
                    int idx = state->blkIdx[c];
                    state->blkIdx[c] = idx + 1;
                    if (!jpeg_prog_ensure(state, c, idx + 1))
                    {
                        ended = 1;
                        break;
                    }
                    if (refinement)
                    {
                        int bit = jpeg_rd_bits(&rd, 1);
                        if (bit < 0)
                        {
                            ended = 1;
                            break;
                        }
                        int dc = state->blockDC[c][idx];
                        if (bit != 0)
                        {
                            state->blockDC[c][idx] = dc + (1 << scan->al);
                        }
                        else
                        {
                            state->blockDC[c][idx] = dc - (1 << scan->al);
                        }
                    }
                    else
                    {
                        int diff = jpeg_decode_dc(&rd, dcTbl);
                        if (rd.eof)
                        {
                            ended = 1;
                            break;
                        }
                        state->dcPred[c] += diff;
                        state->blockDC[c][idx] = state->dcPred[c] << scan->al;
                    }
                }
            }
            mcuIndex++;
        }
    }

    int ok = !ended;
    return ok;
}

/* Turn stored DC coefficients into a downscaled grayscale grid.
 * MCU-row scratch: blocks within one MCU row write different py rows, so
 * fill an (sMaxV*8) x rowW scratch per MCU row, then feed each source row
 * (y < height) to the streaming accumulator — exactly the reference's
 * rowBuf[py+ri] lazy-table pattern, minus the per-pixel Lua tables. */
static ScaleAccum *jpeg_render_prog_dc(const JpegFrame *frame, const JpegProgState *state,
                                       int maxW, int maxH, const JpegQt *qt)
{
    int width = frame->width, height = frame->height;
    int yc = 1; /* luma is component index 1 */

    int sMaxH = 1, sMaxV = 1;
    for (int i = 0; i < frame->ncomp; i++)
    {
        if (frame->comps[i].h > sMaxH) sMaxH = frame->comps[i].h;
        if (frame->comps[i].v > sMaxV) sMaxV = frame->comps[i].v;
    }
    int mcuCols = (width + sMaxH * 8 - 1) / (sMaxH * 8);
    int mcuRows = (height + sMaxV * 8 - 1) / (sMaxV * 8);
    if (mcuCols < 1) mcuCols = 1;
    if (mcuRows < 1) mcuRows = 1;

    ScaleAccum *acc = scale_accum_new(width, height, maxW, maxH);
    if (!acc)
    {
        return NULL;
    }

    int rowW = mcuCols * sMaxH * 8;
    if (rowW < width) rowW = width;
    int scratchH = sMaxV * 8;
    uint8_t *scratch = (uint8_t *)PLUTO_MALLOC((size_t)rowW * (size_t)scratchH);
    if (!scratch)
    {
        scale_accum_free(acc);
        return NULL;
    }

    const int64_t *blockDC = state->blockDC[yc];
    int blockDCcap = state->blockDCcap[yc];
    int yfc_h = frame->comps[yc - 1].h;
    int yfc_v = frame->comps[yc - 1].v;
    const uint16_t *qtz = qt->t[frame->comps[yc - 1].qt & 0x03];
    int qdc = qtz ? qtz[1] : 1;
    int idx = 0;

    for (int mcuY = 0; mcuY < mcuRows; mcuY++)
    {
        memset(scratch, 0, (size_t)rowW * (size_t)scratchH);
        for (int mcuX = 0; mcuX < mcuCols; mcuX++)
        {
            for (int bj = 0; bj < yfc_v; bj++)
            {
                for (int bi = 0; bi < yfc_h; bi++)
                {
                    int64_t dc = (blockDC && idx < blockDCcap) ? blockDC[idx] : 0;
                    idx++;
                    int px = mcuX * sMaxH * 8 + bi * 8;
                    int py = mcuY * sMaxV * 8 + bj * 8;
                    int g = jpeg_clamp255((int)floor((double)dc * qdc / 8.0 + 0.5) + 128);
                    /* Lua fills rowBuf[py+ri][px+cx+1] = g for ri = 0..7 —
                     * every block paints ALL 8 rows of its 8x8 cell with the
                     * flat DC value (py beyond height still consumed idx for
                     * parity but never reaches the downscaler). */
                    for (int ri = 0; ri < 8; ri++)
                    {
                        int y = py + ri;
                        if (y >= height)
                        {
                            break; /* rows beyond height never reach the scaler */
                        }
                        int srow = y - mcuY * sMaxV * 8;
                        if (srow >= 0 && srow < scratchH)
                        {
                            uint8_t *dst = scratch + (size_t)srow * rowW + px;
                            int lim = px + 8 < rowW ? 8 : rowW - px;
                            for (int cx = 0; cx < lim; cx++)
                            {
                                dst[cx] = (uint8_t)g;
                            }
                        }
                    }
                }
            }
        }

        /* Feed completed block rows to the downscaler. */
        int base = mcuY * sMaxV * 8;
        for (int ri = 0; ri < scratchH; ri++)
        {
            int y = base + ri;
            if (y < height)
            {
                scale_accum_add_row(acc, scratch + (size_t)ri * rowW);
            }
        }
    }

    PLUTO_FREE(scratch);
    return acc;
}

/* ── Main decoder ─────────────────────────────────────────────────────────── */

/* Read a 16-bit big-endian length at data[pos]; 0 on underflow. */
static int jpeg_read_u16(const uint8_t *data, size_t len, size_t pos)
{
    if (pos + 1 >= len)
    {
        return 0;
    }
    return (data[pos] << 8) | data[pos + 1];
}

uint8_t **jpeg_decode_gray(const uint8_t *data, size_t len,
                           int maxW, int maxH,
                           int *outCount, int *outWidth)
{
    *outCount = 0;
    *outWidth = 0;
    if (!data || len < 4)
    {
        return NULL;
    }
    if (data[0] != 0xFF || data[1] != 0xD8)
    {
        return NULL; /* not a JPEG (SOI check, Lua parity) */
    }
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    jpeg_idct_init();

    size_t pos = 2; /* Lua pos = 3 (1-based) == index 2 (0-based) */
    JpegFrame frame;
    memset(&frame, 0, sizeof(frame));
    int haveFrame = 0;
    JpegQt qt;
    memset(&qt, 0, sizeof(qt));
    JpegHuff dcTables[4];
    JpegHuff acTables[4];
    memset(dcTables, 0, sizeof(dcTables));
    memset(acTables, 0, sizeof(acTables));
    int haveDc[4] = {0, 0, 0, 0};
    int haveAc[4] = {0, 0, 0, 0};
    int restartInterval = 0;
    int progressive = 0;
    JpegProgState state;
    memset(&state, 0, sizeof(state));

    ScaleAccum *acc = NULL;

    while (pos + 1 < len)
    {
        if (data[pos] != 0xFF)
        {
            break;
        }
        int m = data[pos + 1];
        pos += 2;

        if (m == 0xD9)
        {
            break; /* EOI */
        }
        else if (m == 0xDB) /* DQT */
        {
            int segLen = jpeg_read_u16(data, len, pos);
            if (segLen < 2)
            {
                break;
            }
            size_t segEnd = pos + (size_t)segLen;
            size_t p = pos + 2;
            while (p < segEnd && p < len)
            {
                int pq = data[p];
                p++;
                int id = pq & 0x0F;
                if (id > 3)
                {
                    p = segEnd;
                    break;
                }
                for (int i = 0; i < 64; i++)
                {
                    if ((pq >> 4) == 0)
                    {
                        qt.t[id][i + 1] = (p < len) ? data[p] : 1;
                        p++;
                    }
                    else
                    {
                        qt.t[id][i + 1] = (uint16_t)jpeg_read_u16(data, len, p);
                        p += 2;
                    }
                }
            }
            pos = segEnd;
        }
        else if (m == 0xC4) /* DHT */
        {
            int segLen = jpeg_read_u16(data, len, pos);
            if (segLen < 2)
            {
                break;
            }
            size_t segEnd = pos + (size_t)segLen;
            size_t p = pos + 2;
            while (p < segEnd && p < len)
            {
                int tc = data[p];
                p++;
                uint8_t counts[16];
                int total = 0;
                for (int i = 0; i < 16; i++)
                {
                    counts[i] = (p < len) ? data[p] : 0;
                    p++;
                    total += counts[i];
                }
                uint8_t values[256];
                if (total > 256)
                {
                    total = 256;
                }
                for (int i = 0; i < total; i++)
                {
                    values[i] = (p < len) ? data[p] : 0;
                    p++;
                }
                int cls = (tc >> 4) & 1;
                int id = tc & 0x0F;
                if (id <= 3)
                {
                    if (cls == 0)
                    {
                        jpeg_build_huff(&dcTables[id], counts, values, total);
                        haveDc[id] = 1;
                    }
                    else
                    {
                        jpeg_build_huff(&acTables[id], counts, values, total);
                        haveAc[id] = 1;
                    }
                }
            }
            pos = segEnd;
        }
        else if (m == 0xC0 || m == 0xC2) /* SOF0 / SOF2 */
        {
            int segLen = jpeg_read_u16(data, len, pos);
            size_t p = pos + 2; /* skip length (Lua p = pos+2) */
            frame.precision = (p < len) ? data[p] : 8;
            frame.height = jpeg_read_u16(data, len, p + 1);
            frame.width = jpeg_read_u16(data, len, p + 3);
            frame.ncomp = (p + 5 < len) ? data[p + 5] : 0;
            if (frame.ncomp > 4) frame.ncomp = 4;
            p += 6;
            for (int i = 0; i < frame.ncomp; i++)
            {
                frame.comps[i].id = (p < len) ? data[p] : 0;
                int samp = (p + 1 < len) ? data[p + 1] : 0x11;
                frame.comps[i].qt = (p + 2 < len) ? data[p + 2] : 0;
                frame.comps[i].h = samp >> 4;
                frame.comps[i].v = samp & 0x0F;
                p += 3;
            }
            haveFrame = 1;
            progressive = (m == 0xC2);
            for (int i = 0; i < 5; i++)
            {
                state.blkIdx[i] = 0;
            }
            pos = pos + (size_t)segLen;
        }
        else if (m == 0xDD) /* DRI */
        {
            restartInterval = jpeg_read_u16(data, len, pos + 2);
            int segLen = jpeg_read_u16(data, len, pos);
            pos = pos + (size_t)segLen;
        }
        else if (m == 0xDA) /* SOS */
        {
            if (!haveFrame)
            {
                break;
            }
            int segLen = jpeg_read_u16(data, len, pos);
            JpegScan scan;
            memset(&scan, 0, sizeof(scan));
            scan.ns = (pos + 2 < len) ? data[pos + 2] : 0;
            if (scan.ns > 4) scan.ns = 4;
            size_t p = pos + 3;
            for (int i = 1; i <= scan.ns; i++)
            {
                int cid = (p < len) ? data[p] : 0;
                int tbls = (p + 1 < len) ? data[p + 1] : 0;
                int ci = 0;
                for (int j = 1; j <= frame.ncomp; j++)
                {
                    if (frame.comps[j - 1].id == cid)
                    {
                        ci = j;
                        break;
                    }
                }
                if (ci == 0)
                {
                    ci = i; /* Lua `ci = ci or i` */
                }
                scan.comps[i] = ci;
                scan.dcTbl[i - 1] = tbls >> 4;
                scan.acTbl[i - 1] = tbls & 0x0F;
                p += 2;
            }
            scan.ss = (p < len) ? data[p] : 0;
            scan.se = (p + 1 < len) ? data[p + 1] : 63;
            int ahal = (p + 2 < len) ? data[p + 2] : 0;
            scan.ah = ahal >> 4;
            scan.al = ahal & 0x0F;

            size_t entropyPos = pos + (size_t)segLen;

            if (progressive)
            {
                if (scan.ss == 0 && scan.se == 0)
                {
                    int ok = jpeg_decode_prog_dc(data, len, entropyPos, &frame, &scan,
                                                 dcTables, &qt, restartInterval, &state);
                    /* Lua: pos = nextPos even on truncation, then breaks. */
                    (void)ok;
                    if (!ok)
                    {
                        break; /* truncated */
                    }
                    /* Continue after the scan: skip past its entropy data is
                     * not possible without parsing (markers are inside the
                     * entropy stream); the Lua reference simply continues the
                     * marker loop from reader position — we do the same by
                     * scanning forward for the next FF xx marker. */
                    size_t q = entropyPos;
                    while (q + 1 < len)
                    {
                        if (data[q] == 0xFF && data[q + 1] != 0x00 &&
                            !(data[q + 1] >= 0xD0 && data[q + 1] <= 0xD7))
                        {
                            break;
                        }
                        q++;
                    }
                    pos = q;
                }
                else
                {
                    /* AC scan: we have all DCs — render and stop. */
                    if (acc == NULL)
                    {
                        acc = jpeg_render_prog_dc(&frame, &state, maxW, maxH, &qt);
                    }
                    break;
                }
            }
            else
            {
                JpegBaseResult res = jpeg_decode_baseline(data, len, entropyPos, &frame, &scan,
                                                          dcTables, acTables, &qt,
                                                          restartInterval, maxW, maxH);
                acc = res.acc;
                res.acc = NULL; /* ownership moved */
                pos = res.pos;
                if (res.ended)
                {
                    break;
                }
            }
        }
        else
        {
            /* Other segment: skip by length. */
            int segLen = jpeg_read_u16(data, len, pos);
            if (segLen < 2)
            {
                break;
            }
            pos = pos + (size_t)segLen;
        }
    }

    if (acc == NULL && haveFrame)
    {
        /* Progressive with no AC scans reached yet: render DCs. */
        acc = jpeg_render_prog_dc(&frame, &state, maxW, maxH, &qt);
    }

    jpeg_prog_free(&state);

    if (!acc)
    {
        return NULL;
    }

    int rows = 0, rowW = 0;
    uint8_t **grid = scale_accum_finish(acc, &rows, &rowW);
    if (!grid || rows == 0) /* Lua: acc.count == 0 → nil */
    {
        scale_accum_free(acc);
        return NULL;
    }

    /* Lua hands Dither.toImage (targetW, targetH) from Scale.boxSizes and
     * reads rows[y] (missing -> 255) — so a trailing PARTIAL accumulator row
     * (emitted by finish() when srcH % boxH != 0) exists in rows[] but is
     * never shown: toImage iterates only y < targetH. Clip to targetH here
     * for exact parity (tc3: 64 rows in, boxH=6 -> 10 full + 1 partial ->
     * grid is 16x10, not 16x11). */
    {
        int twc, thc;
        scale_box_sizes(frame.width, frame.height, maxW, maxH, NULL, NULL, &twc, &thc);
        if (rows > thc)
        {
            rows = thc;
        }
    }

    /* Detach the grid from the accumulator so it survives scale_accum_free:
     * finish() returns acc-owned pointers; move them to a caller-owned array
     * with a NUL sentinel so jpeg_gray_free can walk it without a count. */
    uint8_t **out = (uint8_t **)PLUTO_MALLOC(sizeof(uint8_t *) * (size_t)(rows + 1));
    if (!out)
    {
        scale_accum_free(acc);
        return NULL;
    }
    for (int i = 0; i < rows; i++)
    {
        out[i] = grid[i];
        grid[i] = NULL; /* the accumulator frees only non-NULL rows */
    }
    out[rows] = NULL;
    scale_accum_free(acc);

    *outCount = rows;
    *outWidth = rowW;
    return out;
}

void jpeg_gray_free(uint8_t **rows)
{
    if (!rows)
    {
        return;
    }
    for (int i = 0; rows[i]; i++)
    {
        PLUTO_FREE(rows[i]);
    }
    PLUTO_FREE(rows);
}

/* Output-pixel closure state (Lua: rows[y] and r[x+1] or 255). */
typedef struct
{
    uint8_t **rows;
    int outCount;
    int outWidth;
} JpegDitherCtx;

static uint8_t jpeg_dither_pixel(void *ud, int x, int y)
{
    JpegDitherCtx *c = (JpegDitherCtx *)ud;
    if (y < 0 || y >= c->outCount)
    {
        return 255;
    }
    const uint8_t *r = c->rows[y];
    if (!r || x < 0 || x >= c->outWidth)
    {
        return 255;
    }
    return r[x];
}

LCDBitmap *jpeg_decode(const uint8_t *data, size_t len, int maxW, int maxH)
{
    int rows = 0, rowW = 0;
    uint8_t **grid = jpeg_decode_gray(data, len, maxW, maxH, &rows, &rowW);
    if (!grid || rows == 0 || rowW <= 0)
    {
        if (grid)
        {
            jpeg_gray_free(grid);
        }
        return NULL;
    }

    /* int targetH needed for the dither; scale_box_sizes recomputes it. */
    int boxW, boxH, targetW, targetH;
    /* Recover source dims: scale.c stores them in the accumulator, but the
     * public seam recomputes the box — same math (floor(src/box), min 1). */
    /* targetW == rowW; targetH == rows. */
    (void)boxW; (void)boxH;
    targetW = rowW;
    targetH = rows;

    JpegDitherCtx dc = { grid, rows, rowW };
    LCDBitmap *img = dither_to_bitmap(targetW, targetH, jpeg_dither_pixel, &dc);
    jpeg_gray_free(grid);
    return img;
}
