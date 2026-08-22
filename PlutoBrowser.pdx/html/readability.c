// readability.c — C port of Source/html/readability.lua (Readability.distill).
//
// Smart Article Extractor & Reader Mode Distiller.
// See readability.h for the parity notes.

#include "html/readability.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/logger.h"
#include "core/tasks.h"
#include "core/url.h"
#include "util/dynarray.h"
#include "util/mem.h"
#include "util/strbuf.h"

/* ── small string helpers (Lua trimStr / wordCount) ───────────────────── */

static size_t r_trim(char* s) {
    if (s == NULL) return 0;
    size_t n = strlen(s);
    size_t b = 0;
    while (b < n && isspace((unsigned char)s[b])) b++;
    while (n > b && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + b, n - b);
    s[n - b] = '\0';
    return n - b;
}

static int r_wordcount(const char* s) {
    if (s == NULL) return 0;
    int count = 0;
    int inWord = 0;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) {
            inWord = 0;
        } else if (!inWord) {
            inWord = 1;
            count++;
        }
    }
    return count;
}

/* Lua "[%.%?!%:]%s*$" */
static int r_ends_sentence(const char* s) {
    if (s == NULL) return 0;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0) return 0;
    char c = s[n - 1];
    return c == '.' || c == '?' || c == '!' || c == ':';
}

/* True when the text is just a bare URL/domain (e.g.
 * "en.wikipedia.org/wiki/Mesklin"). Such anchors only duplicate the target
 * address, so reader mode hides them (Safari-style reading). */
static int r_is_bare_url(const char* text) {
    if (text == NULL) return 0;
    char* t = pluto_strdup(text);
    if (t == NULL) return 0;
    size_t n = r_trim(t);
    if (n == 0) {
        pluto_free(t);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (isspace((unsigned char)t[i])) {
            pluto_free(t);
            return 0;
        }
    }
    int bare = 0;
    if ((strncmp(t, "http://", 7) == 0 || strncmp(t, "https://", 8) == 0)) {
        bare = 1; /* rest already verified space-free above */
    } else {
        /* ^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*
         * ([/%?][^%s]*)?$  -- the leading run is greedy, so candidate dots
         * are tried right-to-left like regex backtracking would */
        if (isalnum((unsigned char)t[0])) {
            size_t runEnd = 1;
            while (isalnum((unsigned char)t[runEnd]) ||
                   t[runEnd] == '-' || t[runEnd] == '.')
                runEnd++;
            for (size_t j = runEnd; j-- > 1;) {
                if (t[j] != '.') continue;
                if (!isalpha((unsigned char)t[j + 1])) continue;
                size_t k = j + 2;
                while (isalnum((unsigned char)t[k]) || t[k] == '-') k++;
                if (t[k] == '\0') {
                    bare = 1;
                } else if (t[k] == '/' || t[k] == '?') {
                    bare = 1;
                    for (; t[k]; k++) {
                        if (isspace((unsigned char)t[k])) bare = 0;
                    }
                }
                break; /* rightmost dot inside the run decided the outcome */
            }
        }
    }
    pluto_free(t);
    return bare;
}

/* Lua tonumber() for width/height attributes: NULL/invalid -> fallback */
static double r_tonum_or(const char* s, double fallback) {
    if (s == NULL) return fallback;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    double v = strtod(s, &end);
    if (end == s) return fallback;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return fallback;
    return v;
}

/* ── attr access (values are char*; valueless attrs are HT_ATTR_TRUE) ─── */

static const char* r_attr(const StrMap* m, const char* key) {
    if (m == NULL) return NULL;
    void* v = sm_get(m, key);
    if (v == NULL || v == HT_ATTR_TRUE) return NULL;
    return (const char*)v;
}

/* ── block helpers ────────────────────────────────────────────────────── */

static void r_blk_init(DocBlock* b, int type) {
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->fieldWidth = -1;
    b->fieldRows = -1;
    b->maxlength = -1;
}

/* deep-free a block whose contents are still owned (mirrors db_free_fields
 * scope; only fields this module ever sets matter, but freeing every field
 * keeps it future-proof) */
