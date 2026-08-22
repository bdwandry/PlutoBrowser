// selftest_browser.c -- [P32] offline selftests for the browser engine.
//
// Fully deterministic: a fake playdate_tcp vtable serves canned pages from
// a route table (same injection pattern as the P07 suite), a fake
// millisecond clock feeds both HttpClient and the engine, and a scripted
// BrInput queue replaces hardware buttons. The suite drives br_frame()
// exactly like the real run loop, so navigation, rendering tasks, form
// submission, history, crank physics and the pause menu are all exercised
// end to end. Results land in pluto.log.

#include "core/selftest_browser.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "core/browser.h"
#include "core/constants.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "core/storage.h"
#include "core/tasks.h"
#include "html/document.h"
#include "render/layout.h"
#include "render/link_manager.h"
#include "util/mem.h"
#include "util/strbuf.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name) {
    if (cond) { st_pass++; PLUTO_LOG("[P32] PASS %s", name); }
    else      { st_fail++; PLUTO_ERROR("[P32] FAIL %s", name); }
}

static void check_str(const char* name, const char* got, const char* exp) {
    st_check(got != NULL && strcmp(got, exp) == 0, name);
}

/* ── fake TCP vtable (compact P07 copy) ───────────────────────────────── */

#define MAX_SOCKS 8

typedef struct FakeSock {
    char server[160];
    int port, usessl;
    unsigned id;
    TCPOpenCallback* openCb;
    void* openUd;
    TCPConnectionCallback* closedCb;
    PDNetErr openErr;
    int failWrite;
    StrBuf queue;
    StrBuf written;
    int opened, fed, closedCount;
} FakeSock;

static FakeSock SOCKS[MAX_SOCKS];
static int NSOCKS;
static unsigned CLOCK_NOW = 2000000;

static enum accessReply f_requestAccess(const char* server, int port,
                                        bool usessl, const char* purpose,
                                        AccessRequestCallback* cb, void* ud)
{ (void)server;(void)port;(void)usessl;(void)purpose;(void)cb;(void)ud;
  return kAccessAllow; }

static TCPConnection* f_newConnection(const char* server, int port, bool ssl)
{
    if (NSOCKS >= MAX_SOCKS) return NULL;
    FakeSock* s = &SOCKS[NSOCKS++];
    memset(s, 0, sizeof(*s));
    snprintf(s->server, sizeof(s->server), "%s", server ? server : "");
    s->port = port; s->usessl = ssl ? 1 : 0;
    sb_init(&s->queue); sb_init(&s->written);
    return (TCPConnection*)s;
}
static TCPConnection* f_retain(TCPConnection* c) { return c; }
static void f_release(TCPConnection* c) { (void)c; }
static PDNetErr f_getError(TCPConnection* c) { (void)c; return NET_OK; }
static void f_setConnectTimeout(TCPConnection* c, int ms) { (void)c;(void)ms; }
static void f_setReadTimeout(TCPConnection* c, int ms) { (void)c;(void)ms; }
static void f_setReadBufferSize(TCPConnection* c, int b) { (void)c;(void)b; }
static void f_setUserdata(TCPConnection* c, void* ud)
{ ((FakeSock*)c)->id = (unsigned)(uintptr_t)ud; }
static void* f_getUserdata(TCPConnection* c)
{ return (void*)(uintptr_t)((FakeSock*)c)->id; }
static PDNetErr f_open(TCPConnection* c, TCPOpenCallback cb, void* ud)
{ ((FakeSock*)c)->openCb = cb; ((FakeSock*)c)->openUd = ud; return NET_OK; }
static PDNetErr f_close(TCPConnection* c)
{ ((FakeSock*)c)->closedCount++; return NET_OK; }
static void f_setClosedCb(TCPConnection* c, TCPConnectionCallback* cb)
{ ((FakeSock*)c)->closedCb = cb; }
static size_t f_getBytesAvailable(TCPConnection* c)
{ return ((FakeSock*)c)->queue.len; }
static int f_read(TCPConnection* c, void* buffer, size_t length)
{
    FakeSock* s = (FakeSock*)c;
    size_t n = s->queue.len < length ? s->queue.len : length;
    if (n > 0) {
        memcpy(buffer, s->queue.data, n);
        memmove(s->queue.data, s->queue.data + n, s->queue.len - n);
        s->queue.len -= n;
    }
    return (int)n;
}
static int f_write(TCPConnection* c, const void* buffer, size_t length)
{
    FakeSock* s = (FakeSock*)c;
    if (s->failWrite) return s->failWrite;
    sb_append(&s->written, buffer, length);
    return (int)length;
}
static size_t f_getSentPending(TCPConnection* c) { (void)c; return 0; }

