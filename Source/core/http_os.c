/*
 * PlutoBrowser — http_os.c
 * The OS-HTTP network backend (R62). See http_os.h for the contract.
 *
 * DESIGN
 *   - The connection is created FROM THE UPDATE PUMP (never from the
 *     caller's context): playdate.network.http's permission dialog uses
 *     a coroutine yield, which is only legal in the update() context —
 *     the same defer-to-later-frame rule the TCP client follows for its
 *     own TLS settle ("Request written from a later update frame").
 *   - CONNECTION LIFECYCLE (the lldb receipt, 2026-10-26): the OS fires
 *     notifyRequestComplete from a QUEUED notification — a close()/
 *     release() before that notification has been observed crashes the
 *     stack (the notification then derefs a nulled connection slot).
 *     The lifecycle is therefore:
 *       keep-alive + SAME-HOST REUSE first (the common page+css+scripts
 *       flow rides ONE connection — zero churn), and a delivered
 *       connection RETIRES (pointer swap, left open) until its
 *       request-complete / connection-closed notification is observed
 *       in OUR callback — only then is it close()+release()d. A
 *       connection never observed is LEAKED (bounded, logged) rather
 *       than risked — an un-notified close is the documented crash.
 *   - Headers arrive through setHeaderReceivedCallback (per key/value —
 *     multiple Set-Cookie survive: a richer shape than the TCP client's
 *     single-line scan). setHeadersReadCallback marks the header/body
 *     boundary: at that point the cookie jar is fed, the Content-Type /
 *     Content-Length / Content-Encoding facts are recorded, and the
 *     spill-vs-RAM residency decision is made (SW2b semantics).
 *   - Body bytes are pumped from read() in bounded windows — straight
 *     to the spill file (disk) or the RAM staging buffer. The OS fires
 *     request-complete when IT has received everything (its internal
 *     buffer can outrun this pump by frames), so completion NEVER
 *     delivers before the drain: every available window is pumped
 *     first, then the completion marks are honored.
 *   - Stale-callback guard: the request's generation id rides the
 *     connection's userdata; every callback verifies it (pointer
 *     compare for the retired connection — no deref needed).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/http_os.h"
#include "core/url.h"
#include "core/logger.h"
#include "core/pluto_spill.h"
#include "core/pluto_mem.h"
#include "core/cookie_jar.h"
#include "util/strbuf.h"

/* Use the SDK allocator so memory is accounted by the Playdate runtime. */
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)

/* Bounded response-side shapes (mirrors the TCP client's tables). */
#define OS_HTTP_MAX_HEADERS 62
#define OS_HTTP_KEY_MAX 48
#define OS_HTTP_VAL_MAX 256
#define OS_HTTP_SET_COOKIE_MAX 16
#define OS_HTTP_SET_COOKIE_MAX_LEN 512
#define OS_HTTP_READ_CHUNK 16384
#define OS_HTTP_HEADER_BLOCK_MAX 1024
#define OS_HTTP_CONNECT_TIMEOUT_MS 10000
#define OS_HTTP_READ_TIMEOUT_MS 10000
#define OS_HTTP_REQUEST_WATCHDOG_MS 60000
/* The RAM-delivery residency cap (the TCP client's MAX_RESPONSE_SIZE
 * rule): a receiver without onSuccessSpill takes the materialized body
 * only under this cap — bigger responses fail with the standing error
 * instead of ballooning the pool. */
#define OS_HTTP_MAX_RAM_BODY (2 * 1024 * 1024)

/* Phases. */
enum
{
    OS_PHASE_IDLE = 0,
    OS_PHASE_CONNECT, /* request stored; connection created next pump */
    OS_PHASE_READ     /* request queued; body pumping via callbacks */
};

