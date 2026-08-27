// selftest_http.c — P07 verification suite.
//
// Every expectation mirrors observable behavior of Source/core/http_client.lua
// captured via the host-Lua oracle (p07_oracle.lua -> p07_truth.txt): exact
// request bytes, slice-to-end Content-Length quirk, chunked decoding rules,
// one-tick deferred redirects, the too-many-redirects SILENT DROP (source bug
// kept faithfully), 60 s watchdog with the >512-byte partial-data escape,
// closed-connection messages, stale-open self-close after cancel, local
// about: pages, progress semantics (0,0 before headers), access gating.
//
// Runs fully offline: a fake playdate_tcp vtable + fake millisecond clock are
// injected via hc_set_tcp_for_tests()/hc_set_clock_fn().
#include <stdio.h>
#include <string.h>

#include "pd_api.h"
#include "../core/cookie_jar.h"
#include "../core/http_client.h"
#include "../core/logger.h"
#include "../util/mem.h"
#include "../util/strbuf.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P07] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P07] FAIL %s", name);
    }
}

static void check_str(const char* name, const char* got, const char* exp)
{
    st_check(got != NULL && strcmp(got, exp) == 0, name);
}

// Zeroed callback set: delivery becomes a no-op.
static const PlutoHttpCallbacks NC;

// ------------------------------------------------------- fake TCP vtable ----

#define MAX_SOCKS 8

typedef struct FakeSock {
    char server[160];
    int port;
    int usessl;
    unsigned id; // what setUserdata stored (generation id)
    TCPOpenCallback* openCb;
    void* openUd;
    TCPConnectionCallback* closedCb;
    PDNetErr openErr; // err passed to a failed fire_open()
    int failWrite;    // when nonzero, write() returns this error
    StrBuf queue;     // server -> client bytes waiting in read()
    StrBuf written;   // captured client -> server request bytes
    int closedCount;
} FakeSock;

static FakeSock SOCKS[MAX_SOCKS];
static int NSOCKS;

static unsigned CLOCK_NOW = 1000000;
static enum accessReply ACCESS_REPLY = kAccessAllow;

static unsigned fake_clock(void) { return CLOCK_NOW; }
static AccessRequestCallback* ACCESS_CB_PENDING;
static char RA_SERVER[160];
static int RA_PORT;
static int RA_SSL;
static char RA_PURPOSE[64];
static int RA_CALLS;
static int CT_MS = -1;
static int RT_MS = -1;
static int BS_BYTES = -1;

static FakeSock* as_sock(TCPConnection* c) { return (FakeSock*)c; }

static enum accessReply f_requestAccess(const char* server, int port,
                                        bool usessl, const char* purpose,
                                        AccessRequestCallback* cb, void* ud)
{
    (void)ud;
    RA_CALLS++;
    snprintf(RA_SERVER, sizeof(RA_SERVER), "%s", server ? server : "");
    RA_PORT = port;
    RA_SSL = usessl ? 1 : 0;
    snprintf(RA_PURPOSE, sizeof(RA_PURPOSE), "%s", purpose ? purpose : "");
    ACCESS_CB_PENDING = cb;
    return ACCESS_REPLY;
}

static TCPConnection* f_newConnection(const char* server, int port, bool ssl)
{
    if (NSOCKS >= MAX_SOCKS) return NULL;
    FakeSock* s = &SOCKS[NSOCKS++];
    memset(s, 0, sizeof(*s));
    snprintf(s->server, sizeof(s->server), "%s", server ? server : "");
    s->port = port;
    s->usessl = ssl ? 1 : 0;
    sb_init(&s->queue);
    sb_init(&s->written);
    return (TCPConnection*)s;
}

static TCPConnection* f_retain(TCPConnection* c) { return c; }
static void f_release(TCPConnection* c) { (void)c; }
static PDNetErr f_getError(TCPConnection* c) { (void)c; return NET_OK; }

