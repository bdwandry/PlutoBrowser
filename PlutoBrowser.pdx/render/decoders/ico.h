#ifndef PLUTO_RENDER_DECODERS_ICO_H
#define PLUTO_RENDER_DECODERS_ICO_H

#include <stddef.h>
#include <stdint.h>

/* C port of Source/render/decoders/ico.lua (ICODecoder).
 *
 * Parses the ICO container, sorts entries by area desc / bpp desc, then
 * delegates PNG-signature entries to the PNG decoder or classic DIB
 * entries to an embedded-BMP reader. DIB height is doubled (image rows +
 * trailing 1bpp AND mask); AND bits set mean fully transparent, which
 * composites over white. First entry that decodes wins.
 */

/* Platform-independent core: decodes into a grid of grayscale bytes
 * (row-major). maxW/maxH <= 0 selects the source defaults (360/200).
 * Returns 0 on success, -1 on rejection. Caller frees with
 * ico_free_rows(). */
int ico_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH);

void ico_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper returning a 1-bit LCDBitmap (NULL on host
 * builds or decode failure). */
struct PlaydateAPI;
struct LCDBitmap;
struct LCDBitmap* ico_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH);

#endif
