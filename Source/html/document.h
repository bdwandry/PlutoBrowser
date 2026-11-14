/*
 * PlutoBrowser — document.h
 * Port of Source/html/document.lua (part 1: parse helpers + parse skeleton).
 *
 * Remaining parts of document.lua (the element walker building blocks/links)
 * arrive in later phases; document_parse currently implements the exact
 * reference behavior for: empty input, title extraction, reader dispatch
 * (stub until P19 lands readability), base-href override, meta refresh scan,
 * and empty doc/block/link scaffolding.
 */
#ifndef PLUTO_DOCUMENT_H
#define PLUTO_DOCUMENT_H

#include "pd_api.h"
#include "html/dom.h"

/* ── Style map (port of parseStyle's Lua table) ─────────────────────────────
 * Fixed-capacity lowercase key→trimmed-lowercase-value list, preserving the
 * LAST occurrence of duplicate keys (Lua table semantics). */
#define DOC_STYLE_MAX 16

typedef struct
{
    char key[32];
    char val[128];
    int used;
} DocStyleEntry;

typedef struct
{
    DocStyleEntry e[DOC_STYLE_MAX];
    int count;
} DocStyle;

/* ── Box spacing (port of parseBoxSpacing's return table) ────────────────── */
typedef struct
{
    int top, bottom, left, right;
    /* Phase 5: -webkit-line-clamp winning value parsed to a line count
     * (0 = unlimited). Filled by walk_box_spacing (CSS hint query) for
     * every block-open element, assigned to DocBlock.clampLines. */
    int clamp;
} DocBoxSpacing;

/* Split an over-wide word into maxW-fitting pieces (UTF-8 safe: never
 * inside a multibyte sequence; an oversize glyph emits whole). mf(ctx, s)
 * measures NUL-terminated candidates; emit(ectx, start, n, px, isLast)
 * fires per piece (start points INTO word — copy it). Single glyphs wider
 * than maxW emit whole (progress guaranteed). Returns piece count. */
int doc_split_long_word(const char *word, int maxW,
                        int (*mf)(void *ctx, const char *s), void *mctx,
                        void (*emit)(void *ectx, const char *pc, size_t n,
                                     int w, int isLast),
                        void *ectx);
/* ── Meta refresh (port of the metaRefresh table) ────────────────────────── */
typedef struct
{
    int present;      /* metaRefresh found */
    float delay;      /* tonumber(delayStr) or 0 */
    char url[512];    /* resolved URL; url[0]=='\0' means nil */
} DocMetaRefresh;

/* ── Inline items (port of block.inlines entries) ─────────────────────────── */
#define DOC_INLINE_TEXT 0
#define DOC_INLINE_BR 1
#define DOC_INLINE_WBR 2

/* Style flags (mirror Lua boolean fields). */
#define DOC_INF_BOLD 0x0001
#define DOC_INF_ITALIC 0x0002
#define DOC_INF_UNDERLINE 0x0004
#define DOC_INF_CODE 0x0008
#define DOC_INF_SMALL 0x0010
#define DOC_INF_BIG 0x0020
#define DOC_INF_SUB 0x0040
#define DOC_INF_SUP 0x0080
#define DOC_INF_MARK 0x0100
#define DOC_INF_STRIKE 0x0200
#define DOC_INF_INVERT 0x0400
#define DOC_INF_INERT 0x0800
/* CSS text controls (css.h CSS_F_* aliases, static-asserted in document.c).
 * Ride w->flags like all inline flags: inherited through WX_CSS_END
 * scoping, stamped onto inlines at emission, read by the layout breaker
 * (PRE newlines force breaks, NOWRAP suppresses wraps). */
