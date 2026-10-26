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
 *
 * ══ R62: DUAL-STACK ROUTING (TCP | OS HTTP) ══════════════════════════════
 * THIS HEADER IS THE PUBLIC CALLER VIEW: the http_* functions below are
 * the FRONT DISPATCHER (Source/core/http_front.c) — the Settings
 * "Network Protocol" row (storage netProtocol: 0=TCP raw, the standing
 * default; 1=OS HTTP stack) routes every request to its backend:
 *
 *   TCP backend: Source/core/http_client.c — the standing, receipted
 *     implementation. Its SOURCE stays byte-untouched: it is compiled
 *     through Source/core/http_tcp_backend.c (a wrapper TU that defines
 *     PLUTO_COMPILE_TCP_BACKEND then includes the .c file), and the
 *     rename macros in Source/core/http_tcp_rename.h map its public
 *     symbols to http_tcp_* for that one compilation. Callers and
 *     http_front.c see the http_tcp_* declarations through
 *     Source/core/http_tcp_impl.h.
 *
 * In-flight requests keep THEIR backend (both pumps run every frame);
 * the setting routes NEW requests only. about: URLs are
 * network-independent (answered locally) and ride the TCP
 * implementation on either setting.
 */
#ifndef PLUTO_HTTP_CLIENT_H
#define PLUTO_HTTP_CLIENT_H

#include <stddef.h>
#include "pd_api.h"
#include "pluto_spill.h" /* SpillFile (R15 disk delivery) */

/* Callbacks mirroring the Lua table: onSuccess(status, headers, body, bodyLen, url),
 * onError(msg), onProgress(cur, total). All optional (NULL allowed).
 * R15: onSuccessSpill — DISK delivery. When the raw body streamed to disk,
 * the client hands over a pure-body spill HANDLE (body materializes only
 * through pluto_spill_read windows) instead of a body-sized RAM buffer.
 * Ownership: the receiver ADOPTS the handle (must discard/reset it later);
 * if disk delivery is impossible (gzip staging, spill failure) the client
 * falls back to onSuccess with the materialized RAM body. */
typedef struct
{
    void (*onSuccess)(int status, char **headerKeys, char **headerVals,
                      int headerCount, const char *body, size_t bodyLen,
                      const char *url);
    void (*onSuccessSpill)(int status, SpillFile spill, size_t bodyLen,
                           const char *url);
    void (*onError)(const char *message);
    void (*onProgress)(int cur, int total);
} HttpCallbacks;

/* Initialize (stores the API pointer). Call once at boot — inits BOTH
 * backends; the routing setting picks per request. */
void http_client_init(PlaydateAPI *pd);

/* Start a GET. Cancels any in-flight request first (Lua parity).
 * Returns 1 if a request was started (or answered internally), 0 on
 * immediate failure (onError already fired). */
int http_get(const char *urlString, const HttpCallbacks *callbacks);

/* C4 (fetch/POST): start a request with an explicit METHOD and optional
 * REQUEST body. The body bytes are copied (caller keeps ownership);
 * method defaults to GET when NULL/empty. Same cancel-first contract
 * as http_get; the response path is identical, so callbacks are the
 * same struct. Content-Type is set internally (application/json) —
 * the only send format the page's send handler posts today. */
int http_request(const char *method, const char *urlString,
                 const char *body, size_t bodyLen,
                 const HttpCallbacks *callbacks);
/* Same front door with the caller's POST Content-Type (NULL/"" keeps the
 * historical "application/json" — used by the native form fallback for
 * standard urlencoded action posts). */
int http_request_ct(const char *method, const char *urlString,
                    const char *contentType, const char *body, size_t bodyLen,
                    const HttpCallbacks *callbacks);

/* ── NetP2: the extra-header wire lane ────────────────────────────────────
 * Pages set real request headers via setRequestHeader (the fetch shim's
 * `headers:` init object rides the same road). Every pair rides the wire
 * byte-for-byte beside the engine's own base headers. The pair bytes are
 * COPIED at call time (caller may hold transient storage), validated
 * (CRLF/control chars and the wire-integrity set are refused with a log
 * — never invented, never silently dropped), and re-armed across
 * redirect hops (browser-true: a page's headers follow the request).
 * `Range` requests by policy never serve/revalidate from a whole-body
 * cache entry — byte-accurate segments come from the wire. */
#define HTTP_EXTRA_HEADERS_MAX 6
typedef struct HttpHeaderPair
{
    char name[44];
    char value[240];
} HttpHeaderPair;

/* GET with the caller's extra wire headers (posted pages use
 * http_request_h). NULL extra or nExtra<=0 = byte-identical to http_get. */
int http_get_headers(const char *urlString, const HttpHeaderPair *extra,
                     int nExtra, const HttpCallbacks *callbacks);
/* General front door: METHOD + Content-Type + extra wire headers + body.
 * Frozen callers (http_get/http_request_ct) delegate here with zero
 * extras and keep byte-identical request behavior. */
int http_request_h(const char *method, const char *urlString,
                   const char *contentType, const HttpHeaderPair *extra,
                   int nExtra, const char *body, size_t bodyLen,
                   const HttpCallbacks *callbacks);

/* R15 + Compression setting: the request builder sends Accept-Encoding:
 * identity (raw bytes; reliable on weak radio) when the stored
 * encodingMode is Off (default), gzip when the user picks Gzip in
 * Settings. Kept only for API compatibility with existing callers
 * (jsext script fetches) — the builder reads the setting live instead.
 * Gzip RESPONSE decoding stays as a fallback for servers/proxies that
 * compress unasked. */
void http_set_identity_encoding(int on);

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
 * card per entry and navigates to entry.name. Network-independent (the
 * pages are answered locally on either protocol setting). */
typedef struct
{
    const char *name;  /* navigable URL, e.g. "about:javascript" */
    const char *title; /* card label, e.g. "JavaScript Test Suite" */
} HttpTestPage;

/* Fill *entries with the internal-page directory; returns the entry count
 * (currently 5: home, blank, acidtest, javascript, jsext). */
int http_test_pages(const HttpTestPage **entries);

/* Raw HTML body of an internal about: page (NULL when unknown). Single
 * source of truth with INTERNAL_PAGES — the JS XMLHttpRequest layer
 * answers about: requests from here without touching the network.
 * Returns the byte length (0 with *bodyOut NULL when not found). */
size_t http_internal_page_body(const char *url, const char **bodyOut);

#endif /* PLUTO_HTTP_CLIENT_H */
