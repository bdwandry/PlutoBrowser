#include "render/selftest_style.h"
#include "render/style.h"
#include "core/logger.h"
#include <string.h>

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P13] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P13] FAIL %s", name); }
}

/* ── heading / body / inline mapping (environment-independent) ────────── */

static void case_mapping(void) {
    struct { int level, size, lh; } H[] = {
        {1, 24, 6}, {2, 18, 5}, {3, 16, 4},
        {4, 16, 4}, {5, 16, 4}, {6, 16, 4},
        {0, 16, 4}, {7, 16, 4}, {-3, 16, 4}
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++) {
        int sz = -1, lh = -1;
        style_get_heading_font(H[i].level, &sz, &lh);
        if (sz != H[i].size || lh != H[i].lh) ok = 0;
    }
    ck("H.heading_map", ok);

    int sz;
    style_get_body_font(0, 0, &sz);
    ck("H.body_plain_16", sz == 16);
    style_get_body_font(1, 0, &sz);
    ck("H.body_bold_16", sz == 16);
    style_get_body_font(0, 1, &sz);
    ck("H.body_code_15", sz == 15);

    /* inline precedence: code > small/sub/sup > bold/big > plain */
    style_get_inline_font(1, 1, 1, 0, 0, 0, &sz);
    ck("I.code_wins", sz == 15);
    style_get_inline_font(1, 0, 1, 0, 0, 0, &sz);
    ck("I.small_over_bold", sz == 14);
    style_get_inline_font(0, 0, 0, 1, 0, 0, &sz);
    ck("I.sub_14", sz == 14);
    style_get_inline_font(0, 0, 0, 0, 1, 0, &sz);
    ck("I.sup_14", sz == 14);
    style_get_inline_font(0, 0, 1, 1, 1, 1, &sz);
    ck("I.small_group_wins_over_big", sz == 14);
    style_get_inline_font(1, 0, 0, 0, 0, 0, &sz);
    ck("I.bold_16", sz == 16);
    style_get_inline_font(0, 0, 0, 0, 0, 1, &sz);
    ck("I.big_bold_slot_16", sz == 16);
    style_get_inline_font(0, 0, 0, 0, 0, 0, &sz);
    ck("I.plain_16", sz == 16);
}

/* ── widths ───────────────────────────────────────────────────────────── */

static void case_widths(void) {
    ck("W.empty_zero",
       style_get_text_width(NULL, "") == 0 &&
       style_get_text_width(NULL, NULL) == 0);

    /* host harness (no Playdate API): Lua's last resort of 8 px per char */
    int estimating = style_get_text_width(NULL, "Hello") == 40;
    if (estimating) {
        ck("W.estimate_8px_per_char",
           style_get_text_width(NULL, "Hello") == 40 &&
           style_get_text_width(NULL, "A") == 8 &&
           style_get_text_width(NULL, "two words") == 72);
    } else {
        /* device/sim: real fonts loaded -- sanity only */
        int wBody = style_get_text_width(NULL, "Hello World");
        int wCode =
            style_get_text_width(style_get_body_font(0, 1, NULL),
                                 "Hello World");
        int wSmall =
            style_get_text_width(style_get_inline_font(0, 0, 1, 0, 0, 0,
                                                       NULL),
                                 "Hello World");
        int wH1 =
            style_get_text_width(style_get_heading_font(1, NULL, NULL),
                                 "Hello World");
        ck("W.real_fonts_positive",
           wBody > 0 && wCode > 0 && wSmall > 0 && wH1 > 0);
        PLUTO_LOG("[P13] sample 'Hello World' widths: body=%d mono=%d "
                  "small=%d h1=%d", wBody, wCode, wSmall, wH1);
    }

    /* explicit font arg must not change the empty-text rule */
    ck("W.explicit_font_empty_zero",
       style_get_text_width(style_get_body_font(0, 0, NULL), "") == 0);
}

int selftest_style_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_mapping();
    case_widths();
    PLUTO_LOG("[P13] style selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
