/*
 * PlutoBrowser — ico.h
 * Port of Source/render/decoders/ico.lua (reference, 185 lines).
 *
 * Lua → C function map:
 *   readUInt16LE/readUInt32LE/readInt32LE (locals) → static helpers
 *   composite(gray, a)                             → ico_composite (static)
 *   decodeDIB(data, maxW, maxH) (local)            → ico_decode_dib (static)
 *   ICODecoder.decode(data, maxW, maxH)            → ico_decode()
 *
 * Preserved semantics:
 *   - ICONDIR (reserved=0, type 1|2, count>0), 16-byte entries, 0 means 256.
 *   - Best entry: largest area, then highest bit depth (Chromium order).
 *   - Entries tried in order until one decodes (pcall guard → C NULL check).
 *   - Classic DIB: stored height doubled (image + AND mask); bottom-up for
 *     positive height; AND-mask bit 1 = fully transparent (composited over
 *     white → 255); 32bpp alpha < 255 composites toward white.
 *   - PNG-compressed entries decode via png_decode (P23); NULL results
 *     continue the entry retry loop exactly like the Lua pcall(false) path.
 */
#ifndef PLUTO_ICO_H
#define PLUTO_ICO_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode an ICO file to a dithered 1-bit LCDBitmap (caller frees via
 * pd->graphics->freeBitmap) or NULL. maxW/maxH <= 0 → 360/200. */
LCDBitmap *ico_decode(const uint8_t *data, size_t len, int maxW, int maxH);

#endif /* PLUTO_ICO_H */
