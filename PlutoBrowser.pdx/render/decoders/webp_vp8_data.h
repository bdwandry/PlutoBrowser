#ifndef PLUTO_RENDER_DECODERS_WEBP_VP8_DATA_H
#define PLUTO_RENDER_DECODERS_WEBP_VP8_DATA_H

#include <stdint.h>

/* Verbatim transcription of Source/render/decoders/webp_vp8_data.lua
 * (libwebp v1.6.0 VP8 decoder reference tables). Do not edit. */

#ifdef __cplusplus
extern "C" {
#endif

extern const uint16_t kDcTable[128];
extern const uint16_t kAcTable[128];
extern const uint8_t kZigzag[16];
/* Rows are 0-terminated (cat3..cat6). */
extern const uint8_t kCat3456[4][12];
extern const uint8_t kBands[17];
extern const uint16_t kScan[16];
extern const int8_t kYModesIntra4[18];
extern const uint8_t kBModesProba[900];       /* [10][10][9] flattened */
extern const uint8_t CoeffsProba0[4 * 8 * 3 * 11];
extern const uint8_t CoeffsUpdateProba[4 * 8 * 3 * 11];

#ifdef __cplusplus
}
#endif

#endif
