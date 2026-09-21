/*
 * SW2c gzip Content-Encoding host test (macOS only, NOT part of the Playdate
 * build). Exercises the http_client gzip delivery end-to-end over the
 * SDK-network seam (fake playdate_tcp):
 *   1  gzip HTML body (Content-Length = compressed size) — strict ISIZE path
 *   2  gzip + chunked (completion via conn-close; chunk decode, then gunzip)
 *   3  gzip with FNAME/FEXTRA/FCOMMENT/FHCRC header fields (fetch-style)
 *   4  empty gzip member (ISIZE = 0)
 *   5  identity regression — non-gzip body untouched (SW2b paths intact)
 *   6  corrupt gzip member — clean onError, no onSuccess, no crash
 *   7  truncated gzip — clean onError
 *   8  gzip member lying about ISIZE — refused (strict accounting)
 *
 * Fixtures: raw deflate payloads produced by Python zlib (deflate raw) at
 * build time are injected below as string literals (see /tmp/build_gz.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "pd_api.h"
#include "core/http_client.h"

/* ── Fake PD network/tcp ─────────────────────────────────────────────────── */
typedef struct FakeConn
{
    const char *resp;
    size_t len;
    size_t sent;
    TCPConnectionCallback *closedCb;
    int closeFired;
} FakeConn;

static FakeConn g_conn;

static TCPConnection *f_newConnection(const char *server, int port, bool usessl)
{
    (void)server; (void)port; (void)usessl;
    return (TCPConnection *)&g_conn;
}
static TCPConnection *f_retain(TCPConnection *c) { return c; }
static void f_release(TCPConnection *c) { (void)c; }
static PDNetErr f_getError(TCPConnection *c) { (void)c; return NET_OK; }
static void f_setConnectTimeout(TCPConnection *c, int ms) { (void)c; (void)ms; }
static void f_setUserdata(TCPConnection *c, void *ud) { (void)c; (void)ud; }
static void *f_getUserdata(TCPConnection *c) { (void)c; return NULL; }

static PDNetErr f_open(TCPConnection *conn, TCPOpenCallback cb, void *ud)
{
    /* Synchronous success: matches how the pump consumes the callback. */
    cb(conn, NET_OK, ud);
    return NET_OK;
}
static PDNetErr f_closeConn(TCPConnection *c) { (void)c; return NET_OK; }
static void f_setClosedCb(TCPConnection *c, TCPConnectionCallback cb)
{
    ((FakeConn *)c)->closedCb = cb;
}
static void f_setReadTimeout(TCPConnection *c, int ms) { (void)c; (void)ms; }
static void f_setReadBufferSize(TCPConnection *c, int b) { (void)c; (void)b; }

static size_t f_getBytesAvailable(TCPConnection *c)
{
    FakeConn *k = (FakeConn *)c;
    return k->len > k->sent ? k->len - k->sent : 0;
}
static int f_read(TCPConnection *c, void *buffer, size_t length)
{
    FakeConn *k = (FakeConn *)c;
    size_t avail = k->len > k->sent ? k->len - k->sent : 0;
    size_t n = avail < length ? avail : length;
    if (n == 0)
    {
        return 0;
    }
    memcpy(buffer, k->resp + k->sent, n);
    k->sent += n;
    return (int)n;
}
static int f_write(TCPConnection *c, const void *buffer, size_t length)
{
    (void)c; (void)buffer;
    return (int)length;
}
static size_t f_getSentPending(TCPConnection *c) { (void)c; return 0; }

static unsigned int f_seconds(unsigned int *ms)
{
    *ms = 0;
    return 1000000000u;
}
static void f_sysLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}
static unsigned int f_timeMs(void) { return 0; }

static enum accessReply f_requestAccess(const char *server, int port,
                                        bool usessl, const char *purpose,
                                        AccessRequestCallback *cb,
                                        void *userdata)
{
    (void)server; (void)port; (void)usessl; (void)purpose;
    if (cb)
    {
        cb(true, userdata);
    }
    return kAccessAllow;
}

static struct playdate_sys g_fakeSys = {
    .getCurrentTimeMilliseconds = f_timeMs,
    .getSecondsSinceEpoch = f_seconds,
    .logToConsole = f_sysLog,
};
static struct playdate_tcp g_fakeTcp = {
    .requestAccess = f_requestAccess,
    .newConnection = f_newConnection,
    .retain = f_retain,
    .release = f_release,
    .getError = f_getError,
    .setConnectTimeout = f_setConnectTimeout,
    .setUserdata = f_setUserdata,
    .getUserdata = f_getUserdata,
    .open = f_open,
    .close = f_closeConn,
    .setConnectionClosedCallback = f_setClosedCb,
    .setReadTimeout = f_setReadTimeout,
    .setReadBufferSize = f_setReadBufferSize,
    .getBytesAvailable = f_getBytesAvailable,
    .read = f_read,
    .write = f_write,
    .getSentBytesPending = f_getSentPending,
};
static struct playdate_network g_fakeNet = { .tcp = &g_fakeTcp };
static PlaydateAPI g_fakeApi = {
    .system = &g_fakeSys,
    .network = &g_fakeNet,
};

/* Host backends expected by pluto_mem / spill (host builds).
 * NOTE: pluto_pd must return the fake API — logger and the funnel
 * dereference it throughout. */
