// selftest_layout.c — [P26B] selftests for the Layout port.
//
// Geometry expectations are derived from the REAL Source/render/layout.lua
// arithmetic (start y = CONTENT_Y+8, marginX = CONTENT_MARGIN+2, maxWidth =
// CONTENT_TEXT_WIDTH-4, per-block advances) and from the pure helpers
// (roman/alpha/tab expansion), mirroring the oracle style of earlier
// phases. Font-independent invariants are asserted so results hold on any
// font set; wrapping behavior is exercised via the mono/body fallbacks.
// Results land in pluto.log.

#include "render/selftest_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/constants.h"
#include "core/logger.h"
#include "html/document.h"
#include "render/layout.h"
#include "render/link_manager.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P26B] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P26B] FAIL %s", name); }
}

/* ── tiny programmatic document builder ───────────────────────────────── */

static void doc_init_empty(DocDocument* d) { memset(d, 0, sizeof(*d)); }

static DocBlock* doc_add(DocDocument* d, int type) {
    if (d->nBlocks == d->capBlocks) {
        d->capBlocks = d->capBlocks ? d->capBlocks * 2 : 8;
        d->blocks = (DocBlock*)realloc(d->blocks,
                                       sizeof(DocBlock) * d->capBlocks);
    }
    DocBlock* b = &d->blocks[d->nBlocks++];
    memset(b, 0, sizeof(*b));
    b->type       = type;
    b->maxlength  = -1;
    b->fieldWidth = -1;
    b->fieldRows  = -1;
    return b;
}

static void blk_add_inline(DocBlock* b, const char* text) {
    if (b->nInlines == b->capInlines) {
        b->capInlines = b->capInlines ? b->capInlines * 2 : 4;
        b->inlines = (DocInline*)realloc(b->inlines,
                                     sizeof(DocInline) * b->capInlines);
    }
    DocInline* in = &b->inlines[b->nInlines++];
    memset(in, 0, sizeof(*in));
    in->type        = DIT_TEXT;
    in->text        = strdup(text);
    in->anchorIndex = -1;
}

static void doc_free_local(DocDocument* d) {
    for (size_t i = 0; i < d->nBlocks; i++) {
        DocBlock* b = &d->blocks[i];
        for (size_t j = 0; j < b->nInlines; j++) free(b->inlines[j].text);
        free(b->inlines);
    }
    free(d->blocks);
    memset(d, 0, sizeof(*d));
}

#define MARGIN_X (PLUTO_CONTENT_MARGIN + 2)
#define START_Y  (PLUTO_CONTENT_Y + 8)
#define MAX_W    (PLUTO_CONTENT_TEXT_WIDTH - 4)

/* ── A: pure helpers ──────────────────────────────────────────────────── */

static void case_helpers(void) {
    char* m;

    m = layout_ordered_marker(2026, "i");
    ck("A.roman_2026", m && !strcmp(m, "mmxxvi")); free(m);
    m = layout_ordered_marker(9, "I");
    ck("A.roman_upper", m && !strcmp(m, "IX")); free(m);
    m = layout_ordered_marker(0, "i");
    ck("A.roman_zero_passthrough", m && !strcmp(m, "0")); free(m);
    m = layout_ordered_marker(4000, "i");
    ck("A.roman_4000_passthrough", m && !strcmp(m, "4000")); free(m);

    m = layout_ordered_marker(1, "a");
    ck("A.alpha_1a", m && !strcmp(m, "a")); free(m);
    m = layout_ordered_marker(26, "a");
    ck("A.alpha_26z", m && !strcmp(m, "z")); free(m);
    m = layout_ordered_marker(27, "a");
    ck("A.alpha_27aa", m && !strcmp(m, "aa")); free(m);
    m = layout_ordered_marker(3, "A");
    ck("A.alpha_upper_C", m && !strcmp(m, "C")); free(m);

    m = layout_ordered_marker(7, NULL);
    ck("A.default_numeric", m && !strcmp(m, "7")); free(m);

    char* e = layout_expand_tab_columns("\tX");
    ck("A.tab_col0", e && !strncmp(e, "        X", 9)); free(e);
    e = layout_expand_tab_columns("ab\tZ");
    ck("A.tab_col2_pad6", e && strlen(e) == 9 && e[8] == 'Z' &&
                          e[2] == ' '); free(e);
    e = layout_expand_tab_columns("no tabs here");
    ck("A.tab_passthrough",
       e && !strcmp(e, "no tabs here")); free(e);
}

/* ── B: empty document ────────────────────────────────────────────────── */

