/*
 * PlutoBrowser — strbuf.h
 * Growable byte buffer (Phase 1).
 *
 * The Lua reference leans on `..` concatenation and table.concat; C needs an
 * explicit growable buffer to reproduce those patterns with the same results
 * (e.g. HTTP request building, HTML body accumulation, SVG serialization).
 */
#ifndef PLUTO_STRBUF_H
#define PLUTO_STRBUF_H

#include <stddef.h>

typedef struct StrBuf
{
    char *data;   /* NUL-terminated contents (always safe to read) */
    size_t len;   /* bytes used (excluding NUL)                    */
    size_t cap;   /* allocated bytes                               */
} StrBuf;

/* Initialize an empty buffer. Returns 0 on success, -1 on allocation failure. */
int strbuf_init(StrBuf *sb);

/* Free the buffer's storage; resets to empty state. */
void strbuf_free(StrBuf *sb);

/* Reset length to zero (keeps capacity). */
void strbuf_reset(StrBuf *sb);

/* Reserve capacity for at least `extra` more bytes. 0 ok, -1 alloc failure. */
int strbuf_reserve(StrBuf *sb, size_t extra);

/* Append a NUL-terminated string. 0 ok, -1 alloc failure. */
int strbuf_append(StrBuf *sb, const char *str);

/* Append `n` bytes (may contain NULs). 0 ok, -1 alloc failure. */
int strbuf_append_n(StrBuf *sb, const char *str, size_t n);

/* Append a single character. 0 ok, -1 alloc failure. */
int strbuf_append_char(StrBuf *sb, char c);

/* printf-style append. 0 ok, -1 alloc or formatting failure. */
int strbuf_appendf(StrBuf *sb, const char *fmt, ...);

/* Append `count` copies of `c`. 0 ok, -1 alloc failure. */
int strbuf_append_rep(StrBuf *sb, char c, size_t count);

/* Detach the contents: caller owns the returned NUL-terminated string;
 * buffer is reinitialized to empty. NULL on empty/failed buffer. */
char *strbuf_detach(StrBuf *sb);

#endif /* PLUTO_STRBUF_H */
