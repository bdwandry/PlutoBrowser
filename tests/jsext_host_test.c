/*
 * Full-mode external <script src> host test (macOS only, NOT part of the
 * Playdate build). Compiles the REAL pipeline — tokenizer → dom →
 * document_parse_ex (DOC_SCRIPT_FULL) → jsbridge (muJS 1.3.10) — plus the
 * real html/jsext.c collector, and asserts the browser-faithful behaviors
 * (tests 6–11 from the Full-mode plan) deterministically with NO network:
 *
 *   6. An external file's code runs through the SAME engine; a later inline
 *      script on the page consumes what it defined (document order).
 *   7. An external file's document.write output is rendered like an inline
 *      script's.
 *   8. A missing external (collector marks it unfetchable) is skipped with
 *      the page and later scripts unaffected.
 *   9. The same file referenced twice is stored once and EXECUTED at both
 *      slots in page order.
 *   10. A file over the per-script byte cap (64KB) is refused before
 *      execution (its marker global never appears).
 *   11. The per-page budget (160KB) refuses a further file once the
 *      delivered bytes would exceed it.
 *
 * jsext.c's network entry points (http_get/http_cancel) are stubbed here —
 * the collector/local-fill paths under test never call them.
 *
 * Build & run (from repo root — glob html/ because document.c pulls in
 * dom.c/tokenizer; core EXCLUDES tasks.c and http_client.c, both stubbed
 * below):
 *   SDK=$HOME/Developer/PlaydateSDK; cc -o /tmp/jsexttest \
 *     tests/jsext_host_test.c Source/html/*.c \
 *     $(ls Source/core/*.c | grep -vE '/tasks.c|/http_client.c') \
 *     Source/util/*.c Source/js/muJS/*.c Source/js/duktape/duktape.c \
 *     Source/js/QuickJS/quickjs.c Source/js/QuickJS/libregexp.c \
 *     Source/js/QuickJS/libunicode.c Source/js/QuickJS/cutils.c \
 *     Source/js/QuickJS/dtoa.c Source/html/jsbridge_quickjs.c \
 *     Source/html/jsbridge_mujs.c Source/html/jsbridge_duktape.c \
 *     -I. -ISource -ISource/core -ISource/util -ISource/html -ISource/js/muJS \
 *     -ISource/js/duktape -ISource/js/QuickJS -lm \
 *     -ISource/render -I$SDK/C_API \
 *     -DTARGET_EXTENSION=1 -DPDCS_STRDUP=1 && /tmp/jsexttest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

/* ---- Fake Playdate API (same pattern as jsbridge_host_test.c) ---- */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
static struct playdate_sys g_fakeSys;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;
PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeSys.realloc = host_realloc;
        g_fakeApi.system = &g_fakeSys;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}
void pluto_free(void *p) { free(p); }
void *pluto_mem_realloc(void *p, size_t n); /* core/pluto_mem.c funnel */
void *pluto_mem_sdk_realloc(void *p, size_t n) { return realloc(p, n); }
void *pluto_realloc(void *p, size_t n) { return pluto_mem_realloc(p, n); }
void tasks_report_progress(float f) { (void)f; } /* readability stub */

/* http_client stubs: jsext.c references these for the NETWORK prefetch
 * session; the local-fill + collector paths under test never call them. */
typedef struct HttpCallbacks HttpCallbacks;
int http_get(const char *url, const HttpCallbacks *cb)
{
    (void)url;
    (void)cb;
    return 0; /* "immediate failure" — never expected in this test */
}
size_t http_internal_page_body(const char *url, const char **bodyOut)
{
    if (bodyOut) { *bodyOut = NULL; }
    return 0;
}
void http_cancel(void) {}
void http_client_init(PlaydateAPI *pd) { (void)pd; }
void http_update(void) {}
int http_is_loading(void) { return 0; }

#include "html/document.h"
#include "html/jsbridge.h"
#include "html/jsext.h"
#include "core/constants.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name)                                          \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            printf("PASS: %s\n", name);                            \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            printf("FAIL: %s\n", name);                            \
            g_fail++;                                              \
        }                                                          \
    } while (0)

