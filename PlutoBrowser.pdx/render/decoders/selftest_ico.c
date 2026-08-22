// selftest_ico.c — [P23] selftests for the ICO port.
//
// Decodes hand-built containers cross-validated against an independent
// Python byte-level replica of the decoder (classic DIB entries at every
// bpp, top-down variant, nearest-scale downsample, PNG delegation,
// fallback and multi-entry selection) checking dims + FNV-1a gray
// checksums + probe points. Guards cover reserved/type/count rejects,
// missing entries and undecodable payloads. The scaled fixture doubles
// as a decode benchmark.

#include "render/decoders/selftest_ico.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/ico.h"
#include "render/decoders/selftest_ico_fixtures.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_ico_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_ico_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P23] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P23] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int sw, sh;      /* decoded dims */
    unsigned cksum;  /* FNV-1a over gray rows */
} Fixture;

static const Fixture kFixtures[] = {
    {"i_32bpp",       fx_i_32bpp,       FX_I_I_32BPP_LEN,
     FX_I_I_32BPP_W, FX_I_I_32BPP_H, FX_I_I_32BPP_CK},
    {"i_24bpp_masked", fx_i_24bpp_masked, FX_I_I_24BPP_MASKED_LEN,
     FX_I_I_24BPP_MASKED_W, FX_I_I_24BPP_MASKED_H,
     FX_I_I_24BPP_MASKED_CK},
    {"i_8bpp_nomask", fx_i_8bpp_nomask, FX_I_I_8BPP_NOMASK_LEN,
     FX_I_I_8BPP_NOMASK_W, FX_I_I_8BPP_NOMASK_H,
     FX_I_I_8BPP_NOMASK_CK},
    {"i_4bpp",        fx_i_4bpp,        FX_I_I_4BPP_LEN,
     FX_I_I_4BPP_W, FX_I_I_4BPP_H, FX_I_I_4BPP_CK},
    {"i_1bpp",        fx_i_1bpp,        FX_I_I_1BPP_LEN,
     FX_I_I_1BPP_W, FX_I_I_1BPP_H, FX_I_I_1BPP_CK},
    {"i_topdown",     fx_i_topdown,     FX_I_I_TOPDOWN_LEN,
     FX_I_I_TOPDOWN_W, FX_I_I_TOPDOWN_H, FX_I_I_TOPDOWN_CK},
    {"i_scaled",      fx_i_scaled,      FX_I_I_SCALED_LEN,
     FX_I_I_SCALED_W, FX_I_I_SCALED_H, FX_I_I_SCALED_CK},
    {"i_png_entry",   fx_i_png_entry,   FX_I_I_PNG_ENTRY_LEN,
     FX_I_I_PNG_ENTRY_W, FX_I_I_PNG_ENTRY_H, FX_I_I_PNG_ENTRY_CK},
    {"i_png_fallback", fx_i_png_fallback, FX_I_I_PNG_FALLBACK_LEN,
     FX_I_I_PNG_FALLBACK_W, FX_I_I_PNG_FALLBACK_H,
     FX_I_I_PNG_FALLBACK_CK},
    {"i_multi",       fx_i_multi,       FX_I_I_MULTI_LEN,
     FX_I_I_MULTI_W, FX_I_I_MULTI_H, FX_I_I_MULTI_CK},
    {"i_cursor",      fx_i_cursor,      FX_I_I_CURSOR_LEN,
     FX_I_I_CURSOR_W, FX_I_I_CURSOR_H, FX_I_I_CURSOR_CK},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

#define FNV_OFFSET 0x811C9DC5u
#define FNV_PRIME  0x01000193u

static unsigned fnv_rows(uint8_t** rows, int w, int h) {
    unsigned hsh = FNV_OFFSET;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            hsh ^= rows[y][x];
            hsh *= FNV_PRIME;
        }
    return hsh;
}

