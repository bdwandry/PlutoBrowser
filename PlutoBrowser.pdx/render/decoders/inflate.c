// inflate.c — C port of Source/render/decoders/inflate.lua (Inflate).
//
// Pure fast Deflate/zlib decompressor for PNG on Playdate.

#include "render/decoders/inflate.h"

#include <stdlib.h>
#include <string.h>

#include "core/tasks.h"
#include "util/mem.h"

/* ── bit stream (LSB-first) ───────────────────────────────────────────── */

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t bytePos;      /* 0-based (Lua 1-based bytePos) */
    uint32_t bitBuf;
    int bitCount;
} BitStream;

static void bs_init(BitStream* bs, const uint8_t* data, size_t len) {
    bs->data = data;
    bs->len = len;
    bs->bytePos = 0;
    bs->bitBuf = 0;
    bs->bitCount = 0;
}

static int bs_read_bits(BitStream* bs, int n) {
    while (bs->bitCount < n) {
        if (bs->bytePos >= bs->len) return -1;   /* Lua nil */
        uint32_t b = bs->data[bs->bytePos++];
        bs->bitBuf |= b << bs->bitCount;
        bs->bitCount += 8;
    }
    uint32_t mask = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
    int val = (int)(bs->bitBuf & mask);
    bs->bitBuf >>= n;
    bs->bitCount -= n;
    return val;
}

static void bs_align_byte(BitStream* bs) {
    int drop = bs->bitCount & 7;
    if (drop > 0) {
        bs->bitBuf >>= drop;
        bs->bitCount -= drop;
    }
}

/* Lua's `bs:readBits(n) or 0`: EOF (-1/nil) coerces to 0 */
static int rb_or0(BitStream* bs, int n) {
    int v = bs_read_bits(bs, n);
    return v < 0 ? 0 : v;
}

/* ── canonical Huffman tables ─────────────────────────────────────────── */

typedef struct {
    uint16_t code;     /* bit-reversed for the LSB-first stream */
    uint8_t len;
    uint16_t symbol;
} HuffEntry;

typedef struct {
    HuffEntry* entries;
    int count;
} HuffTable;

static int huff_cmp(const void* a, const void* b) {
    const HuffEntry* ea = (const HuffEntry*)a;
    const HuffEntry* eb = (const HuffEntry*)b;
    if (ea->len != eb->len) return ea->len < eb->len ? -1 : 1;
    /* deterministic tie order (Lua's unstable sort is order-irrelevant
     * here: codes within one length are unique) */
    return ea->symbol < eb->symbol ? -1 : (ea->symbol > eb->symbol ? 1 : 0);
}

static void huff_free(HuffTable* t) {
    pluto_free(t->entries);
    t->entries = NULL;
    t->count = 0;
}

static void build_huffman_table(HuffTable* out,
                                const uint8_t* codeLengths, int n) {
    memset(out, 0, sizeof(*out));
    int maxLen = 0;
    for (int i = 0; i < n; i++)
        if (codeLengths[i] > maxLen) maxLen = codeLengths[i];
    if (maxLen == 0) return;

    int blCount[16];
    memset(blCount, 0, sizeof(blCount));
    for (int i = 0; i < n; i++) blCount[codeLengths[i]]++;

    int nextCode[16];
    int code = 0;
    blCount[0] = 0;
    for (int i = 1; i <= maxLen && i < 16; i++) {
        code = (code + blCount[i - 1]) << 1;
        nextCode[i] = code;
    }

    HuffEntry* lut =
        (HuffEntry*)pluto_malloc(sizeof(HuffEntry) * (size_t)n);
    if (lut == NULL) return;
    int cnt = 0;

    for (int sym = 0; sym < n; sym++) {   /* Lua ipairs is 1-based; sym-1 */
        int len = codeLengths[sym];
        if (len > 0 && len < 16) {
            int c = nextCode[len]++;
            /* reverse bits for LSB-first bitstream */
            uint16_t revCode = 0;
            for (int b = 0; b < len; b++)
                if (c & (1 << (len - 1 - b))) revCode |= (uint16_t)(1u << b);
            lut[cnt].code = revCode;
            lut[cnt].len = (uint8_t)len;
            lut[cnt].symbol = (uint16_t)sym;
            cnt++;
        }
    }

    qsort(lut, (size_t)cnt, sizeof(HuffEntry), huff_cmp);   // NOLINT
    out->entries = lut;
    out->count = cnt;
}

