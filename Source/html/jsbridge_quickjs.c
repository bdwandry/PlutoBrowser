/*
 * PlutoBrowser — jsbridge_quickjs.c
 * QuickJS (bellard 2026-06-04) engine implementation behind the
 * JsEngineImpl vtable (engine vendored STOCK under Source/js/QuickJS —
 * never modified; CONFIG_VERSION comes from the Makefile).
 *
 * QuickJS facts this file relies on (verified against the stock 2026-06-04
 * quickjs.h):
 *   - JS_NewRuntime2(&mf, opaque) routes ALL engine memory through the
 *     supplied JSMallocFunctions (here: SDK realloc); JSMallocState.opaque
 *     carries the JsBridge* for the allocators and JS_GetRuntimeOpaque
 *     retrieves it for bridge_of().
 *   - JS_SetMaxStackSize bounds the CONFIG_STACK_CHECK probe (parser
 *     recursion + a few interpreter paths run on the real task stack).
 *   - JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL) accepts a
 *     length-delimited span (no NUL copy needed) and returns the completion
 *     value; exceptions are popped with JS_GetException (JS_ToCString of an
 *     Error value renders "Error: message", mirroring Duktape's safe_to_string).
 *   - Element wrappers are real class instances: ONE prototype carries the
 *     accessor properties + methods; instances hold the DomNode* as the
 *     class opaque. Unknown writes land as ordinary own properties (parity
 *     with the muJS/Duktape bridges).
 *   - Listener functions are JS_DupValue-pinned in our impl state and
 *     JS_FreeValue'd at close; dispatch calls them with JS_Call. The runtime
 *     free in close() releases everything else (wrappers, bytecode).
 *   - JS_RunGC(rt) is the per-script sweep (mirrors the other bridges).
 *
 * DOM surface is identical to jsbridge_mujs.c / jsbridge_duktape.c: element
 * wrappers expose tagName/id/textContent/innerHTML/parentNode/
 * childElementCount/children/nodeType plus getAttribute/setAttribute/
 * removeAttribute/appendChild/removeChild/addEventListener/
 * getElementsByTagName; document exposes getElementById/createElement/
 * createTextNode/title/write/writeln/addEventListener/body; globals add
 * window/navigator/console/location/alert/confirm/prompt/timers.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "jsbridge_bundler.h"
#include "quickjs.h"
#include "../core/logger.h"
#include "../util/strbuf.h"
#include "../core/pluto_mem.h"
#include "../core/pluto_spill.h"
#include "../core/url.h"

/* SDK realloc(0) wrapper (defined in main.c) — same convention as core/url.c. */
extern void pluto_free(void *p);
#include "../html/dom.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)
/* The engine TUs compile quickjs.c through qjs_shim_quickjs.c, which renames
 * js_free/js_malloc/js_realloc to pluto_qjs_* at the source level. This bridge
 * TU includes the headers un-renamed, so declare the engine's own free here —
 * required for buffers QuickJS allocates from its INTERNAL rt allocator
 * (JS_WriteObject output), which the funnel must never free directly. */
struct JSContext;
extern void pluto_qjs_free(struct JSContext *ctx, void *ptr);

/* Engine-wide budget: total engine heap (device heap is ~3MB for the whole
 * browser) and parser/interpreter stack budget. QuickJS's probe guarantees
 * JS never consumes more than stack_size bytes of C stack below the attach
 * point (stack_limit = stack_top - stack_size), so on DEVICE the budget is
 * bounded by the 61.8KB gameTask stack (attach baseline ≈ 2.5KB, ~19KB
 * margin); host/simulator builds have an 8MB stack and -O0 frames are much
 * fatter, so they get a larger budget for the same suite. Override via
 * -DQJS_STACK_LIMIT= for experiments — never ship device above ~48KB. */
#ifndef QJS_MEM_LIMIT
/* R26g: 4.6MB → 5.5MB — the JS_PTR64 struct-JSValue path (16B values vs 8B
 * nan-boxing) grows value-carrying engine heap ~1.5x. R18 measured a 3.94MB
 * boxing peak for the 502KB bryanwandrych.com bundle (a SPLIT webpack bundle
 * keeps every module live in the shared registry); the struct path needs the
 * headroom. The REAL memory gate remains the funnel budget (6.5MB device)
 * that every QuickJS allocation flows through — this inner limit only
 * backstops contained refusal instead of engine OOM. */
#define QJS_MEM_LIMIT (5500u * 1024u)
#endif
#ifdef TARGET_PLAYDATE
/* Device: 61.8KB game-task stack. The probe counts from the ATTACH point,
 * but evals run DEEPER (document_parse → dispatch → run_script → split →
 * JS_Eval), and device fault evidence (INVPC with a corrupted return
 * address ~3s into a whole-file parse, runs 9–12) shows the old 40KB
 * budget outran the real stack at eval depth. 28KB keeps the probe inside
 * the task stack with wide margin for the C chain above the engine. The
 * R18 geometry experiments (qexp) proved every unit of the 502KB site
 * bundle parses+runs containedly at far smaller budgets; deep units
 * degrade to a contained SyntaxError, never a fault.
 * R26g MEASURED FIX (2026-10-05): the attach chain is 24.3KB deep on the
 * current tree (fp=0x20009220 vs the 0x20000000 task-stack base, 61.8KB
 * total) — the old 40KB probe put its floor BELOW the real stack bottom
 * (24.3+40 > 61.8), so recursion walked off the stack before any probe
 * fired (wild jumps, wedges, empty crashlogs), and 28KB left only ~9.5KB
 * of true margin which the parse overshot between probes. 20KB puts the
 * floor 4.3KB above the bottom: deep units throw CONTAINEDLY, the split
 * skips them (failSegs accounting), and the page renders. The durable fix
 * is dispatching evals from the frame root to shrink the 24.3KB chain. */
#define QJS_STACK_LIMIT_DFL (20u * 1024u)
#else
/* Host/sim lab builds (ASan/UBSan at -O0): sanitizer instrumentation and
 * unoptimized frames inflate QuickJS's parser AND runtime call frames
 * several-fold, so the old 64KB budget tripped JS_ThrowStackOverflow on the
 * first trivial script (measured: `var a=6*7;` failed under UBSan -O0; the
 * full about:javascript suite, including its runtime fib(10) recursion,
 * needs ~2MB when the walker's own -O0 frames are factored in). The host
 * stack is 8MB and this budget is only QuickJS's own probe bound — it trips
 * before real exhaustion. The device budget above is the one that protects
 * real hardware. */
#define QJS_STACK_LIMIT_DFL (4096u * 1024u)
#endif
#ifndef QJS_STACK_LIMIT
#define QJS_STACK_LIMIT QJS_STACK_LIMIT_DFL
#endif

/* ── R26g: per-eval wall-clock budget (device) ──────────────────────────
 * QuickJS's interrupt handler fires in the INTERPRETER loop (never during
 * compile), so arming it around each eval bounds EXECUTION: a runaway
 * mount loop aborts containedly ("interrupted") instead of blocking the
 * main loop — the 2026-10-04 live run wedged >15min with the USB event
 * loop unresponsive (no terminate, no datadisk switch). Compile time is
 * intentionally unbounded (no compile-time interrupt exists in QuickJS);
 * the split path keeps compile units small. 15s ≫ per-segment mount work
 * (ms-scale) and ≪ any plausible main-loop heartbeat expectation. */
#ifdef TARGET_PLAYDATE
#define QJS_EVAL_BUDGET_MS 15000UL
static unsigned long qjs_eval_deadline_ms = 0;
/* R26g stack-geometry telemetry (declared unconditionally so the host
 * e4 harness, which compiles all bridges, links) */
static void *g_qjs_init_fp;
static unsigned long qjs_now_ms(void)
{
    PlaydateAPI *pd = pluto_pd();
    return (pd && pd->system)
               ? (unsigned long)pd->system->getCurrentTimeMilliseconds()
               : 0;
}
static int qjs_interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (void)opaque;
    if (qjs_eval_deadline_ms == 0)
        return 0;
    return (qjs_now_ms() > qjs_eval_deadline_ms) ? 1 : 0;
}
static void qjs_eval_budget_arm(void)
{
    unsigned long now = qjs_now_ms();
    qjs_eval_deadline_ms = now ? now + QJS_EVAL_BUDGET_MS : 0;
}
static void qjs_eval_budget_disarm(void) { qjs_eval_deadline_ms = 0; }
#else
static void *g_qjs_init_fp;
static unsigned long qjs_now_ms(void) { return 0; }
static void qjs_eval_budget_arm(void) {}
static void qjs_eval_budget_disarm(void) {}
#endif


/* Engine-private state (b->implState). */
typedef struct
{
    JSRuntime *rt;
    JSContext *ctx;
    JSClassID elClass;
    JSValue elProto; /* element prototype: accessors + methods (once) */
    /* Pinned listener functions; JsListener.ref holds the index. The dup
     * keeps them alive against GC; freed at close. */
    JSValue fns[JSBRIDGE_LISTENERS_MAX];
    /* Pinned timer callbacks; JsTimer.ref holds the tfn[] slot index.
     * Same dup/ownership contract as fns[] — freed at close. */
    JSValue tfn[JSBRIDGE_TIMERS_MAX];
    /* XHR + fetch pins: completion handlers / wrapper objects / promise
     * resolving functions. JsHttpRequest.fnRef holds slot+1 (slot 0
     * RESERVED — the router's NULL-refusal rule); objRef likewise. */
    JSClassID xhrClass;
    JSValue xhrProto;
    JSValue xfn[JSBRIDGE_XHR_MAX];
    JSValue xobj[JSBRIDGE_XHR_MAX];
    JSValue xres[JSBRIDGE_XHR_MAX][2];
    /* SW5: classList + style object classes (shared protos, opaque = node). */
    JSClassID clClass;
    JSValue clProto;
    JSClassID stClass;
    JSValue stProto;
    /* R18: active bundler-split module registry name. When non-empty,
     * segments of one bundle share this global across engine brackets so
     * modules can require each other exactly as authored. */
    char splitMapName[24];
} QjsState;

/* ── allocators: route engine memory through the SDK ────────────────────── */
static void *qjs_sdk_malloc(JSMallocState *s, size_t size)
{
    (void)s;
    return JMalloc(size);
}
static void qjs_sdk_free(JSMallocState *s, void *ptr)
{
    (void)s;
    if (ptr)
    {
        JFree(ptr);
    }
}
static void *qjs_sdk_realloc(JSMallocState *s, void *ptr, size_t size)
{
    (void)s;
    return pluto_mem_realloc(ptr, (unsigned)size);
}
/* Conservative usable-size: 0 makes QuickJS account the requested size only
 * (malloc_usable_size is not portable across the host + device libcs). */
static size_t qjs_sdk_usable_size(const void *ptr)
{
    (void)ptr;
    return 0;
}

static const JSMallocFunctions qjs_mf = {
    qjs_sdk_malloc, qjs_sdk_free, qjs_sdk_realloc, qjs_sdk_usable_size};

#ifdef PLUTO_QJS_PROBE_SPAN
#include "../tests/span1_bytes.h"
/* R26g device bisect: compile a baked-in bundle span from boot-shallow
 * depth in a FRESH runtime (no page, no walker, no bridge state). If this
 * faults, the crash is inside QuickJS's ARM32 compile itself; if it
 * passes, the browser context is a necessary ingredient. SPAN1 prefix is
 * selectable via -DPLUTO_QJS_PROBE_SPAN_PREFIX_LEN=N. */
void qjs_probe_span_compile(void);
void qjs_probe_span_compile(void)
{
    unsigned len = SPAN1_BYTES_LEN;
#ifdef PLUTO_QJS_PROBE_SPAN_PREFIX_LEN
    if ((unsigned)PLUTO_QJS_PROBE_SPAN_PREFIX_LEN < len)
        len = (unsigned)PLUTO_QJS_PROBE_SPAN_PREFIX_LEN;
#endif
    logger_log("[qjsprobe] compiling %u/%d bytes", len, SPAN1_BYTES_LEN);
    JSRuntime *rt = JS_NewRuntime2(&qjs_mf, NULL);
    if (!rt)
    {
        logger_log("[qjsprobe] runtime init failed");
        return;
    }
    JS_SetMaxStackSize(rt, QJS_STACK_LIMIT);
    JS_SetMemoryLimit(rt, QJS_MEM_LIMIT);
#ifdef TARGET_PLAYDATE
    JS_SetInterruptHandler(rt, qjs_interrupt_handler, NULL);
#endif
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx)
    {
        logger_log("[qjsprobe] context init failed");
        JS_FreeRuntime(rt);
        return;
    }
    char *buf = (char *)JMalloc(len + 1);
    memcpy(buf, span1_bytes, len);
    buf[len] = '\0';
    JSValue v = JS_Eval(ctx, buf, len, "[probe]", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v))
    {
        char msg[96];
        msg[0] = '\0';
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        snprintf(msg, sizeof(msg), "%s", s ? s : "(unprintable)");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        logger_log("[qjsprobe] exception: %s", msg);
    }
    else
    {
        logger_log("[qjsprobe] compiled+ran OK (%u bytes)", len);
        JS_FreeValue(ctx, v);
    }
    JFree(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    logger_log("[qjsprobe] done");
}
#endif

