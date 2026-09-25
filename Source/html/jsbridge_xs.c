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
#include "../core/pluto_mem.h"

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "../core/logger.h"
#include "../util/strbuf.h"
#include "../html/dom.h"

/* Stock engine API — the exact include set of the canonical host tool
 * (xs/tools/xst.c): xsAll.h (internals, pulls xsPlatform.h → our
 * Source/html/xs_platform.h via -DINCLUDE_XSPLATFORM), then the public
 * xs.h macros. */
#include "../js/xs_moddable/sources/xsAll.h"
#include "../js/xs_moddable/sources/xsScript.h"
#include "../js/xs_moddable/includes/xs.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)

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
/* Host/sim lab builds (-O0 + sanitizers): instrumented engine frames are
 * several-fold larger than thin -O2 ARM frames, so the 64KB budget tripped
 * "[xs] abort: native stack overflow" on the first trivial script (same
 * class as the QuickJS host budget fix). The host stack is 8MB; this is
 * only the engine's own guard bound — the device budget above is the one
 * that protects real hardware. */
#define XS_CSTACK_LIMIT_DFL (512u * 1024u)
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
    /* Pinned timer callbacks; JsTimer.ref holds the tfn[] slot index.
     * Same xsRemember/xsForget ownership contract as fns[]. */
    xsSlot tfn[JSBRIDGE_TIMERS_MAX];
    unsigned char tfnSet[JSBRIDGE_TIMERS_MAX]; /* slot holds a pin */
    /* XHR + fetch pins (same ownership contract; fetch uses the same
     * xfn[] slots — XS has no native Promise, so only XMLHttpRequest). */
    xsSlot xfn[JSBRIDGE_XHR_MAX]; /* completion handlers / promise resolve */
    xsSlot xobj[JSBRIDGE_XHR_MAX]; /* wrapper objects (this) */
    unsigned char xfnSet[JSBRIDGE_XHR_MAX];
    unsigned char xobjSet[JSBRIDGE_XHR_MAX];
    xsSlot xhrProto; /* XMLHttpRequest prototype (rooted) */
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

/* Method/property definition helpers (defined with the prototype builder). */
static void def_fn(xsMachine *the, xsSlot obj, const char *name,
                   xsCallback fn, int length);
static void def_val(xsMachine *the, xsSlot obj, const char *name,
                    xsSlot value);

/* Element prototype: built once at init (see xs_build_el_proto), accessors
 * + methods live there; instances inherit through the prototype chain. */
static void xs_build_el_proto(JsBridge *b);
static void xs_build_xhr_proto(JsBridge *b);
static void xs_xmlhttprequest_new(xsMachine *the);

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

/* SW5: REAL markup assignment through the router (was textContent). */
static void xs_el_set_innerHTML(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *s = to_cstring(the, xsArg(0));
        if (!s)
        {
            xsTypeError("out of memory");
        }
        int rc = jsbridge_el_set_inner_html(b, n, s, strlen(s));
        JFree(s);
        if (rc != 0)
        {
            xsTypeError("innerHTML assignment failed");
        }
    }
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

/* ── SW5: inline style string reader/writer (shared with the style object
 * accessors; same per-property semantics as the other engines) ────────── */
