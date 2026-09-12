/*
 * PlutoBrowser — webp.c
 * Port of Source/render/decoders/webp.lua (reference, 3052 lines).
 * P27 (this file, part 1): bit reader + Huffman machinery (sub-step a).
 * Later phases append the VP8L/VP8/container sections to this same file,
 * mirroring the reference's single-module layout.
 *
 * Everything here is a verbatim semantic port: the bit reader keeps Lua's
 * 1-based byte positions and its exact eos/latch behavior; the Huffman
 * builder is libwebp's BuildHuffmanTable (two-level, (bits<<16)|value
 * entries) including its error conditions, in the reference's order.
 */
#include <string.h>
#include <stdlib.h>
#include "pd_api.h"
#include "render/decoders/webp.h"
#include "render/decoders/webp-internal.h"
#include "core/logger.h"

extern PlaydateAPI *pluto_pd(void);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_pd()->system->realloc((p), 0)

/* ── Module constants (webp.lua lines 33-69, verbatim values) ────────────── */
const uint8_t webp_code_length_extra_bits[3] = { 2, 3, 7 };
const uint8_t webp_code_length_repeat_offsets[3] = { 3, 3, 11 };
const uint8_t webp_code_length_code_order[WEBP_NUM_CODE_LENGTH_CODES] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
const uint8_t webp_code_to_plane[WEBP_CODE_TO_PLANE_CODES] = {
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
const int webp_alphabet_size[5] = {
    WEBP_NUM_LITERAL_CODES + WEBP_NUM_LENGTH_CODES, /* green + lengths + cache */
    WEBP_NUM_LITERAL_CODES,                         /* red  */
    WEBP_NUM_LITERAL_CODES,                         /* blue */
    WEBP_NUM_LITERAL_CODES,                         /* alpha*/
    WEBP_NUM_DISTANCE_CODES                         /* dist */
};
const uint8_t webp_literal_map[5] = { 0, 1, 1, 1, 0 };

/* ── Little-endian bit reader (webp.lua 75-115) ─────────────────────────────
 * Lua parity notes:
 *  - data:byte(pos) is 1-based; pos == len+1 reads nil in Lua → the loop
 *    condition `pos <= br.len` gives one extra iteration reading past the
 *    last real byte? No: Lua's data:byte(len+1) returns nil and `|` would
 *    error — the reference relies on the while condition ending at pos>len.
 *    The final iteration uses pos == len (the last real byte); after that
 *    pos = len+1 stops the loop. So window fills from real bytes only.
 *  - eos latches once and is never cleared by brAdvance/readBits. */
void webp_br_init(WebPBitReader *br, const uint8_t *data, int len, int startPos)
{
    br->data = data;
    br->len = len;
    br->pos = startPos; /* 1-based, exactly like Lua */
    br->window = 0;
    br->nbits = 0;
    br->eos = 0;
}

uint32_t webp_br_prefetch(WebPBitReader *br, int want)
{
    uint32_t w = br->window;
    int nb = br->nbits;
    int pos = br->pos;
    const uint8_t *data = br->data;
    while (nb < want && pos <= br->len)
    {
        w |= (uint32_t)data[pos - 1] << nb; /* data:byte(pos), 1-based */
        pos++;
        nb += 8;
    }
    br->window = w;
    br->nbits = nb;
    br->pos = pos;
    return w & ((want >= 32) ? 0xFFFFFFFFu : ((1u << want) - 1u));
}

void webp_br_advance(WebPBitReader *br, int n)
{
    br->nbits -= n;
    br->window >>= n;
    if (br->nbits < 0)
    {
        br->eos = 1;
        br->nbits = 0;
    }
}

uint32_t webp_br_read_bits(WebPBitReader *br, int n)
{
    if (n == 0)
    {
        return 0;
    }
    uint32_t v = webp_br_prefetch(br, n);
    if (br->nbits < n)
    {
        br->eos = 1;
        return 0;
    }
    webp_br_advance(br, n);
    return v;
}

/* ── Symbol reading (webp.lua 117-150) ──────────────────────────────────────
 * Tables are flat arrays of (bits << 16) | value, two-level like libwebp. */
uint32_t webp_read_symbol(const uint32_t *table, WebPBitReader *br)
{
    uint32_t val = webp_br_prefetch(br, 16);
    uint32_t low = val & WEBP_HUFFMAN_TABLE_MASK;
    uint32_t entry = table[low];
    int nbits = (int)(entry >> 16) - WEBP_HUFFMAN_TABLE_BITS;
    if (nbits > 0)
    {
        webp_br_advance(br, WEBP_HUFFMAN_TABLE_BITS);
        uint32_t val2 = webp_br_prefetch(br, nbits);
        entry = table[low + (entry & 0xFFFFu) + (val2 & ((1u << nbits) - 1u))];
        webp_br_advance(br, (int)(entry >> 16));
    }
    else
    {
        webp_br_advance(br, (int)(entry >> 16));
    }
    return entry & 0xFFFFu;
}

uint32_t webp_read_symbol7(const uint32_t *table, WebPBitReader *br)
{
    uint32_t val = webp_br_prefetch(br, 15);
    uint32_t low = val & WEBP_LENGTHS_TABLE_MASK;
    uint32_t entry = table[low];
    int nbits = (int)(entry >> 16) - WEBP_LENGTHS_TABLE_BITS;
    if (nbits > 0)
    {
        webp_br_advance(br, WEBP_LENGTHS_TABLE_BITS);
        uint32_t val2 = webp_br_prefetch(br, nbits);
        entry = table[low + (entry & 0xFFFFu) + (val2 & ((1u << nbits) - 1u))];
        webp_br_advance(br, (int)(entry >> 16));
    }
    else
    {
        webp_br_advance(br, (int)(entry >> 16));
    }
    return entry & 0xFFFFu;
}

/* ── Huffman table construction (webp.lua 152-276; libwebp huffman_utils.c) ── */

/* replicateValue: table[base+i*step] = code for i = end/step-1 .. 0.
 * Lua uses 1-based table indices via base+currentEnd; here the table is a
 * plain C array and base/currentEnd are 0-based entry indices. The Lua code
 * indexes table[base + currentEnd] where currentEnd counts down by step —
 * with Lua 1-based tables this writes entries base+end-step .. base+1... the
 * C port reproduces the same POSITIONS the Lua writes reach, because both
 * use identical arithmetic on the logical index space (the C table slot for
 * logical Lua index k is C index k-1; calls pass base/key already in the
 * logical space used by the reference's readSymbol indexing, which indexes
 * table[val & MASK] with 0-based val — the reference's builder therefore
 * writes logical index base+currentEnd-1 ... i.e. C slot base+currentEnd-1+?
 * ... Resolved by construction in webp_replicate: see comment below. */
static void webp_replicate(uint32_t *table, uint32_t base, uint32_t step,
                           uint32_t end_, uint32_t code)
{
    /* Lua: currentEnd = end_; repeat currentEnd -= step; table[base+currentEnd]=code
     * until currentEnd <= 0. Lua table indices are 1-based, so logical slot
     * (base+currentEnd) in Lua == C index (base+currentEnd-1). Calls in the
     * reference pass base=key (0-based logical key) and end_=tableSize where
     * the read side indexes table[val & MASK] (0-based, val < 2^8) — meaning
     * the reference's table[0] holds the entry for code 0. Lua's table[0] is
     * valid (Lua arrays are 1-based but 0 is writable), and readSymbol reads
     * table[low] with low in [0,255] — so the builder's writes at Lua index
     * base+currentEnd land on the same numeric index the reader uses. The C
     * array therefore writes C index (base+currentEnd) directly. */
    uint32_t currentEnd = end_;
    do
    {
        currentEnd -= step;
        table[base + currentEnd] = code;
    } while (currentEnd > 0);
}

static uint32_t webp_get_next_key(uint32_t key, int len)
{
    uint32_t step = 1u << (len - 1);
    while ((key & step) != 0)
    {
        step >>= 1;
    }
    if (step == 0)
    {
        return key;
    }
    return (key & (step - 1)) + step;
}

static int webp_next_table_bit_size(const uint16_t *count, int len, int rootBits)
{
    int left = 1 << (len - rootBits);
    while (len < WEBP_MAX_ALLOWED_CODE_LENGTH)
    {
        left -= count[len];
        if (left <= 0)
        {
            break;
        }
        len++;
        left <<= 1;
    }
    return len - rootBits;
}

int webp_build_huffman_table(const uint8_t *codeLengths, int codeLengthsSize,
                             int rootBits, uint32_t *out_table)
{
    uint32_t totalSize = 1u << rootBits;
    uint16_t count[WEBP_MAX_ALLOWED_CODE_LENGTH + 1];
    uint16_t offset[WEBP_MAX_ALLOWED_CODE_LENGTH + 1];
    int cl;

    memset(count, 0, sizeof(count));
    memset(offset, 0, sizeof(offset));

    for (int symbol = 0; symbol < codeLengthsSize; symbol++)
    {
        cl = codeLengths[symbol];
        if (cl > WEBP_MAX_ALLOWED_CODE_LENGTH)
        {
            return 0;
        }
        count[cl]++;
    }
    if (count[0] == codeLengthsSize)
    {
        return 0;
    }

    offset[1] = 0;
    for (int len = 1; len < WEBP_MAX_ALLOWED_CODE_LENGTH; len++)
    {
        if (count[len] > (1u << len))
        {
            return 0;
        }
        offset[len + 1] = offset[len] + count[len];
    }

    /* sorted[] — Lua: sorted[offset[cl]] = symbol (0-based numeric index).
     * Bound: the total number of nonzero-length symbols is <= codeLengthsSize
     * and offset[] stays < codeLengthsSize (checked below), so a scratch of
     * codeLengthsSize entries suffices; cap at the max alphabet (280+cache).
     * The largest caller alphabet is 280 + 2048 (color cache) — allocate via
     * PLUTO_MALLOC to keep the stack shallow (P22 rule). */
    uint32_t *sorted = (uint32_t *)PLUTO_MALLOC((size_t)codeLengthsSize * sizeof(uint32_t));
    if (!sorted)
    {
        return 0;
    }
    for (int symbol = 0; symbol < codeLengthsSize; symbol++)
    {
        cl = codeLengths[symbol];
        if (cl > 0)
        {
            if (offset[cl] >= codeLengthsSize)
            {
                PLUTO_FREE(sorted);
                return 0;
            }
            sorted[offset[cl]] = (uint32_t)symbol;
            offset[cl]++;
        }
    }

    /* Single-symbol code: fill the whole root table with sorted[0].
     * Reference parity: the stored code is sorted[0] RAW (no (len<<16)
     * bits field) — readSymbol then advances 0 bits and always returns
     * that symbol. Degenerate but exactly what the reference does. */
    if (offset[WEBP_MAX_ALLOWED_CODE_LENGTH] == 1)
    {
        webp_replicate(out_table, 0, 1, totalSize, sorted[0]);
        PLUTO_FREE(sorted);
        return (int)totalSize;
    }

    uint32_t low = 0xFFFFFFFFu;
    uint32_t mask = totalSize - 1;
    uint32_t key = 0;
    uint32_t numNodes = 1;
    uint32_t numOpen = 1;
    uint32_t symbol = 0;
    int tableBits = rootBits;
    uint32_t tableSize = 1u << tableBits;

    /* Root table fill. */
    for (int len = 1; len <= rootBits; len++)
    {
        numOpen <<= 1;
        numNodes += numOpen;
        numOpen -= count[len];
        if ((int)numOpen < 0)
        {
            PLUTO_FREE(sorted);
            return 0;
        }
        uint32_t step = 2u << (len - 1);
        while (count[len] > 0)
        {
            count[len]--;
            uint32_t code = ((uint32_t)len << 16) | sorted[symbol];
            symbol++;
            webp_replicate(out_table, key, step, tableSize, code);
            key = webp_get_next_key(key, len);
        }
    }

    /* Second-level tables. */
    uint32_t tablePos = 0;
    for (int len = rootBits + 1; len <= WEBP_MAX_ALLOWED_CODE_LENGTH; len++)
    {
        numOpen <<= 1;
        numNodes += numOpen;
        numOpen -= count[len];
        if ((int)numOpen < 0)
        {
            PLUTO_FREE(sorted);
            return 0;
        }
        uint32_t step = 2u << (len - rootBits - 1);
        while (count[len] > 0)
        {
            count[len]--;
            if ((key & mask) != low)
            {
                tablePos += tableSize;
                tableBits = webp_next_table_bit_size(count, len, rootBits);
                tableSize = 1u << tableBits;
                totalSize += tableSize;
                low = key & mask;
                out_table[low] = (((uint32_t)tableBits + (uint32_t)rootBits) << 16)
                                 | (tablePos - low);
            }
            uint32_t code = ((uint32_t)(len - rootBits) << 16) | sorted[symbol];
            symbol++;
            webp_replicate(out_table, tablePos + (key >> rootBits), step,
                           tableSize, code);
            key = webp_get_next_key(key, len);
        }
    }

    PLUTO_FREE(sorted);
    if (numNodes != 2u * offset[WEBP_MAX_ALLOWED_CODE_LENGTH] - 1u)
    {
        return 0;
    }
    return (int)totalSize;
}

/* ── Code-length code / Huffman code reading (webp.lua 277-350) ──────────── */
int webp_read_huffman_code_lengths(WebPBitReader *br,
                                   const uint8_t *codeLengthCodeLengths,
                                   int numSymbols, uint8_t *codeLengths,
                                   uint32_t *lengths_table_scratch)
{
    if (!webp_build_huffman_table(codeLengthCodeLengths, WEBP_NUM_CODE_LENGTH_CODES,
                                  WEBP_LENGTHS_TABLE_BITS, lengths_table_scratch))
    {
        return 0;
    }

    int maxSymbol;
    if (webp_br_read_bits(br, 1) == 1)
    {
        int lengthNbits = 2 + 2 * (int)webp_br_read_bits(br, 3);
        maxSymbol = 2 + (int)webp_br_read_bits(br, lengthNbits);
        if (maxSymbol > numSymbols)
        {
            return 0;
        }
    }
    else
    {
        maxSymbol = numSymbols;
    }

    int symbol = 0;
    int prevCodeLen = WEBP_DEFAULT_CODE_LENGTH;
    while (symbol < numSymbols)
    {
        if (maxSymbol == 0)
        {
            break;
        }
        maxSymbol--;
        uint32_t val = webp_br_prefetch(br, 15);
        if (br->nbits < WEBP_LENGTHS_TABLE_BITS)
        {
            br->eos = 1;
            return 0;
        }
        uint32_t p = lengths_table_scratch[val & WEBP_LENGTHS_TABLE_MASK];
        webp_br_advance(br, (int)(p >> 16));
        int codeLen = (int)(p & 0xFFFFu);
        if (codeLen < WEBP_K_CODE_LENGTH_LITERALS)
        {
            codeLengths[symbol] = (uint8_t)codeLen;
            symbol++;
            if (codeLen != 0)
            {
                prevCodeLen = codeLen;
            }
        }
        else
        {
            int usePrev = (codeLen == WEBP_K_CODE_LENGTH_REPEAT_CODE);
            int slot = codeLen - WEBP_K_CODE_LENGTH_LITERALS;
            int repeatCount = (int)webp_br_read_bits(
                                  br, webp_code_length_extra_bits[slot])
                              + webp_code_length_repeat_offsets[slot];
            if (symbol + repeatCount > numSymbols)
            {
                return 0;
            }
            int length = usePrev ? prevCodeLen : 0;
            for (int i = 0; i < repeatCount; i++)
            {
                codeLengths[symbol] = (uint8_t)length;
                symbol++;
            }
        }
    }
    return 1;
}

int webp_read_huffman_code(int alphabetSize, WebPBitReader *br,
                           uint8_t *scratch_code_lengths, uint32_t *out_table)
{
    static uint8_t codeLengthCodeLengths[WEBP_NUM_CODE_LENGTH_CODES];
    static uint32_t lengths_table[1 << WEBP_LENGTHS_TABLE_BITS];

    memset(scratch_code_lengths, 0, (size_t)alphabetSize);
    if (webp_br_read_bits(br, 1) == 1)
    {
        /* Simple code path.
         * Reference parity: Lua writes codeLengths[symbol] = 1 even when
         * symbol >= alphabetSize (the Lua table just grows); buildHuffman-
         * Table only reads [0, alphabetSize), so such a write is silently
         * DROPPED. Reproduce that: mask, never error. */
        int numSymbols = (int)webp_br_read_bits(br, 1) + 1;
        int firstSymbolLenCode = (int)webp_br_read_bits(br, 1);
        int symbol = (int)webp_br_read_bits(br, (firstSymbolLenCode == 0) ? 1 : 8);
        if (symbol < alphabetSize)
        {
            scratch_code_lengths[symbol] = 1;
        }
        if (numSymbols == 2)
        {
            symbol = (int)webp_br_read_bits(br, 8);
            if (symbol < alphabetSize)
            {
                scratch_code_lengths[symbol] = 1;
            }
        }
    }
    else
    {
        int numCodes = (int)webp_br_read_bits(br, 4) + 4;
        memset(codeLengthCodeLengths, 0, sizeof(codeLengthCodeLengths));
        for (int i = 0; i < numCodes; i++)
        {
            codeLengthCodeLengths[webp_code_length_code_order[i]]
                = (uint8_t)webp_br_read_bits(br, 3);
        }
        if (!webp_read_huffman_code_lengths(br, codeLengthCodeLengths,
                                            alphabetSize, scratch_code_lengths,
                                            lengths_table))
        {
            return 0;
        }
    }
    if (br->eos)
    {
        return 0;
    }
    return webp_build_huffman_table(scratch_code_lengths, alphabetSize,
                                    WEBP_HUFFMAN_TABLE_BITS, out_table);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P28 — VP8L lossless decoding (webp.lua 351-1006, sub-steps b+c).
 * Verbatim semantic port: readHuffmanCodes / LZ77 / color cache /
 * decodeImageData / predictor machinery / color transforms / color-indexing
 * (incl. expandColorMap) / decodeImageStream / applyInverseTransforms /
 * decodeVP8LPayload. All pixel buffers are heap; Lua's 1-based numeric
 * indices are kept as 0-based C indices (readSymbol already uses the same
 * logical index space as the reference).
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "render/decoders/scale.h"
#include "render/decoders/dither.h"
#include "core/tasks.h"

/* ── Meta Huffman codes / htree groups (webp.lua 351-439) ────────────────── */
typedef struct WebPHtreeGroup
{
    uint32_t *tables[5];      /* per-stream Huffman tables (heap) */
    int isTrivialLiteral;
    int isTrivialCode;
    uint32_t literalArb;      /* (a<<24)|(r<<16)|b (no green) */
    uint32_t literalArgb;     /* full ARGB for isTrivialCode */
} WebPHtreeGroup;

#define WEBP_MAX_HTREE_GROUPS 1008
static WebPHtreeGroup *g_htreeGroups = NULL; /* heap array, numHtreeGroupsMax */
static int g_numHtreeGroupsMax = 0;

/* Free every group's five tables (NULL-safe), then the group array itself. */
static void vp8l_groups_free(void)
{
    if (g_htreeGroups)
    {
        for (int gi = 0; gi < g_numHtreeGroupsMax; gi++)
        {
            for (int j = 0; j < 5; j++)
            {
                PLUTO_FREE(g_htreeGroups[gi].tables[j]);
                g_htreeGroups[gi].tables[j] = NULL;
            }
        }
    }
    PLUTO_FREE(g_htreeGroups);
    g_htreeGroups = NULL;
    g_numHtreeGroupsMax = 0;
}



static uint32_t *vp8l_decode_image_stream(int xsize, int ysize, int isLevel0,
                                          WebPBitReader *br, WebPDecodeCtx *ctx);
static uint32_t *vp8l_read_huffman_code_table(int alphabetSize, WebPBitReader *br,
                                              uint8_t *scratch_code_lengths);

static void vp8l_ctx_free(WebPDecodeCtx *ctx);
void vp8l_ctx_free_pub(WebPDecodeCtx *ctx) { vp8l_ctx_free(ctx); }
static void vp8l_ctx_free(WebPDecodeCtx *ctx)
{
    PLUTO_FREE(ctx->huffmanImage);
    ctx->huffmanImage = NULL;
    PLUTO_FREE(ctx->colorCacheColors);
    ctx->colorCacheColors = NULL;
    for (int i = 0; i < ctx->numTransforms; i++)
    {
        PLUTO_FREE(ctx->transforms[i].data);
        ctx->transforms[i].data = NULL;
    }
    ctx->numTransforms = 0;
}

/* Build a Huffman table for one meta-code stream. Returns a heap table
 * sized to the exact used entry count (0 on error). buildHuffmanTable
 * writes [0..totalSize); the buffer is shrunk to actual usage. */
static uint32_t *vp8l_read_huffman_code_table(int alphabetSize, WebPBitReader *br,
                                              uint8_t *scratch_code_lengths)
{
    static uint32_t big[WEBP_HUFFMAN_TABLE_MAX]; /* 128KB static, not stack */
    memset(big, 0, sizeof(big));
    /* buildHuffmanTable writes exactly [0, totalSize) entries and returns
     * totalSize (>0) — or 0 on error. Use the returned size directly; a
     * valid table may be all zeros (single-symbol code for symbol 0), so a
     * nonzero-entry scan would wrongly reject it. */
    int used = webp_read_huffman_code(alphabetSize, br, scratch_code_lengths, big);
    if (used <= 0)
    {
        return NULL;
    }
    uint32_t *out = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)used);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, big, sizeof(uint32_t) * (size_t)used);
    return out;
}

