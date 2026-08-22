// selftest_svg.c — [P24] selftests for the SVG renderer port.
//
// Decodes SVG fixtures cross-validated against an independent Python
// replica of the C rasterizer+parser spec (rects incl. rounded corners,
// circles, ringed ellipses, lines, polylines/polygons, path commands
// M/L/H/V/Z/C/S/Q/T/A, use/defs expansion, nested skip containers,
// style-attribute overrides, viewBox offsets, comment/CDATA/DOCTYPE
// skipping) checking dims + FNV-1a gray checksums + probe points +
// drawn-shape counts. Guards cover non-SVG input, zero drawn shapes,
// bad canvas dims, truncated tags and null out-params. The bench
// fixture doubles as a decode benchmark.

#include "render/decoders/selftest_svg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/decoders/selftest_svg_fixtures.h"
#include "render/decoders/svg.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_svg_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_svg_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P24] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P24] FAIL %s", name); }
}

typedef struct {
    const char* name;
    const char* data;
    size_t len;
    int sw, sh;      /* decoded dims */
    unsigned cksum;  /* FNV-1a over gray rows */
    int drawn;       /* expected drawn-shape count */
    const SvgProbe* probes;
    int probes_len;
} Fixture;

static const Fixture kFixtures[] = {
    {"s_rects",        fx_s_rects,        FX_S_RECTS_LEN,
     FX_S_RECTS_W, FX_S_RECTS_H, FX_S_RECTS_CK, FX_S_RECTS_DRAWN,
     fx_sp_rects, FX_SP_RECTS_LEN},
    {"s_shapes",       fx_s_shapes,       FX_S_SHAPES_LEN,
     FX_S_SHAPES_W, FX_S_SHAPES_H, FX_S_SHAPES_CK, FX_S_SHAPES_DRAWN,
     fx_sp_shapes, FX_SP_SHAPES_LEN},
    {"s_path_basic",   fx_s_path_basic,   FX_S_PATH_BASIC_LEN,
     FX_S_PATH_BASIC_W, FX_S_PATH_BASIC_H, FX_S_PATH_BASIC_CK,
     FX_S_PATH_BASIC_DRAWN, fx_sp_path_basic, FX_SP_PATH_BASIC_LEN},
    {"s_path_curves",  fx_s_path_curves,  FX_S_PATH_CURVES_LEN,
     FX_S_PATH_CURVES_W, FX_S_PATH_CURVES_H, FX_S_PATH_CURVES_CK,
     FX_S_PATH_CURVES_DRAWN, fx_sp_path_curves, FX_SP_PATH_CURVES_LEN},
    {"s_arc",          fx_s_arc,          FX_S_ARC_LEN,
     FX_S_ARC_W, FX_S_ARC_H, FX_S_ARC_CK, FX_S_ARC_DRAWN,
     fx_sp_arc, FX_SP_ARC_LEN},
    {"s_use_defs",     fx_s_use_defs,     FX_S_USE_DEFS_LEN,
     FX_S_USE_DEFS_W, FX_S_USE_DEFS_H, FX_S_USE_DEFS_CK,
     FX_S_USE_DEFS_DRAWN, fx_sp_use_defs, FX_SP_USE_DEFS_LEN},
    {"s_nested_skip",  fx_s_nested_skip,  FX_S_NESTED_SKIP_LEN,
     FX_S_NESTED_SKIP_W, FX_S_NESTED_SKIP_H, FX_S_NESTED_SKIP_CK,
     FX_S_NESTED_SKIP_DRAWN, fx_sp_nested_skip, FX_SP_NESTED_SKIP_LEN},
    {"s_style_attr",   fx_s_style_attr,   FX_S_STYLE_ATTR_LEN,
     FX_S_STYLE_ATTR_W, FX_S_STYLE_ATTR_H, FX_S_STYLE_ATTR_CK,
     FX_S_STYLE_ATTR_DRAWN, fx_sp_style_attr, FX_SP_STYLE_ATTR_LEN},
    {"s_viewbox_offset", fx_s_viewbox_offset, FX_S_VIEWBOX_OFFSET_LEN,
     FX_S_VIEWBOX_OFFSET_W, FX_S_VIEWBOX_OFFSET_H, FX_S_VIEWBOX_OFFSET_CK,
     FX_S_VIEWBOX_OFFSET_DRAWN, fx_sp_viewbox_offset,
     FX_SP_VIEWBOX_OFFSET_LEN},
    {"s_comments",     fx_s_comments,     FX_S_COMMENTS_LEN,
     FX_S_COMMENTS_W, FX_S_COMMENTS_H, FX_S_COMMENTS_CK,
     FX_S_COMMENTS_DRAWN, fx_sp_comments, FX_SP_COMMENTS_LEN},
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
        int w = 0, h = 0, drawn = 0;
        char nm[64];
        if (svg_decode_gray(f->data, f->len, 360, 200,
                            &rows, &w, &h, &drawn) != 0 || rows == NULL) {
            snprintf(nm, sizeof(nm), "D.%s_decode", f->name);
            ck(nm, 0);
            continue;
        }
        snprintf(nm, sizeof(nm), "D.%s_dims", f->name);
        ck(nm, w == f->sw && h == f->sh);
        snprintf(nm, sizeof(nm), "D.%s_cksum", f->name);
        ck(nm, fnv_rows(rows, w, h) == f->cksum);
        snprintf(nm, sizeof(nm), "D.%s_drawn", f->name);
        ck(nm, drawn == f->drawn);

        snprintf(nm, sizeof(nm), "D.%s_probes", f->name);
        {
            int probesOk = 1;
            for (k = 0; k < f->probes_len && probesOk; k++) {
                const SvgProbe* p = &f->probes[k];
                if (rows[p->y][p->x] != (uint8_t)p->v) probesOk = 0;
            }
            ck(nm, probesOk);
        }
        svg_free_rows(rows, h);
    }
}