static void f_setConnectTimeout(TCPConnection* c, int ms)
{ (void)c; CT_MS = ms; }
static void f_setReadTimeout(TCPConnection* c, int ms)
{ (void)c; RT_MS = ms; }
static void f_setReadBufferSize(TCPConnection* c, int bytes)
{ (void)c; BS_BYTES = bytes; }

static void f_setUserdata(TCPConnection* c, void* ud)
{ as_sock(c)->id = (unsigned)(uintptr_t)ud; }
static void* f_getUserdata(TCPConnection* c)
{ return (void*)(uintptr_t)as_sock(c)->id; }

static PDNetErr f_open(TCPConnection* c, TCPOpenCallback cb, void* ud)
{
    as_sock(c)->openCb = cb;
    as_sock(c)->openUd = ud;
    return NET_OK;
}

static PDNetErr f_close(TCPConnection* c)
{
    as_sock(c)->closedCount++;
    return NET_OK;
}

static void f_setClosedCb(TCPConnection* c, TCPConnectionCallback* cb)
{ as_sock(c)->closedCb = cb; }

static size_t f_getBytesAvailable(TCPConnection* c)
{ return as_sock(c)->queue.len; }

static int f_read(TCPConnection* c, void* buffer, size_t length)
{
    FakeSock* s = as_sock(c);
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
    FakeSock* s = as_sock(c);
    if (s->failWrite) return s->failWrite;
    sb_append(&s->written, buffer, length);
    return (int)length;
}

static size_t f_getSentPending(TCPConnection* c) { (void)c; return 0; }

static struct playdate_tcp FAKE_TCP = {
    f_requestAccess,
    f_newConnection,
    f_retain,
    f_release,
    f_getError,
    f_setConnectTimeout,
    f_setUserdata,
    f_getUserdata,
    f_open,
    f_close,
    f_setClosedCb,
    f_setReadTimeout,
    f_setReadBufferSize,
    f_getBytesAvailable,
    f_read,
    f_write,
    f_getSentPending,
};

// ------------------------------------------------------------ callbacks ----

#define REC_URL_MAX 2048
typedef struct Rec {
    int progN;
    int progCur[8];
    int progTot[8];
    int successN;
    int status;
    char ct[64];
    char loc[128];
    char bodyHead[96];
    size_t bodyLen;
    char url[REC_URL_MAX];
    int errorN;
    char err[256];
} Rec;

static Rec R;

static void rec_reset(void) { memset(&R, 0, sizeof(R)); }

static void on_prog(void* ud, int cur, int tot)
{
    Rec* r = (Rec*)ud;
    if (r->progN < 8) {
        r->progCur[r->progN] = cur;
        r->progTot[r->progN] = tot;
    }
    r->progN++;
}

static void on_succ(void* ud, int status, const StrMap* headers,
                    const char* body, size_t bodyLen, const char* finalUrl)
{
    Rec* r = (Rec*)ud;
    r->successN++;
    r->status = status;
    r->bodyLen = bodyLen;
    snprintf(r->bodyHead, sizeof(r->bodyHead), "%.*s",
             (int)(bodyLen < sizeof(r->bodyHead) - 1 ? bodyLen
                                                     : sizeof(r->bodyHead) - 1),
             body ? body : "");
    snprintf(r->url, sizeof(r->url), "%s", finalUrl ? finalUrl : "");
    const char* v = headers ? (const char*)sm_get((StrMap*)headers,
                                                  "content-type") : NULL;
    snprintf(r->ct, sizeof(r->ct), "%s", v ? v : "");
    v = headers ? (const char*)sm_get((StrMap*)headers, "location") : NULL;
    snprintf(r->loc, sizeof(r->loc), "%s", v ? v : "");
    // set-cookie must never leak into the delivered map
    if (headers && sm_has((StrMap*)headers, "set-cookie"))
        snprintf(r->ct + strlen(r->ct),
                 sizeof(r->ct) - strlen(r->ct), "!SC!");
}

