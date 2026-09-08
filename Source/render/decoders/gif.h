/*
 * PlutoBrowser — gif.h
 * Port of Source/render/decoders/gif.lua (reference, 293 lines).
 *
 * Lua → C function map:
 *   readUInt16LE (local)            → static helper
 *   GIFDecoder.decode(data, maxW, maxH) → gif_decode()
 *   inner closures (readCode, flushRow, endStream, initCodeTable) → static fns
 *
 * Preserved semantics:
 *   - GIF87a/89a signature, logical screen canvas (white background),
 *     global/local palettes (BGR→gray), GCE transparency index.
 *   - First frame only; blocks before 0x2C are skipped, trailer (0x3B)
 *     before any image → NULL.
 *   - GIF-LZW: variable code size minCodeSize+1..12, clear/end codes,
 *     prefix/suffix table with the reference's exact `or code`/`or 0`
 *     fallbacks, code>=nextCode KwKwK case, deferred table-clearing absent
 *     (no early clear — Lua resets only on clearCode).
 *   - LZW output streams straight into the box-filter downscaler (never a
 *     full-res buffer); interlaced frames are buffered per linear row and
 *     replayed in order; short runs/pixels pad white (255).
 *   - Tasks.yieldCheck() call sites: see dither/inflate header notes — the
 *     cooperative budget lives at the task layer in the C port.
 */
#ifndef PLUTO_GIF_H
#define PLUTO_GIF_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode the first frame of a GIF to a dithered 1-bit LCDBitmap (caller
 * frees via pd->graphics->freeBitmap) or NULL. maxW/maxH <= 0 → 360/200. */
LCDBitmap *gif_decode(const uint8_t *data, size_t len, int maxW, int maxH);

#endif /* PLUTO_GIF_H */
