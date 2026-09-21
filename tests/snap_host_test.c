/* SW6 host test: pluto_snap rendered-snapshot cache.
 *
 * Builds a DocParseResult walk output by hand (the same structs the walker
 * produces), saves it with pluto_snap_save, loads it back with
 * pluto_snap_load, and asserts field-level parity — plus expiry, mode
 * isolation, corrupt-entry refusal, about:-page skip, invalidate, and the
 * LRU sweep. Runs under ASan+UBSan.
 *
 * Build & run (repo root):
 *   cc -o /tmp/snaptest tests/snap_host_test.c Source/core/pluto_snap.c \
 *     Source/core/pluto_spill.c Source/core/pluto_mem.c Source/core/logger.c \
 *     Source/html/document.c Source/html/dom.c Source/html/entities.c \
 *     Source/html/tokenizer.c Source/html/readability.c Source/html/css.c \
 *     Source/core/url.c Source/core/constants.c Source/util/strbuf.c \
 *     Source/util/strutil.c Source/util/json.c \
 *     -I. -ISource -ISource/core -ISource/util -ISource/html \
 *     -DPLUTO_SPILL_HOST -DTARGET_SIMULATOR=1 -DTARGET_EXTENSION=1 \
 *     -DCONFIG_VERSION='"2026-06-04"' -fsanitize=address,undefined && /tmp/snaptest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/pluto_snap.h"
#include "core/pluto_spill.h"
#include "core/constants.h"
#include "html/document.h"
#include "html/dom.h"

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

/* ── build a rich fake walk output (paragraph + list + image + table + form +
 * link + map + datalist — most block kinds the layout consumes) ─────────── */

static DocInline *mk_inline(DocArena *a, int type, const char *text,
                            unsigned flags, const char *href)
{
    DocInline *in = (DocInline *)doc_arena_alloc(a, sizeof(DocInline));
    memset(in, 0, sizeof(*in));
    in->type = type;
    in->text = doc_arena_strdup(a, text ? text : "");
    in->flags = flags;
    in->href = href ? doc_arena_strdup(a, href) : NULL;
    return in;
}

static DocBlock *mk_block(DocArena *a, DocParseResult *d, int type)
{
    (void)a;
    (void)d;
    (void)type;
    return NULL; /* unused helper removed — add_block is the real one */
}

