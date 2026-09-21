/*
 * PlutoBrowser — jsbridge.h
 * JavaScript engine bridge (muJS 1.3.10, Source/js/muJS) wired into the browser's
 * DOM pipeline (document_parse_ex + layout/main event dispatch).
 *
 * Design (device-safe):
 *   - One muJS state per page render. Created on attach (DOC_SCRIPT_RUN /
 *     DOC_SCRIPT_RUN_KEEP), destroyed by js_doc_close — unloading a page
 *     frees everything the page's scripts allocated.
 *   - Hard limits: muJS run-limit (statement/back-edge counter) bounds any
 *     script; the bridge call-budget bounds the DOM work one page may
 *     trigger; allocation caps bound document.write capture. All errors are
 *     contained (js_ploadstring/js_pcall) and surfaced as counters + text.
 *   - DOM objects are muJS userdata wrapping live DomNode pointers (tag
 *     "pluto.dom"). Scripts manipulate the real tree document_parse walks;
 *     after the walk the browser re-renders from the mutated tree.
 *   - document.write parses into nodes adopted under #root (append-only,
 *     the Playdate-feasible subset) before the walker runs.
 */
#ifndef PLUTO_JSBIDGE_H
#define PLUTO_JSBIDGE_H

#include <stddef.h>

#include "html/document.h"

/* ── Resource limits (Playdate-sized) ────────────────────────────────────── */
#define JSBRIDGE_MAX_SCRIPTS 48          /* <script> elements per page (all kinds) */
/* Historical per-script source cap. SW2b: downloads are uncapped, so this
 * now only bounds (a) the RAM-residency threshold above which a fetched
 * source is re-spilled to disk (jsext) and (b) the http RAM-fallback cap.
 * The EXECUTION ceiling is JSBRIDGE_MAX_SCRIPT_SOURCE. */
#define JSBRIDGE_MAX_SCRIPT_BYTES (64 * 1024)
#define JSBRIDGE_MAX_OUTPUT (64 * 1024)  /* total document.write capture */
#define JSBRIDGE_RUNLIMIT 2000000        /* muJS statement/back-edge counter */
/* Single-allocation cap inside the engine. muJS grows its compile and
 * runtime structures with individual mallocs; the device heap has ~3MB
 * free, so budget generously — a failed huge allocation must surface as a
 * compile error, never a device crash. */
#define JSBRIDGE_MAXALLOC (1024 * 1024)  /* single-allocation cap */
#define JSBRIDGE_CALL_BUDGET 1536        /* DOM mutations per page
                                          * (SW5: innerHTML adoption +
                                          * classList/querySelector bursts
                                          * share the mutation budget — was
                                          * 512, sized for the textContent
                                          * era) */
#define JSBRIDGE_LISTENERS_MAX 32        /* click listeners per page */
#define JSBRIDGE_MAX_PROPS 32            /* has-props scan per DOM object */
#define JSBRIDGE_MAX_FILES 32            /* document.files[] size */
#define JSBRIDGE_WRITE_CHUNK 2048        /* per-write() growth step cap */

/* ── SW5: DOM API surface (createElement/appendChild class) ───────────────
 * innerHTML assignment parses real markup: cap the SOURCE size (a page
 * asking to set a megabyte of markup would tokenize for seconds and blow
 * the builder's node cap anyway — refuse early, contained). The parse
 * scratch (tokenizer tokens + a throwaway DomResult) is heap-owned and
 * freed on every path; the budget charges 1 unit + 1 per cleared/adopted
 * node so a markup burst still terminates inside the page budget. */
#ifndef JSBRIDGE_INNER_HTML_MAX
#define JSBRIDGE_INNER_HTML_MAX (16 * 1024)
#endif
/* querySelector scratch buffer size per call site (compound tokens point
 * into it; one 256B stack buffer per bridge call site is device-safe). */
#define JSBRIDGE_QS_SCRATCH 256

/* ── External <script src> support (Full mode) ───────────────────────────── */
#define JSBRIDGE_MAX_EXT_SCRIPTS 24            /* unique external files per page */
#define JSBRIDGE_EXT_URL_MAX 512               /* per-URL storage */
#define JS_SCRIPT_DATA (-2)                    /* SW2d: slot holds a data: URL
                                                * payload (inlineStart/inlineLen
                                                * locate it in the page html) */
#define JSBRIDGE_EXT_PAGE_BUDGET (512 * 1024)  /* SW3: RAM-resident bytes/page
                                                * (was 160KB); disk-resident
                                                * sources are uncapped (SW2b).
                                                * 512KB still leaves ~2MB of the
                                                * 8MB pool under the 2.5MB engine
                                                * heap + DOM + stack. */

