/* webstorage.c — PlutoBrowser engine-side localStorage/sessionStorage.
 *
 * See webstorage.h for the browser-shape contract, the origin rule and
 * the persistence file format. This is the shared road for ALL engines
 * (the adapters only translate the JS surface); the behavior lives here
 * once so every site gets the same semantics everywhere.
 *
 * File road (localStorage only): the game's Data sandbox via the SDK's
 * playdate->file API — the SAME road storage.c uses, so it works on the
 * simulator AND the device with no per-target code. The file holds one
 * text section per origin; only the BOUND origin's section is parsed
 * into RAM and rewritten on save; every OTHER section is preserved
 * byte-identical by streaming it through the g_raw image (no rename
 * games, no temp files — the storage.c truncate-rewrite precedent).
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pd_api.h"
#include "webstorage.h"
#include "../core/logger.h"

extern PlaydateAPI *pluto_pd(void);

#define WS_FILENAME "webstorage_js.txt" /* Data-sandbox file (both targets) */
#define WS_RAW_MAX 12288                /* persisted-file image budget */

typedef struct WsEntry
{
    char key[WS_KEY_MAX];
    char val[WS_VAL_MAX];
} WsEntry;

struct WsStore
{
    char origin[WS_ORIGIN_MAX];
    WsEntry entries[WS_MAX_ENTRIES];
    int count;
};

/* App-lifetime statics (the storage.c pattern: big road buffers hoist
 * to BSS; nothing here frees — the browser run owns them). */
static WsStore g_local[WS_MAX_ORIGINS];
static WsStore g_session[WS_MAX_ORIGINS];
static int g_localCount;
static int g_sessCount;
static char g_origin[WS_ORIGIN_MAX] = "null";
static int g_boundLocal = -1;  /* index into g_local, -1 = fail-store */
static int g_boundSession = -1;
static int g_fileRead;         /* g_raw populated this run? */
static WsStore g_fail;         /* overflow/"null"-origin store: empty forever */
static char g_raw[WS_RAW_MAX]; /* last-read file image (other origins live here) */
static int g_rawLen;
static int g_myStart; /* THIS run's bound origin's section bytes in g_raw */
static int g_myEnd;   /* (== g_myStart when the file has no such section) */

/* ── escape helpers (values carry \\ \n \r \t as \<char> pairs) ──── */

static void ws_escape(const char *in, char *out, size_t outMax)
{
    size_t oi = 0;
    for (const char *p = in; *p && oi + 2 < outMax; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '\n' || c == '\r' || c == '\t')
        {
            if (oi + 3 >= outMax)
            {
                break;
            }
            out[oi++] = '\\';
            out[oi++] = c == '\\' ? '\\' : c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        }
        else
        {
            out[oi++] = (char)c;
        }
    }
    out[oi] = '\0';
}

static void ws_unescape(const char *in, char *out, size_t outMax)
{
    size_t oi = 0;
    for (const char *p = in; *p && oi + 1 < outMax; p++)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            out[oi++] = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p;
        }
        else
        {
            out[oi++] = *p;
        }
    }
    out[oi] = '\0';
}

/* ── origin derivation (the browser's origin rule, this UA's map) ── */

static void ws_derive_origin(const char *url, char *out, size_t outMax)
{
    if (!url || !url[0])
    {
        snprintf(out, outMax, "null");
        return;
    }
    /* The scheme ends at the FIRST colon — hier schemes continue with
     * "//", but about:/file:/data: schemes may not, so parse on ':' not
     * "://" (else "about:apiprobe" would fall into the opaque bucket). */
    const char *colon = strchr(url, ':');
    if (!colon || colon == url)
    {
        snprintf(out, outMax, "null");
        return;
    }
    size_t schemeLen = (size_t)(colon - url);
    const char *rest = colon + 1;
    if (schemeLen == 5 && !strncasecmp(url, "https", 5))
    {
        const char *host = strncmp(rest, "//", 2) == 0 ? rest + 2 : rest;
        const char *end = host;
        while (*end && *end != '/' && *end != '?' && *end != '#')
        {
            end++;
        }
        snprintf(out, outMax, "https://%.*s", (int)(end - host), host);
    }
    else if (schemeLen == 4 && !strncasecmp(url, "http", 4))
    {
        const char *host = strncmp(rest, "//", 2) == 0 ? rest + 2 : rest;
        const char *end = host;
        while (*end && *end != '/' && *end != '?' && *end != '#')
        {
            end++;
        }
        snprintf(out, outMax, "http://%.*s", (int)(end - host), host);
    }
    else if (schemeLen == 4 && !strncasecmp(url, "file", 4))
    {
        /* WebKit/Chrome parity: every local file shares one file: origin. */
        snprintf(out, outMax, "file://");
    }
    else if (schemeLen == 5 && !strncasecmp(url, "about", 5))
    {
        /* UA house pages: per-URL store — internal pages stay isolated
         * from each other, and no site could ever land in this bucket. */
        size_t n = strlen(url);
        while (n > 0 && url[n - 1] == '/')
        {
            n--;
        }
        if (n > WS_ORIGIN_MAX - 1)
        {
            n = WS_ORIGIN_MAX - 1;
        }
        snprintf(out, outMax, "%.*s", (int)n, url);
    }
    else
    {
        snprintf(out, outMax, "null");
    }
}

