/*
 * PlutoBrowser — http_client.h
 * Port of Source/core/http_client.lua (reference, 538 lines).
 *
 * Lua → C function map:
 *   HttpClient.get(url, cb)      → http_get()
 *   HttpClient.cancel()          → http_cancel()
 *   HttpClient.isLoading()       → http_is_loading()
 *   HttpClient.update()          → http_update()  (call once per frame)
 *   buildRequest/parseHeaders/decodeChunked/closeTcp/reset/doGet → static fns
 *   INTERNAL_PAGES (about:home/blank/acidtest) → static structs (verbatim HTML)
 *
 * Preserved semantics (verified against the reference):
 *   - Raw-TCP HTTP/1.1 GET over playdate->network->tcp (the Lua ref deliberately
 *     avoids playdate.network.http: it follows redirects internally and crashed
 *     the WX Simulator on 3xx). Redirects (max 5) are resolved in this module:
 *     on a 3xx with Location, the connection is closed and the next one opens
 *     on a LATER update tick (deferred, reference parity).
 *   - State machine: idle → connecting → (accessWait) → reading → done | error.
 *   - Request written from a later update frame after the open callback fires
 *     (never inside the callback — TLS must settle first).
 *   - Timeouts: connect/read 10 (Lua seconds → 10000 ms C), 60s request
 *     watchdog; >512 bytes buffered at timeout = done (partial content wins).
 *   - 2MB buffer cap; 32KB read chunks; 16KB SDK read buffer.
 *   - Chunked decoding: retry-until-complete (nil while incomplete).
 *   - Header parse on "\r\n\r\n": status line, case-lowered keys, multiple
 *     Set-Cookie → cookie_jar_process_set_cookies(host, list, count).
 *   - Stale-callback generation: requestId bumped on every reset; any SDK
 *     callback whose saved id != current is ignored (and closes its own
 *     connection once the open has resolved — never while still connecting,
 *     which crashed the WX Simulator).
 *   - about: pages answered locally after a 20ms timer (pdtimer), success path.
 *   - Errors surfaced via callbacks.onError; partial-content-on-timeout rule.
 *
 * C API mapping (pd_api_network.h + "Inside Playdate with C" §7.6):
 *   - tcp.new → pd->network->tcp->newConnection(host, port, usessl)
 *   - tcp:open(cb) → tcp->open(conn, TCPOpenCallback, ud)  [err code not bool]
 *   - tcp:write → tcp->write (bytes or negative PDNetErr; NET_WRITE_BUSY retry)
 *   - getBytesAvailable/read → tcp->getBytesAvailable / tcp->read
 *   - setConnectTimeout/setReadTimeout take MS in C (Lua took seconds)
 *   - C requires explicit requestAccess for HTTPS (Lua prompted implicitly):
 *     accessWait state added; the 60s watchdog deliberately does NOT cover it
 *     (a permission dialog must not kill the request — matches the Lua ref,
 *     whose runtime was paused while the dialog was up).
 */
#ifndef PLUTO_HTTP_CLIENT_H
#define PLUTO_HTTP_CLIENT_H

#include <stddef.h>
#include "pd_api.h"

/* Callbacks mirroring the Lua table: onSuccess(status, headers, body, bodyLen, url),
 * onError(msg), onProgress(cur, total). All optional (NULL allowed). */
typedef struct
{
    void (*onSuccess)(int status, char **headerKeys, char **headerVals,
                      int headerCount, const char *body, size_t bodyLen,
                      const char *url);
    void (*onError)(const char *message);
    void (*onProgress)(int cur, int total);
} HttpCallbacks;

/* Initialize (stores the API pointer). Call once at boot. */
void http_client_init(PlaydateAPI *pd);

/* Start a GET. Cancels any in-flight request first (Lua parity).
 * Returns 1 if a request was started (or answered internally), 0 on
 * immediate failure (onError already fired). */
int http_get(const char *urlString, const HttpCallbacks *callbacks);

/* Cancel any in-flight request (safe when idle). */
void http_cancel(void);

/* 1 while a request is connecting/reading. */
int http_is_loading(void);

/* Pump the state machine — call once per frame from the update loop. */
void http_update(void);

/* ── Internal about: page directory ─────────────────────────────────────────
 * Read-only name/title view of http_client's INTERNAL_PAGES table (same
 * order; html bodies stay internal). Single source of truth for UI that
 * lists the built-in pages — the home page's Test Cases section renders one
 * card per entry and navigates to entry.name. */
typedef struct
{
    const char *name;  /* navigable URL, e.g. "about:javascript" */
    const char *title; /* card label, e.g. "JavaScript Test Suite" */
} HttpTestPage;

/* Fill *entries with the internal-page directory; returns the entry count
 * (currently 5: home, blank, acidtest, javascript, jsext). */
int http_test_pages(const HttpTestPage **entries);

#endif /* PLUTO_HTTP_CLIENT_H */
