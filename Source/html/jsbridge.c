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
#include "pd_api.h" /* API-P5: module-lane file: fetches (kFileReadData) */
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

/* API-P5 loader-side module lane (defined near the script router below;
 * the attach loops route type="module" elements through it). */
static void jsbridge_module_root(JsBridge *b, const char *src, size_t len,
                                 const char *importBase, int index);

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
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
        logger_log("[budget] DENY live=%d", b->callBudget);
#endif
        return 0;
    }
    b->callBudget--;
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    if (b->callBudget < 20)
    {
        logger_log("[budget] low=%d", b->callBudget);
    }
#endif
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
                        engine == JS_ENGINE_XS ||
                        engine == JS_ENGINE_XS_NR)
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
#ifndef PLUTO_NO_STOCK_XS
extern const JsEngineImpl js_engine_xs;
#endif
extern const JsEngineImpl js_engine_xs_nr;

/* The simulator dylib compiles every engine in ONE translation pass, so
 * per-file -D isolation is impossible there: the sim build contains the
 * no-recursion fork ONLY (its xs_nr_rename.h renames every fork symbol),
 * and the STOCK XS engine is not linked at all. The stock engine remains
 * fully present in device builds. */
#ifdef TARGET_SIMULATOR
#define PLUTO_STOCK_XS_LINKED 0
#else
#define PLUTO_STOCK_XS_LINKED 1
#endif

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
#ifndef PLUTO_NO_STOCK_XS
    if (b->engine == JS_ENGINE_XS)
    {
        return &js_engine_xs;
    }
