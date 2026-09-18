/*
 * PlutoBrowser — jsbridge_xs.c
 * XS (Moddable) 9.5.0 engine implementation behind the JsEngineImpl
 * vtable (engine vendored STOCK under Source/js/xs_moddable — never
 * modified; compiled per-file via the Makefile with our
 * Source/html/xs_platform.h routed through XS's own XSPLATFORM hook).
 *
 * XS embedding facts this file relies on (all verified against the
 * stock 9.5.0 sources/includes):
 *   - xsCreateMachine(&creation, name, context) allocates one machine;
 *     xsDeleteMachine frees everything the page allocated. One machine
 *     per page render, like the other bridges. xsGetContext carries
 *     the JsBridge*.
 *   - Host code brackets engine calls: xsBeginHostExit/xsEndHostExit
 *     (sets exitStatus = xsNormalExit (-1) on entry; the else-branch on
 *     unwind does NOT re-call fxAbort — the plain xsBeginHost does,
 *     which would loop forever with our fxAbort = longjmp). Inside the
 *     bracket, the macro-local `the` enables the xs* API macros.
 *   - Scripts run exactly like the stock tool host: fxParseScript(the,
 *     &txStringCStream, fxStringCGetter, mxProgramFlag) then fxRunScript
 *     (the, script, mxThis, C_NULL, C_NULL, C_NULL, mxProgram.value.reference).
 *   - Script throws unwind to the enclosing xsTry; unhandled aborts
 *     unwind to the xsBeginHostExit. Both are contained and mirrored to
 *     b->lastError; the browser task never dies.
 *   - Engine aborts (OOM, C-stack overflow, metering limit, unhandled
 *     rejection, "no more keys", …) call fxAbort. The stock default
 *     printf()s and exits the PROCESS, so xs_platform.h sets
 *     mxUseDefaultAbort=0 and this file defines fxAbort: record the
 *     status + fxExitToHost (the stock ESP32 device platform's
 *     MODDEF_XS_ABORT_EXITTOHOST pattern). fxExitToHost longjmps to the
 *     OUTERMOST jump, which is always one of our xsBeginHostExit
 *     brackets → contained.
 *   - mxMetering: xsBeginMetering(machine, cb, step) bounds any script;
 *     the callback receives the bytecode-op count and returning 0
 *     aborts with XS_TOO_MUCH_COMPUTATION_EXIT (the muJS run-limit
 *     analogue; it also meters the parser and RegExp compiler).
 *   - C-stack safety: mxUseDefaultCStackLimit=0 → the HOST provides
 *     fxCStackLimit(); the engine's fxCheckCStack and the parser/
 *     RegExp parsers compare against it and abort with
 *     XS_NATIVE_STACK_OVERFLOW_EXIT before blowing the 61.8KB
 *     game-task stack on device.
 *   - Element wrappers: ONE prototype carries the accessors/methods
 *     (fxNextHostAccessorProperty + fxNextHostFunctionProperty);
 *     instances are created with fxNewHostInstance(the) while the
 *     prototype sits on the machine stack (it clones the prototype's
 *     trailing internal host slot), then get the DomNode* via
 *     fxSetHostData. Wrappers are rebuilt on demand (never cached), so
 *     GC never needs to trace them.
 *   - Listener functions are pinned with xsRemember (roots the C-side
 *     xsSlot against GC) and xsForget at close.
 *   - fxRunLoop is TOOL code (xs/tools/xst.c) — this file provides the
 *     equivalent microtask drain: fxEndJob + fxRunPromiseJobs loop,
 *     then fxCheckUnhandledRejections(machine, 1).
 *
 * DOM surface is identical to jsbridge_mujs.c / jsbridge_duktape.c /
 * jsbridge_quickjs.c (same accessors, methods, budget + write capture).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "../core/logger.h"
#include "../util/strbuf.h"

/* Stock engine API — the exact include set of the canonical host tool
 * (xs/tools/xst.c): xsAll.h (internals, pulls xsPlatform.h → our
 * Source/html/xs_platform.h via -DINCLUDE_XSPLATFORM), then the public
 * xs.h macros. */