static void xs_style_read_prop(const DomNode *n, const char *prop, char *out,
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

static void xs_style_write_prop(JsBridge *b, DomNode *n, const char *prop,
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

static const char *const XS_STYLE_PROPS[] = {
    "display", "visibility", "text-align", "font-weight", "font-style",
    "text-decoration", "color", "background"};

/* Style accessors: one function per property via the prototype loop, the
 * property name carried in a reserved own prop (XS host functions see only
 * the machine state — the getter scans xsThis's hidden "st.prop" marker).
 * Simpler + proven: one getter/setter PAIR per property, closed over by
 * name through separate C functions generated by the table below. */

#define XS_STYLE_GETTER(name, prop)                                        \
    static void xs_style_get_##name(xsMachine *the)                        \
    {                                                                      \
        char val[128];                                                     \
        xs_style_read_prop(this_node(the), prop, val, sizeof(val));         \
        xsResult = xsString(val);                                          \
    }
#define XS_STYLE_SETTER(name, prop)                                        \
    static void xs_style_set_##name(xsMachine *the)                        \
    {                                                                      \
        JsBridge *b = bridge_of(the);                                      \
        BUDGET_OR_THROW(b);                                                \
        DomNode *n = this_node(the);                                       \
        if (n && n->kind == DOM_ELEMENT)                                   \
        {                                                                  \
            char *v = to_cstring(the, xsArg(0));                           \
            if (!v)                                                        \
            {                                                              \
                xsTypeError("out of memory");                              \
            }                                                              \
            xs_style_write_prop(b, n, prop, v);                            \
            JFree(v);                                                      \
        }                                                                  \
    }
XS_STYLE_GETTER(display, "display")
XS_STYLE_SETTER(display, "display")
XS_STYLE_GETTER(visibility, "visibility")
XS_STYLE_SETTER(visibility, "visibility")
XS_STYLE_GETTER(textAlign, "text-align")
XS_STYLE_SETTER(textAlign, "text-align")
XS_STYLE_GETTER(fontWeight, "font-weight")
XS_STYLE_SETTER(fontWeight, "font-weight")
XS_STYLE_GETTER(fontStyle, "font-style")
XS_STYLE_SETTER(fontStyle, "font-style")
XS_STYLE_GETTER(textDecoration, "text-decoration")
XS_STYLE_SETTER(textDecoration, "text-decoration")
XS_STYLE_GETTER(color, "color")
XS_STYLE_SETTER(color, "color")
XS_STYLE_GETTER(background, "background")
XS_STYLE_SETTER(background, "background")

/* Fresh style host object for `node` (accessors over the walker vocab). */
static void xs_push_style(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b);
    xsSlot obj = xsNewHostObject(NULL);
    static const struct
    {
        const char *name;
        xsCallback get;
        xsCallback set;
    } accs[] = {
        {"display", xs_style_get_display, xs_style_set_display},
        {"visibility", xs_style_get_visibility, xs_style_set_visibility},
        {"textAlign", xs_style_get_textAlign, xs_style_set_textAlign},
        {"fontWeight", xs_style_get_fontWeight, xs_style_set_fontWeight},
        {"fontStyle", xs_style_get_fontStyle, xs_style_set_fontStyle},
        {"textDecoration", xs_style_get_textDecoration,
         xs_style_set_textDecoration},
        {"color", xs_style_get_color, xs_style_set_color},
        {"background", xs_style_get_background, xs_style_set_background},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        xsDefine(obj, xsID(accs[i].name), getter, xsIsGetter);
        xsSlot setter = xsNewHostFunction(accs[i].set, 1);
        xsDefine(obj, xsID(accs[i].name), setter, xsIsSetter);
    }
    /* Host data = the element node: classList/style METHODS run with `this`
     * = the object itself, so this_node(the) resolves through it. */
    xsSetHostData(obj, node);
    xsResult = obj;
}

/* ── SW5 (O4): classList host object (fresh per access). Methods mutate
 * through the router; `length` is a live getter; `item(i)` the i-th token. */
static void xs_cl_add(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_add(b, n, tok);
    JFree(tok);
    if (rc != 0)
    {
        xsTypeError("classList.add failed");
    }
}

static void xs_cl_remove(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_remove(b, n, tok);
    JFree(tok);
    if (rc != 0)
    {
        xsTypeError("classList.remove failed");
    }
}

static void xs_cl_toggle(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_toggle(b, n, tok);
    JFree(tok);
    if (rc < 0)
    {
        xsTypeError("classList.toggle failed");
    }
    xsResult = xsBoolean(rc == 1);
}

static void xs_cl_contains(xsMachine *the)
{
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    xsResult = xsBoolean(jsbridge_el_class_has(n, tok));
    JFree(tok);
}

static void xs_cl_item(xsMachine *the)
{
    DomNode *n = this_node(the);
    int idx = (xsToInteger(xsArgc) > 0) ? (int)xsToNumber(xsArg(0)) : -1;
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
            xsResult = xsString(tok);
            return;
        }
        p += n2;
    }
    xsResult = xsNull;
}

static void xs_cl_length(xsMachine *the)
{
    DomNode *n = this_node(the);
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
    xsResult = xsInteger(k);
}

static void xs_push_classlist(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b);
    xsSlot obj = xsNewHostObject(NULL);
    def_fn(the, obj, "add", xs_cl_add, 1);
    def_fn(the, obj, "remove", xs_cl_remove, 1);
    def_fn(the, obj, "toggle", xs_cl_toggle, 1);
    def_fn(the, obj, "contains", xs_cl_contains, 1);
    def_fn(the, obj, "item", xs_cl_item, 1);
    {
        xsSlot getter = xsNewHostFunction(xs_cl_length, 0);
        xsDefine(obj, xsID("length"), getter, xsIsGetter);
    }
    xsSetHostData(obj, node); /* see xs_push_style: methods read `this` */
    xsResult = obj;
}

