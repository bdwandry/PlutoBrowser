// selftest_home_page.c — P28 verification harness (HomePage).
//
// Pins the grid-nav rules, auto-scroll bands, marquee oscillation math,
// smooth-scroll lerp and the A-action semantics against Storage.

#include "ui/selftest_home_page.h"

#include <math.h>
#include <string.h>

#include "core/logger.h"
#include "core/storage.h"
#include "ui/home_page.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P28] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P28] FAIL %s", name);
    }
}

/* ── grid navigation rules ────────────────────────────────────────────── */

static void case_grid_nav(void) {
    int sel, count;

    /* settings -> first card only when cards exist */
    sel = 0; count = 3;
    hp_grid_nav(HP_BTN_DOWN, &sel, count);
    ck("N.down_settings_to_first", sel == 1);
    sel = 0; count = 0;
    hp_grid_nav(HP_BTN_DOWN, &sel, count);
    ck("N.down_settings_no_cards", sel == 0);

    /* down: full row below -> +2; else +1; else stay */
    sel = 1; count = 4;
    hp_grid_nav(HP_BTN_DOWN, &sel, count);
    ck("N.down_plus2", sel == 3);
    sel = 2; count = 3;
    hp_grid_nav(HP_BTN_DOWN, &sel, count);
    ck("N.down_plus1_odd_row", sel == 3);
    sel = 2; count = 2;
    hp_grid_nav(HP_BTN_DOWN, &sel, count);
    ck("N.down_at_last_stays", sel == 2);

    /* up: first row -> settings; else -2 */
    sel = 1; count = 3;
    hp_grid_nav(HP_BTN_UP, &sel, count);
    ck("N.up_first_row_settings", sel == 0);
    sel = 2; count = 5;
    hp_grid_nav(HP_BTN_UP, &sel, count);
    ck("N.up_second_row_settings", sel == 0);
    sel = 3; count = 5;
    hp_grid_nav(HP_BTN_UP, &sel, count);
    ck("N.up_minus2", sel == 1);
    sel = 0; count = 4;
    hp_grid_nav(HP_BTN_UP, &sel, count);
    ck("N.up_at_top_stays", sel == 0);

    /* right: odd index moves, even stays */
    sel = 1; count = 2;
    hp_grid_nav(HP_BTN_RIGHT, &sel, count);
    ck("N.right_odd_moves", sel == 2);
    sel = 2; count = 4;
    hp_grid_nav(HP_BTN_RIGHT, &sel, count);
    ck("N.right_even_stays", sel == 2);
    sel = 3; count = 3;
    hp_grid_nav(HP_BTN_RIGHT, &sel, count);
    ck("N.right_last_stays", sel == 3);

    /* left: even index moves, odd stays */
    sel = 2; count = 3;
    hp_grid_nav(HP_BTN_LEFT, &sel, count);
    ck("N.left_even_moves", sel == 1);
    sel = 1; count = 3;
    hp_grid_nav(HP_BTN_LEFT, &sel, count);
    ck("N.left_odd_stays", sel == 1);
}

/* ── auto-scroll bands ────────────────────────────────────────────────── */

static void case_autoscroll(void) {
    /* settings selection always targets the top */
    ck("S.settings_top",
       hp_autoscroll_target(0, 5, 123.0) == 0.0);

    /* empty page keeps target untouched */
    ck("S.empty_untouched",
       hp_autoscroll_target(2, 0, 77.0) == 77.0);

    /* row 0 (absY=184) inside the visible band: unchanged */
    ck("S.row0_in_band",
       hp_autoscroll_target(1, 8, 100.0) == 100.0);

    /* deep row pushes target so absY sits SCREEN_HEIGHT-40 from top */
    /* sel=7 -> row 3 -> absY=184+162=346; dispY=346>200 ->
       target=346-200=146 */
    ck("S.deep_row_pulls_up",
       fabs(hp_autoscroll_target(7, 8, 0.0) - 146.0) < 1e-9);

    /* scrolled too far: clamp to max(0, absY - CONTENT_Y - 10) */
    /* target=400 -> dispY=-54<34 -> max(0,346-34)=312 */
    ck("S.too_far_clamps",
       fabs(hp_autoscroll_target(7, 8, 400.0) - 312.0) < 1e-9);
}

