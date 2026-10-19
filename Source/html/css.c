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

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "core/logger.h"

/* ── Phase-0 skip-class usage tally (HTML+CSS roadmap Phase 0.2) ────────────
 * Counts every stylesheet input the engine cannot honor today, by class, so
 * the gap backlog ranks by real-page usage (PLATFORM_SCORECARD.md). One
 * [css-skip] row per css_log_stats emit; reset after each emit (each row =
 * occurrences since the previous row). Classes:
 *   sib    = '+' / '~' combinators (sheet + querySelector selector paths)
 *   attrOp = [a~=b] / ^= / $= / *= / |= attribute operators
 *   pcls   = refused pseudo-classes + unknown ::pseudo-elements
 *            (compound dropped; the Phase 7 truth family — :target/
 *            :root/:disabled/:enabled/:checked/:placeholder-shown/
 *            :empty — and the consumed ::-family leave this counter)
 *   medF   = unknown @media feature (query correctly evaluates false)
 *   var    = declaration value contains "var("  (census only — var() resolves since Phase 2)
 *   calc   = declaration value contains "calc(" (census only — calc() evaluates since Phase 2)
 *   prop   = declaration name outside the cascade vocabulary (stored, never read)
 *
 * Compile gate: PLUTO_CSS_SKIP_TALLY=0 strips EVERY tally byte — the whole
 * instrument (counters, helpers, emit, hooks) vanishes from the binary.
 * Default 1 (shipping instrumentation). Exists so a "-DPLUTO_NR_AUTOTEST_TAP
 * -DPLUTO_CSS_SKIP_TALLY=0" build is a byte-equivalent no-instrument rig for
 * hypothesis testing (Phase-0 0.4.1) WITHOUT touching git state. */
#ifndef PLUTO_CSS_SKIP_TALLY
#define PLUTO_CSS_SKIP_TALLY 1
#endif

#if PLUTO_CSS_SKIP_TALLY
typedef enum
{
    CSS_SKIP_SIB = 0,
    CSS_SKIP_ATTROP,
    CSS_SKIP_PCLS,
    CSS_SKIP_MEDF,
    CSS_SKIP_VAR,
    CSS_SKIP_CALC,
    CSS_SKIP_PROP,
    CSS_SKIP_COUNT
} CssSkipClass;
static unsigned long g_cssSkip[CSS_SKIP_COUNT];
/* Phase 7: :target's matcher input (the current walk's URL fragment,
 * written by css_set_target — the matcher is engine-free by contract). */
static char g_cssTarget[56];

/* Phase-0.6 upgrade: record WHICH unknown property names appear (top-5,
 * gliding slots, hit counts), so the field data picks the next
 * vocabulary additions by name instead of by count alone. */
#define CSS_PROP_NAMES_MAX 5
#define CSS_PROP_NAME_MAX 14 /* 13 chars + NUL (longer names truncate) */
static char g_cssPropNames[CSS_PROP_NAMES_MAX][CSS_PROP_NAME_MAX];
static unsigned short g_cssPropNameHits[CSS_PROP_NAMES_MAX];
#endif /* PLUTO_CSS_SKIP_TALLY */

/* Per-render emit: the Phase-0 skip-class row (+ Phase-1 names row), one
 * row per css_log_stats call, counters reset after each emit. (The TEMP
 * Wikipedia-stall workload counters that shared this emit are gone —
 * diagnosis concluded, plan executed.) */
void css_log_stats(void)
{
#if PLUTO_CSS_SKIP_TALLY
    logger_log("[css-skip] sib=%lu attrOp=%lu pcls=%lu medF=%lu var=%lu calc=%lu prop=%lu",
               g_cssSkip[CSS_SKIP_SIB], g_cssSkip[CSS_SKIP_ATTROP],
               g_cssSkip[CSS_SKIP_PCLS], g_cssSkip[CSS_SKIP_MEDF],
               g_cssSkip[CSS_SKIP_VAR], g_cssSkip[CSS_SKIP_CALC],
               g_cssSkip[CSS_SKIP_PROP]);
    {
        /* P0.6: the unknown-property NAME roster (may be empty). */
        char line[96];
        size_t off = 0;
        line[0] = '\0';
        for (int i = 0; i < CSS_PROP_NAMES_MAX; i++)
        {
            if (!g_cssPropNames[i][0])
            {
                continue;
            }
            int w = snprintf(line + off, sizeof(line) - off, "%s%.13s x%u",
                             off ? " " : "", g_cssPropNames[i],
                             (unsigned)g_cssPropNameHits[i]);
            if (w < 0 || (size_t)w >= sizeof(line) - off)
            {
                break;
            }
            off += (size_t)w;
        }
        logger_log("[css-propnames] %s", line);
        memset(g_cssPropNames, 0, sizeof(g_cssPropNames));
        memset(g_cssPropNameHits, 0, sizeof(g_cssPropNameHits));
    }
    memset(g_cssSkip, 0, sizeof(g_cssSkip));
#endif /* PLUTO_CSS_SKIP_TALLY */
}

/* ── Per-element match-mask memo ───────────────────────────────────────
 * css_compute tests every rule once per element; css_winning_value (and
 * pseudo content) for the SAME element then re-test the same rules. The
 * mask records each rule's outcome so later lookups skip re-matching and
 * only re-run the cascade for matching rules (few). Pure memoization:
 * DOM + engine are frozen mid-walk, so reuse is exact. Keyed by
 * (node, engine, generation, pseudo-lane): engine guards arena-reused
 * pointers across documents; generation (bumped at each top-level walk)
 * guards JS mutations between walks. Direct-mapped collisions just
 * recompute. ruleCount never exceeds CSS_MAX_RULES (128): bits beyond
 * the mask always re-test. */
#define CSS_MASK_SIZE 128
typedef struct
{
    const void *node;
    const void *eng;
    unsigned gen;
    unsigned char pseudo;
    unsigned char pad[3];
    uint64_t lo;
    uint64_t hi;
    /* Winning specificity per matching rule (max over the rule's MATCHING
     * selectors, exactly as the cascade computes it). Lets consults skip
     * chain re-matching entirely: spec comes from storage, values from
     * the (static) rule. Rules that didn't match read 0 here AND have a
     * clear bit — belt and suspenders against stale reuse. */
    unsigned short spec[CSS_MAX_RULES];
} CssMaskEntry;
static CssMaskEntry g_maskCache[CSS_MASK_SIZE];
static unsigned g_maskGen = 1;

void css_match_cache_bump(void)
{
    g_maskGen++;
    if (g_maskGen == 0)
    {
        g_maskGen = 1;
        memset(g_maskCache, 0, sizeof(g_maskCache));
    }
}

/* Fetch: 1 + entry borrowed (spec array read-only through the call;
 * single thread, no mutation mid-lookup). */
static int css_mask_fetch(const void *node, const void *eng,
                          unsigned char pseudo, uint64_t *lo, uint64_t *hi,
                          const unsigned short **spec)
{
    CssMaskEntry *e;
    if (!node)
    {
        return 0;
    }
    e = &g_maskCache[(((uintptr_t)node) >> 3) & (CSS_MASK_SIZE - 1)];
    if (e->node == node && e->eng == eng && e->gen == g_maskGen &&
        e->pseudo == pseudo)
    {
        *lo = e->lo;
        *hi = e->hi;
        if (spec)
        {
            *spec = e->spec;
        }
        return 1;
    }
    return 0;
}

static void css_mask_store(const void *node, const void *eng,
                           unsigned char pseudo, uint64_t lo, uint64_t hi,
                           const unsigned short *spec)
{
    CssMaskEntry *e;
    if (!node)
    {
        return;
    }
    e = &g_maskCache[(((uintptr_t)node) >> 3) & (CSS_MASK_SIZE - 1)];
    e->node = node;
    e->eng = eng;
    e->gen = g_maskGen;
    e->pseudo = pseudo;
    e->lo = lo;
    e->hi = hi;
    if (spec)
    {
        memcpy(e->spec, spec, sizeof(e->spec));
    }
    else
    {
        memset(e->spec, 0, sizeof(e->spec));
    }
}

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

/* ── 1.6 external <link rel="stylesheet"> scan (Phase 1.5) ──────────────── */
/* Case-insensitive compare of a bounded slice against a lowercase literal. */
static int css_lite_eq_ci(const char *s, const char *z, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(z - s) != n)
    {
        return 0;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c + 32);
        }
        if (c != lit[i])
        {
            return 0;
        }
    }
    return 1;
}

/* Substring (case-insensitive) of a bounded attribute value. */
static int css_lite_has_ci(const char *s, const char *z, const char *lit)
{
    size_t n = strlen(lit);
    if (n == 0 || (size_t)(z - s) < n)
    {
        return 0;
    }
    for (const char *p = s; p + n <= z; p++)
    {
        const char *q = p;
        int ok = 1;
        for (size_t i = 0; i < n && ok; i++)
        {
            char c = *q;
            if (c >= 'A' && c <= 'Z')
            {
                c = (char)(c + 32);
            }
            ok = (c == lit[i]);
            q++;
        }
        if (ok)
        {
            return 1;
        }
    }
    return 0;
}

/* Mini entity decode for an attribute VALUE slice into a bounded buffer:
 * &amp; &lt; &gt; &quot; &#39; (case-sensitive like HTML5 named refs).
 * Any other sequence copies verbatim. Returns bytes written (0 = empty). */
static size_t css_lite_attr_copy(const char *s, const char *z, char *dst,
                                 size_t dstSize)
{
    size_t off = 0;
    for (const char *p = s; p < z && off + 1 < dstSize; p++)
    {
        char c = *p;
        if (c <= 0)
        {
            break; /* raw-html strings are C strings; stop at NUL safety */
        }
        if (c == '&')
        {
            static const struct
            {
                const char *name;
                size_t n;
                char rep;
            } kEnt[] = {
                {"amp;", 5, '&'},  {"lt;", 4, '<'},
                {"gt;", 4, '>'},   {"quot;", 6, '"'},
                {"#39;", 5, '\''},
            };
            int matched = 0;
            for (size_t k = 0; k < sizeof(kEnt) / sizeof(kEnt[0]); k++)
            {
                if (z - p >= (ptrdiff_t)kEnt[k].n &&
                    memcmp(p + 1, kEnt[k].name, kEnt[k].n - 1) == 0)
                {
                    dst[off++] = kEnt[k].rep;
                    p += kEnt[k].n - 1; /* -1: for-loop advances past ';' */
                    matched = 1;
                    break;
                }
            }
            if (matched)
            {
                continue;
            }
        }
        dst[off++] = c;
    }
    dst[off] = '\0';
    return off;
}

/* forward: the @media evaluator (Phase 3 — link media= screening uses
 * the same query language as the sheet path, so media="print",
 * media="screen and (max-width: 700px)", comma lists and not/only all
 * evaluate honestly instead of the old all/screen substring check). */
static int media_holds(const char *qs, const char *qe);

