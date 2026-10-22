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
#include "core/netmon.h"
#include "util/strbuf.h"
#include "util/pdtimer.h"
#include "core/pluto_mem.h"
#include "core/pluto_spill.h"
#include "core/constants.h"
#include "core/storage.h"
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

/* C4 (fetch/POST): request-line method + optional REQUEST body. The
 * whole-life contract was GET-only (every existing caller reads GET here
 * unchanged: http_get zeroes both). The body is an OWNED copy so the
 * caller's buffer (JsHttpRequest.reqBody) can go away mid-flight... it
 * never does (the router owns the request till the sweep), but a copy
 * keeps the http_client ownership story symmetric with the response. */
static char g_reqMethod[8];          /* "GET" default */
static char *g_reqBody;              /* owned copy */
static size_t g_reqBodyLen;
/* R56g: per-call POST Content-Type (set via http_request_ct; empty =
 * the historical "application/json" so existing callers stay identical). */
static char g_reqContentType[64];
/* NetP2: the overridable base builder lines — a page-set user-agent /
 * accept / accept-language replaces the engine's default (the page's
 * header wins, one line on the wire). Per-request flight state like
 * reqMethod (reset_state clears them; the front door seeds). */
static char g_reqUserAgent[96];
static char g_reqAccept[128];
static char g_reqAcceptLang[64];
/* NetP2: the page's own request headers (setRequestHeader lane) deep-
 * copied at call time — build_request emits them byte-for-byte beside
 * the engine's base headers. Per-request flight state: reset_state
 * clears the count; the front door re-fills it after its own reset
 * (the same alive-after-reset pattern g_reqContentType rides), and the
 * redirect lane deliberately re-arms it across hops (browser-true). */
static HttpHeaderPair g_extraHeaders[HTTP_EXTRA_HEADERS_MAX];
static int g_extraHeaderCount = 0;

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
    HS_RETRY_WAIT, /* R18b: backoff between connect attempts */
    HS_RADIO_HEAL, /* R18c: cycling the radio to re-associate */
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
/* R15: identity-mode selector. The PAGE renderer wants raw streams: gzip
 * bodies are staged WHOLLY in RAM (one contiguous member for the one-shot
 * gunzip), which shreds the heap for a 500KB bundle and starves the next
 * compile. HTML documents keep gzip (small pages, big bandwidth win); the
 * external-script fetcher (jsext) flips this to 1 around its downloads so
 * script bodies stream to disk raw. NOT site-specific: any consumer that
 * adopts the body to disk opts in. */
static int g_identityEncoding = 0;
void http_set_identity_encoding(int on) { g_identityEncoding = on; }
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

/* ── NetP1: HTTP response cache tier ────────────────────────────────────────
 * GET-only, 200-only, byte-exact URL key, RAM-bounded per-boot LRU.
 * Freshness: Cache-Control max-age honored; no-cache or no freshness
 * directive stores with maxAge = -1 = ALWAYS revalidate (the conservative
 * browser stance — reuse only what the server re-approves).
 * Validators: ETag (If-None-Match) primary; Last-Modified rides along as
 * If-Modified-Since when present. Both are injected by build_request from
 * g_revalPending state — client-internal, never page-controlled, so the
 * setRequestHeader boundary is unchanged.
 * 304 handling is browser-true: the wire 304 never reaches the JS road.
 * The pump swaps the cached body in (status 200 + cached content-type +
 * cached bytes) and the ordinary completion check delivers; a cold 304
 * (no pending reval) passes through verbatim as before.
 * Skips storing: empty bodies, bodies over the per-entry cap, no-store,
 * any Vary response, URLs over the key cap; spill zero-copy deliveries
 * never reach the store hook (body stays NULL there) and about: pages
 * bypass the tier entirely (the internal store answers before us). No
 * persistence: RAM-only per boot — cookies stay the only store-backed
 * transport state. */
#define HTTP_CACHE_ENTRIES 8
#define HTTP_CACHE_ENTRY_CAP (128 * 1024) /* per-entry body cap */
#define HTTP_CACHE_TOTAL_CAP (512 * 1024) /* total body-bytes cap */
#define HTTP_CACHE_KEY_CAP 512            /* max URL bytes kept as key */
typedef struct HttpCacheEntry
{
    int used;
    char url[HTTP_CACHE_KEY_CAP];
    char etag[128];   /* verbatim wire token (quoted form preserved) */
    char lastMod[64]; /* verbatim Last-Modified value */
    long maxAge;      /* seconds; -1 = no freshness → always revalidate */
    unsigned int fetchedAt;
    char contentType[128];
    char *body; /* decoded body copy (post-gzip/chunked) */
    size_t bodyLen;
    unsigned int touched; /* monotonic LRU stamp */
} HttpCacheEntry;
static HttpCacheEntry g_httpCache[HTTP_CACHE_ENTRIES];
static unsigned int g_httpCacheStamp = 0;
static size_t g_httpCacheBytes = 0;
/* In-flight revalidation: set by the start_request consult, consumed by
 * the pump's 304 swap; cleared by reset_state (a redirect mid-reval
 * cancels it and the entry simply stays stale for next time). */
static int g_revalPending = 0;
static int g_revalIndex = -1;
/* Delivery-source markers for the done-path store hook: never re-store a
 * body the cache itself just delivered, and never rebuild a 304-revalidated
 * entry from the wire-304's stripped header set (the swap already re-armed
 * the entry's freshness clock — RFC 7234 §4.2.1 behavior). */
static int g_servedFromCache = 0;
static int g_revalServed304 = 0;

/* Redirects: deferred to a later update tick (reference parity). */
static char g_pendingRedirectUrl[1024];
static HttpCallbacks g_pendingRedirectCb;
static int g_hasPendingRedirect = 0;
static int g_redirectDepth = 0;
/* NetP3: redirect verb semantics (browser-true matrix). 301/302/303
 * rewrite POST→GET with the body dropped — the standing, receipted
 * behavior. 307/308 PRESERVE the method + body + Content-Type through
 * the deferred restart. Capture happens in the redirect DECISION block
 * (the body is an OWNED copy — the pointer is stolen there so the
 * decision's reset_state cannot free it); the deferred restart lane
 * re-arms it after its own reset, ownership moving back to
 * g_reqBody. Unconsumed captures are freed by the front door (a
 * brand-new request supersedes the pending hop) — never by
 * reset_state, which must not see the steal. */
static int g_redirectPreserveVerb = 0;   /* 1 = 307/308 hop */
static char g_redirectHopMethod[8];      /* captured verb ("" = GET) */
static char *g_redirectHopBody = NULL;   /* stolen owned body pointer */
static size_t g_redirectHopBodyLen = 0;
static char g_redirectHopCt[64];         /* captured Content-Type */
/* NetP2/NetP3: the page's own request headers follow redirect hops
 * (browser-true) — captured in the DECISION block as well, because the
 * decision's reset_state clears the live lane before the deferred
 * restart can read it. */
static HttpHeaderPair g_hopExtra[HTTP_EXTRA_HEADERS_MAX];
static int g_hopExtraCount = 0;
static char g_hopUA[96], g_hopAccept[128], g_hopAcceptLang[64];

static int g_writePending = 0; /* NET_WRITE_BUSY retry in flight */

/* ── Connect-attempt watchdog + bounded retry (R18b) ───────────────────────
 * Device evidence (runs 1–5, site-agnostic): the SDK open() can sit IN FLIGHT
 * forever — its own 10s setConnectTimeout callback never fires, no error, no
 * close — burning the whole 60s request budget on a wedged attempt. Every
 * desktop browser treats flaky first-connect as routine and retries; that
 * behavior is general-purpose. Design:
 *   - Each connect attempt gets CONNECT_ATTEMPT_TIMEOUT_MS. On expiry, if the
 *     open has NOT resolved, the attempt is handed to the orphan slot (its
 *     stale-callback path closes+releases it — the proven safe teardown) and
 *     a FRESH connection is opened after a short backoff.
 *   - Bounded: HTTP_CONNECT_ATTEMPTS tries total, then the standard error
 *     path runs. The 60s request watchdog still backstops everything.
 *   - The window from open-resolved to first byte is separately watched
 *     (RESPONSE_FIRST_BYTE_TIMEOUT_MS): a connection that opens but never
 *     yields headers is recycled the same way. Once bytes flow, normal
 *     60s budget applies.
 * No URL/host knowledge anywhere — pure transport policy. */
#define CONNECT_ATTEMPT_TIMEOUT_MS   12000 /* per-attempt open watchdog   */
#define HTTP_CONNECT_ATTEMPTS        3     /* total attempts (incl. first) */
#define CONNECT_BACKOFF_MS           1000  /* delay between attempts       */
#define RESPONSE_FIRST_BYTE_TIMEOUT_MS 20000 /* open→headers-only window  */
static unsigned int g_attemptStart = 0;   /* ms timestamp of attempt start */
static int g_connectAttempts = 0;         /* attempts used so far          */
static int g_responseWaitLogged = 0;      /* first-byte warning emitted    */

/* ── R18c: radio self-heal ────────────────────────────────────────────
 * Device evidence: three consecutive fresh TCP opens each sit in flight
 * forever (no open callback, no error, no connect-timeout callback) while
 * the OS still reports the Wi-Fi association up — the stale-association
 * signature. Desktop OSes recover from exactly this by cycling the
 * interface; the SDK exposes the same lever:
 * network->setEnabled(false→true) with completion callbacks. One heal
 * cycle per request, gated on transport-dead evidence, then the connect
 * budget starts over. No URL/host/site knowledge — pure transport policy. */
static int g_radioHealUsed = 0;          /* max one heal per request       */
static int g_radioHealPhase = 0;         /* 1 disabling, 2 enabling, 3 settle */
static unsigned int g_healPhaseStart = 0;
static volatile int g_radioCbFired = 0;
static volatile int g_radioCbErr = 0;
static int g_radioCbErrLogged = 0;       /* enable-cb error logged once */
#define RADIO_PHASE_TIMEOUT_MS 6000 /* per-phase callback wait cap        */
#define RADIO_ASSOC_WAIT_MS   25000 /* association wait after enable req  */
#define RADIO_STEP_GAP_MS      1000 /* pause between disable and enable   */
static int g_lastAssocStatus = -1;       /* for status-transition logging  */


/* ── Internal about: pages (verbatim from the reference) ─────────────────── */
typedef struct
{
    const char *name;
    const char *title;
    const char *html;
} InternalPage;

static const char ACIDTEST_HTML[] =
    "<html><head><title>HTML Renderer Test Suite</title>\n"
    "<style>\n"
    ".css-hide-test { display: none }\n"
    ".css-center-test { text-align: center }\n"
    ".css-right-test { text-align: right }\n"
    ".css-bold-test { font-weight: bold }\n"
    ".css-upper-test { text-transform: uppercase }\n"
".css-nowrap-test { white-space: nowrap }\n"
    ".css-pre-test { white-space: pre }\n"
    "ul.css-disc-test { list-style-type: disc }\n"
    "ol.css-roman-test { list-style-type: lower-roman }\n"
    ".css-box-test { border: 1px solid black; background-color: #cccccc; margin: 10px; padding: 6px }\n"
    ".css-link-test::after { content: \" [\" attr(href) \"]\" }\n"
    "table.css-tab-test { border: 1px solid black; border-collapse: collapse }\n"
    "table.css-tab-test td { padding: 4px }\n"
    ".css-size-test { font-size: x-large }\n"
    ".css-indent-test { text-indent: 20px }\n"
    "ul.css-first-test > li:first-child { font-weight: bold }\n"
    ".css-flex-test { display: flex }\n"
    "@media (max-width: 400px) { .css-media-test { font-weight: bold } }\n"
    "</style></head><body>\n"
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
    "<h2>Document Structure</h2>\n"
    "<p>Body and head have explicit handling: this body renders as a block container, head metadata stays invisible (title above, no stray text).</p>\n"
    "<p><nobr>This-nobr-line-must-never-wrap-even-though-it-is-far-wider-than-the-400-pixel-screen-it-runs-off-the-edge-instead</nobr></p>\n"
    "\n"
    "<h2>Interactive Controls</h2>\n"
    "<p>Tap (or crank-click) the radios and the dropdown, then Submit and check the address bar shows your picks.</p>\n"
    "<form action=\"https://example.com/check\" method=\"get\">\n"
    "<input type=\"radio\" name=\"ship\" value=\"fast\" checked> Fast shipping\n"
    "<input type=\"radio\" name=\"ship\" value=\"slow\"> Slow shipping<br>\n"
    "<select name=\"size\"><option>S</option><option selected>M</option><option>L</option></select>\n"
    "<input type=\"submit\" value=\"Check picks\">\n"
    "</form>\n"
    "<p>Expected after Submit: the address bar contains ship=slow&amp;size=L (if you picked Slow + L).</p>\n"
    "<p>Twin text fields (same name, the way JS apps render forms): type in one, then the other — each keeps its own text.</p>\n"
    "<form action=\"https://example.com/twin\" method=\"get\">\n"
    "<input type=\"text\" name=\"who\" placeholder=\"first twin\">\n"
    "<input type=\"text\" name=\"who\" placeholder=\"second twin\">\n"
    "<input type=\"submit\" value=\"Send twins\">\n"
    "</form>\n"
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
    "<h2>CSS Engine</h2>\n"
    "<p class=\"css-hide-test\">HIDDEN-BY-CSS (this line must never render)</p>\n"
    "<p class=\"css-center-test\">Centered via CSS (this line is centered)</p>\n"
    "<p class=\"css-right-test\">Right via CSS (this line is right-aligned)</p>\n"
    "<p class=\"css-bold-test\">Bold via CSS (this line renders bold)</p>\n"
    "<p class=\"css-upper-test\">upper via css (renders UPPERCASE)</p>\n"
"<p class=\"css-nowrap-test\">This-css-nowrap-line-also-never-wraps-despite-being-far-wider-than-the-screen</p>\n"
    "<p class=\"css-pre-test\">pre   keeps   spaces</p>\n"
    "<ul class=\"css-disc-test\"><li>Disc bullet item</li></ul>\n"
    "<ol class=\"css-roman-test\"><li>Roman one</li><li>Roman two</li></ol>\n"
    "<div class=\"css-box-test\"><p>Boxed paragraph (border + gray shade)</p></div>\n"
    "<p><a class=\"css-link-test\" href=\"https://example.com/css\">Link with printed URL</a></p>\n"
    "<table class=\"css-tab-test\"><tr><td>Bordered cell</td><td>Second cell</td></tr></table>\n"
    "<p class=\"css-size-test\">Big text via CSS (renders big)</p>\n"
    "<p class=\"css-indent-test\">Indented first line via CSS.</p>\n"
    "<ul class=\"css-first-test\"><li>First is bold</li><li>Rest normal</li></ul>\n"
    "<div class=\"css-flex-test\"><span>Flex row one</span><span>Flex row two</span></div>\n"
    "<p class=\"css-media-test\">Bold on narrow screens (media query held)</p>\n"
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
    /* Phase 10 end-event lanes (real dispatch proof): .jspop's declared
     * animation COMPLETES at the resolving walk → the browser fires
     * animationend at render completion (the entrance-anim hook, logo
     * style); the timer class flip on #jtrn mutates a transition-armed
     * element → transitionend. The counters guard exactly-once (a
     * re-fire would print 2). */
    "<style>\n"
    "@keyframes jspf { from { opacity: 0; } to { opacity: 1; } }\n"
    ".jspop { animation: jspf 0.25s; }\n"
    "#jtrn { transition: color 0.3s; }\n"
    ".jhot { color: green; }\n"
    "</style>\n"
    "<div class=\"jspop\" id=\"jpop\">P10 anim victim</div>\n"
    "<div id=\"jtrn\">P10 transition victim</div>\n"
    "<p id=\"jp10out\">P10 animend pending</p>\n"
    "<p id=\"jt10out\">P10 transend pending</p>\n"
    /* Phase 10 lanes, REGISTERED INLINE so they exist before the first
     * walk (the fresh parse runs inline scripts pre-walk; the ext suite
     * is async and would race the animation's single completion — the
     * browser has the same lag-vs-animation race, but our receipt needs
     * determinism). The listeners perform a classList.add after their
     * textContent write: the class mutation escalates domDirty (R58
     * parity), so the NEXT pump rewalk renders the write — otherwise a
     * render-completion write would sit invisible until an unrelated
     * mutation. Exactly-once: a re-fire would print the 2 text. */
    "<script>\n"
    "var np10 = 0, tp10 = 0;\n"
    "var jp10 = document.getElementById('jpop');\n"
    "var jt10 = document.getElementById('jtrn');\n"
    "if (jp10 && typeof jp10.addEventListener === 'function') {\n"
    "  jp10.addEventListener('animationend', function(){\n"
    "    np10 = np10 + 1;\n"
    "    var el = document.getElementById('jp10out');\n"
    "    if (el) {\n"
    "      el.textContent = 'PL10ANIMEND' + np10;\n"
    "      el.classList.add('lnote');\n"
    "    }\n"
    "  });\n"
    "}\n"
    "if (jt10 && typeof jt10.addEventListener === 'function') {\n"
    "  jt10.addEventListener('transitionend', function(){\n"
    "    tp10 = tp10 + 1;\n"
    "    var el = document.getElementById('jt10out');\n"
    "    if (el) {\n"
    "      el.textContent = 'PL10TRANS' + tp10;\n"
    "      el.classList.add('lnote');\n"
    "    }\n"
    "  });\n"
    "}\n"
    "setTimeout(function(){ if (jt10) jt10.classList.toggle('jhot'); }, 120);\n"
    "</script>\n"
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
    "  /* Phase 10: a MOUNTED animation — react's entrance-anim pattern.\n"
    "   * The element is created + appended HERE, so the walk that first\n"
    "   * resolves its declared run happens long after the listeners in\n"
    "   * the inline pre-walk script registered — the delivery race that\n"
    "   * swallowed the static victim's completion (the browser loses it\n"
    "   * the same way when the listener lags the animation) is\n"
    "   * structurally impossible for this receipt. A re-fire would print\n"
    "   * the 2 text. */\n"
    "  var np10m = 0;\n"
    "  var jp10m = document.createElement('div');\n"
    "  jp10m.setAttribute('class', 'jspop');\n"
    "  jp10m.addEventListener('animationend', function(){\n"
    "    np10m = np10m + 1;\n"
    "    var o = document.getElementById('jp10out');\n"
    "    if (o) {\n"
    "      o.textContent = 'PL10ANIMMOUNT' + np10m;\n"
    "      o.classList.add('lnote2');\n"
    "    }\n"
    "  });\n"
    "  jp10m.textContent = 'P10 mounted anim victim';\n"
    "  var oc10 = document.getElementById('out');\n"
    "  if (oc10) oc10.appendChild(jp10m);\n"
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

