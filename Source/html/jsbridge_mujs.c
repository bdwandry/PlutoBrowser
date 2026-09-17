/*
 * PlutoBrowser — jsbridge_mujs.c
 * muJS 1.3.10 engine implementation behind the JsEngineImpl vtable. The
 * behavior is byte-for-byte the original jsbridge.c muJS path; only the
 * engine-agnostic plumbing (script discovery, document.write adoption,
 * bridge alloc/free, logging) moved to the router in jsbridge.c.
 *
 * muJS facts this file relies on (verified against Source/js/muJS 1.3.10):
 *   - js_ploadstring compiles without executing and returns non-zero on a
 *     SyntaxError instead of longjmp-ing (report hook gets the message).
 *   - js_pcall runs the loaded chunk with the error contained; the error
 *     value is left on the stack top.
 *   - js_setlimit(runlimit, memlimit): runlimit counts statements/back-edges
 *     and throws the catchable "script ran too long"; memlimit caps each
 *     individual allocation size ("out of memory" thrown).
 *   - js_defaccessor(idx, name, atts) consumes a getter (-2) and a setter
 *     (-1) function — real property accessors on plain objects.
 *   - js_newuserdatax consumes a pushed prototype object; userdata objects
 *     are plain js_Objects, so method dispatch goes through the prototype
 *     and unknown property writes land as own properties (fallthrough via
 *     the put hook returning 0).
 *   - The userdata `put` hook receives the assigned value on the stack top.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "mujs.h"
#include "../core/logger.h"
#include "../util/strbuf.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_pd()->system->realloc(NULL, (n))
#define JFree(p) pluto_pd()->system->realloc((p), 0)

/* ── muJS allocator: route through the SDK so engine memory is pooled ───── */
static void *js_alloc(void *actx, void *ptr, int size)
{
    (void)actx;
    return pluto_pd()->system->realloc(ptr, (size > 0) ? (unsigned)size : 0u);
}

#define DOM_TAG "pluto.dom" /* userdata tag: data = DomNode* */

/* ── error capture ───────────────────────────────────────────────────────── */
static void bridge_report(js_State *J, const char *message)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    if (b && message && !b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError), "%s", message);
    }
}

static void bridge_take_error(JsBridge *b, js_State *J)
{
    if (!b->lastError[0] && !js_isundefined(J, -1) && !js_isnull(J, -1))
    {
        const char *msg = js_tostring(J, -1);
        if (msg)
        {
            snprintf(b->lastError, sizeof(b->lastError), "%s", msg);
        }
    }
}

#define BUDGET_OR_THROW(b)                      \
    do                                          \
    {                                           \
        if (!budget_take(b))                    \
        {                                       \
            js_error(J, "script did too much"); \
        }                                       \
    } while (0)

/* ── element wrappers ──────────────────────────────────────────────────────
 * Element objects are userdata wrapping the live DomNode. Property reads go
 * through the `has` hook (tagName, id, textContent, ...); writes through the
 * `put` hook (id, textContent, innerHTML), anything else falls through to a
 * normal own property so scripts can attach custom data to elements. */
static int dom_has(js_State *J, void *p, const char *name);
static int dom_put(js_State *J, void *p, const char *name);

static void push_element(js_State *J, DomNode *node)
{
    if (!node)
    {
        js_pushnull(J);
        return;
    }
    js_getregistry(J, "pluto.el.proto");
    js_newuserdatax(J, DOM_TAG, node, dom_has, dom_put, NULL, NULL);
}

static DomNode *to_element(js_State *J, int idx)
{
    return (DomNode *)js_touserdata(J, idx, DOM_TAG);
}

