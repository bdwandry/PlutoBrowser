/*
 * PlutoBrowser — http_client.c
 * Port of Source/core/http_client.lua (reference, 538 lines).
 * Raw-TCP HTTP/1.1 GET engine; see http_client.h for the Lua→C map,
 * preserved semantics, and the C networking API mapping.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/http_client.h"
#include "core/url.h"
#include "core/cookie_jar.h"
#include "core/logger.h"
#include "util/strbuf.h"
#include "util/pdtimer.h"
#include "core/pluto_mem.h"
#include "core/pluto_spill.h"
#include "render/decoders/inflate.h"

/* ── SW2b: disk-backed response body storage ─────────────────────────────
 * The RAW response (headers + body) is written to a spill file as it
 * arrives — disk holds the bulk, RAM holds one bounded read window. The
 * RAM StrBuf keeps ONLY the head up to the header terminator (headers +
 * maybe a few body bytes); from the first post-header byte, everything
 * streams to disk. MAX_RESPONSE_SIZE becomes the RESIDENCY cap for the
 * done-path materialization (a byte-array consumer must fit it in RAM),
 * not a download cap. Byte-array consumers (http_internal_body) opt OUT
 * of spill and keep the historical RAM-growth behavior. */
#define SPILL_NONE 0
#define SPILL_ACTIVE 1
static SpillFile g_spill = PLUTO_SPILL_INVALID;
static int g_spillMode = SPILL_NONE; /* SPILL_ACTIVE once streaming */
static long g_bodySpilled = 0;       /* body bytes written to disk     */

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_mem_realloc((p), 0)

/* ── Constants (verbatim from the reference) ─────────────────────────────── */
#define MAX_RESPONSE_SIZE  2097152 /* 2 MB hard cap to prevent memory growth */
#define REQUEST_TIMEOUT_MS 60000   /* 60 seconds total                       */
#define MAX_REDIRECTS      5
#define READ_CHUNK         16384

/* Read buffer lives in BSS, not on the update-loop stack: the device
 * game-task stack is small, and a 32KB local was the P22-class hazard
 * this file must not repeat. */
static char g_readChunk[READ_CHUNK];
#define SDK_READ_BUFFER    16384   /* Lua setReadBufferSize(16384)           */
#define SDK_TIMEOUT_MS     10000   /* Lua passed 10 (seconds); C takes ms    */

/* SW2b: disk-side runaway cap (no Content-Length + server never closes):
 * bound the spill file so a hostile/hung stream cannot fill flash. */
#define SPILL_MAX_BODY (16 * 1024 * 1024) /* 16MB — far above real pages  */

/* SW2c: gzip delivery ceiling. Compressed staging lives on disk (SW2b spill
 * or the RAM StrBuf); the DECOMPRESSED body is materialized in RAM once at
 * completion, so it must fit the same residency budget as any other body.
 * gzip's expansion factor on real web payloads is ~3×, so this leaves wide
 * margin; a claim beyond it is treated as corrupt (no delivery). */
#define GZIP_DELIVERY_CAP MAX_RESPONSE_SIZE /* 2MB decompressed */

/* ── State ────────────────────────────────────────────────────────────────── */
typedef enum
{
    HS_IDLE = 0,
    HS_CONNECTING,
    HS_ACCESS_WAIT,
    HS_READING,
    HS_DONE,
    HS_ERROR
} HttpState;

static PlaydateAPI *g_pd = NULL;

static TCPConnection *g_tcp = NULL;
static HttpCallbacks g_cb;
static char g_url[1024];
static UrlParsed *g_parsed = NULL; /* heap: UrlParsed is ~1.7KB */

static HttpState g_state = HS_IDLE;
static int g_status = 200;
static StrBuf g_buf;               /* raw response (headers + body) */
static size_t g_bodyStart = 0;     /* 0 = headers not parsed yet    */
static int g_isChunked = 0;
static long g_contentLength = -1;
static int g_isGzip = 0; /* SW2c: Content-Encoding includes gzip */
static int g_gzipHold = 0; /* SW2c: freeze spill — compressed staging in RAM */
static int g_connOpen = 0;         /* open callback fired, connected */
static int g_openFailed = 0;
static int g_connClosed = 0;
static char g_error[256];
static unsigned int g_requestStart = 0;
static unsigned int g_requestId = 0; /* bumped on every reset */
static unsigned int g_accessRequestId = 0; /* generation owning the pending access reply */

/* ── Connection lifecycle (SDK TCP/TLS crash workarounds) ───────────────────
 * Two documented-by-experiment SDK hazards shape this design:
 *   (1) Releasing a connection while its async DNS/connect is still in flight
 *       segfaults the SDK event loop (P33 pool build: the DNS-failed t:80
 *       connection was released in a later host-switch → SEGV).
 *   (2) Closing+releasing a COMPLETED TLS connection and setting up a new TLS
 *       connection shortly after traps the SDK (P33b probe, P33 svg fetch).
 * The TCP docs bless reuse: close() "The connection may be used again for
 * another request". Therefore:
 *   - g_pooledTcp: the most recent connection whose open RESOLVED (success or
 *     open-callback failure). Reused via close()+open() for the same host —
 *     skipping the TLS handshake for same-host image fetches entirely.
 *   - g_orphanTcp: a still-connecting connection handed off at cancel/reset.
 *     Its (stale) open callback closes and releases it — never us.
 *   - g_graveTcp: a pooled connection dropped on host switch — closed now,
 *     RELEASED only after GRAVE_FRAMES frames (http_update tick), long after
 *     the SDK's event loop has drained any pending state for it. */
static TCPConnection *g_pooledTcp = NULL;
static char g_pooledHost[256];
static int g_pooledPort = 0;
static int g_pooledSsl = 0;
static TCPConnection *g_orphanTcp = NULL;  /* open callback owns close+release */
static TCPConnection *g_graveTcp = NULL;   /* closed; release after the delay */
static int g_graveTimer = 0;
#define GRAVE_FRAMES 120  /* ~4s at 30fps */

/* Saved response for the done path (reset() clears live state before the
 * user callback fires — Lua saved locals for the same reason). */
static int g_savedStatus;
static StrBuf g_savedBuf;
static size_t g_savedBodyStart;
static int g_savedIsChunked;
/* Header-line scratch lives in BSS, not on the update-loop stack (P22
 * lesson: the device game-task stack is small). */
static char g_hdrLine[768];

/* Set-Cookie collection buffer — BSS (P22 stack rule). */
static char g_setCookies[16][512];

static char g_savedHeaders[64][2][256]; /* [i][0]=key [i][1]=value */
static int g_savedHeaderCount;

/* Redirects: deferred to a later update tick (reference parity). */
static char g_pendingRedirectUrl[1024];
static HttpCallbacks g_pendingRedirectCb;
static int g_hasPendingRedirect = 0;
static int g_redirectDepth = 0;

static int g_writePending = 0; /* NET_WRITE_BUSY retry in flight */

/* ── Internal about: pages (verbatim from the reference) ─────────────────── */
typedef struct
{
    const char *name;
    const char *title;
    const char *html;
} InternalPage;

static const char ACIDTEST_HTML[] =
    "<html><head><title>HTML Renderer Test Suite</title></head><body>\n"
    "\n"
    "<h1>HTML Renderer Test</h1>\n"
    "<p>Every block &amp; inline element the renderer understands, in one page. Some <b>bold</b>, <i>italic</i>, <u>underlined</u>, <s>struck</s>, <code>code</code>, <mark>marked</mark>, <small>small</small>, <big>big</big> and <sub>sub</sub>/<sup>sup</sup> text, plus an <a href=\"https://example.com\">example link</a> and a <q>short quote</q>.</p>\n"
    "\n"
    "<h2>Headings &amp; Alignment</h2>\n"
    "<h3>Left</h3>\n"
    "<div align=\"center\"><p>This paragraph is centered via align.</p></div>\n"
    "<p style=\"text-align:right\">This paragraph is right-aligned via inline style.</p>\n"
    "\n"
    "<h2>Lists</h2>\n"
    "<ul><li>Unordered item one</li><li>Item two with a nested list:<ul><li>Nested item A</li><li>Nested item B</li></ul></li><li>Item three</li></ul>\n"
    "<ol><li>First ordered</li><li>Second ordered</li><li>Third ordered</li></ol>\n"
    "<dl><dt>Definition term</dt><dd>Definition description that runs on for a bit so we can see wrapping work.</dd><dt>Another term</dt><dd>Another description.</dd></dl>\n"
    "\n"
    "<h2>Quotes &amp; Code</h2>\n"
    "<blockquote>This is a block quotation with a left rail, the way desktop browsers draw them.</blockquote>\n"
    "<pre>function hello()\n"
    "  print(\"Hello, Playdate\")\n"
    "end</pre>\n"
    "\n"
    "<h2>Tables</h2>\n"
    "<table>\n"
    "<caption>Sample Caption</caption>\n"
    "<thead><tr><th>Name</th><th>Score</th><th>Level</th></tr></thead>\n"
    "<tbody>\n"
    "<tr><td>Bryan</td><td align=\"right\">98</td><td>5</td></tr>\n"
    "<tr><td>Comet</td><td align=\"right\">87</td><td>4</td></tr>\n"
    "</tbody>\n"
    "</table>\n"
    "<p>Percent-width table (like news.ycombinator.com):</p>\n"
    "<table width=\"85%\">\n"
    "<tr><td>Cell A1</td><td>Cell A2</td><td>Cell A3</td></tr>\n"
    "<tr><td>Cell B1</td><td>Cell B2</td><td>Cell B3</td></tr>\n"
    "</table>\n"
    "<table width=\"60%\" align=\"center\">\n"
    "<tr><td>60% centered table</td><td>second cell</td></tr>\n"
    "</table>\n"
    "\n"
    "<h2>Figures</h2>\n"
    "<figure><img src=\"https://example.com/test.png\" width=\"160\" height=\"80\" alt=\"Alt text placeholder\"><figcaption>A figure with a caption</figcaption></figure>\n"
    "\n"
    "<h2>Forms</h2>\n"
    "<form action=\"https://example.com/search\" method=\"get\">\n"
    "<label>Search:</label> <input type=\"text\" name=\"q\" placeholder=\"type here\">\n"
    "<input type=\"submit\" value=\"Search\">\n"
    "<fieldset><legend>Preferences</legend>\n"
    "<input type=\"checkbox\" name=\"opt1\" checked> Option one (checked)<br>\n"
    "<input type=\"checkbox\" name=\"opt2\"> Option two<br>\n"
    "<input type=\"radio\" name=\"grp\" value=\"a\" checked> Radio A\n"
    "<input type=\"radio\" name=\"grp\" value=\"b\"> Radio B\n"
    "<select name=\"color\"><option selected>Red</option><option>Green</option><option>Blue</option></select>\n"
    "</fieldset>\n"
    "<textarea name=\"msg\" rows=\"2\">Hello textarea</textarea>\n"
    "</form>\n"
    "\n"
    "<h2>Boxes</h2>\n"
    "<details><summary>Clickable summary line</summary><p>Hidden-until-open body content is shown inline on Playdate.</p></details>\n"
    "<dialog open><p>A dialog box with an open attribute.</p></dialog>\n"
    "\n"
    "<h2>Media &amp; Meters</h2>\n"
    "<p>Progress: <progress value=\"70\" max=\"100\"></progress>  Meter: <meter value=\"0.6\" max=\"1\"></meter></p>\n"
    "<video controls width=\"300\" height=\"120\"><source src=\"movie.mp4\"></video>\n"
    "<iframe width=\"200\" height=\"100\"></iframe>\n"
    "\n"
    "<h2>Horizontal Rule &amp; Misc</h2>\n"
    "<hr>\n"
    "<p>Entities: &amp; &lt; &gt; &quot; &apos; &nbsp; &copy; &mdash; &hellip; 5 &lt; 6 &amp; 4 = 9</p>\n"
    "<p>Unicode fallbacks: &Auml; &ouml; &eacute; &nbsp;</p>\n"
    "\n"
    "<h2>Semantic Containers</h2>\n"
    "<article><h3>Article</h3><p>Content inside an article element.</p></article>\n"
    "<section><h3>Section</h3><p>Content inside a section element.</p></section>\n"
    "<aside>Aside note: off-topic content.</aside>\n"
    "<nav>Nav: <a href=\"https://example.com/1\">one</a> <a href=\"https://example.com/2\">two</a></nav>\n"
    "<footer>Footer line</footer>\n"
    "<address>Address: 1 Infinity Loop, Cupertino</address>\n"
    "<hgroup><h3>Hgroup Heading</h3><p>Grouped subtitle paragraph.</p></hgroup>\n"
    "\n"
    "<h2>Text Semantics</h2>\n"
    "<p>Abbreviation: <abbr title=\"HyperText Markup Language\">HTML</abbr> rocks.</p>\n"
    "<p>Strong vs bold: <strong>strong importance</strong> and <b>bold text</b>.</p>\n"
    "<p>Emphasis: <em>emphasized</em>, <dfn>defined term</dfn>, <var>variable</var>, <cite>citation</cite>.</p>\n"
    "<p>Insertions and deletions: <ins>inserted text</ins> and <del datetime=\"2026-09-01\">deleted text</del>.</p>\n"
    "<p>Key input: <kbd>Ctrl</kbd> + <kbd>S</kbd>. Sample output: <samp>404 Not Found</samp>.</p>\n"
    "<p>Time: <time datetime=\"2026-09-12T10:00\">Sept 12, 10:00</time>. Data: <data value=\"42\">forty-two</data>.</p>\n"
    "<p>Ruby: <ruby>kan<rt>annotation</rt></ruby> and <wbr>word break opportunities here.</p>\n"
    "\n"
    "<h2>Select with Optgroup</h2>\n"
    "<form action=\"https://example.com/pick\" method=\"get\">\n"
    "<select name=\"car\">\n"
    "<optgroup label=\"Swedish Cars\"><option>Volvo</option><option>Saab</option></optgroup>\n"
    "<optgroup label=\"German Cars\"><option selected>Mercedes</option><option>Audi</option></optgroup>\n"
    "</select>\n"
    "<input type=\"submit\" value=\"Pick\">\n"
    "</form>\n"
    "\n"
    "<h2>Ordered List Variants</h2>\n"
    "<ol type=\"a\"><li>Lower alpha</li><li>Second</li></ol>\n"
    "<ol type=\"I\" start=\"10\"><li>Roman from ten</li><li>Eleven</li></ol>\n"
    "<ol reversed start=\"3\"><li>Reversed three</li><li>Reversed two</li></ol>\n"
    "<menu><li>Menu item</li><li>Another menu item</li></menu>\n"
    "\n"
    "</body></html>";