static JsBridge *bridge_of(JSContext *ctx)
{
    /* The JsBridge* rides in the runtime opaque (set at JS_NewRuntime2). */
    return (JsBridge *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

/* Take the pending exception's printable text into b->lastError. Some
 * exceptions have no string form (bare `throw null`) — or the engine is so
 * stack/OOM-starved it cannot format its own Error (observed when lab-build
 * sanitizer frames trip the parser's stack probe; the failure then arrived
 * here as an opaque "exception"). Consume any secondary exception the
 * failed conversion raised, label unprintable Errors distinctly, and always
 * leave the bridge error state populated. */
static void qjs_take_exception_text(JsBridge *b, JSContext *ctx)
{
    logger_log("[js] qjs: exc-text begin");
    JSValue exc = JS_GetException(ctx);
    const char *msg = NULL;
    int owned = 0;
    if (!JS_IsNull(exc) && !JS_IsUndefined(exc))
    {
        msg = JS_ToCString(ctx, exc);
        owned = (msg != NULL);
    }
    logger_log("[js] qjs: exc-text cstring owned=%d", owned);
    if (!msg)
    {
        JSValue sec = JS_GetException(ctx);
        JS_FreeValue(ctx, sec);
        msg = JS_IsError(ctx, exc) ? "exception (Error unprintable)"
                                   : "exception";
    }
    bridge_take_error_text(b, msg);
    /* R17 diagnostics: Error objects carry a `stack` property whose frames
     * name the throw site (function + file:line:col). Log a bounded,
     * one-line excerpt so a CSR page's first-failure point lands in
     * pluto.log without a debugger — site-agnostic, every page benefits. */
    if (JS_IsError(ctx, exc))
    {
        JSValue stk = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stk) && !JS_IsException(stk) && JS_IsString(stk))
        {
            const char *st = JS_ToCString(ctx, stk);
            if (st)
            {
                char excerpt[440];
                size_t n = strlen(st);
                if (n > sizeof(excerpt) - 1)
                    n = sizeof(excerpt) - 1;
                memcpy(excerpt, st, n);
                excerpt[n] = '\0';
                for (size_t i = 0; i < n; i++)
                {
                    if ((unsigned char)excerpt[i] < 0x20)
                        excerpt[i] = ' '; /* flatten to one log line */
                }
                logger_log("[js] qjs: exc stack: %s", excerpt);
                JS_FreeCString(ctx, st);
            }
        }
        JS_FreeValue(ctx, stk);
    }
    logger_log("[js] qjs: exc-text stored");
    if (owned)
    {
        JS_FreeCString(ctx, msg);
    }
    JS_FreeValue(ctx, exc);
    logger_log("[js] qjs: exc-text done");
}

#define BUDGET_OR_THROW(b)                                                    \
    do                                                                        \
    {                                                                         \
        if (!budget_take(b))                                                  \
        {                                                                     \
            JS_Throw((JSContext *)((QjsState *)(b)->implState)->ctx,          \
                     JS_NewString(((QjsState *)(b)->implState)->ctx,         \
                                  "script did too much"));                   \
            return JS_EXCEPTION;                                              \
        }                                                                     \
    } while (0)

/* ── element class + prototype ───────────────────────────────────────────── */

static void qjs_el_finalizer(JSRuntime *rt, JSValue val)
{
    /* DomNode* is owned by the browser — nothing to release here. */
    (void)rt;
    (void)val;
}

static DomNode *this_node(JSContext *ctx, JSValueConst this_val)
{
    return (DomNode *)JS_GetOpaque(this_val, ((QjsState *)bridge_of(ctx)->implState)->elClass);
}

static DomNode *arg_node(JSContext *ctx, JSValueConst *argv, QjsState *st)
{
    return (DomNode *)JS_GetOpaque(argv[0], st->elClass);
}

/* Push a fresh wrapper for `node` (NULL → null); defined after the class
 * helpers, forward-declared here for the getters below. */
static JSValue js_qjs_push_element(JsBridge *b, DomNode *node);

/* Accessor getters (this = element instance). */
static JSValue qjs_el_get_tagName(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    if (!n || n->kind != DOM_ELEMENT)
    {
        return JS_UNDEFINED;
    }
    char up[32];
    size_t tl = strlen(n->tag);
    if (tl < sizeof(up))
    {
        for (size_t i = 0; i < tl; i++)
        {
            up[i] = (n->tag[i] >= 'a' && n->tag[i] <= 'z')
                        ? (char)(n->tag[i] - 32)
                        : n->tag[i];
        }
        up[tl] = '\0';
        return JS_NewString(ctx, up);
    }
    return JS_NewString(ctx, n->tag);
}
static JSValue qjs_el_get_id(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    const char *v = n ? dom_get_attr(n, "id") : NULL;
    return JS_NewString(ctx, v ? v : "");
}

/* ── R17: anchor URL property getters ─────────────────────────────────────
 * Legacy browser behavior: assigning element.href parses the string into
 * URL components, so the time-worn
 *   var a = document.createElement('a'); a.setAttribute('href', u);
 *   a.pathname / a.protocol / a.host ...
 * trick (axios resolveURL, old jQuery ajax, history polyfills, React
 * Router) works. Our bridge stores attributes verbatim, so these getters
 * parse the href attribute on read through the shared URL resolver —
 * category-level behavior, no site knowledge. Non-anchor elements and
 * missing/relative hrefs resolve against the page base URL like a real
 * browser. The decomposition attributes apply to <a>, <link>, <base> and
 * <area> per the HTML "URL decomposition attributes" rules. */
static int qjs_is_url_element(const DomNode *n)
{
    return strcmp(n->tag, "a") == 0 || strcmp(n->tag, "A") == 0 ||
           strcmp(n->tag, "link") == 0 || strcmp(n->tag, "LINK") == 0 ||
           strcmp(n->tag, "base") == 0 || strcmp(n->tag, "BASE") == 0 ||
           strcmp(n->tag, "area") == 0 || strcmp(n->tag, "AREA") == 0;
}
static void qjs_anchor_url_parts(JSContext *ctx, JSValueConst this_val,
                                 UrlParsed *out, int *ok)
{
    DomNode *n = this_node(ctx, this_val);
    *ok = 0;
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        !qjs_is_url_element(n))
    {
        return;
    }
    const char *href = dom_get_attr(n, "href");
    JsBridge *b = bridge_of(ctx);
    const char *base = (b && b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl
                                                           : "about:blank";
    char *abs = url_resolve(base, href ? href : "");
    if (!abs)
    {
        return;
    }
    if (url_parse(abs, out) == 0)
    {
        *ok = 1;
    }
    pluto_free(abs);
}

static JSValue qjs_el_get_a_href(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    /* Normalized absolute href, like a browser after href assignment. */
    return JS_NewString(ctx, ok ? u.normalized : "");
}
static JSValue qjs_el_get_a_protocol(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    char buf[16];
    snprintf(buf, sizeof(buf), "%s:", ok ? u.scheme : "");
    return JS_NewString(ctx, buf);
}
static JSValue qjs_el_get_a_host(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    if (!ok)
    {
        return JS_NewString(ctx, "");
    }
    if (u.port == 80 || u.port == 443 || u.port == 0)
    {
        return JS_NewString(ctx, u.host);
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%s:%d", u.host, u.port);
    return JS_NewString(ctx, buf);
}
static JSValue qjs_el_get_a_hostname(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    return JS_NewString(ctx, ok ? u.host : "");
}
static JSValue qjs_el_get_a_port(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    if (!ok || u.port == 80 || u.port == 443 || u.port == 0)
    {
        return JS_NewString(ctx, "");
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", u.port);
    return JS_NewString(ctx, buf);
}
static JSValue qjs_el_get_a_pathname(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    return JS_NewString(ctx, ok ? u.path : "");
}
static JSValue qjs_el_get_a_search(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    char buf[260];
    snprintf(buf, sizeof(buf), "%s%s", ok && u.query[0] ? "?" : "",
             ok ? u.query : "");
    return JS_NewString(ctx, buf);
}
static JSValue qjs_el_get_a_hash(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    UrlParsed u;
    int ok;
    qjs_anchor_url_parts(ctx, this_val, &u, &ok);
    char buf[132];
    snprintf(buf, sizeof(buf), "%s%s", ok && u.hash[0] ? "#" : "",
             ok ? u.hash : "");
    return JS_NewString(ctx, buf);
}

/* R17: href setter — browsers store the RAW assigned string as the
 * content attribute (the .href property getter re-resolves it against the
 * base on every read), so getAttribute mirrors exactly what the page wrote
 * while the property reflects the absolute URL. */
static JSValue qjs_el_set_a_href(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx, this_val);
    if (!n || n->kind != DOM_ELEMENT || !n->tag || !qjs_is_url_element(n))
    {
        return JS_UNDEFINED; /* silently ignored on non-URL elements */
    }
    const char *v = JS_ToCString(ctx, argv[0]);
    if (!v)
    {
        return JS_EXCEPTION;
    }
    dom_set_attr(b->dom, n, "href", v);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}
static JSValue qjs_el_get_textContent(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    char buf[512];
    if (n)
    {
        doc_concat_node_text(n, buf, sizeof(buf));
        return JS_NewString(ctx, buf);
    }
    return JS_NewString(ctx, "");
}
/* UNSUPPORTED (no subtree serializer): degrades to text — engine parity. */
static JSValue qjs_el_get_innerHTML(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return qjs_el_get_textContent(ctx, this_val, argc, argv);
}
static JSValue qjs_el_get_parentNode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    return js_qjs_push_element(bridge_of(ctx), n ? n->parent : NULL);
}
static JSValue qjs_el_get_childElementCount(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    int c = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                c++;
            }
        }
    }
    return JS_NewInt32(ctx, c);
}
static JSValue qjs_el_get_children(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    int k = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                JS_SetPropertyUint32(ctx, arr, (uint32_t)k++,
                                     js_qjs_push_element(b, n->children[i]));
            }
        }
    }
    return arr;
}
static JSValue qjs_el_get_nodeType(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    return JS_NewInt32(ctx, (n && n->kind == DOM_ELEMENT) ? 1 : 3);
}
/* R17: ownerDocument — React 18's event delegation reads
 * node.ownerDocument (createRoot listeners on the document); returning the
 * page's own document object keeps that path well-defined. */
static JSValue qjs_el_get_ownerDocument(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue doc = JS_GetPropertyStr(ctx, glob, "document");
    JS_FreeValue(ctx, glob);
    return doc;
}

/* R17: getBoundingClientRect — the device has a single flowing 400x240
 * layout with no per-node geometry tracking, so the honest resolved rect
 * is the origin (all zeros): element math (M-UI transitions, sticky
 * headers, tooltip positioning) stays well-defined and degrades to "no
 * offset" instead of throwing. */
static JSValue qjs_el_getBoundingClientRect(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    JSValue r = JS_NewObject(ctx);
    const char *fields[] = {"top", "left", "right", "bottom",
                            "width", "height", "x", "y"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        JS_SetPropertyStr(ctx, r, fields[i], JS_NewFloat64(ctx, 0.0));
    }
    return r;
}

/* R17: nodeName — W3C: for HTMLElements, nodeName === tagName (lowercase
 * in our tree). React reads node.nodeName for target classification. */
static JSValue qjs_el_get_nodeName(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    return JS_NewString(ctx, (n && n->tag) ? n->tag : "");
}

/* Accessor setters (value in argv[0]). */
static JSValue qjs_el_set_id(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *v = JS_ToCString(ctx, argv[0]);
        if (!v)
        {
            return JS_EXCEPTION;
        }
        int rc = dom_set_attr(b->dom, n, "id", v);
        JS_FreeCString(ctx, v);
        if (rc != 0)
        {
            JS_Throw(ctx, JS_NewString(ctx, "id assignment failed"));
            return JS_EXCEPTION;
        }
    }
    return JS_UNDEFINED;
}
static JSValue qjs_el_set_textContent(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *v = JS_ToCString(ctx, argv[0]);
        if (!v)
        {
            return JS_EXCEPTION;
        }
        int rc = dom_set_text(b->dom, n, v);
        JS_FreeCString(ctx, v);
        if (rc != 0)
        {
            JS_Throw(ctx, JS_NewString(ctx, "textContent assignment failed"));
            return JS_EXCEPTION;
        }
    }
    return JS_UNDEFINED;
}
/* SW5: REAL markup assignment through the router (was textContent). */
static JSValue qjs_el_set_innerHTML(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    if (n && n->kind == DOM_ELEMENT)
    {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s)
        {
            return JS_EXCEPTION;
        }
        int rc = jsbridge_el_set_inner_html(b, n, s, len);
        JS_FreeCString(ctx, s);
        if (rc != 0)
        {
            JS_Throw(ctx, JS_NewString(ctx, "innerHTML assignment failed"));
            return JS_EXCEPTION;
        }
    }
    return JS_UNDEFINED;
}