#endif
    if (b->engine == JS_ENGINE_XS_NR)
    {
        return &js_engine_xs_nr;
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

/* ── Data-type script filter (browser-faithful) ───────────────────────────
 * A <script> element whose `type=` attribute names a NON-executable MIME
 * (the JSON family, importmap, speculationrules) must NOT run: browsers
 * only execute a script when `type` is absent/empty or names JavaScript
 * (HTML spec, §4.12.1). The two "SyntaxError: missing ;" failures on
 * bryanwandrych.com/contact are exactly this — <script
 * type="application/ld+json"> bodies hold a bare JSON object literal,
 * which is a JS *block* statement, so every JS engine (browser-faithful
 * ones) rejects it; the browser just never executes the block at all.
 * Shared by BOTH script scanners (legacy inline extract_scripts here and
 * FULL-mode jsbridge_scan_scripts in jsext.c) so every engine and every
 * execution policy applies the same filter. Unknown-but-non-JSON types
 * are NOT skipped (today's engine runs everything else; a stricter
 * allow-list is a behavior change beyond this bug).
 * Returns 1 when the element must be SKIPPED as data, 0 to run. */
int jsbridge_script_type_is_data(const char *lt, const char *gt)
{
    if (!lt || !gt || lt >= gt)
    {
        return 0;
    }
    const char *p = lt;
    while (p + 4 <= gt)
    {
        if ((p[0] | 0x20) == 't' && (p[1] | 0x20) == 'y' &&
            (p[2] | 0x20) == 'p' && (p[3] | 0x20) == 'e')
        {
            /* Attribute name boundary: the previous char must separate an
             * attribute (whitespace or '/'), so words like data-type do
             * not match. */
            const char *prev = p - 1;
            if (p > lt && (*prev == ' ' || *prev == '\t' || *prev == '\n' ||
                           *prev == '\r' || *prev == '/'))
            {
                const char *v = p + 4;
                while (v < gt && (*v == ' ' || *v == '\t' || *v == '\n' ||
                                  *v == '\r'))
                {
                    v++;
                }
                if (v < gt && *v == '=')
                {
                    v++;
                    while (v < gt && (*v == ' ' || *v == '\t' || *v == '\n' ||
                                      *v == '\r'))
                    {
                        v++;
                    }
                    char q = 0;
                    if (v < gt && (*v == '"' || *v == '\''))
                    {
                        q = *v;
                        v++;
                    }
                    char buf[48];
                    size_t n = 0;
                    while (v < gt && n < sizeof(buf) - 1)
                    {
                        char c = *v;
                        if (q && c == q)
                        {
                            break;
                        }
                        if (!q && (c == ' ' || c == '\t' || c == '\n' ||
                                   c == '\r' || c == '>'))
                        {
                            break; /* bareword value end */
                        }
                        buf[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32
                                                                 : c);
                        v++;
                    }
                    /* Trim a bare value's trailing '/' (e.g. type=x/>). */
                    while (n > 0 && buf[n - 1] == '/')
                    {
                        n--;
                    }
                    buf[n] = '\0';
                    if (!buf[0])
                    {
                        return 0; /* empty type ⇒ JavaScript */
                    }
                    if (strstr(buf, "javascript"))
                    {
                        return 0; /* any JS MIME never counts as data */
                    }
                    size_t len = strlen(buf);
                    if (len >= 5 && strcmp(buf + len - 5, "+json") == 0)
                    {
                        return 1; /* RFC 8259 JSON family (ld+json etc.) */
                    }
                    if (strcmp(buf, "application/json") == 0 ||
                        strcmp(buf, "text/json") == 0 ||
                        strcmp(buf, "application/x-json") == 0 ||
                        strcmp(buf, "importmap") == 0 ||
                        strcmp(buf, "speculationrules") == 0)
                    {
                        return 1;
                    }
                    return 0; /* unknown type: keep today's run-everything */
                }
            }
        }
        p++;
    }
    return 0; /* no type attribute ⇒ JavaScript */
}

/* API-P5: the module marker, walking the SAME attr shape type_is_data
 * walks (own copy of the walk so the proven data filter stays byte-for-
 * byte untouched; the two rules travel together in both scanners).
 * Returns 1 when this <script> element's type= reads "module" (any
 * case) — the body/src then routes through the loader-side module
 * lane, not the engine's classic run. */
int jsbridge_script_type_is_module(const char *lt, const char *gt)
{
    if (!lt || !gt || lt >= gt)
    {
        return 0;
    }
    const char *p = lt;
    while (p + 4 <= gt)
    {
        if ((p[0] | 0x20) == 't' && (p[1] | 0x20) == 'y' &&
            (p[2] | 0x20) == 'p' && (p[3] | 0x20) == 'e')
        {
            const char *prev = p - 1;
            if (p > lt && (*prev == ' ' || *prev == '\t' || *prev == '\n' ||
                           *prev == '\r' || *prev == '/'))
            {
                const char *v = p + 4;
                while (v < gt && (*v == ' ' || *v == '\t' || *v == '\n' ||
                                  *v == '\r'))
                {
                    v++;
                }
                if (v < gt && *v == '=')
                {
                    v++;
                    while (v < gt && (*v == ' ' || *v == '\t' || *v == '\n' ||
                                      *v == '\r'))
                    {
                        v++;
                    }
                    char q = 0;
                    if (v < gt && (*v == '"' || *v == '\''))
                    {
                        q = *v;
                        v++;
                    }
                    char buf[16];
                    size_t n = 0;
                    while (v < gt && n < sizeof(buf) - 1)
                    {
                        char c = *v;
                        if (q && c == q)
                        {
                            break;
                        }
                        if (!q && (c == ' ' || c == '\t' || c == '\n' ||
                                   c == '\r' || c == '>'))
                        {
                            break;
                        }
                        buf[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                        v++;
                    }
                    while (n > 0 && buf[n - 1] == '/')
                    {
                        n--;
                    }
                    buf[n] = '\0';
                    return strcmp(buf, "module") == 0;
                }
            }
        }
        p++;
    }
    return 0; /* no type attribute ⇒ classic (never module) */
}

static int extract_scripts(const char *html, const char **starts, size_t *lens,
                           int max, const char **tags, size_t *tagLens)
{
    const char *pos = html;
    const char *end = html + strlen(html);
    int count = 0;
#ifdef TARGET_PLAYDATE
    /* R-kf bisect: two watchdog snapshots froze with lr inside this
     * function's strlen/type-check callees — log the stack depth at entry
     * so the next snapshot can be reconciled. */
    extern unsigned logger_stack_depth(void);
    logger_log("[js] probe: extract enter depth=%u",
               logger_stack_depth());
#endif

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
        /* Data-type scripts (JSON-LD, importmap …) are not executable
         * JavaScript — browsers never run them (HTML spec §4.12.1). Skipping
         * mirrors the src= handling: no body captured, never counted. */
        if (jsbridge_script_type_is_data(lt, gt))
        {
            const char *close = find_from(gt + 1, end, "</script", 8);
            if (!close)
            {
                return count;
            }
            pos = close + 8;
            goto next;
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
                if (tags && tagLens)
                {
                    /* A2: open-tag span for document.currentScript. */
                    tags[count] = lt;
                    tagLens[count] = (size_t)(gt - lt + 1);
                }
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

/* ── A2: current-script context (browser-faithful document.currentScript) ──
 * The ROUTER owns script execution order: it stamps which <script> open
 * tag (plus any resolved src URL) is executing before handing the script to
 * the impl, and clears the context after the run returns. Engines model the
 * browser API off this (the NR engine synthesizes a script element whose
 * getAttribute reads the stamped attrs); browsers expose null outside a
 * script run, so dynamic code (timers, import(), page_script) sees null. */
static void jsbridge_cur_script_clear(JsBridge *b)
{
    b->curTagStart = NULL;
    b->curTagLen = 0;
    b->curSrcUrl = NULL;
}

static void jsbridge_cur_script_stamp(JsBridge *b, const JsScriptSlot *slot,
                                      const JsExtScript *e)
{
    jsbridge_cur_script_clear(b);
    if (!slot)
    {
        return;
    }
    /* data:-URL slots carry no JsExtScript; browsers expose the raw data:
     * URL as currentScript.src — the slot's inlineStart IS that value
     * (SW2d), so passing it keeps parity there too. */
    b->curSrcUrl = (slot->isExt && e) ? e->url
                   : (slot->isExt && slot->extIndex == JS_SCRIPT_DATA)
                       ? slot->inlineStart
                       : NULL;
    /* A2: remember the newest EXTERNAL chunk URL across clears — registry
     * drains run during inline slots (curSrcUrl NULL there) and bundler
     * loaders fall back to this for src-based asset-prefix discovery. The
     * ext list lives for the whole page run, so the pointer stays valid. */
    if (slot->isExt && e)
    {
        b->lastExtSrcUrl = e->url;
    }
    if (slot->tagStart && slot->tagLen)
    {
        b->curTagStart = slot->tagStart;
        b->curTagLen = slot->tagLen;
    }
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
    return jsbridge_el_set_inner_html_ex(b, elp, html, len,
                                         JSBRIDGE_INNER_HTML_MAX);
}

/* The _ex variant takes the caller's cap (XS (No Recursion) bridge passes
 * a document-sized one; the shared router keeps 16KB for every other
 * engine — frozen behavior byte-identical). */
int jsbridge_el_set_inner_html_ex(JsBridge *b, void *elp, const char *html,
                                  size_t len, size_t maxLen)
{
    DomNode *el = (DomNode *)elp;
    if (!b || !b->dom || !el || el->kind != DOM_ELEMENT || !html)
    {
        return -1;
    }
    if (len > maxLen)
    {
        return -1;
    }
    if (!budget_take(b))
    {
        return -1;
    }
    /* Empty string = clear (still legal innerHTML). Trace EVERY clear
     * and every dropped child: this path is the only silent whole-subtree
     * mutation react's fallback client render can reach, and the
     * hydration debug needs the caller's footprint per kid. */
    for (int i = 0; i < el->childCount; i++)
    {
        el->children[i]->parent = NULL; /* detached (JS may hold wrappers) */
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
        logger_log("[dom] innerHTML clear parent=%s(%p) child=%s kind=%d",
                   (el->tag) ? el->tag : "?",
                   (void *)el,
                   (el->children[i]->kind == DOM_ELEMENT &&
                    el->children[i]->tag)
                       ? el->children[i]->tag
                       : "(text)",
                   (int)el->children[i]->kind);
#endif
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

/* ── API-P6: Element.closest — router-owned like the querySelector walk
 * (one budget, one parse, one matcher); the element itself is tested
 * first, then element ancestors up the tree. NULL for no match and for
 * an unusable selector — engines surface both as null. */
void *jsbridge_el_closest(JsBridge *b, void *elp, const char *sel,
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
    return dom_closest(b->dom, el, sel, scratch, scratchSize);
}

/* ── SW5: classList (O4) — router-owned so every engine gets the same
 * token semantics; a mutation charges 1 budget unit (CSS re-application
 * happens on the standard rewalk). */
int jsbridge_el_class_has(void *elp, const char *token)
{
    return dom_class_has((DomNode *)elp, token);
}

/* ── Phase 10: classList change-decorating (transition lane + R58 parity) ──
 * R58 escalted plain appendChild batches to domDirty; the classList
 * methods were left on the budget heuristic. They are live-tree
 * mutations too: a class flip changes computed style, so both the
 * rewalk latch and the transition-lane note ride HERE — but only when
 * the attribute really changed (a no-op add/remove must not fire a
 * transition; browser parity). */
static int cls_before(DomNode *el, char *buf, int cap)
{
    buf[0] = '\0';
    if (!el)
    {
        return 0;
    }
    const char *cls = dom_get_attr(el, "class");
    if (!cls || !cls[0])
    {
        return 0;
    }
    if ((int)strlen(cls) >= cap)
    {
        return -1; /* over-cap: treat as changed (conservative) */
    }
    snprintf(buf, (size_t)cap, "%s", cls);
    return 0;
}

static void cls_after(JsBridge *b, DomNode *el, const char *before)
{
    if (!b || !el)
    {
        return;
    }
    const char *cls = dom_get_attr(el, "class");
    if (cls && cls[0] && strcmp(cls, before) != 0)
    {
        b->domDirty = 1; /* R58 parity: the frame loop must re-anchor */
        jsbridge_note_dom_mutation(b, el); /* transition-lane note */
    }
}

int jsbridge_el_class_add(JsBridge *b, void *elp, const char *token)
{
    char before[192];
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    cls_before((DomNode *)elp, before, sizeof(before));
    int rc = dom_class_add(b->dom, (DomNode *)elp, token);
    if (rc >= 0)
    {
        cls_after(b, (DomNode *)elp, before);
    }
    return rc;
}

int jsbridge_el_class_remove(JsBridge *b, void *elp, const char *token)
{
    char before[192];
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    cls_before((DomNode *)elp, before, sizeof(before));
    int rc = dom_class_remove(b->dom, (DomNode *)elp, token);
    if (rc >= 0)
    {
        cls_after(b, (DomNode *)elp, before);
    }
    return rc;
}

int jsbridge_el_class_toggle(JsBridge *b, void *elp, const char *token)
{
    char before[192];
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    cls_before((DomNode *)elp, before, sizeof(before));
    int rc = dom_class_toggle(b->dom, (DomNode *)elp, token);
    if (rc >= 0)
    {
        cls_after(b, (DomNode *)elp, before);
    }
    return rc;
}

/* API-P9: className WRITE — the whole attribute replaced (browser shape:
 * el.className = 'a b' is setAttribute('class', 'a b') exactly). Same
 * budget + snapshot/dirty-receipt pair as the token mutations. */
int jsbridge_el_class_set(JsBridge *b, void *elp, const char *value)
{
    char before[192];
    if (!b || !b->dom || !budget_take(b))
    {
        return -1;
    }
    cls_before((DomNode *)elp, before, sizeof(before));
    int rc = dom_set_attr(b->dom, (DomNode *)elp, "class", value ? value : "");
    if (rc >= 0)
    {
        cls_after(b, (DomNode *)elp, before);
    }
    return rc;
}

/* ── SW5: ES5-compatible shims (Set / Map / Image / dataset /
 * ReadableStream) * Pure-JS except the bare-minimum ES5 fallbacks,
 * injected as a PREFIX before every page script by run_script (all
 * engines): any engine missing a builtin gets a working-enough ES5
 * implementation; engines that HAVE the builtin are untouched (the
 * prefix defines nothing when the global already exists). Kept tiny:
 * this closes ReferenceError crashes on modern bundles that construct
 * Set/Map at load time — it is NOT a full ES6 collections
 * implementation.
 * Image: constructor-only object (src property accepted, no decode) so
 * `new Image(); img.src = ...` preload patterns don't ReferenceError.
 * dataset: live getAttribute-backed accessor map on documentElement's
 * prototype (camelized data-* keys; unknown keys are plain property
 * misses, like the browser) so bootstrap detection reading
 * `documentElement.dataset.<key>` behaves like the browser on ES5
 * engines.
 * ReadableStream: minimal start/pull/getReader surface for RSC
 * bootstrap (constructor calls start(ctl) once; ctl.enqueue/close/
 * error; stream consumed via getReader().read() promise loops) plus
 * async-pull wrapper sources; Promise-guarded (engines without
 * Promise leave the global untouched).
 * Size: ~3.7KB — prefixed per script, so it costs a few parse bytes
 * per script but no RAM residency beyond the engine's own parse. */
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
    "this.height=h||0;this.src='';};}\n"
    /* A2 (TextEncoder/TextDecoder): framework boots construct one during
     * module init (Next.js App Router builds its RSC payload pipe with
     * `new TextEncoder` before its DOMContentLoaded wait) — a missing
     * constructor killed the boot inside the loader's swallow. encode()
     * emits real UTF-8 (surrogate-pair aware); decode() converts back,
     * re-pairing astral code points. Typed arrays are native (XS/Duktape
     * ship them); engines without typed arrays only hit this shim when a
     * page actually calls encode, which they could not fulfill before
     * either — guarded defines, has-builtin engines are untouched. */
    "if(typeof TextEncoder=='undefined'){var TextEncoder=function(){};"
    /* A2c: UTF-8 buckets were shifted one octave (0x800-0xFFFF emitted
     * 2 bytes with an invalid 0xC0 lead — "Bryan Wandrych — Portfolio"
     * round-tripped a raw 0x14 and XS's JSON.parse rejected the flight
     * row "1: invalid character"). Correct ladder: 1 | 2 | 3 | 4 bytes.
     * The engine's JSON lexer accepts this — same bytes the server sent. */
    "TextEncoder.prototype.encode=function(s){s=String(s);var out=[];"
    "for(var i=0;i<s.length;i++){var c=s.charCodeAt(i);"
    "if(c>=0xD800&&c<=0xDBFF&&i+1<s.length){var d=s.charCodeAt(i+1);"
    "if(d>=0xDC00&&d<=0xDFFF){c=0x10000+((c-0xD800)<<10)+(d-0xDC00);i++;}}"
    "if(c>=0x10000){out.push(0xF0|((c>>18)&63),0x80|((c>>12)&63),"
    "0x80|((c>>6)&63),0x80|(c&63));}"
    "else if(c>=0x800){out.push(0xE0|((c>>12)&15),0x80|((c>>6)&63),"
    "0x80|(c&63));}"
    "else if(c>=0x80){out.push(0xC0|((c>>6)&31),0x80|(c&63));}"
    "else{out.push(c);}}"
    "return new Uint8Array(out);};"
    "TextEncoder.prototype.encodeInto=function(s,a){var u=this.encode(s);"
    "var n=u.length<a.length?u.length:a.length;"
    "for(var i=0;i<n;i++)a[i]=u[i];return{read:n,written:n};};}\n"
    "if(typeof TextDecoder=='undefined'){var TextDecoder=function(){};"
    /* A2b: decode() must NOT build per-byte with s += fromCharCode — each
     * += re-copies the whole accumulator, O(n^2) metered work (the RSC
     * flight row resolver feeds this several-KB rows and the run trips
     * XS's computation watchdog). Collect UTF-16 units in a plain array,
     * then materialize the string in 8192-unit chunks — at most
     * n/8192 concat copies instead of n. */
    "TextDecoder.prototype.decode=function(u){if(!u)return '';"
    "var n=u.length,units=[];"
    "for(var i=0;i<n;){var c=u[i++];"
    "if(c>=0xF0){c=((c&7)<<18)|((u[i++]&63)<<12)|((u[i++]&63)<<6)|(u[i++]&63);}"
    "else if(c>=0xE0){c=((c&15)<<12)|((u[i++]&63)<<6)|(u[i++]&63);}"
    "else if(c>=0xC0){c=((c&31)<<6)|(u[i++]&63);}"
    "if(c>=0x10000){units.push(0xD800+(c>>10),0xDC00+(c&1023));}"
    "else{units.push(c);}}"
    "var s='',total=units.length;"
    "for(var j=0;j<total;j+=8192)s+=String.fromCharCode.apply(null,"
    "j+8192<=total?units.slice(j,j+8192):units.slice(j));"
    "return s;};}\n"
    /* A2 (URLSearchParams/queueMicrotask/atob/btoa): boot-critical globals
     * of modern loaders — Next.js reads URLSearchParams while resolving
     * its router state and schedules its RSC consumer via
     * queueMicrotask/setTimeout; RSC payloads carry their base64 entries
     * through atob/btoa (array包头 enforcement is thin here — string
     * handling only, which is what b64 payloads need). Guarded defines. */
    "if(typeof URLSearchParams=='undefined'){var URLSearchParams=function(q){"
    "this._k=[];this._v=[];"
    "if(q)if(typeof q=='string'){var a=q.split('&');"
    "for(var i=0;i<a.length;i++){var kv=a[i].split('='),n=kv[0],v=kv[1];"
    "if(n||kv.length>1){this._k.push(String(n).replace(/\\+/g,' '));"
    "this._v.push(v===undefined?'':String(v).replace(/\\+/g,' '));}}}"
    "else{for(var k2 in q)if(Object.prototype.hasOwnProperty.call(q,k2)){"
    "this._k.push(String(k2));this._v.push(String(q[k2]));}}};"
    "URLSearchParams.prototype.append=function(k,v){this._k.push(String(k));"
    "this._v.push(String(v));};"
    "URLSearchParams.prototype.get=function(k){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===String(k))return this._v[i];return null;};"
    "URLSearchParams.prototype.getAll=function(k){var o=[];"
    "for(var i=0;i<this._k.length;i++)if(this._k[i]===String(k))o.push(this._v[i]);"
    "return o;};"
    "URLSearchParams.prototype.has=function(k){for(var i=0;i<this._k.length;i++)"
    "if(this._k[i]===String(k))return true;return false;};"
    "URLSearchParams.prototype.set=function(k,v){k=String(k);v=String(v);"
    "var seen=0;for(var i=0;i<this._k.length;i++)if(this._k[i]===k){"
    "if(seen){this._k.splice(i,1);this._v.splice(i,1);i--;continue;}"
    "this._v[i]=v;seen=1;}"
    "if(!seen){this._k.push(k);this._v.push(v);}};"
    "URLSearchParams.prototype['delete']=function(k){k=String(k);"
    "for(var i=0;i<this._k.length;i++)if(this._k[i]===k){"
    "this._k.splice(i,1);this._v.splice(i,1);i--;}};"
    "URLSearchParams.prototype.forEach=function(cb,self){"
    "for(var i=0;i<this._k.length;i++)cb.call(self,this._v[i],this._k[i],this);};"
    "URLSearchParams.prototype.toString=function(){var a='';"
    "for(var i=0;i<this._k.length;i++)a+=(a.length?'&':'')+this._k[i]+'='+this._v[i];"
    "return a;};}\n"
    "if(typeof queueMicrotask=='undefined'){var queueMicrotask=function(fn){"
    "return Promise.resolve().then(fn);};}\n"
    "if(typeof btoa=='undefined'){var btoa=function(s){"
    "var T='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';"
    "s=String(s);var o='';for(var i=0;i<s.length;i+=3){var a=s.charCodeAt(i),"
    "b=s.charCodeAt(i+1),c=s.charCodeAt(i+2);"
    "o+=T[a>>2]+T[((a&3)<<4)|(isNaN(b)?0:(b>>4))]+"
    "(isNaN(b)?'=':T[((b&15)<<2)|(isNaN(c)?0:(c>>6))])+"
    "(isNaN(c)?'=':T[c&63]);}"
    "return o;};}\n"
    "if(typeof atob=='undefined'){var atob=function(s){"
    "var T='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';"
    "s=String(s).replace(/[^A-Za-z0-9+\\/=]/g,'');var D={};"
    "for(var z=0;z<T.length;z++)D[T[z]]=z;var o='';"
    "for(var i=0;i<s.length;i+=4){var a=D[s.charAt(i)],b=D[s.charAt(i+1)];"
    "if(a===undefined||b===undefined)return o;"
    "var c2=s.charAt(i+2),d2=s.charAt(i+3);"
    "var c=c2==='='?0:D[c2],d=d2==='='?0:D[d2];"
    "o+=String.fromCharCode((a<<2|(b>>4))&255);"
    "if(c2!=='=')o+=String.fromCharCode(((b<<4)|(c>>2))&255);"
    "if(d2!=='=')o+=String.fromCharCode(((c<<6)|d)&255);}"
    "return o;};}\n"
    /* SW5 (HTMLElement.dataset): RSC boots read deployment markers via
     * `document.documentElement.dataset.dplId` during module init. With no
     * DOMStringMap the receiver is undefined and reading ANY dataset key
     * throws (`cannot coerce undefined to object`), killing the module
     * tree inside the loader's swallow — in browsers this is a plain
     * property miss returning undefined. The getter builds a map whose
     * known `data-*` attributes are accessor keys backed by
     * getAttribute/setAttribute (reads are live), and whose unknown keys
     * are plain property misses (undefined), matching the browser.
     * Enumeration uses getAttributeNames or the attributes list when an
     * engine has them, otherwise the map stays empty — reads still behave
     * like the browser, which is what boots require. Guarded define;
     * engines with a real dataset are untouched. Key camelization is a
     * char loop (no function-replace callbacks — ES5 engines). */
    "if(typeof document!='undefined'&&document.documentElement){"
    "var DS__P=Object.getPrototypeOf(document.documentElement);"
    "if(DS__P&&(typeof DS__P=='object'||typeof DS__P=='function')"
    "&&typeof DS__P.dataset=='undefined'){"
    "var DS__KEY=function(n){var s='';"
    "for(var i=5;i<n.length;i++){var c=n.charAt(i);"
    "if(c==='-'){i++;if(i<n.length)s+=n.charAt(i).toUpperCase();}"
    "else s+=c;}return s;};"
    "Object.defineProperty(DS__P,'dataset',{configurable:!0,enumerable:!1,"
    "get:function(){var el=this,ds={},names=[],i;"
    "if(typeof el.getAttributeNames=='function')names=el.getAttributeNames();"
    "else if(el.attributes&&el.attributes.length!=null)"
    "for(i=0;i<el.attributes.length;i++)"
    "try{names.push(el.attributes[i].name)}catch(esn){}"
    "for(i=0;i<names.length;i++){var n=String(names[i]);"
    "if(n.indexOf('data-')==0&&n.length>5){var k=DS__KEY(n),a=n;"
    "(function(k2,a2){try{Object.defineProperty(ds,k2,{configurable:!0,"
    "enumerable:!0,get:function(){return el.getAttribute(a2)},"
    "set:function(v){el.setAttribute(a2,v===undefined?'':String(v))}}"
    ")}catch(esd){}})(k,a);}}"
    "return ds;}});}}\n"
    /* ReadableStream: one FIFO queue. enqueue hands the chunk to the
     * oldest parked reader first (parked implies empty queue), else
     * buffers; close/error drain parked readers (done / rejection).
     * getReader().read() resolves buffered chunks immediately (native
     * Promise) and parks with a [resolve, reject] pair when starving;
     * pull sources are kicked once on the first starving read — async
     * pumps keep themselves alive through their enqueues, and the kick
     * re-arms when the pull promise settles. desiredSize reports a
     * positive value so loaders take the clean close() path. */
    "if(typeof Promise!='undefined'&&typeof ReadableStream=='undefined'){"
    "var ReadableStream=function(src){var q=[],pend=[],closed=!1,"
    "errored=null,pulling=!1;"
    "var ctl={desiredSize:1,"
    "enqueue:function(c){if(closed||errored)return;var p=pend.shift();"
    "if(p){p[0]({done:!1,value:c});}else{q.push(c);}},"
    "close:function(){if(closed||errored)return;closed=!0;var p;"
    "while(p=pend.shift())p[0]({done:!0,value:void 0});},"
    "error:function(e){if(closed||errored)return;errored=e;var p;"
    "while(p=pend.shift())p[1](e);}};"
    "var kick=function(){if(pulling||closed||errored)return;"
    "if(src&&typeof src.pull=='function'){pulling=!0;var pr;"
    "try{pr=src.pull(ctl);}catch(ep){pulling=!1;ctl.error(ep);return;}"
    "if(pr&&typeof pr.then=='function')pr.then(function(){pulling=!1;"
    "kick();},function(e2){pulling=!1;ctl.error(e2);});"
    "else pulling=!1;}};"
    "if(src&&typeof src.start=='function'){"
    "try{src.start(ctl);}catch(es){if(!closed&&!errored)errored=es;}}"
    "this.getReader=function(){return{read:function(){"
    "if(q.length>0)return Promise.resolve({done:!1,value:q.shift()});"
    "if(errored)return Promise.reject(errored);"
    "if(closed)return Promise.resolve({done:!0,value:void 0});"
    "kick();"
    "return new Promise(function(res,rej){pend.push([res,rej]);});},"
    "releaseLock:function(){},"
    "cancel:function(){ctl.close();return Promise.resolve();}};};"
    "this.cancel=function(){ctl.close();return Promise.resolve();};};"
    "}\n";

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
                 g_selectedEngine == JS_ENGINE_XS ||
                 g_selectedEngine == JS_ENGINE_XS_NR)
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
    /* A2: parallel open-tag spans for document.currentScript (pointer
     * entries only — P19 stack rule keeps aggregate scratch off the render
     * task; 2×24 pointers/sizes is 384B, same class as starts/lens). */
    const char *tags[JSBRIDGE_MAX_SCRIPTS];
    size_t tagLens[JSBRIDGE_MAX_SCRIPTS];
    int n = 0;
    int total = 0;
#ifdef TARGET_PLAYDATE
    /* R-kf bisect: stack-depth anchors across the mount lane — a device
     * gameTask stack overflow is a known class of failure here (P19 rule
     * origin), and two crash snapshots froze inside extract_scripts. */
    extern unsigned logger_stack_depth(void);
    logger_log("[js] probe: attach depth=%u", logger_stack_depth());
#endif
    if (!(policy == DOC_SCRIPT_FULL && doc->extScripts))
    {
        /* OFF/RUN/RUN_KEEP: extract inline bodies up front. FULL defers this
         * (it scans slots instead) and only extracts in its OOM fallback. */
        n = extract_scripts(doc->rawHtml ? doc->rawHtml : "", starts, lens,
                            JSBRIDGE_MAX_SCRIPTS, tags, tagLens);
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
            /* R54: PARK the plan, don't run it. The old loop executed every
             * script's parse+compile+run bracket inside THIS attach call —
             * one update() frame on device — and the 10s run-loop watchdog
             * hard-crashed (E1) mid-lexer after ~9.5s of ARM parse work
             * (2026-10-18 12:02:50). The parked plan drains ONE entry per
             * frame through jsbridge_script_queue_pump (same branch
             * semantics, same log rows, browser doc order preserved). */
            for (int i = 0; i < totalS; i++)
            {
                b->scriptQueue[b->scriptQueueCount++] = slots[i];
            }
            logger_log("[jsbridge] attach parked %d script slot(s) for "
                       "per-frame runs (was: all in one frame — E1 fix)",
                       b->scriptQueueCount);
        }
        else
        {
            /* Scan-scratch OOM: degrade to inline-only (still logged). */
            logger_log("[js] FULL scan OOM: inline-only fallback");
            n = extract_scripts(doc->rawHtml ? doc->rawHtml : "", starts, lens,
                                JSBRIDGE_MAX_SCRIPTS, tags, tagLens);
            total = (n > JSBRIDGE_MAX_SCRIPTS) ? JSBRIDGE_MAX_SCRIPTS : n;
            /* R54: same park-not-run policy as the main path (the pump
             * hands inline entries to impl->run_script per frame). */
            for (int i = 0; i < total; i++)
            {
                JsScriptSlot s;
                memset(&s, 0, sizeof(s));
                s.isExt = 0;
                s.inlineStart = starts[i];
                s.inlineLen = lens[i];
                s.tagStart = tags[i];
                s.tagLen = tagLens[i];
                s.isModule = jsbridge_script_type_is_module(
                    tags[i], tags[i] + tagLens[i]);
                b->scriptQueue[b->scriptQueueCount++] = s;
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
        /* R54 for ALL policies (R-kf fix, 2026-10-22 device receipts):
         * never run inline script parses inside the attach frame. An NR
         * fxParseScript driven synchronously from the attach context
         * hard-wedges the device — zero ka rows, crashlog register dumps
         * scattering across strlen/extract_scripts/firmware — while the
         * same machine, bracket, and parse from the per-frame pump lane
         * runs clean end to end (tests/logs/
         * device_aboutjavascript_parked_FIX_20261022.log: [nrp] rows
         * print, ka cadence holds, SW battery 54 passed, no crashlog).
         * Park inline bodies and drain them one entry per frame, like
         * the parked FULL path already does. */
        for (int i = 0; i < total; i++)
        {
            JsScriptSlot s;
            memset(&s, 0, sizeof(s));
            s.isExt = 0;
            s.inlineStart = starts[i];
            s.inlineLen = lens[i];
            s.tagStart = tags[i];
            s.tagLen = tagLens[i];
            s.isModule = jsbridge_script_type_is_module(
                tags[i], tags[i] + tagLens[i]);
            b->scriptQueue[b->scriptQueueCount++] = s;
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
    jsbridge_deferred_clear(b); /* waiting scripts die with the page */
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
                impl->clear_xhr_refs(b, r->fnRef, r->errFnRef, r->objRef);
                if (r->body)
                {
                    JFree(r->body);
                }
                /* C4: the request body dies with the request (shared
                 * teardown — frozen engines always have reqBody NULL). */
                if (r->reqBody)
                {
                    JFree(r->reqBody);
                }
                memset(r, 0, sizeof(*r));
            }
        }
        b->xhrCount = 0;
    }
    /* Worker stubs: release engine refs (same contract as the XHR loop). */
    if (impl && impl->clear_worker_refs)
    {
        for (int i = 0; i < b->wkCount; i++)
        {
            JsWorkerStub *w = &b->wk[i];
            if (w->active || w->release)
            {
                impl->clear_worker_refs(b, w->objRef);
                memset(w, 0, sizeof(*w));
            }
        }
        b->wkCount = 0;
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
int jsbridge_mount_parked(JsBridge *b); /* R28: defined below (after eval_pending) */

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
    if (jsbridge_mount_parked(b))
    {
        return 0; /* R28: parked mount owns the machine — timers wait */
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

/* ── R15: deferred compile retry (OS-truth admission) ────────────────────
 * Engines skip a giant script containedly when the OS heap cannot grant
 * its estimated compile peak RIGHT NOW. Disk-resident bodies are re-offered
 * here on later frames: the probe is repeated every frame (cheap — a
 * handful of transient mallocs), and the script compiles the moment memory
 * frees (e.g. the download's network buffers released after attach). RAM
 * sources cannot be deferred (no disk copy) and stay skipped. */
void jsbridge_deferred_offer(JsBridge *b, int engine, SpillFile spill,
                             size_t len)
{
    if (!b || spill < 0 || len == 0)
    {
        return;
    }
    /* Fail-fast: a script whose ESTIMATED compile peak exceeds the whole
     * device budget can never be admitted — deferral would park it forever
     * (R15 device data: a 502KB bundle needs ~7.0MB of parse against a
     * 6.5MB budget; the parse tree is inherently RAM-resident, so no
     * offloading changes this). Log the precise numbers and let it go. */
    unsigned long need = jsbridge_compile_need((unsigned long)len);
    unsigned long ceiling = pluto_mem_budget();
    if (ceiling && need > ceiling)
    {
        logger_log("[js] %zuKB script exceeds the device compile budget "
                   "(need ~%luKB, budget %luKB, ~%d parse bytes per source "
                   "byte) — not runnable on this device in any engine",
                   len >> 10, need >> 10, ceiling >> 10,
                   JSBRIDGE_COMPILE_PER_BYTE);
        return;
    }
    for (int i = 0; i < b->deferredCount; i++)
    {
        if (b->deferred[i].spill == spill)
        {
            return; /* already waiting */
        }
    }
    if (b->deferredCount >= JSBRIDGE_DEFERRED_MAX)
    {
        return; /* table full: the skip stands */
    }
    b->deferred[b->deferredCount].engine = engine;
    b->deferred[b->deferredCount].spill = spill;
    b->deferred[b->deferredCount].len = len;
    b->deferredCount++;
    logger_log("[js] deferred: %zuKB script waits for memory (slot %d/%d)",
               len >> 10, b->deferredCount, JSBRIDGE_DEFERRED_MAX);
}

void jsbridge_deferred_clear(JsBridge *b)
{
    if (b)
    {
        /* Spill handles are OWNED by the doc's ext slots — dropping the
         * table entry never discards the file (page teardown resets spill). */
        memset(b->deferred, 0, sizeof(b->deferred));
        b->deferredCount = 0;
    }
}

/* ── R20: per-frame ENGINE pump ────────────────────────────────────────────
 * A hook for engines with background work that must be spread across
 * frames to keep the SDK run loop alive (device watchdog fires at 10s
 * without an update() return). Currently: the XS-NR fork resumes a
 * time-sliced bundler-monolith split — each frame runs at most its own
 * wall-clock budget of segments, then control returns to the run loop.
 * mutationsOut mirrors jsbridge_deferred_pump: set when resumed work
 * consumed DOM budget (caller schedules the standard re-render). No-op
 * for engines without a pump (or while a callback is on the stack). */
void jsbridge_engine_pump(JsBridge *b, unsigned nowMs, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->inTimer || b->inClick)
    {
        return;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->pump)
    {
        return;
    }
    int budgetBefore = b->callBudget;
    impl->pump(b, nowMs);
    if (b->callBudget < budgetBefore && mutationsOut)
    {
        *mutationsOut = 1; /* resumed work mutated the DOM — re-render */
    }
}

/* R27: 1 while the engine still has background eval work in flight (XS NR
 * bundler split) — or the R54 per-frame script queue still has entries.
 * The render pipeline gates the walk/snapshot on this so they observe the
 * POST-JS DOM: the sync attach loop ran every script before any walk, so
 * walking mid-queue would capture the PRE-JS DOM and (with caching on)
 * poison the snapshot with it. NOTE: the queue pump itself only gates on
 * impl->eval_pending — counting the queue here too would make the pump
 * wait on itself. */
int jsbridge_eval_pending(JsBridge *b)
{
    if (!b || b->inTimer || b->inClick)
    {
        return 0;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->eval_pending)
    {
        return 0;
    }
    /* R54: the parked script queue is part of the pending truth (see the
     * comment header). The pump's OWN gate uses impl->eval_pending only
     * so it can never wait on itself. */
    if (b->scriptQueueNext < b->scriptQueueCount)
    {
        return 1;
    }
    return impl->eval_pending(b);
}

/* R28: 1 while a resumable-mount run is parked mid-program (XS NR). The
 * router pumps SKIP deliveries while this holds — timers/XHR completions
 * wait until the mount finishes, matching browser semantics (no timers
 * during a synchronous task) without dropping anything. */
int jsbridge_mount_parked(JsBridge *b)
{
    if (!b || b->inTimer || b->inClick)
    {
        return 0;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->mount_parked)
    {
        return 0;
    }
    return impl->mount_parked(b);
}

void jsbridge_deferred_pump(JsBridge *b, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->deferredCount == 0 || b->inTimer || b->inClick)
    {
        return;
    }
    if (jsbridge_mount_parked(b))
    {
        return; /* R28: parked mount owns the machine — retry next frame */
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_script_stream)
    {
        return;
    }
    /* Head-of-line: retry the OLDEST waiting script (FIFO keeps page
     * script order meaningful for globals). */
    SpillFile spill = b->deferred[0].spill;
    size_t len = b->deferred[0].len;
    unsigned long need = jsbridge_compile_need((unsigned long)len);
    if (pluto_mem_probe_grantable(need) < need)
    {
        return; /* still not grantable — try again next frame (no spam) */
    }
    /* Remove BEFORE running: run_script_stream re-probes and would
     * re-offer on a between-frames regression — that is the correct
     * outcome (stays deferred), never a same-frame loop. */
    memmove(&b->deferred[0], &b->deferred[1],
            sizeof(b->deferred[0]) * (size_t)(b->deferredCount - 1));
    b->deferredCount--;
    logger_log("[js] deferred retry: memory freed — running %zuKB script "
               "now (live %luKB)",
               len >> 10, pluto_mem_live() >> 10);
    int budgetBefore = b->callBudget;
    b->inTimer = 1; /* same re-entrancy contract as a timer fire */
    impl->run_script_stream(b, spill, len, 2);
    b->inTimer = 0;
    if (b->callBudget < budgetBefore && mutationsOut)
    {
        *mutationsOut = 1; /* the script mutated the DOM — re-render */
    }
}

/* ── API-P5: ES modules — the loader-side module lane ──────────────────
 * <script type="module"> runs through THIS lane on every engine: both
 * scanners flag the element (jsbridge_script_type_is_module), and the
 * router swaps the classic run for a loader-side transform executed in
 * C, BEFORE the engine sees the source. A module then executes as an
 * ordinary script whose import/export syntax has already been
 * browser-processed — engine isolation stays untouched (the vendored
 * engines are never modified) and every engine gets the same lane.
 *
 * Browser-faithful semantics implemented:
 *   - one run per document, wrapped in an IIFE: top-level const/let/
 *     var/function/class stay module-local (never globals) — the module
 *     scope isolation, for free, on every engine;
 *   - static import declarations resolve RELATIVE TO THE MODULE'S OWN
 *     URL (the document URL for inline modules; the module file's URL
 *     for src= modules) through url_resolve — the same resolver the
 *     page loader and the history journal use;
 *   - the resolved specifier fetches through the SAME roads the page
 *     already has: about: = the internal page store (the fetch()/
 *     xhr road, synchronous); file: = the Data-dir rule of the file://
 *     document loader (main.c pluto_load_local_file). http(s) imports
 *     are refused with a receipt — an async module graph is future
 *     work, never faked;
 *   - `export const|let|var X[=…]`, `export function|class X` (and
 *     `export async function X`) at top level are stripped of the
 *     keyword and materialized onto a namespace object
 *     globalThis.__p5mod["<resolved URL>"] after the body runs;
 *   - `import d`, `import {a, b as c}`, `import * as ns` and the bare
 *     `import 'x'` side-effect form all read that namespace;
 *   - graph order = import-first (children run before the importer, in
 *     document order of the import declarations), acyclic; the ledger
 *     (per-document module keys + visiting state on the bridge) gives
 *     the cycle/cap receipts.
 *
 * Documented boundaries (typed gaps, per the program's honesty rule):
 *   - `export default`, `export * [from]`, `export {list} from` do not
 *     materialize: the statement is removed and the refusal logged;
 *   - dynamic `import()` is left untouched — the engine throws at
 *     runtime like any unknown global today;
 *   - a path-only relative specifier on an OPAQUE about: base refuses
 *     exactly as url_resolve does for the history journal — browsers
 *     refuse it identically ("Failed to resolve module specifier" on
 *     opaque origins);
 *   - grammar coverage = the lean lexer's: comments, strings, regexp
 *     literals and template literals are consumed as tokens; template
 *     ${…} internals are NOT re-interpreted (the R16 chunk scanner's
 *     accepted weakening, retained so the loader lexers all agree).
 */

#define JSBRIDGE_MOD_SRC_MAX (16 * 1024)   /* one module's fetched source */
#define JSBRIDGE_MOD_BUILT_MAX (20 * 1024) /* prelude + body + postludes */
/* JSBRIDGE_MOD_IMPORTS moved to jsbridge_internal.h (API-P10 flight table) */
#define JSBRIDGE_MOD_NAMES 6               /* named bindings per import */
#define JSBRIDGE_MOD_EXPORTS 12            /* exported names per module */
#define JSBRIDGE_MOD_SPANS 40              /* removal spans per module */
#define JSBRIDGE_MOD_MAX_DEPTH 4           /* graph nesting guard */
#define JSBRIDGE_MOD_NAME_MAX 64           /* binding local-name cap */

typedef struct
{
    char spec[JSBRIDGE_MOD_KEY_MAX];
    char dflt[JSBRIDGE_MOD_NAME_MAX];
    int namedCount;
    struct
    {
        char in[JSBRIDGE_MOD_NAME_MAX];
        char local[JSBRIDGE_MOD_NAME_MAX];
    } named[JSBRIDGE_MOD_NAMES];
    int star;
    char starLocal[JSBRIDGE_MOD_NAME_MAX];
} ModImport;

typedef struct
{
    int importCount;
    ModImport imp[JSBRIDGE_MOD_IMPORTS];
    int exportCount;
    char exp[JSBRIDGE_MOD_EXPORTS][JSBRIDGE_MOD_NAME_MAX];
    /* API-P10: export {local, local as exported} — the LOCAL binding per
     * export name (== exp[i] unless the `as` alias renames). The build's
     * postludes materialize __p5mod[key][exp[i]] from expLocal[i]. */
    char expLocal[JSBRIDGE_MOD_EXPORTS][JSBRIDGE_MOD_NAME_MAX];
    int xDefault, xList, xStar, xDyn, xOther;
    int spanCount;
    struct
    {
        size_t at; /* inclusive */
        size_t end; /* exclusive */
    } cut[JSBRIDGE_MOD_SPANS];
    /* API-P10: dynamic `import(` RENAME spans — the word's span is copied
     * as `__p5di` by mod_build's body copy (see its prelude). Never inside
     * a cut span: cuts hold top-level statements, rename spans live
     * inside expression positions; both come from the same token scan. */
    int renCount;
    struct
    {
        size_t at; /* inclusive — the `import` word start */
        size_t end; /* exclusive — the word end (6 bytes later) */
    } ren[JSBRIDGE_MOD_SPANS];
    /* graph phase output (children resolved and run BEFORE the build): */
    char impKeys[JSBRIDGE_MOD_IMPORTS][JSBRIDGE_MOD_KEY_MAX];
    unsigned char impLoaded[JSBRIDGE_MOD_IMPORTS];
} ModScan;

static int ml_isidchar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

/* Whitespace + comments trivia skip (mirrors blex_space_at, with line
 * comments added for statement-head positions). Unterminated block
 * comment: returns p so the caller advances (no infinite loop). */
static size_t ml_space_at(const char *src, size_t len, size_t p)
{
    for (;;)
    {
        while (p < len && (src[p] == ' ' || src[p] == '\t' ||
                           src[p] == '\r' || src[p] == '\n'))
        {
            p++;
        }
        if (p + 1 < len && src[p] == '/' && src[p + 1] == '/')
        {
            while (p < len && src[p] != '\n')
            {
                p++;
            }
            continue;
        }
        if (p + 1 < len && src[p] == '/' && src[p + 1] == '*')
        {
            size_t i = p + 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
            {
                i++;
            }
            if (i + 1 >= len)
            {
                return p; /* unterminated: treat as ambiguous */
            }
            p = i + 2;
            continue;
        }
        return p;
    }
}

/* Regex-context classifier — mirrors the bundler's blex_regex_possible
 * (the R16 value/division rule): / after ) ] } " ' ` or a non-keyword
 * identifier is division; after operators, open brackets, keywords and
 * statement starts it opens a regex literal. */
static int ml_regex_possible(const char *src, size_t from)
{
    size_t p = from;
    while (p > 0)
    {
        p--;
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }
        if (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' ||
            c == '`')
        {
            return 0; /* division context */
        }
        if (ml_isidchar((unsigned char)c))
        {
            static const char *const kws[] = {
                "return", "typeof", "case", "in", "of", "instanceof",
                "new", "delete", "void", "do", "else", "throw", "yield",
                "await"
            };
            size_t w0 = p;
            while (w0 > 0 && ml_isidchar((unsigned char)src[w0 - 1]))
            {
                w0--;
            }
            size_t wl = p - w0 + 1;
            for (unsigned i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
            {
                if (strlen(kws[i]) == wl &&
                    memcmp(src + w0, kws[i], wl) == 0)
                {
                    return 1;
                }
            }
            return 0;
        }
        return 1; /* operators / open brackets / start: regex context */
    }
    return 1;
}

/* Exclusive end of the comment/string/template token opened at p.
 * Unterminated token: returns len (the scan then bails naturally). */
static size_t ml_line_end(const char *src, size_t len, size_t p)
{
    while (p < len && src[p] != '\n')
    {
        p++;
    }
    return p;
}

static size_t ml_block_end(const char *src, size_t len, size_t p)
{
    size_t i = p + 2;
    while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
    {
        i++;
    }
    return (i + 1 < len) ? i + 2 : len;
}

static size_t ml_string_end(const char *src, size_t len, size_t p)
{
    char q = src[p];
    p++;
    while (p < len)
    {
        if (src[p] == '\\')
        {
            p += 2;
            continue;
        }
        if (src[p] == q)
        {
            return p + 1;
        }
        p++;
    }
    return len;
}

static size_t ml_templ_end(const char *src, size_t len, size_t p)
{
    p++;
    while (p < len)
    {
        if (src[p] == '\\')
        {
            p += 2;
            continue;
        }
        if (src[p] == '`')
        {
            return p + 1;
        }
        p++;
    }
    return len;
}

static size_t ml_regex_end(const char *src, size_t len, size_t p)
{
    int bracket = 0;
    p++;
    while (p < len)
    {
        if (src[p] == '\\')
        {
            p += 2;
            continue;
        }
        if (src[p] == '[')
        {
            bracket = 1;
        }
        else if (src[p] == ']')
        {
            bracket = 0;
        }
        else if (src[p] == '/' && !bracket)
        {
            p++;
            while (p < len && ml_isidchar((unsigned char)src[p]))
            {
                p++; /* flags */
            }
            return p;
        }
        else if (src[p] == '\n')
        {
            return p; /* unterminated: bail to the raw walker */
        }
        p++;
    }
    return len;
}

/* Exclusive end of the token opened at p ('/'-comment forms included;
 * a division slash returns p+1). */
static size_t ml_token_end(const char *src, size_t len, size_t p)
{
    if (p >= len)
    {
        return p;
    }
    if (src[p] == '/' && p + 1 < len)
    {
        if (src[p + 1] == '/')
        {
            return ml_line_end(src, len, p);
        }
        if (src[p + 1] == '*')
        {
            return ml_block_end(src, len, p);
        }
    }
    if (src[p] == '/')
    {
        return ml_regex_end(src, len, p);
    }
    if (src[p] == '"' || src[p] == '\'')
    {
        return ml_string_end(src, len, p);
    }
    if (src[p] == '`')
    {
        return ml_templ_end(src, len, p);
    }
    return p + 1;
}

/* Boundary-true keyword check: the word `w` (len wl) sits exactly at p,
 * with a non-identifier character on BOTH sides. */
static int ml_word_at(const char *src, size_t len, size_t p,
                      const char *w, size_t wl, size_t *endOut)
{
    if (p + wl > len)
    {
        return 0;
    }
    if (memcmp(src + p, w, wl) != 0)
    {
        return 0;
    }
    if (p > 0 && ml_isidchar((unsigned char)src[p - 1]))
    {
        return 0;
    }
    if (p + wl < len && ml_isidchar((unsigned char)src[p + wl]))
    {
        return 0;
    }
    if (endOut)
    {
        *endOut = p + wl;
    }
    return 1;
}

/* Read ONE trivia-skipping token at *pp:
 *   kind 1 = identifier (in word, ≤ wordn-1 bytes)
 *   kind 2 = string literal (raw contents, unquoted)
 *   kind 0 = one punctuation char in *c0
 * *pp advances past the token; *startOut gets the token's first byte. */
static int ml_token(const char *src, size_t len, size_t *pp, char *word,
                    size_t wordn, char *c0, size_t *startOut)
{
    size_t p = ml_space_at(src, len, *pp);
    if (p >= len)
    {
        *pp = p;
        return -1;
    }
    if (startOut)
    {
        *startOut = p;
    }
    if (ml_isidchar((unsigned char)src[p]) &&
        (p == 0 || !ml_isidchar((unsigned char)src[p - 1])))
    {
        size_t e = p + 1;
        while (e < len && ml_isidchar((unsigned char)src[e]))
        {
            e++;
        }
        size_t wl = e - p;
        if (wl > wordn - 1)
        {
            wl = wordn - 1;
        }
        memcpy(word, src + p, wl);
        word[wl] = '\0';
        *pp = e;
        return 1;
    }
    if (src[p] == '"' || src[p] == '\'')
    {
        size_t e = ml_string_end(src, len, p);
        size_t inner = (e >= p + 2) ? (e - p - 2) : 0;
        if (inner > wordn - 1)
        {
            inner = wordn - 1;
        }
        memcpy(word, src + p + 1, inner);
        word[inner] = '\0';
        *pp = e;
        return 2;
    }
    *c0 = src[p];
    *pp = p + 1;
    return 0;
}

/* Named-binding list after '{'. On success *pp sits past the closing
 * '}' and the slots are copied into `slot`; a malformed list returns 0
 * (the caller fails the import loudly — the engine then reports the
 * raw source). */
static void mod_add_cut(ModScan *m, size_t at, size_t end);
static void mod_add_ren(ModScan *m, size_t at, size_t end);

/* Named-binding list after '{'. On success *pp sits past the closing
 * '}' and the slots are copied into `slot`; a malformed list returns 0
 * (the caller fails the import loudly — the engine then reports the
 * raw source). */
static int mod_named_list(const char *src, size_t len, size_t p,
                          ModImport *slot, size_t *nextOut)
{
    struct
    {
        char in[JSBRIDGE_MOD_NAME_MAX];
        char local[JSBRIDGE_MOD_NAME_MAX];
    } out[JSBRIDGE_MOD_NAMES];
    int n = 0;
    for (;;)
    {
        p = ml_space_at(src, len, p);
        if (p >= len)
        {
            return 0;
        }
        if (src[p] == '}')
        {
            p++;
            break;
        }
        if (src[p] == ',')
        {
            p++;
            continue;
        }
        if (!ml_isidchar((unsigned char)src[p]) ||
            (p > 0 && ml_isidchar((unsigned char)src[p - 1])))
        {
            return 0; /* not a binding name */
        }
        size_t e = p + 1;
        while (e < len && ml_isidchar((unsigned char)src[e]))
        {
            e++;
        }
        size_t wl = e - p;
        if (wl > sizeof(out[0].in) - 1)
        {
            wl = sizeof(out[0].in) - 1;
        }
        memcpy(out[n].in, src + p, wl);
        out[n].in[wl] = '\0';
        snprintf(out[n].local, sizeof(out[n].local), "%s", out[n].in);
        p = ml_space_at(src, len, e);
        if (ml_word_at(src, len, p, "as", 2, NULL))
        {
            p = ml_space_at(src, len, p + 2);
            if (!ml_isidchar((unsigned char)src[p]) ||
                (p > 0 && ml_isidchar((unsigned char)src[p - 1])))
            {
                return 0; /* `as` without an alias */
            }
            size_t e2 = p + 1;
            while (e2 < len && ml_isidchar((unsigned char)src[e2]))
            {
                e2++;
            }
            wl = e2 - p;
            if (wl > sizeof(out[0].local) - 1)
            {
                wl = sizeof(out[0].local) - 1;
            }
            char alias[JSBRIDGE_MOD_NAME_MAX];
            memcpy(alias, src + p, wl);
            alias[wl] = '\0';
            snprintf(out[n].local, sizeof(out[n].local), "%s", alias);
            p = e2;
        }
        n++;
        if (n >= JSBRIDGE_MOD_NAMES)
        {
            logger_log("[module] import named-binding cap %d hit (the "
                       "import keeps only its first %d bindings)",
                       JSBRIDGE_MOD_NAMES, JSBRIDGE_MOD_NAMES);
            break;
        }
    }
    for (int i = 0; i < n; i++)
    {
        snprintf(slot->named[i].in, sizeof(slot->named[i].in), "%s",
                 out[i].in);
        snprintf(slot->named[i].local, sizeof(slot->named[i].local), "%s",
                 out[i].local);
    }
    slot->namedCount = n;
    *nextOut = p;
    return n;
}

/* `* [as <name>]` after 'import'. Advances *pp; fills starLocal (the
 * default "ns" keeps a valid binding when a site writes bare `*`). */
static void mod_read_star(const char *src, size_t len, size_t *pp,
                          char *starLocal, size_t sn)
{
    size_t start = 0;
    char word[JSBRIDGE_MOD_NAME_MAX];
    char c0 = 0;
    int tok = ml_token(src, len, pp, word, sizeof(word), &c0, &start);
    if (tok == 1 && strcmp(word, "as") == 0)
    {
        tok = ml_token(src, len, pp, word, sizeof(word), &c0, &start);
    }
    if (tok == 1)
    {
        snprintf(starLocal, sn, "%s", word);
    }
    else
    {
        snprintf(starLocal, sn, "ns");
    }
}

static void mod_record_import(ModScan *m, const ModImport *im)
{
    if (m->importCount >= JSBRIDGE_MOD_IMPORTS)
    {
        logger_log("[module] import cap %d hit — import '%s' not cut (raw "
                   "source left; the engine reports it)",
                   JSBRIDGE_MOD_IMPORTS, im->spec);
        return;
    }
    m->imp[m->importCount++] = *im;
    logger_log("[module] import declared '%s' (dflt='%s' named=%d star=%d)",
               im->spec, im->dflt[0] ? im->dflt : "-",
               im->namedCount, im->star);
}

/* `from` + the specifier string after a bindings head. On success *pp
 * sits past the string and spec is filled; otherwise returns 0. */
static int mod_from_spec(const char *src, size_t len, size_t *pp,
                         char *spec, size_t specn)
{
    size_t p = *pp;
    size_t start = 0;
    char word[JSBRIDGE_MOD_NAME_MAX];
    char c0 = 0;
    int tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);
    if (!(tok == 1 && strcmp(word, "from") == 0))
    {
        return 0;
    }
    tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);
    if (tok != 2)
    {
        return 0;
    }
    size_t wl = strlen(word);
    if (wl > specn - 1)
    {
        wl = specn - 1;
    }
    memcpy(spec, word, wl);
    spec[wl] = '\0';
    *pp = p;
    return 1;
}

/* Parse the import statement whose keyword begins at `at` (boundary
 * guaranteed by the caller). Always returns 1 with *nextOut advanced:
 *   - a static import is recorded and CUT;

 *   - the dynamic call form is left untouched (receipt only);
 *   - fallen shapes are logged + left for the engine's SyntaxError. */
static void mod_parse_import(const char *src, size_t len, size_t at,
                             ModScan *m, size_t *nextOut)
{
    size_t p = at + 6;
    size_t q = ml_space_at(src, len, p);
    if (q < len && src[q] == '(')
    {
        m->xDyn++;
        /* API-P10: dynamic import() now REWRITES to the bound __p5di
         * helper (mod_build renames the word span and prelude-defines
         * the closure over this module's own import base). The call's
         * argument list copies through untouched. */
        mod_add_ren(m, at, at + 6);
        logger_log("[module] dynamic import() -> __p5di rename span "
                   "(count %d)", m->xDyn);
        *nextOut = at + 6;
        return;
    }

    ModImport im;
    memset(&im, 0, sizeof(im));

    size_t start = 0;
    char word[JSBRIDGE_MOD_NAME_MAX];
    char c0 = 0;
    int tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);
    if (tok == 2)
    {
        /* bare side-effect form: import 'x' */
        snprintf(im.spec, sizeof(im.spec), "%s", word);
    }
    else if (tok == 1 && strcmp(word, "from") != 0)
    {
        /* default binding first: import d [, {list}] [, * as ns] from … */
        snprintf(im.dflt, sizeof(im.dflt), "%s", word);
        tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);
        if (tok == 0 && c0 == ',')
        {
            tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);
        }
        if (tok == 0 && c0 == '{')
        {
            size_t after = p;
            int got = mod_named_list(src, len, after, &im, &after);
            if (!got)
            {
                logger_log("[module] import head unparseable at byte %zu — "
                           "left for the engine's SyntaxError", at);
                *nextOut = at + 6;
                return;
            }
            p = after;
        }
        else if (tok == 0 && c0 == '*')
        {
            im.star = 1;
            mod_read_star(src, len, &p, im.starLocal, sizeof(im.starLocal));
        }
        else if (!(tok == 1 && strcmp(word, "from") == 0))
        {
            logger_log("[module] import head unparseable at byte %zu — "
                       "left for the engine's SyntaxError", at);
            *nextOut = at + 6;
            return;
        }
    }
    else if (tok == 0 && c0 == '{')
    {
        size_t after = p;
        int got = mod_named_list(src, len, after, &im, &after);
        if (!got)
        {
            logger_log("[module] import head unparseable at byte %zu — "
                       "left for the engine's SyntaxError", at);
            *nextOut = at + 6;
            return;
        }
        p = after;
    }
    else if (tok == 0 && c0 == '*')
    {
        im.star = 1;
        mod_read_star(src, len, &p, im.starLocal, sizeof(im.starLocal));
    }
    else
    {
        logger_log("[module] import head unparseable at byte %zu — left "
                   "for the engine's SyntaxError", at);
        *nextOut = at + 6;
        return;
    }

    /* from + specifier — skipped for the bare form (spec already read) */
    if (!im.spec[0])
    {
        if (!mod_from_spec(src, len, &p, im.spec, sizeof(im.spec)))
        {
            logger_log("[module] from-spec unparseable at byte %zu — left "
                       "for the engine's SyntaxError", at);
            *nextOut = at + 6;
            return;
        }
    }
    /* optional trailing ';' (the cut includes it) */
    size_t q2 = ml_space_at(src, len, p);
    if (q2 < len && src[q2] == ';')
    {
        p = q2 + 1;
    }
    mod_record_import(m, &im);
    mod_add_cut(m, at, p);
    *nextOut = p;
}


