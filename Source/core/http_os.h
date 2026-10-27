/*
 * PlutoBrowser — http_os.h
 * The OS-HTTP network backend (R62): the playdate.network.http stack —
 * the same higher-level code path Panic's own system apps ride — behind
 * the browser's standing HttpCallbacks contract.
 *
 * Selected by the Settings "Network Protocol" row (storage netProtocol:
 * 0=TCP raw, 1=OS HTTP, 2=Hybrid default — pages TCP, layout images
 * OS HTTP). The dispatcher lives in
 * Source/core/http_front.c; the raw-TCP implementation
 * (Source/core/http_client.c, compiled as http_tcp_*) is untouched.
 *
 * CONTRACT (identical to the TCP backend):
 *   - one request at a time (single-flight; a new request cancels the
 *     in-flight one, generation guard kills stale callbacks)
 *   - http_callbacks: onSuccess / onSuccessSpill (disk delivery, the
 *     receiver adopts the handle) / onError / onProgress
 *   - big bodies stream to disk (SW2b semantics preserved: bounded read
 *     windows from the OS stack → pluto_spill_write; the bulk never
 *     materializes in RAM), small bodies deliver in RAM
 *   - Set-Cookie pairs feed the shared cookie jar; requests carry the
 *     jar's Cookie header
 *
 * DOCUMENTED v1 DIFFERENCES vs the TCP backend (honest, receipted):
 *   - 3xx redirects are the OS's business (the stack follows them
 *     internally); the TCP backend's 307/308 verb-preservation lane does
 *     not run here.
 *   - No wire response-cache / 304-revalidation lane (the page SNAPSHOT
 *     cache above this layer is unaffected).
 *   - v1 always requests identity encoding (raw bytes) — the OS stack's
 *     gzip behavior is an open receipt; a gzipped response is delivered
 *     raw with a warning log rather than half-decoded.
 *   - about: URLs never reach this backend (the front answers them
 *     locally through the TCP implementation).
 */
#ifndef PLUTO_HTTP_OS_H
#define PLUTO_HTTP_OS_H

#include "pd_api.h"
#include "http_client.h"

void oshttp_init(PlaydateAPI *pd);
int oshttp_request_h(const char *method, const char *urlString,
                     const char *contentType, const HttpHeaderPair *extra,
                     int nExtra, const char *body, size_t bodyLen,
                     const HttpCallbacks *callbacks);
void oshttp_cancel(void);
int oshttp_is_loading(void);
void oshttp_update(void);

#endif /* PLUTO_HTTP_OS_H */