#include "../js/xs_moddable/sources/xsAll.h"
#include "../js/xs_moddable/sources/xsScript.h"
#include "../js/xs_moddable/includes/xs.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_pd()->system->realloc(NULL, (n))
#define JFree(p) pluto_pd()->system->realloc((p), 0)

/* ── machine sizing (Playdate-sized; slots ≈ 32B, chunks are bytes) ──── */
#define XS_INIT_CHUNK (384u * 1024u)
#define XS_INC_CHUNK (192u * 1024u)
#define XS_INIT_HEAP 8192u
#define XS_INC_HEAP 2048u
#define XS_STACK_COUNT 1024u
#define XS_INIT_KEYS 2048u
#define XS_INC_KEYS 256u
#define XS_NAME_MOD 127u
#define XS_SYMBOL_MOD 127u
#define XS_PARSER_BUF (16u * 1024u)
#define XS_PARSER_TBL 127u

/* Metering step (fxBeginMetering's txU8 interval; the engine checks every
 * interval << 16 bytecode units — one op = XS_CODE_METERING = 1<<16, so a
 * step of 8 = a check every 8 bytecode ops). Override with -DXS_METER_STEP=. */
#ifndef XS_METER_STEP
#define XS_METER_STEP 8
#endif

/* Run-limit: the meter callback receives the executed bytecode-op count
 * (meterIndex >> 16; one XS "meter op" = XS_CODE_METERING = 1<<16 raw
 * units) and returns 0 once it crosses XS_RUNLIMIT — same scale as the
 * muJS JSBRIDGE_RUNLIMIT statement counter (2,000,000). */
#ifndef XS_RUNLIMIT
#define XS_RUNLIMIT 2000000u
#endif

/* C-stack budget handed to the engine's stock fxCheckCStack + parser
 * guards. Device game-task stack is 61.8KB; keep the margin the QuickJS
 * bridge uses. Override via -DXS_CSTACK_LIMIT=. */
#ifdef TARGET_PLAYDATE
#define XS_CSTACK_LIMIT_DFL (36u * 1024u)
#else
#define XS_CSTACK_LIMIT_DFL (64u * 1024u)
#endif
#ifndef XS_CSTACK_LIMIT
#define XS_CSTACK_LIMIT XS_CSTACK_LIMIT_DFL
#endif

static const xsCreation xs_creation = {
    XS_INIT_CHUNK,  /* initialChunkSize */
    XS_INC_CHUNK,   /* incrementalChunkSize */
    XS_INIT_HEAP,   /* initialHeapCount */
    XS_INC_HEAP,    /* incrementalHeapCount */
    XS_STACK_COUNT, /* stackCount (slots) */
    XS_INIT_KEYS,   /* initialKeyCount */
    XS_INC_KEYS,    /* incrementalKeyCount */
    XS_NAME_MOD,    /* nameModulo */
    XS_SYMBOL_MOD,  /* symbolModulo */
    XS_PARSER_BUF,  /* parserBufferSize */
    XS_PARSER_TBL,  /* parserTableModulo */
};

/* Engine-private state (b->implState). */
typedef struct
{
    xsMachine *machine;
    xsSlot elProto; /* element prototype (rooted) */
    /* Pinned listener functions; JsListener.ref holds the index.
     * xsRemember'd against GC; xsForget + release at close. */
    xsSlot fns[JSBRIDGE_LISTENERS_MAX];
} XsState;

/* ── host-provided platform functions (mxUseDefault* = 0) ───────────── */

/* fxAbort: every fatal engine condition lands here (OOM, C-stack, meter
 * limit, unhandled rejection, …). Instead of the stock fprintf+exit
 * (would kill the browser), record and unwind to the OUTERMOST host
 * bracket — the stock ESP32 device platform's ABORT_EXITTOHOST pattern. */
