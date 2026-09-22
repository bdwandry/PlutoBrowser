/*
 * PlutoBrowser — jsbridge_duktape.c
 * Duktape 2.7.0 engine implementation behind the JsEngineImpl vtable
 * (engine vendored STOCK under Source/js/duktape — never modified).
 *
 * Duktape facts this file relies on (verified against the 2.7.0 amalgamated
 * header):
 *   - duk_pcompile/duk_pcall return DUK_EXEC_SUCCESS or leave the error on
 *     the stack top (duk_safe_to_string renders it) — all errors contained.
 *   - duk_create_heap(alloc, realloc, free, udata, fatal) routes ALL engine
 *     memory through the SDK realloc; no libc malloc is used.
 *   - duk_c_functions receive `this` via duk_push_this; the bridge pointer
 *     comes from duk_get_heap_udata.
 *   - Objects can hold raw heap pointers as hidden ("\xFF"-prefixed) props
 *     (duk_push_heapptr/duk_get_heapptr). Listener functions are pinned by
 *     storing them in the global stash (duk_push_global_stash) — the stash
 *     reference keeps them alive across gc; destroying the heap at close
 *     frees everything (no per-listener unref needed).
 *   - duk_gc(ctx, 0) is the per-script sweep (mirrors the muJS bridge).
 *
 * DOM surface is identical to the muJS bridge (jsbridge_mujs.c): element
 * wrappers expose tagName/id/textContent/innerHTML/parentNode/
 * childElementCount/children/nodeType plus getAttribute/setAttribute/
 * removeAttribute/appendChild/removeChild/addEventListener/
 * getElementsByTagName; document exposes getElementById/createElement/
 * createTextNode/title/write/writeln/addEventListener/body; globals add
 * window/navigator/console/location/alert/confirm/prompt/timers.
 * Element property GETTERS/SETTERS are real accessor properties here, so
 * arbitrary script data still lands as plain own properties (muJS parity).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "duktape.h"
#include "../core/logger.h"
#include "../util/strbuf.h"
#include "../core/pluto_mem.h"
#include "../html/dom.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)

/* Hidden props on element wrapper objects: */
#define DUK_NODE_PROP "\xFF" "pluto.node" /* heapptr-free: raw DomNode* as pointer */
static int duk_xhr_open(duk_context *ctx);
static int duk_xhr_send(duk_context *ctx);
static int duk_xhr_abort(duk_context *ctx);
static void duk_push_xhr(duk_context *ctx, JsBridge *b);
static void duktape_define_xhr(duk_context *ctx, JsBridge *b);
static int duk_xhr_id(duk_context *ctx);


/* ── heap allocators: route engine memory through the SDK ────────────────── */
static void *duk_sdk_alloc(void *udata, duk_size_t size)
{
    (void)udata;
    return pluto_mem_realloc(NULL, (unsigned)size);
}
static void *duk_sdk_realloc(void *udata, void *ptr, duk_size_t size)
{
    (void)udata;
    return pluto_mem_realloc(ptr, (unsigned)size);
}
static void duk_sdk_free(void *udata, void *ptr)
{
    (void)udata;
    pluto_mem_realloc(ptr, 0);
}
static void duk_sdk_fatal(void *udata, const char *msg)
{
    /* Cold path: only fires on internal invariant violations. Halt like the
     * device abort shim would — do NOT return into the engine. */
    (void)udata;
    logger_log("[js] DUKTAPE FATAL: %s", msg ? msg : "(no message)");
#ifndef TARGET_PLAYDATE
    /* Host builds: make the invariant VISIBLE (the logger stub is silent). */
    fprintf(stderr, "DUKTAPE FATAL: %s\n", msg ? msg : "(no message)");
    fflush(stderr);
#endif
    for (;;)
    {
        /* halted */
    }
}

#define BUDGET_OR_THROW(b)                                        \
    do                                                            \
    {                                                             \
        if (!budget_take(b))                                      \
        {                                                         \
            duk_error((duk_context *)b->implState, DUK_ERR_ERROR, \
                      "script did too much");                     \
        }                                                         \
    } while (0)

#define DUK_BRIDGE_STASH_KEY "\xFF" "pluto.bridge"

static JsBridge *bridge_of(duk_context *ctx)
{
    /* The JsBridge pointer lives in the GLOBAL STASH (public API, persists
     * for the heap's lifetime — Duktape 2.7.0 exposes no heap-udata
     * getter). Pushed once by duktape_init. */
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, DUK_BRIDGE_STASH_KEY);
    JsBridge *b = (JsBridge *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return b;
}

/* ── element wrappers ──────────────────────────────────────────────────────
 * A plain object per wrapper: hidden prop holds the DomNode*, accessor
 * properties mirror the muJS `has` hook, C-function properties mirror the
 * prototype methods, and unknown writes become ordinary own properties. */

