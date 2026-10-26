/*
 * PlutoBrowser — http_tcp_impl.h
 * R62 dual-stack plumbing: the RAW-TCP backend's declaration view for
 * http_front.c (the ONLY consumer besides the backend's own wrapper).
 *
 * The wrapper (http_tcp_backend.c) defines PLUTO_COMPILE_TCP_BACKEND and
 * includes http_client.c — the rename macros (http_tcp_rename.h) map the
 * machine's public symbols to the http_tcp_* names declared here.
 * http_front.c includes this header for the routing calls; its own
 * #undef (after the include) keeps the macros off its own definitions.
 */
#ifndef PLUTO_HTTP_TCP_IMPL_H
#define PLUTO_HTTP_TCP_IMPL_H

#include "core/http_tcp_rename.h"
#include "core/http_client.h"

void http_tcp_init(PlaydateAPI *pd);
int http_tcp_get(const char *urlString, const HttpCallbacks *callbacks);
int http_tcp_request(const char *method, const char *urlString,
                     const char *body, size_t bodyLen,
                     const HttpCallbacks *callbacks);
int http_tcp_request_ct(const char *method, const char *urlString,
                        const char *contentType, const char *body,
                        size_t bodyLen, const HttpCallbacks *callbacks);
int http_tcp_get_headers(const char *urlString, const HttpHeaderPair *extra,
                         int nExtra, const HttpCallbacks *callbacks);
int http_tcp_request_h(const char *method, const char *urlString,
                       const char *contentType, const HttpHeaderPair *extra,
                       int nExtra, const char *body, size_t bodyLen,
                       const HttpCallbacks *callbacks);
void http_tcp_set_identity_encoding(int on);
void http_tcp_cancel(void);
int http_tcp_is_loading(void);
void http_tcp_update(void);

#endif /* PLUTO_HTTP_TCP_IMPL_H */