void fxAbort(txMachine *the, int status)
{
    if (the->exitStatus == xsNormalExit)
    {
        the->exitStatus = status;
    }
    logger_log("[xs] abort: %s", fxAbortString(status));
    fxExitToHost(the);
}

/* fxCStackLimit: host-provided because xs_platform.h sets
 * mxUseDefaultCStackLimit=0 (the stock default returns C_NULL on bare
 * ARM, which disables the check entirely). The engine compares the
 * current stack pointer against this (downward growth) in fxCheckCStack,
 * the JS parser and the RegExp parser. Called per machine/parser init on
 * whichever task is running, so one formula serves sim + device. */
char *fxCStackLimit(void)
{
    static char *base = NULL;
    char probe;
    (void)probe;
    if (!base)
    {
        base = &probe;
    }
    return base - XS_CSTACK_LIMIT;
}

/* fxQueuePromiseJobs: no scheduler to wake (we drain synchronously). */
void fxQueuePromiseJobs(txMachine *the)
{
    (void)the;
}

/* fxCreateMachinePlatform / fxDeleteMachinePlatform: the stock default
 * platform struct is just `void* host;` — nothing to do. */
void fxCreateMachinePlatform(txMachine *the)
{
    (void)the;
}

void fxDeleteMachinePlatform(txMachine *the)
{
    (void)the;
}

/* ── bridge <-> machine plumbing ─────────────────────────────────────── */

static JsBridge *bridge_of(xsMachine *the)
{
    return (JsBridge *)xsGetContext(the);
}

static XsState *state_of(JsBridge *b)
{
    return (XsState *)b->implState;
}

static xsMachine *machine_of(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    return st ? st->machine : NULL;
}

/* DOM-mutation budget, XS flavor: throws (contained) when exhausted. */
#define BUDGET_OR_THROW(b)                      \
    do                                          \
    {                                           \
        if (!budget_take(b))                    \
        {                                       \
            xsTypeError("script did too much"); \
        }                                       \
    } while (0)

/* Copy an engine string out to the SDK heap (survives GC + next calls). */
static char *to_cstring(xsMachine *the, xsSlot slot)
{
    /* xsToString materializes into engine storage; copy before reuse. */
    char *s = xsToString(slot);
    size_t n = strlen(s) + 1;
    char *out = (char *)JMalloc(n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

/* ── element wrappers ────────────────────────────────────────────────── */

static xsSlot xs_push_element(JsBridge *b, DomNode *node);

/* Host data is the DomNode*; xsGetHostDataIf returns NULL for non-host
 * slots (this = undefined/string/number is legal JS on wrappers' props). */
static DomNode *this_node(xsMachine *the)
{
    return (DomNode *)xsGetHostDataIf(xsThis);
}

static DomNode *arg_node(xsMachine *the, xsIntegerValue i)
{
    return (DomNode *)xsGetHostDataIf(xsArg(i));
}

/* Element prototype: built once at init (see xs_build_el_proto), accessors
 * + methods live there; instances inherit through the prototype chain. */
static void xs_build_el_proto(JsBridge *b);

/* ── accessors (this = element wrapper) ──────────────────────────────── */

static void xs_el_get_tagName(xsMachine *the)
{
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        xsResult = xsString("");
        return;
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
        xsResult = xsString(up);
    }
    else
    {
        xsResult = xsString(n->tag);
    }
}

static void xs_el_get_id(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *v = n ? dom_get_attr(n, "id") : NULL;
    xsResult = xsString(v ? v : "");
}

static void xs_el_get_textContent(xsMachine *the)
{
    DomNode *n = this_node(the);
    char buf[512];
    if (n)
    {
        doc_concat_node_text(n, buf, sizeof(buf));
        xsResult = xsString(buf);
    }
    else
    {
        xsResult = xsString("");
    }
}

/* UNSUPPORTED (no subtree serializer): degrades to text — engine parity. */
static void xs_el_get_innerHTML(xsMachine *the)
{
    xs_el_get_textContent(the);
}

static void xs_el_get_parentNode(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the), n ? n->parent : NULL);
}