static DomNode *this_node(duk_context *ctx)
{
    duk_push_this(ctx);
    if (!duk_is_object(ctx, -1))
    {
        duk_pop(ctx);
        return NULL;
    }
    duk_get_prop_string(ctx, -1, DUK_NODE_PROP);
    DomNode *n = (DomNode *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return n;
}

static DomNode *arg_node(duk_context *ctx, duk_idx_t idx)
{
    duk_get_prop_string(ctx, idx, DUK_NODE_PROP);
    DomNode *n = (DomNode *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return n;
}

static void duk_push_element(duk_context *ctx, JsBridge *b, DomNode *node);

/* Accessor getters (this = element wrapper). */
static duk_ret_t duk_el_get_tagName(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT)
    {
        duk_push_undefined(ctx);
        return 1;
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
        duk_push_string(ctx, up);
    }
    else
    {
        duk_push_string(ctx, n->tag);
    }
    return 1;
}
static duk_ret_t duk_el_get_id(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    const char *v = n ? dom_get_attr(n, "id") : NULL;
    duk_push_string(ctx, v ? v : "");
    return 1;
}
static duk_ret_t duk_el_get_textContent(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    char buf[512];
    if (n)
    {
        doc_concat_node_text(n, buf, sizeof(buf));
        duk_push_string(ctx, buf);
    }
    else
    {
        duk_push_string(ctx, "");
    }
    return 1;
}
/* UNSUPPORTED (no subtree serializer): degrades to text — muJS parity. */
static duk_ret_t duk_el_get_innerHTML(duk_context *ctx)
{
    return duk_el_get_textContent(ctx);
}
static duk_ret_t duk_el_get_parentNode(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_push_element(ctx, b, n ? n->parent : NULL);
    return 1;
}
static duk_ret_t duk_el_get_childElementCount(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
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
    duk_push_int(ctx, c);
    return 1;
}
static duk_ret_t duk_el_get_children(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_idx_t arr = duk_push_array(ctx);
    int k = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                duk_push_element(ctx, b, n->children[i]);
                duk_put_prop_index(ctx, arr, (duk_uarridx_t)k++);
            }
        }
    }
    return 1;
}
static duk_ret_t duk_el_get_nodeType(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    duk_push_int(ctx, (n && n->kind == DOM_ELEMENT) ? 1 : 3);
    return 1;
}

/* Accessor setters (this = element wrapper; value on stack top). */
static duk_ret_t duk_el_set_id(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        dom_set_attr(b->dom, n, "id", duk_to_string(ctx, 0));
    }
    return 0;
}
static duk_ret_t duk_el_set_textContent(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        if (dom_set_text(b->dom, n, duk_to_string(ctx, 0)) != 0)
        {
            duk_error(ctx, DUK_ERR_ERROR, "textContent assignment failed");
        }
    }
    return 0;
}
/* SW5: REAL markup assignment through the router (was textContent). */
static duk_ret_t duk_el_set_innerHTML(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *s = duk_to_string(ctx, 0);
        if (jsbridge_el_set_inner_html(b, n, s, s ? strlen(s) : 0) != 0)
        {
            duk_error(ctx, DUK_ERR_ERROR, "innerHTML assignment failed");
        }
    }
    return 0;
}

/* Element methods. */
static duk_ret_t duk_el_getAttribute(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    const char *v = n ? dom_get_attr(n, duk_to_string(ctx, 0)) : NULL;
    if (v)
    {
        duk_push_string(ctx, v);
    }
    else
    {
        duk_push_null(ctx);
    }
    return 1;
}
static duk_ret_t duk_el_setAttribute(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *k = duk_to_string(ctx, 0);
    const char *v = duk_to_string(ctx, 1);
    BUDGET_OR_THROW(b);
    if (!n || dom_set_attr(b->dom, n, k, v) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "setAttribute failed");
    }
    return 0;
}
static duk_ret_t duk_el_removeAttribute(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    if (n)
    {
        dom_remove_attr(n, duk_to_string(ctx, 0));
    }
    return 0;
}
static duk_ret_t duk_el_appendChild(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    DomNode *c = arg_node(ctx, 0);
    BUDGET_OR_THROW(b);
    if (!n || !c || dom_append_child(b->dom, n, c) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "appendChild failed");
    }
    return 0;
}
static duk_ret_t duk_el_removeChild(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    DomNode *c = arg_node(ctx, 0);
    BUDGET_OR_THROW(b);
    if (!n || !c || dom_remove_child(n, c) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "removeChild failed");
    }
    return 0;
}
/* ── SW5: insertBefore / querySelector(All) (methods) ───────────────────── */
static duk_ret_t duk_el_insertBefore(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    DomNode *c = arg_node(ctx, 0);
    DomNode *ref = duk_is_null_or_undefined(ctx, 1) ? NULL : arg_node(ctx, 1);
    BUDGET_OR_THROW(b);
    if (!n || !c || dom_insert_before(b->dom, n, c, ref) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "insertBefore failed");
    }
    return 0;
}

/* querySelector emit: Duktape pushes wrappers into the array being built
 * at index `arr` (carried through the user data pointer). */
typedef struct
{
    duk_context *ctx;
    duk_idx_t arr;
    duk_uarridx_t k;
    JsBridge *b;
} DukQsEmit;

static int qs_emit_duktape(void *elp, void *ud)
{
    DukQsEmit *e = (DukQsEmit *)ud;
    duk_push_element(e->ctx, e->b, (DomNode *)elp);
    duk_put_prop_index(e->ctx, e->arr, e->k++);
    return 0;
}

static duk_ret_t duk_el_querySelectorAll(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *sel = duk_to_string(ctx, 0);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    duk_idx_t arr = duk_push_array(ctx);
    DukQsEmit e = {ctx, arr, 0, b};
    jsbridge_el_query_selector_all(b, n, sel, scratch, sizeof(scratch),
                                   qs_emit_duktape, &e);
    return 1;
}

static duk_ret_t duk_el_querySelector(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *sel = duk_to_string(ctx, 0);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    duk_push_element(ctx, b,
                     (DomNode *)jsbridge_el_query_selector_first(
                         b, n, sel, scratch, sizeof(scratch)));
    return 1;
}

/* ── SW5 (O4): classList object — fresh plain object per access whose
 * methods close over the DomNode via a hidden pointer prop. */
static DomNode *cl_node(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xFF" "cl.node");
    DomNode *n = (DomNode *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return n;
}

static duk_ret_t duk_cl_add(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    if (jsbridge_el_class_add(b, cl_node(ctx), duk_to_string(ctx, 0)) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "classList.add failed");
    }
    return 0;
}

