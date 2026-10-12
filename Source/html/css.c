/*
 * PlutoBrowser — css.c
 * Minimal general-purpose CSS engine. See css.h for the scope contract.
 *
 * Design notes:
 *   - The <style> scanner mirrors jsext.c's jsbridge_scan_scripts byte-walk
 *     (the tokenizer skips <style> content, so the DOM never sees it).
 *   - The parser is forgiving like real browsers: comments stripped, junk
 *     declarations skipped, unusable selectors drop only themselves. A bad
 *     rule never aborts its sheet.
 *   - Matching implements what general lightweight sites actually use
 *     (type/class/id/descendant/groups) inside device budgets; everything
 *     else degrades to "selector skipped", never to a crash or OOM.
 *   - Cascade is per property: among matching rules the highest specificity
 *     wins, later sheet order breaks ties (CSS author-origin ordering).
 *   - No allocation: caller owns every buffer (DocParseResult).
 */
#include "css.h"

#include <string.h>

/* ── tiny utils ───────────────────────────────────────────────────────────── */
static int css_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static char css_low(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int css_eq_ncase(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (css_low(a[i]) != css_low(b[i]))
        {
            return 0;
        }
    }
    return 1;
}

static const char *css_find_from(const char *h, const char *e, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || (size_t)(e - h) < n)
    {
        return NULL;
    }
    for (const char *p = h; p + (ptrdiff_t)n <= e; p++)
    {
        if (*p == needle[0] && memcmp(p, needle, n) == 0)
        {
            return p;
        }
    }
    return NULL;
}

/* ── 1. <style> scanner (byte walk; jsext.c pattern) ──────────────────────── */
int css_scan_sheets(const char *html, CssSheet *sheets, int sheetMax)
{
    if (!html || !sheets || sheetMax <= 0)
    {
        return 0;
    }
    const char *pos = html;
    const char *end = html + strlen(html);
    int found = 0;

    while (pos < end)
    {
        const char *lt = NULL;
        const char *gt = NULL;
        for (const char *p = pos; p < end; p++)
        {
            if (*p == '<')
            {
                lt = p;
                gt = css_find_from(p, end, ">");
                break;
            }
        }
        if (!lt || !gt)
        {
            break;
        }
        const char *b = lt + 1;
        /* tag name "style" (case-insensitive, exact word) */
        if (gt - b >= 5 && css_eq_ncase(b, "style", 5) &&
            (b + 5 == gt || css_isspace(b[5])))
        {
            const char *body = gt + 1;
            /* Find the real close tag: "</style" must be followed by
             * whitespace or '>' — "</stylenot>" does NOT close (HTML spec:
             * appropriate end tag). Keep scanning for a valid one. */
            const char *close = NULL;
            const char *scan = body;
            while (scan < end)
            {
                const char *cand = css_find_from(scan, end, "</style");
                if (!cand)
                {
                    break;
                }
                const char *after = cand + 7;
                if (after >= end || css_isspace(*after) || *after == '>' ||
                    *after == '/')
                {
                    close = cand;
                    break;
                }
                scan = cand + 7; /* "</stylenot…": keep looking */
            }
            const char *next;
            if (close)
            {
                if (found < sheetMax)
                {
                    sheets[found].start = body;
                    sheets[found].len = (size_t)(close - body);
                }
                found++;
                const char *cb = css_find_from(close, end, ">");
                next = cb ? cb + 1 : end;
            }
            else
            {
                /* unterminated: browsers treat the rest of the file as CSS */
                if (found < sheetMax)
                {
                    sheets[found].start = body;
                    sheets[found].len = (size_t)(end - body);
                }
                found++;
                next = end;
            }
            pos = next;
        }
        else
        {
            pos = gt + 1;
        }
    }
    return found;
}

/* ── 2. parser ────────────────────────────────────────────────────────────── */
static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && css_isspace(*p))
    {
        p++;
    }
    return p;
}

static const char *rtrim(const char *s, const char *e)
{
    while (e > s && css_isspace(e[-1]))
    {
        e--;
    }
    return e;
}

/* Strip C-style block comments in place; returns the new end pointer. */
static char *strip_comments(char *p, char *e)
{
    char *w = p;
    char *r = p;
    while (r < e)
    {
        if (r + 1 < e && r[0] == '/' && r[1] == '*')
        {
            r += 2;
            while (r + 1 < e && !(r[0] == '*' && r[1] == '/'))
            {
                r++;
            }
            r = (r + 1 < e) ? r + 2 : e;
            *w++ = ' ';
        }
        else
        {
            *w++ = *r++;
        }
    }
    return p + (w - p);
}

/* Compound boundary: whitespace ends a compound ONLY outside [...],
 * (...) and quotes — [data-x='v y'] and :nth-child(2n+1) are each one
 * compound. An unquoted > + ~ at depth 0 also ends it (div>p splits;
 * the combinator logic consumes the marker itself). */
static const char *compound_end(const char *cs, const char *z)
{
    const char *ce = cs;
    int depth = 0;
    int paren = 0;
    char quote = 0;
    while (ce < z)
    {
        char ch = *ce;
        if (quote)
        {
            if (ch == quote)
            {
                quote = 0;
            }
        }
        else if (ch == '"' || ch == '\'')
        {
            quote = ch;
        }
        else if (ch == '[')
        {
            depth++;
        }
        else if (ch == ']')
        {
            if (depth > 0)
            {
                depth--;
            }
        }
        else if (ch == '(')
        {
            paren++;
        }
        else if (ch == ')')
        {
            if (paren > 0)
            {
                paren--;
            }
        }
        else if (depth == 0 && paren == 0 &&
                 (css_isspace(ch) || ch == '>' || ch == '+' || ch == '~'))
        {
            break;
        }
        ce++;
    }
    return ce;
}

/* Strip a trailing ::before/::after (:before/:after legacy) from a
 * selector member; sets *pseudo (1/2) and returns the new end. A bare
 * "::before" (empty subject) means the universal subject — the caller
 * synthesizes it. Anything else ending in ':' (pseudo-classes) is left
 * for parse_compound to reject. */
static const char *strip_pseudo(const char *s, const char *z,
                                unsigned char *pseudo)
{
    static const struct
    {
        const char *suf;
        size_t n;
        unsigned char p;
    } k[] = {
        {"::before", 8, 1},
        {":before", 7, 1},
        {"::after", 7, 2},
        {":after", 6, 2},
    };
    *pseudo = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    {
        if ((size_t)(z - s) >= k[i].n &&
            css_eq_ncase(z - k[i].n, k[i].suf, k[i].n))
        {
            *pseudo = k[i].p;
            return rtrim(s, z - k[i].n);
        }
    }
    return z;
}

/* ── 2b. @media screening ─────────────────────────────────────────────────
 * A held @media's wrapper ("@media Q {" + matching "}") is blanked so
 * its rules parse as top-level (source order preserved, no pointer
 * movement — rule pointers aim into this same buffer). A dropped query
 * blanks the whole span. Nested @media recurses (AND semantics).
 * Only top-level @-positions are considered (brace depth 0); rule
 * bodies never contain at-rules in valid CSS. Runs after comment
 * stripping, before rule parsing. */

static const char *at_body_end(const char *brace, const char *e)
{
    int depth = 0;
    const char *q = brace;
    while (q < e)
    {
        if (*q == '{')
        {
            depth++;
        }
        else if (*q == '}')
        {
            if (--depth == 0)
            {
                return q;
            }
        }
        q++;
    }
    return NULL;
}

static void blank_span(char *a, char *b)
{
    while (a < b)
    {
        *a++ = ' ';
    }
}

/* "@media" word at p (case-insensitive, ident boundary after). */
static int is_media_at(const char *p, const char *e)
{
    if ((size_t)(e - p) < 6 || !css_eq_ncase(p, "@media", 6))
    {
        return 0;
    }
    if ((size_t)(e - p) == 6)
    {
        return 1;
    }
    char c = p[6];
    if ((c | 32) >= 'a' && (c | 32) <= 'z')
    {
        return 0; /* @mediascreen…: not ours */
    }
    return 1;
}

/* px number: digits + optional fraction (dropped) + "px". -1 on failure. */
static int media_px(const char *s, const char *z)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    int n = 0, have = 0;
    while (s < z && *s >= '0' && *s <= '9')
    {
        have = 1;
        n = n * 10 + (*s - '0');
        s++;
    }
    if (s < z && *s == '.')
    {
        s++;
        while (s < z && *s >= '0' && *s <= '9')
        {
            s++;
        }
    }
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    if (!have || (size_t)(z - s) < 2 || (s[0] | 32) != 'p' ||
        (s[1] | 32) != 'x')
    {
        return -1;
    }
    return n;
}

/* Plain (non-negative) integer, -1 on failure. */
static int media_int(const char *s, const char *z)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    int n = 0, have = 0;
    while (s < z && *s >= '0' && *s <= '9')
    {
        have = 1;
        n = n * 10 + (*s - '0');
        s++;
    }
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    if (!have || s != z)
    {
        return -1;
    }
    return n;
}

