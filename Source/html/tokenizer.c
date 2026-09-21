/*
 * PlutoBrowser — tokenizer.c
 * Single-pass HTML tokenizer (port of Source/html/tokenizer.lua).
 *
 * Every branch of the Lua reference is reproduced, including quirks that are
 * part of observable behavior (verified against the actual reference):
 *   - attribute value search starts at ke+1 (right after the key): with a
 *     spaced '=' ("z = \"b\"") the search lands ON '=', which is excluded
 *     from the unquoted-value charset, so the value is "" and the rest of
 *     the attribute string re-scans from there (tc8 parity: z=[] b=true).
 *   - '</b>' closing tags report isSelfClosing=1 (Lua: isSelfClosing = slash
 *     or isClosing); '<br>' reports isSelfClosing=0.
 *   - boolean attributes store the TRUE sentinel; duplicate attrs first-win.
 *   - the 256KB cut searches '>' from 1-based max(1, cut-128) — i.e. 0-based
 *     MAX-129 — and keeps everything through that '>'.
 *   - an unterminated <title> consumes only the opening tag (no token) when
 *     no '</title>' exists; a found one skips titleClose+8 and may leave
 *     '</title>'-interior bytes for the next loop iteration (Lua parity).
 *
 * Strings produced during tokenizing are copied into an internal arena
 * (grow-only, freed with the result) so 256KB pages don't fragment the small
 * device heap with thousands of tiny allocations.
 */
#include "tokenizer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "core/logger.h"
#include "html/entities.h"
#include "../core/pluto_mem.h"

PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)

const char PLUTO_TOK_ATTR_TRUE[1] = { '\x01' };

#define MAX_HTML_SIZE 262144 /* 256KB max buffer size (Lua parity) */

/* ── String arena: chunk list (same design as dom.c) ──────────────────────
 * Pointers handed out by arena_dup must stay valid for the result's lifetime
 * even as more strings are added — so chunks are allocated once and never
 * moved/reallocated. A chunk that fills up is retired; a new one is appended. */
typedef struct ArenaChunk
{
    struct ArenaChunk *next;
    size_t used;
    size_t cap;
    char data[]; /* flexible array member */
} ArenaChunk;

typedef struct
{
    ArenaChunk *head;
    ArenaChunk *tail;
} Arena;

#define ARENA_CHUNK_MIN 8192

static int arena_reserve(Arena *a, size_t extra)
{
    if (a->tail && a->tail->cap - a->tail->used >= extra + 1)
    {
        return 0;
    }
    size_t cap = ARENA_CHUNK_MIN;
    if (extra + 1 > cap)
    {
        cap = extra + 1; /* oversized string gets its own right-sized chunk */
    }
    ArenaChunk *c = (ArenaChunk *)PLUTO_MALLOC(sizeof(ArenaChunk) + cap);
    if (!c)
    {
        return -1;
    }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    if (a->tail)
    {
        a->tail->next = c;
    }
    else
    {
        a->head = c;
    }
    a->tail = c;
    return 0;
}

/* Copy `n` bytes + NUL into the arena; returns pointer or NULL on failure. */
static char *arena_dup(Arena *a, const char *s, size_t n)
{
    if (arena_reserve(a, n) != 0)
    {
        return NULL;
    }
    char *out = a->tail->data + a->tail->used;
    memcpy(out, s, n);
    out[n] = '\0';
    a->tail->used += n + 1;
    return out;
}

/* ── Token list / attr list ────────────────────────────────────────────────── */
static int toklist_push(TokenList *tl, const Token *t)
{
    if (tl->count == tl->cap)
    {
        int ncap = tl->cap ? tl->cap * 2 : 256;
        Token *ni = (Token *)PLUTO_MALLOC((size_t)ncap * sizeof(Token));
        if (!ni)
        {
            return -1;
        }
        if (tl->items)
        {
            memcpy(ni, tl->items, (size_t)tl->count * sizeof(Token));
            PLUTO_FREE(tl->items);
        }
        tl->items = ni;
        tl->cap = ncap;
    }
    tl->items[tl->count++] = *t;
    return 0;
}