/* about:javascript — exercises the real muJS 1.3.10 engine and the real
 * DOM bridge (document, elements, mutation, events). Every check runs
 * actual code; nothing is hard-coded to pass. With JavaScript disabled in
 * Settings the static banner text is what remains visible. */
static const char JSTEST_HTML[] =
    "<html><head><title>JavaScript Test Suite</title></head><body>\n"
    "<h1>JavaScript Test Suite</h1>\n"
    "<p id=\"banner\">JavaScript is OFF. Enable it in Settings.</p>\n"
    "<p>Engine: <span id=\"engine\">not detected</span>. <span id=\"summary\"></span></p>\n"
    "<h2>Language</h2>\n"
    "<div id=\"langout\">[..] running</div>\n"
    "<h2>DOM</h2>\n"
    "<div id=\"domout\">[..] running</div>\n"
    "<p id=\"domhint\">ready</p>\n"
    "<div id=\"sw5out\">[..] SW5 running</div>\n"
    "<ul id=\"demoList\"><li>one</li><li>two</li><li>three</li></ul>\n"
    "<style>\n"
    ".css-hidden { display: none }\n"
    "h2.css-center { text-align: center }\n"
    ".css-bold { font-weight: bold }\n"
    ".css-invert { background-color: black; color: white }\n"
    "</style>\n"
    "<h2 class=\"css-center\">CSS demo</h2>\n"
    "<div class=\"css-hidden\" id=\"css-hidden-probe\">[HIDDEN-BY-CSS this line must never render]</div>\n"
    "<p class=\"css-bold\">CSS bold text (this line renders bold)</p>\n"
    "<p class=\"css-invert\" id=\"css-invert-probe\">CSS inverted block (renders inverted)</p>\n"
    "<div id=\"cssout\">[..] css checks running</div>\n"
    "<h2>XHR demo</h2>\n"
    "<p id=\"xhrout\">[..] XHR not fired yet</p>\n"
    "<p id=\"fetchout\">[..] fetch not fired yet</p>\n"
    "<h2>Event demo</h2>\n"
    "<p><a href=\"https://example.com/blocked\" id=\"clickme\">Click me</a> - clicks: <b id=\"clickcount\">0</b></p>\n"
    "<p id=\"clickresult\">Handler has not fired yet.</p>\n"
    "<h2>Timer demo</h2>\n"
    "<p id=\"timerout\">[..] timer not fired yet</p>\n"
    "<p id=\"oneshot\">[..] one-shot not fired yet</p>\n"
    "<h2>Known unavailable</h2>\n"
    "<p>localStorage, CSS via JS, and ES6+ syntax\n"
    "(let/const/arrow functions) are not supported on the ES5 engines.\n"
    "document/window-level listeners are accepted but not dispatched;\n"
    "per-element click listeners work. Timers (setTimeout/setInterval) fire\n"
    "between frames with per-page caps. innerHTML degrades to text-only.\n"
    "XMLHttpRequest (GET) and QuickJS fetch() are supported async.\n"
    "</p>\n"
    "<script>\n"
    "var banner = document.getElementById('banner');\n"
    "var pass = 0, part = 0, miss = 0;\n"
    "function report(c, name, st, note) {\n"
    "  var p = document.createElement('p');\n"
    "  var t = '[' + st + '] ' + name;\n"
    "  if (note) t = t + ' - ' + note;\n"
    "  p.textContent = t;\n"
    "  c.appendChild(p);\n"
    "}\n"
    "function mark(st) { if (st === 'PASS') pass++; else if (st === 'PART') part++; else miss++; }\n"
    "function T(c, name, fn) {\n"
    "  try {\n"
    "    var r = fn();\n"
    "    if (r === true) { report(c, name, 'PASS'); mark('PASS'); }\n"
    "    else if (r === false) { report(c, name, 'MISS'); mark('MISS'); }\n"
    "    else { report(c, name, 'PART', '' + r); mark('PART'); }\n"
    "  } catch (e) { report(c, name, 'MISS', 'threw'); mark('MISS'); }\n"
    "  if (typeof console !== 'undefined' && console.log) console.log('[sw-t] ' + name + ' → ' + r);\n"
    "}\n"
    "function P(c, name, fn, note) {\n"
    "  try { if (fn() === true) { report(c, name, 'PART', note); mark('PART'); return; } }\n"
    "  catch (e) {}\n"
    "  report(c, name, 'MISS', 'failed'); mark('MISS');\n"
    "}\n"
    "try {\n"
    "  var ua = navigator.userAgent;\n"
    "  var isDuk = ua.indexOf('Duktape') >= 0;\n"
    "  var isQjs = ua.indexOf('QuickJS') >= 0;\n"
    "  var isXs = ua.indexOf('XS/Moddable') >= 0;\n"
    "  banner.textContent = 'JavaScript ran. ' + (isDuk ? 'Duktape 2.7.0 (ES5.1).' : isQjs ? 'QuickJS 2026-06-04 (ES2023).' : isXs ? 'XS 9.5.0 (ES2023, Moddable).' : 'muJS 1.3.10 (ES5 subset).');\n"
    "  document.getElementById('engine').textContent = isDuk ? 'Duktape 2.7.0' : isQjs ? 'QuickJS 2026-06-04' : isXs ? 'XS 9.5.0' : 'muJS 1.3.10';\n"
    "  var lang = document.getElementById('langout');\n"
    "  var dom = document.getElementById('domout');\n"
    "  var timers = document.getElementById('timerout');\n"
    "  var cssbox = document.getElementById('cssout');\n"
    "  var xhrbox = document.getElementById('xhrout');\n"
    "  T(lang, 'variables + arithmetic', function(){ var a = 6*7; return a === 42; });\n"
    "  T(lang, 'strings + concatenation', function(){ return 'foo' + 1 + true === 'foo1true'; });\n"
    "  T(lang, 'typeof', function(){ return typeof 1 === 'number' && typeof 'x' === 'string' && typeof undefined === 'undefined' && typeof null === 'object'; });\n"
    "  T(lang, 'comparison + ternary', function(){ return (1 < 2 ? 'y' : 'n') === 'y' && (3 >= 4 ? 'y' : 'n') === 'n'; });\n"
    "  T(lang, 'conditionals (if/else)', function(){ var v = 0; if (1) { v = 1; } else { v = 2; } return v === 1; });\n"
    "  T(lang, 'loops (for/while/break/continue)', function(){ var s = 0; for (var i = 1; i <= 10; i++) { if (i === 4) continue; s += i; } while (s > 1000) { s = 0; break; } return s === 51; });\n"
    "  T(lang, 'functions + closures', function(){ function adder(n) { return function(x) { return x + n; }; } return adder(40)(2) === 42; });\n"
    "  T(lang, 'recursion', function(){ function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } return fib(10) === 55; });\n"
    "  T(lang, 'arrays + methods', function(){ var a = [3,1,2]; a.push(5); a.sort(); return a.join(',') === '1,2,3,5' && a.indexOf(2) === 1 && a.length === 4; });\n"
    "  T(lang, 'object literals + properties', function(){ var o = { x: 1, 'y': 2 }; o.z = 3; return o.x + o.y + o['z'] === 6; });\n"
    "  T(lang, 'for-in over object', function(){ var o = {a:1,b:2}, k = 0, n = 0; for (var p in o) { k += o[p]; n++; } return k === 3 && n === 2; });\n"
    "  T(lang, 'try/catch/throw', function(){ try { throw new Error('x'); } catch (e) { return e.message === 'x'; } return false; });\n"
    "  T(lang, 'JSON parse/stringify', function(){ var o = JSON.parse('{\"a\":[1,2]}'); return o.a[1] === 2 && JSON.stringify(o) === '{\"a\":[1,2]}'; });\n"
    "  T(lang, 'RegExp exec', function(){ var m = /(a+)(b+)/.exec('xaabb'); return !!m && m[0] === 'aabb' && m[1] === 'aa' && m[2] === 'bb'; });\n"
    "  T(lang, 'string methods', function(){ return 'abc'.charAt(1) === 'b' && 'abc'.toUpperCase() === 'ABC' && 'a-b-c'.split('-').length === 3 && 'abc'.substring(1) === 'bc'; });\n"
    "  T(lang, 'number methods + parsing', function(){ return (3.14159).toFixed(2) === '3.14' && parseInt('42px', 10) === 42 && parseFloat('2.5') === 2.5; });\n"
    "  T(lang, 'Math object', function(){ return Math.floor(2.7) === 2 && Math.max(1, 9) === 9 && Math.abs(-3) === 3; });\n"
    "  T(lang, 'type conversions', function(){ return Number('42') === 42 && String(42) === '42' && (1 == '1') && !!'x'; });\n"
    "  T(lang, 'Array.isArray (ES5)', function(){ return typeof Array.isArray === 'function' && Array.isArray([1]) && !Array.isArray({}); });\n"
    "  T(lang, 'Object.keys (ES5)', function(){ return typeof Object.keys === 'function' && Object.keys({a:1,b:2}).join('') === 'ab'; });\n"
    "  T(lang, 'array forEach/map (ES5)', function(){ if (typeof [].map !== 'function') return false; var s = 0; [1,2,3].forEach(function(v){ s += v; }); return s === 6 && [1,2,3].map(function(v){ return v*2; }).join(',') === '2,4,6'; });\n"
    "  T(lang, 'string trim (ES5)', function(){ return typeof 'x'.trim === 'function' && '  x '.trim() === 'x'; });\n"
    "  T(lang, 'Date object', function(){ return typeof Date === 'function' && typeof (new Date()).getTime() === 'number'; });\n"
    "  T(dom, 'document.getElementById', function(){ return document.getElementById('demoList').tagName === 'UL'; });\n"
    "  T(dom, 'textContent read', function(){ return document.getElementById('clickcount').textContent === '0'; });\n"
    "  T(dom, 'textContent write + readback', function(){ var s = document.getElementById('domhint'); s.textContent = 'mutated'; var v = s.textContent; s.textContent = 'ready'; return v === 'mutated'; });\n"
    "  T(dom, 'setAttribute/getAttribute', function(){ var d = document.getElementById('domout'); d.setAttribute('data-test', 'ok1'); var v = d.getAttribute('data-test'); d.setAttribute('data-test', 'ok2'); return v === 'ok1' && d.getAttribute('data-test') === 'ok2'; });\n"
    "  T(dom, 'createElement + appendChild', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); p.textContent = 'created-by-JS'; d.appendChild(p); var k = d.children; return k[k.length-1].textContent === 'created-by-JS'; });\n"
    "  T(dom, 'createTextNode + appendChild', function(){ var d = document.getElementById('domout'); var t = document.createTextNode('text-node-ok'); d.appendChild(t); return d.textContent.indexOf('text-node-ok') >= 0; });\n"
    "  T(dom, 'removeChild', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); d.appendChild(p); var n = d.childElementCount; d.removeChild(p); return d.childElementCount === n - 1; });\n"
    "  T(dom, 'getElementsByTagName (element)', function(){ return document.getElementById('demoList').getElementsByTagName('li').length === 3; });\n"
    "  T(dom, 'parentNode', function(){ var li = document.getElementById('demoList').children[0]; return li.parentNode.tagName === 'UL'; });\n"
    "  T(dom, 'childElementCount/children', function(){ return document.getElementById('demoList').childElementCount === 3; });\n"
    "  T(dom, 'location.href (read)', function(){ return typeof location.href === 'string' && location.href.length > 0; });\n"
    "  T(dom, 'navigator.userAgent', function(){ return navigator.userAgent.indexOf('muJS') > 0 || navigator.userAgent.indexOf('Duktape') > 0 || navigator.userAgent.indexOf('QuickJS') > 0 || navigator.userAgent.indexOf('XS/Moddable') > 0; });\n"
    "  T(dom, 'document.title', function(){ return document.title === 'JavaScript Test Suite'; });\n"
    "  T(timers, 'setTimeout returns an id', function(){ var id = setTimeout(function(){}, 50); clearTimeout(id); return typeof id === 'number' && id > 0; });\n"
    "  T(timers, 'clearTimeout(null-ish) is safe', function(){ clearTimeout(0); clearTimeout(undefined); return true; });\n"
    "  T(cssbox, 'CSS: styled elements present in DOM', function(){\n"
    "    var hid = document.getElementById('css-hidden-probe');\n"
    "    var inv = document.getElementById('css-invert-probe');\n"
    "    if (!hid || !inv) return false;\n"
    "    return typeof hid.textContent === 'string' && inv.textContent.indexOf('inverted') >= 0; });\n"
    "  setTimeout(function(){\n"
    "    var out = document.getElementById('oneshot');\n"
    "    if (out) out.textContent = 'timer-fired-ok (one-shot, 50ms)';\n"
    "  }, 50);\n"
    "  var beats = 0;\n"
    "  var beatId = setInterval(function(){\n"
    "    beats++;\n"
    "    var out = document.getElementById('timerout');\n"
    "    if (out) out.textContent = 'interval beat ' + beats;\n"
    "    if (beats >= 5) clearInterval(beatId);\n"
    "  }, 400);\n"
    "  P(dom, 'innerHTML (write)', function(){ var d = document.getElementById('domhint'); d.innerHTML = 'html-as-text'; var v = d.textContent === 'html-as-text'; d.textContent = 'ready'; return v; }, 'no markup parsing - text only');\n"
    "  var sw5 = document.getElementById('sw5out');\n"
    "  T(sw5, 'querySelector (descendant)', function(){ var p = document.querySelector('#demoList li'); return !!p && p.tagName === 'LI'; });\n"
    "  T(sw5, 'querySelector (id selector)', function(){ return document.querySelector('#domhint').id === 'domhint'; });\n"
    "  T(sw5, 'querySelector no-match returns null', function(){ return document.querySelector('#nope-xyz') === null; });\n"
    "  T(sw5, 'querySelectorAll returns all matches', function(){ var a = document.querySelectorAll('#demoList li'); return a.length === 3; });\n"
    "  T(sw5, 'querySelectorAll scoped to element', function(){ var d = document.getElementById('domout'); var p = document.createElement('p'); d.appendChild(p); var a = d.querySelectorAll('p'); var ok = a.length >= 1; d.removeChild(p); return ok; });\n"
    "  T(sw5, 'insertBefore inserts before first child', function(){ var ul = document.getElementById('demoList'); var li = document.createElement('li'); li.textContent = 'zero'; ul.insertBefore(li, ul.children[0]); var ok = ul.children[0].textContent === 'zero' && ul.childElementCount === 4; ul.removeChild(li); return ok; });\n"
    "  T(sw5, 'firstElementChild/nextElementSibling', function(){ var one = document.getElementById('demoList').firstElementChild; return !!one && one.textContent === 'one' && one.nextElementSibling.textContent === 'two'; });\n"
    "  T(sw5, 'classList add/contains/remove', function(){ var d = document.getElementById('domout'); d.classList.add('a1'); d.classList.add('a1'); var ok1 = d.classList.contains('a1') && d.classList.length === 1; d.classList.remove('a1'); return ok1 && !d.classList.contains('a1'); });\n"
    "  T(sw5, 'classList toggle', function(){ var d = document.getElementById('domout'); var on = d.classList.toggle('tg'); var off = d.classList.toggle('tg'); return on === true && off === false && !d.classList.contains('tg'); });\n"
    "  T(sw5, 'classList item', function(){ var d = document.getElementById('domout'); d.classList.add('x1'); d.classList.add('x2'); var it = d.classList.item(1); var ok = it === 'x2'; d.classList.remove('x1'); d.classList.remove('x2'); return ok; });\n"
    "  T(sw5, 'style write + readback', function(){ var d = document.getElementById('domhint'); d.style.display = 'none'; var v = d.style.display; d.style.display = ''; return v === 'none' && d.style.display === ''; });\n"
    "  T(sw5, 'style preserves other properties', function(){ var d = document.getElementById('domhint'); d.style.color = 'red'; d.style.display = 'none'; var ok = d.style.color === 'red' && d.style.display === 'none'; d.style.color = ''; d.style.display = ''; return ok; });\n"
    "  T(sw5, 'Set: add/has/size', function(){ var s = new Set(); s.add(1); s.add(1); s.add('x'); return s.has(1) && s.has('x') && !s.has(2) && s.size === 2; });\n"
    "  T(sw5, 'Map: set/get/has/delete', function(){ var m = new Map(); m.set('k', 42); var v = m.get('k'); var gone = m['delete']('k'); return v === 42 && gone === true && m.has('k') === false && m.get('k') === undefined; });\n"
    "  T(sw5, 'Image: construct', function(){ var img = new Image(); img.src = 'x.png'; return img != null; });\n"
    "  T(dom, 'XMLHttpRequest exists', function(){ return typeof XMLHttpRequest === 'function' || typeof XMLHttpRequest === 'object'; });\n"
    "  if (typeof XMLHttpRequest !== 'undefined') {\n"
    "    var x = new XMLHttpRequest();\n"
    "    x.open('GET', 'about:jsext');\n"
    "    x.onload = function(t) {\n"
    "      var el = document.getElementById('xhrout');\n"
    "      if (el) el.textContent = 'XHR ok: ' + x.status + ', ' + x.responseText.length + ' bytes, body[0..15]=' + String(x.responseText).substring(0, 15);\n"
    "    };\n"
    "    x.onerror = function(t) {\n"
    "      var el = document.getElementById('xhrout');\n"
    "      if (el) el.textContent = 'XHR failed';\n"
    "    };\n"
    "    x.send();\n"
    "    P(dom, 'XMLHttpRequest send accepted', function(){ return true; }, 'async - result appears in the XHR demo section');\n"
    "  }\n"
    "  if (typeof fetch === 'function') {\n"
    "    fetch('about:javascript').then(function(body) {\n"
    "      var el = document.getElementById('fetchout');\n"
    "      if (el) el.textContent = 'fetch ok: ' + body.length + ' bytes';\n"
    "    }, function(err) {\n"
    "      var el = document.getElementById('fetchout');\n"
    "      if (el) el.textContent = 'fetch failed: ' + err;\n"
    "    });\n"
    "    P(dom, 'fetch() accepted', function(){ return true; }, 'async - result appears in the XHR demo section');\n"
    "  }\n"
    "  var link = document.getElementById('clickme');\n"
    "  if (link && typeof link.addEventListener === 'function') {\n"
    "    var clicks = 0;\n"
    "    link.addEventListener('click', function(e) {\n"
    "      clicks = clicks + 1;\n"
    "      document.getElementById('clickcount').textContent = '' + clicks;\n"
    "      document.getElementById('clickresult').textContent = 'Handler ran ' + clicks + ' time(s); navigation prevented by preventDefault().';\n"
    "      e.preventDefault();\n"
    "    });\n"
    "    P(dom, 'element click listener', function(){ return true; }, 'registered - click the link above to fire it');\n"
    "  }\n"
    "  document.getElementById('summary').textContent = pass + ' passed, ' + part + ' partial, ' + miss + ' missing.';\n"
    "  console.log('[sw-suite] ' + pass + ' passed, ' + part + ' partial, ' + miss + ' missing');\n"
    "  document.write('<p>[INFO] document.write appended this line during page load.</p>');\n"
    "} catch (e) {\n"
    "  var emsg = (e && e.message ? e.message : String(e));\n"
    "  banner.textContent = 'JS suite error: ' + emsg;\n"
    "  console.log('suite error: ' + emsg + ' [Set=' + typeof Set + ' Map=' + typeof Map + ' Image=' + typeof Image + ' XHR=' + typeof XMLHttpRequest + ']');\n"
    "}\n"
    "</script>\n"
    "</body></html>";

