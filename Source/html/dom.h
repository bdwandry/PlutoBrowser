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

/* Depth cap for the iterative id lookup (dom_node_by_id). Real-world pages
 * nest far shallower; the builder itself has no depth limit, so lookups on
 * absurd trees simply fail to find the node. */
#define DOM_SEARCH_MAX_DEPTH 64

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

/* Runtime DOM-node id: assigned lazily by the JS bridge (node_id_init) so
 * script-visible document.getElementById("_dN") targets survive tree edits.
 * 0 = unassigned. Not part of the parsed HTML. */
struct DomNode
{
    DomKind kind;
    int nodeId;     /* runtime handle (see node_id_init); 0 = none */
    DomNode *parent; /* set by the builder + dom_append_child; NULL = root/detached */
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
    int nextNodeId; /* runtime node-id counter (jsbridge/dom mutation) */
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

/* ── Runtime DOM mutation API (JavaScript bridge) ─────────────────────────
 * All allocations come from the DomResult's string arena (or the existing
 * heap node/attr arrays), so a mutated page frees exactly like an unmutated
 * one via dom_free_result. Nodes are NOT freed on removal: a detached
 * subtree stays allocated until the document dies (JS may still hold a
 * reference); node counts are bounded by MAX_NODES at build time. */

/* Assign (or return) the node's runtime id. ids are unique per DomResult,
 * monotonic, and never reused. Returns 0 on allocation failure. */
int dom_node_id(const DomResult *dom, DomNode *node);

/* Find a node by runtime id (any kind). NULL when absent. */
DomNode *dom_node_by_id(const DomResult *dom, int id);

/* Create an element node (tag is lowercased-arena). NULL on alloc failure. */
DomNode *dom_create_element(DomResult *dom, const char *tag);

/* Create a text node. NULL on alloc failure. */
DomNode *dom_create_text(DomResult *dom, const char *text);

/* Append `child` to `parent`'s children. Returns 0 ok, -1 alloc failure or
 * bad args (child must not be an ancestor of parent — not checked). */
int dom_append_child(DomResult *dom, DomNode *parent, DomNode *child);

/* Remove `child` from `parent`'s children array (child stays allocated).
 * Returns 0 ok, -1 not found. */
int dom_remove_child(DomNode *parent, DomNode *child);

/* Replace all of `el`'s text-node children with one text node (child
 * elements are kept — innerText semantics keep sub-element markup). Use
 * dom_set_text_all to also drop child elements. Returns 0 ok, -1 alloc. */
int dom_set_text(DomResult *dom, DomNode *el, const char *text);

/* Set attribute `key` to `value` (replaces in place, else appends).
 * Empty value stores "" (still present). Returns 0 ok, -1 alloc. */
int dom_set_attr(DomResult *dom, DomNode *el, const char *key, const char *value);

/* Remove attribute `key`. Returns 0 removed, -1 absent. */
int dom_remove_attr(DomNode *el, const char *key);

/* First element child (DOM_SKIP handled by the builder already). */
DomNode *dom_first_element_child(const DomNode *node);

/* Next element sibling (children after `node` in its parent). */
DomNode *dom_next_element_sibling(const DomNode *node);

/* Release all storage owned by a DomResult (tree, attrs, arena). */
void dom_free_result(DomResult *res);

#endif /* PLUTO_DOM_H */