/* One (feature) — s..z is the INSIDE of the parens. */
static int eval_media_feature(const char *s, const char *z)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    const char *ne = s;
    while (ne < z && (((*ne | 32) >= 'a' && (*ne | 32) <= 'z') || *ne == '-'))
    {
        ne++;
    }
    size_t nlen = (size_t)(ne - s);
    if (nlen == 0 || nlen >= 32)
    {
        return 0;
    }
    char name[32];
    for (size_t i = 0; i < nlen; i++)
    {
        name[i] = css_low(s[i]);
    }
    name[nlen] = '\0';
    const char *vs = ne;
    while (vs < z && css_isspace(*vs))
    {
        vs++;
    }
    int hasVal = (vs < z && *vs == ':');
    const char *val = hasVal ? vs + 1 : NULL;
    /* min-/max- prefix */
    int lo = 0; /* -1 max, +1 min, 0 exact/bare */
    const char *base = name;
    if (memcmp(name, "min-", 4) == 0)
    {
        lo = 1;
        base = name + 4;
    }
    else if (memcmp(name, "max-", 4) == 0)
    {
        lo = -1;
        base = name + 4;
    }
    if (strcmp(base, "width") == 0 || strcmp(base, "device-width") == 0)
    {
        if (!hasVal)
        {
            return 0;
        }
        int n = media_px(val, z);
        if (n < 0)
        {
            return 0;
        }
        int w = CSS_VIEWPORT_W;
        return lo > 0 ? w >= n : lo < 0 ? w <= n : w == n;
    }
    if (strcmp(base, "height") == 0 || strcmp(base, "device-height") == 0)
    {
        if (!hasVal)
        {
            return 0;
        }
        int n = media_px(val, z);
        if (n < 0)
        {
            return 0;
        }
        int h = CSS_VIEWPORT_H;
        return lo > 0 ? h >= n : lo < 0 ? h <= n : h == n;
    }
    if (strcmp(base, "orientation") == 0)
    {
        if (!hasVal || lo)
        {
            return 0;
        }
        while (val < z && css_isspace(*val))
        {
            val++;
        }
        const char *ve = val;
        while (ve < z && !css_isspace(*ve))
        {
            ve++;
        }
        int land = CSS_VIEWPORT_W >= CSS_VIEWPORT_H;
        if ((size_t)(ve - val) == 9 && css_eq_ncase(val, "landscape", 9))
        {
            return land;
        }
        if ((size_t)(ve - val) == 8 && css_eq_ncase(val, "portrait", 8))
        {
            return !land;
        }
        return 0;
    }
    if (strcmp(base, "monochrome") == 0)
    {
        /* 1-bit display: 1 gray bit. */
        if (!hasVal)
        {
            return 1;
        }
        int n = media_int(val, z);
        if (n < 0)
        {
            return 0;
        }
        return lo > 0 ? 1 >= n : lo < 0 ? 1 <= n : 1 == n;
    }
    if (strcmp(base, "color") == 0)
    {
        /* 0 color bits. */
        if (!hasVal)
        {
            return 0;
        }
        int n = media_int(val, z);
        if (n < 0)
        {
            return 0;
        }
        return lo > 0 ? 0 >= n : lo < 0 ? 0 <= n : 0 == n;
    }
    if (strcmp(base, "prefers-color-scheme") == 0)
    {
        /* Default rendering is dark-on-light. */
        if (!hasVal || lo)
        {
            return 0;
        }
        while (val < z && css_isspace(*val))
        {
            val++;
        }
        const char *ve = val;
        while (ve < z && !css_isspace(*ve))
        {
            ve++;
        }
        if ((size_t)(ve - val) == 5 && css_eq_ncase(val, "light", 5))
        {
            return 1;
        }
        return 0;
    }
    return 0; /* unknown feature: the query fails, never misapplies */
}

/* One comma-separated query. only- is ignored (legacy), not- negates. */
static int eval_media_query(const char *s, const char *z)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    const char *e2 = rtrim(s, z);
    if ((size_t)(e2 - s) >= 5 && css_eq_ncase(s, "only ", 5))
    {
        s += 5;
        while (s < e2 && css_isspace(*s))
        {
            s++;
        }
    }
    int neg = 0;
    if ((size_t)(e2 - s) >= 4 && css_eq_ncase(s, "not ", 4))
    {
        neg = 1;
        s += 4;
        while (s < e2 && css_isspace(*s))
        {
            s++;
        }
    }
    /* type (absent = all): ident or '(' */
    int typeOk = 0;
    int hadType = 0;
    if (s < e2 && *s == '(')
    {
        typeOk = 1;
    }
    else
    {
        const char *te = s;
        while (te < e2 && (((*te | 32) >= 'a' && (*te | 32) <= 'z')))
        {
            te++;
        }
        size_t tlen = (size_t)(te - s);
        if (tlen == 0)
        {
            return 0; /* empty query */
        }
        if (tlen == 3 && css_eq_ncase(s, "all", 3))
        {
            typeOk = 1;
        }
        else if (tlen == 6 && css_eq_ncase(s, "screen", 6))
        {
            typeOk = 1;
        }
        else if (tlen == 5 && css_eq_ncase(s, "print", 5))
        {
            /* 1-bit honors print stylesheets (built for B&W). */
            typeOk = 1;
        }
        else
        {
            typeOk = 0; /* speech, tty, unknown… */
        }
        hadType = 1;
        s = te;
    }
    /* ("and" feature)* — plus the first feature directly when no type
     * was consumed ("(max-width: …)"). A direct feature after a type
     * ("screen (…)") is malformed (missing and) and fails. */
    int featsOk = 1;
    int first = 1;
    for (;;)
    {
        while (s < e2 && css_isspace(*s))
        {
            s++;
        }
        if (s >= e2)
        {
            break;
        }
        if (*s == '(')
        {
            if (hadType || !first)
            {
                return 0;
            }
        }
        else
        {
            if ((size_t)(e2 - s) < 3 || !css_eq_ncase(s, "and", 3))
            {
                return 0; /* malformed */
            }
            s += 3;
            while (s < e2 && css_isspace(*s))
            {
                s++;
            }
            if (s >= e2 || *s != '(')
            {
                return 0;
            }
        }
        first = 0;
        /* balanced close (no nesting inside features) */
        const char *fc = s + 1;
        while (fc < e2 && *fc != ')')
        {
            fc++;
        }
        if (fc >= e2)
        {
            return 0;
        }
        if (!eval_media_feature(s + 1, fc))
        {
            featsOk = 0;
        }
        s = fc + 1;
    }
    int v = typeOk && featsOk;
    return neg ? !v : v;
}

/* Comma list (OR). Paren-aware split for safety. */
static int media_holds(const char *qs, const char *qe)
{
    const char *s = qs;
    while (s < qe)
    {
        const char *q = s;
        int paren = 0;
        while (q < qe)
        {
            if (*q == '(')
            {
                paren++;
            }
            else if (*q == ')')
            {
                if (paren > 0)
                {
                    paren--;
                }
            }
            else if (*q == ',' && paren == 0)
            {
                break;
            }
            q++;
        }
        if (eval_media_query(s, q))
        {
            return 1;
        }
        s = (q < qe) ? q + 1 : qe;
    }
    return 0;
}

static void flatten_media(char *buf, char *e, int depth)
{
    if (depth > 8)
    {
        return;
    }
    char *p = buf;
    int bdepth = 0;
    while (p < e)
    {
        if (*p == '{')
        {
            bdepth++;
        }
        else if (*p == '}')
        {
            if (bdepth > 0)
            {
                bdepth--;
            }
        }
        else if (*p == '@' && bdepth == 0 && is_media_at(p, e))
        {
            const char *brace = css_find_from(p, e, "{");
            if (!brace)
            {
                blank_span(p, e); /* malformed tail */
                return;
            }
            const char *bend = at_body_end(brace, e);
            if (!bend)
            {
                blank_span(p, e);
                return;
            }
            if (media_holds(p + 6, brace))
            {
                /* Blank THROUGH the wrapper-open brace: a surviving
                 * "{" parses as an empty selector whose body swallows
                 * the first real rule. */
                blank_span(p, (char *)brace + 1); /* wrapper open */
                blank_span((char *)bend, (char *)bend + 1); /* close */
                flatten_media((char *)brace + 1, (char *)bend, depth + 1);
                p = (char *)bend + 1;
                continue;
            }
            blank_span(p, (char *)bend + 1); /* whole span */
            p = (char *)bend + 1;
            continue;
        }
        p++;
    }
}

/* Parse one compound chunk [s,z) into out; 1 ok, 0 unusable.
 * Handles type / .class / #id / * / [attr] / [attr=value] and combinations
 * ("div.note#x[hidden]"). [~=|^$*=] operators are unusable (never
 * misapplied); at most one attribute test per compound. */
