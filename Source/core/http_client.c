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

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_pd()->system->realloc((p), 0)

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
    "</body></html>";

static const InternalPage INTERNAL_PAGES[] = {
    { "about:home", "PlutoBrowser",
      "<html><head><title>PlutoBrowser</title></head><body><h1>PlutoBrowser</h1><p>Ready.</p></body></html>" },
    { "about:blank", "Blank",
      "<html><body></body></html>" },
    { "about:acidtest", "HTML Acid Test", ACIDTEST_HTML },
};

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
    strbuf_reset(&g_buf);
    g_status = 200;
    g_bodyStart = 0;
    g_isChunked = 0;
    g_contentLength = -1;
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
    char cookie[768];
    cookie_jar_get_header(g_parsed->host, g_parsed->path, g_parsed->isSsl,
                          cookie, sizeof(cookie));
    if (cookie[0] != '\0')
    {
        strbuf_appendf(out, "Cookie: %s\r\n", cookie);
    }
    strbuf_appendf(out, "Connection: close\r\n\r\n");
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

    g_bodyStart = (size_t)(hEnd - buf) + 4;
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
            if (g_buf.len > 512)
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
                if (g_buf.len < MAX_RESPONSE_SIZE)
                {
                    strbuf_append_n(&g_buf, g_readChunk, (size_t)n);
                }
                long tot = g_contentLength;
                if (tot < 0)
                {
                    tot = 0;
                }
                long cur = 0;
                if (g_bodyStart)
                {
                    cur = (long)(g_buf.len - g_bodyStart);
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
        size_t bodyBytes = g_buf.len - g_bodyStart;
        if (g_isChunked)
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
        else if (g_contentLength >= 0)
        {
            if ((long)bodyBytes >= g_contentLength)
            {
                g_state = HS_DONE;
            }
        }
        if (g_buf.len >= MAX_RESPONSE_SIZE)
        {
            g_state = HS_DONE;
        }
    }

    /* ── Server closed the connection ─────────────────────────────────────── */
    if (g_state == HS_READING && g_connClosed)
    {
        if (g_buf.len == 0)
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
        strbuf_reset(&g_savedBuf);
        strbuf_append_n(&g_savedBuf, g_buf.data, g_buf.len);
        g_savedBodyStart = g_bodyStart;
        g_savedIsChunked = g_isChunked;

        size_t bodyOff = g_savedBodyStart ? g_savedBodyStart : 0;
        size_t bodyLen = g_savedBuf.len > bodyOff ? g_savedBuf.len - bodyOff : 0;
        size_t deliveredLen = bodyLen;
        char *body = NULL;
        if (g_savedIsChunked && bodyLen)
        {
            body = decode_chunked(g_savedBuf.data + bodyOff, bodyLen, &deliveredLen);
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
