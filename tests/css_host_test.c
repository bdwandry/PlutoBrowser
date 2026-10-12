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

/* R16b: jsext's identity-encoding switch — no-op under the fake HTTP layer. */
void http_set_identity_encoding(int on) { (void)on; }
/* Host stubs for references added after the harness was written (never
 * exercised here): jsext's load-screen progress hook, and the font
 * accessor used by the XS-NR overlay helper. */
void pluto_ui_net_progress(int cur, int total)
{
    (void)cur;
    (void)total;
}
struct LCDFont;
struct LCDFont *style_font(int role)
{
    (void)role;
    return NULL;
}
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

/* Render helpers: find a block's concatenated text / its align. */static const char *block_align(const DocParseResult *doc, const char *needle)
{
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        if ((b->type == DOC_BLOCK_PARAGRAPH || b->type == DOC_BLOCK_HEADING ||
             b->type == DOC_BLOCK_LIST_ITEM) &&
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

static int block_border(const DocParseResult *doc, const char *needle)
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
                return b->border;
            }
        }
    }
    return -1;
}

static int block_index(const DocParseResult *doc, const char *needle)
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
                return i;
            }
        }
    }
    return -1;
}

static int block_marker(const DocParseResult *doc, const char *needle){
    for (int i = 0; i < doc->blockCount; i++)
    {
        DocBlock *b = doc->blocks[i];
        if (b->type != DOC_BLOCK_LIST_ITEM)
        {
            continue;
        }
        for (int j = 0; j < b->inlineCount; j++)
        {
            DocInline *in = b->inlines[j];
            if (in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return b->markerType;
            }
        }
    }
    return 0;
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
        "h1 { junk; color:#fff; font-weight: 800; }</style>";    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);

    CHECK(eng.ruleCount == 8, "parse: 8 usable rules (@media screen holds)");
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
    CHECK(eng.rules[5].sel[0][0].pclass == 5 &&
              eng.rules[5].sel[0][0].nthA == 2 &&
              eng.rules[5].sel[0][0].nthB == 0,
          "parse: :nth-child(2n) functional");
    CHECK(val_is(css_rule_prop(&eng.rules[7], "font-weight"), "800"),
          "parse: junk declaration skipped, valid ones kept");
    CHECK(val_is(css_rule_prop(&eng.rules[7], "color"), "#fff"),
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
    CHECK(css_compound_matches(&c, "p", NULL, NULL, NULL, NULL) == 0,
          "match: empty compound never matches");
    {
        CssSimple type = {.type = "p", .typeLen = 1};
        CHECK(css_compound_matches(&type, "p", NULL, NULL, NULL, NULL),
              "match: type matches");
        CHECK(!css_compound_matches(&type, "div", NULL, NULL, NULL, NULL),
              "match: type mismatch");
    }
    {
        CssSimple cls = {.cls = "note", .clsLen = 4};
        CHECK(css_compound_matches(&cls, "div", "a note b", NULL, NULL, NULL),
              "match: class token in list");
        CHECK(!css_compound_matches(&cls, "div", "annotated", NULL, NULL, NULL),
              "match: no substring class match");
        CHECK(!css_compound_matches(&cls, "div", NULL, NULL, NULL, NULL),
              "match: class absent");
    }
    {
        CssSimple id = {.id = "main", .idLen = 4};
        CHECK(css_compound_matches(&id, "div", NULL, "main", NULL, NULL),
              "match: id exact");
        CHECK(!css_compound_matches(&id, "div", NULL, "domain", NULL, NULL),
              "match: id not substring");
    }
    {
        CssSimple uni = {.universal = 1};
        CHECK(css_compound_matches(&uni, "anything", "x", "y", NULL, NULL),
              "match: universal");
    }
    {
        CssSimple mixed = {.type = "div", .typeLen = 3,
                           .cls = "warn", .clsLen = 4};
        CHECK(css_compound_matches(&mixed, "div", "warn", NULL, NULL, NULL),
              "match: compound both parts");
        CHECK(!css_compound_matches(&mixed, "span", "warn", NULL, NULL, NULL),
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

/* ── Unit: attribute selectors + UA [hidden] ────────────────────────────── */
typedef struct
{
    const char *name;
    const char *val; /* NULL = absent */
} TAttrRow;

static const TAttrRow *t_rows;
static int t_rowN;

static const char *t_dom_attr(const void *node, const char *name, size_t n)
{
    (void)node;
    for (int i = 0; i < t_rowN; i++)
    {
        if (strlen(t_rows[i].name) == n && memcmp(t_rows[i].name, name, n) == 0)
        {
            return t_rows[i].val;
        }
    }
    return NULL;
}

static CssDom t_dom(void)
{
    CssDom d;
    d.ancestor = NULL;
    d.attr = t_dom_attr;
    return d;
}

static void test_attr(void)
{
    static const char *CSS =
        "<style>[hidden] { color: white }\n"
        "input[type=\"text\"] { font-weight: bold }\n"
        "[data-x='v y'] { text-align: center }\n"
        "[a=b] { text-decoration: underline }\n"
        "div.note#big[data-k=\"1\"] { visibility: hidden }\n"
        "[a~=b] { color: white }\n"
        "[] { color: white }\n"
        "[=b] { color: white }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 5, "attr: 5 usable rules (operators/empty dropped)");
    CHECK(eng.rules[0].selParts[0] == 1 &&
              eng.rules[0].sel[0][0].attrLen == 6 &&
              eng.rules[0].sel[0][0].attrVal == NULL,
          "attr: [hidden] presence shape");
    CHECK(eng.rules[1].sel[0][0].typeLen == 5 &&
              eng.rules[1].sel[0][0].attrLen == 4 &&
              val_is(eng.rules[1].sel[0][0].attrVal, "text"),
          "attr: type + exact quoted value");
    CHECK(val_is(eng.rules[2].sel[0][0].attrVal, "v y"),
          "attr: single-quoted value with space");
    CHECK(val_is(eng.rules[3].sel[0][0].attrVal, "b"),
          "attr: unquoted value");
    CHECK(eng.rules[4].spec[0] == 0x0121,
          "attr: div.note#big[data-k] spec = type+class+attr+id");

    /* Matching through the fake adapter. */
    CssDom d = t_dom();
    {
        static const TAttrRow rows[] = {{"hidden", ""}};
        t_rows = rows;
        t_rowN = 1;
        CssSimple c;
        memset(&c, 0, sizeof(c));
        c.attr = "hidden";
        c.attrLen = 6;
        CHECK(css_compound_matches(&c, "div", NULL, NULL, &d, (void *)1),
              "attr: presence matches");
        CHECK(!css_compound_matches(&c, "div", NULL, NULL, NULL, NULL),
              "attr: no DOM hook never matches");
    }
    {
        static const TAttrRow rows[] = {{"type", "text"}};
        t_rows = rows;
        t_rowN = 1;
        CHECK(css_compound_matches(&eng.rules[1].sel[0][0], "input", NULL,
                                   NULL, &d, (void *)1),
              "attr: exact value matches");
    }
    {
        static const TAttrRow rows[] = {{"type", "password"}};
        t_rows = rows;
        t_rowN = 1;
        CHECK(!css_compound_matches(&eng.rules[1].sel[0][0], "input", NULL,
                                    NULL, &d, (void *)1),
              "attr: value mismatch vetoes");
    }
    {
        /* The engine passes the raw selector name; the real DOM adapter
         * lowercases it (DOM keys are stored lowercase). The fake adapter
         * does not, so a non-lowered name misses — proving the name flows
         * through to the hook verbatim. */
        static const TAttrRow rows[] = {{"type", "text"}};
        t_rows = rows;
        t_rowN = 1;
        CssSimple c;
        memset(&c, 0, sizeof(c));
        c.attr = "TYPE";
        c.attrLen = 4;
        CHECK(!css_compound_matches(&c, "div", NULL, NULL, &d, (void *)1),
              "attr: raw name reaches the hook (real one lowercases)");
        c.attr = "type";
        CHECK(css_compound_matches(&c, "div", NULL, NULL, &d, (void *)1),
              "attr: lowered name hits");
    }

    /* UA origin: bare [hidden] hides with no author CSS at all. */
    {
        static const TAttrRow rows[] = {{"hidden", ""}};
        t_rows = rows;
        t_rowN = 1;
        CssEngine empty;
        memset(&empty, 0, sizeof(empty));
        unsigned bits = css_compute(&empty, "div", NULL, NULL, &d, (void *)1);
        CHECK((bits & CSS_F_HIDDEN), "attr: UA [hidden] hides authorless");
        t_rowN = 0;
        bits = css_compute(&empty, "div", NULL, NULL, &d, (void *)1);
        CHECK(!(bits & CSS_F_HIDDEN), "attr: no hidden attr, no hide");
    }
    /* Any author display re-shows, regardless of specificity. */
    {
        static const TAttrRow rows[] = {{"hidden", ""}};
        t_rows = rows;
        t_rowN = 1;
        static const char *CSS2 =
            "<style>div { display: block }</style>";
        (void)css_scan_sheets(CSS2, sh, 1);
        css_parse_sheets(&eng, sh, 1);
        unsigned bits = css_compute(&eng, "div", NULL, NULL, &d, (void *)1);
        CHECK(!(bits & CSS_F_HIDDEN),
              "attr: author type display:block beats UA [hidden]");
        static const char *CSS3 =
            "<style>[hidden] { display: block }</style>";
        (void)css_scan_sheets(CSS3, sh, 1);
        css_parse_sheets(&eng, sh, 1);
        bits = css_compute(&eng, "div", NULL, NULL, &d, (void *)1);
        CHECK(!(bits & CSS_F_HIDDEN),
              "attr: author [hidden] display:block re-shows");
        static const char *CSS4 =
            "<style>.z { color: white }</style>";
        (void)css_scan_sheets(CSS4, sh, 1);
        css_parse_sheets(&eng, sh, 1);
        bits = css_compute(&eng, "div", "z", NULL, &d, (void *)1);
        CHECK((bits & CSS_F_HIDDEN) && (bits & CSS_F_INVERT),
              "attr: non-display author rule keeps UA hide");
        t_rowN = 0;
    }
}

/* ── Unit: child combinator ─────────────────────────────────────────────── */
typedef struct
{
    const char *tag;
    const char *cls;
    const char *id;
} TAnc;

static const TAnc *t_ancs;
static int t_ancN;

static const void *t_dom_ancestor(const void *node, int i,
                                  const char **tagLower,
                                  const char **classAttr,
                                  const char **idAttr)
{
    (void)node;
    if (i < 0 || i >= t_ancN)
    {
        return NULL;
    }
    *tagLower = t_ancs[i].tag;
    *classAttr = t_ancs[i].cls;
    *idAttr = t_ancs[i].id;
    return (const void *)(t_ancs + i);
}

static void test_child(void)
{
    static const char *CSS =
        "<style>div>p { color: white }\n"
        "div > p { text-align: center }\n"
        "a>b>c { font-weight: bold }\n"
        "div> { color: white }\n"
        ">p { color: white }\n"
        "div>>p { color: white }\n"
        "div+p { color: white }\n"
        "div~p { color: white }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 3, "child: 3 usable (bad/leading/double/sibling out)");
    CHECK(eng.rules[0].selParts[0] == 2 && eng.rules[0].selComb[0][1] == 1,
          "child: div>p parts + combinator");
    CHECK(eng.rules[1].selParts[0] == 2 && eng.rules[1].selComb[0][1] == 1,
          "child: spaced div > p identical");
    CHECK(eng.rules[2].selParts[0] == 3 && eng.rules[2].selComb[0][1] == 1 &&
              eng.rules[2].selComb[0][2] == 1,
          "child: a>b>c chain");

    /* Matching: canned ancestors, nearest-first. */
    CssDom d;
    d.ancestor = t_dom_ancestor;
    d.attr = NULL;
    {
        /* <div><p>: parent IS div. */
        static const TAnc ancs[] = {{"div", NULL, NULL}, {"body", NULL, NULL}};
        t_ancs = ancs;
        t_ancN = 2;
        unsigned bits = css_compute(&eng, "p", NULL, NULL, &d, (void *)1);
        CHECK((bits & CSS_F_INVERT) && (bits & CSS_F_ALIGN_CENTER),
              "child: parent match hits both rules");
    }
    {
        /* <div><span><p>: parent is span, div is grandparent. */
        static const TAnc ancs[] = {{"span", NULL, NULL},
                                    {"div", NULL, NULL}};
        t_ancs = ancs;
        t_ancN = 2;
        unsigned bits = css_compute(&eng, "p", NULL, NULL, &d, (void *)1);
        CHECK(!(bits & CSS_F_INVERT) && !(bits & CSS_F_ALIGN_CENTER),
              "child: grandparent does NOT satisfy >");
    }
    {
        /* Descendant still reaches through: ul li control. */
        static const char *CSS2 = "<style>div p { color: white }</style>";
        (void)css_scan_sheets(CSS2, sh, 1);
        css_parse_sheets(&eng, sh, 1);
        static const TAnc ancs[] = {{"span", NULL, NULL},
                                    {"div", NULL, NULL}};
        t_ancs = ancs;
        t_ancN = 2;
        unsigned bits = css_compute(&eng, "p", NULL, NULL, &d, (void *)1);
        CHECK((bits & CSS_F_INVERT), "child: descendant unaffected");
    }
    {
        /* Mixed: div > p span — p at any depth, div pinned to p's parent. */
        static const char *CSS3 = "<style>div > p span { color: white }</style>";
        (void)css_scan_sheets(CSS3, sh, 1);
        css_parse_sheets(&eng, sh, 1);
        /* span <- p <- div: HIT */
        static const TAnc hit[] = {{"p", NULL, NULL}, {"div", NULL, NULL}};
        t_ancs = hit;
        t_ancN = 2;
        unsigned bits = css_compute(&eng, "span", NULL, NULL, &d, (void *)1);
        CHECK((bits & CSS_F_INVERT), "child: mixed chain hits");
        /* span <- p <- section <- div: p's parent is section, not div. */
        static const TAnc miss[] = {{"p", NULL, NULL},
                                    {"section", NULL, NULL},
                                    {"div", NULL, NULL}};
        t_ancs = miss;
        t_ancN = 3;
        bits = css_compute(&eng, "span", NULL, NULL, &d, (void *)1);
        CHECK(!(bits & CSS_F_INVERT), "child: mixed chain misses past parent");
    }
    t_ancN = 0;
}

/* ── Unit: text-transform + white-space bits ────────────────────────────── */
static void test_text(void)
{
    static const char *CSS =
        "<style>.u { text-transform: uppercase }\n"
        ".l { text-transform: lowercase }\n"
        ".c { text-transform: capitalize }\n"
        ".n { text-transform: none }\n"
        ".p { white-space: pre }\n"
        ".w { white-space: pre-wrap }\n"
        ".pl { white-space: pre-line }\n"
        ".nw { white-space: nowrap }\n"
        ".nm { white-space: normal }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 9, "text: 9 rules parse");
    CHECK(css_compute(&eng, "p", "u", NULL, NULL, NULL) & CSS_F_UPPER,
          "text: uppercase bit");
    CHECK(css_compute(&eng, "p", "l", NULL, NULL, NULL) & CSS_F_LOWER,
          "text: lowercase bit");
    CHECK(css_compute(&eng, "p", "c", NULL, NULL, NULL) & CSS_F_CAP,
          "text: capitalize bit");
    CHECK(!(css_compute(&eng, "p", "n", NULL, NULL, NULL) &
            (CSS_F_UPPER | CSS_F_LOWER | CSS_F_CAP)),
          "text: none sets nothing");
    CHECK(css_compute(&eng, "p", "p", NULL, NULL, NULL) & CSS_F_PRE,
          "text: pre bit");
    CHECK(css_compute(&eng, "p", "w", NULL, NULL, NULL) & CSS_F_PRE,
          "text: pre-wrap maps to pre");
    CHECK(css_compute(&eng, "p", "pl", NULL, NULL, NULL) & CSS_F_PRELINE,
          "text: pre-line bit");
    CHECK(css_compute(&eng, "p", "nw", NULL, NULL, NULL) & CSS_F_NOWRAP,
          "text: nowrap bit");
    CHECK(!(css_compute(&eng, "p", "nm", NULL, NULL, NULL) &
            (CSS_F_PRE | CSS_F_PRELINE | CSS_F_NOWRAP)),
          "text: normal sets nothing");
}

/* ── Unit: ::before/::after ─────────────────────────────────────────────── */
static void test_pseudo(void)
{
    static const char *CSS =
        "<style>p::before { content: \"A\" }\n"
        "div > p::after { content: \"B\" }\n"
        "::before { content: \"C\" }\n"
        "span:before { content: \"D\" }\n"
        "p:hover::before { content: \"E\" }\n"
        "p::before, q::after { content: \"F\" }\n"
        "p::marker { content: \"G\" }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    /* Usable: p::before, div>p::after, bare ::before, span:before,
     * mixed group (first pseudo wins). Dropped: :hover form, ::marker. */
    CHECK(eng.ruleCount == 5, "pseudo: 5 usable rules");
    CHECK(eng.rules[0].pseudo == 1 && eng.rules[0].spec[0] == 2,
          "pseudo: p::before flag + type-level specificity");
    CHECK(eng.rules[1].pseudo == 2 && eng.rules[1].selParts[0] == 2 &&
              eng.rules[1].selComb[0][1] == 1,
          "pseudo: child chain preserved under ::after");
    CHECK(eng.rules[2].pseudo == 1 && eng.rules[2].sel[0][0].universal,
          "pseudo: bare ::before = universal");
    CHECK(eng.rules[3].pseudo == 1, "pseudo: legacy :before");
    CHECK(eng.rules[4].pseudo == 1 && eng.rules[4].selCount == 1,
          "pseudo: mixed group keeps first class only");

    /* Element matching never sees pseudo rules. */
    CHECK(!css_rule_matches(&eng.rules[0], "p", NULL, NULL, NULL, NULL),
          "pseudo: no element match");
    unsigned bits = css_compute(&eng, "p", NULL, NULL, NULL, NULL);
    CHECK(!(bits & CSS_F_HIDDEN), "pseudo: compute unaffected");

    /* Content cascade: specificity, order, importance. */
    static const char *CSS2 =
        "<style>p::before { content: \"low\" }\n"
        ".q::before { content: \"high\" }\n"
        "p::after { content: \"a1\" }\n"
        "p::after { content: \"a2\" }\n"
        ".z::before { content: \"norm\" }\n"
        ".z::before { content: \"imp\" !important }</style>";
    (void)css_scan_sheets(CSS2, sh, 1);
    css_parse_sheets(&eng, sh, 1);
    size_t n = 0;
    /* NOTE: winners are RAW extents (quotes included) — the walker
     * strips quotes/attr() in walk_pseudo_text (covered by e2e9). */
    const char *v = css_pseudo_content(&eng, "p", "q", NULL, NULL, NULL,
                                       1, &n);
    CHECK(v && n == 6 && memcmp(v, "\"high\"", 6) == 0,
          "pseudo: class beats type for content");
    v = css_pseudo_content(&eng, "p", NULL, NULL, NULL, NULL, 0, &n);
    CHECK(v && n == 4 && memcmp(v, "\"a2\"", 4) == 0,
          "pseudo: later ::after wins");
    v = css_pseudo_content(&eng, "p", "z", NULL, NULL, NULL, 1, &n);
    CHECK(v && n == 5 && memcmp(v, "\"imp\"", 5) == 0,
          "pseudo: !important content wins");
    v = css_pseudo_content(&eng, "div", "z", NULL, NULL, NULL, 0, &n);
    CHECK(v == NULL, "pseudo: no matching after rule = NULL");
}

/* ── Unit: structural pseudo-classes ────────────────────────────────────── */
/* Fake siblings are 1-based POINTERS ((void*)1..) mapping to 0-based
 * indices — node (void*)0 would collide with the NULL guard. */
static int t_sibN = 3; /* indices 0..t_sibN-1 */

static const void *t_sibling(const void *node, int dir)
{
    intptr_t k = (intptr_t)node - 1;
    intptr_t j = k + dir;
    if (k < 0 || k >= t_sibN || j < 0 || j >= t_sibN)
    {
        return NULL;
    }
    return (const void *)(j + 1);
}

static CssDom t_dom_sib(void)
{
    CssDom d;
    d.ancestor = NULL;
    d.attr = t_dom_attr;
    d.sibling = t_sibling;
    return d;
}

static void test_pclass(void)
{
    static const char *CSS =
        "<style>li:first-child { color: white }\n"
        "li:last-child { text-align: center }\n"
        "li:only-child { font-weight: bold }\n"
        "a:link { text-decoration: underline }\n"
        "p:nth-child(odd) { color: white }\n"
        "p:nth-child(even) { text-align: center }\n"
        "p:nth-child(3) { font-weight: bold }\n"
        "p:nth-child(2n+1) { text-decoration: underline }\n"
        "p:hover { color: white }\n"
        "p:visited { color: white }\n"
        "p:nth-child(x) { color: white }\n"
        "p:nth-child { color: white }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 8, "pclass: 8 usable (hover/visited/bad-nth out)");
    CHECK(eng.rules[0].sel[0][0].pclass == 1 &&
              eng.rules[0].spec[0] == 0x0011,
          "pclass: first-child shape + class-level specificity");
    CHECK(eng.rules[3].sel[0][0].pclass == 4, "pclass: :link shape");
    CHECK(eng.rules[4].sel[0][0].pclass == 5 &&
              eng.rules[4].sel[0][0].nthA == 2 &&
              eng.rules[4].sel[0][0].nthB == 1,
          "pclass: odd = (2,1)");
    CHECK(eng.rules[5].sel[0][0].nthA == 2 &&
              eng.rules[5].sel[0][0].nthB == 0,
          "pclass: even = (2,0)");
    CHECK(eng.rules[6].sel[0][0].nthA == 0 &&
              eng.rules[6].sel[0][0].nthB == 3,
          "pclass: N = (0,N)");
    CHECK(eng.rules[7].sel[0][0].nthA == 2 &&
              eng.rules[7].sel[0][0].nthB == 1,
          "pclass: 2n+1 = (2,1)");

    /* Matching: node 1 = first of 3, node 2 = middle, node 3 = last. */
    CssDom d = t_dom_sib();
    t_sibN = 3;
    t_rowN = 0;
    unsigned b0 = css_compute(&eng, "li", NULL, NULL, &d, (void *)1);
    unsigned b1 = css_compute(&eng, "li", NULL, NULL, &d, (void *)2);
    unsigned b2 = css_compute(&eng, "li", NULL, NULL, &d, (void *)3);
    CHECK((b0 & CSS_F_INVERT), "pclass: first-child hits node 0");
    CHECK(!(b1 & CSS_F_INVERT) && !(b2 & CSS_F_INVERT),
          "pclass: first-child misses rest");
    CHECK((b2 & CSS_F_ALIGN_CENTER) && !(b0 & CSS_F_ALIGN_CENTER),
          "pclass: last-child hits node 2 only");
    CHECK(!(b0 & CSS_F_BOLD) && !(b1 & CSS_F_BOLD) && !(b2 & CSS_F_BOLD),
          "pclass: only-child misses a trio");
    t_sibN = 1;
    unsigned bs = css_compute(&eng, "li", NULL, NULL, &d, (void *)1);
    CHECK((bs & CSS_F_BOLD), "pclass: only-child hits a singleton");
    /* nth across 4: odd = 1,3; even = 2,4; (3) = 3rd; 2n+1 = 1,3. */
    t_sibN = 4;
    unsigned p0 = css_compute(&eng, "p", NULL, NULL, &d, (void *)1);
    unsigned p1 = css_compute(&eng, "p", NULL, NULL, &d, (void *)2);
    unsigned p2 = css_compute(&eng, "p", NULL, NULL, &d, (void *)3);
    unsigned p3 = css_compute(&eng, "p", NULL, NULL, &d, (void *)4);
    CHECK((p0 & CSS_F_INVERT) && !(p1 & CSS_F_INVERT) &&
              (p2 & CSS_F_INVERT) && !(p3 & CSS_F_INVERT),
          "pclass: odd positions");
    CHECK(!(p0 & CSS_F_ALIGN_CENTER) && (p1 & CSS_F_ALIGN_CENTER) &&
              !(p2 & CSS_F_ALIGN_CENTER) && (p3 & CSS_F_ALIGN_CENTER),
          "pclass: even positions");
    CHECK(!(p0 & CSS_F_BOLD) && !(p1 & CSS_F_BOLD) &&
              (p2 & CSS_F_BOLD) && !(p3 & CSS_F_BOLD),
          "pclass: (3) hits third only");
    CHECK((p0 & CSS_F_UNDERLINE) && !(p1 & CSS_F_UNDERLINE) &&
              (p2 & CSS_F_UNDERLINE) && !(p3 & CSS_F_UNDERLINE),
          "pclass: 2n+1 mirrors odd");
    /* :link needs href. */
    {
        static const TAttrRow rows[] = {{"href", "https://x.co/"}};
        t_rows = rows;
        t_rowN = 1;
        unsigned bl = css_compute(&eng, "a", NULL, NULL, &d, (void *)1);
        CHECK((bl & CSS_F_UNDERLINE), "pclass: :link hits href");
        t_rowN = 0;
        bl = css_compute(&eng, "a", NULL, NULL, &d, (void *)1);
        CHECK(!(bl & CSS_F_UNDERLINE), "pclass: :link misses href-less");
    }
    t_sibN = 3;
    t_rowN = 0;
}

/* ── Unit: @media screening ────────────────────────────────────────────── */
static int media_present(const char *css, const char *needle)
{
    CssSheet sh[1];
    (void)css_scan_sheets(css, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    for (int i = 0; i < eng.ruleCount; i++)
    {
        for (unsigned char pj = 0; pj < eng.rules[i].propCount; pj++)
        {
            if (val_is(eng.rules[i].val[pj], needle))
            {
                return 1;
            }
        }
    }
    return 0;
}

static void test_media(void)
{
    /* Viewport is 400x240 landscape, 1-bit gray, light default. */
    CHECK(media_present("<style>@media screen { p { color: yes1 } }</style>",
                        "yes1"),
          "media: screen holds");
    CHECK(media_present("<style>@media print { p { color: yes2 } }</style>",
                        "yes2"),
          "media: print holds (1-bit honors print)");
    CHECK(media_present("<style>@media all { p { color: yes3 } }</style>",
                        "yes3"),
          "media: all holds");
    CHECK(!media_present("<style>@media speech { p { color: no1 } }</style>",
                         "no1"),
          "media: speech dropped");
    CHECK(!media_present("<style>@media tv { p { color: no2 } }</style>",
                         "no2"),
          "media: unknown type dropped");
    CHECK(media_present("<style>@media (max-width: 400px) { p { color: yes4 } }</style>",
                        "yes4"),
          "media: max-width:400px holds at 400");
    CHECK(!media_present("<style>@media (max-width: 399px) { p { color: no3 } }</style>",
                         "no3"),
          "media: max-width:399px drops");
    CHECK(media_present("<style>@media (min-width: 400px) { p { color: yes5 } }</style>",
                        "yes5"),
          "media: min-width:400px holds");
    CHECK(!media_present("<style>@media (min-width: 401px) { p { color: no4 } }</style>",
                         "no4"),
          "media: min-width:401px drops");
    CHECK(media_present("<style>@media screen and (max-width: 500px) { p { color: yes6 } }</style>",
                        "yes6"),
          "media: type and feature");
    CHECK(!media_present("<style>@media screen and (max-width: 100px) { p { color: no5 } }</style>",
                         "no5"),
          "media: failing feature drops");
    CHECK(media_present("<style>@media speech, screen { p { color: yes7 } }</style>",
                        "yes7"),
          "media: comma OR");
    CHECK(media_present("<style>@media not speech { p { color: yes8 } }</style>",
                        "yes8"),
          "media: not negates");
    CHECK(!media_present("<style>@media not screen { p { color: no6 } }</style>",
                         "no6"),
          "media: not screen drops");
    CHECK(media_present("<style>@media (orientation: landscape) { p { color: yes9 } }</style>",
                        "yes9"),
          "media: landscape");
    CHECK(!media_present("<style>@media (orientation: portrait) { p { color: no7 } }</style>",
                         "no7"),
          "media: portrait drops");
    CHECK(media_present("<style>@media (monochrome) { p { color: yes10 } }</style>",
                        "yes10"),
          "media: monochrome bare holds (1-bit)");
    CHECK(!media_present("<style>@media (color) { p { color: no8 } }</style>",
                         "no8"),
          "media: color bare drops (0 bits)");
    CHECK(media_present("<style>@media (color: 0) { p { color: yes11 } }</style>",
                        "yes11"),
          "media: color:0 holds");
    CHECK(media_present("<style>@media (prefers-color-scheme: light) { p { color: yes12 } }</style>",
                        "yes12"),
          "media: light scheme holds");
    CHECK(!media_present("<style>@media (prefers-color-scheme: dark) { p { color: no9 } }</style>",
                         "no9"),
          "media: dark scheme drops");
    CHECK(!media_present("<style>@media (max-weight: 1kg) { p { color: no10 } }</style>",
                         "no10"),
          "media: unknown feature drops");
    CHECK(media_present("<style>@media only screen { p { color: yes13 } }</style>",
                        "yes13"),
          "media: only- ignored");
    /* Nesting ANDs; source order preserved (media rule between siblings). */
    {
        CssSheet sh[1];
        (void)css_scan_sheets(
            "<style>p { color: first } "
            "@media screen { @media (max-width: 400px) { p { color: nested } } } "
            "@media speech { p { color: gone } } "
            "p { color: last }</style>",
            sh, 1);
        CssEngine eng;
        css_parse_sheets(&eng, sh, 1);
        CHECK(eng.ruleCount == 3, "media: nested holds, speech drops");
        if (eng.ruleCount == 3)
        {
            CHECK(val_is(eng.rules[0].val[0], "first") &&
                      val_is(eng.rules[1].val[0], "nested") &&
                      val_is(eng.rules[2].val[0], "last"),
                  "media: order preserved across flatten");
        }
    }
    /* Other at-rules still skip whole. */
    CHECK(!media_present("<style>@font-face { font-family: x; src: no11; } p { color: yes14 }</style>",
                         "no11") &&
              media_present("<style>@font-face { font-family: x; src: no11; } p { color: yes14 }</style>",
                            "yes14"),
          "media: @font-face skipped, neighbors survive");
}

/* ── Unit: border shorthand + px values ─────────────────────────────────── */
static void test_border(void)
{
    CHECK(css_px_value("2px", 3) == 2, "px: plain");
    CHECK(css_px_value("12px solid black", 15) == 12, "px: first in shorthand");
    CHECK(css_px_value("solid", 5) == -1, "px: absent");
    CHECK(css_px_value("1.5px", 5) == 1, "px: fraction dropped");
    CHECK(css_px_value("-2px", 4) == 0, "px: negative reads 0");
    CHECK(css_px_value("2em", 3) == -1, "px: em is not px");
    CHECK(css_px_value(NULL, 0) == -1, "px: null safe");

    static const char *CSS =
        "<style>.a { border: 2px solid black }\n"
        ".b { border-width: 3px }\n"
        ".c { border: none }\n"
        ".d { border-width: 1px; border: 3px dashed red }\n"
        ".e { border: solid }\n"
        ".f { border-style: none }\n"
        ".g { border-width: 9px }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 7, "border: 7 rules");
    /* Shorthand fans out (no bare "border" entry survives). */
    CHECK(css_rule_prop(&eng.rules[0], "border") == NULL,
          "border: shorthand itself not stored");
    size_t n = 0;
    const char *v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL,
                                      "border-width", &n);
    /* Fanned value is the full shorthand; the walker scans Npx out. */
    CHECK(v && n == 15 && memcmp(v, "2px solid black", 15) == 0 &&
              css_px_value(v, n) == 2,
          "border: width fanned out");
    v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL, "border-style",
                          &n);
    CHECK(v && css_val_has(v, n, "solid"), "border: style fanned out");
    /* Longhand vs shorthand cascade by order (equal specificity). */
    v = css_winning_value(&eng, "p", "d", NULL, NULL, NULL, "border-width",
                          &n);
    CHECK(v && memcmp(v, "3px", 3) == 0, "border: shorthand later wins");
    v = css_winning_value(&eng, "p", "c", NULL, NULL, NULL, "border-width",
                          &n);
    CHECK(v && n == 4 && memcmp(v, "none", 4) == 0,
          "border: none fans out");
    v = css_winning_value(&eng, "p", "g", NULL, NULL, NULL, "border-width",
                          &n);
    CHECK(v && n == 3 && memcmp(v, "9px", 3) == 0,
          "border: raw 9px kept (walker clamps)");
}

/* ── Unit: margin/padding fan-out ───────────────────────────────────────── */
static void test_spacing(void)
{
    static const char *CSS =
        "<style>.a { margin: 10px 20px 30px 40px }\n"
        ".b { margin: 5px 6px }\n"
        ".c { margin: 7px }\n"
        ".d { margin: 1px 2px 3px }\n"
        ".e { padding: 8px }\n"
        ".f { margin: 1px 2px 3px 4px 5px }\n"
        ".g { margin-top: 9px }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 7, "spacing: 7 rules (5-token margin kept as rule)");
    CHECK(css_rule_prop(&eng.rules[0], "margin") == NULL,
          "spacing: shorthand itself not stored");
    size_t n = 0;
    const char *v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL,
                                      "margin-top", &n);
    CHECK(v && n == 4 && memcmp(v, "10px", 4) == 0, "spacing: 4-token top");
    v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL, "margin-right",
                          &n);
    CHECK(v && memcmp(v, "20px", 4) == 0, "spacing: 4-token right");
    v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL, "margin-bottom",
                          &n);
    CHECK(v && memcmp(v, "30px", 4) == 0, "spacing: 4-token bottom");
    v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL, "margin-left",
                          &n);
    CHECK(v && memcmp(v, "40px", 4) == 0, "spacing: 4-token left");
    v = css_winning_value(&eng, "p", "b", NULL, NULL, NULL, "margin-top",
                          &n);
    CHECK(v && memcmp(v, "5px", 3) == 0, "spacing: 2-token tb");
    v = css_winning_value(&eng, "p", "b", NULL, NULL, NULL, "margin-right",
                          &n);
    CHECK(v && memcmp(v, "6px", 3) == 0, "spacing: 2-token rl");
    v = css_winning_value(&eng, "p", "c", NULL, NULL, NULL, "margin-bottom",
                          &n);
    CHECK(v && memcmp(v, "7px", 3) == 0, "spacing: 1-token all");
    v = css_winning_value(&eng, "p", "d", NULL, NULL, NULL, "margin-bottom",
                          &n);
    CHECK(v && memcmp(v, "3px", 3) == 0, "spacing: 3-token bottom");
    v = css_winning_value(&eng, "p", "e", NULL, NULL, NULL, "padding-top",
                          &n);
    CHECK(v && memcmp(v, "8px", 3) == 0, "spacing: padding fans out");
    /* .f (5 tokens): invalid, no longhands — but the RULE survives. */
    v = css_winning_value(&eng, "p", "f", NULL, NULL, NULL, "margin-top",
                          &n);
    CHECK(v == NULL, "spacing: >4 tokens drops expansion");
    v = css_winning_value(&eng, "p", "g", NULL, NULL, NULL, "margin-top",
                          &n);
    CHECK(v && memcmp(v, "9px", 3) == 0, "spacing: longhand direct");
}

