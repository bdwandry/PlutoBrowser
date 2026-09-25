/*
 * pluto_page — SW8 DISK-BACKED DOM PAGING ("swap" for the layer we own).
 *
 * WHY: the JS engines' heaps cannot be swapped (C pointer dereferences are
 * invisible to software; rewriting every -> in Source/js is forbidden). The
 * DOM is OUR data structure: DomNode trees (html/dom.h) owned by DomResult.
 * SW8 pages DISK-DETACHED subtrees of the live DOM to disk and materializes
 * them transparently at the touch points:
 *
 *   - jsbridge id lookups: dom_node_by_id() (every id-based JS access), and
 *   - document.c walker: descend-by-nodeId (render/rewalk of paged regions).
 *
 * Both call dom_touch(); when a node is a stub, dom_touch materializes its
 * subtree from the spill store BEFORE the caller uses it — engines and the
 * walker only ever see in-RAM nodes.
 *
 * HOW (fault model):
 *   - dom_page_out(dom, root, baseUrl): serialize the subtree under `root`
 *     (root included) to the persistent spill store (SW4 bc_<id>_<key>.bin
 *     family, key = FNV-1a32(baseUrl "/" rootId)), stamp root->pagedKey,
 *     then free the subtree EXCEPT root (root stays: it carries the stub
 *     marker and remains linkable in its parent's children[]).
 *   - dom_touch(dom, node): stub? -> bulk-read the whole file ONCE (the SW6
 *     device lesson: per-field spill reads = open+seek+read+close per field
 *     = thousands of flash transactions = watchdog), decode into the doc's
 *     arena, relink into the live tree, free the stub shell + store entry.
 *
 * Storage: one spill-store file per paged-out subtree — the SAME machinery
 * as SW4's bytecode cache and SW6's snapshots. No engine is touched.
 *
 * Ownership: strings restored here come from the doc's OWN arena
 * (dom_arena_dup), so dom_free_result's wholesale arena free stays exact.
 * Node structs + attr/children arrays are heap (PLUTO_MALLOC/pluto_free),
 * identical to the builder — free_node_recursive needs no changes, and a
 * never-materialized stub is just an empty leaf node in the tree.
 */
#include "pluto_page.h"

#include "core/logger.h"
#include "core/pluto_mem.h"
#include "core/pluto_spill.h"
#include "html/dom.h"
#include "html/tokenizer.h"

#include <stdio.h>
#include <string.h>

/* Funnel allocation (SW1): same convention as pluto_snap.c/document.c. */
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)
#define PLUTO_REALLOC(p, n) pluto_mem_realloc((p), (n))

/* ── format ──────────────────────────────────────────────────────────────── */

#define PAGE_MAGIC_0 'P'
#define PAGE_MAGIC_1 '8'
#define PAGE_MAGIC_2 'P'
#define PAGE_MAGIC_3 'D'
#define PAGE_VERSION 1

#define PAGE_HEADER_BYTES 12 /* magic(4) ver(1) flags(1) rootId(4) reserved(2) */

/* sanity caps (mirror the builder's own limits) */
#define PAGE_MAX_NODES 6000
#define PAGE_MAX_DEPTH 64
#define PAGE_MAX_ATTRS 128
#define PAGE_MAX_ATTR_KEY 128
#define PAGE_MAX_ATTR_VAL 512
#define PAGE_MAX_TEXT (16 * 1024)
#define PAGE_MAX_FILE (512 * 1024)

/* SW8 policy: page out only under RAM pressure, and only worthwhile
 * subtrees. Threshold aligns with the SW3a auto-placement gate (jsext
 * spills a response when headroom < 1.5MB — same "1.5MB of headroom left"
 * boundary, so DOM paging engages exactly when the page body already went
 * to disk). Floor: a 12KB+ subtree buys its keep (file I/O + restore cost
 * vs ~4x that in RAM). MUST stay well below the device budget (6.5MB) or
 * the policy would fire on every render regardless of pressure. */
#define PAGE_HEADROOM_TRIGGER (1536UL * 1024UL)
#define PAGE_SUBTREE_FLOOR_BYTES (12 * 1024)

/* Test/autotest override of the subtree floor (0 = default). */
static long g_pageFloorOverride = 0;
void dom_page_set_subtree_floor(long bytes)
{
    g_pageFloorOverride = bytes;
}

static void dom_page_free_subtree_impl(DomNode *n);