/* ── about:jsext — Full-mode external <script src> test suite ─────────────
 * Deterministic tests 6–11 (ordering + shared engine, fail-skip, dedupe,
 * per-script cap, page budget, document.write wiring). The "external"
 * files are served locally by html/jsext's built-in table (matched by
 * file-name tail), so the suite runs identically on simulator and device
 * with zero network dependence. In Off/Inline modes this page renders
 * fine with no report rows (nothing external runs) — mode-regression
 * comes from the settings autotest seam instead. */
static const char JSEXTTEST_HTML[] =
    "<!DOCTYPE html><html><head><title>Full JS Test Suite</title></head><body>\n"
    "<h1>External Script (Full) Suite</h1>\n"
    "<p id=\"banner\">running…</p>\n"
    "<div id=\"out\"></div>\n"
    /* 6: defines window.__extOrder — consumed by the final inline report. */
    "<script src=\"jsext-order.js\"></script>\n"
    /* 8: NOT in the local table → download miss → skipped, later scripts run. */
    "<script src=\"jsext-missing.js\"></script>\n"
    /* 9 (first of two): bumps window.__extCount; logged/fetched once. */
    "<script src=\"jsext-dup.js\"></script>\n"
    "<script>window.__inlineA = (window.__extCount === 1) ? 'A_OK' : 'A_BAD';</script>\n"
    /* 9 (second occurrence): executes again — window.__extCount reads 2. */
    "<script src=\"jsext-dup.js\"></script>\n"
    /* 10: 70KB > 64KB per-script cap → refused BEFORE execution. */
    "<script src=\"jsext-huge.js\"></script>\n"
    /* SW2d: data:-URL script (RFC 2397) — payload decodes + executes like
     * an inline body, no fetch. base64 payload = window.__dataRan=1; */
    "<script src=\"data:application/javascript;base64,d2luZG93Ll9fZGF0YVJhbj0xOw==\"></script>\n"
    /* 11a–11h: ~16KB each, cap-legal and engine-runnable; together with the
     * tiny scripts they consume ~114KB of the 160KB page budget. */
    "<script src=\"jsext-big.js\"></script>\n"
    "<script src=\"jsext-big2.js\"></script>\n"
    "<script src=\"jsext-big3.js\"></script>\n"
    "<script src=\"jsext-big4.js\"></script>\n"
    "<script src=\"jsext-big5.js\"></script>\n"
    "<script src=\"jsext-big6.js\"></script>\n"
    "<script src=\"jsext-big7.js\"></script>\n"
    "<script src=\"jsext-big8.js\"></script>\n"
    /* 11i: 56KB > the remaining ~46KB budget → refused. */
    "<script src=\"jsext-big9.js\"></script>\n"
    /* 7: document.write from an EXTERNAL script, wired like an inline one. */
    "<script src=\"jsext-write.js\"></script>\n"
    "<script>\n"
    "var out = document.getElementById('out');\n"
    "var pass = 0, fail = 0;\n"
    "function T(name, fn) {\n"
    "  var r;\n"
    "  try { r = fn(); } catch (e) { r = 'threw: ' + e; }\n"
    "  if (r === true) { pass = pass + 1; } else { fail = fail + 1; }\n"
    "  console.log('[jsext-t] [' + (r === true ? 'PASS' : 'FAIL') + '] ' + name + (r === true ? '' : ' :: ' + r));\n"
    "  var p = document.createElement('p');\n"
    "  p.textContent = '[' + (r === true ? 'PASS' : 'FAIL') + '] ' + name +\n"
    "    (r === true ? '' : ' :: ' + r);\n"
    "  out.appendChild(p);\n"
    "}\n"
    "try {\n"
    "  T('external defined global (order+engine)', function(){\n"
    "    return window.__extOrder === 'EXT_OK'; });\n"
    "  T('inline-after-external saw ext state', function(){\n"
    "    return window.__inlineA === 'A_OK'; });\n"
    "  T('dup: downloaded once, executed twice', function(){\n"
    "    return window.__extCount === 2; });\n"
    "  T('over-cap file executed (disk-resident)', function(){\n"
    "    return window.__huge === 1; });\n"
    "  T('over-budget file refused (no execution)', function(){\n"
    "    return typeof window.__big3 === 'undefined'; });\n"
    "  T('big files did not break the engine', function(){\n"
    "    return typeof window.__extOrder === 'string'; });\n"
    "  T('72KB file ran from disk (SW2b)', function(){\n"
    "    return window.__huge === 1; });\n"
    "  T('data:-URL script executed (SW2d)', function(){\n"
    "    return window.__dataRan === 1; });\n"
    "  var b = document.getElementById('banner');\n"
    "  b.textContent = pass + ' passed, ' + fail + ' failed.';\n"
    "  console.log('[jsext-test] summary: ' + pass + ' passed, ' + fail + ' failed');\n"
    "} catch (e) {\n"
    "  document.getElementById('banner').textContent = 'Suite error: ' + e;\n"
    "}\n"
    "</script>\n"
    "</body></html>";