static int decode_symbol(BitStream* bs, const HuffTable* huff) {
    int curCode = 0;
    int curLen = 0;
    for (int i = 0; i < huff->count; i++) {
        const HuffEntry* e = &huff->entries[i];
        while (curLen < e->len) {
            int bit = bs_read_bits(bs, 1);
            if (bit < 0) return -1;
            curCode |= bit << curLen;
            curLen++;
        }
        if (curCode == e->code && curLen == e->len) return e->symbol;
    }
    return -1;
}

/* ── fixed tables (built once) ────────────────────────────────────────── */

static HuffTable s_fixedLit, s_fixedDist;
static int s_fixedLitBuilt = 0;

static void get_fixed_tables(void) {
    if (s_fixedLitBuilt) return;

    uint8_t litLens[288];
    for (int i = 0; i <= 143; i++) litLens[i] = 8;
    for (int i = 144; i <= 255; i++) litLens[i] = 9;
    for (int i = 256; i <= 279; i++) litLens[i] = 7;
    for (int i = 280; i <= 287; i++) litLens[i] = 8;
    build_huffman_table(&s_fixedLit, litLens, 288);

    uint8_t distLens[32];
    for (int i = 0; i < 32; i++) distLens[i] = 5;
    build_huffman_table(&s_fixedDist, distLens, 32);
    s_fixedLitBuilt = 1;
}


/* ── length/distance tables ───────────────────────────────────────────── */

static const uint16_t kLengthBase[286] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,
    131,163,195,227,258
};
static const uint8_t kLengthExtra[286] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,
    1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const uint8_t kClOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* dynamic-header helper shared by both decoders; returns 0 ok / -1 eof */
static int read_dynamic_tables(BitStream* bs, HuffTable* litTable,
                               HuffTable* distTable) {
    int hlit = rb_or0(bs, 5) + 257;
    int hdist = rb_or0(bs, 5) + 1;
    int hclen = rb_or0(bs, 4) + 4;
    if (hlit > 288 || hdist > 32) return -1;   /* corrupt; Lua would spin */

    uint8_t codeLens[19];
    memset(codeLens, 0, sizeof(codeLens));
    for (int i = 0; i < hclen; i++) {
        int cl = bs_read_bits(bs, 3);
        codeLens[kClOrder[i]] = (uint8_t)(cl < 0 ? 0 : cl);
    }
    HuffTable clTable;
    build_huffman_table(&clTable, codeLens, 19);

    uint8_t allLens[320];
    memset(allLens, 0, sizeof(allLens));
    int nLens = 0;
    while (nLens < hlit + hdist) {
        int sym = decode_symbol(bs, &clTable);
        if (sym < 0) break;
        if (sym < 16) {
            allLens[nLens++] = (uint8_t)sym;
        } else if (sym == 16) {
            int rc = rb_or0(bs, 2) + 3;
            uint8_t last = nLens > 0 ? allLens[nLens - 1] : 0;
            while (rc-- > 0 && nLens < 320) allLens[nLens++] = last;
        } else if (sym == 17) {
            int rc = rb_or0(bs, 3) + 3;
            while (rc-- > 0 && nLens < 320) allLens[nLens++] = 0;
        } else {   /* 18 */
            int rc = rb_or0(bs, 7) + 11;
            while (rc-- > 0 && nLens < 320) allLens[nLens++] = 0;
        }
    }
    huff_free(&clTable);

    build_huffman_table(litTable, allLens, hlit);
    build_huffman_table(distTable, allLens + hlit, hdist);
    return 0;
}

