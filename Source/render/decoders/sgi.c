/*
 * PlutoBrowser — sgi.c
 * SGI decoder (see sgi.h). Header is 512 bytes; RLE files carry two 512-entry
 * big-endian tables (start/length per scanline per channel). Planes are
 * stored per channel (all rows of R, then G, then B). First-byte-of-row BPC
 * check (storage flag) plus table sanity used to validate.
 */
#include <string.h>
#include <stdlib.h>
#include "sgi.h"
#include "dither.h"
#include "scale.h"
#include "../../core/logger.h"


typedef struct
{
    int width, height, channels;
    uint8_t *rows; /* gray raster w*h */
    int scaleNum, scaleDen;
} SgiPix;

static uint8_t sgi_pixel_gray(void *ud, int outX, int outY)
{
    SgiPix *p = (SgiPix *)ud;
    int srcX = outX * p->scaleNum / p->scaleDen;
    int srcY = outY * p->scaleNum / p->scaleDen;
    if (srcX > p->width - 1) srcX = p->width - 1;
    if (srcY > p->height - 1) srcY = p->height - 1;
    return p->rows[(size_t)srcY * (size_t)p->width + (size_t)srcX];
}

static LCDBitmap *sgi_finish(uint8_t *rows, int w, int h);

static size_t sgi_expand_row(const uint8_t *src, size_t srcLen, uint8_t *out,
                             int w)
{
    /* SGI RLE (per ffmpeg's sgidec.c / the IRIX spec): BYTE-oriented.
     * Read a byte; count = byte & 0x7f. count == 0 ends the row.
     * byte & 0x80 set: `count` literal bytes follow. Clear: the next byte
     * is repeated `count` times. */
    size_t ip = 0, op = 0;
    while (ip < srcLen && op < (size_t)w)
    {
        uint8_t b = src[ip++];
        int count = b & 0x7f;
        if (count == 0) break; /* end of row */
        if (b & 0x80)
        {
            size_t n = (size_t)count;
            if (ip + n > srcLen) n = srcLen - ip;
            if (op + n > (size_t)w) n = (size_t)w - op;
            memcpy(out + op, src + ip, n);
            ip += n;
            op += n;
        }
        else
        {
            if (ip >= srcLen) break;
            uint8_t v = src[ip++];
            size_t n = (size_t)count;
            if (op + n > (size_t)w) n = (size_t)w - op;
            memset(out + op, v, n);
            op += n;
        }
    }
    return op;
}

LCDBitmap *sgi_decode(const uint8_t *data, size_t len)
{
    if (!data || len < 512) return NULL;
    uint32_t magic = ((uint32_t)data[0] << 8) | data[1];
    if (magic != 0x01DA) return NULL;
    int storage = data[2]; /* 0 verbatim, 1 RLE */
    int bpc = data[3];     /* bytes per channel-pixel: 1 (or 2 unsupported) */
    if (bpc != 1) return NULL;
    /* SGI header: magic(0-2) storage(2) bpc(3) dims(4-6) x(6-8) y(8-10) z(10-12) */
    int dim = ((int)data[4] << 8) | data[5];
    int w = ((int)data[6] << 8) | data[7];
    int h = ((int)data[8] << 8) | data[9];
    int z = ((int)data[10] << 8) | data[11];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    int channels = dim == 1 ? 1 : (dim == 2 ? z : z);
    if (channels < 1 || channels > 4) return NULL;

    int minI = data[0x3C] ? 1 : 0; /* little endian flag at byte 60 — rare;
                                      header fields are big-endian normally */

    size_t planeSize = (size_t)w * (size_t)h;
    const uint8_t *pixBase;
    if (storage == 1)
    {
        size_t nLines = (size_t)channels * (size_t)h;
        size_t lenTabOff = 512 + nLines * 4;
        if (len < lenTabOff + nLines * 4) return NULL;
        const uint8_t *startTab = data + 512;
        const uint8_t *lenTab = data + lenTabOff;
        /* sanity: first table entry within file */
        uint32_t s0 = ((uint32_t)startTab[0] << 24) | ((uint32_t)startTab[1] << 16) |
                      ((uint32_t)startTab[2] << 8) | startTab[3];
        if (s0 == 0 || s0 >= len) return NULL;
        uint8_t *planes = (uint8_t *)calloc(planeSize * (size_t)channels, 1);
        if (!planes) return NULL;
        int ok = 1;
        for (int cI = 0; cI < channels && ok; cI++)
        {
            for (int y = 0; y < h && ok; y++)
            {
                size_t idx = (size_t)(cI * h + y);
                uint32_t off = ((uint32_t)startTab[idx * 4] << 24) |
                               ((uint32_t)startTab[idx * 4 + 1] << 16) |
                               ((uint32_t)startTab[idx * 4 + 2] << 8) |
                               startTab[idx * 4 + 3];
                uint32_t cnt = ((uint32_t)lenTab[idx * 4] << 24) |
                               ((uint32_t)lenTab[idx * 4 + 1] << 16) |
                               ((uint32_t)lenTab[idx * 4 + 2] << 8) |
                               lenTab[idx * 4 + 3];
                if (off == 0 || off + cnt > len)
                {
                    ok = 0;
                    break;
                }
                /* SGI rows are stored bottom-up: file row y = image row
                 * (h-1-y). Flip while expanding so plane rows are top-down. */
                sgi_expand_row(data + off, cnt,
                               planes + (size_t)cI * planeSize +
                                   (size_t)(h - 1 - y) * (size_t)w,
                               w);
            }
        }
        if (!ok)
        {
            free(planes);
            return NULL;
        }

        uint8_t *rows = (uint8_t *)calloc(planeSize, 1);
        if (!rows)
        {
            free(planes);
            return NULL;
        }
        for (size_t i = 0; i < planeSize; i++)
        {
            int g;
            if (channels == 1)
                g = planes[i];
            else if (channels == 2)
                g = planes[i]; /* gray + alpha: drop alpha */
            else
                g = dither_rgb_to_gray(planes[i],
                                       planes[planeSize + i],
                                       planes[2 * planeSize + i]);
            rows[i] = (uint8_t)g;
        }
        free(planes);

        return sgi_finish(rows, w, h);
    }

    /* Verbatim: pixels are interleaved (z per pixel). */
    pixBase = data + 512;
    if (512 + planeSize * (size_t)channels > len) return NULL;
    uint8_t *rows = (uint8_t *)calloc(planeSize, 1);
    if (!rows) return NULL;
    for (size_t i = 0; i < planeSize; i++)
    {
        int g;
        if (channels == 1)
            g = pixBase[i];
        else if (channels == 2)
            g = pixBase[i * 2];
        else
            g = dither_rgb_to_gray(pixBase[i * channels],
                                   pixBase[i * channels + 1],
                                   pixBase[i * channels + 2]);
        rows[i] = (uint8_t)g;
    }
    (void)minI;
    return sgi_finish(rows, w, h);
}

/* Shared finish: downscale + dither + log. */
static LCDBitmap *sgi_finish(uint8_t *rows, int w, int h)
{
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
    SgiPix p;
    p.width = w;
    p.height = h;
    p.rows = rows;
    p.scaleNum = scaleNum;
    p.scaleDen = scaleDen;
    LCDBitmap *img = dither_to_bitmap(targetW, targetH, sgi_pixel_gray, &p);
    free(rows);
        if (img)
    {
        logger_log("SGI ok" );
    }
    return img;
}
