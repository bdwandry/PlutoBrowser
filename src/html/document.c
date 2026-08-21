#include "html/document.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "html/entities.h"
#include "core/url.h"
#include "core/tasks.h"
#include "core/constants.h"
#include "util/mem.h"
#include "util/strbuf.h"
#include "core/logger.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DOC_MAX_BLOCKS 1200
#define DOC_MAX_INLINES 900

/* ── small string helpers ─────────────────────────────────────────────── */

static int d_contains(const char* hay, const char* needle) {
    return hay != NULL && strstr(hay, needle) != NULL;
}

static int d_starts_ci(const char* s, const char* ciLower) {
    return s != NULL && strncasecmp(s, ciLower, strlen(ciLower)) == 0;
}

/* Lua tonumber(): full-string numeric parse */
static double d_tonum(const char* v, int* ok) {
    *ok = 0;
    if (v == NULL) return 0.0;
    while (*v != '\0' && isspace((unsigned char)*v)) v++;
    char* end = NULL;
    double n = strtod(v, &end);
    if (end == v) return 0.0;
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0.0;
    *ok = 1;
    return n;
}

/* CSS length -> floor(n / 2) after '%' strip; invalid -> 0 */

static int d_css_half(const char* v) {
    int ok = 0;
    double n;
    if (v == NULL) return 0;
    char buf[48];
    size_t j = 0;
    for (size_t i = 0; v[i] != '\0' && j + 1 < sizeof(buf); i++)
        if (v[i] != '%') buf[j++] = v[i];
    buf[j] = '\0';
    n = d_tonum(buf, &ok);
    if (!ok) return 0;
    return (int)floor(n / 2.0);
}

/* ── attribute accessors ──────────────────────────────────────────────── */

static const char* da_get(StrMap* attrs, const char* key) {
    if (attrs == NULL) return NULL;
    void* v = sm_get(attrs, key);
    return (v == NULL || v == HT_ATTR_TRUE) ? NULL : (const char*)v;
}

static int da_has(StrMap* attrs, const char* key) {
    return attrs != NULL && sm_get(attrs, key) != NULL;
}

/* ── parseStyle: keys/values lowercased + trimmed; LAST duplicate wins ── */

typedef struct {
    char key[40];
    char val[80];
} DStyleEntry;

typedef struct {
    DStyleEntry e[16];
    int n;
} DStyle;

static void d_parse_style(DStyle* out, const char* styleStr) {
    memset(out, 0, sizeof(*out));
    if (styleStr == NULL || styleStr[0] == '\0') return;
    const char* p = styleStr;
    while (*p != '\0') {
        const char* ks = p;
        while (*p != '\0' && (isalnum((unsigned char)*p) || *p == '-')) p++;
        size_t klen = (size_t)(p - ks);
        const char* q = p;
        while (*q != '\0' && isspace((unsigned char)*q)) q++;
        if (klen == 0 || *q != ':') {
            if (*p == '\0') break;
            p = (*p == ';') ? p + 1 : q;
            if (*p == ':') p++;
            while (*p != '\0' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        p = q + 1;
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        const char* vs = p;
        while (*p != '\0' && *p != ';') p++;
        size_t vlen = (size_t)(p - vs);
        if (*p == ';') p++;
        if (klen == 0 || vlen == 0) continue;

        char key[40], val[80];
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, ks, klen);
        key[klen] = '\0';
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        memcpy(val, vs, vlen);
        val[vlen] = '\0';
        {
            size_t a = 0, len = vlen;
            while (a < len && isspace((unsigned char)val[a])) a++;
            while (len > a && isspace((unsigned char)val[len - 1])) len--;
            memmove(val, val + a, len - a);
            val[len - a] = '\0';
            for (char* c = val; *c; c++) *c = (char)tolower((unsigned char)*c);
        }

        int found = 0;
        for (int i = 0; i < out->n; i++) {
            if (strcmp(out->e[i].key, key) == 0) {
                snprintf(out->e[i].val, sizeof(out->e[0].val), "%s", val);
                found = 1;
                break;
            }
        }
        if (!found && out->n < 16) {
            snprintf(out->e[out->n].key, sizeof(out->e[0].key), "%s", key);
            snprintf(out->e[out->n].val, sizeof(out->e[0].val), "%s", val);
            out->n++;
        }
    }
}

static const char* d_style_get(const DStyle* st, const char* key) {
    for (int i = 0; i < st->n; i++)
        if (strcmp(st->e[i].key, key) == 0) return st->e[i].val;
    return NULL;
}

/* ── align / visibility / inversion ───────────────────────────────────── */

static const char* d_parse_align(StrMap* attrs) {
    if (attrs == NULL) return NULL;
    const char* a = da_get(attrs, "align");
    DStyle st;
    d_parse_style(&st, da_get(attrs, "style"));
    const char* ta = d_style_get(&st, "text-align");
    if (ta != NULL) a = ta;
    if (a == NULL) return NULL;
    if (strcasecmp(a, "center") == 0) return "center";
    if (strcasecmp(a, "right") == 0) return "right";
    if (strcasecmp(a, "left") == 0) return "left";
    return NULL;
}

static int d_is_display_none(StrMap* attrs) {
    if (attrs == NULL) return 0;
    if (da_has(attrs, "hidden")) return 1;
    if (da_has(attrs, "popover")) return 1;
    DStyle st;
    d_parse_style(&st, da_get(attrs, "style"));
    const char* disp = d_style_get(&st, "display");
    if (disp != NULL && strstr(disp, "none") != NULL) return 1;
    const char* vis = d_style_get(&st, "visibility");
    if (vis != NULL && strstr(vis, "hidden") != NULL) return 1;
    return 0;
}

static int d_is_inverted_style(StrMap* attrs) {
    if (attrs == NULL) return 0;
    DStyle st;
    d_parse_style(&st, da_get(attrs, "style"));
    const char* c = d_style_get(&st, "color");
    const char* bg = d_style_get(&st, "background-color");
    if (bg == NULL) bg = d_style_get(&st, "background");
    if (c != NULL && (strstr(c, "white") != NULL ||
                      strstr(c, "#fff") != NULL || strstr(c, "#FFFF") != NULL))
        return 1;
    if (bg != NULL && (strstr(bg, "black") != NULL ||
                       strstr(bg, "#000") != NULL))
        return 1;
    return 0;
}

/* ── box spacing ──────────────────────────────────────────────────────── */

typedef struct {
    int top, bottom, left, right;
} DBox;

static size_t d_split_ws_comma(const char* s, char parts[][24], size_t max) {
    size_t n = 0;
    const char* p = s;
    while (*p != '\0' && n < max) {
        while (*p != '\0' && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == '\0') break;
        size_t len = 0;
        while (p[len] != '\0' &&
               !(isspace((unsigned char)p[len]) || p[len] == ','))
            len++;
        if (len > 23) len = 23;
        memcpy(parts[n], p, len);
        parts[n][len] = '\0';
        n++;
        p += len;
    }
    return n;
}

static void d_parse_box_spacing(StrMap* attrs, DBox* out) {
    out->top = out->bottom = out->left = out->right = 0;
    if (attrs == NULL) return;
    DStyle st;
    d_parse_style(&st, da_get(attrs, "style"));

    const char* m = d_style_get(&st, "margin");
    if (m != NULL) {
        char parts[8][24];
        size_t np = d_split_ws_comma(m, parts, 8);
        int haveT = 0, haveB = 0, haveL = 0, haveR = 0;
        int vT = 0, vB = 0, vL = 0, vR = 0;
        if (np == 1) {
            vT = vR = vB = vL = d_css_half(parts[0]);
            haveT = haveR = haveB = haveL = 1;
        } else if (np == 2) {
            vT = vB = d_css_half(parts[0]); haveT = haveB = 1;
            vR = vL = d_css_half(parts[1]); haveR = haveL = 1;
        } else if (np == 3) {
            vT = d_css_half(parts[0]); haveT = 1;
            vR = vL = d_css_half(parts[1]); haveR = haveL = 1;
            vB = d_css_half(parts[2]); haveB = 1;
        } else if (np >= 4) {
            vT = d_css_half(parts[0]); haveT = 1;
            vR = d_css_half(parts[1]); haveR = 1;
            vB = d_css_half(parts[2]); haveB = 1;
            vL = d_css_half(parts[3]); haveL = 1;
        }
        if (haveT) out->top = vT;
        if (haveB) out->bottom = vB;
        if (haveL) out->left = vL;
        if (haveR) out->right = vR;
    } else {
        const char* mt = d_style_get(&st, "margin-top");
        if (mt != NULL) out->top = d_css_half(mt);
        const char* mb = d_style_get(&st, "margin-bottom");
        if (mb != NULL) out->bottom = d_css_half(mb);
        const char* ml = d_style_get(&st, "margin-left");
        if (ml != NULL) out->left = d_css_half(ml);
        const char* mr = d_style_get(&st, "margin-right");
        if (mr != NULL) out->right = d_css_half(mr);
    }

    const char* pad = d_style_get(&st, "padding");
    if (pad != NULL) {
        char parts[8][24];
        size_t np = d_split_ws_comma(pad, parts, 8);
        if (np == 1) out->left += d_css_half(parts[0]);
        else if (np == 2) out->left += d_css_half(parts[1]);
        else if (np >= 4) out->left += d_css_half(parts[3]);
    } else {
        const char* pl = d_style_get(&st, "padding-left");
        if (pl != NULL) out->left += d_css_half(pl);
    }
}

/* ── node text concat + svg serialization ─────────────────────────────── */

static void d_concat_into(DomNode* node, StrBuf* sb);

static char* d_concat_node_text(DomNode* node) {
    StrBuf sb;
    sb_init(&sb);
    d_concat_into(node, &sb);
    return sb_detach(&sb);
}

static void d_concat_into(DomNode* node, StrBuf* sb) {
    if (node == NULL) return;
    if (node->kind == DOM_TEXT) {
        sb_append_str(sb, node->text ? node->text : "");
    } else if (node->kind == DOM_ELEMENT) {
        for (size_t i = 0; i < node->nChildren; i++)
            d_concat_into(node->children[i], sb);
    }
}

static void d_svg_escape_value(const char* v, StrBuf* out) {
    for (const char* p = v ? v : ""; *p != '\0'; p++) {
        if (*p == '"') sb_append_str(out, "&quot;");
        else if (*p == '<') sb_append_str(out, "&lt;");
        else sb_append_char(out, *p);
    }
}

static void d_serialize_svg_into(DomNode* n, StrBuf* out);

static char* d_serialize_svg_node(DomNode* n) {
    StrBuf sb;
    sb_init(&sb);
    d_serialize_svg_into(n, &sb);
    return sb_detach(&sb);
}

static void d_attr_iter_cb(const char* key, void* value, void* ud) {
    StrBuf* out = (StrBuf*)ud;
    sb_append_char(out, ' ');
    sb_append_str(out, key);
    sb_append_str(out, "=\"");
    d_svg_escape_value((value == HT_ATTR_TRUE) ? "" : (const char*)value, out);
    sb_append_char(out, '"');
}

static void d_serialize_svg_into(DomNode* n, StrBuf* out) {
    if (n == NULL) return;
    if (n->kind == DOM_TEXT) {
        size_t encLen = 0;
        char* enc = entities_encode(n->text ? n->text : "",
                                    n->textLen, &encLen);
        if (enc != NULL) {
            sb_append(out, enc, encLen);
            pluto_free(enc);
        }
        return;
    }
    if (n->kind != DOM_ELEMENT) return;
    sb_append_char(out, '<');
    sb_append_str(out, n->tag);
    sm_foreach(n->attrs, d_attr_iter_cb, out);
    if (n->nChildren == 0) {
        sb_append_str(out, "/>");
        return;
    }
    sb_append_char(out, '>');
    for (size_t i = 0; i < n->nChildren; i++)
        d_serialize_svg_into(n->children[i], out);
    sb_append_str(out, "</");
    sb_append_str(out, n->tag);
    sb_append_char(out, '>');
}

static int d_valid_href(const char* raw) {
    if (raw == NULL || raw[0] == '\0') return 0;
    if (raw[0] == '#') return 0;
    if (d_starts_ci(raw, "javascript:")) return 0;
    if (d_starts_ci(raw, "data:")) return 0;
    return 1;
}

/* ── meta refresh content parsing ─────────────────────────────────────── */
/* "^(%d+%.?%d*)%s*;%s*[Uu][Rr][Ll]=%s*(.+)$" or "^(%d+\.?%d*)%s*$"       */

static int d_parse_meta_content(const char* content, double* delayOut,
                                char** urlOut) {
    *urlOut = NULL;
    if (content == NULL) return 0;
    const char* p = content;
    if (!isdigit((unsigned char)*p)) return 0;
    const char* ds = p;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) p++;
    }
    char dbuf[24];
    size_t dl = (size_t)(p - ds);
    if (dl >= sizeof(dbuf)) dl = sizeof(dbuf) - 1;
    memcpy(dbuf, ds, dl);
    dbuf[dl] = '\0';

    if (*p == ';') {
        p++;
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if ((*p == 'u' || *p == 'U') && strncasecmp(p, "url", 3) == 0 &&
            p[3] == '=') {
            p += 4;
            while (*p != '\0' && isspace((unsigned char)*p)) p++;
            const char* us = p;
            while (*p != '\0') p++;          /* (.+)$ to end */
            size_t ul = (size_t)(p - us);
            while (ul > 0 && isspace((unsigned char)us[ul - 1])) ul--;
            if (ul == 0) {
                *delayOut = strtod(dbuf, NULL);
                return 1;
            }
            char* u = pluto_strndup(us, ul);
            if (u == NULL) return 0;
            *delayOut = strtod(dbuf, NULL);
            *urlOut = u;
            return 1;
        }
        return 0;  /* ';' but not url= : neither Lua pattern matches */
    }
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    if (*p != '\0') return 0;
    *delayOut = strtod(dbuf, NULL);
    return 1;
}

