// selftest_settings.c — P31 verification harness (SettingsPage).
//
// Pins the staged-copy semantics: cycle/toggle math per row, nav clamps,
// immediate Clear Cookies action, Save (persist + onChange) vs Cancel
// (discard), and reload-from-disk persistence.

#include "ui/selftest_settings.h"

#include <string.h>

#include "core/constants.h"
#include "core/cookie_jar.h"
#include "core/logger.h"
#include "core/storage.h"
#include "ui/settings_page.h"

static int s_pass, s_fail;
static int s_changeFired;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P31] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P31] FAIL %s", name);
    }
}

static void on_change(void) { s_changeFired++; }

/* ── open + snapshot ──────────────────────────────────────────────────── */

static void case_open(void) {
    PlutoSettings* st = storage_settings();

    sp_open("home");
    ck("O.opens", sp_is_open() == 1 && sp_selected_index() == 1);
    ck("O.prev_state", strcmp(sp_previous_state(), "home") == 0);

    /* staged mirrors stored values */
    char expect[64];
    int idx = st->searchEngine;
    if (idx < 1 || idx > PLUTO_SEARCH_ENGINE_COUNT) idx = 1;
    snprintf(expect, sizeof(expect), "%s",
             PLUTO_SEARCH_ENGINES[idx - 1].name);
    ck("O.engine_snapshot",
       strcmp(sp_staged_engine_name(), expect) == 0);
}

/* ── row cycling / toggling math ──────────────────────────────────────── */

static void nav_to_row(int row) {
    while (sp_selected_index() < row) {
        sp_handle_input(SP_BTN_DOWN);
    }
    while (sp_selected_index() > row) {
        sp_handle_input(SP_BTN_UP);
    }
}

static void case_cycles(void) {
    sp_open(NULL);

    /* engine (row 1): right advances with wrap; left wraps backwards */
    sp_handle_input(SP_BTN_RIGHT); /* 1 -> 2 */
    {
        const char* want = PLUTO_SEARCH_ENGINES[1].name;
        ck("C.engine_right",
           strcmp(sp_staged_engine_name(), want) == 0);
    }
    for (int i = 0; i < 3; i++) sp_handle_input(SP_BTN_RIGHT);
    /* from 2: three rights land on 1 (2->3->4->1) */
    ck("C.engine_wrap_right",
       strcmp(sp_staged_engine_name(), PLUTO_SEARCH_ENGINES[0].name) == 0);
    sp_handle_input(SP_BTN_LEFT); /* 1 -> 4 */
    ck("C.engine_wrap_left",
       strcmp(sp_staged_engine_name(),
              PLUTO_SEARCH_ENGINES[PLUTO_SEARCH_ENGINE_COUNT - 1].name)
           == 0);

    /* mode (row 2): toggles both directions, round-trips */
    nav_to_row(2);
    const char* before = sp_staged_mode_label();
    sp_handle_input(SP_BTN_LEFT);
    const char* mid = sp_staged_mode_label();
    sp_handle_input(SP_BTN_RIGHT);
    ck("C.mode_toggles",
       strcmp(before, mid) != 0 &&
           strcmp(sp_staged_mode_label(), before) == 0);

    /* invert crank (row 3): toggle flips On/Off */
    nav_to_row(3);
    const char* invBefore = sp_staged_invert_label();
    sp_handle_input(SP_BTN_LEFT);
    const char* invMid = sp_staged_invert_label();
    sp_handle_input(SP_BTN_RIGHT);
    ck("C.invert_toggles",
       strcmp(invBefore, invMid) != 0 &&
           strcmp(sp_staged_invert_label(), invBefore) == 0);

    /* image mode (row 4): cycles in NAMES order, wraps both ways */
    nav_to_row(4);
    const char* img0 = sp_staged_image_label();
    sp_handle_input(SP_BTN_RIGHT);
    const char* img1 = sp_staged_image_label();
    sp_handle_input(SP_BTN_LEFT);
    ck("C.image_cycle_roundtrip",
       strcmp(img0, img1) != 0 &&
           strcmp(sp_staged_image_label(), img0) == 0);

    /* five rights/lefts from any start -> back to start (5 modes) */
    const char* imgStart = sp_staged_image_label();
    for (int i = 0; i < 5; i++) sp_handle_input(SP_BTN_RIGHT);
    ck("C.image_wrap_right",
       strcmp(sp_staged_image_label(), imgStart) == 0);
    for (int i = 0; i < 5; i++) sp_handle_input(SP_BTN_LEFT);
    ck("C.image_wrap_left",
       strcmp(sp_staged_image_label(), imgStart) == 0);

    /* protocol (row 6): toggles HTTP <-> TCP, round-trips */
    nav_to_row(6);
    const char* protoBefore = sp_staged_protocol_label();
    sp_handle_input(SP_BTN_LEFT);
    const char* protoMid = sp_staged_protocol_label();
    sp_handle_input(SP_BTN_RIGHT);
    ck("C.protocol_toggles",
       strcmp(protoBefore, protoMid) != 0 &&
           strcmp(sp_staged_protocol_label(), protoBefore) == 0);
}

