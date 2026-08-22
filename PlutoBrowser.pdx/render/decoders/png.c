// png.c — C port of Source/render/decoders/png.lua (PNGDecoder).
//
// Pure on-device PNG decoder: streams the zlib scanline data (never
// holding the full uncompressed image), unfilters one row at a time and
// box-filters down to ~screen size.

#include "render/decoders/png.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "render/decoders/dither.h"
#include "render/decoders/inflate.h"
#include "render/decoders/scale.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_PNG_PD 1
#endif

static uint32_t read_u32be(const uint8_t* s) {
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
           ((uint32_t)s[2] << 8) | (uint32_t)s[3];
}

static int paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Composite an 8-bit gray value over white given alpha 0..255 */
static int composite(int gray, int a) {
    if (a >= 255) return gray;
    if (a <= 0) return 255;
    return (int)floor((double)(gray * a + 255 * (255 - a)) / 255.0 + 0.5);
}

int png_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH) {
    *outRows = NULL;
    *outW = *outH = 0;
    if (data == NULL || len < 24) return -1;

    /* signature: 0x89 'P' 'N' 'G' */
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' ||
        data[3] != 'G')
        return -1;

    size_t pos = 8;   /* Lua pos = 9, 1-based */
    uint32_t width = 0, height = 0;
    int bitDepth = 8, colorType = 0, interlace = 0;

    uint8_t palette[256];      /* gray values */
    uint8_t paletteA[256];
    int nColors = 0;
    memset(paletteA, 255, sizeof(paletteA));

    uint8_t* idat = NULL;
    size_t idatLen = 0, idatCap = 0;
    uint8_t trns[300];
    size_t trnsLen = 0;

    while (pos < len) {
        if (pos + 8 > len) break;
        uint32_t chunkLen = read_u32be(data + pos);
        const uint8_t* type = data + pos + 4;
        size_t dataPos = pos + 8;
        pos = pos + 12 + chunkLen;
        if ((long long)pos > (long long)len + 12) break;   // Lua quirk

        if (memcmp(type, "IHDR", 4) == 0) {
            width = read_u32be(data + dataPos);
            height = read_u32be(data + dataPos + 4);
            bitDepth = dataPos + 8 < len ? data[dataPos + 8] : 8;
            colorType = dataPos + 9 < len ? data[dataPos + 9] : 0;
            interlace = dataPos + 12 < len ? data[dataPos + 12] : 0;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            nColors = (int)(chunkLen / 3);
            for (int i = 0; i < nColors && i < 256; i++) {
                size_t p = dataPos + (size_t)i * 3;
                int r = p < len ? data[p] : 0;
                int g = p + 1 < len ? data[p + 1] : 0;
                int b = p + 2 < len ? data[p + 2] : 0;
                palette[i] =
                    (uint8_t)dither_rgb_to_gray(r, g, b);
                paletteA[i] = 255;
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            trnsLen = chunkLen < sizeof(trns) ? chunkLen : sizeof(trns);
            memcpy(trns, data + dataPos, trnsLen);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idatLen + chunkLen > idatCap) {
                size_t nc = idatCap ? idatCap * 2 : 4096;
                while (nc < idatLen + chunkLen) nc *= 2;
                uint8_t* ni = (uint8_t*)pluto_realloc(idat, nc);
                if (ni == NULL) { pluto_free(idat); return -1; }
                idat = ni;
                idatCap = nc;
            }
            size_t avail = len - dataPos;
            size_t take = chunkLen < avail ? chunkLen : avail;
            memcpy(idat + idatLen, data + dataPos, take);
            idatLen += take;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    if (width == 0 || height == 0) return -1;
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    /* tRNS for palette: one alpha byte per entry */
    if (trnsLen > 0 && colorType == 3) {
        for (size_t i = 0; i < trnsLen && i < 256; i++)
            paletteA[i] = trns[i];
    }

    int channels = 1;
    if (colorType == 2) channels = 3;
    else if (colorType == 3) channels = 1;
    else if (colorType == 4) channels = 2;
    else if (colorType == 6) channels = 4;

    /* interlaced: first Adam7 pass only (every 8th pixel) */
    uint32_t srcW = width, srcH = height;
    if (interlace == 1) {
        srcW = (width + 7) / 8;
        if (srcW < 1) srcW = 1;
        srcH = (height + 7) / 8;
        if (srcH < 1) srcH = 1;
    }

    size_t rowBytes =
        ((size_t)srcW * channels * bitDepth + 7) / 8;
    size_t bppBytes = (size_t)channels * bitDepth / 8;
    if (bppBytes < 1) bppBytes = 1;
    size_t sampleBytes = (size_t)bitDepth / 8;
    if (sampleBytes < 1) sampleBytes = 1;

    /* NOTE: the stream aliases idat (C buffers are not Lua values),
     * so idat stays alive until after the decode loop */
    InflateStream* inflate = inflate_stream_new(idat, idatLen);
    if (inflate == NULL) {
        pluto_free(idat);
        return -1;
    }

    ScaleAccum* acc = scale_accum_new((int)srcW, (int)srcH, maxW, maxH);
    int boxW, boxH, targetW, targetH;
    scale_box_sizes((int)srcW, (int)srcH, maxW, maxH,
                    &boxW, &boxH, &targetW, &targetH);

    uint8_t* prevRow = (uint8_t*)pluto_malloc(rowBytes ? rowBytes : 1);
    uint8_t* curRow = (uint8_t*)pluto_malloc(rowBytes ? rowBytes : 1);
    uint8_t* rawRow = (uint8_t*)pluto_malloc(rowBytes ? rowBytes : 1);
    uint8_t* grayRow = (uint8_t*)pluto_malloc(srcW ? srcW : 1);
    if (prevRow == NULL || curRow == NULL || rawRow == NULL ||
        grayRow == NULL || acc == NULL) {
        pluto_free(prevRow);
        pluto_free(curRow);
        pluto_free(rawRow);
        pluto_free(grayRow);
        scale_accum_free(acc);
        inflate_stream_free(inflate);
        return -1;
    }
    memset(prevRow, 0, rowBytes);

    const int unpackMask = (bitDepth < 8) ? (1 << bitDepth) - 1 : 0;
    const int grayScale =
        (bitDepth < 8) ? 255 / unpackMask : 1;   /* integer div, like Lua */
    const int perByte = (bitDepth < 8) ? 8 / bitDepth : 1;

    /* tRNS keys for grayscale / truecolor */
    int tRNSgrayKey = -1;
    int tRNSkr = -1, tRNSkg = -1, tRNSkb = -1;
    if (trnsLen > 0) {
        if (colorType == 0 && trnsLen >= 2) tRNSgrayKey = trns[0];
        if (colorType == 2 && trnsLen >= 6) {
            tRNSkr = trns[0];
            tRNSkg = trns[2];
            tRNSkb = trns[4];
        }
    }

    for (uint32_t y = 0; y < srcH; y++) {
        tasks_yield_check();

        uint8_t header = 0;
        if (inflate_stream_read(inflate, &header, 1) != 1) break;
        int filterType = header;

        if (inflate_stream_read(inflate, rawRow, rowBytes) != rowBytes)
            break;

        /* unfilter byte-by-byte */
        for (size_t x = 0; x < rowBytes; x++) {
            int xv = rawRow[x];
            int a = (x >= bppBytes) ? curRow[x - bppBytes] : 0;
            int b = prevRow[x];
            int c = (x >= bppBytes) ? prevRow[x - bppBytes] : 0;
            switch (filterType) {
                case 1: curRow[x] = (uint8_t)((xv + a) & 0xFF); break;
                case 2: curRow[x] = (uint8_t)((xv + b) & 0xFF); break;
                case 3:
                    curRow[x] =
                        (uint8_t)((xv + ((a + b) >> 1)) & 0xFF);
                    break;
                case 4:
                    curRow[x] =
                        (uint8_t)((xv + paeth_predictor(a, b, c)) &
                                  0xFF);
                    break;
                default:
                    curRow[x] = (uint8_t)xv;   /* 0 and any unknown type */
            }
        }

#define CR(idx) ((idx) < rowBytes ? curRow[(idx)] : 0)
        /* convert to grayscale */
        if (colorType == 0) {
            if (bitDepth == 16) {
                for (uint32_t x = 0; x < srcW; x++)
                    grayRow[x] = CR(x * 2);
            } else if (bitDepth == 8) {
                for (uint32_t x = 0; x < srcW; x++) grayRow[x] = CR(x);
            } else {
                for (uint32_t x = 0; x < srcW; x++) {
                    size_t byteIdx = x / perByte;
                    int shift = 8 - bitDepth -
                                (int)((x % perByte) * bitDepth);
                    grayRow[x] =
                        (uint8_t)(((CR(byteIdx) >> shift) & unpackMask) *
                                  grayScale);
                }
            }
            if (tRNSgrayKey >= 0) {
                for (uint32_t x = 0; x < srcW; x++)
                    if (grayRow[x] == tRNSgrayKey) grayRow[x] = 255;
            }
        } else if (colorType == 3) {
            for (uint32_t x = 0; x < srcW; x++) {
                int idx;
                if (bitDepth == 8) {
                    idx = CR(x) + 1;
                } else {
                    size_t byteIdx = x / perByte;
                    int shift = 8 - bitDepth -
                                (int)((x % perByte) * bitDepth);
                    idx = ((CR(byteIdx) >> shift) & unpackMask) + 1;
                }
                int g = (idx >= 1 && idx <= nColors) ? palette[idx - 1]
                                                     : 255;
                int al = (idx >= 1 && idx <= nColors) ? paletteA[idx - 1]
                                                      : 255;
                grayRow[x] = (uint8_t)composite(g, al);
            }
        } else if (colorType == 2 || colorType == 6) {
            size_t step = 3;
            if (colorType == 6) step = 4;
            for (uint32_t x = 0; x < srcW; x++) {
                size_t p = (size_t)x * step * sampleBytes;
                int r = CR(p), g = CR(p + sampleBytes),
                    bl = CR(p + sampleBytes * 2);
                if (tRNSkr >= 0 && r == tRNSkr && g == tRNSkg &&
                    bl == tRNSkb) {
                    grayRow[x] = 255;
                    continue;
                }
                int gv = dither_rgb_to_gray(r, g, bl);
                if (colorType == 6)
                    grayRow[x] = (uint8_t)composite(
                        gv, p + sampleBytes * 3 < rowBytes
                                ? curRow[p + sampleBytes * 3]
                                : 255);
                else
                    grayRow[x] = (uint8_t)gv;
            }
        } else if (colorType == 4) {
            if (bitDepth == 16) {
                for (uint32_t x = 0; x < srcW; x++) {
                    size_t p = (size_t)x * 4;
                    grayRow[x] = (uint8_t)composite(
                        CR(p), p + 2 < rowBytes ? curRow[p + 2] : 255);
                }
            } else {
                for (uint32_t x = 0; x < srcW; x++) {
                    size_t p = (size_t)x * 2;
                    grayRow[x] = (uint8_t)composite(
                        CR(p), p + 1 < rowBytes ? curRow[p + 1] : 255);
                }
            }
        }
#undef CR

        scale_accum_add_row(acc, grayRow);

        uint8_t* tmp = prevRow;
        prevRow = curRow;
        curRow = tmp;
    }

    inflate_stream_free(inflate);
    pluto_free(idat);
    pluto_free(prevRow);
    pluto_free(curRow);
    pluto_free(rawRow);
    pluto_free(grayRow);

    int tw, th;
    int count = scale_accum_finish(acc, &tw, &th);
    if (count == 0) {
        scale_accum_free(acc);
        return -1;
    }
    /* convert accumulator int rows into a byte grid we hand off */
    uint8_t** rows =
        (uint8_t**)pluto_malloc(sizeof(uint8_t*) * (size_t)count);
    if (rows == NULL) {
        scale_accum_free(acc);
        return -1;
    }
    int okGrid = 1;
    for (int ry = 0; ry < count && okGrid; ry++) {
        rows[ry] = (uint8_t*)pluto_malloc((size_t)acc->targetW);
        if (rows[ry] == NULL) { okGrid = 0; break; }
        for (int cx = 0; cx < acc->targetW; cx++)
            rows[ry][cx] = (uint8_t)acc->out[ry][cx];
    }
    if (!okGrid) {
        for (int ry = 0; ry < count; ry++) pluto_free(rows[ry]);
        pluto_free(rows);
        scale_accum_free(acc);
        return -1;
    }
    *outRows = rows;
    *outW = acc->targetW;
    *outH = count;   /* actual rows produced (may exceed target) */
    (void)tw;
    (void)targetH;
    scale_accum_free(acc);
    return 0;
}

void png_free_rows(uint8_t** rows, int h) {
    if (rows == NULL) return;
    for (int y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

/* ── device/simulator bitmap wrapper ─────────────────────────────────── */

#ifdef PLUTO_PNG_PD
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} PngPixCtx;

static int png_pix(void* ud, int x, int y) {
    PngPixCtx* c = (PngPixCtx*)ud;
    if (y >= c->h || c->rows[y] == NULL) return 255;
    return x < c->w ? c->rows[y][x] : 255;
}

struct LCDBitmap* png_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    uint8_t** rows = NULL;
    int tw = 0, th = 0;
    if (png_decode_gray(data, len, maxW, maxH, &rows, &tw, &th) != 0)
        return NULL;
    PngPixCtx ctx = { rows, tw, th };
    struct LCDBitmap* img =
        dither_to_image(pd, png_pix, &ctx, tw, th);
    png_free_rows(rows, th);
    return img;
}
#else
struct LCDBitmap* png_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    (void)data;
    (void)len;
    (void)maxW;
    (void)maxH;
    (void)pd;
    return NULL;
}
#endif
