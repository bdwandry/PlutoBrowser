/*
 * PlutoBrowser — css.h
 * Minimal general-purpose CSS engine (roadmap item #2).
 *
 * Scope (deliberately small, device-sized — see MASTER_TODO §0 item 2):
 *   - <style> block extraction from raw HTML (same scanner family as
 *     jsbridge_scan_scripts; the tokenizer skips <style> content and the
 *     DOM builder drops <head>, so the DOM never contains stylesheet text).
 *   - Rule parsing: "sel1, sel2 { prop: value; ... }" with comments and
 *     junk declarations skipped; a bad rule never aborts the sheet.
 *   - Selectors: type, .class, #id, universal (*), [attr]/[attr=value],
 *     :first-child/:last-child/:only-child/:link/:nth-child(odd/even/N/
 *     An+B), descendant and child (>) combinators, ::before/::after
 *     (content cascade only — never element matches), and comma-grouped
 *     lists. Anything else ([attr~=...] operators, other pseudo-classes,
 *     sibling combinators) marks that selector unusable; if no selector
 *     in the group survives, the rule is skipped — the browser behavior
 *     for unparseable selectors.
 *   - @media screening: screen/print/all apply (a 1-bit display honors
 *     print stylesheets — they are built for black-and-white); speech
 *     and unknown types do not. (max|min)-width/height in px run against
 *     the 400x240 viewport; orientation (landscape), monochrome (1),
 *     color (0 bits), prefers-color-scheme (light) answered honestly.
 *     Unknown features fail their query; nested @media ANDs. Other
 *     at-rules (@import/@font-face/…) skip whole.
 *   - UA origin: [hidden] elements are display:none with UA priority — any
 *     author display declaration (any specificity) re-shows them.
 *   - Properties mapped onto the 1-bit render model:
 *       display:none / visibility:hidden → element+subtree suppressed
 *       text-align:center|right          → block align
 *       font-weight (bold/bolder/≥600)   → bold inline flag
 *       font-style italic/oblique        → italic inline flag
 *       text-decoration underline/line-through → flags
 *       text-transform/capitalize        → case flags (walker-applied)
 *       white-space pre/pre-line/nowrap  → spacing flags (walker+layout)
 *       overflow-wrap:break-word / word-break:break-all → mid-word split
 *       list-style-type                  → marker codes (walker+layout)
 *       border / border-width / border-style → block outline (layout)
 *       margin/padding (+ all side longhands) → block spacing (halved px)
 *       width (tables)                   → table width override
 *       border-collapse                  → row separators on/off
 *       padding (cells)                  → cell text inset
 *       background(-color)               → invert / dither shade / paper
 *       font-size (keywords/px/%/em)     → small/big buckets
 *       text-indent                      → block indent (no margin wins)
 *       color white-ish                  → invert flag
 *     Positioning (fixed/sticky/absolute/relative) is not implemented
 *     anywhere in the pipeline — every element flows as static, which
 *     IS the 1-bit mapping (sticky chrome scrolls away instead of
 *     eating the 240px viewport). z-index/top/left are inert.
 *     The inline flags share numeric values with DOC_INF_* so inherited
 *     flags merge into inline flags by mask-OR (document.c static-asserts).
 *   - Cascade: per property, !important declarations beat normal ones;
 *     among equal importance the matching rule with the highest
 *     specificity wins, ties broken by later sheet order. Inline style
 *     wins over everything (walker applies it after CSS).
 *
 * Zero dynamic allocation inside the engine: buffers belong to the caller
 * (DocParseResult), every table is capped, so a pathological page degrades
 * (rules dropped, truncated flag) instead of OOM-ing the device. Fully
 * host-testable with no PD API.
 */
#ifndef PLUTO_CSS_H
#define PLUTO_CSS_H

#include <stddef.h>

/* ── Caps (device budgets) ────────────────────────────────────────────────── */
#define CSS_MAX_SHEETS 8            /* <style> blocks parsed per page */
#define CSS_SHEET_MAX (16 * 1024)   /* per-sheet bytes parsed (16KB) */
#define CSS_MAX_RULES 128           /* rules per page, all sheets combined */
#define CSS_MAX_SELECTORS 4         /* comma-group entries kept per rule */
#define CSS_SELECTOR_MAX 96         /* chars per complex selector */
#define CSS_COMPOUND_MAX 8          /* compound parts per complex selector */
#define CSS_MAX_PROPS 8             /* declarations kept per rule */

