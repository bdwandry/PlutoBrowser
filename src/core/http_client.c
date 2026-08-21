// http_client.c — raw TCP HTTP/HTTPS client (C port of core/http_client.lua).
//
// One request at a time over pd->network->tcp, with the Lua original's state
// machine reproduced block-for-block inside hc_update() (same order, same
// early returns, same quirks — see http_client.h for the annotated list).
//
// C-only additions the Lua SDK wrapper hid:
//  - network access gating via tcp->requestAccess + a session grant cache
//    keyed by host (kAccessAsk waits for the callback before connecting);
//  - millisecond timeouts (Lua used seconds: 10 s -> 10000 ms);
//  - stale-callback identification by connection pointer + generation id
//    stored through setUserdata (the closed-callback carries no userdata);
//  - write/read failures surface PDNetErr names where the Lua wrapper fed
//    arbitrary error strings ("Send failed: NET_WRITE_ERROR").
//
// Testability: hc_set_tcp_for_tests()/hc_set_clock_fn() swap the vtable and
// clock so selftest_http.c can replay the oracle scenarios offline.

#include "http_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "../util/mem.h"
#include "cookie_jar.h"
#include "internal_pages.h"
#include "url.h"

#define HC_MAX_RESPONSE_SIZE ((size_t)2097152)
#define HC_REQUEST_TIMEOUT_MS ((unsigned)60000)
#define HC_MAX_REDIRECTS 5
#define HC_READ_CHUNK 32768
#define HC_CONNECT_TIMEOUT_MS 10000
#define HC_READ_TIMEOUT_MS 10000
#define HC_SOCKET_BUFFER_BYTES 16384
#define HC_ABOUT_DELAY_MS 20
#define HC_URL_MAX 2048
#define HC_ERR_MAX 256
#define HC_MAX_HDRS 128
#define HC_MAX_SET_COOKIES 64
#define HC_NET_PURPOSE "CometBrowser Web Browsing"

enum {
    HC_IDLE = 0,
    HC_CONNECTING,
    HC_READING,
    HC_DONE,
    HC_ERROR,
};

static struct PlaydateAPI* s_pd;
// pd->network->tcp is a const vtable; tests inject a non-const fake.
static const struct playdate_tcp* s_tcp;
static unsigned (*s_clockfn)(void);

static TCPConnection* s_conn;
static int s_state;
static PlutoHttpCallbacks s_cbs;
static char s_url[HC_URL_MAX];
static PlutoUrl s_parsed;
static StrBuf s_buf;
static int s_status;

static StrMap* s_headers;
static char* s_hdrVals[HC_MAX_HDRS]; // owned header values
static size_t s_nHdrVals;
static char* s_setCookies[HC_MAX_SET_COOKIES]; // owned Set-Cookie values
static size_t s_nSetCookies;

static size_t s_bodyStart; // 0-based index of first body byte
static int s_hasBodyStart;
static int s_chunked;
static long s_contentLength;

static int s_connOpen;
static int s_openFailed;
static int s_connClosed;
static char s_error[HC_ERR_MAX];
static unsigned s_startMs;
static unsigned s_reqId;

static char s_pendingUrl[HC_URL_MAX];
static PlutoHttpCallbacks s_pendingCbs;
static int s_hasPendingRedirect;
static int s_redirectDepth;

static int s_aboutPending;
static unsigned s_aboutDueMs;
static const HcInternalPage* s_aboutPage;

static int s_accessWaiting;
static unsigned s_accessReqId;
static char s_grantHost[PLUTO_URL_HOST_MAX];

static const PlutoHttpCallbacks HC_NO_CBS;

// ── Small helpers ─────────────────────────────────────────────────────────────

static unsigned hc_now(void)
{
    if (s_clockfn) return s_clockfn();
    return (unsigned)s_pd->system->getCurrentTimeMilliseconds();
}

