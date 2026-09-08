/*
 * PlutoBrowser — dither.c
 * Port of Source/render/decoders/dither.lua (reference, 60 lines).
 * Fast 1-bit Bayer dithering; see dither.h for the Lua→C map.
 */
#include <stdlib.h>
#include <string.h>
#include "render/decoders/dither.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

/* 4x4 Bayer matrix scaled to 0-255 thresholds (verbatim from Lua). */
static const uint8_t bayer4x4[4][4] = {
    {   0, 128,  32, 160 },
    { 192,  64, 224,  96 },
    {  48, 176,  16, 144 },
    { 240, 112, 208,  80 }
};

int dither_rgb_to_gray(int r, int g, int b)
{
    return (r * 306 + g * 601 + b * 117) >> 10;
}

/* Shared core: computes isBlack per pixel and writes bits (1=white,
 * 0=black), MSB-first, row stride outStride. Mirrors the Lua loop
 * exactly: threshold = bayer4x4[y%4][x%4], isBlack = gray < threshold. */
static int dither_core(int width, int height,
                       uint8_t (*getPixelGray)(void *, int, int),
                       void *userdata, uint8_t *outBits, int outStride)
{
    if (!getPixelGray || !outBits || width <= 0 || height <= 0)
    {
        return 0; /* Lua returns nil for invalid w/h */
    }
    if (width > 380) width = 380; /* Lua clamps */
    if (height > 240) height = 240;

    for (int y = 0; y < height; y++)
    {
        const uint8_t *bayerRow = bayer4x4[y & 3];
        uint8_t *rowBits = outBits + (size_t)y * outStride;
        for (int x = 0; x < width; x++)
        {
            int gray = getPixelGray(userdata, x, y);
            int isBlack = gray < bayerRow[x & 3];
            if (!isBlack)
            {
                rowBits[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    return 1;
}

int dither_to_bits(int width, int height,
                   uint8_t (*getPixelGray)(void *, int, int),
                   void *userdata, uint8_t *outBits, int outStride)
{
    if (!outBits || outStride <= 0)
    {
        return 0;
    }
    if (width > 380) width = 380;
    if (height > 240) height = 240;
    /* Caller's buffer must be clear if they expect white background
     * semantics; dither_to_bitmap white-fills first (Lua image new w/
     * kColorWhite). Here we require a zeroed buffer OR pre-set bits —
     * documented: only bits for non-black pixels are SET. */
    return dither_core(width, height, getPixelGray, userdata, outBits, outStride);
}

LCDBitmap *dither_to_bitmap(int width, int height,
                            uint8_t (*getPixelGray)(void *, int, int),
                            void *userdata)
{
    PlaydateAPI *pd = pluto_pd();
    if (!pd)
    {
        return NULL;
    }
    if (width <= 0 || height <= 0)
    {
        return NULL;
    }
    if (width > 380) width = 380;
    if (height > 240) height = 240;

    /* Lua: gfx.image.new(width, height, kColorWhite) — white background. */
    LCDBitmap *img = pd->graphics->newBitmap(width, height, kColorWhite);
    if (!img)
    {
        return NULL;
    }

    int stride = (width + 7) / 8;
    uint8_t *bits = (uint8_t *)calloc((size_t)stride * height, 1);
    if (!bits)
    {
        pd->graphics->freeBitmap(img);
        return NULL;
    }

    dither_core(width, height, getPixelGray, userdata, bits, stride);

    /* Write the 1-bit data straight into the bitmap (documented deviation:
     * same output as the Lua run-length fillRect loop, but O(pixels) — the
     * Lua comment states its fillRect batching is itself a perf strategy). */
    int bw = 0, bh = 0, rb = 0;
    uint8_t *mask = NULL, *data = NULL;
    pd->graphics->getBitmapData(img, &bw, &bh, &rb, &mask, &data);
    if (data)
    {
        for (int y = 0; y < height; y++)
        {
            memcpy(data + (size_t)y * rb, bits + (size_t)y * stride, (size_t)stride);
        }
    }
    free(bits);
    return img;
}