/* ── byte writer (spill-append based, same shape as pluto_snap) ──────────── */

typedef struct
{
    SpillFile h;
    int err;
} PWriter;

static void pw_bytes(PWriter *w, const void *p, size_t n)
{
    if (w->err || n == 0)
    {
        return;
    }
    if (pluto_spill_write(w->h, p, n) != 0)
    {
        w->err = 1;
    }
}

static void pw_i32(PWriter *w, int v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
    pw_bytes(w, b, 4);
}

static void pw_u16(PWriter *w, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    pw_bytes(w, b, 2);
}

static void pw_str(PWriter *w, const char *s)
{
    if (w->err)
    {
        return;
    }
    size_t len = s ? strlen(s) : 0;
    if (len > PAGE_MAX_TEXT)
    {
        w->err = 1;
        return;
    }
    pw_i32(w, (int)len);
    if (len)
    {
        pw_bytes(w, s, len);
    }
}

/* ── byte reader (in-RAM image; bulk-read once at open) ──────────────────── */

typedef struct
{
    const unsigned char *buf;
    long off;
    long size;
    int err;
} PReader;

static long pr_bytes(PReader *r, void *dst, long n)
{
    if (r->err || n <= 0)
    {
        return n > 0 ? 0 : n;
    }
    if (!r->buf || r->off + n > r->size)
    {
        r->err = 1;
        return 0;
    }
    memcpy(dst, r->buf + r->off, (size_t)n);
    r->off += n;
    return n;
}

static int pr_i32(PReader *r)
{
    unsigned char b[4];
    if (pr_bytes(r, b, 4) != 4)
    {
        return 0;
    }
    return (int)((unsigned)b[0] | ((unsigned)b[1] << 8) |
                 ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24));
}

static unsigned pr_u16(PReader *r)
{
    unsigned char b[2];
    if (pr_bytes(r, b, 2) != 2)
    {
        return 0;
    }
    return (unsigned)(b[0] | (b[1] << 8));
}

/* Zero-copy string read: validates the length and returns a POINTER into
 * the RAM image (NUL bytes are written into the image, which we own for
 * the decode). No stack staging: the device game-task stack is 61KB and
 * per-level 16KB text buffers overflowed it (watchdog crash), while the
 * sim's 8MB stack masked the bug. Returns 0 ok, -1 corrupt. */
static int pr_str_ref(PReader *r, const char **out, int *outLen)
{
    if (r->err)
    {
        return -1;
    }
    int len = pr_i32(r);
    if (r->err || len < 0 || len > PAGE_MAX_TEXT ||
        (long)(r->off + len) >= r->size)
    {
        /* '>= size' also rejects strings flush against EOF: our writer
         * always emits further fields after a string, so this is a
         * corruption signal — and it keeps the NUL clobber in-bounds. */
        r->err = 1;
        return -1;
    }
    /* NO NUL clobber: r->off+len is the next field's first byte — writing
     * there corrupts the stream (the u16 flag/childCount tests caught it).
     * dom_arena_dup copies exactly len bytes and terminates its own copy,
     * so the image never needs in-place termination. */
    *out = (const char *)r->buf + r->off;
    *outLen = len;
    r->off += len;
    return 0;
}

/* ── key derivation ──────────────────────────────────────────────────────── */

static unsigned long page_key(const char *baseUrl, int rootId)
{
    unsigned long h = 2166136261UL;
    const char *s = baseUrl ? baseUrl : "?";
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    h ^= (unsigned char)'/';
    h *= 16777619UL;
    h ^= (unsigned)(rootId & 0xFF);
    h ^= ((unsigned)rootId >> 8) << 9;
    h *= 16777619UL;
    return h ? h : 1;
}

/* ── serialize ─────────────────────────────────────────────────────────────
 * Iterative pre-order writer (explicit frame stack, heap-allocated). The
 * recursive version consumed one C frame per DOM depth; paging runs under
 * memory pressure when trees are at their biggest, so depth unbounded by a
 * frame chain is exactly the safety property SW8 needs. The wire format is
 * UNCHANGED (same field order: kind, id, tag/attrs/text, childCount, then
 * each child) — snapshots paged out by an older build still load. */

typedef struct
{
    const DomNode *node;
    int nextChild;
} PageWFrame;