#define DOC_INF_UPPER 0x1000
#define DOC_INF_LOWER 0x2000
#define DOC_INF_CAP 0x4000
#define DOC_INF_PRE 0x8000
#define DOC_INF_PRELINE 0x10000
#define DOC_INF_NOWRAP 0x20000
#define DOC_INF_BREAKWORD 0x40000
/* Phase 5: text-overflow:ellipsis — == CSS_F_ELLIP (ABI-checked in
 * document.c). NOT part of INHERIT_MASK: text-overflow is a non-
 * inherited property, so the bit travels only within the declaring
 * element's subtree (carried to w->flags at the element-open merge,
 * restored by the same exit frame). */
#define DOC_INF_ELLIP 0x1000000

typedef struct
{
    int type; /* DOC_INLINE_* */
    char *text; /* arena string, NULL for br/wbr */
    unsigned flags; /* DOC_INF_* */
    char *href; /* arena string or NULL */
    int anchorIndex; /* 0 = none */
} DocInline;

/* ── Block types (port of doc.blocks[].type) ─────────────────────────────── */
#define DOC_BLOCK_PARAGRAPH 0
#define DOC_BLOCK_HEADING 1
#define DOC_BLOCK_BLOCKQUOTE 2
#define DOC_BLOCK_LIST_ITEM 3
#define DOC_BLOCK_IMAGE 4
#define DOC_BLOCK_HR 5
#define DOC_BLOCK_CODE_BLOCK 6
#define DOC_BLOCK_TABLE 7
#define DOC_BLOCK_HIDDEN_FIELD 8
#define DOC_BLOCK_CHECKBOX_FIELD 9
#define DOC_BLOCK_INPUT_FIELD 10
#define DOC_BLOCK_INPUT_SUBMIT 11
#define DOC_BLOCK_SELECT_FIELD 12
#define DOC_BLOCK_BOX_OPEN 13
#define DOC_BLOCK_BOX_CLOSE 14
#define DOC_BLOCK_PLACEHOLDER 15
#define DOC_BLOCK_METER 16
#define DOC_BLOCK_MATH 17
#define DOC_BLOCK_READER_HEADER 18 /* emitted by readability (later phase) */

/* Walker caps (Lua MAX_BLOCKS / MAX_INLINES). Shared with readability.c. */
#define DOC_MAX_BLOCKS 1200
#define DOC_MAX_INLINES 900

/* ── Table cells / rows (port of tbl.rows[i].cells[j]) ───────────────────── */
typedef struct
{
    DocInline **inlines; /* heap array */
    int inlineCount;
    int inlineCap;
    int header; /* th */
    int colspan;
    int rowspan;
    int pad; /* CSS cell padding px (halved, snapshot-serialized) */
    char *abbr; /* arena string (may be "") */
    const char *align; /* "center"/"right"/"left" or NULL */
} DocCell;

typedef struct
{
    DocCell **cells; /* heap array */
    int cellCount;
    int cellCap;
} DocRow;

/* Column definition from <colgroup>/<col> (WHATWG §4.9.4/§4.9.5). */
typedef struct
{
    int span;      /* >= 1 (attr span, clamped) */
    int width;     /* -1 = unspecified; else pixel width (attr width prefix) */
    int percent;   /* width attr ended with '%' → percent of table */
    const char *align; /* "center"/"right"/"left" or NULL */
} DocCol;

typedef struct
{
    DocRow **rows; /* heap array */
    int rowCount;
    int rowCap;
    char *caption; /* arena string ("" when absent) */
    const char *align; /* or NULL */
    int border; /* border attr present and != "0" */
    int collapse; /* 0 = legacy separators, 1 = collapse, 2 = separate */
    char *width; /* arena string or NULL */
    DocCol **cols; /* heap array of column defs (NULL when no <colgroup>) */
    int colCount;
    int colCap;
    int colTotal;  /* sum of span over colCount */
    /* Layout scratch (owned by the layout engine, zeroed at parse time): */
    int widthPx;   /* resolved table width in layout pixels */
    int *colX;     /* column left offsets (one per grid column) */
    int *colW;     /* column widths (one per grid column) */
    int gridCount; /* number of grid columns (max colTotal, rowCount) */
} DocTable;

