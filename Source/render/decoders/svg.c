/*
 * PlutoBrowser — svg.c
 * Port of Source/render/decoders/svg.lua (reference, 451 lines).
 * See svg.h for the Lua→C map and every preserved quirk.
 *
 * Stack discipline (P22 rule): tag/attr workspaces are static; the only
 * sizable locals are the expandUses StrBuf (heap) and small scalars.
 */
#include "core/logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "pd_api.h"
#include "render/decoders/svg.h"
#include "util/strbuf.h"
#include "../core/pluto_mem.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_mem_realloc((p), 0)

/* ── Attribute map (bounded, static — Lua tables are unbounded) ───────────── */
#define SVG_MAX_ATTRS 32
#define SVG_MAX_NAME  32
#define SVG_MAX_VAL   64

typedef struct
{
    char key[SVG_MAX_NAME];
    char val[SVG_MAX_VAL];
} SvgAttr;

typedef struct
{
    SvgAttr a[SVG_MAX_ATTRS];
    int n;
} SvgAttrs;

static void attrs_clear(SvgAttrs *m)
{
    m->n = 0;
}

/* Lua: out[k] = v — set (overwrite) or insert. */
static void attrs_set(SvgAttrs *m, const char *k, size_t klen, const char *v, size_t vlen)
{
    if (klen >= SVG_MAX_NAME)
    {
        klen = SVG_MAX_NAME - 1;
    }
    if (vlen >= SVG_MAX_VAL)
    {
        vlen = SVG_MAX_VAL - 1;
    }
    for (int i = 0; i < m->n; i++)
    {
        if (strlen(m->a[i].key) == klen && memcmp(m->a[i].key, k, klen) == 0)
        {
            memcpy(m->a[i].val, v, vlen);
            m->a[i].val[vlen] = 0;
            return;
        }
    }
    if (m->n >= SVG_MAX_ATTRS)
    {
        return; /* documented C-side bound */
    }
    SvgAttr *slot = &m->a[m->n++];
    memcpy(slot->key, k, klen);
    slot->key[klen] = 0;
    memcpy(slot->val, v, vlen);
    slot->val[vlen] = 0;
}

static const char *attrs_get(const SvgAttrs *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
    {
        if (strcmp(m->a[i].key, key) == 0)
        {
            return m->a[i].val;
        }
    }
    return NULL;
}

/* Lua: `([%w%:-]+)%s*=%s*"([^"]*)"` — word/colon/hyphen keys, quoted values. */
static void svg_get_attrs(const char *s, size_t len, SvgAttrs *out)
{
    attrs_clear(out);
    /* Pass 1: double-quoted */
    for (size_t i = 0; i < len;)
    {
        /* key must start with [%w:-] */
        char c0 = s[i];
        if (!(isalnum((unsigned char)c0) || c0 == ':' || c0 == '-'))
        {
            i++;
            continue;
        }
        size_t ks = i;
        while (i < len && (isalnum((unsigned char)s[i]) || s[i] == ':' || s[i] == '-'))
        {
            i++;
        }
        size_t ke = i;
        /* %s*=%s*" */
        size_t j = i;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
        {
            j++;
        }
        if (j >= len || s[j] != '=')
        {
            continue; /* i already advanced past the key run */
        }
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
        {
            j++;
        }
        if (j < len && s[j] == '"')
        {
            j++;
            size_t vs = j;
            while (j < len && s[j] != '"')
            {
                j++;
            }
            if (j > len)
            {
                break;
            }
            attrs_set(out, s + ks, ke - ks, s + vs, j - vs);
            i = (j < len) ? j + 1 : j;
        }
        /* else: not a double-quoted match; skip this key (i advanced) */
    }
    /* Pass 2: single-quoted (Lua runs the same pattern again — later
     * assignments overwrite earlier ones) */
    for (size_t i = 0; i < len;)
    {
        char c0 = s[i];
        if (!(isalnum((unsigned char)c0) || c0 == ':' || c0 == '-'))
        {
            i++;
            continue;
        }
        size_t ks = i;
        while (i < len && (isalnum((unsigned char)s[i]) || s[i] == ':' || s[i] == '-'))
        {
            i++;
        }
        size_t ke = i;
        size_t j = i;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
        {
            j++;
        }
        if (j >= len || s[j] != '=')
        {
            continue;
        }
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
        {
            j++;
        }
        if (j < len && s[j] == '\'')
        {
            j++;
            size_t vs = j;
            while (j < len && s[j] != '\'')
            {
                j++;
            }
            if (j > len)
            {
                break;
            }
            attrs_set(out, s + ks, ke - ks, s + vs, j - vs);
            i = (j < len) ? j + 1 : j;
        }
    }
}

static int svg_is_hidden(const SvgAttrs *a)
{
    const char *d = attrs_get(a, "display");
    if (d && strcmp(d, "none") == 0)
    {
        return 1;
    }
    const char *v = attrs_get(a, "visibility");
    if (v && (strcmp(v, "hidden") == 0 || strcmp(v, "collapse") == 0))
    {
        return 1;
    }
    return 0;
}

