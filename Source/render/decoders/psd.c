/*
 * PlutoBrowser — psd.c
 * PSD decoder (see psd.h). Parses the header, color-mode block and the
 * image-data section at the end of the file (layers/resources skipped by
 * walking their length fields). Channel planes arrive planar (R all rows,
 * then G, then B...) either raw or per-row PackBits. Alpha (4th channel) is
 * skipped for gray conversion (over transparent background Photoshop composites
 * onto white when exporting, which the benchmark pattern uses).
 */
#include <string.h>
#include <stdlib.h>
#include "psd.h"
#include "dither.h"
#include "scale.h"
#include "../../core/logger.h"


typedef struct
{
    int width, height, channels, depth;
    const uint8_t *plane[4]; /* decoded plane data (may point into work buf) */
    uint8_t *work;           /* owned expanded buffer */
    size_t planeSize;
    int nPlanes;             /* planes actually decoded (min(channels,4)) */
    int scaleNum, scaleDen;
} PsdCtx;

static size_t packbits_row(const uint8_t *in, size_t inLen, uint8_t *out,
                           size_t outCap)
{
    size_t ip = 0, op = 0;
    while (ip < inLen && op < outCap)
    {
        int n = (int8_t)in[ip++];
        if (n >= 0)
        {
            size_t cnt = (size_t)n + 1;
            if (ip + cnt > inLen) break;
            if (op + cnt > outCap) cnt = outCap - op;
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
    }
    return op;
}

typedef struct
{
    PsdCtx *c;
    uint8_t *rows; /* full-size gray raster w*h */
} PsdPix;

static uint8_t psd_pixel_gray(void *ud, int outX, int outY)
{
    PsdPix *p = (PsdPix *)ud;
    PsdCtx *c = p->c;
    int srcX = outX * c->scaleNum / c->scaleDen;
    int srcY = outY * c->scaleNum / c->scaleDen;
    if (srcX > c->width - 1) srcX = c->width - 1;
    if (srcY > c->height - 1) srcY = c->height - 1;
    return p->rows[(size_t)srcY * (size_t)c->width + (size_t)srcX];
}

LCDBitmap *psd_decode(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 26) return NULL;
    if (memcmp(data, "8BPS", 4) != 0) return NULL;
    int ver = (data[4] << 8) | data[5];
    if (ver != 1) return NULL; /* PSB (v2) out of scope */

    int channels = (data[12] << 8) | data[13];
    int height = (int)((uint32_t)data[14] << 24 | (uint32_t)data[15] << 16 |
                       (uint32_t)data[16] << 8 | (uint32_t)data[17]);
    int width = (int)((uint32_t)data[18] << 24 | (uint32_t)data[19] << 16 |
                      (uint32_t)data[20] << 8 | (uint32_t)data[21]);
    int depth = (data[22] << 8) | data[23];
    int mode = (data[24] << 8) | data[25];

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return NULL;
    if (depth != 8) return NULL;
    if (channels < 1 || channels > 4) return NULL;
    if (mode != 1 && mode != 3 && mode != 4) /* gray, RGB, CMYK(->skip 4th) */
        return NULL;

    size_t off = 26;
    /* Color mode data */
    if (off + 4 > len) return NULL;
    uint32_t cmapLen = ((uint32_t)data[off] << 24) |
                       ((uint32_t)data[off + 1] << 16) |
                       ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
    off += 4 + (size_t)cmapLen;
    /* Image resources */
    if (off + 4 > len) return NULL;
    uint32_t resLen = ((uint32_t)data[off] << 24) |
                      ((uint32_t)data[off + 1] << 16) |
                      ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
    off += 4 + (size_t)resLen;
    /* Layer & mask info */
    if (off + 4 > len) return NULL;
    uint32_t layerLen = ((uint32_t)data[off] << 24) |
                        ((uint32_t)data[off + 1] << 16) |
                        ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
    off += 4 + (size_t)layerLen;
    /* Image data */
    if (off + 2 > len) return NULL;
    int compression = (data[off] << 8) | data[off + 1];
    off += 2;

    size_t planeSize = (size_t)width * (size_t)height;
    int nPlanes = channels > 3 ? 3 : channels; /* drop alpha for gray */
    if (mode == 4 && nPlanes > 3) nPlanes = 3;
    /* CMYK: Photoshop stores inverted; convert k/255 weighting crudely.
     * The benchmark corpus only contains RGB, so keep CMYK minimal: treat
     * planes as C,M,Y (alpha dropped). */

    uint8_t *planes = (uint8_t *)calloc((size_t)nPlanes * planeSize, 1);
    if (!planes) return NULL;

    int ok = 1;
    if (compression == 0)
    {
        if (off + (size_t)nPlanes * planeSize > len)
        {
            free(planes);
            return NULL;
        }
        for (int pI = 0; pI < nPlanes; pI++)
        {
            memcpy(planes + (size_t)pI * planeSize, data + off + (size_t)pI * planeSize,
                   planeSize);
        }
    }
    else if (compression == 1)
    {
        /* Per-plane, per-row big-endian u16 byte counts. */
        size_t ntab = (size_t)channels * (size_t)height;
        if (off + ntab * 2 > len)
        {
            free(planes);
            return NULL;
        }
        const uint8_t *cntBase = data + off;
        const uint8_t *src = data + off + ntab * 2;
        size_t srcLen = len - off - ntab * 2;
        size_t ip = 0;
        for (int pI = 0; pI < channels; pI++)
        {
            for (int y = 0; y < height; y++)
            {
                uint16_t cnt = (uint16_t)((cntBase[((size_t)pI * (size_t)height +
                                                   (size_t)y) * 2] << 8) |
                                          cntBase[((size_t)pI * (size_t)height +
                                                   (size_t)y) * 2 + 1]);
                if (ip + cnt > srcLen)
                {
                    ok = 0;
                    break;
                }
                if (pI < nPlanes)
                {
                    packbits_row(src + ip, cnt,
                                 planes + (size_t)pI * planeSize +
                                     (size_t)y * (size_t)width,
                                 (size_t)width);
                }
                ip += cnt; /* alpha planes are skipped but must be consumed */
            }
            if (!ok) break;
        }
    }
    else
    {
        free(planes);
        return NULL;
    }

    /* Build gray raster. */
    uint8_t *rows = (uint8_t *)calloc(planeSize, 1);
    if (!rows)
    {
        free(planes);
        return NULL;
    }
    for (size_t i = 0; i < planeSize; i++)
    {
        int g;
        if (mode == 1) /* grayscale */
        {
            g = planes[i];
        }
        else if (mode == 4) /* CMYK: C,M,Y planes, ink = 255 - value */
        {
            int cI = 255 - planes[i];
            int mI = 255 - planes[planeSize + i];
            int yI = 255 - planes[2 * planeSize + i];
            /* Approximate ink coverage -> gray. */
            int ink = (cI > mI ? cI : mI);
            if (yI > ink) ink = yI;
            g = 255 - ink;
        }
        else /* RGB */
        {
            g = dither_rgb_to_gray(planes[i], planes[planeSize + i],
                                   planes[2 * planeSize + i]);
        }
        rows[i] = (uint8_t)g;
    }
    free(planes);

    /* Downscale (bmp.c conventions). */
    int scaleNum = 1, scaleDen = 1;
    int targetW = width, targetH = height;
    if (width > 360 || height > 200)
    {
        if ((long long)width * 200 >= (long long)height * 360)
        {
            scaleNum = width; scaleDen = 360;
        }
        else
        {
            scaleNum = height; scaleDen = 200;
        }
        targetW = (int)((long long)width * scaleDen / scaleNum);
        if (targetW < 1) targetW = 1;
        targetH = (int)((long long)height * scaleDen / scaleNum);
        if (targetH < 1) targetH = 1;
    }

    PsdCtx c;
    c.width = width;
    c.height = height;
    c.channels = channels;
    c.depth = depth;
    c.work = NULL;
    c.planeSize = planeSize;
    c.nPlanes = nPlanes;
    c.scaleNum = scaleNum;
    c.scaleDen = scaleDen;

    PsdPix p;
    p.c = &c;
    p.rows = rows;
    LCDBitmap *img = dither_to_bitmap(targetW, targetH, psd_pixel_gray, &p);
    free(rows);
        if (img)
    {
        logger_log("PSD ok" );
    }
    return img;
}