void doc_free_block_fields(DocBlock* b);

static void r_inl_push(DocBlock* b, const DocInline* in) {
    if (b->nInlines == b->capInlines) {
        size_t nc = b->capInlines ? b->capInlines * 2 : 4;
        DocInline* ni =
            (DocInline*)pluto_realloc(b->inlines, nc * sizeof(DocInline));
        if (ni == NULL) return;
        b->inlines = ni;
        b->capInlines = nc;
    }
    b->inlines[b->nInlines++] = *in;
}

/* full text of an inline-carrier block, trimmed (Lua flushBlock/addText) */
static char* r_concat_text(const DocBlock* b) {
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < b->nInlines; i++)
        if (b->inlines[i].text != NULL)
            sb_append_str(&sb, b->inlines[i].text);
    char* out = sb_detach(&sb);
    r_trim(out);
    return out;
}

/* ── distill state ────────────────────────────────────────────────────── */

typedef struct {
    double score;
    int wordCount;
    int textLen;
    int imageCount;
    int linkWords;
    int isContent;
    int isNav;
    size_t origIndex;
    DocBlock* blocks;
    size_t nBlocks;
    size_t capBlocks;
} RContainer;

typedef struct {
    int isOl;
    int count;
} RListLevel;

typedef struct {
    RContainer* conts;
    size_t nConts, capConts;
    size_t curIdx;

    int stripDepth;
    int isBold, isItalic, isCode;
    char* currentHref;

    RListLevel* listStack;
    size_t nList, capList;

    int inPre;
    StrBuf preBuffer;
    int inBlockquote;

    DocBlock pending;
    int hasPending;

    char* formAction;     /* owned, may be "" */
    char* formMethod;     /* owned, default "get" */

    int inTextarea;
    char* textareaName;   /* owned */
    StrBuf textareaBuffer;

    int inButton;
    StrBuf buttonLabel;
    char* buttonFormAction; /* owned snapshot */
} RD;

static RContainer* r_cur(RD* s) { return &s->conts[s->curIdx]; }

static void r_cont_push_block(RContainer* c, const DocBlock* b) {
    if (c->nBlocks == c->capBlocks) {
        size_t nc = c->capBlocks ? c->capBlocks * 2 : 4;
        DocBlock* nb =
            (DocBlock*)pluto_realloc(c->blocks, nc * sizeof(DocBlock));
        if (nb == NULL) return;
        c->blocks = nb;
        c->capBlocks = nc;
    }
    c->blocks[c->nBlocks++] = *b;
}

/* Lua flushBlock */
static void r_flush(RD* s, DocBlock* b) {
    if (b == NULL) return;
    RContainer* c = r_cur(s);
    if (b->type == DB_HR || b->type == DB_IMAGE ||
        b->type == DB_INPUT_FIELD || b->type == DB_INPUT_SUBMIT) {
        r_cont_push_block(c, b);
        if (b->type == DB_IMAGE) c->imageCount++;
        return;
    }

    char* fullText = r_concat_text(b);
    int drop = (fullText[0] == '\0' && b->type != DB_CODE_BLOCK);

    if (!drop) {
        int wc = r_wordcount(fullText);
        c->wordCount += wc;
        c->textLen += (int)strlen(fullText);
        for (size_t i = 0; i < b->nInlines; i++)
            if (b->inlines[i].href != NULL)
                c->linkWords += r_wordcount(b->inlines[i].text);
        r_cont_push_block(c, b);
    }
    pluto_free(fullText);
    if (drop) doc_free_block_fields(b);
}

static void r_commit(RD* s) {
    if (s->hasPending) {
        r_flush(s, &s->pending);
        s->hasPending = 0;
    }
}

static void r_ensure(RD* s) {
    if (s->hasPending) return;
    r_blk_init(&s->pending, s->inBlockquote ? DB_BLOCKQUOTE : DB_PARAGRAPH);
    s->hasPending = 1;
}

