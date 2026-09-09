/*
 * PlutoBrowser — tif.c
 * Minimal TIFF decoder (see tif.h for the supported matrix).
 *
 * Design notes:
 *   - Tags are parsed into a small table; values are read through
 *     tif_tag_u32 (handles BYTE/SHORT/LONG inline or at offset).
 *   - LZW decoder: standard TIFF variant (early change = 1 code earlier,
 *     code width bumps at 511/1023/2047, clear=256, EOI=257).
 *   - PackBits: per-row Apple RLE (n>=128 -> repeat next byte 257-n times,
 *     n<128 -> copy n+1 literal bytes, n==128 -> noop).
 *   - CCITT G3 1D: white-run/code tables ( terminating + makeup ), black run
 *     codes, EOL tolerance, 1 = black in the decoded bitstream (photometric
 *     0 flips to white-is-zero afterwards).
 *   - Rows expand to 8-bit gray via palette or direct gray, then the usual
 *     dither_to_bitmap finishes.
 */
#include <string.h>
#include <stdlib.h>
#include "tif.h"
#include "dither.h"
#include "scale.h"
#include "../../core/logger.h"


typedef struct
{
    const uint8_t *data;
    size_t len;
    int bo; /* 0 = little (II), 1 = big (MM) */
    int width, height;
    int compression;     /* 1 none, 5 LZW, 32773 PackBits, 2 G3 */
    int photometric;     /* 0 white0, 1 black0, 2 RGB, 3 palette */
    int bps;             /* bits per sample (1 or 8) */
    int samples;         /* samples per pixel */
    int predictor;       /* 1 none, 2 horizontal differencing (bps 8) */
    uint32_t stripOff;   /* first strip offset */
    uint32_t stripBytes; /* first strip byte count */
    uint16_t pal[256][3];
    int scaleNum, scaleDen;
} TifCtx;

static uint32_t tif_u16(const uint8_t *d, size_t off, size_t len, TifCtx *c)
{
    if (off + 2 > len) return 0;
    return c->bo ? (uint32_t)((d[off] << 8) | d[off + 1])
                 : (uint32_t)((d[off + 1] << 8) | d[off]);
}

static uint32_t tif_u32(const uint8_t *d, size_t off, size_t len, TifCtx *c)
{
    if (off + 4 > len) return 0;
    if (c->bo)
        return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
               ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
    return ((uint32_t)d[off + 3] << 24) | ((uint32_t)d[off + 2] << 16) |
           ((uint32_t)d[off + 1] << 8) | (uint32_t)d[off];
}

/* ── LZW (TIFF variant) ──────────────────────────────────────────────────── */

typedef struct
{
    const uint8_t *in;
    size_t inLen;
    size_t bitPos;
    uint16_t dict[4096][2]; /* [prefix, lastByte] */
    int dictLen;
    int codeWidth;
    int earlyChange; /* TIFF: 1 */
    uint8_t *out;
    size_t outCap, outLen;
    int eof;
} LzwState;

static int lzw_bits(LzwState *s, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++)
    {
        size_t byteIdx = s->bitPos >> 3;
        if (byteIdx >= s->inLen)
        {
            s->eof = 1;
            return -1;
        }
        int bit = (s->in[byteIdx] >> (7 - (s->bitPos & 7))) & 1;
        acc = (acc << 1) | (uint32_t)bit;
        s->bitPos++;
    }
    return (int)acc;
}

static int lzw_out(LzwState *s, uint8_t byte)
{
    if (s->outLen >= s->outCap) return 0;
    s->out[s->outLen++] = byte;
    return 1;
}

/* Emit the string for `code` (recursively via stack walk) and set
 * firstByte. Returns length emitted or -1. */
static uint8_t lzw_rev[4096]; /* static: 4KB exceeds the device stack budget */

