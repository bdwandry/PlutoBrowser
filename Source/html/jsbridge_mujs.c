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
#include "../core/pluto_mem.h"
#include "../html/dom.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)

/* ── muJS allocator: route through the SDK so engine memory is pooled ───── */
static void *js_alloc(void *actx, void *ptr, int size)
{
    (void)actx;
    return pluto_mem_realloc(ptr, (size > 0) ? (unsigned)size : 0u);
}

#define DOM_TAG "pluto.dom" /* userdata tag: data = DomNode* */
#define DOM_CL_TAG "pluto.cl" /* classList userdata: data = DomNode* */
#define DOM_STYLE_TAG "pluto.style" /* style userdata: data = DomNode* */

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

/* ── SW5: classList + style userdata hooks (this = the userdata; the C
 * functions read the element out of the boxed DomNode* on stack slot 0 —
 * muJS passes the BOX as the hook's p and method dispatch puts the box at
 * index 0 of a cfunction call like any `this`). */
static DomNode *cl_node(js_State *J)
{
    return (DomNode *)js_touserdata(J, 0, DOM_CL_TAG);
}

static DomNode *style_node(js_State *J)
{
    return (DomNode *)js_touserdata(J, 0, DOM_STYLE_TAG);
}

static void js_cl_add(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    if (jsbridge_el_class_add(b, cl_node(J), js_tostring(J, 1)) != 0)
    {
        js_error(J, "classList.add failed");
    }
    js_pushundefined(J);
}

static void js_cl_remove(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    if (jsbridge_el_class_remove(b, cl_node(J), js_tostring(J, 1)) != 0)
    {
        js_error(J, "classList.remove failed");
    }
    js_pushundefined(J);
}

static void js_cl_toggle(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int rc = jsbridge_el_class_toggle(b, cl_node(J), js_tostring(J, 1));
    if (rc < 0)
    {
        js_error(J, "classList.toggle failed");
    }
    js_pushboolean(J, rc == 1);
}

static void js_cl_contains(js_State *J)
{
    js_pushboolean(J, jsbridge_el_class_has(cl_node(J), js_tostring(J, 1)));
}

static void js_cl_item(js_State *J)
{
    DomNode *n = cl_node(J);
    int idx = js_isundefined(J, 1) ? -1 : (int)js_tointeger(J, 1);
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
            js_pushstring(J, tok);
            return;
        }
        p += n2;
    }
    js_pushnull(J);
}

static void js_cl_length(js_State *J)
{
    DomNode *n = cl_node(J);
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
    js_pushnumber(J, (double)k);
}

/* style object: read/write individual properties over the element's inline
 * style attribute. Known properties map onto the walker's vocabulary
 * (display/visibility/text-align/font-weight/font-style/text-decoration/
 * color/background); unknown property NAMES are accepted and stored so
 * legacy patterns don't throw. Values are plain strings. */