static void w_node(PWriter *w, const DomNode *root)
{
    if (w->err || !root)
    {
        return;
    }
    int cap = 64;
    int top = 0;
    PageWFrame *st = (PageWFrame *)PLUTO_MALLOC(sizeof(PageWFrame) * (size_t)cap);
    if (!st)
    {
        w->err = 1;
        return;
    }
    st[top].node = root;
    st[top].nextChild = 0;
    top++;
    while (top > 0 && !w->err)
    {
        PageWFrame *f = &st[top - 1];
        const DomNode *n = f->node;
        if (f->nextChild == 0)
        {
            /* first visit: emit the node header + own payload */
            pw_u16(w, (unsigned)(n->kind == DOM_TEXT ? 1 : 0));
            pw_i32(w, n->nodeId);
            if (n->kind == DOM_ELEMENT)
            {
                pw_str(w, n->tag ? n->tag : "");
                int attrCount = n->attrCount;
                if (attrCount > PAGE_MAX_ATTRS)
                {
                    attrCount = PAGE_MAX_ATTRS;
                }
                pw_u16(w, (unsigned)attrCount);
                for (int i = 0; i < attrCount; i++)
                {
                    pw_str(w, n->attrs[i].key);
                    /* value may be the PLUTO_TOK_ATTR_TRUE sentinel (boolean
                     * attr): encode a flag + string so strlen never
                     * dereferences it */
                    if (n->attrs[i].value == PLUTO_TOK_ATTR_TRUE)
                    {
                        pw_u16(w, 1);
                        pw_str(w, "");
                    }
                    else
                    {
                        pw_u16(w, 0);
                        pw_str(w, n->attrs[i].value);
                    }
                }
            }
            else
            {
                pw_str(w, n->text ? n->text : "");
            }
            pw_u16(w, (unsigned)n->childCount);
        }
        if (f->nextChild < n->childCount)
        {
            const DomNode *c = n->children[f->nextChild++];
            if (!c)
            {
                continue; /* defensive: builder never stores NULL children */
            }
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                PageWFrame *grown = (PageWFrame *)PLUTO_REALLOC(
                    st, sizeof(PageWFrame) * (size_t)ncap);
                if (!grown)
                {
                    w->err = 1;
                    break;
                }
                st = grown;
                cap = ncap;
                f = &st[top - 1]; /* realloc may have moved the array */
            }
            st[top].node = c;
            st[top].nextChild = 0;
            top++;
        }
        else
        {
            top--;
        }
    }
    PLUTO_FREE(st);
}

/* ── restore (into the doc's arena) ───────────────────────────────────────
 * Iterative reader (explicit frame stack, heap-allocated). The recursive
 * version consumed one C frame per stored DOM depth — the restore path
 * runs on the game-task stack mid-render, exactly where the overflow
 * class this project keeps hitting bites. The wire format is UNCHANGED
 * (identical field order to w_node), so pages paged out by an older
 * build still materialize. Returns the restored ROOT node (children
 * linked, parent pointers set), NULL on any error (r->err set).
 */

/* Decode one node's own payload (kind/id, tag+attrs or text, childCount)
 * from the stream. The children array is allocated but LEFT EMPTY; the
 * driving loop links children as it decodes them. Returns NULL on error. */
