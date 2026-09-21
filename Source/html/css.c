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

/* Parse one compound chunk [s,z) into out; 1 ok, 0 unusable.
 * Handles type / .class / #id / * and combinations ("div.note#x"). */
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
        if (ch == '.' || ch == '#')
        {
            const char *t = p + 1;
            while (t < z && !css_isspace(*t) && *t != '.' && *t != '#')
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
        else
        {
            return 0; /* [attr], :pseudo, digits, escapes … unusable */
        }
    }
    return 1;
}

/* Specificity packing: ids<<8 | classes<<4 | types. */
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
    if (c->typeLen)
    {
        v += 0x0001;
    }
    return (unsigned short)v;
}

static void add_prop(CssRule *r, const char *name, size_t nlen,
                     const char *val, size_t vlen)
{
    if (nlen == 0 || vlen == 0)
    {
        return;
    }
    for (unsigned char i = 0; i < r->propCount; i++)
    {
        if (r->propLen[i] == nlen && css_eq_ncase(r->prop[i], name, nlen))
        {
            r->prop[i] = name; /* last declaration wins */
            r->val[i] = val;
            r->valLen[i] = (unsigned char)vlen;
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
    r->propCount++;
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
            if (ne > ns && ve > vs)
            {
                add_prop(r, ns, (size_t)(ne - ns), vs, (size_t)(ve - vs));
            }
            p = semi ? semi + 1 : e;
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
    static char buf[CSS_SHEET_MAX]; /* static: off the device game-task stack */
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
        memcpy(buf, sheets[si].start, len);
        buf[len] = '\0';
        char *e = strip_comments(buf, buf + len);

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
                /* @media/@import/@font-face/… : skip the whole at-rule. */
                const char *brace = css_find_from(p, e, "{");
                const char *semi = css_find_from(p, e, ";");
                if (semi && (!brace || semi < brace))
                {
                    p = (char *)semi + 1;
                }
                else if (brace)
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
                                break;
                            }
                        }
                        q++;
                    }
                    p = (q < e) ? (char *)q + 1 : e;
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

            /* selector group: keep every usable selector */
            unsigned char selIdx = 0;
            const char *ss = p;
            for (const char *q = p;; q++)
            {
                if (q == brace || (*q == ','))
                {
                    const char *s = skip_ws(ss, q);
                    const char *z = rtrim(s, q);
                    if (selIdx < CSS_MAX_SELECTORS &&
                        (size_t)(z - s) <= CSS_SELECTOR_MAX)
                    {
                        CssSimple parts[CSS_COMPOUND_MAX];
                        int np = 0;
                        int bad = 0;
                        const char *cs = s;
                        while (cs < z && !bad)
                        {
                            const char *ce = cs;
                            while (ce < z && !css_isspace(*ce))
                            {
                                ce++;
                            }
                            if (ce > cs)
                            {
                                if (np >= CSS_COMPOUND_MAX ||
                                    !parse_compound(cs, ce, &parts[np]))
                                {
                                    bad = 1;
                                    break;
                                }
                                np++;
                            }
                            cs = ce;
                            while (cs < z && css_isspace(*cs))
                            {
                                cs++;
                            }
                        }
                        if (!bad && np > 0 && r)
                        {
                            unsigned spec = 0;
                            for (int i = 0; i < np; i++)
                            {
                                r->sel[selIdx][i] = parts[i];
                                spec += compound_spec(&parts[i]);
                            }
                            r->selParts[selIdx] = (unsigned char)np;
                            r->spec[selIdx] = (unsigned short)(spec > 0xFFFF
                                                                   ? 0xFFFF
                                                                   : spec);
                            selIdx++;
                        }
                    }
                    ss = q + 1;
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
                         const char *classAttr, const char *idAttr)
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
    if (!c->typeLen && !c->clsLen && !c->idLen && !c->universal)
    {
        return 0;
    }
    return 1;
}

