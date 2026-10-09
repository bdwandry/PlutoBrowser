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
#include "../core/pluto_spill.h" /* SpillFile (run_script_stream) */

/* ── JS timers (setTimeout / setInterval) — device-safe subset ─────────────
 * Browsers fire timers on an event loop; we have none. The router owns a
 * per-page timer TABLE (engine-agnostic state); engines only pin/call their
 * own function references (same opaque-ref pattern as click listeners).
 * main.c pumps due timers once per frame; each fire is contained like a
 * click handler, and DOM mutations surface through the standard rewalk.
 * Caps (all #ifndef-guarded so builds can tighten them): */
#ifndef JSBRIDGE_TIMERS_MAX
#define JSBRIDGE_TIMERS_MAX 12      /* live timers per page */
#endif
#ifndef JSBRIDGE_TIMER_MIN_MS
#define JSBRIDGE_TIMER_MIN_MS 50    /* clamp tiny delays (battery + thrash) */
#endif
#ifndef JSBRIDGE_TIMER_MAX_MS
#define JSBRIDGE_TIMER_MAX_MS 60000 /* clamp long delays (page lifetime) */
#endif
#ifndef JSBRIDGE_TIMER_BUDGET
#define JSBRIDGE_TIMER_BUDGET 16    /* max fires per pump batch */
#endif
#ifndef JSBRIDGE_TIMER_MAX_FIRES
#define JSBRIDGE_TIMER_MAX_FIRES 64 /* lifetime fires per interval */
#endif
#ifndef JSBRIDGE_TIMER_MAX_CHAIN
#define JSBRIDGE_TIMER_MAX_CHAIN 4  /* nested pumps (self-rescheduling) */
#endif

/* ── XMLHttpRequest / fetch (async HTTP → JS callbacks) ────────────────────
 * The SDK HTTP client is SINGLE-FLIGHT (page body, images and Full-mode
 * scripts share it — see core/http_client.h). The router therefore owns a
 * per-page REQUEST TABLE + one in-flight request at a time; sends queue and
 * are started by the pump whenever the wire is idle. Completion is delivered
 * through the SAME engine bracket discipline as timer fires: run_timer_ref
 * calls the pinned handler, DOM mutations surface via callBudget → rewalk.
 * Caps (#ifndef-guarded): */
#ifndef JSBRIDGE_XHR_MAX
#define JSBRIDGE_XHR_MAX 4          /* live requests per page */
#endif
#ifndef JSBRIDGE_XHR_BODY_MAX
#define JSBRIDGE_XHR_BODY_MAX (64 * 1024)  /* response body cap (SDK-alloc) */
#endif
#ifndef JSBRIDGE_XHR_PAGE_BUDGET
#define JSBRIDGE_XHR_PAGE_BUDGET (128 * 1024) /* delivered bytes per page */
#endif
#ifndef JSBRIDGE_XHR_URL_MAX
#define JSBRIDGE_XHR_URL_MAX 512
#endif

typedef enum
{
    JS_XHR_UNSENT = 0,
    JS_XHR_OPENED = 1,   /* open() ok, not sent */
    JS_XHR_HEADERS_SENT = 2, /* on the wire */
    JS_XHR_DONE = 4,     /* success or error settled (body/error valid) */
    JS_XHR_ABORTED = 5   /* aborted() before completion */
} JsXhrState;

/* One registered request. refs are engine-held (same opaque contract as
 * JsTimer.ref): fnRef = the completion handler, objRef = the wrapper object
 * (so `this` binds and handlers stay reachable). STABLE slots: cleared
 * requests stay parked (release=1) until the post-pump sweep, like timers. */
typedef struct
{
    int active;
    int release;         /* inert slot with engine refs still held */
    int id;              /* stable public id (never reused within a page) */
    JsXhrState state;
    int status;          /* HTTP status (2xx = ok), 0 on network error */
    char ok;             /* 1 = 2xx completed, 0 = error/abort/non-2xx */
    char url[JSBRIDGE_XHR_URL_MAX]; /* ABSOLUTE (resolved at open) */
    char err[96];        /* error message when ok==0 */
    char *body;          /* SDK-allocated response (owned; freed at sweep) */
    size_t bodyLen;
    void *fnRef;         /* engine-held completion handler */
    void *objRef;        /* engine-held wrapper object (this-binding) */
} JsHttpRequest;