int css_ext_scan_links(const char *html,
                       char hrefs[PLUTO_CSS_EXT_MAX][512],
                       unsigned char mediaOk[PLUTO_CSS_EXT_MAX])
{
    if (!html)
    {
        return 0;
    }
    const char *p = html;
    const char *end = p + strlen(p);
    int n = 0;
    while (p < end && n < PLUTO_CSS_EXT_MAX)
    {
        if (*p == '<' && p + 3 + 1 <= end &&
            css_lite_eq_ci(p + 1, p + 5, "link") &&
            (p[5] == '\0' || p[5] == ' ' || p[5] == '>' || p[5] == '/' ||
             p[5] == '\n' || p[5] == '\t' || p[5] == '\r'))
        {
            const char *tz = p + 5;
            const char *gt = tz;
            while (gt < end && *gt != '>')
            {
                gt++;
            }
            if (gt >= end)
            {
                break; /* truncated tag: rest unusable */
            }
            /* attribute walk: name [= value] */
            const char *ap = tz;
            int isSheet = 0;
            int typeOk = 1;
            int someHref = 0;
            char hrefBuf[512];
            int hrefOff = 0;
            hrefBuf[0] = '\0';
            int mediaAll = 1; /* absent/empty ⇒ all media */
            while (ap < gt)
            {
                while (ap < gt && (css_isspace(*ap) || *ap == '/'))
                {
                    ap++;
                }
                const char *ns = ap;
                while (ap < gt && *ap != '=' && *ap != '>' && *ap != '"' &&
                       *ap != '\'' && (css_isspace(*ap) == 0))
                {
                    ap++;
                }
                const char *ae = ap;
                while (ae > ns && (ae[-1] == ' ' || ae[-1] == '/'))
                {
                    ae--;
                }
                const char *vz = NULL;
                if (ap < gt && *ap == '=')
                {
                    ap++;
                    if (ap < gt && (*ap == '"' || *ap == '\''))
                    {
                        char q = *ap;
                        ap++;
                        vz = ap;
                        while (ap < gt && *ap != q)
                        {
                            ap++;
                        }
                    }
                    else
                    {
                        vz = ap;
                        while (ap < gt && *ap != '>' && css_isspace(*ap) == 0)
                        {
                            ap++;
                        }
                    }
                }
                if (ae - ns > 0 && vz)
                {
                    if (css_lite_eq_ci(ns, ae, "rel"))
                    {
                        if (css_lite_eq_ci(vz, ap, "stylesheet"))
                        {
                            isSheet = 1;
                        }
                    }
                    else if (css_lite_eq_ci(ns, ae, "href"))
                    {
                        someHref = 1;
                        hrefOff =
                            (int)css_lite_attr_copy(vz, ap, hrefBuf,
                                                    sizeof(hrefBuf));
                    }
                    else if (css_lite_eq_ci(ns, ae, "media"))
                    {
                        /* Phase 3: the REAL evaluator — media="" means
                         * every medium; anything else runs the same
                         * query grammar the sheet path uses (print is
                         * honored on the 1-bit panel, matching the
                         * @media print decision). */
                        if (ap == vz)
                        {
                            mediaAll = 1;
                        }
                        else
                        {
                            mediaAll =
                                (unsigned char)(media_holds(vz, ap) ? 1 : 0);
                        }
                    }
                    else if (css_lite_eq_ci(ns, ae, "type"))
                    {
                        if (!css_lite_has_ci(vz, ap, "css"))
                        {
                            typeOk = 0;
                        }
                    }
                }
                if (ap < gt && (*ap == '"' || *ap == '\''))
                {
                    ap++; /* closing quote of a quoted value */
                }
            }
            if (isSheet && someHref && hrefOff > 0 && typeOk)
            {
                int dup = 0;
                for (int i = 0; i < n; i++)
                {
                    if (strcmp(hrefs[i], hrefBuf) == 0)
                    {
                        dup = 1;
                        break;
                    }
                }
                if (!dup)
                {
                    snprintf(hrefs[n], sizeof(hrefs[n]), "%s", hrefBuf);
                    mediaOk[n] = (unsigned char)(mediaAll ? 1 : 0);
                    n++;
                }
                else
                {
                    logger_log("[css-ext] dedupe %s", hrefBuf);
                }
            }
            p = gt + 1;
        }
        else if (*p == '<' && p + 4 < end && p[1] == '!' && p[2] == '-' &&
                 p[3] == '-')
        {
            /* comment span ONLY (<!DOCTYPE falls through to the p++ walk):
             * skip to the matching --> (never scans links inside comments) */
            p += 4;
            while (p < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>'))
            {
                p++;
            }
            p = (p < end) ? p + 3 : end;
        }
        else if (*p == '<' && p + 7 < end &&
                 (css_lite_eq_ci(p + 1, p + 7, "script") &&
                  (p[7] == ' ' || p[7] == '>' || p[7] == '\n' ||
                   p[7] == '\t' || p[7] == '\r')))
        {
            /* raw <script> span: bundles carry link-tag STRINGS in their
             * template literals — skip the whole body. */
            p += 7;
            const char *close = p;
            const char *stop = end;
            while (close + 8 < stop)
            {
                if (css_lite_eq_ci(close, close + 8, "</script"))
                {
                    break;
                }
                close++;
            }
            p = (close + 8 < stop) ? close + 8 : end;
        }
        else
        {
            p++;
        }
    }
    return n;
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
        /* Content-carrying pseudo-elements (Phase 7 keeps ::marker). */
        {"::before", 8, 1},
        {":before", 7, 1},
        {"::after", 7, 2},
        {":after", 6, 2},
        {"::marker", 8, 4},
        /* Consumed-inert family (Phase 7): these never match any
         * element per spec, so the rule parses and is kept — the
         * compound no longer silently drops on real sheets that carry
         * ::first-line decorations, ::-placeholder styles or
         * ::selection/view-transition rules. Counting as a class-level
         * specificity member stays (pseudo-elements are classes for
         * specificity in CSS2 grammar terms). */
        {"::first-line", 12, 3},
        {":first-line", 11, 3},
        {"::first-letter", 13, 3},
        {":first-letter", 12, 3},
        {"::selection", 11, 3},
        {":selection", 10, 3},
        {"::placeholder", 13, 3},
        {"::backdrop", 10, 3},
        {"::view-transition", 16, 3},
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

/* ── Phase 3: media value grammar — milli-px lengths, MQ4 units ────────────── */
/* Real sites write breakpoints in em (22.5em ≙ 360px at the CSS 16px
 * base), viewport units, and the MQ4 unitless ZERO exception
 * ("(min-width: 0)"). Every length parses to a milli-px value so exact
 * comparisons (width == 600px) stay exact; px-only parsing would round
 * those away. */

/* Bare number in milli units: digits [. up-to-3 digits]. When endOut is
 * given it receives one-past-the-digits (caller continues parsing,
 * e.g. a unit suffix or a '/' ratio tail); otherwise the span must be
 * ONLY the number (ws at both ends OK). -1 on any failure. */
static long long media_num_milli(const char *s, const char *z,
                                 const char **endOut)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    int neg = (*s == '-');
    if (s < z && (*s == '-' || *s == '+'))
    {
        s++;
    }
    long long whole = 0;
    int have = 0;
    while (s < z && *s >= '0' && *s <= '9')
    {
        have = 1;
        whole = whole * 10 + (*s - '0');
        if (whole > 100000)
        {
            return -1;
        }
        s++;
    }
    long long frac = 0;
    int fd = 0;
    if (s < z && *s == '.')
    {
        s++;
        while (s < z && *s >= '0' && *s <= '9' && fd < 3)
        {
            frac = frac * 10 + (*s - '0');
            fd++;
            s++;
        }
        while (s < z && *s >= '0' && *s <= '9')
        {
            s++; /* precision past 3 digits: parsed, dropped */
        }
    }
    if (!have)
    {
        return -1;
    }
    static const long long kFracMul[4] = {0, 100, 10, 1};
    long long milli = whole * 1000 + frac * kFracMul[fd];
    if (neg)
    {
        milli = -milli;
    }
    if (endOut)
    {
        *endOut = s;
        return milli;
    }
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    return (s == z) ? milli : -1; /* whole-span mode: only-ws after */
}

/* Media length in milli-px: number + optional unit
 * (px ×1, em/rem ×16 at the CSS base, vw/vh at viewport scale).
 * Unitless is legal ONLY for zero (MQ4 zero exception). When endOut is
 * given it receives one-past-the-unit (caller parses on); otherwise
 * the span must end right there. -1 on failure. */
static long long media_len_milli(const char *s, const char *z,
                                 const char **endOut)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    int neg = (*s == '-');
    if (s < z && (*s == '-' || *s == '+'))
    {
        s++;
    }
    long long whole = 0;
    int have = 0;
    while (s < z && *s >= '0' && *s <= '9')
    {
        have = 1;
        whole = whole * 10 + (*s - '0');
        if (whole > 100000)
        {
            return -1;
        }
        s++;
    }
    long long frac = 0;
    int fd = 0;
    if (s < z && *s == '.')
    {
        s++;
        while (s < z && *s >= '0' && *s <= '9' && fd < 3)
        {
            frac = frac * 10 + (*s - '0');
            fd++;
            s++;
        }
        while (s < z && *s >= '0' && *s <= '9')
        {
            s++; /* precision past 3 digits: parsed, dropped */
        }
    }
    if (!have)
    {
        return -1;
    }
    static const long long kFracMul[4] = {0, 100, 10, 1};
    long long milli = whole * 1000 + frac * kFracMul[fd];
    size_t rem = (size_t)(z - s);
    if (rem >= 2 && (s[0] | 32) == 'p' && (s[1] | 32) == 'x')
    {
        s += 2; /* milli ×1 */
    }
    else if (rem >= 2 && (s[0] | 32) == 'e' && (s[1] | 32) == 'm')
    {
        milli *= 16; /* 1em = 16px (CSS root convention) */
        s += 2;
    }
    else if (rem >= 3 && (s[0] | 32) == 'r' && (s[1] | 32) == 'e' &&
             (s[2] | 32) == 'm')
    {
        milli *= 16; /* rem = same base here */
        s += 3;
    }
    else if (rem >= 2 && (s[0] | 32) == 'v' && (s[1] | 32) == 'w')
    {
        milli = milli * CSS_VIEWPORT_W / 1000; /* 1vw = VW/100 px */
        s += 2;
    }
    else if (rem >= 2 && (s[0] | 32) == 'v' && (s[1] | 32) == 'h')
    {
        milli = milli * CSS_VIEWPORT_H / 1000;
        s += 2;
    }
    else if (rem >= 3 && s[0] == 'v' && s[1] == 'm' && s[2] == 'i' &&
             (s[3] | 32) == 'n')
    {
        milli = milli * CSS_VMINDIM / 1000; /* vmin = min(W,H) */
        s += 4;
    }
    else if (rem >= 3 && s[0] == 'v' && s[1] == 'm' && s[2] == 'a' &&
             (s[3] | 32) == 'x')
    {
        milli = milli * CSS_VMAXDIM / 1000; /* vmax = max(W,H) */
        s += 4;
    }
    else if (rem >= 2 && (s[0] | 32) == 'c' && (s[1] | 32) == 'h')
    {
        /* Phase 9 unit widening: no font metrics ship with the engine,
         * so ch takes the documented half-advance approximation — in
         * monospace-flavored sheets this underestimates ≤ ~5%. */
        milli = milli * 8; /* ch ≈ 0.5em = 8px */
        s += 2;
    }
    else if (rem >= 2 && (s[0] | 32) == 'e' && (s[1] | 32) == 'x')
    {
        milli = milli * 8; /* ex ≈ x-height ≈ 0.5em (documented)
                            * — the classic CSS approximation */
        s += 2;
    }
    else
    {
        if (milli != 0)
        {
            return -1; /* unitless: only plain zero is MQ4-legal */
        }
    }
    if (neg)
    {
        milli = -milli;
    }
    if (endOut)
    {
        *endOut = s;
        return milli;
    }
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    return (s == z) ? milli : -1; /* whole-span mode: unit ends it */
}

/* ci compare of the ONE keyword token in [s, z) (letters plus '-'). */
static int media_kw_eq(const char *s, const char *z, const char *kw)
{
    while (s < z && css_isspace(*s))
    {
        s++;
    }
    const char *e = s;
    while (e < z &&
           (((*e | 32) >= 'a' && (*e | 32) <= 'z') || *e == '-'))
    {
        e++;
    }
    size_t kl = strlen(kw);
    return (size_t)(e - s) == kl && css_eq_ncase(s, kw, kl);
}

/* Resolution value in milli-dppx: <n>(dppx|dpi|dpcm|x) or bare number
 * (bare = dppx — the webkit device-pixel-ratio form). -1 on failure. */