static struct playdate_tcp FAKE_TCP = {
    f_requestAccess, f_newConnection, f_retain, f_release, f_getError,
    f_setConnectTimeout, f_setUserdata, f_getUserdata,
    f_open, f_close, f_setClosedCb, f_setReadTimeout, f_setReadBufferSize,
    f_getBytesAvailable, f_read, f_write, f_getSentPending,
};

/* ── route table + auto-serving ───────────────────────────────────────── */

typedef struct {
    const char* path;     /* prefix match against the request target */
    const char* body;     /* full HTML body */
    int netfail;          /* 1: arm write failure for matching host */
    const char* host;     /* host used for open-time netfail arming */
} Route;

static Route ROUTES[8];
static int NROUTES;
static char g_lastTarget[512];   /* request path incl. query */
static char g_lastHost[128];

static void set_routes(Route* r, int n) {
    NROUTES = 0;
    for (int i = 0; i < n && i < 8; i++) ROUTES[NROUTES++] = r[i];
    g_lastTarget[0] = '\0';
    g_lastHost[0] = '\0';
}

static void serve_pending(void) {
    for (int i = 0; i < NSOCKS; i++) {
        FakeSock* s = &SOCKS[i];
        if (s->openCb != NULL && !s->opened) {
            s->opened = 1;
            /* arm network failure before the first write happens */
            for (int rI = 0; rI < NROUTES; rI++)
                if (ROUTES[rI].netfail && ROUTES[rI].host != NULL &&
                    !strcmp(ROUTES[rI].host, s->server)) {
                    s->failWrite = NET_WRITE_ERROR;
                    break;
                }
            s->openCb((TCPConnection*)s, NET_OK, s->openUd);
        }
        if (s->opened && !s->fed && s->written.len > 0) {
            /* record request line + host */
            static char line[768];
            size_t take = s->written.len < sizeof(line) - 1
                              ? s->written.len : sizeof(line) - 1;
            memcpy(line, s->written.data, take);
            line[take] = '\0';
            char* sp = strchr(line, ' ');
            if (sp != NULL) {
                char* e = strchr(sp + 1, ' ');
                if (e != NULL) {
                    size_t n = (size_t)(e - sp - 1);
                    if (n >= sizeof(g_lastTarget))
                        n = sizeof(g_lastTarget) - 1;
                    memcpy(g_lastTarget, sp + 1, n);
                    g_lastTarget[n] = '\0';
                }
            }
            const char* h = strstr(line, "Host: ");
            if (h != NULL) {
                const char* e = strstr(h, "\r\n");
                size_t n = e ? (size_t)(e - h - 6)
                             : strlen(h + 6);
                if (n >= sizeof(g_lastHost)) n = sizeof(g_lastHost) - 1;
                memcpy(g_lastHost, h + 6, n);
                g_lastHost[n] = '\0';
            }

            Route* hit = NULL;
            for (int rI = 0; rI < NROUTES; rI++)
                if (!strncmp(g_lastTarget, ROUTES[rI].path,
                             strlen(ROUTES[rI].path)))
                    { hit = &ROUTES[rI]; break; }

            if (hit != NULL && hit->netfail) {
                s->failWrite = NET_WRITE_ERROR;
                return;
            }
            if (hit == NULL) hit = &ROUTES[NROUTES - 1]; /* catch-all last */

            static char resp[4096];
            snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\n"
                     "\r\n%s",
                     (int)strlen(hit->body), hit->body);
            sb_append_str(&s->queue, resp);
            s->fed = 1;
            if (s->closedCb != NULL)
                s->closedCb((TCPConnection*)s, NET_CONNECTION_CLOSED);
        }
    }
}