static int lzw_emit(LzwState *s, int code, int *firstByte)
{
    /* max expansion is bounded; walk the prefix chain into a static buffer */
    uint8_t *rev = lzw_rev;
    int n = 0;
    int fb = -1;
    int c = code;
    while (c >= 256)
    {
        if (c >= 4096) return -1;
        if (n >= 4096) return -1;
        rev[n++] = s->dict[c][1];
        c = s->dict[c][0];
        if (c < 0) return -1;
    }
    if (n >= 4096) return -1;
    rev[n++] = (uint8_t)c;
    fb = c;
    while (n > 0)
    {
        if (!lzw_out(s, rev[--n])) return -1;
    }
    if (firstByte) *firstByte = fb;
    return 0;
}

static LzwState lzw_state; /* static: 16KB dict far exceeds the device stack */

static int lzw_decode(const uint8_t *in, size_t inLen, uint8_t *out,
                      size_t outCap, size_t *outLen)
{
    LzwState *s = &lzw_state;
    memset(s, 0, sizeof(*s));
    s->in = in;
    s->inLen = inLen;
    s->out = out;
    s->outCap = outCap;
    s->codeWidth = 9;
    s->earlyChange = 1;
    for (int i = 0; i < 256; i++)
    {
        s->dict[i][0] = 0xFFFF;
        s->dict[i][1] = (uint16_t)i;
    }
    s->dictLen = 258; /* 256=clear, 257=EOI */

    int prev = -1;
    while (!s->eof)
    {
        int code = lzw_bits(s, s->codeWidth);
        if (code < 0) break;
        if (code == 256)
        {
            s->codeWidth = 9;
            s->dictLen = 258;
            prev = -1;
            continue;
        }
        if (code == 257) break;

        int fb = -1;
        if (code < s->dictLen)
        {
            if (lzw_emit(s, code, &fb) != 0) break;
            if (prev >= 0 && s->dictLen < 4096)
            {
                s->dict[s->dictLen][0] = (uint16_t)prev;
                s->dict[s->dictLen][1] = (uint16_t)fb;
                s->dictLen++;
            }
        }
        else /* code == dictLen: the KwKwK case */
        {
            if (prev < 0) break;
            if (lzw_emit(s, prev, &fb) != 0) break;
            if (!lzw_out(s, (uint8_t)fb)) break;
            if (s->dictLen < 4096)
            {
                s->dict[s->dictLen][0] = (uint16_t)prev;
                s->dict[s->dictLen][1] = (uint16_t)fb;
                s->dictLen++;
            }
        }

        int limit = (1 << s->codeWidth) - s->earlyChange;
        if (s->dictLen >= limit && s->codeWidth < 12)
        {
            s->codeWidth++;
        }
        prev = code;
    }
    *outLen = s->outLen;
    return s->outLen > 0 ? 0 : -1;
}

/* ── PackBits ────────────────────────────────────────────────────────────── */

static size_t packbits_decode(const uint8_t *in, size_t inLen, uint8_t *out,
                              size_t outCap)
{
    size_t ip = 0, op = 0;
    while (ip < inLen && op < outCap)
    {
        int n = (int8_t)in[ip++];
        if (n >= 0)
        {
            size_t cnt = (size_t)n + 1;
            if (ip + cnt > inLen || op + cnt > outCap) break;
            memcpy(out + op, in + ip, cnt);
            ip += cnt;
            op += cnt;
        }
        else if (n != -128)
        {
            size_t cnt = (size_t)(257 - (size_t)(n & 0xFF));
            if (ip >= inLen) break;
            uint8_t v = in[ip++];
            if (op + cnt > outCap) cnt = outCap - op;
            memset(out + op, v, cnt);
            op += cnt;
        }
        /* -128: no-op */
    }
    return op;
}

/* ── CCITT Group 3, 1D ───────────────────────────────────────────────────── */

typedef struct
{
    uint16_t code;
    uint8_t bits;
    uint16_t run;
} G3Code;

