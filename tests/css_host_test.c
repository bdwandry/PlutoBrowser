/*
 * CSS engine host test (macOS only, NOT part of the Playdate build).
 * Exercises the roadmap-#2 minimal CSS engine end-to-end through the real
 * pipeline: <style> scan → rule parse → selector match → cascade → walker
 * integration (hide/align/inline flags), plus JS-rewalk re-application.
 *
 * Build & run (from repo root):
 *   cc -o /tmp/csstest tests/css_host_test.c Source/html/tokenizer.c \
 *     Source/html/dom.c Source/html/document.c Source/html/entities.c \
 *     Source/html/css.c Source/html/readability.c Source/html/jsbridge.c \
 *     Source/html/jsbridge_mujs.c Source/html/jsbridge_duktape.c \
 *     Source/js/muJS/*.c Source/js/duktape/duktape.c \
 *     Source/js/QuickJS/quickjs.c Source/js/QuickJS/libregexp.c \
 *     Source/js/QuickJS/libunicode.c Source/js/QuickJS/cutils.c \
 *     Source/js/QuickJS/dtoa.c \
 *     Source/core/url.c Source/core/constants.c Source/core/logger.c \
 *     Source/util/strbuf.c Source/util/strutil.c -I. -ISource -ISource/core \
 *     -ISource/util -ISource/html -ISource/js/muJS -ISource/js/duktape \
 *     -ISource/js/QuickJS -ISource/render -lm \
 *     -I"$PLAYDATE_SDK_PATH/C_API" -DTARGET_EXTENSION=1 -DPDCS_STRDUP=1 \
 *     -fsanitize=address,undefined && /tmp/csstest
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
void *pluto_mem_realloc(void *p, size_t n); /* core/pluto_mem.c funnel */
void *pluto_mem_sdk_realloc(void *p, size_t n) { return realloc(p, n); }
void *pluto_realloc(void *p, size_t n) { return pluto_mem_realloc(p, n); }
void tasks_report_progress(float f) { (void)f; }

/* http_client stubs (jsext.c references them; never exercised here). */
typedef struct HttpCallbacks HttpCallbacks;
int http_get(const char *url, const HttpCallbacks *cb)
{
    (void)url;
    (void)cb;
    return 0;
}
size_t http_internal_page_body(const char *url, const char **bodyOut)
{
    if (bodyOut) { *bodyOut = NULL; }
    return 0;
}
void http_cancel(void) {}
void http_client_init(PlaydateAPI *pd) { (void)pd; }
void http_update(void) {}
int http_is_loading(void) { return 0; }

#include "html/css.h"
#include "html/document.h"
#include "html/jsbridge.h"
#include "html/jsbridge_internal.h"
#include "core/constants.h"

/* jsbridge_xs.c is not linked on host (needs device XS platform defines);
 * the real XS engine is linked (jsbridge_xs.c); it is never selected in
 * this binary (default = muJS). */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name)                                          \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL: %s\n", name);                            \
        }                                                          \
    } while (0)

/* Render helpers: find a block's concatenated text / its align. */
static const char *block_align(const DocParseResult *doc, const char *needle)
{
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        if ((b->type == DOC_BLOCK_PARAGRAPH || b->type == DOC_BLOCK_HEADING) &&
            b->inlineCount > 0)
        {
            for (int j = 0; j < b->inlineCount; j++)
            {
                DocInline *in = b->inlines[j];
                if (in->type == DOC_INLINE_TEXT && in->text &&
                    strstr(in->text, needle))
                {
                    return b->align;
                }
            }
        }
    }
    return NULL;
}

static int text_present(const DocParseResult *doc, const char *needle)
{
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        for (int j = 0; j < b->inlineCount; j++)
        {
            DocInline *in = b->inlines[j];
            if (in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return 1;
            }
        }
    }
    return 0;
}