static void r_add_text(RD* s, const char* text, size_t len) {
    if (s->stripDepth > 0) return;
    if (text == NULL || len == 0) return;
    char* t = pluto_strndup(text, len);
    if (t == NULL) return;
    if (!s->inPre) {
        /* [\r\n\t]+ -> " " */
        for (char* p = t; *p; p++) {
            if (*p == '\r' || *p == '\n' || *p == '\t') {
                *p = ' ';
                size_t r2 = (size_t)(p - t) + 1;
                size_t w2 = r2;
                while (t[r2] == '\r' || t[r2] == '\n' || t[r2] == '\t') r2++;
                if (r2 > w2) memmove(t + w2, t + r2, strlen(t + r2) + 1);
            }
        }
    }
    if (t[0] == '\0' || (t[0] == ' ' && t[1] == '\0')) {
        pluto_free(t);
        return;
    }
    if (s->currentHref != NULL && r_is_bare_url(t)) {
        pluto_free(t);
        return;
    }
    r_ensure(s);
    DocInline in;
    memset(&in, 0, sizeof(in));
    in.type = DIT_TEXT;
    in.text = t;
    in.bold = (unsigned char)(s->isBold != 0);
    in.italic = (unsigned char)(s->isItalic != 0);
    in.code = (unsigned char)(s->isCode != 0);
    in.underline = (unsigned char)(s->currentHref != NULL);
    in.href = (s->currentHref != NULL) ? pluto_strdup(s->currentHref) : NULL;
    r_inl_push(&s->pending, &in);
}

static void r_push_container(RD* s, int isContent, int isNav) {
    r_commit(s);
    if (s->nConts == s->capConts) {
        size_t nc = s->capConts ? s->capConts * 2 : 4;
        RContainer* ns =
            (RContainer*)pluto_realloc(s->conts, nc * sizeof(RContainer));
        if (ns == NULL) return;
        s->conts = ns;
        s->capConts = nc;
    }
    RContainer* c = &s->conts[s->nConts++];
    memset(c, 0, sizeof(*c));
    c->origIndex = s->nConts - 1;
    c->isContent = isContent;
    c->isNav = isNav;
    s->curIdx = s->nConts - 1;
}

static void r_pop_container(RD* s) {
    r_commit(s);
    if (s->curIdx > 0) s->curIdx--;
}

/* ── tag dispatch ─────────────────────────────────────────────────────── */

static int r_is_strip_tag(const char* tg) {
    return !strcmp(tg, "script") || !strcmp(tg, "style") ||
           !strcmp(tg, "noscript") || !strcmp(tg, "svg") ||
           !strcmp(tg, "nav") || !strcmp(tg, "footer") ||
           !strcmp(tg, "aside") || !strcmp(tg, "header") ||
           !strcmp(tg, "iframe");
}

static int r_ci_prefix(const char* s, const char* lowPrefix) {
    for (size_t i = 0; lowPrefix[i]; i++) {
        if (tolower((unsigned char)s[i]) != lowPrefix[i]) return 0;
    }
    return 1;
}

static char* r_resolve(const char* base, const char* rel) {
    StrBuf rb;
    sb_init(&rb);
    url_resolve(base, rel, &rb);
    return sb_detach(&rb);
}

static void r_flush_input_field(RD* s, const char* inputType,
                                const char* name, const char* value,
                                const char* placeholder) {
    r_commit(s);
    DocBlock b;
    r_blk_init(&b, DB_INPUT_FIELD);
    b.inputType = pluto_strdup(inputType);
    b.inName = pluto_strdup(name);
    b.inValue = pluto_strdup(value);
    b.placeholder = pluto_strdup(placeholder);
    if (s->formAction[0] != '\0')
        b.formAction = pluto_strdup(s->formAction);
    b.formMethod = pluto_strdup(s->formMethod);
    r_flush(s, &b);
}

static void r_flush_input_submit(RD* s, const char* label,
                                 const char* actionSnapshot) {
    r_commit(s);
    DocBlock b;
    r_blk_init(&b, DB_INPUT_SUBMIT);
    b.submitLabel = pluto_strdup(label);
    /* input-submit passes NULL -> use the LIVE form action; button-close
     * passes its open-time snapshot (possibly "") */
    const char* act = (actionSnapshot != NULL) ? actionSnapshot : s->formAction;
    if (act[0] != '\0') b.formAction = pluto_strdup(act);
    b.formMethod = pluto_strdup(s->formMethod);
    r_flush(s, &b);
}