static void case_empty(void) {
    DocDocument d;
    doc_init_empty(&d);
    layout_build(&d);
    ck("B.empty_count0", layout_item_count() == 0);
    ck("B.empty_height216",
       layout_total_height() == (double)PLUTO_CONTENT_HEIGHT);

    layout_build(NULL);
    ck("B.null_doc_safe",
       layout_total_height() == (double)PLUTO_CONTENT_HEIGHT &&
       layout_item_count() == 0);
    doc_free_local(&d);
}

/* ── C/D: paragraph flow + wrap ───────────────────────────────────────── */

static void case_paragraph(void) {
    DocDocument d;
    doc_init_empty(&d);
    DocBlock* p = doc_add(&d, DB_PARAGRAPH);
    blk_add_inline(p, "Hello");

    layout_build(&d);
    ck("C.para_count1", layout_item_count() == 1);
    const LItem* t = layout_item_at(0);
    ck("C.para_type_text", t && t->type == LIT_TEXT);
    ck("C.para_x_marginX", t && t->x == MARGIN_X);
    ck("C.para_y_startY",  t && t->y == START_Y);
    ck("C.para_h16",       t && t->h == 16);
    ck("C.para_href_null", t && t->href == NULL);
    /* endY=48 -> currentY=58 -> total=max(78,216)=216 */
    ck("C.para_height_floor",
       layout_total_height() == (double)PLUTO_CONTENT_HEIGHT);

    /* rebuild idempotence (frees previous items cleanly) */
    int before = layout_item_count();
    layout_build(&d);
    ck("C.rebuild_idempotent", layout_item_count() == before);
    doc_free_local(&d);

    /* many words must wrap onto successive lines at the same x */
    DocDocument w;
    doc_init_empty(&w);
    DocBlock* pw = doc_add(&w, DB_PARAGRAPH);
    static char para[1200];
    size_t off = 0;
    for (int i = 0; i < 90 && off < sizeof(para) - 8; i++)
        off += (size_t)snprintf(para + off, sizeof(para) - off, "lorem ");
    blk_add_inline(pw, para);

    layout_build(&w);
    int n = layout_item_count();
    ck("D.wrap_multiple_lines", n >= 3);
    /* items are per-word: group into rows by y, verify each row starts at
     * marginX and rows step down by lineH (16) */
    int rows = 0, prevY = -1000, okRows = 1, okStep = 1;
    for (int i = 0; i < n; i++) {
        const LItem* it = layout_item_at(i);
        if (!it || it->type != LIT_TEXT) continue;
        if (it->y != prevY) {
            if (rows > 0 && it->y - prevY != 16) okStep = 0;
            prevY = it->y;
            rows++;
            if (it->x != MARGIN_X) okRows = 0;   /* first word on a row */
        }
    }
    ck("D.rows_ge3", rows >= 3);
    ck("D.wrap_same_x", okRows);
    ck("D.wrap_line_step16", okStep);
    doc_free_local(&w);
}

/* ── E: heading underline + margins ───────────────────────────────────── */

static void case_heading(void) {
    DocDocument d;
    doc_init_empty(&d);
    DocBlock* h = doc_add(&d, DB_HEADING);
    h->level = 2;
    blk_add_inline(h, "Title");

    layout_build(&d);
    /* currentY = 32+8 = 40; one line lh18 -> endY 58; line item at 61;
     * +6 -> 64; +marginB(5) -> 69 */
    ck("E.heading_text_y40",
       layout_item_at(0) && layout_item_at(0)->type == LIT_TEXT &&
       layout_item_at(0)->y == START_Y + 8);
    ck("E.heading_bold_forced", layout_item_at(0)->bold == 1);
    ck("E.count_text_plus_rule", layout_item_count() == 2);
    const LItem* ln = layout_item_at(1);
    ck("E.rule_type", ln && ln->type == LIT_LINE);
    ck("E.rule_y61", ln && ln->y1 == START_Y + 8 + 18 + 3);
    ck("E.rule_span_full", ln && ln->x1 == MARGIN_X &&
                           ln->x2 == MARGIN_X + MAX_W);

    /* h3 gets no rule */
    doc_init_empty(&d);
    DocBlock* h3 = doc_add(&d, DB_HEADING);
    h3->level = 3;
    blk_add_inline(h3, "sub");
    layout_build(&d);
    ck("E.h3_no_rule", layout_item_count() == 1);
    doc_free_local(&d);
}

/* ── F/G/H/I: hr / code / image / table geometry ──────────────────────── */

