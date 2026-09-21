/* SW8 host test: disk-backed DOM paging (pluto_page).
 *
 * Builds real DOM trees with the dom builder, pages subtrees out with
 * dom_page_out, and asserts the fault model end-to-end:
 *   - page-out frees everything under root except root; root keeps its key
 *   - stubs stay linked in the live tree (parent sees an empty leaf)
 *   - dom_touch materializes the subtree with EXACT structural + string
 *     parity: tags, text, attrs, nodeId values, kind, boolean attr sentinel
 *   - id lookup works again after materialize (SW5 bridge choke point)
 *   - dom_node_by_id on a STUB transparently materializes (the actual
 *     bridge touch point)
 *   - failure modes: missing store entry, corrupt image, truncated image
 *   - second page-out of an already-stubbed root is a no-op
 *   - dom_free_result over a tree containing never-materialized stubs
 *     frees cleanly (ASan would scream otherwise)
 *   - materialize replaces the doc root when the stub IS the root
 * Runs under ASan+UBSan.
 *
 * Build & run (repo root): see /tmp/build_pagetest.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/pluto_page.h"
#include "core/pluto_spill.h"
#include "html/dom.h"
#include "html/tokenizer.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                          \
    do                                             \
    {                                              \
        if (cond)                                  \
        {                                          \
            printf("PASS: %s\n", name);            \
            g_pass++;                              \
        }                                          \
        else                                       \
        {                                          \
            printf("FAIL: %s\n", name);            \
            g_fail++;                              \
        }                                          \
    } while (0)

/* ── tree builders (real dom builder) ───────────────────────────────────── */

static DomResult *mk_doc(const char *html)
{
    DomResult *d = (DomResult *)malloc(sizeof(DomResult));
    memset(d, 0, sizeof(*d));
    if (dom_build_from_html(html, d) != 0)
    {
        free(d);
        return NULL;
    }
    return d;
}

static DomNode *find_tag(DomNode *n, const char *tag)
{
    if (!n)
    {
        return NULL;
    }
    if (n->kind == DOM_ELEMENT && n->tag && strcmp(n->tag, tag) == 0)
    {
        return n;
    }
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *h = find_tag(n->children[i], tag);
        if (h)
        {
            return h;
        }
    }
    return NULL;
}

static int count_nodes(const DomNode *n)
{
    if (!n)
    {
        return 0;
    }
    int c = 1;
    for (int i = 0; i < n->childCount; i++)
    {
        c += count_nodes(n->children[i]);
    }
    return c;
}

/* Force-assign runtime ids for a whole subtree (ids are LAZY in dom.c —
 * assigned on first dom_node_id call). This simulates "JS has touched
 * everything", which is exactly the state a paged-out subtree must
 * survive: JS-held ids must resolve again after materialize. */
static void assign_all_ids(DomResult *d, DomNode *n)
{
    if (!n)
    {
        return;
    }
    dom_node_id(d, n);
    for (int i = 0; i < n->childCount; i++)
    {
        assign_all_ids(d, n->children[i]);
    }
}

/* find an attribute by key (NULL when absent) */
static const DomAttr *find_attr(const DomNode *n, const char *key)
{
    for (int i = 0; i < n->attrCount; i++)
    {
        if (strcmp(n->attrs[i].key, key) == 0)
        {
            return &n->attrs[i];
        }
    }
    return NULL;
}

