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
    /* Iterative post-order free (explicit stack, heap-allocated). The
     * recursive version spent one C frame per nesting level; a hostile or
     * deeply-nested payload (a fetched JSON response is untrusted page
     * data) could overflow the device's small game-task stack. Frees each
     * node after its children, same ownership as before. */
    typedef struct
    {
        JsonValue *v;
        int nextChild;
    } JsonFrame;
    if (!v)
    {
        return;
    }
    int cap = 32;
    int top = 0;
    JsonFrame *st = (JsonFrame *)malloc(sizeof(JsonFrame) * (size_t)cap);
    if (!st)
    {
        return; /* nothing we can do; allocator is out */
    }
    st[top].v = v;
    st[top].nextChild = 0;
    top++;
    while (top > 0)
    {
        JsonFrame *f = &st[top - 1];
        JsonValue *n = f->v;
        if (f->nextChild < (int)n->count)
        {
            JsonValue *c = n->items[f->nextChild++];
            if (!c)
            {
                continue;
            }
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                JsonFrame *grown = (JsonFrame *)realloc(st, sizeof(JsonFrame) * (size_t)ncap);
                if (!grown)
                {
                    free(st);
                    return;
                }
                st = grown;
                cap = ncap;
                f = &st[top - 1]; /* realloc may have moved the array */
            }
            st[top].v = c;
            st[top].nextChild = 0;
            top++;
        }
        else
        {
            /* children done: free this node's own storage */
            free(n->string);
            if (n->keys)
            {
                for (size_t i = 0; i < n->count; i++)
                {
                    free(n->keys[i]);
                }
            }
            free(n->items);
            free(n->keys);
            free(n);
            top--;
        }
    }
    free(st);
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

/* ── Parser (iterative) ────────────────────────────────────────────────────
 * The recursive-descent trio (parse_value / parse_object / parse_array)
 * became ONE loop over an explicit frame stack: each frame is a container
 * (object or array) being filled. Behavior is identical — including the
 * strict rejection of `[1,]` / `{"a":1,}` (a ',' is followed by a real
 * member, never a close token) and JSON_MAX_DEPTH enforcement per value.
 *
 * Loop states, folded into two steps per iteration:
 *   1. ATTACH: a produced value joins the top frame's container (or the
 *      empty stack means it is the root — done).
 *   2. ADVANCE: the top frame consumes ',' / close / (objects) the next
 *      key + ':'. A container value pushes a new frame.
 * A heap-grown frame stack replaces the C recursion; depth is still capped
 * by JSON_MAX_DEPTH, so the fixed-bound contract in json.h holds.
 */

static JsonValue *parse_value(Parser *ps)
{
    typedef struct
    {
        JsonValue *container; /* object/array being filled (owned here) */
        char *key;            /* object: key awaiting its value (owned) */
    } PFrame;

    PFrame *st = (PFrame *)malloc(sizeof(PFrame) * 16);
    int cap = 16;
    int top = 0;
    JsonValue *delivered = NULL; /* value produced, awaiting attach */
    JsonValue *root = NULL;

    if (!st)
    {
        return NULL;
    }

    for (;;)
    {
        /* ── 1. Attach a produced value ── */
        if (delivered)
        {
            if (top == 0)
            {
                root = delivered; /* the single top-level value: done */
                delivered = NULL;
                break;
            }
            PFrame *f = &st[top - 1];
            if (f->container->type == JSON_OBJECT)
            {
                if (!container_add(f->container, f->key, delivered))
                {
                    free(f->key);
                    f->key = NULL;
                    json_free(delivered);
                    delivered = NULL;
                    goto fail;
                }
                f->key = NULL; /* ownership moved into the container */
            }
            else
            {
                if (!container_add(f->container, NULL, delivered))
                {
                    json_free(delivered);
                    delivered = NULL;
                    goto fail;
                }
            }
            delivered = NULL;
            /* fall through: the container now decides ',' or close */
        }

        /* ── 2. Advance the top container ── */
        if (top > 0)
        {
            PFrame *f = &st[top - 1];
            skip_ws(ps);
            if (ps->p >= ps->end)
            {
                goto fail; /* truncated member list */
            }
            int afterComma = 0;
            if (*ps->p == ',')
            {
                ps->p++;
                afterComma = 1;
                skip_ws(ps);
                if (ps->p >= ps->end)
                {
                    goto fail;
                }
            }
            if (f->container->type == JSON_OBJECT)
            {
                if (!afterComma && *ps->p == '}')
                {
                    ps->p++;
                    ps->depth--;
                    delivered = f->container; /* container complete → pop */
                    top--;
                    continue;
                }
                /* JSON requires ',' between members: once the object holds
                 * at least one member, the next token must be ',' (consumed
                 * above) or '}' (handled). A bare '"' without a comma is
                 * the {"a":1 "b":2} error the old recursive parser
                 * rejected. */
                if (f->container->count > 0 && !afterComma)
                {
                    goto fail;
                }
                /* member: "key" : value (a bare '}' after ',' is invalid,
                 * same as the recursive parser's string-raw failure). */
                if (*ps->p != '"')
                {
                    goto fail;
                }
                char *key = parse_string_raw(ps);
                if (!key)
                {
                    goto fail;
                }
                skip_ws(ps);
                if (ps->p >= ps->end || *ps->p != ':')
                {
                    free(key);
                    goto fail;
                }
                ps->p++;
                f->key = key;
                /* fall through: parse the value */
            }
            else
            {
                if (!afterComma && *ps->p == ']')
                {
                    ps->p++;
                    ps->depth--;
                    delivered = f->container;
                    top--;
                    continue;
                }
                /* JSON requires ',' between elements (see object branch). */
                if (f->container->count > 0 && !afterComma)
                {
                    goto fail;
                }
                /* fall through: parse the value */
            }
        }

        /* ── 3. Parse one value at the cursor ── */
        skip_ws(ps);
        if (ps->p >= ps->end)
        {
            goto fail;
        }
        if (++ps->depth > JSON_MAX_DEPTH)
        {
            ps->depth--;
            goto fail;
        }
        char c = *ps->p;
        JsonValue *v = NULL;
        int isContainer = 0;
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
            ps->p++;
            v = value_new(JSON_OBJECT);
            isContainer = 1;
        }
        else if (c == '[')
        {
            ps->p++;
            v = value_new(JSON_ARRAY);
            isContainer = 1;
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

        if (!v)
        {
            goto fail;
        }

        if (isContainer)
        {
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                PFrame *grown = (PFrame *)realloc(st, sizeof(PFrame) * (size_t)ncap);
                if (!grown)
                {
                    json_free(v);
                    goto fail;
                }
                st = grown;
                cap = ncap;
            }
            st[top].container = v;
            st[top].key = NULL;
            top++;
            continue; /* empty-member check runs at step 2 */
        }
        delivered = v; /* scalar: attach at step 1 next iteration */
    }

    free(st);
    return root;

fail:
    if (delivered)
    {
        json_free(delivered);
    }
    for (int i = 0; i < top; i++)
    {
        if (st[i].key)
        {
            free(st[i].key);
        }
        json_free(st[i].container);
    }
    free(st);
    return NULL;
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