/* Element methods. */
static JSValue qjs_el_getAttribute(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k)
    {
        return JS_EXCEPTION;
    }
    const char *v = n ? dom_get_attr(n, k) : NULL;
    JS_FreeCString(ctx, k);
    if (v)
    {
        return JS_NewString(ctx, v);
    }
    return JS_NULL;
}
static JSValue qjs_el_setAttribute(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    if (!k || !v)
    {
        if (k)
        {
            JS_FreeCString(ctx, k);
        }
        if (v)
        {
            JS_FreeCString(ctx, v);
        }
        return JS_EXCEPTION;
    }
    BUDGET_OR_THROW(b);
    int rc = (!n) ? -1 : dom_set_attr(b->dom, n, k, v);
    JS_FreeCString(ctx, k);
    JS_FreeCString(ctx, v);
    if (rc != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "setAttribute failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}
static JSValue qjs_el_removeAttribute(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc;
    DomNode *n = this_node(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k)
    {
        return JS_EXCEPTION;
    }
    if (n)
    {
        dom_remove_attr(n, k);
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}
static JSValue qjs_el_appendChild(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    DomNode *c = arg_node(ctx, argv, st);
    if (!n || !c || dom_append_child(b->dom, n, c) != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "appendChild failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}
static JSValue qjs_el_removeChild(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    DomNode *c = arg_node(ctx, argv, st);
    if (!n || !c || dom_remove_child(n, c) != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "removeChild failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}
/* ── SW5: navigation accessors + insertBefore + querySelector(All) ──────── */
static JSValue qjs_el_get_firstElementChild(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    return js_qjs_push_element(bridge_of(ctx),
                               n ? dom_first_element_child(n) : NULL);
}
static JSValue qjs_el_get_classList(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = this_node(ctx, this_val);
    JSValue obj = JS_NewObjectProtoClass(ctx, st->clProto, st->clClass);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_SetOpaque(obj, n);
    return obj;
}
static JSValue qjs_el_get_style(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = this_node(ctx, this_val);
    JSValue obj = JS_NewObjectProtoClass(ctx, st->stProto, st->stClass);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_SetOpaque(obj, n);
    return obj;
}

/* Build the classList + style prototypes once (SW5). Mirrors
 * qjs_build_element_proto's class-registration pattern. */
static JSValue qjs_cl_add(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv); /* fwd: defined below */
static JSValue qjs_cl_remove(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv);
static JSValue qjs_cl_toggle(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv);
static JSValue qjs_cl_contains(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv);
static JSValue qjs_cl_item(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv);
static JSValue qjs_cl_length(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv);
/* Inline-style property table (shared by the getter/setter accessors via
 * magic) — used by qjs_build_sw5_protos below and the accessors above it. */
static const char *const QJS_STYLE_PROPS[] = {
    "display", "visibility", "text-align", "font-weight", "font-style",
    "text-decoration", "color", "background"};
/* The accessor wrappers take the ENGINE's magic signatures exactly
 * (quickjs.h JSCFunctionType): getter_magic(ctx, this_val, magic) and
 * setter_magic(ctx, this_val, val, magic) — the setter's value arrives as
 * a PARAMETER, not through argv. Cast to JSCFunction* at JS_NewCFunction2. */
static JSValue qjs_style_get_wrap(JSContext *ctx, JSValueConst this_val,
                                  int magic);
static JSValue qjs_style_set_wrap(JSContext *ctx, JSValueConst this_val,
                                  JSValueConst val, int magic);
static int qjs_build_sw5_protos(JSContext *ctx, QjsState *st)
{
    JSClassID cid = 0;
    JSClassDef cldef = {"ClassList", NULL, NULL, NULL, NULL};
    if (JS_NewClassID(&cid) == JS_INVALID_CLASS_ID || cid == JS_INVALID_CLASS_ID)
    {
        return -1;
    }
    if (JS_NewClass(JS_GetRuntime(ctx), cid, &cldef) != 0)
    {
        return -1;
    }
    st->clClass = cid;
    st->clProto = JS_NewObjectProto(ctx, JS_NULL);
    struct
    {
        const char *name;
        JSCFunction *fn;
        int nargs;
    } clm[] = {
        {"add", qjs_cl_add, 1},
        {"remove", qjs_cl_remove, 1},
        {"toggle", qjs_cl_toggle, 1},
        {"contains", qjs_cl_contains, 1},
        {"item", qjs_cl_item, 1},
    };
    for (size_t i = 0; i < sizeof(clm) / sizeof(clm[0]); i++)
    {
        JS_SetPropertyStr(ctx, st->clProto, clm[i].name,
                          JS_NewCFunction(ctx, clm[i].fn, clm[i].name,
                                          clm[i].nargs));
    }
    JSAtom cla = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, st->clProto, cla,
                            JS_NewCFunction(ctx, qjs_cl_length, "get length", 0),
                            JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, cla);

    JSClassID sid = 0;
    JSClassDef stdef = {"Style", NULL, NULL, NULL, NULL};
    if (JS_NewClassID(&sid) == JS_INVALID_CLASS_ID || sid == JS_INVALID_CLASS_ID)
    {
        return -1;
    }
    if (JS_NewClass(JS_GetRuntime(ctx), sid, &stdef) != 0)
    {
        return -1;
    }
    st->stClass = sid;
    st->stProto = JS_NewObjectProto(ctx, JS_NULL);
    for (int i = 0;
         i < (int)(sizeof(QJS_STYLE_PROPS) / sizeof(QJS_STYLE_PROPS[0])); i++)
    {
        JSAtom atom = JS_NewAtom(ctx, QJS_STYLE_PROPS[i]);
        JS_DefinePropertyGetSet(
            ctx, st->stProto, atom,
            JS_NewCFunction2(ctx, (JSCFunction *)qjs_style_get_wrap, "get", 0,
                             JS_CFUNC_getter_magic, i),
            JS_NewCFunction2(ctx, (JSCFunction *)qjs_style_set_wrap, "set", 1,
                             JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }
    return 0;
}
static JSValue qjs_el_get_nextElementSibling(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    DomNode *n = this_node(ctx, this_val);
    return js_qjs_push_element(bridge_of(ctx),
                               n ? dom_next_element_sibling(n) : NULL);
}

/* querySelector emit: QuickJS wrappers are RETAINED into the QjsState qsel
 * pin array so the array survives until the engine returns (JS_SetProperty
 * on the array keeps a strong ref anyway; the pin is belt-and-braces for
 * GC across the walk). */
typedef struct
{
    JSContext *ctx;
    QjsState *st;
    JSValue arr;
    int k;
} QjsQsEmit;

static int qs_emit_quickjs(void *elp, void *ud)
{
    QjsQsEmit *e = (QjsQsEmit *)ud;
    JSValue v = js_qjs_push_element(bridge_of(e->ctx), (DomNode *)elp);
    if (JS_IsException(v))
    {
        return -1; /* stop the walk; exception propagates */
    }
    JS_SetPropertyUint32(e->ctx, e->arr, (uint32_t)e->k++, v);
    return 0;
}

static JSValue qjs_el_insertBefore(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    DomNode *c = arg_node(ctx, argv, st);
    DomNode *ref = NULL;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
    {
        ref = (DomNode *)JS_GetOpaque(argv[1], st->elClass);
    }
    if (!n || !c || dom_insert_before(b->dom, n, c, ref) != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "insertBefore failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue qjs_el_querySelectorAll(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    size_t slen = 0;
    const char *sel = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!sel)
    {
        return JS_EXCEPTION;
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    JSValue arr = JS_NewArray(ctx);
    QjsQsEmit e = {ctx, (QjsState *)b->implState, arr, 0};
    jsbridge_el_query_selector_all(b, n, sel, scratch, sizeof(scratch),
                                   qs_emit_quickjs, &e);
    JS_FreeCString(ctx, sel);
    return arr;
}

static JSValue qjs_el_querySelector(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
    {
        return JS_EXCEPTION;
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, n, sel, scratch, sizeof(scratch));
    JS_FreeCString(ctx, sel);
    return js_qjs_push_element(b, hit);
}

/* ── SW5 (O4): classList — a dedicated class instance (opaque = DomNode*)
 * with ONE shared prototype of methods; built fresh per access. */
static DomNode *qjs_sw5_node_of(JSContext *ctx, JSValueConst this_val,
                                JSClassID cid)
{
    return (DomNode *)JS_GetOpaque(this_val, cid);
}

static JSValue qjs_cl_add(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok)
    {
        return JS_EXCEPTION;
    }
    int rc = jsbridge_el_class_add(b, n, tok);
    JS_FreeCString(ctx, tok);
    if (rc != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "classList.add failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue qjs_cl_remove(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok)
    {
        return JS_EXCEPTION;
    }
    int rc = jsbridge_el_class_remove(b, n, tok);
    JS_FreeCString(ctx, tok);
    if (rc != 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "classList.remove failed"));
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue qjs_cl_toggle(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok)
    {
        return JS_EXCEPTION;
    }
    int rc = jsbridge_el_class_toggle(b, n, tok);
    JS_FreeCString(ctx, tok);
    if (rc < 0)
    {
        JS_Throw(ctx, JS_NewString(ctx, "classList.toggle failed"));
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, rc == 1);
}

static JSValue qjs_cl_contains(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok)
    {
        return JS_EXCEPTION;
    }
    int has = jsbridge_el_class_has(n, tok);
    JS_FreeCString(ctx, tok);
    return JS_NewBool(ctx, has);
}

static JSValue qjs_cl_item(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    int idx = -1;
    if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
    {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    const char *cls = (n && n->kind == DOM_ELEMENT) ? dom_get_attr(n, "class")
                                                    : NULL;
    int k = 0;
    const char *p = cls;
    while (p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }
        int n2 = 0;
        while (p[n2] && !strchr(" \t\n\r\f", p[n2]))
        {
            n2++;
        }
        if (k++ == idx)
        {
            char tok[64];
            int cpy = n2 < (int)sizeof(tok) - 1 ? n2 : (int)sizeof(tok) - 1;
            memcpy(tok, p, (size_t)cpy);
            tok[cpy] = '\0';
            return JS_NewString(ctx, tok);
        }
        p += n2;
    }
    return JS_NULL;
}

static JSValue qjs_cl_length(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->clClass);
    const char *cls = (n && n->kind == DOM_ELEMENT) ? dom_get_attr(n, "class")
                                                    : NULL;
    int k = 0;
    const char *p = cls;
    while (p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }
        k++;
        while (*p && !strchr(" \t\n\r\f", *p))
        {
            p++;
        }
    }
    return JS_NewInt32(ctx, k);
}

/* style object: shared prototype with getter/setter accessors over
 * QJS_STYLE_PROPS (the table lives just above qjs_build_sw5_protos). */

static void qjs_style_read_prop(const DomNode *n, const char *prop, char *out,
                                size_t outsz)
{
    out[0] = '\0';
    if (!n || n->kind != DOM_ELEMENT)
    {
        return;
    }
    const char *st = dom_get_attr(n, "style");
    if (!st)
    {
        return;
    }
    size_t plen = strlen(prop);
    const char *p = st;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        const char *seg = p;
        while (*p && *p != ';')
        {
            p++;
        }
        const char *colon = memchr(seg, ':', (size_t)(p - seg));
        if (colon && (size_t)(colon - seg) == plen &&
            strncmp(seg, prop, plen) == 0)
        {
            const char *vs = colon + 1;
            const char *ve = p;
            while (vs < ve && (*vs == ' ' || *vs == '\t'))
            {
                vs++;
            }
            while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t'))
            {
                ve--;
            }
            size_t vn = (size_t)(ve - vs);
            if (vn >= outsz)
            {
                vn = outsz - 1;
            }
            memcpy(out, vs, vn);
            out[vn] = '\0';
            return;
        }
        if (*p)
        {
            p++;
        }
    }
}

static void qjs_style_write_prop(JsBridge *b, DomNode *n, const char *prop,
                                 const char *value)
{
    static char buf[512]; /* static: off the device game-task stack */
    size_t off = 0;
    size_t plen = strlen(prop);
    buf[0] = '\0';
    const char *st = dom_get_attr(n, "style");
    const char *p = st;
    while (st && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        const char *seg = p;
        while (*p && *p != ';')
        {
            p++;
        }
        const char *colon = memchr(seg, ':', (size_t)(p - seg));
        size_t segLen = (size_t)(p - seg);
        if (!colon || (size_t)(colon - seg) != plen ||
            strncmp(seg, prop, plen) != 0)
        {
            while (segLen > 0 && (seg[segLen - 1] == ' ' ||
                                  seg[segLen - 1] == '\t'))
            {
                segLen--;
            }
            if (segLen && off + segLen + 2 < sizeof(buf))
            {
                if (off)
                {
                    buf[off++] = ';';
                }
                memcpy(buf + off, seg, segLen);
                off += segLen;
                buf[off] = '\0';
            }
        }
        if (*p)
        {
            p++;
        }
    }
    if (value && value[0])
    {
        int n1 = snprintf(buf + off, sizeof(buf) - off, "%s%s: %s",
                          off ? ";" : "", prop, value);
        if (n1 < 0 || (size_t)n1 >= sizeof(buf) - off)
        {
            return;
        }
    }
    dom_set_attr(b->dom, n, "style", buf);
}

static JSValue qjs_style_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->stClass);
    char val[128];
    qjs_style_read_prop(n, QJS_STYLE_PROPS[magic], val, sizeof(val));
    return JS_NewString(ctx, val);
}

static JSValue qjs_style_get_wrap(JSContext *ctx, JSValueConst this_val,
                                  int magic)
{
    return qjs_style_get(ctx, this_val, magic);
}

static JSValue qjs_style_set_wrap(JSContext *ctx, JSValueConst this_val,
                                  JSValueConst val, int magic)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, this_val, st->stClass);
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *v = JS_ToCString(ctx, val);
        if (!v)
        {
            return JS_EXCEPTION;
        }
        qjs_style_write_prop(b, n, QJS_STYLE_PROPS[magic], v);
        JS_FreeCString(ctx, v);
    }
    return JS_UNDEFINED;
}

static JSValue qjs_el_addEventListener(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = this_node(ctx, this_val);
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type)
    {
        return JS_EXCEPTION;
    }
    int isClick = (strcmp(type, "click") == 0);
    JS_FreeCString(ctx, type);
    if (!isClick)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so pages
         * registering them don't error — only click is delivered. */
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, argv[1]))
    {
        JS_Throw(ctx, JS_NewString(ctx, "listener must be a function"));
        return JS_EXCEPTION;
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        JS_Throw(ctx, JS_NewString(ctx, "too many event listeners"));
        return JS_EXCEPTION;
    }
    int idx = b->listenerCount++;
    st->fns[idx] = JS_DupValue(ctx, argv[1]); /* pin against GC */
    JsListener *L = &b->listeners[idx];
    L->ref = (void *)(intptr_t)idx;
    L->target = n;
    return JS_UNDEFINED;
}
static JSValue qjs_el_getElementsByTagName(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx, this_val);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
    {
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    int k = 0;
    if (n)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = n;
        while (top > 0)
        {
            DomNode *cur = stack[--top];
            for (int i = 0; i < cur->childCount; i++)
            {
                DomNode *c = cur->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, tag) || !strcmp(tag, "*"))
                {
                    JS_SetPropertyUint32(ctx, arr, (uint32_t)k++,
                                         js_qjs_push_element(b, c));
                }
                if (top + 1 < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    JS_FreeCString(ctx, tag);
    return arr;
}

/* Push a fresh wrapper for `node` (NULL → null). Forward-declared above the
 * getters that use it; defined here where the class id is reachable. */
static JSValue js_qjs_push_element(JsBridge *b, DomNode *node)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx)
    {
        return JS_NULL;
    }
    if (!node)
    {
        return JS_NULL;
    }
    JSValue obj = JS_NewObjectProtoClass(st->ctx, st->elProto, st->elClass);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_SetOpaque(obj, node);
    return obj;
}

/* ── console ─────────────────────────────────────────────────────────────── */
static JSValue qjs_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    char line[160];
    size_t off = 0;
    line[0] = '\0';
    for (int i = 0; i < argc && off < sizeof(line) - 2; i++)
    {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s)
        {
            size_t n = strlen(s);
            if (off + n >= sizeof(line) - 2)
            {
                n = sizeof(line) - 2 - off;
            }
            memcpy(line + off, s, n);
            off += n;
            if (i < argc - 1)
            {
                line[off++] = ' ';
            }
            line[off] = '\0';
            JS_FreeCString(ctx, s);
        }
    }
    logger_log("[js] %s", line);
    return JS_UNDEFINED;
}

/* ── document.write capture ──────────────────────────────────────────────── */
static void dw_append(JsBridge *b, const char *s)
{
    if (!s || b->outputDropped)
    {
        return;
    }
    if (b->output.len + strlen(s) + 1 > JSBRIDGE_MAX_OUTPUT)
    {
        b->outputDropped = 1;
        return;
    }
    strbuf_append(&b->output, s);
}

