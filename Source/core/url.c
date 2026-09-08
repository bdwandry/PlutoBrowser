/*
 * PlutoBrowser — url.c
 * Port of Source/core/url.lua. Every function mirrors the Lua reference's
 * observable behavior, including its quirks:
 *   - encode maps newline → \r\n first, then percent-encodes non [A-Za-z0-9 -_.~ ]
 *     bytes, then maps ' ' → '+'.
 *   - decode maps '+' → ' ' first, then %XX.
 *   - isSearchQuery: protocol/about/file prefixes are URLs; whitespace means
 *     search; localhost/IP are URLs; a dot with non-leading/non-trailing dot
 *     (i.e. contains a domain-ish token) is a URL; otherwise search.
 *   - parse defaults missing scheme to https; port defaults 443/80; about:
 *     pages get host = sub, path = "/" .. sub.
 *   - resolve handles absolute, protocol-relative, #-only, ?-only, /-rooted and
 *     path-relative (with '.'/'..' segment normalization) forms.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/url.h"
#include "util/strutil.h"

extern void pluto_free(void *p);

/* ── internal helpers ─────────────────────────────────────────────────────── */

static void url_free(char *p)
{
    pluto_free(p);
}

static int is_unreserved(unsigned char c)
{
    /* Lua pattern [^%w %-%_%.%~]: word chars, space, '-', '_', '.', '~' are kept */
    if (isalnum(c))
    {
        return 1;
    }
    switch (c)
    {
    case ' ':
    case '-':
    case '_':
    case '.':
    case '~':
        return 1;
    default:
        return 0;
    }
}

static int scheme_prefix_len(const char *s)
{
    /* ^[a-zA-Z][%w+%-%.]*:// — scheme then "://" */
    size_t i = 0;
    if (!s || !isalpha((unsigned char)s[0]))
    {
        return 0;
    }
    i = 1;
    while (s[i] != '\0' && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.'))
    {
        i++;
    }
    if (strncmp(s + i, "://", 3) == 0)
    {
        return (int)(i + 3);
    }
    return 0;
}

static int has_any_space(const char *s)
{
    if (!s)
    {
        return 0;
    }
    for (const char *p = s; *p; p++)
    {
        if (strutil_is_space(*p))
        {
            return 1;
        }
    }
    return 0;
}

static void append_port_if_needed(char *buf, size_t bufSize, const char *scheme, int port)
{
    size_t len = strlen(buf);
    int isSsl = strcmp(scheme, "https") == 0;
    int showPort = (isSsl && port != 443) || (!isSsl && port != 80 && port != 0);
    if (showPort)
    {
        snprintf(buf + len, bufSize - len, ":%d", port);
    }
}

/* ── public API ───────────────────────────────────────────────────────────── */

char *url_encode(const char *str)
{
    if (!str)
    {
        str = "";
    }
    /* First pass: size. CRLF expansion and %XX escapes. */
    size_t maxLen = 0;
    for (const char *p = str; *p; p++)
    {
        if (*p == '\n')
        {
            maxLen += 2;
        }
        else if (is_unreserved((unsigned char)*p) || *p == '\r')
        {
            maxLen += 1;
        }
        else
        {
            maxLen += 3;
        }
    }
    char *out = (char *)malloc(maxLen + 1);
    if (!out)
    {
        return NULL;
    }
    char *o = out;
    for (const char *p = str; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\n')
        {
            *o++ = '\r';
            *o++ = '\n';
        }
        else if (is_unreserved(c))
        {
            /* Lua keeps the byte as-is here; space becomes '+' below. */
            *o++ = (char)c;
        }
        else
        {
            o += sprintf(o, "%%%02X", c);
        }
    }
    /* Second pass: ' ' → '+' in-place (shrink-only, safe). */
    char *w = out;
    for (const char *r = out; *r; r++)
    {
        *w++ = (*r == ' ') ? '+' : *r;
    }
    *w = '\0';
    return out;
}

char *url_decode(const char *str)
{
    if (!str)
    {
        str = "";
    }
    size_t n = strlen(str);
    char *out = (char *)malloc(n + 1);
    if (!out)
    {
        return NULL;
    }
    char *o = out;
    for (const char *p = str; *p;)
    {
        if (*p == '+')
        {
            *o++ = ' ';
            p++;
        }
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))
        {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 3;
        }
        else
        {
            *o++ = *p++;
        }
    }
    *o = '\0';
    return out;
}