static void xs_el_get_childElementCount(xsMachine *the)
{
    DomNode *n = this_node(the);
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
    xsResult = xsInteger(c);
}

static void xs_el_get_children(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    xsResult = xsNewArray(0);
    int k = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                xsSlot el = xs_push_element(b, n->children[i]);
                xsSetAt(xsResult, xsInteger(k++), el);
            }
        }
    }
}

static void xs_el_get_nodeType(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsInteger((n && n->kind == DOM_ELEMENT) ? 1 : 3);
}

static void xs_el_set_id(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
        int rc = dom_set_attr(b->dom, n, "id", v);
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("id assignment failed");
        }
    }
}

static void xs_el_set_textContent(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
        int rc = dom_set_text(b->dom, n, v);
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("textContent assignment failed");
        }
    }
}

/* PARTIAL: treated as textContent (no markup parsing here) — parity. */
static void xs_el_set_innerHTML(xsMachine *the)
{
    xs_el_set_textContent(the);
}

/* ── element methods ─────────────────────────────────────────────────── */

static void xs_el_getAttribute(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    const char *v = n ? dom_get_attr(n, k) : NULL;
    JFree(k);
    xsResult = v ? xsString(v) : xsNull;
}

static void xs_el_setAttribute(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    char *v = to_cstring(the, xsArg(1));
    if (!k || !v)
    {
        if (k)
            JFree(k);
        if (v)
            JFree(v);
        xsTypeError("out of memory");
    }
    BUDGET_OR_THROW(b);
    int rc = (!n) ? -1 : dom_set_attr(b->dom, n, k, v);
    JFree(k);
    JFree(v);
    if (rc != 0)
    {
        xsTypeError("setAttribute failed");
    }
}

static void xs_el_removeAttribute(xsMachine *the)
{
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    if (n)
    {
        dom_remove_attr(n, k);
    }
    JFree(k);
}

static void xs_el_appendChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    if (!n || !c || dom_append_child(b->dom, n, c) != 0)
    {
        xsTypeError("appendChild failed");
    }
}

static void xs_el_removeChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    if (!n || !c || dom_remove_child(n, c) != 0)
    {
        xsTypeError("removeChild failed");
    }
}

static void xs_el_addEventListener(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    DomNode *n = this_node(the);
    char *type = to_cstring(the, xsArg(0));
    if (!type)
    {
        xsTypeError("out of memory");
    }
    int isClick = (strcmp(type, "click") == 0);
    JFree(type);
    if (!isClick)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so
         * pages registering them don't error — only click is delivered. */
        return;
    }
    xsSlot fn = xsArg(1);
    if (!fxIsCallable(the, &fn))
    {
        xsTypeError("listener must be a function");
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        xsTypeError("too many event listeners");
    }
    int idx = b->listenerCount++;
    st->fns[idx] = fn;
    xsRemember(st->fns[idx]); /* pin against GC */
    JsListener *L = &b->listeners[idx];
    L->ref = (void *)(intptr_t)idx;
    L->target = n;
}

static void xs_el_getElementsByTagName(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *tag = to_cstring(the, xsArg(0));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
    xsResult = xsNewArray(0);
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
                    xsSlot el = xs_push_element(b, c);
                    xsSetAt(xsResult, xsInteger(k++), el);
                }
                if (top + 1 < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    JFree(tag);
}

/* Push a fresh wrapper for `node` (NULL → null). The prototype is a
 * rooted host object; xsNewHostInstance clones it (inheriting its
 * internal host slot for the instance's data) and we store the DomNode*
 * as host data. Wrappers are never cached → nothing for GC to trace. */