static duk_ret_t duk_cl_remove(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    if (jsbridge_el_class_remove(b, cl_node(ctx), duk_to_string(ctx, 0)) != 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "classList.remove failed");
    }
    return 0;
}

static duk_ret_t duk_cl_toggle(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    int rc = jsbridge_el_class_toggle(b, cl_node(ctx), duk_to_string(ctx, 0));
    if (rc < 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "classList.toggle failed");
    }
    duk_push_boolean(ctx, rc == 1);
    return 1;
}

static duk_ret_t duk_cl_contains(duk_context *ctx)
{
    duk_push_boolean(ctx,
                     jsbridge_el_class_has(cl_node(ctx), duk_to_string(ctx, 0)));
    return 1;
}

static duk_ret_t duk_cl_item(duk_context *ctx)
{
    DomNode *n = cl_node(ctx);
    int idx = duk_is_undefined(ctx, 0) ? -1 : (int)duk_to_int(ctx, 0);
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
            duk_push_string(ctx, tok);
            return 1;
        }
        p += n2;
    }
    duk_push_null(ctx);
    return 1;
}

static duk_ret_t duk_cl_length(duk_context *ctx)
{
    DomNode *n = cl_node(ctx);
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
    duk_push_int(ctx, k);
    return 1;
}

/* Fresh classList object for `node`. */
static void put_method(duk_context *ctx, duk_idx_t obj, const char *name,
                       duk_c_function fn, duk_idx_t nargs); /* fwd: below */
static void duk_push_classlist(duk_context *ctx, JsBridge *b, DomNode *node)
{
    (void)b;
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_pointer(ctx, (void *)node);
    duk_put_prop_string(ctx, obj, "\xFF" "cl.node");
    put_method(ctx, obj, "add", duk_cl_add, 1);
    put_method(ctx, obj, "remove", duk_cl_remove, 1);
    put_method(ctx, obj, "toggle", duk_cl_toggle, 1);
    put_method(ctx, obj, "contains", duk_cl_contains, 1);
    put_method(ctx, obj, "item", duk_cl_item, 1);
    duk_push_string(ctx, "length");
    duk_push_c_function(ctx, duk_cl_length, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_SET_ENUMERABLE);
}

/* ── SW5: style object — hidden node pointer + has/put through a getter/
 * setter trap: Duktape has no generic property hook on plain objects, so
 * style objects expose the WALKER vocabulary as accessors (display/
 * visibility/textAlign/fontWeight/fontStyle/textDecoration + cssFloat)
 * backed by one shared style-string reader/writer. Legacy write patterns
 * for other property names are accepted no-ops (contained, no throw). */
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

/* Rewrite one property of the inline style attr, preserving others. */
static void style_write_prop(JsBridge *b, DomNode *n, const char *prop,
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
            return; /* overflow: drop the write, don't throw */
        }
    }
    dom_set_attr(b->dom, n, "style", buf);
}

static DomNode *style_node(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xFF" "st.node");
    DomNode *n = (DomNode *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return n;
}

/* Shared getter/setter bodies, switched by the lightfunc's magic index into
 * a property-name table. */
static const char *const STYLE_PROPS[] = {
    "display", "visibility", "text-align", "font-weight", "font-style",
    "text-decoration", "color", "background"};

static duk_ret_t duk_style_get(duk_context *ctx)
{
    int which = (int)duk_get_current_magic(ctx);
    char val[128];
    style_read_prop(style_node(ctx), STYLE_PROPS[which], val, sizeof(val));
    duk_push_string(ctx, val);
    return 1;
}

static duk_ret_t duk_style_set(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    int which = (int)duk_get_current_magic(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = style_node(ctx);
    if (n && n->kind == DOM_ELEMENT)
    {
        style_write_prop(b, n, STYLE_PROPS[which], duk_to_string(ctx, 0));
    }
    return 0;
}

static void duk_push_style(duk_context *ctx, JsBridge *b, DomNode *node)
{
    (void)b;
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_pointer(ctx, (void *)node);
    duk_put_prop_string(ctx, obj, "\xFF" "st.node");
    for (int i = 0; i < (int)(sizeof(STYLE_PROPS) / sizeof(STYLE_PROPS[0]));
         i++)
    {
        duk_push_string(ctx, STYLE_PROPS[i]); /* key */
        /* lightfuncs carry the magic switch compactly (8 accessors). */
        duk_push_c_lightfunc(ctx, duk_style_get, 0, 0, i);
        duk_push_c_lightfunc(ctx, duk_style_set, 1, 1, i);
        /* [obj, key, getter, setter] → target at -4; def pops all three. */
        duk_def_prop(ctx, -4,
                     DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                         DUK_DEFPROP_SET_ENUMERABLE);
    }
}

static duk_ret_t duk_el_addEventListener(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *type = duk_to_string(ctx, 0);
    if (!duk_is_callable(ctx, 1))
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR, "listener must be a function");
    }
    if (!type || strcmp(type, "click") != 0)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so pages
         * registering them don't error — only click is delivered. */
        return 0;
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        duk_error(ctx, DUK_ERR_ERROR, "too many event listeners");
    }
    /* Pin the function in the global stash (GC-safe) and remember its
     * stable heapptr; close() destroys the whole heap. */
    char key[32];
    snprintf(key, sizeof(key), DUK_HIDDEN_SYMBOL("pluto.lid.%d"),
             b->listenerCount);
    duk_push_global_stash(ctx);
    duk_dup(ctx, 1);
    duk_put_prop_string(ctx, -2, key);
    duk_pop(ctx);
    /* Capture the FUNCTION argument's heapptr (it is already pinned by the
     * global-stash put below, so the pointer stays valid for dispatch).
     * Capturing `this` here was the GC-lifetime bug: wrapper objects are
     * unreachable after registration and their heapptrs get freed. */
    void *fnHeapptr = duk_get_heapptr(ctx, 1);
    JsListener *L = &b->listeners[b->listenerCount++];
    L->ref = fnHeapptr;
    L->target = n;
    return 0;
}
static duk_ret_t duk_el_getElementsByTagName(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *tag = duk_to_string(ctx, 0);
    BUDGET_OR_THROW(b);
    duk_idx_t arr = duk_push_array(ctx);
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
                    duk_push_element(ctx, b, c);
                    duk_put_prop_index(ctx, arr, (duk_uarridx_t)k++);
                }
                if (top + 1 < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    return 1;
}

