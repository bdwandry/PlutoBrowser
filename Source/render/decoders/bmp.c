/*
 * PlutoBrowser — bmp.c
 * Port of Source/render/decoders/bmp.lua (reference, 111 lines).
 * Pure on-device BMP decoder → Bayer-dithered 1-bit bitmap.
 * See bmp.h for the Lua→C map and preserved semantics.
 */
#include "core/logger.h"
#include <stdlib.h>
#include <string.h>
#include "render/decoders/bmp.h"
#include "render/decoders/dither.h"

extern PlaydateAPI *pluto_pd(void);

static uint16_t rd16(const uint8_t *d, size_t pos, size_t len)
{
    if (pos + 1 >= len)
    {
        return 0;
    }
    return (uint16_t)(d[pos] | ((uint16_t)d[pos + 1] << 8));
}

static uint32_t rd32(const uint8_t *d, size_t pos, size_t len)
{
    if (pos + 3 >= len)
    {
        return 0;
    }
    return (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) |
           ((uint32_t)d[pos + 2] << 16) | ((uint32_t)d[pos + 3] << 24);
}

static int32_t rds32(const uint8_t *d, size_t pos, size_t len)
{
    uint32_t v = rd32(d, pos, len);
    return (int32_t)v;
}

/* Shared BMP/DIB pixel getter (Lua getPixelGray closure). */
typedef struct
{
    const uint8_t *data;
    size_t len;
    int width, height;
    int isTopDown;
    uint32_t pixelOffset;
    int bpp;
    int rowBytes;
    int isOs2Pal; /* BITMAPCOREHEADER: 3-byte palette entries */
    uint8_t palette[256];
    /* downscale factor as a rational: scale = scaleNum / scaleDen
     * (Lua floor(outX * scale) computed exactly in integer math) */
    int scaleNum, scaleDen;
} BmpCtx;

static uint8_t bmp_pixel_gray(void *ud, int outX, int outY)
{
    BmpCtx *c = (BmpCtx *)ud;
    int srcX = outX * c->scaleNum; /* floor(outX * scale) via scaled int */
    srcX = srcX / c->scaleDen;
    if (srcX > c->width - 1)
    {
        srcX = c->width - 1;
    }
    int srcY = outY * c->scaleNum;
    srcY = srcY / c->scaleDen;
    if (srcY > c->height - 1)
    {
        srcY = c->height - 1;
    }
    int bmpY = c->isTopDown ? srcY : (c->height - 1 - srcY);
    size_t rowStart = c->pixelOffset + (size_t)bmpY * c->rowBytes;

    if (c->bpp == 24)
    {
        size_t p = rowStart + (size_t)srcX * 3;
        if (p + 2 < c->len)
        {
            return (uint8_t)dither_rgb_to_gray(c->data[p + 2], c->data[p + 1], c->data[p]);
        }
    }
    else if (c->bpp == 32)
    {
        size_t p = rowStart + (size_t)srcX * 4;
        if (p + 2 < c->len)
        {
            return (uint8_t)dither_rgb_to_gray(c->data[p + 2], c->data[p + 1], c->data[p]);
        }
    }
    else if (c->bpp == 8)
    {
        size_t p = rowStart + srcX;
        if (p < c->len)
        {
            return c->palette[c->data[p]];
        }
        return 0; /* Lua palette[idx or 0] or 0 */
    }
    else if (c->bpp == 4)
    {
        size_t p = rowStart + (srcX >> 1);
        uint8_t b = p < c->len ? c->data[p] : 0;
        int idx = (srcX % 2 == 0) ? ((b >> 4) & 0x0F) : (b & 0x0F);
        return c->palette[idx];
    }
    else if (c->bpp == 1)
    {
        size_t p = rowStart + (srcX >> 3);
        uint8_t b = p < c->len ? c->data[p] : 0;
        int idx = (b >> (7 - (srcX % 8))) & 1;
        /* Lua: palette[idx] or (idx==1 and 255 or 0) */
        if (idx == 1)
        {
            return 255;
        }
        return 0;
    }
    return 255;
}