static void r_handle_img(RD* s, StrMap* attrs, const char* baseUrl) {
    const char* src = r_attr(attrs, "src");
    if (src == NULL || src[0] == '\0') src = r_attr(attrs, "data-src");
    char* srcBuf = NULL;
    if (src == NULL || src[0] == '\0') {
        src = NULL;
        const char* srcset = r_attr(attrs, "srcset");
        if (srcset != NULL) {
            /* first ^([^%s,]+) */
            size_t n = 0;
            while (srcset[n] != '\0' && !isspace((unsigned char)srcset[n]) &&
                   srcset[n] != ',')
                n++;
            if (n > 0) {
                srcBuf = pluto_strndup(srcset, n);
                src = srcBuf;
            }
        }
    }
    const char* altAttr = r_attr(attrs, "alt");
    if (altAttr == NULL) altAttr = r_attr(attrs, "title");
    int w = (int)r_tonum_or(r_attr(attrs, "width"), 160.0);
    int h = (int)r_tonum_or(r_attr(attrs, "height"), 80.0);

    if (src != NULL && src[0] != '\0' && !strstr(src, "tracking") &&
        !strstr(src, "beacon")) {
        if (w > 360) w = 360;
        if (h > 180) h = 180;
        r_commit(s);
        DocBlock b;
        r_blk_init(&b, DB_IMAGE);
        b.src = r_resolve(baseUrl, src);
        b.alt = pluto_strdup((altAttr != NULL && altAttr[0] != '\0')
                                 ? altAttr : "Image");
        b.width = w;
        b.height = h;
        b.imgHref = (s->currentHref != NULL)
                        ? pluto_strdup(s->currentHref) : NULL;
        r_flush(s, &b);
    }
    pluto_free(srcBuf);
}