/* ── SW5 accessors: navigation + classList/style objects ────────────────── */
static duk_ret_t duk_el_get_firstElementChild(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_push_element(ctx, b, n ? dom_first_element_child(n) : NULL);
    return 1;
}
static duk_ret_t duk_el_get_nextElementSibling(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_push_element(ctx, b, n ? dom_next_element_sibling(n) : NULL);
    return 1;
}
static duk_ret_t duk_el_get_classList(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_push_classlist(ctx, b, n);
    return 1;
}
static duk_ret_t duk_el_get_style(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    duk_push_style(ctx, b, n);
    return 1;
}

/* Push a fresh wrapper object for `node` (NULL → null). */
static void duk_push_element(duk_context *ctx, JsBridge *b, DomNode *node)
{
    if (!node)
    {
        duk_push_null(ctx);
        return;
    }
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_pointer(ctx, (void *)node);
    duk_put_prop_string(ctx, obj, DUK_NODE_PROP);

    /* accessor properties (key at -2, accessor at -1 → obj at -3) */
    struct
    {
        const char *name;
        duk_c_function get;
        duk_c_function set; /* NULL = read-only */
    } accs[] = {
        {"tagName", duk_el_get_tagName, NULL},
        {"id", duk_el_get_id, duk_el_set_id},
        {"textContent", duk_el_get_textContent, duk_el_set_textContent},
        {"innerHTML", duk_el_get_innerHTML, duk_el_set_innerHTML},
        {"parentNode", duk_el_get_parentNode, NULL},
        {"childElementCount", duk_el_get_childElementCount, NULL},
        {"children", duk_el_get_children, NULL},
        {"nodeType", duk_el_get_nodeType, NULL},
        {"firstElementChild", duk_el_get_firstElementChild, NULL},
        {"nextElementSibling", duk_el_get_nextElementSibling, NULL},
        {"classList", duk_el_get_classList, NULL},
        {"style", duk_el_get_style, NULL},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        duk_push_string(ctx, accs[i].name);
        duk_push_c_function(ctx, accs[i].get, 0);
        if (accs[i].set)
        {
            duk_push_c_function(ctx, accs[i].set, 1);
        }
        else
        {
            duk_push_undefined(ctx);
        }
        /* Stack here: [obj, key, getter, setter] → obj_idx = -4; the call
         * consumes key + accessors, leaving [obj]. */
        duk_uint_t flags = DUK_DEFPROP_HAVE_GETTER |
                           DUK_DEFPROP_HAVE_SETTER |
                           DUK_DEFPROP_SET_ENUMERABLE;
        duk_def_prop(ctx, -4, flags);
    }

    /* methods */
    struct
    {
        const char *name;
        duk_c_function fn;
        duk_idx_t nargs;
    } methods[] = {
        {"getAttribute", duk_el_getAttribute, 1},
        {"setAttribute", duk_el_setAttribute, 2},
        {"removeAttribute", duk_el_removeAttribute, 1},
        {"appendChild", duk_el_appendChild, 1},
        {"removeChild", duk_el_removeChild, 1},
        {"insertBefore", duk_el_insertBefore, 2},
        {"querySelector", duk_el_querySelector, 1},
        {"querySelectorAll", duk_el_querySelectorAll, 1},
        {"addEventListener", duk_el_addEventListener, 2},
        {"getElementsByTagName", duk_el_getElementsByTagName, 1},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        duk_push_c_function(ctx, methods[i].fn, methods[i].nargs);
        duk_put_prop_string(ctx, obj, methods[i].name);
    }
    (void)b;
}

