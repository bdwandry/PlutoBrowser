// ico.c — C port of Source/render/decoders/ico.lua (ICODecoder).
//
// Parses the ICO container, then delegates to the PNG decoder for
// PNG-compressed entries or a small embedded-BMP reader for classic DIB
// entries. Classic icons store their DIB height doubled (image + trailing
// 1bpp AND mask); rows are bottom-up for a positive height. AND-mask bits
// set to 1 are fully transparent (composited over white), matching
// Chromium's ICO handling.

#include "render/decoders/ico.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "render/decoders/dither.h"
#include "render/decoders/png.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_ICO_PD 1
#endif

/* little-endian readers mirroring the Lua helpers: missing bytes -> 0. */
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

static int byte_at_or(const uint8_t* d, size_t len, size_t o, int def) {
    if (o >= len) return def;
    return d[o];
}

/* Lua composite(): gray composited over white by alpha. */
static int ico_composite(int gray, int a) {
    double v;
    if (a >= 255) return gray;
    if (a <= 0) return 255;
    v = floor(((double)gray * (double)a +
               255.0 * (double)(255 - a)) / 255.0 + 0.5);
    return (int)v;
}

typedef struct {
    const uint8_t* data;
    size_t len;
    int width, height;
    int bpp;
    int isTopDown;
    int numColors;
    uint8_t palette[256];
    double scale;
    size_t rowBytes;
    size_t andRowBytes;
    size_t pixOff;    /* 0-based first pixel-row byte */
    size_t maskOff;   /* 0-based first AND-mask byte */
    int hasMask;
} IcoDibCtx;

/* Decode a classic embedded DIB (BITMAPINFOHEADER with no "BM" file
 * header) into a grayscale grid, mirroring decodeDIB(). */
