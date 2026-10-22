/*
 * nr_chunk_host_test.c — offline NR-engine harness (macOS host only, NOT
 * part of the Playdate build). Two modes:
 *
 *  replay mode (argv[1] *.html): reproduce the SIM pipeline exactly —
 *    jsext_collect → jsext_prefetch (each http_get served synchronously from
 *    an offline cache: /tmp/jsmanifest.txt "url<TAB>/path" lines) → spill →
 *    document_parse_ex(DOC_SCRIPT_FULL) → js_doc_attach runs inline bodies
 *    AND disk-streamed externals in document order, exactly like main.c.
 *    argv[2] = page base URL (resolution + relative src= against it).
 *
 *  single mode (argv[1] *.js): run one script file via jsbridge_page_script
 *    against a shell page containing #root (old bisect behavior).
 *
 * Env flags: NR_PREFIX=<bytes> (single mode: run only a prefix);
 * HOST_JSMANIFEST=<file> (replay mode cache manifest, default /tmp/jsmanifest.txt).
 *
 * Exit codes: 0 = nothing failed; 1 = JS error/skip/abort; 2 = usage/IO.
 * The [js] logger stream mirrors the device log; "skipped:"/"aborted" lines
 * are detected in the file shim so the verdict reflects the true outcome.
 *
 * Build: /tmp/build_nrchunk.sh (SRCS head is this file). Run from repo root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"

static int g_sawSkip = 0;  /* any '[js] ... skipped:' logger line this run */
static int g_sawAbort = 0; /* any '[js] ... aborted' logger line this run */
static int g_sawFail = 0;  /* any '[js] ... failed:' logger line this run */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
/* File shim so logger_log() output (the [js]/[xs] traces) reaches stdout —
 * same lines the device logs. */
static SDFile *fake_file_open(const char *name, FileOptions mode)
{
    (void)name;
    (void)mode;
    return (SDFile *)1;
}
static int fake_file_close(SDFile *f) { (void)f; return 0; }
static int fake_file_read(SDFile *f, void *buf, unsigned int len)
{
    (void)f;
    (void)buf;
    (void)len;
    return 0; /* EOF: no previous-run log tail on host */
}
static int fake_file_write(SDFile *f, const void *buf, unsigned int len)
{
    (void)f;
    /* Bisect signal: the engine logs skips/aborts via logger. Skips do NOT
     * count as DocParseResult errors, and engine aborts reset the bridge,
     * so track both for an honest exit code. */
    static char probe[512];
    unsigned int copy = len < 511 ? len : 511;
    if (copy > 0)
    {
        memcpy(probe, buf, copy);
        probe[copy] = '\0';
    }
    if (strstr(probe, "skipped:") != NULL)
    {
        g_sawSkip = 1;
    }
    if (strstr(probe, "aborted") != NULL)
    {
        g_sawAbort = 1;
    }
    return (int)fwrite(buf, 1, len, stdout);
}
static unsigned fake_seconds(unsigned *ms)
{
    if (ms)
    {
        *ms = 0;
    }
    return 0;
}
static int32_t fake_tz(void) { return 0; }
static void fake_epoch_to_dt(uint32_t epoch, struct PDDateTime *dt)
{
    (void)epoch;
    memset(dt, 0, sizeof(*dt));
}
static struct playdate_sys g_fakeSys;
static struct playdate_file g_fakeFile;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;
PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeFile, 0, sizeof(g_fakeFile));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeSys.realloc = host_realloc;
        g_fakeSys.getSecondsSinceEpoch = fake_seconds;
        g_fakeSys.getTimezoneOffset = fake_tz;
        g_fakeSys.convertEpochToDateTime = fake_epoch_to_dt;
        g_fakeApi.system = &g_fakeSys;
        g_fakeFile.open = fake_file_open;
        g_fakeFile.close = fake_file_close;
        g_fakeFile.read = fake_file_read;
        g_fakeFile.write = fake_file_write;
        g_fakeApi.file = &g_fakeFile;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}
void pluto_free(void *p) { free(p); }
void *pluto_mem_realloc(void *p, size_t n); /* core/pluto_mem.h (path differs on device) */
void *pluto_realloc(void *p, size_t n) { return pluto_mem_realloc(p, n); }
/* raw SDK backend for the telemetry funnel (mirrors main.c on device) */
void *pluto_mem_sdk_realloc(void *p, size_t n) { return realloc(p, n); }
void tasks_report_progress(float f) { (void)f; } /* readability stub */
void pluto_ui_net_progress(int cur, int total) { (void)cur; (void)total; }