/* ── Select options (port of select options + datalist entries) ──────────── */
typedef struct
{
    char *text; /* arena */
    char *value; /* arena */
    int group;
    int selected;
    int disabled;
    /* Phase 8: the walk-time truth (selected attribute), so a tap on a
     * reset button restores the option set the author shipped (taps on
     * a multiple select accumulate into first-selected). */
    int defaultSelected;
} DocOption;

/* ── Image maps (port of doc.maps[name] = { shape, coords, href, alt }) ──── */
typedef struct
{
    char *shape; /* arena */
    int *coords; /* heap array */
    int coordCount;
    char *href; /* arena or NULL */
    char *alt; /* arena */
} DocArea;

typedef struct
{
    char *name; /* arena */
    DocArea **areas; /* heap array */
    int areaCount;
    int areaCap;
} DocMap;

typedef struct
{
    char *id; /* arena */
    DocOption **options; /* heap array */
    int optionCount;
    int optionCap;
} DocDatalist;

/* ── Blocks (port of doc.blocks[i]) ──────────────────────────────────────── */
typedef struct
{
    int type; /* DOC_BLOCK_* */
    /* layout fields shared by flow blocks */
    int level; /* heading */
    const char *align; /* "center"/"right"/"left" or NULL */
    int spacingTop, spacingBottom, indent;
    /* Phase 5: -webkit-line-clamp line budget (0 = unlimited). Carried
     * by the walker's box-hint query (DocBoxSpacing.clamp) at every
     * text block open; consumed by render/layout.c (BreakOpts.maxLines). */
    int clampLines;
    /* Phase 6: the flex/grid container's row-gap mounted on a CHILD
     * block (halved px; the first item skips — the gap rides BETWEEN
     * items). Consumed by layout's per-block advances; inert on any
     * block opened outside a flex/grid container. See css.h policy. */
    int gapApplied;
    /* Phase 7: the li's ::marker content text (the winning content
     * among ::marker rules — walk_pseudo_text resolved; arena string).
     * NULL = the default bullet glyph ("*" family/number). "" = the
     * author REMOVED the bullet (content: ""), layout draws nothing. */
    char *markerText;
    int hasSpacing; /* Lua parity: element blocks carry spacing keys (printed
                     * even when 0); implicit stray-text paragraphs don't. */
    int invert;
    int border; /* CSS border width px, 0 = none (snapshot-serialized) */
    int bgGray; /* CSS background luminance, -1 = none (new_block inits) */
    /* R71 app bar: the FIRST and LAST block of a top-anchored
     * position:sticky element's subtree. Layout records the item span
     * and re-draws it pinned at the content-viewport top once the page
     * scrolls past its flow position (real sticky semantics). */
    unsigned char pinStart, pinEnd;
    /* Flex/grid ROW transport (CSS display:flex/grid with row direction):
     * direct children of the container become flex items. Blocks from one
     * child share flexItem and stack; different items sit side by side on
     * one row. Layout groups consecutive blocks by flexRow and applies the
     * container's justify-content/align-items. 0 = normal block flow. */
    unsigned short flexRow;  /* row group id (0 = none) */
    unsigned short flexItem; /* item group within the row (0 = none) */
    unsigned char flexJustify; /* 0 start, 1 center, 2 end, 3 space-between */
    unsigned char flexCenter;  /* align-items:center */
    const void *flexSrcNode; /* DOM provenance (post-walk flex assignment) */
    DocInline **inlines; /* heap array */
    int inlineCount;
    int inlineCap;
    /* list_item */
    int isOrdered;
    int hasNumber; /* dt/dd have none */
    int number;
    char markerType; /* '1','a','A','i','I' + CSS 'd' disc,'c' circle,
                        's' square,'n' none (layout skips the marker) */
    int depth;
    int dt, dd;
    /* image */
    char *src, *alt, *usemap; /* arena strings */
    double width, height; /* Lua numbers (Lua tostring prints integral as "N") */
    char *caption; /* figure caption merge (image blocks) */
    char *href; /* image inside a link: resolved target (arena, else NULL) */
    void *img; /* decoded bitmap (later phases) */
    /* code_block / math */
    char *text; /* arena */
    char **lines; /* heap array of arena strings (code_block) */
    int lineCount;
    int lineCap;
    /* table */
    DocTable *table; /* arena */
    /* hidden_field / checkbox_field / input_field / input_submit */
    char *name, *value, *placeholder, *label; /* arena strings */
    int fieldWidth, fieldRows;
    int disabled, readonly, required;
    int maxlength; /* -1 = absent (Lua nil) */
    char *formAction; /* arena resolved URL */
    char *formMethod; /* arena "get"/"post" */
    int inert;
    int radio, checked;
    char *inputType; /* arena: "text","textarea","search",... */
    /* Phase 8 resets + multi-select: the WALK-TIME control state, so a
     * tap on type=reset restores what the author shipped (taps mutate
     * the live fields; the defaults ride these captured copies).
     * defaultChecked/defaultSelIndex = the walk's initial truth;
     * defaultValue NULL = the control shipped empty. buttonType carries
     * the button element's type for the tap dispatch (submit/button/
     * reset) — a submit block never exposes its element type otherwise. */
    char *defaultValue; /* arena, or NULL */
    int defaultChecked;
    int defaultSelIndex; /* -1 = unset */
    char *buttonType;    /* arena: submit/button/reset/file/image */
    /* Phase 9 transform lane: translate-only paint offsets (2-D
     * rotation/scale/skew stay the documented 2-D-compositor policy
     * non-feature). px = walk-resolved HALF px; pct = percent of the
     * element's own drawn box at paint time (translateY(-50%) class).
     * memset-0 = absent, so untouched elements shift by nothing. */
    int tfXpx, tfYpx;
    int tfXpct, tfYpct;
    /* Phase 10 animation lane: the animation NAME this block's element
     * resolved to a REGISTERED @keyframes run and COMPLETED (arena
     * string; NULL = no completed animation — the probe asserts both
     * sides: engaged mounts and refused windows). */
    const char *animName;
    /* select_field */
    DocOption **options; /* heap array */
    int optionCount;
    int optionCap;
    int selectedIndex;
    int multiple;
    /* box_open / box_close + R71 ARIA disclosure */
    char *toggleKey; /* arena "d<N>"/"x<N>" or NULL */
    int toggleOpen;
    unsigned char menuButton; /* R71: render as a compact hamburger button */
    /* R71 menu-trigger box metrics, resolved from the trigger's own winning
     * CSS (full CSS px, capped). Layout draws the control at the author's
     * size/padding/radius instead of the old fixed 56x16 pill. */
    unsigned char menuPadL, menuPadR, menuPadT, menuPadB;
    unsigned char menuBorder, menuRadius, menuGap, menuFontBig;
    /* Flex-container content-box padding (full CSS px). Direct children carry
     * it so row layout insets items exactly like the author's box. */
    unsigned char flexPadL, flexPadR, flexPadT, flexPadB;
    /* A disclosure panel the author positioned out of flow (position:
     * absolute/fixed). Dropdown panels are laid out outside the sticky pin
     * span as a right-aligned overlay box; these bits mark the span. */
    unsigned char dropdown, dropdownStart, dropdownEnd;
    /* form widget column hint (select field sizing) */
    int colWidth; /* <select size>/colgroup hint; -1 = absent */
    /* placeholder */
    char *ptag, *plabel, *phref; /* arena strings */
    double pwidth, pheight;
    /* meter */
    double mvalue, mmax, mmin, mlow, mhigh, moptimum;
    /* reader_header (readability) */
    char *host, *readingTime; /* arena strings or NULL */
    /* FORM blocks: source DOM node so host-driven taps/keystrokes can
     * dispatch real events to page handlers via
     * jsbridge_dispatch_element_event (click, input, change, submit).
     * Stamped for DOC_BLOCK_INPUT_SUBMIT at the walker's creation sites
     * and for input fields (textarea included). NULL for every other
     * block kind and when JS never ran / the node came from a snapshot
     * without one. */
    void *srcNode;
} DocBlock;

