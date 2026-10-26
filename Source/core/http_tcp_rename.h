/*
 * PlutoBrowser — http_tcp_rename.h
 * R62 dual-stack plumbing: the compile-time symbol rename that maps the
 * RAW-TCP client's public names (Source/core/http_client.c) to its
 * backend names (http_tcp_*).
 *
 * WHO DEFINES PLUTO_COMPILE_TCP_BACKEND: ONLY Source/core/
 * http_tcp_backend.c — the wrapper TU that includes the .c file. The
 * macros arm ONLY under that guard, so:
 *   - the wrapper's compilation renames the whole TCP machine
 *     (definitions + internal calls, consistently)
 *   - every OTHER file (callers, http_front.c) sees the macros as inert
 *     no-ops and uses the public http_* front (http_front.c)
 * Source/core/http_client.c is INCLUDED (not edited) by the wrapper —
 * its source text stays byte-untouched.
 */
#ifndef PLUTO_HTTP_TCP_RENAME_H
#define PLUTO_HTTP_TCP_RENAME_H

#ifdef PLUTO_COMPILE_TCP_BACKEND
#define http_client_init http_tcp_init
#define http_get http_tcp_get
#define http_request http_tcp_request
#define http_request_ct http_tcp_request_ct
#define http_get_headers http_tcp_get_headers
#define http_request_h http_tcp_request_h
#define http_set_identity_encoding http_tcp_set_identity_encoding
#define http_cancel http_tcp_cancel
#define http_is_loading http_tcp_is_loading
#define http_update http_tcp_update
#endif

#endif /* PLUTO_HTTP_TCP_RENAME_H */