static xsSlot xs_push_element(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b); /* value macros below need `the` */
    if (!node)
    {
        return xsNull;
    }
    xsSlot obj = xsNewHostInstance(state_of(b)->elProto);
    xsSetHostData(obj, node);
    return obj;
}

/* ── console ─────────────────────────────────────────────────────────── */
static void xs_console_log(xsMachine *the)
{
    char line[160];
    size_t off = 0;
    line[0] = '\0';
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc && off < sizeof(line) - 2; i++)
    {
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
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
        JFree(s);
    }
    logger_log("[js] %s", line);
}

/* ── document.write capture (engine-agnostic router buffer) ─────────── */
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

static void xs_document_write(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
        dw_append(b, s);
        JFree(s);
    }
}

static void xs_document_writeln(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
        dw_append(b, s);
        JFree(s);
    }
    dw_append(b, "\n");
}

/* ── document object ─────────────────────────────────────────────────── */

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

static void xs_document_getElementById(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *id = to_cstring(the, xsArg(0));
    if (!id)
    {
        xsTypeError("out of memory");
    }
    DomNode *n = find_element_by_id(b, id);
    JFree(id);
    xsResult = xs_push_element(b, n);
}

static void xs_document_createElement(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *tag = to_cstring(the, xsArg(0));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
    DomNode *el = dom_create_element(b->dom, tag);
    JFree(tag);
    if (!el)
    {
        xsTypeError("createElement failed");
    }
    xsResult = xs_push_element(b, el);
}

static void xs_document_createTextNode(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *txt = to_cstring(the, xsArg(0));
    if (!txt)
    {
        xsTypeError("out of memory");
    }
    DomNode *t = dom_create_text(b->dom, txt);
    JFree(txt);
    if (!t)
    {
        xsTypeError("createTextNode failed");
    }
    xsResult = xs_push_element(b, t);
}

/* document.title: walk the live tree's <title> so scripts and the chrome
 * agree (doc->title is only filled by the walker, after scripts). */
static void xs_document_getTitle(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
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
                    xsResult = xsString(buf);
                    return;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    xsResult = xsString(b->doc->title[0] ? b->doc->title : "");
}

/* document/window.addEventListener: UNSUPPORTED delivery (no bubbling
 * model); accepted silently so common pages don't throw — parity. */
static void xs_doc_addEventListener(xsMachine *the)
{
    (void)the;
}

static void xs_location_assign(xsMachine *the)
{
    char *u = to_cstring(the, xsArg(0));
    logger_log("[js] location.assign/replace ignored: %s", u ? u : "");
    if (u)
    {
        JFree(u);
    }
}

static void xs_alert(xsMachine *the)
{
    char *s = to_cstring(the, xsArg(0));
    logger_log("[js alert] %s", s ? s : "");
    if (s)
    {
        JFree(s);
    }
}

static void xs_noop(xsMachine *the)
{
    (void)the;
}

/* ── event object (click dispatch) ───────────────────────────────────── */
static void xs_event_preventDefault(xsMachine *the)
{
    bridge_of(the)->preventDef = 1;
}

/* ── prototype + globals (runs inside xsBeginHostExit at init) ───────── */

/* Helper: define a data property `name` = function `cb` on `obj`. */
static void def_fn(xsMachine *the, xsSlot obj, const char *name,
                   xsCallback cb, int length)
{
    xsSlot fn = xsNewHostFunction(cb, length);
    xsDefine(obj, xsID(name), fn, xsDefault);
}

/* Helper: define a plain data property. */
static void def_val(xsMachine *the, xsSlot obj, const char *name,
                    xsSlot value)
{
    (void)the;
    xsDefine(obj, xsID(name), value, xsDefault);
}

/* Element prototype: a host object carrying ONE internal host slot (the
 * "prototype holder" slot; clones inherit a fresh one for their data) +
 * the accessors/methods. Rooted via xsRemember for the machine's life. */