typedef struct
{
    int active;
    int phase;
    unsigned requestId; /* generation guard (userdata-stamped) */
    unsigned startMs;

    /* request side (owned copies) */
    UrlParsed url;
    char fullPath[8192];
    char method[8];
    char headerBlock[OS_HTTP_HEADER_BLOCK_MAX];
    char *body;
    size_t bodyLen;

    /* response side */
    int headersDone;
    int status;
    char hdrKeys[OS_HTTP_MAX_HEADERS][OS_HTTP_KEY_MAX];
    char hdrVals[OS_HTTP_MAX_HEADERS][OS_HTTP_VAL_MAX];
    int hdrCount;
    char setCookies[OS_HTTP_SET_COOKIE_MAX][OS_HTTP_SET_COOKIE_MAX_LEN];
    int setCookieCount;
    int contentLength; /* -1 unknown */
    int isGzip;
    int redirectDepth;

    /* body residency (SW2b semantics) */
    SpillFile spill;
    int spillMode; /* 0 none, 1 SPILL_ACTIVE */
    size_t bodySpilled;
    StrBuf ram; /* RAM staging when not spilling */

    /* completion marks (flags set by OS callbacks, consumed by the pump) */
    int sawHeadersRead;
    int sawComplete;
    int sawConnClosed;
    int retries; /* the one pre-headers re-queue */

    HttpCallbacks cb;
} OsHttpReq;

static OsHttpReq g_req;
static PlaydateAPI *g_pd;
static unsigned g_reqIdStamp;
static char g_osReadChunk[OS_HTTP_READ_CHUNK]; /* P19: static, not stack */

/* ── the connection lifecycle (retire-until-notified) ────────────────────
 * The SDK fires request-complete from a QUEUED notification: a close()/
 * release() before that notification has been observed crashes the stack
 * (the notification then derefs a nulled connection slot). The original
 * single-slot design retired ONE connection and dropped it at the next
 * request if the notification had not landed yet. That is a race: the
 * notification is delivered by the event loop, which can run AFTER the
 * next request's connect pump. The dropped connection was then never
 * released, and — worse — it stayed OPEN. The device allows at most four
 * simultaneous connections; an image-heavy page leaked six, exhausting
 * the pool for every later navigation ("buffer too small (err=-14)").
 *
 * The fix is a small retire LIST. Every connection that is not yet safe
 * to release waits here until its notification is observed. At
 * navigation we close every retired connection (the object stays alive,
 * so the queued notification can still fire safely) to free its slot
 * immediately; the object is released when the notification lands, or
 * after a long idle grace period if the notification never comes. */
#define OS_HTTP_RETIRED_MAX 8
#define OS_HTTP_RETIRE_GRACE_MS 30000
#define OS_HTTP_AUTOCLOSE_MS 2000

typedef struct
{
    HTTPConnection *conn;
    int notifyDone; /* request-complete / connection-closed observed */
    int closed;     /* close() already called at navigation */
    unsigned addedMs;
    unsigned closeMs;
} OsHttpRetired;

static HTTPConnection *g_liveConn;     /* the request's live connection */
static char g_liveHost[128];
static int g_livePort;
static int g_liveSsl;
static OsHttpRetired g_retired[OS_HTTP_RETIRED_MAX];
static int g_retiredN;

void oshttp_init(PlaydateAPI *pd)
{
    g_pd = pd;
    memset(&g_req, 0, sizeof(g_req));
    g_req.spill = PLUTO_SPILL_INVALID;
    g_req.contentLength = -1;
    g_retiredN = 0;
    logger_log("[oshttp] backend ready (OS network.http stack)");
}

/* ── teardown helpers ──────────────────────────────────────────────────── */

/* Close+release is legal only once the queued notification was observed.
 * close() alone is safe at any time: the object stays allocated, so a
 * late notification still has a live slot to dereference. */
static void conn_close(HTTPConnection *conn)
{
    if (conn)
    {
        g_pd->network->http->close(conn);
    }
}

static void conn_release(HTTPConnection *conn)
{
    if (conn)
    {
        g_pd->network->http->release(conn);
    }
}

static void conn_destroy(HTTPConnection *conn)
{
    conn_close(conn);
    conn_release(conn);
}

static int retired_index(HTTPConnection *conn)
{
    if (!conn)
    {
        return -1;
    }
    for (int i = 0; i < g_retiredN; i++)
    {
        if (g_retired[i].conn == conn)
        {
            return i;
        }
    }
    return -1;
}

