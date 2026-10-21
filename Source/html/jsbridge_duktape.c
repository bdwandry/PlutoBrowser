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
#include "../html/webstorage.h"

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
static void duktape_define_worker(duk_context *ctx, JsBridge *b);
static void duktape_define_formdata(duk_context *ctx, JsBridge *b);
static int duk_xhr_id(duk_context *ctx);
static void duk_push_worker(duk_context *ctx, JsBridge *b, int id);
static int duk_worker_postmessage(duk_context *ctx);
static int duk_worker_terminate(duk_context *ctx);


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

/* ── Forms-Phase 1: field life (checked / disabled / readOnly / value /
 * focus() / blur() roads) — the same surface the XS (No Recursion)
 * adapter grew. Frozen-engine culture: attr-store writes go straight to
 * the dom store (no domDirty lane here); live toggle state is read
 * through the node → DocBlock reverse lookup. */

static int duktape_dispatch_focus(JsBridge *b, DomNode *node,
                                  const char *type); /* forward: below */

/* Connectedness rule: the parent walk to the document root (a browser
 * only focuses connected elements). Local to this adapter; frozen
 * engines had no isConnected lane to factor from. */
static int dk_connected(JsBridge *b, DomNode *n)
{
    DomNode *root = (b->dom) ? b->dom->root : NULL;
    for (DomNode *p = n; p; p = p->parent)
    {
        if (p == root)
        {
            return 1;
        }
    }
    return 0;
}

/* Node → LIVE DocBlock reverse lookup (One block per node in the
 * standing build; DocBlock stamps srcNode on rendered controls). */
static DocBlock *dk_block_of_node(JsBridge *b, DomNode *n)
{
    if (!n || !b->doc)
    {
        return NULL;
    }
    for (int i = 0; i < b->doc->blockCount; i++)
    {
        DocBlock *blk = b->doc->blocks[i];
        if (blk && blk->srcNode == (void *)n)
        {
            return blk;
        }
    }
    return NULL;
}

/* The `value` property — browser shape: the attr is the store (the
 * form click/submit reporter and the typed-text store both live there);
 * a valueless <textarea> falls back to its DOM text content. */
/* Forms-Phase 2: select readers (defined after the checked roads — the
 * value accessor's select fork outranks them only via these decls). */
static int dk_select_index(JsBridge *b, DomNode *n);
static DomNode *dk_option_node_at(DomNode *sel, int idx);

static duk_ret_t duk_el_get_value(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *v = n ? dom_get_attr(n, "value") : NULL;
    if (n && n->kind == DOM_ELEMENT && n->tag &&
        strcmp(n->tag, "select") == 0)
    {
        /* Forms-Phase 2: the picker reporting road — the browser's
         * select.value is the selected option's shape (value attr, else
         * the option text), never the element's own attribute. */
        int idx = dk_select_index(b, n);
        if (idx >= 0 && idx != -2147483648)
        {
            DocBlock *blk = dk_block_of_node(b, n);
            if (blk && blk->options && idx < blk->optionCount &&
                blk->options[idx])
            {
                DocOption *o = blk->options[idx];
                duk_push_string(ctx, (o->value && o->value[0])
                                         ? o->value
                                         : (o->text ? o->text : ""));
                return 1;
            }
            DomNode *optn = dk_option_node_at(n, idx);
            if (optn)
            {
                const char *ov = dom_get_attr(optn, "value");
                if (ov && ov[0])
                {
                    duk_push_string(ctx, ov);
                    return 1;
                }
                char obuf[512];
                doc_concat_node_text(optn, obuf, sizeof(obuf));
                duk_push_string(ctx, obuf);
                return 1;
            }
        }
        duk_push_string(ctx, v ? v : "");
        return 1;
    }
    if (!v && n && n->tag && strcmp(n->tag, "textarea") == 0)
    {
        char buf[512];
        doc_concat_node_text(n, buf, sizeof(buf));
        duk_push_string(ctx, buf);
        return 1;
    }
    duk_push_string(ctx, v ? v : "");
    return 1;
}
static duk_ret_t duk_el_set_value(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        dom_set_attr(b->dom, n, "value", duk_to_string(ctx, 0));
    }
    return 0;
}

/* `checked` READ on the toggles: DocBlock.checked is the live state
 * (moves on tap; the markup attribute never does); the markup attribute
 * is only the pre-walk fallback. Non-toggles read false, like a
 * browser's non-form .checked. Getter-only in Phase 1 (writes + radio
 * exclusivity = Phase 2). */
static duk_ret_t duk_el_get_checked(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    int checked = 0;
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *tag = n->tag;
        const char *tv = dom_get_attr(n, "type");
        if (tag && strcmp(tag, "input") == 0 && tv &&
            (strcmp(tv, "checkbox") == 0 || strcmp(tv, "radio") == 0))
        {
            DocBlock *blk = dk_block_of_node(b, n);
            checked = blk ? blk->checked
                          : (dom_get_attr(n, "checked") != NULL);
        }
    }
    duk_push_boolean(ctx, checked ? 1 : 0);
    return 1;
}

/* Forms-Phase 2: `checked` WRITE — radio-group exclusivity, browser
 * semantics. Same live-store model as the XS NR lane: the write flips
 * DocBlock.checked (the store the tap lane, the live-drawn renderer and
 * the block-aware submit serializer all share); the markup attribute
 * stays the walk-time default (reset restores from it later). Setting a
 * radio true first unchecks every OTHER same-name radio (the tap lane's
 * doc-wide group); false simply unchecks. No events (a browser's typed
 * .checked assignment fires none). */
/* Forms-Phase 2: pre-walk `checked` attr mirror (the XS twin). Page
 * scripts run before the walker builds blocks; a write in that window
 * lands on the markup attribute so the walk carries the state, with
 * radio exclusivity removing OTHER same-name radios' `checked` attrs.
 * Depth-capped like the browser-true tree scans. */
