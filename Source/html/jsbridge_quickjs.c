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
#include "quickjs.h"
#include "../core/logger.h"
#include "../util/strbuf.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_pd()->system->realloc(NULL, (n))
#define JFree(p) pluto_pd()->system->realloc((p), 0)

/* Engine-wide budget: total engine heap (device heap is ~3MB for the whole
 * browser) and parser/interpreter stack budget. QuickJS's probe guarantees
 * JS never consumes more than stack_size bytes of C stack below the attach
 * point (stack_limit = stack_top - stack_size), so on DEVICE the budget is
 * bounded by the 61.8KB gameTask stack (attach baseline ≈ 2.5KB, ~19KB
 * margin); host/simulator builds have an 8MB stack and -O0 frames are much
 * fatter, so they get a larger budget for the same suite. Override via
 * -DQJS_STACK_LIMIT= for experiments — never ship device above ~48KB. */
#define QJS_MEM_LIMIT (1024u * 1024u)
#ifdef TARGET_PLAYDATE
#define QJS_STACK_LIMIT_DFL (40u * 1024u)
#else
#define QJS_STACK_LIMIT_DFL (64u * 1024u)
#endif
#ifndef QJS_STACK_LIMIT
#define QJS_STACK_LIMIT QJS_STACK_LIMIT_DFL
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
    return pluto_pd()->system->realloc(ptr, (unsigned)size);
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

static JsBridge *bridge_of(JSContext *ctx)
{
    /* The JsBridge* rides in the runtime opaque (set at JS_NewRuntime2). */
    return (JsBridge *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
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
/* PARTIAL: treated as textContent (no markup parsing here) — parity. */
static JSValue qjs_el_set_innerHTML(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return qjs_el_set_textContent(ctx, this_val, argc, argv);
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
        {"removeAttribute", qjs_el_removeAttribute, 1},
        {"appendChild", qjs_el_appendChild, 1},
        {"removeChild", qjs_el_removeChild, 1},
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
    JS_SetPropertyStr(ctx, doc, "createTextNode",
                      JS_NewCFunction(ctx, qjs_document_createTextNode,
                                      "createTextNode", 1));
    JSAtom tat = JS_NewAtom(ctx, "title");
    JS_DefinePropertyGetSet(
        ctx, doc, tat,
        JS_NewCFunction(ctx, qjs_document_getTitle, "get title", 0), JS_NULL,
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

    /* location */
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, baseUrl ? baseUrl : ""));
    JS_SetPropertyStr(ctx, loc, "host", JS_NewString(ctx, baseUrl ? baseUrl : ""));
    JS_SetPropertyStr(ctx, loc, "assign",
                      JS_NewCFunction(ctx, qjs_location_assign, "assign", 1));
    JS_SetPropertyStr(ctx, loc, "replace",
                      JS_NewCFunction(ctx, qjs_location_assign, "replace", 1));
    JS_SetPropertyStr(ctx, glob, "location", loc);

    /* window/global methods */
    JS_SetPropertyStr(ctx, glob, "alert",
                      JS_NewCFunction(ctx, qjs_alert, "alert", 1));
    JS_SetPropertyStr(ctx, glob, "confirm",
                      JS_NewCFunction(ctx, qjs_noop, "confirm", 1));
    JS_SetPropertyStr(ctx, glob, "prompt",
                      JS_NewCFunction(ctx, qjs_noop, "prompt", 1));
    JS_SetPropertyStr(ctx, glob, "setTimeout",
                      JS_NewCFunction(ctx, qjs_noop, "setTimeout", 2));
    JS_SetPropertyStr(ctx, glob, "setInterval",
                      JS_NewCFunction(ctx, qjs_noop, "setInterval", 2));
    JS_SetPropertyStr(ctx, glob, "clearTimeout",
                      JS_NewCFunction(ctx, qjs_noop, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, glob, "clearInterval",
                      JS_NewCFunction(ctx, qjs_noop, "clearInterval", 1));
    JS_SetPropertyStr(ctx, glob, "requestAnimationFrame",
                      JS_NewCFunction(ctx, qjs_noop, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, glob, "addEventListener",
                      JS_NewCFunction(ctx, qjs_doc_addEventListener,
                                      "addEventListener", 2));
    JS_FreeValue(ctx, glob);
}

/* ── vtable entry points ─────────────────────────────────────────────────── */
static int quickjs_init(JsBridge *b, const char *baseUrl)
{
    QjsState *st = (QjsState *)JMalloc(sizeof(QjsState));
    if (!st)
    {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    b->implState = st;

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

    if (qjs_build_element_proto(st->ctx, st) != 0)
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
    return 0;
}

static void quickjs_run_script(JsBridge *b, const char *src, size_t len,
                               int index)
{
    QjsState *st = (QjsState *)b->implState;
    if (!st || !st->ctx)
    {
        return;
    }
    if (len == 0 || len > JSBRIDGE_MAX_SCRIPT_BYTES)
    {
        if (len > JSBRIDGE_MAX_SCRIPT_BYTES)
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
    /* Same compile-safety gate as muJS/Duktape (one bar for every engine). */
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

    b->ran++;
    /* QuickJS's lexer reads the sentinel byte at input[len] expecting NUL
     * (an in-place span is followed by '</script>' → "unexpected token
     * '<'" at EOF), so copy like the other engines and NUL-terminate. */
    char *buf = (char *)JMalloc(len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, src, len);
    buf[len] = '\0';
    JSValue result = JS_Eval(st->ctx, buf, len, "[page]",
                             JS_EVAL_TYPE_GLOBAL);
    JFree(buf);
    if (JS_IsException(result))
    {
        b->errs++;
        logger_log("[js] script %d failed", index);
        JSValue exc = JS_GetException(st->ctx);
        const char *msg = JS_ToCString(st->ctx, exc);
        bridge_take_error_text(b, msg ? msg : "exception");
        if (msg)
        {
            JS_FreeCString(st->ctx, msg);
        }
        JS_FreeValue(st->ctx, exc);
    }
    else
    {
        JS_FreeValue(st->ctx, result);
    }
    JS_RunGC(st->rt); /* per-script sweep, mirrors the other bridges */
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
            JSValue exc = JS_GetException(st->ctx);
            const char *msg = JS_ToCString(st->ctx, exc);
            bridge_take_error_text(b, msg ? msg : "exception");
            if (msg)
            {
                JS_FreeCString(st->ctx, msg);
            }
            JS_FreeValue(st->ctx, exc);
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
        JS_FreeValue(st->ctx, st->elProto);
        st->elProto = JS_UNDEFINED;
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

const JsEngineImpl js_engine_quickjs = {
    quickjs_init, quickjs_run_script, quickjs_dispatch_click, quickjs_close,
    "QuickJS"};
