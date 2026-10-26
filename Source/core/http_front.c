/*
 * PlutoBrowser — http_front.c
 * The dual-stack FRONT DISPATCHER (R62). Owns the public http_* names:
 * every caller (main.c, jsbridge.c, jsext.c, …) keeps calling the same
 * functions; this file reads the Settings "Network Protocol" row
 * (storage netProtocol: 0=TCP raw — the standing default, 1=OS HTTP)
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

int pluto_http_route_request(const char *method, const char *urlString,
                             const char *contentType,
                             const HttpHeaderPair *extra, int nExtra,
                             const char *body, size_t bodyLen,
                             const HttpCallbacks *callbacks)
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
        return oshttp_request_h(method, urlString, contentType, extra,
                                nExtra, body, bodyLen, callbacks);
    }
    return http_tcp_request_h(method, urlString, contentType, extra,
                              nExtra, body, bodyLen, callbacks);
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