static const char *find_inline_text(const DocParseResult *d,
                                    const char *needle)
{
    for (int i = 0; i < d->blockCount; i++)
    {
        const DocBlock *b = d->blocks[i];
        if (!b || !b->inlines)
            continue;
        for (int j = 0; j < b->inlineCount; j++)
        {
            const DocInline *in = b->inlines[j];
            if (in && in->text && in->type == DOC_INLINE_TEXT &&
                strstr(in->text, needle))
                return in->text;
        }
    }
    return NULL;
}

/* ── The about:jsext page (kept in sync with http_client.c's
 * JSEXTTEST_HTML — the host test cannot include that file). ── */
static const char JSEXT_PAGE[] =
    "<!DOCTYPE html><html><head><title>Full JS Test Suite</title></head><body>"
    "<h1>External Script (Full) Suite</h1>"
    "<p id=\"banner\">running</p>"
    "<div id=\"out\"></div>"
    "<script src=\"jsext-order.js\"></script>"
    "<script src=\"jsext-missing.js\"></script>"
    "<script src=\"jsext-dup.js\"></script>"
    "<script>window.__inlineA = (window.__extCount === 1) ? 'A_OK' : 'A_BAD';</script>"
    "<script src=\"jsext-dup.js\"></script>"
    "<script src=\"jsext-huge.js\"></script>"
    "<script src=\"data:application/javascript;base64,d2luZG93Ll9fZGF0YVJhbj0xOw==\"></script>"
    "<script src=\"jsext-big.js\"></script>"
    "<script src=\"jsext-big2.js\"></script>"
    "<script src=\"jsext-big3.js\"></script>"
    "<script src=\"jsext-big4.js\"></script>"
    "<script src=\"jsext-big5.js\"></script>"
    "<script src=\"jsext-big6.js\"></script>"
    "<script src=\"jsext-big7.js\"></script>"
    "<script src=\"jsext-big8.js\"></script>"
    "<script src=\"jsext-big9.js\"></script>"
    "<script src=\"jsext-write.js\"></script>"
    "<script>"
    "var out = document.getElementById('out');"
    "var pass = 0, fail = 0;"
    "function T(name, fn) {"
    "  var r;"
    "  try { r = fn(); } catch (e) { r = 'threw: ' + e; }"
    "  if (r === true) { pass = pass + 1; } else { fail = fail + 1; }"
    "  var p = document.createElement('p');"
    "  p.textContent = '[' + (r === true ? 'PASS' : 'FAIL') + '] ' + name +"
    "    (r === true ? '' : ' :: ' + r);"
    "  out.appendChild(p);"
    "}"
    "try {"
    "  T('external defined global (order+engine)', function(){"
    "    return window.__extOrder === 'EXT_OK'; });"
    "  T('inline-after-external saw ext state', function(){"
    "    return window.__inlineA === 'A_OK'; });"
    "  T('dup: downloaded once, executed twice', function(){"
    "    return window.__extCount === 2; });"
    "  T('over-cap file executed (disk-resident)', function(){"
    "    return window.__huge === 1; });"
    "  T('over-budget file refused (no execution)', function(){"
    "    return typeof window.__big3 === 'undefined'; });"
    "  T('big files did not break the engine', function(){"
    "    return typeof window.__extOrder === 'string'; });"
    "  T('72KB file ran from disk (SW2b)', function(){"
    "    return window.__huge === 1; });"
    "  T('data:-URL script executed (SW2d)', function(){"
    "    return window.__dataRan === 1; });"
    "  var b = document.getElementById('banner');"
    "  b.textContent = pass + ' passed, ' + fail + ' failed.';"
    "} catch (e) {"
    "  document.getElementById('banner').textContent = 'Suite error: ' + e;"
    "}"
    "</script>"
    "</body></html>";

/* Small page for URL-resolution checks (relative + protocol-relative). */
static const char RESOLVE_PAGE[] =
    "<html><head><title>Resolve</title></head><body>"
    "<script src=\"js/rel.js\"></script>"
    "<script src=\"//cdn.example.com/abs.js\"></script>"
    "<script src=\"js/rel.js\"></script>"
    "</body></html>";

