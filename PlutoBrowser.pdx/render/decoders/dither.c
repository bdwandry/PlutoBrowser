// dither.c — C port of Source/render/decoders/dither.lua (Dither).
//
// Fast 1-bit dithering engine: 4x4 Bayer ordered dither with run-length
// batched fillRect painting into a white-initialized bitmap.

#include "render/decoders/dither.h"

#include "core/logger.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_DITHER_PD 1
#endif

/* 4x4 Bayer matrix scaled to 0-255 thresholds */
static const int kBayer4x4[4][4] = {
    {   0, 128,  32, 160 },
    { 192,  64, 224,  96 },
    {  48, 176,  16, 144 },
    { 240, 112, 208,  80 }
};

int dither_rgb_to_gray(int r, int g, int b) {
    return (r * 306 + g * 601 + b * 117) >> 10;
}

int dither_is_black(int gray, int x, int y) {
    /* Lua quirk preserved: strict less-than, so gray==threshold paints
     * WHITE (e.g. pure gray 0 stays white at Bayer position 0). */
    return gray < kBayer4x4[y % 4][x % 4];
}

#ifdef PLUTO_DITHER_PD
#include "pd_api.h"

struct LCDBitmap* dither_to_image(struct PlaydateAPI* pd,
                                  DitherGrayFn get_pixel_gray,
                                  void* userdata, int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    if (width > 380) width = 380;
    if (height > 240) height = 240;

    LCDBitmap* img = pd->graphics->newBitmap(width, height, kColorWhite);
    if (img == NULL) return NULL;

    pd->graphics->pushContext(img);

    for (int y = 0; y < height; y++) {
        const int* bayerRow = kBayer4x4[y & 3];
        int runStart = -1;

        for (int x = 0; x < width; x++) {
            int gray = get_pixel_gray(userdata, x, y);
            if (gray < bayerRow[x & 3]) {
                if (runStart < 0) runStart = x;
            } else {
                if (runStart >= 0) {
                    pd->graphics->fillRect(runStart, y, x - runStart, 1,
                                           kColorBlack);
                    runStart = -1;
                }
            }
        }

        if (runStart >= 0) {
            pd->graphics->fillRect(runStart, y, width - runStart, 1,
                                   kColorBlack);
        }
    }

    pd->graphics->popContext();
    return img;
}

#else  /* host builds: pixel logic above is fully testable without gfx */

struct LCDBitmap* dither_to_image(struct PlaydateAPI* pd,
                                  DitherGrayFn get_pixel_gray,
                                  void* userdata, int width, int height) {
    (void)get_pixel_gray;
    (void)userdata;
    (void)width;
    (void)height;
    (void)pd;
    return NULL;
}

#endif