/* ── scripted input + clock ───────────────────────────────────────────── */

static BrInput Q[256];
static int nQ, qHead;
static BrInput CURRENT;

static unsigned browser_fake_clock(void) { return CLOCK_NOW; }

static void push_input(unsigned held, unsigned justPressed, float crank) {
    if (nQ >= 256) return;
    Q[nQ].held = held;
    Q[nQ].justPressed = justPressed;
    Q[nQ].crankChange = crank;
    CLOCK_NOW += 16;
    Q[nQ].nowMs = CLOCK_NOW;
    nQ++;
}

static void provider(BrInput* out, void* ud) {
    (void)ud;
    if (qHead < nQ) { CURRENT = Q[qHead++]; *out = CURRENT; }
    else {
        memset(out, 0, sizeof(*out));
        CLOCK_NOW += 16;
        out->nowMs = CLOCK_NOW;
    }
}

/* one frame: serve sockets, step inputs, run the real composition */
static void frame(void) {
    serve_pending();
    br_frame();
}

static void pump(int frames) { for (int i = 0; i < frames; i++) frame(); }

static int cond_state_page(void) { return br_state() == PLUTO_STATE_PAGE; }

/* settled = page reached AND cooperative build finished */
static int cond_settled(void) {
    return br_state() == PLUTO_STATE_PAGE && !br_is_rendering();
}

static void pump_until_page(int maxFrames) {
    frame(); /* always run at least one frame so pendingNav is consumed
                even when the previous page is still on screen */
    for (int i = 0; i < maxFrames && !cond_settled(); i++) frame();
    /* idle tail: absorbs skipInputFrames armed by fake-clock jumps
     * (>500ms gap closes overlays and ignores input for 2 frames) */
    pump(3);
}

/* settle with NO idle tail: for flows where side effects must not fire
 * yet (e.g. a meta-refresh deadline landing during the tail frames) */
static void pump_settle_once(int maxFrames) {
    frame();
    for (int i = 0; i < maxFrames && !cond_settled(); i++) frame();
}

static void reset_all(void) {
    for (int i = 0; i < NSOCKS; i++) {
        sb_free(&SOCKS[i].queue);
        sb_free(&SOCKS[i].written);
    }
    NSOCKS = 0;
    NROUTES = 0;
    nQ = qHead = 0;
    CLOCK_NOW += 1000;
    storage_settings()->mode = PLUTO_MODE_RAW_HTML; /* undo earlier suites */
    br_reset_for_tests();
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

static const char PAGE_A[] =
    "<html><head><title>Page A</title></head><body>"
    "<p>hello <a href=\"b.html\">bee</a> world</p></body></html>";

static const char PAGE_B[] =
    "<html><head><title>Page B</title></head><body><p>bee page</p></body></html>";

static const char TALL_PAGE[] =
    "<html><head><title>Tall</title></head><body>";
static const char TALL_PAGE_END[] = "</body></html>";

static void build_tall(char* out, size_t cap) {
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "%s", TALL_PAGE);
    for (int i = 0; i < 24; i++)
        o += (size_t)snprintf(out + o, cap - o,
             "<p>para %d words to wrap around a bit</p>"
             "<a href=\"l%d.html\">link %d</a>", i, i, i);
    snprintf(out + o, cap - o, "%s", TALL_PAGE_END);
}

static const char FORM_PAGE[] =
    "<html><head><title>Form</title></head><body>"
    "<form action=\"/submit\">"
    "<input type=\"hidden\" name=\"h\" value=\"1\">"
    "<input type=\"text\" name=\"q2\" value=\"v 2\">"
    "<label><input type=\"checkbox\" name=\"c1\" checked>one</label>"
    "<label><input type=\"checkbox\" name=\"c2\">two</label>"
    "<input type=\"radio\" name=\"r\" value=\"ra\" checked>"
    "<input type=\"radio\" name=\"r\" value=\"rb\">"
    "<select name=\"sel\">"
    "<option value=\"s1\">first</option>"
    "<option value=\"s2\" selected>second</option>"
    "</select>"
    "<input type=\"submit\" name=\"btn\" value=\"go\">"
    "</form></body></html>";