int main(void)
{
    /* ── 1. Collector: scan, resolve, dedupe, local-fill caps/budgets ──── */
    JsScriptSlot *slots = NULL;
    JsExtScript *ext = NULL;
    int slotCount = 0, extCount = 0;
    JsExtArena *arena = NULL;
    int total = jsext_collect(JSEXT_PAGE, "about:jsext", &slots, &slotCount,
                              &ext, &extCount, &arena);
    CHECK(total > 0 && slots && ext && arena, "collect: slot table built");
    CHECK(extCount == 14, /* SW2d: the data: script adds NO ext entry */
          "collect: 14 unique externals (dup stored once, missing kept)");
    /* Document-order interleaving: order, missing, dup#1, INLINE, dup#2,
     * huge, big1..big8, big9, write, INLINE-report. */
    CHECK(total == 18, /* SW2d: +1 data:-URL script element */
          "collect: 18 script elements in document order");
    CHECK(slots[0].isExt == 1 && slots[0].extIndex == 0,
          "collect: slot0 = first external");
    CHECK(slots[2].isExt == 1 && slots[2].extIndex == 2 &&
              slots[4].isExt == 1 && slots[4].extIndex == 2,
          "collect: dup occupies two slots -> one ext entry");
    CHECK(slots[3].isExt == 0 && slots[3].inlineLen > 0,
          "collect: inline script keeps its body position");
    CHECK(strcmp(ext[1].url, "jsext-missing.js") == 0,
          "collect: about: base keeps the raw relative src for local fill");

    /* SW3: pin the CLASSIC 160KB page budget for the refusal fixture below
     * (the production default rose to 512KB, under which everything fits).
     * Must be set BEFORE local_fill — it reads the effective budget. */
    jsext_set_page_budget(160 * 1024);
    CHECK(jsext_local_fill(arena, ext, extCount) == extCount,
          "local fill: ran over the whole table");
    for (int i = 0; i < extCount; i++)
    {
        printf("  [diag] ext[%d] url='%s' body=%s len=%zu\n", i, ext[i].url,
               ext[i].body ? "yes" : "no", ext[i].len);
    }
    CHECK(ext[0].body && strstr(ext[0].body, "__extOrder"),
          "local fill: order.js served");
    CHECK(ext[1].url[0] == '\0' && !ext[1].body,
          "local fill: missing.js zeroed (skip + log path)");
    /* SW2b: huge.js (72,819B > 64KB RAM threshold) is now DISK-resident —
     * accepted, not refused. body stays NULL; spill holds the bytes. */
    CHECK(ext[3].url[0] != '\0' && !ext[3].body && ext[3].spill >= 0 &&
              ext[3].len == 72819,
          "local fill: 72KB huge.js accepted as disk-resident (SW2b)");
    CHECK(ext[4].body && ext[4].len == 16800 &&
              ext[11].body && ext[11].len == 16800,
          "local fill: all eight 16KB files accepted (first + last checked)");
    CHECK(ext[12].url[0] == '\0' && !ext[12].body,
          "local fill: 56KB big9 refused by the remaining page budget");
    CHECK(ext[13].body && strstr(ext[13].body, "document.write"),
          "local fill: write.js served (last accepted file)");

    /* ── 2. FULL execution through the real pipeline ───────────────────── */
    {
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        /* Hand the collected table to the doc EXACTLY like render_step does
         * (document_free then owns bodies + arena). */
        d->extScripts = ext;
        d->extScriptCount = extCount;
        d->_extArena = arena;
        int rc = document_parse_ex(JSEXT_PAGE, "about:jsext", MODE_RAW_HTML,
                                   NULL, DOC_SCRIPT_FULL, NULL, d);
        CHECK(rc == 0, "full: document_parse_ex returns 0");
        printf("  [diag] full: jsRan=%d jsErrors=%d last='%s'\n", d->jsRan,
               d->jsErrors, d->jsLastError);
        CHECK(d->jsErrors == 0, "full: no JS errors");
        CHECK(d->jsRan == 16, /* SW2b disk + SW2d data: leg */
              "full: 15 runs (order, dup x2, big1-8, huge, write, 2 inline)");
        CHECK(find_inline_text(d, "[PASS] external defined global"),
              "full(6): external global visible to the engine");
        CHECK(find_inline_text(d, "[PASS] inline-after-external"),
              "full(6): later INLINE script consumed external state");
        CHECK(find_inline_text(d, "[PASS] dup: downloaded once, executed twice"),
              "full(9): same file executed at both slots");
        CHECK(find_inline_text(d, "[PASS] over-cap file executed"),
              "full(10): former over-cap file now runs from disk (SW2b)");
        CHECK(find_inline_text(d, "[PASS] over-budget file refused"),
              "full(11): over-budget file never executed");
        CHECK(find_inline_text(d, "8 passed, 0 failed"),
              "full: page banner shows 8 passed, 0 failed (SW2b+SW2d)");
        if (!find_inline_text(d, "8 passed, 0 failed"))
        {
            /* [diag] dump every inline text so a banner mismatch shows. */
            for (int bi = 0; bi < d->blockCount && bi < 60; bi++)
            {
                const DocBlock *db = d->blocks[bi];
                if (!db || !db->inlines)
                    continue;
                for (int j = 0; j < db->inlineCount; j++)
                {
                    const DocInline *in = db->inlines[j];
                    if (in && in->text && in->type == DOC_INLINE_TEXT)
                        printf("  [diag] text: %.90s\n", in->text);
                }
            }
        }
        CHECK(find_inline_text(d, "EXT-WROTE"),
              "full(7): external document.write output rendered");
        printf("  [diag] post-close, pre-free\n");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d); /* also frees the arena + ext bodies */
        printf("  [diag] post-free\n");
        free(d);
    }

    /* ── 3. Collector on a NETWORK page: resolution + dedupe by raw src ── */
    {
        JsScriptSlot *s2 = NULL;
        JsExtScript *e2 = NULL;
        int sc2 = 0, ec2 = 0;
        JsExtArena *a2 = NULL;
        jsext_collect(RESOLVE_PAGE, "https://ex.com/a/b/page.html", &s2,
                      &sc2, &e2, &ec2, &a2);
        CHECK(ec2 == 2, "resolve: 2 unique files (raw-string dedupe)");
        CHECK(strcmp(e2[0].url, "https://ex.com/a/b/js/rel.js") == 0,
              "resolve: relative src resolved against the page URL");
        CHECK(strcmp(e2[1].url, "https://cdn.example.com/abs.js") == 0,
              "resolve: protocol-relative src got the page scheme");
        CHECK(s2[0].isExt == 1 && s2[2].isExt == 1 &&
                  s2[0].extIndex == 0 && s2[2].extIndex == 0,
              "resolve: repeated relative src maps to the same entry");
        jsext_arena_free(a2);
    }

    /* ── 4. INLINE regression: src= elements are still skipped entirely ── */
    {
        JsScriptSlot *s3 = NULL;
        JsExtScript *e3 = NULL;
        int sc3 = 0, ec3 = 0;
        JsExtArena *a3 = NULL;
        int t3 = jsext_collect(JSEXT_PAGE, "about:jsext", &s3, &sc3, &e3,
                               &ec3, &a3);
        (void)t3;
        jsext_arena_free(a3);
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(JSEXT_PAGE, "about:jsext", MODE_RAW_HTML, NULL,
                          DOC_SCRIPT_RUN_KEEP, NULL, d);
        CHECK(d->jsRan == 2 && d->jsErrors == 0,
              "inline: exactly the 2 inline scripts ran (externals skipped)");
        CHECK(!find_inline_text(d, "EXT_OK"),
              "inline: no external code leaked into Inline mode");
        printf("  [diag] inline: jsRan=%d jsErrors=%d last='%s'\n", d->jsRan,
               d->jsErrors, d->jsLastError);
        printf("  [diag] pre-close\n");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
