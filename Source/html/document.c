/*
 * PlutoBrowser — document.c
 * Complete port of Source/html/document.lua (parse helpers + DOM walk + full
 * Document.parse pipeline).
 *
 * LUA → C FUNCTION MAP (document.lua 1488 lines):
 *   parseStyle        → doc_parse_style        (last duplicate key wins)
 *   parseAlign        → doc_parse_align        (lowercased center/right/left only)
 *   isDisplayNone     → doc_is_display_none    (hidden/popover/display:none)
 *   isInvertedStyle   → doc_is_inverted_style  (patterns on lowercased value)
 *   parseBoxSpacing   → doc_parse_box_spacing  (strict num, halved, left-only pad)
 *   concatNodeText    → doc_concat_node_text
 *   validHref         → doc_valid_href         ("", "#…", javascript:, data:)
 *   serializeSvgNode  → doc_serialize_svg_node
 *   Document.parse    → document_parse (+ document_free)
 *     Lua closure state (state.*) → Walker struct; Lua recursive walk(node) →
 *     iterative frame stack (push_enter/push_exit/WFrame) with one recursion
 *     level for table cells; Lua 1-based table.insert semantics preserved at
 *     every call site (verified against capture batteries P18/P19).
 *
 * Every helper reproduces the Lua reference's observable behavior, including
 * its quirks (each verified against the verbatim Lua code — see MASTER_TODO):
 *   - parseStyle: "([%w%-]+)%s*:%s*([^;]+)" — the LAST duplicate key wins;
 *     unparseable segments (no valid "key:") do NOT abort the scan, the
 *     gmatch simply continues with the segment AFTER the next ';'.
 *   - parseBoxSpacing: num() uses strict tonumber on the component (only the
 *     '%' stripped) — "10px" is NOT a number and the component is dropped.
 *     Component values are halved (floor). padding applies only to LEFT.
 *   - isInvertedStyle patterns run against the LOWERCASED style value (they
 *     come from parseStyle, which lowercases values).
 *   - validHref rejects "", "#…", javascript:, data: (case-insensitive).
 *   - parseAlign returns the LOWERCASED value only for center/right/left.
 * Attributes arrive as raw (key, value) pairs — exactly like tok.attrs in
 * Lua — and every helper parses attrs["style"] internally via parseStyle.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "html/document.h"
#include "html/tokenizer.h"
#include "html/readability.h"
#include "html/entities.h"
#include "core/constants.h"
#include "core/url.h"
#include "util/strbuf.h"

extern PlaydateAPI *pluto_pd(void);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_REALLOC(p, n) pluto_pd()->system->realloc((p), (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

/* Lua string.lower: ASCII only. */
static char lua_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static void lua_lower_buf(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++)
    {
        dst[i] = lua_lower(src[i]);
    }
    dst[i] = '\0';
}

/* ASCII strncasecmp (Lua-style char-class equivalence). */
static int ci_prefix(const char *s, const char *prefix, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char cs = lua_lower(s[i]);
        char cp = lua_lower(prefix[i]);
        if (cs != cp)
        {
            return (unsigned char)cs - (unsigned char)cp;
        }
        if (cs == '\0')
        {
            return 0;
        }
    }
    return 0;
}

/* ── Generic (key, value) map access — mirrors Lua attrs tables ───────────── */

static const char *map_get(const DocStyleEntry *map, int count, const char *key)
{
    if (!map)
    {
        return NULL;
    }
    for (int i = 0; i < count; i++)
    {
        if (strcmp(map[i].key, key) == 0)
        {
            return map[i].val;
        }
    }
    return NULL;
}

/* Lua %s character class: space, \t, \n, \v, \f, \r. */
static int is_lspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

static int is_lkey(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
}

/* ── parseStyle ─────────────────────────────────────────────────────────────── */

void doc_parse_style(const char *styleStr, DocStyle *out)
{
    memset(out, 0, sizeof(*out));
    if (!styleStr || styleStr[0] == '\0')
    {
        return;
    }
    /* Exact emulation of string.gmatch(styleStr, "([%w%-]+)%s*:%s*([^;]+)"):
     * the scan is NOT anchored to ';'-segments — it advances one character at
     * a time and takes the FIRST position where a [%w%-]+ run (optionally
     * space-separated) hits ':'. The value [^;]+ then consumes to the next
     * ';' or end. After a match, scanning resumes after the value. A match
     * with an all-space tail still stores an EMPTY value (Lua "" is truthy
     * and consumers treat it as set-but-blank). */
    const char *s = styleStr;
    size_t n = strlen(s);
    size_t pos = 0;
    while (pos < n)
    {
        if (!is_lkey(s[pos]))
        {
            pos++;
            continue;
        }
        size_t ke = pos;
        while (ke < n && is_lkey(s[ke]))
        {
            ke++;
        }
        size_t c = ke;
        while (c < n && is_lspace(s[c]))
        {
            c++;
        }
        if (c < n && s[c] == ':')
        {
            /* %s* (greedy, with Lua backtracking) then [^;]+ (≥1 char). */
            size_t vStart = c + 1;
            size_t vEnd = vStart;
            while (vEnd < n && is_lspace(s[vEnd]))
            {
                vEnd++;
            }
            if (vEnd == n && vEnd > vStart)
            {
                /* Only spaces to end: Lua backtracks %s* so [^;]+ takes one
                 * space → value trims to "" (an EMPTY entry is stored). */
                vEnd = n;
                size_t a = vStart, z = vEnd;
                char key[32];
                size_t kn = (ke - pos) < sizeof(key) - 1 ? (ke - pos)
                                                         : sizeof(key) - 1;
                size_t i;
                for (i = 0; i < kn; i++)
                {
                    key[i] = lua_lower(s[pos + i]);
                }
                key[i] = '\0';
                int found = -1;
                for (int e = 0; e < out->count; e++)
                {
                    if (strcmp(out->e[e].key, key) == 0)
                    {
                        found = e;
                        break;
                    }
                }
                if (found < 0 && out->count < DOC_STYLE_MAX)
                {
                    snprintf(out->e[out->count].key,
                             sizeof(out->e[out->count].key), "%s", key);
                    out->e[out->count].val[0] = '\0';
                    out->count++;
                }
                (void)a;
                (void)z;
                pos = n;
                continue;
            }
            if (vEnd < n)
            {
                /* [^;]+ consumes to the next ';' or end. */
                while (vEnd < n && s[vEnd] != ';')
                {
                    vEnd++;
                }
                /* Lua trims value with gsub "^%s*(.-)%s*$". */
                size_t a = vStart, z = vEnd;
                while (a < z && is_lspace(s[a]))
                {
                    a++;
                }
                while (z > a && is_lspace(s[z - 1]))
                {
                    z--;
                }
                char key[32];
                char val[128];
                size_t kn = (ke - pos) < sizeof(key) - 1 ? (ke - pos)
                                                         : sizeof(key) - 1;
                size_t vn = (z - a) < sizeof(val) - 1 ? (z - a) : sizeof(val) - 1;
                size_t i;
                for (i = 0; i < kn; i++)
                {
                    key[i] = lua_lower(s[pos + i]);
                }
                key[i] = '\0';
                for (i = 0; i < vn; i++)
                {
                    val[i] = lua_lower(s[a + i]);
                }
                val[i] = '\0';
                int found = -1;
                for (int e = 0; e < out->count; e++)
                {
                    if (strcmp(out->e[e].key, key) == 0)
                    {
                        found = e;
                        break;
                    }
                }
                if (found >= 0)
                {
                    snprintf(out->e[found].val, sizeof(out->e[found].val), "%s",
                             val);
                }
                else if (out->count < DOC_STYLE_MAX)
                {
                    snprintf(out->e[out->count].key,
                             sizeof(out->e[out->count].key), "%s", key);
                    snprintf(out->e[out->count].val,
                             sizeof(out->e[out->count].val), "%s", val);
                    out->count++;
                }
                pos = vEnd; /* gmatch resumes after the whole match */
                continue;
            }
            /* Tail empty after ':' → [^;]+ fails even with backtracking
             * ("k:" at end) → NO entry; resume scan one char later. */
            pos = pos + 1;
            continue;
        }
        /* No ':' after this run → retry at the next character (gmatch). */
        pos = pos + 1;
    }
}

static const char *style_get(const DocStyle *st, const char *key)
{
    for (int i = 0; i < st->count; i++)
    {
        if (strcmp(st->e[i].key, key) == 0)
        {
            return st->e[i].val;
        }
    }
    return NULL;
}

/* ── parseAlign ─────────────────────────────────────────────────────────────── */

const char *doc_parse_align(const DocStyleEntry *attrs, int attrCount)
{
    if (!attrs || attrCount == 0)
    {
        return NULL;
    }
    const char *a = map_get(attrs, attrCount, "align");
    const char *styleStr = map_get(attrs, attrCount, "style");
    static DocStyle st; /* static: 1.6KB struct off the game-task stack */
    doc_parse_style(styleStr, &st);
    const char *ta = style_get(&st, "text-align");
    if (ta)
    {
        a = ta;
    }
    if (!a)
    {
        return NULL;
    }
    static char buf[64];
    lua_lower_buf(buf, a, sizeof(buf));
    if (strcmp(buf, "center") == 0 || strcmp(buf, "right") == 0 ||
        strcmp(buf, "left") == 0)
    {
        return buf;
    }
    return NULL;
}

/* ── isDisplayNone ──────────────────────────────────────────────────────────── */

int doc_is_display_none(const DocStyleEntry *attrs, int attrCount)
{
    if (!attrs || attrCount == 0)
    {
        return 0;
    }
    if (map_get(attrs, attrCount, "hidden"))
    {
        return 1;
    }
    if (map_get(attrs, attrCount, "popover"))
    {
        return 1;
    }
    const char *styleStr = map_get(attrs, attrCount, "style");
    static DocStyle st; /* static: 1.6KB struct off the game-task stack */
    doc_parse_style(styleStr, &st);
    const char *d = style_get(&st, "display");
    if (d && strstr(d, "none"))
    {
        return 1;
    }
    const char *v = style_get(&st, "visibility");
    if (v && strstr(v, "hidden"))
    {
        return 1;
    }
    return 0;
}

/* ── isInvertedStyle ────────────────────────────────────────────────────────── */

int doc_is_inverted_style(const DocStyleEntry *attrs, int attrCount)
{
    if (!attrs || attrCount == 0)
    {
        return 0;
    }
    const char *styleStr = map_get(attrs, attrCount, "style");
    static DocStyle st; /* static: 1.6KB struct off the game-task stack */
    doc_parse_style(styleStr, &st);
    const char *c = style_get(&st, "color");
    if (c)
    {
        /* Patterns run on the already-lowercased value (parseStyle). */
        if (strstr(c, "white") || strstr(c, "#fff") ||
            (strncmp(c, "#ffff", 5) == 0 && c[5] != '\0'))
        {
            return 1;
        }
    }
    const char *bg = style_get(&st, "background-color");
    if (!bg)
    {
        bg = style_get(&st, "background");
    }
    if (bg)
    {
        if (strstr(bg, "black") || strstr(bg, "#000"))
        {
            return 1;
        }
    }
    return 0;
}

/* ── parseBoxSpacing ────────────────────────────────────────────────────────── */

/* Lua num(): strip '%', then STRICT tonumber. "10px" fails → component 0. */
static int box_num(const char *v)
{
    if (!v)
    {
        return 0;
    }
    char tmp[64];
    size_t o = 0;
    for (size_t i = 0; v[i] && o + 1 < sizeof(tmp); i++)
    {
        if (v[i] != '%')
        {
            tmp[o++] = v[i];
        }
    }
    tmp[o] = '\0';
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (end == tmp || *end != '\0')
    {
        return 0;
    }
    return (int)floor(d / 2.0); /* math.floor(n / 2) — true floor */
}

/* Split value on spaces/commas (Lua "[^%s,]+"), max 4 parts. */
static int split_parts(const char *v, char parts[4][32])
{
    int n = 0;
    const char *p = v;
    while (*p && n < 4)
    {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }
        size_t len = 0;
        while (p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != ',' &&
               p[len] != '\n' && p[len] != '\r')
        {
            len++;
        }
        size_t cn = len < 31 ? len : 31;
        memcpy(parts[n], p, cn);
        parts[n][cn] = '\0';
        n++;
        p += len;
    }
    return n;
}

DocBoxSpacing doc_parse_box_spacing(const DocStyleEntry *attrs, int attrCount)
{
    DocBoxSpacing out = {0, 0, 0, 0};
    if (!attrs || attrCount == 0)
    {
        return out;
    }
    const char *styleStr = map_get(attrs, attrCount, "style");
    static DocStyle st; /* static: 1.6KB struct off the game-task stack */
    doc_parse_style(styleStr, &st);
    const char *m = style_get(&st, "margin");
    if (m)
    {
        char parts[4][32];
        int n = split_parts(m, parts);
        int t = 0, r = 0, b = 0, l = 0;
        if (n == 1)
        {
            t = r = b = l = box_num(parts[0]);
        }
        else if (n == 2)
        {
            t = b = box_num(parts[0]);
            r = l = box_num(parts[1]);
        }
        else if (n == 3)
        {
            t = box_num(parts[0]);
            r = l = box_num(parts[1]);
            b = box_num(parts[2]);
        }
        else if (n == 4)
        {
            t = box_num(parts[0]);
            r = box_num(parts[1]);
            b = box_num(parts[2]);
            l = box_num(parts[3]);
        }
        out.top = t;
        out.bottom = b;
        out.left = l;
        out.right = r;
    }
    else
    {
        const char *mt = style_get(&st, "margin-top");
        if (mt)
        {
            out.top = box_num(mt);
        }
        const char *mb = style_get(&st, "margin-bottom");
        if (mb)
        {
            out.bottom = box_num(mb);
        }
        const char *ml = style_get(&st, "margin-left");
        if (ml)
        {
            out.left = box_num(ml);
        }
        const char *mr = style_get(&st, "margin-right");
        if (mr)
        {
            out.right = box_num(mr);
        }
    }
    const char *p = style_get(&st, "padding");
    if (p)
    {
        char parts[4][32];
        int n = split_parts(p, parts);
        if (n == 1)
        {
            out.left += box_num(parts[0]);
        }
        else if (n == 2)
        {
            out.left += box_num(parts[1]);
        }
        else if (n == 4)
        {
            out.left += box_num(parts[3]);
        }
    }
    else
    {
        const char *pl = style_get(&st, "padding-left");
        if (pl)
        {
            out.left += box_num(pl);
        }
    }
    return out;
}

/* ── concatNodeText ─────────────────────────────────────────────────────────── */

static size_t concat_rec(const DomNode *n, char *buf, size_t cap, size_t off)
{
    if (!n)
    {
        return off;
    }
    if (n->kind == DOM_TEXT)
    {
        const char *t = n->text ? n->text : "";
        size_t len = strlen(t);
        if (off + 1 < cap)
        {
            size_t room = cap - off - 1;
            size_t cn = len < room ? len : room;
            memcpy(buf + off, t, cn);
        }
        return off + len; /* logical offset advances by FULL len; writes clamped */
    }
    if (n->kind == DOM_ELEMENT)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            off = concat_rec(n->children[i], buf, cap, off);
        }
    }
    return off;
}