/* growable output shared by decode paths */
typedef struct {
    uint8_t* bytes;
    size_t len, cap;
} ByteOut;

static int bo_reserve(ByteOut* o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    size_t nc = o->cap ? o->cap : 4096;
    while (nc < o->len + extra) nc *= 2;
    uint8_t* nb = (uint8_t*)pluto_realloc(o->bytes, nc);
    if (nb == NULL) return -1;
    o->bytes = nb;
    o->cap = nc;
    return 0;
}

static int bo_push(ByteOut* o, uint8_t b) {
    if (bo_reserve(o, 1) != 0) return -1;
    o->bytes[o->len++] = b;
    return 0;
}

/* copies a match; out-of-range source bytes become 0 (Lua `or 0`) */
static int bo_copy_match(ByteOut* o, int matchLen, long matchDist) {
    if (bo_reserve(o, (size_t)(matchLen > 0 ? matchLen : 0)) != 0)
        return -1;
    /* base offset fixed once; overlapping copies must observe the bytes
     * appended by earlier iterations (classic LZ77 semantics) */
    long srcIdx = (long)o->len - matchDist;
    for (int i = 0; i < matchLen; i++) {
        uint8_t b = 0;
        long idx = srcIdx + i;
        if (idx >= 0 && (size_t)idx < o->len)
            b = o->bytes[idx];
        o->bytes[o->len++] = b;
    }
    return 0;
}

/* skips zlib CMF/FLG (+ preset dict) header when present */
static size_t zlib_skip(const uint8_t* data, size_t len) {
    size_t startPos = 0;
    if (len >= 2) {
        int cmf = data[0], flg = data[1];
        if ((cmf & 0x0F) == 8 && ((cmf * 256 + flg) % 31) == 0) {
            startPos = 2;
            if ((flg & 0x20) != 0) startPos += 4;
        }
    }
    if (startPos > len) startPos = len;
    return startPos;
}

/* ── one-shot decompress ──────────────────────────────────────────────── */

long inflate_decompress(const uint8_t* data, size_t len,
                        uint8_t** out, size_t* outLen) {
    *out = NULL;
    *outLen = 0;
    if (data == NULL || len < 2) return -1;

    BitStream bs;
    bs_init(&bs, data + zlib_skip(data, len), len - zlib_skip(data, len));

    ByteOut output = {0};
    int isFinal = 0;
    while (isFinal == 0) {
        isFinal = bs_read_bits(&bs, 1);
        int btype = bs_read_bits(&bs, 2);
        if (btype < 0) break;

        if (btype == 0) {                       /* uncompressed block */
            bs_align_byte(&bs);
            int blen = bs_read_bits(&bs, 16);
            bs_read_bits(&bs, 16);              /* NLEN */
            if (blen < 0) break;
            for (int i = 0; i < blen; i++) {
                int b = bs_read_bits(&bs, 8);
                if (b < 0) break;
                if (bo_push(&output, (uint8_t)b) != 0) goto done;
            }
        } else if (btype == 1 || btype == 2) {  /* Huffman blocks */
            HuffTable litTable, distTable;
            if (btype == 1) {
                get_fixed_tables();
                litTable = s_fixedLit;
                distTable = s_fixedDist;
            } else {
                if (read_dynamic_tables(&bs, &litTable, &distTable) != 0)
                    break;
            }

            while (1) {
                tasks_yield_check();
                int sym = decode_symbol(&bs, &litTable);
                if (sym < 0 || sym == 256) break;

                if (sym < 256) {
                    if (bo_push(&output, (uint8_t)sym) != 0) {
                        if (btype == 2) { huff_free(&litTable);
                                          huff_free(&distTable); }
                        goto done;
                    }
                } else {
                    int idx = sym - 257;
                    int baseL = idx < 29 ? kLengthBase[idx] : 3;
                    int extraLBits = idx < 29 ? kLengthExtra[idx] : 0;
                    int extraL = 0;
                    if (extraLBits > 0) {
                        int v = bs_read_bits(&bs, extraLBits);
                        if (v < 0) v = 0;
                        extraL = v;
                    }
                    int matchLen = baseL + extraL;

                    int distSym = decode_symbol(&bs, &distTable);
                    if (distSym < 0) distSym = 0;
                    int baseD = distSym < 30 ? kDistBase[distSym] : 1;
                    int extraDBits = distSym < 30 ? kDistExtra[distSym] : 0;
                    int extraD = 0;
                    if (extraDBits > 0) {
                        int v = bs_read_bits(&bs, extraDBits);
                        if (v < 0) v = 0;
                        extraD = v;
                    }
                    if (bo_copy_match(&output, matchLen,
                                      baseD + extraD) != 0) {
                        if (btype == 2) { huff_free(&litTable);
                                          huff_free(&distTable); }
                        goto done;
                    }
                }
            }
            if (btype == 2) {
                huff_free(&litTable);
                huff_free(&distTable);
            }
        }
        /* btype == 3: silently skipped, like the Lua original */
    }

done:
    *out = output.bytes;
    *outLen = output.len;
    return (long)output.len;
}