static const char CSSPROBE_HTML[] =
    /* ── about:cssprobe — CSS engine probe v2 (roadmap Phase 1) ────────────
     * Zero-JS fixture: every style input below is COUNTABLE, so the
     * [css-skip] row emitted at the end of this page's first render is the
     * assertion. Expected skip classes on THIS page (fixture-only facts,
     * post-Phase-7: the truth states (:target/:root/:disabled/:enabled/
     * :checked/:placeholder-shown/:empty) + the consumed ::-family left
     * the pcls counter (0); sib/attrOp are the phase-retired always-zero
     * columns — '+' combinators and attr operators PARSE since Phase 1,
     * the counters only ever counted refusals, of which the fixture's
     * truth-state selectors carry none now); the :root var adds one var:
     *   sib=0  attrOp=0  pcls=0  medF=1  var=4  calc=2  prop=2
     *   (prop = backdrop-filter, border-radius — position/top/left/
     *    right/bottom and z-index CONSUMED by the Phase 4 positioning
     *    lane; gap/grid-template-columns + the flex/grid distribution
     *    family CONSUMED by the Phase 6 container lane — all left the
     *    gauge)
     * [css-propnames] must list the four names above (P0.6 gauge).
     * Visible canaries (asserted by the PLUTO_NR_AUTOTEST block scanner):
     *   absent: ADJ-HIT GENSIB-HIT ATTCARET-HIT ATTDOLLAR-HIT ATTSTAR-HIT
     *           ATTTILDE-HIT ATTBAR-HIT NOT-HIT STATE-HIT
     *           CANARY-DISPLAY-NONE VAR-HIT VARFALL-HIT
     *           MQ-HIT MQEM-HIT MQAND-HIT MQOR-HIT MQCONTRAST-HIT
     *           AROWSTRAY ROOTVARHIT CHKHIT DAFTGONE
     *           EAFTEL-enabled-sibling PHELDGONE FITHIT EMPHIT
     *   present: ADJ-KEEP GENSIB-A ATTCARET-KEEP ATTDOLLAR-KEEP
     *            ATTSTAR-KEEP ATTTILDE-KEEP ATTBAR-KEEP NOT-KEEP
     *            STATE-KEEP VAR-KEEP MQ-KEEP MQNOT-KEEP MQCOLOR-KEEP
     *            MQUNKNOWN-KEEP ABSSTATIC-KEEP ZKEEP REL-KEEP
     *            SRKEEP OVKEEP ELLKEEP CLAMPKEEP
     *            FIXGAPA FIXGAPB FGAPC FGAPD FXROWA FAICC FAICD
     *            FXSB FXNA FXNC AINC
     *            (Phase 10 block canaries: ANIMLIFT completes pf — its
     *            block carries anim=pf; ANFALLDOWN/ANIMPAUSE/ANIMINF
     *            keep a NULL anim field — the registry/guard refusals;
     *            TRNVICTIM rides the direct transition query.)
     *   absent (add): FIXED-HIT ABS-HIT ABSOFF-HIT (out-of-flow + a
     *            signal drops; static-position absolute stays),
     *            SROFF-HIT SRTINY-HIT (the visually-hidden recipe:
     *            out-of-flow + overflow:hidden + a clip shape or a
     *            1px box), TAILSUFF CLAMPCUT (the ellipsis/clamp
     *            ...-tail cuts)
     * Direct-resolution rows (main.c winning lookups, Phase 2):
     *   calc-width=64px  var-display=none
     * Expected [css-skip] row: sib=0 attrOp=0 pcls=0 medF=1 var=3
     * calc=2 prop=2 (the census counts var/calc TEXT; the canaries
     * assert that those texts RESOLVE; medF=1 = the unknown-feature
     * refusal still contained by the MQUNKNOWN-KEEP marker; prop=2
     * after the Phase 4 positioning, the Phase 5 overflow vocabulary
     * and the Phase 6 flex/grid family became consumed — overflow/
     * clip/text-overflow-line-clamp and the gap/flex/grid names all
     * left the gauge; the Phase 10 transition/animation family also
     * left (all family NAMEs joined the census vocabulary, and the
     * @keyframes bodies never parse — they count nowhere), so the
     * fixture's multi-phase declarations here keep the count exact).
     * The page renders identically with JS execution off — used in every
     * mode of the browser, never mode-sensitive. */
    "<!DOCTYPE html><html><head><title>CSS Probe</title><style>\n"
    "/* C0 control — known vocabulary, zero skips expected: */\n"
    "body { margin: 4px; }\n"
    "h1 { text-indent: 2px; }\n"
    ".k { background-color: whitesmoke; }\n"
    "ul.l { list-style-type: none; float: left; width: 300px; }\n"
    "ul.l > li:first-child { text-indent: 8px; }\n"
    "li:nth-child(2n) { border-style: none; }\n"
    "p[hidden] { display: none; }\n"
    "div[data-k=\"v\"] { border-width: 2px; border-collapse: separate; }\n"
    ".bc::before { content: \">> \"; }\n"
    ".gone { display: none; }\n"
    "@media screen and (min-width: 240px) {\n"
    "  .m { display: block; }\n"
    "}\n"
    "/* P1a sibling combinators — NOW SUPPORTED (hits must hide):\n"
    " * h2 + p.adj = element DIRECTLY after an h2; li.gs ~ li = any later\n"
    " * sibling li of li.gs. Controls guard over-matching. */\n"
    "h2 + p.adj { display: none; }\n"
    "li.gs ~ li { display: none; }\n"
    "/* P1b attribute operators — NOW SUPPORTED (hits must hide):\n"
    " * ^= starts / $= ends / *= contains / ~= word / |= hyphen-prefix. */\n"
    "div[data-p^=\"ab\"] { display: none; }\n"
    "div[data-q$=\"ef\"] { display: none; }\n"
    "div[data-r*=\"cd\"] { display: none; }\n"
    "div[data-s~=\"two\"] { display: none; }\n"
    "div[data-t|=\"en\"] { display: none; }\n"
    "/* P1c simple :not() — NOW SUPPORTED (the hit must hide, the keep\n"
    " * sibling with .keep must stay). */\n"
    "p.nk:not(.keep) { display: none; }\n"
    "/* P1d interactivity states — policy (a) always-match (must hide): */\n"
    "p.st:hover { display: none; }\n"
    "/* Phase 3 @media evaluation — 400x240 panel truth: hits hide,\n"
    " * keeps stay, unknown features stay false (medF=1). The and-chain\n"
    " * canary carries a unitless zero (MQ4 exception) + landscape;\n"
    " * MQEM-HIT proves the em breakpoint lane (22.5em = 360px). */\n"
    "@media (min-width: 100px) { .mqhit { display: none; } }\n"
    "@media (min-width: 9000px) { .mqmiss { display: none; } }\n"
    "@media (min-width: 22.5em) { .mqem { display: none; } }\n"
    "@media screen and (min-width: 0) and (orientation: landscape)\n"
    "{ .mqand { display: none; } }\n"
    "@media not screen and (min-width: 1px)\n"
    "{ .mqnot { display: none; } }\n"
    "@media print, (orientation: landscape) { .mqor { display: none; } }\n"
    "@media (prefers-color-scheme: dark) { .mqcd { display: none; } }\n"
    "@media (prefers-contrast: more) { .mqc { display: none; } }\n"
    "@media (bugs: 3) { .mqu { display: none; } }\n"
    "/* Phase 4 positioning policy: fixed/absolute + offset(z-index) =\n"
    " * out-of-flow overlay → dropped (it would cover the page in a\n"
    " * compositor); absolute with a clean static position stays in\n"
    " * flow per CSS; relative keeps flowing; z-index alone is inert. */\n"
    ".fxo { position: fixed; top: 0; }\n"
    ".abz { position: absolute; z-index: 9; }\n"
    ".abp { position: absolute; left: -9999px; }\n"
    ".abs2 { position: absolute; }\n"
    ".z2 { z-index: 500; }\n"
    ".rel2 { position: relative; }\n"
    "/* Phase 5 overflow policy: the visually-hidden recipe (absolute +\n"
    " * overflow:hidden + a concealing clip shape OR a 1px box) drops;\n"
    " * partial patterns flow; text-overflow:ellipsis + nowrap cuts the\n"
    " * overflowing single line with a ... tail; -webkit-line-clamp: 3\n"
    " * caps the block at three lines the same way; plain overflow:\n"
    " * hidden at a normal size is a containment no-op that keeps every\n"
    " * word. */\n"
    ".sro { position: absolute; overflow: hidden; clip-path: inset(50%); }\n"
    ".srt { position: absolute; overflow: hidden; width: 1px; height: 1px; }\n"
    ".srb { position: absolute; overflow: hidden; }\n"
    ".ell { white-space: nowrap; text-overflow: ellipsis; }\n"
    ".cl3 { -webkit-line-clamp: 3; }\n"
    ".ovn { overflow: hidden; }\n"
    "/* Phase 6 flex/grid policy (css.h): the container lane. A flex/grid\n"
    " * box is a block container; gap mounts on the halved-px scale\n"
    " * (16px -> 8, Lua-parity margins) and rem rides the 16px browser\n"
    " * root; row-gap beats the gap shorthand; align-items:center\n"
    " * cross-centers child blocks on REAL flex containers only; the\n"
    " * distribution values (direction/wrap/justify/place/grid-templates)\n"
    " * are consumed names without 2-D behavior — the column axis is the\n"
    " * block flow itself. */\n"
    ".fxg { display: flex; gap: 16px; }\n"
    ".fxr { display: flex; gap: .5rem; }\n"
    ".fxw { display: flex; row-gap: 12px; }\n"
    ".fxa { display: flex; align-items: center; }\n"
    ".fxn { display: flex; }\n"
    ".fss { align-self: center; }\n"
    ".fxd { display: block; gap: 4px; }\n"
    ".aic { align-items: center; }\n"
    "/* Phase 7 pseudo-elements + truth states — the content cascade now\n"
    " * LANDS (walk_pseudo_emit allows punctuation INLINE, keeps the\n"
    " * stray-sibling guard for block subjects; walk_pseudo_text resolves\n"
    " * quotes + attr() + CSS escapes) and ::marker feeds the li bullet:\n"
    " *   .spi  span before+after inline (letters) -> PSBEF-SPMID-PSAFT\n"
    " *   .bcp  p before (letters, block-lane anon sibling) -> its own\n"
    " *         block starting \"BCARROW\" before the div's text\n"
    " *   .arw  block-lane pure punctuation -> the stray-line guard\n"
    " *         keeps it dropped (AROWSTRAY absent) while the div stays\n"
    " *   .beacon::after attr(data-url) -> \" [https://probe.example/beacon]\"\n"
    " *   .lmk li::marker \"MK7\" (the marker text rides the doc field,\n"
    " *         layout draws bold LRI) ; .lmr empty \"\" removes the bullet\n"
    " * Truth states: :root vars match the root element (RHIT gone via\n"
    " * var), :checked/:disabled/:enabled/:placeholder-shown/:focus-within\n"
    " * match their real control state (sibling text canaries), :target\n"
    " * matches the CURRENT fragment only (URL-less probe -> TGTHIT stays)\n"
    " * and :empty matches the truly-empty node (EMPHIT sibling hidden). */\n"
    ".spi::before { content: \"PSBEF-\"; }\n"
    ".spi::after { content: \"-PSAFT\"; }\n"
    ".bcp::before { content: \"BCARROW \"; }\n"
    ".arw::before { content: \">> \"; }\n"
    ".beacon::after { content: \" [\" attr(data-url) \"]\"; }\n"
    ".lmk li::marker { content: \"MK7\"; }\n"
    ".lmr li::marker { content: \"\"; }\n"
    ":root { --roothit: none; }\n"
    ".ph { display: var(--roothit); }\n"
    ".pk { display: block; }\n"
    ".ckr:checked + .ckl { display: none; }\n"
    ".ckr2:checked + .ckl2 { display: none; }\n"
    ".btnd:disabled + p { display: none; }\n"
    ".btne:enabled + .afte { display: none; }\n"
    "button.btnk:disabled + p { display: none; }\n"
    ".ip:placeholder-shown + .aftph { display: none; }\n"
    ".fwr:focus-within { display: none; }\n"
    "#tgx:target { display: none; }\n"
    ".ev:empty + p { display: none; }\n"
    ".ev2:empty + p { display: none; }\n"
    "/* Phase 9 transforms/gradients/units — translate mounts as a REAL\n"
    " * paint offset (DocBlock tf fields; % keys against the element's own\n"
    " * box at draw); rotate/scale stay the 2-D policy line; gradient\n"
    " * backgrounds decide ink polarity by stop average; the unit family\n"
    " * widened (vmin/vmax/ch/ex + rem margins through the shared reader).\n"
    " * ALL canary declarations ride KNOWN vocabulary on purpose — the\n"
    " * [css-skip] tally below stays byte-stable (no new refusals). */\n"
    ".tfx { transform: translateX(12px); }\n"
    ".tfy { transform: translateY(-0.75rem); }\n"
    ".tfp { transform: translate(10%, -50%); }\n"
    ".tfr { transform: rotate(45deg) translateX(4px); }\n"
    ".tfmix { transform: translateY(-50%); }\n"
    "@media (min-width: 120vmin) { .vminhit { display: none; } }\n"
    ".gradhero { background: linear-gradient(#050505, #232323);\n"
    "             color: #000; }\n"
    ".v { color: var(--ink, #111); font-size: 3rem; }\n"
    ".v2 { backdrop-filter: blur(2px); width: calc(100% - 8px); }\n"
    ".g { position: absolute; gap: 4px; border-radius: 4px;\n"
    "     grid-template-columns: repeat(3, 1fr); }\n"
    "/* Phase 2: custom properties + var()/calc() RESOLUTION —\n"
    " * the census above now counts var=3 calc=2 (the two new var()\n"
    " * display rules + the calc width). main.c asserts resolution\n"
    " * directly (calc-width=64px, var-display=none) and via hide\n"
    " * canaries: --hd rides a plain class (v1 note: custom properties\n"
    " * join the doc table in sheet order; :root is a refused\n"
    " * pseudo-class here). */\n"
    ".vroot { --hd: none; }\n"
    ".xfvar { display: var(--hd); }\n"
    ".xffall { display: var(--nope, none); }\n"
    ".vkeep { display: block; }\n"
    ".calcvictim { width: calc(30px + 34px); }\n"
    "/* Phase 10 animation/transition lanes — an animation NAME that the\n"
    " * @keyframes registry holds engages its end event at the walk:\n"
    " * .pop mounts anim=pf. The refusal windows engage NOTHING (browser\n"
    " * parity: an unregistered name is an inert animation; a paused\n"
    " * state never completes; an infinite iteration count never ends).\n"
    " * All declarations ride KNOWN vocabulary (the P10 family joined\n"
    " * the census tables) and the @keyframes body skips whole (never\n"
    " * parsed, never counted) — the [css-skip] tally stays exact. */\n"
    "@keyframes pf { from { opacity: 0; } to { opacity: 1; } }\n"
    ".pop { animation: pf 0.3s; }\n"
    ".popnf { animation: nokey 0.3s; }\n"
    ".poppa { animation: pf 0.3s paused; }\n"
    ".popinf { animation: pf 0.3s infinite; }\n"
    ".trnvictim { transition: color .3s; }\n"
    "</style></head><body>\n"
    "<h1 class=\"k\">CSS Probe</h1>\n"
    "<p class=\"k\">Phase-1 fixture — check the [css-skip] log row.</p>\n"
    "<p class=\"xfvar\">VAR-HIT must be absent (var resolves to none)</p>\n"
    "<p class=\"vkeep\">VAR-KEEP must stay visible</p>\n"
    "<p class=\"xffall\">VARFALL-HIT must be absent (fallback none)</p>\n"
    "<p>Plain paragraph stays visible (unaffected by the var sheet).</p>\n"
    "<div class=\"vroot\"></div>\n"
    "<p class=\"mqhit\">MQ-HIT must be absent (400 >= 100px)</p>\n"
    "<p class=\"mqmiss\">MQ-KEEP must stay visible (9000px)</p>\n"
    "<p class=\"mqem\">MQEM-HIT must be absent (22.5em = 360px)</p>\n"
    "<p class=\"mqand\">MQAND-HIT must be absent (0 + landscape)</p>\n"
    "<p class=\"mqnot\">MQNOT-KEEP must stay visible</p>\n"
    "<p class=\"mqor\">MQOR-HIT must be absent (comma OR)</p>\n"
    "<p class=\"mqcd\">MQCOLOR-KEEP must stay visible (light)</p>\n"
    "<p class=\"mqc\">MQCONTRAST-HIT must be absent (more)</p>\n"
    "<p class=\"mqu\">MQUNKNOWN-KEEP must stay visible</p>\n"
    "<p class=\"fxo\">FIXED-HIT must be absent (fixed + top)</p>\n"
    "<p class=\"abz\">ABS-HIT must be absent (absolute + z)</p>\n"
    "<p class=\"abp\">ABSOFF-HIT must be absent (left -9999px)</p>\n"
    "<p class=\"abs2\">ABSSTATIC-KEEP must stay visible</p>\n"
    "<p class=\"z2\">ZKEEP must stay visible (z alone inert)</p>\n"
    "<p class=\"rel2\">REL-KEEP must stay visible</p>\n"
    "<p class=\"sro\">SROFF-HIT must be absent (visually-hidden recipe)</p>\n"
    "<p class=\"srt\">SRTINY-HIT must be absent (1px box recipe)</p>\n"
    "<p class=\"srb\">SRKEEP must stay visible (no clip shape, no tiny box)</p>\n"
    "<p class=\"ovn\">OVKEEP must stay visible (containment no-op keeps every word)</p>\n"
    "<p class=\"ell\">ELLKEEP visible then a long run of filler words follows here so single nowrap line genuinely overflows the panel budget and the ellipsis cut lands well before TAILSUFF which must go</p>\n"
    "<p class=\"cl3\">CLAMPKEEP then filler so the block wraps past three lines: one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty twenty-one twenty-two twenty-three twenty-four twenty-five twenty-six twenty-seven twenty-eight twenty-nine thirty thirty-one thirty-two thirty-three thirty-four thirty-five thirty-six thirty-seven plus more words to be certain the cut reaches CLAMPCUT which must go</p>\n"
    "<div class=\"fxg\"><div>FIXGAPA child one</div><div>FIXGAPB child two</div></div>\n"
    "<div class=\"fxr\"><div>FGAPC rem child one</div><div>FGAPD rem child two</div></div>\n"
    "<div class=\"fxw\"><div>FXROWA longhand one</div><div>FXROWB longhand two</div></div>\n"
    "<div class=\"fxa\"><div>FAICC centered one</div><div>FAICD centered two</div></div>\n"
    "<div class=\"fxn\"><div>FXNA plain one</div><div class=\"fss\">FXSB self-centered two</div><div>FXNC plain three</div></div>\n"
    "<div class=\"fxd\"><div>FGNPX gap-inert child</div></div>\n"
    "<div class=\"aic\"><div>AINC noflex center child</div></div>\n"
    "<span class=\"spi\">SPMID</span>\n"
    "<p class=\"bcp\">BCBODY arrow-prefixed paragraph</p>\n"
    "<p class=\"beacon\" data-url=\"https://probe.example/beacon\">BEACON attr link</p>\n"
    "<div class=\"arw\">ARWDROP stray-guard keeps this div</div>\n"
    "<ul class=\"lmk\"><li>MARKORD bullet text</li></ul>\n"
    "<ul class=\"lmr\"><li>REMPT bullet-less item</li></ul>\n"
    "<p class=\"ph\">ROOTVARHIT root-var display</p>\n"
    "<p class=\"pk\">ROOTKEEP root-var keep</p>\n"
    "<div class=\"ckwrap\"><input type=\"checkbox\" class=\"ckr\" checked><label class=\"ckl\">CHKHIT checked-label</label><input type=\"checkbox\" class=\"ckr2\"><label class=\"ckl2\">CHKKEEP unchecked-label</label></div>\n"
    "<button class=\"btnd\" disabled>BTNGONE</button>\n"
    "<p>DAFTGONE button-disabled sibling</p>\n"
    "<button class=\"btne\">EAFTEL</button>\n"
    "<p class=\"afte\">EAFTEL enabled-sibling</p>\n"
    "<button class=\"btnk\">BSTKEEP</button>\n"
    "<p>DSTKEEP enabled-boundary sibling</p>\n"
    "<input type=\"text\" class=\"ip\" placeholder=\"PLHELD text\">\n"
    "<p class=\"aftph\">PHELDGONE placeholder sibling</p>\n"
    "<div class=\"fwr\"><p>FITHIT focus-within child</p></div>\n"
    "<p id=\"tgx\">TGTHIT target-less page keeps</p>\n"
    "<p class=\"ev\"></p>\n"
    "<p>EMPHIT empty-sibling</p>\n"
    "<p class=\"ev2\">EV2NOTEMPTY</p>\n"
    "<p>EKKEEP nonempty-sibling</p>\n"
    /* P9 HTML markers — transform mounts read as DOC fields (tfXpx/
     * tfYpx/tfXpct/tfYpct), the gradient hero asserts its invert flag,
     * the media query consumes 120vmin (VMINHIT hides), and the margin
     * unit repair shows as real spacing (2rem top = 16; 50% left = 0,
     * the legacy "%-as-25px" wrong turn gone). */
    "<p class=\"tfx\">TFSHIFTX x-shift</p>\n"
    "<p class=\"tfy\">TFSHIFTY y-shift</p>\n"
    "<p class=\"tfp\">TFSHIFT pct pair</p>\n"
    "<p class=\"tfr\">TFSHIFTR rotate composed</p>\n"
    "<p class=\"tfmix\" style=\"transform: translateX(6px)\">TFSHIFTWIN inline beats sheet</p>\n"
    "<p class=\"vminhit\">VMINHIT must be absent (120vmin = 288px)</p>\n"
    "<div class=\"gradhero\">GRADHERO dark gradient inverts</div>\n"
    "<p style=\"margin-top:2rem\">MRG2REM rem margin (16 half-px top)</p>\n"
    "<p style=\"margin-left:50%\">MRG50PCT refuses at walk (0 left)</p>\n"
    /* P10 HTML markers — the engaged animation mount carries its
     * registry name as a block field (an battery below); the never-
     * completing windows mount nothing (the registry/guard refusals);
     * the transition victim asserts via the direct winner query —
     * no text can express an arm (see the Phase 2 rows). */
    "<div class=\"pop\">ANIMLIFT completes pf</div>\n"
    "<div class=\"popnf\">ANFALLDOWN unregistered name</div>\n"
    "<div class=\"poppa\">ANIMPAUSE paused holds open</div>\n"
    "<div class=\"popinf\">ANIMINF infinite never ends</div>\n"
    "<div class=\"trnvictim\">TRNVICTIM transition armed</div>\n"
    /* P8 HTML interactive set — details/dialog/template/srcset/select/
     * reset. Mostly NO CSS declarations: the [css-skip] tally below
     * asserts EXACT counts (pcls/medF/var/calc/prop), so the P8
     * canaries stay HTML-only to keep that battery byte-stable */
    "<h2>Phase 8 interactive set</h2>\n"
    "<template><p>TMPLJUNK template body must be absent</p></template>\n"
    "<p>TMPLKEEP template-follow keeps</p>\n"
    "<dialog><p>DLGCLOSED must be absent (no open attr)</p></dialog>\n"
    "<dialog open><p>DLGOPEN open dialog prints</p></dialog>\n"
    "<details><p>DETBODY closed skip keeps hiding</p></details>\n"
    "<img src=\"https://probe.example/plain.png\" alt=\"PLAINIMG baseline\">\n"
    "<img src=\"https://probe.example/small.png\" srcset=\"https://probe.example/one.png 200w, https://probe.example/two.png 900w, https://probe.example/three.png 480w\" alt=\"SRCSETW width pick\">\n"
    "<img src=\"https://probe.example/van.png\" srcset=\"https://probe.example/van@1x.png 1x, https://probe.example/van@3x.png 3x, https://probe.example/van@1.5x.png 1.5x\" alt=\"SRCSETD density pick\">\n"
    "<img src=\"https://probe.example/only.png\" srcset=\"\" alt=\"SRCSETF unusable keeps src\">\n"
    "<form action=\"#f8\" method=\"get\"><input type=\"text\" name=\"rn\" value=\"RV0\"><button type=\"reset\">RNRESET</button><input type=\"hidden\" name=\"hn\" value=\"HV0\"></form>\n"
    "<form action=\"#g8\" method=\"get\"><select name=\"msel\" multiple><option value=\"a\" selected>MS1</option><option value=\"b\">MS2</option><option value=\"c\" selected>MS3</option></select><button>FMBTN</button></form>\n"
    "<h2>Adjacent sibling</h2>\n"
    "<p class=\"adj\">ADJ-HIT must be absent</p>\n"
    "<div>ADJ-KEEP must stay visible (follows a div, not an h2)</div>\n"
    "<h2>General sibling</h2>\n"
    "<ul class=\"l\">\n"
    "  <li>GENSIB-A must stay visible (before li.gs)</li>\n"
    "  <li class=\"gs\">GENSIB sandbox entry</li>\n"
    "  <li>GENSIB-HIT must be absent</li>\n"
    "</ul>\n"
    "<h2>Attribute operators</h2>\n"
    "<div data-p=\"abcdef\">ATTCARET-HIT must be absent (^=</div>\n"
    "<div data-p=\"xabc\">ATTCARET-KEEP must stay visible</div>\n"
    "<div data-q=\"abcdef\">ATTDOLLAR-HIT must be absent ($=</div>\n"
    "<div data-q=\"abcx\">ATTDOLLAR-KEEP must stay visible</div>\n"
    "<div data-r=\"abcdefgh\">ATTSTAR-HIT must be absent (*=</div>\n"
    "<div data-r=\"ab_efgh\">ATTSTAR-KEEP must stay visible</div>\n"
    "<div data-s=\"one two three\">ATTTILDE-HIT must be absent (~=</div>\n"
    "<div data-s=\"onetwo\">ATTTILDE-KEEP must stay visible</div>\n"
    "<div data-t=\"en-US\">ATTBAR-HIT must be absent (|=</div>\n"
    "<div data-t=\"ens\">ATTBAR-KEEP must stay visible</div>\n"
    "<h2>Simple :not</h2>\n"
    "<p class=\"nk\">NOT-HIT must be absent</p>\n"
    "<p class=\"nk keep\">NOT-KEEP must stay visible</p>\n"
    "<h2>State pseudo-classes</h2>\n"
    "<p class=\"st\">STATE-HIT must be absent (:hover always-match)</p>\n"
    "<p class=\"st2\">STATE-KEEP must stay visible</p>\n"
    "<div class=\"bc\">CANARY-BEFORE (inline prints; stray block-lane guard keeps it off its own line)</div>\n"
    "<div class=\"m\">media block (min-width:240px valid feature)</div>\n"
    "<p class=\"gone\">CANARY-DISPLAY-NONE must be absent</p>\n"
    "<h2>Value/planned-gap sections</h2>\n"
    "<p class=\"v\">var + font-size text</p>\n"
    "<p class=\"v2\">calc + backdrop-filter text</p>\n"
    "<p class=\"g\">position/gap/border-radius/grid text</p>\n"
    "<p>[css-skip] expected: sib=0 attrOp=0 pcls=0 medF=1 var=4 calc=2 prop=2</p>\n"
    "</body></html>";