/* ── forward declarations (mutually recursive walker) ─────────────────── */

typedef struct DocState DocState;
static void d_walk(DocState* st, DomNode* node);
static void d_walk_children(DocState* st, DomNode* node);

/* ── inline / block plumbing ──────────────────────────────────────────── */

typedef struct {
    unsigned char bold, italic, underline, code, small, big;
    unsigned char sub, sup, mark, strike, invert;
} DFlags;

typedef struct {
    int ordered;
    int start;
    int reversed;
    const char* markerType;  /* static */
    int count;
    int depth;
} DListCtx;

/* progressive <td>/<th> build target (single slot; nested tables inside
 * cells are dropped by parity with the Lua walker) */
typedef struct {
    DocInline* inlines;
    size_t nInlines;
    size_t capInlines;
    int isHeader;
    const char* align;
} DCellBuild;

/* progressive <table> build target */
typedef struct {
    DocTableRow* rows;
    size_t nRows;
    size_t capRows;
} DTableBuild;

struct DocState {
    DocDocument* doc;
    const char* baseUrl;

    DFlags f;

    char* currentHref;         /* owned */
    long currentAnchorIndex;   /* -1 = none */
    StrBuf linkText;
    long anchorCounter;

    int inPre;
    StrBuf preBuffer;

    int inTextarea;
    char* textareaName;        /* owned */
    StrBuf textareaBuffer;

    int inMath;
    StrBuf mathParts;

    DCellBuild* cell;          /* non-NULL while walking a td/th subtree */
    int cellFirstText;
    DTableBuild* tbl;          /* non-NULL between <table> open and close */

    int figureCaptionDone;
    StrBuf figCaption;
    DocBlock* figureImage;
    int figureActive;

    int inert;
    int disabledDepth;
    int truncated;

    DListCtx* listCtx;
    int dlDepth;

    DocBlock* currentBlock;

    char* formAction;          /* owned resolved URL or NULL */
    char* formMethod;          /* owned lowercased or NULL */

    int detailsIndex;
    const DocParseOpts* opts;

    int hasMetaRefresh;
    double metaDelay;
    char* metaUrl;             /* owned */
};

static void di_free(DocInline* in) {
    pluto_free(in->text);
    pluto_free(in->href);
}

/* frees a standalone table row (cells + their inlines) */
static void dtr_free(DocTableRow* r) {
    for (size_t i = 0; i < r->nCells; i++) {
        DocTableCell* c = &r->cells[i];
        for (size_t j = 0; j < c->nInlines; j++) di_free(&c->inlines[j]);
        pluto_free(c->inlines);
        pluto_free(c->abbr);
    }
    pluto_free(r->cells);
    r->cells = NULL;
    r->nCells = r->capCells = 0;
}

/* frees every owned field of a block EXCEPT the struct itself
 * (blocks live inline inside DocDocument.blocks) */
static void db_free_fields(DocBlock* b) {
    for (size_t i = 0; i < b->nInlines; i++) di_free(&b->inlines[i]);
    pluto_free(b->inlines);
    b->inlines = NULL;
    b->nInlines = b->capInlines = 0;

    /* frees a standalone table row (cells + their inlines) */
    for (size_t i = 0; i < b->nRows; i++) {
        DocTableRow* r = &b->rows[i];
        for (size_t j = 0; j < r->nCells; j++) {
            DocTableCell* c2 = &r->cells[j];
            for (size_t k = 0; k < c2->nInlines; k++)
                di_free(&c2->inlines[k]);
            pluto_free(c2->inlines);
            pluto_free(c2->abbr);
        }
        pluto_free(r->cells);
    }
    pluto_free(b->rows);
    b->rows = NULL;
    b->nRows = b->capRows = 0;

    pluto_free(b->codeText);
    for (size_t i = 0; i < b->nLines; i++) pluto_free(b->lines[i]);
    pluto_free(b->lines);
    b->lines = NULL;
    b->nLines = 0;
    pluto_free(b->src);
    pluto_free(b->alt);
    pluto_free(b->caption);
    pluto_free(b->imgHref);
    pluto_free(b->usemap);
    pluto_free(b->svgXml);
    pluto_free(b->tableCaption);
    pluto_free(b->tableWidth);
    pluto_free(b->inputType);
    pluto_free(b->inName);
    pluto_free(b->inValue);
    pluto_free(b->placeholder);
    pluto_free(b->checkboxLabel);
    pluto_free(b->submitLabel);
    pluto_free(b->formAction);
    pluto_free(b->formMethod);
    for (size_t i = 0; i < b->nOptions; i++) {
        pluto_free(b->options[i].text);
        pluto_free(b->options[i].value);
    }
    pluto_free(b->options);
    b->options = NULL;
    b->nOptions = b->capOptions = 0;
    pluto_free(b->boxLabel);
    pluto_free(b->toggleKey);
    pluto_free(b->phTag);
    pluto_free(b->phHref);
}

static void db_free(DocBlock* b) {
    if (b == NULL) return;
    db_free_fields(b);
    pluto_free(b);
}

static int db_push_inline(DocBlock* b, DocInline in) {
    if (b->nInlines == b->capInlines) {
        size_t nc = b->capInlines ? b->capInlines * 2 : 8;
        DocInline* ni = pluto_realloc(b->inlines, nc * sizeof(DocInline));
        if (ni == NULL) return 0;
        b->inlines = ni;
        b->capInlines = nc;
    }
    b->inlines[b->nInlines++] = in;
    return 1;
}

static void doc_add_block(DocState* st, DocBlock* blk) {
    if (blk != NULL && st->doc->nBlocks < DOC_MAX_BLOCKS) {
        DocDocument* doc = st->doc;
        DocBlock* nb = pluto_realloc(doc->blocks,
                                     (doc->nBlocks + 1) * sizeof(DocBlock));
        if (nb == NULL) {
            db_free(blk);
            return;
        }
        doc->blocks = nb;
        doc->capBlocks = doc->nBlocks + 1;
        doc->blocks[doc->nBlocks++] = *blk;
        pluto_free(blk);
        return;
    }
    if (blk != NULL) st->truncated = 1;
    db_free(blk);
}

static void doc_flush_block(DocState* st) {
    DocBlock* cb = st->currentBlock;
    if (cb == NULL) return;
    st->currentBlock = NULL;
    if (!(cb->type == DB_PARAGRAPH && cb->nInlines == 0))
        doc_add_block(st, cb);
    else
        db_free(cb);
}

static void doc_ensure_block(DocState* st) {
    if (st->currentBlock != NULL) return;
    DocBlock* b = pluto_malloc(sizeof(DocBlock));
    if (b == NULL) return;
    memset(b, 0, sizeof(*b));
    b->type = DB_PARAGRAPH;
    st->currentBlock = b;
}

static void doc_add_inline(DocState* st, DocInline* in) {
    doc_ensure_block(st);
    if (st->currentBlock == NULL) {
        di_free(in);
        return;
    }
    if (st->currentBlock->nInlines >= DOC_MAX_INLINES) {
        di_free(in);
        return;
    }
    if (!db_push_inline(st->currentBlock, *in)) di_free(in);
}

static void doc_add_inline_text(DocState* st, const char* raw) {
    if (raw == NULL || raw[0] == '\0') return;
    char* collapsed = NULL;
    const char* text = raw;
    if (!st->inPre) {
        collapsed = pluto_malloc(strlen(raw) * 2 + 2);
        if (collapsed == NULL) return;
        const char* p = raw;
        char* o = collapsed;
        while (*p != '\0') {
            if (*p == '\r' || *p == '\n' || *p == '\t') {
                *o++ = ' ';
                while (*p == '\r' || *p == '\n' || *p == '\t') p++;
            } else {
                *o++ = *p++;
            }
        }
        *o = '\0';
        text = collapsed;
    }
    size_t tl = strlen(text);
    size_t a = 0;
    while (a < tl && isspace((unsigned char)text[a])) a++;
    if (a == tl) {
        pluto_free(collapsed);
        return;
    }

    if (st->currentHref != NULL)
        sb_append_str(&st->linkText, text);

    DocInline in;
    memset(&in, 0, sizeof(in));
    in.type = DIT_TEXT;
    in.text = pluto_strdup(text);
    in.textLen = tl;
    in.bold = st->f.bold;
    in.italic = st->f.italic;
    in.underline = st->f.underline || (st->currentHref != NULL);
    in.code = st->f.code;
    in.small = st->f.small;
    in.big = st->f.big;
    in.sub = st->f.sub;
    in.sup = st->f.sup;
    in.mark = st->f.mark;
    in.strike = st->f.strike;
    in.invert = st->f.invert;
    in.inert = st->inert > 0;
    in.anchorIndex = st->currentAnchorIndex;
    if (st->currentHref != NULL) in.href = pluto_strdup(st->currentHref);
    pluto_free(collapsed);
    doc_add_inline(st, &in);
}

/* ── block factory ────────────────────────────────────────────────────── */

static DocBlock* d_new_block(int type) {
    DocBlock* b = pluto_malloc(sizeof(DocBlock));
    if (b == NULL) return NULL;
    memset(b, 0, sizeof(*b));
    b->type = type;
    return b;
}

/* ── links ────────────────────────────────────────────────────────────── */

static void doc_push_link(DocDocument* doc, const char* href,
                          const char* text, const char* target) {
    DocLink nl;
    memset(&nl, 0, sizeof(nl));
    nl.href = pluto_strdup(href ? href : "");
    nl.text = pluto_strdup(text ? text : "");
    nl.target = pluto_strdup(target ? target : "");
    if (nl.href == NULL || nl.text == NULL || nl.target == NULL) {
        pluto_free(nl.href); pluto_free(nl.text); pluto_free(nl.target);
        return;
    }
    DocLink* arr = pluto_realloc(doc->links,
                                 (doc->nLinks + 1) * sizeof(DocLink));
    if (arr == NULL) {
        pluto_free(nl.href); pluto_free(nl.text); pluto_free(nl.target);
        return;
    }
    doc->links = arr;
    doc->capLinks = doc->nLinks + 1;
    doc->links[doc->nLinks++] = nl;
}