static const char FORM_FALLBACK_PAGE[] =
    "<html><head><title>Fb</title></head><body>"
    "<form action=\"/x\"><input type=\"text\" name=\"f1\" value=\"a\"></form>"
    "<form action=\"/other\"><input type=\"submit\" name=\"s\" value=\"v\"></form>"
    "</body></html>";

static const char META_PAGE[] =
    "<html><head><meta http-equiv=\"refresh\" content=\"0; url=/next\">"
    "<title>Meta</title></head><body><p>m</p></body></html>";

static const char NEXT_PAGE[] =
    "<html><head><title>Next Page</title></head><body><p>n</p></body></html>";

static const char DETAILS_PAGE[] =
    "<html><head><title>Det</title></head><body>"
    "<details><summary>more info</summary><p>secret body</p></details>"
    "<p>after</p></body></html>";

/* find the Nth render item of a given type */
static LItem* find_item(int type, int nth) {
    int seen = 0;
    for (int i = 0; i < layout_item_count(); i++) {
        LItem* it = layout_item_mutable(i);
        if (it->type == type && seen++ == nth) return it;
    }
    return NULL;
}

/* ── test groups ──────────────────────────────────────────────────────── */

static void t_navigation_flow(void) {
    reset_all();

    /* boot lands on home with an empty history */
    br_boot();
    st_check(br_state() == PLUTO_STATE_HOME, "A.boot_home");
    check_str("A.boot_title", br_page_title(), "CometBrowser");
    st_check(br_history_count() == 0, "A.boot_history_empty");

    /* about:home special case: no HTTP, start-page title, pushed */
    br_navigate_to("about:home");
    pump(2);
    st_check(br_state() == PLUTO_STATE_HOME, "B.about_home_state");
    check_str("B.about_home_title", br_page_title(),
              "CometBrowser Start Page");
    st_check(br_history_count() == 1 &&
             !strcmp(br_history_at(0), "about:home"),
             "B.about_home_pushed");

    /* bare host gets https:// prepended and fetches */
    Route ra[] = { { "/b.html", PAGE_B, 0, NULL },
                   { "/", PAGE_A, 0, NULL } };   /* exact "/" must be LAST */
    set_routes(ra, 2);
    br_navigate_to("example.com");
    pump_until_page(30);
    st_check(br_state() == PLUTO_STATE_PAGE, "C.page_reached");
    check_str("C.prepended_host", g_lastHost, "example.com");
    check_str("C.requested_path", g_lastTarget, "/");
    check_str("C.normalized",
              br_current_normalized(), "https://example.com/");
    check_str("C.doc_title", br_page_title(), "Page A");
    st_check(br_history_count() == 2 &&
             !strcmp(br_history_at(1), "https://example.com"),
             "C.history_pushed");

    /* search-query detection rewrites to duckduckgo */
    br_navigate_to("playdate wiki");
    pump_until_page(30);
    st_check(strstr(g_lastTarget, "/html/?q=playdate") != NULL,
             "D.search_query_prefix");
    st_check(strstr(g_lastTarget, "wiki") != NULL, "D.search_query_term");
    check_str("D.search_host", g_lastHost, "html.duckduckgo.com");

    /* relative link follow via selection + A (d-pad walk is READER-mode
     * behavior in CometBrowser, so switch modes like a user would) */
    storage_settings()->mode = PLUTO_MODE_READER;
    br_boot();                     /* re-reads mode into the engine */
    br_navigate_to("http://links.test/");
    pump_until_page(30);
    push_input(0, 0, 0);                       /* settle */
    frame();
    push_input(BR_BTN_DOWN, BR_BTN_DOWN, 0);   /* select first link */
    frame();
    st_check(lm_get_selected_link() != NULL, "E.down_selects");
    push_input(BR_BTN_A, BR_BTN_A, 0);         /* follow it */
    frame();
    pump_until_page(30);
    check_str("E.followed_doc_title", br_page_title(), "Page B");
    st_check(strstr(g_lastTarget, "/b.html") != NULL, "E.relative_resolved");
}