/* Park a connection until its queued notification is observed. */
static void retired_add(HTTPConnection *conn, int notified)
{
    if (!conn || retired_index(conn) >= 0)
    {
        return;
    }
    if (notified)
    {
        conn_destroy(conn);
        return;
    }
    if (g_retiredN >= OS_HTTP_RETIRED_MAX)
    {
        /* Bounded overflow: close to free the slot, leave the object to
         * the SDK. Never release an un-notified object (the crash). */
        logger_log("[oshttp] retired list full — closing object, left to SDK");
        conn_close(conn);
        return;
    }
    OsHttpRetired *r = &g_retired[g_retiredN++];
    r->conn = conn;
    r->notifyDone = 0;
    r->closed = 0;
    r->addedMs = g_pd->system->getCurrentTimeMilliseconds();
    r->closeMs = 0;
}

/* Reap connections whose notification landed. A connection closed at
 * navigation whose notification never arrives is released after a long
 * idle grace period — the queued notification would have fired by then,
 * and keeping the object forever exhausts the SDK's connection pool. */
static void retired_reap(void)
{
    unsigned now = g_pd->system->getCurrentTimeMilliseconds();
    for (int i = 0; i < g_retiredN;)
    {
        OsHttpRetired *r = &g_retired[i];
        if (r->notifyDone)
        {
            conn_destroy(r->conn);
            g_retired[i] = g_retired[--g_retiredN];
            continue;
        }
        if (r->closed && !g_req.active &&
            (now - r->closeMs) > OS_HTTP_RETIRE_GRACE_MS)
        {
            logger_log("[oshttp] releasing un-notified retired connection "
                       "after grace");
            conn_release(r->conn);
            g_retired[i] = g_retired[--g_retiredN];
            continue;
        }
        /* The notification normally lands within a frame or two. If it
         * is slow, close the connection (safe: the object stays alive)
         * so its simultaneous-connection slot is free for the next
         * fetch; the notification can still settle it later. */
        if (!r->closed && (now - r->addedMs) > OS_HTTP_AUTOCLOSE_MS)
        {
            conn_close(r->conn);
            r->closed = 1;
            r->closeMs = now;
            logger_log("[oshttp] auto-closed retired connection after 2s");
        }
        i++;
    }
}

/* Navigation: stop every old connection NOW. Closing frees the SDK's
 * simultaneous-connection slot; the object lives until its notification
 * (or the grace release) so the queued callback can never deref freed
 * memory. */
static void retired_close_all(void)
{
    unsigned now = g_pd->system->getCurrentTimeMilliseconds();
    for (int i = 0; i < g_retiredN; i++)
    {
        if (!g_retired[i].closed)
        {
            conn_close(g_retired[i].conn);
            g_retired[i].closed = 1;
            g_retired[i].closeMs = now;
        }
    }
    retired_reap();
}

static void req_reset(void)
{
    if (g_req.spill != PLUTO_SPILL_INVALID)
    {
        pluto_spill_discard(g_req.spill);
        g_req.spill = PLUTO_SPILL_INVALID;
    }
    g_req.spillMode = 0;
    strbuf_free(&g_req.ram);
    if (g_req.body)
    {
        PLUTO_FREE(g_req.body);
        g_req.body = NULL;
        g_req.bodyLen = 0;
    }
    memset(&g_req, 0, sizeof(g_req));
    g_req.spill = PLUTO_SPILL_INVALID;
    g_req.contentLength = -1;
}

static void req_fail(const char *message)
{
    char errSnapshot[256];
    HttpCallbacks cb = g_req.cb;
    int notified = g_req.sawComplete || g_req.sawConnClosed;
    logger_log("[oshttp] FAIL %.90s", message ? message : "?");
    g_reqIdStamp++; /* stale-callback guard: bump before the fire */
    g_req.active = 0;
    /* Retire (or destroy if the notification was already observed). */
    if (g_liveConn)
    {
        retired_add(g_liveConn, notified);
        g_liveConn = NULL;
    }
    snprintf(errSnapshot, sizeof(errSnapshot), "%s",
             message ? message : "OS-HTTP request failed.");
    req_reset();
    retired_reap();
    if (cb.onError)
    {
        cb.onError(errSnapshot);
    }
}

