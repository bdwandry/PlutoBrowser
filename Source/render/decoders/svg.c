/*
 * PlutoBrowser — svg.c
 * Descendant of the svg.lua 1:1 port (451-line reference). The original port
 * reproduced the reference renderer EXACTLY, including its central limitation:
 * every shape was drawn as a 1px BLACK OUTLINE (gfx stroke primitives only) —
 * no fills, no grayscale, no dithering. Real-world SVG (e.g. the Inkscape
 * test_pattern at wiesmann.codiferes.net/share/bitmaps/) is fill-driven, so
 * SVG images rendered as hollow wireframes next to their dithered PNG/JPEG
 * counterparts.
 *
 * This version keeps the reference's scanner/parser semantics verbatim
 * (getAttrs double/single-quote passes, style-wins-over-attr, viewBox
 * first-match-anywhere, use-expansion, tag scan) and replaces the paint
 * stage with a proper SVG rasterizer:
 *
 *   - fill/stroke paints: #rgb/#rrggbb, rgb()/rgba(), CSS named colors,
 *     none, inherit, and url(#gradient) refs (linearGradient/radialGradient
 *     resolved to the average stop color — the 1-bit approximation).
 *   - Every fill is dithered: luminance → 8x8 Bayer coverage → LCDPattern,
 *     the same ordered-dither look the raster decoders produce (dither.c).
 *   - affine transforms (translate/scale/rotate/skew/matrix) with a
 *     graphics-state stack (fill/stroke/stroke-width/fill-rule/opacity
 *     inheritance + push/pop on containers).
 *   - true arc (A) path commands (endpoint→center parameterization,
 *     sampled polylines), polygon fills via fillPolygon (even-odd or
 *     nonzero per fill-rule), ellipse fills via fillEllipse.
 *   - <use> splicing now carries the use's transform/x/y and presentation
 *     attrs into the spliced element (the reference dropped them).
 *   - attribute values longer than 64 chars (path `d`!) are stored in a
 *     16KB per-tag value pool instead of being silently truncated.
 *
 * The old "ellipse kills the whole decode" pcall-parity quirk is gone —
 * ellipses render (that quirk made real-world SVGs return NULL).
 *
 * Pattern polarity note: the SDK does not document whether LCDPattern data
 * bits mean black or white. svg_build_patterns() self-calibrates on first
 * decode by drawing probe patterns into a scratch bitmap and reading the
 * bits back, so fills are correct under either convention (and the
 * white-underfill fallback covers non-erasing pattern semantics).
 *
 * Stack discipline (P22 rule): all sizable workspaces are static/BSS; the
 * decode is synchronous and single-threaded like every other decoder.
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
#define SVG_VAL_INLINE 64

/* Long values (path `d`, long `style`, `points`...) used to be truncated at
 * 64 chars — the visible bug that ate most of the test card's path data.
 * Values >= SVG_VAL_INLINE are copied into this per-tag pool; pointers stay
 * valid until the NEXT svg_get_attrs call (documented lifetime). */
#define SVG_POOL_SIZE 16384
static char g_valPool[SVG_POOL_SIZE];
static size_t g_valPoolPos = 0;

typedef struct
{
    char key[SVG_MAX_NAME];
    char val[SVG_VAL_INLINE]; /* inline storage (NUL-terminated) */
    char *lval;               /* set → value lives in g_valPool */
} SvgAttr;

typedef struct
{
    SvgAttr a[SVG_MAX_ATTRS];
    int n;
} SvgAttrs;