/* ── about:apiprobe — engine browser-API capability probe (API program) ──
 * Engine-driven capability farm: the inline script runs the battery through
 * the LIVE bridge on whatever engine serves the page, appends one row per
 * capability ("name: PASS|MISS") plus the summary line "apisum: pass=N
 * miss=M" (the API mirror of the [css-skip] census tally). P0 = the
 * baseline run: rows record what exists TODAY. Later API phases flip their
 * named row to an assertion as the lane lands — P1 adds submit/input/
 * change + bubbling rows, P2 vets the fetch shim ([js] fetch shim:
 * injecting — infrastructure already present) and turns the fetch row
 * functional, P3 location/history, P4 console, P5 module rows, P6
 * template/closest/dataset. Zero network dependence.
 * API-P2 (closed): the Fetch receipts battery below turns fetch into
 * full receipts on LOCALLY SERVED pages — about: refs answer from the
 * internal page store through the SAME xhr path a network page uses,
 * settling 200 without a socket (device parity). Rows: fetchbody
 * (fetch about:apiprobe → .ok true/.status 200 → text contains the page
 * marker), fetchjsonreject (r.json() must reject on non-JSON HTML = the
 * rejection lane works), postct (POST + a setRequestHeader
 * content-type override settles ok — on real wire POSTs the override
 * rides http_request_ct), csasync (document.currentScript is NULL
 * inside a settled-promise callback = browser-faithful context
 * clearing; A2 fills currentScript only during a script run). The
 * battery paints "apifetch: pass=N miss=M" after all four rows settle;
 * a shim-less engine prints the fail-side sum immediately. Recorded
 * gap (unchanged): appendChild returns falsy instead of the node —
 * the DOM-conveniences phase's item. API-P1: the
 * bridge's appendChild returns a falsy value rather than the appended
 * node (MDN shape) — noted for the DOM-conveniences phase. API-P1: the
 * Events battery below adds a live form fixture (#evf/#evi/#evb) whose
 * JS-side counters paint "apisev: input=N change=N submit=N bub=N btn=N
 * doc=N" — input/change on the field, click on the button, submit with
 * preventDefault() on the form, and document-level submit/click rows that
 * only count if the dispatched event BUBBLES past the target. The
 * [ev-test] sim driver in main.c types one commit and presses the button
 * through the real tap pipeline, then asserts the counters ≥ 1 each and
 * that the doc pointer never changed (pd honored → live re-walk, not a
 * reload). API-P3 (closed): the Address journal battery below drives
 * the engine's address book end-to-end WITHOUT any network navigation —
 * pushState/replaceState enter their entries through the in-engine
 * journal (fragment/query/opaque-about joins resolve browser-faithfully;
 * cross-origin heads are refused), the LIVE location getters re-derive
 * href/hash/search per read, back/forward walk the recorded stack, and
 * popstate (with the per-entry state) + hashchange (with oldURL/newURL)
 * deliver from the journal at the next pump frame. Rows: histpush
 * (#join, state pin, length 2), histreplace (query swap, length stays,
 * old state replaced), histhashtop (stack grows to 3), docsettitle
 * (document.title set/get lane), histback (journal back → the replaced
 * entry's state p=12, popstate fired once carrying it, hashchange
 * fired once for the "#h9"→"" hop), histbase (back to the seeded doc
 * base, state null), histforward (forward re-enters the recorded
 * entry, popstate again). Paints "apihist: pass=N miss=M" after all
 * seven rows settle. pushState NEVER touches the wire: no http_* call,
 * no app address-bar change — the journal feeds location reads and the
 * back-stack events only.
 * API-P4 (closed): the Console receipts battery drives the real log
 * lane end-to-end — every send lands a tagged [console.*] row in the
 * FILE (the receipt surface; the rendered panel never changes), with
 * printf substitution (%s/%d/%o/%% + an out-of-args token printing
 * literally) and the console-shape renderer for objects/arrays
 * ('[a, b, …n]' / '{k: v, …}', depth-2 bounded). assert = browser
 * semantics: falsey → an ERROR-level 'Assertion failed: …' row that
 * carries the substitution, truthy → SILENT (the battery's
 * P4NOASSERT exercise). consoclean proves the panel-clean half
 * (out.children.length delta ZERO across the storm), and the battery's
 * own `P4COUNT` send prints the JS-side sent counter — the log's
 * console-row count vs it MATCH (17 sends = 16 rows + the documented
 * silent assert), which is the phase's done-when. Paints
 * "apicon: pass=N miss=M sent=S".
 * API-P5 (closed): the Module receipts battery — <script type="module">
 * rides the loader-side lane: inline modules run (modin), static
 * imports resolve through url_resolve against the document base and
 * fetch from the internal page store (modabs = named bindings out of
 * about:p5m0; modstar = the *-import namespace object of about:p5m1);
 * P10 note: the store fixtures stay on the const/let/function forms —
 * the LIST export form (`export {name, name as alias}`) materializes
 * since P10 (aliases included, `export {list} from` still refuses),
 * receipts in jsbridge.c.
 * and the graph runs import-FIRST in declaration order (children load
 * before the body — the receipts assert both values landed). The
 * refusal receipt (modrefuse): a path-only './p5x.js' import on this
 * OPAQUE about: base must NOT run its module (the browser's own
 * module-specifier refusal — the journal rule, kept faithful). The
 * closing classic script counts outm's children: exactly 3 = the modin
 * marker + the two PASS rows (a leak child or a missing row breaks the
 * count). Paints "apimod: kids=K refuse=PASS|FAIL".
 * API-P10 (closed): the async lane battery adds four module scripts —
 * moddyn (dynamic import('about:p5m0'): the promise resolves with the
 * namespace, asserts P5A==41 + the function + P5C; moddynid rides in the
 * same module: a SECOND import( of the SAME key must return the SAME
 * namespace object (===)). modmiss (import('about:p5miss') REJECTS with
 * the browser message class "Failed to fetch dynamically imported
 * module: <spec>" — the assertion is the rejection text). modnet
 * (import() of the PINNED https CDN module punycode@2.1.1 — fetched the
 * module lane (router transport, single-flight), drained through the
 * standing transform where the P10 LIST-EXPORT form
 * (export {local, local as exported}) materializes the 6 named exports,
 * then asserts encode/decode real; network-failed → assert the promise
 * REJECTED instead: conditional-PASS either way, the log branch
 * "[apimod] modnet: fetched/rejected/no dynamic-import path documents
 * which). The row helpers (apimodWrite/apimodRecount) live on
 * globalThis — module scripts have separate scopes, and a throw inside
 * a promise handler rejects the .then chain itself (an honest unhandled
 * rejection), so the writers are globals. Frozen engines feature-detect
 * the import path (no __p5dynimport shim or no Promise class) and write
 * honest MISS rows instead of throwing (kids=5 refuse=FAIL on mujs/duk;
 * kids=7 refuse=PASS on XS). The closing classic script recounts: 7 =
 * the modin marker + modabs + modstar + the four async rows (a leak
 * child or a missing row breaks the count).
 * API-P6 (closed): the DOM-conveniences battery adds six rows —
 * tplparsed/tplinert/tplcreated (template.content: template children
 * parse into a DETACHED fragment, parent-NULL, invisible to
 * getElementById; created templates materialize an empty fragment),
 * closest (self + ancestor climb, miss ⇒ null), dataset (camelCase
 * snapshot, live read-back), appendret (appendChild/removeChild return
 * the node per MDN). Paints "apidom: pass=N miss=M" and lifts the P0
 * template row out of PASS-miss bookkeeping (apisum 12/2).
 * API-P9 (closed): the parked pair lands. The classnames row (seventh
 * row; 6->7) exercises the className accessor end to end — setter +
 * read-back, the getAttribute('class') mirror, a rewrite and the
 * setAttribute reverse-mirror — closing the P6 parked gap (the battery
 * no longer needs the setAttribute detour). The closest/appendret rows
 * double as the wrapper-identity gate on every engine: the one-wrapper
 * per-node cache (browser identity model) makes their `===` chains PASS
 * on mujs/duktape too (they MISSed with fresh wrappers per access
 * before P9; XS NR held its own per-node cache since P1).
 * API-P7 (closed): the storage row became a full receipts battery on
 * the shared webstorage road — localStorage/sessionStorage roundtrip +
 * remove→null + a quota subtest (a 5120-char construction must throw
 * QuotaExceededError and store NOTHING: the catch paints "[p7q]" into
 * the log and the row passes only when the value stayed absent) + an
 * UNPAINTED persistent counter: "visits" increments localStorage on
 * every probe boot and prints "[p7] store: visits=N local=L session=S"
 * to the log (not the canvas, so snapshots stay stable) — the live
 * proof of cross-boot persistence through the Data-sandbox file. The
 * canvas row itself stays one in-place line: no block shifts.
 * API-P8 (closed): the worker row became the contained-stub lane —
 * the Worker constructor SHIPS (a missing constructor would kill the
 * calling script on a ReferenceError) and every construction behaves
 * like a worker whose script always fails to start: the row constructs
 * 'workerstub.js', asserts the shape-complete object (typeof the
 * postMessage/terminate members === 'function', onerror/onmessage
 * null), assigns an onerror that logs "[p8w] worker onerror: <msg>"
 * when the stub tells the page at the NEXT pump frame with
 * ev = {message}, and postMessages 'ping' (absorbed — no thread). The
 * worker script NEVER compiles, is never fetched, never executes; the
 * battery rows add no terminate here, so the delivery receipt lands
 * after the row (browsers: terminate-before-load-failure silences the
 * error). Paints "worker: PASS"; apisum reaches 14/0 on XS (the full
 * P0 battery), 10/4 on mujs/duk (their documented standing rows). */
static const char APIPROBE_HTML[] =
    "<!DOCTYPE html><html><head><title>API Probe</title><style>\n"
    "body { background: white; color: black; }\n"
    "h1 { font-size: 2em; }\n"
    ".sum { font-weight: bold; }\n"
    "</style></head><body>\n"
    "<h1>API Probe</h1>\n"
    "<p>Engine browser-API capability battery — P0 baseline run.</p>\n"
    "<p class=\"sum\" id=\"apisum\">apisum: pending</p>\n"
    "<div id=\"out\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('out');\n"
    "  var sumEl = document.getElementById('apisum');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var npass = 0, nmiss = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    if (verdict === 'PASS') { npass = npass + 1; } else { nmiss = nmiss + 1; }\n"
    "  }\n"
    "  function iscallable(v) { return typeof v === 'function'; }\n"
    "  try { row('domwrite', (function () { var p2 = document.createElement('p'); p2.textContent = 'ok'; return p2 && p2.textContent === 'ok'; })() ? 'PASS' : 'MISS'); } catch (e) { row('domwrite', 'MISS'); }\n"
    "  try { row('title', document.title === 'API Probe' ? 'PASS' : 'MISS'); } catch (e) { row('title', 'MISS'); }\n"
    "  try { row('timers', iscallable(setTimeout) && iscallable(setInterval) && iscallable(clearTimeout) ? 'PASS' : 'MISS'); } catch (e) { row('timers', 'MISS'); }\n"
    "  try { localStorage.setItem('apiprobe', '1'); var st = localStorage.getItem('apiprobe'); localStorage.removeItem('apiprobe'); var gone = localStorage.getItem('apiprobe'); sessionStorage.setItem('apiseed', '2'); var sst = sessionStorage.getItem('apiseed'); sessionStorage.removeItem('apiseed'); var caught = false; try { var big = 'xxxxxxxxxx'; big = big + big; big = big + big; big = big + big; big = big + big; big = big + big; big = big + big; big = big + big; big = big + big; big = big + big; localStorage.setItem('biggie', big); localStorage.removeItem('biggie'); } catch (qe) { console.log('[p7q] ' + (qe && qe.message ? qe.message : qe)); caught = String(qe && qe.message ? qe.message : qe).indexOf('Quota') !== -1; } var ok = typeof localStorage !== 'undefined' && typeof sessionStorage !== 'undefined' && String(st) === '1' && gone === null && String(sst) === '2' && caught && localStorage.getItem('biggie') === null; if (ok) { var pv = parseInt(localStorage.getItem('p7v'), 10); if (isNaN(pv) || pv < 0) { pv = 0; } pv = pv + 1; localStorage.setItem('p7v', String(pv)); console.log('[p7] store: visits=' + pv + ' local=' + localStorage.length + ' session=' + sessionStorage.length); } row('storage', ok ? 'PASS' : 'MISS'); } catch (e) { row('storage', 'MISS'); }\n"
    "  try { row('xhr', typeof XMLHttpRequest !== 'undefined' && iscallable(XMLHttpRequest) ? 'PASS' : 'MISS'); } catch (e) { row('xhr', 'MISS'); }\n"
    "  try { row('listeners', document.addEventListener && iscallable(document.addEventListener) ? 'PASS' : 'MISS'); } catch (e) { row('listeners', 'MISS'); }\n"
    "  try { row('promise', typeof Promise !== 'undefined' && iscallable(Promise) ? 'PASS' : 'MISS'); } catch (e) { row('promise', 'MISS'); }\n"
    "  try { row('fetch', typeof fetch !== 'undefined' && iscallable(fetch) ? 'PASS' : 'MISS'); } catch (e) { row('fetch', 'MISS'); }\n"
    "  try { row('console', typeof console !== 'undefined' && console.log && iscallable(console.log) ? 'PASS' : 'MISS'); } catch (e) { row('console', 'MISS'); }\n"
    "  try { var wok = false, wsh = ''; try { var wk = new Worker('workerstub.js'); wok = true; wsh = (typeof wk === 'object' && typeof wk.postMessage === 'function' && typeof wk.terminate === 'function' && wk.onerror === null && wk.onmessage === null) ? 'ok' : 'shape'; wk.onerror = function (evw) { console.log('[p8w] worker onerror: ' + (evw && evw.message ? evw.message : evw)); }; wk.postMessage('ping'); } catch (we) { wsh = 'ctor:' + String(we && we.message ? we.message : we); } row('worker', (typeof Worker !== 'undefined' && iscallable(Worker) && wok && wsh === 'ok') ? 'PASS' : 'MISS'); } catch (e) { row('worker', 'MISS'); }\n"
    "  try { row('location', typeof location !== 'undefined' ? 'PASS' : 'MISS'); } catch (e) { row('location', 'MISS'); }\n"
    "  try { row('history', typeof history !== 'undefined' ? 'PASS' : 'MISS'); } catch (e) { row('history', 'MISS'); }\n"
    "  try { var tpl = document.createElement('template'); row('template', tpl && tpl.content ? 'PASS' : 'MISS'); } catch (e) { row('template', 'MISS'); }\n"
    "  try { row('currentscript', (function () { var cs = document.currentScript; return !!cs && typeof cs.getAttribute === 'function' && cs.getAttribute('src') === null; })() ? 'PASS' : 'MISS'); } catch (e) { row('currentscript', 'MISS'); }\n"
    "  sumEl.textContent = 'apisum: pass=' + npass + ' miss=' + nmiss;\n"
    "})();\n"
    "</script>\n"
    "<h2>Events battery</h2>\n"
    "<form id=\"evf\">\n"
    "  <input id=\"evi\" type=\"text\" name=\"ev\" value=\"seed\">\n"
    "  <button id=\"evb\" type=\"submit\">Send</button>\n"
    "</form>\n"
    "<p class=\"sum\" id=\"apisev\">apisev: pending</p>\n"
    "<div id=\"outev\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var form = document.getElementById('evf');\n"
    "  var field = document.getElementById('evi');\n"
    "  var btn = document.getElementById('evb');\n"
    "  var out = document.getElementById('outev');\n"
    "  var sumEl = document.getElementById('apisev');\n"
    "  if (!form || !field || !btn || !out || !sumEl) { return; }\n"
    "  var nin = 0, nchg = 0, nsub = 0, nbub = 0, nbtn = 0, ndoc = 0;\n"
    "  function paint(tag) {\n"
    "    sumEl.textContent = 'apisev: input=' + nin + ' change=' + nchg + ' submit=' + nsub + ' bub=' + nbub + ' btn=' + nbtn + ' doc=' + ndoc;\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = 'ev:' + tag;\n"
    "    out.appendChild(d);\n"
    "  }\n"
    "  field.addEventListener('input', function () { nin = nin + 1; paint('input'); });\n"
    "  field.addEventListener('change', function () { nchg = nchg + 1; paint('change'); });\n"
    "  btn.addEventListener('click', function () { nbtn = nbtn + 1; paint('click'); });\n"
    "  form.addEventListener('submit', function (e) { nsub = nsub + 1; if (e && e.preventDefault) { e.preventDefault(); } paint('submit'); });\n"
    "  document.addEventListener('submit', function () { nbub = nbub + 1; paint('bub'); });\n"
    "  document.addEventListener('click', function () { ndoc = ndoc + 1; paint('docclick'); });\n"
    "})();\n"
    "</script>\n"
    "<h2>Fetch receipts</h2>\n"
    "<p class=\"sum\" id=\"apifetch\">apifetch: pending</p>\n"
    "<div id=\"outf\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('outf');\n"
    "  var sumEl = document.getElementById('apifetch');\n"
    "  if (!out || !sumEl || typeof fetch !== 'function') { sumEl.textContent = 'apifetch: pass=0 miss=4'; return; }\n"
    "  var npass = 0, nmiss = 0, ndone = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    if (verdict === 'PASS') { npass = npass + 1; } else { nmiss = nmiss + 1; }\n"
    "    ndone = ndone + 1;\n"
    "    if (ndone >= 4) { sumEl.textContent = 'apifetch: pass=' + npass + ' miss=' + nmiss; }\n"
    "  }\n"
    "  fetch('about:apiprobe').then(function (r) {\n"
    "    if (r && r.ok === true && r.status === 200) {\n"
    "      return r.text().then(function (t) {\n"
    "        row('fetchbody', t && t.length > 100 && t.indexOf('API Probe') >= 0 ? 'PASS' : 'MISS');\n"
    "      });\n"
    "    }\n"
    "    row('fetchbody', 'MISS');\n"
    "  }).catch(function (e) { row('fetchbody', 'MISS'); });\n"
    "  fetch('about:cssprobe').then(function (r) {\n"
    "    return r.json().then(function () { row('fetchjsonreject', 'MISS'); }, function () { row('fetchjsonreject', 'PASS'); });\n"
    "  }).catch(function () { row('fetchjsonreject', 'PASS'); });\n"
    "  fetch('about:blank', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: 'a=1&b=2' }).then(function (r) { row('postct', r && r.ok ? 'PASS' : 'MISS'); }).catch(function () { row('postct', 'MISS'); });\n"
    "  fetch('about:blank').then(function () { row('csasync', document.currentScript === null ? 'PASS' : 'MISS'); }).catch(function () { row('csasync', 'MISS'); });\n"
    "})();\n"
    "</script>\n"
    "<h2>Address journal</h2>\n"
    "<p class=\"sum\" id=\"apihist\">apihist: pending</p>\n"
    "<div id=\"outh\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('outh');\n"
    "  var sumEl = document.getElementById('apihist');\n"
    "  if (!out || !sumEl || !history || !history.pushState) { sumEl.textContent = 'apihist: pass=0 miss=7'; return; }\n"
    "  var npass = 0, nmiss = 0, ndone = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    if (verdict === 'PASS') { npass = npass + 1; } else { nmiss = nmiss + 1; }\n"
    "    ndone = ndone + 1;\n"
    "    if (ndone >= 7) { sumEl.textContent = 'apihist: pass=' + npass + ' miss=' + nmiss; }\n"
    "  }\n"
    "  var pops = 0, hashchg = 0, lastPopState = 'no-event';\n"
    "  try { window.addEventListener('popstate', function (e) { pops = pops + 1; lastPopState = (e && e.state && typeof e.state.p === 'number') ? String(e.state.p) : 'bad'; }); } catch (e0) {}\n"
    "  try { window.addEventListener('hashchange', function (e) { hashchg = hashchg + 1; }); } catch (e1) {}\n"
    "  var t0 = 'ok';\n"
    "  try {\n"
    "    history.pushState({ p: 11 }, '', '#h11');\n"
    "    if (location.hash !== '#h11') t0 = 'bad';\n"
    "    if (location.href.indexOf('#h11') < 0) t0 = 'bad';\n"
    "    if (!history.state || history.state.p !== 11) t0 = 'bad';\n"
    "    if (history.length !== 2) t0 = 'bad';\n"
    "  } catch (e) { t0 = 'thr'; }\n"
    "  row('histpush', t0 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t1 = 'ok';\n"
    "  try {\n"
    "    history.replaceState({ p: 12 }, '', '?a=2');\n"
    "    if (location.search !== '?a=2') t1 = 'bad';\n"
    "    if (location.hash !== '') t1 = 'bad';\n"
    "    if (!history.state || history.state.p !== 12) t1 = 'bad';\n"
    "    if (history.length !== 2) t1 = 'bad';\n"
    "  } catch (e) { t1 = 'thr'; }\n"
    "  row('histreplace', t1 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t2 = 'ok';\n"
    "  try {\n"
    "    history.pushState({ p: 13 }, '', '#h9');\n"
    "    if (location.hash !== '#h9') t2 = 'bad';\n"
    "    if (location.search !== '?a=2') t2 = 'bad';\n"
    "    if (history.length !== 3) t2 = 'bad';\n"
    "  } catch (e) { t2 = 'thr'; }\n"
    "  row('histhashtop', t2 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t3 = 'ok';\n"
    "  try { document.title = 'Pluto J3'; if (document.title !== 'Pluto J3') t3 = 'bad'; } catch (e) { t3 = 'thr'; }\n"
    "  row('docsettitle', t3 === 'ok' ? 'PASS' : 'MISS');\n"
    "  try { history.back(); } catch (e) {}\n"
    "  setTimeout(function () {\n"
    "    var t4 = 'ok';\n"
    "    try {\n"
    "      if (location.search !== '?a=2') t4 = 'bad';\n"
    "      if (location.hash !== '') t4 = 'bad';\n"
    "      if (!history.state || history.state.p !== 12) t4 = 'bad';\n"
    "      if (pops !== 1) t4 = 'bad';\n"
    "      if (lastPopState !== '12') t4 = 'bad';\n"
    "      if (hashchg !== 1) t4 = 'bad';\n"
    "    } catch (e) { t4 = 'thr'; }\n"
    "    row('histback', t4 === 'ok' ? 'PASS' : 'MISS');\n"
    "    try { history.back(); } catch (e2) {}\n"
    "    setTimeout(function () {\n"
    "      var t5 = 'ok';\n"
    "      try {\n"
    "        if (location.href.indexOf('about:apiprobe') !== 0) t5 = 'bad';\n"
    "        if (location.search !== '') t5 = 'bad';\n"
    "        if (location.href.indexOf('?a=2') >= 0) t5 = 'bad';\n"
    "        if (history.state !== null) t5 = 'bad';\n"
    "        if (pops < 2) t5 = 'bad';\n"
    "      } catch (e) { t5 = 'thr'; }\n"
    "      row('histbase', t5 === 'ok' ? 'PASS' : 'MISS');\n"
    "      try { history.forward(); } catch (e3) {}\n"
    "      setTimeout(function () {\n"
    "        var t6 = 'ok';\n"
    "        try {\n"
    "          if (location.search !== '?a=2') t6 = 'bad';\n"
    "          if (pops < 3) t6 = 'bad';\n"
    "        } catch (e) { t6 = 'thr'; }\n"
    "        row('histforward', t6 === 'ok' ? 'PASS' : 'MISS');\n"
    "      }, 90);\n"
    "    }, 90);\n"
    "  }, 90);\n"
    "})();\n"
    "</script>\n"
    "<h2>Console receipts</h2>\n"
    "<p class=\"sum\" id=\"apicon\">apicon: pending</p>\n"
    "<div id=\"outc\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('outc');\n"
    "  var sumEl = document.getElementById('apicon');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var npass = 0, nmiss = 0, sent = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    if (verdict === 'PASS') { npass = npass + 1; } else { nmiss = nmiss + 1; }\n"
    "  }\n"
    "  var t0 = 'ok';\n"
    "  try {\n"
    "    console.log('P4LOG1 %s body', 'str'); sent = sent + 1;\n"
    "    console.log('P4LOG2 %d nums', 42); sent = sent + 1;\n"
    "    console.log('P4LOG3 %o shapes', { a: 1, b: [7, 8, 9] }); sent = sent + 1;\n"
    "    console.log('P4LOG4 100%% literal'); sent = sent + 1;\n"
    "    console.log('P4LOG5', [1, 2, 3, 4, 5]); sent = sent + 1;\n"
    "  } catch (e) { t0 = 'thr'; }\n"
    "  row('consolog', t0 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t1 = 'ok';\n"
    "  try {\n"
    "    console.warn('P4WARN1 %s level', 'warn'); sent = sent + 1;\n"
    "    console.warn('P4WARN2 plain'); sent = sent + 1;\n"
    "  } catch (e) { t1 = 'thr'; }\n"
    "  row('consowarn', t1 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t2 = 'ok';\n"
    "  try {\n"
    "    console.error('P4ERR1 code=%d', 7); sent = sent + 1;\n"
    "    console.error('P4ERR2 plain'); sent = sent + 1;\n"
    "  } catch (e) { t2 = 'thr'; }\n"
    "  row('consoerr', t2 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t3 = 'ok';\n"
    "  try {\n"
    "    console.info('P4INFO1 %s %s', 'i1', 'i2'); sent = sent + 1;\n"
    "    console.info('P4INFO2 plain'); sent = sent + 1;\n"
    "  } catch (e) { t3 = 'thr'; }\n"
    "  row('consoinfo', t3 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t4 = 'ok';\n"
    "  try { console.debug('P4DBG1 debug-level'); sent = sent + 1; } catch (e) { t4 = 'thr'; }\n"
    "  row('consodebug', t4 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var longstr = '';\n"
    "  for (var li = 0; li < 26; li = li + 1) { longstr = longstr + 'abcdefghij'; }\n"
    "  var t5 = 'ok';\n"
    "  try { console.log('P4LONGR ' + longstr); sent = sent + 1; } catch (e) { t5 = 'thr'; }\n"
    "  row('consolong', t5 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var before = out.children.length;\n"
    "  var t6 = 'ok';\n"
    "  try {\n"
    "    if (typeof console.assert !== 'function' || typeof console.dir !== 'function') t6 = 'absent';\n"
    "    console.assert(0 === 1, 'P4ASSERT1 fired number=%d', 1); sent = sent + 1;\n"
    "    console.assert(true, 'P4NOASSERT must stay silent'); sent = sent + 1;\n"
    "    console.dir({ k: 'P4DIR1' }); sent = sent + 1;\n"
    "  } catch (e) { t6 = 'thr'; }\n"
    "  var after = out.children.length;\n"
    "  row('consoassert', t6 === 'ok' ? 'PASS' : 'MISS');\n"
    "  row('consoclean', after === before ? 'PASS' : 'MISS');\n"
    "  sent = sent + 1;\n"
    "  console.log('P4COUNT ' + sent);\n"
    "  sumEl.textContent = 'apicon: pass=' + npass + ' miss=' + nmiss + ' sent=' + sent;\n"
    "})();\n"
    "</script>\n"
    "<h2>Module receipts</h2>\n"
    "<p class=\"sum\" id=\"apimod\">apimod: pending</p>\n"
    "<div id=\"outm\"></div>\n"
    "<script type=\"module\">\n"
    "var d0 = document.createElement('p');\n"
    "d0.textContent = 'mod: modin ran';\n"
    "var om = document.getElementById('outm');\n"
    "if (om) { om.appendChild(d0); }\n"
    "</script>\n"
    "<script type=\"module\">\n"
    "import { P5A, p5b, P5C } from 'about:p5m0';\n"
    "import * as nsM1 from 'about:p5m1';\n"
    "(function () {\n"
    "  var out = document.getElementById('outm');\n"
    "  if (!out) { return; }\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "  }\n"
    "  var t0 = 'ok';\n"
    "  try {\n"
    "    if (P5A !== 41) t0 = 'bad';\n"
    "    if (typeof p5b !== 'function' || p5b() !== 1) t0 = 'bad';\n"
    "    if (P5C !== 'two') t0 = 'bad';\n"
    "  } catch (e) { t0 = 'thr'; }\n"
    "  row('modabs', t0 === 'ok' ? 'PASS' : 'MISS');\n"
    "  var t1 = 'ok';\n"
    "  try {\n"
    "    if (!nsM1 || typeof nsM1 !== 'object' || nsM1.P5D !== 1) t1 = 'bad';\n"
    "  } catch (e) { t1 = 'thr'; }\n"
    "  row('modstar', t1 === 'ok' ? 'PASS' : 'MISS');\n"
    "})();\n"
    "</script>\n"
    "<script type=\"module\">\n"
    "var firstNs = null;\n"
    "/* P10 row helpers live on globalThis so EVERY battery module (separate\n"
    " * module scopes) uses the same writers — a throw inside a promise\n"
    " * handler would reject the .then chain itself (an honest unhandled\n"
    " * rejection at the engine), so the helpers must never miss. */\n"
    "globalThis.apimodWrite = function (name, verdict) {\n"
    "  var out = document.getElementById('outm');\n"
    "  if (!out) { return; }\n"
    "  var d = document.createElement('p');\n"
    "  d.textContent = name + ': ' + verdict;\n"
    "  out.appendChild(d);\n"
    "};\n"
    "globalThis.apimodRecount = function () {\n"
    "  var out = document.getElementById('outm');\n"
    "  var sumEl = document.getElementById('apimod');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var kids = out.children.length;\n"
    "  var verdict = (kids === 7) ? 'PASS' : 'FAIL';\n"
    "  sumEl.textContent = 'apimod: kids=' + kids + ' refuse=' + verdict;\n"
    "};\n"
    "if (typeof __p5dynimport !== 'function' || typeof Promise !== 'function') {\n"
    "  /* frozen engine: no dynamic-import path — assert the rows honestly\n"
    "   * instead of reference-throwing (feature detect, per browsers). */\n"
    "  apimodWrite('moddyn', 'MISS');\n"
    "  apimodRecount();\n"
    "  apimodWrite('moddynid', 'MISS');\n"
    "  apimodRecount();\n"
    "}\n"
    "else {\n"
    "import('about:p5m0').then(function (ns) {\n"
    "  firstNs = ns;\n"
    "  var ok = !!(ns && ns.P5A === 41 && typeof ns.p5b === 'function' && ns.P5C === 'two');\n"
    "  apimodWrite('moddyn', ok ? 'PASS' : 'MISS');\n"
    "  apimodRecount();\n"
    "}, function (e) {\n"
    "  apimodWrite('moddyn', 'MISS');\n"
    "  apimodRecount();\n"
    "});\n"
    "import('about:p5m0').then(function (ns2) {\n"
    "  var ok = !!(firstNs && ns2 === firstNs);\n"
    "  apimodWrite('moddynid', ok ? 'PASS' : 'MISS');\n"
    "  apimodRecount();\n"
    "}, function (e) {\n"
    "  apimodWrite('moddynid', 'MISS');\n"
    "  apimodRecount();\n"
    "});\n"
    "}\n"
    "</script>\n"
    "<script type=\"module\">\n"
    "if (typeof __p5dynimport !== 'function' || typeof Promise !== 'function') {\n"
    "  apimodWrite('modmiss', 'MISS');\n"
    "  apimodRecount();\n"
    "}\n"
    "else {\n"
    "import('about:p5miss').then(function (ns) {\n"
    "  apimodWrite('modmiss', 'MISS');\n"
    "  apimodRecount();\n"
    "}, function (e) {\n"
    "  var ok = !!(e && typeof e.message === 'string' &&\n"
    "    e.message.indexOf('about:p5miss') >= 0);\n"
    "  apimodWrite('modmiss', ok ? 'PASS' : 'MISS');\n"
    "  apimodRecount();\n"
    "});\n"
    "}\n"
    "</script>\n"
    "<script type=\"module\">\n"
    "if (typeof __p5dynimport !== 'function' || typeof Promise !== 'function') {\n"
    "  console.log('[apimod] modnet: no dynamic-import path (engine ceiling)');\n"
    "  apimodWrite('modnet', 'MISS');\n"
    "  apimodRecount();\n"
    "}\n"
    "else {\n"
    "import('https://cdn.jsdelivr.net/npm/punycode@2.1.1/punycode.es6.js').then(function (ns) {\n"
    "  console.log('[apimod] modnet: fetched (namespace on the wire road)');\n"
    "  var ok = !!(ns && (typeof ns.encode === 'function' || typeof ns.decode === 'function'));\n"
    "  apimodWrite('modnet', ok ? 'PASS' : 'MISS');\n"
    "  apimodRecount();\n"
    "}, function (e) {\n"
    "  console.log('[apimod] modnet: rejected (offline or engine ceiling) msg=' + (e && e.message ? e.message : 'none'));\n"
    "  apimodWrite('modnet', e ? 'PASS' : 'MISS');\n"
    "  apimodRecount();\n"
    "});\n"
    "}\n"
    "</script>\n"
    "<script type=\"module\">\n"
    "import { ZZ } from './p5x.js';\n"
    "var dl = document.createElement('p');\n"
    "dl.textContent = 'MODX-LEAK';\n"
    "var om = document.getElementById('outm');\n"
    "if (om) { om.appendChild(dl); }\n"
    "</script>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('outm');\n"
    "  var sumEl = document.getElementById('apimod');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var kids = out.children.length;\n"
    "  var verdict = (kids === 7) ? 'PASS' : 'FAIL';\n"
    "  sumEl.textContent = 'apimod: kids=' + kids + ' refuse=' + verdict;\n"
    "})();\n"
    "</script>\n"
    "<!-- API-P6: parsed-template fixture — the element stays reachable\n"
    "     (id/querySelector per browsers) while its children parse into\n"
    "     the DETACHED content fragment; the ids inside never reach\n"
    "     getElementById or the render tree. -->\n"
    "<template id=\"tplx\"><p id=\"tplinnerA\">A</p><p id=\"tplinnerB\">B</p></template>\n"
    "<h2>DOM conveniences (P6)</h2>\n"
    "<p class=\"sum\" id=\"apidom\">apidom: pending</p>\n"
    "<div id=\"outd2\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('outd2');\n"
    "  var sumEl = document.getElementById('apidom');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var npass = 0, nmiss = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    if (verdict.indexOf('PASS') === 0) npass = npass + 1; else nmiss = nmiss + 1;\n"
    "  }\n"
    "  function check(name, cond) { row(name, cond ? 'PASS' : 'MISS'); }\n"
    "  var t0 = 'ok';\n"
    "  try {\n"
    "    var tpl = document.getElementById('tplx');\n"
    "    var c = tpl ? tpl.content : null;\n"
    "    if (!c) { t0 = 'nocontent'; }\n"
    "    else {\n"
    "      if (c.nodeType !== 11) { t0 = 'badkind:' + c.nodeType + ':' + typeof c.nodeType; }\n"
    "      var kk = c.children ? c.children.length : -99;\n"
    "      if (kk !== 2) { t0 = 'badkids:' + kk + ':' + typeof kk; }\n"
    "      else {\n"
    "        if (c.children[0].id !== 'tplinnerA') { t0 = 'badid:' + c.children[0].id; }\n"
    "        if (c.children[1].id !== 'tplinnerB') { t0 = 'badid2:' + c.children[1].id; }\n"
    "      }\n"
    "    }\n"
    "  } catch (e) { t0 = 'thr:' + (e && e.message ? e.message : '?'); }\n"
    "  if (t0 === 'ok') { row('tplparsed', 'PASS:nt=' + c.nodeType + ' kids=' + c.children.length); } else { row('tplparsed', 'MISS:' + t0); }\n"
    "  var t1 = 'ok';\n"
    "  try {\n"
    "    if (document.getElementById('tplinnerA') !== null) { t1 = 'leak'; }\n"
    "    if (document.getElementById('tplx') === null) { t1 = 'elemmiss'; }\n"
    "  } catch (e) { t1 = 'thr'; }\n"
    "  check('tplinert', t1 === 'ok');\n"
    "  var t2 = 'ok';\n"
    "  try {\n"
    "    var tpl2 = document.createElement('template');\n"
    "    var c2 = tpl2.content;\n"
    "    if (!c2 || c2.nodeType !== 11 || c2.children.length !== 0) { t2 = 'bad'; }\n"
    "    var pn = document.createElement('p');\n"
    "    c2.appendChild(pn);\n"
    "    if (c2.children.length !== 1) { t2 = 'badmount'; }\n"
    "    if (tpl2.children.length !== 0) { t2 = 'badleak'; }\n"
    "  } catch (e) { t2 = 'thr'; }\n"
    "  check('tplcreated', t2 === 'ok');\n"
    "  var t3 = 'ok';\n"
    "  try {\n"
    "    var host = document.getElementById('outd2');\n"
    "    var w = document.createElement('div'); w.setAttribute('class', 'clswrap');\n"
    "    w.setAttribute('data-m', 'wrap');\n"
    "    var inr = document.createElement('div'); inr.setAttribute('class', 'clsinner');\n"
    "    w.appendChild(inr);\n"
    "    host.appendChild(w);\n"
    "    var up = inr.closest('.clswrap');\n"
    "    if (up === null) { t3 = 'badnone'; }\n"
    "    else {\n"
    "      if (up.getAttribute('data-m') !== 'wrap') { t3 = 'badnode'; }\n"
    "      if (up !== w) { t3 = 'badident'; }\n"
    "    }\n"
    "    if (inr.closest('.clsinner') !== inr) { t3 = 'badself'; }\n"
    "    if (inr.closest('#outd2') !== host) { t3 = 'badup'; }\n"
    "    if (inr.closest('.nope-xx') !== null) { t3 = 'badnope'; }\n"
    "  } catch (e) { t3 = 'thr'; }\n"
    "  if (t3 === 'ok') { row('closest', 'PASS'); } else { row('closest', 'MISS:' + t3); }\n"
    "  var t4 = 'ok';\n"
    "  try {\n"
    "    var ds = document.createElement('p');\n"
    "    ds.setAttribute('data-alpha', 'one');\n"
    "    ds.setAttribute('data-max-rows', '7');\n"
    "    if (!ds.dataset || ds.dataset.alpha !== 'one') { t4 = 'badread'; }\n"
    "    if (ds.dataset.maxRows !== '7') { t4 = 'badcamel'; }\n"
    "    ds.setAttribute('data-new-key', 'zz');\n"
    "    if (ds.dataset.newKey !== 'zz') { t4 = 'badlive'; }\n"
    "  } catch (e) { t4 = 'thr'; }\n"
    "  check('dataset', t4 === 'ok');\n"
    "  var t5 = 'ok';\n"
    "  try {\n"
    "    var par = document.createElement('div');\n"
    "    var kid = document.createElement('p');\n"
    "    kid.setAttribute('data-k', 'k1');\n"
    "    var r1 = par.appendChild(kid);\n"
    "    if (!r1 || r1 !== kid || r1.getAttribute('data-k') !== 'k1') { t5 = 'badappend'; }\n"
    "    var r2 = par.removeChild(kid);\n"
    "    if (!r2 || r2 !== kid) { t5 = 'badremove'; }\n"
    "    var ref = document.createElement('p');\n"
    "    par.appendChild(ref);\n"
    "    var kid2 = document.createElement('p');\n"
    "    var r3 = par.insertBefore(kid2, ref);\n"
    "    if (!r3 || r3 !== kid2) { t5 = 'badinsert'; }\n"
    "  } catch (e) { t5 = 'thr'; }\n"
    "  check('appendret', t5 === 'ok');\n"
    "  var t6 = 'ok';\n"
    "  try {\n"
    "    var cn = document.createElement('p');\n"
    "    cn.className = 'pie green';\n"
    "    if (cn.className !== 'pie green') { t6 = 'badwrite'; }\n"
    "    if (cn.getAttribute('class') !== 'pie green') { t6 = 'badmirror'; }\n"
    "    cn.className = 'solo';\n"
    "    if (cn.className !== 'solo' || cn.getAttribute('class') !== 'solo') { t6 = 'badrewrite'; }\n"
    "    cn.setAttribute('class', 'a b');\n"
    "    if (cn.className !== 'a b') { t6 = 'badreverse'; }\n"
    "  } catch (e) { t6 = 'thr'; }\n"
    "  check('classnames', t6 === 'ok');\n"
    "  sumEl.textContent = 'apidom: pass=' + npass + ' miss=' + nmiss;\n"
    "})();\n"
    "</script></body></html>\n";

