/*
 * PlutoBrowser — dither.h
 * Port of Source/render/decoders/dither.lua (reference, 60 lines).
 *
 * Lua → C function map:
 *   Dither.rgbToGray(r,g,b)          → dither_rgb_to_gray()
 *   Dither.toImage(getPixelGray,w,h) → dither_to_bitmap()
 *
 * Preserved semantics:
 *   - 4x4 Bayer matrix thresholds {0,128,32,160 / 192,64,224,96 /
 *     48,176,16,144 / 240,112,208,80}; isBlack = gray < threshold.
 *   - Luminance (r*306 + g*601 + b*117) >> 10.
 *   - Input is clamped to 380x240 (Lua width/height clamps).
 *   - toImage(NULL fn) → NULL (Lua would call nil → error; the port
 *     documents the guard because C has no exception to reproduce).
 */
#ifndef PLUTO_DITHER_H
#define PLUTO_DITHER_H

#include <stdint.h>
#include "pd_api.h"

/* (r*306 + g*601 + b*117) >> 10 — standard luminance scaled by 1024. */
int dither_rgb_to_gray(int r, int g, int b);

/* Paint a 2D grayscale grid into a new 1-bit Playdate image using 4x4
 * Bayer dithering. getPixelGray(x, y) returns 0..255. Clamps to 380x240.
 * Returns an LCDBitmap (caller owns via pd->graphics->freeBitmap) or NULL. */
LCDBitmap *dither_to_bitmap(int width, int height,
                            uint8_t (*getPixelGray)(void *userdata, int x, int y),
                            void *userdata);

/* Dither directly into a caller-provided 1-bit buffer (1 = white, 0 =
 * black, MSB-first, stride = (width+7)/8). Same matrix/clamps as
 * dither_to_bitmap; used by decoders that rasterize into their own
 * buffers and by tests. Returns 0 on invalid args. */
int dither_to_bits(int width, int height,
                   uint8_t (*getPixelGray)(void *userdata, int x, int y),
                   void *userdata, uint8_t *outBits, int outStride);

#endif /* PLUTO_DITHER_H */
