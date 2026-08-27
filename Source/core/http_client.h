// http_client.h — Multi-protocol HTTP/HTTPS client for PlutoBrowser.
//
// Supports two networking backends:
//  1. Native HTTP API (pd->network->http): preferred for HTTP/HTTPS. Handles
//     TLS, HTTP request formatting, status/header parsing internally.
//  2. Raw TCP API (pd->network->tcp): fallback for HTTP/HTTPS if the HTTP API
//     is unavailable or fails. Also used for non-HTTP TCP connections.
//
// Protocol selection: HTTP/HTTPS URLs attempt the native HTTP API first; if
// unavailable or the connection fails, falls back to raw TCP. The browser
// receives responses through the same PlutoHttpCallbacks interface regardless
// of which backend succeeded — the protocol is an implementation detail.
//
// Behavior parity notes (verified against the Lua original via p07_oracle):
//  - One request at a time; hc_get() cancels any previous one.
//  - Redirects (3xx + Location) resolve via url_resolve() and are deferred to
//    the NEXT update tick; up to 5 follows per top-level request.
//  - Content-Length completion delivers everything after the header block,
//    even bytes beyond the declared length (slice-to-end quirk).
//  - Chunked bodies decode only once complete; an incomplete stream at close
//    delivers the RAW undecoded chunked text.
//  - Timeout watchdog: >60 s with >512 buffered bytes completes instead of
//    erroring; otherwise "Connection timed out after 60 seconds."
//  - about:home/blank/acidtest served locally ~20 ms after get(); unknown
//    about:* pages fail synchronously but still return 1 from hc_get().
//  - Header keys lowercased; Set-Cookie excluded from the map and forwarded
//    to the cookie jar. Progress fires per read chunk.
//  - Network access gating via requestAccess() with a session grant cache
//    keyed by host.

#ifndef PLUTO_HTTP_CLIENT_H
#define PLUTO_HTTP_CLIENT_H

#include <stddef.h>

#include "../util/strmap.h"

struct PlaydateAPI;
struct playdate_tcp;
struct playdate_http;

#ifdef __cplusplus
extern "C" {
#endif

// Protocol backend selection (used internally; not exposed to browser.c).
enum HcBackend {
    HC_BACKEND_HTTP = 0,  // native HTTP API (preferred for HTTP/HTTPS)
    HC_BACKEND_TCP  = 1,  // raw TCP API (fallback)
};

typedef struct PlutoHttpCallbacks {
    void* ud;
    void (*onProgress)(void* ud, int cur, int tot);
    // headers/body/finalUrl are borrowed and valid only during the call.
    void (*onSuccess)(void* ud, int status, const StrMap* headers,
                      const char* body, size_t bodyLen,
                      const char* finalUrl);
    void (*onError)(void* ud, const char* msg);
} PlutoHttpCallbacks;

void hc_init(struct PlaydateAPI* pd);

// Starts a request, cancelling any previous one. Returns 1 when accepted
// (like the Lua original this can still be 1 for URLs that later fail via
// onError); 0 means synchronous rejection (onError already fired).
int  hc_get(const char* urlStr, const PlutoHttpCallbacks* cbs);

int  hc_is_loading(void);
void hc_cancel(void);
void hc_update(void);   // call once per frame

// Returns a short label for the active backend: "TCP", "HTTP", or "HTTPS".
const char* hc_backend_label(void);

// Test hooks: swap the HTTP/TCP vtable / clock. NULL restores defaults.
// The fake vtable must stay alive while installed.
void hc_set_http_for_tests(struct playdate_http* fake);
void hc_set_tcp_for_tests(struct playdate_tcp* fake);
void hc_set_clock_fn(unsigned (*fn)(void)); // milliseconds like getCurrentTimeMilliseconds
void hc_restore_http_api(void); // restores real HTTP API after selftests

#ifdef __cplusplus
}
#endif

#endif // PLUTO_HTTP_CLIENT_H