/* ── console ─────────────────────────────────────────────────────────────── */
static duk_ret_t duk_console_log(duk_context *ctx)
{
    char line[160];
    size_t off = 0;
    line[0] = '\0';
    int top = duk_get_top(ctx);
    for (int i = 0; i < top && off < sizeof(line) - 2; i++)
    {
        const char *s = duk_to_string(ctx, i);
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
    return 0;
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

static duk_ret_t duk_document_write(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    int top = duk_get_top(ctx);
    for (int i = 0; i < top; i++)
    {
        BUDGET_OR_THROW(b);
        dw_append(b, duk_to_string(ctx, i));
    }
    return 0;
}
static duk_ret_t duk_document_writeln(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    int top = duk_get_top(ctx);
    for (int i = 0; i < top; i++)
    {
        BUDGET_OR_THROW(b);
        dw_append(b, duk_to_string(ctx, i));
    }
    dw_append(b, "\n");
    return 0;
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

static duk_ret_t duk_document_getElementById(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const char *id = duk_to_string(ctx, 0);
    BUDGET_OR_THROW(b);
    duk_push_element(ctx, b, find_element_by_id(b, id));
    return 1;
}
static duk_ret_t duk_document_createElement(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *el = dom_create_element(b->dom, duk_to_string(ctx, 0));
    if (!el)
    {
        duk_error(ctx, DUK_ERR_ERROR, "createElement failed");
    }
    duk_push_element(ctx, b, el);
    return 1;
}
static duk_ret_t duk_document_createTextNode(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *t = dom_create_text(b->dom, duk_to_string(ctx, 0));
    if (!t)
    {
        duk_error(ctx, DUK_ERR_ERROR, "createTextNode failed");
    }
    duk_push_element(ctx, b, t);
    return 1;
}

/* document.querySelector(All): scoped at the document root (SW5). */
static duk_ret_t duk_document_querySelector(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const char *sel = duk_to_string(ctx, 0);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    DomNode *root = b->dom ? b->dom->root : NULL;
    duk_push_element(ctx, b,
                     (DomNode *)jsbridge_el_query_selector_first(
                         b, root, sel, scratch, sizeof(scratch)));
    return 1;
}

static duk_ret_t duk_document_querySelectorAll(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const char *sel = duk_to_string(ctx, 0);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    DomNode *root = b->dom ? b->dom->root : NULL;
    duk_idx_t arr = duk_push_array(ctx);
    DukQsEmit e = {ctx, arr, 0, b};
    jsbridge_el_query_selector_all(b, root, sel, scratch, sizeof(scratch),
                                   qs_emit_duktape, &e);
    return 1;
}
/* document.title getter: walk the live tree's <title> so scripts and the
 * chrome agree (doc->title is only filled by the walker, after scripts). */
static duk_ret_t duk_document_getTitle(duk_context *ctx)
{
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
                    duk_push_string(ctx, buf);
                    return 1;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    duk_push_string(ctx, b->doc->title[0] ? b->doc->title : "");
    return 1;
}
/* document/window.addEventListener: UNSUPPORTED delivery (no bubbling
 * model); accepted silently so common pages don't throw — muJS parity. */
static duk_ret_t duk_doc_addEventListener(duk_context *ctx)
{
    (void)ctx;
    return 0;
}
static duk_ret_t duk_location_assign(duk_context *ctx)
{
    logger_log("[js] location.assign/replace ignored: %s",
               duk_to_string(ctx, 0));
    return 0;
}
static duk_ret_t duk_alert(duk_context *ctx)
{
    logger_log("[js alert] %s", duk_to_string(ctx, 0));
    return 0;
}
static duk_ret_t duk_noop(duk_context *ctx)
{
    (void)ctx;
    return 0;
}

/* ── event object (click dispatch) ───────────────────────────────────────── */
static duk_ret_t duk_event_preventDefault(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    b->preventDef = 1;
    return 0;
}

/* Add a C method to the object at `obj`. */
static void put_method(duk_context *ctx, duk_idx_t obj, const char *name,
                       duk_c_function fn, duk_idx_t nargs)
{
    duk_push_c_function(ctx, fn, nargs);
    duk_put_prop_string(ctx, obj, name);
}

/* ── timers (setTimeout / setInterval): router table + engine refs ──────── */
/* Registration entry: [0]=fn [1]=delay. Registers in the ROUTER table and
 * returns the public id. Throws on refusal (table full / not a function). */
static duk_ret_t duk_timer_setup(duk_context *ctx, JsBridge *b,
                                 JsTimerKind kind)
{
    if (!duk_is_callable(ctx, 0))
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR, "timer callback must be a function");
    }
    if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
    {
        duk_error(ctx, DUK_ERR_ERROR, "too many timers");
    }
    int delay = 0;
    if (!duk_is_undefined(ctx, 1) && !duk_is_null(ctx, 1))
    {
        delay = (int)duk_to_number(ctx, 1);
    }
    if (delay < 0)
    {
        delay = 0;
    }
    /* Pin the FUNCTION in the global stash (GC-safe, same contract as
     * listeners) and remember its stable heapptr for dispatch. */
    char key[32];
    snprintf(key, sizeof(key), DUK_HIDDEN_SYMBOL("pluto.tid.%d"),
             b->timerIdSeq + 1); /* next id the router will hand out */
    duk_push_global_stash(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, key);
    duk_pop(ctx);
    void *fnHeapptr = duk_get_heapptr(ctx, 0);
    int id = jsbridge_timer_start(b, kind, fnHeapptr, (unsigned)delay);
    if (id == 0)
    {
        duk_push_global_stash(ctx);
        duk_del_prop_string(ctx, -1, key);
        duk_pop(ctx);
        duk_error(ctx, DUK_ERR_ERROR, "too many timers");
    }
    /* Verify the router handed out the id we keyed the stash with (it
     * must — registration is single-threaded and ids are sequential). */
    if (id != b->timerIdSeq)
    {
        /* Defensive: impossible today, but never leak a stash pin. */
        duk_push_global_stash(ctx);
        duk_del_prop_string(ctx, -1, key);
        duk_pop(ctx);
    }
    duk_push_number(ctx, (duk_double_t)id);
    return 1;
}

static duk_ret_t duk_setTimeout(duk_context *ctx)
{
    return duk_timer_setup(ctx, bridge_of(ctx), JS_TIMER_TIMEOUT);
}

static duk_ret_t duk_setInterval(duk_context *ctx)
{
    return duk_timer_setup(ctx, bridge_of(ctx), JS_TIMER_INTERVAL);
}

static duk_ret_t duk_clearTimeout(duk_context *ctx)
{
    int id = duk_is_undefined(ctx, 0) ? 0 : (int)duk_to_number(ctx, 0);
    duk_push_number(ctx, (duk_double_t)jsbridge_timer_clear(bridge_of(ctx), id));
    return 1;
}

static duk_ret_t duk_clearInterval(duk_context *ctx)
{
    int id = duk_is_undefined(ctx, 0) ? 0 : (int)duk_to_number(ctx, 0);
    duk_push_number(ctx, (duk_double_t)jsbridge_timer_clear(bridge_of(ctx), id));
    return 1;
}

static void define_globals(duk_context *ctx, JsBridge *b, const char *baseUrl)
{
    /* window: identity alias for the global object. */
    duk_push_global_object(ctx);
    duk_put_global_lstring(ctx, "window", 6);

    /* document */
    duk_idx_t doc = duk_push_object(ctx);
    put_method(ctx, doc, "getElementById", duk_document_getElementById, 1);
    put_method(ctx, doc, "createElement", duk_document_createElement, 1);
    put_method(ctx, doc, "createTextNode", duk_document_createTextNode, 1);
    put_method(ctx, doc, "querySelector", duk_document_querySelector, 1);
    put_method(ctx, doc, "querySelectorAll", duk_document_querySelectorAll, 1);
    duk_push_string(ctx, "title");
    duk_push_c_function(ctx, duk_document_getTitle, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_SET_ENUMERABLE);
    put_method(ctx, doc, "write", duk_document_write, 1);
    put_method(ctx, doc, "writeln", duk_document_writeln, 1);
    put_method(ctx, doc, "addEventListener", duk_doc_addEventListener, 2);
    DomNode *body = first_element(b->dom->root, "body");
    duk_push_element(ctx, b, body ? body : b->dom->root);
    duk_put_prop_string(ctx, doc, "body");
    duk_put_global_lstring(ctx, "document", 8);

    /* navigator */
    duk_idx_t nav = duk_push_object(ctx);
    duk_push_string(ctx, "PlutoBrowser/1.0 (Playdate; Duktape 2.7.0 ES5.1)");
    duk_put_prop_string(ctx, nav, "userAgent");
    duk_push_string(ctx, "PlutoBrowser");
    duk_put_prop_string(ctx, nav, "appCodeName");
    duk_push_string(ctx, "1.0");
    duk_put_prop_string(ctx, nav, "appVersion");
    duk_put_global_lstring(ctx, "navigator", 9);

    /* console */
    duk_idx_t con = duk_push_object(ctx);
    put_method(ctx, con, "log", duk_console_log, DUK_VARARGS);
    put_method(ctx, con, "warn", duk_console_log, DUK_VARARGS);
    put_method(ctx, con, "error", duk_console_log, DUK_VARARGS);
    duk_put_global_lstring(ctx, "console", 7);

    /* location */
    duk_idx_t loc = duk_push_object(ctx);
    duk_push_string(ctx, baseUrl ? baseUrl : "");
    duk_put_prop_string(ctx, loc, "href");
    duk_push_string(ctx, baseUrl ? baseUrl : "");
    duk_put_prop_string(ctx, loc, "host");
    put_method(ctx, loc, "assign", duk_location_assign, 1);
    put_method(ctx, loc, "replace", duk_location_assign, 1);
    duk_put_global_lstring(ctx, "location", 8);

    /* window/global methods (explicit global push: the stack is empty
     * after the put_global calls above). */
    duk_push_global_object(ctx);
    duk_idx_t glob = duk_get_top_index(ctx);
    put_method(ctx, glob, "alert", duk_alert, 1);
    put_method(ctx, glob, "confirm", duk_noop, 1);
    put_method(ctx, glob, "prompt", duk_noop, 1);
    put_method(ctx, glob, "setTimeout", duk_setTimeout, 2);
    put_method(ctx, glob, "setInterval", duk_setInterval, 2);
    put_method(ctx, glob, "clearTimeout", duk_clearTimeout, 1);
    put_method(ctx, glob, "clearInterval", duk_clearInterval, 1);
    put_method(ctx, glob, "requestAnimationFrame", duk_setTimeout, 1);
    put_method(ctx, glob, "addEventListener", duk_doc_addEventListener, 2);
    duk_pop(ctx);
}

/* ── vtable entry points ─────────────────────────────────────────────────── */
static int duktape_init(JsBridge *b, const char *baseUrl)
{
    duk_context *ctx = duk_create_heap(duk_sdk_alloc, duk_sdk_realloc,
                                       duk_sdk_free, b, duk_sdk_fatal);
    if (!ctx)
    {
        return -1;
    }
    b->implState = ctx;
    /* Publish the bridge pointer in the global stash for bridge_of(). */
    duk_push_global_stash(ctx);
    duk_push_pointer(ctx, (void *)b);
    duk_put_prop_string(ctx, -2, DUK_BRIDGE_STASH_KEY);
    duk_pop(ctx);
    define_globals(ctx, b, baseUrl);
    duktape_define_xhr(ctx, b);
    return 0;
}

static void duktape_run_script(JsBridge *b, const char *src, size_t len,
                               int index)
{
    duk_context *ctx = (duk_context *)b->implState;
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
    /* Same compile-safety gate as muJS (one bar for every engine). */
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
    /* SW5: ES5 builtin compat prefix (Set/Map/Image) — self-guarding. */
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

    b->ran++;
    duk_push_string(ctx, buf);     /* source */
    duk_push_string(ctx, "[page]"); /* filename — duk_pcompile consumes BOTH */
    if (duk_pcompile(ctx, 0) != DUK_EXEC_SUCCESS)
    {
        b->errs++;
        /* Error text included: distinguishes OOM ("out of memory"/
         * "internal error") from RangeError (recursion/limits) from real
         * syntax errors — decisive for device flip-crash diagnosis. */
        logger_log("[js] script %d failed to compile: %s", index,
                   duk_safe_to_string(ctx, -1));
        if (!b->lastError[0])
        {
            bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
        }
        duk_pop(ctx); /* error */
    }
    else
    {
        if (duk_pcall(ctx, 0) != DUK_EXEC_SUCCESS)
        {
            b->errs++;
            bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
        }
        duk_pop(ctx); /* result or error */
    }
    duk_gc(ctx, 0); /* per-script sweep, mirrors the muJS bridge */
    JFree(buf);
}

static int duktape_dispatch_click(JsBridge *b, const void *anchorNode)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx)
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

        duk_push_heapptr(ctx, L->ref); /* pinned in the global stash */
        duk_push_element(ctx, b, (DomNode *)L->target); /* this */
        /* event: { type:"click", preventDefault() } */
        duk_idx_t ev = duk_push_object(ctx);
        duk_push_string(ctx, "click");
        duk_put_prop_string(ctx, ev, "type");
        duk_push_c_function(ctx, duk_event_preventDefault, 0);
        duk_put_prop_string(ctx, ev, "preventDefault");

        if (duk_pcall_method(ctx, 1) != DUK_EXEC_SUCCESS)
        {
            b->errs++;
            bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
        }
        duk_pop(ctx); /* result or error */
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