static void d_cell_push_inline(DocState* st, DocInline* in) {
    DCellBuild* c = st->cell;
    if (c->nInlines >= DOC_MAX_INLINES) {
        di_free(in);
        return;
    }
    if (c->nInlines == c->capInlines) {
        size_t nc = c->capInlines ? c->capInlines * 2 : 8;
        DocInline* ni = pluto_realloc(c->inlines, nc * sizeof(DocInline));
        if (ni == NULL) {
            di_free(in);
            return;
        }
        c->inlines = ni;
        c->capInlines = nc;
    }
    c->inlines[c->nInlines++] = *in;
}

/* ── text node routing ────────────────────────────────────────────────── */

static void d_handle_text_node(DocState* st, DomNode* n) {
    const char* text = (n->text != NULL) ? n->text : "";
    if (text[0] == '\0') return;
    if (st->inPre) { sb_append_str(&st->preBuffer, text); return; }
    if (st->inTextarea) { sb_append_str(&st->textareaBuffer, text); return; }
    if (st->cell != NULL) {
        /* first text of the cell loses its leading whitespace; other runs
         * only collapse \r\n\t to spaces (no full whitespace squeeze) */
        const char* txt = text;
        char buf[512];
        if (st->cellFirstText) {
            while (*txt != '\0' && isspace((unsigned char)*txt)) txt++;
            st->cellFirstText = 0;
        }
        char* o = buf;
        for (const char* p = txt; *p != '\0' && o + 1 < buf + sizeof(buf); p++) {
            if (*p == '\r' || *p == '\n' || *p == '\t') {
                *o++ = ' ';
                while (p[1] == '\r' || p[1] == '\n' || p[1] == '\t') p++;
            } else {
                *o++ = *p;
            }
        }
        *o = '\0';
        if (buf[0] == '\0') return;
        DocInline in;
        memset(&in, 0, sizeof(in));
        in.type = DIT_TEXT;
        in.text = pluto_strdup(buf);
        in.textLen = strlen(buf);
        if (in.text == NULL) return;
        in.bold = st->f.bold;
        in.italic = st->f.italic;
        in.underline = st->f.underline || (st->currentHref != NULL);
        in.code = st->f.code;
        in.small = st->f.small;
        in.big = st->f.big;
        in.sub = st->f.sub;
        in.sup = st->f.sup;
        in.mark = st->f.mark;
        in.strike = st->f.strike;
        in.invert = st->f.invert;
        in.inert = st->inert > 0;
        in.anchorIndex = st->currentAnchorIndex;
        if (st->currentHref != NULL)
            in.href = pluto_strdup(st->currentHref);
        d_cell_push_inline(st, &in);
        return;
    }
    if (st->figureActive && !st->figureCaptionDone) {
        sb_append_str(&st->figCaption, text);
        return;
    }
    if (st->inMath) { sb_append_str(&st->mathParts, text); return; }
    doc_add_inline_text(st, text);
}

/* ── images ───────────────────────────────────────────────────────────── */

static void d_handle_image(DocState* st, StrMap* attrs) {
    char srcBuf[1024];
    const char* src = da_get(attrs, "src");
    if (src == NULL || src[0] == '\0') src = da_get(attrs, "data-src");
    if (src == NULL || src[0] == '\0') {
        const char* ss = da_get(attrs, "srcset");
        if (ss != NULL && ss[0] != '\0') {
            size_t j = 0;
            const char* p = ss;
            while (*p != '\0' && !(isspace((unsigned char)*p) || *p == ',') &&
                   j + 1 < sizeof(srcBuf))
                srcBuf[j++] = *p++;
            srcBuf[j] = '\0';
            if (j > 0) src = srcBuf;
        }
    }
    if (src == NULL || src[0] == '\0') return;

    const char* alt = da_get(attrs, "alt");
    if (alt == NULL || alt[0] == '\0') alt = da_get(attrs, "title");
    if (alt == NULL || alt[0] == '\0') alt = "Image";

    int ok = 0;
    double wv = d_tonum(da_get(attrs, "width"), &ok);
    int w = (ok && wv > 0) ? (int)wv : 160;
    double hv = d_tonum(da_get(attrs, "height"), &ok);
    int h = (ok && hv > 0) ? (int)hv : 80;

    /* filter before resolution, on the raw attribute */
    if (d_contains(src, "tracking") || d_contains(src, "beacon")) return;

    if (w > 360) w = 360;
    if (h > 180) h = 180;

    const char* usemap = da_get(attrs, "usemap");
    if (usemap != NULL && usemap[0] == '#') usemap++;

    StrBuf resolved;
    sb_init(&resolved);
    url_resolve(st->baseUrl, src, &resolved);

    DocBlock* blk = d_new_block(DB_IMAGE);
    if (blk == NULL) { sb_clear(&resolved); return; }
    blk->src = sb_detach(&resolved);
    blk->alt = pluto_strdup(alt);
    blk->width = w;
    blk->height = h;
    blk->align = d_parse_align(attrs);
    blk->imgInert = st->inert > 0;
    if (st->currentHref != NULL)
        blk->imgHref = pluto_strdup(st->currentHref);
    if (usemap != NULL && usemap[0] != '\0')
        blk->usemap = pluto_strdup(usemap);

    if (st->figureActive) {
        db_free(st->figureImage);
        st->figureImage = blk;
        return;
    }
    doc_add_block(st, blk);
}

/* ── element dispatch ─────────────────────────────────────────────────── */

static void d_quote_char(DocState* st) {
    DocInline in;
    memset(&in, 0, sizeof(in));
    in.type = DIT_TEXT;
    in.text = pluto_strdup("\"");
    in.bold = 1;
    in.italic = 1;
    doc_add_inline(st, &in);
}

static void d_break_inline(DocState* st, int type) {
    DocInline in;
    memset(&in, 0, sizeof(in));
    in.type = type;
    in.bold = st->f.bold;
    in.italic = st->f.italic;
    in.underline = st->f.underline || (st->currentHref != NULL);
    in.code = st->f.code;
    in.small = st->f.small;
    in.big = st->f.big;
    in.sub = st->f.sub;
    in.sup = st->f.sup;
    in.mark = st->f.mark;
    in.strike = st->f.strike;
    in.invert = st->f.invert;
    in.inert = st->inert > 0;
    in.anchorIndex = st->currentAnchorIndex;
    if (st->currentHref != NULL) in.href = pluto_strdup(st->currentHref);
    doc_add_inline(st, &in);
}

static void d_split_code_lines(DocBlock* b) {
    const char* p = (b->codeText != NULL) ? b->codeText : "";
    size_t cap = 0;
    const char* seg = p;
    for (;;) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - seg);
            if (len > 0 && seg[len - 1] == '\r') len--;
            if (b->nLines == cap) {
                size_t nc = cap ? cap * 2 : 8;
                char** nl = pluto_realloc(b->lines, nc * sizeof(char*));
                if (nl == NULL) return;
                b->lines = nl;
                cap = nc;
            }
            b->lines[b->nLines++] = pluto_strndup(seg, len);
            if (*p == '\0') break;
            seg = p + 1;
        }
        p++;
    }
}

/* ── P11 helpers ──────────────────────────────────────────────────────── */

static char* d_lower_dup(const char* s) {
    if (s == NULL) return NULL;
    char* out = pluto_strdup(s);
    if (out == NULL) return NULL;
    for (char* c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
    return out;
}

/* in-place Lua "%s+" -> " " collapse plus end trim */
static void d_collapse_trim(char* s) {
    if (s == NULL) return;
    size_t r = 0, w = 0;
    int inWs = 0;
    while (s[r] != '\0') {
        if (isspace((unsigned char)s[r])) {
            inWs = 1;
            r++;
        } else {
            if (inWs && w > 0) s[w++] = ' ';
            inWs = 0;
            s[w++] = s[r++];
        }
    }
    s[w] = '\0';
}

static int d_details_override(const DocState* st, const char* key,
                              int defaultOpen) {
    if (st->opts != NULL && st->opts->detailsOverrides != NULL) {
        for (size_t i = 0; i < st->opts->nOverrides; i++)
            if (strcmp(st->opts->detailsOverrides[i].key, key) == 0)
                return st->opts->detailsOverrides[i].open ? 1 : 0;
    }
    return defaultOpen;
}

/* shared "common" attribute bundle for input-family blocks */
typedef struct {
    char* name;          /* owned */
    char* value;         /* owned */
    unsigned char disabled, readonlyFlag, requiredFlag, inertFlag;
    int maxlength;       /* -1 unset */
} DInputCommon;

static void d_input_common(DocState* st, StrMap* attrs, DInputCommon* c) {
    memset(c, 0, sizeof(*c));
    const char* nm = da_get(attrs, "name");
    c->name = pluto_strdup((nm != NULL) ? nm : "q");
    const char* val = da_get(attrs, "value");
    c->value = pluto_strdup((val != NULL) ? val : "");
    int dis = da_has(attrs, "disabled") || st->disabledDepth > 0;
    c->disabled = (unsigned char)dis;
    c->readonlyFlag =
        (unsigned char)(da_has(attrs, "readonly") || dis);
    c->requiredFlag = (unsigned char)da_has(attrs, "required");
    int ok = 0;
    double mv = d_tonum(da_get(attrs, "maxlength"), &ok);
    c->maxlength = ok ? (int)mv : -1;
    c->inertFlag = (unsigned char)((st->inert > 0) || dis);
}

/* copies the common bundle into a block; takes ownership of name/value */
static void d_block_set_common(DocBlock* b, DInputCommon* c,
                               DocState* st) {
    b->inName = c->name;
    b->inValue = c->value;
    b->disabledFlag = c->disabled;
    b->readonlyFlag = c->readonlyFlag;
    b->requiredFlag = c->requiredFlag;
    b->maxlength = c->maxlength;
    b->blockInert = c->inertFlag;
    if (st->formAction != NULL && st->formAction[0] != '\0')
        b->formAction = pluto_strdup(st->formAction);
    if (st->formMethod != NULL && st->formMethod[0] != '\0')
        b->formMethod = pluto_strdup(st->formMethod);
}

/* per-element formaction / formmethod attribute overrides */
static void d_form_overrides(DocState* st, StrMap* attrs, DocBlock* b) {
    const char* fa = da_get(attrs, "formaction");
    if (fa != NULL && fa[0] != '\0') {
        StrBuf fb;
        sb_init(&fb);
        url_resolve(st->baseUrl, fa, &fb);
        pluto_free(b->formAction);
        b->formAction = sb_detach(&fb);
    }
    const char* fm = da_get(attrs, "formmethod");
    if (fm != NULL && fm[0] != '\0') {
        pluto_free(b->formMethod);
        b->formMethod = d_lower_dup(fm);
    }
}

static int d_str_eq_ci(const char* a, const char* b) {
    return a != NULL && b != NULL && strcasecmp(a, b) == 0;
}

/* viewBox fallback sizing. The Lua source binds captures #1/#2 (minx/miny)
 * and uses them as w/h fallbacks — replicate exactly, quirks included:
 *   vbW, vbH = match("^%s*([num]+)%s+([num]+)%s+[num]+%s+[num]+$")   */
static void d_parse_viewbox(const char* vb, double* wOut, double* hOut) {
    *wOut = 0;
    *hOut = 0;
    if (vb == NULL) return;
    int n = 0;
    double first = 0, second = 0;
    const char* p = vb;
    while (*p != '\0' && n < 2) {
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        char* end = NULL;
        double v = strtod(p, &end);
        if (end == p) { p++; continue; }
        if (n == 0) first = v;
        else second = v;
        n++;
        p = end;
    }
    /* full pattern requires FOUR number groups before it matches */
    if (n < 2) return;
    int groups = 2;
    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        char* end = NULL;
        (void)strtod(p, &end);
        if (end == p) break;
        groups++;
        p = end;
    }
    if (groups < 4) return;
    *wOut = first;
    *hOut = second;
}

