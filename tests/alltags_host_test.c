/*
 * Full element-index coverage test: every HTML element listed in the
 * WHATWG standard (including obsolete/non-conforming ones, which the
 * reference PDF also specifies) is parsed through the real pipeline.
 * PASS requires: no parse error AND text after the element survives
 * (i.e., the tag has a defined behavior and does not truncate the tree).
 *
 * Build & run:
 *   SDK=/path/to/PlaydateSDK
 *   cc -DTARGET_EXTENSION=1 -o /tmp/alltags tests/alltags_host_test.c \
 *     Source/html/tokenizer.c Source/html/dom.c Source/html/document.c \
 *     Source/html/entities.c Source/html/readability.c Source/core/url.c \
 *     Source/core/constants.c Source/core/logger.c Source/util/strbuf.c \
 *     Source/util/strutil.c Source/util/pdtimer.c Source/core/tasks.c \
 *     -I. -I"$SDK/C_API" -I"$SDK/C_API/pd_api" -ISource -ISource/core \
 *     -ISource/util -ISource/html -ISource/render && /tmp/alltags
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

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
void tasks_report_progress(float f) { (void)f; } /* readability stub */

#include "html/document.h"
#include "core/constants.h"

/* The complete HTML element index: §4.1–4.12 standard elements, the
 * obsolete/non-conforming list (§15.2 and friends), and the newer
 * proposals (portal, fencedframe, selectedcontent). Every one must have
 * a defined, non-destructive behavior. */
static const char *ALL_TAGS[] = {
    /* §4.1–4.3 document metadata & sectioning */
    "html", "head", "body", "title", "base", "link", "meta", "style",
    "article", "section", "nav", "aside", "h1", "h2", "h3", "h4", "h5",
    "h6", "hgroup", "header", "footer", "address", "search",
    /* §4.4 grouping */
    "p", "hr", "pre", "blockquote", "ol", "ul", "menu", "li", "dl", "dt",
    "dd", "figure", "figcaption", "main", "div",
    /* §4.5 text-level */
    "a", "em", "strong", "small", "s", "cite", "q", "dfn", "abbr", "ruby",
    "rt", "rp", "rb", "rtc", "data", "time", "code", "var", "samp", "kbd",
    "sub", "sup", "i", "b", "u", "mark", "br", "wbr", "bdi", "bdo", "span",
    /* §4.7 edits */
    "ins", "del",
    /* §4.8 embedded */
    "picture", "source", "img", "iframe", "embed", "object", "video",
    "audio", "track", "map", "area", "canvas", "portal", "fencedframe",
    "svg", "math",
    /* §4.9 tables */
    "table", "caption", "colgroup", "col", "tbody", "thead", "tfoot",
    "tr", "td", "th",
    /* §4.10 forms */
    "form", "label", "input", "button", "select", "datalist", "optgroup",
    "option", "textarea", "output", "progress", "meter", "fieldset",
    "legend",
    /* §4.11 interactive */
    "details", "summary", "dialog", "geolocation",
    /* §4.12 scripting */
    "script", "noscript", "template", "slot", "content", "shadow",
    /* obsolete / non-conforming but still specified */
    "center", "font", "big", "strike", "tt", "acronym", "dir", "marquee",
    "nobr", "spacer", "noembed", "noframes", "noindex", "frameset",
    "frame", "param", "basefont", "bgsound", "keygen", "isindex",
    "menuitem", "plaintext", "listing", "xmp", "selectedcontent", "image",
};
#define NTAGS (sizeof(ALL_TAGS) / sizeof(ALL_TAGS[0]))

static int text_survives(const DocParseResult *d, const char *needle)
{
    for (int i = 0; i < d->blockCount; i++)
    {
        const DocBlock *b = d->blocks[i];
        if (!b)
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

int main(void)
{
    int pass = 0, fail = 0;
    char doc[256];
    for (size_t t = 0; t < NTAGS; t++)
    {
        /* Element between two markers; both markers must survive and the
         * parse must not error (tag has defined behavior, no truncation). */
        snprintf(doc, sizeof(doc),
                 "<p>MARKER-A</p><%s>%s-content</%s><p>MARKER-B</p>",
                 ALL_TAGS[t], ALL_TAGS[t], ALL_TAGS[t]);
        DocParseResult *d = calloc(1, sizeof(DocParseResult));
        document_parse(doc, "https://example.com/", MODE_RAW_HTML, NULL, d);
        int ok = !d->parseError && text_survives(d, "MARKER-A") &&
                 text_survives(d, "MARKER-B");
        printf("%-14s %s\n", ALL_TAGS[t], ok ? "OK" : "FAIL");
        if (ok)
        {
            pass++;
        }
        else
        {
            fail++;
        }
        document_free(d);
        free(d);
    }
    printf("\n%d/%d elements have defined, non-destructive behavior\n",
           pass, (int)NTAGS);
    return fail ? 1 : 0;
}