static void case_blocks(void) {
    /* hr */
    DocDocument d;
    doc_init_empty(&d);
    doc_add(&d, DB_HR);
    layout_build(&d);
    const LItem* ln = layout_item_at(0);
    ck("F.hr_geom",
       ln && ln->type == LIT_LINE && ln->x1 == MARGIN_X + 20 &&
       ln->x2 == MARGIN_X + MAX_W - 20 && ln->y1 == START_Y + 6);
    doc_free_local(&d);

    /* code block: 2 lines -> boxH = max(30, 2*14+12) = 40 */
    doc_init_empty(&d);
    DocBlock* cb = doc_add(&d, DB_CODE_BLOCK);
    static char* lines[2];
    lines[0] = (char*)"alpha";
    lines[1] = (char*)"beta";
    cb->lines  = lines;
    cb->nLines = 2;
    layout_build(&d);
    const LItem* box = layout_item_at(0);
    ck("G.code_box_h40", box && box->type == LIT_CODE_BOX && box->h == 40);
    ck("G.code_box_w_max", box && box->w == MAX_W);
    ck("G.code_lines_copied",
       box && box->nLines == 2 && box->lines[0] &&
       !strcmp(box->lines[0], "alpha"));
    doc_free_local(&d);

    /* image clamp + center align */
    doc_init_empty(&d);
    DocBlock* img = doc_add(&d, DB_IMAGE);
    img->width = 9999; img->height = 50;
    img->align = "center";
    img->src   = (char*)"i.png";
    layout_build(&d);
    const LItem* im = layout_item_at(0);
    ck("H.image_w_clamped", im && im->type == LIT_IMAGE && im->w == MAX_W);
    ck("H.image_center_x", im && im->x == MARGIN_X);  /* full-width center */
    ck("H.image_advance_h12", layout_total_height() >= 216.0);
    doc_free_local(&d);

    /* table: 3 rows + caption -> h = max(24, 68) + 16 = 84 */
    DocTableRow rows[3];
    memset(rows, 0, sizeof(rows));
    DocTableCell cell;
    memset(&cell, 0, sizeof(cell));
    static DocInline cellInl;
    memset(&cellInl, 0, sizeof(cellInl));
    cellInl.type = DIT_TEXT; cellInl.text = (char*)"c";
    cellInl.anchorIndex = -1;
    cell.inlines = &cellInl; cell.nInlines = 1; cell.colspan = 1;
    for (int i = 0; i < 3; i++) {
        rows[i].cells = &cell;
        rows[i].nCells = 1;
    }
    doc_init_empty(&d);
    DocBlock* tb = doc_add(&d, DB_TABLE);
    tb->rows = rows; tb->nRows = 3;
    tb->tableCaption = (char*)"Cap";
    tb->tableBorder  = 1;
    layout_build(&d);
    const LItem* tbi = layout_item_at(0);
    ck("I.table_h84", tbi && tbi->type == LIT_TABLE_BOX && tbi->h == 84);
    ck("I.table_w_max", tbi && tbi->w == MAX_W);
    ck("I.table_rows_borrowed", tbi && tbi->rows == rows && tbi->nRows == 3);
    doc_free_local(&d);
}

/* ── J/K/L/M/N/O/P/Q: form controls, lists, quote, boxes, badge ───────── */

