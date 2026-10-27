/*
 * PlutoBrowser — image_decoder.c
 * Port of Source/render/image_decoder.lua (reference, 308 lines).
 *
 * Every observable behavior of the reference is reproduced — magic dispatch,
 * async-vs-sync split, sequential queue with dedupe and the isLoading gate,
 * the 16ms re-schedule, cache-entries-are-bitmap-or-false, stall recovery in
 * update(), and the exact placeholder-card painter.
 *
 * C mapping of the Lua control flow:
 *   - decodeRawImageData's async branches (JPEG/PNG/GIF/WebP) schedule a
 *     tasks_run() whose single step performs the decode; onDone/onError map
 *     1:1 onto Tasks.run's onComplete/onError, including the isDecoding flag.
 *   - The sync branches (BMP/ICO/SVG) decode inline (pcall ≈ NULL check).
 *   - processNextImage's playdate.timer.performAfterDelay(16, …) re-schedule
 *     uses the project's pdtimer framework (P4), which fires on the first
 *     frame update after the delay — the same observable pacing.
 *   - The heavy decoders keep their Tasks.yieldCheck() call sites as
 *     documented comments (the P24–P29 architecture: the task layer owns the
 *     frame budget; the decode completes inside one step on device).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"
#include "core/logger.h"

#include "render/image_decoder.h"
#include "core/http_client.h"
#include "core/tasks.h"
#include "core/pluto_mem.h"
#include "core/pluto_spill.h" /* R64 the disk-delivery image receiver */
#include "util/pdtimer.h"
#include "render/style.h"
#include "render/decoders/bmp.h"
#include "render/decoders/svg.h" /* R64b async kind 5 */
#include "render/decoders/gif.h"
#include "render/decoders/png.h"
#include "render/decoders/svg.h"
#include "render/decoders/tif.h"
#include "render/decoders/tga.h"
#include "render/decoders/psd.h"
#include "render/decoders/sgi.h"
#include "render/decoders/xbm.h"
#include "render/decoders/pdfimg.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/ico.h"
#include "render/decoders/webp.h"
#include "render/decoders/avif.h"

static PlaydateAPI *pd = NULL;

/* ── Module state (Lua locals imageCache/downloadQueue/isDownloading/isDecoding) ── */

#define IMGDEC_QUEUE_CAP 64
#define IMGDEC_URL_MAX   512

typedef struct ImgCacheEntry
{
    char url[IMGDEC_URL_MAX];
    LCDBitmap *bmp; /* NULL == Lua's `false` (failed); absent key == not tried */
    int present;
} ImgCacheEntry;

#define IMGDEC_CACHE_CAP 256
static ImgCacheEntry g_cache[IMGDEC_CACHE_CAP];
static int g_cacheCount = 0;

static char g_queue[IMGDEC_QUEUE_CAP][IMGDEC_URL_MAX];
static int g_queueCount = 0;

static int g_isDownloading = 0;
static int g_isDecoding = 0;

/* R64: the retry pacing for transient refusal errors (see http_on_error):
 * a refused fetch re-tries on this cadence instead of the 16ms pacing,
 * so a funnel-pressure moment gives the resync window time to reopen
 * headroom before the URL's next attempt. */
static unsigned g_refusalRetryBackoff = 1000;
static char g_currentUrl[IMGDEC_URL_MAX]; /* URL being downloaded (for callbacks) */

/* ── Cache helpers ────────────────────────────────────────────────────────── */

static ImgCacheEntry *cache_find(const char *url)
{
    if (!url || !url[0]) return NULL;
    for (int i = 0; i < g_cacheCount; i++)
        if (strncmp(g_cache[i].url, url, IMGDEC_URL_MAX) == 0)
            return &g_cache[i];
    return NULL;
}

/* TEMP R64: one-line probes for the device placeholder loop receipt
 * (who re-enqueues, who puts, who misses — a fingerprint of the
 * key set at BOTH sides). The Mookie-style non-thumb unscaled URL
 * is the receipted 20-decode looper. */