static void on_err(void* ud, const char* msg)
{
    Rec* r = (Rec*)ud;
    r->errorN++;
    snprintf(r->err, sizeof(r->err), "%s", msg ? msg : "");
}

static PlutoHttpCallbacks make_cbs(void)
{
    PlutoHttpCallbacks c;
    c.ud = &R;
    c.onProgress = on_prog;
    c.onSuccess = on_succ;
    c.onError = on_err;
    return c;
}

// -------------------------------------------------------------- helpers ----

static void reset_fixture(void)
{
    for (int i = 0; i < NSOCKS; i++) {
        sb_free(&SOCKS[i].queue);
        sb_free(&SOCKS[i].written);
    }
    NSOCKS = 0;
    ACCESS_REPLY = kAccessAllow;
    ACCESS_CB_PENDING = NULL;
    RA_CALLS = 0;
    CLOCK_NOW += 1000;
    cj_clear();
    hc_cancel();
    rec_reset();
}

static FakeSock* last_sock(void) { return &SOCKS[NSOCKS - 1]; }

static void feed(FakeSock* s, const char* bytes)
{
    sb_append_str(&s->queue, bytes);
}

static void fire_open(FakeSock* s, int ok)
{
    s->openCb((TCPConnection*)s, ok ? NET_OK : s->openErr, s->openUd);
}

static void server_close(FakeSock* s)
{
    s->closedCb((TCPConnection*)s, NET_CONNECTION_CLOSED);
}

static void run(int frames)
{
    for (int i = 0; i < frames; i++) hc_update();
}

// ---------------------------------------------------------------- tests ----

// A. buildRequest: byte-exact format, no cookies.
static void test_build_request_plain(void)
{
    reset_fixture();
    int ret = hc_get("http://example.com/x?a=1", &NC);
    FakeSock* s = last_sock();
    st_check(ret == 1, "A.ret");
    st_check(hc_is_loading() == 1, "A.loading_after_get");
    st_check(strcmp(s->server, "example.com") == 0 && s->port == 80 &&
                 s->usessl == 0,
             "A.conn_args");
    st_check(CT_MS == 10000 && RT_MS == 10000 && BS_BYTES == 16384,
             "A.timeouts_ms_and_bufsize");
    fire_open(s, 1);
    run(1); // write happens on the update AFTER open
    static const char REQ[] =
        "GET /x?a=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: CometBrowser/1.0 (Playdate)\r\n"
        "Accept: text/html,text/plain;q=0.8\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Connection: close\r\n"
        "\r\n";
    st_check(s->written.len == sizeof(REQ) - 1 &&
                 memcmp(s->written.data, REQ, sizeof(REQ) - 1) == 0,
             "A.request_bytes_exact");
    // Complete it so isLoading returns to false.
    feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    run(3);
    st_check(hc_is_loading() == 0, "A.loading_after_success");
}

// A2/A3: cookie header injection, custom port, default-port suppression.
static void test_build_request_cookie_port(void)
{
    reset_fixture();
    cj_store("secure.example", "sid=abc");
    cj_store("secure.example", "t=1");
    hc_get("https://secure.example:8443/p", &NC);
    FakeSock* s = last_sock();
    st_check(s->port == 8443 && s->usessl == 1, "A2.conn_ssl_8443");
    fire_open(s, 1);
    run(1);
    static const char REQ[] =
        "GET /p HTTP/1.1\r\n"
        "Host: secure.example:8443\r\n"
        "User-Agent: CometBrowser/1.0 (Playdate)\r\n"
        "Accept: text/html,text/plain;q=0.8\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cookie: sid=abc; t=1\r\n"
        "Connection: close\r\n"
        "\r\n";
    st_check(s->written.len == sizeof(REQ) - 1 &&
                 memcmp(s->written.data, REQ, sizeof(REQ) - 1) == 0,
             "A2.cookie_request_bytes");

    reset_fixture();
    hc_get("http://plain.example/", &NC);
    s = last_sock();
    fire_open(s, 1);
    run(1);
    st_check(strstr(s->written.data, "Host: plain.example\r\n") != NULL,
             "A3.default_port_suppressed");
}

