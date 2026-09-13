/*
 * PlutoBrowser — inflate.c
 * Port of Source/render/decoders/inflate.lua (reference, 427 lines).
 * Pure DEFLATE decompressor (one-shot + streaming) for PNG.
 * See inflate.h for the Lua→C map and documented deviations.
 */
#include "core/logger.h"
#include <stdlib.h>
#include <string.h>
#include "render/decoders/inflate.h"

/* ── Bit stream (Lua createBitStream / bs:readBits / bs:alignByte) ─────── */
typedef struct
{
    const uint8_t *data;
    size_t len;     /* Lua #str */
    size_t bytePos; /* Lua 1-based bytePos → C 0-based index */
    uint32_t bitBuf;
    int bitCount;
} BitStream;

/* Returns 1 on success, 0 = Lua nil (out untouched). */
static int bs_read_bits(BitStream *bs, int n, uint32_t *out)
{
    while (bs->bitCount < n)
    {
        if (bs->bytePos >= bs->len)
        {
            return 0;
        }
        bs->bitBuf |= (uint32_t)bs->data[bs->bytePos] << bs->bitCount;
        bs->bytePos++;
        bs->bitCount += 8;
    }
    *out = bs->bitBuf & ((1u << n) - 1);
    bs->bitBuf >>= n;
    bs->bitCount -= n;
    return 1;
}

/* Lua `bs:readBits(n) or 0` fallback sites. */
static uint32_t bs_read_bits_or0(BitStream *bs, int n)
{
    uint32_t v = 0;
    bs_read_bits(bs, n, &v);
    return v;
}

static void bs_align_byte(BitStream *bs)
{
    int drop = bs->bitCount & 7;
    if (drop > 0)
    {
        bs->bitBuf >>= drop;
        bs->bitCount -= drop;
    }
}

/* ── Canonical Huffman (Lua buildHuffmanTable / decodeSymbol) ──────────── */
#define INF_MAX_ENTRIES 288
typedef struct
{
    int count;
    struct
    {
        uint32_t code;
        int len;
        int symbol;
    } e[INF_MAX_ENTRIES];
} HuffTable;

static void inf_build_table(const uint8_t *lens, int n, HuffTable *t)
{
    t->count = 0;
    int maxLen = 0;
    for (int i = 0; i < n; i++)
    {
        if (lens[i] > maxLen)
        {
            maxLen = lens[i];
        }
    }
    if (maxLen == 0)
    {
        return;
    }

    int blCount[16] = {0};
    for (int i = 0; i < n; i++)
    {
        blCount[lens[i]]++;
    }

    int nextCode[16];
    int code = 0;
    blCount[0] = 0;
    for (int i = 1; i <= maxLen; i++)
    {
        code = (code + blCount[i - 1]) << 1;
        nextCode[i] = code;
    }

    /* Symbols in order (Lua sym 1..n → C 0..n-1); bit-reverse for the
     * LSB-first bitstream. */
    for (int sym = 0; sym < n; sym++)
    {
        int len = lens[sym];
        if (len > 0)
        {
            int c = nextCode[len]++;
            uint32_t rev = 0;
            for (int b = 0; b < len; b++)
            {
                if (c & (1 << (len - 1 - b)))
                {
                    rev |= (1u << b);
                }
            }
            t->e[t->count].code = rev;
            t->e[t->count].len = len;
            t->e[t->count].symbol = sym;
            t->count++;
        }
    }

    /* Lua table.sort by len: stable insertion sort keeps symbol order
     * within equal lengths (qsort is not stable). */
    for (int i = 1; i < t->count; i++)
    {
        typeof(t->e[0]) tmp = t->e[i];
        int j = i - 1;
        while (j >= 0 && t->e[j].len > tmp.len)
        {
            t->e[j + 1] = t->e[j];
            j--;
        }
        t->e[j + 1] = tmp;
    }
}

/* Returns symbol 0..287, or -1 = Lua nil. */
static int inf_decode_symbol(BitStream *bs, const HuffTable *h)
{
    if (h->count == 0)
    {
        return -1; /* empty lut: Lua ipairs loop never runs → nil */
    }
    uint32_t curCode = 0;
    int curLen = 0;
    for (int i = 0; i < h->count; i++)
    {
        while (curLen < h->e[i].len)
        {
            uint32_t bit;
            if (!bs_read_bits(bs, 1, &bit))
            {
                return -1;
            }
            curCode |= bit << curLen;
            curLen++;
        }
        if (curCode == h->e[i].code && curLen == h->e[i].len)
        {
            return h->e[i].symbol;
        }
    }
    return -1;
}