/* 1-bit viewport for @media evaluation (device geometry). */
#define CSS_VIEWPORT_W 400
#define CSS_VIEWPORT_H 240

/* Computed-property bits. CSS_F_BOLD/ITALIC/UNDERLINE/STRIKE/INVERT are
 * numerically IDENTICAL to DOC_INF_BOLD/ITALIC/UNDERLINE/STRIKE/INVERT
 * (document.h) so inherited flags merge into inline flags by mask-OR.
 * The text-control bits are likewise DOC_INF_* aliases (asserted). */
#define CSS_F_BOLD 0x0001           /* == DOC_INF_BOLD */
#define CSS_F_ITALIC 0x0002         /* == DOC_INF_ITALIC */
#define CSS_F_UNDERLINE 0x0004      /* == DOC_INF_UNDERLINE */
#define CSS_F_HIDDEN 0x0008         /* display:none / visibility:hidden */
#define CSS_F_ALIGN_CENTER 0x0010
#define CSS_F_ALIGN_RIGHT 0x0020
#define CSS_F_STRIKE 0x0200         /* == DOC_INF_STRIKE */
#define CSS_F_INVERT 0x0400         /* == DOC_INF_INVERT */
#define CSS_F_UPPER 0x1000          /* == DOC_INF_UPPER */
#define CSS_F_LOWER 0x2000          /* == DOC_INF_LOWER */
#define CSS_F_CAP 0x4000            /* == DOC_INF_CAP */
#define CSS_F_PRE 0x8000            /* == DOC_INF_PRE */
#define CSS_F_PRELINE 0x10000       /* == DOC_INF_PRELINE */
#define CSS_F_NOWRAP 0x20000        /* == DOC_INF_NOWRAP */
#define CSS_F_BREAKWORD 0x40000     /* == DOC_INF_BREAKWORD */
/* font-size buckets use FRESH bits (NOT the align values: CENTER/RIGHT
 * share 0x10/0x20 and coexist in cssBits with size — aliasing would turn
 * every centered element's text small). The walker translates these to
 * DOC_INF_SMALL/BIG at inline emission. */
#define CSS_F_FSMALL 0x80000
#define CSS_F_FBIG 0x100000
#define CSS_INHERIT_MASK (CSS_F_BOLD | CSS_F_ITALIC | CSS_F_UNDERLINE | \
                          CSS_F_STRIKE | CSS_F_INVERT | CSS_F_UPPER | \
                          CSS_F_LOWER | CSS_F_CAP | CSS_F_PRE | \
                          CSS_F_PRELINE | CSS_F_NOWRAP | CSS_F_BREAKWORD | \
                          CSS_F_FSMALL | CSS_F_FBIG)

/* One <style> body (points into the page's raw HTML buffer). */
typedef struct
{
    const char *start;
    size_t len;
} CssSheet;

/* One simple selector atom: type / .class / #id / * / [attr[=value]]
 * (combinable in one compound, e.g. "div.note#x[hidden]"). */
typedef struct
{
    const char *type;        /* element tag (lowercased compare) or NULL */
    unsigned char typeLen;
    const char *cls;         /* class token (case-sensitive) or NULL */
    unsigned char clsLen;
    const char *id;          /* id token (case-sensitive) or NULL */
    unsigned char idLen;
    unsigned char universal; /* '*' seen */
    const char *attr;        /* attribute name (lowercased compare) or NULL */
    unsigned char attrLen;
    const char *attrVal;     /* exact-match value or NULL (= presence test) */
    unsigned char attrValLen;
    /* Structural pseudo-class (single-colon): 0 none, 1 :first-child,
     * 2 :last-child, 3 :only-child, 4 :link (has href), 5 :nth-child
     * (nthA/nthB: odd=(2,1) even=(2,0) N=(0,N)). Others (:hover,
     * :visited, ::marker…) are unusable. Counts as a class for
     * specificity. */
    unsigned char pclass;
    short nthA;
    short nthB;
} CssSimple;