/* ── readHuffmanCodes (webp.lua 351-439) ──────────────────────────────────── */
static int vp8l_read_huffman_codes(WebPBitReader *br, int xsize, int ysize,
                                         int colorCacheBits, int allowRecursion,
                                         WebPDecodeCtx *ctx)
{
    uint32_t *huffmanImage = NULL;
    int numHtreeGroups = 1;
    int numHtreeGroupsMax = 1;
    int8_t *mapping = NULL; /* -1 = unmapped; only allocated in the remap case */

    if (allowRecursion && webp_br_read_bits(br, 1) == 1)
    {
        int huffmanPrecision = WEBP_MIN_HUFFMAN_BITS + (int)webp_br_read_bits(br, WEBP_NUM_HUFFMAN_BITS);
        int huffmanXsize = (xsize + (1 << huffmanPrecision) - 1) >> huffmanPrecision;
        int huffmanYsize = (ysize + (1 << huffmanPrecision) - 1) >> huffmanPrecision;
        int huffmanPixs = huffmanXsize * huffmanYsize;
        WebPDecodeCtx subCtx;
        memset(&subCtx, 0, sizeof(subCtx));
        uint32_t *sub = vp8l_decode_image_stream(huffmanXsize, huffmanYsize, 0, br, &subCtx);
        if (!sub)
        {
            return 0;
        }
        vp8l_ctx_free(&subCtx);
        huffmanImage = sub; /* takes ownership of the sub-stream pixel array */
        ctx->huffmanSubsampleBits = huffmanPrecision;
        for (int i = 0; i < huffmanPixs; i++)
        {
            uint32_t group = (huffmanImage[i] >> 8) & 0xFFFFu;
            huffmanImage[i] = group;
            if ((int)group >= numHtreeGroupsMax)
            {
                numHtreeGroupsMax = (int)group + 1;
            }
        }
        if (numHtreeGroupsMax > 1000 || numHtreeGroupsMax > xsize * ysize)
        {
            mapping = (int8_t *)PLUTO_MALLOC((size_t)numHtreeGroupsMax);
            if (!mapping)
            {
                PLUTO_FREE(huffmanImage);
                return 0;
            }
            memset(mapping, -1, (size_t)numHtreeGroupsMax);
            numHtreeGroups = 0;
            for (int i = 0; i < huffmanPixs; i++)
            {
                int g = (int)huffmanImage[i];
                int mapped = mapping[g];
                if (mapped == -1)
                {
                    mapping[g] = (int8_t)numHtreeGroups;
                    mapped = numHtreeGroups;
                    numHtreeGroups++;
                }
                huffmanImage[i] = (uint32_t)mapped;
            }
        }
        else
        {
            numHtreeGroups = numHtreeGroupsMax;
        }
    }

    if (br->eos)
    {
        PLUTO_FREE(huffmanImage);
        PLUTO_FREE(mapping);
        return 0;
    }

    /* groups: allocate exactly numHtreeGroupsMax group slots. mapping maps
     * original group index -> compact index (or -1 = "codes present but not
     * instantiated"). htrees[gi] may be NULL (compact/unmapped slots). */
    vp8l_groups_free();
    g_htreeGroups = (WebPHtreeGroup *)PLUTO_MALLOC(
        sizeof(WebPHtreeGroup) * (size_t)(numHtreeGroupsMax > 0 ? numHtreeGroupsMax : 1));
    if (!g_htreeGroups)
    {
        PLUTO_FREE(huffmanImage);
        PLUTO_FREE(mapping);
        return 0;
    }
    g_numHtreeGroupsMax = numHtreeGroupsMax;
    memset(g_htreeGroups, 0, sizeof(WebPHtreeGroup) * (size_t)numHtreeGroupsMax);

    uint8_t *codeLengths = (uint8_t *)PLUTO_MALLOC(280 + 2048 + 16);
    if (!codeLengths)
    {
        PLUTO_FREE(huffmanImage);
        PLUTO_FREE(mapping);
        return 0;
    }

    for (int i = 0; i < numHtreeGroupsMax; i++)
    {
        if (mapping && mapping[i] == -1)
        {
            /* Group never referenced by the huffman image: its 5 codes are
             * still read from the stream (and discarded) — verbatim parity. */
            for (int j = 0; j < WEBP_HUFFMAN_CODES_PER_META; j++)
            {
                int alphaSize = webp_alphabet_size[j];
                if (j == 0 && colorCacheBits > 0)
                {
                    alphaSize += (1 << colorCacheBits);
                }
                uint32_t *t = vp8l_read_huffman_code_table(alphaSize, br, codeLengths);
                if (!t)
                {
                    PLUTO_FREE(codeLengths);
                    PLUTO_FREE(huffmanImage);
                    PLUTO_FREE(mapping);
                    return 0;
                }
                PLUTO_FREE(t);
            }
            continue;
        }
        int gi = (mapping == NULL) ? i : mapping[i];
        WebPHtreeGroup *group = &g_htreeGroups[gi];
        memset(group, 0, sizeof(*group));
        int totalSize = 0;
        int isTrivialLiteral = 1;
        for (int j = 0; j < WEBP_HUFFMAN_CODES_PER_META; j++)
        {
            int alphaSize = webp_alphabet_size[j];
            if (j == 0 && colorCacheBits > 0)
            {
                alphaSize += (1 << colorCacheBits);
            }
            uint32_t *t = vp8l_read_huffman_code_table(alphaSize, br, codeLengths);
            if (!t)
            {
                PLUTO_FREE(codeLengths);
                PLUTO_FREE(huffmanImage);
                PLUTO_FREE(mapping);
                return 0;
            }
            group->tables[j] = t;
            int firstBits = (int)(t[0] >> 16);
            totalSize += firstBits;
            if (webp_literal_map[j] == 1 && firstBits != 0)
            {
                isTrivialLiteral = 0;
            }
        }
        if (isTrivialLiteral)
        {
            uint32_t red   = group->tables[1][0] & 0xFFFFu;
            uint32_t blue  = group->tables[2][0] & 0xFFFFu;
            uint32_t alpha = group->tables[3][0] & 0xFFFFu;
            group->literalArb = (alpha << 24) | (red << 16) | blue;
            if (totalSize == 0)
            {
                uint32_t greenVal = group->tables[0][0] & 0xFFFFu;
                if (greenVal < WEBP_NUM_LITERAL_CODES)
                {
                    group->isTrivialCode = 1;
                    group->literalArgb = group->literalArb | (greenVal << 8);
                }
            }
        }
    }

    PLUTO_FREE(codeLengths);
    ctx->huffmanImage = huffmanImage;
    ctx->numHtreeGroups = numHtreeGroups;
    (void)ysize;
    /* Reference returns true/false — NOT the huffmanImage pointer (which is
     * legitimately NULL for single-group streams without a meta image). */
    return 1;
}