/* mfenced attribute: entity-decoded with default; returns owned string */
static char* d_mfenced_attr(StrMap* attrs, const char* key,
                            const char* dflt) {
    const char* raw = da_get(attrs, key);
    if (raw == NULL || raw[0] == '\0') raw = dflt;
    size_t outLen = 0;
    char* dec = entities_decode(raw, strlen(raw), &outLen);
    if (dec != NULL) {
        dec[outLen] = '\0';
        return dec;
    }
    return pluto_strdup(dflt);
}

/* ── table row processing ─────────────────────────────────────────────── */

static void d_handle_row(DocState* st, DomNode* trNode, DTableBuild* tbl);

static void d_handle_row(DocState* st, DomNode* trNode, DTableBuild* tbl) {
    DocTableRow row;
    memset(&row, 0, sizeof(row));
    for (size_t i = 0; i < trNode->nChildren; i++) {
        DomNode* cellNode = trNode->children[i];
        if (cellNode->kind != DOM_ELEMENT)
            continue;
        int isTh = !strcmp(cellNode->tag, "th");
        if (!isTh && strcmp(cellNode->tag, "td") != 0)
            continue;

        char* savedHref = st->currentHref;
        long savedAnchor = st->currentAnchorIndex;

        DCellBuild cellBuild;
        memset(&cellBuild, 0, sizeof(cellBuild));
        cellBuild.isHeader = isTh;
        cellBuild.align = d_parse_align(cellNode->attrs);
        st->cell = &cellBuild;
        st->cellFirstText = 1;
        st->currentHref = NULL;
        st->currentAnchorIndex = -1;
        d_walk_children(st, cellNode);
        if (cellBuild.nInlines == 0) {
            DocInline sp;
            memset(&sp, 0, sizeof(sp));
            sp.type = DIT_TEXT;
            sp.text = pluto_strdup(" ");
            sp.textLen = 1;
            if (sp.text != NULL) d_cell_push_inline(st, &sp);
        }
        st->cell = NULL;

        DocTableCell* arr = pluto_realloc(
            row.cells, (row.nCells + 1) * sizeof(DocTableCell));
        if (arr == NULL) {
            for (size_t j = 0; j < cellBuild.nInlines; j++)
                di_free(&cellBuild.inlines[j]);
            pluto_free(cellBuild.inlines);
        } else {
            DocTableCell* c = &arr[row.nCells++];
            row.cells = arr;
            row.capCells = row.nCells;
            c->inlines = cellBuild.inlines;
            c->nInlines = cellBuild.nInlines;
            c->capInlines = cellBuild.capInlines;
            c->isHeader = isTh;
            int ok = 0;
            double cs = d_tonum(da_get(cellNode->attrs, "colspan"), &ok);
            c->colspan = (ok && cs >= 1) ? (int)cs : 1;
            double rs = d_tonum(da_get(cellNode->attrs, "rowspan"), &ok);
            c->rowspan = (ok && rs >= 1) ? (int)rs : 1;
            const char* ab = da_get(cellNode->attrs, "abbr");
            if (ab == NULL || ab[0] == '\0') ab = da_get(cellNode->attrs, "title");
            c->abbr = pluto_strdup((ab != NULL) ? ab : "");
            c->align = cellBuild.align;
        }

        st->currentHref = savedHref;
        st->currentAnchorIndex = savedAnchor;
    }
    DocTableRow* rarr =
        pluto_realloc(tbl->rows, (tbl->nRows + 1) * sizeof(DocTableRow));
    if (rarr == NULL) {
        dtr_free(&row);
        return;
    }
    tbl->rows = rarr;
    tbl->capRows = tbl->nRows + 1;
    tbl->rows[tbl->nRows++] = row;
}

/* ── select options ───────────────────────────────────────────────────── */

static void d_collect_select_options(DocState* st, DomNode* node,
                                     DocBlock* b) {
    for (size_t i = 0; i < node->nChildren; i++) {
        DomNode* c = node->children[i];
        if (c->kind != DOM_ELEMENT) continue;
        if (!strcmp(c->tag, "option")) {
            char* text = d_concat_node_text(c);
            d_collapse_trim(text);
            const char* labelAttr = da_get(c->attrs, "label");
            char* finalText = text;
            if (labelAttr != NULL && labelAttr[0] != '\0') {
                pluto_free(text);
                finalText = pluto_strdup(labelAttr);
            }
            const char* val = da_get(c->attrs, "value");
            DocSelectOpt* arr = pluto_realloc(
                b->options, (b->nOptions + 1) * sizeof(DocSelectOpt));
            if (arr == NULL || finalText == NULL) {
                pluto_free(finalText);
                continue;
            }
            b->options = arr;
            b->capOptions = b->nOptions + 1;
            DocSelectOpt* o = &b->options[b->nOptions++];
            o->text = finalText;
            o->value = pluto_strdup((val != NULL) ? val : finalText);
            o->selected = (unsigned char)da_has(c->attrs, "selected");
            o->disabled = (unsigned char)da_has(c->attrs, "disabled");
            o->group = 0;
        } else if (!strcmp(c->tag, "optgroup")) {
            const char* gl = da_get(c->attrs, "label");
            size_t childrenBefore = b->nOptions;
            d_collect_select_options(st, c, b);
            if (gl != NULL && gl[0] != '\0' &&
                childrenBefore <= b->nOptions) {
                DocSelectOpt* arr = pluto_realloc(
                    b->options, (b->nOptions + 1) * sizeof(DocSelectOpt));
                if (arr == NULL) continue;
                b->options = arr;
                b->capOptions = b->nOptions + 1;
                memmove(&b->options[childrenBefore + 1],
                        &b->options[childrenBefore],
                        (b->nOptions - childrenBefore) *
                            sizeof(DocSelectOpt));
                DocSelectOpt* g = &b->options[childrenBefore];
                g->text = pluto_strdup(gl);
                g->value = pluto_strdup("");
                g->group = 1;
                g->disabled = 1;
                g->selected = 0;
                b->nOptions++;
            }
        }
    }
}

/* ── media placeholders ───────────────────────────────────────────────── */

static void d_handle_media_placeholder(DocState* st, const char* tag,
                                       DomNode* n, StrMap* attrs) {
    doc_flush_block(st);
    const char* src = da_get(attrs, "src");
    if (src == NULL || src[0] == '\0') src = da_get(attrs, "data");
    if (src == NULL || src[0] == '\0') {
        for (size_t i = 0; i < n->nChildren; i++) {
            DomNode* c = n->children[i];
            if (c->kind == DOM_ELEMENT && !strcmp(c->tag, "source")) {
                const char* s = da_get(c->attrs, "src");
                if (s != NULL && s[0] != '\0') {
                    src = s;
                    break;
                }
            }
        }
    }

    const char* label = da_get(attrs, "title");
    if (label == NULL || label[0] == '\0') label = da_get(attrs, "alt");
    char labelBuf[512];
    int labelIsBuf = 0;
    if ((label == NULL || label[0] == '\0')) {
        if (src != NULL && src[0] != '\0') {
            /* base = last path segment, tolerating one trailing slash */
            const char* end = src + strlen(src);
            if (end > src && end[-1] == '/') end--;
            const char* seg = end;
            while (seg > src && seg[-1] != '/') seg--;
            snprintf(labelBuf, sizeof(labelBuf), "[%s: %.*s]", tag,
                     (int)(end - seg), seg);
        } else {
            snprintf(labelBuf, sizeof(labelBuf), "[%s]", tag);
        }
        label = labelBuf;
        labelIsBuf = 1;
    }
    (void)labelIsBuf;

    int okw = 0, okh = 0;
    double wv = d_tonum(da_get(attrs, "width"), &okw);
    double hv = d_tonum(da_get(attrs, "height"), &okh);
    int w = okw ? (int)wv : 160;
    int h = okh ? (int)hv : 60;
    if (w > 360) w = 360;
    if (h > 120) h = 120;

    DocBlock* b = d_new_block(DB_PLACEHOLDER);
    if (b == NULL) return;
    b->phTag = pluto_strdup(tag);
    b->boxLabel = pluto_strdup(label);
    b->width = w;
    b->height = h;
    if ((!strcmp(tag, "iframe") || !strcmp(tag, "portal")) &&
        src != NULL && src[0] != '\0' && d_valid_href(src)) {
        StrBuf rb;
        sb_init(&rb);
        url_resolve(st->baseUrl, src, &rb);
        b->phHref = sb_detach(&rb);
    }
    doc_add_block(st, b);
}

/* ── image maps ───────────────────────────────────────────────────────── */

static void d_map_free(DocMap* m) {
    pluto_free(m->name);
    for (size_t i = 0; i < m->nRegions; i++) {
        DocAreaRegion* r = &m->regions[i];
        pluto_free(r->shape);
        pluto_free(r->coords);
        pluto_free(r->href);
        pluto_free(r->alt);
    }
    pluto_free(m->regions);
}

static void d_collect_areas(DocState* st, DomNode* n, DocMap* m) {
    for (size_t i = 0; i < n->nChildren; i++) {
        DomNode* c = n->children[i];
        if (c->kind != DOM_ELEMENT) continue;
        if (!strcmp(c->tag, "area")) {
            StrMap* a = c->attrs;
            const char* shape = da_get(a, "shape");
            const char* coordsRaw = da_get(a, "coords");
            const char* alt = da_get(a, "alt");

            DocAreaRegion r;
            memset(&r, 0, sizeof(r));
            r.shape = pluto_strdup((shape != NULL && shape[0] != '\0')
                                       ? shape : "rect");

            int* carr = NULL;
            size_t cn = 0;
            const char* p = (coordsRaw != NULL) ? coordsRaw : "";
            while (*p != '\0') {
                while (*p != '\0' && !isdigit((unsigned char)*p)) p++;
                if (*p == '\0') break;
                long v = strtol(p, (char**)&p, 10);
                int* narr = pluto_realloc(carr, (cn + 1) * sizeof(int));
                if (narr == NULL) break;
                carr = narr;
                carr[cn++] = (int)v;
            }
            r.coords = carr;
            r.nCoords = cn;

            if (d_valid_href(da_get(a, "href"))) {
                StrBuf rb;
                sb_init(&rb);
                url_resolve(st->baseUrl, da_get(a, "href"), &rb);
                r.href = sb_detach(&rb);
            }
            r.alt = pluto_strdup((alt != NULL) ? alt : "");

            DocAreaRegion* arr = pluto_realloc(
                m->regions, (m->nRegions + 1) * sizeof(DocAreaRegion));
            if (arr == NULL) {
                pluto_free(r.shape);
                pluto_free(r.coords);
                pluto_free(r.href);
                pluto_free(r.alt);
                return;
            }
            m->regions = arr;
            m->capRegions = m->nRegions + 1;
            m->regions[m->nRegions++] = r;
        }
        /* direct children only — matches the source collector */
    }
}

/* ── datalists ────────────────────────────────────────────────────────── */

static void d_datalist_free(DocDatalist* dl) {
    pluto_free(dl->id);
    for (size_t i = 0; i < dl->nOpts; i++) {
        pluto_free(dl->opts[i].text);
        pluto_free(dl->opts[i].value);
    }
    pluto_free(dl->opts);
}

