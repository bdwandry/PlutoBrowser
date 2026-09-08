/*
 * PlutoBrowser — webp.h
 * Port of Source/render/decoders/webp.lua (reference, 3052 lines) + the
 * generated tables in webp_vp8_data.c (webp_vp8_data.lua, file #30).
 *
 * Phase status (see MASTER_TODO):
 *   P27 (this phase): VP8 constant tables; bit reader (LSB-first); canonical
 *               Huffman table builder (two-level, root 8); readSymbol /
 *               readSymbol7; readHuffmanCodeLengths; readHuffmanCode
 *               (simple-code + code-length-code paths).
 *   P28: VP8L lossless — meta huffman groups, color cache, decodeImageData,
 *        predictors, inverse transforms (incl. color-map expand).
 *   P29: VP8 lossy (bool decoder, headers, frame decode, loop filter,
 *        upsampling), alpha plane, RIFF container dispatch, composite ->
 *        scale -> dither public entry.
 *
 * Lua -> C function map (P27):
 *   newBitReader(data, startPos)  -> webp_br_init()
 *   brPrefetch(br, want)          -> webp_br_prefetch()
 *   brAdvance(br, n)              -> webp_br_advance()
 *   brReadBits(br, n)             -> webp_br_read_bits()
 *   readSymbol(table, br)         -> webp_read_symbol()
 *   readSymbol7(table, br)        -> webp_read_symbol7()
 *   replicateValue/getNextKey/
 *   nextTableBitSize              -> static helpers
 *   buildHuffmanTable(...)        -> webp_build_huffman_table()
 *   readHuffmanCodeLengths(...)   -> webp_read_huffman_code_lengths()
 *   readHuffmanCode(...)          -> webp_read_huffman_code()
 *
 * Table representation (identical to the reference): flat uint32 entries
 * encoding (bits shifted left 16) OR value, two-level with an 8-bit root
 * (HUFFMAN_TABLE_BITS) or 7-bit root for the code-length table.
 *
 * Stack discipline (P22 rule): public entry points are shallow; the builder
 * uses fixed scratch (count/offset uint16 arrays sized MAX_ALLOWED_CODE_LENGTH+1)
 * and a caller-provided or heap-grown table array.
 */
#ifndef PLUTO_WEBP_H
#define PLUTO_WEBP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants (libwebp format_constants.h / huffman_utils.h, verbatim) ──── */
#define WEBP_VP8L_MAGIC_BYTE          0x2F
#define WEBP_VP8L_IMAGE_SIZE_BITS     14
#define WEBP_HUFFMAN_CODES_PER_META   5
#define WEBP_MAX_CACHE_BITS           11
#define WEBP_DEFAULT_CODE_LENGTH      8
#define WEBP_MAX_ALLOWED_CODE_LENGTH  15
#define WEBP_NUM_LITERAL_CODES        256
#define WEBP_NUM_LENGTH_CODES         24
#define WEBP_NUM_DISTANCE_CODES       40
#define WEBP_NUM_CODE_LENGTH_CODES    19
#define WEBP_CODE_TO_PLANE_CODES      120
#define WEBP_MIN_HUFFMAN_BITS         2
#define WEBP_NUM_HUFFMAN_BITS         3
#define WEBP_MIN_TRANSFORM_BITS       2
#define WEBP_NUM_TRANSFORM_BITS       3
#define WEBP_HUFFMAN_TABLE_BITS       8
#define WEBP_HUFFMAN_TABLE_MASK       ((1 << WEBP_HUFFMAN_TABLE_BITS) - 1)
#define WEBP_LENGTHS_TABLE_BITS       7
#define WEBP_LENGTHS_TABLE_MASK       ((1 << WEBP_LENGTHS_TABLE_BITS) - 1)
#define WEBP_ARGB_BLACK               0xFF000000u

#define WEBP_PREDICTOR_TRANSFORM       0
#define WEBP_CROSS_COLOR_TRANSFORM     1
#define WEBP_SUBTRACT_GREEN_TRANSFORM  2
#define WEBP_COLOR_INDEXING_TRANSFORM  3

#define WEBP_K_CODE_LENGTH_LITERALS       16
#define WEBP_K_CODE_LENGTH_REPEAT_CODE    16
/* kCodeLengthExtraBits = {2,3,7}; kCodeLengthRepeatOffsets = {3,3,11} */
extern const uint8_t  webp_code_length_extra_bits[3];
extern const uint8_t  webp_code_length_repeat_offsets[3];
extern const uint8_t  webp_code_length_code_order[WEBP_NUM_CODE_LENGTH_CODES];
extern const uint8_t  webp_code_to_plane[WEBP_CODE_TO_PLANE_CODES];
/* kAlphabetSize = {280, 256, 256, 256, 40}; kLiteralMap = {0,1,1,1,0} */
#define WEBP_ALPHABET_SIZE_GREEN (WEBP_NUM_LITERAL_CODES + WEBP_NUM_LENGTH_CODES)
extern const int webp_alphabet_size[5];
extern const uint8_t webp_literal_map[5];