/* ── LZ77 helpers + color cache (webp.lua 442-485) ────────────────────────── */
static int vp8l_get_copy_distance(int distanceSymbol, WebPBitReader *br)
{
    if (distanceSymbol < 4)
    {
        return distanceSymbol + 1;
    }
    int extraBits = (distanceSymbol - 2) >> 1;
    int offset = (2 + (distanceSymbol & 1)) << extraBits;
    return offset + (int)webp_br_read_bits(br, extraBits) + 1;
}

static int vp8l_plane_code_to_distance(int xsize, int planeCode)
{
    if (planeCode > WEBP_CODE_TO_PLANE_CODES)
    {
        return planeCode - WEBP_CODE_TO_PLANE_CODES;
    }
    /* Lua kCodeToPlane is 1-based (indices 1..120); the transcribed C array
     * is 0-based, so index planeCode-1. (planeCode >= 1 always: both callers
     * produce >= 1.) */
    int distCode = webp_code_to_plane[planeCode - 1];
    int yoffset = distCode >> 4;
    int xoffset = 8 - (distCode & 0xF);
    int dist = yoffset * xsize + xoffset;
    if (dist < 1)
    {
        return 1;
    }
    return dist;
}

#define WEBP_K_HASH_MUL 0x1e35a7bdu

