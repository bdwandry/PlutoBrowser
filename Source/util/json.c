/*
 * PlutoBrowser — json.c
 * See json.h. Recursive-descent parser over a NUL-terminated string.
 * Depth is bounded by JSON_MAX_DEPTH to keep stack use fixed (device safety);
 * deeper nesting reports an error, exactly like the Lua decoder's limits.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "util/json.h"

#define JSON_MAX_DEPTH 64

typedef struct
{
    const char *p;
    const char *end;
    int depth;
} Parser;

static JsonValue *parse_value(Parser *ps);

static JsonValue *value_new(JsonType t)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v)
    {
        v->type = t;
    }
    return v;
}

void json_free(JsonValue *v)
{
    if (!v)
    {
        return;
    }
    free(v->string);
    for (size_t i = 0; i < v->count; i++)
    {
        json_free(v->items[i]);
        if (v->keys)
        {
            free(v->keys[i]);
        }
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end)
    {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            ps->p++;
        }
        else
        {
            break;
        }
    }
}

static int read_hex4(Parser *ps, unsigned *out)
{
    unsigned val = 0;
    for (int i = 0; i < 4; i++)
    {
        if (ps->p >= ps->end)
        {
            return 0;
        }
        char c = *ps->p++;
        val <<= 4;
        if (c >= '0' && c <= '9')
        {
            val |= (unsigned)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            val |= (unsigned)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            val |= (unsigned)(c - 'A' + 10);
        }
        else
        {
            return 0;
        }
    }
    *out = val;
    return 1;
}

/* Appends the UTF-8 encoding of cp to buf. */
static size_t utf8_encode(unsigned cp, char *buf)
{
    if (cp < 0x80)
    {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

typedef struct
{
    char *data;
    size_t len, cap;
} StrBuf;

static int sb_put(StrBuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap)
    {
        size_t nc = sb->cap ? sb->cap * 2 : 32;
        while (nc < sb->len + n + 1)
        {
            nc *= 2;
        }
        char *nd = (char *)realloc(sb->data, nc);
        if (!nd)
        {
            return 0;
        }
        sb->data = nd;
        sb->cap = nc;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 1;
}

static int sb_put_utf8(StrBuf *sb, unsigned cp)
{
    char tmp[4];
    size_t n = utf8_encode(cp, tmp);
    return sb_put(sb, tmp, n);
}

/* Parses an escape sequence (the backslash is already consumed). */
static int parse_escape(Parser *ps, StrBuf *sb)
{
    if (ps->p >= ps->end)
    {
        return 0;
    }
    char c = *ps->p++;
    switch (c)
    {
    case '"':
        return sb_put(sb, "\"", 1);
    case '\\':
        return sb_put(sb, "\\", 1);
    case '/':
        return sb_put(sb, "/", 1);
    case 'b':
        return sb_put(sb, "\b", 1);
    case 'f':
        return sb_put(sb, "\f", 1);
    case 'n':
        return sb_put(sb, "\n", 1);
    case 'r':
        return sb_put(sb, "\r", 1);
    case 't':
        return sb_put(sb, "\t", 1);
    case 'u':
    {
        unsigned cp;
        if (!read_hex4(ps, &cp))
        {
            return 0;
        }
        /* Surrogate pair handling (RFC 8259 §7.2). */
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
            {
                return 0;
            }
            ps->p += 2;
            unsigned lo;
            if (!read_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF)
            {
                return 0;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            return 0; /* lone low surrogate */
        }
        return sb_put_utf8(sb, cp);
    }
    default:
        return 0;
    }
}

static char *parse_string_raw(Parser *ps)
{
    if (ps->p >= ps->end || *ps->p != '"')
    {
        return NULL;
    }
    ps->p++;
    StrBuf sb = {0};
    while (ps->p < ps->end)
    {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"')
        {
            ps->p++;
            if (!sb.data && sb.len == 0)
            {
                /* Empty string still needs a valid heap pointer. */
                sb.data = (char *)malloc(1);
                if (sb.data)
                {
                    sb.data[0] = '\0';
                }
            }
            return sb.data; /* ownership passes to caller; NULL on OOM */
        }
        if (c == '\\')
        {
            ps->p++;
            if (!parse_escape(ps, &sb))
            {
                goto fail;
            }
        }
        else if (c < 0x20)
        {
            goto fail; /* raw control characters are invalid JSON */
        }
        else
        {
            ps->p++;
            if (!sb_put(&sb, (const char *)&c, 1))
            {
                goto fail;
            }
        }
    }
fail:
    free(sb.data);
    return NULL;
}

static JsonValue *parse_number(Parser *ps)
{
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-')
    {
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
    {
        return NULL;
    }
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
    {
        ps->p++;
    }
    if (ps->p < ps->end && *ps->p == '.')
    {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
        {
            return NULL;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
        {
            ps->p++;
        }
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
    {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
        {
            ps->p++;
        }
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
        {
            return NULL;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
        {
            ps->p++;
        }
    }
    JsonValue *v = value_new(JSON_NUMBER);
    if (!v)
    {
        return NULL;
    }
    v->number = strtod(start, NULL);
    return v;
}

static int container_add(JsonValue *v, char *key, JsonValue *item)
{
    if (v->count == v->cap)
    {
        size_t nc = v->cap ? v->cap * 2 : 4;
        JsonValue **ni = (JsonValue **)realloc(v->items, nc * sizeof(*ni));
        if (!ni)
        {
            return 0;
        }
        v->items = ni;
        char **nk = (char **)realloc(v->keys, nc * sizeof(*nk));
        if (!nk)
        {
            v->items = (JsonValue **)realloc(v->items, v->cap * sizeof(*ni));
            return 0;
        }
        v->keys = nk;
        v->cap = nc;
    }
    v->items[v->count] = item;
    v->keys[v->count] = key; /* arrays keep NULL keys */
    v->count++;
    return 1;
}

static JsonValue *parse_object(Parser *ps)
{
    ps->p++; /* '{' */
    JsonValue *v = value_new(JSON_OBJECT);
    if (!v)
    {
        return NULL;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}')
    {
        ps->p++;
        return v;
    }
    for (;;)
    {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key)
        {
            json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':')
        {
            free(key);
            json_free(v);
            return NULL;
        }
        ps->p++;
        skip_ws(ps);
        JsonValue *item = parse_value(ps);
        if (!item)
        {
            free(key);
            json_free(v);
            return NULL;
        }
        if (!container_add(v, key, item))
        {
            free(key);
            json_free(item);
            json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',')
        {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}')
        {
            ps->p++;
            return v;
        }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_array(Parser *ps)
{
    ps->p++; /* '[' */
    JsonValue *v = value_new(JSON_ARRAY);
    if (!v)
    {
        return NULL;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']')
    {
        ps->p++;
        return v;
    }
    for (;;)
    {
        skip_ws(ps);
        JsonValue *item = parse_value(ps);
        if (!item)
        {
            json_free(v);
            return NULL;
        }
        if (!container_add(v, NULL, item))
        {
            json_free(item);
            json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',')
        {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']')
        {
            ps->p++;
            return v;
        }
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_value(Parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end)
    {
        return NULL;
    }
    if (++ps->depth > JSON_MAX_DEPTH)
    {
        ps->depth--;
        return NULL;
    }
    JsonValue *v = NULL;
    char c = *ps->p;
    if (c == '"')
    {
        char *s = parse_string_raw(ps);
        if (s)
        {
            v = value_new(JSON_STRING);
            if (v)
            {
                v->string = s;
            }
            else
            {
                free(s);
            }
        }
    }
    else if (c == '{')
    {
        v = parse_object(ps);
    }
    else if (c == '[')
    {
        v = parse_array(ps);
    }
    else if (c == 't')
    {
        if (ps->end - ps->p >= 4 && strncmp(ps->p, "true", 4) == 0)
        {
            ps->p += 4;
            v = value_new(JSON_BOOL);
            if (v)
            {
                v->boolean = 1;
            }
        }
    }
    else if (c == 'f')
    {
        if (ps->end - ps->p >= 5 && strncmp(ps->p, "false", 5) == 0)
        {
            ps->p += 5;
            v = value_new(JSON_BOOL);
        }
    }
    else if (c == 'n')
    {
        if (ps->end - ps->p >= 4 && strncmp(ps->p, "null", 4) == 0)
        {
            ps->p += 4;
            v = value_new(JSON_NULL);
        }
    }
    else
    {
        v = parse_number(ps);
    }
    ps->depth--;
    return v;
}

JsonValue *json_decode(const char *text)
{
    if (!text)
    {
        return NULL;
    }
    Parser ps = {text, text + strlen(text), 0};
    JsonValue *v = parse_value(&ps);
    if (!v)
    {
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end)
    {
        json_free(v);
        return NULL; /* trailing garbage */
    }
    return v;
}

JsonValue *json_get(const JsonValue *v, const char *key)
{
    if (!v || v->type != JSON_OBJECT || !key)
    {
        return NULL;
    }
    for (size_t i = 0; i < v->count; i++)
    {
        if (v->keys[i] && strcmp(v->keys[i], key) == 0)
        {
            return v->items[i];
        }
    }
    return NULL;
}

int json_as_string(const JsonValue *v, const char **out)
{
    if (!v || v->type != JSON_STRING || !v->string)
    {
        return 0;
    }
    *out = v->string;
    return 1;
}

int json_as_number(const JsonValue *v, double *out)
{
    if (!v || v->type != JSON_NUMBER)
    {
        return 0;
    }
    *out = v->number;
    return 1;
}