/* about:formprobe — Forms & interaction capability battery (the Forms
 * program's Phase 0 baseline page). Every row feature-detects a browser-
 * true form capability in the live engine — nothing is hard-coded to
 * pass. Road guards use the `in`-membership test so only PROTOTYPE-level
 * accessors (real engine roads) count — a plain expando write can never
 * fake a PASS. Four POLICY rows state the device-truth non-features so
 * nothing is silently relitigated. Forms-Phase 6 adds the dialog-method
 * fixture (fpdlg — the spec's "otherwise not submit" hold, receipted by
 * the formval driver's tap) and the method/enctype reflection rows.
 * The fixture form doubles as the manual-interaction surface; the
 * Events battery self-drives the roads the engine actually exposes
 * (focus()/blur()/click() attempts guarded by typeof, the submit
 * handler preventDefaults so the page never navigates) and receipts
 * the counters once at settle. Rows fire by parse order (script comes
 * after its fixtures). Plain ES5 throughout — the page runs unchanged
 * wherever the vocabulary exists. */
static const char FORMPROBE_HTML[] =
    "<!DOCTYPE html><html><head><title>Form Probe</title><style>\n"
    "body { background: white; color: black; }\n"
    "h1 { font-size: 2em; }\n"
    ".sum { font-weight: bold; }\n"
    "</style></head><body>\n"
    "<h1>Form Probe</h1>\n"
    "<p>Forms &amp; interaction capability battery — Phase 0 baseline run.</p>\n"
    "<p class=\"sum\" id=\"formsum\">formsum: pending</p>\n"
    "<div id=\"out\"></div>\n"
    "<h2>Field fixtures</h2>\n"
    "<form id=\"fpf\">\n"
    "<p>Text: <input id=\"fpt\" type=\"text\" name=\"text\" value=\"seed\"></p>\n"
    "<p>Story: <textarea id=\"fpta\" name=\"story\" rows=\"2\">hello</textarea></p>\n"
    "<p><input id=\"fpcb\" type=\"checkbox\" name=\"opt1\" checked> One (checked)</p>\n"
    "<p><input id=\"fpcb2\" type=\"checkbox\" name=\"opt2\"> Two (unchecked)</p>\n"
    "<p><input id=\"fpra\" type=\"radio\" name=\"grp\" value=\"a\" checked> Radio A</p>\n"
    "<p><input id=\"fprb\" type=\"radio\" name=\"grp\" value=\"b\"> Radio B</p>\n"
    "<p>Pick: <select id=\"fpsel\" name=\"pick\"><option>Alpha</option><option selected>Beta</option><option>Gamma</option></select></p>\n"
    "<p><input id=\"fpsend\" type=\"submit\" value=\"Send probe\"> <input id=\"fprst\" type=\"reset\" value=\"Reset probe\"></p>\n"
    "</form>\n"
    "<h2>Validation fixtures</h2>\n"
    "<p>Forms-Phase 3 battery fields — the per-rule rule rows read these.</p>\n"
    "<form id=\"fpv\">\n"
    "<p><input id=\"fprq\" type=\"text\" name=\"rv\" required placeholder=\"required\"></p>\n"
    "<p><input id=\"fppat\" type=\"text\" name=\"pv\" pattern=\"[0-9]+\" value=\"abc\"></p>\n"
    "<p><input id=\"fpplong\" type=\"text\" name=\"lv\" maxlength=\"4\" value=\"pluto\"></p>\n"
    "<p><input id=\"fpshort\" type=\"text\" name=\"sv\" minlength=\"4\" value=\"ab\"></p>\n"
    "<p><input id=\"fpnum\" type=\"number\" name=\"nv\" min=\"3\" max=\"9\" value=\"11\"></p>\n"
    "<p><textarea id=\"fvta\" name=\"tv\" rows=\"2\">story</textarea></p>\n"
    "<p><input id=\"fphide\" type=\"hidden\" name=\"hv\" value=\"h\"></p>\n"
    "<p><input id=\"fpdis\" type=\"text\" name=\"dv\" disabled></p>\n"
    "<p><input type=\"submit\" value=\"Validate me\"></p>\n"
    "</form>\n"
    "<h2>FormData fixtures</h2>\n"
    "<p>Forms-Phase 5 parity form — the battery builds FormData from this\n"
    "same form and submits it; the [fbform] POST line carries the wire\n"
    "bytes (engage: fd.submit(), no gate, no submitter pair).</p>\n"
    "<form id=\"fdp\" method=\"post\" action=\"about:formprobe\">\n"
    "<p>U: <input id=\"fdpu\" name=\"user\" value=\"Pluto\"></p>\n"
    "<p>A: <input id=\"fdpa\" name=\"age\" value=\"7\"></p>\n"
    "<p>N: <input id=\"fdpuu\" type=\"text\" value=\"noname\"></p>\n"
    "<p>M: <textarea id=\"fdpt\" name=\"msg\">hello world</textarea></p>\n"
    "<p><input id=\"fdph\" type=\"hidden\" name=\"hdn\" value=\"h1\"></p>\n"
    "<p><input id=\"fdpd\" type=\"text\" name=\"dv\" value=\"xd\" disabled></p>\n"
    "<p>C: <input id=\"fdpc\" name=\"ck\" type=\"checkbox\" checked></p>\n"
    "<p><button id=\"fdpb\" type=\"submit\">Send</button></p>\n"
    "</form>\n"
    "<h2>Dialog fixture</h2>\n"
    "<p>Forms-Phase 6 — method=dialog: with no ancestor dialog element the\n"
    "submission is the spec's honest no-op (\"otherwise not submit\"); the\n"
    "driver taps this button through the real tap lane and receipts the\n"
    "hold (the page stays on the GET URL the wire produced).</p>\n"
    "<form id=\"fpdlg\" method=\"dialog\">\n"
    "<p>F: <input id=\"fpdlgu\" name=\"dlguser\" value=\"held\"></p>\n"
    "<p><button id=\"fpdlgb\" name=\"dlgbtn\" value=\"1\" type=\"submit\">Hold page</button></p>\n"
    "</form>\n"
    "<p class=\"sum\" id=\"formev\">formev: pending</p>\n"
    "<div id=\"outev\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('out');\n"
    "  var sumEl = document.getElementById('formsum');\n"
    "  if (!out || !sumEl) { return; }\n"
    "  var npass = 0, nmiss = 0, npol = 0;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    var vs = String(verdict);\n"
    "    if (vs.indexOf('PASS') === 0) { npass = npass + 1; }\n"
    "    else if (vs.indexOf('POLICY') === 0) { npol = npol + 1; }\n"
    "    else { nmiss = nmiss + 1; }\n"
    "    console.log('[formprobe] ' + name + ': ' + vs);\n"
    "  }\n"
    "  function iscallable(v) { return typeof v === 'function'; }\n"
    "  try { row('title', document.title === 'Form Probe' ? 'PASS' : 'MISS'); } catch (e) { row('title', 'MISS'); }\n"
    "  var f = null, ta = null, cb = null, cb2 = null, ra = null, rb = null, sel = null, btn = null, rst = null, fm = null;\n"
    "  try {\n"
    "    f = document.getElementById('fpt');\n"
    "    ta = document.getElementById('fpta');\n"
    "    cb = document.getElementById('fpcb');\n"
    "    cb2 = document.getElementById('fpcb2');\n"
    "    ra = document.getElementById('fpra');\n"
    "    rb = document.getElementById('fprb');\n"
    "    sel = document.getElementById('fpsel');\n"
    "    btn = document.getElementById('fpsend');\n"
    "    rst = document.getElementById('fprst');\n"
    "    fm = document.getElementById('fpf');\n"
    "    row('fixtures', f && ta && cb && cb2 && ra && rb && sel && btn && rst && fm ? 'PASS' : 'MISS');\n"
    "  } catch (e) { row('fixtures', 'MISS'); }\n"
    "  var fprq = null, fppat = null, fpplong = null, fpshort = null, fpnum = null, fvta = null, fphide = null, fpdis = null, icount = 0;\n"
    "  try {\n"
    "    fprq = document.getElementById('fprq');\n"
    "    fppat = document.getElementById('fppat');\n"
    "    fpplong = document.getElementById('fpplong');\n"
    "    fpshort = document.getElementById('fpshort');\n"
    "    fpnum = document.getElementById('fpnum');\n"
    "    fvta = document.getElementById('fvta');\n"
    "    fphide = document.getElementById('fphide');\n"
    "    fpdis = document.getElementById('fpdis');\n"
    "    if (fppat && fppat.addEventListener) {\n"
    "      fppat.addEventListener('invalid', function () { icount = icount + 1; });\n"
    "    }\n"
    "  } catch (e) {}\n"
    "  try { row('types', f && f.type === 'text' && btn && btn.type === 'submit' ? 'PASS' : 'MISS t=' + String(f && f.type) + ',' + String(btn && btn.type)); } catch (e) { row('types', 'MISS'); }\n"
    "  try { var v = f ? f.value : undefined; row('valread', f && v === 'seed' ? 'PASS' : 'MISS v=' + String(v)); } catch (e) { row('valread', 'MISS'); }\n"
    "  try { var back = ''; if (f && ('value' in f)) { f.value = 'pluto'; back = f.value; f.value = 'seed'; } row('valwrite', back === 'pluto' ? 'PASS' : 'MISS back=' + back); } catch (e) { row('valwrite', 'MISS'); }\n"
    "  try { var tv = ta ? ta.value : undefined, tw = ''; if (ta && ('value' in ta)) { ta.value = 'wrote'; tw = ta.value; try { ta.removeAttribute('value'); } catch (ee) {} } row('textarea', (tv === 'hello' && tw === 'wrote') ? 'PASS' : 'MISS v=' + String(tv) + ' w=' + String(tw)); } catch (e) { row('textarea', 'MISS'); }\n"
    "  try { row('checkbox', cb && cb2 && ('checked' in cb) && ('checked' in cb2) && cb.checked === true && cb2.checked === false ? 'PASS' : 'MISS c=' + String(cb && cb.checked) + ',' + String(cb2 && cb2.checked)); } catch (e) { row('checkbox', 'MISS'); }\n"
    "  try { row('radiogroup', ra && rb && ('checked' in ra) && ('checked' in rb) && ra.checked === true && rb.checked === false ? 'PASS' : 'MISS r=' + String(ra && ra.checked) + ',' + String(rb && rb.checked)); } catch (e) { row('radiogroup', 'MISS'); }\n"
    "  var rpair = 'thr';\n"
    "  try {\n"
    "    if (rb && ('checked' in rb)) {\n"
    "      rb.checked = true;\n"
    "      rpair = 'rb=' + String(rb.checked) + ',' + String(ra && ra.checked);\n"
    "      rb.checked = false;\n"
    "      if (ra && ('checked' in ra)) { ra.checked = true; }\n"
    "    }\n"
    "  } catch (e) { rpair = 'thr'; }\n"
    "  row('radiowrite', rpair === 'rb=true,false' ? 'PASS' : 'MISS ' + rpair);\n"
    "  try { var sv = sel ? sel.value : undefined, si = sel ? sel.selectedIndex : undefined; row('selectvalue', (sel && ('value' in sel) && ('selectedIndex' in sel) && sv === 'Beta' && si === 1) ? 'PASS' : 'MISS v=' + String(sv) + ' i=' + String(si)); } catch (e) { row('selectvalue', 'MISS'); }\n"
    "  try { row('focusroad', f && ('focus' in f) && ('blur' in f) && iscallable(f.focus) ? 'PASS' : 'MISS'); } catch (e) { row('focusroad', 'MISS'); }\n"
    "  try { row('btnclk', btn && ('click' in btn) && iscallable(btn.click) ? 'PASS' : 'MISS'); } catch (e) { row('btnclk', 'MISS'); }\n"
    "  try { row('formstate', fm && ('reset' in fm) && ('elements' in fm) ? 'PASS' : 'MISS'); } catch (e) { row('formstate', 'MISS'); }\n"
    "  try { row('formsubmit', fm && ('submit' in fm) && ('requestSubmit' in fm) ? 'PASS' : 'MISS'); } catch (e) { row('formsubmit', 'MISS'); }\n"
    "  try {\n"
    "    var step = 0, ok = false, n = 0, s = '';\n"
    "    step = 1; var dform = document.getElementById('fdp');\n"
    "    step = 2; var hasC = typeof FormData === 'function';\n"
    "    step = 3; var fd = hasC && dform ? new FormData(dform) : null;\n"
    "    step = 4; var ok4 = !!fd && typeof fd.append === 'function' && typeof fd.get === 'function' && typeof fd.getAll === 'function' && typeof fd.has === 'function' && typeof fd.set === 'function' && typeof fd.delete === 'function' && typeof fd.entries === 'function';\n"
    "    if (ok4) {\n"
    "      step = 5; var it = fd.entries(), r = it.next();\n"
    "      step = 6; ok = r && r.done === false && r.value && r.value.length === 2 && r.value[0] === 'user' && r.value[1] === 'Pluto';\n"
    "      step = 7; ok = ok && fd.get('msg') === 'hello world';\n"
    "      step = 8; ok = ok && fd.has('age') === true && fd.has('zz') === false;\n"
    "      step = 9; ok = ok && !fd.has('dv') && !fd.has('fdpuu') && !fd.has('fdpb');\n"
    "      step = 10; ok = ok && fd.get('ck') === 'on';\n"
    "      step = 11; ok = ok && fd.getAll('user').length === 1;\n"
    "      step = 12; var fde = new FormData(); ok = ok && fde.get('none') === null && fde.has('none') === false;\n"
    "      step = 13; fd.append('user', 'u2');\n"
    "      step = 14; ok = ok && fd.getAll('user').length === 2;\n"
    "      step = 15; fd.set('user', 'only');\n"
    "      step = 16; ok = ok && fd.get('user') === 'only' && fd.getAll('user').length === 1;\n"
    "      step = 17; fd.delete('user');\n"
    "      step = 18; ok = ok && fd.has('user') === false;\n"
    "      step = 19; var fd2 = new FormData(dform);\n"
    "      step = 20; s = '';\n"
    "      step = 21; it = fd2.entries();\n"
    "      step = 22; r = it.next();\n"
    "      while (r && r.done === false) {\n"
    "        s = s + (n ? '&' : '') + encodeURIComponent(r.value[0]) + '=' + encodeURIComponent(r.value[1]);\n"
    "        n = n + 1;\n"
    "        r = it.next();\n"
    "      }\n"
    "      step = 23; s = s.split('%20').join('+');\n"
    "      step = 24; ok = ok && n === 5 && s.length === 45;\n"
    "      row('formdata', ok ? 'PASS len=' + s.length : 'MISS n=' + n + ' len=' + s.length);\n"
    "    } else {\n"
    "      row('formdata', 'MISS ctor t=' + (typeof FormData));\n"
    "    }\n"
    "  } catch (e) { row('formdata', 'MISS ex=' + String(e).slice(0, 30)); }\n"
    "  try { row('validity', f && f.validity && iscallable(f.checkValidity) && typeof f.validationMessage === 'string' && typeof f.willValidate === 'boolean' ? 'PASS' : 'MISS'); } catch (e) { row('validity', 'MISS'); }\n"
    "  try { var vp = fvta ? fvta.validity : undefined; row('validshape', vp && typeof vp === 'object' && vp.valid === true && ('valueMissing' in vp) && ('patternMismatch' in vp) && ('stepMismatch' in vp) ? 'PASS' : 'MISS'); } catch (e) { row('validshape', 'MISS'); }\n"
    "  try { var bad = fprq && fprq.checkValidity() === false; var vm = bad && fprq.validity && fprq.validity.valueMissing === true; var msg = vm && typeof fprq.validationMessage === 'string' && fprq.validationMessage.length > 0; if (fprq) { fprq.value = 'ok'; } var okv = fprq && fprq.checkValidity() === true && fprq.validity.valid === true; if (fprq) { fprq.value = ''; } row('validreq', bad && vm && msg && okv ? 'PASS' : 'MISS b=' + (bad ? 1 : 0) + ' v=' + (vm ? 1 : 0) + ' m=' + (msg ? 1 : 0) + ' o=' + (okv ? 1 : 0)); } catch (e) { row('validreq', 'MISS'); }\n"
    "  try { var pm0 = fppat && fppat.checkValidity() === false && fppat.validity.patternMismatch === true; if (fppat) { fppat.value = '123'; } var pm1 = fppat && fppat.checkValidity() === true; if (fppat) { fppat.value = 'abc'; } row('validpat', pm0 && pm1 ? 'PASS' : 'MISS'); } catch (e) { row('validpat', 'MISS'); }\n"
    "  try { var tl = fpplong && fpplong.checkValidity() === false && fpplong.validity.tooLong === true; var ts = fpshort && fpshort.checkValidity() === false && fpshort.validity.tooShort === true; row('validlen', tl && ts ? 'PASS' : 'MISS'); } catch (e) { row('validlen', 'MISS'); }\n"
    "  try { var ov = fpnum && fpnum.checkValidity() === false && fpnum.validity.rangeOverflow === true; if (fpnum) { fpnum.value = '1'; } var uf = fpnum && fpnum.checkValidity() === false && fpnum.validity.rangeUnderflow === true; if (fpnum) { fpnum.value = '5'; } var inv = fpnum && fpnum.checkValidity() === true; if (fpnum) { fpnum.value = '11'; } row('validrange', ov && uf && inv ? 'PASS' : 'MISS'); } catch (e) { row('validrange', 'MISS'); }\n"
    "  try { var w0 = f ? f.willValidate === true : false; var w1 = fphide ? fphide.willValidate === false : false; var w2 = fpdis ? fpdis.willValidate === false : false; row('willval', w0 && w1 && w2 ? 'PASS' : 'MISS'); } catch (e) { row('willval', 'MISS'); }\n"
    "  try { icount = 0; var iv = fppat ? fppat.checkValidity() : undefined; row('validinvalid', iv === false && icount === 1 ? 'PASS' : 'MISS cnt=' + icount); } catch (e) { row('validinvalid', 'MISS'); }\n"
    "  try { var ev0 = f && f.validationMessage === ''; row('validmsg', ev0 ? 'PASS' : 'MISS'); } catch (e) { row('validmsg', 'MISS'); }\n"
    "  try { var els = fm ? fm.elements : null; row('frmels', els && els.length === 9 ? 'PASS' : 'MISS len=' + (els ? els.length : -1)); } catch (e) { row('frmels', 'MISS'); }\n"
    "  try { row('frmreok', fm && iscallable(fm.reset) && fm.reset() === undefined ? 'PASS' : 'MISS'); } catch (e) { row('frmreok', 'MISS'); }\n"
    "  var dlgf = document.getElementById('fpdlg');\n"
    "  try { var dlm = dlgf ? dlgf.method : undefined; row('formdlg', typeof dlm === 'string' && dlm === 'dialog' ? 'PASS' : 'MISS m=' + String(dlm)); } catch (e) { row('formdlg', 'MISS'); }\n"
    "  try {\n"
    "    var gm = fm ? fm.method : undefined;\n"
    "    var ge = fm ? fm.enctype : undefined;\n"
    "    var ga = fm ? fm.encoding : undefined;\n"
    "    var gdp = document.getElementById('fdp');\n"
    "    var gp = gdp ? gdp.method : undefined;\n"
    "    var sw = '', attr = '', inv = '', rst = '';\n"
    "    if (fm && ('method' in fm)) {\n"
    "      fm.method = 'post'; sw = fm.method; attr = fm.getAttribute('method');\n"
    "      fm.method = 'frob'; inv = fm.method;\n"
    "      fm.method = 'get'; rst = fm.method;\n"
    "    }\n"
    "    row('formmeth', (gm === 'get' && ge === 'application/x-www-form-urlencoded' && ga === ge && gp === 'post' && sw === 'post' && attr === 'post' && inv === 'get' && rst === 'get') ? 'PASS' : 'MISS m=' + String(gm) + ' e=' + String(ge) + ' a=' + String(ga) + ' p=' + String(gp) + ' s=' + sw + ' v=' + inv + ' r=' + rst);\n"
    "  } catch (e) { row('formmeth', 'MISS'); }\n"
    "  row('policy-file', 'POLICY no OS file picker on this device');\n"
    "  row('policy-date', 'POLICY date/range/color widgets not on this panel');\n"
    "  row('policy-multiselect', 'POLICY multi-row select stays one-line field');\n"
    "  row('policy-formproto', 'POLICY form API roads ride the shared element prototype (tag-gated calls)');\n"
    "  row('policy-encoding', 'POLICY multipart/form-data withheld (no file picker, no encoder); urlencoded is the wire; text/plain rides raw CRLF');\n"
    "  sumEl.textContent = 'formsum: pass=' + npass + ' miss=' + nmiss + ' policy=' + npol;\n"
    "  console.log('[formprobe] formsum: pass=' + npass + ' miss=' + nmiss + ' policy=' + npol);\n"
    /* Forms-Phase 5: the parity submit rides the page's own XHR — the
     * standing POST-PARSE delivery lane (run_xhr_ref exists on every
     * engine; frozen bridges carry no run_timer_ref, so setTimeout only
     * ever registers there). Scripts run mid-parse where doc->_dom is
     * not yet promoted, so a same-bracket submit would fall through to
     * the legacy self-GET; the XHR completion fires on a later pump
     * pass, after the walk, and fdp.submit() takes the route-standard
     * POST whose body MUST equal this run's fd recount (len=45). */
    "  try {\n"
    "    console.log('[formprobe] fdpost pre');\n"
    "    var fx = new XMLHttpRequest();\n"
    "    fx.open('GET', 'about:formprobe');\n"
    "    fx.onload = function () {\n"
    "      try {\n"
    "        console.log('[formprobe] fdpost fire');\n"
    "        var fdpf = document.getElementById('fdp');\n"
    "        if (fdpf && iscallable(fdpf.submit)) { fdpf.submit(); } else { console.log('[formprobe] fdpost skip s=' + (fdpf ? 'form' : 'null') + ' c=' + (fdpf ? typeof fdpf.submit : '?')); }\n"
    "      } catch (f2) { console.log('[formprobe] fdpost inner ' + String(f2).slice(0, 30)); }\n"
    "    };\n"
    "    fx.onerror = function () { console.log('[formprobe] fdpost err'); };\n"
    "    fx.send();\n"
    "    console.log('[formprobe] fdpost queued');\n"
    "  } catch (e) { console.log('[formprobe] fdpost defer failed ' + String(e).slice(0, 30)); }\n"
    "})();\n"
    "</script>\n"
    "<script>\n"
    "(function () {\n"
    "  var f = document.getElementById('fpt');\n"
    "  var fm = document.getElementById('fpf');\n"
    "  var btn = document.getElementById('fpsend');\n"
    "  var out = document.getElementById('outev');\n"
    "  var sumEl = document.getElementById('formev');\n"
    "  if (!f || !fm || !btn || !out || !sumEl) { return; }\n"
    "  var nf = 0, nb = 0, nin = 0, nchg = 0, nsub = 0, ndoc = 0;\n"
    "  function paint(tag) {\n"
    "    sumEl.textContent = 'formev: focus=' + nf + ' blur=' + nb + ' input=' + nin + ' change=' + nchg + ' submit=' + nsub + ' docclick=' + ndoc;\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = 'ev:' + tag;\n"
    "    out.appendChild(d);\n"
    "  }\n"
    "  f.addEventListener('focus', function () { nf = nf + 1; paint('focus'); });\n"
    "  f.addEventListener('blur', function () { nb = nb + 1; paint('blur'); });\n"
    "  f.addEventListener('input', function () { nin = nin + 1; paint('input'); });\n"
    "  f.addEventListener('change', function () { nchg = nchg + 1; paint('change'); });\n"
    "  fm.addEventListener('submit', function (e) { nsub = nsub + 1; if (e && e.preventDefault) { e.preventDefault(); } paint('submit'); });\n"
    "  document.addEventListener('click', function () { ndoc = ndoc + 1; paint('docclick'); });\n"
    "  try { if (f && typeof f.focus === 'function') { f.focus(); } } catch (e) { paint('focusthr'); }\n"
    "  try { if (f && typeof f.blur === 'function') { f.blur(); } } catch (e) { paint('blurthr'); }\n"
    "  try { if (btn && typeof btn.click === 'function') { btn.click(); } } catch (e) { paint('clickthr'); }\n"
    "  setTimeout(function () {\n"
    "    console.log('[formprobe] formev: focus=' + nf + ' blur=' + nb + ' input=' + nin + ' change=' + nchg + ' submit=' + nsub + ' docclick=' + ndoc);\n"
    "  }, 300);\n"
    "})();\n"
    "</script></body></html>\n";