static void dk_attr_uncheck_radios(DomNode *n, const char *name,
                                   DomNode *skip, int depth)
{
    if (!n || depth > 16)
    {
        return;
    }
    if (n->kind == DOM_ELEMENT && n->tag && strcmp(n->tag, "input") == 0 &&
        n != skip)
    {
        const char *tv = dom_get_attr(n, "type");
        if (tv && strcmp(tv, "radio") == 0)
        {
            const char *nm = dom_get_attr(n, "name");
            if (nm && name && strcmp(nm, name) == 0)
            {
                dom_remove_attr(n, "checked");
            }
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        dk_attr_uncheck_radios(n->children[i], name, skip, depth + 1);
    }
}

static duk_ret_t duk_el_set_checked(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (!n || n->kind != DOM_ELEMENT)
    {
        return 0;
    }
    const char *tag = n->tag;
    const char *tv = dom_get_attr(n, "type");
    if (!(tag && strcmp(tag, "input") == 0 && tv &&
          (strcmp(tv, "checkbox") == 0 || strcmp(tv, "radio") == 0)))
    {
        return 0;
    }
    DocBlock *blk = dk_block_of_node(b, n);
    int want = duk_to_boolean(ctx, 0);
    if (!blk)
    {
        /* Pre-walk window: attr mirror (see the XS twin). */
        const char *nm = dom_get_attr(n, "name");
        if (want)
        {
            if (strcmp(tv, "radio") == 0 && nm)
            {
                dk_attr_uncheck_radios(b->dom->root, nm, n, 0);
            }
            dom_set_attr(b->dom, n, "checked", "");
        }
        else
        {
            dom_remove_attr(n, "checked");
        }
        return 0;
    }
    if (want)
    {
        if (blk->radio)
        {
            for (int i = 0; i < b->doc->blockCount; i++)
            {
                DocBlock *sib = b->doc->blocks[i];
                if (sib && sib != blk && sib->radio && sib->name &&
                    blk->name && strcmp(sib->name, blk->name) == 0)
                {
                    sib->checked = 0;
                }
            }
        }
        blk->checked = 1;
    }
    else
    {
        blk->checked = 0;
    }
    return 0;
}

/* Forms-Phase 2: select.selectedIndex — the browser's 0-based read of
 * the LIVE pick. Block road first (tap-lane store); markup fallback =
 * the authored `selected` option, else the first option child.
 * INT_MIN-ish sentinel for "not a select": the caller outranks it. */
static int dk_select_index(JsBridge *b, DomNode *n)
{
    const char *tag = (n && n->kind == DOM_ELEMENT) ? n->tag : NULL;
    if (!tag || strcmp(tag, "select") != 0)
    {
        return -2147483648;
    }
    DocBlock *blk = dk_block_of_node(b, n);
    if (blk && blk->options && blk->optionCount > 0)
    {
        if (blk->multiple)
        {
            for (int k = 0; k < blk->optionCount; k++)
            {
                DocOption *o = blk->options[k];
                if (o && !o->group && o->selected)
                {
                    return k;
                }
            }
            return -1;
        }
        int si = blk->selectedIndex;
        return (si >= 1 && si <= blk->optionCount) ? si - 1 : -1;
    }
    int sel = -1, first = -1, k = 0;
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *c = n->children[i];
        if (!c || c->kind != DOM_ELEMENT || !c->tag ||
            strcmp(c->tag, "option") != 0)
        {
            continue;
        }
        if (first < 0)
        {
            first = k;
        }
        if (sel < 0 && dom_get_attr(c, "selected"))
        {
            sel = k;
        }
        k++;
    }
    return sel >= 0 ? sel : first;
}

/* nth direct <option> child (the fallback's option-node lookup). */
static DomNode *dk_option_node_at(DomNode *sel, int idx)
{
    int k = 0;
    for (int i = 0; i < sel->childCount; i++)
    {
        DomNode *c = sel->children[i];
        if (c && c->kind == DOM_ELEMENT && c->tag &&
            strcmp(c->tag, "option") == 0)
        {
            if (k == idx)
            {
                return c;
            }
            k++;
        }
    }
    return NULL;
}

static duk_ret_t duk_el_get_selectedIndex(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    int idx = dk_select_index(b, n);
    if (idx == -2147483648)
    {
        duk_push_undefined(ctx);
        return 1;
    }
    duk_push_int(ctx, idx);
    return 1;
}

/* select.options — every <option> element under the select (groups'
 * options included, group headers are not options). Fresh array. */
static duk_ret_t duk_el_get_options(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        strcmp(n->tag, "select") != 0)
    {
        duk_push_undefined(ctx);
        return 1;
    }
    duk_idx_t arr = duk_push_array(ctx);
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
            if (!strcmp(c->tag, "option"))
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
    return 1;
}

/* `disabled` / `readOnly` reflection — the REFLECTED property: own
 * attribute presence only (a disabled fieldset does NOT flip a child's
 * .disabled); writes go through the same dom store setAttribute uses. */
static duk_ret_t duk_el_get_disabled(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    const char *v = n ? dom_get_attr(n, "disabled") : NULL;
    duk_push_boolean(ctx, v ? 1 : 0);
    return 1;
}
static duk_ret_t duk_el_set_disabled(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        if (duk_to_boolean(ctx, 0))
        {
            dom_set_attr(b->dom, n, "disabled", "");
        }
        else
        {
            dom_remove_attr(n, "disabled");
        }
    }
    return 0;
}
static duk_ret_t duk_el_get_readOnly(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    const char *v = n ? dom_get_attr(n, "readonly") : NULL;
    duk_push_boolean(ctx, v ? 1 : 0);
    return 1;
}
static duk_ret_t duk_el_set_readOnly(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    BUDGET_OR_THROW(b);
    if (n && n->kind == DOM_ELEMENT)
    {
        if (duk_to_boolean(ctx, 0))
        {
            dom_set_attr(b->dom, n, "readonly", "");
        }
        else
        {
            dom_remove_attr(n, "readonly");
        }
    }
    return 0;
}

/* Form type — the XS twin's exact shape (Forms-Phase 7): the attr
 * verbatim, no case folding; a missing attr reads <input> "text",
 * <button> "submit"; other tags read undefined. Getter-only on every
 * engine (no IDL setter), like the accessor table's NULL setters. */
static duk_ret_t duk_el_get_type(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT)
    {
        duk_push_undefined(ctx);
        return 1;
    }
    const char *v = dom_get_attr(n, "type");
    if (v)
    {
        duk_push_string(ctx, v);
    }
    else if (n->tag && !strcmp(n->tag, "input"))
    {
        duk_push_string(ctx, "text");
    }
    else if (n->tag && !strcmp(n->tag, "button"))
    {
        duk_push_string(ctx, "submit");
    }
    else
    {
        duk_push_undefined(ctx);
    }
    return 1;
}

/* el.focus() / el.blur() roads — state + NON-bubbling event (the
 * keyboard lane stays native; the tap path is untouched). focus on the
 * already-focused element is a no-op, as is focusing a disconnected
 * element; blur when not focused is a no-op. Pointer-identity latch
 * only — never dereferenced. */
static duk_ret_t duk_el_focus(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT)
    {
        return 0;
    }
    if (!dk_connected(b, n) || b->focusNode == n)
    {
        return 0;
    }
    b->focusNode = n;
    (void)duktape_dispatch_focus(b, n, "focus");
    return 0;
}
static duk_ret_t duk_el_blur(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (!n || b->focusNode != n)
    {
        return 0;
    }
    b->focusNode = NULL;
    (void)duktape_dispatch_focus(b, n, "blur");
    return 0;
}

/* Forms-Phase 3: the constraint-validation surface — validity snapshot
 * object per access, validationMessage, willValidate, and the
 * checkValidity/reportValidity methods (invalid rides the non-bubbling
 * kind-6 road; no OS report surface exists here, so reportValidity's
 * "report" is the invalid delivery). */