// B/C: status parse, lowercased keys, Set-Cookie capture into real jar.
static void test_status_headers_cookies(void)
{
    reset_fixture();
    hc_get("http://h/b",
           &NC); // no callbacks: delivery is a no-op
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 404 Not Found\r\nContent-Type: Text/Plain\r\n"
            "Content-Length: 5\r\nSet-Cookie: sid=abc\r\n"
            "Set-Cookie: t=1\r\n\r\nhello");
    run(3);
    st_check(cj_count() == 2, "C.jar_count_2");
    // Round-trip: next request to same host carries both cookies.
    hc_get("http://h/x2", &NC);
    FakeSock* s2 = last_sock();
    fire_open(s2, 1);
    run(1);
    st_check(strstr(s2->written.data, "Cookie: sid=abc; t=1\r\n") != NULL,
             "C.cookie_header_roundtrip");
}

// D. Content-Length completion + slice-to-end quirk (16 delivered > CL 5).
static void test_slice_to_end_quirk(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    int ret = hc_get("http://origin.example/d", &cbs);
    FakeSock* s = last_sock();
    st_check(ret == 1, "D.ret");
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloWORLD_EXTRA");
    run(3);
    st_check(R.successN == 1 && R.status == 200, "D.status");
    st_check(R.bodyLen == 16, "D.bodylen_beyond_cl");
    check_str("D.slice_to_end", R.bodyHead, "helloWORLD_EXTRA");
    st_check(strcmp(R.url, "http://origin.example/d") == 0, "D.url");
}

// E. chunked decoding: case-insensitive TE, extensions, trailers, split feed.
static void test_chunked(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://h/e", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChunkEd\r\n\r\n5\r\nhello\r");
    run(2);
    st_check(R.successN == 0, "E.no_success_midstream");
    feed(s, "\n3;x=y\r\nabc\r\n0\r\nTrailer: t\r\n\r\n");
    run(3);
    st_check(R.successN == 1 && R.status == 200, "E.status");
    check_str("E.decoded_body", R.bodyHead, "helloabc");
}

// F. redirect deferral: hop opens NEXT tick; relative Location resolved.
static void test_redirect_deferral(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://origin.example/dir/page", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 301 Moved Permanently\r\nLocation: b?q=2\r\n"
            "Content-Length: 0\r\n\r\n");
    run(1); // parses 301 -> defer + close
    st_check(NSOCKS == 1 && s->closedCount == 1, "F.first_hop_closed");
    run(1); // deferred doGet opens hop2 HERE
    FakeSock* s2 = last_sock();
    st_check(NSOCKS == 2 && strcmp(s2->server, "origin.example") == 0 &&
                 s2->port == 80 && s2->usessl == 0,
             "F.hop2_conn");
    fire_open(s2, 1);
    run(1); // writes hop2 request
    st_check(strncmp(s2->written.data, "GET /dir/b?q=2 HTTP/1.1\r\n", 25)
                 == 0,
             "F.relative_resolution");
    feed(s2, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nBBBB");
    run(3);
    st_check(R.successN == 1 && R.status == 200, "F.final_status");
    st_check(strcmp(R.url, "http://origin.example/dir/b?q=2") == 0,
             "F.final_url");
    st_check(NSOCKS == 2, "F.sock_count");
}

