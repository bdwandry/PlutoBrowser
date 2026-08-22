// strbuf.h — growable byte buffer ("string builder").
//
// Replaces Lua's immutable-string concatenation. Buffers are byte strings:
// they may contain embedded NULs; sb_detach() NUL-terminates for C-string use.

#ifndef PLUTO_STRBUF_H
#define PLUTO_STRBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} StrBuf;

void  sb_init(StrBuf* sb);
void  sb_free(StrBuf* sb);
void  sb_clear(StrBuf* sb);

// Reserve room for `extra` more bytes beyond len. Returns 0 on OOM.
int   sb_reserve(StrBuf* sb, size_t extra);

// All append functions return 0 on OOM, 1 on success.
int   sb_append(StrBuf* sb, const void* bytes, size_t n);
int   sb_append_str(StrBuf* sb, const char* cstr);
int   sb_append_char(StrBuf* sb, char c);
int   sb_append_buf(StrBuf* sb, const StrBuf* other); // may alias? no — must differ
int   sb_printf(StrBuf* sb, const char* fmt, ...);

// NUL-terminate and hand ownership of the heap block to the caller.
// The StrBuf is reset to empty. Caller frees with pluto_free().
char* sb_detach(StrBuf* sb);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_STRBUF_H
