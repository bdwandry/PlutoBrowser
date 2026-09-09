/*
 * PlutoBrowser — sgi.h
 * SGI (Silicon Graphics / .sgi / .rgb) decoder: verbatim and RLE storage,
 * 8 bits/channel, 1–4 channels (gray/RGB/RGBA), origin variants.
 */
#ifndef PLUTO_SGI_H
#define PLUTO_SGI_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode an SGI file to a dithered 1-bit LCDBitmap (caller frees) or NULL. */
LCDBitmap *sgi_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_SGI_H */
