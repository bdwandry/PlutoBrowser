/*
 * PlutoBrowser — jpeg.h
 * Port of Source/render/decoders/jpeg.lua (reference, 670 lines).
 *
 * Lua → C function map:
 *   JPEGDecoder.decode(data, maxW, maxH) → jpeg_decode()
 *   decodeRawImageData dispatch (async via Tasks) stays in image_decoder
 *   (P2x later) — the decoder itself is synchronous here, like the Lua
 *   function JPEGDecoder.decode; the async wrapper is image_decoder's job.
 *   newReader/readBits/expectRestart    → JpegReader + jpeg_rd_* fns
 *   buildHuff/decodeSymbol/extend       → jpeg_huff_* / jpeg_extend
 *   decodeDC/decodeAC                   → jpeg_decode_dc / jpeg_decode_ac
 *   idct2d (fixed-point separable)      → jpeg_idct2d (verbatim constants)
 *   decodeBaseline                      → jpeg_decode_baseline
 *   decodeProgressiveDC                 → jpeg_decode_prog_dc
 *   renderProgressiveDC                 → jpeg_render_prog_dc
 *   parseHuffSegment/parseQTSegment     → inline in jpeg_decode_gray
 *
 * Preserved semantics (verified against the reference):
 *   - Baseline (SOF0): luma-only 8x8 IDCT; chroma coefficients consumed for
 *     bitstream sync but NOT upsampled (luma-only output by design).
 *   - DC-only fast path when boxW >= 4 or boxH >= 4 or w*h > 200000:
 *     consume AC symbols for sync, render DC*q/8+128 flat blocks.
 *   - Progressive (SOF2): DC scans only (ss==0&&se==0); DC refinement scans
 *     (ah~=0) apply ±(1<<al) correction bits; the FIRST AC scan triggers the
 *     render and stops the parse. No-AC-yet truncation still renders DCs.
 *   - Arithmetic JPEGs (SOF9/10/11) fall into the "skip segment" path and
 *     produce acc==NULL → nil (same observable result as the reference).
 *   - Zigzag[zz] → natural order; extend() two's-complement; DC prediction
 *     per component (baseline) / per component in state (progressive).
 *   - Restart markers: expectRestart resets the bit buffer, requires the
 *     exact RSTn (n = mcuIndex % 8), resets DC predictors.
 *   - Streaming: rows go straight into Scale.newAccum; addRow(NULL) skips
 *     (missing rows past truncation), trailing partial row block handled by
 *     scale.c exactly like the reference's finish().
 *   - Output: Dither.toImage(grid, targetW, targetH) — 255 for missing.
 */
#ifndef PLUTO_JPEG_H
#define PLUTO_JPEG_H

#include <stdint.h>
#include <stddef.h>
#include "pd_api.h"

/* Full decode: data → dithered 1-bit LCDBitmap (or NULL). Matches
 * JPEGDecoder.decode(data, maxW, maxH); maxW/maxH default to 360/200 when 0. */
LCDBitmap *jpeg_decode(const uint8_t *data, size_t len, int maxW, int maxH);

/* Test seam: decode to the pre-dither grayscale grid (scale.c output rows).
 * Returns rows (caller frees via jpeg_gray_free) or NULL. The battery uses
 * this to compare against ffmpeg's decoded pixels BEFORE the 1-bit dither
 * collapses them — the same grid Dither.toImage would consume. */
uint8_t **jpeg_decode_gray(const uint8_t *data, size_t len,
                           int maxW, int maxH,
                           int *outCount, int *outWidth);
void jpeg_gray_free(uint8_t **rows);

#endif /* PLUTO_JPEG_H */