/* Statement extent from p: past the terminating ';' at level 0 (or a
 * stray EOF). Level counts { ( [ ; strings/templates/regex/comments are
 * consumed as tokens. */
static size_t ml_stmt_end(const char *src, size_t len, size_t p)
{
    int level = 0;
    while (p < len)
    {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            p++;
            continue;
        }
        if (c == '/' && p + 1 < len &&
            (src[p + 1] == '/' || src[p + 1] == '*'))
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '/' && ml_regex_possible(src, p))
        {
            p = ml_regex_end(src, len, p);
            continue;
        }
        if (c == '/' || c == '"' || c == '\'' || c == '`')
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '{' || c == '(' || c == '[')
        {
            level++;
            p++;
            continue;
        }
        if (c == '}' || c == ')' || c == ']')
        {
            if (level == 0)
            {
                break; /* unbalanced close: caller's bisect */
            }
            level--;
            p++;
            continue;
        }
        if (c == ';' && level == 0)
        {
            return p + 1;
        }
        p++;
    }
    return len;
}

static void mod_add_cut(ModScan *m, size_t at, size_t end)
{
    if (at >= end || m->spanCount >= JSBRIDGE_MOD_SPANS)
    {
        if (m->spanCount >= JSBRIDGE_MOD_SPANS)
        {
            logger_log("[module] scan overflow (cut table full) — module "
                       "left uncut at byte %zu (critical SyntaxError path)",
                       at);
        }
        return;
    }
    m->cut[m->spanCount].at = at;
    m->cut[m->spanCount].end = end;
    m->spanCount++;
}

/* API-P10: record an `import` word span for the `__p5di` rename at
 * emission time. Overflow leaves the call UNREWRITTEN (the engine's own
 * reserved-word handling answers, the documented P5 boundary behavior). */
static void mod_add_ren(ModScan *m, size_t at, size_t end)
{
    if (at >= end || m->renCount >= JSBRIDGE_MOD_SPANS)
    {
        if (m->renCount >= JSBRIDGE_MOD_SPANS)
        {
            logger_log("[module] scan overflow (rename table full) — "
                       "dynamic import() left unrewritten at byte %zu",
                       at);
        }
        return;
    }
    m->ren[m->renCount].at = at;
    m->ren[m->renCount].end = end;
    m->renCount++;
}

static void mod_add_export(ModScan *m, const char *name)
{
    if (!name[0])
    {
        return;
    }
    if (m->exportCount >= JSBRIDGE_MOD_EXPORTS)
    {
        logger_log("[module] export-name cap %d hit ('%s' not materialized "
                   "— the declaration still runs)",
                   JSBRIDGE_MOD_EXPORTS, name);
        return;
    }
    snprintf(m->exp[m->exportCount], JSBRIDGE_MOD_NAME_MAX, "%s", name);
    snprintf(m->expLocal[m->exportCount], JSBRIDGE_MOD_NAME_MAX, "%s",
             name);
    m->exportCount++;
}

/* API-P10: the export {local [as exported]} pair form — the build
 * materializes name → the LOCAL binding (defaults to the same name). */
static void mod_add_export_pair(ModScan *m, const char *local,
                                const char *name)
{
    if (!name[0] || !local[0])
    {
        return;
    }
    if (m->exportCount >= JSBRIDGE_MOD_EXPORTS)
    {
        logger_log("[module] export-name cap %d hit ('%s' not materialized "
                   "— the declaration still runs)",
                   JSBRIDGE_MOD_EXPORTS, name);
        return;
    }
    snprintf(m->exp[m->exportCount], JSBRIDGE_MOD_NAME_MAX, "%s", name);
    snprintf(m->expLocal[m->exportCount], JSBRIDGE_MOD_NAME_MAX, "%s",
             local);
    m->exportCount++;
}

/* const/let/var declarator names, level 0: the FIRST ident of each
 * top-level declarator (a,b in `const a = f(1), b = 2`). Destructuring
 * heads are consumed but not named (receipt; honest gap). *endOut =
 * past the terminating ';'. */
static void mod_decl_names(const char *src, size_t len, size_t p, ModScan *m,
                           size_t *endOut)
{
    int level = 0;
    int expectName = 1;
    while (p < len)
    {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            p++;
            continue;
        }
        if (c == '/' && p + 1 < len &&
            (src[p + 1] == '/' || src[p + 1] == '*'))
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '/' && ml_regex_possible(src, p))
        {
            p = ml_regex_end(src, len, p);
            continue;
        }
        if (c == '/' || c == '"' || c == '\'' || c == '`')
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '{')
        {
            if (expectName && level == 0)
            {
                logger_log("[module] export destructure unref (names stay "
                           "local — documented boundary)");
                expectName = 0;
            }
            level++;
            p++;
            continue;
        }
        if (c == '(' || c == '[')
        {
            level++;
            expectName = 0;
            p++;
            continue;
        }
        if (c == '}' || c == ')' || c == ']')
        {
            if (level > 0)
            {
                level--;
            }
            p++;
            continue;
        }
        if (c == ',' && level == 0)
        {
            expectName = 1;
            p++;
            continue;
        }
        if (c == ';' && level == 0)
        {
            p++;
            break;
        }
        if (ml_isidchar((unsigned char)c) &&
            (p == 0 || !ml_isidchar((unsigned char)src[p - 1])))
        {
            if (level == 0 && expectName)
            {
                size_t e = p + 1;
                while (e < len && ml_isidchar((unsigned char)src[e]))
                {
                    e++;
                }
                size_t wl = e - p;
                char nbuf[JSBRIDGE_MOD_NAME_MAX];
                if (wl > sizeof(nbuf) - 1)
                {
                    wl = sizeof(nbuf) - 1;
                }
                memcpy(nbuf, src + p, wl);
                nbuf[wl] = '\0';
                mod_add_export(m, nbuf);
                p = e;
                expectName = 0;
                continue;
            }
            while (p < len && ml_isidchar((unsigned char)src[p]))
            {
                p++;
            }
            continue;
        }
        p++;
    }
    *endOut = p;
}

/* Parse the export statement whose keyword begins at `at`. Always
 * returns with *nextOut past the affected span:
 *   - decl kinds cut the 'export' head (keyword+ws) and record names;
 *   - default/list/star/generic refuse (removed whole + receipts);
 *   - unrecognizable shapes are errored honestly by the engine (the
 *     keyword is left and the walker resumes after it). */
static void mod_parse_export(const char *src, size_t len, size_t at,
                             ModScan *m, size_t *nextOut)
{
    size_t p = at + 6;
    size_t start = 0;
    char word[JSBRIDGE_MOD_NAME_MAX];
    char c0 = 0;
    int tok = ml_token(src, len, &p, word, sizeof(word), &c0, &start);

    if (tok == 1 && (!strcmp(word, "const") || !strcmp(word, "let") ||
                     !strcmp(word, "var")))
    {
        mod_add_cut(m, at, start);
        size_t send = 0;
        mod_decl_names(src, len, p, m, &send);
        *nextOut = send;
        return;
    }
    if (tok == 1 && (!strcmp(word, "function") || !strcmp(word, "class")))
    {
        mod_add_cut(m, at, start);
        /* the name: next ident (a generator's '*' sits between) */
        size_t np = start;
        char c0b = 0;
        size_t sb = 0;
        int t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
        if (t2 == 1 && (strcmp(word, "function") == 0 ||
                        strcmp(word, "class") == 0))
        {
            t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
        }
        if (t2 == 0 && c0b == '*')
        {
            t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
        }
        if (t2 == 1)
        {
            mod_add_export(m, word);
        }
        *nextOut = start; /* the walker resumes at the kind keyword */
        return;
    }
    if (tok == 1 && strcmp(word, "async") == 0)
    {
        /* export async function name … */
        size_t np = p;
        char c0b = 0;
        size_t sb = 0;
        int t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
        if (t2 == 1 && strcmp(word, "function") == 0)
        {
            mod_add_cut(m, at, start);
            size_t save = start;
            t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
            if (t2 == 0 && c0b == '*')
            {
                t2 = ml_token(src, len, &np, word, sizeof(word), &c0b, &sb);
            }
            if (t2 == 1)
            {
                mod_add_export(m, word);
            }
            *nextOut = save; /* resume at the 'async' keyword */
            return;
        }
        logger_log("[module] export head unrecognized at byte %zu — left "
                   "for the engine's SyntaxError", at);
        *nextOut = at + 6;
        return;
    }
    if (tok == 1 && strcmp(word, "default") == 0)
    {
        m->xDefault++;
        size_t send = ml_stmt_end(src, len, p);
        mod_add_cut(m, at, send);
        *nextOut = send;
        logger_log("[module] export default unref (removed; the module "
                   "still runs — documented boundary)");
        return;
    }
    if (tok == 0 && c0 == '{')
    {
        /* API-P10: export {local[, local [as exported]]} — the LOCAL
         * list form lands for real now: the statement cuts away, the
         * listed bindings stay plain const/var/function locals in the
         * body, and mod_build's exp postludes materialize each name
         * from its local binding (aliases included). The `from`
         * re-export form stays a documented boundary (receipted). */
        char pairLocal[JSBRIDGE_MOD_EXPORTS][JSBRIDGE_MOD_NAME_MAX];
        char pairAlias[JSBRIDGE_MOD_EXPORTS][JSBRIDGE_MOD_NAME_MAX];
        int pairCount = 0;
        int pairOverflow = 0;
        int bad = 0;
        int fromForm = 0;
        size_t q = p; /* already past '{' */
        logger_log("[module] export {list} parsing (P10 list form)");
        while (q < len)
        {
            char word2[JSBRIDGE_MOD_NAME_MAX];
            char c0b2 = 0;
            size_t st2 = 0;
            int t2 = ml_token(src, len, &q, word2, sizeof(word2), &c0b2,
                              &st2);
            if (t2 < 0)
            {
                bad = 1; /* EOF inside the list */
                break;
            }
            if (t2 == 0 && c0b2 == '}')
            {
                break; /* the list closed */
            }
            if (t2 == 0 && c0b2 == ',')
            {
                continue;
            }
            if (t2 != 1)
            {
                bad = 1; /* only idents (and , } ) fit this grammar */
                break;
            }
            /* the local name; an optional `as alias` may follow */
            if (pairCount >= JSBRIDGE_MOD_EXPORTS)
            {
                pairOverflow = 1;
            }
            else
            {
                snprintf(pairLocal[pairCount], JSBRIDGE_MOD_NAME_MAX, "%s",
                         word2);
                snprintf(pairAlias[pairCount], JSBRIDGE_MOD_NAME_MAX, "%s",
                         word2);
            }
            size_t peek = ml_space_at(src, len, q);
            size_t wEnd = 0;
            if (ml_word_at(src, len, peek, "as", 2, &wEnd))
            {
                q = wEnd;
                t2 = ml_token(src, len, &q, word2, sizeof(word2), &c0b2,
                              &st2);
                if (t2 != 1)
                {
                    bad = 1; /* `as` without the alias name */
                    break;
                }
                if (pairCount < JSBRIDGE_MOD_EXPORTS)
                {
                    snprintf(pairAlias[pairCount], JSBRIDGE_MOD_NAME_MAX,
                             "%s", word2);
                }
            }
            pairCount++;
        }
        /* the from-peek: `export {…} from 'spec'` = the re-export form */
        if (!bad && q < len)
        {
            char word3[8];
            char c0b3 = 0;
            size_t st3 = 0;
            int t3 = ml_token(src, len, &q, word3, sizeof(word3), &c0b3,
                              &st3);
            if (t3 == 1 && strcmp(word3, "from") == 0)
            {
                fromForm = 1;
                /* skip the specifier without disturbing the scan state:
                 * the cut already removes the statement through ';'. */
            }
        }
        if (fromForm || bad || pairOverflow)
        {
            m->xList++;
            logger_log("[module] export {list}%s unref (removed; the module "
                       "still runs — documented boundary)",
                       fromForm ? " from" : (bad ? " (syntax)" : " (cap)"));
            size_t send = ml_stmt_end(src, len, p);
            mod_add_cut(m, at, send);
            *nextOut = send;
            return;
        }
        for (int i = 0; i < pairCount; i++)
        {
            mod_add_export_pair(m, pairLocal[i], pairAlias[i]);
        }
        size_t send = ml_stmt_end(src, len, p);
        mod_add_cut(m, at, send);
        *nextOut = send;
        logger_log("[module] export {list} registered %zd name%s (P10 list "
                   "form)",
                   (long)pairCount, pairCount == 1 ? "" : "s");
        return;
    }
    if (tok == 0 && c0 == '*')
    logger_log("[module] export head unrecognized at byte %zu — left for "
               "the engine's SyntaxError", at);
    *nextOut = at + 6;
}

/* Pass A: ONE lexical walk of a module source; terminates at EOF. */
static void mod_scan(const char *src, size_t len, ModScan *m)
{
    size_t p = 0;
    int depth = 0;
    while (p < len)
    {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            p++;
            continue;
        }
        if (c == '/' && p + 1 < len &&
            (src[p + 1] == '/' || src[p + 1] == '*'))
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '/' && ml_regex_possible(src, p))
        {
            p = ml_regex_end(src, len, p);
            continue;
        }
        if (c == '/' || c == '"' || c == '\'' || c == '`')
        {
            p = ml_token_end(src, len, p);
            continue;
        }
        if (c == '{')
        {
            depth++;
            p++;
            continue;
        }
        if (c == '}')
        {
            if (depth > 0)
            {
                depth--;
            }
            p++;
            continue;
        }
        if (ml_isidchar((unsigned char)c) &&
            (p == 0 || !ml_isidchar((unsigned char)src[p - 1])) &&
            (p == 0 || src[p - 1] != '.'))
        {
            if (depth == 0)
            {
                size_t wEnd = 0;
                if (ml_word_at(src, len, p, "import", 6, &wEnd))
                {
                    mod_parse_import(src, len, p, m, &p);
                    continue;
                }
                if (ml_word_at(src, len, p, "export", 6, &wEnd))
                {
                    mod_parse_export(src, len, p, m, &p);
                    continue;
                }
                (void)wEnd;
            }
            else
            {
                /* API-P10: dynamic import() INSIDE nested scopes — any
                 * `import` word followed by `(` renames; statements here
                 * stay unparseable (engines raise their own SyntaxError
                 * for a bare import keyword mid-expression, unchanged).
                 * Strings/comments/templates/regex already skimmed above,
                 * so no false renames. */
                size_t wEnd = 0;
                if (ml_word_at(src, len, p, "import", 6, &wEnd))
                {
                    size_t q = ml_space_at(src, len, wEnd);
                    if (q < len && src[q] == '(')
                    {
                        m->xDyn++;
                        mod_add_ren(m, p, wEnd);
                        logger_log("[module] dynamic import() (nested) -> "
                                   "__p5di rename span (count %d)",
                                   m->xDyn);
                        p = wEnd;
                        continue;
                    }
                }
            }
            while (p < len && ml_isidchar((unsigned char)src[p]))
            {
                p++;
            }
            continue;
        }
        p++;
    }
}