static void d_handle_element(DocState* st, DomNode* n) {
    const char* tag = n->tag;
    StrMap* attrs = n->attrs;

    /* metadata containers never render */
    if (!strcmp(tag, "script") || !strcmp(tag, "style") ||
        !strcmp(tag, "title"))
        return;

    if (!strcmp(tag, "img")) { d_handle_image(st, attrs); return; }

    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
        doc_flush_block(st);
        if (st->cell != NULL) {
            d_walk_children(st, n);
            return;
        }
        DBox sp;
        d_parse_box_spacing(attrs, &sp);
        DocBlock* b = d_new_block(DB_HEADING);
        if (b == NULL) return;
        b->level = tag[1] - '0';
        b->spacingTop = sp.top;
        b->spacingBottom = sp.bottom;
        b->align = d_parse_align(attrs);
        b->indent = sp.left;
        if (d_is_inverted_style(attrs)) b->invert = 1;
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "br")) { d_break_inline(st, DIT_BR); return; }
    if (!strcmp(tag, "wbr")) { d_break_inline(st, DIT_WBR); return; }

    if (!strcmp(tag, "hr")) {
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_HR);
        if (b == NULL) return;
        b->spacingTop = 6;
        b->spacingBottom = 6;
        doc_add_block(st, b);
        return;
    }

    if (!strcmp(tag, "center") || !strcmp(tag, "marquee")) {
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_PARAGRAPH);
        if (b == NULL) return;
        b->align = "center";
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
        !strcmp(tag, "article") || !strcmp(tag, "header") ||
        !strcmp(tag, "footer") || !strcmp(tag, "main") ||
        !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
        !strcmp(tag, "address") || !strcmp(tag, "hgroup") ||
        !strcmp(tag, "noindex") || !strcmp(tag, "search")) {
        if (st->cell != NULL) {
            d_walk_children(st, n);
            return;
        }
        doc_flush_block(st);
        DBox sp;
        d_parse_box_spacing(attrs, &sp);
        DocBlock* b = d_new_block(DB_PARAGRAPH);
        if (b == NULL) return;
        b->align = d_parse_align(attrs);
        if (d_is_inverted_style(attrs)) b->invert = 1;
        b->spacingTop = sp.top;
        b->spacingBottom = sp.bottom;
        b->indent = sp.left;
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "blockquote")) {
        doc_flush_block(st);
        DBox sp;
        d_parse_box_spacing(attrs, &sp);
        DocBlock* b = d_new_block(DB_BLOCKQUOTE);
        if (b == NULL) return;
        b->align = d_parse_align(attrs);
        if (d_is_inverted_style(attrs)) b->invert = 1;
        b->indent = sp.left + 12;
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "pre") || !strcmp(tag, "xmp") || !strcmp(tag, "listing") ||
        !strcmp(tag, "plaintext")) {
        doc_flush_block(st);
        int savedPre = st->inPre;
        StrBuf savedBuf = st->preBuffer;
        sb_init(&st->preBuffer);
        st->inPre = 1;
        char* codeText;
        if (!strcmp(tag, "pre")) {
            d_walk_children(st, n);
            codeText = sb_detach(&st->preBuffer);
        } else {
            codeText = d_concat_node_text(n);
        }
        DocBlock* b = d_new_block(DB_CODE_BLOCK);
        if (b == NULL) {
            pluto_free(codeText);
        } else {
            b->codeText = codeText;
            d_split_code_lines(b);
            doc_add_block(st, b);
        }
        sb_clear(&st->preBuffer);
        st->preBuffer = savedBuf;
        st->inPre = savedPre;
        return;
    }

    {
        unsigned char* fld = NULL;
        if (!strcmp(tag, "b") || !strcmp(tag, "strong")) fld = &st->f.bold;
        else if (!strcmp(tag, "i") || !strcmp(tag, "em")) fld = &st->f.italic;
        else if (!strcmp(tag, "u")) fld = &st->f.underline;
        else if (!strcmp(tag, "s") || !strcmp(tag, "strike") ||
                 !strcmp(tag, "del")) fld = &st->f.strike;
        else if (!strcmp(tag, "mark")) fld = &st->f.mark;
        else if (!strcmp(tag, "small")) fld = &st->f.small;
        else if (!strcmp(tag, "big")) fld = &st->f.big;
        else if (!strcmp(tag, "sub")) fld = &st->f.sub;
        else if (!strcmp(tag, "sup")) fld = &st->f.sup;
        else if (!strcmp(tag, "tt") || !strcmp(tag, "code") ||
                 !strcmp(tag, "kbd") || !strcmp(tag, "samp"))
            fld = &st->f.code;
        if (fld != NULL) {
            (*fld)++;
            d_walk_children(st, n);
            (*fld)--;
            return;
        }
    }

    if (!strcmp(tag, "q")) {
        d_quote_char(st);
        d_walk_children(st, n);
        d_quote_char(st);
        return;
    }

    if (!strcmp(tag, "a")) {
        const char* rawHref = da_get(attrs, "href");
        int haveResolved = 0;
        if (d_valid_href(rawHref)) {
            StrBuf rb;
            sb_init(&rb);
            url_resolve(st->baseUrl, rawHref, &rb);
            st->currentHref = sb_detach(&rb);
            st->currentAnchorIndex = st->anchorCounter++;
            haveResolved = 1;
        }
        sb_clear(&st->linkText);
        d_walk_children(st, n);
        if (haveResolved) {
            char* lt = (st->linkText.len > 0) ? sb_detach(&st->linkText) : NULL;
            const char* title = da_get(attrs, "title");
            const char* txt =
                (lt != NULL) ? lt : ((title != NULL && title[0]) ? title : rawHref);
            doc_push_link(st->doc, st->currentHref, txt, da_get(attrs, "target"));
            pluto_free(lt);
        } else {
            sb_clear(&st->linkText);
        }
        pluto_free(st->currentHref);
        st->currentHref = NULL;
        st->currentAnchorIndex = -1;
        return;
    }

    if (!strcmp(tag, "span") || !strcmp(tag, "font") || !strcmp(tag, "time") ||
        !strcmp(tag, "data")) {
        DFlags sv = st->f;
        DStyle styl;
        d_parse_style(&styl, da_get(attrs, "style"));
        const char* fw = d_style_get(&styl, "font-weight");
        if (fw != NULL && strstr(fw, "bold") != NULL) st->f.bold++;
        const char* fsy = d_style_get(&styl, "font-style");
        if (fsy != NULL && strstr(fsy, "italic") != NULL) st->f.italic++;
        const char* td = d_style_get(&styl, "text-decoration");
        if (td != NULL) {
            if (strstr(td, "underline") != NULL) st->f.underline++;
            if (strstr(td, "line-through") != NULL) st->f.strike++;
        }
        const char* col = d_style_get(&styl, "color");
        if (col != NULL && (strstr(col, "#FFFF") != NULL ||
                            strstr(col, "#fff") != NULL))
            st->f.invert++;
        const char* fsz = d_style_get(&styl, "font-size");
        if (fsz != NULL) {
            if (strstr(fsz, "large") != NULL) st->f.big++;
            else if (strstr(fsz, "small") != NULL) st->f.small++;
        }
        const char* va = d_style_get(&styl, "vertical-align");
        if (va != NULL && strcmp(va, "super") == 0) st->f.sup++;
        else if (va != NULL && strcmp(va, "sub") == 0) st->f.sub++;
        const char* bgc = d_style_get(&styl, "background-color");
        if (bgc != NULL && bgc[0] != '\0') st->f.mark++;
        if (!strcmp(tag, "font")) {
            int ok = 0;
            double szv = d_tonum(da_get(attrs, "size"), &ok);
            if (ok) {
                if (szv >= 5) st->f.big++;
                else if (szv <= 2) st->f.small++;
            }
        }
        long countBefore =
            (st->currentBlock != NULL) ? (long)st->currentBlock->nInlines : 0;
        d_walk_children(st, n);
        long countAfter =
            (st->currentBlock != NULL) ? (long)st->currentBlock->nInlines : 0;
        if (!strcmp(tag, "time") || !strcmp(tag, "data")) {
            const char* fb = !strcmp(tag, "time")
                                 ? da_get(attrs, "datetime")
                                 : da_get(attrs, "value");
            if (fb != NULL && fb[0] != '\0' && countBefore == countAfter)
                doc_add_inline_text(st, fb);
        }
        st->f = sv;
        return;
    }

    if (!strcmp(tag, "abbr") || !strcmp(tag, "acronym") ||
        !strcmp(tag, "noscript") || !strcmp(tag, "noembed") ||
        !strcmp(tag, "noframes") || !strcmp(tag, "ruby") ||
        !strcmp(tag, "rt") || !strcmp(tag, "rp") || !strcmp(tag, "rb") ||
        !strcmp(tag, "rtc") || !strcmp(tag, "picture") ||
        !strcmp(tag, "slot")) {
        d_walk_children(st, n);
        return;
    }

    if (!strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "menu") ||
        !strcmp(tag, "dir")) {
        DListCtx nc;
        memset(&nc, 0, sizeof(nc));
        nc.depth = (st->listCtx != NULL) ? st->listCtx->depth + 1 : 1;
        nc.count = 0;
        nc.start = 1;
        nc.markerType = "1";
        nc.ordered = !strcmp(tag, "ol");
        if (nc.ordered) {
            int ok = 0;
            double sv = d_tonum(da_get(attrs, "start"), &ok);
            if (ok) nc.start = (int)sv;
            nc.reversed = da_has(attrs, "reversed");
            const char* ty = da_get(attrs, "type");
            if (ty != NULL && (!strcmp(ty, "1") || !strcmp(ty, "a") ||
                               !strcmp(ty, "A") || !strcmp(ty, "i") ||
                               !strcmp(ty, "I")))
                nc.markerType = ty;
        }
        DListCtx* old = st->listCtx;
        st->listCtx = &nc;
        d_walk_children(st, n);
        st->listCtx = old;
        return;
    }

    if (!strcmp(tag, "li")) {
        static const char* const MARKERS[5] = {"1", "a", "A", "i", "I"};
        DListCtx def;
        memset(&def, 0, sizeof(def));
        def.ordered = 0;
        def.start = 1;
        def.count = 0;
        def.depth = 1;
        def.markerType = "1";
        DListCtx* cx = (st->listCtx != NULL) ? st->listCtx : &def;
        long number;
        int vok = 0;
        double vv = d_tonum(da_get(attrs, "value"), &vok);
        if (vok) {
            number = (long)vv;
            cx->start = cx->reversed ? number - 1 : number + 1;
            cx->count = 0;
        } else {
            cx->count++;
            number = cx->reversed
                         ? (long)cx->start - ((long)cx->count - 1)
                         : (long)cx->start + ((long)cx->count - 1);
        }
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_LIST_ITEM);
        if (b == NULL) return;
        b->isOrdered = cx->ordered;
        b->number = number;
        /* markerType must stay valid after tokens are freed: normalize to
         * a static literal */
        for (int mi = 0; mi < 5; mi++)
            if (cx->markerType != NULL &&
                strcmp(cx->markerType, MARKERS[mi]) == 0) {
                b->markerType = MARKERS[mi];
                break;
            }
        if (b->markerType == NULL) b->markerType = "1";
        b->depth = cx->depth;
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "dl")) {
        st->dlDepth++;
        d_walk_children(st, n);
        st->dlDepth--;
        return;
    }

    if (!strcmp(tag, "dt") || !strcmp(tag, "dd")) {
        int isDt = !strcmp(tag, "dt");
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_PARAGRAPH);
        if (b == NULL) return;
        if (isDt)
            b->dtFlag = 1;
        else {
            b->ddFlag = 1;
            b->indent = 20 * st->dlDepth;
        }
        st->currentBlock = b;
        d_walk_children(st, n);
        doc_flush_block(st);
        return;
    }

    if (!strcmp(tag, "figure")) {
        doc_flush_block(st);
        StrBuf savedCap = st->figCaption;
        DocBlock* savedImg = st->figureImage;
        int savedDone = st->figureCaptionDone;
        int savedActive = st->figureActive;
        sb_init(&st->figCaption);
        st->figureImage = NULL;
        st->figureCaptionDone = 0;
        st->figureActive = 1;
        d_walk_children(st, n);
        DocBlock* img = st->figureImage;
        st->figureImage = savedImg;
        st->figureActive = savedActive;
        st->figureCaptionDone = savedDone;
        if (img != NULL) {
            if (st->figCaption.len > 0)
                img->caption = sb_detach(&st->figCaption);
            else
                sb_clear(&st->figCaption);
            doc_add_block(st, img);
        } else if (st->figCaption.len > 0) {
            /* caption without an image becomes a centered italic paragraph */
            char* capText = sb_detach(&st->figCaption);
            DocBlock* b = d_new_block(DB_PARAGRAPH);
            if (b != NULL) {
                b->align = "center";
                DocInline in;
                memset(&in, 0, sizeof(in));
                in.type = DIT_TEXT;
                in.text = capText;
                in.textLen = strlen(capText);
                in.italic = 1;
                db_push_inline(b, in);
                doc_add_block(st, b);
            } else {
                pluto_free(capText);
            }
        } else {
            sb_clear(&st->figCaption);
        }
        st->figCaption = savedCap;
        return;
    }

    if (!strcmp(tag, "figcaption")) {
        int saved = st->figureCaptionDone;
        st->figureCaptionDone = 0;
        d_walk_children(st, n);
        st->figureCaptionDone = 1;
        (void)saved;
        return;
    }

    /* ── Tables ─────────────────────────────────────────────────────────── */

    if (!strcmp(tag, "table")) {
        if (st->cell != NULL) return;   /* nested table inside a cell: dropped */
        doc_flush_block(st);
        DTableBuild tbl;
        memset(&tbl, 0, sizeof(tbl));
        st->tbl = &tbl;
        StrBuf caption;
        sb_init(&caption);
        for (size_t i = 0; i < n->nChildren; i++) {
            DomNode* c = n->children[i];
            if (c->kind != DOM_ELEMENT) continue;
            if (!strcmp(c->tag, "caption")) {
                char* t = d_concat_node_text(c);
                d_collapse_trim(t);
                if (t[0] != '\0') sb_append_str(&caption, t);
                pluto_free(t);
            } else if (!strcmp(c->tag, "tr")) {
                d_handle_row(st, c, &tbl);
            } else if (!strcmp(c->tag, "thead") || !strcmp(c->tag, "tbody") ||
                       !strcmp(c->tag, "tfoot")) {
                for (size_t j = 0; j < c->nChildren; j++) {
                    DomNode* r = c->children[j];
                    if (r->kind == DOM_ELEMENT && !strcmp(r->tag, "tr"))
                        d_handle_row(st, r, &tbl);
                }
            }
        }
        st->tbl = NULL;
        int border = da_has(attrs, "border");
        const char* bw = da_get(attrs, "border");
        if (bw != NULL && strcmp(bw, "0") == 0) border = 0;
        if (tbl.nRows > 0) {
            DocBlock* b = d_new_block(DB_TABLE);
            if (b != NULL) {
                b->rows = tbl.rows;
                b->nRows = tbl.nRows;
                b->capRows = tbl.capRows;
                b->tableCaption = (caption.len > 0) ? sb_detach(&caption)
                                                    : NULL;
                if (caption.len == 0) sb_clear(&caption);
                b->tableBorder = border;
                const char* tw = da_get(attrs, "width");
                if (tw != NULL && tw[0] != '\0')
                    b->tableWidth = pluto_strdup(tw);
                b->align = d_parse_align(attrs);
                doc_add_block(st, b);
            } else {
                for (size_t i = 0; i < tbl.nRows; i++) dtr_free(&tbl.rows[i]);
                pluto_free(tbl.rows);
                sb_clear(&caption);
            }
        } else {
            for (size_t i = 0; i < tbl.nRows; i++) dtr_free(&tbl.rows[i]);
            pluto_free(tbl.rows);
            sb_clear(&caption);
        }
        return;
    }

    if (!strcmp(tag, "tr")) {
        if (st->tbl != NULL && st->cell == NULL)
            d_handle_row(st, n, st->tbl);   /* stray <tr> directly in flow */
        return;
    }

    if (!strcmp(tag, "td") || !strcmp(tag, "th"))
        return;   /* cells are handled by row processing; strays ignored */

    /* ── Forms ──────────────────────────────────────────────────────────── */

    if (!strcmp(tag, "form")) {
        char* savedAction = st->formAction;
        char* savedMethod = st->formMethod;
        StrBuf rb;
        sb_init(&rb);
        url_resolve(st->baseUrl, da_get(attrs, "action"), &rb);
        st->formAction = sb_detach(&rb);
        const char* m = da_get(attrs, "method");
        st->formMethod = (m != NULL && m[0] != '\0') ? d_lower_dup(m)
                                                     : pluto_strdup("get");
        d_walk_children(st, n);
        pluto_free(st->formAction);
        pluto_free(st->formMethod);
        st->formAction = savedAction;
        st->formMethod = savedMethod;
        return;
    }

    if (!strcmp(tag, "input")) {
        char* ty = d_lower_dup(da_get(attrs, "type"));
        const char* inputType = (ty != NULL) ? ty : "text";
        const char* ph = da_get(attrs, "placeholder");
        if (ph == NULL || ph[0] == '\0') ph = da_get(attrs, "aria-label");

        if (!strcmp(inputType, "hidden")) {
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_INPUT_FIELD);
            if (b != NULL) {
                b->inputType = pluto_strdup("hidden");
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                d_form_overrides(st, attrs, b);
                doc_add_block(st, b);
            }
        } else if (!strcmp(inputType, "checkbox") ||
                   !strcmp(inputType, "radio")) {
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_CHECKBOX_FIELD);
            if (b != NULL) {
                b->inputType = pluto_strdup(inputType);
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                d_form_overrides(st, attrs, b);
                b->radioFlag = !strcmp(inputType, "radio");
                b->checkedFlag = (unsigned char)da_has(attrs, "checked");
                const char* lb = da_get(attrs, "label");
                if (lb == NULL || lb[0] == '\0') lb = da_get(attrs, "title");
                if (lb == NULL || lb[0] == '\0') lb = ph;
                if (lb == NULL || lb[0] == '\0') {
                    const char* nm = da_get(attrs, "name");
                    lb = (nm != NULL) ? nm : "";
                }
                b->checkboxLabel = pluto_strdup(lb);
                doc_add_block(st, b);
            }
        } else if (!strcmp(inputType, "text") ||
                   !strcmp(inputType, "search") ||
                   !strcmp(inputType, "email") ||
                   !strcmp(inputType, "url") ||
                   !strcmp(inputType, "number") ||
                   !strcmp(inputType, "password") ||
                   !strcmp(inputType, "tel") ||
                   !strcmp(inputType, "date") ||
                   !strcmp(inputType, "time") ||
                   !strcmp(inputType, "month") ||
                   !strcmp(inputType, "week") ||
                   !strcmp(inputType, "datetime-local") ||
                   !strcmp(inputType, "color")) {
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_INPUT_FIELD);
            if (b != NULL) {
                b->inputType = pluto_strdup(inputType);
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                d_form_overrides(st, attrs, b);
                b->placeholder = pluto_strdup((ph != NULL) ? ph : "");
                int ok = 0;
                double sv = d_tonum(da_get(attrs, "size"), &ok);
                b->fieldWidth = ok ? (int)sv : -1;
                doc_add_block(st, b);
            }
        } else if (!strcmp(inputType, "submit") ||
                   !strcmp(inputType, "button")) {
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_INPUT_SUBMIT);
            if (b != NULL) {
                b->inputType = pluto_strdup(inputType);
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                d_form_overrides(st, attrs, b);
                const char* val = da_get(attrs, "value");
                if (val != NULL && val[0] != '\0')
                    b->submitLabel = pluto_strdup(val);
                else
                    b->submitLabel = pluto_strdup(
                        !strcmp(inputType, "button") ? "Button" : "Submit");
                doc_add_block(st, b);
            }
        } else if (!strcmp(inputType, "file") ||
                   !strcmp(inputType, "reset") ||
                   !strcmp(inputType, "image")) {
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_INPUT_SUBMIT);
            if (b != NULL) {
                b->inputType = pluto_strdup(inputType);
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                d_form_overrides(st, attrs, b);
                const char* val = da_get(attrs, "value");
                if (val != NULL && val[0] != '\0')
                    b->submitLabel = pluto_strdup(val);
                else
                    b->submitLabel = pluto_strdup(
                        !strcmp(inputType, "file")     ? "Choose File"
                        : !strcmp(inputType, "reset")  ? "Reset"
                                                       : "Submit");
                doc_add_block(st, b);
            }
        }
        pluto_free(ty);
        return;
    }

    if (!strcmp(tag, "textarea")) {
        doc_flush_block(st);
        int savedIn = st->inTextarea;
        char* savedName = st->textareaName;
        StrBuf savedBuf = st->textareaBuffer;
        sb_init(&st->textareaBuffer);
        const char* nm = da_get(attrs, "name");
        st->textareaName =
            pluto_strdup((nm != NULL && nm[0] != '\0') ? nm : "q");
        st->inTextarea = 1;
        d_walk_children(st, n);
        st->inTextarea = 0;
        int dis = da_has(attrs, "disabled") || st->disabledDepth > 0;
        DocBlock* b = d_new_block(DB_INPUT_FIELD);
        if (b != NULL) {
            b->inputType = pluto_strdup("textarea");
            DInputCommon c;
            d_input_common(st, attrs, &c);
            d_block_set_common(b, &c, st);
            b->inName = st->textareaName;
            st->textareaName = NULL;
            b->inValue = sb_detach(&st->textareaBuffer);
            const char* ph2 = da_get(attrs, "placeholder");
            b->placeholder = pluto_strdup((ph2 != NULL) ? ph2 : "");
            int ok = 0;
            double cv = d_tonum(da_get(attrs, "cols"), &ok);
            b->fieldWidth = ok ? (int)cv : -1;
            double rv = d_tonum(da_get(attrs, "rows"), &ok);
            b->fieldRows = ok ? (int)rv : -1;
            b->readonlyFlag =
                (unsigned char)(da_has(attrs, "readonly") || dis);
            b->blockInert = (unsigned char)((st->inert > 0) || dis);
            doc_add_block(st, b);
        }
        sb_clear(&st->textareaBuffer);
        st->textareaBuffer = savedBuf;
        pluto_free(st->textareaName);
        st->textareaName = savedName;
        st->inTextarea = savedIn;
        return;
    }

    if (!strcmp(tag, "button")) {
        char* bt = d_lower_dup(da_get(attrs, "type"));
        const char* btype = (bt != NULL) ? bt : "submit";
        if (!strcmp(btype, "submit") || !strcmp(btype, "button")) {
            char* raw = d_concat_node_text(n);
            d_collapse_trim(raw);
            int dis = da_has(attrs, "disabled") || st->disabledDepth > 0;
            doc_flush_block(st);
            DocBlock* b = d_new_block(DB_INPUT_SUBMIT);
            if (b != NULL) {
                b->inputType = pluto_strdup(btype);
                DInputCommon c;
                d_input_common(st, attrs, &c);
                d_block_set_common(b, &c, st);
                if (raw[0] != '\0')
                    b->submitLabel = pluto_strdup(raw);
                else
                    b->submitLabel = pluto_strdup(
                        !strcmp(btype, "button") ? "Button" : "Submit");
                const char* fa = da_get(attrs, "formaction");
                if (fa != NULL && fa[0] != '\0') {
                    StrBuf fb;
                    sb_init(&fb);
                    url_resolve(st->baseUrl, fa, &fb);
                    pluto_free(b->formAction);
                    b->formAction = sb_detach(&fb);
                }
                const char* fm = da_get(attrs, "formmethod");
                if (fm != NULL && fm[0] != '\0') {
                    pluto_free(b->formMethod);
                    b->formMethod = d_lower_dup(fm);
                }
                b->disabledFlag = (unsigned char)dis;
                b->blockInert = (unsigned char)((st->inert > 0) || dis);
                doc_add_block(st, b);
            }
            pluto_free(raw);
        }
        pluto_free(bt);
        return;
    }

    if (!strcmp(tag, "select")) {
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_SELECT_FIELD);
        if (b != NULL) {
            DInputCommon c;
            d_input_common(st, attrs, &c);
            d_collect_select_options(st, n, b);
            long sel = 1;
            for (size_t i = 0; i < b->nOptions; i++)
                if (b->options[i].selected && !b->options[i].disabled)
                    sel = (long)i + 1;
            b->selectedIndex = (int)sel;
            b->multipleFlag = (unsigned char)da_has(attrs, "multiple");
            d_block_set_common(b, &c, st);
            if (b->nOptions > 0)
                doc_add_block(st, b);
            else
                db_free(b);
        }
        return;
    }

    /* ── Bordered boxes ─────────────────────────────────────────────────── */

    if (!strcmp(tag, "fieldset")) {
        if (st->cell != NULL) {
            d_walk_children(st, n);
            return;
        }
        doc_flush_block(st);
        char* label = NULL;
        for (size_t i = 0; i < n->nChildren; i++) {
            DomNode* c = n->children[i];
            if (c->kind == DOM_ELEMENT && !strcmp(c->tag, "legend")) {
                label = d_concat_node_text(c);
                d_collapse_trim(label);
                break;
            }
        }
        DocBlock* b = d_new_block(DB_BOX_OPEN);
        if (b != NULL) {
            b->boxLabel = (label != NULL && label[0] != '\0')
                              ? pluto_strdup(label) : NULL;
            doc_add_block(st, b);
        }
        pluto_free(label);
        int wasDisabled = st->disabledDepth;
        if (da_has(attrs, "disabled")) st->disabledDepth++;
        for (size_t i = 0; i < n->nChildren; i++) {
            DomNode* c = n->children[i];
            if (!(c->kind == DOM_ELEMENT && !strcmp(c->tag, "legend")))
                d_walk(st, c);
        }
        st->disabledDepth = wasDisabled;
        DocBlock* cb = d_new_block(DB_BOX_CLOSE);
        if (cb != NULL) doc_add_block(st, cb);
        return;
    }

    if (!strcmp(tag, "details")) {
        if (st->cell != NULL) {
            d_walk_children(st, n);
            return;
        }
        doc_flush_block(st);
        st->detailsIndex++;
        char dkey[24];
        snprintf(dkey, sizeof(dkey), "d%d", st->detailsIndex);
        char* label = NULL;
        for (size_t i = 0; i < n->nChildren; i++) {
            DomNode* c = n->children[i];
            if (c->kind == DOM_ELEMENT && !strcmp(c->tag, "summary")) {
                label = d_concat_node_text(c);
                d_collapse_trim(label);
                break;
            }
        }
        int isOpen = da_has(attrs, "open");
        isOpen = d_details_override(st, dkey, isOpen);
        DocBlock* b = d_new_block(DB_BOX_OPEN);
        if (b != NULL) {
            if (label != NULL && label[0] != '\0') {
                StrBuf bl;
                sb_init(&bl);
                sb_append_str(&bl, "> ");
                sb_append_str(&bl, label);
                b->boxLabel = sb_detach(&bl);
            }
            b->toggleKey = pluto_strdup(dkey);
            b->toggleOpen = isOpen;
            doc_add_block(st, b);
        }
        pluto_free(label);
        if (isOpen) {
            for (size_t i = 0; i < n->nChildren; i++) {
                DomNode* c = n->children[i];
                if (!(c->kind == DOM_ELEMENT && !strcmp(c->tag, "summary")))
                    d_walk(st, c);
            }
        }
        DocBlock* cb = d_new_block(DB_BOX_CLOSE);
        if (cb != NULL) {
            cb->toggleKey = pluto_strdup(dkey);
            cb->toggleOpen = isOpen;
            doc_add_block(st, cb);
        }
        return;
    }

    if (!strcmp(tag, "dialog")) {
        if (st->cell != NULL) {
            d_walk_children(st, n);
            return;
        }
        if (!da_has(attrs, "open")) return;  /* closed dialog: not rendered */
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_BOX_OPEN);
        if (b != NULL) doc_add_block(st, b);
        d_walk_children(st, n);
        DocBlock* cb = d_new_block(DB_BOX_CLOSE);
        if (cb != NULL) doc_add_block(st, cb);
        return;
    }

    /* ── Media placeholders ─────────────────────────────────────────────── */

    if (!strcmp(tag, "video") || !strcmp(tag, "audio") ||
        !strcmp(tag, "iframe") || !strcmp(tag, "canvas") ||
        !strcmp(tag, "object") || !strcmp(tag, "embed") ||
        !strcmp(tag, "portal")) {
        d_handle_media_placeholder(st, tag, n, attrs);
        return;
    }

    if (!strcmp(tag, "progress") || !strcmp(tag, "meter")) {
        doc_flush_block(st);
        DocBlock* b = d_new_block(DB_METER);
        if (b != NULL) {
            int ok = 0;
            double v;
            v = d_tonum(da_get(attrs, "value"), &ok);
            b->mValue = ok ? v : 0;
            v = d_tonum(da_get(attrs, "max"), &ok);
            b->mMax = ok ? v : 1;
            if (b->mMax <= 0) b->mMax = 1;
            v = d_tonum(da_get(attrs, "min"), &ok);
            b->mMin = ok ? v : 0;
            v = d_tonum(da_get(attrs, "low"), &ok);
            b->mLow = ok ? v : 0;
            v = d_tonum(da_get(attrs, "high"), &ok);
            b->mHigh = ok ? v : b->mMax;
            v = d_tonum(da_get(attrs, "optimum"), &ok);
            b->mOptimum = ok ? v : 0;
            const char* lb = da_get(attrs, "title");
            b->boxLabel = pluto_strdup((lb != NULL) ? lb : "");
            doc_add_block(st, b);
        }
        return;
    }

    if (!strcmp(tag, "map")) {
        const char* nameAttr = da_get(attrs, "name");
        const char* name = nameAttr;
        if (name != NULL && name[0] == '#') name++;
        if (name != NULL && name[0] != '\0') {
            DocMap m;
            memset(&m, 0, sizeof(m));
            m.name = pluto_strdup(name);
            d_collect_areas(st, n, &m);
            if (m.name != NULL) {
                DocDocument* doc = st->doc;
                DocMap* arr = pluto_realloc(
                    doc->maps, (doc->nMaps + 1) * sizeof(DocMap));
                if (arr != NULL) {
                    doc->maps = arr;
                    doc->capMaps = doc->nMaps + 1;
                    doc->maps[doc->nMaps++] = m;
                } else {
                    d_map_free(&m);
                }
            } else {
                d_map_free(&m);
            }
        }
        return;
    }

    if (!strcmp(tag, "datalist")) {
        const char* id = da_get(attrs, "id");
        if (id != NULL && id[0] != '\0') {
            DocDatalist dl;
            memset(&dl, 0, sizeof(dl));
            dl.id = pluto_strdup(id);
            for (size_t i = 0; i < n->nChildren; i++) {
                DomNode* c = n->children[i];
                if (c->kind != DOM_ELEMENT || strcmp(c->tag, "option") != 0)
                    continue;
                char* text = d_concat_node_text(c);
                d_collapse_trim(text);
                const char* val = da_get(c->attrs, "value");
                DocDatalistOpt* arr = pluto_realloc(
                    dl.opts, (dl.nOpts + 1) * sizeof(DocDatalistOpt));
                if (arr == NULL) {
                    pluto_free(text);
                    break;
                }
                dl.opts = arr;
                dl.capOpts = dl.nOpts + 1;
                dl.opts[dl.nOpts].text = text;
                dl.opts[dl.nOpts].value = pluto_strdup(
                    (val != NULL) ? val : text);
                dl.nOpts++;
            }
            if (dl.id != NULL) {
                DocDocument* doc = st->doc;
                DocDatalist* arr = pluto_realloc(
                    doc->datalists,
                    (doc->nDatalists + 1) * sizeof(DocDatalist));
                if (arr != NULL) {
                    doc->datalists = arr;
                    doc->capDatalists = doc->nDatalists + 1;
                    doc->datalists[doc->nDatalists++] = dl;
                } else {
                    d_datalist_free(&dl);
                }
            } else {
                d_datalist_free(&dl);
            }
        }
        return;
    }

    if (!strcmp(tag, "template") || !strcmp(tag, "menuitem") ||
        !strcmp(tag, "content") || !strcmp(tag, "shadow") ||
        !strcmp(tag, "geolocation"))
        return;   /* inert / obsolete: children never rendered */

    if (!strcmp(tag, "fencedframe")) {
        doc_flush_block(st);
        int okw = 0, okh = 0;
        double wv = d_tonum(da_get(attrs, "width"), &okw);
        double hv = d_tonum(da_get(attrs, "height"), &okh);
        int w = okw ? (int)wv : 160;
        int h = okh ? (int)hv : 60;
        if (w > 360) w = 360;
        if (h > 120) h = 120;
        DocBlock* b = d_new_block(DB_PLACEHOLDER);
        if (b != NULL) {
            b->phTag = pluto_strdup("fencedframe");
            b->boxLabel = pluto_strdup("[fencedframe]");
            b->width = w;
            b->height = h;
            doc_add_block(st, b);
        }
        return;
    }

    if (!strcmp(tag, "svg")) {
        doc_flush_block(st);
        char* xml = d_serialize_svg_node(n);
        int okw = 0, okh = 0;
        double wv = d_tonum(da_get(attrs, "width"), &okw);
        double hv = d_tonum(da_get(attrs, "height"), &okh);
        int w = (okw && wv > 0) ? (int)wv : 0;
        int h = (okh && hv > 0) ? (int)hv : 0;
        if (w <= 0 || h <= 0) {
            double vbW = 0, vbH = 0;
            d_parse_viewbox(da_get(attrs, "viewBox"), &vbW, &vbH);
            if (w <= 0 && vbW > 0) w = (int)vbW;
            if (h <= 0 && vbH > 0) h = (int)vbH;
        }
        if (w <= 0) w = 120;
        if (h <= 0) h = 40;
        if (w > 360) w = 360;
        if (h > 180) h = 180;
        const char* alt = "";
        if (d_str_eq_ci(da_get(attrs, "role"), "img")) {
            const char* al = da_get(attrs, "aria-label");
            if (al == NULL || al[0] == '\0') al = da_get(attrs, "title");
            alt = (al != NULL) ? al : "";
        }
        DocBlock* b = d_new_block(DB_IMAGE);
        if (b != NULL) {
            b->imgIsSvg = 1;
            b->svgXml = xml;
            b->width = w;
            b->height = h;
            b->alt = pluto_strdup(alt);
            b->imgHref = (st->currentHref != NULL)
                             ? pluto_strdup(st->currentHref) : NULL;
            b->align = d_parse_align(attrs);
            b->imgInert = st->inert > 0;
            doc_add_block(st, b);
        } else {
            pluto_free(xml);
        }
        return;
    }

    /* MathML linearization */

    if (!strcmp(tag, "math")) {
        doc_flush_block(st);
        int wasMath = st->inMath;
        StrBuf savedMath = st->mathParts;
        sb_init(&st->mathParts);
        st->inMath = 1;
        d_walk_children(st, n);
        st->inMath = wasMath;
        if (st->mathParts.len > 0 && st->mathParts.data != NULL) {
            /* StrBuf contents are not NUL-terminated until detach */
            if (sb_reserve(&st->mathParts, 1))
                st->mathParts.data[st->mathParts.len] = '\0';
            d_collapse_trim(st->mathParts.data);
        }
        if (st->mathParts.len > 0 && st->mathParts.data != NULL &&
            st->mathParts.data[0] != '\0') {
            DocBlock* b = d_new_block(DB_MATH);
            if (b != NULL) {
                b->codeText = sb_detach(&st->mathParts);
                doc_add_block(st, b);
            } else {
                sb_clear(&st->mathParts);
            }
        } else {
            sb_clear(&st->mathParts);
        }
        st->mathParts = savedMath;
        return;
    }

    if (st->inMath && !strcmp(tag, "mfrac")) {
        for (size_t i = 0; i < n->nChildren; i++) {
            if (i > 0) sb_append_str(&st->mathParts, " / ");
            d_walk(st, n->children[i]);
        }
        return;
    }
    if (st->inMath && !strcmp(tag, "msup")) {
        for (size_t i = 0; i < n->nChildren; i++) {
            if (i > 0) sb_append_char(&st->mathParts, '^');
            d_walk(st, n->children[i]);
        }
        return;
    }
    if (st->inMath && !strcmp(tag, "msub")) {
        for (size_t i = 0; i < n->nChildren; i++) {
            if (i > 0) sb_append_char(&st->mathParts, '_');
            d_walk(st, n->children[i]);
        }
        return;
    }
    if (st->inMath && !strcmp(tag, "msubsup")) {
        for (size_t i = 0; i < n->nChildren; i++) {
            if (i == 1) sb_append_char(&st->mathParts, '_');
            if (i == 2) sb_append_char(&st->mathParts, '^');
            d_walk(st, n->children[i]);
        }
        return;
    }
    if (st->inMath && !strcmp(tag, "msqrt")) {
        sb_append_str(&st->mathParts, "sqrt(");
        d_walk_children(st, n);
        sb_append_char(&st->mathParts, ')');
        return;
    }
    if (st->inMath && !strcmp(tag, "mroot")) {
        sb_append_str(&st->mathParts, "sqrt(");
        for (size_t i = 0; i < n->nChildren; i++) {
            if (i > 0) sb_append_str(&st->mathParts, "^(1/");
            if (i + 1 == n->nChildren) sb_append_char(&st->mathParts, ')');
            d_walk(st, n->children[i]);
        }
        sb_append_char(&st->mathParts, ')');
        return;
    }
    if (st->inMath && !strcmp(tag, "mfenced")) {
        char* openCh = d_mfenced_attr(attrs, "open", "(");
        char* closeCh = d_mfenced_attr(attrs, "close", ")");
        char* sep = d_mfenced_attr(attrs, "separators", ",");
        sb_append_str(&st->mathParts, openCh);
        size_t count = 0;
        for (size_t i = 0; i < n->nChildren; i++) {
            if (count > 0) sb_append_str(&st->mathParts, sep);
            d_walk(st, n->children[i]);
            count++;
        }
        sb_append_str(&st->mathParts, closeCh);
        pluto_free(openCh);
        pluto_free(closeCh);
        pluto_free(sep);
        return;
    }
    if (st->inMath && !strcmp(tag, "mspace")) {
        sb_append_char(&st->mathParts, ' ');
        return;
    }

    if (!strcmp(tag, "meta")) {
        const char* he = da_get(attrs, "http-equiv");
        const char* ct = da_get(attrs, "content");
        if (he != NULL && ct != NULL && strcasecmp(he, "refresh") == 0) {
            double delay = 0;
            char* u = NULL;
            if (d_parse_meta_content(ct, &delay, &u)) {
                st->hasMetaRefresh = 1;
                st->metaDelay = delay;
                pluto_free(st->metaUrl);
                st->metaUrl = u;
            }
        }
        return;
    }

    if (!strcmp(tag, "base") || !strcmp(tag, "link") ||
        !strcmp(tag, "col") || !strcmp(tag, "colgroup") ||
        !strcmp(tag, "source") || !strcmp(tag, "track") ||
        !strcmp(tag, "param") || !strcmp(tag, "frameset") ||
        !strcmp(tag, "frame"))
        return;

    d_walk_children(st, n);
}