static long long media_dppx_milli(const char *s, const char *z)
{
    const char *tail = NULL;
    long long milli = media_num_milli(s, z, &tail);
    if (milli < 0)
    {
        return -1;
    }
    size_t rem = (size_t)(z - tail);
    if (rem >= 4 && (tail[0] | 32) == 'd' && (tail[1] | 32) == 'p' &&
        (tail[2] | 32) == 'p' && (tail[3] | 32) == 'x')
    {
        tail += 4; /* already milli-dppx */
    }
    else if (rem >= 1 && (tail[0] | 32) == 'x')
    {
        tail += 1; /* 3x = 3 dppx */
    }
    else if (rem >= 3 && (tail[0] | 32) == 'd' && (tail[1] | 32) == 'p' &&
             (tail[2] | 32) == 'i')
    {
        milli = milli * 25 / 2400; /* 96 dpi = 1 dppx */
        tail += 3;
    }
    else if (rem >= 4 && (tail[0] | 32) == 'd' && (tail[1] | 32) == 'p' &&
             (tail[2] | 32) == 'c' && (tail[3] | 32) == 'm')
    {
        milli = milli * 127 / 4800; /* 1 dpcm = 2.54 dpi = 2.54/96 dppx */
        tail += 4;
    }
    /* bare = dppx (webkit ratio shape) */
    while (tail < z && css_isspace(*tail))
    {
        tail++;
    }
    if (tail != z)
    {
        return -1;
    }
    return milli;
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
        long long n = media_len_milli(val, z, NULL);
        if (n < 0)
        {
            return 0;
        }
        long long w = (long long)CSS_VIEWPORT_W * 1000;
        return lo > 0 ? w >= n : lo < 0 ? w <= n : w == n;
    }
    if (strcmp(base, "height") == 0 || strcmp(base, "device-height") == 0)
    {
        if (!hasVal)
        {
            return 0;
        }
        long long n = media_len_milli(val, z, NULL);
        if (n < 0)
        {
            return 0;
        }
        long long h = (long long)CSS_VIEWPORT_H * 1000;
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
    /* ── Phase 3: the remaining MQ4/5 vocabulary, device truth table ──
     * Every answer below is the HONEST value for this panel: 400×240
     * 1-bit grayscale, no hover pointer, coarse primary pointer, LCD
     * refresh, SDR, forced fixed palette, standalone app. Sites then
     * take the branch they would take on a real device of this class —
     * narrow-phone layouts for narrow viewports, 1x-resolution asset
     * variants, calm motion — instead of every unknown-feature branch
     * dying silently. Unknown names STILL reach the medF census below
     * and evaluate false, so containment is unchanged. */
    if (strcmp(base, "aspect-ratio") == 0 ||
        strcmp(base, "device-aspect-ratio") == 0)
    {
        if (!hasVal)
        {
            return 0;
        }
        const char *slash = NULL;
        for (const char *q = val; q < z; q++)
        {
            if (*q == '/')
            {
                slash = q;
                break;
            }
        }
        const char *nt = NULL;
        long long num = media_num_milli(val, slash ? slash : z, &nt);
        if (num <= 0)
        {
            return 0; /* aspect must be positive */
        }
        long long den = 1000; /* bare number = N/1 */
        if (slash)
        {
            while (nt < slash && css_isspace(*nt))
            {
                nt++;
            }
            if (nt != slash)
            {
                return 0; /* junk between number and '/' */
            }
            den = media_num_milli(slash + 1, z, NULL);
            if (den <= 0)
            {
                return 0;
            }
        }
        /* cross-multiply in px×milli: exact, no floats */
        long long l = (long long)CSS_VIEWPORT_W * den;
        long long r = num * CSS_VIEWPORT_H;
        return lo > 0 ? l >= r : lo < 0 ? l <= r : l == r;
    }
    if (strcmp(base, "resolution") == 0 ||
        strcmp(base, "device-pixel-ratio") == 0 ||
        strcmp(base, "-webkit-device-pixel-ratio") == 0)
    {
        /* 1 CSS px per device pixel (dppx = 1): sites asking for 2x
         * variants stay false so their 1x assets serve — the right
         * choice for this envelope. */
        if (!hasVal)
        {
            return 0;
        }
        long long req = media_dppx_milli(val, z);
        if (req < 0)
        {
            return 0;
        }
        long long self = 1000; /* 1 dppx in milli form */
        return lo > 0 ? self >= req : lo < 0 ? self <= req : self == req;
    }
    if (strcmp(base, "prefers-contrast") == 0)
    {
        /* 1-bit panel = maximum contrast; "more" is the true branch. */
        if (!hasVal)
        {
            return 1;
        }
        if (lo)
        {
            return 0;
        }
        return media_kw_eq(val, z, "more");
    }
    if (strcmp(base, "forced-colors") == 0 ||
        strcmp(base, "prefers-reduced-motion") == 0 ||
        strcmp(base, "prefers-reduced-transparency") == 0 ||
        strcmp(base, "prefers-reduced-data") == 0)
    {
        /* forced colors: the palette IS fixed (active). Motion and
         * transparency: a reflective e-ink-class screen prefers calm.
         * Bare forms mean "not the no/calm default" — true here. */
        if (!hasVal)
        {
            return 1;
        }
        if (lo)
        {
            return 0;
        }
        if (strcmp(base, "forced-colors") == 0)
        {
            return media_kw_eq(val, z, "active");
        }
        return media_kw_eq(val, z, "reduce");
    }
    if (strcmp(base, "hover") == 0 || strcmp(base, "any-hover") == 0 ||
        strcmp(base, "pointer") == 0 || strcmp(base, "any-pointer") == 0)
    {
        /* Crank/d-pad input: no persistent hover, coarse primary
         * pointer — phone-parity, so touch-first layouts apply. */
        if (!hasVal)
        {
            return memcmp(base, "hover", 5) == 0 ? 0 : 1;
        }
        if (lo)
        {
            return 0;
        }
        if (memcmp(base, "pointer", 7) == 0 ||
            memcmp(base, "any-pointer", 11) == 0)
        {
            return media_kw_eq(val, z, "coarse");
        }
        return media_kw_eq(val, z, "none");
    }
    if (strcmp(base, "color-index") == 0)
    {
        if (!hasVal)
        {
            return 0; /* 0 palette entries: boolean false */
        }
        int n = media_int(val, z);
        if (n < 0)
        {
            return 0;
        }
        return lo > 0 ? 0 >= n : lo < 0 ? 0 <= n : 0 == n;
    }
    if (strcmp(base, "grid") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 0; /* 0: not a character-grid terminal */
        }
        int n = media_int(val, z);
        if (n < 0)
        {
            return 0;
        }
        return n == 0;
    }
    if (strcmp(base, "scan") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "progressive");
    }
    if (strcmp(base, "scripting") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "enabled");
    }
    if (strcmp(base, "display-mode") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "standalone");
    }
    if (strcmp(base, "inverted-colors") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "none");
    }
    if (strcmp(base, "update") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "fast"); /* 50Hz LCD refresh */
    }
    if (strcmp(base, "dynamic-range") == 0 ||
        strcmp(base, "video-dynamic-range") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "standard"); /* SDR panel */
    }
    if (strcmp(base, "overflow-block") == 0 ||
        strcmp(base, "overflow-inline") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "scroll"); /* continuous crank scroll */
    }
    if (strcmp(base, "environment-blending") == 0)
    {
        if (lo)
        {
            return 0;
        }
        if (!hasVal)
        {
            return 1;
        }
        return media_kw_eq(val, z, "opaque");
    }
#if PLUTO_CSS_SKIP_TALLY
    g_cssSkip[CSS_SKIP_MEDF]++;
#endif
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

#if PLUTO_CSS_SKIP_TALLY
/* Is this declaration name inside the cascade's known vocabulary? The union
 * of (a) every property the per-element computed-flags ledger honors
 * (apply_prop arms below) and (b) every name any css_winning_value or
 * css_pseudo_content call site queries (document.c walk, box/table paths).
 * Anything else is stored by add_prop but NEVER read: a stored-but-inert
 * declaration — the Phase-0 "prop=" usage gauge. Fan-out shorthands
 * (border/margin/padding/background) never store as their bare name, so
 * their longhands (above) are the honest check. */
static int css_known_prop(const char *n, size_t len)
{
    static const struct
    {
        const char *n;
        unsigned char len;
    } kKnown[] = {
        /* apply_prop (computed-flags ledger): */
        {"display", 7},          {"visibility", 10},
        {"text-align", 10},      {"font-weight", 11},
        {"font-style", 10},      {"text-decoration", 15},
        {"text-transform", 14},  {"white-space", 11},
        {"overflow-wrap", 13},   {"word-break", 10},
        /* Phase 4 positioning vocabulary (apply_prop arms): */
        {"position", 8},         {"top", 3},
        {"right", 5},            {"bottom", 6},
        {"left", 4},             {"z-index", 7},
        /* Phase 5 overflow family (apply_prop arms / walker recipe /
         * block-hint clamp transport): */
        {"overflow", 8},         {"text-overflow", 13},
        {"clip-path", 9},        {"clip", 4},
        {"-webkit-line-clamp", 18},
        /* Phase 6 flex/grid family — the gap trio is read by the
         * walker's container transport (css_winning_value call sites,
         * document.c flex_gap_px); the DISTRIBUTION names (direction,
         * wrap, order, justify-*, place-*, grid-template/area/
         * auto-*, grow/shrink/basis) carry no 1-bit behavior (the
         * column axis is the flow itself) but match in apply_prop's
         * inspector arm — consumed per the criteria, not silent. */
        {"gap", 3},              {"row-gap", 7},
        {"column-gap", 10},
        {"flex-direction", 14},  {"flex-flow", 9},
        {"flex-wrap", 9},        {"flex", 4},
        {"flex-grow", 9},        {"flex-shrink", 11},
        {"flex-basis", 10},      {"order", 5},
        {"justify-content", 15}, {"align-items", 11},
        {"align-self", 10},      {"align-content", 13},
        {"place-items", 11},     {"place-content", 13},
        {"grid-template-columns", 21}, {"grid-template-rows", 18},
        {"grid-template-areas", 19},   {"grid-auto-columns", 17},
        {"grid-auto-rows", 14},  {"grid-auto-flow", 14},
        {"grid-column", 11},     {"grid-row", 8},
        {"grid-area", 9},
        /* Consumed since the earliest phases via css_winning_value call
         * sites (tables) and now by the Phase 5 tiny-box recipe probe —
         * the probe fixture exposed this gap: width/height always
         * belonged in the census vocabulary. */
        {"width", 5},            {"height", 6},
        {"font-size", 9},        {"color", 5},
        /* Phase 9 transform lane: `transform` values parse at the walk
         * side (translate/translateX/translateY = real paint offsets via
         * DocBlock.tfX/tfY — px and % both); rotate/scale/skew/matrix =
         * the documented 2-D-compositor policy non-feature (consumed as
         * census, no raster, trajectory line). transform-origin is the
         * box-relative geometry knob (consumed vocabulary). */
        {"transform", 9},        {"transform-origin", 16},
        /* Phase 10 animation lane: the transition + animation families
         * join as consumed vocabulary (census), and the subset the END
         * events need reads at the walk side: animation-name/animation
         * (registry-resolved), animation-iteration-count (infinite/zero
         * refuses) and animation-play-state (paused refuses). Timing
         * function/delay/direction/fill-mode/transition-* remainder =
         * the flat renderer's no-timeline policy (consumed, no
         * raster — same class as the Phase 9 2-D-compositor set). */
        {"transition", 10},      {"transition-property", 19},
        {"transition-duration", 19},
        {"transition-timing-function", 26},
        {"transition-delay", 16},
        {"animation", 9},        {"animation-name", 14},
        {"animation-duration", 18},
        {"animation-timing-function", 25},
        {"animation-delay", 15},
        {"animation-iteration-count", 25},
        {"animation-play-state", 20},
        {"animation-fill-mode", 19},
        {"animation-direction", 19},
        /* winning_value / pseudo-content call sites: */
        {"background-color", 16}, {"border-collapse", 15},
        {"border-style", 12},     {"border-width", 12},
        {"float", 5},             {"list-style-type", 15},
        {"text-indent", 11},      {"width", 5},
        {"padding-left", 12},     {"content", 7},
        {"margin-top", 10},       {"margin-right", 12},
        {"margin-bottom", 13},    {"margin-left", 11},
        {"padding-top", 11},      {"padding-right", 13},
        {"padding-bottom", 14},
    };
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); i++)
    {
        if (len == kKnown[i].len && css_eq_ncase(n, kKnown[i].n, len))
        {
            return 1;
        }
    }
    return 0;
}

/* Case-insensitive needle search inside a bounded value slice (values are
 * length-delimited, never NUL-terminated). Used by the Phase-0 var/calc
 * usage gauge in add_prop. */
static int css_value_has_ci(const char *v, size_t len, const char *needle)
{
    size_t nl = strlen(needle);
    if (len < nl)
    {
        return 0;
    }
    for (size_t i = 0; i + nl <= len; i++)
    {
        if (css_eq_ncase(v + i, needle, nl))
        {
            return 1;
        }
    }
    return 0;
}
#endif /* PLUTO_CSS_SKIP_TALLY */

