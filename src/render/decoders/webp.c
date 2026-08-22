// webp.c — C port of Source/render/decoders/webp.lua (P21: lossless VP8L).
//
// Parses the RIFF container and decodes a VP8L payload into a full-
// resolution ARGB grid (0xAARRGGBB). Covers the little-endian bit reader,
// two-level Huffman tables (rootBits 8, code-length table 7), meta
// Huffman groups, LZ77 with distance mapping, the color cache, and all
// four inverse transforms (predictor, cross-color, subtract-green,
// color-indexing). Lossy VP8 and animation arrive in P22/P23.

#include "render/decoders/webp.h"

#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"
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

/* RIFF container: locate the VP8L chunk payload. */
static int parse_webp(const uint8_t* data, size_t len,
                      const uint8_t** outPayload, size_t* outLen) {
    size_t pos;
    if (len < 20) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return 0;
    pos = 12;
    while (pos + 8 <= len) {
        uint32_t size = (uint32_t)data[pos + 4] |
                        ((uint32_t)data[pos + 5] << 8) |
                        ((uint32_t)data[pos + 6] << 16) |
                        ((uint32_t)data[pos + 7] << 24);
        if (memcmp(data + pos, "VP8L", 4) == 0) {
            size_t avail = len - (pos + 8);
            *outPayload = data + pos + 8;
            *outLen = size < avail ? size : avail;
            return 1;
        }
        pos += 8 + size + (size & 1);
    }
    return 0;
}

int webp_decode_argb(const uint8_t* data, size_t len, int maxW, int maxH,
                     uint32_t*** outRows, int* outW, int* outH) {
    const uint8_t* payload = NULL;
    size_t plen = 0;
    uint32_t bits;
    int w, h;
    Br br;
    VP8LCtx ctx;
    uint32_t* flat = NULL;
    uint32_t* finalPix = NULL;
    uint32_t** grid = NULL;
    int y;

    (void)maxW;
    (void)maxH;
    if (!outRows || !outW || !outH) return -1;
    *outRows = NULL;
    *outW = 0;
    *outH = 0;
    if (!parse_webp(data, len, &payload, &plen)) return -1;
    if (plen < 5 || payload[0] != VP8L_MAGIC_BYTE) return -1;

    bits = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
           ((uint32_t)payload[3] << 16);
    w = (int)(bits & 0x3FFF) + 1;
    h = (int)((bits >> 14) & 0x3FFF) + 1;
    if (((bits >> 22) & 7) != 0) return -1; /* unknown version */
    if (w < 1 || h < 1 || (long long)w * h > WEBP_MAX_PIXELS) return -1;

    memset(&ctx, 0, sizeof(ctx));
    br.data = payload;
    br.len = plen;
    br.pos = 5;
    br.window = 0;
    br.nbits = 0;
    br.eos = 0;

    if (!decode_image_stream(w, h, 1, &br, &ctx, NULL)) goto fail;

    {
        int tw = ctx.transformXsize;
        int th = ctx.transformYsize;
        if (tw < 1 || th < 1 || tw * th > WEBP_MAX_PIXELS) goto fail;
        flat = (uint32_t*)pluto_calloc((size_t)tw * (size_t)th,
                                       sizeof(uint32_t));
        if (!flat) goto fail;
        if (!decode_image_data(&br, &ctx, flat, tw, th)) goto fail;
        if (br.eos) goto fail;

        finalPix = apply_inverse_transforms(&ctx, flat, th);
        flat = NULL; /* consumed (freed or returned) */
        if (!finalPix) goto fail;

        grid = (uint32_t**)pluto_malloc(sizeof(uint32_t*) * (size_t)th);
        if (!grid) goto fail;
        memset(grid, 0, sizeof(uint32_t*) * (size_t)th);
        for (y = 0; y < th; y++) {
            grid[y] = (uint32_t*)pluto_malloc(sizeof(uint32_t) * (size_t)w);
            if (!grid[y]) goto fail;
            memcpy(grid[y], finalPix + (size_t)y * (size_t)w,
                   sizeof(uint32_t) * (size_t)w);
        }
        pluto_free(finalPix);
        ctx_free(&ctx);
        *outRows = grid;
        *outW = w;
        *outH = th;
        return 0;
    }

fail:
    if (grid) {
        for (y = 0; y < h && grid[y]; y++) pluto_free(grid[y]);
        pluto_free(grid);
    }
    pluto_free(finalPix);
    pluto_free(flat);
    ctx_free(&ctx);
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