static int ico_decode_dib(const uint8_t* data, size_t len,
                          int maxW, int maxH,
                          uint8_t*** outRows, int* outW, int* outH) {
    IcoDibCtx c;
    uint8_t** rows;
    int headerSize, width, rawHeight, bpp, compression;
    int x, y;

    *outRows = NULL;
    *outW = *outH = 0;
    if (data == NULL || len < 40) return -1;

    headerSize = (int)rd32le(data, len, 0);
    width = rd32les(data, len, 4);
    rawHeight = rd32les(data, len, 8);
    bpp = (int)rd16le(data, len, 14);
    compression = (int)rd32le(data, len, 16);

    if (headerSize < 40 || width <= 0 || rawHeight == 0 ||
        compression != 0)
        return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return -1;

    memset(&c, 0, sizeof(c));
    c.data = data;
    c.len = len;
    c.width = width;
    c.bpp = bpp;
    /* The stored height is doubled (image rows + AND mask rows). */
    c.isTopDown = rawHeight < 0;
    {
        int absH = rawHeight < 0 ? -rawHeight : rawHeight;
        c.height = absH / 2;   /* math.floor(abs/2) */
    }
    if (c.height <= 0) return -1;

    /* palette for indexed modes: entries beyond the file become 0 */
    if (bpp <= 8) {
        int numColors = 1 << bpp;
        size_t palOff = (size_t)headerSize;   /* Lua headerSize + 1 */
        int i;
        if (numColors > 256) numColors = 256;
        c.numColors = numColors;
        for (i = 0; i < numColors; i++) {
            size_t pPos = palOff + (size_t)i * 4;
            if (pPos < len && pPos + 2 < len) {
                c.palette[i] = (uint8_t)dither_rgb_to_gray(
                    data[pPos + 2], data[pPos + 1], data[pPos]);
            } else {
                c.palette[i] = 0;
            }
        }
    }

    c.rowBytes = (size_t)(((bpp * width + 31) / 32) * 4);
    c.andRowBytes = (size_t)(((width + 31) / 32) * 4);
    c.pixOff = (size_t)headerSize +
               (bpp <= 8 ? (size_t)(1 << bpp) * 4 : 0);
    c.maskOff = c.pixOff + c.rowBytes * (size_t)c.height;
    c.hasMask =
        (c.maskOff + c.andRowBytes * (size_t)c.height) <= len;

    /* nearest-neighbor downscale when over the target box */
    {
        double scale = 1.0;
        if (width > maxW || c.height > maxH)
            scale = fmax((double)width / (double)maxW,
                         (double)c.height / (double)maxH);
        c.scale = scale;
    }
    {
        int targetW = (int)floor((double)width / c.scale);
        int targetH = (int)floor((double)c.height / c.scale);
        if (targetW < 1) targetW = 1;
        if (targetH < 1) targetH = 1;
        *outW = targetW;
        *outH = targetH;
    }

    rows = (uint8_t**)pluto_malloc(sizeof(uint8_t*) * (size_t)*outH);
    if (rows == NULL) { *outW = *outH = 0; return -1; }

#define ICO_GRAY_MISSING 0
#define ICO_ALPHA_MISSING 255
    for (y = 0; y < *outH; y++) {
        rows[y] = (uint8_t*)pluto_malloc((size_t)*outW);
        if (rows[y] == NULL) {
            ico_free_rows(rows, y);
            *outW = *outH = 0;
            return -1;
        }
        for (x = 0; x < *outW; x++) {
            const uint8_t* d = c.data;
            size_t len_ = c.len;
            int srcX = (int)floor((double)x * c.scale);
            int srcY = (int)floor((double)y * c.scale);
            int bmpY, alpha, gray = 255;
            size_t rowStart;

            if (srcX > c.width - 1) srcX = c.width - 1;
            if (srcY > c.height - 1) srcY = c.height - 1;
            bmpY = c.isTopDown ? srcY : (c.height - 1 - srcY);
            rowStart = c.pixOff + (size_t)bmpY * c.rowBytes;

            if (c.bpp == 32) {
                size_t pPos = rowStart + (size_t)srcX * 4;
                int b = byte_at_or(d, len_, pPos, ICO_GRAY_MISSING);
                int g = byte_at_or(d, len_, pPos + 1, ICO_GRAY_MISSING);
                int r = byte_at_or(d, len_, pPos + 2, ICO_GRAY_MISSING);
                gray = dither_rgb_to_gray(r, g, b);
                alpha = byte_at_or(d, len_, pPos + 3, ICO_ALPHA_MISSING);
            } else if (c.bpp == 24) {
                size_t pPos = rowStart + (size_t)srcX * 3;
                int b = byte_at_or(d, len_, pPos, ICO_GRAY_MISSING);
                int g = byte_at_or(d, len_, pPos + 1, ICO_GRAY_MISSING);
                int r = byte_at_or(d, len_, pPos + 2, ICO_GRAY_MISSING);
                gray = dither_rgb_to_gray(r, g, b);
                alpha = 255;
            } else if (c.bpp == 8) {
                int idx = byte_at_or(d, len_,
                                     rowStart + (size_t)srcX,
                                     ICO_GRAY_MISSING);
                gray = idx < c.numColors ? c.palette[idx]
                                         : ICO_GRAY_MISSING;
                alpha = 255;
            } else if (c.bpp == 4) {
                int b = byte_at_or(d, len_,
                                   rowStart + (size_t)(srcX >> 1),
                                   ICO_GRAY_MISSING);
                int idx = (srcX % 2 == 0) ? ((b >> 4) & 0x0F)
                                          : (b & 0x0F);
                gray = idx < c.numColors ? c.palette[idx]
                                         : ICO_GRAY_MISSING;
                alpha = 255;
            } else {   /* bpp == 1 */
                int b = byte_at_or(d, len_,
                                   rowStart + (size_t)(srcX >> 3),
                                   ICO_GRAY_MISSING);
                int idx = (b >> (7 - (srcX % 8))) & 1;
                gray = idx < c.numColors
                           ? c.palette[idx]
                           : (idx == 1 ? 255 : ICO_GRAY_MISSING);
                alpha = 255;
            }

            /* AND mask bit set means fully transparent -> white. */
            if (c.hasMask) {
                size_t mRow = c.maskOff +
                              (size_t)bmpY * c.andRowBytes;
                int mb = byte_at_or(d, len_,
                                    mRow + (size_t)(srcX >> 3),
                                    ICO_GRAY_MISSING);
                if (((mb >> (7 - (srcX % 8))) & 1) == 1) {
                    rows[y][x] = 255;
                    continue;
                }
            }

            if (alpha < 255) gray = ico_composite(gray, alpha);
            rows[y][x] = (uint8_t)gray;
        }
    }
#undef ICO_GRAY_MISSING
#undef ICO_ALPHA_MISSING

    *outRows = rows;
    return 0;
}

typedef struct {
    int w, h, bpp;
    uint32_t offset, size;
} IcoEntry;

/* Best entry: largest area, then highest bit depth (matches Chromium).
 * Equal area+bpp keeps file order (stable tie-break). */