int url_is_search_query(const char *input)
{
    if (!input || !*input)
    {
        return 0;
    }
    char *trimmed = strutil_trim_dup(input);
    if (!trimmed)
    {
        return 0;
    }
    const char *t = trimmed;

    if (strutil_istarts_with(t, "http://") || strutil_istarts_with(t, "https://") ||
        strutil_istarts_with(t, "about:") || strutil_istarts_with(t, "file://"))
    {
        url_free(trimmed);
        return 0;
    }

    if (has_any_space(t))
    {
        url_free(trimmed);
        return 1;
    }

    /* localhost or dotted-quad IP */
    if (strncmp(t, "localhost", 9) == 0)
    {
        url_free(trimmed);
        return 0;
    }
    {
        int digits = 0, dots = 0, ok = 1;
        for (const char *p = t; *p; p++)
        {
            if (isdigit((unsigned char)*p))
            {
                digits++;
            }
            else if (*p == '.')
            {
                dots++;
            }
            else
            {
                ok = 0;
                break;
            }
        }
        if (ok && digits > 0 && dots == 3)
        {
            url_free(trimmed);
            return 0; /* IPv4 */
        }
    }

    /* Contains a dot that is neither leading nor trailing → URL (Lua's
     * "[%."]-heuristic with ^%. and %.$ exclusions). */
    {
        size_t len = strlen(t);
        if (len >= 1 && t[0] != '.' && t[len - 1] != '.' && strchr(t, '.') != NULL)
        {
            url_free(trimmed);
            return 0;
        }
    }

    url_free(trimmed);
    return 1;
}

