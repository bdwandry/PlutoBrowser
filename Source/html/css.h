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
 *     (content cascade only — never element matches), ::marker (li
 *     bullet text lane), the consumed-inert ::-family
 *     (::first-line/::first-letter/::selection/::placeholder/
 *     ::backdrop), the Phase 7 truth pseudo-classes (:target/:root/
 *     :disabled/:enabled/:checked/:placeholder-shown/:empty), and
 *     comma-grouped lists. Anything else ([attr~=...] operators, other
 *     pseudo-classes, sibling combinators) marks that selector
 *     unusable; if no selector in the group survives, the rule is
 *     skipped — the browser behavior for unparseable selectors.
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
 *     Positioning (Phase 4 policy — spec-anchored, reader-mode
 *     precedent, one-line reversible in walk_node): static flows.
 *     relative and sticky keep flowing (sticky chrome scrolls away
 *     instead of eating the 240px viewport — the honest 1-bit
 *     mapping). absolute/fixed with NO offset and NO z-index resolve
 *     to their STATIC position per CSS — they flow exactly as they
 *     would where written. absolute/fixed WITH an offset
 *     (any of top/left/right/bottom non-auto) or WITH a numeric
 *     z-index is an out-of-flow OVERLAY — in a compositor it would
 *     render ON TOP of the document, not as document content — so the
 *     flat renderer drops it (like Firefox/Safari reader modes strip
 *     fixed chrome; bryanwandrych.com receipts: the skip-link and the
 *     off-screen helper both leave, sticky nav and relative wrappers
 *     stay). z-index alone on a static element is inert (has no
 *     effect without a positioned box — CSS-correct).
 *     Flexbox/Grid (Phase 6 policy — the honest 1-bit mapping is the
 *     COLUMN model: real browsers run a 2-D compositor; the flat page
 *     cannot place items side-by-side, but the flex-column axis is
 *     EXACTLY what the block flow already does, so the lane implements
 *     the axis-honest subset and leaves the 2-D distribution inspected.
 *       display:flex|inline-flex|grid|inline-grid → container bit
 *         (a flex container is a block box — walk_display_mode already
 *         routed these values to the fresh-block path; the bit now
 *         also gates the gap transport below);
 *         display:contents → the element generates NO box (walker
 *         flattening — children flow into the parent's context),
 *         already honored on the div/span box-switch path;
 *       gap / row-gap → real vertical spacing between the container's
 *         children (halved px, mounted on child spacing-hints like
 *         margins; the EXACT semantics subtract it again from the
 *         last child at the container's CSS exit — CSS: no gap after
 *         the last item). Real-sheet values are mostly rem (modern
 *         default 16px root), so the transport parses px|rem|em|pt|
 *         vw|%|bare numbers on the FIRST token (row-gap wins over
 *         the gap shorthand; column-gap stays inline-inert);
 *         gap delegated on a NON-flex box is inert (CSS-correct).
 *         Deviation, documented: the walker applies the gap to every
 *         child block and subtracts only the last, so one blank-ish
 *         advance after the container can occur when the last flex
 *         item is a text-only inline mixture — the dominant modern
 *         pattern (block-level card children) is exact.
 *       align-items:center propagates cross-axis centering to the
 *         container's child blocks through the CSS-exit carry; the
 *         container's own text block centers too (it is an anonymous
 *         flex item). align-self:center centers only its own element
 *         (element-scoped carry, stripped at the boundary). Both are
 *         gated on the container actually being flex/grid — on a
 *         block box the name is inspected but inert (CSS-correct).
 *         flex-direction (row/column/…): the flow already IS the
 *         column mapping — content order preserved, items stack —
 *         so all four axis values are inspected no-ops and
 *         justify/justify-items, place/align-content, flex-grow,
 *         shrink/basis/order and the grid templates remain census
 *         vocabulary (a 2-D compositor is the only honest home for
 *         them). The family names leave the unknown-prop gauge —
 *         census shows them consumed.
 *     Overflow (Phase 5 policy — the flat page IS the only scroll
 *     region; nested scroll regions cannot exist): overflow:auto/
 *     scroll/visible are inspected and honored as no-ops (content
 *     always fully visible — the reader stance; containment
 *     wrappers keep every word). BUT the W3C "visually-hidden"
 *     recipe — position: absolute/fixed + overflow:hidden + a
 *     CONCEALING clip shape (clip-path inset with a nonzero draw,
 *     legacy clip:rect) or a 1px-by-1px box — is INVISIBLE on the
 *     real web, so the walker drops it (author-intent honored;
 *     bryanwandrych.com carries the recipe verbatim in its sheet:
 *     .card-hit>span). text-overflow:ellipsis + white-space:nowrap
 *     truncates the overflowing single line with "..." (the author
 *     explicitly asked for the cut). -webkit-line-clamp: N caps a
 *     text block to N wrapped lines with the same "..." tail (the
 *     classic card-snippet truncation). The overflow names leave
 *     the unknown-prop gauge — census shows them consumed.
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

/* Phase 1.5: external <link rel="stylesheet"> fetch caps. PLUTO_CSS_EXT_MAX
 * joins the INLINE sheet slots (CSS_MAX_SHEETS total join); the byte cap
 * keeps the per-doc RAM envelope (bodies are doc-lifetime buffers). */
#define PLUTO_CSS_EXT_MAX 6
#define PLUTO_CSS_EXT_TOTAL_MAX (96 * 1024)
#define CSS_SHEET_MAX (16 * 1024)   /* per-sheet bytes parsed (16KB) */
#define CSS_MAX_RULES 128           /* rules per page, all sheets combined */
#define CSS_MAX_SELECTORS 4         /* comma-group entries kept per rule */
#define CSS_SELECTOR_MAX 96         /* chars per complex selector */
#define CSS_COMPOUND_MAX 8          /* compound parts per complex selector */
#define CSS_MAX_PROPS 8             /* declarations kept per rule */

/* 1-bit viewport for @media evaluation (device geometry). */
#define CSS_VIEWPORT_W 400
#define CSS_VIEWPORT_H 240
/* Phase 9 unit widening: the vmin/vmax baselines (min/max of the
 * viewport geometry — a 400x240 panel never reflows, so these are
 * compile-time constants like W/H themselves). */
#define CSS_VMINDIM 240
#define CSS_VMAXDIM 400

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
/* Phase 4: positioning policy bits (never in INHERIT_MASK — computed
 * per element, inherited flag merge must not touch them). */
#define CSS_F_OOF 0x200000  /* position: fixed / absolute (out of flow) */
#define CSS_F_OFF 0x400000  /* an offset exists (top/left/right/bottom) */
#define CSS_F_OZ 0x800000   /* z-index carries a numeric value */
/* Phase 5: overflow-family policy bits. ELLIP is the one bit that rides
 * the DocInline ABI (== DOC_INF_ELLIP, see document.c's static asserts);
 * it must be CARRIED to the walker's flag merge explicitly because
 * text-overflow is NOT an inherited property (scoped to its declaration
 * subtree through the element-open path). OVH/CLIPN stay walker-side. */
#define CSS_F_ELLIP 0x1000000 /* text-overflow: ellipsis (ABI bit) */
#define CSS_F_OVH 0x4000000   /* overflow: hidden declared */
#define CSS_F_CLIPN 0x8000000 /* a concealing clip shape */
/* Phase 6: flex/grid-family policy bits (walker-side; never in
 * INHERIT_MASK — the subtree carries come from the explicit CSS-exit
 * transport, not inherited-flags merging). AIC/ASC ride the walker's
 * flag word with the ELLIP pattern; both land undefined in DOC_INF
 * space on inline spans (harmless there — block-align is block-level). */
#define CSS_F_FLEXCT 0x2000000  /* display: flex|inline-flex|grid|…‑grid */
#define CSS_F_AIC 0x10000000    /* align-items: center (container lane) */
#define CSS_F_ASC 0x20000000    /* align-self: center (element lane) */
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
 * (combinable in one compound, e.g. "div.note#x[hidden]") + Phase-1:
 * attribute OPERATORS ([a^=v] ^= $= *= ~= |=) and one simple :not token. */
typedef struct
{
    const char *type;        /* element tag (lowercased compare) or NULL */
    const char *cls;         /* class token (case-sensitive) or NULL */
    const char *id;          /* id token (case-sensitive) or NULL */
    const char *attr;        /* attribute name (lowercased compare) or NULL */
    const char *attrVal;     /* comparison value or NULL (= presence test) */
    const char *notTok;      /* :not(inner) token or NULL (Phase 1) */
    /* Comparison operator for [attr … value] (Phase 1):
     * 0 = presence (attrVal stays NULL), 1 = equals (plain [a=v]),
     * 2 = ~= word-in-list, 3 = |= hyphen/lang prefix, 4 = ^= starts-with,
     * 5 = $= ends-with, 6 = *= contains. Value compares stay case-
     * sensitive per CSS. */
    unsigned char attrOp;
    /* :not(<simple>) store (Phase 1, ONE simple token — the CSS3 shape):
     * 1 = element tag, 2 = .class token, 3 = #id token, 4 = [attr]
     * presence. 0 = no :not constraint. Tokens point into sheet text. */
    unsigned char notKind;
    unsigned char notLen;
    unsigned char typeLen;
    unsigned char clsLen;
    unsigned char idLen;
    unsigned char universal; /* '*' seen */
    unsigned char attrLen;
    unsigned char attrValLen;
    /* Structural pseudo-class (single-colon): 0 none, 1 :first-child,
     * 2 :last-child, 3 :only-child, 4 :link (has href), 5 :nth-child
     * (nthA/nthB: odd=(2,1) even=(2,0) N=(0,N)), 8 = interactivity
     * states (:hover/:focus/:active/:visited) — ALWAYS MATCH on this
     * device: there is no persistent hover pointer, and desktop pages
     * often keep menu/panel content invisible until one fires (which
     * would otherwise hide content forever). Others (::marker, …) are
     * unusable. Counts as a class for specificity. */
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
     * (whitespace), 1 = child (>), 2 = adjacent sibling (+), 3 =
     * general sibling (~). */
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
    /* Pseudo-element rule (pseudo != 0): never matches elements; only
     * the content cascade (css_pseudo_content) reads ::before/::after
     * (1/2) and ::marker (4). 3 = the Phase 7 consumed-inert family
     * (::first-line/::first-letter/::selection/::placeholder/
     * ::backdrop — kept, counted for specificity, never asked again). */
    unsigned char pseudo;  /* 0 = element, 1 = ::before, 2 = ::after,
                            * 3 = consumed inert, 4 = ::marker */
} CssRule;

/* ── Phase 2: custom properties (--name) for var() resolution ──
 * One engine-table entry; see CssEngine for the cascade comment. */
#define CSS_CUSTOM_MAX 24
#define CSS_CUSTOM_NAME 12 /* 11 chars + NUL; longer names refuse */
/* Phase 10: @keyframes name registry caps. Names longer than
 * CSS_KFRAMES_NAME - 1 chars REFUSE to register (an unregistered name
 * means an inert animation — no end event, no false dedupe collisions). */
#define CSS_KFRAMES_MAX 24
#define CSS_KFRAMES_NAME 25 /* 24 chars + NUL */
typedef struct
{
    const char *val;
    unsigned char valLen;
    unsigned char important;
    char name[CSS_CUSTOM_NAME]; /* the name WITHOUT the leading "--" */
} CssCustom;

/* Per-page engine state. Lives on DocParseResult._css (heap, PLUTO_FREE). */
typedef struct
{
    CssRule rules[CSS_MAX_RULES];
    int ruleCount;
    int truncated; /* any cap hit (diagnostics) */
    /* ── Phase 2: custom properties (--name) for var() resolution ──
     * Document-order table filled at sheet parse (LAST declaration wins
     * unless an earlier one was !important and the new one is not —
     * importance beats order, mirroring the rule cascade). val points
     * INTO sheet text (doc-lifetime, same contract as rules). */
    CssCustom custom[CSS_CUSTOM_MAX];
    int customCount;
    /* Phase 7: the CURRENT DOCUMENT FRAGMENT (the id after '#' in the
     * page URL) — :target's honest matcher input. The walker fills it
     * before each walk (css_set_target). Empty = no destination, so
     * :target matches nothing (spec). Case-sensitive: HTML ids are. */
    char target[56];
    /* Phase 10: @keyframes NAME registry (the frame bodies stay
     * unparsed — the engine needs only the NAMES so an `animation:`
     * shorthand/longhand can resolve to a declared animation and the
     * animation-lane end event can engage). Collected during sheet
     * parse from top-level sheets AND held-@media bodies (flatten_media
     * inlines held bodies before this scan; dropped bodies never reach
     * the scan — cascade-honest). Names are case-insensitive CSS
     * idents, stored lowercased-compare via css_eq_ncase. */
    char keyframes[CSS_KFRAMES_MAX][CSS_KFRAMES_NAME];
    int keyframeCount;
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
 * :nth-child. NULL sibling = structural pseudos never match.
 * siblingInfo(node, dir, …) = the same nearest ELEMENT sibling, filling
 * out tagLower/classAttr/idAttr for it — backing the sibling
 * COMBINATORS ('+' adjacent, '~' general). NULL siblingInfo (or NULL
 * result from it mid-chain) = sibling combinator selectors never match
 * (graceful, never misapplied). childless(node) = 1 when the node
 * generates NO content children (spec :empty: no elements and no text;
 * comment nodes are not content), 0 otherwise, -1 unknown. NULL →
 * :empty never matches (graceful, like every other absent accessor). */
typedef struct CssDom
{
    const void *(*ancestor)(const void *node, int i, const char **tagLower,
                            const char **classAttr, const char **idAttr);
    const char *(*attr)(const void *node, const char *name, size_t namelen);
    const void *(*sibling)(const void *node, int dir);
    const void *(*siblingInfo)(const void *node, int dir,
                               const char **tagLower, const char **classAttr,
                               const char **idAttr);
    int (*childless)(const void *node); /* Phase 7: :empty truth */
} CssDom;

/* ── Extract <style> blocks from raw HTML, document order.
 * Returns the UNcapped count found; only the first sheetMax are stored.
 * An unterminated final block yields the remainder as one sheet. */
int css_scan_sheets(const char *html, CssSheet *sheets, int sheetMax);

/* ── Phase 1.5: external <link rel="stylesheet"> scan ──
 * Light scan of raw HTML for stylesheet LINKS the renderer should
 * prefetch. hrefs[i] (≤511 chars, entity-decoded &amp;-style basics) is
 * the RAW href — absolute or relative; resolving against the page URL is
 * the caller's job (url_resolve). mediaOk[i] = 1 when the media
 * attribute is absent/empty or names "all"/"screen" (substring, case-
 * insensitive) — rich queries stay with Phase 3. A type attribute
 * present WITHOUT "css" anywhere in it skips that entry. Scans skip
 * comment spans and <script> bodies (bundles routinely carry link-tag
 * strings inside template literals). Returns the count stored (capped,
 * deduped by href); never fails hard. */
int css_ext_scan_links(const char *html,
                       char hrefs[PLUTO_CSS_EXT_MAX][512],
                       unsigned char mediaOk[PLUTO_CSS_EXT_MAX]);

/* ── SW5: parse ONE complex selector string ("div.note p" — whitespace-
 * separated compounds = descendant; "div > p" = child; "h2 + p" =
 * adjacent sibling; "li ~ li" = general sibling) into parts[]. The
 * SAME compound grammar the rule parser uses (type / .class / #id / *
 * / [attr] with operators and combinations); returns the compound count
 * (0 = unusable selector: junk inside brackets, unparseable :not args,
 * over-length … — querySelector treats 0 as a null result, never an
 * exception).
 * combOut (may be NULL) receives the combinator before each compound
 * (0 = descendant, 1 = child, 2 = adjacent, 3 = general sibling). The
 * compound tokens point INTO `scratch`
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

/* ── Forms-Phase 3: the shared constraint-pattern matcher (the honest
 * common subset lives here so BOTH the CSS :valid/:invalid road and the
 * JS validity layer run ONE rulebook; see the css.c block for the exact
 * grammar). Returns 1 when `value` matches `pattern` under the pattern
 * attribute's implicit ^…$ anchoring; an unsupported pattern never
 * mismatches (a browser's invalid-pattern rule). */
int css_pattern_matches(const char *pattern, const char *value);

/* ── Parse sheets into the engine (clears it first). */
void css_parse_sheets(CssEngine *eng, const CssSheet *sheets, int sheetCount);

/* ── Phase 10: @keyframes name registry ───────────────────────────────────
 * The engine needs only the NAMES: an `animation:` value resolves to a
 * declared animation when a token matches a collected name, and the
 * walker engages the animation-lane end event for that window. Frame
 * bodies stay unparsed (the @-rule skip path is untouched — keyframes
 * remain census-free). Names refuse beyond CSS_KFRAMES_NAME - 1 chars
 * (unregistered = inert animation, no end event, no dedupe collision). */
int css_keyframes_collect(CssEngine *eng, const char *name, size_t len);
/* 1 when the registry holds this name (case-insensitive CSS ident). */
int css_keyframes_known(const CssEngine *eng, const char *name, size_t len);
/* Pick the FIRST name token of a `animation:`/`animation-name:` VALUE
 * span that the registry knows (the spec's registry disambiguation:
 * duration/timing/direction/fill/state/bounds/iteration keywords and
 * functional notation are not name candidates). Returns a pointer INTO
 * `val` (sheet-lifetime doc text) with *outLen set, or NULL when no
 * token registers (inert animation — the walker must not engage). */
const char *css_anim_registered_name(const CssEngine *eng, const char *val,
                                     size_t len, size_t *outLen);

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

/* ── Winning content value among pseudo rules matching this element:
 * same cascade as css_winning_value, winner among pseudo rules only.
 * `lane`: 1 = ::before, 2 = ::after, 4 = ::marker (the content-carrying
 * pseudo-elements; 3 = the consumed-inert family, never queried — the
 * rules parse+keep with pseudo-element "never matches elements" spec
 * semantics, so a stylesheet's ::first-line/etc costs nothing but
 * census). Raw value extent: quotes/attr() resolved by the caller
 * (walk_pseudo_text); NULL when no pseudo rule of that lane declares
 * content. */
const char *css_pseudo_content(const CssEngine *eng, const char *tagLower,
                               const char *classAttr, const char *idAttr,
                               const CssDom *dom, const void *node,
                               int lane, size_t *vlenOut);

/* Phase 7: fill :target's matcher input — the CURRENT fragment id from
 * the page URL ("" = none: then no element matches :target). The walker
 * calls once per walk (parse + re-walk paths) before any query. */
void css_set_target(CssEngine *eng, const char *id, size_t len);

/* Phase 8: standalone media= evaluator for link/source screening (link
 * media= uses this same language today; <picture><source media> joins).
 * qs/qe = half-open [qs,qe) span, qe may be NULL (strlen). 1 = holds. */
int css_media_holds(const char *qs, const char *qe);

/* Phase 9: gradient stop average → luminance 0..255 (-1 = no resolvable
 * stop). Feeds the reader's ink-polarity decision for gradient
 * BACKGROUNDS (dark hero = browser-invisible black text; the average
 * is what a solid background-color would say). linear/radial/conic
 * share the shape; unresolved stops drop out of the average. */
int css_gradient_luma(const char *val, size_t vlen);

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
