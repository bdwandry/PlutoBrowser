// strbuf.c — growable byte buffer implementation.

#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

void sb_init(StrBuf* sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf* sb)
{
    if (sb->data != NULL) {
        pluto_free(sb->data);
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_clear(StrBuf* sb)
{
    sb->len = 0; // keep capacity for reuse
    if (sb->data != NULL) sb->data[0] = '\0';
}

int sb_reserve(StrBuf* sb, size_t extra)
{
    size_t need, newCap;
    char* nd;

    if (extra == 0) {
        return (sb->data != NULL || sb->cap == 0) ? 1 : 1;
    }
    if (sb->cap - sb->len >= extra) {
        return 1;
    }
    need = sb->len + extra;
    newCap = (sb->cap == 0) ? 64 : sb->cap;
    while (newCap < need) {
        newCap *= 2;
    }
    nd = (char*)pluto_realloc(sb->data, newCap);
    if (nd == NULL) {
        return 0;
    }
    sb->data = nd;
    sb->cap = newCap;
    return 1;
}

int sb_append(StrBuf* sb, const void* bytes, size_t n)
{
    if (n == 0) {
        return 1;
    }
    if (!sb_reserve(sb, n + 1)) {
        return 0;
    }
    memcpy(sb->data + sb->len, bytes, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 1;
}

int sb_append_str(StrBuf* sb, const char* cstr)
{
    if (cstr == NULL) {
        return 1;
    }
    return sb_append(sb, cstr, strlen(cstr));
}

int sb_append_char(StrBuf* sb, char c)
{
    return sb_append(sb, &c, 1);
}

int sb_append_buf(StrBuf* sb, const StrBuf* other)
{
    if (other == NULL || other == sb) {
        return other == NULL ? 1 : 0;
    }
    return sb_append(sb, other->data, other->len);
}

int sb_printf(StrBuf* sb, const char* fmt, ...)
{
    char stack[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return 0;
    }
    if ((size_t)n < sizeof(stack)) {
        return sb_append(sb, stack, (size_t)n);
    }
    {
        char* heap = (char*)pluto_malloc((size_t)n + 1);
        int ok;
        if (heap == NULL) {
            return 0;
        }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        ok = sb_append(sb, heap, (size_t)n);
        pluto_free(heap);
        return ok;
    }
}

char* sb_detach(StrBuf* sb)
{
    char* out;
    if (!sb_reserve(sb, 1)) {
        return NULL;
    }
    sb->data[sb->len] = '\0';
    out = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}
