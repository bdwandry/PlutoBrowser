/*
 * PlutoBrowser — sdisco.c
 * Script-Derived Submission Discovery. See sdisco.h for the contract.
 *
 * The scanner is a bounded, allocation-free pass over minified-or-not
 * JavaScript. It recognizes the two shapes a page's own submission code
 * actually takes on the real web:
 *
 *   fetch( <string> , { ... method:"POST" ... Content-Type:"..." ...
 *                       body: JSON.stringify({ key : <value> , ... }) })
 *   <expr>.open( "POST" , <string> )
 *
 * Everything is best-effort by design: malformed/mixed shapes produce no
 * record, and the consumer (main.c pluto_form_fallback) scores records
 * against the submitting form's own fields before trusting one. No
 * regular expressions (newlib-safe), no dynamic allocation, no site
 * knowledge anywhere.
 */
#include <string.h>
#include <stdio.h>

#include "html/sdisco.h"
#include "core/logger.h"
#include "../core/pluto_mem.h"

/* Bounded statics (P19: aggregate scratch never lives on a task stack). */
#define SD_URL_MAX 224
#define SD_CT_MAX 48
#define SD_KEY_CAP 24
#define SD_KEY_MAX 8
#define SD_MAX_ENDPOINTS 8
/* fetch-options look-ahead window past the URL literal: minified code
 * packs method/headers/body tightly (~200B on the live contact page);
 * 768 covers the shape with room for longer header blocks. */
#define SD_WINDOW 768
/* Spill-lane windowing: any fetch record whose whole span (URL literal +
 * options window) ends this far before the read edge is seen complete. */
#define SD_CARRY 1280
#define SD_READ 1536

static SdiscoEndpoint g_sdTable[SD_MAX_ENDPOINTS];
static int g_sdCount;

void sdisco_reset(void)
{
    g_sdCount = 0;
}

int sdisco_count(void)
{
    return g_sdCount;
}

const SdiscoEndpoint *sdisco_at(int i)
{
    if (i < 0 || i >= g_sdCount)
    {
        return NULL;
    }
    return &g_sdTable[i];
}

/* ── small scanning helpers ─────────────────────────────────────────── */