// G. too many redirects: faithful silent drop (error branch wiped by reset).
static void test_too_many_redirects_silent(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://loop.example/start", &cbs);
    for (int i = 1; i <= 10; i++) {
        fire_open(last_sock(), 1);
        run(1);
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 302 Found\r\nLocation: /next%d\r\n"
                 "Content-Length: 0\r\n\r\n",
                 i);
        feed(last_sock(), resp);
        run(2);
        if (!hc_is_loading() && (R.errorN > 0 || R.successN > 0)) break;
    }
    st_check(NSOCKS == 6, "G.six_sockets");
    st_check(R.errorN == 0 && R.successN == 0, "G.silent_drop_no_callbacks");
    st_check(hc_is_loading() == 0, "G.not_loading_after");
}

// H/I. timeout watchdog: partial >512 completes; small buffer errors.
static void test_timeout_watchdog(void)
{
    reset_fixture();
    CLOCK_NOW = 500000;
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://slow.example/big", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    StrBuf big;
    sb_init(&big);
    sb_append_str(&big, "HTTP/1.1 200 OK\r\nContent-Length: 99999\r\n\r\n");
    for (int i = 0; i < 600; i++) sb_append_char(&big, 'x');
    sb_append(&s->queue, big.data, big.len);
    sb_free(&big);
    run(1); // pump + parse while clock fresh
    CLOCK_NOW += 60001;
    run(1);
    st_check(R.successN == 1 && R.errorN == 0, "H.partial_over_512_done");
    st_check(R.bodyLen == 600, "H.bodylen_600");

    reset_fixture();
    CLOCK_NOW = 500000;
    hc_get("http://slow.example/stall", &cbs);
    s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HT");
    run(1); // pump the fragment
    CLOCK_NOW += 60001;
    run(1);
    st_check(R.successN == 0 && R.errorN == 1, "I.timeout_error");
    check_str("I.timeout_message", R.err,
              "Connection timed out after 60 seconds.");
}

// J/P. connection-close handling.
static void test_closed_connection(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://h/j", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    server_close(s);
    run(1);
    st_check(R.errorN == 1 && R.successN == 0, "J.closed_no_data_error");
    check_str("J.closed_message", R.err,
              "Connection closed before any data was received.");

    reset_fixture();
    hc_get("http://h/p", &cbs);
    s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial!");
    run(1);
    server_close(s);
    run(1);
    st_check(R.successN == 1 && R.errorN == 0, "P.closed_midbody_done");
    check_str("P.partial_body", R.bodyHead, "partial!");
}

// K. stale open callback after cancel: socket self-closes, zero callbacks.
static void test_stale_open_after_cancel(void)
{
    reset_fixture();
    hc_get("http://k/k", &NC);
    FakeSock* s = last_sock();
    hc_cancel();
    fire_open(s, 1); // stale now: must be closed by the callback itself
    st_check(s->closedCount == 1, "K.stale_socket_closed");
    st_check(R.errorN == 0 && R.successN == 0, "K.silent");
    st_check(hc_is_loading() == 0, "K.idle");
}

// L. internal about: pages incl. 20 ms timer and unknown-page error.
static void test_about_pages(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    int ret = hc_get("about:home", &cbs);
    st_check(ret == 1 && hc_is_loading() == 1, "L.get_about_loading");
    run(1); // clock not advanced yet: still pending
    st_check(R.successN == 0 && hc_is_loading() == 1, "L.still_pending");
    CLOCK_NOW += 20;
    run(1);
    st_check(R.progN == 1 && R.progCur[0] == 100 && R.progTot[0] == 100,
             "L.progress_100");
    st_check(R.successN == 1 && R.status == 200, "L.home_status");
    check_str("L.home_ct", R.ct, "text/html");
    st_check(R.bodyLen == 100, "L.home_len_100");
    check_str("L.home_url", R.url, "about:home");
    st_check(hc_is_loading() == 0, "L.done_not_loading");

    reset_fixture();
    ret = hc_get("about:nope", &cbs);
    st_check(ret == 1, "L.unknown_ret_true");
    st_check(R.errorN == 1, "L.unknown_error");
    check_str("L.unknown_message", R.err, "Unknown internal page: about:nope");
    st_check(hc_is_loading() == 0, "L.unknown_idle");

    reset_fixture();
    hc_get("about:blank", &cbs);
    CLOCK_NOW += 20;
    run(1);
    st_check(R.bodyLen == 26, "L.blank_len_26");

    reset_fixture();
    hc_get("about:acidtest", &cbs);
    CLOCK_NOW += 20;
    run(1);
    st_check(R.bodyLen == 2984, "L.acid_len_2984");
    st_check(strncmp(R.bodyHead, "<html><head><title>HTML Renderer Test Suite</title>", 51) == 0,
             "L.acid_title_present");
}