typedef enum
{
    JS_TIMER_TIMEOUT = 0,  /* setTimeout: fires once */
    JS_TIMER_INTERVAL = 1  /* setInterval: repeats until cleared/capped */
} JsTimerKind;

/* One registered timer. `ref` is engine-held (opaque, same contract as
 * JsListener.ref) — only the engine that created it may call/free it.
 * STABLE-SLOT contract: slots are never moved while a callback can run;
 * clears/retirements only mark active=0 (+release=1) and the router sweeps
 * refs + compacts the array at pump end, outside every engine bracket. */
typedef struct
{
    int active;
    int release;         /* inert slot with an engine ref still held */
    int id;              /* stable public id (never reused within a page) */
    JsTimerKind kind;
    unsigned intervalMs; /* fire period */
    unsigned dueMs;      /* next fire, absolute SDK ms clock (wrap-safe cmp) */
    int gen;             /* bumped before each fire (callback guard) */
    int fires;           /* lifetime fires (interval cap) */
    void *ref;           /* engine-held callback reference */
} JsTimer;

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

    /* JS timers (router-owned table; engine-held refs — see JsTimer) */
    JsTimer timers[JSBRIDGE_TIMERS_MAX];
    int timerCount;      /* active timers */
    int timerIdSeq;      /* last public timer id handed out */
    int timerChain;      /* nested pump depth (self-rescheduling guards) */
    int inTimer;         /* a timer callback is on the stack */
    int mutationsQueued; /* a fire consumed DOM budget → re-render */

    /* XHR / fetch (router-owned table; engine-held refs — see JsHttpRequest) */
    JsHttpRequest xhr[JSBRIDGE_XHR_MAX];
    int xhrCount;   /* used slots (active + parked) */
    int xhrIdSeq;   /* last public request id handed out */
    int xhrInFlight;/* public id of the request on the wire (0 = none) */
    int inXhr;      /* a completion callback is on the stack */
    int xhrBudgetEpoch; /* monotonically bumped per page; resets the
                         * module-global per-page byte budget on attach */


    int callBudget; /* DOM-mutation budget (per page) */
    int inClick;    /* dispatch re-entrancy guard */
    int preventDef; /* event.preventDefault() flag during dispatch */

    char lastError[128];
    int ran, errs;

    /* R15: scripts skipped by the admission gate, waiting for memory
     * (JSBRIDGE_DEFERRED_MAX above). */
    struct
    {
        int engine; /* engine index to run under when memory allows */
        SpillFile spill;
        size_t len;
    } deferred[4];
    int deferredCount;
};

/* Engine id (kept in sync with storage jsEngine / settings rows). */
typedef enum
{
    JS_ENGINE_MUJS = 0,
    JS_ENGINE_DUKTAPE = 1,
    JS_ENGINE_QUICKJS = 2,
    JS_ENGINE_XS = 3,
    JS_ENGINE_XS_NR = 4 /* XS (No Recursion): heap-bounded walkers fork */
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
    /* Compile + execute one DISK-RESIDENT script body (SW2b spill handle;
     * position-independent reads via pluto_spill_read). Engines that can
     * stream their parser input set this; the router prefers it over the
     * materialize→run_script→free round-trip so a giant page script never
     * needs a full RAM copy (compile-time peak drops by the source size).
     * Optional: NULL = router falls back to run_script + materializer. */
    void (*run_script_stream)(JsBridge *b, SpillFile spill, size_t len,
                              int index);
    /* Call all click listeners registered on anchorNode. */
    int (*dispatch_click)(JsBridge *b, const void *anchorNode);
    /* Release one engine-held timer ref (table slot is already inert;
     * called from jsbridge_timer_clear and at close). */
    void (*clear_timer_ref)(JsBridge *b, void *fnRef);
    /* Invoke one pinned timer callback. Returns 0 ok, 1 = contained JS
     * error, -1 = engine abort (the engine is dead — stop pumping). */
    int (*run_timer_ref)(JsBridge *b, void *fnRef);
    /* Invoke one pinned XHR completion handler: fn(responseText) with
     * `this` = the wrapper object. `r` is the settled request record
     * (stable during the call — stable-slot contract). Same return contract
     * as run_timer_ref. fnRef/objRef use each engine's own pin encoding
     * (tfn[] slot / stash heapptr / registry key). */
    int (*run_xhr_ref)(JsBridge *b, void *fnRef, void *objRef,
                       const JsHttpRequest *r);
    /* Release one request's engine-held refs (slot already inert; called
     * from the pump sweep and at close). Either pointer may be NULL. */
    void (*clear_xhr_refs)(JsBridge *b, void *fnRef, void *objRef);
    /* Destroy the engine + engine-held references. */
    void (*close)(JsBridge *b);
    /* R20: per-frame engine pump (called from jsbridge_deferred_pump's
     * caller path in main.c via the router). NULL = engine has no
     * background work. Currently: XS-NR resumes a time-sliced bundler
     * split across frames (watchdog-safe execution of giant bundles). */
    void (*pump)(JsBridge *b, unsigned nowMs);
    /* R27: 1 while a time-sliced bundler split is still executing its
     * plan (background eval in flight). The render pipeline consults
     * this BEFORE walking/snapshotting: the walker must observe the
     * POST-JS DOM, which only exists after the last segment runs. NULL
     * = engine has no background eval (always 0). */
    int (*eval_pending)(JsBridge *b);
    /* R28: 1 while a resumable-mount run is PARKED mid-program (machine
     * registers hold a half-executed mount). The router pumps (timers,
     * XHR completions, deferred scripts) consult this and SKIP their
     * deliveries without consuming them — a browser never runs timers
     * during a synchronous task, and the parked mount resumes on this
     * very frame via pump(). NULL = engine never parks (always 0). */
    int (*mount_parked)(JsBridge *b);
    /* One-word engine name for logs ("muJS", "Duktape"). */
    const char *name;
} JsEngineImpl;