static JSValue qjs_document_write(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s)
        {
            return JS_EXCEPTION;
        }
        dw_append(b, s);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}
static JSValue qjs_document_writeln(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s)
        {
            return JS_EXCEPTION;
        }
        dw_append(b, s);
        JS_FreeCString(ctx, s);
    }
    dw_append(b, "\n");
    return JS_UNDEFINED;
}

/* ── document object ─────────────────────────────────────────────────────── */
static DomNode *first_element(DomNode *n, const char *tag)
{
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *c = n->children[i];
        if (c->kind == DOM_ELEMENT && !strcmp(c->tag, tag))
        {
            return c;
        }
    }
    return NULL;
}

static DomNode *find_element_by_id(JsBridge *b, const char *id)
{
    if (!b->dom || !b->dom->root || !id)
    {
        return NULL;
    }
    DomNode *hit = NULL;
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = b->dom->root;
    while (top > 0 && !hit)
    {
        DomNode *n = stack[--top];
        if (n->kind == DOM_ELEMENT)
        {
            const char *v = dom_get_attr(n, "id");
            if (v && strcmp(v, id) == 0)
            {
                hit = n;
                break;
            }
        }
        if (top + n->childCount > DOM_SEARCH_MAX_DEPTH)
        {
            break;
        }
        for (int i = 0; i < n->childCount; i++)
        {
            stack[top++] = n->children[i];
        }
    }
    if (!hit && id[0] == '_' && id[1] == 'd')
    {
        hit = dom_node_by_id(b->dom, atoi(id + 2));
    }
    return hit;
}

static JSValue qjs_document_getElementById(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id)
    {
        return JS_EXCEPTION;
    }
    DomNode *n = find_element_by_id(b, id);
    JS_FreeCString(ctx, id);
    return js_qjs_push_element(b, n);
}
static JSValue qjs_document_createElement(JSContext *ctx,
                                          JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
    {
        return JS_EXCEPTION;
    }
    DomNode *el = dom_create_element(b->dom, tag);
    JS_FreeCString(ctx, tag);
    if (!el)
    {
        JS_Throw(ctx, JS_NewString(ctx, "createElement failed"));
        return JS_EXCEPTION;
    }
    return js_qjs_push_element(b, el);
}

/* R17: document.createComment — comment nodes materialize as detached
 * text nodes (never rendered; textContent accessors stay well-defined).
 * React uses comment nodes as insertion markers for fragments/suspense. */
static JSValue qjs_document_createComment(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *t = dom_create_text(b->dom, "");
    if (!t)
    {
        JS_Throw(ctx, JS_NewString(ctx, "createComment failed"));
        return JS_EXCEPTION;
    }
    return js_qjs_push_element(b, t);
}
static JSValue qjs_document_createTextNode(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    const char *txt = JS_ToCString(ctx, argv[0]);
    if (!txt)
    {
        return JS_EXCEPTION;
    }
    DomNode *t = dom_create_text(b->dom, txt);
    JS_FreeCString(ctx, txt);
    if (!t)
    {
        JS_Throw(ctx, JS_NewString(ctx, "createTextNode failed"));
        return JS_EXCEPTION;
    }
    return js_qjs_push_element(b, t);
}

/* document.querySelector(All): scoped at the document root (SW5). */
static JSValue qjs_document_querySelector(JSContext *ctx,
                                          JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
    {
        return JS_EXCEPTION;
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *root = b->dom ? b->dom->root : NULL;
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, root, sel, scratch, sizeof(scratch));
    JS_FreeCString(ctx, sel);
    return js_qjs_push_element(b, hit);
}

static JSValue qjs_document_querySelectorAll(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel)
    {
        return JS_EXCEPTION;
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *root = b->dom ? b->dom->root : NULL;
    JSValue arr = JS_NewArray(ctx);
    QjsQsEmit e = {ctx, (QjsState *)b->implState, arr, 0};
    jsbridge_el_query_selector_all(b, root, sel, scratch, sizeof(scratch),
                                   qs_emit_quickjs, &e);
    JS_FreeCString(ctx, sel);
    return arr;
}
/* document.title getter: walk the live tree's <title> so scripts and the
 * chrome agree (doc->title is only filled by the walker, after scripts). */
static JSValue qjs_document_getTitle(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    if (b->dom && b->dom->root)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = b->dom->root;
        while (top > 0)
        {
            DomNode *n = stack[--top];
            for (int i = 0; i < n->childCount; i++)
            {
                DomNode *c = n->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, "title"))
                {
                    char buf[256];
                    doc_concat_node_text(c, buf, sizeof(buf));
                    return JS_NewString(ctx, buf);
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    return JS_NewString(ctx, b->doc->title[0] ? b->doc->title : "");
}
/* document/window.addEventListener: UNSUPPORTED delivery (no bubbling
 * model); accepted silently so common pages don't throw — parity. */
static JSValue qjs_doc_addEventListener(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}
static JSValue qjs_location_assign(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    const char *u = JS_ToCString(ctx, argv[0]);
    logger_log("[js] location.assign/replace ignored: %s", u ? u : "");
    if (u)
    {
        JS_FreeCString(ctx, u);
    }
    return JS_UNDEFINED;
}
static JSValue qjs_alert(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    const char *s = JS_ToCString(ctx, argv[0]);
    logger_log("[js alert] %s", s ? s : "");
    if (s)
    {
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}
static JSValue qjs_noop(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

/* R17: inert DOM constructor globals. Library code does
 * `x instanceof window.HTMLIFrameElement` (React DOM getActiveElementDeep)
 * and `typeof x === 'object'` feature checks; with the constructor missing,
 * `instanceof` THROWS (invalid right operand) and kills the page. Real
 * semantics here: we have no iframes, so a constructor whose prototype
 * never matches our element wrappers is behaviorally exact. Only defines
 * names that are still undefined (never overrides real surfaces like
 * XMLHttpRequest). */
static void qjs_define_dom_ctors(JSContext *ctx, JSValue glob)
{
    static const char *names[] = {
        "HTMLElement",     "HTMLInputElement",    "HTMLTextAreaElement",
        "HTMLSelectElement", "HTMLOptionElement",  "HTMLIFrameElement",
        "HTMLCanvasElement", "HTMLImageElement",   "HTMLAnchorElement",
        "HTMLScriptElement", "HTMLStyleElement",   "HTMLLinkElement",
        "HTMLHtmlElement",   "HTMLBodyElement",    "HTMLDivElement",
        "HTMLSpanElement",   "HTMLParagraphElement",
        "HTMLUListElement",  "HTMLLIElement",      "HTMLButtonElement",
        "HTMLFormElement",   "SVGElement",         "Element",
        "Node",            "Document",           "Window",
        "Event",           "CustomEvent",         "MouseEvent",
        "KeyboardEvent",   "NodeList",           "HTMLCollection",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        JSValue existing = JS_GetPropertyStr(ctx, glob, names[i]);
        int missing = JS_IsUndefined(existing);
        JS_FreeValue(ctx, existing);
        if (!missing)
        {
            continue;
        }
        JSValue fn = JS_NewCFunction(ctx, qjs_noop, names[i], 0);
        JS_SetPropertyStr(ctx, fn, "prototype", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, glob, names[i], fn);
    }
}

/* ── event object (click dispatch) ───────────────────────────────────────── */
static JSValue qjs_event_preventDefault(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    b->preventDef = 1;
    return JS_UNDEFINED;
}

/* Build the element prototype once: accessor properties + methods live here
 * and are shared by every wrapper instance. */
static int qjs_build_element_proto(JSContext *ctx, QjsState *st)
{
    JSClassID cid = 0;
    JSClassDef def = {"Element", qjs_el_finalizer, NULL, NULL, NULL};
    /* JS_NewClassID RETURNS the new class id (JS_INVALID_CLASS_ID on
     * failure) — it is not an error-code API. */
    if (JS_NewClassID(&cid) == JS_INVALID_CLASS_ID ||
        cid == JS_INVALID_CLASS_ID)
    {
        return -1;
    }
    if (JS_NewClass(JS_GetRuntime(ctx), cid, &def) != 0)
    {
        return -1;
    }
    st->elClass = cid;
    st->elProto = JS_NewObjectProto(ctx, JS_NULL);

    /* accessor properties: [name, getter, setter(NULL=read-only)] */
    struct
    {
        const char *name;
        JSCFunction *get;
        JSCFunction *set;
    } accs[] = {
        {"tagName", qjs_el_get_tagName, NULL},
        {"id", qjs_el_get_id, qjs_el_set_id},
        {"textContent", qjs_el_get_textContent, qjs_el_set_textContent},
        {"innerHTML", qjs_el_get_innerHTML, qjs_el_set_innerHTML},
        {"parentNode", qjs_el_get_parentNode, NULL},
        {"childElementCount", qjs_el_get_childElementCount, NULL},
        {"children", qjs_el_get_children, NULL},
        {"nodeType", qjs_el_get_nodeType, NULL},
        {"nodeName", qjs_el_get_nodeName, NULL},
        {"firstElementChild", qjs_el_get_firstElementChild, NULL},
        {"nextElementSibling", qjs_el_get_nextElementSibling, NULL},
        {"classList", qjs_el_get_classList, NULL},
        {"style", qjs_el_get_style, NULL},
        {"ownerDocument", qjs_el_get_ownerDocument, NULL},
        {"ownerDocument", qjs_el_get_ownerDocument, NULL},
        /* R17: anchor URL components (gate to URL elements inside the
         * accessors; other elements return "" — the HTMLAnchorElement-
         * only surface for library feature detection). href is settable:
         * resolved against the base and stored, like a browser. */
        {"href", qjs_el_get_a_href, qjs_el_set_a_href},
        {"protocol", qjs_el_get_a_protocol, NULL},
        {"host", qjs_el_get_a_host, NULL},
        {"hostname", qjs_el_get_a_hostname, NULL},
        {"port", qjs_el_get_a_port, NULL},
        {"pathname", qjs_el_get_a_pathname, NULL},
        {"search", qjs_el_get_a_search, NULL},
        {"hash", qjs_el_get_a_hash, NULL},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        JSAtom atom = JS_NewAtom(ctx, accs[i].name);
        JSValue get = JS_NewCFunction(ctx, accs[i].get, accs[i].name, 0);
        JSValue set = accs[i].set
                          ? JS_NewCFunction(ctx, accs[i].set, accs[i].name, 1)
                          : JS_NULL;
        JS_DefinePropertyGetSet(ctx, st->elProto, atom, get, set,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }

    /* methods */
    struct
    {
        const char *name;
        JSCFunction *fn;
        int nargs;
    } methods[] = {
        {"getAttribute", qjs_el_getAttribute, 1},
        {"setAttribute", qjs_el_setAttribute, 2},
        {"getBoundingClientRect", qjs_el_getBoundingClientRect, 0},
        {"removeAttribute", qjs_el_removeAttribute, 1},
        {"appendChild", qjs_el_appendChild, 1},
        {"removeChild", qjs_el_removeChild, 1},
        {"insertBefore", qjs_el_insertBefore, 2},
        {"querySelector", qjs_el_querySelector, 1},
        {"querySelectorAll", qjs_el_querySelectorAll, 1},
        {"addEventListener", qjs_el_addEventListener, 2},
        {"getElementsByTagName", qjs_el_getElementsByTagName, 1},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        JS_SetPropertyStr(ctx, st->elProto, methods[i].name,
                          JS_NewCFunction(ctx, methods[i].fn, methods[i].name,
                                          methods[i].nargs));
    }
    return 0;
}

/* ── timers (setTimeout / setInterval): router table + engine refs ──────── */
/* Registration entry: argc[0]=fn argc[1]=delay. Returns the public id.
 * The callback is JS_DupValue-pinned in st->tfn[] (GC-safe); the router
 * table owns the pin until clear/close — the SAME ownership the listener
 * fns[] array uses. fnRef = the tfn[] slot index. */
static JSValue qjs_timer_setup(JSContext *ctx, JsBridge *b, QjsState *st,
                               int argc, JSValueConst *argv, JsTimerKind kind)
{
    if (!argc || !JS_IsFunction(ctx, argv[0]))
    {
        return JS_ThrowTypeError(ctx, "timer callback must be a function");
    }
    if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
    {
        return JS_ThrowTypeError(ctx, "too many timers");
    }
    int slot = -1;
    /* Slot 0 is RESERVED: JsTimer.ref == NULL is the router's refusal
     * sentinel, and slot 0 would encode as (void*)0 — so the very first
     * timer would be rejected as "too many timers" (same bug class the XS
     * bridge hit; found live in the simulator suite). Scan from 1. */
    for (int i = 1; i < JSBRIDGE_TIMERS_MAX; i++)
    {
        if (JS_IsUndefined(st->tfn[i]) || JS_IsUninitialized(st->tfn[i]))
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return JS_ThrowTypeError(ctx, "too many timers");
    }
    int delay = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
    {
        JS_ToInt32(ctx, &delay, argv[1]);
    }
    if (delay < 0)
    {
        delay = 0;
    }
    int id = jsbridge_timer_start(b, kind, (void *)(intptr_t)slot,
                                  (unsigned)delay);
    if (id == 0)
    {
        return JS_ThrowTypeError(ctx, "too many timers");
    }
    st->tfn[slot] = JS_DupValue(ctx, argv[0]); /* pin AFTER acceptance */
    return JS_NewInt32(ctx, id);
}

static JSValue qjs_setTimeout(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    return qjs_timer_setup(ctx, b, st, argc, argv, JS_TIMER_TIMEOUT);
}

static JSValue qjs_setInterval(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    return qjs_timer_setup(ctx, b, st, argc, argv, JS_TIMER_INTERVAL);
}

static JSValue qjs_clear_common(JSContext *ctx, int argc, JSValueConst *argv)
{
    JsBridge *b = bridge_of(ctx);
    int id = 0;
    if (argc && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
    {
        JS_ToInt32(ctx, &id, argv[0]);
    }
    return JS_NewInt32(ctx, jsbridge_timer_clear(b, id));
}

static JSValue qjs_clearTimeout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    return qjs_clear_common(ctx, argc, argv);
}

static JSValue qjs_clearInterval(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    return qjs_clear_common(ctx, argc, argv);
}

/* ── R17: window.getComputedStyle ────────────────────────────────────
 * Frameworks (Material-UI transitions, animation libs) read computed
 * styles before animating: getComputedStyle(el).getPropertyValue(name).
 * Backed by the element's inline style attribute — the resolved-style
 * source this bridge maintains — every unknown name returns "" (a real
 * browser returns the cascade default; callers guard on empty). */
static void qjs_gcs_read(const DomNode *n, const char *name, char *out,
                         size_t outLen)
{
    out[0] = '\0';
    if (!n || n->kind != DOM_ELEMENT || !name || !name[0])
    {
        return;
    }
    const char *st = dom_get_attr((DomNode *)n, "style");
    if (!st)
    {
        return;
    }
    size_t nl = strlen(name);
    const char *p = st;
    while (*p)
    {
        while (*p == ' ' || *p == ';')
        {
            p++;
        }
        const char *colon = strchr(p, ':');
        if (!colon)
        {
            break;
        }
        const char *semi = strchr(colon, ';');
        size_t keyLen = (size_t)(colon - p);
        /* trim trailing spaces on the key */
        while (keyLen > 0 && p[keyLen - 1] == ' ')
        {
            keyLen--;
        }
        if (keyLen == nl)
        {
            int eq = 1;
            for (size_t i = 0; i < nl; i++)
            {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z')
                {
                    a = (char)(a + 32);
                }
                if (b >= 'A' && b <= 'Z')
                {
                    b = (char)(b + 32);
                }
                if (a != b)
                {
                    eq = 0;
                    break;
                }
            }
            if (eq)
            {
                const char *val = colon + 1;
                size_t vlen = semi ? (size_t)(semi - val) : strlen(val);
                while (vlen > 0 && val[0] == ' ')
                {
                    val++;
                    vlen--;
                }
                while (vlen > 0 && val[vlen - 1] == ' ')
                {
                    vlen--;
                }
                if (vlen >= outLen)
                {
                    vlen = outLen - 1;
                }
                memcpy(out, val, vlen);
                out[vlen] = '\0';
                return;
            }
        }
        p = semi ? semi + 1 : colon + strlen(colon);
    }
}

static JSValue qjs_gcs_getPropertyValue(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    JSValue elv = JS_GetPropertyStr(ctx, this_val, "_el");
    DomNode *n = qjs_sw5_node_of(ctx, elv, st->elClass);
    JS_FreeValue(ctx, elv);
    const char *name = JS_ToCString(ctx, argv[0]);
    char val[128];
    qjs_gcs_read(n, name ? name : "", val, sizeof(val));
    if (name)
    {
        JS_FreeCString(ctx, name);
    }
    return JS_NewString(ctx, val);
}

static JSValue qjs_window_getComputedStyle(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    DomNode *n = qjs_sw5_node_of(ctx, argv[0], st->elClass);
    JSValue obj = JS_NewObject(ctx);
    if (n)
    {
        JS_SetPropertyStr(ctx, obj, "_el", JS_DupValue(ctx, argv[0]));
    }
    JS_SetPropertyStr(ctx, obj, "getPropertyValue",
                      JS_NewCFunction(ctx, qjs_gcs_getPropertyValue,
                                      "getPropertyValue", 1));
    return obj;
}

/* R17: document.title setter — SPA pages retitle themselves from route
 * effects (React useEffect + document.title = ...). Browsers rewrite the
 * <title> element; do the same when it exists in the DOM. */
static JSValue qjs_document_setTitle(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    const char *v = JS_ToCString(ctx, argv[0]);
    if (!v)
    {
        return JS_EXCEPTION;
    }
    if (b->dom && b->dom->root)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = b->dom->root;
        while (top > 0)
        {
            DomNode *n = stack[--top];
            for (int i = 0; i < n->childCount; i++)
            {
                DomNode *c = n->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, "title"))
                {
                    /* Replace the title element's text child. */
                    DomNode *t = dom_create_text(b->dom, v);
                    if (t)
                    {
                        dom_append_child(b->dom, c, t);
                    }
                    top = 0; /* stop the walk */
                    break;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    snprintf(b->doc->title, sizeof(b->doc->title), "%s", v);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

/* R17: document.activeElement getter — <body> (no focus model on device;
 * real browsers behave identically with nothing focused). */
static JSValue qjs_doc_get_activeElement(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    DomNode *body = first_element(b->dom->root, "body");
    return js_qjs_push_element(b, body ? body : b->dom->root);
}

/* R17: document.defaultView getter — the global object (window). React
 * Router's history shim does `window = document.defaultView` first thing. */
static JSValue qjs_doc_get_defaultView(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    (void)this_val;
    return JS_GetGlobalObject(ctx);
}

/* ── R17: WHATWG URL class ─────────────────────────────────────────────
 * `new URL(url[, base])` — React Router's encodeLocation, fetch polyfills
 * and worker code construct these constantly. Implemented over the shared
 * core/url resolver so browser, jsext and JS all agree on one URL grammar
 * (no site knowledge; category-level platform surface). */
static JSValue qjs_url_ctor(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *u = JS_ToCString(ctx, argv[0]);
    if (!u)
    {
        return JS_EXCEPTION;
    }
    const char *base = NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
    {
        base = JS_ToCString(ctx, argv[1]);
    }
    JsBridge *b = bridge_of(ctx);
    const char *pageBase = (b && b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl
                                                               : "about:blank";
    char *abs = url_resolve((base && *base) ? base : pageBase, u);
    if (base)
    {
        JS_FreeCString(ctx, base);
    }
    JS_FreeCString(ctx, u);
    if (!abs)
    {
        JS_Throw(ctx, JS_NewString(ctx, "URL: failed to resolve"));
        return JS_EXCEPTION;
    }
    UrlParsed p;
    int ok = (url_parse(abs, &p) == 0);
    /* QuickJS C constructors receive no auto-created `this` — build and
     * return the instance ourselves (also correct for plain calls). */
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, abs));
    JS_SetPropertyStr(ctx, obj, "origin",
                      JS_NewString(ctx, ok ? p.normalized : abs));
    JS_SetPropertyStr(ctx, obj, "protocol",
                      JS_NewString(ctx, ok ? (p.isSsl ? "https:" : "http:") : ""));
    JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, ok ? p.host : ""));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, ok ? p.host : ""));
    {
        char pbuf[12];
        snprintf(pbuf, sizeof(pbuf), "%d", (ok && p.port != 80 && p.port != 443) ? p.port : 0);
        JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, pbuf));
    }
    JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, ok ? p.path : "/"));
    {
        char sbuf[264];
        snprintf(sbuf, sizeof(sbuf), "%s%s", ok && p.query[0] ? "?" : "",
                 ok ? p.query : "");
        JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, sbuf));
        snprintf(sbuf, sizeof(sbuf), "%s%s", ok && p.hash[0] ? "#" : "",
                 ok ? p.hash : "");
        JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, sbuf));
    }
    /* searchParams: minimal get/has over the parsed query pairs. */
    JSValue sp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sp, "params", JS_NewString(ctx, ok ? p.query : ""));
    JS_SetPropertyStr(ctx, sp, "get",
                      JS_NewCFunction(ctx, qjs_noop, "get", 1));
    JS_SetPropertyStr(ctx, obj, "searchParams", sp);
    pluto_free(abs);
    return obj;
}