static DomNode *r_node_head(PReader *r, DomResult *dom, DomNode *parent,
                            int *count)
{
    if (r->err || *count >= PAGE_MAX_NODES)
    {
        r->err = 1;
        return NULL;
    }
    unsigned kind = pr_u16(r);
    int nodeId = pr_i32(r);
    DomNode *n = (DomNode *)PLUTO_MALLOC(sizeof(DomNode));
    if (!n)
    {
        r->err = 1;
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->kind = (kind == 1) ? DOM_TEXT : DOM_ELEMENT;
    n->nodeId = nodeId;
    n->parent = parent;
    (*count)++;

    if (n->kind == DOM_ELEMENT)
    {
        const char *tag = NULL;
        int tagLen = 0;
        if (pr_str_ref(r, &tag, &tagLen) != 0)
        {
            PLUTO_FREE(n);
            return NULL;
        }
        n->tag = dom_arena_dup(dom, tagLen ? tag : "", tagLen);
        if (!n->tag)
        {
            PLUTO_FREE(n);
            r->err = 1;
            return NULL;
        }
        unsigned attrCount = pr_u16(r);
        if (r->err || attrCount > PAGE_MAX_ATTRS)
        {
            PLUTO_FREE(n);
            r->err = 1;
            return NULL;
        }
        if (attrCount > 0)
        {
            n->attrs = (DomAttr *)PLUTO_MALLOC(sizeof(DomAttr) * attrCount);
            if (!n->attrs)
            {
                PLUTO_FREE(n);
                r->err = 1;
                return NULL;
            }
            for (unsigned i = 0; i < attrCount; i++)
            {
                const char *k = NULL, *v = NULL;
                int kLen = 0, vLen = 0;
                /* wire order matches the writer: key, flag, value */
                if (pr_str_ref(r, &k, &kLen) != 0)
                {
                    PLUTO_FREE(n->attrs);
                    n->attrs = NULL;
                    PLUTO_FREE(n);
                    r->err = 1;
                    return NULL;
                }
                unsigned isTrue = pr_u16(r);
                if (pr_str_ref(r, &v, &vLen) != 0)
                {
                    PLUTO_FREE(n->attrs);
                    n->attrs = NULL;
                    PLUTO_FREE(n);
                    r->err = 1;
                    return NULL;
                }
                if (isTrue == 1)
                {
                    n->attrs[i].key = dom_arena_dup(dom, k, kLen);
                    n->attrs[i].value = (char *)PLUTO_TOK_ATTR_TRUE;
                    if (!n->attrs[i].key)
                    {
                        PLUTO_FREE(n->attrs);
                        n->attrs = NULL;
                        PLUTO_FREE(n);
                        r->err = 1;
                        return NULL;
                    }
                    continue;
                }
                n->attrs[i].key = dom_arena_dup(dom, k, kLen);
                n->attrs[i].value = dom_arena_dup(dom, v, vLen);
                if (!n->attrs[i].key || !n->attrs[i].value)
                {
                    /* strings die with the doc arena; free the arrays */
                    PLUTO_FREE(n->attrs);
                    n->attrs = NULL;
                    PLUTO_FREE(n);
                    r->err = 1;
                    return NULL;
                }
            }
            n->attrCount = (int)attrCount;
        }
    }
    else
    {
        const char *text = NULL;
        int textLen = 0;
        if (pr_str_ref(r, &text, &textLen) != 0)
        {
            PLUTO_FREE(n);
            return NULL;
        }
        n->text = dom_arena_dup(dom, textLen ? text : "", textLen);
        if (!n->text)
        {
            PLUTO_FREE(n);
            r->err = 1;
            return NULL;
        }
    }

    unsigned childCount = pr_u16(r);
    if (r->err || childCount > PAGE_MAX_NODES)
    {
        r->err = 1;
        dom_page_free_subtree_impl(n);
        return NULL;
    }
    if (childCount > 0)
    {
        n->children = (DomNode **)PLUTO_MALLOC(sizeof(DomNode *) * childCount);
        if (!n->children)
        {
            r->err = 1;
            dom_page_free_subtree_impl(n);
            return NULL;
        }
        /* childCap carries the DECLARED count while decoding (childCount
         * counts what has been linked so far); the driving loop fills the
         * array up to childCap. Fully decoded ⇒ childCount == childCap. */
        n->childCap = (int)childCount;
    }
    return n;
}

static DomNode *r_node(PReader *r, DomResult *dom, DomNode *parent, int depth,
                       int *count)
{
    typedef struct
    {
        DomNode *node; /* node whose children are being decoded */
        int nextChild; /* children linked so far */
        int depth;
    } PageRFrame;
    if (r->err || depth > PAGE_MAX_DEPTH || *count >= PAGE_MAX_NODES)
    {
        r->err = 1;
        return NULL;
    }
    int cap = 64;
    int top = 0;
    PageRFrame *st = (PageRFrame *)PLUTO_MALLOC(sizeof(PageRFrame) * (size_t)cap);
    if (!st)
    {
        r->err = 1;
        return NULL;
    }
    DomNode *root = r_node_head(r, dom, parent, count);
    if (!root)
    {
        PLUTO_FREE(st);
        return NULL;
    }
    st[top].node = root;
    st[top].nextChild = 0;
    st[top].depth = depth;
    top++;
    while (top > 0 && !r->err)
    {
        PageRFrame *f = &st[top - 1];
        if (f->nextChild >= f->node->childCap)
        {
            top--; /* all declared children linked */
            continue;
        }
        int childDepth = f->depth + 1;
        if (childDepth > PAGE_MAX_DEPTH || *count >= PAGE_MAX_NODES)
        {
            r->err = 1;
            break;
        }
        DomNode *c = r_node_head(r, dom, f->node, count);
        if (!c)
        {
            break; /* r->err set by r_node_head */
        }
        f->node->children[f->nextChild] = c;
        f->node->childCount = f->nextChild + 1;
        f->nextChild++; /* advance the parent's cursor in BOTH branches */
        if (c->childCap > 0)
        {
            /* c itself declared children: descend (depth-first, wire order) */
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                PageRFrame *grown = (PageRFrame *)PLUTO_REALLOC(
                    st, sizeof(PageRFrame) * (size_t)ncap);
                if (!grown)
                {
                    r->err = 1;
                    break;
                }
                st = grown;
                cap = ncap;
                f = &st[top - 1]; /* realloc may have moved the array */
            }
            st[top].node = c;
            st[top].nextChild = 0;
            st[top].depth = childDepth;
            top++;
        }
    }
    PLUTO_FREE(st);
    if (r->err)
    {
        dom_page_free_subtree_impl(root);
        return NULL;
    }
    return root;
}