/* Terminating codes 0..63 (white). From ITU T.4. */
/* T.4 code tables — white & black, terminating (0-63) + makeup (64-1728)
 * + extended makeup (1792-2560). 104 entries each. Data sourced from FFmpeg's
 * libavcodec/faxcompr.c (ccitt_codes_bits/lens), which matches libtiff and the
 * ITU T.4 spec. Codes are stored left-aligned: the top `bits` bits of `code`
 * are the MSB-first bit pattern. */
static const G3Code G3_WHITE[] = {
    {0x35,8,0},{0x07,6,1},{0x07,4,2},{0x08,4,3},{0x0B,4,4},{0x0C,4,5},
    {0x0E,4,6},{0x0F,4,7},{0x13,5,8},{0x14,5,9},{0x07,5,10},{0x08,5,11},
    {0x08,6,12},{0x03,6,13},{0x34,6,14},{0x35,6,15},{0x2A,6,16},{0x2B,6,17},
    {0x27,7,18},{0x0C,7,19},{0x08,7,20},{0x17,7,21},{0x03,7,22},{0x04,7,23},
    {0x28,7,24},{0x2B,7,25},{0x13,7,26},{0x24,7,27},{0x18,7,28},{0x02,8,29},
    {0x03,8,30},{0x1A,8,31},{0x1B,8,32},{0x12,8,33},{0x13,8,34},{0x14,8,35},
    {0x15,8,36},{0x16,8,37},{0x17,8,38},{0x28,8,39},{0x29,8,40},{0x2A,8,41},
    {0x2B,8,42},{0x2C,8,43},{0x2D,8,44},{0x04,8,45},{0x05,8,46},{0x0A,8,47},
    {0x0B,8,48},{0x52,8,49},{0x53,8,50},{0x54,8,51},{0x55,8,52},{0x24,8,53},
    {0x25,8,54},{0x58,8,55},{0x59,8,56},{0x5A,8,57},{0x5B,8,58},{0x4A,8,59},
    {0x4B,8,60},{0x32,8,61},{0x33,8,62},{0x34,8,63},{0x1B,5,64},{0x12,5,128},
    {0x17,6,192},{0x37,7,256},{0x36,8,320},{0x37,8,384},{0x64,8,448},{0x65,8,512},
    {0x68,8,576},{0x67,8,640},{0xCC,9,704},{0xCD,9,768},{0xD2,9,832},{0xD3,9,896},
    {0xD4,9,960},{0xD5,9,1024},{0xD6,9,1088},{0xD7,9,1152},{0xD8,9,1216},{0xD9,9,1280},
    {0xDA,9,1344},{0xDB,9,1408},{0x98,9,1472},{0x99,9,1536},{0x9A,9,1600},{0x18,6,1664},
    {0x9B,9,1728},{0x08,11,1792},{0x0C,11,1856},{0x0D,11,1920},{0x12,12,1984},{0x13,12,2048},
    {0x14,12,2112},{0x15,12,2176},{0x16,12,2240},{0x17,12,2304},{0x1C,12,2368},{0x1D,12,2432},
    {0x1E,12,2496},{0x1F,12,2560}
};
static const G3Code G3_BLACK[] = {
    {0x37,10,0},{0x02,3,1},{0x03,2,2},{0x02,2,3},{0x03,3,4},{0x03,4,5},
    {0x02,4,6},{0x03,5,7},{0x05,6,8},{0x04,6,9},{0x04,7,10},{0x05,7,11},
    {0x07,7,12},{0x04,8,13},{0x07,8,14},{0x18,9,15},{0x17,10,16},{0x18,10,17},
    {0x08,10,18},{0x67,11,19},{0x68,11,20},{0x6C,11,21},{0x37,11,22},{0x28,11,23},
    {0x17,11,24},{0x18,11,25},{0xCA,12,26},{0xCB,12,27},{0xCC,12,28},{0xCD,12,29},
    {0x68,12,30},{0x69,12,31},{0x6A,12,32},{0x6B,12,33},{0xD2,12,34},{0xD3,12,35},
    {0xD4,12,36},{0xD5,12,37},{0xD6,12,38},{0xD7,12,39},{0x6C,12,40},{0x6D,12,41},
    {0xDA,12,42},{0xDB,12,43},{0x54,12,44},{0x55,12,45},{0x56,12,46},{0x57,12,47},
    {0x64,12,48},{0x65,12,49},{0x52,12,50},{0x53,12,51},{0x24,12,52},{0x37,12,53},
    {0x38,12,54},{0x27,12,55},{0x28,12,56},{0x58,12,57},{0x59,12,58},{0x2B,12,59},
    {0x2C,12,60},{0x5A,12,61},{0x66,12,62},{0x67,12,63},{0x0F,10,64},{0xC8,12,128},
    {0xC9,12,192},{0x5B,12,256},{0x33,12,320},{0x34,12,384},{0x35,12,448},{0x6C,13,512},
    {0x6D,13,576},{0x4A,13,640},{0x4B,13,704},{0x4C,13,768},{0x4D,13,832},{0x72,13,896},
    {0x73,13,960},{0x74,13,1024},{0x75,13,1088},{0x76,13,1152},{0x77,13,1216},{0x52,13,1280},
    {0x53,13,1344},{0x54,13,1408},{0x55,13,1472},{0x5A,13,1536},{0x5B,13,1600},{0x64,13,1664},
    {0x65,13,1728},{0x08,11,1792},{0x0C,11,1856},{0x0D,11,1920},{0x12,12,1984},{0x13,12,2048},
    {0x14,12,2112},{0x15,12,2176},{0x16,12,2240},{0x17,12,2304},{0x1C,12,2368},{0x1D,12,2432},
    {0x1E,12,2496},{0x1F,12,2560}
};
static const G3Code G3_EXT_MAKEUP[] = {
    {0x08,11,1792},{0x0C,11,1856},{0x0D,11,1920},{0x12,12,1984},
    {0x13,12,2048},{0x14,12,2112},{0x15,12,2176},{0x16,12,2240},
    {0x17,12,2304},{0x1C,12,2368},{0x1D,12,2432},{0x1E,12,2496},
    {0x1F,12,2560}
};