/* Attr storage: arrays grow by doubling; attr capacity is derived from
 * attrCount (attr counts per tag are tiny; correctness over speed). */
static int attr_ensure_cap(Token *t)
{
    int cap = 4;
    while (cap < t->attrCount + 1)
    {
        cap *= 2;
    }
    if (t->attrs && cap <= 4 && t->attrCount < 4)
    {
        return 0; /* initial block already large enough */
    }
    if (t->attrCount == 0)
    {
        if (!t->attrs)
        {
            t->attrs = (TokenAttr *)PLUTO_MALLOC((size_t)cap * sizeof(TokenAttr));
            return t->attrs ? 0 : -1;
        }
        return 0;
    }
    TokenAttr *na = (TokenAttr *)PLUTO_MALLOC((size_t)cap * sizeof(TokenAttr));
    if (!na)
    {
        return -1;
    }
    memcpy(na, t->attrs, (size_t)t->attrCount * sizeof(TokenAttr));
    PLUTO_FREE(t->attrs);
    t->attrs = na;
    return 0;
}

static int attr_push(Token *t, char *key, char *value)
{
    if (attr_ensure_cap(t) != 0)
    {
        return -1;
    }
    t->attrs[t->attrCount].key = key;
    t->attrs[t->attrCount].value = value;
    t->attrCount++;
    return 0;
}