static void cache_probe(const char *tag, const char *url)
{
    if (url && strstr(url, "Mookie_Wilson") != NULL)
    {
        logger_log("[cacheprobe] %s len=%zu tail=%.60s count=%d",
                   tag, url ? strlen(url) : 0, (url + (strlen(url) > 60 ? strlen(url) - 60 : 0)),
                   g_cacheCount);
    }
}
static void cache_put(const char *url, LCDBitmap *bmp)
{
    if (!url || !url[0]) return;
    ImgCacheEntry *e = cache_find(url);
    if (e)
    {
        if (e->bmp && e->bmp != bmp) pd->graphics->freeBitmap(e->bmp);
        e->bmp = bmp;
        e->present = 1;
        cache_probe("put-hit", url);
        return;
    }
    cache_probe("put-new", url);
    if (g_cacheCount >= IMGDEC_CACHE_CAP)
    {
        /* Full: first drop a NEGATIVE entry (failed URLs are only hints),
         * else drop the oldest entry (FIFO). Never drop silently on put. */
        int victim = -1;
        for (int i = 0; i < g_cacheCount; i++)
        {
            if (!g_cache[i].bmp)
            {
                victim = i;
                break;
            }
        }
        if (victim < 0)
        {
            victim = 0;
        }
        if (g_cache[victim].bmp)
        {
            pd->graphics->freeBitmap(g_cache[victim].bmp);
        }
        for (int j = victim + 1; j < g_cacheCount; j++)
            g_cache[j - 1] = g_cache[j];
        g_cacheCount--;
    }
    e = &g_cache[g_cacheCount++];
    snprintf(e->url, IMGDEC_URL_MAX, "%s", url);
    e->bmp = bmp;
    e->present = 1;
}

static void process_next_timer(void *userdata);
static void process_next(void);

/* ── Async decode task (one step does the work; mirrors Tasks.run's contract) ── */

typedef struct AsyncDecodeState
{
    uint8_t *data;   /* owned copy of the body */
    size_t len;
    char url[IMGDEC_URL_MAX];
    int kind;        /* 0 jpeg, 1 png, 2 webp, 3 gif, 4 avif, 5 svg */
    int phase;       /* 0 = decode pending, 1 = finished */
    LCDBitmap *result;
    TaskCtx *ctx;    /* set by the step for yield_check parity */
} AsyncDecodeState;

static int async_decode_step(TaskCtx *ctx)
{
    AsyncDecodeState *st = (AsyncDecodeState *)ctx->data;
    if (!st || st->phase != 0) return 0;
    st->ctx = ctx;
    /* Tasks.yieldCheck() call site (task layer owns the frame budget) —
     * JPEG/PNG/GIF/WebP decode loops check it per row/MCU in the Lua ref. */
    switch (st->kind)
    {
    case 0: st->result = jpeg_decode(st->data, st->len, 360, 200); break;
    case 1: st->result = png_decode(st->data, st->len, 360, 200); break;
    case 2: st->result = webp_decode(st->data, st->len, 360, 200); break;
    case 3: st->result = gif_decode(st->data, st->len, 360, 200); break;
    case 5: st->result = svg_decode((const char *)st->data, 360, 200); break;
    default: st->result = avif_decode(st->data, st->len); break;
    }

    st->phase = 1;
    /* Returning 0 completes the task; ctx->data is delivered to onDone. */
    ctx->data = st;
    return 0;
}

static void async_decode_done(void *result, void *userdata)
{
    AsyncDecodeState *st = (AsyncDecodeState *)result;
    (void)userdata;
    g_isDecoding = 0;
    if (!st) return;
    /* P33 diagnostics: decode outcome per image. */
    if (st->result)
    {
        int iw = 0, ih = 0;
        pd->graphics->getBitmapData(st->result, &iw, &ih, NULL, NULL, NULL);
        logger_log("IMGDEC ok %s kind=%d %dx%d (%zu bytes)",
                   st->url, st->kind, iw, ih, st->len);
    }
    else
    {
        logger_log("IMGDEC FAIL %s kind=%d (%zu bytes)",
                   st->url, st->kind, st->len);
    }
    /* Lua: imageCache[url] = img or false */
    cache_put(st->url, st->result); /* NULL stored as the "false" entry */
    free(st->data);
    free(st);
    /* Lua: playdate.timer.performAfterDelay(16, processNextImage) */
    pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
}

