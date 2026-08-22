// selftest_webp.c — [P21] selftests for the VP8L (lossless WebP) port.
//
// Decodes Pillow-encoded lossless files cross-validated offline against
// dwebp (flat, gradient, palette/color-indexing, alpha ramp, predictor
// blobs, structured noise for LZ77/cache, large multi-region for meta
// Huffman groups) checking dims + FNV-1a ARGB checksums + probe points.
// Guards cover bad signature, short RIFF and truncation rejects. The
// bench-sized file is smoke-decoded and benchmarked.

#include "render/decoders/selftest_webp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/scale.h"
#include "render/decoders/webp.h"
#include "render/decoders/selftest_webp_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_webp_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_webp_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P21] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P21] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const unsigned char* data;
    size_t len;
    int sw, sh;      /* source dims */
    unsigned cksum;  /* FNV-1a over full-res ARGB rows */
    int genIdx;      /* index into webp_probes[] img ids */
} Fixture;

static const Fixture kFixtures[] = {
    {"flat",      fx_w_flat,      FX_W_FLAT_LEN,
     FX_W_FLAT_W, FX_W_FLAT_H, FX_W_FLAT_CK, 0},
    {"gradient",  fx_w_gradient,  FX_W_GRADIENT_LEN,
     FX_W_GRADIENT_W, FX_W_GRADIENT_H, FX_W_GRADIENT_CK, 1},
    {"palette",   fx_w_palette,   FX_W_PALETTE_LEN,
     FX_W_PALETTE_W, FX_W_PALETTE_H, FX_W_PALETTE_CK, 2},
    {"alpha",     fx_w_alpha,     FX_W_ALPHA_LEN,
     FX_W_ALPHA_W, FX_W_ALPHA_H, FX_W_ALPHA_CK, 3},
    {"predictor", fx_w_predictor, FX_W_PREDICTOR_LEN,
     FX_W_PREDICTOR_W, FX_W_PREDICTOR_H, FX_W_PREDICTOR_CK, 4},
    {"cache",     fx_w_cache,     FX_W_CACHE_LEN,
     FX_W_CACHE_W, FX_W_CACHE_H, FX_W_CACHE_CK, 5},
    {"meta",      fx_w_meta,      FX_W_META_LEN,
     FX_W_META_W, FX_W_META_H, FX_W_META_CK, 6},
    {"lossy_flat", fx_w_lossy_flat, FX_W_LOSSY_FLAT_LEN,
     FX_W_LOSSY_FLAT_W, FX_W_LOSSY_FLAT_H, FX_W_LOSSY_FLAT_CK, 8},
    {"lossy_grad", fx_w_lossy_grad, FX_W_LOSSY_GRAD_LEN,
     FX_W_LOSSY_GRAD_W, FX_W_LOSSY_GRAD_H, FX_W_LOSSY_GRAD_CK, 9},
    {"lossy_alpha", fx_w_lossy_alpha, FX_W_LOSSY_ALPHA_LEN,
     FX_W_LOSSY_ALPHA_W, FX_W_LOSSY_ALPHA_H, FX_W_LOSSY_ALPHA_CK, 10},
};
#define NFIX (sizeof(kFixtures) / sizeof(kFixtures[0]))

#define FNV_OFFSET 0x811C9DC5u
#define FNV_PRIME  0x01000193u

static unsigned fnv_rows(uint32_t** rows, int w, int h) {
    unsigned hsh = FNV_OFFSET;
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            hsh ^= rows[y][x];
            hsh *= FNV_PRIME;
        }
    }
    return hsh;
}

static unsigned fnv_flat(const uint32_t* pix, int w, int h) {
    unsigned hsh = FNV_OFFSET;
    size_t i, n = (size_t)w * (size_t)h;
    for (i = 0; i < n; i++) {
        hsh ^= pix[i];
        hsh *= FNV_PRIME;
    }
    return hsh;
}

