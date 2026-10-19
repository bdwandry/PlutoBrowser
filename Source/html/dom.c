/*
 * PlutoBrowser — dom.c
 * HTML tree builder (port of Source/html/dom.lua). See dom.h for the
 * preserved rule set. The Lua reference's helpers (closeOpenP, popToTag,
 * prepareListItem, prepareDtDd, prepareRow, prepareCell, prepareOption) are
 * reproduced stack-index-for-index so stray-element edge cases behave
 * identically.
 */
#include "dom.h"
#include "css.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "core/logger.h"
#include "../core/pluto_mem.h"

PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)

#define MAX_NODES 6000

#define PLUTO_REALLOC(p, n) pluto_mem_realloc((p), (n))

#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
/* Hydration-debug registry (harness builds only): nodes made through
 * dom_create_element, walked by dom_created_dump to find DETACHED fragment
 * tops after a react commit (parent==NULL subtrees = content react built
 * but never attached). */
#define DOM_CREATED_MAX 2048
static DomNode *g_created[DOM_CREATED_MAX];
static int g_createdCount;
#endif

/* ── String arena: chunk list (same design as tokenizer.c) ────────────────
 * Pointers handed out by arena_dup must stay valid for the result's lifetime
 * even as more strings are added — so chunks are allocated once and never
 * moved/reallocated. A chunk that fills up is retired; a new one is appended. */
typedef struct ArenaChunk
{
    struct ArenaChunk *next;
    size_t used;
    size_t cap;
    char data[]; /* flexible array member */
} ArenaChunk;

typedef struct
{
    ArenaChunk *head;
    ArenaChunk *tail;
} Arena;

#define ARENA_CHUNK_MIN 8192

static int arena_reserve(Arena *a, size_t extra)
{
    if (a->tail && a->tail->cap - a->tail->used >= extra + 1)
    {
        return 0;
    }
    size_t cap = ARENA_CHUNK_MIN;
    if (extra + 1 > cap)
    {
        cap = extra + 1; /* oversized string gets its own right-sized chunk */
    }
    ArenaChunk *c = (ArenaChunk *)PLUTO_MALLOC(sizeof(ArenaChunk) + cap);
    if (!c)
    {
        return -1;
    }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    if (a->tail)
    {
        a->tail->next = c;
    }
    else
    {
        a->head = c;
    }
    a->tail = c;
    return 0;
}

static char *arena_dup(Arena *a, const char *s, size_t n)
{
    if (arena_reserve(a, n) != 0)
    {
        return NULL;
    }
    char *out = a->tail->data + a->tail->used;
    memcpy(out, s, n);
    out[n] = '\0';
    a->tail->used += n + 1;
    return out;
}

/* ── SW8: public arena facade (core/pluto_page.c restores strings into the
 * doc's own arena so dom_free_result's wholesale free stays exact) ── */
char *dom_arena_dup(DomResult *dom, const char *s, int len)
{
    if (!dom || !dom->_arena || !s || len < 0)
    {
        return NULL;
    }
    return arena_dup((Arena *)dom->_arena, s, (size_t)len);
}

/* ── Phase 10: animation-completion dedupe map ────────────────────────────
 * One (node, name) pair per COMPLETED declared run, document-lifetime.
 * Returns 1 when the pair was already recorded (dedupe held), 0 when
 * newly recorded. Names copy into the doc string arena (≤24 chars —
 * longer names never reach here; the registry refuses them, so an
 * over-long animation is inert and never engages). */
int dom_anim_end_report(DomResult *dom, const void *node,
                        const char *name, int nameLen)
{
    if (!dom || !node || !name || nameLen <= 0 || nameLen > 24)
    {
        return 0; /* malformed: report "not recorded" (caller skips) */
    }
    for (int i = 0; i < dom->animFiredCount; i++)
    {
        if (dom->animFired[i].node == node &&
            dom->animFired[i].nameLen == nameLen &&
            memcmp(dom->animFired[i].name, name, (size_t)nameLen) == 0)
        {
            return 1;
        }
    }
    if (dom->animFiredCount >= DOM_ANIMFIRED_MAX)
    {
        return 0; /* cap reached: record nothing (documented residual) */
    }
    char *copy = dom_arena_dup(dom, name, nameLen);
    if (!copy)
    {
        return 0; /* arena exhaustion: record nothing */
    }
    dom->animFired[dom->animFiredCount].node = (void *)node;
    dom->animFired[dom->animFiredCount].name = copy;
    dom->animFired[dom->animFiredCount].nameLen = nameLen;
    dom->animFiredCount++;
    return 0;
}