/* http_client fake: offline replay cache. jsext_prefetch_step calls
 * http_get one sequential request at a time; each URL present in the
 * manifest resolves to a local file and settles synchronously through
 * onSuccess with the real downloaded bytes — jsext then does its own
 * budget/spill work, identical to the simulator's flow (PLUTO_SPILL_HOST
 * writes the spill files under /tmp/plutobrowser_spill). */
#include "core/http_client.h" /* real HttpCallbacks layout (fake below) */
#define HOST_MANIFEST_MAX 64
#define HOST_URL_MAX 256
static char g_manifestUrl[HOST_MANIFEST_MAX][HOST_URL_MAX];
static char g_manifestPath[HOST_MANIFEST_MAX][HOST_URL_MAX];
static int g_manifestCount = 0;
static int g_manifestReady = 0;
static int g_fakeHttpCancel = 0;
/* Async delivery state (mirrors the real http_client): http_get starts a
 * request; the harness pump later fires onSuccess like http_update does
 * (jsext only treats a request as its own while its session is active, so
 * a delivery inside http_get itself is dropped — the SIM semantics). */
static HttpCallbacks g_pendingCb;
static char g_pendingUrl[HOST_URL_MAX];
static char g_pendingPath[HOST_URL_MAX];
static int g_pending = 0;
static int g_cachedBytes = 0;
static void host_manifest_init(void)
{
    if (g_manifestReady)
    {
        return;
    }
    g_manifestReady = 1;
    const char *fn = getenv("HOST_JSMANIFEST");
    if (!fn)
    {
        fn = "/tmp/jsmanifest.txt";
    }
    FILE *f = fopen(fn, "r");
    if (!f)
    {
        printf("[host] no manifest %s (replay URLs will skip)\n", fn);
        return;
    }
    char line[1024];
    while (g_manifestCount < HOST_MANIFEST_MAX &&
           fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab)
        {
            continue;
        }
        *tab = '\0';
        snprintf(g_manifestUrl[g_manifestCount], HOST_URL_MAX, "%s", line);
        snprintf(g_manifestPath[g_manifestCount], HOST_URL_MAX, "%s", tab + 1);
        g_manifestCount++;
    }
    fclose(f);
    printf("[host] manifest %s: %d entr(y|ies)\n", fn, g_manifestCount);
}
int http_get(const char *url, const HttpCallbacks *cb)
{
    host_manifest_init();
    for (int i = 0; i < g_manifestCount; i++)
    {
        if (url && strcmp(g_manifestUrl[i], url) == 0)
        {
            /* MANUAL: hold in-flight like the real client; the pump fires
             * onSuccess after the caller's state is committed. */
            g_pendingCb = *cb;
            snprintf(g_pendingUrl, sizeof(g_pendingUrl), "%s", url);
            snprintf(g_pendingPath, sizeof(g_pendingPath), "%s", g_manifestPath[i]);
            g_pending = 1;
            return 1;
        }
    }
    return 0; /* not cached → jsext logs the skip, like a failed download */
}
/* Harness-side pump: deliver the pending cached body (http_update stand-in). */
static void host_http_pump(void)
{
    if (!g_pending)
    {
        return;
    }
    g_pending = 0;
    FILE *f = fopen(g_pendingPath, "rb");
    if (!f)
    {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *body = (char *)malloc((size_t)size + 1);
    if (!body)
    {
        fclose(f);
        return;
    }
    size_t got = fread(body, 1, (size_t)size, f);
    fclose(f);
    body[got] = '\0';
    g_cachedBytes += (int)got;
    printf("[host] cache deliver %s (%zu bytes)\n", g_pendingUrl, got);
    if (g_pendingCb.onSuccess)
    {
        g_pendingCb.onSuccess(200, NULL, NULL, 0, body, got, g_pendingUrl);
    }
    free(body);
}
/* C4: the router's POST entry. The host replay has no live wire: POST
 * URLs are simply not in the manifest → 0 (fetch shim sees the failure
 * path), so replay behavior stays offline-hermetic like every GET. */
int http_request(const char *method, const char *url, const char *body,
                 size_t bodyLen, const HttpCallbacks *cb)
{
    (void)method;
    (void)body;
    (void)bodyLen;
    return http_get(url, cb);
}
void http_cancel(void) { g_fakeHttpCancel++; }
void http_client_init(PlaydateAPI *pd) { (void)pd; }
void http_set_identity_encoding(int on) { (void)on; }
void http_update(void) {}
int http_is_loading(void) { return 0; }
/* Router's local about: path (XHR for about: URLs). */
size_t http_internal_page_body(const char *url, const char **bodyOut)
{
    static const char aboutBody[] =
        "<html><head><title>about</title></head><body>about-body</body></html>";
    if (bodyOut)
    {
        *bodyOut = NULL;
    }
    if (!url || strncmp(url, "about:", 6) != 0)
    {
        return 0;
    }
    if (bodyOut)
    {
        *bodyOut = aboutBody;
    }
    return sizeof(aboutBody) - 1;
}

#include "html/document.h"
#include "html/jsbridge.h"
#include "html/jsbridge_internal.h" /* JS_ENGINE_XS_NR (shared enum) */
#include "html/jsext.h"             /* external <script src> state machine */
#include "render/style.h"          /* PlutoFontRole (weak style_font stub below) */
#include "core/constants.h"
#include "core/logger.h"
#include "core/pluto_spill.h"

/* Host build: jsbridge_xs_nr.c's loading overlay weakly guards style_font
 * (see R26o/R30c comment at jsbridge_xs_nr.c:2888-2891). Provide the absent
 * real symbol as a stub returning NULL — the overlay then skips text
 * drawing, exactly like on host builds without the render pipeline. */
LCDFont *style_font(PlutoFontRole role)
{
    (void)role;
    return NULL;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

/* ── single mode: one .js file through jsbridge_page_script ─────────── */
static int run_single(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 2;
    }
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fprintf(stderr, "empty file %s\n", path);
        fclose(f);
        return 2;
    }
    char *js = (char *)malloc((size_t)size + 1);
    if (!js || fread(js, 1, (size_t)size, f) != (size_t)size)
    {
        fprintf(stderr, "read failed %s\n", path);
        free(js);
        fclose(f);
        return 2;
    }
    js[size] = '\0';
    fclose(f);
    size_t runLen = (size_t)size;
    const char *pref = getenv("NR_PREFIX");
    if (pref)
    {
        size_t n = (size_t)strtoul(pref, NULL, 10);
        if (n > 0 && n < runLen)
        {
            runLen = n;
        }
    }

    /* Minimal SPA shell — a #root div plus one trivial inline script to
     * force bridge attach under RUN_KEEP. */
    static const char SHELL[] =
        "<!doctype html><html><head><title>host</title></head><body>"
        "<div id=\"root\"></div><script>var __hostShell = 1;</script>"
        "</body></html>";
    DocParseResult *d = (DocParseResult *)calloc(1, sizeof(DocParseResult));
    int rc = document_parse_ex(SHELL, "https://bryanwandrych.com/contact",
                               MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP, NULL, d);
    if (rc != 0 || !d->_jsbridge)
    {
        fprintf(stderr, "bridge attach failed rc=%d jsErrors=%d lastErr=%s\n",
                rc, d->jsErrors,
                d->jsLastError[0] ? d->jsLastError : "(none)");
        document_free(d);
        free(d);
        free(js);
        return 2;
    }
    JsBridge *b = d->_jsbridge;
    printf("[host] bridge attached (engine %d) script=%s (%ld bytes, "
           "running %zu)\n",
           (int)JS_ENGINE_XS_NR, path, size, runLen);
    printf("[host] shell: jsRan=%d jsErrors=%d\n", d->jsRan, d->jsErrors);

    int baseline = d->jsErrors;
    int r = jsbridge_page_script(b, js, runLen);
    printf("[host] page_script ret=%d jsErrors=%d lastErr=%s\n", r,
           d->jsErrors, d->jsLastError[0] ? d->jsLastError : "(none)");

    /* Let timers/deferred/engine/XHR work settle so downstream failures
     * surface here too, mirroring main.c's per-frame pumps. */
    int last = d->jsErrors;
    for (int i = 0; i < 300; i++)
    {
        int mutations = 0;
        unsigned now = (unsigned)(i * 16);
        jsbridge_timers_pump(b, now, &mutations);
        jsbridge_deferred_pump(b, &mutations);
        jsbridge_engine_pump(b, now, &mutations);
        jsbridge_xhr_pump(b, &mutations);
        if (d->jsErrors > last)
        {
            printf("[host] pump %d: new jsErrors=%d lastErr=%s\n", i,
                   d->jsErrors, d->jsLastError[0] ? d->jsLastError : "(none)");
            last = d->jsErrors;
        }
    }
    printf("[host] done: total jsErrors=%d (%d from subject script) "
           "scriptRan=%d%s%s\n",
           d->jsErrors, d->jsErrors - baseline, r == 0,
           g_sawSkip ? " [skip logged]" : "",
           g_sawAbort ? " [ABORT logged]" : "");
    int exitCode = (r == 0 && d->jsErrors == baseline && !g_sawSkip &&
                    !g_sawAbort && !g_sawFail)
                       ? 0
                       : 1;

    js_doc_close(b);
    d->_jsbridge = NULL;
    document_free(d);
    free(d);
    free(js);
    return exitCode;
}