static int attr_has_key(const Token *tok, const char *key)
{
    for (int a = 0; a < tok->attrCount; a++)
    {
        if (strcmp(tok->attrs[a].key, key) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* Store an attribute (entity-decoded) if the key is not already present. */
static int store_attr(Token *tok, Arena *arena, const char *key,
                      const char *val, size_t vlen)
{
    if (attr_has_key(tok, key))
    {
        return 0; /* first-wins parity (Lua: attrs[lk] == nil guard) */
    }
    char *k = arena_dup(arena, key, strlen(key));
    if (!k)
    {
        return -1;
    }
    char *raw = (char *)PLUTO_MALLOC(vlen + 1);
    if (!raw)
    {
        return -1;
    }
    memcpy(raw, val, vlen);
    raw[vlen] = '\0';
    char *dec = entities_decode(raw);
    PLUTO_FREE(raw);
    if (!dec)
    {
        return -1;
    }
    char *v = arena_dup(arena, dec, strlen(dec));
    pluto_free(dec);
    if (!v)
    {
        return -1;
    }
    return attr_push(tok, k, v);
}

/* ── findTagEnd: closing '>' of a tag, skipping quoted values ──────────────── */
static const char *find_tag_end(const char *p)
{
    while (*p)
    {
        while (*p && *p != '>' && *p != '"' && *p != '\'')
        {
            p++;
        }
        if (!*p)
        {
            return NULL;
        }
        if (*p == '>')
        {
            return p;
        }
        char q = *p; /* inside a quoted value: skip to the matching quote */
        p++;
        while (*p && *p != q)
        {
            p++;
        }
        if (!*p)
        {
            return NULL;
        }
        p++;
    }
    return NULL;
}

/* Bounded substring search. */
static const char *find_from(const char *hay, const char *hayEnd,
                             const char *needle, size_t needleLen)
{
    if (needleLen == 0 || hayEnd - hay < (ptrdiff_t)needleLen)
    {
        return NULL;
    }
    for (const char *p = hay; p + (ptrdiff_t)needleLen <= hayEnd; p++)
    {
        if (*p == needle[0] && memcmp(p, needle, needleLen) == 0)
        {
            return p;
        }
    }
    return NULL;
}

/* Case-insensitive search for "</tag>" starting at `from`. */
static const char *find_close_tag(const char *from, const char *hayEnd, const char *tag)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "</%s>", tag);
    size_t plen = strlen(pat);
    for (const char *p = from; p + (ptrdiff_t)plen <= hayEnd; p++)
    {
        if (*p == '<' && strncasecmp(p, pat, plen) == 0)
        {
            return p;
        }
    }
    return NULL;
}

/* ── parseAttributes (Lua algorithm, quirks included) ──────────────────────── */
static int parse_attributes(const char *p0, const char *n0, Token *tok, Arena *arena)
{
    const char *i = p0;
    const char *n = n0;
    while (i < n)
    {
        /* find next non-space (%S) */
        while (i < n && isspace((unsigned char)*i))
        {
            i++;
        }
        if (i >= n)
        {
            break;
        }
        const char *s = i;

        /* key end: first char not in [%w%-_:] */
        const char *ke = s;
        while (ke < n && (isalnum((unsigned char)*ke) || *ke == '-' || *ke == '_' || *ke == ':'))
        {
            ke++;
        }
        if (ke == s)
        {
            i = s + 1;
            continue;
        }

        size_t klen = (size_t)(ke - s);
        char lowerKey[64];
        if (klen >= sizeof(lowerKey))
        {
            klen = sizeof(lowerKey) - 1;
        }
        for (size_t c = 0; c < klen; c++)
        {
            lowerKey[c] = (char)tolower((unsigned char)s[c]);
        }
        lowerKey[klen] = '\0';

        /* Lua: string.find(attrStr, "^%s*=", ke) — whitespace BEFORE '=' only */
        const char *e = ke;
        while (e < n && isspace((unsigned char)*e))
        {
            e++;
        }
        int hasEquals = (e < n && *e == '=');

        if (!hasEquals)
        {
            /* boolean attribute → Lua true (first win) */
            if (!attr_has_key(tok, lowerKey))
            {
                char *k = arena_dup(arena, lowerKey, strlen(lowerKey));
                if (!k || attr_push(tok, k, (char *)PLUTO_TOK_ATTR_TRUE) != 0)
                {
                    return -1;
                }
            }
            i = ke;
            continue;
        }

        /* Lua: vs = find(attrStr, "%S", ke + 1) — starts right after the key,
         * so with a spaced '=' this lands ON '=' (empty value quirk). */
        const char *vs = ke + 1;
        while (vs < n && isspace((unsigned char)*vs))
        {
            vs++;
        }
        if (vs >= n)
        {
            /* key= with nothing after it → attrs[key] = "" (once) */
            if (!attr_has_key(tok, lowerKey))
            {
                char *k = arena_dup(arena, lowerKey, strlen(lowerKey));
                char *v = arena_dup(arena, "", 0);
                if (!k || !v || attr_push(tok, k, v) != 0)
                {
                    return -1;
                }
            }
            break;
        }

        char c = *vs;
        if (c == '"' || c == '\'')
        {
            const char *ve = vs + 1;
            while (ve < n && *ve != c)
            {
                ve++;
            }
            if (ve >= n)
            {
                break; /* Lua: unterminated quote aborts the loop */
            }
            if (store_attr(tok, arena, lowerKey, vs + 1, (size_t)(ve - vs - 1)) != 0)
            {
                return -1;
            }
            i = ve + 1;
        }
        else
        {
            /* unquoted: ends at first char not in [%w%-_%.%/%?%#] — note '='
             * is excluded, so a spaced '=' yields an empty value (Lua parity) */
            const char *ve = vs;
            while (ve < n && (isalnum((unsigned char)*ve) || *ve == '-' || *ve == '_' ||
                              *ve == '.' || *ve == '/' || *ve == '?' || *ve == '#'))
            {
                ve++;
            }
            if (store_attr(tok, arena, lowerKey, vs, (size_t)(ve - vs)) != 0)
            {
                return -1;
            }
            i = ve;
        }
    }
    return 0;
}

/* ── helpers for the main loop ─────────────────────────────────────────────── */

/* Trim [b,e) like Lua gsub("^%s*(.-)%s*$","%1"). */
static void trim_span(const char **b, const char **e)
{
    while (*b < *e && isspace((unsigned char)**b))
    {
        (*b)++;
    }
    while (*e > *b && isspace((unsigned char)(*e)[-1]))
    {
        (*e)--;
    }
}

/* Decode a text span and push a text token (skipped when empty). */
static int push_text(TokenizeResult *res, Arena *arena, const char *p, size_t len)
{
    if (len == 0)
    {
        return 0;
    }
    char *raw = (char *)PLUTO_MALLOC(len + 1);
    if (!raw)
    {
        return -1;
    }
    memcpy(raw, p, len);
    raw[len] = '\0';
    char *dec = entities_decode(raw);
    PLUTO_FREE(raw);
    if (!dec)
    {
        return -1;
    }
    size_t dlen = strlen(dec);
    if (dlen == 0)
    {
        pluto_free(dec);
        return 0; /* Lua: text ~= "" guard */
    }
    char *stored = arena_dup(arena, dec, dlen);
    pluto_free(dec);
    if (!stored)
    {
        return -1;
    }
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_TEXT;
    t.content = stored;
    return toklist_push(&res->tokens, &t);
}

/* Collapse runs of whitespace to single spaces (Lua gsub("%s+"," ")). */
static void collapse_ws(const char *src, size_t len, char *dst, size_t dstCap)
{
    size_t o = 0;
    int lastSpace = 0;
    for (size_t i = 0; i < len && o + 1 < dstCap; i++)
    {
        char c = src[i];
        if (isspace((unsigned char)c))
        {
            if (!lastSpace && o > 0)
            {
                dst[o++] = ' ';
            }
            lastSpace = 1;
        }
        else
        {
            dst[o++] = c;
            lastSpace = 0;
        }
    }
    while (o > 0 && dst[o - 1] == ' ')
    {
        o--; /* trim trailing */
    }
    dst[o] = '\0';
}

/* ── Public API ────────────────────────────────────────────────────────────── */

int tokenizer_tokenize(const char *html, TokenizeResult *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->pageTitle, "Web Page");

    Arena *arena = (Arena *)PLUTO_MALLOC(sizeof(Arena));
    if (!arena)
    {
        return -1;
    }
    memset(arena, 0, sizeof(*arena));
    out->_arena = arena;

    if (!html || !html[0])
    {
        return 0; /* Lua: empty input → no tokens, default title */
    }

    const char *src = html;
    size_t len = strlen(src);
    char *cutBuf = NULL;

    if (len > MAX_HTML_SIZE)
    {
        /* Lua: find ">" from 1-based max(1, MAX-128) = 0-based MAX-129;
         * cut keeps everything through that '>' (or MAX when none). */
        size_t cut = MAX_HTML_SIZE;
        const char *gt = strchr(src + (MAX_HTML_SIZE - 129), '>');
        if (gt)
        {
            cut = (size_t)(gt - src) + 1;
        }
        cutBuf = (char *)PLUTO_MALLOC(cut + 1);
        if (!cutBuf)
        {
            return -1;
        }
        memcpy(cutBuf, src, cut);
        cutBuf[cut] = '\0';
        src = cutBuf;
        len = cut;
    }

    const char *pos = src;
    const char *end = src + len;

    while (pos < end)
    {
        const char *tagStart = memchr(pos, '<', (size_t)(end - pos));
        if (!tagStart)
        {
            if (push_text(out, arena, pos, (size_t)(end - pos)) != 0)
            {
                goto fail;
            }
            break;
        }

        /* text before this tag */
        if (tagStart > pos)
        {
            if (push_text(out, arena, pos, (size_t)(tagStart - pos)) != 0)
            {
                goto fail;
            }
        }

        const char *tagEnd = find_tag_end(tagStart + 1);
        if (!tagEnd)
        {
            /* unterminated tag: drop the broken remainder (Lua parity) */
            break;
        }

        const char *b = tagStart + 1;
        const char *e = tagEnd;
        trim_span(&b, &e);
        size_t insideLen = (size_t)(e - b);

        /* head = lowercase first 8 bytes of the trimmed inside */
        char head[9] = { 0 };
        for (size_t c = 0; c < insideLen && c < 8; c++)
        {
            head[c] = (char)tolower((unsigned char)b[c]);
        }

        if (insideLen >= 3 && strncmp(b, "!--", 3) == 0)
        {
            /* 1. HTML comment: <!-- ... --> */
            const char *cend = find_from(tagStart, end, "-->", 3);
            pos = cend ? cend + 3 : tagEnd + 1;
        }
        else if (strncmp(head, "script", 6) == 0)
        {
            /* 2. skip <script> ... </script> */
            const char *sc = find_close_tag(tagEnd, end, "script");
            if (sc)
            {
                const char *gt = memchr(sc, '>', (size_t)(end - sc));
                pos = (gt ? gt : sc) + 1;
            }
            else
            {
                pos = end;
            }
        }
        else if (strncmp(head, "style", 5) == 0)
        {
            /* 3. skip <style> ... </style> */
            const char *sc = find_close_tag(tagEnd, end, "style");
            if (sc)
            {
                const char *gt = memchr(sc, '>', (size_t)(end - sc));
                pos = (gt ? gt : sc) + 1;
            }
            else
            {
                pos = end;
            }
        }
        else if (strncmp(head, "title", 5) == 0)
        {
            /* 4. page <title>: no token; pageTitle = collapsed decoded text */
            const char *tc = find_close_tag(tagEnd, end, "title");
            if (tc)
            {
                char collapsed[256];
                collapse_ws(tagEnd + 1, (size_t)(tc - (tagEnd + 1)),
                            collapsed, sizeof(collapsed));
                char *dec = entities_decode(collapsed);
                if (dec)
                {
                    snprintf(out->pageTitle, sizeof(out->pageTitle), "%s", dec);
                    pluto_free(dec);
                }
                pos = tc + 8; /* Lua: titleClose + 8 */
            }
            else
            {
                pos = tagEnd + 1;
            }
        }
        else
        {
            /* 7. normal tag */
            int isClosing = (insideLen > 0 && b[0] == '/');
            const char *body = isClosing ? b + 1 : b;
            size_t bodyLen = isClosing ? insideLen - 1 : insideLen;
            /* tagBody already trimmed (rawInside was trimmed) */

            int selfFromSlash = (bodyLen > 0 && body[bodyLen - 1] == '/');
            int isSelfClosing = selfFromSlash || isClosing;
            if (selfFromSlash)
            {
                bodyLen--;
            }

            size_t nameLen = 0;
            while (nameLen < bodyLen &&
                   (isalnum((unsigned char)body[nameLen]) || body[nameLen] == '-' ||
                    body[nameLen] == ':'))
            {
                nameLen++;
            }

            if (nameLen > 0)
            {
                Token t;
                memset(&t, 0, sizeof(t));
                t.type = TOK_TAG;
                t.name = arena_dup(arena, body, nameLen);
                if (!t.name)
                {
                    goto fail;
                }
                for (size_t c = 0; c < nameLen; c++)
                {
                    t.name[c] = (char)tolower((unsigned char)t.name[c]);
                }
                t.isClosing = isClosing;
                t.isSelfClosing = isSelfClosing;
                if (parse_attributes(body + nameLen, body + bodyLen, &t, arena) != 0)
                {
                    goto fail;
                }
                if (toklist_push(&out->tokens, &t) != 0)
                {
                    goto fail;
                }
            }

            pos = tagEnd + 1;
        }
    }

    if (cutBuf)
    {
        PLUTO_FREE(cutBuf);
    }
    return 0;

fail:
    if (cutBuf)
    {
        PLUTO_FREE(cutBuf);
    }
    return -1;
}

void tokenizer_free_result(TokenizeResult *res)
{
    if (!res)
    {
        return;
    }
    for (int i = 0; i < res->tokens.count; i++)
    {
        Token *t = &res->tokens.items[i];
        if (t->attrs)
        {
            PLUTO_FREE(t->attrs);
        }
    }
    if (res->tokens.items)
    {
        PLUTO_FREE(res->tokens.items);
    }
    Arena *arena = (Arena *)res->_arena;
    if (arena)
    {
        ArenaChunk *c = arena->head;
        while (c)
        {
            ArenaChunk *nx = c->next;
            PLUTO_FREE(c);
            c = nx;
        }
        PLUTO_FREE(arena);
    }
    memset(res, 0, sizeof(*res));
}