typedef struct
{
    const uint8_t *data;
    size_t len;
    size_t bitPos;
} G3Bit;

static int g3_bit(G3Bit *b)
{
    if (b->bitPos >= b->len * 8) return -1;
    int bit = (b->data[b->bitPos >> 3] >> (7 - (b->bitPos & 7))) & 1;
    b->bitPos++;
    return bit;
}

/* Match a code at bitPos. ffmpeg's table stores plain right-aligned code
 * values (e.g. white 2 = '0111' = 0x07), so compare acc directly. */
static int g3_match(const G3Bit *b, size_t bitPos, const G3Code *tab,
                    int n, int *run)
{
    uint32_t acc = 0;
    size_t maxBits = b->len * 8 - bitPos;
    for (int i = 0; i < 13; i++)
    {
        if ((size_t)i >= maxBits) break;
        int bit = (b->data[(bitPos + (size_t)i) >> 3] >>
                   (7 - ((bitPos + (size_t)i) & 7))) & 1;
        acc = (acc << 1) | (uint32_t)bit;
        for (int j = 0; j < n; j++)
        {
            if (tab[j].bits == i + 1 && (uint32_t)tab[j].code == acc)
            {
                *run = tab[j].run;
                return i + 1;
            }
        }
    }
    return 0;
}

/* Skip an EOL (000000000001) plus fill bits (zeros) that pad to a byte
 * boundary when `fill` is set. Returns 0 ok, -1 out of data. On failure the
 * bit position is restored to the entry value. */