/* ── store claim / bind ──────────────────────────────────────────── */

static WsStore *claim(WsStore *arr, int *count, const char *origin)
{
    for (int i = 0; i < *count; i++)
    {
        if (!strcmp(arr[i].origin, origin))
        {
            return &arr[i];
        }
    }
    if (*count >= WS_MAX_ORIGINS)
    {
        return NULL;
    }
    WsStore *s = &arr[(*count)++];
    memset(s, 0, sizeof(*s));
    snprintf(s->origin, sizeof(s->origin), "%s", origin);
    return s;
}

int ws_count(WsStore *s)
{
    return s ? s->count : 0;
}

const char *ws_origin(void)
{
    return g_origin;
}

static WsStore *pick(WsStore *arr, int bound)
{
    return bound >= 0 ? &arr[bound] : &g_fail;
}

WsStore *ws_local(void)
{
    return pick(g_local, g_boundLocal);
}

WsStore *ws_session(void)
{
    return pick(g_session, g_boundSession);
}

/* Parse g_raw and pull THIS origin's section into the bound store.
 * Only the bound origin goes resident; g_myStart/g_myEnd record where
 * its section lives so saves can stream the rest through untouched. */
static void ws_load_origin(WsStore *l)
{
    g_myStart = g_myEnd = g_rawLen; /* default: no section → append at end */
    l->count = 0;
    /* Section header = "@<origin>" on its own line. Compare against the
     * LINE CONTENT (lineLen excludes the newline): a strict equality on
     * both length and bytes keeps "@about:apiprobe2" from ever matching
     * the "about:apiprobe" origin. */
    char header[WS_ORIGIN_MAX];
    snprintf(header, sizeof(header), "@%s", g_origin);
    size_t hlen = strlen(header);
    const char *p = g_raw;
    while (p < g_raw + g_rawLen)
    {
        const char *eol = memchr(p, '\n', (size_t)(g_raw + g_rawLen - p));
        size_t lineLen = eol ? (size_t)(eol - p) : (size_t)(g_raw + g_rawLen - p);
        if (lineLen == hlen && !memcmp(p, header, hlen))
        {
            /* my section: parse `key\tvalue` lines until a blank or '@' */
            g_myStart = (int)(p - g_raw);
            const char *q = eol ? eol + 1 : g_raw + g_rawLen;
            while (q < g_raw + g_rawLen && l->count < WS_MAX_ENTRIES)
            {
                const char *qEol = memchr(q, '\n', (size_t)(g_raw + g_rawLen - q));
                size_t qLen = qEol ? (size_t)(qEol - q) : (size_t)(g_raw + g_rawLen - q);
                if (qLen == 0 || *q == '@')
                {
                    break; /* end of section (blank or the next origin) */
                }
                const char *tab = memchr(q, '\t', qLen);
                if (!tab || tab == q)
                {
                    q = qEol ? qEol + 1 : g_raw + g_rawLen;
                    continue; /* malformed line — skip, keep streaming */
                }
                size_t kLen = (size_t)(tab - q);
                size_t vLen = qLen - (size_t)(tab - q) - 1;
                char k[WS_KEY_MAX];
                char v[WS_VAL_MAX * 2];
                snprintf(k, sizeof(k), "%.*s", (int)(kLen > WS_KEY_MAX - 1 ? WS_KEY_MAX - 1 : kLen), q);
                snprintf(v, sizeof(v), "%.*s", (int)(vLen > sizeof(v) - 1 ? (int)sizeof(v) - 1 : (int)vLen), tab + 1);
                /* un-escape keeps persist/restore symmetric */
                char uk[WS_KEY_MAX];
                char uv[WS_VAL_MAX];
                ws_unescape(k, uk, sizeof(uk));
                ws_unescape(v, uv, sizeof(uv));
                snprintf(l->entries[l->count].key, sizeof(l->entries[l->count].key), "%s", uk);
                snprintf(l->entries[l->count].val, sizeof(l->entries[l->count].val), "%s", uv);
                l->count++;
                q = qEol ? qEol + 1 : g_raw + g_rawLen;
            }
            g_myEnd = (int)(q - g_raw);
            return;
        }
        p = eol ? eol + 1 : g_raw + g_rawLen;
    }
}

