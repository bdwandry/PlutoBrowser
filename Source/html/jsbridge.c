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
#include "../core/logger.h"
#include "../util/strbuf.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_pd()->system->realloc(NULL, (n))
#define JFree(p) pluto_pd()->system->realloc((p), 0)

/* Selected engine for ALL subsequently attached pages (0=muJS 1=Duktape).
 * Set from Settings via jsbridge_set_engine(); NOT from core/storage so the
 * html layer stays decoupled and host tests can drive both engines. */
static int g_selectedEngine = 0;

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
    g_selectedEngine = (engine == JS_ENGINE_DUKTAPE ||
                        engine == JS_ENGINE_QUICKJS)
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

int jsbridge_script_compile_safe(const char *src, size_t len)
{
    return pluto_script_compile_safe(src, len);
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
    /* Engine selection comes from Settings ("Javascript Engine"): the
     * choice routes ALL of this page's scripts to exactly one engine (no
     * cross-execution, no fallback). */
    b->engine = (g_selectedEngine == JS_ENGINE_DUKTAPE ||
                 g_selectedEngine == JS_ENGINE_QUICKJS)
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

void js_doc_close(JsBridge *b)
{
    if (!b)
    {
        return;
    }
    const JsEngineImpl *impl = js_bridge_impl(b);
    logger_log("[js] close[%s]: ran=%d errs=%d listeners=%d budgetLeft=%d%s%s",
               impl ? impl->name : "?", b->ran, b->errs, b->listenerCount,
               b->callBudget,
               b->lastError[0] ? " last=" : "",
               b->lastError[0] ? b->lastError : "");
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