static void style_read_prop(const DomNode *n, const char *prop, char *out,
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

static int style_has(js_State *J, void *p, const char *name)
{
    DomNode *n = (DomNode *)p;
    char val[128];
    style_read_prop(n, name, val, sizeof(val));
    js_pushstring(J, val);
    return 1;
}

static int style_put(js_State *J, void *up, const char *name)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = (DomNode *)up;
    if (!n || n->kind != DOM_ELEMENT)
    {
        return 0;
    }
    const char *value = js_tostring(J, -1);
    if (!budget_take(b))
    {
        js_error(J, "script did too much");
    }
    /* Rewrite the inline style attribute preserving other declarations.
     * buf MUST be zero-terminated up front: on a clear (empty value) with
     * no other declarations the loop never writes it, and stale stack
     * bytes would otherwise re-commit the previous style string. */
    char buf[512];
    buf[0] = '\0';
    size_t off = 0;
    size_t plen = strlen(name);
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
            strncmp(seg, name, plen) != 0)
        {
            /* keep this declaration verbatim */
            while (seg < p && (seg[segLen - 1] == ' ' ||
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
    if (value[0])
    {
        int n1 = snprintf(buf + off, sizeof(buf) - off, "%s%s: %s",
                          off ? ";" : "", name, value);
        if (n1 < 0 || (size_t)n1 >= sizeof(buf) - off)
        {
            return 1; /* style attr overflow: drop the write, don't throw */
        }
    }
    dom_set_attr(b->dom, n, "style", buf);
    return 1;
}

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
        if (!strcmp(name, "firstElementChild"))
        {
            /* SW5: live element-child navigation (append-before patterns). */
            push_element(J, dom_first_element_child(n));
            return 1;
        }
        if (!strcmp(name, "nextElementSibling"))
        {
            push_element(J, dom_next_element_sibling(n));
            return 1;
        }
        if (!strcmp(name, "classList"))
        {
            /* SW5 (O4): live token list over the class attribute. Built
             * fresh per access; methods mutate through the router. */
            js_newobject(J);
            js_newuserdatax(J, DOM_CL_TAG, n, NULL, NULL, NULL, NULL);
            js_newcfunction(J, js_cl_add, "add", 1);
            js_setproperty(J, -2, "add");
            js_newcfunction(J, js_cl_remove, "remove", 1);
            js_setproperty(J, -2, "remove");
            js_newcfunction(J, js_cl_toggle, "toggle", 1);
            js_setproperty(J, -2, "toggle");
            js_newcfunction(J, js_cl_contains, "contains", 1);
            js_setproperty(J, -2, "contains");
            js_newcfunction(J, js_cl_item, "item", 1);
            js_setproperty(J, -2, "item");
            js_newcfunction(J, js_cl_length, "get length", 0);
            js_pushundefined(J);
            js_defaccessor(J, -3, "length", JS_READONLY);
            return 1;
        }
        if (!strcmp(name, "style"))
        {
            /* SW5: style OBJECT (legacy pattern — sites assign
             * el.style.display = 'none' and read it back). A userdata with
             * has/put hooks over the element's inline style="…" attribute
             * (the walker already applies inline style); NOT the CSSOM. */
            js_newobject(J);
            js_newuserdatax(J, DOM_STYLE_TAG, n, style_has, style_put,
                            NULL, NULL);
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
        /* SW5: REAL markup assignment (was: textContent degradation). The
         * router parses + adopts; failures throw contained. */
        size_t slen = 0;
        const char *s = js_tostring(J, -1);
        slen = s ? strlen(s) : 0;
        if (!budget_take(b))
        {
            js_error(J, "script did too much");
        }
        if (jsbridge_el_set_inner_html(b, n, s, slen) != 0)
        {
            js_error(J, "innerHTML assignment failed");
        }
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

static int qs_emit_mujs(void *elp, void *ud); /* defined with el methods */

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

/* document.querySelector(All): scoped at the document root (SW5). */
static void js_document_querySelector(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    const char *sel = js_tostring(J, 1);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    DomNode *root = b->dom ? b->dom->root : NULL;
    push_element(J, (DomNode *)jsbridge_el_query_selector_first(
                        b, root, sel, scratch, sizeof(scratch)));
}

static void js_document_querySelectorAll(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    const char *sel = js_tostring(J, 1);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    DomNode *root = b->dom ? b->dom->root : NULL;
    js_newarray(J);
    jsbridge_el_query_selector_all(b, root, sel, scratch, sizeof(scratch),
                                   qs_emit_mujs, (void *)J);
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

/* ── SW5: insertBefore / querySelector(All) / classList-backed methods ──── */
static void js_el_insertBefore(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    DomNode *c = to_element(J, 1);
    DomNode *ref = js_isnull(J, 2) || js_isundefined(J, 2) ? NULL
                                                           : to_element(J, 2);
    BUDGET_OR_THROW(b);
    if (!n || !c || dom_insert_before(b->dom, n, c, ref) != 0)
    {
        js_error(J, "insertBefore failed");
    }
    js_pushundefined(J);
}

/* querySelector emit: muJS pushes wrappers straight onto the engine stack
 * inside an array being built at -1. */
static int qs_emit_mujs(void *elp, void *ud)
{
    js_State *J = (js_State *)ud;
    push_element(J, (DomNode *)elp);
    js_setindex(J, -2, js_getlength(J, -2));
    return 0;
}

static void js_el_querySelectorAll(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    const char *sel = js_tostring(J, 1);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    js_newarray(J);
    jsbridge_el_query_selector_all(b, n, sel, scratch, sizeof(scratch),
                                   qs_emit_mujs, (void *)J);
}

static void js_el_querySelector(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    DomNode *n = to_element(J, 0);
    const char *sel = js_tostring(J, 1);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    push_element(J, (DomNode *)jsbridge_el_query_selector_first(
                        b, n, sel, scratch, sizeof(scratch)));
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
        js_newcfunction(J, js_el_insertBefore, "insertBefore", 2);
        js_setproperty(J, -2, "insertBefore");
        js_newcfunction(J, js_el_querySelector, "querySelector", 1);
        js_setproperty(J, -2, "querySelector");
        js_newcfunction(J, js_el_querySelectorAll, "querySelectorAll", 1);
        js_setproperty(J, -2, "querySelectorAll");
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
        js_newcfunction(J, js_document_querySelector, "querySelector", 1);
        js_setproperty(J, -2, "querySelector");
        js_newcfunction(J, js_document_querySelectorAll, "querySelectorAll", 1);
        js_setproperty(J, -2, "querySelectorAll");
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

/* Forward declarations: mujs_init binds these globals before their bodies. */
static void mujs_define_xhr(js_State *J);
static void js_setTimeout(js_State *J);
static void js_setInterval(js_State *J);
static void js_clearTimeout(js_State *J);
static void js_clearInterval(js_State *J);

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
    js_newcfunction(J, js_setTimeout, "setTimeout", 2);
    js_setglobal(J, "setTimeout");
    js_newcfunction(J, js_setInterval, "setInterval", 2);
    js_setglobal(J, "setInterval");
    js_newcfunction(J, js_clearTimeout, "clearTimeout", 1);
    js_setglobal(J, "clearTimeout");
    js_newcfunction(J, js_clearInterval, "clearInterval", 1);
    js_setglobal(J, "clearInterval");
    js_newcfunction(J, js_setTimeout, "requestAnimationFrame", 1);
    js_setglobal(J, "requestAnimationFrame");
    js_newcfunction(J, js_doc_addEventListener, "window.addEventListener", 2);
    js_setglobal(J, "addEventListener");
}

/* ── timers (setTimeout / setInterval): router table + engine refs ──────── */
static int js_push_timer_args(js_State *J, JsBridge *b, int top,
                              JsTimerKind kind)
{
    /* muJS c-function ABI: index 0 is `this`, arguments are 1..top-1. */
    if (top < 2 || !js_iscallable(J, 1))
    {
        js_error(J, "timer callback must be a function");
        return -1;
    }
    if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
    {
        js_error(J, "too many timers");
        return -1;
    }
    int delay = 0;
    if (top > 2 && !js_isundefined(J, 2) && !js_isnull(J, 2))
    {
        delay = js_tointeger(J, 2);
    }
    if (delay < 0)
    {
        delay = 0;
    }
    /* Pin the callback in the registry (muJS ref = registry key string).
     * The TABLE owns it: released via js_unref in clear_timer_ref/close. */
    js_copy(J, 1);
    const char *refKey = js_ref(J); /* rooted until js_unref */
    if (!refKey)
    {
        js_error(J, "timer ref failed");
        return -1;
    }
    int id = jsbridge_timer_start(b, kind, (void *)refKey, (unsigned)delay);
    if (id == 0)
    {
        js_unref(J, refKey); /* refusal: table full — release the pin */
        js_error(J, "too many timers");
        return -1;
    }
    js_pushnumber(J, (double)id); /* the handle pages clearTimeout with */
    return 0;
}

static void js_setTimeout(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    if (js_push_timer_args(J, b, js_gettop(J), JS_TIMER_TIMEOUT) != 0)
    {
        js_error(J, "setTimeout failed");
        return;
    }
}

static void js_setInterval(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    if (js_push_timer_args(J, b, js_gettop(J), JS_TIMER_INTERVAL) != 0)
    {
        js_error(J, "setInterval failed");
        return;
    }
}

static void js_clearTimeout(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int id = js_isundefined(J, 1) ? 0 : (int)js_tointeger(J, 1);
    js_pushnumber(J, (double)jsbridge_timer_clear(b, id));
}

static void js_clearInterval(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int id = js_isundefined(J, 1) ? 0 : (int)js_tointeger(J, 1);
    js_pushnumber(J, (double)jsbridge_timer_clear(b, id));
}

/* ── script execution ────────────────────────────────────────────────────── */
static void mujs_run_script(JsBridge *b, const char *src, size_t len, int index)
{
    js_State *J = (js_State *)b->implState;
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
    /* SW5: ES5 builtin compat prefix (Set/Map/Image) — self-guarding, so
     * engines with native builtins define nothing. One copy per script. */
    const char *prefix = NULL;
    size_t plen = jsbridge_sw5_prefix(&prefix);
    char *buf = (char *)JMalloc(plen + len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, src, len);
    buf[plen + len] = '\0';
    len += plen;
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
    mujs_define_xhr(J);
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

/* Timer vtable: release one pinned registry ref (slot already inert). */
static void mujs_clear_timer_ref(JsBridge *b, void *fnRef)
{
    js_State *J = (js_State *)b->implState;
    if (J && fnRef)
    {
        js_unref(J, (const char *)fnRef);
    }
}

/* Invoke one pinned callback. Returns 0 ok, 1 contained error, -1 abort. */
static int mujs_run_timer_ref(JsBridge *b, void *fnRef)
{
    js_State *J = (js_State *)b->implState;
    if (!J || !fnRef)
    {
        return 1;
    }
    js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);
    js_getregistry(J, (const char *)fnRef);
    js_pushnull(J); /* this */
    if (js_pcall(J, 0) != 0)
    {
        int fatal = !b->lastError[0];
        bridge_take_error(b, J);
        js_pop(J, 1);
        return fatal ? -1 : 1; /* one contained error; never a task crash */
    }
    js_pop(J, 1);
    return 0;
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
    /* Timer refs are released by the ROUTER via clear_timer_ref BEFORE
     * close (see js_doc_close) — nothing to unref here. */
    js_freestate(J);
    b->implState = NULL;
}

/* ── XMLHttpRequest (async HTTP → JS callbacks) ────────────────────────────
 * Router-owned request table (jsbridge.c); the engine side is thin glue on
 * the SAME patterns as everything else in this file:
 *   - the constructor builds a fresh wrapper userdata (tag "pluto.xhr")
 *     boxing the public request id (0 until open); works for `new
 *     XMLHttpRequest()` (muJS C-constructor contract: build + return) and
 *     the legacy bare `XMLHttpRequest()` call;
 *   - the userdata `has` hook PUSHES the live router state for
 *     readyState/status/responseText/response/responseURL (muJS getproperty
 *     contract — see jsR_hasproperty);
 *   - onload/onerror/onreadystatechange fall through to own properties and
 *     are PINNED at send (registry refs, table-owned until sweep/close);
 *     the router calls exactly ONE completion ref (onload → onerror →
 *     onreadystatechange precedence, chosen at pin time by which is set —
 *     onerror still fires for failures, onreadystatechange for both).
 * Caps + validation + byte budgets live entirely in the router. */
#define XHR_TAG "pluto.xhr"

static int xhr_has(js_State *J, void *p, const char *name);

static int xhr_has(js_State *J, void *p, const char *name)
{
    /* muJS hands the box data as `p` — hooks run during property lookups
     * where index 0 is NOT the object (a method get on a non-slot-0
     * wrapper previously threw "not a pluto.xhr" here on device). */
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int *box = (int *)p;
    const JsHttpRequest *r = jsbridge_xhr_get(b, box ? *box : 0);
    if (!strcmp(name, "readyState"))
    {
        js_pushnumber(J, r ? (double)r->state : 0.0);
        return 1;
    }
    if (!strcmp(name, "status"))
    {
        /* XHR spec: status reads 0 until the response settles. */
        js_pushnumber(J, (r && r->state >= JS_XHR_DONE) ? (double)r->status
                                                        : 0.0);
        return 1;
    }
    if (!strcmp(name, "responseText") || !strcmp(name, "response"))
    {
        js_pushstring(J, (r && r->body) ? r->body : "");
        return 1;
    }
    if (!strcmp(name, "responseURL"))
    {
        js_pushstring(J, (r && r->url[0]) ? r->url : "");
        return 1;
    }
    if (!strcmp(name, "withCredentials"))
    {
        js_pushboolean(J, 0);
        return 1;
    }
    return 0;
}

static int xhr_put(js_State *J, void *p, const char *name)
{
    (void)J;
    (void)p;
    (void)name;
    return 0; /* handler props land as own props, pinned at send */
}

static void js_xhr_open(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int *box = (int *)js_touserdata(J, 0, XHR_TAG);
    const char *method = js_tostring(J, 1);
    const char *url = js_tostring(J, 2);
    int id = jsbridge_xhr_open(b, method, url);
    if (id == 0)
    {
        js_error(J, "%s", b->lastError[0] ? b->lastError : "xhr open failed");
    }
    *box = id;
    js_pushundefined(J);
}

/* Pick the completion handler from the wrapper's own props and pin it.
 * Precedence: onload → onerror → onreadystatechange. Returns the pinned
 * registry key (ownership transfers to the router) or NULL. */
static const char *xhr_pin_completion(js_State *J)
{
    static const char *const names[] = {"onload", "onerror",
                                        "onreadystatechange"};
    for (int i = 0; i < 3; i++)
    {
        js_getproperty(J, 0, names[i]);
        if (!js_isundefined(J, -1) && !js_isnull(J, -1) && js_iscallable(J, -1))
        {
            js_copy(J, -1);
            js_pop(J, 1);
            return js_ref(J); /* pinned; table owns until sweep/close */
        }
        js_pop(J, 1);
    }
    return NULL;
}

static void js_xhr_send(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int *box = (int *)js_touserdata(J, 0, XHR_TAG);
    if (*box <= 0)
    {
        js_error(J, "xhr: send before open");
    }
    const char *fnRef = xhr_pin_completion(J);
    if (!fnRef)
    {
        js_error(J, "xhr: no onload/onerror/onreadystatechange handler");
    }
    js_copy(J, 0); /* pin the wrapper object as `this` for the completion */
    const char *objRef = js_ref(J);
    if (!objRef)
    {
        js_unref(J, fnRef);
        js_error(J, "xhr: pin failed");
    }
    jsbridge_xhr_send(b, *box, (void *)fnRef, (void *)objRef);
    js_pushundefined(J);
}

static void js_xhr_abort(js_State *J)
{
    JsBridge *b = (JsBridge *)js_getcontext(J);
    int *box = (int *)js_touserdata(J, 0, XHR_TAG);
    jsbridge_xhr_abort(b, box ? *box : 0);
    js_pushundefined(J);
}

/* Box finalizer: the wrapper's request-id box is SDK-allocated. */
static void xhr_box_finalize(js_State *J, void *p)
{
    (void)J;
    if (p)
    {
        JFree(p);
    }
}

/* Constructor: builds + RETURNS a fresh wrapper (muJS C-constructor
 * contract; `new XHR()` and bare XHR() behave identically here — the
 * prototype methods/accessors live on pluto.xhr.proto either way). */
static void js_xmlhttprequest_new(js_State *J)
{
    int *box = (int *)JMalloc(sizeof(int));
    if (!box)
    {
        js_error(J, "xhr: out of memory");
    }
    *box = 0;
    js_getregistry(J, "pluto.xhr.proto");
    js_newuserdatax(J, XHR_TAG, box, xhr_has, xhr_put, NULL,
                    xhr_box_finalize);
}

static void mujs_define_xhr(js_State *J)
{
    js_newobject(J); /* "pluto.xhr.proto" */
    {
        js_newcfunction(J, js_xhr_open, "open", 2);
        js_setproperty(J, -2, "open");
        js_newcfunction(J, js_xhr_send, "send", 0);
        js_setproperty(J, -2, "send");
        js_newcfunction(J, js_xhr_abort, "abort", 0);
        js_setproperty(J, -2, "abort");
    }
    js_setregistry(J, "pluto.xhr.proto");

    /* newcconstructor consumes the pushed prototype object (its rot2
     * idiom, same as jsB_initboolean) — push it first, on an empty
     * stack this underflows the value stack. */
    js_getregistry(J, "pluto.xhr.proto");
    js_newcconstructor(J, js_xmlhttprequest_new, js_xmlhttprequest_new,
                       "XMLHttpRequest", 0);
    js_setglobal(J, "XMLHttpRequest");
}

/* XHR vtable: release the pinned completion + wrapper refs (slot inert).
 * Both refs are registry keys (see xhr_pin_completion / js_xhr_send). */
static void mujs_clear_xhr_refs(JsBridge *b, void *fnRef, void *objRef)
{
    js_State *J = (js_State *)b->implState;
    if (!J)
    {
        return;
    }
    if (fnRef)
    {
        js_unref(J, (const char *)fnRef);
    }
    if (objRef)
    {
        js_unref(J, (const char *)objRef);
    }
}

/* Invoke the pinned completion: fn(responseText) with `this` = wrapper.
 * Returns 0 ok, 1 contained error, -1 engine abort. */
static int mujs_run_xhr_ref(JsBridge *b, void *fnRef, void *objRef,
                            const JsHttpRequest *r)
{
    js_State *J = (js_State *)b->implState;
    if (!J || !fnRef)
    {
        return 1;
    }
    js_setlimit(J, JSBRIDGE_RUNLIMIT, JSBRIDGE_MAXALLOC);
    js_getregistry(J, (const char *)fnRef);
    if (objRef)
    {
        js_getregistry(J, (const char *)objRef); /* this */
    }
    else
    {
        js_pushnull(J);
    }
    js_pushstring(J, (r && r->body) ? r->body : "");
    if (js_pcall(J, 1) != 0)
    {
        int fatal = !b->lastError[0];
        bridge_take_error(b, J);
        js_pop(J, 1);
        return fatal ? -1 : 1;
    }
    js_pop(J, 1);
    return 0;
}

const JsEngineImpl js_engine_mujs = {
    mujs_init,   mujs_run_script,     mujs_dispatch_click,
    mujs_clear_timer_ref, mujs_run_timer_ref,
    mujs_run_xhr_ref,     mujs_clear_xhr_refs, mujs_close,
    "muJS"};