static const InternalPage INTERNAL_PAGES[] = {
    { "about:home", "PlutoBrowser",
      "<html><head><title>PlutoBrowser</title></head><body><h1>PlutoBrowser</h1><p>Ready.</p></body></html>" },
    { "about:blank", "Blank",
      "<html><body></body></html>" },
    { "about:acidtest", "HTML Acid Test", ACIDTEST_HTML },
    { "about:javascript", "JavaScript Test Suite", JSTEST_HTML },
    { "about:jsext", "Full JS Test Suite", JSEXTTEST_HTML },
};

/* Read-only directory view (http_client.h): name + title per internal page,
 * same order as INTERNAL_PAGES — the home page's Test Cases section lists
 * exactly these entries (all 5 programmed about: sites). */
static const HttpTestPage TEST_PAGES[] = {
    { "about:home", "PlutoBrowser Home" },
    { "about:blank", "Blank Page" },
    { "about:acidtest", "HTML Acid Test" },
    { "about:javascript", "JavaScript Test Suite" },
    { "about:jsext", "Full JS Test Suite" },
};

size_t http_internal_page_body(const char *url, const char **bodyOut)
{
    if (bodyOut)
    {
        *bodyOut = NULL;
    }
    if (!url)
    {
        return 0;
    }
    for (size_t i = 0; i < sizeof(INTERNAL_PAGES) / sizeof(INTERNAL_PAGES[0]);
         i++)
    {
        if (strcmp(url, INTERNAL_PAGES[i].name) == 0)
        {
            if (bodyOut)
            {
                *bodyOut = INTERNAL_PAGES[i].html;
            }
            return strlen(INTERNAL_PAGES[i].html);
        }
    }
    return 0;
}