static void qjs_define_url(JSContext *ctx, JSValue glob)
{
    JSValue existing = JS_GetPropertyStr(ctx, glob, "URL");
    int missing = JS_IsUndefined(existing);
    JS_FreeValue(ctx, existing);
    if (!missing)
    {
        return;
    }
    JSValue fn = JS_NewCFunction(ctx, qjs_url_ctor, "URL", 2);
    /* new URL() requires the constructor bit on the C function. */
    JS_SetConstructorBit(ctx, fn, 1);
    JS_SetPropertyStr(ctx, glob, "URL", fn);
}

/* ── R17: window.history shim ─────────────────────────────────────────
 * React Router (and most SPAs) drive navigation through history.pushState/
 * replaceState/state/go. A real SPA back-button model is impossible here,
 * but the FULL API contract is implementable: state is stored on the
 * object itself, push/replace record it, go/back/forward are accepted
 * no-ops (nothing to rewind to — the page never actually navigated). */
static JSValue qjs_history_pushState(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
    {
        JS_SetPropertyStr(ctx, this_val, "state", JS_DupValue(ctx, argv[0]));
    }
    return JS_UNDEFINED;
}
static JSValue qjs_history_go(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED; /* nothing to rewind to */
}
static void qjs_define_history(JSContext *ctx, JSValue glob)
{
    JSValue existing = JS_GetPropertyStr(ctx, glob, "history");
    int missing = JS_IsUndefined(existing);
    JS_FreeValue(ctx, existing);
    if (!missing)
    {
        return;
    }
    JSValue hist = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, hist, "state", JS_NULL);
    JS_SetPropertyStr(ctx, hist, "scrollRestoration", JS_NewString(ctx, "auto"));
    JS_SetPropertyStr(ctx, hist, "length", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, hist, "pushState",
                      JS_NewCFunction(ctx, qjs_history_pushState, "pushState", 3));
    JS_SetPropertyStr(ctx, hist, "replaceState",
                      JS_NewCFunction(ctx, qjs_history_pushState, "replaceState", 3));
    JS_SetPropertyStr(ctx, hist, "go", JS_NewCFunction(ctx, qjs_history_go, "go", 1));
    JS_SetPropertyStr(ctx, hist, "back", JS_NewCFunction(ctx, qjs_history_go, "back", 0));
    JS_SetPropertyStr(ctx, hist, "forward", JS_NewCFunction(ctx, qjs_history_go, "forward", 0));
    JS_SetPropertyStr(ctx, glob, "history", hist);
}

