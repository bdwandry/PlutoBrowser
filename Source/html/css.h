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
 *   - Selectors: type, .class, #id, universal (*), descendant combinator,
 *     and comma-grouped lists. Anything else ([attr], pseudo-classes,
 *     child/sibling combinators) marks that selector unusable; if no
 *     selector in the group survives, the rule is skipped — the browser
 *     behavior for unparseable selectors.
 *   - Properties mapped onto the 1-bit render model:
 *       display:none / visibility:hidden → element+subtree suppressed
 *       text-align:center|right          → block align
 *       font-weight (bold/bolder/≥600)   → bold inline flag
 *       font-style italic/oblique        → italic inline flag
 *       text-decoration underline/line-through → flags
 *       color white-ish / background black-ish → invert flag
 *     The five inline flags share numeric values with DOC_INF_* so inherited
 *     flags merge into inline flags by mask-OR (document.c static-asserts).
 *   - Cascade: per property, the winning declaration is the one from the
 *     matching rule with the highest specificity, ties broken by later
 *     sheet order. Inline style wins over everything (walker applies it
 *     after CSS).
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

/* Computed-property bits. CSS_F_BOLD/ITALIC/UNDERLINE/STRIKE/INVERT are
 * numerically IDENTICAL to DOC_INF_BOLD/ITALIC/UNDERLINE/STRIKE/INVERT
 * (document.h) so inherited flags merge into inline flags by mask-OR. */
#define CSS_F_BOLD 0x0001           /* == DOC_INF_BOLD */
#define CSS_F_ITALIC 0x0002         /* == DOC_INF_ITALIC */
#define CSS_F_UNDERLINE 0x0004      /* == DOC_INF_UNDERLINE */
#define CSS_F_HIDDEN 0x0008         /* display:none / visibility:hidden */
#define CSS_F_ALIGN_CENTER 0x0010
#define CSS_F_ALIGN_RIGHT 0x0020
#define CSS_F_STRIKE 0x0200         /* == DOC_INF_STRIKE */
#define CSS_F_INVERT 0x0400         /* == DOC_INF_INVERT */
#define CSS_INHERIT_MASK (CSS_F_BOLD | CSS_F_ITALIC | CSS_F_UNDERLINE | \
                          CSS_F_STRIKE | CSS_F_INVERT)

/* One <style> body (points into the page's raw HTML buffer). */
typedef struct
{
    const char *start;
    size_t len;
} CssSheet;

/* One simple selector atom: type / .class / #id / * (combinable in one
 * compound, e.g. "div.note#x"). */
typedef struct
{
    const char *type;        /* element tag (lowercased compare) or NULL */
    unsigned char typeLen;
    const char *cls;         /* class token (case-sensitive) or NULL */
    unsigned char clsLen;
    const char *id;          /* id token (case-sensitive) or NULL */
    unsigned char idLen;
    unsigned char universal; /* '*' seen */
} CssSimple;

/* One rule: up to CSS_MAX_SELECTORS complex selectors + declarations. */
typedef struct
{
    CssSimple sel[CSS_MAX_SELECTORS][CSS_COMPOUND_MAX];
    unsigned char selParts[CSS_MAX_SELECTORS]; /* compounds per selector */
    unsigned char selCount;
    unsigned short spec[CSS_MAX_SELECTORS]; /* precomputed specificity */
    const char *prop[CSS_MAX_PROPS]; /* lowercased, points into sheet text */
    const char *val[CSS_MAX_PROPS];
    unsigned char propLen[CSS_MAX_PROPS];
    unsigned char valLen[CSS_MAX_PROPS];
    unsigned char propCount;
    unsigned short order;  /* sheet position (cascade tie-break) */
    unsigned char skipped; /* no usable selector / overflow → never match */
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
 * ancestor. Strings only need to stay valid until the next call. */
typedef struct CssDom
{
    const void *(*ancestor)(const void *node, int i, const char **tagLower,
                            const char **classAttr, const char **idAttr);
} CssDom;

/* ── Extract <style> blocks from raw HTML, document order.
 * Returns the UNcapped count found; only the first sheetMax are stored.
 * An unterminated final block yields the remainder as one sheet. */
int css_scan_sheets(const char *html, CssSheet *sheets, int sheetMax);

/* ── SW5: parse ONE complex selector string ("div.note p" — whitespace-
 * separated compounds = descendant) into parts[]. The SAME compound grammar
 * the rule parser uses (type / .class / #id / * and combinations); returns
 * the compound count (0 = unusable selector: [attr], :pseudo, child
 * combinators, over-length … — querySelector treats 0 as a null result,
 * never an exception). The compound tokens point INTO `scratch` (the caller
 * keeps it alive as long as `parts` is used; JSBRIDGE_QS_SCRATCH bytes is
 * the documented size; `sel` itself is not modified). */
#define CSS_QS_MAX_COMPOUNDS CSS_COMPOUND_MAX
int css_parse_selector(const char *sel, char *scratch, size_t scratchSize,
                       CssSimple parts[CSS_QS_MAX_COMPOUNDS]);

/* ── SW5: does one parsed compound match an element? (Plain strings: tag
 * lowercased, class attribute, id attribute — NULL ok. A thin wrapper over
 * css_compound_matches for querySelector call sites.) */
int css_compound_matches_attrs(const CssSimple *c, const char *tagLower,
                               const char *classAttr, const char *idAttr);

/* ── Parse sheets into the engine (clears it first). */
void css_parse_sheets(CssEngine *eng, const CssSheet *sheets, int sheetCount);

/* ── One compound selector vs one element's identity. tagLower is the
 * lowercased tag ("" ok); class/id attrs may be NULL. */
int css_compound_matches(const CssSimple *c, const char *tagLower,
                         const char *classAttr, const char *idAttr);

/* ── Does `rule` match this element? dom resolves descendant selectors;
 * dom may be NULL → only single-compound selectors can match. */
int css_rule_matches(const CssRule *rule, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node);

/* ── Cascade all matching rules into computed bits. Per property the
 * highest-specificity matching rule wins, later order breaking ties.
 * Any matching display:none/visibility:hidden sets CSS_F_HIDDEN. */
unsigned css_compute(const CssEngine *eng, const char *tagLower,
                     const char *classAttr, const char *idAttr,
                     const CssDom *dom, const void *node);

/* Property lookup on a rule (NUL-terminated compare; NULL when absent). */
const char *css_rule_prop(const CssRule *r, const char *prop);

#endif /* PLUTO_CSS_H */