static int dom_has(js_State *J, void *p, const char *name)
{
    DomNode *n = (DomNode *)p;
    if (n->kind == DOM_ELEMENT)
    {
        if (!strcmp(name, "tagName"))
        {
            /* HTML tagName is uppercase ("UL"), the tree stores lowercase. */
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
                js_pushstring(J, up);
            }
            else
            {
                js_pushstring(J, n->tag);
            }
            return 1;
        }
        if (!strcmp(name, "id"))
        {
            const char *v = dom_get_attr(n, "id");
            js_pushstring(J, v ? v : "");
            return 1;
        }
        if (!strcmp(name, "textContent"))
        {
            char buf[512];
            doc_concat_node_text(n, buf, sizeof(buf));
            js_pushstring(J, buf);
            return 1;
        }
        if (!strcmp(name, "innerHTML"))
        {
            /* UNSUPPORTED (no subtree serializer): degrades to text. */
            char buf[512];
            doc_concat_node_text(n, buf, sizeof(buf));
            js_pushstring(J, buf);
            return 1;
        }
        if (!strcmp(name, "parentNode"))
        {
            push_element(J, n->parent);
            return 1;
        }
        if (!strcmp(name, "childElementCount"))
        {
            int c = 0;
            for (int i = 0; i < n->childCount; i++)
            {
                if (n->children[i]->kind == DOM_ELEMENT)
                {
                    c++;
                }
            }
            js_pushnumber(J, c);
            return 1;
        }
        if (!strcmp(name, "children"))
        {
            /* Array of element children (indexable, with .length). */
            js_newarray(J);
            int k = 0;
            for (int i = 0; i < n->childCount; i++)
            {
                if (n->children[i]->kind == DOM_ELEMENT)
                {
                    push_element(J, n->children[i]);
                    js_setindex(J, -2, k++);
                }
            }
            return 1;
        }
        if (!strcmp(name, "nodeType"))
        {
            js_pushnumber(J, 1);
            return 1;
        }
    }
    else if (!strcmp(name, "nodeType"))
    {
        js_pushnumber(J, 3); /* text node */
        return 1;
    }
    return 0;
}

static int dom_put(js_State *J, void *p, const char *name)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = (DomNode *)p;
    if (n->kind != DOM_ELEMENT)
    {
        return 0; /* text nodeValue writes land as own props (harmless) */
    }
    if (!strcmp(name, "id"))
    {
        if (!budget_take(b))
        {
            js_error(J, "script did too much");
        }
        dom_set_attr(b->dom, n, "id", js_tostring(J, -1));
        return 1;
    }
    if (!strcmp(name, "textContent"))
    {
        if (!budget_take(b))
        {
            js_error(J, "script did too much");
        }
        if (dom_set_text(b->dom, n, js_tostring(J, -1)) != 0)
        {
            js_error(J, "textContent assignment failed");
        }
        return 1;
    }
    if (!strcmp(name, "innerHTML"))
    {
        /* PARTIAL: treated as textContent (no markup parsing here). */
        if (!budget_take(b))
        {
            js_error(J, "script did too much");
        }
        dom_set_text(b->dom, n, js_tostring(J, -1));
        return 1;
    }
    return 0; /* everything else: normal own property */
}

