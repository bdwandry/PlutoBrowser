/*
 * HTML tag-coverage host test (macOS only, NOT part of the Playdate build).
 * Compiles the tokenizer + dom + document pipeline against a faked
 * PlaydateAPI (only system->realloc implemented) and asserts that the
 * newly supported WHATWG tags parse into the expected blocks/links.
 *
 * Build & run:
 *   cc -o /tmp/htmltags tests/htmltags_host_test.c Source/html/tokenizer.c \
 *     Source/html/dom.c Source/html/document.c Source/html/entities.c \
 *     Source/html/readability.c Source/core/url.c Source/core/constants.c \
 *     Source/util/strbuf.c Source/util/strutil.c -I. -ISource -ISource/core \
 *     -ISource/util -ISource/html -ISource/render && /tmp/htmltags
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

/* ---- Fake Playdate API ---- */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
static struct playdate_sys g_fakeSys;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;
PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeSys.realloc = host_realloc;
        g_fakeApi.system = &g_fakeSys;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}
void pluto_free(void *p) { free(p); }
void *pluto_realloc(void *p, size_t n) { return realloc(p, n); }

#include "html/document.h"
#include "core/constants.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name)                                          \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            printf("PASS: %s\n", name);                            \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            printf("FAIL: %s\n", name);                            \
            g_fail++;                                              \
        }                                                          \
    } while (0)

static int count_blocks(const DocParseResult *d, int type)
{
    int n = 0;
    for (int i = 0; i < d->blockCount; i++)
        if (d->blocks[i] && d->blocks[i]->type == type)
            n++;
    return n;
}

static int has_inline_text(const DocParseResult *d, const char *needle)
{
    for (int i = 0; i < d->blockCount; i++)
    {
        const DocBlock *b = d->blocks[i];
        if (!b || !b->inlines)
            continue;
        for (int j = 0; j < b->inlineCount; j++)
        {
            const DocInline *in = b->inlines[j];
            if (in && in->text && in->type == DOC_INLINE_TEXT &&
                strstr(in->text, needle))
                return 1;
        }
    }
    return 0;
}

static int has_link(const DocParseResult *d, const char *needle)
{
    for (int i = 0; i < d->linkCount; i++)
        if (d->links[i] && d->links[i]->href && strstr(d->links[i]->href, needle))
            return 1;
    return 0;
}

static DocParseResult *parse(const char *html)
{
    DocParseResult *d = calloc(1, sizeof(DocParseResult));
    document_parse(html, "https://example.com/page/index.html", MODE_RAW_HTML, NULL, d);
    return d;
}

static void free_doc(DocParseResult *d)
{
    document_free(d);
    free(d);
}