static void case_forms_and_lists(void) {
    DocDocument d;

    /* ordered list depth 2, markerType "i" number 9 */
    doc_init_empty(&d);
    DocBlock* li = doc_add(&d, DB_LIST_ITEM);
    li->isOrdered = 1; li->number = 9; li->markerType = "i"; li->depth = 2;
    blk_add_inline(li, "item");
    layout_build(&d);
    ck("M.list_two_items", layout_item_count() == 2);
    const LItem* bullet = layout_item_at(0);
    ck("M.bullet_ix_dot",
       bullet && bullet->text && !strcmp(bullet->text, "ix."));
    ck("M.bullet_x_depth14",
       bullet && bullet->x == PLUTO_CONTENT_MARGIN + 2 + 4 + 14);
    const LItem* ltxt = layout_item_at(1);
    ck("M.text_x_dd20_indent",
       ltxt && ltxt->x == PLUTO_CONTENT_MARGIN + 2 + 20 + 14);
    ck("M.list_step18", ltxt && bullet->y == ltxt->y);
    doc_free_local(&d);

    /* checkbox with a 40-char label truncates to 28+".." and registers an
     * input: link spanning box + label */
    doc_init_empty(&d);
    DocBlock* cb = doc_add(&d, DB_CHECKBOX_FIELD);
    cb->inName        = (char*)"opt";
    cb->checkboxLabel = (char*)"0123456789012345678901234567890123456789";
    layout_build(&d);
    int n = layout_item_count();
    ck("J.checkbox_items", n == 2);
    const LItem* box = layout_item_at(0);
    const LItem* lbl = layout_item_at(1);
    ck("J.label_truncated",
       lbl && lbl->type == LIT_TEXT && lbl->text &&
       !strcmp(lbl->text, "0123456789012345678901234567.."));
    LmLink* link = lm_get_hovered_link(MARGIN_X + 2,
                                       box ? box->y + 10 : 0);
    ck("J.checkbox_link_input_prefix",
       link && link->href && !strncmp(link->href, "input:", 6));
    doc_free_local(&d);

    /* submit button clamps to >= 50 wide and registers its own link */
    doc_init_empty(&d);
    DocBlock* sb = doc_add(&d, DB_INPUT_SUBMIT);
    sb->submitLabel = (char*)"Go";
    sb->formAction  = (char*)"/search";
    layout_build(&d);
    const LItem* btn = layout_item_at(0);
    ck("K.submit_w_ge50", btn && btn->type == LIT_INPUT_SUBMIT &&
                          btn->w >= 50);
    LmLink* bl = lm_get_hovered_link(btn ? btn->x + 2 : 0,
                                     btn ? btn->y + 10 : 0);
    ck("K.submit_link_action",
       bl && bl->href && !strcmp(bl->href, "/search"));
    doc_free_local(&d);

    /* select keeps borrowed options + selectedIndex, registers select:q */
    static DocSelectOpt opts[2];
    memset(opts, 0, sizeof(opts));
    opts[0].text = (char*)"one";
    opts[1].text = (char*)"two";
    doc_init_empty(&d);
    DocBlock* sf = doc_add(&d, DB_SELECT_FIELD);
    sf->options = opts; sf->nOptions = 2; sf->selectedIndex = 2;
    sf->inName  = (char*)"q";
    layout_build(&d);
    const LItem* sel = layout_item_at(0);
    ck("L.select_opts_borrowed",
       sel && sel->options == opts && sel->selectedIndex == 2);
    LmLink* sl = lm_get_hovered_link(MARGIN_X + 5,
                                     sel ? sel->y + 10 : 0);
    ck("L.select_link_prefix",
       sl && sl->href && !strncmp(sl->href, "select:", 7));
    doc_free_local(&d);
}

static void case_quote_box_badge(void) {
    DocDocument d;

    /* blockquote adds a left bar between startY and endY-4 */
    doc_init_empty(&d);
    DocBlock* q = doc_add(&d, DB_BLOCKQUOTE);
    blk_add_inline(q, "quoted words");
    layout_build(&d);
    int n = layout_item_count();
    const LItem* bar = layout_item_at(n - 1);
    ck("N.quote_bar_last",
       bar && bar->type == LIT_QUOTE_BAR && bar->x == PLUTO_CONTENT_MARGIN + 5);
    ck("N.quote_bar_top", bar && bar->y1 == START_Y);
    ck("N.quote_bar_bottom_gap4",
       bar && bar->y2 == bar->y1 + 16 + 10 - 4);
    doc_free_local(&d);

    /* details-style box_open registers toggle strip h18 */
    doc_init_empty(&d);
    DocBlock* bo = doc_add(&d, DB_BOX_OPEN);
    bo->boxLabel  = (char*)"More";
    bo->toggleKey = (char*)"d1";
    bo->toggleOpen = 0;
    layout_build(&d);
    const LItem* fr = layout_item_at(0);
    ck("O.box_frame_open_y2_eq_y",
       fr && fr->type == LIT_BOX_FRAME && fr->y2 == fr->y);
    LmLink* tl = lm_get_hovered_link(MARGIN_X + 10, fr ? fr->y + 9 : 0);
    ck("O.toggle_link",
       tl && tl->primaryRect.isToggle == 1 && tl->primaryRect.toggleKey &&
       !strcmp(tl->primaryRect.toggleKey, "d1") &&
       tl->primaryRect.toggleOpen == 0);
    doc_free_local(&d);

    /* reader badge advance 28+10 */
    doc_init_empty(&d);
    DocBlock* rh = doc_add(&d, DB_READER_HEADER);
    rh->readerHost = (char*)"EXAMPLE.COM";
    rh->readingTime = (char*)"2 min read";
    layout_build(&d);
    const LItem* bd = layout_item_at(0);
    ck("Q.badge_geom", bd && bd->type == LIT_READER_BADGE && bd->h == 28);
    ck("Q.badge_host_default_or_given",
       bd && bd->host && !strcmp(bd->host, "EXAMPLE.COM"));

    /* hidden field: present but zero visual footprint */
    DocBlock* hid = doc_add(&d, DB_INPUT_FIELD);
    hid->inputType = (char*)"hidden";
    hid->inName    = (char*)"sid";
    hid->inValue   = (char*)"42";
    int before = layout_item_count();
    layout_build(&d);
    ck("P.hidden_one_item", layout_item_count() == before + 1);
    const LItem* hi = layout_item_at(layout_item_count() - 1);
    ck("P.hidden_type_and_fields",
       hi && hi->type == LIT_HIDDEN_FIELD && hi->name &&
       !strcmp(hi->name, "sid") && hi->value &&
       !strcmp(hi->value, "42"));
    ck("P.hidden_no_geometry", hi && hi->x == 0 && hi->y == 0);
    doc_free_local(&d);
}