/* The delivery (done) path — the TCP client's contract: onSuccess rides
 * the collected header table (per-header OS callbacks kept every pair —
 * multiple Set-Cookie included) + the RAM body copy; the spill lane
 * hands the adopted handle. Static scratch for the callback's duration
 * (single-flight makes sharing safe; P19: never on the task stack). */
static char *g_deliverKeys[OS_HTTP_MAX_HEADERS];
static char *g_deliverVals[OS_HTTP_MAX_HEADERS];
static char g_deliverKeyStore[OS_HTTP_MAX_HEADERS][OS_HTTP_KEY_MAX];
static char g_deliverValStore[OS_HTTP_MAX_HEADERS][OS_HTTP_VAL_MAX];

static void deliver_with_headers(void)
{
    int status = g_req.status;
    HttpCallbacks cb = g_req.cb;
    char urlSnap[128];
    size_t bodyLen;
    char *bodyCopy = NULL;
    int i;

    snprintf(urlSnap, sizeof(urlSnap), "%.127s", g_req.url.normalized);

    /* the spill lane: finish the file, hand the ADOPTED handle over —
     * the bulk never materialized in RAM (SW2b/R15 disk delivery) */
    if (g_req.spillMode && g_req.bodySpilled > 0 &&
        g_req.spill != PLUTO_SPILL_INVALID)
    {
        long total = pluto_spill_finish(g_req.spill);
        SpillFile sp = g_req.spill;
        int notified = g_req.sawComplete || g_req.sawConnClosed;
        g_req.spill = PLUTO_SPILL_INVALID;
        g_req.spillMode = 0;
        logger_log("[oshttp] done status=%d spilled=%ld (disk delivery)",
                   status, total);
        g_reqIdStamp++;
        g_req.active = 0;
        if (g_liveConn)
        {
            retired_add(g_liveConn, notified);
            g_liveConn = NULL;
        }
        req_reset();
        retired_reap();
        if (cb.onSuccessSpill && total > 0)
        {
            cb.onSuccessSpill(status, sp, (size_t)total, urlSnap);
        }
        else if (cb.onError)
        {
            cb.onError("OS-HTTP spill delivery failed.");
        }
        return;
    }

    for (i = 0; i < g_req.hdrCount; i++)
    {
        g_deliverKeyStore[i][0] = '\0';
        g_deliverValStore[i][0] = '\0';
        snprintf(g_deliverKeyStore[i], OS_HTTP_KEY_MAX, "%s",
                 g_req.hdrKeys[i]);
        snprintf(g_deliverValStore[i], OS_HTTP_VAL_MAX, "%s",
                 g_req.hdrVals[i]);
        g_deliverKeys[i] = g_deliverKeyStore[i];
        g_deliverVals[i] = g_deliverValStore[i];
    }
    bodyLen = g_req.ram.len;
    if (bodyLen > 0 && g_req.ram.data)
    {
        bodyCopy = (char *)PLUTO_MALLOC(bodyLen + 1);
        if (bodyCopy)
        {
            memcpy(bodyCopy, g_req.ram.data, bodyLen);
            bodyCopy[bodyLen] = '\0';
        }
    }
    logger_log("[oshttp] done status=%d hdrs=%d ram=%zu (RAM delivery)",
               status, g_req.hdrCount, bodyLen);
    {
        int notified = g_req.sawComplete || g_req.sawConnClosed;
        int hdrCount = g_req.hdrCount;
        g_reqIdStamp++;
        g_req.active = 0;
        if (g_liveConn)
        {
            retired_add(g_liveConn, notified);
            g_liveConn = NULL;
        }
        req_reset();
        retired_reap();
        if (bodyLen > 0 && !bodyCopy)
        {
            if (cb.onError)
            {
                cb.onError("OS-HTTP body copy OOM.");
            }
            return;
        }
        if (cb.onSuccess)
        {
            cb.onSuccess(status, g_deliverKeys, g_deliverVals, hdrCount,
                         bodyCopy ? bodyCopy : "", bodyLen, urlSnap);
        }
    }
    if (bodyCopy)
    {
        PLUTO_FREE(bodyCopy);
    }
}