static void r_handle_tag(RD* s, HttToken* t, const char* baseUrl) {
    (void)0;
    const char* tag = t->name;
    int closing = t->isClosing;
    StrMap* attrs = t->attrs;

    if (r_is_strip_tag(tag)) {
        if (!closing)
            s->stripDepth++;
        else if (s->stripDepth > 0)
            s->stripDepth--;
        return;
    }
    if (s->stripDepth > 0) return;

    /* NOTE: the Lua source has an elseif branch pushing nav/header/footer/
     * aside containers here, but those tags never reach it because they are
     * STRIP_TAGS handled above -- dead code, omitted. */

    if (!strcmp(tag, "article") || !strcmp(tag, "main")) {
        if (!closing)
            r_push_container(s, 1, 0);
        else
            r_pop_container(s);

    } else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' &&
               tag[2] == '\0') {
        r_commit(s);
        int level = tag[1] - '0';
        if (!closing) {
            r_blk_init(&s->pending, DB_HEADING);
            s->pending.level = level;
            s->hasPending = 1;
        } else {
            r_commit(s);
        }

    } else if (!strcmp(tag, "p") || !strcmp(tag, "div") ||
               !strcmp(tag, "section")) {
        r_commit(s);

    } else if (!strcmp(tag, "br")) {
        if (s->inPre) {
            sb_append_char(&s->preBuffer, '\n');
        } else if (s->hasPending) {
            DocInline in;
            memset(&in, 0, sizeof(in));
            in.type = DIT_BR;
            r_inl_push(&s->pending, &in);
        } else {
            r_commit(s);
        }

    } else if (!strcmp(tag, "hr")) {
        r_commit(s);
        DocBlock b;
        r_blk_init(&b, DB_HR);
        r_flush(s, &b);

    } else if (!strcmp(tag, "b") || !strcmp(tag, "strong")) {
        s->isBold = !closing;

    } else if (!strcmp(tag, "i") || !strcmp(tag, "em") ||
               !strcmp(tag, "cite")) {
        s->isItalic = !closing;

    } else if (!strcmp(tag, "code") || !strcmp(tag, "kbd") ||
               !strcmp(tag, "samp") || !strcmp(tag, "tt")) {
        s->isCode = !closing;

    } else if (!strcmp(tag, "pre")) {
        r_commit(s);
        if (!closing) {
            s->inPre = 1;
            sb_clear(&s->preBuffer);
        } else {
            s->inPre = 0;
            if (s->preBuffer.len > 0) {
                DocBlock b;
                r_blk_init(&b, DB_CODE_BLOCK);
                sb_reserve(&s->preBuffer, 1);
                s->preBuffer.data[s->preBuffer.len] = '\0';
                b.codeText = sb_detach(&s->preBuffer);
                r_flush(s, &b);
                sb_clear(&s->preBuffer);
            }
        }

    } else if (!strcmp(tag, "blockquote")) {
        r_commit(s);
        s->inBlockquote = !closing;

    } else if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
        r_commit(s);
        if (!closing) {
            if (s->nList == s->capList) {
                size_t nc = s->capList ? s->capList * 2 : 4;
                RListLevel* ns = (RListLevel*)pluto_realloc(
                    s->listStack, nc * sizeof(RListLevel));
                if (ns == NULL) return;
                s->listStack = ns;
                s->capList = nc;
            }
            s->listStack[s->nList].isOl = (tag[0] == 'o');
            s->listStack[s->nList].count = 0;
            s->nList++;
        } else if (s->nList > 0) {
            s->nList--;
        }

    } else if (!strcmp(tag, "li")) {
        r_commit(s);
        if (!closing) {
            RListLevel def;
            def.isOl = 0;
            def.count = 0;
            RListLevel* parent =
                (s->nList > 0) ? &s->listStack[s->nList - 1] : &def;
            parent->count++;
            r_blk_init(&s->pending, DB_LIST_ITEM);
            s->pending.isOrdered = parent->isOl;
            s->pending.number = parent->count;
            s->hasPending = 1;
        } else {
            r_commit(s);
        }

    } else if (!strcmp(tag, "a")) {
        if (!closing) {
            const char* rawHref = r_attr(attrs, "href");
            if (rawHref != NULL && rawHref[0] != '\0' && rawHref[0] != '#' &&
                !r_ci_prefix(rawHref, "javascript:")) {
                pluto_free(s->currentHref);
                s->currentHref = r_resolve(baseUrl, rawHref);
            }
        } else {
            pluto_free(s->currentHref);
            s->currentHref = NULL;
        }

    } else if (!strcmp(tag, "form")) {
        if (!closing) {
            pluto_free(s->formAction);
            s->formAction =
                r_resolve(baseUrl, r_attr(attrs, "action"));
            pluto_free(s->formMethod);
            const char* m = r_attr(attrs, "method");
            if (m != NULL) {
                s->formMethod = pluto_strdup(m);
                for (char* p = s->formMethod; *p; p++)
                    *p = (char)tolower((unsigned char)*p);
            } else {
                s->formMethod = pluto_strdup("get");
            }
        } else {
            pluto_free(s->formAction);
            s->formAction = pluto_strdup("");
        }

    } else if (!strcmp(tag, "input")) {
        const char* tyRaw = r_attr(attrs, "type");
        char* inputType = pluto_strdup((tyRaw != NULL) ? tyRaw : "text");
        if (inputType == NULL) return;
        for (char* p = inputType; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        const char* nm = r_attr(attrs, "name");
        const char* val = r_attr(attrs, "value");
        const char* ph = r_attr(attrs, "placeholder");
        if (ph == NULL) ph = r_attr(attrs, "aria-label");

        if (!strcmp(inputType, "text") || !strcmp(inputType, "search") ||
            !strcmp(inputType, "email") || !strcmp(inputType, "url") ||
            !strcmp(inputType, "number") || !strcmp(inputType, "password")) {
            r_flush_input_field(s, inputType, (nm != NULL) ? nm : "q",
                                (val != NULL) ? val : "",
                                (ph != NULL) ? ph : "");
        } else if (!strcmp(inputType, "submit") ||
                   !strcmp(inputType, "button")) {
            r_flush_input_submit(
                s, (val != NULL && val[0] != '\0') ? val : "Submit", NULL);
        }
        pluto_free(inputType);

    } else if (!strcmp(tag, "textarea")) {
        if (!closing) {
            r_commit(s);
            s->inTextarea = 1;
            pluto_free(s->textareaName);
            const char* nm = r_attr(attrs, "name");
            s->textareaName = pluto_strdup((nm != NULL) ? nm : "q");
            sb_clear(&s->textareaBuffer);
        } else {
            s->inTextarea = 0;
            r_commit(s);
            DocBlock b;
            r_blk_init(&b, DB_INPUT_FIELD);
            b.inputType = pluto_strdup("textarea");
            b.inName = (s->textareaName != NULL)
                           ? s->textareaName : pluto_strdup("q");
            s->textareaName = NULL;
            sb_reserve(&s->textareaBuffer, 1);
            s->textareaBuffer.data[s->textareaBuffer.len] = '\0';
            b.inValue = sb_detach(&s->textareaBuffer);
            b.placeholder = pluto_strdup("");
            if (s->formAction[0] != '\0')
                b.formAction = pluto_strdup(s->formAction);
            b.formMethod = pluto_strdup(s->formMethod);
            r_flush(s, &b);
            sb_clear(&s->textareaBuffer);
        }

    } else if (!strcmp(tag, "button")) {
        if (!closing) {
            const char* tyRaw = r_attr(attrs, "type");
            char btype[16];
            snprintf(btype, sizeof(btype), "%s", (tyRaw != NULL) ? tyRaw : "submit");
            for (char* p = btype; *p; p++)
                *p = (char)tolower((unsigned char)*p);
            if (!strcmp(btype, "submit") || !strcmp(btype, "button")) {
                r_commit(s);
                s->inButton = 1;
                sb_clear(&s->buttonLabel);
                pluto_free(s->buttonFormAction);
                s->buttonFormAction = pluto_strdup(s->formAction);
            }
        } else if (s->inButton) {
            s->inButton = 0;
            sb_reserve(&s->buttonLabel, 1);
            s->buttonLabel.data[s->buttonLabel.len] = '\0';
            char* label = sb_detach(&s->buttonLabel);
            r_trim(label);
            sb_clear(&s->buttonLabel);
            r_flush_input_submit(s, (label[0] != '\0') ? label : "Submit",
                                 s->buttonFormAction);
            pluto_free(label);
        }

    } else if (!strcmp(tag, "img") && attrs != NULL) {
        r_handle_img(s, attrs, baseUrl);
    }
}

