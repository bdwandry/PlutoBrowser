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
    /* C4 (fetch/POST, NR engine): send METHOD captured at open and the
     * REQUEST body (owned copy; freed with the request at sweep/abort/
     * close). Zero-init keeps every existing GET-only caller identical:
     * method[0]=='\0' means GET and reqBody==NULL means no body. */
    char method[8];
    char *reqBody;
    size_t reqBodyLen;
    /* API-P2: setRequestHeader-carried request headers. Content-Type
     * rides http_request_ct on wire POSTs (last write wins; zero-init =
     * the historical application/json default — pre-existing callers stay
     * byte-identical). Every OTHER header is RECORDed for receipts only:
     * the HTTP client exposes no extra-header knob, so that's a
     * documented boundary, never a faked conductor. */
    char reqCt[64];
    char xhdrName[40];
    char xhdrVal[72];
} JsHttpRequest;

/* API-P3: the in-engine address journal (history.pushState/replaceState
 * + back/forward/go over the ENGINE'S OWN book). Entry URLs are absolute
 * (resolved against the page base or the current entry — fragments,
 * queries, and paths resolve browser-faithfully; a scheme/host mismatch
 * is refused like a real cross-origin push). The doc base seeds entry 0
 * lazily on first journal use; a REAL navigation per page replaces the
 * whole bridge, so re-seeding per page is automatic. pushState NEVER
 * touches the wire (no http_* call, no pendingNav, no app-bar change):
 * the journal feeds location reads and popstate/hashchange delivery
 * only. Bytes: 8 entries x 260 = ~2KB per page (fixed, freed with the
 * bridge). */
#define JSBRIDGE_HIST_MAX 8
#define JSBRIDGE_HIST_URL_MAX 260

/* ── API-P8: the Worker contained-stub lane ─────────────────────────────
 * A real background thread is not available inside this browser's single
 * cooperative pump (device truth: one CPU, one frame budget). The honest
 * browser-shaped story is NOT to hide the constructor — a page doing
 * `new Worker(url)` would die on a ReferenceError and never run — but to
 * hand the page a WORKER OBJECT that behaves like a worker whose script
 * always fails to start, and to tell it through the browser's own failure
 * protocol: the object constructs without throwing, carries postMessage/
 * terminate/onerror/onmessage, and its `error` event fires ONCE at the
 * next frame (never synchronously — the setTimeout-less analog of a real
 * failed script load) with {message}. The worker SCRIPT is never
 * compiled, never fetched, never executed: onmessage never fires; the
 * main thread never blocks; a page with a non-worker fallback takes it
 * exactly as it would for any briefly-broken script URL. postMessage is
 * absorbed silently (logged; data dropped) — the message never comes
 * back. terminate() silences the pending delivery or is a no-op after
 * it. Caps (documented): 8 stubs resident per page; constructions beyond
 * the 8th carry NO delivery slot (the object still works; onerror never
 * fires — logged once per page). The stub table rides the SAME stable-
 * slot + bracket discipline as timers/XHR (engine-held objRef; deliveries
 * inside the pump; sweep outside every bracket). */
#ifndef JSBRIDGE_WORKER_MAX
#define JSBRIDGE_WORKER_MAX 8        /* resident worker stubs per page */
#endif
#ifndef JSBRIDGE_WORKER_URL_MAX
#define JSBRIDGE_WORKER_URL_MAX 96   /* as-constructed URL (never resolved) */
#endif
#ifndef JSBRIDGE_WORKER_MSG_MAX
#define JSBRIDGE_WORKER_MSG_MAX 128  /* the delivered error .message text */
#endif

/* One registered worker stub. objRef is engine-held (same opaque contract
 * as JsHttpRequest.objRef). The URL is as the page wrote it: the router
 * never resolves it because nothing is ever fetched. `terminated` before
 * delivery = the stub stays silent (browser: the load was stopped);
 * after delivery = a no-op. STABLE slots (cleared stubs stay parked
 * (release=1) until the pump sweep), like timers/XHR. */
typedef struct
{
    int active;    /* constructed, waiting for (or past) its delivery */
    int release;   /* inert slot with an engine ref still held */
    int id;        /* stable public id (never reused within a page) */
    int terminated;/* terminate() seen; pending delivery cancelled */
    int posts;     /* postMessage count (receipt; absorb always happens) */
    char url[JSBRIDGE_WORKER_URL_MAX];
    char message[JSBRIDGE_WORKER_MSG_MAX]; /* delivered event .message */
    void *objRef;  /* engine-held wrapper object */
} JsWorkerStub;