static void define_globals(JSContext *ctx, QjsState *st, JsBridge *b,
                           const char *baseUrl)
{
    JSValue glob = JS_GetGlobalObject(ctx);

    /* window: identity alias for the global object. */
    JS_SetPropertyStr(ctx, glob, "window", JS_DupValue(ctx, glob));

    /* document */
    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "getElementById",
                      JS_NewCFunction(ctx, qjs_document_getElementById,
                                      "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "createElement",
                      JS_NewCFunction(ctx, qjs_document_createElement,
                                      "createElement", 1));
    /* R17: createElementNS + createComment — React DOM's commit phase
     * creates every element through createElementNS and uses comment nodes
     * as insertion markers. Namespace arg is ignored (HTML-only tree);
     * comments materialize as detached text nodes (never rendered). */
    JS_SetPropertyStr(ctx, doc, "createElementNS",
                      JS_NewCFunction(ctx, qjs_document_createElement,
                                      "createElementNS", 2));
    JS_SetPropertyStr(ctx, doc, "createComment",
                      JS_NewCFunction(ctx, qjs_document_createComment,
                                      "createComment", 1));
    JS_SetPropertyStr(ctx, doc, "createTextNode",
                      JS_NewCFunction(ctx, qjs_document_createTextNode,
                                      "createTextNode", 1));
    JS_SetPropertyStr(ctx, doc, "querySelector",
                      JS_NewCFunction(ctx, qjs_document_querySelector,
                                      "querySelector", 1));
    JS_SetPropertyStr(ctx, doc, "querySelectorAll",
                      JS_NewCFunction(ctx, qjs_document_querySelectorAll,
                                      "querySelectorAll", 1));
    JSAtom tat = JS_NewAtom(ctx, "title");
    JS_DefinePropertyGetSet(
        ctx, doc, tat,
        JS_NewCFunction(ctx, qjs_document_getTitle, "get title", 0),
        JS_NewCFunction(ctx, qjs_document_setTitle, "set title", 1),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, tat);
    JS_SetPropertyStr(ctx, doc, "write",
                      JS_NewCFunction(ctx, qjs_document_write, "write", 1));
    JS_SetPropertyStr(ctx, doc, "writeln",
                      JS_NewCFunction(ctx, qjs_document_writeln, "writeln", 1));
    JS_SetPropertyStr(ctx, doc, "addEventListener",
                      JS_NewCFunction(ctx, qjs_doc_addEventListener,
                                      "addEventListener", 2));
    DomNode *body = first_element(b->dom->root, "body");
    JS_SetPropertyStr(ctx, doc, "body", js_qjs_push_element(b, body ? body
                                                                    : b->dom->root));
    /* R17: documentElement + head — standard handles React reads while
     * mounting (event delegation, focus management, style injection). */
    JS_SetPropertyStr(ctx, doc, "documentElement",
                      js_qjs_push_element(b, first_element(b->dom->root, "html")
                                               ? first_element(b->dom->root, "html")
                                               : b->dom->root));
    JS_SetPropertyStr(ctx, doc, "head",
                      js_qjs_push_element(b, first_element(b->dom->root, "head")
                                               ? first_element(b->dom->root, "head")
                                               : b->dom->root));
    /* R17: styleSheets — empty StyleSheetList. CSS-in-JS (emotion/styled-
     * components) walks document.styleSheets for their speedy insertRule
     * path; with an empty list the walk finds nothing and the insert throws
     * INSIDE the library's own try/catch (swallowed, like a browser whose
     * stylesheet quota rejected the rule). Structural rendering proceeds. */
    JS_SetPropertyStr(ctx, doc, "styleSheets", JS_NewArray(ctx));
    /* R17: activeElement — real browsers return <body> when nothing has
     * focus; React's getActiveElement reads it before every text insertion. */
    {
        JSAtom ae = JS_NewAtom(ctx, "activeElement");
        JS_DefinePropertyGetSet(
            ctx, doc, ae,
            JS_NewCFunction(ctx, qjs_doc_get_activeElement, "get activeElement", 0),
            JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, ae);
    }
    {
        JSAtom dv = JS_NewAtom(ctx, "defaultView");
        JS_DefinePropertyGetSet(
            ctx, doc, dv,
            JS_NewCFunction(ctx, qjs_doc_get_defaultView, "get defaultView", 0),
            JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, dv);
    }
    JS_SetPropertyStr(ctx, glob, "document", doc);

    /* navigator */
    JSValue nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "userAgent",
                      JS_NewString(ctx, "PlutoBrowser/1.0 (Playdate; QuickJS "
                                        "2026-06-04)"));
    JS_SetPropertyStr(ctx, nav, "appCodeName", JS_NewString(ctx, "PlutoBrowser"));
    JS_SetPropertyStr(ctx, nav, "appVersion", JS_NewString(ctx, "1.0"));
    JS_SetPropertyStr(ctx, glob, "navigator", nav);

    /* console */
    JSValue con = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, con, "log",
                      JS_NewCFunction(ctx, qjs_console_log, "log", 0));
    JS_SetPropertyStr(ctx, con, "warn",
                      JS_NewCFunction(ctx, qjs_console_log, "warn", 0));
    JS_SetPropertyStr(ctx, con, "error",
                      JS_NewCFunction(ctx, qjs_console_log, "error", 0));
    JS_SetPropertyStr(ctx, glob, "console", con);

    /* location — enriched with the standard URL components; React Router
     * destructures {pathname, search, hash} from window.location and reads
     * location.origin for its history shim. */
    JSValue loc = JS_NewObject(ctx);
    {
        UrlParsed u;
        int haveUrl = (baseUrl && url_parse(baseUrl, &u) == 0);
        JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, baseUrl ? baseUrl : ""));
        JS_SetPropertyStr(ctx, loc, "origin",
                          JS_NewString(ctx, haveUrl ? u.normalized
                                                    : (baseUrl ? baseUrl : "")));
        JS_SetPropertyStr(ctx, loc, "protocol",
                          JS_NewString(ctx, haveUrl ? (u.isSsl ? "https:" : "http:") : ""));
        JS_SetPropertyStr(ctx, loc, "host", JS_NewString(ctx, haveUrl ? u.host : ""));
        JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, haveUrl ? u.host : ""));
        {
            char pbuf[12];
            snprintf(pbuf, sizeof(pbuf), "%d", haveUrl ? u.port : 0);
            JS_SetPropertyStr(ctx, loc, "port", JS_NewString(ctx, pbuf));
        }
        JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, haveUrl ? u.path : "/"));
        {
            char sbuf[264];
            snprintf(sbuf, sizeof(sbuf), "%s%s",
                     haveUrl && u.query[0] ? "?" : "", haveUrl ? u.query : "");
            JS_SetPropertyStr(ctx, loc, "search", JS_NewString(ctx, sbuf));
            snprintf(sbuf, sizeof(sbuf), "%s%s",
                     haveUrl && u.hash[0] ? "#" : "", haveUrl ? u.hash : "");
            JS_SetPropertyStr(ctx, loc, "hash", JS_NewString(ctx, sbuf));
        }
    }
    JS_SetPropertyStr(ctx, loc, "assign",
                      JS_NewCFunction(ctx, qjs_location_assign, "assign", 1));
    JS_SetPropertyStr(ctx, loc, "replace",
                      JS_NewCFunction(ctx, qjs_location_assign, "replace", 1));
    JS_SetPropertyStr(ctx, glob, "location", loc);

    /* R17: getComputedStyle + viewport metrics (Playdate 400x240). */
    JS_SetPropertyStr(ctx, glob, "getComputedStyle",
                      JS_NewCFunction(ctx, qjs_window_getComputedStyle,
                                      "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, glob, "innerWidth", JS_NewInt32(ctx, 400));
    JS_SetPropertyStr(ctx, glob, "innerHeight", JS_NewInt32(ctx, 240));

    /* R17: window-level listener API (accepted, undelivered — element-level
     * click is the delivery path; SPAs register popstate/hashchange here). */
    {
        JSValue ae = JS_GetPropertyStr(ctx, glob, "addEventListener");
        int missingAe = JS_IsUndefined(ae);
        JS_FreeValue(ctx, ae);
        if (missingAe)
        {
            JS_SetPropertyStr(ctx, glob, "addEventListener",
                              JS_NewCFunction(ctx, qjs_noop, "addEventListener", 2));
            JS_SetPropertyStr(ctx, glob, "removeEventListener",
                              JS_NewCFunction(ctx, qjs_noop, "removeEventListener", 2));
        }
    }

    /* R17: inert DOM constructor globals (instanceof right-hand operands). */
    qjs_define_dom_ctors(ctx, glob);

    /* R17: SPA history shim. */
    qjs_define_history(ctx, glob);

    /* R17: WHATWG URL class. */
    qjs_define_url(ctx, glob);

    /* window/global methods */
    JS_SetPropertyStr(ctx, glob, "alert",
                      JS_NewCFunction(ctx, qjs_alert, "alert", 1));
    JS_SetPropertyStr(ctx, glob, "confirm",
                      JS_NewCFunction(ctx, qjs_noop, "confirm", 1));
    JS_SetPropertyStr(ctx, glob, "prompt",
                      JS_NewCFunction(ctx, qjs_noop, "prompt", 1));
    JS_SetPropertyStr(ctx, glob, "setTimeout",
                      JS_NewCFunction(ctx, qjs_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, glob, "setInterval",
                      JS_NewCFunction(ctx, qjs_setInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, glob, "clearTimeout",
                      JS_NewCFunction(ctx, qjs_clearTimeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, glob, "clearInterval",
                      JS_NewCFunction(ctx, qjs_clearInterval, "clearInterval", 1));
    JS_SetPropertyStr(ctx, glob, "requestAnimationFrame",
                      JS_NewCFunction(ctx, qjs_setTimeout,
                                      "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, glob, "addEventListener",
                      JS_NewCFunction(ctx, qjs_doc_addEventListener,
                                      "addEventListener", 2));
    JS_FreeValue(ctx, glob);
}

/* ── vtable entry points ─────────────────────────────────────────────────── */
static void quickjs_define_xhr(JSContext *ctx, QjsState *st);
static JSValue qjs_fetch(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);

static int quickjs_init(JsBridge *b, const char *baseUrl)
{
    QjsState *st = (QjsState *)JMalloc(sizeof(QjsState));
    if (!st)
    {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    b->implState = st;
    /* JS_UNDEFINED is a valid tagged value even when all-zero, but set it
     * explicitly so slot scans (tfn[] and the XHR/fetch pin arrays) never
     * read an uninitialized tag. */
    for (int i = 0; i < JSBRIDGE_TIMERS_MAX; i++)
    {
        st->tfn[i] = JS_UNDEFINED;
    }
    for (int i = 0; i < JSBRIDGE_XHR_MAX; i++)
    {
        st->xfn[i] = JS_UNDEFINED;
        st->xobj[i] = JS_UNDEFINED;
        st->xres[i][0] = JS_UNDEFINED;
        st->xres[i][1] = JS_UNDEFINED;
    }

    JSRuntime *rt = JS_NewRuntime2(&qjs_mf, (void *)b);
    if (!rt)
    {
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    st->rt = rt;
    JS_SetRuntimeOpaque(rt, (void *)b);
    JS_SetMemoryLimit(rt, QJS_MEM_LIMIT);
    JS_SetMaxStackSize(rt, QJS_STACK_LIMIT);
#ifdef TARGET_PLAYDATE
    /* R26g: bounds every eval's EXECUTION (see qjs_interrupt_handler). */
    JS_SetInterruptHandler(rt, qjs_interrupt_handler, NULL);
#endif

    st->ctx = JS_NewContext(rt);
    if (!st->ctx)
    {
        JS_FreeRuntime(rt);
        st->rt = NULL;
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    JS_SetContextOpaque(st->ctx, (void *)b);

    if (qjs_build_element_proto(st->ctx, st) != 0 ||
        qjs_build_sw5_protos(st->ctx, st) != 0)
    {
        JS_FreeContext(st->ctx);
        JS_FreeRuntime(rt);
        st->rt = NULL;
        st->ctx = NULL;
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    define_globals(st->ctx, st, b, baseUrl);
    quickjs_define_xhr(st->ctx, st);
    /* R26g ground truth for crash symbolization + stack geometry: the
     * runtime address of a known ELF symbol pins the pdex.bin load base;
     * the frame address anchors the probe floor. */
    g_qjs_init_fp = __builtin_frame_address(0);
    logger_log("[qjs] init &define_xhr=%p fp=%p",
               (void *)(uintptr_t)&quickjs_define_xhr, g_qjs_init_fp);
    /* fetch() global (QuickJS-only: needs the engine's native Promise).
     * JS_GetGlobalObject returns a STRONG ref — free it like define_globals
     * does (an unbalanced ref trips the teardown leak assert). */
    JSValue glob = JS_GetGlobalObject(st->ctx);
    JS_SetPropertyStr(st->ctx, glob, "fetch",
                      JS_NewCFunction(st->ctx, qjs_fetch, "fetch", 1));
    JS_FreeValue(st->ctx, glob);
    return 0;
}

/* ── R18: bundler-monolith split execution ────────────────────────────────
 * A webpack/rollup production bundle is ONE top-level statement — the R16
 * statement chunker cannot split it, and its whole-file parse/compile
 * working set exceeds what the device can admit. jsbridge_bundler_plan
 * recognizes the module-map STRUCTURE (never a site: no URLs, no names) and
 * we execute the resulting segments sequentially in THIS engine: a fresh
 * `var <m>={}` registry, one `<m>[key]=<value>` statement per module (the
 * registry SURVIVES across engine brackets, so modules require each other
 * exactly as authored), then the runtime bootstrap + app entry as its own
 * IIFE. Measured on the 502KB bryanwandrych.com bundle at device geometry
 * (64KB OS stack, 40KB probe budget, stubbed DOM): all 96 segments + the
 * 253KB tail run, peak heap ~3.94MB, zero stack faults — QuickJS's own
 * CONFIG_STACK_CHECK probe (which covers BOTH parser recursion via
 * next_token and every interpreter call path) converts any too-deep unit
 * into a contained SyntaxError instead of a bus fault.
 *
 * One segment failing does NOT poison the rest: a module that throws at
 * definition time is contained (the bundle's own loader surfaces it), and
 * the tail segment's failure is contained like any other script.
 * Segment spills: each materialized segment is disk-backed so admission
 * logic and future reuse can stream it — mirror of the router's spill
 * discipline, but scoped to this one run_script call. */

static void quickjs_run_script_split(JsBridge *b, QjsState *st,
                                     const char *src, size_t len, int index,
                                     const PlutoBundlerPlan *plan)
{
    int seg = 0;
    int okSegs = 0, failSegs = 0;
    for (seg = 0; seg < plan->nSpans; seg++)
    {
        size_t cap = jsbridge_bundler_segment_cap(plan, seg);
        if (cap == 0 || cap > JSBRIDGE_MAX_SCRIPT_SOURCE)
        {
            failSegs++;
            continue;
        }
        char *buf = (char *)JMalloc(cap + 1);
        if (!buf)
        {
            failSegs++; /* contained: skip this segment */
            continue;
        }
        long nseg = jsbridge_bundler_emit(src, len, plan, seg, buf, cap);
        if (nseg < 0)
        {
            JFree(buf);
            failSegs++;
            continue;
        }
        if (nseg > (long)cap)
        {
            /* Defensive: an emit overrun would corrupt the funnel heap and
             * crash far from the cause. Contain it instead. */
            JFree(buf);
            failSegs++;
            logger_log("[js] bundle %d seg %d/%d emit overflow", index, seg,
                       plan->nSpans - 1);
            continue;
        }
        /* R26g DEVICE-CRITICAL: QuickJS's eval contract is
         * "'input' must be zero terminated i.e. input[input_len] = '\\0'"
         * (quickjs.c:37111). The classic SW4 path copies + terminates; this
         * split path never did — buf[nseg] was UNINITIALIZED funnel heap,
         * which on device holds recycled garbage. The lexer's sentinel
         * reads then parse garbage → wild jump (INVSTATE crash at seg 1,
         * 2026-10-04/05, deterministic; host ASan quarantine happened to
         * hand back zeros, masking it). Terminate at the emitted length. */
        buf[nseg] = '\0';
        /* Per-segment progress: with 97 segments the crash window is too
         * wide to bisect from a single terminal log line — log before and
         * after each eval so a device fault names its exact segment. */
        /* R26g stack-geometry telemetry: the probe floor is set at attach
         * depth; the REAL margin under it is 61.8KB minus the eval-chain
         * depth from attach. Log the chain depth per span so the probe
         * value can be chosen from evidence, not folklore. */
        logger_log("[js] b%d s%d/%d eval %ldB fpdelta=%lu head '%.48s'",
                   index, seg, plan->nSpans - 1, nseg,
                   (unsigned long)((uintptr_t)g_qjs_init_fp -
                                   (uintptr_t)__builtin_frame_address(0)),
                   buf);
#ifdef PLUTO_QJS_BISECT_SKIP_S1
        /* R26g bisect: seg 1 (emotion autoprefixer, 9.7KB) hard-faults in
         * COMPILE on device (3 runs, deterministic; host 64KB-thread repro
         * compiles it containedly). Skip it: the split continues, React
         * mounts without CSS-in-JS, and the page renders visibly. If the
         * fault moves to a later span, the compile-phase theory was wrong. */
        if (seg == 1)
        {
            failSegs++;
            logger_log("[js] b%d s1 SKIPPED (R26g bisect)", index);
            JFree(buf);
            continue;
        }
#endif
        /* R26g discriminator: COMPILE and EXECUTE as separate brackets so a
         * device fault names its half (compile = parser/codegen recursion;
         * execute = interpreter). Same total work, one extra bracket. */
        qjs_eval_budget_arm();
        JSValue comp = JS_Eval(st->ctx, buf, (size_t)nseg, "[bundle]",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        qjs_eval_budget_disarm();
        if (JS_IsException(comp))
        {
            failSegs++;
            logger_log("[js] b%d s%d/%d COMPILE failed", index, seg,
                       plan->nSpans - 1);
            qjs_take_exception_text(b, st->ctx);
            JFree(buf);
            continue;
        }
        logger_log("[js] b%d s%d/%d compiled ok", index, seg,
                   plan->nSpans - 1);
        /* segment-local registry handoff: segment 0 DECLARES the fresh
         * registry; per-module segments ASSIGN into it; the tail reads it.
         * The name rides in engine state so every eval of this bundle —
         * across JS_Eval brackets — resolves the same global. */
        if (seg == 0)
        {
            snprintf(st->splitMapName, sizeof(st->splitMapName), "%s",
                     plan->name);
        }
        /* R26g: arm the interrupt budget + log the span's eval time —
         * a wedged device log must name its segment and its cost. */
        unsigned long qjsT0 = qjs_now_ms();
        qjs_eval_budget_arm();
        JSValue v = JS_EvalFunction(st->ctx, comp);
        qjs_eval_budget_disarm();
        unsigned long qjsDt = qjs_now_ms() - qjsT0;
        if (JS_IsException(v))
        {
            failSegs++;
            logger_log("[js] bundle %d seg %d/%d failed (%lums)", index, seg,
                       plan->nSpans - 1, qjsDt);
            qjs_take_exception_text(b, st->ctx);
        }
        else
        {
            okSegs++;
            logger_log("[js] b%d s%d/%d ok in %lums live=%luKB", index, seg,
                       plan->nSpans - 1, qjsDt, pluto_mem_live() / 1024);
            JS_FreeValue(st->ctx, v);
        }
        JFree(buf);
        JS_RunGC(st->rt);
    }
    st->splitMapName[0] = '\0';
    logger_log("[js] bundle %d split run complete: %d ok, %d failed segs",
               index, okSegs, failSegs);
    if (failSegs > 0 && !b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError),
                 "bundle %d: %d segment failures", index, failSegs);
    }
}

static void quickjs_run_script(JsBridge *b, const char *src, size_t len,
                               int index)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx)
    {
        return;
    }
    if (len == 0 || len > JSBRIDGE_MAX_SCRIPT_SOURCE)
    {
        if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
        {
            b->errs++;
            if (!b->lastError[0])
            {
                snprintf(b->lastError, sizeof(b->lastError),
                         "script %d too large", index);
            }
        }
        return;
    }
    /* R18 first: if this is a bundler monolith, execute it as plan
     * segments. The gate below never sees the monolith (the C recursion
     * scanner is itself unprobed recursion — the SW4 crash mechanism — and
     * QuickJS's own probe makes it unnecessary for this engine). */
    if (len > PLUTO_BUNDLER_MIN_SOURCE)
    {
        /* Diagnostic: planner NO_MAP on valid-looking bytes is how a bad
         * inline-extraction pointer shows up — log the head so a wrong
         * buffer/offset is visible in one device run. */
        {
            char head[25];
            size_t n = len < 24 ? len : 24;
            memcpy(head, src, n);
            head[n] = '\0';
            logger_log("[qjs] src head '%s'", head);
        }
        /* Span table lives on the funnel heap (2048 x 8B = 16KB — far too
         * big for the 61.8KB task stack; the SW4 crash taught this). */
        PlutoBundlerSpan *spans =
            (PlutoBundlerSpan *)JMalloc(sizeof(PlutoBundlerSpan) *
                                        PLUTO_BUNDLER_MAX_SPANS);
        logger_log("[qjs] rs %d len=%zu spans=%s", index, len,
                   spans ? "ok" : "REFUSED");
        if (spans)
        {
            PlutoBundlerPlan plan;
            plan.spans = spans;
            plan.maxSpans = PLUTO_BUNDLER_MAX_SPANS;
            int prc = jsbridge_bundler_plan(src, len, &plan);
            logger_log("[bundler] plan rc=%d", (int)prc);
            if (prc == PLUTO_BUNDLER_OK)
            {
                logger_log("[js] script %d: bundler monolith (%zuB, %d "
                           "modules) — split execution", index, len,
                           plan.entries);
                b->ran++;
                quickjs_run_script_split(b, st, src, len, index, &plan);
                JFree(spans);
                return;
            }
            JFree(spans);
        }
    }
    /* Compile-safety gate, engine- and target-aware. DEVICE: QuickJS's own
     * CONFIG_STACK_CHECK probe covers parser recursion (next_token probes
     * per token) AND every interpreter call path — a too-deep script throws
     * containedly at the 40KB probe budget instead of overrunning the
     * 61.8KB gameTask stack, so the C recursion scanner (itself UNprobed
     * recursion at ~2.5KB/level — the actual SW4 depth-24 crash mechanism)
     * is unnecessary here and is SKIPPED on device. R17's comment claimed
     * the parser was unprobed; the vendored engine proves otherwise
     * (quickjs.c js_check_stack_overflow call sites). Non-bundler huge
     * scripts still face the SW5 admission gate below. HOST: keep the wide
     * scanner — sim/host builds have no task-stack cliff and the scanner
     * doubles as a parse-failure pre-filter for the suites. */
#ifndef TARGET_PLAYDATE
    if (!jsbridge_script_compile_safe(src, len))
    {
        b->errs++;
        if (!b->lastError[0])
        {
            snprintf(b->lastError, sizeof(b->lastError),
                     "script %d too deeply nested", index);
        }
        logger_log("[js] script %d skipped (nesting guard)", index);
        return;
    }
#endif

    b->ran++;

    /* SW5: ES5 builtin compat prefix (Set/Map/Image) — self-guarding. The
     * prefix rides INSIDE the compiled unit, so the SW4 cache key hashes
     * the page source + prefix together (one salt covers both); a prefix
     * change between builds recompiles rather than mis-executing. */
    const char *prefix = NULL;
    size_t plen = jsbridge_sw5_prefix(&prefix);

    /* ── SW4: bytecode cache (stock JS_WriteObject/JS_ReadObject) ─────────
     * Parsing a 500KB bundle costs seconds on the device; the compiled
     * bytecode is cached to the persistent spill store keyed by a 32-bit
     * hash of the SOURCE bytes, so the SAME source skips the parse on the
     * next visit. Store family survives page navigation (session spill
     * reset does not touch it). Every failure path falls back to the
     * classic JS_Eval parse — the cache can only SPEED UP, never break. */
    unsigned long bcKey = jsbridge_source_key(prefix, plen, 0);
    bcKey = jsbridge_source_key(src, len, bcKey ^ JSBRIDGE_BC_KEY_SALT);
    size_t bcLen = 0;
    uint8_t *bc = NULL;
    if (bcKey && pluto_spill_store_find(bcKey))
    {
        SpillFile sh = pluto_spill_store_open_read(bcKey);
        if (sh != PLUTO_SPILL_INVALID)
        {
            long fsz = pluto_spill_size(sh);
            if (fsz > 0 && fsz <= JSBRIDGE_BC_MAX_BYTES)
            {
                bc = (uint8_t *)JMalloc((size_t)fsz);
                if (bc)
                {
                    long got = pluto_spill_read(sh, 0, bc, (size_t)fsz);
                    if (got != fsz)
                    {
                        JFree(bc);
                        bc = NULL;
                    }
                    else
                    {
                        bcLen = (size_t)got;
                    }
                }
            }
        }
    }
    if (bc)
    {
        JSValue fun = JS_ReadObject(st->ctx, bc, bcLen, JS_READ_OBJ_BYTECODE);
        JFree(bc);
        if (JS_IsException(fun))
        {
            logger_log("[js] bc %08lx unreadable — reparsing", bcKey);
            JS_FreeValue(st->ctx, fun);
        }
        else
        {
            qjs_eval_budget_arm();
            JSValue result = JS_EvalFunction(st->ctx, fun);
            qjs_eval_budget_disarm();
            if (JS_IsException(result))
            {
                b->errs++;
                logger_log("[js] script %d failed (bc hit)", index);
                qjs_take_exception_text(b, st->ctx);
            }
            else
            {
                JS_FreeValue(st->ctx, result);
                logger_log("[js] bc hit %08lx (script %d, %zu bytes)",
                           bcKey, index, bcLen);
            }
            JS_RunGC(st->rt);
            return;
        }
    }

    /* Compile-only pass: the function object doubles as (a) the thing we
     * serialize to the store and (b) the thing we evaluate NOW via
     * JS_EvalFunction — one compile serves both paths. The source is
     * copied + NUL-terminated first (QuickJS's lexer reads the sentinel
     * byte at input[len] — same reason the classic path copied). */
    char *buf = (char *)JMalloc(plen + len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, src, len);
    buf[plen + len] = '\0';
    JSValue compiled = JS_Eval(st->ctx, buf, plen + len, "[page]",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JFree(buf);
    if (JS_IsException(compiled))
    {
        b->errs++;
        logger_log("[js] script %d failed", index);
        qjs_take_exception_text(b, st->ctx);
        JS_RunGC(st->rt);
        return;
    }
    if (bcKey && len >= JSBRIDGE_BC_MIN_SOURCE)
    {
        size_t wlen = 0;
        logger_log("[js] bc %08lx serializing (script %d, %zu src)", bcKey,
                   index, len);
        uint8_t *w = JS_WriteObject(st->ctx, &wlen, compiled,
                                    JS_WRITE_OBJ_BYTECODE);
        if (!w)
        {
            logger_log("[js] bc %08lx serialize failed (script %d)", bcKey,
                       index);
        }
        else if (wlen > 0 && wlen <= JSBRIDGE_BC_MAX_BYTES)
        {
            SpillFile sh = pluto_spill_store_open_create(bcKey);
            if (sh == PLUTO_SPILL_INVALID)
            {
                logger_log("[js] bc %08lx store slot unavailable", bcKey);
            }
            else if (pluto_spill_write(sh, w, wlen) == 0 &&
                     pluto_spill_finish(sh) >= 0)
            {
                logger_log("[js] bc store %08lx (script %d: %zu src -> "
                           "%zu bc)", bcKey, index, len, wlen);
            }
            else
            {
                pluto_spill_discard(sh);
                logger_log("[js] bc store write failed %08lx", bcKey);
            }
        }
        else
        {
            logger_log("[js] bc %08lx over store cap (%zu) — not cached",
                       bcKey, wlen);
        }
        if (w)
        {
            /* JS_WriteObject's buffer comes from QuickJS's internal rt
             * allocator (arenas/small-blocks on top of the funnel, compiled
             * in the shim TU where js_free exports as pluto_qjs_free) —
             * only the engine's own free can release it safely. */
            pluto_qjs_free(st->ctx, w);
        }
    }
    logger_log("[js] qjs: eval begin (script %d)", index);
    qjs_eval_budget_arm();
    JSValue result = JS_EvalFunction(st->ctx, compiled);
    qjs_eval_budget_disarm();
    logger_log("[js] qjs: eval returned (script %d)", index);
    if (JS_IsException(result))
    {
        b->errs++;
        logger_log("[js] script %d failed", index);
        qjs_take_exception_text(b, st->ctx);
    }
    else
    {
        JS_FreeValue(st->ctx, result);
    }
    logger_log("[js] qjs: GC begin (script %d)", index);
    JS_RunGC(st->rt); /* per-script sweep, mirrors the other bridges */
    logger_log("[js] qjs: GC done (script %d)", index);
}

static int quickjs_dispatch_click(JsBridge *b, const void *anchorNode)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx)
    {
        return JSB_CLICK_NONE;
    }
    int fired = 0;
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        if (L->target != anchorNode)
        {
            continue;
        }
        fired = 1;
        b->preventDef = 0;
        b->inClick = 1;

        JSValue fn = JS_DupValue(st->ctx, st->fns[i]);
        JSValue self = js_qjs_push_element(b, (DomNode *)L->target);
        /* event: { type:"click", preventDefault() } */
        JSValue ev = JS_NewObject(st->ctx);
        JS_SetPropertyStr(st->ctx, ev, "type", JS_NewString(st->ctx, "click"));
        JS_SetPropertyStr(st->ctx, ev, "preventDefault",
                          JS_NewCFunction(st->ctx, qjs_event_preventDefault,
                                          "preventDefault", 0));
        JSValueConst argv[1] = {ev};
        JSValue result = JS_Call(st->ctx, fn, self, 1, argv);
        if (JS_IsException(result))
        {
            b->errs++;
            qjs_take_exception_text(b, st->ctx);
        }
        else
        {
            JS_FreeValue(st->ctx, result);
        }
        JS_FreeValue(st->ctx, ev);
        JS_FreeValue(st->ctx, self);
        JS_FreeValue(st->ctx, fn);
        b->inClick = 0;
        b->doc->jsErrors = b->errs;
        snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                 b->lastError);
        if (b->preventDef)
        {
            return JSB_CLICK_SUPPRESSED;
        }
    }
    return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
}

/* Timer vtable: release one pinned callback (fnRef = tfn[] slot index). */
static void quickjs_clear_timer_ref(JsBridge *b, void *fnRef)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx || !fnRef)
    {
        return;
    }
    int slot = (int)(intptr_t)fnRef;
    if (slot >= 0 && slot < JSBRIDGE_TIMERS_MAX)
    {
        JS_FreeValue(st->ctx, st->tfn[slot]);
        st->tfn[slot] = JS_UNDEFINED;
    }
}

/* Invoke one pinned callback. Returns 0 ok, 1 contained error, -1 abort. */
static int quickjs_run_timer_ref(JsBridge *b, void *fnRef)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx || !fnRef)
    {
        return 1;
    }
    int slot = (int)(intptr_t)fnRef;
    if (slot < 0 || slot >= JSBRIDGE_TIMERS_MAX ||
        JS_IsUndefined(st->tfn[slot]))
    {
        return 1;
    }
    JSValue result = JS_Call(st->ctx, st->tfn[slot], JS_UNDEFINED, 0, NULL);
    if (JS_IsException(result))
    {
        int firstErr = !b->lastError[0];
        qjs_take_exception_text(b, st->ctx);
        return firstErr ? -1 : 1;
    }
    JS_FreeValue(st->ctx, result);
    return 0;
}

