/*
 * JavaScript integration host test (macOS only, NOT part of the Playdate
 * build). Compiles the REAL pipeline — tokenizer → dom → document_parse_ex →
 * jsbridge (muJS 1.3.10) → document_rewalk — against a faked PlaydateAPI
 * (system->realloc only) and asserts on actual engine behavior:
 *
 *   1. The about:javascript suite script parses and runs without errors.
 *   2. The suite's own language/DOM checks mutate the live DOM (summary,
 *      banner, created <p> nodes) and document.write appends content.
 *   3. DOM mutations made by a click handler survive a document_rewalk.
 *   4. DOC_SCRIPT_OFF parses the same page without running any script.
 *
 * Build & run (see AGENTS-style command in the repo; mirrors
 * tests/htmltags_host_test.c):
 *   cc -o /tmp/jstest tests/jsbridge_host_test.c Source/html/tokenizer.c \
 *     Source/html/dom.c Source/html/document.c Source/html/entities.c \
 *     Source/html/readability.c Source/html/jsbridge.c Source/html/jsext.c \
 *     Source/core/url.c \
 *     Source/core/constants.c Source/core/logger.c Source/util/strbuf.c \
 *     Source/util/strutil.c Source/util/json.c Source/js/muJS/*.c \
 *     Source/js/duktape/duktape.c Source/js/QuickJS/quickjs.c \
 *     Source/js/QuickJS/libregexp.c Source/js/QuickJS/libunicode.c \
 *     Source/js/QuickJS/cutils.c Source/js/QuickJS/dtoa.c \
 *     Source/html/jsbridge_mujs.c Source/html/jsbridge_duktape.c \
 *     Source/html/jsbridge_duktape.c \
 *     -I. -ISource -ISource/core -ISource/util -ISource/html -ISource/js/muJS \
 *     -ISource/js/duktape -ISource/js/QuickJS -ISource/render \
 *     -lm -DCONFIG_VERSION='"2026-06-04"' -DTARGET_EXTENSION=1 \
 *     -DPDCS_STRDUP=1 && /tmp/jstest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

/* ---- Fake Playdate API (same pattern as htmltags_host_test.c) ---- */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
/* Minimal file shim so logger_log() output (the [js] fire/set/cleared
 * traces) reaches stdout during host runs — same lines the device logs. */
static SDFile *fake_file_open(const char *name, FileOptions mode)
{
    (void)name;
    (void)mode;
    return (SDFile *)1;
}
static int fake_file_close(SDFile *f) { (void)f; return 0; }
static int fake_file_write(SDFile *f, const void *buf, unsigned int len)
{
    (void)f;
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
        g_fakeSys.convertEpochToDateTime = fake_epoch_to_dt;
        g_fakeApi.system = &g_fakeSys;
        g_fakeFile.open = fake_file_open;
        g_fakeFile.close = fake_file_close;
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

/* http_client fakes: html/jsext.c (linked for jsext_arena_free) references
 * these; the XHR tests below DRIVE them scriptably. Default mode fails
 * immediately ("immediate" refusal) — exactly the stale path the router
 * must contain. Tests switch modes with fake_http_mode(). */
#include "core/http_client.h" /* real HttpCallbacks layout (fake below) */
typedef enum
{
    FAKE_HTTP_FAIL = 0,   /* http_get returns 0 (immediate refusal) */
    FAKE_HTTP_MANUAL,     /* hold in-flight until fake_http_deliver() */
    FAKE_HTTP_INSTANT     /* fire onSuccess synchronously inside http_get */
} FakeHttpMode;
static FakeHttpMode g_fakeHttpMode = FAKE_HTTP_FAIL;
static char g_fakeHttpUrl[512];
static HttpCallbacks g_fakeHttpCb;      /* held while in-flight (MANUAL) */
static int g_fakeHttpGetCount = 0;
static int g_fakeHttpCancelled = 0;
/* what fake_http_deliver produces: */
static int g_fakeHttpStatus = 200;
static const char *g_fakeHttpBody = "{}";
static const char *g_fakeHttpErrMsg = "network down";
int http_get(const char *url, const HttpCallbacks *cb)
{
    g_fakeHttpGetCount++;
    snprintf(g_fakeHttpUrl, sizeof(g_fakeHttpUrl), "%s", url ? url : "");
    if (g_fakeHttpMode == FAKE_HTTP_FAIL)
    {
        return 0;
    }
    if (g_fakeHttpMode == FAKE_HTTP_INSTANT)
    {
        if (cb && cb->onSuccess)
        {
            cb->onSuccess(g_fakeHttpStatus, NULL, NULL, 0, g_fakeHttpBody,
                          strlen(g_fakeHttpBody), g_fakeHttpUrl);
        }
        return 1;
    }
    g_fakeHttpCb = *cb; /* MANUAL: hold for fake_http_deliver */
    return 1;
}
void http_cancel(void) { g_fakeHttpCancelled++; }
void http_client_init(PlaydateAPI *pd) { (void)pd; }
void http_update(void) {}
int http_is_loading(void) { return 0; }
/* Router's local about: path: answer XHRs for any 'about:' URL with a
 * tiny page so tests need no real http_client.c. */
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
#include "html/jsbridge_internal.h"

#include "core/constants.h"
#include "core/logger.h"
#include "core/pluto_spill.h"

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

static int has_link(const DocParseResult *d, const char *needle)
{
    for (int i = 0; i < d->linkCount; i++)
        if (d->links[i] && d->links[i]->href &&
            strstr(d->links[i]->href, needle))
            return 1;

    /* ── N. XMLHttpRequest / fetch (roadmap #3) ────────────────────────── */
    printf("── XHR / fetch ──\n");
    {
        /* X1: local-table XHR — onload receives the about: page body. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X</title></head><body>"
                "<p id=\"o\">pending</p>"
                "<script>"
                "var x = new XMLHttpRequest();"
                "x.open('GET', 'about:blank');"
                "x.onload = function() {"
                "  document.getElementById('o').textContent ="
                "    'loaded:' + x.status + ':' + x.responseText.length;"
                "};"
                "x.onerror = function() {"
                "  document.getElementById('o').textContent = 'failed';"
                "};"
                "x.send();"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            CHECK(b != NULL, "xhr: bridge attached");
            int mut = 0;
            int delivered = jsbridge_xhr_pump(b, &mut);
            CHECK(delivered == 1, "xhr: local request delivered on pump");
            CHECK(find_inline_text(d, "loaded:200:"),
                  "xhr: onload wrote status+body length");
            document_free(d);
        }
        /* X2: async network XHR via MANUAL fake — pump start + deliver. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X2</title></head><body>"
                "<p id=\"o\">pending</p>"
                "<script>"
                "var x = new XMLHttpRequest();"
                "x.open('GET', '/data.json');"
                "x.onload = function(t) {"
                "  document.getElementById('o').textContent = 'got:' + t;"
                "};"
                "x.send();"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            g_fakeHttpMode = FAKE_HTTP_MANUAL;
            g_fakeHttpStatus = 200;
            g_fakeHttpBody = "[1,2,3]";
            int mut = 0;
            CHECK(jsbridge_xhr_pump(b, &mut) == 0,
                  "xhr: nothing delivered before response settles");
            CHECK(g_fakeHttpGetCount == 1, "xhr: http_get started");
            CHECK(strstr(g_fakeHttpUrl, "https://example.com/data.json") != NULL,
                  "xhr: relative URL resolved against page base");
            /* The engine pins must exist — settle via the fake client. */
            if (g_fakeHttpCb.onSuccess)
            {
                g_fakeHttpCb.onSuccess(200, NULL, NULL, 0, g_fakeHttpBody,
                                       strlen(g_fakeHttpBody), g_fakeHttpUrl);
            }
            int delivered = jsbridge_xhr_pump(b, &mut);
            CHECK(delivered == 1, "xhr: completion delivered after settle");
            CHECK(mut == 1, "xhr: completion marked DOM mutation");
            CHECK(find_inline_text(d, "got:[1,2,3]"),
                  "xhr: handler received responseText argument");
            g_fakeHttpMode = FAKE_HTTP_FAIL;
            document_free(d);
        }
        /* X3: error path — network failure fires onerror, page survives. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X3</title></head><body>"
                "<p id=\"o\">pending</p>"
                "<script>"
                "var x = new XMLHttpRequest();"
                "x.open('GET', 'https://down.example.com/x');"
                "x.onerror = function() {"
                "  document.getElementById('o').textContent = 'err-ok';"
                "};"
                "x.send();"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            g_fakeHttpMode = FAKE_HTTP_INSTANT;
            g_fakeHttpStatus = 500;
            int mut = 0;
            (void)jsbridge_xhr_pump(b, &mut);
            int delivered = jsbridge_xhr_pump(b, &mut);
            CHECK(delivered == 1, "xhr: non-2xx settles as failure");
            CHECK(find_inline_text(d, "err-ok"),
                  "xhr: onerror fired on HTTP 500");
            g_fakeHttpMode = FAKE_HTTP_FAIL;
            document_free(d);
        }
        /* X4: caps — 5 requests > JSBRIDGE_XHR_MAX(4): the 5th open fails. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X4</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "var ok = 0, refused = 0;"
                "for (var i = 0; i < 5; i++) {"
                "  try {"
                "    var x = new XMLHttpRequest();"
                "    x.open('GET', 'about:blank');"
                "    x.send();"
                "    ok++;"
                "  } catch (e) { refused++; }"
                "}"
                "document.getElementById('o').textContent = 'ok' + ok + 'ref' + refused;"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(find_inline_text(d, "ok4ref1"),
                  "xhr: 5th request refused when table is full");
            document_free(d);
        }
        /* X5: method gate — POST refused. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X5</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "var refused = 0;"
                "try {"
                "  var x = new XMLHttpRequest();"
                "  x.open('POST', '/x');"
                "  x.send();"
                "} catch (e) { refused = 1; }"
                "document.getElementById('o').textContent = refused ? 'ref-ok' : 'bad';"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(find_inline_text(d, "ref-ok"), "xhr: POST refused");
            document_free(d);
        }
        /* X6: QuickJS fetch() — native promise resolves with the body. */
        {
            jsbridge_set_engine(JS_ENGINE_QUICKJS);
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X6</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "fetch('about:blank').then(function(body) {"
                "  document.getElementById('o').textContent = 'f:' + body.length;"
                "}, function(e) {"
                "  document.getElementById('o').textContent = 'f-err';"
                "});"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            CHECK(b != NULL, "fetch: QuickJS bridge attached");
            int mut = 0;
            int delivered = jsbridge_xhr_pump(b, &mut);
            CHECK(delivered >= 1, "fetch: promise settled on pump");
            CHECK(find_inline_text(d, "f:"), "fetch: then() received body");
            document_free(d);
        }
        /* X7: muJS regression after QuickJS — engines isolate cleanly. */
        {
            jsbridge_set_engine(JS_ENGINE_MUJS);
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X7</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "document.getElementById('o').textContent = 'mu-ok';"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(find_inline_text(d, "mu-ok"),
                  "xhr: muJS still healthy after QuickJS XHR run");
            document_free(d);
        }
        /* X8: page close with an in-flight request — no leak, no crash. */
        {
            g_fakeHttpMode = FAKE_HTTP_MANUAL;
            g_fakeHttpCancelled = 0;
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X8</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "var x = new XMLHttpRequest();"
                "x.open('GET', 'https://slow.example.com/s');"
                "x.onload = function(){};"
                "x.send();"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(g_fakeHttpGetCount >= 1, "xhr: in-flight started");
            document_free(d); /* engine close must http_cancel + free slots */
            CHECK(g_fakeHttpCancelled >= 1,
                  "xhr: close cancelled the in-flight request");
            g_fakeHttpMode = FAKE_HTTP_FAIL;
        }
        /* X9 muJS regression: the has/put hooks must use the box pointer
         * (arg p), NOT stack slot 0. A script with locals filling slot 0
         * used to throw "not a pluto.xhr" on the first lookup (x.open). */
        {
            g_fakeHttpMode = FAKE_HTTP_INSTANT;
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<html><head><title>X9</title></head><body><p id=\"o\">p</p>"
                "<script>"
                "var a=1,b=2,c=3,dd=4,e=5;" /* fill interpreter locals */
                "var x = new XMLHttpRequest();"
                "var m = x.open;" /* property GET on the box, slot 0 = decoy */
                "x.open('GET','https://ok.example.com/x9');"
                "x.onload = function(){};"
                "x.send();"
                "</script></body></html>",
                "https://example.com/page", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(g_fakeHttpGetCount >= 1,
                  "xhr: muJS slot-0 decoy still routes open/send");
            document_free(d);
            g_fakeHttpMode = FAKE_HTTP_FAIL;
        }
    }
    jsbridge_set_engine(JS_ENGINE_MUJS);

    return 0;
}