static int parse_compound(const char *s, const char *z, CssSimple *out)
{
    memset(out, 0, sizeof(*out));
    if (s >= z)
    {
        return 0;
    }
    const char *p = s;
    while (p < z)
    {
        char ch = *p;
        if (ch == '[')
        {
            if (out->attr)
            {
                return 0; /* one attribute test per compound */
            }
            const char *ce = p + 1;
            while (ce < z && *ce != ']')
            {
                ce++;
            }
            if (ce >= z)
            {
                return 0; /* unterminated [ */
            }
            /* content = p+1 .. ce: name [= value] */
            const char *ns = skip_ws(p + 1, ce);
            const char *ne = ns;
            /* attr names: letters/digits/hyphen/underscore/colon */
            while (ne < ce && (((*ne | 32) >= 'a' && (*ne | 32) <= 'z') ||
                               (*ne >= '0' && *ne <= '9') || *ne == '-' ||
                               *ne == '_' || *ne == ':'))
            {
                ne++;
            }
            if (ne == ns)
            {
                return 0; /* empty name */
            }
            const char *vs = skip_ws(ne, ce);
            if (vs < ce && *vs != '=' && *vs != ']')
            {
                /* [a~=b] / [a|=b] / … operators: unsupported */
                if ((*vs == '~' || *vs == '|' || *vs == '^' || *vs == '$' ||
                     *vs == '*') &&
                    vs + 1 < ce && vs[1] == '=')
                {
                    return 0;
                }
                return 0; /* junk inside brackets */
            }
            out->attr = ns;
            out->attrLen = (unsigned char)(ne - ns);
            if (vs < ce && *vs == '=')
            {
                const char *ws = skip_ws(vs + 1, ce);
                const char *we = rtrim(ws, ce);
                if (we > ws &&
                    ((*ws == '"' && we[-1] == '"') ||
                     (*ws == '\'' && we[-1] == '\'')))
                {
                    ws++;
                    we--;
                    we = rtrim(ws, we);
                }
                if (we <= ws)
                {
                    return 0; /* empty value */
                }
                out->attrVal = ws;
                out->attrValLen = (unsigned char)(we - ws);
            }
            p = ce + 1;
        }
        else         if (ch == '.' || ch == '#')
        {
            const char *t = p + 1;
            while (t < z && !css_isspace(*t) && *t != '.' && *t != '#' &&
                   *t != '[' && *t != ':')
            {
                t++;
            }
            if (t == p + 1)
            {
                return 0; /* dangling . or # */
            }
            if (ch == '.')
            {
                if (out->cls)
                {
                    return 0;
                }
                out->cls = p + 1;
                out->clsLen = (unsigned char)(t - (p + 1));
            }
            else
            {
                if (out->id)
                {
                    return 0;
                }
                out->id = p + 1;
                out->idLen = (unsigned char)(t - (p + 1));
            }
            p = t;
        }
        else if (ch == '*' || ((ch | 32) >= 'a' && (ch | 32) <= 'z'))
        {
            if (ch == '*')
            {
                if (out->universal || out->type)
                {
                    return 0;
                }
                out->universal = 1;
                p++;
            }
            else
            {
                if (out->type || out->universal)
                {
                    return 0;
                }
                /* Type name: letters, then letters/digits/hyphen
                 * (h1..h6, custom elements like my-widget). */
                const char *t = p;
                while (t < z &&
                       (((*t | 32) >= 'a' && (*t | 32) <= 'z') ||
                        (*t >= '0' && *t <= '9') || *t == '-'))
                {
                    t++;
                }
                if (t == p)
                {
                    return 0;
                }
                out->type = p;
                out->typeLen = (unsigned char)(t - p);
                p = t;
            }
        }
        else if (ch == ':')
        {
            /* Structural pseudo-class (single colon; :: forms are
             * stripped upstream for sheets and rejected here for QS). */
            if (p + 1 < z && p[1] == ':')
            {
                return 0;
            }
            const char *t = p + 1;
            while (t < z &&
                   (((*t | 32) >= 'a' && (*t | 32) <= 'z') || *t == '-'))
            {
                t++;
            }
            size_t ilen = (size_t)(t - (p + 1));
            if (out->pclass)
            {
                return 0; /* one pseudo-class per compound */
            }
            if (ilen == 11 && css_eq_ncase(p + 1, "first-child", 11))
            {
                out->pclass = 1;
                p = t;
            }
            else if (ilen == 10 && css_eq_ncase(p + 1, "last-child", 10))
            {
                out->pclass = 2;
                p = t;
            }
            else if (ilen == 10 && css_eq_ncase(p + 1, "only-child", 10))
            {
                out->pclass = 3;
                p = t;
            }
            else if (ilen == 4 && css_eq_ncase(p + 1, "link", 4))
            {
                out->pclass = 4;
                p = t;
            }
            else if (ilen == 9 && css_eq_ncase(p + 1, "nth-child", 9))
            {
                /* Functional: odd / even / N / An+B. */
                const char *q = t;
                while (q < z && css_isspace(*q))
                {
                    q++;
                }
                if (q >= z || *q != '(')
                {
                    return 0;
                }
                q++;
                while (q < z && css_isspace(*q))
                {
                    q++;
                }
                int na = 0, nb = 0, ok = 0;
                if ((size_t)(z - q) >= 3 && css_eq_ncase(q, "odd", 3))
                {
                    na = 2;
                    nb = 1;
                    q += 3;
                    ok = 1;
                }
                else if ((size_t)(z - q) >= 4 && css_eq_ncase(q, "even", 4))
                {
                    na = 2;
                    nb = 0;
                    q += 4;
                    ok = 1;
                }
                else
                {
                    int sign = 1;
                    if (q < z && (*q == '+' || *q == '-'))
                    {
                        if (*q == '-')
                        {
                            sign = -1;
                        }
                        q++;
                    }
                    int haveNum = 0, num = 0;
                    while (q < z && *q >= '0' && *q <= '9')
                    {
                        haveNum = 1;
                        num = num * 10 + (*q - '0');
                        q++;
                    }
                    while (q < z && css_isspace(*q))
                    {
                        q++;
                    }
                    if (q < z && (*q == 'n' || *q == 'N'))
                    {
                        na = haveNum ? sign * num : sign;
                        q++;
                        while (q < z && css_isspace(*q))
                        {
                            q++;
                        }
                        nb = 0;
                        if (q < z && (*q == '+' || *q == '-'))
                        {
                            int bsign = (*q == '-') ? -1 : 1;
                            q++;
                            while (q < z && css_isspace(*q))
                            {
                                q++;
                            }
                            int bnum = 0, haveB = 0;
                            while (q < z && *q >= '0' && *q <= '9')
                            {
                                haveB = 1;
                                bnum = bnum * 10 + (*q - '0');
                                q++;
                            }
                            if (!haveB)
                            {
                                return 0;
                            }
                            nb = bsign * bnum;
                        }
                        ok = 1;
                    }
                    else if (haveNum)
                    {
                        na = 0;
                        nb = sign * num;
                        ok = 1;
                    }
                }
                if (!ok)
                {
                    return 0;
                }
                while (q < z && css_isspace(*q))
                {
                    q++;
                }
                if (q >= z || *q != ')')
                {
                    return 0;
                }
                q++;
                if (na > 30000)
                {
                    na = 30000;
                }
                if (na < -30000)
                {
                    na = -30000;
                }
                if (nb > 30000)
                {
                    nb = 30000;
                }
                if (nb < -30000)
                {
                    nb = -30000;
                }
                out->pclass = 5;
                out->nthA = (short)na;
                out->nthB = (short)nb;
                p = q;
            }
            else
            {
                return 0; /* :hover, :visited, :empty, … unusable */
            }
        }
        else
        {
            return 0; /* digits, escapes … unusable */
        }
    }
    return 1;
}

/* Specificity packing: ids<<8 | classes<<4 | types. Attribute selectors
 * and pseudo-classes count as classes (CSS spec). */
static unsigned short compound_spec(const CssSimple *c)
{
    unsigned v = 0;
    if (c->idLen)
    {
        v += 0x0100;
    }
    if (c->clsLen)
    {
        v += 0x0010;
    }
    if (c->attrLen)
    {
        v += 0x0010;
    }
    if (c->pclass)
    {
        v += 0x0010;
    }
    if (c->typeLen)
    {
        v += 0x0001;
    }
    return (unsigned short)v;
}

/* Word char for color-name boundaries (ident chars + parens, so
 * url(gray.png) and gradient(gray,…) never read as colors). */