static int g3_eol(G3Bit *b, int fill)
{
    size_t entryPos = b->bitPos;
    /* search for 000000000001 within the next ~200 bits (rows that decode
     * short leave residual bits before the next EOL + fill) */
    for (int i = 0; i < 200; i++)
    {
        if (b->bitPos + 12 > b->len * 8) return -1;
        /* check 11 zeros then 1 at current pos */
        int ok = 1;
        for (int k = 0; k < 11; k++)
        {
            int bit = (b->data[(b->bitPos + (size_t)k) >> 3] >>
                       (7 - ((b->bitPos + (size_t)k) & 7))) & 1;
            if (bit != 0) { ok = 0; break; }
        }
        if (ok)
        {
            int last = (b->data[(b->bitPos + 11) >> 3] >>
                        (7 - ((b->bitPos + 11) & 7))) & 1;
            if (last == 1)
            {
                b->bitPos += 12;
                if (fill)
                {
                    /* pad to byte boundary with zeros (already-read EOL keeps
                     * the stream byte-aligned afterwards) */
                    size_t pad = (8 - (b->bitPos & 7)) & 7;
                    for (size_t k = 0; k < pad; k++)
                    {
                        int bit = (b->data[(b->bitPos + k) >> 3] >>
                                   (7 - ((b->bitPos + k) & 7))) & 1;
                        if (bit != 0) break; /* not fill; data started */
                    }
                    b->bitPos = (b->bitPos + 7) & ~(size_t)7;
                }
                return 0;
            }
        }
        b->bitPos++;
    }
    b->bitPos = entryPos;
    return -1;
}

/* Decode one 1D line: color starts white. bits = 1 for black. */
static int g3_line(G3Bit *b, uint8_t *outRow, int w)
{
    int x = 0;
    int color = 0; /* 0 white, 1 black */
    memset(outRow, 0, (size_t)w);
    while (x < w)
    {
        const G3Code *tab;
        int ntab;
        if (color == 0)
        {
            tab = G3_WHITE;
            ntab = 104;
        }
        else
        {
            tab = G3_BLACK;
            ntab = 104;
        }
        int run = -1;
        int used = g3_match(b, b->bitPos, tab, ntab, &run);
        if (used == 0)
        {
            used = g3_match(b, b->bitPos, G3_EXT_MAKEUP,
                            (int)(sizeof(G3_EXT_MAKEUP) / sizeof(G3Code)),
                            &run);
        }
        if (used == 0)
        {
            /* EOL mid-row or dead data: resync */
            if (g3_eol(b, 0) != 0) return -1;
            return 1; /* signal: line aborted, keep what we have */
        }
        for (int i = 0; i < used; i++) g3_bit(b);
        if (run < 0) return -1;
        if (x + run > w) run = w - x;
        if (color == 1)
        {
            memset(outRow + x, 1, (size_t)run);
        }
        x += run;
        /* T.4: a terminating code (run 0..63) ends the run and the color
         * flips; a makeup code (run >= 64) continues with the SAME color's
         * terminating code. */
        if (run < 64) color ^= 1;
    }
    return 0;
}

/* ── pixel access ────────────────────────────────────────────────────────── */

typedef struct
{
    TifCtx *c;
    uint8_t *rows; /* full-size 8-bit gray raster (w*h), 0..255 */
} TifPix;

static uint8_t tif_pixel_gray(void *ud, int outX, int outY)
{
    TifPix *p = (TifPix *)ud;
    TifCtx *c = p->c;
    int srcX = outX * c->scaleNum / c->scaleDen;
    int srcY = outY * c->scaleNum / c->scaleDen;
    if (srcX > c->width - 1) srcX = c->width - 1;
    if (srcY > c->height - 1) srcY = c->height - 1;
    return p->rows[(size_t)srcY * (size_t)c->width + (size_t)srcX];
}

/* ── main entry ──────────────────────────────────────────────────────────── */