static void async_decode_error(const char *message, void *userdata)
{
    (void)message;
    (void)userdata;
    g_isDecoding = 0;
    pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
}

static void process_next(void);

/* ── decodeRawImageData (magic dispatch) ──────────────────────────────────── */

/* R64b: SVG magic scan for the async-kind routing — an SVG file's
 * markup may start with a decl, a comment, whitespace, or the <svg root
 * directly. Bounded scan: first 200 bytes. */
static int svg_magic(const uint8_t *data, size_t len)
{
    const char *s = (const char *)data;
    size_t n = len < 200 ? len : 200;
    for (size_t i = 0; i + 5 <= n; i++)
    {
        if (s[i] == '<' && strncmp(s + i + 1, "svg", 3) == 0)
        {
            char c = s[i + 4];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' ||
                c == '/')
            {
                return 1;
            }
        }
    }
    return 0;
}

static void decode_raw_image_data(const uint8_t *data, size_t len, const char *url)
{
    if (!data || len < 4)
    {
        /* onDone(nil): caller stores `false`. */
        cache_put(url, NULL);
        g_isDecoding = 0;
        return;
    }    uint8_t b1 = data[0], b2 = data[1], b3 = data[2], b4 = data[3];

    int kind = -1; /* -1 = not async */
    if (b1 == 0xFF && b2 == 0xD8)
        kind = 0; /* JPEG */
    else if (b1 == 0x89 && b2 == 0x50 && b3 == 0x4E && b4 == 0x47)
        kind = 1; /* PNG */
    else if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
        kind = 2; /* WebP */
    else if (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)
        kind = 3; /* GIF */
    else if (len >= 12 && memcmp(data + 4, "ftyp", 4) == 0 &&
             (memcmp(data + 8, "avif", 4) == 0 ||
              memcmp(data + 8, "avis", 4) == 0))
        kind = 4; /* AVIF stills (mif1 alone may be HEIC: not claimed) */
    else if (svg_magic(data, len))
        kind = 5; /* R64b: SVGs ride the ASYNC task pipeline — the receipted
                     sync-in-deliver path stalled the OS lane's frame for
                     200-400ms per big SVG (wikipedia receipted: enwiki-25
                     + wordmark + tagline inline during delivery). Kind 5
                     moves them into the queue's step contract. */

    if (kind >= 0)
    {
        AsyncDecodeState *st = (AsyncDecodeState *)calloc(1, sizeof(AsyncDecodeState));
        if (st)
        {
            st->data = (uint8_t *)malloc(len);
            if (st->data)
            {
                memcpy(st->data, data, len);
                st->len = len;
                snprintf(st->url, IMGDEC_URL_MAX, "%s", url ? url : "");
                st->kind = kind;
                g_isDecoding = 1;
                if (tasks_run(async_decode_step, st, async_decode_done,
                              async_decode_error, NULL) != 0)
                {
                    /* Queue full — behave like the error path. */
                    g_isDecoding = 0;
                    free(st->data);
                    free(st);
                    cache_put(url, NULL);
                }
                return;
            }
            free(st);
        }
        cache_put(url, NULL);
        g_isDecoding = 0;
        return;
    }

    LCDBitmap *img = NULL;

    if (data[0] == 'B' && data[1] == 'M')
    {
        img = bmp_decode(data, len); /* pcall: NULL on error */
    }
    else if (b1 == 0 && b2 == 0 && (b3 == 1 || b3 == 2) && b4 == 0 &&
             !(len >= 18 && data[2] == 2 && data[3] == 0 &&
               data[16] >= 8 && data[16] <= 32))
    {
        img = ico_decode(data, len, 360, 200); /* ICO / CUR */
    }
    else if ((b1 == 'I' && b2 == 'I' && b3 == '*' && b4 == 0) ||
             (b1 == 'M' && b2 == 'M' && b3 == 0 && b4 == '*'))
    {
        img = tif_decode(data, len); /* TIFF */
    }
    else if (len >= 18 && b1 == 0x01 && b2 == 0xDA)
    {
        img = sgi_decode(data, len); /* SGI (0x01DA magic at offset 0) */
    }
    else if (len >= 26 && memcmp(data, "8BPS", 4) == 0)
    {
        img = psd_decode(data, len); /* Photoshop */
    }
    else if (len >= 5 && memcmp(data, "%PDF-", 5) == 0)
    {
        img = pdfimg_decode(data, len); /* PDF embedded raster */
    }
    else if (len > 32 && (memcmp(data, "#define", 7) == 0))
    {
        img = xbm_decode(data, len); /* X BitMap (ASCII) */
    }
    else if (len >= 18 && (data[1] == 0 || data[1] == 1) &&
             (data[2] == 1 || data[2] == 2 || data[2] == 3 ||
              data[2] == 9 || data[2] == 10 || data[2] == 11) &&
             data[16] >= 8 && data[16] <= 32)
    {
        img = tga_decode(data, len); /* TGA last (weak header) */
    }
    else
    {
        char head[201];
        size_t hl = len < 200 ? len : 200;
        memcpy(head, data, hl);
        head[hl] = '\0';
        for (size_t i = 0; i < hl; i++)
            head[i] = (char)((head[i] >= 'A' && head[i] <= 'Z') ? head[i] + 32 : head[i]);
        if (strstr(head, "<svg") || strstr(head, "<?xml"))
            img = svg_decode((const char *)data, 360, 200);
    }

    /* onDone(img or nil) → img or false */
    if (!img)
    {
        logger_log("IMGDEC FAIL %s sync (head %02x %02x %02x %02x, %zu bytes)",
                   url, b1, b2, b3, b4, len);
    }
    else
    {
        int iw = 0, ih = 0;
        pd->graphics->getBitmapData(img, &iw, &ih, NULL, NULL, NULL);
        logger_log("IMGDEC ok %s sync %dx%d (%zu bytes)", url, iw, ih, len);
    }
    cache_put(url, img);
}