static const char* memfind(const char* hay, size_t hayLen, const char* needle,
                           size_t nLen)
{
    if (nLen == 0) return hay;
    if (hay == NULL || hayLen < nLen) return NULL;
    for (size_t i = 0; i + nLen <= hayLen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nLen) == 0)
            return hay + i;
    return NULL;
}

// Lua plain tonumber(str): full-string decimal parse (whitespace allowed),
// optional sign, >= 1 digit. "12abc" fails.
static int luatonum10(const char* str, long* out)
{
    if (!str) return 0;
    const char* p = str;
    while (*p && isspace((unsigned char)*p)) p++;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

// Lua tonumber(trimmed, 16): full-string hex parse, optional leading '-'.
// "5x", "", "0x10" fail; " ff " -> 255; "-4" -> -4.
static int luatonum16(const char* str, size_t len, long* out)
{
    size_t i = 0;
    while (i < len && isspace((unsigned char)str[i])) i++;
    int neg = 0;
    if (i < len && (str[i] == '+' || str[i] == '-')) {
        neg = (str[i] == '-');
        i++;
    }
    if (i >= len) return 0;
    long v = 0;
    for (; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + d;
    }
    *out = neg ? -v : v;
    return 1;
}

static const char* neterr_name(PDNetErr err)
{
    switch (err) {
    case NET_OK: return "ok";
    case NET_NO_DEVICE: return "NET_NO_DEVICE";
    case NET_BUSY: return "NET_BUSY";
    case NET_WRITE_ERROR: return "NET_WRITE_ERROR";
    case NET_WRITE_BUSY: return "NET_WRITE_BUSY";
    case NET_WRITE_TIMEOUT: return "NET_WRITE_TIMEOUT";
    case NET_READ_ERROR: return "NET_READ_ERROR";
    case NET_READ_BUSY: return "NET_READ_BUSY";
    case NET_READ_TIMEOUT: return "NET_READ_TIMEOUT";
    case NET_READ_OVERFLOW: return "NET_READ_OVERFLOW";
    case NET_FRAME_ERROR: return "NET_FRAME_ERROR";
    case NET_BAD_RESPONSE: return "NET_BAD_RESPONSE";
    case NET_ERROR_RESPONSE: return "NET_ERROR_RESPONSE";
    case NET_RESET_TIMEOUT: return "NET_RESET_TIMEOUT";
    case NET_BUFFER_TOO_SMALL: return "NET_BUFFER_TOO_SMALL";
    case NET_UNEXPECTED_RESPONSE: return "NET_UNEXPECTED_RESPONSE";
    case NET_NOT_CONNECTED_TO_AP: return "NET_NOT_CONNECTED_TO_AP";
    case NET_NOT_IMPLEMENTED: return "NET_NOT_IMPLEMENTED";
    case NET_CONNECTION_CLOSED: return "NET_CONNECTION_CLOSED";
    default: return "NET_UNKNOWN";
    }
}

static void set_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

// ── Header storage ────────────────────────────────────────────────────────────

static void clear_headers(void)
{
    sm_destroy(s_headers);
    s_headers = NULL;
    for (size_t i = 0; i < s_nHdrVals; i++) pluto_free(s_hdrVals[i]);
    s_nHdrVals = 0;
    for (size_t i = 0; i < s_nSetCookies; i++) pluto_free(s_setCookies[i]);
    s_nSetCookies = 0;
}

static void store_header(const char* key, size_t keyLen, const char* val,
                         size_t valLen)
{
    char lower[128];
    if (keyLen >= sizeof(lower)) keyLen = sizeof(lower) - 1;
    for (size_t i = 0; i < keyLen; i++)
        lower[i] = (char)tolower((unsigned char)key[i]);
    lower[keyLen] = '\0';

    char* v = (char*)pluto_malloc(valLen + 1);
    if (!v) return;
    memcpy(v, val, valLen);
    v[valLen] = '\0';

    if (strcmp(lower, "set-cookie") == 0) {
        if (s_nSetCookies < HC_MAX_SET_COOKIES) {
            s_setCookies[s_nSetCookies++] = v;
            return;
        }
    } else if (s_nHdrVals < HC_MAX_HDRS && s_headers) {
        sm_put(s_headers, lower, v); // map keeps latest value per key
        s_hdrVals[s_nHdrVals++] = v; // we free every stored value ourselves
        return;
    }
    pluto_free(v);
}

// ── Request building ──────────────────────────────────────────────────────────

static void build_request(const PlutoUrl* parsed, StrBuf* out)
{
    sb_printf(out, "GET %s HTTP/1.1\r\n", parsed->fullPath);
    // Lua rule verbatim (port 0 is truthy there): any port other than
    // 80/443 is shown — including the parse("") -> blank:0 quirk.
    if (parsed->port != 80 && parsed->port != 443)
        sb_printf(out, "Host: %s:%d\r\n", parsed->host, parsed->port);
    else
        sb_printf(out, "Host: %s\r\n", parsed->host);
    sb_append_str(out, "User-Agent: CometBrowser/1.0 (Playdate)\r\n");
    sb_append_str(out, "Accept: text/html,text/plain;q=0.8\r\n");
    sb_append_str(out, "Accept-Language: en-US,en;q=0.9\r\n");
    StrBuf cookie;
    sb_init(&cookie);
    cj_get_header(parsed->host, parsed->path, parsed->isSsl, &cookie);
    if (cookie.len > 0) {
        sb_append_str(out, "Cookie: ");
        sb_append(out, cookie.data, cookie.len);
        sb_append_str(out, "\r\n");
    }
    sb_free(&cookie);
    sb_append_str(out, "Connection: close\r\n");
    sb_append_str(out, "\r\n");
}

// ── Chunked decoding ──────────────────────────────────────────────────────────

// Returns a heap StrBuf with the decoded body, or NULL while incomplete /
// malformed (caller keeps buffering, exactly like the Lua version).
static StrBuf* decode_chunked(const char* str, size_t len)
{
    StrBuf* out = (StrBuf*)pluto_malloc(sizeof(StrBuf));
    if (!out) return NULL;
    sb_init(out);
    size_t pos = 0;
    for (;;) {
        const char* le = memfind(str + pos, len - pos, "\r\n", 2);
        if (!le) goto fail;
        size_t lineEnd = (size_t)(le - str);
        size_t sizeEnd = lineEnd;
        for (size_t i = pos; i < lineEnd; i++)
            if (str[i] == ';') { sizeEnd = i; break; }
        long size = 0;
        if (!luatonum16(str + pos, sizeEnd - pos, &size)) goto fail;
        pos = lineEnd + 2;
        if (size == 0) return out;
        if ((long)len < (long)pos + size + 2) goto fail;
        if (!sb_append(out, str + pos, (size_t)size)) goto fail;
        pos += (size_t)size + 2;
    }
fail:
    sb_free(out);
    pluto_free(out);
    return NULL;
}

// ── Header parsing ────────────────────────────────────────────────────────────

static void parse_headers(size_t hEnd)
{
    size_t headLen = hEnd; // bytes up to (not including) the first \r
    s_bodyStart = hEnd + 4;
    s_hasBodyStart = 1;

    clear_headers();
    s_headers = sm_create(16);

    // Status line: first [^\r\n] run, pattern "HTTP/%d+.%d+ (%d+)".
    size_t slEnd = headLen;
    for (size_t i = 0; i < headLen; i++)
        if (s_buf.data[i] == '\r' || s_buf.data[i] == '\n') { slEnd = i; break; }
    if (slEnd > 5 && strncmp(s_buf.data, "HTTP/", 5) == 0) {
        size_t i = 5;
        while (i < slEnd && isdigit((unsigned char)s_buf.data[i])) i++;
        if (i < slEnd && s_buf.data[i] == '.') {
            i++;
            while (i < slEnd && isdigit((unsigned char)s_buf.data[i])) i++;
            if (i < slEnd && s_buf.data[i] == ' ') {
                i++;
                long st = 0;
                size_t digits = 0;
                while (i < slEnd && isdigit((unsigned char)s_buf.data[i])) {
                    st = st * 10 + (s_buf.data[i] - '0');
                    i++;
                    digits++;
                    if (digits > 10) break;
                }
                if (digits > 0) s_status = (int)st;
            }
        }
    }

    // Header lines: split on '\n', virtual terminator at headLen (mirrors
    // gmatch(headPart .. "\n", "([^\n]+)\n")). Key capture is everything
    // after leading whitespace up to ':' INCLUDING trailing spaces (greedy
    // [^:]+); value trims both ends.
    size_t segStart = 0;
    for (size_t i = 0; i <= headLen; i++) {
        int atEnd = (i == headLen);
        if (!atEnd && s_buf.data[i] != '\n') continue;
        size_t s = segStart, e = i;
        segStart = i + 1;
        if (e <= s) continue; // ([^\n]+) requires >= 1 char
        while (s < e && isspace((unsigned char)s_buf.data[s])) s++; // ^%s*
        const char* colon =
            (const char*)memchr(s_buf.data + s, ':', e - s);
        if (!colon) continue;
        size_t colonOff = (size_t)(colon - s_buf.data);
        if (colonOff == s) continue; // ([^:]+) needs >= 1 char
        size_t v = colonOff + 1;
        while (v < e && isspace((unsigned char)s_buf.data[v])) v++;  // :%s*
        size_t ve = e;
        while (ve > v && isspace((unsigned char)s_buf.data[ve - 1])) ve--;
        store_header(s_buf.data + s, colonOff - s, s_buf.data + v, ve - v);
    }

    if (s_parsed.host[0] != '\0' && s_nSetCookies > 0)
        cj_process_set_cookies(s_parsed.host,
                               (const char* const*)s_setCookies,
                               s_nSetCookies);

    const char* te =
        s_headers ? (const char*)sm_get(s_headers, "transfer-encoding") : NULL;
    char teLow[64] = "";
    if (te) {
        size_t n = strlen(te);
        if (n >= sizeof(teLow)) n = sizeof(teLow) - 1;
        for (size_t i = 0; i < n; i++)
            teLow[i] = (char)tolower((unsigned char)te[i]);
        teLow[n] = '\0';
    }
    if (strstr(teLow, "chunked")) {
        s_chunked = 1;
        s_contentLength = -1;
    } else {
        const char* clStr = s_headers
                                ? (const char*)sm_get(s_headers,
                                                      "content-length")
                                : NULL;
        long cl;
        s_contentLength = luatonum10(clStr, &cl) ? cl : -1;
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

static void hc_open_cb(TCPConnection* conn, PDNetErr err, void* ud);
static void hc_closed_cb(TCPConnection* conn, PDNetErr err);
static int connect_now(void);

static void close_tcp(void)
{
    if (!s_conn) return;
    TCPConnection* conn = s_conn;
    s_conn = NULL;
    // Only close once open has resolved; a still-connecting socket is closed
    // by its own open callback when it notices it is stale (WX sim crash).
    if (s_connOpen || s_openFailed) s_tcp->close(conn);
}

static void reset(void)
{
    close_tcp();
    s_reqId++;
    s_cbs = HC_NO_CBS;
    s_url[0] = '\0';
    sb_clear(&s_buf);
    s_status = 200;
    clear_headers();
    s_bodyStart = 0;
    s_hasBodyStart = 0;
    s_chunked = 0;
    s_contentLength = -1;
    s_connOpen = 0;
    s_openFailed = 0;
    s_connClosed = 0;
    s_error[0] = '\0';
    s_state = HC_IDLE;
    s_aboutPending = 0;
    s_aboutPage = NULL;
    // Deliberately untouched (Lua parity): pendingRedirectUrl/-Callbacks,
    // redirectDepth, accessWaiting/grant cache.
}

static int connect_now(void)
{
    TCPConnection* conn =
        s_tcp->newConnection(s_parsed.host, s_parsed.port, s_parsed.isSsl != 0);
    if (!conn) return 0;
    s_conn = conn;
    s_tcp->setUserdata(conn, (void*)(uintptr_t)s_reqId);
    s_tcp->setConnectTimeout(conn, HC_CONNECT_TIMEOUT_MS);
    s_tcp->setReadTimeout(conn, HC_READ_TIMEOUT_MS);
    s_tcp->setReadBufferSize(conn, HC_SOCKET_BUFFER_BYTES);
    s_tcp->setConnectionClosedCallback(conn, hc_closed_cb);
    if (s_tcp->open(conn, hc_open_cb, NULL) != NET_OK) return 0;
    return 1;
}

static void connect_failed(void)
{
    PlutoHttpCallbacks cb = s_cbs;
    char msg[HC_ERR_MAX];
    snprintf(msg, sizeof(msg), "Could not open connection to %.200s",
             s_parsed.host);
    reset();
    if (cb.onError) cb.onError(cb.ud, msg);
}

static void access_cb(bool allowed, void* userdata)
{
    (void)userdata;
    if (!s_accessWaiting) return;
    s_accessWaiting = 0;
    if (s_reqId != s_accessReqId) return; // superseded while waiting
    if (allowed) {
        strncpy(s_grantHost, s_parsed.host, sizeof(s_grantHost) - 1);
        s_grantHost[sizeof(s_grantHost) - 1] = '\0';
        if (!connect_now()) connect_failed();
    } else {
        PlutoHttpCallbacks cb = s_cbs;
        char msg[HC_ERR_MAX];
        snprintf(msg, sizeof(msg), "Networking not available.");
        reset();
        if (cb.onError) cb.onError(cb.ud, msg);
    }
}

// Internal doGet(): mirrors core/http_client.lua doGet block for block.
static int do_get(const char* urlString, const PlutoHttpCallbacks* cbs)
{
    s_cbs = cbs ? *cbs : HC_NO_CBS;
    snprintf(s_url, sizeof(s_url), "%s", urlString ? urlString : "");
    s_state = HC_CONNECTING;
    s_startMs = hc_now();
    sb_clear(&s_buf);
    s_status = 200;
    clear_headers();
    s_bodyStart = 0;
    s_hasBodyStart = 0;
    s_chunked = 0;
    s_contentLength = -1;
    s_connOpen = 0;
    s_openFailed = 0;
    s_connClosed = 0;
    s_error[0] = '\0';

    // ── Internal about: pages ────────────────────────────────────────────
    if (strncmp(s_url, "about:", 6) == 0) {
        const HcInternalPage* page = hc_internal_page(s_url);
        if (page) {
            s_aboutPending = 1;
            s_aboutPage = page;
            s_aboutDueMs = hc_now() + HC_ABOUT_DELAY_MS;
        } else {
            PlutoHttpCallbacks cb = s_cbs;
            char msg[HC_ERR_MAX];
            snprintf(msg, sizeof(msg), "Unknown internal page: %.200s",
                     s_url);
            reset();
            if (cb.onError) cb.onError(cb.ud, msg);
        }
        return 1;
    }

    // ── Parse URL ────────────────────────────────────────────────────────
    url_parse(s_url, &s_parsed);
    if (s_parsed.host[0] == '\0') {
        PlutoHttpCallbacks cb = s_cbs;
        char msg[HC_ERR_MAX];
        snprintf(msg, sizeof(msg), "Invalid URL (no hostname): %.200s",
                 s_url);
        reset();
        if (cb.onError) cb.onError(cb.ud, msg);
        return 0;
    }

    // ── Network availability ─────────────────────────────────────────────
    if (!s_pd->network || !s_pd->network->tcp || !s_tcp) {
        PlutoHttpCallbacks cb = s_cbs;
        char msg[HC_ERR_MAX];
        snprintf(msg, sizeof(msg), "Networking not available.");
        reset();
        if (cb.onError) cb.onError(cb.ud, msg);
        return 0;
    }

    // ── Access gating (C-only; the Lua SDK did this inside tcp.new) ──────
    if (strcmp(s_grantHost, s_parsed.host) != 0) {
        enum accessReply ar =
            s_tcp->requestAccess(s_parsed.host, s_parsed.port,
                                 s_parsed.isSsl != 0, HC_NET_PURPOSE,
                                 access_cb, NULL);
        if (ar == kAccessAllow) {
            strncpy(s_grantHost, s_parsed.host, sizeof(s_grantHost) - 1);
            s_grantHost[sizeof(s_grantHost) - 1] = '\0';
        } else if (ar == kAccessDeny) {
            PlutoHttpCallbacks cb = s_cbs;
            char msg[HC_ERR_MAX];
            snprintf(msg, sizeof(msg), "Networking not available.");
            reset();
            if (cb.onError) cb.onError(cb.ud, msg);
            return 0;
        } else { // kAccessAsk: wait for access_cb
            s_accessWaiting = 1;
            s_accessReqId = s_reqId;
            return 1;
        }
    }

    if (!connect_now()) connect_failed();
    return hc_is_loading() ? 1 : 0;
}

// ── Delivery ──────────────────────────────────────────────────────────────────

// requestState == "done": slice the body, attempt chunked decode, hand
// everything to onSuccess after reset() (Lua order preserved).
static void deliver_done(void)
{
    PlutoHttpCallbacks cb = s_cbs;
    int st = s_status;
    char url[HC_URL_MAX];
    memcpy(url, s_url, sizeof(url));

    StrBuf body;
    sb_init(&body);
    size_t off = s_hasBodyStart ? s_bodyStart : 0;
    if (s_buf.len > off) sb_append(&body, s_buf.data + off, s_buf.len - off);
    if (s_chunked) {
        StrBuf* dec = decode_chunked(body.data, body.len);
        if (dec) {
            sb_free(&body);
            body = *dec;
            pluto_free(dec);
        }
    }

    // Take header ownership away so reset() cannot free what we deliver.
    StrMap* map = s_headers;
    s_headers = NULL;
    char* vals[HC_MAX_HDRS];
    size_t nVals = s_nHdrVals;
    memcpy(vals, s_hdrVals, nVals * sizeof(char*));
    s_nHdrVals = 0;

    reset();

    if (cb.onSuccess)
        cb.onSuccess(cb.ud, st, map, body.data ? body.data : "", body.len,
                     url);

    for (size_t i = 0; i < nVals; i++) pluto_free(vals[i]);
    sm_destroy(map);
    sb_free(&body);
}

// ── Update ────────────────────────────────────────────────────────────────────

void hc_update(void)
{
    // A deferred redirect (from a prior tick) opens the next connection now,
    // well after the previous connection was fully closed by us.
    if (s_hasPendingRedirect) {
        char u[HC_URL_MAX];
        memcpy(u, s_pendingUrl, sizeof(u));
        PlutoHttpCallbacks cb = s_pendingCbs;
        s_hasPendingRedirect = 0;
        s_pendingCbs = HC_NO_CBS;
        do_get(u, &cb);
        return;
    }

    if (s_state == HC_IDLE) return;

    // about-page timer (playdate.timer.performAfterDelay(20) parity).
    // Faithful ordering: progress -> success -> reset (a get() issued from
    // inside onSuccess is clobbered by this trailing reset, like in Lua).
    if (s_aboutPending && hc_now() >= s_aboutDueMs) {
        PlutoHttpCallbacks cb = s_cbs;
        const HcInternalPage* page = s_aboutPage;
        char url[HC_URL_MAX];
        memcpy(url, s_url, sizeof(url));
        if (cb.onProgress) cb.onProgress(cb.ud, 100, 100);
        if (cb.onSuccess) {
            StrMap* m = sm_create(4);
            char* v = pluto_strdup("text/html");
            if (m && v) {
                sm_put(m, "content-type", v);
                cb.onSuccess(cb.ud, 200, m, page->html, page->htmlLen, url);
            }
            pluto_free(v);
            sm_destroy(m);
        }
        reset();
        return;
    }

    unsigned now = hc_now();

    // Timeout watchdog
    if ((s_state == HC_CONNECTING || s_state == HC_READING) &&
        now - s_startMs > HC_REQUEST_TIMEOUT_MS) {
        if (s_buf.len > 512) {
            // Got some data — treat as done rather than fail silently
            s_state = HC_DONE;
        } else {
            set_error("Connection timed out after 60 seconds.");
            s_state = HC_ERROR;
        }
    }

    // Send the HTTP request once the connection is open. Written from a later
    // update frame (not inside the SDK open callback) so TLS settles first.
    if (s_state == HC_CONNECTING && s_connOpen && s_conn) {
        StrBuf req;
        sb_init(&req);
        build_request(&s_parsed, &req);
        int sent = s_tcp->write(s_conn, req.data, req.len);
        sb_free(&req);
        if (sent > 0) {
            s_state = HC_READING;
        } else if (sent == 0) {
            set_error("Send failed: ?");
            s_state = HC_ERROR;
        } else {
            set_error("Send failed: %s", neterr_name((PDNetErr)sent));
            s_state = HC_ERROR;
        }
    }

    // Pump incoming data. The staging buffer is static: the SDK update loop
    // is single-threaded and 32 KiB would blow the 60 KiB app stack.
    if (s_state == HC_READING && s_conn && s_connOpen) {
        static char pumpTmp[HC_READ_CHUNK];
        size_t avail = s_tcp->getBytesAvailable(s_conn);
        if (avail > 0) {
            size_t want = avail < HC_READ_CHUNK ? avail : HC_READ_CHUNK;
            int n = s_tcp->read(s_conn, pumpTmp, want);
            if (n > 0) {
                if (s_buf.len < HC_MAX_RESPONSE_SIZE)
                    sb_append(&s_buf, pumpTmp, (size_t)n);
                long tot = s_contentLength >= 0 ? s_contentLength : 0;
                long cur = 0;
                if (s_hasBodyStart) {
                    // Report body bytes only (buffer includes headers), and
                    // never overshoot the known total.
                    cur = (long)s_buf.len - (long)s_bodyStart;
                    if (cur < 0) cur = 0;
                    if (tot > 0 && cur > tot) cur = tot;
                }
                if (s_cbs.onProgress)
                    s_cbs.onProgress(s_cbs.ud, (int)cur, (int)tot);
            }
            // read failure: Lua inspected getError() and ignored it — same.
        }
    }

    // Parse headers once they've fully arrived, handle redirects in-band.
    if (s_state == HC_READING && !s_hasBodyStart) {
        const char* h = memfind(s_buf.data, s_buf.len, "\r\n\r\n", 4);
        if (h) {
            parse_headers((size_t)(h - s_buf.data));
            const char* loc = s_headers
                                  ? (const char*)sm_get(s_headers, "location")
                                  : NULL;
            if (s_status >= 300 && s_status < 400 && loc && loc[0]) {
                s_redirectDepth++;
                if (s_redirectDepth <= HC_MAX_REDIRECTS) {
                    StrBuf abs;
                    sb_init(&abs);
                    url_resolve(s_url, loc, &abs);
                    snprintf(s_pendingUrl, sizeof(s_pendingUrl), "%s",
                             abs.data ? abs.data : "");
                    sb_free(&abs);
                    s_pendingCbs = s_cbs;
                    s_hasPendingRedirect = 1;
                } else {
                    set_error("Too many redirects to %s", s_url);
                    s_state = HC_ERROR;
                }
                // Faithful quirk: this unconditional reset() closes the TCP
                // connection AND wipes errorMessage/state on the cap branch,
                // so an over-deep chain is silently dropped (p07 G truth).
                reset();
                return;
            }
        }
    }

    // Detect a complete body
    if (s_state == HC_READING && s_hasBodyStart) {
        long bodyBytes = (long)s_buf.len - (long)s_bodyStart;
        if (s_chunked) {
            StrBuf* dec =
                decode_chunked(s_buf.data + s_bodyStart,
                               s_buf.len - s_bodyStart);
            if (dec) {
                sb_free(dec);
                pluto_free(dec);
                s_state = HC_DONE;
            }
        } else if (s_contentLength >= 0 && bodyBytes >= s_contentLength) {
            s_state = HC_DONE;
        }
        if (s_buf.len >= HC_MAX_RESPONSE_SIZE) s_state = HC_DONE;
    }

    // Server closed the connection
    if (s_state == HC_READING && s_connClosed) {
        if (s_buf.len == 0) {
            set_error("Connection closed before any data was received.");
            s_state = HC_ERROR;
        } else {
            s_state = HC_DONE;
        }
    }

    // Handle completed request
    if (s_state == HC_DONE) {
        deliver_done();
        return;
    }
    if (s_state == HC_ERROR) {
        PlutoHttpCallbacks cb = s_cbs;
        char msg[HC_ERR_MAX];
        snprintf(msg, sizeof(msg), "%s",
                 s_error[0] ? s_error : "Connection failed.");
        reset();
        if (cb.onError) cb.onError(cb.ud, msg);
        return;
    }
}

// ── TCP callbacks ─────────────────────────────────────────────────────────────

static void hc_open_cb(TCPConnection* conn, PDNetErr err, void* ud)
{
    (void)ud;
    unsigned myId = (unsigned)(uintptr_t)s_tcp->getUserdata(conn);
    if (myId != s_reqId || conn != s_conn) {
        // Cancelled or superseded while still connecting; open has resolved,
        // so closing here is safe (see close_tcp comment).
        s_tcp->close(conn);
        return;
    }
    if (err != NET_OK) {
        s_openFailed = 1;
        set_error("Connection failed: %s", neterr_name(err));
        s_state = HC_ERROR;
        return;
    }
    s_connOpen = 1;
}

static void hc_closed_cb(TCPConnection* conn, PDNetErr err)
{
    (void)err;
    unsigned myId = (unsigned)(uintptr_t)s_tcp->getUserdata(conn);
    if (myId == s_reqId && conn == s_conn) s_connClosed = 1;
}

// ── Public API ────────────────────────────────────────────────────────────────

void hc_init(struct PlaydateAPI* pd)
{
    s_pd = pd;
    s_tcp = (pd && pd->network) ? pd->network->tcp : NULL;
    s_clockfn = NULL;
    sb_init(&s_buf);
    s_reqId = 1;
    s_state = HC_IDLE;
    s_grantHost[0] = '\0';
}

int hc_get(const char* urlStr, const PlutoHttpCallbacks* cbs)
{
    // Cancel any previous request cleanly.
    hc_cancel();
    // A fresh top-level request starts a new redirect chain and must not be
    // pre-empted by a stale redirect enqueued for the previous request.
    s_hasPendingRedirect = 0;
    s_pendingCbs = HC_NO_CBS;
    s_redirectDepth = 0;
    return do_get(urlStr, cbs);
}

int hc_is_loading(void)
{
    return s_state == HC_CONNECTING || s_state == HC_READING;
}

void hc_cancel(void) { reset(); }

void hc_set_tcp_for_tests(struct playdate_tcp* fake)
{
    s_tcp = fake ? fake : ((s_pd && s_pd->network) ? s_pd->network->tcp : NULL);
}

void hc_set_clock_fn(unsigned (*fn)(void)) { s_clockfn = fn; }