/* ── Unit: break-word splitter ──────────────────────────────────────────── */
static int t_measure(void *ctx, const char *s)
{
    (void)ctx;
    return (int)strlen(s) * 8; /* 8px monospace fiction */
}

typedef struct
{
    char got[8][32];
    int px[8];
    int last[8];
    int n;
} TSplit;

static void t_emit(void *ectx, const char *pc, size_t n, int w, int isLast)
{
    TSplit *t = (TSplit *)ectx;
    if (t->n < 8)
    {
        size_t k = n < 31 ? n : 31;
        memcpy(t->got[t->n], pc, k);
        t->got[t->n][k] = '\0';
        t->px[t->n] = w;
        t->last[t->n] = isLast;
        t->n++;
    }
}

static void test_split(void)
{
    TSplit t;
    memset(&t, 0, sizeof(t));
    int n = doc_split_long_word("abcdef", 20, t_measure, NULL, t_emit, &t);
    CHECK(n == 3 && t.n == 3, "split: 3 pieces");
    CHECK(strcmp(t.got[0], "ab") == 0 && strcmp(t.got[1], "cd") == 0 &&
              strcmp(t.got[2], "ef") == 0,
          "split: 2-char pieces at 8px/20px");
    CHECK(t.px[0] == 16 && t.last[2] == 1 && t.last[0] == 0,
          "split: widths + last flag");
    memset(&t, 0, sizeof(t));
    n = doc_split_long_word("abc", 100, t_measure, NULL, t_emit, &t);
    CHECK(n == 1 && strcmp(t.got[0], "abc") == 0 && t.last[0] == 1,
          "split: fitting word whole");
    memset(&t, 0, sizeof(t));
    n = doc_split_long_word("abcdef", 4, t_measure, NULL, t_emit, &t);
    CHECK(n == 6, "split: sub-glyph maxW still progresses");
    /* UTF-8 safety: é is 2 bytes; no piece may start with a continuation. */
    memset(&t, 0, sizeof(t));
    n = doc_split_long_word("ab\xc3\xa9" "def", 20, t_measure, NULL, t_emit,
                            &t);
    CHECK(n == 4, "split: multibyte word pieces");
    if (n == 4)
    {
        CHECK(((unsigned char)t.got[1][0]) == 0xC3,
              "split: never splits inside a codepoint");
    }
    CHECK(doc_split_long_word(NULL, 20, t_measure, NULL, t_emit, &t) == 0,
          "split: null safe");
    CHECK(doc_split_long_word("abc", 0, t_measure, NULL, t_emit, &t) == 0,
          "split: non-positive maxW safe");
}