/* ── console ─────────────────────────────────────────────────────────────── */
static void js_console_log(js_State *J)
{
    int top = js_gettop(J);
    char line[160];
    size_t off = 0;
    line[0] = '\0';
    for (int i = 1; i < top && off < sizeof(line) - 1; i++)
    {
        const char *s = js_tostring(J, i);
        if (s)
        {
            size_t n = strlen(s);
            if (off + n >= sizeof(line) - 2)
            {
                n = sizeof(line) - 2 - off;
            }
            memcpy(line + off, s, n);
            off += n;
            if (i < top - 1)
            {
                line[off++] = ' ';
            }
            line[off] = '\0';
        }
    }
    logger_log("[js] %s", line);
    js_pushundefined(J);
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

static void js_document_write(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int top = js_gettop(J);
    for (int i = 1; i < top; i++)
    {
        BUDGET_OR_THROW(b);
        dw_append(b, js_tostring(J, i));
    }
    js_pushundefined(J);
}

static void js_document_writeln(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int top = js_gettop(J);
    for (int i = 1; i < top; i++)
    {
        BUDGET_OR_THROW(b);
        dw_append(b, js_tostring(J, i));
    }
    dw_append(b, "\n");
    js_pushundefined(J);
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

static void js_document_getElementById(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    const char *id = js_tostring(J, 1);
    BUDGET_OR_THROW(b);
    if (!b->dom || !id)
    {
        js_pushnull(J);
        return;
    }
    /* Real `id` attribute first, then the runtime _dN key. */
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
    push_element(J, hit);
}

static void js_document_createElement(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    BUDGET_OR_THROW(b);
    DomNode *el = dom_create_element(b->dom, js_tostring(J, 1));
    if (!el)
    {
        js_error(J, "createElement failed");
    }
    push_element(J, el);
}

static void js_document_createTextNode(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    BUDGET_OR_THROW(b);
    DomNode *t = dom_create_text(b->dom, js_tostring(J, 1));
    if (!t)
    {
        js_error(J, "createTextNode failed");
    }
    push_element(J, t);
}

static void js_document_getTitle(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    /* doc->title is only filled by the walker (after scripts run); walk the
     * live tree's <title> so scripts and the chrome agree. */
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
                    js_pushstring(J, buf);
                    return;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    js_pushstring(J, b->doc->title[0] ? b->doc->title : "");
}

/* ── element prototype methods ───────────────────────────────────────────── */
static void js_el_getAttribute(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    BUDGET_OR_THROW(b);
    const char *v = dom_get_attr(n, js_tostring(J, 1));
    if (v)
    {
        js_pushstring(J, v);
    }
    else
    {
        js_pushnull(J);
    }
}

static void js_el_setAttribute(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    const char *k = js_tostring(J, 1);
    const char *v = js_tostring(J, 2);
    BUDGET_OR_THROW(b);
    if (dom_set_attr(b->dom, n, k, v) != 0)
    {
        js_error(J, "setAttribute failed");
    }
    js_pushundefined(J);
}

static void js_el_removeAttribute(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    BUDGET_OR_THROW(b);
    dom_remove_attr(n, js_tostring(J, 1));
    js_pushundefined(J);
}

static void js_el_appendChild(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    DomNode *c = to_element(J, 1);
    BUDGET_OR_THROW(b);
    if (dom_append_child(b->dom, n, c) != 0)
    {
        js_error(J, "appendChild failed");
    }
    js_pushundefined(J);
}

static void js_el_removeChild(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    DomNode *c = to_element(J, 1);
    BUDGET_OR_THROW(b);
    if (dom_remove_child(n, c) != 0)
    {
        js_error(J, "removeChild failed");
    }
    js_pushundefined(J);
}

static void js_el_addEventListener(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    const char *type = js_tostring(J, 1);
    if (!js_iscallable(J, 2))
    {
        js_typeerror(J, "listener must be a function");
    }
    if (!type || strcmp(type, "click") != 0)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so pages
         * registering them don't error — only click is delivered. */
        js_pushundefined(J);
        return;
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        js_error(J, "too many event listeners");
    }
    JsListener *L = &b->listeners[b->listenerCount++];
    js_copy(J, 2);
    L->ref = (void *)js_ref(J);
    L->target = n;
    js_pushundefined(J);
}

static void js_el_get_by_tag(js_State *J)
{
    /* getElementsByTagName → array of element wrappers (subtree scan). */
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    const char *tag = js_tostring(J, 1);
    BUDGET_OR_THROW(b);
    js_newarray(J);
    int k = 0;
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
                push_element(J, c);
                js_setindex(J, -2, k++);
            }
            if (top + 1 < DOM_SEARCH_MAX_DEPTH)
            {
                stack[top++] = c;
            }
        }
    }
}

/* ── event object (click dispatch) ───────────────────────────────────────── */
static void js_event_preventDefault(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    b->preventDef = 1;
    js_pushundefined(J);
}

/* ── window / global helpers ─────────────────────────────────────────────── */
static void js_alert(js_State *J)
{
    logger_log("[js alert] %s", js_tostring(J, 1));
    js_pushundefined(J);
}

static void js_noop(js_State *J)
{
    (void)J;
    js_pushundefined(J);
}

/* document/window.addEventListener: UNSUPPORTED delivery (no bubbling model);
 * accepted silently so common pages don't throw. Element-level click works. */
static void js_doc_addEventListener(js_State *J)
{
    (void)J;
    js_pushundefined(J);
}

static void js_location_assign(js_State *J)
{
    /* Navigation from scripts is not supported this pass: logged only. */
    logger_log("[js] location.assign/replace ignored: %s", js_tostring(J, 1));
    js_pushundefined(J);
}

