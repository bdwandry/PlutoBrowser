/*
 * PlutoBrowser — gif.c
 * Port of Source/render/decoders/gif.lua (reference, 293 lines).
 * First-frame GIF decoder; LZW output streams directly into the box-filter
 * downscaler (bounded memory), interlace rows buffered and replayed.
 * See gif.h for the Lua→C map and preserved semantics.
 */
#include "core/logger.h"
#include <stdlib.h>
#include <string.h>
#include "render/decoders/gif.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"

extern PlaydateAPI *pluto_pd(void);

static uint16_t rd16(const uint8_t *d, size_t pos, size_t len)
{
    if (pos + 1 >= len)
    {
        return 0;
    }
    return (uint16_t)(d[pos] | ((uint16_t)d[pos + 1] << 8));
}

/* ── Decode state (Lua upvalues → struct) ──────────────────────────────── */
typedef struct
{
    /* LZW bitstream (concatenated sub-blocks) */
    const uint8_t *lzw;
    size_t lzwLen;
    int bitPos;

    /* code table */
    int minCodeSize;
    int clearCode, endCode, codeSize, maxCode;
    int prefix[4096];
    int suffix[4096];
    int nextCode;
    int oldCode;
    int firstChar;

    /* frame geometry / palettes */
    int screenW, screenH;
    int imgLeft, imgTop, imgW, imgH;
    uint8_t palette[256];
    int palCount; /* Lua: indexes >= palCount read as nil → 255 */
    int transparentIndex; /* -1 = none (Lua nil) */

    /* output streaming */
    ScaleAccum *acc;
    uint8_t *rowBuf;    /* imgW current-row pixels (palette grays) */
    uint8_t *canvasRow; /* screenW composited row */
    int rowIdx;
    int x;
    int pixelCounter;

    /* interlace */
    int interlaced;
    int interlaceRows[4096];
    int interlaceCount;
    uint8_t **bufferedRows; /* imgH entries (linear order) */

    /* LZW decode stack (Lua local `stack`) — kept in the state so the
     * whole context can live in BSS instead of the 66KB game-task stack. */
    int stack[4096];
} GifState;

static void gif_init_code_table(GifState *g)
{
    g->codeSize = g->minCodeSize + 1;
    g->maxCode = 1 << g->codeSize;
    /* Lua: prefix[i]=-1/suffix[i]=i for roots; clear/end codes stay nil.
     * In the walk, Lua-nil and -1 take the SAME else-branch, so a full
     * -1 fill reproduces the reference exactly (never-added codes are
     * unreachable: the KwKwK guard bounds code < nextCode). */
    for (int i = 0; i < 4096; i++)
    {
        g->prefix[i] = -1;
        g->suffix[i] = 0;
    }
    for (int i = 0; i < g->clearCode; i++)
    {
        g->suffix[i] = i;
    }
}

/* Lua readCode: 3-byte LSB window; NULL (past end) → -1. */
static int gif_read_code(GifState *g)
{
    size_t bytePos = (size_t)(g->bitPos >> 3);
    int bitOffset = g->bitPos & 7;
    if (bytePos >= g->lzwLen)
    {
        return -1;
    }
    uint32_t b1 = g->lzw[bytePos];
    uint32_t b2 = bytePos + 1 < g->lzwLen ? g->lzw[bytePos + 1] : 0;
    uint32_t b3 = bytePos + 2 < g->lzwLen ? g->lzw[bytePos + 2] : 0;
    uint32_t val = (b1 | (b2 << 8) | (b3 << 16)) >> bitOffset;
    g->bitPos += g->codeSize;
    return (int)(val & ((1u << g->codeSize) - 1));
}

