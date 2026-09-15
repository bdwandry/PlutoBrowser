/*
 * PlutoBrowser — dom.c
 * HTML tree builder (port of Source/html/dom.lua). See dom.h for the
 * preserved rule set. The Lua reference's helpers (closeOpenP, popToTag,
 * prepareListItem, prepareDtDd, prepareRow, prepareCell, prepareOption) are
 * reproduced stack-index-for-index so stray-element edge cases behave
 * identically.
 */
#include "dom.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "core/logger.h"

PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

#define MAX_NODES 6000

#define PLUTO_REALLOC(p, n) pluto_pd()->system->realloc((p), (n))

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
    return strcmp(tag, "template") == 0 || strcmp(tag, "head") == 0 ||
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
        "address", "article", "aside", "blockquote", "caption", "center",
        "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
        "h5", "h6", "header", "hgroup", "hr", "li", "listing", "main",
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

DomNode *dom_node_by_id(const DomResult *dom, int id)
{
    if (!dom || !dom->root || id <= 0)
    {
        return NULL;
    }
    /* Iterative search (deep pages must not recurse). */
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
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

int dom_append_child(DomResult *dom, DomNode *parent, DomNode *child)
{
    (void)dom;
    if (!parent || !child || parent == child)
    {
        return -1;
    }
    if (node_append_child(parent, child) != 0)
    {
        return -1;
    }
    child->parent = parent;
    return 0;
}

int dom_remove_child(DomNode *parent, DomNode *child)
{
    if (!parent || !child)
    {
        return -1;
    }
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