static void quickjs_close(JsBridge *b)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st)
    {
        return;
    }
    if (st->ctx)
    {
        for (int i = 0; i < b->listenerCount && i < JSBRIDGE_LISTENERS_MAX;
             i++)
        {
            JS_FreeValue(st->ctx, st->fns[i]);
            st->fns[i] = JS_UNDEFINED;
        }
        for (int i = 0; i < JSBRIDGE_TIMERS_MAX; i++)
        {
            if (!JS_IsUndefined(st->tfn[i]) && !JS_IsUninitialized(st->tfn[i]))
            {
                JS_FreeValue(st->ctx, st->tfn[i]);
                st->tfn[i] = JS_UNDEFINED;
            }
        }
        JS_FreeValue(st->ctx, st->elProto);
        st->elProto = JS_UNDEFINED;
        for (int i = 0; i < JSBRIDGE_XHR_MAX; i++)
        {
            JS_FreeValue(st->ctx, st->xfn[i]);
            st->xfn[i] = JS_UNDEFINED;
            JS_FreeValue(st->ctx, st->xobj[i]);
            st->xobj[i] = JS_UNDEFINED;
        }
        JS_FreeValue(st->ctx, st->xhrProto);
        st->xhrProto = JS_UNDEFINED;
        JS_FreeValue(st->ctx, st->clProto);
        st->clProto = JS_UNDEFINED;
        JS_FreeValue(st->ctx, st->stProto);
        st->stProto = JS_UNDEFINED;
        JS_FreeContext(st->ctx);
        st->ctx = NULL;
    }
    if (st->rt)
    {
        /* Runtime free releases wrappers, bytecode and every pinned value. */
        JS_FreeRuntime(st->rt);
        st->rt = NULL;
    }
    JFree(st);
    b->implState = NULL;
}