/* Timer vtable: release one stash-pinned function (slot already inert). */
static void duktape_clear_timer_ref(JsBridge *b, void *fnRef)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !fnRef)
    {
        return;
    }
    /* Find + delete the stash entry whose heapptr matches. */
    duk_push_global_stash(ctx);
    duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 0))
    {
        const char *k = duk_get_string(ctx, -1);
        if (k && strstr(k, "\xFF" "pluto.tid."))
        {
            duk_get_prop(ctx, -2); /* stash[key] */
            void *hp = duk_get_heapptr(ctx, -1);
            duk_pop(ctx);
            if (hp == fnRef)
            {
                duk_del_prop(ctx, -2);
                break;
            }
        }
        duk_pop(ctx); /* key */
    }
    duk_pop_2(ctx); /* enum, stash */
}

/* Invoke one pinned callback. Returns 0 ok, 1 contained error, -1 abort. */
static int duktape_run_timer_ref(JsBridge *b, void *fnRef)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !fnRef)
    {
        return 1;
    }
    duk_push_heapptr(ctx, fnRef);
    duk_push_undefined(ctx); /* this */
    if (duk_pcall(ctx, 0) != DUK_EXEC_SUCCESS)
    {
        int firstErr = !b->lastError[0];
        bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
        duk_pop(ctx);
        return firstErr ? -1 : 1;
    }
    duk_pop(ctx);
    return 0;
}