/* One rule: up to CSS_MAX_SELECTORS complex selectors + declarations. */
typedef struct
{
    CssSimple sel[CSS_MAX_SELECTORS][CSS_COMPOUND_MAX];
    unsigned char selParts[CSS_MAX_SELECTORS]; /* compounds per selector */
    /* Combinator BEFORE each compound (index 0 unused): 0 = descendant
     * (whitespace), 1 = child (>). */
    unsigned char selComb[CSS_MAX_SELECTORS][CSS_COMPOUND_MAX];
    unsigned char selCount;
    unsigned short spec[CSS_MAX_SELECTORS]; /* precomputed specificity */
    const char *prop[CSS_MAX_PROPS]; /* lowercased, points into sheet text */
    const char *val[CSS_MAX_PROPS];
    unsigned char propLen[CSS_MAX_PROPS];
    unsigned char valLen[CSS_MAX_PROPS];
    unsigned char propImp[CSS_MAX_PROPS]; /* 1 = declared !important */
    unsigned char propCount;
    unsigned short order;  /* sheet position (cascade tie-break) */
    unsigned char skipped; /* no usable selector / overflow → never match */
    /* ::before/::after rule (pseudo != 0): never matches elements; only
     * the content cascade (css_pseudo_content) reads these. Counts as a
     * type selector for specificity. */
    unsigned char pseudo;  /* 0 = element, 1 = ::before, 2 = ::after */
} CssRule;

/* Per-page engine state. Lives on DocParseResult._css (heap, PLUTO_FREE). */
typedef struct
{
    CssRule rules[CSS_MAX_RULES];
    int ruleCount;
    int truncated; /* any cap hit (diagnostics) */
} CssEngine;

/* DOM access for descendant matching, supplied by the tree owner.
 * ancestor(node, i) fills out-params for the i-th ancestor (0 = parent):
 * returns a non-NULL handle and sets *tagLower ("" ok), *classAttr and
 * *idAttr (either may be NULL); returns NULL when there is no i-th
 * ancestor. Strings only need to stay valid until the next call.
 * attr(node, name, namelen) returns the attribute VALUE ("" ok) or NULL
 * when absent — backing [attr]/[attr=value] on any compound including
 * ancestors. NULL attr = attribute selectors never match (graceful).
 * sibling(node, dir) returns the nearest ELEMENT sibling (-1 previous,
 * +1 next) or NULL — backing :first-child/:last-child/:only-child/
 * :nth-child. NULL sibling = structural pseudos never match. */
typedef struct CssDom
{
    const void *(*ancestor)(const void *node, int i, const char **tagLower,
                            const char **classAttr, const char **idAttr);
    const char *(*attr)(const void *node, const char *name, size_t namelen);
    const void *(*sibling)(const void *node, int dir);
} CssDom;

/* ── Extract <style> blocks from raw HTML, document order.
 * Returns the UNcapped count found; only the first sheetMax are stored.
 * An unterminated final block yields the remainder as one sheet. */
int css_scan_sheets(const char *html, CssSheet *sheets, int sheetMax);

/* ── SW5: parse ONE complex selector string ("div.note p" — whitespace-
 * separated compounds = descendant; "div > p" = child) into parts[]. The
 * SAME compound grammar the rule parser uses (type / .class / #id / *
 * / [attr] and combinations); returns the compound count (0 = unusable
 * selector: [attr] operators, :pseudo, sibling combinators, over-length
 * … — querySelector treats 0 as a null result, never an exception).
 * combOut (may be NULL) receives the combinator before each compound
 * (0 = descendant, 1 = child). The compound tokens point INTO `scratch`
 * (the caller keeps it alive as long as `parts` is used;
 * JSBRIDGE_QS_SCRATCH bytes is the documented size; `sel` itself is not
 * modified). */
#define CSS_QS_MAX_COMPOUNDS CSS_COMPOUND_MAX
int css_parse_selector(const char *sel, char *scratch, size_t scratchSize,
                       CssSimple parts[CSS_QS_MAX_COMPOUNDS],
                       unsigned char combOut[CSS_QS_MAX_COMPOUNDS]);

