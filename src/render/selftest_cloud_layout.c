// selftest_cloud_layout.c — P26 verification harness.
//
// Exercises CloudLayout.parse/build against fixture payloads (all element
// types, malformed JSON, defaults, empty docs) plus the LinkManager form
// input registration. cl_draw itself is exercised visually by the sim.

#include "render/selftest_cloud_layout.h"

#include <string.h>

#include "core/constants.h"
#include "core/logger.h"
#include "render/cloud_layout.h"
#include "render/link_manager.h"
#include "render/style.h"
#include "util/json.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P26] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P26] FAIL %s", name);
    }
}

/* Fixture covering every element type incl. a negative-y skip. */
static const char* kFixture =
    "{\"title\":\"Cloud Demo\",\"totalHeight\":500,\"elements\":["
    "{\"type\":\"text\",\"x\":8,\"y\":4,\"w\":384,\"h\":20,"
    "\"text\":\"Hello Cloud\",\"font\":\"large\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":30,\"w\":200,\"h\":14,"
    "\"text\":\"bold line\",\"font\":\"bold\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":50,\"w\":200,\"h\":14,"
    "\"text\":\"code line\",\"font\":\"mono\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":70,\"w\":300,\"h\":14,"
    "\"text\":\"plain line\"},"
    "{\"type\":\"image\",\"x\":8,\"y\":90,\"w\":64,\"h\":64,"
    "\"src\":\"img/logo.png\"},"
    "{\"type\":\"link\",\"x\":8,\"y\":160,\"w\":100,\"h\":14,"
    "\"href\":\"https://example.com/a\"},"
    "{\"type\":\"input\",\"x\":8,\"y\":180,\"w\":180,\"h\":22,"
    "\"inputType\":\"email\",\"name\":\"email\",\"value\":\"a@b.c\","
    "\"placeholder\":\"you@example.com\"},"
    "{\"type\":\"submit\",\"x\":196,\"y\":180,\"w\":60,\"h\":22,"
    "\"label\":\"Go\",\"formAction\":\"/search\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":-40,\"w\":100,\"h\":14,"
    "\"text\":\"hidden\"}"
    "]}";

/* ── valid payload ────────────────────────────────────────────────────── */

static void case_parse_valid(void) {
    ClDoc* doc = cl_parse(kFixture, strlen(kFixture), NULL);
    ck("P.parse_ok", doc != NULL);
    if (doc == NULL) return;

    ck("P.title", strcmp(cl_doc_title(doc), "Cloud Demo") == 0);

    const JsonValue* els = cl_doc_elements(doc);
    size_t n = (els != NULL) ? json_arr_count(els) : 0;
    ck("P.elements_count", n == 9);

    cl_build(doc);

    /* 4 texts - 1 hidden + 1 image + 1 input + 1 submit = 7
     * (the plain link registers in LinkManager, not as an item) */
    ck("B.item_count", cl_item_count() == 7);

    int okTypes = 1;
    ClItemType want[] = { CL_TEXT, CL_TEXT, CL_TEXT,
                          CL_TEXT, CL_IMAGE, CL_INPUT, CL_SUBMIT };
    for (int i = 0; i < 7; i++) {
        const ClItem* it = cl_item_at(i);
        if (it == NULL || it->type != want[i]) okTypes = 0;
    }
    ck("B.item_types_order", okTypes);

    const ClItem* t0 = cl_item_at(0);
    ck("B.y_offset_content_y", t0->y == 24 + 4);   /* CONTENT_Y + el.y */
    ck("B.text_copy", t0->text && strcmp(t0->text, "Hello Cloud") == 0);

    /* font slot mapping */
    int sz, lh;
    PlutoFont* h1 = style_get_heading_font(1, NULL, NULL);
    PlutoFont* bb = style_get_body_font(1, 0, &sz);
    PlutoFont* mo = style_get_body_font(0, 1, &sz);
    PlutoFont* bo = style_get_body_font(0, 0, &sz);
    ck("F.large_heading1", cl_item_at(0)->font == h1);
    ck("F.bold_bodyBold", cl_item_at(1)->font == bb);
    ck("F.mono_mono", cl_item_at(2)->font == mo);
    ck("F.default_body", cl_item_at(3)->font == bo);

    const ClItem* img = cl_item_at(4);
    ck("I.src_alt_default",
       img->src && strcmp(img->src, "img/logo.png") == 0 &&
       img->alt && strcmp(img->alt, "Image") == 0);

    const ClItem* inp = cl_item_at(5);
    ck("N.input_fields",
       inp->inputType && strcmp(inp->inputType, "email") == 0 &&
       inp->name && strcmp(inp->name, "email") == 0 &&
       inp->value && strcmp(inp->value, "a@b.c") == 0 &&
       inp->placeholder &&
       strcmp(inp->placeholder, "you@example.com") == 0);

    const ClItem* sub = cl_item_at(6);
    ck("N.submit_fields",
       sub->label && strcmp(sub->label, "Go") == 0 &&
       sub->formAction && strcmp(sub->formAction, "/search") == 0);

    ck("S.total_height_kept", cl_total_height() == 500.0);
    ck("X.no_selected_input", cl_selected_input() == NULL);

    /* negative-y text skipped entirely */
    int hiddenFound = 0;
    for (int i = 0; i < cl_item_count(); i++)
        if (cl_item_at(i)->text &&
            strcmp(cl_item_at(i)->text, "hidden") == 0)
            hiddenFound = 1;
    ck("B.negative_y_skipped", !hiddenFound);

    /* LinkManager got: 1 plain link + input link + submit link.
     * Walk the selection exactly count times: selectNext wraps around,
     * so it never signals the end by returning NULL. */
    ck("L.lm_count_3", lm_get_count() == 3);
    LmLink* l = (lm_get_count() > 0) ? lm_select_next(0) : NULL;
    int formLinks = 0, blocksOk = 0;
    size_t steps = 1;
    while (l != NULL && steps <= lm_get_count()) {
        if (l->isFormInput) {
            formLinks++;
            if (l->inputBlock == (void*)inp || l->inputBlock == (void*)sub)
                blocksOk++;
        }
        l = lm_select_next(l->primaryRect.y);
        steps++;
    }
    ck("L.form_links_2", formLinks == 2);
    ck("L.input_blocks_wired", blocksOk == 2);

    cl_doc_free(doc);
}