/* ── environment construction ────────────────────────────────────────────── */
static void define_element_proto(js_State *J)
{
    js_newobject(J);
    {
        js_newcfunction(J, js_el_getAttribute, "getAttribute", 1);
        js_setproperty(J, -2, "getAttribute");
        js_newcfunction(J, js_el_setAttribute, "setAttribute", 2);
        js_setproperty(J, -2, "setAttribute");
        js_newcfunction(J, js_el_removeAttribute, "removeAttribute", 1);
        js_setproperty(J, -2, "removeAttribute");
        js_newcfunction(J, js_el_appendChild, "appendChild", 1);
        js_setproperty(J, -2, "appendChild");
        js_newcfunction(J, js_el_removeChild, "removeChild", 1);
        js_setproperty(J, -2, "removeChild");
        js_newcfunction(J, js_el_addEventListener, "addEventListener", 2);
        js_setproperty(J, -2, "addEventListener");
        js_newcfunction(J, js_el_get_by_tag, "getElementsByTagName", 1);
        js_setproperty(J, -2, "getElementsByTagName");
    }
    js_setregistry(J, "pluto.el.proto");
}

static void define_document(js_State *J, JsBridge *b)
{
    js_newobject(J);
    {
        js_newcfunction(J, js_document_getElementById, "getElementById", 1);
        js_setproperty(J, -2, "getElementById");
        js_newcfunction(J, js_document_createElement, "createElement", 1);
        js_setproperty(J, -2, "createElement");
        js_newcfunction(J, js_document_createTextNode, "createTextNode", 1);
        js_setproperty(J, -2, "createTextNode");
        /* document.title: real accessor property (getter reads the live
         * tree's <title>; the walker fills doc->title only after scripts). */
        js_newcfunction(J, js_document_getTitle, "get title", 0); /* getter */
        js_pushundefined(J);                                      /* setter */
        js_defaccessor(J, -3, "title", JS_READONLY); /* pops getter+setter */
        js_newcfunction(J, js_document_write, "write", 1);
        js_setproperty(J, -2, "write");
        js_newcfunction(J, js_document_writeln, "writeln", 1);
        js_setproperty(J, -2, "writeln");
        js_newcfunction(J, js_doc_addEventListener, "addEventListener", 2);
        js_setproperty(J, -2, "addEventListener");
        /* document.body: element wrapper (falls back to the root). */
        DomNode *body = first_element(b->dom->root, "body");
        push_element(J, body ? body : b->dom->root);
        js_setproperty(J, -2, "body");
    }
    js_setglobal(J, "document");
}

static void define_globals(js_State *J, JsBridge *b, const char *baseUrl)
{
    /* Bare window object: identity alias for the global object. */
    js_pushglobal(J);
    js_setglobal(J, "window");

    define_element_proto(J);
    define_document(J, b);

    /* navigator */
    js_newobject(J);
    {
        js_pushstring(J, "PlutoBrowser/1.0 (Playdate; muJS 1.3.10 ES5)");
        js_setproperty(J, -2, "userAgent");
        js_pushstring(J, "PlutoBrowser");
        js_setproperty(J, -2, "appCodeName");
        js_pushstring(J, "1.0");
        js_setproperty(J, -2, "appVersion");
    }
    js_setglobal(J, "navigator");

    /* console */
    js_newobject(J);
    {
        js_newcfunction(J, js_console_log, "log", 0);
        js_setproperty(J, -2, "log");
        js_newcfunction(J, js_console_log, "warn", 0);
        js_setproperty(J, -2, "warn");
        js_newcfunction(J, js_console_log, "error", 0);
        js_setproperty(J, -2, "error");
    }
    js_setglobal(J, "console");

    /* location: href is a data string; assign/replace logged + ignored. */
    js_newobject(J);
    {
        js_pushstring(J, baseUrl ? baseUrl : "");
        js_setproperty(J, -2, "href");
        js_pushstring(J, baseUrl ? baseUrl : "");
        js_setproperty(J, -2, "host");
        js_newcfunction(J, js_location_assign, "assign", 1);
        js_setproperty(J, -2, "assign");
        js_newcfunction(J, js_location_assign, "replace", 1);
        js_setproperty(J, -2, "replace");
    }
    js_setglobal(J, "location");

    js_newcfunction(J, js_alert, "alert", 1);
    js_setglobal(J, "alert");
    js_newcfunction(J, js_noop, "confirm", 1);
    js_setglobal(J, "confirm");
    js_newcfunction(J, js_noop, "prompt", 1);
    js_setglobal(J, "prompt");
    js_newcfunction(J, js_noop, "setTimeout", 2);
    js_setglobal(J, "setTimeout");
    js_newcfunction(J, js_noop, "setInterval", 2);
    js_setglobal(J, "setInterval");
    js_newcfunction(J, js_noop, "clearTimeout", 1);
    js_setglobal(J, "clearTimeout");
    js_newcfunction(J, js_noop, "clearInterval", 1);
    js_setglobal(J, "clearInterval");
    js_newcfunction(J, js_noop, "requestAnimationFrame", 1);
    js_setglobal(J, "requestAnimationFrame");
    js_newcfunction(J, js_doc_addEventListener, "window.addEventListener", 2);
    js_setglobal(J, "addEventListener");
}