/* ── API-P9: the per-node element-wrapper cache ─────────────────────────
 * Browser identity model: ONE wrapper object per DOM node for the page's
 * whole life — getElementById twice, a parentNode climb, a querySelector
 * hit and an appendChild return all hand back the SAME object, so strict
 * `===` chains and any expando the page wrote survive across accesses.
 * XS NR has held one JS-visible wrapper per node since P1 (state arrays
 * + pinned slots). mujs and duktape now share these BRIDGE-SCOPED
 * entries (fresh per page — the bridge is memset(0) at attach) with
 * engine-held refs, forwarded by the engine push_element factories:
 *   - mujs: a js_ref registry key (the xhr-pin contract; unref'd at
 *     page close along with the listeners);
 *   - duktape: a heapptr whose only strong ref lives in the global
 *     stash under the hidden key pluto.elw.<i> (i = the entry index;
 *     the heap dies with the engine at page close, as documented).
 * Aliasing-safe within a page: dom_remove_child DETACHES nodes (never
 * frees), so a node pointer stays unique for the page's life; the
 * whole DOM frees with the doc, after the engine close. */
#define JSBRIDGE_ELW_MAX 128        /* wrapper cache entries per page */
typedef struct
{
    DomNode *node; /* the cached node (arena-live for the page) */
    void *ref;     /* engine-held wrapper ref (opaque per engine) */
} JsElwEntry;


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
 * JSValue index) — only the engine that created it may deref or free it.
 * `kind` (XS NR only; zero elsewhere) selects the event class the handler
 * answers: 0 = click (dispatch_click), 1 = load, 2 = error. The engine
 * zero-inits registrations elsewhere, so the shared click path is
 * unchanged for the frozen engines. */
typedef struct
{
    void *ref;       /* engine-held handler reference */
    DomNode *target; /* element the listener was registered on */
    int kind;        /* 0 = click, 1 = load, 2 = error (default 0) */
    /* B3: exact event type ("click","input","change","submit","load",
     * "error","DOMContentLoaded") — the bubbling dispatch routes
     * listeners by type match. R54: widened 12→24 — "DOMContentLoaded"
     * is 17 bytes with its NUL and was silently snprintf-truncated to
     * "DOMContentD", which row 483's registration stored as a dead
     * alias the drain dispatch (kind 3 scan) could never find.
     * Frozen engines never set it (stays "" → their dispatch_click keeps
     * answering kind matches only). */
    char type[24];
} JsListener;

/* API-P5: per-document ES-module graph state (loader-side lane; the
 * transform runs in C before any engine sees the source). Keys = the
 * RESOLVED import URLs (about: store keys, file: Data-dir paths); an
 * inline module gets a synthetic "<docUrl>#p5mi<N>" key (inline modules
 * are not addressable, so their namespace is theirs alone).
 * API-P10: 12 — the standing probe battery grew four async-lane module
 * scripts (moddyn/moddynid/modmiss/modnet) + their store children, so
 * the old 8-slot graph cap refused honest edges (receipted run 1). */
#define JSBRIDGE_MAX_MODULES 12
#define JSBRIDGE_MOD_KEY_MAX 128
#define JSBRIDGE_MOD_IMPORTS 6 /* P10: imports per module (single def) */

/* API-P10: async http(s) module graphs. The flight table holds the lane's
 * in-flight remote modules plus parents parked while their children load
 * (ONE entry per pending role — see jsbridge.c). Sources are HEAP copies
 * (the cap stays JSBRIDGE_MOD_SRC_MAX in jsbridge.c) so the table outside
 * a loading graph costs only its headers. dynQ = dynamic-import()
 * settlement tickets; both tables are fresh per page (bridge memset(0)). */
#define JSBRIDGE_MOD_FLIGHT_MAX 6
#define JSBRIDGE_DYNQ_MAX 4

typedef struct JsModFlight
{
    int set;      /* slot in use */
    int state;    /* 1 = fetching (no src yet), 2 = holding/pending
                   * children, 3 = drained (slot free at pump sweep) */
    char key[JSBRIDGE_MOD_KEY_MAX];
    char importBase[JSBRIDGE_MOD_KEY_MAX];
    char *src;    /* heap copy of this module's source (held) */
    size_t srcLen;
    int depth;
    int index;
    int pendCount;                                /* remote children left */
    char childKeys[JSBRIDGE_MOD_IMPORTS][JSBRIDGE_MOD_KEY_MAX];
    unsigned char impLoaded[JSBRIDGE_MOD_IMPORTS];
    unsigned epoch;   /* page epoch at enqueue; stale deliveries drop */
    unsigned startMs; /* pump clock at fetch start (watchdog) */
    unsigned char retried; /* watchdog requeue-once latch */
} JsModFlight;