/* ── malformed JSON → Parse Error fallback ────────────────────────────── */

static void case_parse_error(void) {
    static const char* bad = "{\"title\": \"Broken\",,,}";
    ClDoc* doc = cl_parse(bad, strlen(bad), NULL);
    ck("E.doc_returned", doc != NULL);
    if (doc == NULL) return;

    ck("E.title_parse_error", strcmp(cl_doc_title(doc), "Parse Error") == 0);
    ck("E.elements_empty", cl_doc_elements(doc) == NULL);

    cl_build(doc);
    ck("E.build_resets_to_content_h",
       cl_total_height() == (double)PLUTO_CONTENT_HEIGHT &&
       cl_item_count() == 0);

    cl_doc_free(doc);
}

/* ── defaults: missing title/totalHeight/elements ─────────────────────── */

static void case_defaults(void) {
    static const char* bare = "{}";
    ClDoc* doc = cl_parse(bare, strlen(bare), NULL);
    ck("D.doc_returned", doc != NULL);
    if (doc == NULL) return;

    ck("D.title_default", strcmp(cl_doc_title(doc), "Cloud Page") == 0);
    ck("D.height_default", cl_doc_total_height(doc) == 240.0);
    ck("D.elements_default_empty", cl_doc_elements(doc) == NULL);

    cl_build(doc);
    ck("D.build_empty_resets",
       cl_total_height() == (double)PLUTO_CONTENT_HEIGHT);

    cl_doc_free(doc);
}

/* ── null doc / null json safety ──────────────────────────────────────── */

static void case_null_safety(void) {
    cl_build(NULL);
    ck("G.null_doc_safe",
       cl_total_height() == (double)PLUTO_CONTENT_HEIGHT &&
       cl_item_count() == 0);

    ClDoc* doc = cl_parse(NULL, 0, NULL);
    ck("G.null_json_is_parse_error",
       doc != NULL && strcmp(cl_doc_title(doc), "Parse Error") == 0);
    if (doc != NULL) cl_doc_free(doc);

    ck("G.item_at_oob", cl_item_at(-1) == NULL && cl_item_at(99) == NULL);
}

/* ── rebuild clears previous state (build is idempotent) ─────────────── */

static void case_rebuild(void) {
    ClDoc* doc = cl_parse(kFixture, strlen(kFixture), NULL);
    if (doc == NULL) {
        ck("R.parse", 0);
        return;
    }
    cl_build(doc);
    int first = cl_item_count();
    cl_build(doc);
    ck("R.rebuild_stable",
       cl_item_count() == first && lm_get_count() == 3);
    cl_doc_free(doc);
    cl_free_items();
    lm_clear();
}

int selftest_cloud_layout_run(int* passed, int* failed) {
    s_pass = s_fail = 0;

    case_parse_valid();
    case_parse_error();
    case_defaults();
    case_null_safety();
    case_rebuild();

    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P26] CloudLayout selftest complete: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}
