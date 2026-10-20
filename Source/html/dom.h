/*
 * PlutoBrowser — dom.h
 * HTML tree builder (port of Source/html/dom.lua).
 *
 * Converts the flat token stream from the tokenizer into a lightweight DOM
 * tree with the Lua reference's simplified WHATWG rules preserved exactly:
 *   - VOID elements never get children/closing tags.
 *   - SKIP_SUBTREE (selectedcontent today) drops the whole subtree until
 *     the matching close tag, with the reference's 500-token safety
 *     valve quirk preserved (reset WITHOUT consuming the token).
 *     <template> is handled separately since API-P6: the element stays
 *     in the tree (id/querySelector reach) while its children parse
 *     into a DETACHED content fragment (browser-faithful inert content;
 *     see the templateContent field + dom.c's builder branch).
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
#include "html/css.h"

/* Depth cap for the iterative id lookup (dom_node_by_id). Real-world pages
 * nest far shallower; the builder itself has no depth limit, so lookups on
 * absurd trees simply fail to find the node. */
#define DOM_SEARCH_MAX_DEPTH 64

typedef enum
{
    DOM_ELEMENT = 0,
    DOM_TEXT,
    DOM_COMMENT
} DomKind;

typedef struct DomNode DomNode;

/* Iterative depth-capped scan frame (the house shape: parent + child
 * index held instead of pushing every child at once — the old push-all
 * shape's peak was top + WIDTH at the widest node, so a body with 65+
 * element children truncated the whole scan at DOM_SEARCH_MAX_DEPTH and
 * silently hid every later id from getElementById). Kept beside the cap
 * so every scan picks the same rules. */
typedef struct
{
    DomNode *node;
    int nextChild;
} DomScanFrame;

typedef struct
{
    char *key;   /* arena-owned */
    char *value; /* arena-owned decoded string, "", or PLUTO_TOK_ATTR_TRUE */
} DomAttr;

/* DOM_COMMENT nodes keep the RAW comment data in `text` (same field the
 * tokenizer stores; comments are not entity-decoded). React's SSR
 * hydration matcher walks these markers ("$!", "$?", "$~", "&", "F!",
 * "F", "/$", "/&") to bind Suspense boundaries. */

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
    /* Shadow-DOM association (NR engine only; react's route announcer does
     * host.attachShadow({mode:"open"}).appendChild(div)). The shadow root
     * is a REAL element node kept OUT of the render tree (parent NULL →
     * invisible to layout, tag scans and serializer output) but owned by
     * full Element machinery, so appendChild/childNodes/getRootNode work
     * on shadow content. Zero for every parsed tree node and every other
     * detached fragment, so frozen engines are untouched. */
    DomNode *shadowRoot; /* on the HOST element: its open shadow root */
    DomNode *shadowHost; /* on the SHADOW ROOT node: owning element */
    /* API-P6: <template> content — a DETACHED fragment node (parent
     * NULL, tag "#template-content") owned by the template element.
     * Browser-faithful inert content: the parsed children never reach
     * the render tree, the id lookup or any parent-connected walk, while
     * the element itself stays reachable (id/querySelector like any
     * browser). NULL until a parsed template fills it in the builder or
     * a created template materializes it lazily (dom_template_content).
     * Freed with the document: dom_free_result walks these owned
     * detached roots first (this covers the shadow roots too). */
    DomNode *templateContent; /* on the TEMPLATE element: its fragment */
};

/* Phase 10 dedupe-map cap: 24 completed (element, animation-name) pairs per
 * document. Beyond the cap the pair records nothing — that run may re-fire
 * on a re-walk (deterministic, additive, bounded; real pages land far under
 * the cap and the residual is documented in the scorecard). */
#define DOM_ANIMFIRED_MAX 24

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

    /* Phase 10: animation-completion dedupe map. The walker records one
     * (node, name) pair per COMPLETED declared run, document-lifetime:
     * a later walk of the SAME element with the SAME animation-name
     * must not re-engage (the browser completes each run once until the
     * declaration changes). Names live in the doc's own string arena
     * (same lifetime as the nodes); nodes never free before the
     * document — entries are plain pointers, safe across re-walks. */
    struct
    {
        void *node;   /* the element that completed the run */
        char *name;   /* arena copy (NUL-terminated, ≤24 chars) */
        int nameLen;
    } animFired[DOM_ANIMFIRED_MAX];
    int animFiredCount;
} DomResult;

/* Duplicate (s,len) into the arena, NUL-terminated. NULL on exhaustion. */
char *dom_arena_dup(DomResult *dom, const char *s, int len);