static const char *attr_val(const SvgAttr *sa)
{
    return sa->lval ? sa->lval : sa->val;
}

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
    char *dst = NULL;
    if (vlen < SVG_VAL_INLINE)
    {
        dst = NULL; /* inline */
    }
    else if (g_valPoolPos + vlen + 1 <= SVG_POOL_SIZE)
    {
        dst = g_valPool + g_valPoolPos; /* long value → pool */
        g_valPoolPos += vlen + 1;
    }
    for (int i = 0; i < m->n; i++)
    {
        if (strlen(m->a[i].key) == klen && memcmp(m->a[i].key, k, klen) == 0)
        {
            if (dst)
            {
                memcpy(dst, v, vlen);
                dst[vlen] = 0;
                m->a[i].lval = dst;
            }
            else
            {
                if (vlen >= SVG_VAL_INLINE)
                {
                    vlen = SVG_VAL_INLINE - 1; /* pool exhausted → truncate */
                }
                memcpy(m->a[i].val, v, vlen);
                m->a[i].val[vlen] = 0;
                m->a[i].lval = NULL;
            }
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
    if (dst)
    {
        memcpy(dst, v, vlen);
        dst[vlen] = 0;
        slot->lval = dst;
    }
    else
    {
        if (vlen >= SVG_VAL_INLINE)
        {
            vlen = SVG_VAL_INLINE - 1;
        }
        memcpy(slot->val, v, vlen);
        slot->val[vlen] = 0;
        slot->lval = NULL;
    }
}

static const char *attrs_get(const SvgAttrs *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
    {
        if (strcmp(m->a[i].key, key) == 0)
        {
            return attr_val(&m->a[i]);
        }
    }
    return NULL;
}

/* Lua: `([%w%:-]+)%s*=%s*"([^"]*)"` — word/colon/hyphen keys, quoted values.
 * Resets the long-value pool: all lval pointers die at the next call. */
static void svg_get_attrs(const char *s, size_t len, SvgAttrs *out)
{
    attrs_clear(out);
    g_valPoolPos = 0;
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

/* Style scratch: parsing appends long values to g_valPool, so the SOURCE
 * must not live in the pool (self-overlap). Copy it here first. */
static char g_styleScratch[2048];

/* Lua: mergeStyle — style wins over attributes. C: parse style into a
 * scratch map then assign each pair over the attrs map. */
static void svg_merge_style(SvgAttrs *attrs)
{
    const char *style = attrs_get(attrs, "style");
    if (!style || !*style)
    {
        return;
    }
    size_t sl = strlen(style);
    if (sl >= sizeof(g_styleScratch))
    {
        sl = sizeof(g_styleScratch) - 1;
    }
    memcpy(g_styleScratch, style, sl);
    g_styleScratch[sl] = 0;
    /* static: 1.4KB struct off the game-task stack (walker is sequential,
     * non-reentrant — same pattern as g_svgPts/g_svgCo and DocStyle). */
    static SvgAttrs st;
    svg_parse_style(g_styleScratch, sl, &st);
    for (int i = 0; i < st.n; i++)
    {
        const char *v = attr_val(&st.a[i]);
        attrs_set(attrs, st.a[i].key, strlen(st.a[i].key), v, strlen(v));
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
/* Raw attr string of the current tag — path `d` lives here too, so this
 * must hold multi-KB tags (was 1KB: the grid path lost ~2/3 of its lines). */
static char g_tagAttrsRaw[8192];

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

/* ── <use> expansion (re-serialize the id'd element, carrying the use's
 *    transform / x / y / presentation attrs — the reference dropped them,
 *    which stacked every tiled clone on one spot) ────────────────────────── */
static int strbuf_append_strz(StrBuf *sb, const char *s)
{
    return strbuf_append(sb, s);
}

/* <use>-expansion scratch, hoisted to BSS — svg_get_attrs clears its output
 * on entry; single-threaded: safe to share. The use's own attr values are
 * copied into fixed buffers BEFORE the referenced element is parsed (the
 * second svg_get_attrs resets the value pool). */
static SvgAttrs g_useAttrs;
static SvgAttrs g_refAttrs;
static char g_useHref[SVG_VAL_INLINE];
static char g_useTransform[512];
static char g_useStyle[512];
static char g_useFill[SVG_VAL_INLINE];
static char g_useStroke[SVG_VAL_INLINE];
static char g_useFOpacity[SVG_VAL_INLINE];
static char g_useSOpacity[SVG_VAL_INLINE];
static char g_useOpacity[SVG_VAL_INLINE];
static char g_useSWidth[SVG_VAL_INLINE];
static char g_useFRule[SVG_VAL_INLINE];
static char g_refAttrsBuf[4096];   /* referenced element's raw attr string */
static char g_injectBuf[1024];     /* composed transform / merged style */
static char g_useXY[2][SVG_VAL_INLINE]; /* use's x/y (translate offset) */

static void svg_copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!src)
    {
        dst[0] = 0;
        return;
    }
    size_t l = strlen(src);
    if (l >= cap)
    {
        l = cap - 1;
    }
    memcpy(dst, src, l);
    dst[l] = 0;
}

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
            /* copy everything we may need BEFORE the ref scan resets the
             * value pool */
            svg_copy_bounded(g_useHref, sizeof(g_useHref),
                             attrs_get(ua, "href") ? attrs_get(ua, "href")
                                                   : attrs_get(ua, "xlink:href"));
            svg_copy_bounded(g_useTransform, sizeof(g_useTransform), attrs_get(ua, "transform"));
            svg_copy_bounded(g_useStyle, sizeof(g_useStyle), attrs_get(ua, "style"));
            svg_copy_bounded(g_useFill, sizeof(g_useFill), attrs_get(ua, "fill"));
            svg_copy_bounded(g_useStroke, sizeof(g_useStroke), attrs_get(ua, "stroke"));
            svg_copy_bounded(g_useFOpacity, sizeof(g_useFOpacity), attrs_get(ua, "fill-opacity"));
            svg_copy_bounded(g_useSOpacity, sizeof(g_useSOpacity), attrs_get(ua, "stroke-opacity"));
            svg_copy_bounded(g_useOpacity, sizeof(g_useOpacity), attrs_get(ua, "opacity"));
            svg_copy_bounded(g_useSWidth, sizeof(g_useSWidth), attrs_get(ua, "stroke-width"));
            svg_copy_bounded(g_useFRule, sizeof(g_useFRule), attrs_get(ua, "fill-rule"));
            svg_copy_bounded(g_useXY[0], sizeof(g_useXY[0]), attrs_get(ua, "x"));
            svg_copy_bounded(g_useXY[1], sizeof(g_useXY[1]), attrs_get(ua, "y"));
            /* x/y of 0 carry no offset */
            if (g_useXY[0][0] && strtod(g_useXY[0], NULL) == 0)
            {
                g_useXY[0][0] = 0;
            }
            if (g_useXY[1][0] && strtod(g_useXY[1], NULL) == 0)
            {
                g_useXY[1][0] = 0;
            }
            const char *h = g_useHref[0] ? g_useHref : NULL;
            if (h && h[0] == '#')
            {
                const char *id = h + 1;
                /* find first element whose id attr == id */
                size_t q = 0;
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
                            /* re-serialize: "<" elTag elStr ">" — elTag is
                             * the name run, elStr the rest MINUS any trailing
                             * self-close slash (we re-add it cleanly after
                             * injecting the use's attrs). */
                            size_t ns = 1;
                            while (ns < inLen && (isalnum((unsigned char)g_tagAttrsRaw[ns]) ||
                                                  g_tagAttrsRaw[ns] == ':' || g_tagAttrsRaw[ns] == '-'))
                            {
                                ns++;
                            }
                            /* copy the raw attr string, strip trailing "/" */
                            size_t rawLen = inLen - ns;
                            if (rawLen >= sizeof(g_refAttrsBuf))
                            {
                                rawLen = sizeof(g_refAttrsBuf) - 1;
                            }
                            memcpy(g_refAttrsBuf, g_tagAttrsRaw + ns, rawLen);
                            g_refAttrsBuf[rawLen] = 0;
                            while (rawLen > 0 && isspace((unsigned char)g_refAttrsBuf[rawLen - 1]))
                            {
                                g_refAttrsBuf[--rawLen] = 0;
                            }
                            int selfClosed = 0;
                            if (rawLen > 0 && g_refAttrsBuf[rawLen - 1] == '/')
                            {
                                selfClosed = 1;
                                g_refAttrsBuf[--rawLen] = 0;
                                while (rawLen > 0 && isspace((unsigned char)g_refAttrsBuf[rawLen - 1]))
                                {
                                    g_refAttrsBuf[--rawLen] = 0;
                                }
                            }
                            /* compose the injected attrs */
                            g_injectBuf[0] = 0;
                            size_t inj = 0;
                            /* transform: translate(x,y) then use's transform,
                             * then the element's own (rightmost applies first) */
                            const char *origT = attrs_get(ea, "transform");
                            const char *origStyle = attrs_get(ea, "style");
                            /* build transform value */
                            {
                                char tBuf[768];
                                tBuf[0] = 0;
                                size_t tb = 0;
                                if (g_useXY[0][0] || g_useXY[1][0])
                                {
                                    int n = snprintf(tBuf, sizeof(tBuf), "translate(%s,%s)",
                                                     g_useXY[0][0] ? g_useXY[0] : "0",
                                                     g_useXY[1][0] ? g_useXY[1] : "0");
                                    if (n > 0)
                                    {
                                        tb = (size_t)n;
                                    }
                                }
                                if (g_useTransform[0])
                                {
                                    if (tb && tb < sizeof(tBuf) - 1)
                                    {
                                        tBuf[tb++] = ' ';
                                    }
                                    size_t tl = strlen(g_useTransform);
                                    if (tl > sizeof(tBuf) - 1 - tb)
                                    {
                                        tl = sizeof(tBuf) - 1 - tb;
                                    }
                                    memcpy(tBuf + tb, g_useTransform, tl);
                                    tb += tl;
                                    tBuf[tb] = 0;
                                }
                                if (origT && *origT)
                                {
                                    if (tb && tb < sizeof(tBuf) - 1)
                                    {
                                        tBuf[tb++] = ' ';
                                    }
                                    size_t tl = strlen(origT);
                                    if (tl > sizeof(tBuf) - 1 - tb)
                                    {
                                        tl = sizeof(tBuf) - 1 - tb;
                                    }
                                    memcpy(tBuf + tb, origT, tl);
                                    tb += tl;
                                    tBuf[tb] = 0;
                                }
                                if (tBuf[0])
                                {
                                    inj += (size_t)snprintf(g_injectBuf + inj, sizeof(g_injectBuf) - inj,
                                                            " transform=\"%s\"", tBuf);
                                    if (inj >= sizeof(g_injectBuf))
                                    {
                                        inj = sizeof(g_injectBuf) - 1;
                                    }
                                }
                            }
                            /* style: merge — element's own first, use's wins */
                            if (g_useStyle[0])
                            {
                                char sBuf[1024];
                                if (origStyle && *origStyle)
                                {
                                    snprintf(sBuf, sizeof(sBuf), "%s;%s", origStyle, g_useStyle);
                                }
                                else
                                {
                                    svg_copy_bounded(sBuf, sizeof(sBuf), g_useStyle);
                                }
                                inj += (size_t)snprintf(g_injectBuf + inj, sizeof(g_injectBuf) - inj,
                                                        " style=\"%s\"", sBuf);
                                if (inj >= sizeof(g_injectBuf))
                                {
                                    inj = sizeof(g_injectBuf) - 1;
                                }
                            }
#define INJECT_SIMPLE(field, name)                                                    \
    if (field[0])                                                                     \
    {                                                                                 \
        inj += (size_t)snprintf(g_injectBuf + inj, sizeof(g_injectBuf) - inj,         \
                                " " name "=\"%s\"", field);                           \
        if (inj >= sizeof(g_injectBuf))                                               \
        {                                                                             \
            inj = sizeof(g_injectBuf) - 1;                                            \
        }                                                                             \
    }
                            INJECT_SIMPLE(g_useFill, "fill")
                            INJECT_SIMPLE(g_useStroke, "stroke")
                            INJECT_SIMPLE(g_useFOpacity, "fill-opacity")
                            INJECT_SIMPLE(g_useSOpacity, "stroke-opacity")
                            INJECT_SIMPLE(g_useOpacity, "opacity")
                            INJECT_SIMPLE(g_useSWidth, "stroke-width")
                            INJECT_SIMPLE(g_useFRule, "fill-rule")
#undef INJECT_SIMPLE
                            strbuf_append_strz(out, "<");
                            strbuf_append_n(out, g_tagAttrsRaw, ns);
                            if (rawLen > 0)
                            {
                                strbuf_append_n(out, g_refAttrsBuf, rawLen);
                            }
                            if (inj > 0)
                            {
                                strbuf_append_n(out, g_injectBuf, inj);
                            }
                            strbuf_append_strz(out, selfClosed ? "/>" : ">");
                            break;
                        }
                    }
                }
            }
        }
        lastPos = ei + 1;
    }
    return 0;
}

/* ── Color parsing ────────────────────────────────────────────────────────── */
typedef struct
{
    float r, g, b; /* 0..255 */
    float a;       /* 0..1 */
    unsigned char none;
} SvgColor;

static const struct
{
    const char *name;
    unsigned char r, g, b;
} svg_colors[] = {
    {"black", 0, 0, 0},           {"white", 255, 255, 255},
    {"red", 255, 0, 0},           {"lime", 0, 255, 0},
    {"blue", 0, 0, 255},          {"yellow", 255, 255, 0},
    {"cyan", 0, 255, 255},        {"aqua", 0, 255, 255},
    {"magenta", 255, 0, 255},     {"fuchsia", 255, 0, 255},
    {"silver", 192, 192, 192},    {"gray", 128, 128, 128},
    {"grey", 128, 128, 128},      {"maroon", 128, 0, 0},
    {"olive", 128, 128, 0},       {"green", 0, 128, 0},
    {"purple", 128, 0, 128},      {"teal", 0, 128, 128},
    {"navy", 0, 0, 128},          {"orange", 255, 165, 0},
    {"pink", 255, 192, 203},      {"brown", 165, 42, 42},
    {"gold", 255, 215, 0},        {"indigo", 75, 0, 130},
    {"violet", 238, 130, 238},    {"lightgray", 211, 211, 211},
    {"lightgrey", 211, 211, 211}, {"darkgray", 169, 169, 169},
    {"darkgrey", 169, 169, 169},  {"gainsboro", 220, 220, 220},
    {"whitesmoke", 245, 245, 245},
};