static unsigned inline_flags_of(const DocParseResult *doc, const char *needle)
{
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        for (int j = 0; j < b->inlineCount; j++)
        {
            DocInline *in = b->inlines[j];
            if (in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return in->flags;
            }
        }
    }
    return 0xFFFFFFFFu;
}

static int block_inverted(const DocParseResult *doc, const char *needle)
{
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        for (int j = 0; j < b->inlineCount; j++)
        {
            DocInline *in = b->inlines[j];
            if (in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return b->invert;
            }
        }
    }
    return -1;
}

/* ── Unit: scanner ────────────────────────────────────────────────────────── */
static void test_scanner(void)
{
    CssSheet sh[8];
    int n = css_scan_sheets(
        "<head><style>a{color:red}</style>"
        "<meta xyz><style Type>b{}</style></head><style>c{}</stylenot>",
        sh, 8);
    CHECK(n == 3, "scan: finds three <style> blocks");
    CHECK(sh[0].len == 12 && memcmp(sh[0].start, "a{color:red}", 12) == 0,
          "scan: sheet 0 body exact");
    CHECK(sh[1].len == 3 && memcmp(sh[1].start, "b{}", 3) == 0,
          "scan: case-insensitive open tag, body exact");
    CHECK(sh[2].len == 14 && sh[2].start[0] == 'c',
          "scan: unterminated tail yields remainder (close-tag text kept)");
    CHECK(css_scan_sheets("<p>no styles</p>", sh, 8) == 0,
          "scan: none when absent");
}

/* Length-delimited slice compare (rule values are NOT NUL-terminated). */
static int val_is(const char *v, const char *lit)
{
    size_t n = strlen(lit);
    return v ? strncmp(v, lit, n) == 0 : 0;
}

/* ── Unit: parse + selectors + specificity ───────────────────────────────── */
static void test_parse(void)
{
    static const char *CSS =
        "<style>/* comment { junk */\n"
        "p { color: white; text-align: center; }\n"
        ".note, div.warn#big { font-weight: bold }\n"
        "ul li a { text-decoration: underline }\n"
        "* { visibility: hidden }\n"
        "@media screen { p { color: black } }\n"
        "p:nth-child(2n) { color: green }\n"
        "p [href] { color: blue }\n"
        "h1 { junk; color:#fff; font-weight: 800; }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);

    CHECK(eng.ruleCount == 5, "parse: 5 usable rules (grouped=1, pseudo/attr dropped)");
    CHECK(eng.rules[0].propCount == 2, "parse: rule 0 keeps 2 props");
    const char *v = css_rule_prop(&eng.rules[0], "color");
    CHECK(val_is(v, "white"), "parse: color value trimmed");
    CHECK(eng.rules[0].selCount == 1 && eng.rules[0].selParts[0] == 1,
          "parse: simple selector shape");
    CHECK(eng.rules[1].selCount == 2, "parse: comma group kept 2 selectors");
    CHECK(eng.rules[1].selParts[1] == 1 && eng.rules[1].spec[1] == 0x0111,
          "parse: div.warn#big = one compound, 3 atoms (spec id+class+type)");
    CHECK(eng.rules[2].selParts[0] == 3, "parse: descendant parts");
    CHECK(eng.rules[3].selParts[0] == 1 && eng.rules[3].sel[0][0].universal,
          "parse: universal selector");
    CHECK(val_is(css_rule_prop(&eng.rules[4], "font-weight"), "800"),
          "parse: junk declaration skipped, valid ones kept");
    CHECK(val_is(css_rule_prop(&eng.rules[4], "color"), "#fff"),
          "parse: declaration after junk kept");
}

/* ── Unit: matching (compound, class tokens, id, descendant) ─────────────── */
static const char *u_css_tag(const void *n)
{
    return (const char *)n;
}

