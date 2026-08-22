// image_decoder.c — C port of Source/render/image_decoder.lua (ImageDecoder).
//
// Sequential image pipeline: FIFO download queue drained one URL at a time
// through HttpClient while no page load is active, magic-byte format
// dispatch (JPEG/PNG/WebP/GIF async via the task scheduler; BMP/ICO/SVG
// inline), url -> bitmap|failed cache. See image_decoder.h for the full
// parity notes.

#include "render/image_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_ID_PD 1
#include "pd_api.h"
#endif

#include "core/http_client.h"
#include "core/logger.h"
#include "core/tasks.h"
#include "render/decoders/bmp.h"
#include "render/decoders/gif.h"
#include "render/decoders/ico.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/png.h"
#include "render/decoders/svg.h"
#include "render/decoders/webp.h"
#include "render/style.h"
#include "util/mem.h"

#ifdef PLUTO_ID_PD
static PlaydateAPI* s_pd = NULL;
#endif

void id_init(struct PlaydateAPI* pd) {
#ifdef PLUTO_ID_PD
    s_pd = pd;
#else
    (void)pd;
#endif
}

/* ------------------------------------------------------------------ */
/* cache + queue state                                                 */

typedef struct {
    char* url;
    struct LCDBitmap* img;   /* NULL == Lua false sentinel */
} CacheEnt;

static CacheEnt* s_cache = NULL;
static int s_cacheN = 0, s_cacheCap = 0;

static char** s_queue = NULL;
static int s_qN = 0, s_qCap = 0;

static int s_isDownloading = 0;
static int s_isDecoding = 0;

static CacheEnt* cache_find(const char* url) {
    int i;
    for (i = 0; i < s_cacheN; i++)
        if (strcmp(s_cache[i].url, url) == 0) return &s_cache[i];
    return NULL;
}

static void cache_put(const char* url, struct LCDBitmap* img) {
    CacheEnt* e = cache_find(url);
    if (e) {
        if (e->img && e->img != img) {
#ifdef PLUTO_ID_PD
            s_pd->graphics->freeBitmap((LCDBitmap*)e->img);
#endif
        }
        e->img = img;
        return;
    }
    if (s_cacheN == s_cacheCap) {
        int ncap = s_cacheCap ? s_cacheCap * 2 : 8;
        CacheEnt* nc =
            pluto_realloc(s_cache, (size_t)ncap * sizeof(CacheEnt));
        if (!nc) return;
        s_cache = nc;
        s_cacheCap = ncap;
    }
    s_cache[s_cacheN].url = pluto_strdup(url);
    if (!s_cache[s_cacheN].url) return;
    s_cache[s_cacheN].img = img;
    s_cacheN++;
}

static void cache_drop_at(int idx) {
    if (idx < 0 || idx >= s_cacheN) return;
    if (s_cache[idx].img) {
#ifdef PLUTO_ID_PD
        s_pd->graphics->freeBitmap((LCDBitmap*)s_cache[idx].img);
#endif
        s_cache[idx].img = NULL;
    }
    pluto_free(s_cache[idx].url);
    memmove(&s_cache[idx], &s_cache[idx + 1],
            (size_t)(s_cacheN - idx - 1) * sizeof(CacheEnt));
    s_cacheN--;
}

void id_clear_cache(void) {
    while (s_cacheN > 0) cache_drop_at(s_cacheN - 1);
    while (s_qN > 0) pluto_free(s_queue[--s_qN]);
    s_isDownloading = 0;
    s_isDecoding = 0;
}

int id_is_cached(const char* src) {
    if (!src || !src[0]) return 0;
    return cache_find(src) != NULL;
}

int id_is_decoded(const char* src) {
    CacheEnt* e;
    if (!src || !src[0]) return 0;
    e = cache_find(src);
    return e != NULL && e->img != NULL;
}

struct LCDBitmap* id_get_image(const char* src) {
    CacheEnt* e;
    if (!src || !src[0]) return NULL;
    e = cache_find(src);
    if (e && e->img) return e->img;
    return NULL;
}

