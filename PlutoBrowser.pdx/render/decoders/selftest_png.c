// selftest_png.c — [P17] selftests for the PNG decoder port.
//
// Decodes generated fixtures (solid colors, alpha compositing, palette +
// tRNS, 1/8/16-bit grayscale, gray+alpha, Adam7 first pass, all five
// filter types) and checks analytic golden probes + downscaled dims.
// Benchmarks the largest fixture and logs dims/ms to pluto.log.

#include "render/decoders/selftest_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#include <time.h>
#endif

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/png.h"
#include "render/decoders/selftest_png_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_png_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_png_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P17] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P17] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int tw, th;                       /* expected decoded grid dims */
} Fixture;

static const Fixture kFixtures[] = {
    {"red", fx_red, FX_RED_LEN, 64, 48},
    {"rgba", fx_rgba, FX_RGBA_LEN, 40, 30},
    {"pal", fx_pal, FX_PAL_LEN, 32, 16},
    {"gray8", fx_gray8, FX_GRAY8_LEN, 100, 10},
    {"gray1", fx_gray1, FX_GRAY1_LEN, 32, 8},
    {"ga", fx_ga, FX_GA_LEN, 24, 12},
    {"gray16", fx_gray16, FX_GRAY16_LEN, 16, 4},
    {"adam7", fx_adam7, FX_ADAM7_LEN, 5, 3},
    {"bench", fx_bench, FX_BENCH_LEN, 266, 200},
    {"down", fx_down, FX_DOWN_LEN, 50, 40},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

/* decode every fixture once; cache the bench result for the timing run */
static uint8_t** g_benchRows = NULL;
static int g_benchW, g_benchH;

static void case_decode_all(void) {
    for (size_t i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint8_t** rows = NULL;
        int w = 0, hgt = 0;
        int rc = png_decode_gray(f->data, f->len,
                                 i == NFIX - 1 ? 50 : 360,   /* down */
                                 i == NFIX - 1 ? 40 : 200,
                                 &rows, &w, &hgt);
        if (rc != 0 || rows == NULL) {
            char nm[64];
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        int dimsOk = (w == f->tw && hgt == f->th);
        int probesOk = 1;
        for (int pi = 0; pi < PNG_PROBES_LEN && probesOk; pi++) {
            const PngProbe* pr = &png_probes[pi];
            if (pr->img != (int)i) continue;
            for (int k = 0; k < pr->nProbes; k++) {
                int x = pr->p[k].x, y = pr->p[k].y;
                if (x >= w || y >= hgt ||
                    rows[y][x] != (uint8_t)pr->p[k].g) {
                    PLUTO_ERROR("[P17] probe %s(%d,%d): got %d want %d",
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

        if (i == NFIX - 2) {           /* keep bench grid for timing */
            g_benchRows = rows;
            g_benchW = w;
            g_benchH = hgt;
            continue;
        }
        png_free_rows(rows, hgt);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w, hgt;
    ck("G.short_input",
       png_decode_gray(fx_red, 23, 360, 200, &rows, &w, &hgt) == -1);
    ck("G.bad_signature",
       png_decode_gray((const unsigned char*)"JUNKJUNKJUNKJUNKJUNK"
                                          "JUNKJUNK", 24,
                       360, 200, &rows, &w, &hgt) == -1);
}

static void case_benchmark(void) {
    if (g_benchRows == NULL) {
        ck("B.bench_ran", 0);
        return;
    }
    double ms = -1.0;
#if defined(TARGET_PLAYDATE)
    struct PlaydateAPI* pd = s_pd;   /* installed via selftest_png_set_pd */
    if (pd != NULL) {
        float t0 = pd->system->getElapsedTime();
        uint8_t** rows = NULL;
        int w, hgt;
        png_decode_gray(fx_bench, FX_BENCH_LEN, 360, 200,
                        &rows, &w, &hgt);
        png_free_rows(rows, hgt);
        ms = (double)(pd->system->getElapsedTime() - t0) * 1000.0;
    }
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t** rows = NULL;
    int w, hgt;
    png_decode_gray(fx_bench, FX_BENCH_LEN, 360, 200, &rows, &w, &hgt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
         (t1.tv_nsec - t0.tv_nsec) / 1e6;
    png_free_rows(rows, hgt);
#endif
    PLUTO_LOG("[P17] bench 800x600 -> %dx%d in %.1f ms",
              g_benchW, g_benchH, ms);
    ck("B.bench_grid_alive", g_benchRows[0] != NULL &&
                             g_benchW == 266 && g_benchH == 200);
}

int selftest_png_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_benchmark();
    if (g_benchRows != NULL) png_free_rows(g_benchRows, g_benchH);
    g_benchRows = NULL;

    PLUTO_LOG("[P17] png selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