/* ── Unit: color luminance + background fan-out ─────────────────────────── */
static void test_gray(void)
{
    CHECK(css_color_luminance("#fff", 4) == 255, "lum: #fff");
    CHECK(css_color_luminance("#000", 4) == 0, "lum: #000");
    CHECK(css_color_luminance("#888", 4) == 136, "lum: #888");
    CHECK(css_color_luminance("#ff0000", 7) == 76, "lum: pure red");
    CHECK(css_color_luminance("gray", 4) == 128, "lum: named gray");
    CHECK(css_color_luminance("darkgray", 8) == 169, "lum: darkgray beats gray");
    CHECK(css_color_luminance("rgb(10, 20, 30)", 15) == 18, "lum: rgb()");
    CHECK(css_color_luminance("rgb(300,0,0)", 12) == 76, "lum: rgb clamped");
    CHECK(css_color_luminance("url(gray.png)", 13) == -1,
          "lum: url() is not a color");
    CHECK(css_color_luminance("none", 4) == -1, "lum: none");
    CHECK(css_color_luminance("1px solid black", 15) == 0,
          "lum: black inside shorthand");
    CHECK(css_color_luminance(NULL, 0) == -1, "lum: null safe");

    static const char *CSS =
        "<style>.a { background: #888 url(x.png) }\n"
        ".b { background-color: silver }\n"
        ".c { background: url(y.png) }\n"
        ".d { background: black }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(css_rule_prop(&eng.rules[0], "background") == NULL,
          "gray: shorthand itself not stored");
    size_t n = 0;
    const char *v = css_winning_value(&eng, "p", "a", NULL, NULL, NULL,
                                      "background-color", &n);
    CHECK(v && n == 4 && memcmp(v, "#888", 4) == 0,
          "gray: hex fanned out");
    v = css_winning_value(&eng, "p", "b", NULL, NULL, NULL,
                          "background-color", &n);
    CHECK(v && css_color_luminance(v, n) == 192, "gray: named fans out");
    v = css_winning_value(&eng, "p", "c", NULL, NULL, NULL,
                          "background-color", &n);
    CHECK(v == NULL, "gray: imageless background registers nothing");
    v = css_winning_value(&eng, "p", "d", NULL, NULL, NULL,
                          "background-color", &n);
    CHECK(v && css_color_luminance(v, n) == 0, "gray: black fans out");
}