static uint32_t vp8l_cache_key(const WebPDecodeCtx *ctx, uint32_t argb)
{
    /* Reference parity: ((argb * kHashMul) & 0xFFFFFFFF) >> hashShift — the
     * product is masked to 32 bits BEFORE the shift, otherwise the high
     * product bits leak into the key and it exceeds the cache size. */
    uint32_t prod = (uint32_t)((uint64_t)argb * WEBP_K_HASH_MUL);
    return prod >> ctx->colorCacheHashShift;
}

/* ── decodeImageData (webp.lua 487-607) ───────────────────────────────────── */
static int vp8l_decode_image_data(WebPBitReader *br, WebPDecodeCtx *ctx,
                                  uint32_t *data, int width, int height);
int vp8l_decode_image_data_pub(WebPBitReader *br, WebPDecodeCtx *ctx,
                               uint32_t *data, int width, int height)
{
    return vp8l_decode_image_data(br, ctx, data, width, height);
}
static int vp8l_decode_image_data(WebPBitReader *br, WebPDecodeCtx *ctx,
                                  uint32_t *data, int width, int height)
{
    int row = 0, col = 0;
    int src = 0;
    int lenCodeLimit = WEBP_NUM_LITERAL_CODES + WEBP_NUM_LENGTH_CODES;
    int colorCacheLimit = lenCodeLimit + ctx->colorCacheSize;
    int srcEnd = width * height;
    int huffmanBits = ctx->huffmanSubsampleBits;
    const uint32_t *huffmanImage = ctx->huffmanImage;
    int huffmanXsize = ctx->huffmanXsize;
    uint32_t mask = (huffmanBits == 0) ? 0xFFFFFFFFu : (1u << huffmanBits) - 1u;
    WebPHtreeGroup *group = NULL;
    int lastCached = 0;

    while (src < srcEnd)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        if ((col & mask) == 0)
        {
            int metaIndex = 0;
            if (huffmanBits != 0)
            {
                metaIndex = (int)huffmanImage[huffmanXsize * (row >> huffmanBits)
                                              + (col >> huffmanBits)];
            }
            group = &g_htreeGroups[metaIndex];
        }
        if (group->isTrivialCode)
        {
            data[src++] = group->literalArgb;
            if (++col >= width)
            {
                col = 0;
                row++;
            }
            if (ctx->colorCacheColors)
            {
                while (lastCached < src)
                {
                    ctx->colorCacheColors[vp8l_cache_key(ctx, data[lastCached])] = data[lastCached];
                    lastCached++;
                }
            }
        }
        else
        {
            uint32_t code = webp_read_symbol(group->tables[0], br);
            if (code < WEBP_NUM_LITERAL_CODES)
            {
                uint32_t pixel;
                if (group->isTrivialLiteral)
                {
                    pixel = group->literalArb | (code << 8);
                }
                else
                {
                    uint32_t red = webp_read_symbol(group->tables[1], br);
                    uint32_t blue = webp_read_symbol(group->tables[2], br);
                    uint32_t alpha = webp_read_symbol(group->tables[3], br);
                    pixel = (alpha << 24) | (red << 16) | (code << 8) | blue;
                }
                data[src++] = pixel;
                if (++col >= width)
                {
                    col = 0;
                    row++;
                }
                if (ctx->colorCacheColors)
                {
                    while (lastCached < src)
                    {
                        ctx->colorCacheColors[vp8l_cache_key(ctx, data[lastCached])] = data[lastCached];
                        lastCached++;
                    }
                }
            }
            else if (code < (uint32_t)lenCodeLimit)
            {
                int length = vp8l_get_copy_distance((int)code - WEBP_NUM_LITERAL_CODES, br);
                int distSymbol = (int)webp_read_symbol(group->tables[4], br);
                int dist = vp8l_plane_code_to_distance(width, vp8l_get_copy_distance(distSymbol, br));
                if (src - dist < 0 || src + length > srcEnd)
                {
                    return 0;
                }
                for (int i = 0; i < length; i++)
                {
                    data[src + i] = data[src + i - dist];
                }
                src += length;
                col += length;
                while (col >= width)
                {
                    col -= width;
                    row++;
                }
                if ((col & mask) != 0)
                {
                    int metaIndex = 0;
                    if (huffmanBits != 0)
                    {
                        metaIndex = (int)huffmanImage[huffmanXsize * (row >> huffmanBits)
                                                      + (col >> huffmanBits)];
                    }
                    group = &g_htreeGroups[metaIndex];
                }
                if (ctx->colorCacheColors)
                {
                    while (lastCached < src)
                    {
                        ctx->colorCacheColors[vp8l_cache_key(ctx, data[lastCached])] = data[lastCached];
                        lastCached++;
                    }
                }
            }
            else if (code < (uint32_t)colorCacheLimit)
            {
                if (ctx->colorCacheColors)
                {
                    while (lastCached < src)
                    {
                        ctx->colorCacheColors[vp8l_cache_key(ctx, data[lastCached])] = data[lastCached];
                        lastCached++;
                    }
                }
                data[src++] = ctx->colorCacheColors[code - (uint32_t)lenCodeLimit];
                if (++col >= width)
                {
                    col = 0;
                    row++;
                }
                if (ctx->colorCacheColors)
                {
                    while (lastCached < src)
                    {
                        ctx->colorCacheColors[vp8l_cache_key(ctx, data[lastCached])] = data[lastCached];
                        lastCached++;
                    }
                }
            }
            else
            {
                return 0;
            }
        }
        if (br->eos)
        {
            return 0;
        }
    }
    if (br->eos)
    {
        return 0;
    }
    return 1;
}