static int is_color_boundary(char c)
{
    return ((c | 32) >= 'a' && (c | 32) <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '(' ||
           c == ')';
}

/* Named grays (lowercase compare), longest-first so gray never shadows
 * darkgray. Values are the 0-255 gray. */
static const struct
{
    const char *name;
    int lum;
} kGrays[] = {
    {"whitesmoke", 245}, {"lightgrey", 211}, {"lightgray", 211},
    {"darkgrey", 169}, {"darkgray", 169}, {"gainsboro", 220},
    {"silver", 192}, {"dimgrey", 105}, {"dimgray", 105},
    {"grey", 128}, {"gray", 128}, {"white", 255}, {"black", 0},
};

/* First color token in a background shorthand value (for fan-out):
 * #hex run, rgb(...) span, or boundary-checked named color. NULL none. */
static const char *find_color_token(const char *vs, size_t vlen,
                                    size_t *tlenOut)
{
    const char *z = vs + vlen;
    /* #hex */
    for (const char *p = vs; p < z; p++)
    {
        if (*p == '#')
        {
            const char *he = p + 1;
            while (he < z &&
                   ((he[0] >= '0' && he[0] <= '9') ||
                    ((he[0] | 32) >= 'a' && (he[0] | 32) <= 'f')))
            {
                he++;
            }
            size_t n = (size_t)(he - (p + 1));
            if (n == 3 || n == 4 || n == 6 || n == 8)
            {
                if (tlenOut)
                {
                    *tlenOut = (size_t)(he - p);
                }
                return p;
            }
        }
    }
    /* rgb(...) */
    for (const char *p = vs; p + 4 <= z; p++)
    {
        if ((p[0] | 32) == 'r' && (p[1] | 32) == 'g' && (p[2] | 32) == 'b' &&
            p[3] == '(')
        {
            const char *fc = p + 4;
            while (fc < z && *fc != ')')
            {
                fc++;
            }
            if (fc < z)
            {
                if (tlenOut)
                {
                    *tlenOut = (size_t)(fc + 1 - p);
                }
                return p;
            }
        }
    }
    /* named: earliest position wins; longest name at each spot. */
    for (const char *p = vs; p < z; p++)
    {
        for (size_t k = 0; k < sizeof(kGrays) / sizeof(kGrays[0]); k++)
        {
            size_t n = strlen(kGrays[k].name);
            if ((size_t)(z - p) < n)
            {
                continue;
            }
            size_t i;
            for (i = 0; i < n; i++)
            {
                if (css_low(p[i]) != kGrays[k].name[i])
                {
                    break;
                }
            }
            if (i == n)
            {
                int pre = (p == vs) || !is_color_boundary(p[-1]);
                int post = (p + n >= z) || !is_color_boundary(p[n]);
                if (pre && post)
                {
                    if (tlenOut)
                    {
                        *tlenOut = n;
                    }
                    return p;
                }
            }
        }
    }
    return NULL;
}

/* margin/padding shorthand fan-out: split the value into 1-4 tokens and
 * register side longhands (1→all, 2→tb/rl, 3→t/rl/b, 4→t/r/b/l). */
static void add_prop(CssRule *r, const char *name, size_t nlen,
                     const char *val, size_t vlen, int important);
static void expand_box(CssRule *r, const char *name, size_t nlen,
                       const char *vs, size_t vlen, int important)
{
    static const char *kMargin[4] = {"margin-top", "margin-right",
                                     "margin-bottom", "margin-left"};
    static const char *kPadding[4] = {"padding-top", "padding-right",
                                      "padding-bottom", "padding-left"};
    static const size_t kMarginLen[4] = {10, 12, 13, 11};
    static const size_t kPaddingLen[4] = {11, 13, 14, 12};
    int isMargin = (nlen == 6);
    const char **names = isMargin ? kMargin : kPadding;
    const size_t *lens = isMargin ? kMarginLen : kPaddingLen;
    const char *tok[4];
    size_t tlen[4];
    int ntok = 0;
    const char *p = vs;
    const char *z = vs + vlen;
    while (p < z && ntok < 5)
    {
        while (p < z && css_isspace(*p))
        {
            p++;
        }
        if (p >= z)
        {
            break;
        }
        const char *te = p;
        while (te < z && !css_isspace(*te))
        {
            te++;
        }
        if (ntok < 4)
        {
            tok[ntok] = p;
            tlen[ntok] = (size_t)(te - p);
        }
        ntok++;
        p = te;
    }
    if (ntok == 0 || ntok > 4)
    {
        return; /* empty or invalid (>4 components) */
    }
    int map[4]; /* side -> token index */
    if (ntok == 1)
    {
        map[0] = map[1] = map[2] = map[3] = 0;
    }
    else if (ntok == 2)
    {
        map[0] = map[2] = 0;
        map[1] = map[3] = 1;
    }
    else if (ntok == 3)
    {
        map[0] = 0;
        map[1] = map[3] = 1;
        map[2] = 2;
    }
    else
    {
        map[0] = 0;
        map[1] = 1;
        map[2] = 2;
        map[3] = 3;
    }
    for (int side = 0; side < 4; side++)
    {
        add_prop(r, names[side], lens[side], tok[map[side]], tlen[map[side]],
                 important);
    }
}

static void add_prop(CssRule *r, const char *name, size_t nlen,
                     const char *val, size_t vlen, int important){
    if (nlen == 0 || vlen == 0)
    {
        return;
    }
    for (unsigned char i = 0; i < r->propCount; i++)
    {
        if (r->propLen[i] == nlen && css_eq_ncase(r->prop[i], name, nlen))
        {
            /* Same rule, same property: !important beats normal; among
             * equal importance the later declaration wins (CSS order). */
            if (important || !r->propImp[i])
            {
                r->prop[i] = name; /* last declaration wins */
                r->val[i] = val;
                r->valLen[i] = (unsigned char)vlen;
                r->propImp[i] = (unsigned char)(important ? 1 : 0);
            }
            return;
        }
    }
    if (r->propCount >= CSS_MAX_PROPS)
    {
        return;
    }
    r->prop[r->propCount] = name;
    r->val[r->propCount] = val;
    r->propLen[r->propCount] = (unsigned char)nlen;
    r->valLen[r->propCount] = (unsigned char)vlen;
    r->propImp[r->propCount] = (unsigned char)(important ? 1 : 0);
    r->propCount++;
}

/* Trailing "!important" (case-insensitive, optional space before the
 * bang): strip it and report 1. Values are length-delimited, never
 * NUL-terminated, so the tail is compared in place; *ve is moved back
 * past the marker AND any whitespace before it, then re-trimmed. */
static int strip_important(const char *vs, const char **ve)
{
    static const char kImp[] = "!important";
    const char *e = *ve;
    if ((size_t)(e - vs) < sizeof(kImp) - 1)
    {
        return 0;
    }
    const char *tail = e - (sizeof(kImp) - 1);
    for (size_t i = 0; i < sizeof(kImp) - 1; i++)
    {
        if (css_low(tail[i]) != kImp[i])
        {
            return 0;
        }
    }
    e = rtrim(vs, tail);
    *ve = e;
    return 1;
}

static void parse_decl_block(CssRule *r, const char *p, const char *e)
{
    while (p < e)
    {
        /* Declaration = name ':' value, terminated by ';' or block end.
         * A segment with NO colon is junk: skip to the next ';' (or end)
         * and continue — never abort the remaining declarations. */
        const char *colon = NULL;
        const char *semi = NULL;
        for (const char *q = p; q < e; q++)
        {
            if (*q == ';')
            {
                semi = q;
                break; /* segment ends here (colon or not) */
            }
            if (!colon && *q == ':')
            {
                colon = q;
            }
        }
        if (colon && (!semi || colon < semi))
        {
            const char *zend = semi ? semi : e;
            const char *ns = skip_ws(p, colon);
            const char *ne = rtrim(ns, colon);
            const char *vs = skip_ws(colon + 1, zend);
            const char *ve = rtrim(vs, zend);
            int important = strip_important(vs, &ve);
            if (ne > ns && ve > vs)
            {
                size_t nlen = (size_t)(ne - ns);
                if (nlen == 6 && css_eq_ncase(ns, "border", 6))
                {
                    /* Shorthand fans out INSTEAD of registering (the bare
                     * "border" name is never queried — saves a prop slot):
                     * width and style cascade as independent longhands (a
                     * later border-width then overrides by normal
                     * specificity/order/importance). Both read the full
                     * value — the width scan takes the first Npx, the
                     * style scan its keyword. */
                    add_prop(r, "border-width", 12, vs, (size_t)(ve - vs),
                             important);
                    add_prop(r, "border-style", 12, vs, (size_t)(ve - vs),
                             important);
                }
                else if ((nlen == 6 && css_eq_ncase(ns, "margin", 6)) ||
                         (nlen == 7 && css_eq_ncase(ns, "padding", 7)))
                {
                    /* margin/padding fan out to side longhands (never
                     * queried as shorthands). 1→all, 2→tb/rl, 3→t/rl/b,
                     * 4→t/r/b/l; >4 tokens is invalid (dropped). Token
                     * slices point into the value (same lifetime). */
                    expand_box(r, ns, nlen, vs, (size_t)(ve - vs),
                               important);
                }
                else if (nlen == 10 && css_eq_ncase(ns, "background", 10))
                {
                    /* background fans out its color token as
                     * background-color (never queried as shorthand).
                     * Colorless backgrounds (images/gradients) register
                     * nothing — transparent, correctly. */
                    size_t tlen = 0;
                    const char *tok = find_color_token(vs, (size_t)(ve - vs),
                                                       &tlen);
                    if (tok)
                    {
                        add_prop(r, "background-color", 16, tok, tlen,
                                 important);
                    }
                }
                else
                {
                    add_prop(r, ns, nlen, vs, (size_t)(ve - vs), important);
                }
            }            p = semi ? semi + 1 : e;
        }
        else
        {
            /* junk (no colon before the segment end) — skip it */
            p = semi ? semi + 1 : e;
        }
    }
}

void css_parse_sheets(CssEngine *eng, const CssSheet *sheets, int sheetCount)
{
    memset(eng, 0, sizeof(*eng));
    if (!sheets || sheetCount <= 0)
    {
        return;
    }
    static char bufs[CSS_MAX_SHEETS][CSS_SHEET_MAX]; /* static: off the
                                                     * device game-task
                                                     * stack. One buffer
                                                     * PER SHEET: rule
                                                     * pointers aim into
                                                     * these, so sharing a
                                                     * single buffer would
                                                     * corrupt earlier
                                                     * sheets' rules when
                                                     * the next sheet
                                                     * copies over them. */
    unsigned short order = 0;

    for (int si = 0; si < sheetCount && si < CSS_MAX_SHEETS; si++)
    {
        size_t len = sheets[si].len;
        if (!sheets[si].start)
        {
            continue;
        }
        if (len > CSS_SHEET_MAX - 1)
        {
            len = CSS_SHEET_MAX - 1;
            eng->truncated = 1;
        }
        char *buf = bufs[si];
        memcpy(buf, sheets[si].start, len);
        buf[len] = '\0';
        char *e = strip_comments(buf, buf + len);
        /* @media pre-pass: held bodies join the rule stream in order,
         * dropped queries blank out (remaining at-rules skip as before). */
        flatten_media(buf, e, 0);

        char *p = buf;
        while (p < e)
        {
            p = (char *)skip_ws(p, e);
            if (p >= e)
            {
                break;
            }
            if (*p == '@')
            {
                /* Non-media at-rules (@import/@font-face/…): skip whole. */
                const char *brace = css_find_from(p, e, "{");
                const char *semi = css_find_from(p, e, ";");
                if (semi && (!brace || semi < brace))
                {
                    p = (char *)semi + 1;
                }
                else if (brace)
                {
                    const char *bend = at_body_end(brace, e);
                    p = bend ? (char *)bend + 1 : e;
                }
                else
                {
                    p = e;
                }
                continue;
            }

            const char *brace = css_find_from(p, e, "{");
            if (!brace)
            {
                break;
            }
            const char *bend = css_find_from(brace, e, "}");
            if (!bend)
            {
                break;
            }

            CssRule *r = (eng->ruleCount < CSS_MAX_RULES)
                             ? &eng->rules[eng->ruleCount]
                             : NULL;
            if (r)
            {
                memset(r, 0, sizeof(*r));
            }
            else
            {
                eng->truncated = 1;
            }

            /* selector group: keep every usable selector. The comma split
             * is bracket/quote-aware ([a=","] is one selector). */
            unsigned char selIdx = 0;
            /* A rule holds ONE pseudo class: mixed groups (p::before, q)
             * keep the first member's class and drop the rest. */
            int havePseudo = 0;
            unsigned char rulePseudo = 0;
            const char *ss = p;
            int gdepth = 0;
            char gquote = 0;
            for (const char *q = p;; q++)
            {
                int isSep = 0;
                if (q == brace)
                {
                    isSep = 1;
                }
                else if (q < brace)
                {
                    char ch = *q;
                    if (gquote)
                    {
                        if (ch == gquote)
                        {
                            gquote = 0;
                        }
                    }
                    else if (ch == '"' || ch == '\'')
                    {
                        gquote = ch;
                    }
                    else if (ch == '[')
                    {
                        gdepth++;
                    }
                    else if (ch == ']')
                    {
                        if (gdepth > 0)
                        {
                            gdepth--;
                        }
                    }
                    else if (ch == ',' && gdepth == 0)
                    {
                        isSep = 1;
                    }
                }
                if (isSep)
                {
                    const char *s = skip_ws(ss, q);
                    const char *z = rtrim(s, q);
                    if (selIdx < CSS_MAX_SELECTORS &&
                        (size_t)(z - s) <= CSS_SELECTOR_MAX)
                    {
                        CssSimple parts[CSS_COMPOUND_MAX];
                        unsigned char combs[CSS_COMPOUND_MAX];
                        int np = 0;
                        int bad = 0;
                        unsigned char pseudo = 0;
                        z = strip_pseudo(s, z, &pseudo);
                        if (pseudo && s >= z)
                        {
                            /* Bare ::before/::after = universal subject. */
                            memset(&parts[0], 0, sizeof(parts[0]));
                            parts[0].universal = 1;
                            combs[0] = 0;
                            np = 1;
                        }
                        else
                        {
                            /* Combinator before the next compound: ... */
                            int comb = 0;
                            int needComb = 0;
                            const char *cs = s;
                            while (cs < z && !bad)
                            {
                                if (css_isspace(*cs))
                                {
                                    cs++;
                                    continue;
                                }
                                if (*cs == '>')
                                {
                                    if (np == 0 || needComb)
                                    {
                                        bad = 1;
                                        break;
                                    }
                                    comb = 1;
                                    needComb = 1;
                                    cs++;
                                    continue;
                                }
                                if (*cs == '+' || *cs == '~')
                                {
                                    bad = 1;
                                    break;
                                }
                                const char *ce = compound_end(cs, z);
                                if (ce > cs)
                                {
                                    if (np >= CSS_COMPOUND_MAX ||
                                        !parse_compound(cs, ce, &parts[np]))
                                    {
                                        bad = 1;
                                        break;
                                    }
                                    combs[np] = (unsigned char)comb;
                                    np++;
                                    comb = 0;
                                    needComb = 0;
                                }
                                cs = ce;
                            }
                            if (needComb)
                            {
                                bad = 1; /* trailing '>' */
                            }
                        }
                        if (!bad && np > 0 && r &&
                            (!havePseudo || pseudo == rulePseudo))
                        {
                            unsigned spec = 0;
                            for (int i = 0; i < np; i++)
                            {
                                r->sel[selIdx][i] = parts[i];
                                r->selComb[selIdx][i] = combs[i];
                                spec += compound_spec(&parts[i]);
                            }
                            if (pseudo)
                            {
                                /* Pseudo-elements count as a type
                                 * selector for specificity. */
                                spec += 1;
                            }
                            if (!havePseudo)
                            {
                                havePseudo = 1;
                                rulePseudo = pseudo;
                                r->pseudo = pseudo;
                            }
                            r->selParts[selIdx] = (unsigned char)np;
                            r->spec[selIdx] = (unsigned short)(spec > 0xFFFF
                                                                   ? 0xFFFF
                                                                   : spec);
                            selIdx++;
                        }
                    }
                    ss = q + 1;
                    gdepth = 0;
                    gquote = 0;
                    if (q == brace)
                    {
                        break;
                    }
                }
                if (q >= brace)
                {
                    break;
                }
            }

            if (r)
            {
                r->selCount = selIdx;
                r->order = order++;
                if (selIdx > 0)
                {
                    parse_decl_block(r, brace + 1, bend);
                    eng->ruleCount++;
                }
                else
                {
                    r->skipped = 1;
                }
            }
            p = (char *)bend + 1;
        }
    }
}

/* ── 3. matching ──────────────────────────────────────────────────────────── */
int css_compound_matches(const CssSimple *c, const char *tagLower,
                          const char *classAttr, const char *idAttr,
                          const CssDom *dom, const void *node)
{
    if (!c)
    {
        return 0;
    }
    if (c->typeLen)
    {
        size_t n = tagLower ? strlen(tagLower) : 0;
        if (n != c->typeLen || !css_eq_ncase(tagLower, c->type, c->typeLen))
        {
            return 0;
        }
    }
    if (c->clsLen)
    {
        if (!classAttr)
        {
            return 0;
        }
        /* class="a b c": whole-token match, case-sensitive per CSS */
        const char *p = classAttr;
        int found = 0;
        while (*p)
        {
            const char *t = p;
            while (*t && !css_isspace(*t))
            {
                t++;
            }
            if ((size_t)(t - p) == c->clsLen && memcmp(p, c->cls, c->clsLen) == 0)
            {
                found = 1;
                break;
            }
            if (!*t)
            {
                break;
            }
            p = t + 1;
        }
        if (!found)
        {
            return 0;
        }
    }
    if (c->idLen)
    {
        if (!idAttr || strlen(idAttr) != c->idLen ||
            memcmp(idAttr, c->id, c->idLen) != 0)
        {
            return 0;
        }
    }
    if (c->attrLen)
    {
        /* Attribute test needs a DOM hook; without one it never matches
         * (graceful — never misapplied). Name compare is case-insensitive
         * (HTML), value compare case-sensitive (CSS). */
        if (!dom || !dom->attr || !node)
        {
            return 0;
        }
        const char *v = dom->attr(node, c->attr, c->attrLen);
        if (!v)
        {
            return 0;
        }
        if (c->attrVal)
        {
            size_t n = strlen(v);
            if (n != c->attrValLen ||
                memcmp(v, c->attrVal, c->attrValLen) != 0)
            {
                return 0;
            }
        }
    }
    if (c->pclass)
    {
        /* Structural pseudo-class needs the sibling hook (graceful
         * without it). Positions are 1-based among ELEMENT siblings. */
        if (!dom || !dom->sibling || !node)
        {
            return 0;
        }
        if (c->pclass == 4)
        {
            /* :link = has an href (no visited concept here). */
            if (!dom->attr || !dom->attr(node, "href", 4))
            {
                return 0;
            }
        }
        else
        {
            int idx = 1;
            const void *s = node;
            while ((s = dom->sibling(s, -1)) != NULL)
            {
                idx++;
            }
            if (c->pclass == 1)
            {
                if (idx != 1)
                {
                    return 0;
                }
            }
            else if (c->pclass == 2)
            {
                if (dom->sibling(node, 1) != NULL)
                {
                    return 0;
                }
            }
            else if (c->pclass == 3)
            {
                if (idx != 1 || dom->sibling(node, 1) != NULL)
                {
                    return 0;
                }
            }
            else if (c->pclass == 5)
            {
                int na = c->nthA;
                int nb = c->nthB;
                if (na == 0)
                {
                    if (idx != nb)
                    {
                        return 0;
                    }
                }
                else
                {
                    int dd = idx - nb;
                    if (dd < 0 || dd % na != 0)
                    {
                        return 0;
                    }
                }
            }
        }
    }
    if (!c->typeLen && !c->clsLen && !c->idLen && !c->universal &&
        !c->attrLen && !c->pclass)
    {
        return 0;
    }
    return 1;
}

int css_chain_matches(const CssSimple *parts, const unsigned char *combs,
                      int np, const char *tagLower, const char *classAttr,
                      const char *idAttr, const CssDom *dom, const void *node)
{
    static const unsigned char kDesc[CSS_COMPOUND_MAX] = {0};
    if (!parts || np <= 0 || np > CSS_COMPOUND_MAX)
    {
        return 0;
    }
    if (!combs)
    {
        combs = kDesc; /* no combinators = all descendant */
    }
    if (np == 1)
    {
        return css_compound_matches(&parts[0], tagLower, classAttr, idAttr,
                                    dom, node);
    }
        /* descendant/child: subject is the last compound; walk ancestors
         * nearest-first, consuming compounds right-to-left. Absolute
         * ancestor indexing keeps mixed chains honest (div > p span:
         * the span's p may be at any depth, but div must be p's PARENT).
         * comb[i] is the combinator BEFORE compound i: 1 = child means
         * only the immediate next ancestor may satisfy compound i. */
        if (!dom || !dom->ancestor || !node)
        {
            return 0;
        }
        if (!css_compound_matches(&parts[np - 1], tagLower, classAttr,
                                  idAttr, dom, node))
        {
            return 0;
        }
        int want = (int)np - 2; /* next compound to satisfy */
        int ai = 0;             /* absolute ancestor cursor (0 = parent) */
        while (want >= 0)
        {
            const char *atag = NULL;
            const char *acls = NULL;
            const char *aid = NULL;
            const void *a = dom->ancestor(node, ai, &atag, &acls, &aid);
            if (!a)
            {
                return 0; /* ran out of ancestors: selector fails */
            }
            if (combs[want + 1] == 1)
            {
                /* Child: this ancestor (and only this one) must match. */
                if (!css_compound_matches(&parts[want], atag, acls, aid,
                                          dom, a))
                {
                    return 0;
                }
                want--;
            }
            else if (css_compound_matches(&parts[want], atag, acls, aid,
                                          dom, a))
            {
                want--;
            }
            ai++;
        }
        return 1;
}

int css_rule_matches(const CssRule *rule, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node)
{
    /* ::before/::after rules never match elements — only the content
     * cascade reads them. */
    if (!rule || rule->skipped || rule->selCount == 0 || rule->pseudo)
    {
        return 0;
    }
    for (unsigned char si = 0; si < rule->selCount; si++)
    {
        if (css_chain_matches(rule->sel[si], rule->selComb[si],
                              rule->selParts[si], tagLower, classAttr,
                              idAttr, dom, node))
        {
            return 1;
        }
    }
    return 0;
}

/* ── SW5: querySelector selector parsing (O3) ─────────────────────────────── */
int css_parse_selector(const char *sel, char *scratch, size_t scratchSize,
                       CssSimple parts[CSS_QS_MAX_COMPOUNDS],
                       unsigned char combOut[CSS_QS_MAX_COMPOUNDS])
{
    if (!sel || !scratch || !parts || scratchSize == 0)
    {
        return 0;
    }
    size_t slen = strlen(sel);
    if (slen == 0 || slen >= scratchSize || slen > CSS_SELECTOR_MAX)
    {
        return 0;
    }
    memcpy(scratch, sel, slen + 1);
    char *z = scratch + slen;
    int np = 0;
    int comb = 0;
    int needComb = 0;
    const char *cs = scratch;
    while (cs < z)
    {
        if (css_isspace(*cs))
        {
            cs++;
            continue;
        }
        if (*cs == '>')
        {
            if (np == 0 || needComb)
            {
                return 0;
            }
            comb = 1;
            needComb = 1;
            cs++;
            continue;
        }
        if (*cs == '+' || *cs == '~')
        {
            return 0; /* sibling combinators unsupported */
        }
        const char *ce = compound_end(cs, z);
        if (ce > cs)
        {
            if (np >= CSS_QS_MAX_COMPOUNDS ||
                !parse_compound(cs, ce, &parts[np]))
            {
                return 0; /* unusable selector (attr ops/pseudo/depth) */
            }
            if (combOut)
            {
                combOut[np] = (unsigned char)comb;
            }
            np++;
            comb = 0;
            needComb = 0;
        }
        cs = ce;
    }
    if (needComb || np == 0)
    {
        return 0;
    }
    return np;
}

int css_compound_matches_attrs(const CssSimple *c, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node)
{
    return css_compound_matches(c, tagLower, classAttr, idAttr, dom, node);
}

/* Length-delimited substring (values are never NUL-terminated). */
int css_val_has(const char *val, size_t vlen, const char *needle){
    size_t n = strlen(needle);
    if (!val || n == 0 || vlen < n)
    {
        return 0;
    }
    for (size_t i = 0; i + n <= vlen; i++)
    {
        if (val[i] == needle[0] && memcmp(val + i, needle, n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* Length-delimited exact compare, case-insensitive. */
int css_val_eq(const char *val, size_t vlen, const char *lit)
{
    size_t n = strlen(lit);
    if (!val || vlen != n)
    {
        return 0;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (css_low(val[i]) != lit[i])
        {
            return 0;
        }
    }
    return 1;
}

/* First Npx in a length-delimited value (fraction dropped, negative → 0),
 * or -1 when absent. */
int css_px_value(const char *val, size_t vlen){
    if (!val)
    {
        return -1;
    }
    for (size_t i = 0; i < vlen; i++)
    {
        int neg = 0;
        size_t j = i;
        if ((val[j] == '-' || val[j] == '+') && j + 1 < vlen &&
            val[j + 1] >= '0' && val[j + 1] <= '9')
        {
            neg = (val[j] == '-');
            j++;
        }
        if (j >= vlen || val[j] < '0' || val[j] > '9')
        {
            continue;
        }
        int n = 0;
        while (j < vlen && val[j] >= '0' && val[j] <= '9')
        {
            n = n * 10 + (val[j] - '0');
            j++;
        }
        if (j < vlen && val[j] == '.')
        {
            j++;
            while (j < vlen && val[j] >= '0' && val[j] <= '9')
            {
                j++;
            }
        }
        if (j + 1 < vlen && (val[j] | 32) == 'p' && (val[j + 1] | 32) == 'x')
        {
            /* px must end the token (2em must not read as 2 + "em"... it
             * can't: 'e' != 'p'. "2pxx"? trailing ident char voids. */
            if (j + 2 < vlen)
            {
                char c = val[j + 2];
                if ((c | 32) >= 'a' && (c | 32) <= 'z')
                {
                    i = j;
                    continue;
                }
            }
            return neg ? 0 : n;
        }
        i = (j > i + 1) ? j - 1 : i;
    }
    return -1;
}

/* Hex run → luminance. digits = 3/6 (4/8 with alpha: alpha ignored). */
static int hex_lum(const char *h, size_t n)
{
    unsigned v[8];
    if (n != 3 && n != 4 && n != 6 && n != 8)
    {
        return -1;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = h[i];
        if (c >= '0' && c <= '9')
        {
            v[i] = (unsigned)(c - '0');
        }
        else if ((c | 32) >= 'a' && (c | 32) <= 'f')
        {
            v[i] = (unsigned)((c | 32) - 'a' + 10);
        }
        else
        {
            return -1;
        }
    }
    unsigned r, g, b;
    if (n <= 4)
    {
        r = v[0] * 17;
        g = v[1] * 17;
        b = v[2] * 17;
    }
    else
    {
        r = v[0] * 16 + v[1];
        g = v[2] * 16 + v[3];
        b = v[4] * 16 + v[5];
    }
    return (int)(299 * r + 587 * g + 114 * b) / 1000;
}

static int rgb_lum(const char *s, const char *z)
{
    int c[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++)
    {
        while (s < z && css_isspace(*s))
        {
            s++;
        }
        if (s >= z || *s < '0' || *s > '9')
        {
            return -1;
        }
        int n = 0;
        while (s < z && *s >= '0' && *s <= '9')
        {
            n = n * 10 + (*s - '0');
            s++;
        }
        while (s < z && css_isspace(*s))
        {
            s++;
        }
        if (k < 2)
        {
            if (s >= z || *s != ',')
            {
                return -1;
            }
            s++;
        }
        if (n < 0)
        {
            n = 0;
        }
        if (n > 255)
        {
            n = 255;
        }
        c[k] = n;
    }
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    if (s != z)
    {
        return -1; /* trailing junk (alpha etc.): unparsable */
    }
    return (299 * c[0] + 587 * c[1] + 114 * c[2]) / 1000;
}

int css_color_luminance(const char *val, size_t vlen)
{
    if (!val || vlen == 0)
    {
        return -1;
    }
    const char *z = val + vlen;
    /* #hex first (exact token scan). */
    for (const char *p = val; p < z; p++)
    {
        if (*p == '#')
        {
            const char *h = p + 1;
            const char *he = h;
            while (he < z &&
                   ((he[0] >= '0' && he[0] <= '9') ||
                    ((he[0] | 32) >= 'a' && (he[0] | 32) <= 'f')))
            {
                he++;
            }
            int lum = hex_lum(h, (size_t)(he - h));
            if (lum >= 0)
            {
                return lum;
            }
            /* "#zz" garbage: keep scanning (a later token may hit). */
        }
    }
    /* rgb(...) */
    for (const char *p = val; p + 4 <= z; p++)
    {
        if ((p[0] | 32) == 'r' && (p[1] | 32) == 'g' && (p[2] | 32) == 'b' &&
            p[3] == '(')
        {
            const char *fc = p + 4;
            while (fc < z && *fc != ')')
            {
                fc++;
            }
            if (fc < z)
            {
                int lum = rgb_lum(p + 4, fc);
                if (lum >= 0)
                {
                    return lum;
                }
            }
        }
    }
    /* named (longest-first table order decides shadowing). Word
     * boundaries keep url(gray.png) and gradient(gray,…) from reading
     * as colors (parens count as word chars — a name touching one is
     * a function argument, not a color). */
    for (size_t k = 0; k < sizeof(kGrays) / sizeof(kGrays[0]); k++)
    {
        size_t n = strlen(kGrays[k].name);
        for (const char *p = val; p + n <= z; p++)
        {
            size_t i;
            for (i = 0; i < n; i++)
            {
                if (css_low(p[i]) != kGrays[k].name[i])
                {
                    break;
                }
            }
            if (i == n)
            {
                int pre = (p == val) || !is_color_boundary(p[-1]);
                int post =
                    (p + n >= z) || !is_color_boundary(p[n]);
                if (pre && post)
                {
                    return kGrays[k].lum;
                }
            }
        }
    }
    return -1;
}

const char *css_rule_prop(const CssRule *r, const char *prop)
{
    if (!r)
    {
        return NULL;
    }
    size_t n = strlen(prop);
    for (unsigned char i = 0; i < r->propCount; i++)
    {
        if (r->propLen[i] == n && memcmp(r->prop[i], prop, n) == 0)
        {
            return r->val[i];
        }
    }
    return NULL;
}

/* Shared cascade core: winning declaration for one property among rules
 * of one origin class (wantPseudo: 0 = element rules, 1/2 = ::before/
 * ::after rules). Importance → specificity → later order; the UA [hidden]
 * seed participates only in the element-display lane. */
static const char *winning_core(const CssEngine *eng, const char *tagLower,
                                const char *classAttr, const char *idAttr,
                                const CssDom *dom, const void *node,
                                const char *prop, int wantPseudo,
                                size_t *vlenOut)
{
    if (vlenOut)
    {
        *vlenOut = 0;
    }
    if (!eng || !prop)
    {
        return NULL;
    }
    size_t plen = strlen(prop);
    const char *best = NULL;
    size_t blen = 0;
    unsigned bestSpec = 0;
    unsigned short bestOrder = 0;
    unsigned char bestImp = 0;
    int bestUa = 0;
    int have = 0;

    /* UA seed (mirrors css_compute): [hidden] display:none, beaten by any
     * author declaration unconditionally. Element lane only. */
    if (wantPseudo == 0 && plen == 7 && memcmp(prop, "display", 7) == 0 &&
        dom && dom->attr && node && dom->attr(node, "hidden", 6))
    {
        best = "none";
        blen = 4;
        bestUa = 1;
        have = 1;
    }

    for (int ri = 0; ri < eng->ruleCount; ri++)
    {
        const CssRule *r = &eng->rules[ri];
        if (r->pseudo != wantPseudo)
        {
            continue;
        }
        /* Matching selector's specificity (max over MATCHING selectors —
         * a non-matching group member must not inflate the win). */
        unsigned spec = 0;
        int hit = 0;
        for (unsigned char si = 0; si < r->selCount; si++)
        {
            if (css_chain_matches(r->sel[si], r->selComb[si],
                                  r->selParts[si], tagLower, classAttr,
                                  idAttr, dom, node))
            {
                hit = 1;
                if (r->spec[si] > spec)
                {
                    spec = r->spec[si];
                }
            }
        }
        if (!hit)
        {
            continue;
        }
        for (unsigned char pj = 0; pj < r->propCount; pj++)
        {
            if (r->propLen[pj] != plen ||
                memcmp(r->prop[pj], prop, plen) != 0)
            {
                continue;
            }
            unsigned char imp = r->propImp[pj];
            if (have && !bestUa &&
                (imp < bestImp ||
                 (imp == bestImp &&
                  (spec < bestSpec ||
                   (spec == bestSpec && r->order <= bestOrder)))))
            {
                continue;
            }
            best = r->val[pj];
            blen = r->valLen[pj];
            bestSpec = spec;
            bestOrder = r->order;
            bestImp = imp;
            bestUa = 0;
            have = 1;
        }
    }
    if (have && vlenOut)
    {
        *vlenOut = blen;
    }
    return best;
}

const char *css_winning_value(const CssEngine *eng, const char *tagLower,
                              const char *classAttr, const char *idAttr,
                              const CssDom *dom, const void *node,
                              const char *prop, size_t *vlenOut)
{
    return winning_core(eng, tagLower, classAttr, idAttr, dom, node, prop,
                        0, vlenOut);
}

const char *css_pseudo_content(const CssEngine *eng, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node,
                               int before, size_t *vlenOut)
{
    return winning_core(eng, tagLower, classAttr, idAttr, dom, node,
                        "content", before ? 1 : 2, vlenOut);
}

/* ── 4. property application + cascade ───────────────────────────────────── */
static void apply_prop(unsigned *out, const char *prop, size_t plen,
                       const char *val, size_t vlen)
{
    char buf[48];
    size_t n = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
    memcpy(buf, val, n);
    buf[n] = '\0';

    if (plen == 7 && memcmp(prop, "display", 7) == 0)
    {
        if (strstr(buf, "none"))
        {
            *out |= CSS_F_HIDDEN;
        }
    }
    else if (plen == 10 && memcmp(prop, "visibility", 10) == 0)
    {
        if (strstr(buf, "hidden"))
        {
            *out |= CSS_F_HIDDEN;
        }
    }
    else if (plen == 10 && memcmp(prop, "text-align", 10) == 0)
    {
        if (strstr(buf, "center"))
        {
            *out |= CSS_F_ALIGN_CENTER;
        }
        else if (strstr(buf, "right"))
        {
            *out |= CSS_F_ALIGN_RIGHT;
        }
    }
    else if (plen == 11 && memcmp(prop, "font-weight", 11) == 0)
    {
        if (strstr(buf, "bold"))
        {
            *out |= CSS_F_BOLD;
        }
        else
        {
            int num = 0;
            const char *q = buf;
            while (*q >= '0' && *q <= '9')
            {
                num = num * 10 + (*q - '0');
                q++;
            }
            if (num >= 600)
            {
                *out |= CSS_F_BOLD;
            }
        }
    }
    else if (plen == 10 && memcmp(prop, "font-style", 10) == 0)
    {
        if (strstr(buf, "italic") || strstr(buf, "oblique"))
        {
            *out |= CSS_F_ITALIC;
        }
    }
    else if (plen == 15 && memcmp(prop, "text-decoration", 15) == 0)
    {
        if (strstr(buf, "underline"))
        {
            *out |= CSS_F_UNDERLINE;
        }
        if (strstr(buf, "line-through"))
        {
            *out |= CSS_F_STRIKE;
        }
    }
    else if (plen == 14 && memcmp(prop, "text-transform", 14) == 0)
    {
        if (strstr(buf, "uppercase"))
        {
            *out |= CSS_F_UPPER;
        }
        else if (strstr(buf, "lowercase"))
        {
            *out |= CSS_F_LOWER;
        }
        else if (strstr(buf, "capitalize"))
        {
            *out |= CSS_F_CAP;
        }
    }
    else if (plen == 11 && memcmp(prop, "white-space", 11) == 0)
    {
        if (strstr(buf, "nowrap"))
        {
            *out |= CSS_F_NOWRAP;
        }
        else if (strstr(buf, "pre-line"))
        {
            *out |= CSS_F_PRELINE;
        }
        else if (strstr(buf, "pre"))
        {
            /* pre-wrap and pre: preserve spaces (wrapping stays on). */
            *out |= CSS_F_PRE;
        }
    }
    else if (plen == 13 && memcmp(prop, "overflow-wrap", 13) == 0)
    {
        if (strstr(buf, "break-word") || strstr(buf, "anywhere"))
        {
            *out |= CSS_F_BREAKWORD;
        }
    }
    else if (plen == 10 && memcmp(prop, "word-break", 10) == 0)
    {
        if (strstr(buf, "break-all"))
        {
            *out |= CSS_F_BREAKWORD;
        }
    }
    else if (plen == 9 && memcmp(prop, "font-size", 9) == 0)
    {
        /* Keywords first ("smaller" contains "small" — same bucket). */
        if (strstr(buf, "small"))
        {
            *out |= CSS_F_FSMALL;
        }
        else if (strstr(buf, "large"))
        {
            *out |= CSS_F_FBIG;
        }
        else
        {
            /* Numeric: px / % / em / rem against a ~16px body. */
            const char *q = buf;
            while (*q == ' ' || *q == '\t')
            {
                q++;
            }
            if (*q == '-')
            {
                /* negative: invalid, ignore */
            }
            else
            {
                if (*q == '+')
                {
                    q++;
                }
                long ip = 0;
                int hd = 0;
                while (*q >= '0' && *q <= '9')
                {
                    hd = 1;
                    ip = ip * 10 + (*q - '0');
                    q++;
                }
                long fp = 0, fdiv = 1;
                if (*q == '.')
                {
                    q++;
                    while (*q >= '0' && *q <= '9' && fdiv < 100)
                    {
                        fp = fp * 10 + (*q - '0');
                        fdiv *= 10;
                        q++;
                    }
                    while (*q >= '0' && *q <= '9')
                    {
                        q++;
                    }
                }
                if (hd)
                {
                    while (*q == ' ' || *q == '\t')
                    {
                        q++;
                    }
                    if ((q[0] | 32) == 'p' && (q[1] | 32) == 'x')
                    {
                        if (ip < 13)
                        {
                            *out |= CSS_F_FSMALL;
                        }
                        else if (ip > 20)
                        {
                            *out |= CSS_F_FBIG;
                        }
                    }
                    else if (*q == '%')
                    {
                        if (ip < 100)
                        {
                            *out |= CSS_F_FSMALL;
                        }
                        else if (ip > 120)
                        {
                            *out |= CSS_F_FBIG;
                        }
                    }
                    else if (((q[0] | 32) == 'e' && q[1] &&
                              (q[1] | 32) == 'm') ||
                             (q[0] && q[1] && q[2] &&
                              (q[0] | 32) == 'r' && (q[1] | 32) == 'e' &&
                              (q[2] | 32) == 'm'))
                    {
                        long v100 = ip * 100 + fp * 100 / fdiv;
                        if (v100 < 100)
                        {
                            *out |= CSS_F_FSMALL;
                        }
                        else if (v100 > 125)
                        {
                            *out |= CSS_F_FBIG;
                        }
                    }
                }
            }
        }
    }
    else if (plen == 5 && memcmp(prop, "color", 5) == 0)
    {
        if (strstr(buf, "white") || strstr(buf, "#fff"))
        {
            *out |= CSS_F_INVERT;
        }
    }
    /* NOTE: background/background-color intentionally unmapped here —
     * gray bands need VALUES (dither fills), so the walker resolves
     * them through css_winning_value (luminance → invert/shade/none). */
}

unsigned css_compute(const CssEngine *eng, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node)
{
    unsigned out = 0;
    if (!eng)
    {
        return out;
    }
    /* Per property, find the winning declaration among matching rules. */
    const char *bestProp[CSS_MAX_PROPS];
    const char *bestVal[CSS_MAX_PROPS];
    unsigned char bestLen[CSS_MAX_PROPS];  /* property NAME lengths */
    unsigned char bestVlen[CSS_MAX_PROPS]; /* winning VALUE lengths */
    unsigned bestSpec[CSS_MAX_PROPS];
    unsigned short bestOrder[CSS_MAX_PROPS];
    unsigned char bestImp[CSS_MAX_PROPS];
    unsigned char bestUa[CSS_MAX_PROPS]; /* 1 = seeded by the UA origin */
    unsigned char bestCount = 0;

    /* UA origin: [hidden] elements are display:none unless ANY author
     * display declaration wins — author origin beats UA at any
     * specificity (CSS cascade), so the seed loses to every author
     * challenger unconditionally. */
    if (dom && dom->attr && node && dom->attr(node, "hidden", 6))
    {
        bestProp[0] = "display";
        bestVal[0] = "none";
        bestLen[0] = 7;
        bestVlen[0] = 4;
        bestSpec[0] = 0;
        bestOrder[0] = 0;
        bestImp[0] = 0;
        bestUa[0] = 1;
        bestCount = 1;
    }

    for (int ri = 0; ri < eng->ruleCount; ri++)
    {
        const CssRule *r = &eng->rules[ri];
        if (!css_rule_matches(r, tagLower, classAttr, idAttr, dom, node))
        {
            continue;
        }
        unsigned spec = 0;
        for (unsigned char si = 0; si < r->selCount; si++)
        {
            if (r->spec[si] > spec)
            {
                spec = r->spec[si];
            }
        }
        for (unsigned char pj = 0; pj < r->propCount; pj++)
        {
            /* find this property in the best-so-far table */
            unsigned char bi;
            for (bi = 0; bi < bestCount; bi++)
            {
                if (bestLen[bi] == r->propLen[pj] &&
                    memcmp(bestProp[bi], r->prop[pj], r->propLen[pj]) == 0)
                {
                    break;
                }
            }
            if (bi == bestCount)
            {
                if (bestCount >= CSS_MAX_PROPS)
                {
                    continue; /* engine-level property cap for this element */
                }
                bi = bestCount++;
                bestProp[bi] = r->prop[pj];
                bestVal[bi] = NULL;
                bestLen[bi] = r->propLen[pj];
                bestVlen[bi] = 0;
                bestSpec[bi] = 0;
                bestOrder[bi] = 0;
                bestImp[bi] = 0;
                bestUa[bi] = 0;
            }
            /* !important beats any normal declaration regardless of
             * specificity/order; among equal importance the usual
             * specificity-then-later-order decides. A UA-seeded value
             * loses to EVERY author challenger (origin beats
             * specificity) — there are no UA rules in the table, so any
             * challenger here is author-origin. */
            unsigned char imp = r->propImp[pj];
            if (bestVal[bi] && !bestUa[bi] &&
                (imp < bestImp[bi] ||
                 (imp == bestImp[bi] &&
                  (spec < bestSpec[bi] ||
                   (spec == bestSpec[bi] && r->order <= bestOrder[bi])))))
            {
                continue; /* an earlier rule wins this property */
            }
            bestVal[bi] = r->val[pj];
            bestVlen[bi] = r->valLen[pj];
            bestSpec[bi] = spec;
            bestOrder[bi] = r->order;
            bestImp[bi] = imp;
            bestUa[bi] = 0;
        }
    }
    for (unsigned char bi = 0; bi < bestCount; bi++)
    {
        if (bestVal[bi])
        {
            /* Exact lengths, NOT strlen: names and values point into the
             * sheet buffer with no terminators — strlen once false-
             * positived display:block on a sheet that mentioned "none"
             * 30 bytes later, and name lengths truncate values. */
            apply_prop(&out, bestProp[bi], bestLen[bi], bestVal[bi],
                       bestVlen[bi]);
        }
    }
    return out;
}