static int svg_has_ink(const SvgAttrs *a)
{
    const char *stroke = attrs_get(a, "stroke");
    if (stroke && strcmp(stroke, "none") != 0 && strcmp(stroke, "") != 0)
    {
        return 1;
    }
    const char *fill = attrs_get(a, "fill");
    if (fill && strcmp(fill, "none") == 0)
    {
        return 0;
    }
    return 1;
}

/* Lua: parseStyle — "([^;:]+)%s*:%s*([^;]+)", key trimmed+lowered, val
 * trimmed; LAST duplicate wins (table assignment). */
static void svg_parse_style(const char *style, size_t len, SvgAttrs *out)
{
    attrs_clear(out);
    size_t i = 0;
    while (i < len)
    {
        /* next segment: up to ';' (or end) */
        size_t segEnd = i;
        while (segEnd < len && style[segEnd] != ';')
        {
            segEnd++;
        }
        /* find ':' in segment */
        size_t colon = i;
        while (colon < segEnd && style[colon] != ':')
        {
            colon++;
        }
        if (colon < segEnd)
        {
            /* prop = [i, colon), val = [colon+1, segEnd) — both may include
             * surrounding spaces; trim them. Lua's `[^;:]+` cannot be empty. */
            size_t ps = i, pe = colon;
            while (ps < pe && (style[ps] == ' ' || style[ps] == '\t'))
            {
                ps++;
            }
            while (pe > ps && (style[pe - 1] == ' ' || style[pe - 1] == '\t'))
            {
                pe--;
            }
            size_t vs = colon + 1, ve = segEnd;
            while (vs < ve && (style[vs] == ' ' || style[vs] == '\t'))
            {
                vs++;
            }
            while (ve > vs && (style[ve - 1] == ' ' || style[ve - 1] == '\t'))
            {
                ve--;
            }
            if (pe > ps)
            {
                /* lowercase the key (Lua string.lower) */
                char key[SVG_MAX_NAME];
                size_t kl = pe - ps;
                if (kl >= SVG_MAX_NAME)
                {
                    kl = SVG_MAX_NAME - 1;
                }
                for (size_t t = 0; t < kl; t++)
                {
                    key[t] = (char)tolower((unsigned char)style[ps + t]);
                }
                key[kl] = 0;
                attrs_set(out, key, kl, style + vs, ve - vs);
            }
        }
        i = segEnd + 1;
    }
}

/* Lua: mergeStyle — style wins over attributes. C: parse style into a
 * scratch map then assign each pair over the attrs map. */
static void svg_merge_style(SvgAttrs *attrs)
{
    const char *style = attrs_get(attrs, "style");
    if (!style || !*style)
    {
        return;
    }
    /* static: 1.4KB struct off the game-task stack (walker is sequential,
     * non-reentrant — same pattern as g_svgPts/g_svgCo and DocStyle). */
    static SvgAttrs st;
    svg_parse_style(style, strlen(style), &st);
    for (int i = 0; i < st.n; i++)
    {
        attrs_set(attrs, st.a[i].key, strlen(st.a[i].key), st.a[i].val, strlen(st.a[i].val));
    }
}

/* ── Path number tokenizer (character-level, verbatim algorithm) ──────────── */
#define SVG_MAX_NUMS 4096
typedef struct
{
    double v[SVG_MAX_NUMS];
    int n;
} SvgNums;

static void svg_tokenize_numbers(const char *s, size_t len, SvgNums *out)
{
    out->n = 0;
    size_t i = 0;
    while (i < len)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x2C)
        {
            i++;
            continue;
        }
        size_t start = i;
        int hasDigit = 0;
        if (c == 0x2B || c == 0x2D) /* +/- */
        {
            i++;
            if (i >= len)
            {
                break;
            }
            c = (unsigned char)s[i];
        }
        while (i < len && s[i] >= 0x30 && s[i] <= 0x39)
        {
            hasDigit = 1;
            i++;
        }
        c = (i < len) ? (unsigned char)s[i] : 0;
        if (c == 0x2E) /* '.' */
        {
            if (i + 1 < len && s[i + 1] >= 0x30 && s[i + 1] <= 0x39)
            {
                i++;
                while (i < len && s[i] >= 0x30 && s[i] <= 0x39)
                {
                    hasDigit = 1;
                    i++;
                }
            }
            else if (!hasDigit)
            {
                i++;
                while (i < len && s[i] >= 0x30 && s[i] <= 0x39)
                {
                    hasDigit = 1;
                    i++;
                }
            }
            /* else: digits then '.' with no following digit → number ends
             * before the dot (verbatim) */
        }
        c = (i < len) ? (unsigned char)s[i] : 0;
        if ((c == 0x65 || c == 0x45) && hasDigit) /* e/E */
        {
            i++;
            if (i < len)
            {
                c = (unsigned char)s[i];
                if (c == 0x2B || c == 0x2D)
                {
                    i++;
                }
                while (i < len && s[i] >= 0x30 && s[i] <= 0x39)
                {
                    i++;
                }
            }
        }
        if (i > start && hasDigit && out->n < SVG_MAX_NUMS)
        {
            char tmp[64];
            size_t tl = i - start;
            if (tl >= sizeof(tmp))
            {
                tl = sizeof(tmp) - 1;
            }
            memcpy(tmp, s + start, tl);
            tmp[tl] = 0;
            char *endp = NULL;
            double num = strtod(tmp, &endp);
            if (endp && endp != tmp)
            {
                out->v[out->n++] = num;
            }
            else
            {
                /* Lua: tonumber(sub) — if nil, not appended */
            }
        }
        if (i == start)
        {
            i = start + 1; /* skip unknown char (verbatim) */
        }
    }
}