/* Execution ceiling: the largest script SOURCE (inline or external) the
 * engines are ever handed. SW2b: downloads are disk-streamed and uncapped
 * (jsext no longer cuts the socket at 65KB), so this source ceiling
 * is the single uniform execution limit.
 * SW3 raise (measured, 2026-09-20): the matrix re-score showed real bundles
 * (bryanwandrych.com main.*.js = 502716 bytes) downloading + spilling fine
 * through SW2b/SW2c but being REFUSED at execution by the old 256KB ceiling.
 * 768KB covers the webpack-bundle class while bounding worst-case compile
 * expansion inside the raised 2.5MB engine heaps (QuickJS measured ~2.1x
 * source size in RAM for a 502KB minified bundle; other engines expand less).
 * Engines still REFUSE gracefully (skip + log, page continues) if their own
 * heap limit would be exceeded — the soft budget is the real guard. */
#define JSBRIDGE_MAX_SCRIPT_SOURCE (768 * 1024)

/* ── SW4: QuickJS bytecode cache (QuickJS bridge only) ────────────────────
 * Sources shorter than this parse faster than a disk round-trip is worth:
 * not cached. Bytecode blobs over the cap are not stored (store stays tiny;
 * a 502KB minified bundle compiles to ~1.1MB bytecode — capped generously
 * at 1.5MB but bounded). Keys are a 32-bit FNV-1a of the SOURCE bytes with
 * a format-version salt: any engine/build change that altered bytecode
 * semantics bumps the salt and orphans every old entry (each is also
 * validated by the reader — corrupt/foreign blobs fall back to a reparse). */
#define JSBRIDGE_BC_MIN_SOURCE 4096
#define JSBRIDGE_BC_MAX_BYTES (4 * 1024 * 1024)
#define JSBRIDGE_BC_KEY_SALT 0x514A4231UL /* "QJB1" */

/* One fetched external script file. TWO residency modes:
 *  - body != NULL: RAM-resident raw source, arena-allocated, owned by the
 *    DocParseResult (the pre-SW2b path; run_script copies before running).
 *  - body == NULL && spill >= 0: disk-resident source (spill-resident);
 *    len is its byte size. The executor materializes it just-in-time into
 *    one run_script-size buffer (SW2b: disk holds bulk, RAM holds the
 *    active script). Exactly one mode is active per file.
 *  - body == NULL && spill < 0: not fetched / refused / failed.
 * (Tagged so document.h can hold pointers to it without a cycle.) */
typedef struct JsExtScript_
{
    char url[JSBRIDGE_EXT_URL_MAX]; /* ABSOLUTE url (resolved by html/jsext) */
    char *body;
    size_t len;
    int spill;                      /* pluto_spill handle when disk-resident */
} JsExtScript;

/* One <script> element in document order: an inline body, or a reference to
 * an extScripts[] entry. Slots are produced by jsbridge_scan_scripts and
 * executed in array order — the browser-faithful interleaving. */
typedef struct
{
    int isExt;               /* 0 = inline body below; 1 = extScripts[extIndex]
                              *    or extIndex == JS_SCRIPT_DATA (data: URL) */
    const char *inlineStart; /* inline body start (points into the page html);
                              *    SW2d: for JS_SCRIPT_DATA slots this points
                              *    at the data: URL VALUE inside the src attr */
    size_t inlineLen;
    int extIndex;            /* -1 = external but unresolvable/over-cap;
                              *    JS_SCRIPT_DATA = data:-URL payload slot */
} JsScriptSlot;

typedef struct JsBridge JsBridge;

/* Materialize a spill-resident external script into one run_script-size
 * buffer (malloc'd, NUL-terminated; caller frees). Returns NULL when the
 * handle is bad, the source is over JSBRIDGE_MAX_SCRIPT_SOURCE, or the
 * read fails. RAM-resident files never take this path. */
char *jsext_materialize_spill_script(const JsExtScript *e);

/* SW2d: decode a data:-URL script payload (RFC 2397 — percent-escaped text
 * or ;base64) into a malloc'd NUL-terminated source string, or NULL on
 * malformed input. Caller frees; the engines copy before running. */
char *jsext_decode_data_script(const char *src, size_t len);

/* jsbridge_dispatch_link_click results: */
typedef enum
{
    JSB_CLICK_NONE = 0,       /* no handler ran → navigate normally */
    JSB_CLICK_NAVIGATE = 1,   /* handlers ran, default not prevented */
    JSB_CLICK_SUPPRESSED = 2  /* preventDefault() → re-render, don't navigate */
} JsBridgeClickResult;

/* ── Script scanning ───────────────────────────────────────────────────────
 * Scan raw HTML for <script> elements in document order into `slots`.
 * Signature lives here so both Inline and Full modes share one parser.
 * extRaw (optional) additionally receives the RAW src= strings of external
 * references (deduplicated; html/jsext resolves them against the page URL);
 * external elements then occupy slots at their page positions. When
 * extRaw is NULL, src= elements are skipped entirely (legacy Inline-mode
 * scan, bit-identical to the pre-Full behavior). Returns the TOTAL slot
 * count (uncapped); only the first slotMax entries are stored. */
int jsbridge_scan_scripts(const char *html, JsScriptSlot *slots, int slotMax,
                          char (*extRaw)[JSBRIDGE_EXT_URL_MAX], int extMax,
                          int *extCountOut);

