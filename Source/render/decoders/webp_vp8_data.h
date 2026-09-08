/*
 * PlutoBrowser — webp_vp8_data.h
 * Transcribed from Source/render/decoders/webp_vp8_data.lua
 * (generated from libwebp v1.6.0 decoder tables — DO NOT hand-edit;
 *  regenerate with tests/gen_p27_vp8_tables.py).
 *
 * Lua -> C map (element order preserved; Lua 1-based -> C 0-based):
 *   VP8Tables.kDcTable             -> vp8_dc_table[128] (uint8_t)
 *   VP8Tables.kAcTable             -> vp8_ac_table[128] (uint16_t)
 *   VP8Tables.kZigzag              -> vp8_zigzag[16] (uint8_t)
 *   VP8Tables.kCat3456             -> vp8_cat3456[48] (uint8_t)
 *   VP8Tables.kBands               -> vp8_bands[17] (uint8_t)
 *   VP8Tables.kScan                -> vp8_scan[16] (uint16_t)
 *   VP8Tables.kYModesIntra4        -> vp8_ymodes_intra4[18] (int8_t)
 *   VP8Tables.kBModesProba         -> vp8_bmodes_proba[900] (uint8_t)
 *   VP8Tables.CoeffsProba0         -> vp8_coeffs_proba0[1056] (uint8_t)
 *   VP8Tables.CoeffsUpdateProba    -> vp8_coeffs_update_proba[1056] (uint8_t)
 * kCat3456 rows are zero-padded to 12 entries as a [4][12] 2-D array; the
 * decoder reads a row until the 0 terminator exactly like the Lua reference.
 */
#ifndef PLUTO_WEBP_VP8_DATA_H
#define PLUTO_WEBP_VP8_DATA_H

#include <stdint.h>

#define VP8_CAT3456_ROW 12

extern const uint8_t vp8_dc_table[128];
extern const uint16_t vp8_ac_table[128];
extern const uint8_t vp8_zigzag[16];
extern const uint8_t vp8_cat3456[4][VP8_CAT3456_ROW];
extern const uint8_t vp8_bands[17];
extern const uint16_t vp8_scan[16];
extern const int8_t vp8_ymodes_intra4[18];
extern const uint8_t vp8_bmodes_proba[900];
extern const uint8_t vp8_coeffs_proba0[1056];
extern const uint8_t vp8_coeffs_update_proba[1056];

#endif /* PLUTO_WEBP_VP8_DATA_H */
