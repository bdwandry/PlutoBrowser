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
#include "../core/pluto_mem.h"
#include "../core/pluto_spill.h"

/* SW2b: bodies at or over this size become disk-resident (re-spilled);
 * smaller ones stay arena RAM copies exactly as before. The old per-script
 * cap REFUSED these; they now download fully and live on disk. */
#define JSEXT_SPILL_THRESHOLD JSBRIDGE_MAX_SCRIPT_BYTES

PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);
static size_t jsext_page_budget(void); /* forward: SW3 unified budget lookup */
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)

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

/* SW2d: span variant of tag_attr_value — returns pointers INTO the tag
 * (start + length of the value) instead of copying. data: URLs can carry
 * the whole script in the attribute, so the value must be located without
 * the JSBRIDGE_EXT_URL_MAX copy truncation; the payload is only ever read
 * (never modified) before the page HTML dies with the document. */
static int tag_attr_value_span(const char *afterName, const char *gt,
                               const char *name, size_t nlen,
                               const char **outStart, size_t *outLen)
{
    for (const char *q = afterName; q + nlen + 1 <= gt;)
    {
        if ((q[0] | 0x20) == (name[0] | 0x20) &&
            strncasecmp(q, name, nlen) == 0)
        {
            const char *v = q + nlen;
            if (v >= gt)
            {
                return 0;
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
                    *outStart = v;
                    *outLen = (size_t)(close - v);
                    return 1;
                }
                const char *e = v;
                while (e < gt && *e != ' ' && *e != '\t' && *e != '\n' &&
                       *e != '\r')
                {
                    e++;
                }
                *outStart = v;
                *outLen = (size_t)(e - v);
                return 1;
            }
            q = v;
            continue;
        }
        q++;
    }
    return 0;
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

        /* src= present? (same 3-byte check as the legacy scanner). SW2d:
         * capture the value span so data:-URL payloads can be located in
         * the page HTML at execution time (they can exceed the 512B URL
         * storage — the payload IS the script). */
        int hasSrc = 0;
        const char *srcStart = NULL;
        size_t srcLen = 0;
        {
            const char *q = afterName;
            while (q + 3 <= gt)
            {
                if ((q[0] | 0x20) == 's' && (q[1] | 0x20) == 'r' &&
                    (q[2] | 0x20) == 'c')
                {
                    hasSrc = 1;
                    /* Read back the full attribute value via the span
                     * variant (search backwards for the attr start is
                     * unnecessary — re-locate precisely below). */
                    if (!tag_attr_value_span(afterName, gt, "src", 3,
                                             &srcStart, &srcLen))
                    {
                        srcStart = NULL;
                        srcLen = 0;
                    }
                    break;
                }
                q++;
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

        if (hasSrc && srcStart && srcLen >= 5 &&
            (srcStart[0] | 0x20) == 'd' && (srcStart[1] | 0x20) == 'a' &&
            (srcStart[2] | 0x20) == 't' && (srcStart[3] | 0x20) == 'a' &&
            srcStart[4] == ':')
        {
            /* SW2d: data:-URL script — the payload IS the source. Mark the
             * slot with the JS_SCRIPT_DATA sentinel; execution decodes the
             * payload straight out of the page HTML (no URL storage, no
             * fetch). Dedup/budget stay on the URL path only. */
            if (count < slotMax)
            {
                slots[count].isExt = 1;
                slots[count].inlineStart = srcStart;
                slots[count].inlineLen = srcLen;
                slots[count].extIndex = JS_SCRIPT_DATA;
            }
            count++;
            pos = close + 8;
            goto advance;
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
    for (int i = 0; i < extCap; i++)
    {
        ext[i].spill = -1; /* no spill handle (0 is a VALID handle) */
    }

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
    size_t budget = jsext_page_budget();
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
            /* Pre-flight RAM-budget gate: only skips files that could never
             * fit the arena as RAM residents. SW2b: big files are disk-
             * residents and don't consume this budget, so this gate never
             * refuses them — the source ceiling applies at execution. */
            logger_log("[jsext] page budget exhausted: skip %s", ext[i].url);
            ext[i].url[0] = '\0'; /* never fetched, never executed */
            continue;
        }
    }
    /* (RAM budget is enforced against DELIVERED RAM bytes — see
     * jsext_prefetch_step
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

/* Test 10: 72,821 bytes — SW2b: now ACCEPTED (the old per-script download
 * cap is gone); it becomes a RAM-resident source under the page budget and
 * the per-script SOURCE ceiling gates it at execution. */
static const char JSEXT_HUGE[] = JSEXT_PAD1000 JSEXT_PAD300 "window.__huge = 1;\n";
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
    size_t budget = jsext_page_budget();
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
        if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
        {
            /* SW2b: the SOURCE ceiling (execution), not a download cap. */
            logger_log("[jsext] local %s over script source ceiling (%zu)",
                       hit->name, len);
            ext[i].url[0] = '\0';
            continue;
        }
        if (len > JSEXT_SPILL_THRESHOLD)
        {
            /* SW2b: over the RAM-residency threshold — ALWAYS disk-resident
             * (same as the network path), regardless of budget. The arena
             * cannot hold a source this size anyway (small-block chunks). */
            SpillFile sp = pluto_spill_begin();
            if (sp != PLUTO_SPILL_INVALID &&
                pluto_spill_write(sp, hit->source, len) == 0)
            {
                pluto_spill_finish(sp);
                ext[i].body = NULL;
                ext[i].spill = sp;
                ext[i].len = len;
                logger_log("[jsext] local ok %s (%zu bytes, disk-resident)",
                           hit->name, len);
                continue;
            }
            if (sp != PLUTO_SPILL_INVALID)
            {
                pluto_spill_discard(sp);
            }
            logger_log("[jsext] FAIL %s spill write (no disk?)", hit->name);
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

/* ── SW2d: data:-URL script decoder ──────────────────────────────────── */

/* Hex digit value 0-15, or -1 (NUL and non-hex both rejected — unlike
 * strchr, which also matches the terminator). */
static int jsext_hex_val(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* RFC 3986 pct-decode: %XX hex pairs (also decodes '+' as space? NO —
 * data: URLs are NOT form-encoded; '+' is literal). Invalid escapes
 * (short or non-hex) pass through literally, matching lenient web
 * practice. Returns decoded length ≤ srcLen (decoding only shrinks). */
static size_t jsext_pct_decode(const char *src, size_t len, char *out)
{
    size_t r = 0, w = 0;
    while (r < len)
    {
        if (src[r] == '%' && r + 2 < len)
        {
            int v1 = jsext_hex_val(src[r + 1]);
            int v2 = jsext_hex_val(src[r + 2]);
            if (v1 >= 0 && v2 >= 0)
            {
                out[w++] = (char)(v1 * 16 + v2);
                r += 3;
                continue;
            }
        }
        out[w++] = src[r++];
    }
    return w;
}

static int jsext_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode the payload of a data: URL (RFC 2397) into a malloc'd NUL-
 * terminated string (the script source), or NULL on malformed input.
 * Expected shape: data:[mediatype][;base64],payload — everything before
 * the FIRST comma is metadata; the payload may itself contain commas. */
char *jsext_decode_data_script(const char *src, size_t len)
{
    if (!src || len < 6)
    {
        return NULL;
    }
    /* Metadata region: up to the first comma. */
    const char *comma = NULL;
    for (size_t i = 0; i < len; i++)
    {
        if (src[i] == ',')
        {
            comma = &src[i];
            break;
        }
    }
    if (!comma)
    {
        return NULL;
    }
    /* Mediatype params (everything before ','): look for ";base64"
     * (case-insensitive) — the only parameter we act on. */
    int isB64 = 0;
    {
        size_t metaLen = (size_t)(comma - src);
        if (metaLen >= 7)
        {
            /* scan for ";base64" allowing any case */
            for (size_t i = 0; i + 7 <= metaLen; i++)
            {
                if ((src[i] | 0x20) == ';' &&
                    (src[i + 1] | 0x20) == 'b' &&
                    (src[i + 2] | 0x20) == 'a' &&
                    (src[i + 3] | 0x20) == 's' &&
                    (src[i + 4] | 0x20) == 'e' &&
                    (src[i + 5] | 0x20) == '6' &&
                    (src[i + 6] | 0x20) == '4')
                {
                    isB64 = 1;
                    break;
                }
            }
        }
    }
    const char *payload = comma + 1;
    size_t plen = len - (size_t)(payload - src);
    if (plen == 0)
    {
        return NULL;
    }

    if (isB64)
    {
        /* base64: 4 chars → 3 bytes. Skip whitespace; '=' = padding.
         * Ceiling: plen/4*3+3 — allocate exactly that. */
        size_t cap = plen / 4 * 3 + 3;
        unsigned char *buf = (unsigned char *)PLUTO_MALLOC(cap);
        if (!buf)
        {
            return NULL;
        }
        size_t w = 0;
        int quad[4], qn = 0;
        for (size_t i = 0; i < plen; i++)
        {
            int c = (unsigned char)payload[i];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            {
                continue;
            }
            if (c == '=')
            {
                quad[qn++] = -2; /* padding */
            }
            else
            {
                int v = jsext_b64_val(c);
                if (v < 0)
                {
                    PLUTO_FREE(buf);
                    return NULL; /* non-alphabet char: malformed */
                }
                quad[qn++] = v;
            }
            if (qn == 4)
            {
                int a = quad[0], b = quad[1], c2 = quad[2], d = quad[3];
                buf[w++] = (unsigned char)(a << 2 | (b >> 4));
                if (c2 != -2)
                {
                    buf[w++] = (unsigned char)(b << 4 | (c2 >> 2));
                }
                if (d != -2)
                {
                    buf[w++] = (unsigned char)(c2 << 6 | d);
                }
                qn = 0;
            }
        }
        /* Reject a dangling 1- or 2-char final quad (malformed). 3-char is
         * legal (one padding char). */
        if (qn == 1 || qn == 2)
        {
            PLUTO_FREE(buf);
            return NULL;
        }
        if (qn == 3)
        {
            int a = quad[0], b = quad[1], c2 = quad[2];
            buf[w++] = (unsigned char)(a << 2 | (b >> 4));
            buf[w++] = (unsigned char)(b << 4 | (c2 >> 2));
        }
        /* The script source is text; NUL-terminate for run_script callers. */
        char *out = (char *)PLUTO_MALLOC(w + 1);
        if (!out)
        {
            PLUTO_FREE(buf);
            return NULL;
        }
        memcpy(out, buf, w);
        out[w] = '\0';
        PLUTO_FREE(buf);
        return out;
    }

    /* Percent-encoded (or plain) text payload. Decode in place-ish. */
    char *out = (char *)PLUTO_MALLOC(plen + 1);
    if (!out)
    {
        return NULL;
    }
    size_t w = jsext_pct_decode(payload, plen, out);
    out[w] = '\0';
    return out;
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
    int overCap;    /* current file exceeded the response cap (RAM fallback) */
    size_t totalBytes;
};

static JsExtFetch *g_fetch = NULL; /* one session at a time (single-flight) */
static size_t g_lastBytes = 0;

/* Remaining per-page external-JS budget (delivered bytes). Owned by the
 * active session; reset in jsext_prefetch_begin. */
static size_t g_pageBudget = 0;

/* Current-file response sink. SW2b: big bodies are re-spilled to disk
 * (uncapped download — the old 65KB socket cut is gone); small bodies
 * stay arena RAM copies under the page budget. */
static void fetch_on_success(int status, char **keys, char **vals, int hc,
                             const char *body, size_t bodyLen, const char *url)
{
    (void)keys;
    (void)vals;
    (void)hc;
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
    else if (f->overCap)
    {
        /* Legacy RAM fallback path only (spill mode never over-caps). */
        logger_log("[jsext] FAIL %s too big (no spill, cap %d)", url,
                   JSEXT_MAX_RESPONSE);
    }
    else if (!body || bodyLen == 0)
    {
        logger_log("[jsext] FAIL %s empty body", url);
    }
    else if (bodyLen > JSEXT_SPILL_THRESHOLD)
    {
        /* Disk-resident adoption: write the delivered body to a spill file
         * (one sequential flash write; the RAM copy dies with the callback).
         * No page budget: disk residency is the point of SW2b. The per-
         * script SOURCE ceiling applies at materialization (execution). */
        SpillFile sp = pluto_spill_begin();
        if (sp != PLUTO_SPILL_INVALID &&
            pluto_spill_write(sp, body, bodyLen) == 0)
        {
            pluto_spill_finish(sp);
            f->ext[i].body = NULL;
            f->ext[i].spill = sp;
            f->ext[i].len = bodyLen;
            f->totalBytes += bodyLen;
            logger_log("[jsext] ok %s (%zu bytes, disk-resident)", url,
                       bodyLen);
        }
        else
        {
            if (sp != PLUTO_SPILL_INVALID)
            {
                pluto_spill_discard(sp);
            }
            logger_log("[jsext] FAIL %s spill write (no disk?)", url);
        }
    }
    else if (bodyLen > g_pageBudget ||
             bodyLen > pluto_mem_headroom_bytes())
    {
        /* SW3a AUTO-PLACEMENT (the runtime budget manager): RAM residency is
         * granted only while BOTH the page budget AND the live app heap have
         * room — the same uniform threshold for every site, no site knowledge.
         * Under pressure the body goes to DISK instead and materializes
         * just-in-time at execution (SW2b path): behavior is identical, only
         * residency changes. NULL-body slots still run from disk via spill. */
        SpillFile sp = pluto_spill_begin();
        if (sp != PLUTO_SPILL_INVALID &&
            pluto_spill_write(sp, body, bodyLen) == 0)
        {
            pluto_spill_finish(sp);
            f->ext[i].body = NULL;
            f->ext[i].spill = sp;
            f->ext[i].len = bodyLen;
            f->totalBytes += bodyLen;
            logger_log("[jsext] ok %s (%zu bytes, disk-resident [heap-pressure])",
                       url, bodyLen);
        }
        else
        {
            if (sp != PLUTO_SPILL_INVALID)
            {
                pluto_spill_discard(sp);
            }
            logger_log("[jsext] FAIL %s over page budget (%zu left, need %zu) "
                       "and spill write failed",
                       url, g_pageBudget, bodyLen);
        }
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
            logger_log("[jsext] ok %s (%zu bytes, ram-resident, %zu budget left)",
                       url, bodyLen, g_pageBudget);
        }
        else
        {
            logger_log("[jsext] FAIL %s arena OOM", url);
        }
    }
    f->fetching = 0;
    f->idx++; /* advance: otherwise the session re-fetches this file forever */
}

/* Progress sink. SW2b: NO socket cut anymore — http_client streams the
 * body to disk (uncapped), so a 500KB script no longer trips anything
 * here. The sink remains only for logging large downloads in flight.
 * (The legacy overCap path stays armed for the no-spill RAM fallback.) */
static void fetch_on_progress(int cur, int total)
{
    (void)total;
    JsExtFetch *f = g_fetch;
    if (!f || !f->fetching)
    {
        return;
    }
    (void)cur;
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

/* SW3 test hook: 0 = production default (JSBRIDGE_EXT_PAGE_BUDGET). */
static size_t g_pageBudgetOverride = 0;

void jsext_set_page_budget(size_t bytes) { g_pageBudgetOverride = bytes; }

/* Effective per-page RAM budget — override first, production default else.
 * Consulted by collect pre-flight, local fill, and the delivery callback. */
static size_t jsext_page_budget(void)
{
    return g_pageBudgetOverride ? g_pageBudgetOverride
                                : JSBRIDGE_EXT_PAGE_BUDGET;
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
    g_pageBudget = jsext_page_budget();
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
    /* SW2b note: http_client owns the response's spill stream and delivers
     * one materialized body here. fetch_on_success RE-SPILLS bodies over
     * the old per-script cap to disk (one flash write, RAM transient) and
     * adopts that handle as the disk-resident source — the arena never
     * holds big sources. */
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
    /* SW2b: release any disk-resident sources adopted before the abort —
     * the arena free below takes the arrays, but spill files live outside
     * it and would leak until pluto_spill_reset. */
    for (int k = 0; k < f->extCount; k++)
    {
        if (f->ext[k].spill >= 0)
        {
            pluto_spill_discard(f->ext[k].spill);
            f->ext[k].spill = -1;
        }
    }
    if (g_fetch == f)
    {
        g_fetch = NULL;
    }
    jsext_arena_free(f->arena);
    PLUTO_FREE(f);
}

char *jsext_materialize_spill_script(const JsExtScript *e)
{
    if (!e || e->body || e->spill < 0 || e->len == 0)
    {
        return NULL;
    }
    if (e->len > JSBRIDGE_MAX_SCRIPT_SOURCE)
    {
        logger_log("[jsext] %s over script source ceiling (%zu > %d) — skipped",
                   e->url[0] ? e->url : "(url)", e->len,
                   JSBRIDGE_MAX_SCRIPT_SOURCE);
        return NULL;
    }
    char *buf = (char *)PLUTO_MALLOC(e->len + 1);
    if (!buf)
    {
        logger_log("[jsext] materialize OOM (%zu bytes)", e->len);
        return NULL;
    }
    size_t done = 0;
    while (done < e->len)
    {
        size_t want = e->len - done;
        if (want > 16384)
        {
            want = 16384;
        }
        long got = pluto_spill_read(e->spill, (long)done, buf + done, want);
        if (got <= 0)
        {
            logger_log("[jsext] materialize read short at %zu/%zu", done,
                       e->len);
            PLUTO_FREE(buf);
            return NULL;
        }
        done += (size_t)got;
    }
    buf[e->len] = '\0';
    return buf;
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