/* ── Tag rule tables (Lua parity) ───────────────────────────────────────── */
static int is_void(const char *tag)
{
    /* WHATWG §13.1.2 void-elements. (basefont/bgsound/keygen/frame are
     * legacy-void in the spec — see also is_legacy_void below.) */
    static const char *const VOID[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(VOID) / sizeof(VOID[0]); i++)
    {
        if (strcmp(tag, VOID[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int is_skip_subtree(const char *tag)
{
    /* R17: <head> is no longer skipped — its metadata children (<link>,
     * <meta>, <base>) must exist in the DOM because pages address them by
     * id (favicon swaps, canonical-link updates) and getElementById must
     * find them like any browser. The render walker already no-ops these
     * tags, so nothing leaks into the visual page. <template> stays skipped
     * (inert content per HTML spec — browsers keep it out of getElementById
     * reach too). */
    (void)tag;
    return strcmp(tag, "template") == 0 ||
           strcmp(tag, "selectedcontent") == 0;
}

/* Legacy void elements (WHATWG §13.1.2, non-conforming-but-void list):
 * they never take children, so a stray close tag must not pop ancestors.
 * (basefont/bgsound/keygen were missing before — a page containing
 * '</basefont>' would have truncated the whole tree at that point.) */
static int is_legacy_void(const char *tag)
{
    static const char *const LEGACY_VOID[] = {
        "basefont", "bgsound", "frame", "keygen"
    };
    for (size_t i = 0; i < sizeof(LEGACY_VOID) / sizeof(LEGACY_VOID[0]); i++)
    {
        if (strcmp(tag, LEGACY_VOID[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int is_block(const char *tag)
{
    /* Block-level boxes for the implied-<p>-close rule (WHATWG §13.2.6.4:
    * a <p> element's end tag is implied by many start tags). Extended with
    * caption/optgroup/address/xmp/listing/plaintext/marquee/colgroup/fenced
    * frame/h1group beyond the reference set. */
    static const char *const BLOCK[] = {
        "address", "article", "aside", "blockquote", "body", "caption", "center",
        "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
        "h5", "h6", "head", "header", "hgroup", "hr", "li", "listing", "main",
        "marquee", "menu", "nav", "ol", "optgroup", "p", "plaintext",
        "pre", "section", "table", "ul", "xmp"
    };
    for (size_t i = 0; i < sizeof(BLOCK) / sizeof(BLOCK[0]); i++)
    {
        if (strcmp(tag, BLOCK[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* ── Node helpers ───────────────────────────────────────────────────────── */
static DomNode *node_new(void)
{
    DomNode *n = (DomNode *)PLUTO_MALLOC(sizeof(DomNode));
    if (n)
    {
        memset(n, 0, sizeof(*n));
    }
    return n;
}

static int children_reserve(DomNode *n, int need)
{
    if (need <= n->childCap)
    {
        return 0;
    }
    int cap = n->childCap ? n->childCap : 4;
    while (cap < need)
    {
        cap *= 2;
    }
    DomNode **nc = (DomNode **)PLUTO_MALLOC((size_t)cap * sizeof(DomNode *));
    if (!nc)
    {
        return -1;
    }
    if (n->children)
    {
        memcpy(nc, n->children, (size_t)n->childCount * sizeof(DomNode *));
        PLUTO_FREE(n->children);
    }
    n->children = nc;
    n->childCap = cap;
    return 0;
}

static int node_append_child(DomNode *parent, DomNode *child)
{
    if (children_reserve(parent, parent->childCount + 1) != 0)
    {
        return -1;
    }
    parent->children[parent->childCount++] = child;
    return 0;
}

/* ── Stack ──────────────────────────────────────────────────────────────── */
typedef struct
{
    DomNode **items;
    int count;
    int cap;
} Stack;

static int stack_push(Stack *s, DomNode *n)
{
    if (s->count == s->cap)
    {
        int ncap = s->cap ? s->cap * 2 : 32;
        DomNode **ni = (DomNode **)PLUTO_MALLOC((size_t)ncap * sizeof(DomNode *));
        if (!ni)
        {
            return -1;
        }
        if (s->items)
        {
            memcpy(ni, s->items, (size_t)s->count * sizeof(DomNode *));
            PLUTO_FREE(s->items);
        }
        s->items = ni;
        s->cap = ncap;
    }
    s->items[s->count++] = n;
    return 0;
}

static int stack_top_tag(const Stack *s, const char **tag)
{
    if (s->count == 0)
    {
        return 0;
    }
    *tag = s->items[s->count - 1]->tag;
    return 1;
}

/* Pop elements until (and including) the matching tag; indices mirror the
 * Lua loops: scan from top down to index 2 (1-based), i.e. never pop root. */
static void pop_to_tag(Stack *s, const char *tag)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, tag) == 0)
        {
            s->count = i; /* drop items[i..top] */
            return;
        }
    }
}

/* Lua closeOpenP: scan for the deepest open "p" (root excluded) and pop it
 * and everything above it. */
static void close_open_p(Stack *s)
{
    pop_to_tag(s, "p");
}

static int stack_has_tag(const Stack *s, const char *tag)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, tag) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* prepareListItem: close the deepest open li (stopping at a list container),
 * then require a list container in scope; otherwise collapse to root. */
static void prepare_list_item(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "li") == 0)
        {
            s->count = i;
            break;
        }
        if (strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0 ||
            strcmp(t, "menu") == 0 || strcmp(t, "dir") == 0)
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0 ||
            strcmp(t, "menu") == 0 || strcmp(t, "dir") == 0)
        {
            return;
        }
    }
    s->count = 1;
}

static void prepare_dt_dd(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "dt") == 0 || strcmp(t, "dd") == 0)
        {
            s->count = i;
            break;
        }
        if (strcmp(t, "dl") == 0)
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, "dl") == 0)
        {
            return;
        }
    }
    s->count = 1;
}

static int is_table_ctx(const char *t)
{
    return strcmp(t, "table") == 0 || strcmp(t, "tbody") == 0 ||
           strcmp(t, "thead") == 0 || strcmp(t, "tfoot") == 0;
}

/* Returns 1 when the element should be pushed, 0 when dropped. */
static int prepare_row(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "tr") == 0 || strcmp(t, "td") == 0 || strcmp(t, "th") == 0)
        {
            s->count = i;
            break;
        }
        if (is_table_ctx(t))
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (is_table_ctx(s->items[i]->tag))
        {
            return 1;
        }
    }
    return 0;
}

static int prepare_cell(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0)
        {
            s->count = i;
            break;
        }
        if (strcmp(t, "tr") == 0)
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, "tr") == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int prepare_option(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "option") == 0)
        {
            s->count = i;
            break;
        }
        if (strcmp(t, "select") == 0)
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, "select") == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* <optgroup>: an open optgroup is closed by another optgroup or by
 * </select> (WHATWG §13.2.6.5: end tags implied by optgroup start tags).
 * An optgroup outside a <select> is dropped (parity with prepare_option). */
