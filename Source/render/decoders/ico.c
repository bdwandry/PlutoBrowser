/*
 * PlutoBrowser — ico.c
 * Port of Source/render/decoders/ico.lua (reference, 185 lines).
 * ICO container + classic embedded-DIB decoder → dithered 1-bit bitmap.
 * See ico.h for the Lua→C map and the PNG-entry deviation note.
 */
#include "core/logger.h"
#include <stdlib.h>
#include <string.h>
#include "render/decoders/ico.h"
#include "render/decoders/dither.h"
#include "render/decoders/png.h"

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
    return (int32_t)rd32(d, pos, len);
}

/* Lua composite(gray, a): alpha toward white, half-up. */
static uint8_t ico_composite(int gray, int a)
{
    if (a >= 255)
    {
        return (uint8_t)gray;
    }
    if (a <= 0)
    {
        return 255;
    }
    return (uint8_t)((gray * a + 255 * (255 - a)) / 255 + 0.5);
}

/* DIB pixel getter (Lua decodeDIB getPixelGray closure). */
typedef struct
{
    const uint8_t *data;
    size_t len;
    int width, height;
    int isTopDown;
    size_t pixelOffset;
    int bpp;
    int rowBytes;
    size_t andMaskOffset;
    int andRowBytes;
    int hasMask;
    uint8_t palette[256];
    int scaleNum, scaleDen;
} IcoDibCtx;

static uint8_t ico_dib_pixel_gray(void *ud, int outX, int outY)
{
    IcoDibCtx *c = (IcoDibCtx *)ud;
    int srcX = outX * c->scaleNum / c->scaleDen;
    if (srcX > c->width - 1)
    {
        srcX = c->width - 1;
    }
    int srcY = outY * c->scaleNum / c->scaleDen;
    if (srcY > c->height - 1)
    {
        srcY = c->height - 1;
    }
    int bmpY = c->isTopDown ? srcY : (c->height - 1 - srcY);
    size_t rowStart = c->pixelOffset + (size_t)bmpY * c->rowBytes;

    int gray = 0;
    int alpha = 255;

    if (c->bpp == 32)
    {
        size_t p = rowStart + (size_t)srcX * 4;
        uint8_t b = p < c->len ? c->data[p] : 0;
        uint8_t g = p + 1 < c->len ? c->data[p + 1] : 0;
        uint8_t r = p + 2 < c->len ? c->data[p + 2] : 0;
        uint8_t a = p + 3 < c->len ? c->data[p + 3] : 255;
        gray = dither_rgb_to_gray(r, g, b);
        alpha = a;
    }
    else if (c->bpp == 24)
    {
        size_t p = rowStart + (size_t)srcX * 3;
        uint8_t b = p < c->len ? c->data[p] : 0;
        uint8_t g = p + 1 < c->len ? c->data[p + 1] : 0;
        uint8_t r = p + 2 < c->len ? c->data[p + 2] : 0;
        gray = dither_rgb_to_gray(r, g, b);
    }
    else if (c->bpp == 8)
    {
        size_t p = rowStart + srcX;
        int idx = p < c->len ? c->data[p] : 0;
        gray = c->palette[idx];
    }
    else if (c->bpp == 4)
    {
        size_t p = rowStart + (srcX >> 1);
        uint8_t b = p < c->len ? c->data[p] : 0;
        int idx = (srcX % 2 == 0) ? ((b >> 4) & 0x0F) : (b & 0x0F);
        gray = c->palette[idx];
    }
    else /* 1bpp */
    {
        size_t p = rowStart + (srcX >> 3);
        uint8_t b = p < c->len ? c->data[p] : 0;
        int idx = (b >> (7 - (srcX % 8))) & 1;
        gray = (idx == 1) ? 255 : 0;
    }

    /* AND mask bit set = fully transparent (white). */
    if (c->hasMask)
    {
        size_t mRow = c->andMaskOffset + (size_t)bmpY * c->andRowBytes;
        size_t mp = mRow + (srcX >> 3);
        uint8_t mb = mp < c->len ? c->data[mp] : 0;
        if ((mb >> (7 - (srcX % 8))) & 1)
        {
            return 255;
        }
    }

    if (alpha < 255)
    {
        return ico_composite(gray, alpha);
    }
    return (uint8_t)gray;
}