static void duktape_close(JsBridge *b)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx)
    {
        return;
    }
    /* Safety net: sweep any timer stash pins the router did not release
     * (paranoia — js_doc_close clears the table before close). */
    duk_push_global_stash(ctx);
    duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 0))
    {
        const char *k = duk_get_string(ctx, -1);
        if (k && strstr(k, "\xFF" "pluto.tid."))
        {
            duk_del_prop(ctx, -2);
        }
        duk_pop(ctx);
    }
    duk_pop_2(ctx);
    /* Heap destruction frees every engine object — listener stash entries,
     * element wrappers and compiled code included (no per-listener unref). */
    duk_destroy_heap(ctx);
    b->implState = NULL;
}


/* ── XMLHttpRequest (async HTTP → JS callbacks) ────────────────────────────
 * Same thin-glue contract as the muJS bridge: wrapper object carries the
 * public request id as a pointer prop; live state reads go straight to the
 * router table; onload/onerror/onreadystatechange are pinned at send by
 * stashing them in the global stash (GC-safe) and remembering heapptrs.
 * Caps + validation + byte budgets live in the router (jsbridge.c). */
#define DUK_XHR_ID_PROP "\xFF" "pluto.xhrid"

/* Wrapper factory: fresh object + methods + request-id prop. */
static void duk_push_xhr(duk_context *ctx, JsBridge *b)
{
    (void)b;
    duk_idx_t obj = duk_push_object(ctx);
    /* request id lives as a pointer prop (0 = unopened); updated by open. */
    duk_push_pointer(ctx, (void *)0);
    duk_put_prop_string(ctx, obj, DUK_XHR_ID_PROP);

    duk_push_c_function(ctx, duk_xhr_open, 2);
    duk_put_prop_string(ctx, obj, "open");
    duk_push_c_function(ctx, duk_xhr_send, 0);
    duk_put_prop_string(ctx, obj, "send");
    duk_push_c_function(ctx, duk_xhr_abort, 0);
    duk_put_prop_string(ctx, obj, "abort");
}

static int duk_xhr_open(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    duk_push_this(ctx);
    const char *method = duk_to_string(ctx, 0);
    const char *url = duk_to_string(ctx, 1);
    int id = jsbridge_xhr_open(b, method, url);
    if (id == 0)
    {
        return duk_error(ctx, DUK_ERR_ERROR, "%s",
                         b->lastError[0] ? b->lastError : "xhr open failed");
    }
    duk_push_pointer(ctx, (void *)(intptr_t)id);
    duk_put_prop_string(ctx, -2, DUK_XHR_ID_PROP);
    return 0;
}

static int duk_xhr_id(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_XHR_ID_PROP);
    int id = (int)(intptr_t)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return id;
}

static int duk_xhr_send(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    int id = duk_xhr_id(ctx);
    if (id <= 0)
    {
        duk_error(ctx, DUK_ERR_ERROR, "xhr: send before open");
    }
    /* Pin the completion handler (onload → onerror → onreadystatechange
     * precedence) in the global stash and remember its heapptr. */
    static const char *const names[] = {"onload", "onerror",
                                        "onreadystatechange"};
    void *fnHeapptr = NULL;
    char key[40];
    snprintf(key, sizeof(key), DUK_HIDDEN_SYMBOL("pluto.xfn.%d"),
             b->xhrIdSeq + 1); /* next id the router hands out on start */
    for (int i = 0; i < 3 && !fnHeapptr; i++)
    {
        duk_push_this(ctx);
        if (duk_get_prop_string(ctx, -1, names[i]) && duk_is_callable(ctx, -1))
        {
            duk_push_global_stash(ctx);
            duk_dup(ctx, -2); /* the handler value */
            duk_put_prop_string(ctx, -2, key);
            duk_pop(ctx); /* stash */
            duk_dup(ctx, -2);
            fnHeapptr = duk_get_heapptr(ctx, -1);
            duk_pop(ctx); /* dup */
        }
        duk_pop_2(ctx); /* prop value + this */
    }
    if (!fnHeapptr)
    {
        duk_error(ctx, DUK_ERR_ERROR,
                  "xhr: no onload/onerror/onreadystatechange handler");
    }
    /* Pin the wrapper object as `this` for the completion. */
    char okey[40];
    snprintf(okey, sizeof(okey), DUK_HIDDEN_SYMBOL("pluto.xobj.%d"),
             b->xhrIdSeq + 1);
    duk_push_this(ctx);
    void *objHeapptr = duk_get_heapptr(ctx, -1);
    duk_push_global_stash(ctx);
    duk_dup(ctx, -2);
    duk_put_prop_string(ctx, -2, okey);
    duk_pop(ctx); /* stash */
    duk_pop(ctx); /* this */

    jsbridge_xhr_send(b, id, fnHeapptr, objHeapptr);
    return 0;
}

