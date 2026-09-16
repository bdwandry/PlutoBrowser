/*
 * PlutoBrowser — jsext.c
 * External <script src> support for the DOC_SCRIPT_FULL policy.
 *
 * Responsibilities (html/jsbridge.c stays muJS-only):
 *   - jsbridge_scan_scripts: implemented HERE and shared — extends the
 *     legacy inline scanner so it also records src= references as slots.
 *     Slots are the single source of truth for execution order (the
 *     browser-faithful interleaving of inline and external scripts) and come
 *     from the same position-based parse the inline scanner always used.
 *   - jsext_collect: resolves raw src= values to absolute URLs (url_resolve
 *     against the resolved page URL — <base href> is honored because
 *     document parsing stores the base-resolved URL), dedupes by raw src
 *     string, and enforces the unique-file cap. Unresolvable files get
 *     url[0]='\0' → never fetched, never executed, and logged.
 *   - Per-page byte budget (JSBRIDGE_EXT_PAGE_BUDGET) is enforced against
 *     DELIVERED bytes by the fetch session / local fill — files that would
 *     exceed it are refused with a log line, never executed.
 *   - JsExtFetch: sequential fetch state machine. One request at a time —
 *     the HTTP client is single-flight by design (pooled TLS connections,
 *     orphan/graveyard handling, one g_tcp). Each external gets at most one
 *     http_get with a strict response-body cap (JSBRIDGE_MAX_SCRIPT_BYTES +
 *     headroom): the socket is cut the moment the cap is exceeded so a huge
 *     file cannot flood the 2MB response buffer, and non-2xx statuses are
 *     rejected. Success bodies are arena-copied into ext[i].body/len.
 *   - Failure policy (user decision): a failed/refused download is SKIPPED
 *     with one log line; the page still renders and later scripts still run
 *     — the same treatment unreadable images already get.
 */
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "html/jsext.h"
#include "html/jsbridge.h"
#include "core/url.h"
#include "core/logger.h"
#include "core/http_client.h"
#include "pd_api.h"

PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

/* Response over-cap headroom: a body larger than this cannot be a valid
 * script (the per-script source cap is JSBRIDGE_MAX_SCRIPT_BYTES), so the
 * fetch cuts the socket at this size. */
#define JSEXT_MAX_RESPONSE (JSBRIDGE_MAX_SCRIPT_BYTES + 1024)

/* ── Scratch arena (SDK allocator, block list) ────────────────────────────── */
struct JsArenaBlock
{
    struct JsArenaBlock *next;
    size_t used, cap;
    /* data[] follows */
};

struct JsExtArena
{
    struct JsArenaBlock *blocks;
    size_t total;
};

static void *arena_alloc(JsExtArena *a, size_t n)
{
    if (n == 0)
    {
        n = 1;
    }
    /* Align to 8 so structs in the block stay aligned. */
    n = (n + 7u) & ~(size_t)7u;
    if (!a->blocks || a->blocks->cap - a->blocks->used < n)
    {
        size_t cap = 4096;
        while (cap < n)
        {
            cap *= 2;
        }
        struct JsArenaBlock *b = (struct JsArenaBlock *)PLUTO_MALLOC(
            sizeof(struct JsArenaBlock) + cap);
        if (!b)
        {
            return NULL;
        }
        b->next = a->blocks;
        b->used = 0;
        b->cap = cap;
        a->blocks = b;
        a->total += cap;
    }
    void *p = (char *)(a->blocks + 1) + a->blocks->used;
    a->blocks->used += n;
    return p;
}