int http_test_pages(const HttpTestPage **entries)
{
    if (entries)
    {
        *entries = TEST_PAGES;
    }
    return (int)(sizeof(TEST_PAGES) / sizeof(TEST_PAGES[0]));
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Lua closeTcp: only close once the open has resolved; a still-connecting
 * socket is left to its open callback, which detects staleness via requestId
 * and closes itself (closing-while-connecting crashed the WX Simulator). */
static void close_tcp(void)
{
    if (g_tcp)
    {
        TCPConnection *t = g_tcp;
        g_tcp = NULL;
        if (g_connOpen || g_openFailed)
        {
            /* Open RESOLVED (success or failure): safe to close. Keep the
             * object pooled — never release a TLS connection the SDK may
             * still have event-loop state for (hazard 2). */
            g_pd->network->tcp->close(t);
        }
        else
        {
            /* Still connecting: touching it here crashed the WX Simulator.
             * Hand it to the orphan slot — its (stale) open callback will
             * close AND release it when the async open settles (hazard 1). */
            g_orphanTcp = t;
            if (g_pooledTcp == t)
            {
                g_pooledTcp = NULL; /* pool entry now owned by the orphan */
            }
        }
    }
}

static void reset_state(void)
{
    close_tcp();
    g_requestId++;
    memset(&g_cb, 0, sizeof(g_cb));
    g_url[0] = '\0';
    if (g_parsed)
    {
        pluto_free(g_parsed);
        g_parsed = NULL;
    }
    if (g_spill != PLUTO_SPILL_INVALID)
    {
        pluto_spill_discard(g_spill);
        g_spill = PLUTO_SPILL_INVALID;
    }
    g_spillMode = SPILL_NONE;
    g_bodySpilled = 0;
    strbuf_reset(&g_buf);
    g_status = 200;
    g_bodyStart = 0;
    g_isChunked = 0;
    g_contentLength = -1;
    g_isGzip = 0;
    g_gzipHold = 0;
    g_connOpen = 0;
    g_openFailed = 0;
    g_connClosed = 0;
    g_error[0] = '\0';
    g_state = HS_IDLE;
}

/* Build a minimal HTTP/1.1 GET request (reference buildRequest). */
static void build_request(StrBuf *out)
{
    strbuf_reset(out);
    strbuf_appendf(out, "GET %s HTTP/1.1\r\n", g_parsed->fullPath);
    if (g_parsed->port != 80 && g_parsed->port != 443)
    {
        strbuf_appendf(out, "Host: %s:%d\r\n", g_parsed->host, g_parsed->port);
    }
    else
    {
        strbuf_appendf(out, "Host: %s\r\n", g_parsed->host);
    }
    strbuf_appendf(out, "User-Agent: CometBrowser/1.0 (Playdate)\r\n");
    strbuf_appendf(out, "Accept: text/html,text/plain;q=0.8\r\n");
    strbuf_appendf(out, "Accept-Language: en-US,en;q=0.9\r\n");
    /* SW2c: opt into gzip. deflate (zlib) is NOT requested — our raw-
     * entry inflate handles the rare HTTP "deflate" as raw deflate if a
     * server sends it, but we do not advertise it. */
    strbuf_appendf(out, "Accept-Encoding: gzip\r\n");
    char cookie[768];
    cookie_jar_get_header(g_parsed->host, g_parsed->path, g_parsed->isSsl,
                          cookie, sizeof(cookie));
    if (cookie[0] != '\0')
    {
        strbuf_appendf(out, "Cookie: %s\r\n", cookie);
    }
    strbuf_appendf(out, "Connection: close\r\n\r\n");
}

/* SW2c: gzip member header/footer unwrap. RFC 1952: ID1=0x1f ID2=0x8b
 * CM=8, 4-byte MTIME, XFL, OS, then optional FEXTRA/FNAME/FCOMMENT/FHCRC
 * fields, then the raw deflate payload. Returns the payload size (the
 * deflate stream ends 8 bytes before the buffer end — CRC32 + ISIZE),
 * or 0 if this is not a well-formed single-member gzip buffer.
 * Servers send single-member streams; multi-member inputs would need
 * the footer skip repeated — treated as malformed here (0). */
static size_t gzip_payload_span(const unsigned char *b, size_t len)
{
    if (len < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8)
    {
        return 0;
    }
    size_t p = 10;
    unsigned char flg = b[3];
    if (flg & 0x04) /* FEXTRA */
    {
        if (p + 2 > len)
        {
            return 0;
        }
        size_t xlen = (size_t)b[p] | ((size_t)b[p + 1] << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) /* FNAME: NUL-terminated */
    {
        while (p < len && b[p] != 0)
        {
            p++;
        }
        p++;
    }
    if (flg & 0x10) /* FCOMMENT */
    {
        while (p < len && b[p] != 0)
        {
            p++;
        }
        p++;
    }
    if (flg & 0x02) /* FHCRC */
    {
        p += 2;
    }
    if (p + 8 >= len) /* payload + 8-byte footer must fit */
    {
        return 0;
    }
    return len - 8 - p;
}

/* Returns the byte offset of the raw deflate payload inside a gzip member
 * (after the 10-byte fixed header and any optional fields), or 0 when the
 * buffer is not a well-formed gzip member (for a real member the offset
 * is always ≥ 10). */
static size_t gzip_payload_offset(const unsigned char *b, size_t len)
{
    if (len < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8)
    {
        return 0;
    }
    size_t p = 10;
    unsigned char flg = b[3];
    if (flg & 0x04) /* FEXTRA */
    {
        if (p + 2 > len)
        {
            return 0;
        }
        size_t xlen = (size_t)b[p] | ((size_t)b[p + 1] << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) /* FNAME: NUL-terminated */
    {
        while (p < len && b[p] != 0)
        {
            p++;
        }
        p++;
    }
    if (flg & 0x10) /* FCOMMENT */
    {
        while (p < len && b[p] != 0)
        {
            p++;
        }
        p++;
    }
    if (flg & 0x02) /* FHCRC */
    {
        p += 2;
    }
    if (p + 8 >= len) /* payload + 8-byte footer must fit */
    {
        return 0;
    }
    return p;
}

/* Decode a chunked-encoded body. Returns a malloc'd string while the chunk
 * stream is complete, NULL while still incomplete (caller keeps buffering). */
static char *decode_chunked(const char *str, size_t len, size_t *outLen)
{
    StrBuf body;
    if (strbuf_init(&body) != 0)
    {
        return NULL;
    }
    if (outLen)
    {
        *outLen = 0;
    }
    size_t pos = 0;
    for (;;)
    {
        const char *crlf = NULL;
        for (size_t i = pos; i + 1 < len; i++)
        {
            if (str[i] == '\r' && str[i + 1] == '\n')
            {
                crlf = &str[i];
                break;
            }
        }
        if (!crlf)
        {
            strbuf_free(&body);
            return NULL; /* incomplete size line */
        }
        char sizeStr[32];
        size_t n = (size_t)(crlf - &str[pos]);
        if (n >= sizeof(sizeStr))
        {
            n = sizeof(sizeStr) - 1;
        }
        memcpy(sizeStr, &str[pos], n);
        sizeStr[n] = '\0';
        char *semi = strchr(sizeStr, ';');
        if (semi)
        {
            *semi = '\0';
        }
        /* Lua ^%s*(.-)%s*$ trim */
        char *s = sizeStr;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = '\0';

        long size = strtol(s, NULL, 16);
        if (size < 0)
        {
            strbuf_free(&body);
            return NULL;
        }
        pos = (size_t)(crlf - str) + 2;
        if (size == 0)
        {
            if (outLen)
            {
                *outLen = body.len;
            }
            return strbuf_detach(&body);
        }
        if (len < pos + (size_t)size + 2)
        {
            strbuf_free(&body);
            return NULL; /* incomplete chunk */
        }
        strbuf_append_n(&body, &str[pos], (size_t)size);
        pos += (size_t)size + 2;
    }
}

/* Case-insensitive header lookup over the saved set. */
static const char *saved_header(const char *key)
{
    for (int i = 0; i < g_savedHeaderCount; i++)
    {
        if (strcasecmp(g_savedHeaders[i][0], key) == 0)
        {
            return g_savedHeaders[i][1];
        }
    }
    return NULL;
}

/* Parse the status line and headers once "\r\n\r\n" has arrived.
 * Fills the SAVED header table (the done path consumes it after reset). */
static int parse_headers_saved(void)
{
    const char *buf = g_buf.data;
    size_t blen = g_buf.len;
    const char *hEnd = NULL;
    for (size_t i = 0; i + 3 < blen; i++)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            hEnd = &buf[i];
            break;
        }
    }
    if (!hEnd)
    {
        return 0;
    }

    g_savedHeaderCount = 0;
    g_isChunked = 0;
    g_contentLength = -1;
    g_isGzip = 0;
    g_gzipHold = 0;
    g_status = 200;

    /* Status line: first line. Lua: tonumber(match("HTTP/%d+%.%d+ (%d+)")) */
    {
        const char *lineEnd = memchr(buf, '\n', (size_t)(hEnd - buf));
        size_t llen = lineEnd ? (size_t)(lineEnd - buf) : (size_t)(hEnd - buf);
        char statusLine[128];
        if (llen >= sizeof(statusLine))
        {
            llen = sizeof(statusLine) - 1;
        }
        memcpy(statusLine, buf, llen);
        statusLine[llen] = '\0';
        const char *sp = strstr(statusLine, " ");
        if (sp)
        {
            /* Lua: tonumber(match("HTTP/%d+%.%d+ (%d+)")) — the HTTP/x.y
             * prefix is REQUIRED; anything else keeps the 200 default. */
            int vmaj = 0, vmin = 0, st = 0;
            if (sscanf(statusLine, "HTTP/%d.%d %d", &vmaj, &vmin, &st) == 3 && st > 0)
            {
                g_status = st;
            }
            (void)sp;
        }
    }

    /* Collect set-cookie values (need the host at call time).
     * BSS, not stack — 16×512B would sit under parse_headers_saved's caller
     * every frame a header parse runs (P22 stack rule). */
    char (*setCookies)[512] = g_setCookies;
    int setCookieCount = 0;

    const char *firstNl = (const char *)memchr(buf, '\n', blen);
    size_t lineStart = firstNl ? (size_t)(firstNl - buf) + 1 : 0;
    while (lineStart < (size_t)(hEnd - buf) &&
           g_savedHeaderCount < 62 && setCookieCount < 16)
    {
        size_t lineEnd = lineStart;
        while (lineEnd < (size_t)(hEnd - buf) && buf[lineEnd] != '\n')
        {
            lineEnd++;
        }
        size_t llen = lineEnd - lineStart;
        if (llen > 0 && buf[lineStart + llen - 1] == '\r')
        {
            llen--;
        }
        if (llen == 0 || llen >= sizeof(g_hdrLine))
        {
            lineStart = lineEnd + 1;
            continue;
        }
        memcpy(g_hdrLine, &buf[lineStart], llen);
        g_hdrLine[llen] = '\0';

        /* Lua ^%s*([^:]+)%s*:%s*(.-)%s*$ */
        char *colon = strchr(g_hdrLine, ':');
        if (colon)
        {
            *colon = '\0';
            char *k = g_hdrLine;
            while (*k == ' ' || *k == '\t') k++;
            char *kend = k + strlen(k);
            while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
            *kend = '\0';
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            char *vend = v + strlen(v);
            while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
            *vend = '\0';

            if (k[0] != '\0' && v[0] != '\0')
            {
                if (strcasecmp(k, "set-cookie") == 0)
                {
                    strncpy(setCookies[setCookieCount], v, 511);
                    setCookies[setCookieCount][511] = '\0';
                    setCookieCount++;
                }
                else if (g_savedHeaderCount < 62)
                {
                    size_t klen = strlen(k);
                    if (klen > 255)
                    {
                        klen = 255;
                    }
                    memcpy(g_savedHeaders[g_savedHeaderCount][0], k, klen);
                    g_savedHeaders[g_savedHeaderCount][0][klen] = '\0';
                    /* lower-case the key (Lua lk) */
                    for (char *p = g_savedHeaders[g_savedHeaderCount][0]; *p; p++)
                    {
                        if (*p >= 'A' && *p <= 'Z')
                        {
                            *p += 32;
                        }
                    }
                    size_t vlen = strlen(v);
                    if (vlen > 255)
                    {
                        vlen = 255;
                    }
                    memcpy(g_savedHeaders[g_savedHeaderCount][1], v, vlen);
                    g_savedHeaders[g_savedHeaderCount][1][vlen] = '\0';
                    g_savedHeaderCount++;
                }
            }
        }
        lineStart = lineEnd + 1;
    }

    /* Feed Set-Cookies to the jar now (host needed; Lua did it here too). */
    if (g_parsed && g_parsed->host[0] && setCookieCount > 0)
    {
        char *list[16];
        for (int i = 0; i < setCookieCount; i++)
        {
            list[i] = setCookies[i];
        }
        cookie_jar_process_set_cookies(g_parsed->host, list, setCookieCount);
    }

    const char *te = saved_header("transfer-encoding");
    g_isChunked = te && strstr(te, "chunked") != NULL;
    if (g_isChunked)
    {
        g_contentLength = -1;
    }
    else
    {
        const char *cl = saved_header("content-length");
        g_contentLength = cl ? atol(cl) : -1;
    }

    /* SW2c: detect gzip Content-Encoding. Any token-list containing the
     * exact token "gzip" (x-gzip treated as gzip, per browser practice). */
    {
        const char *ce = saved_header("content-encoding");
        g_isGzip = ce != NULL && (strstr(ce, "gzip") != NULL ||
                                  strstr(ce, "x-gzip") != NULL);
        if (g_isGzip)
        {
            logger_log("[http] gzip body (compressed %ld bytes)",
                       g_contentLength);
        }
    }

    g_bodyStart = (size_t)(hEnd - buf) + 4;

    /* SW2b: switch to disk streaming from here on. If headers landed with
     * zero body bytes in the same TCP read, open the spill file now; the
     * pump starts streaming on the next read. Any body bytes already in
     * RAM stay there (they are the prefix; disk continues after them). */
    if (g_status >= 200 && g_status < 300)
    {
        if (g_isGzip)
        {
            /* SW2c: no spill file at all — compressed staging stays in the
             * RAM StrBuf (one contiguous member for the one-shot gunzip).
             * g_gzipHold also raises the pump's StrBuf cap. */
            g_gzipHold = 1;
        }
        else
        {
            pluto_spill_init();
            g_spill = pluto_spill_begin();
            g_spillMode = (g_spill != PLUTO_SPILL_INVALID) ? SPILL_ACTIVE
                                                           : SPILL_NONE;
            if (g_spillMode == SPILL_ACTIVE)
            {
                logger_log("[http] spill: streaming body to disk");
            }
        }
    }
    return 1;
}

/* ── SDK callbacks ────────────────────────────────────────────────────────── */

static void tcp_closed_cb(TCPConnection *conn, PDNetErr err)
{
    (void)conn;
    (void)err;
    /* Stale events from a previous connection are ignored via generation id. */
    if (g_tcp == conn && g_state != HS_IDLE)
    {
        g_connClosed = 1;
    }
}

static void tcp_open_cb(TCPConnection *conn, PDNetErr err, void *ud)
{
    unsigned int myId = (unsigned int)(uintptr_t)ud;
    if (myId != g_requestId)
    {
        /* Stale open (cancelled/superseded while connecting): the async open
         * has settled NOW, so closing+releasing is finally safe. This is the
         * orphan handoff from close_tcp — plus a defensive pool check. */
        g_pd->network->tcp->close(conn);
        g_pd->network->tcp->release(conn);
        if (g_orphanTcp == conn)
        {
            g_orphanTcp = NULL;
        }
        if (g_pooledTcp == conn)
        {
            g_pooledTcp = NULL;
        }
        return;
    }
    if (err != NET_OK)
    {
        g_openFailed = 1;
        snprintf(g_error, sizeof(g_error), "Connection failed: %d", (int)err);
        g_state = HS_ERROR;
        return;
    }
    g_connOpen = 1;
}

static void access_cb(bool allowed, void *ud)
{
    (void)ud;
    if (!allowed)
    {
        /* Only meaningful if this reply still belongs to the live request.
         * A reply for a cancelled/reset request must not clobber fresh
         * state: reset_state() bumped g_requestId before reuse. */
        if (g_accessRequestId != g_requestId)
        {
            return;
        }
        snprintf(g_error, sizeof(g_error), "Network access denied.");
        g_state = HS_ERROR;
        return;
    }
    /* Connection opens on the NEXT http_update tick, never inside this SDK
     * callback — matching the reference's tick-deferral pattern. */
    if (g_state == HS_ACCESS_WAIT && g_accessRequestId == g_requestId)
    {
        g_state = HS_CONNECTING;
    }
}

/* ── Request start (Lua doGet) ────────────────────────────────────────────── */

typedef struct
{
    InternalPage *page;
    unsigned int id; /* request generation at scheduling time */
} AboutTimerCtx;

static void about_timer_cb(void *ud)
{
    AboutTimerCtx *ctx = (AboutTimerCtx *)ud;
    InternalPage *page = ctx->page;
    unsigned int id = ctx->id;
    PLUTO_FREE(ctx);
    if (id != g_requestId)
    {
        /* Superseded between scheduling and firing: a stale about: timer
         * must never deliver content (Lua's closure captured its own
         * callbacks; our global g_cb needs the generation tag). */
        return;
    }
    if (g_cb.onProgress)
    {
        g_cb.onProgress(100, 100);
    }
    if (g_cb.onSuccess)
    {
        char *keys[1] = { "content-type" };
        char *vals[1] = { "text/html" };
        g_cb.onSuccess(200, keys, vals, 1, page->html, strlen(page->html), g_url);
    }
    reset_state();
}

static int start_request(const char *urlString, const HttpCallbacks *callbacks)
{
    if (callbacks)
    {
        g_cb = *callbacks;
    }
    else
    {
        memset(&g_cb, 0, sizeof(g_cb));
    }
    strncpy(g_url, urlString ? urlString : "", sizeof(g_url) - 1);
    g_url[sizeof(g_url) - 1] = '\0';
    g_state = HS_CONNECTING;
    g_requestStart = g_pd->system->getCurrentTimeMilliseconds();
    strbuf_reset(&g_buf);
    g_status = 200;
    g_bodyStart = 0;
    g_isChunked = 0;
    g_contentLength = -1;
    g_isGzip = 0;
    g_gzipHold = 0;
    g_connOpen = 0;
    g_openFailed = 0;
    g_connClosed = 0;
    g_error[0] = '\0';

    /* ── Internal about: pages ────────────────────────────────────────────── */
    if (strncmp(g_url, "about:", 6) == 0)
    {
        InternalPage *page = NULL;
        for (size_t i = 0; i < sizeof(INTERNAL_PAGES) / sizeof(INTERNAL_PAGES[0]); i++)
        {
            if (strcmp(g_url, INTERNAL_PAGES[i].name) == 0)
            {
                page = (InternalPage *)&INTERNAL_PAGES[i];
                break;
            }
        }
        if (page)
        {
            /* Stay HS_CONNECTING for the 20ms window (Lua: requestState
             * stays "connecting" until the timer delivers) — setting DONE
             * here made http_update's done-path pre-fire an empty success
             * before the timer. */
            AboutTimerCtx *ctx = (AboutTimerCtx *)PLUTO_MALLOC(sizeof(AboutTimerCtx));
            if (ctx)
            {
                ctx->page = page;
                ctx->id = g_requestId;
                pdtimer_perform_after_delay(g_pd, 20, about_timer_cb, ctx);
            }
            else
            {
                /* Allocation failed: fail the request instead of hanging in
                 * CONNECTING until the 60s watchdog (and, without this,
                 * g_requestId is never bumped for this request, so a queued
                 * image fetch would tag its TCP callback with the PREVIOUS
                 * generation and be discarded as stale). */
                if (g_cb.onError)
                {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "out of memory");
                    g_cb.onError(msg);
                }
                reset_state();
            }
        }
        else
        {
            if (g_cb.onError)
            {
                char msg[1100];
                snprintf(msg, sizeof(msg), "Unknown internal page: %s", g_url);
                g_cb.onError(msg);
            }
            reset_state();
        }
        return 1;
    }

    /* ── Parse URL ────────────────────────────────────────────────────────── */
    if (!g_parsed)
    {
        g_parsed = (UrlParsed *)PLUTO_MALLOC(sizeof(UrlParsed));
        if (!g_parsed)
        {
            return 0;
        }
    }
    if (url_parse(g_url, g_parsed) != 0 || g_parsed->host[0] == '\0')
    {
        if (g_cb.onError)
        {
            char msg[1100];
            snprintf(msg, sizeof(msg), "Invalid URL (no hostname): %s", g_url);
            g_cb.onError(msg);
        }
        reset_state();
        return 0;
    }

    /* ── Network availability ─────────────────────────────────────────────── */
    if (!g_pd->network || !g_pd->network->tcp)
    {
        if (g_cb.onError)
        {
            g_cb.onError("Networking not available.");
        }
        reset_state();
        return 0;
    }

    /* ── HTTPS access request (C-only requirement; Lua prompted implicitly) ─ */
    if (g_parsed->isSsl)
    {
        /* Official docs: requestAccess returns an accessReply —
         *   kAccessAllow: already granted (or auto-granted); no dialog, the
         *     callback may never fire → proceed to connecting NOW.
         *   kAccessDeny: denied → error out.
         *   kAccessAsk: a dialog is up; the callback fires after the user
         *     responds → wait in HS_ACCESS_WAIT (watchdog deliberately does
         *     not cover this state). */
        g_accessRequestId = ++g_requestId;
        int reply = g_pd->network->tcp->requestAccess(
            g_parsed->host, g_parsed->port, 1,
            "CometBrowser Web Browsing", access_cb, NULL);
        if (reply == kAccessAllow)
        {
                g_state = HS_CONNECTING;
            return 1;
        }
        if (reply == kAccessDeny)
        {
            snprintf(g_error, sizeof(g_error), "Network access denied.");
            g_state = HS_ERROR;
            return 1;
        }
        /* kAccessAsk: mark the state and wait for access_cb. */
        g_state = HS_ACCESS_WAIT;
        return 1;
    }

    g_state = HS_CONNECTING;
    return 1;
}