struct JsBridge
{
    int engine; /* 0 = muJS, 1 = Duktape, 2 = QuickJS, 3 = XS (JsEngine) */
    void *implState; /* engine-private state (js_State* / duk_context* /
                      * QjsState*) */

    DocParseResult *doc; /* borrowed; the browser owns it */
    DomResult *dom;      /* == doc->_dom; live tree (browser frees it) */

    /* A2: `document.currentScript` context — the ROUTER stamps the running
     * script's <script …> open-tag span (+ resolved src for data:/remote
     * runs) before EVERY script run and clears it after; impls that model
     * the browser API synthesize the element on demand (Turbopack/webpack
     * loaders read currentScript.src to find their chunk URL). NULL/0
     * between runs: dynamic import()/timers see null, like a browser. */
    const char *curTagStart;
    size_t curTagLen;
    const char *curSrcUrl;

    /* A2: newest stamped EXTERNAL chunk URL, kept clear-after-script
     * (unlike curSrcUrl): bundler registry drains run while an inline
     * bootstrap slot is stamped and loaders there read
     * currentScript.src — a src-bearing fallback (see the NR engine's
     * currentScript getter) resolves against this like a chunk run. */
    const char *lastExtSrcUrl;

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

    /* API-P8: the Worker contained-stub table (see the JsWorkerStub note
     * above). Router-owned like the XHR table; deliveries ride the same
     * pump bracket discipline. */
    JsWorkerStub wk[JSBRIDGE_WORKER_MAX];
    int wkCount;
    int wkIdSeq;
    int wkOverflow; /* >WORKER_MAX stubs this page (logged once) */
    int inWorker;   /* a worker callback is on the stack */

    /* API-P9: the per-node wrapper cache (browser identity model; see
     * the JsElwEntry note above). Engine-held refs; fresh per page. */
    JsElwEntry elw[JSBRIDGE_ELW_MAX];
    int elwCount;
    int elwOverflow; /* >ELW_MAX nodes this page (logged once) */

    /* API-P10: async module lane (see the JsModFlight note above).
     * modFlight = the in-flight remote modules + parked parents; the
     * ledger's modState gains 3 = remote-flighting. dynQ = dynamic
     * import() settlement tickets (id = a counter handed to the shim's
     * __p5modRequest; delivery = a tiny __p5dynq eval at the pump).
     * modFetchQueued = the ONE module fetch serialized lane-side;
     * dynShimDone = the per-page __p5dynimport shim latch. */
    JsModFlight modFlight[JSBRIDGE_MOD_FLIGHT_MAX];
    int modFlightCount; /* used slots (active + parked) */
    int modFetchBusy;   /* a module fetch owns the client right now */
    int modFetchStop;   /* receipted module-fetch error seen (drop the head) */
    struct
    {
        int set;
        int id;
        char key[JSBRIDGE_MOD_KEY_MAX];
        unsigned char state; /* 0 pending, 1 resolved, 2 rejected */
    } dynQ[JSBRIDGE_DYNQ_MAX];
    int dynQCount;
    int dynIdSeq;    /* last __p5modRequest ticket id handed out */
    int dynShimDone; /* the __p5dynimport shim ran once on this page */
    int inModRun;    /* a module body is on the stack (nest guard) */

    /* P10 scratch: mod_finish_run fills these right before returning 2
     * (pending) — the resolved keys of REMOTE children still in flight.
     * Duplicated keys dedupe into ONE flight wait (a double import is a
     * single load). */
    int modPendCount;
    char modPendKeys[JSBRIDGE_MOD_IMPORTS][JSBRIDGE_MOD_KEY_MAX];

    /* API-P3: the in-engine address journal (see the JSBRIDGE_HIST_* note
     * above). histCount 0 = unseeded; entry 0 = the doc base seed. */
    char histUrl[JSBRIDGE_HIST_MAX][JSBRIDGE_HIST_URL_MAX];
    int histCount; /* entries in use (0..JSBRIDGE_HIST_MAX) */
    int histIdx;   /* current entry index */


    int callBudget; /* DOM-mutation budget (per page) */
    int inClick;    /* dispatch re-entrancy guard */
    int preventDef; /* event.preventDefault() flag during dispatch */

    char lastError[128];
    int ran, errs;

    /* B3: event.stopPropagation() latch during a bubbled dispatch (the
     * walk stops climbing past the listener's node; frozen engines never
     * set it — additive trailing field, zero-init like the rest). */
    int stopProp;