static void duk_push_validity(duk_context *ctx, JsBridge *b, DomNode *n)
{
    JsbValidity v;
    memset(&v, 0, sizeof(v));
    if (n && n->kind == DOM_ELEMENT)
    {
        jsbridge_form_validity_eval(b, n, &v);
    }
    duk_push_object(ctx);
    duk_push_boolean(ctx, v.flags & JSBV_VALUEMISSING ? 1 : 0);
    duk_put_prop_string(ctx, -2, "valueMissing");
    duk_push_boolean(ctx, v.flags & JSBV_TYPEMISMATCH ? 1 : 0);
    duk_put_prop_string(ctx, -2, "typeMismatch");
    duk_push_boolean(ctx, v.flags & JSBV_PATTERNMISMATCH ? 1 : 0);
    duk_put_prop_string(ctx, -2, "patternMismatch");
    duk_push_boolean(ctx, v.flags & JSBV_TOOLONG ? 1 : 0);
    duk_put_prop_string(ctx, -2, "tooLong");
    duk_push_boolean(ctx, v.flags & JSBV_TOOSHORT ? 1 : 0);
    duk_put_prop_string(ctx, -2, "tooShort");
    duk_push_boolean(ctx, v.flags & JSBV_RANGEUNDERFLOW ? 1 : 0);
    duk_put_prop_string(ctx, -2, "rangeUnderflow");
    duk_push_boolean(ctx, v.flags & JSBV_RANGEOVERFLOW ? 1 : 0);
    duk_put_prop_string(ctx, -2, "rangeOverflow");
    duk_push_boolean(ctx, 0);
    duk_put_prop_string(ctx, -2, "stepMismatch");
    duk_push_boolean(ctx, v.flags & JSBV_BADINPUT ? 1 : 0);
    duk_put_prop_string(ctx, -2, "badInput");
    duk_push_boolean(ctx, 0);
    duk_put_prop_string(ctx, -2, "customError");
    duk_push_boolean(ctx, v.flags ? 0 : 1);
    duk_put_prop_string(ctx, -2, "valid");
}

static duk_ret_t duk_el_get_validity(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    duk_push_validity(ctx, b, this_node(ctx));
    return 1;
}

static duk_ret_t duk_el_get_validationMessage(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    char msg[96];
    msg[0] = '\0';
    if (n && n->kind == DOM_ELEMENT)
    {
        JsbValidity v;
        memset(&v, 0, sizeof(v));
        jsbridge_form_validity_eval(b, n, &v);
        snprintf(msg, sizeof(msg), "%s", v.message);
    }
    duk_push_string(ctx, msg);
    return 1;
}

static duk_ret_t duk_el_get_willValidate(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    int will = (n && n->kind == DOM_ELEMENT)
                   ? jsbridge_form_will_validate(b, n)
                   : 0;
    duk_push_boolean(ctx, will);
    return 1;
}

static duk_ret_t duk_el_checkValidity(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    int ok = 1;
    if (n && n->kind == DOM_ELEMENT)
    {
        JsbValidity v;
        memset(&v, 0, sizeof(v));
        jsbridge_form_validity_eval(b, n, &v);
        ok = (v.flags == 0);
        if (!ok)
        {
            (void)duktape_dispatch_focus(b, n, "invalid");
        }
    }
    duk_push_boolean(ctx, ok);
    return 1;
}

static duk_ret_t duk_el_reportValidity(duk_context *ctx)
{
    /* Same evaluate+deliver shake (device truth: no OS report UI). */
    return duk_el_checkValidity(ctx);
}

/* ── Forms-Phase 4: the HTMLFormElement API + HTMLElement.click() ───────── *
 * Same contract as the XS twin: prototype-level surface (tag-gated
 * inside), the chains ride the main.c host hooks, and the frozen
 * dispatcher dissolves event delivery (NONE falls through to the wire —
 * the standing boundary a page with no listeners matches). */
static DomNode *dk_form_receiver(DomNode *n)
{
    return (n && n->kind == DOM_ELEMENT && n->tag &&
            !strcmp(n->tag, "form"))
               ? n
               : NULL;
}

static DomNode *dk_form_ancestor(DomNode *n)
{
    for (const DomNode *p = n ? n->parent : NULL; p; p = p->parent)
    {
        if (p->kind == DOM_ELEMENT && p->tag && !strcmp(p->tag, "form"))
        {
            return (DomNode *)p;
        }
    }
    return NULL;
}

/* The el.click() activation classes: submit button/button-defaulted or
 * reset controls (the click's default action runs the form roads). */
static int dk_node_is_form_button(DomNode *n)
{
    if (!n || n->kind != DOM_ELEMENT || !n->tag)
    {
        return 0;
    }
    if (!strcmp(n->tag, "button"))
    {
        const char *t = dom_get_attr(n, "type");
        return (!t || !t[0] || !strcmp(t, "submit") || !strcmp(t, "reset"));
    }
    if (!strcmp(n->tag, "input"))
    {
        const char *t = dom_get_attr(n, "type");
        return t && (!strcmp(t, "submit") || !strcmp(t, "reset") ||
                     !strcmp(t, "image"));
    }
    return 0;
}

static duk_ret_t duk_form_get_elements(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (!dk_form_receiver(n))
    {
        duk_push_undefined(ctx);
        return 1;
    }
    void *buf[64]; /* scratch — 64 controls is far past any real page */
    int cnt = jsbridge_form_controls_collect(n, buf, 64);
    duk_idx_t arr = duk_push_array(ctx);
    for (int i = 0; i < cnt; i++)
    {
        duk_push_element(ctx, b, (DomNode *)buf[i]);
        duk_put_prop_index(ctx, arr, (duk_uarridx_t)i);
    }
    return 1;
}

static duk_ret_t duk_form_reset(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (dk_form_receiver(n) && dk_connected(b, n))
    {
        (void)jsbridge_form_host_reset(b, n); /* no events, ever */
    }
    return 0;
}

static duk_ret_t duk_form_submit(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (dk_form_receiver(n) && dk_connected(b, n))
    {
        /* form.submit(): no gate, no submit event — straight to the wire. */
        (void)jsbridge_form_host_wire(b, n, NULL, 0, 0);
    }
    return 0;
}

static duk_ret_t duk_form_requestSubmit(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (!dk_form_receiver(n) || !dk_connected(b, n))
    {
        return 0;
    }
    /* requestSubmit([submitter]): the argument must be a form
     * descendant (the spec's NotFoundError, contained as TypeError). */
    DomNode *submitter = NULL;
    if (!duk_is_undefined(ctx, 0) && !duk_is_null(ctx, 0))
    {
        submitter = arg_node(ctx, 0);
        if (!submitter || dk_form_ancestor(submitter) != n)
        {
            duk_error(ctx, DUK_ERR_TYPE_ERROR,
                      "requestSubmit: submitter is not this form's "
                      "descendant");
        }
    }
    (void)jsbridge_form_host_wire(b, n, submitter, 1, 1);
    return 0;
}

/* Forms-Phase 6: the form's enumerated IDL mirrors (get + set) — the
 * shared reflection helper covers the browser's limited enum shape
 * (missing/invalid → the GET / urlencoded default on READ; the setter
 * writes the lowercase value verbatim, exactly like setAttribute — this
 * engine's standing attribute-road note: no dirty-tag on attr writes).
 * Tag-gated: non-form receivers read undefined / absorb the write. */
