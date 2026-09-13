/*
 * PlutoBrowser — readability.c
 * Port of Source/html/readability.lua (see readability.h for the mapping and
 * preserved-semantics notes).
 *
 * The distiller consumes the tokenizer's flat token array (same input the
 * Lua reference received) and emits DocBlocks into a DocParseResult. Blocks,
 * inlines and strings are allocated from a DocArena owned by the result —
 * identical lifetime model to document.c's walker output, so document_free
 * releases everything.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#include "html/readability.h"
#include "html/document.h"
#include "core/url.h"
#include "core/constants.h"
#include "core/tasks.h"
#include "util/strbuf.h"

extern void pluto_free(void *p);
extern void *pluto_realloc(void *p, size_t n);

#define PLUTO_MALLOC(n) pluto_realloc(NULL, (n))
#define PLUTO_REALLOC(p, n) pluto_realloc((p), (n))
#define PLUTO_FREE(p) pluto_free(p)

/* ── Arena (same layout as document.c's DocArena; kept in sync) ───────────── */

typedef struct RdChunk
{
    struct RdChunk *next;
    size_t used;
    size_t cap;
} RdChunk;

typedef struct
{
    RdChunk *head;
} RdArena;

static void *rd_arena_alloc(RdArena *a, size_t n)
{
    n = (n + 7u) & ~(size_t)7u;
    if (a->head && a->head->used + n <= a->head->cap)
    {
        void *p = (char *)a->head + sizeof(RdChunk) + a->head->used;
        a->head->used += n;
        return p;
    }
    size_t c = n > 4096 ? n : 4096;
    RdChunk *ch = (RdChunk *)PLUTO_MALLOC(sizeof(RdChunk) + c);
    if (!ch)
    {
        return NULL;
    }
    ch->next = a->head;
    ch->used = 0;
    ch->cap = c;
    a->head = ch;
    void *p = (char *)ch + sizeof(RdChunk);
    ch->used = n;
    return p;
}

static void rd_arena_free_all(RdArena *a)
{
    RdChunk *ch = a->head;
    while (ch)
    {
        RdChunk *next = ch->next;
        PLUTO_FREE(ch);
        ch = next;
    }
    a->head = NULL;
}

static char *rd_arena_str(RdArena *a, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    size_t n = strlen(s);
    char *p = (char *)rd_arena_alloc(a, n + 1);
    if (p)
    {
        memcpy(p, s, n + 1);
    }
    return p;
}

static int rd_ptrarr_push(void ***arr, int *count, int *cap, void *item)
{
    if (*count >= *cap)
    {
        int nc = *cap ? *cap * 2 : 8;
        void **na = (void **)PLUTO_REALLOC(*arr, (size_t)nc * sizeof(void *));
        if (!na)
        {
            return -1;
        }
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*count)++] = item;
    return 0;
}

/* Attribute lookup over a Token (value NULL when boolean-attr marker). */
static int rd_get_attr(const Token *tok, const char *key, const char **outVal)
{
    for (int i = 0; i < tok->attrCount; i++)
    {
        if (strcmp(tok->attrs[i].key, key) == 0)
        {
            const char *v = tok->attrs[i].value;
            *outVal = (v == PLUTO_TOK_ATTR_TRUE) ? "" : v;
            return 1;
        }
    }
    return 0;
}

/* ── Helpers (ports of the Lua locals) ────────────────────────────────────── */

/* Lua trimStr: gsub("^%s*(.-)%s*$", "%1") — leading+trailing ASCII ws. */
static void rd_trim(const char *s, const char **out, size_t *outLen)
{
    if (!s)
    {
        *out = "";
        *outLen = 0;
        return;
    }
    size_t n = strlen(s);
    size_t b = 0, e = n;
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' ||
                     s[b] == '\n' || s[b] == '\f' || s[b] == '\v'))
    {
        b++;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n' || s[e - 1] == '\f' || s[e - 1] == '\v'))
    {
        e--;
    }
    *out = s + b;
    *outLen = e - b;
}

/* Lua wordCount: count of %S+ runs. */
static int rd_word_count(const char *s, size_t len)
{
    if (!s || len == 0)
    {
        return 0;
    }
    int count = 0;
    int inWord = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        int isSpace = (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                       c == '\f' || c == '\v');
        if (!isSpace && !inWord)
        {
            count++;
            inWord = 1;
        }
        else if (isSpace)
        {
            inWord = 0;
        }
    }
    return count;
}

/* Lua isBareUrlText. Patterns, verbatim:
 *   ^https?://[^%s]+$                       (scheme + rest, no spaces)
 *   ^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*([/%?][^%s]*)?$ */
