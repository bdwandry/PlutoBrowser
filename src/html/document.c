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
    StrBuf textareaBuffer;

    int inMath;
    StrBuf mathParts;

    DomNode* currentTableCell; /* P11: tables */
    int cellFirstText;

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

    int hasMetaRefresh;
    double metaDelay;
    char* metaUrl;             /* owned */
};

static void di_free(DocInline* in) {
    pluto_free(in->text);
    pluto_free(in->href);
}

static void db_free_contents(DocBlock* b) {
    for (size_t i = 0; i < b->nInlines; i++) di_free(&b->inlines[i]);
    pluto_free(b->inlines);
    b->inlines = NULL;
    b->nInlines = b->capInlines = 0;
}

static void db_free(DocBlock* b) {
    if (b == NULL) return;
    db_free_contents(b);
    pluto_free(b->codeText);
    for (size_t i = 0; i < b->nLines; i++) pluto_free(b->lines[i]);
    pluto_free(b->lines);
    pluto_free(b->src);
    pluto_free(b->alt);
    pluto_free(b->caption);
    pluto_free(b->imgHref);
    pluto_free(b->usemap);
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

/* ── text node routing ────────────────────────────────────────────────── */

static void d_handle_text_node(DocState* st, DomNode* n) {
    const char* text = (n->text != NULL) ? n->text : "";
    if (text[0] == '\0') return;
    if (st->inPre) { sb_append_str(&st->preBuffer, text); return; }
    if (st->inTextarea) { sb_append_str(&st->textareaBuffer, text); return; }
    if (st->currentTableCell != NULL) return;              /* P11: tables */
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
        DocBlock* b = d_new_block(DB_HEADING);
        if (b == NULL) return;
        b->level = tag[1] - '0';
        b->spacingTop = 18;
        b->spacingBottom = 8;
        b->align = d_parse_align(attrs);
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
        !strcmp(tag, "noindex") || !strcmp(tag, "search")) {
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

    if (!strcmp(tag, "meta")) {
        const char* he = da_get(attrs, "http-equiv");
        const char* ct = da_get(attrs, "content");
        if (he != NULL && ct != NULL && strcasecmp(he, "refresh") == 0 &&
            !st->hasMetaRefresh) {
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
        !strcmp(tag, "input") || !strcmp(tag, "area") ||
        !strcmp(tag, "col") || !strcmp(tag, "embed") ||
        !strcmp(tag, "source") || !strcmp(tag, "track") ||
        !strcmp(tag, "param"))
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
    int inertHere = da_has(n->attrs, "aria-hidden");
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
        d_push_notice(st, "(Page truncated: too many blocks)", 0);

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

DocDocument* doc_parse(const char* html, const char* baseUrl, int mode) {
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
    sb_init(&st.linkText);
    sb_init(&st.preBuffer);
    sb_init(&st.figCaption);
    sb_init(&st.mathParts);

    if (root != NULL) d_walk_children(&st, root);

    d_finalize(&st, scannedHas, scannedDelay, scannedUrl);

    sb_clear(&st.linkText);
    sb_clear(&st.preBuffer);
    sb_clear(&st.figCaption);
    sb_clear(&st.mathParts);
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
    for (size_t i = 0; i < doc->nBlocks; i++) db_free_contents(&doc->blocks[i]);
    pluto_free(doc->blocks);
    for (size_t i = 0; i < doc->nLinks; i++) {
        pluto_free(doc->links[i].href);
        pluto_free(doc->links[i].text);
        pluto_free(doc->links[i].target);
    }
    pluto_free(doc->links);
    pluto_free(doc);
}

