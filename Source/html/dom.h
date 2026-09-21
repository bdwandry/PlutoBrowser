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

#include "core/pluto_mem.h"
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

/* ── SW8: arena + paging support (implemented in dom.c / pluto_page.c) ──
 * The paging module allocates restored strings from the doc's OWN arena so
 * dom_free_result's wholesale arena free stays exact; the facade below
 * makes the internal allocator callable from Source/core/pluto_page.c. */

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
    /* SW8 disk-backed DOM paging: non-zero when this node is a STUB whose
     * subtree lives in the persistent store under this key. Materialized
     * transparently by dom_touch() (called at the bridge/walker touch
     * points). 0 = fully RAM-resident. */
    unsigned long pagedKey;
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
    /* SW8 paging: stub shells left by dom_page_out stay LINKED in the live
     * tree (empty leaves carrying pagedKey), so the normal tree free pass
     * releases them — no separate bookkeeping needed. */
} DomResult;

/* Duplicate (s,len) into the arena, NUL-terminated. NULL on exhaustion. */
char *dom_arena_dup(DomResult *dom, const char *s, int len);

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

/* Insert `child` into `parent`'s children BEFORE existing child `ref`.
 * ref == NULL appends (insertBefore's null-ref semantics). If `child` is
 * already a child of `parent` it is MOVED (removed then re-inserted),
 * matching DOM insertBefore. Returns 0 ok, -1 bad args / ref not a child. */
int dom_insert_before(DomResult *dom, DomNode *parent, DomNode *child,
                      const DomNode *ref);

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

/* ── SW5: class attribute as a token list (classList, O4) ─────────────────
 * All ops are 0 ok / -1 on alloc failure (or bad args). Mutations rewrite
 * the element's class attribute through the arena (same ownership as any
 * attr value); a class list that becomes empty removes the attribute
 * (browsers keep class="" on set; removal matches the CSS matcher's
 * NULL-class fast path). Token compare is case-sensitive (CSS semantics). */
int dom_class_has(const DomNode *el, const char *token);
int dom_class_add(DomResult *dom, DomNode *el, const char *token);
int dom_class_remove(DomResult *dom, DomNode *el, const char *token);
/* 1 added, 0 removed, -1 alloc failure. */
int dom_class_toggle(DomResult *dom, DomNode *el, const char *token);

/* ── SW5: querySelector machinery (O3) ────────────────────────────────────
 * Selector strings come from the JS caller; parsing (compound grammar:
 * type/.class/#id/*, whitespace = descendant) is css_parse_selector with a
 * CALLER-PROVIDED scratch buffer (the parsed compounds point into it — one
 * 256B buffer per call site, stack-safe on device). Matching walks the
 * subtree under `scope` (inclusive) iteratively; elements only. Both take
 * an optional out-callback: qs_all invokes it per match (with the caller's
 * usize context) and returns the total match count (a callback error stops
 * the walk early); qs_first stops at the first hit. Returns -1 for an
 * unusable selector (callers surface null, never throw). */
#define DOM_QS_SCRATCH 256
int dom_query_selector_all(const DomResult *dom, const DomNode *scope,
                           const char *sel, char *scratch, size_t scratchSize,
                           int (*emit)(DomNode *el, void *ud), void *ud);
DomNode *dom_query_selector_first(const DomResult *dom, const DomNode *scope,
                                  const char *sel, char *scratch,
                                  size_t scratchSize);



/* Release all storage owned by a DomResult (tree, attrs, arena). */
void dom_free_result(DomResult *res);

/* ── SW8: disk-backed DOM paging (implemented in core/pluto_page.c) ──────── */

/* Serialize the subtree under `root` (root included) into the persistent
 * spill store under key FNV-1a(baseUrl "/" rootId), stamp the stub marker
 * on `root`, and free the subtree EXCEPT root (root stays linked in its
 * parent's children[] carrying the marker). Returns 0 ok, -1 failure (the
 * tree is untouched — page-out is all-or-nothing). */
int dom_page_out(DomResult *dom, DomNode *root, const char *baseUrl);

/* Materialize a stub node's subtree from the store: bulk-read once, decode
 * into the doc's arena, graft the restored children INTO the stub node
 * (in-place: node identity — pointer, nodeId, parent — is preserved, so
 * callers may hold node pointers across a touch). The store entry itself
 * is left in place: the spill family has no per-key delete; the quota
 * sweep (or invalidate_all on navigation) reclaims it, like SW4's
 * bytecode cache. No-op when the node is RAM-resident. On failure the
 * stub survives with its marker cleared (degrades to an empty leaf). */
void dom_touch(DomResult *dom, DomNode *node);

/* 1 when the node is a paged-out stub. */
int dom_is_paged(const DomNode *node);

/* Free a partially-built subtree (pluto_page.c internal failure path;
 * exposed for tests). Strings live in the doc's arena — they die with it. */
void dom_page_free_subtree(DomNode *n);

#endif /* PLUTO_DOM_H */