/* ── Unit: font-size buckets ────────────────────────────────────────────── */
static void test_size(void)
{
    static const char *CSS =
        "<style>.a { font-size: small }\n"
        ".b { font-size: xx-large }\n"
        ".c { font-size: medium }\n"
        ".d { font-size: 10px }\n"
        ".e { font-size: 24px }\n"
        ".f { font-size: 16px }\n"
        ".g { font-size: 80% }\n"
        ".h { font-size: 150% }\n"
        ".i { font-size: 0.8em }\n"
        ".j { font-size: 2rem }\n"
        ".k { font-size: larger }\n"
        ".l { font-size: smaller }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 12, "size: 12 rules");
    CHECK(css_compute(&eng, "p", "a", NULL, NULL, NULL) & CSS_F_FSMALL,
          "size: small");
    CHECK(css_compute(&eng, "p", "b", NULL, NULL, NULL) & CSS_F_FBIG,
          "size: xx-large");
    CHECK(!(css_compute(&eng, "p", "c", NULL, NULL, NULL) &
            (CSS_F_FSMALL | CSS_F_FBIG)),
          "size: medium sets nothing");
    CHECK(css_compute(&eng, "p", "d", NULL, NULL, NULL) & CSS_F_FSMALL,
          "size: 10px small");
    CHECK(css_compute(&eng, "p", "e", NULL, NULL, NULL) & CSS_F_FBIG,
          "size: 24px big");
    CHECK(!(css_compute(&eng, "p", "f", NULL, NULL, NULL) &
            (CSS_F_FSMALL | CSS_F_FBIG)),
          "size: 16px neutral");
    CHECK(css_compute(&eng, "p", "g", NULL, NULL, NULL) & CSS_F_FSMALL,
          "size: 80% small");
    CHECK(css_compute(&eng, "p", "h", NULL, NULL, NULL) & CSS_F_FBIG,
          "size: 150% big");
    CHECK(css_compute(&eng, "p", "i", NULL, NULL, NULL) & CSS_F_FSMALL,
          "size: 0.8em small");
    CHECK(css_compute(&eng, "p", "j", NULL, NULL, NULL) & CSS_F_FBIG,
          "size: 2rem big");
    CHECK(css_compute(&eng, "p", "k", NULL, NULL, NULL) & CSS_F_FBIG,
          "size: larger");
    CHECK(css_compute(&eng, "p", "l", NULL, NULL, NULL) & CSS_F_FSMALL,
          "size: smaller");
}