void ws_bind(const char *docUrl)
{
    char origin[WS_ORIGIN_MAX];
    ws_derive_origin(docUrl, origin, sizeof(origin));

    /* Opaque origins ("null": data: URLs / unknown schemes / empty)
     * NEVER claim a slot — they land on the fail-store (empty reads,
     * quota-failed writes; the browser's SecurityError road, shaped
     * for the device). */
    int opaque = !strcmp(origin, "null");
    WsStore *l = opaque ? NULL : claim(g_local, &g_localCount, origin);
    WsStore *se = opaque ? NULL : claim(g_session, &g_sessCount, origin);
    int newLocal = l && (g_boundLocal < 0 ||
                         strcmp(g_local[g_boundLocal].origin, origin) != 0);
    snprintf(g_origin, sizeof(g_origin), "%s", origin);
    g_boundLocal = l ? (int)(l - g_local) : -1;
    g_boundSession = se ? (int)(se - g_session) : -1;

    if (!g_fileRead)
    {
        g_fileRead = 1;
        g_rawLen = 0;
        g_myStart = g_myEnd = 0;
        if (pluto_pd())
        {
            SDFile *f = pluto_pd()->file->open(WS_FILENAME, kFileReadData);
            if (f)
            {
                g_rawLen = pluto_pd()->file->read(f, g_raw, WS_RAW_MAX - 1);
                if (g_rawLen < 0)
                {
                    g_rawLen = 0;
                }
                pluto_pd()->file->close(f);
                logger_log("[webstorage] file read: bytes=%d", g_rawLen);
            }
            else
            {
                g_fileRead = 2; /* missing file: first-run shape, one log */
                logger_log("[webstorage] file miss: starting empty (%s)", WS_FILENAME);
            }
        }
        if (g_rawLen > 0)
        {
            g_raw[g_rawLen] = '\0';
        }
    }

    if (l)
    {
        int newlyLoaded = newLocal || (l->count == 0 && !opaque);
        if (newlyLoaded && g_rawLen > 0)
        {
            ws_load_origin(l);
        }
        logger_log("[webstorage] bind url=%s origin=%s local=%s entries=%d session=%s entries=%d",
                   docUrl ? docUrl : "(null)", g_origin,
                   g_boundLocal >= 0 ? "resident" : "overflow",
                   l->count, g_boundSession >= 0 ? "resident" : "overflow",
                   ws_count(se));
    }
    else
    {
        logger_log("[webstorage] bind url=%s origin=%s local=fail-store session=fail-store",
                   docUrl ? docUrl : "(null)", g_origin);
    }
}

/* ── the save: rebuild the image, stream other origins through ────── */

static char g_img[WS_RAW_MAX]; /* save-build scratch (a BSS hoist, as usual) */

static void ws_save_file(const char *why)
{
    if (!pluto_pd())
    {
        return;
    }
    if (g_boundLocal < 0)
    {
        return; /* overflow origin: nothing to persist */
    }
    WsStore *l = &g_local[g_boundLocal];

    /* Build the FULL new image: other origins' bytes (prefix + suffix,
     * untouched) + my rebuilt section. The image then becomes g_raw —
     * the file's truth — so a later re-bind of any origin re-parses
     * exactly what landed on disk (a cleared section can never be
     * resurrected from a stale image). */
    int len = 0;
    int prefix = g_myStart > 0 ? g_myStart : 0;
    if (prefix > 0)
    {
        memcpy(g_img, g_raw, (size_t)prefix);
        len = prefix;
    }
    len += snprintf(g_img + len, sizeof(g_img) - len, "@%s\n", g_origin);
    char line[WS_VAL_MAX * 2 + WS_KEY_MAX + 8];
    for (int i = 0; i < l->count; i++)
    {
        char ek[WS_KEY_MAX * 2];
        char ev[WS_VAL_MAX * 2];
        ws_escape(l->entries[i].key, ek, sizeof(ek));
        ws_escape(l->entries[i].val, ev, sizeof(ev));
        len += snprintf(g_img + len, sizeof(g_img) - len, "%s\t%s\n", ek, ev);
        if (len >= WS_RAW_MAX - 8)
        {
            len = WS_RAW_MAX - 8; /* hard cap, image stays parseable */
            break;
        }
    }
    int tail = g_myEnd < g_rawLen ? g_rawLen - g_myEnd : 0;
    if (tail > 0 && len + tail <= WS_RAW_MAX - 1)
    {
        memcpy(g_img + len, g_raw + g_myEnd, (size_t)tail);
        len += tail;
    }
    g_img[len] = '\0';
    int newStart = prefix;
    int newEnd = prefix + (len - prefix - tail);

    SDFile *f = pluto_pd()->file->open(WS_FILENAME, kFileWrite);
    if (!f)
    {
        logger_log("[webstorage] save open FAILED (%s)", WS_FILENAME);
        return;
    }
    /* One bounded write (≤ WS_RAW_MAX inside the spill road's proven
     * 16KB single-write envelope). */
    pluto_pd()->file->write(f, g_img, (unsigned int)len);
    pluto_pd()->file->close(f);

    /* Adopt the image: g_raw = file truth; section span updates. */
    memcpy(g_raw, g_img, (size_t)len);
    g_rawLen = len;
    g_raw[g_rawLen] = '\0';
    g_myStart = newStart;
    g_myEnd = newEnd;
    logger_log("[webstorage] save(%s): bytes=%d entries=%d", why, len, l->count);
}

