// bmp.c — C port of Source/render/decoders/bmp.lua (BMPDecoder).
//
// Pure on-device BMP decoder: parses uncompressed BMPs (1/4/8-bit
// palette-indexed and 24/32-bit truecolor), nearest-neighbor downscales
// to fit the Playdate screen and hands the pixels to Dither.toImage.

#include "render/decoders/bmp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render/decoders/dither.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_BMP_PD 1
#endif

/* little-endian readers mirroring the Lua helpers: missing bytes -> 0.
 * (contiguous ranges: checking first+last byte is equivalent to the
 * Lua `if not b1 or not b4` guard) */
static unsigned rd16le(const uint8_t* d, size_t len, size_t o) {
    if (o >= len || o + 1 >= len) return 0;
    return (unsigned)d[o] | ((unsigned)d[o + 1] << 8);
}

static uint32_t rd32le(const uint8_t* d, size_t len, size_t o) {
    if (o >= len || o + 3 >= len) return 0;
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
           ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

static int rd32les(const uint8_t* d, size_t len, size_t o) {
    uint32_t v = rd32le(d, len, o);
    if (v >= 2147483648u) v -= 4294967296u;
    return (int)v;
}

typedef struct {
    const uint8_t* data;
    size_t len;
    int pixelOffset;
    int width, height;
    int bpp;
    int isTopDown;
    int numColors;
    double scale;
    size_t rowBytes;
    uint8_t palette[256];
} BmpCtx;

/* getPixelGray parity: nearest-neighbor sample with clamped src coords */
static int bmp_sample(const BmpCtx* c, int outX, int outY) {
    const uint8_t* d = c->data;
    size_t len = c->len;

    int srcX = (int)floor(outX * c->scale);
    if (srcX > c->width - 1) srcX = c->width - 1;
    int srcY = (int)floor(outY * c->scale);
    if (srcY > c->height - 1) srcY = c->height - 1;
    int bmpY = c->isTopDown ? srcY : (c->height - 1 - srcY);

    size_t rowStart = (size_t)c->pixelOffset +
                      (size_t)bmpY * c->rowBytes;

    if (c->bpp == 24 || c->bpp == 32) {
        size_t stride = (size_t)c->bpp / 8;
        size_t pPos = rowStart + (size_t)srcX * stride;
        if (pPos < len && pPos + 2 < len)
            return dither_rgb_to_gray(d[pPos + 2], d[pPos + 1],
                                      d[pPos]);
        /* falls through to 255 like the Lua `if r and g and b` miss */
    } else if (c->bpp == 8) {
        size_t pPos = rowStart + (size_t)srcX;
        int idx = pPos < len ? d[pPos] : 0;   /* `idx or 0` */
        return idx < c->numColors ? c->palette[idx] : 0;
    } else if (c->bpp == 4) {
        size_t pPos = rowStart + (size_t)(srcX >> 1);
        int b = pPos < len ? d[pPos] : 0;
        int idx = (srcX % 2 == 0) ? ((b >> 4) & 0x0F) : (b & 0x0F);
        return idx < c->numColors ? c->palette[idx] : 0;
    } else if (c->bpp == 1) {
        size_t pPos = rowStart + (size_t)(srcX >> 3);
        int bitIdx = 7 - (srcX % 8);
        int b = pPos < len ? d[pPos] : 0;
        int idx = (b >> bitIdx) & 1;
        if (idx < c->numColors) return c->palette[idx];
        return idx == 1 ? 255 : 0;
    }
    return 255;   /* unsupported bpp (e.g. 16) */
}

int bmp_decode_gray(const uint8_t* data, size_t len,
                    uint8_t*** outRows, int* outW, int* outH) {
    *outRows = NULL;
    *outW = *outH = 0;
    if (data == NULL || len < 54) return -1;

    /* signature "BM" */
    if (data[0] != 'B' || data[1] != 'M') return -1;

    BmpCtx c;
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.len = len;
    /* Lua positions are 1-based: 11/15/19/23/29/31 -> C offsets 10..30 */
    c.pixelOffset = (int)rd32le(data, len, 10);
    int headerSize = (int)rd32le(data, len, 14);
    int width = rd32les(data, len, 18);
    int rawHeight = rd32les(data, len, 22);
    c.bpp = (int)rd16le(data, len, 28);
    int compression = rd32les(data, len, 30);   /* read but unchecked,
                                                   exactly like the source */
    (void)headerSize;
    (void)compression;

    if (width <= 0 || rawHeight == 0) return -1;
    c.width = width;
    c.isTopDown = rawHeight < 0;
    c.height = rawHeight < 0 ? -rawHeight : rawHeight;

    /* scale down if larger than the Playdate screen area */
    double scale = 1.0;
    if (width > 360 || c.height > 200) {
        double scaleX = (double)width / 360.0;
        double scaleY = (double)c.height / 200.0;
        scale = scaleX > scaleY ? scaleX : scaleY;
    }
    c.scale = scale;
    int targetW = (int)floor((double)width / scale);
    if (targetW < 1) targetW = 1;
    int targetH = (int)floor((double)c.height / scale);
    if (targetH < 1) targetH = 1;

    /* palette for indexed modes (1, 4, 8 bpp): built to the FULL
     * 1<<bpp size; entries beyond the file become 0 like the source */
    if (c.bpp <= 8 && c.bpp > 0) {
        c.numColors = 1 << c.bpp;
        if (c.numColors > 256) c.numColors = 256;
        size_t palOffset = 14 + (size_t)headerSize;   /* Lua 15-based */
        for (int i = 0; i < c.numColors; i++) {
            size_t pPos = palOffset + (size_t)i * 4;
            if (pPos < len && pPos + 2 < len) {
                int b = data[pPos], g = data[pPos + 1],
                    r = data[pPos + 2];
                c.palette[i] =
                    (uint8_t)dither_rgb_to_gray(r, g, b);
            } else {
                c.palette[i] = 0;
            }
        }
    }

    c.rowBytes = (size_t)(((c.bpp * width + 31) / 32) * 4);

    uint8_t** rows =
        (uint8_t**)pluto_malloc(sizeof(uint8_t*) * (size_t)targetH);
    if (rows == NULL) return -1;
    for (int y = 0; y < targetH; y++) {
        rows[y] = (uint8_t*)pluto_malloc((size_t)targetW);
        if (rows[y] == NULL) {
            bmp_free_rows(rows, y);
            return -1;
        }
        for (int x = 0; x < targetW; x++)
            rows[y][x] = (uint8_t)bmp_sample(&c, x, y);
    }

    *outRows = rows;
    *outW = targetW;
    *outH = targetH;
    return 0;
}

void bmp_free_rows(uint8_t** rows, int h) {
    if (rows == NULL) return;
    for (int y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

/* ── device/simulator bitmap wrapper ─────────────────────────────────── */

#ifdef PLUTO_BMP_PD
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} BmpGridCtx;

static int bmp_grid_pix(void* ud, int x, int y) {
    BmpGridCtx* g = (BmpGridCtx*)ud;
    if (y >= g->h || g->rows[y] == NULL) return 255;
    return x < g->w ? g->rows[y][x] : 255;
}

struct LCDBitmap* bmp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len) {
    uint8_t** rows = NULL;
    int tw = 0, th = 0;
    if (bmp_decode_gray(data, len, &rows, &tw, &th) != 0) return NULL;
    BmpGridCtx gc = { rows, tw, th };
    struct LCDBitmap* img =
        dither_to_image(pd, bmp_grid_pix, &gc, tw, th);
    bmp_free_rows(rows, th);
    return img;
}
#else
struct LCDBitmap* bmp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len) {
    (void)data;
    (void)len;
    (void)pd;
    return NULL;
}
#endif
