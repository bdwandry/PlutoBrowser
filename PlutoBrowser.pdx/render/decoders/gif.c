// gif.c — C port of Source/render/decoders/gif.lua (GIFDecoder).
//
// Decodes the first frame of a GIF87a/89a, streaming LZW output straight
// into the box-filter downscaler so a full-resolution pixel buffer is
// never allocated. The LOGICAL SCREEN is the source canvas (white
// background), so offset frames composite correctly. Animated GIFs
// render as their first frame. Interlaced frames buffer their rows and
// replay them in linear order after the stream ends. The decode loop
// calls tasks_yield_check() so large images decode cooperatively.

#include "render/decoders/gif.h"

#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_GIF_PD 1
#endif

static unsigned gif_u16le(const uint8_t* d, size_t len, size_t o) {
    if (o >= len || o + 1 >= len) return 0;
    return (unsigned)d[o] | ((unsigned)d[o + 1] << 8);
}

/* byte accessor: -1 == Lua nil */
static int gif_byte(const uint8_t* d, size_t len, size_t o) {
    return o < len ? d[o] : -1;
}

typedef struct {
    int imgLeft, imgW, imgH;
    int screenW;
    int transparentIndex;
    const uint8_t* palette;
    const int* havePal;

    ScaleAccum* acc;
    uint8_t* rowBuf;      /* imgW entries */
    uint8_t* canvasRow;   /* screenW entries */

    int interlaced;
    int* interlaceRows;   /* emitted row -> linear row */
    int nInterRows;
    uint8_t** interBuf;   /* linear row -> canvas copy (or NULL) */
    uint8_t* blankRow;    /* screenW of 255 */

    long rowIdx;
    int x;
    long pixelCounter;
} GifDeco;

/* flushRow parity: pad the tail, composite onto white screen canvas,
 * buffer or addRow, advance row index */
static void gif_flush_row(GifDeco* g) {
    for (int cx = g->x; cx < g->imgW; cx++) g->rowBuf[cx] = 255;
    memset(g->canvasRow, 255, (size_t)g->screenW);
    for (int cx = 0; cx < g->imgW; cx++) {
        int dst = g->imgLeft + cx;
        if (dst >= 0 && dst < g->screenW)
            g->canvasRow[dst] = g->rowBuf[cx];
    }
    if (g->interlaced) {
        int lin = g->rowIdx < g->nInterRows
                      ? g->interlaceRows[g->rowIdx]
                      : -1;
        if (lin >= 0 && lin < g->imgH) {
            /* rows may replay more than once only if passes overlap;
             * they do not in the standard scheme, so allocate lazily */
            if (g->interBuf[lin] == NULL)
                g->interBuf[lin] =
                    (uint8_t*)pluto_malloc((size_t)g->screenW);
            if (g->interBuf[lin] != NULL)
                memcpy(g->interBuf[lin], g->canvasRow,
                       (size_t)g->screenW);
        }
    } else {
        scale_accum_add_row(g->acc, g->canvasRow);
    }
    g->rowIdx++;
    g->x = 0;
    g->pixelCounter++;
    if (g->pixelCounter % 8 == 0) tasks_yield_check();
}

/* endStream parity: flush the padded partial row, then fill the rest */
static void gif_end_stream(GifDeco* g) {
    gif_flush_row(g);
    while (g->rowIdx < g->imgH) gif_flush_row(g);
}

static int gif_read_code(long* bitPos, int codeSize,
                         const uint8_t* lzw, size_t lzwLen) {
    size_t bytePos = (size_t)(*bitPos >> 3);   /* Lua bytePos-1 */
    if (bytePos >= lzwLen) return -1;
    int b1 = lzw[bytePos];
    int b2 = bytePos + 1 < lzwLen ? lzw[bytePos + 1] : 0;
    int b3 = bytePos + 2 < lzwLen ? lzw[bytePos + 2] : 0;
    unsigned val = ((unsigned)b1 | ((unsigned)b2 << 8) |
                    ((unsigned)b3 << 16)) >>
                   (*bitPos & 7);
    *bitPos += codeSize;
    return (int)(val & ((1u << codeSize) - 1));
}