/* ── the mapping members ─────────────────────────────────────────── */

static int ws_bytes_of(WsStore *s)
{
    int n = 0;
    for (int i = 0; i < s->count; i++)
    {
        n += (int)strlen(s->entries[i].key) + (int)strlen(s->entries[i].val) + 2;
    }
    return n;
}

static int ws_find(WsStore *s, const char *key)
{
    for (int i = 0; i < s->count; i++)
    {
        if (!strcmp(s->entries[i].key, key))
        {
            return i;
        }
    }
    return -1;
}

int ws_key(WsStore *s, int idx, char *out, size_t outMax)
{
    if (!s || idx < 0 || idx >= s->count)
    {
        return 0;
    }
    snprintf(out, outMax, "%s", s->entries[idx].key);
    return 1;
}

int ws_get(WsStore *s, const char *key, char *out, size_t outMax)
{
    int idx = s ? ws_find(s, key) : -1;
    if (idx < 0)
    {
        return 0;
    }
    snprintf(out, outMax, "%s", s->entries[idx].val);
    return 1;
}

int ws_set(WsStore *s, const char *key, const char *value)
{
    if (!s || !key)
    {
        return 0;
    }
    int idx = ws_find(s, key);
    int before = idx >= 0 ? (int)strlen(s->entries[idx].key) +
                               (int)strlen(s->entries[idx].val) + 2 : 0;
    int newBytes = (int)strlen(key) + (int)strlen(value) + 2;
    if (ws_bytes_of(s) - before + newBytes > WS_QUOTA_BYTES)
    {
        logger_log("[webstorage] quota: origin=%s want=%d have=%d cap=%d key='%s'",
                   g_origin, ws_bytes_of(s) - before + newBytes,
                   ws_bytes_of(s) - before, WS_QUOTA_BYTES, key);
        return 0;
    }
    if (idx < 0 && s->count >= WS_MAX_ENTRIES)
    {
        logger_log("[webstorage] entry cap: origin=%s entries=%d", g_origin, s->count);
        return 0;
    }
    if (idx >= 0)
    {
        snprintf(s->entries[idx].val, sizeof(s->entries[idx].val), "%s", value);
    }
    else
    {
        snprintf(s->entries[s->count].key, sizeof(s->entries[s->count].key), "%s", key);
        snprintf(s->entries[s->count].val, sizeof(s->entries[s->count].val), "%s", value);
        s->count++;
    }
    logger_log("[webstorage] set origin=%s key='%s' bytes=%d total=%d",
               g_origin, key, newBytes, ws_bytes_of(s));
    if (s == ws_local())
    {
        ws_save_file("set");
    }
    return 1;
}

void ws_remove(WsStore *s, const char *key)
{
    if (!s || !key)
    {
        return;
    }
    int idx = ws_find(s, key);
    if (idx < 0)
    {
        return;
    }
    for (int i = idx; i + 1 < s->count; i++)
    {
        s->entries[i] = s->entries[i + 1];
    }
    s->count--;
    memset(&s->entries[s->count], 0, sizeof(WsEntry));
    logger_log("[webstorage] remove origin=%s key='%s'", g_origin, key);
    if (s == ws_local())
    {
        ws_save_file("remove");
    }
}

void ws_clear(WsStore *s)
{
    if (!s)
    {
        return;
    }
    logger_log("[webstorage] clear origin=%s entries=%d", g_origin, s->count);
    s->count = 0;
    if (s == ws_local())
    {
        ws_save_file("clear");
    }
}