/* about:formval — Forms-Phase 3's real page with NATIVE-only validation:
 * the submit button requires the engine's constraint layer — the page's
 * one script only OBSERVES (an invalid listener paints a counter; it
 * never prevents anything). A blocked submit keeps the URL; a passing
 * one navigates the real GET wire to about:formprobe. The stylesheet
 * exercises :required/:optional/:valid/:invalid — the [css-skip] pcls
 * census receipt shows these consuming (zero pcls for this page now). */
static const char FORMVAL_HTML[] =
    "<!DOCTYPE html><html><head><title>Native Validation</title><style>\n"
    "body { background: white; color: black; }\n"
    "h1 { font-size: 2em; }\n"
    ".sum { font-weight: bold; }\n"
    "input:required { color: blue; }\n"
    "input:optional { color: black; }\n"
    "#fvu:invalid { color: red; }\n"
    "input:valid { color: green; }\n"
    "</style></head><body>\n"
    "<h1>Native Validation</h1>\n"
    "<p>Real page, native-only validation: the engine's constraint layer "
    "owns the submit gate. The script below only observes.</p>\n"
    "<p class=\"sum\" id=\"fvsum\">formval: pending</p>\n"
    "<p id=\"fvinv\">invalidcount=0</p>\n"
    "<form id=\"fv\" method=\"get\" action=\"about:formprobe\">\n"
    "<p>User: <input id=\"fvu\" name=\"user\" required placeholder=\"required\"></p>\n"
    "<p>Age (3..9): <input id=\"fva\" type=\"number\" name=\"age\" min=\"3\" "
    "max=\"9\" placeholder=\"age\"></p>\n"
    "<p><input id=\"fvsend\" type=\"submit\" value=\"Submit\"></p>\n"
    "</form>\n"
    "<script>\n"
    "(function () {\n"
    "  var inv = 0;\n"
    "  var el = document.getElementById('fvinv');\n"
    "  function tick() {\n"
    "    inv = inv + 1;\n"
    "    if (el) { el.textContent = 'invalidcount=' + inv; }\n"
    "    console.log('[formval] invalid#' + inv);\n"
    "  }\n"
    "  var fvu = document.getElementById('fvu');\n"
    "  var fva = document.getElementById('fva');\n"
    "  // invalid does NOT bubble: each control listens on itself\n"
    "  if (fvu && fvu.addEventListener) { fvu.addEventListener('invalid', function () { tick(); }); }\n"
    "  if (fva && fva.addEventListener) { fva.addEventListener('invalid', function () { tick(); }); }\n"
    "  var s = document.getElementById('fvsum');\n"
    "  if (s) { s.textContent = 'formval: resting'; }\n"
    "})();\n"
    "</script></body></html>\n";

/* ── about:netprobe — Network program probe (Phase 0 + Phase 1, SIM driver
 * builds) ──
 * The network battery: 23 rows on the standing three-road surface —
 *   SYNC/LOCAL (always run, engine-identical):
 *     fixtures / localfetch / localxhr / fetchpostlocal
 *   WIRE (17 rows, lab-origin builds only; honest SKIP rows in plain prod):
 *     cachefresh wireget wiregzip wirechunked wire404 headerecho
 *     cookieecho redir302 redirget302 redirverb307 reval304 rangeseg
 *     labtimeout
 *     cookieexp cookiedmn netdns netrefuse        (Phase 5: expiry,
 *      IP-host Domain refusal, DNS taxonomy, refused taxonomy)
 *   POLICY (2 rows, standing device stances):
 *     policy-h1 policy-keepalive
 * Final line — `[netprobe] netsum: pass=N miss=M policy=K skip=S` (sum +
 * console, formprobe pattern). Wire rows ride the netlab server origin
 * (tests/netlab_server.py on 127.0.0.1:8931) which reaches the app ONLY
 * through the PLUTO_NETLAB_URL build define below — simulator driver
 * builds set it via SIMDEFS (unquoted — stringify handles the C literal);
 * the shipped app never references the lab, manual prod boots see wire
 * rows as SKIP with the lab reasons printed.
 * Phase 1 (response-cache tier) owns two rows. The lab alternates each
 * endpoint's body version on every FULL GET (ETag "FRESH-<v>"), so a
 * client WITHOUT a cache always gets a body bump (an honest MISS) and a
 * working cache keeps the body byte-identical no matter how it answers:
 *   cachefresh — /fresh (max-age=1): fetch twice back-to-back; the second
 *               must be a fresh LOCAL serve (no wire round-trip at all)
 *               → status 200 + byte-identical body.
 *   reval304   — /fresh2 (max-age=0: immediately stale → revalidates
 *               back-to-back with no timer): the client sends
 *               If-None-Match, the wire answers 304, and the cache
 *               serves the CACHED body as a browser-true 200 → status
 *               200 + byte-identical body (the 304 itself is browser-true
 *               invisible to JS).
 *   Standing-defect MISS rows (flip in their owning phases):
 *   (none — Phase 3 flipped redirverb307 PASS: the deferred redirect
 *   restart now captures verb+body+Content-Type at the DECISION block and
 *   re-arms them for 307/308 hops; 301/302/303 keep the browser-true
 *   POST→GET rewrite standing. Phase 4 owns wirechunked, wire404,
 *   labtimeout.)
 * These MISS rows are the measurement, not a failure: they flip PASS in
 * their owning phases. (reval304 flipped PASS in Phase 1; NetP2 flipped
 * headerecho + rangeseg PASS — the page's setRequestHeader list rides
 * the wire byte-for-byte and Range/206 delivers its slice as a 2xx;
 * NetP3 flipped redirverb307 PASS — 307/308 preserve method+body.)
 * Lab-origin seam: bare HOST+PORT defines (NOT a URL — a URL value would
 * need quoting through make->sh->compiler AND its // reads as a
 * preprocessor comment at translation phase 3 when unquoted; the first
 * engine-4 probe boot receipted exactly that: LAB collapsed to 'http:'
 * and every wire row dialed http:443-tls with an empty host).
 * PLUTO_NETLAB_ON arms the lab at all (driver builds only — plain prod
 * builds leave LAB='' and the wire rows print honest SKIPs). Defaults
 * set the standing lab; driver builds simply pass:
 *   -DPLUTO_NETLAB_ON   (override bare if the lab moves:
 *    -DPLUTO_NETLAB_HOST=127.0.0.1 -DPLUTO_NETLAB_PORT=8931) */
#ifdef PLUTO_NETLAB_ON
#ifndef PLUTO_NETLAB_HOST
#define PLUTO_NETLAB_HOST 127.0.0.1
#endif
#ifndef PLUTO_NETLAB_PORT
#define PLUTO_NETLAB_PORT 8931
#endif
/* C-stringify the lab parts so bare numeric defines build clean. */
#define PLUTO_NETLAB_Q_(x) #x
#define PLUTO_NETLAB_Q(x) PLUTO_NETLAB_Q_(x)
#define PLUTO_NETLAB_LABLINE \
    "  var LAB = 'http://" PLUTO_NETLAB_Q(PLUTO_NETLAB_HOST) ":" PLUTO_NETLAB_Q(PLUTO_NETLAB_PORT) "';\n"