/* ── Fixed tables (Lua getFixedTables, cached) ─────────────────────────── */
static HuffTable fixedLit;
static HuffTable fixedDist;
static int fixedBuilt = 0;

static void inf_fixed_tables(void)
{
    if (fixedBuilt)
    {
        return;
    }
    uint8_t litLens[288];
    for (int i = 0; i <= 143; i++) litLens[i] = 8;
    for (int i = 144; i <= 255; i++) litLens[i] = 9;
    for (int i = 256; i <= 279; i++) litLens[i] = 7;
    for (int i = 280; i <= 287; i++) litLens[i] = 8;
    inf_build_table(litLens, 288, &fixedLit);

    uint8_t distLens[32];
    for (int i = 0; i < 32; i++)
    {
        distLens[i] = 5;
    }
    inf_build_table(distLens, 32, &fixedDist);
    fixedBuilt = 1;
}

/* ── Length/distance tables (verbatim from Lua) ────────────────────────── */
static const uint16_t lengthBaseTab[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t lengthExtraTab[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t distBaseTab[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t distExtraTab[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ── Dynamic Huffman header (shared logic of Lua decompress + beginBlock) ──
 * strict=1 (streaming): cl-symbol EOF → return 0 (caller sets eof).
 * strict=0 (one-shot): cl-symbol EOF → stop filling (Lua `break`), continue. */
/* Dynamic-block scratch (512 code lengths), hoisted to BSS — see
 * inf_read_dynamic. */
static uint8_t g_infAllLens[512];

static int inf_read_dynamic(BitStream *bs, HuffTable *lit, HuffTable *dist, int strict)
{
    uint32_t hlit = bs_read_bits_or0(bs, 5) + 257;
    uint32_t hdist = bs_read_bits_or0(bs, 5) + 1;
    uint32_t hclen = bs_read_bits_or0(bs, 4) + 4;

    /* Scratch hoisted to BSS: this frame + inflate_decompress's own
     * 7.0KB reached ~11.4KB of the game-task stack in one chain.
     * Single-threaded cooperative tasks: safe to share. */
    static uint8_t codeLens[19];
    static HuffTable clTable;
    memset(codeLens, 0, sizeof(codeLens));
    for (uint32_t i = 0; i < hclen && i < 19; i++)
    {
        codeLens[clOrder[i]] = (uint8_t)bs_read_bits_or0(bs, 3);
    }
    inf_build_table(codeLens, 19, &clTable);

    /* hlit+hdist ≤ 320; a trailing run can overshoot (Lua allows it) —
     * 512 covers the worst case so overflow entries are ignored, as in
     * Lua where only 1..hlit / hlit+1..hlit+hdist are consumed. */
    uint8_t *allLens = g_infAllLens;
    int allCount = 0;
    while (allCount < (int)(hlit + hdist))
    {
        int sym = inf_decode_symbol(bs, &clTable);
        if (sym < 0)
        {
            if (strict)
            {
                return 0;
            }
            break;
        }
        if (sym < 16)
        {
            allLens[allCount++] = (uint8_t)sym;
        }
        else if (sym == 16)
        {
            uint32_t rc = bs_read_bits_or0(bs, 2) + 3;
            uint8_t last = allCount > 0 ? allLens[allCount - 1] : 0;
            while (rc-- > 0 && allCount < 512)
            {
                allLens[allCount++] = last;
            }
        }
        else if (sym == 17)
        {
            uint32_t rc = bs_read_bits_or0(bs, 3) + 3;
            while (rc-- > 0 && allCount < 512)
            {
                allLens[allCount++] = 0;
            }
        }
        else
        {
            uint32_t rc = bs_read_bits_or0(bs, 7) + 11;
            while (rc-- > 0 && allCount < 512)
            {
                allLens[allCount++] = 0;
            }
        }
    }

    uint8_t litLens[288] = {0};
    uint8_t distLens[32] = {0};
    for (uint32_t i = 0; i < hlit && i < 288; i++)
    {
        litLens[i] = i < (uint32_t)allCount ? allLens[i] : 0;
    }
    for (uint32_t j = 0; j < hdist && j < 32; j++)
    {
        size_t idx = hlit + j;
        distLens[j] = idx < (size_t)allCount ? allLens[idx] : 0;
    }
    inf_build_table(litLens, 288, lit);
    inf_build_table(distLens, 32, dist);
    return 1;
}

/* ── One-shot (Lua Inflate.decompress) ─────────────────────────────────── */
typedef struct
{
    uint8_t *d;
    size_t n, cap;
    int failed;
} OutBuf;

static void out_push(OutBuf *b, uint8_t v)
{
    if (b->failed)
    {
        return;
    }
    if (b->n == b->cap)
    {
        size_t nc = b->cap ? b->cap * 2 : 256;
        uint8_t *g = (uint8_t *)realloc(b->d, nc);
        if (!g)
        {
            b->failed = 1;
            return;
        }
        b->d = g;
        b->cap = nc;
    }
    b->d[b->n++] = v;
}

/* Dynamic-block HuffTables for the one-shot path, hoisted to BSS (each is
 * 288 entries ≈ 4.6KB; two on the stack = 9.2KB of game-task stack).
 * Single-threaded cooperative tasks: safe to share. */
static HuffTable g_dynLit;
static HuffTable g_dynDist;

uint8_t *inflate_decompress(const uint8_t *data, size_t len, size_t *outLen)
{
    logger_stack_touch();
    if (outLen)
    {
        *outLen = 0;
    }
    if (!data || len < 2)
    {
        return NULL;
    }

    /* Skip zlib header (CMF/FLG) if present. */
    int cmf = data[0];
    int flg = data[1];
    size_t startPos = 0;
    if ((cmf & 0x0F) == 8 && ((cmf * 256 + flg) % 31) == 0)
    {
        startPos = 2;
        if (flg & 0x20)
        {
            startPos += 4; /* preset dictionary */
        }
    }
    if (startPos > len)
    {
        return NULL;
    }

    BitStream bs = { data + startPos, len - startPos, 0, 0, 0 };
    OutBuf out = {0};
    int isFinal = 0;

    while (isFinal == 0)
    {
        uint32_t f;
        if (!bs_read_bits(&bs, 1, &f))
        {
            break; /* Lua: isFinal=nil → while condition false → exit */
        }
        isFinal = (int)f;
        uint32_t btype;
        if (!bs_read_bits(&bs, 2, &btype))
        {
            break; /* Lua: if not btype then break end */
        }

        if (btype == 0)
        {
            /* Uncompressed block. */
            bs_align_byte(&bs);
            uint32_t blen, bnlen;
            int okLen = bs_read_bits(&bs, 16, &blen);
            (void)bs_read_bits(&bs, 16, &bnlen); /* NLEN (unused, Lua parity) */
            if (!okLen)
            {
                break; /* Lua: if not len then break end */
            }
            for (uint32_t i = 0; i < blen; i++)
            {
                uint32_t b;
                if (!bs_read_bits(&bs, 8, &b))
                {
                    break; /* Lua: if not b then break end (inner loop) */
                }
                out_push(&out, (uint8_t)b);
            }
        }
        else if (btype == 1 || btype == 2)
        {
            const HuffTable *litT;
            const HuffTable *distT;
            HuffTable *dynLit = &g_dynLit;
            HuffTable *dynDist = &g_dynDist;
            if (btype == 1)
            {
                inf_fixed_tables();
                litT = &fixedLit;
                distT = &fixedDist;
            }
            else
            {
                if (!inf_read_dynamic(&bs, dynLit, dynDist, 0))
                {
                    /* strict=0 never returns 0; defensive only */
                    break;
                }
                litT = dynLit;
                distT = dynDist;
            }

            for (;;)
            {
                /* Tasks.yieldCheck() call site (see header note) */
                int sym = inf_decode_symbol(&bs, litT);
                if (sym < 0 || sym == 256)
                {
                    break;
                }
                if (sym < 256)
                {
                    out_push(&out, (uint8_t)sym);
                }
                else
                {
                    int li = sym - 257;
                    int baseL = (li >= 0 && li < 29) ? lengthBaseTab[li] : 3;
                    int exB = (li >= 0 && li < 29) ? lengthExtraTab[li] : 0;
                    uint32_t extraL = exB > 0 ? bs_read_bits_or0(&bs, exB) : 0;
                    int matchLen = baseL + (int)extraL;

                    int distSym = inf_decode_symbol(&bs, distT);
                    if (distSym < 0)
                    {
                        distSym = 0; /* Lua `or 0` (one-shot only) */
                    }
                    int baseD = distSym <= 29 ? distBaseTab[distSym] : 1;
                    int edB = distSym <= 29 ? distExtraTab[distSym] : 0;
                    uint32_t extraD = edB > 0 ? bs_read_bits_or0(&bs, edB) : 0;
                    int matchDist = baseD + (int)extraD;

                    long long src0 = (long long)out.n - matchDist;
                    for (int i = 0; i < matchLen; i++)
                    {
                        /* Tasks.yieldCheck() call site */
                        uint8_t b = (src0 + i >= 0 && src0 + i < (long long)out.n)
                                        ? out.d[src0 + i]
                                        : 0; /* Lua `output[idx] or 0` */
                        out_push(&out, b);
                    }
                }
            }
        }
        /* btype == 3: Lua has no branch — falls through to the next
         * while iteration (reads the next block header). Preserved. */
    }

    if (out.failed)
    {
        free(out.d);
        return NULL;
    }
    if (!out.d)
    {
        out.d = (uint8_t *)malloc(1); /* Lua returns "" for empty output */
        if (!out.d)
        {
            return NULL;
        }
    }
    if (outLen)
    {
        *outLen = out.n;
    }
    return out.d;
}

/* ── Streaming (Lua Inflate.createStream / s:read) ─────────────────────── */
#define INF_WIN_KEEP 32768
#define INF_WIN_MAX 65536

struct InflateStream
{
    BitStream bs;
    int eof;
    int mode; /* 0=block 1=uncompressed 2=huff */
    int remaining;
    int isFinalBlock;

    uint8_t *pending;
    size_t pendingN, pendingCap;
    size_t consumed; /* bytes already handed out by read() */

    uint8_t *win;
    int winN;
    long long winStart;
    long long produced;

    HuffTable litT, distT;
};

static void st_emit(InflateStream *s, uint8_t b)
{
    if (s->pendingN == s->pendingCap)
    {
        size_t nc = s->pendingCap ? s->pendingCap * 2 : 1024;
        uint8_t *g = (uint8_t *)realloc(s->pending, nc);
        if (!g)
        {
            s->eof = 1;
            return;
        }
        s->pending = g;
        s->pendingCap = nc;
    }
    s->pending[s->pendingN++] = b;

    if (s->winN < INF_WIN_MAX)
    {
        s->win[s->winN++] = b;
    }
    else
    {
        /* Lua: win = win[32769..] (keep last 32768), winStart = produced-#win */
        memmove(s->win, s->win + (INF_WIN_MAX - INF_WIN_KEEP), INF_WIN_KEEP);
        s->winN = INF_WIN_KEEP;
        s->win[s->winN++] = b;
        s->winStart = s->produced - s->winN + 1;
    }
    s->produced++;
}

static void st_emit_match(InflateStream *s, int matchLen, int matchDist)
{
    for (int i = 0; i < matchLen; i++)
    {
        long long rel = (s->produced - matchDist) - s->winStart;
        uint8_t b = (rel >= 0 && rel < s->winN) ? s->win[rel] : 0; /* win[rel] or 0 */
        st_emit(s, b);
    }
}

/* Lua beginBlock: returns 0 when eof was set. */
static int st_begin_block(InflateStream *s)
{
    uint32_t fin = 0, btype = 0;
    int okF = bs_read_bits(&s->bs, 1, &fin);
    int okB = bs_read_bits(&s->bs, 2, &btype);
    if (!okF || !okB)
    {
        s->eof = 1;
        return 0;
    }
    s->isFinalBlock = (fin == 1);
    if (btype == 0)
    {
        bs_align_byte(&s->bs);
        s->remaining = (int)bs_read_bits_or0(&s->bs, 16);
        (void)bs_read_bits_or0(&s->bs, 16); /* NLEN */
        s->mode = 1;
    }
    else if (btype == 1)
    {
        inf_fixed_tables();
        s->litT = fixedLit;
        s->distT = fixedDist;
        s->mode = 2;
    }
    else if (btype == 2)
    {
        if (!inf_read_dynamic(&s->bs, &s->litT, &s->distT, 1))
        {
            s->eof = 1;
            return 0;
        }
        s->mode = 2;
    }
    /* btype == 3: no branch in Lua — mode unchanged; the pump keeps
     * calling beginBlock, consuming bits until EOF. Preserved. */
    return 1;
}

static void st_pump(InflateStream *s, size_t n)
{
    while (!s->eof && s->pendingN < n)
    {
        /* Tasks.yieldCheck() call site (see header note) */
        if (s->mode == 1)
        {
            if (s->remaining > 0)
            {
                s->remaining--;
                uint32_t b;
                if (!bs_read_bits(&s->bs, 8, &b))
                {
                    s->eof = 1;
                    break;
                }
                st_emit(s, (uint8_t)b);
            }
            else
            {
                if (s->isFinalBlock)
                {
                    s->eof = 1;
                    break;
                }
                s->mode = 0;
            }
        }
        else if (s->mode == 0)
        {
            if (!st_begin_block(s))
            {
                break;
            }
        }
        else
        {
            int sym = inf_decode_symbol(&s->bs, &s->litT);
            if (sym < 0)
            {
                s->eof = 1;
                break;
            }
            if (sym == 256)
            {
                if (s->isFinalBlock)
                {
                    s->eof = 1;
                    break;
                }
                s->mode = 0;
            }
            else if (sym < 256)
            {
                st_emit(s, (uint8_t)sym);
            }
            else
            {
                int li = sym - 257;
                int baseL = (li >= 0 && li < 29) ? lengthBaseTab[li] : 3;
                int el = (li >= 0 && li < 29) ? lengthExtraTab[li] : 0;
                uint32_t extraL = el > 0 ? bs_read_bits_or0(&s->bs, el) : 0;
                int matchLen = baseL + (int)extraL;
                int distSym = inf_decode_symbol(&s->bs, &s->distT);
                if (distSym < 0)
                {
                    s->eof = 1; /* streaming: NO `or 0` fallback (Lua parity) */
                    break;
                }
                int baseD = distSym <= 29 ? distBaseTab[distSym] : 1;
                int ed = distSym <= 29 ? distExtraTab[distSym] : 0;
                uint32_t extraD = ed > 0 ? bs_read_bits_or0(&s->bs, ed) : 0;
                st_emit_match(s, matchLen, baseD + (int)extraD);
            }
        }
    }
}

InflateStream *inflate_stream_new(const uint8_t *data, size_t len)
{
    logger_stack_touch();
    if (!data || len < 2)
    {
        return NULL;
    }

    int cmf = data[0];
    int flg = data[1];
    size_t startPos = 0;
    if ((cmf & 0x0F) == 8 && ((cmf * 256 + flg) % 31) == 0)
    {
        startPos = 2;
        if (flg & 0x20)
        {
            startPos += 4;
        }
    }
    if (startPos > len)
    {
        return NULL;
    }

    InflateStream *s = (InflateStream *)calloc(1, sizeof(InflateStream));
    if (!s)
    {
        return NULL;
    }
    s->bs.data = data + startPos;
    s->bs.len = len - startPos;
    s->mode = 0; /* "block" */
    s->win = (uint8_t *)malloc(INF_WIN_MAX);
    if (!s->win)
    {
        free(s);
        return NULL;
    }
    return s;
}

static void st_drop_consumed(InflateStream *s)
{
    if (s->consumed > 0)
    {
        memmove(s->pending, s->pending + s->consumed, s->pendingN - s->consumed);
        s->pendingN -= s->consumed;
        s->consumed = 0;
    }
}

const uint8_t *inflate_stream_read(InflateStream *s, size_t want, size_t *outLen)
{
    if (outLen)
    {
        *outLen = 0;
    }
    if (!s)
    {
        return NULL;
    }
    if (want == 0)
    {
        want = 1; /* Lua n = n or 1 */
    }
    st_drop_consumed(s);
    st_pump(s, want);
    if (s->pendingN == 0)
    {
        return NULL;
    }
    size_t k = want < s->pendingN ? want : s->pendingN;
    if (outLen)
    {
        *outLen = k;
    }
    s->consumed = k; /* pointer stays valid until the next read/free */
    return s->pending;
}

void inflate_stream_free(InflateStream *s)
{
    if (!s)
    {
        return;
    }
    free(s->pending);
    free(s->win);
    free(s);
}