/* Free a partially-built subtree (heap structs + arrays; strings live in
 * the doc arena and die with it). Mirrors free_node_recursive ownership.
 * Iterative (explicit stack, heap-allocated): the recursive version spent
 * one C frame per depth on the game-task stack — teardown of a deep tree
 * overflowed it. Frees children first (post-order) so arrays are read
 * before the node struct goes away. */
static void dom_page_free_subtree_impl(DomNode *root)
{
    if (!root)
    {
        return;
    }
    DomNode *stack[64];
    int top = 0;
    stack[top++] = root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children && n->children[i] &&
                top < (int)(sizeof(stack) / sizeof(stack[0])))
            {
                stack[top++] = n->children[i];
            }
        }
        if (n->children)
        {
            PLUTO_FREE(n->children);
        }
        if (n->attrs)
        {
            PLUTO_FREE(n->attrs);
        }
        PLUTO_FREE(n);
    }
}

void dom_page_free_subtree(DomNode *n)
{
    dom_page_free_subtree_impl(n);
}/* ── public API ──────────────────────────────────────────────────────────── */

void dom_touch(DomResult *dom, DomNode *node)
{
    if (!dom || !node || node->pagedKey == 0)
    {
        return; /* RAM-resident (or nothing to do) */
    }
    unsigned long key = node->pagedKey;
    if (!pluto_spill_store_find(key))
    {
        logger_log("[page] materialize failed: no store entry key=%08lx",
                   key);
        node->pagedKey = 0;
        return;
    }
    SpillFile h = pluto_spill_store_open_read(key);
    if (h == PLUTO_SPILL_INVALID)
    {
        logger_log("[page] materialize failed: open key=%08lx", key);
        node->pagedKey = 0;
        return;
    }
    long fsz = pluto_spill_size(h);
    if (fsz < PAGE_HEADER_BYTES || fsz > PAGE_MAX_FILE)
    {
        pluto_spill_store_close(h);
        node->pagedKey = 0;
        return;
    }
    /* The SW6 device lesson: ONE bulk read, decode from RAM. */
    unsigned char *image = (unsigned char *)PLUTO_MALLOC((size_t)fsz);
    if (!image)
    {
        pluto_spill_store_close(h);
        return; /* keep the stub; a later touch may succeed */
    }
    if (pluto_spill_read(h, 0, image, (size_t)fsz) != fsz)
    {
        PLUTO_FREE(image);
        pluto_spill_store_close(h);
        node->pagedKey = 0;
        return;
    }
    pluto_spill_store_close(h);

    PReader r;
    memset(&r, 0, sizeof(r));
    r.buf = image;
    r.size = fsz;
    unsigned char hdr[PAGE_HEADER_BYTES];
    if (pr_bytes(&r, hdr, PAGE_HEADER_BYTES) != PAGE_HEADER_BYTES ||
        hdr[0] != PAGE_MAGIC_0 || hdr[1] != PAGE_MAGIC_1 ||
        hdr[2] != PAGE_MAGIC_2 || hdr[3] != PAGE_MAGIC_3 ||
        hdr[4] != PAGE_VERSION)
    {
        PLUTO_FREE(image);
        node->pagedKey = 0;
        return;
    }
    /* Guard against a key collision: the image must belong to THIS stub. */
    int hdrRootId = (int)((unsigned)hdr[6] | ((unsigned)hdr[7] << 8) |
                          ((unsigned)hdr[8] << 16) | ((unsigned)hdr[9] << 24));
    if (hdrRootId != node->nodeId)
    {
        PLUTO_FREE(image);
        node->pagedKey = 0;
        logger_log("[page] materialize refused: id mismatch key=%08lx", key);
        return;
    }

    int count = 0;
    DomNode *tmp = r_node(&r, dom, node->parent, 0, &count);
    PLUTO_FREE(image);
    if (!tmp || r.err || r.off != r.size)
    {
        if (tmp)
        {
            dom_page_free_subtree_impl(tmp);
        }
        logger_log("[page] materialize failed: corrupt key=%08lx", key);
        node->pagedKey = 0;
        return;
    }

    /* Restore IN PLACE: the stub node itself becomes the restored root
     * (its identity — pointer, nodeId, parent — is preserved; callers may
     * hold node pointers across a touch). Only the children array is
     * grafted back; the stub's own tag/text/attrs never left RAM. tmp's
     * duplicate root strings are arena-owned and die with the doc. */
    PLUTO_FREE(node->children); /* NULL after page-out; harmless anyway */
    node->children = tmp->children;
    node->childCount = tmp->childCount;
    node->childCap = tmp->childCap;
    tmp->children = NULL;
    tmp->childCount = 0;
    tmp->childCap = 0;
    for (int i = 0; i < node->childCount; i++)
    {
        node->children[i]->parent = node; /* they pointed at tmp */
    }
    dom_page_free_subtree_impl(tmp);
    node->pagedKey = 0;
    logger_log("[page] materialized key=%08lx nodes=%d bytes=%ld", key, count,
               fsz);
}