#else
#define PLUTO_NETLAB_LABLINE "  var LAB = '';\n"
#endif
static const char NETPROBE_HTML[] =
    "<!DOCTYPE html><html><head><title>Net Probe</title><style>\n"
    "body { background: white; color: black; }\n"
    "h1 { font-size: 2em; }\n"
    ".sum { font-weight: bold; }\n"
    "</style></head><body>\n"
    "<h1>Net Probe</h1>\n"
    "<p>Network capability battery — Phase 0 baseline + Phase 1 response-cache run.</p>\n"
    "<p class=\"sum\" id=\"netsum\">netsum: pending</p>\n"
    "<div id=\"out\"></div>\n"
    "<script>\n"
    "(function () {\n"
    "  var out = document.getElementById('out');\n"
    "  var sumEl = document.getElementById('netsum');\n"
    "  if (!out || !sumEl) { return; }\n"
    PLUTO_NETLAB_LABLINE
    "  var npass = 0, nmiss = 0, npol = 0, nskip = 0, ndone = 0;\n"
    "  var TOTAL = 23;\n"
    "  function row(name, verdict) {\n"
    "    var d = document.createElement('p');\n"
    "    d.textContent = name + ': ' + verdict;\n"
    "    out.appendChild(d);\n"
    "    var vs = String(verdict);\n"
    "    if (vs.indexOf('PASS') === 0) { npass = npass + 1; }\n"
    "    else if (vs.indexOf('POLICY') === 0) { npol = npol + 1; }\n"
    "    else if (vs.indexOf('SKIP') === 0) { nskip = nskip + 1; }\n"
    "    else { nmiss = nmiss + 1; }\n"
    "    ndone = ndone + 1;\n"
    "    console.log('[netprobe] ' + name + ': ' + vs);\n"
    "    if (ndone >= TOTAL) {\n"
    "      var line = 'netsum: pass=' + npass + ' miss=' + nmiss +\n"
    "        ' policy=' + npol + ' skip=' + nskip;\n"
    "      sumEl.textContent = line;\n"
    "      console.log('[netprobe] ' + line);\n"
    "    }\n"
    "  }\n"
    /* Local rows — the standing roads, every build, every engine: */
    "  try { row('fixtures', (typeof fetch === 'function' &&\n"
    "    typeof XMLHttpRequest !== 'undefined' &&\n"
    "    typeof Promise === 'function') ? 'PASS' : 'MISS f=' + typeof fetch +\n"
    "    ' x=' + typeof XMLHttpRequest); } catch (e) { row('fixtures', 'MISS'); }\n"
    "  try { row('policy-h1', 'POLICY client speaks HTTP/1.1 line protocol only (no HTTP/2 negotiation)'); } catch (e) {}\n"
    "  try { row('policy-keepalive', 'POLICY per-request TCP (Connection: close) through the single-flight client; no pooled reuse'); } catch (e) {}\n"
    "  try {\n"
    "    fetch('about:apiprobe').then(function (r) {\n"
    "      if (r && r.ok === true && r.status === 200) {\n"
    "        return r.text().then(function (t) {\n"
    "          row('localfetch', t && t.length > 100 && t.indexOf('API') >= 0 ?\n"
    "            'PASS len=' + t.length : 'MISS len=' + (t ? t.length : -1));\n"
    "        });\n"
    "      }\n"
    "      row('localfetch', 'MISS');\n"
    "    }).catch(function () { row('localfetch', 'MISS'); });\n"
    "  } catch (e) { row('localfetch', 'MISS'); }\n"
    "  try {\n"
    "    var x = new XMLHttpRequest();\n"
    "    var xdone = 0;\n"
    "    x.open('GET', 'about:javascript');\n"
    "    x.onload = function () {\n"
    "      try {\n"
    "        xdone = 1;\n"
    "        row('localxhr', x.status === 200 && x.responseText &&\n"
    "          x.responseText.length > 0 ? 'PASS len=' + x.responseText.length :\n"
    "          'MISS st=' + String(x.status));\n"
    "      } catch (e2) { row('localxhr', 'MISS inner ' + String(e2).substring(0, 40)); }\n"
    "    };\n"
    "    x.onerror = function () { try { xdone = 1; row('localxhr', 'MISS onerror'); } catch (e3) {} };\n"
    "    x.send();\n"
    "    /* Settle-guard: an engine that aborts the local XHR completion lane\n"
    "     * (receipted aloud by the bridge) must not stall the battery — the\n"
    "     * timer row is the grim honest fallback line. */\n"
    "    try { setTimeout(function () {\n"
    "      if (xdone) { return; }\n"
    "      try { row('localxhr', 'MISS no-completion-in-2s (engine abort receipt stands)'); } catch (e4) {}\n"
    "    }, 2000); } catch (e5) {}\n"
    "  } catch (e) { row('localxhr', 'MISS'); }\n"
    "  try {\n"
    "    fetch('about:blank', { method: 'POST',\n"
    "      headers: { 'content-type': 'application/x-www-form-urlencoded' },\n"
    "      body: 'a=1&b=2' }).then(function (r) {\n"
    "      row('fetchpostlocal', r && r.ok ? 'PASS' : 'MISS');\n"
    "    }).catch(function () { row('fetchpostlocal', 'MISS'); });\n"
    "  } catch (e) { row('fetchpostlocal', 'MISS'); }\n"
    /* Wire rows — lab builds only; plain builds print honest SKIPs. */
    "  if (LAB !== '') {\n"
    "    var namesW = ['cachefresh', 'wireget', 'wiregzip', 'wirechunked',\n"
    "      'wire404', 'headerecho', 'cookieecho', 'redir302', 'redirget302',\n"
    "      'redirverb307', 'reval304', 'rangeseg', 'labtimeout',\n"
    "      'cookieexp', 'cookiedmn', 'netdns', 'netrefuse'];\n"
    "    var steps = [\n"
    "      function (n) {\n"
    "        fetch(LAB + '/fresh').then(function (r1) {\n"
    "          return r1.text().then(function (b1) {\n"
    "            fetch(LAB + '/fresh').then(function (r2) {\n"
    "              return r2.text().then(function (b2) {\n"
    "                var ok = r2 && r2.status === 200 && b2 === b1;\n"
    "                row('cachefresh', ok ?\n"
    "                  'PASS (fresh local serve — body identical within max-age)' :\n"
    "                  'MISS st=' + String(r2 && r2.status) + ' eq=' +\n"
    "                  String(b2 === b1) + ' (no freshness cache)');\n"
    "                n();\n"
    "              });\n"
    "            }, function (e) { row('cachefresh', 'MISS ' + String(e)); n(); });\n"
    "          });\n"
    "        }, function (e) { row('cachefresh', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/text').then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('wireget', r && r.status === 200 &&\n"
    "              b === 'NETLAB-OK-0123456789ABCDEF' ? 'PASS' :\n"
    "              'MISS st=' + String(r && r.status) + ' b=' + String(b).substring(0, 12));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('wireget', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/gzip').then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('wiregzip', r && r.status === 200 &&\n"
    "              b === 'NETLAB-OK-0123456789ABCDEF' ? 'PASS (decoded)' :\n"
    "              'MISS st=' + String(r && r.status) + ' b=' + String(b).substring(0, 12));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('wiregzip', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/chunked').then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('wirechunked', r && r.status === 200 &&\n"
    "              b === 'NETLAB-OK-0123456789ABCDEF' ? 'PASS (decoded)' :\n"
    "              'MISS st=' + String(r && r.status) + ' b=' + String(b).substring(0, 12));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('wirechunked', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/status404').then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('wire404', r && r.ok === false && r.status === 404 &&\n"
    "              b === 'NETLAB-404-BODY-00' ? 'PASS' :\n"
    "              'MISS ok=' + String(r && r.ok) + ' st=' + String(r && r.status));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('wire404', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/hdr', { headers: { 'X-Pluto-Receipt': 'r1' } }).then(function (r) {\n"
    "          return r.text().then(function (b1) {\n"
    "            fetch(LAB + '/hdr', { method: 'POST', headers: {\n"
    "              'X-Pluto-Receipt': 'p1', 'content-type': 'text/plain' },\n"
    "              body: 'x=1' }).then(function (r2) {\n"
    "              return r2.text().then(function (b2) {\n"
    "                row('headerecho', b1 === 'HDR=r1' && b2 === 'HDR=p1' ?\n"
    "                  'PASS (wire GET+POST)' :\n"
    "                  'MISS b1=' + String(b1) + ' b2=' + String(b2));\n"
    "                n();\n"
    "              });\n"
    "            }, function (e) { row('headerecho', 'MISS POST ' +\n"
    "              String(e)); n(); });\n"
    "          });\n"
    "        }, function (e) { row('headerecho', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/setcookie').then(function () {\n"
    "          fetch(LAB + '/echo').then(function (r2) {\n"
    "            return r2.text().then(function (b) {\n"
    "              row('cookieecho', b && b.indexOf('COOKIE=nlab=1') === 0 ?\n"
    "                'PASS (jar round-trip)' : 'MISS v=' + String(b).substring(0, 30));\n"
    "              n();\n"
    "            });\n"
    "          }, function (e) { row('cookieecho', 'MISS echo ' + String(e)); n(); });\n"
    "        }, function (e) { row('cookieecho', 'MISS set ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/redirect?mode=302&hops=2').then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('redir302', r && r.status === 200 &&\n"
    "              b === 'NETLAB-OK-0123456789ABCDEF' ? 'PASS (2 hops)' :\n"
    "              'MISS st=' + String(r && r.status));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('redir302', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/redirectpost?mode=302', { method: 'POST',\n"
    "          headers: { 'content-type': 'application/x-www-form-urlencoded' },\n"
    "          body: 'a=1&b=2' }).then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('redirget302', b && b.indexOf('method=GET') === 0 ?\n"
    "              'PASS (302 rewrites POST to GET, body dropped)' :\n"
    "              'MISS v=' + String(b).substring(0, 30));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('redirget302', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/redirectpost?mode=307', { method: 'POST',\n"
    "          headers: { 'content-type': 'application/x-www-form-urlencoded' },\n"
    "          body: 'a=1&b=2' }).then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('redirverb307', b && b.indexOf('method=POST') === 0 ?\n"
    "              'PASS (307 preserves method+body)' :\n"
    "              'MISS v=' + String(b).substring(0, 40));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('redirverb307', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/fresh2').then(function (r1) {\n"
    "          return r1.text().then(function (b1) {\n"
    "            fetch(LAB + '/fresh2').then(function (r2) {\n"
    "              return r2.text().then(function (b2) {\n"
    "                var ok = r2 && r2.status === 200 && b2 === b1;\n"
    "                row('reval304', ok ?\n"
    "                  'PASS (stale entry revalidated: wire 304, cached body served as 200)' :\n"
    "                  'MISS st=' + String(r2 && r2.status) + ' eq=' +\n"
    "                  String(b2 === b1) + ' (stale entry not revalidated to its cached body)');\n"
    "                n();\n"
    "              });\n"
    "            }, function (e) { row('reval304', 'MISS ' + String(e)); n(); });\n"
    "          });\n"
    "        }, function (e) { row('reval304', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/range', { headers: { 'Range': 'bytes=0-4' } }).then(function (r) {\n"
    "          return r.text().then(function (b) {\n"
    "            row('rangeseg', r && r.status === 206 && b === 'NETLA' ?\n"
    "              'PASS (206 slice)' :\n"
    "              'MISS st=' + String(r && r.status) + ' b=' + String(b).substring(0, 12));\n"
    "            n();\n"
    "          });\n"
    "        }, function (e) { row('rangeseg', 'MISS ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/slow').then(function () {\n"
    "          row('labtimeout', 'MISS resolved-ok (server answered ahead of watchdog)');\n"
    "          n();\n"
    "        }, function (e) {\n"
    "          row('labtimeout', 'PASS (bounded rejection)');\n"
    "          n();\n"
    "        });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/setcookieexp').then(function () {\n"
    "          setTimeout(function () {\n"
    "            fetch(LAB + '/echo').then(function (r2) {\n"
    "              return r2.text().then(function (b) {\n"
    "                var replayed = b && b.indexOf('exp=') >= 0;\n"
    "                var healthy = b && b.indexOf('COOKIE=nlab=1') === 0;\n"
    "                row('cookieexp', !replayed && healthy ?\n"
    "                  'PASS (Max-Age expiry pruned at replay)' :\n"
    "                  'MISS replay=' + String(replayed) + ' jar=' +\n"
    "                  String(healthy) + ' v=' + String(b).substring(0, 40));\n"
    "                n();\n"
    "              });\n"
    "            }, function (e) { row('cookieexp', 'MISS echo ' + String(e)); n(); });\n"
    "          }, 1200);\n"
    "        }, function (e) { row('cookieexp', 'MISS set ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch(LAB + '/setcookiedmn').then(function () {\n"
    "          fetch(LAB + '/echo').then(function (r2) {\n"
    "            return r2.text().then(function (b) {\n"
    "              var refused = b && b.indexOf('dmn=') < 0;\n"
    "              var healthy = b && b.indexOf('COOKIE=nlab=1') === 0;\n"
    "              row('cookiedmn', refused && healthy ?\n"
    "                'PASS (Domain on IP host refused entirely)' :\n"
    "                'MISS refused=' + String(refused) + ' jar=' +\n"
    "                String(healthy) + ' v=' + String(b).substring(0, 40));\n"
    "              n();\n"
    "            });\n"
    "          }, function (e) { row('cookiedmn', 'MISS echo ' + String(e)); n(); });\n"
    "        }, function (e) { row('cookiedmn', 'MISS set ' + String(e)); n(); });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch('http://nosuchhost.invalid/').then(function () {\n"
    "          row('netdns', 'MISS resolved-ok (bogus host answered?)');\n"
    "          n();\n"
    "        }, function (e) {\n"
    "          var m = String(e);\n"
    "          var dns = m.indexOf('not found') >= 0 || m.indexOf('resolve') >= 0 ||\n"
    "                    m.indexOf('Name') >= 0;\n"
    "          row('netdns', dns ? 'PASS (DNS taxonomy: ' + m.substring(0, 70) + ')'\n"
    "                            : 'MISS rejected with: ' + m.substring(0, 80));\n"
    "          n();\n"
    "        });\n"
    "      },\n"
    "      function (n) {\n"
    "        fetch('http://127.0.0.1:8939/').then(function () {\n"
    "          row('netrefuse', 'MISS resolved-ok (nothing listens on 8939?)');\n"
    "          n();\n"
    "        }, function (e) {\n"
    "          var m = String(e);\n"
    "          row('netrefuse', m.indexOf('refused') >= 0 ?\n"
    "            'PASS (refused taxonomy: ' + m.substring(0, 60) + ')'\n"
    "            : 'MISS rejected with: ' + m.substring(0, 80));\n"
    "          n();\n"
    "        });\n"
    "      }\n"
    "    ];\n"
    "    var qi = 0;\n"
    "    function next() {\n"
    "      if (typeof fetch !== 'function') {\n"
    "        for (var wi2 = 0; wi2 < namesW.length; wi2++) {\n"
    "          if (wi2 >= qi) { row(namesW[wi2], 'MISS no fetch on this engine (ceiling)'); }\n"
    "        }\n"
    "        return;\n"
    "      }\n"
    "      if (qi < steps.length) {\n"
    "        var f = steps[qi];\n"
    "        qi = qi + 1;\n"
    "        f(next);\n"
    "      }\n"
    "    }\n"
    "    next();\n"
    "  } else {\n"
    "    var names = ['cachefresh', 'wireget', 'wiregzip', 'wirechunked',\n"
    "      'wire404', 'headerecho', 'cookieecho', 'redir302', 'redirget302',\n"
    "      'redirverb307', 'reval304', 'rangeseg', 'labtimeout',\n"
    "      'cookieexp', 'cookiedmn', 'netdns', 'netrefuse'];\n"
    "    for (var wi = 0; wi < names.length; wi++) {\n"
    "      row(names[wi], 'SKIP no lab in this build (wire rows are lab-driver receipts)');\n"
    "    }\n"
    "  }\n"
    "})();\n"
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
    { "about:cssprobe", "CSS Probe", CSSPROBE_HTML },
    { "about:apiprobe", "API Probe", APIPROBE_HTML },
    { "about:formprobe", "Form Probe", FORMPROBE_HTML },
    { "about:formval", "Native Validation", FORMVAL_HTML },
    /* Network program Phase 0: the wire battery (18 rows; wire rows ride
     * the lab origin builds only — see NETPROBE_HTML above). */
    { "about:netprobe", "Net Probe", NETPROBE_HTML },
    /* API-P5: fixture MODULES served from the same store road fetch()
     * and xhr use. Import-only — deliberately NOT in the PageDirectory:
     * the battery imports them by absolute key. */
    { "about:p5m0", "P5 Module M0",
      "export const P5A = 41;\n"
      "export function p5b() { return 1; }\n"
      "export let P5C = 'two';\n"
      "export var P5U = 'unused-shape';\n" },
    { "about:p5m1", "P5 Module M1",
      "export const P5D = 1;\n"
      "export var P5E = 'e1';\n"
      "export function p5f(x) { return x + 1; }\n" },
};

/* Read-only directory view (http_client.h): name + title per internal page,
 * same order as INTERNAL_PAGES — the home page's Test Cases section lists
 * exactly these entries (all 10 programmed about: sites; the about:p5m0/
 * about:p5m1 module fixtures stay import-only, deliberately NOT here). */
static const HttpTestPage TEST_PAGES[] = {
    { "about:home", "PlutoBrowser Home" },
    { "about:blank", "Blank Page" },
    { "about:acidtest", "HTML Acid Test" },
    { "about:javascript", "JavaScript Test Suite" },
    { "about:jsext", "Full JS Test Suite" },
    { "about:cssprobe", "CSS Probe" },
    { "about:apiprobe", "API Probe" },
    { "about:formprobe", "Form Probe" },
    { "about:formval", "Native Validation" },
    { "about:netprobe", "Net Probe" },
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
    g_attemptStart = 0;
    g_connectAttempts = 0;
    g_responseWaitLogged = 0;
    g_radioHealUsed = 0;
    g_radioHealPhase = 0;
    g_radioCbErrLogged = 0;
    /* NetP1: revalidation + delivery-source markers are per-request flight
     * state — a serve or reval never outlives its request. */
    g_revalPending = 0;
    g_revalIndex = -1;
    g_servedFromCache = 0;
    g_revalServed304 = 0;
    /* C4: drop the request body + method with the rest of the request state
     * (both are per-request; a stale body outliving a cancel would leak AND
     * a stale method would retag the NEXT page load as POST). NetP3: the
     * redirect verb capture (g_redirectHopBody) deliberately lives OUTSIDE
     * this free — the decision block steals before any reset_state, the
     * deferred restart lane re-owns after it, and the front door frees an
     * unconsumed steal; aliasing never happens. */
    if (g_reqBody)
    {
        PLUTO_FREE(g_reqBody);
        g_reqBody = NULL;
    }
    g_reqBodyLen = 0;
    g_reqMethod[0] = '\0';
    /* R29 base-line overrides are per-request too (a page-set UA/Accept
     * must never retag a different page's request). */
    g_reqUserAgent[0] = '\0';
    g_reqAccept[0] = '\0';
    g_reqAcceptLang[0] = '\0';
    /* NetP2: the page's header lane clears with the request — the front
     * door re-seeds it; the redirect lane re-arms it deliberately. */
    g_extraHeaderCount = 0;
    g_state = HS_IDLE;
}
/* ── NetP2: extra-header helpers ───────────────────────────────────────────
 * Case-insensitive lane lookups against the page's stored pairs. */