/* ── SW5: element method additions ───────────────────────────────────── */
static void xs_el_insertBefore(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    /* arg_node is fxGetHostDataIf-based: undefined/null/any primitive
     * yields NULL, which dom_insert_before treats as append-at-end. */
    DomNode *ref = (xsToInteger(xsArgc) > 1) ? arg_node(the, 1) : NULL;
    if (!n || !c || dom_insert_before(b->dom, n, c, ref) != 0)
    {
        xsTypeError("insertBefore failed");
    }
}

/* querySelector emit: XS pushes wrappers into the array slot carried in
 * the user data (array + running index). */
typedef struct
{
    JsBridge *b;
    xsMachine *m; /* the xsSetAt macro below needs a local named `the` */
    xsSlot arr;
    int k;
} XsQsEmit;

static int qs_emit_xs(void *elp, void *ud)
{
    XsQsEmit *e = (XsQsEmit *)ud;
    xsSlot v = xs_push_element(e->b, (DomNode *)elp);
    xsMachine *the = e->m;
    xsSetAt(e->arr, xsInteger(e->k++), v);
    return 0;
}

static void xs_el_querySelectorAll(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    xsResult = xsNewArray(0);
    XsQsEmit e = {b, the, xsResult, 0};
    jsbridge_el_query_selector_all(b, n, sel, scratch, sizeof(scratch),
                                   qs_emit_xs, &e);
    JFree(sel);
}

static void xs_el_querySelector(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, n, sel, scratch, sizeof(scratch));
    JFree(sel);
    xsResult = xs_push_element(b, hit);
}

/* SW5 sim finding: the document object is a plain host object (host data
 * NULL), so the ELEMENT querySelector fns read this_node→NULL and return
 * null/empty at document level. Dedicated document-level fns scope to the
 * root — same pattern as the other bridges. */
static void xs_document_querySelector(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, b->dom ? b->dom->root : NULL, sel, scratch, sizeof(scratch));
    JFree(sel);
    xsResult = xs_push_element(b, hit);
}

static void xs_document_querySelectorAll(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    xsResult = xsNewArray(0);
    XsQsEmit e = {b, the, xsResult, 0};
    jsbridge_el_query_selector_all(b, b->dom ? b->dom->root : NULL, sel,
                                   scratch, sizeof(scratch), qs_emit_xs, &e);
    JFree(sel);
}

/* ── SW5 accessors: navigation + classList/style object getters ──────── */
static void xs_el_get_firstElementChild(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the),
                               n ? dom_first_element_child(n) : NULL);
}

static void xs_el_get_nextElementSibling(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the),
                               n ? dom_next_element_sibling(n) : NULL);
}

static void xs_el_get_classList(xsMachine *the)
{
    xs_push_classlist(bridge_of(the), this_node(the));
}