/* ── Download queue (processNextImage) ────────────────────────────────────── */

static void http_on_success(int status, char **headerKeys, char **headerVals,
                            int headerCount, const char *body, size_t bodyLen,
                            const char *url)
{
    (void)status; (void)headerKeys; (void)headerVals; (void)headerCount; (void)url;
    /* The busy flag is released by the decode completion / the failure path
     * below; the queue re-schedules 16ms after the cache write. */
    /* Lua: if body and #body > 8 → decodeRawImageData(body, url, cb);
     * the decode completion (or the sync path below) writes the cache and
     * re-schedules the queue after 16ms.
     * NOTE: bodyLen is the BYTE length from the HTTP client — binary image
     * bodies contain NUL bytes, so strlen() must never be used here (the
     * pre-P33 build truncated every PNG/JPEG/GIF/WebP at its first NUL). */
    if (body && bodyLen > 8)
    {
        /* TEMP AVIF pixel hunt: hash the exact bytes being decoded. */
        {
            unsigned h = 2166136261u;
            const uint8_t *bb = (const uint8_t *)body;
            size_t hi;
            for (hi = 0; hi < bodyLen; hi++)
            {
                h ^= bb[hi];
                h *= 16777619u;
            }
            logger_log("[imgbytes] %s len=%zu fnv=%08x head=%02x%02x%02x%02x%02x%02x%02x%02x",
                       g_currentUrl, bodyLen, h, bb[0], bb[1], bb[2], bb[3],
                       bb[4], bb[5], bb[6], bb[7]);
        }
        size_t blen = bodyLen;
        int async = 1;
        uint8_t b1 = (uint8_t)body[0];
        if (blen >= 4)
        {
            uint8_t b2 = (uint8_t)body[1], b3 = (uint8_t)body[2], b4 = (uint8_t)body[3];
            if ((b1 == 0xFF && b2 == 0xD8) ||
                (b1 == 0x89 && b2 == 0x50 && b3 == 0x4E && b4 == 0x47) ||
                (blen > 8 && memcmp(body, "RIFF", 4) == 0 && memcmp(body + 8, "WEBP", 4) == 0) ||
                (blen > 5 && (memcmp(body, "GIF87a", 6) == 0 || memcmp(body, "GIF89a", 6) == 0)))
                async = 1;
            else
                async = 0; /* BMP/ICO/SVG decode synchronously */
        }
        if (async)
        {
            g_isDownloading = 0;
            decode_raw_image_data((const uint8_t *)body, blen, g_currentUrl);
            if (!g_isDecoding)
            {
                /* Sync dispatch already cached — re-schedule now. */
                pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
            }
        }
        else
        {
            g_isDownloading = 0;
            decode_raw_image_data((const uint8_t *)body, blen, g_currentUrl);
            pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
        }
    }
    else
    {
        cache_put(g_currentUrl, NULL);
        g_isDownloading = 0;
        pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
    }
}