/* ── Unit: multi-sheet pointer lifetime ─────────────────────────────────── */
static void test_multisheet(void)
{
    /* Sheet 1's copy must not corrupt sheet 0's rule pointers (they used
     * to share one static buffer). Sheet 1 is deliberately long and
     * different so a stale pointer reads visibly wrong content. */
    static const char *HTML =
        "<style>.aaa { text-align: center }</style>"
        "<style>.bbb { text-align: right; font-weight: bold; "
        "color: white; text-decoration: underline; "
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx: y }</style>";
    CssSheet sh[8];
    int n = css_scan_sheets(HTML, sh, 8);
    CHECK(n == 2, "multisheet: two sheets scanned");
    CssEngine eng;
    css_parse_sheets(&eng, sh, n);
    CHECK(eng.ruleCount == 2, "multisheet: two rules");
    unsigned bits = css_compute(&eng, "p", "aaa", NULL, NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_CENTER) && !(bits & CSS_F_ALIGN_RIGHT),
          "multisheet: sheet-0 rule intact after sheet-1 parse");
    bits = css_compute(&eng, "p", "bbb", NULL, NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_RIGHT) && (bits & CSS_F_BOLD) &&
              (bits & CSS_F_UNDERLINE) && (bits & CSS_F_INVERT),
          "multisheet: sheet-1 rule intact");
}