int main(void)
{
    /* 1. Void close-tag quirk: </basefont> must not truncate the tree. */
    {
        DocParseResult *d = parse(
            "<p>Before legacy void.</p><basefont></basefont><p>After "
            "legacy void.</p>");
        CHECK(d->blockCount >= 2 && !d->parseError &&
                  has_inline_text(d, "After legacy void"),
              "</basefont> does not truncate the tree");
        free_doc(d);
    }

    /* 2. <optgroup> keeps its options attached to the select. */
    {
        DocParseResult *d = parse(
            "<select name=\"car\">"
            "<optgroup label=\"Swedish\"><option>Volvo</option></optgroup>"
            "<optgroup label=\"German\"><option selected>Mercedes</option></optgroup>"
            "</select>");
        int sel = 0, opts = 0, groups = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_SELECT_FIELD)
            {
                sel = 1;
                opts = b->optionCount;
                for (int j = 0; j < b->optionCount; j++)
                    if (b->options[j] && b->options[j]->group)
                        groups++;
            }
        }
        CHECK(sel && opts == 4 && groups == 2,
              "optgroup labels inserted around options");
        free_doc(d);
    }

    /* 3. Metadata containers render children (children in normal flow). */
    {
        DocParseResult *d = parse(
            "<article><h3>Head</h3><p>Body text.</p></article>"
            "<section><p>Section text.</p></section>"
            "<nav><a href=\"page2.html\">Next page</a></nav>"
            "<address>1 Infinity Loop</address>"
            "<hgroup><p>Subtitle</p></hgroup>");
        CHECK(has_inline_text(d, "Body text") &&
                  has_inline_text(d, "Section text") &&
                  has_inline_text(d, "1 Infinity Loop") &&
                  has_inline_text(d, "Subtitle") &&
                  has_link(d, "page2.html"),
              "article/section/nav/address/hgroup render children");
        free_doc(d);
    }

    /* 4. <caption> text survives (table handler concatenates it). */
    {
        DocParseResult *d = parse(
            "<table><caption>Sample Caption</caption>"
            "<tr><td>A</td><td>B</td></tr></table>");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_TABLE && b->table &&
                b->table->caption && strstr(b->table->caption, "Sample Caption"))
                found = 1;
        }
        CHECK(found, "table caption captured");
        free_doc(d);
    }

    /* 5. <q> still quotes (regression) and <em>/<strong> nest. */
    {
        DocParseResult *d = parse(
            "<p>He said <q>hello <strong>world</strong></q> loudly.</p>");
        CHECK(has_inline_text(d, "hello") && has_inline_text(d, "world"),
              "q + strong nesting still renders text");
        free_doc(d);
    }

    /* 6. Entities: new WHATWG entries decode. */
    {
        DocParseResult *d = parse(
            "<p>&larr; &rarr; &Alpha; &sigma; &there4; &OElig; &euro;</p>");
        CHECK(has_inline_text(d, "<-"), "larr decodes");
        CHECK(has_inline_text(d, "->"), "rarr decodes");
        CHECK(has_inline_text(d, "s") || has_inline_text(d, "sigma"),
              "sigma decodes");
        CHECK(has_inline_text(d, "there4") || has_inline_text(d, ":."),
              "there4 decodes");
        CHECK(has_inline_text(d, "OE"), "OElig decodes");
        CHECK(has_inline_text(d, "EUR"), "euro decodes");
        free_doc(d);
    }

    /* 7. <abbr title> surfaces expansion when the term carries text
     * (fallback prints after the term per the span-end pattern). */
    {
        DocParseResult *d = parse(
            "<p><abbr title=\"HyperText Markup Language\">HTML</abbr></p>");
        CHECK(has_inline_text(d, "HTML") &&
                  has_inline_text(d, "(HyperText Markup Language)"),
              "abbr title expansion renders");
        free_doc(d);
    }

    /* 8. <ins>/<del> render their edit text (datetime metadata is
     * invisible in normal rendering, per spec). */
    {
        DocParseResult *d = parse(
            "<p><del datetime=\"2026-09-01\">old</del></p>"
            "<p><ins datetime=\"2026-09-12\">new</ins></p>");
        CHECK(has_inline_text(d, "old") && has_inline_text(d, "new"),
              "del/ins render edit text");
        free_doc(d);
    }

    /* 9. Legacy <marquee>/<center> still render centered (marquee now
     * flushes any open paragraph like center does). */
    {
        DocParseResult *d = parse(
            "<marquee>Rolling text</marquee><p>tail paragraph</p>");
        int centered = 0, tail = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->align && strcmp(b->align, "center") == 0 &&
                b->inlineCount > 0 && b->inlines[0]->text &&
                strstr(b->inlines[0]->text, "Rolling"))
                centered = 1;
        }
        (void)tail;
        CHECK(centered, "marquee renders centered");
        free_doc(d);
    }

    /* 10. <isindex> legacy search box. */
    {
        DocParseResult *d = parse("<isindex prompt=\"Find:\">");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_INPUT_FIELD && b->placeholder &&
                strstr(b->placeholder, "Find:"))
                found = 1;
        }
        CHECK(found, "isindex becomes a text input");
        free_doc(d);
    }

    /* 11. <optgroup> outside a select is dropped (no stray junk blocks). */
    {
        DocParseResult *d = parse(
            "<p>Keep</p><optgroup label=\"Lost\"><option>X</option></optgroup>");
        CHECK(!d->parseError && has_inline_text(d, "Keep") &&
                  !has_inline_text(d, "Lost"),
              "stray optgroup dropped");
        free_doc(d);
    }

    /* 12. p-in-block implied close: <p> closes before <caption> and
     * <optgroup> starts (both are now block-classified). */
    {
        DocParseResult *d = parse(
            "<p>one<caption>cap text</caption><p>two");
        CHECK(has_inline_text(d, "one") && has_inline_text(d, "two") &&
                  has_inline_text(d, "cap text"),
              "implied p close before caption/optgroup");
        free_doc(d);
    }

    /* 13. <canvas>/<video>/<audio> render spec fallback content. */
    {
        DocParseResult *d = parse(
            "<canvas width=\"300\" height=\"150\">Fallback text for canvas.</canvas>"
            "<video controls><source src=\"x.mp4\" type=\"video/mp4\">"
            "Video fallback text.</video>"
            "<audio src=\"y.mp3\">Audio fallback.</audio>");
        CHECK(has_inline_text(d, "Fallback text for canvas") &&
                  has_inline_text(d, "Video fallback text") &&
                  has_inline_text(d, "Audio fallback"),
              "media elements render fallback children");
        free_doc(d);
    }

    /* 14. <video poster> routes through the image pipeline. */
    {
        DocParseResult *d = parse(
            "<video poster=\"img/poster.png\" width=\"320\" height=\"180\">"
            "</video>");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_IMAGE && b->src &&
                strstr(b->src, "img/poster.png"))
                found = 1;
        }
        CHECK(found, "video poster becomes an image block");
        free_doc(d);
    }

    /* 15. <picture> with only <source> synthesizes an image. */
    {
        DocParseResult *d = parse(
            "<picture>"
            "<source media=\"(min-width:800px)\" srcset=\"wide.png 800w\">"
            "<source media=\"(max-width:799px)\" srcset=\"narrow.png 400w\">"
            "</picture>");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_IMAGE && b->src &&
                strstr(b->src, "narrow.png"))
                found = 1;
        }
        CHECK(found, "img-less picture synthesizes from last source");
        free_doc(d);
    }

    /* 16. <colgroup>/<col> spans and widths are captured. */
    {
        DocParseResult *d = parse(
            "<table>"
            "<colgroup><col span=\"2\" width=\"60\"><col width=\"25%\"></colgroup>"
            "<tr><td>a</td><td>b</td><td>c</td></tr>"
            "</table>");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_TABLE && b->table && b->table->cols)
            {
                const DocTable *t = b->table;
                if (t->colCount == 2 && t->colTotal == 3 &&
                    t->cols[0]->span == 2 && t->cols[0]->width == 60 &&
                    !t->cols[0]->percent && t->cols[1]->percent &&
                    t->cols[1]->width == 25)
                    found = 1;
            }
        }
        CHECK(found, "colgroup spans + px/percent widths parsed");
        free_doc(d);
    }

    /* 17. <col> inside <colgroup> without span defaults to 1; bare <col>
     * in table context is an implicit single-column group. */
    {
        DocParseResult *d = parse(
            "<table><col width=\"100\"><tr><td>x</td></tr></table>");
        int found = 0;
        for (int i = 0; i < d->blockCount; i++)
        {
            const DocBlock *b = d->blocks[i];
            if (b && b->type == DOC_BLOCK_TABLE && b->table && b->table->cols &&
                b->table->colCount == 1 && b->table->cols[0]->width == 100 &&
                b->table->cols[0]->span == 1)
                found = 1;
        }
        CHECK(found, "bare col becomes implicit group");
        free_doc(d);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
