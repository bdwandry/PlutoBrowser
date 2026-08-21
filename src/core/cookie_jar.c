// cookie_jar.c — see header.

#include "cookie_jar.h"

#include "pd_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../util/dynarray.h"
#include "../util/luanum.h"
#include "../util/luapattern.h"
#include "logger.h"
#include "storage.h"

#define PLIT(lit) (lit), (sizeof(lit) - 1)

static int ascii_isspace_ch(unsigned char c);
static struct PlaydateAPI* s_pd = NULL;
static CJNowFn s_nowFn = NULL;

void cj_init(struct PlaydateAPI* pd) { s_pd = pd; }

void cj_set_now_fn(CJNowFn fn) { s_nowFn = fn; }

double cj_now(void)
{
    if (s_nowFn != NULL) {
        return s_nowFn();
    }
    if (s_pd != NULL) {
        unsigned int ms = 0;
        uint32_t epoch = s_pd->system->getSecondsSinceEpoch(&ms);
        return (double)epoch;
    }
    return 0.0; // Lua: pcall(getTime) failed -> 0
}

// floor division for possibly-negative operands (Lua math.floor(a/b))
static long long fdiv_ll(long long a, long long b)
{
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

double cj_make_timestamp(double year, double month, double day, double hour,
                         double minute, double second)
{
    long long Y, M, D, y, era, yoe, mp, doy, doe, days;

    if (year < 0.0) year = 0.0;
    year = (double)(long long)year; // math.floor (values >= 0 here)
    if (month < 1.0) month = 1.0;
    if (month > 12.0) month = 12.0;
    month = (double)(long long)month;
    if (day < 1.0) day = 1.0;
    day = (double)(long long)day;

    Y = (long long)year;
    M = (long long)month;
    D = (long long)day;

    y = Y - ((M <= 2) ? 1 : 0);
    era = fdiv_ll(y, 400);
    yoe = y - era * 400;
    mp = M + ((M > 2) ? -3 : 9);
    doy = (153 * mp + 2) / 5 + D - 1;
    doe = yoe * 365 + fdiv_ll(yoe, 4) - fdiv_ll(yoe, 100) + doy;
    days = era * 146097 + doe - 719468;

    return (double)days * 86400.0 + hour * 3600.0 + minute * 60.0 +
           second;
}

// ---------------------------------------------------------------- dates ----

static int month_from_name(const char* mon, size_t len)
{
    static const char* const kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    int i;
    if (len != 3) {
        return 0;
    }
    for (i = 0; i < 12; i++) {
        if (memcmp(mon, kMonths[i], 3) == 0) {
            return i + 1; // case-sensitive like the Lua MONTHS table
        }
    }
    return 0;
}

// Copy capture i as a NUL-terminated string into out.
static int cap_copy(const char* subj, const LPCap caps[], int ncaps, int i,
                    char* out, size_t outSz)
{
    size_t cs, cl;
    if (!lp_cap_span(subj, caps, ncaps, i, &cs, &cl)) {
        return 0;
    }
    if (cl >= outSz) {
        cl = outSz - 1;
    }
    memcpy(out, subj + cs, cl);
    out[cl] = '\0';
    return 1;
}

static int cap_num(const char* subj, const LPCap caps[], int ncaps, int i,
                   double* out)
{
    char tmp[32];
    if (!cap_copy(subj, caps, ncaps, i, tmp, sizeof(tmp))) {
        return 0;
    }
    return pluto_str_tonumber_strict(tmp, out);
}

int cj_parse_date(const char* str, double* out)
{
    char buf[128];
    size_t len;
    LPCap caps[LP_MAXCAPTURES];
    int nc;
    double d, mo, y, h, mi, s;
    int m;

    if (str == NULL || str[0] == '\0') {
        return 0;
    }
    len = strlen(str);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, str, len);
    buf[len] = '\0';
    {
        size_t st = 0, en = len;
        while (st < en && ascii_isspace_ch((unsigned char)buf[st])) st++;
        while (en > st && ascii_isspace_ch((unsigned char)buf[en - 1])) en--;
        if (st > 0 && en > st) memmove(buf, buf + st, en - st);
        buf[en - st] = '\0';
        len = en - st;
    }
    if (len == 0) {
        return 0;
    }

    // IMF-fixdate: "Wed, 21 Oct 2015 07:28:00 GMT"
    if (lp_match(buf, len,
                 PLIT("^%a+, (%d+) (%a+) (%d+) (%d+):(%d+):(%d+)"), 0,
                 caps, &nc) == 6 &&
        cap_num(buf, caps, nc, 0, &d) && cap_num(buf, caps, nc, 2, &y) &&
        cap_num(buf, caps, nc, 3, &h) && cap_num(buf, caps, nc, 4, &mi) &&
        cap_num(buf, caps, nc, 5, &s)) {
        char monName[8];
        if (!cap_copy(buf, caps, nc, 1, monName, sizeof(monName))) {
            return 0;
        }
        m = month_from_name(monName, strlen(monName));
        if (m == 0) {
            return 0;
        }
        *out = cj_make_timestamp(y, (double)m, d, h, mi, s);
        return 1;
    }

    // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"
    if (lp_match(buf, len,
                 PLIT("^%a+, (%d+)%-(%a+)%-(%d+) (%d+):(%d+):(%d+)"), 0,
                 caps, &nc) == 6 &&
        cap_num(buf, caps, nc, 0, &d) && cap_num(buf, caps, nc, 2, &y) &&
        cap_num(buf, caps, nc, 3, &h) && cap_num(buf, caps, nc, 4, &mi) &&
        cap_num(buf, caps, nc, 5, &s)) {
        char monName[8];
        if (!cap_copy(buf, caps, nc, 1, monName, sizeof(monName))) {
            return 0;
        }
        m = month_from_name(monName, strlen(monName));
        if (m == 0) {
            return 0;
        }
        if (y < 70.0) y += 2000.0;
        else if (y < 100.0) y += 1900.0;
        *out = cj_make_timestamp(y, (double)m, d, h, mi, s);
        return 1;
    }

    // asctime: "Sun Nov 06 08:49:37 1994" — note the single-space quirk:
    // real asctime pads single-digit days ("Sun Nov  6") which does NOT
    // match this pattern (Lua parity preserved).
    if (lp_match(buf, len,
                 PLIT("^%a+ (%a+) (%d+) (%d+):(%d+):(%d+) (%d+)"), 0,
                 caps, &nc) == 6 &&
        cap_num(buf, caps, nc, 1, &d) && cap_num(buf, caps, nc, 5, &y) &&
        cap_num(buf, caps, nc, 2, &h) && cap_num(buf, caps, nc, 3, &mi) &&
        cap_num(buf, caps, nc, 4, &s)) {
        char monName[8];
        if (!cap_copy(buf, caps, nc, 0, monName, sizeof(monName))) {
            return 0;
        }
        m = month_from_name(monName, strlen(monName));
        if (m == 0) {
            return 0;
        }
        *out = cj_make_timestamp(y, (double)m, d, h, mi, s);
        return 1;
    }

    return 0;
}