void jsext_arena_free(JsExtArena *a)
{
    if (!a)
    {
        return;
    }
    struct JsArenaBlock *p = a->blocks;
    while (p)
    {
        struct JsArenaBlock *n = p->next;
        PLUTO_FREE(p);
        p = n;
    }
    PLUTO_FREE(a);
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* Case-insensitive memmem-ish find of `needle` in [p,end). */
static const char *find_from(const char *p, const char *end,
                             const char *needle, size_t nlen)
{
    if (nlen == 0 || (size_t)(end - p) < nlen)
    {
        return NULL;
    }
    for (const char *q = p; q + nlen <= end; q++)
    {
        size_t i = 0;
        while (i < nlen && (q[i] | 0x20) == (needle[i] | 0x20))
        {
            i++;
        }
        if (i == nlen)
        {
            return q;
        }
    }
    return NULL;
}

/* Extract the value of attr `name` from a tag's attribute region
 * [afterName, gt). Returns a malloc'd value or NULL. Handles name=value,
 * name="value", name='value', and valueless (returned as ""). */
static char *tag_attr_value(const char *afterName, const char *gt,
                            const char *name, size_t nlen)
{
    for (const char *q = afterName; q + nlen + 1 <= gt;)
    {
        if ((q[0] | 0x20) == (name[0] | 0x20) &&
            strncasecmp(q, name, nlen) == 0)
        {
            const char *v = q + nlen;
            if (v >= gt)
            {
                return NULL;
            }
            if (*v == '=')
            {
                v++;
                if (v < gt && (*v == '"' || *v == '\''))
                {
                    char quote = *v++;
                    const char *close = v;
                    while (close < gt && *close != quote)
                    {
                        close++;
                    }
                    size_t len = (size_t)(close - v);
                    char *out = (char *)PLUTO_MALLOC(len + 1);
                    if (out)
                    {
                        memcpy(out, v, len);
                        out[len] = '\0';
                    }
                    return out;
                }
                /* unquoted value: up to whitespace or tag end */
                const char *e = v;
                while (e < gt && *e != ' ' && *e != '\t' && *e != '\n' &&
                       *e != '\r')
                {
                    e++;
                }
                size_t len = (size_t)(e - v);
                char *out = (char *)PLUTO_MALLOC(len + 1);
                if (out)
                {
                    memcpy(out, v, len);
                    out[len] = '\0';
                }
                return out;
            }
            /* valueless attr (e.g. `defer`): not what we scan for, skip past */
            q = v;
            continue;
        }
        q++;
    }
    return NULL;
}

/* ── Scanner: slots for inline bodies + external refs, document order ──────
 * extRaw (optional) receives the RAW src= strings in first-seen order;
 * dedup happens here so identical src= values share one extRaw entry. When
 * extRaw is NULL the scan is the LEGACY behavior: src= elements are skipped
 * entirely (not counted) — bit-identical to the pre-Full inline scanner.
 * The scanner NEVER touches JsExtScript (bodies stay owned by the caller). */
int jsbridge_scan_scripts(const char *html, JsScriptSlot *slots, int slotMax,
                          char (*extRaw)[JSBRIDGE_EXT_URL_MAX], int extMax,
                          int *extCountOut)
{
    if (!html || !slots || slotMax <= 0)
    {
        if (extCountOut)
        {
            *extCountOut = 0;
        }
        return 0;
    }
    const char *pos = html;
    const char *end = html + strlen(html);
    int count = 0;
    int extCount = 0;

    while (pos < end)
    {
        /* find the next '<' */
        while (pos < end && *pos != '<')
        {
            pos++;
        }
        if (pos >= end)
        {
            break;
        }
        const char *lt = pos;
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
        /* tag name must be exactly "script" (case-insensitive) */
        if (gt - b < 6 || (b[0] | 0x20) != 's' || (b[1] | 0x20) != 'c' ||
            (b[2] | 0x20) != 'r' || (b[3] | 0x20) != 'i' ||
            (b[4] | 0x20) != 'p' || (b[5] | 0x20) != 't')
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

        /* src= present? (same 3-byte check as the legacy scanner) */
        int hasSrc = 0;
        for (const char *q = afterName; q + 3 <= gt; q++)
        {
            if ((q[0] | 0x20) == 's' && (q[1] | 0x20) == 'r' &&
                (q[2] | 0x20) == 'c')
            {
                hasSrc = 1;
                break;
            }
        }

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

        if (hasSrc)
        {
            /* Record (or skip) the external reference, then advance past
             * the whole element — exactly where the legacy scanner jumped.
             * Every counted element gets a slot (missing/empty src= still
             * occupies one with extIndex=-1 → skipped at execution) so the
             * slot list never desyncs from the element count. The slot is
             * initialized BEFORE the src lookup so no field is ever read
             * uninitialized. */
            if (count < slotMax)
            {
                slots[count].isExt = 1;
                slots[count].inlineStart = NULL;
                slots[count].inlineLen = 0;
                slots[count].extIndex = -1;
            }
            char *raw = tag_attr_value(afterName, gt, "src", 3);
            if (raw && raw[0] && extRaw)
            {
                int dup = -1;
                for (int i = 0; i < extCount; i++)
                {
                    if (strcmp(extRaw[i], raw) == 0)
                    {
                        dup = i;
                        break;
                    }
                }
                if (dup < 0 && extCount < extMax)
                {
                    snprintf(extRaw[extCount], JSBRIDGE_EXT_URL_MAX, "%s", raw);
                    extCount++;
                    dup = extCount - 1;
                }
                /* dup >= 0 → mapped (new or repeated reference); dup < 0 →
                 * over the unique-file cap: stays -1, skipped + logged */
                if (count < slotMax)
                {
                    slots[count].extIndex = dup;
                }
            }
            if (raw)
            {
                PLUTO_FREE(raw);
            }
            if (extRaw)
            {
                count++; /* Full mode: externals occupy slots */
            }
            pos = close + 8;
            goto advance;
        }

        {
            if (count < slotMax)
            {
                slots[count].isExt = 0;
                slots[count].inlineStart = gt + 1;
                slots[count].inlineLen = (size_t)(close - (gt + 1));
                slots[count].extIndex = -1;
            }
            count++;
            pos = close + 8;
        }
    advance:
        while (pos < end && *pos != '>')
        {
            pos++;
        }
        if (pos < end)
        {
            pos++;
        }
    }
    if (extCountOut)
    {
        *extCountOut = extCount;
    }
    return count;
}

/* ── Resolve + dedupe + budget (jsext_collect) ───────────────────────────── */
int jsext_collect(const char *html, const char *pageUrl,
                  JsScriptSlot **outSlots, int *outSlotCount,
                  JsExtScript **outExt, int *outExtCount,
                  JsExtArena **outArena)
{
    *outSlots = NULL;
    *outExt = NULL;
    *outSlotCount = 0;
    *outExtCount = 0;
    if (outArena)
    {
        *outArena = NULL;
    }
    if (!html)
    {
        return 0;
    }

    JsExtArena *a = (JsExtArena *)PLUTO_MALLOC(sizeof(JsExtArena));
    if (!a)
    {
        return 0;
    }
    memset(a, 0, sizeof(*a));

    int extCap = JSBRIDGE_MAX_EXT_SCRIPTS;
    JsExtScript *ext = (JsExtScript *)arena_alloc(
        a, (size_t)extCap * sizeof(JsExtScript));
    if (!ext)
    {
        jsext_arena_free(a);
        return 0;
    }
    memset(ext, 0, (size_t)extCap * sizeof(JsExtScript));

    int slotCap = JSBRIDGE_MAX_SCRIPTS;
    JsScriptSlot *slots = (JsScriptSlot *)arena_alloc(
        a, (size_t)slotCap * sizeof(JsScriptSlot));
    char (*extRaw)[JSBRIDGE_EXT_URL_MAX] = (char (*)[JSBRIDGE_EXT_URL_MAX])
        arena_alloc(a, (size_t)extCap * JSBRIDGE_EXT_URL_MAX);
    if (!slots || !extRaw)
    {
        jsext_arena_free(a);
        return 0;
    }

    int extCount = 0;
    int total = jsbridge_scan_scripts(html, slots, slotCap, extRaw, extCap,
                                      &extCount);

    /* Resolve each RAW src value to an absolute URL into the ext table and
     * enforce the per-page budget. Slots pointing at a dropped file (over
     * budget, unresolvable, over the unique-file cap) carry extIndex=-1 and
     * are skipped at execution time (logged once here). */
    size_t budget = JSBRIDGE_EXT_PAGE_BUDGET;
    for (int i = 0; i < extCount; i++)
    {
        char raw[JSBRIDGE_EXT_URL_MAX];
        snprintf(raw, sizeof(raw), "%s", extRaw[i]);
        char *abs = url_resolve(pageUrl ? pageUrl : "", raw);
        int ok = 0;
        if (abs && abs[0] && strlen(abs) < JSBRIDGE_EXT_URL_MAX)
        {
            snprintf(ext[i].url, sizeof(ext[i].url), "%s", abs);
            ok = 1;
        }
        if (abs)
        {
            pluto_free(abs);
        }
        if (!ok)
        {
            logger_log("[jsext] unresolvable src: %.96s", raw);
            ext[i].url[0] = '\0';
            continue;
        }
        if (budget < JSBRIDGE_MAX_SCRIPT_BYTES)
        {
            logger_log("[jsext] page budget exhausted: skip %s", ext[i].url);
            ext[i].url[0] = '\0'; /* never fetched, never executed */
            continue;
        }
    }
    /* (Budget is enforced against DELIVERED bytes — see jsext_prefetch_step
     * and jsext_local_fill, the only paths that fill ext[i].body — so the
     * cap covers what actually lands on the device, not optimistic guesses.) */

    *outSlots = slots;
    *outSlotCount = total;
    *outExt = ext;
    *outExtCount = extCount;
    if (outArena)
    {
        *outArena = a;
    }
    return total;
}

/* ── Built-in test scripts (about:jsext — deterministic Full-mode tests) ──
 * Served locally so the suite runs identically on simulator and device with
 * zero WiFi dependence. Sizes for the cap/budget tests are generated at
 * COMPILE TIME by macro expansion (the pattern is tiny; the linker stores
 * the expanded string), so strlen() below sees the full file. */

/* Test 6: defines a global that the page's LATER inline script consumes —
 * proves external fetch + shared engine + document-order execution. */
static const char JSEXT_ORDER_JS[] = "window.__extOrder = 'EXT_OK';\n";

/* Test 7: rewrites the page during load — wired exactly like an inline
 * script's document.write (same capture, same flush into the live tree). */
static const char JSEXT_WRITE_JS[] =
    "document.write('<p id=\"extw\">EXT-WROTE</p>');\n";

/* Tests 9 + 6b: referenced TWICE on the page — one download (one log line),
 * two executions (window.__extCount must read 2). */
static const char JSEXT_DUP_JS[] =
    "window.__extCount = (window.__extCount || 0) + 1;\n";

/* Padding generator: one 56-byte ES5 line (var redeclaration is legal).
 *   PAD300 = exactly 300 lines = 16,800 bytes — comfortably under the 64KB
 *   per-script cap AND compilable within muJS's 256KB per-script allocation
 *   budget (31KB sources are NOT — their compile tree exceeds it; 16.8KB
 *   ≈ 134KB of tree fits). Eight such files (~134.5KB with the tiny scripts)
 *   fit the 160KB page budget; the 56KB probe trips the DELIVERED-BYTES
 *   budget refusal. PAD1000 = 56,000 bytes. */
#define JSEXT_PAD_LINE \
    "var pad='abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGH';\n"
#define JSEXT_PAD10 \
    JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE \
    JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE JSEXT_PAD_LINE
#define JSEXT_PAD100 JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10 \
    JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10 JSEXT_PAD10
#define JSEXT_PAD300 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100
#define JSEXT_PAD1000 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100 \
    JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100 JSEXT_PAD100

/* Tests 11a–11h: 16,800 bytes each, cap-legal AND engine-runnable; with the
 * tiny scripts they total ~134.5KB of the 160KB page budget. */
static const char JSEXT_BIG_A[] = JSEXT_PAD300;
static const char JSEXT_BIG_B[] = JSEXT_PAD300;

/* Test 11i probe: 56,046 bytes — cap-legal, but the page's remaining
 * budget (~25KB) can't cover it → budget refusal, observable in-page. */
static const char JSEXT_BIG_C[] = JSEXT_PAD1000 "window.__big3 = 1;\n";

/* Test 10: 72,821 bytes — refused by the per-script cap BEFORE execution;
 * the page asserts the marker never appeared. */
static const char JSEXT_HUGE[] = JSEXT_PAD1000 JSEXT_PAD300 "window.__huge = 1;\n";

/* One served file: matched by file-name TAIL of the resolved src= URL. */
typedef struct
{
    const char *name;
    const char *source;
} JsExtLocalFile;

static const JsExtLocalFile JSEXT_LOCAL_FILES[] = {
    { "jsext-order.js", JSEXT_ORDER_JS },
    { "jsext-write.js", JSEXT_WRITE_JS },
    { "jsext-dup.js", JSEXT_DUP_JS },
    { "jsext-big.js", JSEXT_BIG_A },
    { "jsext-big2.js", JSEXT_BIG_B },
    { "jsext-big3.js", JSEXT_BIG_A },
    { "jsext-big4.js", JSEXT_BIG_B },
    { "jsext-big5.js", JSEXT_BIG_A },
    { "jsext-big6.js", JSEXT_BIG_B },
    { "jsext-big7.js", JSEXT_BIG_A },
    { "jsext-big8.js", JSEXT_BIG_B },
    { "jsext-big9.js", JSEXT_BIG_C },
    { "jsext-huge.js", JSEXT_HUGE },
};
#define JSEXT_LOCAL_COUNT (sizeof(JSEXT_LOCAL_FILES) / sizeof(JSEXT_LOCAL_FILES[0]))

int jsext_local_fill(JsExtArena *arena, JsExtScript *ext, int extCount)
{
    if (!arena || !ext || extCount <= 0)
    {
        return 0;
    }
    size_t budget = JSBRIDGE_EXT_PAGE_BUDGET;
    for (int i = 0; i < extCount; i++)
    {
        if (ext[i].url[0] == '\0')
        {
            continue; /* unresolvable: already logged */
        }
        /* Match on the file-name tail of the (absolute) src URL. */
        const char *tail = ext[i].url;
        const char *slash = strrchr(ext[i].url, '/');
        if (slash)
        {
            tail = slash + 1;
        }
        const JsExtLocalFile *hit = NULL;
        for (size_t k = 0; k < JSEXT_LOCAL_COUNT; k++)
        {
            if (strcmp(JSEXT_LOCAL_FILES[k].name, tail) == 0)
            {
                hit = &JSEXT_LOCAL_FILES[k];
                break;
            }
        }
        if (!hit)
        {
            logger_log("[jsext] local miss: %.96s", ext[i].url);
            ext[i].url[0] = '\0';
            continue;
        }
        size_t len = strlen(hit->source);
        if (len > JSBRIDGE_MAX_SCRIPT_BYTES)
        {
            logger_log("[jsext] local %s over per-script cap (%zu)", hit->name,
                       len);
            ext[i].url[0] = '\0';
            continue;
        }
        if (budget < len)
        {
            logger_log("[jsext] local page budget exhausted: skip %s",
                       hit->name);
            ext[i].url[0] = '\0';
            continue;
        }
        char *copy = (char *)arena_alloc(arena, len + 1);
        if (!copy)
        {
            logger_log("[jsext] local arena OOM: %s", hit->name);
            ext[i].url[0] = '\0';
            continue;
        }
        memcpy(copy, hit->source, len + 1);
        ext[i].body = copy;
        ext[i].len = len;
        budget -= len;
        logger_log("[jsext] local ok %s (%zu bytes)", hit->name, len);
    }
    return extCount; /* array unchanged; unfilled entries have body==NULL */
}

/* ── Fetch session ───────────────────────────────────────────────────── */
struct JsExtFetch
{
    JsExtArena *arena;
    JsScriptSlot *slots;
    int slotCount;
    JsExtScript *ext;
    int extCount;
    int idx;        /* current external being fetched */
    int fetching;   /* 1 while an http_get is in flight */
    int overCap;    /* current file exceeded the response cap */
    size_t totalBytes;
};

static JsExtFetch *g_fetch = NULL; /* one session at a time (single-flight) */
static size_t g_lastBytes = 0;

/* Remaining per-page external-JS budget (delivered bytes). Owned by the
 * active session; reset in jsext_prefetch_begin. */
static size_t g_pageBudget = 0;

/* Current-file response sink: success with a capped body. */
static void fetch_on_success(int status, char **keys, char **vals, int hc,
                             const char *body, size_t bodyLen, const char *url)
{
    (void)keys;
    (void)vals;
    (void)hc;
    (void)body;
    JsExtFetch *f = g_fetch;
    if (!f || !f->fetching || !url)
    {
        return; /* stale callback: session gone or request already aborted */
    }
    int i = f->idx;
    if (status < 200 || status >= 300)
    {
        logger_log("[jsext] FAIL %s status=%d", url, status);
    }
    else if (f->overCap || bodyLen > JSBRIDGE_MAX_SCRIPT_BYTES)
    {
        /* overCap means the progress sink cut the socket — the buffer only
         * holds the head of an oversized file; report what we saw. */
        logger_log("[jsext] FAIL %s too big (>%d bytes, cap %d)", url,
                   JSEXT_MAX_RESPONSE, JSBRIDGE_MAX_SCRIPT_BYTES);
    }
    else if (!body || bodyLen == 0)
    {
        logger_log("[jsext] FAIL %s empty body", url);
    }
    else if (bodyLen > g_pageBudget)
    {
        /* Per-page budget (delivered bytes): refuse like the per-script cap
         * — the page and later scripts still run. */
        logger_log("[jsext] FAIL %s over page budget (%zu left, need %zu)",
                   url, g_pageBudget, bodyLen);
    }
    else
    {
        char *copy = (char *)arena_alloc(f->arena, bodyLen + 1);
        if (copy)
        {
            memcpy(copy, body, bodyLen);
            copy[bodyLen] = '\0';
            f->ext[i].body = copy;
            f->ext[i].len = bodyLen;
            f->totalBytes += bodyLen;
            g_pageBudget -= bodyLen;
            logger_log("[jsext] ok %s (%zu bytes, %zu budget left)", url,
                       bodyLen, g_pageBudget);
        }
        else
        {
            logger_log("[jsext] FAIL %s arena OOM", url);
        }
    }
    f->fetching = 0;
    f->idx++; /* advance: otherwise the session re-fetches this file forever */
}

/* Progress sink: cut the socket the moment the response passes the cap so a
 * huge file cannot flood the HTTP client's 2MB buffer. */
static void fetch_on_progress(int cur, int total)
{
    (void)total;
    JsExtFetch *f = g_fetch;
    if (!f || !f->fetching)
    {
        return;
    }
    if (cur > JSEXT_MAX_RESPONSE)
    {
        f->overCap = 1;
        http_cancel();
        /* Cancel is silent (callbacks are zeroed, no onError fires): close
         * out this file here or the session waits on it forever. */
        f->fetching = 0;
        f->idx++;
    }
}

static void fetch_on_error(const char *message)
{
    JsExtFetch *f = g_fetch;
    if (!f || !f->fetching)
    {
        return; /* stale */
    }
    logger_log("[jsext] FAIL %s network: %s",
               f->ext[f->idx].url[0] ? f->ext[f->idx].url : "(url)", message);
    f->fetching = 0;
    f->idx++; /* advance past the failed file: skip-on-fail semantics */
}

JsExtFetch *jsext_prefetch_begin(JsExtArena *arena,
                                 JsScriptSlot *slots, int slotCount,
                                 JsExtScript *ext, int extCount)
{
    (void)slotCount;
    (void)slots;
    if (!ext || extCount <= 0 || !arena)
    {
        return NULL; /* caller keeps ownership */
    }
    JsExtFetch *f = (JsExtFetch *)PLUTO_MALLOC(sizeof(JsExtFetch));
    if (!f)
    {
        return NULL;
    }
    memset(f, 0, sizeof(*f));
    f->arena = arena;
    f->ext = ext;
    f->extCount = extCount;
    f->slots = slots;
    f->slotCount = slotCount;
    g_fetch = f;
    g_lastBytes = 0;
    g_pageBudget = JSBRIDGE_EXT_PAGE_BUDGET;
    return f;
}

int jsext_prefetch_step(void *state)
{
    JsExtFetch *f = (JsExtFetch *)state;
    if (!f)
    {
        return 0;
    }
    if (f->fetching)
    {
        return 1; /* HTTP client pumped by the main loop; wait */
    }
    if (f->idx >= f->extCount)
    {
        g_lastBytes = f->totalBytes;
        return 0; /* done */
    }
    JsExtScript *e = &f->ext[f->idx];
    if (e->url[0] == '\0')
    {
        f->idx++; /* unresolvable / over-budget: already logged */
        return 1;
    }
    HttpCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.onSuccess = fetch_on_success;
    cbs.onProgress = fetch_on_progress;
    cbs.onError = fetch_on_error;
    f->overCap = 0;
    int before = f->idx;
    if (!http_get(e->url, &cbs))
    {
        logger_log("[jsext] FAIL %s immediate", e->url);
        f->idx++;
        return 1;
    }
    /* The client can complete synchronously inside http_get (its callback
     * already advanced idx): only mark in-flight when nothing advanced. */
    if (f->idx == before)
    {
        f->fetching = 1;
    }
    return 1;
}

void jsext_fetch_detach(JsExtFetch *f, JsExtArena **outArena,
                        JsExtScript **outExt, int *outExtCount)
{
    if (outArena)
    {
        *outArena = NULL;
    }
    if (outExt)
    {
        *outExt = NULL;
    }
    if (outExtCount)
    {
        *outExtCount = 0;
    }
    if (!f)
    {
        return;
    }
    /* Hand ownership back BEFORE freeing the shell: the caller's Stage-1
     * stack copies of these pointers are long gone across the task yield —
     * the session is the only surviving owner. */
    if (outArena)
    {
        *outArena = f->arena;
    }
    if (outExt)
    {
        *outExt = f->ext;
    }
    if (outExtCount)
    {
        *outExtCount = f->extCount;
    }
    if (g_fetch == f)
    {
        g_fetch = NULL;
    }
    PLUTO_FREE(f); /* arena + arrays now owned by the caller */
}

void jsext_prefetch_abort(JsExtFetch *f)
{
    if (!f)
    {
        return;
    }
    if (f->fetching)
    {
        f->fetching = 0;
        http_cancel(); /* client's stale-callback generations make this safe */
        logger_log("[jsext] aborted at file %d/%d", f->idx, f->extCount);
    }
    if (g_fetch == f)
    {
        g_fetch = NULL;
    }
    jsext_arena_free(f->arena);
    PLUTO_FREE(f);
}

size_t jsext_last_bytes(void)
{
    return g_lastBytes;
}

void jsext_abort_active(void)
{
    /* Navigation/teardown hook: g_fetch is a module global, so a cancelled
     * render task cannot leave dangling callbacks behind. prefetch_abort
     * clears g_fetch itself when it matches. */
    if (g_fetch)
    {
        jsext_prefetch_abort(g_fetch);
    }
}