int dom_page_out(DomResult *dom, DomNode *root, const char *baseUrl)
{
    if (!dom || !root || !baseUrl || !baseUrl[0])
    {
        return -1;
    }
    if (root->pagedKey != 0)
    {
        return 0; /* already paged */
    }
    if (!root->nodeId)
    {
        /* stubs are addressed by their runtime id; make sure it has one */
        if (dom_node_id(dom, root) == 0)
        {
            return -1;
        }
    }
    unsigned long key = page_key(baseUrl, root->nodeId);
    PWriter w;
    memset(&w, 0, sizeof(w));
    w.h = pluto_spill_store_open_create(key);
    if (w.h == PLUTO_SPILL_INVALID)
    {
        return -1;
    }
    unsigned char hdr[PAGE_HEADER_BYTES];
    hdr[0] = PAGE_MAGIC_0;
    hdr[1] = PAGE_MAGIC_1;
    hdr[2] = PAGE_MAGIC_2;
    hdr[3] = PAGE_MAGIC_3;
    hdr[4] = PAGE_VERSION;
    hdr[5] = 0; /* flags */
    hdr[6] = (unsigned char)(root->nodeId & 0xFF);
    hdr[7] = (unsigned char)((root->nodeId >> 8) & 0xFF);
    hdr[8] = (unsigned char)((root->nodeId >> 16) & 0xFF);
    hdr[9] = (unsigned char)((root->nodeId >> 24) & 0xFF);
    hdr[10] = 0;
    hdr[11] = 0;
    pw_bytes(&w, hdr, PAGE_HEADER_BYTES);
    w_node(&w, root);
    if (w.err)
    {
        pluto_spill_discard(w.h);
        return -1;
    }
    long total = pluto_spill_finish(w.h);
    if (total <= 0)
    {
        pluto_spill_discard(w.h);
        return -1;
    }
    /* Committed: stamp the stub and shed the RAM. Everything under root
     * EXCEPT root itself is freed; root remains linked in its parent
     * carrying the marker (an empty leaf to every walker). */
    root->pagedKey = key;
    for (int i = 0; i < root->childCount; i++)
    {
        dom_page_free_subtree_impl(root->children[i]);
    }
    if (root->children)
    {
        PLUTO_FREE(root->children);
        root->children = NULL;
    }
    root->childCount = 0;
    root->childCap = 0;
    logger_log("[page] paged out id=%d key=%08lx bytes=%ld", root->nodeId,
               key, total);
    return 0;
}

int dom_is_paged(const DomNode *node)
{
    return node && node->pagedKey != 0;
}

/* ── pressure policy ─────────────────────────────────────────────────────── */