void id_evict(const char* src) {
    int i;
    if (!src) return;
    for (i = 0; i < s_cacheN; i++) {
        if (strcmp(s_cache[i].url, src) == 0) {
            cache_drop_at(i);
            return;
        }
    }
}

void id_enqueue(const char* src) {
    int i;
    if (!src || !src[0]) return;
    if (cache_find(src)) return;
    for (i = 0; i < s_qN; i++)
        if (strcmp(s_queue[i], src) == 0) return;
    if (s_qN == s_qCap) {
        int ncap = s_qCap ? s_qCap * 2 : 8;
        char** nq = pluto_realloc(s_queue, (size_t)ncap * sizeof(char*));
        if (!nq) return;
        s_queue = nq;
        s_qCap = ncap;
    }
    s_queue[s_qN] = pluto_strdup(src);
    if (s_queue[s_qN]) s_qN++;
}

/* ------------------------------------------------------------------ */
/* decode dispatch                                                     */

typedef void (*IdDoneFn)(void* ud, struct LCDBitmap* img);

typedef struct IdJob {
    uint8_t* data;           /* owned copy for async formats */
    size_t len;
    int fmt;                 /* 0 jpeg 1 png 2 webp 3 gif */
    IdDoneFn done;
    void* ud;
} IdJob;

static int job_step(void* ctx) {
    IdJob* j = (IdJob*)ctx;
#ifdef PLUTO_ID_PD
    struct LCDBitmap* img = NULL;
    switch (j->fmt) {
        case 0:
            img = jpeg_decode(s_pd, j->data, j->len, 360, 200);
            break;
        case 1:
            img = png_decode(s_pd, j->data, j->len, 360, 200);
            break;
        case 2:
            img = webp_decode(s_pd, j->data, j->len, 360, 200);
            break;
        default:
            img = gif_decode(s_pd, j->data, j->len, 360, 200);
            break;
    }
    /* deliver inside the scheduler frame, like the Lua coroutine result */
    j->done(j->ud, img);
#else
    (void)j;
#endif
    return PLUTO_TASK_DONE;
}

/* results are delivered from job_step; nothing to do here */
static void job_on_complete(void* ctx, void* ud) { (void)ctx; (void)ud; }

/* Lua parity: err message discarded, onDone(nil) fires */
static void job_on_error(const char* msg, void* ud) {
    IdJob* j = (IdJob*)ud;
    (void)msg;
    if (j && j->done) j->done(j->ud, NULL);
}

static void job_ctxfree(void* ctx) {
    IdJob* j = (IdJob*)ctx;
    if (!j) return;
    pluto_free(j->data);
    pluto_free(j);
}

static void decode_async(int fmt, const uint8_t* data, size_t len,
                         IdDoneFn done, void* ud) {
    IdJob* j = (IdJob*)pluto_malloc(sizeof(IdJob));
    uint8_t* copy;
    if (!j) { done(ud, NULL); return; }
    copy = (uint8_t*)pluto_malloc(len ? len : 1);
    if (!copy) { pluto_free(j); done(ud, NULL); return; }
    memcpy(copy, data, len);
    j->data = copy;
    j->len = len;
    j->fmt = fmt;
    j->done = done;
    j->ud = ud;
    s_isDecoding = 1;
    tasks_run(job_step, j, job_ctxfree,
              job_on_complete, job_on_error, j);
}

static void decode_raw(const uint8_t* data, size_t len,
                       IdDoneFn done, void* ud) {
    uint8_t b1, b2, b3, b4;
    struct LCDBitmap* img = NULL;
    if (!data || len < 4) { done(ud, NULL); return; }
    b1 = data[0];
    b2 = data[1];
    b3 = data[2];
    b4 = data[3];

#ifndef PLUTO_ID_PD
    (void)b3; (void)b4;
#endif

    /* JPEG: FF D8 (async, can take seconds for big photos) */
    if (b1 == 0xFF && b2 == 0xD8) {
        decode_async(0, data, len, done, ud);
        return;
    }
    /* PNG / GIF / WebP also run through the task scheduler */
    if (b1 == 0x89 && b2 == 0x50 && b3 == 0x4E && b4 == 0x47) {
        decode_async(1, data, len, done, ud);
        return;
    }
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0) {
        decode_async(2, data, len, done, ud);
        return;
    }
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
                     memcmp(data, "GIF89a", 6) == 0)) {
        decode_async(3, data, len, done, ud);
        return;
    }

    /* inline formats */