int url_parse(const char *urlString, UrlParsed *out)
{
    memset(out, 0, sizeof(*out));

    if (!urlString || !*urlString)
    {
        strcpy(out->raw, "");
        strcpy(out->normalized, "about:blank");
        strcpy(out->scheme, "about");
        strcpy(out->host, "blank");
        out->port = 0;
        strcpy(out->path, "/");
        strcpy(out->query, "");
        strcpy(out->hash, "");
        strcpy(out->fullPath, "/");
        out->isSsl = 1;
        return 0;
    }

    char *raw = strutil_trim_dup(urlString);
    if (!raw)
    {
        return -1;
    }
    snprintf(out->raw, sizeof(out->raw), "%s", raw);

    /* about: pages */
    if (strncmp(raw, "about:", 6) == 0)
    {
        const char *sub = raw + 6;
        snprintf(out->normalized, sizeof(out->normalized), "%s", raw);
        strcpy(out->scheme, "about");
        snprintf(out->host, sizeof(out->host), "%s", sub);
        out->port = 0;
        snprintf(out->path, sizeof(out->path), "/%s", sub);
        strcpy(out->query, "");
        strcpy(out->hash, "");
        snprintf(out->fullPath, sizeof(out->fullPath), "/%s", sub);
        out->isSsl = 1;
        url_free(raw);
        return 0;
    }

    static char scheme[12]; /* BSS: initialized explicitly below (stack hoist) */
    scheme[0] = 'h'; scheme[1] = 't'; scheme[2] = 't'; scheme[3] = 'p'; scheme[4] = 's'; scheme[5] = '\0';
    const char *rest = raw;
    int splen = scheme_prefix_len(raw);
    if (splen > 0)
    {
        size_t sl = (size_t)splen - 3;
        if (sl >= sizeof(scheme))
        {
            sl = sizeof(scheme) - 1;
        }
        memcpy(scheme, raw, sl);
        scheme[sl] = '\0';
        for (char *c = scheme; *c; c++)
        {
            *c = (char)tolower((unsigned char)*c);
        }
        rest = raw + splen;
    }
    int isSsl = strcmp(scheme, "https") == 0;
    snprintf(out->scheme, sizeof(out->scheme), "%s", scheme);
    out->isSsl = isSsl;

    static char hash[128]; /* BSS: zeroed explicitly below (stack hoist) */
    hash[0] = '\0';
    const char *hashIdx = strchr(rest, '#');
    if (hashIdx)
    {
        snprintf(hash, sizeof(hash), "%s", hashIdx + 1);
        size_t keep = (size_t)(hashIdx - rest);
        /* truncate rest in place: rest points into raw */
        ((char *)rest)[keep] = '\0'; /* safe: rest is inside raw buffer */
    }

    static char query[256]; /* BSS: zeroed explicitly below (stack hoist) */
    query[0] = '\0';
    const char *queryIdx = strchr(rest, '?');
    if (queryIdx)
    {
        snprintf(query, sizeof(query), "%s", queryIdx + 1);
        ((char *)rest)[queryIdx - rest] = '\0';
    }

    /* host[:port] and path */
    char hostPart[128] = "";
    char pathPart[256] = "/";
    const char *slashIdx = strchr(rest, '/');
    if (slashIdx)
    {
        size_t hl = (size_t)(slashIdx - rest);
        if (hl >= sizeof(hostPart))
        {
            hl = sizeof(hostPart) - 1;
        }
        memcpy(hostPart, rest, hl);
        hostPart[hl] = '\0';
        snprintf(pathPart, sizeof(pathPart), "%s", slashIdx);
    }
    else
    {
        snprintf(hostPart, sizeof(hostPart), "%s", rest);
    }
    if (pathPart[0] == '\0')
    {
        strcpy(pathPart, "/");
    }

    /* host/port */
    static char host[128]; /* hoisted: device gameTask stack is tiny */
    int port = isSsl ? 443 : 80;
    const char *colonIdx = strchr(hostPart, ':');
    if (colonIdx)
    {
        size_t hl = (size_t)(colonIdx - hostPart);
        if (hl >= sizeof(host))
        {
            hl = sizeof(host) - 1;
        }
        memcpy(host, hostPart, hl);
        host[hl] = '\0';
        int customPort = atoi(colonIdx + 1);
        if (customPort > 0)
        {
            port = customPort;
        }
    }
    else
    {
        snprintf(host, sizeof(host), "%s", hostPart);
    }
    for (char *c = host; *c; c++)
    {
        *c = (char)tolower((unsigned char)*c);
    }
    snprintf(out->host, sizeof(out->host), "%s", host);
    out->port = port;
    snprintf(out->path, sizeof(out->path), "%s", pathPart);
    snprintf(out->query, sizeof(out->query), "%s", query);
    snprintf(out->hash, sizeof(out->hash), "%s", hash);

    /* fullPath */
    snprintf(out->fullPath, sizeof(out->fullPath), "%s", pathPart);
    if (query[0] != '\0')
    {
        strncat(out->fullPath, "?", sizeof(out->fullPath) - strlen(out->fullPath) - 1);
        strncat(out->fullPath, query, sizeof(out->fullPath) - strlen(out->fullPath) - 1);
    }
    if (hash[0] != '\0')
    {
        strncat(out->fullPath, "#", sizeof(out->fullPath) - strlen(out->fullPath) - 1);
        strncat(out->fullPath, hash, sizeof(out->fullPath) - strlen(out->fullPath) - 1);
    }

    /* normalized */
    {
        char portBuf[16] = "";
        int isSsl2 = strcmp(scheme, "https") == 0;
        if ((isSsl2 && port != 443) || (!isSsl2 && port != 80 && port != 0))
        {
            snprintf(portBuf, sizeof(portBuf), ":%d", port);
        }
        /* Truncation of pathological URLs is acceptable (Lua capped too). */
        (void)snprintf(out->normalized, sizeof(out->normalized), "%s://%s%s%.300s",
                       scheme, host, portBuf, out->fullPath);
    }

    url_free(raw);
    return 0;
}

