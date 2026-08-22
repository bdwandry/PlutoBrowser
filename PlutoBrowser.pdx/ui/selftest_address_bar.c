// selftest_address_bar.c — P29 verification harness (AddressBar).
//
// Pins the pure decision logic (trim, search-vs-URL, launch gate, prefill
// rule) and open/cancel state transitions. Real keyboard rendering/typing
// is exercised by the manual sim test (frame 600 auto-launch).

#include "ui/selftest_address_bar.h"

#include <string.h>

#include "core/constants.h"
#include "core/logger.h"
#include "core/url.h"
#include "ui/address_bar.h"
#include "util/strbuf.h"

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) {
        s_pass++;
        PLUTO_LOG("[P29] PASS %s", name);
    } else {
        s_fail++;
        PLUTO_ERROR("[P29] FAIL %s", name);
    }
}

/* ── trim ─────────────────────────────────────────────────────────────── */

static void case_trim(void) {
    char b[PLUTO_URL_INPUT_MAX];

    snprintf(b, sizeof(b), "%s", "  hello  ");
    ab_trim(b);
    ck("T.basic", strcmp(b, "hello") == 0);

    snprintf(b, sizeof(b), "%s", "\t\n x \r");
    ab_trim(b);
    ck("T.mixed_ws", strcmp(b, "x") == 0);

    snprintf(b, sizeof(b), "%s", "");
    ab_trim(b);
    ck("T.empty", b[0] == '\0');

    snprintf(b, sizeof(b), "%s", "   ");
    ab_trim(b);
    ck("T.all_ws", b[0] == '\0');

    snprintf(b, sizeof(b), "%s", "a b c");
    ab_trim(b);
    ck("T.inner_kept", strcmp(b, "a b c") == 0);

    /* Lua ^%s*(.-)%s*$ also strips a fully-wrapped single word */
    snprintf(b, sizeof(b), "%s", "\v\faaa\f\v");
    ab_trim(b);
    ck("T.vertical_tab", strcmp(b, "aaa") == 0);
}

/* ── final-URL decision ───────────────────────────────────────────────── */

static void case_final_url(void) {
    /* PlutoUrl + URL caps are large; keep them off the frame stack */
    static char out[PLUTO_URL_NORMALIZED_MAX];
    static char expect[PLUTO_URL_NORMALIZED_MAX];
    static PlutoUrl p;
    static PlutoUrl p2;

    /* empty -> cancel path (returns 0) */
    ck("F.empty_cancels",
       ab_build_final_url("   ", out, sizeof(out)) == 0);

    /* search query: multi-word input routes to the selected engine */
    {
        StrBuf sb;
        sb_init(&sb);
        url_build_search_url(PLUTO_SEARCH_ENGINES[0].url,
                             "playdate sdk review", &sb);
        snprintf(expect, sizeof(expect), "%s",
                 sb.data ? sb.data : "");
        sb_free(&sb);
    }
    ck("F.search_query_is_search",
       ab_build_final_url("playdate sdk review", out, sizeof(out)) == 1);
    ck("F.search_engine_url", strcmp(out, expect) == 0);

    /* non-search: URL.parse normalized parity */
    url_parse("example.com/path?a=1", &p);
    ck("F.url_normalized",
       ab_build_final_url("example.com/path?a=1", out, sizeof(out)) == 1 &&
       strcmp(out, p.normalized) == 0);

    /* scheme'd page keeps normalization but not search routing */
    url_parse("about:blank", &p2);
    ck("F.about_blank",
       ab_build_final_url("about:blank", out, sizeof(out)) == 1 &&
       strcmp(out, p2.normalized) == 0 && strcmp(out, "about:blank") == 0);
}

/* ── launch gate ──────────────────────────────────────────────────────── */

static void case_gate(void) {
    ck("G.closed_never", ab_should_launch_keyboard(0, 0, 0) == 0);
    ck("G.already_shown", ab_should_launch_keyboard(1, 1, 0) == 0);
    ck("G.b_held_blocks", ab_should_launch_keyboard(1, 0, 1) == 0);
    ck("G.armed_launches", ab_should_launch_keyboard(1, 0, 0) == 1);
}

/* ── open prefill + cancel ────────────────────────────────────────────── */

static int s_submitted = 0;
static const char* s_lastUrl = "";
static void submit_cb(const char* url, void* ud) {
    (void)ud;
    s_submitted = 1;
    s_lastUrl = url;
}

static void case_open_cancel(void) {
    /* about: URLs are not prefilled */
    ab_open("about:blank", submit_cb, NULL);
    ck("O.about_not_prefilled", strcmp(ab_input_text(), "") == 0);
    ck("O.open_state", ab_is_open() == 1 && ab_keyboard_shown() == 0);

    /* real URLs are */
    ab_open("https://example.com/x?y=1", submit_cb, NULL);
    ck("O.url_prefilled",
       strcmp(ab_input_text(), "https://example.com/x?y=1") == 0);

    /* NULL currentUrl -> empty prefill */
    ab_open(NULL, submit_cb, NULL);
    ck("O.null_empty", strcmp(ab_input_text(), "") == 0);

    /* cancel clears state + callback without firing it */
    s_submitted = 0;
    ab_cancel();
    ck("C.closed", ab_is_open() == 0 && ab_keyboard_shown() == 0);
    ck("C.no_submit_fired", s_submitted == 0);
    ck("C.no_skip_set", ab_input_skip_remaining() == 0);

    /* after cancel the callback is gone: re-open with NULL cb and make
     * sure nothing dangles (state-level guarantee) */
    ab_open(NULL, NULL, NULL);
    ab_cancel();
}

/* ── skip-frame counter defaults ──────────────────────────────────────── */

static void case_skip(void) {
    ck("S.default_zero",
       ab_input_skip_remaining() == 0 && ab_pop_input_skip() == 0);
}

int selftest_address_bar_run(int* passed, int* failed) {
    s_pass = s_fail = 0;

    case_trim();
    case_final_url();
    case_gate();
    case_open_cancel();
    case_skip();

    if (passed) *passed += s_pass;
    if (failed) *failed += s_fail;
    PLUTO_LOG("[P29] AddressBar selftest complete: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}
