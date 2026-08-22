// url.c — URL parser, normalizer, resolver (see header for parity notes).

#include "url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../util/dynarray.h"
#include "../util/luanum.h"
#include "../util/luapattern.h"
#include "logger.h"

// Literal-with-length for patterns (all literals are NUL-free here).
#define PLIT(lit) (lit), (sizeof(lit) - 1)

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Byte-exact (locale-free) replacements for ctype.h — Lua pattern classes
// operate on raw bytes, so host locale must never influence behavior.
static int ascii_isalnum(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static int ascii_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

// Boolean pattern helpers.
static int pat_match(const char* s, size_t slen, const char* pat, size_t plen)
{
    LPCap caps[LP_MAXCAPTURES];
    int nc;
    return lp_match(s, slen, pat, plen, 0, caps, &nc) > 0;
}

static int pat_find(const char* s, size_t slen, const char* pat, size_t plen)
{
    size_t a, b;
    return lp_find(s, slen, pat, plen, 0, &a, &b) == 1;
}

// Bounded copy; logs when truncation actually occurs.
static void copy_bounded(char* dst, size_t dstSz, const char* src, size_t len,
                         const char* what)
{
    if (len >= dstSz) {
        PLUTO_ERROR("url: truncating %s (%u -> %u)", what, (unsigned)len,
                    (unsigned)(dstSz - 1));
        len = dstSz - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_z(char* dst, size_t dstSz, const char* src, const char* what)
{
    copy_bounded(dst, dstSz, src, strlen(src), what);
}

// Trim a mutable buffer in place; returns new length.
static size_t trim_in_place(char* buf, size_t len)
{
    size_t s = 0, e = len;
    while (s < e && ascii_isspace((unsigned char)buf[s])) s++;
    while (e > s && ascii_isspace((unsigned char)buf[e - 1])) e--;
    if (s > 0 && e > s) {
        memmove(buf, buf + s, e - s);
    }
    buf[e - s] = '\0';
    return e - s;
}

// Lua-style strict numeric conversion lives in util/luanum.

void url_encode(const char* str, StrBuf* out)
{
    static const char hex[] = "0123456789ABCDEF";
    if (str == NULL || out == NULL) {
        return;
    }
    for (; *str != '\0'; str++) {
        unsigned char c = (unsigned char)*str;
        if (c == '\n') {
            // Lua pre-pass replaces "\n" with "\r\n"; both bytes then encode.
            sb_append_str(out, "%0D%0A");
        } else if (c == ' ') {
            // Lua post-pass replaces remaining spaces with '+'.
            sb_append_char(out, '+');
        } else if (ascii_isalnum(c) || c == '-' || c == '_' || c == '.' ||
                   c == '~') {
            sb_append_char(out, (char)c);
        } else {
            sb_append_char(out, '%');
            sb_append_char(out, hex[c >> 4]);
            sb_append_char(out, hex[c & 0x0F]);
        }
    }
}

void url_decode(const char* str, StrBuf* out)
{
    if (str == NULL || out == NULL) {
        return;
    }
    for (; *str != '\0'; str++) {
        if (*str == '+') {
            sb_append_char(out, ' ');
            continue;
        }
        if (*str == '%' && is_hex((unsigned char)str[1]) &&
            is_hex((unsigned char)str[2])) {
            int hi = str[1], lo = str[2];
            int v;
            v = (hi <= '9') ? hi - '0' : ((ascii_lower((char)hi) - 'a') + 10);
            v <<= 4;
            v |= (lo <= '9') ? lo - '0'
                             : ((ascii_lower((char)lo) - 'a') + 10);
            sb_append_char(out, (char)v);
            str += 2;
            continue;
        }
        sb_append_char(out, *str);
    }
}

int url_is_search_query(const char* input)
{
    char buf[PLUTO_URL_INPUT_MAX];
    size_t len, tl;

    if (input == NULL || input[0] == '\0') {
        return 0;
    }
    len = strlen(input);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, input, len);
    buf[len] = '\0';
    tl = trim_in_place(buf, len);

    // Known protocols mean URL.
    if (pat_match(buf, tl, PLIT("^https?://")) ||
        pat_find(buf, tl, PLIT("^about:")) ||
        pat_find(buf, tl, PLIT("^file://"))) {
        return 0;
    }

    // Any whitespace -> search query.
    if (pat_find(buf, tl, PLIT("%s"))) {
        return 1;
    }

    // localhost / bare IP prefix -> URL.
    if (pat_find(buf, tl, PLIT("^localhost")) ||
        pat_find(buf, tl, PLIT("^%d+%.%d+%.%d+%.%d+"))) {
        return 0;
    }

    // A dot that is neither leading nor trailing -> URL.
    if (pat_find(buf, tl, PLIT("%.")) &&
        !pat_find(buf, tl, PLIT("^%.")) &&
        !pat_find(buf, tl, PLIT("%.$"))) {
        return 0;
    }

    return 1;
}

void url_parse(const char* urlString, PlutoUrl* out)
{
    char work[PLUTO_URL_INPUT_MAX];
    char raw[PLUTO_URL_INPUT_MAX];
    size_t rawLen;
    size_t idx, spanEnd;
    LPCap caps[LP_MAXCAPTURES];
    int ncaps;

    memset(out, 0, sizeof(*out));

    // Empty input -> internal about:blank record (Lua parity).
    if (urlString == NULL || urlString[0] == '\0') {
        copy_z(out->normalized, sizeof(out->normalized), "about:blank", "norm");
        copy_z(out->scheme, sizeof(out->scheme), "about", "scheme");
        copy_z(out->host, sizeof(out->host), "blank", "host");
        out->port = 0;
        copy_z(out->path, sizeof(out->path), "/", "path");
        copy_z(out->query, sizeof(out->query), "", "query");
        copy_z(out->hash, sizeof(out->hash), "", "hash");
        copy_z(out->fullPath, sizeof(out->fullPath), "/", "fullPath");
        out->isSsl = 1;
        return;
    }

    // Trimmed raw.
    rawLen = strlen(urlString);
    if (rawLen >= sizeof(raw)) {
        PLUTO_ERROR("url: input over %u bytes truncated",
                    (unsigned)sizeof(raw) - 1);
        rawLen = sizeof(raw) - 1;
    }
    memcpy(work, urlString, rawLen);
    work[rawLen] = '\0';
    rawLen = trim_in_place(work, rawLen);
    memcpy(raw, work, rawLen + 1);
    copy_bounded(out->raw, sizeof(out->raw), raw, rawLen, "raw");

    // about: scheme record
    if (pat_find(raw, rawLen, PLIT("^about:"))) {
        const char* sub = raw + 6;
        size_t subLen = rawLen - 6;
        copy_z(out->scheme, sizeof(out->scheme), "about", "scheme");
        copy_bounded(out->host, sizeof(out->host), sub, subLen, "host");
        out->port = 0;
        out->path[0] = '/';
        copy_bounded(out->path + 1, sizeof(out->path) - 1, sub, subLen, "path");
        copy_z(out->query, sizeof(out->query), "", "query");
        copy_z(out->hash, sizeof(out->hash), "", "hash");
        copy_z(out->fullPath, sizeof(out->fullPath), out->path, "fullPath");
        copy_bounded(out->normalized, sizeof(out->normalized), raw, rawLen, "norm");
        out->isSsl = 1;
        return;
    }

    // Scheme split: default https when no scheme present.
    {
        char rest[PLUTO_URL_INPUT_MAX];
        size_t restLen;

        if (lp_match(raw, rawLen,
                     PLIT("^([a-zA-Z][%w+%-%.]*)://(.*)$"), 0,
                     caps, &ncaps) == 2) {
            copy_bounded(out->scheme, sizeof(out->scheme),
                         raw + caps[0].start, caps[0].len, "scheme");
            restLen = caps[1].len;
            copy_bounded(rest, sizeof(rest), raw + caps[1].start, restLen, "rest");
        } else {
            copy_z(out->scheme, sizeof(out->scheme), "https", "scheme");
            restLen = rawLen;
            memcpy(rest, raw, rawLen);
            rest[restLen] = '\0';
        }
        {
            size_t i;
            for (i = 0; out->scheme[i] != '\0'; i++) {
                out->scheme[i] = ascii_lower(out->scheme[i]);
            }
        }
        out->isSsl = (strcmp(out->scheme, "https") == 0);

        // Split hash FIRST (first '#').
        out->hash[0] = '\0';
        if (lp_find_plain(rest, restLen, "#", 1, 0, &idx, &spanEnd) == 1) {
            copy_bounded(out->hash, sizeof(out->hash), rest + idx + 1,
                         restLen - (idx + 1), "hash");
            restLen = idx;
            rest[restLen] = '\0';
        }

        // Then query (first '?').
        out->query[0] = '\0';
        if (pat_find(rest, restLen, PLIT("%?"))) {
            lp_find(rest, restLen, PLIT("%?"), 0, &idx, &spanEnd);
            copy_bounded(out->query, sizeof(out->query), rest + idx + 1,
                         restLen - (idx + 1), "query");
            restLen = idx;
            rest[restLen] = '\0';
        }

        // Split host[:port] and path at first '/'.
        {
            char hostPart[PLUTO_URL_HOST_MAX];
            size_t hostPartLen;
            char pathPart[PLUTO_URL_PATH_MAX];

            if (lp_find_plain(rest, restLen, "/", 1, 0, &idx, &spanEnd) == 1) {
                hostPartLen = idx;
                copy_bounded(hostPart, sizeof(hostPart), rest, hostPartLen,
                             "hostPart");
                copy_bounded(pathPart, sizeof(pathPart), rest + idx,
                             restLen - idx, "path");
            } else {
                hostPartLen = restLen;
                copy_bounded(hostPart, sizeof(hostPart), rest, restLen,
                             "hostPart");
                pathPart[0] = '/';
                pathPart[1] = '\0';
            }

            // Host + optional custom port.
            hostPart[hostPartLen] = '\0';
            copy_bounded(out->host, sizeof(out->host), hostPart,
                         hostPartLen, "host");
            out->port = out->isSsl ? 443 : 80;
            if (memchr(hostPart, ':', hostPartLen) != NULL) {
                idx = (size_t)((const char*)memchr(hostPart, ':', hostPartLen) -
                               hostPart);
                {
                    char portTxt[64];
                    double customPort;
                    size_t plen = hostPartLen - (idx + 1);
                    if (plen >= sizeof(portTxt)) {
                        plen = sizeof(portTxt) - 1;
                    }
                    memcpy(portTxt, hostPart + idx + 1, plen);
                    portTxt[plen] = '\0';
                    hostPart[idx] = '\0';
                    copy_bounded(out->host, sizeof(out->host), hostPart,
                                 idx, "host");
                    if (pluto_str_tonumber_strict(portTxt, &customPort) &&
                        customPort > 0 &&
                        customPort == (double)(int)customPort) {
                        out->port = (int)customPort;
                    }
                }
            }
            {
                size_t i;
                for (i = 0; out->host[i] != '\0'; i++) {
                    out->host[i] = ascii_lower(out->host[i]);
                }
            }

            if (pathPart[0] == '\0') {
                out->path[0] = '/';
                out->path[1] = '\0';
            } else {
                copy_z(out->path, sizeof(out->path), pathPart, "path");
            }
        }
    }

    // fullPath + normalized.
    {
        StrBuf fp;
        StrBuf norm;
        sb_init(&fp);
        sb_init(&norm);
        sb_append_str(&fp, out->path);
        if (out->query[0] != '\0') {
            sb_append_char(&fp, '?');
            sb_append_str(&fp, out->query);
        }
        if (out->hash[0] != '\0') {
            sb_append_char(&fp, '#');
            sb_append_str(&fp, out->hash);
        }
        copy_bounded(out->fullPath, sizeof(out->fullPath),
                     fp.data ? fp.data : "", fp.len, "fullPath");

        sb_printf(&norm, "%s://%s", out->scheme, out->host);
        if ((out->isSsl && out->port != 443) ||
            (!out->isSsl && out->port != 80 && out->port != 0)) {
            sb_printf(&norm, ":%d", out->port);
        }
        sb_append(&norm, fp.data ? fp.data : "", fp.len);
        copy_bounded(out->normalized, sizeof(out->normalized),
                     norm.data ? norm.data : "", norm.len, "normalized");
        sb_free(&fp);
        sb_free(&norm);
    }
}

int url_unwrap_redirect(const char* urlString, StrBuf* out)
{
    LPCap caps[LP_MAXCAPTURES];
    int ncaps;
    size_t uLen;
    char uddg[PLUTO_URL_INPUT_MAX];

    if (urlString == NULL || urlString[0] == '\0' || out == NULL) {
        return 0;
    }
    uLen = strlen(urlString);
    if (!pat_find(urlString, uLen, PLIT("duckduckgo%.com/l/"))) {
        return 0;
    }
    if (lp_match(urlString, uLen, PLIT("[?&]uddg=([^&]+)"), 0,
                 caps, &ncaps) != 1) {
        return 0;
    }
    if (caps[0].len == 0 || caps[0].len >= sizeof(uddg)) {
        return 0;
    }
    memcpy(uddg, urlString + caps[0].start, caps[0].len);
    uddg[caps[0].len] = '\0';
    url_decode(uddg, out);
    return out->len > 0;
}

// Append "scheme://host[:port]" used by resolve branches. Port shown unless
// it equals the scheme default (80 http / 443 https) — Lua rule inline.
static void append_origin(StrBuf* out, const PlutoUrl* base)
{
    sb_append_str(out, base->scheme);
    sb_append_str(out, "://");
    sb_append_str(out, base->host);
    if (base->port != 80 && base->port != 443) {
        sb_printf(out, ":%d", base->port);
    }
}

void url_resolve(const char* baseUrlStr, const char* relativeUrlStr,
                 StrBuf* out)
{
    char rel[PLUTO_URL_INPUT_MAX];
    size_t relLen;
    PlutoUrl base;

    if (relativeUrlStr == NULL || relativeUrlStr[0] == '\0') {
        sb_append_str(out, baseUrlStr ? baseUrlStr : "");
        return;
    }

    relLen = strlen(relativeUrlStr);
    if (relLen >= sizeof(rel)) {
        PLUTO_ERROR("url: relative over %u bytes truncated",
                    (unsigned)sizeof(rel) - 1);
        relLen = sizeof(rel) - 1;
    }
    memcpy(rel, relativeUrlStr, relLen);
    rel[relLen] = '\0';
    relLen = trim_in_place(rel, relLen);

    // Absolute schemes / internal specials pass through verbatim.
    if (pat_match(rel, relLen, PLIT("^[a-zA-Z][%w+%-%.]*://")) ||
        pat_find(rel, relLen, PLIT("^about:")) ||
        pat_find(rel, relLen, PLIT("^data:")) ||
        pat_match(rel, relLen,
                  PLIT("^[Jj][Aa][Vv][Aa][Ss][Cc][Rr][Ii][Pp][Tt]:"))) {
        sb_append(out, rel, relLen);
        return;
    }

    url_parse(baseUrlStr, &base);
    if (strcmp(base.scheme, "about") == 0) {
        sb_append(out, rel, relLen);
        return;
    }

    // Protocol-relative "//host/path"
    if (relLen >= 2 && rel[0] == '/' && rel[1] == '/') {
        sb_append_str(out, base.scheme);
        sb_append_char(out, ':');
        sb_append(out, rel, relLen);
        return;
    }

    // Anchor-only "#section"
    if (rel[0] == '#') {
        append_origin(out, &base);
        sb_append_str(out, base.path);
        if (base.query[0] != '\0') {
            sb_append_char(out, '?');
            sb_append_str(out, base.query);
        }
        sb_append(out, rel, relLen);
        return;
    }

    // Query-only "?key=val"
    if (rel[0] == '?') {
        append_origin(out, &base);
        sb_append_str(out, base.path);
        sb_append(out, rel, relLen);
        return;
    }

    // Root-relative "/path"
    if (rel[0] == '/') {
        append_origin(out, &base);
        sb_append(out, rel, relLen);
        return;
    }

    // Path-relative with dot-segment normalization.
    {
        typedef struct {
            size_t off;
            size_t len;
        } SegSpan;
        static const char SEG_PAT[] = "[^/]+";
        char combined[PLUTO_URL_INPUT_MAX * 2];
        size_t dirLen = 0;
        size_t i;
        DynArray segs;
        LPGMatch it;
        LPCap caps[LP_MAXCAPTURES];
        int ncaps;
        StrBuf resolvedPath;

        // dir = longest prefix of base.path ending in '/' (Lua "^(.*/)");
        // parse() guarantees path starts with '/', so this always matches.
        for (i = 0; base.path[i] != '\0'; i++) {
            if (base.path[i] == '/') {
                dirLen = i + 1;
            }
        }
        if (dirLen == 0) {
            dirLen = 1; // degenerate: treat as "/"
        }
        if (dirLen + relLen + 1 > sizeof(combined)) {
            PLUTO_ERROR("url: resolve overflow");
            sb_append(out, rel, relLen);
            return;
        }
        memcpy(combined, base.path, dirLen);
        memcpy(combined + dirLen, rel, relLen);
        combined[dirLen + relLen] = '\0';

        da_init(&segs, sizeof(SegSpan));
        lp_gmatch_init(&it, combined, dirLen + relLen, SEG_PAT,
                       sizeof(SEG_PAT) - 1);
        while (lp_gmatch_next(&it, caps, &ncaps)) {
            SegSpan sp;
            sp.off = caps[0].start;
            sp.len = caps[0].len;
            if (sp.len == 2 && combined[sp.off] == '.' &&
                combined[sp.off + 1] == '.') {
                if (segs.count > 0) {
                    da_pop(&segs);
                }
                // beyond-root ".." silently dropped (Lua parity)
            } else if (sp.len == 1 && combined[sp.off] == '.') {
                // "." segments skipped
            } else {
                da_push(&segs, &sp);
            }
        }

        sb_init(&resolvedPath);
        sb_append_char(&resolvedPath, '/');
        for (i = 0; i < segs.count; i++) {
            SegSpan* sp = (SegSpan*)da_get(&segs, i);
            if (i > 0) {
                sb_append_char(&resolvedPath, '/');
            }
            sb_append(&resolvedPath, combined + sp->off, sp->len);
        }
        append_origin(out, &base);
        sb_append(out, resolvedPath.data ? resolvedPath.data : "/",
                  resolvedPath.len);
        sb_free(&resolvedPath);
        da_free(&segs);
    }
}

void url_build_search_url(const char* engineUrl, const char* queryText,
                          StrBuf* out)
{
    if (out == NULL) {
        return;
    }
    sb_append_str(out, engineUrl);
    url_encode(queryText, out);
}
