/*
 * PlutoBrowser — psd.h
 * PSD (Photoshop) decoder: composite-image section only (layers ignored),
 * 8-bit RGB/Gray, raw (compression 0) and PackBits (1) channel planes.
 */
#ifndef PLUTO_PSD_H
#define PLUTO_PSD_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode a PSD file to a dithered 1-bit LCDBitmap (caller frees) or NULL. */
LCDBitmap *psd_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_PSD_H */