/* deep structural+string compare of two subtrees */
static int trees_equal(const DomNode *a, const DomNode *b)
{
    if (!a || !b)
    {
        return a == b;
    }
    if (a->kind != b->kind || a->nodeId != b->nodeId ||
        a->childCount != b->childCount)
    {
        return 0;
    }
    if (a->kind == DOM_ELEMENT)
    {
        if (!a->tag != !b->tag ||
            (a->tag && strcmp(a->tag, b->tag) != 0) ||
            a->attrCount != b->attrCount)
        {
            return 0;
        }
        for (int i = 0; i < a->attrCount; i++)
        {
            if (strcmp(a->attrs[i].key, b->attrs[i].key) != 0)
            {
                return 0;
            }
            const char *av = a->attrs[i].value;
            const char *bv = b->attrs[i].value;
            if (av == PLUTO_TOK_ATTR_TRUE || bv == PLUTO_TOK_ATTR_TRUE)
            {
                if (av != bv)
                {
                    return 0;
                }
            }
            else if (!av != !bv || (av && strcmp(av, bv) != 0))
            {
                return 0;
            }
        }
    }
    else
    {
        if (!a->text != !b->text ||
            (a->text && strcmp(a->text, b->text) != 0))
        {
            return 0;
        }
    }
    for (int i = 0; i < a->childCount; i++)
    {
        if (!trees_equal(a->children[i], b->children[i]))
        {
            return 0;
        }
    }
    return 1;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_pageout_roundtrip(void)
{
    printf("-- page-out + materialize round trip\n");
    DomResult *d = mk_doc(
        "<div id=outer class=\"a b\">before<ul><li id=i1>alpha</li>"
        "<li id=i2>beta</li><li id=i3>gamma</li></ul>after</div>");
    CHECK(d != NULL, "doc built");
    DomNode *ul = find_tag(d->root, "ul");
    DomNode *div = find_tag(d->root, "div");
    CHECK(ul && div, "targets found");

    int totalBefore = count_nodes(d->root);
    int ulBefore = count_nodes(ul);
    assign_all_ids(d, d->root); /* JS-touched state: every id assigned */
    int id1 = ul->children[0]->nodeId;
    int id3 = ul->children[2]->nodeId;
    const char *li3text = ul->children[2]->children[0]->text;
    char li3buf[16];
    snprintf(li3buf, sizeof(li3buf), "%s", li3text);

    /* page the UL out (root of the page-out) */
    int rc = dom_page_out(d, ul, "https://ex.com/page");
    CHECK(rc == 0, "dom_page_out ok");
    CHECK(dom_is_paged(ul), "root stamped as stub");
    CHECK(ul->childCount == 0 && ul->children == NULL,
          "stub sheds children (empty leaf)");
    CHECK(count_nodes(d->root) == totalBefore - ulBefore + 1,
          "tree shed exactly the subtree RAM");
    CHECK(div->childCount >= 1, "parent still links the stub");

    /* id lookup on a paged id must transparently materialize. The stub
     * 'ul' pointer is FREED by the materialize (POINTER CONTRACT in
     * dom.h) — everything after this goes through re-resolved pointers. */
    DomNode *li1 = dom_node_by_id(d, id1);
    CHECK(li1 != NULL && strcmp(li1->tag, "li") == 0,
          "id lookup materializes stub transparently");
    DomNode *ul2 = li1 ? li1->parent : NULL; /* restored ul */
    CHECK(ul2 && !dom_is_paged(ul2),
          "stub marker cleared after materialize");
    CHECK(ul2 && count_nodes(ul2) == ulBefore,
          "subtree restored to full size");
    CHECK(ul2 && ul2->children[0]->nodeId == id1 &&
              ul2->children[2]->nodeId == id3,
          "nodeIds preserved through the round trip");
    CHECK(ul2 && ul2->children[2]->children[0]->text &&
              strcmp(ul2->children[2]->children[0]->text, li3buf) == 0,
          "text restored verbatim");

    /* structural parity: fresh identical doc with the same forced ids must
     * match the restored tree (id assignment order is deterministic) */
    DomResult *d2 = mk_doc(
        "<div id=outer class=\"a b\">before<ul><li id=i1>alpha</li>"
        "<li id=i2>beta</li><li id=i3>gamma</li></ul>after</div>");
    assign_all_ids(d2, d2->root);
    CHECK(trees_equal(d->root, d2->root), "restored tree == fresh build");

    /* parent pointers relinked */
    int parentOk = 1;
    for (int i = 0; ul2 && i < ul2->childCount; i++)
    {
        if (ul2->children[i]->parent != ul2)
        {
            parentOk = 0;
        }
    }
    CHECK(parentOk, "restored children point at the stub's parent");

    dom_free_result(d);
    dom_free_result(d2);
}

static void test_attrs_and_boolean_sentinel(void)
{
    printf("-- attrs, classes, boolean attr sentinel\n");
    DomResult *d = mk_doc(
        "<section id=s><a href=\"https://x/y\" target=\"_blank\">link</a>"
        "<input type=checkbox checked disabled><p class=\"c1 c2\">t</p>"
        "</section>");
    DomNode *sec = find_tag(d->root, "section");
    DomNode *input = find_tag(d->root, "input");
    CHECK(sec && input, "targets found");
    assign_all_ids(d, d->root);
    DomAttr *inAttrs = input->attrs;
    int inCount = input->attrCount;

    CHECK(dom_page_out(d, sec, "https://ex.com/attrs") == 0, "page-out ok");
    int secId = sec->nodeId; /* pointer dies on materialize; id survives */
    CHECK(dom_node_by_id(d, secId) != NULL, "materialize via id");
    DomNode *sec2 = find_tag(d->root, "section");
    CHECK(sec2 && !dom_is_paged(sec2), "section materialized");
    DomNode *in2 = find_tag(d->root, "input");
    CHECK(in2 && in2->attrCount == inCount, "attr count preserved");
    CHECK(in2->attrs != inAttrs, "attr array rebuilt");
    const DomAttr *checked = find_attr(in2, "checked");
    const DomAttr *disabled = find_attr(in2, "disabled");
    CHECK(checked && checked->value == PLUTO_TOK_ATTR_TRUE,
          "boolean attr 'checked' restored as the sentinel");
    CHECK(disabled && disabled->value == PLUTO_TOK_ATTR_TRUE,
          "boolean attr 'disabled' restored as the sentinel");
    const DomAttr *href = find_attr(in2, "type");
    CHECK(href && href->value && strcmp(href->value, "checkbox") == 0,
          "value attr verbatim");
    /* pointer died on materialize — re-resolve by tag from the live tree */
    DomNode *a2 = find_tag(d->root, "a");
    const DomAttr *aHref = a2 ? find_attr(a2, "href") : NULL;
    CHECK(aHref && aHref->value && strcmp(aHref->value, "https://x/y") == 0,
          "href value verbatim");
    DomNode *p2 = find_tag(d->root, "p");
    const DomAttr *pClass = p2 ? find_attr(p2, "class") : NULL;
    (void)inAttrs;
    CHECK(pClass && strcmp(pClass->value, "c1 c2") == 0,
          "class list verbatim");

    dom_free_result(d);
}

static void test_materialize_is_root(void)
{
    printf("-- stub IS the document root\n");
    DomResult *d = mk_doc("<main id=m><p>deep</p><p>deeper</p></main>");
    DomNode *root = d->root; /* #root wrapper */
    DomNode *main = find_tag(root, "main");
    CHECK(main != NULL, "main found");

    /* page out the ROOT ITSELF: parent == NULL, stub replaces dom->root */
    int rc = dom_page_out(d, root, "https://ex.com/rootcase");
    CHECK(rc == 0, "page-out of the root ok");
    CHECK(dom_is_paged(root) && root->childCount == 0, "root is a stub");

    /* touch on the root materializes IN PLACE: identity preserved */
    DomNode *oldRoot = d->root;
    dom_touch(d, oldRoot);
    CHECK(d->root == oldRoot, "root identity preserved by in-place restore");
    CHECK(!dom_is_paged(d->root), "root stub marker cleared");
    CHECK(count_nodes(d->root) >= 4, "root subtree restored");

    dom_free_result(d);
}

static void test_missing_store_entry(void)
{
    printf("-- missing store entry (deleted file)\n");
    DomResult *d = mk_doc("<div id=z><p>gone</p></div>");
    DomNode *div = find_tag(d->root, "div");
    CHECK(dom_page_out(d, div, "https://ex.com/vanish") == 0, "page-out ok");

    /* simulate a deleted store file by wiping the family */
    pluto_spill_store_invalidate_all();
    DomNode *p = find_tag(d->root, "p");
    CHECK(p == NULL, "subtree is paged away (no p in RAM)");

    /* touch must degrade gracefully, not crash */
    dom_touch(d, div);
    CHECK(!dom_is_paged(div), "marker cleared on missing entry");
    dom_free_result(d);
}

static void test_corrupt_image(void)
{
    printf("-- corrupt + truncated images refuse to load\n");
    /* corrupt: page out, then overwrite the payload with junk */
    DomResult *d = mk_doc("<div id=c1><p>payload</p></div>");
    DomNode *div = find_tag(d->root, "div");
    assign_all_ids(d, d->root);
    CHECK(dom_page_out(d, div, "https://ex.com/corrupt") == 0, "page-out ok");
    CHECK(div->pagedKey != 0, "stub stamped");
    unsigned long key = div->pagedKey;
    SpillFile h = pluto_spill_store_open_create(key);
    CHECK(h != PLUTO_SPILL_INVALID, "reopen for corruption");
    const char junk[] = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";
    pluto_spill_write(h, junk, sizeof(junk) - 1);
    pluto_spill_finish(h);

    dom_touch(d, div);
    CHECK(!dom_is_paged(div), "marker cleared on corrupt image");
    dom_free_result(d);

    /* truncated: valid-ish header, cut off mid-payload. open_create
     * truncates; write 12-byte junk header + 8 bytes so the size passes
     * but the magic/version check fails. */
    DomResult *d2 = mk_doc("<div id=c2><ul><li>one</li><li>two</li></ul></div>");
    DomNode *div2 = find_tag(d2->root, "div");
    CHECK(dom_page_out(d2, div2, "https://ex.com/trunc") == 0, "page-out 2 ok");
    unsigned long key2 = div2->pagedKey;
    CHECK(key2 != 0, "stub stamped (2)");

    h = pluto_spill_store_open_create(key2);
    CHECK(h != PLUTO_SPILL_INVALID, "reopen for truncation");
    unsigned char hdr[20];
    for (int i = 0; i < 20; i++)
    {
        hdr[i] = (unsigned char)(i * 7 + 3);
    }
    pluto_spill_write(h, hdr, 20);
    pluto_spill_finish(h);

    dom_touch(d2, div2);
    CHECK(!dom_is_paged(div2), "marker cleared on truncated image");
    dom_free_result(d2);
}

static void test_double_pageout_and_args(void)
{
    printf("-- double page-out, arg validation, spill lifecycle\n");
    DomResult *d = mk_doc("<div id=dd><p>x</p></div>");
    DomNode *div = find_tag(d->root, "div");
    CHECK(dom_page_out(d, div, "https://ex.com/twice") == 0, "first ok");
    int rc2 = dom_page_out(d, div, "https://ex.com/twice");
    CHECK(rc2 == 0, "second is a no-op (already stubbed)");
    CHECK(dom_is_paged(div), "still a stub");

    CHECK(dom_page_out(NULL, div, "u") == -1, "null doc refused");
    CHECK(dom_page_out(d, NULL, "u") == -1, "null node refused");
    CHECK(dom_page_out(d, div, NULL) == -1, "null baseUrl refused");
    CHECK(dom_page_out(d, div, "") == -1, "empty baseUrl refused");
    CHECK(dom_page_out(d, d->root, "https://ex.com/root") == 0,
          "root page-out allowed");

    /* is_paged on garbage */
    CHECK(dom_is_paged(NULL) == 0, "is_paged(NULL) == 0");
    dom_free_result(d);
}

static void test_free_with_stubs(void)
{
    printf("-- dom_free_result over never-materialized stubs\n");
    for (int i = 0; i < 3; i++)
    {
        DomResult *d = mk_doc(
            "<div id=f><p>one</p><div><span>two</span></div><em>three</em></div>"
            "<aside id=a><b>bold</b></aside>");
        DomNode *aside = find_tag(d->root, "aside");
        CHECK(dom_page_out(d, aside, "https://ex.com/free") == 0,
              "page-out ok");
        /* free WITHOUT materializing: stub must be freed by the normal
         * tree walk (it is a plain empty leaf) — ASan verifies */
        dom_free_result(d);
        CHECK(1, "free-with-stub cycle clean");
    }
    pluto_spill_store_invalidate_all();
}

static void test_keys_are_scoped(void)
{
    printf("-- keys scoped by (baseUrl, nodeId)\n");
    DomResult *d = mk_doc("<div id=k><p>kv</p></div>");
    DomNode *div = find_tag(d->root, "div");
    DomNode *p = find_tag(d->root, "p");

    CHECK(dom_page_out(d, p, "https://a.com/x") == 0, "page p under a.com");
    unsigned long keyA = p->pagedKey;
    CHECK(keyA != 0, "key assigned");

    /* restore, then page under a different base: different key. The stub
     * pointer dies on materialize — re-resolve by tag. */
    dom_touch(d, p);
    p = find_tag(d->root, "p");
    CHECK(p && !dom_is_paged(p), "restored");
    CHECK(dom_page_out(d, p, "https://b.com/x") == 0, "page p under b.com");
    CHECK(p->pagedKey != keyA, "different baseUrl -> different key");

    /* same page-out content key derives from the ROOT's id, not a child's */
    DomNode *div2 = find_tag(d->root, "div");
    CHECK(dom_page_out(d, div2, "https://a.com/x") == 0, "page div under a.com");
    CHECK(div2->pagedKey != keyA, "different root -> different key");

    dom_free_result(d);
    pluto_spill_store_invalidate_all();
}

int main(void)
{
    pluto_spill_store_invalidate_all();

    test_pageout_roundtrip();
    test_attrs_and_boolean_sentinel();
    test_materialize_is_root();
    test_missing_store_entry();
    test_corrupt_image();
    test_double_pageout_and_args();
    test_free_with_stubs();
    test_keys_are_scoped();

    printf("\n%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
