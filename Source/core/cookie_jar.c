/*
 * PlutoBrowser — cookie_jar.c
 * RFC 6265 cookie jar (port of Source/core/cookie_jar.lua). See cookie_jar.h.
 * Every quirk of the Lua reference is preserved: name/value validation,
 * domain acceptance rules, path defaulting, the three Expires formats,
 * Max-Age<=0 / past-Expires delete semantics, dedupe by key with identical
 * value+expires no-op, 300-cap, lazy prune in getHeader, prune-without-save.
 */
#include <string.h>
#include <stdlib.h>

#include "core/cookie_jar.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_REALLOC(p, n) pluto_pd()->system->realloc((p), (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

/* ── storage hook (wired by core/storage in Phase 9) ─────────────────────── */
static void (*g_saveHook)(void) = NULL;

void cookie_jar_set_save_hook(void (*fn)(void))
{
    g_saveHook = fn;
}

static void save(void)
{
    if (g_saveHook)
    {
        g_saveHook();
    }
}

/* ── stored list ─────────────────────────────────────────────────────────── */
static Cookie *g_list[COOKIE_JAR_MAX];
static int g_count = 0;

int cookie_jar_count(void)
{
    return g_count;
}

const Cookie *cookie_jar_get(int index)
{
    if (index < 0 || index >= g_count)
    {
        return NULL;
    }
    return g_list[index];
}

/* ── small helpers (mirror the Lua locals) ───────────────────────────────── */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Lua: string.gsub(s, "^%s*(.-)%s*$", "%1") */
static void trim(const char *s, char *out, size_t cap)
{
    size_t len = strlen(s);
    size_t b = 0, e = len;
    while (b < e && is_space(s[b]))
    {
        b++;
    }
    while (e > b && is_space(s[e - 1]))
    {
        e--;
    }
    size_t n = e - b;
    if (n >= cap)
    {
        n = cap - 1;
    }
    memcpy(out, s + b, n);
    out[n] = '\0';
}

/* Howard Hinnant's days-from-civil algorithm → epoch seconds.
 * Clamping preserved from the Lua reference: year>=0, month 1..12, day>=1. */
static long long make_timestamp(long long year, long long month, long long day,
                                long long hour, long long minute, long long second)
{
    if (year < 0)
    {
        year = 0;
    }
    if (month < 1)
    {
        month = 1;
    }
    if (month > 12)
    {
        month = 12;
    }
    if (day < 1)
    {
        day = 1;
    }
    long long y = year - (month <= 2 ? 1 : 0);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long mp = month + (month > 2 ? -3 : 9);
    long long doy = (153 * mp + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static int month_from_name(const char *s, size_t len)
{
    /* Exact 3-letter names, case-sensitive (Lua table lookup semantics). */
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (len != 3)
    {
        return 0;
    }
    for (int i = 0; i < 12; i++)
    {
        if (s[0] == months[i][0] && s[1] == months[i][1] && s[2] == months[i][2])
        {
            return i + 1;
        }
    }
    return 0;
}

static long long parse_digits(const char *s, size_t *pos)
{
    long long v = 0;
    int any = 0;
    while (s[*pos] >= '0' && s[*pos] <= '9')
    {
        v = v * 10 + (s[*pos] - '0');
        (*pos)++;
        any = 1;
    }
    return any ? v : -1;
}

static size_t skip_alpha(const char *s, size_t i)
{
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))
    {
        i++;
    }
    return i;
}

/* Parse an RFC 6265 Expires date (IMF-fixdate / RFC 850 / asctime) into epoch
 * seconds, or 0 if unrecognized. Separator rules are faithful to the Lua
 * string.match patterns (single literal spaces / dashes). */
