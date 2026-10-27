/*
 * PlutoBrowser — http_front.c
 * The dual-stack FRONT DISPATCHER (R62). Owns the public http_* names:
 * every caller (main.c, jsbridge.c, jsext.c, …) keeps calling the same
 * functions; this file reads the Settings "Network Protocol" row
 * (storage netProtocol: 0=TCP raw, 1=OS HTTP, 2=HYBRID — the default
 * since R64: pages ride raw TCP, layout images ride the OS HTTP stack)
 * and routes each request to the selected backend.
 *
 *   TCP backend: Source/core/http_client.c (compiled as http_tcp_* via
 *                the header's rename macros — the file's SOURCE is
 *                byte-untouched, the R61-proven machine).
 *   OS backend:  Source/core/http_os.c (playdate.network.http).
 *
 * about: URLs ALWAYS ride the TCP implementation — internal pages are
 * answered locally with zero wire, on either setting.
 *
 * In-flight requests keep THEIR backend (both pumps run every frame);
 * the setting only routes NEW requests — a flip mid-load can never
 * strand a connection.
 *
 * ── HYBRID (R63) ─────────────────────────────────────────────────────────
 * netProtocol=2 splits BY REQUEST KIND, not by URL sniffing: page/document
 * traffic (HTML, CSS, scripts, XHR/fetch, form posts — every caller that
 * keeps using the generic http_* names) rides the raw-TCP stack — the
 * reliable machine; the DISPLAY-IMAGE lane rides the OS HTTP stack — the
 * fast machine. Only the image pipeline's one fetch site is told apart:
 * render/image_decoder.c fetches through http_get_image() (a kind-carrying
 * front door), and this dispatcher consults the kind ONLY while Hybrid is
 * selected. On netProtocol 0/1 (and for about: URLs in every mode) the
 * kind is ignored — routing is byte-identical to stock. NEITHER backend's
 * process flow is touched: TCP stays the receipted single-flight machine,
 * OS HTTP stays the receipted stack — Hybrid merely routes BETWEEN them.
 * Under Hybrid both backends stay idle-or-busy exactly as the image queue
 * already gates (process_next refuses to start while http_is_loading() is
 * true over BOTH pumps), so at most one request is ever on the wire.
 */
#include <string.h>

#include "core/http_client.h"
#include "core/http_tcp_impl.h"
#include "core/http_os.h"
#include "core/storage.h"
#include "core/logger.h"

/* http_tcp_impl.h's rename macros are still active here (the wrapper TU
 * needs them for its WHOLE compilation; this file got them too via the
 * shared rename header) — this file's own DEFINITIONS ride the PUBLIC
 * names, so lift the macros first. The http_tcp_* declarations (renamed
 * in the impl header) stay visible for the routing calls below. */
#undef http_client_init
#undef http_get
#undef http_request
#undef http_request_ct
#undef http_get_headers
#undef http_request_h
#undef http_set_identity_encoding
#undef http_cancel
#undef http_is_loading
#undef http_update

/* Request kinds (R63). GENERIC = page/document/script/CSS/XHR traffic;
 * IMAGE = the layout display-image pipeline's fetch. Only the Hybrid
 * setting consults the kind. */
#define PLUTO_HTTP_ROUTE_GENERIC 0
#define PLUTO_HTTP_ROUTE_IMAGE 1

/* R64: the front door OWNS the "return 0 ⇒ onError already fired"
 * contract (http_client.h). The OS backend (stock, byte-untouched)
 * may refuse BEFORE arming its request (url/parse OOM, empty target,
 * body OOM) and returns 0 WITHOUT callbacks — the dispatch glue fires
 * the error here so every caller's immediate-refusal handling (the
 * image queue's negative cache; the page's error page) always lands,
 * and the queue never re-hammers a refused URL (receipted: 140
 * identical parse-fail retries with no negative entry). */
static int oshttp_route_or_refuse(const char *method, const char *urlString,
                                  const char *contentType,
                                  const HttpHeaderPair *extra, int nExtra,
                                  const char *body, size_t bodyLen,
                                  const HttpCallbacks *callbacks)
{
    int rc = oshttp_request_h(method, urlString, contentType, extra,
                              nExtra, body, bodyLen, callbacks);
    if (rc == 0)
    {
        logger_log("[http] OS route refused (onError fired) full=%.140s len=%zu",
                   urlString ? urlString : "", urlString ? strlen(urlString) : 0);
        if (callbacks && callbacks->onError)
        {
            callbacks->onError("OS-HTTP request refused (no wire started).");
        }
    }
    return rc;
}

