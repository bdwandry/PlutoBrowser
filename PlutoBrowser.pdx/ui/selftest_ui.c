// selftest_ui.c — P27 verification harness (Chrome + Hud pure helpers).
//
// The drawing itself is exercised visually in the simulator; here we pin
// the host-display rules, comet/progress math, scrollbar geometry and the
// label-clip boundaries to exact values.

#include "ui/selftest_ui.h"

#include <string.h>

#include "core/logger.h"
#include "ui/chrome.h"
#include "ui/hud.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P27] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P27] FAIL %s", name);
    }
}

/* ── Chrome host display rules ────────────────────────────────────────── */

static void case_display_host(void) {
    char out[PLUTO_URL_HOST_MAX + 16];
    PlutoUrl u;
    memset(&u, 0, sizeof(u));

    ch_display_host(NULL, out, sizeof(out));
    ck("H.null_default", strcmp(out, "CometBrowser") == 0);

    ch_display_host(&u, out, sizeof(out));
    ck("H.empty_default", strcmp(out, "CometBrowser") == 0);

    strcpy(u.scheme, "about");
    strcpy(u.host, "home");
    ch_display_host(&u, out, sizeof(out));
    ck("H.about_home", strcmp(out, "about:home") == 0);

    strcpy(u.host, "");
    ch_display_host(&u, out, sizeof(out));
    ck("H.about_nohost", strcmp(out, "about:home") == 0);

    strcpy(u.host, "docs");
    ch_display_host(&u, out, sizeof(out));
    ck("H.about_docs", strcmp(out, "about:docs") == 0);

    strcpy(u.scheme, "https");
    strcpy(u.host, "");
    ch_display_host(&u, out, sizeof(out));
    ck("H.web_emptyhost_default", strcmp(out, "CometBrowser") == 0);

    strcpy(u.host, "example.com");
    ch_display_host(&u, out, sizeof(out));
    ck("H.simple", strcmp(out, "example.com") == 0);

    /* exactly 28 chars: unchanged */
    memset(u.host, 'a', 28);
    u.host[28] = '\0';
    ch_display_host(&u, out, sizeof(out));
    ck("H.len28_kept",
       strlen(out) == 28 && out[0] == 'a' && out[27] == 'a');

    /* 29 chars -> first 25 + "..." */
    memset(u.host, 'b', 29);
    u.host[29] = '\0';
    ch_display_host(&u, out, sizeof(out));
    ck("H.len29_clipped",
       strlen(out) == 28 &&
       strncmp(out, "bbbbbbbbbbbbbbbbbbbbbbbbb...", 28) == 0);
}

/* ── loading animation + progress math ────────────────────────────────── */

static void case_anim_progress(void) {
    int ok = 1;
    for (int f = 0; f <= 3; f++) ok = ok && (ch_comet_group(f) == 0);
    for (int f = 4; f <= 7; f++) ok = ok && (ch_comet_group(f) == 1);
    for (int f = 8; f <= 11; f++) ok = ok && (ch_comet_group(f) == 2);
    ok = ok && (ch_comet_group(12) == 0) && (ch_comet_group(23) == 2) &&
         (ch_comet_group(24) == 0); /* 24 % 12 == 0 */
    ck("A.comet_groups_12phase", ok);

    ck("P.known_half", ch_progress_ratio(100, 200, 7) == 0.5);
    ck("P.known_clamp_over", ch_progress_ratio(500, 200, 0) == 1.0);
    ck("P.indet_frame0", ch_progress_ratio(0, 0, 0) == 0.0);
    /* ((33*3)%100)/100 = 99/100 ; ((34*3)%100)=2 */
    ck("P.indet_frame33", ch_progress_ratio(0, 0, 33) == 0.99);
    ck("P.indet_wrap34", ch_progress_ratio(0, 0, 34) == 0.02);
}

/* ── Hud scrollbar geometry ───────────────────────────────────────────── */

static void case_scrollbar(void) {
    int y = -99, h = -99;

    hd_scrollbar_thumb(216.0, 10, &y, &h);
    ck("S.no_bar_at_content_h", y == -1 && h == -1);
    hd_scrollbar_thumb(100.0, 0, &y, &h);
    ck("S.no_bar_short_page", y == -1 && h == -1);

    hd_scrollbar_thumb(432.0, 108, &y, &h);
    /* trackH=212; thumbH=floor(212*216/432)=106; ratio=.5 ->
     * thumbY=26+floor(106*.5)=79 */
    ck("S.double_height_mid", h == 106 && y == 79);

    hd_scrollbar_thumb(432.0, -50, &y, &h);
    ck("S.clamp_negative", y == 26 && h == 106);

    hd_scrollbar_thumb(432.0, 9999, &y, &h);
    ck("S.clamp_over", y == 26 + (212 - 106) && h == 106);

    /* huge page -> minimum thumb height 12 */
    hd_scrollbar_thumb(100000.0, 49892, &y, &h);
    /* maxScroll=99784; ratio≈0.5 -> 26+floor((212-12)*ratio) */
    ck("S.min_thumb_h", h == 12 && y >= 26 && y <= 26 + 200);
}

/* ── label clipping ───────────────────────────────────────────────────── */
/* (checked inline below: link 56/57 boundaries with "-> " prefix,
 * hover 50/51 boundaries, NULL href and empty url cases) */

int selftest_ui_run(int* passed, int* failed) {
    s_pass = s_fail = 0;

    case_display_host();
    case_anim_progress();
    case_scrollbar();

    {
        char out[PLUTO_URL_INPUT_MAX];
        char hover[128];

        hd_format_link("https://x.io/a", out, sizeof(out));
        ck("C.link_prefix", strcmp(out, "-> https://x.io/a") == 0);
        hd_format_link(NULL, out, sizeof(out));
        ck("C.link_null_href", strcmp(out, "-> ") == 0);

        memset(hover, 'a', 53);
        hover[53] = '\0';
        hd_format_link(hover, out, sizeof(out));
        ck("C.link_len56_kept", strlen(out) == 56);
        hover[53] = 'a';
        hover[54] = '\0';
        hd_format_link(hover, out, sizeof(out));
        ck("C.link_len57_clipped",
           strlen(out) == 56 && strncmp(out + 53, "...", 3) == 0);

        memset(hover, 'u', 50);
        hover[50] = '\0';
        hd_format_hover(hover, out, sizeof(out));
        ck("C.hover_len50_kept", strlen(out) == 50);
        hover[50] = 'u';
        hover[51] = '\0';
        hd_format_hover(hover, out, sizeof(out));
        ck("C.hover_len51_clipped",
           strlen(out) == 50 && strncmp(out + 47, "...", 3) == 0);

        hd_format_hover("", out, sizeof(out));
        ck("C.hover_empty", strcmp(out, "") == 0);
    }

    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P27] UI selftest complete: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}