/* ── replay mode: full page through the SIM pipeline ────────────────── */

/* Host helper: report the live body innerHTML length through the page's
 * own console (the bridge surface) — the engine prints the BODYBYTES row. */
static void body_inner_bytes(DocParseResult *d, const char *when)
{
    if (!d || !d->_jsbridge)
    {
        printf("[host] bodyBytes %s: (no bridge)\n", when ? when : "-");
        return;
    }
    char probe[160];
    snprintf(probe, sizeof(probe),
             "var bh = (document.body && document.body.innerHTML)"
             " ? document.body.innerHTML.length : -1;"
             "console.log('HOSTBODY %s=' + bh);",
             when ? when : "-");
    jsbridge_page_script(d->_jsbridge, probe, strlen(probe));
}

/* Host helper: depth-first DOM-tree census (tags + child counts) from a
 * root — reveals where react attached its committed subtree. */
static void dump_dom_tree(DomNode *root, int maxDepth, int maxShown)
{
    typedef struct
    {
        DomNode *n;
        char path[96];
        int depth;
    } Entry;
    static Entry q[512];
    int top = 0, shown = 0;
    if (!root)
    {
        printf("[host] tree: (no root)\n");
        return;
    }
    q[top].n = root;
    q[top].depth = 0;
    snprintf(q[top].path, sizeof(q[top].path), "#root");
    top++;
    while (top > 0 && shown < maxShown)
    {
        Entry e = q[--top];
        printf("[host] tree %s (kids=%d\n", e.path, e.n->childCount);
        shown++;
        if (e.depth >= maxDepth)
        {
            continue;
        }
        for (int i = e.n->childCount - 1; i >= 0 && top < 512; i--)
        {
            DomNode *c = e.n->children[i];
            q[top].n = c;
            q[top].depth = e.depth + 1;
            snprintf(q[top].path, sizeof(q[top].path), "%s > %s",
                     strcmp(e.path, "#root") == 0 ? "" : e.path,
                     c->tag ? c->tag : "?");
            top++;
        }
    }
}