static int entry_cmp(const void* pa, const void* pb) {
    const IcoEntry* a = (const IcoEntry*)pa;
    const IcoEntry* b = (const IcoEntry*)pb;
    long areaA = (long)a->w * (long)a->h;
    long areaB = (long)b->w * (long)b->h;
    if (areaA != areaB) return areaA > areaB ? -1 : 1;
    if (a->bpp != b->bpp) return a->bpp > b->bpp ? -1 : 1;
    return 0;
}

int ico_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH) {
    unsigned reserved, fileType, count, i;
    IcoEntry* entries = NULL;
    int nEntries = 0;

    if (outRows == NULL || outW == NULL || outH == NULL) return -1;
    *outRows = NULL;
    *outW = *outH = 0;
    if (data == NULL || len < 22) return -1;
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    /* ICONDIR: reserved(2)=0, type(2): 1=icon, 2=cursor, count(2) */
    reserved = rd16le(data, len, 0);
    fileType = rd16le(data, len, 2);
    count = rd16le(data, len, 4);
    if (reserved != 0 || (fileType != 1 && fileType != 2) || count == 0)
        return -1;

    entries = (IcoEntry*)pluto_malloc(sizeof(IcoEntry) * count);
    if (entries == NULL) return -1;
    for (i = 0; i < count; i++) {
        size_t base = 6 + (size_t)i * 16;   /* Lua 7 + i*16 */
        IcoEntry e;
        int w = byte_at_or(data, len, base, 0);
        int h = byte_at_or(data, len, base + 1, 0);
        if (w == 0) w = 256;
        if (h == 0) h = 256;
        e.w = w;
        e.h = h;
        e.bpp = (fileType == 1)
                    ? (int)rd16le(data, len, base + 6) : 0;
        e.size = rd32le(data, len, base + 8);
        e.offset = rd32le(data, len, base + 12);
        if (e.offset > 0 && e.size > 0 &&
            (size_t)e.offset + (size_t)e.size <= len)
            entries[nEntries++] = e;
    }
    if (nEntries == 0) {
        pluto_free(entries);
        return -1;
    }
    qsort(entries, (size_t)nEntries, sizeof(IcoEntry), entry_cmp);

    /* Try each entry in order until one decodes successfully. */
    {
        int j;
        for (j = 0; j < nEntries; j++) {
            const IcoEntry* e = &entries[j];
            const uint8_t* chunk = data + e->offset;
            if (chunk[0] == 0x89 && chunk[1] == 0x50 &&
                chunk[2] == 0x4E && chunk[3] == 0x47) {
                /* pcall-style containment: a failed PNG entry just
                 * falls through to the next one */
                uint8_t** rows = NULL;
                int w = 0, h = 0;
                if (png_decode_gray(chunk, e->size, maxW, maxH,
                                    &rows, &w, &h) == 0 && rows) {
                    pluto_free(entries);
                    *outRows = rows;
                    *outW = w;
                    *outH = h;
                    return 0;
                }
            } else {
                uint8_t** rows = NULL;
                int w = 0, h = 0;
                if (ico_decode_dib(chunk, e->size, maxW, maxH,
                                   &rows, &w, &h) == 0 && rows) {
                    pluto_free(entries);
                    *outRows = rows;
                    *outW = w;
                    *outH = h;
                    return 0;
                }
            }
        }
    }
    pluto_free(entries);
    return -1;
}

void ico_free_rows(uint8_t** rows, int h) {
    if (rows == NULL) return;
    for (; h > 0; h--) pluto_free(rows[h - 1]);
    pluto_free(rows);
}

/* ── device/simulator bitmap wrapper ─────────────────────────────────── */

#ifdef PLUTO_ICO_PD
#include "pd_api.h"

typedef struct {
    uint8_t** rows;
    int w, h;
} IcoGridCtx;

static int ico_grid_pix(void* ud, int x, int y) {
    IcoGridCtx* g = (IcoGridCtx*)ud;
    if (y >= g->h || g->rows[y] == NULL) return 255;
    return x < g->w ? g->rows[y][x] : 255;
}

struct LCDBitmap* ico_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    uint8_t** rows = NULL;
    int tw = 0, th = 0;
    IcoGridCtx gc;
    struct LCDBitmap* img;
    if (ico_decode_gray(data, len, maxW, maxH, &rows, &tw, &th) != 0)
        return NULL;
    gc.rows = rows;
    gc.w = tw;
    gc.h = th;
    img = dither_to_image(pd, ico_grid_pix, &gc, tw, th);
    ico_free_rows(rows, th);
    return img;
}
#else
struct LCDBitmap* ico_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH) {
    (void)data;
    (void)len;
    (void)pd;
    (void)maxW;
    (void)maxH;
    return NULL;
}
#endif