int gif_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH) {
    *outRows = NULL;
    *outW = *outH = 0;
    if (data == NULL || len < 14) return -1;

    if (memcmp(data, "GIF87a", 6) != 0 &&
        memcmp(data, "GIF89a", 6) != 0)
        return -1;

    if (maxW <= 0) maxW = 360;      /* Lua `maxW or 360` */
    if (maxH <= 0) maxH = 200;

    int screenW = (int)gif_u16le(data, len, 6);
    int screenH = (int)gif_u16le(data, len, 8);
    int packed = gif_byte(data, len, 10);
    if (packed < 0) packed = 0;

    int hasGlobalPal = (packed & 0x80) != 0;
    int globalPalCount = 1 << ((packed & 0x07) + 1);

    size_t pos = 13;   /* Lua pos = 14, 1-based */
    uint8_t globalPal[256];
    int haveGlobal[256];   /* mirrors which keys exist in the Lua table */
    memset(haveGlobal, 0, sizeof(haveGlobal));

    if (hasGlobalPal) {
        if (globalPalCount > 256) globalPalCount = 256;
        for (int i = 0; i < globalPalCount; i++) {
            int r = gif_byte(data, len, pos);
            int g = gif_byte(data, len, pos + 1);
            int b = gif_byte(data, len, pos + 2);
            pos += 3;
            if (r >= 0 && g >= 0 && b >= 0)
                globalPal[i] = (uint8_t)dither_rgb_to_gray(r, g, b);
            else
                globalPal[i] = 0;
            haveGlobal[i] = 1;
        }
    }

    /* process blocks until the first Image Descriptor (0x2C) */
    int transparentIndex = -1;   /* -1 == Lua nil */

    while (pos <= len) {
        int b = gif_byte(data, len, pos);
        if (b < 0 || b == 0x3B) {   /* trailer or end of data */
            return -1;
        } else if (b == 0x21) {     /* extension block */
            int extType = gif_byte(data, len, pos + 1);
            pos += 2;
            if (extType == 0xF9) {  /* graphic control extension */
                int gcePacked = gif_byte(data, len, pos + 1);
                if (gcePacked < 0) gcePacked = 0;
                if ((gcePacked & 1) != 0) {
                    transparentIndex =
                        gif_byte(data, len, pos + 4);   /* maybe -1 */
                }
            }
            while (pos <= len) {
                int blockSize = gif_byte(data, len, pos);
                pos += 1;
                if (blockSize < 0) blockSize = 0;
                if (blockSize == 0) break;
                pos += (size_t)blockSize;
            }
        } else if (b == 0x2C) {     /* image descriptor: decode frame */
            GifDeco g;
            memset(&g, 0, sizeof(g));
            g.imgLeft = (int)gif_u16le(data, len, pos + 1);
            /* imgTop unused by the source beyond parsing (rows always
             * composite at imgLeft on full-width canvas lines) */
            (void)gif_u16le(data, len, pos + 3);
            g.imgW = (int)gif_u16le(data, len, pos + 5);
            g.imgH = (int)gif_u16le(data, len, pos + 7);
            int imgPacked = gif_byte(data, len, pos + 9);
            if (imgPacked < 0) imgPacked = 0;
            pos += 10;

            g.screenW = screenW;
            g.transparentIndex = transparentIndex;
            g.palette = globalPal;
            g.havePal = haveGlobal;

            uint8_t localPal[256];
            int haveLocal[256];

            if ((imgPacked & 0x80) != 0) {   /* local palette */
                int localPalSize = 1 << ((imgPacked & 0x07) + 1);
                if (localPalSize > 256) localPalSize = 256;
                memset(haveLocal, 0, sizeof(haveLocal));
                for (int i = 0; i < localPalSize; i++) {
                    int r = gif_byte(data, len, pos);
                    int gg = gif_byte(data, len, pos + 1);
                    int bl = gif_byte(data, len, pos + 2);
                    pos += 3;
                    if (r >= 0 && gg >= 0 && bl >= 0)
                        localPal[i] =
                            (uint8_t)dither_rgb_to_gray(r, gg, bl);
                    else
                        localPal[i] = 0;
                    haveLocal[i] = 1;
                }
                g.palette = localPal;
                g.havePal = haveLocal;
            }

            int minCodeSize = gif_byte(data, len, pos);
            if (minCodeSize < 0) minCodeSize = 2;
            pos += 1;

            /* concatenate LZW sub-blocks into one bitstream */
            uint8_t* lzw = NULL;
            size_t lzwLen = 0, lzwCap = 0;
            while (pos <= len) {
                int blockSize = gif_byte(data, len, pos);
                pos += 1;
                if (blockSize < 0) blockSize = 0;
                if (blockSize == 0) break;
                if (lzwLen + (size_t)blockSize > lzwCap) {
                    size_t nc = lzwCap ? lzwCap * 2 : 1024;
                    while (nc < lzwLen + (size_t)blockSize) nc *= 2;
                    uint8_t* nl = (uint8_t*)pluto_realloc(lzw, nc);
                    if (nl == NULL) {
                        pluto_free(lzw);
                        return -1;
                    }
                    lzw = nl;
                    lzwCap = nc;
                }
                for (int k = 0; k < blockSize; k++) {
                    int cb = gif_byte(data, len, pos + k);
                    lzw[lzwLen++] = (uint8_t)(cb < 0 ? 0 : cb);
                }
                pos += (size_t)blockSize;
            }

            if (screenW <= 0 || screenH <= 0 || g.imgW <= 0 ||
                g.imgH <= 0 || minCodeSize < 1 || minCodeSize > 11) {
                pluto_free(lzw);
                return -1;
            }

            /* LZW state (codes widen from minCodeSize+1 to 12);
             * tables live on the heap (~40KB would blow the stack) */
            int16_t* prefixTab =
                (int16_t*)pluto_malloc(sizeof(int16_t) * 4096);
            int* suffixTab = (int*)pluto_malloc(sizeof(int) * 4096);
            int* emitStack = (int*)pluto_malloc(sizeof(int) * (4096 + 2));
            if (prefixTab == NULL || suffixTab == NULL ||
                emitStack == NULL) {
                pluto_free(prefixTab);
                pluto_free(suffixTab);
                pluto_free(emitStack);
                pluto_free(lzw);
                return -1;
            }
            int clearCode = 1 << minCodeSize;
            int endCode = clearCode + 1;
            int codeSize = minCodeSize + 1;
            int maxCode = 1 << codeSize;

#define GIF_INIT_TABLE()                              \
    do {                                              \
        codeSize = minCodeSize + 1;                   \
        maxCode = 1 << codeSize;                      \
        for (int i = 0; i < clearCode; i++) {         \
            prefixTab[i] = -1;                        \
            suffixTab[i] = i;                         \
        }                                             \
        for (int i = clearCode; i < 4096; i++) {      \
            prefixTab[i] = -1;                        \
            suffixTab[i] = -1;                        \
        }                                             \
    } while (0)
            GIF_INIT_TABLE();

            int nextCode = endCode + 1;
            int oldCode = -1;
            int firstChar = 0;
            long bitPos = 0;

            g.acc = scale_accum_new(screenW, screenH, maxW, maxH);
            g.rowBuf = (uint8_t*)pluto_malloc((size_t)g.imgW);
            g.canvasRow = (uint8_t*)pluto_malloc((size_t)screenW);
            if (g.acc == NULL || g.rowBuf == NULL ||
                g.canvasRow == NULL) {
                pluto_free(lzw);
                pluto_free(g.rowBuf);
                pluto_free(g.canvasRow);
                scale_accum_free(g.acc);
                return -1;
            }

            g.interlaced = (imgPacked & 0x40) != 0;
            if (g.interlaced) {
                static const int passes[4][2] = {
                    {0, 8}, {4, 8}, {2, 4}, {1, 2}};
                g.interlaceRows =
                    (int*)pluto_malloc(sizeof(int) * (size_t)g.imgH);
                g.interBuf = (uint8_t**)pluto_malloc(
                    sizeof(uint8_t*) * (size_t)g.imgH);
                g.blankRow = (uint8_t*)pluto_malloc((size_t)screenW);
                if (g.blankRow != NULL)
                    memset(g.blankRow, 255, (size_t)screenW);
                if (g.interBuf != NULL)
                    memset(g.interBuf, 0,
                           sizeof(uint8_t*) * (size_t)g.imgH);
                if (g.interlaceRows != NULL) {
                    for (int p = 0; p < 4; p++) {
                        for (int r = passes[p][0]; r < g.imgH;
                             r += passes[p][1])
                            g.interlaceRows[g.nInterRows++] = r;
                    }
                }
                if (g.interlaceRows == NULL || g.interBuf == NULL ||
                    g.blankRow == NULL) {
                    pluto_free(prefixTab);
                    pluto_free(suffixTab);
                    pluto_free(emitStack);
                    pluto_free(g.rowBuf);
                    pluto_free(g.canvasRow);
                    if (g.interlaceRows != NULL)
                        pluto_free(g.interlaceRows);
                    if (g.blankRow != NULL) pluto_free(g.blankRow);
                    if (g.interBuf != NULL) pluto_free(g.interBuf);
                    pluto_free(lzw);
                    scale_accum_free(g.acc);
                    return -1;
                }
            }

            while (g.rowIdx < g.imgH) {
                int code = gif_read_code(&bitPos, codeSize, lzw, lzwLen);
                if (code < 0 || code == endCode) {
                    gif_end_stream(&g);
                    break;
                }

                if (code == clearCode) {
                    GIF_INIT_TABLE();
                    nextCode = endCode + 1;
                    oldCode = -1;
                } else {
                    int inCode = code;
                    int top = 0;
                    int* stack = emitStack;

                    if (code >= nextCode) {
                        if (oldCode >= 0) {
                            stack[top++] = firstChar;
                            code = oldCode;
                        } else {
                            gif_end_stream(&g);
                            goto frame_done;
                        }
                    }

                    while (code >= 0 && code < 4096) {
                        if (prefixTab[code] >= 0) {
                            stack[top++] = suffixTab[code];
                            code = prefixTab[code];
                        } else {
                            /* `suffix[code] or code` fallback */
                            stack[top++] = suffixTab[code] >= 0
                                               ? suffixTab[code]
                                               : code;
                            break;
                        }
                    }

                    firstChar = top > 0 ? stack[top - 1] : 0;

                    /* emit run into rowBuf, flushing rows as they fill
                     * so runs spanning a boundary survive it */
                    while (top > 0 && g.rowIdx < g.imgH) {
                        int palIdx = stack[--top];
                        if (palIdx == g.transparentIndex &&
                            g.transparentIndex >= 0)
                            g.rowBuf[g.x] = 255;
                        else
                            g.rowBuf[g.x] =
                                (palIdx >= 0 && palIdx < 256 &&
                                 g.havePal[palIdx])
                                    ? g.palette[palIdx]
                                    : 255;
                        g.x++;
                        if (g.x >= g.imgW) gif_flush_row(&g);
                    }

                    if (oldCode >= 0 && nextCode < 4096) {
                        prefixTab[nextCode] = (int16_t)oldCode;
                        suffixTab[nextCode] = firstChar;
                        nextCode++;
                        if (nextCode >= maxCode && codeSize < 12) {
                            codeSize++;
                            maxCode = 1 << codeSize;
                        }
                    }

                    oldCode = inCode;
                }
            }

frame_done:
            pluto_free(prefixTab);
            pluto_free(suffixTab);
            pluto_free(emitStack);
            if (g.interlaced) {
                for (int y = 0; y < g.imgH; y++)
                    scale_accum_add_row(
                        g.acc,
                        g.interBuf[y] != NULL ? g.interBuf[y]
                                              : g.blankRow);
            }

            pluto_free(g.rowBuf);
            pluto_free(g.canvasRow);
            if (g.interlaceRows != NULL) pluto_free(g.interlaceRows);
            if (g.blankRow != NULL) pluto_free(g.blankRow);
            if (g.interBuf != NULL) {
                for (int y = 0; y < g.imgH; y++)
                    if (g.interBuf[y] != NULL)
                        pluto_free(g.interBuf[y]);
                pluto_free(g.interBuf);
            }
            pluto_free(lzw);

            int tw = 0, th = 0;
            int count = scale_accum_finish(g.acc, &tw, &th);
            if (count == 0) {
                // Lua hands an empty grid to toImage; treat as failure
                scale_accum_free(g.acc);
                return -1;
            }

            uint8_t** rows = (uint8_t**)pluto_malloc(
                sizeof(uint8_t*) * (size_t)count);
            if (rows == NULL) {
                scale_accum_free(g.acc);
                return -1;
            }
            int okGrid = 1;
            for (int ry = 0; ry < count && okGrid; ry++) {
                rows[ry] =
                    (uint8_t*)pluto_malloc((size_t)g.acc->targetW);
                if (rows[ry] == NULL) {
                    okGrid = 0;
                    break;
                }
                for (int cx = 0; cx < g.acc->targetW; cx++)
                    rows[ry][cx] = (uint8_t)g.acc->out[ry][cx];
            }
            if (!okGrid) {
                for (int ry = 0; ry < count; ry++)
                    pluto_free(rows[ry]);
                pluto_free(rows);
                scale_accum_free(g.acc);
                return -1;
            }
            *outRows = rows;
            *outW = g.acc->targetW;
            *outH = count;   /* actual rows produced (may exceed target) */
            (void)tw;
            (void)th;
            scale_accum_free(g.acc);
            return 0;
        } else {
            pos += 1;   /* unknown block type: skip like the source */
        }
    }

    return -1;   /* no image descriptor found */
}

void gif_free_rows(uint8_t** rows, int h) {
    if (rows == NULL) return;
    for (int y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

/* ── device/simulator bitmap wrapper ─────────────────────────────────── */

#ifdef PLUTO_GIF_PD
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} GifPixCtx;

static int gif_pix(void* ud, int x, int y) {
    GifPixCtx* c = (GifPixCtx*)ud;
    if (y >= c->h || c->rows[y] == NULL) return 255;
    return x < c->w ? c->rows[y][x] : 255;
}

struct LCDBitmap* gif_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    uint8_t** rows = NULL;
    int tw = 0, th = 0;
    if (gif_decode_gray(data, len, maxW, maxH, &rows, &tw, &th) != 0)
        return NULL;
    GifPixCtx ctx = { rows, tw, th };
    struct LCDBitmap* img =
        dither_to_image(pd, gif_pix, &ctx, tw, th);
    gif_free_rows(rows, th);
    return img;
}
#else
struct LCDBitmap* gif_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    (void)data;
    (void)len;
    (void)maxW;
    (void)maxH;
    (void)pd;
    return NULL;
}
#endif