int css_rule_matches(const CssRule *rule, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node)
{
    if (!rule || rule->skipped || rule->selCount == 0)
    {
        return 0;
    }
    for (unsigned char si = 0; si < rule->selCount; si++)
    {
        unsigned char np = rule->selParts[si];
        if (np == 0)
        {
            continue;
        }
        if (np == 1)
        {
            if (css_compound_matches(&rule->sel[si][0], tagLower, classAttr,
                                     idAttr))
            {
                return 1;
            }
            continue;
        }
        /* descendant: subject is the last compound; walk ancestors
         * nearest-first, consuming compounds right-to-left. */
        if (!dom || !dom->ancestor || !node)
        {
            continue;
        }
        if (!css_compound_matches(&rule->sel[si][np - 1], tagLower, classAttr,
                                  idAttr))
        {
            continue;
        }
        int want = (int)np - 2; /* next compound to satisfy */
        for (int i = 0; want >= 0; i++)
        {
            const char *atag = NULL;
            const char *acls = NULL;
            const char *aid = NULL;
            const void *a = dom->ancestor(node, i, &atag, &acls, &aid);
            if (!a)
            {
                want = -2; /* ran out of ancestors: selector fails */
                break;
            }
            if (css_compound_matches(&rule->sel[si][want], atag, acls, aid))
            {
                want--;
            }
        }
        if (want == -1)
        {
            return 1;
        }
    }
    return 0;
}

/* ── SW5: querySelector selector parsing (O3) ─────────────────────────────── */
int css_parse_selector(const char *sel, char *scratch, size_t scratchSize,
                       CssSimple parts[CSS_QS_MAX_COMPOUNDS])
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
    const char *cs = scratch;
    while (cs < z)
    {
        const char *ce = cs;
        while (ce < z && !css_isspace(*ce))
        {
            ce++;
        }
        if (ce > cs)
        {
            if (np >= CSS_QS_MAX_COMPOUNDS ||
                !parse_compound(cs, ce, &parts[np]))
            {
                return 0; /* unusable selector (attr/pseudo/child/depth) */
            }
            np++;
        }
        cs = ce;
        while (cs < z && css_isspace(*cs))
        {
            cs++;
        }
    }
    return np;
}

int css_compound_matches_attrs(const CssSimple *c, const char *tagLower,
                               const char *classAttr, const char *idAttr)
{
    return css_compound_matches(c, tagLower, classAttr, idAttr);
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
    else if (plen == 5 && memcmp(prop, "color", 5) == 0)
    {
        if (strstr(buf, "white") || strstr(buf, "#fff"))
        {
            *out |= CSS_F_INVERT;
        }
    }
    else if ((plen == 10 && memcmp(prop, "background", 10) == 0) ||
             (plen == 16 && memcmp(prop, "background-color", 16) == 0))
    {
        if (strstr(buf, "black") || strstr(buf, "#000"))
        {
            *out |= CSS_F_INVERT;
        }
    }
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
    unsigned char bestLen[CSS_MAX_PROPS];
    unsigned bestSpec[CSS_MAX_PROPS];
    unsigned short bestOrder[CSS_MAX_PROPS];
    unsigned char bestCount = 0;

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
                bestSpec[bi] = 0;
                bestOrder[bi] = 0;
            }
            if (bestVal[bi] &&
                (spec < bestSpec[bi] ||
                 (spec == bestSpec[bi] && r->order <= bestOrder[bi])))
            {
                continue; /* an earlier rule wins this property */
            }
            bestVal[bi] = r->val[pj];
            bestLen[bi] = r->propLen[pj];
            bestSpec[bi] = spec;
            bestOrder[bi] = r->order;
        }
    }
    for (unsigned char bi = 0; bi < bestCount; bi++)
    {
        if (bestVal[bi])
        {
            apply_prop(&out, bestProp[bi], bestLen[bi], bestVal[bi],
                       (size_t)strlen(bestVal[bi]));
        }
    }
    return out;
}