static void case_decode_argb_all(void) {
    size_t i;
    int pi;
    for (i = 0; i < NFIX; i++) {
        const Fixture* f = &kFixtures[i];
        uint32_t** rows = NULL;
        int w = 0, hgt = 0;
        char nm[64];
        if (webp_decode_argb(f->data, f->len, 360, 200,
                             &rows, &w, &hgt) != 0 || rows == NULL) {
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        snprintf(nm, sizeof(nm), "D.%s_dims", f->name);
        ck(nm, w == f->sw && hgt == f->sh);
        snprintf(nm, sizeof(nm), "D.%s_cksum", f->name);
        ck(nm, fnv_rows(rows, w, hgt) == f->cksum);

        {
            int probesOk = 1;
            for (pi = 0; pi < webp_probes_len && probesOk; pi++) {
                const WebpProbe* pr = &webp_probes[pi];
                int k;
                if (pr->img != f->genIdx) continue;
                for (k = 0; k < pr->nProbes; k++) {
                    int x = pr->p[k].x, y = pr->p[k].y;
                    unsigned got, want = pr->p[k].argb;
                    if (x >= w || y >= hgt) { probesOk = 0; break; }
                    got = rows[y][x];
                    if (got != want) {
                        PLUTO_ERROR(
                            "[P21] probe %s(%d,%d): got %08X want %08X",
                            f->name, x, y, got, want);
                        probesOk = 0;
                        break;
                    }
                }
            }
            snprintf(nm, sizeof(nm), "D.%s_probes", f->name);
            ck(nm, probesOk);
        }
        webp_free_rows_argb(rows, hgt);
    }
}

static void case_gray_smoke(void) {
    uint32_t** argb = NULL;
    uint8_t** rows = NULL;
    int aw = 0, ah = 0, gw = 0, gh = 0;
    int bw, bh, tw, th;
    int ok;

    /* gray pipeline consistency on the gradient fixture */
    ok = webp_decode_gray(fx_w_gradient, FX_W_GRADIENT_LEN, 360, 200,
                          &rows, &gw, &gh) == 0 && rows != NULL;
    ck("G.gradient_decode", ok);
    scale_box_sizes(FX_W_GRADIENT_W, FX_W_GRADIENT_H, 360, 200,
                    &bw, &bh, &tw, &th);
    ck("G.gradient_dims", ok && gw == tw && gh == th);
    if (rows) webp_free_rows(rows, gh);
    rows = NULL;

    /* alpha compositing path runs without crashing on the alpha ramp */
    ok = webp_decode_argb(fx_w_alpha, FX_W_ALPHA_LEN, 24, 16,
                          &argb, &aw, &ah) == 0 && argb != NULL;
    ck("G.alpha_argb", ok);
    if (argb) webp_free_rows_argb(argb, ah);

    /* tiny flat image stays exact through the box filter (no scaling) */
    ok = webp_decode_gray(fx_w_flat, FX_W_FLAT_LEN, 64, 64,
                          &rows, &gw, &gh) == 0 && rows != NULL;
    scale_box_sizes(FX_W_FLAT_W, FX_W_FLAT_H, 64, 64, &bw, &bh, &tw, &th);
    ck("G.flat_dims", ok && gw == tw && gh == th);
    if (rows) webp_free_rows(rows, gh);
}

static void case_anim(void) {
    WebPAnim* anim = NULL;
    const int durs[4] = {FX_W_ANIM_DUR0, FX_W_ANIM_DUR1,
                         FX_W_ANIM_DUR2, FX_W_ANIM_DUR3};
    const unsigned cks[4] = {FX_W_ANIM_F0_CK, FX_W_ANIM_F1_CK,
                             FX_W_ANIM_F2_CK, FX_W_ANIM_F3_CK};
    const WebpProbePt* pts[4] = {wp_anim_f0, wp_anim_f1,
                                 wp_anim_f2, wp_anim_f3};
    const int plens[4] = {wp_anim_f0_len, wp_anim_f1_len,
                          wp_anim_f2_len, wp_anim_f3_len};
    int i;

    ck("A.anim_badsig_reject",
       webp_decode_anim(fx_w_badsig, FX_W_BADSIG_LEN, &anim) != 0 &&
       anim == NULL);
    ck("A.decode",
       webp_decode_anim(fx_w_anim, FX_W_ANIM_LEN, &anim) == 0 &&
       anim != NULL);
    if (!anim) return;
    ck("A.dims",
       anim->width == FX_W_ANIM_W && anim->height == FX_W_ANIM_H);
    ck("A.meta",
       anim->loopCount == FX_W_ANIM_LOOP &&
       anim->numFrames == FX_W_ANIM_NFRAMES &&
       anim->bgcolor == FX_W_ANIM_BG);

    {
        int ok = anim->numFrames <= 4;
        for (i = 0; i < anim->numFrames && ok; i++)
            if (anim->frames[i].durationMs != durs[i]) ok = 0;
        ck("A.durations", ok);
    }
    {
        int okCk = 1, okPr = 1;
        for (i = 0; i < anim->numFrames && okCk && okPr; i++) {
            int k;
            char nm[64];
            snprintf(nm, sizeof(nm), "A.f%d_cksum", i);
            ck(nm, fnv_flat(anim->frames[i].pix, anim->width,
                            anim->height) == cks[i]);
            for (k = 0; k < plens[i]; k++) {
                int x = pts[i][k].x, y = pts[i][k].y;
                unsigned got = anim->frames[i]
                    .pix[(size_t)y * anim->width + x];
                if (got != pts[i][k].argb) {
                    PLUTO_ERROR("[P21] anim f%d(%d,%d): got %08X want %08X",
                                i, x, y, got, pts[i][k].argb);
                    okPr = 0;
                    break;
                }
            }
            if (!okPr) break;
        }
        ck("A.probes", okPr);
    }
    webp_anim_free(anim);
    webp_anim_free(NULL); /* must be a no-op */
}

static void case_guards(void) {
    uint32_t** rows = NULL;
    int w = 0, h = 0;
    ck("X.badsig_reject",
       webp_decode_argb(fx_w_badsig, FX_W_BADSIG_LEN, 64, 64,
                        &rows, &w, &h) != 0 && rows == NULL);
    ck("X.riff_short_reject",
       webp_decode_argb(fx_w_riff_short, FX_W_RIFF_SHORT_LEN, 64, 64,
                        &rows, &w, &h) != 0 && rows == NULL);
    ck("X.trunc_reject",
       webp_decode_argb(fx_w_trunc, FX_W_TRUNC_LEN, 64, 64,
                        &rows, &w, &h) != 0 && rows == NULL);
    ck("X.lossy_trunc_reject",
       webp_decode_argb(fx_w_lossy_trunc, FX_W_LOSSY_TRUNC_LEN, 64, 64,
                        &rows, &w, &h) != 0 && rows == NULL);
    ck("X.null_args_reject",
       webp_decode_argb(fx_w_flat, FX_W_FLAT_LEN, 64, 64,
                        NULL, &w, &h) != 0);
}

static void case_pil_bench(void) {
    uint32_t** rows = NULL;
    int w = 0, h = 0;
    double ms = 0.0;
    int it, iters = 30;
    int ok = webp_decode_argb(fx_w_pil_bench, FX_W_PIL_BENCH_LEN, 360, 200,
                              &rows, &w, &h) == 0 && rows != NULL;
    ck("pil_smoke_266x200", ok && w == FX_W_PIL_BENCH_W &&
                                h == FX_W_PIL_BENCH_H &&
                                fnv_rows(rows, w, h) == FX_W_PIL_BENCH_CK);
    if (rows) webp_free_rows_argb(rows, h);

#if defined(TARGET_PLAYDATE)
    if (s_pd != NULL) {
        float t0 = s_pd->system->getElapsedTime();
        for (it = 0; it < iters; it++) {
            rows = NULL;
            if (webp_decode_argb(fx_w_pil_bench, FX_W_PIL_BENCH_LEN,
                                 360, 200, &rows, &w, &h) == 0)
                webp_free_rows_argb(rows, h);
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
            if (webp_decode_argb(fx_w_pil_bench, FX_W_PIL_BENCH_LEN,
                                 360, 200, &rows, &w, &h) == 0)
                webp_free_rows_argb(rows, h);
            else
                break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
             (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
#endif
    PLUTO_LOG("[P21] bench %dx%d: %d iters in %.2f ms (%.3f ms/iter)",
              FX_W_PIL_BENCH_W, FX_W_PIL_BENCH_H, iters, ms, ms / iters);
    ck("B.bench_ran", it == iters && ms >= 0.0);
}

static void case_lossy_bench(void) {
    uint32_t** rows = NULL;
    int w = 0, h = 0;
    double ms = 0.0;
    int it, iters = 200;
    int ok = webp_decode_argb(fx_w_lossy_grad, FX_W_LOSSY_GRAD_LEN,
                              FX_W_LOSSY_GRAD_W, FX_W_LOSSY_GRAD_H,
                              &rows, &w, &h) == 0 && rows != NULL;
    ck("B.lossy_smoke", ok && w == FX_W_LOSSY_GRAD_W &&
                            h == FX_W_LOSSY_GRAD_H &&
                            fnv_rows(rows, w, h) == FX_W_LOSSY_GRAD_CK);
    if (rows) webp_free_rows_argb(rows, h);

#if defined(TARGET_PLAYDATE)
    if (s_pd != NULL) {
        float t0 = s_pd->system->getElapsedTime();
        for (it = 0; it < iters; it++) {
            rows = NULL;
            if (webp_decode_argb(fx_w_lossy_grad, FX_W_LOSSY_GRAD_LEN,
                                 FX_W_LOSSY_GRAD_W, FX_W_LOSSY_GRAD_H,
                                 &rows, &w, &h) == 0)
                webp_free_rows_argb(rows, h);
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
            if (webp_decode_argb(fx_w_lossy_grad, FX_W_LOSSY_GRAD_LEN,
                                 FX_W_LOSSY_GRAD_W, FX_W_LOSSY_GRAD_H,
                                 &rows, &w, &h) == 0)
                webp_free_rows_argb(rows, h);
            else
                break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
             (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
#endif
    PLUTO_LOG("[P22] bench lossy %dx%d: %d iters in %.2f ms (%.3f ms/iter)",
              FX_W_LOSSY_GRAD_W, FX_W_LOSSY_GRAD_H, iters, ms, ms / iters);
    ck("B.lossy_bench_ran", it == iters && ms >= 0.0);
}

int selftest_webp_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_argb_all();
    case_gray_smoke();
    case_guards();
    case_anim();
    case_pil_bench();
    case_lossy_bench();
    PLUTO_LOG("[P21] webp selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed) *passed = s_pass;
    if (failed) *failed = s_fail;
    return s_fail;
}