/* ── OS callbacks (tiny: stamp-guard + record; work happens in the pump) ──
 * A retired connection's late notifications land here too: mark the
 * entry so the reaper can close+release it. Pointer lookup only — the
 * retired object is alive (never released before its notification). */
static int cb_is_retired(HTTPConnection *conn)
{
    return retired_index(conn) >= 0;
}

static int cb_generation_is_current(HTTPConnection *conn)
{
    void *ud = g_pd->network->http->getUserdata(conn);
    return g_req.active && ud && (unsigned)(uintptr_t)ud == g_req.requestId;
}

static void oshttp_header_cb(HTTPConnection *conn, const char *key,
                             const char *value)
{
    if (cb_is_retired(conn) || !cb_generation_is_current(conn))
    {
        return; /* stale callback: retired connection or old generation */
    }
    if (!key || !value)
    {
        return;
    }
    if (g_req.hdrCount < OS_HTTP_MAX_HEADERS)
    {
        snprintf(g_req.hdrKeys[g_req.hdrCount], OS_HTTP_KEY_MAX, "%s", key);
        snprintf(g_req.hdrVals[g_req.hdrCount], OS_HTTP_VAL_MAX, "%.250s",
                 value);
        g_req.hdrCount++;
    }
    /* the Set-Cookie lane (raw values, the jar parses the shape) */
    if (!strcasecmp(key, "set-cookie") &&
        g_req.setCookieCount < OS_HTTP_SET_COOKIE_MAX)
    {
        snprintf(g_req.setCookies[g_req.setCookieCount],
                 OS_HTTP_SET_COOKIE_MAX_LEN, "%s", value);
        g_req.setCookieCount++;
    }
    if (!strcasecmp(key, "content-length"))
    {
        g_req.contentLength = atoi(value);
    }
    else if (!strcasecmp(key, "content-encoding") &&
             strstr(value, "gzip") != NULL)
    {
        g_req.isGzip = 1;
    }
}

static void oshttp_headers_read_cb(HTTPConnection *conn)
{
    if (cb_is_retired(conn) || !cb_generation_is_current(conn))
    {
        return;
    }
    g_req.sawHeadersRead = 1;
    g_req.status = g_pd->network->http->getResponseStatus(conn);
    g_req.headersDone = 1;
    logger_log("[oshttp] headers read: status=%d hdrs=%d cookies=%d "
               "len=%d gzip=%d",
               g_req.status, g_req.hdrCount, g_req.setCookieCount,
               g_req.contentLength, g_req.isGzip);
    /* the cookie jar (browser-true: response cookies land before the
     * body's consumers run) */
    if (g_req.setCookieCount > 0 && g_req.url.host[0])
    {
        char *list[OS_HTTP_SET_COOKIE_MAX];
        for (int i = 0; i < g_req.setCookieCount; i++)
        {
            list[i] = g_req.setCookies[i];
        }
        cookie_jar_process_set_cookies(g_req.url.host, list,
                                       g_req.setCookieCount);
    }
    /* residency decision — SW2b semantics: big/unknown bodies stream to
     * disk ONLY when the receiver can adopt a spill handle (the TCP
     * client's rule — a RAM-only receiver takes the materialized body);
     * gzip staging stays in RAM (v1 delivers it raw + warning). */
    if (g_req.isGzip)
    {
        logger_log("[oshttp] WARNING gzip response on the OS stack — "
                   "v1 delivers raw (identity was requested)");
    }
    else if (g_req.cb.onSuccessSpill)
    {
        pluto_spill_init();
        g_req.spill = pluto_spill_begin();
        g_req.spillMode = (g_req.spill != PLUTO_SPILL_INVALID) ? 1 : 0;
    }
}

static void oshttp_complete_cb(HTTPConnection *conn)
{
    int idx = retired_index(conn);
    if (idx >= 0)
    {
        /* the delivered connection's notification JUST fired — its
         * close()+release() is now safe (the reaper takes it) */
        g_retired[idx].notifyDone = 1;
        return;
    }
    if (!cb_generation_is_current(conn))
    {
        return;
    }
    g_req.sawComplete = 1;
}

