/* webstorage.h — PlutoBrowser engine-side localStorage/sessionStorage.
 *
 * API-P7: the browser's key/value stores for page scripts, on all
 * engines. Browser shape: STRING keys map to STRING values (arguments
 * are coerced), getItem misses return null, length is a live count,
 * key(i) returns the i-th key or null, clear() empties the current
 * origin's store only, and setItem() fails (quota) beyond 12KB per
 * origin — the same DOMException semantic browsers report, sized for
 * this device rather than the desktop's 5MB. The 8KB value ceiling fits
 * the website's own GitHub payload cache.
 *
 * Scope (the browser's origin rule, adapted for this UA — documented
 * for the whole engine, never per-site):
 *   http(s)://authority  → per-authority store (scheme/host[:port])
 *   file://...           → one local-file origin ("file://"), matching
 *                          WebKit/Chrome's single file:-origin bucket
 *   about:*              → per-URL store — these are the UA's own
 *                          house pages (an extension-page-analogous
 *                          grant, isolated per URL so one internal page
 *                          never reads another's keys)
 *   data:/unknown/empty  → "null" opaque origin: reads run empty and
 *                          setItem quota-fails (browsers throw
 *                          SecurityError on these; the empty+quota road
 *                          is the honest device-shaped refusal)
 *   >WS_MAX_ORIGINS origins touched in one browser run → overflow URLs
 *   bind to the same overflow-fail store (accesses run empty,
 *   setItem quota-fails — documented boundary); previously-saved file
 *   sections stay on disk and load on a later run
 *
 * localStorage persists across browser runs in one file in the game's
 * Data sandbox (the same playdate->file road storage.c uses, so it
 * works on the simulator AND the device unmodified):
 *
 *   @<origin>
 *   <key>\t<escaped value>
 *   ...
 *
 * Values escape backslash, newline, CR and TAB; parse reverses it.
 * Only the BOUND origin's section is resident (≤ 12KB total budget);
 * on save the other origins' sections stream through a raw stash, so
 * the file stays the union of all origins seen this run.
 *
 * sessionStorage lives in memory only for the browser run (the
 * single-tab session model: survives reloads + same-tab navigations,
 * dies with the app).
 */

#ifndef PLUTO_WEBSTORAGE_H
#define PLUTO_WEBSTORAGE_H

#define WS_KEY_MAX 56    /* key chars + NUL (browsers: any; capped here) */
#define WS_VAL_MAX 8192  /* value chars + NUL (fits the site's GitHub cache) */
#define WS_MAX_ENTRIES 1 /* one key per origin: the site caches one payload */
#define WS_MAX_ORIGINS 4
#define WS_ORIGIN_MAX 64
#define WS_QUOTA_BYTES 12288 /* per-origin quota; keeps one 7.5KB cache whole */

typedef struct WsStore WsStore;

/* Per-document attach: derive the origin from the page URL, bind both
 * stores to it, lazy-load local's section from disk. */
void ws_bind(const char *docUrl);

/* The CURRENT document's stores — never NULL (overflow binds get the
 * honest empty fail-store; see the header comment). */
WsStore *ws_local(void);
WsStore *ws_session(void);

/* The bound origin label (receipts/logs). */
const char *ws_origin(void);

int ws_count(WsStore *s);
int ws_key(WsStore *s, int idx, char *out, size_t outMax);
int ws_get(WsStore *s, const char *key, char *out, size_t outMax);
/* Borrowed pointer to the resident value (NULL on miss). Valid until the
 * store mutates; callers that copy immediately (the XS getItem) use this to
 * avoid a value-sized stack buffer on the device's ~62KB game-task stack. */
const char *ws_get_ptr(WsStore *s, const char *key);
/* 1 = stored; 0 = quota exceeded (caller throws the browser's road). */
int ws_set(WsStore *s, const char *key, const char *value);
void ws_remove(WsStore *s, const char *key);
void ws_clear(WsStore *s);

#endif /* PLUTO_WEBSTORAGE_H */