/* ── Links (port of doc.links[i]) ────────────────────────────────────────── */
typedef struct
{
    char *href; /* arena */
    char *text; /* arena */
    char *target; /* arena or NULL */
    void *srcNode; /* source <a> DOM node (jsbridge click events); may be NULL */
} DocLink;

/* ── Parse options (port of Document.parse's opts table) ─────────────────── */
typedef int (*DocSvgDecoderFn)(const char *xml, int w, int h, void **outBitmap);

typedef struct
{
    /* opts.detailsOpen: positional overrides for keys d1..dN.
     * details[i-1] = 1 → open, 0 → closed. NULL → no overrides. */
    const int *detailsOpen;
    int detailsOpenCount;
    /* R71: positional overrides for the ARIA disclosure widgets the
     * walker numbers x1..xN (an element with aria-expanded +
     * aria-controls). disclosureOpen[i-1] = 1 → the controlled panel
     * renders even though its stylesheet clips it (height:0 /
     * max-height:0 + overflow:hidden — the universal collapse recipe).
     * NULL → the element's own aria-expanded default decides. */
    const int *disclosureOpen;
    int disclosureOpenCount;
    DocSvgDecoderFn svgDecoder; /* NULL → inline svg decode fails (no block) */
    /* adoptHtml: the caller transfers ownership of the NUL-terminated
     * htmlString buffer to the DocParseResult (out->rawHtml points at it;
     * document_free frees it). Saves a full page-body copy — the render
     * task's body becomes the doc's rawHtml instead of being duplicated
     * for the page lifetime (device Reuters 1.24MB body receipt). */
    int adoptHtml;
} DocParseOpts;