static void xs_build_el_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);

    st->elProto = xsNewHostObject(NULL);

    static const struct
    {
        const char *name;
        xsCallback get;
        xsCallback set;
    } accs[] = {
        {"tagName", xs_el_get_tagName, NULL},
        {"id", xs_el_get_id, xs_el_set_id},
        {"textContent", xs_el_get_textContent, xs_el_set_textContent},
        {"innerHTML", xs_el_get_innerHTML, xs_el_set_innerHTML},
        {"parentNode", xs_el_get_parentNode, NULL},
        {"childElementCount", xs_el_get_childElementCount, NULL},
        {"children", xs_el_get_children, NULL},
        {"nodeType", xs_el_get_nodeType, NULL},
    };
    static const struct
    {
        const char *name;
        xsCallback fn;
        int length;
    } methods[] = {
        {"getAttribute", xs_el_getAttribute, 1},
        {"setAttribute", xs_el_setAttribute, 2},
        {"removeAttribute", xs_el_removeAttribute, 1},
        {"appendChild", xs_el_appendChild, 1},
        {"removeChild", xs_el_removeChild, 1},
        {"addEventListener", xs_el_addEventListener, 2},
        {"getElementsByTagName", xs_el_getElementsByTagName, 1},
    };

    xsSlot proto = st->elProto;
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        if (accs[i].set)
        {
            xsSlot setter = xsNewHostFunction(accs[i].set, 1);
            xsDefine(proto, xsID(accs[i].name), getter, xsIsGetter);
            xsDefine(proto, xsID(accs[i].name), setter, xsIsSetter);
        }
        else
        {
            xsDefine(proto, xsID(accs[i].name), getter, xsIsGetter);
        }
    }
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        def_fn(the, proto, methods[i].name, methods[i].fn, methods[i].length);
    }

    xsRemember(st->elProto); /* root for the machine's lifetime */
}

static void xs_build_globals(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    xsSlot glob = xsGlobal;

    /* window === globalThis */
    xsDefine(glob, xsID("window"), glob, xsDefault);

    /* console.log only — parity with the other bridges */
    {
        xsSlot con = xsNewHostObject(NULL);
        def_fn(the, con, "log", xs_console_log, 0);
        def_val(the, glob, "console", con);
    }

    /* navigator */
    {
        xsSlot nav = xsNewHostObject(NULL);
        def_val(the, nav, "userAgent",
                xsString("PlutoBrowser/1.0 (Playdate; XS/Moddable 9.5.0)"));
        def_val(the, nav, "appCodeName", xsString("PlutoBrowser"));
        def_val(the, nav, "appVersion", xsString("1.0"));
        def_val(the, glob, "navigator", nav);
    }

    /* location: href (read-only string) + ignored assign/replace */
    {
        xsSlot loc = xsNewHostObject(NULL);
        def_val(the, loc, "href",
                xsString(b->doc->baseUrl ? b->doc->baseUrl : ""));
        def_fn(the, loc, "assign", xs_location_assign, 1);
        def_fn(the, loc, "replace", xs_location_assign, 1);
        def_val(the, glob, "location", loc);
    }

    /* document */
    {
        xsSlot doc = xsNewHostObject(NULL);
        def_fn(the, doc, "getElementById", xs_document_getElementById, 1);
        def_fn(the, doc, "createElement", xs_document_createElement, 1);
        def_fn(the, doc, "createTextNode", xs_document_createTextNode, 1);
        def_fn(the, doc, "write", xs_document_write, 0);
        def_fn(the, doc, "writeln", xs_document_writeln, 0);
        def_fn(the, doc, "addEventListener", xs_doc_addEventListener, 2);
        /* title as a live getter (walks the tree; see above) */
        {
            xsSlot getter = xsNewHostFunction(xs_document_getTitle, 0);
            xsDefine(doc, xsID("title"), getter, xsIsGetter);
        }
        /* body: live wrapper for <body> (or root) */
        DomNode *body = first_element(b->dom->root, "body");
        def_val(the, doc, "body",
                xs_push_element(b, body ? body : b->dom->root));
        def_val(the, glob, "document", doc);
    }

    /* alert/confirm/prompt + no-op timers (UNSUPPORTED — parity) */
    def_fn(the, glob, "alert", xs_alert, 0);
    def_fn(the, glob, "confirm", xs_noop, 0);
    def_fn(the, glob, "prompt", xs_noop, 0);
    def_fn(the, glob, "setTimeout", xs_noop, 0);
    def_fn(the, glob, "clearTimeout", xs_noop, 0);
    def_fn(the, glob, "setInterval", xs_noop, 0);
    def_fn(the, glob, "clearInterval", xs_noop, 0);
    def_fn(the, glob, "requestAnimationFrame", xs_noop, 0);
}