static void t_history_stack(void) {
    reset_all();
    br_boot();
    Route r1[] = { { "/", PAGE_A, 0 } };
    set_routes(r1, 1);

    /* seed three entries */
    const char* urls[] = { "u.test/1", "u.test/2", "u.test/3" };
    for (int i = 0; i < 3; i++) {
        br_navigate_to(urls[i]);
        pump_until_page(30);
    }
    st_check(br_history_count() == 3, "F.seeded_count");  /* boot pushes 0 */
    st_check(br_history_index() == 2, "F.seeded_index");

    /* consecutive duplicate is dropped */
    br_navigate_to("u.test/3");
    pump_until_page(30);
    st_check(br_history_count() == 3, "F.dedupe_consecutive");

    /* fresh nav appends while at the tip */
    br_navigate_to("u.test/4");
    pump_until_page(30);
    st_check(br_history_count() == 4, "G.truncate_future_count");
    st_check(br_history_index() == 3, "G.truncate_index");

    /* walk back to the start */
    st_check(br_go_back() == 1, "G.back_ok");
    st_check(br_history_index() == 2, "G.back_index");
    check_str("G.back_url", br_history_at(2), "https://u.test/3");
    br_go_back(); br_go_back();
    st_check(br_history_index() == 0, "G.back_to_start");
    st_check(br_go_back() == 0, "G.back_boundary");

    /* walk forward to the tip */
    st_check(br_go_forward() == 1, "G.forward_ok");
    br_go_forward(); br_go_forward();
    st_check(br_history_index() == 3, "G.forward_index_tip");
    st_check(strstr(br_history_at(3), "u.test/4") != NULL,
             "G.tip_is_u4");

    /* consume the navigatingHistory arm with a real (deduped) jump,
     * exactly like a user completing a back/forward navigation */
    br_navigate_to("u.test/4");
    pump_until_page(30);
    st_check(br_history_count() == 4, "G.arm_consumed");

    /* cap 30: oldest entries fall off */
    for (int i = 10; i < 45; i++) {
        static char u[64];
        snprintf(u, sizeof(u), "cap.test/p%d", i);
        br_navigate_to(u);
        pump_until_page(30);
    }
    st_check(br_history_count() == 30, "H.cap30_count");
    st_check(br_history_index() == 29, "H.cap30_index_at_end");
    st_check(strstr(br_history_at(0), "p15") != NULL, "H.oldest_trimmed");
}

static void t_loading_and_error(void) {
    reset_all();
    br_boot();
    Route r1[] = { { "/", PAGE_A, 0 } };
    set_routes(r1, 1);

    br_navigate_to("slow.test/");
    pump(1);                                   /* pendingNav consumed */
    st_check(br_state() == PLUTO_STATE_LOADING, "I.loading_state");
    st_check(hc_is_loading() == 1, "I.http_in_flight");

    /* B cancels back to home */
    push_input(BR_BTN_B, BR_BTN_B, 0);
    frame();
    pump(2);
    st_check(br_state() == PLUTO_STATE_HOME, "I.cancel_b_home");
    st_check(hc_is_loading() == 0, "I.cancel_stopped_http");

    /* Left cancels and goes back */
    br_navigate_to("slow2.test/");
    pump(1);
    push_input(BR_BTN_LEFT, BR_BTN_LEFT, 0);
    frame();
    pump(2);
    st_check(br_state() == PLUTO_STATE_HOME ||
             br_state() == PLUTO_STATE_LOADING, "I.cancel_left_back");

    /* network failure lands on the error page; retry recovers */
    Route dead[] = { { "/", PAGE_A, 1, "dead.test" } };
    set_routes(dead, 1);
    br_navigate_to("dead.test/");
    pump(1);
    st_check(br_state() == PLUTO_STATE_LOADING, "J.dead_loading");
    pump(10);                                  /* open -> write fails */
    st_check(br_state() == PLUTO_STATE_ERROR, "J.error_state");
    check_str("J.error_title", br_page_title(), "Connection Error");

    Route r2[] = { { "/", PAGE_A, 0 } };
    set_routes(r2, 1);
    push_input(BR_BTN_A, BR_BTN_A, 0);         /* retry (default row) */
    frame();
    pump_until_page(30);
    st_check(br_state() == PLUTO_STATE_PAGE, "J.retry_recovers");
}