static duk_ret_t duk_form_get_method(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    if (!n || !dk_form_receiver(n))
    {
        duk_push_undefined(ctx);
        return 1;
    }
    duk_push_string(ctx, jsbridge_form_method_idl(dom_get_attr(n, "method")));
    return 1;
}

static duk_ret_t duk_form_set_method(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (n && dk_form_receiver(n))
    {
        dom_set_attr(b->dom, n, "method", duk_to_string(ctx, 0));
    }
    return 0;
}

static duk_ret_t duk_form_get_enctype(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    if (!n || !dk_form_receiver(n))
    {
        duk_push_undefined(ctx);
        return 1;
    }
    duk_push_string(ctx,
                    jsbridge_form_enctype_idl(dom_get_attr(n, "enctype")));
    return 1;
}

static duk_ret_t duk_form_set_enctype(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (n && dk_form_receiver(n))
    {
        dom_set_attr(b->dom, n, "enctype", duk_to_string(ctx, 0));
    }
    return 0;
}

/* HTMLElement.click(): the dispatched click first (kind-0 — frozen
 * engines return NONE here and still activate, matching a user tap on
 * a script-less page); a non-prevented click on a form-associated
 * submit/reset button then runs the form's activation chain. */
static duk_ret_t duk_el_click(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT || !dk_connected(b, n))
    {
        return 0;
    }
    int rc = jsbridge_dispatch_element_event(b, n, "click");
    if (rc == JSB_EVENT_SUPPRESSED)
    {
        b->domDirty = 1; /* the click handler owns the page */
        return 0;
    }
    DomNode *form = dk_form_ancestor(n);
    if (form && dk_node_is_form_button(n))
    {
        (void)jsbridge_form_host_wire(b, form, n, 1, 1);
    }
    return 0;
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
    /* API-P6: the template's detached content fragment reports 11 —
     * made as an element for the machinery, but it IS the browser's
     * DocumentFragment surface. Parent-NULL is part of the rule (same
     * rule as the XS NR and muJS getters). */
    if (n && n->kind == DOM_ELEMENT && !n->parent && n->tag &&
        !strcmp(n->tag, "#template-content"))
    {
        duk_push_int(ctx, 11);
        return 1;
    }
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

/* API-P9: className accessor pair — the read is the class attribute
 * (never NULL; mapped 1:1 onto 'class', browser shape), the write goes
 * through the shared class road with the classList family shape (the
 * router owns the single budget charge; identical store to
 * setAttribute('class')). Closes the P6 parked gap on this engine. */