/* ── microtask drain (fxRunLoop parity, tool-free) ───────────────────── */
static void xs_drain_jobs(xsMachine *the)
{
    fxEndJob(the);
    while (the->promiseJobs)
    {
        while (the->promiseJobs)
        {
            the->promiseJobs = 0;
            fxRunPromiseJobs(the);
        }
        fxEndJob(the);
    }
}

/* ── metering (per-script run limit) ─────────────────────────────────── */
static txBoolean xs_meter_callback(txMachine *the, txU8 count)
{
    (void)the;
    return count <= (txU8)XS_RUNLIMIT;
}

/* ── vtable entry points ─────────────────────────────────────────────── */

static int xs_init(JsBridge *b, const char *baseUrl)
{
    (void)baseUrl;
    XsState *st = (XsState *)JMalloc(sizeof(XsState));
    if (!st)
    {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    b->implState = st;

    xsMachine *m = xsCreateMachine(&xs_creation, "pluto-page", (void *)b);
    if (!m)
    {
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    st->machine = m;

    /* One contained bracket for ALL setup; any abort here fails init. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xs_build_el_proto(b);
        xs_build_globals(b);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    if (m->exitStatus != xsNormalExit)
    {
        /* aborted during setup (OOM/stack/meter): contained init failure */
        logger_log("[xs] init aborted (%s)", fxAbortString(m->exitStatus));
        xsDeleteMachine(m);
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    return 0;
}

static void xs_fail_script(JsBridge *b, int index, const char *msg)
{
    b->errs++;
    if (!b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError), "%s", msg);
    }
    logger_log("[js] script %d skipped: %s", index, msg);
}