static void t_forms(void) {
    reset_all();
    br_boot();
    Route rf[] = { { "/form", FORM_PAGE, 0 },
                   { "/fb", FORM_FALLBACK_PAGE, 0 },
                   { "/submit?h=1&q2=v%202&c1=on&r=ra&sel=s2&btn=go", PAGE_A, 0 },
                   { "/other?f1=a&s=v", PAGE_A, 0 },
                   { "/", PAGE_A, 0 } };
    set_routes(rf, 5);

    br_navigate_to("forms.test/form");
    pump_until_page(30);
    if (br_state() != PLUTO_STATE_PAGE || layout_item_count() == 0) {
        PLUTO_ERROR("[P32] forms page not loaded (st=%d items=%d tgt=%s)",
                    br_state(), layout_item_count(), g_lastTarget);
        st_check(0, "K.forms_page_loaded");
        return;
    }
    st_check(1, "K.forms_page_loaded");

    /* checkbox toggle */
    LItem* c2 = find_item(LIT_CHECKBOX_FIELD, 1);
    st_check(c2 != NULL && c2->checked == 0, "K.checkbox_initial");
    br_activate_item(c2);
    st_check(c2->checked == 1, "K.checkbox_toggled");

    /* radio exclusivity within the same name */
    LItem* ra_ = find_item(LIT_CHECKBOX_FIELD, 2);
    LItem* rb_ = find_item(LIT_CHECKBOX_FIELD, 3);
    if (ra_ == NULL || rb_ == NULL) { st_check(0, "K.radios_found"); return; }
    st_check(ra_ != NULL && ra_->radio && ra_->checked == 1,
             "K.radio_a_checked");
    br_activate_item(rb_);
    st_check(rb_->checked == 1 && ra_->checked == 0, "K.radio_exclusive");

    /* select cycle wraps to the first option */
    LItem* sel = find_item(LIT_SELECT_FIELD, 0);
    st_check(sel != NULL && sel->selectedIndex == 2, "K.select_initial");
    br_activate_item(sel);
    st_check(sel->selectedIndex == 1, "K.select_wrapped");

    /* keyboard finalize truncates to maxlength */
    LItem* txt = find_item(LIT_INPUT_FIELD, 0);
    if (txt == NULL) { st_check(0, "L.input_found"); return; }
    txt->maxlength = 4;
    br_activate_item(txt);
    st_check(br_keyboard_open() == 1, "L.kb_opens");
    br_keyboard_finalize("abcdefgh");
    st_check(strcmp(txt->value, "abcd") == 0, "L.maxlength_truncated");
    st_check(br_keyboard_open() == 0, "L.kb_closed_after_hide");

    /* submit: hidden + text + checked boxes + radios + select + button
     * (state after the toggles above) */
    LItem* sub = find_item(LIT_INPUT_SUBMIT, 0);
    if (sub == NULL) { st_check(0, "M.submit_found"); return; }
    st_check(1, "M.submit_found");
    br_activate_item(sub);
    pump(1);                                    /* pendingNav consumed */
    pump_until_page(30);
    check_str("M.submit_query_assembly", g_lastTarget,
              "/submit?h=1&q2=abcd&c1=on&c2=on&r=rb&sel=s1&btn=go");

    /* fallback path collects all named visible inputs when actions
     * don't match the clicked submit's formAction */
    br_navigate_to("forms.test/fb");
    pump_until_page(30);
    LItem* sub2 = find_item(LIT_INPUT_SUBMIT, 0);
    if (sub2 == NULL) { st_check(0, "N.fallback_submit_found"); return; }
    br_activate_item(sub2);
    pump(1);
    pump_until_page(30);
    st_check(strcmp(g_lastTarget, "/other?f1=a&s=v") == 0,
             "N.fallback_collects_named_inputs");
}

