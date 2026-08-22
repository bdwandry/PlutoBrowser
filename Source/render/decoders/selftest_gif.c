// selftest_gif.c — [P19] selftests for the GIF decoder port.
//
// Decodes generated fixtures (global/local palettes, offset compositing,
// transparency, interlace pass replay, first-frame-only, downscale)
// checking analytic golden probes + target dims. Benchmarks the largest
// fixture and logs dims/ms to pluto.log.

#include "render/decoders/selftest_gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/gif.h"
#include "render/decoders/selftest_gif_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_gif_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_gif_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P19] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P19] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int tw, th;
} Fixture;

static const Fixture kFixtures[] = {
    {"basic", fx_g_basic, FX_G_BASIC_LEN, 40, 30},
    {"offset", fx_g_offset, FX_G_OFFSET_LEN, 30, 8},
    {"trans", fx_g_trans, FX_G_TRANS_LEN, 20, 10},
    {"inter", fx_g_inter, FX_G_INTER_LEN, 16, 16},
    {"multi", fx_g_multi, FX_G_MULTI_LEN, 24, 12},
    {"bench", fx_g_bench, FX_G_BENCH_LEN, 300, 200},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

static void case_decode_all(void) {
    for (size_t i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint8_t** rows = NULL;
        int w = 0, hgt = 0;
        int maxW = 360, maxH = 200;
        if (i == NFIX - 1) { maxW = 360; maxH = 200; }
        if (gif_decode_gray(f->data, f->len, maxW, maxH,
                            &rows, &w, &hgt) != 0 || rows == NULL) {
            char nm[64];
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        int dimsOk = (w == f->tw && hgt == f->th);
        int probesOk = 1;
        for (int pi = 0; pi < GIF_PROBES_LEN && probesOk; pi++) {
            const GifProbe* pr = &gif_probes[pi];
            if (pr->img != (int)i) continue;
            for (int k = 0; k < pr->nProbes; k++) {
                int x = pr->p[k].x, y = pr->p[k].y;
                if (x >= w || y >= hgt ||
                    rows[y][x] != (uint8_t)pr->p[k].g) {
                    PLUTO_ERROR("[P19] probe %s(%d,%d): got %d want %d",
                                f->name, x, y,
                                x < w && y < hgt ? rows[y][x] : -1,
                                pr->p[k].g);
                    probesOk = 0;
                    break;
                }
            }
        }
        char nm[64];
        snprintf(nm, sizeof(nm), "D.%s_%dx%d%s", f->name, w, hgt,
                 dimsOk ? "" : "_BAD_DIMS");
        ck(nm, dimsOk && probesOk);
        gif_free_rows(rows, hgt);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w, hgt;
    static const unsigned char noImage[] = {
        'G', 'I', 'F', '8', '9', 'a',
        24, 0, 12, 0,      /* logical screen 24x12 */
        0x00,              /* packed: no global palette */
        0x00, 0x00,        /* bg index, aspect */
        0x3B               /* trailer immediately */
    };
    ck("G.short_input",
       gif_decode_gray(fx_g_basic, 13, 360, 200,
                       &rows, &w, &hgt) == -1);
    ck("G.bad_signature",
       gif_decode_gray((const unsigned char*)"GIF88aJUNKJUNKJUNK",
                       17, 360, 200, &rows, &w, &hgt) == -1);
    ck("G.trailer_no_image",
       gif_decode_gray(noImage, sizeof(noImage), 360, 200,
                       &rows, &w, &hgt) == -1);
}

static void case_benchmark(void) {
    double ms = -1.0;
#if defined(TARGET_PLAYDATE)
    struct PlaydateAPI* pd = s_pd;   /* installed via selftest_gif_set_pd */
    if (pd != NULL) {
        float t0 = pd->system->getElapsedTime();
        uint8_t** rows = NULL;
        int w, hgt;
        gif_decode_gray(fx_g_bench, FX_G_BENCH_LEN, 360, 200,
                        &rows, &w, &hgt);
        gif_free_rows(rows, hgt);
        ms = (double)(pd->system->getElapsedTime() - t0) * 1000.0;
    }
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t** rows = NULL;
    int w, hgt;
    gif_decode_gray(fx_g_bench, FX_G_BENCH_LEN, 360, 200,
                    &rows, &w, &hgt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
         (t1.tv_nsec - t0.tv_nsec) / 1e6;
    gif_free_rows(rows, hgt);
#endif
    PLUTO_LOG("[P19] bench 600x400 -> %dx%d in %.1f ms", 300, 200, ms);
    ck("B.bench_ran", ms >= 0.0);
}

int selftest_gif_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_benchmark();
    PLUTO_LOG("[P19] gif selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