/* ── Tag scanner state shared with the decode walk ────────────────────────── */
static char g_tagName[SVG_MAX_NAME];
static char g_tagAttrsRaw[1024];

/* 32KB-each number buffers must NOT live on the stack (device game-task
 * stack is shallow; the P22/P24 lesson). Decode is synchronous and
 * single-threaded, so BSS scratch is safe. */
static SvgNums g_svgPts;
static SvgNums g_svgCo;

/* Lua: scanTags — verbatim traversal: comments, CDATA, !DOCTYPE (!D/!d) and
 * processing instructions (?x/?X) are skipped; other tags trimmed, close and
 * self-close flags detected, name lowercased. */
static const char *svg_scan_next(const char *body, size_t len, size_t *pos,
                                 const char **tagNameOut, size_t *tagLen,
                                 SvgAttrs *attrs, int *isClose, int *isSelfClose)
{
    size_t p = *pos;
    while (p < len)
    {
        const char *s = memchr(body + p, '<', len - p);
        if (!s)
        {
            break;
        }
        size_t si = (size_t)(s - body);
        const char *e = memchr(s, '>', len - si);
        if (!e)
        {
            break;
        }
        size_t ei = (size_t)(e - body);
        p = ei + 1;
        size_t inLen = ei - si - 1;
        const char *inside = s + 1;
        const char *head = inside;
        if (inLen >= 3 && head[0] == '!' && head[1] == '-' && head[2] == '-')
        {
            const char *ce = NULL;
            for (size_t q = ei + 1; q + 2 < len; q++)
            {
                if (body[q] == '-' && body[q + 1] == '-' && body[q + 2] == '>')
                {
                    ce = body + q;
                    break;
                }
            }
            if (ce)
            {
                p = (size_t)(ce - body) + 3;
            }
            continue;
        }
        if (inLen >= 2 && head[0] == '!' && head[1] == '[')
        {
            const char *ce = NULL;
            for (size_t q = ei + 1; q + 2 < len; q++)
            {
                if (body[q] == ']' && body[q + 1] == ']' && body[q + 2] == '>')
                {
                    ce = body + q;
                    break;
                }
            }
            if (ce)
            {
                p = (size_t)(ce - body) + 3;
            }
            continue;
        }
        if (inLen >= 2 && ((head[0] == '!' && (head[1] == 'D' || head[1] == 'd')) ||
                           (head[0] == '?' && (head[1] == 'x' || head[1] == 'X'))))
        {
            continue;
        }
        /* trim whitespace (Lua: gsub "^%s*(.-)%s*$") */
        size_t ts = 0, te = inLen;
        while (ts < te && isspace((unsigned char)inside[ts]))
        {
            ts++;
        }
        while (te > ts && isspace((unsigned char)inside[te - 1]))
        {
            te--;
        }
        if (ts >= te)
        {
            continue; /* trimmed == "" */
        }
        *isClose = (inside[ts] == '/');
        size_t tb = *isClose ? ts + 1 : ts;
        size_t tbe = te;
        *isSelfClose = (inside[tbe - 1] == '/');
        if (*isSelfClose)
        {
            tbe--;
        }
        /* tag name: ^([%w%:-]+) */
        size_t ns = tb;
        while (ns < tbe && (isalnum((unsigned char)inside[ns]) || inside[ns] == ':' || inside[ns] == '-'))
        {
            ns++;
        }
        size_t nl = ns - tb;
        if (nl == 0)
        {
            continue; /* string.match failed → no visit */
        }
        if (nl >= SVG_MAX_NAME)
        {
            nl = SVG_MAX_NAME - 1;
        }
        for (size_t t = 0; t < nl; t++)
        {
            g_tagName[t] = (char)tolower((unsigned char)inside[tb + t]);
        }
        g_tagName[nl] = 0;
        /* attrStr = sub(tagBody, #tagName+1) — relative to tagBody start tb */
        size_t as = tb + nl, ae = tbe;
        size_t alen = ae > as ? ae - as : 0;
        if (alen >= sizeof(g_tagAttrsRaw))
        {
            alen = sizeof(g_tagAttrsRaw) - 1;
        }
        memcpy(g_tagAttrsRaw, inside + as, alen);
        g_tagAttrsRaw[alen] = 0;
        svg_get_attrs(g_tagAttrsRaw, alen, attrs);
        *tagNameOut = g_tagName;
        *tagLen = nl;
        *pos = p;
        return s;
    }
    return NULL;
}

/* ── <use> expansion (verbatim: re-serialize the id'd element) ────────────── */
static int strbuf_append_strz(StrBuf *sb, const char *s)
{
    return strbuf_append(sb, s);
}

/* <use>-expansion attr scratch, hoisted to BSS — two 2.1KB SvgAttrs on the
 * stack made svg_expand_uses the second-largest game-task frame (6.2KB).
 * svg_get_attrs clears its output on entry; single-threaded: safe to share. */