/* Open the TCP connection (runs on the frame after start/access-allowed —
 * never inside the SDK access callback, matching the reference's tick
 * deferral pattern). */
static void open_connection(void)
{
    int pooled = g_pooledTcp && g_pooledPort == g_parsed->port &&
                 g_pooledSsl == g_parsed->isSsl &&
                 strncmp(g_pooledHost, g_parsed->host, sizeof(g_pooledHost)) == 0;
    TCPConnection *tcp;
    if (pooled)
    {
        /* Same host as last request: reopen the pooled connection (skips the
         * TLS handshake and dodges the SDK re-setup trap). */
        tcp = g_pooledTcp;
        g_pd->network->tcp->close(tcp);
    }
    else
    {
        if (g_pooledTcp)
        {
            /* Host switch: close now, but DEFER the release to the graveyard
             * tick (GRAVE_FRAMES later) — releasing while the SDK event loop
             * may still drain the connection's state crashes it (hazards 1+2). */
            g_pd->network->tcp->close(g_pooledTcp);
            g_graveTcp = g_pooledTcp;
            g_graveTimer = GRAVE_FRAMES;
            g_pooledTcp = NULL;
        }
        tcp = g_pd->network->tcp->newConnection(
            g_parsed->host, g_parsed->port, g_parsed->isSsl);
    }
    if (!tcp)
    {
        if (g_cb.onError)
        {
            char msg[300];
            snprintf(msg, sizeof(msg), "Could not open connection to %s", g_parsed->host);
            g_cb.onError(msg);
        }
        reset_state();
        return;
    }

    if (!pooled)
    {
        g_pooledTcp = tcp;
        snprintf(g_pooledHost, sizeof(g_pooledHost), "%s", g_parsed->host);
        g_pooledPort = g_parsed->port;
        g_pooledSsl = g_parsed->isSsl;
    }

    g_tcp = tcp;

    /* Generation id: any callback that no longer matches is a stale event. */
    unsigned int myId = ++g_requestId;

    g_pd->network->tcp->setConnectTimeout(tcp, SDK_TIMEOUT_MS);
    g_pd->network->tcp->setReadTimeout(tcp, SDK_TIMEOUT_MS);
    g_pd->network->tcp->setReadBufferSize(tcp, SDK_READ_BUFFER);
    g_pd->network->tcp->setConnectionClosedCallback(tcp, tcp_closed_cb);

    PDNetErr rc = g_pd->network->tcp->open(tcp, tcp_open_cb, (void *)(uintptr_t)myId);
    if (rc != NET_OK)
    {
        if (g_cb.onError)
        {
            char msg[300];
            snprintf(msg, sizeof(msg), "Could not open connection to %s", g_parsed->host);
            g_cb.onError(msg);
        }
        g_tcp = NULL; /* callback ownership not transferred on sync failure */
        if (g_pooledTcp == tcp)
        {
            g_pooledTcp = NULL; /* never leave a dangling pool entry */
        }
        g_pd->network->tcp->close(tcp);
        g_pd->network->tcp->release(tcp);
        reset_state();
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void http_client_init(PlaydateAPI *pd)
{
    g_pd = pd;
    strbuf_init(&g_buf);
    strbuf_init(&g_savedBuf);
}

int http_get(const char *urlString, const HttpCallbacks *callbacks)
{
    /* Cancel any previous request cleanly (Lua HttpClient.get). */
    reset_state();
    /* A fresh top-level request starts a new redirect chain. */
    g_hasPendingRedirect = 0;
    g_redirectDepth = 0;
    return start_request(urlString, callbacks);
}

void http_cancel(void)
{
    reset_state();
}


int http_is_loading(void)
{
    return g_state == HS_CONNECTING || g_state == HS_READING ||
           g_state == HS_ACCESS_WAIT;
}

/* ── Update: call once per frame ──────────────────────────────────────────── */

void http_update(void)
{
    if (!g_pd)
    {
        return;
    }

    /* Graveyard: release a host-switched connection once its event-loop state
     * has long drained (deferred release, see the lifecycle comment). */
    if (g_graveTcp && --g_graveTimer <= 0)
    {
        g_pd->network->tcp->release(g_graveTcp);
        g_graveTcp = NULL;
    }

    /* A deferred redirect (from a prior tick) opens the next connection now,
     * well after the previous connection was closed by us. */
    if (g_hasPendingRedirect)
    {
        HttpCallbacks cb = g_pendingRedirectCb;
        g_hasPendingRedirect = 0;
        reset_state();
        /* No stack copy needed: start_request memcpy's the URL into g_url
         * before anything else can touch g_pendingRedirectUrl. */
        start_request(g_pendingRedirectUrl, &cb);
        return;
    }

    if (g_state == HS_IDLE)
    {
        return;
    }

    unsigned int now = g_pd->system->getCurrentTimeMilliseconds();

    /* HTTPS access dialog is up: do nothing until the user answers (the 60s
     * watchdog deliberately does not cover this state). */
    if (g_state == HS_ACCESS_WAIT)
    {
        return;
    }

    /* Timeout watchdog (connecting/reading only — never ACCESS_WAIT). */
    if (g_state == HS_CONNECTING || g_state == HS_READING)
    {
        if (now - g_requestStart > REQUEST_TIMEOUT_MS)
        {
            if (g_buf.len > 512 || g_bodySpilled > 0)
            {
                g_state = HS_DONE; /* partial content wins */
            }
            else
            {
                snprintf(g_error, sizeof(g_error),
                         "Connection timed out after 60 seconds.");
                g_state = HS_ERROR;
            }
        }
    }

    /* ── Open the TCP connection on the frame AFTER start (deferred open) ── */
    if (g_state == HS_CONNECTING && !g_tcp && g_parsed && g_parsed->host[0] &&
        !g_openFailed && g_connOpen == 0)
    {
        /* Plain-http connections open here; the https path reaches this via
         * access_cb → HS_CONNECTING. open_connection guards double-open via
         * the g_tcp check. */
        open_connection();
        if (g_state != HS_CONNECTING)
        {
            return; /* open failed synchronously */
        }
    }

    /* ── Send the HTTP request once the connection is open ────────────────── */
    if (g_state == HS_CONNECTING && g_connOpen && g_tcp)
    {
        StrBuf req;
        strbuf_init(&req);
        build_request(&req);
        int sent = g_pd->network->tcp->write(g_tcp, req.data, req.len);
        strbuf_free(&req);
        if (sent >= 0)
        {
            g_state = HS_READING;
        }
        else if (sent == NET_WRITE_BUSY)
        {
            g_writePending = 1; /* retry next frame */
        }
        else
        {
            snprintf(g_error, sizeof(g_error), "Send failed: %d", (int)sent);
            g_state = HS_ERROR;
        }
    }

    /* ── Pump incoming data ───────────────────────────────────────────────── */
    if (g_state == HS_READING && g_tcp && g_connOpen)
    {
        size_t avail = g_pd->network->tcp->getBytesAvailable(g_tcp);
        if (avail > 0)
        {
            size_t want = avail < READ_CHUNK ? avail : READ_CHUNK;
            int n = g_pd->network->tcp->read(g_tcp, g_readChunk, want);
            if (n > 0)
            {
                /* SW2c: gzip bodies do NOT stream to disk — the compressed
                 * staging stays in the RAM StrBuf so the whole member is
                 * in one contiguous buffer for the one-shot gunzip at the
                 * done path. g_gzipHold freezes spill from the first
                 * post-header byte (headers alone are < MTU-sized TCP
                 * reads, so the entire compressed body lands in RAM). */
                if (g_bodyStart && !g_gzipHold &&
                    g_spillMode == SPILL_ACTIVE &&
                    g_spill != PLUTO_SPILL_INVALID)
                {
                    /* SW2b: stream body bytes straight to disk. The header
                     * terminator is fully in RAM before bodyStart is set,
                     * so every chunk here is pure body — no seam math. */
                    if (pluto_spill_write(g_spill, g_readChunk, (size_t)n) != 0)
                    {
                        /* Disk failure: reassemble the true stream order in
                         * RAM (prefix already there, spilled bytes read back,
                         * then this chunk) and finish in RAM, residency-
                         * capped like the old behavior. */
                        logger_log("[http] spill write failed; RAM fallback");
                        g_spillMode = SPILL_NONE;
                        size_t prefixLen = g_buf.len;
                        size_t need = prefixLen + (size_t)g_bodySpilled +
                                      (size_t)n;
                        char *nb = (char *)PLUTO_MALLOC(need + 1);
                        if (nb)
                        {
                            memcpy(nb, g_buf.data, prefixLen);
                            if (g_bodySpilled > 0)
                            {
                                pluto_spill_read(g_spill, 0,
                                                 nb + prefixLen,
                                                 (size_t)g_bodySpilled);
                            }
                            memcpy(nb + prefixLen + g_bodySpilled,
                                   g_readChunk, (size_t)n);
                            nb[need] = '\0';
                            pluto_free(g_buf.data);
                            g_buf.data = nb;
                            g_buf.len = need;
                            g_buf.cap = need + 1;
                        }
                        else
                        {
                            strbuf_append_n(&g_buf, g_readChunk, (size_t)n);
                        }
                        g_bodySpilled = 0; /* now redundant (in g_buf) */
                    }
                    else
                    {
                        g_bodySpilled += n;
                    }
                }
                else if (g_buf.len <
                         (g_gzipHold ? GZIP_DELIVERY_CAP : MAX_RESPONSE_SIZE))
                {
                    strbuf_append_n(&g_buf, g_readChunk, (size_t)n);
                }
                /* else: over cap — bytes dropped; completion/overflow checks
                 * below turn this into the "too large" error path. */
                long tot = g_contentLength;
                if (tot < 0)
                {
                    tot = 0;
                }
                long cur = 0;
                if (g_bodyStart)
                {
                    /* SW2b: body bytes = RAM prefix + disk-streamed tail. */
                    cur = (long)(g_buf.len - g_bodyStart) + g_bodySpilled;
                    if (cur < 0)
                    {
                        cur = 0;
                    }
                    if (tot > 0 && cur > tot)
                    {
                        cur = tot;
                    }
                }
                if (g_cb.onProgress)
                {
                    g_cb.onProgress((int)cur, (int)tot);
                }
            }
        }
    }

    /* ── Parse headers once they've fully arrived ─────────────────────────── */
    if (g_state == HS_READING && !g_bodyStart)
    {
        int haveSep = 0;
        for (size_t i = 0; i + 3 < g_buf.len; i++)
        {
            if (g_buf.data[i] == '\r' && g_buf.data[i + 1] == '\n' &&
                g_buf.data[i + 2] == '\r' && g_buf.data[i + 3] == '\n')
            {
                haveSep = 1;
                break;
            }
        }
        if (haveSep)
        {
            parse_headers_saved();

            /* Redirect handling entirely here (why we're on raw TCP). */
            const char *loc = NULL;
            if (g_status >= 300 && g_status < 400)
            {
                for (int i = 0; i < g_savedHeaderCount; i++)
                {
                    if (strcasecmp(g_savedHeaders[i][0], "location") == 0)
                    {
                        loc = g_savedHeaders[i][1];
                        break;
                    }
                }
            }
            if (g_status >= 300 && g_status < 400 && loc && loc[0])
            {
                g_redirectDepth++;
                if (g_redirectDepth <= MAX_REDIRECTS)
                {
                    char *resolved = url_resolve(g_url, loc);
                    if (resolved)
                    {
                        strncpy(g_pendingRedirectUrl, resolved,
                                sizeof(g_pendingRedirectUrl) - 1);
                        g_pendingRedirectUrl[sizeof(g_pendingRedirectUrl) - 1] = '\0';
                        pluto_free(resolved);
                    }
                    else
                    {
                        strncpy(g_pendingRedirectUrl, loc,
                                sizeof(g_pendingRedirectUrl) - 1);
                        g_pendingRedirectUrl[sizeof(g_pendingRedirectUrl) - 1] = '\0';
                    }
                    g_pendingRedirectCb = g_cb;
                    g_hasPendingRedirect = 1;
                }
                else
                {
                    snprintf(g_error, sizeof(g_error), "Too many redirects to %.200s",
                             g_url);
                    g_state = HS_ERROR;
                }
                reset_state(); /* closes TCP; redirect opens next tick */
                return;
            }
        }
    }

    /* ── Detect a complete body ───────────────────────────────────────────── */
    if (g_state == HS_READING && g_bodyStart)
    {
        /* SW2b: bodyBytes = RAM prefix + disk-streamed tail. */
        size_t bodyBytes = (g_buf.len > g_bodyStart ? g_buf.len - g_bodyStart
                                                    : 0) +
                           (size_t)g_bodySpilled;
        if (g_isChunked)
        {
            /* SW2b: only decode for completion once ALL bytes are in one
             * place. Spill mode keeps the tail on disk, so completion is
             * declared when the connection closes (below). A spilled
             * chunked stream therefore always waits for connClosed — same
             * total wait as Content-Length-less streams, correct result. */
            if (!g_spillMode)
            {
                size_t decLen;
                char *dec = decode_chunked(g_buf.data + g_bodyStart,
                                           g_buf.len - g_bodyStart, &decLen);
                if (dec)
                {
                    pluto_free(dec);
                    g_state = HS_DONE;
                }
            }
        }
        else if (g_contentLength >= 0)
        {
            if ((long)bodyBytes >= g_contentLength)
            {
                g_state = HS_DONE;
            }
        }
        if (g_spillMode == SPILL_ACTIVE &&
            (size_t)g_bodySpilled >= SPILL_MAX_BODY)
        {
            /* Runaway no-length stream (server ignore/close semantics):
             * bound the disk file. Delivered as a partial body. */
            logger_log("[http] spill cap %d reached; partial delivery",
                       SPILL_MAX_BODY);
            g_state = HS_DONE;
        }
        if (!g_spillMode && g_buf.len >= MAX_RESPONSE_SIZE)
        {
            g_state = HS_DONE;
        }
    }

    /* ── Server closed the connection ─────────────────────────────────────── */
    if (g_state == HS_READING && g_connClosed)
    {
        if (g_buf.len == 0 && g_bodySpilled == 0)
        {
            snprintf(g_error, sizeof(g_error),
                     "Connection closed before any data was received.");
            g_state = HS_ERROR;
        }
        else
        {
            g_state = HS_DONE;
        }
    }

    /* ── Handle completed request ─────────────────────────────────────────── */
    if (g_state == HS_DONE)
    {
        /* Save everything the callback needs (Lua saved locals). */
        g_savedStatus = g_status;
        g_savedBodyStart = g_bodyStart;
        g_savedIsChunked = g_isChunked;
        size_t bodyOff = g_savedBodyStart ? g_savedBodyStart : 0;
        size_t bodyLen;
        size_t deliveredLen;
        char *body = NULL;

        /* SW2c: gunzip delivery. gzip bodies stage compressed (spill frozen
         * via g_gzipHold), so the delivery buffer is the DECOMPRESSED body:
         * unwrap the gzip member, inflate the raw deflate payload, enforce
         * the footer ISIZE as a strict bound (input consumed must equal
         * declared size — never deliver trailing-garbage output). Corrupt
         * input = clean onError, no HTML partial-render. Chunked bodies
         * de-chunk FIRST (the member arrives in chunks; the gunzip input
         * must be one contiguous buffer). */
        char *gzSource = NULL;
        size_t gzSourceLen = 0;
        if (g_isGzip)
        {
            if (g_savedIsChunked)
            {
                size_t decLen = 0;
                gzSource = decode_chunked(g_buf.data + bodyOff,
                                          g_buf.len - bodyOff, &decLen);
                gzSourceLen = decLen;
                if (!gzSource)
                {
                    snprintf(g_error, sizeof(g_error),
                             "Corrupt chunked gzip body received.");
                    char errSnapshot[256];
                    strncpy(errSnapshot, g_error, sizeof(errSnapshot) - 1);
                    errSnapshot[sizeof(errSnapshot) - 1] = '\0';
                    HttpCallbacks errCb = g_cb;
                    reset_state();
                    if (errCb.onError)
                    {
                        errCb.onError(errSnapshot);
                    }
                    return;
                }
                /* The chunk-decode buffer is now the delivery buffer's
                 * source; ownership transfers to the gunzip path below
                 * (freed there via PLUTO_FREE). */
            }
            else
            {
                gzSource = g_buf.data + bodyOff;
                gzSourceLen = g_buf.len - bodyOff;
            }
        }

        if (g_isGzip && gzSource)
        {
            const unsigned char *cbuf = (const unsigned char *)gzSource;
            size_t clen = gzSourceLen;
            size_t payload = gzip_payload_span(cbuf, clen);
            size_t poff = gzip_payload_offset(cbuf, clen);
            size_t isize = 0;
            if (payload > 0)
            {
                isize = (size_t)cbuf[clen - 4] |
                        ((size_t)cbuf[clen - 3] << 8) |
                        ((size_t)cbuf[clen - 2] << 16) |
                        ((size_t)cbuf[clen - 1] << 24);
            }
            /* Chunked sources are heap buffers from decode_chunked (PLUTO
             * allocator) — release them on every exit; g_buf slices are
             * interior pointers, never freed here. */
#define GZ_FREE_SOURCE()                                               \
    do                                                                 \
    {                                                                  \
        if (gzSource && gzSource != (char *)(g_buf.data + bodyOff))    \
        {                                                              \
            PLUTO_FREE(gzSource);                                      \
        }                                                              \
    } while (0)

            if (payload == 0 || isize > (size_t)GZIP_DELIVERY_CAP)
            {
                logger_log("[http] gzip member malformed (payload=%zu "
                           "isize=%zu)", payload, isize);
                GZ_FREE_SOURCE();
                snprintf(g_error, sizeof(g_error),
                         "Corrupt gzip body received.");
                char errSnapshot[256];
                strncpy(errSnapshot, g_error, sizeof(errSnapshot) - 1);
                errSnapshot[sizeof(errSnapshot) - 1] = '\0';
                HttpCallbacks errCb = g_cb;
                reset_state();
                if (errCb.onError)
                {
                    errCb.onError(errSnapshot);
                }
                return;
            }
            bodyLen = isize;
            deliveredLen = 0;
            body = (char *)PLUTO_MALLOC(isize + 1);
            if (body)
            {
                size_t got = 0;
                if (isize == 0)
                {
                    body[0] = '\0';
                    deliveredLen = 0;
                }
                else
                {
                    InflateStream *is = inflate_stream_new_raw(cbuf + poff, payload);
                    if (is)
                    {
                        for (;;)
                        {
                            size_t chunk = 0;
                            const uint8_t *p = inflate_stream_read(
                                is, 8192, &chunk);
                            if (!p || chunk == 0)
                            {
                                break; /* end of stream */
                            }
                            if (got + chunk > isize)
                            {
                                logger_log("[http] gzip output exceeds "
                                           "ISIZE (%zu > %zu)",
                                           got + chunk, isize);
                                got = 0;
                                break;
                            }
                            memcpy(body + got, p, chunk);
                            got += chunk;
                        }
                        inflate_stream_free(is);
                    }
                    body[got] = '\0';
                    deliveredLen = got;
                    if (got != isize)
                    {
                        logger_log("[http] gunzip short: got %zu of %zu",
                                   got, isize);
                        PLUTO_FREE(body);
                        body = NULL;
                        GZ_FREE_SOURCE();
                        snprintf(g_error, sizeof(g_error),
                                 "Corrupt gzip body received.");
                        char errSnapshot[256];
                        strncpy(errSnapshot, g_error,
                                sizeof(errSnapshot) - 1);
                        errSnapshot[sizeof(errSnapshot) - 1] = '\0';
                        HttpCallbacks errCb = g_cb;
                        reset_state();
                        if (errCb.onError)
                        {
                            errCb.onError(errSnapshot);
                        }
                        return;
                    }
                }
            }
            if (!body)
            {
                /* Allocation failure (or isize==0 without a buffer): surface
                 * as error — a silent no-callback DONE would hang the caller. */
                GZ_FREE_SOURCE();
                snprintf(g_error, sizeof(g_error),
                         "Out of memory delivering response.");
                char errSnapshot[256];
                strncpy(errSnapshot, g_error, sizeof(errSnapshot) - 1);
                errSnapshot[sizeof(errSnapshot) - 1] = '\0';
                HttpCallbacks errCb = g_cb;
                reset_state();
                if (errCb.onError)
                {
                    errCb.onError(errSnapshot);
                }
                return;
            }
            /* Success: skip the legacy assembly below. */
            GZ_FREE_SOURCE();
        }
        else if (g_spillMode == SPILL_ACTIVE && g_spill != PLUTO_SPILL_INVALID)
        {
            /* SW2b: assemble the delivery buffer ONCE, exact size — the RAM
             * prefix (headers + first body bytes) followed by the disk tail
             * read back in bounded chunks. One body-sized allocation
             * replaces the old ~3× response peak (live buffer + full saved
             * copy + body slice). Overflow-guarded against hostile lengths. */
            long diskBytes = pluto_spill_finish(g_spill);
            SpillFile sp = g_spill;      /* handle stays valid for reads */
            g_spill = PLUTO_SPILL_INVALID;
            g_spillMode = SPILL_NONE;
            size_t ramBody = g_buf.len > bodyOff ? g_buf.len - bodyOff : 0;
            if (diskBytes < 0 ||
                ramBody > (size_t)MAX_RESPONSE_SIZE ||
                (size_t)diskBytes > (size_t)MAX_RESPONSE_SIZE ||
                ramBody + (size_t)diskBytes > (size_t)MAX_RESPONSE_SIZE)
            {
                logger_log("[http] spill delivery over residency cap "
                           "(ram=%zu disk=%ld)", ramBody, diskBytes);
                pluto_spill_discard(sp);
                snprintf(g_error, sizeof(g_error),
                         "Response too large to deliver.");
                g_state = HS_ERROR;
                /* g_buf still holds the prefix; the ERROR path below frees
                 * it via reset_state — handle immediately, not next frame: */
                char errSnapshot[256];
                strncpy(errSnapshot, g_error, sizeof(errSnapshot) - 1);
                errSnapshot[sizeof(errSnapshot) - 1] = '\0';
                HttpCallbacks errCb = g_cb;
                reset_state();
                if (errCb.onError)
                {
                    errCb.onError(errSnapshot);
                }
                return;
            }
            bodyLen = ramBody + (size_t)diskBytes;
            deliveredLen = bodyLen;
            body = (char *)PLUTO_MALLOC(bodyLen + 1);
            if (body)
            {
                if (ramBody)
                {
                    memcpy(body, g_buf.data + bodyOff, ramBody);
                }
                size_t done = ramBody;
                while (done < bodyLen)
                {
                    size_t want = bodyLen - done;
                    if (want > READ_CHUNK)
                    {
                        want = READ_CHUNK;
                    }
                    long got = pluto_spill_read(sp, (long)done,
                                                body + done, want);
                    if (got <= 0)
                    {
                        logger_log("[http] spill read short at %zu", done);
                        break;
                    }
                    done += (size_t)got;
                }
                body[done] = '\0';
                deliveredLen = done;
            }
            pluto_spill_discard(sp);
        }
        else
        {
            /* RAM path (unchanged semantics): keep the saved-copy flow —
             * spill consumers already materialized their own delivery. */
            strbuf_reset(&g_savedBuf);
            strbuf_append_n(&g_savedBuf, g_buf.data, g_buf.len);
            bodyLen = g_savedBuf.len > bodyOff
                          ? g_savedBuf.len - bodyOff
                          : 0;
            deliveredLen = bodyLen;
            if (g_savedIsChunked && bodyLen)
            {
                body = decode_chunked(g_savedBuf.data + bodyOff, bodyLen,
                                      &deliveredLen);
                if (!body)
                {
                    body = strbuf_detach(&g_savedBuf) + bodyOff; /* unreachable */
                }
            }
            if (!body)
            {
                /* NUL-terminate a copy of the body slice for the callback. */
                body = (char *)PLUTO_MALLOC(bodyLen + 1);
                if (body)
                {
                    memcpy(body, g_savedBuf.data + bodyOff, bodyLen);
                    body[bodyLen] = '\0';
                }
            }
        }

        /* Headers stay in BSS g_savedHeaders (reset_state does not clear it,
         * and no reentrant path can parse new headers during the callback) —
         * a 32KB stack copy here was the last update-loop stack hazard. */
        int hc = g_savedHeaderCount;
        /* static: ~1.1KB off the game-task stack (the done path runs inside
         * http_update inside updateFrame; device gameTask stack is tiny and
         * the callback chain below adds several KB more). The done path
         * cannot reenter itself: reset_state() already ran and no other
         * request can start until the callback returns. */
        static char *k[64], *v[64];
        for (int i = 0; i < hc; i++)
        {
            k[i] = g_savedHeaders[i][0];
            v[i] = g_savedHeaders[i][1];
        }
        static char urlSnapshot[1024];
        strncpy(urlSnapshot, g_url, sizeof(urlSnapshot) - 1);
        urlSnapshot[sizeof(urlSnapshot) - 1] = '\0';
        HttpCallbacks cb = g_cb;

        reset_state();

        if (cb.onSuccess && body)
        {
            cb.onSuccess(g_savedStatus, k, v, hc, body, deliveredLen, urlSnapshot);
        }
        PLUTO_FREE(body);
    }
    else if (g_state == HS_ERROR)
    {
        char errSnapshot[256];
        strncpy(errSnapshot, g_error[0] ? g_error : "Connection failed.",
                sizeof(errSnapshot) - 1);
        errSnapshot[sizeof(errSnapshot) - 1] = '\0';
        HttpCallbacks cb = g_cb;

        reset_state();

        if (cb.onError)
        {
            cb.onError(errSnapshot);
        }
    }
}

/* TEST-ONLY accessor (host CSS verification): the raw body of an internal
 * page. Not in the header — host tests declare it extern. */
const char *pluto_internal_body_for_test(const char *url)
{
    for (size_t i = 0; i < sizeof(INTERNAL_PAGES) / sizeof(INTERNAL_PAGES[0]);
         i++)
    {
        if (strcmp(url, INTERNAL_PAGES[i].name) == 0)
        {
            return INTERNAL_PAGES[i].html;
        }
    }
    return NULL;
}