/* ── nav clamps ───────────────────────────────────────────────────────── */

static void case_nav(void) {
    sp_open(NULL);
    sp_handle_input(SP_BTN_UP);
    ck("N.up_clamps_top", sp_selected_index() == 1);
    for (int i = 0; i < 9; i++) sp_handle_input(SP_BTN_DOWN);
    ck("N.down_clamps_bottom", sp_selected_index() == SP_OPTION_COUNT);
}

/* ── Clear Cookies immediate action ───────────────────────────────────── */

static void case_clear_cookies(void) {
    cj_store("cookies.example", "sid=abc123; Path=/");
    size_t seeded = cj_count();
    if (seeded == 0) {
        PLUTO_ERROR("[P31] cookie seed failed; skipping clear test");
        return;
    }

    sp_open(NULL);
    for (int i = 0; i < 4; i++) sp_handle_input(SP_BTN_DOWN); /* row 5 */
    s_changeFired = 0;
    SpAction act = sp_handle_input(SP_BTN_A);
    ck("K.cookies_cleared", cj_count() == 0);
    ck("K.stays_open_no_save",
       act == SP_ACT_NONE && sp_is_open() == 1 && s_changeFired == 0);
    sp_close();
}

/* ── Save semantics + persistence ─────────────────────────────────────── */

static void case_save(void) {
    sp_open(NULL);
    sp_set_on_change(on_change);

    /* change engine to slot 2 and save */
    while (strcmp(sp_staged_engine_name(),
                  PLUTO_SEARCH_ENGINES[1].name) != 0) {
        sp_handle_input(SP_BTN_RIGHT);
    }
    s_changeFired = 0;
    SpAction act = sp_handle_input(SP_BTN_A);

    PlutoSettings* st = storage_settings();
    ck("S.saved_action", act == SP_ACT_SAVED);
    ck("S.applied_to_storage", st != NULL && st->searchEngine == 2);
    ck("S.onchange_fired_once", s_changeFired == 1);
    ck("S.closed_after_save", sp_is_open() == 0);

    /* persistence across reload */
    ck("S.reload_keeps", storage_load() == 1 &&
                             st->searchEngine == 2);
    st = storage_settings();
    ck("S.reload_pointer_same_values", st != NULL && st->searchEngine == 2);

    /* protocol persists to storage + survives reload */
    sp_open(NULL);
    nav_to_row(6);
    sp_handle_input(SP_BTN_LEFT); /* HTTP -> TCP */
    s_changeFired = 0;
    act = sp_handle_input(SP_BTN_A);
    st = storage_settings();
    ck("S.protocol_saved", act == SP_ACT_SAVED && st != NULL &&
                               st->protocol == PLUTO_PROTOCOL_TCP);
    ck("S.protocol_reload", storage_load() == 1 &&
                                storage_settings()->protocol ==
                                    PLUTO_PROTOCOL_TCP);
    /* restore default protocol for other tests/sessions */
    sp_open(NULL);
    nav_to_row(6);
    sp_handle_input(SP_BTN_RIGHT); /* TCP -> HTTP */
    sp_handle_input(SP_BTN_A);
    ck("S.protocol_restored", storage_settings()->protocol ==
                                  PLUTO_PROTOCOL_HTTP);

    /* restore default engine for other tests/sessions */
    sp_open(NULL);
    while (st != NULL &&
           strcmp(sp_staged_engine_name(), PLUTO_SEARCH_ENGINES[0].name)
               != 0) {
        sp_handle_input(SP_BTN_RIGHT);
    }
    s_changeFired = 0;
    sp_handle_input(SP_BTN_A);
    ck("S.restore_default", st->searchEngine == 1);
}

/* ── Cancel semantics ─────────────────────────────────────────────────── */

static void case_cancel(void) {
    PlutoSettings* st = storage_settings();
    int engineBefore = st ? st->searchEngine : 1;

    sp_open(NULL);
    sp_handle_input(SP_BTN_RIGHT); /* stage a different engine */
    s_changeFired = 0;
    SpAction act = sp_handle_input(SP_BTN_B);

    ck("X.closed_action", act == SP_ACT_CLOSED && sp_is_open() == 0);
    ck("X.storage_untouched",
       st != NULL && st->searchEngine == engineBefore);
    ck("X.no_onchange", s_changeFired == 0);

    /* reopen snapshots original value (staged copy discarded) */
    sp_open(NULL);
    ck("X.restage_original",
       strcmp(sp_staged_engine_name(),
              PLUTO_SEARCH_ENGINES[engineBefore - 1].name) == 0);
    sp_close();
}

int selftest_settings_run(int* passed, int* failed) {
    s_pass = s_fail = 0;
    s_changeFired = 0;

    case_open();
    case_cycles();
    case_nav();
    case_clear_cookies();
    case_save();
    case_cancel();

    sp_set_on_change(NULL);
    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P31] SettingsPage selftest complete: %d passed, "
              "%d failed", s_pass, s_fail);
    return s_fail;
}
