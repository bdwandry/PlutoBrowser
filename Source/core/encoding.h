/*
 * PlutoBrowser — encoding.h
 * Character encoding detection & conversion (port of Source/core/encoding.lua).
 *
 * HTML-spec precedence, preserved exactly from the Lua reference:
 *   1. BOM (UTF-8 / UTF-16LE / UTF-16BE)
 *   2. Transport charset from the HTTP Content-Type header
 *   3. <meta charset="..."> in the first 1024 bytes
 *   4. <meta http-equiv="Content-Type" content="...; charset=...">
 *   5. Default: windows-1252 (practical web default)
 *
 * All returned strings are heap-allocated via the Playdate realloc API;
 * free them with pluto_free().
 */
#ifndef PLUTO_ENCODING_H
#define PLUTO_ENCODING_H

#include <stddef.h>

/*
 * Detect the document encoding and return a heap-allocated UTF-8 normalized
 * copy of `data` (len bytes). `contentType` may be NULL. For an empty input
 * the function returns a heap copy of the input. Returns NULL only on
 * allocation failure.
 */
char *encoding_to_utf8(const unsigned char *data, size_t len, const char *contentType);

/* Heuristics exposed for testing (mirror the Lua locals). */
int encoding_is_utf8(const unsigned char *data, size_t len);

#endif /* PLUTO_ENCODING_H */
