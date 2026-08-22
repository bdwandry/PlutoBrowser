// selftest_jpeg.c — [P20] selftests for the JPEG decoder port.
//
// Decodes hand-crafted fixtures with exactly-known coefficients (baseline
// full-IDCT, DC-only shortcut, quant scaling, restart intervals, 4:2:0
// subsampling with chroma-sync consumption, progressive DC-only renders,
// multi-scan early render, DC refinement scans) checking golden probes +
// target dims. Guards cover signature/EOI/SOF9/truncation rejects. The
// Pillow-encoded real-world file is smoke-decoded and benchmarked.

#include "render/decoders/selftest_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/selftest_jpeg_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_jpeg_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_jpeg_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P20] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P20] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int tw, th;
} Fixture;

static const Fixture kFixtures[] = {
    {"flat", fx_j_flat, FX_J_FLAT_LEN,
     FX_J_FLAT_TW, FX_J_FLAT_TH},
    {"dcstripes", fx_j_dcstripes, FX_J_DCSTRIPES_LEN,
     FX_J_DCSTRIPES_TW, FX_J_DCSTRIPES_TH},
    {"acblocks", fx_j_acblocks, FX_J_ACBLOCKS_LEN,
     FX_J_ACBLOCKS_TW, FX_J_ACBLOCKS_TH},
    {"restart", fx_j_restart, FX_J_RESTART_LEN,
     FX_J_RESTART_TW, FX_J_RESTART_TH},
    {"sub420", fx_j_sub420, FX_J_SUB420_LEN,
     FX_J_SUB420_TW, FX_J_SUB420_TH},
    {"prog_dc", fx_j_prog_dc, FX_J_PROG_DC_LEN,
     FX_J_PROG_DC_TW, FX_J_PROG_DC_TH},
    {"prog_multi", fx_j_prog_multi, FX_J_PROG_MULTI_LEN,
     FX_J_PROG_MULTI_TW, FX_J_PROG_MULTI_TH},
    {"prog_refine", fx_j_prog_refine, FX_J_PROG_REFINE_LEN,
     FX_J_PROG_REFINE_TW, FX_J_PROG_REFINE_TH},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

static void case_decode_all(void) {
    for (size_t i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint8_t** rows = NULL;
        int w = 0, hgt = 0;
        if (jpeg_decode_gray(f->data, f->len, 360, 200,
                             &rows, &w, &hgt) != 0 || rows == NULL) {
            char nm[64];
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        int dimsOk = (w == f->tw && hgt == f->th);
        int probesOk = 1;
        for (int pi = 0; pi < JPEG_PROBES_LEN && probesOk; pi++) {
            const JpegProbe* pr = &jpeg_probes[pi];
            if (pr->img != (int)i) continue;
            for (int k = 0; k < pr->nProbes; k++) {
                int x = pr->p[k].x, y = pr->p[k].y;
                if (x >= w || y >= hgt ||
                    rows[y][x] != (uint8_t)pr->p[k].g) {
                    PLUTO_ERROR("[P20] probe %s(%d,%d): got %d want %d",
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
        jpeg_free_rows(rows, hgt);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w, hgt;
    ck("G.short_input",
       jpeg_decode_gray(fx_j_flat, 3, 360, 200,
                        &rows, &w, &hgt) == -1);
    ck("G.bad_signature",
       jpeg_decode_gray((const unsigned char*)"NOTAJPEG-not-a-jpeg", 19,
                        360, 200, &rows, &w, &hgt) == -1);
    ck("G.eoi_only",
       jpeg_decode_gray(fx_j_eoi_only, FX_J_EOI_ONLY_LEN,
                        360, 200, &rows, &w, &hgt) == -1);
    ck("G.sof9_arithmetic",
       jpeg_decode_gray(fx_j_sof9, FX_J_SOF9_LEN,
                        360, 200, &rows, &w, &hgt) == -1);
    ck("G.trunc_sof",
       jpeg_decode_gray(fx_j_trunc_sof, FX_J_TRUNC_SOF_LEN,
                        360, 200, &rows, &w, &hgt) == -1);
}

static void case_benchmark(void) {
    /* Smoke: the real-world Pillow file must decode to 266x200. */
    uint8_t** rows = NULL;
    int w = 0, hgt = 0;
    int ok = jpeg_decode_gray(fx_j_pil_bench, FX_J_PIL_BENCH_LEN,
                              360, 200, &rows, &w, &hgt) == 0 &&
             rows != NULL && w == 266 && hgt == 200;
    if (rows) jpeg_free_rows(rows, hgt);
    ck("D.pil_smoke_266x200", ok);

    double ms = -1.0;
#if defined(TARGET_PLAYDATE)
    struct PlaydateAPI* pd = s_pd;   /* installed via selftest_jpeg_set_pd */
    if (pd != NULL) {
        float t0 = pd->system->getElapsedTime();
        rows = NULL;
        jpeg_decode_gray(fx_j_pil_bench, FX_J_PIL_BENCH_LEN,
                         360, 200, &rows, &w, &hgt);
        jpeg_free_rows(rows, hgt);
        ms = (double)(pd->system->getElapsedTime() - t0) * 1000.0;
    }
#else
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rows = NULL;
    jpeg_decode_gray(fx_j_pil_bench, FX_J_PIL_BENCH_LEN,
                     360, 200, &rows, &w, &hgt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
         (t1.tv_nsec - t0.tv_nsec) / 1e6;
    jpeg_free_rows(rows, hgt);
#endif
    PLUTO_LOG("[P20] bench 800x600 -> %dx%d in %.1f ms", w, hgt, ms);
    ck("B.bench_ran", ms >= 0.0);
}

int selftest_jpeg_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_benchmark();
    PLUTO_LOG("[P20] jpeg selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