// M. invalid URL (empty host): synchronous rejection, ret 0.
static void test_invalid_url(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    int ret = hc_get("http://", &cbs);
    st_check(ret == 0, "M.ret_false");
    st_check(R.errorN == 1, "M.sync_error");
    check_str("M.message", R.err, "Invalid URL (no hostname): http://");
    st_check(hc_is_loading() == 0, "M.idle");
}

// N. send failure surfaces an error and idles.
static void test_send_failure(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://n/n", &cbs);
    FakeSock* s = last_sock();
    s->failWrite = NET_WRITE_ERROR;
    fire_open(s, 1);
    run(1);
    st_check(R.errorN == 1, "N.send_error");
    st_check(strncmp(R.err, "Send failed: ", 13) == 0, "N.send_failed_prefix");
    st_check(hc_is_loading() == 0, "N.idle");
}

// O. progress semantics: (0,0) until headers parsed, then clamped body bytes.
static void test_progress_semantics(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://o/o", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1); // writes
    feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n");
    run(1); // pump: headers only -> (0,0)
    feed(s, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"); // 40
    run(1);
    feed(s,
         "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"); // 60
    run(1); // completes at exactly CL
    st_check(R.progN == 3, "O.three_progress_calls");
    st_check(R.progCur[0] == 0 && R.progTot[0] == 0, "O.first_0_0");
    st_check(R.progCur[1] == 40 && R.progTot[1] == 100, "O.second_body_bytes");
    st_check(R.progCur[2] == 100 && R.progTot[2] == 100, "O.clamped_final");
    st_check(R.successN == 1 && R.bodyLen == 100, "O.delivered");
}

// Q. 304 without Location falls through as a normal response.
static void test_status_304_fallthrough(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://q/q", &cbs);
    FakeSock* s = last_sock();
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 304 Not Modified\r\nETag: \"x\"\r\n\r\n");
    run(1);
    server_close(s); // no CL/close would stall forever (parity behavior)
    run(1);
    st_check(R.successN == 1 && R.status == 304 && R.errorN == 0,
             "Q.delivered_as_normal");
    st_check(R.bodyLen == 0, "Q.empty_body");
}