static void http_on_error(const char *message)
{
    /* R64: the dispatcher's immediate-refusal class ("OS-HTTP request
     * refused (no wire started)." — fired by the front door when the
     * OS backend refuses BEFORE arming a request, e.g. a transient
     * funnel-budget malloc refusal) is NOT a content failure: caching
     * it here made a momentary resource condition a PERMANENT
     * placeholder for that URL (the wikipedia receipted class). No
     * negative entry: the URL leaves the cache unwritten, so the next
     * in-view re-enqueue (layout's per-frame enqueue policy) retries
     * it like any never-fetched image. Real transport/content failures
     * keep the receipted negative-cache behavior below. */
    int transientRefusal =
        message &&
        strncmp(message, "OS-HTTP request refused", 22) == 0;
    if (!transientRefusal)
    {
        cache_put(g_currentUrl, NULL);
    }
    g_isDownloading = 0;
    pdtimer_perform_after_delay(pd, g_refusalRetryBackoff, process_next_timer,
                                NULL);
}

/* R64: the disk-delivery receiver (general-purpose, any site, both
 * backends). The channel streamed the body to disk via the receipted
 * SW2b/R15 spill machinery; ADOPT the handle, materialize the body
 * ONCE for the decode batch (the decoders take a RAM pointer — the
 * materialize-then-discard pattern is the receipted consumer shape),
 * then feed the normal decode path and own the buffer. The channel's
 * own RAM staging copy and its 2MB RAM-cap delivery class are gone
 * for spilled bodies; net decode-time RAM becomes exactly ONE body
 * copy instead of channel + copy. */
static void http_on_success_spill(int status, SpillFile spill,
                                  size_t bodyLen, const char *url)
{
    (void)status;
    /* R64b: key by the QUEUE's own string (g_currentUrl) — exactly what
     * layout enqueued and what layout's draw lookup asks for. The
     * delivered-url parameter is the channel's OWN snapshot (receipted
     * truncation class: the 128-char delivery snap cut the cache key at
     * 127 while the lookup string runs longer — receipted on the
     * wikipedia page: enqueue=153 chars vs put=127, every long URL
     * decode-ok'd yet drew its placeholder). Using the queue string
     * aligns put/get at the consumer for EVERY site and every URL
     * length; the shorter delivered key never enters the cache. */
    const char *urlKey = g_currentUrl;
    uint8_t *body = (bodyLen > 0)
                        ? (uint8_t *)pluto_mem_realloc(NULL, bodyLen + 1)
                        : NULL;
    if (!body)
    {
        /* Read/alloc failure: adopt-discard + negative cache (same
         * shape as the RAM receiver's empty body). */
        if (spill != PLUTO_SPILL_INVALID)
        {
            pluto_spill_discard(spill);
        }
        cache_put(urlKey, NULL);
        g_isDownloading = 0;
        pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
        return;
    }
    size_t off = 0;
    while (off < bodyLen)
    {
        long r = pluto_spill_read(spill, (long)off, body + off,
                                  bodyLen - off);
        if (r <= 0)
        {
            break;
        }
        off += (size_t)r;
    }
    pluto_spill_discard(spill);
    if (off < bodyLen)
    {
        pluto_mem_realloc(body, 0);
        cache_put(urlKey, NULL);
        g_isDownloading = 0;
        pdtimer_perform_after_delay(pd, 16, process_next_timer, NULL);
        return;
    }
    body[bodyLen] = '\0'; /* RAM-delivery parity */

    decode_raw_image_data(body, bodyLen, urlKey);
    /* Ownership: the decode path either copied the bytes into its own
     * state (async kinds) or finished synchronously — the caller of the
     * spill receiver is the only bone holding this buffer. */
    pluto_mem_realloc(body, 0);
}

