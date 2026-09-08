/*
 * PlutoBrowser — dom.h
 * HTML tree builder (port of Source/html/dom.lua).
 *
 * Converts the flat token stream from the tokenizer into a lightweight DOM
 * tree with the Lua reference's simplified WHATWG rules preserved exactly:
 *   - VOID elements never get children/closing tags.
 *   - SKIP_SUBTREE (template/head/selectedcontent) drops the whole subtree
 *     until the matching close tag, with the reference's 500-token safety
 *     valve quirk preserved (reset WITHOUT consuming the token).
 *   - implied end tags: p (before blocks), li, dt/dd, tr/row-sections,
 *     td/th, option; stray elements dropped per the reference helpers.
 *   - nested <a> closes the outer anchor.
 *   - self-closing tags are appended but never pushed on the stack.
 *   - 6000-node cap: append() refuses, loop breaks on the cap, diag recorded.
 *
 * C ownership: the builder uses an internal string arena for tag names and
 * text; node/children arrays are heap (pluto_free via dom_free_tree).
 */
#ifndef PLUTO_DOM_H
#define PLUTO_DOM_H

#include <stddef.h>

#include "html/tokenizer.h"

typedef enum
{
    DOM_ELEMENT = 0,
    DOM_TEXT
} DomKind;

typedef struct DomNode DomNode;

typedef struct
{
    char *key;   /* arena-owned */
    char *value; /* arena-owned decoded string, "", or PLUTO_TOK_ATTR_TRUE */
} DomAttr;

struct DomNode
{
    DomKind kind;
    /* element */
    char *tag;      /* arena-owned; "#root" for the root */
    DomAttr *attrs; /* heap array (pluto_free) */
    int attrCount;
    DomNode **children; /* heap array (pluto_free) */
    int childCount;
    int childCap;
    /* text */
    char *text; /* arena-owned */
};

typedef struct
{
    DomNode *root;
    /* Diagnostics (Lua root._diag) */
    int textNodes;
    int elemNodes;
    int tokensProcessed;
    int maxNodesHit;
    int skippedDepth;
    void *_arena; /* internal string arena */
} DomResult;

/* Build a DOM tree from tokenized input (tokenizer_tokenize output).
 * Returns 0 ok, -1 alloc failure. Consume with dom_free_result. */
int dom_build(const TokenizeResult *tokens, DomResult *out);

/* Convenience: tokenize + build in one step. */
int dom_build_from_html(const char *html, DomResult *out);

/* Find the first child element with the given tag (NULL if none). */
const DomNode *dom_find_child(const DomNode *node, const char *tag);

/* Look up an attribute value; returns "" for boolean attributes present,
 * NULL when the attribute does not exist. */
const char *dom_get_attr(const DomNode *node, const char *key);

/* Release all storage owned by a DomResult (tree, attrs, arena). */
void dom_free_result(DomResult *res);

#endif /* PLUTO_DOM_H */
