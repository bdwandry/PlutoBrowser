/*
 * PlutoBrowser — jsbridge.c
 * muJS 1.3.10 bridge: engine lifecycle + browser/DOM surface bound to the
 * live DomResult that document_parse walks. See jsbridge.h for the design
 * contract and limits.
 *
 * muJS facts this file relies on (verified against Source/js 1.3.10):
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

typedef struct
{
    const char *ref; /* muJS registry ref to the handler function */
    DomNode *target; /* element the listener was registered on */
} JsListener;

struct JsBridge
{
    js_State *J;
    DocParseResult *doc; /* borrowed; the browser owns it */
    DomResult *dom;      /* == doc->_dom; live tree (browser frees it) */

    /* document.write capture */
    StrBuf output;
    int outputDropped; /* capture limit hit */

    JsListener listeners[JSBRIDGE_LISTENERS_MAX];
    int listenerCount;

    int callBudget;  /* DOM-mutation budget (per page) */
    int inClick;     /* dispatch re-entrancy guard */
    int preventDef;  /* event.preventDefault() flag during dispatch */

    char lastError[128];
    int ran, errs;
};

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

/* One shared budget bounds DOM work per page; exhausted budget throws a
 * catchable error so a page cannot spin the browser. */
static int budget_take(JsBridge *b)
{
    if (b->callBudget <= 0)
    {
        return 0;
    }
    b->callBudget--;
    return 1;
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
    L->ref = js_ref(J);
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

/* ── script extraction (mirrors the tokenizer's <script> skip rules) ────── */
static const char *find_from(const char *hay, const char *end, const char *n,
                             size_t nlen)
{
    if (nlen == 0 || (size_t)(end - hay) < nlen)
    {
        return NULL;
    }
    for (const char *p = hay; p + nlen <= end; p++)
    {
        if (p[0] == n[0] && memcmp(p, n, nlen) == 0)
        {
            return p;
        }
    }
    return NULL;
}

/* Extract <script> body spans (pointers into the raw HTML). Returns the
 * number found (may exceed `max`; the caller runs the first max). */
static int extract_scripts(const char *html, const char **starts, size_t *lens,
                           int max)
{
    const char *pos = html;
    const char *end = html + strlen(html);
    int count = 0;

    while (pos < end)
    {
        const char *lt = memchr(pos, '<', (size_t)(end - pos));
        if (!lt)
        {
            break;
        }
        /* Only a tag start ("<s…") can open a script. */
        if (lt + 1 >= end || (lt[1] | 0x20) != 's')
        {
            pos = lt + 1;
            continue;
        }
        const char *gt = find_from(lt, end, ">", 1);
        if (!gt)
        {
            break;
        }
        const char *b = lt + 1;
        /* Tag name must be exactly "script" (case-insensitive). */
        if (gt - b < 6 || (b[0] | 0x20) != 's' || (b[1] | 0x20) != 'c' ||
            (b[2] | 0x20) != 'r' || (b[3] | 0x20) != 'i' || (b[4] | 0x20) != 'p' ||
            (b[5] | 0x20) != 't')
        {
            pos = gt + 1;
            continue;
        }
        const char *afterName = b + 6;
        if (afterName < gt &&
            ((*afterName >= 'a' && *afterName <= 'z') ||
             (*afterName >= 'A' && *afterName <= 'Z')))
        {
            pos = gt + 1;
            continue; /* <scriptx … */
        }
        /* src= scripts: external JS is not fetched (no second engine pass);
         * skip the element like the tokenizer does. */
        for (const char *q = afterName; q + 3 <= gt; q++)
        {
            if ((q[0] | 0x20) == 's' && (q[1] | 0x20) == 'r' &&
                (q[2] | 0x20) == 'c')
            {
                const char *close = find_from(gt + 1, end, "</script", 8);
                if (!close)
                {
                    return count;
                }
                pos = close + 8;
                goto next;
            }
        }

        {
            const char *close = find_from(gt + 1, end, "</script", 8);
            if (!close)
            {
                const char *c2 = gt + 1;
                while (c2 + 8 <= end)
                {
                    if (c2[0] == '<' && c2[1] == '/' &&
                        (c2[2] | 0x20) == 's' && (c2[3] | 0x20) == 'c' &&
                        (c2[4] | 0x20) == 'r' && (c2[5] | 0x20) == 'i' &&
                        (c2[6] | 0x20) == 'p' && (c2[7] | 0x20) == 't')
                    {
                        close = c2;
                        break;
                    }
                    c2++;
                }
            }
            if (!close)
            {
                break; /* unterminated: drop (tokenizer parity) */
            }

            if (count < max)
            {
                starts[count] = gt + 1;
                lens[count] = (size_t)(close - (gt + 1));
            }
            count++;

            pos = close + 8;
        }
    next:
        while (pos < end && *pos != '>')
        {
            pos++;
        }
        if (pos < end)
        {
            pos++;
        }
    }
    return count;
}

/* ── compile-safety pre-scan (device stack guard) ──────────────────────────
 * The vendored muJS compiler is used exactly as shipped (no Source/js edits).
 * Its parser recurses per nesting level and its regex compiler recurses on
 * consecutive escapes — on the device's 61.8KB game-task stack a pathological
 * script can overflow the stack and corrupt the task, crashing far from the
 * cause. This scan runs in OUR code before the engine sees the script and
 * rejects inputs whose nesting/regex shape could plausibly exceed the
 * budget. Heuristic by design: false rejects (deep-but-legal scripts) are
 * acceptable on this hardware; a stack overflow is not. */
#define PLUTO_SCAN_MAX_DEPTH 40   /* parser nesting levels (muJS ASTLIMIT 400) */
#define PLUTO_SCAN_MAX_REGEX_LEN 8192
#define PLUTO_SCAN_MAX_REGEX_ESC 64  /* consecutive escapes = recursion depth */
#define PLUTO_SCAN_MAX_OPENS 2000    /* total {[( opens per script: bounds the
                                      * total parser WORK (nodes emitted), not
                                      * just depth — a 40KB flat script with
                                      * thousands of blocks is parser load
                                      * even when nesting stays shallow */
#define PLUTO_SCAN_MAX_REGEXES 12    /* regex literals per script: each regex
                                      * compiles through js_regcompx (the
                                      * engine's regexp compiler); bound the
                                      * count so one script cannot queue a
                                      * long chain of compiles (google.com:
                                      * 16 regexes across 10 scripts) */
static int pluto_script_compile_safe(const char *src, size_t len)
{
    static const char *const kw[] = {
        "return", "typeof", "instanceof", "in", "of", "new", "delete",
        "void", "case", "do", "else", "throw", "yield", "await"
    };
    int depth = 0, maxDepth = 0;
    int rxLen = 0, rxEsc = 0;
    int opens = 0;      /* total {[( seen — bounds total parser work */
    int regexCount = 0; /* regex literals seen — bounds js_regcompx calls */
    char q = 0; /* active quote: 0, '"', '\'', '/' */
    int inLine = 0, inBlock = 0;
    const char *p = src, *end = src + len;

    while (p < end)
    {
        char c = *p;
        if (inLine)
        {
            if (c == '\n') inLine = 0;
        }
        else if (inBlock)
        {
            if (c == '*' && p + 1 < end && p[1] == '/')
            {
                inBlock = 0;
                p++;
            }
        }
        else if (q == '/')
        {
            if (c == '\\')
            {
                if (p + 1 < end) { p++; rxEsc++; }
            }
            else if (c == '\n')
            {
                q = 0; /* unterminated: treat as division, keep scanning */
            }
            else if (c == '/')
            {
                q = 0;
            }
        }
        else if (q == '"' || q == '\'')
        {
            if (c == '\\')
            {
                if (p + 1 < end) p++;
            }
            else if (c == q)
            {
                q = 0;
            }
        }
        else if (c == '/' && p + 1 < end && p[1] == '/')
        {
            inLine = 1;
            p++;
        }
        else if (c == '/' && p + 1 < end && p[1] == '*')
        {
            inBlock = 1;
            p++;
        }
        else if (c == '"' || c == '\'')
        {
            q = c;
        }
        else if (c == '/')
        {
            /* Division-vs-regex heuristic: a '/' after something that can
             * end an expression is division; after an operator/keyword it
             * starts a regex literal. */
            const char *s = p - 1;
            while (s >= src && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
                s--;
            if (s >= src && (isalnum((unsigned char)*s) || *s == '_' || *s == '$'))
            {
                const char *w = s;
                while (w >= src && (isalnum((unsigned char)*w) || *w == '_' || *w == '$'))
                    w--;
                size_t wl = (size_t)(s - w);
                int isKw = 0;
                for (size_t k = 0; k < sizeof(kw) / sizeof(kw[0]); k++)
                {
                    if (wl == strlen(kw[k]) && strncmp(w + 1, kw[k], wl) == 0)
                    {
                        isKw = 1;
                        break;
                    }
                }
                if (!isKw)
                {
                    p++; /* division */
                    continue;
                }
            }
            q = '/'; /* regex literal */
            regexCount++;
            if (regexCount > PLUTO_SCAN_MAX_REGEXES)
            {
                return 0;
            }
            rxLen = 0;
            rxEsc = 0;
        }
        else if (c == '{' || c == '(' || c == '[')
        {
            depth++;
            opens++;
            if (depth > maxDepth) maxDepth = depth;
            if (opens > PLUTO_SCAN_MAX_OPENS)
            {
                return 0;
            }
        }
        else if (c == '}' || c == ')' || c == ']')
        {
            if (depth > 0) depth--;
        }
        if (q == '/' && ++rxLen > PLUTO_SCAN_MAX_REGEX_LEN)
            return 0;
        if (rxEsc > PLUTO_SCAN_MAX_REGEX_ESC)
            return 0;
        p++;
    }
    return maxDepth <= PLUTO_SCAN_MAX_DEPTH;
}

/* ── script execution ────────────────────────────────────────────────────── */
static void run_one_script(JsBridge *b, const char *src, size_t len, int index)
{
    js_State *J = b->J;
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
    /* Compile-safety gate (our code, engine untouched): reject scripts whose
     * nesting or regex shape could overflow the device task stack inside the
     * vendored muJS compiler. Skipped like any other failed script. */
    if (!pluto_script_compile_safe(src, len))
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
    JFree(buf);
}

/* ── document.write output → live DOM (deep copy across arenas) ─────────── */
static int adopt_nodes(JsBridge *b, DomNode *dstParent, const DomNode *src,
                       int depth)
{
    if (depth > 24)
    {
        return -1; /* runaway markup guard */
    }
    for (int i = 0; i < src->childCount; i++)
    {
        const DomNode *c = src->children[i];
        DomNode *copy = NULL;
        if (c->kind == DOM_ELEMENT)
        {
            copy = dom_create_element(b->dom, c->tag);
            if (!copy)
            {
                return -1;
            }
            for (int a = 0; a < c->attrCount; a++)
            {
                const char *v = c->attrs[a].value;
                if (dom_set_attr(b->dom, copy, c->attrs[a].key,
                                 v ? v : "") != 0)
                {
                    return -1;
                }
            }
        }
        else
        {
            copy = dom_create_text(b->dom, c->text ? c->text : "");
            if (!copy)
            {
                return -1;
            }
        }
        if (dom_append_child(b->dom, dstParent, copy) != 0)
        {
            return -1;
        }
        if (c->childCount > 0 && adopt_nodes(b, copy, c, depth + 1) != 0)
        {
            return -1;
        }
    }
    return 0;
}

void js_doc_flush_output(JsBridge *b)
{
    if (!b || !b->dom || b->output.len == 0)
    {
        return;
    }
    TokenizeResult tr;
    if (tokenizer_tokenize(b->output.data ? b->output.data : "", &tr) != 0)
    {
        return;
    }
    DomResult scratch;
    memset(&scratch, 0, sizeof(scratch));
    if (dom_build(&tr, &scratch) == 0 && scratch.root)
    {
        adopt_nodes(b, b->dom->root, scratch.root, 0);
        dom_free_result(&scratch);
    }
    tokenizer_free_result(&tr);
}

/* ── engine lifecycle ────────────────────────────────────────────────────── */
int js_doc_attach(JsBridge **out, DocParseResult *doc, DocScriptPolicy policy)
{
    *out = NULL;
    if (!doc || !doc->_dom || policy == DOC_SCRIPT_OFF)
    {
        return 0;
    }
    JsBridge *b = (JsBridge *)JMalloc(sizeof(JsBridge));
    if (!b)
    {
        return -1;
    }
    memset(b, 0, sizeof(*b));
    b->doc = doc;
    b->dom = (DomResult *)doc->_dom;
    b->callBudget = JSBRIDGE_CALL_BUDGET;

    if (strbuf_init(&b->output) != 0)
    {
        JFree(b);
        return -1;
    }

    js_State *J = js_newstate(js_alloc, NULL, 0);
    if (!J)
    {
        strbuf_free(&b->output);
        JFree(b);
        return -1;
    }
    b->J = J;
    js_setcontext(J, b);
    js_setreport(J, bridge_report);
    /* A panic hook would still fall through to abort() in muJS; every
     * bridge entry point runs under js_pcall, so the panic path is
     * unreachable — keep the report hook as the sole error channel. */

    define_globals(J, b, doc->baseUrl);
    js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);

    const char *starts[JSBRIDGE_MAX_SCRIPTS];
    size_t lens[JSBRIDGE_MAX_SCRIPTS];
    int n = 0;
    int total = 0;
    if (!(policy == DOC_SCRIPT_FULL && doc->extScripts))
    {
        /* OFF/RUN/RUN_KEEP: extract inline bodies up front. FULL defers this
         * (it scans slots instead) and only extracts in its OOM fallback. */
        n = extract_scripts(doc->rawHtml ? doc->rawHtml : "", starts, lens,
                            JSBRIDGE_MAX_SCRIPTS);
        total = (n > JSBRIDGE_MAX_SCRIPTS) ? JSBRIDGE_MAX_SCRIPTS : n;
    }
    if (policy == DOC_SCRIPT_FULL && doc->extScripts)
    {
        /* FULL: execute inline bodies and fetched externals in DOCUMENT
         * ORDER (browser-faithful interleaving). The slot scan parses the
         * same rawHtml with the same first-seen external ordering the
         * prefetcher used, so slot.extIndex maps positionally onto
         * doc->extScripts. Files that never arrived (failed download,
         * refused, over budget) are skipped with a log line — the page and
         * later scripts still run (image-failure philosophy). BOTH scratch
         * arrays are heap-allocated: the 12KB URL table AND the slot table
         * must not join the render-task stack (P19 stack rule) — a stack
         * copy here charged +784B on EVERY page load's parse chain and
         * contributed to a device gameTask stack overflow. */
        JsScriptSlot *slots = (JsScriptSlot *)JMalloc(
            sizeof(JsScriptSlot) * (size_t)JSBRIDGE_MAX_SCRIPTS);
        char (*extRaw)[JSBRIDGE_EXT_URL_MAX] =
            (char (*)[JSBRIDGE_EXT_URL_MAX])JMalloc(
                sizeof(char[JSBRIDGE_MAX_EXT_SCRIPTS][JSBRIDGE_EXT_URL_MAX]));
        if (slots && extRaw)
        {
            int extCount = 0;
            int nslots = jsbridge_scan_scripts(doc->rawHtml, slots,
                                               JSBRIDGE_MAX_SCRIPTS, extRaw,
                                               JSBRIDGE_MAX_EXT_SCRIPTS,
                                               &extCount);
            int totalS = (nslots > JSBRIDGE_MAX_SCRIPTS) ? JSBRIDGE_MAX_SCRIPTS
                                                         : nslots;
            for (int i = 0; i < totalS; i++)
            {
                if (!slots[i].isExt)
                {
                    run_one_script(b, slots[i].inlineStart,
                                   slots[i].inlineLen, i + 1);
                }
                else if (slots[i].extIndex >= 0 &&
                         slots[i].extIndex < doc->extScriptCount)
                {
                    JsExtScript *e =
                        &((JsExtScript *)doc->extScripts)[slots[i].extIndex];
                    if (e->body && e->len)
                    {
                        /* run_one_script copies the source before running,
                         * so the doc-owned body is never aliased. */
                        run_one_script(b, e->body, e->len, i + 1);
                    }
                    else
                    {
                        logger_log("[js] ext skip (not fetched): %s",
                                   e->url[0] ? e->url : "(unresolved)");
                    }
                }
                else
                {
                    logger_log("[js] ext skip (unmapped slot) #%d", i);
                }
                js_gc(J, 0);
            }
        }
        else
        {
            /* Scan-scratch OOM: degrade to inline-only (still logged). */
            logger_log("[js] FULL scan OOM: inline-only fallback");
            n = extract_scripts(doc->rawHtml ? doc->rawHtml : "", starts, lens,
                                JSBRIDGE_MAX_SCRIPTS);
            total = (n > JSBRIDGE_MAX_SCRIPTS) ? JSBRIDGE_MAX_SCRIPTS : n;
            for (int i = 0; i < total; i++)
            {
                run_one_script(b, starts[i], lens[i], i + 1);
                js_gc(J, 0);
            }
        }
        if (slots)
        {
            JFree(slots); /* realloc(p,0) — safe on NULL */
        }
        if (extRaw)
        {
            JFree(extRaw);
        }
    }
    else
    {
        for (int i = 0; i < total; i++)
        {
            run_one_script(b, starts[i], lens[i], i + 1);
            js_gc(J, 0);
        }
    }

    doc->jsRan = b->ran;
    doc->jsErrors = b->errs;
    snprintf(doc->jsLastError, sizeof(doc->jsLastError), "%s", b->lastError);

    /* Both policies return the live bridge: document_parse flushes the
     * document.write capture and closes the engine itself for RUN. */
    *out = b;
    return 0;
}

void js_doc_close(JsBridge *b)
{
    if (!b)
    {
        return;
    }
    logger_log("[js] close: ran=%d errs=%d listeners=%d budgetLeft=%d%s%s",
               b->ran, b->errs, b->listenerCount, b->callBudget,
               b->lastError[0] ? " last=" : "",
               b->lastError[0] ? b->lastError : "");
    if (b->doc && b->doc->_jsbridge == b)
    {
        b->doc->_jsbridge = NULL; /* detach before the engine dies */
    }
    if (b->J)
    {
        for (int i = 0; i < b->listenerCount; i++)
        {
            js_unref(b->J, b->listeners[i].ref);
        }
        js_freestate(b->J);
        b->J = NULL;
    }
    strbuf_free(&b->output);
    JFree(b);
}

/* ── click dispatch (browser → page) ─────────────────────────────────────── */
int jsbridge_dispatch_link_click(JsBridge *b, const void *anchorNode)
{
    if (!b || !b->J || b->inClick || !anchorNode)
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
        js_setlimit(b->J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);

        /* muJS call convention: [fn, this, arg1..argN] then js_pcall(J, n). */
        js_getregistry(b->J, L->ref); /* fn */
        js_pushnull(b->J);            /* this */
        /* event: { type:"click", preventDefault() } */
        js_newobject(b->J);
        {
            js_pushstring(b->J, "click");
            js_setproperty(b->J, -2, "type");
            js_newcfunction(b->J, js_event_preventDefault, "preventDefault", 0);
            js_setproperty(b->J, -2, "preventDefault");
        }
        if (js_pcall(b->J, 1) != 0)
        {
            b->errs++;
            bridge_take_error(b, b->J);
            js_pop(b->J, 1);
        }
        else
        {
            js_pop(b->J, 1);
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

int jsbridge_listener_count(const JsBridge *b)
{
    return b ? b->listenerCount : 0;
}

/* ── Device link shims ───────────────────────────────────────────────────── */
#ifdef TARGET_PLAYDATE
/* js_defaultpanic calls abort() only if no panic hook is installed; the
 * bridge always contains errors via js_pcall, so this is a cold path.
 * Provide local shims so the toolchain's abort/_exit syscall chain isn't
 * pulled into the binary. */
void _exit(int code)
{
    (void)code;
    for (;;)
    {
        /* halted */
    }
}
void abort(void);
void abort(void)
{
    _exit(134);
}
int _write(int fd, const void *buf, unsigned len)
{
    (void)fd;
    (void)buf;
    return (int)len;
}
int _close(int fd)
{
    (void)fd;
    return 0;
}
int _fstat(int fd, void *st)
{
    (void)fd;
    (void)st;
    return 0;
}
int _isatty(int fd)
{
    (void)fd;
    return 0;
}
int _lseek(int fd, int off, int whence)
{
    (void)fd;
    (void)off;
    (void)whence;
    return 0;
}
int _read(int fd, void *buf, unsigned len)
{
    (void)fd;
    (void)buf;
    return 0;
}
int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}
int _getpid(void)
{
    return 1;
}
int _gettimeofday(void *tv, void *tz)
{
    (void)tv;
    (void)tz;
    return -1;
}
#endif