static void xs_run_script(JsBridge *b, const char *src, size_t len,
                          int index)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    if (len > JSBRIDGE_MAX_SCRIPT_BYTES)
    {
        xs_fail_script(b, index, "script too large");
        return;
    }
    /* Same compile-safety gate as muJS/Duktape/QuickJS (one bar). */
    if (!jsbridge_script_compile_safe(src, len))
    {
        xs_fail_script(b, index, "script too deeply nested (compile guard)");
        return;
    }

    b->ran++;

    /* Copy + NUL-terminate (span is followed by '</script>' in the page
     * buffer; the lexer stops at NUL like the other bridges' copy). */
    char *buf = (char *)JMalloc(len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, src, len);
    buf[len] = '\0';

    xsMachine *m = st->machine;
    /* Nesting mirrors the stock tool host (xst.c): Metering bracket
     * outside, a full Host bracket inside a block, each Begin paired
     * with its own End. HostExit stays OUTERMOST so fxExitToHost (our
     * fxAbort) unwinds here, contained, with exitStatus readable. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
            txStringCStream stream;
            stream.buffer = buf;
            stream.offset = 0;
            stream.size = len;
            txScript *script =
                fxParseScript(the, &stream, fxStringCGetter, mxProgramFlag);
            if (script)
            {
                xsVar(0) = xsUndefined;
                fxRunScript(the, script, mxThis, C_NULL, C_NULL, C_NULL,
                            mxProgram.value.reference);
                xs_drain_jobs(the);
            }
        }
        xsCatch
        {
            /* Script-level throw: contained. Surface "message: text"
             * like the other bridges render Error values. */
            char msg[112];
            msg[0] = '\0';
            {
                /* mxException holds the thrown value at this point. */
                xsSlot exc = xsException;
                if (exc.kind == XS_REFERENCE_KIND)
                {
                    xsSlot m2 = xsGet(exc, xsID("message"));
                    char *s = to_cstring(the, m2);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
                if (!msg[0])
                {
                    char *s = to_cstring(the, exc);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
            }
            logger_log("[js] script %d failed: %s", index,
                       msg[0] ? msg : "exception");
            bridge_take_error_text(b, msg[0] ? msg : "exception");
            b->errs++;
        }
    }
        xsEndHost(m);
    }
    /* Per-script meter budget, like the other bridges' run limits. */
    xsEndMetering(m);
    xsEndHostExit(m);
    JFree(buf);

    if (m->exitStatus != xsNormalExit)
    {
        /* Engine ABORT (OOM / C-stack / meter / unhandled rejection):
         * the page's machine state is no longer trustworthy — fail the
         * script and tear the engine down so nothing stale survives. */
        b->errs++;
        char why[64];
        snprintf(why, sizeof(why), "%s", fxAbortString(m->exitStatus));
        bridge_take_error_text(b, why);
        logger_log("[js] script %d aborted engine (%s) — engine reset",
                   index, why);
        st->machine = NULL;
        xsDeleteMachine(m);
    }
}

static int xs_dispatch_click(JsBridge *b, const void *anchorNode)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
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

        xsMachine *m = st->machine;
        xsBeginHostExit(m);
        xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
        {
            xsBeginHost(m);
            {
                xsVars(1);
                xsTry
                {
                    /* fresh wrapper for the target + event object */
                    xsVar(0) = xs_push_element(b, (DomNode *)L->target);
                    xsSlot ev = xsNewHostObject(NULL);
                    xsDefine(ev, xsID("type"), xsString("click"), xsDefault);
                    xsDefine(ev, xsID("preventDefault"),
                             xsNewHostFunction(xs_event_preventDefault, 0),
                             xsDefault);
                    xsCall2_noResult(st->fns[i], xsID("call"), xsVar(0), ev);
                    xs_drain_jobs(the);
                }
                xsCatch
                {
                    b->errs++;
                    bridge_take_error_text(b, "listener exception");
                    logger_log("[js] click handler failed");
                }
            }
            xsEndHost(m);
        }
        xsEndMetering(m);
        xsEndHostExit(m);

        b->inClick = 0;
        b->doc->jsErrors = b->errs;
        snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                 b->lastError);
        if (m->exitStatus != xsNormalExit)
        {
            b->errs++;
            bridge_take_error_text(b, "engine abort in click handler");
            logger_log("[js] click dispatch aborted engine — engine reset");
            st->machine = NULL;
            xsDeleteMachine(m);
            return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
        }
        if (b->preventDef)
        {
            return JSB_CLICK_SUPPRESSED;
        }
    }
    return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
}

static void xs_close(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st)
    {
        return;
    }
    if (st->machine)
    {
        xsMachine *m = st->machine;
        xsBeginHostExit(m);
        {
            for (int i = 0; i < b->listenerCount && i < JSBRIDGE_LISTENERS_MAX;
                 i++)
            {
                xsForget(st->fns[i]);
            }
            xsForget(st->elProto);
        }
        xsEndHostExit(m);
        xsDeleteMachine(m); /* frees wrappers, scripts, pinned slots */
        st->machine = NULL;
    }
    JFree(st);
    b->implState = NULL;
}

const JsEngineImpl js_engine_xs = {
    xs_init, xs_run_script, xs_dispatch_click, xs_close, "XS (Moddable)"};