static void process_next(void)
{
    if (g_isDownloading || g_isDecoding || g_queueCount == 0) return;
    /* Don't download images while the main page is loading. */
    if (http_is_loading()) return;

    /* The cache-hit skip is a LOOP, not self-recursion: the recursive form
     * burned one ~552B gameTask frame per consecutive cached entry (queue
     * cap 64 ⇒ ~35KB in one timer tick) — measured device stack overflow
     * 2026-09-24 19:37, right after image downloads first started working
     * on hardware (spill-reassembly fix). The timer path runs this with an
     * XS timer fire nested in the same pdtimer_update pass, so those frames
     * stacked on top of a live JS callback chain. Identical semantics: pop
     * entries until an uncached URL is found (then download it) or the
     * queue drains. */
    while (g_queueCount > 0 && !g_isDownloading && !g_isDecoding &&
           !http_is_loading())
    {
        char url[IMGDEC_URL_MAX];
        snprintf(url, IMGDEC_URL_MAX, "%s", g_queue[0]);
        for (int i = 1; i < g_queueCount; i++)
            memcpy(g_queue[i - 1], g_queue[i], IMGDEC_URL_MAX);
        g_queueCount--;

        if (cache_find(url))
        {
            /* Present (bitmap or Lua-parity `false`) → drop the entry and
             * keep popping. A failed URL keeps its negative entry, exactly
             * like the reference, so an in-view per-frame enqueue() cannot
             * loop forever on a host that always errors (seen live:
             * api.flattr.com → 177 retries). Eviction modes delete the
             * entry outright (imgdec_evict), which is what makes a later
             * re-fetch legal. */
            continue;
        }

        snprintf(g_currentUrl, IMGDEC_URL_MAX, "%s", url);
        g_isDownloading = 1;
        /* R63: the display-image fetch rides the IMAGE lane front door.
         * Identical GET/callback contract to http_get; the dispatcher
         * only uses the hint under the Hybrid setting (images → OS HTTP
         * stack), byte-identical routing on TCP/OS-HTTP settings. The
         * image queue's own gating is untouched (still refuses to start
         * while http_is_loading() is true over both backends).
         * R64: the lane ALSO advertises the disk-delivery receiver, so
         * spilled bodies never RAM-stage in the channel — every site,
         * any size, same decode path. */
        HttpCallbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.onSuccess = http_on_success;
        cb.onSuccessSpill = http_on_success_spill;
        cb.onError = http_on_error;
        if (!http_get_image(url, &cb))
        {
            /* Immediate failure: onError already fired (cache + timer
             * scheduled). Return, exactly like the recursive original, so
             * the next URL is attempted on the timer's 16ms pacing rather
             * than hammered in this same tick. */
            g_isDownloading = 0;
            return;
        }
        break; /* download started — callbacks own the next step */
    }
}
static void process_next_timer(void *userdata)
{
    (void)userdata;
    process_next();
}


/* ── Public API ───────────────────────────────────────────────────────────── */

void imgdec_init(PlaydateAPI *api) { pd = api; }

void imgdec_clear_cache(void)
{
    for (int i = 0; i < g_cacheCount; i++)
        if (g_cache[i].bmp) pd->graphics->freeBitmap(g_cache[i].bmp);
    g_cacheCount = 0;
    g_queueCount = 0;
    g_isDownloading = 0;
    g_isDecoding = 0;
}

void imgdec_enqueue(const char *src)
{
    if (!src || !src[0]) return;
    if (strstr(src, "Mookie_Wilson") != NULL)
    {
        logger_log("[cacheprobe] enqueue len=%zu tail=%.60s inqueue=%d cache=%d",
                   strlen(src),
                   (src + (strlen(src) > 60 ? strlen(src) - 60 : 0)),
                   g_queueCount, g_cacheCount); /* TEMP R64 */
    }
    if (cache_find(src)) return;
    for (int i = 0; i < g_queueCount; i++)
        if (strncmp(g_queue[i], src, IMGDEC_URL_MAX) == 0) return;
    if (g_queueCount >= IMGDEC_QUEUE_CAP) return;
    snprintf(g_queue[g_queueCount++], IMGDEC_URL_MAX, "%s", src);
}