static void t_meta_refresh(void) {
    reset_all();
    br_boot();
    Route rm[] = { { "/meta", META_PAGE, 0 },
                   { "/next", NEXT_PAGE, 0 },
                   { "/", PAGE_A, 0 } };
    set_routes(rm, 3);

    br_navigate_to("meta.test/meta");
    pump_settle_once(30);
    st_check(br_state() == PLUTO_STATE_PAGE, "O.meta_first_page");
    frame();                 /* deadline check stages the /next navigation */
    pump_until_page(30);     /* now settle means /next has landed */
    st_check(strstr(g_lastTarget, "/next") != NULL, "O.redirect_requested");
    check_str("O.redirect_landed", br_page_title(), "Next Page");
}

static void t_crank_physics(void) {
    reset_all();
    br_boot();
    static char tall[4096];
    build_tall(tall, sizeof(tall));
    Route rt[] = { { "/", tall, 0 } };
    set_routes(rt, 1);

    br_navigate_to("tall.test/");
    pump_until_page(30);
    double maxScroll = layout_total_height() -
                       (double)PLUTO_CONTENT_HEIGHT;
    st_check(maxScroll > 200, "P.tall_page_scrollable");

    /* crank spins up velocity which decays toward zero */
    for (int i = 0; i < 6; i++) push_input(0, 0, 60.0f);
    pump(6);
    double afterCrank = br_target_scroll_y();
    st_check(afterCrank > 100.0, "P.crank_scrolls_down");

    /* velocity decays: idle frames stop growing the target */
    pump(6);
    double settled = br_target_scroll_y();
    pump(6);
    st_check(br_target_scroll_y() == settled, "P.velocity_decays_out");

    /* scroll lerps toward target */
    st_check(br_target_scroll_y() >= afterCrank, "P.target_advanced");
    st_check(br_scroll_y() <= br_target_scroll_y() + 0.001,
             "P.lerp_below_target");

    /* clamp at bottom */
    for (int i = 0; i < 60; i++) { push_input(0, 0, 120.0f); }
    pump(70);
    st_check(br_target_scroll_y() <= maxScroll + 0.5, "P.clamp_max");
    st_check(br_target_scroll_y() >= maxScroll - 1.0, "P.reached_bottom");

    /* Up walks selection backwards without crashing */
    push_input(BR_BTN_UP, BR_BTN_UP, 0);
    frame();
    st_check(br_state() == PLUTO_STATE_PAGE, "P.up_no_crash");
}

static void t_details_toggle(void) {
    reset_all();
    br_boot();
    /* RAW_HTML mode: readability strips <details> in reader mode, so the
     * toggle rect only exists on the full document. Selection uses the
     * same lm_select_next() the reader d-pad handler calls. */
    Route rd[] = { { "/", DETAILS_PAGE, 0, NULL } };
    set_routes(rd, 1);

    br_navigate_to("det.test/");
    pump_until_page(30);
    LmLink* lk = lm_select_next((int)br_scroll_y());
    st_check(lk != NULL && lk->primaryRect.isToggle == 1,
             "Q.toggle_rect_selected");
    push_input(BR_BTN_A, BR_BTN_A, 0);         /* A toggles it */
    frame();
    pump(12);                                  /* cooperative reparse */
    st_check(br_state() == PLUTO_STATE_PAGE, "Q.still_page_after_toggle");
    st_check(br_is_rendering() == 0, "Q.rendering_finished");
}

