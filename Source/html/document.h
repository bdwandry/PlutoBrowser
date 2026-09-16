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
} DocBoxSpacing;

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
    int hasSpacing; /* Lua parity: element blocks carry spacing keys (printed
                     * even when 0); implicit stray-text paragraphs don't. */
    int invert;
    DocInline **inlines; /* heap array */
    int inlineCount;
    int inlineCap;
    /* list_item */
    int isOrdered;
    int hasNumber; /* dt/dd have none */
    int number;
    char markerType; /* '1','a','A','i','I' */
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
    /* select_field */
    DocOption **options; /* heap array */
    int optionCount;
    int optionCap;
    int selectedIndex;
    int multiple;
    /* box_open / box_close */
    char *toggleKey; /* arena "d<N>" or NULL */
    int toggleOpen;
    /* form widget column hint (select field sizing) */
    int colWidth; /* <select size>/colgroup hint; -1 = absent */
    /* placeholder */
    char *ptag, *plabel, *phref; /* arena strings */
    double pwidth, pheight;
    /* meter */
    double mvalue, mmax, mmin, mlow, mhigh, moptimum;
    /* reader_header (readability) */
    char *host, *readingTime; /* arena strings or NULL */
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
    DocSvgDecoderFn svgDecoder; /* NULL → inline svg decode fails (no block) */
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

/* Re-run ONLY the element walker over the (possibly JS-mutated) live DOM
 * kept by doc->_dom: blocks/links are rebuilt without re-parsing HTML or
 * re-running scripts. Layout must be cleared first (it borrows strings).
 * Returns 0 ok, -1 alloc failure. */
int document_rewalk(DocParseResult *doc);

void document_free(DocParseResult *doc);

#endif /* PLUTO_DOCUMENT_H */