static SvgAttrs g_useAttrs;
static SvgAttrs g_refAttrs;

static int svg_expand_uses(const char *src, size_t len, StrBuf *out)
{
    size_t lastPos = 0;
    while (lastPos < len)
    {
        /* find <[uU][sS][eE] tag - manual scan (kept on one line: a literal
         * [^>]*\/?> pattern would terminate this block comment early) */
        size_t s = lastPos;
        int found = 0;
        while (s + 3 < len)
        {
            if (src[s] == '<' && (src[s + 1] == 'u' || src[s + 1] == 'U') &&
                (src[s + 2] == 's' || src[s + 2] == 'S') &&
                (src[s + 3] == 'e' || src[s + 3] == 'E'))
            {
                found = 1;
                break;
            }
            s++;
        }
        if (!found)
        {
            strbuf_append_n(out, src + lastPos, len - lastPos);
            break;
        }
        const char *e = memchr(src + s, '>', len - s);
        if (!e)
        {
            strbuf_append_n(out, src + lastPos, len - lastPos);
            break;
        }
        size_t ei = (size_t)(e - src);
        strbuf_append_n(out, src + lastPos, s - lastPos);
        /* Lua: getAttrs(sub(src, s + 4, e - 1)) — attrs after "<use" */
        size_t aStart = s + 4, aEnd = ei; /* sub is 1-based inclusive; aEnd = e-1 0-based exclusive */
        if (aEnd > aStart)
        {
            SvgAttrs *ua = &g_useAttrs;
            svg_get_attrs(src + aStart, aEnd - aStart, ua);
            const char *href = attrs_get(ua, "href");
            const char *xhref = attrs_get(ua, "xlink:href");
            const char *h = href ? href : xhref;
            if (h && h[0] == '#')
            {
                const char *id = h + 1;
                /* find first element whose id attr == id */
                size_t q = 0;
                int spliced = 0;
                while (q < len)
                {
                    const char *lt = memchr(src + q, '<', len - q);
                    if (!lt)
                    {
                        break;
                    }
                    size_t lti = (size_t)(lt - src);
                    const char *gt = memchr(lt, '>', len - lti);
                    if (!gt)
                    {
                        break;
                    }
                    size_t gti = (size_t)(gt - src);
                    q = gti + 1;
                    /* Lua gmatch includes EVERY <...> segment, then getAttrs
                     * parses the whole inside; emulate: parse inside text. */
                    size_t inLen = gti - lti - 1;
                    if (inLen && inLen < sizeof(g_tagAttrsRaw))
                    {
                        memcpy(g_tagAttrsRaw, lt + 1, inLen);
                        g_tagAttrsRaw[inLen] = 0;
                        SvgAttrs *ea = &g_refAttrs;
                        svg_get_attrs(g_tagAttrsRaw, inLen, ea);
                        const char *eid = attrs_get(ea, "id");
                        if (eid && strcmp(eid, id) == 0)
                        {
                            /* ref = "<" .. elTag .. elStr .. ">" where the
                             * gmatch capture is <([%w%:-]+)([^>]*)>: elTag is
                             * the name run, elStr the rest INCLUDING self-close
                             * slash if present. */
                            size_t ns = 1;
                            while (ns < inLen && (isalnum((unsigned char)g_tagAttrsRaw[ns]) ||
                                                  g_tagAttrsRaw[ns] == ':' || g_tagAttrsRaw[ns] == '-'))
                            {
                                ns++;
                            }
                            strbuf_append_strz(out, "<");
                            strbuf_append_n(out, g_tagAttrsRaw, ns);
                            strbuf_append_n(out, g_tagAttrsRaw + ns, inLen - ns);
                            strbuf_append_strz(out, ">");
                            spliced = 1;
                            break;
                        }
                    }
                }
                (void)spliced; /* Lua appends "" when no ref: nothing to do */
            }
        }
        lastPos = ei + 1;
    }
    return 0;
}

/* ── Decode ───────────────────────────────────────────────────────────────── */
#define SVG_MAX_DEPTH 128