void *pluto_mem_sdk_realloc(void *p, size_t n) { return realloc(p, n); }
PlaydateAPI *pluto_pd(void) { return &g_fakeApi; }

/* ── Result capture ──────────────────────────────────────────────────────── */
static char g_body[262144];
static size_t g_bodyLen;
static int g_gotSuccess, g_gotError;
static char g_errMsg[256];
static int g_status;

static void on_success(int status, char **headerKeys, char **headerVals,
                       int headerCount, const char *body, size_t bodyLen,
                       const char *url)
{
    (void)headerKeys; (void)headerVals; (void)headerCount; (void)url;
    g_status = status;
    g_bodyLen = bodyLen < sizeof(g_body) ? bodyLen : sizeof(g_body) - 1;
    memcpy(g_body, body, g_bodyLen);
    g_body[g_bodyLen] = '\0';
    g_gotSuccess = 1;
}
static void on_error(const char *msg)
{
    strncpy(g_errMsg, msg, sizeof(g_errMsg) - 1);
    g_gotError = 1;
}

/* ── One request against a canned response (binary-safe) ─────────────── */
static void run_case_len(const char *resp, size_t len)
{
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.resp = resp;
    g_conn.len = len;
    g_gotSuccess = g_gotError = 0;
    g_status = 0;
    g_bodyLen = 0;
    g_errMsg[0] = '\0';

    HttpCallbacks cb = { .onSuccess = on_success, .onError = on_error };
    if (!http_get("http://test.local/x", &cb))
    {
        return;
    }
    FakeConn *k = &g_conn;
    for (int i = 0; i < 5000; i++)
    {
        http_update();
        if (g_gotSuccess || g_gotError)
        {
            break;
        }
        if (k->sent >= k->len && k->closedCb && !k->closeFired)
        {
            k->closeFired = 1;
            k->closedCb((TCPConnection *)k, NET_OK);
        }
    }
    if (!g_gotSuccess && !g_gotError)
    {
        printf("[diag] TIMEOUT (no callback fired)\n");
    }
    else
    {
        printf("[diag] success=%d error=%d status=%d bodyLen=%zu err=%s\n",
               g_gotSuccess, g_gotError, g_status, g_bodyLen, g_errMsg);
    }
}
#define run_case(resp) run_case_len(resp, resp##_LEN)

/* ── Fixtures injected at build time (binary-safe; lengths via macros) ──── */
/* Each GZn_RESPONSE is a complete HTTP response (head + binary gzip body),
 * escaped as a C string literal by build_gz.sh (Python zlib for the
 * deflate payloads, Python composes chunked framing and Content-Length
 * headers). GZn_RESPONSE_LEN macros come from the same generator. */
__GZ_FIXTURES__

/* ── Cases ───────────────────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                    \
    do                                                                       \
    {                                                                        \
        if (cond) { printf("PASS %s\n", name); g_pass++; }                   \
        else      { printf("FAIL %s\n", name); g_fail++; }                   \
    } while (0)

int main(void)
{
    http_client_init(&g_fakeApi);

    /* 1: plain gzip HTML, Content-Length = compressed size. */
    run_case(GZ1_RESPONSE);
    CHECK(g_gotSuccess && g_status == 200 && g_bodyLen == strlen(GZ1_TEXT) &&
              memcmp(g_body, GZ1_TEXT, strlen(GZ1_TEXT)) == 0,
          "gzip body delivered byte-exact (strict ISIZE)");

    /* 2: gzip + chunked, no Content-Length (conn-close completion). */
    run_case(GZ2_RESPONSE);
    CHECK(g_gotSuccess && g_bodyLen == strlen(GZ1_TEXT) &&
              memcmp(g_body, GZ1_TEXT, strlen(GZ1_TEXT)) == 0,
          "gzip+chunked decoded then gunzipped");

    /* 3: FEXTRA/FNAME/FCOMMENT/FHCRC all present. */
    run_case(GZ3_RESPONSE);
    CHECK(g_gotSuccess && g_bodyLen == strlen(GZ3_TEXT) &&
              memcmp(g_body, GZ3_TEXT, strlen(GZ3_TEXT)) == 0,
          "gzip with all optional header fields");

    /* 4: empty member (ISIZE 0). */
    run_case(GZ4_RESPONSE);
    CHECK(g_gotSuccess && g_bodyLen == 0,
          "empty gzip member delivers empty body");

    /* 5: identity regression — plain body untouched. */
    run_case(GZ5_RESPONSE);
    CHECK(g_gotSuccess && g_bodyLen == 12 &&
              memcmp(g_body, "hello world!", 12) == 0,
          "identity body untouched (no gzip sniff)");

    /* 6: corrupt member (bad magic). */
    run_case(GZ6_RESPONSE);
    CHECK(g_gotError && !g_gotSuccess,
          "corrupt gzip -> onError, no onSuccess");

    /* 7: truncated member (declares full length but data cut short). */
    run_case(GZ7_RESPONSE);
    CHECK(g_gotError && !g_gotSuccess,
          "truncated gzip -> onError");

    /* 8: ISIZE lie (footer smaller than produced). */
    run_case(GZ8_RESPONSE);
    CHECK(g_gotError && !g_gotSuccess,
          "ISIZE lie refused (strict accounting)");

    printf("gzip suite: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
