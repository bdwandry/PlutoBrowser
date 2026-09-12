/*
 * PlutoBrowser — strbuf.c
 * Growable byte buffer (Phase 1). See strbuf.h for the contract.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "util/strbuf.h"

/* Accessor implemented in main.c; lets this module use the SDK allocator. */
extern PlaydateAPI *pluto_pd(void);

/* Use the SDK allocator so memory is accounted by the Playdate runtime. */
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_REALLOC(p, n) pluto_pd()->system->realloc((p), (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

int strbuf_init(StrBuf *sb)
{
    sb->data = (char *)PLUTO_MALLOC(64);
    if (!sb->data)
    {
        sb->len = 0;
        sb->cap = 0;
        return -1;
    }
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = 64;
    return 0;
}

void strbuf_free(StrBuf *sb)
{
    if (sb->data)
    {
        PLUTO_FREE(sb->data);
        sb->data = NULL;
    }
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_reset(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data)
    {
        sb->data[0] = '\0';
    }
}

int strbuf_reserve(StrBuf *sb, size_t extra)
{
    size_t needed = sb->len + extra + 1; /* +1 for NUL */
    if (needed <= sb->cap)
    {
        return 0;
    }
    size_t newCap = sb->cap ? sb->cap : 64;
    while (newCap < needed)
    {
        newCap *= 2;
    }
    char *nd = (char *)PLUTO_REALLOC(sb->data, newCap);
    if (!nd)
    {
        return -1;
    }
    sb->data = nd;
    sb->cap = newCap;
    return 0;
}

int strbuf_append_n(StrBuf *sb, const char *str, size_t n)
{
    if (n == 0)
    {
        return 0;
    }
    if (strbuf_reserve(sb, n) != 0)
    {
        return -1;
    }
    memcpy(sb->data + sb->len, str, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int strbuf_append(StrBuf *sb, const char *str)
{
    if (!str)
    {
        return 0;
    }
    return strbuf_append_n(sb, str, strlen(str));
}

int strbuf_append_char(StrBuf *sb, char c)
{
    return strbuf_append_n(sb, &c, 1);
}

int strbuf_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int need = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (need < 0)
    {
        va_end(argsCopy);
        return -1;
    }
    if (strbuf_reserve(sb, (size_t)need) != 0)
    {
        va_end(argsCopy);
        return -1;
    }
    vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, argsCopy);
    va_end(argsCopy);
    sb->len += (size_t)need;
    return 0;
}

int strbuf_append_rep(StrBuf *sb, char c, size_t count)
{
    if (count == 0)
    {
        return 0;
    }
    if (strbuf_reserve(sb, count) != 0)
    {
        return -1;
    }
    memset(sb->data + sb->len, c, count);
    sb->len += count;
    sb->data[sb->len] = '\0';
    return 0;
}

char *strbuf_detach(StrBuf *sb)
{
    char *out = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}