/* Parse one compound chunk [s,z) into out; 1 ok, 0 unusable.
 * Handles type / .class / #id / * / [attr] / [attr=value] with the full
 * operator set [a~=b] [a|=b] [a^=b] [a$=b] [a*=b] (Phase 1) and
 * combinations ("div.note#x[hidden]"). At most one attribute test per
 * compound. :not(<simple>) (one simple token) also parses here. */
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
            unsigned char op = 0; /* presence */
            if (vs < ce && *vs != '=' && *vs != ']')
            {
                /* [a~=b] ^= $= *= |= — the Phase-1 operator set. The
                 * operator char + '=' are consumed together; the value
                 * reader below handles the rest. Anything else is junk
                 * inside brackets (unusable, tallied). */
                if ((*vs == '~' || *vs == '|' || *vs == '^' || *vs == '$' ||
                     *vs == '*') &&
                    vs + 1 < ce && vs[1] == '=')
                {
                    op = (*vs == '~') ? 2
                                      : (*vs == '|') ? 3
                                                     : (*vs == '^') ? 4
                                                                    : (*vs == '$') ? 5
                                                                                   : 6;
                    vs++;
                    vs = skip_ws(vs + 1, ce);
                    if (vs < ce && *vs == '=')
                    {
                        return 0; /* double '=': junk */
                    }
                }
                else
                {
                    return 0; /* junk inside brackets */
                }
            }
            out->attr = ns;
            out->attrLen = (unsigned char)(ne - ns);
            if (op != 0 || (vs < ce && *vs == '='))
            {
                /* Value compare (plain '=', or the op consumed both of
                 * its characters above — the reader continues from the
                 * value start either way). */
                if (vs < ce && *vs == '=')
                {
                    vs++;
                }
                const char *ws = (vs < ce) ? skip_ws(vs, ce) : vs;
                if (ws >= ce)
                {
                    return 0; /* operator/plain '=' with no value */
                }
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
                out->attrOp = op;
                if (op == 0)
                {
                    out->attrOp = 1; /* plain [a=v]: equality */
                }
            }
            else if (op != 0)
            {
                return 0; /* [a^=] with no value: unusable */
            }
            else
            {
                out->attrOp = 0; /* [attr] presence only */
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
                /* Unknown pseudo-ELEMENT (::v): a remaining :: tail
                 * means a vendor/ext name the strip table doesn't know
                 * (::view-transition-group, ::-webkit-scrollbar…).
                 * Spec: pseudo-element rules never match elements, so
                 * the drop is CSS-correct — but the census counts it
                 * now (Phase 7), same lane as refused :pseudo-classes,
                 * so a sheet's tail is disclosed and its size known. */
#if PLUTO_CSS_SKIP_TALLY
                g_cssSkip[CSS_SKIP_PCLS]++;
#endif
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
            else if ((ilen == 5 && css_eq_ncase(p + 1, "hover", 5)) ||
                     (ilen == 5 && css_eq_ncase(p + 1, "focus", 5)) ||
                     (ilen == 6 && css_eq_ncase(p + 1, "active", 6)) ||
                     (ilen == 7 && css_eq_ncase(p + 1, "visited", 7)) ||
                     (ilen == 13 &&
                      css_eq_ncase(p + 1, "focus-visible", 13)) ||
                     (ilen == 12 &&
                      css_eq_ncase(p + 1, "focus-within", 12)))
            {
                /* Interactivity states (Phase 1; Phase 7 adds the focus
                 * VARIANTS): ALWAYS MATCH on this device. No persistent
                 * hover exists on a 1-bit panel, and refusing used to
                 * hide desktop-menu content forever when a stylesheet
                 * expected :hover/:focus to reveal it. Same reader
                 * stance covers :focus-visible (keyboard-focus
                 * affordances render always) and :focus-within (the
                 * ancestor-of-focus styles: a panel that hides content
                 * until focus must not hide it forever). Documented
                 * policy decision (a). */
                out->pclass = 8;
                p = t;
            }
            else if (ilen == 8 && css_eq_ncase(p + 1, "disabled", 8))
            {
                /* Phase 7 form-truth states — honest attr/device match,
                 * no always-match: the matcher reads the REAL control
                 * state (see the 9..15 arms in css_chain_matches). */
                out->pclass = 9;
                p = t;
            }
            else if (ilen == 7 && css_eq_ncase(p + 1, "enabled", 7))
            {
                out->pclass = 10;
                p = t;
            }
            else if (ilen == 7 && css_eq_ncase(p + 1, "checked", 7))
            {
                out->pclass = 11;
                p = t;
            }
            else if (ilen == 17 &&
                     css_eq_ncase(p + 1, "placeholder-shown", 17))
            {
                out->pclass = 12;
                p = t;
            }
            else if (ilen == 6 && css_eq_ncase(p + 1, "target", 6))
            {
                out->pclass = 13;
                p = t;
            }
            else if (ilen == 4 && css_eq_ncase(p + 1, "root", 4))
            {
                out->pclass = 14;
                p = t;
            }
            else if (ilen == 5 && css_eq_ncase(p + 1, "empty", 5))
            {
                out->pclass = 15;
                p = t;
            }
            else if (ilen == 3 && css_eq_ncase(p + 1, "not", 3) &&
                     p + 4 < z && p[4] == '(')
            {
                /* :not(<simple>) — ONE simple token (CSS3 shape):
                 * element tag / .class / #id / [attr]; case alongside
                 * the token (the node must NOT satisfy it). */
                const char *q = p + 5; /* past '(' */
                while (q < z && css_isspace(*q))
                {
                    q++;
                }
                unsigned char k = 0;
                const char *tok = q;
                if (q < z && *q == '.')
                {
                    k = 2;
                    q++;
                    tok = q;
                }
                else if (q < z && *q == '#')
                {
                    k = 3;
                    q++;
                    tok = q;
                }
                else if (q < z && *q == '[')
                {
                    k = 4;
                    q++;
                    tok = q;
                }
                else if (q < z && (((*q | 32) >= 'a' && (*q | 32) <= 'z')))
                {
                    k = 1;
                    q++;
                }
                const char *te = q;
                if (k == 1)
                {
                    while (te < z &&
                           (((*te | 32) >= 'a' && (*te | 32) <= 'z') ||
                            (*te >= '0' && *te <= '9') || *te == '-' ||
                            *te == '_'))
                    {
                        te++;
                    }
                }
                else if (k == 2)
                {
                    while (te < z && !css_isspace(*te) && *te != ')' &&
                           *te != '[' && *te != ':' && *te != '.' &&
                           *te != '#' && *te != '>' && *te != '+' &&
                           *te != '~' && *te != '*')
                    {
                        te++;
                    }
                }
                else if (k == 3)
                {
                    while (te < z && !css_isspace(*te) && *te != ')' &&
                           *te != '[' && *te != ':' && *te != '.' &&
                           *te != '#')
                    {
                        te++;
                    }
                }
                else
                {
                    /* [attr] presence: token runs to ']'; anything else
                     * (a value compare) is out of scope for the ONE-
                     * simple-selector :not of CSS3. */
                    while (te < z && !css_isspace(*te) && *te != ']' &&
                           *te != ')' && *te != '=')
                    {
                        te++;
                    }
                }
                if (k == 0 || te == tok || (size_t)(te - tok) > 255)
                {
                    return 0; /* no token / nested ops / huge token */
                }
                if (k == 4)
                {
                    /* consume the closing ']' of the attribute test */
                    while (te < z && css_isspace(*te))
                    {
                        te++;
                    }
                    if (te < z && *te == ']')
                    {
                        te++;
                    }
                    else
                    {
                        return 0;
                    }
                }
                const char *re = te;
                while (re < z && css_isspace(*re))
                {
                    re++;
                }
                if (re >= z || *re != ')')
                {
                    return 0; /* junk or unsupported second token */
                }
                out->notKind = k;
                out->notTok = tok;
                out->notLen = (unsigned char)(te - tok);
                p = re + 1;
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
#if PLUTO_CSS_SKIP_TALLY
                g_cssSkip[CSS_SKIP_PCLS]++;
#endif
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
    if (c->notKind)
    {
        /* :not(inner): the inner token counts (tag = element, others =
         * class-level), per CSS specificity rules. */
        v += (c->notKind == 1) ? 0x0001 : 0x0010;
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
static void add_prop(CssEngine *eng, CssRule *r, const char *name,
                     size_t nlen, const char *val, size_t vlen,
                     int important);
static void expand_box(CssEngine *eng, CssRule *r, const char *name,
                       size_t nlen, const char *vs, size_t vlen,
                       int important)
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
        add_prop(eng, r, names[side], lens[side], tok[map[side]], tlen[map[side]],
                 important);
    }
}

/* ── Phase 2a: custom property (var) table fill ────────────────────────────── */
/* Store or update a --custom declaration in the engine table. LAST
 * declaration wins unless the stored entry was !important and the new
 * one is not (importance beats order, mirroring the rule cascade).
 * Names over 11 chars and entries past the cap are refused silently —
 * a later var() lookup then fails openly and the raw value keeps
 * flowing (same visible behavior as pre-Phase-2 pages). */
static void css_custom_store(CssEngine *eng, const char *name, size_t nlen,
                             const char *val, size_t vlen, int important)
{
    if (!eng || nlen == 0 || nlen >= CSS_CUSTOM_NAME || vlen == 0 ||
        vlen > 255)
    {
        return;
    }
    for (int i = 0; i < eng->customCount; i++)
    {
        if (strlen(eng->custom[i].name) == nlen &&
            memcmp(eng->custom[i].name, name, nlen) == 0)
        {
            if (!eng->custom[i].important || important)
            {
                eng->custom[i].val = val;
                eng->custom[i].valLen = (unsigned char)vlen;
                eng->custom[i].important = (unsigned char)(important ? 1 : 0);
            }
            return;
        }
    }
    if (eng->customCount >= CSS_CUSTOM_MAX)
    {
        return;
    }
    CssCustom *c = &eng->custom[eng->customCount++];
    memset(c, 0, sizeof(*c));
    memcpy(c->name, name, nlen);
    c->name[nlen] = '\0';
    c->val = val;
    c->valLen = (unsigned char)vlen;
    c->important = (unsigned char)(important ? 1 : 0);
}

static void add_prop(CssEngine *eng, CssRule *r, const char *name,
                     size_t nlen, const char *val, size_t vlen,
                     int important)
{
    if (nlen == 0 || vlen == 0)
    {
        return;
    }
    if (nlen >= 2 && name[0] == '-' && name[1] == '-')
    {
        /* Phase 2: --custom declarations register in the engine's
         * var() table, NEVER as a rule property (no consumer queries a
         * custom name) and never in the names gauge (a --name is not a
         * missing vocabulary item — it is a variable definition). */
        css_custom_store(eng, name + 2, nlen - 2, val, vlen, important);
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
#if PLUTO_CSS_SKIP_TALLY
    /* Phase-0 skip-class tally: fresh declarations only (dedup path returns
     * above). See the block comment near the top of this file. */
    if (!css_known_prop(name, nlen))
    {
        g_cssSkip[CSS_SKIP_PROP]++;
        /* P0.6: log the name (dedup, gliding min-count slot replace). */
        size_t keep = (nlen < CSS_PROP_NAME_MAX - 1)
                          ? nlen
                          : (size_t)(CSS_PROP_NAME_MAX - 1);
        int slot = -1;
        for (int i = 0; i < CSS_PROP_NAMES_MAX; i++)
        {
            if (strlen(g_cssPropNames[i]) == keep &&
                memcmp(g_cssPropNames[i], name, keep) == 0)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
        {
            unsigned short minC = 0xFFFF;
            for (int i = 0; i < CSS_PROP_NAMES_MAX; i++)
            {
                if (g_cssPropNameHits[i] < minC)
                {
                    minC = g_cssPropNameHits[i];
                    slot = i;
                }
            }
            memcpy(g_cssPropNames[slot], name, keep);
            g_cssPropNames[slot][keep] = '\0';
            g_cssPropNameHits[slot] = 0;
        }
        if (g_cssPropNameHits[slot] < 0xFFFF)
        {
            g_cssPropNameHits[slot]++;
        }
    }
    if (css_value_has_ci(val, vlen, "var("))
    {
        g_cssSkip[CSS_SKIP_VAR]++;
    }
    if (css_value_has_ci(val, vlen, "calc("))
    {
        g_cssSkip[CSS_SKIP_CALC]++;
    }
#endif /* PLUTO_CSS_SKIP_TALLY */
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

static void parse_decl_block(CssEngine *eng, CssRule *r, const char *p,
                             const char *e)
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
                    add_prop(eng, r, "border-width", 12, vs, (size_t)(ve - vs),
                             important);
                    add_prop(eng, r, "border-style", 12, vs, (size_t)(ve - vs),
                             important);
                }
                else if ((nlen == 6 && css_eq_ncase(ns, "margin", 6)) ||
                         (nlen == 7 && css_eq_ncase(ns, "padding", 7)))
                {
                    /* margin/padding fan out to side longhands (never
                     * queried as shorthands). 1→all, 2→tb/rl, 3→t/rl/b,
                     * 4→t/r/b/l; >4 tokens is invalid (dropped). Token
                     * slices point into the value (same lifetime). */
                    expand_box(eng, r, ns, nlen, vs, (size_t)(ve - vs),
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
                        add_prop(eng, r, "background-color", 16, tok, tlen,
                                 important);
                    }
                }
                else
                {
                    add_prop(eng, r, ns, nlen, vs, (size_t)(ve - vs), important);
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

/* ── Phase 10: @keyframes name registry ───────────────────────────────────
 * A NAME span is a CSS ident: letters, digits, hyphens, underscores.
 * Quotes/braces/colons/others never name a rule here (they guard the
 * conservative extraction — the token after `@keyframes` is not a name,
 * so nothing registers). */
static int css_keyframes_name_chars(const char *s, const char *e)
{
    if (s >= e)
    {
        return 0;
    }
    for (const char *p = s; p < e; p++)
    {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
        {
            return 0;
        }
    }
    return 1;
}

int css_keyframes_known(const CssEngine *eng, const char *name, size_t len)
{
    if (!eng || !name || len == 0 || len > CSS_KFRAMES_NAME - 1)
    {
        return 0;
    }
    for (int i = 0; i < eng->keyframeCount; i++)
    {
        const char *k = eng->keyframes[i];
        if (strlen(k) == len && css_eq_ncase(k, name, len))
        {
            return 1;
        }
    }
    return 0;
}

int css_keyframes_collect(CssEngine *eng, const char *name, size_t len)
{
    if (!eng || !name || len == 0 || len > CSS_KFRAMES_NAME - 1)
    {
        return 0;
    }
    if (css_keyframes_known(eng, name, len))
    {
        return 1; /* duplicate definition: the name is what matters */
    }
    if (eng->keyframeCount >= CSS_KFRAMES_MAX)
    {
        return 0;
    }
    char *dst = eng->keyframes[eng->keyframeCount];
    memcpy(dst, name, len);
    dst[len] = '\0';
    eng->keyframeCount++;
    return 1;
}

/* The animation shorthand's non-name tokens (spec grammar keywords that
 * are NOT @keyframes-name candidates, plus "none"). Functional notation
 * (steps()/cubic-bezier()) carries '(' — never an ident candidate anyway. */
static const char *const ANIM_KWS[] = {
    "none", "infinite", "linear", "ease", "ease-in", "ease-out",
    "ease-in-out", "step-start", "step-end", "normal", "alternate",
    "reverse", "alternate-reverse", "forwards", "backwards", "both",
    "running", "paused",
};

const char *css_anim_registered_name(const CssEngine *eng, const char *val,
                                     size_t len, size_t *outLen)
{
    if (!eng || !val || len == 0)
    {
        return NULL;
    }
    size_t i = 0;
    while (i < len)
    {
        while (i < len && (val[i] == ' ' || val[i] == '\t' ||
                           val[i] == '\n' || val[i] == '\r' ||
                           val[i] == ','))
        {
            i++;
        }
        size_t s = i;
        while (i < len && val[i] != ' ' && val[i] != '\t' &&
               val[i] != '\n' && val[i] != '\r' && val[i] != ',')
        {
            i++;
        }
        size_t tlen = i - s;
        if (tlen == 0)
        {
            break; /* trailing whitespace */
        }
        char c = val[s];
        /* numeric spans (durations/iteration counts) are not names */
        if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-')
        {
            continue;
        }
        /* functional notation (steps(3), cubic-bezier(...)) */
        int isFn = 0;
        for (size_t j = 0; j < tlen; j++)
        {
            if (val[s + j] == '(')
            {
                isFn = 1;
                break;
            }
        }
        if (isFn)
        {
            continue;
        }
        int isKw = 0;
        for (size_t j = 0; j < sizeof(ANIM_KWS) / sizeof(ANIM_KWS[0]); j++)
        {
            size_t kl = strlen(ANIM_KWS[j]);
            if (kl == tlen && css_eq_ncase(val + s, ANIM_KWS[j], tlen))
            {
                isKw = 1;
                break;
            }
        }
        if (isKw)
        {
            continue;
        }
        if (css_keyframes_known(eng, val + s, tlen))
        {
            if (outLen)
            {
                *outLen = tlen;
            }
            return val + s;
        }
        /* unregistered tokens keep scanning (comma lists may hold both
         * an inert and a live run); nothing further here */
    }
    return NULL;
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
                /* Phase 10: @keyframes NAME extraction — the frame bodies
                 * still skip whole (below), but the NAME itself feeds the
                 * registry so `animation:`/`animation-name:` values can
                 * resolve to a declared run and the walk's animation lane
                 * can engage an end event. Both spellings (unprefixed and
                 * -webkit-) share the semantics. The name must carry
                 * BEFORE the body brace; anything else (locations,
                 * import skins) never matches these prefixes. */
                const char *kf = p + 1;
                if ((size_t)(e - kf) >= 17 &&
                    css_eq_ncase(kf, "-webkit-keyframes", 17))
                {
                    kf += 17;
                }
                else if ((size_t)(e - kf) >= 9 &&
                         css_eq_ncase(kf, "keyframes", 9))
                {
                    kf += 9;
                }
                else
                {
                    kf = NULL;
                }
                if (kf)
                {
                    const char *kbrace = css_find_from(p, e, "{");
                    while (kbrace && kf < kbrace &&
                           (*kf == ' ' || *kf == '\t' || *kf == '\n' ||
                            *kf == '\r'))
                    {
                        kf++;
                    }
                    const char *kend = kf;
                    while (kbrace && kend < kbrace && *kend != ' ' &&
                           *kend != '\t' && *kend != '\n' && *kend != '\r' &&
                           *kend != ';')
                    {
                        kend++;
                    }
                    if (kf && kbrace && kend > kf &&
                        css_keyframes_name_chars(kf, kend))
                    {
                        css_keyframes_collect(eng, kf, (size_t)(kend - kf));
                    }
                }
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
                                    if (np == 0 || needComb)
                                    {
                                        bad = 1;
                                        break;
                                    }
                                    comb = (*cs == '+') ? 2 : 3;
                                    needComb = 1;
                                    cs++;
                                    continue;
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
                    parse_decl_block(eng, r, brace + 1, bend);
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
        size_t n = strlen(v);
        if (!c->attrVal)
        {
            /* [attr] presence — nothing more to test. */
        }
        else if (c->attrOp == 2)
        {
            /* [a~=v]: v is one whitespace-separated word (case-
             * sensitive, same token compare as .class). */
            int hit = 0;
            const char *w = v;
            while (*w)
            {
                const char *t = w;
                while (*t && !css_isspace(*t))
                {
                    t++;
                }
                if ((size_t)(t - w) == c->attrValLen &&
                    memcmp(w, c->attrVal, c->attrValLen) == 0)
                {
                    hit = 1;
                    break;
                }
                if (!*t)
                {
                    break;
                }
                w = t + 1;
            }
            if (!hit)
            {
                return 0;
            }
        }
        else if (c->attrOp == 3)
        {
            /* [a|=v]: equal, or prefix followed by '-' (lang codes). */
            if (n < c->attrValLen ||
                memcmp(v, c->attrVal, c->attrValLen) != 0 ||
                (n > c->attrValLen && v[c->attrValLen] != '-'))
            {
                return 0;
            }
        }
        else if (c->attrOp == 4)
        {
            /* [a^=v]: value starts with v. */
            if (n < c->attrValLen ||
                memcmp(v, c->attrVal, c->attrValLen) != 0)
            {
                return 0;
            }
        }
        else if (c->attrOp == 5)
        {
            /* [a$=v]: value ends with v. */
            if (n < c->attrValLen ||
                memcmp(v + n - c->attrValLen, c->attrVal,
                       c->attrValLen) != 0)
            {
                return 0;
            }
        }
        else if (c->attrOp == 6)
        {
            /* [a*=v]: value contains v. */
            int hit = 0;
            for (size_t i = 0; i + c->attrValLen <= n; i++)
            {
                if (v[i] == c->attrVal[0] &&
                    memcmp(v + i, c->attrVal, c->attrValLen) == 0)
                {
                    hit = 1;
                    break;
                }
            }
            if (!hit)
            {
                return 0;
            }
        }
        else
        {
            /* [a=v] plain equality (case-sensitive). */
            if (n != c->attrValLen ||
                memcmp(v, c->attrVal, c->attrValLen) != 0)
            {
                return 0;
            }
        }
    }
    if (c->pclass == 8)
    {
        /* :hover/:focus/:active/:visited + Phase 7 additions
         * (:focus-visible/:focus-within) — interactivity states ALWAYS
         * match (Phase 1 policy (a)). A 1-bit panel has no persistent
         * hover, and desktop stylesheets often keep menus/panels
         * invisible until a state fires — refusing hid that content
         * forever. No structural hooks needed; no position walk. */
    }
    else if (c->pclass >= 9)
    {
        /* Phase 7 form/document-truth states: HONEST matchers backed by
         * real device state (attributes, the navigation fragment, the
         * root element, content children). Unlike the 8-family these
         * CAN genuinely fail, so they never join the always-match
         * policy — a hidden disabled control is exactly what the
         * author asked for. */
        if (!node)
        {
            return 0;
        }
        if (c->pclass == 9)
        {
            /* :disabled — the control's real disabled attribute. */
            if (!dom || !dom->attr || !dom->attr(node, "disabled", 8))
            {
                return 0;
            }
        }
        else if (c->pclass == 10)
        {
            /* :enabled — an ENABLEABLE control tag (the disableable
             * form family) carrying no disabled attribute; anchors and
             * plain text are not disableable and never match. */
            if (!dom || !dom->attr)
            {
                return 0;
            }
            size_t tl = tagLower ? strlen(tagLower) : 0;
            int enableable = 0;
            const char *tags[] = {"input",    "button", "select",
                                  "textarea", "option", "optgroup",
                                  "fieldset"};
            size_t tlens[] = {5, 6, 6, 8, 6, 8, 8};
            for (size_t ti = 0; ti < sizeof(tags) / sizeof(tags[0]); ti++)
            {
                if (tl == tlens[ti] &&
                    css_eq_ncase(tagLower, tags[ti], tlens[ti]))
                {
                    enableable = 1;
                    break;
                }
            }
            if (!enableable || dom->attr(node, "disabled", 8))
            {
                return 0;
            }
        }
        else if (c->pclass == 11)
        {
            /* :checked — checkbox/radio with the checked attribute, or
             * an <option> carrying selected (Selectors 4 §:checked). */
            if (!dom || !dom->attr)
            {
                return 0;
            }
            size_t tl = tagLower ? strlen(tagLower) : 0;
            int chk = 0;
            if (tl == 5 && css_eq_ncase(tagLower, "input", 5))
            {
                const char *tv = dom->attr(node, "type", 4);
                if (tv && (strcmp(tv, "checkbox") == 0 ||
                           strcmp(tv, "radio") == 0))
                {
                    chk = dom->attr(node, "checked", 7) != NULL;
                }
            }
            else if (tl == 6 && css_eq_ncase(tagLower, "option", 6))
            {
                chk = dom->attr(node, "selected", 8) != NULL;
            }
            if (!chk)
            {
                return 0;
            }
        }
        else if (c->pclass == 12)
        {
            /* :placeholder-shown — a text-bearing control whose
             * placeholder attribute resolves non-empty (that IS the
             * text the device shows). */
            if (!dom || !dom->attr || !tagLower)
            {
                return 0;
            }
            size_t tl = strlen(tagLower);
            int inputy =
                (tl == 5 && css_eq_ncase(tagLower, "input", 5)) ||
                (tl == 8 && css_eq_ncase(tagLower, "textarea", 8));
            const char *pv = dom->attr(node, "placeholder", 11);
            if (!inputy || !pv || !pv[0])
            {
                return 0;
            }
        }
        else if (c->pclass == 13)
        {
            /* :target — the node's id equals the CURRENT navigation
             * fragment (css_set_target fills it per walk; case-
             * sensitive, matching HTML id semantics). No fragment in
             * the URL = nothing is targeted. */
            if (!g_cssTarget[0] || !idAttr ||
                strcmp(idAttr, g_cssTarget) != 0)
            {
                return 0;
            }
        }
        else if (c->pclass == 14)
        {
            /* :root — the document's root element. */
            if (!tagLower || !css_eq_ncase(tagLower, "html", 4))
            {
                return 0;
            }
        }
        else if (c->pclass == 15)
        {
            /* :empty — no content children (element-less AND text-less;
             * comment-only subtrees stay empty per spec; -1 unknown =
             * never match, graceful). */
            if (!dom || !dom->childless || dom->childless(node) != 1)
            {
                return 0;
            }
        }
    }
    else if (c->pclass)
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
        else if (c->pclass == 1)
        {
            /* :first-child = no previous ELEMENT sibling. One hook call
             * with early exit — NOT a full backward count. The old code
             * walked to the start (O(position) hook calls, each O(siblings)
             * internally = O(n^3) per sibling list), which stalled the
             * 10s run-loop watchdog on list-heavy pages (Wikipedia main).
             * dom->sibling skips non-elements, so existence ⟺ idx != 1. */
            if (dom->sibling(node, -1) != NULL)
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
            /* :only-child = neither direction has an element sibling. */
            if (dom->sibling(node, -1) != NULL ||
                dom->sibling(node, 1) != NULL)
            {
                return 0;
            }
        }
        else
        {
            /* :nth-child and anything future: full 1-based element
             * position (the only branch that needs the count). */
            int idx = 1;
            const void *s = node;
            while ((s = dom->sibling(s, -1)) != NULL)
            {
                idx++;
            }
            if (c->pclass == 5)
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
            /* Unknown pclass values: no constraint (match), exactly as
             * before — only 1,2,3,4,5 reach here (8 matches above; the
             * Phase 7 truth-family 9..15 dispatches above too). */
        }
    }
    if (c->notKind)
    {
        /* :not(<simple>) — inverted single-token test, same compare
         * semantics as the compound items above. Phase 1 keeps the
         * CSS3 shape: ONE simple selector inside the parens. */
        int hit = 0;
        if (c->notKind == 1)
        {
            size_t n = tagLower ? strlen(tagLower) : 0;
            if (n == c->notLen &&
                css_eq_ncase(tagLower, c->notTok, c->notLen))
            {
                hit = 1;
            }
        }
        else if (c->notKind == 2)
        {
            /* whole-token class compare (case-sensitive), inverted */
            if (classAttr)
            {
                const char *w = classAttr;
                while (*w)
                {
                    const char *t = w;
                    while (*t && !css_isspace(*t))
                    {
                        t++;
                    }
                    if ((size_t)(t - w) == c->notLen &&
                        memcmp(w, c->notTok, c->notLen) == 0)
                    {
                        hit = 1;
                        break;
                    }
                    if (!*t)
                    {
                        break;
                    }
                    w = t + 1;
                }
            }
        }
        else if (c->notKind == 3)
        {
            if (idAttr && strlen(idAttr) == c->notLen &&
                memcmp(idAttr, c->notTok, c->notLen) == 0)
            {
                hit = 1;
            }
        }
        else /* notKind == 4: [attr] presence */
        {
            if (dom && dom->attr && node &&
                dom->attr(node, c->notTok, c->notLen))
            {
                hit = 1;
            }
        }
        if (hit)
        {
            return 0; /* the node DOES satisfy the :not argument */
        }
    }
    if (!c->typeLen && !c->clsLen && !c->idLen && !c->universal &&
        !c->attrLen && !c->pclass && !c->notKind)
    {
        return 0;
    }
    return 1;
}

/* Phase 1: bounded right-to-left chain search for chains containing a
 * sibling combinator ('+' / '~'). Compound `want` must match at `node`;
 * candidates for `want-1` follow combs[want]: 0 = any ancestor, 1 = the
 * parent only, 2 = the immediately previous ELEMENT sibling only,
 * 3 = any previous ELEMENT sibling. Mixed chains need real backtracking
 * ("h2 + p span": the p may sit at any ancestor depth, and only one of
 * those ancestors has an h2 before it), so this is depth-first over the
 * (tiny) selector DAG. *budget counts compound tests; exhausting it
 * returns "no match" — bounded, never misapplied, never unbounded work. */
static int css_chain_rest(const CssSimple *parts, const unsigned char *combs,
                          int want, const void *node, const char *tagLower,
                          const char *classAttr, const char *idAttr,
                          const CssDom *dom, unsigned *budget)
{
    if (parts == NULL || dom == NULL || node == NULL || *budget == 0)
    {
        return 0; /* over budget: treated as unmatched */
    }
    (*budget)--;
    if (!css_compound_matches(&parts[want], tagLower, classAttr, idAttr,
                              dom, node))
    {
        return 0;
    }
    if (want == 0)
    {
        return 1; /* whole chain consumed */
    }
    unsigned char comb = combs[want]; /* relation between want and want-1 */
    if (comb == 2 || comb == 3)
    {
        /* Adjacent (2): exactly the previous ELEMENT sibling.
         * General sibling (3): walk backwards until something fits. */
        if (!dom->siblingInfo)
        {
            return 0; /* graceful: hook missing, selector never matches */
        }
        const char *stg = NULL;
        const char *scl = NULL;
        const char *sid = NULL;
        const void *s = dom->siblingInfo(node, -1, &stg, &scl, &sid);
        if (comb == 2)
        {
            if (!s)
            {
                return 0;
            }
            return css_chain_rest(parts, combs, want - 1, s, stg, scl, sid,
                                  dom, budget);
        }
        while (s)
        {
            if (css_chain_rest(parts, combs, want - 1, s, stg, scl, sid,
                               dom, budget))
            {
                return 1;
            }
            if (*budget == 0)
            {
                return 0;
            }
            s = dom->siblingInfo(s, -1, &stg, &scl, &sid);
        }
        return 0;
    }
    /* comb 0 (descendant) or 1 (child): walk the ancestor hook. Child
     * only gets one candidate (the parent); descendant tries each. */
    if (!dom->ancestor)
    {
        return 0;
    }
    for (int ai = 0;; ai++)
    {
        const char *atg = NULL;
        const char *acl = NULL;
        const char *aid = NULL;
        const void *a = dom->ancestor(node, ai, &atg, &acl, &aid);
        if (!a)
        {
            return 0; /* ran out of ancestors */
        }
        if (css_chain_rest(parts, combs, want - 1, a, atg, acl, aid, dom,
                           budget))
        {
            return 1;
        }
        if (*budget == 0)
        {
            return 0;
        }
    }
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
    int hasSib = 0;
    for (int i = 1; i < np; i++)
    {
        if (combs[i] == 2 || combs[i] == 3)
        {
            hasSib = 1;
            break;
        }
    }
    if (!hasSib)
    {
        /* desc28: descendant/child-only chains keep the proven linear
         * ancestor walk (subject-relative indexing: R57 cache path).
         * Sibling combinators jump the walk off the ancestor line, so
         * those chains take the bounded search instead. Absolute
         * ancestor indexing keeps mixed descent honest (div > p span:
         * the span's p may be at any depth, but div must be p's
         * PARENT). comb[i] is the combinator BEFORE compound i:
         * 1 = child means only the immediate next ancestor may satisfy
         * compound i. */
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
    /* Phase 1: chain contains '+' or '~' — bounded backtracking search.
     * Sibling runs are typically shallow; the budget caps any mix. */
    if (!dom || !node)
    {
        return 0;
    }
    unsigned budget = 256;
    return css_chain_rest(parts, combs, np - 1, node, tagLower, classAttr,
                          idAttr, dom, &budget);
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
            if (np == 0 || needComb)
            {
                return 0;
            }
            comb = (*cs == '+') ? 2 : 3;
            needComb = 1;
            cs++;
            continue;
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

/* Phase 9: gradient stop average → a 0..255 luminance, or -1 when no
 * stop resolves. The 1-bit page has no raster to grade, but a gradient
 * BACKGROUND still decides the reader's ink polarity: `color:#000` on
 * a dark gradient hero is browser-invisible, so the average luminance
 * feeds the same invert machinery a solid background-color does. Only
 * resolves-able stops count (#hex / rgb()/rgba() via rgb_lum / the
 * named-gray table via css_color_luminance); angles, percentage
 * positions, var()/currentColor/transparent stops sit out (currentColor
 * = the local ink, not a background; an unresolved stop that FAILS its
 * parse is skipped but the AVERAGE over remaining stops stays usable —
 * the CSS spec itself drops invalid stops). linear/radial/conic share
 * one grammar for this purpose: the stop list is position-independent. */
int css_gradient_luma(const char *val, size_t vlen)
{
    if (!val || vlen == 0)
    {
        return -1;
    }
    const char *z = val + vlen;
    const char *p = val;
    const char *gs = NULL;
    for (; p + 9 <= z; p++)
    {
        if (((p[0] | 32) == 'g') && (p + 9 <= z) &&
            (p[1] | 32) == 'r' && (p[2] | 32) == 'a' &&
            (p[3] | 32) == 'd' && (p[4] | 32) == 'i' &&
            (p[5] | 32) == 'e' && (p[6] | 32) == 'n' &&
            (p[7] | 32) == 't' && p[8] == '(')
        {
            gs = p + 9;
            break;
        }
    }
    if (!gs)
    {
        return -1;
    }
    /* The first '(' opens the fn; nested fns decode their own ')' —
     * simple depth walk (the gradient body is the OUTERMOST span). */
    int depth = 1;
    const char *ge = gs;
    while (ge < z && depth > 0)
    {
        if (*ge == '(')
        {
            depth++;
        }
        else if (*ge == ')')
        {
            depth--;
        }
        ge++;
    }
    if (ge > z)
    {
        ge = z;
    }
    /* Comma-split the argument list; every comma-LEVEL stop is the
     * top-level span (a stop may itself hold rgb(a,b,c) — its commas
     * live inside parens). Track the paren depth during the split. */
    long long sum = 0;
    int count = 0;
    const char *s = gs;
    int pd = 0;
    for (const char *q = gs; q <= ge; q++)
    {
        if (q == ge || (*q == ',' && pd == 0))
        {
            if (count < 64)
            {
                int l = css_color_luminance(s, (size_t)(q - s));
                if (l >= 0)
                {
                    sum += l;
                    count++;
                }
            }
            s = q + 1;
        }
        else if (*q == '(')
        {
            pd++;
        }
        else if (*q == ')')
        {
            pd--;
        }
    }
    if (count == 0)
    {
        return -1;
    }
    return (int)(sum / count);
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

/* ── Phase 2b: var() substitution + calc() evaluation ──────────────────────────── */
/* Resolution layer for values containing var(/calc(. It runs at LOOKUP
 * time (winning_core exit + apply_prop consume) so untainted values keep
 * their original sheet-text pointers byte-for-byte — parity guarantee:
 * pages without var/calc resolve NOTHING, not even a string copy.
 *
 * Shapes:
 *   var  = var(--name[, fallback]) anywhere in the value; one level — a
 *          fallback may hold calc() but not another var() (nesting
 *          refuses: the substituted text keeps the raw remainder and the
 *          consumer rejects it, same as pre-Phase-2).
 *   calc = calc(<expr>) covering the WHOLE (post-substitution) value;
 *          px / % / unitless-number arithmetic at milli-unit fixed
 *          point. Other units and mixed-unit +/- refuse (raw flows).
 *          The result is canonical "Npx"/"N%" text the attr-format
 *          consumers already eat.
 * Scratch: a fixed 4-slot ring (72B each) — a lookup site may
 * co-resolve up to 4 values (the observed max is the paired wv/sv,
 * dv/fv lookups). Overflow never truncates: any substitution that does
 * not fit aborts the whole resolution (the raw value flows). */
#define CSS_RES_SLOTS 4
#define CSS_RES_MAX 72
static char g_cssRes[CSS_RES_SLOTS][CSS_RES_MAX];
static unsigned char g_cssResCursor = 0;

static char *css_res_slot(void)
{
    char *dst = g_cssRes[g_cssResCursor];
    g_cssResCursor = (unsigned char)((g_cssResCursor + 1) % CSS_RES_SLOTS);
    return dst;
}

/* Bounded case-insensitive search for a literal needle (lowercase). */
static const char *css_ifind(const char *p, const char *e, const char *lit)
{
    size_t n = strlen(lit);
    if (n == 0 || n > (size_t)(e - p))
    {
        return NULL;
    }
    for (const char *s = p; s + (ptrdiff_t)n <= e; s++)
    {
        size_t i = 0;
        while (i < n && css_low(s[i]) == lit[i])
        {
            i++;
        }
        if (i == n)
        {
            return s;
        }
    }
    return NULL;
}

static int val_needs_resolve(const char *v, size_t n)
{
    return css_value_has_ci(v, n, "var(") || css_value_has_ci(v, n, "calc(");
}

/* Calc: milli-unit fixed point (scale 1000). unit 0 = unitless number,
 * 1 = px, 2 = %. Magnitudes clamp at ±9999 whole units (envelope). */
typedef struct
{
    long long mm;
    int unit;
} CssNum;

#define CSS_MSCALE 1000LL

static int css_calc_add(CssNum a, char op, CssNum b, CssNum *out)
{
    /* +/-: same unit, or exactly one side unitless (it adopts the
     * other's unit — a permissive superset of the CSS rule). */
    if (a.unit == 0)
    {
        a.unit = b.unit;
    }
    else if (b.unit == 0)
    {
        b.unit = a.unit;
    }
    if (a.unit != b.unit)
    {
        return 0;
    }
    out->mm = (op == '+') ? (a.mm + b.mm) : (a.mm - b.mm);
    out->unit = a.unit;
    return 1;
}

/* Grammar (bounded depth 8; parentheses grow depth, so it terminates):
 *   expr   := term  { (+ | -) term }
 *   term   := factor { (* | /) factor }
 *   factor := number[px|%] | ( expr ) | (+|-) factor
 */
static int css_calc_expr(const char *p, const char *e, int depth, CssNum *out,
                         const char **endOut);

static int css_calc_factor(const char *p, const char *e, int depth,
                           CssNum *out, const char **endOut)
{
    const char *q = skip_ws(p, e);
    if (q >= e)
    {
        return 0;
    }
    if (*q == '(')
    {
        CssNum v;
        q++;
        if (!css_calc_expr(q, e, depth + 1, &v, &q))
        {
            return 0;
        }
        q = skip_ws(q, e);
        if (q >= e || *q != ')')
        {
            return 0;
        }
        out->mm = v.mm;
        out->unit = v.unit;
        *endOut = q + 1;
        return 1;
    }
    if (*q == '-' || *q == '+')
    {
        int sign = (*q == '-') ? -1 : 1;
        CssNum v;
        if (!css_calc_factor(q + 1, e, depth, &v, &q))
        {
            return 0;
        }
        out->mm = v.mm * sign;
        out->unit = v.unit;
        *endOut = q;
        return 1;
    }
    long long whole = 0;
    long long frac = 0;
    int fd = 0;
    int any = 0;
    while (q < e && *q >= '0' && *q <= '9')
    {
        whole = whole * 10 + (*q - '0');
        if (whole > 9999)
        {
            return 0;
        }
        q++;
        any = 1;
    }
    if (q < e && *q == '.')
    {
        const char *dot = q;
        q++;
        while (q < e && *q >= '0' && *q <= '9' && fd < 3)
        {
            frac = frac * 10 + (*q - '0');
            fd++;
            q++;
            any = 1;
        }
        if (fd == 0)
        {
            q = dot; /* lone dot: backtrack to the digit run */
        }
    }
    if (!any)
    {
        return 0;
    }
    static const long long kFracMul[4] = {1, 100, 10, 1};
    long long mm = whole * CSS_MSCALE + frac * kFracMul[fd];
    int unit = 0;
    if (q < e && *q == '%')
    {
        unit = 2;
        q++;
    }
    else if (e - q >= 2 && css_low(q[0]) == 'p' && css_low(q[1]) == 'x')
    {
        unit = 1;
        q += 2;
    }
    else if (q < e && !css_isspace(*q) && *q != ')' && *q != '+' &&
             *q != '-' && *q != '*' && *q != '/')
    {
        return 0; /* unsupported unit (em, rem, vw, …) */
    }
    out->mm = mm;
    out->unit = unit;
    *endOut = q;
    return 1;
}

static int css_calc_term(const char *p, const char *e, int depth, CssNum *out,
                         const char **endOut)
{
    CssNum a;
    const char *q = p;
    if (!css_calc_factor(q, e, depth, &a, &q))
    {
        return 0;
    }
    while (q < e)
    {
        const char *op = skip_ws(q, e);
        if (op >= e || (*op != '*' && *op != '/'))
        {
            break;
        }
        char o = *op;
        CssNum b;
        const char *q2 = NULL;
        if (!css_calc_factor(op + 1, e, depth, &b, &q2))
        {
            return 0;
        }
        if (o == '*')
        {
            /* one side must be unitless */
            if (a.unit == 0 && b.unit != 0)
            {
                a.mm = (a.mm * b.mm) / CSS_MSCALE;
                a.unit = b.unit;
            }
            else if (b.unit == 0)
            {
                a.mm = (a.mm * b.mm) / CSS_MSCALE;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            /* '/': divisor must be unitless and non-zero */
            if (b.unit != 0 || b.mm == 0)
            {
                return 0;
            }
            a.mm = (a.mm * CSS_MSCALE) / b.mm;
        }
        q = q2;
    }
    *out = a;
    *endOut = q;
    return 1;
}

static int css_calc_expr(const char *p, const char *e, int depth, CssNum *out,
                         const char **endOut)
{
    if (depth > 8)
    {
        return 0;
    }
    CssNum a;
    const char *q = p;
    if (!css_calc_term(q, e, depth, &a, &q))
    {
        return 0;
    }
    while (q < e)
    {
        const char *op = skip_ws(q, e);
        if (op >= e || (*op != '+' && *op != '-'))
        {
            break;
        }
        char o = *op;
        CssNum b;
        const char *q2 = NULL;
        if (!css_calc_term(op + 1, e, depth, &b, &q2))
        {
            return 0;
        }
        if (!css_calc_add(a, o, b, &a))
        {
            return 0;
        }
        q = q2;
    }
    *out = a;
    *endOut = q;
    return 1;
}

/* Evaluate ONE whole-value calc() over [s,e): the calc( ... ) span must
 * cover the whole value (prefix/suffix ws allowed), else it is a
 * composite and refuses. Ring slot gets "Npx"/"N%" (rounded
 * half-away-from-zero). NULL on any refuse. */
static char *css_calc_text(const char *s, const char *e)
{
    const char *hit = css_ifind(s, e, "calc(");
    if (!hit)
    {
        return NULL;
    }
    const char *open = s;
    while (open < hit && css_isspace(*open))
    {
        open++;
    }
    if (open != hit)
    {
        return NULL; /* non-ws before calc( = composite value */
    }
    const char *cp = hit + 5;
    int depth = 1;
    const char *close = cp;
    while (close < e && depth)
    {
        if (*close == '(')
        {
            depth++;
        }
        else if (*close == ')')
        {
            depth--;
        }
        close++;
    }
    if (depth || close <= cp)
    {
        return NULL;
    }
    close--; /* at the matching ')' */
    const char *tail = close + 1;
    while (tail < e && css_isspace(*tail))
    {
        tail++;
    }
    if (tail != e)
    {
        return NULL; /* trailing tokens = composite value */
    }
    CssNum v;
    const char *endp = NULL;
    if (!css_calc_expr(cp, close, 1, &v, &endp))
    {
        return NULL;
    }
    endp = skip_ws(endp, close);
    if (endp != close)
    {
        return NULL; /* the expression did not consume its input */
    }
    if (v.unit == 0)
    {
        return NULL; /* a bare number is not a length the channels carry */
    }
    char *dst = css_res_slot();
    long long r = (v.mm < 0) ? (v.mm - CSS_MSCALE / 2) / CSS_MSCALE
                             : (v.mm + CSS_MSCALE / 2) / CSS_MSCALE;
    if (v.unit == 2)
    {
        snprintf(dst, CSS_RES_MAX, "%lld%%", r);
    }
    else
    {
        snprintf(dst, CSS_RES_MAX, "%lldpx", r);
    }
    return dst;
}

static const char *css_resolve_value(const CssEngine *eng, const char *val,
                                     size_t vlen, size_t *outLen)
{
    const char *ze = val + vlen;
    if (!eng || !val || vlen == 0 || !val_needs_resolve(val, vlen))
    {
        return NULL;
    }
    /* Pass 1: var(-substitution into a ring slot. Source bytes copy
     * verbatim; every var( site swaps in its definition (exact-name,
     * case-sensitive like CSS) or the fallback text; neither = the whole
     * resolution refuses (the raw value keeps flowing). */
    char *dst = css_res_slot();
    size_t off = 0;
    const char *p = val;
    while (p < ze)
    {
        const char *hit = css_ifind(p, ze, "var(");
        if (!hit)
        {
            if (off + (size_t)(ze - p) >= CSS_RES_MAX)
            {
                return NULL;
            }
            memcpy(dst + off, p, (size_t)(ze - p));
            off += (size_t)(ze - p);
            break;
        }
        if (off + (size_t)(hit - p) >= CSS_RES_MAX)
        {
            return NULL;
        }
        memcpy(dst + off, p, (size_t)(hit - p));
        off += (size_t)(hit - p);
        /* var( args: balanced-paren scan (a fallback's parens count) */
        const char *ap = hit + 4;
        int depth = 1;
        const char *close = ap;
        while (close < ze && depth)
        {
            if (*close == '(')
            {
                depth++;
            }
            else if (*close == ')')
            {
                depth--;
            }
            close++;
        }
        if (depth)
        {
            return NULL;
        }
        close--;
        const char *ns = skip_ws(ap, close);
        const char *src = NULL;
        size_t srcLen = 0;
        if (close - ns >= 2 && ns[0] == '-' && ns[1] == '-')
        {
            const char *ne = ns + 2;
            while (ne < close && !css_isspace(*ne) && *ne != ',')
            {
                ne++;
            }
            size_t nn = (size_t)(ne - ns) - 2;
            const char *comma = NULL;
            for (const char *q = ne; q < close; q++)
            {
                if (*q == ',')
                {
                    comma = q;
                    break;
                }
            }
            const char *fs = comma ? skip_ws(comma + 1, close) : NULL;
            const char *fe = comma ? close : NULL;
            if (fe)
            {
                while (fe > fs && css_isspace(fe[-1]))
                {
                    fe--;
                }
            }
            if (nn > 0 && nn < CSS_CUSTOM_NAME)
            {
                for (int i = 0; i < eng->customCount && !src; i++)
                {
                    if (strlen(eng->custom[i].name) == nn &&
                        memcmp(eng->custom[i].name, ns + 2, nn) == 0)
                    {
                        src = eng->custom[i].val;
                        srcLen = eng->custom[i].valLen;
                    }
                }
            }
            if (!src && fs && fe > fs)
            {
                src = fs;
                srcLen = (size_t)(fe - fs);
            }
            if (!src || off + srcLen >= CSS_RES_MAX)
            {
                return NULL;
            }
            memcpy(dst + off, src, srcLen);
            off += srcLen;
            p = close + 1;
        }
        else
        {
            return NULL; /* var( without a --name is unusable */
        }
    }
    dst[off] = '\0';
    if (outLen)
    {
        *outLen = off;
    }
    /* Pass 2: whole-value calc over the substituted text (a calc with no
     * var landings here on iteration one through the same tail copy). */
    if (css_ifind(dst, dst + off, "calc("))
    {
        char *rt = css_calc_text(dst, dst + off);
        if (rt)
        {
            if (outLen)
            {
                *outLen = strlen(rt);
            }
            return rt;
        }
    }
    return dst;
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

    /* Mask consult: a previous lookup for this (element, pseudo-lane)
     * already tested every rule. Hit: only matching rules reach the
     * cascade below, with their stored winning specificity (no chains). */
    uint64_t mlo = 0, mhi = 0;
    const unsigned short *storedSpec = NULL;
    unsigned short fillSpec[CSS_MAX_RULES];
    int useMask = css_mask_fetch(node, eng, (unsigned char)wantPseudo,
                                 &mlo, &mhi, &storedSpec);
    if (!useMask)
    {
        memset(fillSpec, 0, sizeof(fillSpec));
    }

    for (int ri = 0; ri < eng->ruleCount; ri++)
    {
        const CssRule *r = &eng->rules[ri];
        unsigned spec = 0;
        if (useMask)
        {
            int bit = 0;
            if (ri < 64)
            {
                bit = (int)((mlo >> ri) & 1ULL);
            }
            else if (ri < 128)
            {
                bit = (int)((mhi >> (ri - 64)) & 1ULL);
            }
            if (!bit)
            {
                continue;
            }
            /* Winning specificity comes from storage (computed with the
             * same max-over-matching rule during fill). */
            if (ri < CSS_MAX_RULES)
            {
                spec = storedSpec[ri];
            }
            if (r->pseudo != wantPseudo)
            {
                continue;
            }
        }
        else
        {
            if (r->pseudo != wantPseudo)
            {
                continue;
            }
            /* Matching selector's specificity (max over MATCHING
             * selectors — a non-matching group member must not inflate
             * the win). */
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
            /* Record the outcome for later lookups of this element. */
            if (ri < 64)
            {
                mlo |= 1ULL << ri;
            }
            else if (ri < 128)
            {
                mhi |= 1ULL << (ri - 64);
            }
            if (ri < CSS_MAX_RULES)
            {
                fillSpec[ri] = (unsigned short)(spec > 0xFFFF ? 0xFFFF
                                                              : spec);
            }
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
    if (!useMask)
    {
        css_mask_store(node, eng, (unsigned char)wantPseudo, mlo, mhi,
                       fillSpec);
    }
    if (have && best && val_needs_resolve(best, blen))
    {
        /* Phase 2: var/calc values resolve at the exit — the ring copy
         * lives until a later lookup reuses it; every consumer either
         * copies into the doc arena or compares immediately. */
        size_t rl = blen;
        const char *rv = css_resolve_value(eng, best, blen, &rl);
        if (rv && rl > 0)
        {
            best = rv;
            blen = rl;
        }
        /* refuse (NULL) keeps the raw sheet-text value — same visible
         * behavior as pre-Phase-2. */
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

/* Phase 8: standalone evaluator for link/source media= screening — the
 * SAME query language as the sheet path (a 1-bit display honors
 * screen/all; print fails; unknown features evaluate false). qs/qe
 * delimit the query text (no NUL required). 0 = does not hold. */
int css_media_holds(const char *qs, const char *qe)
{
    if (!qs)
    {
        return 0;
    }
    return media_holds(qs, qe ? qe : qs + strlen(qs));
}

const char *css_pseudo_content(const CssEngine *eng, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node,
                               int lane, size_t *vlenOut)
{
    /* lane passes through to the rule-pseudo equality (1 before,
     * 2 after, 4 marker). */
    return winning_core(eng, tagLower, classAttr, idAttr, dom, node,
                        "content", lane, vlenOut);
}

void css_set_target(CssEngine *eng, const char *id, size_t len)
{
    if (!eng)
    {
        return;
    }
    if (!id || len >= sizeof(eng->target))
    {
        eng->target[0] = '\0';
        g_cssTarget[0] = '\0';
        return;
    }
    memcpy(eng->target, id, len);
    eng->target[len] = '\0';
    /* The matcher reads the walk-current fragment (single-threaded
     * cooperative pump — one render walks at a time; the eng->target
     * copy exists for API symmetry/debug). */
    size_t cl = len < sizeof(g_cssTarget) - 1 ? len
                                              : sizeof(g_cssTarget) - 1;
    memcpy(g_cssTarget, id, cl);
    g_cssTarget[cl] = '\0';
}

/* ── 4. property application + cascade ───────────────────────────────────── */
/* Phase 6: the flex/grid DISTRIBUTION family — the names the inspector
 * arm consumes (no 1-bit behavior; the column axis is the block flow
 * itself. css.h policy). Matches the kKnown block above exactly. */
static int css_flexgrid_name(const char *n, size_t len)
{
    static const struct
    {
        const char *n;
        unsigned char len;
    } kFg[] = {
        {"flex-direction", 14},  {"flex-flow", 9},
        {"flex-wrap", 9},        {"flex", 4},
        {"flex-grow", 9},        {"flex-shrink", 11},
        {"flex-basis", 10},      {"order", 5},
        {"justify-content", 15}, {"align-content", 13},
        {"place-items", 11},     {"place-content", 13},
        {"grid-template-columns", 21}, {"grid-template-rows", 18},
        {"grid-template-areas", 19},   {"grid-auto-columns", 17},
        {"grid-auto-rows", 14},  {"grid-auto-flow", 14},
        {"grid-column", 11},     {"grid-row", 8},
        {"grid-area", 9},
    };
    for (size_t i = 0; i < sizeof(kFg) / sizeof(kFg[0]); i++)
    {
        if (len == kFg[i].len && memcmp(n, kFg[i].n, len) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void apply_prop(CssEngine *eng, unsigned *out, const char *prop,
                       size_t plen, const char *val, size_t vlen)
{
    char buf[48];
    if (val && val_needs_resolve(val, vlen))
    {
        /* Phase 2: resolve var/calc before the flag match (display:
         * var(--hd) lands exactly like display: none). A refuse keeps
         * the raw value — no flag matches, as before. */
        size_t rl = vlen;
        const char *rv = css_resolve_value(eng, val, vlen, &rl);
        if (rv && rl > 0)
        {
            val = rv;
            vlen = rl;
        }
    }
    size_t n = vlen < sizeof(buf) - 1 ? vlen : sizeof(buf) - 1;
    memcpy(buf, val, n);
    buf[n] = '\0';

    if (plen == 7 && memcmp(prop, "display", 7) == 0)
    {
        if (strstr(buf, "none"))
        {
            *out |= CSS_F_HIDDEN;
        }
        else if (strstr(buf, "flex") || strstr(buf, "grid"))
        {
            /* Phase 6: the flex/grid container lane (inline variants
             * contain "flex"/"grid" as substrings). A flex/grid box
             * is a block-level container in the 1-bit flow; the bit
             * gates the gap transport + align-items carry (css.h
             * policy). display: contents sets NO bit — the walker's
             * box-switch path flattens it on its own lane. */
            *out |= CSS_F_FLEXCT;
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
    else if (plen == 8 && memcmp(prop, "position", 8) == 0)
    {
        /* Phase 4 positioning vocabulary: which values are OUT OF FLOW.
         * static/relative/sticky keep flowing — sticky chrome scrolls
         * away instead of eating the 240px viewport. */
        if (strstr(buf, "fixed") || strstr(buf, "absolute"))
        {
            *out |= CSS_F_OOF;
        }
    }
    else if ((plen == 3 || plen == 4 || plen == 5 || plen == 6) &&
             (memcmp(prop, "top", 3) == 0 || memcmp(prop, "left", 4) == 0 ||
              memcmp(prop, "right", 5) == 0 ||
              memcmp(prop, "bottom", 6) == 0))
    {
        /* Offset SIGNAL: any non-auto value participates (0px IS an
         * offset; auto = the static position per CSS, no signal).
         * inherit/initial/unset cannot resolve here — no signal. */
        if (!strstr(buf, "auto") && !strstr(buf, "inherit") &&
            !strstr(buf, "initial") && !strstr(buf, "unset"))
        {
            *out |= CSS_F_OFF;
        }
    }
    else if (plen == 7 && memcmp(prop, "z-index", 7) == 0)
    {
        /* z-index with an INTEGER value (an invalid declaration is
         * ignored per CSS — garbage must not carry the signal). */
        for (const char *q = buf; *q; q++)
        {
            if (*q >= '0' && *q <= '9')
            {
                *out |= CSS_F_OZ;
                break;
            }
        }
    }
    else if (plen == 13 && memcmp(prop, "text-overflow", 13) == 0)
    {
        /* Phase 5: the author's EXPLICIT want-truncate declaration.
         * ellipsis = cut the overflowing single line and append "..."
         * (rides the DocInline ABI into the breaker). Value "clip"
         * (cut WITHOUT dots) stays census — rare, and inventing a
         * second ABI bit for it buys little; documented residual. */
        if (strstr(buf, "ellipsis"))
        {
            *out |= CSS_F_ELLIP;
        }
    }
    else if (plen == 8 && memcmp(prop, "overflow", 8) == 0)
    {
        /* Phase 5 overflow values: auto/scroll/visible are inspected
         * no-ops (the flat page IS the sole scroll region — regions
         * cannot exist). ONLY hidden arms the sr-only recipe detector
         * (one leg of the walker's visually-hidden conjunction). */
        if (strstr(buf, "hidden"))
        {
            *out |= CSS_F_OVH;
        }
    }
    else if (plen == 9 && memcmp(prop, "clip-path", 9) == 0)
    {
        /* Concealing shapes only: inset(D...) where D != 0 — the
         * sr-only recipe's preferrable form; a literal inset(0) keeps
         * everything visible (no drop). circle/ellipse/polygon/mask
         * are generic paint vocabulary (not assertion territory). */
        const char *in = strstr(buf, "inset");
        if (in)
        {
            for (const char *q = in; *q; q++)
            {
                if (*q >= '1' && *q <= '9')
                {
                    *out |= CSS_F_CLIPN;
                    break;
                }
            }
        }
    }
    else if (plen == 4 && memcmp(prop, "clip", 4) == 0)
    {
        /* Legacy clip: rect(...) — the W3C visually-hidden recipe's
         * original form. ANY rect shape is a concealing draw (rect
         * exists for this recipe; a fuller rect is invisible anyway). */
        if (strstr(buf, "rect"))
        {
            *out |= CSS_F_CLIPN;
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
    else if (plen == 16 && memcmp(prop, "background-color", 16) == 0)
    {
        /* Phase 9: a SHEET-side background-color decides polarity the
         * same way the inline route (doc_is_inverted_style /
         * walk_inverted) has always decided it — through the
         * established luminance scale: any resolvable dark color
         * (< 130 of 255) inverts; a gradient background goes through
         * the stop average. A light background answers nothing (the
         * default is transparent on a light page; brighter-than-text
         * polarity lives in the color arms only). */
        if (strstr(buf, "gradient("))
        {
            int glum = css_gradient_luma(buf, strlen(buf));
            if (glum >= 0 && glum < 130)
            {
                *out |= CSS_F_INVERT;
            }
        }
        else
        {
            int blum = css_color_luminance(buf, strlen(buf));
            if (blum >= 0 && blum < 130)
            {
                *out |= CSS_F_INVERT;
            }
        }
    }
    else if (plen == 10 && memcmp(prop, "align-self", 10) == 0)
    {
        /* Phase 6: element-scoped cross-axis centering — carried as
         * CSS_F_ASC at this element's CSS-exit (document.c), stripped
         * at the boundary exactly like the non-inherited ELLIP bit. */
        if (strstr(buf, "center"))
        {
            *out |= CSS_F_ASC;
        }
    }
    else if (plen == 11 && memcmp(prop, "align-items", 11) == 0)
    {
        /* Phase 6: container cross-axis centering — CSS_F_AIC propagates
         * to the container's child blocks ONLY on a flex/grid container
         * (the walker gates the carry on CSS_F_FLEXCT; on a block box
         * the declaration stays inert, which is the CSS behavior). */
        if (strstr(buf, "center"))
        {
            *out |= CSS_F_AIC;
        }
    }
    else if (css_flexgrid_name(prop, plen))
    {
        /* Phase 6: the flex/grid DISTRIBUTION family (direction, wrap,
         * order, grow/shrink/basis, justify-*, place-*, grid-*).
         * Inspected-but-inert: the flat page's only honest flex/grid
         * mapping is the column axis, which the block flow already IS
         * — content order preserved, items stacked (css.h policy). The
         * match here is what consumes the names (census shows them
         * out of the unknown gauge); the values would need a real 2-D
         * compositor for anything more. align-items has its own arm
         * above this (center route); the gap trio rides the walker's
         * winning-value transport (flex_gap_px), not this ledger. */
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
    /* Mask fill: record this element's rule outcomes for later
     * css_winning_value lookups (pseudo-0 lane). Specificity is max over
     * MATCHING selectors (winning_core semantics — a non-matching group
     * member must not inflate the win; this also fixes the old max-over-
     * all behavior for mixed-specificity groups). */
    uint64_t cmlo = 0, cmhi = 0;
    unsigned short cspec[CSS_MAX_RULES];
    memset(cspec, 0, sizeof(cspec));
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
        if (r->pseudo != 0)
        {
            continue;
        }
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
        if (ri < 64)
        {
            cmlo |= 1ULL << ri;
        }
        else if (ri < 128)
        {
            cmhi |= 1ULL << (ri - 64);
        }
        if (ri < CSS_MAX_RULES)
        {
            cspec[ri] = (unsigned short)(spec > 0xFFFF ? 0xFFFF : spec);
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
    css_mask_store(node, eng, 0, cmlo, cmhi, cspec);
    for (unsigned char bi = 0; bi < bestCount; bi++)
    {
        if (bestVal[bi])
        {
            /* Exact lengths, NOT strlen: names and values point into the
             * sheet buffer with no terminators — strlen once false-
             * positived display:block on a sheet that mentioned "none"
             * 30 bytes later, and name lengths truncate values. */
            apply_prop(eng, &out, bestProp[bi], bestLen[bi], bestVal[bi],
                       bestVlen[bi]);
        }
    }
    return out;
}