/* ── Inverse transform pixel helpers (webp.lua 610-693) ───────────────────── */
static uint32_t vp8l_add_pixels(uint32_t a, uint32_t b)
{
    uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u);
    uint32_t rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
    return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
}

static uint32_t vp8l_average2(uint32_t a0, uint32_t a1)
{
    return (((a0 ^ a1) & 0xFEFEFEFEu) >> 1) + (a0 & a1);
}

static uint32_t vp8l_average3(uint32_t a0, uint32_t a1, uint32_t a2)
{
    return vp8l_average2(vp8l_average2(a0, a2), a1);
}

static uint32_t vp8l_average4(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return vp8l_average2(vp8l_average2(a0, a1), vp8l_average2(a2, a3));
}

static uint32_t vp8l_clip255(uint32_t a)
{
    if (a < 256)
    {
        return a;
    }
    return (~a >> 24) & 0xFFu;
}

static int vp8l_div2trunc(int x)
{
    if (x >= 0)
    {
        return x >> 1;
    }
    return -((-x) >> 1);
}

static int vp8l_sub3(int a, int b, int c)
{
    return (b - c < 0 ? -(b - c) : b - c) - (a - c < 0 ? -(a - c) : a - c);
}

static uint32_t vp8l_select(int a, int b, int c)
{
    int paMinusPb =
        vp8l_sub3((a >> 24) & 0xFF, (b >> 24) & 0xFF, (c >> 24) & 0xFF) +
        vp8l_sub3((a >> 16) & 0xFF, (b >> 16) & 0xFF, (c >> 16) & 0xFF) +
        vp8l_sub3((a >> 8) & 0xFF, (b >> 8) & 0xFF, (c >> 8) & 0xFF) +
        vp8l_sub3(a & 0xFF, b & 0xFF, c & 0xFF);
    if (paMinusPb <= 0)
    {
        return a;
    }
    return b;
}