/* Lua flushRow: pad the row, composite onto white canvas, emit. */
static void gif_flush_row(GifState *g)
{
    for (int cx = g->x; cx < g->imgW; cx++)
    {
        g->rowBuf[cx] = 255;
    }
    memset(g->canvasRow, 255, (size_t)g->screenW);
    for (int cx = 0; cx < g->imgW; cx++)
    {
        int px = g->imgLeft + cx;
        if (px >= 0 && px < g->screenW)
        {
            g->canvasRow[px] = g->rowBuf[cx];
        }
    }
    if (g->interlaced)
    {
        int linearRow = (g->rowIdx < g->interlaceCount) ? g->interlaceRows[g->rowIdx] : -1;
        if (linearRow >= 0 && linearRow < g->imgH)
        {
            uint8_t *copy = (uint8_t *)malloc((size_t)g->screenW);
            if (copy)
            {
                memcpy(copy, g->canvasRow, (size_t)g->screenW);
                free(g->bufferedRows[linearRow]);
                g->bufferedRows[linearRow] = copy;
            }
        }
    }
    else
    {
        scale_accum_add_row(g->acc, g->canvasRow);
    }
    g->rowIdx++;
    g->x = 0;
}

/* Lua endStream: flush the partial row, then flush whole rows to imgH. */
static void gif_end_stream(GifState *g)
{
    gif_flush_row(g);
    while (g->rowIdx < g->imgH)
    {
        gif_flush_row(g);
    }
}

/* Ramp pixel source for the final dither pass (Lua getPixelGray). */
typedef struct
{
    uint8_t **out;
    int width;
} GifOutCtx;

static uint8_t gif_out_pixel(void *ud, int x, int y)
{
    GifOutCtx *c = (GifOutCtx *)ud;
    uint8_t *row = c->out[y];
    return row ? row[x] : 255;
}