static int rd_is_bare_url_text(const char *text)
{
    if (!text)
    {
        return 0;
    }
    const char *t;
    size_t tl;
    rd_trim(text, &t, &tl);
    if (tl == 0)
    {
        return 0;
    }
    for (size_t i = 0; i < tl; i++)
    {
        unsigned char c = (unsigned char)t[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v')
        {
            return 0;
        }
    }
    /* ^https?:// */
    if (tl > 8 &&
        ((t[0] == 'h' && t[1] == 't' && t[2] == 't' && t[3] == 'p' &&
          t[4] == 's' && t[5] == ':' && t[6] == '/' && t[7] == '/') ||
         (t[0] == 'h' && t[1] == 't' && t[2] == 't' && t[3] == 'p' &&
          t[4] == ':' && t[5] == '/' && t[6] == '/')))
    {
        return 1;
    }
    /* ^[a-zA-Z0-9][a-zA-Z0-9.-]*\.[a-zA-Z][a-zA-Z0-9-]*([/?][^ ]*)?$ */
    if (!((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z') ||
          (t[0] >= '0' && t[0] <= '9')))
    {
        return 0;
    }
    size_t i = 1;
    while (i < tl && ((t[i] >= 'a' && t[i] <= 'z') ||
                      (t[i] >= 'A' && t[i] <= 'Z') ||
                      (t[i] >= '0' && t[i] <= '9') || t[i] == '-' ||
                      t[i] == '.'))
    {
        i++;
    }
    if (i == tl || t[i] != '.')
    {
        return 0;
    }
    i++;
    if (i >= tl || !((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z')))
    {
        return 0;
    }
    i++;
    while (i < tl && ((t[i] >= 'a' && t[i] <= 'z') ||
                      (t[i] >= 'A' && t[i] <= 'Z') ||
                      (t[i] >= '0' && t[i] <= '9') || t[i] == '-'))
    {
        i++;
    }
    if (i == tl)
    {
        return 1;
    }
    if (t[i] != '/' && t[i] != '?')
    {
        return 0;
    }
    i++;
    for (; i < tl; i++)
    {
        unsigned char c = (unsigned char)t[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v')
        {
            return 0;
        }
    }
    return 1;
}

/* ── Distiller state ──────────────────────────────────────────────────────── */

typedef struct
{
    int score;
    int textLen;
    int wordCount;
    int imageCount;
    DocBlock **blocks;
    int blockCount;
    int blockCap;
    int linkWords;
    int isContent;
    int isNav;
} RdContainer;

typedef struct
{
    RdArena arena;
    DocParseResult *out;
    int error;

    RdContainer *containers;
    int containerCount;
    int containerCap;
    int currentContainerIdx; /* 1-based, Lua parity */

    int stripDepth;
    int isBold, isItalic, isCode;
    char *currentHref; /* arena */
    StrBuf linkText;

    /* list stack (ul/ol + counters) */
    struct
    {
        char type[4];
        int count;
    } *listStack;
    int listDepth, listCap;

    int inPre;
    StrBuf preBuf;
    int inBlockquote;
    DocBlock *pendingBlock;
    char *formAction; /* arena */
    char *formMethod; /* arena; Lua initializes to "get" */
    int inTextarea;
    char *textareaName; /* arena */
    StrBuf textareaBuf;
    int inButton;
    StrBuf buttonBuf;
    char *buttonFormAction; /* arena */
} RdState;

static char *rd_arena_str_s(RdState *st, const char *s)
{
    if (!s)
    {
        return NULL;
    }
    char *p = rd_arena_str(&st->arena, s);
    if (!p)
    {
        st->error = 1;
    }
    return p;
}

static RdContainer *rd_current_container(RdState *st)
{
    return &st->containers[st->currentContainerIdx - 1];
}

static DocBlock *rd_new_block(RdState *st, int type)
{
    DocBlock *b = (DocBlock *)rd_arena_alloc(&st->arena, sizeof(DocBlock));
    if (!b)
    {
        st->error = 1;
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->maxlength = -1;
    b->fieldWidth = -1;
    b->fieldRows = -1;
    return b;
}

static DocInline *rd_new_inline(RdState *st, int type)
{
    DocInline *inl = (DocInline *)rd_arena_alloc(&st->arena, sizeof(DocInline));
    if (!inl)
    {
        st->error = 1;
        return NULL;
    }
    memset(inl, 0, sizeof(*inl));
    inl->type = type;
    return inl;
}

static int rd_add_block(RdState *st, RdContainer *c, DocBlock *b)
{
    if (!b)
    {
        return 0;
    }
    if (c->blockCount >= DOC_MAX_BLOCKS || rd_ptrarr_push((void ***)&c->blocks,
                                                          &c->blockCount,
                                                          &c->blockCap, b))
    {
        st->error = 1;
        return 0;
    }
    return 1;
}

static int rd_add_inline(RdState *st, DocBlock *b, DocInline *inl)
{
    if (!inl)
    {
        return 0;
    }
    if (b->inlineCount >= DOC_MAX_INLINES ||
        rd_ptrarr_push((void ***)&b->inlines, &b->inlineCount, &b->inlineCap,
                       inl))
    {
        st->error = 1;
        return 0;
    }
    return 1;
}

/* Lua flushBlock. */
static void rd_flush_block(RdState *st, DocBlock *block)
{
    if (!block)
    {
        return;
    }
    RdContainer *c = rd_current_container(st);
    if (block->type == DOC_BLOCK_HR || block->type == DOC_BLOCK_IMAGE ||
        block->type == DOC_BLOCK_INPUT_FIELD ||
        block->type == DOC_BLOCK_INPUT_SUBMIT)
    {
        rd_add_block(st, c, block);
        if (block->type == DOC_BLOCK_IMAGE)
        {
            c->imageCount++;
        }
        return;
    }

    /* fullText = concat(inl.text), trimmed. */
    size_t total = 0;
    for (int i = 0; i < block->inlineCount; i++)
    {
        if (block->inlines[i]->text)
        {
            total += strlen(block->inlines[i]->text);
        }
    }
    char *full = NULL;
    if (total > 0)
    {
        full = (char *)PLUTO_MALLOC(total + 1);
        if (!full)
        {
            st->error = 1;
            return;
        }
        size_t off = 0;
        for (int i = 0; i < block->inlineCount; i++)
        {
            if (block->inlines[i]->text)
            {
                size_t n = strlen(block->inlines[i]->text);
                memcpy(full + off, block->inlines[i]->text, n);
                off += n;
            }
        }
        full[off] = '\0';
    }
    const char *ft;
    size_t ftLen;
    rd_trim(full ? full : "", &ft, &ftLen);
    if (ftLen == 0 && block->type != DOC_BLOCK_CODE_BLOCK)
    {
        PLUTO_FREE(full);
        return;
    }

    c->wordCount += rd_word_count(ft, ftLen);
    c->textLen += (int)ftLen;

    for (int i = 0; i < block->inlineCount; i++)
    {
        if (block->inlines[i]->href)
        {
            const char *lt = block->inlines[i]->text ? block->inlines[i]->text : "";
            c->linkWords += rd_word_count(lt, strlen(lt));
        }
    }
    rd_add_block(st, c, block);
    PLUTO_FREE(full);
}

static void rd_commit_block(RdState *st)
{
    if (st->pendingBlock)
    {
        rd_flush_block(st, st->pendingBlock);
        st->pendingBlock = NULL;
    }
}

/* Lua ensureBlock: type defaults to blockquote-in-context or paragraph. */
static DocBlock *rd_ensure_block(RdState *st, int type /* -1 = default */)
{
    if (!st->pendingBlock)
    {
        int t = type >= 0 ? type
                          : (st->inBlockquote ? DOC_BLOCK_BLOCKQUOTE
                                              : DOC_BLOCK_PARAGRAPH);
        st->pendingBlock = rd_new_block(st, t);
    }
    return st->pendingBlock;
}

/* Lua addText. */
static void rd_add_text(RdState *st, const char *text)
{
    if (st->stripDepth > 0 || !text || text[0] == '\0')
    {
        return;
    }
    /* Tokenizer already entity-decoded this text. */
    if (!st->inPre)
    {
        /* Lua gsub "[\r\n\t]+" → " ": collapse each RUN to one space. */
        StrBuf sb;
        strbuf_init(&sb);
        const char *p = text;
        while (*p)
        {
            if (*p == '\r' || *p == '\n' || *p == '\t')
            {
                while (*p == '\r' || *p == '\n' || *p == '\t')
                {
                    p++;
                }
                strbuf_append(&sb, " ");
            }
            else
            {
                strbuf_append_n(&sb, p, 1);
                p++;
            }
        }
        text = strbuf_detach(&sb);
        if (!text)
        {
            st->error = 1;
            return;
        }
    }
    if (text[0] == '\0' || (text[0] == ' ' && text[1] == '\0'))
    {
        if (!st->inPre)
        {
            PLUTO_FREE((void *)text);
        }
        return;
    }

    /* Skip anchors whose visible text is just the URL itself. */
    if (st->currentHref && rd_is_bare_url_text(text))
    {
        if (!st->inPre)
        {
            PLUTO_FREE((void *)text);
        }
        return;
    }

    DocBlock *b = rd_ensure_block(st, -1);
    if (st->error)
    {
        if (!st->inPre)
        {
            PLUTO_FREE((void *)text);
        }
        return;
    }
    DocInline *inl = rd_new_inline(st, DOC_INLINE_TEXT);
    if (!inl)
    {
        if (!st->inPre)
        {
            PLUTO_FREE((void *)text);
        }
        return;
    }
    inl->text = rd_arena_str_s(st, text);
    if (!st->inPre)
    {
        PLUTO_FREE((void *)text);
    }
    if (!inl->text)
    {
        return;
    }
    inl->flags = 0;
    if (st->isBold)
    {
        inl->flags |= DOC_INF_BOLD;
    }
    if (st->isItalic)
    {
        inl->flags |= DOC_INF_ITALIC;
    }
    if (st->isCode)
    {
        inl->flags |= DOC_INF_CODE;
    }
    if (st->currentHref)
    {
        inl->flags |= DOC_INF_UNDERLINE;
        inl->href = st->currentHref;
    }
    rd_add_inline(st, b, inl);
    if (st->currentHref)
    {
        /* text may already have been freed above (the non-pre path frees it
         * right after the arena copy) — copy from the arena string instead. */
        strbuf_append(&st->linkText, inl->text);
    }
}

/* Lua pushContainer. */
static void rd_push_container(RdState *st, int isContent, int isNav)
{
    rd_commit_block(st);
    if (st->containerCount == st->containerCap)
    {
        int nc = st->containerCap ? st->containerCap * 2 : 8;
        RdContainer *ncn = (RdContainer *)PLUTO_REALLOC(
            st->containers, (size_t)nc * sizeof(RdContainer));
        if (!ncn)
        {
            st->error = 1;
            return;
        }
        st->containers = ncn;
        st->containerCap = nc;
    }
    RdContainer *c = &st->containers[st->containerCount++];
    memset(c, 0, sizeof(*c));
    c->isContent = isContent;
    c->isNav = isNav;
    st->currentContainerIdx = st->containerCount; /* 1-based */
}

/* Lua popContainer. */
static void rd_pop_container(RdState *st)
{
    rd_commit_block(st);
    if (st->currentContainerIdx > 1)
    {
        st->currentContainerIdx--;
    }
}

/* ── mergeParagraphFragments (exact port) ─────────────────────────────────── */

static int rd_ends_sentence(const char *s, size_t n)
{
    /* Lua: match(lastAccum, "[%.%?!%:]%s*$") — one terminator char then ws. */
    if (n == 0)
    {
        return 0;
    }
    size_t i = n;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t'))
    {
        i--;
    }
    if (i == 0)
    {
        return 0;
    }
    char c = s[i - 1];
    return c == '.' || c == '?' || c == '!' || c == ':';
}

typedef struct
{
    DocBlock **items;
    int count;
    int cap;
} RdBlockVec;

static void rd_bv_push(RdBlockVec *v, DocBlock *b)
{
    if (v->count == v->cap)
    {
        int nc = v->cap ? v->cap * 2 : 16;
        DocBlock **ni = (DocBlock **)PLUTO_REALLOC(v->items,
                                                   (size_t)nc * sizeof(*ni));
        if (!ni)
        {
            return;
        }
        v->items = ni;
        v->cap = nc;
    }
    v->items[v->count++] = b;
}

static void rd_merge_paragraph_fragments(RdState *st, RdBlockVec *src,
                                         RdBlockVec *out)
{
    const char *lastAccum = NULL; /* trimmed text of the last out paragraph */
    size_t lastAccumLen = 0;
    char *lastAccumBuf = NULL; /* heap; grown via realloc */

    for (int bi = 0; bi < src->count; bi++)
    {
        DocBlock *blk = src->items[bi];
        int merged = 0;
        if (blk->type == DOC_BLOCK_PARAGRAPH)
        {
            /* curText = concat(inl.text), trimmed. */
            size_t total = 0;
            for (int i = 0; i < blk->inlineCount; i++)
            {
                if (blk->inlines[i]->text)
                {
                    total += strlen(blk->inlines[i]->text);
                }
            }
            char *cur = (char *)PLUTO_MALLOC(total + 1);
            if (!cur)
            {
                st->error = 1;
                break;
            }
            size_t off = 0;
            for (int i = 0; i < blk->inlineCount; i++)
            {
                if (blk->inlines[i]->text)
                {
                    size_t n = strlen(blk->inlines[i]->text);
                    memcpy(cur + off, blk->inlines[i]->text, n);
                    off += n;
                }
            }
            cur[off] = '\0';
            const char *ct;
            size_t ctLen;
            rd_trim(cur, &ct, &ctLen);

            int outHasLast = out->count > 0 &&
                             out->items[out->count - 1]->type ==
                                 DOC_BLOCK_PARAGRAPH;
            if (outHasLast && lastAccum)
            {
                DocBlock *last = out->items[out->count - 1];
                /* Join when the previous text does not end a sentence, the
                 * combined length stays ≤ 200 and the incoming text is ≤ 40
                 * words. */
                if (!rd_ends_sentence(lastAccum, lastAccumLen) &&
                    lastAccumLen + ctLen <= 200 && rd_word_count(ct, ctLen) <= 40)
                {
                    for (int i = 0; i < blk->inlineCount; i++)
                    {
                        rd_add_inline(st, last, blk->inlines[i]);
                    }
                    /* lastAccum = trim(lastAccum .. curText) */
                    size_t nn = lastAccumLen + ctLen;
                    char *nb = (char *)PLUTO_MALLOC(nn + 1);
                    if (nb)
                    {
                        memcpy(nb, lastAccum, lastAccumLen);
                        memcpy(nb + lastAccumLen, ct, ctLen);
                        nb[nn] = '\0';
                        const char *nt;
                        size_t ntLen;
                        rd_trim(nb, &nt, &ntLen);
                        char *nb2 = (char *)PLUTO_MALLOC(ntLen + 1);
                        if (nb2)
                        {
                            memcpy(nb2, nt, ntLen);
                            nb2[ntLen] = '\0';
                        }
                        PLUTO_FREE(nb);
                        PLUTO_FREE(lastAccumBuf);
                        lastAccumBuf = nb2;
                        lastAccum = nb2;
                        lastAccumLen = nb2 ? ntLen : 0;
                    }
                    merged = 1;
                }
            }
            if (!merged)
            {
                /* lastAccum = trim(curText) — the block itself becomes the
                 * new tail. NOTE: ct points INTO cur, so the copy must
                 * happen before cur is freed (ASan-caught UAF). */
                char *nb = (char *)PLUTO_MALLOC(ctLen + 1);
                if (nb)
                {
                    memcpy(nb, ct, ctLen);
                    nb[ctLen] = '\0';
                }
                PLUTO_FREE(lastAccumBuf);
                lastAccumBuf = nb;
                lastAccum = nb;
                lastAccumLen = nb ? ctLen : 0;
            }
            PLUTO_FREE(cur);
        }
        if (!merged)
        {
            rd_bv_push(out, blk);
            if (blk->type == DOC_BLOCK_PARAGRAPH)
            {
                /* lastAccum already set above for paragraphs. */
            }
            else
            {
                PLUTO_FREE(lastAccumBuf);
                lastAccumBuf = NULL;
                lastAccum = NULL;
                lastAccumLen = 0;
            }
        }
    }
    PLUTO_FREE(lastAccumBuf);
}

/* ── Scoring + assembly ───────────────────────────────────────────────────── */

static int rd_cmp_container(const void *pa, const void *pb)
{
    const RdContainer *a = (const RdContainer *)pa;
    const RdContainer *b = (const RdContainer *)pb;
    /* Lua table.sort(comparator) is not stable; qsort isn't either. Descend
     * by score; tie order is unspecified in both. */
    if (a->score != b->score)
    {
        return b->score - a->score;
    }
    return 0;
}

/* Free a distill result (blocks' inline arrays, block array, arena). */
void readability_free_result(DocParseResult *doc)
{
    if (!doc)
    {
        return;
    }
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        if (b->inlines)
        {
            PLUTO_FREE(b->inlines);
        }
    }
    if (doc->blocks)
    {
        PLUTO_FREE(doc->blocks);
        doc->blocks = NULL;
    }
    doc->blockCount = doc->blockCap = 0;
    if (doc->_arena)
    {
        rd_arena_free_all((RdArena *)doc->_arena);
        PLUTO_FREE(doc->_arena);
        doc->_arena = NULL;
    }
}

int readability_distill(const TokenizeResult *tr, const char *rawTitle,
                        const char *baseUrl, DocParseResult *out)
{
    RdState st;
    memset(&st, 0, sizeof(st));
    st.out = out;
    strbuf_init(&st.linkText);
    strbuf_init(&st.preBuf);
    strbuf_init(&st.textareaBuf);
    strbuf_init(&st.buttonBuf);

    const char *hostUpper = "WEB PAGE";
    char hostBuf[128];
    {
        /* Lua: string.upper(URL.parse(baseUrl or "").host or "WEB PAGE").
         * Note "" parses to the about:blank record (host="blank" → "BLANK"),
         * and an empty host "" is truthy in Lua → upper("") = "". */
        UrlParsed up;
        if (url_parse(baseUrl ? baseUrl : "", &up) == 0)
        {
            const char *h = up.host;
            size_t n = strlen(h);
            if (n < sizeof(hostBuf))
            {
                for (size_t i = 0; i < n; i++)
                {
                    hostBuf[i] = (char)toupper((unsigned char)h[i]);
                }
                hostBuf[n] = '\0';
                hostUpper = hostBuf;
            }
            /* over-long host: keep the "WEB PAGE" fallback (Lua would print
             * the full host; C buffer bound documented in the header) */
        }
    }
    const char *pageTitle = (rawTitle && rawTitle[0]) ? rawTitle : "Untitled Article";

    /* Root container. */
    rd_push_container(&st, 0, 0);
    st.formMethod = "get"; /* Lua: local currentFormMethod = "get" */

    char formActionBuf[1024];
    char formMethodBuf[16];

    for (int ti = 0; ti < tr->tokens.count && !st.error; ti++)
    {
        const Token *tok = &tr->tokens.items[ti];
        /* Lua Tasks.yieldCheck / reportProgress — no task context during a
         * synchronous distill; the C tasks API no-ops without a ctx. */
        tasks_report_progress(0.5f + 0.3f * ((float)(ti + 1) /
                                             (float)(tr->tokens.count ? tr->tokens.count : 1)));

        if (tok->type == TOK_TEXT)
        {
            if (st.stripDepth == 0)
            {
                if (st.inPre)
                {
                    strbuf_append(&st.preBuf, tok->content ? tok->content : "");
                }
                else if (st.inTextarea)
                {
                    strbuf_append(&st.textareaBuf, tok->content ? tok->content : "");
                }
                else if (st.inButton)
                {
                    strbuf_append(&st.buttonBuf, tok->content ? tok->content : "");
                }
                else
                {
                    rd_add_text(&st, tok->content);
                }
            }
            continue;
        }

        /* tag token */
        const char *tag = tok->name;
        int isClosing = tok->isClosing;
        static const char *stripTags[] = {"script", "style", "noscript", "svg",
                                          "nav", "footer", "aside", "header",
                                          "iframe"};

        int isStrip = 0;
        for (size_t i = 0; i < sizeof(stripTags) / sizeof(stripTags[0]); i++)
        {
            if (strcmp(tag, stripTags[i]) == 0)
            {
                isStrip = 1;
                break;
            }
        }
        if (isStrip)
        {
            if (!isClosing)
            {
                st.stripDepth++;
            }
            else if (st.stripDepth > 0)
            {
                st.stripDepth--;
            }
            continue;
        }
        if (st.stripDepth != 0)
        {
            continue;
        }

        const char *attrVal;
        if (strcmp(tag, "article") == 0 || strcmp(tag, "main") == 0)
        {
            if (!isClosing)
            {
                rd_push_container(&st, 1, 0);
            }
            else
            {
                rd_pop_container(&st);
            }
        }
        else if (strcmp(tag, "nav") == 0 || strcmp(tag, "header") == 0 ||
                 strcmp(tag, "footer") == 0 || strcmp(tag, "aside") == 0)
        {
            if (!isClosing)
            {
                rd_push_container(&st, 0, 1);
            }
            else
            {
                rd_pop_container(&st);
            }
        }
        else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0')
        {
            rd_commit_block(&st);
            int level = tag[1] - '0';
            if (!isClosing)
            {
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_HEADING);
                if (b)
                {
                    b->level = level;
                    st.pendingBlock = b;
                }
            }
            else
            {
                rd_commit_block(&st);
            }
        }
        else if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
                 strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
                 strcmp(tag, "main") == 0 || strcmp(tag, "header") == 0 ||
                 strcmp(tag, "footer") == 0 || strcmp(tag, "aside") == 0 ||
                 strcmp(tag, "nav") == 0 || strcmp(tag, "figure") == 0 ||
                 strcmp(tag, "figcaption") == 0 || strcmp(tag, "blockquote") == 0 ||
                 strcmp(tag, "hgroup") == 0 || strcmp(tag, "address") == 0 ||
                 strcmp(tag, "fieldset") == 0 || strcmp(tag, "details") == 0 ||
                 strcmp(tag, "dialog") == 0 || strcmp(tag, "summary") == 0)
        {
            rd_commit_block(&st);
        }
        else if (strcmp(tag, "br") == 0)
        {
            if (st.inPre)
            {
                strbuf_append(&st.preBuf, "\n");
            }
            else if (st.pendingBlock)
            {
                DocInline *inl = rd_new_inline(&st, DOC_INLINE_BR);
                rd_add_inline(&st, st.pendingBlock, inl);
            }
            else
            {
                rd_commit_block(&st);
            }
        }
        else if (strcmp(tag, "hr") == 0)
        {
            rd_commit_block(&st);
            rd_flush_block(&st, rd_new_block(&st, DOC_BLOCK_HR));
        }
        else if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0)
        {
            st.isBold = !isClosing;
        }
        else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0 ||
                 strcmp(tag, "cite") == 0)
        {
            st.isItalic = !isClosing;
        }
        else if (strcmp(tag, "code") == 0 || strcmp(tag, "kbd") == 0 ||
                 strcmp(tag, "samp") == 0 || strcmp(tag, "tt") == 0)
        {
            st.isCode = !isClosing;
        }
        else if (strcmp(tag, "pre") == 0)
        {
            rd_commit_block(&st);
            if (!isClosing)
            {
                st.inPre = 1;
                strbuf_reset(&st.preBuf);
            }
            else
            {
                st.inPre = 0;
                if (st.preBuf.len > 0)
                {
                    DocBlock *b = rd_new_block(&st, DOC_BLOCK_CODE_BLOCK);
                    if (b)
                    {
                        b->text = rd_arena_str_s(&st, st.preBuf.data);
                        rd_flush_block(&st, b);
                    }
                    strbuf_reset(&st.preBuf);
                }
            }
        }
        else if (strcmp(tag, "blockquote") == 0)
        {
            rd_commit_block(&st);
            st.inBlockquote = !isClosing;
        }
        else if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0)
        {
            rd_commit_block(&st);
            if (!isClosing)
            {
                if (st.listDepth == st.listCap)
                {
                    int nc = st.listCap ? st.listCap * 2 : 4;
                    void *nb = PLUTO_REALLOC(st.listStack,
                                             (size_t)nc * sizeof(*st.listStack));
                    if (!nb)
                    {
                        st.error = 1;
                        break;
                    }
                    st.listStack = nb;
                    st.listCap = nc;
                }
                snprintf(st.listStack[st.listDepth].type,
                         sizeof(st.listStack[st.listDepth].type), "%s", tag);
                st.listStack[st.listDepth].count = 0;
                st.listDepth++;
            }
            else if (st.listDepth > 0)
            {
                st.listDepth--;
            }
        }
        else if (strcmp(tag, "li") == 0)
        {
            rd_commit_block(&st);
            if (!isClosing)
            {
                const char *ptype = "ul";
                int pcount = 0;
                if (st.listDepth > 0)
                {
                    ptype = st.listStack[st.listDepth - 1].type;
                    pcount = ++st.listStack[st.listDepth - 1].count;
                }
                else
                {
                    pcount = 1; /* Lua parent.count+1 on the fresh default */
                }
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_LIST_ITEM);
                if (b)
                {
                    b->isOrdered = (strcmp(ptype, "ol") == 0);
                    b->hasNumber = 1;
                    b->number = pcount;
                    st.pendingBlock = b;
                }
            }
            else
            {
                rd_commit_block(&st);
            }
        }
        else if (strcmp(tag, "a") == 0)
        {
            if (!isClosing)
            {
                const char *rawHref = "";
                if (rd_get_attr(tok, "href", &attrVal))
                {
                    rawHref = attrVal;
                }
                if (rawHref[0] != '\0' && rawHref[0] != '#' &&
                    (rawHref[0] != 'j' && rawHref[0] != 'J'))
                {
                    /* Lua: not match(rawHref, "^[Jj][Aa][Vv][Aa][Ss][Cc][Rr][Ii][Pp][Tt]:") */
                    const char *low = rawHref;
                    char head[12];
                    size_t n = strlen(rawHref);
                    if (n > 10)
                    {
                        n = 10;
                    }
                    for (size_t i = 0; i < n; i++)
                    {
                        head[i] = (char)tolower((unsigned char)rawHref[i]);
                    }
                    head[n] = '\0';
                    if (strncmp(head, "javascript:", 11 - 1) != 0)
                    {
                        char *resolved = url_resolve(baseUrl, rawHref);
                        if (resolved)
                        {
                            st.currentHref = rd_arena_str_s(&st, resolved);
                            PLUTO_FREE(resolved);
                        }
                        else
                        {
                            st.currentHref = rd_arena_str_s(&st, rawHref);
                        }
                        strbuf_reset(&st.linkText);
                    }
                }
            }
            else
            {
                st.currentHref = NULL;
                strbuf_reset(&st.linkText);
            }
        }
        else if (strcmp(tag, "form") == 0)
        {
            if (!isClosing)
            {
                const char *act = "";
                if (rd_get_attr(tok, "action", &attrVal))
                {
                    act = attrVal;
                }
                char *resolved = url_resolve(baseUrl, act);
                snprintf(formActionBuf, sizeof(formActionBuf), "%s",
                         resolved ? resolved : act);
                PLUTO_FREE(resolved);
                st.formAction = formActionBuf;
                const char *mth = "get";
                if (rd_get_attr(tok, "method", &attrVal) && attrVal[0])
                {
                    mth = attrVal;
                }
                for (char *p = formMethodBuf; ; p++)
                {
                    (void)p;
                    break;
                }
                size_t mn = strlen(mth);
                if (mn >= sizeof(formMethodBuf))
                {
                    mn = sizeof(formMethodBuf) - 1;
                }
                for (size_t i = 0; i < mn; i++)
                {
                    formMethodBuf[i] = (char)tolower((unsigned char)mth[i]);
                }
                formMethodBuf[mn] = '\0';
                st.formMethod = formMethodBuf;
            }
            else
            {
                st.formAction = "";
            }
        }
        else if (strcmp(tag, "input") == 0)
        {
            char inputType[32];
            const char *itv = "text";
            if (rd_get_attr(tok, "type", &attrVal))
            {
                itv = attrVal;
            }
            snprintf(inputType, sizeof(inputType), "%s", itv);
            for (char *p = inputType; *p; p++)
            {
                *p = (char)tolower((unsigned char)*p);
            }
            const char *inputName = "q";
            if (rd_get_attr(tok, "name", &attrVal))
            {
                inputName = attrVal;
            }
            const char *inputVal = "";
            if (rd_get_attr(tok, "value", &attrVal))
            {
                inputVal = attrVal;
            }
            const char *placeholder = "";
            if (!rd_get_attr(tok, "placeholder", &attrVal))
            {
                if (rd_get_attr(tok, "aria-label", &attrVal))
                {
                    placeholder = attrVal;
                }
            }
            else
            {
                placeholder = attrVal;
            }

            if (strcmp(inputType, "text") == 0 || strcmp(inputType, "search") == 0 ||
                strcmp(inputType, "email") == 0 || strcmp(inputType, "url") == 0 ||
                strcmp(inputType, "number") == 0 ||
                strcmp(inputType, "password") == 0)
            {
                rd_commit_block(&st);
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_INPUT_FIELD);
                if (b)
                {
                    b->inputType = rd_arena_str_s(&st, inputType);
                    b->name = rd_arena_str_s(&st, inputName);
                    b->value = rd_arena_str_s(&st, inputVal);
                    b->placeholder = rd_arena_str_s(&st, placeholder);
                    b->formAction = rd_arena_str_s(&st, st.formAction);
                    b->formMethod = rd_arena_str_s(&st, st.formMethod);
                    rd_flush_block(&st, b);
                }
            }
            else if (strcmp(inputType, "submit") == 0 ||
                     strcmp(inputType, "button") == 0)
            {
                rd_commit_block(&st);
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_INPUT_SUBMIT);
                if (b)
                {
                    b->label = rd_arena_str_s(&st, inputVal[0] ? inputVal : "Submit");
                    b->formAction = rd_arena_str_s(&st, st.formAction);
                    b->formMethod = rd_arena_str_s(&st, st.formMethod);
                    rd_flush_block(&st, b);
                }
            }
        }
        else if (strcmp(tag, "textarea") == 0)
        {
            if (!isClosing)
            {
                rd_commit_block(&st);
                st.inTextarea = 1;
                const char *tn = "q";
                if (rd_get_attr(tok, "name", &attrVal))
                {
                    tn = attrVal;
                }
                st.textareaName = rd_arena_str_s(&st, tn);
                strbuf_reset(&st.textareaBuf);
            }
            else
            {
                st.inTextarea = 0;
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_INPUT_FIELD);
                if (b)
                {
                    b->inputType = rd_arena_str_s(&st, "textarea");
                    b->name = rd_arena_str_s(&st, st.textareaName);
                    b->value = rd_arena_str_s(&st, st.textareaBuf.data);
                    b->placeholder = rd_arena_str_s(&st, "");
                    b->formAction = rd_arena_str_s(&st, st.formAction);
                    b->formMethod = rd_arena_str_s(&st, st.formMethod);
                    rd_flush_block(&st, b);
                }
                strbuf_reset(&st.textareaBuf);
            }
        }
        else if (strcmp(tag, "button") == 0)
        {
            if (!isClosing)
            {
                const char *btype = "submit";
                if (rd_get_attr(tok, "type", &attrVal) && attrVal[0])
                {
                    btype = attrVal;
                }
                char bl[16];
                snprintf(bl, sizeof(bl), "%s", btype);
                for (char *p = bl; *p; p++)
                {
                    *p = (char)tolower((unsigned char)*p);
                }
                if (strcmp(bl, "submit") == 0 || strcmp(bl, "button") == 0)
                {
                    rd_commit_block(&st);
                    st.inButton = 1;
                    strbuf_reset(&st.buttonBuf);
                    st.buttonFormAction = st.formAction;
                }
            }
            else if (st.inButton)
            {
                st.inButton = 0;
                const char *lb;
                size_t ll;
                rd_trim(st.buttonBuf.data, &lb, &ll);
                char labelBuf[512];
                size_t nn = ll < sizeof(labelBuf) - 1 ? ll : sizeof(labelBuf) - 1;
                memcpy(labelBuf, lb, nn);
                labelBuf[nn] = '\0';
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_INPUT_SUBMIT);
                if (b)
                {
                    b->label = rd_arena_str_s(&st, labelBuf[0] ? labelBuf : "Submit");
                    b->formAction = rd_arena_str_s(&st, st.buttonFormAction);
                    b->formMethod = rd_arena_str_s(&st, st.formMethod);
                    rd_flush_block(&st, b);
                }
                strbuf_reset(&st.buttonBuf);
            }
        }
        else if (strcmp(tag, "img") == 0)
        {
            char first[1024]; /* srcset first-entry buffer (hoisted: src may
                               * point at it for the rest of the branch) */
            first[0] = '\0';
            const char *src = "";
            if (rd_get_attr(tok, "src", &attrVal))
            {
                src = attrVal;
            }
            if (src[0] == '\0')
            {
                if (rd_get_attr(tok, "data-src", &attrVal))
                {
                    src = attrVal;
                }
            }
            if (src[0] == '\0')
            {
                if (rd_get_attr(tok, "srcset", &attrVal) && attrVal[0])
                {
                    /* Lua: match(srcset, "^([^%s,]+)") — up to first ws/comma. */
                    size_t n = 0;
                    while (attrVal[n] && attrVal[n] != ' ' && attrVal[n] != '\t' &&
                           attrVal[n] != '\r' && attrVal[n] != '\n' &&
                           attrVal[n] != ',' && attrVal[n] != '\f' &&
                           attrVal[n] != '\v')
                    {
                        n++;
                    }
                    size_t nn = n < sizeof(first) - 1 ? n : sizeof(first) - 1;
                    memcpy(first, attrVal, nn);
                    first[nn] = '\0';
                    src = first;
                }
            }
            const char *alt = "Image";
            if (rd_get_attr(tok, "alt", &attrVal) && attrVal[0])
            {
                alt = attrVal;
            }
            else if (rd_get_attr(tok, "title", &attrVal) && attrVal[0])
            {
                alt = attrVal;
            }
            double w = 160, h = 80;
            if (rd_get_attr(tok, "width", &attrVal))
            {
                char *end = NULL;
                double v = strtod(attrVal, &end);
                if (end && end != attrVal)
                {
                    w = v;
                }
            }
            if (rd_get_attr(tok, "height", &attrVal))
            {
                char *end = NULL;
                double v = strtod(attrVal, &end);
                if (end && end != attrVal)
                {
                    h = v;
                }
            }
            if (src[0] != '\0' && !strstr(src, "tracking") &&
                !strstr(src, "beacon"))
            {
                if (w > 360)
                {
                    w = 360;
                }
                if (h > 180)
                {
                    h = 180;
                }
                rd_commit_block(&st);
                DocBlock *b = rd_new_block(&st, DOC_BLOCK_IMAGE);
                if (b)
                {
                    char *resolved = url_resolve(baseUrl, src);
                    b->src = rd_arena_str_s(&st, resolved ? resolved : src);
                    PLUTO_FREE(resolved);
                    b->alt = rd_arena_str_s(&st, alt);
                    b->width = w;
                    b->height = h;
                    b->href = st.currentHref;
                    rd_flush_block(&st, b);
                }
            }
        }
    }

    rd_commit_block(&st);

    /* ── Score containers ─────────────────────────────────────────────────── */
    for (int i = 0; i < st.containerCount; i++)
    {
        RdContainer *c = &st.containers[i];
        double score = (double)c->wordCount + (double)c->imageCount * 30.0;
        double linkDensity = c->wordCount > 0
                                 ? ((double)c->linkWords / (double)c->wordCount)
                                 : 0.0;
        if (linkDensity > 0.5)
        {
            score *= 0.2;
        }
        else if (linkDensity > 0.33)
        {
            score *= 0.6;
        }
        if (c->isContent)
        {
            score *= 3.0;
        }
        if (c->isNav)
        {
            score *= 0.05;
        }
        c->score = (int)score;
    }
    qsort(st.containers, (size_t)st.containerCount, sizeof(RdContainer),
          rd_cmp_container);

    RdBlockVec bestBlocks = {0};
    RdBlockVec allNonNav = {0};
    if (st.containerCount > 0)
    {
        RdContainer *best = &st.containers[0];
        if (best->blockCount > 0)
        {
            int threshold = (int)(best->score * 0.15);
            /* Walk containers in SORTED order (Lua iterates the sorted table). */
            for (int i = 0; i < st.containerCount; i++)
            {
                RdContainer *c = &st.containers[i];
                if (!c->isNav && c->blockCount > 0 &&
                    (c == best || c->score >= threshold))
                {
                    for (int bi = 0; bi < c->blockCount; bi++)
                    {
                        rd_bv_push(&bestBlocks, c->blocks[bi]);
                    }
                }
            }
        }
        /* Fallback collection (Lua builds it lazily; same order). */
        for (int i = 0; i < st.containerCount; i++)
        {
            RdContainer *c = &st.containers[i];
            if (!c->isNav)
            {
                for (int bi = 0; bi < c->blockCount; bi++)
                {
                    rd_bv_push(&allNonNav, c->blocks[bi]);
                }
            }
        }
        if (bestBlocks.count == 0)
        {
            bestBlocks = allNonNav;
            allNonNav.items = NULL;
            allNonNav.cap = 0;
            allNonNav.count = 0;
        }
    }

    int totalWords = 0;
    for (int i = 0; i < bestBlocks.count; i++)
    {
        DocBlock *blk = bestBlocks.items[i];
        for (int ii = 0; ii < blk->inlineCount; ii++)
        {
            const char *t = blk->inlines[ii]->text ? blk->inlines[ii]->text : "";
            totalWords += rd_word_count(t, strlen(t));
        }
    }
    int readingMins = (int)ceil((double)totalWords / 180.0);
    if (readingMins < 1)
    {
        readingMins = 1;
    }
    char readTimeStr[64];
    snprintf(readTimeStr, sizeof(readTimeStr), "%d min read (%d words)",
             readingMins, totalWords);

    /* ── Emit into the DocParseResult ─────────────────────────────────────── */
    DocBlock *hdr = rd_new_block(&st, DOC_BLOCK_READER_HEADER);
    DocBlock *titleBlk = NULL;
    DocBlock *hrBlk = NULL;
    if (hdr)
    {
        hdr->host = rd_arena_str_s(&st, hostUpper);
        hdr->text = rd_arena_str_s(&st, pageTitle); /* title lives in text */
        hdr->readingTime = rd_arena_str_s(&st, readTimeStr);
        rd_ptrarr_push((void ***)&out->blocks, &out->blockCount, &out->blockCap,
                       hdr);

        titleBlk = rd_new_block(&st, DOC_BLOCK_HEADING);
        if (titleBlk)
        {
            titleBlk->level = 1;
            DocInline *inl = rd_new_inline(&st, DOC_INLINE_TEXT);
            if (inl)
            {
                inl->text = rd_arena_str_s(&st, pageTitle);
                inl->flags = DOC_INF_BOLD;
                rd_add_inline(&st, titleBlk, inl);
            }
            rd_ptrarr_push((void ***)&out->blocks, &out->blockCount,
                           &out->blockCap, titleBlk);
        }
        hrBlk = rd_new_block(&st, DOC_BLOCK_HR);
        if (hrBlk)
        {
            rd_ptrarr_push((void ***)&out->blocks, &out->blockCount,
                           &out->blockCap, hrBlk);
        }
    }

    RdBlockVec merged = {0};
    rd_merge_paragraph_fragments(&st, &bestBlocks, &merged);
    for (int i = 0; i < merged.count && !st.error; i++)
    {
        rd_ptrarr_push((void ***)&out->blocks, &out->blockCount, &out->blockCap,
                       merged.items[i]);
    }

    snprintf(out->title, sizeof(out->title), "%s", pageTitle);
    snprintf(out->baseUrl, sizeof(out->baseUrl), "%s", baseUrl ? baseUrl : "");
    out->isReaderMode = 1;
    out->mode = MODE_READER;
    out->readingTimeWords = totalWords;
    out->readingTimeStr = rd_arena_str_s(&st, readTimeStr);

    /* Stash the arena + per-container block arrays for document_free. */
    PLUTO_FREE(bestBlocks.items);
    PLUTO_FREE(allNonNav.items);
    PLUTO_FREE(merged.items);
    PLUTO_FREE(st.listStack);
    strbuf_free(&st.linkText);
    strbuf_free(&st.preBuf);
    strbuf_free(&st.textareaBuf);
    strbuf_free(&st.buttonBuf);

    /* Keep container block arrays (blocks are referenced by out->blocks, but
     * the arrays themselves leak unless freed; blocks live in the arena). */
    for (int i = 0; i < st.containerCount; i++)
    {
        PLUTO_FREE(st.containers[i].blocks);
    }
    PLUTO_FREE(st.containers);

    if (st.error)
    {
        rd_arena_free_all(&st.arena);
        out->parseError = 1;
        return -1;
    }

    RdArena *keep = (RdArena *)PLUTO_MALLOC(sizeof(RdArena));
    if (!keep)
    {
        rd_arena_free_all(&st.arena);
        out->parseError = 1;
        return -1;
    }
    *keep = st.arena;
    st.arena.head = NULL;
    out->_arena = keep;
    return 0;
}