static duk_ret_t duk_el_get_className(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    const char *v = n ? dom_get_attr(n, "class") : NULL;
    duk_push_string(ctx, v ? v : "");
    return 1;
}
static duk_ret_t duk_el_set_className(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    if (n && n->kind == DOM_ELEMENT)
    {
        if (jsbridge_el_class_set(b, n, duk_to_string(ctx, 0)) != 0)
        {
            duk_error(ctx, DUK_ERR_ERROR, "className assignment failed");
        }
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
    /* API-P6: browsers return the appended child. */
    duk_push_element(ctx, b, c);
    return 1;
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
    /* API-P6: browsers return the removed child. */
    duk_push_element(ctx, b, c);
    return 1;
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
    /* API-P6: browsers return the inserted child. */
    duk_push_element(ctx, b, c);
    return 1;
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

/* ── API-P6: Element.closest — the element itself first, then element
 * ancestors; same parse + matcher as querySelector (the two can never
 * disagree). NULL result pushes null, per browsers. */
static duk_ret_t duk_el_closest(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    const char *sel = duk_to_string(ctx, 0);
    char scratch[JSBRIDGE_QS_SCRATCH];
    BUDGET_OR_THROW(b);
    duk_push_element(ctx, b,
                     (DomNode *)jsbridge_el_closest(b, n, sel, scratch,
                                                    sizeof(scratch)));
    return 1;
}

/* ── API-P6: template.content (getter) — the element's detached content
 * fragment; created templates materialize an EMPTY fragment on first
 * access. Non-template elements read undefined, like browsers. */
static duk_ret_t duk_el_get_content(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *n = this_node(ctx);
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        strcmp(n->tag, "template") != 0)
    {
        duk_push_undefined(ctx);
        return 1;
    }
    DomNode *frag = dom_template_content(b->dom, n);
    if (!frag)
    {
        duk_error(ctx, DUK_ERR_ERROR, "template content unavailable");
    }
    duk_push_element(ctx, b, frag);
    logger_log("[tpl] content -> node %d (children=%d)", frag->nodeId,
               frag->childCount);
    return 1;
}

/* ── API-P6: dataset (getter) — a fresh plain object per access mapping
 * data-* attributes to camelCase keys (read-through from the live
 * attribute list, same semantics as the XS engine's getter; direct
 * writes to the object do NOT write back — set attributes with
 * setAttribute like the browser's own pattern). */
static duk_ret_t duk_el_get_dataset(duk_context *ctx)
{
    DomNode *n = this_node(ctx);
    duk_idx_t obj = duk_push_object(ctx);
    if (!n)
    {
        return 1; /* empty object like the other engines' NULL path */
    }
    for (int i = 0; i < n->attrCount; i++)
    {
        const char *k = n->attrs[i].key;
        if (strncmp(k, "data-", 5) != 0)
        {
            continue;
        }
        char camel[64];
        int ci = 0;
        for (const char *p = k + 5; *p && ci + 1 < (int)sizeof(camel); p++)
        {
            if (*p == '-' && p[1])
            {
                p++;
                camel[ci++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
            }
            else
            {
                camel[ci++] = *p;
            }
        }
        camel[ci] = '\0';
        const char *v = dom_get_attr(n, k);
        duk_push_string(ctx, v ? v : "");
        duk_put_prop_string(ctx, obj, camel);
    }
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
    /* Forms-Phase 1: focus/blur join the accepted set as the kind-6
     * NON-bubbling class (delivered by the el.focus()/el.blur() roads
     * only); Phase 3 joins `invalid` (the constraint-validation event
     * doesn't bubble either). click stays the kind-0 bubbling class. */
    int kind = 0;
    if (type && (strcmp(type, "focus") == 0 || strcmp(type, "blur") == 0 ||
                 strcmp(type, "invalid") == 0))
    {
        kind = 6;
    }
    else if (!type || strcmp(type, "click") != 0)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so pages
         * registering them don't error — click (kind 0) and the focus/
         * blur roads (kind 6, Forms-Phase 1) are the delivered classes. */
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
    L->kind = kind; /* 0 = click class; 6 = focus/blur roads (see JsListener.kind) */
    /* Forms-Phase 7: store the event type — the XS twins write the same
     * string at registration (shared JsListener shape), and the kind-6
     * dispatch (focus/blur/invalid) routes listeners by exact type
     * match. The click dispatcher stays target-only (it never consults
     * `type`), so the standing click road is unchanged. */
    snprintf(L->type, sizeof(L->type), "%s", type);
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
    /* API-P9: the per-node wrapper cache — browser identity model (ONE
     * wrapper per node for the page's whole life, so strict `===` chains
     * and page-written expandos survive accesses; same contract XS NR
     * holds). Hit = push the stash-pinned heapptr; miss = build fresh
     * and pin below. See the JsElwEntry note in jsbridge_internal.h. */
    for (int i = b->elwCount - 1; i >= 0; i--)
    {
        if (b->elw[i].node == node && b->elw[i].ref)
        {
            duk_push_heapptr(ctx, b->elw[i].ref);
            return;
        }
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
        {"className", duk_el_get_className, duk_el_set_className},
        /* Forms-Phase 1+2: the field-life surface (value incl. the
         * textarea text road and the Phase-2 select fork; checked gains
         * the write side in Phase 2 — radio-group exclusivity rides the
         * same doc-wide group the tap lane uses, writes flip the LIVE
         * block, no attribute mirror; disabled/readOnly reflect their
         * own attributes). selectedIndex/options are the picker
         * reporting road (reads only this phase). */
        {"value", duk_el_get_value, duk_el_set_value},
        {"checked", duk_el_get_checked, duk_el_set_checked},
        {"selectedIndex", duk_el_get_selectedIndex, NULL},
        {"options", duk_el_get_options, NULL},
        {"disabled", duk_el_get_disabled, duk_el_set_disabled},
        {"readOnly", duk_el_get_readOnly, duk_el_set_readOnly},
        /* Forms-Phase 7: the control-type reflect (the XS twin's exact
         * shape — attr verbatim / input "text" / button "submit"; other
         * tags undefined). Getter-only everywhere. */
        {"type", duk_el_get_type, NULL},
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
        {"content", duk_el_get_content, NULL},
        {"dataset", duk_el_get_dataset, NULL},
        /* Forms-Phase 3: the constraint-validation getters. */
        {"validity", duk_el_get_validity, NULL},
        {"validationMessage", duk_el_get_validationMessage, NULL},
        {"willValidate", duk_el_get_willValidate, NULL},
        /* Forms-Phase 4: the form's controls collection (tag-gated
         * inside; non-forms read undefined). */
        {"elements", duk_form_get_elements, NULL},
        /* Forms-Phase 6: the form's enumerated IDL mirrors (the browser
         * reflect shapes; tagging the aliasing encoding → enctype). */
        {"method", duk_form_get_method, duk_form_set_method},
        {"enctype", duk_form_get_enctype, duk_form_set_enctype},
        {"encoding", duk_form_get_enctype, duk_form_set_enctype},
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
        {"closest", duk_el_closest, 1},
        {"addEventListener", duk_el_addEventListener, 2},
        /* Forms-Phase 1: the focus/blur roads (state + non-bubbling
         * event; keyboard lane untouched). */
        {"focus", duk_el_focus, 0},
        {"blur", duk_el_blur, 0},
        /* Forms-Phase 3: the validity check/report methods. */
        {"checkValidity", duk_el_checkValidity, 0},
        {"reportValidity", duk_el_reportValidity, 0},
        /* Forms-Phase 4: the form API + the click activation road
         * (tag-gated/no-op inside; the chains ride the host hooks). */
        {"reset", duk_form_reset, 0},
        {"submit", duk_form_submit, 0},
        {"requestSubmit", duk_form_requestSubmit, 1},
        {"click", duk_el_click, 0},
        {"getElementsByTagName", duk_el_getElementsByTagName, 1},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        duk_push_c_function(ctx, methods[i].fn, methods[i].nargs);
        duk_put_prop_string(ctx, obj, methods[i].name);
    }

    /* API-P9: pin the fresh wrapper in the global stash (GC-safe, the
     * listener-pin contract) and remember its stable heapptr — the ONLY
     * strong live ref; the heap dies with the engine at page close. */
    if (b->elwCount < JSBRIDGE_ELW_MAX)
    {
        char key[32];
        snprintf(key, sizeof(key), DUK_HIDDEN_SYMBOL("pluto.elw.%d"),
                 b->elwCount);
        duk_push_global_stash(ctx);
        duk_dup(ctx, -2); /* the fresh wrapper under the stash */
        duk_put_prop_string(ctx, -2, key);
        duk_pop(ctx); /* stash */
        b->elw[b->elwCount].node = node;
        b->elw[b->elwCount].ref = duk_get_heapptr(ctx, -1);
        b->elwCount++;
    }
    else if (!b->elwOverflow)
    {
        b->elwOverflow = 1;
        logger_log("[elw] duk wrapper cache full (%d) — fresh wrappers "
                   "from here on",
                   JSBRIDGE_ELW_MAX);
    }
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
    /* API-P6: road moved to the shared O(depth) scan (dom_find_by_id) —
     * the former push-all-children walk aborted whole once a body held
     * 65+ element children, hiding every later id from getElementById. */
    return dom_find_by_id(b->dom, id);
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

/* Forms-Phase 1: the NON-bubbling focus/blur dispatcher — the frozen-
 * engine twin of the click dispatcher above (one flat pass, no
 * listener-class mixing: kind 0 never fires here and vice versa).
 * focus/blur never bubble in browsers, so delivery
 * targets ONLY kind-6 listeners of the exact type on the node; the
 * event object carries bubbles:false + cancelable:false (neither is
 * cancelable per HTML; preventDefault rides along for shape parity but
 * has no effect). Returns JSB_EVENT_NAVIGATE when anything fired. */
static int duktape_dispatch_focus(JsBridge *b, DomNode *node,
                                  const char *type)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !node || !type)
    {
        return JSB_EVENT_NONE;
    }
    int fired = 0;
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        if (L->target != node || L->kind != 6 || /* focus/blur class */
            strcmp(L->type, type) != 0)          /* exact type route */
        {
            continue;
        }
        fired = 1;
        b->preventDef = 0;
        b->inClick = 1;

        duk_push_heapptr(ctx, L->ref);      /* pinned in the global stash */
        duk_push_element(ctx, b, node);     /* this = the focused node */
        duk_idx_t ev = duk_push_object(ctx);
        duk_push_string(ctx, type);
        duk_put_prop_string(ctx, ev, "type");
        duk_push_boolean(ctx, 0); /* bubbles:false — non-bubbling class */
        duk_put_prop_string(ctx, ev, "bubbles");
        duk_push_boolean(ctx, 0); /* cancelable:false — HTML truth */
        duk_put_prop_string(ctx, ev, "cancelable");
        duk_push_element(ctx, b, node);
        duk_put_prop_string(ctx, ev, "target");
        duk_push_element(ctx, b, node);
        duk_put_prop_string(ctx, ev, "currentTarget");
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
    }
    return fired ? JSB_EVENT_NAVIGATE : JSB_EVENT_NONE;
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

/* ── API-P7: localStorage / sessionStorage (see webstorage.h) ────────── */
#define WS_DUK_KIND_KEY "\xff" "wsKind" /* hidden per-instance store kind */

/* The store this call belongs to: local (kind 0) or session (kind 1). */
static WsStore *duk_storage_of(duk_context *ctx)
{
    WsStore *s = ws_local();
    duk_push_this(ctx);
    if (duk_is_object(ctx, -1))
    {
        duk_get_prop_string(ctx, -1, WS_DUK_KIND_KEY);
        if (duk_is_number(ctx, -1) && duk_get_int(ctx, -1) == 1)
        {
            s = ws_session();
        }
        duk_pop(ctx);
    }
    duk_pop(ctx);
    return s;
}

static duk_ret_t duk_storage_getItem(duk_context *ctx)
{
    WsStore *s = duk_storage_of(ctx);
    const char *k = duk_to_string(ctx, 0);
    char out[WS_VAL_MAX];
    int hit = s && ws_get(s, k, out, sizeof(out));
    if (!hit)
    {
        logger_log("[webstorage] get origin=%s key='%s' -> null", ws_origin(), k);
    }
    if (hit)
    {
        duk_push_string(ctx, out);
    }
    else
    {
        duk_push_null(ctx);
    }
    return 1;
}

static duk_ret_t duk_storage_setItem(duk_context *ctx)
{
    WsStore *s = duk_storage_of(ctx);
    const char *k = duk_to_string(ctx, 0);
    const char *v = duk_to_string(ctx, 1);
    if (!ws_set(s, k, v))
    {
        /* The browser's QuotaExceededError road (4KB here, not 5MB). */
        duk_error(ctx, DUK_ERR_ERROR,
                  "QuotaExceededError: per-origin storage quota exceeded");
    }
    return 0;
}

static duk_ret_t duk_storage_removeItem(duk_context *ctx)
{
    WsStore *s = duk_storage_of(ctx);
    ws_remove(s, duk_to_string(ctx, 0));
    return 0;
}

static duk_ret_t duk_storage_clear(duk_context *ctx)
{
    ws_clear(duk_storage_of(ctx));
    return 0;
}

static duk_ret_t duk_storage_key(duk_context *ctx)
{
    WsStore *s = duk_storage_of(ctx);
    char out[WS_KEY_MAX];
    if (ws_key(s, (int)duk_to_int(ctx, 0), out, sizeof(out)))
    {
        duk_push_string(ctx, out);
        return 1;
    }
    duk_push_null(ctx);
    return 1;
}

static duk_ret_t duk_storage_getLength(duk_context *ctx)
{
    duk_push_int(ctx, ws_count(duk_storage_of(ctx)));
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

    /* API-P7: localStorage + sessionStorage — the browser's key/value
     * stores driven by the shared webstorage.c road (same origin rule,
     * caps and persistence file as the other engines). Two instances,
     * methods on each; the kind (0=local / 1=session) rides the hidden
     * "\xffwsKind" property read through duk_push_this. length is a
     * live getter; getItem misses return null; setItem quota-fails
     * with the browser's QuotaExceededError road. */
    ws_bind(baseUrl ? baseUrl : "");
    for (int sk = 0; sk < 2; sk++)
    {
        duk_idx_t so = duk_push_object(ctx);
        put_method(ctx, so, "getItem", duk_storage_getItem, 1);
        put_method(ctx, so, "setItem", duk_storage_setItem, 2);
        put_method(ctx, so, "removeItem", duk_storage_removeItem, 1);
        put_method(ctx, so, "clear", duk_storage_clear, 0);
        put_method(ctx, so, "key", duk_storage_key, 1);
        duk_push_string(ctx, "length");
        duk_push_c_function(ctx, duk_storage_getLength, 0);
        duk_def_prop(ctx, so, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_SET_ENUMERABLE);
        duk_push_int(ctx, sk);
        duk_put_prop_lstring(ctx, so, "\xff" "wsKind", 7);
        if (sk)
        {
            duk_put_global_lstring(ctx, "sessionStorage", 14);
        }
        else
        {
            duk_put_global_lstring(ctx, "localStorage", 12);
        }
    }
    logger_log("[webstorage] globals bound (duktape): origin=%s", ws_origin());

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
    duktape_define_worker(ctx, b);
    duktape_define_formdata(ctx, b); /* Forms-Phase 5 */
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

/* API-P10: dynamic-import bridge native — delegates to the shared core
 * (jsbridge_dyn_request in jsbridge.c); status-string reply protocol
 * ("k:<key>" resolved / "p:<id>" ticket / "x:<msg>" refused). */
static duk_ret_t duk_dyn_request(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const char *spec = duk_to_string(ctx, 0);
    const char *base = duk_to_string(ctx, 1);
    char out[JSBRIDGE_MOD_KEY_MAX + 24];
    jsbridge_dyn_request(b, spec, base, out, sizeof(out));
    duk_push_string(ctx, out);
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

    /* API-P10: the dynamic-import bridge — one native over the shared
     * core; status-string reply protocol (see jsbridge_internal.h). */
    duk_push_c_function(ctx, duk_dyn_request, 2);
    duk_put_global_string(ctx, "__p5modRequest");
}

/* ── API-P8: Worker (the contained-stub lane; see the JsWorkerStub note
 * in jsbridge_internal.h) ─────────────────────────────────────────────
 * Same thin-glue contract as this bridge's XHR: wrapper = plain object,
 * the stub's public id rides a hidden pointer prop (0 = the documented
 * delivery-less cap shape); methods are per-instance; onerror/onmessage
 * materialize as null so a fresh stub reads `null` for both. The OBJECT
 * is pinned in the stash at construct (heapptr key pluto.wobj.<id>) and
 * the delivery reads the CURRENT onerror off it — browser at-fire
 * semantics. The worker SCRIPT never compiles or runs. */
#define DUK_WORKER_ID_PROP "\xFF" "pluto.wkid"

static void duk_push_worker(duk_context *ctx, JsBridge *b, int id)
{
    (void)b;
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_pointer(ctx, (void *)(intptr_t)id);
    duk_put_prop_string(ctx, obj, DUK_WORKER_ID_PROP);
    duk_push_null(ctx);
    duk_put_prop_string(ctx, obj, "onerror");
    duk_push_null(ctx);
    duk_put_prop_string(ctx, obj, "onmessage");
    duk_push_c_function(ctx, duk_worker_postmessage, 1);
    duk_put_prop_string(ctx, obj, "postMessage");
    duk_push_c_function(ctx, duk_worker_terminate, 0);
    duk_put_prop_string(ctx, obj, "terminate");
}

static int duk_worker_id(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_WORKER_ID_PROP);
    int id = (int)(intptr_t)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return id;
}

static int duk_worker_postmessage(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    jsbridge_worker_post(b, duk_worker_id(ctx));
    return 0;
}

static int duk_worker_terminate(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    jsbridge_worker_terminate(b, duk_worker_id(ctx));
    return 0;
}

/* Constructor global: new Worker(url) / Worker(url) → stub wrapper. */
static duk_ret_t duk_worker_new(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    const char *url = duk_to_string(ctx, 0); /* recorded, never resolved */
    int id = jsbridge_worker_open(b, url); /* 0 = delivery-less (cap) */
    duk_push_worker(ctx, b, id);
    if (id > 0)
    {
        /* Pin the wrapper object for the cross-frame delivery. */
        char key[40];
        snprintf(key, sizeof(key), DUK_HIDDEN_SYMBOL("pluto.wobj.%d"), id);
        void *hp = duk_get_heapptr(ctx, -1);
        duk_push_global_stash(ctx);
        duk_dup(ctx, -2);
        duk_put_prop_string(ctx, -2, key);
        duk_pop(ctx); /* stash */
        jsbridge_worker_pin(b, id, hp);
    }
    return 1;
}

static void duktape_define_worker(duk_context *ctx, JsBridge *b)
{
    (void)b;
    duk_push_c_function(ctx, duk_worker_new, 1);
    duk_push_object(ctx); /* prototype for instances */
    {
        duk_push_null(ctx);
        duk_put_prop_string(ctx, -2, "onerror");
        duk_push_null(ctx);
        duk_put_prop_string(ctx, -2, "onmessage");
    }
    duk_put_prop_string(ctx, -2, "prototype");
    duk_put_global_string(ctx, "Worker");
}

/* ── Forms-Phase 5: FormData (factory ctor + hidden pointer props) ──────
 * Same thin-glue contract as this bridge's XHR: instance = plain object
 * carrying the BRIDGE-side FormDataEntry address as a hidden pointer
 * prop (the pair lives page-long in bridge-owned memory — nothing to
 * free, browser lifetime semantics); methods are per-instance like
 * duk_push_xhr. Args: form at 0, submitter at 1 (duktape convention).
 * Iterators are their own factory (duktape lane of the shared pool). */
#define DUK_FD_PROP "\xFF" "pluto.fd"
#define DUK_FDI_PROP "\xFF" "pluto.fdi"

static int duk_formdata_append(duk_context *ctx);
static int duk_formdata_get(duk_context *ctx);
static int duk_formdata_getAll(duk_context *ctx);
static int duk_formdata_has(duk_context *ctx);
static int duk_formdata_set(duk_context *ctx);
static int duk_formdata_delete(duk_context *ctx);
static int duk_formdata_entries(duk_context *ctx);
static int duk_formdata_keys(duk_context *ctx);
static int duk_formdata_values(duk_context *ctx);
static int duk_formdata_foreach(duk_context *ctx);
static int duk_fditer_next(duk_context *ctx);

static FormDataEntry *duk_formdata_this(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_FD_PROP);
    FormDataEntry *e = (FormDataEntry *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    if (!e || !b || !e->used)
    {
        return NULL;
    }
    return e;
}

static int duk_formdata_new(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    DomNode *form = NULL, *sub = NULL;
    FormDataEntry *e;
    int err = 0;
    if (duk_get_top(ctx) >= 1 && !duk_is_null(ctx, 0) &&
        !duk_is_undefined(ctx, 0))
    {
        form = arg_node(ctx, 0);
    }
    if (form && duk_get_top(ctx) >= 2 && !duk_is_null(ctx, 1) &&
        !duk_is_undefined(ctx, 1))
    {
        sub = arg_node(ctx, 1);
    }
    e = jsbridge_form_data_construct(b, form, sub, &err);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  err == 1 ? "FormData: parameter is not of type "
                             "HTMLFormElement"
                  : err == 2 ? "FormData: submitter is not this form's "
                               "descendant"
                             : "FormData: too many FormData objects "
                               "this page");
    }
    duk_push_object(ctx);
    {
        static const struct
        {
            const char *name;
            duk_c_function fn;
            int arity;
        } methods[] = {
            {"append", duk_formdata_append, 2},
            {"get", duk_formdata_get, 1},
            {"getAll", duk_formdata_getAll, 1},
            {"has", duk_formdata_has, 1},
            {"set", duk_formdata_set, 2},
            {"delete", duk_formdata_delete, 1},
            {"entries", duk_formdata_entries, 0},
            {"keys", duk_formdata_keys, 0},
            {"values", duk_formdata_values, 0},
            {"forEach", duk_formdata_foreach, 2},
        };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        {
            duk_push_c_function(ctx, methods[i].fn, methods[i].arity);
            duk_put_prop_string(ctx, -2, methods[i].name);
        }
    }
    duk_push_pointer(ctx, (void *)e);
    duk_put_prop_string(ctx, -2, DUK_FD_PROP);
    return 1;
}

