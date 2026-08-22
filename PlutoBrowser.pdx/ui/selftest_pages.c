// selftest_pages.c — P30 verification harness (Error/Bookmarks/History).
//
// Pins the pure list-core primitives, the error-page input state machine,
// and both list pages' handle_input against the live Storage arrays.

#include "ui/selftest_pages.h"

#include <string.h>

#include "core/logger.h"
#include "core/storage.h"
#include "core/url.h"
#include "ui/bookmarks_page.h"
#include "ui/error_page.h"
#include "ui/history_page.h"
#include "ui/list_core.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P30] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P30] FAIL %s", name);
    }
}

/* ── list core ────────────────────────────────────────────────────────── */

static void case_list_core(void) {
    int sel;

    sel = 1;
    lr_nav(&sel, LR_BTN_UP, 5);
    ck("L.up_clamps_top", sel == 1);
    lr_nav(&sel, LR_BTN_DOWN, 5);
    ck("L.down_moves", sel == 2);
    sel = 5;
    lr_nav(&sel, LR_BTN_DOWN, 5);
    ck("L.down_clamps_bottom", sel == 5);
    sel = 3;
    lr_nav(&sel, LR_BTN_UP, 0); /* empty list: no-op */
    ck("L.empty_noop", sel == 3);

    double sy = 10.0;
    lr_scroll(&sy, 4.0);
    ck("L.scroll_plus", sy == 18.0);
    lr_scroll(&sy, -100.0);
    ck("L.scroll_clamps_zero", sy == 0.0);
    lr_scroll(&sy, 0.0);
    ck("L.scroll_zero_crank", sy == 0.0);

    ck("V.top_visible", lr_row_visible(24.0f, 34.0f) != 0);
    ck("V.bottom_visible", lr_row_visible(240.0f, 34.0f) != 0);
    ck("V.below_hidden", lr_row_visible(241.0f, 34.0f) == 0);
    ck("V.above_hidden", lr_row_visible(-20.0f, 34.0f) == 0);

    static char c[PLUTO_BM_URL_MAX];
    lr_clip_title(c, sizeof(c), "12345678901234567890123456789012345");
    ck("C.title_35_clips", strlen(c) == 34 &&
                           strcmp(c + 31, "...") == 0);
    lr_clip_title(c, sizeof(c), "1234567890123456789012345678901234");
    ck("C.title_34_kept",
       strlen(c) == 34 && memcmp(c + 31, "234", 3) == 0);
    lr_clip_url(c, sizeof(c),
                "https://a-very-long-domain.example.org/x/y/z?q=1234567");
    ck("C.url_47_clips", strlen(c) == 46 &&
                         strcmp(c + 43, "...") == 0);
    lr_clip_url(c, sizeof(c), "");
    ck("C.empty_ok", c[0] == '\0');
}

/* ── error page state machine ─────────────────────────────────────────── */

static void case_error_page(void) {
    ep_show(NULL, NULL);
    ck("E.default_msg",
       strcmp(ep_error_msg(), "Unable to load webpage") == 0);
    ck("E.sel_resets", ep_selected_index() == 1);

    ep_handle_input(EP_BTN_RIGHT);
    ep_handle_input(EP_BTN_RIGHT);
    ep_handle_input(EP_BTN_RIGHT); /* clamp at 3 */
    ck("E.right_clamps", ep_selected_index() == 3);

    EpAction a = ep_handle_input(EP_BTN_A);
    ck("E.a_home_at_3", a == EP_ACT_HOME);

    ep_handle_input(EP_BTN_LEFT);
    ep_handle_input(EP_BTN_LEFT);
    ep_handle_input(EP_BTN_LEFT); /* clamp at 1 */
    a = ep_handle_input(EP_BTN_A);
    ck("E.left_clamps_and_a_retry",
       ep_selected_index() == 1 && a == EP_ACT_RETRY);

    ep_handle_input(EP_BTN_DOWN); /* -> 2 */
    a = ep_handle_input(EP_BTN_A);
    ck("E.a_search_at_2", a == EP_ACT_SEARCH);

    ep_handle_input(EP_BTN_UP); /* up also moves left */
    ck("E.up_moves_left", ep_selected_index() == 1);

    /* long msg/url survive show() (clip is draw-time, like Lua) */
    const char* big =
        "This error message is intentionally far longer than forty-eight";
    ep_show(big, "https://example.invalid/long/target");
    ck("E.long_msg_stored", strcmp(ep_error_msg(), big) == 0);
    ep_show(NULL, NULL);
}

/* ── bookmarks page against live Storage ──────────────────────────────── */

static void case_bookmarks(void) {
    char url[PLUTO_URL_FULLPATH_MAX];
    LpAction act;

    DynArray* bms = storage_bookmarks();
    int count = bms ? (int)bms->count : 0;
    if (count < 1) {
        PLUTO_ERROR("[P30] no bookmarks in store; skipping bm cases");
        return;
    }

    bm_open();
    ck("B.open_resets", bm_selected_index() == 1 && bm_scroll_y() == 0.0);

    act = bm_handle_input(LR_BTN_DOWN, NULL, 0);
    ck("B.down_none", act == LP_ACT_NONE && bm_selected_index() == 2);
    act = bm_handle_input(LR_BTN_UP, NULL, 0);
    ck("B.up_back", bm_selected_index() == 1);

    PlutoSavedBookmark* b0 = da_get(bms, 0);
    url[0] = '\0';
    act = bm_handle_input(LR_BTN_A, url, sizeof(url));
    ck("B.a_opens", act == LP_ACT_OPEN);
    ck("B.a_url_matches", b0 != NULL && strcmp(url, b0->url) == 0);

    act = bm_handle_input(LR_BTN_B, NULL, 0);
    ck("B.b_closes", act == LP_ACT_CLOSE);

    /* crank scroll x2 with non-negative clamp */
    bm_open();
    bm_draw(-50.0); /* host-safe: draws nothing without pd; sim has pd */
    ck("B.scroll_clamped", bm_scroll_y() >= 0.0);
}

/* ── history page against live Storage ────────────────────────────────── */

static void case_history(void) {
    char url[PLUTO_URL_FULLPATH_MAX];
    LpAction act;

    DynArray* hist = storage_history();
    int count = hist ? (int)hist->count : 0;
    if (count < 1) {
        PLUTO_ERROR("[P30] no history in store; skipping hi cases");
        return;
    }

    hi_open();
    ck("H.open_resets", hi_selected_index() == 1 && hi_scroll_y() == 0.0);

    act = hi_handle_input(LR_BTN_DOWN, NULL, 0);
    ck("H.down_moves", act == LP_ACT_NONE && hi_selected_index() == 2);
    act = hi_handle_input(LR_BTN_UP, NULL, 0);
    ck("H.up_back", hi_selected_index() == 1);

    PlutoHistoryItem* h0 = da_get(hist, 0);
    url[0] = '\0';
    act = hi_handle_input(LR_BTN_A, url, sizeof(url));
    ck("H.a_opens", act == LP_ACT_OPEN);
    ck("H.a_url_matches", h0 != NULL && strcmp(url, h0->url) == 0);

    act = hi_handle_input(LR_BTN_B, NULL, 0);
    ck("H.b_closes", act == LP_ACT_CLOSE);
}

int selftest_pages_run(int* passed, int* failed) {
    s_pass = s_fail = 0;

    case_list_core();
    case_error_page();
    case_bookmarks();
    case_history();

    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P30] Pages selftest complete: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}