static int vp8l_add_sub_half(int a, int b)
{
    return (int)vp8l_clip255((uint32_t)(a + vp8l_div2trunc(a - b)));
}

static uint32_t vp8l_clamped_add_subtract_full(uint32_t c0, uint32_t c1, uint32_t c2)
{
    uint32_t a = vp8l_clip255(((c0 >> 24) & 0xFF) + ((c1 >> 24) & 0xFF) - ((c2 >> 24) & 0xFF));
    uint32_t r = vp8l_clip255(((c0 >> 16) & 0xFF) + ((c1 >> 16) & 0xFF) - ((c2 >> 16) & 0xFF));
    uint32_t g = vp8l_clip255(((c0 >> 8) & 0xFF) + ((c1 >> 8) & 0xFF) - ((c2 >> 8) & 0xFF));
    uint32_t b = vp8l_clip255((c0 & 0xFF) + (c1 & 0xFF) - (c2 & 0xFF));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t vp8l_clamped_add_subtract_half(uint32_t c0, uint32_t c1, uint32_t c2)
{
    uint32_t ave = vp8l_average2(c0, c1);
    uint32_t a = (uint32_t)vp8l_add_sub_half((ave >> 24) & 0xFF, (c2 >> 24) & 0xFF);
    uint32_t r = (uint32_t)vp8l_add_sub_half((ave >> 16) & 0xFF, (c2 >> 16) & 0xFF);
    uint32_t g = (uint32_t)vp8l_add_sub_half((ave >> 8) & 0xFF, (c2 >> 8) & 0xFF);
    uint32_t b = (uint32_t)vp8l_add_sub_half(ave & 0xFF, c2 & 0xFF);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* predictor row-add (webp.lua 697-706). out points at the row start;
 * px indexes are absolute (0-based) exactly like the Lua numerics. */
static void vp8l_predictor_add(int mode, const uint32_t *in_, int iIn,
                               uint32_t *out, int oOut, int x, int count, int width)
{
    for (int i = 0; i < count; i++)
    {
        int px = oOut + x + i;
        int inx = iIn + x + i;
        uint32_t left = out[px - 1];
        uint32_t top = out[px - width];
        uint32_t top1 = out[px - width + 1];
        uint32_t topm1 = out[px - width - 1];
        uint32_t pred;
        switch (mode)
        {
        case 0:  pred = WEBP_ARGB_BLACK; break;
        case 1:  pred = left; break;
        case 2:  pred = top; break;
        case 3:  pred = top1; break;
        case 4:  pred = topm1; break;
        case 5:  pred = vp8l_average3(left, top, top1); break;
        case 6:  pred = vp8l_average2(left, topm1); break;
        case 7:  pred = vp8l_average2(left, top); break;
        case 8:  pred = vp8l_average2(topm1, top); break;
        case 9:  pred = vp8l_average2(top, top1); break;
        case 10: pred = vp8l_average4(left, topm1, top, top1); break;
        case 11: pred = vp8l_select(top, left, topm1); break;
        case 12: pred = vp8l_clamped_add_subtract_full(left, top, topm1); break;
        default: pred = vp8l_clamped_add_subtract_half(left, top, topm1); break;
        }
        out[px] = vp8l_add_pixels(in_[inx], pred);
    }
}

static int vp8l_sub_sample_size(int v, int n)
{
    return (v + (1 << n) - 1) >> n;
}

/* ── predictorInverse (webp.lua 708-741) ──────────────────────────────────── */
static void vp8l_predictor_inverse(const WebPTransform *t, const uint32_t *in_,
                                   uint32_t *out, int rows)
{
    int width = t->xsize;
    int tileWidth = 1 << t->bits;
    int mask = tileWidth - 1;
    int tilesPerRow = vp8l_sub_sample_size(width, t->bits);
    const uint32_t *tdata = t->data;

    out[0] = vp8l_add_pixels(in_[0], WEBP_ARGB_BLACK);
    for (int x = 1; x < width; x++)
    {
        out[x] = vp8l_add_pixels(in_[x], out[x - 1]);
    }

    int iIn = width;
    int oOut = width;
    int y = 1;
    while (y < rows)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        int predModeBase = (y >> t->bits) * tilesPerRow;
        int predModeSrc = predModeBase;
        out[oOut] = vp8l_add_pixels(in_[iIn], out[oOut - width]);
        int x = 1;
        while (x < width)
        {
            int mode = (int)((tdata[predModeSrc] >> 8) & 0xF);
            predModeSrc++;
            int xEnd = (x & ~mask) + tileWidth;
            if (xEnd > width)
            {
                xEnd = width;
            }
            vp8l_predictor_add(mode, in_, iIn, out, oOut, x, xEnd - x, width);
            x = xEnd;
        }
        iIn += width;
        oOut += width;
        y++;
    }
}

static int vp8l_to_int8(int v)
{
    if (v >= 128)
    {
        return v - 256;
    }
    return v;
}

static int vp8l_color_transform_delta(int colorPred, int color)
{
    return (colorPred * color) >> 5;
}

static void vp8l_transform_color_inverse(const int m[3], const uint32_t *src,
                                         int srcPos, int num, uint32_t *dst,
                                         int dstPos)
{
    for (int i = 0; i < num; i++)
    {
        uint32_t argb = src[srcPos + i];
        int green = vp8l_to_int8((int)((argb >> 8) & 0xFF));
        int newRed = (int)((argb >> 16) & 0xFF);
        int newBlue = (int)(argb & 0xFF);
        newRed = (newRed + vp8l_color_transform_delta(m[0], green)) & 0xFF;
        newBlue = (newBlue + vp8l_color_transform_delta(m[1], green)) & 0xFF;
        newBlue = (newBlue + vp8l_color_transform_delta(m[2], vp8l_to_int8(newRed))) & 0xFF;
        dst[dstPos + i] = (argb & 0xFF00FF00u) | ((uint32_t)newRed << 16) | (uint32_t)newBlue;
    }
}

/* ── colorSpaceInverse (webp.lua 765-803) ─────────────────────────────────── */
static void vp8l_color_space_inverse(const WebPTransform *t, const uint32_t *in_,
                                     uint32_t *out, int rows)
{
    int width = t->xsize;
    int tileWidth = 1 << t->bits;
    int mask = tileWidth - 1;
    int safeWidth = width & ~mask;
    int remainingWidth = width - safeWidth;
    int tilesPerRow = vp8l_sub_sample_size(width, t->bits);
    const uint32_t *tdata = t->data;
    int predRow = 0;
    int y = 0;
    while (y < rows)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        int predIdx = predRow;
        int m[3] = { 0, 0, 0 };
        int srcPos = y * width;
        int dstPos = y * width;
        int safeEnd = srcPos + safeWidth;
        int endPos = srcPos + width;
        while (srcPos < safeEnd)
        {
            uint32_t code = tdata[predIdx++];
            m[0] = vp8l_to_int8((int)(code & 0xFF));
            m[1] = vp8l_to_int8((int)((code >> 8) & 0xFF));
            m[2] = vp8l_to_int8((int)((code >> 16) & 0xFF));
            vp8l_transform_color_inverse(m, in_, srcPos, tileWidth, out, dstPos);
            srcPos += tileWidth;
            dstPos += tileWidth;
        }
        if (srcPos < endPos)
        {
            uint32_t code = tdata[predIdx];
            m[0] = vp8l_to_int8((int)(code & 0xFF));
            m[1] = vp8l_to_int8((int)((code >> 8) & 0xFF));
            m[2] = vp8l_to_int8((int)((code >> 16) & 0xFF));
            vp8l_transform_color_inverse(m, in_, srcPos, remainingWidth, out, dstPos);
        }
        y++;
        if ((y & mask) == 0)
        {
            predRow += tilesPerRow;
        }
    }
}