/* ── SW5: does one parsed compound match an element? (Plain strings: tag
 * lowercased, class attribute, id attribute — NULL ok. A thin wrapper over
 * css_compound_matches for querySelector call sites. dom/node back [attr]
 * compounds; NULL dom (or NULL attr hook) = attribute parts never match.) */
int css_compound_matches_attrs(const CssSimple *c, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node);

/* ── Parse sheets into the engine (clears it first). */
void css_parse_sheets(CssEngine *eng, const CssSheet *sheets, int sheetCount);

/* ── One compound selector vs one element's identity. tagLower is the
 * lowercased tag ("" ok); class/id attrs may be NULL. dom/node back
 * [attr] compounds (NULL dom or NULL attr hook = never match). */
int css_compound_matches(const CssSimple *c, const char *tagLower,
                         const char *classAttr, const char *idAttr,
                         const CssDom *dom, const void *node);

/* ── Does `rule` match this element? dom resolves descendant selectors;
 * dom may be NULL → only single-compound selectors can match. */
int css_rule_matches(const CssRule *rule, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node);

/* ── One selector (parts + per-compound combinators) vs one element.
 * Shared ancestor walk behind css_rule_matches and querySelector, so the
 * two can never disagree on descendant/child semantics. combs[i] is the
 * combinator BEFORE compound i (0 = descendant, 1 = child). */
int css_chain_matches(const CssSimple *parts, const unsigned char *combs,
                      int np, const char *tagLower, const char *classAttr,
                      const char *idAttr, const CssDom *dom,
                      const void *node);

/* ── Cascade all matching rules into computed bits. Per property the
 * highest-specificity matching rule wins, later order breaking ties.
 * Any matching display:none/visibility:hidden sets CSS_F_HIDDEN. */
unsigned css_compute(const CssEngine *eng, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node);

/* Property lookup on a rule (NUL-terminated compare; NULL when absent). */
const char *css_rule_prop(const CssRule *r, const char *prop);

/* ── Cascade winner for ONE property (NUL-terminated name): the declaration
 * value with exact (ptr,len) extent, or NULL when no rule declares it.
 * Same cascade as css_compute (importance → specificity → order, UA-seeded
 * [hidden] display:none included — an author display always beats the
 * seed). Lets callers apply VALUE-carrying properties (border widths,
 * spacing, list styles, display re-show checks) that don't fit the
 * CSS_F_* bit channel. Points into sheet text (or a static for the UA
 * seed); valid until the next css_parse_sheets. */
const char *css_winning_value(const CssEngine *eng, const char *tagLower,
                              const char *classAttr, const char *idAttr,
                              const CssDom *dom, const void *node,
                              const char *prop, size_t *vlenOut);

/* ── Winning content value among ::before (before=1) / ::after (before=0)
 * rules matching this element: same cascade as css_winning_value, winner
 * among pseudo rules only. Raw value extent (quotes/attr() resolved by
 * the caller); NULL when no pseudo rule declares content. */
const char *css_pseudo_content(const CssEngine *eng, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node,
                               int before, size_t *vlenOut);

/* Length-delimited substring: strstr for values with no terminator. */
int css_val_has(const char *val, size_t vlen, const char *needle);

/* Length-delimited exact compare (case-insensitive): whole value equals
 * the literal (no trimming — values arrive pre-trimmed from the parser). */
int css_val_eq(const char *val, size_t vlen, const char *lit);

/* First Npx number in a length-delimited value (fraction dropped,
 * negative → 0); -1 when absent. Backs border-width and friends. */
int css_px_value(const char *val, size_t vlen);

/* Color → 0-255 luminance (#rgb/#rrggbb/named/rgb(); non-gray colors by
 * luminance — the 1-bit approximation). -1 when absent. */
int css_color_luminance(const char *val, size_t vlen);

/* TEMP INSTRUMENTATION (Wikipedia stall hunt — REMOVE after diagnosis). */
void css_log_stats(void);

/* Per-element match-mask memo generation: bump at each top-level walk
 * (parse + rewalk); consult/fill is internal to css_compute/winning. */
void css_match_cache_bump(void);

#endif /* PLUTO_CSS_H */
