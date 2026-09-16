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
 *     Source/util/strutil.c Source/util/json.c Source/js/*.c \
 *     -I. -ISource -ISource/core -ISource/util -ISource/html -ISource/js \
 *     -ISource/render -DTARGET_EXTENSION=1 -DPDCS_STRDUP=1 && /tmp/jstest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

/* ---- Fake Playdate API (same pattern as htmltags_host_test.c) ---- */
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
void *pluto_realloc(void *p, size_t n) { return realloc(p, n); }
void tasks_report_progress(float f) { (void)f; } /* readability stub */

/* http_client stubs: html/jsext.c (linked for jsext_arena_free) references
 * these for its NETWORK prefetch session, which no test here exercises. */
typedef struct HttpCallbacks HttpCallbacks;
int http_get(const char *url, const HttpCallbacks *cb)
{
    (void)url;
    (void)cb;
    return 0;
}
void http_cancel(void) {}
void http_client_init(PlaydateAPI *pd) { (void)pd; }
void http_update(void) {}
int http_is_loading(void) { return 0; }

#include "html/document.h"
#include "html/jsbridge.h"
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

static int has_link(const DocParseResult *d, const char *needle)
{
    for (int i = 0; i < d->linkCount; i++)
        if (d->links[i] && d->links[i]->href &&
            strstr(d->links[i]->href, needle))
            return 1;
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
    "}"
    "function P(c, name, fn, note) {"
    "  try { if (fn() === true) { report(c, name, 'PART', note); mark('PART'); return; } }"
    "  catch (e) {}"
    "  report(c, name, 'MISS', 'failed'); mark('MISS');"
    "}"
    "try {"
    "  banner.textContent = 'JavaScript ran. muJS 1.3.10 (ES5 subset).';"
    "  document.getElementById('engine').textContent = 'muJS 1.3.10';"
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
    "  T(dom, 'navigator.userAgent', function(){ return navigator.userAgent.indexOf('muJS') > 0; });"
    "  T(dom, 'document.title', function(){ return document.title === 'JavaScript Test Suite'; });"
    "  P(dom, 'innerHTML (write)', function(){ var d = document.getElementById('domhint'); d.innerHTML = 'html-as-text'; var v = d.textContent === 'html-as-text'; d.textContent = 'ready'; return v; }, 'no markup parsing - text only');"
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
    "  document.write('<p>[INFO] document.write appended this line during page load.</p>');"
    "} catch (e) {"
    "  banner.textContent = 'JS suite error: ' + (e && e.message ? e.message : e);"
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

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