static long long parse_date(const char *str)
{
    if (!str || !str[0])
    {
        return 0;
    }
    char t[128];
    trim(str, t, sizeof(t));

    size_t i = 0;
    long long d, y, h, mi, s;
    int mon;
    size_t a0;

    /* Format 1: IMF-fixdate  "Wdy, DD Mon YYYY HH:MM:SS" */
    i = skip_alpha(t, 0);
    if (i > 0 && t[i] == ',' && t[i + 1] == ' ')
    {
        i += 2;
        d = parse_digits(t, &i);
        if (d >= 0 && t[i] == ' ')
        {
            i++;
            a0 = i;
            i = skip_alpha(t, i);
            mon = month_from_name(t + a0, i - a0);
            if (mon && t[i] == ' ')
            {
                i++;
                y = parse_digits(t, &i);
                if (y >= 0 && t[i] == ' ')
                {
                    i++;
                    h = parse_digits(t, &i);
                    if (h >= 0 && t[i] == ':')
                    {
                        i++;
                        mi = parse_digits(t, &i);
                        if (mi >= 0 && t[i] == ':')
                        {
                            i++;
                            s = parse_digits(t, &i);
                            if (s >= 0)
                            {
                                return make_timestamp(y, mon, d, h, mi, s);
                            }
                        }
                    }
                }
            }
        }
        /* fall through: same "Wdy, " prefix is also RFC 850's */
    }

    /* Format 2: RFC 850  "Wdy, DD-Mon-YY HH:MM:SS" */
    i = skip_alpha(t, 0);
    if (i > 0 && t[i] == ',' && t[i + 1] == ' ')
    {
        i += 2;
        d = parse_digits(t, &i);
        if (d >= 0 && t[i] == '-')
        {
            i++;
            a0 = i;
            i = skip_alpha(t, i);
            mon = month_from_name(t + a0, i - a0);
            if (mon && t[i] == '-')
            {
                i++;
                y = parse_digits(t, &i);
                if (y >= 0 && y < 100 && t[i] == ' ')
                {
                    if (y < 70)
                    {
                        y += 2000;
                    }
                    else
                    {
                        y += 1900;
                    }
                    i++;
                    h = parse_digits(t, &i);
                    if (h >= 0 && t[i] == ':')
                    {
                        i++;
                        mi = parse_digits(t, &i);
                        if (mi >= 0 && t[i] == ':')
                        {
                            i++;
                            s = parse_digits(t, &i);
                            if (s >= 0)
                            {
                                return make_timestamp(y, mon, d, h, mi, s);
                            }
                        }
                    }
                }
            }
        }
        return 0;
    }

    /* Format 3: asctime  "Wdy Mon DD HH:MM:SS YYYY" (single spaces) */
    i = skip_alpha(t, 0);
    if (i > 0 && t[i] == ' ')
    {
        i++;
        a0 = i;
        i = skip_alpha(t, i);
        mon = month_from_name(t + a0, i - a0);
        if (mon && t[i] == ' ')
        {
            i++;
            d = parse_digits(t, &i);
            if (d >= 0 && t[i] == ' ')
            {
                i++;
                h = parse_digits(t, &i);
                if (h >= 0 && t[i] == ':')
                {
                    i++;
                    mi = parse_digits(t, &i);
                    if (mi >= 0 && t[i] == ':')
                    {
                        i++;
                        s = parse_digits(t, &i);
                        if (s >= 0 && t[i] == ' ')
                        {
                            i++;
                            y = parse_digits(t, &i);
                            if (y >= 0)
                            {
                                return make_timestamp(y, mon, d, h, mi, s);
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static long long now_seconds(void)
{
    unsigned int ms = 0;
    return (long long)pluto_pd()->system->getSecondsSinceEpoch(&ms);
}

/* ── matching (mirror the Lua functions) ─────────────────────────────────── */

static int domain_matches(const Cookie *c, const char *host)
{
    if (c->hostOnly)
    {
        return strcmp(host, c->domain) == 0;
    }
    size_t cdlen = strlen(c->domain);
    size_t hlen = strlen(host);
    if (strcmp(host, c->domain) == 0)
    {
        return 1;
    }
    if (hlen > cdlen + 1)
    {
        /* string.sub(host, -#cd - 1) == "." .. cd */
        return host[hlen - cdlen - 1] == '.' &&
               strcmp(host + hlen - cdlen, c->domain) == 0;
    }
    return 0;
}

static int path_matches(const Cookie *c, const char *reqPath)
{
    const char *cpath = c->path ? c->path : "/";
    const char *rp = reqPath ? reqPath : "/";
    size_t clen = strlen(cpath);
    if (strcmp(rp, cpath) == 0)
    {
        return 1;
    }
    if (strncmp(rp, cpath, clen) == 0)
    {
        char tail = rp[clen]; /* 0 at end-of-string == Lua's "" tail */
        if (clen > 0 && cpath[clen - 1] == '/')
        {
            return 1;
        }
        if (tail == '/')
        {
            return 1;
        }
    }
    return 0;
}

/* "H:"/"D:" .. domain .. "|" .. path|"/" .. "|" .. name */
static void cookie_key(const Cookie *c, char *out, size_t cap)
{
    snprintf(out, cap, "%c:%s|%s|%s",
             c->hostOnly ? 'H' : 'D',
             c->domain ? c->domain : "",
             c->path ? c->path : "/",
             c->name ? c->name : "");
}

/* ── allocation helpers ──────────────────────────────────────────────────── */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)PLUTO_MALLOC(n);
    if (p)
    {
        memcpy(p, s, n);
    }
    return p;
}

void cookie_jar_free_cookie(Cookie *c)
{
    if (!c)
    {
        return;
    }
    PLUTO_FREE(c->name);
    PLUTO_FREE(c->value);
    PLUTO_FREE(c->domain);
    PLUTO_FREE(c->path);
    PLUTO_FREE(c);
}

/* ── parseSetCookie ──────────────────────────────────────────────────────── */

static int name_char_ok(char c)
{
    /* Lua class: %w!#$%&'*+-.^_`|~ */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    {
        return 1;
    }
    return strchr("!#$%&'*+-.^_`|~", c) != NULL && c != '\0';
}

Cookie *cookie_jar_parse_set_cookie(const char *host, const char *raw)
{
    if (!host || !raw)
    {
        return NULL;
    }
    static char s[1024]; /* hoisted: device gameTask stack is tiny */
    trim(raw, s, sizeof(s));
    if (s[0] == '\0')
    {
        return NULL;
    }

    /* split at first ';' */
    static char first[512]; /* hoisted: device gameTask stack is tiny */
    static char rest[512]; /* hoisted: device gameTask stack is tiny */
    char *semi = strchr(s, ';');
    if (semi)
    {
        size_t fl = (size_t)(semi - s);
        if (fl >= sizeof(first))
        {
            fl = sizeof(first) - 1;
        }
        memcpy(first, s, fl);
        first[fl] = '\0';
        trim(semi + 1, rest, sizeof(rest)); /* keep attr text; re-split below */
        /* NOTE: Lua iterated gmatch("[^;]+") over the RAW rest (no pre-trim),
         * then trimmed each attr. Preserve that by not collapsing; the trim
         * above only removed the outer whitespace, inner attrs are trimmed
         * individually below anyway. */
    }
    else
    {
        /* s can be up to 1023 bytes; first is 512 — clamp like the split path */
        size_t fl = strlen(s);
        if (fl >= sizeof(first))
        {
            fl = sizeof(first) - 1;
        }
        memcpy(first, s, fl);
        first[fl] = '\0';
        rest[0] = '\0';
    }

    /* name [= value] */
    size_t i = 0;
    while (is_space(first[i]))
    {
        i++;
    }
    size_t n0 = i;
    while (first[i] && first[i] != '=' && first[i] != ';' && !is_space(first[i]))
    {
        i++;
    }
    size_t n1 = i;
    if (n1 == n0)
    {
        return NULL; /* empty name */
    }
    while (is_space(first[i]))
    {
        i++;
    }
    if (first[i] != '=')
    {
        return NULL; /* Lua pattern requires '=' */
    }
    i++;
    while (is_space(first[i]))
    {
        i++;
    }
    static char value[512]; /* hoisted: device gameTask stack is tiny */
    trim(first + i, value, sizeof(value));

    static char name[128]; /* hoisted: device gameTask stack is tiny */
    {
        size_t nl = n1 - n0;
        if (nl >= sizeof(name))
        {
            nl = sizeof(name) - 1;
        }
        memcpy(name, first + n0, nl);
        name[nl] = '\0';
    }

    /* Validation: reject invalid names/values outright. */
    for (const char *p = name; *p; p++)
    {
        if (!name_char_ok(*p))
        {
            return NULL;
        }
    }
    for (const char *p = value; *p; p++)
    {
        if (*p == ';' || *p == ',' || *p == '"' || (unsigned char)*p < 0x20 ||
            (unsigned char)*p == 0x7F)
        {
            return NULL;
        }
    }

    Cookie *c = (Cookie *)PLUTO_MALLOC(sizeof(Cookie));
    if (!c)
    {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->name = dup_str(name);
    c->value = dup_str(value);
    c->domain = dup_str(host);
    c->hostOnly = 1;
    c->path = dup_str("/");
    c->secure = 0;
    c->httpOnly = 0;
    c->samesite[0] = '\0';
    c->expires = -1;
    c->deleteFlag = 0;
    if (!c->name || !c->value || !c->domain || !c->path)
    {
        cookie_jar_free_cookie(c);
        return NULL;
    }

    int domainRejected = 0;
    long long now = now_seconds();

    /* iterate attributes "attr;attr;..." */
    static char attr[512]; /* hoisted: device gameTask stack is tiny */
    const char *rp = rest;
    while (*rp)
    {
        const char *sc = strchr(rp, ';');
        size_t alen = sc ? (size_t)(sc - rp) : strlen(rp);
        if (alen >= sizeof(attr))
        {
            alen = sizeof(attr) - 1;
        }
        memcpy(attr, rp, alen);
        attr[alen] = '\0';

        char a[512];
        trim(attr, a, sizeof(a));
        if (a[0] != '\0')
        {
            /* aname [= avalue] with Lua pattern ^%s*([^=%s]+)%s*=%s*(.-)%s*$ */
            size_t j = 0;
            char aname[64];
            char avalue[384];
            while (is_space(a[j]))
            {
                j++;
            }
            size_t k0 = j;
            while (a[j] && a[j] != '=' && !is_space(a[j]))
            {
                j++;
            }
            size_t k1 = j;
            if (k1 == k0)
            {
                /* no name token: Lua falls back to aname = whole attr */
                trim(a, aname, sizeof(aname));
                avalue[0] = '\0';
            }
            else
            {
                size_t nl = k1 - k0;
                if (nl >= sizeof(aname))
                {
                    nl = sizeof(aname) - 1;
                }
                memcpy(aname, a + k0, nl);
                aname[nl] = '\0';
                while (is_space(a[j]))
                {
                    j++;
                }
                if (a[j] == '=')
                {
                    j++;
                    while (is_space(a[j]))
                    {
                        j++;
                    }
                    trim(a + j, avalue, sizeof(avalue));
                }
                else
                {
                    /* no '=': Lua's match fails → aname = whole attr */
                    trim(a, aname, sizeof(aname));
                    avalue[0] = '\0';
                }
            }

            /* lowercase aname */
            for (char *p = aname; *p; p++)
            {
                if (*p >= 'A' && *p <= 'Z')
                {
                    *p = (char)(*p - 'A' + 'a');
                }
            }

            if (strcmp(aname, "domain") == 0)
            {
                char d[256];
                trim(avalue, d, sizeof(d));
                for (char *p = d; *p; p++)
                {
                    if (*p >= 'A' && *p <= 'Z')
                    {
                        *p = (char)(*p - 'A' + 'a');
                    }
                }
                if (d[0] == '.')
                {
                    memmove(d, d + 1, strlen(d)); /* gsub("^%.", "") */
                }
                size_t dlen = strlen(d);
                size_t hlen = strlen(host);
                int accepted = 0;
                if (strcmp(d, host) == 0 || (hlen > dlen && host[hlen - dlen - 1] == '.' &&
                                             strcmp(host + hlen - dlen, d) == 0))
                {
                    if (strchr(d, '.') || strcmp(d, "localhost") == 0)
                    {
                        PLUTO_FREE(c->domain);
                        c->domain = dup_str(d);
                        c->hostOnly = 0;
                        accepted = 1;
                    }
                }
                if (!accepted)
                {
                    domainRejected = 1;
                }
            }
            else if (strcmp(aname, "path") == 0)
            {
                const char *p = avalue;
                if (p[0] != '/')
                {
                    p = "/";
                }
                PLUTO_FREE(c->path);
                c->path = dup_str(p);
            }
            else if (strcmp(aname, "secure") == 0)
            {
                c->secure = 1;
            }
            else if (strcmp(aname, "httponly") == 0)
            {
                c->httpOnly = 1;
            }
            else if (strcmp(aname, "samesite") == 0)
            {
                char ss[32];
                trim(avalue, ss, sizeof(ss));
                for (char *p = ss; *p; p++)
                {
                    if (*p >= 'A' && *p <= 'Z')
                    {
                        *p = (char)(*p - 'A' + 'a');
                    }
                }
                if (strcmp(ss, "lax") == 0)
                {
                    strcpy(c->samesite, "lax");
                }
                else if (strcmp(ss, "strict") == 0)
                {
                    strcpy(c->samesite, "strict");
                }
                else if (strcmp(ss, "none") == 0)
                {
                    strcpy(c->samesite, "none");
                }
            }
            else if (strcmp(aname, "max-age") == 0)
            {
                /* tonumber semantics: optional sign + digits (decimal part
                 * truncated by the arithmetic in the Lua ref). */
                char *end = NULL;
                long long ma = strtoll(avalue, &end, 10);
                if (end != avalue)
                {
                    if (ma <= 0)
                    {
                        c->deleteFlag = 1;
                    }
                    else
                    {
                        c->expires = now + ma;
                    }
                }
            }
            else if (strcmp(aname, "expires") == 0)
            {
                if (c->expires < 0)
                {
                    long long ts = parse_date(avalue);
                    if (ts > 0)
                    {
                        if (ts <= now)
                        {
                            c->deleteFlag = 1;
                        }
                        else
                        {
                            c->expires = ts;
                        }
                    }
                }
            }
        }

        if (!sc)
        {
            break;
        }
        rp = sc + 1;
    }

    if (domainRejected)
    {
        cookie_jar_free_cookie(c);
        return NULL;
    }
    return c;
}

/* ── store / delete ──────────────────────────────────────────────────────── */

static void list_remove(int index)
{
    cookie_jar_free_cookie(g_list[index]);
    for (int i = index; i < g_count - 1; i++)
    {
        g_list[i] = g_list[i + 1];
    }
    g_list[g_count - 1] = NULL;
    g_count--;
}

void cookie_jar_store(const char *host, const char *raw)
{
    Cookie *parsed = cookie_jar_parse_set_cookie(host, raw);
    if (!parsed)
    {
        return;
    }

    char key[512];
    cookie_key(parsed, key, sizeof(key));

    int i = 0;
    while (i < g_count)
    {
        char k2[512];
        cookie_key(g_list[i], k2, sizeof(k2));
        if (strcmp(k2, key) == 0)
        {
            if (!parsed->deleteFlag && strcmp(g_list[i]->value, parsed->value) == 0 &&
                g_list[i]->expires == parsed->expires)
            {
                cookie_jar_free_cookie(parsed);
                return; /* identical → no-op */
            }
            list_remove(i);
        }
        else
        {
            i++;
        }
    }

    if (!parsed->deleteFlag)
    {
        if (g_count < COOKIE_JAR_MAX)
        {
            g_list[g_count++] = parsed;
        }
        else
        {
            /* Lua: append then table.remove(list, 1) until #list <= 300 —
             * i.e. the NEWEST cookie is kept and the OLDEST is dropped. */
            cookie_jar_free_cookie(g_list[0]);
            for (int k = 1; k < g_count; k++)
            {
                g_list[k - 1] = g_list[k];
            }
            g_list[g_count - 1] = parsed;
        }
    }
    else
    {
        cookie_jar_free_cookie(parsed);
    }
    save();
}

void cookie_jar_process_set_cookies(const char *host, char **list, int count)
{
    if (!list || count <= 0)
    {
        return;
    }
    for (int i = 0; i < count; i++)
    {
        cookie_jar_store(host, list[i]);
    }
}

/* ── getHeader / prune / clear ───────────────────────────────────────────── */

static int cookie_is_dead(const Cookie *c, long long now)
{
    return (!c->name) || (!c->domain) || (c->expires >= 0 && c->expires <= now);
}

char *cookie_jar_get_header(const char *host, const char *path, int isSsl,
                            char *buf, size_t cap)
{
    long long now = now_seconds();
    size_t used = 0;
    if (cap > 0)
    {
        buf[0] = '\0';
    }
    int pruned = 0;

    int i = 0;
    while (i < g_count)
    {
        Cookie *c = g_list[i];
        if (cookie_is_dead(c, now))
        {
            list_remove(i);
            pruned = 1;
            continue;
        }
        if (domain_matches(c, host) && path_matches(c, path) && (!c->secure || isSsl))
        {
            size_t need = strlen(c->name) + 1 + strlen(c->value) + 2;
            size_t room = cap > used ? cap - used : 0;
            if (need < room)
            {
                if (used > 0)
                {
                    buf[used++] = ';';
                    buf[used++] = ' ';
                }
                int w = snprintf(buf + used, cap - used, "%s=%s", c->name, c->value);
                if (w < 0)
                {
                    w = 0;
                }
                size_t lim = cap - used - 1; /* space left for chars, not NUL */
                used += ((size_t)w > lim) ? lim : (size_t)w;
            }
        }
        i++;
    }
    if (pruned)
    {
        save();
    }
    return buf;
}

void cookie_jar_prune(void)
{
    long long now = now_seconds();
    int i = 0;
    while (i < g_count)
    {
        if (cookie_is_dead(g_list[i], now))
        {
            list_remove(i);
        }
        else
        {
            i++;
        }
    }
    /* Faithful to Lua: prune() does NOT call Storage.save(). */
}

void cookie_jar_clear(void)
{
    while (g_count > 0)
    {
        list_remove(0);
    }
    save();
}