/* scriptPolicy: how document_parse handles inline <script> bodies.
 *   DOC_SCRIPT_OFF       skip scripts entirely (JavaScript setting Off)
 *   DOC_SCRIPT_RUN       execute before the walker; document.write re-parsed
 *   DOC_SCRIPT_RUN_KEEP  same, and the JS engine stays attached to the result
 *                        for click events (js_doc_close before document_free) */
typedef enum
{
    DOC_SCRIPT_OFF = 0,
    DOC_SCRIPT_RUN = 1,
    DOC_SCRIPT_RUN_KEEP = 2,
    DOC_SCRIPT_FULL = 3 /* inline + fetched external <script src> files */
} DocScriptPolicy;

/* Phase 10: end-event queue cap (animation lane nodes per walk). */
#define DOC_PEND_END_MAX 16

/* R71: spanning box decorations. A CSS border / gray background on a
 * CONTAINER belongs around the element's whole subtree, not around the
 * empty block the element itself creates (the old per-block attach made
 * cards lose their outline whenever their content lived in child
 * blocks). The walker records one entry per decorated block container;
 * layout draws the frame/shade over the finished block span.
 * startBlock/endBlock are indices into DocParseResult.blocks; an entry
 * that produced no blocks is ignored (startBlock = endBlock = -1). */
#define DOC_BOX_DECOR_MAX 12
typedef struct
{
    int startBlock;
    int endBlock;
    int border; /* CSS border px (0..3) */
    int bgGray; /* 96..215 = dither shade, else -1 */
    int radius; /* rounded-corner px (0 = square) */
} DocBoxDecor;