/* Router helpers shared with the impls (jsbridge.c). */
const JsEngineImpl *js_bridge_impl(JsBridge *b);
int budget_take(JsBridge *b);
void bridge_take_error_text(JsBridge *b, const char *msg);
/* Compile-safety gate (shared by ALL engines — one safety bar). */
int jsbridge_script_compile_safe(const char *src, size_t len);

/* ── R15: OS-truth admission + deferred-retry for giant scripts ──────────
 * A compile's peak footprint (parse tree + emitted bytecode + machine
 * growth) is estimated from the MEASURED device parser rate (~14 funnel
 * bytes per input byte — R13 hardware data; using the true marginal rate
 * here keeps admission conservative so a granted compile can always finish
 * before the allocator's collapse point) + a machine-growth allowance.
 * When the OS cannot grant that much RIGHT NOW, engines skip containedly
 * and the ROUTER remembers the script (disk-resident only — the body
 * never has to live in RAM) and re-offers it on later frames as memory
 * frees. Scripts whose parse would exceed the engine's parser-memory cap
 * fail fast instead (deferral could never succeed). */
#define JSBRIDGE_COMPILE_PER_BYTE 14u
#define JSBRIDGE_COMPILE_CODEGEN_PER_BYTE 5u /* R26f: emitted bytecode + coder nodes */
#define JSBRIDGE_COMPILE_ALLOWANCE (512u * 1024u)
static inline unsigned long jsbridge_compile_need(unsigned long len)
{
    /* R26f: include the codegen term. The cap exemption (fxNRParserCodegen)
     * lets the coder allocate past the parse-only grant, which means the
     * FUNNEL's admission grant is now the only brake between a successful
     * parse and an unbounded codegen — size it for both phases or a 253KB
     * monolith parses fine and then fxAborts the pool at codegen. Measured
     * shape (react-dom 118KB on device, R26f): parse ≈14B/B fit the old
     * grant; codegen pushed parser->total past it before dying. */
    return len * (JSBRIDGE_COMPILE_PER_BYTE +
                  JSBRIDGE_COMPILE_CODEGEN_PER_BYTE) +
           JSBRIDGE_COMPILE_ALLOWANCE;
}

/* Router-side deferred table (router owns lifetime; engine never holds
 * state across calls). Capacity must match struct JsBridge.deferred[]. */
#define JSBRIDGE_DEFERRED_MAX 4
void jsbridge_deferred_offer(JsBridge *b, int engine, SpillFile spill,
                             size_t len);
void jsbridge_deferred_clear(JsBridge *b);
/* Per-frame retry (called from the timer pump path in main.c): probes
 * the OS heap and runs the first script that now fits. mutationsOut (may
 * be NULL) is set when the run consumed DOM budget — caller re-renders. */
