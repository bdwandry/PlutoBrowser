#ifndef PLUTO_RENDER_DECODERS_WEBP_H
#define PLUTO_RENDER_DECODERS_WEBP_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;
struct LCDBitmap;

/* Platform-independent core (P21: lossless VP8L only). Parses the RIFF
 * container and decodes a VP8L payload into a full-resolution grid of
 * ARGB pixels (0xAARRGGBB, straight alpha). Returns 0 on success,
 * -1 on rejection/failure. Caller frees with webp_free_rows_argb(). */
int webp_decode_argb(const uint8_t* data, size_t len,
                     int maxW, int maxH,
                     uint32_t*** outRows, int* outW, int* outH);

/* Convenience wrapper: luma of the decoded ARGB grid, box-filtered
 * down to the target dims like jpeg/gif/png. Returns 0 on success.
 * Caller frees with webp_free_rows(). */
int webp_decode_gray(const uint8_t* data, size_t len,
                     int maxW, int maxH,
                     uint8_t*** outRows, int* outW, int* outH);

void webp_free_rows_argb(uint32_t** rows, int h);
void webp_free_rows(uint8_t** rows, int h);

/* Device/simulator wrapper returning a 1-bit LCDBitmap (NULL on host
 * builds or decode failure). */
struct LCDBitmap* webp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH);

#endif
