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
#define JSBRIDGE_MAX_SCRIPT_BYTES (64 * 1024) /* per-script source cap */
#define JSBRIDGE_MAX_OUTPUT (64 * 1024)  /* total document.write capture */
#define JSBRIDGE_RUNLIMIT 2000000        /* muJS statement/back-edge counter */
/* Single-allocation cap inside the engine. muJS grows its compile and
 * runtime structures with individual mallocs; the device heap has ~3MB
 * free, so budget generously — a failed huge allocation must surface as a
 * compile error, never a device crash. */
#define JSBRIDGE_MAXALLOC (1024 * 1024)  /* single-allocation cap */
#define JSBRIDGE_CALL_BUDGET 512         /* DOM mutations per page */
#define JSBRIDGE_LISTENERS_MAX 32        /* click listeners per page */
#define JSBRIDGE_MAX_PROPS 32            /* has-props scan per DOM object */
#define JSBRIDGE_MAX_FILES 32            /* document.files[] size */
#define JSBRIDGE_WRITE_CHUNK 2048        /* per-write() growth step cap */

/* ── External <script src> support (Full mode) ───────────────────────────── */
#define JSBRIDGE_MAX_EXT_SCRIPTS 24            /* unique external files per page */
#define JSBRIDGE_EXT_URL_MAX 512               /* per-URL storage */
#define JSBRIDGE_EXT_PAGE_BUDGET (160 * 1024)  /* total external JS bytes/page */

/* One fetched external script file. body is the raw source (SDK-allocated,
 * owned by the DocParseResult); NULL = not fetched / refused / failed.
 * (Tagged so document.h can hold pointers to it without a cycle.) */
typedef struct JsExtScript_
{
    char url[JSBRIDGE_EXT_URL_MAX]; /* ABSOLUTE url (resolved by html/jsext) */
    char *body;
    size_t len;
} JsExtScript;

/* One <script> element in document order: an inline body, or a reference to
 * an extScripts[] entry. Slots are produced by jsbridge_scan_scripts and
 * executed in array order — the browser-faithful interleaving. */
typedef struct
{
    int isExt;               /* 0 = inline body below; 1 = extScripts[extIndex] */
    const char *inlineStart; /* inline body start (points into the page html) */
    size_t inlineLen;
    int extIndex;            /* -1 = external but unresolvable/over-cap */
} JsScriptSlot;

typedef struct JsBridge JsBridge;

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

/* Diagnostics accessors (logging/telemetry). */
int jsbridge_listener_count(const JsBridge *bridge);

/* Select the engine for ALL subsequently attached pages: JS_ENGINE_MUJS
 * (default) or JS_ENGINE_DUKTAPE. Any other value selects muJS. Called by
 * main when Settings change; pages never mix engines — the chosen engine
 * runs the page exclusively. (See jsbridge_internal.h for JS_ENGINE_*.) */
void jsbridge_set_engine(int engine);

/* Current engine selection (JS_ENGINE_MUJS or JS_ENGINE_DUKTAPE) for
 * logging/telemetry — does not affect any page. */
int jsbridge_current_engine(void);

#endif /* PLUTO_JSBIDGE_H */
