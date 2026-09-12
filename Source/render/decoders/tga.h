/*
 * PlutoBrowser — tga.h
 * TGA (Targa) decoder: types 1 (cmap) / 2 (truecolor), 8/16/24/32 bpp,
 * RLE (10/9) and raw, bottom-up and top-down, image-ID skip.
 */
#ifndef PLUTO_TGA_H
#define PLUTO_TGA_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode a TGA file to a dithered 1-bit LCDBitmap (caller frees) or NULL. */
LCDBitmap *tga_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_TGA_H */