void jsbridge_deferred_pump(JsBridge *b, int *mutationsOut);
/* Engine-aware variant: the QuickJS DEVICE parser has no internal stack
 * probe (unlike muJS/Duktape/XS), so its gate uses a tighter depth cap.
 * Sim/host builds pass the wide cap. Defined here so both jsbridge.c
 * (scanner) and jsbridge_quickjs.c (gate call) share one number. */
#define PLUTO_SCAN_MAX_DEPTH 40     /* muJS/Duktape/XS + all sim/host paths */
/* QuickJS on DEVICE: parser depth costs ~2.5KB of the OS-set 61.8KB
 * game-task stack per level (SW4 measurement), and the OS stack is FIXED —
 * the Makefile STACK_SIZE define does not reach pdex games (no .S in the
 * link; the scheduler owns the task stack). Depth 12 is the measured safe
 * ceiling; R17 briefly tried 24 and the device bus-faulted mid-parse
 * (crashlog 2026-10-02 15:10) — revert is evidence, not caution. A 500KB
 * minified IIFE bundle parses at ~depth 24 ⇒ genuinely beyond a one-shot
 * device compile on every engine (muJS/Duktape: ES5 parser; XS NR: 768KB
 * parser-memory cap; QuickJS: this stack wall). */
#define PLUTO_SCAN_MAX_DEPTH_QJS 12 /* QuickJS on DEVICE (parser stack wall) */
int jsbridge_script_compile_safe_ex(const char *src, size_t len,
                                    int max_depth);

/* SW4: 32-bit FNV-1a over the source bytes, salted with the bytecode
 * format version (JSBRIDGE_BC_KEY_SALT). key == 0 is reserved as "no
 * cache" (a real hash collision with 0 maps to no-caching — harmless). */
unsigned long jsbridge_source_key(const char *src, size_t len,
                                  unsigned long salt);

/* ── SW5: ES5 builtin compat prefix (Set/Map/Image shims) ─────────────────
 * Defined in jsbridge.c; EVERY engine's run_script copies this before the
 * page source so a modern bundle's `new Set()` / `new Map()` / `new
 * Image()` resolves on the ES5 engines. The prefix is self-guarding
 * (typeof checks) — engines with native builtins define nothing. It is NOT
 * part of the SW4 bytecode-cache key input: the cache hashes the PAGE
 * source only, and the prefix is deterministic per build.
 * Returns the byte length of the prefix (strlen). */
size_t jsbridge_sw5_prefix(const char **prefixOut);

/* ── Timer table (engines call start/clear from their JS globals) ──────────
 * start: registers the engine-pinned fnRef with the clamped delay, returns
 * the public timer id (>0) or 0 on refusal (table full / no engine — the
 * CALLER must unpin its ref). clear: deactivate + engine-release via the
 * clear_timer_ref vtable; returns 1 if the id was live. */
int jsbridge_timer_start(JsBridge *b, JsTimerKind kind, void *fnRef,
                         unsigned delayMs);
int jsbridge_timer_clear(JsBridge *b, int id);

/* ── XHR table (engines call open/send/abort from XMLHttpRequest methods) ──
 * start: register + resolve `url` against the page's base URL, return the
 * public request id (>0) or 0 on refusal (table full / bad URL — the caller
 * must unpin its refs). send: arm the request (starts when the wire is
 * idle, queues otherwise). abort: settle as JS_XHR_ABORTED (delivering
 * onerror through the pump). clear: deactivate (used before close). */
int jsbridge_xhr_open(JsBridge *b, const char *method, const char *url);
void jsbridge_xhr_send(JsBridge *b, int id, void *fnRef, void *objRef);
void jsbridge_xhr_abort(JsBridge *b, int id);
/* Live request lookup (engines read status/responseText off the wrapper). */
const JsHttpRequest *jsbridge_xhr_get(JsBridge *b, int id);
/* Pump: start queued sends on the idle wire, deliver settled completions
 * through the engine vtable. mutationsOut set when a handler mutated DOM.
 * Returns the number of completions delivered (0 when none). */
int jsbridge_xhr_pump(JsBridge *b, int *mutationsOut);

#endif /* PLUTO_JSBIDGE_INTERNAL_H */