// R/S/T/U. network access gating (C-only behavior).
static void test_access_gating(void)
{
    // Deny is synchronous.
    reset_fixture();
    ACCESS_REPLY = kAccessDeny;
    PlutoHttpCallbacks cbs = make_cbs();
    int ret = hc_get("http://denied.example/x", &cbs);
    st_check(ret == 0, "R.deny_ret_false");
    st_check(R.errorN == 1, "R.deny_error");
    check_str("R.deny_message", R.err, "Networking not available.");
    st_check(strcmp(RA_PURPOSE, "CometBrowser Web Browsing") == 0,
            "R.purpose_string");

    // Ask defers until the callback grants; grant is cached per host.
    reset_fixture();
    ACCESS_REPLY = kAccessAsk;
    ret = hc_get("http://ask.example/a", &cbs);
    st_check(ret == 1, "S.ask_ret_true");
    st_check(hc_is_loading() == 1 && NSOCKS == 0, "S.waiting_no_socket_yet");
    st_check(RA_CALLS == 1, "S.one_access_call");
    st_check(ACCESS_CB_PENDING != NULL, "S.callback_recorded");
    FakeSock* s = NULL;
    if (ACCESS_CB_PENDING) ACCESS_CB_PENDING(true, NULL);
    st_check(NSOCKS == 1, "S.socket_after_grant");
    if (NSOCKS > 0) {
        s = last_sock();
        fire_open(s, 1);
        run(1);
        feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        run(3);
    }
    st_check(R.successN == 1 && R.status == 200, "S.completed_after_grant");

    // Same host again: no new access call (session grant cache).
    reset_fixture();
    hc_get("http://ask.example/b", &NC);
    s = last_sock();
    st_check(RA_CALLS == 0, "S.grant_cached_same_host");
    fire_open(s, 1);
    run(1);
    feed(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    run(3);

    // Async denial via callback.
    reset_fixture();
    ACCESS_REPLY = kAccessAsk;
    hc_get("http://other.example/c", &cbs);
    st_check(NSOCKS == 0 && hc_is_loading() == 1, "T.waiting");
    if (ACCESS_CB_PENDING) ACCESS_CB_PENDING(false, NULL);
    st_check(R.errorN == 1, "T.async_deny_error");
    check_str("T.async_deny_message", R.err, "Networking not available.");
    st_check(hc_is_loading() == 0, "T.idle_after_deny");
}

// Empty-string input quirk: parses to host "blank" over https (Lua parity).
static void test_empty_string_quirk(void)
{
    reset_fixture();
    hc_get("", &NC);
    FakeSock* s = last_sock();
    st_check(strcmp(s->server, "blank") == 0 && s->port == 0 && s->usessl == 1,
             "V.empty_parses_to_blank_host");
    fire_open(s, 1);
    run(1);
    st_check(strstr(s->written.data, "Host: blank:0\r\n") != NULL,
             "V.host_blank_port0");
}

// W. open failure: error delivered on the next update tick.
static void test_open_failure(void)
{
    reset_fixture();
    PlutoHttpCallbacks cbs = make_cbs();
    hc_get("http://w/w", &cbs);
    FakeSock* s = last_sock();
    s->openErr = NET_NO_DEVICE;
    fire_open(s, 0); // open callback resolves with a failure (state=error now)
    st_check(R.errorN == 0 && hc_is_loading() == 0,
             "W.error_state_before_tick");
    run(1); // error branch delivers
    st_check(R.errorN == 1 && R.successN == 0, "W.open_failed_delivered");
    check_str("W.open_failed_message", R.err,
              "Connection failed: NET_NO_DEVICE");
    st_check(hc_is_loading() == 0, "W.idle");
}

void selftest_http_run(int* outPass, int* outFail)
{
    st_pass = 0;
    st_fail = 0;

    hc_set_http_for_tests(NULL); // force TCP backend for selftests
    hc_set_tcp_for_tests(&FAKE_TCP);
    hc_set_clock_fn(fake_clock);

    test_build_request_plain();
    test_build_request_cookie_port();
    test_status_headers_cookies();
    test_slice_to_end_quirk();
    test_chunked();
    test_redirect_deferral();
    test_too_many_redirects_silent();
    test_timeout_watchdog();
    test_closed_connection();
    test_stale_open_after_cancel();
    test_about_pages();
    test_invalid_url();
    test_send_failure();
    test_progress_semantics();
    test_status_304_fallthrough();
    test_access_gating();
    test_open_failure();
    test_empty_string_quirk();

    hc_cancel();
    hc_set_clock_fn(NULL);
    hc_set_tcp_for_tests(NULL); // restore real networking for benchmarks
    hc_restore_http_api(); // restore real HTTP API for benchmarks

    PLUTO_LOG("[P07] http selftests done: %d passed, %d failed", st_pass,
              st_fail);
    *outPass = st_pass;
    *outFail = st_fail;
}
