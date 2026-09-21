/*
 * PlutoBrowser — jsext.h
 * External <script src> support for DOC_SCRIPT_FULL (JavaScript: "Full").
 *
 * Browser-faithful model: scan all <script> elements in document order,
 * resolve + dedupe src= URLs, prefetch the files (sequential fetches through
 * the single-flight HTTP client — exactly like the page body and images),
 * then let jsbridge execute every slot in page order — inline bodies and
 * fetched externals interleaved exactly as they appear on the page.
 *
 * Ownership: jsext_collect returns a scratch arena holding the slot list and
 * the JsExtScript array (bodies are arena copies made when each fetch
 * settles). jsext_prefetch_begin TRANSFERS arena ownership to the fetch
 * session; on success jsext_fetch_detach hands the arena + arrays to the
 * caller (main.c packs them into the render task; document_parse_ex stores
 * them on the DocParseResult; document_free frees them). jsext_prefetch_abort
 * (navigation away mid-prefetch) frees the arena + session instead.
 */
#ifndef PLUTO_JSEXT_H
#define PLUTO_JSEXT_H

#include <stddef.h>
#include "html/jsbridge.h"

/* ── Scratch arena (page-lifetime; SDK allocator, block list) ─────────────── */
typedef struct JsExtArena JsExtArena;
void jsext_arena_free(JsExtArena *a);

/* ── Scan + resolve (one pass over the page HTML) ────────────────────────────
 * Scans `html` into a slot list (document order) and an external-file table
 * (deduped by RAW src string, then resolved to ABSOLUTE URLs against
 * `pageUrl` — <base href> honored because the caller passes the resolved
 * page URL). Unresolvable / over-cap entries keep url[0]=='\0' and are
 * logged + skipped later. Returns the total slot count (uncapped). */
int jsext_collect(const char *html, const char *pageUrl,
                  JsScriptSlot **outSlots, int *outSlotCount,
                  JsExtScript **outExt, int *outExtCount,
                  JsExtArena **outArena);

/* ── Prefetch state machine (driven as a cooperative task step) ──────────────
 * Step: returns 1 while a fetch is in flight, 0 when all downloads settled.
 * One sequential http_get per unique external; each success is copied into
 * the arena and logged; failures (network error, non-2xx, over the
 * per-script byte cap, over the per-page budget) are logged and skipped —
 * the page still renders (user-approved failure policy). */
int jsext_prefetch_step(void *state);

/* Opaque session (created by jsext_prefetch_begin; owns the arena). */
typedef struct JsExtFetch JsExtFetch;

/* Override the per-page RAM-residency budget for the NEXT session (tests;
 * production leaves the JSBRIDGE_EXT_PAGE_BUDGET default). 0 restores it. */
void jsext_set_page_budget(size_t bytes);

/* Begin a session over the collected externals (takes ownership of `arena`
 * and the arrays). Returns NULL when there is nothing to fetch (extCount 0),
 * in which case ownership stays with the caller. */
JsExtFetch *jsext_prefetch_begin(JsExtArena *arena,
                                 JsScriptSlot *slots, int slotCount,
                                 JsExtScript *ext, int extCount);

/* Success: clear internal callback state + free the session shell WITHOUT
 * freeing the arena/arrays, and hand them back through the out-params (the
 * caller's Stage-1 stack copies are gone by the time downloads settle — the
 * session is the only owner across the task yield). */
void jsext_fetch_detach(JsExtFetch *f, JsExtArena **outArena,
                        JsExtScript **outExt, int *outExtCount);

/* Abort (navigation away mid-prefetch): cancels any in-flight script
 * request, frees the arena + session. Safe on an idle/NULL session. */
void jsext_prefetch_abort(JsExtFetch *f);

/* Total bytes fetched by the most recent session (diagnostics). */
size_t jsext_last_bytes(void);

/* Navigation/teardown hook: abort ANY active session (module-global lookup,
 * safe on none). Called from navigate_to/render_error so a cancelled render
 * task never leaves a dangling fetch session behind. */
void jsext_abort_active(void);

/* ── Local fill (about: pages — deterministic Full-mode tests) ─────────────
 * Fill `ext[0..extCount)` bodies for about: pages from the built-in table
 * (matched by file-name TAIL of the resolved src= URL; same per-script cap
 * + per-page budget + skip-and-log policy as network fetches). Returns
 * extCount; unfilled entries keep body==NULL. */
int jsext_local_fill(JsExtArena *arena, JsExtScript *ext, int extCount);

#endif /* PLUTO_JSEXT_H */