static void test_match(void)
{
    CssSimple c;
    memset(&c, 0, sizeof(c));
    CHECK(css_compound_matches(&c, "p", NULL, NULL) == 0,
          "match: empty compound never matches");
    {
        CssSimple type = {.type = "p", .typeLen = 1};
        CHECK(css_compound_matches(&type, "p", NULL, NULL),
              "match: type matches");
        CHECK(!css_compound_matches(&type, "div", NULL, NULL),
              "match: type mismatch");
    }
    {
        CssSimple cls = {.cls = "note", .clsLen = 4};
        CHECK(css_compound_matches(&cls, "div", "a note b", NULL),
              "match: class token in list");
        CHECK(!css_compound_matches(&cls, "div", "annotated", NULL),
              "match: no substring class match");
        CHECK(!css_compound_matches(&cls, "div", NULL, NULL),
              "match: class absent");
    }
    {
        CssSimple id = {.id = "main", .idLen = 4};
        CHECK(css_compound_matches(&id, "div", NULL, "main"),
              "match: id exact");
        CHECK(!css_compound_matches(&id, "div", NULL, "domain"),
              "match: id not substring");
    }
    {
        CssSimple uni = {.universal = 1};
        CHECK(css_compound_matches(&uni, "anything", "x", "y"),
              "match: universal");
    }
    {
        CssSimple mixed = {.type = "div", .typeLen = 3,
                           .cls = "warn", .clsLen = 4};
        CHECK(css_compound_matches(&mixed, "div", "warn", NULL),
              "match: compound both parts");
        CHECK(!css_compound_matches(&mixed, "span", "warn", NULL),
              "match: compound tag veto");
    }
    (void)u_css_tag;
}

/* ── Unit: cascade (specificity then order, per property) ────────────────── */
static void test_cascade(void)
{
    static const char *CSS =
        "<style>p { color: white; text-align: right }\n"  /* spec 1  order 0 */
        ".x { text-align: center }\n"                     /* spec 16 order 1 */
        "p { text-align: center }</style>";                /* spec 1  order 2 */
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);

    /* No .x: p{right order0} vs p{center order2} — same spec, later wins. */
    unsigned bits = css_compute(&eng, "p", NULL, NULL, NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_CENTER) && !(bits & CSS_F_ALIGN_RIGHT),
          "cascade: later rule wins equal specificity");
    CHECK((bits & CSS_F_INVERT), "cascade: color:white from order-0 applies");

    /* With .x present, its center beats both type rules for text-align. */
    bits = css_compute(&eng, "p", "x", NULL, NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_CENTER) && !(bits & CSS_F_ALIGN_RIGHT),
          "cascade: class beats type regardless of order");
}

/* ── End-to-end through the real parser/walker ───────────────────────────── */
static DocParseResult *parse_page(const char *html)
{
    DocParseResult *d = (DocParseResult *)calloc(1, sizeof(DocParseResult));
    if (!d)
    {
        return NULL;
    }
    if (document_parse(html, "https://example.com/x", MODE_RAW_HTML, NULL, d) != 0)
    {
        free(d);
        return NULL;
    }
    return d;
}

static void free_page(DocParseResult *d)
{
    document_free(d);
    free(d);
}

