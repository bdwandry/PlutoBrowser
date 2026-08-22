// selftest_decoders.c — [P15] selftests for the dither + scale ports.
//
// Pure math is verified identically on host and device; the simulator-
// only section additionally renders a real LCDBitmap through
// Dither.toImage and cross-checks its 1-bit data against the pure
// Bayer logic, logging both counts to pluto.log.

#include "render/decoders/selftest_decoders.h"

#include <string.h>

#include "core/logger.h"
#include "render/decoders/dither.h"
#include "render/decoders/scale.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P15] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P15] FAIL %s", name); }
}

/* ── Dither.rgbToGray ─────────────────────────────────────────────────── */

static void case_gray(void) {
    ck("G.black_white",
       dither_rgb_to_gray(0, 0, 0) == 0 &&
       dither_rgb_to_gray(255, 255, 255) == 255);
    /* channels in isolation: 306/601/117 scaled by >>10 */
    ck("G.channels",
       dither_rgb_to_gray(255, 0, 0) == 76 &&
       dither_rgb_to_gray(0, 255, 0) == 149 &&
       dither_rgb_to_gray(0, 0, 255) == 29);
    ck("G.midpoint",
       dither_rgb_to_gray(128, 128, 128) == 128 &&   /* 1024*128 >> 10 */
       dither_rgb_to_gray(100, 150, 200) ==
           ((30600 + 90150 + 23400) >> 10));
    ck("G.monotonic", dither_rgb_to_gray(10, 10, 10) <
                      dither_rgb_to_gray(11, 11, 11));
}

/* ── Bayer matrix + threshold semantics ──────────────────────────────── */

static const int kBayer[4][4] = {
    {   0, 128,  32, 160 },
    { 192,  64, 224,  96 },
    {  48, 176,  16, 144 },
    { 240, 112, 208,  80 }
};

static void case_bayer(void) {
    int ok = 1;
    for (int y = 0; y < 4 && ok; y++)
        for (int x = 0; x < 4; x++) {
            int b = kBayer[y][x];
            if (!dither_is_black(b - 1, x, y) ||
                dither_is_black(b, x, y)) { ok = 0; break; }
        }
    ck("B.threshold_boundary_strict", ok);  /* gray==t -> white */

    /* QUIRK pinned: pure black (gray 0) stays white where threshold 0 */
    ck("B.gray0_corner_white",
       !dither_is_black(0, 0, 0) &&
       !dither_is_black(0, 4, 8) &&          /* pattern repeats mod 4 */
       dither_is_black(0, 1, 0));            /* t=128 -> black */

    int allWhite = 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (dither_is_black(255, x, y)) allWhite = 0;
    ck("B.white_all_white", allWhite);
}

/* ── Scale.boxSizes ───────────────────────────────────────────────────── */

static void case_box_sizes(void) {
    int bw, bh, tw, th;

    scale_box_sizes(100, 100, 50, 50, &bw, &bh, &tw, &th);
    ck("S.half", bw == 2 && bh == 2 && tw == 50 && th == 50);

    scale_box_sizes(101, 33, 50, 10, &bw, &bh, &tw, &th);
    ck("S.ceil_floors", bw == 3 && bh == 4 && tw == 33 && th == 8);

    scale_box_sizes(5, 5, 100, 100, &bw, &bh, &tw, &th);
    ck("S.upscale_clamps_to_1x1_boxes",
       bw == 1 && bh == 1 && tw == 5 && th == 5);

    scale_box_sizes(300, 200, 0, 0, &bw, &bh, &tw, &th);
    ck("S.max_zero_guard", bw == 300 && bh == 200 && tw == 1 && th == 1);

    scale_box_sizes(0, 0, 50, 50, &bw, &bh, &tw, &th);
    ck("S.src_zero", bw == 1 && bh == 1 && tw == 1 && th == 1);
}

/* ── Scale accumulator ────────────────────────────────────────────────── */

static void fill_row(unsigned char* row, int w, unsigned char v) {
    memset(row, v, (size_t)w);
}