static void mod_cut_end_at(const ModScan *m, size_t p, size_t *endOut)
{
    for (int i = 0; i < m->spanCount; i++)
    {
        if (p >= m->cut[i].at && p < m->cut[i].end)
        {
            *endOut = m->cut[i].end;
            return;
        }
    }
    *endOut = p + 1;
}

/* Growable (capped) accumulator for the built source. */
typedef struct
{
    char *buf;
    size_t cap;
    size_t n;
    int over;
} ModOut;

/* JS-string literal escape for keys/names inside the built source. */
static void mod_out(ModOut *o, const char *s, size_t n)
{
    if (o->over)
    {
        return;
    }
    if (o->n + n + 1 > o->cap)
    {
        o->over = 1;
        return;
    }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
}

static void mod_outs(ModOut *o, const char *s)
{
    mod_out(o, s, strlen(s));
}

static void mod_outj(ModOut *o, const char *s)
{
    for (const char *p = s; p && *p; p++)
    {
        if (*p == '\\' || *p == '"')
        {
            mod_outs(o, "\\");
            mod_out(o, p, 1);
        }
        else if (*p == '\n')
        {
            mod_outs(o, "\\n");
        }
        else if (*p == '\r')
        {
            mod_outs(o, "\\r");
        }
        else if (*p == '\t')
        {
            mod_outs(o, "\\t");
        }
        else
        {
            mod_out(o, p, 1);
        }
    }
}

/* Pass C: assemble the built (classic-clean) module source:
 *   __p5mod init; (function(){ import preludes; BODY; export
 *   postludes; })(); */
static char *mod_build(const char *src, size_t len, const ModScan *m,
                       const char *key, const char *importBase,
                       size_t *builtLenOut)
{
    char *buf = (char *)JMalloc(JSBRIDGE_MOD_BUILT_MAX);
    if (!buf)
    {
        return NULL;
    }
    ModOut o = { buf, JSBRIDGE_MOD_BUILT_MAX, 0, 0 };

    mod_outs(&o, "if (!globalThis.__p5mod) globalThis.__p5mod = {};\n");
    mod_outs(&o, "globalThis.__p5mod[\"");
    mod_outj(&o, key);
    mod_outs(&o, "\"] = {};\n(function(){\n");

    if (m->renCount > 0)
    {
        /* API-P10: the bound dynamic-import helper — one per module
         * scope, capturing THIS module's own import base so the call
         * site keeps its single argument (the spec). */
        mod_outs(&o, "var __p5di = function (spec) { return "
                     "globalThis.__p5dynimport(spec, \"");
        mod_outj(&o, importBase ? importBase : key);
        mod_outs(&o, "\"); };\n");
    }

    for (int i = 0; i < m->importCount; i++)
    {
        const ModImport *im = &m->imp[i];
        char ns[24];
        snprintf(ns, sizeof(ns), "__p5i%d", i);
        if (!m->impLoaded[i])
        {
            mod_outs(&o, "throw new Error(\"module '");
            mod_outj(&o, im->spec);
            mod_outs(&o, "' unavailable\");\n");
            continue;
        }
        mod_outs(&o, "var ");
        mod_outs(&o, ns);
        mod_outs(&o, " = globalThis.__p5mod[\"");
        mod_outj(&o, m->impKeys[i]);
        mod_outs(&o, "\"];\nif (!");
        mod_outs(&o, ns);
        mod_outs(&o, ") throw new Error(\"module '\");\n");
        for (int j = 0; j < im->namedCount; j++)
        {
            mod_outs(&o, "var ");
            mod_outs(&o, im->named[j].local);
            mod_outs(&o, " = ");
            mod_outs(&o, ns);
            mod_outs(&o, "[\"");
            mod_outj(&o, im->named[j].in);
            mod_outs(&o, "\"];\n");
        }
        if (im->dflt[0])
        {
            mod_outs(&o, "var ");
            mod_outs(&o, im->dflt);
            mod_outs(&o, " = ");
            mod_outs(&o, ns);
            mod_outs(&o, "[\"default\"];\n");
        }
        if (im->star)
        {
            mod_outs(&o, "var ");
            mod_outs(&o, im->starLocal[0] ? im->starLocal : "ns");
            mod_outs(&o, " = ");
            mod_outs(&o, ns);
            mod_outs(&o, ";\n");
        }
    }

    size_t p = 0;
    int renIdx = 0; /* next rename span (spans come sorted; renames are
                     * never inside cut spans, so sequential matching
                     * holds even with cuts interleaved). */
    while (p < len)
    {
        size_t end = 0;
        mod_cut_end_at(m, p, &end);
        if (end > p + 1)
        {
            p = end; /* inside a cut span — skip to its end */
            continue;
        }
        if (renIdx < m->renCount && p == m->ren[renIdx].at)
        {
            /* API-P10: emit the bound helper name in place of the
             * `import` keyword; the call's arguments copy onwards. */
            mod_outs(&o, "__p5di");
            p = m->ren[renIdx].end;
            renIdx++;
            continue;
        }
        while (renIdx < m->renCount && m->ren[renIdx].at <= p)
        {
            renIdx++; /* skipped past (defensive: can't happen — renames
                       * never overlap cuts — keep invariant anyway) */
        }
        mod_out(&o, src + p, 1);
        p++;
    }

    for (int i = 0; i < m->exportCount; i++)
    {
        mod_outs(&o, "try { globalThis.__p5mod[\"");
        mod_outj(&o, key);
        mod_outs(&o, "\"][\"");
        mod_outj(&o, m->exp[i]);
        mod_outs(&o, "\"] = ");
        mod_outs(&o, m->expLocal[i]);
        mod_outs(&o, "; } catch (__p5e) {}\n");
    }
    mod_outs(&o, "})();\n");
    if (o.over)
    {
        logger_log("[module] built source over cap %d — module skipped "
                   "(documented lane budget)", JSBRIDGE_MOD_BUILT_MAX);
        JFree(buf);
        return NULL;
    }
    buf[o.n] = '\0';
    *builtLenOut = o.n;
    return buf;
}

/* Fetch a module source for ledger key `key` through the page's own
 * roads: about: = the internal page store; file: = the Data-dir rule of
 * the file:// document loader (main.c pluto_load_local_file); every
 * other scheme refuses with a receipt (async graphs = future work). */
static int mod_fetch_source(const char *key, char *buf, size_t bufn,
                            size_t *lenOut)
{
    if (strncmp(key, "about:", 6) == 0)
    {
        const char *local = NULL;
        size_t llen = http_internal_page_body(key, &local);
        if (local && llen)
        {
            if (llen > bufn - 1)
            {
                logger_log("[module] store hit too big: %s (%zu > %zu)",
                           key, llen, bufn - 1);
                return 0;
            }
            memcpy(buf, local, llen);
            buf[llen] = '\0';
            *lenOut = llen;
            logger_log("[module] store hit: %s (%zu bytes)", key, llen);
            return 1;
        }
        logger_log("[module] store miss: %s", key);
        return 0;
    }
    if (strncmp(key, "file://", 7) == 0)
    {
        const char *path = key + 7;
        while (*path == '/')
        {
            path++; /* file:///Data/x → /Data/x (device), Data/x (sim) */
        }
        PlaydateAPI *pd = pluto_pd();
        if (!pd || !pd->file || !pd->file->open)
        {
            logger_log("[module] file fetch unavailable: %s", key);
            return 0;
        }
        SDFile *fp = pd->file->open(path, kFileReadData);
        if (!fp)
        {
            logger_log("[module] file open failed: %s", path);
            return 0;
        }
        pd->file->seek(fp, 0, SEEK_END);
        int flen = pd->file->tell(fp);
        pd->file->seek(fp, 0, SEEK_SET);
        if (flen <= 0 || (size_t)flen > bufn - 1)
        {
            logger_log("[module] file bad size %d: %s", flen, path);
            pd->file->close(fp);
            return 0;
        }
        int got = pd->file->read(fp, buf, (unsigned int)flen);
        pd->file->close(fp);
        if (got != flen)
        {
            logger_log("[module] file short read %d/%d: %s", got, flen, path);
            return 0;
        }
        buf[flen] = '\0';
        *lenOut = (size_t)flen;
        logger_log("[module] file hit: %s (%d bytes)", path, flen);
        return 1;
    }
    logger_log("[module] fetch refused (scheme unserved on this lane): %s",
               key);
    return 0;
}

static int mod_ledger_find(JsBridge *b, const char *key)
{
    for (int i = 0; i < b->modCount; i++)
    {
        if (strcmp(b->modKeys[i], key) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int mod_ledger_add(JsBridge *b, const char *key)
{
    if (b->modCount >= JSBRIDGE_MAX_MODULES)
    {
        return -1;
    }
    snprintf(b->modKeys[b->modCount], JSBRIDGE_MOD_KEY_MAX, "%s", key);
    b->modState[b->modCount] = 0;
    return b->modCount++;
}

static int mod_finish_run(JsBridge *b, const char *src, size_t srcLen,
                          const char *key, const char *importBase,
                          int depth, int index);
static int jsbridge_module_run_key(JsBridge *b, const char *spec,
                                   const char *importBase, int depth,
                                   char *keyOut, size_t keyOutn, int index);

/* Transform + graph-run one module whose source is IN HAND. `key` =
 * its ledger key (the namespace name); `importBase` = the URL its
 * imports resolve against (identical to key for fetched modules; the
 * document URL for inline modules whose synthetic key is not a URL). */
static int mod_finish_run(JsBridge *b, const char *src, size_t srcLen,
                          const char *key, const char *importBase,
                          int depth, int index)
{
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_script)
    {
        logger_log("[module] lane unavailable (no engine): %s", key);
        return 0;
    }
    ModScan *m = (ModScan *)JMalloc(sizeof(ModScan));
    if (!m)
    {
        logger_log("[module] scan OOM: %s", key);
        return 0;
    }
    memset(m, 0, sizeof(*m));
    mod_scan(src, srcLen, m);
    for (int i = 0; i < m->exportCount; i++)
    {
        logger_log("[module] export '%s' (key '%s')", m->exp[i], key);
    }
    if (m->xDefault || m->xList || m->xStar || m->xOther)
    {
        logger_log("[module] refusals in '%s': default=%d list=%d star=%d "
                   "other=%d (documented boundaries)",
                   key, m->xDefault, m->xList, m->xStar, m->xOther);
    }

    /* GRAPH ORDER: children first, in import-declaration order. */
    for (int i = 0; i < m->importCount; i++)
    {
        m->impLoaded[i] = (unsigned char)jsbridge_module_run_key(
            b, m->imp[i].spec, importBase, depth + 1, m->impKeys[i],
            JSBRIDGE_MOD_KEY_MAX, index);
    }

    /* API-P10: any child still IN FLIGHT (run_key returned 2 = a remote
     * module enqueued on the flight lane) defers this module's build+run
     * to the drain. mod_build never sees a pending child — with children
     * pending, the build's impLoaded==0 rows would read as REFUSALS and
     * emit throws, and the module would run half-loaded. Return the
     * pending contract instead and hand the caller the pending child
     * keys (deduped) for its flight park. */
    b->modPendCount = 0;
    memset(b->modPendKeys, 0, sizeof(b->modPendKeys));
    for (int i = 0; i < m->importCount; i++)
    {
        if (m->impLoaded[i] != 2 || !m->impKeys[i][0])
        {
            continue;
        }
        int dup = 0;
        for (int j = 0; j < b->modPendCount; j++)
        {
            if (strcmp(b->modPendKeys[j], m->impKeys[i]) == 0)
            {
                dup = 1;
                break;
            }
        }
        if (dup)
        {
            continue;
        }
        if (b->modPendCount < JSBRIDGE_MOD_IMPORTS)
        {
            snprintf(b->modPendKeys[b->modPendCount],
                     JSBRIDGE_MOD_KEY_MAX, "%s", m->impKeys[i]);
            b->modPendCount++;
        }
        else
        {
            logger_log("[module] pending-children cap %d hit on '%s' "
                       "(imports per module) — refusing one graph edge "
                       "(imports per module = %d)",
                       JSBRIDGE_MOD_IMPORTS, key, JSBRIDGE_MOD_IMPORTS);
            JFree(m);
            return 0;
        }
    }
    if (b->modPendCount > 0)
    {
        logger_log("[module] pending: '%s' waits on %d remote child%s "
                   "(first '%s')", key, b->modPendCount,
                   b->modPendCount == 1 ? "" : "ren",
                   b->modPendKeys[0]);
        JFree(m);
        return 2;
    }

    size_t builtLen = 0;
    char *built = mod_build(src, srcLen, m, key, importBase, &builtLen);
    int impCount = m->importCount;
    int expCount = m->exportCount;
    JFree(m);
    if (!built)
    {
        logger_log("[module] build failed: %s", key);
        return 0;
    }
    int preErrs = b->errs;
    impl->run_script(b, built, builtLen, index);
    if (b->errs != preErrs && b->lastError[0])
    {
        /* P10: a thrown module body used to vanish silently on engines
         * without XS's abort receipts — surface the body error here. */
        logger_log("[module] body error in '%s': %.96s", key, b->lastError);
    }
    JFree(built);
    logger_log("[module] ran key='%s' bytes=%zu imports=%d exports=%d "
               "(script #%d)",
               key, builtLen, impCount, expCount, index);
    return 1;
}

/* Resolve one import specifier to its ledger key. url_resolve does the
 * joining exactly as the page loader and the history journal do. An
 * OPAQUE about: base refuses path-only forms (the journal's rule;
 * browsers refuse it identically on opaque origins). The resolved key
 * must name a road this lane can serve: about: (store) or file: (Data
 * dir); data:/http(s)/other refuse with receipts. */
static int mod_resolve_key(const char *spec, const char *base, char *key,
                           size_t keyn)
{
    if (!spec || !spec[0])
    {
        logger_log("[module] resolve refuse: empty specifier");
        return 0;
    }
    char trimmed[JSBRIDGE_MOD_KEY_MAX];
    size_t tl = strlen(spec);
    while (tl > 0 && (spec[tl - 1] == ' ' || spec[tl - 1] == '\t' ||
                      spec[tl - 1] == '\r' || spec[tl - 1] == '\n'))
    {
        tl--;
    }
    const char *s0 = spec;
    while (*s0 == ' ' || *s0 == '\t' || *s0 == '\r' || *s0 == '\n')
    {
        s0++;
    }
    size_t sl = (size_t)(spec + tl - s0);
    if (sl >= sizeof(trimmed))
    {
        logger_log("[module] resolve refuse: specifier over %d bytes",
                   (int)sizeof(trimmed) - 1);
        return 0;
    }
    memcpy(trimmed, s0, sl);
    trimmed[sl] = '\0';

    int haveAbs = (strncmp(trimmed, "about:", 6) == 0 ||
                   strncmp(trimmed, "data:", 5) == 0 ||
                   strncmp(trimmed, "javascript:", 11) == 0 ||
                   (strstr(trimmed, "://") != NULL &&
                    strstr(trimmed, "://") - trimmed <= 10));
    if (!base || !base[0])
    {
        logger_log("[module] resolve refuse (no doc base): '%s'", trimmed);
        return 0;
    }
    char *abs = url_resolve(base, trimmed);
    if (!abs)
    {
        logger_log("[module] resolve refuse (resolver): '%s'", trimmed);
        return 0;
    }
    if (strncmp(base, "about:", 6) == 0 && !haveAbs)
    {
        /* url_resolve's about: passthrough = the opaque refusal */
        JFree(abs);
        logger_log("[module] refuse (opaque base): '%s' on '%s' — exactly "
                   "the browser's module-specifier refusal", trimmed, base);
        return 0;
    }

    int ok = 0;
    if (strncmp(abs, "about:", 6) == 0 || strncmp(abs, "file:", 5) == 0)
    {
        /* file-road flat join: our Data-dir model keeps file urls FLAT
         * ('file://wikia.html' → the Data dir's 'wikia.html' — the
         * pluto_load_local_file rule), so a path-relative import under a
         * flat file base ('./p5rel.js' on 'file://modpage.html') must
         * not carry the document's own name into the module path:
         * 'file://modpage.html/p5rel.js' drops the base name →
         * 'file://p5rel.js'. Genuinely nested paths (host 'dir',
         * '/page.html') keep the resolver's full join (the loader opens
         * them subdirectory-relative, matching the base tree). */
        if (strncmp(abs, "file://", 7) == 0 && strncmp(base, "file:", 5) == 0)
        {
            const char *baseName = base + 7;
            while (*baseName == '/')
            {
                baseName++;
            }
            const char *after = abs + 7;
            while (*after == '/')
            {
                after++;
            }
            size_t nlen = strlen(baseName);
            if (nlen && strncmp(after, baseName, nlen) == 0 &&
                (after[nlen] == '/' || after[nlen] == '\0'))
            {
                after += nlen;
                while (*after == '/')
                {
                    after++;
                }
                if (strlen(after) > 0 &&
                    (size_t)(after - abs) + strlen(after) < keyn)
                {
                    snprintf(key, keyn, "file://%s", after);
                    logger_log("[module] file-road flat join: %s -> %s "
                               "(the Data-dir loader rule)", abs, key);
                    ok = 1;
                }
                else
                {
                    logger_log("[module] refuse (key cap %d): '%s'",
                               (int)keyn - 1, abs);
                }
            }
        }
        if (!ok && strlen(abs) < keyn)
        {
            snprintf(key, keyn, "%s", abs);
            ok = 1;
        }
        else if (!ok)
        {
            logger_log("[module] refuse (key cap %d): '%s'", (int)keyn - 1,
                       abs);
        }
    }
    else if (strncmp(abs, "http://", 7) == 0 || strncmp(abs, "https://", 8) == 0)
    {
        /* API-P10: the wire roads are NOW served — resolved module-relative
         * exactly like about:/file above, then fetched on the async
         * flight lane (one at a time behind the page's own client;
         * jsbridge_modules_pump drives it). */
        if (strlen(abs) < keyn)
        {
            snprintf(key, keyn, "%s", abs);
            ok = 1;
            logger_log("[module] http serve: '%s' -> the P10 flight lane",
                       abs);
        }
        else
        {
            logger_log("[module] refuse (key cap %d): '%s'",
                       (int)keyn - 1, abs);
        }
    }
    else if (strncmp(abs, "data:", 5) == 0)
    {
        logger_log("[module] refuse (data: module): '%s'", spec);
    }
    else
    {
        logger_log("[module] refuse (scheme unserved on this lane): '%s' — "
                   "only about:/file:/http(s) roads exist", abs);
    }
    JFree(abs);
    return ok;
}

/* Graph node: resolve → ledger (cycle/cap guard) → fetch → transform →
 * run. Children nest via mod_finish_run BEFORE the importer builds.
 * API-P10: returns 2 = PENDING when the module cannot finish synchronously
 * because a REMOTE child is on the flight lane (the caller parks holding
 * its source; the drain re-enters with every child reusable). */
static int mod_flight_enqueue(JsBridge *b, const char *key, int depth,
                              int index);
static void mod_park_parent(JsBridge *b, const char *key,
                            const char *importBase, const char *src,
                            size_t srcLen, int depth, int index);
static void mod_ledger_commit(JsBridge *b, const char *key, int rc);
static void jsbridge_dyn_shim_maybe(JsBridge *b, int index);
static int jsbridge_module_run_key(JsBridge *b, const char *spec,
                                   const char *importBase, int depth,
                                   char *keyOut, size_t keyOutn, int index)
{
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !spec || !spec[0])
    {
        return 0;
    }
    if (depth > JSBRIDGE_MOD_MAX_DEPTH)
    {
        logger_log("[module] refuse (graph depth %d > %d): '%s'", depth,
                   JSBRIDGE_MOD_MAX_DEPTH, spec);
        return 0;
    }
    char key[JSBRIDGE_MOD_KEY_MAX];
    if (!mod_resolve_key(spec, importBase, key, sizeof(key)))
    {
        return 0;
    }
    if (keyOut && keyOutn)
    {
        snprintf(keyOut, keyOutn, "%s", key);
    }
    logger_log("[module] import resolve '%s' (base '%s') -> '%s'", spec,
               importBase, key);

    int idx = mod_ledger_find(b, key);
    if (idx >= 0 && b->modState[idx] == 2)
    {
        logger_log("[module] reuse: '%s'", key);
        return 1;
    }
    if (idx >= 0 && b->modState[idx] == 1)
    {
        logger_log("[module] refuse (cycle): '%s' (depth %d)", key, depth);
        return 0;
    }
    if (idx >= 0 && b->modState[idx] == 3)
    {
        /* API-P10: already parked/flighting — the importing module waits
         * exactly like a browser's importer (the graph engine that
         * started the load will finish it at the pump). */
        logger_log("[module] pending remote: '%s' already flighting "
                   "(depth %d)", key, depth);
        return 2;
    }
    if (idx < 0)
    {
        idx = mod_ledger_add(b, key);
        if (idx < 0)
        {
            logger_log("[module] refuse (graph cap %d): '%s'",
                       JSBRIDGE_MAX_MODULES, key);
            return 0;
        }
    }
    b->modState[idx] = 1;

    /* API-P10: http(s) rides the async flight lane — the sync lane never
     * served the wire roads (the old refusal receipt stays in
     * mod_fetch_source's docs). Book the flight and hand the caller the
     * pending contract; the module pump drives start → delivery → drain
     * → ledger commit → parent cascade. */
    if (strncmp(key, "http://", 7) == 0 || strncmp(key, "https://", 8) == 0)
    {
        int booked = mod_flight_enqueue(b, key, depth, index);
        int idx2 = mod_ledger_find(b, key);
        if (booked < 0)
        {
            logger_log("[module] refuse (flight cap %d): '%s'",
                       JSBRIDGE_MOD_FLIGHT_MAX, key);
            if (idx2 >= 0)
            {
                b->modState[idx2] = 2; /* failed loads stay failed */
                b->modOk[idx2] = 0;
            }
            return 0;
        }
        if (idx2 >= 0)
        {
            b->modState[idx2] = 3; /* remote-flighting */
            b->modOk[idx2] = 0;
        }
        return 2;
    }

    char *src = (char *)JMalloc(JSBRIDGE_MOD_SRC_MAX);
    if (!src)
    {
        logger_log("[module] source OOM: '%s'", key);
        return 0;
    }
    size_t len = 0;
    int ok = mod_fetch_source(key, src, JSBRIDGE_MOD_SRC_MAX, &len);
    if (ok)
    {
        ok = mod_finish_run(b, src, len, key, key, depth, index);
    }
    if (ok == 2)
    {
        /* P10: a LOCAL road parked on pending REMOTE grandchildren — the
         * flight lane holds the copied source; this slot goes to state 3
         * so a second importer ALSO waits (pending, not cycle). */
        mod_park_parent(b, key, key, src, len, depth, index);
        JFree(src);
        return 2;
    }
    JFree(src);
    mod_ledger_commit(b, key, ok);
    return ok;
}

/* The slot-road entry: the module element's own source (inline body,
 * data: payload, fetched ext module). Inline/data: modules get a
 * SYNTHETIC ledger key '<importBase>#p5mi<N>' — browsers cannot import
 * an inline module either, so its namespace is addressable by nobody;
 * ext modules key on their resolved URL (importable, like real
 * browsers' fetched modules). */
static void jsbridge_module_root(JsBridge *b, const char *src, size_t len,
                                 const char *importBase, int index)
{
    if (!src || !len)
    {
        logger_log("[module] root skip (empty body) (script #%d)", index);
        return;
    }
    if (len > JSBRIDGE_MOD_SRC_MAX)
    {
        logger_log("[module] root skip (over lane source cap %d): %zu "
                   "bytes (script #%d)",
                   JSBRIDGE_MOD_SRC_MAX, len, index);
        return;
    }
    char key[JSBRIDGE_MOD_KEY_MAX];
    snprintf(key, sizeof(key), "%s#p5mi%d",
             importBase ? importBase : "(nourl)", b->modSeq++);
    int idx = mod_ledger_add(b, key);
    if (idx < 0)
    {
        logger_log("[module] refuse (graph cap %d): inline module",
                   JSBRIDGE_MAX_MODULES);
        return;
    }
    b->modState[idx] = 1;
    /* P10: the dynamic-import shim runs BEFORE the first module body on
     * the page (the shim defines __p5dynimport/__p5dynq; pages without
     * modules never pay for the injection). */
    jsbridge_dyn_shim_maybe(b, index);
    int ok = mod_finish_run(b, src, len, key, importBase, 0, index);
    if (ok == 2)
    {
        /* P10: the root parks holding its body while remote children
         * load; the ledger stays parked (state 3 set in the park) and
         * the drain at parking-clear re-runs the FULL lane (children
         * then reuse from the ledger). */
        mod_park_parent(b, key, importBase, src, len, 0, index);
        logger_log("[module] root parked on remote children: '%s'", key);
        return;
    }
    if (ok == 0)
    {
        logger_log("[module] root failed: '%s'", key);
    }
    mod_ledger_commit(b, key, ok);
}

/* ══ API-P10: the async module lane ═════════════════════════════════════
 * http(s) module graphs + dynamic import(). The transport stays in THIS
 * router (http_client.c untouched): the client is single-flight (Lua
 * cancel-first parity, R53 graveyard), so the lane serializes ONE wire
 * fetch behind `http_is_loading()==0` + a lane-side head, with a per-
 * flight watchdog (requeue once after ~20s of silence — a superseded
 * module fetch delivers NO callback at all, so silence is the only
 * detectable supersession; terminal refuse after ~40s). Page-level
 * fetches keep byte-identical contracts; the graph = delivery → local
 * children inline → remote children enrolled as flights → drain when
 * pendCount clears → ledger done → cascade decrements parked parents. */

#define MF_WAIT_START 0 /* booked; client not started yet */
#define MF_FETCHING   1 /* on the wire */
#define MF_HOLD       2 /* bytes delivered; drain pending */
#define MF_PARKED     3 /* parent holding while remote children load */
#define MF_ERRSTOP    4 /* error receipt arrived; retry/refuse decision */
#define MOD_WATCH_REQUEUE_MS 20000u
#define MOD_WATCH_TERMINAL_MS 40000u

/* The __p5dynimport shim (shipped once per page before the first module
 * body). Engine-agnostic: JS over the per-engine __p5modRequest native.
 * muJS has no native Promise, so a small ES5 substitute rides along —
 * gates exercise exactly the .then(onr)/.then(,onj) shapes the battery
 * uses; chains defer through the router timers (setTimeout 0). */
static const char g_modShimSrc[] =
    "/* pluto dynamic-import shim (API-P10, engine-owned) */\n"
    "(function (g) {\n"
    "  if (typeof g.__p5dynimport === 'function') { return; }\n"
    "  if (typeof g.__p5modRequest !== 'function') { return; }\n"
    "  function P5P(fn) {\n"
    "    if (!(this instanceof P5P)) { return new P5P(fn); }\n"
    "    var me = this; me.s = 0; me.v = null; me.t = [];\n"
    "    function step(st, v) {\n"
    "      if (me.s) { return; } me.s = st; me.v = v;\n"
    "      var w = me.t; me.t = [];\n"
    "      for (var i = 0; i < w.length; i++) { w[i](); }\n"
    "    }\n"
    "    function ok(v) { step(1, v); }\n"
    "    function bad(e) { step(2, e); }\n"
    "    me.then = function (rf, jf) {\n"
    "      var self = me;\n"
    "      return new P5P(function (o2, b2) {\n"
    "        var run = function () {\n"
    "          var f = (self.s === 2) ? jf : rf;\n"
    "          if (typeof f !== 'function') {\n"
    "            if (self.s === 2) { b2(self.v); } else { o2(self.v); }\n"
    "            return;\n"
    "          }\n"
    "          var r;\n"
    "          try { r = f(self.v); } catch (e) { b2(e); return; }\n"
    "          if (r && r.then) { r.then(o2, b2); } else { o2(r); }\n"
    "        };\n"
    "        if (self.s) { setTimeout(run, 0); }\n"
    "        else { self.t.push(run); }\n"
    "      });\n"
    "    };\n"
    "    me['catch'] = function (f) { return me.then(null, f); };\n"
    "    try { fn(ok, bad); } catch (e) { bad(e); }\n"
    "  }\n"
    "  var Use = (typeof Promise === 'function') ? Promise : P5P;\n"
    "  g.__p5dynimport = function (spec, base) {\n"
    "    return new Use(function (res, rej) {\n"
    "      var st;\n"
    "      try { st = g.__p5modRequest(String(spec), String(base)); }\n"
    "      catch (e) { rej(new TypeError('dynamic import bridge failed')); "
    "return; }\n"
    "      if (typeof st !== 'string') {\n"
    "        rej(new TypeError('dynamic import: bad bridge reply')); return;\n"
    "      }\n"
    "      if (st.charAt(0) === 'k') {\n"
    "        var key = st.slice(2);\n"
    "        res((g.__p5mod || {})[key]);\n"
    "      }\n"
    "      else if (st.charAt(0) === 'p') {\n"
    "        var id = st.slice(2);\n"
    "        var q = g.__p5dynq || (g.__p5dynq = {});\n"
    "        q[id] = { r: res, j: rej };\n"
    "      }\n"
    "      else { rej(new TypeError(st.slice(2))); }\n"
    "    });\n"
    "  };\n"
    "  g.__p5dynq = g.__p5dynq || {};\n"
    "})(globalThis);\n";

static void jsbridge_dyn_shim_maybe(JsBridge *b, int index)
{
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_script || b->dynShimDone)
    {
        return;
    }
    b->dynShimDone = 1;
    logger_log("[module] p10 shim: injecting (before module script %d)",
               index);
    impl->run_script(b, g_modShimSrc, sizeof(g_modShimSrc) - 1, index);
    logger_log("[module] p10 shim: %s",
               b->dynShimDone ? "latched" : "unlatched");
}

