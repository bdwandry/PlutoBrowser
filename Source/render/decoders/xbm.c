/*
 * PlutoBrowser — xbm.c
 * XBM decoder (see xbm.h). Parses "#define <name>_width N" / "_height N",
 * then hex bytes until the closing brace. Bit (x,y) = byte[y*stride + x/8]
 * bit (x%8) with LSB-first ordering (X11 hot bit layout). A set bit is black
 * (XBM is a cursor mask: 1 = ink). Output via Bayer dither.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "xbm.h"
#include "dither.h"
#include "scale.h"
#include "../../core/logger.h"


typedef struct
{
    const uint8_t *bits;
    int stride;
    int width, height;
    int scaleNum, scaleDen;
} XbmPix;

static uint8_t xbm_pixel_gray(void *ud, int outX, int outY)
{
    XbmPix *p = (XbmPix *)ud;
    int srcX = outX * p->scaleNum / p->scaleDen;
    int srcY = outY * p->scaleNum / p->scaleDen;
    if (srcX > p->width - 1) srcX = p->width - 1;
    if (srcY > p->height - 1) srcY = p->height - 1;
    int bit = (p->bits[(size_t)srcY * (size_t)p->stride + (size_t)(srcX >> 3)] >>
               (srcX & 7)) & 1;
    return bit ? 0 : 255; /* 1 = ink = black */
}

static int xbm_find_define(const char *s, const char *suffix, size_t len,
                           int *value)
{
    /* Matches: #define <anything><suffix>  <number> */
    size_t sl = strlen(suffix);
    const char *p = s;
    size_t pos = 0;
    while (pos < len)
    {
        if (strncmp(p, "#define", 7) == 0)
        {
            const char *lineEnd = strchr(p, '\n');
            if (!lineEnd) lineEnd = p + len - pos;
            /* token after #define */
            const char *q = p + 7;
            while (q < lineEnd && isspace((unsigned char)*q)) q++;
            const char *tokStart = q;
            while (q < lineEnd && !isspace((unsigned char)*q)) q++;
            size_t tokLen = (size_t)(q - tokStart);
            if (tokLen >= sl && strncmp(tokStart + tokLen - sl, suffix, sl) == 0)
            {
                /* parse the number later in the line */
                const char *num = q;
                while (num < lineEnd && !isdigit((unsigned char)*num)) num++;
                if (num < lineEnd)
                {
                    *value = atoi(num);
                    return 1;
                }
            }
            pos += (size_t)(lineEnd - p) + 1;
            p = lineEnd + 1;
            if (pos >= len) break;
        }
        else
        {
            p++;
            pos++;
        }
    }
    return 0;
}

LCDBitmap *xbm_decode(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 32) return NULL;
    const char *s = (const char *)data;

    int w = 0, h = 0;
    if (!xbm_find_define(s, "_width", len, &w)) return NULL;
    if (!xbm_find_define(s, "_height", len, &h)) return NULL;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;

    /* Find the opening brace of the bits array. */
    const char *brace = memchr(s, '{', len);
    if (!brace) return NULL;
    const char *p = brace + 1;
    const char *end = memchr(brace, '}', len - (size_t)(brace - s));
    if (!end) end = s + len;

    int stride = (w + 7) / 8;
    size_t need = (size_t)stride * (size_t)h;
    uint8_t *bits = (uint8_t *)calloc(need, 1);
    if (!bits) return NULL;

    size_t bi = 0;
    while (p < end && bi < need)
    {
        if (*p == '0' && (p + 1 < end) && (p[1] == 'x' || p[1] == 'X'))
        {
            p += 2;
            int v = 0, nd = 0;
            while (p < end && nd < 2 && isxdigit((unsigned char)*p))
            {
                int c = *p;
                int d = isdigit((unsigned char)c) ? c - '0'
                                                  : (tolower(c) - 'a' + 10);
                v = v * 16 + d;
                p++;
                nd++;
            }
            bits[bi++] = (uint8_t)v;
        }
        else
        {
            p++;
        }
    }

    if (bi == 0)
    {
        free(bits);
        return NULL;
    }

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

    XbmPix pix;
    pix.bits = bits;
    pix.stride = stride;
    pix.width = w;
    pix.height = h;
    pix.scaleNum = scaleNum;
    pix.scaleDen = scaleDen;

    LCDBitmap *img = dither_to_bitmap(targetW, targetH, xbm_pixel_gray, &pix);
    free(bits);
        if (img)
    {
        logger_log("XBM ok" );
    }
    return img;
}