/* Load the palette (bpp<=8): 1<<bpp entries of BGR at palOffset. */
static void bmp_load_palette(BmpCtx *c, size_t palOffset)
{
    int numColors = 1 << c->bpp;
    size_t stride = c->isOs2Pal ? 3 : 4; /* OS/2: RGBTRIPLE (3 bytes) */
    for (int i = 0; i < numColors && i < 256; i++)
    {
        size_t p = palOffset + (size_t)i * stride;
        if (p + 2 < c->len)
        {
            c->palette[i] = (uint8_t)dither_rgb_to_gray(c->data[p + 2], c->data[p + 1], c->data[p]);
        }
        else
        {
            c->palette[i] = 0;
        }
    }
}

LCDBitmap *bmp_decode(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 54)
    {
        return NULL;
    }
    if (data[0] != 'B' || data[1] != 'M')
    {
        return NULL;
    }

    BmpCtx c;
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.len = len;
    c.pixelOffset = rd32(data, 10, len);
    uint32_t headerSize = rd32(data, 14, len);
    uint32_t compression = 0;
    int32_t rawHeight;
    if (headerSize == 12)
    {
        /* OS/2 BITMAPCOREHEADER: RGBTRIPLE palette, uint16 dims. */
        c.width = rd16(data, 18, len);
        rawHeight = (int32_t)rd16(data, 20, len);
        c.bpp = rd16(data, 24, len);
        c.isOs2Pal = 1;
    }
    else
    {
        c.width = rds32(data, 18, len);
        rawHeight = rds32(data, 22, len);
        c.bpp = rd16(data, 28, len);
        compression = rd32(data, 30, len);
    }
    (void)compression; /* read but unchecked (Lua parity) */

    if (c.width <= 0 || rawHeight == 0)
    {
        return NULL;
    }
    c.isTopDown = rawHeight < 0;
    c.height = rawHeight < 0 ? -rawHeight : rawHeight;

    /* Downscale target (Lua: scale = max(w/360, h/200) when over). */
    int scaleNum = 1, scaleDen = 1; /* scale = num/den */
    int targetW = c.width, targetH = c.height;
    if (c.width > 360 || c.height > 200)
    {
        /* scale as a rational max(w/360, h/200). */
        int n1 = c.width, d1 = 360;
        int n2 = c.height, d2 = 200;
        if ((long long)n1 * d2 >= (long long)n2 * d1)
        {
            scaleNum = n1;
            scaleDen = d1;
        }
        else
        {
            scaleNum = n2;
            scaleDen = d2;
        }
        /* floor(width / scale) where scale = scaleNum/scaleDen:
         * width / (num/den) = width * den / num. (Dividing by scaleNum
         * alone collapsed every downscaled BMP to 1x1 — caught by the P33
         * benchmark's 200x150 BMP, which the small P22b vectors missed.) */
        targetW = (int)((long long)c.width * scaleDen / scaleNum);
        if (targetW < 1) targetW = 1;
        targetH = (int)((long long)c.height * scaleDen / scaleNum);
        if (targetH < 1) targetH = 1;
    }
    c.scaleNum = scaleNum;
    c.scaleDen = scaleDen;

    if (c.bpp <= 8)
    {
        if (c.bpp != 1 && c.bpp != 4 && c.bpp != 8)
        {
            return NULL; /* 2bpp is not a real BMP mode */
        }
        size_t palOffset = 14 + headerSize; /* Lua 15+headerSize (1-based) */
        bmp_load_palette(&c, palOffset);
    }

    c.rowBytes = ((c.bpp * c.width + 31) / 32) * 4;

    return dither_to_bitmap(targetW, targetH, bmp_pixel_gray, &c);
}