/* ── streaming inflate ────────────────────────────────────────────────── */

struct InflateStream {
    BitStream bs;

    uint8_t* pending;
    size_t pendLen, pendCap, pendHead;

    uint8_t win[65536];       /* sliding LZ77 window */
    size_t winLen;
    long produced;
    long winStart;            /* absolute index of win[0] */

    enum { ST_BLOCK, ST_UNCOMPRESSED, ST_HUFF } mode;
    int remaining;            /* bytes left in uncompressed block */
    HuffTable litTable, distTable;
    int eof;
    int isFinalBlock;
    int tablesOwned;          /* dynamic tables need freeing */
};

static void st_emit(InflateStream* s, uint8_t b) {
    if (s->pendLen == s->pendCap) {
        /* compact consumed head first */
        if (s->pendHead > 0) {
            memmove(s->pending, s->pending + s->pendHead,
                    s->pendLen - s->pendHead);
            s->pendLen -= s->pendHead;
            s->pendHead = 0;
        } else {
            size_t nc = s->pendCap ? s->pendCap * 2 : 1024;
            uint8_t* np = (uint8_t*)pluto_realloc(s->pending, nc);
            if (np == NULL) return;   /* OOM: drop byte (Lua would throw) */
            s->pending = np;
            s->pendCap = nc;
        }
    }
    s->pending[s->pendLen++] = b;
    s->produced++;
    if (s->winLen < sizeof(s->win)) {
        s->win[s->winLen++] = b;
    } else {
        /* keep the most recent half, mirroring the Lua window trim */
        memmove(s->win, s->win + 32768, 32768);
        s->winLen = 32768;
        s->win[s->winLen++] = b;
        s->winStart = s->produced - (long)s->winLen;
    }
}

static void st_emit_match(InflateStream* s, int matchLen, long matchDist) {
    for (int i = 0; i < matchLen; i++) {
        long rel = (s->produced - matchDist) - s->winStart;
        uint8_t b = (rel >= 0 && (size_t)rel < s->winLen)
                        ? s->win[rel] : 0;
        st_emit(s, b);
    }
}