/* ── addGreenToBlueAndRed (webp.lua 805-814) ──────────────────────────────── */
static void vp8l_add_green_to_blue_and_red(const uint32_t *src, int num, uint32_t *dst)
{
    for (int i = 0; i < num; i++)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        uint32_t argb = src[i];
        uint32_t green = (argb >> 8) & 0xFFu;
        uint32_t redBlue = (argb & 0x00FF00FFu) + ((green << 16) | green);
        redBlue &= 0x00FF00FFu;
        dst[i] = (argb & 0xFF00FF00u) | redBlue;
    }
}

/* ── colorIndexInverse + expandColorMap (webp.lua 816-947) ────────────────── */
static void vp8l_color_index_inverse(const WebPTransform *t, const uint32_t *in_,
                                     uint32_t *out, int rows)
{
    int bitsPerPixel = 8 >> t->bits;
    int width = t->xsize;
    const uint32_t *colorMap = t->data;
    if (bitsPerPixel < 8)
    {
        int ppb = 1 << t->bits;
        int bitMask = (1 << bitsPerPixel) - 1;
        int idx = 0;
        for (int y = 0; y < rows; y++)
        {
            /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
            int x = 0;
            int oBase = y * width;
            while (x + ppb <= width)
            {
                uint32_t packed = (in_[idx] >> 8) & 0xFFu;
                idx++;
                for (int p = 0; p < ppb; p++)
                {
                    out[oBase + x] = colorMap[packed & (uint32_t)bitMask];
                    packed >>= bitsPerPixel;
                    x++;
                }
            }
            if (x < width)
            {
                uint32_t packed = (in_[idx] >> 8) & 0xFFu;
                idx++;
                while (x < width)
                {
                    out[oBase + x] = colorMap[packed & (uint32_t)bitMask];
                    packed >>= bitsPerPixel;
                    x++;
                }
            }
        }
    }
    else
    {
        int total = rows * width;
        for (int i = 0; i < total; i++)
        {
            /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
            out[i] = colorMap[(in_[i] >> 8) & 0xFFu];
        }
    }
}

static uint32_t *vp8l_expand_color_map(int numColors, const WebPTransform *t)
{
    int finalNum = 1 << (8 >> t->bits);
    uint32_t *newData = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)finalNum);
    if (!newData)
    {
        return NULL;
    }
    const uint32_t *tdata = t->data;
    newData[0] = tdata[0];
    for (int i = 1; i < numColors; i++)
    {
        uint32_t prev = newData[i - 1];
        uint32_t cur = tdata[i];
        uint32_t a = ((prev >> 24) & 0xFF) + ((cur >> 24) & 0xFF);
        uint32_t r = ((prev >> 16) & 0xFF) + ((cur >> 16) & 0xFF);
        uint32_t g = ((prev >> 8) & 0xFF) + ((cur >> 8) & 0xFF);
        uint32_t b = (prev & 0xFF) + (cur & 0xFF);
        newData[i] = ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
    for (int i = numColors; i < finalNum; i++)
    {
        newData[i] = 0;
    }
    return newData;
}

/* ── applyInverseTransforms (webp.lua 950-971) ────────────────────────────── */
static uint32_t *vp8l_apply_inverse_transforms(WebPDecodeCtx *ctx, uint32_t *data,
                                               int rows, int *outOwned);
uint32_t *vp8l_apply_inverse_transforms_pub(WebPDecodeCtx *ctx, uint32_t *data,
                                            int rows, int *outOwned)
{
    return vp8l_apply_inverse_transforms(ctx, data, rows, outOwned);
}
static uint32_t *vp8l_apply_inverse_transforms(WebPDecodeCtx *ctx, uint32_t *data,
                                               int rows, int *out_owned)
{
    if (ctx->numTransforms == 0)
    {
        *out_owned = 0;
        return data;
    }
    uint32_t *in_ = data;
    int inOwned = 0;
    for (int i = ctx->numTransforms - 1; i >= 0; i--)
    {
        const WebPTransform *t = &ctx->transforms[i];
        uint32_t *out = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)t->xsize * (size_t)rows);
        if (!out)
        {
            if (inOwned)
            {
                PLUTO_FREE(in_);
            }
            *out_owned = 0;
            return NULL;
        }
        if (t->type == WEBP_COLOR_INDEXING_TRANSFORM)
        {
            vp8l_color_index_inverse(t, in_, out, rows);
        }
        else if (t->type == WEBP_SUBTRACT_GREEN_TRANSFORM)
        {
            vp8l_add_green_to_blue_and_red(in_, t->xsize * rows, out);
        }
        else if (t->type == WEBP_PREDICTOR_TRANSFORM)
        {
            vp8l_predictor_inverse(t, in_, out, rows);
        }
        else
        {
            vp8l_color_space_inverse(t, in_, out, rows);
        }
        if (inOwned)
        {
            PLUTO_FREE(in_);
        }
        in_ = out;
        inOwned = 1;
    }
    *out_owned = inOwned;
    return in_;
}