    /* innerHTML-assignment mutation latch (XS (No Recursion) only, so the
     * frozen engines stay untouched): set when a wrapper-level
     * innerHTML assignment REPLACED/cleared subtree children so the sim's
     * frame loop re-runs page_rewalk_now() (layout must re-anchor to the
     * fresh subtree — react's fallback restore swaps head/body wholesale,
     * and stale layout blocks would dispatch to DETACHED nodes). Reply
     * after each frame through jsbridge_take_dom_dirty(). */
    int domDirty;

    /* Phase 10: DOM-mutation note ring (transition lane). The bridge
     * marks WHICH element changed (see jsbridge_note_dom_mutation in
     * jsbridge.h); the browser drains at render completion and fires
     * transitionend for each noted node still armed with a transition.
     * Frozen engines never note (the ring stays empty for them). */
    struct
    {
        void *node;
    } mutNotes[JSB_MUT_NOTES_MAX];
    int mutNoteCount;

    /* R15: scripts skipped by the admission gate, waiting for memory
     * (JSBRIDGE_DEFERRED_MAX above). */
    struct
    {
        int engine; /* engine index to run under when memory allows */
        SpillFile spill;
        size_t len;
    } deferred[4];
    int deferredCount;

    /* R54: per-frame script stage park (FULL mode). js_doc_attach scans
     * the slot plan, parks it HERE in document order, and returns; the
     * frame sweep then runs ONE entry per update() through
     * jsbridge_script_queue_pump. The old sync attach loop executed every
     * script's parse+compile+run bracket inside ONE update() frame — the
     * device breached the 10s SDK watchdog mid-lexer (E1 2026-10-18
     * 12:02:50, pc=fxNewParserSymbol_nr) after ~9.5s of ARM parse work;
     * the host sim finishes the whole stage in <1s and never reproduced
     * it. Entries are copies: every span points into the DOC-owned
     * rawHtml / extScripts, valid for the bridge's lifetime. */
    JsScriptSlot scriptQueue[JSBRIDGE_MAX_SCRIPTS];
    int scriptQueueCount; /* entries parked (<= JSBRIDGE_MAX_SCRIPTS) */
    int scriptQueueNext;  /* next to run; == scriptQueueCount when drained */

    /* API-P5: the module graph ledger. modKeys hold every resolved module
     * URL this document has loaded; modState 0/1/2 = unvisited/visiting/
     * done (the cycle guard). Fresh per bridge (memset at construct) —
     * parts of the ledger never outlive the document. modSeq numbers
     * synthetic keys for inline modules. */
    char modKeys[JSBRIDGE_MAX_MODULES][JSBRIDGE_MOD_KEY_MAX];
    unsigned char modState[JSBRIDGE_MAX_MODULES];
    /* API-P10: per-ledger success byte for the dynamic delivery path
     * (modState 2 cannot distinguish a loaded module from a refused
     * one — failed loads stay reusable per the standing P5 contract).
     * 1 = the body ran; 0 = refused/failed (namespace missing). */
    unsigned char modOk[JSBRIDGE_MAX_MODULES];
    int modCount;
    int modSeq;
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

/* API-P10: the dynamic-import bridge core (declared here so every
 * engine adapter registers its own tiny native wrapper over it). The
 * shim's __p5dynimport calls the native; the reply is a status STRING:
 * "k:<key>" = resolved now (namespace on globalThis.__p5mod),
 * "p:<id>" = a ticket the module pump settles via __p5dynq evals,
 * "x:<msg>" = refused with the browser's message class. Never runs
 * module work inside the engine call (the pump owns brackets). */
void jsbridge_dyn_request(JsBridge *b, const char *spec, const char *base,
                          char *out, size_t outn);

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

    /* B2: element-event dispatch with browser bubble semantics
     * ("click"/"input"/"change"/"submit"; see jsbridge.h's
     * jsbridge_dispatch_element_event). Additive LAST slot: the frozen
     * engines' positional vtable initializers keep their exact mapping,
     * and trailing unlisted members zero-init to NULL — the router then
     * short-circuits to NONE so the caller takes its native action. */
    int (*dispatch_event)(JsBridge *b, const void *node, const char *type);
    /* R54: called ONCE by the router when the per-frame script queue
     * drains (the sync attach loop's effective end).NULL = engine has
     * no drained-time work (frozen engines zero-init). Currently: the
     * XS-NR fork dispatches DOMContentLoaded to document-level
     * listeners — browser parse semantics (loading during the script
     * stage via xs_document_readyState; next.js's app-router arms its
     * RSC stream closer on DCL, and its setTimeout fallback was
     * interleave-unsafe with the per-frame queue). */
    void (*script_queue_drained)(JsBridge *b);
    /* API-P8: invoke one worker stub's error delivery — the CURRENT
     * onerror property on the pinned stub object is consulted at fire
     * time (browser at-fire semantics) and called as onerror(ev) with
     * `this` = the stub object and ev = {message: w->message}. A stub
     * without a callable onerror delivers nothing (browsers: silent).
     * `w` is stable during the call (stable-slot contract). Same return
     * contract as run_timer_ref (0 ok / 1 contained / -1 engine abort). */
    int (*run_worker_ref)(JsBridge *b, void *objRef, const JsWorkerStub *w);
    /* Release one stub's engine-held object ref (slot inert; called from
     * the pump sweep and at close). objRef may be NULL. */
    void (*clear_worker_refs)(JsBridge *b, void *objRef);
} JsEngineImpl;