/* Ledger landing for any finish point (P10): state 2 + the ok byte that
 * tells dynamic deliveries resolve vs reject (failed loads stay refused
 * for the document, the standing P5 reuse contract). */
static void mod_ledger_commit(JsBridge *b, const char *key, int rc)
{
    int idx = mod_ledger_find(b, key);
    if (idx >= 0)
    {
        b->modOk[idx] = (rc == 1);
        b->modState[idx] = 2;
    }
    if (rc == 0)
    {
        logger_log("[module] failed (stays refused for this document): "
                   "'%s'", key);
    }
}

/* Book a REMOTE module flight (idempotent by ledger key). Returns 0
 * booked (fresh or already in the table), -1 = table full (honest
 * refusal; the caller marks the ledger failed). */
static int mod_flight_enqueue(JsBridge *b, const char *key, int depth,
                              int index)
{
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        if (b->modFlight[i].set &&
            strcmp(b->modFlight[i].key, key) == 0)
        {
            logger_log("[module] net: flight already booked '%s'", key);
            return 0;
        }
    }
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        JsModFlight *f = &b->modFlight[i];
        if (f->set)
        {
            continue;
        }
        memset(f, 0, sizeof(*f));
        f->set = 1;
        f->state = MF_WAIT_START;
        snprintf(f->key, JSBRIDGE_MOD_KEY_MAX, "%s", key);
        /* A module's own import base = its own URL (children resolve
         * against the module file, the standard relative rule). */
        snprintf(f->importBase, JSBRIDGE_MOD_KEY_MAX, "%s", key);
        f->depth = depth;
        f->index = index;
        f->epoch = (unsigned)b->xhrBudgetEpoch;
        logger_log("[module] net: enqueue remote module '%s' (depth %d) "
                   "(script #%d)", key, depth, index);
        return 0;
    }
    logger_log("[module] net: flight table full (cap %d) for '%s'",
               JSBRIDGE_MOD_FLIGHT_MAX, key);
    return -1;
}

/* Park a module whose children include REMOTE loads: holds the source
 * copy, records the pending child keys (mod_finish_run's deduped
 * scratch), flips the ledger to state 3 (parked — a second importer of
 * this key gets the pending contract, not the cycle refusal). Reused for
 * roots (fresh copy) and delivered remote children (src already held). */
static void mod_park_parent(JsBridge *b, const char *key,
                            const char *importBase, const char *src,
                            size_t srcLen, int depth, int index)
{
    if (b->modPendCount <= 0)
    {
        logger_log("[module] park refused (no pending children): '%s'",
                   key);
        return;
    }
    JsModFlight *f = NULL;
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        if (b->modFlight[i].set &&
            strcmp(b->modFlight[i].key, key) == 0)
        {
            f = &b->modFlight[i];
            break;
        }
    }
    if (!f)
    {
        for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
        {
            if (!b->modFlight[i].set)
            {
                f = &b->modFlight[i];
                break;
            }
        }
    }
    if (!f)
    {
        logger_log("[module] refuse (flight table full) parking '%s' — "
                   "the node fails, children fly orphaned", key);
        mod_ledger_commit(b, key, 0);
        return;
    }
    if (!f->src)
    {
        char *hold = (char *)JMalloc(srcLen + 1);
        if (!hold)
        {
            logger_log("[module] park OOM: '%s' — fails honestly", key);
            mod_ledger_commit(b, key, 0);
            return;
        }
        memcpy(hold, src, srcLen);
        hold[srcLen] = '\0';
        f->src = hold;
        f->srcLen = srcLen;
        if (!f->set)
        {
            f->set = 1;
            f->depth = depth;
            f->index = index;
            f->state = MF_PARKED;
            snprintf(f->key, JSBRIDGE_MOD_KEY_MAX, "%s", key);
            f->epoch = (unsigned)b->xhrBudgetEpoch;
        }
    }
    f->state = MF_PARKED;
    snprintf(f->importBase, JSBRIDGE_MOD_KEY_MAX, "%s",
             importBase ? importBase : key);
    for (int i = 0; i < b->modPendCount && i < JSBRIDGE_MOD_IMPORTS; i++)
    {
        snprintf(f->childKeys[i], JSBRIDGE_MOD_KEY_MAX, "%s",
                 b->modPendKeys[i]);
    }
    f->pendCount = b->modPendCount;
    int idx = mod_ledger_find(b, key);
    if (idx >= 0)
    {
        b->modState[idx] = 3; /* parked-pending (more importers wait too) */
        b->modOk[idx] = 0;
    }
    logger_log("[module] parked '%s' on %d remote child(ren) (first "
               "'%s')", key, f->pendCount, f->childKeys[0]);
}

static void mod_flight_process(JsBridge *b, JsModFlight *f, int depth);
static void mod_pump_dynq(JsBridge *b);

/* Free a drained/refused flight slot. */
static void mod_flight_free(JsBridge *b, JsModFlight *f)
{
    (void)b;
    if (f->src)
    {
        JFree(f->src);
    }
    memset(f, 0, sizeof(*f));
}

/* The cascade: every parked parent waiting on this key decrements; a
 * parent reaching 0 drains right here (its own completion cascades one
 * level deeper — depth stays under the flight count). */
static void mod_pump_cascade(JsBridge *b, const char *doneKey, int depth)
{
    if (depth > JSBRIDGE_MOD_FLIGHT_MAX + 2)
    {
        logger_log("[module] cascade depth cap hit at '%s' (flight count "
                   "%d) — stopping deeper drains (documented bound)",
                   doneKey, JSBRIDGE_MOD_FLIGHT_MAX);
        return;
    }
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        JsModFlight *f = &b->modFlight[i];
        if (!f->set || f->state != MF_PARKED || f->pendCount <= 0)
        {
            continue;
        }
        for (int j = 0; j < JSBRIDGE_MOD_IMPORTS; j++)
        {
            if (f->childKeys[j][0] &&
                strcmp(f->childKeys[j], doneKey) == 0)
            {
                f->childKeys[j][0] = '\0'; /* consume the one wait */
                f->pendCount--;
                logger_log("[module] cascade: '%s' settled child '%s' "
                           "(pending %d)", f->key, doneKey, f->pendCount);
                break;
            }
        }
        if (f->pendCount == 0)
        {
            mod_flight_process(b, f, depth + 1);
        }
    }
}

/* Drain one flight: the standing lane (transform → import-first body →
 * export materialization) re-runs with every child now reusable or
 * failed; failures abort the body exactly as the sync lane always did
 * (the throw prelude). Flight freeing + ledger commit + parent cascade
 * ride the rc contract. depth bounds the drain-at-cascade recursion. */
static void mod_flight_process(JsBridge *b, JsModFlight *f, int depth)
{
    if (!f || !f->set || !f->src)
    {
        return;
    }
    char doneKey[JSBRIDGE_MOD_KEY_MAX];
    snprintf(doneKey, sizeof(doneKey), "%s", f->key);
    if (b->inModRun)
    {
        logger_log("[module] drain blocked (a module body is running): "
                   "'%s'", f->key);
        return;
    }
    b->inModRun = 1;
    logger_log("[module] drain: '%s' (bytes %zu, depth %d)", f->key,
               f->srcLen, f->depth);
    int preErrs = b->errs;
    int rc = mod_finish_run(b, f->src, f->srcLen, f->key, f->importBase,
                            f->depth, f->index);
    b->inModRun = 0;
    if (rc == 1 && b->errs != preErrs)
    {
        /* P10 honesty rule: the module body failed to compile or threw
         * at runtime — the namespace never materialized, so the graph
         * FAILED like a browser's failed module fetch (the dynamic
         * promise rejects; static importers fail with the standing
         * throw prelude). The sync lane's own P5 receipts are untouched
         * (this rule applies from the drain only). */
        logger_log("[module] drain body error on '%s' (errs %d -> %d) — "
                   "graph failed honestly", f->key, preErrs, b->errs);
        rc = 0;
    }
    if (rc == 2)
    {
        /* Defensive: children STILL pending at drain — re-park (this is
         * reachable only when a fresh remote edge appeared inside a
         * refused-scope import tree; the contract still finishes). */
        if (b->modPendCount > 0)
        {
            mod_park_parent(b, f->key, f->importBase, f->src, f->srcLen,
                            f->depth, f->index);
            logger_log("[module] drain re-parked '%s' on %d pending",
                       f->key, b->modPendCount);
        }
        else
        {
            f->state = MF_HOLD; /* reprocess next pump */
            logger_log("[module] drain pending without list: '%s' — "
                       "reprocessed next pump", f->key);
        }
        return;
    }
    mod_ledger_commit(b, f->key, rc);
    mod_flight_free(b, f);
    mod_pump_cascade(b, doneKey, depth);
}

/* Wire receipts (the client's callbacks; http_client.c stays untouched).
 * The lane owns exclusive head starts, so the delivery either belongs to
 * the head flight or is stale (page navigation, epoch mismatch) and is
 * ignored with a receipt. */
static JsBridge *g_modJsb = NULL;      /* the bridge that started it */
static int g_modFlightIdx = -1;        /* the head flight's table slot */

static void mod_http_error(JsBridge *b, const char *key, const char *msg)
{
    b->modFetchBusy = 0;
    JsModFlight *f = &b->modFlight[g_modFlightIdx];
    if (g_modFlightIdx >= 0 && g_modFlightIdx < JSBRIDGE_MOD_FLIGHT_MAX &&
        f->set)
    {
        f->state = MF_ERRSTOP; /* the pump decides retry vs refuse */
    }
    logger_log("[module] net: error%s%s: %s", key ? " for '" : "",
               key ? key : "", msg);
}

static void mod_http_ok(int status, char **headerKeys, char **headerVals,
                        int headerCount, const char *body, size_t bodyLen,
                        const char *url)
{
    (void)headerKeys;
    (void)headerVals;
    (void)headerCount;
    (void)url;
    JsBridge *b = g_modJsb;
    if (!b || !b->modFetchBusy || g_modFlightIdx < 0)
    {
        logger_log("[module] net: orphan delivery ignored (no head)");
        return;
    }
    JsModFlight *f = &b->modFlight[g_modFlightIdx];
    if (!f->set || f->state != MF_FETCHING ||
        f->epoch != (unsigned)b->xhrBudgetEpoch)
    {
        logger_log("[module] net: stale delivery ignored (slot/epoch)");
        b->modFetchBusy = 0;
        return;
    }
    char key[JSBRIDGE_MOD_KEY_MAX];
    snprintf(key, sizeof(key), "%s", f->key);
    b->modFetchBusy = 0;
    if (!body || bodyLen == 0)
    {
        mod_http_error(b, key, "empty body");
        return;
    }
    if (status < 200 || status >= 300)
    {
        char msg[48];
        snprintf(msg, sizeof(msg), "HTTP status %d", status);
        mod_http_error(b, key, msg);
        return;
    }
    if (bodyLen > JSBRIDGE_MOD_SRC_MAX)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "body %zu over lane source cap %d",
                 bodyLen, JSBRIDGE_MOD_SRC_MAX);
        mod_http_error(b, key, msg);
        return;
    }
    char *hold = (char *)JMalloc(bodyLen + 1);
    if (!hold)
    {
        mod_http_error(b, key, "delivery OOM");
        return;
    }
    memcpy(hold, body, bodyLen);
    hold[bodyLen] = '\0';
    f->src = hold;
    f->srcLen = bodyLen;
    f->state = MF_HOLD;
    logger_log("[module] net: delivered %zu bytes status=%d for '%s'",
               bodyLen, status, key);
}

static void mod_http_err(const char *message)
{
    JsBridge *b = g_modJsb;
    if (!b || !b->modFetchBusy || g_modFlightIdx < 0)
    {
        logger_log("[module] net: orphan error ignored: %s",
                   message ? message : "(no message)");
        return;
    }
    JsModFlight *f = &b->modFlight[g_modFlightIdx];
    char key[JSBRIDGE_MOD_KEY_MAX];
    snprintf(key, sizeof(key), "%s", f->key);
    mod_http_error(b, key, message ? message : "(no message)");
}

static void mod_flight_handle_error(JsBridge *b, JsModFlight *f,
                                    unsigned nowMs)
{
    char doneKey[JSBRIDGE_MOD_KEY_MAX];
    snprintf(doneKey, sizeof(doneKey), "%s", f->key);
    if (!f->retried)
    {
        f->retried = 1;
        f->state = MF_WAIT_START;
        f->startMs = nowMs;
        logger_log("[module] net: '%s' requeued once (watchdog/error)",
                   f->key);
        return;
    }
    logger_log("[module] net: '%s' terminal after retry — refused for "
               "this document", f->key);
    mod_ledger_commit(b, f->key, 0);
    mod_flight_free(b, f);
    mod_pump_cascade(b, doneKey, 1);
}

/* The per-frame module pump: deliveries → watchdog/error decisions →
 * the next wire head (client idle only) → dynamic-import tickets. */
void jsbridge_modules_pump(JsBridge *b, unsigned nowMs)
{
    if (!b || !b->doc || b->inModRun)
    {
        return;
    }
    /* P10 diagnostic: a 1Hz flight-state snapshot while any flight is
     * armed (the freeze paths never log otherwise). */
    {
        static unsigned modDiagMs = 0;
        int armed = 0;
        for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
        {
            if (b->modFlight[i].set)
            {
                armed = 1;
                break;
            }
        }
        if (armed && nowMs - modDiagMs >= 1000u)
        {
            modDiagMs = nowMs;
            for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
            {
                JsModFlight *fd = &b->modFlight[i];
                if (!fd->set)
                {
                    continue;
                }
                logger_log("[module] pump-diag: slot %d '%.48s' state=%d "
                           "pend=%d srcLen=%zu epoch=%u page=%u busy=%d",
                           i, fd->key, fd->state, fd->pendCount, fd->srcLen,
                           fd->epoch, (unsigned)b->xhrBudgetEpoch,
                           b->modFetchBusy);
            }
        }
    }
    if (b->modFetchStop)
    {
        b->modFetchStop = 0;
        if (!b->modFetchBusy && g_modFlightIdx >= 0)
        {
            JsModFlight *f = &b->modFlight[g_modFlightIdx];
            if (f->set && f->state == MF_FETCHING)
            {
                f->state = MF_ERRSTOP;
            }
        }
    }
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        JsModFlight *f = &b->modFlight[i];
        if (f->set && f->state == MF_HOLD)
        {
            mod_flight_process(b, f, 1);
        }
    }
    for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
    {
        JsModFlight *f = &b->modFlight[i];
        if (!f->set || f->state == MF_WAIT_START)
        {
            continue;
        }
        if (f->epoch != (unsigned)b->xhrBudgetEpoch)
        {
            /* page navigation: the whole table is stale — sweep */
            logger_log("[module] net: stale flight sweep '%s'", f->key);
            mod_flight_free(b, f);
            continue;
        }
        if (f->state == MF_FETCHING)
        {
            if (f->startMs == 0)
            {
                f->startMs = nowMs;
            }
            if (nowMs >= f->startMs + (f->retried ? MOD_WATCH_TERMINAL_MS
                                                  : MOD_WATCH_REQUEUE_MS))
            {
                if (!f->retried)
                {
                    f->retried = 1;
                    f->state = MF_WAIT_START;
                    f->startMs = nowMs;
                    logger_log("[module] net: watchdog ~20s: '%s' "
                               "requeued once", f->key);
                    continue;
                }
                logger_log("[module] net: watchdog ~40s terminal: "
                           "'%s'", f->key);
                mod_flight_handle_error(b, f, nowMs);
            }
            continue;
        }
        if (f->state == MF_ERRSTOP)
        {
            mod_flight_handle_error(b, f, nowMs);
        }
    }
    if (!b->modFetchBusy && !b->modFetchStop)
    {
        for (int i = 0; i < JSBRIDGE_MOD_FLIGHT_MAX; i++)
        {
            JsModFlight *f = &b->modFlight[i];
            if (!f->set || f->state != MF_WAIT_START ||
                f->epoch != (unsigned)b->xhrBudgetEpoch)
            {
                continue;
            }
            if (http_is_loading())
            {
                /* the page owns the wire — this head waits (receipted
                 * every pump: quiet-spam guard, one row) */
                if (f->retried || i == 0)
                {
                    logger_log("[module] net: head '%s' defers (page "
                               "fetch owns the client)", f->key);
                }
                break;
            }
            HttpCallbacks cbs;
            memset(&cbs, 0, sizeof(cbs));
            cbs.onSuccess = mod_http_ok;
            cbs.onError = mod_http_err;
            g_modJsb = b;
            g_modFlightIdx = i;
            b->modFetchBusy = 1;
            int startRc = http_get(f->key, &cbs);
            if (startRc == 0)
            {
                /* 0 = the client refused to start (validation/OOM/state
                 * machine) — 1 = the request is on the head (possibly
                 * still in the access-arm wait), the delivery receipt
                 * completes it either way. */
                logger_log("[module] net: start refused rc=0 for '%s'",
                           f->key);
                b->modFetchBusy = 0;
                f->state = MF_ERRSTOP;
                b->modFetchStop = 1;
            }
            else
            {
                f->state = MF_FETCHING;
                f->startMs = nowMs;
                logger_log("[module] net: fetch start '%s'", f->key);
            }
            break;
        }
    }
    mod_pump_dynq(b);
}

/* The dynamic-import bridge (native __p5modRequest core). Returns the
 * status STRING the shim parses: "k:<key>" resolved now, "p:<id>" a
 * ticket the pump settles, "x:<msg>" refused with the browser's message
 * class. NO module work runs here (native bracket = never re-entrant).
 * Declared in jsbridge_internal.h; each adapter registers the global. */
void jsbridge_dyn_request(JsBridge *b, const char *spec, const char *base,
                          char *out, size_t outn)
{
    out[0] = '\0';
    if (!b || !b->doc)
    {
        snprintf(out, outn, "x:dynamic import before page attach");
        return;
    }
    if (!spec || !spec[0])
    {
        snprintf(out, outn, "x:empty module specifier");
        return;
    }
    char key[JSBRIDGE_MOD_KEY_MAX];
    if (!mod_resolve_key(spec, base, key, sizeof(key)))
    {
        logger_log("[module] dyn: resolve refused '%s' (base '%s')", spec,
                   base ? base : "(nourl)");
        snprintf(out, outn, "x:Failed to resolve module specifier: %.90s",
                 spec);
        return;
    }
    int idx = mod_ledger_find(b, key);
    if (idx >= 0 && b->modState[idx] == 2 && b->modOk[idx])
    {
        logger_log("[module] dyn: immediate '%s'", key);
        snprintf(out, outn, "k:%s", key);
        return;
    }
    if (b->dynQCount >= JSBRIDGE_DYNQ_MAX)
    {
        logger_log("[module] dyn: ticket table full (cap %d) — '%s' "
                   "refused", JSBRIDGE_DYNQ_MAX, spec);
        snprintf(out, outn, "x:dynamic import queue full");
        return;
    }
    if (idx < 0)
    {
        idx = mod_ledger_add(b, key);
        if (idx < 0)
        {
            logger_log("[module] dyn: ledger cap on '%s'", key);
            snprintf(out, outn, "x:module graph at ledger cap");
            return;
        }
    }
    int id = ++b->dynIdSeq;
    b->dynQ[b->dynQCount].set = 1;
    b->dynQ[b->dynQCount].id = id;
    snprintf(b->dynQ[b->dynQCount].key, JSBRIDGE_MOD_KEY_MAX, "%s", key);
    b->dynQ[b->dynQCount].state = 0;
    b->dynQCount++;
    logger_log("[module] dyn: import('%s') base '%s' -> ticket #%d key "
               "'%s'", spec, base ? base : "(nourl)", id, key);
    snprintf(out, outn, "p:%d", id);
}

/* JS-string escape for injected keys inside the settle evals. */
static void mod_jsesc(char *dst, size_t cap, const char *s)
{
    size_t d = 0;
    for (const char *p = s; p && *p && d + 2 < cap; p++)
    {
        if (*p == '\\' || *p == '"')
        {
            dst[d++] = '\\';
        }
        dst[d++] = *p;
    }
    dst[d] = '\0';
}

/* Ticket settlements: start unstarted lanes (locals drain inline here;
 * remotes enqueue), then land resolution/rejection through the shim's
 * __p5dynq registry — a tiny eval, in the pump bracket, never inside
 * the native. */