/* ── XMLHttpRequest + fetch (async HTTP → JS callbacks) ────────────────────
 * Same thin-glue contract as the other bridges. The wrapper is an instance
 * of a dedicated "XMLHttpRequest" class (opaque = public request id cast to
 * a pointer); live state reads (readyState/status/responseText/responseURL)
 * are prototype accessors over the router table. onload/onerror/
 * onreadystatechange are pinned at send into xfn[] (JS_DupValue; fnRef =
 * slot+1, slot 0 RESERVED — same rule as the timer tfn[]). The wrapper is
 * pinned into xobj[] (objRef = slot+1) so `this` binds at completion.
 *
 * fetch() uses the ENGINE'S NATIVE Promise + job queue: the call creates
 * the capability and pins its resolving functions in xres[]; the router's
 * completion resolves the promise; the awaiting script's continuation runs
 * through JS_ExecutePendingJob inside the pump's engine bracket.
 * Caps + validation + byte budgets live in the router (jsbridge.c). */

static int qjs_xhr_id_of(JSContext *ctx, JSValueConst this_val)
{
    void *p = JS_GetOpaque(this_val,
                           ((QjsState *)bridge_of(ctx)->implState)->xhrClass);
    return (int)(intptr_t)p;
}

static const JsHttpRequest *qjs_xhr_state(JSContext *ctx,
                                          JSValueConst this_val)
{
    JsBridge *b = bridge_of(ctx);
    return jsbridge_xhr_get(b, qjs_xhr_id_of(ctx, this_val));
}

static JSValue qjs_xhr_open(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JsBridge *b = bridge_of(ctx);
    if (argc < 2)
    {
        return JS_ThrowTypeError(ctx, "xhr: open(method, url)");
    }
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url)
    {
        if (method)
            JS_FreeCString(ctx, method);
        if (url)
            JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "xhr: open needs strings");
    }
    int id = jsbridge_xhr_open(b, method, url);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    if (id == 0)
    {
        return JS_ThrowTypeError(ctx, "%s",
                                 b->lastError[0] ? b->lastError
                                                 : "xhr open failed");
    }
    JS_SetOpaque(this_val, (void *)(intptr_t)id);
    return JS_UNDEFINED;
}

/* Find a free pin slot (index ≥ 1 — slot 0 reserved, router's NULL rule). */
static int qjs_xhr_slot(JSValue *arr, int n)
{
    for (int i = 1; i < n; i++)
    {
        if (JS_IsUndefined(arr[i]) || JS_IsUninitialized(arr[i]))
        {
            return i;
        }
    }
    return -1;
}

static JSValue qjs_xhr_send(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    int id = qjs_xhr_id_of(ctx, this_val);
    if (id <= 0)
    {
        return JS_ThrowTypeError(ctx, "xhr: send before open");
    }
    int fslot = qjs_xhr_slot(st->xfn, JSBRIDGE_XHR_MAX);
    int oslot = qjs_xhr_slot(st->xobj, JSBRIDGE_XHR_MAX);
    if (fslot < 0 || oslot < 0)
    {
        return JS_ThrowTypeError(ctx, "xhr: pin slots full");
    }
    /* Pick the completion handler (onload → onerror → onreadystatechange
     * precedence, read live off the wrapper). */
    static const char *const names[] = {"onload", "onerror",
                                        "onreadystatechange"};
    JSValue handler = JS_UNDEFINED;
    for (int i = 0; i < 3 && JS_IsUndefined(handler); i++)
    {
        JSValue v = JS_GetPropertyStr(ctx, this_val, names[i]);
        if (JS_IsFunction(ctx, v))
        {
            handler = v;
        }
        else
        {
            JS_FreeValue(ctx, v);
        }
    }
    if (JS_IsUndefined(handler))
    {
        return JS_ThrowTypeError(
            ctx, "xhr: no onload/onerror/onreadystatechange handler");
    }
    st->xfn[fslot] = JS_DupValue(ctx, handler);
    JS_FreeValue(ctx, handler);
    st->xobj[oslot] = JS_DupValue(ctx, this_val);
    jsbridge_xhr_send(b, id, (void *)(intptr_t)(fslot + 1),
                      (void *)(intptr_t)(oslot + 1));
    return JS_UNDEFINED;
}

static JSValue qjs_xhr_abort(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    JsBridge *b = bridge_of(ctx);
    jsbridge_xhr_abort(b, qjs_xhr_id_of(ctx, this_val));
    return JS_UNDEFINED;
}

static JSValue qjs_xhr_get_readyState(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    const JsHttpRequest *r = qjs_xhr_state(ctx, this_val);
    return JS_NewInt32(ctx, r ? r->state : 0);
}

static JSValue qjs_xhr_get_status(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    const JsHttpRequest *r = qjs_xhr_state(ctx, this_val);
    return JS_NewInt32(ctx, (r && r->state >= JS_XHR_DONE) ? r->status : 0);
}

static JSValue qjs_xhr_get_responseText(JSContext *ctx,
                                        JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    const JsHttpRequest *r = qjs_xhr_state(ctx, this_val);
    return JS_NewString(ctx, (r && r->body) ? r->body : "");
}

static JSValue qjs_xhr_get_responseURL(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    const JsHttpRequest *r = qjs_xhr_state(ctx, this_val);
    return JS_NewString(ctx, (r && r->url[0]) ? r->url : "");
}

/* Constructor: new XMLHttpRequest() → fresh wrapper (opaque id 0). */
static JSValue qjs_xhr_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    QjsState *st = (QjsState *)bridge_of(ctx)->implState;
    JSValue obj = JS_NewObjectProtoClass(ctx, st->xhrProto, st->xhrClass);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_SetOpaque(obj, (void *)(intptr_t)0);
    return obj;
}

static void qjs_xhr_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val; /* opaque holds only the id — nothing to free */
}

/* fetch(url) → Promise<string>. Native-promise implementation: the call
 * creates the capability, pins its resolving functions in xres[] (fnRef =
 * slot+1, objRef NULL = promise mode) and RETURNS the promise to the
 * script; the router's completion resolves/rejects it, and the awaiting
 * continuation drains via JS_ExecutePendingJob inside the pump bracket. */
static JSValue qjs_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    JsBridge *b = bridge_of(ctx);
    QjsState *st = (QjsState *)b->implState;
    if (argc < 1)
    {
        return JS_ThrowTypeError(ctx, "fetch(url) needs a url");
    }
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url)
    {
        return JS_EXCEPTION;
    }
    int id = jsbridge_xhr_open(b, "GET", url);
    JS_FreeCString(ctx, url);
    if (id == 0)
    {
        return JS_ThrowTypeError(ctx, "%s",
                                 b->lastError[0] ? b->lastError
                                                 : "fetch open failed");
    }
    int slot = -1;
    for (int i = 1; i < JSBRIDGE_XHR_MAX; i++)
    {
        if (JS_IsUndefined(st->xres[i][0]) ||
            JS_IsUninitialized(st->xres[i][0]))
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return JS_ThrowTypeError(ctx, "fetch: too many in-flight requests");
    }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
    {
        return promise;
    }
    st->xres[slot][0] = funcs[0]; /* resolve */
    st->xres[slot][1] = funcs[1]; /* reject  */
    jsbridge_xhr_send(b, id, (void *)(intptr_t)(slot + 1), NULL);
    return promise; /* the script owns it from here */
}

static void quickjs_define_xhr(JSContext *ctx, QjsState *st)
{
    JSClassID cid = 0;
    JSClassDef def = {"XMLHttpRequest", qjs_xhr_finalizer, NULL, NULL, NULL};
    if (JS_NewClassID(&cid) == JS_INVALID_CLASS_ID ||
        cid == JS_INVALID_CLASS_ID)
    {
        return;
    }
    if (JS_NewClass(JS_GetRuntime(ctx), cid, &def) != 0)
    {
        return;
    }
    st->xhrClass = cid;
    st->xhrProto = JS_NewObjectProto(ctx, JS_NULL);
    JS_SetPropertyStr(ctx, st->xhrProto, "open",
                      JS_NewCFunction(ctx, qjs_xhr_open, "open", 2));
    JS_SetPropertyStr(ctx, st->xhrProto, "send",
                      JS_NewCFunction(ctx, qjs_xhr_send, "send", 0));
    JS_SetPropertyStr(ctx, st->xhrProto, "abort",
                      JS_NewCFunction(ctx, qjs_xhr_abort, "abort", 0));
    struct
    {
        const char *name;
        JSCFunction *get;
    } accs[] = {
        {"readyState", qjs_xhr_get_readyState},
        {"status", qjs_xhr_get_status},
        {"responseText", qjs_xhr_get_responseText},
        {"response", qjs_xhr_get_responseText},
        {"responseURL", qjs_xhr_get_responseURL},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        JSAtom atom = JS_NewAtom(ctx, accs[i].name);
        JSValue get = JS_NewCFunction(ctx, accs[i].get, accs[i].name, 0);
        JS_DefinePropertyGetSet(ctx, st->xhrProto, atom, get, JS_NULL,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }
    JSValue ctor = JS_NewCFunction2(ctx, qjs_xhr_ctor, "XMLHttpRequest", 0,
                                    JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, st->xhrProto, "constructor", JS_DupValue(ctx, ctor));
    JSValue glob = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, glob, "XMLHttpRequest", ctor);
    JS_FreeValue(ctx, glob);
}

/* XHR vtable: run the pinned completion. TWO modes:
 *   objRef != NULL → call fn(responseText) with `this` = wrapper;
 *   objRef == NULL → fetch mode: resolve/reject the pinned promise with
 *   the response text / an Error, free the resolving funcs, then drain the
 *   pending-job queue (the awaiting continuation) under the same bracket. */
static int quickjs_run_xhr_ref(JsBridge *b, void *fnRef, void *objRef,
                               const JsHttpRequest *r)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx || !fnRef)
    {
        return 1;
    }
    int fslot = (int)(intptr_t)fnRef - 1;
    if (fslot < 0 || fslot >= JSBRIDGE_XHR_MAX)
    {
        return 1;
    }
    const char *body = (r && r->body) ? r->body : "";
    if (!objRef)
    {
        /* fetch promise mode. */
        if (JS_IsUndefined(st->xres[fslot][0]))
        {
            return 1;
        }
        JSValue ret;
        if (r && r->ok)
        {
            JSValue arg = JS_NewString(st->ctx, body);
            ret = JS_Call(st->ctx, st->xres[fslot][0], JS_UNDEFINED, 1, &arg);
            JS_FreeValue(st->ctx, arg);
        }
        else
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s",
                     (r && r->err[0]) ? r->err : "fetch failed");
            JSValue err = JS_NewError(st->ctx);
            JS_SetPropertyStr(st->ctx, err, "message", JS_NewString(st->ctx, msg));
            ret = JS_Call(st->ctx, st->xres[fslot][1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(st->ctx, err);
        }
        JS_FreeValue(st->ctx, st->xres[fslot][0]);
        JS_FreeValue(st->ctx, st->xres[fslot][1]);
        st->xres[fslot][0] = JS_UNDEFINED;
        st->xres[fslot][1] = JS_UNDEFINED;
        if (JS_IsException(ret))
        {
            /* Rejecting with a handler that throws: contained like any JS
             * error (unhandled-rejection noise is bounded by the caps). */
            qjs_take_exception_text(b, st->ctx);
        }
        else
        {
            JS_FreeValue(st->ctx, ret);
        }
        /* Drain the awaiting continuation(s) — bounded. */
        for (int i = 0; i < 16; i++)
        {
            JSContext *jctx = NULL;
            if (JS_ExecutePendingJob(JS_GetRuntime(st->ctx), &jctx) <= 0)
            {
                break;
            }
        }
        return 0;
    }
    int oslot = (int)(intptr_t)objRef - 1;
    if (JS_IsUndefined(st->xfn[fslot]))
    {
        return 1;
    }
    JSValue args[1];
    args[0] = JS_NewString(st->ctx, body);
    JSValue thisObj = (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX &&
                       !JS_IsUndefined(st->xobj[oslot]))
                          ? st->xobj[oslot]
                          : JS_UNDEFINED;
    JSValue result = JS_Call(st->ctx, st->xfn[fslot], thisObj, 1, args);
    JS_FreeValue(st->ctx, args[0]);
    if (JS_IsException(result))
    {
        int firstErr = !b->lastError[0];
        qjs_take_exception_text(b, st->ctx);
        return firstErr ? -1 : 1;
    }
    JS_FreeValue(st->ctx, result);
    return 0;
}

/* XHR vtable: release the pinned completion + wrapper (slot inert); a
 * NULL objRef means fetch promise mode — free any unresolved funcs. */
static void quickjs_clear_xhr_refs(JsBridge *b, void *fnRef, void *objRef)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx)
    {
        return;
    }
    int fslot = (int)(intptr_t)fnRef - 1;
    int oslot = (int)(intptr_t)objRef - 1;
    if (fslot >= 0 && fslot < JSBRIDGE_XHR_MAX)
    {
        if (!JS_IsUndefined(st->xfn[fslot]))
        {
            JS_FreeValue(st->ctx, st->xfn[fslot]);
            st->xfn[fslot] = JS_UNDEFINED;
        }
        if (!JS_IsUndefined(st->xres[fslot][0]))
        {
            JS_FreeValue(st->ctx, st->xres[fslot][0]);
            st->xres[fslot][0] = JS_UNDEFINED;
            JS_FreeValue(st->ctx, st->xres[fslot][1]);
            st->xres[fslot][1] = JS_UNDEFINED;
        }
    }
    if (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX &&
        !JS_IsUndefined(st->xobj[oslot]))
    {
        JS_FreeValue(st->ctx, st->xobj[oslot]);
        st->xobj[oslot] = JS_UNDEFINED;
    }
}

const JsEngineImpl js_engine_quickjs = {
    quickjs_init,            quickjs_run_script,  NULL, /* run_script_stream (R15: XS NR only) */
    quickjs_dispatch_click,
    quickjs_clear_timer_ref, quickjs_run_timer_ref,
    quickjs_run_xhr_ref,     quickjs_clear_xhr_refs, quickjs_close,
    NULL, /* pump (R20: time-sliced split is XS NR only) */
    NULL, /* eval_pending (R27: no background eval) */
    NULL, /* mount_parked (R28: never parks) */
    "QuickJS"};
