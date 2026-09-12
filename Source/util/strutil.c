/*
 * PlutoBrowser — strutil.c
 * String helpers (Phase 1). See strutil.h for the contract.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "util/strutil.h"

/* Accessor implemented in main.c; lets this module use the SDK allocator. */
extern PlaydateAPI *pluto_pd(void);

#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

int strutil_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char *strutil_trim_dup(const char *s)
{
    if (!s)
    {
        s = "";
    }
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && strutil_is_space(s[start]))
    {
        start++;
    }
    size_t end = n;
    while (end > start && strutil_is_space(s[end - 1]))
    {
        end--;
    }
    size_t len = end - start;
    char *out = (char *)PLUTO_MALLOC(len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, s + start, len);
    out[len] = '\0';
    return out;
}

char *strutil_lower_dup(const char *s)
{
    if (!s)
    {
        s = "";
    }
    size_t n = strlen(s);
    char *out = (char *)PLUTO_MALLOC(n + 1);
    if (!out)
    {
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[n] = '\0';
    return out;
}

char *strutil_upper_dup(const char *s)
{
    if (!s)
    {
        s = "";
    }
    size_t n = strlen(s);
    char *out = (char *)PLUTO_MALLOC(n + 1);
    if (!out)
    {
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = (char)toupper((unsigned char)s[i]);
    }
    out[n] = '\0';
    return out;
}

int strutil_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
    {
        return 0;
    }
    size_t pl = strlen(prefix);
    return strncmp(s, prefix, pl) == 0;
}

int strutil_istarts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
    {
        return 0;
    }
    size_t i = 0;
    while (prefix[i] != '\0')
    {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
        {
            return 0;
        }
        i++;
    }
    return 1;
}

int strutil_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix)
    {
        return 0;
    }
    size_t sl = strlen(s);
    size_t fl = strlen(suffix);
    if (fl > sl)
    {
        return 0;
    }
    return strcmp(s + sl - fl, suffix) == 0;
}

ptrdiff_t strutil_find(const char *s, const char *needle, size_t from)
{
    if (!s || !needle)
    {
        return -1;
    }
    size_t n = strlen(s);
    if (from > n)
    {
        return -1;
    }
    const char *hit = strstr(s + from, needle);
    if (!hit)
    {
        return -1;
    }
    return (ptrdiff_t)(hit - s);
}

ptrdiff_t strutil_find_any(const char *s, const char *set, size_t from)
{
    if (!s || !set)
    {
        return -1;
    }
    size_t n = strlen(s);
    for (size_t i = from; i < n; i++)
    {
        if (strchr(set, s[i]))
        {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

ptrdiff_t strutil_find_not_any(const char *s, const char *set, size_t from)
{
    if (!s)
    {
        return -1;
    }
    size_t n = strlen(s);
    for (size_t i = from; i < n; i++)
    {
        if (!set || !strchr(set, s[i]))
        {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

char *strutil_collapse_ws_dup(const char *s)
{
    if (!s)
    {
        s = "";
    }
    size_t n = strlen(s);
    char *out = (char *)PLUTO_MALLOC(n + 1);
    if (!out)
    {
        return NULL;
    }
    size_t o = 0;
    int inRun = 0;
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (c == '\r' || c == '\n' || c == '\t')
        {
            if (!inRun)
            {
                out[o++] = ' ';
                inRun = 1;
            }
        }
        else
        {
            out[o++] = c;
            inRun = 0;
        }
    }
    out[o] = '\0';
    return out;
}

char *strutil_replace_dup(const char *s, const char *pat, const char *rep)
{
    if (!s || !pat || !*pat)
    {
        return strutil_trim_dup(s ? s : "");
    }
    if (!rep)
    {
        rep = "";
    }
    size_t patLen = strlen(pat);
    size_t repLen = strlen(rep);

    /* Count occurrences to size the output in one allocation. */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, pat)) != NULL)
    {
        count++;
        p += patLen;
    }

    size_t sl = strlen(s);
    size_t outLen = sl + count * repLen - count * patLen;
    char *out = (char *)PLUTO_MALLOC(outLen + 1);
    if (!out)
    {
        return NULL;
    }
    char *o = out;
    p = s;
    const char *hit;
    while ((hit = strstr(p, pat)) != NULL)
    {
        size_t chunk = (size_t)(hit - p);
        memcpy(o, p, chunk);
        o += chunk;
        memcpy(o, rep, repLen);
        o += repLen;
        p = hit + patLen;
    }
    strcpy(o, p);
    return out;
}
