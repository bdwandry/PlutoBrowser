#ifndef PLUTO_RENDER_DECODERS_JPEG_H
#define PLUTO_RENDER_DECODERS_JPEG_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;
struct LCDBitmap;

/* Platform-independent core: decodes baseline (SOF0, luma-only IDCT)
 * and progressive (SOF2, DC-scans-only) JPEGs into a targetW x targetH
 * grayscale grid. Returns 0 on success, -1 on rejection/failure.
 * Caller frees with jpeg_free_rows(). */
int jpeg_decode_gray(const uint8_t* data, size_t len,
                     int maxW, int maxH,
                     uint8_t*** outRows, int* outW, int* outH);

void jpeg_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper returning a 1-bit LCDBitmap (NULL on host
 * builds or decode failure). */
struct LCDBitmap* jpeg_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH);

#endif