static int sd_isident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static const char *sd_skip_ws(const char *p, const char *end)
{
    while (p < end &&
           (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    {
        p++;
    }
    return p;
}

static char sd_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Bounded ci-substring find (no strcasestr on newlib). */
static const char *sd_find_ci(const char *p, const char *end,
                              const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || (size_t)(end - p) < nlen)
    {
        return NULL;
    }
    for (const char *s = p; s + (long)nlen <= end; s++)
    {
        size_t i = 0;
        while (i < nlen &&
               sd_lower(s[i]) == sd_lower(needle[i]))
        {
            i++;
        }
        if (i == nlen)
        {
            return s;
        }
    }
    return NULL;
}

/* Token find with identifier boundaries: the needle must not sit inside
 * a longer identifier (so "fetch" never matches "prefetch"). */
static const char *sd_find_token(const char *p, const char *end,
                                 const char *needle)
{
    size_t nlen = strlen(needle);
    int leftIdent = sd_isident(needle[0]);
    int rightIdent = sd_isident(needle[nlen - 1]);
    while ((size_t)(end - p) >= nlen)
    {
        const char *hit = sd_find_ci(p, end, needle);
        if (!hit || (size_t)(end - hit) < nlen)
        {
            return NULL;
        }
        const char *after = hit + nlen;
        int ok = 1;
        if (leftIdent && hit > p && sd_isident(hit[-1]))
        {
            ok = 0; /* "prefetch" — swallow and continue past */
        }
        if (rightIdent && after < end && sd_isident(*after))
        {
            ok = 0; /* "fetchx" */
        }
        if (ok)
        {
            return hit;
        }
        p = hit + 1;
    }
    return NULL;
}

/* Read a JS string literal; p points AT the opening quote. Returns one
 * past the closing quote (NULL = unterminated), value copied to out. */
static const char *sd_read_string(const char *p, const char *end, char *out,
                                  size_t cap)
{
    char q = *p++;
    size_t n = 0;
    while (p < end && *p != q)
    {
        char c = *p;
        if (c == '\\' && p + 1 < end)
        {
            p++; /* escaped char: keep it literally (urls/keys have none) */
            c = *p;
        }
        if (c == '\0')
        {
            return NULL; /* a real NUL never sits inside JS source text */
        }
        if (n + 1 < cap)
        {
            out[n++] = c;
        }
        p++;
    }
    if (p >= end)
    {
        return NULL;
    }
    out[n] = '\0';
    return p + 1;
}

/* Skip one balanced JS value (string, or anything with paired () [] {})
 * starting at p. Returns the pointer AT the , or } that closed the value
 * at depth 0, or NULL (overrun). */
static const char *sd_skip_value(const char *p, const char *end)
{
    int depth = 0;
    while (p < end)
    {
        char c = *p;
        if (c == '"' || c == '\'')
        {
            char buf[2]; /* reuse the reader as a skipper */
            const char *after = sd_read_string(p, end, buf, sizeof(buf));
            if (!after)
            {
                return NULL;
            }
            p = after;
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
        {
            depth++;
        }
        else if (c == ')' || c == ']' || c == '}')
        {
            if (depth == 0)
            {
                return p; /* the object's own } or the list , position */
            }
            depth--;
        }
        else if (c == ',' && depth == 0)
        {
            return p;
        }
        p++;
    }
    return NULL;
}

/* Parse JSON.stringify({ k : v, ... }) keys at/after p. Returns the key
 * count (0 = no object-literal payload found). */
static int sd_scan_stringify_keys(const char *p, const char *end,
                                  char keys[][SD_KEY_CAP], int maxKeys)
{
    const char *j = sd_find_token(p, end, "JSON.stringify");
    if (!j)
    {
        return 0;
    }
    j = sd_skip_ws(j + 14, end);
    if (j >= end || *j != '(')
    {
        return 0;
    }
    j = sd_skip_ws(j + 1, end);
    if (j >= end || *j != '{')
    {
        return 0; /* a variable payload: keys unknown, honestly 0 */
    }
    j++;
    int count = 0;
    while (j < end && count < maxKeys)
    {
        j = sd_skip_ws(j, end);
        if (j >= end || *j == '}')
        {
            break;
        }
        char key[SD_KEY_CAP];
        if (*j == '"' || *j == '\'')
        {
            const char *after = sd_read_string(j, end, key, sizeof(key));
            if (!after)
            {
                break;
            }
            j = after;
        }
        else if (sd_isident(*j))
        {
            size_t n = 0;
            while (j < end && sd_isident(*j))
            {
                if (n + 1 < sizeof(key))
                {
                    key[n++] = *j;
                }
                j++;
            }
            key[n] = '\0';
        }
        else
        {
            break; /* spread/computed keys: not a plain literal shape */
        }
        j = sd_skip_ws(j, end);
        if (j >= end || *j != ':')
        {
            break;
        }
        j = sd_skip_value(j + 1, end);
        if (!j || j >= end)
        {
            break;
        }
        if (*j == '}')
        {
            snprintf(keys[count], SD_KEY_CAP, "%s", key);
            count++;
            break;
        }
        /* *j == ',' — record and continue */
        snprintf(keys[count], SD_KEY_CAP, "%s", key);
        count++;
        j++;
    }
    return count;
}

/* ── recording ───────────────────────────────────────────────────────── */

static void sd_record(const char *url, int methodPost, const char *ct,
                      const char keys[][SD_KEY_CAP], int keyCount)
{
    if (!url[0])
    {
        return;
    }
    if (!methodPost)
    {
        return; /* GET fetches / opens are data loads, not submissions */
    }
    /* dedupe by URL: merge the richest sighting of the same endpoint */
    for (int i = 0; i < g_sdCount; i++)
    {
        if (strcmp(g_sdTable[i].url, url) == 0)
        {
            if (!g_sdTable[i].methodPost && methodPost)
            {
                g_sdTable[i].methodPost = 1;
            }
            if (!g_sdTable[i].contentType[0] && ct[0])
            {
                snprintf(g_sdTable[i].contentType,
                         sizeof(g_sdTable[i].contentType), "%s", ct);
            }
            if (g_sdTable[i].keyCount == 0 && keyCount > 0)
            {
                for (int k = 0; k < keyCount && k < SD_KEY_MAX; k++)
                {
                    snprintf(g_sdTable[i].keys[k], SD_KEY_CAP, "%s",
                             keys[k]);
                }
                g_sdTable[i].keyCount = keyCount;
            }
            return;
        }
    }
    if (g_sdCount >= SD_MAX_ENDPOINTS)
    {
        return; /* bounded table: extra endpoints are dropped, not grown */
    }
    SdiscoEndpoint *r = &g_sdTable[g_sdCount];
    memset(r, 0, sizeof(*r));
    snprintf(r->url, sizeof(r->url), "%s", url);
    r->methodPost = 1;
    snprintf(r->contentType, sizeof(r->contentType), "%s", ct ? ct : "");
    for (int k = 0; k < keyCount && k < SD_KEY_MAX; k++)
    {
        snprintf(r->keys[k], SD_KEY_CAP, "%s", keys[k]);
    }
    r->keyCount = keyCount;
    g_sdCount++;
    logger_log("[sdisco] endpoint discovered: %.90s ct=%s keys=%d",
               r->url, r->contentType[0] ? r->contentType : "(none)",
               r->keyCount);
}

/* ── the pattern passes over one buffer ──────────────────────────────── */

static void sd_scan_buffer(const char *p, const char *end)
{
    /* every fetch( token whose first argument is a string literal */
    const char *s = p;
    while ((s = sd_find_token(s, end, "fetch")) != NULL)
    {
        const char *q = sd_skip_ws(s + 5, end);
        if (q < end && *q == '(')
        {
            q = sd_skip_ws(q + 1, end);
            if (q < end && (*q == '"' || *q == '\''))
            {
                char url[SD_URL_MAX];
                const char *after = sd_read_string(q, end, url, sizeof(url));
                if (after)
                {
                    const char *wend = after + SD_WINDOW;
                    if (wend > end)
                    {
                        wend = end;
                    }
                    /* method:"POST" (first method: key in the window) */
                    int post = 0;
                    const char *m = sd_find_token(after, wend, "method");
                    if (m)
                    {
                        const char *mv = sd_skip_ws(m + 6, wend);
                        if (mv < wend && *mv == ':')
                        {
                            mv = sd_skip_ws(mv + 1, wend);
                            if (mv < wend && (*mv == '"' || *mv == '\''))
                            {
                                char meth[16];
                                if (sd_read_string(mv, wend, meth,
                                                   sizeof(meth)))
                                {
                                    const char *a = meth;
                                    const char *b = meth + strlen(meth);
                                    post = (b - a == 4 &&
                                            sd_lower(a[0]) == 'p' &&
                                            sd_lower(a[1]) == 'o' &&
                                            sd_lower(a[2]) == 's' &&
                                            sd_lower(a[3]) == 't');
                                }
                            }
                        }
                    }
                    char ct[SD_CT_MAX];
                    ct[0] = '\0';
                    const char *cth = sd_find_ci(after, wend, "content-type");
                    if (cth)
                    {
                        const char *cv = strchr(cth, ':');
                        if (cv && cv < wend)
                        {
                            cv = sd_skip_ws(cv + 1, wend);
                            if (cv < wend && (*cv == '"' || *cv == '\''))
                            {
                                sd_read_string(cv, wend, ct, sizeof(ct));
                            }
                        }
                    }
                    char keys[SD_KEY_MAX][SD_KEY_CAP];
                    int kc = sd_scan_stringify_keys(after, wend, keys,
                                                    SD_KEY_MAX);
                    sd_record(url, post, ct, keys, kc);
                }
            }
        }
        s += 5;
    }
    /* every .open("POST", "url") — the XHR shape */
    const char *o = p;
    while ((o = sd_find_token(o, end, ".open")) != NULL)
    {
        const char *q = sd_skip_ws(o + 5, end);
        if (q < end && *q == '(')
        {
            q = sd_skip_ws(q + 1, end);
            if (q < end && (*q == '"' || *q == '\''))
            {
                char meth[16];
                const char *after = sd_read_string(q, end, meth, sizeof(meth));
                if (after)
                {
                    const char *cv = sd_skip_ws(after, end);
                    if (cv < end && *cv == ',')
                    {
                        cv = sd_skip_ws(cv + 1, end);
                        if (cv < end && (*cv == '"' || *cv == '\''))
                        {
                            char url[SD_URL_MAX];
                            if (sd_read_string(cv, end, url, sizeof(url)))
                            {
                                int post = (meth[0] | 0x20) == 'p' &&
                                           (meth[1] | 0x20) == 'o' &&
                                           (meth[2] | 0x20) == 's' &&
                                           (meth[3] | 0x20) == 't' &&
                                           meth[4] == '\0';
                                sd_record(url, post, "", NULL, 0);
                            }
                        }
                    }
                }
            }
        }
        o += 5;
    }
}

/* ── public entry points ─────────────────────────────────────────────── */

void sdisco_scan_script(const char *src, size_t len)
{
    if (!src || len == 0)
    {
        return;
    }
    logger_log("[sdisco] scan len=%u head='%.32s'", (unsigned)len, src);
    sd_scan_buffer(src, src + len);
}

void sdisco_scan_spill(SpillFile h, long len)
{
    /* Bounded window + carry: a record whose whole span ends SD_CARRY
     * before the read edge is always seen complete; the carry region
     * re-scans next round and the dedupe makes that harmless. Static
     * scratch (P19): this runs inside the fetch-delivery callback. */
    static char buf[SD_CARRY + SD_READ];
    if (h < 0 || len <= 0)
    {
        return;
    }
    long off = 0;
    size_t carry = 0;
    while (off < len)
    {
        long want = (len - off < SD_READ) ? (len - off) : SD_READ;
        long got = pluto_spill_read(h, off, buf + carry, (size_t)want);
        if (got <= 0)
        {
            break;
        }
        size_t have = carry + (size_t)got;
        int final = (off + got >= len);
        size_t scanTo = final ? have
                              : (have > SD_CARRY ? have - SD_CARRY : 0);
        if (scanTo > 0)
        {
            sd_scan_buffer(buf, buf + scanTo);
        }
        if (final)
        {
            break;
        }
        /* carry the tail: keep the last SD_CARRY bytes in front */
        if (have > SD_CARRY)
        {
            memmove(buf, buf + have - SD_CARRY, SD_CARRY);
            carry = SD_CARRY;
        }
        else
        {
            carry = have;
        }
        off += got;
    }
}