static void xs_el_get_style(xsMachine *the)
{
    xs_push_style(bridge_of(the), this_node(the));
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
        {"firstElementChild", xs_el_get_firstElementChild, NULL},
        {"nextElementSibling", xs_el_get_nextElementSibling, NULL},
        {"classList", xs_el_get_classList, NULL},
        {"style", xs_el_get_style, NULL},
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
        {"insertBefore", xs_el_insertBefore, 2},
        {"querySelector", xs_el_querySelector, 1},
        {"querySelectorAll", xs_el_querySelectorAll, 1},
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

/* ── timers (setTimeout / setInterval): router table + engine refs ──────── */
/* XS host functions read args via xsArg(i) and return via xsResult.
 * Registration mirrors the listener path: the callback is stored +
 * xsRemember'd (GC-pinned); JsTimer.ref = tfn[] slot index. */
static void xs_timer_setup(xsMachine *the, JsBridge *b, XsState *st,
                           JsTimerKind kind)
{
    xsSlot fn = xsArg(0);
    if (!fxIsCallable(the, &fn))
    {
        xsTypeError("timer callback must be a function");
    }
    if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
    {
        xsTypeError("too many timers");
    }
    int slot = -1;
    /* Slot 0 is RESERVED: JsTimer.ref == NULL is the router's refusal
     * sentinel, and slot 0 would encode as (void*)0 — so the very first
     * timer would be rejected as "too many timers". Scan from 1. */
    for (int i = 1; i < JSBRIDGE_TIMERS_MAX; i++)
    {
        if (!st->tfnSet[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        xsTypeError("too many timers");
    }
    int delay = 0;
    if (xsToInteger(xsArgc) > 1)
    {
        delay = (int)xsToNumber(xsArg(1));
    }
    if (delay < 0)
    {
        delay = 0;
    }
    int id = jsbridge_timer_start(b, kind, (void *)(intptr_t)slot,
                                  (unsigned)delay);
    if (id == 0)
    {
        xsTypeError("too many timers");
    }
    st->tfn[slot] = fn;
    st->tfnSet[slot] = 1;
    xsRemember(st->tfn[slot]); /* pin against GC */
    xsResult = xsInteger(id);
}

static void xs_setTimeout(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_timer_setup(the, b, st, JS_TIMER_TIMEOUT);
}

static void xs_setInterval(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_timer_setup(the, b, st, JS_TIMER_INTERVAL);
}

static void xs_clear_common(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int id = (xsToInteger(xsArgc) > 0) ? (int)xsToNumber(xsArg(0)) : 0;
    int cleared = jsbridge_timer_clear(b, id);
    xsResult = xsInteger(cleared);
}

static void xs_clearTimeout(xsMachine *the)
{
    xs_clear_common(the);
}

static void xs_clearInterval(xsMachine *the)
{
    xs_clear_common(the);
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
        def_fn(the, doc, "querySelector", xs_document_querySelector, 1);
        def_fn(the, doc, "querySelectorAll", xs_document_querySelectorAll, 1);
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
    def_fn(the, glob, "setTimeout", xs_setTimeout, 0);
    def_fn(the, glob, "clearTimeout", xs_clearTimeout, 0);
    def_fn(the, glob, "setInterval", xs_setInterval, 0);
    def_fn(the, glob, "clearInterval", xs_clearInterval, 0);
    def_fn(the, glob, "requestAnimationFrame", xs_setTimeout, 0);
}

/* ── microtask drain (fxRunLoop parity, tool-free) ─────────────────────
 * Jobs live in the reserved mxPendingJobs slot (fixed offset from
 * stackTop, addressable whenever the machine is quiescent — both drain
 * call sites are). fxRunPromiseJobs moves the chain to mxRunningJobs;
 * jobs queued *during* a job append to mxPendingJobs again, so loop
 * until the chain is empty. fxEndJob = post-drain cleanup.
 */
static void xs_drain_jobs(xsMachine *the)
{
    fxEndJob(the);
    while (mxPendingJobs.value.reference->next)
    {
        fxRunPromiseJobs(the);
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
        xs_build_xhr_proto(b);
        /* XMLHttpRequest constructor global (xsGlobal pattern, line 1030).
         * SW5 sim finding: xsNewHostFunction does NOT set the constructor
         * flag, so `new XMLHttpRequest()` threw XS_TYPE_ERROR ("new: not a
         * constructor") — the suite's only unguarded `new`. Use
         * xsNewHostConstructor so both call forms work (needs `the` from the
         * xsBeginHost bracket; prototype pins the instance shape). */
        xsBeginHost(m);
        {
            xsSlot xglob = xsGlobal;
            xsDefine(xglob, xsID("XMLHttpRequest"),
                     xsNewHostConstructor(xs_xmlhttprequest_new, 0, st->xhrProto), xsDefault);
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    if (m->exitStatus != xsNormalExit)
    {
        /* aborted during setup (OOM/stack/meter): contained init failure */
        logger_log("[xs] init aborted (%s)", fxAbortString(m->exitStatus));
        xsDeleteMachine(m);
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
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
    if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
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

    /* SW5 ES5 prefix + page source in ONE buffer (prefix is deterministic
     * per build and NOT part of the compile gate input). */
    const char *prefix = NULL;
    size_t plen = jsbridge_sw5_prefix(&prefix);

    /* Copy + NUL-terminate (span is followed by '</script>' in the page
     * buffer; the lexer stops at NUL like the other bridges' copy). */
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
        /* xsDeleteMachine's stock fxDeleteMachine frees whole heap chunks
         * through c_free, but funnel size-tracking can drop those entries
         * (table pressure, 8-probe cap) and grow-shrinks subtract nothing
         * when the old entry was lost — the dead machine's bytes then stay
         * as phantom "live" weight. Left alone, the SW3a gate refuses the
         * parser/walker's arena chunks (device 2026-09-24: bing.com →
         * "[xs] abort: memory full" → phantom ~6.5MB live > 6.5MB budget →
         * refusals → w->error → spurious "Parse Error" page). Re-base the
         * counter to the tracked table's sum. */
        pluto_mem_resync_live();
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
            pluto_mem_resync_live(); /* counter re-base (see run_script) */
            return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
        }
        if (b->preventDef)
        {
            return JSB_CLICK_SUPPRESSED;
        }
    }
    return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
}

/* Timer vtable: release one pinned callback (fnRef = tfn[] slot). */
static void xs_clear_timer_ref(JsBridge *b, void *fnRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return;
    }
    int slot = (int)(intptr_t)fnRef;
    if (slot < 0 || slot >= JSBRIDGE_TIMERS_MAX || !st->tfnSet[slot])
    {
        return;
    }
    xsBeginHostExit(st->machine);
    xsForget(st->tfn[slot]);
    xsEndHostExit(st->machine);
    st->tfnSet[slot] = 0;
}

/* Invoke one pinned callback. Returns 0 ok, 1 contained error, -1 abort. */
static int xs_run_timer_ref(JsBridge *b, void *fnRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return 1;
    }
    int slot = (int)(intptr_t)fnRef;
    if (slot < 0 || slot >= JSBRIDGE_TIMERS_MAX || !st->tfnSet[slot])
    {
        return 1;
    }
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                xsVar(0) = st->tfn[slot];
                xsCall0_noResult(xsVar(0), xsID("call"));
            }
            xsCatch
            {
                b->errs++;
                bridge_take_error_text(b, "timer callback exception");
                logger_log("[js] timer handler failed");
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
        bridge_take_error_text(b, "engine abort in timer callback");
        logger_log("[js] timer dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    return 0;
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
            for (int i = 0; i < JSBRIDGE_TIMERS_MAX; i++)
            {
                if (st->tfnSet[i])
                {
                    xsForget(st->tfn[i]);
                    st->tfnSet[i] = 0;
                }
            }
        }
        xsEndHostExit(m);
        xsDeleteMachine(m); /* frees wrappers, scripts, pinned slots */
        st->machine = NULL;
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
    }
    JFree(st);
    b->implState = NULL;
}


/* ── XMLHttpRequest (async HTTP → JS callbacks) ────────────────────────────
 * Same thin-glue contract as the other bridges (router owns everything):
 * wrapper = host instance of a rooted prototype (host data = public request
 * id); live state reads are prototype accessors over the router table;
 * onload/onerror/onreadystatechange are pinned at send into xfn[]/xobj[]
 * (xsRemember'd; JsHttpRequest.fnRef = xfn slot, objRef = xobj slot — both
 * indexes, NO +1: slot 0 of the PINS is usable because the router's NULL
 * rule applies to the ref POINTER, and slots here are small ints; encode
 * slot+1 anyway to stay uniform with the other engines). Caps + budgets
 * live in the router (jsbridge.c). No native Promise in XS → no fetch(). */
static void xs_xhr_open(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const char *method = xsToString(xsArg(0));
    const char *url = xsToString(xsArg(1));
    int id = jsbridge_xhr_open(b, method, url);
    if (id == 0)
    {
        xsTypeError("%s", b->lastError[0] ? b->lastError : "xhr open failed");
    }
    xsSetHostData(xsThis, (void *)(intptr_t)id);
    xsResult = xsUndefined;
}

static int xs_xhr_id(xsMachine *the)
{
    return (int)(intptr_t)xsGetHostDataIf(xsThis);
}

static void xs_xhr_send(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    int id = xs_xhr_id(the);
    if (id <= 0)
    {
        xsTypeError("xhr: send before open");
    }
    int fslot = -1, oslot = -1;
    for (int i = 0; i < JSBRIDGE_XHR_MAX; i++)
    {
        if (!st->xfnSet[i] && fslot < 0)
            fslot = i;
        if (!st->xobjSet[i] && oslot < 0)
            oslot = i;
    }
    if (fslot < 0 || oslot < 0)
    {
        xsTypeError("xhr: pin slots full");
    }
    /* Completion handler: onload → onerror → onreadystatechange. */
    static const char *const names[] = {"onload", "onerror",
                                        "onreadystatechange"};
    xsSlot fn;
    int found = 0;
    for (int i = 0; i < 3 && !found; i++)
    {
        xsSlot v = xsGet(xsThis, xsID(names[i]));
        if (fxIsCallable(the, &v))
        {
            fn = v;
            found = 1;
        }
    }
    if (!found)
    {
        xsTypeError("xhr: no onload/onerror/onreadystatechange handler");
    }
    st->xfn[fslot] = fn;
    st->xfnSet[fslot] = 1;
    xsRemember(st->xfn[fslot]);
    st->xobj[oslot] = xsThis;
    st->xobjSet[oslot] = 1;
    xsRemember(st->xobj[oslot]);
    jsbridge_xhr_send(b, id, (void *)(intptr_t)(fslot + 1),
                      (void *)(intptr_t)(oslot + 1));
    xsResult = xsUndefined;
}

static void xs_xhr_abort(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    jsbridge_xhr_abort(b, xs_xhr_id(the));
    xsResult = xsUndefined;
}

static void xs_xhr_get_readyState(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsInteger(r ? r->state : 0);
}

static void xs_xhr_get_status(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsInteger((r && r->state >= JS_XHR_DONE) ? r->status : 0);
}

static void xs_xhr_get_responseText(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsString((r && r->body) ? r->body : "");
}

static void xs_xhr_get_responseURL(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsString((r && r->url[0]) ? r->url : "");
}

static void xs_build_xhr_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);
    st->xhrProto = xsNewHostObject(NULL);
    def_fn(the, st->xhrProto, "open", xs_xhr_open, 2);
    def_fn(the, st->xhrProto, "send", xs_xhr_send, 0);
    def_fn(the, st->xhrProto, "abort", xs_xhr_abort, 0);
    struct
    {
        const char *name;
        xsCallback get;
    } accs[] = {
        {"readyState", xs_xhr_get_readyState},
        {"status", xs_xhr_get_status},
        {"responseText", xs_xhr_get_responseText},
        {"response", xs_xhr_get_responseText},
        {"responseURL", xs_xhr_get_responseURL},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        xsDefine(st->xhrProto, xsID(accs[i].name), getter, xsIsGetter);
    }
    xsRemember(st->xhrProto);
}