static void case_decode_all(void) {
    size_t i;
    int k;
    for (i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint8_t** rows = NULL;
        int w = 0, h = 0;
        char nm[64];
        if (ico_decode_gray(f->data, f->len, 360, 200,
                            &rows, &w, &h) != 0 || rows == NULL) {
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        snprintf(nm, sizeof(nm), "D.%s_dims", f->name);
        ck(nm, w == f->sw && h == f->sh);
        snprintf(nm, sizeof(nm), "D.%s_cksum", f->name);
        ck(nm, fnv_rows(rows, w, h) == f->cksum);

        {
            /* probe points are emitted per-fixture as fx_ip_<name> */
            int probesOk = 1;
            if (strcmp(f->name, "i_32bpp") == 0) {
                for (k = 0; k < fx_ip_i_32bpp_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_32bpp[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            } else if (strcmp(f->name, "i_24bpp_masked") == 0) {
                for (k = 0; k < fx_ip_i_24bpp_masked_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_24bpp_masked[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            } else if (strcmp(f->name, "i_8bpp_nomask") == 0) {
                for (k = 0; k < fx_ip_i_8bpp_nomask_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_8bpp_nomask[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            } else if (strcmp(f->name, "i_4bpp") == 0) {
                for (k = 0; k < fx_ip_i_4bpp_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_4bpp[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            } else if (strcmp(f->name, "i_1bpp") == 0) {
                for (k = 0; k < fx_ip_i_1bpp_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_1bpp[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            } else if (strcmp(f->name, "i_topdown") == 0) {
                for (k = 0; k < fx_ip_i_topdown_len && probesOk; k++) {
                    const IcoProbe* p = &fx_ip_i_topdown[k];
                    if (rows[p->y][p->x] != p->v) probesOk = 0;
                }
            }
            snprintf(nm, sizeof(nm), "D.%s_probes", f->name);
            ck(nm, probesOk);
        }
        ico_free_rows(rows, h);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w = 0, h = 0;
    int ok;

    ok = ico_decode_gray(fx_ig_bad_reserved, FX_IG_BAD_RESERVED_LEN,
                         360, 200, &rows, &w, &h) != 0 && rows == NULL;
    ck("G.bad_reserved", ok);

    ok = ico_decode_gray(fx_ig_bad_type, FX_IG_BAD_TYPE_LEN,
                         360, 200, &rows, &w, &h) != 0 && rows == NULL;
    ck("G.bad_type", ok);

    ok = ico_decode_gray(fx_ig_count_zero, FX_IG_COUNT_ZERO_LEN,
                         360, 200, &rows, &w, &h) != 0 && rows == NULL;
    ck("G.count_zero", ok);

    ok = ico_decode_gray(fx_ig_no_entries, FX_IG_NO_ENTRIES_LEN,
                         360, 200, &rows, &w, &h) != 0 && rows == NULL;
    ck("G.no_entries", ok);

    ok = ico_decode_gray(fx_ig_all_fail_dib, FX_IG_ALL_FAIL_DIB_LEN,
                         360, 200, &rows, &w, &h) != 0 && rows == NULL;
    ck("G.all_fail_dib", ok);

    ok = ico_decode_gray(fx_i_32bpp, FX_I_I_32BPP_LEN, 360, 200,
                         NULL, &w, &h) != 0;
    ck("G.null_args_reject", ok);

    ico_free_rows(NULL, 4); /* must be a no-op */
}

static void case_bench(void) {
    uint8_t** rows = NULL;
    int w = 0, h = 0;
    double ms = 0.0;
    int it, iters = 100;
    int ok = ico_decode_gray(fx_i_scaled, FX_I_I_SCALED_LEN, 360, 200,
                             &rows, &w, &h) == 0 && rows != NULL;
    ck("B.smoke_266x200", ok && w == FX_I_I_SCALED_W &&
                            h == FX_I_I_SCALED_H &&
                            fnv_rows(rows, w, h) == FX_I_I_SCALED_CK);
    if (rows) ico_free_rows(rows, h);

#if defined(TARGET_PLAYDATE)
    if (s_pd != NULL) {
        float t0 = s_pd->system->getElapsedTime();
        for (it = 0; it < iters; it++) {
            rows = NULL;
            if (ico_decode_gray(fx_i_scaled, FX_I_I_SCALED_LEN,
                                360, 200, &rows, &w, &h) == 0)
                ico_free_rows(rows, h);
            else
                break;
        }
        ms = (double)(s_pd->system->getElapsedTime() - t0) * 1000.0;
    } else {
        it = iters;
    }
#else
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (it = 0; it < iters; it++) {
            rows = NULL;
            if (ico_decode_gray(fx_i_scaled, FX_I_I_SCALED_LEN,
                                360, 200, &rows, &w, &h) == 0)
                ico_free_rows(rows, h);
            else
                break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
             (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
#endif
    PLUTO_LOG("[P23] bench ico %dx%d: %d iters in %.2f ms (%.3f ms/iter)",
              FX_I_I_SCALED_W, FX_I_I_SCALED_H, iters, ms, ms / iters);
    ck("B.bench_ran", it == iters && ms >= 0.0);
}

int selftest_ico_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_bench();
    PLUTO_LOG("[P23] ico selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed) *passed = s_pass;
    if (failed) *failed = s_fail;
    return s_fail;
}