static int run_replay(const char *htmlPath, const char *pageUrl)
{
    FILE *f = fopen(htmlPath, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", htmlPath);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fprintf(stderr, "empty file %s\n", htmlPath);
        fclose(f);
        return 2;
    }
    char *html = (char *)malloc((size_t)size + 1);
    if (!html || fread(html, 1, (size_t)size, f) != (size_t)size)
    {
        fprintf(stderr, "read failed %s\n", htmlPath);
        free(html);
        fclose(f);
        return 2;
    }
    html[size] = '\0';
    fclose(f);
    printf("[host] replay: %s (%ld bytes) base=%s engine=%d\n", htmlPath,
           size, pageUrl, (int)JS_ENGINE_XS_NR);

    /* Stage 1 (main.c flow): scan slots + externals BEFORE parsing. */
    JsScriptSlot *slots = NULL;
    JsExtScript *ext = NULL;
    int slotCount = 0, extCount = 0;
    JsExtArena *arena = NULL;
    jsext_collect(html, pageUrl, &slots, &slotCount, &ext, &extCount, &arena);
    printf("[host] jsext_collect: slots=%d ext=%d\n", slotCount, extCount);
    if (extCount > 0)
    {
        JsExtFetch *fetch = jsext_prefetch_begin(arena, slots, slotCount,
                                                 ext, extCount);
        if (fetch)
        {
            int guard = 0;
            while (jsext_prefetch_step(fetch) == 1 && ++guard < 512)
            {
                host_http_pump(); /* http_update stand-in per frame */
            }
            if (guard >= 512)
            {
                fprintf(stderr, "prefetch stuck (missing cache entry?)\n");
            }
            JsExtArena *a2 = NULL;
            JsExtScript *e2 = NULL;
            jsext_fetch_detach(fetch, &a2, &e2, &extCount);
            arena = a2;
            ext = e2;
            printf("[host] prefetch done: %d file(s), %zu bytes\n", extCount,
                   jsext_last_bytes());
        }
        else
        {
            jsext_arena_free(arena);
            arena = NULL;
            ext = NULL;
            extCount = 0;
        }
    }
    else if (arena)
    {
        jsext_arena_free(arena);
        arena = NULL;
    }

    /* doc owns extScripts/_extArena (freed by document_free). */
    DocParseResult *d = (DocParseResult *)calloc(1, sizeof(DocParseResult));
    if (extCount > 0)
    {
        d->extScripts = ext;
        d->extScriptCount = extCount;
        d->_extArena = arena;
    }
    int rc = document_parse_ex(html, pageUrl, MODE_RAW_HTML, NULL,
                               DOC_SCRIPT_FULL, NULL, d);
    printf("[host] parsed rc=%d jsRan=%d jsErrors=%d lastErr=%s\n", rc,
           d->jsRan, d->jsErrors, d->jsLastError[0] ? d->jsLastError : "(none)");
    if (rc != 0)
    {
        document_free(d);
        free(d);
        free(html);
        return 2;
    }

    /* Per-frame pumps, mirroring main.c's js_timers_update ordering. */
    JsBridge *b = d->_jsbridge;
    dump_dom_tree(b->dom ? (DomNode *)b->dom->root : NULL, 3, 12); /* P0 */
    int last = d->jsErrors;
    int lastShownL = -1, lastShownT = -1;
    for (int i = 0; i < 600; i++)
    {
        int mutations = 0;
        unsigned now = (unsigned)(i * 16);
        int fired = jsbridge_timers_pump(b, now, &mutations);
        jsbridge_deferred_pump(b, &mutations);
        jsbridge_engine_pump(b, now, &mutations);
        jsbridge_xhr_pump(b, &mutations);
        /* A2 telemetry: print every frame something CHANGES — a silent
         * boot (queue never fills) vs a busy boot that stalls mid-work
         * print very different shapes here. */
        int L = jsbridge_listener_count(b);
        int t = jsbridge_timers_active(b);
        if (fired || L != lastShownL || t != lastShownT || mutations)
        {
            printf("[host] frame %d: fired=%d listeners=%d timers=%d mut=%d\n",
                   i, fired, L, t, mutations);
            lastShownL = L;
            lastShownT = t;
        }
        if (d->jsErrors > last)
        {
            printf("[host] pump %d: new jsErrors=%d lastErr=%s\n", i,
                   d->jsErrors, d->jsLastError[0] ? d->jsLastError : "(none)");
            last = d->jsErrors;
        }
        /* Tree watch: live root + first-child (html) child counts — the
         * frame where the tree's shape changes tells us which pump work
         * detached react's committed tree. Pointer identity included: the
         * root's child[0] can be REPLACED by a same-tag different node
         * (react fallback re-render), which child-counts alone hide. */
        if (b->dom && b->dom->root)
        {
            static int lastR = -1, lastH = -1;
            static void *lastC0 = NULL;
            static void *lastRoot = NULL;
            DomNode *rt = (DomNode *)b->dom->root;
            void *c0 = (rt->childCount > 0) ? (void *)rt->children[0] : NULL;
            int rk = rt->childCount;
            int hk = c0 ? rt->children[0]->childCount : -1;
            if (rk != lastR || hk != lastH || c0 != lastC0 || rt != lastRoot)
            {
                printf("[host] treeWatch f=%d root=%p kids=%d c0=%p tag=%s "
                       "kids=%d\n",
                       i, (void *)rt, rk, c0,
                       c0 && ((DomNode *)c0)->tag ? ((DomNode *)c0)->tag : "?",
                       hk);
                lastR = rk;
                lastH = hk;
                lastC0 = c0;
                lastRoot = (void *)rt;
            }
        }
    }
    printf("[host] replay done: jsErrors=%d%s%s\n", d->jsErrors,
           g_sawSkip ? " [skip logged]" : "",
           g_sawAbort ? " [ABORT logged]" : "");
    dump_dom_tree(b->dom ? (DomNode *)b->dom->root : NULL, 3, 12); /* P1 */
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    dom_created_dump(); /* detached fragment tops (hydration-debug) */
#endif

    /* A2 boot-state probe: the page's own state through the bridge's
     * console path — TURBOPACK queue shape (raw array vs {push:H}), the
     * __next_f RSC residue, document.readyState, and whether React
     * rendered anything into body. React-shaped pages only: the probe
     * assumes TURBOPACK exists, so running it on plain pages failed
     * them spuriously (formtap replay, "get TURBOPACK: undefined"). */
    if (html && strstr(html, "TURBOPACK"))
    {
        static const char diag[] =
            "var tp = TURBOPACK, arr = Array.isArray(tp),"
            " push = tp && typeof tp.push;"
            "console.log('DIAG turbopack: typeof=' + (typeof tp)"
            " + ' isArray=' + arr + ' push=' + push);"
            "var nf = self.__next_f;"
            "console.log('DIAG next_f: typeof=' + (typeof nf)"
            " + ' len=' + (nf ? nf.length : -1));"
            "console.log('DIAG readyState=' + document.readyState);"
            "var bh = (document.body && document.body.innerHTML)"
            " ? document.body.innerHTML.length : -1;"
            "console.log('DIAG bodyBytes=' + bh);"
            "var btn = document.querySelectorAll('button, input[type=submit]')[0];"
            "var chain = [], bn = btn;"
            "while (bn) { chain.push(bn.tagName); bn = bn.parentNode; }"
            "console.log('DIAG btnchain=' + (chain.length"
            " ? chain.join(' > ') : '(none)'));"
            "var names = ['AbortController','AbortSignal','ReadableStream',"
            "'WritableStream','TransformStream','MutationObserver',"
            "'ResizeObserver','IntersectionObserver','Event','CustomEvent',"
            "'MessageChannel','URL','URLSearchParams','performance',"
            "'requestIdleCallback','setImmediate','crypto','navigator',"
            "'location','history','localStorage','getComputedStyle',"
            "'matchMedia','structuredClone','Symbol','Proxy','Reflect',"
            "'AggregateError','FinalizationRegistry','globalThis'];"
            "var miss = '';"
            "for (var i = 0; i < names.length; i++)"
            "  if (typeof globalThis[names[i]] === 'undefined')"
            "    miss += names[i] + ' ';"
            "console.log('DIAG missing: ' + (miss || '(none)'));";
        jsbridge_page_script(b, diag, strlen(diag));
    }

    /* ── Track B6: synthetic submit-button dispatch (offline validation) ──
     * The page is boot-complete here (react commit finished in the pump
     * loop above). Find the walker-stamped submit block and drive the
     * SAME sequence main.c's activate_form_block runs: dispatch "click"
     * on the button node, then "submit" on the nearest <form> — logging
     * whether a page handler SUPPRESSED the default action, plus the
     * DOM's innerHTML shape before/after so react mutations are visible. */
    {
        const DocBlock *submitB = NULL;
        for (int bi = 0; bi < d->blockCount; bi++)
        {
            DocBlock *blk = d->blocks[bi];
            if (blk->type == DOC_BLOCK_INPUT_SUBMIT && blk->srcNode)
            {
                submitB = blk;
                break;
            }
        }
        if (submitB)
        {
            DomNode *bn = (DomNode *)submitB->srcNode;
            printf("[host] synthetic tap: tag=%s\n",
                   (bn->tag && bn->kind == DOM_ELEMENT) ? bn->tag : "?");
            {
                DomNode *p = bn;
                int d = 0, hitRoot = 0;
                for (; d < 12 && p; d++, p = p->parent)
                {
                    printf("[host] btnchain[%d] tag=%s parent=%p\n", d,
                           p->tag ? p->tag : "?", (void *)p->parent);
                }
                if (b->dom && b->dom->root)
                {
                    for (p = bn; p; p = p->parent)
                    {
                        if (p == b->dom->root)
                        {
                            hitRoot = 1;
                            break;
                        }
                    }
                }
                printf("[host] dom root=%p chainTop=%s inRoot=%d\n",
                       (void *)(b->dom && b->dom->root ? b->dom->root : NULL),
                       p ? p->tag : "(NULL)", hitRoot);
                /* live-tree shape: depth ≤ 3 census from dom root (where
                 * did react attach its committed subtree?) */
                dump_dom_tree(b->dom ? (DomNode *)b->dom->root : NULL, 3, 30);
            }
            body_inner_bytes(d, "pre-dispatch");
            int rc = jsbridge_dispatch_element_event(
                b, submitB->srcNode, "click");
            printf("[host] synthetic click rc=%d (%s)\n", rc,
                   rc == JSB_EVENT_SUPPRESSED ? "suppressed"
                   : rc == JSB_EVENT_NAVIGATE ? "navigate"
                                              : "none");
            if (rc != JSB_EVENT_SUPPRESSED)
            {
                /* Browser: click's default action is the form submission,
                 * fired even when NO click listener ran (NONE) — so the
                 * submit dispatch runs for NONE and NAVIGATE alike. */
                DomNode *formN = NULL;
                for (const DomNode *p = bn->parent; p; p = p->parent)
                {
                    if (p->kind == DOM_ELEMENT && p->tag &&
                        strcmp(p->tag, "form") == 0)
                    {
                        formN = (DomNode *)p;
                        break;
                    }
                }
                if (formN)
                {
                    int rcs = jsbridge_dispatch_element_event(
                        b, formN, "submit");
                    printf("[host] synthetic submit rc=%d (%s)\n", rcs,
                           rcs == JSB_EVENT_SUPPRESSED ? "suppressed"
                           : rcs == JSB_EVENT_NAVIGATE ? "navigate"
                                                       : "none");
                }
                else
                {
                    printf("[host] synthetic: no <form> ancestor\n");
                }
            }
            /* Deliver whatever the dispatch scheduled (react state updates
             * run on timers/microtasks; mirror the replay pump frame). */
            for (int i = 600; i < 800; i++)
            {
                int mutations = 0;
                unsigned now = (unsigned)(i * 16);
                jsbridge_timers_pump(b, now, &mutations);
                jsbridge_deferred_pump(b, &mutations);
                jsbridge_engine_pump(b, now, &mutations);
                jsbridge_xhr_pump(b, &mutations);
            }
            body_inner_bytes(d, "post-dispatch");
            printf("[host] post-dispatch jsErrors=%d\n", d->jsErrors);
            /* B6 verdict probes: the page's own handler marks prove the
             * dispatch reached JS and preventDefault suppression worked. */
            static const char verdict[] =
                "console.log('VERDICT caught=' + window.__caught"
                " + ' clickTarget=' + window.__clickTarget"
                " + ' lastInput=' + (window.__lastInput === undefined"
                " ? '(none)' : window.__lastInput)"
                " + ' status=' + (document.getElementById('status')"
                " ? document.getElementById('status').textContent"
                " : '(no status)') + ' done=' + "
                "(document.getElementById('status')"
                " ? document.getElementById('status').getAttribute('data-done')"
                " : '?'));";
            jsbridge_page_script(b, verdict, strlen(verdict));
        }
        else
        {
            printf("[host] synthetic: no stamped submit block found\n");
        }
    }

    int exitCode = (d->jsErrors == 0 && !g_sawSkip && !g_sawAbort) ? 0 : 1;

    js_doc_close(b);
    d->_jsbridge = NULL;
    document_free(d);
    free(d);
    free(html);
    return exitCode;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: nrchunk <page.html [baseUrl] | script.js>\n");
        return 2;
    }
    logger_init(pluto_pd());
    jsbridge_set_engine(JS_ENGINE_XS_NR); /* the only engine under test */
    if (ends_with(argv[1], ".html") || ends_with(argv[1], ".htm"))
    {
        return run_replay(argv[1],
                          argc > 2 ? argv[2] : "https://bryanwandrych.com/contact");
    }
    return run_single(argv[1]);
}