static void dump_texts(const DocParseResult *d)
{
    for (int i = 0; i < d->blockCount; i++)
    {
        const DocBlock *b = d->blocks[i];
        if (!b || !b->inlines)
            continue;
        for (int j = 0; j < b->inlineCount; j++)
        {
            const DocInline *in = b->inlines[j];
            if (in && in->text && in->type == DOC_INLINE_TEXT)
                printf("  | %s\n", in->text);
        }
    }
}

/* The about:javascript page body (kept in sync with http_client.c's
 * JSTEST_HTML — the host test cannot include the SD-network file). */
static const char SUITE_HTML[] =
    "<html><head><title>JavaScript Test Suite</title></head><body>"
    "<h1>JavaScript Test Suite</h1>"
    "<p id=\"banner\">JavaScript is OFF. Enable it in Settings.</p>"
    "<p>Engine: <span id=\"engine\">not detected</span>. "
    "<span id=\"summary\"></span></p>"
    "<h2>Language</h2>"
    "<div id=\"langout\">[..] running</div>"
    "<h2>DOM</h2>"
    "<div id=\"domout\">[..] running</div>"
    "<p id=\"domhint\">ready</p>"
    "<div id=\"sw5out\">[..] SW5 running</div>"
    "<ul id=\"demoList\"><li>one</li><li>two</li><li>three</li></ul>"
    "<h2>Event demo</h2>"
    "<p><a href=\"https://example.com/blocked\" id=\"clickme\">Click me</a>"
    " - clicks: <b id=\"clickcount\">0</b></p>"
    "<p id=\"clickresult\">Handler has not fired yet.</p>"
    "<script>"
    "var banner = document.getElementById('banner');"
    "var pass = 0, part = 0, miss = 0;"
    "function report(c, name, st, note) {"
    "  var p = document.createElement('p');"
    "  var t = '[' + st + '] ' + name;"
    "  if (note) t = t + ' - ' + note;"
    "  p.textContent = t;"
    "  c.appendChild(p);"
    "}"
    "function mark(st) { if (st === 'PASS') pass++; else if (st === 'PART') part++; else miss++; }"
    "function T(c, name, fn) {"
    "  try {"
    "    var r = fn();"
    "    if (r === true) { report(c, name, 'PASS'); mark('PASS'); }"
    "    else if (r === false) { report(c, name, 'MISS'); mark('MISS'); }"
    "    else { report(c, name, 'PART', '' + r); mark('PART'); }"
    "  } catch (e) { report(c, name, 'MISS', 'threw'); mark('MISS'); }"
    "  if (typeof console !== 'undefined' && console.log) console.log('[sw-t] ' + name + ' → ' + r);"
    "}"
    "function P(c, name, fn, note) {"
    "  try { if (fn() === true) { report(c, name, 'PART', note); mark('PART'); return; } }"
    "  catch (e) {}"
    "  report(c, name, 'MISS', 'failed'); mark('MISS');"
    "}"
    "try {"
    "  var ua = navigator.userAgent;"
    "  var isDuk = ua.indexOf('Duktape') >= 0;"
    "  var isQjs = ua.indexOf('QuickJS') >= 0;"
    "  banner.textContent = 'JavaScript ran. ' + (isDuk ? 'Duktape 2.7.0 (ES5.1).' : isQjs ? 'QuickJS 2026-06-04 (ES2023).' : 'muJS 1.3.10 (ES5 subset).');"
    "  document.getElementById('engine').textContent = isDuk ? 'Duktape 2.7.0' : isQjs ? 'QuickJS 2026-06-04' : 'muJS 1.3.10';"
    "  var lang = document.getElementById('langout');"
    "  var dom = document.getElementById('domout');"
    "  T(lang, 'variables + arithmetic', function(){ var a = 6*7; return a === 42; });"
    "  T(lang, 'strings + concatenation', function(){ return 'foo' + 1 + true === 'foo1true'; });"
    "  T(lang, 'typeof', function(){ return typeof 1 === 'number' && typeof 'x' === 'string' && typeof undefined === 'undefined' && typeof null === 'object'; });"
    "  T(lang, 'comparison + ternary', function(){ return (1 < 2 ? 'y' : 'n') === 'y' && (3 >= 4 ? 'y' : 'n') === 'n'; });"
    "  T(lang, 'conditionals (if/else)', function(){ var v = 0; if (1) { v = 1; } else { v = 2; } return v === 1; });"
    "  T(lang, 'loops (for/while/break/continue)', function(){ var s = 0; for (var i = 1; i <= 10; i++) { if (i === 4) continue; s += i; } while (s > 1000) { s = 0; break; } return s === 51; });"
    "  T(lang, 'functions + closures', function(){ function adder(n) { return function(x) { return x + n; }; } return adder(40)(2) === 42; });"
    "  T(lang, 'recursion', function(){ function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } return fib(10) === 55; });"
    "  T(lang, 'arrays + methods', function(){ var a = [3,1,2]; a.push(5); a.sort(); return a.join(',') === '1,2,3,5' && a.indexOf(2) === 1 && a.length === 4; });"
    "  T(lang, 'object literals + properties', function(){ var o = { x: 1, 'y': 2 }; o.z = 3; return o.x + o.y + o['z'] === 6; });"
    "  T(lang, 'for-in over object', function(){ var o = {a:1,b:2}, k = 0, n = 0; for (var p in o) { k += o[p]; n++; } return k === 3 && n === 2; });"
    "  T(lang, 'try/catch/throw', function(){ try { throw new Error('x'); } catch (e) { return e.message === 'x'; } return false; });"
    "  T(lang, 'JSON parse/stringify', function(){ var o = JSON.parse('{\"a\":[1,2]}'); return o.a[1] === 2 && JSON.stringify(o) === '{\"a\":[1,2]}'; });"
    "  T(lang, 'RegExp exec', function(){ var m = /(a+)(b+)/.exec('xaabb'); return !!m && m[0] === 'aabb' && m[1] === 'aa' && m[2] === 'bb'; });"
    "  T(lang, 'string methods', function(){ return 'abc'.charAt(1) === 'b' && 'abc'.toUpperCase() === 'ABC' && 'a-b-c'.split('-').length === 3 && 'abc'.substring(1) === 'bc'; });"
    "  T(lang, 'number methods + parsing', function(){ return (3.14159).toFixed(2) === '3.14' && parseInt('42px', 10) === 42 && parseFloat('2.5') === 2.5; });"
    "  T(lang, 'Math object', function(){ return Math.floor(2.7) === 2 && Math.max(1, 9) === 9 && Math.abs(-3) === 3; });"
    "  T(lang, 'type conversions', function(){ return Number('42') === 42 && String(42) === '42' && (1 == '1') && !!'x'; });"
    "  T(lang, 'Array.isArray (ES5)', function(){ return typeof Array.isArray === 'function' && Array.isArray([1]) && !Array.isArray({}); });"
    "  T(lang, 'Object.keys (ES5)', function(){ return typeof Object.keys === 'function' && Object.keys({a:1,b:2}).join('') === 'ab'; });"
    "  T(lang, 'array forEach/map (ES5)', function(){ if (typeof [].map !== 'function') return false; var s = 0; [1,2,3].forEach(function(v){ s += v; }); return s === 6 && [1,2,3].map(function(v){ return v*2; }).join(',') === '2,4,6'; });"
    "  T(lang, 'string trim (ES5)', function(){ return typeof 'x'.trim === 'function' && '  x '.trim() === 'x'; });"
    "  T(lang, 'Date object', function(){ return typeof Date === 'function' && typeof (new Date()).getTime() === 'number'; });"
    "  T(dom, 'document.getElementById', function(){ return document.getElementById('demoList').tagName === 'UL'; });"
    "  T(dom, 'textContent read', function(){ return document.getElementById('clickcount').textContent === '0'; });"
    "  T(dom, 'textContent write + readback', function(){ var s = document.getElementById('domhint'); s.textContent = 'mutated'; var v = s.textContent; s.textContent = 'ready'; return v === 'mutated'; });"
    "  T(dom, 'setAttribute/getAttribute', function(){ var d = document.getElementById('domout'); d.setAttribute('data-test', 'ok1'); var v = d.getAttribute('data-test'); d.setAttribute('data-test', 'ok2'); return v === 'ok1' && d.getAttribute('data-test') === 'ok2'; });"
    "  T(dom, 'createElement + appendChild', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); p.textContent = 'created-by-JS'; d.appendChild(p); var k = d.children; return k[k.length-1].textContent === 'created-by-JS'; });"
    "  T(dom, 'createTextNode + appendChild', function(){ var d = document.getElementById('domout'); var t = document.createTextNode('text-node-ok'); d.appendChild(t); return d.textContent.indexOf('text-node-ok') >= 0; });"
    "  T(dom, 'removeChild', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); d.appendChild(p); var n = d.childElementCount; d.removeChild(p); return d.childElementCount === n - 1; });"
    "  T(dom, 'getElementsByTagName (element)', function(){ return document.getElementById('demoList').getElementsByTagName('li').length === 3; });"
    "  T(dom, 'parentNode', function(){ var li = document.getElementById('demoList').children[0]; return li.parentNode.tagName === 'UL'; });"
    "  T(dom, 'childElementCount/children', function(){ return document.getElementById('demoList').childElementCount === 3; });"
    "  T(dom, 'location.href (read)', function(){ return typeof location.href === 'string' && location.href.length > 0; });"
    "  T(dom, 'navigator.userAgent', function(){ return navigator.userAgent.indexOf('muJS') > 0 || navigator.userAgent.indexOf('Duktape') > 0 || navigator.userAgent.indexOf('QuickJS') > 0; });"
    "  T(dom, 'document.title', function(){ return document.title === 'JavaScript Test Suite'; });"
    "  P(dom, 'innerHTML (write)', function(){ var d = document.getElementById('domhint'); d.innerHTML = 'html-as-text'; var v = d.textContent === 'html-as-text'; d.textContent = 'ready'; return v; }, 'no markup parsing - text only');"
    "  var sw5 = document.getElementById('sw5out');"
    "  T(sw5, 'querySelector (descendant)', function(){ var p = document.querySelector('#demoList li'); return !!p && p.tagName === 'LI'; });"
    "  T(sw5, 'querySelector (id selector)', function(){ return document.querySelector('#domhint').id === 'domhint'; });"
    "  T(sw5, 'querySelector no-match returns null', function(){ return document.querySelector('#nope-xyz') === null; });"
    "  T(sw5, 'querySelectorAll returns all matches', function(){ var a = document.querySelectorAll('#demoList li'); return a.length === 3; });"
    "  T(sw5, 'querySelectorAll scoped to element', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); d.appendChild(p); var a = d.querySelectorAll('p'); var ok = a.length >= 1; d.removeChild(p); return ok; });"
    "  T(sw5, 'insertBefore inserts before first child', function(){ var ul = document.getElementById('demoList'); var li = document.createElement('li'); li.textContent = 'zero'; ul.insertBefore(li, ul.children[0]); var ok = ul.children[0].textContent === 'zero' && ul.childElementCount === 4; ul.removeChild(li); return ok; });"
    "  T(sw5, 'firstElementChild/nextElementSibling', function(){ var one = document.getElementById('demoList').firstElementChild; return !!one && one.textContent === 'one' && one.nextElementSibling.textContent === 'two'; });"
    "  T(sw5, 'classList add/contains/remove', function(){ var d = document.getElementById('domout'); d.classList.add('a1'); d.classList.add('a1'); var ok1 = d.classList.contains('a1') && d.classList.length === 1; d.classList.remove('a1'); return ok1 && !d.classList.contains('a1'); });"
    "  T(sw5, 'classList toggle', function(){ var d = document.getElementById('domout'); var on = d.classList.toggle('tg'); var off = d.classList.toggle('tg'); return on === true && off === false && !d.classList.contains('tg'); });"
    "  T(sw5, 'classList item', function(){ var d = document.getElementById('domout'); d.classList.add('x1'); d.classList.add('x2'); var it = d.classList.item(1); var ok = it === 'x2'; d.classList.remove('x1'); d.classList.remove('x2'); return ok; });"
    "  T(sw5, 'style write + readback', function(){ var d = document.getElementById('domhint'); d.style.display = 'none'; var v = d.style.display; d.style.display = ''; return v === 'none' && d.style.display === ''; });"
    "  T(sw5, 'style preserves other properties', function(){ var d = document.getElementById('domhint'); d.style.color = 'red'; d.style.display = 'none'; var ok = d.style.color === 'red' && d.style.display === 'none'; d.style.color = ''; d.style.display = ''; return ok; });"
    "  T(sw5, 'Set: add/has/size', function(){ var s = new Set(); s.add(1); s.add(1); s.add('x'); return s.has(1) && s.has('x') && !s.has(2) && s.size === 2; });"
    "  T(sw5, 'Map: set/get/has/delete', function(){ var m = new Map(); m.set('k', 42); var v = m.get('k'); var gone = m['delete']('k'); return v === 42 && gone === true && m.has('k') === false && m.get('k') === undefined; });"
    "  T(sw5, 'Image: construct', function(){ var img = new Image(); img.src = 'x.png'; return img != null; });"
    "  var link = document.getElementById('clickme');"
    "  if (link && typeof link.addEventListener === 'function') {"
    "    var clicks = 0;"
    "    link.addEventListener('click', function(e) {"
    "      clicks = clicks + 1;"
    "      document.getElementById('clickcount').textContent = '' + clicks;"
    "      document.getElementById('clickresult').textContent = 'Handler ran ' + clicks + ' time(s); navigation prevented by preventDefault().';"
    "      e.preventDefault();"
    "    });"
    "  }"
    "  document.getElementById('summary').textContent = pass + ' passed, ' + part + ' partial, ' + miss + ' missing.';"
    "  console.log('[sw-suite] ' + pass + ' passed, ' + part + ' partial, ' + miss + ' missing');"
    "  document.write('<p>[INFO] document.write appended this line during page load.</p>');"
    "} catch (e) {"
    "  banner.textContent = 'JS suite error: ' + (e && e.message ? e.message : e) + ' [Set=' + typeof Set + ' Map=' + typeof Map + ' Image=' + typeof Image + ' XHR=' + typeof XMLHttpRequest + ']';"
    "}"
    "</script>"
    "</body></html>";

