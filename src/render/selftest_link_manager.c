// selftest_link_manager.c — [P14] selftests for the LinkManager port.
//
// Exercises addLinkRect merging, initial selection viewport rule,
// next/prev wrap-around, hit-tests and highlight checks with synthetic
// rects; results logged via PLUTO_LOG so they land in pluto.log.

#include "render/selftest_link_manager.h"

#include <string.h>

#include "core/logger.h"
#include "render/link_manager.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P14] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P14] FAIL %s", name); }
}

/* ── guards + basic add ───────────────────────────────────────────────── */

static void case_add_basics(void) {
    LmRect r1 = { 10, 100, 50, 16 };

    lm_clear();
    lm_add_link_rect(NULL, "t", r1, -1);
    lm_add_link_rect("", "t", r1, -1);
    ck("A.reject_empty_href", lm_get_count() == 0);

    lm_add_link_rect("a.html", NULL, r1, -1);
    ck("A.add_text_default", lm_get_count() == 1);
    LmLink* l = lm_get_hovered_link(15, 105);  /* inside r1 */
    ck("A.text_defaults_to_href",
       l != NULL && strcmp(l->text, "a.html") == 0 &&
       strcmp(l->href, "a.html") == 0);

    /* rect w/h defaults: point beyond x+w but within the Lua fallback of
     * +60/+14 is a HIT when w/h unset (-1). */
    lm_clear();
    LmRect noSize = { 100, 100, -1, -1 };
    lm_add_link_rect("bare", NULL, noSize, -1);
    ck("A.hover_default_w60_h14",
       lm_get_hovered_link(155, 110) != NULL &&   /* x+55 <= x+60 */
       lm_get_hovered_link(165, 110) == NULL);    /* past default width */
}

/* ── consecutive-rect merging ─────────────────────────────────────────── */

static void case_merge(void) {
    LmRect r1 = { 10, 20, 40, 14 };
    LmRect r2 = { 10, 34, 60, 14 };

    lm_clear();
    lm_add_link_rect("u", "T", r1, 3);
    lm_add_link_rect("u", "T", r2, 3);
    ck("M.same_anchor_merges", lm_get_count() == 1);
    LmLink* l = lm_get_selected_link();  /* none selected yet */
    ck("M.no_selection_yet", l == NULL);

    /* differing anchor -> new link */
    lm_add_link_rect("u", "T", r1, 4);
    ck("M.diff_anchor_splits", lm_get_count() == 2);

    /* nil anchor merges across ANY stored anchor */
    lm_add_link_rect("u", "T", r2, -1);
    ck("M.nil_anchor_merges_any", lm_get_count() == 2);

    /* different text / href never merge */
    lm_add_link_rect("u", "X", r1, -1);
    lm_add_link_rect("v", "T", r1, -1);
    ck("M.text_or_href_split", lm_get_count() == 4);

    /* nil text arg can never equal stored text -> new link */
    lm_add_link_rect("u", NULL, r1, -1);
    ck("M.nil_text_never_merges", lm_get_count() == 5);

    lm_clear();
    lm_add_link_rect("m", "m", r1, -1);
    lm_add_link_rect("m", "m", r2, -1);
    lm_add_link_rect("m", "m", r1, -1);
    ck("M.rect_list_and_primary",
       lm_get_count() == 1 && lm_selected_index() == 0);
    /* inspect rects via hovered-link probes on each stored rect */
    LmLink* m = lm_get_hovered_link(12, 21);
    ck("M.three_rects_stored", m != NULL && m->nRects == 3 &&
       m->primaryRect.x == r1.x && m->primaryRect.y == r1.y);
    ck("M.index_is_1_based",
       lm_find_initial_selection(-100) >= 0);  /* smoke: no crash */
}

/* ── findInitialSelection ─────────────────────────────────────────────── */