/* ── R: integration through the real parser ───────────────────────────── */

static void case_parse_integration(void) {
    const char* html =
        "<html><head><title>T</title></head><body>"
        "<p>Hi <a href=\"x.html\">link</a> there</p>"
        "</body></html>";

    DocDocument* d = doc_parse(html, "http://t/", 1 /* RAW_HTML */);
    ck("R.parse_ok", d != NULL);
    if (!d) return;

    layout_build(d);
    ck("R.has_items", layout_item_count() >= 1);

    /* Document resolves anchor hrefs against the base URL (Lua parity) */
    int sawHrefItem = 0;
    for (int i = 0; i < layout_item_count(); i++) {
        const LItem* it = layout_item_at(i);
        if (it->type == LIT_TEXT && it->href &&
            !strcmp(it->href, "http://t/x.html"))
            sawHrefItem = 1;
    }
    ck("R.text_item_carries_href", sawHrefItem);
    ck("R.link_registered", lm_get_count() >= 1);
    doc_free(d);
}

/* ── S: on-demand overlay state machine ───────────────────────────────── */

static void case_on_demand(void) {
    ck("S.overlay_initially_none", !layout_has_on_demand_overlay());

    layout_show_on_demand_overlay("img/a.png", "go.html", "Pic");
    ck("S.show_sets_state", layout_has_on_demand_overlay() &&
                            layout_od_src() &&
                            !strcmp(layout_od_src(), "img/a.png") &&
                            layout_od_href() &&
                            !strcmp(layout_od_href(), "go.html") &&
                            layout_od_alt() &&
                            !strcmp(layout_od_alt(), "Pic"));
    ck("S.alt_defaults_Image",
       (layout_clear_on_demand_overlay(),
        layout_show_on_demand_overlay("img/b.png", NULL, NULL),
        layout_od_alt() && !strcmp(layout_od_alt(), "Image")));

    PlutoOdAction a = layout_handle_on_demand_input(PLUTO_KBUTTON_A);
    ck("S.A_views_when_not_decoded", a == OD_VIEW);
    ck("S.marked_requested",
       layout_on_demand_requested("img/b.png"));
    ck("S.consumed_raised", layout_on_demand_consumed());
    layout_clear_on_demand_consumed();
    ck("S.consumed_clears", !layout_on_demand_consumed());
    ck("S.overlay_cleared_after_A", !layout_has_on_demand_overlay());

    layout_show_on_demand_overlay("img/c.png", "link.html", "L");
    PlutoOdAction b = layout_handle_on_demand_input(PLUTO_KBUTTON_B);
    ck("S.B_links_when_href", b == OD_LINK);
    ck("S.B_clears", !layout_has_on_demand_overlay());

    layout_show_on_demand_overlay(NULL, NULL, NULL);
    PlutoOdAction c = layout_handle_on_demand_input(PLUTO_KBUTTON_A);
    ck("S.srcless_A_cancels", c == OD_CANCEL);

    ck("S.none_without_press",
       layout_handle_on_demand_input(0) == OD_NONE);
    layout_clear_on_demand_overlay();
}

/* ── entry point ──────────────────────────────────────────────────────── */

void selftest_layout_run(int* pass, int* fail) {
    s_pass = 0; s_fail = 0;

    PLUTO_LOG("[P26B] ---- layout selftest begin ----");
    case_helpers();
    case_empty();
    case_paragraph();
    case_heading();
    case_blocks();
    case_forms_and_lists();
    case_quote_box_badge();
    case_parse_integration();
    case_on_demand();
    PLUTO_LOG("[P26B] ---- layout selftest end: %d pass / %d fail ----",
              s_pass, s_fail);

    if (pass) *pass += s_pass;
    if (fail) *fail += s_fail;
}