/* ── walker ───────────────────────────────────────────────────────────── */

static void d_walk_children(DocState* st, DomNode* node) {
    for (size_t i = 0; i < node->nChildren; i++)
        d_walk(st, node->children[i]);
}

static void d_walk(DocState* st, DomNode* n) {
    tasks_yield_check();
    if (n->kind == DOM_TEXT) {
        d_handle_text_node(st, n);
        return;
    }
    if (n->kind != DOM_ELEMENT) return;
    if (d_is_display_none(n->attrs)) return;
    int inertHere = da_has(n->attrs, "inert");
    if (inertHere) st->inert++;
    d_handle_element(st, n);
    if (inertHere) st->inert--;
}

/* ── finalization ─────────────────────────────────────────────────────── */

static void d_push_notice(DocState* st, const char* text, int italic) {
    DocBlock* b = d_new_block(DB_PARAGRAPH);
    if (b == NULL) return;
    b->align = "center";
    DocInline in;
    memset(&in, 0, sizeof(in));
    in.type = DIT_TEXT;
    in.text = pluto_strdup(text);
    if (italic)
        in.italic = 1;
    else
        in.bold = 1;
    db_push_inline(b, in);
    DocDocument* doc = st->doc;
    DocBlock* arr = pluto_realloc(doc->blocks,
                                  (doc->nBlocks + 1) * sizeof(DocBlock));
    if (arr == NULL) {
        db_free(b);
        return;
    }
    doc->blocks = arr;
    doc->capBlocks = doc->nBlocks + 1;
    doc->blocks[doc->nBlocks++] = *b;
    pluto_free(b);
}