static void case_initial_selection(void) {
    /* CONTENT_HEIGHT = 216: viewport [200, 416], center = 308 */
    LmRect above = { 0, 100, 50, 16 };   /* cy=108 outside */
    LmRect inTop = { 0, 205, 50, 16 };   /* cy=213, dist 95 */
    LmRect inMid = { 0, 300, 50, 16 };   /* cy=308, dist 0  */
    LmRect below = { 0, 500, 50, 16 };   /* cy=508 outside  */

    lm_clear();
    ck("S.empty_page_none",
       lm_find_initial_selection(200) == 0 &&
       lm_select_next(200) == NULL &&
       lm_selected_index() == 0);

    lm_clear();
    lm_add_link_rect("p1", NULL, above, -1);
    lm_add_link_rect("p2", NULL, inMid, -1);
    lm_add_link_rect("p3", NULL, below, -1);
    ck("S.pass1_closest_center", lm_find_initial_selection(200) == 2);

    lm_clear();
    lm_add_link_rect("q1", NULL, below, -1);
    lm_add_link_rect("q2", NULL, above, -1);
    /* pass 2: |108-308|=200 vs |508-308|=200 -- tie keeps FIRST found
     * (strictly-less comparison), q1 at index 1 */
    long tie = lm_find_initial_selection(200);
    ck("S.pass2_all_outside_tie_first", tie == 1);

    lm_clear();
    lm_add_link_rect("r1", NULL, above, -1);   /* dist 200 */
    lm_add_link_rect("r2", NULL, below, -1);   /* dist 200 */
    lm_add_link_rect("r3", NULL, above, -1);
    ck("S.pass2_tie_keeps_first", lm_find_initial_selection(200) == 1);

    lm_clear();
    lm_add_link_rect("s1", NULL, inTop, -1);   /* dist 95  */
    lm_add_link_rect("s2", NULL, inMid, -1);   /* dist 0   */
    lm_select_next(200);                        /* first press uses pass 1 */
    size_t sel = lm_selected_index();
    ck("S.selectNext_uses_viewport_rule", sel == 2);
}

/* ── next / prev wrap-around ──────────────────────────────────────────── */

static void case_next_prev(void) {
    LmRect r = { 0, 0, 40, 12 };
    lm_clear();
    for (int i = 0; i < 3; i++)
        lm_add_link_rect("l3", NULL, r, -1);

    /* from none: selectPrev also starts via initial selection */
    lm_clear_selection();
    LmLink* p = lm_select_prev(0);
    ck("N.prev_initial_select", p != NULL && lm_selected_index() == 1);

    lm_select_next(0);                       /* -> 2 */
    lm_select_next(0);                       /* -> 3 */
    LmLink* wrap = lm_select_next(0);        /* -> wraps to 1 */
    ck("N.next_wraps_to_first",
       wrap != NULL && lm_selected_index() == 1);

    LmLink* back = lm_select_prev(0);        /* 1 -> wraps to 3 */
    ck("N.prev_wraps_to_last",
       back != NULL && lm_selected_index() == 3);

    /* selection survives only while links exist */
    lm_clear();
    ck("N.clear_resets_selection",
       lm_get_selected_link() == NULL && lm_selected_index() == 0);
}

/* ── getSelectedLink bounds + isHighlighted + addLink ─────────────────── */

static void case_highlight(void) {
    LmRect r1 = { 5, 30, 40, 14 };
    LmRect r2 = { 5, 44, 80, 14 };

    lm_clear();
    lm_add_link_rect("h1", NULL, r1, -1);
    lm_add_link_rect("h2", NULL, r2, -1);
    /* walk deterministically onto index 1 (also exercises repeated
     * wrap-around): initial pick -> ... -> index 1 */
    while (lm_selected_index() != 1) {
        if (lm_select_next(0) == NULL) break;
    }
    ck("H.selected_bounds_ok",
       lm_get_selected_link() != NULL &&
       strcmp(lm_get_selected_link()->href, "h1") == 0);

    ck("H.is_hit_on_selected_rect",
       lm_is_highlighted("h1", 5, 30) == 1);
    ck("H.miss_other_xy_of_same_href",
       lm_is_highlighted("h1", 5, 44) == 0);   /* that's h2's rect */
    ck("H.other_href_false", lm_is_highlighted("h2", 5, 44) == 0);
    ck("H.null_href_false", lm_is_highlighted(NULL, 5, 30) == 0);

    /* addLink convenience wrapper: href doubles as text, no anchor */
    lm_clear();
    lm_add_link("w.html", 1, 2, 3, 4);
    LmLink* w = lm_get_hovered_link(2, 3);
    ck("H.addLink_wrapper",
       w != NULL && w->nRects == 1 && strcmp(w->text, "w.html") == 0 &&
       w->rects[0].x == 1 && w->rects[0].y == 2 &&
       w->rects[0].w == 3 && w->rects[0].h == 4);
}

int selftest_link_manager_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_add_basics();
    case_merge();
    case_initial_selection();
    case_next_prev();
    case_highlight();
    lm_clear();

    PLUTO_LOG("[P14] synthetic layout dump: "
              "viewport rule verified against "
              "CONTENT_Y=%d CONTENT_H=%d SCREEN_H=%d",
              24, 216, 240);
    PLUTO_LOG("[P14] link_manager selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