/* ── Document result ─────────────────────────────────────────────────────── */
typedef struct
{
    char title[256];
    char baseUrl[512];
    char *rawHtml; /* malloc'd copy; caller frees via document_free */
    int isReaderMode;
    int mode; /* MODE_READER / MODE_RAW_HTML */
    DocMetaRefresh metaRefresh;
    /* ── JavaScript integration (jsbridge) ── */
    void *_dom; /* live DomResult while the bridge is attached (freed by
                 * js_doc_close, NOT by document_free) */
    struct JsBridge *_jsbridge; /* opaque; valid while scripts may still run */
    int jsRan;                  /* scripts executed for this page (all kinds) */
    int jsErrors;               /* scripts that failed to compile/run */
    char jsLastError[128];      /* first error ("" when none) */
    /* ── External <script src> files (DOC_SCRIPT_FULL) ── */
    struct JsExtScript_ *extScripts; /* heap array; body owned here */
    int extScriptCount;              /* unique src= URLs stored */
    void *_extArena;                 /* jsext scratch arena (document.c frees) */
    /* ── External <link rel="stylesheet"> sheets (Phase 1.5) ──
     * Bodies fetched by the render task BEFORE parsing (pre-parse stage);
     * the engine's rule text points into these buffers, so they live as
     * long as the doc and are freed by document_free (with the engine,
     * in that order). */
    char **cssExtBody;   /* heap array: cssExtN malloc'd bodies (each owned) */
    size_t *cssExtLen;   /* parallel body lengths */
    int cssExtN;         /* external sheets fetched (0 = none) */
    /* <noscript> handling policy for this page (parsed and every rewalk):
     * 0 = render the fallback content (JavaScript setting Off, reader mode);
     * 1 = suppress it (an engine ran this page — browser parity: the
     * scripting flag is on, so <noscript> is invisible). Set by
     * document_parse_ex after the script policy is known; document_rewalk
     * copies it onto the walker (the browser never sets it by hand). */
    int suppressNoscript;
    /* Sticky: set when the walker meets a <script> tag (inline or src).
     * A script page that walks to zero blocks is still loading (its
     * content arrives when the code runs), so the walker emits a
     * "Loading..." placeholder instead of "(Empty Web Page)". Never
     * cleared: memset-0 at doc creation, survives rewalks like the DOM. */
    int sawScripts;
    /* ── CSS engine (html/css.h; roadmap #2) ──
     * _css owns the parsed <style> rules for this page; heap-allocated,
     * freed by document_free. NULL when the page has no stylesheets.
     * Survives across document_rewalk (rules are re-applied to JS-mutated
     * DOMs; sheet text points into rawHtml which lives as long as the doc). */
    void *_css; /* opaque CssEngine* (html/css.h) */
    /* Walker output */
    DocBlock **blocks; /* heap array */
    int blockCount;
    int blockCap;
    DocLink **links; /* heap array */
    int linkCount;
    int linkCap;
    DocMap **maps; /* heap array */
    int mapCount;
    int mapCap;
    DocDatalist **datalists; /* heap array */
    int datalistCount;
    int datalistCap;
    /* The reference throws (e.g. bare <li>: arithmetic on nil ctx.start) and
     * CometBrowser propagates the error. C sets parseError=1 and stops.
     * Callers surface it as the reference's error outcome. */
    int parseError;
    /* Reader-mode extras (Lua doc.wordCount / doc.readingTime). */
    int readingTimeWords;
    char *readingTimeStr; /* arena string (readability arena) */
    void *_arena; /* walker string/object arena (document.c internal) */
    /* Resumable-walk state (document.c internal, opaque): non-NULL only
     * while a sliced walk is parked between frames. document_free aborts
     * it; the browser's render task resumes it via document_parse_resume. */
    void *_walkState;

    /* ── Phase 10: end-event queue (animation lane) ──
     * Nodes whose declared CSS animation COMPLETED during the just-run
     * walk (immediate-fire contract: the flat renderer has no animation
     * timeline — a declared run finishes at the walk that first resolves
     * it). Filled by the walker (walk_anim_engage), drained + cleared by
     * the browser at render completion, reset at every walk start so the
     * queue reflects exactly the current pass. Per-(node,name) dedupe
     * persists on the DOM (dom_anim_end_report) across walks. */
    struct
    {
        void *node; /* live DomNode that completed the run */
    } pendEnd[DOC_PEND_END_MAX];
    int pendEndCount;
    /* R66: the walk options must survive a JS-mutation rewalk. Before this,
     * document_rewalk ran with w.opts=NULL, so inline SVG logos disappeared
     * after the first DOM mutation and details toggles lost their state.
     * The decoder pointer and the browser's details-open array are stable
     * for the page's lifetime (main.c owns the array). */
    DocSvgDecoderFn svgDecoder;
    const int *detailsOpen;
    int detailsOpenCount;
    /* R71 disclosure-widget overrides (same contract as opts). */
    const int *disclosureOpen;
    int disclosureOpenCount;
    /* R71 spanning box decorations (see DocBoxDecor above). */
    DocBoxDecor decor[DOC_BOX_DECOR_MAX];
    int decorCount;
} DocParseResult;

