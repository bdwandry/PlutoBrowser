/*
 * PlutoBrowser — jsbridge_internal.h (internal, not installed)
 * Engine-agnostic contract every JS-engine bridge implements. The router in
 * jsbridge.c allocates one impl struct per attached page and forwards the
 * public jsbridge.h operations to it. Engines are fully isolated: a bridge
 * touches only its own engine's API — pages run exclusively on the engine
 * selected in Settings (no cross-engine fallback, no shared engine state).
 *
 * Conventions shared by all impls (keep behavior engine-identical):
 *   - b->doc / b->dom are borrowed; the browser owns them.
 *   - ran/errs/lastError are updated by the impl; the router copies them
 *     into doc->jsRan/jsErrors/jsLastError after attach.
 *   - DOM objects wrap live DomNode* from doc->_dom; mutation budget is
 *     b->callBudget (JSBRIDGE_CALL_BUDGET), decremented via budget_take.
 *   - Listeners are C-registered targets + engine-held function refs
 *     dispatched by jsbridge_dispatch_link_click (click only).
 *   - document.write is captured to b->output (JSBRIDGE_MAX_OUTPUT cap);
 *     js_doc_flush_output parses + adopts it under the live root.
 *   - All script errors are contained (never abort the task) and mirrored
 *     to pluto.log via the "[js]" lines.
 */
#ifndef PLUTO_JSBIDGE_INTERNAL_H
#define PLUTO_JSBIDGE_INTERNAL_H

#include "jsbridge.h"
#include "../util/strbuf.h"

/* One click listener: target element + engine-held function reference.
 * `ref` is opaque (muJS registry ref / Duktape heapptr / QuickJS pinned
 * JSValue index) — only the engine that created it may deref or free it. */
typedef struct
{
    void *ref;       /* engine-held handler reference */
    DomNode *target; /* element the listener was registered on */
} JsListener;

struct JsBridge
{
    int engine; /* 0 = muJS, 1 = Duktape, 2 = QuickJS, 3 = XS (JsEngine) */
    void *implState; /* engine-private state (js_State* / duk_context* /
                      * QjsState*) */

    DocParseResult *doc; /* borrowed; the browser owns it */
    DomResult *dom;      /* == doc->_dom; live tree (browser frees it) */

    /* document.write capture (shared, engine-agnostic) */
    StrBuf output;
    int outputDropped; /* capture limit hit */

    /* click listeners (engine-held function refs; see JsListener above) */
    JsListener listeners[JSBRIDGE_LISTENERS_MAX];
    int listenerCount;

    int callBudget; /* DOM-mutation budget (per page) */
    int inClick;    /* dispatch re-entrancy guard */
    int preventDef; /* event.preventDefault() flag during dispatch */

    char lastError[128];
    int ran, errs;
};

/* Engine id (kept in sync with storage jsEngine / settings rows). */
typedef enum
{
    JS_ENGINE_MUJS = 0,
    JS_ENGINE_DUKTAPE = 1,
    JS_ENGINE_QUICKJS = 2,
    JS_ENGINE_XS = 3
} JsEngine;

/* Exported from jsbridge.h so main.c/settings can select the engine; the
 * enum here is the shared definition. */

/* ── Per-engine vtable (implemented in jsbridge_mujs.c,
 * jsbridge_duktape.c and jsbridge_quickjs.c)
 * The ROUTER owns script discovery/order (inline extraction, Full-mode slot
 * scan) and calls run_script for every body in document order — engines
 * only ever see "run this source", never the HTML. */
typedef struct JsEngineImpl
{
    /* Create the engine and bind the full browser surface (globals, DOM,
     * console, listeners). Returns 0 ok, -1 engine init failure. */
    int (*init)(JsBridge *b, const char *baseUrl);
    /* Compile + execute one script body (already NUL-terminated-safe span
     * from the router; len < JSBRIDGE_MAX_SCRIPT_BYTES). Contained errors
     * increment b->errs / set b->lastError. */
    void (*run_script)(JsBridge *b, const char *src, size_t len, int index);
    /* Call all click listeners registered on anchorNode. */
    int (*dispatch_click)(JsBridge *b, const void *anchorNode);
    /* Destroy the engine + engine-held references. */
    void (*close)(JsBridge *b);
    /* One-word engine name for logs ("muJS", "Duktape"). */
    const char *name;
} JsEngineImpl;

/* Router helpers shared with the impls (jsbridge.c). */
const JsEngineImpl *js_bridge_impl(JsBridge *b);
int budget_take(JsBridge *b);
void bridge_take_error_text(JsBridge *b, const char *msg);
/* Compile-safety gate (shared by ALL engines — one safety bar). */
int jsbridge_script_compile_safe(const char *src, size_t len);

#endif /* PLUTO_JSBIDGE_INTERNAL_H */
