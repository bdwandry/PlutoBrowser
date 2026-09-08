/*
 * PlutoBrowser — url.h
 * URL parser, normalizer and resolver (port of Source/core/url.lua).
 */
#ifndef PLUTO_URL_H
#define PLUTO_URL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed URL components (URL.parse result table in Lua).
 * SIZED FOR THE PLAYDATE STACK: the game task stack is small — a prior
 * revision used ~4KB here and overflowed the device stack when nested
 * (device errorlog: "stack overflow in task gameTask"). Keep this struct
 * lean; long URLs are truncated exactly as the Lua implementation's practical
 * limits allowed. */
typedef struct
{
    char raw[128];         /* trimmed input                      */
    char normalized[512];  /* scheme://host[:port]fullPath       */
    char scheme[12];       /* lowercased                         */
    char host[128];        /* lowercased; "blank" for empty      */
    int  port;             /* 80/443 default or explicit         */
    char path[256];        /* pathPart, "/" if empty             */
    char query[256];       /* after '?' (no '?')                 */
    char hash[128];        /* after '#' (no '#')                 */
    char fullPath[512];    /* path[?query][#hash]                */
    int  isSsl;            /* scheme == https                    */
} UrlParsed;  /* ~1.7KB — safe on the game task stack, one per frame */

/* URL.encode: application/x-www-form-urlencoded (' '→'+', percent-escape).
 * Returns malloc'd (SDK) string; caller frees via pluto_free(). Never NULL. */
char *url_encode(const char *str);

/* URL.decode: '+'→' ' and %XX unescaping. malloc'd; caller frees. */
char *url_decode(const char *str);

/* URL.isSearchQuery heuristic. 1 = treat as search query. */
int url_is_search_query(const char *input);

/* URL.parse into `out`. Returns 0 on success. Handles empty → about:blank,
 * about: pages, scheme defaults, hash/query splitting, host[:port]. */
int url_parse(const char *urlString, UrlParsed *out);

/* URL.unwrapRedirect: DuckDuckGo /l/?uddg=... → real target.
 * Returns malloc'd string (caller frees) or NULL when nothing to unwrap. */
char *url_unwrap_redirect(const char *urlString);

/* URL.resolve: resolve `relative` against `baseUrlStr`.
 * Returns malloc'd string; caller frees. NULL on allocation failure. */
char *url_resolve(const char *baseUrlStr, const char *relativeUrlStr);

/* URL.buildSearchUrl: engineUrl + encode(queryText). malloc'd; caller frees. */
char *url_build_search_url(const char *searchEngineUrl, const char *queryText);

/* Convenience: parse and return malloc'd normalized URL (or NULL). */
char *url_normalize_dup(const char *urlString);

#ifdef __cplusplus
}
#endif

#endif /* PLUTO_URL_H */