static char* d_resolve_meta_url(const char* baseUrl, const char* raw) {
    if (raw == NULL || raw[0] == '\0') return NULL;
    StrBuf r;
    sb_init(&r);
    url_resolve(baseUrl, raw, &r);
    return sb_detach(&r);
}

static void d_finalize(DocState* st, int scannedHas, double scannedDelay,
                       char* scannedUrl) {
    DocDocument* doc = st->doc;
    doc_flush_block(st);

    if (st->truncated)
        d_push_notice(st, "(Page too large - rest not rendered)", 0);

    if (doc->nBlocks == 0)
        d_push_notice(st, "(Empty Web Page)", 1);

    if (st->hasMetaRefresh) {
        doc->hasMetaRefresh = 1;
        doc->metaDelay = st->metaDelay;
        doc->metaUrl = d_resolve_meta_url(doc->baseUrl, st->metaUrl);
        pluto_free(st->metaUrl);
        st->metaUrl = NULL;
        pluto_free(scannedUrl);
    } else if (scannedHas) {
        doc->hasMetaRefresh = 1;
        doc->metaDelay = scannedDelay;
        doc->metaUrl = d_resolve_meta_url(doc->baseUrl, scannedUrl);
        pluto_free(scannedUrl);
    } else {
        pluto_free(scannedUrl);
    }
    pluto_free(st->metaUrl);
    st->metaUrl = NULL;
}

