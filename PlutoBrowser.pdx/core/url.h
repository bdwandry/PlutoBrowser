// url.h — URL parser, normalizer, resolver (C port of core/url.lua).
//
// Behavior parity notes (verified against the Lua original):
//  - parse("") yields the internal about:blank record; whitespace-only
//    strings are NOT empty and parse to scheme=https, host="".
//  - Hash (#) is split BEFORE query (?), each taking the first occurrence.
//  - Custom ports go through Lua-strict numeric conversion ("8080x" -> nil),
//    must be > 0, otherwise the 80/443 default stands. Micro-deviation:
//    fractional ports ("8.5") are treated as invalid rather than accepted.
//  - Userinfo ("user:pass@host") is not understood: the colon rule splits
//    host at the first ':' — parity preserved, quirks included.
//  - resolve() specials are exactly about:, data:, javascript:, scheme://,
    // protocol-relative "//", "#...", "?...", "/..." and dot-segment paths.
//    mailto:, tel:, etc. fall through as path-relative text.

#ifndef PLUTO_URL_H
#define PLUTO_URL_H

#include <stddef.h>

#include "../util/strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_URL_RAW_MAX        512
#define PLUTO_URL_SCHEME_MAX     32
#define PLUTO_URL_HOST_MAX       256
#define PLUTO_URL_PATH_MAX       512
#define PLUTO_URL_QUERY_MAX      512
#define PLUTO_URL_HASH_MAX       256
#define PLUTO_URL_FULLPATH_MAX   1024
#define PLUTO_URL_NORMALIZED_MAX 1536
#define PLUTO_URL_INPUT_MAX      512

typedef struct {
    char raw[PLUTO_URL_RAW_MAX];
    char normalized[PLUTO_URL_NORMALIZED_MAX];
    char scheme[PLUTO_URL_SCHEME_MAX];
    char host[PLUTO_URL_HOST_MAX];
    int  port;
    char path[PLUTO_URL_PATH_MAX];
    char query[PLUTO_URL_QUERY_MAX];
    char hash[PLUTO_URL_HASH_MAX];
    char fullPath[PLUTO_URL_FULLPATH_MAX];
    int  isSsl;
} PlutoUrl;

// Form-encode: bytes outside [A-Za-z0-9 ' ' - _ . ~] become %XX (uppercase);
// \n becomes %0D%0A; finally ' ' becomes '+'. Appends to out.
void url_encode(const char* str, StrBuf* out);

// Form-decode: '+' -> ' ', %XX pairs decoded (invalid escapes stay literal).
void url_decode(const char* str, StrBuf* out);

// Mirrors URL.isSearchQuery decision order exactly. Returns 1/0.
int  url_is_search_query(const char* input);

// Always produces a valid record (empty input -> about:blank). Truncates
// oversized components defensively.
void url_parse(const char* urlString, PlutoUrl* out);

// DuckDuckGo /l/?uddg=... unwrapper. Returns 1 and fills out with the
// decoded target when unwrapped; returns 0 leaving out untouched.
int  url_unwrap_redirect(const char* urlString, StrBuf* out);

// Resolve relative against base; appends absolute result to out. Empty rel
// yields baseUrlStr verbatim.
void url_resolve(const char* baseUrlStr, const char* relativeUrlStr,
                 StrBuf* out);

// engineUrlTemplate .. formEncode(queryText)
void url_build_search_url(const char* engineUrl, const char* queryText,
                          StrBuf* out);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_URL_H
