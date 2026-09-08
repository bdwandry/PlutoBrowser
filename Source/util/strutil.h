/*
 * PlutoBrowser — strutil.h
 * String helpers replacing Lua's pattern-based helpers (Phase 1).
 *
 * The Lua reference uses `string.gsub(s, "^%s*(.-)%s*$", "%1")` (trim),
 * `string.lower`, `string.find` with plain text, `string.match` prefixes etc.
 * These helpers reproduce that behavior for C code.
 */
#ifndef PLUTO_STRUTIL_H
#define PLUTO_STRUTIL_H

#include <stddef.h>

/* Trim leading/trailing whitespace into a newly allocated string. Caller frees. */
char *strutil_trim_dup(const char *s);

/* Lowercase a string into a newly allocated buffer. Caller frees. */
char *strutil_lower_dup(const char *s);

/* Uppercase a string into a newly allocated buffer. Caller frees. */
char *strutil_upper_dup(const char *s);

/* Case-insensitive prefix test. */
int strutil_istarts_with(const char *s, const char *prefix);

/* Case-sensitive prefix test. */
int strutil_starts_with(const char *s, const char *prefix);

/* Case-sensitive suffix test. */
int strutil_ends_with(const char *s, const char *suffix);

/* Find first occurrence of `needle` in `s` starting at `from` (byte index).
 * Returns index or -1. Plain-text search (Lua string.find plain mode). */
ptrdiff_t strutil_find(const char *s, const char *needle, size_t from);

/* Find first occurrence of any character in `set` from `from`. Index or -1. */
ptrdiff_t strutil_find_any(const char *s, const char *set, size_t from);

/* Find first character NOT in `set` from `from`. Index or -1. */
ptrdiff_t strutil_find_not_any(const char *s, const char *set, size_t from);

/* True when the byte at index i is whitespace (space, \t, \r, \n, \f, \v). */
int strutil_is_space(char c);

/* Collapsed-copy: replace every run [\r\n\t]+ with a single space (Lua
 * document walker does `gsub(text, "[\r\n\t]+", " ")`). Caller frees. */
char *strutil_collapse_ws_dup(const char *s);

/* Duplicate with all occurrences of `pat` replaced by `rep` (plain text).
 * Caller frees. NULL if out of memory. */
char *strutil_replace_dup(const char *s, const char *pat, const char *rep);

#endif /* PLUTO_STRUTIL_H */