void imgdec_evict(const char *src)
{
    if (!src) return;
    /* Don't delete the URL currently downloading: its callbacks would
     * re-insert a fresh entry mid-flight (cache_put on completion), which
     * desyncs the queue bookkeeping. */
    if (g_isDownloading && strcmp(g_currentUrl, src) == 0) return;
    for (int i = 0; i < g_cacheCount; i++)
    {
        if (strncmp(g_cache[i].url, src, IMGDEC_URL_MAX) == 0)
        {
            if (g_cache[i].bmp) pd->graphics->freeBitmap(g_cache[i].bmp);
            for (int j = i + 1; j < g_cacheCount; j++)
                g_cache[j - 1] = g_cache[j];
            g_cacheCount--;
            return;
        }
    }
}

int imgdec_is_decoded(const char *src)
{
    if (!src || !src[0]) return 0;
    ImgCacheEntry *e = cache_find(src);
    return e && e->bmp;
}

LCDBitmap *imgdec_get_image(const char *src)
{
    if (!src || !src[0]) return NULL;
    ImgCacheEntry *e = cache_find(src);
    return e ? e->bmp : NULL;
}

int imgdec_is_cached(const char *src)
{
    return cache_find(src) != NULL;
}


void imgdec_update(void)
{
    /* Stall recovery: a download whose HTTP client went idle (cancelled by
     * navigation) will never fire callbacks — release the flag. */
    if (g_isDownloading && !http_is_loading())
        g_isDownloading = 0;
    /* A decode task cancelled by navigation never runs onDone. */
    if (g_isDecoding && !tasks_is_running())
        g_isDecoding = 0;
    if (!g_isDownloading && !g_isDecoding && g_queueCount > 0 && !http_is_loading())
        process_next();
}

/* ── draw() ───────────────────────────────────────────────────────────────── */

void imgdec_draw(int x, int y, int w, int h, const char *altText,
                 const char *href, int isSelected, const char *src)
{
    (void)href;
    x = x ? x : 0;
    y = y ? y : 0;
    if (w < 40) w = 40;
    if (h < 20) h = 20;
    if (w > 360) w = 360;
    if (h > 180) h = 180;

    /* Cached image path */
    if (src && src[0])
    {
        ImgCacheEntry *e = cache_find(src);
        if (e && e->bmp)
        {
            int iw = 0, ih = 0, rb = 0;
            uint8_t *mask = NULL, *dat = NULL;
            pd->graphics->getBitmapData(e->bmp, &iw, &ih, &rb, &mask, &dat);
            if (iw > 0 && ih > 0)
            {
                float scaleX = (float)w / (float)iw;
                float scaleY = (float)h / (float)ih;
                float scale = scaleX < scaleY ? scaleX : scaleY;
                int dw = (int)(iw * scale);
                int dh = (int)(ih * scale);
                int dx = x + (w - dw) / 2;
                int dy = y + (h - dh) / 2;
                pd->graphics->drawScaledBitmap(e->bmp, dx, dy, scale, scale);
            }
            else
            {
                pd->graphics->drawBitmap(e->bmp, x, y, kBitmapUnflipped);
            }
            if (isSelected)
            {
                pd->graphics->drawRoundRect(x, y, w, h, 4, 2, kColorBlack);
            }
            return;
        }
        if (!e)
            imgdec_enqueue(src);
    }

    /* Loading placeholder card */
    pd->graphics->fillRoundRect(x, y, w, h, 4, kColorWhite);
    pd->graphics->drawRoundRect(x, y, w, h, 4, 1, kColorBlack);

    /* Hatch pattern */
    for (int hx = x + 4; hx <= x + w - 4; hx += 10)
        pd->graphics->drawLine(hx, y + 3, hx, y + h - 3, 1, kColorBlack);

    /* Camera icon */
    int iconX = x + w / 2 - 8;
    int iconY = y + h / 2 - 6;
    pd->graphics->drawRoundRect(iconX, iconY, 16, 11, 2, 1, kColorBlack);
    /* Reference: gfx.fillCircleAtPoint(iconX + 8, iconY + 5, 3) — inclusive
     * pixel convention ⇒ 7×7 bounding box. */
    pd->graphics->fillEllipse(iconX + 8 - 3, iconY + 5 - 3, 7, 7, 0.f, 360.f, kColorBlack);
    pd->graphics->fillRect(iconX + 13, iconY + 1, 1, 1, kColorBlack);

    /* Alt text */
    if (altText && altText[0] && h > 30)
    {
        LCDFont *font = style_font(PLUTO_FONT_SMALL);
        char label[32];
        if (strlen(altText) > 26)
        {
            strncpy(label, altText, 23);
            label[23] = '\0';
            strcat(label, "...");
        }
        else
        {
            snprintf(label, sizeof(label), "%s", altText);
        }
        int lw = style_get_text_width(PLUTO_FONT_SMALL, label);
        int lx = x + (w - lw) / 2;
        int ly = iconY + 14;
        /* Only draw the label when it fits INSIDE the card: an over-wide
         * label would spill onto neighboring content with no clipping
         * (seen as garbage glyphs around small/loading images). */
        if (lw <= w && ly + 10 <= y + h - 2)
        {
            pd->graphics->setFont(font);
            pd->graphics->drawText(label, (size_t)strlen(label), kUTF8Encoding, lx, ly);
        }
    }

    pd->graphics->setDrawMode(kDrawModeCopy);
}

