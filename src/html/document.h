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

/* ── Block model (P10 subset; P11 extends the enum) ───────────────────── */

enum {
    DB_PARAGRAPH = 0,
    DB_HEADING,
    DB_BLOCKQUOTE,
    DB_LIST_ITEM,
    DB_CODE_BLOCK,
    DB_HR,
    DB_IMAGE
};

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

    /* image */
    char* src;               /* owned resolved URL */
    char* alt;               /* owned */
    char* caption;           /* owned; figure caption override */
    char* imgHref;           /* owned */
    char* usemap;            /* owned */
    int width, height;
    int imgInert;
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
} DocDocument;

#define DOC_ALIGN_NONE    ((const char*)NULL)
/* align values are the static strings "left" / "center" / "right" */

void doc_init(struct PlaydateAPI* pd);

/* HTML-mode parse (PLUTO_MODE_RAW_HTML). Reader mode is ported in P12 and
 * returns NULL for PLUTO_MODE_READER for now. NULL on OOM / reader mode. */
DocDocument* doc_parse(const char* htmlString, const char* baseUrlStr, int mode);

void doc_free(DocDocument* d);

#endif
