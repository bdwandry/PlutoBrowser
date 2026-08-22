#ifndef PLUTO_RENDER_DECODERS_PNG_H
#define PLUTO_RENDER_DECODERS_PNG_H

#include <stddef.h>
#include <stdint.h>

/* C port of Source/render/decoders/png.lua (PNGDecoder).
 *
 * Streams zlib scanline data through Inflate.createStream, unfilters one
 * row at a time and box-filters down to ~screen size, so huge PNGs decode
 * in bounded memory. Adam7 interlaced images decode from their FIRST pass
 * only (every 8th pixel), per the source.
 *
 * Faithful quirks preserved:
 *  - chunk walk accepts a final chunk ending anywhere <= len+12 bytes
 *  - bitDepth<8 grayScale is INTEGER 255/mask division
 *  - tRNS keys are compared against RAW stored bytes (high byte of 16-bit
 *    samples), not scaled values
 *  - unknown filter types (>4) are treated as filter 0
 *  - out-of-range samples read as 0 (colors) / 255 (alpha)
 */

/* Decodes into downscaled grayscale rows (the platform-independent core).
 * On success (*outRows)[y][x] holds gray 0..255 for y<*outH, x<*outW;
 * rows are pluto_malloc'd (caller frees rows, each row, via
 * png_free_rows). Returns 0 on success, -1 on reject/corruption. */
int png_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH);

void png_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper: decodes + dithers into a 1-bit LCDBitmap.
 * Host builds return NULL. */
struct PlaydateAPI;
struct LCDBitmap;
struct LCDBitmap* png_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH);

#endif
