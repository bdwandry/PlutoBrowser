// selftest_bmp.c — [P18] selftests for the BMP decoder port.
//
// Decodes generated fixtures (24bpp bottom-up, 32bpp top-down,
// 8bpp short palette quirk, 4bpp nibbles, 1bpp MSB-first, forced
// downscale) checking analytic golden probes + target dims.
// Benchmarks the largest fixture and logs dims/ms to pluto.log.

#include "render/decoders/selftest_bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/bmp.h"
#include "render/decoders/selftest_bmp_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_bmp_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_bmp_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P18] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P18] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int tw, th;                       /* expected decoded grid dims */
} Fixture;

static const Fixture kFixtures[] = {
    {"b24", fx_b24, FX_B24_LEN, 64, 48},
    {"b32", fx_b32, FX_B32_LEN, 20, 10},
    {"b8", fx_b8, FX_B8_LEN, 16, 16},
    {"b4", fx_b4, FX_B4_LEN, 12, 8},
    {"b1", fx_b1, FX_B1_LEN, 24, 6},
    {"wide", fx_wide, FX_WIDE_LEN, 360, 90},
    {"bench", fx_bmp_bench, FX_BMP_BENCH_LEN, 300, 200},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

static void case_decode_all(void) {
    for (size_t i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint8_t** rows = NULL;
        int w = 0, hgt = 0;
        if (bmp_decode_gray(f->data, f->len, &rows, &w, &hgt) != 0 ||
            rows == NULL) {
            char nm[64];
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        int dimsOk = (w == f->tw && hgt == f->th);
        int probesOk = 1;
        for (int pi = 0; pi < BMP_PROBES_LEN && probesOk; pi++) {
            const BmpProbe* pr = &bmp_probes[pi];
            if (pr->img != (int)i) continue;
            for (int k = 0; k < pr->nProbes; k++) {
                int x = pr->p[k].x, y = pr->p[k].y;
                if (x >= w || y >= hgt ||
                    rows[y][x] != (uint8_t)pr->p[k].g) {
                    PLUTO_ERROR("[P18] probe %s(%d,%d): got %d want %d",
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
        bmp_free_rows(rows, hgt);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w, hgt;
    ck("G.short_input",
       bmp_decode_gray(fx_b24, 53, &rows, &w, &hgt) == -1);
    ck("G.bad_signature",
       bmp_decode_gray((const unsigned char*)"JMBJUNKJUNKJUNKJUNK"
                                          "JUNKJUNKJUNK", 54,
                       &rows, &w, &hgt) == -1);
}

static void case_benchmark(void) {
    double ms = -1.0;
#if defined(TARGET_PLAYDATE)
    struct PlaydateAPI* pd = s_pd;   /* installed via selftest_bmp_set_pd */
    if (pd != NULL) {
        float t0 = pd->system->getElapsedTime();
        uint8_t** rows = NULL;
        int w, hgt;
        bmp_decode_gray(fx_bmp_bench, FX_BMP_BENCH_LEN,
                        &rows, &w, &hgt);
        bmp_free_rows(rows, hgt);
        ms = (double)(pd->system->getElapsedTime() - t0) * 1000.0;
    }
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t** rows = NULL;
    int w, hgt;
    bmp_decode_gray(fx_bmp_bench, FX_BMP_BENCH_LEN,
                        &rows, &w, &hgt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
         (t1.tv_nsec - t0.tv_nsec) / 1e6;
    bmp_free_rows(rows, hgt);
#endif
    PLUTO_LOG("[P18] bench 600x400 -> %dx%d in %.1f ms", 300, 200, ms);
    ck("B.bench_ran", ms >= 0.0);
}

int selftest_bmp_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_benchmark();
    PLUTO_LOG("[P18] bmp selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
