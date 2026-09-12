/*
 * PlutoBrowser — tif.h
 * Minimal TIFF decoder (extension format, no CometBrowser Lua counterpart).
 *
 * Supported (verified against the wiesmann.codiferes.net benchmark corpus):
 *   - II* / MM* byte order, first IFD, single strip (or first strip)
 *   - compression: none (1), LZW (5), PackBits (32773), CCITT G3 1D (2)
 *   - photometric: RGB (2), palette (3), WhiteIsZero/BlackIsZero (0/1)
 *   - bits per sample 1 or 8 (palette also 4)
 *   - nearest-neighbor downscale to 360x200 (bmp.c conventions)
 *   - output via Dither Bayer 1-bit LCDBitmap (caller frees)
 */
#ifndef PLUTO_TIF_H
#define PLUTO_TIF_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode a TIFF file to a dithered 1-bit LCDBitmap (caller frees via
 * pd->graphics->freeBitmap) or NULL on unsupported/truncated input. */
LCDBitmap *tif_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_TIF_H */