/* Subtree RAM size: nodes + attr arrays + child arrays. Strings (tag/text/
 * attr values) are excluded — they live in the doc arena, which dies with
 * the document regardless. Includes stub shells (72B each, near-zero).
 * Iterative (explicit stack, heap-allocated): the recursive version spent
 * one C frame per depth inside the pressure policy's hot loop. */
typedef struct
{
    const DomNode *node;
    int nextChild;
} SbFrame;

static long subtree_bytes(const DomNode *root)
{
    if (!root)
    {
        return 0;
    }
    int cap = 64;
    int top = 0;
    SbFrame *st = (SbFrame *)PLUTO_MALLOC(sizeof(SbFrame) * (size_t)cap);
    if (!st)
    {
        return 0; /* allocator out: report 0 (policy treats it as small) */
    }
    st[top].node = root;
    st[top].nextChild = 0;
    top++;
    long total = 0;
    while (top > 0)
    {
        SbFrame *f = &st[top - 1];
        const DomNode *n = f->node;
        if (f->nextChild == 0)
        {
            total += (long)sizeof(DomNode);
            if (n->attrs)
            {
                total += (long)n->attrCount * (long)sizeof(DomAttr);
            }
            if (n->children)
            {
                total += (long)n->childCap * (long)sizeof(DomNode *);
            }
        }
        if (f->nextChild < n->childCount)
        {
            const DomNode *c = n->children[f->nextChild++];
            if (!c)
            {
                continue;
            }
            if (top + 1 >= cap)
            {
                int ncap = cap * 2;
                SbFrame *grown = (SbFrame *)PLUTO_REALLOC(
                    st, sizeof(SbFrame) * (size_t)ncap);
                if (!grown)
                {
                    PLUTO_FREE(st);
                    return total;
                }
                st = grown;
                cap = ncap;
            }
            st[top].node = c;
            st[top].nextChild = 0;
            top++;
        }
        else
        {
            top--;
        }
    }
    PLUTO_FREE(st);
    return total;
}

/* Deepest-first: the largest child subtree of `root` that clears the floor
 * (paging a big leaf region frees the most RAM for one file). Returns the
 * child to page (its whole subtree goes), NULL when nothing qualifies. */
static DomNode *pick_page_candidate(DomResult *dom, DomNode *root,
                                    long floorBytes)
{
    DomNode *best = NULL;
    long bestBytes = floorBytes;
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (n->pagedKey)
        {
            continue; /* already a stub */
        }
        for (int i = 0; i < n->childCount; i++)
        {
            DomNode *c = n->children[i];
            if (!c || c->pagedKey || c == root)
            {
                continue;
            }
            long b = subtree_bytes(c);
            if (b > bestBytes)
            {
                bestBytes = b;
                best = c;
            }
            if (top < DOM_SEARCH_MAX_DEPTH)
            {
                stack[top++] = c;
            }
        }
    }
    (void)dom;
    return best;
}

/* Number of stubs currently in the live tree (autotest/telemetry). */
int dom_page_stub_count(DomResult *dom)
{
    if (!dom || !dom->root)
    {
        return 0;
    }
    int count = 0;
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (n->pagedKey)
        {
            count++;
            continue; /* stubs have no children */
        }
        for (int i = 0; i < n->childCount; i++)
        {
            if (top < DOM_SEARCH_MAX_DEPTH)
            {
                stack[top++] = n->children[i];
            }
        }
    }
    return count;
}

int dom_page_out_under_pressure(DomResult *dom, const char *baseUrl)
{
    if (!dom || !dom->root || !baseUrl || !baseUrl[0])
    {
        return 0;
    }
    unsigned long live = pluto_mem_live();
    unsigned long budget = pluto_mem_budget();
    if (budget == 0 || live >= budget ||
        budget - live > PAGE_HEADROOM_TRIGGER)
    {
        return 0; /* comfortable: keep everything in RAM */
    }
    long floorBytes =
        g_pageFloorOverride > 0 ? g_pageFloorOverride
                                : PAGE_SUBTREE_FLOOR_BYTES;
    int paged = 0;
    while (budget - pluto_mem_live() <= PAGE_HEADROOM_TRIGGER)
    {
        DomNode *pick = pick_page_candidate(dom, dom->root, floorBytes);
        if (!pick)
        {
            break; /* nothing left worth paging */
        }
        if (dom_page_out(dom, pick, baseUrl) != 0)
        {
            break; /* store write failed: stop trying */
        }
        paged++;
    }
    return paged;
}