/* ── base-href validation: ^[a-zA-Z][%w+%-.]*:// ──────────────────────── */

static int d_valid_absolute_url(const char* s) {
    if (s == NULL) return 0;
    unsigned char c0 = (unsigned char)s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return 0;
    size_t i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' ||
           s[i] == '.')
        i++;
    return s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/';
}

/* ── entry point ──────────────────────────────────────────────────────── */

DocDocument* doc_parse_opts(const char* html, const char* baseUrl, int mode,
                            const DocParseOpts* opts) {
    tasks_yield_check();

    if (html == NULL || html[0] == '\0') {
        DocDocument* d = pluto_malloc(sizeof(DocDocument));
        if (d == NULL) return NULL;
        memset(d, 0, sizeof(*d));
        d->title = pluto_strdup("Blank Page");
        d->baseUrl = pluto_strdup((baseUrl != NULL && baseUrl[0]) ? baseUrl
                                                                  : "about:blank");
        d->rawHtml = pluto_strdup("");
        return d;
    }

    HttTokens* tokens = htt_tokenize(html, strlen(html));
    if (tokens == NULL) return NULL;

    if (mode == PLUTO_MODE_READER) {
        PLUTO_LOG("Reader-mode parsing arrives in Phase 12");
        htt_free(tokens);
        return NULL;
    }

    /* token-level scans run BEFORE dom_build(): dom_build transfers
     * ownership of token attrs/text, so scans must read them first */
    const char* effBase = (baseUrl != NULL && baseUrl[0]) ? baseUrl
                                                          : "about:blank";
    char* baseOverride = NULL;
    for (size_t i = 0; i < tokens->count; i++) {
        HttToken* t = &tokens->items[i];
        if (t->type != HTT_TAG || t->isClosing || t->name == NULL ||
            strcmp(t->name, "base") != 0)
            continue;
        const char* href = da_get(t->attrs, "href");
        if (href != NULL && href[0] != '\0') {
            if (d_valid_absolute_url(href)) baseOverride = pluto_strdup(href);
            break;
        }
    }
    if (baseOverride != NULL) effBase = baseOverride;

    int scannedHas = 0;
    double scannedDelay = 0.0;
    char* scannedUrl = NULL;
    for (size_t i = 0; i < tokens->count && !scannedHas; i++) {
        HttToken* t = &tokens->items[i];
        if (t->type != HTT_TAG || t->isClosing || t->name == NULL ||
            strcmp(t->name, "meta") != 0)
            continue;
        const char* he = da_get(t->attrs, "http-equiv");
        const char* ct = da_get(t->attrs, "content");
        if (he == NULL || ct == NULL || strcasecmp(he, "refresh") != 0)
            continue;
        double delay = 0;
        char* u = NULL;
        if (d_parse_meta_content(ct, &delay, &u)) {
            scannedHas = 1;
            scannedDelay = delay;
            scannedUrl = u;
        }
    }

    DomDiag diag;
    memset(&diag, 0, sizeof(diag));
    DomNode* root = dom_build(tokens, &diag);

    DocDocument* doc = pluto_malloc(sizeof(DocDocument));
    if (doc == NULL) {
        pluto_free(baseOverride);
        pluto_free(scannedUrl);
        if (root != NULL) dom_free(root);
        htt_free(tokens);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->title = pluto_strdup((tokens->pageTitle != NULL) ? tokens->pageTitle
                                                          : "Web Page");
    doc->baseUrl = pluto_strdup(effBase);
    doc->rawHtml = pluto_strdup(html);
    if (doc->title == NULL || doc->baseUrl == NULL || doc->rawHtml == NULL) {
        doc_free(doc);
        pluto_free(baseOverride);
        pluto_free(scannedUrl);
        if (root != NULL) dom_free(root);
        htt_free(tokens);
        return NULL;
    }

    DocState st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.baseUrl = doc->baseUrl;
    st.currentAnchorIndex = -1;
    st.opts = opts;
    sb_init(&st.linkText);
    sb_init(&st.preBuffer);
    sb_init(&st.figCaption);
    sb_init(&st.mathParts);
    sb_init(&st.textareaBuffer);

    if (root != NULL) d_walk_children(&st, root);

    d_finalize(&st, scannedHas, scannedDelay, scannedUrl);

    sb_clear(&st.linkText);
    sb_clear(&st.preBuffer);
    sb_clear(&st.figCaption);
    sb_clear(&st.mathParts);
    sb_clear(&st.textareaBuffer);
    pluto_free(st.textareaName);
    pluto_free(baseOverride);
    if (root != NULL) dom_free(root);
    htt_free(tokens);
    return doc;
}

void doc_init(struct PlaydateAPI* pd) {
    (void)pd;
}

void doc_free(DocDocument* doc) {
    if (doc == NULL) return;
    pluto_free(doc->title);
    pluto_free(doc->baseUrl);
    pluto_free(doc->rawHtml);
    pluto_free(doc->metaUrl);
    for (size_t i = 0; i < doc->nBlocks; i++) db_free_fields(&doc->blocks[i]);
    pluto_free(doc->blocks);
    for (size_t i = 0; i < doc->nLinks; i++) {
        pluto_free(doc->links[i].href);
        pluto_free(doc->links[i].text);
        pluto_free(doc->links[i].target);
    }
    pluto_free(doc->links);
    for (size_t i = 0; i < doc->nMaps; i++) d_map_free(&doc->maps[i]);
    pluto_free(doc->maps);
    for (size_t i = 0; i < doc->nDatalists; i++)
        d_datalist_free(&doc->datalists[i]);
    pluto_free(doc->datalists);
    pluto_free(doc);
}

