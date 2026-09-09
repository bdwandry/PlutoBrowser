/*
 * PlutoBrowser — xbm.h
 * XBM (X BitMap) decoder: ASCII C-array format, 1bpp, bits LSB-first per
 * byte, rows padded to byte boundaries. Size taken from the #define lines.
 */
#ifndef PLUTO_XBM_H
#define PLUTO_XBM_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode an XBM file to a dithered 1-bit LCDBitmap (caller frees) or NULL. */
LCDBitmap *xbm_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_XBM_H */