char *url_unwrap_redirect(const char *urlString)
{
    if (!urlString || !*urlString)
    {
        return NULL;
    }
    if (strstr(urlString, "duckduckgo.com/l/") == NULL)
    {
        return NULL;
    }
    /* find uddg= param: [?&]uddg=([^&]+) */
    const char *p = urlString;
    const char *uddg = NULL;
    while ((p = strchr(p, 'u')) != NULL)
    {
        if (strncmp(p, "uddg=", 5) == 0 &&
            (p == urlString || p[-1] == '?' || p[-1] == '&'))
        {
            uddg = p + 5;
            break;
        }
        p++;
    }
    if (!uddg)
    {
        return NULL;
    }
    size_t len = 0;
    while (uddg[len] != '\0' && uddg[len] != '&')
    {
        len++;
    }
    char *enc = (char *)malloc(len + 1);
    if (!enc)
    {
        return NULL;
    }
    memcpy(enc, uddg, len);
    enc[len] = '\0';
    char *dec = url_decode(enc);
    url_free(enc);
    return dec;
}

char *url_resolve(const char *baseUrlStr, const char *relativeUrlStr)
{
    if (!relativeUrlStr || !*relativeUrlStr)
    {
        return strutil_trim_dup(baseUrlStr ? baseUrlStr : "");
    }

    char *rel = strutil_trim_dup(relativeUrlStr);
    if (!rel)
    {
        return NULL;
    }

    /* Absolute scheme, about:, data: or javascript: → as-is */
    if (scheme_prefix_len(rel) > 0 || strutil_istarts_with(rel, "about:") ||
        strutil_istarts_with(rel, "data:") || strutil_istarts_with(rel, "javascript:"))
    {
        return rel; /* transfer ownership */
    }

    static UrlParsed base; /* hoisted: device gameTask stack is tiny */
    url_parse(baseUrlStr ? baseUrlStr : "", &base);
    if (strcmp(base.scheme, "about") == 0)
    {
        return rel;
    }

    /* Port suffix helper value */
    char portStr[16] = "";
    if (base.port != 80 && base.port != 443)
    {
        snprintf(portStr, sizeof(portStr), ":%d", base.port);
    }

    char *out = NULL;

    /* Protocol-relative: //host/path */
    if (rel[0] == '/' && rel[1] == '/')
    {
        size_t need = strlen(base.scheme) + 1 + strlen(rel) + 1;
        out = (char *)malloc(need);
        if (out)
        {
            snprintf(out, need, "%s:%s", base.scheme, rel);
        }
        url_free(rel);
        return out;
    }

    /* Anchor-only: #section — note the Lua builds this WITHOUT portStr when
     * port is 80/443 but the expression differs slightly; mirror Lua exactly:
     * scheme://host[:port-if-not-80/443]path[?query]rel  */
    if (rel[0] == '#')
    {
        char anchorPort[16] = "";
        /* Lua: (base.port ~= 80 and base.port ~= 443 and (":"..base.port) or "") */
        if (base.port != 80 && base.port != 443)
        {
            snprintf(anchorPort, sizeof(anchorPort), ":%d", base.port);
        }
        size_t need = strlen(base.scheme) + 3 + strlen(base.host) + strlen(anchorPort) +
                      strlen(base.path) + strlen(base.query) + 1 + strlen(rel) + 8;
        out = (char *)malloc(need);
        if (out)
        {
            snprintf(out, need, "%s://%s%s%s%s%s",
                     base.scheme, base.host, anchorPort, base.path,
                     base.query[0] ? "?" : "", base.query);
            strcat(out, rel);
        }
        url_free(rel);
        return out;
    }

    /* Query-only: ?key=val (Lua omits query here) */
    if (rel[0] == '?')
    {
        size_t need = strlen(base.scheme) + 3 + strlen(base.host) + strlen(portStr) +
                      strlen(base.path) + strlen(rel) + 1;
        out = (char *)malloc(need);
        if (out)
        {
            snprintf(out, need, "%s://%s%s%s%s",
                     base.scheme, base.host, portStr, base.path, rel);
        }
        url_free(rel);
        return out;
    }

    /* Root-relative: /path */
    if (rel[0] == '/')
    {
        size_t need = strlen(base.scheme) + 3 + strlen(base.host) + strlen(portStr) +
                      strlen(rel) + 1;
        out = (char *)malloc(need);
        if (out)
        {
            snprintf(out, need, "%s://%s%s%s", base.scheme, base.host, portStr, rel);
        }
        url_free(rel);
        return out;
    }

    /* Path-relative: resolve '.'/'..' segments against the base directory */
    const char *basePath = base.path;
    const char *lastSlash = strrchr(basePath, '/');
    size_t dirLen = lastSlash ? (size_t)(lastSlash - basePath) + 1 : 1;

    /* combined = dir + rel */
    size_t combLen = dirLen + strlen(rel) + 2;
    char *combined = (char *)malloc(combLen);
    if (!combined)
    {
        url_free(rel);
        return NULL;
    }
    if (lastSlash)
    {
        memcpy(combined, basePath, dirLen);
        combined[dirLen] = '\0';
    }
    else
    {
        strcpy(combined, "/");
    }
    strcat(combined, rel);

    /* Normalize segments */
    char *resolvedPath = (char *)malloc(combLen + 2);
    if (!resolvedPath)
    {
        url_free(combined);
        url_free(rel);
        return NULL;
    }
    resolvedPath[0] = '\0';
    {
        /* token walk */
        char *segments[256];
        int nseg = 0;
        char *tok = strtok(combined, "/");
        while (tok)
        {
            if (strcmp(tok, "..") == 0)
            {
                if (nseg > 0)
                {
                    nseg--;
                }
            }
            else if (strcmp(tok, ".") != 0)
            {
                if (nseg < 256)
                {
                    segments[nseg++] = tok;
                }
            }
            tok = strtok(NULL, "/");
        }
        /* rebuild: "/a/b/c" */
        size_t w = 0;
        for (int i = 0; i < nseg; i++)
        {
            resolvedPath[w++] = '/';
            size_t slen = strlen(segments[i]);
            memcpy(resolvedPath + w, segments[i], slen);
            w += slen;
        }
        if (w == 0)
        {
            resolvedPath[w++] = '/';
        }
        resolvedPath[w] = '\0';
    }

    size_t need = strlen(base.scheme) + 3 + strlen(base.host) + strlen(portStr) +
                  strlen(resolvedPath) + 1;
    out = (char *)malloc(need);
    if (out)
    {
        snprintf(out, need, "%s://%s%s%s", base.scheme, base.host, portStr, resolvedPath);
    }

    url_free(resolvedPath);
    url_free(combined);
    url_free(rel);
    return out;
}

char *url_build_search_url(const char *searchEngineUrl, const char *queryText)
{
    char *enc = url_encode(queryText);
    if (!enc)
    {
        return NULL;
    }
    size_t need = strlen(searchEngineUrl) + strlen(enc) + 1;
    char *out = (char *)malloc(need);
    if (out)
    {
        snprintf(out, need, "%s%s", searchEngineUrl, enc);
    }
    url_free(enc);
    return out;
}

char *url_normalize_dup(const char *urlString)
{
    static UrlParsed p; /* hoisted: device gameTask stack is tiny */
    if (url_parse(urlString, &p) != 0)
    {
        return NULL;
    }
    return strutil_trim_dup(p.normalized);
}