static int duk_formdata_append(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = duk_formdata_this(ctx);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    if (duk_get_top(ctx) < 2)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData.append: name and value required");
    }
    jsbridge_form_data_append(e, duk_to_string(ctx, 0),
                              duk_to_string(ctx, 1));
    return 0;
}

static int duk_formdata_get(duk_context *ctx)
{
    FormDataEntry *e = duk_formdata_this(ctx);
    const char *v;
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    if (duk_get_top(ctx) < 1)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR, "FormData.get: name required");
    }
    v = jsbridge_form_data_get(e, duk_to_string(ctx, 0));
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

static int duk_formdata_getAll(duk_context *ctx)
{
    FormDataEntry *e = duk_formdata_this(ctx);
    int hits = 0, arr;
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    const char *nm = duk_get_top(ctx) >= 1 ? duk_to_string(ctx, 0) : "";
    arr = duk_push_array(ctx);
    for (int i = 0; i < e->npairs; i++)
    {
        if (strcmp(e->pairs[i].name, nm) == 0)
        {
            duk_push_string(ctx, e->pairs[i].value);
            duk_put_prop_index(ctx, arr, hits++);
        }
    }
    return 1;
}

static int duk_formdata_has(duk_context *ctx)
{
    FormDataEntry *e = duk_formdata_this(ctx);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    duk_push_boolean(ctx, jsbridge_form_data_has(
                              e, duk_get_top(ctx) >= 1
                                     ? duk_to_string(ctx, 0)
                                     : ""));
    return 1;
}