static int prepare_optgroup(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "optgroup") == 0)
        {
            s->count = i;
            break;
        }
        if (strcmp(t, "select") == 0)
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, "select") == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* <caption>: closes an open caption; dropped outside a <table>
 * (WHATWG table foster rules, simplified to the reference's flavor). */
static int prepare_caption(Stack *s)
{
    for (int i = s->count - 1; i >= 1; i--)
    {
        const char *t = s->items[i]->tag;
        if (strcmp(t, "caption") == 0)
        {
            s->count = i;
            break;
        }
        if (is_table_ctx(t))
        {
            break;
        }
    }
    for (int i = s->count - 1; i >= 1; i--)
    {
        if (strcmp(s->items[i]->tag, "table") == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* ── Attribute copy (tokens → DOM; arena-owned strings) ─────────────────── */
static int copy_attrs(DomNode *el, const Token *tok, Arena *arena)
{
    if (tok->attrCount == 0)
    {
        return 0;
    }
    el->attrs = (DomAttr *)PLUTO_MALLOC((size_t)tok->attrCount * sizeof(DomAttr));
    if (!el->attrs)
    {
        return -1;
    }
    for (int a = 0; a < tok->attrCount; a++)
    {
        const TokenAttr *ta = &tok->attrs[a];
        char *k = arena_dup(arena, ta->key, strlen(ta->key));
        if (!k)
        {
            return -1;
        }
        char *v;
        if (ta->value == PLUTO_TOK_ATTR_TRUE)
        {
            v = (char *)PLUTO_TOK_ATTR_TRUE;
        }
        else
        {
            v = arena_dup(arena, ta->value, strlen(ta->value));
            if (!v)
            {
                return -1;
            }
        }
        el->attrs[el->attrCount].key = k;
        el->attrs[el->attrCount].value = v;
        el->attrCount++;
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int dom_build(const TokenizeResult *tokens, DomResult *out)
{
    memset(out, 0, sizeof(*out));

    Arena *arena = (Arena *)PLUTO_MALLOC(sizeof(Arena));
    if (!arena)
    {
        return -1;
    }
    memset(arena, 0, sizeof(*arena));
    out->_arena = arena;

    DomNode *root = node_new();
    if (!root)
    {
        return -1;
    }
    root->kind = DOM_ELEMENT;
    root->tag = arena_dup(arena, "#root", 5);
    if (!root->tag)
    {
        return -1;
    }
    out->root = root;
    root->parent = NULL;

    Stack stack;
    memset(&stack, 0, sizeof(stack));
    if (stack_push(&stack, root) != 0)
    {
        return -1;
    }

    int count = 0;
    int skipDepth = 0;
    const char *skipTag = NULL;
    int diagSkipped = 0;

    for (int i = 0; i < tokens->tokens.count; i++)
    {
        const Token *tok = &tokens->tokens.items[i];
        out->tokensProcessed++;
        if (count >= MAX_NODES)
        {
            break;
        }

        if (skipDepth > 0)
        {
            if (tok->type == TOK_TAG && tok->isClosing && skipTag &&
                strcmp(tok->name, skipTag) == 0)
            {
                skipDepth = 0;
                skipTag = NULL;
            }
            else
            {
                diagSkipped++;
                if (diagSkipped >= 500)
                {
                    skipDepth = 0; /* valve quirk: token NOT consumed */
                    skipTag = NULL;
                }
            }
        }
        else if (tok->type == TOK_TEXT)
        {
            if (count < MAX_NODES)
            {
                DomNode *n = node_new();
                if (!n)
                {
                    goto fail;
                }
                n->kind = DOM_TEXT;
                n->text = arena_dup(arena, tok->content, strlen(tok->content));
                n->parent = stack.items[stack.count - 1];
                if (!n->text || node_append_child(stack.items[stack.count - 1], n) != 0)
                {
                    goto fail;
                }
                count++;
                out->textNodes++;
            }
        }
        else if (tok->type == TOK_COMMENT)
        {
            /* Comment nodes stay at the OPEN-ELEMENT insertion point
             * (WHATWG keeps comments outside the element-depth stack).
             * React's hydration markers ride on these nodes. */
            if (count < MAX_NODES)
            {
                DomNode *n = node_new();
                if (!n)
                {
                    goto fail;
                }
                n->kind = DOM_COMMENT;
                n->text = arena_dup(arena, tok->content ? tok->content : "",
                                    tok->content ? strlen(tok->content) : 0);
                if (!n->text)
                {
                    PLUTO_FREE(n);
                    goto fail;
                }
                n->parent = stack.items[stack.count - 1];
                if (node_append_child(stack.items[stack.count - 1], n) != 0)
                {
                    goto fail;
                }
                count++;
            }
        }
        else
        {
            const char *tag = tok->name ? tok->name : "";
            if (tok->isClosing)
            {
                if (!is_void(tag) && !is_legacy_void(tag) && !is_skip_subtree(tag))
                {
                    pop_to_tag(&stack, tag);
                }
            }
            else
            {
                /* WHATWG §13.2.6.4.7: an <image> start tag in body is a
                 * parse error; change the tag name to "img" and
                 * reprocess — the image renders as an image. Must run
                 * before the void check so it takes img's code path. */
                if (strcmp(tag, "image") == 0)
                {
                    tag = "img";
                }
                if (is_skip_subtree(tag))
                {
                    skipDepth = 1;
                    skipTag = tag;
                }
                else if (is_void(tag) || is_legacy_void(tag))
                {
                    /* Legacy voids (basefont/bgsound/frame/keygen) append
                     * like voids and are never pushed — children after them
                     * stay in the parent's flow (WHATWG §13.1.2). */
                    DomNode *el = node_new();
                    if (!el)
                    {
                        goto fail;
                    }
                    el->kind = DOM_ELEMENT;
                    el->tag = arena_dup(arena, tag, strlen(tag));
                    el->parent = stack.items[stack.count - 1];
                    if (!el->tag || copy_attrs(el, tok, arena) != 0 ||
                        node_append_child(stack.items[stack.count - 1], el) != 0)
                    {
                        goto fail;
                    }
                    count++;
                    out->elemNodes++;
                }
                else
                {
                    int doPush = 1;
                    if (is_block(tag))
                    {
                        close_open_p(&stack);
                    }
                    if (strcmp(tag, "li") == 0)
                    {
                        prepare_list_item(&stack);
                    }
                    else if (strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0)
                    {
                        prepare_dt_dd(&stack);
                    }
                    else if (strcmp(tag, "tr") == 0 || strcmp(tag, "thead") == 0 ||
                             strcmp(tag, "tbody") == 0 || strcmp(tag, "tfoot") == 0)
                    {
                        doPush = prepare_row(&stack);
                    }
                    else if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0)
                    {
                        doPush = prepare_cell(&stack);
                    }
                    else if (strcmp(tag, "option") == 0)
                    {
                        doPush = prepare_option(&stack);
                    }
                    else if (strcmp(tag, "optgroup") == 0)
                    {
                        doPush = prepare_optgroup(&stack);
                    }
                    else if (strcmp(tag, "caption") == 0)
                    {
                        doPush = prepare_caption(&stack);
                    }
                    else if (strcmp(tag, "a") == 0)
                    {
                        pop_to_tag(&stack, "a");
                    }

                    if (doPush)
                    {
                        DomNode *el = node_new();
                        if (!el)
                        {
                            goto fail;
                        }
                        el->kind = DOM_ELEMENT;
                        el->tag = arena_dup(arena, tag, strlen(tag));
                        el->parent = stack.items[stack.count - 1];
                        if (!el->tag || copy_attrs(el, tok, arena) != 0)
                        {
                            goto fail;
                        }
                        if (node_append_child(stack.items[stack.count - 1], el) != 0)
                        {
                            goto fail;
                        }
                        count++;
                        out->elemNodes++;
                        if (!tok->isSelfClosing)
                        {
                            if (stack_push(&stack, el) != 0)
                            {
                                goto fail;
                            }
                        }
                    }
                }
            }
        }
    }

    out->skippedDepth = diagSkipped;
    /* Lua parity: maxNodesHit is only set inside append() when it refuses;
     * the token loop breaks at the cap first, so it stays false (tc6). */
    out->maxNodesHit = 0;
    PLUTO_FREE(stack.items);
    return 0;

fail:
    PLUTO_FREE(stack.items);
    return -1;
}

int dom_build_from_html(const char *html, DomResult *out)
{
    memset(out, 0, sizeof(*out));
    TokenizeResult tr;
    if (tokenizer_tokenize(html, &tr) != 0)
    {
        return -1;
    }
    int rc = dom_build(&tr, out);
    tokenizer_free_result(&tr);
    return rc;
}

const DomNode *dom_find_child(const DomNode *node, const char *tag)
{
    if (!node || node->kind != DOM_ELEMENT)
    {
        return NULL;
    }
    for (int i = 0; i < node->childCount; i++)
    {
        const DomNode *c = node->children[i];
        if (c->kind == DOM_ELEMENT && strcmp(c->tag, tag) == 0)
        {
            return c;
        }
    }
    return NULL;
}

const char *dom_get_attr(const DomNode *node, const char *key)
{
    for (int i = 0; i < node->attrCount; i++)
    {
        if (strcmp(node->attrs[i].key, key) == 0)
        {
            return node->attrs[i].value == PLUTO_TOK_ATTR_TRUE ? "" : node->attrs[i].value;
        }
    }
    return NULL;
}

const char *dom_css_attr(const void *node, const char *name, size_t namelen)
{
    const DomNode *n = (const DomNode *)node;
    char key[64];
    if (!n || !name || namelen == 0 || namelen >= sizeof(key))
    {
        return NULL;
    }
    for (size_t i = 0; i < namelen; i++)
    {
        char c = name[i];
        key[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    key[namelen] = '\0';
    return dom_get_attr(n, key);
}

const void *dom_css_sibling(const void *node, int dir)
{
    const DomNode *n = (const DomNode *)node;
    if (!n || !n->parent || (dir != -1 && dir != 1))
    {
        return NULL;
    }
    const DomNode *par = n->parent;
    int idx = -1;
    for (int i = 0; i < par->childCount; i++)
    {
        if (par->children[i] == n)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        return NULL; /* detached (should not happen) */
    }
    for (int i = idx + dir; i >= 0 && i < par->childCount; i += dir)
    {
        const DomNode *s = par->children[i];
        if (s && s->kind == DOM_ELEMENT)
        {
            return s;
        }
    }
    return NULL;
}

/* Phase 1: nearest ELEMENT sibling WITH selector-relevant info filled
 * (tag lowercased compare, class, id). Backs the '+' / '~' sibling
 * combinators in css_chain_matches — same walk as dom_css_sibling,
 * adding the qs_compound_of-style info extraction. */
static int qs_compound_of(const DomNode *n, const char **tagLower,
                          const char **classAttr, const char **idAttr);

const void *dom_css_sibling_info(const void *node, int dir,
                                 const char **tagLower,
                                 const char **classAttr,
                                 const char **idAttr)
{
    const DomNode *n = (const DomNode *)node;
    if (!n || !n->parent || (dir != -1 && dir != 1))
    {
        return NULL;
    }
    const DomNode *par = n->parent;
    int idx = -1;
    for (int i = 0; i < par->childCount; i++)
    {
        if (par->children[i] == n)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        return NULL; /* detached (should not happen) */
    }
    for (int i = idx + dir; i >= 0 && i < par->childCount; i += dir)
    {
        const DomNode *s = par->children[i];
        if (s && s->kind == DOM_ELEMENT)
        {
            qs_compound_of(s, tagLower, classAttr, idAttr);
            return s;
        }
    }
    return NULL;
}

static int dom_css_childless(const void *node); /* Phase 7 :empty truth */

void dom_css_dom(CssDom *d,
                 const void *(*ancestor)(const void *node, int i,
                                         const char **tagLower,
                                         const char **classAttr,
                                         const char **idAttr))
{
    if (!d)
    {
        return;
    }
    d->ancestor = ancestor;
    d->attr = dom_css_attr;
    d->sibling = dom_css_sibling;
    d->siblingInfo = dom_css_sibling_info;
    d->childless = dom_css_childless;
}

/* Phase 7: spec :empty truth (CSS Selectors 4 §9.1): the node generates
 * NO content — no element children and no text; comment children are
 * transparent ("<p><!-- --></p>" IS :empty). Data/doc-type children
 * (innerHTML blobs, raw data nodes) stay content — conservative true
 * only for the comment-only case. */
static int dom_css_childless(const void *node)
{
    const DomNode *n = (const DomNode *)node;
    if (!n)
    {
        return -1;
    }
    for (int i = 0; i < n->childCount; i++)
    {
        const DomNode *c = n->children[i];
        if (c && c->kind != DOM_COMMENT)
        {
            return 0;
        }
    }
    return 1;
}

/* ── Runtime DOM mutation API (JavaScript bridge) ────────────────────────── */

int dom_node_id(const DomResult *dom, DomNode *node)
{
    if (!node)
    {
        return 0;
    }
    if (node->nodeId)
    {
        return node->nodeId;
    }
    /* ids are unique per document and never reused: a detached node's old
     * _dN id must never resolve to a different element. */
    DomResult *m = (DomResult *)dom;
    m->nextNodeId += 1;
    node->nodeId = m->nextNodeId;
    return node->nodeId;
}

/* First stub (pagedKey != 0) in the tree, iterative. */
static DomNode *find_first_stub(DomNode *root)
{
    if (!root)
    {
        return NULL;
    }
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (n->pagedKey)
        {
            return n;
        }
        if (top + n->childCount > DOM_SEARCH_MAX_DEPTH)
        {
            return NULL;
        }
        for (int i = 0; i < n->childCount; i++)
        {
            stack[top++] = n->children[i];
        }
    }
    return NULL;
}

/* RAM-only pass used by dom_node_by_id: finds the id without materializing
 * any paged-out stubs (the common case stays zero-flash-cost). */
static DomNode *find_id_ram_only(DomResult *dom, int id)
{
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (n->pagedKey)
        {
            continue; /* stub: skip (its restored children keep their ids) */
        }
        if (n->nodeId == id)
        {
            return n;
        }
        if (top + n->childCount > DOM_SEARCH_MAX_DEPTH)
        {
            return NULL; /* pathological breadth; give up rather than grow */
        }
        for (int i = 0; i < n->childCount; i++)
        {
            stack[top++] = n->children[i];
        }
    }
    return NULL;
}

/* SW8 touch point: when the fast RAM pass misses, the id may live inside a
 * paged-out stub (JS held the id across a page-out). Materialize one stub
 * per retry (materialize is O(stub)); bail out after a few so a corrupt
 * store can't spin the loop forever. */
DomNode *dom_node_by_id(const DomResult *dom, int id)
{
    if (!dom || !dom->root || id <= 0)
    {
        return NULL;
    }
    DomResult *m = (DomResult *)dom;
    for (int pass = 0; pass < 8; pass++)
    {
        DomNode *hit = find_id_ram_only(m, id);
        if (hit || pass == 7)
        {
            return hit;
        }
        DomNode *stub = find_first_stub(m->root);
        if (!stub)
        {
            return NULL; /* genuinely absent */
        }
        dom_touch(m, stub);
    }
    return NULL;
}

DomNode *dom_create_element(DomResult *dom, const char *tag)
{
    if (!dom || !tag || !tag[0])
    {
        return NULL;
    }
    /* Lowercase per HTML tag-name rules. */
    char lower[32];
    size_t n = strlen(tag);
    if (n >= sizeof(lower))
    {
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = tag[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[n] = '\0';

    /* Browsers make createElement("noscript") an inert stub (its children
     * never render while scripting is on, which is always true here since
     * scripts only run through the bridge). Keep one from becoming a live
     * carrier for SPA warning markup. DOCTYPE guard: pre-HTML5 pages treat
     * unknown/void-ish elements as open (classic noembed/noframes behavior) —
     * noscript has no meaningful fallback content there either way. */
    if (strcmp(lower, "noscript") == 0 && dom->root &&
        dom->root->childCount > 0)
    {
        const DomNode *fc = dom->root->children[0];
        if (fc && fc->kind == DOM_ELEMENT &&
            fc->tag && strcmp(fc->tag, "html") == 0 &&
            fc->childCount > 0 && fc->children[0] &&
            fc->children[0]->kind == DOM_TEXT &&
            fc->children[0]->text &&
            strncmp(fc->children[0]->text, "<!DOCTYPE", 9) == 0)
        {
            return NULL; /* pre-HTML5 page: pre-H5 noscript = inert stub */
        }
        return NULL; /* standards page: inert stub (never rendered) */
    }

    DomNode *el = node_new();
    if (!el)
    {
        return NULL;
    }
    el->kind = DOM_ELEMENT;
    el->tag = arena_dup((Arena *)dom->_arena, lower, n);
    if (!el->tag)
    {
        PLUTO_FREE(el);
        return NULL;
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    /* Hydration-debug registry: nodes created via the DOM API (react's
     * createElement path). Harness-only `dom_created_dump` walks it to
     * find detached fragment tops after commits. */
    if (g_createdCount < DOM_CREATED_MAX)
    {
        g_created[g_createdCount++] = el;
    }
#endif
    return el;
}

DomNode *dom_create_text(DomResult *dom, const char *text)
{
    if (!dom || !text)
    {
        return NULL;
    }
    DomNode *n = node_new();
    if (!n)
    {
        return NULL;
    }
    n->kind = DOM_TEXT;
    n->text = arena_dup((Arena *)dom->_arena, text, strlen(text));
    if (!n->text)
    {
        PLUTO_FREE(n);
        return NULL;
    }
    return n;
}

/* ── dom_build companion: zero-child comment node (hydration markers,
 * document.createComment). Data is RAW (no entity decode). */
DomNode *dom_create_comment(DomResult *dom, const char *data)
{
    if (!dom || !data)
    {
        return NULL;
    }
    DomNode *n = node_new();
    if (!n)
    {
        return NULL;
    }
    n->kind = DOM_COMMENT;
    n->text = arena_dup((Arena *)dom->_arena, data, strlen(data));
    if (!n->text)
    {
        PLUTO_FREE(n);
        return NULL;
    }
    return n;
}

int dom_append_child(DomResult *dom, DomNode *parent, DomNode *child)
{
    (void)dom;
    if (!parent || !child || parent == child)
    {
        return -1;
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[dom] append parent=%s(%p) child=%s(%p) kind=%d",
               (parent->tag) ? parent->tag : "?", (void *)parent,
               (child->tag) ? child->tag : "?", (void *)child,
               (int)child->kind);
#endif
    if (node_append_child(parent, child) != 0)
    {
        return -1;
    }
    child->parent = parent;
    return 0;
}

#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
static void dom_created_dump_dfs(DomNode *n, int depth, int *shown)
{
    if (!n || depth > 4 || *shown > 40)
    {
        return;
    }
    logger_log("[frg] %.*s %s kids=%d", depth * 2, "        ",
               (n->tag) ? n->tag : (n->kind == DOM_TEXT ? "#text" : "?"),
               n->childCount);
    (*shown)++;
    for (int i = 0; i < n->childCount; i++)
    {
        dom_created_dump_dfs(n->children[i], depth + 1, shown);
    }
}

/* Dump every root-less fragment (top node with parent==NULL and real
 * content) among the created-element registry — the shape of the tree
 * react COMMITTED but never attached. */
void dom_created_dump(void)
{
    logger_log("[frg] dump begin created=%d", g_createdCount);
    for (int i = 0; i < g_createdCount; i++)
    {
        DomNode *n = g_created[i];
        if (n && n->parent == NULL && n->childCount > 0 &&
            n->tag && strcmp(n->tag, "#root") != 0 &&
            !n->shadowHost) /* shadow roots are detached by DESIGN */
        {
            int shown = 0;
            dom_created_dump_dfs(n, 0, &shown);
        }
    }
    logger_log("[frg] dump end");
}
#endif

int dom_remove_child(DomNode *parent, DomNode *child)
{
    if (!parent || !child)
    {
        return -1;
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[dom] remove parent=%s child=%s",
               (parent->tag) ? parent->tag : "?",
               (child->tag) ? child->tag : "?");
#endif
    for (int i = 0; i < parent->childCount; i++)
    {
        if (parent->children[i] == child)
        {
            memmove(&parent->children[i], &parent->children[i + 1],
                    sizeof(DomNode *) * (size_t)(parent->childCount - i - 1));
            parent->childCount--;
            child->parent = NULL;
            return 0;
        }
    }
    return -1;
}

int dom_set_text(DomResult *dom, DomNode *el, const char *text)
{
    if (!dom || !el || el->kind != DOM_ELEMENT || !text)
    {
        return -1;
    }
    /* Drop existing text-node children; element children are kept (like
     * innerText assignment keeping sub-element markup). */
    int w = 0;
    for (int i = 0; i < el->childCount; i++)
    {
        if (el->children[i]->kind != DOM_TEXT)
        {
            el->children[w++] = el->children[i];
        }
    }
    el->childCount = w;
    DomNode *t = dom_create_text(dom, text);
    if (!t)
    {
        return -1;
    }
    return node_append_child(el, t);
}

int dom_set_attr(DomResult *dom, DomNode *el, const char *key, const char *value)
{
    if (!dom || !el || el->kind != DOM_ELEMENT || !key || !key[0] || !value)
    {
        return -1;
    }
    /* Replace in place. */
    for (int i = 0; i < el->attrCount; i++)
    {
        if (strcmp(el->attrs[i].key, key) == 0)
        {
            char *v = arena_dup((Arena *)dom->_arena, value, strlen(value));
            if (!v)
            {
                return -1;
            }
            el->attrs[i].value = v;
            return 0;
        }
    }
    /* Append. Boolean-attribute marker is never produced here: script-set
     * attributes always carry a string value (possibly ""). */
    DomAttr *na = (DomAttr *)PLUTO_REALLOC(el->attrs,
                    sizeof(DomAttr) * (size_t)(el->attrCount + 1));
    if (!na)
    {
        return -1;
    }
    el->attrs = na;
    char *k = arena_dup((Arena *)dom->_arena, key, strlen(key));
    if (!k)
    {
        return -1;
    }
    char *v = arena_dup((Arena *)dom->_arena, value, strlen(value));
    if (!v)
    {
        return -1;
    }
    el->attrs[el->attrCount].key = k;
    el->attrs[el->attrCount].value = v;
    el->attrCount++;
    return 0;
}

int dom_remove_attr(DomNode *el, const char *key)
{
    if (!el || el->kind != DOM_ELEMENT || !key)
    {
        return -1;
    }
    for (int i = 0; i < el->attrCount; i++)
    {
        if (strcmp(el->attrs[i].key, key) == 0)
        {
            memmove(&el->attrs[i], &el->attrs[i + 1],
                    sizeof(DomAttr) * (size_t)(el->attrCount - i - 1));
            el->attrCount--;
            return 0;
        }
    }
    return -1;
}

/* ── innerHTML subtree serializer (browser-parity markup writer) ─────────
 * Serializes an element's children as HTML markup. Next.js's root error
 * boundary snapshots `document.documentElement.innerHTML` and re-renders
 * `<html dangerouslySetInnerHTML={{__html: snapshot}}>` on a hydration
 * fallback; with the old text-degraded getter the snapshot was plain body
 * text and that assignment erased head+body (the hydration gap). This
 * writes REAL markup: `<tag attr="v">…</tag>`, void elements bare,
 * text escaped for & < >, attrs also for ", comments copied,
 * script/style text raw.
 * Returns 0 + heap string (caller frees via PLUTO_FREE); -1 bad arg,
 * -2 alloc failure, -3 runaway depth. len (optional) gets the length. */
int dom_serialize_children_html(const DomNode *el, char **out, size_t *len);

typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
} HtmlBuf;

static int hb_grow(HtmlBuf *hb, size_t need)
{
    if (hb->len + need + 1 <= hb->cap)
    {
        return 0;
    }
    size_t ncap = hb->cap ? hb->cap : 1024;
    while (hb->len + need + 1 > ncap)
    {
        ncap *= 2; /* 1024 → 8MB ceiling (32 doublings) */
    }
    char *nb = (char *)PLUTO_REALLOC(hb->buf, ncap);
    if (!nb)
    {
        return -1;
    }
    hb->buf = nb;
    hb->cap = ncap;
    return 0;
}

static int hb_put(HtmlBuf *hb, const char *s, size_t n)
{
    if (!s || n == 0)
    {
        return 0;
    }
    if (hb_grow(hb, n) != 0)
    {
        return -1;
    }
    memcpy(hb->buf + hb->len, s, n);
    hb->len += n;
    hb->buf[hb->len] = '\0';
    return 0;
}

static int hb_puts(HtmlBuf *hb, const char *s)
{
    return hb_put(hb, s, s ? strlen(s) : 0);
}

/* Text-escape & < > ; attr-escape & < > " (needsQuotes path). */
static int hb_put_text(HtmlBuf *hb, const char *s, int isAttr)
{
    for (size_t i = 0; s[i]; i++)
    {
        char c = s[i];
        const char *rep =
            (c == '&') ? "&amp;" : (c == '<') ? "&lt;" : (c == '>') ? "&gt;"
                                     : (isAttr && c == '"') ? "&quot;"
                                                            : NULL;
        if (rep)
        {
            if (hb_puts(hb, rep) != 0)
            {
                return -1;
            }
        }
        else
        {
            if (hb_put(hb, &c, 1) != 0)
            {
                return -1;
            }
        }
    }
    return 0;
}

static int dom_serialize_node_html(const DomNode *n, HtmlBuf *hb, int depth,
                                   int rawText)
{
    if (!n || !hb)
    {
        return -1;
    }
    if (depth > 24)
    {
        return -3; /* runaway markup guard (top-level contract preserved) */
    }
    if (n->kind == DOM_TEXT)
    {
        return hb_put_text(hb, n->text ? n->text : "", rawText);
    }
    if (n->kind == DOM_COMMENT)
    {
        if (hb_puts(hb, "<!--") != 0)
        {
            return -1;
        }
        if (hb_puts(hb, n->text ? n->text : "") != 0)
        {
            return -1;
        }
        return hb_puts(hb, "-->");
    }
    if (n->kind != DOM_ELEMENT || !n->tag)
    {
        return -1;
    }
    /* NOTE: bool attribute marker stores presence as "" — emitted as
     * `key=""` (browser-shaped; attr serializers never emit bare keys). */
    static const char *const VOID_TAGS[] = {"area", "base", "br", "col",
                                            "embed", "hr", "img", "input",
                                            "link", "meta", "param", "source",
                                            "track", "wbr"};
    if (hb_puts(hb, "<") != 0 || hb_puts(hb, n->tag) != 0)
    {
        return -1;
    }
    for (int a = 0; a < n->attrCount; a++)
    {
        const char *v = dom_get_attr(n, n->attrs[a].key);
        if (hb_puts(hb, " ") != 0 || hb_puts(hb, n->attrs[a].key) != 0)
        {
            return -1;
        }
        if (v && v[0])
        {
            if (hb_puts(hb, "=\"") != 0 || hb_put_text(hb, v, 1) != 0 ||
                hb_puts(hb, "\"") != 0)
            {
                return -1;
            }
        }
        else
        {
            /* value "" / marker: emit `attr=""` (browser round-trip). */
            if (hb_puts(hb, "=\"") != 0 || hb_puts(hb, "\"") != 0)
            {
                return -1;
            }
        }
    }
    if (hb_puts(hb, ">") != 0)
    {
        return -1;
    }
    int isVoid = 0;
    for (size_t v = 0; v < sizeof(VOID_TAGS) / sizeof(VOID_TAGS[0]); v++)
    {
        if (strcmp(n->tag, VOID_TAGS[v]) == 0)
        {
            isVoid = 1;
            break;
        }
    }
    if (isVoid)
    {
        return 0;
    }
    /* script/style content renders raw (textarea escapes like plain
     * text nodes — browser serialization parity). */
    int kidsRaw =
        (!strcmp(n->tag, "script") || !strcmp(n->tag, "style")) ? 1 : 0;
    for (int i = 0; i < n->childCount; i++)
    {
        if (dom_serialize_node_html(n->children[i], hb, depth + 1, kidsRaw) != 0)
        {
            return -1;
        }
    }
    if (hb_puts(hb, "</") != 0 || hb_puts(hb, n->tag) != 0 ||
        hb_puts(hb, ">") != 0)
    {
        return -1;
    }
    return 0;
}

int dom_serialize_children_html(const DomNode *el, char **out, size_t *len)
{
    if (!out || !el || el->kind != DOM_ELEMENT)
    {
        return -1;
    }
    *out = NULL;
    if (len)
    {
        *len = 0;
    }
    HtmlBuf hb;
    hb.buf = NULL;
    hb.len = 0;
    hb.cap = 0;
    for (int i = 0; i < el->childCount; i++)
    {
        int rc = dom_serialize_node_html(el->children[i], &hb, 0, 0);
        if (rc == -3)
        {
            PLUTO_FREE(hb.buf);
            return -3;
        }
        if (rc != 0)
        {
            PLUTO_FREE(hb.buf);
            return -2;
        }
    }
    if (!hb.buf)
    {
        hb.buf = (char *)PLUTO_MALLOC(1);
        if (hb.buf)
        {
            hb.buf[0] = '\0';
        }
    }
    if (!hb.buf)
    {
        return -2;
    }
    *out = hb.buf;
    if (len)
    {
        *len = hb.len;
    }
    return 0;
}

DomNode *dom_first_element_child(const DomNode *node)
{
    if (!node)
    {
        return NULL;
    }
    for (int i = 0; i < node->childCount; i++)
    {
        if (node->children[i]->kind == DOM_ELEMENT)
        {
            return node->children[i];
        }
    }
    return NULL;
}

DomNode *dom_next_element_sibling(const DomNode *node)
{
    if (!node || !node->parent)
    {
        return NULL;
    }
    const DomNode *p = node->parent;
    for (int i = 0; i < p->childCount; i++)
    {
        if (p->children[i] == node)
        {
            for (int j = i + 1; j < p->childCount; j++)
            {
                if (p->children[j]->kind == DOM_ELEMENT)
                {
                    return p->children[j];
                }
            }
            return NULL;
        }
    }
    return NULL;
}

int dom_insert_before(DomResult *dom, DomNode *parent, DomNode *child,
                      const DomNode *ref)
{
    (void)dom;
    if (!parent || !child || parent == child || child == ref)
    {
        return -1;
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[dom] insertBefore parent=%s child=%s ref=%s",
               (parent->tag) ? parent->tag : "?", (child->tag) ? child->tag : "?",
               ref ? (ref->tag ? ref->tag : "?") : "(null)");
#endif
    if (ref && ref->parent != parent)
    {
        return -1; /* ref must be a child of parent (DOM semantics) */
    }
    /* Same-node re-insert = move: remove from its current position first. */
    if (child->parent == parent)
    {
        dom_remove_child(parent, child);
    }
    else if (child->parent)
    {
        dom_remove_child(child->parent, child);
    }
    int at = parent->childCount;
    if (ref)
    {
        at = parent->childCount;
        for (int i = 0; i < parent->childCount; i++)
        {
            if (parent->children[i] == ref)
            {
                at = i;
                break;
            }
        }
        if (at == parent->childCount)
        {
            return -1; /* not found after the remove — refuse (can't happen:
                        * the ref check above ran pre-remove, but stay safe) */
        }
    }
    if (children_reserve(parent, parent->childCount + 1) != 0)
    {
        return -1;
    }
    memmove(&parent->children[at + 1], &parent->children[at],
            sizeof(DomNode *) * (size_t)(parent->childCount - at));
    parent->children[at] = child;
    parent->childCount++;
    child->parent = parent;
    return 0;
}

/* ── SW5: classList (O4) ─────────────────────────────────────────────────── */

typedef struct
{
    char buf[512]; /* rewritten "a b c" — class lists beyond this drop */
} ClassScratch;

static int class_token_len(const char *p)
{
    int n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           p[n] != '\r' && p[n] != '\f')
    {
        n++;
    }
    return n;
}

int dom_class_has(const DomNode *el, const char *token)
{
    if (!el || el->kind != DOM_ELEMENT || !token || !token[0])
    {
        return 0;
    }
    const char *cls = dom_get_attr(el, "class");
    if (!cls)
    {
        return 0;
    }
    const char *p = cls;
    while (*p)
    {
        int n = class_token_len(p);
        if (n > 0 && strlen(token) == (size_t)n && memcmp(p, token, (size_t)n) == 0)
        {
            return 1;
        }
        p += n;
        while (*p && class_token_len(p) == 0)
        {
            p++;
        }
    }
    return 0;
}

/* Rewrite the class attribute from a keep-filter over the current tokens.
 * keep(p, n, ud) returns 1 to keep the token. mode 0 = plain rewrite
 * (remove). Returns 0 ok, -1 alloc failure / oversized result. */
static int class_rewrite(DomResult *dom, DomNode *el,
                         int (*keep)(const char *p, int n, void *ud),
                         void *ud)
{
    static ClassScratch cs; /* static: off the device game-task stack */
    const char *cls = dom_get_attr(el, "class");
    size_t off = 0;
    cs.buf[0] = '\0';
    if (cls)
    {
        const char *p = cls;
        while (*p)
        {
            int n = class_token_len(p);
            if (n > 0 && keep(p, n, ud))
            {
                if (off > 0)
                {
                    cs.buf[off++] = ' ';
                }
                if (off + (size_t)n >= sizeof(cs.buf))
                {
                    return -1; /* class list outgrew the budget: no change */
                }
                memcpy(cs.buf + off, p, (size_t)n);
                off += (size_t)n;
                cs.buf[off] = '\0';
            }
            p += n;
            while (*p && class_token_len(p) == 0)
            {
                p++;
            }
        }
    }
    if (off == 0)
    {
        if (cls)
        {
            dom_remove_attr(el, "class");
        }
        return 0;
    }
    return dom_set_attr(dom, el, "class", cs.buf);
}

int dom_class_add(DomResult *dom, DomNode *el, const char *token)
{
    if (!dom || !el || el->kind != DOM_ELEMENT || !token || !token[0])
    {
        return -1;
    }
    if (class_token_len(token) != (int)strlen(token))
    {
        return -1; /* whitespace inside a token is invalid DOM */
    }
    if (dom_class_has(el, token))
    {
        return 0;
    }
    static ClassScratch cs; /* static: off the device game-task stack */
    const char *cls = dom_get_attr(el, "class");
    size_t off = 0;
    cs.buf[0] = '\0';
    if (cls && cls[0])
    {
        size_t cl = strlen(cls);
        if (cl >= sizeof(cs.buf))
        {
            return -1;
        }
        memcpy(cs.buf, cls, cl + 1);
        off = cl;
    }
    if (off + strlen(token) + 2 > sizeof(cs.buf))
    {
        return -1;
    }
    if (off > 0)
    {
        cs.buf[off++] = ' ';
    }
    memcpy(cs.buf + off, token, strlen(token) + 1);
    return dom_set_attr(dom, el, "class", cs.buf);
}

static int keep_not_token(const char *p, int n, void *ud)
{
    const char *tok = (const char *)ud;
    return !(strlen(tok) == (size_t)n && memcmp(p, tok, (size_t)n) == 0);
}

int dom_class_remove(DomResult *dom, DomNode *el, const char *token)
{
    if (!dom || !el || el->kind != DOM_ELEMENT || !token || !token[0])
    {
        return -1;
    }
    if (!dom_class_has(el, token))
    {
        return 0; /* removing an absent class is a no-op (DOM spec) */
    }
    return class_rewrite(dom, el, keep_not_token, (void *)token);
}

int dom_class_toggle(DomResult *dom, DomNode *el, const char *token)
{
    if (!dom || !el || el->kind != DOM_ELEMENT || !token || !token[0])
    {
        return -1;
    }
    if (dom_class_has(el, token))
    {
        return (dom_class_remove(dom, el, token) == 0) ? 0 : -1;
    }
    return (dom_class_add(dom, el, token) == 0) ? 1 : -1;
}



/* ── SW5: querySelector machinery (O3) ─────────────────────────────────────
 * The parsed compounds point into the caller's scratch buffer, so the walk
 * below is allocation-free: one bounded iterative DFS, elements only.
 * Ancestor resolution for descendant compounds walks parent pointers
 * (elements only, matching the walker's css_dom_ancestor adapter). */
static int qs_compound_of(const DomNode *n, const char **tagLower,
                          const char **classAttr, const char **idAttr)
{
    *tagLower = n->tag ? n->tag : "";
    *classAttr = dom_get_attr(n, "class");
    *idAttr = dom_get_attr(n, "id");
    return 1;
}

/* nth-element-ancestor adapter for the shared chain walk. i=0 → parent. */
static const void *qs_ancestor(const void *node, int i, const char **tagLower,
                               const char **classAttr, const char **idAttr);

static int qs_matches(const CssSimple *parts, const unsigned char *combs,
                      int np, const DomNode *el)
{
    const char *tag = NULL;
    const char *cls = NULL;
    const char *id = NULL;
    qs_compound_of(el, &tag, &cls, &id);
    /* One shared ancestor walk with the stylesheet engine
     * (css_chain_matches) — QS and CSS can never disagree on
     * descendant/child semantics. */
    CssDom qsDom;
    dom_css_dom(&qsDom, qs_ancestor);
    return css_chain_matches(parts, combs, np, tag, cls, id, &qsDom, el);
}

/* nth-element-ancestor adapter for the shared chain walk. i=0 → parent. */
static const void *qs_ancestor(const void *node, int i, const char **tagLower,
                               const char **classAttr, const char **idAttr)
{
    const DomNode *n = (const DomNode *)node;
    if (!n)
    {
        return NULL;
    }
    for (int d = 0; d <= i; d++)
    {
        n = n->parent;
        if (!n || n->kind != DOM_ELEMENT)
        {
            if (!n)
            {
                return NULL;
            }
            d--; /* non-element ancestor: don't count it */
        }
    }
    qs_compound_of(n, tagLower, classAttr, idAttr);
    return n;
}

typedef struct
{
    const CssSimple *parts;
    const unsigned char *combs; /* combinator before each compound */
    int np;
    int (*emit)(DomNode *el, void *ud);
    void *ud;
    int count;
    DomNode *found; /* first match (qs_first mode) */
    int stop; /* set by emit() < 0 (qs_first) */
} QsCtx;

/* TEMP INSTRUMENTATION (Wikipedia stall hunt — REMOVE after diagnosis). */
unsigned long g_qsCalls = 0;
unsigned long g_qsVisits = 0;
unsigned long g_qsLogged = 0;

void dom_log_qs_stats(void)
{
    logger_log("[qs-stats] calls=%lu visits=%lu", g_qsCalls, g_qsVisits);
}

static void qs_walk(QsCtx *q, DomNode *n)
{    /* Bounded iterative DFS with an explicit stack (device stack rule). */
    typedef struct
    {
        DomNode *node;
        int nextChild;
    } QsFrame;
    QsFrame stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top].node = n;
    stack[top].nextChild = 0;
    top++;
    while (top > 0 && !q->stop)
    {
        QsFrame *f = &stack[top - 1];
        if (f->nextChild == 0 && f->node->kind == DOM_ELEMENT)
        {
            g_qsVisits++; /* TEMP stall stats */
            if (qs_matches(q->parts, q->combs, q->np, f->node))
            {
                q->count++;
                if (!q->found)
                {
                    q->found = f->node;
                }
                if (q->emit && q->emit(f->node, q->ud) < 0)
                {
                    q->stop = 1;
                    break;
                }
                if (!q->emit)
                {
                    /* qs_first mode: found it, done. */
                    q->stop = 1;
                    break;
                }
            }
        }
        if (f->nextChild < f->node->childCount &&
            top < DOM_SEARCH_MAX_DEPTH)
        {
            DomNode *c = f->node->children[f->nextChild];
            f->nextChild++;
            stack[top].node = c;
            stack[top].nextChild = 0;
            top++;
        }
        else
        {
            top--;
        }
    }
}

int dom_query_selector_all(const DomResult *dom, const DomNode *scope,
                           const char *sel, char *scratch, size_t scratchSize,
                           int (*emit)(DomNode *el, void *ud), void *ud)
{
    g_qsCalls++; /* TEMP stall stats (globals below, near qs_walk) */
    if (g_qsLogged < 12)
    {
        g_qsLogged++;
        logger_log("[qs] call %lu: %.120s", g_qsCalls, sel ? sel : "?");
    }
    if (!dom || !scope || !sel || !scratch || scratchSize == 0 || !emit)
    {
        return -1;
    }
    CssSimple parts[CSS_QS_MAX_COMPOUNDS];
    unsigned char combs[CSS_QS_MAX_COMPOUNDS];
    int np = css_parse_selector(sel, scratch, scratchSize, parts, combs);
    if (np <= 0)
    {
        return -1; /* unusable selector → null result, never an exception */
    }
    QsCtx q;
    q.parts = parts;
    q.combs = combs;
    q.np = np;
    q.emit = emit;
    q.ud = ud;
    q.count = 0;
    q.stop = 0;
    q.found = NULL; /* MUST init: the walk only assigns when still NULL */
    qs_walk(&q, (DomNode *)scope);
    return q.count;
}

DomNode *dom_query_selector_first(const DomResult *dom, const DomNode *scope,
                                  const char *sel, char *scratch,
                                  size_t scratchSize)
{
    if (!dom || !scope || !sel || !scratch || scratchSize == 0)
    {
        return NULL;
    }
    CssSimple parts[CSS_QS_MAX_COMPOUNDS];
    unsigned char combs[CSS_QS_MAX_COMPOUNDS];
    int np = css_parse_selector(sel, scratch, scratchSize, parts, combs);
    if (np <= 0)
    {
        return NULL;
    }
    QsCtx q;
    q.parts = parts;
    q.combs = combs;
    q.np = np;
    q.emit = NULL; /* NULL emit = stop-at-first mode */
    q.ud = NULL;
    q.count = 0;
    q.stop = 0;
    q.found = NULL; /* MUST init: the walk only assigns when still NULL */
    qs_walk(&q, (DomNode *)scope);
    return q.count > 0 ? q.found : NULL;
}

/* Iterative free (explicit stack). The previous recursive version recursed
 * once per DOM depth; a deep page could overflow the game-task stack during
 * teardown on device (same class of failure as the P18 watchdog crash). */
static void free_node_recursive(DomNode *root)
{
    if (!root)
    {
        return;
    }
    typedef struct
    {
        DomNode *node;
        int nextChild;
    } FreFrame;
    int cap = 64;
    int top = 0;
    FreFrame *st = (FreFrame *)PLUTO_MALLOC(sizeof(FreFrame) * (size_t)cap);
    if (!st)
    {
        return; /* nothing we can do; allocator is out */
    }
    st[top].node = root;
    st[top].nextChild = 0;
    while (top >= 0)
    {
        DomNode *n = st[top].node;
        if (st[top].nextChild < n->childCount)
        {
            DomNode *c = n->children[st[top].nextChild];
            st[top].nextChild++;
            if (c)
            {
                if (top + 1 >= cap)
                {
                    cap *= 2;
                    FreFrame *grown = (FreFrame *)PLUTO_MALLOC(sizeof(FreFrame) * (size_t)cap);
                    if (!grown)
                    {
                        PLUTO_FREE(st);
                        return;
                    }
                    memcpy(grown, st, sizeof(FreFrame) * (size_t)top);
                    PLUTO_FREE(st);
                    st = grown;
                }
                top++;
                st[top].node = c;
                st[top].nextChild = 0;
            }
        }
        else
        {
            if (n->children)
            {
                PLUTO_FREE(n->children);
            }
            if (n->attrs)
            {
                PLUTO_FREE(n->attrs);
            }
            PLUTO_FREE(n);
            top--;
        }
    }
    PLUTO_FREE(st);
}

void dom_free_result(DomResult *res)
{
    if (!res)
    {
        return;
    }
    free_node_recursive(res->root);
    Arena *arena = (Arena *)res->_arena;
    if (arena)
    {
        ArenaChunk *c = arena->head;
        while (c)
        {
            ArenaChunk *nx = c->next;
            PLUTO_FREE(c);
            c = nx;
        }
        PLUTO_FREE(arena);
    }
    memset(res, 0, sizeof(*res));
}
