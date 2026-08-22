#ifndef PLUTO_RENDER_DECODERS_DITHER_H
#define PLUTO_RENDER_DECODERS_DITHER_H

#include <stdint.h>

struct PlaydateAPI;
typedef int (*DitherGrayFn)(void* userdata, int x, int y);

/* (r*306 + g*601 + b*117) >> 10 -- standard luminance scaled by 1024 */
int dither_rgb_to_gray(int r, int g, int b);

/* Bayer threshold lookup: is this gray value black at (x,y)? */
int dither_is_black(int gray, int x, int y);

/* Paint a grayscale grid (via getPixelGray(userdata,x,y), 0..255) into a new
 * 1-bit image using horizontal run-length batching. Host builds (no Playdate
 * API) return NULL. Mirrors Dither.toImage: rejects/clamps sizes, 380x240 cap,
 * white background, strict `gray < threshold` comparison. */
struct LCDBitmap;
struct LCDBitmap* dither_to_image(struct PlaydateAPI* pd,
                                  DitherGrayFn get_pixel_gray,
                                  void* userdata, int width, int height);

#endif