static void st_begin_block(InflateStream* s) {
    int fin = bs_read_bits(&s->bs, 1);
    int btype = bs_read_bits(&s->bs, 2);
    if (fin < 0 || btype < 0) {
        s->eof = 1;
        return;
    }
    s->isFinalBlock = (fin == 1);
    if (btype == 0) {
        bs_align_byte(&s->bs);
        int rem = bs_read_bits(&s->bs, 16);
        bs_read_bits(&s->bs, 16);   /* NLEN */
        s->remaining = rem < 0 ? 0 : rem;
        s->mode = ST_UNCOMPRESSED;
    } else if (btype == 1) {
        get_fixed_tables();
        s->litTable = s_fixedLit;
        s->distTable = s_fixedDist;
        s->tablesOwned = 0;
        s->mode = ST_HUFF;
    } else if (btype == 2) {
        if (s->tablesOwned) {
            huff_free(&s->litTable);
            huff_free(&s->distTable);
            s->tablesOwned = 0;
        }
        if (read_dynamic_tables(&s->bs, &s->litTable,
                                &s->distTable) != 0) {
            s->eof = 1;
            return;
        }
        s->tablesOwned = 1;
        s->mode = ST_HUFF;
    } else {
        s->eof = 1;   /* btype 3: avoid spinning forever in a stream */
    }
}

static void st_pump_until(InflateStream* s, size_t n) {
    while (!s->eof && (s->pendLen - s->pendHead) < n) {
        tasks_yield_check();
        if (s->mode == ST_UNCOMPRESSED) {
            if (s->remaining > 0) {
                s->remaining--;
                int b = bs_read_bits(&s->bs, 8);
                if (b < 0) { s->eof = 1; break; }
                st_emit(s, (uint8_t)b);
            } else {
                if (s->isFinalBlock) { s->eof = 1; break; }
                s->mode = ST_BLOCK;
            }
        } else if (s->mode == ST_BLOCK) {
            st_begin_block(s);
        } else {   /* ST_HUFF */
            int sym = decode_symbol(&s->bs, &s->litTable);
            if (sym < 0) { s->eof = 1; break; }
            if (sym == 256) {
                if (s->isFinalBlock) { s->eof = 1; break; }
                s->mode = ST_BLOCK;
            } else if (sym < 256) {
                st_emit(s, (uint8_t)sym);
            } else {
                int idx = sym - 257;
                int baseL = idx < 29 ? kLengthBase[idx] : 3;
                int el = idx < 29 ? kLengthExtra[idx] : 0;
                int extraL = 0;
                if (el > 0) {
                    int v = bs_read_bits(&s->bs, el);
                    extraL = v < 0 ? 0 : v;
                }
                int len = baseL + extraL;
                int distSym = decode_symbol(&s->bs, &s->distTable);
                if (distSym < 0) { s->eof = 1; break; }
                int baseD = distSym < 30 ? kDistBase[distSym] : 1;
                int ed = distSym < 30 ? kDistExtra[distSym] : 0;
                int extraD = 0;
                if (ed > 0) {
                    int v = bs_read_bits(&s->bs, ed);
                    extraD = v < 0 ? 0 : v;
                }
                st_emit_match(s, len, baseD + extraD);
            }
        }
    }
}

InflateStream* inflate_stream_new(const uint8_t* data, size_t len) {
    if (data == NULL || len < 2) return NULL;
    InflateStream* s =
        (InflateStream*)pluto_malloc(sizeof(InflateStream));
    if (s == NULL) return NULL;
    memset(s, 0, sizeof(*s));
    size_t skip = zlib_skip(data, len);
    bs_init(&s->bs, data + skip, len - skip);
    s->mode = ST_BLOCK;
    return s;
}

size_t inflate_stream_read(InflateStream* s, uint8_t* buf, size_t n) {
    if (s == NULL || buf == NULL || n == 0) return 0;
    st_pump_until(s, n);
    size_t avail = s->pendLen - s->pendHead;
    if (avail == 0) return 0;   /* Lua nil */
    size_t k = n < avail ? n : avail;
    memcpy(buf, s->pending + s->pendHead, k);
    s->pendHead += k;
    if (s->pendHead == s->pendLen) s->pendHead = s->pendLen = 0;
    return k;
}

void inflate_stream_free(InflateStream* s) {
    if (s == NULL) return;
    if (s->tablesOwned) {
        huff_free(&s->litTable);
        huff_free(&s->distTable);
    }
    pluto_free(s->pending);
    pluto_free(s);
}
