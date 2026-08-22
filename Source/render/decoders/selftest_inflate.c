// selftest_inflate.c — [P16] selftests for the inflate port.
//
// Round-trips zlib-produced fixtures (fixed/dynamic/stored/flush blocks,
// raw deflate without a zlib header) through both the one-shot decoder
// and the streaming reader, logging results to pluto.log.

#include "render/decoders/selftest_inflate.h"

#include <string.h>

#include "core/logger.h"
#include "render/decoders/inflate.h"
#include "render/decoders/selftest_inflate_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P16] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P16] FAIL %s", name); }
}

static int bytes_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    return memcmp(a, b, n) == 0;
}

static void case_oneshot(void) {
    uint8_t* out = NULL;
    size_t outLen = 0;

    /* A: dynamic/fixed Huffman via standard zlib wrapper */
    long r = inflate_decompress(fx_A_dyn, FX_A_DYN_LEN, &out, &outLen);
    ck("O.A_dyn_roundtrip",
       r == PL_A_LEN && out != NULL &&
       bytes_eq(out, pl_A, PL_A_LEN));
    pluto_free(out); out = NULL;

    /* B: raw deflate, no zlib header (CMF check must fail -> skip 0) */
    r = inflate_decompress(fx_B_raw, FX_B_RAW_LEN, &out, &outLen);
    ck("O.B_raw_roundtrip",
       r == PL_D_LEN && out != NULL && bytes_eq(out, pl_D, PL_D_LEN));
    pluto_free(out); out = NULL;

    /* C: stored (uncompressed) blocks */
    r = inflate_decompress(fx_C_stored, FX_C_STORED_LEN, &out, &outLen);
    ck("O.C_stored_roundtrip",
       r == PL_C_LEN && out != NULL && bytes_eq(out, pl_C, PL_C_LEN));
    pluto_free(out); out = NULL;

    /* D: dynamic blocks + Z_FULL_FLUSH empty stored block mid-stream */
    r = inflate_decompress(fx_D_flush, FX_D_FLUSH_LEN, &out, &outLen);
    ck("O.D_flush_roundtrip",
       r == PL_D_LEN && out != NULL && bytes_eq(out, pl_D, PL_D_LEN));
    pluto_free(out); out = NULL;

    /* guards: too-short input rejected like Lua (#data < 2 -> nil) */
    uint8_t one[1] = { 0xAB };
    ck("O.short_input_rejected",
       inflate_decompress(one, 1, &out, &outLen) == -1);
    ck("O.empty_input_rejected",
       inflate_decompress(NULL, 0, &out, &outLen) == -1);

    /* truncated stream: returns partial output gracefully */
    r = inflate_decompress(fx_A_dyn, FX_A_DYN_LEN / 2, &out, &outLen);
    ck("O.truncated_partial", r >= 0 && outLen <= PL_A_LEN);
    pluto_free(out); out = NULL;

    /* hand-built stored block with NLEN and byte-aligned header */
    {
        /* zlib hdr 0x01 0x00 + final stored block: LEN=3 NLEN=~3 */
        uint8_t m[] = { 0x78, 0x9C, 0x01, 0x03, 0x00, 0xFC, 0xFF,
                        'a', 'b', 'c' };
        r = inflate_decompress(m, sizeof(m), &out, &outLen);
        ck("O.handmade_stored",
           r == 3 && out != NULL && memcmp(out, "abc", 3) == 0);
        pluto_free(out); out = NULL;
    }
}

static void case_stream(void) {
    InflateStream* s = inflate_stream_new(fx_D_flush, FX_D_FLUSH_LEN);
    uint8_t* acc = (uint8_t*)pluto_malloc(PL_D_LEN + 1);
    size_t got = 0;
    int ok = s != NULL && acc != NULL;
    if (ok) {
        while (got < PL_D_LEN) {
            size_t k = inflate_stream_read(s, acc + got,
                                           7);   /* awkward chunking */
            if (k == 0) break;
            got += k;
        }
        ok = got == PL_D_LEN && bytes_eq(acc, pl_D, PL_D_LEN);

        /* exhausted stream keeps returning 0 */
        ok = ok && inflate_stream_read(s, acc, 7) == 0;
    }
    ck("S.chunked_roundtrip", ok);
    inflate_stream_free(s);
    pluto_free(acc);

    /* single-shot full-size read */
    s = inflate_stream_new(fx_A_dyn, FX_A_DYN_LEN);
    acc = (uint8_t*)pluto_malloc(PL_A_LEN + 1);
    ok = s != NULL && acc != NULL;
    if (ok) {
        size_t k = inflate_stream_read(s, acc, PL_A_LEN + 8);
        ok = k == PL_A_LEN && bytes_eq(acc, pl_A, PL_A_LEN) &&
             inflate_stream_read(s, acc, 4) == 0;
    }
    ck("S.big_read_then_eof", ok);
    inflate_stream_free(s);
    pluto_free(acc);

    /* raw deflate stream (no zlib header) */
    s = inflate_stream_new(fx_B_raw, FX_B_RAW_LEN);
    acc = (uint8_t*)pluto_malloc(PL_D_LEN + 1);
    ok = s != NULL && acc != NULL;
    if (ok) {
        size_t got2 = 0;
        while (got2 < PL_D_LEN) {
            size_t k = inflate_stream_read(s, acc + got2, 1000);
            if (k == 0) break;
            got2 += k;
        }
        ok = got2 == PL_D_LEN && bytes_eq(acc, pl_D, PL_D_LEN);
    }
    ck("S.raw_stream", ok);
    inflate_stream_free(s);
    pluto_free(acc);

    /* guards */
    uint8_t buf[4];
    ck("S.guards",
       inflate_stream_new(NULL, 0) == NULL &&
       inflate_stream_read(NULL, buf, 4) == 0);
}

int selftest_inflate_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_oneshot();
    case_stream();
    PLUTO_LOG("[P16] fixtures: A=%dB->%dB B=%dB->%dB C=%dB->%dB "
              "D=%dB->%dB",
              FX_A_DYN_LEN, PL_A_LEN, FX_B_RAW_LEN, PL_D_LEN,
              FX_C_STORED_LEN, PL_C_LEN, FX_D_FLUSH_LEN, PL_D_LEN);
    PLUTO_LOG("[P16] inflate selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
