#ifndef PLUTO_RENDER_DECODERS_GIF_H
#define PLUTO_RENDER_DECODERS_GIF_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;
struct LCDBitmap;

/* Platform-independent core: decodes the FIRST frame of a GIF87a/89a
 * into a targetW x targetH grid of grayscale bytes (logical screen
 * composited over white). Returns 0 on success, -1 on rejection.
 * Caller frees with gif_free_rows(). */
int gif_decode_gray(const uint8_t* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH);

void gif_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper returning a 1-bit LCDBitmap (NULL on host
 * builds or decode failure). */
struct LCDBitmap* gif_decode(struct PlaydateAPI* pd, const uint8_t* data,
                             size_t len, int maxW, int maxH);

#endif
