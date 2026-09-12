/*
 * PlutoBrowser — png.h
 * Port of Source/render/decoders/png.lua (reference, 270 lines).
 *
 * Lua → C function map:
 *   readUInt32BE (local)                 → rd32be (static)
 *   paethPredictor                       → paeth_predictor (static)
 *   composite                            → png_composite (static, exact int form)
 *   PNGDecoder.decode(data, maxW, maxH)  → png_decode()
 *   per-row closures (unpack/composite)  → static row loops
 *
 * Preserved semantics:
 *   - Signature check (0x89 "PNG"), chunk scan WITHOUT CRC validation
 *     (Lua parity), IHDR/PLTE/tRNS/IDAT/IEND handling, `pos > #data + 12`
     overrun break, zero-length-chunk termination.
 *   - Color types 0/2/3/4/6; bit depths 1/2/4/8/16 (16-bit: high byte only);
 *     sub-byte unpacking (perByte, MSB-first shift, grayScale = 255//mask).
 *   - All 5 unfilters incl. Paeth; prev/cur row swap; missing raw bytes → 0
 *     (Lua `string.byte(...) or 0`), missing pixel samples → 0/255 fallbacks.
 *   - tRNS: palette alphas (colorType 3), gray key (colorType 0, byte 1),
 *     RGB key (colorType 2, bytes 1/3/5). Alpha composites over white.
 *   - Adam7 interlace: FIRST PASS ONLY (ceil(w/8) x ceil(h/8) samples).
 *   - Streaming: inflate_stream_read feeds one row at a time into the box
 *     downscaler — bounded memory (reference architecture).
 *   - Tasks.yieldCheck() call sites are preserved as comments; the task
 *     layer already budgets frames per step (see inflate.h note).
 */
#ifndef PLUTO_PNG_H
#define PLUTO_PNG_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Decode a PNG to a dithered 1-bit LCDBitmap (caller frees via
 * pd->graphics->freeBitmap) or NULL. maxW/maxH <= 0 → 360/200. */
LCDBitmap *png_decode(const uint8_t *data, size_t len, int maxW, int maxH);

#endif /* PLUTO_PNG_H */