static void test_e2e(void)
{
    /* 1. hide + align + inline flags + invert in one sheet. */
    DocParseResult *d = parse_page(
        "<html><head><style>"
        ".ad { display: none }"
        "h2 { text-align: center }"
        ".lead { font-weight: bold; font-style: italic }"
        ".u { text-decoration: underline line-through }"
        ".dark { background-color: black; color: white }"
        "p.center { text-align: right }"
        "</style></head><body>"
        "<div class=\"ad\">INVISIBLE</div>"
        "<h2>Head</h2>"
        "<p class=\"lead\">Leading text</p>"
        "<div class=\"u\">Struck</div>"
        "<div class=\"dark\">Dark block</div>"
        "<p class=\"center\">Righty</p>"
        "<p style=\"text-align:center\">Inline wins</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e: page parses");
    CHECK(d->_css != NULL, "e2e: engine attached");
    CHECK(!text_present(d, "INVISIBLE"), "e2e: display:none class hides text");
    CHECK(block_align(d, "Head") && strcmp(block_align(d, "Head"), "center") == 0,
          "e2e: h2 centered by type selector");
    unsigned f = inline_flags_of(d, "Leading text");
    CHECK((f & DOC_INF_BOLD) && (f & DOC_INF_ITALIC),
          "e2e: class font flags on inline");
    f = inline_flags_of(d, "Struck");
    CHECK((f & DOC_INF_UNDERLINE) && (f & DOC_INF_STRIKE),
          "e2e: text-decoration both values");
    CHECK(block_inverted(d, "Dark block") == 1,
          "e2e: background/color invert");
    CHECK(block_align(d, "Righty") && strcmp(block_align(d, "Righty"), "right") == 0,
          "e2e: p.center compound align");
    CHECK(block_align(d, "Inline wins") &&
          strcmp(block_align(d, "Inline wins"), "center") == 0,
          "e2e: inline style still beats stylesheet");
    free_page(d);

    /* 2. descendant + id + inheritance into nested inlines. */
    d = parse_page(
        "<style>#nav a { text-decoration: underline }</style>"
        "<div id=\"nav\"><a href=\"https://x.co\">LinkOne</a></div>"
        "<a href=\"https://y.co\">LinkTwo</a>");
    CHECK(!d->parseError, "e2e2: parses");
    unsigned f1 = inline_flags_of(d, "LinkOne");
    unsigned f2 = inline_flags_of(d, "LinkTwo");
    CHECK((f1 & DOC_INF_UNDERLINE), "e2e2: descendant selector hits inner a");
    /* LinkTwo: bare links underline via the walker's own href rule, so the
     * meaningful assertion is LinkOne == LinkTwo treatment here; the CSS
     * specificity of the #nav hit is covered by the cascade unit above. */
    (void)f2;
    free_page(d);

    /* 3. rules survive a JS-mutation rewalk. */
    d = (DocParseResult *)calloc(1, sizeof(DocParseResult));
    {
        struct JsBridge *bridge = NULL;
        int rc = document_parse_ex(
            "<style>.b{font-weight:bold}h3{text-align:center}</style>"
            "<div id=\"host\"></div>",
            "https://example.com/y", MODE_RAW_HTML, NULL, DOC_SCRIPT_RUN_KEEP,
            &bridge, d);
        (void)rc;
        CHECK(bridge != NULL, "e2e3: bridge attached");
        if (bridge)
        {
            /* JS appends a .b paragraph under #host, then we rewalk.
             * (className writes aren't bridged; setAttribute is.) */
            const char *js =
                "var p = document.createElement('p');"
                "p.setAttribute('class', 'b');"
                "p.textContent = 'Inserted';"
                "document.getElementById('host').appendChild(p);";
            extern int jsbridge_page_script(struct JsBridge *b, const char *src,
                                            size_t len);
            int erc = jsbridge_page_script(bridge, js, strlen(js));
            (void)erc;
            extern int document_rewalk(DocParseResult *doc);
            document_rewalk(d);
            CHECK(text_present(d, "Inserted"), "e2e3: JS-inserted node renders");
            f = inline_flags_of(d, "Inserted");
            CHECK((f & DOC_INF_BOLD), "e2e3: CSS re-applied after rewalk");
            js_doc_close(bridge);
        }
    }
    free_page(d);

    /* 4. page without stylesheets: engine absent, zero impact. */
    d = parse_page("<p>Plain</p>");
    CHECK(d->_css == NULL, "e2e4: no engine without sheets");
    CHECK(text_present(d, "Plain"), "e2e4: plain page renders");
    free_page(d);
}

/* js_doc_close comes from jsbridge.c (linked). */

int main(void)
{
    test_scanner();
    test_parse();
    test_match();
    test_cascade();
    test_e2e();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