static void mod_pump_dynq(JsBridge *b)
{
    if (b->dynQCount == 0 || b->inModRun)
    {
        return;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_script)
    {
        return;
    }
    for (int i = 0; i < JSBRIDGE_DYNQ_MAX; i++)
    {
        if (!b->dynQ[i].set)
        {
            continue;
        }
        char key[JSBRIDGE_MOD_KEY_MAX];
        snprintf(key, sizeof(key), "%s", b->dynQ[i].key);
        int id = b->dynQ[i].id;
        if (b->dynQ[i].state == 0)
        {
            b->dynQ[i].state = 1;
            b->inModRun = 1;
            int preErrs = b->errs;
            int rc = jsbridge_module_run_key(b, key, key, 0, NULL, 0, 0);
            b->inModRun = 0;
            if (rc == 1 && b->errs != preErrs)
            {
                /* P10 honesty rule (drain parity): the local module body
                 * failed to compile/throw — commit failed so the ticket
                 * settles as a rejection, never as "resolved undefined". */
                mod_ledger_commit(b, key, 0);
                logger_log("[module] dyn ticket body error on '%s' — "
                           "rejects honestly", key);
            }
            logger_log("[module] dyn: ticket #%d lane rc=%d key '%s'",
                       id, rc, key);
        }
        int idx = mod_ledger_find(b, key);
        if (idx < 0 || b->modState[idx] != 2)
        {
            continue; /* still flighting/parked — settles on commit */
        }
        char kesc[JSBRIDGE_MOD_KEY_MAX * 2];
        mod_jsesc(kesc, sizeof(kesc), key);
        char eval[JSBRIDGE_MOD_KEY_MAX * 2 + 160];
        char eval2[JSBRIDGE_MOD_KEY_MAX * 2 + 200];
        if (b->modOk[idx])
        {
            snprintf(eval, sizeof(eval),
                     "var q = globalThis.__p5dynq || {}; "
                     "var e = q[%d]; "
                     "if (e && typeof e.r === 'function') { "
                     "e.r((globalThis.__p5mod || {})['%s']); } "
                     "q[%d] = null;",
                     id, kesc, id);
            logger_log("[module] dyn: ticket #%d resolves '%s'", id, key);
        }
        else
        {
            snprintf(eval, sizeof(eval),
                     "var q = globalThis.__p5dynq || {}; "
                     "var e = q[%d]; "
                     "if (e && typeof e.j === 'function') { "
                     "e.j(new TypeError('Failed to fetch dynamically "
                     "imported module: %s')); } "
                     "q[%d] = null;",
                     id, kesc, id);
            logger_log("[module] dyn: ticket #%d rejects '%s'", id, key);
        }
        (void)eval2;
        b->inModRun = 1;
        impl->run_script(b, eval, strlen(eval), 0);
        b->inModRun = 0;
        memset(&b->dynQ[i], 0, sizeof(b->dynQ[i]));
        b->dynQCount--;
    }
}


/* ── R54: per-frame script stage (parked FULL-mode plan) ─────────────────
 * js_doc_attach parks the scanned slot plan on the bridge in document
 * order; this dispatch helper runs ONE entry with branch semantics and
 * log text IDENTICAL to the old sync attach loop (the device E1 of
 * 2026-10-18 12:02:50 killed the loop inside a parse after ~9.5s of
 * one-frame script work; see the struct field and the pump below).
 * Returns void; the pump measures DOM budget around the call. Slots'
 * spans point into doc-owned rawHtml — valid for the bridge lifetime.
 * The index handed to engines is zero-based-slot+1 (same numbering as
 * the sync loop, so log rows stay comparable). */