static void oshttp_closed_cb(HTTPConnection *conn)
{
    int idx = retired_index(conn);
    if (idx >= 0)
    {
        /* the server closed a retired connection: either notification
         * settles it (the request-complete one may never come) */
        g_retired[idx].notifyDone = 1;
        return;
    }
    if (!cb_generation_is_current(conn))
    {
        return;
    }
    g_req.sawConnClosed = 1;
}

/* ── request side ──────────────────────────────────────────────────────── */

void oshttp_cancel(void)
{
    if (g_req.active)
    {
        int notified = g_req.sawComplete || g_req.sawConnClosed;
        logger_log("[oshttp] cancel");
        g_reqIdStamp++; /* stale-callback guard: bump before the reset */
        g_req.active = 0;
        if (g_liveConn)
        {
            retired_add(g_liveConn, notified);
            g_liveConn = NULL;
        }
        req_reset();
    }
    /* Navigation / abort: close every retired connection so its SDK
     * simultaneous-connection slot is free for the next page. The object
     * stays alive until its notification (or the idle grace release). */
    retired_close_all();
}

int oshttp_is_loading(void)
{
    return g_req.active;
}

int oshttp_request_h(const char *method, const char *urlString,
                     const char *contentType, const HttpHeaderPair *extra,
                     int nExtra, const char *body, size_t bodyLen,
                     const HttpCallbacks *callbacks)
{
    if (!urlString || !urlString[0])
    {
        return 0;
    }
    if (g_req.active)
    {
        oshttp_cancel(); /* single-flight: a new request cancels the old */
    }

    /* parse the target (BSS request struct owns the parsed shape) */
    if (url_parse(urlString, &g_req.url) != 0)
    {
        logger_log("[oshttp] FAIL url parse: %.80s", urlString);
        return 0;
    }

    /* the request spec (owned copies — the caller's storage is transient) */
    snprintf(g_req.method, sizeof(g_req.method), "%s",
             (method && method[0]) ? method : "GET");
    snprintf(g_req.fullPath, sizeof(g_req.fullPath), "%s",
             g_req.url.fullPath[0] ? g_req.url.fullPath : "/");
    /* R66: prefer the raw URL's request-target when it fits (long modern
     * script/CSS URLs exceed UrlParsed.fullPath's compact storage). */
    url_request_target(urlString, g_req.fullPath, sizeof(g_req.fullPath));
    if (body && bodyLen > 0)
    {
        g_req.body = (char *)PLUTO_MALLOC(bodyLen);
        if (!g_req.body)
        {
            logger_log("[oshttp] FAIL body OOM");
            req_reset();
            return 0;
        }
        memcpy(g_req.body, body, bodyLen);
        g_req.bodyLen = bodyLen;
    }

    /* the raw header block: identity encoding (v1 policy), the jar's
     * Cookie, the caller's extra pairs, the POST Content-Type */
    {
        size_t used = 0;
#define OS_APPEND_HDR(fmt, val)                                              \
    do                                                                       \
    {                                                                        \
        if (used + strlen(val) + 4 < sizeof(g_req.headerBlock))              \
        {                                                                    \
            used += (size_t)snprintf(g_req.headerBlock + used,               \
                                     sizeof(g_req.headerBlock) - used,       \
                                     fmt, val);                              \
        }                                                                    \
    } while (0)
        OS_APPEND_HDR("%s", "Accept-Encoding: identity\r\n");
        {
            static char cookieBuf[512];
            cookie_jar_get_header(g_req.url.host, g_req.url.path,
                                  g_req.url.isSsl, cookieBuf,
                                  sizeof(cookieBuf));
            if (cookieBuf[0])
            {
                OS_APPEND_HDR("Cookie: %s\r\n", cookieBuf);
            }
        }
        for (int i = 0; extra && i < nExtra && i < HTTP_EXTRA_HEADERS_MAX;
             i++)
        {
            char line[300];
            snprintf(line, sizeof(line), "%.40s: %.240s\r\n", extra[i].name,
                     extra[i].value);
            OS_APPEND_HDR("%s", line);
        }
        if (g_req.body)
        {
            OS_APPEND_HDR("Content-Type: %s\r\n",
                          (contentType && contentType[0]) ? contentType
                                                          : "application/json");
        }
#undef OS_APPEND_HDR
    }

    g_req.cb = *callbacks;
    g_req.phase = OS_PHASE_CONNECT;
    g_req.requestId = ++g_reqIdStamp;
    g_req.startMs = g_pd->system->getCurrentTimeMilliseconds();
    g_req.active = 1;
    logger_log("[oshttp] request %s %.90s (body=%u)", g_req.method,
               urlString, (unsigned)g_req.bodyLen);
    return 1;
}