/* ── mergeParagraphFragments (faithful port) ───────────────────────────── */

typedef struct {
    DocBlock* items;
    size_t n, cap;
} RBlkArr;

static void r_ba_push(RBlkArr* a, const DocBlock* b) {
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        DocBlock* ni = (DocBlock*)pluto_realloc(a->items, nc * sizeof(DocBlock));
        if (ni == NULL) return;
        a->items = ni;
        a->cap = nc;
    }
    a->items[a->n++] = *b;
}

/* move src's inline array into dst */
static void r_absorb_inlines(DocBlock* dst, DocBlock* src) {
    for (size_t i = 0; i < src->nInlines; i++)
        r_inl_push(dst, &src->inlines[i]);
    pluto_free(src->inlines);
    src->inlines = NULL;
    src->nInlines = src->capInlines = 0;
}

static void r_merge_fragments(RBlkArr* blocks, RBlkArr* out) {
    char* lastAccum = NULL;
    for (size_t bi = 0; bi < blocks->n; bi++) {
        DocBlock* blk = &blocks->items[bi];
        int merged = 0;
        if (blk->type == DB_PARAGRAPH) {
            DocBlock* last =
                (out->n > 0) ? &out->items[out->n - 1] : NULL;
            char* curText = r_concat_text(blk);
            if (last != NULL && last->type == DB_PARAGRAPH &&
                lastAccum != NULL) {
                if (!r_ends_sentence(lastAccum) &&
                    strlen(lastAccum) + strlen(curText) <= 200 &&
                    r_wordcount(curText) <= 40) {
                    r_absorb_inlines(last, blk);
                    /* lastAccum = trim(lastAccum .. curText) */
                    {
                        size_t la = strlen(lastAccum);
                        char* nj = (char*)pluto_realloc(
                            lastAccum, la + strlen(curText) + 1);
                        if (nj != NULL) {
                            lastAccum = nj;
                            strcpy(lastAccum + la, curText);
                            r_trim(lastAccum);
                        }
                    }
                    merged = 1;
                }
            }
            if (!merged) {
                pluto_free(lastAccum);
                lastAccum = curText;
            } else {
                pluto_free(curText);
            }
        }
        if (!merged) {
            r_ba_push(out, blk);
            if (blk->type == DB_PARAGRAPH) {
                pluto_free(lastAccum);
                lastAccum = r_concat_text(blk);
            } else {
                pluto_free(lastAccum);
                lastAccum = NULL;
            }
        }
    }
    pluto_free(lastAccum);
}