static void jsbridge_run_script_entry(JsBridge *b, const JsScriptSlot *slot,
                                      int index)
{
    /* The sync loop's skip/decode-failure rows print zero-based scan
     * indices (`#%d`); index-1 preserves those exact texts. */
    int i = index - 1;
    const DocParseResult *doc = b->doc;
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !doc)
    {
        return;
    }
    /* API-P5: a module element routes through the loader-side module
     * lane — same three source shapes as the classic pipeline below
     * (inline body / data: payload / fetched ext file), so the classic
     * shapes stay byte-for-byte untouched. The stamps mirror the
     * browser's rule: inline modules expose currentScript (their own
     * element); fetched module code exposes none (the ext stamp covers
     * only the element identity here, the lane logs own its receipts). */
    if (slot->isModule)
    {
        if (!slot->isExt)
        {
            jsbridge_cur_script_stamp(b, slot, NULL);
            jsbridge_module_root(b, slot->inlineStart, slot->inlineLen,
                                 doc->baseUrl, index);
            jsbridge_cur_script_clear(b);
            logger_log("[module] inline module entry #%d done",
                       index);
        }
        else if (slot->extIndex == JS_SCRIPT_DATA)
        {
            jsbridge_cur_script_stamp(b, slot, NULL);
            char *dsrc = jsext_decode_data_script(slot->inlineStart,
                                                  slot->inlineLen);
            if (dsrc)
            {
                size_t dlen = strlen(dsrc);
                jsbridge_module_root(b, dsrc, dlen, doc->baseUrl, index);
                jsbridge_cur_script_clear(b);
                JFree(dsrc);
                logger_log("[module] data: module ran (%zu bytes)", dlen);
            }
            else
            {
                jsbridge_cur_script_clear(b);
                logger_log("[module] data: module skip (decode failed) #%d",
                           i);
            }
        }
        else if (slot->extIndex >= 0 &&
                 slot->extIndex < doc->extScriptCount)
        {
            JsExtScript *e =
                &((JsExtScript *)doc->extScripts)[slot->extIndex];
            if (e->body && e->len)
            {
                jsbridge_cur_script_stamp(b, slot, e);
                jsbridge_module_root(b, e->body, e->len, e->url, index);
                jsbridge_cur_script_clear(b);
            }
            else if (e->spill >= 0 && e->len)
            {
                char *src = jsext_materialize_spill_script(e);
                if (src)
                {
                    jsbridge_cur_script_stamp(b, slot, e);
                    jsbridge_module_root(b, src, e->len, e->url, index);
                    jsbridge_cur_script_clear(b);
                    JFree(src);
                    logger_log("[module] ext module ran from disk: %s "
                               "(%zu bytes)",
                               e->url[0] ? e->url : "(url)", e->len);
                }
                else
                {
                    logger_log("[module] ext module skip (materialize "
                               "failed): %s",
                               e->url[0] ? e->url : "(url)");
                }
            }
            else
            {
                logger_log("[module] ext module skip (not fetched): %s",
                           e->url[0] ? e->url : "(unresolved)");
            }
        }
        else
        {
            logger_log("[module] ext module skip (unmapped slot) #%d", i);
        }
        return;
    }
    if (!slot->isExt)
    {
        jsbridge_cur_script_stamp(b, slot, NULL);
        impl->run_script(b, slot->inlineStart, slot->inlineLen, index);
        jsbridge_cur_script_clear(b);
    }
    else if (slot->extIndex == JS_SCRIPT_DATA)
    {
        /* SW2d: data:-URL script — decode the payload straight out of the
         * page HTML (percent-escapes or base64 per the mediatype params)
         * and run it as an inline body. Transient decode buffer:
         * allocated, run, freed — RAM holds one at a time. */
        jsbridge_cur_script_stamp(b, slot, NULL);
        char *dsrc = jsext_decode_data_script(slot->inlineStart,
                                              slot->inlineLen);
        if (dsrc)
        {
            size_t dlen = strlen(dsrc);
            impl->run_script(b, dsrc, dlen, index);
            jsbridge_cur_script_clear(b);
            JFree(dsrc);
            logger_log("[js] data: script ran (%zu bytes)", dlen);
        }
        else
        {
            jsbridge_cur_script_clear(b);
            logger_log("[js] data: script skip (decode failed) #%d", i);
        }
    }
    else if (slot->extIndex >= 0 && slot->extIndex < doc->extScriptCount)
    {
        JsExtScript *e =
            &((JsExtScript *)doc->extScripts)[slot->extIndex];
        if (e->body && e->len)
        {
            /* run_script copies the source before running, so the
             * doc-owned body is never aliased. */
            jsbridge_cur_script_stamp(b, slot, e);
            impl->run_script(b, e->body, e->len, index);
            jsbridge_cur_script_clear(b);
        }
        else if (e->spill >= 0 && e->len)
        {
            const JsEngineImpl *implNow = js_bridge_impl(b);
            if (implNow && implNow->run_script_stream)
            {
                /* R15: the engine can parse straight off the disk spill
                 * handle — no RAM copy of the source at all. */
                jsbridge_cur_script_stamp(b, slot, e);
                implNow->run_script_stream(b, e->spill, e->len, index);
                jsbridge_cur_script_clear(b);
                logger_log("[js] ext ran from disk (stream): %s (%zu bytes)",
                           e->url[0] ? e->url : "(url)", e->len);
            }
            else
            {
                /* SW2b: disk-resident source — materialize just-in-time
                 * (RAM holds one active script; disk holds the rest),
                 * run, release. */
                char *src = jsext_materialize_spill_script(e);
                if (src)
                {
                    jsbridge_cur_script_stamp(b, slot, e);
                    impl->run_script(b, src, e->len, index);
                    jsbridge_cur_script_clear(b);
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

/* Per-frame head run: consumes ONE parked entry. Same gating contract as
 * jsbridge_deferred_pump (busy engine → the head waits, retried next
 * frame): mount-parked machines (R28), timer/click re-entrancy, and a
 * mid-flight bundler split (impl->eval_pending) all hold the line.
 * mutationsOut mirrors the deferred pump: set when the entry consumed
 * DOM budget (caller schedules the standard re-render). */
void jsbridge_script_queue_pump(JsBridge *b, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->scriptQueueNext >= b->scriptQueueCount || b->inTimer ||
        b->inClick)
    {
        return;
    }
    if (jsbridge_mount_parked(b))
    {
        return; /* R28: parked mount owns the machine — retry next frame */
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (impl && impl->eval_pending && impl->eval_pending(b))
    {
        return; /* bundler split mid-flight — retry next frame */
    }
#ifdef PLUTO_R54_QUEUE_SYNC
    /* A/B seam (sim-only, NO device build defines it): drain the whole
     * parked queue synchronously — byte-identical ordering to the original
     * sync attach loop — used to bisect "per-frame gaps break the page's
     * RSC ingestion" vs "the park/drain mechanics themselves". */
    do
    {
        JsScriptSlot slot = b->scriptQueue[b->scriptQueueNext];
        int index = b->scriptQueueNext + 1;
        b->scriptQueueNext++;
        int budgetBefore = b->callBudget;
        b->inTimer = 1;
        jsbridge_run_script_entry(b, &slot, index);
        b->inTimer = 0;
        b->doc->jsRan = b->ran;
        b->doc->jsErrors = b->errs;
        snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                 b->lastError);
        if (b->callBudget < budgetBefore && mutationsOut)
        {
            *mutationsOut = 1;
        }
    } while (b->scriptQueueNext < b->scriptQueueCount);
    if (mutationsOut)
    {
        *mutationsOut = 1; /* drain-complete re-walk (same as async) */
    }
    /* R54: DOMContentLoaded at drain (same as the per-frame path). */
    {
        const JsEngineImpl *dimpl = js_bridge_impl(b);
        if (dimpl && dimpl->script_queue_drained)
        {
            dimpl->script_queue_drained(b);
        }
    }
    logger_log("[jsbridge] script queue drained (SYNC seam): %d slot(s), "
               "ran=%d errs=%d",
               b->scriptQueueCount, b->ran, b->errs);
    return;
#endif
    JsScriptSlot slot = b->scriptQueue[b->scriptQueueNext];
    int index = b->scriptQueueNext + 1; /* sync-loop numbering */
    b->scriptQueueNext++;               /* consume BEFORE running */
    b->inTimer = 1; /* same re-entrancy contract as a timer fire */
    jsbridge_run_script_entry(b, &slot, index);
    b->inTimer = 0;
    /* doc counters stay the sync loop's post-attach truth, refreshed. */
    b->doc->jsRan = b->ran;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    /* R54: NO re-walks mid-drain. A/B evidence: 1 entry/frame with
     * inter-frame re-walks → scripts 12-14 threw "missing bootstrap
     * script", byte-sync drain green. Mechanism (bundle source, module
     * 98569 in 3dmm3v_fq28gc.js): the tail scripts' __next_f pushes
     * throw once the RSC push buffer `a` is voided; the closer M runs
     * react's setTimeout fallback between frames (undefined readyState
     * pre-R54) — see xs_document_readyState / xs_script_queue_drained
     * for the readyState + DOMContentLoaded fix. Keeping the sync
     * attach loop's single-visual-pass behavior anyway: mutations
     * surface at the ONE drain-complete re-walk below. */
    if (b->scriptQueueNext >= b->scriptQueueCount)
    {
        /* R54: browser parse moment — DOMContentLoaded fires once after
         * the last script. The NR impl dispatches it to document-level
         * DCL listeners (next.js's RSC closer arms itself there; see
         * xs_script_queue_drained). Frozen engines: no hook, nothing. */
        const JsEngineImpl *dimpl = js_bridge_impl(b);
        if (dimpl && dimpl->script_queue_drained)
        {
            dimpl->script_queue_drained(b);
        }
        logger_log("[jsbridge] script queue drained: %d slot(s), ran=%d "
                   "errs=%d",
                   b->scriptQueueCount, b->ran, b->errs);
        if (mutationsOut)
        {
            *mutationsOut = 1;
        }
    }
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
        /* Phase 4: browser-true non-2xx — fetch RESOLVES for any completed
         * HTTP response with ok=false (404/500 pages stay readable), and
         * XHR fires onload with the real status. The old early return
         * dropped BOTH the status (JS saw st=0) and the body. The body is
         * stored under the same size/budget caps as 2xx; `err` stays an
         * honest failure text for consumers that read it, while status/
         * body surface the truth. */
        r->ok = 0;
        r->status = status;
        snprintf(r->err, sizeof(r->err), "HTTP %d", status);
        if (body && bodyLen && bodyLen <= (size_t)JSBRIDGE_XHR_BODY_MAX &&
            bodyLen <= (size_t)g_xhrPageBudget)
        {
            r->body = (char *)JMalloc(bodyLen + 1);
            if (r->body)
            {
                memcpy(r->body, body, bodyLen);
                r->body[bodyLen] = '\0';
                r->bodyLen = bodyLen;
                g_xhrPageBudget -= bodyLen;
                logger_log("[js] xhr #%d HTTP %d (error body %zu bytes "
                           "stored, ok=0)", r->id, status, bodyLen);
            }
            else
            {
                snprintf(r->err, sizeof(r->err), "out of memory");
                logger_log("[js] xhr #%d HTTP %d body OOM", r->id, status);
            }
        }
        else
        {
            logger_log("[js] xhr #%d HTTP %d (status stored, body %s)",
                       r->id, status,
                       (body && bodyLen) ? "dropped (cap/budget)" : "none");
        }
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
        r->status = status; /* completed HTTP response — browser keeps status */
        snprintf(r->err, sizeof(r->err), "response too large");
        logger_log("[js] xhr #%d FAIL too big (%zu bytes)", r->id, bodyLen);
        return;
    }
    if (bodyLen > g_xhrPageBudget)
    {
        r->ok = 0;
        r->status = status;
        snprintf(r->err, sizeof(r->err), "page budget exhausted");
        logger_log("[js] xhr #%d FAIL over page budget (%zu left)",
                   r->id, g_xhrPageBudget);
        return;
    }
    r->body = (char *)JMalloc(bodyLen + 1);
    if (!r->body)
    {
        r->ok = 0;
        r->status = status;
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

/* API-P3: resolve a history.pushState/replaceState URL argument against
 * the journal's CURRENT entry (not the doc base — fragment pushes stack
 * on whatever entry is current, like a browser's document URL). Forms:
 *   ''          same entry (dup of the current URL)
 *   '#frag'     keep entry path+query, swap the fragment
 *   '?q'        keep entry path, swap the query (fragment drops — the
 *               URL standard's query state doesn't inherit a fragment)
 *   '/path'     root-relative against the entry's scheme+authority
 *   'other'     path-relative per url_resolve
 *   'scheme:…'  absolute (the engine's same-origin guard then judges it)
 * about: bases are opaque: url_resolve hands '#x'/'?x' back untouched,
 * so they join onto the opaque token ('about:probe' + '#h9' →
 * 'about:probe#h9', never a stranded bare '#h9' entry). Returns 1 with
 * out filled on success, 0 on refusal (no current entry, resolver
 * failure, or no room). Never throws — the engine layer turns refusals
 * into callbacks/exceptions with their own receipts. */
static int hist_is_about_head(const char *u)
{
    char head[8];
    for (int i = 0; i < 6 && u[i]; i++)
    {
        head[i] = (char)tolower((unsigned char)u[i]);
    }
    head[6] = '\0';
    return strcmp(head, "about:") == 0;
}

int jsbridge_hist_resolve_url(JsBridge *b, const char *raw,
                              char *out, size_t outLen)
{
    if (!out || outLen == 0)
    {
        return 0;
    }
    out[0] = '\0';
    const char *cur = (b->histCount > 0 && b->histUrl[b->histIdx][0])
                          ? b->histUrl[b->histIdx]
                          : ((b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl : NULL);
    if (!cur || !cur[0])
    {
        return 0;
    }
    if (!raw)
    {
        raw = "";
    }
    /* url_resolve('') dups the base — the same-document push form. */
    char *abs = url_resolve(cur, raw);
    if (!abs)
    {
        return 0;
    }
    if ((raw[0] == '#' || raw[0] == '?') && strcmp(abs, raw) == 0 &&
        hist_is_about_head(cur))
    {
        /* opaque join with field replacement (the URL standard's
         * relative forms): '#raw' replaces the fragment and KEEPS the
         * path+query prefix; '?raw' replaces the query AND drops any
         * fragment. Keeps the opaque token, never strands a bare
         * '#h9'/'?a=2' entry, never double-appends ('#h11' + '#h9' is
         * ONE fragment field, not '...#h11#h9'). */
        const char *q = strchr(cur, '?');
        const char *fr = strchr(cur, '#');
        size_t kq = q ? (size_t)(q - cur) : strlen(cur);
        size_t kf = fr ? (size_t)(fr - cur) : strlen(cur);
        size_t keep = (raw[0] == '?') ? ((kq < kf) ? kq : kf) : kf;
        if (keep >= outLen - strlen(raw) - 1)
        {
            JFree(abs);
            out[0] = '\0';
            return 0;
        }
        memcpy(out, cur, keep);
        out[keep] = '\0';
        strcat(out, raw);
        JFree(abs);
        return 1;
    }
    /* An about: base strands path-only/scheme-relative forms — refuse
     * rather than record a relative fragment as an "address". */
    if (hist_is_about_head(cur) && abs[0] != '\0' && abs[0] != '#' &&
        strstr(abs, "://") == NULL && strncmp(abs, "about:", 6) != 0)
    {
        JFree(abs);
        return 0;
    }
    int need = snprintf(out, outLen, "%s", abs);
    JFree(abs);
    return need >= 0 && (size_t)need < outLen;
}

int jsbridge_xhr_open(JsBridge *b, const char *method, const char *url)
{
    if (!b)
    {
        return 0;
    }
#if defined(PLUTO_HYDRATE_TRACE)
    logger_log("[xhr] router open method='%.16s' url='%.50s'",
               method ? method : "(null)", url ? url : "(null)");
#endif
    /* C4: POST (and other verbs that imply a body in practice) joins GET.
     * The stored METHOD drives both the engine (fetch shim passes
     * POST) and the wire path (http_request). Case-insensitive input,
     * upcased storage. NULL keeps the old contract (error), and the
     * unsupported-verb error text stays byte-identical for the frozen
     * engines' logs. */
    if (!method || !method[0])
    {
        bridge_take_error_text(b, "xhr: only GET is supported");
        return 0;
    }
    char ucMethod[8];
    memset(ucMethod, 0, sizeof(ucMethod));
    for (int i = 0; i < (int)sizeof(ucMethod) - 1 && method[i]; i++)
    {
        char c = method[i];
        ucMethod[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    int isGet = (strcmp(ucMethod, "GET") == 0);
    if (!isGet && strcmp(ucMethod, "POST") != 0)
    {
#if defined(PLUTO_HYDRATE_TRACE)
        logger_log("[xhr] router reject: ucMethod='%.8s' raw='%.16s'",
                   ucMethod, method);
#endif
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
    snprintf(r->method, sizeof(r->method), "%s", isGet ? "GET" : ucMethod);
    logger_log("[js] xhr #%d open %s %s", r->id, r->method, r->url);
    return r->id;
}

/* C4: send variant that carries the REQUEST BODY (fetch shim's POST JSON).
 * Additive twin of jsbridge_xhr_send (frozen engines keep calling the
 * body-less twin; body is deep-copied so the caller's string can be a
 * transient). */
/* API-P2: the fetch shim's setRequestHeader lane. Content-Type overrides
 * the wire Content-Type (the request builder's replace lane; last write
 * wins). NetP2: every OTHER header is REAL — stored per-request and
 * carried byte-for-byte onto the wire by the client's extra-header lane.
 * Browser-faithful guard: the spec/engine-owned forbidden set and any
 * CRLF/control byte refuse with a logged reason (nothing invented,
 * nothing silently dropped); a repeated name joins its values with the
 * browser's ", " fold; the list is capped at JSBRIDGE_XHR_EXTRA_MAX
 * pairs with an honest refusal past the cap. Only valid while the
 * request is OPENED, like a browser. */
int jsbridge_xhr_set_header(JsBridge *b, int id, const char *name,
                            const char *value)
{
    if (!b || id <= 0 || !name || !name[0])
    {
        return 0;
    }
    for (int i = 0; i < b->xhrCount; i++)
    {
        JsHttpRequest *r = &b->xhr[i];
        if (r->active && r->id == id)
        {
            if (r->state != JS_XHR_OPENED)
            {
                bridge_take_error_text(b, "xhr: header after send");
                return 0;
            }
            char lname[40];
            snprintf(lname, sizeof(lname), "%s", name);
            for (int c = 0; lname[c]; c++)
            {
                if (lname[c] >= 'A' && lname[c] <= 'Z')
                {
                    lname[c] = (char)(lname[c] + 32);
                }
            }
            if (strcmp(lname, "content-type") == 0)
            {
                snprintf(r->reqCt, sizeof(r->reqCt), "%s",
                         value ? value : "");
                logger_log("[js] xhr #%d header content-type: %.60s",
                           r->id, r->reqCt);
                return 1;
            }
            /* NetP2 wire-integrity guard (mirrors the client's): name
             * must be a printable token, value printable ASCII, and the
             * framing/authority/client-lane names are refused before
             * they are ever stored — logging the honest reason. */
            {
                int bad = 0;
                for (const char *p = name; *p && !bad; p++)
                {
                    if (*p < 0x21 || *p > 0x7e)
                    {
                        bad = 1;
                    }
                }
                for (const char *p = value ? value : ""; *p && !bad; p++)
                {
                    if (*p < 0x20 || *p > 0x7e)
                    {
                        bad = 1;
                    }
                }
                if (bad)
                {
                    logger_log("[js] xhr #%d header refused "
                               "(CRLF/control byte): %.40s",
                               r->id, name);
                    return 0;
                }
                static const char *const FORBIDDEN[] = {
                    "accept-charset", "accept-encoding",
                    "access-control-request-headers",
                    "access-control-request-method", "connection",
                    "content-length", "cookie", "cookie2", "date", "dnt",
                    "expect", "host", "if-modified-since",
                    "if-none-match", "keep-alive", "origin", "referer",
                    "te", "trailer", "transfer-encoding", "upgrade",
                    "via"};
                for (size_t fi = 0; fi < sizeof(FORBIDDEN) /
                                             sizeof(FORBIDDEN[0]);
                     fi++)
                {
                    if (strcmp(lname, FORBIDDEN[fi]) == 0)
                    {
                        logger_log("[js] xhr #%d header refused "
                                   "(engine-owned/spec-forbidden): %s",
                                   r->id, name);
                        return 0;
                    }
                }
                if (strncmp(lname, "proxy-", 6) == 0 ||
                    strncmp(lname, "sec-", 4) == 0)
                {
                    logger_log("[js] xhr #%d header refused "
                               "(spec-forbidden family): %s",
                               r->id, name);
                    return 0;
                }
            }
            for (int xi = 0; xi < r->xhdrCount; xi++)
            {
                if (strcasecmp(r->xhdrName[xi], name) == 0)
                {
                    int vf = (int)strlen(r->xhdrVal[xi]);
                    int al = (int)strlen(value ? value : "");
                    if (vf + 2 + al < (int)sizeof(r->xhdrVal[xi]))
                    {
                        snprintf(r->xhdrVal[xi] + vf,
                                 sizeof(r->xhdrVal[xi]) - vf, ", %s",
                                 value ? value : "");
                        logger_log("[js] xhr #%d header %s merged "
                                   "(now %.60s)",
                                   r->id, name, r->xhdrVal[xi]);
                        return 1;
                    }
                    logger_log("[js] xhr #%d header %s merge overflow "
                               "(honest refusal, %d exists)",
                               r->id, name, vf);
                    return 0;
                }
            }
            if (r->xhdrCount >= JSBRIDGE_XHR_EXTRA_MAX)
            {
                logger_log("[js] xhr #%d header lane full (%d) — "
                           "refused: %s (logged, never sent)",
                           r->id, JSBRIDGE_XHR_EXTRA_MAX, name);
                return 0;
            }
            snprintf(r->xhdrName[r->xhdrCount],
                     sizeof(r->xhdrName[r->xhdrCount]), "%s", name);
            snprintf(r->xhdrVal[r->xhdrCount],
                     sizeof(r->xhdrVal[r->xhdrCount]), "%s",
                     value ? value : "");
            logger_log("[js] xhr #%d header %s: %.60s (rides the "
                       "wire — NetP2 lane)",
                       r->id, name, value ? value : "");
            r->xhdrCount++;
            return 1;
        }
    }
    logger_log("[js] xhr #%d header refused (no open request)", id);
    return 0;
}

void jsbridge_xhr_send_body(JsBridge *b, int id, const char *body,
                            size_t bodyLen, void *fnRef, void *errFnRef,
                            void *objRef)
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
            if (body && bodyLen && !r->reqBody &&
                strcmp(r->method, "POST") == 0)
            {
                r->reqBody = (char *)JMalloc(bodyLen);
                if (r->reqBody)
                {
                    memcpy(r->reqBody, body, bodyLen);
                    r->reqBodyLen = bodyLen;
                }
                else
                {
                    bridge_take_error_text(b, "xhr: out of memory (body)");
                    return;
                }
            }
            break;
        }
    }
    jsbridge_xhr_send(b, id, fnRef, errFnRef, objRef);
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
    /* C4: POST requests ride http_request with the stored request body;
     * every GET (and any never-bodied request) keeps the old http_get path
     * byte-for-byte. NetP2: when the page stored extra headers on this
     * request, the list rides along (the pair list copies OUT of the
     * record snapshot-first; the client owns a deep copy before the call
     * returns). Static, not stack: the send road runs inside the JS pump
     * (eval frames already sit deep on the game-task stack) and cannot
     * re-enter — the client is single-flight and the copy completes
     * synchronously before any callback can fire. */
    int started;
    static HttpHeaderPair pairs[JSBRIDGE_XHR_EXTRA_MAX];
    int npairs = 0;
    for (int xi = 0; xi < r->xhdrCount; xi++)
    {
        if (npairs >= JSBRIDGE_XHR_EXTRA_MAX)
        {
            break;
        }
        snprintf(pairs[npairs].name, sizeof(pairs[npairs].name), "%s",
                 r->xhdrName[xi]);
        snprintf(pairs[npairs].value, sizeof(pairs[npairs].value), "%s",
                 r->xhdrVal[xi]);
        npairs++;
    }
    if (r->method[0] && strcmp(r->method, "POST") == 0 && r->reqBody)
    {
        /* R56f: expose the POST payload's lead bytes so the native form
         * fallback can replicate the page's exact body format. */
        char bodyTxt[112];
        int bodyCut = (int)r->reqBodyLen;
        if (bodyCut > 96)
            bodyCut = 96;
        snprintf(bodyTxt, sizeof(bodyTxt), "%.*s", bodyCut, r->reqBody);
        logger_log("[js] xhr #%d POST body %u first96='%s'", r->id,
                   (unsigned)r->reqBodyLen, bodyTxt);
        /* API-P2: a setRequestHeader Content-Type rides the wire here.
         * NULL (no header set) keeps the historical application/json
         * default — no pre-existing path changes. */
        if (r->reqCt[0])
        {
            logger_log("[js] xhr #%d POST ct='%s'", r->id, r->reqCt);
        }
        if (npairs > 0)
        {
            logger_log("[js] xhr #%d POST extras=%d (NetP2 wire lane)",
                       r->id, npairs);
            started = http_request_h(r->method, r->url,
                                     r->reqCt[0] ? r->reqCt : NULL,
                                     pairs, npairs, r->reqBody,
                                     r->reqBodyLen, &cbs);
        }
        else
        {
            started = http_request_ct(r->method, r->url,
                                      r->reqCt[0] ? r->reqCt : NULL,
                                      r->reqBody, r->reqBodyLen, &cbs);
        }
    }
    else if (npairs > 0)
    {
        /* NetP2: GET (or the never-bodied road) carries the page's header
         * list in the same cancel-first shape. Method semantics match
         * the existing else-branch (GET on the wire) so pre-NetP2
         * behavior holds whenever no extra header was set. */
        logger_log("[js] xhr #%d send extras=%d (NetP2 wire lane)",
                   r->id, npairs);
        started = http_get_headers(r->url, pairs, npairs, &cbs);
    }
    else
    {
        started = http_get(r->url, &cbs);
    }
    if (!started)
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

void jsbridge_xhr_send(JsBridge *b, int id, void *fnRef, void *errFnRef,
                       void *objRef)
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
             * the request starts now or queues behind the in-flight one.
             * errFnRef==0 means the engine pinned no separate onerror
             * (the dispatch falls back to fnRef). */
            r->fnRef = fnRef;
            r->objRef = objRef;
            r->errFnRef = errFnRef;
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

int jsbridge_take_dom_dirty(JsBridge *b)
{
    if (!b || !b->domDirty)
    {
        return 0;
    }
    b->domDirty = 0;
    logger_log("[dom] dirty: innerHTML assignment triggered rewalk");
    return 1;
}

/* ── Phase 10: transition-lane mutation notes ─────────────────────────────
 * Ring with in-ring dedupe: a node mutated twice in one batch (multiple
 * attribute writes) notes once — drain-time the browser consults the
 * node once per batch, browser-style (one event per change batch). */
void jsbridge_note_dom_mutation(JsBridge *b, void *node)
{
    if (!b || !node)
    {
        return;
    }
    for (int i = 0; i < b->mutNoteCount; i++)
    {
        if (b->mutNotes[i].node == node)
        {
            return; /* already noted this batch */
        }
    }
    if (b->mutNoteCount < JSB_MUT_NOTES_MAX)
    {
        b->mutNotes[b->mutNoteCount++].node = node;
    }
}

int jsbridge_take_mut_notes(JsBridge *b, void **out, int max)
{
    int n = 0;
    if (b)
    {
        for (int i = 0; i < b->mutNoteCount && out && n < max; i++)
        {
            out[n++] = b->mutNotes[i].node;
        }
        b->mutNoteCount = 0; /* dropped notes are gone (bounded lane) */
    }
    return n;
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
    if (jsbridge_mount_parked(b))
    {
        return 0; /* R28: parked mount owns the machine — completions wait */
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
         * never reorders the table, so `r` stays valid across the call.
         * Phase 4 browser-true error routing: a TRANSPORT failure
         * (status==0 with ok!=1 — connect refused, timeout, OOM at open)
         * dispatches the pinned onerror (fetch REJECTS on network
         * failures; XHR fires onerror). Every COMPLETED HTTP response —
         * 2xx, 404, 500 — keeps the success dispatch (onload), with the
         * bridge having stored the real status and body. Engines without
         * an error pin (everything but NR today) fall back to fnRef. */
        void *useFn = r->fnRef;
        if (r->status == 0 && r->ok != 1 && r->errFnRef)
        {
            useFn = r->errFnRef;
        }
        b->inXhr = 1;
        int rc = impl->run_xhr_ref(b, useFn, r->objRef, r);
        b->inXhr = 0;
        if (rc < 0)
        {
            /* Phase 4: name the error — every engine receipted this line
             * without ever seeing WHY (the impl stores the thrown text in
             * lastError; browsers log uncaught completion errors the same
             * way). */
            logger_log("[js] xhr completion aborted the engine (%s) — stop",
                       b->lastError[0] ? b->lastError : "no error text");
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
                impl->clear_xhr_refs(b, r->fnRef, r->errFnRef, r->objRef);
                if (r->body)
                {
                    JFree(r->body);
                }
                /* C4: free the request body alongside the response (see
                 * the close-path sweep note). */
                if (r->reqBody)
                {
                    JFree(r->reqBody);
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

/* ── API-P8: the Worker contained-stub router (see the JsWorkerStub note
 * in jsbridge_internal.h) ───────────────────────────────────────────── */

/* Constructor helper: register a stub (id 0 = the documented-cap
 * delivery-less shape — the object still works, the page keeps running). */
int jsbridge_worker_open(JsBridge *b, const char *url)
{
    if (!b)
    {
        return 0;
    }
    JsWorkerStub *w = NULL;
    for (int i = 0; i < b->wkCount; i++)
    {
        if (!b->wk[i].active && !b->wk[i].release)
        {
            w = &b->wk[i]; /* reuse a fully-dead slot */
            break;
        }
    }
    if (!w && b->wkCount < JSBRIDGE_WORKER_MAX)
    {
        w = &b->wk[b->wkCount++];
    }
    if (!w)
    {
        b->wkIdSeq++; /* ids stay monotonic for delivery-less stubs too */
        if (!b->wkOverflow)
        {
            logger_log("[worker] stub table full (%d) — past here stubs are "
                       "delivery-less (documented cap)",
                       JSBRIDGE_WORKER_MAX);
        }
        b->wkOverflow = 1;
        return 0;
    }
    memset(w, 0, sizeof(*w));
    w->active = 1;
    w->id = ++b->wkIdSeq;
    if (url && url[0])
    {
        snprintf(w->url, sizeof(w->url), "%s", url);
    }
    snprintf(w->message, sizeof(w->message),
             "the worker script is not running on this device "
             "(single-threaded browser: worker stub)");
    logger_log("[worker] construct #%d url='%.60s' (contained stub — "
               "script never compiles or runs)",
               w->id, w->url);
    return w->id;
}

const JsWorkerStub *jsbridge_worker_get(JsBridge *b, int id)
{
    if (!b || id <= 0)
    {
        return NULL;
    }
    for (int i = 0; i < b->wkCount; i++)
    {
        if (b->wk[i].id == id)
        {
            return &b->wk[i];
        }
    }
    return NULL;
}

/* terminate(): browser semantics — terminate BEFORE the error event is
 * delivered cancels it (browsers: the script load never settles); after
 * delivery or on an unknown/delivery-less stub it is a silent no-op. */
void jsbridge_worker_terminate(JsBridge *b, int id)
{
    if (!b || id <= 0)
    {
        return;
    }
    for (int i = 0; i < b->wkCount; i++)
    {
        JsWorkerStub *w = &b->wk[i];
        if (w->id == id && (w->active || w->release))
        {
            if (w->active && !w->terminated)
            {
                w->terminated = 1;
                logger_log("[worker] terminate #%d (pending delivery "
                           "cancelled)", id);
            }
            return;
        }
    }
}

/* postMessage(): always absorbed — messages are never delivered back
 * (onmessage cannot fire; the data is dropped). Receipt = logarithmic:
 * the first two posts log per stub, then a single "silently" line. */
void jsbridge_worker_post(JsBridge *b, int id)
{
    if (!b)
    {
        return;
    }
    if (id <= 0)
    {
        /* Delivery-less stub (documented cap): absorb + one receipt. */
        if (!b->wkOverflow)
        {
            logger_log("[worker] postMessage absorbed (delivery-less stub)");
        }
        b->wkOverflow = 1;
        return;
    }
    for (int i = 0; i < b->wkCount; i++)
    {
        JsWorkerStub *w = &b->wk[i];
        if (w->id == id && (w->active || w->release))
        {
            w->posts++;
            if (w->posts <= 2)
            {
                logger_log("[worker] postMessage #%d absorbed (stub: data "
                           "dropped — no thread)", id);
            }
            else if (w->posts == 3)
            {
                logger_log("[worker] #%d further posts absorbed silently", id);
            }
            return;
        }
    }
}

/* Engine-side pin: hand the router a stub's engine-held object ref.
 * Set once at construct (the stub delivers once); matching-id + NULL-ref
 * guards mean late/duplicate/mismatched pins are silent no-ops. */
void jsbridge_worker_pin(JsBridge *b, int id, void *objRef)
{
    if (!b || id <= 0 || !objRef)
    {
        return;
    }
    for (int i = 0; i < b->wkCount; i++)
    {
        if (b->wk[i].id == id && !b->wk[i].objRef)
        {
            b->wk[i].objRef = objRef;
            return;
        }
    }
}

/* The worker pump: deliver every non-terminated stub's error event ONCE
 * (the next-frame analog of a failed script load). Same bracket +
 * sweep + mutation discipline as jsbridge_xhr_pump. */
int jsbridge_worker_pump(JsBridge *b, int *mutationsOut)
{
    if (mutationsOut)
    {
        *mutationsOut = 0;
    }
    if (!b || b->inXhr || b->inTimer || b->inClick || b->inWorker)
    {
        return 0;
    }
    if (jsbridge_mount_parked(b))
    {
        return 0; /* parked mount owns the machine — deliveries wait */
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->run_worker_ref || !impl->clear_worker_refs)
    {
        return 0;
    }
    int delivered = 0;
    int mutations = 0;
    int budgetBefore = b->callBudget;
    for (int i = 0; i < b->wkCount; i++)
    {
        JsWorkerStub *w = &b->wk[i];
        if (!w->active || w->terminated || !w->objRef)
        {
            continue;
        }
        b->inWorker = 1;
        int rc = impl->run_worker_ref(b, w->objRef, w);
        b->inWorker = 0;
        if (rc < 0)
        {
            logger_log("[js] worker delivery aborted the engine — stop");
            break;
        }
        if (b->callBudget < budgetBefore)
        {
            mutations = 1;
            budgetBefore = b->callBudget;
        }
        w->active = 0;
        w->release = 1;
        delivered++;
    }
    /* Sweep (outside every bracket): release refs, compact — the same
     * discipline as the timer/XHR sweeps. */
    if (!b->inWorker && !b->inXhr && !b->inTimer && !b->inClick)
    {
        for (int i = 0; i < b->wkCount; i++)
        {
            JsWorkerStub *w = &b->wk[i];
            if (w->release && !w->active)
            {
                impl->clear_worker_refs(b, w->objRef);
                memset(w, 0, sizeof(*w));
            }
        }
        int wr = 0;
        for (int rd = 0; rd < b->wkCount; rd++)
        {
            if (b->wk[rd].active || b->wk[rd].release)
            {
                if (wr != rd)
                {
                    b->wk[wr] = b->wk[rd];
                }
                wr++;
            }
        }
        memset(&b->wk[wr], 0, sizeof(b->wk[0]) * (size_t)(b->wkCount - wr));
        b->wkCount = wr;
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

/* ── element event dispatch (B2: routed like link clicks) ────────────────── */
int jsbridge_dispatch_element_event(JsBridge *b, const void *node,
                                    const char *type)
{
    if (!b || b->inClick || !node || !type)
    {
        return JSB_EVENT_NONE;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    if (!impl || !impl->dispatch_event /* NULL = frozen engines: native */)
    {
        return JSB_EVENT_NONE;
    }
    return impl->dispatch_event(b, node, type);
}

int jsbridge_listener_count(const JsBridge *b)
{
    return b ? b->listenerCount : 0;
}

/* R30o: see jsbridge.h. Every axis that could still call back into the
 * engine must be quiet: listeners (click handlers), timers (setTimeout /
 * setInterval, including inert slots with a deferred engine ref), and XHR
 * (in-flight flag plus any active/pending request entry — the xhrInFlight
 * short-circuit alone would miss a queued-but-not-yet-sent request). */
int jsbridge_is_idle(const JsBridge *b)
{
    if (!b)
    {
        return 0;
    }
    if (b->listenerCount != 0 || b->timerCount != 0 || b->xhrInFlight)
    {
        return 0;
    }
    for (int i = 0; i < b->xhrCount; i++)
    {
        if (b->xhr[i].active || b->xhr[i].release)
        {
            return 0;
        }
    }
    return 1;
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

/* ── Forms-Phase 3: the native constraint validation layer ──────────────── *
 * Browser truth underneath page JS: every submit attempt and every
 * checkValidity()/reportValidity() consults these rules — a page's own
 * JS validation keeps working above this layer untouched. The stores
 * are the two-armed model: typed text/JS writes live on the value
 * ATTRIBUTE; user taps live on the DocBlock; pre-walk scripts see the
 * markup attributes alone. The JSBV_* flag bits and the JsbValidity
 * struct live in jsbridge_internal.h (adapters consume both). */

static int jsbv_absent_int(void)
{
    return -2147483647;
}

/* The willValidate candidate rule (browser): submittable-and-not-barred
 * controls. Hidden/button/submit/reset/image inputs and <button> are
 * barred; disabled controls are barred; text-family readonly barred; a
 * multiple select barred. (Fieldset-inherited disabled does NOT bar
 * here: standing `.disabled` reflection is own-attribute only.) */
int jsbridge_form_will_validate(JsBridge *b, DomNode *n)
{
    (void)b;
    if (!n || n->kind != DOM_ELEMENT || !n->tag)
    {
        return 0;
    }
    if (strcmp(n->tag, "input") == 0)
    {
        const char *tv = dom_get_attr(n, "type");
        const char *t = (tv && tv[0]) ? tv : "text";
        if (strcmp(t, "hidden") == 0 || strcmp(t, "button") == 0 ||
            strcmp(t, "submit") == 0 || strcmp(t, "reset") == 0 ||
            strcmp(t, "image") == 0)
        {
            return 0;
        }
        if (dom_get_attr(n, "disabled"))
        {
            return 0;
        }
        if (dom_get_attr(n, "readonly"))
        {
            const char *ro[] = {"text", "search", "email", "url",
                                "password", "tel", "number"};
            for (size_t i = 0; i < sizeof(ro) / sizeof(ro[0]); i++)
            {
                if (strcmp(t, ro[i]) == 0)
                {
                    return 0;
                }
            }
        }
        return 1;
    }
    if (strcmp(n->tag, "textarea") == 0)
    {
        return !dom_get_attr(n, "disabled") && !dom_get_attr(n, "readonly");
    }
    if (strcmp(n->tag, "select") == 0)
    {
        return !dom_get_attr(n, "disabled") && !dom_get_attr(n, "multiple");
    }
    return 0;
}

/* The LIVE value of a control for the rules:
 * text-family input → the value attribute; textarea → its concatenated
 * DOM text; select → the selected option's value attribute else its
 * text — from the LIVE block when one exists (taps move the block),
 * else the markup `selected` option, else the first option (the
 * browser's default-selection rule); toggles → "" (their missing-ness
 * is about checkedness, handled in the evaluator). */
static void jsbv_value_of(JsBridge *b, DomNode *n, char *out, size_t cap)
{
    out[0] = '\0';
    const char *tag = (n && n->kind == DOM_ELEMENT && n->tag) ? n->tag : NULL;
    if (!tag)
    {
        return;
    }
    if (strcmp(tag, "textarea") == 0)
    {
        doc_concat_node_text(n, out, cap);
        return;
    }
    if (strcmp(tag, "select") == 0)
    {
        DocBlock *blk = NULL;
        if (b && b->doc)
        {
            for (int i = 0; i < b->doc->blockCount && !blk; i++)
            {
                DocBlock *it = b->doc->blocks[i];
                if (it && it->srcNode == (void *)n)
                {
                    blk = it;
                }
            }
        }
        if (blk && blk->options && blk->optionCount > 0)
        {
            DocOption *o = NULL;
            if (blk->multiple)
            {
                for (int k = 0; k < blk->optionCount && !o; k++)
                {
                    DocOption *c = blk->options[k];
                    if (c && !c->group && c->selected)
                    {
                        o = c;
                    }
                }
            }
            else
            {
                int si = blk->selectedIndex;
                if (si >= 1 && si <= blk->optionCount)
                {
                    o = blk->options[si - 1];
                }
            }
            if (o && !o->group)
            {
                const char *v = (o->value && o->value[0]) ? o->value : o->text;
                snprintf(out, cap, "%s", v ? v : "");
            }
            return;
        }
        const char *pick = NULL;
        DomNode *first = NULL;
        for (int i = 0; i < n->childCount; i++)
        {
            DomNode *c = n->children[i];
            if (!c || c->kind != DOM_ELEMENT || !c->tag ||
                strcmp(c->tag, "option") != 0)
            {
                continue;
            }
            if (!first)
            {
                first = c;
            }
            if (dom_get_attr(c, "selected"))
            {
                pick = dom_get_attr(c, "value");
                if (!pick)
                {
                    char tbuf[256];
                    doc_concat_node_text(c, tbuf, sizeof(tbuf));
                    snprintf(out, cap, "%s", tbuf);
                    return;
                }
                snprintf(out, cap, "%s", pick);
                return;
            }
        }
        if (first)
        {
            const char *v = dom_get_attr(first, "value");
            if (v)
            {
                snprintf(out, cap, "%s", v);
                return;
            }
            doc_concat_node_text(first, out, cap);
        }
        return;
    }
    /* input family: the attr store */
    const char *v = dom_get_attr(n, "value");
    snprintf(out, cap, "%s", v ? v : "");
}

static int jsbv_attr_int(DomNode *n, const char *key, int *present)
{
    const char *v = dom_get_attr(n, key);
    *present = 0;
    if (!v || !v[0])
    {
        return jsbv_absent_int();
    }
    char *end = NULL;
    long val = strtol(v, &end, 10);
    if (end == v)
    {
        return jsbv_absent_int();
    }
    *present = 1;
    return (int)val;
}

/* Any checked radio with the group's name, by DOM attr road. */
static int jsbv_group_attr_checked(DomNode *n, const char *name, int depth)
{
    if (!n || depth > 16)
    {
        return 0;
    }
    if (n->kind == DOM_ELEMENT && n->tag && strcmp(n->tag, "input") == 0)
    {
        const char *tv = dom_get_attr(n, "type");
        if (tv && strcmp(tv, "radio") == 0)
        {
            const char *nm = dom_get_attr(n, "name");
            if (nm && name && strcmp(nm, name) == 0 &&
                dom_get_attr(n, "checked"))
            {
                return 1;
            }
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        if (jsbv_group_attr_checked(n->children[i], name, depth + 1))
        {
            return 1;
        }
    }
    return 0;
}

/* The doc-wide radio group pick check: the LIVE block road first (taps
 * flip blocks), else the markup attribute road. */
static int jsbv_group_checked(JsBridge *b, DomNode *n, const char *name)
{
    if (!name || !name[0])
    {
        return 0;
    }
    if (b && b->doc)
    {
        for (int i = 0; i < b->doc->blockCount; i++)
        {
            DocBlock *blk = b->doc->blocks[i];
            if (blk && blk->radio && blk->name &&
                strcmp(blk->name, name) == 0 && blk->checked)
            {
                return 1;
            }
        }
    }
    if (n && n->parent)
    {
        DomNode *root = n;
        while (root->parent)
        {
            root = root->parent;
        }
        return jsbv_group_attr_checked(root, name, 0);
    }
    return 0;
}

/* The checkbox/radio live checked state (block road, attr fallback). */
static int jsbv_checked_of(JsBridge *b, DomNode *n)
{
    if (b && b->doc)
    {
        for (int i = 0; i < b->doc->blockCount; i++)
        {
            DocBlock *blk = b->doc->blocks[i];
            if (blk && blk->srcNode == (void *)n && !blk->radio)
            {
                return blk->checked;
            }
        }
    }
    return dom_get_attr(n, "checked") != NULL;
}

static int jsbv_isspace_str(const char *s)
{
    while (*s)
    {
        if (!isspace((unsigned char)*s))
        {
            return 0;
        }
        s++;
    }
    return 1;
}

/* The one rule evaluator. Fills flags + the browser's first-failure
 * message. Barred controls come back trivially valid. */
void jsbridge_form_validity_eval(JsBridge *b, DomNode *n, JsbValidity *out)
{
    memset(out, 0, sizeof(*out));
    if (!jsbridge_form_will_validate(b, n))
    {
        return;
    }
    const char *tag = n->tag ? n->tag : "";
    const char *type = dom_get_attr(n, "type");
    if (!type || !type[0])
    {
        type = "text";
    }
    unsigned flags = 0;
    char value[384];
    jsbv_value_of(b, n, value, sizeof(value));
    size_t vlen = strlen(value);
    if (vlen >= sizeof(value))
    {
        vlen = sizeof(value) - 1;
    }

    /* — badInput (number type only on this panel); trailing whitespace
     * parses clean, browser-style — */
    double num = 0;
    int numOk = 0;
    if (strcmp(tag, "input") == 0 && strcmp(type, "number") == 0 &&
        vlen > 0)
    {
        char *end = NULL;
        num = strtod(value, &end);
        if (end && (!end[0] || jsbv_isspace_str(end)))
        {
            numOk = 1;
        }
        else
        {
            flags |= JSBV_BADINPUT;
        }
    }
    /* — valueMissing (browser: the control must ALSO carry required —
     * an empty optional control is valid, never a mismatch) — */
    if (!flags)
    {
        const char *rq = dom_get_attr(n, "required");
        if (strcmp(tag, "input") == 0)
        {
            if (strcmp(type, "checkbox") == 0)
            {
                if (rq && !jsbv_checked_of(b, n))
                {
                    flags |= JSBV_VALUEMISSING;
                }
            }
            else if (strcmp(type, "radio") == 0)
            {
                const char *nm = dom_get_attr(n, "name");
                if (rq && nm && !jsbv_group_checked(b, n, nm))
                {
                    flags |= JSBV_VALUEMISSING;
                }
            }
            else if (vlen == 0 && rq)
            {
                flags |= JSBV_VALUEMISSING;
            }
        }
        else if (vlen == 0 && rq)
        {
            flags |= JSBV_VALUEMISSING;
        }
    }
    /* — typeMismatch (email/url shapes, non-empty values) — */
    if (!flags && strcmp(tag, "input") == 0 && vlen > 0)
    {
        if (strcmp(type, "email") == 0)
        {
            const char *at = strchr(value, '@');
            int ok = at && at != value && at[1] && !strchr(value, ' ') &&
                     strchr(at + 1, '.');
            if (!ok)
            {
                flags |= JSBV_TYPEMISMATCH;
            }
        }
        else if (strcmp(type, "url") == 0)
        {
            int ok = (value[0] >= 'a' && value[0] <= 'z') ||
                     (value[0] >= 'A' && value[0] <= 'Z');
            size_t i = 1;
            while (ok && value[i] && value[i] != ':' &&
                   ((value[i] >= 'a' && value[i] <= 'z') ||
                    (value[i] >= 'A' && value[i] <= 'Z') ||
                    (value[i] >= '0' && value[i] <= '9') ||
                    value[i] == '+' || value[i] == '-' || value[i] == '.'))
            {
                i++;
            }
            ok = ok && value[i] == ':' && i >= 1 && i <= 32;
            if (!ok)
            {
                flags |= JSBV_TYPEMISMATCH;
            }
        }
    }
    /* — patternMismatch (all input types, non-empty value) — */
    if (!flags && strcmp(tag, "input") == 0 && vlen > 0)
    {
        const char *pat = dom_get_attr(n, "pattern");
        if (pat && !css_pattern_matches(pat, value))
        {
            flags |= JSBV_PATTERNMISMATCH;
        }
    }
    /* — tooLong / tooShort (value attribute + writes = the dirty store;
     * a browser bars attribute-set defaults, our writes ARE the attr —
     * browser-identical for the assigned case, documented) — */
    int hasML = 0, hasNL = 0;
    int maxl = jsbv_attr_int(n, "maxlength", &hasML);
    int minl = jsbv_attr_int(n, "minlength", &hasNL);
    int lenable = strcmp(tag, "input") == 0 || strcmp(tag, "textarea") == 0;
    if (!flags && lenable && hasML && maxl >= 0 && (int)vlen > maxl)
    {
        flags |= JSBV_TOOLONG;
    }
    if (!flags && lenable && hasNL && minl >= 0 && vlen > 0 &&
        (int)vlen < minl)
    {
        flags |= JSBV_TOOSHORT;
    }
    /* — rangeUnderflow / rangeOverflow (number type on this panel).
     * min/max parse as doubles (browser-true); unparseable bounds are
     * ignored, exactly a browser's silent invalid-attribute rule. */
    int hasMin = 0, hasMax = 0;
    double mn = 0, mx = 0;
    const char *mv = dom_get_attr(n, "min");
    if (mv && mv[0])
    {
        char *e = NULL;
        double d = strtod(mv, &e);
        if (e && !e[0])
        {
            hasMin = 1;
            mn = d;
        }
    }
    mv = dom_get_attr(n, "max");
    if (mv && mv[0])
    {
        char *e = NULL;
        double d = strtod(mv, &e);
        if (e && !e[0])
        {
            hasMax = 1;
            mx = d;
        }
    }
    if (!flags && numOk && strcmp(tag, "input") == 0 &&
        strcmp(type, "number") == 0)
    {
        if (hasMin && num < mn)
        {
            flags |= JSBV_RANGEUNDERFLOW;
        }
        else if (hasMax && num > mx)
        {
            flags |= JSBV_RANGEOVERFLOW;
        }
    }
    /* stepMismatch / customError never fire (panel boundaries, standing
     * policy rows). */

    out->flags = flags;
    if (flags & JSBV_VALUEMISSING)
    {
        snprintf(out->message, sizeof(out->message), "Fill in this field.");
    }
    else if (flags & JSBV_BADINPUT)
    {
        snprintf(out->message, sizeof(out->message), "Enter a number.");
    }
    else if (flags & JSBV_TYPEMISMATCH)
    {
        snprintf(out->message, sizeof(out->message),
                 strcmp(type, "url") == 0 ? "Enter a URL."
                                          : "Enter an email address.");
    }
    else if (flags & JSBV_PATTERNMISMATCH)
    {
        snprintf(out->message, sizeof(out->message), "Match the requested format.");
    }
    else if (flags & JSBV_TOOLONG)
    {
        snprintf(out->message, sizeof(out->message),
                 "Shorten this text to %d characters or fewer.", maxl);
    }
    else if (flags & JSBV_TOOSHORT)
    {
        snprintf(out->message, sizeof(out->message), "Use %d characters or more.", minl);
    }
    else if (flags & JSBV_RANGEUNDERFLOW)
    {
        snprintf(out->message, sizeof(out->message),
                 "Value must be greater than or equal to %g.", mn);
    }
    else if (flags & JSBV_RANGEOVERFLOW)
    {
        snprintf(out->message, sizeof(out->message),
                 "Value must be less than or equal to %g.", mx);
    }
}

/* Form-level gate: count the invalid candidates among the submittable
 * controls below formNode; the first `cap` offenders land in the
 * caller's array. Returns the count (0 = the form validates). */
int jsbridge_form_check(JsBridge *b, DomNode *formNode, DomNode **offenders,
                        int cap, JsbValidity *validityOut)
{
    if (!b || !formNode || !offenders || cap <= 0)
    {
        return 0;
    }
    int cnt = 0;
    int fill = 0;
    static DomNode *queue[64]; /* bss scratch — single-threaded caller */
    int qn = 0;
    for (int i = 0; i < formNode->childCount && qn < 64; i++)
    {
        queue[qn++] = formNode->children[i];
    }
    int qh = 0;
    while (qh < qn)
    {
        DomNode *c = queue[qh++];
        if (!c || c->kind != DOM_ELEMENT)
        {
            continue;
        }
        if (c->tag && !strcmp(c->tag, "form"))
        {
            for (int i = 0; i < c->childCount && qn < 64; i++)
            {
                queue[qn++] = c->children[i];
            }
            continue; /* nested forms are not parts of THIS form */
        }
        JsbValidity v;
        jsbridge_form_validity_eval(b, c, &v);
        if (v.flags)
        {
            if (fill < cap)
            {
                offenders[fill++] = c;
            }
            if (validityOut && cnt == 0)
            {
                *validityOut = v; /* the FIRST offender's message */
            }
            cnt++;
        }
        for (int i = 0; i < c->childCount && qn < 64; i++)
        {
            queue[qn++] = c->children[i];
        }
    }
    return cnt;
}

/* ── Forms-Phase 4: the HTMLFormElement API host hooks ──────────────────── *
 * The router holds the two function pointers registered by main.c at boot;
 * the engine bridges call the public roads below from their form-element
 * methods. The BRIDGE rides the call (a mid-parse script has no host
 * g_pageJs yet — the page's own machine is the delivery context). No hook
 * = a bridge running under test scaffolding without the host — the roads
 * report "nothing happened" (0) and never throw. Nodes arrive as live
 * DomNode pointers matching jsbridge.h's element handles. */
static JsbFormHostReset g_hostFormReset = NULL;
static JsbFormHostWire g_hostFormWire = NULL;

void jsbridge_set_form_hosts(JsbFormHostReset resetFn, JsbFormHostWire wireFn)
{
    g_hostFormReset = resetFn;
    g_hostFormWire = wireFn;
    logger_log("[form] host hooks registered (reset=%p wire=%p)",
               (void *)resetFn, (void *)wireFn);
}

int jsbridge_form_host_reset(JsBridge *b, void *formNode)
{
    return g_hostFormReset ? g_hostFormReset(b, formNode) : 0;
}

int jsbridge_form_host_wire(JsBridge *b, void *formNode, void *submitterNode,
                            int wantGate, int wantSubmitEvent)
{
    return g_hostFormWire ? g_hostFormWire(b, formNode, submitterNode,
                                           wantGate, wantSubmitEvent)
                          : 0;
}

/* Forms-Phase 4 `elements` backing: every descendant input/textarea/
 * select/button of the form node in tree order (browser association:
 * descendants ride the enclosing form; the form= attribute association
 * is out of scope this phase). Nested forms break containment exactly
 * like the validity gate's walk. Returns the collected count. */
int jsbridge_form_controls_collect(DomNode *formNode, void **out, int cap)
{
    if (!formNode || !out || cap <= 0)
    {
        return 0;
    }
    int cnt = 0;
    static DomNode *queue[128]; /* bss scratch — single-threaded caller */
    int qn = 0;
    for (int i = 0; i < formNode->childCount && qn < 128; i++)
    {
        queue[qn++] = formNode->children[i];
    }
    int qh = 0;
    while (qh < qn)
    {
        DomNode *c = queue[qh++];
        if (!c || c->kind != DOM_ELEMENT)
        {
            continue;
        }
        int hit = 0;
        if (c->tag)
        {
            if (!strcmp(c->tag, "input") || !strcmp(c->tag, "textarea") ||
                !strcmp(c->tag, "select") || !strcmp(c->tag, "button"))
            {
                hit = 1;
            }
            else if (!strcmp(c->tag, "form"))
            {
                /* nested forms are not parts of THIS form's elements */
                continue;
            }
        }
        if (hit && cnt < cap)
        {
            out[cnt++] = c;
        }
        for (int i = 0; i < c->childCount && qn < 128; i++)
        {
            queue[qn++] = c->children[i];
        }
    }
    return cnt;
}

/* ── Forms-Phase 5: FormData → the ONE form-data rulebook ────────────────
 * `new FormData(form[, submitter])` on every Forms engine AND the
 * route-standard POST body serialize through THIS collector, so the
 * JS-side form-data bytes and the actual POST wire bytes cannot drift:
 * one rulebook, two roads (the phase bar).
 *
 * The rules are the browser's form data set, written browser-true:
 *  - tree-order DFS (depth-first pre-order — document order, NOT the
 *    BFS order of the Phase-4 `elements` walker);
 *  - nested <form> elements excluded (a form is never part of itself);
 *  - controls disabled by a `disabled` FIELDSET ancestor are excluded;
 *  - a control participates only with a NON-EMPTY `name` attribute —
 *    strict names, NOT the fb-walk's id fallback (that fallback belongs
 *    to the profile arc, not to the browser);
 *  - `disabled` / aria-disabled="true" excluded (the agreement recorded
 *    for this engine family);
 *  - radios/checkboxes only when checked, value attr defaulting to
 *    "on" (the browser's own default);
 *  - type=file skipped (no file picker — the standing POLICY row);
 *    type=image skipped (click coords only exist on real activation);
 *  - button/submit/reset contribute a pair ONLY as the submitter (the
 *    activation click / the ctor's second argument), value attr else
 *    "Submit" for submit / "" for reset / button text for button —
 *    the browser's own defaults;
 *  - select → the LIVE selected option (block when taps moved it, else
 *    the markup selected option, else the first) via jsbv_value_of,
 *    one entry per select (multiple rows are the standing one-line
 *    POLICY); textarea → its live DOM text;
 *  - the submitter must be a form descendant (the browser's
 *    NotFoundError family — contained as an errCode). */

/* A control whose pair rides the form data set (fieldset handled as a
 * disabler only — the fieldset's own name entry is the standing note). */
static int fd_is_control(const char *tag)
{
    return tag && (!strcmp(tag, "input") || !strcmp(tag, "textarea") ||
                   !strcmp(tag, "select") || !strcmp(tag, "button"));
}

static int fd_aria_disabled(DomNode *n)
{
    const char *a = dom_get_attr(n, "aria-disabled");
    return a && !strcmp(a, "true");
}

/* The value read for the plain lanes — jsbv_value_of is the phase-3
 * LIVE probe (select block first, textarea text, input attr). */
static void fd_walk(JsBridge *b, DomNode *n, DomNode *submitter,
                    int fsDisabled, int depth, FdPair *out, int cap,
                    int *cnt)
{
    if (!n || n->kind != DOM_ELEMENT || !n->tag || depth > 24)
    {
        return;
    }
    const char *tag = n->tag;
    if (depth > 0 && !strcmp(tag, "form"))
    {
        return; /* nested form: not this form's data (the root passes) */
    }
    if (!strcmp(tag, "fieldset"))
    {
        if (dom_get_attr(n, "disabled"))
        {
            fsDisabled = 1; /* the browser's whole-fieldset disablement */
        }
    }
    else if (fd_is_control(tag) && !fsDisabled &&
             !dom_get_attr(n, "disabled") && !fd_aria_disabled(n))
    {
        const char *nm = dom_get_attr(n, "name");
        if (nm && nm[0])
        {
            char type[16] = "";
            if (!strcmp(tag, "input"))
            {
                const char *ty = dom_get_attr(n, "type");
                snprintf(type, sizeof(type), "%s",
                         (ty && ty[0]) ? ty : "text");
            }
            else if (!strcmp(tag, "textarea"))
            {
                snprintf(type, sizeof(type), "textarea");
            }
            else if (!strcmp(tag, "select"))
            {
                snprintf(type, sizeof(type), "select");
            }
            else
            {
                snprintf(type, sizeof(type), "button");
            }
            /* The lanes: toggles/gates first, then the value probes. */
            if (!strcmp(type, "file") || !strcmp(type, "image"))
            {
                /* file: no picker (standing POLICY); image: coords exist
                 * only on a real activation we cannot observe. */
            }
            else if (!strcmp(type, "submit") || !strcmp(type, "reset") ||
                     !strcmp(type, "button"))
            {
                if (n == submitter && *cnt < cap)
                {
                    FdPair *p = &out[(*cnt)++];
                    snprintf(p->name, sizeof(p->name), "%s", nm);
                    const char *v = dom_get_attr(n, "value");
                    if (v)
                    {
                        snprintf(p->value, sizeof(p->value), "%s", v);
                    }
                    else if (!strcmp(tag, "button"))
                    {
                        /* <button> defaults to its text content. */
                        char tbuf[JSBRIDGE_FD_VALUE];
                        doc_concat_node_text(n, tbuf, sizeof(tbuf));
                        snprintf(p->value, sizeof(p->value), "%s", tbuf);
                    }
                    else
                    {
                        snprintf(p->value, sizeof(p->value), "%s",
                                 !strcmp(type, "submit") ? "Submit" : "");
                    }
                }
            }
            else if (!strcmp(type, "checkbox") || !strcmp(type, "radio"))
            {
                if (dom_get_attr(n, "checked") && *cnt < cap)
                {
                    FdPair *p = &out[(*cnt)++];
                    snprintf(p->name, sizeof(p->name), "%s", nm);
                    const char *v = dom_get_attr(n, "value");
                    snprintf(p->value, sizeof(p->value), "%s",
                             (v && v[0]) ? v : "on");
                }
            }
            else if (*cnt < cap)
            {
                FdPair *p = &out[(*cnt)++];
                snprintf(p->name, sizeof(p->name), "%s", nm);
                jsbv_value_of(b, n, p->value, sizeof(p->value));
            }
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        fd_walk(b, n->children[i], submitter, fsDisabled, depth + 1, out,
                cap, cnt);
    }
}

int jsbridge_form_data_collect(JsBridge *b, void *formNode,
                               void *submitterNode, FdPair *out, int cap)
{
    DomNode *form = (DomNode *)formNode;
    if (!form || form->kind != DOM_ELEMENT || !form->tag ||
        strcmp(form->tag, "form") != 0)
    {
        return -1; /* not-a-form (the browser's TypeError family) */
    }
    if (submitterNode)
    {
        DomNode *s = (DomNode *)submitterNode;
        int found = 0;
        for (DomNode *p = s->parent; p; p = p->parent)
        {
            if (p == form)
            {
                found = 1;
                break;
            }
        }
        if (!found)
        {
            return -2; /* submitter outside the form */
        }
    }
    if (!out || cap <= 0)
    {
        return 0;
    }
    int cnt = 0;
    fd_walk(b, form, (DomNode *)submitterNode, 0, 0, out, cap, &cnt);
    return cnt;
}

/* ── Fixed-store ops (shared by all three engine layers) ──────────────── */

FormDataEntry *jsbridge_form_data_construct(JsBridge *b, void *formNode,
                                            void *submitterNode,
                                            int *errCode)
{
    int err = 0;
    FormDataEntry *e = NULL;
    if (b)
    {
        if (!formNode)
        {
            /* Browser truth: the form argument became optional — a bare
             * `new FormData()` claims an EMPTY instance (no pairs). */
            for (int i = 0; i < JSBRIDGE_FD_MAX && !e; i++)
            {
                if (!b->fd[i].used)
                {
                    e = &b->fd[i];
                }
            }
            if (!e)
            {
                err = 3;
            }
            else
            {
                e->used = 1;
                e->npairs = 0;
                memset(e->pairs, 0, sizeof(e->pairs));
            }
        }
        else
        {
            /* Probe validity first so a full store never masks a
             * TypeError. */
            int cnt = jsbridge_form_data_collect(b, formNode,
                                                 submitterNode, NULL, 0);
            if (cnt == -1)
            {
                err = 1;
            }
            else if (cnt == -2)
            {
                err = 2;
            }
            else
            {
                for (int i = 0; i < JSBRIDGE_FD_MAX && !e; i++)
                {
                    if (!b->fd[i].used)
                    {
                        e = &b->fd[i];
                    }
                }
                if (!e)
                {
                    err = 3;
                }
                else
                {
                    e->used = 1;
                    e->npairs = 0;
                    memset(e->pairs, 0, sizeof(e->pairs));
                    e->npairs = jsbridge_form_data_collect(
                        b, formNode, submitterNode, e->pairs,
                        JSBRIDGE_FD_PAIRS);
                }
            }
        }
    }
    if (errCode)
    {
        *errCode = err;
    }
    return e;
}

void jsbridge_form_data_append(FormDataEntry *e, const char *name,
                               const char *value)
{
    if (!e || e->npairs >= JSBRIDGE_FD_PAIRS)
    {
        return; /* pair cap: dropped honestly (standing policy shape) */
    }
    FdPair *p = &e->pairs[e->npairs++];
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
    snprintf(p->value, sizeof(p->value), "%s", value ? value : "");
}

const char *jsbridge_form_data_get(const FormDataEntry *e, const char *name)
{
    if (e && name)
    {
        for (int i = 0; i < e->npairs; i++)
        {
            if (strcmp(e->pairs[i].name, name) == 0)
            {
                return e->pairs[i].value;
            }
        }
    }
    return NULL;
}

int jsbridge_form_data_has(const FormDataEntry *e, const char *name)
{
    return jsbridge_form_data_get(e, name) != NULL;
}

int jsbridge_form_data_set(FormDataEntry *e, const char *name,
                           const char *value)
{
    if (!e)
    {
        return 0;
    }
    int hit = -1;
    for (int i = 0; i < e->npairs; i++)
    {
        if (strcmp(e->pairs[i].name, name) == 0)
        {
            if (hit < 0)
            {
                hit = i;
            }
            else
            {
                /* drop the later duplicates */
                memmove(&e->pairs[i], &e->pairs[i + 1],
                        sizeof(FdPair) * (size_t)(e->npairs - i - 1));
                e->npairs--;
                i--;
            }
        }
    }
    if (hit >= 0)
    {
        snprintf(e->pairs[hit].value, sizeof(e->pairs[hit].value), "%s",
                 value ? value : "");
        return 1;
    }
    int before = e->npairs;
    jsbridge_form_data_append(e, name, value); /* browser set = append */
    return e->npairs > before;
}

int jsbridge_form_data_delete(FormDataEntry *e, const char *name)
{
    if (!e || !name)
    {
        return 0;
    }
    int removed = 0;
    for (int i = 0; i < e->npairs; i++)
    {
        if (strcmp(e->pairs[i].name, name) == 0)
        {
            memmove(&e->pairs[i], &e->pairs[i + 1],
                    sizeof(FdPair) * (size_t)(e->npairs - i - 1));
            e->npairs--;
            i--;
            removed = 1;
        }
    }
    return removed;
}

int jsbridge_form_data_count(const FormDataEntry *e)
{
    return e ? e->npairs : 0;
}

const FdPair *jsbridge_form_data_at(const FormDataEntry *e, int i)
{
    if (e && i >= 0 && i < e->npairs)
    {
        return &e->pairs[i];
    }
    return NULL;
}

FormDataIter *jsbridge_form_data_iter(JsBridge *b, FormDataEntry *e,
                                      int *full)
{
    if (full)
    {
        *full = 0;
    }
    if (!e)
    {
        return NULL;
    }
    if (!b)
    {
        return NULL;
    }
    for (int i = 0; i < JSBRIDGE_FD_ITER_MAX; i++)
    {
        if (!b->fdi[i].used)
        {
            b->fdi[i].used = 1;
            b->fdi[i].entry = e;
            b->fdi[i].idx = 0;
            return &b->fdi[i];
        }
    }
    if (full)
    {
        *full = 1;
    }
    return NULL;
}

FormDataEntry *jsbridge_form_data_iter_entry(const FormDataIter *it)
{
    return it ? it->entry : NULL;
}

/* ── The one x-www-form-urlencoded escaper ──────────────────────────────
 * Byte rules UNCHANGED from the standing main.c serializer (letters/
 * digits/-_.~ verbatim, space → '+', everything else %XX-hex). The
 * standing JS-side page serializer agrees on the A-Za-z0-9-_ .~ set,
 * so the battery's in-page recount cannot drift from this core.
 * Appends into out at *used, always NUL-terminates, truncates at
 * cap-4 — exactly the standing fb_urlenc_append contract. */
void jsbridge_urlenc_cpy(const char *raw, char *out, size_t cap,
                         size_t *used)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t u = *used;
    for (; *raw && u + 4 < cap; raw++)
    {
        char c = *raw;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~')
        {
            out[u++] = c;
        }
        else if (c == ' ')
        {
            out[u++] = '+';
        }
        else
        {
            out[u++] = '%';
            out[u++] = hex[((unsigned char)c >> 4) & 0xF];
            out[u++] = hex[(unsigned char)c & 0xF];
        }
    }
    out[u] = '\0';
    *used = u;
}

/* The wire road: collect the form data set ONCE and serialize it with
 * the escaper above — byte-for-byte what the route-standard POST body
 * carries (see the pluto_form_fallback POST branch). Static scratch:
 * single-threaded caller, same bss-scratch precedent as the controls
 * walker. */
void jsbridge_form_data_urlenc(JsBridge *b, void *formNode,
                               void *submitterNode, char *out, size_t cap)
{
    static FdPair buf[JSBRIDGE_FD_PAIRS];
    out[0] = '\0';
    if (!out || cap == 0)
    {
        return;
    }
    int cnt = jsbridge_form_data_collect(b, formNode, submitterNode, buf,
                                         JSBRIDGE_FD_PAIRS);
    if (cnt < 0)
    {
        cnt = 0;
    }
    size_t used = 0;
    for (int i = 0; i < cnt; i++)
    {
        if (used && used + 1 < cap)
        {
            out[used++] = '&';
        }
        jsbridge_urlenc_cpy(buf[i].name, out, cap, &used);
        if (used + 1 < cap)
        {
            out[used++] = '=';
        }
        jsbridge_urlenc_cpy(buf[i].value, out, cap, &used);
    }
    out[used] = '\0';
}

/* ── Forms-Phase 6: the enumerated IDL reflection + the text/plain road ── */

/* ASCII case-insensitive keyword match (the getter road runs every
 * reflected read; keep it tiny). */
static int fdw_eq_ci(const char *v, const char *kw)
{
    if (!v)
        return 0;
    while (*v && *kw)
    {
        char lv = (*v >= 'A' && *v <= 'Z') ? (char)(*v + 32) : *v;
        char lk = (*kw >= 'A' && *kw <= 'Z') ? (char)(*kw + 32) : *kw;
        if (lv != lk)
            return 0;
        v++;
        kw++;
    }
    return *v == *kw;
}

/* HTMLFormElement.method IDL — the reflected enumerated attribute,
 * "limited to only known values" (spec): the getter returns the
 * canonical keyword when the attribute names one, else the enumerated
 * GET default — the missing-value default AND the invalid-value default
 * share the GET keyword. The IDL setter is the browser's own shape: the
 * engine layers write the lowercased string verbatim; the DEFAULT is
 * applied here on every read (so an invalid set reads "get"). Returns
 * one of "get"/"post"/"dialog" (static storage, no scratch). */
const char *jsbridge_form_method_idl(const char *raw)
{
    if (raw && raw[0])
    {
        if (fdw_eq_ci(raw, "get"))
            return "get";
        if (fdw_eq_ci(raw, "post"))
            return "post";
        if (fdw_eq_ci(raw, "dialog"))
            return "dialog";
    }
    return "get";
}

/* HTMLFormElement.enctype (and the .encoding alias) IDL — the same
 * limited reflection for the three submission encodings. Missing or
 * invalid maps to application/x-www-form-urlencoded (both defaults).
 * SHAREd with the wire road: pluto_form_fallback resolves the
 * submitter's effective enctype through this helper, so the reflected
 * read and the actual serialization cannot disagree. */
const char *jsbridge_form_enctype_idl(const char *raw)
{
    if (raw && raw[0])
    {
        if (fdw_eq_ci(raw, "application/x-www-form-urlencoded"))
            return "application/x-www-form-urlencoded";
        if (fdw_eq_ci(raw, "multipart/form-data"))
            return "multipart/form-data";
        if (fdw_eq_ci(raw, "text/plain"))
            return "text/plain";
    }
    return "application/x-www-form-urlencoded";
}

/* One text/plain segment copy (a name or a value) with the browser's
 * own newline normalization: CRLF stays CRLF; lone CR and lone LF each
 * become CRLF (spec: entry-list construction). Appends at *used, always
 * NUL-terminates, truncates at cap-4 like the standing serializers. */
static void fdw_plain_cpy(const char *s, char *out, size_t cap, size_t *used)
{
    for (; *s && *used + 4 < cap; s++)
    {
        char c = *s;
        if (c == '\r')
        {
            out[(*used)++] = '\r';
            out[(*used)++] = '\n';
            if (s[1] == '\n')
            {
                s++; /* the paired LF rides the emitted CRLF */
            }
        }
        else if (c == '\n')
        {
            out[(*used)++] = '\r';
            out[(*used)++] = '\n';
        }
        else
        {
            out[(*used)++] = c;
        }
    }
}

/* The text/plain wire: the entry list serialized RAW (name=value with
 * NO escaping — the browser's own human-readable encoding), each pair
 * terminated with CRLF (spec step: append name, '=', value, CRLF for
 * every pair — the final pair keeps its CRLF). Same static scratch as
 * the urlencoded sibling — single-threaded caller. */
void jsbridge_form_data_plain(JsBridge *b, void *formNode,
                              void *submitterNode, char *out, size_t cap)
{
    static FdPair buf[JSBRIDGE_FD_PAIRS];
    out[0] = '\0';
    if (!out || cap == 0)
    {
        return;
    }
    int cnt = jsbridge_form_data_collect(b, formNode, submitterNode, buf,
                                         JSBRIDGE_FD_PAIRS);
    if (cnt < 0)
    {
        cnt = 0;
    }
    size_t used = 0;
    for (int i = 0; i < cnt; i++)
    {
        fdw_plain_cpy(buf[i].name, out, cap, &used);
        if (used + 1 < cap)
        {
            out[used++] = '=';
        }
        fdw_plain_cpy(buf[i].value, out, cap, &used);
        if (used + 2 < cap)
        {
            out[used++] = '\r';
            out[used++] = '\n';
        }
    }
    out[used] = '\0';
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