/* simple realloc-based push (mirrors doc_ptrarr_push semantics) */
static int push_ptr(void ***arr, int *count, int *cap, void *item)
{
    if (*count >= *cap)
    {
        int nc = *cap ? *cap * 2 : 8;
        void **na = (void **)realloc(*arr, (size_t)nc * sizeof(void *));
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

static DocArena *g_a = NULL; /* test arena (freed via doc_arena_free_all) */

static DocBlock *add_block(DocParseResult *d, int type)
{
    DocBlock *b = (DocBlock *)doc_arena_alloc(g_a, sizeof(DocBlock));
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->maxlength = -1;
    b->fieldWidth = -1;
    b->fieldRows = -1;
    b->colWidth = -1;
    push_ptr((void ***)&d->blocks, &d->blockCount, &d->blockCap, b);
    return b;
}

static DocLink *add_link(DocParseResult *d, const char *href, const char *text)
{
    DocLink *l = (DocLink *)doc_arena_alloc(g_a, sizeof(DocLink));
    memset(l, 0, sizeof(*l));
    l->href = doc_arena_strdup(g_a, href);
    l->text = doc_arena_strdup(g_a, text);
    push_ptr((void ***)&d->links, &d->linkCount, &d->linkCap, l);
    return l;
}

static DocParseResult *build_fake_doc(void)
{
    DocParseResult *d = (DocParseResult *)calloc(1, sizeof(DocParseResult));
    g_a = (DocArena *)calloc(1, sizeof(DocArena));
    d->_arena = g_a;
    snprintf(d->title, sizeof(d->title), "Snapshot Test Page");
    snprintf(d->baseUrl, sizeof(d->baseUrl), "https://example.com/page");
    d->metaRefresh.present = 0;
    d->suppressNoscript = 1;
    d->jsRan = 2;
    d->jsErrors = 0;
    snprintf(d->jsLastError, sizeof(d->jsLastError), "");

    /* heading */
    DocBlock *h = add_block(d, DOC_BLOCK_HEADING);
    h->level = 1;
    h->inlines = NULL;
    {
        DocInline *in = mk_inline(g_a, DOC_INLINE_TEXT, "Hello Snapshot",
                                  DOC_INF_BOLD, NULL);
        push_ptr((void ***)&h->inlines, &h->inlineCount, &h->inlineCap, in);
    }

    /* paragraph with styled + linked inline */
    DocBlock *p = add_block(d, DOC_BLOCK_PARAGRAPH);
    p->hasSpacing = 1;
    p->spacingTop = 4;
    p->spacingBottom = 6;
    {
        push_ptr((void ***)&p->inlines, &p->inlineCount, &p->inlineCap,
                 mk_inline(g_a, DOC_INLINE_TEXT, "plain ", 0, NULL));
        push_ptr((void ***)&p->inlines, &p->inlineCount, &p->inlineCap,
                 mk_inline(g_a, DOC_INLINE_TEXT, "italic", DOC_INF_ITALIC,
                           "https://example.com/target"));
    }

    /* list item */
    DocBlock *li = add_block(d, DOC_BLOCK_LIST_ITEM);
    li->isOrdered = 1;
    li->hasNumber = 1;
    li->number = 7;
    li->markerType = 'a';
    li->depth = 1;
    push_ptr((void ***)&li->inlines, &li->inlineCount, &li->inlineCap,
             mk_inline(g_a, DOC_INLINE_TEXT, "list text", 0, NULL));

    /* image */
    DocBlock *img = add_block(d, DOC_BLOCK_IMAGE);
    img->src = doc_arena_strdup(g_a, "https://example.com/pic.png");
    img->alt = doc_arena_strdup(g_a, "a picture");
    img->width = 320.0;
    img->height = 240.0;

    /* code block */
    DocBlock *code = add_block(d, DOC_BLOCK_CODE_BLOCK);
    code->text = doc_arena_strdup(g_a, "");
    {
        char *l1 = doc_arena_strdup(g_a, "int main(void)");
        char *l2 = doc_arena_strdup(g_a, "{");
        push_ptr((void ***)&code->lines, &code->lineCount, &code->lineCap, l1);
        push_ptr((void ***)&code->lines, &code->lineCount, &code->lineCap, l2);
    }

    /* table 2x2 with header row + caption */
    DocBlock *tb = add_block(d, DOC_BLOCK_TABLE);
    tb->table = (DocTable *)doc_arena_alloc(g_a, sizeof(DocTable));
    memset(tb->table, 0, sizeof(*tb->table));
    tb->table->border = 1;
    tb->table->caption = doc_arena_strdup(g_a, "Demo table");
    tb->table->colTotal = 2;
    for (int r = 0; r < 2; r++)
    {
        DocRow *row = (DocRow *)doc_arena_alloc(g_a, sizeof(DocRow));
        memset(row, 0, sizeof(*row));
        for (int c = 0; c < 2; c++)
        {
            DocCell *cell = (DocCell *)doc_arena_alloc(g_a, sizeof(DocCell));
            memset(cell, 0, sizeof(*cell));
            cell->header = (r == 0);
            cell->colspan = 1;
            cell->rowspan = 1;
            cell->abbr = doc_arena_strdup(g_a, "");
            push_ptr((void ***)&cell->inlines, &cell->inlineCount,
                     &cell->inlineCap,
                     mk_inline(g_a, DOC_INLINE_TEXT,
                               (r == 0 && c == 0) ? "H0"
                               : (r == 0 && c == 1) ? "H1"
                               : (c == 0) ? "r1c0" : "r1c1",
                               0, NULL));
            push_ptr((void ***)&row->cells, &row->cellCount, &row->cellCap, cell);
        }
        push_ptr((void ***)&tb->table->rows, &tb->table->rowCount,
                 &tb->table->rowCap, row);
    }

    /* input field + select */
    DocBlock *inp = add_block(d, DOC_BLOCK_INPUT_FIELD);
    inp->name = doc_arena_strdup(g_a, "user");
    inp->value = doc_arena_strdup(g_a, "typed value");
    inp->placeholder = doc_arena_strdup(g_a, "your name");
    inp->inputType = doc_arena_strdup(g_a, "text");
    inp->maxlength = 32;
    inp->fieldWidth = 20;
    DocBlock *sel = add_block(d, DOC_BLOCK_SELECT_FIELD);
    sel->name = doc_arena_strdup(g_a, "pick");
    sel->selectedIndex = 2;
    for (int i = 0; i < 3; i++)
    {
        DocOption *o = (DocOption *)doc_arena_alloc(g_a, sizeof(DocOption));
        memset(o, 0, sizeof(*o));
        char buf[24];
        snprintf(buf, sizeof(buf), "option %d", i);
        o->text = doc_arena_strdup(g_a, buf);
        o->value = doc_arena_strdup(g_a, buf);
        o->selected = (i == 1);
        push_ptr((void ***)&sel->options, &sel->optionCount, &sel->optionCap, o);
    }

    /* meter */
    DocBlock *m = add_block(d, DOC_BLOCK_METER);
    m->mvalue = 0.5;
    m->mmin = 0.0;
    m->mmax = 1.0;
    m->mlow = 0.2;
    m->mhigh = 0.8;
    m->moptimum = 0.6;

    /* links + map + datalist */
    add_link(d, "https://example.com/a", "Link A");
    add_link(d, "https://example.com/b", "Link B");

    d->maps = NULL;
    DocMap *map = (DocMap *)doc_arena_alloc(g_a, sizeof(DocMap));
    memset(map, 0, sizeof(*map));
    map->name = doc_arena_strdup(g_a, "mainmap");
    DocArea *area = (DocArea *)doc_arena_alloc(g_a, sizeof(DocArea));
    memset(area, 0, sizeof(*area));
    area->shape = doc_arena_strdup(g_a, "rect");
    area->coords = (int *)malloc(4 * sizeof(int));
    area->coords[0] = 0;
    area->coords[1] = 0;
    area->coords[2] = 10;
    area->coords[3] = 10;
    area->coordCount = 4;
    area->href = doc_arena_strdup(g_a, "https://example.com/area");
    area->alt = doc_arena_strdup(g_a, "area alt");
    push_ptr((void ***)&map->areas, &map->areaCount, &map->areaCap, area);
    push_ptr((void ***)&d->maps, &d->mapCount, &d->mapCap, map);

    DocDatalist *dl = (DocDatalist *)doc_arena_alloc(g_a, sizeof(DocDatalist));
    memset(dl, 0, sizeof(*dl));
    dl->id = doc_arena_strdup(g_a, "dl1");
    DocOption *do1 = (DocOption *)doc_arena_alloc(g_a, sizeof(DocOption));
    memset(do1, 0, sizeof(*do1));
    do1->text = doc_arena_strdup(g_a, "suggestion");
    do1->value = doc_arena_strdup(g_a, "suggestion");
    push_ptr((void ***)&dl->options, &dl->optionCount, &dl->optionCap, do1);
    push_ptr((void ***)&d->datalists, &d->datalistCount, &d->datalistCap, dl);

    return d;
}

/* free a fake doc the same way document_free would (arrays + arena) */
static void free_fake_doc(DocParseResult *d)
{
    /* hand-built: only top arrays + area coords were heap-allocated */
    for (int i = 0; i < d->mapCount; i++)
    {
        DocMap *m = d->maps[i];
        for (int j = 0; j < m->areaCount; j++)
        {
            free(m->areas[j]->coords);
        }
        free(m->areas);
    }
    free(d->maps);
    free(d->blocks);
    free(d->links);
    free(d->datalists);
    doc_arena_free_all((DocArena *)d->_arena);
    free(d->_arena);
    free(d);
}

/* field-level parity between original and restored docs */
static int docs_equal(const DocParseResult *a, const DocParseResult *b)
{
    if (strcmp(a->title, b->title) != 0 ||
        strcmp(a->baseUrl, b->baseUrl) != 0 ||
        a->blockCount != b->blockCount ||
        a->linkCount != b->linkCount ||
        a->mapCount != b->mapCount ||
        a->datalistCount != b->datalistCount ||
        a->jsRan != b->jsRan ||
        a->suppressNoscript != b->suppressNoscript)
    {
        return 0;
    }
    for (int i = 0; i < a->blockCount; i++)
    {
        const DocBlock *ba = a->blocks[i];
        const DocBlock *bb = b->blocks[i];
        if (ba->type != bb->type || ba->level != bb->level ||
            ba->inlineCount != bb->inlineCount ||
            ba->spacingTop != bb->spacingTop ||
            ba->number != bb->number ||
            ba->markerType != bb->markerType)
        {
            return 0;
        }
        for (int j = 0; j < ba->inlineCount; j++)
        {
            const DocInline *ia = ba->inlines[j];
            const DocInline *ib = bb->inlines[j];
            if (ia->type != ib->type || ia->flags != ib->flags ||
                ia->anchorIndex != ib->anchorIndex ||
                !ia->text != !ib->text ||
                (ia->text && strcmp(ia->text, ib->text) != 0))
            {
                return 0;
            }
            if ((ia->href ? 1 : 0) != (ib->href ? 1 : 0) ||
                (ia->href && strcmp(ia->href, ib->href) != 0))
            {
                return 0;
            }
        }
        /* deep fields by type */
        if (ba->type == DOC_BLOCK_IMAGE &&
            (strcmp(ba->src, bb->src) != 0 ||
             ba->width != bb->width || ba->height != bb->height))
        {
            return 0;
        }
        if (ba->type == DOC_BLOCK_CODE_BLOCK &&
            (ba->lineCount != bb->lineCount ||
             (ba->lineCount && strcmp(ba->lines[0], bb->lines[0]) != 0)))
        {
            return 0;
        }
        if (ba->type == DOC_BLOCK_TABLE &&
            (ba->table->rowCount != bb->table->rowCount ||
             ba->table->border != bb->table->border ||
             strcmp(ba->table->caption, bb->table->caption) != 0 ||
             ba->table->rows[0]->cells[0]->header !=
                 bb->table->rows[0]->cells[0]->header ||
             strcmp(ba->table->rows[1]->cells[1]->inlines[0]->text,
                    bb->table->rows[1]->cells[1]->inlines[0]->text) != 0))
        {
            return 0;
        }
        if (ba->type == DOC_BLOCK_INPUT_FIELD &&
            (strcmp(ba->value, bb->value) != 0 ||
             ba->maxlength != bb->maxlength ||
             (ba->name ? !bb->name : 0) ||
             (ba->name && strcmp(ba->name, bb->name) != 0)))
        {
            return 0;
        }
        if (ba->type == DOC_BLOCK_SELECT_FIELD &&
            (ba->optionCount != bb->optionCount ||
             ba->selectedIndex != bb->selectedIndex ||
             strcmp(ba->options[1]->text, bb->options[1]->text) != 0 ||
             ba->options[1]->selected != bb->options[1]->selected))
        {
            return 0;
        }
        if (ba->type == DOC_BLOCK_METER && ba->mvalue != bb->mvalue)
        {
            return 0;
        }
    }
    for (int i = 0; i < a->linkCount; i++)
    {
        if (strcmp(a->links[i]->href, b->links[i]->href) != 0 ||
            strcmp(a->links[i]->text, b->links[i]->text) != 0)
        {
            return 0;
        }
    }
    if (a->mapCount &&
        (strcmp(a->maps[0]->name, b->maps[0]->name) != 0 ||
         a->maps[0]->areas[0]->coordCount != b->maps[0]->areas[0]->coordCount ||
         a->maps[0]->areas[0]->coords[3] != b->maps[0]->areas[0]->coords[3]))
    {
        return 0;
    }
    if (a->datalistCount &&
        (strcmp(a->datalists[0]->id, b->datalists[0]->id) != 0 ||
         strcmp(a->datalists[0]->options[0]->text,
                b->datalists[0]->options[0]->text) != 0))
    {
        return 0;
    }
    return 1;
}

#define URL "https://example.com/page"
#define URL2 "https://example.com/other"

int main(void)
{
    system("rm -rf /tmp/plutobrowser_spill && mkdir -p /tmp/plutobrowser_spill");

    CHECK(pluto_spill_init() == 0, "spill init");

    DocParseResult *orig = build_fake_doc();
    CHECK(orig->blockCount == 9, "fake doc has 9 blocks");

    /* 1. save + full round-trip */
    long bytes = pluto_snap_save(orig, URL, MODE_RAW_HTML, 1000000UL);
    CHECK(bytes > 0, "save returns byte count");
    system("rm -rf /tmp/snapdump && cp -r /tmp/plutobrowser_spill /tmp/snapdump");
    DocParseResult *back = pluto_snap_load(URL, MODE_RAW_HTML, 1000060UL, 3600UL);
    CHECK(back != NULL, "load returns a doc within TTL");
    if (back)
    {
        CHECK(docs_equal(orig, back), "field-level parity after round-trip");
        CHECK(back->_dom == NULL && back->_jsbridge == NULL,
              "restored doc has no live DOM/bridge");
        CHECK(back->blocks[0]->inlines[0]->text != orig->blocks[0]->inlines[0]->text,
              "strings are freshly allocated (not aliased)");
        document_free(back);
        free(back);
    }

    /* 2. mode isolation */
    CHECK(pluto_snap_load(URL, MODE_READER, 1000060UL, 3600UL) == NULL,
          "mode mismatch misses (no cross-mode replay)");

    /* 3. TTL expiry */
    CHECK(pluto_snap_load(URL, MODE_RAW_HTML, 1000000UL + 3601UL, 3600UL) == NULL,
          "expired entry misses");
    CHECK(pluto_snap_load(URL, MODE_RAW_HTML, 1000000UL + 3600UL, 3600UL) != NULL,
          "exactly-at-TTL entry still hits");
    if (pluto_snap_load(URL, MODE_RAW_HTML, 1000000UL + 3600UL, 3600UL))
    {
        DocParseResult *t = pluto_snap_load(URL, MODE_RAW_HTML, 1000000UL + 3600UL, 3600UL);
        document_free(t);
        free(t);
    }
    CHECK(pluto_snap_load(URL, MODE_RAW_HTML, 0, 3600UL) != NULL,
          "nowEpoch=0 disables the age check");
    {
        DocParseResult *t = pluto_snap_load(URL, MODE_RAW_HTML, 0, 3600UL);
        document_free(t);
        free(t);
    }

    /* 4. the live-JS suite pages are never cached; other about: pages are */
    CHECK(pluto_snap_save(orig, "about:javascript", MODE_RAW_HTML, 1000000UL) == 0,
          "about:javascript is skipped on save");
    CHECK(pluto_snap_load("about:javascript", MODE_RAW_HTML, 0, 0) == NULL,
          "about:javascript is skipped on load");
    CHECK(pluto_snap_save(orig, "about:home", MODE_RAW_HTML, 1000000UL) > 0,
          "about:home saves (cacheable about: page)");
    pluto_snap_invalidate("about:home", MODE_RAW_HTML);
    CHECK(pluto_snap_load("about:home", MODE_RAW_HTML, 0, 0) == NULL,
          "about:home invalidate removes the entry");

    /* 5. invalidate */
    pluto_snap_invalidate(URL, MODE_RAW_HTML);
    CHECK(pluto_snap_load(URL, MODE_RAW_HTML, 0, 0) == NULL,
          "invalidate removes the entry");

    /* 6. re-save for the LRU test + a second url */
    CHECK(pluto_snap_save(orig, URL, MODE_RAW_HTML, 1000000UL) > 0, "re-save ok");
    DocParseResult *orig2 = build_fake_doc();
    CHECK(pluto_snap_save(orig2, URL2, MODE_RAW_HTML, 1000001UL) > 0,
          "second url saves");
    CHECK(pluto_spill_store_count() == 2, "store holds 2 snapshots");
    CHECK(pluto_snap_lru_sweep(1) == 1, "lru sweep evicts the oldest");
    CHECK(pluto_spill_store_count() == 1, "store holds 1 after sweep");
    CHECK(pluto_snap_lru_sweep(8) == 0, "sweep under cap is a no-op");

    /* 7. corrupt entry: truncate a store file on disk */
    {
        DocParseResult *orig3 = build_fake_doc();
        CHECK(pluto_snap_save(orig3, "https://example.com/corrupt",
                              MODE_RAW_HTML, 1000002UL) > 0,
              "corrupt-target url saved");
        free_fake_doc(orig3);
    }
    /* newest file = the corrupt-url entry just saved (store ids ascend) */
    system("f=$(ls -t /tmp/plutobrowser_spill/bc_*.bin | head -1); "
           "truncate -s 40 \"$f\"");
    CHECK(pluto_snap_load("https://example.com/corrupt", MODE_RAW_HTML, 0, 0) == NULL,
          "truncated entry refuses to load");

    /* cleanup */
    pluto_snap_invalidate_all();
    CHECK(pluto_spill_store_count() == 0, "invalidate_all empties the store");
    free_fake_doc(orig);
    free_fake_doc(orig2);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