/* Inline <script> execution (attach/flush/close + click dispatch) lives in
 * html/jsbridge.h, driven by document_parse_ex and the browser. */

/* ── Parsing helpers (exact ports; see document.c for quirk notes) ──────── */

/* parseStyle(styleStr): "k:v;k2:v2" → lowercase keys/values, trimmed values.
 * Empty/NULL → empty map. Unparseable junk between ';' is SKIPPED (continue). */
void doc_parse_style(const char *styleStr, DocStyle *out);

/* parseAlign(attrs): align attr overridden by style text-align; only
 * center/right/left (case-insensitive) are valid; NULL/empty attrs → NULL.
 * Returns the lowercased value (static storage; valid until next call). */
const char *doc_parse_align(const DocStyleEntry *attrs, int attrCount);

/* isDisplayNone(attrs): hidden/popover attr (any value incl. PLUTO_TOK_ATTR_TRUE)
 * or style display/visibility containing "none"/"hidden". */
int doc_is_display_none(const DocStyleEntry *attrs, int attrCount);

/* isInvertedStyle(attrs): color white/#fff/#FFFFxx or background(-color)
 * black/#000/#000000 (patterns applied to the LOWERCASED style value). */
int doc_is_inverted_style(const DocStyleEntry *attrs, int attrCount);

/* parseBoxSpacing(attrs): margin/padding shorthand + longhand extraction.
 * QUIRK (Lua parity): component values must parse as PURE numbers
 * ("10px" fails tonumber → treated as absent); values are halved (floor). */
DocBoxSpacing doc_parse_box_spacing(const DocStyleEntry *attrs, int attrCount);

/* concatNodeText(node): concatenated text of all descendants. Writes into
 * buf (NUL-terminated); returns needed length (like snprintf). */
size_t doc_concat_node_text(const DomNode *node, char *buf, size_t cap);

/* validHref(raw): "" / #x / javascript: / data: → 0; else 1. NULL → 0. */
int doc_valid_href(const char *raw);

/* serializeSvgNode(n): inline SVG subtree → XML string for the rasterizer.
 * Attr values get '"'< escaped; text nodes go through Entities.encode.
 * Returns malloc'd string (caller frees with PLUTO_FREE/pd realloc), or NULL. */
char *doc_serialize_svg_node(const DomNode *n);

/* ── Document.parse ─────────────────────────────────────────────────────────
 * mode: MODE_READER or MODE_RAW_HTML (constants.h). opts may be NULL.
 * Returns 0 ok (check out->parseError), -1 alloc failure. */
int document_parse(const char *htmlString, const char *baseUrl, int mode,
                   const DocParseOpts *opts, DocParseResult *out);

/* Full-control variant: scriptPolicy selects inline <script> handling
 * (DOC_SCRIPT_OFF keeps the historical skip-scripts path); outBridge (may be
 * NULL) receives the JS bridge when one was attached (DOC_SCRIPT_RUN_KEEP). */
