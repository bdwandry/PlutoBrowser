#ifndef PLUTO_RENDER_DECODERS_BMP_H
#define PLUTO_RENDER_DECODERS_BMP_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;
struct LCDBitmap;

/* Platform-independent core: decodes into a targetW x targetH grid of
 * grayscale bytes (row-major). Returns 0 on success, -1 on rejection.
 * Caller frees with bmp_free_rows(). */
int bmp_decode_gray(const uint8_t* data, size_t len,
                    uint8_t*** outRows, int* outW, int* outH);

void bmp_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper returning a 1-bit LCDBitmap (NULL on host
 * builds or decode failure). */
struct LCDBitmap* bmp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len);

#endif