/* ── decodeImageStream (webp.lua 977-1035) ────────────────────────────────── */
int vp8l_decode_image_stream_pub(int xsize, int ysize, int isLevel0,
                                 WebPBitReader *br, WebPDecodeCtx *ctx)
{
    return (vp8l_decode_image_stream(xsize, ysize, isLevel0, br, ctx) != NULL) ? 1 : 0;
}
static uint32_t *vp8l_decode_image_stream(int xsize, int ysize, int isLevel0,
                                          WebPBitReader *br, WebPDecodeCtx *ctx)
{
    int transformXsize = xsize;
    int transformYsize = ysize;
    uint32_t transformsSeen = 0;
    int colorCacheBits = 0;
    memset(ctx, 0, sizeof(*ctx));

    if (isLevel0)
    {
        while (webp_br_read_bits(br, 1) == 1)
        {
            int type_ = (int)webp_br_read_bits(br, 2);
            if ((transformsSeen & (1u << type_)) != 0)
            {
                return NULL;
            }
            transformsSeen |= (1u << type_);
            if (ctx->numTransforms >= WEBP_MAX_TRANSFORMS)
            {
                return NULL;
            }
            WebPTransform *t = &ctx->transforms[ctx->numTransforms];
            memset(t, 0, sizeof(*t));
            t->type = type_;
            t->xsize = transformXsize;
            t->ysize = transformYsize;
            if (type_ == WEBP_PREDICTOR_TRANSFORM || type_ == WEBP_CROSS_COLOR_TRANSFORM)
            {
                t->bits = WEBP_MIN_TRANSFORM_BITS + (int)webp_br_read_bits(br, WEBP_NUM_TRANSFORM_BITS);
                WebPDecodeCtx subCtx;
                t->data = vp8l_decode_image_stream(
                    vp8l_sub_sample_size(t->xsize, t->bits),
                    vp8l_sub_sample_size(t->ysize, t->bits), 0, br, &subCtx);
                if (!t->data)
                {
                    return NULL;
                }
                vp8l_ctx_free(&subCtx);
            }
            else if (type_ == WEBP_COLOR_INDEXING_TRANSFORM)
            {
                int numColors = (int)webp_br_read_bits(br, 8) + 1;
                int bits = (numColors > 16) ? 0 : (numColors > 4) ? 1 : (numColors > 2) ? 2 : 3;
                transformXsize = vp8l_sub_sample_size(transformXsize, bits);
                t->bits = bits;
                WebPDecodeCtx subCtx;
                t->data = vp8l_decode_image_stream(numColors, 1, 0, br, &subCtx);
                if (!t->data)
                {
                    return NULL;
                }
                vp8l_ctx_free(&subCtx);
                uint32_t *expanded = vp8l_expand_color_map(numColors, t);
                if (!expanded)
                {
                    PLUTO_FREE(t->data);
                    t->data = NULL;
                    return NULL;
                }
                PLUTO_FREE(t->data);
                t->data = expanded;
            }
            else if (type_ == WEBP_SUBTRACT_GREEN_TRANSFORM)
            {
                /* nothing to read */
            }
            else
            {
                return NULL;
            }
            ctx->numTransforms++;
        }
    }

    if (webp_br_read_bits(br, 1) == 1)
    {
        colorCacheBits = (int)webp_br_read_bits(br, 4);
        if (colorCacheBits < 1 || colorCacheBits > WEBP_MAX_CACHE_BITS)
        {
            return NULL;
        }
    }

    if (!vp8l_read_huffman_codes(br, transformXsize, transformYsize,
                                 colorCacheBits, isLevel0, ctx))
    {
        return NULL;
    }

    if (colorCacheBits > 0)
    {
        ctx->colorCacheSize = 1 << colorCacheBits;
        ctx->colorCacheColors = (uint32_t *)PLUTO_MALLOC(
            sizeof(uint32_t) * (size_t)ctx->colorCacheSize);
        if (!ctx->colorCacheColors)
        {
            return NULL;
        }
        memset(ctx->colorCacheColors, 0, sizeof(uint32_t) * (size_t)ctx->colorCacheSize);
        ctx->colorCacheHashShift = 32 - colorCacheBits;
    }

    ctx->huffmanSubsampleBits = ctx->huffmanSubsampleBits; /* already set or 0 */
    ctx->transformXsize = transformXsize;
    ctx->transformYsize = transformYsize;
    ctx->huffmanXsize = (transformXsize + (1 << ctx->huffmanSubsampleBits) - 1)
                        >> ctx->huffmanSubsampleBits;

    if (isLevel0)
    {
        return (uint32_t *)(intptr_t)1; /* success sentinel; ctx populated */
    }

    int totalSize = transformXsize * transformYsize;
    uint32_t *data = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)totalSize);
    if (!data)
    {
        return NULL;
    }
    memset(data, 0, sizeof(uint32_t) * (size_t)totalSize);
    if (!vp8l_decode_image_data(br, ctx, data, transformXsize, transformYsize))
    {
        PLUTO_FREE(data);
        return NULL;
    }
    if (br->eos)
    {
        PLUTO_FREE(data);
        return NULL;
    }
    return data;
}

/* ── decodeVP8LPayload (webp.lua 973-1005) ────────────────────────────────── */
uint32_t *webp_decode_vp8l_payload(const uint8_t *payload, size_t payloadLen,
                                   int *outW, int *outH, int *outOwned)
{
    if (payloadLen < 5)
    {
        return NULL;
    }
    if (payload[0] != WEBP_VP8L_MAGIC_BYTE)
    {
        return NULL;
    }
    uint32_t bits = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8)
                    | ((uint32_t)payload[3] << 16);
    int width = (int)(bits & 0x3FFFu) + 1;
    int height = (int)((bits >> 14) & 0x3FFFu) + 1;
    if (((bits >> 29) & 7u) != 0)
    {
        return NULL;
    }
    if (width < 1 || height < 1 || width * height > WEBP_MAX_PIXELS)
    {
        return NULL;
    }

    WebPBitReader br;
    webp_br_init(&br, payload, (int)payloadLen, 6);
    WebPDecodeCtx ctx;
    if (!vp8l_decode_image_stream(width, height, 1, &br, &ctx))
    {
        vp8l_ctx_free(&ctx);
        vp8l_groups_free();
        return NULL;
    }

    int transformXsize = ctx.transformXsize;
    int transformYsize = ctx.transformYsize;
    if (transformXsize < 1 || transformYsize < 1)
    {
        vp8l_ctx_free(&ctx);
        vp8l_groups_free();
        return NULL;
    }
    int totalSize = transformXsize * transformYsize;
    uint32_t *data = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)totalSize);
    if (!data)
    {
        vp8l_ctx_free(&ctx);
        vp8l_groups_free();
        return NULL;
    }
    memset(data, 0, sizeof(uint32_t) * (size_t)totalSize);
    if (!vp8l_decode_image_data(&br, &ctx, data, transformXsize, transformYsize))
    {
        PLUTO_FREE(data);
        vp8l_ctx_free(&ctx);
        vp8l_groups_free();
        return NULL;
    }
    if (br.eos)
    {
        PLUTO_FREE(data);
        vp8l_ctx_free(&ctx);
        vp8l_groups_free();
        return NULL;
    }

    int finalOwned = 0;
    uint32_t *final = vp8l_apply_inverse_transforms(&ctx, data, transformYsize, &finalOwned);
    vp8l_ctx_free(&ctx);
    vp8l_groups_free();
    if (!final)
    {
        PLUTO_FREE(data);
        return NULL;
    }
    if (!finalOwned)
    {
        /* No transforms: the input array is the result. */
    }
    *outW = width;
    *outH = height;
    *outOwned = 1;
    (void)finalOwned;
    return final;
}