LCDBitmap *gif_decode(const uint8_t *data, size_t len, int maxW, int maxH)
{
    logger_stack_touch();
    if (!data || len < 14)
    {
        return NULL;
    }
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)
    {
        return NULL;
    }

    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    int screenW = rd16(data, 6, len);
    int screenH = rd16(data, 8, len);
    uint8_t packed = data[10];

    int hasGlobalPal = (packed & 0x80) != 0;
    int globalPalCount = 1 << ((packed & 0x07) + 1);

    size_t pos = 13;
    uint8_t globalPal[256];
    memset(globalPal, 0, sizeof(globalPal));
    int globalPalCountEff = 0;

    if (hasGlobalPal)
    {
        for (int i = 0; i < globalPalCount && i < 256; i++)
        {
            size_t p = pos + (size_t)i * 3;
            if (p + 2 < len)
            {
                globalPal[i] = (uint8_t)dither_rgb_to_gray(data[p], data[p + 1], data[p + 2]);
            }
            else
            {
                globalPal[i] = 0;
            }
        }
        pos += (size_t)globalPalCount * 3;
        globalPalCountEff = globalPalCount > 256 ? 256 : globalPalCount;
    }

    int transparentIndex = -1; /* Lua nil */

    while (pos < len)
    {
        uint8_t b = data[pos];
        if (b == 0x3B)
        {
            return NULL; /* trailer before any image descriptor */
        }
        else if (b == 0x21)
        {
            /* Extension block */
            uint8_t extType = pos + 1 < len ? data[pos + 1] : 0;
            pos += 2;
            if (extType == 0xF9)
            {
                uint8_t gcePacked = pos + 1 < len ? data[pos + 1] : 0;
                if (gcePacked & 1)
                {
                    if (pos + 4 < len)
                    {
                        transparentIndex = data[pos + 4];
                    }
                }
            }
            while (pos < len)
            {
                uint8_t blockSize = data[pos];
                pos++;
                if (blockSize == 0)
                {
                    break;
                }
                pos += blockSize;
            }
        }
        else if (b == 0x2C)
        {
            /* Image descriptor */
            int imgLeft = rd16(data, pos + 1, len);
            int imgTop = rd16(data, pos + 3, len);
            int imgW = rd16(data, pos + 5, len);
            int imgH = rd16(data, pos + 7, len);
            uint8_t imgPacked = pos + 9 < len ? data[pos + 9] : 0;
            pos += 10;

            /* Static (BSS): the state is ~66KB — far beyond the device's
             * game-task stack. Single-task, non-reentrant (one decode at a
             * time by design, like every other decoder phase). */
            static GifState g;
            memset(&g, 0, sizeof(g));
            g.screenW = screenW;
            g.screenH = screenH;
            g.imgLeft = imgLeft;
            g.imgTop = imgTop;
            g.imgW = imgW;
            g.imgH = imgH;
            g.transparentIndex = transparentIndex;
            g.oldCode = -1;

            memcpy(g.palette, globalPal, 256);
            g.palCount = globalPalCountEff;
            if (imgPacked & 0x80)
            {
                int localPalSize = 1 << ((imgPacked & 0x07) + 1);
                g.palCount = localPalSize > 256 ? 256 : localPalSize;
                memset(g.palette, 0, sizeof(g.palette));
                for (int i = 0; i < localPalSize && i < 256; i++)
                {
                    size_t p = pos + (size_t)i * 3;
                    if (p + 2 < len)
                    {
                        g.palette[i] = (uint8_t)dither_rgb_to_gray(data[p], data[p + 1], data[p + 2]);
                    }
                    else
                    {
                        g.palette[i] = 0;
                    }
                }
                pos += (size_t)localPalSize * 3;
            }

            g.minCodeSize = pos < len ? data[pos] : 2;
            pos++;

            /* Concatenate LZW sub-blocks into one contiguous bitstream
             * (Lua table.concat of string.sub chunks — length bytes do
             * NOT belong to the stream). */
            uint8_t *lzwBuf = (uint8_t *)malloc(4096);
            size_t lzwTotal = 0;
            size_t lzwCap = 4096;
            int lzwOOM = 0;
            if (!lzwBuf)
            {
                lzwOOM = 1;
            }
            while (pos < len)
            {
                uint8_t blockSize = data[pos];
                pos++;
                if (blockSize == 0)
                {
                    break;
                }
                if (!lzwOOM)
                {
                    if (lzwTotal + blockSize > lzwCap)
                    {
                        size_t nc = lzwCap;
                        while (lzwTotal + blockSize > nc)
                        {
                            nc *= 2;
                        }
                        uint8_t *grown = (uint8_t *)realloc(lzwBuf, nc);
                        if (!grown)
                        {
                            lzwOOM = 1;
                        }
                        else
                        {
                            lzwBuf = grown;
                            lzwCap = nc;
                        }
                    }
                    if (!lzwOOM && pos + blockSize <= len)
                    {
                        memcpy(lzwBuf + lzwTotal, data + pos, blockSize);
                        lzwTotal += blockSize;
                    }
                }
                pos += blockSize;
            }
            g.lzw = lzwBuf;
            g.lzwLen = lzwTotal;

            g.clearCode = 1 << g.minCodeSize;
            g.endCode = g.clearCode + 1;
            gif_init_code_table(&g);
            g.nextCode = g.endCode + 1;

            g.acc = scale_accum_new(screenW, screenH, maxW, maxH);
            g.rowBuf = (uint8_t *)malloc((size_t)(imgW > 0 ? imgW : 1));
            g.canvasRow = (uint8_t *)malloc((size_t)(screenW > 0 ? screenW : 1));
            g.interlaced = (imgPacked & 0x40) != 0;
            if (g.interlaced)
            {
                static const int passes[4][2] = { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } };
                int n = 0;
                for (int p = 0; p < 4; p++)
                {
                    for (int r = passes[p][0]; r < imgH && n < 4096; r += passes[p][1])
                    {
                        g.interlaceRows[n++] = r;
                    }
                }
                g.interlaceCount = n;
                g.bufferedRows = (uint8_t **)calloc((size_t)(imgH > 0 ? imgH : 1), sizeof(uint8_t *));
            }

            LCDBitmap *result = NULL;
            if (g.acc && g.rowBuf && g.canvasRow && (!g.interlaced || g.bufferedRows))
            {
                while (g.rowIdx < g.imgH)
                {
                    int code = gif_read_code(&g);
                    if (code < 0 || code == g.endCode)
                    {
                        gif_end_stream(&g);
                        break;
                    }

                    if (code == g.clearCode)
                    {
                        gif_init_code_table(&g);
                        g.nextCode = g.endCode + 1;
                        g.oldCode = -1;
                    }
                    else
                    {
                        int inCode = code;
                        int *stack = g.stack;
                        int top = 0;

                        if (code >= g.nextCode)
                        {
                            if (g.oldCode >= 0)
                            {
                                stack[top++] = g.firstChar;
                                code = g.oldCode;
                            }
                            else
                            {
                                gif_end_stream(&g);
                                break;
                            }
                        }

                        /* Lua walk: prefix[code] ~= nil AND >= 0 → follow;
                         * else push `suffix[code] or code` and stop. In C a
                         * -1 fill makes nil and -1 both take the else. */
                        while (code >= 0 && code < 4096)
                        {
                            if (g.prefix[code] >= 0)
                            {
                                stack[top++] = g.suffix[code];
                                code = g.prefix[code];
                            }
                            else
                            {
                                stack[top++] = g.suffix[code] ? g.suffix[code] : code;
                                break;
                            }
                        }

                        g.firstChar = top > 0 ? stack[top - 1] : 0;

                        /* Emit decoded run into rowBuf (Lua stack unwinding). */
                        while (top > 0 && g.rowIdx < g.imgH)
                        {
                            int palIdx = stack[--top];
                            if (g.x < g.imgW)
                            {
                                if (palIdx == g.transparentIndex)
                                {
                                    g.rowBuf[g.x] = 255;
                                }
                                else
                                {
                                    /* Lua `palette[palIdx] or 255`: out-of-range
                                     * index → nil → white. */
                                    g.rowBuf[g.x] =
                                        (palIdx >= 0 && palIdx < g.palCount)
                                            ? g.palette[palIdx]
                                            : 255;
                                }
                            }
                            g.x++;
                            if (g.x >= g.imgW)
                            {
                                gif_flush_row(&g);
                                g.pixelCounter++;
                                /* Tasks.yieldCheck() call site (every 8 rows) */
                            }
                        }

                        if (g.oldCode >= 0 && g.nextCode < 4096)
                        {
                            g.prefix[g.nextCode] = g.oldCode;
                            g.suffix[g.nextCode] = g.firstChar;
                            g.nextCode++;
                            if (g.nextCode >= g.maxCode && g.codeSize < 12)
                            {
                                g.codeSize++;
                                g.maxCode = 1 << g.codeSize;
                            }
                        }

                        g.oldCode = inCode;
                    }
                }

                if (g.interlaced)
                {
                    /* Lua passes blankRow (all 255) for never-buffered rows —
                     * addRow(nil) would be a no-op, so explicit white. */
                    uint8_t *blank = (uint8_t *)malloc((size_t)(g.screenW > 0 ? g.screenW : 1));
                    if (blank)
                    {
                        memset(blank, 255, (size_t)g.screenW);
                    }
                    for (int y = 0; y < g.imgH; y++)
                    {
                        scale_accum_add_row(g.acc,
                                            g.bufferedRows[y] ? g.bufferedRows[y] : blank);
                    }
                    free(blank);
                }

                int cnt = 0, wid = 0;
                uint8_t **out = scale_accum_finish(g.acc, &cnt, &wid);
                GifOutCtx octx = { out, wid };
                result = dither_to_bitmap(wid, cnt, gif_out_pixel, &octx);
            }

            if (g.interlaced)
            {
                for (int y = 0; y < g.imgH; y++)
                {
                    free(g.bufferedRows[y]);
                }
                free(g.bufferedRows);
            }
            free(g.rowBuf);
            free(g.canvasRow);
            free(lzwBuf);
            scale_accum_free(g.acc);
            return result;
        }
        else
        {
            pos++; /* unknown block byte (Lua parity) */
        }
    }

    return NULL;
}
