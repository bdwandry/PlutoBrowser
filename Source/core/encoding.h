// encoding.h — charset detection & conversion (C port of core/encoding.lua).
//
// Phase P06. Precedence mirrors the HTML-spec order used by the source:
//   1. BOM (UTF-8 strip / UTF-16LE / UTF-16BE decode)
//   2. Content-Type header charset (unless quoted/unparseable -> nil)
//   3. <meta charset=...> / <meta http-equiv content=...> within first 1024
//      bytes — scanned ONLY when header charset is nil OR "utf-8"
//   4. windows-1252 best-effort for shift_jis/euc-jp/gbk/big5
//   5. anything undecodable passes through unchanged
//
// Ownership: every returned string is a fresh pluto_malloc buffer (NUL-
// terminated); outLen receives the byte length (Lua strings may hold any
// bytes). Caller frees.
#ifndef PLUTO_ENCODING_H
#define PLUTO_ENCODING_H

#include <stddef.h>

struct PlaydateAPI;

void encoding_init(struct PlaydateAPI* pd);

// Encoding.toUtf8(data, contentType). contentType may be NULL.
char* encoding_to_utf8(const char* data, size_t len,
                       const char* contentType, size_t* outLen);

// normalizeCharset(name): strip non-alphanumerics, lowercase, alias map.
// Returns malloc'd token or NULL for NULL input.
char* encoding_normalize_charset(const char* name);

// charsetFromHeader(contentType): lowercased scan for charset=<token>.
// Quoted values do NOT match (Lua pattern parity). malloc'd or NULL.
char* encoding_charset_from_header(const char* contentType);

// scanMetaCharset(data, limit): first `limit` bytes (default 1024),
// two <meta> patterns. malloc'd or NULL.
char* encoding_scan_meta_charset(const char* data, size_t len);

#endif // PLUTO_ENCODING_H