/* Run all inline <script> bodies of `doc` per policy (see document.h).
 * policy DOC_SCRIPT_RUN frees the engine before returning; RUN_KEEP leaves
 * it attached for jsbridge_dispatch_click; FULL also executes fetched
 * externals (doc->extScripts bodies) at their page positions. Returns 0 ok
 * (check doc->jsErrors / doc->jsLastError), -1 engine init failure. */
int js_doc_attach(JsBridge **out, DocParseResult *doc, DocScriptPolicy policy);

/* Parse the captured document.write output and adopt the nodes under the
 * document root. Called by document_parse before the walker. */
void js_doc_flush_output(JsBridge *bridge);

/* Destroy the engine and release the page's live DomResult. Call before
 * document_free (document_free also frees a still-attached _dom). */
void js_doc_close(JsBridge *bridge);

/* Dispatch a click to page handlers registered on `anchorNode` (the <a>
 * element's DomNode). Returns a JsBridgeClickResult; a JSB_CLICK_SUPPRESSED
 * result means the page mutated the DOM (preventDefault) and the caller
 * should re-render instead of navigating. */
int jsbridge_dispatch_link_click(JsBridge *bridge, const void *anchorNode);

/* Run one extra script body against an attached page bridge (routed through
 * the same engine/compile-safety/budget path as page scripts). Intended for
 * host tests and diagnostics; returns 0 dispatched, -1 rejected. */
int jsbridge_page_script(JsBridge *bridge, const char *src, size_t len);

/* ── SW5: engine-agnostic DOM API surface (all four engines bind these) ────
 * Router-owned so every engine gets IDENTICAL semantics and caps:
 * innerHTML assignment parses real markup (16KB source cap, budgeted per
 * node); querySelector/All walk the live subtree with the #2 CSS compound
 * grammar (type/.class/#id/* + descendant; anything else = null result,
 * never an exception); classList mutates the class attribute token list.
 * Engines only marshal arguments/results. All functions are safe on a
 * detached node (they operate on the node, not the tree position). */
int jsbridge_el_set_inner_html(JsBridge *b, void *el, const char *html,
                               size_t len);
/* emit is called per match in document order (return <0 to stop early);
 * the return is the match count, -1 on an unusable selector / budget-out. */
int jsbridge_el_query_selector_all(JsBridge *b, void *el, const char *sel,
                                   char *scratch, size_t scratchSize,
                                   int (*emit)(void *el, void *ud), void *ud);
/* First match or NULL (NULL also for an unusable selector — engines map
 * both to `null`). */
void *jsbridge_el_query_selector_first(JsBridge *b, void *el, const char *sel,
                                       char *scratch, size_t scratchSize);
/* classList: 0 ok, -1 failure; toggle returns 1 added / 0 removed / -1. */
int jsbridge_el_class_has(void *el, const char *token);
int jsbridge_el_class_add(JsBridge *b, void *el, const char *token);
int jsbridge_el_class_remove(JsBridge *b, void *el, const char *token);
int jsbridge_el_class_toggle(JsBridge *b, void *el, const char *token);

/* Diagnostics accessors (logging/telemetry). */
int jsbridge_listener_count(const JsBridge *bridge);

/* ── JS timer pump (setTimeout / setInterval) ──────────────────────────────
 * Fire every due timer once. mutationsOut (may be NULL) is set to 1 when a
 * callback consumed DOM budget — the caller should schedule a page
 * re-render (the same path a preventDefault click uses). Returns the number
 * of timers fired this call (0 when no bridge / no timers). */
int jsbridge_timers_pump(JsBridge *bridge, unsigned nowMs, int *mutationsOut);
/* Diagnostics: live timers / due-right-now timers (telemetry + tests). */
int jsbridge_timers_active(const JsBridge *bridge);
int jsbridge_timers_pending(const JsBridge *bridge, unsigned nowMs);

/* ── XHR/fetch pump (async HTTP completions) ─────────────────────────
 * Deliver every settled request once (router owns the table and the
 * single-flight HTTP session; main.c pumps after http_update). Set 1
 * through mutationsOut (may be NULL) when a completion wrote DOM — the
 * caller schedules the standard re-render. Returns deliveries made. */
int jsbridge_xhr_pump(JsBridge *bridge, int *mutationsOut);

/* Select the engine for ALL subsequently attached pages: JS_ENGINE_MUJS
 * (default), JS_ENGINE_DUKTAPE, JS_ENGINE_QUICKJS or JS_ENGINE_XS. Any
 * other value selects muJS. Called by
 * main when Settings change; pages never mix engines — the chosen engine
 * runs the page exclusively. (See jsbridge_internal.h for JS_ENGINE_*.) */
void jsbridge_set_engine(int engine);

/* Current engine selection (JS_ENGINE_MUJS, JS_ENGINE_DUKTAPE,
 * JS_ENGINE_QUICKJS or JS_ENGINE_XS) for
 * logging/telemetry — does not affect any page. */
int jsbridge_current_engine(void);

#endif /* PLUTO_JSBIDGE_H */