/* ── the pump ──────────────────────────────────────────────────────────── */

static void pump_connect(void)
{
    /* Reap every retired connection whose notification landed. */
    retired_reap();

    /* v1: NO connection reuse — each request gets a fresh connection
     * (without keep-alive the server closes it after one response
     * anyway). Any live connection still here is retired, never
     * close+released before its notification. */
    if (g_liveConn)
    {
        retired_add(g_liveConn, 0);
        g_liveConn = NULL;
    }
    g_liveConn = g_pd->network->http->newConnection(
        g_req.url.host, g_req.url.port, g_req.url.isSsl);
    if (!g_liveConn)
    {
        logger_log("[oshttp] newConnection NULL (retired=%d) host=%.80s",
                   g_retiredN, g_req.url.host);
        req_fail("connection object failed");
        return;
    }
    snprintf(g_liveHost, sizeof(g_liveHost), "%s", g_req.url.host);
    g_livePort = g_req.url.port;
    g_liveSsl = g_req.url.isSsl;
    /* NOTE: setKeepAlive is deliberately NOT set — the A/B run with
     * keep-alive armed left the OS stack silently never processing the
     * queued request (60s watchdog, 03:00 run). */
    g_pd->network->http->setConnectTimeout(g_liveConn,
                                           OS_HTTP_CONNECT_TIMEOUT_MS);
    g_pd->network->http->setReadTimeout(g_liveConn,
                                        OS_HTTP_READ_TIMEOUT_MS);
    g_pd->network->http->setReadBufferSize(g_liveConn, 16384);
    g_pd->network->http->setHeaderReceivedCallback(g_liveConn,
                                                   oshttp_header_cb);
    g_pd->network->http->setHeadersReadCallback(g_liveConn,
                                                oshttp_headers_read_cb);
    g_pd->network->http->setRequestCompleteCallback(g_liveConn,
                                                    oshttp_complete_cb);
    g_pd->network->http->setConnectionClosedCallback(g_liveConn,
                                                     oshttp_closed_cb);
    g_req.requestId = ++g_reqIdStamp; /* the id the callbacks will see */
    g_pd->network->http->setUserdata(g_liveConn,
                                     (void *)(uintptr_t)g_req.requestId);

    {
        PDNetErr err;
        if (!strcmp(g_req.method, "POST"))
        {
            err = g_pd->network->http->post(
                g_liveConn, g_req.fullPath, g_req.headerBlock,
                strlen(g_req.headerBlock), g_req.body ? g_req.body : "",
                g_req.bodyLen);
        }
        else
        {
            /* the general door: any METHOD rides query (GET with a body
             * stays honest — the browser's own rule) */
            err = g_pd->network->http->query(
                g_liveConn, g_req.method, g_req.fullPath, g_req.headerBlock,
                strlen(g_req.headerBlock), g_req.body ? g_req.body : "",
                g_req.bodyLen);
        }
        if (err != 0)
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "OS-HTTP queue failed: %d",
                     (int)err);
            req_fail(msg);
            return;
        }
    }
    g_req.phase = OS_PHASE_READ;
    logger_log("[oshttp] request queued (path=%.70s hdrs=%u)",
               g_req.fullPath, (unsigned)strlen(g_req.headerBlock));
}