// ------------------------------------------------------- set-cookie parse ----

static char ascii_lower_c(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static void copy_field(char* dst, size_t dstSz, const char* src)
{
    size_t n;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstSz) {
        n = dstSz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void lower_into(char* dst, size_t dstSz, const char* src, size_t n)
{
    size_t i;
    if (n >= dstSz) n = dstSz - 1;
    for (i = 0; i < n; i++) dst[i] = ascii_lower_c(src[i]);
    dst[n] = '\0';
}

static int ascii_isspace_ch(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static void trim_span_buf(char* buf, size_t* len)
{
    size_t s = 0, e = *len;
    while (s < e && ascii_isspace_ch((unsigned char)buf[s])) s++;
    while (e > s && ascii_isspace_ch((unsigned char)buf[e - 1])) e--;
    if (s > 0 && e > s) memmove(buf, buf + s, e - s);
    buf[e - s] = '\0';
    *len = e - s;
}

// RFC 2616 token chars allowed in cookie names.
static int token_char(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z')) {
        return 1;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return 1;
        default:
            return 0;
    }
}

int cj_parse_set_cookie(const char* host, const char* raw, PlutoCookie* out)
{
    char buf[1024];
    size_t bufLen;
    char first[512], rest[768];
    size_t firstLen, restLen;
    LPCap caps[LP_MAXCAPTURES];
    int nc;
    char name[PLUTO_CK_NAME_MAX];
    char value[PLUTO_CK_VALUE_MAX];
    int domainRejected = 0;
    LPGMatch it;

    memset(out, 0, sizeof(*out));
    if (host == NULL || raw == NULL) {
        return 0;
    }
    // defaults (Lua record literal)
    {
        size_t hl = strlen(host);
        if (hl >= PLUTO_CK_DOMAIN_MAX) hl = PLUTO_CK_DOMAIN_MAX - 1;
        memcpy(out->domain, host, hl);
        out->domain[hl] = '\0';
    }
    out->hostOnly = 1;
    strcpy(out->path, "/");
    out->del = 0;

    bufLen = strlen(raw);
    if (bufLen >= sizeof(buf)) {
        bufLen = sizeof(buf) - 1;
    }
    memcpy(buf, raw, bufLen);
    buf[bufLen] = '\0';
    trim_span_buf(buf, &bufLen);
    if (bufLen == 0) {
        return 0;
    }

    // split at FIRST ';'
    firstLen = bufLen;
    restLen = 0;
    rest[0] = '\0';
    {
        const char* semi = memchr(buf, ';', bufLen);
        if (semi != NULL) {
            firstLen = (size_t)(semi - buf);
            restLen = bufLen - firstLen - 1;
            memcpy(rest, semi + 1, restLen);
            rest[restLen] = '\0';
        }
    }
    if (firstLen >= sizeof(first)) {
        firstLen = sizeof(first) - 1;
    }
    memcpy(first, buf, firstLen);
    first[firstLen] = '\0';

    // name=value
    if (lp_match(first, firstLen, PLIT("^%s*([^=;%s]+)%s*=%s*(.*)$"), 0,
                 caps, &nc) != 2) {
        return 0;
    }
    if (!cap_copy(first, caps, nc, 0, name, sizeof(name))) {
        return 0;
    }
    {
        char vraw[PLUTO_CK_VALUE_MAX];
        size_t vlen;
        if (!cap_copy(first, caps, nc, 1, vraw, sizeof(vraw))) {
            return 0;
        }
        vlen = strlen(vraw);
        trim_span_buf(vraw, &vlen);
        if (vlen >= sizeof(value)) {
            vlen = sizeof(value) - 1;
        }
        memcpy(value, vraw, vlen);
        value[vlen] = '\0';
    }

    // validation
    {
        size_t i;
        for (i = 0; name[i] != '\0'; i++) {
            if (!token_char((unsigned char)name[i])) {
                return 0;
            }
        }
        for (i = 0; value[i] != '\0'; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c == ';' || c == ',' || c == '"' || c < 0x20 || c == 0x7F) {
                return 0;
            }
        }
    }
    strcpy(out->name, name);
    strcpy(out->value, value);

    lp_gmatch_init(&it, rest, restLen, "[^;]+", 5);
    while (lp_gmatch_next(&it, caps, &nc)) {
        char a[256];
        size_t alen;
        char aname[64], avalue[600];
        char anameLow[64];

        if (!cap_copy(rest, caps, nc, 0, a, sizeof(a))) {
            continue;
        }
        alen = strlen(a);
        trim_span_buf(a, &alen);
        if (alen == 0) {
            continue;
        }

        if (lp_match(a, alen, PLIT("^%s*([^=%s]+)%s*=%s*(.-)%s*$"), 0,
                     caps, &nc) == 2) {
            if (!cap_copy(a, caps, nc, 0, aname, sizeof(aname)) ||
                !cap_copy(a, caps, nc, 1, avalue, sizeof(avalue))) {
                continue;
            }
        } else {
            // no '=' present: aname = whole attr text, avalue = ""
            size_t n2 = alen;
            if (n2 >= sizeof(aname)) {
                n2 = sizeof(aname) - 1;
            }
            memcpy(aname, a, n2);
            aname[n2] = '\0';
            avalue[0] = '\0';
        }
        lower_into(anameLow, sizeof(anameLow), aname, strlen(aname));

        if (strcmp(anameLow, "domain") == 0) {
            char d[PLUTO_CK_DOMAIN_MAX];
            size_t dl;
            copy_field(d, sizeof(d), avalue);
            dl = strlen(d);
            trim_span_buf(d, &dl);
            lower_into(d, sizeof(d), d, dl); // lowercase in place
            if (dl > 0 && d[0] == '.') {     // strip ONE leading dot
                memmove(d, d + 1, dl);       // includes trailing NUL
                dl--;
            }
            {
                size_t hl = strlen(host);
                int hostMatch =
                    (strcmp(d, host) == 0) ||
                    (hl > dl && host[hl - dl - 1] == '.' &&
                     strcmp(host + hl - dl, d) == 0);
                if (hostMatch) {
                    // accept only dotted domains or literal "localhost"
                    if (strchr(d, '.') != NULL ||
                        strcmp(d, "localhost") == 0) {
                        copy_field(out->domain, sizeof(out->domain), d);
                        out->hostOnly = 0;
                    }
                    // else: matched but undotted non-localhost -> ignored
                } else {
                    domainRejected = 1;
                }
            }
        } else if (strcmp(anameLow, "path") == 0) {
            if (avalue[0] != '/') {
                strcpy(out->path, "/");
            } else {
                copy_field(out->path, sizeof(out->path), avalue);
            }
        } else if (strcmp(anameLow, "secure") == 0) {
            out->secure = 1;
        } else if (strcmp(anameLow, "httponly") == 0) {
            out->httpOnly = 1;
        } else if (strcmp(anameLow, "samesite") == 0) {
            char ss[32];
            lower_into(ss, sizeof(ss), avalue, strlen(avalue));
            if (strcmp(ss, "lax") == 0 || strcmp(ss, "strict") == 0 ||
                strcmp(ss, "none") == 0) {
                copy_field(out->samesite, sizeof(out->samesite), ss);
            }
        } else if (strcmp(anameLow, "max-age") == 0) {
            double ma;
            if (pluto_str_tonumber_strict(avalue, &ma)) {
                if (ma <= 0.0) {
                    out->del = 1;
                } else {
                    out->expires = cj_now() + ma;
                    out->hasExpires = 1;
                }
            }
        } else if (strcmp(anameLow, "expires") == 0) {
            if (!out->hasExpires) {
                double tsVal;
                if (cj_parse_date(avalue, &tsVal)) {
                    if (tsVal <= cj_now()) {
                        out->del = 1;
                    } else {
                        out->expires = tsVal;
                        out->hasExpires = 1;
                    }
                }
            }
        }
    }

    if (domainRejected) {
        return 0;
    }
    return 1;
}

// ------------------------------------------------------------- matching ----

static int host_matches_domain(const char* host, const char* domain)
{
    size_t hl = strlen(host), dl = strlen(domain);
    if (strcmp(host, domain) == 0) {
        return 1;
    }
    return hl > dl && host[hl - dl - 1] == '.' &&
           strcmp(host + hl - dl, domain) == 0;
}

static int domain_matches(const PlutoCookie* c, const char* host)
{
    if (c->hostOnly) {
        return strcmp(host, c->domain) == 0;
    }
    return host_matches_domain(host, c->domain);
}

static int path_matches(const char* cpath, const char* rp)
{
    size_t pl = strlen(cpath);
    if (rp == NULL || rp[0] == '\0') {
        rp = "/";
    }
    if (strcmp(rp, cpath) == 0) {
        return 1;
    }
    if (strncmp(rp, cpath, pl) != 0) {
        return 0;
    }
    if (pl > 0 && cpath[pl - 1] == '/') {
        return 1;
    }
    return rp[pl] == '/';
}

static void build_key(const PlutoCookie* c, StrBuf* out)
{
    sb_append_str(out, c->hostOnly ? "H:" : "D:");
    sb_append_str(out, c->domain);
    sb_append_char(out, '|');
    sb_append_str(out, c->path[0] != '\0' ? c->path : "/");
    sb_append_char(out, '|');
    sb_append_str(out, c->name);
}

// Remove element i (order-preserving); local copy of storage.c helper.
static void cj_remove_at(DynArray* a, size_t i)
{
    if (i >= a->count) {
        return;
    }
    if (i + 1 < a->count) {
        memmove((char*)a->items + i * a->elemsize,
                (char*)a->items + (i + 1) * a->elemsize,
                (a->count - i - 1) * a->elemsize);
    }
    a->count--;
}

void cj_store(const char* host, const char* raw)
{
    PlutoCookie parsed;
    DynArray* list;
    StrBuf key, ktmp;
    size_t i;

    if (!cj_parse_set_cookie(host, raw, &parsed)) {
        return;
    }

    list = storage_cookies();
    sb_init(&key);
    build_key(&parsed, &key);
    sb_init(&ktmp);

    i = 0;
    while (i < list->count) {
        PlutoCookie* c = (PlutoCookie*)da_get(list, i);
        int sameKey;
        sb_clear(&ktmp);
        build_key(c, &ktmp);
        sameKey = (c != NULL && ktmp.data != NULL && key.data != NULL &&
                   strcmp(ktmp.data, key.data) == 0);
        if (sameKey) {
            // Lua: identical value AND expiry (and not a delete) -> no-op
            // WITHOUT save; otherwise remove the stored entry.
            if (!parsed.del && strcmp(c->value, parsed.value) == 0 &&
                c->hasExpires == parsed.hasExpires &&
                (!parsed.hasExpires || c->expires == parsed.expires)) {
                sb_free(&key);
                sb_free(&ktmp);
                return;
            }
            cj_remove_at(list, i);
        } else {
            i++;
        }
    }
    sb_free(&key);
    sb_free(&ktmp);

    if (!parsed.del) {
        da_push(list, &parsed);
        while (list->count > CJ_MAX_COOKIES) {
            cj_remove_at(list, 0); // evict OLDEST (front)
        }
    }
    storage_save();
}

void cj_process_set_cookies(const char* host, const char* const* list,
                            size_t n)
{
    size_t i;
    if (list == NULL || n == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        cj_store(host, list[i]);
    }
}

// Remove malformed / expired entries; returns how many were dropped.
static size_t prune_pass(DynArray* list, double now)
{
    size_t i = 0, removed = 0;
    while (i < list->count) {
        PlutoCookie* c = (PlutoCookie*)da_get(list, i);
        int drop = (c == NULL || c->name[0] == '\0' ||
                    c->domain[0] == '\0' ||
                    (c->hasExpires && c->expires <= now));
        if (drop) {
            cj_remove_at(list, i);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

void cj_get_header(const char* host, const char* path, int isSsl, StrBuf* out)
{
    DynArray* list = storage_cookies();
    double now = cj_now();
    size_t pruned;
    size_t i;
    int first = 1;

    if (out == NULL) {
        return;
    }
    pruned = prune_pass(list, now);

    for (i = 0; i < list->count; i++) {
        PlutoCookie* c = (PlutoCookie*)da_get(list, i);
        if (c == NULL || !domain_matches(c, host)) {
            continue;
        }
        if (!path_matches(c->path, path)) {
            continue;
        }
        if (c->secure && !isSsl) {
            continue;
        }
        if (!first) {
            sb_append_str(out, "; ");
        }
        sb_append_str(out, c->name);
        sb_append_char(out, '=');
        sb_append_str(out, c->value);
        first = 0;
    }

    if (pruned > 0) {
        storage_save();
    }
}

// Drop expired / malformed cookies (startup pass; no save — Lua parity).
void cj_prune(void)
{
    prune_pass(storage_cookies(), cj_now());
}

void cj_clear(void)
{
    storage_cookies()->count = 0;
    storage_save();
}

size_t cj_count(void)
{
    return storage_cookies()->count;
}