int document_parse_ex(const char *htmlString, const char *baseUrl, int mode,
                      const DocParseOpts *opts, DocScriptPolicy scriptPolicy,
                      struct JsBridge **outBridge, DocParseResult *out);

/* True when the block text matches an SPA no-JS warning that desktop
 * browsers hide when scripting is enabled ("You need to enable JavaScript
 * to run this app." and its React/webpack variants). Case/space/punct
 * insensitive, "JavaScript" optional, ≤96 chars. Used to suppress
 * <noscript> fallback text for pages that ran an engine
 * (DocParseResult.suppressNoscript). Never touches non-noscript content. */
int doc_is_noscript_warning(const char *text);

/* R29: True when a walk output is an EMPTY RENDER — the generic
 * "(Empty Web Page)" placeholder paragraph (or the "Loading..."
 * placeholder a script page shows while its content has not arrived yet)
 * and nothing else. pluto_snap_save refuses to persist such a doc and
 * pluto_snap_load treats a persisted one as a MISS, so a server bot-block,
 * a failed first visit, or a mid-load snapshot can never be immortalized
 * by the fast-path cache. Site-agnostic by construction. */
int doc_is_empty_render(const DocParseResult *doc);

/* Re-run ONLY the element walker over the (possibly JS-mutated) live DOM
 * kept by doc->_dom: blocks/links are rebuilt without re-parsing HTML or
 * re-running scripts. Layout must be cleared first (it borrows strings).
 * Returns 0 ok, -1 alloc failure. */
int document_rewalk(DocParseResult *doc);

/* ── Resumable walk (device watchdog contract) ─────────────────────────────
 * document_parse_ex_sliced / document_rewalk_sliced run the same pipeline
 * as their synchronous twins, but the element walk is bounded by budgetMs
 * per call. A return of 1 means "parked: call document_parse_resume next
 * frame"; 0 means complete; -1 means failure. The parked state lives in
 * doc->_walkState and is aborted by document_free. budgetMs 0 = run to
 * completion (the historical synchronous behavior; the simulator path). */
int document_parse_ex_sliced(const char *htmlString, const char *baseUrl,
                             int mode, const DocParseOpts *opts,
                             DocScriptPolicy scriptPolicy,
                             DocParseResult *out, unsigned long budgetMs);
int document_rewalk_sliced(DocParseResult *doc, unsigned long budgetMs);
int document_parse_resume(DocParseResult *doc, unsigned long budgetMs);

/* ── SW6 snapshot support: the walker's string/object arena ───────────────
 * pluto_snap allocates restored strings/blocks from a fresh arena with
 * these calls, and document_free frees the arena exactly as it does for a
 * parsed doc (restored docs mirror the walker's memory model: arena
 * structs+strings, heap pointer arrays). The struct is defined here (not
 * opaque) so the snapshot module can allocate one on its own; layout of
 * the chunk list is otherwise private to document.c. */
typedef struct DocChunk DocChunk;

typedef struct DocArena
{
    struct DocChunk *head;
} DocArena;

/* Allocate n bytes (8-byte aligned) from the arena. NULL on OOM. */
void *doc_arena_alloc(DocArena *a, size_t n);

/* Free every chunk of the arena (does not free the DocArena itself). */
void doc_arena_free_all(DocArena *a);

/* Allocate a zero-initialized (n+1)-byte arena string. NULL on OOM. */
char *doc_arena_alloc_str(DocArena *a, size_t n);

/* Duplicate str into the arena ("") never NULL. NULL on OOM. */
char *doc_arena_strdup(DocArena *a, const char *str);

void document_free(DocParseResult *doc);
/* ── Phase 10: transition lane — is this live node's winning style armed
 * with a transition (shorthand/longhand whose value is not `none`)? The
 * browser consults it at end-event drain time for each node a script
 * mutated. 1 = armed, 0 = inert. */
int document_node_transition_hot(DocParseResult *doc, const void *node);

#endif /* PLUTO_DOCUMENT_H */
