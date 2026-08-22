// webp.c — C port of Source/render/decoders/webp.lua.
//
// Parses the RIFF container and decodes VP8L (lossless), VP8 (lossy)
// and animated payloads into a full-resolution ARGB grid (0xAARRGGBB).
// Lossless: little-endian bit reader, two-level Huffman tables (rootBits
// 8, code-length table 7), meta Huffman groups, LZ77 with distance
// mapping, color cache, and all four inverse transforms. Lossy:
// boolean decoder (BITS=24), segment/filter/quant/probability headers,
// residual tokens, intra predictions, DCT/WHT transforms, loop filter,
// and fancy chroma upsampling byte-compatible with `dwebp -ppm`.
// Animation: VP8X/ANIM/ANMF demux + non-premultiplied blender.

#include "render/decoders/webp.h"

#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"
#include "render/decoders/webp_vp8_data.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_WEBP_PD 1
#endif

#define VP8L_MAGIC_BYTE          0x2F
#define MAX_CACHE_BITS           11
#define DEFAULT_CODE_LENGTH      8
#define MAX_ALLOWED_CODE_LENGTH  15
#define NUM_LITERAL_CODES        256
#define NUM_LENGTH_CODES         24
#define NUM_DISTANCE_CODES       40
#define NUM_CODE_LENGTH_CODES    19
#define CODE_TO_PLANE_CODES      120
#define MIN_HUFFMAN_BITS         2
#define NUM_HUFFMAN_BITS         3
#define MIN_TRANSFORM_BITS       2
#define NUM_TRANSFORM_BITS       3
#define HUFFMAN_TABLE_BITS       8
#define HUFFMAN_TABLE_MASK       ((1u << HUFFMAN_TABLE_BITS) - 1)
#define LENGTHS_TABLE_BITS       7
#define LENGTHS_TABLE_MASK       ((1u << LENGTHS_TABLE_BITS) - 1)
#define ARGB_BLACK               0xFF000000u
#define WEBP_MAX_PIXELS          (2048 * 2048)

#define PREDICTOR_TRANSFORM       0
#define CROSS_COLOR_TRANSFORM     1
#define SUBTRACT_GREEN_TRANSFORM  2
#define COLOR_INDEXING_TRANSFORM  3

static const int kCodeLengthExtraBits[3] = { 2, 3, 7 };
static const int kCodeLengthRepeatOffsets[3] = { 3, 3, 11 };
static const int kCodeLengthCodeOrder[NUM_CODE_LENGTH_CODES] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
static const uint8_t kCodeToPlane[CODE_TO_PLANE_CODES] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a,
    0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
    0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
    0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e,
    0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
    0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};
static const int kAlphabetSize[5] = {
    NUM_LITERAL_CODES + NUM_LENGTH_CODES,
    NUM_LITERAL_CODES,
    NUM_LITERAL_CODES,
    NUM_LITERAL_CODES,
    NUM_DISTANCE_CODES,
};
static const int kLiteralMap[5] = { 0, 1, 1, 1, 0 };

/* ------------------------------------------------------------------ */
/* Little-endian bit reader (VP8L reads bits LSB-first)                */

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;      /* next byte to load (0-based) */
    uint32_t window;
    int nbits;
    int eos;
} Br;

static int br_prefetch(Br* br, int want) {
    while (br->nbits < want && br->pos < br->len) {
        br->window |= (uint32_t)br->data[br->pos] << br->nbits;
        br->pos++;
        br->nbits += 8;
    }
    return (int)(br->window & (((uint32_t)1 << want) - 1));
}

static void br_advance(Br* br, int n) {
    br->nbits -= n;
    br->window >>= n;
    if (br->nbits < 0) {
        br->eos = 1;
        br->nbits = 0;
    }
}

static int br_read_bits(Br* br, int n) {
    int v;
    if (n == 0) return 0;
    v = br_prefetch(br, n);
    if (br->nbits < n) {
        br->eos = 1;
        return 0;
    }
    br_advance(br, n);
    return v;
}