/* ── script execution ────────────────────────────────────────────────────── */
static void mujs_run_script(JsBridge *b, const char *src, size_t len, int index)
{
    js_State *J = (js_State *)b->implState;
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
    /* Compile-safety gate (shared by ALL engines, lives in the router):
     * reject scripts whose nesting or regex shape could overflow the device
     * task stack inside the vendored muJS compiler. Skipped like any other
     * failed script. */
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
    char *buf = (char *)JMalloc(len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, src, len);
    buf[len] = '\0';
    /* (gc runs at the tail of every run below, after each script) */

    /* Limits reset per script: the counters decrement monotonically. */
    js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);
    b->ran++;
    if (js_ploadstring(J, "[page]", buf) != 0)
    {
        b->errs++;
        /* Compile failure (syntax or the per-script allocation budget):
         * record which slot failed — muJS's error object needs an extra
         * unwrap, so the log line carries the position, not the message. */
        logger_log("[js] script %d failed to compile", index);
        js_pop(J, 1); /* error object */
    }
    else
    {
        js_pushundefined(J);
        if (js_pcall(J, 0) != 0)
        {
            b->errs++;
            bridge_take_error(b, J);
            js_pop(J, 1);
        }
        else
        {
            js_pop(J, 1); /* the pushed return value */
        }
    }
    js_gc(J, 0); /* per-script sweep, as the original bridge did */
    JFree(buf);
}

/* ── vtable entry points ─────────────────────────────────────────────────── */
static int mujs_init(JsBridge *b, const char *baseUrl)
{
    js_State *J = js_newstate(js_alloc, NULL, 0);
    if (!J)
    {
        return -1;
    }
    b->implState = J;
    js_setcontext(J, b);
    js_setreport(J, bridge_report);
    /* A panic hook would still fall through to abort() in muJS; every
     * bridge entry point runs under js_pcall, so the panic path is
     * unreachable — keep the report hook as the sole error channel. */

    define_globals(J, b, baseUrl);
    js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);
    return 0;
}

static int mujs_dispatch_click(JsBridge *b, const void *anchorNode)
{
    js_State *J = (js_State *)b->implState;
    if (!J)
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
        js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);

        /* muJS call convention: [fn, this, arg1..argN] then js_pcall(J, n). */
        js_getregistry(J, (const char *)L->ref); /* fn */
        js_pushnull(J);            /* this */
        /* event: { type:"click", preventDefault() } */
        js_newobject(J);
        {
            js_pushstring(J, "click");
            js_setproperty(J, -2, "type");
            js_newcfunction(J, js_event_preventDefault, "preventDefault", 0);
            js_setproperty(J, -2, "preventDefault");
        }
        if (js_pcall(J, 1) != 0)
        {
            b->errs++;
            bridge_take_error(b, J);
            js_pop(J, 1);
        }
        else
        {
            js_pop(J, 1);
        }
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

static void mujs_close(JsBridge *b)
{
    js_State *J = (js_State *)b->implState;
    if (!J)
    {
        return;
    }
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        js_unref(J, (const char *)L->ref);
    }
    js_freestate(J);
    b->implState = NULL;
}

const JsEngineImpl js_engine_mujs = {
    mujs_init, mujs_run_script, mujs_dispatch_click, mujs_close, "muJS"};