/* Constructor global: new XMLHttpRequest() / XHR() → fresh wrapper. */
static void xs_xmlhttprequest_new(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xsSlot obj = xsNewHostInstance(st->xhrProto);
    xsSetHostData(obj, (void *)(intptr_t)0);
    xsResult = obj;
}

/* XHR vtable: release the pinned completion + wrapper (slot inert). */
static void xs_clear_xhr_refs(JsBridge *b, void *fnRef, void *objRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    int fslot = (int)(intptr_t)fnRef - 1;
    int oslot = (int)(intptr_t)objRef - 1;
    xsBeginHostExit(st->machine);
    if (fslot >= 0 && fslot < JSBRIDGE_XHR_MAX && st->xfnSet[fslot])
    {
        xsForget(st->xfn[fslot]);
        st->xfnSet[fslot] = 0;
    }
    if (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX && st->xobjSet[oslot])
    {
        xsForget(st->xobj[oslot]);
        st->xobjSet[oslot] = 0;
    }
    xsEndHostExit(st->machine);
}

/* Invoke the pinned completion: fn.call(this, responseText) — the same
 * bracket pattern as xs_run_timer_ref (metering + contained exception +
 * abort → engine reset). */
static int xs_run_xhr_ref(JsBridge *b, void *fnRef, void *objRef,
                          const JsHttpRequest *r)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return 1;
    }
    int fslot = (int)(intptr_t)fnRef - 1;
    int oslot = (int)(intptr_t)objRef - 1;
    if (fslot < 0 || fslot >= JSBRIDGE_XHR_MAX || !st->xfnSet[fslot])
    {
        return 1;
    }
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(3);
            xsTry
            {
                xsVar(0) = st->xfn[fslot];
                xsVar(1) = (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX &&
                            st->xobjSet[oslot])
                               ? st->xobj[oslot]
                               : xsNull;
                xsVar(2) = xsString((r && r->body) ? r->body : "");
                xsCall2_noResult(xsVar(0), xsID("call"), xsVar(1), xsVar(2));
            }
            xsCatch
            {
                if (!b->lastError[0])
                {
                    bridge_take_error_text(b, xsToString(xsException));
                }
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
        bridge_take_error_text(b, "engine abort in xhr callback");
        logger_log("[js] xhr dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    return 0;
}

const JsEngineImpl js_engine_xs = {
    xs_init,
    xs_run_script,
    xs_dispatch_click,
    xs_clear_timer_ref,
    xs_run_timer_ref,
    xs_run_xhr_ref,
    xs_clear_xhr_refs,
    xs_close,
    "XS (Moddable)"};