/* Router helpers shared with the impls (jsbridge.c). */
const JsEngineImpl *js_bridge_impl(JsBridge *b);
int budget_take(JsBridge *b);
void bridge_take_error_text(JsBridge *b, const char *msg);
/* Compile-safety gate (shared by ALL engines — one safety bar). */
int jsbridge_script_compile_safe(const char *src, size_t len);
/* Browser-faithful data-type filter (shared by both script scanners in
 * jsbridge.c + jsext.c): returns 1 when this <script> element's `type=`
 * names a non-executable MIME (JSON family / importmap …) and the element
 * must be skipped, 0 when its body/src is executable JavaScript. */
int jsbridge_script_type_is_data(const char *lt, const char *gt);

/* API-P5 module marker (both scanners, same rule): returns 1 when this
 * <script> element's `type=` reads "module" (any case). Static import/
 * export syntax then routes through the loader-side module lane
 * (jsbridge.c) instead of the engine's classic run. */
int jsbridge_script_type_is_module(const char *lt, const char *gt);

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
/* C4 (fetch/POST): twin of jsbridge_xhr_send that also carries the
 * REQUEST body (deep-copied). Frozen engines never call it. */
void jsbridge_xhr_send_body(JsBridge *b, int id, const char *body,
                            size_t bodyLen, void *fnRef, void *objRef);
/* API-P2: the fetch shim's setRequestHeader lane. Content-Type overrides
 * the wire default on POSTs (http_request_ct); other headers are
 * recorded for receipts (documented wire boundary), an error text is
 * set when the request is past OPENED. Returns 1 stored, 0 refused. */
int jsbridge_xhr_set_header(JsBridge *b, int id, const char *name,
                            const char *value);
void jsbridge_xhr_abort(JsBridge *b, int id);
/* Live request lookup (engines read status/responseText off the wrapper). */
const JsHttpRequest *jsbridge_xhr_get(JsBridge *b, int id);
/* Pump: start queued sends on the idle wire, deliver settled completions
 * through the engine vtable. mutationsOut set when a handler mutated DOM.
 * Returns the number of completions delivered (0 when none). */
int jsbridge_xhr_pump(JsBridge *b, int *mutationsOut);

/* ── API-P8: the Worker contained-stub router (see JsWorkerStub) ──
 * open() registers a stub and returns its public id; 0 = a delivery-less
 * stub past the documented cap (the object still works). get/terminate/
 * post follow the XHR router shapes. The pump = the delivery lane. */
int jsbridge_worker_open(JsBridge *b, const char *url);
const JsWorkerStub *jsbridge_worker_get(JsBridge *b, int id);
void jsbridge_worker_terminate(JsBridge *b, int id);
void jsbridge_worker_post(JsBridge *b, int id);
/* Engine-side pin: hand the router a stub's engine-held object ref (set
 * once at construct — matching-id + NULL-ref guards). */
void jsbridge_worker_pin(JsBridge *b, int id, void *objRef);

/* ── API-P3: the address journal (history.pushState/replaceState) ──
 * The journal lives in the bridge (histUrl[]), the delivery machinery in
 * each engine. pushState NEVER routes through the wire — no http_* call,
 * no pendingNav, no app address-bar change: entries feed location reads
 * and the engine's popstate/hashchange back-stack delivery only. */
/* Resolver for pushState/replaceState URL arguments: resolves `raw`
 * against the journal's CURRENT entry (fragment/query/path forms per the
 * URL standard; about: opaque join — see the jsbridge.c note). Returns 1
 * with out filled, 0 on refusal. Never throws. */
int jsbridge_hist_resolve_url(JsBridge *b, const char *raw,
                              char *out, size_t outLen);

#endif /* PLUTO_JSBIDGE_INTERNAL_H */