/* Two-level Huffman table capacity: root table (1<<8) plus worst-case second
 * level tables. libwebp uses the same bound: sum over lengths 9..15 of
 * ceil(count)/2^(len-8) cannot exceed 2^8 + 2^8 + ... = 1<<9 second-level
 * entries per overflow; the safe flat bound used here is (1<<15) entries,
 * which covers any valid code (worst case ~ 2*(1<<8) tables of <= 2^7).
 * A table array of WEBP_HUFFMAN_TABLE_MAX entries is always sufficient. */
#define WEBP_HUFFMAN_TABLE_MAX  (1 << 15)
#define WEBP_MAX_PIXELS         (2048 * 2048)

/* ── Bit reader (little-endian, LSB-first — verbatim semantics) ──────────── */
typedef struct WebPBitReader
{
    const uint8_t *data;
    int len;      /* Lua: #data */
    int pos;      /* 1-based byte position into data (Lua byte indexing) */
    uint32_t window;
    int nbits;
    int eos;
} WebPBitReader;

void webp_br_init(WebPBitReader *br, const uint8_t *data, int len, int startPos);
uint32_t webp_br_prefetch(WebPBitReader *br, int want);
void webp_br_advance(WebPBitReader *br, int n);
uint32_t webp_br_read_bits(WebPBitReader *br, int n);

/* ── Huffman symbol reading ───────────────────────────────────────────────── */
uint32_t webp_read_symbol(const uint32_t *table, WebPBitReader *br);
uint32_t webp_read_symbol7(const uint32_t *table, WebPBitReader *br);

/* ── Huffman table construction ─────────────────────────────────────────────
 * Builds the flat two-level lookup table from code lengths. Returns the
 * number of entries used (0 on error — Lua returns nil), and fills
 * out_table (capacity WEBP_HUFFMAN_TABLE_MAX uint32 entries). Returns the
 * total table size (>0) on success, 0 on error. NOTE: a valid table may be
 * all zeros (single-symbol code for symbol 0), so success is size>0, never
 * a nonzero entry scan.
 * codeLengths: numSymbols entries (0..15). rootBits: 8 (symbols) or 7 (the
 * code-length table). */
int webp_build_huffman_table(const uint8_t *codeLengths, int codeLengthsSize,
                             int rootBits, uint32_t *out_table);

/* High-level code reader: simple-code path or full code-length-code path.
 * Returns 1 and fills out_table on success; 0 on error (Lua nil).
 * scratch_code_lengths must hold alphabetSize bytes (caller scratch). */
int webp_read_huffman_code(int alphabetSize, WebPBitReader *br,
                           uint8_t *scratch_code_lengths, uint32_t *out_table);

/* Internal: readHuffmanCodeLengths — exposed for the P27 battery only. */
int webp_read_huffman_code_lengths(WebPBitReader *br,
                                   const uint8_t *codeLengthCodeLengths,
                                   int numSymbols, uint8_t *codeLengths,
                                   uint32_t *lengths_table_scratch);

/* ── P28: VP8L lossless payload decode (decodeVP8LPayload) ─────────────────
 * payload: the VP8L chunk body (starting with the 0x2F magic byte).
 * On success returns a heap ARGB pixel array (caller frees with PLUTO_FREE)
 * and sets outW and outH to the IMAGE dimensions (pre-transform), with
 * outOwned set to 1. On error returns NULL. Transforms are fully applied
 * before return. */
uint32_t *webp_decode_vp8l_payload(const uint8_t *payload, size_t payloadLen,
                                   int *outW, int *outH, int *outOwned);

/* ── P29: VP8 lossy + container + animation (webp_vp8.c/webp_container.c) ───
 * webp_decode: full decode pipeline (parse container → payload decode →
 * ARGB → grayscale box-scale → Bayer dither) == WebPDecoder.decode.
 * webp_decode_raw: full-file ARGB dump == WebPDecoder._testDecodeRaw.
 * webp_decode_animation: blended frames (== WebPDecoder.decodeAnimation). */
LCDBitmap *webp_decode(const uint8_t *data, size_t len, int maxW, int maxH);
uint32_t *webp_decode_raw(const uint8_t *data, size_t len, int *outW, int *outH);

#ifdef __cplusplus
}
#endif

#endif /* PLUTO_WEBP_H */