static void case_accum(void) {
    unsigned char row[32];
    /* uniform image halves cleanly */
    ScaleAccum* acc = scale_accum_new(4, 4, 2, 2);
    fill_row(row, 4, 100);
    for (int i = 0; i < 4; i++) scale_accum_add_row(acc, row);
    int tw, th;
    int n = scale_accum_finish(acc, &tw, &th);
    int ok = n == 2 && tw == 2 && th == 2;
    for (int ry = 0; ok && ry < n; ry++)
        for (int cx = 0; cx < tw; cx++)
            if (acc->out[ry][cx] != 100) { ok = 0; }
    ck("A.uniform_half", ok);
    scale_accum_free(acc);

    /* boxW==1 fast path matches general path expectations */
    acc = scale_accum_new(6, 2, 6, 3);
    fill_row(row, 6, 40);
    scale_accum_add_row(acc, row);
    scale_accum_add_row(acc, row);
    n = scale_accum_finish(acc, &tw, &th);
    ck("A.fast_path_identity",
       n == 2 && tw == 6 && th == 2 && acc->out[0][5] == 40 &&
       acc->out[1][0] == 40);
    scale_accum_free(acc);

    /* rounding: floor(sum/div + 0.5) -- half rounds up */
    acc = scale_accum_new(3, 1, 1, 1);   /* boxW=3, single row */
    row[0] = 1; row[1] = 1; row[2] = 2;  /* 4/3 -> floor(1.83)=1 */
    scale_accum_add_row(acc, row);
    n = scale_accum_finish(acc, &tw, &th);
    ck("A.round_down_case", n == 1 && acc->out[0][0] == 1);
    scale_accum_free(acc);

    acc = scale_accum_new(3, 1, 1, 1);
    row[0] = 2; row[1] = 2; row[2] = 2;  /* 6/3=2 exact */
    scale_accum_add_row(acc, row);
    scale_accum_finish(acc, &tw, &th);
    ck("A.round_exact", acc->out[0][0] == 2);
    scale_accum_free(acc);

    acc = scale_accum_new(2, 1, 1, 1);   /* boxW=2 */
    row[0] = 1; row[1] = 2;              /* 3/2 -> floor(2.0)=2 half-up */
    scale_accum_add_row(acc, row);
    scale_accum_finish(acc, &tw, &th);
    ck("A.round_half_up", acc->out[0][0] == 2);
    scale_accum_free(acc);

    /* QUIRK pinned: trailing partial block adds an EXTRA row --
     * srcH=3, boxH=2 -> targetH=1 but count=2, divisor boxW*filled */
    acc = scale_accum_new(4, 3, 2, 2);
    fill_row(row, 4, 10); scale_accum_add_row(acc, row);
    fill_row(row, 4, 20); scale_accum_add_row(acc, row);   /* full: 15 */
    fill_row(row, 4, 30); scale_accum_add_row(acc, row);   /* part: 30 */
    n = scale_accum_finish(acc, &tw, &th);
    ck("A.partial_block_extra_row_quirk",
       n == 2 && tw == 2 && th == 1 &&
       acc->out[0][0] == 15 && acc->out[0][1] == 15 &&
       acc->out[1][0] == 30 && acc->out[1][1] == 30);
    scale_accum_free(acc);

    /* NULL row ignored */
    acc = scale_accum_new(4, 1, 2, 2);
    scale_accum_add_row(acc, NULL);
    n = scale_accum_finish(acc, &tw, &th);
    ck("A.null_row_ignored", n == 0);
    scale_accum_free(acc);
}

/* ── simulator/device-only: real bitmap render cross-check ───────────── */

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)

typedef struct {
    int w, h;
} GradCtx;

static int grad_pixel(void* ud, int x, int y) {
    GradCtx* g = (GradCtx*)ud;
    return (x * 7 + y * 11 + x * y) & 0xFF;
}

#include "pd_api.h"

static void case_real_bitmap(PlaydateAPI* pd) {
    GradCtx g = { 64, 48 };
    struct LCDBitmap* img =
        dither_to_image(pd, grad_pixel, &g, 400, 500);  /* clamps */
    if (img == NULL) {
        ck("R.bitmap_rendered", 0);
        return;
    }
    int bw = 0, bh = 0, rowbytes = 0;
    uint8_t* data = NULL;
    uint8_t* mask = NULL;
    pd->graphics->getBitmapData(img, &bw, &bh, &rowbytes, &mask, &data);
    int stride = rowbytes;

    /* expected black count from the pure Bayer logic over clamped size */
    int expectBlack = 0;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            if (dither_is_black(grad_pixel(&g, x, y), x, y))
                expectBlack++;

    int actualBlack = 0;   /* bit cleared = black, MSB first per byte */
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            if (!((data[y * stride + x / 8] >> (7 - (x & 7))) & 1))
                actualBlack++;

    PLUTO_LOG("[P15] dump toImage %dx%d (from 400x500) "
              "expected_black=%d actual_black=%d",
              bw, bh, expectBlack, actualBlack);
    ck("R.clamped_380x240", bw == 380 && bh == 240);
    ck("R.bitmap_pixels_match_bayer", actualBlack == expectBlack);
    pd->graphics->freeBitmap(img);

    /* degenerate sizes rejected */
    ck("R.degenerate_rejected",
       dither_to_image(pd, grad_pixel, &g, 0, 10) == NULL &&
       dither_to_image(pd, grad_pixel, &g, 10, -1) == NULL);
}
#endif

int selftest_decoders_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_gray();
    case_bayer();
    case_box_sizes();
    case_accum();
    PLUTO_LOG("[P15] decoders selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
/* called separately by main.c once pd is installed: renders a real
 * LCDBitmap through Dither.toImage and cross-checks its pixels */
void selftest_decoders_device(struct PlaydateAPI* pd) {
    s_pass = 0;
    s_fail = 0;
    case_real_bitmap((PlaydateAPI*)pd);
    PLUTO_LOG("[P15] decoders device checks done: %d passed, %d failed",
              s_pass, s_fail);
    if (s_fail > 0) PLUTO_ERROR("P15 DEVICE FAILURES: %d", s_fail);
}
#endif
