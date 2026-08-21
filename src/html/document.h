#ifndef PLUTO_HTML_DOCUMENT_H
#define PLUTO_HTML_DOCUMENT_H

#include <stddef.h>

struct PlaydateAPI;

/* ── Inline model ─────────────────────────────────────────────────────── */

enum {
    DIT_TEXT = 0,
    DIT_BR   = 1,
    DIT_WBR  = 2
};

typedef struct DocInline {
    int type;
    char* text;          /* TEXT: owned */
    size_t textLen;
    unsigned char bold, italic, underline, code, small, big;
    unsigned char sub, sup, mark, strike, invert, inert;
    char* href;          /* owned; NULL when not a link */
    long anchorIndex;    /* -1 when none */
} DocInline;

/* ── Block model ───────────────────────────────────────────────────────── */

enum {
    DB_PARAGRAPH = 0,
    DB_HEADING,
    DB_BLOCKQUOTE,
    DB_LIST_ITEM,
    DB_CODE_BLOCK,
    DB_HR,
    DB_IMAGE,
    DB_TABLE,
    DB_INPUT_FIELD,
    DB_CHECKBOX_FIELD,
    DB_INPUT_SUBMIT,
    DB_SELECT_FIELD,
    DB_BOX_OPEN,
    DB_BOX_CLOSE,
    DB_PLACEHOLDER,
    DB_METER,
    DB_MATH
};

/* tables */

typedef struct DocTableCell {
    DocInline* inlines;
    size_t nInlines;
    size_t capInlines;
    int isHeader;        /* <th> */
    int colspan;         /* >= 1 */
    int rowspan;         /* >= 1 */
    char* abbr;          /* owned: abbr || title || "" */
    const char* align;   /* static */
} DocTableCell;

typedef struct DocTableRow {
    DocTableCell* cells;
    size_t nCells;
    size_t capCells;
} DocTableRow;

/* selects */

typedef struct DocSelectOpt {
    char* text;          /* owned */
    char* value;         /* owned */
    int selected;
    int disabled;
    int group;           /* optgroup header entry */
} DocSelectOpt;

/* image-map regions (<map>/<area>) */

typedef struct DocAreaRegion {
    char* shape;         /* owned: "rect" | ... */
    int* coords;         /* owned */
    size_t nCoords;
    char* href;          /* owned resolved URL or NULL */
    char* alt;           /* owned */
} DocAreaRegion;

typedef struct DocMap {
    char* name;          /* owned */
    DocAreaRegion* regions;
    size_t nRegions;
    size_t capRegions;
} DocMap;

/* <datalist id> suggestion metadata */

typedef struct DocDatalistOpt {
    char* text;
    char* value;
} DocDatalistOpt;

typedef struct DocDatalist {
    char* id;            /* owned */
    DocDatalistOpt* opts;
    size_t nOpts;
    size_t capOpts;
} DocDatalist;

typedef struct DocBlock {
    int type;
    /* inline carriers: paragraph / heading / blockquote / list_item */
    DocInline* inlines;
    size_t nInlines;
    size_t capInlines;

    const char* align;   /* NULL | "left" | "center" | "right" (static) */
    int spacingTop, spacingBottom, indent;
    int invert;

    /* heading */
    int level;

    /* list_item */
    int isOrdered;
    int number;
    int depth;
    const char* markerType;  /* "1" | "a" | "A" | "i" | "I" (static) */
    unsigned char dtFlag, ddFlag;

    /* code_block */
    char* codeText;          /* owned, full pre buffer */
    char** lines;            /* owned, split on \r?\n */
    size_t nLines;

    /* image (DB_IMAGE) */
    char* src;               /* owned resolved URL */
    char* alt;               /* owned */
    char* caption;           /* owned; figure caption override */
    char* imgHref;           /* owned */
    char* usemap;            /* owned */
    int width, height;
    int imgInert;
    int imgIsSvg;            /* inline <svg> serialized to XML */
    char* svgXml;            /* owned serialized subtree (P24 rasterizes) */

    /* table (DB_TABLE) */
    DocTableRow* rows;
    size_t nRows;
    size_t capRows;
    char* tableCaption;      /* owned collapsed+trimmed or NULL */
    int tableBorder;         /* border attr present and != "0" */
    char* tableWidth;        /* owned raw width attr or NULL */

    /* input_field / checkbox_field (shared flat fields) */
    char* inputType;         /* owned lowercased type string */
    char* inName;            /* owned */
    char* inValue;           /* owned */
    char* placeholder;       /* owned */
    char* checkboxLabel;     /* owned */
    int fieldWidth;          /* -1 when unset */
    int fieldRows;           /* -1 when unset */
    int maxlength;           /* -1 when unset */
    unsigned char disabledFlag, readonlyFlag, requiredFlag;
    unsigned char radioFlag, checkedFlag;
    unsigned char blockInert; /* input-family: inert || disabled */

    /* input_submit */
    char* submitLabel;       /* owned */

    /* form plumbing shared by field/checkbox/submit/select */
    char* formAction;        /* owned resolved or NULL */
    char* formMethod;        /* owned lowercased ("get" default) */

    /* select_field */
    DocSelectOpt* options;
    size_t nOptions;
    size_t capOptions;
    int selectedIndex;
    unsigned char multipleFlag;

    /* box_open / box_close (fieldset / details / dialog) */
    char* boxLabel;          /* owned ("" allowed) */
    char* toggleKey;         /* owned "dN" or NULL */
    int toggleOpen;

    /* placeholder */
    char* phTag;             /* owned tag name */
    char* phHref;            /* owned resolved URL or NULL */

    /* meter */
    double mValue, mMax, mMin, mLow, mHigh, mOptimum;
} DocBlock;

/* ── Links & document ─────────────────────────────────────────────────── */

typedef struct DocLink {
    char* href;
    char* text;
    char* target;        /* owned or NULL */
} DocLink;

typedef struct DocDocument {
    char* title;
    char* baseUrl;
    char* rawHtml;
    int isReaderMode;

    DocBlock* blocks;
    size_t nBlocks;
    size_t capBlocks;

    DocLink* links;
    size_t nLinks;
    size_t capLinks;

    int hasMetaRefresh;  /* body-walker result preferred over head scan */
    double metaDelay;
    char* metaUrl;       /* owned resolved URL or NULL */

    DocMap* maps;        /* <map name> -> area regions */
    size_t nMaps;
    size_t capMaps;

    DocDatalist* datalists;
    size_t nDatalists;
    size_t capDatalists;
} DocDocument;

#define DOC_ALIGN_NONE    ((const char*)NULL)
/* align values are the static strings "left" / "center" / "right" */

/* Parse-time overrides. <details> elements get deterministic keys
 * "d1","d2",... in walk order; an override forces a key open or closed. */
typedef struct {
    const char* key;     /* "d1" style */
    int open;
} DocDetailsOverride;

typedef struct {
    const DocDetailsOverride* detailsOverrides; /* NULL when unused */
    size_t nOverrides;
} DocParseOpts;

void doc_init(struct PlaydateAPI* pd);

/* HTML-mode parse (PLUTO_MODE_RAW_HTML). Reader mode is ported in P12 and
 * returns NULL for PLUTO_MODE_READER for now. NULL on OOM / reader mode. */
DocDocument* doc_parse_opts(const char* htmlString, const char* baseUrlStr,
                            int mode, const DocParseOpts* opts);
#define doc_parse(html, base, m) \
    doc_parse_opts((html), (base), (m), (const DocParseOpts*)NULL)

void doc_free(DocDocument* d);

#endif