/* Lua decodeDIB: classic embedded BITMAPINFOHEADER (no "BM" header). */
static LCDBitmap *ico_decode_dib(const uint8_t *data, size_t len, int maxW, int maxH)
{
    if (!data || len < 40)
    {
        return NULL;
    }

    uint32_t headerSize = rd32(data, 0, len);
    int width = rds32(data, 4, len);
    int32_t rawHeight = rds32(data, 8, len);
    int bpp = rd16(data, 14, len);
    uint32_t compression = rd32(data, 16, len);

    if (headerSize < 40 || width <= 0 || rawHeight == 0 || compression != 0)
    {
        return NULL;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
    {
        return NULL;
    }

    int isTopDown = rawHeight < 0;
    int height = (rawHeight < 0 ? -rawHeight : rawHeight) / 2; /* doubled */
    if (height <= 0)
    {
        return NULL;
    }

    IcoDibCtx c;
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.len = len;
    c.width = width;
    c.height = height;
    c.isTopDown = isTopDown;
    c.bpp = bpp;

    if (bpp <= 8)
    {
        size_t palOffset = headerSize; /* Lua headerSize+1 (1-based) */
        int numColors = 1 << bpp;
        for (int i = 0; i < numColors && i < 256; i++)
        {
            size_t p = palOffset + (size_t)i * 4;
            if (p + 2 < len)
            {
                c.palette[i] = (uint8_t)dither_rgb_to_gray(data[p + 2], data[p + 1], data[p]);
            }
            else
            {
                c.palette[i] = 0;
            }
        }
    }

    c.rowBytes = ((bpp * width + 31) / 32) * 4;
    c.andRowBytes = ((width + 31) / 32) * 4;
    c.pixelOffset = headerSize + ((bpp <= 8) ? (size_t)(1 << bpp) * 4 : 0);
    c.andMaskOffset = c.pixelOffset + (size_t)c.rowBytes * height;
    c.hasMask = (c.andMaskOffset + (size_t)c.andRowBytes * height) <= len;

    int scaleNum = 1, scaleDen = 1;
    int targetW = width, targetH = height;
    if (width > maxW || height > maxH)
    {
        int n1 = width, d1 = maxW;
        int n2 = height, d2 = maxH;
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
        targetW = width / scaleNum;
        if (targetW < 1) targetW = 1;
        targetH = height / scaleNum;
        if (targetH < 1) targetH = 1;
    }
    c.scaleNum = scaleNum;
    c.scaleDen = scaleDen;

    return dither_to_bitmap(targetW, targetH, ico_dib_pixel_gray, &c);
}

LCDBitmap *ico_decode(const uint8_t *data, size_t len, int maxW, int maxH)
{
    logger_stack_touch();
    if (!data || len < 22)
    {
        return NULL;
    }

    uint16_t reserved = rd16(data, 0, len);
    uint16_t fileType = rd16(data, 2, len);
    uint16_t count = rd16(data, 4, len);
    if (reserved != 0 || (fileType != 1 && fileType != 2) || count == 0)
    {
        return NULL;
    }

    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;

    /* Collect valid entries. */
    int *ew = NULL, *eh = NULL, *ebpp = NULL;
    size_t *eoff = NULL, *esize = NULL;
    int n = 0;
    int cap = count > 0 ? count : 1;
    ew = (int *)malloc(sizeof(int) * (size_t)cap);
    eh = (int *)malloc(sizeof(int) * (size_t)cap);
    ebpp = (int *)malloc(sizeof(int) * (size_t)cap);
    eoff = (size_t *)malloc(sizeof(size_t) * (size_t)cap);
    esize = (size_t *)malloc(sizeof(size_t) * (size_t)cap);
    if (!ew || !eh || !ebpp || !eoff || !esize)
    {
        free(ew); free(eh); free(ebpp); free(eoff); free(esize);
        return NULL;
    }

    for (int i = 0; i < count; i++)
    {
        size_t base = 6 + (size_t)i * 16;
        int w = base < len ? data[base] : 0;
        int h = base + 1 < len ? data[base + 1] : 0;
        if (w == 0) w = 256;
        if (h == 0) h = 256;
        int bitCount = (fileType == 1) ? rd16(data, base + 6, len) : 0;
        uint32_t byteSize = rd32(data, base + 8, len);
        uint32_t imageOffset = rd32(data, base + 12, len);
        if (imageOffset > 0 && byteSize > 0 && (size_t)imageOffset + byteSize <= len)
        {
            ew[n] = w;
            eh[n] = h;
            ebpp[n] = bitCount;
            eoff[n] = imageOffset;
            esize[n] = byteSize;
            n++;
        }
    }
    if (n == 0)
    {
        free(ew); free(eh); free(ebpp); free(eoff); free(esize);
        return NULL;
    }

    /* Sort: largest area first, then highest bpp (Chromium). Stable
     * insertion sort preserves the file order of equal entries. */
    for (int i = 1; i < n; i++)
    {
        int tw = ew[i], th = eh[i], tb = ebpp[i];
        size_t to = eoff[i], ts = esize[i];
        int j = i - 1;
        while (j >= 0)
        {
            int ja = ew[j] * eh[j];
            int ta = tw * th;
            if (ja < ta || (ja == ta && ebpp[j] < tb))
            {
                ew[j + 1] = ew[j]; eh[j + 1] = eh[j]; ebpp[j + 1] = ebpp[j];
                eoff[j + 1] = eoff[j]; esize[j + 1] = esize[j];
                j--;
            }
            else
            {
                break;
            }
        }
        ew[j + 1] = tw; eh[j + 1] = th; ebpp[j + 1] = tb;
        eoff[j + 1] = to; esize[j + 1] = ts;
    }

    /* Try each entry in order until one decodes. */
    LCDBitmap *result = NULL;
    for (int i = 0; i < n && !result; i++)
    {
        const uint8_t *chunk = data + eoff[i];
        size_t clen = esize[i];
        int isPng = clen >= 4 && chunk[0] == 0x89 && chunk[1] == 0x50 &&
                    chunk[2] == 0x4E && chunk[3] == 0x47;
        if (isPng)
        {
            /* Lua pcall-decodes via PNGDecoder (and retries the next entry
             * on failure) — a NULL here continues the loop identically. */
            result = png_decode(chunk, clen, maxW, maxH);
        }
        else
        {
            result = ico_decode_dib(chunk, clen, maxW, maxH);
        }
    }

    free(ew); free(eh); free(ebpp); free(eoff); free(esize);
    return result;
}