#ifdef PLUTO_ID_PD
    /* BMP: BM */
    if (b1 == 'B' && b2 == 'M') {
        img = bmp_decode(s_pd, data, len);
    } else if (b1 == 0 && b2 == 0 && (b3 == 1 || b3 == 2) && b4 == 0) {
        /* ICO / CUR: reserved(2)=0, type(2)=1 icon / 2 cursor */
        img = ico_decode(s_pd, data, len, 360, 200);
    } else {
        char head[201];
        size_t hn = len < 200 ? len : 200;
        size_t k;
        for (k = 0; k < hn; k++) {
            char c = (char)data[k];
            head[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        head[hn] = '\0';
        if (strstr(head, "<svg") || strstr(head, "<?xml")) {
            img = svg_decode(s_pd, (const char*)data, len, 360, 200);
        }
    }
#endif
    done(ud, img);
}

static void test_sink_done(void* ud, struct LCDBitmap* img) {
    struct LCDBitmap** out = (struct LCDBitmap**)ud;
    if (out) *out = img;
}

struct LCDBitmap* id_test_decode(const uint8_t* data, size_t len) {
    static struct LCDBitmap* sink;
    sink = NULL;
    decode_raw(data, len, test_sink_done, &sink);
    return sink;
}

/* ------------------------------------------------------------------ */
/* download pipeline                                                   */

typedef struct {
    char* url;   /* owned */
} FinishCtx;

static void finish_download(FinishCtx* fc, struct LCDBitmap* img) {
    /* Lua: imageCache[url] = img or false */
    cache_put(fc->url, img);
    s_isDownloading = 0;
    pluto_free(fc->url);
    pluto_free(fc);
}

static void dl_done(void* ud, struct LCDBitmap* img) {
    finish_download((FinishCtx*)ud, img);
}

static void dl_on_success(void* ud, int status, const StrMap* headers,
                          const char* body, size_t bodyLen,
                          const char* finalUrl) {
    FinishCtx* fc = (FinishCtx*)ud;
    (void)status; (void)headers; (void)finalUrl;
    if (body && bodyLen > 8)
        decode_raw((const uint8_t*)body, bodyLen, dl_done, fc);
    else
        finish_download(fc, NULL);
}

static void dl_on_error(void* ud, const char* msg) {
    (void)msg;   /* parity: err discarded */
    finish_download((FinishCtx*)ud, NULL);
}

static void process_next_image(void) {
    if (s_isDownloading || s_isDecoding || s_qN == 0) return;
    if (hc_is_loading()) return;   /* not while a page is loading */

    while (s_qN > 0) {
        char* url = s_queue[0];
        memmove(&s_queue[0], &s_queue[1],
                (size_t)(s_qN - 1) * sizeof(char*));
        s_qN--;
        if (cache_find(url)) {   /* success OR failure counts as present */
            pluto_free(url);
            continue;
        }
        {
            FinishCtx* fc = (FinishCtx*)pluto_malloc(sizeof(FinishCtx));
            PlutoHttpCallbacks cbs;
            if (!fc) { pluto_free(url); continue; }
            fc->url = url;       /* ownership moves here */
            memset(&cbs, 0, sizeof(cbs));
            cbs.ud = fc;
            cbs.onSuccess = dl_on_success;
            cbs.onError = dl_on_error;
            s_isDownloading = 1;
            /* a synchronous rejection has already fired onError/freed fc */
            hc_get(fc->url, &cbs);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* frame pump                                                          */

void id_update(void) {
    /* If we think we're downloading but the HTTP client is idle, the
     * download was cancelled (e.g. by a navigation) and its callbacks
     * will never fire: release the busy flag or the queue stalls. */
    if (s_isDownloading && !hc_is_loading()) s_isDownloading = 0;
    /* A cancelled decode task never runs its completion callback. */
    if (s_isDecoding && !tasks_is_running()) s_isDecoding = 0;
    if (!s_isDownloading && !s_isDecoding && s_qN > 0 &&
        !hc_is_loading())
        process_next_image();
}

/* ------------------------------------------------------------------ */
/* ImageDecoder.draw                                                   */

void id_draw(int x, int y, int w, int h,
             const char* altText, const char* href,
             int isSelected, const char* src) {
#ifdef PLUTO_ID_PD
    CacheEnt* e;
    if (w < 40) w = 40;
    if (h < 20) h = 20;
    if (w > 360) w = 360;
    if (h > 180) h = 180;
    (void)href;   /* accepted but unused in the source too */

    e = (src && src[0]) ? cache_find(src) : NULL;
    if (e && e->img) {
        int iw = 0, ih = 0, rb = 0;
        uint8_t* mask = NULL;
        uint8_t* dat = NULL;
        s_pd->graphics->getBitmapData((LCDBitmap*)e->img,
                                      &iw, &ih, &rb, &mask, &dat);
        if (iw > 0 && ih > 0) {
            float sx = (float)w / (float)iw;
            float sy = (float)h / (float)ih;
            float scale = sx < sy ? sx : sy;
            int dw = (int)floorf((float)iw * scale);
            int dh = (int)floorf((float)ih * scale);
            int dx = x + (w - dw) / 2;
            int dy = y + (h - dh) / 2;
            s_pd->graphics->drawScaledBitmap((LCDBitmap*)e->img,
                                             dx, dy, scale, scale);
        } else {
            s_pd->graphics->drawScaledBitmap((LCDBitmap*)e->img,
                                             x, y, 1.0f, 1.0f);
        }
        if (isSelected) {
            s_pd->graphics->drawRoundRect(x, y, w, h, 4, 2, kColorBlack);
        }
        return;
    }
    if (src && src[0] && !e) id_enqueue(src);

    /* loading placeholder card */
    s_pd->graphics->fillRoundRect(x, y, w, h, 4, kColorWhite);
    s_pd->graphics->drawRoundRect(x, y, w, h, 4, 1, kColorBlack);

    {
        int hx;
        for (hx = x + 4; hx <= x + w - 4; hx += 10)
            s_pd->graphics->drawLine(hx, y + 3, hx, y + h - 3,
                                     1, kColorBlack);
    }

    /* camera icon */
    {
        int iconX = x + w / 2 - 8;
        int iconY = y + h / 2 - 6;
        s_pd->graphics->drawRoundRect(iconX, iconY, 16, 11, 2, 1,
                                      kColorBlack);
        s_pd->graphics->fillEllipse(iconX + 5, iconY + 2, 6, 6,
                                    0.0f, 360.0f, kColorBlack);
        s_pd->graphics->fillRect(iconX + 13, iconY + 1, 1, 1, kColorBlack);

        if (altText && altText[0] && h > 30) {
            PlutoFont* f = style_get_small_font();
            char label[27];
            size_t alen = strlen(altText);
            if (alen > 26) {
                memcpy(label, altText, 23);
                memcpy(label + 23, "...", 4);
            } else {
                memcpy(label, altText, alen + 1);
            }
            if (f) s_pd->graphics->setFont((LCDFont*)f);
            {
                int lw = style_get_text_width(f, label);
                int lx = x + (w - lw) / 2;
                int ly = iconY + 14;
                if (ly + 10 <= y + h - 2) {
                    s_pd->graphics->drawText(label, strlen(label),
                                             kASCIIEncoding, lx, ly);
                }
            }
        }
    }

    s_pd->graphics->setDrawMode(kDrawModeCopy);
#else
    (void)x; (void)y; (void)w; (void)h;
    (void)altText; (void)href; (void)isSelected; (void)src;
#endif
}