static int extra_find(const char *lname)
{
    for (int i = 0; i < g_extraHeaderCount; i++)
    {
        if (strcasecmp(g_extraHeaders[i].name, lname) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int extra_has_range(void)
{
    return extra_find("range") >= 0;
}

/* Wire-integrity guard. A request header the page sets must not be allowed
 * to double the engine's framing/authority lines or corrupt the request
 * itself: bytes outside printable ASCII (CRLF injection!) and tokens that
 * would forge the client-owned lanes are refused. Refusals are logged —
 * never invented, never silently dropped — and the remaining pairs still
 * ride (one broken header must not take a whole page down). */
static int extra_header_invalid(const char *name, const char *value,
                                char *why, size_t whyLen)
{
    if (!name || !name[0])
    {
        snprintf(why, whyLen, "empty name");
        return 1;
    }
    for (const char *p = name; *p; p++)
    {
        if (*p < 0x21 || *p > 0x7e)
        {
            snprintf(why, whyLen, "illegal byte 0x%02x in name", *p);
            return 1;
        }
    }
    for (const char *p = value ? value : ""; *p; p++)
    {
        if (*p < 0x20 || *p > 0x7e)
        {
            snprintf(why, whyLen, "illegal byte 0x%02x in value", *p);
            return 1;
        }
    }
    /* Forbidden request-header set (browser-faithful, fetch spec) plus the
     * lanes the engine mediates for receipts and the cookie jar — a
     * forged duplicate there would double-send a line or impersonate
     * client-internal policy. The overridable identity headers
     * (user-agent / accept / accept-language) and content-type are
     * NOT here: they replace the builder's base value instead. */
    static const char *const FORBIDDEN[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length",
        "cookie", "cookie2", "date", "dnt", "expect", "host",
        "if-modified-since", "if-none-match", "keep-alive", "origin",
        "referer", "te", "trailer", "transfer-encoding", "upgrade", "via"};
    for (size_t i = 0;
         i < sizeof(FORBIDDEN) / sizeof(FORBIDDEN[0]); i++)
    {
        if (strcasecmp(name, FORBIDDEN[i]) == 0)
        {
            snprintf(why, whyLen, "engine-owned or spec-forbidden");
            return 1;
        }
    }
    /* Structured-field families the base protocol machinery mediates: the
     * hop-set itself, browser-credential hop lanes, and the client's own
     * access-policy lanes. Browser-faithful: fetch spec forbids them. */
    if (strncasecmp(name, "proxy-", 6) == 0 ||
        strncasecmp(name, "sec-", 4) == 0)
    {
        snprintf(why, whyLen, "spec-forbidden family");
        return 1;
    }
    return 0;
}

/* The overridable base builder lines: a page-set user-agent / accept /
 * accept-language / content-type replaces the engine's default (browser
 * semantics — the page's header wins, one line on the wire). */

/* Route one validated page header to its landing spot: the four
 * replace-lanes apply to the builder's base lines, everything else
 * appends to the extra pair list (capped; a repeated name joins its
 * values with the browser's ", " fold). */
static void extra_route(const char *name, const char *value)
{
    if (strcasecmp(name, "user-agent") == 0)
    {
        snprintf(g_reqUserAgent, sizeof(g_reqUserAgent), "%s",
                 value ? value : "");
        return;
    }
    if (strcasecmp(name, "accept") == 0)
    {
        snprintf(g_reqAccept, sizeof(g_reqAccept), "%s", value ? value : "");
        return;
    }
    if (strcasecmp(name, "accept-language") == 0)
    {
        snprintf(g_reqAcceptLang, sizeof(g_reqAcceptLang), "%s",
                 value ? value : "");
        return;
    }
    if (strcasecmp(name, "content-type") == 0)
    {
        snprintf(g_reqContentType, sizeof(g_reqContentType), "%s",
                 value ? value : "");
        return;
    }
    int dup = extra_find(name);
    if (dup >= 0)
    {
        int vf = (int)strlen(g_extraHeaders[dup].value);
        if (vf + 2 + (int)strlen(value ? value : "") <
            (int)sizeof(g_extraHeaders[dup].value))
        {
            snprintf(g_extraHeaders[dup].value + vf,
                     sizeof(g_extraHeaders[dup].value) - vf, ", %s",
                     value ? value : "");
            logger_log("[http] extra merged: %s (now %.60s)", name,
                       g_extraHeaders[dup].value);
            return;
        }
        logger_log("[http] extra merge overflow refused: %s", name);
        return;
    }
    if (g_extraHeaderCount >= HTTP_EXTRA_HEADERS_MAX)
    {
        logger_log("[http] extra lane full (%d) — refused: %s (logged, "
                   "never sent)",
                   HTTP_EXTRA_HEADERS_MAX, name);
        return;
    }
    snprintf(g_extraHeaders[g_extraHeaderCount].name,
             sizeof(g_extraHeaders[g_extraHeaderCount].name), "%s", name);
    snprintf(g_extraHeaders[g_extraHeaderCount].value,
             sizeof(g_extraHeaders[g_extraHeaderCount].value), "%s",
             value ? value : "");
    g_extraHeaderCount++;
    logger_log("[http] extra: %s: %.60s (%d on lane)", name, value,
               g_extraHeaderCount);
}

/* Seed the whole lane from a caller's list: validate every pair first
 * (invalid pairs log their reason and are skipped; the rest still ride),
 * then route. Called by the front door after reset — the lane starts
 * empty. */
static void extra_seed(const HttpHeaderPair *extra, int nExtra)
{
    for (int i = 0; i < nExtra; i++)
    {
        const char *n = extra[i].name[0] ? extra[i].name : NULL;
        const char *v = extra[i].value;
        char why[48];
        if (extra_header_invalid(n, v, why, sizeof(why)))
        {
            logger_log("[http] extra refused (%s): %s", why,
                       n ? n : "(no name)");
            continue;
        }
        extra_route(n, v);
    }
}

/* Build a minimal HTTP request (reference buildRequest). GET carried the
 * engine for its whole life; C4 adds the POST variant the JS fetch shim
 * needs — same header set, plus Content-Type/Length for body requests. */
static void build_request(StrBuf *out)
{
    strbuf_reset(out);
    strbuf_appendf(out, "%s %s HTTP/1.1\r\n",
                   g_reqMethod[0] ? g_reqMethod : "GET",
                   g_parsed->fullPath);
    if (g_parsed->port != 80 && g_parsed->port != 443)
    {
        strbuf_appendf(out, "Host: %s:%d\r\n", g_parsed->host, g_parsed->port);
    }
    else
    {
        strbuf_appendf(out, "Host: %s\r\n", g_parsed->host);
    }
    /* R29: single source of truth (constants.h USER_AGENT) — the ad-hoc
     * short string here disagreed with the documented constant and both
     * invited bot-walls. Chrome-like per user decision (we render the
     * desktop page anyway). NetP2: a page-set user-agent/accept/
     * accept-language REPLACES the base line (browser replace semantics,
     * one line on the wire). */
    strbuf_appendf(out, "User-Agent: %s\r\n",
                   g_reqUserAgent[0] ? g_reqUserAgent : USER_AGENT);
    strbuf_appendf(out, "Accept: %s\r\n",
                   g_reqAccept[0] ? g_reqAccept : "text/html,text/plain;q=0.8");
    strbuf_appendf(out, "Accept-Language: %s\r\n",
                   g_reqAcceptLang[0] ? g_reqAcceptLang : "en-US,en;q=0.9");
    /* Compression setting (user decision, default Off): Off sends
     * Accept-Encoding: identity (raw bytes — reliable on weak radio);
     * Gzip opts back into gzip (smaller transfers on good WiFi).
     * Read live from storage per request (same pattern as invertCrank/
     * mode) so all sessions follow it with no apply step.
     * g_identityEncoding is intentionally ignored here (kept only for
     * API compatibility with jsext); the gzip RESPONSE decode path
     * below stays intact as a fallback for servers/proxies that send
     * gzip unasked. */
    if (storage_setting_int("encodingMode") == 1)
    {
        strbuf_appendf(out, "Accept-Encoding: gzip\r\n");
    }
    else
    {
        strbuf_appendf(out, "Accept-Encoding: identity\r\n");
    }
    char cookie[768];
    cookie_jar_get_header(g_parsed->host, g_parsed->path, g_parsed->isSsl,
                          cookie, sizeof(cookie));
    if (cookie[0] != '\0')
    {
        strbuf_appendf(out, "Cookie: %s\r\n", cookie);
    }
    /* C4: body requests carry the JSON content type + exact length (the
     * response reader keys off Content-Length/chunked exactly as before;
     * no transfer-encoding surprises on the send path). A bodyless POST
     * still gets Content-Length: 0 (no 411 from strict servers). */
    int isPostReq = (g_reqMethod[0] && strcmp(g_reqMethod, "POST") == 0);
    if (isPostReq)
    {
        strbuf_appendf(out, "Content-Type: %s\r\n",
                       g_reqContentType[0] ? g_reqContentType
                                           : "application/json");
        strbuf_appendf(out, "Content-Length: %u\r\n",
                       (unsigned)((g_reqBody && g_reqBodyLen)
                                      ? g_reqBodyLen
                                      : 0));
    }
    /* NetP2: the page's own request headers ride here, byte-for-byte —
     * the browser's real header set. Emitted before the terminator; the
     * validator lane below stays client-internal (pages never forge the
     * revalidation lane). */
    for (int i = 0; i < g_extraHeaderCount; i++)
    {
        strbuf_appendf(out, "%s: %s\r\n", g_extraHeaders[i].name,
                       g_extraHeaders[i].value);
    }
    /* NetP1: client-internal validator lane for an in-flight revalidation
     * of a stale cache entry. Injected ONLY from g_revalPending state —
     * never page-controlled (the setRequestHeader boundary is unchanged). */
    if (g_revalPending && g_revalIndex >= 0 &&
        g_revalIndex < HTTP_CACHE_ENTRIES)
    {
        const HttpCacheEntry *e = &g_httpCache[g_revalIndex];
        if (e->used && strcmp(e->url, g_url) == 0)
        {
            if (e->etag[0])
            {
                strbuf_appendf(out, "If-None-Match: %s\r\n", e->etag);
            }
            if (e->lastMod[0])
            {
                strbuf_appendf(out, "If-Modified-Since: %s\r\n", e->lastMod);
            }
        }
    }
    strbuf_appendf(out, "Connection: close\r\n\r\n");
    if (isPostReq && g_reqBody && g_reqBodyLen)
    {
        strbuf_append_n(out, g_reqBody, g_reqBodyLen);
    }
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
        /* Phase 5: thread the REQUEST-URI path so RFC 6265 §5.1.4
         * default-path lands the cookie in its browser-correct directory
         * (was: always "/"). */
        cookie_jar_process_set_cookies_ex(g_parsed->host, g_parsed->path,
                                          list, setCookieCount);
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

/* ── Phase 5: connection-error taxonomy ──────────────────────────────────
 * The PDNetErr enum has no dedicated DNS/refused codes — the simulator
 * runtime reports every failed CONNECT as NET_CONNECTION_FAILED=-21
 * (receipted boot19 2026-10-21: nosuchhost.invalid AND a closed port both
 * arrived -21 via tcp_open_cb). Browsers distinguish by resolving FIRST;
 * no lookup API exists in the SDK, so the client classes the failure by
 * HOST SHAPE exactly as browsers do: a numeric host has no DNS step, so
 * its immediate connect failure is the refused family; a name host's
 * immediate connect failure may be resolution OR refusal — the phrase
 * names the ambiguity honestly. The raw code always rides the surfaced
 * text so a shape change is receiptable. The watchdog keeps its own
 * "Connection timed out after N seconds." for the silent-dead lane.
 *
 * ALSO RECEIPTED: the vendored pd_api_network.h enum is STALE vs the
 * runtime — the SDK's own reference enumerates NET_CONNECT_TIMEOUT=-18,
 * NET_CONNECTION_CLOSED=-19 (the header claims -18), NET_PERMISSION_DENIED
 * =-20 and NET_CONNECTION_FAILED=-21. The header's NET_CONNECTION_CLOSED
 * case is therefore dead for runtime values; the four runtime constants
 * are mirrored here without touching the SDK header. */
#define PLUTO_NET_CONNECT_TIMEOUT   ((PDNetErr)(-18))
#define PLUTO_NET_CLOSED_RUNTIME    ((PDNetErr)(-19))
#define PLUTO_NET_PERMISSION_DENIED ((PDNetErr)(-20))
#define PLUTO_NET_CONNECTION_FAILED ((PDNetErr)(-21))

/* Browser-faithful IP-literal test: digits/dots/colons only (with digits)
 * is a numeric address, never a DNS name — mirrors cookie_jar.c. */
static int net_host_is_numeric(const char *h)
{
    if (!h || h[0] == '\0')
    {
        return 0;
    }
    int digits = 0;
    for (const char *p = h; *p; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            digits++;
        }
        else if (*p != '.' && *p != ':')
        {
            return 0;
        }
    }
    return digits > 0;
}

static const char *net_err_text(PDNetErr err)
{
    switch (err)
    {
    case NET_OK:
        return "ok";
    case NET_NO_DEVICE:
        return "no network device";
    case NET_BUSY:
        return "network busy";
    case NET_WRITE_ERROR:
        return "write error";
    case NET_WRITE_BUSY:
        return "write busy";
    case NET_WRITE_TIMEOUT:
        return "write timeout";
    case NET_READ_ERROR:
        return "read error";
    case NET_READ_BUSY:
        return "read busy";
    case NET_READ_TIMEOUT:
        return "read timeout";
    case NET_READ_OVERFLOW:
        return "read overflow";
    case NET_FRAME_ERROR:
        return "frame error";
    case NET_BAD_RESPONSE:
        return "bad response";
    case NET_ERROR_RESPONSE:
        return "error response";
    case NET_RESET_TIMEOUT:
        return "reset timeout";
    case NET_BUFFER_TOO_SMALL:
        return "buffer too small";
    case NET_UNEXPECTED_RESPONSE:
        return "unexpected response";
    case NET_NOT_CONNECTED_TO_AP:
        return "not connected to WiFi";
    case NET_NOT_IMPLEMENTED:
        return "not implemented";
    case PLUTO_NET_CLOSED_RUNTIME:
        return "connection closed";
    case PLUTO_NET_PERMISSION_DENIED:
        return "network permission denied";
    default:
        return "unknown network error";
    }
}

/* Mapped phrase for a FAILED OPEN (sync rc or async cb). The raw code is
 * appended by the callers so the surfaced text stays receiptable. */
static const char *net_open_err_text(const char *host, PDNetErr err)
{
    if (err == PLUTO_NET_CONNECT_TIMEOUT)
    {
        return "connect timed out";
    }
    if (err == PLUTO_NET_CONNECTION_FAILED)
    {
        if (net_host_is_numeric(host))
        {
            /* IP literal: no resolution exists, so an immediate connect
             * failure is the refused family (closed port/unreachable). */
            return "connection refused (server did not accept)";
        }
        /* Name host: the SDK resolves internally and reports both NXDOMAIN
         * and refused-with-resolution the same way — name it honestly. */
        return "host not found (could not resolve or connect)";
    }
    return net_err_text(err);
}

static void tcp_open_cb(TCPConnection *conn, PDNetErr err, void *ud)
{
    unsigned int myId = (unsigned int)(uintptr_t)ud;
    if (myId != g_requestId || conn != g_tcp)
    {
        /* Stale open (cancelled/superseded while connecting): the async open
         * has settled NOW, so closing+releasing is finally safe. This is the
         * orphan handoff from close_tcp — plus a defensive pool check.
         *
         * Phase 4 (zombie-retry race): the id test alone had a hole — the
         * R18b attempt-recycle hands the dead attempt to the orphan slot
         * and nulls g_tcp WITHOUT bumping the generation id (the bump
         * happens later, when the retry actually re-opens). A stale
         * resolve landing inside that backoff window still matched the id
         * and entered this live path: a success marked g_connOpen for an
         * attempt that is not g_tcp's (the retry lane then believed its
         * connection was already open and never opened it — the request
         * sat silent until the 60s watchdog, the receipted zombie shape),
         * and a failure killed the live retry outright. The conn-identity
         * test (mirroring tcp_closed_cb's guard) makes any resolve that
         * is not the live attempt's connection stale BY DEFINITION. */
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
        netmon_probe_fail(); /* network-layer fault */
        /* Phase 5 taxonomy: the raw SDK code stays on the log so the
         * refused/DNS shapes are receipted, and the mapped phrase rides
         * the user-visible error (surfaced to JS via the bridge). */
        logger_log("[http] open-async fail host=%.80s err=%d (%s)",
                   g_parsed ? g_parsed->host : "?", (int)err,
                   net_open_err_text(g_parsed ? g_parsed->host : "", err));
        snprintf(g_error, sizeof(g_error), "Connection failed: %s (err=%d)",
                 net_open_err_text(g_parsed ? g_parsed->host : "", err),
                 (int)err);
        g_state = HS_ERROR;
        return;
    }
    g_connOpen = 1;
}

/* R18c: setEnabled completion — runs on the SDK event loop; only flips a
 * flag. All sequencing happens on the update tick (never inside an SDK
 * callback). */
static void radio_enabled_cb(PDNetErr err)
{
    g_radioCbFired = 1;
    g_radioCbErr = (int)err;
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

/* ── NetP1: response-cache primitives ─────────────────────────────────────── */
static int http_cache_lookup(const char *url)
{
    for (int i = 0; i < HTTP_CACHE_ENTRIES; i++)
    {
        if (g_httpCache[i].used && strcmp(g_httpCache[i].url, url) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void http_cache_evict_lru(void)
{
    int victim = -1;
    for (int i = 0; i < HTTP_CACHE_ENTRIES; i++)
    {
        if (g_httpCache[i].used &&
            (victim < 0 ||
             g_httpCache[i].touched < g_httpCache[victim].touched))
        {
            victim = i;
        }
    }
    if (victim >= 0)
    {
        logger_log("[http] cache evict url=%.80s", g_httpCache[victim].url);
        g_httpCacheBytes -= g_httpCache[victim].bodyLen;
        PLUTO_FREE(g_httpCache[victim].body);
        memset(&g_httpCache[victim], 0, sizeof(g_httpCache[victim]));
    }
}

/* Freshness test within the max-age window (wrap-safe unsigned math;
 * maxAge < 0 is never fresh — the always-revalidate stance). */
static int http_cache_fresh(int idx, unsigned int now)
{
    if (idx < 0 || idx >= HTTP_CACHE_ENTRIES || !g_httpCache[idx].used)
    {
        return 0;
    }
    if (g_httpCache[idx].maxAge < 0)
    {
        return 0;
    }
    long elapsedMs = (long)(unsigned int)(now - g_httpCache[idx].fetchedAt);
    long window = g_httpCache[idx].maxAge * 1000L; /* clamped at store */
    return elapsedMs < window;
}

/* Fresh hit → synthesize a fully completed response state; the pump's
 * ordinary HS_DONE delivery runs unchanged next tick (the about: page
 * rehearsed this one-content-type shape; budgets, materialization and
 * snap rendering all apply identically). No parse, no network, no TLS
 * access gate touched. */
static void http_cache_serve(int idx, unsigned int now)
{
    HttpCacheEntry *e = &g_httpCache[idx];
    strbuf_reset(&g_buf);
    strbuf_append_n(&g_buf, e->body, e->bodyLen);
    g_status = 200;
    g_bodyStart = 0; /* g_buf IS the body → the done path resolves bodyOff 0 */
    g_isChunked = 0;
    g_contentLength = (long)e->bodyLen;
    g_isGzip = 0; /* stored bodies are already decoded */
    g_gzipHold = 0;
    g_spillMode = SPILL_NONE;
    g_bodySpilled = 0;
    g_connClosed = 1; /* no wire involved */
    g_error[0] = '\0';
    g_savedHeaderCount = 0;
    if (e->contentType[0])
    {
        snprintf(g_savedHeaders[0][0], sizeof(g_savedHeaders[0][0]),
                 "content-type");
        snprintf(g_savedHeaders[0][1], sizeof(g_savedHeaders[0][1]),
                 "%.250s", e->contentType);
        g_savedHeaderCount = 1;
    }
    g_state = HS_DONE;
    g_servedFromCache = 1;
    e->touched = ++g_httpCacheStamp;
    logger_log("[http] cache serve (fresh) url=%.90s bytes=%zu age=%ums",
               e->url, e->bodyLen, (unsigned int)(now - e->fetchedAt));
}

/* Case-insensitive token find for header VALUES (values keep wire casing;
 * the parser lowercases only the KEYS). */
static const char *cache_find_ci(const char *hay, const char *needle)
{
    if (!hay)
    {
        return NULL;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c == needle[0] && strncasecmp(p, needle, nlen) == 0)
        {
            return p;
        }
    }
    return NULL;
}

/* Store hook (done path, pre-reset: g_url + saved headers are intact).
 * The caller gates method (GET) and status (200); here we gate caps,
 * no-store and Vary, then parse validators + freshness. The cache keeps
 * its OWN copy — the caller PLUTO_FREEs its delivery buffer. */
static void http_cache_store(const char *url, const char *body, size_t bodyLen)
{
    if (!body || bodyLen == 0 || !url || url[0] == '\0')
    {
        return; /* nothing redeliverable — empty bodies are never cached */
    }
    if (bodyLen > (size_t)HTTP_CACHE_ENTRY_CAP)
    {
        logger_log("[http] cache skip (body over %dk cap) url=%.90s bytes=%zu",
                   (int)(HTTP_CACHE_ENTRY_CAP / 1024), url, bodyLen);
        return;
    }
    if (strlen(url) >= HTTP_CACHE_KEY_CAP)
    {
        logger_log("[http] cache skip (url over key cap) url=%.90s", url);
        return;
    }
    const char *cc = saved_header("cache-control");
    if (cc && cache_find_ci(cc, "no-store"))
    {
        logger_log("[http] cache skip (no-store) url=%.90s", url);
        return;
    }
    const char *vary = saved_header("vary");
    if (vary && vary[0])
    {
        logger_log("[http] cache skip (vary) url=%.90s vary=%.40s", url, vary);
        return;
    }
    /* Freshness: an explicit max-age wins; anything else (no-cache, no
     * directive) → -1 = always revalidate before reuse. */
    long maxAge = -1;
    if (cc)
    {
        const char *ma = cache_find_ci(cc, "max-age=");
        if (ma)
        {
            maxAge = atol(ma + 8);
            if (maxAge < 0)
            {
                maxAge = -1;
            }
            else if (maxAge > 86400)
            {
                maxAge = 86400; /* 1-day clamp: fresh() math stays 32-bit safe */
            }
        }
    }
    int i = http_cache_lookup(url);
    if (i < 0)
    {
        /* Evict LRU entries (oldest reuse stamp first) until a free slot
         * exists AND the total cap admits the body. With a 512KB total and
         * a 128KB per-entry cap this always terminates. */
        for (int guard = 0; guard < HTTP_CACHE_ENTRIES + 1; guard++)
        {
            int freeSlot = -1;
            int lru = -1;
            for (int j = 0; j < HTTP_CACHE_ENTRIES; j++)
            {
                if (!g_httpCache[j].used)
                {
                    freeSlot = j;
                }
                else if (lru < 0 || g_httpCache[j].touched <
                                        g_httpCache[lru].touched)
                {
                    lru = j;
                }
            }
            if (freeSlot >= 0 && g_httpCacheBytes + bodyLen <=
                                     (size_t)HTTP_CACHE_TOTAL_CAP)
            {
                i = freeSlot;
                break;
            }
            if (lru < 0)
            {
                break; /* nothing left to evict */
            }
            http_cache_evict_lru();
        }
    }
    if (i < 0)
    {
        logger_log("[http] cache skip (cap pressure) url=%.90s", url);
        return;
    }
    HttpCacheEntry *e = &g_httpCache[i];
    if (e->used)
    {
        g_httpCacheBytes -= e->bodyLen;
        PLUTO_FREE(e->body);
        e->body = NULL;
        logger_log("[http] cache replace url=%.90s", url);
    }
    e->body = (char *)PLUTO_MALLOC(bodyLen + 1);
    if (!e->body)
    {
        memset(e, 0, sizeof(*e));
        logger_log("[http] cache store failed (OOM) url=%.90s", url);
        return;
    }
    memcpy(e->body, body, bodyLen);
    e->body[bodyLen] = '\0';
    e->bodyLen = bodyLen;
    e->used = 1;
    snprintf(e->url, sizeof(e->url), "%s", url);
    const char *etag = saved_header("etag");
    const char *lastMod = saved_header("last-modified");
    const char *ctype = saved_header("content-type");
    snprintf(e->etag, sizeof(e->etag), "%.124s", etag ? etag : "");
    snprintf(e->lastMod, sizeof(e->lastMod), "%.60s", lastMod ? lastMod : "");
    snprintf(e->contentType, sizeof(e->contentType), "%.124s",
             (ctype && ctype[0]) ? ctype : "application/octet-stream");
    e->maxAge = maxAge;
    e->fetchedAt = g_pd->system->getCurrentTimeMilliseconds();
    e->touched = ++g_httpCacheStamp;
    g_httpCacheBytes += bodyLen;
    logger_log("[http] cache store url=%.90s etag=%.44s maxage=%ld bytes=%zu",
               url, (etag && etag[0]) ? etag : "(none)", maxAge, bodyLen);
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
    g_attemptStart = 0;
    g_connectAttempts = 0;
    g_responseWaitLogged = 0;
    g_radioHealUsed = 0;
    g_radioHealPhase = 0;
    g_radioCbErrLogged = 0;
    /* NetP1: belt-and-braces clears — request entry points that skip
     * reset_state cannot inherit a prior flight's reval/serve markers;
     * the consult below may set these for stale entries. */
    g_revalPending = 0;
    g_revalIndex = -1;
    g_servedFromCache = 0;
    g_revalServed304 = 0;

    /* ── Internal about: pages ────────────────────────────────────────────── */
    if (strncmp(g_url, "about:", 6) == 0)
    {
        /* Forms-Phase 3: the GET wire may carry a query (?user=Pluto&...)
         * on an about: action — browsers resolve the PAGE on the path
         * part; the registry matches the bare name. */
        const char *bareEnd = strchr(g_url + 6, '?');
        size_t bareLen = bareEnd ? (size_t)(bareEnd - g_url)
                                 : strlen(g_url);
        InternalPage *page = NULL;
        for (size_t i = 0; i < sizeof(INTERNAL_PAGES) / sizeof(INTERNAL_PAGES[0]); i++)
        {
            size_t nl = strlen(INTERNAL_PAGES[i].name);
            if (bareLen == nl &&
                strncmp(g_url, INTERNAL_PAGES[i].name, nl) == 0)
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

    /* ── NetP1: response-cache consult (GET only; about: bypassed above) ────
     * Runs BEFORE the URL parse: a fresh hit needs no host, no TLS access
     * gate and no network — the pump's ordinary done path delivers it.
     * NetP2 policy: a request carrying a Range header skips the whole
     * consult — byte-accurate segments come from the wire; serving a
     * whole-body entry for a range-slice request (or revalidating it)
     * would fabricate endpoint semantics. */
    if ((g_reqMethod[0] == '\0' || strcmp(g_reqMethod, "GET") == 0) &&
        !extra_has_range())
    {
        int ci = http_cache_lookup(g_url);
        if (ci >= 0)
        {
            unsigned int cnow = g_pd->system->getCurrentTimeMilliseconds();
            if (http_cache_fresh(ci, cnow))
            {
                http_cache_serve(ci, cnow);
                return 1;
            }
            /* Stale: the request proceeds to the wire carrying client-
             * internal validators; the pump's 304 swap delivers the cached
             * body as a browser-true 200. max-age=0-style entries sit here
             * on every fetch (immediately stale). */
            g_revalPending = 1;
            g_revalIndex = ci;
            g_httpCache[ci].touched = ++g_httpCacheStamp;
            logger_log("[http] cache revalidate url=%.90s etag=%.44s maxage=%ld",
                       g_url,
                       g_httpCache[ci].etag[0] ? g_httpCache[ci].etag
                                                : "(no etag)",
                       g_httpCache[ci].maxAge);
        }
    }

    /* ── Parse URL ────────────────────────────────────────────────────────── */
    if (!g_parsed)
    {
        g_parsed = (UrlParsed *)PLUTO_MALLOC(sizeof(UrlParsed));
        if (!g_parsed)
        {
            /* Refused (SW3a soft budget): fail the request NOW. g_state is
             * already HS_CONNECTING with the 60s watchdog armed — returning
             * while leaving CONNECTING made a zombie request that sat
             * silent with no connect attempt until the watchdog fired
             * "Connection timed out" (R28 sim revisit died exactly here).
             * reset_state drops to HS_IDLE; the refused-malloc onError
             * contract matches the about-page OOM path. */
            if (g_cb.onError)
            {
                g_cb.onError("out of memory");
            }
            reset_state();
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
    /* R52: POOLING DISABLED (device runs 48+49: byte-identical hard crash
     * 8s into the soak's second navigation, r0/pc pointing into the string
     * "bryanwandrych"). Sequence: the page's TLS connection is pooled, the
     * snapshot fast path serves the next page, its image/script fetch hits
     * the SAME https host, and open_connection RE-OPENS the just-closed
     * pooled TLS object — exactly the SDK re-setup trap the lifecycle
     * comment documents as hazard 2 ("closing+releasing a COMPLETED TLS
     * connection and setting up a new TLS connection shortly after traps
     * the SDK"). The pooled path closes and immediately re-opens the SAME
     * object with no cooling-off window, and the crash reproduces 100%.
     * Every new fetch now gets a FRESH connection; the old connection is
     * closed and handed to the graveyard (released GRAVE_FRAMES later —
     * the proven-safe deferred pattern). Cost: one extra TLS handshake
     * per same-host fetch (~0.5-1s on device, measured in runs 40/42);
     * correctness wins. */
    TCPConnection *tcp;
    {
        if (g_pooledTcp)
        {
            /* Close the old connection now, DEFER the release to the
             * graveyard tick (GRAVE_FRAMES later) — never touch the object
             * again while the SDK event loop may still have state for it
             * (hazards 1+2), and NEVER re-open it. */
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
            /* Phase 5 taxonomy: newConnection==NULL is its own audible lane
             * (SDK could not even build the connection object for the
             * host) — logged with the host + the mapped async-open code
             * when one is known. */
            logger_log("[http] open-sync: newConnection NULL host=%.80s",
                       g_parsed->host);
            snprintf(msg, sizeof(msg), "Could not open connection to %s",
                     g_parsed->host);
            g_cb.onError(msg);
        }
        reset_state();
        return;
    }

    /* R52: every fetch pools its connection (close happens at the NEXT
     * fetch's open_connection, via the graveyard) — the pool fields stay
     * bookkept so host-switch logging and the defensive pool checks in
     * tcp_open_cb/tcp_closed_cb keep working. */
    g_pooledTcp = tcp;
    snprintf(g_pooledHost, sizeof(g_pooledHost), "%s", g_parsed->host);
    g_pooledPort = g_parsed->port;
    g_pooledSsl = g_parsed->isSsl;

    g_tcp = tcp;

    /* Generation id: any callback that no longer matches is a stale event. */
    unsigned int myId = ++g_requestId;

    /* R18b: attempt bookkeeping — the update-loop watchdog (below) measures
     * from HERE, when this attempt actually goes in flight. */
    g_connectAttempts++;
    g_attemptStart = g_pd->system->getCurrentTimeMilliseconds();
    logger_log("[http] connect attempt %d to %s:%d%s", g_connectAttempts,
               g_parsed->host, g_parsed->port,
               g_parsed->isSsl ? " (tls)" : "");

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
            /* Phase 5 taxonomy: the synchronous open rc carries its mapped
             * phrase + raw code (reveals refused/DNS shapes the SDK
             * reports synchronously). */
            logger_log("[http] open-sync fail host=%.80s err=%d (%s)",
                       g_parsed->host, (int)rc, net_open_err_text(g_parsed->host, rc));
            snprintf(msg, sizeof(msg), "Could not open connection to %s "
                                       "(err=%d %s)",
                     g_parsed->host, (int)rc, net_open_err_text(g_parsed->host, rc));
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

/* Shared front door for GET (http_get) and C4 POST (http_request): the
 * reset happens FIRST, the method/body set AFTER it (reset_state clears
 * the previous request's params), then start_request. NetP2: the extra
 * header lane seeds after the reset too — same alive-after-reset
 * pattern the method/body ride. */
static int http_call_ex(const char *method, const char *urlString,
                        const char *contentType, const HttpHeaderPair *extra,
                        int nExtra, const char *body, size_t bodyLen,
                        const HttpCallbacks *callbacks)
{
    reset_state();
    g_hasPendingRedirect = 0;
    g_redirectDepth = 0;
    /* NetP3: a brand-new request supersedes any pending redirect hop —
     * retire an unconsumed capture (the stolen body is owned here until
     * a hop lane consumes it; reset_state must never see the steal,
     * which is why g_reqBody and g_redirectHopBody are never aliased —
     * the steal nulls one, the re-arm nulls the other). */
    if (g_redirectHopBody)
    {
        PLUTO_FREE(g_redirectHopBody);
        g_redirectHopBody = NULL;
    }
    g_redirectHopBodyLen = 0;
    g_redirectPreserveVerb = 0;
    memset(g_reqMethod, 0, sizeof(g_reqMethod));
    method = (method && method[0]) ? method : "GET";
    for (int i = 0; i < (int)sizeof(g_reqMethod) - 1 && method[i]; i++)
    {
        char c = method[i];
        g_reqMethod[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (body && bodyLen)
    {
        g_reqBody = (char *)PLUTO_MALLOC(bodyLen);
        if (!g_reqBody)
        {
            g_reqMethod[0] = '\0';
            return 0; /* OOM before the wire: contained */
        }
        memcpy(g_reqBody, body, bodyLen);
        g_reqBodyLen = bodyLen;
    }
    else
    {
        g_reqBody = NULL;
        g_reqBodyLen = 0;
    }
    g_reqContentType[0] = '\0';
    if (contentType && contentType[0])
    {
        snprintf(g_reqContentType, sizeof(g_reqContentType), "%s",
                 contentType);
    }
    g_reqUserAgent[0] = '\0';
    g_reqAccept[0] = '\0';
    g_reqAcceptLang[0] = '\0';
    g_extraHeaderCount = 0;
    if (extra && nExtra > 0)
    {
        /* Bytes are copied into client-owned state: the caller's buffers
         * (the bridge's JsHttpRequest) may be transient; the wire
         * builders read state after the caller has returned. */
        extra_seed(extra, nExtra);
    }
    return start_request(urlString, callbacks);
}

static int http_call(const char *method, const char *urlString,
                     const char *body, size_t bodyLen,
                     const HttpCallbacks *callbacks)
{
    return http_call_ex(method, urlString, NULL, NULL, 0, body, bodyLen,
                        callbacks);
}

int http_get(const char *urlString, const HttpCallbacks *callbacks)
{
    return http_call("GET", urlString, NULL, 0, callbacks);
}

/* C4 (fetch/POST): explicit METHOD + optional REQUEST body. Mirrors
 * http_get (cancel-first contract); the body lives from this call until
 * the next http_call/reset. Method defaults to GET; upcased in place. */
int http_request(const char *method, const char *urlString,
                 const char *body, size_t bodyLen,
                 const HttpCallbacks *callbacks)
{
    return http_request_ct(method, urlString, NULL, body, bodyLen, callbacks);
}

/* Same front door, but the caller picks the POST Content-Type (the form
 * fallback sends application/x-www-form-urlencoded for HTML-standard
 * action posts). NULL/"" = the historical "application/json" — every
 * existing caller keeps byte-identical request behavior. */
int http_request_ct(const char *method, const char *urlString,
                    const char *contentType, const char *body, size_t bodyLen,
                    const HttpCallbacks *callbacks)
{
    /* NetP2: ct routes through the shared front door (set after its own
     * reset — the pre-reset write could not survive); zero extras keeps
     * every existing caller byte-identical. */
    return http_call_ex(method, urlString, contentType, NULL, 0, body,
                        bodyLen, callbacks);
}

/* NetP2: GET with the page's extra wire headers (the fetch shim's
 * headers: init object reaches here via the bridge's setRequestHeader
 * lane). NULL/<=0 extras = byte-identical to http_get. */
int http_get_headers(const char *urlString, const HttpHeaderPair *extra,
                     int nExtra, const HttpCallbacks *callbacks)
{
    return http_call_ex("GET", urlString, NULL, extra, nExtra, NULL, 0,
                        callbacks);
}

/* NetP2: the fully general front door (method + Content-Type + extra
 * wire headers + body) — pages and future roads. */
int http_request_h(const char *method, const char *urlString,
                   const char *contentType, const HttpHeaderPair *extra,
                   int nExtra, const char *body, size_t bodyLen,
                   const HttpCallbacks *callbacks)
{
    return http_call_ex(method, urlString, contentType, extra, nExtra,
                        body, bodyLen, callbacks);
}

void http_cancel(void)
{
    reset_state();
}


int http_is_loading(void)
{
    return g_state == HS_CONNECTING || g_state == HS_READING ||
           g_state == HS_RETRY_WAIT || g_state == HS_RADIO_HEAL ||
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
     * has long drained (deferred release, see the lifecycle comment).
     * R53 (device run 55, 03:28:38 crash): the release must ALSO wait for
     * the CURRENT request to finish. Evidence: fetch A (:443) opened at
     * t+0, fetch B (:80) at t+1 buried A in the graveyard, the 120-frame
     * release fired at ~t+4 while B's fresh TLS session was LIVE, and B's
     * response read crashed 4s later with pc inside the SDK heap executing
     * the URL string ('yanw' = bytes 9-12 of http://bryanwandrych.com/).
     * Releasing one TLS object while another is mid-session is hazard 2's
     * remaining window — the graveyard delay alone doesn't cover it. */
    if (g_graveTcp && --g_graveTimer <= 0)
    {
        if (http_is_loading())
        {
            /* An active request holds SDK TLS event-loop state; releasing
             * the grave connection now lands in hazard 2. Hold until the
             * next idle tick (graveTimer stays 0 → release on that tick). */
            ;
        }
        else
        {
            g_pd->network->tcp->release(g_graveTcp);
            g_graveTcp = NULL;
        }
    }

    /* A deferred redirect (from a prior tick) opens the next connection now,
     * well after the previous connection was closed by us. All hop state
     * (the page's header lane + the 307/308 verb capture) is captured in
     * the DECISION block — this lane only re-arms it after its own
     * reset_state (which clears method/body/lanes; the hop statics live
     * outside reset_state's reach and cannot be freed by it). */
    if (g_hasPendingRedirect)
    {
        HttpCallbacks cb = g_pendingRedirectCb;
        int preserveVerb = g_redirectPreserveVerb;
        g_hasPendingRedirect = 0;
        g_redirectPreserveVerb = 0;
        reset_state();
        /* No stack copy needed: start_request memcpy's the URL into g_url
         * before anything else can touch g_pendingRedirectUrl. */
        /* NetP2: the captured base-line overrides re-arm first. */
        snprintf(g_reqUserAgent, sizeof(g_reqUserAgent), "%s", g_hopUA);
        snprintf(g_reqAccept, sizeof(g_reqAccept), "%s", g_hopAccept);
        snprintf(g_reqAcceptLang, sizeof(g_reqAcceptLang), "%s",
                 g_hopAcceptLang);
        /* NetP3: a 307/308 hop re-fires the page's verb — method, the
         * stolen body pointer (ownership moves back to the live request),
         * and the captured Content-Type. */
        if (preserveVerb && g_redirectHopMethod[0])
        {
            snprintf(g_reqMethod, sizeof(g_reqMethod), "%s",
                     g_redirectHopMethod);
        }
        if (preserveVerb && g_redirectHopBody)
        {
            g_reqBody = g_redirectHopBody;
            g_reqBodyLen = g_redirectHopBodyLen;
            g_redirectHopBody = NULL;
            g_redirectHopBodyLen = 0;
            logger_log("[http] redirect hop re-fires %s (preserved body "
                       "%zu bytes, ct=%.40s)",
                       g_reqMethod, g_reqBodyLen, g_redirectHopCt);
        }
        if (preserveVerb && g_redirectHopCt[0])
        {
            snprintf(g_reqContentType, sizeof(g_reqContentType), "%s",
                     g_redirectHopCt);
        }
        g_redirectHopMethod[0] = '\0';
        g_redirectHopCt[0] = '\0';
        /* NetP2: the page's extra-header lane re-arms for the hop. */
        if (g_hopExtraCount > 0)
        {
            extra_seed(g_hopExtra, g_hopExtraCount);
        }
        g_hopExtraCount = 0;
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

    /* ── R18c: radio self-heal sequencer (runs on the update tick) ──────
     * Phase 1: disable the radio. Phase 2: re-enable it. Phase 3: let the
     * association settle. Each phase is callback-gated with a timeout:
     * a missing setEnabled callback must not wedge the request (we fall
     * through to the error path — still strictly better than silence).
     * After a successful heal the full connect-attempt budget restarts. */
    if (g_state == HS_RADIO_HEAL)
    {
        if (g_radioHealPhase == 1)
        {
            if (g_radioCbFired)
            {
                logger_log("[http] radio disable cb err=%d", g_radioCbErr);
                g_radioHealPhase = 2;
                g_healPhaseStart = now;
                return; /* gap starts next tick */
            }
            if (now - g_healPhaseStart > RADIO_STEP_GAP_MS)
            {
                /* No callback for the disable step: proceed anyway — the
                 * enable step below is what actually matters. */
                logger_log("[http] radio disable cb absent; continuing");
                g_radioHealPhase = 2;
                g_healPhaseStart = now;
                return;
            }
            return;
        }
        if (g_radioHealPhase == 2)
        {
            if (now - g_healPhaseStart < RADIO_STEP_GAP_MS)
            {
                return; /* hold the 1s gap between off and on */
            }
            g_radioCbFired = 0;
            g_radioCbErr = 0;
            g_pd->network->setEnabled(1, radio_enabled_cb); /* returns void */
            logger_log("[http] radio enable requested");
            g_radioHealPhase = 3;
            g_healPhaseStart = now;
            return;
        }
        /* Phase 3: wait for real association. The enable callback never
         * fired on device (run 8), so poll getStatus() instead — success
         * is the OS reporting kWifiConnected. Association takes seconds;
         * give it RADIO_ASSOC_WAIT_MS before declaring failure. */
        if (g_radioCbFired && !g_radioCbErrLogged)
        {
            logger_log("[http] radio enable cb err=%d", g_radioCbErr);
            g_radioCbErrLogged = 1;
        }
        {
            int st = (int)g_pd->network->getStatus();
            if (st != g_lastAssocStatus)
            {
                logger_log("[http] wifi status %d -> %d (t+%d ms)",
                           g_lastAssocStatus, st,
                           (int)(now - g_healPhaseStart));
                g_lastAssocStatus = st;
            }
            if (st == (int)kWifiConnected &&
                now - g_healPhaseStart > 500)
            {
                g_radioHealPhase = 4;
            }
            else if (now - g_healPhaseStart > RADIO_ASSOC_WAIT_MS)
            {
                logger_log("[http] association wait expired (status=%d)", st);
                snprintf(g_error, sizeof(g_error),
                         "Wi-Fi did not reassociate after radio reset "
                         "(status %d).", st);
                reset_state();
                return;
            }
            else
            {
                return;
            }
        }
        if (g_radioHealPhase == 4)
        {
            logger_log("[http] radio healed; restarting connect attempts");
            g_state = HS_CONNECTING;
            g_requestStart = now;
            g_attemptStart = 0;
            g_connectAttempts = 0;
            /* Old attempt objects were already handed to orphan/graveyard
             * before entering the heal; nothing else to reclaim here. */
            return;
        }
        return;
    }

    /* ── R18b: per-attempt connect watchdog ─────────────────────────────
     * A wedged in-flight open (SDK connect-timeout callback does not fire;
     * device runs 1–5 all sat silent for the full 60s) is recycled:
     * hand the attempt to the orphan slot — its stale open callback closes
     * and releases it, the proven-safe teardown — then open a fresh
     * connection after a short backoff. Bounded by HTTP_CONNECT_ATTEMPTS;
     * the 60s request watchdog still backstops. Pure transport policy:
     * identical for every site. */
    if (g_state == HS_CONNECTING && g_tcp && !g_connOpen && !g_openFailed &&
        now - g_attemptStart > CONNECT_ATTEMPT_TIMEOUT_MS)
    {
        TCPConnection *dead = g_tcp;
        g_tcp = NULL;
        if (g_pooledTcp == dead)
        {
            g_pooledTcp = NULL; /* pool entry owned by the orphan now */
        }
        g_orphanTcp = dead; /* its stale open callback closes + releases */
        if (g_connectAttempts < HTTP_CONNECT_ATTEMPTS)
        {
            logger_log("[http] connect attempt %d stalled; retrying",
                       g_connectAttempts);
            g_attemptStart = now; /* backoff phase starts now */
            g_state = HS_RETRY_WAIT;
            return;
        }
        /* R18c: before giving up, heal the radio once and retry the whole
         * connect cycle — the stale-association recovery every desktop OS
         * performs. The 60s request budget is extended for the heal window
         * (heal is bounded: ≤ ~10s). */
        logger_log("[http] connect attempt %d stalled; giving up",
                   g_connectAttempts);        int wifiNow = (int)g_pd->network->getStatus();
        int hasSetEnabled = (g_pd->network->setEnabled != NULL);
        /* Two healable states, both transport-only, no site knowledge:
         *  - kWifiNotAvailable (observed on device: os wifi=2): the OS
         *    reports association was ATTEMPTED but no AP came up — the
         *    lazy (re)associate never completed. Recovery: request
         *    association via setEnabled(true). This does NOT override a
         *    user's OFF setting (that reports kWifiNotConnected). */
        int healable = (wifiNow == (int)kWifiConnected ||
                        wifiNow == (int)kWifiNotAvailable);
        if (!g_radioHealUsed && healable && hasSetEnabled)
        {
            g_radioHealUsed = 1;
            g_radioCbFired = 0;
            g_radioCbErr = 0;
            g_radioCbErrLogged = 0;
            g_lastAssocStatus = -1;
            g_healPhaseStart = now;
            /* `dead` is ALREADY in the orphan slot (set above): its stale
             * open callback closes+releases it when the open eventually
             * settles. Do NOT close() here — closing a still-connecting
             * connection is the documented SDK crash. */
            if (wifiNow == (int)kWifiNotAvailable)
            {
                /* Skip the disable step — nothing to disassociate from.
                 * Enter phase 2; the sequencer issues setEnabled(true)
                 * after the gap elapses (no double-enable here). */
                g_radioHealPhase = 2;
                g_healPhaseStart = now;
                logger_log("[http] os wifi=NotAvailable; requesting "
                           "(re)association");
            }
            else
            {
                g_radioHealPhase = 1;
                g_pd->network->setEnabled(0, radio_enabled_cb); /* void */
                logger_log("[http] transport dead (os wifi=Connected); "
                           "cycling radio");
            }
            g_state = HS_RADIO_HEAL;
            return;
        }
        logger_log("[http] giving up (os wifi=%d, healUsed=%d, setEnabled=%d)",
                   wifiNow, g_radioHealUsed, hasSetEnabled);
        netmon_probe_fail(); /* network-layer fault */
        snprintf(g_error, sizeof(g_error),
                 "Connection timed out after %d seconds.",
                 (int)(REQUEST_TIMEOUT_MS / 1000));
        g_state = HS_ERROR;
        return;
    }

    /* R18b: backoff between attempts, then reopen on a later tick. */
    if (g_state == HS_RETRY_WAIT)
    {
        if (now - g_attemptStart >= CONNECT_BACKOFF_MS)
        {
            g_state = HS_CONNECTING;
        }
        return;
    }

    /* ── R18b: open→first-byte watchdog ─────────────────────────────────
     * Connection is open but no headers have arrived: if the server/radio
     * never sends anything, recycle exactly like a wedged connect (same
     * orphan teardown). Once any byte flows the normal 60s budget covers
     * the rest of the transfer. */
    if (g_state == HS_READING && g_connOpen && g_tcp && !g_bodyStart &&
        g_buf.len == 0 &&
        now - g_attemptStart > RESPONSE_FIRST_BYTE_TIMEOUT_MS)
    {
        TCPConnection *dead = g_tcp;
        g_tcp = NULL;
        g_pd->network->tcp->close(dead); /* open fully resolved: close is safe */
        if (g_pooledTcp == dead)
        {
            g_pooledTcp = NULL;
        }
        /* Release the closed connection long after the SDK event loop has
         * drained it (same deferred-release pattern as host switches). */
        g_graveTcp = dead;
        g_graveTimer = GRAVE_FRAMES;
        if (g_connectAttempts < HTTP_CONNECT_ATTEMPTS)
        {
            logger_log("[http] no response in %d s (attempt %d); retrying",
                       (int)(RESPONSE_FIRST_BYTE_TIMEOUT_MS / 1000),
                       g_connectAttempts);
            g_attemptStart = now; /* backoff phase starts now */
            g_connOpen = 0; /* retry must re-open (deferred-open guard) */
            g_state = HS_RETRY_WAIT;
            return;
        }
        netmon_probe_fail(); /* network-layer fault */
        snprintf(g_error, sizeof(g_error),
                 "No response from server.");
        g_state = HS_ERROR;
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
                netmon_probe_fail(); /* network-layer fault */
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
        int reqLen = (int)req.len;
        int sent = g_pd->network->tcp->write(g_tcp, req.data, req.len);
        strbuf_free(&req);
        if (sent >= 0)
        {
            g_state = HS_READING;
            g_attemptStart = now; /* first-byte window starts at send time */
            logger_log("[http] request sent (%d bytes), awaiting response "
                       "[net: %s, signal %d/3]",
                       reqLen, netmon_state_name(), netmon_level());
        }
        else if (sent == NET_WRITE_BUSY)
        {
            g_writePending = 1; /* retry next frame */
        }
        else
        {
            netmon_probe_fail(); /* network-layer fault */
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
                /* netmon: FIRST byte of the response completes the latency
                 * probe (request-send → first byte = DNS+TLS+TTFB of a
                 * fresh connection — the browser's own radio probe). */
                netmon_probe_ok(g_requestId,
                                now - g_attemptStart);
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
                    /* NetP3: verb capture — BEFORE the reset_state below
                     * (it clears method + body + the extra-header lane).
                     * Browser-true matrix: 301/302/303 rewrite POST→GET,
                     * body dropped (standing receipted rows); 307/308
                     * preserve the method + body + Content-Type. The
                     * body pointer is STOLEN here (g_reqBody nulled) so
                     * the decision's reset_state frees nothing; the
                     * deferred lane re-owns it. */
                    g_redirectPreserveVerb = (g_status == 307 ||
                                              g_status == 308);
                    if (g_redirectHopBody)
                    {
                        /* A prior hop's steal was never consumed (e.g. a
                         * 307→302 chain rewrite) — retire it now. */
                        PLUTO_FREE(g_redirectHopBody);
                        g_redirectHopBody = NULL;
                        g_redirectHopBodyLen = 0;
                    }
                    g_redirectHopMethod[0] = '\0';
                    g_redirectHopCt[0] = '\0';
                    if (g_redirectPreserveVerb)
                    {
                        snprintf(g_redirectHopMethod,
                                 sizeof(g_redirectHopMethod), "%s",
                                 g_reqMethod);
                        if (g_reqBody)
                        {
                            g_redirectHopBody = g_reqBody;
                            g_redirectHopBodyLen = g_reqBodyLen;
                            g_reqBody = NULL;
                            g_reqBodyLen = 0;
                        }
                        snprintf(g_redirectHopCt, sizeof(g_redirectHopCt),
                                 "%s", g_reqContentType);
                        logger_log("[http] redirect %d preserves verb "
                                   "(%s bodyLen=%zu ct=%.40s)",
                                   g_status, g_redirectHopMethod,
                                   g_redirectHopBodyLen, g_redirectHopCt);
                    }
                    else
                    {
                        logger_log("[http] redirect %d rewrites to GET "
                                   "(browser-true)",
                                   g_status);
                    }
                    /* NetP2/NetP3: the page's header set follows the hop
                     * (browser-true) — captured HERE, not in the deferred
                     * lane: the reset_state below clears the live lane
                     * first. (Phase 2 captured in the deferred lane,
                     * after the clear — an inert ordering for extras; the
                     * verb capture demanded the decision point.) */
                    g_hopExtraCount = g_extraHeaderCount;
                    if (g_hopExtraCount > HTTP_EXTRA_HEADERS_MAX)
                    {
                        g_hopExtraCount = HTTP_EXTRA_HEADERS_MAX;
                    }
                    memcpy(g_hopExtra, g_extraHeaders,
                           sizeof(HttpHeaderPair) *
                           (size_t)g_hopExtraCount);
                    snprintf(g_hopUA, sizeof(g_hopUA), "%s",
                             g_reqUserAgent);
                    snprintf(g_hopAccept, sizeof(g_hopAccept), "%s",
                             g_reqAccept);
                    snprintf(g_hopAcceptLang, sizeof(g_hopAcceptLang),
                             "%s", g_reqAcceptLang);
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

            /* NetP1: 304 revalidation — browser-true transparent service.
             * The wire 304 NEVER reaches the JS road: the cached body is
             * swapped in (status 200 + cached content-type + cached bytes)
             * and the ordinary contentLength completion check below
             * delivers it. Guards: a pending reval AND an entry whose URL
             * still matches this wire request — an LRU eviction or a
             * restart mid-reval must not corrupt a foreign entry; a cold
             * 304 (no pending reval) passes through verbatim as before.
             * The 304 saw no spill (parse arms disk only for 2xx) and no
             * gzip — the cached body is injected into g_buf directly and
             * completes this very tick. */
            if (g_status == 304 && g_revalPending &&
                g_revalIndex >= 0 && g_revalIndex < HTTP_CACHE_ENTRIES)
            {
                HttpCacheEntry *e = &g_httpCache[g_revalIndex];
                g_revalPending = 0;
                g_revalIndex = -1;
                if (e->used && e->body && strcmp(e->url, g_url) == 0)
                {
                    if (e->contentType[0])
                    {
                        g_savedHeaderCount = 1;
                        snprintf(g_savedHeaders[0][0],
                                 sizeof(g_savedHeaders[0][0]), "content-type");
                        snprintf(g_savedHeaders[0][1],
                                 sizeof(g_savedHeaders[0][1]), "%.250s",
                                 e->contentType);
                    }
                    strbuf_append_n(&g_buf, e->body, e->bodyLen);
                    g_contentLength = (long)e->bodyLen;
                    g_status = 200;
                    g_revalServed304 = 1;
                    e->fetchedAt = g_pd->system->getCurrentTimeMilliseconds();
                    e->touched = ++g_httpCacheStamp;
                    logger_log("[http] cache revalidated (304 -> 200) url=%.90s "
                               "bytes=%zu",
                               g_url, e->bodyLen);
                }
                else
                {
                    logger_log("[http] cache reval lost (entry gone; raw 304 "
                               "delivers) url=%.90s", g_url);
                }
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
            /* R15: ZERO-COPY disk delivery — when the receiver can adopt a
             * spill handle and the ENTIRE body is on disk (ramBody == 0:
             * headers ended before the first read; any RAM prefix forces
             * the materialized path), build a pure-body spill file in
             * bounded windows instead of a body-sized RAM buffer. Saves the
             * whole body-size transient at delivery (R14 measured 490KB
             * bigAlloc here) AND the receiver's re-spill round-trip. */
            if (g_cb.onSuccessSpill && ramBody == 0 && diskBytes > 0 &&
                bodyLen <= (size_t)MAX_RESPONSE_SIZE)
            {
                SpillFile out = pluto_spill_begin();
                int failed = 0;
                if (out != PLUTO_SPILL_INVALID)
                {
                    size_t done = 0;
                    while (done < bodyLen)
                    {
                        size_t want = bodyLen - done;
                        if (want > READ_CHUNK)
                        {
                            want = READ_CHUNK;
                        }
                        long got = pluto_spill_read(sp, (long)done,
                                                    g_readChunk, want);
                        if (got <= 0)
                        {
                            failed = 1;
                            break;
                        }
                        if (pluto_spill_write(out, g_readChunk,
                                              (size_t)got) != 0)
                        {
                            failed = 1;
                            break;
                        }
                        done += (size_t)got;
                    }
                }
                else
                {
                    failed = 1;
                }
                if (!failed)
                {
                    pluto_spill_finish(out);
                    pluto_spill_discard(sp); /* source no longer needed */
                    /* Snapshot + reset BEFORE the callback (same contract
                     * as the materialized path below): g_url/g_cb are
                     * module state the callback must not see mutated, and
                     * reset_state does not touch `out` (receiver-owned). */
                    static char urlSnap[1024];
                    strncpy(urlSnap, g_url, sizeof(urlSnap) - 1);
                    urlSnap[sizeof(urlSnap) - 1] = '\0';
                    HttpCallbacks cbDisk = g_cb;
                    int statusDisk = g_savedStatus;
                    size_t lenDisk = (size_t)bodyLen;
                    reset_state();
                    logger_log("[http] disk delivery: %zu bytes (no RAM body)",
                               lenDisk);
                    cbDisk.onSuccessSpill(statusDisk, out, lenDisk, urlSnap);
                    return;
                }
                /* Disk delivery failed: drop the partial copy and fall
                 * through to the materialized-RAM delivery (sp still
                 * valid — the windows are re-read there). */
                if (out != PLUTO_SPILL_INVALID)
                {
                    pluto_spill_discard(out);
                }
                logger_log("[http] disk delivery failed; RAM fallback");
            }
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
                    /* Spill offsets are FILE-relative: the first spilled
                     * byte lives at 0, NOT at ramBody. done indexes the
                     * combined [ram|disk] delivery buffer, so the file
                     * offset is done - ramBody (SW2b fix: the old code
                     * passed `done`, skipping the first ramBody spilled
                     * bytes and reading past EOF — truncating every
                     * non-gzip body, i.e. all images). */
                    long got = pluto_spill_read(sp, (long)(done - ramBody),
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
                if (g_savedIsChunked && deliveredLen > 0)
                {
                    /* Phase 4: a spilled chunked stream materializes with
                     * its framing intact (de-chunking happens once at
                     * delivery, SW2b). The XHR road never binds
                     * onSuccessSpill (the zero-copy lane above is gated on
                     * it), so its fallback delivered the RAW chunk frames
                     * to JS — the standing wirechunked MISS. Decode the
                     * reassembled buffer: success swaps in the decoded
                     * body; a decode failure delivers the raw bytes
                     * honestly with a log. */
                    size_t decLen = 0;
                    char *dec = decode_chunked(body, deliveredLen, &decLen);
                    if (dec)
                    {
                        PLUTO_FREE(body);
                        body = dec;
                        deliveredLen = decLen;
                        logger_log("[http] spill chunked decoded %zu -> %zu "
                                   "bytes", done, decLen);
                    }
                    else
                    {
                        logger_log("[http] spill chunked decode failed; "
                                   "delivering raw (%zu bytes)",
                                   deliveredLen);
                    }
                }
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

        /* NetP1: response-cache store — still pre-reset (g_url, the saved
         * header set and the method are intact). The redirect path exits
         * earlier, so only final 200s land here; cache-serve deliveries
         * and 304 revalidations skip it (delivering the cache to the
         * cache is a no-op, and a revalidated entry keeps ITS metadata —
         * the swap already re-armed its freshness clock). */
        if (g_savedStatus == 200 && body && deliveredLen > 0 &&
            !g_servedFromCache && !g_revalServed304 &&
            (g_reqMethod[0] == '\0' || strcmp(g_reqMethod, "GET") == 0))
        {
            http_cache_store(g_url, body, deliveredLen);
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