static void t_pause_menu(void) {
    reset_all();
    br_boot();
    Route rh[] = { { "/", PAGE_A, 0 } };
    set_routes(rh, 1);

    /* rows on HOME: Home / Settings / History / Clear Cookies */
    br_on_pause();
    st_check(br_menu_open() == 1, "R.menu_opens");
    push_input(0, 0, 0); frame();              /* menu swallows frame */
    push_input(BR_BTN_DOWN, BR_BTN_DOWN, 0); frame();  /* Settings */
    push_input(BR_BTN_DOWN, BR_BTN_DOWN, 0); frame();  /* History */
    push_input(BR_BTN_A, BR_BTN_A, 0); frame();        /* activate */
    st_check(br_menu_open() == 0, "R.menu_closes_on_action");
    st_check(br_state() == PLUTO_STATE_HISTORY, "R.history_entry");

    br_on_resume();
    st_check(br_menu_open() == 0, "R.resume_closes");

    /* Home-Page row navigates to about:home */
    br_on_pause();
    push_input(BR_BTN_A, BR_BTN_A, 0); frame();        /* first row */
     pump(2);
     st_check(br_state() == PLUTO_STATE_HOME, "R.home_row_navigates");
 }

/* S: HTML-mode virtual mouse cursor (Lua 754-834, 1002-1021) */
static void t_html_cursor(void) {
    reset_all();
    br_boot();                     /* RAW_HTML is the default mode */
    Route rs[] = { { "/b.html", PAGE_B, 0 },
                   { "/", PAGE_A, 0 } };   /* exact "/" last: prefix rule */
    set_routes(rs, 2);

    br_navigate_to("cur.test/");
    pump_until_page(30);
    br_set_mouse_for_tests(200, 120);

    /* D-pad movement at 4 px/frame */
    for (int i = 0; i < 5; i++) {
        push_input(BR_BTN_RIGHT, BR_BTN_RIGHT, 0); frame();
    }
    st_check(br_mouse_x() == 220, "S.cursor_moves");

    /* screen-edge clamps */
    for (int i = 0; i < 60; i++) {
        push_input(BR_BTN_RIGHT, BR_BTN_RIGHT, 0); frame();
    }
    st_check(br_mouse_x() == PLUTO_SCREEN_WIDTH - 2, "S.clamp_right");
    for (int i = 0; i < 40; i++) {
        push_input(BR_BTN_DOWN, BR_BTN_DOWN, 0); frame();
    }
    st_check(br_mouse_y() == PLUTO_SCREEN_HEIGHT - 2, "S.clamp_bottom");
    for (int i = 0; i < 80; i++) {
        push_input(BR_BTN_UP, BR_BTN_UP, 0); frame();
    }
    st_check(br_mouse_y() == PLUTO_CONTENT_Y + 2, "S.clamp_top");

    /* A + Left = history back */
    br_set_mouse_for_tests(200, 120);
    br_navigate_to("cur.test/b.html");
    pump_until_page(30);
    push_input(BR_BTN_A | BR_BTN_LEFT, BR_BTN_A | BR_BTN_LEFT, 0); frame();
    pump_until_page(30);
    check_str("S.a_left_back", br_page_title(), "Page A");
}

 /* ── entry point ──────────────────────────────────────────────────────── */

void selftest_browser_run(struct PlaydateAPI* pd, int* pass, int* fail) {
    st_pass = 0; st_fail = 0;

    PLUTO_LOG("[P32] ---- browser selftest begin ----");

    br_init(pd);                   /* real graphics; HW keyboard gated off */
    br_set_keyboard_enabled(0);
    br_set_clock_fn(browser_fake_clock);
    br_set_input_source(provider, NULL);
    hc_set_tcp_for_tests(&FAKE_TCP);
    hc_set_clock_fn(browser_fake_clock);

    t_navigation_flow();
    t_history_stack();
    t_loading_and_error();
    t_forms();
    t_meta_refresh();
    t_crank_physics();
    t_details_toggle();
    t_pause_menu();
    t_html_cursor();

    hc_set_clock_fn(NULL);         /* restore real clock */
    hc_set_tcp_for_tests(NULL);    /* restore real networking */
    br_set_input_source(NULL, NULL);   /* restore hardware input */
    br_set_clock_fn(NULL);             /* restore engine clock */
    br_reset_for_tests();

    PLUTO_LOG("[P32] ---- browser selftest end: %d pass / %d fail ----",
              st_pass, st_fail);

    if (pass) *pass += st_pass;
    if (fail) *fail += st_fail;
}