LCDBitmap *svg_decode(const char *xml, int maxW, int maxH)
{
    logger_stack_touch();
    if (!xml || !strstr(xml, "<svg"))
    {
        return NULL;
    }
    if (maxW <= 0)
    {
        maxW = 360;
    }
    if (maxH <= 0)
    {
        maxH = 200;
    }
    size_t len = strlen(xml);

    /* Lua string.match FIRST-MATCH-ANYWHERE semantics (stroke-width trap). */
    double vbMinX = 0, vbMinY = 0, vbW = 0, vbH = 0;
    int haveVb = 0;
    {
        const char *v = strstr(xml, "viewBox");
        if (v)
        {
            /* viewBox%s*=%s*["']%s*number %s+ number %s+ number %s+ number */
            const char *p = v + 7;
            while (*p == ' ' || *p == '\t')
            {
                p++;
            }
            if (*p == '=')
            {
                p++;
                while (*p == ' ' || *p == '\t')
                {
                    p++;
                }
                if (*p == '"' || *p == '\'')
                {
                    p++;
                    int got[4] = {0};
                    double nums[4];
                    for (int k = 0; k < 4; k++)
                    {
                        while (*p == ' ' || *p == '\t')
                        {
                            p++;
                        }
                        char *endp = NULL;
                        double d = strtod(p, &endp);
                        if (endp == p)
                        {
                            break;
                        }
                        nums[k] = d;
                        got[k] = 1;
                        p = endp;
                    }
                    if (got[0] && got[1] && got[2] && got[3])
                    {
                        vbMinX = nums[0];
                        vbMinY = nums[1];
                        vbW = nums[2];
                        vbH = nums[3];
                        haveVb = 1;
                    }
                }
            }
        }
    }
    double srcW = 0, srcH = 0, minX = 0, minY = 0;
    if (haveVb)
    {
        srcW = vbW;
        srcH = vbH;
        minX = vbMinX;
        minY = vbMinY;
    }
    if (srcW <= 0 || srcH <= 0)
    {
        /* Lua: tonumber(vbW) or tonumber(wRaw) or 100 — width/height first
         * match anywhere (so stroke-width leaks in exactly the same way). */
        const char *w = strstr(xml, "width");
        const char *h = strstr(xml, "height");
        double wv = 0, hv = 0;
        int haveW = 0, haveH = 0;
        if (w)
        {
            const char *p = w + 5;
            while (*p == ' ' || *p == '\t')
            {
                p++;
            }
            if (*p == '=')
            {
                p++;
                while (*p == ' ' || *p == '\t')
                {
                    p++;
                }
                if (*p == '"' || *p == '\'')
                {
                    p++;
                    char *endp = NULL;
                    double d = strtod(p, &endp);
                    if (endp != p)
                    {
                        wv = d;
                        haveW = 1;
                    }
                }
            }
        }
        if (h)
        {
            const char *p = h + 6;
            while (*p == ' ' || *p == '\t')
            {
                p++;
            }
            if (*p == '=')
            {
                p++;
                while (*p == ' ' || *p == '\t')
                {
                    p++;
                }
                if (*p == '"' || *p == '\'')
                {
                    p++;
                    char *endp = NULL;
                    double d = strtod(p, &endp);
                    if (endp != p)
                    {
                        hv = d;
                        haveH = 1;
                    }
                }
            }
        }
        srcW = haveW ? wv : 100.0;
        srcH = haveH ? hv : 100.0;
        if (haveVb)
        {
            /* Lua reads minX/minY only from viewBox (already set above) */
        }
        else
        {
            minX = 0;
            minY = 0;
        }
    }
    if (srcW <= 0 || srcH <= 0)
    {
        return NULL;
    }

    double scale = (double)maxW / srcW;
    double sy = (double)maxH / srcH;
    if (sy < scale)
    {
        scale = sy;
    }
    if (scale > 2)
    {
        scale = 2;
    }
    int targetW = (int)floor(srcW * scale);
    int targetH = (int)floor(srcH * scale);
    if (targetW < 20)
    {
        targetW = 20;
    }
    if (targetH < 20)
    {
        targetH = 20;
    }

    /* expandUses */
    StrBuf body;
    if (strbuf_init(&body) != 0)
    {
        return NULL;
    }
    svg_expand_uses(xml, len, &body);
    const char *doc = body.data ? body.data : "";
    size_t docLen = body.len;

    PlaydateAPI *pd = pluto_pd();
    LCDBitmap *img = pd->graphics->newBitmap(targetW, targetH, (LCDColor)kColorWhite);
    if (!img)
    {
        strbuf_free(&body);
        return NULL;
    }

    pd->graphics->pushContext(img);
    /* Lua gfx.setLineWidth(1) — no C setter exists; C drawLine takes an
     * explicit width of 1 per call (same observable 1px strokes). All draw
     * calls below pass kColorBlack explicitly (no C setColor). */

    double curX = 0, curY = 0, startX = 0, startY = 0;
    double lastCtrlX = 0, lastCtrlY = 0;
    int hasPoint = 0;
    int errFlag = 0; /* Lua pcall: ellipse → gfx.drawEllipse error → nil */

    int skipDepth = 0;
    /* static: 512B + 1.4KB off the game-task stack (single decode at a time
     * by design — the SVG sync-decode path runs inside the HTTP done
     * callback, and the device gameTask stack is tiny; the P33 device run
     * overflowed exactly here). */
    static int stack[SVG_MAX_DEPTH];
    int stackN = 0;
    int drawn = 0;

#define TX(v) (int)floor(((v) - minX) * scale)
#define TY(v) (int)floor(((v) - minY) * scale)

    size_t pos = 0;
    const char *tagName = NULL;
    size_t tagLen = 0;
    static SvgAttrs attrs; /* 1.4KB off the game-task stack (see above) */
    int isClose = 0, isSelfClose = 0;
    while (!errFlag &&
           svg_scan_next(doc, docLen, &pos, &tagName, &tagLen, &attrs, &isClose, &isSelfClose))
    {
        if (!isClose)
        {
            svg_merge_style(&attrs);
            int ownHidden = svg_is_hidden(&attrs);
            int enteringSkip = (strcmp(tagName, "defs") == 0) || ownHidden;
            int container = (strcmp(tagName, "svg") == 0 || strcmp(tagName, "g") == 0 ||
                             strcmp(tagName, "a") == 0 || strcmp(tagName, "symbol") == 0 ||
                             strcmp(tagName, "mask") == 0 || strcmp(tagName, "clippath") == 0 ||
                             strcmp(tagName, "defs") == 0 || strcmp(tagName, "pattern") == 0 ||
                             strcmp(tagName, "marker") == 0 || strcmp(tagName, "switch") == 0);
            if (container && !isSelfClose)
            {
                if (stackN < SVG_MAX_DEPTH)
                {
                    stack[stackN++] = skipDepth;
                }
                if (enteringSkip)
                {
                    skipDepth++;
                }
            }
            if (!enteringSkip && skipDepth == 0)
            {
                if (strcmp(tagName, "rect") == 0 && svg_has_ink(&attrs) &&
                    attrs_get(&attrs, "x") && attrs_get(&attrs, "y") &&
                    attrs_get(&attrs, "width") && attrs_get(&attrs, "height"))
                {
                    int x = TX(strtod(attrs_get(&attrs, "x"), NULL));
                    int y = TY(strtod(attrs_get(&attrs, "y"), NULL));
                    int w = (int)floor(strtod(attrs_get(&attrs, "width"), NULL) * scale);
                    int h = (int)floor(strtod(attrs_get(&attrs, "height"), NULL) * scale);
                    if (w < 1)
                    {
                        w = 1;
                    }
                    if (h < 1)
                    {
                        h = 1;
                    }
                    const char *rx = attrs_get(&attrs, "rx");
                    const char *ry = attrs_get(&attrs, "ry");
                    if (rx || ry)
                    {
                        /* Lua parity: math.floor((tonumber(attrs["rx"]) or 2) * scale)
                         * — rx falls back to 2, ry is never consulted here. */
                        double rv = rx ? strtod(rx, NULL) : 2.0;
                        int rad = (int)floor(rv * scale);
                        if (rad < 1)
                        {
                            rad = 1;
                        }
                        if (rad > 4)
                        {
                            rad = 4;
                        }
                        pd->graphics->drawRoundRect(x, y, w, h, rad, 1, (LCDColor)kColorBlack);
                    }
                    else
                    {
                        pd->graphics->drawRect(x, y, w, h, (LCDColor)kColorBlack);
                    }
                    drawn++;
                }
                else if (strcmp(tagName, "circle") == 0 && svg_has_ink(&attrs) &&
                         attrs_get(&attrs, "cx") && attrs_get(&attrs, "cy") && attrs_get(&attrs, "r"))
                {
                    int r = (int)floor(strtod(attrs_get(&attrs, "r"), NULL) * scale);
                    if (r < 1)
                    {
                        r = 1;
                    }
                    int cx = TX(strtod(attrs_get(&attrs, "cx"), NULL));
                    int cy = TY(strtod(attrs_get(&attrs, "cy"), NULL));
                    /* Lua drawCircleAtPoint = drawEllipseInRect(cx-r, cy-r, 2r, 2r) */
                    pd->graphics->drawEllipse(cx - r, cy - r, r * 2, r * 2, 1, 0, 360,
                                              (LCDColor)kColorBlack);
                    drawn++;
                }
                else if (strcmp(tagName, "ellipse") == 0 && svg_has_ink(&attrs) &&
                         attrs_get(&attrs, "cx") && attrs_get(&attrs, "cy") &&
                         attrs_get(&attrs, "rx") && attrs_get(&attrs, "ry"))
                {
                    /* VERBATIM PARITY: the reference calls gfx.drawEllipse,
                     * which does not exist in the SDK Lua API → runtime error
                     * → pcall unwinds → decode returns nil. Reproduce the
                     * observable result (nil) exactly. */
                    errFlag = 1;
                }
                else if (strcmp(tagName, "line") == 0 && svg_has_ink(&attrs) &&
                         attrs_get(&attrs, "x1") && attrs_get(&attrs, "y1") &&
                         attrs_get(&attrs, "x2") && attrs_get(&attrs, "y2"))
                {
                    pd->graphics->drawLine(
                        TX(strtod(attrs_get(&attrs, "x1"), NULL)),
                        TY(strtod(attrs_get(&attrs, "y1"), NULL)),
                        TX(strtod(attrs_get(&attrs, "x2"), NULL)),
                        TY(strtod(attrs_get(&attrs, "y2"), NULL)), 1, (LCDColor)kColorBlack);
                    drawn++;
                }
                else if ((strcmp(tagName, "polygon") == 0 || strcmp(tagName, "polyline") == 0) &&
                         svg_has_ink(&attrs) && attrs_get(&attrs, "points"))
                {
                    SvgNums *pts = &g_svgPts;
                    svg_tokenize_numbers(attrs_get(&attrs, "points"), strlen(attrs_get(&attrs, "points")), pts);
                    for (int pi = 0; pi + 3 < pts->n; pi += 2)
                    {
                        pd->graphics->drawLine(TX(pts->v[pi]), TY(pts->v[pi + 1]),
                                               TX(pts->v[pi + 2]), TY(pts->v[pi + 3]), 1,
                                               (LCDColor)kColorBlack);
                    }
                    if (strcmp(tagName, "polygon") == 0 && pts->n >= 4)
                    {
                        pd->graphics->drawLine(TX(pts->v[pts->n - 2]), TY(pts->v[pts->n - 1]),
                                               TX(pts->v[0]), TY(pts->v[1]), 1, (LCDColor)kColorBlack);
                    }
                    drawn++;
                }
                else if (strcmp(tagName, "path") == 0 && svg_has_ink(&attrs) &&
                         attrs_get(&attrs, "d"))
                {
                    curX = 0;
                    curY = 0;
                    startX = 0;
                    startY = 0;
                    hasPoint = 0;
                    lastCtrlX = 0;
                    lastCtrlY = 0;
                    const char *d = attrs_get(&attrs, "d");
                    size_t dl = strlen(d);
                    size_t i = 0;
                    while (i < dl)
                    {
                        /* find command letter */
                        while (i < dl && !isalpha((unsigned char)d[i]))
                        {
                            i++;
                        }
                        if (i >= dl)
                        {
                            break;
                        }
                        char cmd = d[i];
                        i++;
                        /* args: up to the next letter */
                        size_t as = i;
                        while (i < dl && !isalpha((unsigned char)d[i]))
                        {
                            i++;
                        }
                        SvgNums *co = &g_svgCo;
                        svg_tokenize_numbers(d + as, i - as, co);
                        int isRel = islower((unsigned char)cmd);
                        char c = (char)toupper((unsigned char)cmd);
#define PT(px, py) (isRel ? (curX + (px)) : (px)), (isRel ? (curY + (py)) : (py))
                        if (c == 'M')
                        {
                            for (int k = 0; k + 1 < co->n; k += 2)
                            {
                                double px, py;
                                if (isRel)
                                {
                                    px = curX + co->v[k];
                                    py = curY + co->v[k + 1];
                                }
                                else
                                {
                                    px = co->v[k];
                                    py = co->v[k + 1];
                                }
                                if (k == 0)
                                {
                                    curX = px;
                                    curY = py;
                                    startX = curX;
                                    startY = curY;
                                    hasPoint = 1;
                                }
                                else
                                {
                                    pd->graphics->drawLine(TX(curX), TY(curY), TX(px), TY(py), 1, (LCDColor)kColorBlack);
                                    curX = px;
                                    curY = py;
                                }
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'L')
                        {
                            for (int k = 0; k + 1 < co->n; k += 2)
                            {
                                double px, py;
                                if (isRel)
                                {
                                    px = curX + co->v[k];
                                    py = curY + co->v[k + 1];
                                }
                                else
                                {
                                    px = co->v[k];
                                    py = co->v[k + 1];
                                }
                                pd->graphics->drawLine(TX(curX), TY(curY), TX(px), TY(py), 1, (LCDColor)kColorBlack);
                                curX = px;
                                curY = py;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'H')
                        {
                            for (int k = 0; k < co->n; k++)
                            {
                                double nx = isRel ? (curX + co->v[k]) : co->v[k];
                                pd->graphics->drawLine(TX(curX), TY(curY), TX(nx), TY(curY), 1, (LCDColor)kColorBlack);
                                curX = nx;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'V')
                        {
                            for (int k = 0; k < co->n; k++)
                            {
                                double ny = isRel ? (curY + co->v[k]) : co->v[k];
                                pd->graphics->drawLine(TX(curX), TY(curY), TX(curX), TY(ny), 1, (LCDColor)kColorBlack);
                                curY = ny;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'Z')
                        {
                            if (hasPoint)
                            {
                                pd->graphics->drawLine(TX(curX), TY(curY), TX(startX), TY(startY), 1, (LCDColor)kColorBlack);
                                curX = startX;
                                curY = startY;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'C')
                        {
                            for (int k = 0; k + 5 < co->n; k += 6)
                            {
                                double x1, y1, x2, y2, x3, y3;
                                if (isRel)
                                {
                                    x1 = curX + co->v[k];
                                    y1 = curY + co->v[k + 1];
                                    x2 = curX + co->v[k + 2];
                                    y2 = curY + co->v[k + 3];
                                    x3 = curX + co->v[k + 4];
                                    y3 = curY + co->v[k + 5];
                                }
                                else
                                {
                                    x1 = co->v[k];
                                    y1 = co->v[k + 1];
                                    x2 = co->v[k + 2];
                                    y2 = co->v[k + 3];
                                    x3 = co->v[k + 4];
                                    y3 = co->v[k + 5];
                                }
                                for (int t = 1; t <= 8; t++)
                                {
                                    double u = t / 8.0;
                                    double nx = (1 - u) * (1 - u) * (1 - u) * curX + 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u * x3;
                                    double ny = (1 - u) * (1 - u) * (1 - u) * curY + 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u * y3;
                                    pd->graphics->drawLine(TX(curX), TY(curY), TX(nx), TY(ny), 1, (LCDColor)kColorBlack);
                                    curX = nx;
                                    curY = ny;
                                }
                                lastCtrlX = x2;
                                lastCtrlY = y2;
                            }
                        }
                        else if (c == 'S')
                        {
                            for (int k = 0; k + 3 < co->n; k += 4)
                            {
                                double sx1 = curX * 2 - lastCtrlX;
                                double sy1 = curY * 2 - lastCtrlY;
                                double x2, y2, x3, y3;
                                if (isRel)
                                {
                                    x2 = curX + co->v[k];
                                    y2 = curY + co->v[k + 1];
                                    x3 = curX + co->v[k + 2];
                                    y3 = curY + co->v[k + 3];
                                }
                                else
                                {
                                    x2 = co->v[k];
                                    y2 = co->v[k + 1];
                                    x3 = co->v[k + 2];
                                    y3 = co->v[k + 3];
                                }
                                for (int t = 1; t <= 8; t++)
                                {
                                    double u = t / 8.0;
                                    double nx = (1 - u) * (1 - u) * (1 - u) * curX + 3 * (1 - u) * (1 - u) * u * sx1 + 3 * (1 - u) * u * u * x2 + u * u * u * x3;
                                    double ny = (1 - u) * (1 - u) * (1 - u) * curY + 3 * (1 - u) * (1 - u) * u * sy1 + 3 * (1 - u) * u * u * y2 + u * u * u * y3;
                                    pd->graphics->drawLine(TX(curX), TY(curY), TX(nx), TY(ny), 1, (LCDColor)kColorBlack);
                                    curX = nx;
                                    curY = ny;
                                }
                                lastCtrlX = x2;
                                lastCtrlY = y2;
                            }
                        }
                        else if (c == 'Q')
                        {
                            for (int k = 0; k + 3 < co->n; k += 4)
                            {
                                double x1, y1, x2, y2;
                                if (isRel)
                                {
                                    x1 = curX + co->v[k];
                                    y1 = curY + co->v[k + 1];
                                    x2 = curX + co->v[k + 2];
                                    y2 = curY + co->v[k + 3];
                                }
                                else
                                {
                                    x1 = co->v[k];
                                    y1 = co->v[k + 1];
                                    x2 = co->v[k + 2];
                                    y2 = co->v[k + 3];
                                }
                                for (int t = 1; t <= 6; t++)
                                {
                                    double u = t / 6.0;
                                    double nx = (1 - u) * (1 - u) * curX + 2 * (1 - u) * u * x1 + u * u * x2;
                                    double ny = (1 - u) * (1 - u) * curY + 2 * (1 - u) * u * y1 + u * u * y2;
                                    pd->graphics->drawLine(TX(curX), TY(curY), TX(nx), TY(ny), 1, (LCDColor)kColorBlack);
                                    curX = nx;
                                    curY = ny;
                                }
                                lastCtrlX = x1;
                                lastCtrlY = y1;
                            }
                        }
                        else if (c == 'T')
                        {
                            for (int k = 0; k + 1 < co->n; k += 2)
                            {
                                double tx1 = curX * 2 - lastCtrlX;
                                double ty1 = curY * 2 - lastCtrlY;
                                double x2, y2;
                                if (isRel)
                                {
                                    x2 = curX + co->v[k];
                                    y2 = curY + co->v[k + 1];
                                }
                                else
                                {
                                    x2 = co->v[k];
                                    y2 = co->v[k + 1];
                                }
                                for (int t = 1; t <= 6; t++)
                                {
                                    double u = t / 6.0;
                                    double nx = (1 - u) * (1 - u) * curX + 2 * (1 - u) * u * tx1 + u * u * x2;
                                    double ny = (1 - u) * (1 - u) * curY + 2 * (1 - u) * u * ty1 + u * u * y2;
                                    pd->graphics->drawLine(TX(curX), TY(curY), TX(nx), TY(ny), 1, (LCDColor)kColorBlack);
                                    curX = nx;
                                    curY = ny;
                                }
                                lastCtrlX = tx1;
                                lastCtrlY = ty1;
                            }
                        }
                        else if (c == 'A')
                        {
                            for (int k = 0; k + 6 < co->n; k += 7)
                            {
                                double exv = (k + 5 < co->n) ? co->v[k + 5] : 0;
                                double eyv = (k + 6 < co->n) ? co->v[k + 6] : 0;
                                double ex, ey;
                                if (isRel)
                                {
                                    ex = curX + exv;
                                    ey = curY + eyv;
                                }
                                else
                                {
                                    ex = exv;
                                    ey = eyv;
                                }
                                pd->graphics->drawLine(TX(curX), TY(curY), TX(ex), TY(ey), 1, (LCDColor)kColorBlack);
                                curX = ex;
                                curY = ey;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
#undef PT
                    }
                    drawn++;
                }
            }
        }
        else
        {
            if (strcmp(tagName, "svg") == 0 || strcmp(tagName, "g") == 0 ||
                strcmp(tagName, "a") == 0 || strcmp(tagName, "symbol") == 0 ||
                strcmp(tagName, "mask") == 0 || strcmp(tagName, "clippath") == 0 ||
                strcmp(tagName, "defs") == 0 || strcmp(tagName, "pattern") == 0 ||
                strcmp(tagName, "marker") == 0 || strcmp(tagName, "switch") == 0)
            {
                if (stackN > 0)
                {
                    skipDepth = stack[--stackN];
                }
            }
        }
    }

    pd->graphics->popContext();
    strbuf_free(&body);

    if (errFlag)
    {
        pd->graphics->freeBitmap(img);
        return NULL; /* Lua pcall parity */
    }
    if (drawn == 0)
    {
        pd->graphics->freeBitmap(img);
        return NULL;
    }
    return img;
}