static void pump_read(void)
{
    /* ── DRAIN FIRST ── the OS fires request-complete when IT has
     * received everything (its internal buffer), which can outrun this
     * pump by whole frames. Never deliver on a completion mark before
     * draining every available byte window (bounded loop). */
    for (int guard = 0; guard < 8; guard++)
    {
        size_t avail;
        size_t want;
        int n;
        if (!g_req.headersDone)
        {
            break;
        }
        avail = g_pd->network->http->getBytesAvailable(g_liveConn);
        if (avail == 0)
        {
            break;
        }
        want = avail < OS_HTTP_READ_CHUNK ? avail : OS_HTTP_READ_CHUNK;
        n = g_pd->network->http->read(g_liveConn, g_osReadChunk, want);
        if (n <= 0)
        {
            break; /* busy/timeout: retry next frame */
        }
        if (g_req.spillMode)
        {
            if (pluto_spill_write(g_req.spill, g_osReadChunk, (size_t)n) !=
                0)
            {
                logger_log("[oshttp] spill write failed; RAM fallback");
                g_req.spillMode = 0;
            }
            else
            {
                g_req.bodySpilled += (size_t)n;
            }
        }
        if (!g_req.spillMode)
        {
            if ((size_t)n + g_req.ram.len > OS_HTTP_MAX_RAM_BODY)
            {
                req_fail("Response too large to deliver.");
                return;
            }
            strbuf_append_n(&g_req.ram, g_osReadChunk, (size_t)n);
        }
        if (g_req.cb.onProgress)
        {
            long total = g_req.contentLength;
            g_req.cb.onProgress((int)(g_req.spillMode ? g_req.bodySpilled
                                                      : (size_t)g_req.ram.len),
                                (int)(total > 0 ? total : 0));
        }
    }

    /* ── completion marks AFTER the drain (bodyBytes is now the truth) ── */
    {
        size_t bodyBytes = (g_req.spillMode ? g_req.bodySpilled
                                            : (size_t)g_req.ram.len);
        if (g_req.sawComplete || g_req.sawConnClosed ||
            (g_req.contentLength >= 0 &&
             (long)bodyBytes >= g_req.contentLength))
        {
            if (g_req.headersDone)
            {
                deliver_with_headers();
            }
            else if (g_req.retries < 1)
            {
                /* the connection died pre-headers: re-queue ONCE (the
                 * OS stack churns connections under rapid sequential
                 * requests — one retry absorbs it, jsext's skip-on-fail
                 * never sees it) */
                g_req.retries++;
                logger_log("[oshttp] pre-headers close — retry %d",
                           g_req.retries);
                /* the conn RETIRES (notification pending) — the retry's
                 * connect phase makes a fresh one */
                if (g_liveConn)
                {
                    retired_add(g_liveConn,
                                g_req.sawComplete || g_req.sawConnClosed);
                    g_liveConn = NULL;
                }
                g_req.phase = OS_PHASE_CONNECT;
                g_req.sawConnClosed = 0;
                g_req.sawComplete = 0;
                g_req.headersDone = 0;
                g_req.hdrCount = 0;
                g_req.setCookieCount = 0;
                g_req.contentLength = -1;
                g_req.isGzip = 0;
                g_req.startMs = g_pd->system->getCurrentTimeMilliseconds();
            }
            else
            {
                req_fail("connection closed before headers");
            }
            return;
        }
    }

    /* ── fatal-error check (never on top of a completion mark) ── */
    {
        PDNetErr err = g_pd->network->http->getError(g_liveConn);
        if (err != 0 && err != NET_READ_BUSY && err != NET_READ_TIMEOUT)
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "OS-HTTP error: %d", (int)err);
            req_fail(msg);
            return;
        }
    }
}

void oshttp_update(void)
{
    /* Retired connections settle here even when no request is active —
     * the queued notification often lands after the request finished. */
    retired_reap();
    if (!g_req.active)
    {
        return;
    }
    /* the 60s request watchdog (permission-dialog pauses stop update()
     * itself, so they cannot count against it — the TCP client's rule) */
    {
        unsigned now = g_pd->system->getCurrentTimeMilliseconds();
        if (now - g_req.startMs > OS_HTTP_REQUEST_WATCHDOG_MS)
        {
            req_fail("Request timed out.");
            return;
        }
    }
    if (g_req.phase == OS_PHASE_CONNECT)
    {
        pump_connect();
        return; /* the queue fires on the next pump (TLS settle rule) */
    }
    if (g_req.phase == OS_PHASE_READ)
    {
        pump_read();
    }
}