static void test_important(void){
    static const char *CSS =
        "<style>#id { text-align: center }\n"          /* spec 256 order 0 */
        "p { text-align: right !important }\n"         /* spec 1   order 1 */
        "p { text-align: left }\n"                     /* spec 1   order 2 */
        ".x { font-weight: bold !important; font-weight: normal }\n"
        ".y { display: none !IMPORTANT }</style>";
    CssSheet sh[1];
    (void)css_scan_sheets(CSS, sh, 1);
    CssEngine eng;
    css_parse_sheets(&eng, sh, 1);
    CHECK(eng.ruleCount == 5, "important: all 5 rules parse");

    /* !important type rule beats the #id normal rule despite specificity. */
    unsigned bits = css_compute(&eng, "p", NULL, "id", NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_RIGHT) && !(bits & CSS_F_ALIGN_CENTER),
          "important: type!important beats #id normal");
    /* A later NORMAL declaration must not override the earlier !important. */
    CHECK(!(bits & CSS_F_ALIGN_CENTER),
          "important: later normal does not clobber !important");
    /* Same-rule: later normal loses to earlier !important (spec order). */
    bits = css_compute(&eng, "p", "x", NULL, NULL, NULL);
    CHECK((bits & CSS_F_BOLD), "important: same-rule important survives");
    /* Case-insensitive marker; display:none still maps. */
    bits = css_compute(&eng, "p", "y", NULL, NULL, NULL);
    CHECK((bits & CSS_F_HIDDEN), "important: !IMPORTANT hides");
    /* A later !important DOES override an earlier !important. */
    static const char *CSS2 =
        "<style>.a { text-align: right !important }"
        ".b { text-align: center !important }</style>";
    (void)css_scan_sheets(CSS2, sh, 1);
    css_parse_sheets(&eng, sh, 1);
    bits = css_compute(&eng, "p", "a b", NULL, NULL, NULL);
    CHECK((bits & CSS_F_ALIGN_CENTER) && !(bits & CSS_F_ALIGN_RIGHT),
          "important: later !important beats earlier !important");
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

    /* 5. attribute selectors + UA [hidden] through the real walker. */
    d = parse_page(
        "<html><head><style>"
        "[data-secret] { display: none }"
        "span[title=\"hi\"] { font-weight: bold }"
        "[hidden] { color: white }"
        "</style></head><body>"
        "<div data-secret=\"1\">HUSH</div>"
        "<div data-secret>QUIET</div>"
        "<span title=\"hi\">Titled</span>"
        "<span title=\"bye\">Untitled</span>"
        "<div hidden>CONCEALED</div>"
        "<div hidden class=\"shown\" style=\"display:block\">REVEALED</div>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e5: parses");
    CHECK(!text_present(d, "HUSH"), "e2e5: [attr=value] hides");
    CHECK(!text_present(d, "QUIET"), "e2e5: [attr] presence hides");
    f = inline_flags_of(d, "Titled");
    CHECK((f & DOC_INF_BOLD), "e2e5: [attr=value] compound styles");
    f = inline_flags_of(d, "Untitled");
    CHECK(!(f & DOC_INF_BOLD), "e2e5: value mismatch leaves unstyled");
    CHECK(!text_present(d, "CONCEALED"), "e2e5: UA [hidden] hides");
    CHECK(text_present(d, "REVEALED"), "e2e5: inline display un-hides");
    free_page(d);

    /* 6. child combinator through the real walker: only direct children.
     * (Bold inherits, so the exclusion proof uses text-align: block-level
     * and non-inherited. The bold checks prove direct hits + inheritance
     * composing through a child match.) */
    d = parse_page(
        "<html><head><style>"
        "ul.t>li { font-weight: bold; text-align: center }"
        "</style></head><body>"
        "<ul class=\"t\"><li>OuterKID</li><li>Mid<ul><li>InnerKID</li></ul></li></ul>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e6: parses");
    f = inline_flags_of(d, "OuterKID");
    CHECK((f & DOC_INF_BOLD), "e2e6: direct child styled");
    f = inline_flags_of(d, "Mid");
    CHECK((f & DOC_INF_BOLD), "e2e6: second direct child styled");
    f = inline_flags_of(d, "InnerKID");
    CHECK((f & DOC_INF_BOLD), "e2e6: grandchild inherits via matched parent");
    CHECK(block_align(d, "OuterKID") &&
              strcmp(block_align(d, "OuterKID"), "center") == 0,
          "e2e6: direct child block centered");
    {
        const char *ia = block_align(d, "InnerKID");
        CHECK(ia == NULL || strcmp(ia, "center") != 0,
              "e2e6: grandchild block NOT centered by >");
    }
    free_page(d);

    /* 7. text-transform + white-space through the real walker. */
    d = parse_page(
        "<html><head><style>"
        ".u { text-transform: uppercase }"
        ".l { text-transform: lowercase }"
        ".c { text-transform: capitalize }"
        ".p { white-space: pre }"
        ".pl { white-space: pre-line }"
        "</style></head><body>"
        "<p class=\"u\">shout quietly</p>"
        "<p class=\"l\">LOUD NOISES</p>"
        "<p class=\"c\">hello brave new world</p>"
        "<p class=\"p\">spaced   out</p>"
        "<p class=\"pl\">line\nbreak</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e7: parses");
    CHECK(text_present(d, "SHOUT QUIETLY"), "e2e7: uppercase applied");
    CHECK(!text_present(d, "shout quietly"), "e2e7: original gone");
    CHECK(text_present(d, "loud noises"), "e2e7: lowercase applied");
    CHECK(text_present(d, "Hello Brave New World"), "e2e7: capitalize applied");
    CHECK(text_present(d, "spaced   out"), "e2e7: pre keeps spaces");
    f = inline_flags_of(d, "line");
    CHECK((f & DOC_INF_PRELINE), "e2e7: pre-line flag on inline");
    free_page(d);

    /* 8. list-style-type through the real walker (marker codes). */
    d = parse_page(
        "<html><head><style>"
        "ul.sq { list-style-type: square }"
        "ul.ci { list-style-type: circle }"
        "ol.rm { list-style-type: lower-roman }"
        "ol.ur { list-style-type: upper-roman; list-style-type: decimal }"
        "li.plain-none { list-style-type: none }"
        "li.direct { list-style-type: upper-alpha }"
        "</style></head><body>"
        "<ul class=\"sq\"><li>SqItem</li><li><ul><li>NestedPlain</li></ul></li></ul>"
        "<ul class=\"ci\"><li>CiItem</li></ul>"
        "<ol class=\"rm\"><li>Three</li></ol>"
        "<ol class=\"ur\"><li>Seven</li></ol>"
        "<ul><li class=\"plain-none\">Gone</li><li class=\"direct\">Beta</li></ul>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e8: parses");
    CHECK(block_marker(d, "SqItem") == 's', "e2e8: ul square");
    CHECK(block_marker(d, "NestedPlain") == '1',
          "e2e8: nested plain ul keeps default");
    CHECK(block_marker(d, "CiItem") == 'c', "e2e8: ul circle");
    CHECK(block_marker(d, "Three") == 'i', "e2e8: ol lower-roman");
    CHECK(block_marker(d, "Seven") == '1',
          "e2e8: later decimal beats upper-roman");
    CHECK(block_marker(d, "Gone") == 'n', "e2e8: li none");
    CHECK(block_marker(d, "Beta") == 'A', "e2e8: li direct override");
    free_page(d);

    /* 9. ::before/::after generated content through the real walker. */
    d = parse_page(
        "<html><head><style>"
        "p.q::before { content: \"FIRST-BEFORE\" }"
        "p.q::before { content: \"SECOND-BEFORE\" }"
        "p.q::after { content: \"AFTER-TAIL\" }"
        "a.u::after { content: \" [\" attr(href) \"]\" }"
        "span.s::before { content: \"S:\" }"
        "</style></head><body>"
        "<p class=\"q\">MidKid</p>"
        "<p>PlainKid <a class=\"u\" href=\"https://example.com/x\">LinkKid</a></p>"
        "<p><span class=\"s\">SpanKid</span></p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e9: parses");
    CHECK(!text_present(d, "FIRST-BEFORE"), "e2e9: later content wins");
    CHECK(text_present(d, "SECOND-BEFORE"), "e2e9: ::before emitted");
    CHECK(text_present(d, "AFTER-TAIL"), "e2e9: ::after emitted");
    CHECK(text_present(d, "MidKid"), "e2e9: element text intact");
    CHECK(text_present(d, "[https://example.com/x]"),
          "e2e9: attr(href) resolved");
    CHECK(text_present(d, "S:"), "e2e9: inline ::before emitted");
    free_page(d);

    /* 10. structural pseudo-classes through the real walker. */
    d = parse_page(
        "<html><head><style>"
        "ul.z>li:first-child { font-weight: bold }"
        "ul.z>li:nth-child(even) { text-decoration: underline }"
        "ul.z>li:nth-child(3) { font-style: italic }"
        "a.nl:link { font-style: italic }"
        "</style></head><body>"
        "<ul class=\"z\"><li>OneKid</li><li>TwoKid</li><li>ThreeKid</li></ul>"
        "<p><a class=\"nl\" href=\"https://example.com/q\">LinkKid</a></p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e10: parses");
    f = inline_flags_of(d, "OneKid");
    CHECK((f & DOC_INF_BOLD) && !(f & DOC_INF_UNDERLINE),
          "e2e10: first-child only");
    f = inline_flags_of(d, "TwoKid");
    CHECK((f & DOC_INF_UNDERLINE) && !(f & DOC_INF_BOLD),
          "e2e10: even only");
    f = inline_flags_of(d, "ThreeKid");
    CHECK((f & DOC_INF_ITALIC) && !(f & DOC_INF_BOLD) &&
              !(f & DOC_INF_UNDERLINE),
          "e2e10: (3) only");
    f = inline_flags_of(d, "LinkKid");
    CHECK((f & DOC_INF_ITALIC), "e2e10: :link hits href");
    free_page(d);

    /* 10c. break-word flag through the real walker (layout splits). */
    d = parse_page(
        "<html><head><style>"
        "p.bw { overflow-wrap: break-word }"
        "p.ba { word-break: break-all }"
        "</style></head><body>"
        "<p class=\"bw\">BreakKid https://example.com/very/long/url</p>"
        "<p class=\"ba\">AllKid</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e10c: parses");
    f = inline_flags_of(d, "BreakKid");
    CHECK((f & DOC_INF_BREAKWORD), "e2e10c: overflow-wrap flag");
    f = inline_flags_of(d, "AllKid");
    CHECK((f & DOC_INF_BREAKWORD), "e2e10c: break-all flag");
    free_page(d);

    /* 13. gray backgrounds through the real walker (invert/shade/none). */
    d = parse_page(
        "<html><head><style>"
        ".dk { background-color: black }"
        ".md { background-color: #888 }"
        ".lt { background-color: silver }"
        ".pl { background-color: white }"
        ".sh { background: #aaa url(x.png) }"
        "</style></head><body>"
        "<p class=\"dk\">DarkKid</p>"
        "<p class=\"md\">MidKid</p>"
        "<p class=\"lt\">LightKid</p>"
        "<p class=\"pl\">PaperKid</p>"
        "<p class=\"sh\">ShortKid</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e13: parses");
    {
        int dkInv = -1, mdGray = -2, ltGray = -2, plGray = -2, shGray = -2;
        int mdInv = -1;
        for (int i = 0; i < d->blockCount; i++)
        {
            DocBlock *b = d->blocks[i];
            for (int j = 0; j < b->inlineCount; j++)
            {
                const char *t = b->inlines[j]->text;
                if (!t)
                {
                    continue;
                }
                if (strstr(t, "DarkKid"))
                {
                    dkInv = b->invert;
                }
                if (strstr(t, "MidKid"))
                {
                    mdGray = b->bgGray;
                    mdInv = b->invert;
                }
                if (strstr(t, "LightKid"))
                {
                    ltGray = b->bgGray;
                }
                if (strstr(t, "PaperKid"))
                {
                    plGray = b->bgGray;
                }
                if (strstr(t, "ShortKid"))
                {
                    shGray = b->bgGray;
                }
            }
        }
        CHECK(dkInv == 1, "e2e13: black inverts (legacy mapping kept)");
        CHECK(mdGray == 136 && mdInv == 0, "e2e13: mid gray shades");
        CHECK(ltGray == 192, "e2e13: silver shades");
        CHECK(plGray == -1, "e2e13: white is paper (nothing)");
        CHECK(shGray == 170, "e2e13: shorthand hex shades");
    }
    free_page(d);

    /* 14. font-size buckets + text-indent through the real walker. */
    d = parse_page(
        "<html><head><style>"
        ".sm { font-size: small }"
        ".bg { font-size: x-large }"
        ".ti { text-indent: 20px }"
        ".tm { margin-left: 40px; text-indent: 20px }"
        "</style></head><body>"
        "<p class=\"sm\">SmallKid</p>"
        "<p class=\"bg\">BigKid</p>"
        "<p class=\"ti\">IndentKid</p>"
        "<p class=\"tm\">MarginKid</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e14: parses");
    f = inline_flags_of(d, "SmallKid");
    CHECK((f & DOC_INF_SMALL) && !(f & DOC_INF_BIG), "e2e14: small flag");
    f = inline_flags_of(d, "BigKid");
    CHECK((f & DOC_INF_BIG) && !(f & DOC_INF_SMALL), "e2e14: big flag");
    {
        int ti = -1, tm = -1;
        for (int i = 0; i < d->blockCount; i++)
        {
            DocBlock *b = d->blocks[i];
            for (int j = 0; j < b->inlineCount; j++)
            {
                const char *t = b->inlines[j]->text;
                if (!t)
                {
                    continue;
                }
                if (strstr(t, "IndentKid"))
                {
                    ti = b->indent;
                }
                if (strstr(t, "MarginKid"))
                {
                    tm = b->indent;
                }
            }
        }
        CHECK(ti == 10, "e2e14: text-indent halved");
        CHECK(tm == 20, "e2e14: margin-left wins over text-indent");
    }
    free_page(d);

    /* 15. display box-switching on div/span through the real walker. */
    d = parse_page(
        "<html><head><style>"
        "div.inl { display: inline }"
        "span.blk { display: block }"
        "span.flx { display: flex }"
        "span.grd { display: grid }"
        "span.flt { float: left }"
        "</style></head><body>"
        "<div>StartKid <div class=\"inl\">InlineKid</div> EndKid</div>"
        "<p><span class=\"blk\">BlockKid</span>AfterKid</p>"
        "<p><span class=\"flx\">FlexA</span><span class=\"flx\">FlexB</span></p>"
        "<p><span class=\"grd\">GridKid</span>GridTail</p>"
        "<p><span class=\"flt\">FloatKid</span>TailKid</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e15: parses");
    CHECK(block_index(d, "StartKid") == block_index(d, "InlineKid") &&
              block_index(d, "InlineKid") == block_index(d, "EndKid"),
          "e2e15: div inline flows into the paragraph");
    CHECK(block_index(d, "BlockKid") != block_index(d, "AfterKid"),
          "e2e15: span block breaks");
    {
        int fa = block_index(d, "FlexA");
        int fb = block_index(d, "FlexB");
        CHECK(fa >= 0 && fb >= 0 && fa != fb && fa < fb,
              "e2e15: flex linearizes in order");
    }
    CHECK(block_index(d, "GridKid") != block_index(d, "GridTail"),
          "e2e15: grid linearizes");
    CHECK(block_index(d, "FloatKid") != block_index(d, "TailKid"),
          "e2e15: float linearizes");
    free_page(d);

    /* 10b. table width/collapse/cell-padding through the real walker. */
    d = parse_page(
        "<html><head><style>"
        "table.wt { width: 85% }"
        "table.wx { width: 300px }"
        "table.cp { border-collapse: collapse }"
        "table.sp { border-collapse: separate }"
        "td.pp { padding: 10px }"
        "</style></head><body>"
        "<table class=\"wt\"><tr><td>WideTab</td></tr></table>"
        "<table class=\"wx\"><tr><td>PxTab</td></tr></table>"
        "<table class=\"cp\" border=\"1\"><tr><td>CollapseTab</td></tr></table>"
        "<table class=\"sp\" border=\"1\"><tr><td>SeparateTab</td></tr></table>"
        "<table><tr><td class=\"pp\">PaddedCell</td></tr></table>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e10b: parses");
    {
        const char *wt = NULL, *wx = NULL;
        int cp = -1, sp = -1, pad = -1;
        for (int i = 0; i < d->blockCount; i++)
        {
            DocBlock *b = d->blocks[i];
            if (b->type == DOC_BLOCK_TABLE && b->table)
            {
                for (int r = 0; r < b->table->rowCount; r++)
                {
                    DocRow *row = b->table->rows[r];
                    for (int c = 0; c < row->cellCount; c++)
                    {
                        DocCell *cell = row->cells[c];
                        for (int k = 0; k < cell->inlineCount; k++)
                        {
                            const char *t = cell->inlines[k]->text;
                            if (!t)
                            {
                                continue;
                            }
                            if (strstr(t, "WideTab"))
                            {
                                wt = b->table->width;
                            }
                            if (strstr(t, "PxTab"))
                            {
                                wx = b->table->width;
                            }
                            if (strstr(t, "CollapseTab"))
                            {
                                cp = b->table->collapse;
                            }
                            if (strstr(t, "SeparateTab"))
                            {
                                sp = b->table->collapse;
                            }
                            if (strstr(t, "PaddedCell"))
                            {
                                pad = cell->pad;
                            }
                        }
                    }
                }
            }
        }
        CHECK(wt && strcmp(wt, "85%") == 0, "e2e10b: percent width kept");
        CHECK(wx && strcmp(wx, "300px") == 0, "e2e10b: px width kept");
        CHECK(cp == 1, "e2e10b: collapse flag");
        CHECK(sp == 2, "e2e10b: separate flag");
        CHECK(pad == 5, "e2e10b: cell padding halved");
    }
    free_page(d);

    /* 11. CSS borders through the real walker (block widths + tables). */
    d = parse_page(
        "<html><head><style>"
        "p.bx { border: 2px solid black }"
        "p.no { border: none }"
        "p.thick { border-width: 9px }"
        "p.sty { border-style: none; border-width: 2px }"
        "table.bt { border: 1px solid black }"
        "table.bn { border: none }"
        "</style></head><body>"
        "<p class=\"bx\">BoxedKid</p>"
        "<p class=\"no\">PlainKid</p>"
        "<p class=\"thick\">ThickKid</p>"
        "<p class=\"sty\">StyledKid</p>"
        "<table class=\"bt\"><tr><td>BorderedTab</td></tr></table>"
        "<table class=\"bn\" border=\"1\"><tr><td>UnborderedTab</td></tr></table>"
        "<table border=\"1\"><tr><td>AttrTab</td></tr></table>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e11: parses");
    CHECK(block_border(d, "BoxedKid") == 2, "e2e11: shorthand width");
    CHECK(block_border(d, "PlainKid") == 0, "e2e11: none");
    CHECK(block_border(d, "ThickKid") == 3, "e2e11: clamped to 3");
    CHECK(block_border(d, "StyledKid") == 0, "e2e11: style none kills");
    {
        int bt = -1, bn = -1, at = -1;
        for (int i = 0; i < d->blockCount; i++)
        {
            DocBlock *b = d->blocks[i];
            if (b->type == DOC_BLOCK_TABLE && b->table)
            {
                for (int r = 0; r < b->table->rowCount; r++)
                {
                    DocRow *row = b->table->rows[r];
                    for (int c = 0; c < row->cellCount; c++)
                    {
                        DocCell *cell = row->cells[c];
                        for (int k = 0; k < cell->inlineCount; k++)
                        {
                            const char *t = cell->inlines[k]->text;
                            if (t && strstr(t, "BorderedTab"))
                            {
                                bt = b->table->border;
                            }
                            if (t && strstr(t, "UnborderedTab"))
                            {
                                bn = b->table->border;
                            }
                            if (t && strstr(t, "AttrTab"))
                            {
                                at = b->table->border;
                            }
                        }
                    }
                }
            }
        }
        CHECK(bt == 1, "e2e11: table CSS border");
        CHECK(bn == 0, "e2e11: table CSS none beats attr");
        CHECK(at == 1, "e2e11: table attr border kept");
    }
    free_page(d);

    /* 12. margin/padding through the real walker (halved Lua-parity px). */
    d = parse_page(
        "<html><head><style>"
        "p.mg { margin: 20px 10px }"
        "p.pd { padding-left: 30px }"
        "p.mx { margin-top: 7px }"
        "p.ov { margin-top: 20px }"
        "</style></head><body>"
        "<p class=\"mg\">MarginKid</p>"
        "<p class=\"pd\">PaddingKid</p>"
        "<p class=\"mx\">HalvedKid</p>"
        "<p class=\"ov\" style=\"margin-top:40px\">OverrideKid</p>"
        "</body></html>");
    CHECK(d && !d->parseError, "e2e12: parses");
    {
        int top = -1, bottom = -1, indent = -1;
        int pindent = -1, htop = -1, ovtop = -1;
        for (int i = 0; i < d->blockCount; i++)
        {
            DocBlock *b = d->blocks[i];
            for (int j = 0; j < b->inlineCount; j++)
            {
                const char *t = b->inlines[j]->text;
                if (!t)
                {
                    continue;
                }
                if (strstr(t, "MarginKid"))
                {
                    top = b->spacingTop;
                    bottom = b->spacingBottom;
                    indent = b->indent;
                }
                if (strstr(t, "PaddingKid"))
                {
                    pindent = b->indent;
                }
                if (strstr(t, "HalvedKid"))
                {
                    htop = b->spacingTop;
                }
                if (strstr(t, "OverrideKid"))
                {
                    ovtop = b->spacingTop;
                }
            }
        }
        CHECK(top == 10 && bottom == 10, "e2e12: margin tb halved");
        CHECK(indent == 5, "e2e12: margin rl fills indent");
        CHECK(pindent == 15, "e2e12: padding-left fills indent");
        CHECK(htop == 3, "e2e12: 7px floors to 3");
        CHECK(ovtop == 10, "e2e12: stylesheet beats inline hint");
    }
    free_page(d);
}

/* js_doc_close comes from jsbridge.c (linked). */

int main(void)
{
    test_scanner();
    test_parse();
    test_match();
    test_cascade();
    test_attr();
    test_child();
    test_text();
    test_pseudo();
    test_pclass();
    test_media();
    test_border();
    test_spacing();
    test_split();
    test_gray();
    test_size();
    test_multisheet();
    test_important();
    test_e2e();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