static int duk_xhr_abort(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    jsbridge_xhr_abort(b, duk_xhr_id(ctx));
    return 0;
}

/* Read-only live accessors (implemented as plain methods bound via
 * defineProperty on the prototype below — Duktape wrappers here are
 * per-instance, so the accessors are attached in duk_push_xhr instead). */
static int duk_xhr_get_readyState(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const JsHttpRequest *r = jsbridge_xhr_get(b, duk_xhr_id(ctx));
    duk_push_int(ctx, r ? r->state : 0);
    return 1;
}
static int duk_xhr_get_status(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const JsHttpRequest *r = jsbridge_xhr_get(b, duk_xhr_id(ctx));
    duk_push_int(ctx, (r && r->state >= JS_XHR_DONE) ? r->status : 0);
    return 1;
}
static int duk_xhr_get_responseText(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const JsHttpRequest *r = jsbridge_xhr_get(b, duk_xhr_id(ctx));
    duk_push_string(ctx, (r && r->body) ? r->body : "");
    return 1;
}
static int duk_xhr_get_responseURL(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const JsHttpRequest *r = jsbridge_xhr_get(b, duk_xhr_id(ctx));
    duk_push_string(ctx, (r && r->url[0]) ? r->url : "");
    return 1;
}

/* Constructor global: new XMLHttpRequest() / XHR() both return a wrapper. */
static duk_ret_t duk_xmlhttprequest_new(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    duk_push_xhr(ctx, b);
    return 1;
}

static void duktape_define_xhr(duk_context *ctx, JsBridge *b)
{
    duk_push_c_function(ctx, duk_xmlhttprequest_new, 0);
    duk_push_object(ctx); /* prototype for instances */
    {
        duk_push_c_function(ctx, duk_xhr_open, 2);
        duk_put_prop_string(ctx, -2, "open");
        duk_push_c_function(ctx, duk_xhr_send, 0);
        duk_put_prop_string(ctx, -2, "send");
        duk_push_c_function(ctx, duk_xhr_abort, 0);
        duk_put_prop_string(ctx, -2, "abort");
        struct
        {
            const char *name;
            duk_c_function get;
        } accs[] = {
            {"readyState", duk_xhr_get_readyState},
            {"status", duk_xhr_get_status},
            {"responseText", duk_xhr_get_responseText},
            {"response", duk_xhr_get_responseText},
            {"responseURL", duk_xhr_get_responseURL},
        };
        for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
        {
            duk_push_string(ctx, accs[i].name);
            duk_push_c_function(ctx, accs[i].get, 0);
            duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER |
                                      DUK_DEFPROP_SET_ENUMERABLE);
        }
    }
    duk_put_prop_string(ctx, -2, "prototype");
    duk_put_global_string(ctx, "XMLHttpRequest");
}

/* XHR vtable: delete the stash pins whose heapptrs match. */
static void duktape_clear_xhr_refs(JsBridge *b, void *fnRef, void *objRef)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx)
    {
        return;
    }
    duk_push_global_stash(ctx);
    duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 0))
    {
        const char *k = duk_get_string(ctx, -1);
        if (k && (strstr(k, "\xFF" "pluto.xfn.") || strstr(k, "\xFF" "pluto.xobj.")))
        {
            duk_get_prop(ctx, -2); /* stash[key] */
            void *hp = duk_get_heapptr(ctx, -1);
            duk_pop(ctx);
            if (hp == fnRef || hp == objRef)
            {
                duk_del_prop(ctx, -2);
            }
        }
        duk_pop(ctx); /* key */
        if (!fnRef && !objRef)
        {
            break;
        }
    }
    duk_pop(ctx); /* enum */
    duk_pop(ctx); /* stash */
}

/* Invoke the pinned completion: fn(responseText), this = wrapper. */
static int duktape_run_xhr_ref(JsBridge *b, void *fnRef, void *objRef,
                               const JsHttpRequest *r)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !fnRef)
    {
        return 1;
    }
    duk_push_heapptr(ctx, fnRef);
    if (objRef)
    {
        duk_push_heapptr(ctx, objRef); /* this */
    }
    else
    {
        duk_push_undefined(ctx);
    }
    duk_push_string(ctx, (r && r->body) ? r->body : "");
    if (duk_pcall_method(ctx, 1) != DUK_EXEC_SUCCESS)
    {
        int firstErr = !b->lastError[0];
        bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
        duk_pop(ctx);
        return firstErr ? -1 : 1;
    }
    duk_pop(ctx);
    return 0;
}

const JsEngineImpl js_engine_duktape = {
    duktape_init,     duktape_run_script,    duktape_dispatch_click,
    duktape_clear_timer_ref, duktape_run_timer_ref,
    duktape_run_xhr_ref,     duktape_clear_xhr_refs, duktape_close,
    "Duktape"};
