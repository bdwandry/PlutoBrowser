/*
 * PlutoBrowser — jsbridge.c
 * Engine-agnostic JS bridge ROUTER: owns the public jsbridge.h API, the
 * script-slot scanning, the document.write adoption and the shared bridge
 * state (see jsbridge_internal.h). Per-engine work lives in
 * jsbridge_mujs.c / jsbridge_duktape.c behind the JsEngineImpl vtable.
 *
 * Engine isolation contract (user rule): vendored engines under Source/js
 * are NEVER modified; each page runs exclusively on the engine selected in
 * Settings ("Javascript Engine": muJS or Duktape). There is no fallback and
 * no shared engine state — selecting an engine means ALL of the page's
 * scripts (inline + Full-mode externals + click handlers) execute on that
 * engine's binaries alone.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "jsext.h"
#include "../core/logger.h"
#include "../core/url.h"
#include "../core/http_client.h"
#include "../util/strbuf.h"
#include "../core/pluto_mem.h"
#include "../html/dom.h"
#include "../html/css.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)

/* Selected engine for ALL subsequently attached pages (0=muJS 1=Duktape).
 * Set from Settings via jsbridge_set_engine(); NOT from core/storage so the
 * html layer stays decoupled and host tests can drive both engines. */
static int g_selectedEngine = 0;

/* XHR per-page byte-budget epoch (monotonic page counter). */
static int g_xhrEpochSeq = 0;

/* Module-global HTTP session (context-free client callbacks — jsext.c
 * pattern): the live page bridge, the id on the wire, the per-page byte
 * budget + the epoch it belongs to. */
static JsBridge *g_xhrBridge = NULL;
static int g_xhrActiveId = 0;
static size_t g_xhrPageBudget = JSBRIDGE_XHR_PAGE_BUDGET;
static int g_xhrBudgetPage = 0;

static void xhr_session_bind(JsBridge *b);

/* ── shared budget (impls call this before every DOM mutation) ───────────── */
int budget_take(JsBridge *b)
{
    if (b->callBudget <= 0)
    {
        return 0;
    }
    b->callBudget--;
    return 1;
}

void bridge_take_error_text(JsBridge *b, const char *msg)
{
    if (b && msg && !b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError), "%s", msg);
    }
}

void jsbridge_set_engine(int engine)
{
    g_selectedEngine = (engine == JS_ENGINE_MUJS ||
                        engine == JS_ENGINE_DUKTAPE ||
                        engine == JS_ENGINE_QUICKJS ||
                        engine == JS_ENGINE_XS)
                           ? engine
                           : JS_ENGINE_MUJS;
}

int jsbridge_current_engine(void)
{
    return g_selectedEngine;
}

/* ── engine vtables (defined in the per-engine files) ────────────────────── */
extern const JsEngineImpl js_engine_mujs;
extern const JsEngineImpl js_engine_duktape;
extern const JsEngineImpl js_engine_quickjs;
extern const JsEngineImpl js_engine_xs;

