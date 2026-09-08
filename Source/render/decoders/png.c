/*
 * PlutoBrowser — png.c
 * Port of Source/render/decoders/png.lua (reference, 270 lines).
 * Streaming PNG decode: chunk scan → inflate stream → row unfilter →
 * grayscale conversion → box-filter downscale → Bayer dither.
 * See png.h for the Lua→C map and preserved semantics.
 */
#include <stdlib.h>
#include <string.h>
#include "render/decoders/png.h"
#include "core/logger.h"
#include "render/decoders/dither.h"
#include "render/decoders/inflate.h"
#include "render/decoders/scale.h"

extern PlaydateAPI *pluto_pd(void);

static uint32_t rd32be(const uint8_t *d, size_t pos, size_t len)
{
    if (pos + 3 >= len)
    {
        return 0;
    }
    return ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos + 1] << 16) |
           ((uint32_t)d[pos + 2] << 8) | (uint32_t)d[pos + 3];
}

/* Lua paethPredictor (verbatim tie-break order). */
static int paeth_predictor(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p - a;
    if (pa < 0) pa = -pa;
    int pb = p - b;
    if (pb < 0) pb = -pb;
    int pc = p - c;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Lua composite: exact floor((gray*a + 255*(255-a))/255 + 0.5). */
static uint8_t png_composite(int gray, int a)
{
    if (a >= 255) return (uint8_t)gray;
    if (a <= 0) return 255;
    return (uint8_t)((gray * a + 255 * (255 - a)) / 255);
}

/* Output-pixel closure state (Lua: rows[y], r[x+1] or 255). */
typedef struct
{
    uint8_t **rows;
    int outCount;
    int outWidth;
} PngCtx;

static uint8_t png_out_pixel(void *ud, int x, int y)
{
    PngCtx *c = (PngCtx *)ud;
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

LCDBitmap *png_decode(const uint8_t *data, size_t len, int maxW, int maxH)
{
    if (!data || len < 24)
    {
        logger_log("PNG decode: short input %zu", len);
        return NULL;
    }
    /* Signature: 0x89 'P' 'N' 'G' \r \n 0x1A \n (Lua's double-check collapses). */
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G' ||
        data[4] != 0x0D || data[5] != 0x0A || data[6] != 0x1A || data[7] != 0x0A)
    {
        logger_log("PNG decode: bad signature");
        return NULL;
    }

    uint32_t width = 0, height = 0;
    int haveIhdr = 0;
    uint8_t bitDepth = 8, colorType = 0, interlace = 0;
    uint8_t palette[256];   /* palette[i] = gray */
    uint8_t paletteA[256];  /* alpha per entry, 255 default */
    int palCount = 0;
    /* IDAT chunks: Lua table.concat's them; scan twice — first to size, then
     * to copy (the chunk scan is chunkLen-driven, not sequential). */
    struct { size_t off; size_t clen; } idat[64];
    int idatCount = 0;
    size_t idatTotal = 0;
    const uint8_t *trnsData = NULL;
    size_t trnsLen = 0;

    memset(palette, 0, sizeof(palette));
    memset(paletteA, 255, sizeof(paletteA));

    size_t pos = 8;
    while (pos < len)
    {
        uint32_t chunkLen = rd32be(data, pos, len);
        const uint8_t *ctype = data + pos + 4;
        size_t chunkDataPos = pos + 8;
        pos = pos + 12 + chunkLen;
        if (pos > len + 12)
        {
            break; /* Lua `pos > #data + 12` overrun break */
        }
        if (pos < 12 || chunkDataPos + chunkLen > len)
        {
            /* Lua slices clamp short reads; treat a short chunk's data as
             * truncated and stop scanning (parity with `or nil` fallbacks). */
            if (chunkDataPos + chunkLen > len)
            {
                break;
            }
        }

        if (memcmp(ctype, "IHDR", 4) == 0)
        {
            width = rd32be(data, chunkDataPos, len);
            height = rd32be(data, chunkDataPos + 4, len);
            bitDepth = (chunkDataPos + 8 < len) ? data[chunkDataPos + 8] : 8;
            colorType = (chunkDataPos + 9 < len) ? data[chunkDataPos + 9] : 0;
            interlace = (chunkDataPos + 12 < len) ? data[chunkDataPos + 12] : 0;
            haveIhdr = 1;
        }
        else if (memcmp(ctype, "PLTE", 4) == 0)
        {
            int numColors = (int)(chunkLen / 3);
            for (int i = 0; i < numColors && i < 256; i++)
            {
                size_t p = chunkDataPos + (size_t)i * 3;
                int r = (p + 2 < len) ? data[p + 2] : 0;
                int g = (p + 1 < len) ? data[p + 1] : 0;
                int b = (p < len) ? data[p] : 0;
                palette[i] = (uint8_t)dither_rgb_to_gray(r, g, b);
                paletteA[i] = 255;
            }
            palCount = numColors > 256 ? 256 : numColors;
        }
        else if (memcmp(ctype, "tRNS", 4) == 0)
        {
            trnsData = data + chunkDataPos;
            trnsLen = chunkLen;
        }
        else if (memcmp(ctype, "IDAT", 4) == 0)
        {
            if (idatCount < 64)
            {
                idat[idatCount].off = chunkDataPos;
                idat[idatCount].clen = chunkLen;
                idatTotal += chunkLen;
                idatCount++;
            }
        }
        else if (memcmp(ctype, "IEND", 4) == 0)
        {
            break;
        }
        if (chunkLen == 0 && pos <= 12)
        {
            break; /* zero-length chunk guard (Lua slice-emptiness behavior) */
        }
    }

    if (!haveIhdr || width == 0 || height == 0 ||
        width > 0x7FFFFFF || height > 0x7FFFFFF)
    {
        logger_log("PNG decode: hdr bad (have=%d w=%u h=%u)", haveIhdr, width, height);
        return NULL; /* Lua: `if not width or not height or <= 0` */
    }
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    /* tRNS for palette: one alpha byte per entry. */
    if (trnsData && colorType == 3)
    {
        for (size_t i = 0; i < trnsLen && i < 256; i++)
        {
            paletteA[i] = trnsData[i];
        }
    }

    int channels = 1;
    if (colorType == 2) channels = 3;
    else if (colorType == 3) channels = 1;
    else if (colorType == 4) channels = 2;
    else if (colorType == 6) channels = 4;

    int srcW = (int)width, srcH = (int)height;
    if (interlace == 1)
    {
        srcW = (srcW + 7) / 8;
        srcH = (srcH + 7) / 8;
        if (srcW < 1) srcW = 1;
        if (srcH < 1) srcH = 1;
    }

    int depth = bitDepth;
    size_t rowBytes = ((size_t)srcW * channels * depth + 7) / 8;
    int bppBytes = channels * depth / 8;
    if (bppBytes < 1) bppBytes = 1;
    int sampleBytes = depth / 8;
    if (sampleBytes < 1) sampleBytes = 1;

    /* Concatenate IDAT chunks (Lua table.concat). */
    uint8_t *compressed = (uint8_t *)malloc(idatTotal ? idatTotal : 1);
    if (!compressed)
    {
        return NULL;
    }
    size_t coff = 0;
    for (int i = 0; i < idatCount; i++)
    {
        memcpy(compressed + coff, data + idat[i].off, idat[i].clen);
        coff += idat[i].clen;
    }

    InflateStream *inf = inflate_stream_new(compressed, idatTotal);
    if (!inf)
    {
        free(compressed);
        logger_log("PNG decode: stream_new NULL (idatTotal=%zu count=%d)", idatTotal, idatCount);
        return NULL;
    }
    /* NOTE: the stream references `compressed` in place (Lua-parity no-copy);
     * it must stay alive until inflate_stream_free. */

    ScaleAccum *acc = scale_accum_new(srcW, srcH, maxW, maxH);
    int boxW = 0, boxH = 0, targetW = 0, targetH = 0;
    scale_box_sizes(srcW, srcH, maxW, maxH, &boxW, &boxH, &targetW, &targetH);
    if (!acc)
    {
        inflate_stream_free(inf);
        return NULL;
    }

    uint8_t *prevRow = (uint8_t *)calloc(rowBytes ? rowBytes : 1, 1);
    uint8_t *curRow = (uint8_t *)calloc(rowBytes ? rowBytes : 1, 1);
    uint8_t *grayRow = (uint8_t *)malloc((size_t)srcW ? (size_t)srcW : 1);
    if (!prevRow || !curRow || !grayRow)
    {
        free(prevRow);
        free(curRow);
        free(grayRow);
        scale_accum_free(acc);
        inflate_stream_free(inf);
        return NULL;
    }

    /* tRNS keys for grayscale / truecolor (Lua tRNSkey). */
    int haveTrnsKey = 0;
    int trnsGrayKey = 0;
    int trnsR = 0, trnsG = 0, trnsB = 0;
    if (trnsData)
    {
        if (colorType == 0 && trnsLen >= 2)
        {
            haveTrnsKey = 1;
            trnsGrayKey = trnsData[0]; /* Lua byte(1): high byte of the 16-bit value */
        }
        else if (colorType == 2 && trnsLen >= 6)
        {
            haveTrnsKey = 1;
            trnsR = trnsData[0];
            trnsG = trnsData[2];
            trnsB = trnsData[4];
        }
    }

    const int unpackMask = depth < 8 ? (1 << depth) - 1 : 0;
    const int grayScale = depth < 8 ? 255 / unpackMask : 1;
    const int perByte = depth < 8 ? 8 / depth : 1;

    int done = 0;
    for (int y = 0; y < srcH && !done; y++)
    {
        /* Tasks.yieldCheck() site (parity comment; budgeted at task layer). */
        size_t got1 = 0;
        const uint8_t *h = inflate_stream_read(inf, 1, &got1);
        if (!h || got1 == 0)
        {
            done = 1;
            break;
        }
        int filterType = h[0];

        size_t gotR = 0;
        const uint8_t *raw = inflate_stream_read(inf, rowBytes, &gotR);
        if (!raw || gotR == 0)
        {
            done = 1;
            break;
        }

        /* Unfilter row (missing bytes read as 0 — Lua `or 0`). */
        for (size_t x = 0; x < rowBytes; x++)
        {
            int xv = x < gotR ? raw[x] : 0;
            int a = (x >= (size_t)bppBytes) ? curRow[x - bppBytes] : 0;
            int b = prevRow[x];
            int c = (x >= (size_t)bppBytes) ? prevRow[x - bppBytes] : 0;
            int v;
            switch (filterType)
            {
            case 1: v = (xv + a) & 0xFF; break;
            case 2: v = (xv + b) & 0xFF; break;
            case 3: v = (xv + ((a + b) >> 1)) & 0xFF; break;
            case 4: v = (xv + paeth_predictor(a, b, c)) & 0xFF; break;
            default: v = xv; break; /* 0 and unknown → copy (Lua else) */
            }
            curRow[x] = (uint8_t)v;
        }

        /* Convert to grayscale row. */
        if (colorType == 0)
        {
            if (depth == 16)
            {
                for (int x = 0; x < srcW; x++)
                {
                    grayRow[x] = curRow[x * 2];
                }
            }
            else if (depth == 8)
            {
                memcpy(grayRow, curRow, (size_t)srcW);
            }
            else
            {
                for (int x = 0; x < srcW; x++)
                {
                    int byteIdx = x / perByte;
                    int shift = 8 - depth - (x % perByte) * depth;
                    grayRow[x] = (uint8_t)(((curRow[byteIdx] >> shift) & unpackMask) * grayScale);
                }
            }
            if (haveTrnsKey)
            {
                for (int x = 0; x < srcW; x++)
                {
                    if (grayRow[x] == trnsGrayKey)
                    {
                        grayRow[x] = 255;
                    }
                }
            }
        }
        else if (colorType == 3)
        {
            for (int x = 0; x < srcW; x++)
            {
                int idx;
                if (depth == 8)
                {
                    idx = curRow[x];
                }
                else
                {
                    int byteIdx = x / perByte;
                    int shift = 8 - depth - (x % perByte) * depth;
                    idx = (curRow[byteIdx] >> shift) & unpackMask;
                }
                /* Lua palette[idx or 1] or 255 — out-of-range → 255 */
                int g = idx < palCount ? palette[idx] : 255;
                int a = idx < palCount ? paletteA[idx] : 255;
                grayRow[x] = png_composite(g, a);
            }
        }
        else if (colorType == 2 || colorType == 6)
        {
            int step = (colorType == 6) ? 4 : 3;
            for (int x = 0; x < srcW; x++)
            {
                size_t p = (size_t)x * step * sampleBytes;
                int r = (p < rowBytes) ? curRow[p] : 0;
                int g = (p + sampleBytes < rowBytes) ? curRow[p + sampleBytes] : 0;
                int b = (p + sampleBytes * 2 < rowBytes) ? curRow[p + sampleBytes * 2] : 0;
                int gv;
                if (haveTrnsKey && r == trnsR && g == trnsG && b == trnsB)
                {
                    grayRow[x] = 255;
                    continue;
                }
                gv = dither_rgb_to_gray(r, g, b);
                if (colorType == 6)
                {
                    int av = (p + sampleBytes * 3 < rowBytes) ? curRow[p + sampleBytes * 3] : 255;
                    grayRow[x] = png_composite(gv, av);
                }
                else
                {
                    grayRow[x] = (uint8_t)gv;
                }
            }
        }
        else if (colorType == 4)
        {
            if (depth == 16)
            {
                for (int x = 0; x < srcW; x++)
                {
                    size_t p = (size_t)x * 4;
                    int g = (p < rowBytes) ? curRow[p] : 0;
                    int a = (p + 2 < rowBytes) ? curRow[p + 2] : 255;
                    grayRow[x] = png_composite(g, a);
                }
            }
            else
            {
                for (int x = 0; x < srcW; x++)
                {
                    size_t p = (size_t)x * 2;
                    int g = (p < rowBytes) ? curRow[p] : 0;
                    int a = (p + 1 < rowBytes) ? curRow[p + 1] : 255;
                    grayRow[x] = png_composite(g, a);
                }
            }
        }

        scale_accum_add_row(acc, grayRow);
        uint8_t *tmp = prevRow;
        prevRow = curRow;
        curRow = tmp;
    }

    int outCount = 0, outWidth = 0;
    uint8_t **rows = scale_accum_finish(acc, &outCount, &outWidth);
    LCDBitmap *img = NULL;
    if (rows && outCount > 0) /* Lua: if acc.count == 0 then return nil */
    {
        PngCtx pc = { rows, outCount, outWidth };
        img = dither_to_bitmap(targetW, targetH, png_out_pixel, &pc);
    }

    free(prevRow);
    free(curRow);
    free(grayRow);
    scale_accum_free(acc);
    inflate_stream_free(inf);
    free(compressed);
    return img;
}
