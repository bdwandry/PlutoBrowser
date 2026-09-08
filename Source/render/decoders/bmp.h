/*
 * PlutoBrowser — bmp.h
 * Port of Source/render/decoders/bmp.lua (reference, 111 lines).
 *
 * Lua → C function map:
 *   readUInt16LE/readUInt32LE/readInt32LE (locals) → static helpers
 *   BMPDecoder.decode(data)                        → bmp_decode()
 *   (inner getPixelGray closure)                   → bmp_pixel_gray (static)
 *
 * Preserved semantics:
 *   - Signature "BM", min length 54; width>0, rawHeight≠0 (negative =
 *     top-down); bpp 1/4/8 (palette at 15+headerSize, 1<<bpp entries,
 *     BGR order) / 24 / 32 (BGR/X).
 *   - Downscale: scale = max(w/360, h/200) when over; target floor(w/scale)
 *     min 1; nearest-source sampling floor(outX*scale) clamped to width-1.
 *   - Row stride floor((bpp*w+31)/32)*4; bottom-up for positive height.
 *   - 1bpp palette fallback `palette[idx] or (idx==1 and 255 or 0)`.
 *   - Output via Dither.toImage (Bayer dithered 1-bit bitmap) or NULL.
 */
#ifndef PLUTO_BMP_H
#define PLUTO_BMP_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode a BMP file to a dithered 1-bit LCDBitmap (caller frees via
 * pd->graphics->freeBitmap) or NULL on invalid/truncated input. */
LCDBitmap *bmp_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_BMP_H */