const JsEngineImpl *js_bridge_impl(JsBridge *b)
{
    if (!b)
    {
        return NULL;
    }
    if (b->engine == JS_ENGINE_DUKTAPE)
    {
        return &js_engine_duktape;
    }
    if (b->engine == JS_ENGINE_QUICKJS)
    {
        return &js_engine_quickjs;
    }
    if (b->engine == JS_ENGINE_XS)
    {
        return &js_engine_xs;
    }
    return &js_engine_mujs;
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
 * Runs in OUR code before EITHER engine sees the script. It bounds the
 * nesting/regex shape of a script so recursive engines (muJS's parser and
 * regex compiler are the reference case) cannot overflow the device's
 * 61.8KB game-task stack. Duktape's compiler is byte-code based and far
 * shallower, but the SAME gate is applied to both engines on purpose: one
 * policy, one log signature, no engine gets a different safety bar.
 * Heuristic by design: false rejects (deep-but-legal scripts) are
 * acceptable on this hardware; a stack overflow is not.
 *
 * DEPTH IS ENGINE- AND TARGET-AWARE (SW4 device crash lesson): the bounds
 * here must predict each engine parser's real C-stack cost. QuickJS's
 * parse recursion is NOT stack-probed (only its regex compiler and the
 * interpreter are), and a depth-19 minified bundle overran the 61.8KB
 * gameTask stack on hardware (errorlog: "stack overflow in task gameTask")
 * — the simulator never sees this with its 8MB host stack. muJS, Duktape
 * and XS all parsed the same bundle fine (their own AST/stack guards
 * tripped or their parsers are leaner), so the shared gate takes an
 * engine-aware depth cap: PLUTO_SCAN_MAX_DEPTH for those engines,
 * PLUTO_SCAN_MAX_DEPTH_QJS for QuickJS on DEVICE (sim keeps the wide
 * bound; its stack is huge and host suites exercise deep fixtures). */
#include "jsbridge_internal.h" /* already included at top; this call-site
                               * comment marks that PLUTO_SCAN_MAX_DEPTH(_QJS)
                               * caps come from there (SW4 device-crash lesson) */
#define PLUTO_SCAN_MAX_REGEX_LEN 8192
#define PLUTO_SCAN_MAX_REGEX_ESC 64  /* consecutive escapes = recursion depth */
#define PLUTO_SCAN_MAX_OPENS 8000    /* total {[( opens per script: bounds the
                                      * total parser WORK (nodes emitted), not
                                      * just depth — a 40KB flat script with
                                      * thousands of blocks is parser load
                                      * even when nesting stays shallow */
#define PLUTO_SCAN_MAX_REGEXES 128   /* regex literals per script: each regex
                                      * compiles through js_regcompx (the
                                      * engine's regexp compiler); bound the
                                      * count so one script cannot queue a
                                      * long chain of compiles (google.com:
                                      * 16 regexes across 10 scripts) */
static int pluto_script_compile_safe_ex(const char *src, size_t len,
                                        int max_depth)
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
    return maxDepth <= max_depth;
}

int jsbridge_script_compile_safe(const char *src, size_t len)
{
    return pluto_script_compile_safe_ex(src, len, PLUTO_SCAN_MAX_DEPTH);
}

int jsbridge_script_compile_safe_ex(const char *src, size_t len,
                                    int max_depth)
{
    return pluto_script_compile_safe_ex(src, len, max_depth);
}

unsigned long jsbridge_source_key(const char *src, size_t len,
                                  unsigned long salt)
{
    if (!src || len == 0)
    {
        return 0;
    }
    /* FNV-1a 32-bit, salt folded in first (byte-spreading, cheap on M7). */
    unsigned long h = 2166136261UL ^ salt;
    for (size_t i = 0; i < len; i++)
    {
        h ^= (unsigned char)src[i];
        h *= 16777619UL;
    }
    return h;
}

/* ── document.write output → live DOM (deep copy across arenas) ─────────
 * Iterative copy (explicit frame stack, heap-allocated). The previous
 * recursion consumed one C frame per markup nesting level — document.write
 * output is untrusted page content and can nest arbitrarily deep, so this
 * ran on the device's 61.8KB game-task stack. `depth` now bounds the COPY
 * tree height (runaway markup guard, same 24-level contract); the frame
 * stack grows on the heap beyond it.
 * Returns 0 ok, -1 on any failure (guard exceeded / alloc failure). */
static int adopt_nodes(JsBridge *b, DomNode *dstParent, const DomNode *src,
                       int depth)
{
    typedef struct
    {
        DomNode *dst;        /* node under which the level's children are adopted */
        const DomNode *src;  /* source node whose children are being copied */
        int nextChild;
        int depth;
    } AdoptFrame;
    if (depth > 24)
    {
        return -1; /* runaway markup guard (top-level contract preserved) */
    }
    if (!b || !dstParent || !src)
    {
        return -1;
    }
    int cap = 16;
    int top = 0;
    AdoptFrame *st = (AdoptFrame *)JMalloc(sizeof(AdoptFrame) * (size_t)cap);
    if (!st)
    {
        return -1;
    }
    st[top].dst = dstParent;
    st[top].src = src;
    st[top].nextChild = 0;
    st[top].depth = depth;
    top++;
    int rc = 0;
    while (top > 0 && rc == 0)
    {
        AdoptFrame *f = &st[top - 1];
        if (f->nextChild >= f->src->childCount)
        {
            top--;
            continue;
        }
        const DomNode *c = f->src->children[f->nextChild++];
        DomNode *copy = NULL;
        if (c->kind == DOM_ELEMENT)
        {
            copy = dom_create_element(b->dom, c->tag);
            if (!copy)
            {
                rc = -1;
                break;
            }
            for (int a = 0; a < c->attrCount; a++)
            {
                const char *v = c->attrs[a].value;
                if (dom_set_attr(b->dom, copy, c->attrs[a].key,
                                 v ? v : "") != 0)
                {
                    rc = -1;
                    break;
                }
            }
        }
        else
        {
            copy = dom_create_text(b->dom, c->text ? c->text : "");
            if (!copy)
            {
                rc = -1;
                break;
            }
        }
        if (rc == 0 && dom_append_child(b->dom, f->dst, copy) != 0)
        {
            rc = -1;
            break;
        }
        if (rc == 0 && c->childCount > 0)
        {
            int d = f->depth + 1;
            if (d > 24)
            {
                rc = -1; /* runaway markup guard (per-subtree, same contract) */
                break;
            }
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                AdoptFrame *grown = (AdoptFrame *)JMalloc(
                    sizeof(AdoptFrame) * (size_t)ncap);
                if (!grown)
                {
                    rc = -1;
                    break;
                }
                memcpy(grown, st, sizeof(AdoptFrame) * (size_t)top);
                JFree(st);
                st = grown;
                cap = ncap;
                f = &st[top - 1]; /* the buffer may have moved */
            }
            st[top].dst = copy;
            st[top].src = c;
            st[top].nextChild = 0;
            st[top].depth = d;
            top++;
        }
    }
    JFree(st);
    return rc;
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

/* ── SW5: element.innerHTML ASSIGNMENT (router-owned; engines call this) ──
 * Parses the markup into a THROWAWAY DomResult (full tokenizer → builder
 * pipeline — the same one document.write uses), then adopts the surviving
 * nodes under `el` after dropping ALL of its current children. This is the
 * first innerHTML with real markup semantics (previously every bridge
 * degraded the WRITE to textContent).
 * Device-safety: the scratch DomResult is heap-owned and freed on every
 * path; node counts are bounded by the builder's MAX_NODES cap and depth
 * by adopt_nodes' runaway guard; the mutation costs 1 budget unit + 1 per
 * adopted node (a 512-budget burst cannot exceed the page's budget).
 * Returns 0 ok, -1 bad args / tokenize+build failure (engines throw a
 * contained error, page continues). */
int jsbridge_el_set_inner_html(JsBridge *b, void *elp, const char *html,
                               size_t len)
{
    DomNode *el = (DomNode *)elp;
    if (!b || !b->dom || !el || el->kind != DOM_ELEMENT || !html)
    {
        return -1;
    }
    if (len > JSBRIDGE_INNER_HTML_MAX)
    {
        return -1;
    }
    if (!budget_take(b))
    {
        return -1;
    }
    /* Empty string = clear (still legal innerHTML). */
    for (int i = 0; i < el->childCount; i++)
    {
        el->children[i]->parent = NULL; /* detached (JS may hold wrappers) */
        if (!budget_take(b))
        {
            return -1;
        }
    }
    el->childCount = 0;
    if (len == 0)
    {
        return 0;
    }
    char *buf = (char *)JMalloc(len + 1);
    if (!buf)
    {
        return -1;
    }
    memcpy(buf, html, len);
    buf[len] = '\0';
    TokenizeResult tr;
    DomResult scratch;
    int rc = -1;
    if (tokenizer_tokenize(buf, &tr) == 0)
    {
        memset(&scratch, 0, sizeof(scratch));
        if (dom_build(&tr, &scratch) == 0 && scratch.root)
        {
            rc = adopt_nodes(b, el, scratch.root, 0);
            dom_free_result(&scratch);
        }
        tokenizer_free_result(&tr);
    }
    JFree(buf);
    return rc;
}

/* ── SW5: querySelector(All) shared walk (O3) ─────────────────────────────
 * The CSS compound grammar is reused through css_parse_selector; matching
 * runs over the live subtree via dom_query_selector_*. Engines push wrapper
 * elements through their own push_element, so the emit callback carries a
 * tiny engine-tagged slot: QS_EMIT_PUSH (muJS/Duktape/XS push straight onto
 * the engine stack; QuickJS needs the value RETAINED — its emit allocates
 * the wrapper into st->qsel[] pins). One scratch buffer per call site; a
 * selector that parses to 0 compounds returns "no results" (null/empty
 * array — browsers throw for invalid selectors, we degrade, contained). */
int jsbridge_el_query_selector_all(JsBridge *b, void *elp, const char *sel,
                                   char *scratch, size_t scratchSize,
                                   int (*emit)(void *el, void *ud),
                                   void *ud)
{
    DomNode *el = (DomNode *)elp;
    if (!b || !b->dom)
    {
        return -1;
    }
    if (!budget_take(b))
    {
        return -1;
    }
    return dom_query_selector_all(b->dom, el, sel, scratch, scratchSize,
                                  (int (*)(DomNode *, void *))emit, ud);
}

void *jsbridge_el_query_selector_first(JsBridge *b, void *elp, const char *sel,
                                       char *scratch, size_t scratchSize)
{
    DomNode *el = (DomNode *)elp;
    if (!b || !b->dom)
    {
        return NULL;
    }
    if (!budget_take(b))
    {
        return NULL;
    }
    return dom_query_selector_first(b->dom, el, sel, scratch, scratchSize);
}

/* ── SW5: classList (O4) — router-owned so every engine gets the same
 * token semantics; a mutation charges 1 budget unit (CSS re-application
 * happens on the standard rewalk). */
int jsbridge_el_class_has(void *elp, const char *token)
{
    return dom_class_has((DomNode *)elp, token);
}

int jsbridge_el_class_add(JsBridge *b, void *elp, const char *token)
{
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    return dom_class_add(b->dom, (DomNode *)elp, token);
}

int jsbridge_el_class_remove(JsBridge *b, void *elp, const char *token)
{
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    return dom_class_remove(b->dom, (DomNode *)elp, token);
}

int jsbridge_el_class_toggle(JsBridge *b, void *elp, const char *token)
{
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    return dom_class_toggle(b->dom, (DomNode *)elp, token);
}

/* ── SW5: ES5-compatible Set / Map / Image shims (matrix old.reddit class)
 * * Pure-JS except the bare-minimum ES5 fallbacks, injected as a PREFIX
 * before every page script by run_script (all engines): any engine missing
 * a builtin gets a working-enough ES5 implementation; engines that HAVE
 * the builtin are untouched (the prefix defines nothing when the global
 * already exists). Kept tiny: this closes ReferenceError crashes on modern
 * bundles that construct Set/Map at load time — it is NOT a full ES6
 * collections implementation.
 * Image: constructor-only object (src property accepted, no decode) so
 * `new Image(); img.src = ...` preload patterns don't ReferenceError.
 * Size: ~1.4KB — prefixed per script, so it costs a few parse bytes per
 * script but no RAM residency beyond the engine's own parse. */
static const char JSBRIDGE_SW5_PREFIX[] =
    /* Set / Map: linear-array ES5 implementations. Keys are stored by
     * reference (===) which covers the object/string/number keying real
     * bundles use; size is a maintained property (defineProperty is not
     * dependable on the ES5 engines). */
    "if(typeof Set=='undefined'){var Set=function(){this._k=[];this.size=0;};"
    "Set.prototype.add=function(v){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===v)return this;this._k.push(v);this.size=this._k.length;"
    "return this;};"
    "Set.prototype.has=function(v){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===v)return true;return false;};"
    "Set.prototype['delete']=function(v){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===v){this._k.splice(i,1);this.size=this._k.length;"
    "return true;}return false;};"
    "Set.prototype.clear=function(){this._k=[];this.size=0;};"
    "Set.prototype.forEach=function(cb,self){var k=this._k.slice(0);"
    "for(var i=0;i<k.length;i++)cb.call(self,k[i],k[i],this);};"
    "Set.prototype.values=function(){return this._k.slice(0);};"
    "Set.prototype.keys=function(){return this._k.slice(0);};}\n"
    "if(typeof Map=='undefined'){var Map=function(){this._k=[];this._v=[];"
    "this.size=0;};"
    "Map.prototype.set=function(k,v){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===k){this._v[i]=v;return this;}this._k.push(k);"
    "this._v.push(v);this.size=this._k.length;return this;};"
    "Map.prototype.get=function(k){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===k)return this._v[i];return undefined;};"
    "Map.prototype.has=function(k){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===k)return true;return false;};"
    "Map.prototype['delete']=function(k){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===k){this._k.splice(i,1);this._v.splice(i,1);"
    "this.size=this._k.length;return true;}return false;};"
    "Map.prototype.clear=function(){this._k=[];this._v=[];this.size=0;};"
    "Map.prototype.forEach=function(cb,self){var a=this._k.slice(0);"
    "var b=this._v.slice(0);for(var i=0;i<a.length;i++)"
    "cb.call(self,b[i],a[i],this);};}\n"
    /* Image: constructor-only shim so `new Image(); img.src=...` preload
     * patterns construct cleanly (no decode — the walker never sees it). */
    "if(typeof Image=='undefined'){var Image=function(w,h){this.width=w||0;"
    "this.height=h||0;this.src='';};}\n";

size_t jsbridge_sw5_prefix(const char **prefixOut)
{
    if (prefixOut)
    {
        *prefixOut = JSBRIDGE_SW5_PREFIX;
    }
    return sizeof(JSBRIDGE_SW5_PREFIX) - 1;
}

/* ── engine lifecycle (router) ───────────────────────────────────────────── */
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
    b->xhrBudgetEpoch = ++g_xhrEpochSeq;
    xhr_session_bind(b); /* module-global HTTP session → this page */
    /* Engine selection comes from Settings ("Javascript Engine"): the
     * choice routes ALL of this page's scripts to exactly one engine (no
     * cross-execution, no fallback). */
    b->engine = (g_selectedEngine == JS_ENGINE_MUJS ||
                 g_selectedEngine == JS_ENGINE_DUKTAPE ||
                 g_selectedEngine == JS_ENGINE_QUICKJS ||
                 g_selectedEngine == JS_ENGINE_XS)
                    ? g_selectedEngine
                    : JS_ENGINE_MUJS;

    if (strbuf_init(&b->output) != 0)
    {
        JFree(b);
        return -1;
    }

    const JsEngineImpl *impl = js_bridge_impl(b);
    if (impl->init(b, doc->baseUrl) != 0)
    {
        strbuf_free(&b->output);
        JFree(b);
        return -1;
    }
    /* Attach-time proof: name the engine that will run THIS page, taken
     * from the live vtable (not the Settings value) so a routing mismatch
     * can never hide again. The matching close log is [js] close[<name>]. */
    logger_log("[jsbridge] page attach: %s (engine=%d)", impl->name,
               b->engine);

    /* ── Script discovery + execution order (ENGINE-AGNOSTIC) ── */
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
                    impl->run_script(b, slots[i].inlineStart,
                                     slots[i].inlineLen, i + 1);
                }
                else if (slots[i].extIndex == JS_SCRIPT_DATA)
                {
                    /* SW2d: data:-URL script — decode the payload straight
                     * out of the page HTML (percent-escapes or base64 per
                     * the mediatype params) and run it as an inline body.
                     * Transient decode buffer: allocated, run, freed — RAM
                     * holds one at a time, same as the spill materializer. */
                    char *dsrc = jsext_decode_data_script(
                        slots[i].inlineStart, slots[i].inlineLen);
                    if (dsrc)
                    {
                        size_t dlen = strlen(dsrc);
                        impl->run_script(b, dsrc, dlen, i + 1);
                        JFree(dsrc);
                        logger_log("[js] data: script ran (%zu bytes)", dlen);
                    }
                    else
                    {
                        logger_log("[js] data: script skip (decode failed) "
                                   "#%d", i);
                    }
                }
                else if (slots[i].extIndex >= 0 &&
                         slots[i].extIndex < doc->extScriptCount)
                {
                    JsExtScript *e =
                        &((JsExtScript *)doc->extScripts)[slots[i].extIndex];
                    if (e->body && e->len)
                    {
                        /* run_script copies the source before running, so
                         * the doc-owned body is never aliased. */
                        impl->run_script(b, e->body, e->len, i + 1);
                    }
                    else if (e->spill >= 0 && e->len)
                    {
                        /* SW2b: disk-resident source — materialize just-in-
                         * time (RAM holds one active script; disk holds the
                         * rest), run, release. Source-ceiling gating and
                         * short-read handling live in the materializer. */
                        char *src = jsext_materialize_spill_script(e);
                        if (src)
                        {
                            impl->run_script(b, src, e->len, i + 1);
                            JFree(src);
                            logger_log("[js] ext ran from disk: %s (%zu bytes)",
                                       e->url[0] ? e->url : "(url)", e->len);
                        }
                        else
                        {
                            logger_log("[js] ext skip (materialize failed): %s",
                                       e->url[0] ? e->url : "(url)");
                        }
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
                impl->run_script(b, starts[i], lens[i], i + 1);
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
            impl->run_script(b, starts[i], lens[i], i + 1);
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

int jsbridge_page_script(JsBridge *b, const char *src, size_t len)
{
    if (!b || !src || len == 0)
    {
        return -1;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl)
    {
        return -1;
    }
    impl->run_script(b, src, len, 0);
    return 0;
}

void js_doc_close(JsBridge *b)
{
    if (!b)
    {
        return;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    logger_log("[js] close[%s]: ran=%d errs=%d listeners=%d timers=%d "
               "budgetLeft=%d%s%s",
               impl ? impl->name : "?", b->ran, b->errs, b->listenerCount,
               b->timerCount, b->callBudget,
               b->lastError[0] ? " last=" : "",
               b->lastError[0] ? b->lastError : "");
    /* Kill any in-flight XHR owned by this page BEFORE the engine dies;
     * http_cancel is stale-guarded, so a later callback is dropped. */
    if (b->xhrInFlight)
    {
        http_cancel();
        b->xhrInFlight = 0;
    }
    if (g_xhrBridge == b)
    {
        g_xhrBridge = NULL; /* context-free HTTP callbacks go stale */
        g_xhrActiveId = 0;
    }
    /* Release every still-held timer ref BEFORE the engine dies — live
     * timers AND inert slots with a deferred release (same contract as the
     * listener unref loop at close). */
    if (impl && impl->clear_timer_ref)
    {
        for (int i = 0; i < b->timerCount; i++)
        {
            JsTimer *t = &b->timers[i];
            if (t->active || t->release)
            {
                t->active = 0;
                t->release = 0;
                impl->clear_timer_ref(b, t->ref);
                t->ref = NULL;
            }
        }
        b->timerCount = 0;
    }
    /* XHR slots: release engine refs + free bodies (same contract). */
    if (impl && impl->clear_xhr_refs)
    {
        for (int i = 0; i < b->xhrCount; i++)
        {
            JsHttpRequest *r = &b->xhr[i];
            if (r->active || r->release)
            {
                impl->clear_xhr_refs(b, r->fnRef, r->objRef);
                if (r->body)
                {
                    JFree(r->body);
                }
                memset(r, 0, sizeof(*r));
            }
        }
        b->xhrCount = 0;
    }
    if (b->doc && b->doc->_jsbridge == b)
    {
        b->doc->_jsbridge = NULL; /* detach before the engine dies */
    }
    if (impl)
    {
        impl->close(b);
    }
    strbuf_free(&b->output);
    JFree(b);
}

/* ── JS timers (setTimeout / setInterval) — router-owned table ─────────────
 * STABLE-SLOT design: slots are NEVER moved while a callback can run.
 * clear()/timeout-completion only mark a slot inert (active=0); the engine
 * ref release is deferred to the next sweep when the clear happened inside
 * a callback (we may be inside an engine host bracket — releasing engine
 * refs mid-bracket is engine-specific risk we do not take). The sweep runs
 * at pump end (outside every bracket), compacting the table once. */

/* Public surface: engines call this from their setTimeout/setInterval
 * globals. fnRef ownership transfers to the TABLE on success (the engine
 * releases it via clear_timer_ref at sweep/close); on refusal (return 0)
 * ownership stays with the caller, which must unpin it. */
int jsbridge_timer_start(JsBridge *b, JsTimerKind kind, void *fnRef,
                         unsigned delayMs)
{
    if (!b || !fnRef)
    {
        return 0;
    }
    /* Reuse a fully-dead slot first (inert AND released); registration only
     * fails when JSBRIDGE_TIMERS_MAX timers are genuinely LIVE. */
    JsTimer *t = NULL;
    for (int i = 0; i < b->timerCount; i++)
    {
        if (!b->timers[i].active && !b->timers[i].release)
        {
            t = &b->timers[i];
            break;
        }
    }
    if (!t)
    {
        if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
        {
            logger_log("[js] timer refused (table full)");
            return 0;
        }
        t = &b->timers[b->timerCount++];
        memset(t, 0, sizeof(*t));
    }
    t->active = 1;
    t->release = 0;
    t->id = ++b->timerIdSeq; /* public ids never reused within a page */
    t->kind = kind;
    if (delayMs < JSBRIDGE_TIMER_MIN_MS)
    {
        delayMs = JSBRIDGE_TIMER_MIN_MS;
    }
    else if (delayMs > JSBRIDGE_TIMER_MAX_MS)
    {
        delayMs = JSBRIDGE_TIMER_MAX_MS;
    }
    t->intervalMs = delayMs;
    t->dueMs = 0; /* armed on first pump sight, from the live clock */
    t->gen = 0;
    t->fires = 0;
    t->ref = fnRef;
    logger_log("[js] timer #%d set %s %ums bridge=%p", t->id,
               kind == JS_TIMER_INTERVAL ? "interval" : "timeout", delayMs,
               (void *)b);
    return t->id;
}

/* Mark inert; release the engine ref now unless we are inside a callback
 * (deferred to the sweep — never release engine refs mid-bracket). */
int jsbridge_timer_clear(JsBridge *b, int id)
{
    if (!b || id <= 0)
    {
        return 0;
    }
    for (int i = 0; i < b->timerCount; i++)
    {
        JsTimer *t = &b->timers[i];
        if (t->active && t->id == id)
        {
            t->active = 0;
            if (b->inTimer || b->inClick)
            {
                t->release = 1; /* swept after the pump */
            }
            else
            {
                const JsEngineImpl *impl = js_bridge_impl(b);
                if (impl && impl->clear_timer_ref)
                {
                    impl->clear_timer_ref(b, t->ref);
                }
            }
            logger_log("[js] timer #%d cleared", id);
            return 1;
        }
    }
    return 0;
}

/* Saturating SDK-clock difference (wrap-safe): due-when in ms, >0 means due. */
static int jsbridge_timer_due(unsigned now, unsigned due)
{
    return (int)(now - due) > 0; /* unsigned wrap arithmetic */
}

/* Release one slot's engine ref (no-op when nothing is pending). */
static void jsbridge_timer_release(JsBridge *b, JsTimer *t)
{
    if (t->release && !t->active)
    {
        const JsEngineImpl *impl = js_bridge_impl(b);
        if (impl && impl->clear_timer_ref)
        {
            impl->clear_timer_ref(b, t->ref);
        }
        t->release = 0;
        t->ref = NULL;
    }
}

/* Retire a fired one-shot / capped interval: mark inert (release deferred
 * to the sweep — we may be mid-pump). */
static void jsbridge_timer_retire(JsBridge *b, JsTimer *t)
{
    (void)b;
    t->active = 0;
    t->release = 1;
}

/* Fire every due timer once. mutationsOut (may be NULL) is set when a
 * callback consumed DOM budget (caller re-renders). Returns fires executed.
 * ≤ JSBRIDGE_TIMER_BUDGET fires per call; nested pumps (a timer scheduling
 * another timer) are bounded by JSBRIDGE_TIMER_MAX_CHAIN. */
int jsbridge_timers_pump(JsBridge *b, unsigned nowMs, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->timerCount == 0 || b->inTimer)
    {
        return 0; /* nothing armed / nested call — stay silent */
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_timer_ref)
    {
        return 0;
    }
    int fires = 0;
    int mutations = 0;
    int budgetBefore = b->callBudget;
    b->timerChain++;
    if (b->timerChain > JSBRIDGE_TIMER_MAX_CHAIN)
    {
        b->timerChain--;
        return 0; /* nested-pump guard: skip this batch */
    }

    /* Iterate the STABLE slot array (live count only — see sweep note:
     * slots beyond timerCount hold STALE post-compaction bytes and MUST
     * NOT be visited, or a moved timer would fire from its ghost slot
     * too). Slots never move during the loop, so no callback-induced
     * shift can alias our cursor. Newly-registered timers land in dead
     * slots with dueMs==0 and are merely ARMED this pass — they can never
     * fire in the same pump that registered them (no burst loops). */
    for (int i = 0; i < b->timerCount; i++)
    {
        JsTimer *t = &b->timers[i];
        if (!t->active)
        {
            continue;
        }
        if (t->dueMs == 0)
        {
            t->dueMs = nowMs + t->intervalMs; /* arm; fires on a later pump */
            continue;
        }
        if (!jsbridge_timer_due(nowMs, t->dueMs))
        {
            continue;
        }
        if (fires >= JSBRIDGE_TIMER_BUDGET)
        {
            break; /* batch budget: the rest wait for the next pump */
        }
        fires++;
        t->gen++;
        logger_log("[js] timer #%d fire #%d %s now=%u due=%u active=%d bridge=%p",
                   t->id, t->fires + 1,
                   t->kind == JS_TIMER_INTERVAL ? "interval" : "timeout",
                   nowMs, t->dueMs, t->active, (void *)b);
        b->inTimer = 1;
        int rc = impl->run_timer_ref(b, t->ref);
        b->inTimer = 0;
        if (rc < 0)
        {
            /* Engine abort inside the callback: engine is dead — stop. */
            logger_log("[js] timer fire aborted the engine — stop pumping");
            b->timerChain--;
            if (mutationsOut)
            {
                *mutationsOut = mutations;
            }
            return fires;
        }
        if (b->callBudget < budgetBefore)
        {
            mutations = 1; /* callback consumed DOM budget → re-render */
            budgetBefore = b->callBudget;
        }
        /* The callback may have cleared ANY timer — but slots never moved,
         * so `t` is still this timer. Skip rescheduling if it self-cleared. */
        if (!t->active)
        {
            continue;
        }
        t->fires++;
        if (t->kind == JS_TIMER_TIMEOUT)
        {
            jsbridge_timer_retire(b, t); /* one-shot done */
        }
        else if (t->fires >= JSBRIDGE_TIMER_MAX_FIRES)
        {
            logger_log("[js] timer #%d hit the fires cap — cleared", t->id);
            jsbridge_timer_retire(b, t);
        }
        else
        {
            /* Reschedule with a catch-up guard: a page that stalled past a
             * whole period skips the missed beats (never burst-fires). */
            unsigned late = nowMs - t->dueMs;
            if (late > t->intervalMs)
            {
                t->dueMs = nowMs + t->intervalMs;
            }
            else
            {
                t->dueMs += t->intervalMs;
            }
        }
    }

    /* Sweep (outside every engine bracket): release pending refs, then
     * compact once. Engine ref release MUST NOT run inside a callback —
     * this is the only place clears/fires get their refs freed. */
    if (!b->inTimer && !b->inClick)
    {
        for (int i = 0; i < b->timerCount; i++)
        {
            jsbridge_timer_release(b, &b->timers[i]);
        }
        int w = 0;
        for (int r = 0; r < b->timerCount; r++)
        {
            if (b->timers[r].active || b->timers[r].release)
            {
                if (w != r)
                {
                    b->timers[w] = b->timers[r];
                }
                w++;
            }
        }
        /* WIPE the vacated tail: those bytes still read as live timers
         * (active=1 copies of moved slots). A future pump must never see
         * them, whatever bound it iterates with. */
        memset(&b->timers[w], 0, sizeof(b->timers[0]) *
                                       (size_t)(b->timerCount - w));
        b->timerCount = w;
    }
    b->timerChain--;
    if (mutationsOut)
    {
        *mutationsOut = mutations;
    }
    return fires;
}

/* ── XMLHttpRequest / fetch — router-owned table + single-flight wire ──────
 * Same architecture as the timer table: the ROUTER owns state + caps + the
 * HTTP session; engines only pin a completion handler + wrapper object and
 * read status/responseText through jsbridge_xhr_get(). Requests are one-shot
 * (browser XHR semantics): DONE settles the slot, the sweep frees its body
 * and releases engine refs outside every bracket. The SDK HTTP client is
 * single-flight — one request on the wire (xhrInFlight), further sends queue
 * and the pump starts them FIFO when idle.
 *
 * The HTTP client's callbacks are context-free, so the ACTIVE request is
 * tracked in module globals (the jsext.c pattern); every callback is
 * stale-guarded against session teardown/navigation. */

/* Called from js_doc_attach (new page) and js_doc_close (page gone). */
static void xhr_session_bind(JsBridge *b)
{
    g_xhrBridge = b;
    if (b && b->xhrBudgetEpoch != g_xhrBudgetPage)
    {
        g_xhrBudgetPage = b->xhrBudgetEpoch;
        g_xhrPageBudget = JSBRIDGE_XHR_PAGE_BUDGET;
    }
}

/* Progress sink: cut the socket when the response passes the body cap so a
 * huge file cannot flood the HTTP client's 2MB buffer (jsext pattern). */
static void xhr_on_progress(int cur, int total)
{
    (void)total;
    JsHttpRequest *r = NULL;
    if (!g_xhrBridge || !g_xhrActiveId)
    {
        return;
    }
    r = (JsHttpRequest *)jsbridge_xhr_get(g_xhrBridge, g_xhrActiveId);
    if (!r || !r->active)
    {
        return;
    }
    if (cur > JSBRIDGE_XHR_BODY_MAX)
    {
        logger_log("[js] xhr #%d too big (cut mid-stream, cap %d)", r->id,
                   JSBRIDGE_XHR_BODY_MAX);
        snprintf(r->err, sizeof(r->err), "response too large");
        http_cancel(); /* client zeroes its callbacks — no stray callbacks */
        r->state = JS_XHR_DONE;
        r->ok = 0;
        g_xhrActiveId = 0;
        if (g_xhrBridge)
        {
            g_xhrBridge->xhrInFlight = 0;
        }
    }
}

static void xhr_on_success(int status, char **keys, char **vals, int hc,
                           const char *body, size_t bodyLen, const char *url)
{
    (void)keys;
    (void)vals;
    (void)hc;
    (void)url;
    JsBridge *b = g_xhrBridge;
    if (!b || !g_xhrActiveId)
    {
        return; /* stale: page gone or request already aborted */
    }
    JsHttpRequest *r = (JsHttpRequest *)jsbridge_xhr_get(b, g_xhrActiveId);
    g_xhrActiveId = 0;
    b->xhrInFlight = 0;
    if (!r || !r->active || r->state == JS_XHR_DONE)
    {
        return;
    }
    r->state = JS_XHR_DONE;
    if (status < 200 || status >= 300)
    {
        r->ok = 0;
        snprintf(r->err, sizeof(r->err), "HTTP %d", status);
        logger_log("[js] xhr #%d FAIL status=%d", r->id, status);
        return;
    }
    if (!body || bodyLen == 0)
    {
        /* 2xx with an empty body is still a success (200 empty). */
        r->ok = 1;
        r->status = status;
        logger_log("[js] xhr #%d ok (empty body)", r->id);
        return;
    }
    if (bodyLen > (size_t)JSBRIDGE_XHR_BODY_MAX)
    {
        r->ok = 0;
        snprintf(r->err, sizeof(r->err), "response too large");
        logger_log("[js] xhr #%d FAIL too big (%zu bytes)", r->id, bodyLen);
        return;
    }
    if (bodyLen > g_xhrPageBudget)
    {
        r->ok = 0;
        snprintf(r->err, sizeof(r->err), "page budget exhausted");
        logger_log("[js] xhr #%d FAIL over page budget (%zu left)",
                   r->id, g_xhrPageBudget);
        return;
    }
    r->body = (char *)JMalloc(bodyLen + 1);
    if (!r->body)
    {
        r->ok = 0;
        snprintf(r->err, sizeof(r->err), "out of memory");
        logger_log("[js] xhr #%d FAIL OOM", r->id);
        return;
    }
    memcpy(r->body, body, bodyLen);
    r->body[bodyLen] = '\0';
    r->bodyLen = bodyLen;
    r->ok = 1;
    r->status = status;
    g_xhrPageBudget -= bodyLen;
    logger_log("[js] xhr #%d ok %s (%zu bytes, %zu budget left)", r->id,
               r->url, bodyLen, g_xhrPageBudget);
}

static void xhr_on_error(const char *message)
{
    JsBridge *b = g_xhrBridge;
    if (!b || !g_xhrActiveId)
    {
        return; /* stale */
    }
    JsHttpRequest *r = (JsHttpRequest *)jsbridge_xhr_get(b, g_xhrActiveId);
    g_xhrActiveId = 0;
    b->xhrInFlight = 0;
    if (!r || !r->active || r->state == JS_XHR_DONE)
    {
        return;
    }
    r->state = JS_XHR_DONE;
    r->ok = 0;
    snprintf(r->err, sizeof(r->err), "%.90s", message ? message : "network");
    logger_log("[js] xhr #%d FAIL network: %s", r->id,
               message ? message : "network");
}

/* Absolute-URL resolution shared with jsext (url_resolve mallocs via SDK). */
static char *xhr_resolve_url(JsBridge *b, const char *url)
{
    if (!url || !url[0])
    {
        return NULL;
    }
    const char *base = (b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl : NULL;
    char *abs = url_resolve(base ? base : "", url);
    if (!abs)
    {
        return NULL;
    }
    if (strlen(abs) >= JSBRIDGE_XHR_URL_MAX)
    {
        JFree(abs);
        return NULL;
    }
    return abs;
}

int jsbridge_xhr_open(JsBridge *b, const char *method, const char *url)
{
    if (!b)
    {
        return 0;
    }
    if (!method || (strcmp(method, "GET") != 0 && strcmp(method, "get") != 0))
    {
        bridge_take_error_text(b, "xhr: only GET is supported");
        return 0;
    }
    char *abs = xhr_resolve_url(b, url);
    if (!abs)
    {
        bridge_take_error_text(b, "xhr: bad url");
        return 0;
    }
    /* Reuse a fully-dead slot first (inert AND swept). */
    JsHttpRequest *r = NULL;
    for (int i = 0; i < b->xhrCount; i++)
    {
        if (!b->xhr[i].active && !b->xhr[i].release)
        {
            r = &b->xhr[i];
            break;
        }
    }
    if (!r)
    {
        if (b->xhrCount >= JSBRIDGE_XHR_MAX)
        {
            logger_log("[js] xhr refused (table full)");
            JFree(abs);
            return 0;
        }
        r = &b->xhr[b->xhrCount++];
    }
    int id = ++b->xhrIdSeq;
    memset(r, 0, sizeof(*r));
    r->active = 1;
    r->id = id;
    r->state = JS_XHR_OPENED;
    snprintf(r->url, sizeof(r->url), "%s", abs);
    JFree(abs);
    logger_log("[js] xhr #%d open %s", r->id, r->url);
    return r->id;
}

/* Start the actual fetch for an OPENED request. Returns 1 armed. Local
 * about: pages are answered WITHOUT the wire (synchronously settle DONE). */
static int xhr_start_fetch(JsBridge *b, JsHttpRequest *r)
{
    HttpCallbacks cbs;
    const char *local = NULL;
    size_t localLen = http_internal_page_body(r->url, &local);
    if (local)
    {
        /* No socket: settle immediately (mirrors the 20ms local answer the
         * page loader sees). Still capped + budgeted like a real response. */
        r->state = JS_XHR_HEADERS_SENT; /* transient, settled below */
        if (localLen > (size_t)JSBRIDGE_XHR_BODY_MAX ||
            localLen > g_xhrPageBudget)
        {
            r->state = JS_XHR_DONE;
            r->ok = 0;
            snprintf(r->err, sizeof(r->err), "response too large");
            logger_log("[js] xhr #%d local FAIL (cap/budget)", r->id);
        }
        else
        {
            r->body = (char *)JMalloc(localLen + 1);
            if (r->body)
            {
                memcpy(r->body, local, localLen);
                r->body[localLen] = '\0';
                r->bodyLen = localLen;
                r->ok = 1;
                r->status = 200;
                g_xhrPageBudget -= localLen;
                logger_log("[js] xhr #%d local ok (%zu bytes)", r->id,
                           localLen);
            }
            else
            {
                r->ok = 0;
                snprintf(r->err, sizeof(r->err), "out of memory");
            }
            r->state = JS_XHR_DONE;
        }
        return 1;
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.onSuccess = xhr_on_success;
    cbs.onError = xhr_on_error;
    cbs.onProgress = xhr_on_progress;
    g_xhrActiveId = r->id;
    b->xhrInFlight = r->id;
    r->state = JS_XHR_HEADERS_SENT;
    logger_log("[js] xhr #%d send %s", r->id, r->url);
    if (!http_get(r->url, &cbs))
    {
        /* Immediate refusal: http_get may or may not have fired onError
         * already; settle only if still unsettled (the callback path owns
         * the final state). */
        if (g_xhrActiveId == r->id)
        {
            logger_log("[js] xhr #%d immediate failure", r->id);
            r->state = JS_XHR_DONE;
            r->ok = 0;
            snprintf(r->err, sizeof(r->err), "request failed");
            g_xhrActiveId = 0;
            b->xhrInFlight = 0;
        }
        return 0;
    }
    return 1;
}

void jsbridge_xhr_send(JsBridge *b, int id, void *fnRef, void *objRef)
{
    if (!b || id <= 0)
    {
        return;
    }
    for (int i = 0; i < b->xhrCount; i++)
    {
        JsHttpRequest *r = &b->xhr[i];
        if (r->active && r->id == id)
        {
            if (r->state != JS_XHR_OPENED)
            {
                bridge_take_error_text(b, "xhr: already sent");
                return;
            }
            /* Take the refs up front — the completion needs them whether
             * the request starts now or queues behind the in-flight one. */
            r->fnRef = fnRef;
            r->objRef = objRef;
            if (b->xhrInFlight == 0)
            {
                xhr_start_fetch(b, r);
            }
            else
            {
                logger_log("[js] xhr #%d queued (wire busy)", id);
            }
            return;
        }
    }
}

void jsbridge_xhr_abort(JsBridge *b, int id)
{
    if (!b || id <= 0)
    {
        return;
    }
    for (int i = 0; i < b->xhrCount; i++)
    {
        JsHttpRequest *r = &b->xhr[i];
        if (r->active && r->id == id)
        {
            if (b->xhrInFlight == id)
            {
                /* http_cancel is stale-guarded (client zeroes its callbacks
                 * before tearing down) — any later callback for this
                 * generation is dropped. */
                http_cancel();
                b->xhrInFlight = 0;
                if (g_xhrActiveId == id)
                {
                    g_xhrActiveId = 0;
                }
            }
            if (r->state == JS_XHR_HEADERS_SENT || r->state == JS_XHR_OPENED)
            {
                r->state = JS_XHR_ABORTED;
                r->ok = 0;
                snprintf(r->err, sizeof(r->err), "aborted");
            }
            return;
        }
    }
}

const JsHttpRequest *jsbridge_xhr_get(JsBridge *b, int id)
{
    if (!b || id <= 0)
    {
        return NULL;
    }
    for (int i = 0; i < b->xhrCount; i++)
    {
        if (b->xhr[i].id == id)
        {
            return &b->xhr[i];
        }
    }
    return NULL;
}

int jsbridge_xhr_pump(JsBridge *b, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->inXhr || b->inTimer || b->inClick)
    {
        return 0;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_xhr_ref || !impl->clear_xhr_refs)
    {
        return 0;
    }
    /* Start the next queued send when the wire is idle. */
    if (b->xhrInFlight == 0)
    {
        for (int i = 0; i < b->xhrCount; i++)
        {
            JsHttpRequest *r = &b->xhr[i];
            if (r->active && r->state == JS_XHR_OPENED && r->fnRef)
            {
                xhr_start_fetch(b, r);
                break; /* single-flight: one per pump */
            }
        }
    }
    int delivered = 0;
    int mutations = 0;
    int budgetBefore = b->callBudget;
    for (int i = 0; i < b->xhrCount; i++)
    {
        JsHttpRequest *r = &b->xhr[i];
        if (!r->active || r->state != JS_XHR_DONE || !r->fnRef)
        {
            continue;
        }
        /* Slots are stable; a handler may settle/abort OTHER requests but
         * never reorders the table, so `r` stays valid across the call. */
        b->inXhr = 1;
        int rc = impl->run_xhr_ref(b, r->fnRef, r->objRef, r);
        b->inXhr = 0;
        if (rc < 0)
        {
            logger_log("[js] xhr completion aborted the engine — stop");
            break;
        }
        if (b->callBudget < budgetBefore)
        {
            mutations = 1;
            budgetBefore = b->callBudget;
        }
        r->active = 0;
        r->release = 1;
        delivered++;
    }
    /* Sweep (outside every bracket): release engine refs, free bodies,
     * compact once — identical discipline to the timer sweep. */
    if (!b->inXhr && !b->inTimer && !b->inClick)
    {
        for (int i = 0; i < b->xhrCount; i++)
        {
            JsHttpRequest *r = &b->xhr[i];
            if (r->release && !r->active)
            {
                impl->clear_xhr_refs(b, r->fnRef, r->objRef);
                if (r->body)
                {
                    JFree(r->body);
                }
                memset(r, 0, sizeof(*r));
            }
        }
        int w = 0;
        for (int rd = 0; rd < b->xhrCount; rd++)
        {
            if (b->xhr[rd].active || b->xhr[rd].release)
            {
                if (w != rd)
                {
                    b->xhr[w] = b->xhr[rd];
                }
                w++;
            }
        }
        /* Wipe the vacated tail (ghost-slot lesson from the timer table). */
        memset(&b->xhr[w], 0, sizeof(b->xhr[0]) * (size_t)(b->xhrCount - w));
        b->xhrCount = w;
    }
    if (mutationsOut)
    {
        *mutationsOut = mutations;
    }
    return delivered;
}

/* ── click dispatch (browser → page) ─────────────────────────────────────── */
int jsbridge_dispatch_link_click(JsBridge *b, const void *anchorNode)
{
    if (!b || b->inClick || !anchorNode)
    {
        return JSB_CLICK_NONE;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl)
    {
        return JSB_CLICK_NONE;
    }
    return impl->dispatch_click(b, anchorNode);
}

int jsbridge_listener_count(const JsBridge *b)
{
    return b ? b->listenerCount : 0;
}

int jsbridge_timers_active(const JsBridge *b)
{
    return b ? b->timerCount : 0;
}

int jsbridge_timers_pending(const JsBridge *b, unsigned nowMs)
{
    if (!b)
    {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < b->timerCount; i++)
    {
        if (b->timers[i].active && b->timers[i].dueMs != 0 &&
            jsbridge_timer_due(nowMs, b->timers[i].dueMs))
        {
            n++;
        }
    }
    return n;
}

/* ── Device link shims (router-owned; shared by both engines) ────────────── */
#ifdef TARGET_PLAYDATE
/* Engine panic paths land here only if an impl fails to contain an error;
 * both bridges contain every entry point, so this is a cold path.
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
