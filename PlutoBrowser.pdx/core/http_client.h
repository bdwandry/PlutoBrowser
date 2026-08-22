// http_client.h — raw TCP HTTP/HTTPS client (C port of core/http_client.lua).
//
// Behavior parity notes (verified against the Lua original via p07_oracle):
//  - One request at a time; hc_get() cancels any previous one.
//  - The request bytes are written from hc_update() on a frame AFTER the open
//    callback, never from inside it.
//  - Redirects (3xx + Location) resolve via url_resolve() and are deferred to
//    the NEXT update tick; up to 5 follows per top-level request. Faithful
//    bug kept: when the cap trips, the error branch is immediately followed
//    by reset(), which wipes state AND message — nothing is ever delivered.
//  - Content-Length completion delivers everything after the header block,
//    even bytes beyond the declared length (slice-to-end quirk).
//  - Chunked bodies decode only once complete; an incomplete stream at close
//    delivers the RAW undecoded chunked text. Size lines are full-string hex
//    parses (whitespace-trimmed; "5x" fails), extensions after ';' ignored.
//  - Timeout watchdog: >60 s with >512 buffered bytes completes instead of
//    erroring; otherwise "Connection timed out after 60 seconds."
//  - about:home/blank/acidtest served locally ~20 ms after get(); unknown
//    about:* pages fail synchronously but still return 1 from hc_get().
//  - Header keys lowercased; Set-Cookie excluded from the map and forwarded
//    to the cookie jar. Progress fires per read chunk: (0,0) until headers
//    parsed, then body bytes clamped to the known total.
//  - C-only addition required by the SDK's raw TCP surface: network access
//    gating via requestAccess() with a session grant cache keyed by host.

#ifndef PLUTO_HTTP_CLIENT_H
#define PLUTO_HTTP_CLIENT_H

#include <stddef.h>

#include "../util/strmap.h"

struct PlaydateAPI;
struct playdate_tcp;

#ifdef __cplusplus
extern "C" {
#endif

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

// Test hooks: swap the TCP vtable / clock. NULL restores defaults.
// The fake vtable must stay alive while installed.
void hc_set_tcp_for_tests(struct playdate_tcp* fake);
void hc_set_clock_fn(unsigned (*fn)(void)); // milliseconds like getCurrentTimeMilliseconds

#ifdef __cplusplus
}
#endif

#endif // PLUTO_HTTP_CLIENT_H