static int svg_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* Parse a color value (not url()/none — callers pre-handle those). */
static void svg_parse_color(const char *s, SvgColor *out)
{
    out->r = out->g = out->b = 0;
    out->a = 1.0f;
    out->none = 0;
    if (!s || !*s)
    {
        return; /* black */
    }
    char buf[64];
    size_t l = strlen(s);
    if (l >= sizeof(buf))
    {
        l = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < l; i++)
    {
        buf[i] = (char)tolower((unsigned char)s[i]);
    }
    buf[l] = 0;
    /* trim */
    char *p = buf;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (p[0] == '#')
    {
        int n = 0, v[8];
        for (int i = 1; i < (int)sizeof(v) + 1 && p[i]; i++)
        {
            int h = svg_hex_nibble(p[i]);
            if (h < 0)
            {
                break;
            }
            v[n++] = h;
        }
        if (n == 3)
        {
            out->r = (float)(v[0] * 17);
            out->g = (float)(v[1] * 17);
            out->b = (float)(v[2] * 17);
        }
        else if (n >= 6)
        {
            out->r = (float)(v[0] * 16 + v[1]);
            out->g = (float)(v[2] * 16 + v[3]);
            out->b = (float)(v[4] * 16 + v[5]);
            if (n >= 8)
            {
                out->a = (float)(v[6] * 16 + v[7]) / 255.0f;
            }
        }
        return;
    }
    if (!strncmp(p, "rgb(", 4) || !strncmp(p, "rgba(", 5))
    {
        const char *q = strchr(p, '(') + 1;
        double c[4] = {0, 0, 0, 1};
        int n = 0;
        while (*q && n < 4)
        {
            char *endp = NULL;
            double d = strtod(q, &endp);
            if (endp == q)
            {
                break;
            }
            q = endp;
            while (*q == ' ' || *q == '\t')
            {
                q++;
            }
            if (*q == '%')
            {
                d = d * 255.0 / 100.0;
                q++;
            }
            c[n++] = d;
            while (*q && *q != ',' && *q != ')')
            {
                q++;
            }
            if (*q == ',')
            {
                q++;
            }
            else
            {
                break;
            }
        }
        out->r = (float)c[0];
        out->g = (float)c[1];
        out->b = (float)c[2];
        if (n >= 4)
        {
            out->a = (float)c[3];
        }
        return;
    }
    for (size_t i = 0; i < sizeof(svg_colors) / sizeof(svg_colors[0]); i++)
    {
        if (strcmp(p, svg_colors[i].name) == 0)
        {
            out->r = (float)svg_colors[i].r;
            out->g = (float)svg_colors[i].g;
            out->b = (float)svg_colors[i].b;
            return;
        }
    }
    /* unknown → black (SVG fallback for an invalid paint is none, but black
     * matches the wireframe look users expect from unparsed colors) */
}

/* ── Gradients (pre-pass) ─────────────────────────────────────────────────── */
#define SVG_MAX_GRADS 8
typedef struct
{
    char id[40];
    char href[40];
    float lum;   /* averaged stop luminance 0..255 */
    float alpha; /* averaged stop alpha */
    int nstops;
} SvgGrad;
static SvgGrad g_grads[SVG_MAX_GRADS];
static int g_gradN = 0;

/* luminance — same weights as the raster decoders (dither.c) */
static float svg_luminance(float r, float g, float b)
{
    return (float)((int)(r * 306 + g * 601 + b * 117) >> 10);
}

/* Scan the raw document for linearGradient/radialGradient stops and resolve
 * href chains (Inkscape style: a gradient with xlink:href inherits stops). */
static void svg_collect_gradients(const char *xml, size_t len)
{
    g_gradN = 0;
    memset(g_grads, 0, sizeof(g_grads));
    size_t pos = 0;
    const char *tagName = NULL;
    size_t tagLen = 0;
    static SvgAttrs ga; /* BSS: 1.4KB off the game-task stack */
    int isClose = 0, isSelfClose = 0;
    int gradDepth = 0;
    int cur = -1;
    double rs = 0, gs = 0, bs = 0, as = 0;
    int ns = 0;
    while (svg_scan_next(xml, len, &pos, &tagName, &tagLen, &ga, &isClose, &isSelfClose))
    {
        int isGrad = (strcmp(tagName, "lineargradient") == 0 ||
                      strcmp(tagName, "radialgradient") == 0);
        if (isClose)
        {
            if (isGrad && gradDepth > 0)
            {
                if (cur >= 0 && ns > 0 && g_grads[cur].nstops == 0)
                {
                    g_grads[cur].lum = svg_luminance((float)(rs / ns), (float)(gs / ns),
                                                     (float)(bs / ns));
                    g_grads[cur].alpha = (float)(as / ns);
                    g_grads[cur].nstops = ns;
                }
                gradDepth--;
                cur = -1;
                rs = gs = bs = as = 0;
                ns = 0;
            }
            continue;
        }
        svg_merge_style(&ga);
        if (isGrad)
        {
            if (gradDepth == 0 && g_gradN < SVG_MAX_GRADS)
            {
                SvgGrad *g = &g_grads[g_gradN++];
                memset(g, 0, sizeof(*g));
                const char *gid = attrs_get(&ga, "id");
                if (gid)
                {
                    svg_copy_bounded(g->id, sizeof(g->id), gid);
                }
                const char *href = attrs_get(&ga, "xlink:href");
                if (!href)
                {
                    href = attrs_get(&ga, "href");
                }
                if (href && href[0] == '#')
                {
                    svg_copy_bounded(g->href, sizeof(g->href), href + 1);
                }
                if (!isSelfClose)
                {
                    cur = g_gradN - 1;
                    gradDepth++;
                }
                else
                {
                    cur = -1;
                }
            }
            else if (!isSelfClose)
            {
                gradDepth++; /* nested gradient — track depth, ignore */
            }
        }
        else if (strcmp(tagName, "stop") == 0 && gradDepth > 0 && cur >= 0)
        {
            const char *sc = attrs_get(&ga, "stop-color");
            if (sc)
            {
                SvgColor c;
                svg_parse_color(sc, &c);
                float sa = 1.0f;
                const char *so = attrs_get(&ga, "stop-opacity");
                if (so)
                {
                    sa = (float)strtod(so, NULL);
                }
                if (sa < 0)
                {
                    sa = 0;
                }
                if (sa > 1)
                {
                    sa = 1;
                }
                /* premultiplied average */
                rs += (double)c.r * sa;
                gs += (double)c.g * sa;
                bs += (double)c.b * sa;
                as += sa;
                ns++;
                if (ns >= 64)
                {
                    break;
                }
            }
        }
    }
    /* resolve href chains (up to 4 hops) */
    for (int round = 0; round < 4; round++)
    {
        for (int i = 0; i < g_gradN; i++)
        {
            if (g_grads[i].nstops == 0 && g_grads[i].href[0])
            {
                for (int j = 0; j < g_gradN; j++)
                {
                    if (j != i && g_grads[j].nstops > 0 &&
                        strcmp(g_grads[j].id, g_grads[i].href) == 0)
                    {
                        g_grads[i].lum = g_grads[j].lum;
                        g_grads[i].alpha = g_grads[j].alpha;
                        g_grads[i].nstops = g_grads[j].nstops;
                        break;
                    }
                }
            }
        }
    }
}

/* ── Paints & graphics state ──────────────────────────────────────────────── */
typedef struct
{
    float lum;           /* raw color luminance 0..255 */
    float alpha;         /* paint alpha 0..1 (color alpha * fill-opacity) */
    unsigned char none;  /* paint = none → skip */
} SvgPaint;

typedef struct
{
    float a, b, c, d, e, f; /* x' = a*x + c*y + e ; y' = b*x + d*y + f */
} SvgMat;

typedef struct
{
    SvgMat m;
    SvgPaint fill, stroke;
    float strokeW; /* user units */
    int evenOdd;
    float opacity; /* cumulative group opacity 0..1 */
} SvgState;

#define SVG_MAX_DEPTH 128
static SvgState g_stk[SVG_MAX_DEPTH];