/* ── scoring / selection / assembly ───────────────────────────────────── */

static int r_score_cmp(const void* pa, const void* pb) {
    const RContainer* a = (const RContainer*)pa;
    const RContainer* b = (const RContainer*)pb;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    if (a->origIndex < b->origIndex) return -1;
    if (a->origIndex > b->origIndex) return 1;
    return 0;
}

DocDocument* readability_distill(HttTokens* tokens, const char* rawTitle,
                                 const char* baseUrl) {
    if (tokens == NULL) return NULL;

    RD s;
    memset(&s, 0, sizeof(s));
    sb_init(&s.preBuffer);
    sb_init(&s.textareaBuffer);
    sb_init(&s.buttonLabel);
    s.formMethod = pluto_strdup("get");
    s.formAction = pluto_strdup("");
    r_push_container(&s, 0, 0);
    s.curIdx = 0;

    PlutoUrl pu;
    url_parse((baseUrl != NULL) ? baseUrl : "", &pu);
    char hostUp[PLUTO_URL_HOST_MAX];
    snprintf(hostUp, sizeof(hostUp), "%s", pu.host);
    for (char* p = hostUp; *p; p++) *p = (char)toupper((unsigned char)*p);
    /* Lua: parsedUrl.host or "WEB PAGE" -- unreachable (host is always a
     * string, "blank" for empty input); kept as documentation. */
    const char* hostName = hostUp;
    char pageTitleBuf[256];
    snprintf(pageTitleBuf, sizeof(pageTitleBuf), "%s",
             (rawTitle != NULL) ? rawTitle : "Untitled Article");

    for (size_t i = 0; i < tokens->count; i++) {
        tasks_yield_check();
        tasks_report_progress(0.5 + 0.3 *
                                        ((double)(i + 1) /
                                         (double)(tokens->count > 0
                                                      ? tokens->count
                                                      : 1)));
        HttToken* t = &tokens->items[i];
        if (t->type == HTT_TEXT) {
            if (s.stripDepth == 0) {
                if (s.inPre)
                    sb_append(&s.preBuffer, t->text, t->textLen);
                else if (s.inTextarea)
                    sb_append(&s.textareaBuffer, t->text, t->textLen);
                else if (s.inButton)
                    sb_append(&s.buttonLabel, t->text, t->textLen);
                else
                    r_add_text(&s, t->text, t->textLen);
            }
        } else if (t->type == HTT_TAG && t->name != NULL) {
            r_handle_tag(&s, t, (baseUrl != NULL) ? baseUrl : "");
        }
    }
    r_commit(&s);

    /* score containers and pick the best article content */
    for (size_t i = 0; i < s.nConts; i++) {
        RContainer* c = &s.conts[i];
        double score = (double)(c->wordCount + c->imageCount * 30);
        double density =
            (c->wordCount > 0)
                ? ((double)c->linkWords / (double)c->wordCount) : 0.0;
        if (density > 0.5)
            score *= 0.2;
        else if (density > 0.33)
            score *= 0.6;
        if (c->isContent) score *= 3.0;
        if (c->isNav) score *= 0.05;
        c->score = score;
    }

    qsort(s.conts, s.nConts, sizeof(RContainer), r_score_cmp);

    RContainer* best = (s.nConts > 0) ? &s.conts[0] : NULL;
    RBlkArr bestBlocks;
    memset(&bestBlocks, 0, sizeof(bestBlocks));

    if (best != NULL && best->nBlocks > 0) {
        double threshold = best->score * 0.15;
        /* Lua comment says document order but iterates the SORTED array;
         * replicated exactly. Pointers stay valid: conts is not resized. */
        for (size_t i = 0; i < s.nConts; i++) {
            RContainer* c = &s.conts[i];
            if (!c->isNav && c->nBlocks > 0 &&
                (c == best || c->score >= threshold)) {
                for (size_t k = 0; k < c->nBlocks; k++)
                    r_ba_push(&bestBlocks, &c->blocks[k]);
                c->nBlocks = 0; /* moved */
            }
        }
    }
    if (bestBlocks.n == 0) {
        for (size_t i = 0; i < s.nConts; i++) {
            RContainer* c = &s.conts[i];
            if (!c->isNav) {
                for (size_t k = 0; k < c->nBlocks; k++)
                    r_ba_push(&bestBlocks, &c->blocks[k]);
                c->nBlocks = 0;
            }
        }
    }

    int totalWords = 0;
    for (size_t i = 0; i < bestBlocks.n; i++) {
        DocBlock* blk = &bestBlocks.items[i];
        for (size_t k = 0; k < blk->nInlines; k++)
            totalWords += r_wordcount(blk->inlines[k].text);
    }
    int readingMins = (totalWords + 179) / 180;
    if (readingMins < 1) readingMins = 1;
    char readTimeStr[64];
    snprintf(readTimeStr, sizeof(readTimeStr), "%d min read (%d words)",
             readingMins, totalWords);

    /* finalBlocks = reader_header, h1 title, hr, then merged content */
    DocDocument* d = (DocDocument*)pluto_malloc(sizeof(DocDocument));
    if (d == NULL) return NULL;
    memset(d, 0, sizeof(*d));

    RBlkArr finalB;
    memset(&finalB, 0, sizeof(finalB));

    DocBlock hdr;
    r_blk_init(&hdr, DB_READER_HEADER);
    hdr.readerHost = pluto_strdup(hostName);
    hdr.readerTitle = pluto_strdup(pageTitleBuf);
    hdr.readingTime = pluto_strdup(readTimeStr);
    r_ba_push(&finalB, &hdr);

    DocBlock h1;
    r_blk_init(&h1, DB_HEADING);
    h1.level = 1;
    {
        DocInline in;
        memset(&in, 0, sizeof(in));
        in.type = DIT_TEXT;
        in.text = pluto_strdup(pageTitleBuf);
        in.bold = 1;
        r_inl_push(&h1, &in);
    }
    r_ba_push(&finalB, &h1);

    DocBlock hrB;
    r_blk_init(&hrB, DB_HR);
    r_ba_push(&finalB, &hrB);

    /* mergeParagraphFragments over the selected content blocks */
    r_merge_fragments(&bestBlocks, &finalB);
    pluto_free(bestBlocks.items);

    d->title = pluto_strdup(pageTitleBuf);
    d->baseUrl = (baseUrl != NULL) ? pluto_strdup(baseUrl) : NULL;
    d->isReaderMode = 1;
    d->readerWords = totalWords;
    d->readerTime = pluto_strdup(readTimeStr);
    d->blocks = finalB.items;
    d->nBlocks = finalB.n;
    d->capBlocks = finalB.cap;

    /* free leftover containers (dropped / moved-out shells) */
    for (size_t i = 0; i < s.nConts; i++) {
        for (size_t k = 0; k < s.conts[i].nBlocks; k++)
            doc_free_block_fields(&s.conts[i].blocks[k]);
        pluto_free(s.conts[i].blocks);
    }
    pluto_free(s.conts);
    pluto_free(s.currentHref);
    pluto_free(s.listStack);
    sb_free(&s.preBuffer);
    sb_free(&s.textareaBuffer);
    sb_free(&s.buttonLabel);
    pluto_free(s.formAction);
    pluto_free(s.formMethod);
    pluto_free(s.textareaName);
    pluto_free(s.buttonFormAction);
    return d;
}