/* One Huffman symbol from a two-level table of (bits << 16) | value. */
static int read_symbol(const uint32_t* table, Br* br) {
    int val = br_prefetch(br, HUFFMAN_TABLE_BITS * 2);
    int low = val & (int)HUFFMAN_TABLE_MASK;
    uint32_t entry = table[low];
    int nbits = (int)(entry >> 16) - HUFFMAN_TABLE_BITS;
    if (nbits > 0) {
        int val2;
        br_advance(br, HUFFMAN_TABLE_BITS);
        val2 = br_prefetch(br, nbits);
        entry = table[low + (int)(entry & 0xFFFF) +
                      (val2 & (int)(((uint32_t)1 << nbits) - 1))];
        br_advance(br, (int)(entry >> 16));
    } else {
        br_advance(br, (int)(entry >> 16));
    }
    return (int)(entry & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* Huffman table construction (BuildHuffmanTable from huffman_utils.c) */

static void replicate_value(uint32_t* table, int base, int step, int end,
                            uint32_t code) {
    int i;
    for (i = end - step; i >= 0; i -= step) table[base + i] = code;
}

static int get_next_key(int key, int len) {
    int step = 1 << (len - 1);
    while ((key & step) != 0) step >>= 1;
    if (step == 0) return key;
    return (key & (step - 1)) + step;
}

static int next_table_bit_size(const int* count, int len, int rootBits) {
    int left = 1 << (len - rootBits);
    while (len < MAX_ALLOWED_CODE_LENGTH) {
        left -= count[len];
        if (left <= 0) break;
        len++;
        left <<= 1;
    }
    return len - rootBits;
}

/* Returns a malloc'd flat lookup table, NULL on invalid code lengths. */
static uint32_t* build_huffman_table(const uint8_t* codeLengths,
                                     int codeLengthsSize, int rootBits) {
    int totalSize = 1 << rootBits;
    int cap = totalSize;
    uint32_t* table;
    int count[MAX_ALLOWED_CODE_LENGTH + 1];
    int offset[MAX_ALLOWED_CODE_LENGTH + 1];
    int* sorted;
    int len, symbol;
    long long low = 0xFFFFFFFFLL;
    uint32_t mask;
    int key = 0, numNodes = 1, numOpen = 1;
    int sortedPos = 0;
    int tableBits = rootBits, tableSize = 1 << tableBits;
    int tablePos = 0;

    memset(count, 0, sizeof(count));
    memset(offset, 0, sizeof(offset));
    for (symbol = 0; symbol < codeLengthsSize; symbol++) {
        int cl = codeLengths[symbol];
        if (cl > MAX_ALLOWED_CODE_LENGTH) return NULL;
        count[cl]++;
    }
    if (count[0] == codeLengthsSize) return NULL;

    offset[1] = 0;
    for (len = 1; len < MAX_ALLOWED_CODE_LENGTH; len++) {
        if (count[len] > (1 << len)) return NULL;
        offset[len + 1] = offset[len] + count[len];
    }

    sorted = (int*)pluto_malloc(
        sizeof(int) * (size_t)(codeLengthsSize > 0 ? codeLengthsSize : 1));
    if (!sorted) return NULL;
    for (symbol = 0; symbol < codeLengthsSize; symbol++) {
        int cl = codeLengths[symbol];
        if (cl > 0) {
            if (offset[cl] >= codeLengthsSize) {
                pluto_free(sorted);
                return NULL;
            }
            sorted[offset[cl]] = symbol;
            offset[cl]++;
        }
    }

    if (offset[MAX_ALLOWED_CODE_LENGTH] == 1) {
        /* Single-symbol tree: value repeats, no bits consumed. */
        table = (uint32_t*)pluto_malloc(sizeof(uint32_t) * (size_t)totalSize);
        if (!table) {
            pluto_free(sorted);
            return NULL;
        }
        replicate_value(table, 0, 1, totalSize, (uint32_t)sorted[0]);
        pluto_free(sorted);
        return table;
    }

    table = (uint32_t*)pluto_malloc(sizeof(uint32_t) * (size_t)cap);
    if (!table) {
        pluto_free(sorted);
        return NULL;
    }
    mask = (uint32_t)totalSize - 1;

    /* Root table. */
    for (len = 1; len <= rootBits; len++) {
        int step = 2 << (len - 1);
        numOpen <<= 1;
        numNodes += numOpen;
        numOpen -= count[len];
        if (numOpen < 0) goto fail;
        while (count[len] > 0) {
            uint32_t code =
                ((uint32_t)len << 16) | (uint32_t)sorted[sortedPos++];
            count[len]--;
            replicate_value(table, key, step, tableSize, code);
            key = get_next_key(key, len);
        }
    }

    /* Second-level tables + pointers stored in the root table. */
    for (len = rootBits + 1; len <= MAX_ALLOWED_CODE_LENGTH; len++) {
        int step = 2 << (len - rootBits - 1);
        numOpen <<= 1;
        numNodes += numOpen;
        numOpen -= count[len];
        if (numOpen < 0) goto fail;
        while (count[len] > 0) {
            uint32_t* grown;
            count[len]--;
            if ((key & (int)mask) != (int)low) {
                tablePos += tableSize;
                tableBits = next_table_bit_size(count, len, rootBits);
                tableSize = 1 << tableBits;
                totalSize += tableSize;
                low = key & (int)mask;
                if (totalSize > cap) {
                    grown = (uint32_t*)pluto_realloc(
                        table, sizeof(uint32_t) * (size_t)totalSize);
                    if (!grown) goto fail;
                    table = grown;
                    cap = totalSize;
                }
                table[low] = ((uint32_t)(tableBits + rootBits) << 16) |
                             (uint32_t)(tablePos - low);
            }
            replicate_value(table, tablePos + (key >> rootBits), step,
                            tableSize,
                            ((uint32_t)(len - rootBits) << 16) |
                                (uint32_t)sorted[sortedPos++]);
            key = get_next_key(key, len);
        }
    }

    if (numNodes != 2 * offset[MAX_ALLOWED_CODE_LENGTH] - 1) goto fail;
    pluto_free(sorted);
    return table;

fail:
    pluto_free(sorted);
    pluto_free(table);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Code-length codes / Huffman code reading                            */

static int read_huffman_code_lengths(Br* br,
                                     const uint8_t* codeLengthCodeLengths,
                                     int numSymbols, uint8_t* codeLengths) {
    uint32_t* table =
        build_huffman_table(codeLengthCodeLengths, NUM_CODE_LENGTH_CODES,
                            LENGTHS_TABLE_BITS);
    int maxSymbol, symbol, prevCodeLen;
    if (!table) return 0;
    if (br_read_bits(br, 1) == 1) {
        int lengthNbits = 2 + 2 * br_read_bits(br, 3);
        maxSymbol = 2 + br_read_bits(br, lengthNbits);
        if (maxSymbol > numSymbols) {
            pluto_free(table);
            return 0;
        }
    } else {
        maxSymbol = numSymbols;
    }

    symbol = 0;
    prevCodeLen = DEFAULT_CODE_LENGTH;
    while (symbol < numSymbols) {
        int val, codeLen;
        if (maxSymbol == 0) break;
        maxSymbol--;
        val = br_prefetch(br, LENGTHS_TABLE_BITS * 2 + 1);
        if (br->nbits < LENGTHS_TABLE_BITS) {
            br->eos = 1;
            pluto_free(table);
            return 0;
        }
        {
            uint32_t p = table[val & LENGTHS_TABLE_MASK];
            br_advance(br, (int)(p >> 16));
            codeLen = (int)(p & 0xFFFF);
        }
        if (codeLen < 16) {
            codeLengths[symbol++] = (uint8_t)codeLen;
            if (codeLen != 0) prevCodeLen = codeLen;
        } else {
            int usePrev = (codeLen == 16);
            int slot = codeLen - 16;
            int repeatCount = br_read_bits(br, kCodeLengthExtraBits[slot]) +
                              kCodeLengthRepeatOffsets[slot];
            int length = usePrev ? prevCodeLen : 0;
            if (symbol + repeatCount > numSymbols) {
                pluto_free(table);
                return 0;
            }
            while (repeatCount-- > 0) codeLengths[symbol++] = (uint8_t)length;
        }
    }
    pluto_free(table);
    return 1;
}

static uint32_t* read_huffman_code(int alphabetSize, Br* br,
                                   uint8_t* codeLengths,
                                   uint8_t* codeLengthCodeLengths) {
    memset(codeLengths, 0, (size_t)alphabetSize);
    if (br_read_bits(br, 1) == 1) {
        int numSymbols = br_read_bits(br, 1) + 1;
        int firstSymbolLenCode = br_read_bits(br, 1);
        int symbol = br_read_bits(br, firstSymbolLenCode == 0 ? 1 : 8);
        if (symbol >= alphabetSize) return NULL;
        codeLengths[symbol] = 1;
        if (numSymbols == 2) {
            symbol = br_read_bits(br, 8);
            if (symbol >= alphabetSize) return NULL;
            codeLengths[symbol] = 1;
        }
    } else {
        int numCodes = br_read_bits(br, 4) + 4;
        int i;
        memset(codeLengthCodeLengths, 0, NUM_CODE_LENGTH_CODES);
        for (i = 0; i < numCodes; i++) {
            codeLengthCodeLengths[kCodeLengthCodeOrder[i]] =
                (uint8_t)br_read_bits(br, 3);
        }
        if (!read_huffman_code_lengths(br, codeLengthCodeLengths, alphabetSize,
                                       codeLengths)) {
            return NULL;
        }
    }
    if (br->eos) return NULL;
    return build_huffman_table(codeLengths, alphabetSize, HUFFMAN_TABLE_BITS);
}

/* ------------------------------------------------------------------ */
/* Meta Huffman codes / htree groups                                   */

typedef struct {
    uint32_t* htrees[5];
    int isTrivialLiteral;
    int isTrivialCode;
    uint32_t literalArb;
    uint32_t literalArgb;
} HTreeGroup;

typedef struct {
    int type;
    int xsize, ysize;
    int bits;
    uint32_t* data;
} Transform;

typedef struct {
    uint32_t* colors;
    int hashShift;
} ColorCache;

typedef struct {
    int colorCacheSize;
    ColorCache* colorCache;
    int huffmanSubsampleBits;
    int huffmanXsize;
    int* huffmanImage;       /* meta-cell -> group id (NULL when absent) */
    int numHtreeGroups;
    int numHtreeGroupsMax;
    HTreeGroup* htreeGroups; /* calloc'd [numHtreeGroupsMax] */
    int transformXsize, transformYsize;
    Transform transforms[4];
    int numTransforms;
} VP8LCtx;

static void free_color_cache(ColorCache* cc);

static void ctx_free(VP8LCtx* ctx) {
    int i, j;
    if (ctx->htreeGroups) {
        for (i = 0; i < ctx->numHtreeGroupsMax; i++)
            for (j = 0; j < 5; j++) pluto_free(ctx->htreeGroups[i].htrees[j]);
        pluto_free(ctx->htreeGroups);
        ctx->htreeGroups = NULL;
    }
    pluto_free(ctx->huffmanImage);
    ctx->huffmanImage = NULL;
    free_color_cache(ctx->colorCache);
    ctx->colorCache = NULL;
    for (i = 0; i < ctx->numTransforms; i++) {
        pluto_free(ctx->transforms[i].data);
        ctx->transforms[i].data = NULL;
    }
}

static int decode_image_stream(int xsize, int ysize, int isLevel0, Br* br,
                               VP8LCtx* ctx, uint32_t** outData);

static int sub_sample_size(int v, int n) {
    return (v + (1 << n) - 1) >> n;
}

static int read_huffman_codes(Br* br, int xsize, int ysize, int colorCacheBits,
                              int allowRecursion, VP8LCtx* ctx) {
    int* huffmanImage = NULL;
    int numHtreeGroups = 1;
    int numHtreeGroupsMax = 1;
    int* mapping = NULL;
    HTreeGroup* groups = NULL;
    uint8_t codeLengths[2328];
    uint8_t codeLengthCodeLengths[NUM_CODE_LENGTH_CODES];
    int i, j;

    if (allowRecursion && br_read_bits(br, 1) == 1) {
        int huffmanPrecision =
            MIN_HUFFMAN_BITS + br_read_bits(br, NUM_HUFFMAN_BITS);
        int huffmanXsize = sub_sample_size(xsize, huffmanPrecision);
        int huffmanYsize = sub_sample_size(ysize, huffmanPrecision);
        int huffmanPixs = huffmanXsize * huffmanYsize;
        uint32_t* pixs = NULL;
        int ok;
        VP8LCtx subCtx;
        memset(&subCtx, 0, sizeof(subCtx));
        ok = decode_image_stream(huffmanXsize, huffmanYsize, 0, br, &subCtx,
                                 &pixs);
        ctx_free(&subCtx);
        if (!ok) {
            pluto_free(pixs);
            return 0;
        }
        huffmanImage = (int*)pluto_malloc(sizeof(int) * (size_t)huffmanPixs);
        if (!huffmanImage) {
            pluto_free(pixs);
            return 0;
        }
        ctx->huffmanSubsampleBits = huffmanPrecision;
        for (i = 0; i < huffmanPixs; i++) {
            int group = (int)((pixs[i] >> 8) & 0xFFFF);
            huffmanImage[i] = group;
            if (group >= numHtreeGroupsMax) numHtreeGroupsMax = group + 1;
        }
        pluto_free(pixs);
        if (numHtreeGroupsMax > 1000 || numHtreeGroupsMax > xsize * ysize) {
            mapping =
                (int*)pluto_malloc(sizeof(int) * (size_t)numHtreeGroupsMax);
            if (!mapping) {
                pluto_free(huffmanImage);
                return 0;
            }
            for (i = 0; i < numHtreeGroupsMax; i++) mapping[i] = -1;
            numHtreeGroups = 0;
            for (i = 0; i < huffmanPixs; i++) {
                int g = huffmanImage[i];
                int mapped = mapping[g];
                if (mapped == -1) {
                    mapped = numHtreeGroups;
                    mapping[g] = mapped;
                    numHtreeGroups++;
                }
                huffmanImage[i] = mapped;
            }
        } else {
            numHtreeGroups = numHtreeGroupsMax;
        }
    }

    if (br->eos) {
        pluto_free(huffmanImage);
        pluto_free(mapping);
        return 0;
    }

    groups =
        (HTreeGroup*)pluto_calloc((size_t)numHtreeGroupsMax, sizeof(HTreeGroup));
    if (!groups) {
        pluto_free(huffmanImage);
        pluto_free(mapping);
        return 0;
    }

    for (i = 0; i < numHtreeGroupsMax; i++) {
        HTreeGroup* grp;
        int gi;
        if (mapping && mapping[i] == -1) {
            /* Encoded but unreferenced group: consume and discard. */
            for (j = 0; j < 5; j++) {
                int alphaSize = kAlphabetSize[j];
                uint32_t* t;
                if (j == 0 && colorCacheBits > 0) alphaSize += 1 << colorCacheBits;
                t = read_huffman_code(alphaSize, br, codeLengths,
                                      codeLengthCodeLengths);
                if (!t) goto fail;
                pluto_free(t);
            }
            continue;
        }
        gi = (mapping == NULL) ? i : mapping[i];
        grp = &groups[gi];
        {
            int totalSize = 0;
            int isTrivialLiteral = 1;
            for (j = 0; j < 5; j++) {
                int alphaSize = kAlphabetSize[j];
                uint32_t* t;
                int firstBits;
                if (j == 0 && colorCacheBits > 0) alphaSize += 1 << colorCacheBits;
                t = read_huffman_code(alphaSize, br, codeLengths,
                                      codeLengthCodeLengths);
                if (!t) goto fail;
                grp->htrees[j] = t;
                firstBits = (int)(t[0] >> 16);
                totalSize += firstBits;
                if (kLiteralMap[j] == 1 && firstBits != 0) isTrivialLiteral = 0;
            }
            if (isTrivialLiteral) {
                uint32_t red = grp->htrees[1][0] & 0xFFFF;
                uint32_t blue = grp->htrees[2][0] & 0xFFFF;
                uint32_t alpha = grp->htrees[3][0] & 0xFFFF;
                grp->isTrivialLiteral = 1;
                grp->literalArb = (alpha << 24) | (red << 16) | blue;
                if (totalSize == 0) {
                    uint32_t greenVal = grp->htrees[0][0] & 0xFFFF;
                    if (greenVal < NUM_LITERAL_CODES) {
                        grp->isTrivialCode = 1;
                        grp->literalArgb = grp->literalArb | (greenVal << 8);
                    }
                }
            }
        }
    }

    ctx->huffmanImage = huffmanImage;
    ctx->numHtreeGroups = numHtreeGroups;
    ctx->numHtreeGroupsMax = numHtreeGroupsMax;
    ctx->htreeGroups = groups;
    pluto_free(mapping);
    return 1;

fail:
    for (i = 0; i < numHtreeGroupsMax; i++) {
        for (j = 0; j < 5; j++) pluto_free(groups[i].htrees[j]);
    }
    pluto_free(groups);
    pluto_free(huffmanImage);
    pluto_free(mapping);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LZ77 helpers                                                        */

static int get_copy_distance(int distanceSymbol, Br* br) {
    int extraBits, offset;
    if (distanceSymbol < 4) return distanceSymbol + 1;
    extraBits = (distanceSymbol - 2) >> 1;
    offset = (2 + (distanceSymbol & 1)) << extraBits;
    return offset + br_read_bits(br, extraBits) + 1;
}

static int plane_code_to_distance(int xsize, int planeCode) {
    int distCode, yoffset, xoffset, dist;
    if (planeCode > CODE_TO_PLANE_CODES) return planeCode - CODE_TO_PLANE_CODES;
    distCode = kCodeToPlane[planeCode - 1];
    yoffset = distCode >> 4;
    xoffset = 8 - (distCode & 0xF);
    dist = yoffset * xsize + xoffset;
    if (dist < 1) return 1;
    return dist;
}

/* ------------------------------------------------------------------ */
/* Color cache                                                         */

#define K_HASH_MUL 0x1e35a7bdu

static ColorCache* new_color_cache(int bits) {
    ColorCache* cc = (ColorCache*)pluto_malloc(sizeof(ColorCache));
    if (!cc) return NULL;
    cc->colors = (uint32_t*)pluto_calloc((size_t)1 << bits, sizeof(uint32_t));
    if (!cc->colors) {
        pluto_free(cc);
        return NULL;
    }
    cc->hashShift = 32 - bits;
    return cc;
}

static void free_color_cache(ColorCache* cc) {
    if (!cc) return;
    pluto_free(cc->colors);
    pluto_free(cc);
}

static void color_cache_insert(ColorCache* cc, uint32_t argb) {
    uint32_t key = (argb * K_HASH_MUL) >> cc->hashShift;
    cc->colors[key] = argb;
}

/* ------------------------------------------------------------------ */
/* Image data (LZ77) decoding                                          */

static int decode_image_data(Br* br, VP8LCtx* ctx, uint32_t* data,
                             int width, int height) {
    int row = 0, col = 0, src = 0;
    int lenCodeLimit = NUM_LITERAL_CODES + NUM_LENGTH_CODES;
    int colorCacheLimit = lenCodeLimit + ctx->colorCacheSize;
    int srcEnd = width * height;
    int huffmanBits = ctx->huffmanSubsampleBits;
    const int* huffmanImage = ctx->huffmanImage;
    int huffmanXsize = ctx->huffmanXsize;
    HTreeGroup* htreeGroups = ctx->htreeGroups;
    ColorCache* colorCache = ctx->colorCache;
    unsigned mask =
        (huffmanBits == 0) ? 0xFFFFFFFFu : ((1u << huffmanBits) - 1);
    HTreeGroup* group = NULL;
    int lastCached = 0;

    while (src < srcEnd) {
        tasks_yield_check();
        if ((col & mask) == 0) {
            int metaIndex = 0;
            if (huffmanBits != 0)
                metaIndex = huffmanImage[huffmanXsize * (row >> huffmanBits) +
                                         (col >> huffmanBits)];
            group = &htreeGroups[metaIndex];
        }
        if (!group || !group->htrees[0]) return 0;
        if (group->isTrivialCode) {
            data[src++] = group->literalArgb;
            col++;
            if (col >= width) {
                col = 0;
                row++;
            }
            if (colorCache) {
                while (lastCached < src)
                    color_cache_insert(colorCache, data[lastCached++]);
            }
        } else {
            int code = read_symbol(group->htrees[0], br);
            if (code < NUM_LITERAL_CODES) {
                uint32_t pixel;
                if (group->isTrivialLiteral) {
                    pixel = group->literalArb | ((uint32_t)code << 8);
                } else {
                    int red = read_symbol(group->htrees[1], br);
                    int blue = read_symbol(group->htrees[2], br);
                    int alpha = read_symbol(group->htrees[3], br);
                    pixel = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
                            ((uint32_t)code << 8) | (uint32_t)blue;
                }
                data[src++] = pixel;
                col++;
                if (col >= width) {
                    col = 0;
                    row++;
                }
                if (colorCache) {
                    while (lastCached < src)
                        color_cache_insert(colorCache, data[lastCached++]);
                }
            } else if (code < lenCodeLimit) {
                int length = get_copy_distance(code - NUM_LITERAL_CODES, br);
                int distSymbol = read_symbol(group->htrees[4], br);
                int dist = plane_code_to_distance(
                    width, get_copy_distance(distSymbol, br));
                int i;
                if (src - dist < 0 || src + length > srcEnd) return 0;
                for (i = 0; i < length; i++)
                    data[src + i] = data[src + i - dist];
                src += length;
                col += length;
                while (col >= width) {
                    col -= width;
                    row++;
                }
                if ((col & mask) != 0) {
                    int metaIndex = 0;
                    if (huffmanBits != 0)
                        metaIndex =
                            huffmanImage[huffmanXsize * (row >> huffmanBits) +
                                         (col >> huffmanBits)];
                    group = &htreeGroups[metaIndex];
                }
                if (colorCache) {
                    while (lastCached < src)
                        color_cache_insert(colorCache, data[lastCached++]);
                }
            } else if (code < colorCacheLimit) {
                uint32_t cached;
                while (lastCached < src)
                    color_cache_insert(colorCache, data[lastCached++]);
                cached = colorCache->colors[code - lenCodeLimit];
                data[src++] = cached;
                col++;
                if (col >= width) {
                    col = 0;
                    row++;
                }
                if (colorCache) {
                    while (lastCached < src)
                        color_cache_insert(colorCache, data[lastCached++]);
                }
            } else {
                return 0;
            }
        }
        if (br->eos) return 0;
    }
    if (br->eos) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Inverse transforms (ports from lossless.c)                          */

static uint32_t add_pixels(uint32_t a, uint32_t b) {
    uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u);
    uint32_t rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
    return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
}

static uint32_t average2(uint32_t a0, uint32_t a1) {
    return (((a0 ^ a1) & 0xFEFEFEFEu) >> 1) + (a0 & a1);
}

static uint32_t average3(uint32_t a0, uint32_t a1, uint32_t a2) {
    return average2(average2(a0, a2), a1);
}

static uint32_t average4(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return average2(average2(a0, a1), average2(a2, a3));
}

static uint32_t clip255(uint32_t a) {
    if (a < 256u) return a;
    return (~a >> 24) & 0xFFu;
}

static int div2trunc(int x) {
    if (x >= 0) return x >> 1;
    return -((-x) >> 1);
}

static int sub3(int a, int b, int c) {
    int bc = b - c; int ac = a - c;
    return (bc < 0 ? -bc : bc) - (ac < 0 ? -ac : ac);
}

static uint32_t select_p(uint32_t a, uint32_t b, uint32_t c) {
    int paMinusPb =
        sub3((int)(a >> 24), (int)(b >> 24), (int)(c >> 24)) +
        sub3((int)((a >> 16) & 0xFF), (int)((b >> 16) & 0xFF),
             (int)((c >> 16) & 0xFF)) +
        sub3((int)((a >> 8) & 0xFF), (int)((b >> 8) & 0xFF),
             (int)((c >> 8) & 0xFF)) +
        sub3((int)(a & 0xFF), (int)(b & 0xFF), (int)(c & 0xFF));
    if (paMinusPb <= 0) return a;
    return b;
}

static uint32_t clamped_add_subtract_full(uint32_t c0, uint32_t c1,
                                          uint32_t c2) {
    uint32_t a = clip255(((c0 >> 24) & 0xFF) + ((c1 >> 24) & 0xFF) -
                         ((c2 >> 24) & 0xFF));
    uint32_t r = clip255(((c0 >> 16) & 0xFF) + ((c1 >> 16) & 0xFF) -
                         ((c2 >> 16) & 0xFF));
    uint32_t g = clip255(((c0 >> 8) & 0xFF) + ((c1 >> 8) & 0xFF) -
                         ((c2 >> 8) & 0xFF));
    uint32_t b = clip255((c0 & 0xFF) + (c1 & 0xFF) - (c2 & 0xFF));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t add_sub_half(uint32_t a, uint32_t b) {
    return clip255((uint32_t)((int)a + div2trunc((int)a - (int)b)));
}

static uint32_t clamped_add_subtract_half(uint32_t c0, uint32_t c1,
                                          uint32_t c2) {
    uint32_t ave = average2(c0, c1);
    uint32_t a = add_sub_half((ave >> 24) & 0xFF, (c2 >> 24) & 0xFF);
    uint32_t r = add_sub_half((ave >> 16) & 0xFF, (c2 >> 16) & 0xFF);
    uint32_t g = add_sub_half((ave >> 8) & 0xFF, (c2 >> 8) & 0xFF);
    uint32_t b = add_sub_half(ave & 0xFF, c2 & 0xFF);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Predictor modes 0..13; args are (left, top, top1, topm1). */
static uint32_t predictor_value(int mode, uint32_t left, uint32_t top,
                                uint32_t top1, uint32_t topm1) {
    switch (mode) {
        case 0:  return ARGB_BLACK;
        case 1:  return left;
        case 2:  return top;
        case 3:  return top1;
        case 4:  return topm1;
        case 5:  return average3(left, top, top1);
        case 6:  return average2(left, topm1);
        case 7:  return average2(left, top);
        case 8:  return average2(topm1, top);
        case 9:  return average2(top, top1);
        case 10: return average4(left, topm1, top, top1);
        case 11: return select_p(top, left, topm1);
        case 12: return clamped_add_subtract_full(left, top, topm1);
        default: return clamped_add_subtract_half(left, top, topm1);
    }
}

static void predictor_add(int mode, const uint32_t* in, int iIn,
                          uint32_t* out, int oOut, int x, int count,
                          int width) {
    int i;
    int baseIn = iIn + x;
    int baseOut = oOut + x;
    for (i = 0; i < count; i++) {
        int px = baseOut + i;
        uint32_t pred = predictor_value(mode, out[px - 1], out[px - width],
                                        out[px - width + 1],
                                        out[px - width - 1]);
        out[px] = add_pixels(in[baseIn + i], pred);
    }
}

static void predictor_inverse(const Transform* t, const uint32_t* in,
                              uint32_t* out, int rows) {
    int width = t->xsize;
    int tileWidth = 1 << t->bits;
    int tilesPerRow = sub_sample_size(width, t->bits);
    const uint32_t* tdata = t->data;
    int iIn = width, oOut = width, y;

    out[0] = add_pixels(in[0], ARGB_BLACK);
    for (y = 1; y < width; y++) out[y] = add_pixels(in[y], out[y - 1]);

    y = 1;
    while (y < rows) {
        int predModeBase = (y >> t->bits) * tilesPerRow;
        int predModeSrc = predModeBase;
        int x = 1;
        tasks_yield_check();
        out[oOut] = add_pixels(in[iIn], out[oOut - width]);
        while (x < width) {
            int mode = (int)((tdata[predModeSrc] >> 8) & 0xF);
            int xEnd = (x & ~(tileWidth - 1)) + tileWidth;
            predModeSrc++;
            if (xEnd > width) xEnd = width;
            predictor_add(mode, in, iIn, out, oOut, x, xEnd - x, width);
            x = xEnd;
        }
        iIn += width;
        oOut += width;
        y++;
    }
}

static int to_int8(int v) {
    return v >= 128 ? v - 256 : v;
}

static int color_transform_delta(int colorPred, int color) {
    return (colorPred * color) >> 5;
}

static void transform_color_inverse(const int m[3], const uint32_t* src,
                                    int srcPos, int num, uint32_t* dst,
                                    int dstPos) {
    int i;
    for (i = 0; i < num; i++) {
        uint32_t argb = src[srcPos + i];
        int green = to_int8((int)((argb >> 8) & 0xFF));
        uint32_t newRed = (argb >> 16) & 0xFF;
        uint32_t newBlue = argb & 0xFF;
        newRed = (newRed + (uint32_t)color_transform_delta(m[0], green)) &
                 0xFF;
        newBlue = (newBlue + (uint32_t)color_transform_delta(m[1], green)) &
                  0xFF;
        newBlue = (newBlue + (uint32_t)color_transform_delta(
                                m[2], to_int8((int)newRed))) &
                  0xFF;
        dst[dstPos + i] = (argb & 0xFF00FF00u) | (newRed << 16) | newBlue;
    }
}

static void color_space_inverse(const Transform* t, const uint32_t* in,
                                uint32_t* out, int rows) {
    int width = t->xsize;
    int tileWidth = 1 << t->bits;
    int mask = tileWidth - 1;
    int safeWidth = width & ~mask;
    int remainingWidth = width - safeWidth;
    int tilesPerRow = sub_sample_size(width, t->bits);
    const uint32_t* tdata = t->data;
    int predRow = 0;
    int y = 0;
    while (y < rows) {
        int m[3];
        int predIdx = predRow;
        int srcPos = y * width;
        int dstPos = y * width;
        int safeEnd = srcPos + safeWidth;
        int endPos = srcPos + width;
        tasks_yield_check();
        while (srcPos < safeEnd) {
            uint32_t code = tdata[predIdx];
            predIdx++;
            m[0] = to_int8((int)(code & 0xFF));
            m[1] = to_int8((int)((code >> 8) & 0xFF));
            m[2] = to_int8((int)((code >> 16) & 0xFF));
            transform_color_inverse(m, in, srcPos, tileWidth, out, dstPos);
            srcPos += tileWidth;
            dstPos += tileWidth;
        }
        if (srcPos < endPos) {
            uint32_t code = tdata[predIdx];
            m[0] = to_int8((int)(code & 0xFF));
            m[1] = to_int8((int)((code >> 8) & 0xFF));
            m[2] = to_int8((int)((code >> 16) & 0xFF));
            transform_color_inverse(m, in, srcPos, remainingWidth, out, dstPos);
        }
        y++;
        if ((y & mask) == 0) predRow += tilesPerRow;
    }
}

static void add_green_to_blue_and_red(const uint32_t* src, int num,
                                      uint32_t* dst) {
    int i;
    for (i = 0; i < num; i++) {
        uint32_t argb = src[i];
        uint32_t green = (argb >> 8) & 0xFF;
        uint32_t redBlue =
            (argb & 0x00FF00FFu) + ((green << 16) | green);
        redBlue &= 0x00FF00FFu;
        dst[i] = (argb & 0xFF00FF00u) | redBlue;
    }
}

static void color_index_inverse(const Transform* t, const uint32_t* in,
                                uint32_t* out, int rows) {
    int bitsPerPixel = 8 >> t->bits;
    int width = t->xsize;
    const uint32_t* colorMap = t->data;
    if (bitsPerPixel < 8) {
        int ppb = 1 << t->bits;
        uint32_t bitMask = ((uint32_t)1 << bitsPerPixel) - 1;
        int idx = 0;
        int y;
        for (y = 0; y < rows; y++) {
            int x = 0;
            int oBase = y * width;
            tasks_yield_check();
            while (x + ppb <= width) {
                uint32_t packed = (in[idx] >> 8) & 0xFF;
                int p;
                idx++;
                for (p = 0; p < ppb; p++) {
                    out[oBase + x] = colorMap[packed & bitMask];
                    packed >>= bitsPerPixel;
                    x++;
                }
            }
            if (x < width) {
                uint32_t packed = (in[idx] >> 8) & 0xFF;
                idx++;
                while (x < width) {
                    out[oBase + x] = colorMap[packed & bitMask];
                    packed >>= bitsPerPixel;
                    x++;
                }
            }
        }
    } else {
        int total = rows * width;
        int i;
        for (i = 0; i < total; i++) {
            tasks_yield_check();
            out[i] = colorMap[(in[i] >> 8) & 0xFF];
        }
    }
}

static uint32_t* expand_color_map(int numColors, const Transform* t) {
    int finalNum = 1 << (8 >> t->bits);
    const uint32_t* oldData = t->data;
    uint32_t* newData =
        (uint32_t*)pluto_calloc((size_t)finalNum, sizeof(uint32_t));
    int i;
    if (!newData) return NULL;
    newData[0] = oldData[0];
    for (i = 1; i < numColors; i++) {
        uint32_t prev = newData[i - 1];
        uint32_t cur = oldData[i];
        uint32_t a = ((prev >> 24) & 0xFF) + ((cur >> 24) & 0xFF);
        uint32_t r = ((prev >> 16) & 0xFF) + ((cur >> 16) & 0xFF);
        uint32_t g = ((prev >> 8) & 0xFF) + ((cur >> 8) & 0xFF);
        uint32_t b = (prev & 0xFF) + (cur & 0xFF);
        newData[i] = ((a & 0xFF) << 24) | ((r & 0xFF) << 16) |
                     ((g & 0xFF) << 8) | (b & 0xFF);
    }
    return newData;
}

/* ------------------------------------------------------------------ */
/* DecodeImageStream: transforms, color cache, Huffman codes, pixels.  */

static int decode_image_stream(int xsize, int ysize, int isLevel0, Br* br,
                               VP8LCtx* ctx, uint32_t** outData) {
    int transformXsize = xsize;
    int transformYsize = ysize;
    Transform transforms[4];
    int numTransforms = 0;
    unsigned transformsSeen = 0;
    int colorCacheBits = 0;

    if (isLevel0) {
        while (br_read_bits(br, 1) == 1) {
            int type_ = br_read_bits(br, 2);
            Transform t;
            memset(&t, 0, sizeof(t));
            if (numTransforms >= 4 || (transformsSeen & (1u << type_)))
                return 0;
            transformsSeen |= 1u << type_;
            t.type = type_;
            t.xsize = transformXsize;
            t.ysize = transformYsize;
            if (type_ == PREDICTOR_TRANSFORM ||
                type_ == CROSS_COLOR_TRANSFORM) {
                VP8LCtx subCtx;
                uint32_t* sub = NULL;
                int ok;
                memset(&subCtx, 0, sizeof(subCtx));
                t.bits =
                    MIN_TRANSFORM_BITS + br_read_bits(br, NUM_TRANSFORM_BITS);
                ok = decode_image_stream(sub_sample_size(t.xsize, t.bits),
                                         sub_sample_size(t.ysize, t.bits), 0,
                                         br, &subCtx, &sub);
                ctx_free(&subCtx);
                if (!ok) {
                    pluto_free(sub);
                    return 0;
                }
                t.data = sub;
            } else if (type_ == COLOR_INDEXING_TRANSFORM) {
                VP8LCtx subCtx;
                uint32_t* raw = NULL;
                uint32_t* expanded;
                int numColors = br_read_bits(br, 8) + 1;
                int bits = (numColors > 16)   ? 0
                           : (numColors > 4)  ? 1
                           : (numColors > 2)  ? 2
                                              : 3;
                memset(&subCtx, 0, sizeof(subCtx));
                t.bits = bits;
                transformXsize = sub_sample_size(transformXsize, bits);
                if (!decode_image_stream(numColors, 1, 0, br, &subCtx,
                                         &raw)) {
                    pluto_free(raw);
                    ctx_free(&subCtx);
                    return 0;
                }
                ctx_free(&subCtx);
                t.data = raw;
                expanded = expand_color_map(numColors, &t);
                pluto_free(raw);
                t.data = expanded;
                if (!expanded) return 0;
            } else if (type_ != SUBTRACT_GREEN_TRANSFORM) {
                return 0;
            }
            transforms[numTransforms++] = t;
        }
    }

    if (br_read_bits(br, 1) == 1) {
        colorCacheBits = br_read_bits(br, 4);
        if (colorCacheBits < 1 || colorCacheBits > MAX_CACHE_BITS) return 0;
    }

    if (!read_huffman_codes(br, transformXsize, transformYsize,
                            colorCacheBits, isLevel0, ctx)) {
        return 0;
    }

    if (colorCacheBits > 0) {
        ctx->colorCacheSize = 1 << colorCacheBits;
        ctx->colorCache = new_color_cache(colorCacheBits);
        if (!ctx->colorCache) return 0;
    } else {
        ctx->colorCacheSize = 0;
        ctx->colorCache = NULL;
    }
    ctx->transformXsize = transformXsize;
    ctx->transformYsize = transformYsize;
    ctx->huffmanXsize =
        sub_sample_size(transformXsize, ctx->huffmanSubsampleBits);

    if (isLevel0) {
        int i;
        for (i = 0; i < numTransforms; i++)
            ctx->transforms[ctx->numTransforms++] = transforms[i];
        return 1;
    }

    {
        int totalSize = transformXsize * transformYsize;
        uint32_t* data =
            (uint32_t*)pluto_calloc((size_t)totalSize, sizeof(uint32_t));
        if (!data) return 0;
        if (!decode_image_data(br, ctx, data, transformXsize,
                               transformYsize)) {
            pluto_free(data);
            return 0;
        }
        if (br->eos) {
            pluto_free(data);
            return 0;
        }
        *outData = data;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Top-level decode                                                    */

static uint32_t* apply_inverse_transforms(VP8LCtx* ctx, uint32_t* data,
                                          int rows) {
    uint32_t* in_ = data;
    int i;
    if (ctx->numTransforms == 0) return data;
    for (i = ctx->numTransforms - 1; i >= 0; i--) {
        const Transform* t = &ctx->transforms[i];
        uint32_t* out =
            (uint32_t*)pluto_calloc((size_t)t->xsize * (size_t)rows,
                                    sizeof(uint32_t));
        if (!out) {
            pluto_free(in_);
            return NULL;
        }
        if (t->type == COLOR_INDEXING_TRANSFORM) {
            color_index_inverse(t, in_, out, rows);
        } else if (t->type == SUBTRACT_GREEN_TRANSFORM) {
            add_green_to_blue_and_red(in_, t->xsize * rows, out);
        } else if (t->type == PREDICTOR_TRANSFORM) {
            predictor_inverse(t, in_, out, rows);
        } else {
            color_space_inverse(t, in_, out, rows);
        }
        pluto_free(in_);
        in_ = out;
    }
    return in_;
}

/* ------------------------------------------------------------------ */
/* P22: ALPH chunk (alpha plane for lossy images)                      */

static void alpha_unfilter(uint8_t* alpha, int width, int height,
                           int filter) {
    int y;
    for (y = 0; y < height; y++) {
        int o = y * width;
        if (y == 0 || filter == 1) {
            /* Horizontal: prediction carries across rows when filter==1. */
            int pred = (y == 0) ? 0 : alpha[o - width];
            int x;
            for (x = 0; x < width; x++) {
                int v = (pred + alpha[o + x]) & 0xFF;
                alpha[o + x] = (uint8_t)v;
                pred = v;
            }
        } else if (filter == 2) { /* Vertical */
            int prev = o - width;
            int x;
            for (x = 0; x < width; x++)
                alpha[o + x] =
                    (uint8_t)((alpha[prev + x] + alpha[o + x]) & 0xFF);
        } else if (filter == 3) { /* Gradient */
            int prev = o - width;
            int top = alpha[prev];
            int topLeft = top;
            int left = top;
            int x;
            for (x = 0; x < width; x++) {
                int g;
                top = alpha[prev + x];
                g = left + top - topLeft;
                if ((g & ~0xff) != 0) g = (g < 0) ? 0 : 255;
                left = (alpha[o + x] + g) & 0xFF;
                topLeft = top;
                alpha[o + x] = (uint8_t)left;
            }
        }
    }
}

/* Returns a malloc'd width*height alpha plane, or NULL. A NULL payload
 * (no ALPH chunk) yields NULL: the image is opaque. */
static uint8_t* alpha_decode_plane(const uint8_t* payload, size_t plen,
                                   int width, int height) {
    int h0, method, filter, total, i;
    uint8_t* alpha;
    if (!payload || plen < 2) return NULL;
    h0 = payload[0];
    method = h0 & 0x03;
    filter = (h0 >> 2) & 0x03;
    if (method > 1 || ((h0 >> 4) & 0x03) > 1 || ((h0 >> 6) & 0x03) != 0)
        return NULL;
    total = width * height;
    if (total < 1 || total > WEBP_MAX_PIXELS) return NULL;

    if (method == 0) {
        if ((size_t)(plen - 1) < (size_t)total) return NULL;
        alpha = (uint8_t*)pluto_malloc((size_t)total);
        if (!alpha) return NULL;
        memcpy(alpha, payload + 1, (size_t)total);
    } else {
        Br br;
        VP8LCtx ctx;
        uint32_t* flat;
        uint32_t* argb;
        int tw, th;
        memset(&ctx, 0, sizeof(ctx));
        br.data = payload;
        br.len = plen;
        br.pos = 1; /* bitstream starts right after the ALPH header byte */
        br.window = 0;
        br.nbits = 0;
        br.eos = 0;
        if (!decode_image_stream(width, height, 1, &br, &ctx, NULL)) {
            ctx_free(&ctx);
            return NULL;
        }
        tw = ctx.transformXsize;
        th = ctx.transformYsize;
        if (tw < 1 || th < 1 || tw * th > WEBP_MAX_PIXELS) {
            ctx_free(&ctx);
            return NULL;
        }
        flat = (uint32_t*)pluto_calloc((size_t)tw * (size_t)th,
                                       sizeof(uint32_t));
        if (!flat) {
            ctx_free(&ctx);
            return NULL;
        }
        if (!decode_image_data(&br, &ctx, flat, tw, th)) {
            pluto_free(flat);
            ctx_free(&ctx);
            return NULL;
        }
        if (br.eos) {
            pluto_free(flat);
            ctx_free(&ctx);
            return NULL;
        }
        argb = apply_inverse_transforms(&ctx, flat, th);
        flat = NULL; /* consumed (freed or returned) */
        ctx_free(&ctx);
        if (!argb) return NULL;
        alpha = (uint8_t*)pluto_malloc((size_t)total);
        if (!alpha) {
            pluto_free(argb);
            return NULL;
        }
        for (i = 0; i < total; i++)
            alpha[i] = (uint8_t)((argb[i] >> 8) & 0xFF);
        pluto_free(argb);
    }
    if (filter != 0) alpha_unfilter(alpha, width, height, filter);
    return alpha;
}

/* ------------------------------------------------------------------ */
/* P22: VP8 lossy decoder (boolean decoder, headers, transforms, intra */
/* predictions, loop filter) byte-compatible with `dwebp -ppm`.        */

#define VP8_BPS   32 /* working-buffer row stride */
#define VP8_YBASE 64 /* top-left of the luma block inside the buffer */
#define VP8_UBASE 32 /* chroma block offset (stride 32, 1px border) */
#define VP8_VBASE 32

#define VP8_DC_PRED 0
#define VP8_TM_PRED 1
#define VP8_V_PRED  2
#define VP8_H_PRED  3

static const int kFilterExtraRows[3] = { 0, 2, 8 };

/* Persistent working buffers, zero-initialized once (mirrors the file-
 * level locals in webp.lua that survive across decodes). */
static uint8_t s_yArr[640];
/* Lua tables accept negative keys; the ported working-buffer math dips
 * to index -4 on the chroma planes during column shifts, so both arrays
 * get 4 bytes of lead-in padding reached through an offset pointer. */
static uint8_t s_uStorage[324];
static uint8_t s_vStorage[324];
#define s_uArr (s_uStorage + 4)
#define s_vArr (s_vStorage + 4)

static int s_vp8Log2[256];
static int s_vp8Log2Init = 0;

static void vp8_init_log2(void) {
    int i;
    if (s_vp8Log2Init) return;
    for (i = 1; i <= 255; i++) {
        int v = i, n = 0;
        while (v > 1) {
            v >>= 1;
            n++;
        }
        s_vp8Log2[i] = n;
    }
    s_vp8Log2Init = 1;
}

static int vp8_clip8(int v) {
    if ((v & ~0xff) == 0) return v;
    return (v < 0) ? 0 : 255;
}

static int vp8_ksclip1(int v) {
    if (v < -128) return -128;
    if (v > 127) return 127;
    return v;
}

static int vp8_ksclip2(int v) {
    if (v < -16) return -16;
    if (v > 15) return 15;
    return v;
}

static int vp8_kabs0(int v) { return (v < 0) ? -v : v; }

/* Boolean decoder (BITS=24). The cursor p is 1-based like the Lua
 * original; reads outside [1..limit] yield 0 (`or 0` semantics).
 * p/end1/max1 are signed long long so max1 (= end-3) cannot underflow
 * on tiny partitions. */
typedef struct {
    const uint8_t* data;
    long long limit;
    long long p, end1, max1;
    uint32_t value;
    int range;
    int bits;
    int eof;
} Vp8Br;

static unsigned int vp8_byte(const Vp8Br* br, long long idx) {
    if (idx < 1 || idx > br->limit) return 0;
    return br->data[idx - 1];
}

static void vp8_load_new(Vp8Br* br) {
    if (br->p < br->max1) {
        unsigned int b0 = vp8_byte(br, br->p);
        unsigned int b1 = vp8_byte(br, br->p + 1);
        unsigned int b2 = vp8_byte(br, br->p + 2);
        br->value = (br->value << 24) | (b0 << 16) | (b1 << 8) | b2;
        br->bits += 24;
        br->p += 3;
    } else if (br->p < br->end1) {
        br->value = (br->value << 8) | vp8_byte(br, br->p);
        br->bits += 8;
        br->p += 1;
    } else if (br->eof == 0) {
        br->value <<= 8;
        br->bits += 8;
        br->eof = 1;
    } else {
        br->bits = 0;
    }
}

static void vp8_new_br(Vp8Br* br, const uint8_t* data, size_t limit,
                       long long start, long long size) {
    br->data = data;
    br->limit = (long long)limit;
    br->p = start;
    br->end1 = start + size;
    br->max1 = start + size - 3;
    br->range = 254;
    br->value = 0;
    br->bits = -8;
    br->eof = 0;
    vp8_load_new(br);
}

static int vp8_get_bit(Vp8Br* br, int prob) {
    int pos, range, split, bit, shift;
    unsigned int value;
    if (br->bits < 0) vp8_load_new(br);
    pos = br->bits;
    range = br->range;
    split = (range * prob) >> 8;
    value = br->value >> pos;
    if (value > (unsigned int)split) {
        bit = 1;
        range -= split;
        br->value -= ((unsigned int)split + 1u) << pos;
    } else {
        bit = 0;
        range = split + 1;
    }
    shift = 7 - s_vp8Log2[range];
    range <<= shift;
    br->bits = pos - shift;
    br->range = range - 1;
    return bit;
}

static int vp8_get_signed(Vp8Br* br, int v) {
    int pos, split, mask;
    unsigned int value;
    if (br->bits < 0) vp8_load_new(br);
    pos = br->bits;
    split = br->range >> 1;
    value = br->value >> pos;
    mask = (value > (unsigned int)split) ? -1 : 0;
    br->bits = pos - 1;
    br->range = (br->range + mask) | 1;
    br->value -= (((unsigned int)split + 1u) & (unsigned int)mask) << pos;
    return (v ^ mask) - mask;
}

static int vp8_get_value(Vp8Br* br, int bits) {
    int v = 0, i;
    for (i = bits - 1; i >= 0; i--)
        v |= vp8_get_bit(br, 128) << i;
    return v;
}

static int vp8_get_signed_value(Vp8Br* br, int bits) {
    int value = vp8_get_value(br, bits);
    if (vp8_get_bit(br, 128) != 0) return -value;
    return value;
}

/* ------------------------------------------------------------------ */
/* VP8 header parsing                                                  */

typedef struct {
    int useSegment, updateMap, absoluteDelta;
    int quantizer[4];
    int filterStrength[4];
} Vp8SegmentHdr;

typedef struct {
    int simple, level, sharpness, useLfDelta;
    int refLfDelta[4], modeLfDelta[4];
} Vp8FilterHdr;

typedef struct {
    int fLimit, fIlevel, fInner, hevThresh;
} Vp8FStrength;

typedef struct {
    int nz, nzDc;
} Vp8MbInfo;

typedef struct {
    int segment, skip, isI4x4;
    int imodes[16];
    int uvMode;
    int coeffs[384];
    unsigned int nonZeroY, nonZeroUv;
    int dither;
    int fLimit, fIlevel, hevThresh, fInner;
} Vp8Block;

typedef struct {
    int y[16];
    int u[8];
    int v[8];
} Vp8TopYuv;

typedef struct {
    int y1[2]; /* DC, AC */
    int y2[2];
    int uv[2];
} Vp8Quant;

typedef struct {
    int width, height, mbW, mbH;
    int filterType;
    Vp8SegmentHdr segmentHdr;
    Vp8FilterHdr filterHdr;
    int probaSegments[3];
    uint8_t proba[1056];
    Vp8Quant dqm[4];
    Vp8FStrength fstrengths[4][2];
    int useSkipProba, skipP;
    Vp8Br br;
    int numPartsMinusOne;
    Vp8Br parts[16];
    int extra, extraUV;
    int cacheYStride, cacheUvStride;
    uint8_t* cacheY; /* (extra+16)*yBps */
    uint8_t* cacheU; /* (extraUV+8)*uvBps */
    uint8_t* cacheV;
    uint8_t* rgb; /* width*height*3 */
    uint8_t* tmpY; /* width */
    uint8_t* tmpU; /* uvWidth */
    uint8_t* tmpV;
    Vp8Block* mbData;   /* [mbW] */
    Vp8MbInfo* mbInfo;  /* [mbW+1]: [0]=left col, [mbX+1]=current col */
    int* intraT;        /* [4*mbW] */
    int intraL[4];
    Vp8TopYuv* yuvT;    /* [mbW+1] */
    int mbX, mbY;
} Vp8Dec;

static int vp8_parse_segment_header(Vp8Br* br, Vp8Dec* dec) {
    Vp8SegmentHdr* hdr = &dec->segmentHdr;
    int s;
    hdr->useSegment = vp8_get_bit(br, 128);
    if (hdr->useSegment != 0) {
        hdr->updateMap = vp8_get_bit(br, 128);
        if (vp8_get_bit(br, 128) != 0) {
            hdr->absoluteDelta = vp8_get_bit(br, 128);
            for (s = 0; s < 4; s++)
                hdr->quantizer[s] =
                    (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 7) : 0;
            for (s = 0; s < 4; s++)
                hdr->filterStrength[s] =
                    (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 6) : 0;
        }
        if (hdr->updateMap != 0) {
            for (s = 0; s < 3; s++)
                dec->probaSegments[s] =
                    (vp8_get_bit(br, 128) != 0) ? vp8_get_value(br, 8) : 255;
        }
    } else {
        hdr->updateMap = 0;
    }
    return br->eof == 0;
}

static int vp8_parse_filter_header(Vp8Br* br, Vp8Dec* dec) {
    Vp8FilterHdr* hdr = &dec->filterHdr;
    int i;
    hdr->simple = vp8_get_bit(br, 128);
    hdr->level = vp8_get_value(br, 6);
    hdr->sharpness = vp8_get_value(br, 3);
    hdr->useLfDelta = vp8_get_bit(br, 128);
    if (hdr->useLfDelta != 0) {
        if (vp8_get_bit(br, 128) != 0) {
            for (i = 0; i < 4; i++)
                if (vp8_get_bit(br, 128) != 0)
                    hdr->refLfDelta[i] = vp8_get_signed_value(br, 6);
            for (i = 0; i < 4; i++)
                if (vp8_get_bit(br, 128) != 0)
                    hdr->modeLfDelta[i] = vp8_get_signed_value(br, 6);
        }
    }
    if (hdr->level == 0)
        dec->filterType = 0;
    else if (hdr->simple != 0)
        dec->filterType = 1;
    else
        dec->filterType = 2;
    return br->eof == 0;
}

static int vp8_clip_q(int v, int m) {
    if (v < 0) return 0;
    if (v > m) return m;
    return v;
}

static void vp8_parse_quant(Vp8Br* br, Vp8Dec* dec) {
    const Vp8SegmentHdr* hdr = &dec->segmentHdr;
    int baseQ0 = vp8_get_value(br, 7);
    int dqy1dc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dqy2dc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dqy2ac = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dquvdc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dquvac = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int i;
    for (i = 0; i < 4; i++) {
        int q, t;
        if (hdr->useSegment == 0 && i > 0) {
            dec->dqm[i] = dec->dqm[0];
            continue;
        }
        if (hdr->useSegment != 0) {
            q = hdr->quantizer[i];
            if (hdr->absoluteDelta == 0) q += baseQ0;
        } else {
            q = baseQ0;
        }
        dec->dqm[i].y1[0] = kDcTable[vp8_clip_q(q + dqy1dc, 127)];
        dec->dqm[i].y1[1] = kAcTable[vp8_clip_q(q, 127)];
        dec->dqm[i].y2[0] = kDcTable[vp8_clip_q(q + dqy2dc, 127)] * 2;
        t = kAcTable[vp8_clip_q(q + dqy2ac, 127)];
        dec->dqm[i].y2[1] = (t * 101581) >> 16;
        if (dec->dqm[i].y2[1] < 8) dec->dqm[i].y2[1] = 8;
        dec->dqm[i].uv[0] = kDcTable[vp8_clip_q(q + dquvdc, 117)];
        dec->dqm[i].uv[1] = kAcTable[vp8_clip_q(q + dquvac, 127)];
    }
}

static void vp8_parse_proba(Vp8Br* br, Vp8Dec* dec) {
    int t, b, c, p, ci = 0;
    for (t = 0; t < 4; t++) {
        for (b = 0; b < 8; b++) {
            for (c = 0; c < 3; c++) {
                for (p = 0; p <= 10; p++, ci++) {
                    if (vp8_get_bit(br, CoeffsUpdateProba[ci]) != 0)
                        dec->proba[ci] = vp8_get_value(br, 8);
                    else
                        dec->proba[ci] = CoeffsProba0[ci];
                }
            }
        }
    }
    dec->useSkipProba = vp8_get_bit(br, 128);
    if (dec->useSkipProba != 0) dec->skipP = vp8_get_value(br, 8);
}

/* ------------------------------------------------------------------ */
/* VP8 transforms                                                      */

static int vp8_sar(int v, int n) {
    return (v >= 0) ? (v >> n) : ~(~v >> n);
}

static int vp8_mul1(int a) { return vp8_sar(a * 20091, 16) + a; }

static int vp8_mul2(int a) { return vp8_sar(a * 35468, 16); }

static void vp8_transform_one(const int* in_, int inOff, uint8_t* buf,
                              int dst) {
    int t[16];
    int i;
    for (i = 0; i < 4; i++) {
        int a = in_[inOff + i] + in_[inOff + 8 + i];
        int b = in_[inOff + i] - in_[inOff + 8 + i];
        int c = vp8_mul2(in_[inOff + 4 + i]) - vp8_mul1(in_[inOff + 12 + i]);
        int d = vp8_mul1(in_[inOff + 4 + i]) + vp8_mul2(in_[inOff + 12 + i]);
        t[i * 4] = a + d;
        t[i * 4 + 1] = b + c;
        t[i * 4 + 2] = b - c;
        t[i * 4 + 3] = a - d;
    }
    for (i = 0; i < 4; i++) {
        int o = dst + i * VP8_BPS;
        int dc = t[i] + 4;
        int a = dc + t[i + 8];
        int b = dc - t[i + 8];
        int c = vp8_mul2(t[i + 4]) - vp8_mul1(t[i + 12]);
        int d = vp8_mul1(t[i + 4]) + vp8_mul2(t[i + 12]);
        buf[o]     = (uint8_t)vp8_clip8(buf[o]     + vp8_sar(a + d, 3));
        buf[o + 1] = (uint8_t)vp8_clip8(buf[o + 1] + vp8_sar(b + c, 3));
        buf[o + 2] = (uint8_t)vp8_clip8(buf[o + 2] + vp8_sar(b - c, 3));
        buf[o + 3] = (uint8_t)vp8_clip8(buf[o + 3] + vp8_sar(a - d, 3));
    }
}

static void vp8_transform_ac3(const int* in_, int inOff, uint8_t* buf,
                              int dst) {
    int a = in_[inOff] + 4;
    int c4 = vp8_mul2(in_[inOff + 4]);
    int d4 = vp8_mul1(in_[inOff + 4]);
    int c1 = vp8_mul2(in_[inOff + 1]);
    int d1 = vp8_mul1(in_[inOff + 1]);
    int y;
    for (y = 0; y < 4; y++) {
        int dc;
        int o;
        switch (y) {
            case 0: dc = a + d4; break;
            case 1: dc = a + c4; break;
            case 2: dc = a - c4; break;
            default: dc = a - d4; break;
        }
        o = dst + y * VP8_BPS;
        buf[o]     = (uint8_t)vp8_clip8(buf[o]     + vp8_sar(dc + d1, 3));
        buf[o + 1] = (uint8_t)vp8_clip8(buf[o + 1] + vp8_sar(dc + c1, 3));
        buf[o + 2] = (uint8_t)vp8_clip8(buf[o + 2] + vp8_sar(dc - c1, 3));
        buf[o + 3] = (uint8_t)vp8_clip8(buf[o + 3] + vp8_sar(dc - d1, 3));
    }
}

static void vp8_transform_dc(const int* in_, int inOff, uint8_t* buf,
                             int dst) {
    int v = vp8_sar(in_[inOff] + 4, 3);
    int j;
    for (j = 0; j < 4; j++) {
        int o = dst + j * VP8_BPS;
        int i;
        for (i = 0; i < 4; i++)
            buf[o + i] = (uint8_t)vp8_clip8(buf[o + i] + v);
    }
}

static void vp8_transform_uv(const int* in_, int inOff, uint8_t* buf,
                             int dst) {
    vp8_transform_one(in_, inOff, buf, dst);
    vp8_transform_one(in_, inOff + 16, buf, dst + 4);
    vp8_transform_one(in_, inOff + 32, buf, dst + 4 * VP8_BPS);
    vp8_transform_one(in_, inOff + 48, buf, dst + 4 * VP8_BPS + 4);
}

static void vp8_transform_dcuv(const int* in_, int inOff, uint8_t* buf,
                               int dst) {
    if (in_[inOff] != 0)      vp8_transform_dc(in_, inOff, buf, dst);
    if (in_[inOff + 16] != 0) vp8_transform_dc(in_, inOff + 16, buf, dst + 4);
    if (in_[inOff + 32] != 0)
        vp8_transform_dc(in_, inOff + 32, buf, dst + 4 * VP8_BPS);
    if (in_[inOff + 48] != 0)
        vp8_transform_dc(in_, inOff + 48, buf, dst + 4 * VP8_BPS + 4);
}

static void vp8_transform_wht(const int* in_, int inOff, int* out,
                              int outOff) {
    int t[16];
    int i;
    for (i = 0; i < 4; i++) {
        int a0 = in_[inOff + i] + in_[inOff + 12 + i];
        int a1 = in_[inOff + 4 + i] + in_[inOff + 8 + i];
        int a2 = in_[inOff + 4 + i] - in_[inOff + 8 + i];
        int a3 = in_[inOff + i] - in_[inOff + 12 + i];
        t[i] = a0 + a1;
        t[8 + i] = a0 - a1;
        t[4 + i] = a3 + a2;
        t[12 + i] = a3 - a2;
    }
    for (i = 0; i < 4; i++) {
        int dc = t[i * 4] + 3;
        int a0 = dc + t[i * 4 + 3];
        int a1 = t[i * 4 + 1] + t[i * 4 + 2];
        int a2 = t[i * 4 + 1] - t[i * 4 + 2];
        int a3 = dc - t[i * 4 + 3];
        out[outOff]      = vp8_sar(a0 + a1, 3);
        out[outOff + 16] = vp8_sar(a3 + a2, 3);
        out[outOff + 32] = vp8_sar(a0 - a1, 3);
        out[outOff + 48] = vp8_sar(a3 - a2, 3);
        outOff += 64;
    }
}

static void vp8_do_transform(unsigned int bits, const int* coeffs, int inOff,
                             uint8_t* buf, int dst) {
    switch ((int)(bits >> 30)) {
        case 3: vp8_transform_one(coeffs, inOff, buf, dst); break;
        case 2: vp8_transform_ac3(coeffs, inOff, buf, dst); break;
        case 1: vp8_transform_dc(coeffs, inOff, buf, dst); break;
        default: break;
    }
}

static void vp8_do_uv_transform(unsigned int bits, const int* coeffs,
                                int inOff, uint8_t* buf, int dst) {
    if ((bits & 0xffu) != 0) {
        if ((bits & 0xaau) != 0)
            vp8_transform_uv(coeffs, inOff, buf, dst);
        else
            vp8_transform_dcuv(coeffs, inOff, buf, dst);
    }
}

/* ------------------------------------------------------------------ */
/* VP8 intra predictions (stride 32)                                   */

static int vp8_avg2(int a, int b) { return (a + b + 1) >> 1; }

static int vp8_avg3(int a, int b, int c) {
    return (a + 2 * b + c + 2) >> 2;
}

static void vp8_true_motion(uint8_t* buf, int dst, int size) {
    int topBase = dst - VP8_BPS;
    int topleft = buf[topBase - 1];
    int y, x;
    for (y = 0; y < size; y++) {
        int left = buf[dst - 1 + y * VP8_BPS];
        int r = dst + y * VP8_BPS;
        for (x = 0; x < size; x++)
            buf[r + x] =
                (uint8_t)vp8_clip8(buf[topBase + x] + left - topleft);
    }
}

static void vp8_fill_const(uint8_t* buf, int dst, int size, int v) {
    int j, i;
    for (j = 0; j < size; j++) {
        int r = dst + j * VP8_BPS;
        for (i = 0; i < size; i++) buf[r + i] = (uint8_t)v;
    }
}

static void vp8_dc16(uint8_t* buf, int dst) {
    int dc = 16, j;
    for (j = 0; j < 16; j++)
        dc += buf[dst - 1 + j * VP8_BPS] + buf[dst - VP8_BPS + j];
    vp8_fill_const(buf, dst, 16, dc >> 5);
}

static void vp8_dc16_no_top(uint8_t* buf, int dst) {
    int dc = 8, j;
    for (j = 0; j < 16; j++) dc += buf[dst - 1 + j * VP8_BPS];
    vp8_fill_const(buf, dst, 16, dc >> 4);
}

static void vp8_dc16_no_left(uint8_t* buf, int dst) {
    int dc = 8, i;
    for (i = 0; i < 16; i++) dc += buf[dst - VP8_BPS + i];
    vp8_fill_const(buf, dst, 16, dc >> 4);
}

static void vp8_ve16(uint8_t* buf, int dst) {
    int top = dst - VP8_BPS;
    int j, i;
    for (j = 0; j < 16; j++) {
        int r = dst + j * VP8_BPS;
        for (i = 0; i < 16; i++) buf[r + i] = buf[top + i];
    }
}

static void vp8_he16(uint8_t* buf, int dst) {
    int j, i;
    for (j = 0; j < 16; j++) {
        int v = buf[dst - 1 + j * VP8_BPS];
        int r = dst + j * VP8_BPS;
        for (i = 0; i < 16; i++) buf[r + i] = (uint8_t)v;
    }
}

static void vp8_pred_luma16(int mode, uint8_t* buf, int dst) {
    switch (mode) {
        case 0: vp8_dc16(buf, dst); break;
        case 1: vp8_true_motion(buf, dst, 16); break;
        case 2: vp8_ve16(buf, dst); break;
        case 3: vp8_he16(buf, dst); break;
        case 4: vp8_dc16_no_top(buf, dst); break;
        case 5: vp8_dc16_no_left(buf, dst); break;
        default: vp8_fill_const(buf, dst, 16, 128); break;
    }
}

static void vp8_dc8uv(uint8_t* buf, int dst) {
    int dc0 = 8, i;
    for (i = 0; i < 8; i++)
        dc0 += buf[dst - VP8_BPS + i] + buf[dst - 1 + i * VP8_BPS];
    vp8_fill_const(buf, dst, 8, dc0 >> 4);
}

static void vp8_dc8uv_no_top(uint8_t* buf, int dst) {
    int dc0 = 4, i;
    for (i = 0; i < 8; i++) dc0 += buf[dst - 1 + i * VP8_BPS];
    vp8_fill_const(buf, dst, 8, dc0 >> 3);
}

static void vp8_dc8uv_no_left(uint8_t* buf, int dst) {
    int dc0 = 4, i;
    for (i = 0; i < 8; i++) dc0 += buf[dst - VP8_BPS + i];
    vp8_fill_const(buf, dst, 8, dc0 >> 3);
}

static void vp8_ve8uv(uint8_t* buf, int dst) {
    int top = dst - VP8_BPS;
    int j, i;
    for (j = 0; j < 8; j++) {
        int r = dst + j * VP8_BPS;
        for (i = 0; i < 8; i++) buf[r + i] = buf[top + i];
    }
}

static void vp8_he8uv(uint8_t* buf, int dst) {
    int j, i;
    for (j = 0; j < 8; j++) {
        int v = buf[dst - 1 + j * VP8_BPS];
        int r = dst + j * VP8_BPS;
        for (i = 0; i < 8; i++) buf[r + i] = (uint8_t)v;
    }
}

static void vp8_pred_chroma8(int mode, uint8_t* buf, int dst) {
    switch (mode) {
        case 0: vp8_dc8uv(buf, dst); break;
        case 1: vp8_true_motion(buf, dst, 8); break;
        case 2: vp8_ve8uv(buf, dst); break;
        case 3: vp8_he8uv(buf, dst); break;
        case 4: vp8_dc8uv_no_top(buf, dst); break;
        case 5: vp8_dc8uv_no_left(buf, dst); break;
        default: vp8_fill_const(buf, dst, 8, 128); break;
    }
}

static void vp8_dc4(uint8_t* buf, int dst) {
    int dc = 4, i;
    for (i = 0; i < 4; i++)
        dc += buf[dst - VP8_BPS + i] + buf[dst - 1 + i * VP8_BPS];
    vp8_fill_const(buf, dst, 4, dc >> 3);
}

static void vp8_ve4(uint8_t* buf, int dst) {
    int top = dst - VP8_BPS;
    int v0 = vp8_avg3(buf[top - 1], buf[top], buf[top + 1]);
    int v1 = vp8_avg3(buf[top], buf[top + 1], buf[top + 2]);
    int v2 = vp8_avg3(buf[top + 1], buf[top + 2], buf[top + 3]);
    int v3 = vp8_avg3(buf[top + 2], buf[top + 3], buf[top + 4]);
    int j;
    for (j = 0; j < 4; j++) {
        int r = dst + j * VP8_BPS;
        buf[r] = (uint8_t)v0;
        buf[r + 1] = (uint8_t)v1;
        buf[r + 2] = (uint8_t)v2;
        buf[r + 3] = (uint8_t)v3;
    }
}

static void vp8_he4(uint8_t* buf, int dst) {
    int a = buf[dst - 1 - VP8_BPS];
    int b = buf[dst - 1];
    int c = buf[dst - 1 + VP8_BPS];
    int d = buf[dst - 1 + 2 * VP8_BPS];
    int e = buf[dst - 1 + 3 * VP8_BPS];
    int w0 = vp8_avg3(a, b, c);
    int w1 = vp8_avg3(b, c, d);
    int w2 = vp8_avg3(c, d, e);
    int w3 = vp8_avg3(d, e, e);
    int j, i;
    for (j = 0; j < 4; j++) {
        int w;
        switch (j) {
            case 0: w = w0; break;
            case 1: w = w1; break;
            case 2: w = w2; break;
            default: w = w3; break;
        }
        for (i = 0; i < 4; i++)
            buf[dst + j * VP8_BPS + i] = (uint8_t)w;
    }
}

static void vp8_rd4(uint8_t* buf, int dst) {
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    buf[dst + 3 * VP8_BPS] = (uint8_t)vp8_avg3(j, k, l);
    buf[dst + 1 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(i, j, k);
    buf[dst + 0 + 2 * VP8_BPS] = buf[dst + 1 + 3 * VP8_BPS];
    buf[dst + 2 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(x, i, j);
    buf[dst + 1 + 2 * VP8_BPS] = buf[dst + 2 + 3 * VP8_BPS];
    buf[dst + 0 + 1 * VP8_BPS] = buf[dst + 2 + 3 * VP8_BPS];
    buf[dst + 3 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(a, x, i);
    buf[dst + 2 + 2 * VP8_BPS] = buf[dst + 3 + 3 * VP8_BPS];
    buf[dst + 1 + 1 * VP8_BPS] = buf[dst + 3 + 3 * VP8_BPS];
    buf[dst + 0 + 0 * VP8_BPS] = buf[dst + 3 + 3 * VP8_BPS];
    buf[dst + 3 + 2 * VP8_BPS] = (uint8_t)vp8_avg3(b, a, x);
    buf[dst + 2 + 1 * VP8_BPS] = buf[dst + 3 + 2 * VP8_BPS];
    buf[dst + 1 + 0 * VP8_BPS] = buf[dst + 3 + 2 * VP8_BPS];
    buf[dst + 3 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(c, b, a);
    buf[dst + 2 + 0 * VP8_BPS] = buf[dst + 3 + 1 * VP8_BPS];
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(d, c, b);
}

static void vp8_ld4(uint8_t* buf, int dst) {
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    int e = buf[dst - VP8_BPS + 4];
    int f = buf[dst - VP8_BPS + 5];
    int g = buf[dst - VP8_BPS + 6];
    int h = buf[dst - VP8_BPS + 7];
    buf[dst + 0 * VP8_BPS] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + 1 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + 0 + 1 * VP8_BPS] = buf[dst + 1 + 0 * VP8_BPS];
    buf[dst + 2 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + 1 + 1 * VP8_BPS] = buf[dst + 2 + 0 * VP8_BPS];
    buf[dst + 0 + 2 * VP8_BPS] = buf[dst + 2 + 0 * VP8_BPS];
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 2 + 1 * VP8_BPS] = buf[dst + 3 + 0 * VP8_BPS];
    buf[dst + 1 + 2 * VP8_BPS] = buf[dst + 3 + 0 * VP8_BPS];
    buf[dst + 0 + 3 * VP8_BPS] = buf[dst + 3 + 0 * VP8_BPS];
    buf[dst + 3 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 2 + 2 * VP8_BPS] = buf[dst + 3 + 1 * VP8_BPS];
    buf[dst + 1 + 3 * VP8_BPS] = buf[dst + 3 + 1 * VP8_BPS];
    buf[dst + 3 + 2 * VP8_BPS] = (uint8_t)vp8_avg3(f, g, h);
    buf[dst + 2 + 3 * VP8_BPS] = buf[dst + 3 + 2 * VP8_BPS];
    buf[dst + 3 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(g, h, h);
}

static void vp8_vr4(uint8_t* buf, int dst) {
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    buf[dst + 0 * VP8_BPS] = (uint8_t)vp8_avg2(x, a);
    buf[dst + 1 + 2 * VP8_BPS] = buf[dst + 0 * VP8_BPS];
    buf[dst + 1 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(a, b);
    buf[dst + 2 + 2 * VP8_BPS] = buf[dst + 1 + 0 * VP8_BPS];
    buf[dst + 2 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 3 + 2 * VP8_BPS] = buf[dst + 2 + 0 * VP8_BPS];
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(c, d);
    buf[dst + 0 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(k, j, i);
    buf[dst + 0 + 2 * VP8_BPS] = (uint8_t)vp8_avg3(j, i, x);
    buf[dst + 0 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + 1 + 3 * VP8_BPS] = buf[dst + 0 + 1 * VP8_BPS];
    buf[dst + 1 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(x, a, b);
    buf[dst + 2 + 3 * VP8_BPS] = buf[dst + 1 + 1 * VP8_BPS];
    buf[dst + 2 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + 3 + 3 * VP8_BPS] = buf[dst + 2 + 1 * VP8_BPS];
    buf[dst + 3 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(b, c, d);
}

static void vp8_vl4(uint8_t* buf, int dst) {
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    int e = buf[dst - VP8_BPS + 4];
    int f = buf[dst - VP8_BPS + 5];
    int g = buf[dst - VP8_BPS + 6];
    int h = buf[dst - VP8_BPS + 7];
    buf[dst + 0 * VP8_BPS] = (uint8_t)vp8_avg2(a, b);
    buf[dst + 1 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 0 + 2 * VP8_BPS] = buf[dst + 1 + 0 * VP8_BPS];
    buf[dst + 2 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(c, d);
    buf[dst + 1 + 2 * VP8_BPS] = buf[dst + 2 + 0 * VP8_BPS];
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(d, e);
    buf[dst + 2 + 2 * VP8_BPS] = buf[dst + 3 + 0 * VP8_BPS];
    buf[dst + 0 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + 1 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + 0 + 3 * VP8_BPS] = buf[dst + 1 + 1 * VP8_BPS];
    buf[dst + 2 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + 1 + 3 * VP8_BPS] = buf[dst + 2 + 1 * VP8_BPS];
    buf[dst + 3 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 2 + 3 * VP8_BPS] = buf[dst + 3 + 1 * VP8_BPS];
    buf[dst + 3 + 2 * VP8_BPS] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 3 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(f, g, h);
}

static void vp8_hd4(uint8_t* buf, int dst) {
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    buf[dst + 0 * VP8_BPS] = (uint8_t)vp8_avg2(i, x);
    buf[dst + 2 + 1 * VP8_BPS] = buf[dst + 0 * VP8_BPS];
    buf[dst + 0 + 1 * VP8_BPS] = (uint8_t)vp8_avg2(j, i);
    buf[dst + 2 + 2 * VP8_BPS] = buf[dst + 0 + 1 * VP8_BPS];
    buf[dst + 0 + 2 * VP8_BPS] = (uint8_t)vp8_avg2(k, j);
    buf[dst + 2 + 3 * VP8_BPS] = buf[dst + 0 + 2 * VP8_BPS];
    buf[dst + 0 + 3 * VP8_BPS] = (uint8_t)vp8_avg2(l, k);
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + 2 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(x, a, b);
    buf[dst + 1 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + 3 + 1 * VP8_BPS] = buf[dst + 1 + 0 * VP8_BPS];
    buf[dst + 1 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(j, i, x);
    buf[dst + 3 + 2 * VP8_BPS] = buf[dst + 1 + 1 * VP8_BPS];
    buf[dst + 1 + 2 * VP8_BPS] = (uint8_t)vp8_avg3(k, j, i);
    buf[dst + 3 + 3 * VP8_BPS] = buf[dst + 1 + 2 * VP8_BPS];
    buf[dst + 1 + 3 * VP8_BPS] = (uint8_t)vp8_avg3(l, k, j);
}

static void vp8_hu4(uint8_t* buf, int dst) {
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    buf[dst + 0 * VP8_BPS] = (uint8_t)vp8_avg2(i, j);
    buf[dst + 2 + 0 * VP8_BPS] = (uint8_t)vp8_avg2(j, k);
    buf[dst + 0 + 1 * VP8_BPS] = buf[dst + 2 + 0 * VP8_BPS];
    buf[dst + 2 + 1 * VP8_BPS] = (uint8_t)vp8_avg2(k, l);
    buf[dst + 0 + 2 * VP8_BPS] = buf[dst + 2 + 1 * VP8_BPS];
    buf[dst + 1 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(i, j, k);
    buf[dst + 3 + 0 * VP8_BPS] = (uint8_t)vp8_avg3(j, k, l);
    buf[dst + 1 + 1 * VP8_BPS] = buf[dst + 3 + 0 * VP8_BPS];
    buf[dst + 3 + 1 * VP8_BPS] = (uint8_t)vp8_avg3(k, l, l);
    buf[dst + 1 + 2 * VP8_BPS] = buf[dst + 3 + 1 * VP8_BPS];
    buf[dst + 3 + 2 * VP8_BPS] = (uint8_t)l;
    buf[dst + 2 + 2 * VP8_BPS] = (uint8_t)l;
    buf[dst + 0 + 3 * VP8_BPS] = (uint8_t)l;
    buf[dst + 1 + 3 * VP8_BPS] = (uint8_t)l;
    buf[dst + 2 + 3 * VP8_BPS] = (uint8_t)l;
    buf[dst + 3 + 3 * VP8_BPS] = (uint8_t)l;
}

static void vp8_pred_luma4(int mode, uint8_t* buf, int dst) {
    switch (mode) {
        case 0: vp8_dc4(buf, dst); break;
        case 1: vp8_true_motion(buf, dst, 4); break;
        case 2: vp8_ve4(buf, dst); break;
        case 3: vp8_he4(buf, dst); break;
        case 4: vp8_rd4(buf, dst); break;
        case 5: vp8_vr4(buf, dst); break;
        case 6: vp8_ld4(buf, dst); break;
        case 7: vp8_vl4(buf, dst); break;
        case 8: vp8_hd4(buf, dst); break;
        default: vp8_hu4(buf, dst); break;
    }
}

static int vp8_check_mode(int mbX, int mbY, int mode) {
    if (mode == VP8_DC_PRED) {
        if (mbX == 0) {
            if (mbY == 0) return 6;
            return 5;
        }
        if (mbY == 0) return 4;
        return 0;
    }
    return mode;
}

/* ------------------------------------------------------------------ */
/* In-loop filtering                                                   */

static void vp8_do_filter2(uint8_t* buf, int p, int step) {
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int a = 3 * (q0 - p0) + vp8_ksclip1(p1 - q1);
    int a1 = vp8_ksclip2(vp8_sar(a + 4, 3));
    int a2 = vp8_ksclip2(vp8_sar(a + 3, 3));
    buf[p - step] = (uint8_t)vp8_clip8(p0 + a2);
    buf[p] = (uint8_t)vp8_clip8(q0 - a1);
}

static void vp8_do_filter4(uint8_t* buf, int p, int step) {
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int a = 3 * (q0 - p0);
    int a1 = vp8_ksclip2(vp8_sar(a + 4, 3));
    int a2 = vp8_ksclip2(vp8_sar(a + 3, 3));
    int a3 = vp8_sar(a1 + 1, 1);
    buf[p - 2 * step] = (uint8_t)vp8_clip8(p1 + a3);
    buf[p - step] = (uint8_t)vp8_clip8(p0 + a2);
    buf[p] = (uint8_t)vp8_clip8(q0 - a1);
    buf[p + step] = (uint8_t)vp8_clip8(q1 - a3);
}

static void vp8_do_filter6(uint8_t* buf, int p, int step) {
    int p2 = buf[p - 3 * step];
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int q2 = buf[p + 2 * step];
    int a = vp8_ksclip1(3 * (q0 - p0) + vp8_ksclip1(p1 - q1));
    int a1 = vp8_sar(27 * a + 63, 7);
    int a2 = vp8_sar(18 * a + 63, 7);
    int a3 = vp8_sar(9 * a + 63, 7);
    buf[p - 3 * step] = (uint8_t)vp8_clip8(p2 + a3);
    buf[p - 2 * step] = (uint8_t)vp8_clip8(p1 + a2);
    buf[p - step] = (uint8_t)vp8_clip8(p0 + a1);
    buf[p] = (uint8_t)vp8_clip8(q0 - a1);
    buf[p + step] = (uint8_t)vp8_clip8(q1 - a2);
    buf[p + 2 * step] = (uint8_t)vp8_clip8(q2 - a3);
}

static int vp8_hev(const uint8_t* buf, int p, int step, int thresh) {
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    return (vp8_kabs0(p1 - p0) > thresh) || (vp8_kabs0(q1 - q0) > thresh);
}

static int vp8_needs_filter(const uint8_t* buf, int p, int step, int t) {
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    return (4 * vp8_kabs0(p0 - q0) + vp8_kabs0(p1 - q1)) <= t;
}

static int vp8_needs_filter2(const uint8_t* buf, int p, int step, int t,
                             int it) {
    int p3 = buf[p - 4 * step];
    int p2 = buf[p - 3 * step];
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int q2 = buf[p + 2 * step];
    int q3 = buf[p + 3 * step];
    if ((4 * vp8_kabs0(p0 - q0) + vp8_kabs0(p1 - q1)) > t) return 0;
    return vp8_kabs0(p3 - p2) <= it && vp8_kabs0(p2 - p1) <= it &&
           vp8_kabs0(p1 - p0) <= it && vp8_kabs0(q3 - q2) <= it &&
           vp8_kabs0(q2 - q1) <= it && vp8_kabs0(q1 - q0) <= it;
}

static void vp8_filter_loop26(uint8_t* buf, int p, int hstride, int vstride,
                              int size, int thresh, int ithresh,
                              int hevThresh) {
    int t = 2 * thresh + 1;
    int n;
    for (n = 0; n < size; n++) {
        if (vp8_needs_filter2(buf, p, hstride, t, ithresh)) {
            if (vp8_hev(buf, p, hstride, hevThresh))
                vp8_do_filter2(buf, p, hstride);
            else
                vp8_do_filter6(buf, p, hstride);
        }
        p += vstride;
    }
}

static void vp8_filter_loop24(uint8_t* buf, int p, int hstride, int vstride,
                              int size, int thresh, int ithresh,
                              int hevThresh) {
    int t = 2 * thresh + 1;
    int n;
    for (n = 0; n < size; n++) {
        if (vp8_needs_filter2(buf, p, hstride, t, ithresh)) {
            if (vp8_hev(buf, p, hstride, hevThresh))
                vp8_do_filter2(buf, p, hstride);
            else
                vp8_do_filter4(buf, p, hstride);
        }
        p += vstride;
    }
}

static void vp8_vfilter16(uint8_t* buf, int p, int stride, int thresh,
                          int ithresh, int hevThresh) {
    vp8_filter_loop26(buf, p, stride, 1, 16, thresh, ithresh, hevThresh);
}

static void vp8_hfilter16(uint8_t* buf, int p, int stride, int thresh,
                          int ithresh, int hevThresh) {
    vp8_filter_loop26(buf, p, 1, stride, 16, thresh, ithresh, hevThresh);
}

static void vp8_vfilter16i(uint8_t* buf, int p, int stride, int thresh,
                           int ithresh, int hevThresh) {
    int k;
    for (k = 0; k < 3; k++) {
        p += 4 * stride;
        vp8_filter_loop24(buf, p, stride, 1, 16, thresh, ithresh, hevThresh);
    }
}

static void vp8_hfilter16i(uint8_t* buf, int p, int stride, int thresh,
                           int ithresh, int hevThresh) {
    int k;
    for (k = 0; k < 3; k++) {
        p += 4;
        vp8_filter_loop24(buf, p, 1, stride, 16, thresh, ithresh, hevThresh);
    }
}

static void vp8_vfilter8(uint8_t* bufU, int u, uint8_t* bufV, int v,
                         int stride, int thresh, int ithresh, int hevThresh) {
    vp8_filter_loop26(bufU, u, stride, 1, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop26(bufV, v, stride, 1, 8, thresh, ithresh, hevThresh);
}

static void vp8_vfilter8i(uint8_t* bufU, int u, uint8_t* bufV, int v,
                          int stride, int thresh, int ithresh,
                          int hevThresh) {
    vp8_filter_loop24(bufU, u + 4 * stride, stride, 1, 8, thresh, ithresh,
                      hevThresh);
    vp8_filter_loop24(bufV, v + 4 * stride, stride, 1, 8, thresh, ithresh,
                      hevThresh);
}

static void vp8_hfilter8(uint8_t* bufU, int u, uint8_t* bufV, int v,
                         int stride, int thresh, int ithresh, int hevThresh) {
    vp8_filter_loop26(bufU, u, 1, stride, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop26(bufV, v, 1, stride, 8, thresh, ithresh, hevThresh);
}

static void vp8_hfilter8i(uint8_t* bufU, int u, uint8_t* bufV, int v,
                          int stride, int thresh, int ithresh,
                          int hevThresh) {
    vp8_filter_loop24(bufU, u + 4, 1, stride, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop24(bufV, v + 4, 1, stride, 8, thresh, ithresh, hevThresh);
}

static void vp8_simple_vfilter16(uint8_t* buf, int p, int stride, int thresh) {
    int t = 2 * thresh + 1;
    int i;
    for (i = 0; i < 16; i++) {
        int pp = p + i;
        if (vp8_needs_filter(buf, pp, stride, t)) vp8_do_filter2(buf, pp, stride);
    }
}

static void vp8_simple_hfilter16(uint8_t* buf, int p, int stride, int thresh) {
    int t = 2 * thresh + 1;
    int i;
    for (i = 0; i < 16; i++) {
        int pp = p + i * stride;
        if (vp8_needs_filter(buf, pp, 1, t)) vp8_do_filter2(buf, pp, 1);
    }
}

static void vp8_simple_vfilter16i(uint8_t* buf, int p, int stride,
                                  int thresh) {
    int k;
    for (k = 0; k < 3; k++) {
        p += 4 * stride;
        vp8_simple_vfilter16(buf, p, stride, thresh);
    }
}

static void vp8_simple_hfilter16i(uint8_t* buf, int p, int stride,
                                  int thresh) {
    int k;
    for (k = 0; k < 3; k++) {
        p += 4;
        vp8_simple_hfilter16(buf, p, stride, thresh);
    }
}

static void vp8_precompute_filter_strengths(Vp8Dec* dec) {
    const Vp8FilterHdr* hdr = &dec->filterHdr;
    const Vp8SegmentHdr* segHdr = &dec->segmentHdr;
    int s, i4x4;
    if (dec->filterType <= 0) return;
    for (s = 0; s < 4; s++) {
        int baseLevel;
        if (segHdr->useSegment != 0) {
            baseLevel = segHdr->filterStrength[s];
            if (segHdr->absoluteDelta == 0) baseLevel += hdr->level;
        } else {
            baseLevel = hdr->level;
        }
        for (i4x4 = 0; i4x4 <= 1; i4x4++) {
            Vp8FStrength* fs = &dec->fstrengths[s][i4x4];
            int level = baseLevel;
            if (hdr->useLfDelta != 0) {
                level += hdr->refLfDelta[0];
                if (i4x4 == 1) level += hdr->modeLfDelta[0];
            }
            if (level < 0) level = 0;
            if (level > 63) level = 63;
            fs->fLimit = 0;
            fs->fIlevel = 0;
            fs->fInner = i4x4;
            fs->hevThresh = 0;
            if (level > 0) {
                int ilevel = level;
                if (hdr->sharpness > 0) {
                    ilevel >>= (hdr->sharpness > 4) ? 2 : 1;
                    if (ilevel > 9 - hdr->sharpness)
                        ilevel = 9 - hdr->sharpness;
                }
                if (ilevel < 1) ilevel = 1;
                fs->fIlevel = ilevel;
                fs->fLimit = 2 * level + ilevel;
                if (level >= 40)
                    fs->hevThresh = 2;
                else if (level >= 15)
                    fs->hevThresh = 1;
            }
        }
    }
}

static void vp8_do_filter(Vp8Dec* dec, int mbX, int mbY) {
    const Vp8Block* block = &dec->mbData[mbX];
    int yBps = dec->cacheYStride;
    int yDst = dec->extra * yBps + mbX * 16;
    int ilevel = block->fIlevel;
    int limit = block->fLimit;
    if (limit == 0) return;
    if (dec->filterType == 1) {
        if (mbX > 0)
            vp8_simple_hfilter16(dec->cacheY, yDst, yBps, limit + 4);
        if (block->fInner == 1)
            vp8_simple_hfilter16i(dec->cacheY, yDst, yBps, limit);
        if (mbY > 0)
            vp8_simple_vfilter16(dec->cacheY, yDst, yBps, limit + 4);
        if (block->fInner == 1)
            vp8_simple_vfilter16i(dec->cacheY, yDst, yBps, limit);
    } else {
        int uvBps = dec->cacheUvStride;
        int uvDst = dec->extraUV * uvBps + mbX * 8;
        int hevThresh = block->hevThresh;
        if (mbX > 0) {
            vp8_hfilter16(dec->cacheY, yDst, yBps, limit + 4, ilevel,
                          hevThresh);
            vp8_hfilter8(dec->cacheU, uvDst, dec->cacheV, uvDst, uvBps,
                         limit + 4, ilevel, hevThresh);
        }
        if (block->fInner == 1) {
            vp8_hfilter16i(dec->cacheY, yDst, yBps, limit, ilevel, hevThresh);
            vp8_hfilter8i(dec->cacheU, uvDst, dec->cacheV, uvDst, uvBps,
                          limit, ilevel, hevThresh);
        }
        if (mbY > 0) {
            vp8_vfilter16(dec->cacheY, yDst, yBps, limit + 4, ilevel,
                          hevThresh);
            vp8_vfilter8(dec->cacheU, uvDst, dec->cacheV, uvDst, uvBps,
                         limit + 4, ilevel, hevThresh);
        }
        if (block->fInner == 1) {
            vp8_vfilter16i(dec->cacheY, yDst, yBps, limit, ilevel, hevThresh);
            vp8_vfilter8i(dec->cacheU, uvDst, dec->cacheV, uvDst, uvBps,
                          limit, ilevel, hevThresh);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Coefficient decoding                                                */

static int vp8_get_large_value(Vp8Br* br, const uint8_t* proba, int p) {
    int v;
    if (vp8_get_bit(br, proba[p + 3]) == 0) {
        if (vp8_get_bit(br, proba[p + 4]) == 0)
            v = 2;
        else
            v = 3 + vp8_get_bit(br, proba[p + 5]);
    } else if (vp8_get_bit(br, proba[p + 6]) == 0) {
        if (vp8_get_bit(br, proba[p + 7]) == 0) {
            v = 5 + vp8_get_bit(br, 159);
        } else {
            v = 7 + 2 * vp8_get_bit(br, 165);
            v += vp8_get_bit(br, 145);
        }
    } else {
        int bit1 = vp8_get_bit(br, proba[p + 8]);
        int bit0 = vp8_get_bit(br, proba[p + 9 + bit1]);
        int cat = 2 * bit1 + bit0;
        const uint8_t* tab = kCat3456[cat];
        int i;
        v = 0;
        for (i = 0; tab[i] != 0; i++)
            v = v + v + vp8_get_bit(br, tab[i]);
        v += 3 + (8 << cat);
    }
    return v;
}

/* proba is a flat 1056-entry table; dc/ac are this segment's quantizers. */
static int vp8_get_coeffs(Vp8Br* br, const uint8_t* proba, int t, int n,
                          int ctxIdx, int dc, int ac, int* out, int outOff) {
    int p = ((t * 8 + kBands[n]) * 3 + ctxIdx) * 11;
    while (n < 16) {
        int v;
        if (vp8_get_bit(br, proba[p]) == 0) return n;
        while (vp8_get_bit(br, proba[p + 1]) == 0) {
            n++;
            if (n == 16) return 16;
            p = (t * 8 + kBands[n]) * 33;
        }
        if (vp8_get_bit(br, proba[p + 2]) == 0) {
            v = 1;
            p = (t * 8 + kBands[n + 1]) * 33 + 11;
        } else {
            v = vp8_get_large_value(br, proba, p);
            p = (t * 8 + kBands[n + 1]) * 33 + 22;
        }
        out[outOff + kZigzag[n]] =
            vp8_get_signed(br, v) * ((n > 0) ? ac : dc);
        n++;
    }
    return 16;
}

static int vp8_parse_residuals(Vp8Dec* dec, Vp8Br* tokenBr) {
    int mbX = dec->mbX;
    Vp8MbInfo* mb = &dec->mbInfo[mbX + 1];
    Vp8MbInfo* leftMb = &dec->mbInfo[0];
    Vp8Block* block = &dec->mbData[mbX];
    const int* q = dec->dqm[block->segment].y1; /* luma AC quantizer pair */
    const int* qy2 = dec->dqm[block->segment].y2;
    const int* quv = dec->dqm[block->segment].uv;
    int* dst = block->coeffs;
    unsigned int nonZeroY = 0, nonZeroUv = 0;
    int first, outOff = 0;
    int acT, tnz, lnz, outTNz, outLNz;
    int y, x, ch;
    memset(dst, 0, sizeof(int) * 384);
    if (block->isI4x4 == 0) {
        int dc[16];
        int ctxIdx = mb->nzDc + leftMb->nzDc;
        int nz;
        memset(dc, 0, sizeof(dc));
        nz = vp8_get_coeffs(tokenBr, dec->proba, 1, 0, ctxIdx, qy2[0], qy2[1],
                            dc, 0);
        mb->nzDc = (nz > 0) ? 1 : 0;
        leftMb->nzDc = mb->nzDc;
        if (nz > 1) {
            vp8_transform_wht(dc, 0, dst, 0);
        } else {
            int dc0 = vp8_sar(dc[0] + 3, 3);
            int i;
            for (i = 0; i < 16; i++) dst[i * 16] = dc0;
        }
        first = 1;
    } else {
        first = 0;
    }
    acT = (block->isI4x4 == 0) ? 0 : 3;
    tnz = mb->nz & 0x0f;
    lnz = leftMb->nz & 0x0f;
    for (y = 0; y < 4; y++) {
        int l = lnz & 1;
        int nzCoeffs = 0;
        for (x = 0; x < 4; x++) {
            int ctxIdx = l + (tnz & 1);
            int nz = vp8_get_coeffs(tokenBr, dec->proba, acT, first, ctxIdx,
                                    q[0], q[1], dst, outOff);
            l = (nz > first) ? 1 : 0;
            tnz = ((tnz >> 1) | (l << 7)) & 0xff;
            {
                int code = 0;
                if (nz > 3)
                    code = 3;
                else if (nz > 1)
                    code = 2;
                else if (dst[outOff] != 0)
                    code = 1;
                nzCoeffs = ((nzCoeffs << 2) | code) & 0xff;
            }
            outOff += 16;
        }
        tnz >>= 4;
        lnz = ((lnz >> 1) | (l << 7)) & 0xff;
        nonZeroY = ((nonZeroY << 8) | (unsigned int)nzCoeffs);
    }
    outTNz = tnz;
    outLNz = lnz >> 4;
    for (ch = 0; ch <= 2; ch += 2) {
        int nzCoeffs = 0;
        tnz = (mb->nz >> (4 + ch)) & 0xff;
        lnz = (leftMb->nz >> (4 + ch)) & 0xff;
        for (y = 0; y < 2; y++) {
            int l = lnz & 1;
            for (x = 0; x < 2; x++) {
                int ctxIdx = l + (tnz & 1);
                int nz = vp8_get_coeffs(tokenBr, dec->proba, 2, 0, ctxIdx,
                                        quv[0], quv[1], dst, outOff);
                l = (nz > 0) ? 1 : 0;
                tnz = ((tnz >> 1) | (l << 3)) & 0xff;
                {
                    int code = 0;
                    if (nz > 3)
                        code = 3;
                    else if (nz > 1)
                        code = 2;
                    else if (dst[outOff] != 0)
                        code = 1;
                    nzCoeffs = ((nzCoeffs << 2) | code) & 0xff;
                }
                outOff += 16;
            }
            tnz >>= 2;
            lnz = ((lnz >> 1) | (l << 5)) & 0xff;
        }
        nonZeroUv |= (unsigned int)nzCoeffs << (4 * ch);
        outTNz |= (tnz << 4) << ch;
        outLNz |= (lnz & 0xf0) << ch;
    }
    mb->nz = outTNz & 0xff;
    leftMb->nz = outLNz & 0xff;
    block->nonZeroY = nonZeroY;
    block->nonZeroUv = nonZeroUv;
    block->dither = 0;
    return (nonZeroY | nonZeroUv) == 0;
}

static void vp8_parse_intra_mode(Vp8Br* br, Vp8Dec* dec, int mbX) {
    int topBase = mbX * 4;
    Vp8Block* block = &dec->mbData[mbX];
    if (dec->segmentHdr.updateMap != 0) {
        if (vp8_get_bit(br, dec->probaSegments[0]) == 0)
            block->segment = vp8_get_bit(br, dec->probaSegments[1]);
        else
            block->segment = vp8_get_bit(br, dec->probaSegments[2]) + 2;
    } else {
        block->segment = 0;
    }
    if (dec->useSkipProba != 0) block->skip = vp8_get_bit(br, dec->skipP);
    block->isI4x4 = (vp8_get_bit(br, 145) == 0) ? 1 : 0;
    if (block->isI4x4 == 0) {
        int ymode;
        if (vp8_get_bit(br, 156) != 0)
            ymode = (vp8_get_bit(br, 128) != 0) ? VP8_TM_PRED : VP8_H_PRED;
        else
            ymode = (vp8_get_bit(br, 163) != 0) ? VP8_V_PRED : VP8_DC_PRED;
        block->imodes[0] = ymode;
        {
            int i;
            for (i = 0; i < 4; i++) {
                dec->intraT[topBase + i] = ymode;
                dec->intraL[i] = ymode;
            }
        }
    } else {
        int y, x;
        for (y = 0; y < 4; y++) {
            int ymode = dec->intraL[y];
            for (x = 0; x < 4; x++) {
                int base = (dec->intraT[topBase + x] * 10 + ymode) * 9;
                if (vp8_get_bit(br, kBModesProba[base]) == 0)
                    ymode = 0;
                else if (vp8_get_bit(br, kBModesProba[base + 1]) == 0)
                    ymode = 1;
                else if (vp8_get_bit(br, kBModesProba[base + 2]) == 0)
                    ymode = 2;
                else if (vp8_get_bit(br, kBModesProba[base + 3]) == 0) {
                    if (vp8_get_bit(br, kBModesProba[base + 4]) == 0)
                        ymode = 3;
                    else if (vp8_get_bit(br, kBModesProba[base + 5]) == 0)
                        ymode = 4;
                    else
                        ymode = 5;
                } else if (vp8_get_bit(br, kBModesProba[base + 6]) == 0) {
                    ymode = 6;
                } else if (vp8_get_bit(br, kBModesProba[base + 7]) == 0) {
                    ymode = 7;
                } else if (vp8_get_bit(br, kBModesProba[base + 8]) == 0) {
                    ymode = 8;
                } else {
                    ymode = 9;
                }
                dec->intraT[topBase + x] = ymode;
                block->imodes[y * 4 + x] = ymode;
            }
            dec->intraL[y] = ymode;
        }
    }
    if (vp8_get_bit(br, 142) == 0) {
        block->uvMode = VP8_DC_PRED;
    } else if (vp8_get_bit(br, 114) == 0) {
        block->uvMode = VP8_V_PRED;
    } else {
        block->uvMode = (vp8_get_bit(br, 183) != 0) ? VP8_TM_PRED : VP8_H_PRED;
    }
}

static int vp8_parse_intra_mode_row(Vp8Br* br, Vp8Dec* dec) {
    int mbX;
    for (mbX = 0; mbX < dec->mbW; mbX++)
        vp8_parse_intra_mode(br, dec, mbX);
    return br->eof == 0;
}

static void vp8_init_scanline(Vp8Dec* dec) {
    Vp8MbInfo* leftMb = &dec->mbInfo[0];
    leftMb->nz = 0;
    leftMb->nzDc = 0;
    dec->intraL[0] = 0;
    dec->intraL[1] = 0;
    dec->intraL[2] = 0;
    dec->intraL[3] = 0;
    dec->mbX = 0;
}

static int vp8_decode_mb(Vp8Dec* dec, Vp8Br* tokenBr) {
    int mbX = dec->mbX;
    Vp8MbInfo* leftMb = &dec->mbInfo[0];
    Vp8MbInfo* mb = &dec->mbInfo[mbX + 1];
    Vp8Block* block = &dec->mbData[mbX];
    int skip = (dec->useSkipProba != 0) ? block->skip : 0;
    if (skip == 0) {
        skip = vp8_parse_residuals(dec, tokenBr) ? 1 : 0;
    } else {
        leftMb->nz = 0;
        mb->nz = 0;
        if (block->isI4x4 == 0) {
            leftMb->nzDc = 0;
            mb->nzDc = 0;
        }
        block->nonZeroY = 0;
        block->nonZeroUv = 0;
        block->dither = 0;
    }
    if (dec->filterType > 0) {
        const Vp8FStrength* fs = &dec->fstrengths[block->segment][block->isI4x4];
        block->fLimit = fs->fLimit;
        block->fIlevel = fs->fIlevel;
        block->hevThresh = fs->hevThresh;
        block->fInner = (fs->fInner != 0 || skip == 0) ? 1 : 0;
    }
    return tokenBr->eof == 0;
}

static void vp8_reconstruct_row(Vp8Dec* dec, int mbY) {
    int mbW = dec->mbW;
    int mbH = dec->mbH;
    int yBps = dec->cacheYStride;
    int uvBps = dec->cacheUvStride;
    uint8_t* cacheY = dec->cacheY;
    uint8_t* cacheU = dec->cacheU;
    uint8_t* cacheV = dec->cacheV;
    int j, i, mbX;

    for (j = 0; j < 16; j++) s_yArr[VP8_YBASE + j * VP8_BPS - 1] = 129;
    for (j = 0; j < 8; j++) {
        s_uArr[VP8_UBASE + j * VP8_BPS - 1] = 129;
        s_vArr[VP8_VBASE + j * VP8_BPS - 1] = 129;
    }
    if (mbY > 0) {
        s_yArr[VP8_YBASE - 33] = 129;
        s_uArr[VP8_UBASE - 33] = 129;
        s_vArr[VP8_VBASE - 33] = 129;
    } else {
        for (i = -1; i <= 19; i++) s_yArr[VP8_YBASE - 32 + i] = 127;
        for (i = -1; i <= 7; i++) {
            s_uArr[VP8_UBASE - 32 + i] = 127;
            s_vArr[VP8_VBASE - 32 + i] = 127;
        }
    }

    for (mbX = 0; mbX < mbW; mbX++) {
        Vp8Block* block = &dec->mbData[mbX];
        const int* coeffs = block->coeffs;
        Vp8TopYuv* topYuv = &dec->yuvT[mbX];
        if (mbX > 0) {
            /* Shift the cache columns 4px to the right. */
            for (j = -1; j <= 15; j++) {
                for (i = 0; i < 4; i++)
                    s_yArr[VP8_YBASE + j * VP8_BPS - 4 + i] =
                        s_yArr[VP8_YBASE + j * VP8_BPS + 12 + i];
            }
            for (j = -1; j <= 7; j++) {
                for (i = 0; i < 4; i++) {
                    s_uArr[VP8_UBASE + j * VP8_BPS - 4 + i] =
                        s_uArr[VP8_UBASE + j * VP8_BPS + 4 + i];
                    s_vArr[VP8_VBASE + j * VP8_BPS - 4 + i] =
                        s_vArr[VP8_VBASE + j * VP8_BPS + 4 + i];
                }
            }
        }
        if (mbY > 0) {
            for (i = 0; i < 16; i++)
                s_yArr[VP8_YBASE - VP8_BPS + i] = (uint8_t)topYuv->y[i];
            for (i = 0; i < 8; i++) {
                s_uArr[VP8_UBASE - VP8_BPS + i] = (uint8_t)topYuv->u[i];
                s_vArr[VP8_VBASE - VP8_BPS + i] = (uint8_t)topYuv->v[i];
            }
        }
        if (block->isI4x4 != 0) {
            if (mbY > 0) {
                if (mbX >= mbW - 1) {
                    int v = topYuv->y[15];
                    for (i = 0; i < 4; i++)
                        s_yArr[VP8_YBASE - VP8_BPS + 16 + i] = (uint8_t)v;
                } else {
                    const Vp8TopYuv* nxt = &dec->yuvT[mbX + 1];
                    for (i = 0; i < 4; i++)
                        s_yArr[VP8_YBASE - VP8_BPS + 16 + i] =
                            (uint8_t)nxt->y[i];
                }
            }
            for (j = 1; j <= 3; j++) {
                for (i = 0; i < 4; i++)
                    s_yArr[VP8_YBASE - VP8_BPS + 16 + j * 128 + i] =
                        s_yArr[VP8_YBASE - VP8_BPS + 16 + i];
            }
            {
                unsigned int bits = block->nonZeroY;
                int n;
                for (n = 0; n < 16; n++) {
                    int d = VP8_YBASE + kScan[n];
                    vp8_pred_luma4(block->imodes[n], s_yArr, d);
                    vp8_do_transform(bits, coeffs, n * 16, s_yArr, d);
                    bits <<= 2;
                }
            }
        } else {
            int pred = vp8_check_mode(mbX, mbY, block->imodes[0]);
            vp8_pred_luma16(pred, s_yArr, VP8_YBASE);
            if (block->nonZeroY != 0) {
                unsigned int bits = block->nonZeroY;
                int n;
                for (n = 0; n < 16; n++) {
                    vp8_do_transform(bits, coeffs, n * 16, s_yArr,
                                     VP8_YBASE + kScan[n]);
                    bits <<= 2;
                }
            }
        }
        {
            unsigned int bitsUV = block->nonZeroUv;
            int pred = vp8_check_mode(mbX, mbY, block->uvMode);
            vp8_pred_chroma8(pred, s_uArr, VP8_UBASE);
            vp8_pred_chroma8(pred, s_vArr, VP8_VBASE);
            vp8_do_uv_transform(bitsUV & 0xffu, coeffs, 256, s_uArr,
                                VP8_UBASE);
            vp8_do_uv_transform(bitsUV >> 8, coeffs, 320, s_vArr, VP8_VBASE);
        }
        if (mbY < mbH - 1) {
            for (i = 0; i < 16; i++)
                topYuv->y[i] = s_yArr[VP8_YBASE + 480 + i];
            for (i = 0; i < 8; i++) {
                topYuv->u[i] = s_uArr[VP8_UBASE + 224 + i];
                topYuv->v[i] = s_vArr[VP8_VBASE + 224 + i];
            }
        }
        {
            int yOut = dec->extra * yBps + mbX * 16;
            int uvOut = dec->extraUV * uvBps + mbX * 8;
            for (j = 0; j < 16; j++) {
                int sr = VP8_YBASE + j * VP8_BPS;
                int dr = yOut + j * yBps;
                for (i = 0; i < 16; i++) cacheY[dr + i] = s_yArr[sr + i];
            }
            for (j = 0; j < 8; j++) {
                int sr = VP8_UBASE + j * VP8_BPS;
                int dr = uvOut + j * uvBps;
                for (i = 0; i < 8; i++) {
                    cacheU[dr + i] = s_uArr[sr + i];
                    cacheV[dr + i] = s_vArr[sr + i];
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Fancy chroma upsampling + RGB conversion                            */

static int vp8_clip_yuv(int v) {
    if ((v & ~16383) == 0) return v >> 6;
    return (v < 0) ? 0 : 255;
}

static void vp8_write_rgb_pixel(uint8_t* rgb, int o, int yv, int u, int v) {
    rgb[o] = (uint8_t)vp8_clip_yuv(((yv * 19077) >> 8) + ((v * 26149) >> 8) -
                                   14234);
    rgb[o + 1] =
        (uint8_t)vp8_clip_yuv(((yv * 19077) >> 8) - ((u * 6419) >> 8) -
                              ((v * 13320) >> 8) + 8708);
    rgb[o + 2] = (uint8_t)vp8_clip_yuv(((yv * 19077) >> 8) +
                                       ((u * 33050) >> 8) - 17685);
}

/* Port of libwebp's UpsampleRgbLinePair_C. yBot == NULL mirrors the
 * single-row case (dstBot unused then). Each sample row is described by
 * (arr, base); samples are arr[base + i]. */
static void vp8_upsample_line_pair(
        const uint8_t* yTop, int yTopBase,
        const uint8_t* yBot, int yBotBase,
        const uint8_t* uTop, int uTopBase,
        const uint8_t* vTop, int vTopBase,
        const uint8_t* uBot, int uBotBase,
        const uint8_t* vBot, int vBotBase,
        uint8_t* rgb, int width, int dstTop, int dstBot) {
    int lastPair = (width - 1) >> 1;
    int tlU = uTop[uTopBase];
    int tlV = vTop[vTopBase];
    int lU = uBot[uBotBase];
    int lV = vBot[vBotBase];
    int o = dstTop * width * 3;
    int x;

    vp8_write_rgb_pixel(rgb, o, yTop[yTopBase], (3 * tlU + lU + 2) >> 2,
                        (3 * tlV + lV + 2) >> 2);
    if (yBot != NULL) {
        int ob = dstBot * width * 3;
        vp8_write_rgb_pixel(rgb, ob, yBot[yBotBase], (3 * lU + tlU + 2) >> 2,
                            (3 * lV + tlV + 2) >> 2);
    }
    for (x = 1; x <= lastPair; x++) {
        int tU = uTop[uTopBase + x];
        int tV = vTop[vTopBase + x];
        int cU = uBot[uBotBase + x];
        int cV = vBot[vBotBase + x];
        int avgU = tlU + tU + lU + cU + 8;
        int avgV = tlV + tV + lV + cV + 8;
        int d12U = (avgU + 2 * (tU + lU)) >> 3;
        int d03U = (avgU + 2 * (tlU + cU)) >> 3;
        int d12V = (avgV + 2 * (tV + lV)) >> 3;
        int d03V = (avgV + 2 * (tlV + cV)) >> 3;
        int o2 = o + (2 * x - 1) * 3;
        vp8_write_rgb_pixel(rgb, o2, yTop[yTopBase + 2 * x - 1],
                            (d12U + tlU) >> 1, (d12V + tlV) >> 1);
        vp8_write_rgb_pixel(rgb, o2 + 3, yTop[yTopBase + 2 * x],
                            (d03U + tU) >> 1, (d03V + tV) >> 1);
        if (yBot != NULL) {
            int ob = dstBot * width * 3 + (2 * x - 1) * 3;
            vp8_write_rgb_pixel(rgb, ob, yBot[yBotBase + 2 * x - 1],
                                (d03U + lU) >> 1, (d03V + lV) >> 1);
            vp8_write_rgb_pixel(rgb, ob + 3, yBot[yBotBase + 2 * x],
                                (d12U + cU) >> 1, (d12V + cV) >> 1);
        }
        tlU = tU;
        tlV = tV;
        lU = cU;
        lV = cV;
    }
    if ((width & 1) == 0) {
        int o2 = o + (width - 1) * 3;
        vp8_write_rgb_pixel(rgb, o2, yTop[yTopBase + width - 1],
                            (3 * tlU + lU + 2) >> 2, (3 * tlV + lV + 2) >> 2);
        if (yBot != NULL) {
            int ob = dstBot * width * 3 + (width - 1) * 3;
            vp8_write_rgb_pixel(rgb, ob, yBot[yBotBase + width - 1],
                                (3 * lU + tlU + 2) >> 2,
                                (3 * lV + tlV + 2) >> 2);
        }
    }
}

static void vp8_finish_row(Vp8Dec* dec, int isFirst, int isLast) {
    int width = dec->width;
    int height = dec->height;
    int mbY = dec->mbY;
    int yStart = mbY * 16;
    int yEnd = yStart + 16;
    int k = 1;
    int curY;
    if (!isFirst) yStart -= dec->extra;
    if (!isLast) yEnd -= dec->extra;
    if (yEnd > height) yEnd = height;
    if (yStart < yEnd) {
        int rowOff = isFirst ? dec->extra : 0;
        int uvOff = isFirst ? dec->extraUV : 0;
        int yBps = dec->cacheYStride;
        int uvBps = dec->cacheUvStride;
        uint8_t* cacheY = dec->cacheY;
        uint8_t* cacheU = dec->cacheU;
        uint8_t* cacheV = dec->cacheV;
        uint8_t* rgb = dec->rgb;
        if (yStart == 0) {
            /* First line is special-cased: mirror u/v at the boundary. */
            int yBase = rowOff * yBps;
            int uvBase = uvOff * uvBps;
            vp8_upsample_line_pair(cacheY, yBase, NULL, 0,
                                   cacheU, uvBase, cacheV, uvBase,
                                   cacheU, uvBase, cacheV, uvBase,
                                   rgb, width, 0, 0);
        } else {
            /* Finish the left-over row from the previous call. */
            int uvBase = uvOff * uvBps;
            vp8_upsample_line_pair(dec->tmpY, 0, cacheY, rowOff * yBps,
                                   dec->tmpU, 0, dec->tmpV, 0,
                                   cacheU, uvBase, cacheV, uvBase,
                                   rgb, width, yStart - 1, yStart);
        }
        while (yStart + 2 * k < yEnd) {
            int yRow = rowOff + 2 * k - 1;
            int uvRow = uvOff + k - 1;
            vp8_upsample_line_pair(cacheY, yRow * yBps, cacheY,
                                   (yRow + 1) * yBps,
                                   cacheU, uvRow * uvBps, cacheV,
                                   uvRow * uvBps,
                                   cacheU, (uvRow + 1) * uvBps, cacheV,
                                   (uvRow + 1) * uvBps,
                                   rgb, width, yStart + 2 * k - 1,
                                   yStart + 2 * k);
            k++;
        }
        curY = yStart + 2 * (k - 1) + 1;
        if (yEnd < height) {
            /* Save the unfinished samples for the next call. */
            int yBase = (rowOff + curY - yStart) * yBps;
            int uvBase = (uvOff + ((curY >> 1) - (yStart >> 1))) * uvBps;
            int uvWidth = (width + 1) >> 1;
            int i;
            for (i = 0; i < width; i++) dec->tmpY[i] = cacheY[yBase + i];
            for (i = 0; i < uvWidth; i++) {
                dec->tmpU[i] = cacheU[uvBase + i];
                dec->tmpV[i] = cacheV[uvBase + i];
            }
        } else if ((yEnd & 1) == 0) {
            /* Very last row of an even-sized picture. */
            int yBase = (rowOff + yEnd - 1 - yStart) * yBps;
            int uvBase = (uvOff + ((yEnd - 1 - yStart) >> 1)) * uvBps;
            vp8_upsample_line_pair(cacheY, yBase, NULL, 0,
                                   cacheU, uvBase, cacheV, uvBase,
                                   cacheU, uvBase, cacheV, uvBase,
                                   rgb, width, yEnd - 1, 0);
        }
    }
    if (!isLast) {
        int yBps = dec->cacheYStride;
        int uvBps = dec->cacheUvStride;
        uint8_t* cacheY = dec->cacheY;
        uint8_t* cacheU = dec->cacheU;
        uint8_t* cacheV = dec->cacheV;
        int kk, i;
        for (kk = 0; kk < dec->extra; kk++) {
            int src = (16 + kk) * yBps;
            int dst = kk * yBps;
            for (i = 0; i < yBps; i++) cacheY[dst + i] = cacheY[src + i];
        }
        for (kk = 0; kk < dec->extraUV; kk++) {
            int src = (8 + kk) * uvBps;
            int dst = kk * uvBps;
            for (i = 0; i < uvBps; i++) {
                cacheU[dst + i] = cacheU[src + i];
                cacheV[dst + i] = cacheV[src + i];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* VP8 payload entry point                                             */

/* Guarded little-endian read: bytes beyond len yield 0 (`or 0`). */
static unsigned int webp_read_le24(const uint8_t* data, size_t len,
                                   size_t pos) {
    unsigned int b1 = (pos < len) ? data[pos] : 0;
    unsigned int b2 = (pos + 1 < len) ? data[pos + 1] : 0;
    unsigned int b3 = (pos + 2 < len) ? data[pos + 2] : 0;
    return b1 | (b2 << 8) | (b3 << 16);
}

/* Decodes one lossy key frame. On success returns 1 and sets outRgb
 * (w*h*3 bytes, malloc'd) plus an optional outAlpha plane (NULL when no
 * ALPH chunk is present). Caller frees both buffers. */
static int vp8_decode_payload(const uint8_t* payload, size_t len,
                              const uint8_t* alphaPayload, size_t alphaLen,
                              uint8_t** outRgb, uint8_t** outAlpha,
                              int* outW, int* outH) {
    Vp8Dec dec;
    unsigned int bits;
    long long partitionLength, sizesStart, partStart;
    int profile, nParts, last, p, w, h;
    int extra, extraUV, yBps, uvBps, i;
    int mbY, mbX;

    vp8_init_log2();
    memset(&dec, 0, sizeof(dec));
    *outRgb = NULL;
    *outAlpha = NULL;
    *outW = 0;
    *outH = 0;

    if (len < 11) return 0;
    bits = (unsigned int)payload[0] | ((unsigned int)payload[1] << 8) |
           ((unsigned int)payload[2] << 16);
    if ((bits & 1u) != 0) return 0; /* interframe */
    profile = (int)((bits >> 1) & 7u);
    if (profile > 3) return 0;
    if (((bits >> 4) & 1u) == 0) return 0; /* not a key frame */
    partitionLength = (long long)(bits >> 5);
    if (payload[3] != 0x9d || payload[4] != 0x01 || payload[5] != 0x2a)
        return 0;
    w = (int)(((unsigned int)payload[7] << 8 | payload[6]) & 0x3fff);
    h = (int)(((unsigned int)payload[9] << 8 | payload[8]) & 0x3fff);
    if (w < 1 || h < 1 || (long long)w * h > WEBP_MAX_PIXELS) return 0;
    if (11 + partitionLength > (long long)len + 1) return 0;

    dec.width = w;
    dec.height = h;
    dec.mbW = (w + 15) >> 4;
    dec.mbH = (h + 15) >> 4;
    dec.filterType = 0;
    dec.probaSegments[0] = 255;
    dec.probaSegments[1] = 255;
    dec.probaSegments[2] = 255;
    memcpy(dec.proba, CoeffsProba0, sizeof(dec.proba));
    vp8_new_br(&dec.br, payload, len, 11, partitionLength);

    (void)vp8_get_bit(&dec.br, 128); /* colorspace */
    (void)vp8_get_bit(&dec.br, 128); /* clamp_type */
    if (!vp8_parse_segment_header(&dec.br, &dec)) return 0;
    if (!vp8_parse_filter_header(&dec.br, &dec)) return 0;
    nParts = 1 << vp8_get_value(&dec.br, 2);
    dec.numPartsMinusOne = nParts - 1;
    last = nParts - 1;
    sizesStart = 11 + partitionLength;
    partStart = sizesStart + 3LL * last;
    if (partStart > (long long)len + 1) return 0;
    for (p = 0; p < last; p++) {
        long long psize =
            (long long)webp_read_le24(payload, len,
                                      (size_t)sizesStart + (size_t)p * 3);
        long long remaining = (long long)len - partStart + 1;
        if (psize > remaining) psize = remaining;
        vp8_new_br(&dec.parts[p], payload, len, partStart, psize);
        partStart += psize;
    }
    vp8_new_br(&dec.parts[last], payload, len, partStart,
               (long long)len - partStart + 1);

    vp8_parse_quant(&dec.br, &dec);
    (void)vp8_get_bit(&dec.br, 128); /* refresh entropy probabilities */
    vp8_parse_proba(&dec.br, &dec);

    extra = kFilterExtraRows[dec.filterType];
    extraUV = extra >> 1;
    yBps = 16 * dec.mbW;
    uvBps = 8 * dec.mbW;
    dec.extra = extra;
    dec.extraUV = extraUV;
    dec.cacheYStride = yBps;
    dec.cacheUvStride = uvBps;

    dec.cacheY = (uint8_t*)pluto_malloc((size_t)(extra + 16) * (size_t)yBps);
    dec.cacheU = (uint8_t*)pluto_malloc((size_t)(extraUV + 8) *
                                        (size_t)uvBps);
    dec.cacheV = (uint8_t*)pluto_malloc((size_t)(extraUV + 8) *
                                        (size_t)uvBps);
    dec.rgb = (uint8_t*)pluto_calloc((size_t)w * (size_t)h * 3, 1);
    dec.tmpY = (uint8_t*)pluto_calloc((size_t)w, 1);
    dec.tmpU = (uint8_t*)pluto_calloc((size_t)((w + 1) / 2), 1);
    dec.tmpV = (uint8_t*)pluto_calloc((size_t)((w + 1) / 2), 1);
    dec.mbData = (Vp8Block*)pluto_calloc((size_t)dec.mbW, sizeof(Vp8Block));
    dec.mbInfo =
        (Vp8MbInfo*)pluto_calloc((size_t)dec.mbW + 1, sizeof(Vp8MbInfo));
    dec.intraT = (int*)pluto_calloc((size_t)4 * (size_t)dec.mbW, sizeof(int));
    dec.yuvT =
        (Vp8TopYuv*)pluto_calloc((size_t)dec.mbW + 1, sizeof(Vp8TopYuv));
    if (!dec.cacheY || !dec.cacheU || !dec.cacheV || !dec.rgb ||
        !dec.tmpY || !dec.tmpU || !dec.tmpV || !dec.mbData ||
        !dec.mbInfo || !dec.intraT || !dec.yuvT)
        goto fail;
    for (i = 0; i < (extra + 16) * yBps; i++) dec.cacheY[i] = 127;
    for (i = 0; i < (extraUV + 8) * uvBps; i++) dec.cacheU[i] = 127;
    for (i = 0; i < (extraUV + 8) * uvBps; i++) dec.cacheV[i] = 127;
    vp8_precompute_filter_strengths(&dec);

    dec.mbX = 0;
    for (mbY = 0; mbY < dec.mbH; mbY++) {
        Vp8Br* tokenBr;
        tasks_yield_check();
        dec.mbY = mbY;
        tokenBr = &dec.parts[mbY & dec.numPartsMinusOne];
        if (!vp8_parse_intra_mode_row(&dec.br, &dec)) goto fail;
        for (mbX = 0; mbX < dec.mbW; mbX++) {
            dec.mbX = mbX;
            if (!vp8_decode_mb(&dec, tokenBr)) goto fail;
        }
        vp8_init_scanline(&dec);
        vp8_reconstruct_row(&dec, mbY);
        if (dec.filterType > 0) {
            for (mbX = 0; mbX < dec.mbW; mbX++)
                vp8_do_filter(&dec, mbX, mbY);
        }
        vp8_finish_row(&dec, mbY == 0, mbY == dec.mbH - 1);
    }

    *outAlpha = alpha_decode_plane(alphaPayload, alphaLen, w, h);
    *outRgb = dec.rgb;
    *outW = w;
    *outH = h;
    pluto_free(dec.cacheY);
    pluto_free(dec.cacheU);
    pluto_free(dec.cacheV);
    pluto_free(dec.tmpY);
    pluto_free(dec.tmpU);
    pluto_free(dec.tmpV);
    pluto_free(dec.mbData);
    pluto_free(dec.mbInfo);
    pluto_free(dec.intraT);
    pluto_free(dec.yuvT);
    return 1;

fail:
    pluto_free(dec.cacheY);
    pluto_free(dec.cacheU);
    pluto_free(dec.cacheV);
    pluto_free(dec.rgb);
    pluto_free(dec.tmpY);
    pluto_free(dec.tmpU);
    pluto_free(dec.tmpV);
    pluto_free(dec.mbData);
    pluto_free(dec.mbInfo);
    pluto_free(dec.intraT);
    pluto_free(dec.yuvT);
    return 0;
}

/* Decodes one lossless payload into a flat w*h ARGB grid. Returns 1 on
 * success; caller frees *outPix. */
static int vp8l_decode_payload(const uint8_t* payload, size_t plen,
                               uint32_t** outPix, int* outW, int* outH) {
    uint32_t bits;
    int w, h;
    Br br;
    VP8LCtx ctx;
    uint32_t* flat = NULL;
    uint32_t* finalPix = NULL;

    memset(&ctx, 0, sizeof(ctx));
    *outPix = NULL;
    *outW = 0;
    *outH = 0;
    if (plen < 5 || payload[0] != VP8L_MAGIC_BYTE) return 0;
    bits = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
           ((uint32_t)payload[3] << 16);
    w = (int)(bits & 0x3FFF) + 1;
    h = (int)((bits >> 14) & 0x3FFF) + 1;
    if (((bits >> 22) & 7) != 0) return 0; /* unknown version */
    if (w < 1 || h < 1 || (long long)w * h > WEBP_MAX_PIXELS) return 0;

    br.data = payload;
    br.len = plen;
    br.pos = 5;
    br.window = 0;
    br.nbits = 0;
    br.eos = 0;
    tasks_yield_check();
    if (!decode_image_stream(w, h, 1, &br, &ctx, NULL)) goto fail;

    {
        int tw = ctx.transformXsize;
        int th = ctx.transformYsize;
        if (tw < 1 || th < 1 || tw * th > WEBP_MAX_PIXELS)
            goto fail;
        flat = (uint32_t*)pluto_calloc((size_t)tw * (size_t)th,
                                       sizeof(uint32_t));
        if (!flat) goto fail;
        if (!decode_image_data(&br, &ctx, flat, tw, th)) goto fail;
        if (br.eos) goto fail;
        finalPix = apply_inverse_transforms(&ctx, flat, th);
        flat = NULL; /* consumed (freed or returned) */
        if (!finalPix) goto fail;
        ctx_free(&ctx);
        /* The inverse transforms expand the (possibly sub-sampled)
         * decode grid back to the full header width; rows stay th. */
        *outPix = finalPix;
        *outW = w;
        *outH = th;
        return 1;
    }

fail:
    pluto_free(finalPix);
    pluto_free(flat);
    ctx_free(&ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* P22: animated WebP (VP8X + ANIM + ANMF demux + non-premultiplied    */
/* blending, ported from libwebp 1.6.0 demux.c / anim_decode.c)        */

enum { WEBP_CID_NONE = 0, WEBP_CID_VP8L, WEBP_CID_VP8 };

static int vp8l_decode_payload(const uint8_t* payload, size_t plen,
                               uint32_t** outPix, int* outW, int* outH);

typedef struct {
    int x, y, w, h;
    int durationMs;
    int dispose, noBlend;
    int cid;
    const uint8_t* payload;
    size_t plen;
    const uint8_t* alpha;
    size_t alen;
} WebPFrameRec;

static unsigned int webp_byte_at(const uint8_t* data, size_t len,
                                 size_t pos) {
    return (pos < len) ? data[pos] : 0;
}

static uint32_t webp_chunk_size(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Parses the container of an animated WebP. Returns 0 if not animated.
 * Frame payloads point into `data` (no copies taken). */
static int anim_parse_webp_animation(
        const uint8_t* data, size_t len,
        int* outCw, int* outCh, uint32_t* outBgcolor, int* outLoopCount,
        WebPFrameRec** outFrames, int* outNumFrames) {
    size_t pos = 12;
    int canvasW = 0, canvasH = 0, loopCount = 0;
    uint32_t bgcolor = 0;
    int isExtended = 0, seenAnim = 0;
    WebPFrameRec* frames = NULL;
    int numFrames = 0, capFrames = 0;

    *outFrames = NULL;
    *outNumFrames = 0;
    if (len < 20) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return 0;
    while (pos + 8 <= len) {
        uint32_t size = webp_chunk_size(data + pos + 4);
        if (memcmp(data + pos, "VP8X", 4) == 0) {
            size_t cp = pos + 8;
            unsigned int flags;
            isExtended = 1;
            flags = webp_byte_at(data, len, cp);
            /* VP8X payload: 3 reserved bytes, flags byte, then
             * canvas-width-1 and canvas-height-1 as LE24. */
            canvasW = (int)webp_read_le24(data, len, cp + 4) + 1;
            canvasH = (int)webp_read_le24(data, len, cp + 7) + 1;
            if (canvasW < 1 || canvasH < 1 ||
                (long long)canvasW * canvasH > WEBP_MAX_PIXELS)
                goto fail;
            if ((flags & 0x02u) == 0)
                goto fail; /* ANIMATION_FLAG not set */
        } else if (memcmp(data + pos, "ANIM", 4) == 0) {
            size_t ap = pos + 8;
            unsigned int a, r, g, b;
            seenAnim = 1;
            /* ANIM stores the canvas background color in BGRA byte
             * order; expose it as a canonical 0xAARRGGBB word. */
            b = webp_byte_at(data, len, ap);
            g = webp_byte_at(data, len, ap + 1);
            r = webp_byte_at(data, len, ap + 2);
            a = webp_byte_at(data, len, ap + 3);
            bgcolor = (a << 24) | (r << 16) | (g << 8) | b;
            loopCount = (int)(webp_byte_at(data, len, ap + 4) |
                              (webp_byte_at(data, len, ap + 5) << 8));
        } else if (memcmp(data + pos, "ANMF", 4) == 0) {
            WebPFrameRec f;
            size_t avail = len - (pos + 8);
            size_t fplen = size < avail ? size : avail;
            const uint8_t* fpay = data + pos + 8;
            size_t ipos = 16;
            unsigned int bits2;
            if (!seenAnim || canvasW == 0) goto fail;
            if (fplen < 16) goto fail;
            memset(&f, 0, sizeof(f));
            f.x = (int)(2u * webp_read_le24(fpay, fplen, 0));
            f.y = (int)(2u * webp_read_le24(fpay, fplen, 3));
            f.w = (int)webp_read_le24(fpay, fplen, 6) + 1;
            f.h = (int)webp_read_le24(fpay, fplen, 9) + 1;
            f.durationMs = (int)webp_read_le24(fpay, fplen, 12);
            bits2 = webp_byte_at(fpay, fplen, 15);
            f.dispose = (bits2 & 1u) ? 1 : 0;
            f.noBlend = ((bits2 >> 1) & 1u) ? 1 : 0;
            while (ipos + 8 <= fplen) {
                uint32_t isize = webp_chunk_size(fpay + ipos + 4);
                size_t iavail = fplen - (ipos + 8);
                size_t ilen = isize < iavail ? isize : iavail;
                if (memcmp(fpay + ipos, "ALPH", 4) == 0) {
                    f.alpha = fpay + ipos + 8;
                    f.alen = ilen;
                } else if (memcmp(fpay + ipos, "VP8L", 4) == 0) {
                    f.cid = WEBP_CID_VP8L;
                    f.payload = fpay + ipos + 8;
                    f.plen = ilen;
                } else if (memcmp(fpay + ipos, "VP8 ", 4) == 0) {
                    f.cid = WEBP_CID_VP8;
                    f.payload = fpay + ipos + 8;
                    f.plen = ilen;
                }
                ipos += 8 + (size_t)isize + (isize & 1u);
            }
            if (f.cid == WEBP_CID_NONE || f.payload == NULL) goto fail;
            if (f.w < 1 || f.h < 1 || f.x < 0 || f.y < 0 ||
                f.x + f.w > canvasW || f.y + f.h > canvasH)
                goto fail;
            if (numFrames == capFrames) {
                int ncap = capFrames == 0 ? 8 : capFrames * 2;
                WebPFrameRec* nf = (WebPFrameRec*)pluto_realloc(
                    frames, (size_t)ncap * sizeof(WebPFrameRec));
                if (nf == NULL) goto fail;
                frames = nf;
                capFrames = ncap;
            }
            frames[numFrames++] = f;
        }
        pos += 8 + (size_t)size + (size & 1u);
    }
    if (!isExtended || numFrames == 0) goto fail;
    *outCw = canvasW;
    *outCh = canvasH;
    *outBgcolor = bgcolor;
    *outLoopCount = loopCount;
    *outFrames = frames;
    *outNumFrames = numFrames;
    return 1;
fail:
    pluto_free(frames);
    return 0;
}

/* Non-premultiplied alpha blending (libwebp AnimDecoder blend_func).
 * Returns src over dst in ARGB. */
static uint32_t anim_blend_pixel(uint32_t src, uint32_t dst) {
    unsigned int srcA = (src >> 24) & 0xFFu;
    unsigned int dstFactorA, blendA, scale, out;
    int shift;
    if (srcA == 0) return dst;
    dstFactorA = (((dst >> 24) & 0xFFu) * (256u - srcA)) >> 8;
    blendA = srcA + dstFactorA;
    scale = 16777216u / blendA;
    out = blendA << 24;
    for (shift = 0; shift <= 16; shift += 8) {
        uint64_t sc = (src >> shift) & 0xFFu;
        uint64_t dc = (dst >> shift) & 0xFFu;
        unsigned int c =
            (unsigned)(((sc * srcA + dc * dstFactorA) * scale) / 16777216u);
        out |= c << shift;
    }
    return out;
}

/* Blends one horizontal range of semi-transparent pixels onto the
 * previous canvas contents. */
static void anim_blend_range(uint32_t* curr, const uint32_t* prev,
                             size_t off, int width) {
    int fx;
    for (fx = 0; fx < width; fx++) {
        uint32_t v = curr[off + (size_t)fx];
        if (((v >> 24) & 0xFFu) != 0xFFu)
            curr[off + (size_t)fx] =
                anim_blend_pixel(v, prev[off + (size_t)fx]);
    }
}

/* Decodes every frame of an animated WebP into a canvas-sized blended
 * ARGB grid (libwebp AnimDecoder semantics). Returns 0 on success,
 * -1 on failure; caller frees with webp_anim_free(). */
int webp_decode_anim(const uint8_t* data, size_t len, WebPAnim** outAnim) {
    WebPFrameRec* recs = NULL;
    int numRecs = 0, cw = 0, chh = 0, loop = 0;
    uint32_t bg = 0;
    WebPAnim* anim = NULL;
    uint32_t* curr = NULL;
    uint32_t* prevDisposed = NULL;
    int i, prevWasKey = 1;
    size_t total;

    *outAnim = NULL;
    if (!anim_parse_webp_animation(data, len, &cw, &chh, &bg, &loop, &recs,
                                   &numRecs))
        return -1;
    total = (size_t)cw * (size_t)chh;
    curr = (uint32_t*)pluto_calloc(total, sizeof(uint32_t));
    prevDisposed = (uint32_t*)pluto_malloc(total * sizeof(uint32_t));
    anim = (WebPAnim*)pluto_calloc(1, sizeof(WebPAnim));
    anim->frames =
        (WebPAnimFrame*)pluto_calloc((size_t)numRecs, sizeof(WebPAnimFrame));
    if (!curr || !prevDisposed || !anim || !anim->frames) goto fail2;

    for (i = 0; i < numRecs; i++) {
        const WebPFrameRec* f = &recs[i];
        const WebPFrameRec* prev = (i > 0) ? &recs[i - 1] : NULL;
        uint32_t* fargb = NULL;
        uint8_t* rgb = NULL;
        uint8_t* alpha = NULL;
        int hasAlpha, isKey, fw = 0, fh = 0, fy;

        tasks_yield_check();
        if (f->cid == WEBP_CID_VP8L) {
            if (!vp8l_decode_payload(f->payload, f->plen, &fargb, &fw, &fh))
                goto fail2;
            hasAlpha = 1;
        } else {
            size_t j, npix;
            if (!vp8_decode_payload(f->payload, f->plen, f->alpha, f->alen,
                                    &rgb, &alpha, &fw, &fh))
                goto fail2;
            hasAlpha = (alpha != NULL);
            npix = (size_t)fw * (size_t)fh;
            fargb = (uint32_t*)pluto_malloc(npix * sizeof(uint32_t));
            if (!fargb) {
                pluto_free(rgb);
                pluto_free(alpha);
                goto fail2;
            }
            for (j = 0; j < npix; j++) {
                unsigned int a = alpha ? alpha[j] : 0xFFu;
                fargb[j] = (a << 24) | ((uint32_t)rgb[j * 3] << 16) |
                           ((uint32_t)rgb[j * 3 + 1] << 8) |
                           rgb[j * 3 + 2];
            }
            pluto_free(rgb);
            pluto_free(alpha);
        }
        if (fw != f->w || fh != f->h) {
            pluto_free(fargb);
            goto fail2;
        }

        /* Keyframe detection (libwebp is_key_frame). */
        if (i == 0) {
            isKey = 1;
        } else if ((!hasAlpha || f->noBlend) && f->w == cw && f->h == chh) {
            isKey = 1;
        } else if (prev->dispose && (prev->w == cw || prevWasKey)) {
            isKey = 1;
        } else {
            isKey = 0;
        }

        if (isKey) {
            memset(curr, 0, total * sizeof(uint32_t));
        } else {
            memcpy(curr, prevDisposed, total * sizeof(uint32_t));
        }

        /* Copy the frame rectangle onto the canvas. */
        for (fy = 0; fy < f->h; fy++) {
            uint32_t* d = curr + (size_t)(f->y + fy) * cw + (size_t)f->x;
            const uint32_t* s = fargb + (size_t)fy * f->w;
            memcpy(d, s, sizeof(uint32_t) * (size_t)f->w);
        }

        /* Blend against the disposed background where needed. */
        if (i > 0 && !f->noBlend && !isKey) {
            if (!prev->dispose) {
                for (fy = 0; fy < f->h; fy++) {
                    anim_blend_range(curr, prevDisposed,
                                     (size_t)(f->y + fy) * cw + (size_t)f->x,
                                     f->w);
                }
            } else {
                /* Only pixels outside the previously-disposed rectangle
                 * have a defined background. */
                int srcMaxX = f->x + f->w;
                int dstMaxX = prev->x + prev->w;
                int dstMaxY = prev->y + prev->h;
                for (fy = 0; fy < f->h; fy++) {
                    int canvasY = f->y + fy;
                    if (canvasY < prev->y || canvasY >= dstMaxY ||
                        f->x >= dstMaxX || srcMaxX <= prev->x) {
                        anim_blend_range(curr, prevDisposed,
                                         (size_t)canvasY * cw +
                                             (size_t)f->x,
                                         f->w);
                    } else {
                        if (f->x < prev->x)
                            anim_blend_range(curr, prevDisposed,
                                             (size_t)canvasY * cw +
                                                 (size_t)f->x,
                                             prev->x - f->x);
                        if (srcMaxX > dstMaxX)
                            anim_blend_range(curr, prevDisposed,
                                             (size_t)canvasY * cw +
                                                 (size_t)dstMaxX,
                                             srcMaxX - dstMaxX);
                    }
                }
            }
        }

        memcpy(prevDisposed, curr, total * sizeof(uint32_t));

        anim->frames[i].pix =
            (uint32_t*)pluto_malloc(total * sizeof(uint32_t));
        if (!anim->frames[i].pix) {
            pluto_free(fargb);
            goto fail2;
        }
        memcpy(anim->frames[i].pix, curr, total * sizeof(uint32_t));
        anim->frames[i].durationMs = f->durationMs;

        if (f->dispose) {
            for (fy = 0; fy < f->h; fy++) {
                uint32_t* d = prevDisposed + (size_t)(f->y + fy) * cw +
                              (size_t)f->x;
                memset(d, 0, sizeof(uint32_t) * (size_t)f->w);
            }
        }
        pluto_free(fargb);
        prevWasKey = isKey;
    }

    anim->width = cw;
    anim->height = chh;
    anim->bgcolor = bg;
    anim->loopCount = loop;
    anim->numFrames = numRecs;
    pluto_free(recs);
    pluto_free(curr);
    pluto_free(prevDisposed);
    *outAnim = anim;
    return 0;

fail2:
    if (anim != NULL && anim->frames != NULL) {
        for (i = 0; i < numRecs; i++)
            pluto_free(anim->frames[i].pix);
        pluto_free(anim->frames);
    }
    pluto_free(anim);
    pluto_free(curr);
    pluto_free(prevDisposed);
    pluto_free(recs);
    return -1;
}

void webp_anim_free(WebPAnim* anim) {
    int i;
    if (!anim) return;
    if (anim->frames) {
        for (i = 0; i < anim->numFrames; i++)
            pluto_free(anim->frames[i].pix);
        pluto_free(anim->frames);
    }
    pluto_free(anim);
}

/* ------------------------------------------------------------------ */
/* RIFF container parsing (still images).                              */

enum { WEBP_CHUNK_NONE = 0, WEBP_CHUNK_VP8L, WEBP_CHUNK_VP8 };

/* Locates the primary image chunk of a simple or extended WebP file
 * plus an optional sibling ALPH chunk. Animation chunks (ANIM/ANMF)
 * are skipped: animated decoding goes through webp_decode_anim(). */
static int parse_webp(const uint8_t* data, size_t len,
                      const uint8_t** outPayload, size_t* outLen,
                      const uint8_t** outAlpha, size_t* outAlphaLen,
                      int* outKind) {
    size_t pos;
    if (len < 20) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return 0;
    *outPayload = NULL;
    *outLen = 0;
    *outAlpha = NULL;
    *outAlphaLen = 0;
    *outKind = WEBP_CHUNK_NONE;
    pos = 12;
    while (pos + 8 <= len) {
        uint32_t size = (uint32_t)data[pos + 4] |
                        ((uint32_t)data[pos + 5] << 8) |
                        ((uint32_t)data[pos + 6] << 16) |
                        ((uint32_t)data[pos + 7] << 24);
        size_t avail = len - (pos + 8);
        size_t clen = size < avail ? size : avail;
        if (memcmp(data + pos, "VP8L", 4) == 0 ||
            memcmp(data + pos, "VP8 ", 4) == 0) {
            if (*outPayload == NULL) {
                *outKind = (data[pos + 3] == 'L') ? WEBP_CHUNK_VP8L
                                                  : WEBP_CHUNK_VP8;
                *outPayload = data + pos + 8;
                *outLen = clen;
            }
        } else if (memcmp(data + pos, "ALPH", 4) == 0 &&
                   *outAlpha == NULL) {
            *outAlpha = data + pos + 8;
            *outAlphaLen = clen;
        }
        pos += 8 + size + (size & 1);
    }
    return (*outPayload != NULL) ? 1 : 0;
}

int webp_decode_argb(const uint8_t* data, size_t len, int maxW, int maxH,
                     uint32_t*** outRows, int* outW, int* outH) {
    const uint8_t* payload = NULL;
    const uint8_t* alphaPay = NULL;
    size_t plen = 0, alphaLen = 0;
    int kind = WEBP_CHUNK_NONE;
    uint32_t* flat = NULL;
    uint32_t** grid = NULL;
    int w = 0, h = 0, y;

    (void)maxW;
    (void)maxH;
    if (!outRows || !outW || !outH) return -1;
    *outRows = NULL;
    *outW = 0;
    *outH = 0;
    if (!parse_webp(data, len, &payload, &plen, &alphaPay, &alphaLen,
                    &kind))
        return -1;

    tasks_yield_check();
    if (kind == WEBP_CHUNK_VP8L) {
        if (!vp8l_decode_payload(payload, plen, &flat, &w, &h)) return -1;
    } else {
        uint8_t* rgb = NULL;
        uint8_t* alpha = NULL;
        size_t i, npix;
        if (!vp8_decode_payload(payload, plen, alphaPay, alphaLen,
                                &rgb, &alpha, &w, &h)) {
            return -1;
        }
        npix = (size_t)w * (size_t)h;
        flat = (uint32_t*)pluto_malloc(npix * sizeof(uint32_t));
        if (!flat) {
            pluto_free(rgb);
            pluto_free(alpha);
            return -1;
        }
        for (i = 0; i < npix; i++) {
            unsigned int a = alpha ? alpha[i] : 0xFFu;
            flat[i] = (a << 24) | ((uint32_t)rgb[i * 3] << 16) |
                      ((uint32_t)rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
        }
        pluto_free(rgb);
        pluto_free(alpha);
    }

    grid = (uint32_t**)pluto_malloc(sizeof(uint32_t*) * (size_t)h);
    if (!grid) {
        pluto_free(flat);
        return -1;
    }
    memset(grid, 0, sizeof(uint32_t*) * (size_t)h);
    for (y = 0; y < h; y++) {
        grid[y] = (uint32_t*)pluto_malloc(sizeof(uint32_t) * (size_t)w);
        if (!grid[y]) goto fail;
        memcpy(grid[y], flat + (size_t)y * (size_t)w,
               sizeof(uint32_t) * (size_t)w);
    }
    pluto_free(flat);
    *outRows = grid;
    *outW = w;
    *outH = h;
    return 0;

fail:
    for (y = 0; y < h && grid[y]; y++) pluto_free(grid[y]);
    pluto_free(grid);
    pluto_free(flat);
    return -1;
}

void webp_free_rows_argb(uint32_t** rows, int h) {
    int y;
    if (!rows) return;
    for (y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

void webp_free_rows(uint8_t** rows, int h) {
    int y;
    if (!rows) return;
    for (y = 0; y < h; y++) pluto_free(rows[y]);
    pluto_free(rows);
}

int webp_decode_gray(const uint8_t* data, size_t len, int maxW, int maxH,
                     uint8_t*** outRows, int* outW, int* outH) {
    uint32_t** argb = NULL;
    uint8_t** grid = NULL;
    ScaleAccum* acc = NULL;
    uint8_t* grayRow = NULL;
    int aw = 0, ah = 0;
    int bw, bh, tw, th, count, rc = -1;
    int y, x;

    if (webp_decode_argb(data, len, maxW, maxH, &argb, &aw, &ah) != 0)
        return -1;

    scale_box_sizes(aw, ah, maxW, maxH, &bw, &bh, &tw, &th);
    acc = scale_accum_new(aw, ah, maxW, maxH);
    grayRow = (uint8_t*)pluto_malloc((size_t)(aw ? aw : 1));
    if (!acc || !grayRow) goto cleanup;

    for (y = 0; y < ah; y++) {
        for (x = 0; x < aw; x++) {
            uint32_t px = argb[y][x];
            int a = (int)((px >> 24) & 0xFF);
            int r = (int)((px >> 16) & 0xFF);
            int g = (int)((px >> 8) & 0xFF);
            int b = (int)(px & 0xFF);
            if (a < 255) {
                r = (r * a + 255 * (255 - a)) / 255;
                g = (g * a + 255 * (255 - a)) / 255;
                b = (b * a + 255 * (255 - a)) / 255;
            }
            grayRow[x] = (uint8_t)dither_rgb_to_gray(r, g, b);
        }
        scale_accum_add_row(acc, grayRow);
    }
    count = scale_accum_finish(acc, &tw, &th);
    if (count <= 0) goto cleanup;
    grid = (uint8_t**)pluto_malloc(sizeof(uint8_t*) * (size_t)count);
    if (!grid) goto cleanup;
    for (y = 0; y < count; y++) {
        grid[y] = (uint8_t*)pluto_malloc((size_t)tw);
        if (!grid[y]) { count = y; goto cleanup; }
        for (x = 0; x < tw; x++) grid[y][x] = (uint8_t)acc->out[y][x];
    }
    *outRows = grid;
    *outW = tw;
    *outH = count;
    grid = NULL;
    rc = 0;

cleanup:
    if (grid) {
        for (y = 0; grid[y] && y < count; y++) pluto_free(grid[y]);
        pluto_free(grid);
    }
    pluto_free(grayRow);
    scale_accum_free(acc);
    webp_free_rows_argb(argb, ah);
    return rc;
}

#if defined(PLUTO_WEBP_PD)
typedef struct {
    uint32_t** rows;
    int w, h;
} WebPPixCtx;

static int webp_pix(void* ud, int x, int y) {
    WebPPixCtx* c = (WebPPixCtx*)ud;
    uint32_t px;
    int a, r, g, b;
    if (y >= c->h || c->rows[y] == NULL) return 255;
    if (x >= c->w) return 255;
    px = c->rows[y][x];
    a = (int)((px >> 24) & 0xFF);
    r = (int)((px >> 16) & 0xFF);
    g = (int)((px >> 8) & 0xFF);
    b = (int)(px & 0xFF);
    if (a < 255) {
        r = (r * a + 255 * (255 - a)) / 255;
        g = (g * a + 255 * (255 - a)) / 255;
        b = (b * a + 255 * (255 - a)) / 255;
    }
    return dither_rgb_to_gray(r, g, b);
}

struct LCDBitmap* webp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH) {
    uint32_t** rows = NULL;
    int tw = 0, th = 0;
    WebPPixCtx ctx;
    struct LCDBitmap* img;
    if (webp_decode_argb(data, len, maxW, maxH, &rows, &tw, &th) != 0)
        return NULL;
    ctx.rows = rows;
    ctx.w = tw;
    ctx.h = th;
    img = dither_to_image(pd, webp_pix, &ctx, tw, th);
    webp_free_rows_argb(rows, th);
    return img;
}
#else
struct LCDBitmap* webp_decode(struct PlaydateAPI* pd, const uint8_t* data,
                              size_t len, int maxW, int maxH) {
    (void)pd; (void)data; (void)len; (void)maxW; (void)maxH;
    return NULL;
}
#endif