static int duk_formdata_set(duk_context *ctx)
{
    FormDataEntry *e = duk_formdata_this(ctx);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    if (duk_get_top(ctx) < 2)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData.set: name and value required");
    }
    (void)jsbridge_form_data_set(e, duk_to_string(ctx, 0),
                                 duk_to_string(ctx, 1));
    return 0;
}

static int duk_formdata_delete(duk_context *ctx)
{
    FormDataEntry *e = duk_formdata_this(ctx);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    duk_push_boolean(ctx, jsbridge_form_data_delete(
                              e, duk_get_top(ctx) >= 1
                                     ? duk_to_string(ctx, 0)
                                     : ""));
    return 1;
}

static void duk_formdata_iter(duk_context *ctx, int mode)
{
    JsBridge *b = bridge_of(ctx);
    FormDataEntry *e = duk_formdata_this(ctx);
    int full = 0;
    FormDataIter *it;
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    it = jsbridge_form_data_iter(b, e, &full);
    if (!it)
    {
        duk_error(ctx, DUK_ERR_ERROR, full ? "FormData: too many "
                                     "iterators this page"
                                           : "FormData: iterator "
                                             "unavailable");
    }
    it->mode = (unsigned char)mode;
    duk_push_object(ctx);
    duk_push_c_function(ctx, duk_fditer_next, 0);
    duk_put_prop_string(ctx, -2, "next");
    duk_push_pointer(ctx, (void *)it);
    duk_put_prop_string(ctx, -2, DUK_FDI_PROP);
}