/* ── Phase 10: record/consult one COMPLETED animation run ─────────────────
 * Returns 1 when this (node, name) pair was already recorded (dedupe:
 * the walker must NOT engage again); 0 when newly recorded. The name
 * copies into the doc's own string arena (truncated at 24 chars). */
int dom_anim_end_report(DomResult *dom, const void *node,
                        const char *name, int nameLen);

/* Build a DOM tree from tokenized input (tokenizer_tokenize output).
 * Returns 0 ok, -1 alloc failure. Consume with dom_free_result. */
int dom_build(const TokenizeResult *tokens, DomResult *out);

/* Convenience: tokenize + build in one step. */
int dom_build_from_html(const char *html, DomResult *out);

/* Find the first child element with the given tag (NULL if none). */
const DomNode *dom_find_child(const DomNode *node, const char *tag);

#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
/* Harness-only hydration debug: dump root-less fragment tops among the
 * nodes created via dom_create_element (see dom.c). */
void dom_created_dump(void);
#endif

/* Look up an attribute value; returns "" for boolean attributes present,
 * NULL when the attribute does not exist. */
const char *dom_get_attr(const DomNode *node, const char *key);

/* CSS-engine adapter: length-delimited, case-insensitive attribute lookup
 * for CssDom.attr. DOM keys are stored lowercased (tokenizer), so the name
 * is lowered into a bounded stack buffer first; over-long names never
 * match. Returns the value ("" ok) or NULL when absent. */
const char *dom_css_attr(const void *node, const char *name, size_t namelen);

/* CSS-engine adapter for structural pseudo-classes: the nearest ELEMENT
 * sibling in direction dir (-1 = previous, +1 = next), or NULL when none.
 * Text/comment siblings never count (CSS element-sibling semantics). */
const void *dom_css_sibling(const void *node, int dir);

/* Phase 1 sibling-COMBINATOR adapter: same nearest-ELEMENT walk as
 * dom_css_sibling, filling *tagLower / *classAttr / *idAttr for that
 * sibling (strings valid until the next call). Backs '+' / '~' in
 * css_chain_matches. */
const void *dom_css_sibling_info(const void *node, int dir,
                                 const char **tagLower,
                                 const char **classAttr,
                                 const char **idAttr);

/* Fill a CssDom with the shared hooks (attribute + sibling) plus the
 * caller's ancestor hook — one place so the three never drift. */
void dom_css_dom(CssDom *d,
                 const void *(*ancestor)(const void *node, int i,
                                         const char **tagLower,
                                         const char **classAttr,
                                         const char **idAttr));

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

/* Zero-child comment node (raw, non-decoded data). React hydration
 * markers and document.createComment ride on these. */
DomNode *dom_create_comment(DomResult *dom, const char *data);

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

/* Serialize an element's children as HTML markup (innerHTML parity; see
 * dom.c for the writer rules). Returns 0 + heap string (caller frees via
 * the funnel: JFree / pluto_mem_realloc(p, 0)) or -1 on bad args,
 * -2 alloc failure, -3 runaway depth. `len` (nullable) receives the
 * markup byte length. */
int dom_serialize_children_html(const DomNode *el, char **out, size_t *len);

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
 * type .class #id star attr-ops; combinators: whitespace = descendant,
 * '>' child, '+' adjacent sibling, '~' general sibling) is
 * css_parse_selector with a
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

/* ── API-P6: Element.closest — the element itself first, then element
 * ancestors; first match wins, NULL = no match. Same parse + matcher
 * as dom_query_selector_first (one scratch rule, same shapes). */
DomNode *dom_closest(const DomResult *dom, DomNode *start, const char *sel,
                     char *scratch, size_t scratchSize);

/* ── API-P6: <template>.content — the element's detached content
 * fragment (materialized empty on first access for created templates).
 * NULL for a non-template element or on allocation failure. */
DomNode *dom_template_content(DomResult *dom, DomNode *el);

/* ── API-P6: document.getElementById road — ONE shared O(depth) scan the
 * engine adapters all call (the push-all-children copies that predate this
 * aborted their whole walk once a node held 65+ element children — a wide
 * body silently hid every later id from getElementById). Document order,
 * elements only, depth capped at DOM_SEARCH_MAX_DEPTH; width never
 * truncates. Falls back to the runtime _dN node-id road. */
DomNode *dom_find_by_id(DomResult *dom, const char *id);

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

/* TEMP INSTRUMENTATION (Wikipedia stall hunt — REMOVE after diagnosis). */
void dom_log_qs_stats(void);

#endif /* PLUTO_DOM_H */