size_t doc_concat_node_text(const DomNode *node, char *buf, size_t cap)
{
    size_t need = concat_rec(node, buf, cap, 0);
    if (cap > 0)
    {
        size_t written = need < cap - 1 ? need : cap - 1;
        buf[written] = '\0';
    }
    return need;
}

/* ── validHref ──────────────────────────────────────────────────────────────── */

int doc_valid_href(const char *raw)
{
    if (!raw || raw[0] == '\0')
    {
        return 0;
    }
    if (raw[0] == '#')
    {
        return 0;
    }
    if (ci_prefix(raw, "javascript:", 11) == 0)
    {
        return 0;
    }
    if (ci_prefix(raw, "data:", 5) == 0)
    {
        return 0;
    }
    return 1;
}

/* ── serializeSvgNode ───────────────────────────────────────────────────────── */

static int svg_append_escaped_attr(StrBuf *sb, const char *v)
{
    for (const char *p = v; *p; p++)
    {
        if (*p == '"')
        {
            if (strbuf_append(sb, "&quot;") != 0)
            {
                return -1;
            }
        }
        else if (*p == '<')
        {
            if (strbuf_append(sb, "&lt;") != 0)
            {
                return -1;
            }
        }
        else if (strbuf_append_char(sb, *p) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int svg_serialize(const DomNode *n, StrBuf *sb)
{
    if (n->kind == DOM_TEXT)
    {
        char *enc = entities_encode(n->text ? n->text : "");
        if (!enc)
        {
            return -1;
        }
        int rc = strbuf_append(sb, enc);
        PLUTO_FREE(enc);
        return rc;
    }
    if (n->kind == DOM_ELEMENT)
    {
        if (strbuf_append_char(sb, '<') != 0 ||
            strbuf_append(sb, n->tag ? n->tag : "") != 0)
        {
            return -1;
        }
        for (int i = 0; i < n->attrCount; i++)
        {
            const char *v = n->attrs[i].value == PLUTO_TOK_ATTR_TRUE
                                ? ""
                                : (n->attrs[i].value ? n->attrs[i].value : "");
            if (strbuf_append_char(sb, ' ') != 0 ||
                strbuf_append(sb, n->attrs[i].key) != 0 ||
                strbuf_append(sb, "=\"") != 0 ||
                svg_append_escaped_attr(sb, v) != 0 ||
                strbuf_append_char(sb, '"') != 0)
            {
                return -1;
            }
        }
        if (n->childCount == 0)
        {
            return strbuf_append(sb, "/>") != 0 ? -1 : 0;
        }
        if (strbuf_append_char(sb, '>') != 0)
        {
            return -1;
        }
        for (int c = 0; c < n->childCount; c++)
        {
            if (svg_serialize(n->children[c], sb) != 0)
            {
                return -1;
            }
        }
        if (strbuf_append(sb, "</") != 0 ||
            strbuf_append(sb, n->tag ? n->tag : "") != 0 ||
            strbuf_append_char(sb, '>') != 0)
        {
            return -1;
        }
    }
    return 0; /* other kinds → "" */
}

char *doc_serialize_svg_node(const DomNode *n)
{
    StrBuf sb;
    if (strbuf_init(&sb) != 0)
    {
        return NULL;
    }
    if (n && svg_serialize(n, &sb) != 0)
    {
        strbuf_free(&sb);
        return NULL;
    }
    return strbuf_detach(&sb);
}
/* ── P19: element walker — port of document.lua lines 241–1443 ─────────────── */

typedef struct DocChunk
{
    struct DocChunk *next;
    size_t used;
    size_t cap;
} DocChunk;

typedef struct
{
    DocChunk *head;
} DocArena;

typedef struct Walker Walker;

static void *doc_arena_alloc(DocArena *a, size_t n)
{
    n = (n + 7u) & ~(size_t)7u;
    if (a->head && a->head->used + n <= a->head->cap)
    {
        void *p = (char *)a->head + sizeof(DocChunk) + a->head->used;
        a->head->used += n;
        return p;
    }
    size_t c = n > 4096 ? n : 4096;
    DocChunk *ch = (DocChunk *)PLUTO_MALLOC(sizeof(DocChunk) + c);
    if (!ch)
    {
        return NULL;
    }
    ch->next = a->head;
    ch->used = n;
    ch->cap = c;
    a->head = ch;
    return (char *)ch + sizeof(DocChunk);
}

static void doc_arena_free_all(DocArena *a)
{
    DocChunk *ch = a->head;
    while (ch)
    {
        DocChunk *nx = ch->next;
        PLUTO_FREE(ch);
        ch = nx;
    }
    a->head = NULL;
}

static int doc_ptrarr_push(void ***arr, int *count, int *cap, void *item)
{
    if (*count >= *cap)
    {
        int nc = *cap ? *cap * 2 : 8;
        void **na = (void **)PLUTO_REALLOC(*arr, (size_t)nc * sizeof(void *));
        if (!na)
        {
            return -1;
        }
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*count)++] = item;
    return 0;
}

/* ── Attribute access (DomAttr arrays) ────────────────────────────────────── */

typedef struct
{
    const DomAttr *items;
    int count;
} AttrList;

static AttrList attrs_of(const DomNode *n)
{
    AttrList a;
    a.items = n ? n->attrs : NULL;
    a.count = n ? n->attrCount : 0;
    return a;
}

/* NULL when absent; "" for boolean attributes (PLUTO_TOK_ATTR_TRUE). */
static const char *attr_val(AttrList a, const char *key)
{
    for (int i = 0; i < a.count; i++)
    {
        if (strcmp(a.items[i].key, key) == 0)
        {
            const char *v = a.items[i].value;
            return (v == PLUTO_TOK_ATTR_TRUE || !v) ? "" : v;
        }
    }
    return NULL;
}

/* Attribute PRESENCE ("" value still counts, matches Lua attrs[k] ~= nil). */
static int attr_has(AttrList a, const char *key)
{
    for (int i = 0; i < a.count; i++)
    {
        if (strcmp(a.items[i].key, key) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static const char *attr_or_d(AttrList a, const char *key, const char *dflt)
{
    const char *v = attr_val(a, key);
    return v ? v : dflt;
}

/* Lua tonumber (strict, trailing space allowed). */
static double strict_num(const char *s, double fb)
{
    if (!s)
    {
        return fb;
    }
    char *end;
    double d = strtod(s, &end);
    if (end == s)
    {
        return fb;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
    {
        end++;
    }
    if (*end != '\0')
    {
        return fb;
    }
    return d;
}

/* ── Walker state ─────────────────────────────────────────────────────────── */

typedef struct ListCtx
{
    int ordered;
    int count;
    int depth;
    int start;
    int reversed;
    char markerType;
} ListCtx;

typedef struct FigureCtx
{
    StrBuf *cap; /* heap; freed at figure end / walker teardown */
    DocBlock *image;
} FigureCtx;

/* Exit-frame actions (post-children work the Lua handler does inline). */
enum
{
    WX_NONE = 0,
    WX_FLUSH,        /* p/div/…/h1-h6/blockquote/center/marquee/li/dt/dd */
    WX_STYLE,        /* b/i/u/code/mark/small/big/sub/sup/del/rt: restore style */
    WX_LINK_END,     /* a: push doc.links, clear href/anchor/linkText */
    WX_QUOTE_END,    /* q: closing quote */
    WX_PRE_END,      /* pre/xmp/…: build code_block */
    WX_FIGURE_END,   /* figure: caption merge + restore */
    WX_LIST_END,     /* ul/ol/menu/dir: restore listCtx */
    WX_DL_END,       /* dl: restore dlDepth */
    WX_FORM_END,     /* form: restore formAction/method */
    WX_TEXTAREA_END, /* textarea: build input_field */
    WX_MATH_END,     /* math: build math block */
    WX_FIELDSET_END, /* fieldset: box_close + restore disabledDepth */
    WX_DETAILS_END,  /* details: box_close */
    WX_DIALOG_END,   /* dialog: box_close */
    WX_SPAN_END,     /* span/font/time/data/…: fallback + restore style */
    WX_FIGCAP_END,   /* figcaption: restore figureCaptionDone */
    WX_INERT_END     /* inert attribute: restore state.inert after the subtree */
};

typedef struct ExitCtx
{
    int kind;
    unsigned flags; /* saved inline style */
    char *href; /* saved currentHref (a) */
    int anchorIndex;
    char *title; /* a: attrs["title"] */
    char *target; /* a: attrs["target"] */
    int hadInlines; /* span: inline count at entry */
    char *fallback; /* span: time datetime / data value */
    void *listCtx; /* saved ListCtx* */
    void *figure; /* saved FigureCtx* */
    int inMath;
    int disabledDepth;
    int dlDepth;
    int detailsIdx;
    int toggleOpen;
    char *formAction;
    char *formMethod;
    int inertSaved; /* inert attribute: state.inert at entry */
} ExitCtx;

typedef struct WFrame
{
    int kind; /* 0 enter node, 1 exit ctx, 2 separator */
    const DomNode *node;
    ExitCtx *ctx;
    const char *sep;
} WFrame;

struct Walker
{
    DocParseResult *doc;
    DocArena arena;
    const DocParseOpts *opts;
    WFrame *frames;
    int frameCount;
    int frameCap;

    /* inline style flags */
    unsigned flags;

    /* link context */
    char *currentHref; /* arena */
    int currentAnchorIndex;
    StrBuf linkText;

    /* pre / textarea / math */
    int inPre;
    StrBuf preBuf;
    int inTextarea;
    char *textareaName; /* arena */
    StrBuf textareaBuf;
    int inMath;
    StrBuf mathBuf;

    /* lists */
    ListCtx *listCtx; /* arena */
    int dlDepth;

    /* table cell */
    DocCell *cell;
    int cellFirstText;
    DocTable *table; /* current table (stray <tr>) */

    /* figure */
    FigureCtx *figure;

    /* misc */
    int inert;
    int disabledDepth;
    int truncated;
    int detailsIndex;
    int anchorCounter;
    int figureCaptionDone;
    char *formAction; /* arena */
    char *formMethod; /* arena "get"/"post" */

    DocBlock *currentBlock;

    /* heap StrBufs owned by this walker (figure captions); freed at teardown */
    StrBuf **heapBufs;
    int heapBufCount;
    int heapBufCap;

    int error;
};

static char *doc_arena_str(Walker *w, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    size_t n = strlen(s);
    char *p = (char *)doc_arena_alloc(&w->arena, n + 1);
    if (!p)
    {
        w->error = 1;
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

static char *doc_arena_strn(Walker *w, const char *s, size_t n)
{
    char *p = (char *)doc_arena_alloc(&w->arena, n + 1);
    if (!p)
    {
        w->error = 1;
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Lowercased copy into the arena. */
static char *doc_arena_str_lower(Walker *w, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    size_t n = strlen(s);
    char *p = (char *)doc_arena_alloc(&w->arena, n + 1);
    if (!p)
    {
        w->error = 1;
        return NULL;
    }
    for (size_t i = 0; i <= n; i++)
    {
        p[i] = lua_lower(s[i]);
    }
    return p;
}

/* Lua string.gsub(s, "^(%s*).-(%s*)$" → trimmed copy). NULL → NULL. */
static char *doc_arena_trim(Walker *w, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    const char *a = s;
    const char *z = s + strlen(s);
    while (a < z && is_lspace(*a))
    {
        a++;
    }
    while (z > a && is_lspace(*(z - 1)))
    {
        z--;
    }
    return doc_arena_strn(w, a, (size_t)(z - a));
}

/* Collapse Lua %s+ runs into a single space, then trim. */
static char *doc_arena_collapse(Walker *w, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    static char tmp[1024]; /* hoisted: device gameTask stack is tiny */
    size_t o = 0;
    for (const char *p = s; *p && o + 1 < sizeof(tmp); p++)
    {
        if (is_lspace(*p))
        {
            if (o > 0 && tmp[o - 1] != ' ')
            {
                tmp[o++] = ' ';
            }
            while (is_lspace(p[1]) && o + 3 < sizeof(tmp))
            {
                p++;
            }
        }
        else
        {
            tmp[o++] = *p;
        }
    }
    while (o > 0 && is_lspace(tmp[o - 1]))
    {
        o--;
    }
    tmp[o] = '\0';
    return doc_arena_str(w, tmp);
}

/* ── Frame machinery ──────────────────────────────────────────────────────── */

static int frame_push(Walker *w, WFrame f)
{
    if (w->frameCount >= w->frameCap)
    {
        int nc = w->frameCap ? w->frameCap * 2 : 64;
        WFrame *na = (WFrame *)PLUTO_REALLOC(w->frames, (size_t)nc * sizeof(WFrame));
        if (!na)
        {
            w->error = 1;
            return -1;
        }
        w->frames = na;
        w->frameCap = nc;
    }
    w->frames[w->frameCount++] = f;
    return 0;
}

static int push_enter(Walker *w, const DomNode *node)
{
    WFrame f;
    f.kind = 0;
    f.node = node;
    f.ctx = NULL;
    f.sep = NULL;
    return frame_push(w, f);
}

/* Push an exit frame with the given kind; returns ctx (NULL on OOM). */
static ExitCtx *push_exit(Walker *w, int kind)
{
    ExitCtx *x = (ExitCtx *)doc_arena_alloc(&w->arena, sizeof(ExitCtx));
    WFrame f;
    if (!x)
    {
        w->error = 1;
        return NULL;
    }
    memset(x, 0, sizeof(*x));
    x->kind = kind;
    f.kind = 1;
    f.node = NULL;
    f.ctx = x;
    f.sep = NULL;
    if (frame_push(w, f))
    {
        return NULL;
    }
    return x;
}

static int push_sep(Walker *w, const char *s)
{
    WFrame f;
    f.kind = 2;
    f.node = NULL;
    f.ctx = NULL;
    f.sep = s;
    return frame_push(w, f);
}

/* Children enter-frames (reversed so they process in order). Push BEFORE any
 * exit frame that must run after the children (exit runs last). */
static int push_children(Walker *w, const DomNode *node)
{
    if (!node)
    {
        return 0;
    }
    for (int i = node->childCount - 1; i >= 0; i--)
    {
        if (push_enter(w, node->children[i]))
        {
            return -1;
        }
    }
    return 0;
}

/* Children except a given tag (fieldset skips legend, details skips summary). */
static int push_children_except(Walker *w, const DomNode *node, const char *tag)
{
    if (!node)
    {
        return 0;
    }
    for (int i = node->childCount - 1; i >= 0; i--)
    {
        const DomNode *c = node->children[i];
        if (c->kind == DOM_ELEMENT && c->tag && strcmp(c->tag, tag) == 0)
        {
            continue;
        }
        if (push_enter(w, c))
        {
            return -1;
        }
    }
    return 0;
}

/* ── Block/inline builders ────────────────────────────────────────────────── */

static char *resolve_href(Walker *w, const char *raw); /* used by the image handler below; defined after URL glue */

static DocBlock *new_block(Walker *w, int type)
{
    DocBlock *b = (DocBlock *)doc_arena_alloc(&w->arena, sizeof(DocBlock));
    if (!b)
    {
        w->error = 1;
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->maxlength = -1;  /* Lua-nil sentinel: absent maxlength prints nothing */
    b->fieldWidth = -1; /* Lua-nil sentinel */
    b->fieldRows = -1;  /* Lua-nil sentinel */
    return b;
}

static DocInline *new_inline(Walker *w, int type)
{
    DocInline *inl = (DocInline *)doc_arena_alloc(&w->arena, sizeof(DocInline));
    if (!inl)
    {
        w->error = 1;
        return NULL;
    }
    memset(inl, 0, sizeof(*inl));
    inl->type = type;
    return inl;
}

static int add_block(Walker *w, DocBlock *b)
{
    if (b && w->doc->blockCount < DOC_MAX_BLOCKS)
    {
        if (doc_ptrarr_push((void ***)&w->doc->blocks, &w->doc->blockCount,
                            &w->doc->blockCap, b))
        {
            w->error = 1;
            return 0;
        }
        return 1;
    }
    if (b)
    {
        w->truncated = 1;
    }
    return 0;
}

static void flush_current_block(Walker *w)
{
    if (w->currentBlock)
    {
        if (w->currentBlock->type != DOC_BLOCK_PARAGRAPH ||
            w->currentBlock->inlineCount > 0)
        {
            add_block(w, w->currentBlock);
        }
        w->currentBlock = NULL;
    }
}

static void ensure_block(Walker *w)
{
    if (!w->currentBlock)
    {
        w->currentBlock = new_block(w, DOC_BLOCK_PARAGRAPH);
    }
}

static int add_inline(Walker *w, DocInline *inl)
{
    ensure_block(w);
    if (!w->currentBlock || w->currentBlock->inlineCount >= DOC_MAX_INLINES)
    {
        return 0;
    }
    if (doc_ptrarr_push((void ***)&w->currentBlock->inlines,
                        &w->currentBlock->inlineCount,
                        &w->currentBlock->inlineCap, inl))
    {
        w->error = 1;
        return -1;
    }
    return 0;
}

static DocInline *make_text_inline(Walker *w, const char *text, size_t len)
{
    DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
    if (!inl)
    {
        return NULL;
    }
    inl->text = doc_arena_strn(w, text, len);
    inl->flags = w->flags;
    inl->anchorIndex = w->currentAnchorIndex;
    if (w->currentHref)
    {
        inl->flags |= DOC_INF_UNDERLINE;
        inl->href = doc_arena_str(w, w->currentHref);
    }
    if (w->inert > 0)
    {
        inl->flags |= DOC_INF_INERT;
    }
    return inl;
}

/* addInlineText (document.lua): collapse [\r\n\t]+ to space unless inPre;
 * skip empty / all-whitespace runs; append to linkText inside a link. */
static void add_inline_text(Walker *w, const char *raw)
{
    if (!raw || raw[0] == '\0')
    {
        return;
    }
    char tmp[512];
    const char *text = raw;
    if (!w->inPre)
    {
        size_t o = 0;
        size_t i = 0;
        while (raw[i] && o + 1 < sizeof(tmp))
        {
            if (raw[i] == '\r' || raw[i] == '\n' || raw[i] == '\t')
            {
                tmp[o++] = ' ';
                while ((raw[i + 1] == '\r' || raw[i + 1] == '\n' ||
                        raw[i + 1] == '\t') &&
                       i + 2 < sizeof(tmp))
                {
                    i++;
                }
            }
            else
            {
                tmp[o++] = raw[i];
            }
            i++;
        }
        tmp[o] = '\0';
        text = tmp;
    }
    if (text[0] == '\0')
    {
        return;
    }
    {
        const char *p;
        int allSpace = 1;
        for (p = text; *p; p++)
        {
            if (!is_lspace(*p))
            {
                allSpace = 0;
                break;
            }
        }
        if (allSpace)
        {
            return;
        }
    }
    if (w->currentHref)
    {
        if (strbuf_append(&w->linkText, text))
        {
            w->error = 1;
            return;
        }
    }
    DocInline *inl = make_text_inline(w, text, strlen(text));
    if (inl)
    {
        add_inline(w, inl);
    }
}

/* ── Style helpers on DomAttr lists (small stack; walker is sequential) ──── */

static const char *walk_align(Walker *w, AttrList a)
{
    const char *al = attr_val(a, "align");
    const char *styleStr = attr_val(a, "style");
    static DocStyle st; /* static: sees one style map at a time (walker is sequential) */
    doc_parse_style(styleStr, &st);
    const char *ta = style_get(&st, "text-align");
    if (ta)
    {
        al = ta;
    }
    if (!al)
    {
        return NULL;
    }
    static char lbuf[16];
    lua_lower_buf(lbuf, al, sizeof(lbuf));
    if (strcmp(lbuf, "center") == 0 || strcmp(lbuf, "right") == 0 ||
        strcmp(lbuf, "left") == 0)
    {
        return doc_arena_str(w, lbuf);
    }
    return NULL;
}

static int walk_display_none(AttrList a)
{
    if (a.count == 0)
    {
        return 0;
    }
    if (attr_has(a, "hidden") || attr_has(a, "popover"))
    {
        return 1;
    }
    const char *styleStr = attr_val(a, "style");
    static DocStyle st;
    doc_parse_style(styleStr, &st);
    const char *d = style_get(&st, "display");
    if (d && strstr(d, "none"))
    {
        return 1;
    }
    const char *v = style_get(&st, "visibility");
    if (v && strstr(v, "hidden"))
    {
        return 1;
    }
    return 0;
}

static int walk_inverted(AttrList a)
{
    if (a.count == 0)
    {
        return 0;
    }
    const char *styleStr = attr_val(a, "style");
    static DocStyle st;
    doc_parse_style(styleStr, &st);
    const char *c = style_get(&st, "color");
    if (c)
    {
        if (strstr(c, "white") || strstr(c, "#fff") ||
            (strncmp(c, "#ffff", 5) == 0 && c[5] != '\0'))
        {
            return 1;
        }
    }
    const char *bg = style_get(&st, "background-color");
    if (!bg)
    {
        bg = style_get(&st, "background");
    }
    if (bg && (strstr(bg, "black") || strstr(bg, "#000")))
    {
        return 1;
    }
    return 0;
}

static DocBoxSpacing walk_box_spacing(AttrList a)
{
    DocBoxSpacing out = {0, 0, 0, 0};
    if (a.count == 0)
    {
        return out;
    }
    const char *styleStr = attr_val(a, "style");
    static DocStyle st;
    doc_parse_style(styleStr, &st);
    const char *m = style_get(&st, "margin");
    if (m)
    {
        char parts[4][32];
        int n = split_parts(m, parts);
        int t = 0, r = 0, b = 0, l = 0;
        if (n == 1)
        {
            t = r = b = l = box_num(parts[0]);
        }
        else if (n == 2)
        {
            t = b = box_num(parts[0]);
            r = l = box_num(parts[1]);
        }
        else if (n == 3)
        {
            t = box_num(parts[0]);
            r = l = box_num(parts[1]);
            b = box_num(parts[2]);
        }
        else if (n == 4)
        {
            t = box_num(parts[0]);
            r = box_num(parts[1]);
            b = box_num(parts[2]);
            l = box_num(parts[3]);
        }
        out.top = t;
        out.bottom = b;
        out.left = l;
        out.right = r;
    }
    else
    {
        const char *mt = style_get(&st, "margin-top");
        if (mt)
        {
            out.top = box_num(mt);
        }
        const char *mb = style_get(&st, "margin-bottom");
        if (mb)
        {
            out.bottom = box_num(mb);
        }
        const char *ml = style_get(&st, "margin-left");
        if (ml)
        {
            out.left = box_num(ml);
        }
        const char *mr = style_get(&st, "margin-right");
        if (mr)
        {
            out.right = box_num(mr);
        }
    }
    const char *p = style_get(&st, "padding");
    if (p)
    {
        char parts[4][32];
        int n = split_parts(p, parts);
        if (n == 1)
        {
            out.left += box_num(parts[0]);
        }
        else if (n == 2)
        {
            out.left += box_num(parts[1]);
        }
        else if (n == 4)
        {
            out.left += box_num(parts[3]);
        }
    }
    else
    {
        const char *pl = style_get(&st, "padding-left");
        if (pl)
        {
            out.left += box_num(pl);
        }
    }
    return out;
}

/* ── Text node ─────────────────────────────────────────────────────────────── */

static void handle_text_node(Walker *w, const DomNode *node)
{
    const char *text = node->text ? node->text : "";
    if (w->inPre)
    {
        if (strbuf_append(&w->preBuf, text))
        {
            w->error = 1;
        }
    }
    else if (w->inTextarea)
    {
        if (strbuf_append(&w->textareaBuf, text))
        {
            w->error = 1;
        }
    }
    else if (w->cell)
    {
        static char tmp[1024]; /* hoisted: device gameTask stack is tiny */
        const char *txt = text;
        size_t o = 0;
        if (w->cellFirstText)
        {
            w->cellFirstText = 0;
            const char *p = txt;
            while (*p && is_lspace(*p))
            {
                p++;
            }
            txt = p;
        }
        if (txt[0] != '\0')
        {
            for (const char *p = txt; *p && o + 1 < sizeof(tmp); p++)
            {
                if (*p == '\r' || *p == '\n' || *p == '\t')
                {
                    tmp[o++] = ' ';
                    while ((p[1] == '\r' || p[1] == '\n' || p[1] == '\t') &&
                           o + 1 < sizeof(tmp))
                    {
                        p++;
                    }
                }
                else
                {
                    tmp[o++] = *p;
                }
            }
            tmp[o] = '\0';
            if (tmp[0] != '\0')
            {
                DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
                if (inl)
                {
                    inl->text = doc_arena_str(w, tmp);
                    inl->flags = w->flags & (DOC_INF_BOLD | DOC_INF_ITALIC |
                                             DOC_INF_CODE | DOC_INF_INVERT);
                    if (w->currentHref)
                    {
                        inl->flags |= DOC_INF_UNDERLINE;
                        inl->href = doc_arena_str(w, w->currentHref);
                    }
                    inl->anchorIndex = w->currentAnchorIndex;
                    if (w->inert > 0)
                    {
                        inl->flags |= DOC_INF_INERT;
                    }
                    if (doc_ptrarr_push((void ***)&w->cell->inlines,
                                        &w->cell->inlineCount,
                                        &w->cell->inlineCap, inl))
                    {
                        w->error = 1;
                    }
                    if (w->currentHref)
                    {
                        if (strbuf_append(&w->linkText, tmp))
                        {
                            w->error = 1;
                        }
                    }
                }
            }
        }
    }
    else if (w->figure && !w->figureCaptionDone)
    {
        if (strbuf_append(w->figure->cap, text))
        {
            w->error = 1;
        }
    }
    else if (w->inMath)
    {
        if (strbuf_append(&w->mathBuf, text))
        {
            w->error = 1;
        }
    }
    else
    {
        add_inline_text(w, text);
    }
}

/* ── Image ─────────────────────────────────────────────────────────────────── */

static void handle_image(Walker *w, AttrList a)
{
    const char *src = attr_val(a, "src");
    if (!src)
    {
        src = attr_val(a, "data-src");
    }
    if (!src)
    {
        src = "";
    }
    if (src[0] == '\0')
    {
        const char *ss = attr_val(a, "srcset");
        if (ss)
        {
            size_t n = 0;
            while (ss[n] && ss[n] != ' ' && ss[n] != '\t' && ss[n] != '\n' &&
                   ss[n] != ',' && ss[n] != '\r')
            {
                n++;
            }
            src = n ? doc_arena_strn(w, ss, n) : "";
        }
    }
    const char *alt = attr_val(a, "alt");
    if (!alt)
    {
        alt = attr_val(a, "title");
    }
    if (!alt)
    {
        alt = "Image";
    }
    double wd = strict_num(attr_val(a, "width"), 160);
    double ht = strict_num(attr_val(a, "height"), 80);
    if (wd <= 0)
    {
        wd = 160;
    }
    if (ht <= 0)
    {
        ht = 80;
    }
    if (src[0] != '\0' && !strstr(src, "tracking") && !strstr(src, "beacon"))
    {
        if (wd > 360)
        {
            wd = 360;
        }
        if (ht > 180)
        {
            ht = 180;
        }
        const char *usemap = attr_or_d(a, "usemap", "");
        if (usemap[0] == '#')
        {
            usemap++;
        }
        DocBlock *img = new_block(w, DOC_BLOCK_IMAGE);
        if (img)
        {
            img->src = resolve_href(w, src); /* Lua: URL.resolve(baseUrl, src) */
            img->alt = doc_arena_str(w, alt);
            img->width = wd;
            img->height = ht;
            img->align = walk_align(w, a); /* Lua: align = parseAlign(attrs) */
            img->usemap = doc_arena_str(w, usemap);
            img->href = w->currentHref ? doc_arena_str(w, w->currentHref) : NULL;
            img->inert = (w->inert > 0);
            if (w->figure)
            {
                w->figure->image = img;
            }
            else
            {
                flush_current_block(w);
                add_block(w, img);
            }
        }
    }
}

/* ── Select options (collectSelectOptions, iterative) ─────────────────────── */

static void collect_options(Walker *w, const DomNode *node, DocBlock *into)
{
    typedef struct
    {
        const DomNode *n;
        int phase; /* 0 = scan, 1 = post (insert group) */
        int lenBefore;
        const char *groupLabel;
    } OF;
    OF *stack;
    int sc = 0, scap = 16;
    stack = (OF *)PLUTO_MALLOC(sizeof(OF) * scap);
    if (!stack)
    {
        w->error = 1;
        return;
    }
    stack[sc].n = node;
    stack[sc].phase = 0;
    stack[sc].lenBefore = 0;
    stack[sc].groupLabel = NULL;
    sc++;
    while (sc > 0)
    {
        OF top = stack[--sc];
        if (top.phase == 1)
        {
            if (top.groupLabel && top.groupLabel[0] != '\0')
            {
                DocOption *go = (DocOption *)doc_arena_alloc(&w->arena, sizeof(DocOption));
                if (go)
                {
                    memset(go, 0, sizeof(*go));
                    go->text = doc_arena_str(w, top.groupLabel);
                    go->value = doc_arena_str(w, "");
                    go->group = 1;
                    go->disabled = 1;
                    /* Lua table.insert(out, childrenBefore + 1, group) — 1-based,
                     * i.e. 0-based index == lenBefore. Append then shift right. */
                    int at = top.lenBefore;
                    if (doc_ptrarr_push((void ***)&into->options,
                                        &into->optionCount, &into->optionCap, go))
                    {
                        w->error = 1;
                    }
                    else
                    {
                        for (int i = into->optionCount - 1; i > at; i--)
                        {
                            into->options[i] = into->options[i - 1];
                        }
                        into->options[at] = go;
                    }
                }
            }
            continue;
        }
        for (int i = 0; i < top.n->childCount; i++)
        {
            const DomNode *c = top.n->children[i];
            if (c->kind != DOM_ELEMENT)
            {
                continue;
            }
            if (strcmp(c->tag, "option") == 0)
            {
                char txtbuf[512];
                size_t need = doc_concat_node_text(c, txtbuf, sizeof(txtbuf));
                (void)need;
                char *t = doc_arena_collapse(w, txtbuf);
                AttrList ca = attrs_of(c);
                const char *label = attr_val(ca, "label");
                if (label && label[0] != '\0')
                {
                    t = doc_arena_str(w, label);
                }
                const char *val = attr_or_d(ca, "value", t ? t : "");
                DocOption *o = (DocOption *)doc_arena_alloc(&w->arena, sizeof(DocOption));
                if (o)
                {
                    memset(o, 0, sizeof(*o));
                    o->text = t;
                    o->value = doc_arena_str(w, val);
                    o->selected = attr_has(ca, "selected");
                    o->disabled = attr_has(ca, "disabled");
                    if (doc_ptrarr_push((void ***)&into->options,
                                        &into->optionCount, &into->optionCap, o))
                    {
                        w->error = 1;
                    }
                }
            }
            else if (strcmp(c->tag, "optgroup") == 0)
            {
                AttrList ca = attrs_of(c);
                const char *gl = attr_val(ca, "label");
                OF post;
                post.n = c;
                post.phase = 1;
                post.lenBefore = into->optionCount;
                post.groupLabel = gl ? doc_arena_str(w, gl) : doc_arena_str(w, "");
                OF scan;
                scan.n = c;
                scan.phase = 0;
                scan.lenBefore = 0;
                scan.groupLabel = NULL;
                /* Push the post frame first so the scan frame (on top) is
                 * processed before the group label is inserted. */
                if (sc + 2 > scap)
                {
                    int nc = scap * 2 + 2;
                    OF *na = (OF *)PLUTO_REALLOC(stack, sizeof(OF) * nc);
                    if (!na)
                    {
                        w->error = 1;
                        break;
                    }
                    stack = na;
                    scap = nc;
                }
                stack[sc++] = post;
                stack[sc++] = scan;
            }
        }
    }
    if (stack)
    {
        PLUTO_FREE(stack);
    }
}

/* ── Forward declarations ──────────────────────────────────────────────────── */

static void handle_element(Walker *w, const DomNode *node);
static void run_exit(Walker *w, ExitCtx *x);
static int walk_children(Walker *w, const DomNode *parent);
static void handle_row(Walker *w, const DomNode *trNode, DocTable *tbl);
static int refresh_from_content(DocParseResult *out, const char *content,
                                const char *baseUrl);

/* ── Small helpers ─────────────────────────────────────────────────────────── */

/* Lua tonumber with an ok flag (distinguishes nil from 0). */
static double tonum_or(const char *s, int *ok)
{
    *ok = 0;
    if (!s)
    {
        return 0;
    }
    char *end;
    double d = strtod(s, &end);
    if (end == s)
    {
        return 0;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
    {
        end++;
    }
    if (*end != '\0')
    {
        return 0;
    }
    *ok = 1;
    return d;
}

/* URL.resolve into the arena (NULL when resolve fails). */
static char *resolve_href(Walker *w, const char *raw)
{
    if (!raw)
    {
        return NULL;
    }
    char *res = url_resolve(w->doc->baseUrl, raw);
    if (!res)
    {
        return NULL;
    }
    char *a = doc_arena_str(w, res);
    PLUTO_FREE(res);
    return a;
}

static DocBlock *box_open_block(Walker *w, const char *label)
{
    DocBlock *b = new_block(w, DOC_BLOCK_BOX_OPEN);
    if (b)
    {
        b->label = doc_arena_str(w, label);
    }
    return b;
}

/* qQuote (document.lua): literal '"' inline; cell variant carries href. */
static void q_quote(Walker *w)
{
    DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
    if (!inl)
    {
        return;
    }
    inl->text = doc_arena_str(w, "\"");
    inl->flags = w->flags & (DOC_INF_BOLD | DOC_INF_ITALIC);
    if (w->cell)
    {
        if (w->currentHref)
        {
            inl->href = doc_arena_str(w, w->currentHref);
            inl->anchorIndex = w->currentAnchorIndex;
        }
        if (doc_ptrarr_push((void ***)&w->cell->inlines, &w->cell->inlineCount,
                            &w->cell->inlineCap, inl))
        {
            w->error = 1;
        }
    }
    else
    {
        add_inline(w, inl);
    }
}

/* First element child with the given tag (legend/summary lookup). */
static const DomNode *first_child_tag(const DomNode *node, const char *tag)
{
    if (!node)
    {
        return NULL;
    }
    for (int i = 0; i < node->childCount; i++)
    {
        const DomNode *c = node->children[i];
        if (c->kind == DOM_ELEMENT && c->tag && strcmp(c->tag, tag) == 0)
        {
            return c;
        }
    }
    return NULL;
}

/* ── handleElement — the full dispatch (document.lua ~530–1395) ───────────── */

static void handle_element(Walker *w, const DomNode *node)
{
    const char *tag = node->tag ? node->tag : "";
    AttrList a = attrs_of(node);

    /* ── Flow containers / paragraph-like blocks ── */
    if ((tag[0] == 'h') && (tag[1] >= '1' && tag[1] <= '6') && tag[2] == '\0')
    {
        flush_current_block(w);
        int level = tag[1] - '0';
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        DocBoxSpacing sp = walk_box_spacing(a);
        DocBlock *b = new_block(w, DOC_BLOCK_HEADING);
        if (!b)
        {
            return;
        }
        b->level = level;
        b->align = walk_align(w, a);
        b->spacingTop = sp.top;
        b->spacingBottom = sp.bottom;
        b->indent = sp.left;
        b->hasSpacing = 1;
        b->invert = walk_inverted(a);
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }
    else if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
             strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
             strcmp(tag, "main") == 0 || strcmp(tag, "header") == 0 ||
             strcmp(tag, "footer") == 0 || strcmp(tag, "nav") == 0 ||
             strcmp(tag, "aside") == 0 || strcmp(tag, "address") == 0 ||
             strcmp(tag, "hgroup") == 0 || strcmp(tag, "noindex") == 0 ||
             strcmp(tag, "search") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        flush_current_block(w);
        DocBoxSpacing sp = walk_box_spacing(a);
        DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
        if (!b)
        {
            return;
        }
        b->align = walk_align(w, a);
        b->spacingTop = sp.top;
        b->spacingBottom = sp.bottom;
        b->indent = sp.left;
        b->hasSpacing = 1;
        b->invert = walk_inverted(a);
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }
    else if (strcmp(tag, "blockquote") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        flush_current_block(w);
        DocBoxSpacing sp = walk_box_spacing(a);
        DocBlock *b = new_block(w, DOC_BLOCK_BLOCKQUOTE);
        if (!b)
        {
            return;
        }
        b->align = walk_align(w, a);
        b->spacingTop = sp.top;
        b->spacingBottom = sp.bottom;
        b->indent = sp.left + 12;
        b->hasSpacing = 1;
        b->invert = walk_inverted(a);
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }
    else if (strcmp(tag, "center") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        flush_current_block(w);
        DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
        if (!b)
        {
            return;
        }
        b->align = doc_arena_str(w, "center");
        b->hasSpacing = 1; /* center/marquee: spacing keys present, all 0 */
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }
    else if (strcmp(tag, "marquee") == 0)
    {
        flush_current_block(w);
        DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
        if (!b)
        {
            return;
        }
        b->align = doc_arena_str(w, "center");
        b->hasSpacing = 1; /* spacing keys present, all 0 */
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }

    /* ── Line breaks & rules ── */
    else if (strcmp(tag, "br") == 0)
    {
        if (w->inPre)
        {
            if (strbuf_append(&w->preBuf, "\n"))
            {
                w->error = 1;
            }
        }
        else if (w->cell)
        {
            DocInline *inl = new_inline(w, DOC_INLINE_BR);
            if (inl)
            {
                if (doc_ptrarr_push((void ***)&w->cell->inlines,
                                    &w->cell->inlineCount, &w->cell->inlineCap, inl))
                {
                    w->error = 1;
                }
            }
        }
        else
        {
            DocInline *inl = new_inline(w, DOC_INLINE_BR);
            if (inl)
            {
                add_inline(w, inl);
            }
        }
    }
    else if (strcmp(tag, "wbr") == 0)
    {
        DocInline *inl = new_inline(w, DOC_INLINE_WBR);
        if (inl)
        {
            add_inline(w, inl);
        }
    }
    else if (strcmp(tag, "hr") == 0)
    {
        if (!w->cell)
        {
            flush_current_block(w);
            add_block(w, new_block(w, DOC_BLOCK_HR));
        }
    }

    /* ── Preformatted text & inline code ── */
    else if (strcmp(tag, "pre") == 0 || strcmp(tag, "xmp") == 0 ||
             strcmp(tag, "listing") == 0 || strcmp(tag, "plaintext") == 0)
    {
        flush_current_block(w);
        if (strcmp(tag, "pre") == 0)
        {
            ExitCtx *x = push_exit(w, WX_PRE_END);
            (void)x;
            push_children(w, node);
            w->inPre = 1;
            strbuf_reset(&w->preBuf);
        }
        else
        {
            /* Legacy raw-text blocks: concatenate all descendant text. */
            static char big[2048]; /* hoisted: device gameTask stack is tiny */
            doc_concat_node_text(node, big, sizeof(big));
            w->inPre = 1;
            strbuf_reset(&w->preBuf);
            if (strbuf_append(&w->preBuf, big))
            {
                w->error = 1;
            }
            w->inPre = 0;
            /* Build the code_block immediately (reference does the same). */
            ExitCtx x;
            memset(&x, 0, sizeof(x));
            x.kind = WX_PRE_END;
            run_exit(w, &x);
        }
    }

    /* ── Script fallback / inert containers ── */
    else if (strcmp(tag, "noscript") == 0 || strcmp(tag, "noembed") == 0 ||
             strcmp(tag, "noframes") == 0 || strcmp(tag, "slot") == 0)
    {
        push_children(w, node);
    }

    /* ── Inline formatting ── */
    else if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_BOLD;
    }
    else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0 ||
             strcmp(tag, "cite") == 0 || strcmp(tag, "var") == 0 ||
             strcmp(tag, "dfn") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_ITALIC;
    }
    else if (strcmp(tag, "u") == 0 || strcmp(tag, "ins") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_UNDERLINE;
    }
    else if (strcmp(tag, "code") == 0 || strcmp(tag, "kbd") == 0 ||
             strcmp(tag, "samp") == 0 || strcmp(tag, "tt") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_CODE;
    }
    else if (strcmp(tag, "mark") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_MARK | DOC_INF_BOLD;
    }
    else if (strcmp(tag, "small") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_SMALL;
    }
    else if (strcmp(tag, "big") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_BIG;
    }
    else if (strcmp(tag, "sub") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_SUB | DOC_INF_SMALL;
    }
    else if (strcmp(tag, "sup") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_SUP | DOC_INF_SMALL;
    }
    else if (strcmp(tag, "del") == 0 || strcmp(tag, "s") == 0 ||
             strcmp(tag, "strike") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_STRIKE;
    }
    else if (strcmp(tag, "q") == 0)
    {
        push_exit(w, WX_QUOTE_END);
        push_children(w, node);
        q_quote(w);
    }
    else if (strcmp(tag, "abbr") == 0 || strcmp(tag, "acronym") == 0)
    {
        push_children(w, node);
    }

    /* Elements that carry style attributes */
    else if (strcmp(tag, "span") == 0 || strcmp(tag, "font") == 0 ||
             strcmp(tag, "time") == 0 || strcmp(tag, "data") == 0 ||
             strcmp(tag, "bdi") == 0 || strcmp(tag, "bdo") == 0 ||
             strcmp(tag, "label") == 0 || strcmp(tag, "output") == 0 ||
             strcmp(tag, "legend") == 0 || strcmp(tag, "summary") == 0)
    {
        ExitCtx *x = push_exit(w, WX_SPAN_END);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        const char *styleStr = attr_val(a, "style");
        static DocStyle st;
        doc_parse_style(styleStr, &st);
        const char *fw = style_get(&st, "font-weight");
        if (fw && (strcmp(fw, "bold") == 0 || strcmp(fw, "bolder") == 0 ||
                   strcmp(fw, "700") == 0))
        {
            w->flags |= DOC_INF_BOLD;
        }
        const char *fs = style_get(&st, "font-style");
        if (fs && strcmp(fs, "italic") == 0)
        {
            w->flags |= DOC_INF_ITALIC;
        }
        const char *td = style_get(&st, "text-decoration");
        if (td)
        {
            if (strstr(td, "underline"))
            {
                w->flags |= DOC_INF_UNDERLINE;
            }
            if (strstr(td, "line-through"))
            {
                w->flags |= DOC_INF_STRIKE;
            }
        }
        if (strcmp(tag, "font") == 0)
        {
            const char *sz = attr_val(a, "size");
            if (sz)
            {
                int isn;
                double n = tonum_or(sz, &isn);
                if (strcmp(sz, "+1") == 0 || (isn && n >= 5))
                {
                    w->flags |= DOC_INF_BIG;
                }
                else if (strcmp(sz, "-1") == 0 || strcmp(sz, "-2") == 0 ||
                         (isn && n <= 3))
                {
                    w->flags |= DOC_INF_SMALL;
                }
            }
        }
        if (walk_inverted(a))
        {
            w->flags |= DOC_INF_INVERT;
        }
        if (x)
        {
            x->hadInlines = w->currentBlock ? w->currentBlock->inlineCount : 0;
            const char *fb = NULL;
            if (strcmp(tag, "time") == 0)
            {
                fb = attr_val(a, "datetime");
            }
            else if (strcmp(tag, "data") == 0)
            {
                fb = attr_val(a, "value");
            }
            x->fallback = fb ? doc_arena_str(w, fb) : NULL;
        }
    }
    else if (strcmp(tag, "ruby") == 0 || strcmp(tag, "rp") == 0 ||
             strcmp(tag, "rb") == 0 || strcmp(tag, "rtc") == 0)
    {
        push_children(w, node);
    }
    else if (strcmp(tag, "rt") == 0)
    {
        ExitCtx *x = push_exit(w, WX_STYLE);
        if (x)
        {
            x->flags = w->flags;
        }
        push_children(w, node);
        w->flags |= DOC_INF_SMALL;
    }

    /* ── Links ── */
    else if (strcmp(tag, "a") == 0)
    {
        ExitCtx *x = push_exit(w, WX_LINK_END);
        if (x)
        {
            const char *ti = attr_val(a, "title");
            const char *tg = attr_val(a, "target");
            x->title = ti ? doc_arena_str(w, ti) : NULL;
            x->target = tg ? doc_arena_str(w, tg) : NULL;
        }
        push_children(w, node);
        const char *rawHref = attr_val(a, "href");
        if (doc_valid_href(rawHref))
        {
            w->currentHref = resolve_href(w, rawHref);
            w->anchorCounter++;
            w->currentAnchorIndex = w->anchorCounter;
            strbuf_reset(&w->linkText);
        }
    }

    /* ── Lists ── */
    else if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0 ||
             strcmp(tag, "menu") == 0 || strcmp(tag, "dir") == 0)
    {
        ExitCtx *x = push_exit(w, WX_LIST_END);
        if (x)
        {
            x->listCtx = w->listCtx;
        }
        push_children(w, node);
        ListCtx *nc = (ListCtx *)doc_arena_alloc(&w->arena, sizeof(ListCtx));
        if (!nc)
        {
            w->error = 1;
            return;
        }
        int ordered = (strcmp(tag, "ol") == 0);
        nc->ordered = ordered;
        nc->count = 0;
        nc->depth = (w->listCtx ? w->listCtx->depth : 0) + 1;
        if (ordered)
        {
            nc->start = (int)strict_num(attr_val(a, "start"), 1);
            nc->reversed = attr_has(a, "reversed");
            const char *t = attr_or_d(a, "type", "1");
            char mt = t[0];
            if (!(mt == 'a' || mt == 'A' || mt == 'i' || mt == 'I' || mt == '1') ||
                t[1] != '\0')
            {
                mt = '1';
            }
            nc->markerType = mt;
        }
        else
        {
            nc->start = 1;
            nc->reversed = 0;
            nc->markerType = '1';
        }
        w->listCtx = nc;
    }
    else if (strcmp(tag, "li") == 0)
    {
        flush_current_block(w);
        ListCtx *ctx = w->listCtx;
        ListCtx tmp;
        int haveCtx = 1;
        if (!ctx)
        {
            /* Reference fallback: { ordered = false, count = 0, depth = 1 } —
             * it has NO start field. With a value attr the or-1 fallback
             * applies; without one the reference THROWS (documented quirk). */
            memset(&tmp, 0, sizeof(tmp));
            tmp.ordered = 0;
            tmp.count = 0;
            tmp.depth = 1;
            ctx = &tmp;
            haveCtx = 0;
        }
        double number;
        if (attr_has(a, "value"))
        {
            /* Lua: tonumber(v) or (ctx.start or 1) — 0 is truthy, so the
             * fallback is 1 only when there is NO list context (nil start). */
            number = strict_num(attr_val(a, "value"),
                                haveCtx ? (double)ctx->start : 1.0);
            ctx->start = (ctx->reversed) ? (int)(number - 1) : (int)(number + 1);
            ctx->count = 0;
        }
        else
        {
            if (!haveCtx)
            {
                /* Mirror the reference's arithmetic-on-nil error. */
                w->error = 1;
                return;
            }
            ctx->count++;
            number = ctx->reversed ? (double)(ctx->start - (ctx->count - 1))
                                   : (double)(ctx->start + ctx->count - 1);
        }
        DocBlock *b = new_block(w, DOC_BLOCK_LIST_ITEM);
        if (!b)
        {
            return;
        }
        b->isOrdered = ctx->ordered;
        b->hasNumber = 1;
        b->number = (int)number;
        b->markerType = ctx->markerType ? ctx->markerType : '1';
        b->depth = ctx->depth;
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }
    else if (strcmp(tag, "dl") == 0)
    {
        ExitCtx *x = push_exit(w, WX_DL_END);
        if (x)
        {
            x->dlDepth = w->dlDepth;
        }
        push_children(w, node);
        w->dlDepth++;
    }
    else if (strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0)
    {
        flush_current_block(w);
        DocBlock *b = new_block(w, DOC_BLOCK_LIST_ITEM);
        if (!b)
        {
            return;
        }
        if (strcmp(tag, "dt") == 0)
        {
            b->dt = 1;
        }
        else
        {
            b->dd = 1;
        }
        b->depth = w->dlDepth;
        w->currentBlock = b;
        push_exit(w, WX_FLUSH);
        push_children(w, node);
    }

    /* ── Images & figures ── */
    else if (strcmp(tag, "img") == 0)
    {
        handle_image(w, a);
    }
    else if (strcmp(tag, "picture") == 0)
    {
        push_children(w, node);
    }
    else if (strcmp(tag, "figure") == 0)
    {
        flush_current_block(w);
        ExitCtx *x = push_exit(w, WX_FIGURE_END);
        if (x)
        {
            x->figure = w->figure;
        }
        push_children(w, node);
        FigureCtx *f = (FigureCtx *)doc_arena_alloc(&w->arena, sizeof(FigureCtx));
        if (!f)
        {
            w->error = 1;
            return;
        }
        memset(f, 0, sizeof(*f));
        StrBuf *cap = (StrBuf *)PLUTO_MALLOC(sizeof(StrBuf));
        if (!cap || strbuf_init(cap))
        {
            w->error = 1;
            return;
        }
        if (doc_ptrarr_push((void ***)&w->heapBufs, &w->heapBufCount,
                            &w->heapBufCap, cap))
        {
            w->error = 1;
            return;
        }
        f->cap = cap;
        w->figure = f;
        w->figureCaptionDone = 1;
    }
    else if (strcmp(tag, "figcaption") == 0)
    {
        push_exit(w, WX_FIGCAP_END);
        push_children(w, node);
        w->figureCaptionDone = 0;
    }

    /* ── Tables ── */
    else if (strcmp(tag, "table") == 0)
    {
        flush_current_block(w);
        if (w->cell)
        {
            return;
        }
        DocTable *tbl = (DocTable *)doc_arena_alloc(&w->arena, sizeof(DocTable));
        if (!tbl)
        {
            w->error = 1;
            return;
        }
        memset(tbl, 0, sizeof(*tbl));
        tbl->caption = doc_arena_str(w, "");
        tbl->align = walk_align(w, a);
        const char *bord = attr_val(a, "border");
        tbl->border = (bord != NULL && strcmp(bord, "0") != 0);
        const char *tw = attr_val(a, "width");
        tbl->width = tw ? doc_arena_str(w, tw) : NULL;
        w->table = tbl;
        for (int i = 0; i < node->childCount; i++)
        {
            const DomNode *c = node->children[i];
            if (c->kind != DOM_ELEMENT)
            {
                continue;
            }
            if (strcmp(c->tag, "caption") == 0)
            {
                static char cbuf[1024]; /* hoisted: device gameTask stack is tiny */
                doc_concat_node_text(c, cbuf, sizeof(cbuf));
                tbl->caption = doc_arena_collapse(w, cbuf);
            }
            else if (strcmp(c->tag, "tr") == 0)
            {
                handle_row(w, c, tbl);
            }
            else if (strcmp(c->tag, "thead") == 0 || strcmp(c->tag, "tbody") == 0 ||
                     strcmp(c->tag, "tfoot") == 0)
            {
                for (int j = 0; j < c->childCount; j++)
                {
                    const DomNode *r = c->children[j];
                    if (r->kind == DOM_ELEMENT && strcmp(r->tag, "tr") == 0)
                    {
                        handle_row(w, r, tbl);
                    }
                }
            }
        }
        w->table = NULL;
        if (tbl->rowCount > 0)
        {
            DocBlock *tb = new_block(w, DOC_BLOCK_TABLE);
            if (tb)
            {
                tb->table = tbl;
                tb->align = tbl->align; /* Lua block IS tbl: align key lives on it */
                add_block(w, tb);
            }
        }
    }
    else if (strcmp(tag, "tr") == 0)
    {
        if (w->table)
        {
            handle_row(w, node, w->table);
        }
    }
    else if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0)
    {
        /* Cells are processed by handleRow; stray cells are ignored. */
    }

    /* ── Forms ── */
    else if (strcmp(tag, "form") == 0)
    {
        ExitCtx *x = push_exit(w, WX_FORM_END);
        if (x)
        {
            x->formAction = w->formAction;
            x->formMethod = w->formMethod;
        }
        push_children(w, node);
        const char *act = attr_val(a, "action");
        w->formAction = resolve_href(w, act ? act : "");
        const char *mth = attr_val(a, "method");
        char mbuf[16];
        lua_lower_buf(mbuf, mth ? mth : "get", sizeof(mbuf));
        w->formMethod = doc_arena_str(w, mbuf);
    }
    else if (strcmp(tag, "input") == 0)
    {
        char tbuf[32];
        lua_lower_buf(tbuf, attr_or_d(a, "type", "text"), sizeof(tbuf));
        const char *inputName = attr_or_d(a, "name", "q");
        const char *inputVal = attr_or_d(a, "value", "");
        const char *ph = attr_val(a, "placeholder");
        if (!ph)
        {
            ph = attr_val(a, "aria-label");
        }
        if (!ph)
        {
            ph = "";
        }
        int isChecked = attr_has(a, "checked");
        int disabled = attr_has(a, "disabled") || (w->disabledDepth > 0);
        int readonly = attr_has(a, "readonly") || disabled;
        int required = attr_has(a, "required");
        int hasMax;
        double maxlen = tonum_or(attr_val(a, "maxlength"), &hasMax);
        int hasSize;
        double size = tonum_or(attr_val(a, "size"), &hasSize);
        const char *formAction = w->formAction;
        const char *formMethod = w->formMethod;
        if (attr_val(a, "formaction"))
        {
            formAction = resolve_href(w, attr_val(a, "formaction"));
        }
        if (attr_val(a, "formmethod"))
        {
            char fm[16];
            lua_lower_buf(fm, attr_val(a, "formmethod"), sizeof(fm));
            formMethod = doc_arena_str(w, fm);
        }

#define P19_COMMON(b)                                    \
    do                                                   \
    {                                                    \
        (b)->formAction = doc_arena_str(w, formAction ? formAction : "");  \
        (b)->formMethod = doc_arena_str(w, formMethod ? formMethod : "get"); \
        (b)->disabled = disabled;                        \
        (b)->readonly = readonly;                        \
        (b)->required = required;                        \
        (b)->maxlength = hasMax ? (int)maxlen : -1;      \
        (b)->inert = (w->inert > 0) || disabled;         \
    } while (0)

        if (strcmp(tbuf, "hidden") == 0)
        {
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_HIDDEN_FIELD);
            if (b)
            {
                b->name = doc_arena_str(w, inputName);
                b->value = doc_arena_str(w, inputVal);
                P19_COMMON(b);
                add_block(w, b);
            }
            return;
        }
        else if (strcmp(tbuf, "checkbox") == 0 || strcmp(tbuf, "radio") == 0)
        {
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_CHECKBOX_FIELD);
            if (b)
            {
                b->radio = (strcmp(tbuf, "radio") == 0);
                b->checked = isChecked;
                b->name = doc_arena_str(w, inputName);
                b->value = doc_arena_str(w, inputVal);
                const char *lb = attr_val(a, "label");
                if (!lb)
                {
                    lb = attr_val(a, "title");
                }
                if (!lb)
                {
                    lb = ph; /* Lua: placeholder or inputName — but placeholder
                              * defaults to "" which is truthy, so inputName is
                              * unreachable; the "" is kept. */
                }
                b->label = doc_arena_str(w, lb);
                P19_COMMON(b);
                add_block(w, b);
            }
        }
        else if (strcmp(tbuf, "text") == 0 || strcmp(tbuf, "search") == 0 ||
                 strcmp(tbuf, "email") == 0 || strcmp(tbuf, "url") == 0 ||
                 strcmp(tbuf, "number") == 0 || strcmp(tbuf, "password") == 0 ||
                 strcmp(tbuf, "tel") == 0 || strcmp(tbuf, "date") == 0 ||
                 strcmp(tbuf, "time") == 0 || strcmp(tbuf, "month") == 0 ||
                 strcmp(tbuf, "week") == 0 || strcmp(tbuf, "datetime-local") == 0 ||
                 strcmp(tbuf, "color") == 0)
        {
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_INPUT_FIELD);
            if (b)
            {
                b->inputType = doc_arena_str(w, tbuf);
                b->name = doc_arena_str(w, inputName);
                b->value = doc_arena_str(w, inputVal);
                b->placeholder = doc_arena_str(w, ph);
                b->fieldWidth = hasSize ? (int)size : -1;
                P19_COMMON(b);
                add_block(w, b);
            }
        }
        else if (strcmp(tbuf, "submit") == 0 || strcmp(tbuf, "button") == 0)
        {
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_INPUT_SUBMIT);
            if (b)
            {
                b->name = doc_arena_str(w, inputName);
                b->value = doc_arena_str(w, inputVal);
                const char *lb;
                if (inputVal[0] != '\0')
                {
                    lb = inputVal;
                }
                else
                {
                    lb = (strcmp(tbuf, "button") == 0) ? "Button" : "Submit";
                }
                b->label = doc_arena_str(w, lb);
                P19_COMMON(b);
                add_block(w, b);
            }
        }
        else if (strcmp(tbuf, "file") == 0 || strcmp(tbuf, "reset") == 0 ||
                 strcmp(tbuf, "image") == 0)
        {
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_INPUT_SUBMIT);
            if (b)
            {
                b->name = doc_arena_str(w, inputName);
                b->value = doc_arena_str(w, inputVal);
                const char *lb;
                if (inputVal[0] != '\0')
                {
                    lb = inputVal;
                }
                else if (strcmp(tbuf, "file") == 0)
                {
                    lb = "Choose File";
                }
                else if (strcmp(tbuf, "reset") == 0)
                {
                    lb = "Reset";
                }
                else
                {
                    lb = "Submit";
                }
                b->label = doc_arena_str(w, lb);
                P19_COMMON(b);
                add_block(w, b);
            }
        }
#undef P19_COMMON
    }
    else if (strcmp(tag, "textarea") == 0)
    {
        flush_current_block(w);
        /* Pre-create the block (all attr fields except the value, which the
         * children fill into textareaBuf); carried through the exit ctx. */
        DocBlock *b = new_block(w, DOC_BLOCK_INPUT_FIELD);
        if (!b)
        {
            return;
        }
        b->inputType = doc_arena_str(w, "textarea");
        b->name = doc_arena_str(w, attr_or_d(a, "name", "q"));
        b->placeholder = doc_arena_str(w, attr_or_d(a, "placeholder", ""));
        int hasC, hasR;
        b->fieldWidth = attr_val(a, "cols")
                            ? (int)tonum_or(attr_val(a, "cols"), &hasC)
                            : -1;
        b->fieldRows = attr_val(a, "rows")
                           ? (int)tonum_or(attr_val(a, "rows"), &hasR)
                           : -1;
        int tdis = attr_has(a, "disabled") || (w->disabledDepth > 0);
        b->disabled = tdis;
        b->readonly = attr_has(a, "readonly") || tdis;
        b->required = attr_has(a, "required");
        int hasMv;
        b->maxlength = attr_val(a, "maxlength")
                           ? (int)tonum_or(attr_val(a, "maxlength"), &hasMv)
                           : -1;
        b->formAction = doc_arena_str(w, w->formAction ? w->formAction : "");
        b->formMethod = doc_arena_str(w, w->formMethod ? w->formMethod : "get");
        b->inert = (w->inert > 0) || tdis;
        ExitCtx *x = push_exit(w, WX_TEXTAREA_END);
        if (x)
        {
            x->figure = b; /* carry the block through the exit */
        }
        push_children(w, node);
        w->inTextarea = 1;
        w->textareaName = b->name;
        strbuf_reset(&w->textareaBuf);
    }
    else if (strcmp(tag, "button") == 0)
    {
        char bbuf[16];
        lua_lower_buf(bbuf, attr_or_d(a, "type", "submit"), sizeof(bbuf));
        if (strcmp(bbuf, "submit") == 0 || strcmp(bbuf, "button") == 0)
        {
            static char lbuf[1024]; /* hoisted: device gameTask stack is tiny */
            doc_concat_node_text(node, lbuf, sizeof(lbuf));
            char *label = doc_arena_collapse(w, lbuf);
            int disabled = attr_has(a, "disabled") || (w->disabledDepth > 0);
            flush_current_block(w);
            DocBlock *b = new_block(w, DOC_BLOCK_INPUT_SUBMIT);
            if (b)
            {
                const char *nm = attr_val(a, "name");
                const char *vl = attr_val(a, "value");
                b->name = nm ? doc_arena_str(w, nm) : NULL;
                b->value = vl ? doc_arena_str(w, vl) : NULL;
                const char *lb;
                if (label && label[0] != '\0')
                {
                    lb = label;
                }
                else
                {
                    lb = (strcmp(bbuf, "button") == 0) ? "Button" : "Submit";
                }
                b->label = doc_arena_str(w, lb);
                b->disabled = disabled;
                const char *fa = attr_val(a, "formaction");
                b->formAction = fa ? resolve_href(w, fa) : doc_arena_str(w, w->formAction ? w->formAction : "");
                const char *fm = attr_val(a, "formmethod");
                if (fm)
                {
                    char fmb[16];
                    lua_lower_buf(fmb, fm, sizeof(fmb));
                    b->formMethod = doc_arena_str(w, fmb);
                }
                else
                {
                    b->formMethod = doc_arena_str(w, w->formMethod ? w->formMethod : "get");
                }
                b->inert = (w->inert > 0) || disabled;
                add_block(w, b);
            }
        }
    }
    else if (strcmp(tag, "select") == 0)
    {
        flush_current_block(w);
        DocBlock *b = new_block(w, DOC_BLOCK_SELECT_FIELD);
        if (b)
        {
            b->name = doc_arena_str(w, attr_or_d(a, "name", "q"));
            collect_options(w, node, b);
            int selIndex = 1;
            for (int i = 0; i < b->optionCount; i++)
            {
                DocOption *o = b->options[i];
                if (o->selected && !o->disabled)
                {
                    selIndex = i + 1;
                }
            }
            if (b->optionCount > 0)
            {
                b->selectedIndex = selIndex;
                b->multiple = attr_has(a, "multiple");
                b->disabled = attr_has(a, "disabled") || (w->disabledDepth > 0);
                b->required = attr_has(a, "required");
                b->formAction = doc_arena_str(w, w->formAction ? w->formAction : "");
                b->formMethod = doc_arena_str(w, w->formMethod ? w->formMethod : "get");
                b->inert = (w->inert > 0) || attr_has(a, "disabled");
                add_block(w, b);
            }
        }
    }

    /* ── Bordered boxes ── */
    else if (strcmp(tag, "fieldset") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        flush_current_block(w);
        static char lbuf[1024]; /* hoisted: device gameTask stack is tiny */
        lbuf[0] = '\0';
        const DomNode *lg = first_child_tag(node, "legend");
        if (lg)
        {
            doc_concat_node_text(lg, lbuf, sizeof(lbuf));
        }
        char *label = doc_arena_collapse(w, lbuf);
        DocBlock *bo = box_open_block(w, label ? label : "");
        if (bo)
        {
            add_block(w, bo);
        }
        ExitCtx *x = push_exit(w, WX_FIELDSET_END);
        if (x)
        {
            x->disabledDepth = w->disabledDepth;
        }
        push_children_except(w, node, "legend");
        if (attr_has(a, "disabled"))
        {
            w->disabledDepth++;
        }
    }
    else if (strcmp(tag, "details") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        flush_current_block(w);
        w->detailsIndex++;
        int idx = w->detailsIndex;
        static char lbuf[1024]; /* hoisted: device gameTask stack is tiny */
        lbuf[0] = '\0';
        const DomNode *sm = first_child_tag(node, "summary");
        if (sm)
        {
            doc_concat_node_text(sm, lbuf, sizeof(lbuf));
        }
        char *label = doc_arena_collapse(w, lbuf);
        int isOpen = attr_has(a, "open");
        if (w->opts && w->opts->detailsOpen && (idx - 1) < w->opts->detailsOpenCount)
        {
            isOpen = w->opts->detailsOpen[idx - 1];
        }
        static char withPrefix[1100]; /* hoisted: device gameTask stack is tiny */
        snprintf(withPrefix, sizeof(withPrefix), "%s%s",
                 (label && label[0]) ? "> " : "", (label && label[0]) ? label : "");
        DocBlock *bo = box_open_block(w, withPrefix);
        if (bo)
        {
            char keybuf[16];
            snprintf(keybuf, sizeof(keybuf), "d%d", idx);
            bo->toggleKey = doc_arena_str(w, keybuf);
            bo->toggleOpen = isOpen;
            add_block(w, bo);
        }
        ExitCtx *x = push_exit(w, WX_DETAILS_END);
        if (x)
        {
            x->detailsIdx = idx;
            x->toggleOpen = isOpen;
        }
        if (isOpen)
        {
            push_children_except(w, node, "summary");
        }
    }
    else if (strcmp(tag, "dialog") == 0)
    {
        if (w->cell)
        {
            push_children(w, node);
            return;
        }
        /* A dialog without the open attribute is not rendered at all. */
        if (!attr_has(a, "open"))
        {
            return;
        }
        flush_current_block(w);
        add_block(w, box_open_block(w, ""));
        push_exit(w, WX_DIALOG_END);
        push_children(w, node);
    }

    /* ── Media placeholders ── */
    else if (strcmp(tag, "video") == 0 || strcmp(tag, "audio") == 0 ||
             strcmp(tag, "iframe") == 0 || strcmp(tag, "canvas") == 0 ||
             strcmp(tag, "object") == 0 || strcmp(tag, "embed") == 0 ||
             strcmp(tag, "portal") == 0)
    {
        flush_current_block(w);
        const char *src = attr_val(a, "src");
        if (!src)
        {
            src = attr_val(a, "data");
        }
        if (!src || src[0] == '\0')
        {
            /* Look for the first <source src="..."> child. */
            for (int i = 0; i < node->childCount; i++)
            {
                const DomNode *c = node->children[i];
                if (c->kind == DOM_ELEMENT && c->tag &&
                    strcmp(c->tag, "source") == 0)
                {
                    const char *ss = attr_val(attrs_of(c), "src");
                    if (ss)
                    {
                        src = ss;
                        break;
                    }
                }
            }
        }
        const char *lbl = attr_val(a, "title");
        if (!lbl)
        {
            lbl = attr_val(a, "alt");
        }
        if (!lbl)
        {
            lbl = "";
        }
        static char lbuf[512]; /* hoisted: device gameTask stack is tiny */
        if (lbl[0] == '\0')
        {
            if (src && src[0])
            {
                /* Lua: string.match(src, "([^/]+)/?$") or src — one optional
                 * trailing slash, then the trailing non-slash run. */
                size_t slen = strlen(src);
                size_t e = slen;
                size_t s;
                if (src[e - 1] == '/')
                {
                    e--;
                }
                s = e;
                while (s > 0 && src[s - 1] != '/')
                {
                    s--;
                }
                if (e > s)
                {
                    snprintf(lbuf, sizeof(lbuf), "[%s: %.*s]", tag,
                             (int)(e - s), src + s);
                }
                else
                {
                    snprintf(lbuf, sizeof(lbuf), "[%s: %s]", tag, src);
                }
            }
            else
            {
                snprintf(lbuf, sizeof(lbuf), "[%s]", tag);
            }
            lbl = lbuf;
        }
        double wd = strict_num(attr_val(a, "width"), 160);
        double ht = strict_num(attr_val(a, "height"), 60);
        if (wd > 360)
        {
            wd = 360;
        }
        if (ht > 120)
        {
            ht = 120;
        }
        DocBlock *ph = new_block(w, DOC_BLOCK_PLACEHOLDER);
        if (ph)
        {
            ph->plabel = doc_arena_str(w, lbl);
            ph->pwidth = wd;
            ph->pheight = ht;
            ph->ptag = doc_arena_str(w, tag);
            if ((strcmp(tag, "iframe") == 0 || strcmp(tag, "portal") == 0) &&
                src && src[0] && doc_valid_href(src))
            {
                ph->phref = resolve_href(w, src);
            }
            add_block(w, ph);
        }
    }
    else if (strcmp(tag, "progress") == 0 || strcmp(tag, "meter") == 0)
    {
        flush_current_block(w);
        double value = strict_num(attr_val(a, "value"), 0);
        double max = strict_num(attr_val(a, "max"), 1);
        if (max <= 0)
        {
            max = 1;
        }
        DocBlock *b = new_block(w, DOC_BLOCK_METER);
        if (b)
        {
            b->mvalue = value;
            b->mmax = max;
            b->mmin = strict_num(attr_val(a, "min"), 0);
            b->mlow = strict_num(attr_val(a, "low"), 0);
            b->mhigh = strict_num(attr_val(a, "high"), max);
            b->moptimum = strict_num(attr_val(a, "optimum"), 0);
            b->label = doc_arena_str(w, attr_or_d(a, "title", ""));
            add_block(w, b);
        }
    }
    else if (strcmp(tag, "map") == 0)
    {
        /* Image map: not rendered, but its <area> regions are collected. */
        const char *nm0 = attr_or_d(a, "name", "");
        const char *nm = nm0[0] == '#' ? nm0 + 1 : nm0;
        if (nm[0] != '\0')
        {
            DocMap *mp = NULL;
            for (int i = 0; i < w->doc->mapCount; i++)
            {
                if (w->doc->maps[i]->name && strcmp(w->doc->maps[i]->name, nm) == 0)
                {
                    mp = w->doc->maps[i];
                    break;
                }
            }
            if (!mp)
            {
                mp = (DocMap *)doc_arena_alloc(&w->arena, sizeof(DocMap));
                if (!mp)
                {
                    w->error = 1;
                    return;
                }
                memset(mp, 0, sizeof(*mp));
                mp->name = doc_arena_str(w, nm);
                if (doc_ptrarr_push((void ***)&w->doc->maps, &w->doc->mapCount,
                                    &w->doc->mapCap, mp))
                {
                    w->error = 1;
                    return;
                }
            }
            /* Replace regions (Lua doc.maps[name] = regions). */
            if (mp->areas)
            {
                PLUTO_FREE(mp->areas);
                mp->areas = NULL;
                mp->areaCount = 0;
                mp->areaCap = 0;
            }
            for (int i = 0; i < node->childCount; i++)
            {
                const DomNode *c = node->children[i];
                if (c->kind != DOM_ELEMENT || strcmp(c->tag, "area") != 0)
                {
                    continue;
                }
                AttrList aa = attrs_of(c);
                DocArea *ar = (DocArea *)doc_arena_alloc(&w->arena, sizeof(DocArea));
                if (!ar)
                {
                    w->error = 1;
                    return;
                }
                memset(ar, 0, sizeof(*ar));
                ar->shape = doc_arena_str(w, attr_or_d(aa, "shape", "rect"));
                const char *co = attr_or_d(aa, "coords", "");
                int *coords = NULL;
                int cc = 0, ccap = 0;
                for (const char *p = co; *p;)
                {
                    if (*p >= '0' && *p <= '9')
                    {
                        long v = 0;
                        while (*p >= '0' && *p <= '9')
                        {
                            v = v * 10 + (*p - '0');
                            p++;
                        }
                        if (cc >= ccap)
                        {
                            int nc = ccap ? ccap * 2 : 8;
                            int *na = (int *)PLUTO_REALLOC(coords,
                                                           (size_t)nc * sizeof(int));
                            if (!na)
                            {
                                w->error = 1;
                                break;
                            }
                            coords = na;
                            ccap = nc;
                        }
                        coords[cc++] = (int)v;
                    }
                    else
                    {
                        p++;
                    }
                }
                ar->coords = coords;
                ar->coordCount = cc;
                const char *h = attr_val(aa, "href");
                ar->href = (h && doc_valid_href(h)) ? resolve_href(w, h) : NULL;
                ar->alt = doc_arena_str(w, attr_or_d(aa, "alt", ""));
                if (doc_ptrarr_push((void ***)&mp->areas, &mp->areaCount,
                                    &mp->areaCap, ar))
                {
                    w->error = 1;
                }
            }
        }
    }
    else if (strcmp(tag, "datalist") == 0)
    {
        const char *id = attr_or_d(a, "id", "");
        if (id[0] != '\0')
        {
            DocDatalist *dl = (DocDatalist *)doc_arena_alloc(&w->arena, sizeof(DocDatalist));
            if (!dl)
            {
                w->error = 1;
                return;
            }
            memset(dl, 0, sizeof(*dl));
            dl->id = doc_arena_str(w, id);
            for (int i = 0; i < node->childCount; i++)
            {
                const DomNode *c = node->children[i];
                if (c->kind == DOM_ELEMENT && strcmp(c->tag, "option") == 0)
                {
                    static char tbuf2[512]; /* hoisted: device gameTask stack is tiny */
                    doc_concat_node_text(c, tbuf2, sizeof(tbuf2));
                    AttrList ca = attrs_of(c);
                    DocOption *o = (DocOption *)doc_arena_alloc(&w->arena, sizeof(DocOption));
                    if (!o)
                    {
                        w->error = 1;
                        return;
                    }
                    memset(o, 0, sizeof(*o));
                    o->text = doc_arena_collapse(w, tbuf2);
                    o->value = doc_arena_str(w, attr_or_d(ca, "value", o->text ? o->text : ""));
                    if (doc_ptrarr_push((void ***)&dl->options, &dl->optionCount,
                                        &dl->optionCap, o))
                    {
                        w->error = 1;
                    }
                }
            }
            if (doc_ptrarr_push((void ***)&w->doc->datalists, &w->doc->datalistCount,
                                &w->doc->datalistCap, dl))
            {
                w->error = 1;
            }
        }
    }
    else if (strcmp(tag, "template") == 0 || strcmp(tag, "menuitem") == 0 ||
             strcmp(tag, "content") == 0 || strcmp(tag, "shadow") == 0 ||
             strcmp(tag, "geolocation") == 0)
    {
        /* Inert / non-rendered. */
    }
    else if (strcmp(tag, "fencedframe") == 0)
    {
        flush_current_block(w);
        double wd = strict_num(attr_val(a, "width"), 160);
        double ht = strict_num(attr_val(a, "height"), 60);
        if (wd > 360)
        {
            wd = 360;
        }
        if (ht > 120)
        {
            ht = 120;
        }
        DocBlock *ph = new_block(w, DOC_BLOCK_PLACEHOLDER);
        if (ph)
        {
            ph->plabel = doc_arena_str(w, "[fencedframe]");
            ph->pwidth = wd;
            ph->pheight = ht;
            add_block(w, ph);
        }
    }
    else if (strcmp(tag, "source") == 0 || strcmp(tag, "track") == 0 ||
             strcmp(tag, "col") == 0 || strcmp(tag, "colgroup") == 0 ||
             strcmp(tag, "area") == 0 || strcmp(tag, "param") == 0 ||
             strcmp(tag, "frameset") == 0 || strcmp(tag, "frame") == 0)
    {
        /* Void / non-rendered. */
    }
    else if (strcmp(tag, "svg") == 0)
    {
        /* Inline SVG: serialize the subtree and rasterize it on-device. */
        flush_current_block(w);
        char *xml = doc_serialize_svg_node(node);
        double wd = strict_num(attr_val(a, "width"), 0);
        double ht = strict_num(attr_val(a, "height"), 0);
        if (wd <= 0 || ht <= 0)
        {
            /* viewBox: ^%s*([%-%d%.]+)%s+([%-%d%.]+)%s+([%-%d%.]+)%s+([%-%d%.]+) */
            const char *vb = attr_or_d(a, "viewBox", "");
            const char *p = vb;
            double nums[4] = {0, 0, 0, 0};
            int got = 1;
            for (int k = 0; k < 4 && got; k++)
            {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                {
                    p++;
                }
                const char *st2 = p;
                while (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9'))
                {
                    p++;
                }
                if (p == st2 || (k < 3 && *p == '\0'))
                {
                    got = 0;
                }
                else
                {
                    char nb[64];
                    size_t nl = (size_t)(p - st2);
                    if (nl >= sizeof(nb))
                    {
                        nl = sizeof(nb) - 1;
                    }
                    memcpy(nb, st2, nl);
                    nb[nl] = '\0';
                    int okn;
                    nums[k] = tonum_or(nb, &okn);
                    if (!okn)
                    {
                        got = 0;
                    }
                    /* %s+ requires at least one space between numbers */
                    if (k < 3 && *p != '\0' && !is_lspace(*p))
                    {
                        got = 0;
                    }
                }
            }
            if (got)
            {
                if (wd <= 0)
                {
                    wd = nums[0];
                }
                if (ht <= 0)
                {
                    ht = nums[1];
                }
            }
        }
        if (wd <= 0)
        {
            wd = 120;
        }
        if (ht <= 0)
        {
            ht = 40;
        }
        if (wd > 360)
        {
            wd = 360;
        }
        if (ht > 180)
        {
            ht = 180;
        }
        void *bmp = NULL;
        int ok = 0;
        if (xml && w->opts && w->opts->svgDecoder)
        {
            ok = (w->opts->svgDecoder(xml, (int)wd, (int)ht, &bmp) == 0 && bmp);
        }
        if (xml)
        {
            PLUTO_FREE(xml);
        }
        if (ok && bmp)
        {
            DocBlock *b = new_block(w, DOC_BLOCK_IMAGE);
            if (b)
            {
                b->img = bmp;
                b->width = wd;
                b->height = ht;
                const char *role = attr_val(a, "role");
                if (role && strcmp(role, "img") == 0)
                {
                    const char *al = attr_val(a, "aria-label");
                    if (!al)
                    {
                        al = attr_val(a, "title");
                    }
                    b->alt = doc_arena_str(w, al ? al : "");
                }
                else
                {
                    b->alt = doc_arena_str(w, "");
                }
                b->href = w->currentHref ? doc_arena_str(w, w->currentHref) : NULL;
                b->align = walk_align(w, a);
                b->inert = (w->inert > 0);
                add_block(w, b);
            }
        }
    }

    /* ── MathML ── */
    else if (strcmp(tag, "math") == 0)
    {
        flush_current_block(w);
        ExitCtx *x = push_exit(w, WX_MATH_END);
        if (x)
        {
            x->inMath = w->inMath;
        }
        push_children(w, node);
        w->inMath = 1;
        strbuf_reset(&w->mathBuf);
    }
    else if (strcmp(tag, "mfrac") == 0 || strcmp(tag, "msup") == 0 ||
             strcmp(tag, "msub") == 0)
    {
        const char *sep = (strcmp(tag, "mfrac") == 0) ? " / "
                          : (strcmp(tag, "msup") == 0) ? "^"
                                                       : "_";
        int n = node->childCount;
        if (n > 0)
        {
            for (int i = n - 1; i >= 1; i--)
            {
                push_enter(w, node->children[i]);
                push_sep(w, sep);
            }
            push_enter(w, node->children[0]);
        }
    }
    else if (strcmp(tag, "msubsup") == 0)
    {
        int n = node->childCount;
        if (n > 0)
        {
            for (int i = n - 1; i >= 1; i--)
            {
                push_enter(w, node->children[i]);
                if (i == 2)
                {
                    push_sep(w, "_");
                }
                if (i == 3)
                {
                    push_sep(w, "^");
                }
            }
            push_enter(w, node->children[0]);
        }
    }
    else if (strcmp(tag, "msqrt") == 0)
    {
        push_sep(w, ")");
        push_children(w, node);
        push_sep(w, "sqrt(");
    }
    else if (strcmp(tag, "mroot") == 0)
    {
        int n = node->childCount;
        push_sep(w, ")"); /* final close */
        for (int i = n; i >= 1; i--)
        {
            push_enter(w, node->children[i - 1]);
            if (i == n)
            {
                push_sep(w, ")");
            }
            if (i > 1)
            {
                push_sep(w, "^(1/");
            }
        }
        push_sep(w, "sqrt(");
    }
    else if (strcmp(tag, "mfenced") == 0)
    {
        char *openS = entities_decode(attr_or_d(a, "open", "("));
        char *closeS = entities_decode(attr_or_d(a, "close", ")"));
        char *sepS = entities_decode(attr_or_d(a, "separators", ","));
        push_sep(w, doc_arena_str(w, closeS ? closeS : ")"));
        int n = node->childCount;
        for (int i = n; i >= 1; i--)
        {
            push_enter(w, node->children[i - 1]);
            if (i > 1)
            {
                push_sep(w, doc_arena_str(w, sepS ? sepS : ","));
            }
        }
        push_sep(w, doc_arena_str(w, openS ? openS : "("));
        if (openS)
        {
            PLUTO_FREE(openS);
        }
        if (closeS)
        {
            PLUTO_FREE(closeS);
        }
        if (sepS)
        {
            PLUTO_FREE(sepS);
        }
    }
    else if (strcmp(tag, "mspace") == 0)
    {
        if (strbuf_append(&w->mathBuf, " "))
        {
            w->error = 1;
        }
    }

    else if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
             strcmp(tag, "title") == 0)
    {
        /* Non-rendered: content stripped by the tokenizer; must not walk. */
    }
    else if (strcmp(tag, "meta") == 0)
    {
        const char *httpEquiv = attr_val(a, "http-equiv");
        if (httpEquiv)
        {
            char le[64];
            lua_lower_buf(le, httpEquiv, sizeof(le));
            if (strcmp(le, "refresh") == 0)
            {
                refresh_from_content(w->doc, attr_or_d(a, "content", ""),
                                     w->doc->baseUrl);
            }
        }
    }
    else
    {
        /* Unknown element: render its children in normal flow. */
        push_children(w, node);
    }
}

/* ── Exit actions (post-children work) ─────────────────────────────────────── */

static void run_exit(Walker *w, ExitCtx *x)
{
    switch (x->kind)
    {
    case WX_FLUSH:
        flush_current_block(w);
        break;
    case WX_STYLE:
        w->flags = x->flags;
        break;
    case WX_LINK_END:
        if (w->currentHref)
        {
            DocLink *lk = (DocLink *)doc_arena_alloc(&w->arena, sizeof(DocLink));
            if (lk)
            {
                memset(lk, 0, sizeof(*lk));
                const char *lt = w->linkText.data;
                const char *text = (lt && lt[0] != '\0') ? lt : x->title;
                lk->href = w->currentHref;
                lk->text = doc_arena_str(w, (text && text[0]) ? text : w->currentHref);
                lk->target = x->target;
                if (doc_ptrarr_push((void ***)&w->doc->links, &w->doc->linkCount,
                                    &w->doc->linkCap, lk))
                {
                    w->error = 1;
                }
            }
        }
        w->currentHref = NULL;
        w->currentAnchorIndex = 0;
        strbuf_reset(&w->linkText);
        break;
    case WX_QUOTE_END:
        q_quote(w);
        w->flags = x->flags;
        break;
    case WX_PRE_END:
    {
        const char *txt = w->preBuf.data;
        if (txt && txt[0] != '\0')
        {
            DocBlock *b = new_block(w, DOC_BLOCK_CODE_BLOCK);
            if (b)
            {
                size_t len = w->preBuf.len;
                b->text = doc_arena_strn(w, txt, len);
                /* lines = gmatch(preBuffer .. "\\n", "(.-)\r?\n") — the final
                 * (appended) newline always terminates one more line. */
                size_t p = 0;
                while (p <= len)
                {
                    size_t q = p;
                    while (q < len && txt[q] != '\n')
                    {
                        q++;
                    }
                    size_t e = q;
                    if (e > p && txt[e - 1] == '\r')
                    {
                        e--;
                    }
                    char *l = doc_arena_strn(w, txt + p, e - p);
                    if (!l || doc_ptrarr_push((void ***)&b->lines, &b->lineCount,
                                              &b->lineCap, l))
                    {
                        w->error = 1;
                        break;
                    }
                    p = q + 1;
                    if (q == len)
                    {
                        break; /* consumed the appended newline */
                    }
                }
                add_block(w, b);
            }
        }
        w->inPre = 0;
        strbuf_reset(&w->preBuf);
        break;
    }
    case WX_FIGURE_END:
    {
        FigureCtx *f = w->figure;
        if (f)
        {
            if (f->image)
            {
                char *cap = doc_arena_collapse(w, f->cap->data ? f->cap->data : "");
                if (cap && cap[0] != '\0')
                {
                    f->image->alt = cap;
                    f->image->caption = cap;
                }
                flush_current_block(w);
                add_block(w, f->image);
            }
            else if (f->cap->len > 0)
            {
                /* caption has non-whitespace? */
                const char *cd = f->cap->data ? f->cap->data : "";
                int hasNonSpace = 0;
                for (const char *p = cd; *p; p++)
                {
                    if (!is_lspace(*p))
                    {
                        hasNonSpace = 1;
                        break;
                    }
                }
                if (hasNonSpace)
                {
                    flush_current_block(w);
                    DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
                    if (b)
                    {
                        b->align = doc_arena_str(w, "center");
                        DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
                        if (inl)
                        {
                            inl->text = doc_arena_str(w, cd);
                            inl->flags = DOC_INF_ITALIC;
                            if (doc_ptrarr_push((void ***)&b->inlines,
                                                &b->inlineCount, &b->inlineCap, inl))
                            {
                                w->error = 1;
                            }
                        }
                        add_block(w, b);
                    }
                }
            }
            /* restore */
            w->figure = (FigureCtx *)x->figure;
            strbuf_free(f->cap);
            for (int i = 0; i < w->heapBufCount; i++)
            {
                if (w->heapBufs[i] == f->cap)
                {
                    w->heapBufs[i] = NULL;
                    break;
                }
            }
        }
        break;
    }
    case WX_LIST_END:
        w->listCtx = (ListCtx *)x->listCtx;
        break;
    case WX_DL_END:
        w->dlDepth = x->dlDepth;
        break;
    case WX_FORM_END:
        w->formAction = x->formAction;
        w->formMethod = x->formMethod;
        break;
    case WX_TEXTAREA_END:
    {
        DocBlock *b = (DocBlock *)x->figure;
        if (b)
        {
            b->value = doc_arena_str(w, w->textareaBuf.data ? w->textareaBuf.data : "");
            add_block(w, b);
        }
        w->inTextarea = 0;
        strbuf_reset(&w->textareaBuf);
        break;
    }
    case WX_MATH_END:
    {
        char *s = doc_arena_collapse(w, w->mathBuf.data ? w->mathBuf.data : "");
        if (s && s[0] != '\0')
        {
            DocBlock *b = new_block(w, DOC_BLOCK_MATH);
            if (b)
            {
                b->text = s;
                add_block(w, b);
            }
        }
        w->inMath = x->inMath;
        strbuf_reset(&w->mathBuf);
        break;
    }
    case WX_FIELDSET_END:
        add_block(w, new_block(w, DOC_BLOCK_BOX_CLOSE));
        w->disabledDepth = x->disabledDepth;
        break;
    case WX_DETAILS_END:
    {
        DocBlock *bc = new_block(w, DOC_BLOCK_BOX_CLOSE);
        if (bc)
        {
            char keybuf[16];
            snprintf(keybuf, sizeof(keybuf), "d%d", x->detailsIdx);
            bc->toggleKey = doc_arena_str(w, keybuf);
            bc->toggleOpen = x->toggleOpen;
            add_block(w, bc);
        }
        break;
    }
    case WX_DIALOG_END:
        add_block(w, new_block(w, DOC_BLOCK_BOX_CLOSE));
        break;
    case WX_INERT_END:
        w->inert = x->inertSaved;
        break;
    case WX_SPAN_END:
        if (x->fallback &&
            (!w->currentBlock || w->currentBlock->inlineCount == x->hadInlines))
        {
            DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
            if (inl)
            {
                inl->text = x->fallback;
                inl->flags = w->flags & (DOC_INF_BOLD | DOC_INF_ITALIC);
                add_inline(w, inl);
            }
        }
        w->flags = x->flags;
        break;
    case WX_FIGCAP_END:
        w->figureCaptionDone = 1;
        break;
    default:
        break;
    }
}

/* ── Recursive walker → explicit frame loop ────────────────────────────────── */

static void walk_node(Walker *w, const DomNode *node)
{
    if (!node)
    {
        return;
    }
    if (node->kind == DOM_TEXT)
    {
        handle_text_node(w, node);
        return;
    }
    AttrList a = attrs_of(node);
    /* hidden/popover/display:none suppress the element AND its subtree */
    if (walk_display_none(a))
    {
        return;
    }
    int wasInert = w->inert;
    if (attr_has(a, "inert"))
    {
        /* Lua: wasInert = state.inert; state.inert += 1; walk(node);
         * state.inert = wasInert — the scope covers the whole subtree, so the
         * restore must run AFTER the children (exit frame, pushed first). */
        w->inert++;
        ExitCtx *x = push_exit(w, WX_INERT_END);
        if (x)
        {
            x->inertSaved = wasInert;
        }
    }
    handle_element(w, node);
}

static int walk_children(Walker *w, const DomNode *parent)
{
    if (w->error)
    {
        return -1;
    }
    int base = w->frameCount; /* re-entrant calls only unwind their own frames */
    if (parent)
    {
        for (int i = parent->childCount - 1; i >= 0; i--)
        {
            if (push_enter(w, parent->children[i]))
            {
                return -1;
            }
        }
    }
    while (!w->error && w->frameCount > base)
    {
        WFrame f = w->frames[--w->frameCount];
        if (f.kind == 1)
        {
            run_exit(w, f.ctx);
        }
        else if (f.kind == 2)
        {
            if (strbuf_append(&w->mathBuf, f.sep))
            {
                w->error = 1;
            }
        }
        else
        {
            walk_node(w, f.node);
        }
    }
    return w->error ? -1 : 0;
}

/* Finalize: truncated marker + empty-page fallback (top-level only). */
static void walker_finish(Walker *w)
{
    flush_current_block(w);
    if (w->truncated)
    {
        DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
        if (b)
        {
            b->align = doc_arena_str(w, "center");
            DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
            if (inl)
            {
                inl->text = doc_arena_str(w, "(Page too large - rest not rendered)");
                inl->flags = DOC_INF_BOLD;
                if (doc_ptrarr_push((void ***)&b->inlines, &b->inlineCount,
                                    &b->inlineCap, inl))
                {
                    w->error = 1;
                }
            }
            /* unconditional insert (reference table.insert) */
            if (doc_ptrarr_push((void ***)&w->doc->blocks, &w->doc->blockCount,
                                &w->doc->blockCap, b))
            {
                w->error = 1;
            }
        }
    }
    if (w->doc->blockCount == 0)
    {
        DocBlock *b = new_block(w, DOC_BLOCK_PARAGRAPH);
        if (b)
        {
            DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
            if (inl)
            {
                inl->text = doc_arena_str(w, "(Empty Web Page)");
                inl->flags = DOC_INF_ITALIC;
                if (doc_ptrarr_push((void ***)&b->inlines, &b->inlineCount,
                                    &b->inlineCap, inl))
                {
                    w->error = 1;
                }
            }
            if (doc_ptrarr_push((void ***)&w->doc->blocks, &w->doc->blockCount,
                                &w->doc->blockCap, b))
            {
                w->error = 1;
            }
        }
    }
}

/* ── Free helpers ──────────────────────────────────────────────────────────── */

static void doc_free_walk_output(DocParseResult *out)
{
    for (int i = 0; i < out->blockCount; i++)
    {
        DocBlock *b = out->blocks[i];
        if (b->inlines)
        {
            PLUTO_FREE(b->inlines);
        }
        if (b->lines)
        {
            PLUTO_FREE(b->lines);
        }
        if (b->options)
        {
            PLUTO_FREE(b->options);
        }
        if (b->table)
        {
            DocTable *t = b->table;
            for (int r = 0; r < t->rowCount; r++)
            {
                DocRow *row = t->rows[r];
                for (int c = 0; c < row->cellCount; c++)
                {
                    if (row->cells[c]->inlines)
                    {
                        PLUTO_FREE(row->cells[c]->inlines);
                    }
                }
                if (row->cells)
                {
                    PLUTO_FREE(row->cells);
                }
            }
            if (t->rows)
            {
                PLUTO_FREE(t->rows);
            }
        }
    }
    if (out->blocks)
    {
        PLUTO_FREE(out->blocks);
        out->blocks = NULL;
    }
    if (out->links)
    {
        PLUTO_FREE(out->links);
        out->links = NULL;
    }
    for (int i = 0; i < out->mapCount; i++)
    {
        DocMap *m = out->maps[i];
        for (int j = 0; j < m->areaCount; j++)
        {
            if (m->areas[j]->coords)
            {
                PLUTO_FREE(m->areas[j]->coords);
            }
        }
        if (m->areas)
        {
            PLUTO_FREE(m->areas);
        }
    }
    if (out->maps)
    {
        PLUTO_FREE(out->maps);
        out->maps = NULL;
    }
    for (int i = 0; i < out->datalistCount; i++)
    {
        if (out->datalists[i]->options)
        {
            PLUTO_FREE(out->datalists[i]->options);
        }
    }
    if (out->datalists)
    {
        PLUTO_FREE(out->datalists);
        out->datalists = NULL;
    }
    out->blockCount = out->blockCap = 0;
    out->linkCount = out->linkCap = 0;
    out->mapCount = out->mapCap = 0;
    out->datalistCount = out->datalistCap = 0;
}

/* ── meta refresh content parser (shared by token scan + <meta> handler) ──── */

static int refresh_from_content(DocParseResult *out, const char *content,
                                const char *baseUrl)
{
    /* Lua pattern 1: "^(%d+%.?%d*)%s*;%s*[Uu][Rr][Ll]=%s*(.+)$"
     * Lua pattern 2: "^(%d+%.?%d*)%s*$" */
    const char *p = content;
    while (*p >= '0' && *p <= '9')
    {
        p++;
    }
    if (*p == '.')
    {
        /* %d+%.?%d* consumes the dot even without following digits. */
        p++;
        while (*p >= '0' && *p <= '9')
        {
            p++;
        }
    }
    if (p == content)
    {
        return 0;
    }
    char delayStr[32];
    size_t dn = (size_t)(p - content);
    if (dn >= sizeof(delayStr))
    {
        dn = sizeof(delayStr) - 1;
    }
    memcpy(delayStr, content, dn);
    delayStr[dn] = '\0';

    const char *urlPart = NULL;
    int matched = 0;
    const char *q = p;
    while (*q == ' ' || *q == '\t')
    {
        q++;
    }
    if (*q == ';')
    {
        q++;
        while (*q == ' ' || *q == '\t')
        {
            q++;
        }
        if (ci_prefix(q, "url=", 4) == 0)
        {
            q += 4;
            while (*q == ' ' || *q == '\t')
            {
                q++;
            }
            if (*q) /* (.+)$ — non-empty required */
            {
                urlPart = q;
                matched = 1;
            }
        }
    }
    else if (*q == '\0')
    {
        matched = 1; /* pattern 2: delay only */
    }
    if (!matched)
    {
        return 0;
    }

    out->metaRefresh.present = 1;
    out->metaRefresh.delay = (float)atof(delayStr);
    if (urlPart)
    {
        const char *a2 = urlPart;
        const char *z = urlPart + strlen(urlPart);
        while (a2 < z && (*a2 == ' ' || *a2 == '\t'))
        {
            a2++;
        }
        while (z > a2 && (*(z - 1) == ' ' || *(z - 1) == '\t'))
        {
            z--;
        }
        char trimmed[512];
        size_t tn = (size_t)(z - a2);
        if (tn >= sizeof(trimmed))
        {
            tn = sizeof(trimmed) - 1;
        }
        memcpy(trimmed, a2, tn);
        trimmed[tn] = '\0';
        out->metaRefresh.url[0] = '\0';
        if (trimmed[0] != '\0')
        {
            char *resolved = url_resolve(baseUrl, trimmed);
            if (resolved)
            {
                snprintf(out->metaRefresh.url, sizeof(out->metaRefresh.url), "%s",
                         resolved);
                PLUTO_FREE(resolved);
            }
        }
    }
    return 1;
}

/* ── Table row helper (handleRow) ──────────────────────────────────────────── */

static void handle_row(Walker *w, const DomNode *trNode, DocTable *tbl)
{
    DocRow *row = (DocRow *)doc_arena_alloc(&w->arena, sizeof(DocRow));
    if (!row)
    {
        w->error = 1;
        return;
    }
    memset(row, 0, sizeof(*row));
    for (int i = 0; i < trNode->childCount; i++)
    {
        const DomNode *cellNode = trNode->children[i];
        if (cellNode->kind != DOM_ELEMENT)
        {
            continue;
        }
        if (strcmp(cellNode->tag, "td") != 0 && strcmp(cellNode->tag, "th") != 0)
        {
            continue;
        }
        AttrList ca = attrs_of(cellNode);
        char *savedHref = w->currentHref;
        int savedAnchor = w->currentAnchorIndex;
        size_t savedLinkLen = w->linkText.len;

        DocCell *cell = (DocCell *)doc_arena_alloc(&w->arena, sizeof(DocCell));
        if (!cell)
        {
            w->error = 1;
            return;
        }
        memset(cell, 0, sizeof(*cell));
        cell->header = (strcmp(cellNode->tag, "th") == 0);
        cell->colspan = (int)strict_num(attr_val(ca, "colspan"), 1);
        cell->rowspan = (int)strict_num(attr_val(ca, "rowspan"), 1);
        const char *ab = attr_val(ca, "abbr");
        if (!ab)
        {
            ab = attr_val(ca, "title");
        }
        cell->abbr = doc_arena_str(w, ab ? ab : "");
        cell->align = walk_align(w, ca);

        w->cell = cell;
        w->cellFirstText = 1;
        w->currentHref = NULL;
        w->currentAnchorIndex = 0;
        strbuf_reset(&w->linkText);
        walk_children(w, cellNode);
        if (cell->inlineCount == 0)
        {
            DocInline *inl = new_inline(w, DOC_INLINE_TEXT);
            if (inl)
            {
                inl->text = doc_arena_str(w, " ");
                if (doc_ptrarr_push((void ***)&cell->inlines,
                                    &cell->inlineCount, &cell->inlineCap, inl))
                {
                    w->error = 1;
                }
            }
        }
        if (doc_ptrarr_push((void ***)&row->cells, &row->cellCount, &row->cellCap, cell))
        {
            w->error = 1;
        }

        w->cell = NULL;
        w->currentHref = savedHref;
        w->currentAnchorIndex = savedAnchor;
        /* Restore the saved linkText prefix (cell text is discarded). */
        if (w->linkText.len > savedLinkLen)
        {
            w->linkText.len = savedLinkLen;
            if (w->linkText.data)
            {
                w->linkText.data[savedLinkLen] = '\0';
            }
        }
    }
    if (doc_ptrarr_push((void ***)&tbl->rows, &tbl->rowCount, &tbl->rowCap, row))
    {
        w->error = 1;
    }
}

/* ── Document.parse (full HTML-mode pipeline) ───────────────────────────────── */

static int doc_get_attr(const Token *tok, const char *key, const char **outVal)
{
    for (int i = 0; i < tok->attrCount; i++)
    {
        if (strcmp(tok->attrs[i].key, key) == 0)
        {
            const char *v = tok->attrs[i].value;
            *outVal = (v == PLUTO_TOK_ATTR_TRUE) ? "" : v;
            return 1;
        }
    }
    return 0;
}

int document_parse(const char *htmlString, const char *baseUrl, int mode,
                   const DocParseOpts *opts, DocParseResult *out)
{
    memset(out, 0, sizeof(*out));

    if (!htmlString || htmlString[0] == '\0')
    {
        snprintf(out->title, sizeof(out->title), "Blank Page");
        snprintf(out->baseUrl, sizeof(out->baseUrl), "%s",
                 baseUrl ? baseUrl : "about:blank");
        out->rawHtml = (char *)PLUTO_MALLOC(1);
        if (!out->rawHtml)
        {
            return -1;
        }
        out->rawHtml[0] = '\0';
        out->isReaderMode = 0;
        return 0;
    }

    /* 1. Tokenize. */
    TokenizeResult tr;
    if (tokenizer_tokenize(htmlString, &tr) != 0)
    {
        return -1;
    }
    snprintf(out->title, sizeof(out->title), "%s",
             tr.pageTitle[0] ? tr.pageTitle : "Web Page");

    /* 2. Reader mode → Readability.distill. */
    if (mode == MODE_READER)
    {
        int rc = readability_distill(&tr, out->title, baseUrl, out);
        tokenizer_free_result(&tr);
        if (rc != 0)
        {
            return -1;
        }
        out->rawHtml = (char *)PLUTO_MALLOC(strlen(htmlString) + 1);
        if (!out->rawHtml)
        {
            return -1;
        }
        strcpy(out->rawHtml, htmlString);
        out->mode = mode;
        return 0;
    }

    /* 3. Base href override (absolute URLs only) + meta refresh scan. */
    {
        char base[512];
        snprintf(base, sizeof(base), "%s", baseUrl ? baseUrl : "");
        const char *tokenBase = NULL;
        for (int i = 0; i < tr.tokens.count; i++)
        {
            const Token *tok = &tr.tokens.items[i];
            if (tok->type == TOK_TAG && strcmp(tok->name, "base") == 0 && !tok->isClosing)
            {
                const char *href = NULL;
                doc_get_attr(tok, "href", &href);
                if (href && href[0] != '\0')
                {
                    tokenBase = href;
                    break;
                }
            }
        }
        if (tokenBase)
        {
            /* Lua: string.match(baseHref, "^[a-zA-Z][%w+%-%.]*://") */
            int schemeOk = 0;
            const char *p = tokenBase;
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            {
                p++;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')
                {
                    p++;
                }
                if (strncmp(p, "://", 3) == 0)
                {
                    schemeOk = 1;
                }
            }
            if (schemeOk)
            {
                snprintf(base, sizeof(base), "%s", tokenBase);
            }
        }
        snprintf(out->baseUrl, sizeof(out->baseUrl), "%s", base);

        for (int i = 0; i < tr.tokens.count; i++)
        {
            const Token *tok = &tr.tokens.items[i];
            if (tok->type == TOK_TAG && strcmp(tok->name, "meta") == 0 && !tok->isClosing)
            {
                const char *httpEquiv = NULL;
                if (doc_get_attr(tok, "http-equiv", &httpEquiv) && httpEquiv)
                {
                    char le[64];
                    lua_lower_buf(le, httpEquiv, sizeof(le));
                    if (strcmp(le, "refresh") == 0)
                    {
                        const char *content = NULL;
                        doc_get_attr(tok, "content", &content);
                        if (refresh_from_content(out, content ? content : "", base))
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    /* 4. Build DOM and run the element walker. */
    DomResult dom;
    if (dom_build(&tr, &dom) != 0)
    {
        tokenizer_free_result(&tr);
        return -1;
    }

    Walker w;
    memset(&w, 0, sizeof(w));
    w.doc = out;
    w.opts = opts;
    w.formAction = NULL;
    w.formMethod = doc_arena_str_lower(&w, "get");
    int sbFail = strbuf_init(&w.linkText) || strbuf_init(&w.preBuf) ||
                 strbuf_init(&w.textareaBuf) || strbuf_init(&w.mathBuf);
    int werr = sbFail;

    /* formMethod default: "get" (reference state.formMethod = "get"). */
    if (!werr && walk_children(&w, dom.root) != 0)
    {
        werr = 1;
    }
    if (!werr)
    {
        walker_finish(&w);
        werr = w.error;
    }

    /* Walker teardown (buffers + frames; arena kept on success). */
    strbuf_free(&w.linkText);
    strbuf_free(&w.preBuf);
    strbuf_free(&w.textareaBuf);
    strbuf_free(&w.mathBuf);
    for (int i = 0; i < w.heapBufCount; i++)
    {
        if (w.heapBufs[i])
        {
            strbuf_free(w.heapBufs[i]);
            PLUTO_FREE(w.heapBufs[i]);
        }
    }
    if (w.heapBufs)
    {
        PLUTO_FREE(w.heapBufs);
    }
    if (w.frames)
    {
        PLUTO_FREE(w.frames);
    }
    dom_free_result(&dom);
    tokenizer_free_result(&tr);

    out->mode = mode;
    out->isReaderMode = 0;
    out->rawHtml = (char *)PLUTO_MALLOC(strlen(htmlString) + 1);
    if (out->rawHtml)
    {
        strcpy(out->rawHtml, htmlString);
    }

    if (werr || !out->rawHtml)
    {
        /* The reference throws out of Document.parse (bare <li> etc); the
         * caller gets no doc. Mirror with parseError + an emptied result. */
        doc_free_walk_output(out);
        out->parseError = 1;
        if (out->rawHtml)
        {
            /* keep the copy for the error path */
        }
        /* arena still holds objects; free it now */
        doc_arena_free_all(&w.arena);
        return 0;
    }

    /* Keep the arena (blocks/links/strings live in it). */
    DocArena *keep = (DocArena *)PLUTO_MALLOC(sizeof(DocArena));
    if (!keep)
    {
        doc_free_walk_output(out);
        doc_arena_free_all(&w.arena);
        return -1;
    }
    *keep = w.arena;
    w.arena.head = NULL;
    out->_arena = keep;
    return 0;
}

void document_free(DocParseResult *doc)
{
    if (!doc)
    {
        return;
    }
    doc_free_walk_output(doc);
    if (doc->rawHtml)
    {
        PLUTO_FREE(doc->rawHtml);
        doc->rawHtml = NULL;
    }
    if (doc->_arena)
    {
        doc_arena_free_all((DocArena *)doc->_arena);
        PLUTO_FREE(doc->_arena);
        doc->_arena = NULL;
    }
}
