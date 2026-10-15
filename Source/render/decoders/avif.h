/*
 * PlutoBrowser — avif.h
 * AVIF still-image decoder: minimal ISO-BMFF container parse (ftyp/meta/
 * iloc, primary av01 item, file-offset extents) + vendored dav1d 1.5.1
 * (Source/render/decoders/dav1d — upstream files never modified; C-only,
 * 8-bit-only, single-threaded; see dav1d/config.h).
 *
 * Supported inputs: single-item AVIF stills (brands `avif`/`avis`),
 * 8-bit, ≤1600px per side. Refused cleanly (NULL → placeholder card):
 * grid images, alpha aux items (ignored — color item decoded), 10/12-bit,
 * oversize frames, idat construction, truncated boxes. Never crashes on
 * hostile input: every failure path frees and returns NULL.
 *
 * Output follows the bmp.c/tif.c convention: nearest-sampled to fit
 * 360x200, then Bayer-dithered to 1-bit.
 */
#ifndef PLUTO_AVIF_H
#define PLUTO_AVIF_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode an AVIF file to a dithered 1-bit LCDBitmap (caller frees via
 * pd->graphics->freeBitmap) or NULL on invalid/truncated/unsupported
 * input. */
LCDBitmap *avif_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_AVIF_H */