/* ── marquee oscillation ──────────────────────────────────────────────── */

static void case_marquee(void) {
    const double tw = 200.0, maxW = 100.0; /* range=100, travel=2s */
    double off;

    ck("M.fits_no_scroll", hp_marquee_offset(9.9, 80.0, 100.0) == 0.0);

    off = hp_marquee_offset(0.5, tw, maxW); /* dwell hold start */
    ck("M.dwell_start", off == 0.0);

    off = hp_marquee_offset(1.5, tw, maxW); /* (1.5-1)*50 */
    ck("M.travel_out", fabs(off - 25.0) < 1e-9);

    off = hp_marquee_offset(3.5, tw, maxW); /* second dwell at range */
    ck("M.dwell_end", off == 100.0);

    off = hp_marquee_offset(4.5, tw, maxW); /* return leg: 100-25 */
    ck("M.travel_back", fabs(off - 75.0) < 1e-9);

    off = hp_marquee_offset(6.5, tw, maxW); /* cycle wraps to dwell */
    ck("M.cycle_wrap", off == 0.0);
}

/* ── smooth scroll lerp ───────────────────────────────────────────────── */

static void case_smooth_scroll(void) {
    double v = hp_smooth_scroll(0.0, 100.0);
    ck("L.lerp_step30", fabs(v - 30.0) < 1e-9);

    ck("L.snap_under_half", hp_smooth_scroll(99.8, 100.0) == 100.0);

    ck("L.clamp_nonnegative", hp_smooth_scroll(10.0, -50.0) == 0.0);
}

/* ── input actions against Storage ────────────────────────────────────── */

static int s_settingsFired = 0;
static void settings_cb(void) { s_settingsFired = 1; }

static void case_actions(void) {
    char url[PLUTO_BM_URL_MAX];
    HpAction act;

    hp_reset();
    s_settingsFired = 0;

    /* The data disk ships with the immutable default dial list; work
     * against whatever the store actually holds. */
    int count = hp_bookmark_count();
    ck("A.have_bookmarks", count >= 2);
    PlutoSavedBookmark* b0 =
        (PlutoSavedBookmark*)da_get(storage_bookmarks(), 0);
    ck("A.first_entry_readable", b0 != NULL);
    if (b0 == NULL) return;

    hp_set_settings_callback(settings_cb);

    act = hp_handle_input(HP_BTN_A, url, sizeof(url));
    ck("A.settings_action", act == HP_ACT_SETTINGS);
    ck("A.settings_callback_fired", s_settingsFired == 1);

    hp_handle_input(HP_BTN_DOWN, NULL, 0); /* -> card 1 */
    ck("A.nav_after_down", hp_selected_index() == 1);

    act = hp_handle_input(HP_BTN_A, url, sizeof(url));
    ck("A.card_open_action", act == HP_ACT_OPEN_URL);
    ck("A.card_url_matches_store",
       strcmp(url, b0->url) == 0);

    hp_reset();
    hp_set_settings_callback(NULL);
    s_settingsFired = 0;
    act = hp_handle_input(HP_BTN_A, url, sizeof(url));
    ck("A.no_callback_ok",
       act == HP_ACT_SETTINGS && s_settingsFired == 0);
}

int selftest_home_page_run(int* passed, int* failed) {
    s_pass = s_fail = 0;

    case_grid_nav();
    case_autoscroll();
    case_marquee();
    case_smooth_scroll();
    case_actions();

    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P28] HomePage selftest complete: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}