int pluto_http_route_request_kind(const char *method, const char *urlString,
                                  const char *contentType,
                                  const HttpHeaderPair *extra, int nExtra,
                                  const char *body, size_t bodyLen,
                                  const HttpCallbacks *callbacks, int kind)
{
    int proto = storage_setting_int("netProtocol");
    if (!urlString || strncmp(urlString, "about:", 6) == 0)
    {
        /* internal pages: answered locally (zero wire) — always TCP */
        return http_tcp_request_h(method, urlString, contentType, extra,
                                  nExtra, body, bodyLen, callbacks);
    }
    if (proto == 1)
    {
        logger_log("[http] route=OS-HTTP %.70s", urlString);
        return oshttp_route_or_refuse(method, urlString, contentType, extra,
                                      nExtra, body, bodyLen, callbacks);
    }
    if (proto == 2)
    {
        /* HYBRID: image fetches ride the OS HTTP stack, everything else
         * rides the raw-TCP stack. about: never reaches here (the early
         * return above already answered it on TCP). */
        if (kind == PLUTO_HTTP_ROUTE_IMAGE)
        {
            logger_log("[http] route=Hybrid-OSIMAGE %.70s", urlString);
            return oshttp_route_or_refuse(method, urlString, contentType,
                                          extra, nExtra, body, bodyLen,
                                          callbacks);
        }
        logger_log("[http] route=Hybrid-TCP %.70s", urlString);
        return http_tcp_request_h(method, urlString, contentType, extra,
                                  nExtra, body, bodyLen, callbacks);
    }
    return http_tcp_request_h(method, urlString, contentType, extra,
                              nExtra, body, bodyLen, callbacks);
}

int pluto_http_route_request(const char *method, const char *urlString,
                             const char *contentType,
                             const HttpHeaderPair *extra, int nExtra,
                             const char *body, size_t bodyLen,
                             const HttpCallbacks *callbacks)
{
    /* GENERIC kind: on TCP/OS-HTTP settings the kind is ignored, so the
     * stock routes are unchanged; only Hybrid distinguishes lanes. */
    return pluto_http_route_request_kind(method, urlString, contentType,
                                         extra, nExtra, body, bodyLen,
                                         callbacks, PLUTO_HTTP_ROUTE_GENERIC);
}

void http_client_init(PlaydateAPI *pd)
{
    http_tcp_init(pd);
    oshttp_init(pd);
}

int http_get(const char *urlString, const HttpCallbacks *callbacks)
{
    return pluto_http_route_request(NULL, urlString, NULL, NULL, 0, NULL, 0,
                                    callbacks);
}

int http_request(const char *method, const char *urlString,
                 const char *body, size_t bodyLen,
                 const HttpCallbacks *callbacks)
{
    return pluto_http_route_request(method, urlString, NULL, NULL, 0, body,
                                    bodyLen, callbacks);
}

int http_request_ct(const char *method, const char *urlString,
                    const char *contentType, const char *body, size_t bodyLen,
                    const HttpCallbacks *callbacks)
{
    return pluto_http_route_request(method, urlString, contentType, NULL, 0,
                                    body, bodyLen, callbacks);
}

int http_get_headers(const char *urlString, const HttpHeaderPair *extra,
                     int nExtra, const HttpCallbacks *callbacks)
{
    return pluto_http_route_request(NULL, urlString, NULL, extra, nExtra,
                                    NULL, 0, callbacks);
}

/* R63: the display-image lane's front door. Same GET contract as
 * http_get (same callbacks struct, same cancel-first single-flight
 * discipline, body never cached as HTML); ONLY the routing hint
 * differs. The kind is consulted by the dispatcher ONLY under the
 * Hybrid setting — on TCP (0) and OS HTTP (1) this is byte-identical
 * to http_get. */
int http_get_image(const char *urlString, const HttpCallbacks *callbacks)
{
    return pluto_http_route_request_kind(NULL, urlString, NULL, NULL, 0,
                                         NULL, 0, callbacks,
                                         PLUTO_HTTP_ROUTE_IMAGE);
}

int http_request_h(const char *method, const char *urlString,
                   const char *contentType, const HttpHeaderPair *extra,
                   int nExtra, const char *body, size_t bodyLen,
                   const HttpCallbacks *callbacks)
{
    return pluto_http_route_request(method, urlString, contentType, extra,
                                    nExtra, body, bodyLen, callbacks);
}

void http_set_identity_encoding(int on)
{
    /* the TCP backend's builder flag; the OS backend requests identity
     * unconditionally (v1 policy, see http_os.c) */
    http_tcp_set_identity_encoding(on);
}

void http_cancel(void)
{
    http_tcp_cancel();
    oshttp_cancel(); /* safe when idle */
}

int http_is_loading(void)
{
    return http_tcp_is_loading() || oshttp_is_loading();
}

void http_update(void)
{
    http_tcp_update();
    oshttp_update();
}