static void svg_mat_identity(SvgMat *m)
{
    m->a = 1;
    m->b = 0;
    m->c = 0;
    m->d = 1;
    m->e = 0;
    m->f = 0;
}

/* r = A ∘ B (B applied to points first, then A) */
static void svg_mat_compose(SvgMat *r, const SvgMat *A, const SvgMat *B)
{
    SvgMat t;
    t.a = A->a * B->a + A->c * B->b;
    t.b = A->b * B->a + A->d * B->b;
    t.c = A->a * B->c + A->c * B->d;
    t.d = A->b * B->c + A->d * B->d;
    t.e = A->a * B->e + A->c * B->f + A->e;
    t.f = A->b * B->e + A->d * B->f + A->f;
    *r = t;
}

static void svg_mat_apply(const SvgMat *m, double x, double y, double *ox, double *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* Parse an SVG transform list; returns 0 if nothing parsed. */
static int svg_parse_transform(const char *s, SvgMat *out)
{
    svg_mat_identity(out);
    SvgMat acc;
    svg_mat_identity(&acc);
    int any = 0;
    size_t i = 0;
    size_t len = s ? strlen(s) : 0;
    while (i < len)
    {
        while (i < len && !isalpha((unsigned char)s[i]))
        {
            i++;
        }
        size_t ws = i;
        while (i < len && (isalnum((unsigned char)s[i])))
        {
            i++;
        }
        if (ws == i)
        {
            break;
        }
        char name[16];
        size_t nl = i - ws;
        if (nl >= sizeof(name))
        {
            nl = sizeof(name) - 1;
        }
        for (size_t t = 0; t < nl; t++)
        {
            name[t] = (char)tolower((unsigned char)s[ws + t]);
        }
        name[nl] = 0;
        while (i < len && s[i] != '(')
        {
            if (!isspace((unsigned char)s[i]) && s[i] != ',')
            {
                break;
            }
            i++;
        }
        if (i >= len || s[i] != '(')
        {
            continue;
        }
        i++;
        double v[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        while (i < len && s[i] != ')' && n < 6)
        {
            char *endp = NULL;
            double d = strtod(s + i, &endp);
            if (endp == s + i)
            {
                i++; /* skip junk */
                continue;
            }
            v[n++] = d;
            i = (size_t)(endp - s);
            while (i < len && (s[i] == ',' || s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            {
                i++;
            }
        }
        if (i < len && s[i] == ')')
        {
            i++;
        }
        SvgMat t;
        svg_mat_identity(&t);
        if (strcmp(name, "translate") == 0 && n >= 1)
        {
            t.e = v[0];
            t.f = (n >= 2) ? v[1] : 0;
        }
        else if (strcmp(name, "scale") == 0 && n >= 1)
        {
            t.a = v[0];
            t.d = (n >= 2) ? v[1] : v[0];
        }
        else if (strcmp(name, "matrix") == 0 && n >= 6)
        {
            t.a = v[0];
            t.b = v[1];
            t.c = v[2];
            t.d = v[3];
            t.e = v[4];
            t.f = v[5];
        }
        else if (strcmp(name, "rotate") == 0 && n >= 1)
        {
            double ang = v[0] * 3.14159265358979323846 / 180.0;
            double cs = cos(ang), sn = sin(ang);
            if (n >= 3)
            {
                /* rotate(a, cx, cy) = T(cx,cy) ∘ R ∘ T(-cx,-cy) */
                SvgMat r1, r2, r3;
                svg_mat_identity(&r1);
                r1.e = v[2 - 1 + 1]; /* cx */
                r1.f = v[2 + 1];     /* cy */
                svg_mat_identity(&r2);
                r2.a = cs;
                r2.b = sn;
                r2.c = -sn;
                r2.d = cs;
                svg_mat_identity(&r3);
                r3.e = -v[3];
                r3.f = -v[4];
                SvgMat tmp;
                svg_mat_compose(&tmp, &r2, &r3);
                svg_mat_compose(&t, &r1, &tmp);
            }
            else
            {
                t.a = cs;
                t.b = sn;
                t.c = -sn;
                t.d = cs;
            }
        }
        else if (strcmp(name, "skewx") == 0 && n >= 1)
        {
            t.c = tan(v[0] * 3.14159265358979323846 / 180.0);
        }
        else if (strcmp(name, "skewy") == 0 && n >= 1)
        {
            t.b = tan(v[0] * 3.14159265358979323846 / 180.0);
        }
        else
        {
            continue; /* unknown transform — skip */
        }
        /* left-to-right list: acc = acc ∘ t (t applied first) */
        svg_mat_compose(&acc, &acc, &t);
        any = 1;
    }
    *out = acc;
    return any;
}

/* Resolve a paint value (fill=/stroke=) against the gradient table. */
static void svg_resolve_paint(const char *s, SvgPaint *out)
{
    out->lum = 0;
    out->alpha = 1.0f;
    out->none = 0;
    if (!s)
    {
        return; /* absent → keep inherited (caller decides) */
    }
    char buf[SVG_VAL_INLINE];
    size_t l = strlen(s);
    if (l >= sizeof(buf))
    {
        l = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < l; i++)
    {
        buf[i] = (char)tolower((unsigned char)s[i]);
    }
    buf[l] = 0;
    const char *p = buf;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (strcmp(p, "none") == 0 || strcmp(p, "transparent") == 0)
    {
        out->none = 1;
        return;
    }
    if (strcmp(p, "inherit") == 0)
    {
        out->none = 2; /* signal: keep inherited — caller checks */
        return;
    }
    if (strncmp(p, "url(", 4) == 0)
    {
        const char *h = strchr(p, '#');
        if (h)
        {
            char id[40];
            size_t il = 0;
            h++;
            while (h[il] && h[il] != ')' && il < sizeof(id) - 1)
            {
                id[il] = h[il];
                il++;
            }
            id[il] = 0;
            for (int i = 0; i < g_gradN; i++)
            {
                if (g_grads[i].nstops > 0 && strcmp(g_grads[i].id, id) == 0)
                {
                    out->lum = g_grads[i].lum;
                    out->alpha = g_grads[i].alpha;
                    out->none = 0;
                    return;
                }
            }
        }
        out->none = 1; /* unresolved paint server → treat as none */
        return;
    }
    if (strcmp(p, "currentcolor") == 0)
    {
        return; /* black */
    }
    SvgColor c;
    svg_parse_color(p, &c);
    if (c.none)
    {
        out->none = 1;
        return;
    }
    out->lum = svg_luminance(c.r, c.g, c.b);
    out->alpha = c.a;
}

/* Apply presentation attributes from a tag onto a state (elements + groups). */
static void svg_state_apply(SvgState *st, const SvgAttrs *attrs)
{
    const char *tr = attrs_get(attrs, "transform");
    if (tr)
    {
        SvgMat t;
        if (svg_parse_transform(tr, &t))
        {
            svg_mat_compose(&st->m, &st->m, &t);
        }
    }
    const char *op = attrs_get(attrs, "opacity");
    if (op)
    {
        float a = (float)strtod(op, NULL);
        if (a < 0)
        {
            a = 0;
        }
        if (a > 1)
        {
            a = 1;
        }
        st->opacity *= a;
    }
    const char *fill = attrs_get(attrs, "fill");
    if (fill)
    {
        SvgPaint p;
        svg_resolve_paint(fill, &p);
        if (p.none != 2) /* 2 = inherit → keep current */
        {
            st->fill = p;
        }
    }
    const char *fo = attrs_get(attrs, "fill-opacity");
    if (fo)
    {
        float a = (float)strtod(fo, NULL);
        if (a < 0)
        {
            a = 0;
        }
        if (a > 1)
        {
            a = 1;
        }
        st->fill.alpha *= a;
    }
    const char *stroke = attrs_get(attrs, "stroke");
    if (stroke)
    {
        SvgPaint p;
        svg_resolve_paint(stroke, &p);
        if (p.none != 2)
        {
            st->stroke = p;
        }
    }
    const char *so = attrs_get(attrs, "stroke-opacity");
    if (so)
    {
        float a = (float)strtod(so, NULL);
        if (a < 0)
        {
            a = 0;
        }
        if (a > 1)
        {
            a = 1;
        }
        st->stroke.alpha *= a;
    }
    const char *sw = attrs_get(attrs, "stroke-width");
    if (sw)
    {
        double w = strtod(sw, NULL);
        if (w > 0)
        {
            st->strokeW = (float)w;
        }
    }
    const char *fr = attrs_get(attrs, "fill-rule");
    if (fr && strstr(fr, "evenodd"))
    {
        st->evenOdd = 1;
    }
}

/* ── Dither patterns (8x8 Bayer coverage → LCDPattern) ────────────────────── */
static uint8_t g_bayerPat[65][16]; /* data rows + opaque mask rows */
static int g_patReady = 0;
static int g_patOpaque = 1; /* probe result: patterns erase (0-bits → white) */

/* Standard 8x8 ordered-dither (Bayer) threshold matrix, 0..63. */
static const uint8_t g_bayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

/*
 * Build 65 coverage patterns (index = count of black pixels per 8x8 tile).
 * The SDK docs do not pin down LCDPattern data-bit polarity, so after
 * building with "1 bit = black pixel" intent we PROBE: draw the 12.5%-black
 * pattern into a white scratch bitmap and count white bits (1 = white in the
 * frame plane, per dither.c). count 224 → polarity as built; 32/256 → the
 * data bits mean white → invert every data byte. A second probe (87.5%
 * pattern over black) decides whether 0-bits erase (opaque) or no-op; if
 * they no-op, fills pre-paint the shape solid white so light fills still
 * erase what is underneath.
 */
static void svg_build_patterns(PlaydateAPI *pd)
{
    if (g_patReady || !pd)
    {
        return;
    }
    g_patReady = 1;
    memset(g_bayerPat, 0, sizeof(g_bayerPat));
    for (int cov = 0; cov <= 64; cov++)
    {
        for (int y = 0; y < 8; y++)
        {
            uint8_t row = 0;
            for (int x = 0; x < 8; x++)
            {
                if (g_bayer8[y][x] < cov)
                {
                    row |= (uint8_t)(0x80 >> x);
                }
            }
            g_bayerPat[cov][y] = row;
            g_bayerPat[cov][8 + y] = 0xff; /* opaque mask */
        }
    }
    /* polarity probe */
    LCDBitmap *t = pd->graphics->newBitmap(16, 16, (LCDColor)kColorWhite);
    if (t)
    {
        pd->graphics->pushContext(t);
        pd->graphics->fillRect(0, 0, 16, 16, (LCDColor)(uintptr_t)&g_bayerPat[8][0]);
        pd->graphics->popContext();
        int bw = 0, bh = 0, rb = 0;
        uint8_t *mask = NULL, *data = NULL;
        pd->graphics->getBitmapData(t, &bw, &bh, &rb, &mask, &data);
        if (data)
        {
            int whites = 0;
            for (int y = 0; y < bh; y++)
            {
                for (int x = 0; x < bw; x++)
                {
                    if (data[(size_t)y * rb + (x >> 3)] & (0x80 >> (x & 7)))
                    {
                        whites++;
                    }
                }
            }
            if (whites < 160 || whites > 248)
            {
                /* data bit 1 = white → invert the table (mask untouched) */
                for (int cov = 0; cov <= 64; cov++)
                {
                    for (int y = 0; y < 8; y++)
                    {
                        g_bayerPat[cov][y] = (uint8_t)~g_bayerPat[cov][y];
                    }
                }
            }
        }
        pd->graphics->freeBitmap(t);
    }
    /* erase-semantics probe: 87.5% pattern over solid black */
    t = pd->graphics->newBitmap(16, 16, (LCDColor)kColorBlack);
    if (t)
    {
        pd->graphics->pushContext(t);
        pd->graphics->fillRect(0, 0, 16, 16, (LCDColor)(uintptr_t)&g_bayerPat[56][0]);
        pd->graphics->popContext();
        int bw = 0, bh = 0, rb = 0;
        uint8_t *mask = NULL, *data = NULL;
        pd->graphics->getBitmapData(t, &bw, &bh, &rb, &mask, &data);
        if (data)
        {
            int whites = 0;
            for (int y = 0; y < bh; y++)
            {
                for (int x = 0; x < bw; x++)
                {
                    if (data[(size_t)y * rb + (x >> 3)] & (0x80 >> (x & 7)))
                    {
                        whites++;
                    }
                }
            }
            /* opaque erase → ≈12.5% white (32 of 256 px); one-sided
             * (0-bits no-op) → 0 white: nothing was erased */
            g_patOpaque = (whites >= 16) ? 1 : 0;
        }
        pd->graphics->freeBitmap(t);
    }
    logger_log("SVGDEC patterns ready (polarity calibrated, opaque=%d)", g_patOpaque);
}

/*
 * Paint → drawable color op.
 * Returns 0 = skip, 1 = single draw with *out, 2 = draw solid-white
 * underfill first, then *out (non-erasing pattern semantics).
 */
static int svg_paint_op(const SvgPaint *p, float groupAlpha, LCDColor *out)
{
    if (p->none)
    {
        return 0;
    }
    float a = p->alpha * groupAlpha;
    if (a <= 0.004f)
    {
        return 0;
    }
    if (a > 1.0f)
    {
        a = 1.0f;
    }
    float eff = 255.0f - a * (255.0f - p->lum); /* blend toward white bg */
    int cov = (int)((255.0f - eff) * 64.0f / 255.0f + 0.5f);
    if (cov <= 0)
    {
        *out = (LCDColor)kColorWhite; /* opaque white — still erases */
        return 1;
    }
    if (cov >= 64)
    {
        *out = (LCDColor)kColorBlack;
        return 1;
    }
    *out = (LCDColor)(uintptr_t)&g_bayerPat[cov][0];
    return g_patOpaque ? 1 : 2;
}

/* ── Polygon fill buffer (path subpath accumulation) ──────────────────────── */
#define SVG_MAX_POLY_PTS 2048
/* R64: maximum vertices SUBMITTED to pd->graphics->fillPolygon in one
 * call. Real-world SVG paths (enwiki-25.svg receipted: submits of 962,
 * 1393 and 1036 RAW curve vertices) emit long runs of pixel-identical
 * points from the curve→screen math, and the OS's fillPolygon crashes
 * the gameTask (stack overflow receipted 2026-10-26, device) on that
 * data shape — the crash point MOVED between instrumented builds (a
 * timing-dependent corruption class) while identical vector content
 * returned. Submit-time normalization: consecutive-duplicate points
 * dropped + even-sample thinned to the cap below, applied per submit
 * (polyline STROKES keep the raw set; the wireframe draws were always
 * receipted-crash-free). */
#define SVG_POLY_SUBMIT_CAP 512
static int g_poly[2 * SVG_MAX_POLY_PTS];
static int g_polyN = 0;

static void svg_poly_reset(void)
{
    g_polyN = 0;
}

/* R64: normalize the accumulated polygon IN PLACE before a fillPolygon
 * submit — drop consecutive pixel-identical vertices (curve bloat), then
 * even-sample thin to SVG_POLY_SUBMIT_CAP. Returns the submitted count. */
static int svg_poly_submit_prep(void)
{
    int m = 0;
    for (int i = 0; i < g_polyN; i++)
    {
        if (m == 0 || g_poly[2 * m] != g_poly[2 * i] ||
            g_poly[2 * m + 1] != g_poly[2 * i + 1])
        {
            g_poly[2 * m] = g_poly[2 * i];
            g_poly[2 * m + 1] = g_poly[2 * i + 1];
            m++;
        }
    }
    g_polyN = m;
    if (g_polyN > SVG_POLY_SUBMIT_CAP)
    {
        for (int k = 0; k < SVG_POLY_SUBMIT_CAP; k++)
        {
            long idx = ((long)k * (long)g_polyN) / SVG_POLY_SUBMIT_CAP;
            g_poly[2 * k] = g_poly[2 * idx];
            g_poly[2 * k + 1] = g_poly[2 * idx + 1];
        }
        g_polyN = SVG_POLY_SUBMIT_CAP;
    }
    return g_polyN;
}

static void svg_poly_add(int x, int y)
{
    if (g_polyN < SVG_MAX_POLY_PTS)
    {
        g_poly[2 * g_polyN] = x;
        g_poly[2 * g_polyN + 1] = y;
        g_polyN++;
    }
}

/* ── Decode ───────────────────────────────────────────────────────────────── */
static double g_vbMinX = 0, g_vbMinY = 0, g_svgScale = 1;

#define TX(v) (int)floor(((v) - g_vbMinX) * g_svgScale)
#define TY(v) (int)floor(((v) - g_vbMinY) * g_svgScale)

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

    PlaydateAPI *pd = pluto_pd();
    if (!pd)
    {
        return NULL;
    }
    logger_log("[svgd] enter %zu bytes (P22 TEMP)", len); /* TEMP R64 */
    logger_stack_touch();
    svg_build_patterns(pd);
    svg_collect_gradients(xml, len);

    /* expandUses */
    StrBuf body;
    if (strbuf_init(&body) != 0)
    {
        return NULL;
    }
    svg_expand_uses(xml, len, &body);
    const char *doc = body.data ? body.data : "";
    size_t docLen = body.len;
    logger_log("[svgd] uses expanded -> %zu px, grads=%d (P22 TEMP)",
               docLen, g_gradN);

    LCDBitmap *img = pd->graphics->newBitmap(targetW, targetH, (LCDColor)kColorWhite);
    if (!img)
    {
        strbuf_free(&body);
        return NULL;
    }

    pd->graphics->pushContext(img);
    logger_log("[svgd] draw start %dx%d scale=%.3f (P22 TEMP)",
               targetW, targetH, scale);

    g_vbMinX = minX;
    g_vbMinY = minY;
    g_svgScale = scale;

    double curX = 0, curY = 0, startX = 0, startY = 0;
    double lastCtrlX = 0, lastCtrlY = 0;
    int hasPoint = 0;

    /* root graphics state */
    int stkN = 1;
    {
        SvgState *root = &g_stk[0];
        svg_mat_identity(&root->m);
        root->fill.lum = 0;
        root->fill.alpha = 1.0f;
        root->fill.none = 0; /* SVG default fill = black */
        root->stroke.lum = 0;
        root->stroke.alpha = 1.0f;
        root->stroke.none = 1; /* SVG default stroke = none */
        root->strokeW = 1.0f;
        root->evenOdd = 0;
        root->opacity = 1.0f;
    }
    int skipDepth = 0;
    int stackSkip[SVG_MAX_DEPTH];
    int stackN = 0;
    int drawn = 0;

    size_t pos = 0;
    const char *tagName = NULL;
    size_t tagLen = 0;
    static SvgAttrs attrs; /* 1.4KB off the game-task stack (see above) */
    int isClose = 0, isSelfClose = 0;
    int svgdOps = 0; /* TEMP R64 */
    while (svg_scan_next(doc, docLen, &pos, &tagName, &tagLen, &attrs, &isClose, &isSelfClose))
    {
        if (++svgdOps % 32 == 0)
        {
            logger_log("[svgd] el#%d tag=%.10s peak=%u (P22 TEMP)",
                       svgdOps, tagName ? tagName : "?", logger_stack_peak());
        }
        if (!isClose)
        {
            svg_merge_style(&attrs);
            int ownHidden = svg_is_hidden(&attrs);
            int enteringSkip = (strcmp(tagName, "defs") == 0) ||
                               (strcmp(tagName, "mask") == 0) ||
                               (strcmp(tagName, "clippath") == 0) ||
                               (strcmp(tagName, "pattern") == 0) ||
                               (strcmp(tagName, "marker") == 0) ||
                               ownHidden;
            int container = (strcmp(tagName, "svg") == 0 || strcmp(tagName, "g") == 0 ||
                             strcmp(tagName, "a") == 0 || strcmp(tagName, "symbol") == 0 ||
                             strcmp(tagName, "mask") == 0 || strcmp(tagName, "clippath") == 0 ||
                             strcmp(tagName, "defs") == 0 || strcmp(tagName, "pattern") == 0 ||
                             strcmp(tagName, "marker") == 0 || strcmp(tagName, "switch") == 0);
            if (container && !isSelfClose)
            {
                if (stackN < SVG_MAX_DEPTH)
                {
                    stackSkip[stackN++] = skipDepth;
                }
                if (enteringSkip)
                {
                    skipDepth++;
                }
                if (stkN < SVG_MAX_DEPTH)
                {
                    g_stk[stkN] = g_stk[stkN - 1];
                    svg_state_apply(&g_stk[stkN], &attrs);
                    stkN++;
                }
            }
            if (!enteringSkip && skipDepth == 0 && stkN > 0)
            {
                /* shape state = copy of current group state + own attrs */
                SvgState S = g_stk[stkN - 1];
                svg_state_apply(&S, &attrs);
                LCDColor col;
                int sw = (int)(S.strokeW * scale + 0.5);
                if (sw < 1)
                {
                    sw = 1;
                }
                int matAxis = (S.m.b == 0.0f && S.m.c == 0.0f);

                if (strcmp(tagName, "rect") == 0 &&
                    attrs_get(&attrs, "x") && attrs_get(&attrs, "y") &&
                    attrs_get(&attrs, "width") && attrs_get(&attrs, "height"))
                {
                    double x0 = strtod(attrs_get(&attrs, "x"), NULL);
                    double y0 = strtod(attrs_get(&attrs, "y"), NULL);
                    double rw = strtod(attrs_get(&attrs, "width"), NULL);
                    double rh = strtod(attrs_get(&attrs, "height"), NULL);
                    const char *rx = attrs_get(&attrs, "rx");
                    const char *ry = attrs_get(&attrs, "ry");
                    if (matAxis)
                    {
                        double ux0 = S.m.a * x0 + S.m.e;
                        double uy0 = S.m.d * y0 + S.m.f;
                        double ux1 = S.m.a * (x0 + rw) + S.m.e;
                        double uy1 = S.m.d * (y0 + rh) + S.m.f;
                        if (ux1 < ux0)
                        {
                            double t = ux0;
                            ux0 = ux1;
                            ux1 = t;
                        }
                        if (uy1 < uy0)
                        {
                            double t = uy0;
                            uy0 = uy1;
                            uy1 = t;
                        }
                        int X = TX(ux0), Y = TY(uy0);
                        int W = (int)floor((ux1 - ux0) * scale);
                        int H = (int)floor((uy1 - uy0) * scale);
                        if (W < 1)
                        {
                            W = 1;
                        }
                        if (H < 1)
                        {
                            H = 1;
                        }
                        int op = svg_paint_op(&S.fill, S.opacity, &col);
                        if (op)
                        {
                            if (op == 2)
                            {
                                pd->graphics->fillRect(X, Y, W, H, (LCDColor)kColorWhite);
                            }
                            pd->graphics->fillRect(X, Y, W, H, col);
                            drawn++;
                        }
                        op = svg_paint_op(&S.stroke, S.opacity, &col);
                        if (op)
                        {
                            int rad = 0;
                            if (rx || ry)
                            {
                                /* Lua parity: rx falls back to 2, ry never
                                 * consulted; rad<=0 → square corners */
                                double rv = rx ? strtod(rx, NULL) : 2.0;
                                rad = (int)floor(rv * scale);
                            }
                            if (rad >= 1)
                            {
                                if (rad > 4)
                                {
                                    rad = 4;
                                }
                                pd->graphics->drawRoundRect(X, Y, W, H, rad, sw, col);
                            }
                            else
                            {
                                pd->graphics->drawRect(X, Y, W, H, col);
                            }
                            drawn++;
                        }
                    }
                    else
                    {
                        /* rotated/skewed rect → quad fill + outline */
                        double cxr[4], cyr[4];
                        const double px[4] = {x0, x0 + rw, x0 + rw, x0};
                        const double py[4] = {y0, y0, y0 + rh, y0 + rh};
                        for (int i = 0; i < 4; i++)
                        {
                            svg_mat_apply(&S.m, px[i], py[i], &cxr[i], &cyr[i]);
                            svg_poly_add(TX(cxr[i]), TY(cyr[i]));
                        }
                        int op = svg_paint_op(&S.fill, S.opacity, &col);
                        if (op)
                        {
                            if (op == 2)
                            {
                                logger_log("[svgd] fp n=%d (P22 TEMP)", g_polyN);
                                pd->graphics->fillPolygon(4, g_poly, (LCDColor)kColorWhite,
                                                          kPolygonFillNonZero);
                            }
                            logger_log("[svgd] fp n=%d (P22 TEMP)", g_polyN);
                            pd->graphics->fillPolygon(4, g_poly, col,
                                                      S.evenOdd ? kPolygonFillEvenOdd
                                                                : kPolygonFillNonZero);
                            drawn++;
                        }
                        op = svg_paint_op(&S.stroke, S.opacity, &col);
                        if (op)
                        {
                            for (int i = 0; i < 4; i++)
                            {
                                int j = (i + 1) & 3;
                                pd->graphics->drawLine(g_poly[2 * i], g_poly[2 * i + 1],
                                                       g_poly[2 * j], g_poly[2 * j + 1], sw, col);
                            }
                            drawn++;
                        }
                        svg_poly_reset();
                    }
                }
                else if (strcmp(tagName, "circle") == 0 &&
                         attrs_get(&attrs, "cx") && attrs_get(&attrs, "cy") &&
                         attrs_get(&attrs, "r"))
                {
                    double cx = strtod(attrs_get(&attrs, "cx"), NULL);
                    double cy = strtod(attrs_get(&attrs, "cy"), NULL);
                    double r = strtod(attrs_get(&attrs, "r"), NULL);
                    double ucx, ucy;
                    svg_mat_apply(&S.m, cx, cy, &ucx, &ucy);
                    int X = TX(ucx), Y = TY(ucy);
                    int opFill = svg_paint_op(&S.fill, S.opacity, &col);
                    int opStroke = svg_paint_op(&S.stroke, S.opacity, &col);
                    if (matAxis)
                    {
                        int srx = (int)floor(r * fabs((double)S.m.a) * scale);
                        int sry = (int)floor(r * fabs((double)S.m.d) * scale);
                        if (srx < 1)
                        {
                            srx = 1;
                        }
                        if (sry < 1)
                        {
                            sry = 1;
                        }
                        if (opFill)
                        {
                            if (opFill == 2)
                            {
                                pd->graphics->fillEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                          0, 360, (LCDColor)kColorWhite);
                            }
                            pd->graphics->fillEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                      0, 360, col);
                            drawn++;
                        }
                        if (opStroke)
                        {
                            pd->graphics->drawEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                      sw, 0, 360, col);
                            drawn++;
                        }
                    }
                    else
                    {
                        /* non-axis-aligned circle → 32-gon */
                        svg_poly_reset();
                        for (int i = 0; i < 32; i++)
                        {
                            double th = i * 3.14159265358979323846 / 16.0;
                            double ux = cx + r * cos(th);
                            double uy = cy + r * sin(th);
                            double tx2, ty2;
                            svg_mat_apply(&S.m, ux, uy, &tx2, &ty2);
                            svg_poly_add(TX(tx2), TY(ty2));
                        }
                        if (opFill)
                        {
                            if (opFill == 2)
                            {
                                    pd->graphics->fillPolygon(g_polyN, g_poly, (LCDColor)kColorWhite,
                                                          kPolygonFillNonZero);
                            }
                            pd->graphics->fillPolygon(g_polyN, g_poly, col,
                                                      S.evenOdd ? kPolygonFillEvenOdd
                                                                : kPolygonFillNonZero);
                            drawn++;
                        }
                        if (opStroke)
                        {
                            for (int i = 0; i < 32; i++)
                            {
                                int j = (i + 1) & 31;
                                pd->graphics->drawLine(g_poly[2 * i], g_poly[2 * i + 1],
                                                       g_poly[2 * j], g_poly[2 * j + 1], sw, col);
                            }
                            drawn++;
                        }
                    }
                }
                else if (strcmp(tagName, "ellipse") == 0 &&
                         attrs_get(&attrs, "cx") && attrs_get(&attrs, "cy") &&
                         attrs_get(&attrs, "rx") && attrs_get(&attrs, "ry"))
                {
                    /* The reference renderer aborted the whole decode on
                     * ellipse (gfx.drawEllipse does not exist in the SDK Lua
                     * API → pcall nil). Real-world SVGs use ellipses; render
                     * them instead of failing. */
                    double cx = strtod(attrs_get(&attrs, "cx"), NULL);
                    double cy = strtod(attrs_get(&attrs, "cy"), NULL);
                    double rxu = strtod(attrs_get(&attrs, "rx"), NULL);
                    double ryu = strtod(attrs_get(&attrs, "ry"), NULL);
                    double ucx, ucy;
                    svg_mat_apply(&S.m, cx, cy, &ucx, &ucy);
                    int X = TX(ucx), Y = TY(ucy);
                    int opFill = svg_paint_op(&S.fill, S.opacity, &col);
                    int opStroke = svg_paint_op(&S.stroke, S.opacity, &col);
                    if (matAxis)
                    {
                        int srx = (int)floor(rxu * fabs((double)S.m.a) * scale);
                        int sry = (int)floor(ryu * fabs((double)S.m.d) * scale);
                        if (srx < 1)
                        {
                            srx = 1;
                        }
                        if (sry < 1)
                        {
                            sry = 1;
                        }
                        if (opFill)
                        {
                            if (opFill == 2)
                            {
                                pd->graphics->fillEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                          0, 360, (LCDColor)kColorWhite);
                            }
                            pd->graphics->fillEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                      0, 360, col);
                            drawn++;
                        }
                        if (opStroke)
                        {
                            pd->graphics->drawEllipse(X - srx, Y - sry, srx * 2, sry * 2,
                                                      sw, 0, 360, col);
                            drawn++;
                        }
                    }
                    else
                    {
                        svg_poly_reset();
                        for (int i = 0; i < 32; i++)
                        {
                            double th = i * 3.14159265358979323846 / 16.0;
                            double ux = cx + rxu * cos(th);
                            double uy = cy + ryu * sin(th);
                            double tx2, ty2;
                            svg_mat_apply(&S.m, ux, uy, &tx2, &ty2);
                            svg_poly_add(TX(tx2), TY(ty2));
                        }
                        if (opFill)
                        {
                            if (opFill == 2)
                            {
                                    pd->graphics->fillPolygon(g_polyN, g_poly, (LCDColor)kColorWhite,
                                                          kPolygonFillNonZero);
                            }
                            pd->graphics->fillPolygon(g_polyN, g_poly, col,
                                                      S.evenOdd ? kPolygonFillEvenOdd
                                                                : kPolygonFillNonZero);
                            drawn++;
                        }
                        if (opStroke)
                        {
                            for (int i = 0; i < 32; i++)
                            {
                                int j = (i + 1) & 31;
                                pd->graphics->drawLine(g_poly[2 * i], g_poly[2 * i + 1],
                                                       g_poly[2 * j], g_poly[2 * j + 1], sw, col);
                            }
                            drawn++;
                        }
                    }
                }
                else if (strcmp(tagName, "line") == 0 &&
                         attrs_get(&attrs, "x1") && attrs_get(&attrs, "y1") &&
                         attrs_get(&attrs, "x2") && attrs_get(&attrs, "y2"))
                {
                    if (svg_paint_op(&S.stroke, S.opacity, &col))
                    {
                        double x1 = strtod(attrs_get(&attrs, "x1"), NULL);
                        double y1 = strtod(attrs_get(&attrs, "y1"), NULL);
                        double x2 = strtod(attrs_get(&attrs, "x2"), NULL);
                        double y2 = strtod(attrs_get(&attrs, "y2"), NULL);
                        double ux1, uy1, ux2, uy2;
                        svg_mat_apply(&S.m, x1, y1, &ux1, &uy1);
                        svg_mat_apply(&S.m, x2, y2, &ux2, &uy2);
                        pd->graphics->drawLine(TX(ux1), TY(uy1), TX(ux2), TY(uy2), sw, col);
                        drawn++;
                    }
                }
                else if ((strcmp(tagName, "polygon") == 0 || strcmp(tagName, "polyline") == 0) &&
                         attrs_get(&attrs, "points"))
                {
                    SvgNums *pts = &g_svgPts;
                    svg_tokenize_numbers(attrs_get(&attrs, "points"),
                                         strlen(attrs_get(&attrs, "points")), pts);
                    svg_poly_reset();
                    for (int pi = 0; pi + 1 < pts->n; pi += 2)
                    {
                        double ux, uy;
                        svg_mat_apply(&S.m, pts->v[pi], pts->v[pi + 1], &ux, &uy);
                        svg_poly_add(TX(ux), TY(uy));
                    }
                    if (strcmp(tagName, "polygon") == 0 && g_polyN >= 3)
                    {
                        int op = svg_paint_op(&S.fill, S.opacity, &col);
                        if (op)
                        {
                            int nPre = svg_poly_submit_prep(); /* TEMP R64 */
                            logger_log("[svgd] fill %d\u2192%d (P22 TEMP)", nPre, g_polyN);
                            if (op == 2)
                            {
                                    pd->graphics->fillPolygon(g_polyN, g_poly, (LCDColor)kColorWhite,
                                                          kPolygonFillNonZero);
                            }
                            pd->graphics->fillPolygon(g_polyN, g_poly, col,
                                                      S.evenOdd ? kPolygonFillEvenOdd
                                                                : kPolygonFillNonZero);
                            drawn++;
                        }
                    }
                    if (svg_paint_op(&S.stroke, S.opacity, &col))
                    {
                        for (int pi = 0; pi + 1 < g_polyN; pi++)
                        {
                            int j = pi + 1;
                            pd->graphics->drawLine(g_poly[2 * pi], g_poly[2 * pi + 1],
                                                   g_poly[2 * j], g_poly[2 * j + 1], sw, col);
                        }
                        if (strcmp(tagName, "polygon") == 0 && g_polyN >= 2)
                        {
                            pd->graphics->drawLine(g_poly[2 * (g_polyN - 1)],
                                                   g_poly[2 * (g_polyN - 1) + 1],
                                                   g_poly[0], g_poly[1], sw, col);
                        }
                        drawn++;
                    }
                    svg_poly_reset();
                }
                else if (strcmp(tagName, "path") == 0 && attrs_get(&attrs, "d"))
                {
                    curX = 0;
                    curY = 0;
                    startX = 0;
                    startY = 0;
                    hasPoint = 0;
                    lastCtrlX = 0;
                    lastCtrlY = 0;
                    int fillOn = 0;
                    LCDColor fillCol = (LCDColor)kColorBlack;
                    int fillOp = svg_paint_op(&S.fill, S.opacity, &fillCol);
                    fillOn = (fillOp != 0);
                    LCDColor strokeCol = (LCDColor)kColorBlack;
                    int strokeOn = svg_paint_op(&S.stroke, S.opacity, &strokeCol);
                    const char *d = attrs_get(&attrs, "d");
                    size_t dl = strlen(d);
                    size_t i = 0;
                    svg_poly_reset();

                    /* flush the accumulated subpath as a filled polygon */
#define FLUSH_SUBPATH()                                                              \
    do                                                                               \
    {                                                                                \
        if (fillOn && g_polyN >= 3)                                                  \
        {                                                                            \
            int nPre = svg_poly_submit_prep(); /* TEMP R64 log kept for receipt */   \
            logger_log("[svgd] fill %d→%d (P22 TEMP)", nPre, g_polyN);              \
            if (fillOp == 2)                                                         \
            {                                                                        \
                pd->graphics->fillPolygon(g_polyN, g_poly, (LCDColor)kColorWhite,    \
                                          kPolygonFillNonZero);                      \
            }                                                                        \
            pd->graphics->fillPolygon(g_polyN, g_poly, fillCol,                      \
                                      S.evenOdd ? kPolygonFillEvenOdd                \
                                                : kPolygonFillNonZero);              \
            drawn++;                                                                 \
        }                                                                            \
        svg_poly_reset();                                                            \
    } while (0)

#define EMIT_POINT(px, py)                                            \
    do                                                                \
    {                                                                 \
        double ex_, ey_;                                              \
        svg_mat_apply(&S.m, (px), (py), &ex_, &ey_);                  \
        svg_poly_add(TX(ex_), TY(ey_));                               \
    } while (0)

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
                                    FLUSH_SUBPATH();
                                    curX = px;
                                    curY = py;
                                    startX = curX;
                                    startY = curY;
                                    hasPoint = 1;
                                    EMIT_POINT(curX, curY);
                                }
                                else
                                {
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(px), TY(py), sw, strokeCol);
                                    }
                                    EMIT_POINT(px, py);
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
                                if (strokeOn)
                                {
                                    pd->graphics->drawLine(TX(curX), TY(curY),
                                                           TX(px), TY(py), sw, strokeCol);
                                }
                                EMIT_POINT(px, py);
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
                                if (strokeOn)
                                {
                                    pd->graphics->drawLine(TX(curX), TY(curY),
                                                           TX(nx), TY(curY), sw, strokeCol);
                                }
                                EMIT_POINT(nx, curY);
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
                                if (strokeOn)
                                {
                                    pd->graphics->drawLine(TX(curX), TY(curY),
                                                           TX(curX), TY(ny), sw, strokeCol);
                                }
                                EMIT_POINT(curX, ny);
                                curY = ny;
                            }
                            lastCtrlX = curX;
                            lastCtrlY = curY;
                        }
                        else if (c == 'Z')
                        {
                            if (strokeOn && hasPoint)
                            {
                                pd->graphics->drawLine(TX(curX), TY(curY),
                                                       TX(startX), TY(startY), sw, strokeCol);
                            }
                            if (hasPoint)
                            {
                                EMIT_POINT(startX, startY);
                            }
                            FLUSH_SUBPATH();
                            curX = startX;
                            curY = startY;
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
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(nx), TY(ny), sw, strokeCol);
                                    }
                                    EMIT_POINT(nx, ny);
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
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(nx), TY(ny), sw, strokeCol);
                                    }
                                    EMIT_POINT(nx, ny);
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
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(nx), TY(ny), sw, strokeCol);
                                    }
                                    EMIT_POINT(nx, ny);
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
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(nx), TY(ny), sw, strokeCol);
                                    }
                                    EMIT_POINT(nx, ny);
                                    curX = nx;
                                    curY = ny;
                                }
                                lastCtrlX = tx1;
                                lastCtrlY = ty1;
                            }
                        }
                        else if (c == 'A')
                        {
                            /* real arcs: endpoint → center parameterization
                             * (SVG 1.1 F.6.5), sampled as polylines */
                            for (int k = 0; k + 6 < co->n; k += 7)
                            {
                                double rxr = co->v[k];
                                double ryr = co->v[k + 1];
                                double phi = co->v[k + 2] * 3.14159265358979323846 / 180.0;
                                double fa = co->v[k + 3];
                                double fs = co->v[k + 4];
                                double exv = co->v[k + 5];
                                double eyv = co->v[k + 6];
                                if (isRel)
                                {
                                    exv = curX + exv;
                                    eyv = curY + eyv;
                                }
                                double x1 = curX, y1 = curY, x2 = exv, y2 = eyv;
                                if (rxr == 0 || ryr == 0)
                                {
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(x1), TY(y1), TX(x2), TY(y2),
                                                               sw, strokeCol);
                                    }
                                    EMIT_POINT(x2, y2);
                                    curX = x2;
                                    curY = y2;
                                    continue;
                                }
                                if (rxr < 0)
                                {
                                    rxr = -rxr;
                                }
                                if (ryr < 0)
                                {
                                    ryr = -ryr;
                                }
                                double cph = cos(phi), sph = sin(phi);
                                double dx = (x1 - x2) / 2.0, dy = (y1 - y2) / 2.0;
                                double x1p = cph * dx + sph * dy;
                                double y1p = -sph * dx + cph * dy;
                                double lam = x1p * x1p / (rxr * rxr) + y1p * y1p / (ryr * ryr);
                                if (lam > 1)
                                {
                                    double sq = sqrt(lam);
                                    rxr *= sq;
                                    ryr *= sq;
                                }
                                double sign = (fa != fs) ? 1.0 : -1.0;
                                double num = rxr * rxr * ryr * ryr -
                                             rxr * rxr * y1p * y1p - ryr * ryr * x1p * x1p;
                                double den = rxr * rxr * y1p * y1p + ryr * ryr * x1p * x1p;
                                double cfac = 0;
                                if (den > 0)
                                {
                                    cfac = sign * sqrt((num < 0 ? 0 : num) / den);
                                }
                                double cxp = (rxr * y1p / ryr) * cfac;
                                double cyp = (-ryr * x1p / rxr) * cfac;
                                double ccx = cph * cxp - sph * cyp + (x1 + x2) / 2.0;
                                double ccy = sph * cxp + cph * cyp + (y1 + y2) / 2.0;
                                double th1 = atan2((y1 - ccy) / ryr, (x1 - ccx) / rxr);
                                double th2 = atan2((y2 - ccy) / ryr, (x2 - ccx) / rxr);
                                double dth = th2 - th1;
                                if (dth > 0 && !fs)
                                {
                                    dth -= 2 * 3.14159265358979323846;
                                }
                                if (dth < 0 && fs)
                                {
                                    dth += 2 * 3.14159265358979323846;
                                }
                                /* sample density ~ every 2 screen px */
                                double rmax = (rxr > ryr ? rxr : ryr) * scale;
                                int steps = (int)(fabs(dth) * rmax / 2.0);
                                if (steps < 8)
                                {
                                    steps = 8;
                                }
                                if (steps > 512)
                                {
                                    steps = 512;
                                }
                                for (int t = 1; t <= steps; t++)
                                {
                                    double th = th1 + dth * (double)t / (double)steps;
                                    double ux = ccx + rxr * cos(th) * cph - ryr * sin(th) * sph;
                                    double uy = ccy + rxr * cos(th) * sph + ryr * sin(th) * cph;
                                    if (strokeOn)
                                    {
                                        pd->graphics->drawLine(TX(curX), TY(curY),
                                                               TX(ux), TY(uy), sw, strokeCol);
                                    }
                                    EMIT_POINT(ux, uy);
                                    curX = ux;
                                    curY = uy;
                                }
                                lastCtrlX = curX;
                                lastCtrlY = curY;
                            }
                        }
#undef PT
                    }
                    FLUSH_SUBPATH();
                    if (strokeOn)
                    {
                        drawn++; /* path stroked (even with no fill) */
                    }
#undef EMIT_POINT
#undef FLUSH_SUBPATH
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
                    skipDepth = stackSkip[--stackN];
                }
                if (stkN > 1)
                {
                    stkN--;
                }
            }
        }
    }
    logger_log("[svgd] walk done ops=%d drawn=%d (P22 TEMP)",
               svgdOps, drawn);

    pd->graphics->popContext();
    strbuf_free(&body);

    if (drawn == 0)
    {
        pd->graphics->freeBitmap(img);
        logger_log("SVGDEC drew nothing → NULL");
        return NULL;
    }
    {
        int iw = 0, ih = 0;
        pd->graphics->getBitmapData(img, &iw, &ih, NULL, NULL, NULL);
        logger_log("SVGDEC ok %dx%d drawn=%d grads=%d", iw, ih, drawn, g_gradN);
    }
    return img;
}