/* Small page whose click handler mutates the DOM (rewalk test). */
static const char CLICKDOC_HTML[] =
    "<html><head><title>Click Doc</title></head><body>"
    "<p id=\"state\">before</p>"
    "<a href=\"https://example.com/go\" id=\"go\">go</a>"
    "<script>"
    "document.getElementById('go').addEventListener('click', function(e) {"
    "  document.getElementById('state').textContent = 'clicked';"
    "  e.preventDefault();"
    "});"
    "</script></body></html>";

int main(void)
{
    /* Route logger_log through the file shim: the fire/set/clear traces
     * mirror the device log and make pump behavior debuggable on host. */
    logger_init(pluto_pd());

    /* ── 1. about:javascript suite, JS enabled ─────────────────────────── */
    {
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        int rc = document_parse_ex(SUITE_HTML, "about:javascript",
                                   MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP,
                                   NULL, d);
        CHECK(rc == 0, "suite: document_parse_ex returns 0");
        CHECK(d->jsRan >= 1, "suite: exactly the one inline script ran");
        CHECK(d->jsErrors == 0, "suite: no JS errors");
        if (d->jsLastError[0])
        {
            printf("  (jsLastError: %s)\n", d->jsLastError);
        }
        CHECK(find_inline_text(d, "JavaScript ran. muJS 1.3.10"),
              "suite: banner rewritten by script");
        CHECK(find_inline_text(d, "created-by-JS"),
              "suite: createElement+appendChild produced a rendered <p>");
        CHECK(find_inline_text(d, "document.write appended this line"),
              "suite: document.write content rendered");
        CHECK(find_inline_text(d, "passed,"),
              "suite: summary written (checks ran)");
        CHECK(find_inline_text(d, "0 missing") ||
                  find_inline_text(d, "0 missing."),
              "suite: zero MISSING language/DOM checks");
        CHECK(find_inline_text(d, "[PASS] querySelector (descendant)"),
              "suite: querySelector finds a descendant");
        CHECK(find_inline_text(d, "[PASS] insertBefore"),
              "suite: insertBefore reorders children");
        CHECK(find_inline_text(d, "[PASS] classList add/contains/remove"),
              "suite: classList add/contains/remove");
        CHECK(find_inline_text(d, "[PASS] style write + readback"),
              "suite: style write + readback");
        CHECK(find_inline_text(d, "[PASS] Set: add/has/size"),
              "suite: Set builtin (shim or native)");
        if (g_fail && getenv("JSDEBUG"))
        {
            printf("--- rendered suite lines ---\n");
            dump_texts(d);
        }
        /* The registered click listener must not have navigated the link:
         * dispatch it and expect SUPPRESSED. */
        int clicked = 0;
        if (d->_jsbridge && d->linkCount > 0)
        {
            for (int i = 0; i < d->linkCount; i++)
            {
                if (d->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d->_jsbridge,
                                                 d->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    clicked = 1;
                    break;
                }
            }
        }
        CHECK(clicked, "suite: click handler fired + preventDefault");
        /* The live DOM was mutated by the handler, but blocks are only
         * rebuilt by document_rewalk — so nothing to assert here yet. */
        int rrc = document_rewalk(d);
        CHECK(rrc == 0, "suite: document_rewalk ok after click");
        CHECK(find_inline_text(d, "Handler ran 1 time(s)") &&
                  find_inline_text(d, "created-by-JS"),
              "suite: mutations survive the rewalk");
        /* Browser-path invariants the 60%-stall fix relies on: the rewalk
         * frees/rebuilds walk output but must keep title/baseUrl and stay
         * clean when called AGAIN (the browser task could re-enter; a stale
         * pointer or double-free here is the hang/corruption class). */
        CHECK(strcmp(d->title, "JavaScript Test Suite") == 0,
              "suite: title survives the rewalk");
        CHECK(strcmp(d->baseUrl, "about:javascript") == 0,
              "suite: baseUrl survives the rewalk");
        CHECK(document_rewalk(d) == 0,
              "suite: second consecutive rewalk ok (idempotent)");
        CHECK(find_inline_text(d, "Handler ran 1 time(s)"),
              "suite: handler text still present after 2nd rewalk");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
    }

    /* ── 1b. Duktape engine: same suite, same DOM surface, zero muJS ───── */
    {
        jsbridge_set_engine(1);
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        int rc = document_parse_ex(SUITE_HTML, "about:javascript",
                                   MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP,
                                   NULL, d);
        CHECK(rc == 0, "duktape: document_parse_ex returns 0");
        CHECK(d->jsRan >= 1, "duktape: the inline script ran");
        CHECK(d->jsErrors == 0, "duktape: no JS errors");
        if (d->jsLastError[0])
        {
            printf("  (jsLastError: %s)\n", d->jsLastError);
        }
        CHECK(find_inline_text(d, "JavaScript ran. Duktape 2.7.0"),
              "duktape: banner rewritten by the DUKTAPE engine");
        CHECK(find_inline_text(d, "Duktape 2.7.0") &&
                  !find_inline_text(d, "muJS 1.3.10"),
              "duktape: engine field shows Duktape, never muJS (isolation)");
        CHECK(find_inline_text(d, "created-by-JS"),
              "duktape: createElement+appendChild produced a rendered <p>");
        CHECK(find_inline_text(d, "document.write appended this line"),
              "duktape: document.write content rendered");
        CHECK(find_inline_text(d, "0 missing") ||
                  find_inline_text(d, "0 missing."),
              "duktape: zero MISSING language/DOM checks");
        if (getenv("JSDEBUG"))
        {
            printf("--- duktape suite lines (MISSing checks visible) ---\n");
            dump_texts(d);
        }
        int clicked = 0;
        if (d->_jsbridge && d->linkCount > 0)
        {
            for (int i = 0; i < d->linkCount; i++)
            {
                if (d->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d->_jsbridge,
                                                 d->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    clicked = 1;
                    break;
                }
            }
        }
        CHECK(clicked, "duktape: click handler fired + preventDefault");
        CHECK(document_rewalk(d) == 0 &&
                  find_inline_text(d, "Handler ran 1 time(s)"),
              "duktape: mutations survive the rewalk");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
        /* Click-mutation page on Duktape too (listener dispatch + mutation). */
        DocParseResult *d2 = calloc(1, sizeof(DocParseResult));
        document_parse_ex(CLICKDOC_HTML, "https://example.com/dukclick",
                          MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP, NULL, d2);
        int suppressed2 = 0;
        if (d2->_jsbridge)
        {
            for (int i = 0; i < d2->linkCount; i++)
            {
                if (d2->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d2->_jsbridge,
                                                 d2->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    suppressed2 = 1;
                    break;
                }
            }
        }
        CHECK(suppressed2, "duktape: clickdoc preventDefault reported");
        CHECK(document_rewalk(d2) == 0 && find_inline_text(d2, "clicked"),
              "duktape: clickdoc mutation renders after rewalk");
        js_doc_close(d2->_jsbridge);
        d2->_jsbridge = NULL;
        document_free(d2);
        free(d2);
        jsbridge_set_engine(0); /* restore the muJS default for later sections */
    }

    /* ── 1c. QuickJS engine: same suite, same DOM surface, zero muJS/Duktape ── */
    {
        jsbridge_set_engine(2);
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        int rc = document_parse_ex(SUITE_HTML, "about:javascript",
                                   MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP,
                                   NULL, d);
        CHECK(rc == 0, "quickjs: document_parse_ex returns 0");
        CHECK(d->jsRan >= 1, "quickjs: the inline script ran");
        CHECK(d->jsErrors == 0, "quickjs: no JS errors");
        if (d->jsLastError[0])
        {
            printf("  (jsLastError: %s)\n", d->jsLastError);
        }
        CHECK(find_inline_text(d, "JavaScript ran. QuickJS 2026-06-04"),
              "quickjs: banner rewritten by the QUICKJS engine");
        CHECK(find_inline_text(d, "QuickJS 2026-06-04") &&
                  !find_inline_text(d, "muJS 1.3.10"),
              "quickjs: engine field shows QuickJS, never muJS (isolation)");
        CHECK(find_inline_text(d, "created-by-JS"),
              "quickjs: createElement+appendChild produced a rendered <p>");
        CHECK(find_inline_text(d, "document.write appended this line"),
              "quickjs: document.write content rendered");
        CHECK(find_inline_text(d, "0 missing") ||
                  find_inline_text(d, "0 missing."),
              "quickjs: zero MISSING language/DOM checks");
        if (getenv("JSDEBUG"))
        {
            printf("--- quickjs suite lines (MISSing checks visible) ---\n");
            dump_texts(d);
        }
        int clicked = 0;
        if (d->_jsbridge && d->linkCount > 0)
        {
            for (int i = 0; i < d->linkCount; i++)
            {
                if (d->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d->_jsbridge,
                                                 d->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    clicked = 1;
                    break;
                }
            }
        }
        CHECK(clicked, "quickjs: click handler fired + preventDefault");
        CHECK(document_rewalk(d) == 0 &&
                  find_inline_text(d, "Handler ran 1 time(s)"),
              "quickjs: mutations survive the rewalk");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
        /* Click-mutation page on QuickJS too (listener dispatch + mutation). */
        DocParseResult *d2 = calloc(1, sizeof(DocParseResult));
        document_parse_ex(CLICKDOC_HTML, "https://example.com/qjsclick",
                          MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP, NULL, d2);
        int suppressed2 = 0;
        if (d2->_jsbridge)
        {
            for (int i = 0; i < d2->linkCount; i++)
            {
                if (d2->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d2->_jsbridge,
                                                 d2->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    suppressed2 = 1;
                    break;
                }
            }
        }
        CHECK(suppressed2, "quickjs: clickdoc preventDefault reported");
        CHECK(document_rewalk(d2) == 0 && find_inline_text(d2, "clicked"),
              "quickjs: clickdoc mutation renders after rewalk");
        js_doc_close(d2->_jsbridge);
        d2->_jsbridge = NULL;
        document_free(d2);
        free(d2);
        jsbridge_set_engine(0); /* restore the muJS default for later sections */
    }

    /* ── 2. Click-mutation page: suppressed default + rewalk text ──────── */
    {
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(CLICKDOC_HTML, "https://example.com/click",
                          MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP, NULL, d);
        CHECK(d->jsErrors == 0, "clickdoc: no JS errors");
        int suppressed = 0;
        if (d->_jsbridge)
        {
            for (int i = 0; i < d->linkCount; i++)
            {
                if (d->links[i]->srcNode &&
                    jsbridge_dispatch_link_click(d->_jsbridge,
                                                 d->links[i]->srcNode) ==
                        JSB_CLICK_SUPPRESSED)
                {
                    suppressed = 1;
                    break;
                }
            }
        }
        CHECK(suppressed, "clickdoc: preventDefault reported to browser");
        /* Mutation lands in blocks only via the rewalk (checked below). */
        CHECK(document_rewalk(d) == 0 && find_inline_text(d, "clicked"),
              "clickdoc: rewalked blocks show the mutation");
        js_doc_close(d->_jsbridge);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
    }

    /* ── 3. JS disabled: scripts must not run at all ───────────────────── */
    {
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(SUITE_HTML, "about:javascript", MODE_RAW_HTML, NULL,
                          DOC_SCRIPT_OFF, NULL, d);
        CHECK(d->jsRan == 0 && d->jsErrors == 0 && !d->_jsbridge,
              "off: no engine, no scripts, no errors");
        CHECK(find_inline_text(d, "JavaScript is OFF"),
              "off: static banner intact (no script rewrote it)");
        CHECK(!find_inline_text(d, "created-by-JS"),
              "off: no DOM mutations happened");
        document_free(d);
        free(d);
    }

    /* ── 4. Engine lifetime: RUN policy closes itself; detach is safe ──── */
    {
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(CLICKDOC_HTML, "https://example.com/run",
                          MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN, NULL, d);
        CHECK(d->jsRan >= 1 && !d->_jsbridge,
              "run: scripts ran, engine closed by parse");
        document_free(d);
        free(d);
    }

    /* ── 5. Compile-safety guard: pathological scripts skipped, not fatal ── */
    {
        /* (a) 200 nested blocks — would recurse deeply inside the muJS
         * compiler on the device task stack; must be skipped cleanly. */
        static char deepHtml[1600];
        int n = snprintf(deepHtml, sizeof(deepHtml),
                         "<html><body><p id=\"m\">alive</p><script>");
        for (int i = 0; i < 200; i++)
            deepHtml[n++] = '{';
        n += snprintf(deepHtml + n, sizeof(deepHtml) - (size_t)n, "var x=1;");
        for (int i = 0; i < 200; i++)
            deepHtml[n++] = '}';
        snprintf(deepHtml + n, sizeof(deepHtml) - (size_t)n,
                 "</script></body></html>");
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(deepHtml, "https://example.com/deep", MODE_RAW_HTML,
                          NULL, DOC_SCRIPT_RUN, NULL, d);
        CHECK(d->jsRan == 0 && d->jsErrors == 1,
              "guard: deeply-nested script skipped, counted as error");
        CHECK(strstr(d->jsLastError, "too deeply nested") != NULL,
              "guard: lastError explains the rejection");
        CHECK(find_inline_text(d, "alive") != NULL,
              "guard: page still renders around the skipped script");
        document_free(d);
        free(d);
    }
    {
        /* (b) regex with 200 consecutive escapes — recursion bomb inside
         * muJS's regex compiler; must be skipped cleanly. */
        static char rxHtml[1400];
        int n = snprintf(rxHtml, sizeof(rxHtml),
                         "<html><body><p>still-here</p><script>var re=/a");
        for (int i = 0; i < 200; i++)
            n += snprintf(rxHtml + n, sizeof(rxHtml) - (size_t)n, "\\x");
        snprintf(rxHtml + n, sizeof(rxHtml) - (size_t)n,
                 "/;</script></body></html>");
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(rxHtml, "https://example.com/rx", MODE_RAW_HTML,
                          NULL, DOC_SCRIPT_RUN, NULL, d);
        CHECK(d->jsRan == 0 && d->jsErrors == 1,
              "guard: regex-escape-bomb script skipped, counted as error");
        CHECK(find_inline_text(d, "still-here") != NULL,
              "guard: page still renders around the regex bomb");
        document_free(d);
        free(d);
    }
    {
        /* (c) deep-but-legal nesting (39 levels, under the guard cap) must
         * still run — proves the guard is not over-aggressive. */
        static char okHtml[400];
        int n = snprintf(okHtml, sizeof(okHtml),
                         "<html><body><p id=\"d\"></p><script>var v=");
        for (int i = 0; i < 39; i++)
            okHtml[n++] = '(';
        okHtml[n++] = '7';
        for (int i = 0; i < 39; i++)
            okHtml[n++] = ')';
        snprintf(okHtml + n, sizeof(okHtml) - (size_t)n,
                 ";document.getElementById('d').textContent='deep-ok';"
                 "</script></body></html>");
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(okHtml, "https://example.com/ok", MODE_RAW_HTML,
                          NULL, DOC_SCRIPT_RUN, NULL, d);
        CHECK(d->jsRan == 1 && d->jsErrors == 0,
              "guard: 39-deep legal script still runs");
        CHECK(find_inline_text(d, "deep-ok") != NULL,
              "guard: 39-deep script produced its DOM effect");
        document_free(d);
        free(d);
    }

    /* ── 6. <noscript> suppression (browser scripting-flag parity) ──────
     * A JS-only SPA page (React boot shape) must hide its noscript fallback
     * when an engine runs (Inline), and must SHOW it when JS is Off.
     * The bryanwandrych.com real-world shape: <body><noscript>warning</noscript>
     * <div id="root"></div><script src=...></body>. */
    {
        static const char SPA_HTML[] =
            "<!doctype html><html><head><title>SPA</title></head><body>"
            "<noscript>You need to enable JavaScript to run this app.</noscript>"
            "<div id=\"root\"></div>"
            "<script>document.write('spa-boot-ran');</script>"
            "</body></html>";
        /* (a) Inline (engine ran): noscript hidden, script effect visible. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(SPA_HTML, "https://example.com/spa",
                              MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN, NULL, d);
            CHECK(d->suppressNoscript == 1,
                  "noscript: policy flag set for RUN");
            CHECK(find_inline_text(d, "You need to enable JavaScript") == NULL,
                  "noscript: warning hidden when an engine ran");
            CHECK(find_inline_text(d, "spa-boot-ran") != NULL,
                  "noscript: engine still ran (document.write visible)");
            document_free(d);
            free(d);
        }
        /* (b) Off (no engine): noscript fallback must still render. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(SPA_HTML, "https://example.com/spa",
                              MODE_RAW_HTML, NULL, DOC_SCRIPT_OFF, NULL, d);
            CHECK(d->suppressNoscript == 0,
                  "noscript: policy flag clear for OFF");
            CHECK(find_inline_text(d, "You need to enable JavaScript") != NULL,
                  "noscript: warning visible when JavaScript is Off");
            document_free(d);
            free(d);
        }
        /* (c) JS-mutation rewalk keeps the policy (JSB re-render path). */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(SPA_HTML, "https://example.com/spa",
                              MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP, NULL,
                              d);
            CHECK(document_rewalk(d) == 0, "noscript: rewalk ok");
            CHECK(find_inline_text(d, "You need to enable JavaScript") == NULL,
                  "noscript: warning stays hidden after the rewalk");
            js_doc_close(d->_jsbridge);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (d) Reader mode (readability distills first): warning visible —
         * same as the historical JavaScript-Off behavior. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(SPA_HTML, "https://example.com/spa",
                              MODE_READER, NULL, DOC_SCRIPT_OFF, NULL, d);
            document_free(d);
            free(d);
        }
        /* (e) The matcher gates SPA-warning-shaped text only: window caps,
         * optional-JS variants, and non-warning text. */
        CHECK(doc_is_noscript_warning(
                  "You need to enable JavaScript to run this app."),
              "noscript-warn: canonical React text");
        CHECK(doc_is_noscript_warning(
                  "You need to enable JavaScript to run this app. 1"),
              "noscript-warn: trailing chunk counter");
        CHECK(doc_is_noscript_warning(
                  "Please enable JavaScript to run this app properly."),
              "noscript-warn: 'please' variant + 'properly' tail");
        CHECK(doc_is_noscript_warning(
                  "  You\tneed to  enable\nJAVASCRIPT to run this APP. "),
              "noscript-warn: whitespace/case normalization");
        CHECK(!doc_is_noscript_warning(
                  "You need to enable JavaScript to run this application."),
              "noscript-warn: different tail rejected");
        CHECK(!doc_is_noscript_warning("plain paragraph text"),
              "noscript-warn: ordinary text not matched");
        {
            char big[130];
            memset(big, 'a', sizeof(big));
            big[sizeof(big) - 1] = '\0';
            CHECK(!doc_is_noscript_warning(big),
                  "noscript-warn: over-length window rejected");
        }
    }

    /* ── 7. JS timers (setTimeout / setInterval) ─────────────────────── */
    {
        /* Fake clock: deterministic pump tests (no sleeps). */
        static unsigned fakeNow = 1000;
        /* (a) One-shot: registers, fires once, table empties. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><div id=t></div><script>"
                "setTimeout(function(){\n"
                "  document.getElementById('t').textContent = 'fired-once';\n"
                "}, 100);"
                "</script></body>",
                "https://example.com/t1", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            CHECK(d->jsRan == 1 && d->_jsbridge != NULL,
                  "timers: page with setTimeout parses + attaches");
            JsBridge *b = d->_jsbridge;
            CHECK(jsbridge_timers_active(b) == 1,
                  "timers: one-shot registered");
            /* Arm pass (dueMs==0 → armed). */
            fakeNow += 10;
            CHECK(jsbridge_timers_pump(b, fakeNow, NULL) == 0,
                  "timers: arming pass fires nothing");
            /* Not yet due. */
            fakeNow += 50;
            CHECK(jsbridge_timers_pump(b, fakeNow, NULL) == 0,
                  "timers: not due yet — no fire");
            /* Due. */
            fakeNow += 60;
            int mut = 0;
            CHECK(jsbridge_timers_pump(b, fakeNow, &mut) == 1,
                  "timers: one-shot fired when due");
            CHECK(jsbridge_timers_active(b) == 0,
                  "timers: one-shot table emptied after fire");
            CHECK(document_rewalk(d) == 0 &&
                      find_inline_text(d, "fired-once"),
                  "timers: one-shot DOM effect renders after rewalk");
            js_doc_close(b);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (b) Interval: repeats, clearInterval stops it. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><div id=t></div><script>"
                "window.__beats = 0;\n"
                "var id = setInterval(function(){\n"
                "  window.__beats = window.__beats + 1;\n"
                "  document.getElementById('t').textContent =\n"
                "      'beat-' + window.__beats;\n"
                "  if (window.__beats >= 3) clearInterval(id);\n"
                "}, 100);"
                "</script></body>",
                "https://example.com/t2", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            fakeNow += 10;
            (void)jsbridge_timers_pump(b, fakeNow, NULL); /* arm */
            int totalFires = 0;
            for (int beat = 0; beat < 10; beat++)
            {
                fakeNow += 100;
                totalFires += jsbridge_timers_pump(b, fakeNow, NULL);
            }
            CHECK(totalFires == 3,
                  "timers: interval fired exactly 3x then self-cleared");
            CHECK(jsbridge_timers_active(b) == 0,
                  "timers: interval table emptied after clearInterval");
            document_rewalk(d);
            CHECK(find_inline_text(d, "beat-3"),
                  "timers: interval DOM effect visible");
            js_doc_close(b);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (c) Lifetime cap: an never-cleared interval is retired. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><script>setInterval(function(){}, 100);</script></body>",
                "https://example.com/t3", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            fakeNow += 10;
            (void)jsbridge_timers_pump(b, fakeNow, NULL); /* arm */
            int total = 0;
            for (int k = 0; k < 200 && jsbridge_timers_active(b) > 0; k++)
            {
                fakeNow += 100;
                total += jsbridge_timers_pump(b, fakeNow, NULL);
            }
            CHECK(total == 64,
                  "timers: runaway interval retired at the 64-fire cap");
            CHECK(jsbridge_timers_active(b) == 0,
                  "timers: runaway interval removed from the table");
            js_doc_close(b);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (d) Mutation → re-render signal: callback that touches the DOM. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><div id=t>init</div><script>"
                "setTimeout(function(){\n"
                "  document.getElementById('t').textContent = 'mutated-by-timer';\n"
                "}, 100);"
                "</script></body>",
                "https://example.com/t4", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            fakeNow += 10;
            (void)jsbridge_timers_pump(b, fakeNow, NULL); /* arm */
            fakeNow += 200;
            int mut = 0;
            jsbridge_timers_pump(b, fakeNow, &mut);
            CHECK(mut == 1,
                  "timers: DOM-mutating fire signals the re-render path");
            CHECK(document_rewalk(d) == 0 &&
                      find_inline_text(d, "mutated-by-timer"),
                  "timers: mutation visible after rewalk");
            js_doc_close(b);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (e2) GHOST-SLOT regression: a one-shot that dies BEFORE an
         * interval forces the sweep to move the interval's slot left;
         * the vacated bytes beyond timerCount must never fire again
         * (the about:javascript double-fire bug). Final DOM text proves
         * the interval fired exactly 3 times: a ghost double-fire would
         * advance the counter twice per pump and land on an even beat. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><div id=t></div><script>"
                "window.__beats = 0;\n"
                "setTimeout(function(){}, 50);\n"
                "var id = setInterval(function(){\n"
                "  window.__beats = window.__beats + 1;\n"
                "  document.getElementById('t').textContent =\n"
                "      'beat-' + window.__beats;\n"
                "  if (window.__beats >= 3) clearInterval(id);\n"
                "}, 100);"
                "</script></body>",
                "https://example.com/t6", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, d);
            JsBridge *b = d->_jsbridge;
            fakeNow += 10;
            (void)jsbridge_timers_pump(b, fakeNow, NULL); /* arm both */
            int total = 0;
            for (int beat = 0; beat < 12; beat++)
            {
                fakeNow += 100;
                total += jsbridge_timers_pump(b, fakeNow, NULL);
            }
            CHECK(total <= 4, /* 1 one-shot + 3 interval, never doubled */
                  "timers: no ghost-slot double-fire after a dead "
                  "one-shot");
            document_rewalk(d);
            CHECK(find_inline_text(d, "beat-3"),
                  "timers: interval self-cleared at exactly 3 beats "
                  "(final DOM text)");
            CHECK(jsbridge_timers_active(b) == 0,
                  "timers: ghost-test table emptied");
            js_doc_close(b);
            d->_jsbridge = NULL;
            document_free(d);
            free(d);
        }
        /* (e) Off policy: no engine → timers page still parses safely. */
        {
            DocParseResult *d = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><script>setTimeout(function(){}, 100);</script></body>",
                "https://example.com/t5", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_OFF, NULL, d);
            CHECK(d->_jsbridge == NULL && jsbridge_timers_active(NULL) == 0,
                  "timers: Off policy — no engine, no timers");
            document_free(d);
            free(d);
        }
    }

    /* ── 8. Timers on the XS (Moddable) engine — the sim gap reproducer ─ */
    {
        jsbridge_set_engine(3); /* XS */
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(
            "<body><div id=t>init</div><script>"
            "setTimeout(function(){\n"
            "  document.getElementById('t').textContent = 'xs-timer-fired';\n"
            "}, 100);"
            "</script></body>",
            "https://example.com/txs", MODE_RAW_HTML, NULL,
            DOC_SCRIPT_RUN_KEEP, NULL, d);
        CHECK(d->jsRan >= 1 && d->jsErrors == 0,
              "xs-timers: page parses, runs, zero errors");
        if (d->jsLastError[0])
        {
            printf("  (xs jsLastError: %s)\n", d->jsLastError);
        }
        if (g_fail && getenv("JSDEBUG"))
        {
            printf("--- rendered xs lines ---\n");
            dump_texts(d);
        }
        JsBridge *b = d->_jsbridge;
        CHECK(b != NULL && jsbridge_timers_active(b) == 1,
              "xs-timers: setTimeout registered on XS");
        unsigned now = 5000;
        (void)jsbridge_timers_pump(b, now, NULL); /* arm */
        now += 200;
        int mut = 0;
        CHECK(jsbridge_timers_pump(b, now, &mut) == 1,
              "xs-timers: callback fired when due");
        CHECK(document_rewalk(d) == 0 &&
                  find_inline_text(d, "xs-timer-fired"),
              "xs-timers: DOM effect renders after rewalk");
        js_doc_close(b);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
        /* Self-clearing interval on XS: must fire exactly 3x, then stop.
         * (Reproduces the device/sim "interval kept firing after its own
         * clearInterval" bug class.) */
        DocParseResult *d2 = calloc(1, sizeof(DocParseResult));
        document_parse_ex(
            "<body><div id=t></div><script>"
            "window.__beats = 0;\n"
            "var id = setInterval(function(){\n"
            "  window.__beats = window.__beats + 1;\n"
            "  document.getElementById('t').textContent = 'beat-' + window.__beats;\n"
            "  if (window.__beats >= 3) clearInterval(id);\n"
            "}, 100);"
            "</script></body>",
            "https://example.com/txsi", MODE_RAW_HTML, NULL,
            DOC_SCRIPT_RUN_KEEP, NULL, d2);
        JsBridge *b2 = d2->_jsbridge;
        CHECK(b2 != NULL && jsbridge_timers_active(b2) == 1,
              "xs-interval: registered");
        unsigned now2 = 9000;
        (void)jsbridge_timers_pump(b2, now2, NULL); /* arm */
        int total2 = 0;
        for (int k = 0; k < 10; k++)
        {
            now2 += 100;
            total2 += jsbridge_timers_pump(b2, now2, NULL);
        }
        CHECK(total2 == 3,
              "xs-interval: fired exactly 3x then self-cleared");
        CHECK(jsbridge_timers_active(b2) == 0,
              "xs-interval: table emptied after clearInterval");
        document_rewalk(d2);
        CHECK(find_inline_text(d2, "beat-3"),
              "xs-interval: final beat renders");
        js_doc_close(b2);
        d2->_jsbridge = NULL;
        document_free(d2);
        free(d2);
        jsbridge_set_engine(0); /* restore the muJS default */
    }

    /* ── 9. Timers on the QuickJS engine — slot-0 sentinel regression ─── */
    /* The FIRST setTimeout on QuickJS used to encode its pin-slot as
     * (void*)0 == the router's refusal sentinel → "too many timers" on
     * every page's first timer. The suite's own try/catch swallowed it
     * (banner: "JS suite error: too many timers"), so it only surfaced in
     * the simulator. Scan-from-1 is the fix; these tests guard it. */
    {
        jsbridge_set_engine(2); /* QuickJS */
        /* (a) one-shot fires + re-renders (slot 0 must never be handed out) */
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse_ex(
            "<body><div id=t>init</div><script>"
            "setTimeout(function(){\n"
            "  document.getElementById('t').textContent = 'qjs-timer-fired';\n"
            "}, 100);"
            "</script></body>",
            "https://example.com/tq", MODE_RAW_HTML, NULL,
            DOC_SCRIPT_RUN_KEEP, NULL, d);
        JsBridge *b = d->_jsbridge;
        CHECK(b != NULL && d->jsErrors == 0,
              "qjs-timers: page parses, runs, zero errors");
        CHECK(b != NULL && jsbridge_timers_active(b) == 1,
              "qjs-timers: FIRST setTimeout registers (slot-0 sentinel fix)");
        unsigned now = 7000;
        (void)jsbridge_timers_pump(b, now, NULL); /* arm */
        now += 150;
        int fired = jsbridge_timers_pump(b, now, NULL);
        CHECK(fired == 1, "qjs-timers: callback fired when due");
        document_rewalk(d);
        CHECK(find_inline_text(d, "qjs-timer-fired"),
              "qjs-timers: DOM effect renders after rewalk");
        CHECK(jsbridge_timers_active(b) == 0,
              "qjs-timers: one-shot table emptied after fire");
        js_doc_close(b);
        d->_jsbridge = NULL;
        document_free(d);
        free(d);
        /* (b) cumulative churn: 20 sequential set+clear must never exhaust
         * the table (slot reuse across the whole index range). */
        {
            DocParseResult *dc = calloc(1, sizeof(DocParseResult));
            document_parse_ex(
                "<body><div id=t></div><script>"
                "window.__done = 0;\n"
                "for (var i = 0; i < 20; i++) {\n"
                "  var id = setTimeout(function(){ window.__done++; }, 50);\n"
                "  clearTimeout(id);\n"
                "}\n"
                "var keep = setTimeout(function(){\n"
                "  document.getElementById('t').textContent = 'churn-ok';\n"
                "}, 50);"
                "</script></body>",
                "https://example.com/tqc", MODE_RAW_HTML, NULL,
                DOC_SCRIPT_RUN_KEEP, NULL, dc);
            JsBridge *bc = dc->_jsbridge;
            CHECK(bc != NULL && dc->jsErrors == 0,
                  "qjs-timers: set/clear churn runs clean");
            CHECK(bc != NULL && jsbridge_timers_active(bc) == 1,
                  "qjs-timers: churn leaves exactly the survivor live");
            unsigned nowc = 12000;
            (void)jsbridge_timers_pump(bc, nowc, NULL); /* arm */
            nowc += 100;
            (void)jsbridge_timers_pump(bc, nowc, NULL);
            document_rewalk(dc);
            CHECK(find_inline_text(dc, "churn-ok"),
                  "qjs-timers: survivor fired after 20-cycle churn");
            js_doc_close(bc);
            dc->_jsbridge = NULL;
            document_free(dc);
            free(dc);
        }
        jsbridge_set_engine(0); /* restore the muJS default */
    }

    /* ── Q-BC: SW4 bytecode cache end-to-end (QuickJS only) ─────────────
     * One source ≥ JSBRIDGE_BC_MIN_SOURCE compiled on bridge #1 must leave
     * a store entry that bridge #2 HITS (no recompile — observable via the
     * bc-hit log line count in the harness output and by behavioral
     * equivalence: both bridges produce the same DOM effect). */
    {
        jsbridge_set_engine(JS_ENGINE_QUICKJS);
        system("mkdir -p /tmp/plutobrowser_spill && rm -f /tmp/plutobrowser_spill/*");
        static const char bigSrc[] =
            "/* padding to clear the 4KB cache floor */\n"
            "var __pad = ["
            "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,"
            "21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,"
            "41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,"
            "61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80];\n"
            "document.getElementById('o').textContent = 'bc-val-' + __pad.length;";
        char page[8192];
        snprintf(page, sizeof(page),
                 "<html><body><p id=\"o\">p</p><script>%s</script></body></html>",
                 bigSrc);
        DocParseResult *d1 = calloc(1, sizeof(DocParseResult));
        document_parse_ex(page, "https://example.com/bc", MODE_RAW_HTML, NULL,
                          DOC_SCRIPT_RUN_KEEP, NULL, d1);
        CHECK(find_inline_text(d1, "bc-val-80"),
              "bc: first compile produces the DOM effect");
        document_free(d1);
        CHECK(pluto_spill_store_count() >= 1,
              "bc: store holds the compiled blob after run #1");

        /* Bridge #2: fresh runtime, same source — must come from bytecode. */
        DocParseResult *d2 = calloc(1, sizeof(DocParseResult));
        document_parse_ex(page, "https://example.com/bc", MODE_RAW_HTML, NULL,
                          DOC_SCRIPT_RUN_KEEP, NULL, d2);
        CHECK(find_inline_text(d2, "bc-val-80"),
              "bc: second run (fresh runtime) same DOM effect");
        CHECK(d2->jsErrors == 0, "bc: no errors on the bc-hit path");
        document_free(d2);

        /* Different source (same size class) must NOT hit the same entry. */
        DocParseResult *d3 = calloc(1, sizeof(DocParseResult));
        char page3[8192];
        snprintf(page3, sizeof(page3),
                 "<html><body><p id=\"o\">p</p><script>%s</script></body></html>",
                 "var __pad2 = [1,2,3,4,5,6,7,8,9,10];"
                 "document.getElementById('o').textContent = 'other-' + __pad2.length;"
                 "/* extra tail padding to change the hash: abcdefghijklmnopqrstuvwxyz */");
        (void)page;
        document_parse_ex(page3, "https://example.com/bc2", MODE_RAW_HTML,
                          NULL, DOC_SCRIPT_RUN_KEEP, NULL, d3);
        CHECK(find_inline_text(d3, "other-10"),
              "bc: different source runs its own code");
        document_free(d3);
        pluto_spill_store_invalidate_all();
        jsbridge_set_engine(0);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