/* ── Test hook ────────────────────────────────────────────────────────────── */

typedef struct TestDecodeState
{
    const uint8_t *data;
    size_t len;
    LCDBitmap *result;
    int done;
} TestDecodeState;

static int test_decode_step(TaskCtx *ctx)
{
    TestDecodeState *t = (TestDecodeState *)ctx->userdata;
    /* Run the dispatch inline: temporarily neutralize the async split so the
     * result is available synchronously (the reference's _testDecode had the
     * same caveat — async formats were not covered by it). */
    uint8_t b1 = t->data[0], b2 = t->len > 1 ? t->data[1] : 0;
    int isJpeg = (b1 == 0xFF && b2 == 0xD8);
    if (isJpeg)
        t->result = jpeg_decode(t->data, t->len, 360, 200);
    else if (t->len > 8 && memcmp(t->data, "RIFF", 4) == 0 && memcmp(t->data + 8, "WEBP", 4) == 0)
        t->result = webp_decode(t->data, t->len, 360, 200);
    else if (t->len > 3 && b1 == 0x89 && b2 == 0x50)
        t->result = png_decode(t->data, t->len, 360, 200);
    else if (t->len > 5 && (memcmp(t->data, "GIF87a", 6) == 0 || memcmp(t->data, "GIF89a", 6) == 0))
        t->result = gif_decode(t->data, t->len, 360, 200);
    else if (t->len > 1 && t->data[0] == 'B' && t->data[1] == 'M')
        t->result = bmp_decode(t->data, t->len);
    else if (t->len > 3 && t->data[0] == 0 && t->data[1] == 0 && (t->data[2] == 1 || t->data[2] == 2) && t->data[3] == 0)
        t->result = ico_decode(t->data, t->len, 360, 200);
    else
    {
        char head[201];
        size_t hl = t->len < 200 ? t->len : 200;
        memcpy(head, t->data, hl);
        head[hl] = '\0';
        if (strstr(head, "<svg") || strstr(head, "<?xml"))
            t->result = svg_decode((const char *)t->data, 360, 200);
    }
    t->done = 1;
    ctx->data = t;
    return 0;
}

LCDBitmap *imgdec_test_decode(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return NULL;
    TestDecodeState t;
    memset(&t, 0, sizeof(t));
    t.data = data;
    t.len = len;
    /* test_decode_step expects a real TaskCtx (it reads ctx->userdata and
     * writes ctx->data) — a stack ctx with userdata = &t. */
    TaskCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.userdata = &t;
    test_decode_step(&ctx);
    return t.result;
}