static int duk_formdata_entries(duk_context *ctx)
{
    duk_formdata_iter(ctx, 0);
    return 1;
}

static int duk_formdata_keys(duk_context *ctx)
{
    duk_formdata_iter(ctx, 1);
    return 1;
}

static int duk_formdata_values(duk_context *ctx)
{
    duk_formdata_iter(ctx, 2);
    return 1;
}

static int duk_fditer_next(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_FDI_PROP);
    FormDataIter *it = (FormDataIter *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    duk_push_object(ctx);
    if (it && it->entry && it->idx < it->entry->npairs)
    {
        const FdPair *p = &it->entry->pairs[it->idx++];
        if (it->mode == 1)
        {
            duk_push_string(ctx, p->name);
        }
        else if (it->mode == 2)
        {
            duk_push_string(ctx, p->value);
        }
        else
        {
            int arr = duk_push_array(ctx);
            duk_push_string(ctx, p->name);
            duk_put_prop_index(ctx, arr, 0);
            duk_push_string(ctx, p->value);
            duk_put_prop_index(ctx, arr, 1);
        }
        duk_put_prop_string(ctx, -2, "value");
        duk_push_boolean(ctx, 0);
        duk_put_prop_string(ctx, -2, "done");
    }
    else
    {
        duk_push_undefined(ctx);
        duk_put_prop_string(ctx, -2, "value");
        duk_push_boolean(ctx, 1);
        duk_put_prop_string(ctx, -2, "done");
    }
    return 1;
}

static int duk_formdata_foreach(duk_context *ctx)
{
    JsBridge *b = bridge_of(ctx);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = duk_formdata_this(ctx);
    if (!e)
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData: receiver is not a FormData");
    }
    if (duk_get_top(ctx) < 1 || !duk_is_callable(ctx, 0))
    {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "FormData.forEach: callback required");
    }
    for (int i = 0; i < e->npairs; i++)
    {
        const FdPair *p = &e->pairs[i];
        /* duk_pcall_method idiom: [fn][this][arg0][arg1] — the callback
         * gets (value, name), the browser's first two arguments. */
        duk_push_null(ctx);
        duk_copy(ctx, 0, -1); /* fn = the callback argument */
        if (duk_get_top(ctx) >= 2 && !duk_is_undefined(ctx, 1))
        {
            duk_copy(ctx, 1, -1); /* this = thisArg */
        }
        else
        {
            duk_push_undefined(ctx);
        }
        duk_push_string(ctx, p->value);
        duk_push_string(ctx, p->name);
        if (duk_pcall_method(ctx, 2) != DUK_EXEC_SUCCESS)
        {
            b->errs++;
            bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
            duk_pop(ctx); /* error object */
            break;        /* the exception stops forEach (browser) */
        }
        duk_pop(ctx); /* result */
    }
    return 0;
}

/* Constructor global: new FormData(form[, submitter]); the shared core
 * validates the form/submitter pair (TypeError family, contained). */
static void duktape_define_formdata(duk_context *ctx, JsBridge *b)
{
    (void)b;
    duk_push_c_function(ctx, duk_formdata_new, 1);
    duk_push_object(ctx); /* prototype for instanceof parity */
    duk_put_prop_string(ctx, -2, "prototype");
    duk_put_global_string(ctx, "FormData");
}

/* Worker vtable: delete the stub's stash pin whose heapptr matches. */
static void duktape_clear_worker_refs(JsBridge *b, void *objRef)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !objRef)
    {
        return;
    }
    duk_push_global_stash(ctx);
    duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 1))
    {
        const char *k = duk_get_string(ctx, -2);
        void *hp = NULL;
        char kb[40];
        if (k && strstr(k, "\xFF" "pluto.wobj."))
        {
            hp = duk_get_heapptr(ctx, -1);
            snprintf(kb, sizeof(kb), "%s", k); /* key dies with the slot */
        }
        duk_pop_2(ctx); /* key + value */
        if (hp && hp == objRef)
        {
            duk_del_prop_string(ctx, -2, kb); /* -2 = the stash */
            break;
        }
    }
    duk_pop(ctx); /* enum */
    duk_pop(ctx); /* stash */
}

/* Delivery: worker.onerror(ev) with `this` = the pinned wrapper and
 * ev = {message}. The CURRENT onerror is consulted at fire time; a stub
 * without a callable onerror delivers nothing. Same bracket + contained
 * error contract as duktape_run_xhr_ref. */
static int duktape_run_worker_ref(JsBridge *b, void *objRef,
                                  const JsWorkerStub *w)
{
    duk_context *ctx = (duk_context *)b->implState;
    if (!ctx || !objRef || !w)
    {
        return 1;
    }
    duk_push_heapptr(ctx, objRef);
    duk_get_prop_string(ctx, -1, "onerror");
    if (duk_is_callable(ctx, -1))
    {
        void *fnHp = duk_get_heapptr(ctx, -1);
        duk_pop_2(ctx); /* fn value + wrapper */
        if (fnHp)
        {
            duk_push_heapptr(ctx, fnHp);  /* method */
            duk_push_heapptr(ctx, objRef); /* this */
            duk_push_object(ctx);          /* ev */
            duk_push_string(ctx, w->message);
            duk_put_prop_string(ctx, -2, "message");
            if (duk_pcall_method(ctx, 1) != DUK_EXEC_SUCCESS)
            {
                int firstErr = !b->lastError[0];
                bridge_take_error_text(b, duk_safe_to_string(ctx, -1));
                duk_pop(ctx);
                return firstErr ? -1 : 1;
            }
            duk_pop(ctx); /* result */
            logger_log("[worker] onerror delivered #%d present=1", w->id);
        }
        else
        {
            logger_log("[worker] onerror delivered #%d present=1 "
                       "(non-heapptr callable — undelivered)", w->id);
        }
    }
    else
    {
        duk_pop_2(ctx); /* handler value + wrapper */
        logger_log("[worker] onerror delivered #%d present=0 (silent — "
                   "unhandled worker error)", w->id);
    }
    return 0;
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
    duktape_init,     duktape_run_script,    NULL, /* run_script_stream (R15: XS NR only) */
    duktape_dispatch_click,
    duktape_clear_timer_ref, duktape_run_timer_ref,
    duktape_run_xhr_ref,     duktape_clear_xhr_refs, duktape_close,
    NULL, /* pump (R20: time-sliced split is XS NR only) */
    NULL, /* eval_pending (R27: no background eval) */
    NULL, /* mount_parked (R28: never parks) */
    "Duktape",
    NULL, NULL, /* dispatch_event / script_queue_drained (frozen engine) */
    duktape_run_worker_ref, duktape_clear_worker_refs /* API-P8 */};