static void case_guards(void) {
    uint8_t** rows = NULL;
    int w = 0, h = 0, drawn = 0;
    int ok;

    ok = svg_decode_gray(fx_sg_no_svg, FX_SG_NO_SVG_LEN, 360, 200,
                         &rows, &w, &h, &drawn) != 0 && rows == NULL;
    ck("G.no_svg", ok);

    ok = svg_decode_gray(fx_sg_zero_drawn, FX_SG_ZERO_DRAWN_LEN, 360, 200,
                         &rows, &w, &h, &drawn) != 0 && rows == NULL;
    ck("G.zero_drawn", ok);

    ok = svg_decode_gray(fx_sg_bad_dims, FX_SG_BAD_DIMS_LEN, 360, 200,
                         &rows, &w, &h, &drawn) != 0 && rows == NULL;
    ck("G.bad_dims", ok);

    ok = svg_decode_gray(fx_sg_trunc_tag, FX_SG_TRUNC_TAG_LEN, 360, 200,
                         &rows, &w, &h, &drawn) != 0 && rows == NULL;
    ck("G.trunc_tag", ok);

    /* null outRows must be rejected without crashing */
    ok = svg_decode_gray(fx_s_bench, FX_S_BENCH_LEN, 360, 200,
                         NULL, &w, &h, &drawn) != 0;
    ck("G.null_args_reject", ok);

    svg_free_rows(NULL, 4); /* must be a no-op */
}

static void case_bench(void) {
    uint8_t** rows = NULL;
    int w = 0, h = 0, drawn = 0;
    double ms = 0.0;
    int it, iters = 100;
    int ok = svg_decode_gray(fx_s_bench, FX_S_BENCH_LEN, 360, 200,
                             &rows, &w, &h, &drawn) == 0 && rows != NULL;
    ck("B.smoke_192x192", ok && w == FX_S_BENCH_W &&
                            h == FX_S_BENCH_H &&
                            fnv_rows(rows, w, h) == FX_S_BENCH_CK &&
                            drawn == FX_S_BENCH_DRAWN);
    if (rows) svg_free_rows(rows, h);

#if defined(TARGET_PLAYDATE)
    if (s_pd != NULL) {
        float t0 = s_pd->system->getElapsedTime();
        for (it = 0; it < iters; it++) {
            rows = NULL;
            if (svg_decode_gray(fx_s_bench, FX_S_BENCH_LEN,
                                360, 200, &rows, &w, &h, &drawn) == 0)
                svg_free_rows(rows, h);
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
            if (svg_decode_gray(fx_s_bench, FX_S_BENCH_LEN,
                                360, 200, &rows, &w, &h, &drawn) == 0)
                svg_free_rows(rows, h);
            else
                break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
             (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
#endif
    PLUTO_LOG("[P24] bench svg %dx%d: %d iters in %.2f ms (%.3f ms/iter)",
              FX_S_BENCH_W, FX_S_BENCH_H, iters, ms, ms / iters);
    ck("B.bench_ran", it == iters && ms >= 0.0);
}

int selftest_svg_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_decode_all();
    case_guards();
    case_bench();
    PLUTO_LOG("[P24] svg selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed) *passed = s_pass;
    if (failed) *failed = s_fail;
    return s_fail;
}
