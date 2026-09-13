/*
 * PlutoBrowser — tga.c
 * TGA decoder (see tga.h). Expands the file into an 8-bit gray raster
 * (truecolor via luminance, palette via 24/32-bit BGR colormap entries, or
 * direct 8-bit gray), then downsamples + Bayer-dithers like bmp.c/tif.c.
 * Handles types 1/2/3 and RLE 9/10/11, bottom-up and top-down, image-ID skip.
 */
#include <string.h>
#include <stdlib.h>
#include "tga.h"
#include "dither.h"
#include "scale.h"
#include "../../core/logger.h"


typedef struct
{
    int width, height;
    int bpp;
    int cmapBpp;
    const uint8_t *cmap;
    size_t cmapLen;
    int cmapStart;
    int hasCmap;
    const uint8_t *pix;
    size_t pixLen;
    int rle;
    /* sequential RLE/raw cursor */
    size_t rp;
    int runRemain;
    int blockIsRle;
    uint8_t rlePix[4];
    int scaleNum, scaleDen;
} TgaCtx;

/* Read the next pixel from the stream (RLE-aware). Returns pointer to the
 * pixel bytes or NULL at end of data. */
static const uint8_t *tga_next_pixel(TgaCtx *c)
{
    int bytes = c->bpp / 8;
    if (c->rle)
    {
        if (c->runRemain == 0)
        {
            if (c->rp >= c->pixLen) return NULL;
            int hdr = c->pix[c->rp++];
            c->blockIsRle = (hdr & 0x80) != 0;
            c->runRemain = (hdr & 0x7F) + 1;
            if (c->blockIsRle)
            {
                if (c->rp + (size_t)bytes > c->pixLen) return NULL;
                memcpy(c->rlePix, c->pix + c->rp, (size_t)bytes);
                c->rp += (size_t)bytes;
            }
        }
        c->runRemain--;
        if (c->blockIsRle) return c->rlePix;
        if (c->rp + (size_t)bytes > c->pixLen) return NULL;
        const uint8_t *p = c->pix + c->rp;
        c->rp += (size_t)bytes;
        return p;
    }
    if (c->rp + (size_t)bytes > c->pixLen) return NULL;
    const uint8_t *p = c->pix + c->rp;
    c->rp += (size_t)bytes;
    return p;
}

static uint8_t tga_gray(TgaCtx *c, const uint8_t *p)
{
    if (c->bpp == 8)
    {
        if (c->hasCmap && c->cmap && c->cmapLen)
        {
            int bytes = c->cmapBpp / 8;
            size_t e = (size_t)(c->cmapStart + p[0]) * (size_t)bytes;
            if (e + 2 < c->cmapLen)
            {
                return (uint8_t)dither_rgb_to_gray(c->cmap[e + 2],
                                                   c->cmap[e + 1], c->cmap[e]);
            }
            return 0;
        }
        return p[0];
    }
    if (c->bpp == 24 || c->bpp == 32)
    {
        return (uint8_t)dither_rgb_to_gray(p[2], p[1], p[0]);
    }
    if (c->bpp == 16)
    {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
        int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        return (uint8_t)dither_rgb_to_gray(r << 3, g << 3, b << 3);
    }
    return 0;
}

typedef struct
{
    TgaCtx *c;
    uint8_t *rows; /* full-size gray raster, w*h */
} TgaPix;

static uint8_t tga_pixel_gray(void *ud, int outX, int outY)
{
    TgaPix *p = (TgaPix *)ud;
    TgaCtx *c = p->c;
    int srcX = outX * c->scaleNum / c->scaleDen;
    int srcY = outY * c->scaleNum / c->scaleDen;
    if (srcX > c->width - 1) srcX = c->width - 1;
    if (srcY > c->height - 1) srcY = c->height - 1;
    return p->rows[(size_t)srcY * (size_t)c->width + (size_t)srcX];
}

LCDBitmap *tga_decode(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 18) return NULL;
    int idLen = data[0];
    int cmapType = data[1];
    int imgType = data[2];
    int cmapStart = data[3] | (data[4] << 8);
    int cmapCount = data[5] | (data[6] << 8);
    int cmapDepth = data[7];
    int w = data[12] | (data[13] << 8);
    int h = data[14] | (data[15] << 8);
    int bpp = data[16];
    int desc = data[17];

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    int rle = 0;
    if (imgType == 1 || imgType == 2 || imgType == 3)
        rle = 0;
    else if (imgType == 9 || imgType == 10 || imgType == 11)
        rle = 1;
    else
        return NULL;

    size_t off = 18 + (size_t)idLen;
    const uint8_t *cmap = NULL;
    size_t cmapLen = 0;
    if (cmapType == 1)
    {
        cmapLen = (size_t)cmapCount * (size_t)(cmapDepth / 8);
        if (cmapDepth != 15 && cmapDepth != 16 && cmapDepth != 24 &&
            cmapDepth != 32)
            return NULL;
        if (off + cmapLen > len) return NULL;
        cmap = data + off;
        off += cmapLen;
    }

    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return NULL;
    if (cmapType == 1 && bpp != 8) return NULL;
    if (cmapType == 0 && bpp == 8 && imgType != 3) return NULL;

    TgaCtx c;
    memset(&c, 0, sizeof(c));
    c.width = w;
    c.height = h;
    c.bpp = bpp;
    c.cmapBpp = cmapDepth;
    c.cmap = cmap;
    c.cmapLen = cmapLen;
    c.cmapStart = cmapStart;
    c.hasCmap = cmapType == 1;
    c.pix = data + off;
    c.pixLen = len - off;
    c.rle = rle;

    /* Expand sequentially in FILE order, then place rows per orientation. */
    size_t raster = (size_t)w * (size_t)h;
    uint8_t *rows = (uint8_t *)calloc(raster, 1);
    if (!rows) return NULL;

    int isTopDown = (desc & 0x20) != 0;
    int fileY0 = isTopDown ? 0 : h - 1;
    int fileStep = isTopDown ? 1 : -1;
    int ok = 1;
    for (int y = 0; y < h && ok; y++)
    {
        int dstY = fileY0 + y * fileStep;
        for (int x = 0; x < w; x++)
        {
            const uint8_t *p = tga_next_pixel(&c);
            if (!p)
            {
                ok = 0; /* truncated: stop, keep decoded part + white fill */
                break;
            }
            rows[(size_t)dstY * (size_t)w + (size_t)x] = tga_gray(&c, p);
        }
    }

    LCDBitmap *img = NULL;
    if (ok)
    {
        /* Downscale target (bmp.c conventions). */
        int scaleNum = 1, scaleDen = 1;
        int targetW = w, targetH = h;
        if (w > 360 || h > 200)
        {
            if ((long long)w * 200 >= (long long)h * 360)
            {
                scaleNum = w; scaleDen = 360;
            }
            else
            {
                scaleNum = h; scaleDen = 200;
            }
            targetW = (int)((long long)w * scaleDen / scaleNum);
            if (targetW < 1) targetW = 1;
            targetH = (int)((long long)h * scaleDen / scaleNum);
            if (targetH < 1) targetH = 1;
        }
        c.scaleNum = scaleNum;
        c.scaleDen = scaleDen;

        TgaPix tp;
        tp.c = &c;
        tp.rows = rows;
        img = dither_to_bitmap(targetW, targetH, tga_pixel_gray, &tp);
    }
    free(rows);
        if (img)
    {
        logger_log("TGA ok" );
    }
    return img;
}