LCDBitmap *tif_decode(const uint8_t *data, size_t len)
{
    if (!data || len < 16) return NULL;
    int bo;
    if (data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0)
        bo = 0;
    else if (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42)
        bo = 1;
    else
        return NULL;

    TifCtx c;
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.len = len;
    c.bo = bo;
    c.bps = 8;
    c.samples = 1;
    c.predictor = 1;

    uint32_t ifdOff = tif_u32(data, 4, len, &c);
    if (ifdOff == 0 || ifdOff + 2 > len) return NULL;
    uint32_t nTags = tif_u16(data, ifdOff, len, &c);
    if (nTags > 128) return NULL;

    uint32_t stripOff = 0, stripCnt = 0, stripBytes = 0;
    uint32_t rowsPerStrip = 0xFFFFFFFFu;
    uint32_t stripOffsAt = 0, stripCntAt = 0, stripBytesAt = 0;

    for (uint32_t i = 0; i < nTags; i++)
    {
        size_t e = ifdOff + 2 + (size_t)i * 12;
        if (e + 12 > len) return NULL;
        uint32_t tag = tif_u16(data, e, len, &c);
        uint32_t typ = tif_u16(data, e + 2, len, &c);
        uint32_t cnt = tif_u32(data, e + 4, len, &c);
        uint32_t val;
        if (typ == 3 && cnt == 1)
        {
            val = c.bo ? (uint32_t)((data[e + 8] << 8) | data[e + 9])
                       : (uint32_t)((data[e + 9] << 8) | data[e + 8]);
        }
        else if (typ == 4 && cnt == 1)
        {
            val = tif_u32(data, e + 8, len, &c);
        }
        else if (cnt == 1 && typ <= 5)
        {
            val = tif_u32(data, e + 8, len, &c);
        }
        else
        {
            val = tif_u32(data, e + 8, len, &c); /* offset to array */
        }

        switch (tag)
        {
        case 256: c.width = (int)val; break;
        case 257: c.height = (int)val; break;
        case 258: /* bits per sample */
            if (cnt == 1)
            {
                c.bps = (int)val;
            }
            else
            {
                /* array: first entry (RGB all 8) */
                c.bps = (int)tif_u16(data, val, len, &c);
            }
            break;
        case 259: c.compression = (int)val; break;
        case 262: c.photometric = (int)val; break;
        case 273: stripOff = val; stripCnt = cnt; stripOffsAt = (uint32_t)e; break;
        case 277: c.samples = (int)val; break;
        case 317: c.predictor = (int)val; break;
        case 278: rowsPerStrip = val; break;
        case 279: stripBytes = val; stripBytesAt = (uint32_t)e; break;
        case 320: /* color map */
        {
            uint32_t n = cnt;
            if (n > 768) n = 768;
            /* 16-bit components, 2^bps entries per channel */
            uint32_t per = 1u << (c.bps ? c.bps : 1);
            for (uint32_t k = 0; k < n / 3 && k < 256; k++)
            {
                c.pal[k][0] = (uint16_t)tif_u16(data, val + k * 2, len, &c);
                c.pal[k][1] =
                    (uint16_t)tif_u16(data, val + per * 2 + k * 2, len, &c);
                c.pal[k][2] =
                    (uint16_t)tif_u16(data, val + per * 4 + k * 2, len, &c);
            }
            break;
        }
        default: break;
        }
    }

    if (c.width <= 0 || c.height <= 0 || c.width > 4096 || c.height > 4096)
        return NULL;

    /* Multi-strip: use the offsets array (first strip is enough for a
     * thumbnail; but prefer to honor bytes when a single strip). */
    (void)stripCntAt;
    (void)stripBytesAt;
    if (stripCnt > 1)
    {
        stripOff = tif_u32(data, stripOff, len, &c); /* first entry */
    }
    if (stripOff == 0 || stripOff >= len) return NULL;
    c.stripOff = stripOff;
    c.stripBytes = stripBytes ? stripBytes : (uint32_t)(len - stripOff);
    if (c.stripOff + c.stripBytes > len)
    {
        c.stripBytes = (uint32_t)(len - c.stripOff);
    }
    if (rowsPerStrip == 0xFFFFFFFFu) rowsPerStrip = (uint32_t)c.height;

    /* ── expand the strip to an 8-bit gray raster ── */
    size_t raster = (size_t)c.width * (size_t)c.height;
    uint8_t *rows = (uint8_t *)calloc(raster, 1);
    if (!rows) return NULL;

    int ok = 0;
    if ((c.compression == 2 || c.compression == 3 || c.compression == 4) &&
        c.bps == 1)
    {
        /* CCITT G3/G4 1D fax. compression 2 = G3 (EOL optional),
         * 3 = G3 (EOL required by convention), 4 = G4 1D-ish fallback.
         * T4Options bit 2 (=4) means fill bits pad rows to byte bounds. */
        G3Bit b;
        b.data = data + c.stripOff;
        b.len = c.stripBytes;
        b.bitPos = 0;
        uint8_t *line = (uint8_t *)malloc((size_t)c.width);
        if (line)
        {
            ok = 1;
            int fill = (c.compression == 3 || c.compression == 4);
            for (int y = 0; y < c.height && ok; y++)
            {
                /* EOL before each row; some encoders omit it for row 0 or
                 * between all rows (compression 2) — tolerate absence. */
                if (g3_eol(&b, fill) != 0 && y > 0 && c.compression != 2)
                {
                    ok = 0;
                    break;
                }
                int lr = g3_line(&b, line, c.width);
                if (lr < 0)
                {
                    /* resync failed: keep going with what we have */
                }
                for (int x = 0; x < c.width; x++)
                {
                    /* 1=black. photometric 0 (WhiteIsZero) maps black to
                     * gray 0; photometric 1 (BlackIsZero) maps black to
                     * gray 0 as well — identical result, 0 kept. */
                    rows[(size_t)y * c.width + x] =
                        c.photometric == 0 ? (line[x] ? 0 : 255)
                                           : (line[x] ? 255 : 0);
                }
            }
            free(line);
        }
    }
    else
    {
        uint8_t *raw = (uint8_t *)malloc((size_t)c.stripBytes);
        uint8_t *exp = NULL;
        if (raw)
        {
            memcpy(raw, data + c.stripOff, c.stripBytes);
            const uint8_t *pix = raw;
            size_t pixLen = c.stripBytes;

            if (c.compression == 5)
            {
                /* LZW expands to w*h*samples bytes (bps==8) or packed bits */
                size_t expCap = raster * (size_t)(c.bps == 1 ? 1 : c.samples);
                exp = (uint8_t *)malloc(expCap);
                if (exp)
                {
                    size_t expLen = 0;
                    if (lzw_decode(raw, c.stripBytes, exp, expCap, &expLen) == 0)
                    {
                        pix = exp;
                        pixLen = expLen;
                    }
                    else
                    {
                        free(exp);
                        exp = NULL;
                    }
                }
            }
            else if (c.compression == 32773)
            {
                size_t expCap = raster * (size_t)c.samples;
                exp = (uint8_t *)malloc(expCap);
                if (exp)
                {
                    size_t got = packbits_decode(raw, c.stripBytes, exp, expCap);
                    pix = exp;
                    pixLen = got;
                }
            }
            else if (c.compression != 1 &&
                     !(c.bps == 1 && (c.compression == 2 ||
                                      c.compression == 3 ||
                                      c.compression == 4)))
            {
                free(raw);
                free(rows);
                return NULL; /* unsupported compression */
            }

            int rowBytes = (int)((c.bps * c.width * c.samples + 7) / 8);

            /* Horizontal differencing undo (predictor 2, bps 8): each
             * sample in a row is a delta from the previous sample of that
             * pixel channel. Applied in place on the expanded data. */
            if (c.predictor == 2 && c.bps == 8 && pixLen >= (size_t)c.width * (size_t)c.samples)
            {
                for (int y = 0; y < c.height; y++)
                {
                    uint8_t *row = exp ? exp + (size_t)y * (size_t)rowBytes
                                       : raw + (size_t)y * (size_t)rowBytes;
                    for (int x = c.samples; x < c.width * c.samples; x++)
                        row[x] = (uint8_t)(row[x] + row[x - c.samples]);
                }
            }
            ok = 1;
            for (int y = 0; y < c.height && ok; y++)
            {
                const uint8_t *r = pix + (size_t)y * rowBytes;
                if ((size_t)(y * rowBytes) + rowBytes > pixLen)
                {
                    /* truncated: leave remaining rows white */
                    for (int x = 0; x < c.width; x++)
                        rows[(size_t)y * c.width + x] = 255;
                    continue;
                }
                if (c.photometric == 2 && c.bps == 8)
                {
                    /* RGB */
                    for (int x = 0; x < c.width; x++)
                    {
                        size_t p = (size_t)x * 3;
                        rows[(size_t)y * c.width + x] =
                            (uint8_t)dither_rgb_to_gray(r[p], r[p + 1], r[p + 2]);
                    }
                }
                else if (c.photometric == 3 && c.bps == 8)
                {
                    for (int x = 0; x < c.width; x++)
                    {
                        int idx = r[x];
                        rows[(size_t)y * c.width + x] =
                            (uint8_t)dither_rgb_to_gray(c.pal[idx][0] >> 8,
                                                        c.pal[idx][1] >> 8,
                                                        c.pal[idx][2] >> 8);
                    }
                }
                else if (c.photometric == 3 && c.bps == 4)
                {
                    for (int x = 0; x < c.width; x++)
                    {
                        int idx = (x & 1) ? (r[x >> 1] & 0xF) : (r[x >> 1] >> 4);
                        rows[(size_t)y * c.width + x] =
                            (uint8_t)dither_rgb_to_gray(c.pal[idx][0] >> 8,
                                                        c.pal[idx][1] >> 8,
                                                        c.pal[idx][2] >> 8);
                    }
                }
                else if (c.bps == 1)
                {
                    /* bitonal: photometric 0 = WhiteIsZero (bit 0 = white) */
                    for (int x = 0; x < c.width; x++)
                    {
                        int bit = (r[x >> 3] >> (7 - (x & 7))) & 1;
                        int white = (c.photometric == 0) ? (bit == 0) : (bit == 1);
                        rows[(size_t)y * c.width + x] = white ? 255 : 0;
                    }
                }
                else /* 8-bit gray */
                {
                    for (int x = 0; x < c.width; x++)
                    {
                        int g = r[x];
                        if (c.photometric == 0) g = 255 - g; /* WhiteIsZero */
                        rows[(size_t)y * c.width + x] = (uint8_t)g;
                    }
                }
            }
            free(exp);
        }
        free(raw);
    }

    if (!ok)
    {
        free(rows);
        return NULL;
    }

    /* Downscale target (bmp.c conventions). */
    int scaleNum = 1, scaleDen = 1;
    int targetW = c.width, targetH = c.height;
    if (c.width > 360 || c.height > 200)
    {
        if ((long long)c.width * 200 >= (long long)c.height * 360)
        {
            scaleNum = c.width; scaleDen = 360;
        }
        else
        {
            scaleNum = c.height; scaleDen = 200;
        }
        targetW = (int)((long long)c.width * scaleDen / scaleNum);
        if (targetW < 1) targetW = 1;
        targetH = (int)((long long)c.height * scaleDen / scaleNum);
        if (targetH < 1) targetH = 1;
    }
    c.scaleNum = scaleNum;
    c.scaleDen = scaleDen;

    TifPix p;
    p.c = &c;
    p.rows = rows;
    LCDBitmap *img = dither_to_bitmap(targetW, targetH, tif_pixel_gray, &p);
    free(rows);
    if (img)
    {
        logger_log("TIF ok %dx%d", c.width, c.height);
    }
    return img;
}
