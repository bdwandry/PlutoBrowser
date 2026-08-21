#include "html/selftest_document.h"
#include "html/document.h"
#include "html/tokenizer.h"
#include "core/constants.h"
#include "core/logger.h"
#include "util/mem.h"
#include <string.h>
#include <stdio.h>

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P11] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P11] FAIL %s", name); }
}

#define BASE "http://ex.com/dir/page.html"

static DocDocument* parse(const char* html) {
    return doc_parse(html, BASE, PLUTO_MODE_RAW_HTML);
}

static DocBlock* blk(const DocDocument* d, size_t i) {
    return (d != NULL && i < d->nBlocks) ? &d->blocks[i] : NULL;
}

static DocBlock* find_type(const DocDocument* d, int type) {
    if (d == NULL) return NULL;
    for (size_t i = 0; i < d->nBlocks; i++)
        if (d->blocks[i].type == type) return &d->blocks[i];
    return NULL;
}

static const DocInline* inl(const DocBlock* b, size_t i) {
    return (b != NULL && i < b->nInlines) ? &b->inlines[i] : NULL;
}

static int txt_is(const DocInline* x, const char* s) {
    return x != NULL && x->type == DIT_TEXT && x->text != NULL &&
           strcmp(x->text, s) == 0;
}

/* ── A. empty / blank ─────────────────────────────────────────────────── */

static void case_empty(void) {
    DocDocument* d = parse("");
    ck("A.blank_title", d != NULL && d->title != NULL &&
       strcmp(d->title, "Blank Page") == 0);
    ck("A.blank_base", d != NULL && d->baseUrl != NULL &&
       strcmp(d->baseUrl, BASE) == 0);
    ck("A.empty_no_blocks", d != NULL && d->nBlocks == 0 &&
       d->links == NULL && d->nLinks == 0 && !d->hasMetaRefresh);
    if (d != NULL) ck("A.raw_html", d->rawHtml != NULL &&
                      strcmp(d->rawHtml, "") == 0);
    doc_free(d);
}

static void case_reader_gate(void) {
    DocDocument* d = doc_parse("<p>hi</p>", BASE, PLUTO_MODE_READER);
    ck("B.reader_null", d == NULL);
}

/* ── C. headings ──────────────────────────────────────────────────────── */

static void case_headings(void) {
    DocDocument* d = parse("<h2>T</h2>");
    DocBlock* h = find_type(d, DB_HEADING);
    ck("C.h2_meta", h != NULL && h->level == 2 &&
        h->spacingTop == 0 && h->spacingBottom == 0 &&
        h->nInlines == 1 && txt_is(inl(h, 0), "T"));
    doc_free(d);

    d = parse("<h6 align=center>X</h6>");
    h = find_type(d, DB_HEADING);
    ck("C.h6_align_attr", h != NULL && h->level == 6 &&
       h->align != NULL && strcmp(h->align, "center") == 0);
    doc_free(d);

    d = parse("<h3 style=\"text-align: right\">Y</h3>");
    h = find_type(d, DB_HEADING);
    ck("C.h3_align_style", h != NULL && h->level == 3 &&
       h->align != NULL && strcmp(h->align, "right") == 0);
    doc_free(d);
}

/* ── D. paragraphs / spacing / inversion ──────────────────────────────── */

static void case_paragraphs(void) {
    DocDocument* d = parse("<div>hello</div>");
    ck("D.div_para", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].type == DB_PARAGRAPH &&
       txt_is(inl(&d->blocks[0], 0), "hello"));
    doc_free(d);

    d = parse("<section>s</section>");
    ck("D.section_para", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].type == DB_PARAGRAPH);
    doc_free(d);

    d = parse("<p style=\"margin: 10 20\">x</p>");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("D.margin_shorthand2", p != NULL && p->spacingTop == 5 &&
       p->spacingBottom == 5 && p->indent == 10);
    doc_free(d);

    d = parse("<p style=\"padding-left: 8\">x</p>");
    p = find_type(d, DB_PARAGRAPH);
    ck("D.padding_left", p != NULL && p->indent == 4);
    doc_free(d);

    d = parse("<div style=\"background-color: black\" align=center>v</div>");
    p = find_type(d, DB_PARAGRAPH);
    ck("D.invert_center", p != NULL && p->invert == 1 &&
       p->align != NULL && strcmp(p->align, "center") == 0);
    doc_free(d);
}

/* ── E. inline formats ────────────────────────────────────────────────── */

static void case_formats(void) {
    DocDocument* d = parse("<b>B<i>I</i></b>");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("E.nest_bi", p != NULL && p->nInlines == 2 &&
       txt_is(inl(p, 0), "B") && inl(p, 0)->bold && !inl(p, 0)->italic &&
       txt_is(inl(p, 1), "I") && inl(p, 1)->bold && inl(p, 1)->italic);
    doc_free(d);

    d = parse("<u>u</u><s>x</s><mark>m</mark><small>s</small>"
              "<big>g</big><sub>lo</sub><sup>hi</sup>");
    p = find_type(d, DB_PARAGRAPH);
    ck("E.flags7", p != NULL && p->nInlines == 7 &&
       inl(p, 0)->underline && !inl(p, 1)->underline &&
       inl(p, 1)->strike && inl(p, 2)->mark && inl(p, 3)->small &&
       inl(p, 4)->big && inl(p, 5)->sub && inl(p, 6)->sup);
    doc_free(d);

    d = parse("<tt>c</tt><kbd>k</kbd><samp>s</samp>");
    p = find_type(d, DB_PARAGRAPH);
    ck("E.code_family", p != NULL && p->nInlines == 3 &&
       inl(p, 0)->code && inl(p, 1)->code && inl(p, 2)->code);
    doc_free(d);
}

/* ── F. br / wbr / hr ─────────────────────────────────────────────────── */

static void case_breaks(void) {
    DocDocument* d = parse("a<br>b");
    ck("F.br_same_para", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].nInlines == 3 &&
       d->blocks[0].inlines[1].type == DIT_BR &&
       txt_is(inl(&d->blocks[0], 0), "a") &&
       txt_is(inl(&d->blocks[0], 2), "b"));
    doc_free(d);

    d = parse("<b>a<br>b</b>");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("F.br_flags", p != NULL && p->nInlines == 3 &&
       inl(p, 1)->type == DIT_BR && inl(p, 1)->bold);
    doc_free(d);

    d = parse("x<wbr>y");
    p = find_type(d, DB_PARAGRAPH);
    ck("F.wbr", p != NULL && p->nInlines == 3 &&
       p->inlines[1].type == DIT_WBR);
    doc_free(d);

    d = parse("x<hr>y");
    ck("F.hr_flushes", d != NULL && d->nBlocks == 3 &&
       d->blocks[0].type == DB_PARAGRAPH &&
       d->blocks[1].type == DB_HR &&
       d->blocks[1].spacingTop == 6 && d->blocks[1].spacingBottom == 6 &&
       d->blocks[2].type == DB_PARAGRAPH);
    doc_free(d);
}

/* ── G. pre family ────────────────────────────────────────────────────── */

static void case_pre(void) {
    DocDocument* d = parse("<pre>a\n\tb</pre>");
    DocBlock* c = find_type(d, DB_CODE_BLOCK);
    ck("G.pre_raw", c != NULL && c->codeText != NULL &&
       strcmp(c->codeText, "a\n\tb") == 0 &&
       c->nLines == 2 && c->lines != NULL &&
       strcmp(c->lines[0], "a") == 0 && strcmp(c->lines[1], "\tb") == 0);
    doc_free(d);

    d = parse("<pre>l1\nl2\n\nl4</pre>");
    c = find_type(d, DB_CODE_BLOCK);
    ck("G.pre_blank_line", c != NULL && c->nLines == 4 &&
       strcmp(c->lines[2], "") == 0);
    doc_free(d);

    d = parse("<xmp>A\nB</xmp>");
    c = find_type(d, DB_CODE_BLOCK);
    ck("G.xmp", c != NULL && c->codeText != NULL &&
       strcmp(c->codeText, "A\nB") == 0);
    doc_free(d);
}

/* ── H. span / font / time / data ─────────────────────────────────────── */

static void case_span(void) {
    DocDocument* d =
        parse("<span style=\"font-size: small\">s</span>"
              "<span style=\"font-size: xx-large\">L</span>"
              "<span style=\"vertical-align: super\">^</span>"
              "<span style=\"vertical-align: sub\">v</span>"
              "<span style=\"background-color: #eee\">m</span>"
              "<span style=\"color: #FFFFFF\">w</span>");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("H.span_styles", p != NULL && p->nInlines == 6 &&
       inl(p, 0)->small && inl(p, 1)->big && inl(p, 2)->sup &&
       inl(p, 3)->sub && inl(p, 4)->mark && inl(p, 5)->invert);
    doc_free(d);

    d = parse("<font size=7>A</font><font size=1>B</font>"
              "<font size=\"+2\">C</font><font size=3>D</font>");
    p = find_type(d, DB_PARAGRAPH);
    ck("H.font_size", p != NULL && p->nInlines == 4 &&
       inl(p, 0)->big && !inl(p, 0)->small &&
       inl(p, 1)->small && !inl(p, 1)->big &&
       inl(p, 2)->small && inl(p, 3)->big == 0 && inl(p, 3)->small == 0);
    doc_free(d);

    d = parse("<time datetime=\"2026-01-01\"></time>");
    p = find_type(d, DB_PARAGRAPH);
    ck("H.time_fallback", p != NULL && p->nInlines == 1 &&
       txt_is(inl(p, 0), "2026-01-01"));
    doc_free(d);

    d = parse("<data value=\"42\">x</data>");
    p = find_type(d, DB_PARAGRAPH);
    ck("H.data_children_win", p != NULL && p->nInlines == 1 &&
       txt_is(inl(p, 0), "x"));
    doc_free(d);

    d = parse("<time>t</time>");
    p = find_type(d, DB_PARAGRAPH);
    ck("H.time_text_no_fallback", p != NULL && p->nInlines == 1 &&
       txt_is(inl(p, 0), "t"));
    doc_free(d);
}

/* ── I. q quotes ──────────────────────────────────────────────────────── */

static void case_quote(void) {
    DocDocument* d = parse("<q>hi</q>");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("I.quotes", p != NULL && p->nInlines == 3 &&
       txt_is(inl(p, 0), "\"") && inl(p, 0)->bold && inl(p, 0)->italic &&
       txt_is(inl(p, 1), "hi") && !inl(p, 1)->bold &&
       txt_is(inl(p, 2), "\"") && inl(p, 2)->bold);
    doc_free(d);
}

/* ── J. links ─────────────────────────────────────────────────────────── */

static void case_links(void) {
    DocDocument* d = parse("<a href=\"x.html\" target=_blank>go</a>");
    ck("J.resolve_target", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].href, "http://ex.com/dir/x.html") == 0 &&
       strcmp(d->links[0].text, "go") == 0 &&
       strcmp(d->links[0].target, "_blank") == 0);
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("J.inline_link", p != NULL && p->nInlines == 1 &&
       inl(p, 0)->underline && inl(p, 0)->href != NULL &&
       inl(p, 0)->anchorIndex == 0);
    doc_free(d);

    d = parse("<a href=\"#f\">1</a><a href=\"javascript:x\">2</a>"
              "<a href=\"data:text/plain,hi\">3</a><a>4</a>");
    ck("J.invalid_skipped", d != NULL && d->nLinks == 0);
    p = find_type(d, DB_PARAGRAPH);
    ck("J.invalid_plain", p != NULL && p->nInlines == 4 &&
       !inl(p, 0)->underline && inl(p, 0)->href == NULL);
    doc_free(d);

    d = parse("<a href=\"/t\" title=\"TT\"></a>");
    ck("J.title_fallback", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].text, "TT") == 0);
    doc_free(d);

    d = parse("<a href=\"/r\"></a>");
    ck("J.href_fallback", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].text, "/r") == 0);
    doc_free(d);

    d = parse("<a href=\"/m\">a<b>b</b>c</a>");
    ck("J.text_accum", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].text, "abc") == 0);
    doc_free(d);

    d = parse("<a href=\"/1\">x</a><a href=\"/2\">y</a>");
    ck("J.anchor_seq", d != NULL && d->nLinks == 2 &&
       strcmp(d->links[0].href, "http://ex.com/1") == 0 &&
       strcmp(d->links[1].href, "http://ex.com/2") == 0);
    doc_free(d);
}

/* ── K. lists ─────────────────────────────────────────────────────────── */

static void case_lists(void) {
    DocDocument* d = parse("<ul><li>a<li>b</ul>");
    ck("K.ul_defaults", d != NULL && d->nBlocks == 2 &&
       d->blocks[0].type == DB_LIST_ITEM && !d->blocks[0].isOrdered &&
       d->blocks[0].number == 1 && d->blocks[0].depth == 1 &&
       d->blocks[0].markerType != NULL &&
       strcmp(d->blocks[0].markerType, "1") == 0 &&
       d->blocks[1].number == 2);
    doc_free(d);

    d = parse("<ol start=3><li>x<li>y</ol>");
    ck("K.ol_start", d != NULL && d->blocks[0].isOrdered &&
       d->blocks[0].number == 3 && d->blocks[1].number == 4);
    doc_free(d);

    d = parse("<ol start=5 reversed><li>x<li>y</ol>");
    ck("K.ol_reversed", d != NULL && d->blocks[0].number == 5 &&
       d->blocks[1].number == 4);
    doc_free(d);

    d = parse("<ol type=A><li>x</ol>");
    ck("K.ol_typeA", d != NULL && d->blocks[0].markerType != NULL &&
       strcmp(d->blocks[0].markerType, "A") == 0);
    doc_free(d);

    d = parse("<ol type=9><li>x</ol>");
    ck("K.ol_type_invalid", d != NULL && d->blocks[0].markerType != NULL &&
       strcmp(d->blocks[0].markerType, "1") == 0);
    doc_free(d);

    d = parse("<ol><li>o<ul><li>i</ul></ol>");
    ck("K.nested_depth", d != NULL && d->nBlocks == 2 &&
       d->blocks[0].depth == 1 && d->blocks[0].isOrdered &&
       d->blocks[1].depth == 2 && !d->blocks[1].isOrdered);
    doc_free(d);

    d = parse("<li>stray");
    ck("K.stray_li", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].type == DB_LIST_ITEM && !d->blocks[0].isOrdered &&
       d->blocks[0].number == 1 && d->blocks[0].depth == 1);
    doc_free(d);

    d = parse("<ol><li value=10>x<li>y</ol>");
    ck("K.value_renumber", d != NULL && d->blocks[0].number == 10 &&
       d->blocks[1].number == 11);
    doc_free(d);
}

/* ── L. dl / dt / dd ──────────────────────────────────────────────────── */

static void case_dl(void) {
    DocDocument* d = parse("<dl><dt>t<dd>d</dl>");
    ck("L.dt_dd_flags", d != NULL && d->nBlocks == 2 &&
       d->blocks[0].dtFlag == 1 && d->blocks[0].ddFlag == 0 &&
       d->blocks[1].ddFlag == 1 && d->blocks[1].dtFlag == 0 &&
       d->blocks[1].indent == 20);
    doc_free(d);

    d = parse("<dl><dl><dd>x</dl></dl>");
    DocBlock* dd = find_type(d, DB_PARAGRAPH);
    ck("L.dl_nested_indent", dd != NULL && dd->indent == 40);
    doc_free(d);
}

/* ── N. figure / figcaption ───────────────────────────────────────────── */

static void case_figure(void) {
    DocDocument* d =
        parse("<figure><img src=\"/i.png\" width=10 height=10>"
              "<figcaption>CAP</figcaption></figure>");
    DocBlock* img = find_type(d, DB_IMAGE);
    ck("N.caption_attached", img != NULL && img->caption != NULL &&
       strcmp(img->caption, "CAP") == 0 &&
       strcmp(img->src, "http://ex.com/i.png") == 0);
    doc_free(d);

    d = parse("<figure><figcaption>only</figcaption></figure>");
    ck("N.no_image_no_block", d != NULL && find_type(d, DB_IMAGE) == NULL);
    doc_free(d);
}

/* ── O. images ────────────────────────────────────────────────────────── */

static void case_images(void) {
    DocDocument* d = parse("<img src=\"/a.png\" width=999 height=999>");
    DocBlock* img = find_type(d, DB_IMAGE);
    ck("O.clamps", img != NULL && img->width == 360 && img->height == 180 &&
       strcmp(img->src, "http://ex.com/a.png") == 0 &&
       strcmp(img->alt, "Image") == 0);
    doc_free(d);

    d = parse("<img src=\"x\" width=0 height=-5 alt=\"A\">");
    img = find_type(d, DB_IMAGE);
    ck("O.defaults", img != NULL && img->width == 160 && img->height == 80 &&
       strcmp(img->alt, "A") == 0);
    doc_free(d);

    d = parse("<img title=\"TT\" src=\"x\">");
    img = find_type(d, DB_IMAGE);
    ck("O.alt_title", img != NULL && strcmp(img->alt, "TT") == 0);
    doc_free(d);

    d = parse("<img data-src=\"/d.png\">");
    img = find_type(d, DB_IMAGE);
    ck("O.data_src", img != NULL &&
       strcmp(img->src, "http://ex.com/d.png") == 0);
    doc_free(d);

    d = parse("<img srcset=\"/s.png 2x, /b.png 3x\">");
    img = find_type(d, DB_IMAGE);
    ck("O.srcset_first", img != NULL &&
       strcmp(img->src, "http://ex.com/s.png") == 0);
    doc_free(d);

    d = parse("<img src=\"/tracking.gif\"><img src=\"/beacon.png\">"
              "<img src=\"/ok.png\">");
    img = find_type(d, DB_IMAGE);
    ck("O.filters", img != NULL && d->nBlocks == 1 &&
       strcmp(img->src, "http://ex.com/ok.png") == 0);
    doc_free(d);

    d = parse("<img src=/u.png usemap=\"#m\">");
    img = find_type(d, DB_IMAGE);
    ck("O.usemap_strip", img != NULL && img->usemap != NULL &&
       strcmp(img->usemap, "m") == 0);
    doc_free(d);

    d = parse("<a href=\"/l\"><img src=i.png></a>");
    img = find_type(d, DB_IMAGE);
    ck("O.img_in_link", img != NULL && img->imgHref != NULL &&
       strcmp(img->imgHref, "http://ex.com/l") == 0);
    doc_free(d);
}

/* ── P. visibility / inert ────────────────────────────────────────────── */

static void case_visibility(void) {
    DocDocument* d = parse("<p>on</p><div hidden>off</div><p>on2</p>");
    ck("P.hidden_skip", d != NULL && d->nBlocks == 2 &&
       txt_is(inl(&d->blocks[0], 0), "on") &&
       txt_is(inl(&d->blocks[1], 0), "on2"));
    doc_free(d);

    d = parse("<div style=\"display: none\">x</div><p>y</p>");
    ck("P.display_none", d != NULL && d->nBlocks == 1 &&
       txt_is(inl(&d->blocks[0], 0), "y"));
    doc_free(d);

    d = parse("a<span inert>sec</span>b");
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("P.inert_attr", p != NULL && p->nInlines == 3 &&
       !inl(p, 0)->inert && inl(p, 1)->inert && !inl(p, 2)->inert);
    doc_free(d);
}

/* ── Q. meta refresh ──────────────────────────────────────────────────── */

static void case_meta(void) {
    DocDocument* d =
        parse("<html><head><meta http-equiv=refresh "
              "content=\"5; url=/next.html\"></head><body>x</body></html>");
    ck("Q.scan_pattern_a", d != NULL && d->hasMetaRefresh == 1 &&
       d->metaDelay == 5.0 && d->metaUrl != NULL &&
       strcmp(d->metaUrl, "http://ex.com/next.html") == 0);
    doc_free(d);

    d = parse("<meta http-equiv=\"REFRESH\" content=\"3\">");
    ck("Q.scan_pattern_b", d != NULL && d->hasMetaRefresh &&
       d->metaDelay == 3.0 && d->metaUrl == NULL);
    doc_free(d);

    d = parse("<head><meta http-equiv=refresh content=\"9; url=/h.html\">"
              "</head><body><meta http-equiv=refresh "
              "content=\"2; url=b.html\"></body>");
    ck("Q.body_overrides", d != NULL && d->hasMetaRefresh &&
       d->metaDelay == 2.0 && d->metaUrl != NULL &&
       strcmp(d->metaUrl, "http://ex.com/dir/b.html") == 0);
    doc_free(d);

    d = parse("<meta http-equiv=refresh content=\"later\">");
    ck("Q.invalid_ignored", d != NULL && d->hasMetaRefresh == 0 &&
       d->metaUrl == NULL);
    doc_free(d);
}

/* ── R. base href ─────────────────────────────────────────────────────── */

static void case_base(void) {
    DocDocument* d =
        parse("<head><base href=\"http://cdn.ex/root/\"></head>"
              "<body><a href=\"x.html\">l</a></body>");
    ck("R.base_override", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].href, "http://cdn.ex/root/x.html") == 0);
    doc_free(d);

    d = parse("<base href=\"/not/absolute\"><base "
              "href=\"http://good.ex/\"><a href=\"y\">l</a>");
    ck("R.first_nonempty_wins", d != NULL && d->nLinks == 1 &&
       strcmp(d->links[0].href, "http://ex.com/dir/y") == 0);
    doc_free(d);

    d = parse("<base href=\"https://s.ex/p/\"><img src=z.png>");
    DocBlock* img = find_type(d, DB_IMAGE);
    ck("R.https_base", img != NULL &&
       strcmp(img->src, "https://s.ex/p/z.png") == 0);
    doc_free(d);
}

/* ── S. truncation + caps ─────────────────────────────────────────────── */

static char s_bigHtml[16384];

static void case_caps(void) {
    size_t off = 0;
    s_bigHtml[0] = '\0';
    for (int i = 0; i < 1250 && off + 16 < sizeof(s_bigHtml); i++)
        off += (size_t)snprintf(s_bigHtml + off, sizeof(s_bigHtml) - off,
                                "<p>x</p>");
    DocDocument* d = parse(s_bigHtml);
    ck("S.block_cap_notice", d != NULL && d->nBlocks == 1201 &&
       d->blocks[1200].type == DB_PARAGRAPH &&
       strcmp(d->blocks[1200].align, "center") == 0 &&
       d->blocks[1200].nInlines == 1 && inl(&d->blocks[1200], 0)->bold &&
       txt_is(inl(&d->blocks[1200], 0),
              "(Page too large - rest not rendered)"));
    doc_free(d);

    off = 0;
    for (int i = 0; i < 1000 && off + 16 < sizeof(s_bigHtml); i++)
        off += (size_t)snprintf(s_bigHtml + off, sizeof(s_bigHtml) - off,
                                "<b>x</b>");
    d = parse(s_bigHtml);
    DocBlock* p = find_type(d, DB_PARAGRAPH);
    ck("S.inline_cap", p != NULL && p->nInlines == 900);
    doc_free(d);
}

/* ── T. tables ────────────────────────────────────────────────────────── */

static void case_tables(void) {
    DocDocument* d =
        parse("<table><tr><td>a</td><td>b</td></tr>"
              "<tr><td>c</td></tr></table>");
    ck("T.basic_rows", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].type == DB_TABLE && d->blocks[0].nRows == 2 &&
       d->blocks[0].rows[0].nCells == 2 &&
       d->blocks[0].rows[1].nCells == 1 &&
       d->blocks[0].rows[0].cells[0].inlines != NULL &&
       strcmp(d->blocks[0].rows[0].cells[0].inlines[0].text, "a") == 0 &&
       strcmp(d->blocks[0].rows[0].cells[1].inlines[0].text, "b") == 0);
    doc_free(d);

    /* parity quirk: a new <tr> only closes an open <td>, so an implicitly
     * reopened row nests inside the previous one and its cells are lost */
    d = parse("<table><tr><td>a<td>b<tr><td>c</table>");
    {
        DocBlock* t = find_type(d, DB_TABLE);
        ck("T.implicit_tr_quirk", t != NULL && t->nRows == 1 &&
           t->rows[0].nCells == 2);
    }
    doc_free(d);

    d = parse("<table><tr>"
              "<th colspan=3 abbr=\"AB\">H</th><td rowspan=2>x</td>"
              "</tr></table>");
    ck("T.th_spans", d != NULL && find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->rows[0].cells[0].isHeader == 1 &&
       find_type(d, DB_TABLE)->rows[0].cells[0].colspan == 3 &&
       find_type(d, DB_TABLE)->rows[0].cells[0].rowspan == 1 &&
       find_type(d, DB_TABLE)->rows[0].cells[0].abbr != NULL &&
       strcmp(find_type(d, DB_TABLE)->rows[0].cells[0].abbr, "AB") == 0 &&
       find_type(d, DB_TABLE)->rows[0].cells[1].isHeader == 0 &&
       find_type(d, DB_TABLE)->rows[0].cells[1].rowspan == 2);
    doc_free(d);

    d = parse("<table><caption>  Cap   x </caption><tr><td>y</table>");
    ck("T.caption_trim", d != NULL && find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->tableCaption != NULL &&
       strcmp(find_type(d, DB_TABLE)->tableCaption, "Cap x") == 0);
    doc_free(d);

    d = parse("<table border=1 width=\"80%\" align=center>"
              "<tr><td>i</table>");
    ck("T.border_width_align", d != NULL &&
       find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->tableBorder == 1 &&
       find_type(d, DB_TABLE)->tableWidth != NULL &&
       strcmp(find_type(d, DB_TABLE)->tableWidth, "80%") == 0 &&
       find_type(d, DB_TABLE)->align != NULL &&
       strcmp(find_type(d, DB_TABLE)->align, "center") == 0);
    doc_free(d);

    d = parse("<table border=0><tr><td>i</table>");
    ck("T.border_zero_off", d != NULL &&
       find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->tableBorder == 0);
    doc_free(d);

    d = parse("<table><tbody><tr><td>t1</tr><tr><td>t2</tr></tbody>"
              "</table>");
    ck("T.tbody_wrapper", d != NULL && find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->nRows == 2);
    doc_free(d);

    d = parse("<table><tr><td><table><tr><td>deep</td></tr></table>"
              "</td></tr></table>");
    {
        DocBlock* t = find_type(d, DB_TABLE);
        /* outer table keeps exactly one row; nested table dropped whole */
        int outerOnly = (d != NULL && t != NULL && d->nBlocks == 1 &&
                         t->nRows == 1 && t->rows[0].nCells == 1);
        int spaceFilled = outerOnly &&
            t->rows[0].cells[0].nInlines == 1 &&
            strcmp(t->rows[0].cells[0].inlines[0].text, " ") == 0;
        ck("T.nested_dropped", outerOnly);
        ck("T.empty_cell_space", spaceFilled);
    }
    doc_free(d);

    d = parse("<table><tr><td><b>b</b><a href=\"/l\">L</a></td></tr>"
              "</table>");
    {
        DocBlock* t = find_type(d, DB_TABLE);
        const DocTableCell* c =
            (t != NULL) ? &t->rows[0].cells[0] : NULL;
        ck("T.cell_flags_link",
           c != NULL && c->nInlines == 2 &&
           c->inlines[0].bold && !c->inlines[0].underline &&
           c->inlines[1].underline && c->inlines[1].href != NULL &&
           strcmp(c->inlines[1].href, "http://ex.com/l") == 0 &&
           d->nLinks == 1);   /* <a> still registers the link */
    }
    doc_free(d);

    d = parse("<table><tr><td> lead</td><td>\tmid\n</td></tr></table>");
    {
        DocBlock* t = find_type(d, DB_TABLE);
        /* first cell text loses leading ws; later cells keep trailing ws */
        ck("T.cell_first_ws_strip",
           t != NULL &&
           strcmp(t->rows[0].cells[0].inlines[0].text, "lead") == 0 &&
           strcmp(t->rows[0].cells[1].inlines[0].text, "mid ") == 0);
    }
    doc_free(d);

    /* parity quirk: stray row/cell markup outside a table — the DOM
     * builder drops the elements but their text leaks to the root */
    d = parse("<tr><td>stray</td></tr>");
    ck("T.stray_tr_text_leak", d != NULL && d->nBlocks == 1 &&
       d->blocks[0].type == DB_PARAGRAPH &&
       find_type(d, DB_TABLE) == NULL &&
       txt_is(inl(&d->blocks[0], 0), "stray"));
    doc_free(d);

    d = parse("<td>stray cell</td>");
    ck("T.stray_td_text_leak", d != NULL && d->nBlocks == 1 &&
       txt_is(inl(&d->blocks[0], 0), "stray cell"));
    doc_free(d);

    d = parse("<table><tr><td><h3>h</h3><div>dv</div></td></tr></table>");
    ck("T.block_tags_plain_in_cell",
       d != NULL && find_type(d, DB_HEADING) == NULL &&
       find_type(d, DB_TABLE) != NULL &&
       find_type(d, DB_TABLE)->rows[0].cells[0].nInlines >= 2);
    doc_free(d);
}

/* ── U. forms ─────────────────────────────────────────────────────────── */

static void case_forms(void) {
    DocDocument* d = parse("<input type=hidden value=v>");
    {
        DocBlock* f = find_type(d, DB_INPUT_FIELD);
        ck("U.hidden_block", f != NULL &&
           f->inputType != NULL && strcmp(f->inputType, "hidden") == 0 &&
           f->inName != NULL && strcmp(f->inName, "q") == 0 &&
           f->inValue != NULL && strcmp(f->inValue, "v") == 0);
    }
    doc_free(d);

    d = parse("<input>");
    {
        DocBlock* f = find_type(d, DB_INPUT_FIELD);
        ck("U.text_defaults", f != NULL &&
           f->inputType != NULL && strcmp(f->inputType, "text") == 0 &&
           f->fieldWidth == -1 && f->maxlength == -1 &&
           (f->placeholder == NULL || f->placeholder[0] == '\0') &&
           !f->disabledFlag && !f->readonlyFlag && !f->requiredFlag);
    }
    doc_free(d);

    d = parse("<input type=email name=u value=v1 "
              "placeholder=\"P\" size=30 maxlength=10 "
              "disabled readonly required>");
    {
        DocBlock* f = find_type(d, DB_INPUT_FIELD);
        ck("U.text_attrs", f != NULL &&
           strcmp(f->inputType, "email") == 0 &&
           strcmp(f->inName, "u") == 0 &&
           strcmp(f->inValue, "v1") == 0 &&
           strcmp(f->placeholder, "P") == 0 &&
           f->fieldWidth == 30 && f->maxlength == 10 &&
           f->disabledFlag && f->readonlyFlag && f->requiredFlag &&
           f->blockInert);
    }
    doc_free(d);

    d = parse("<input type=checkbox aria-label=\"AL\">"
              "<input type=checkbox title=T2 name=n2>"
              "<input type=checkbox label=L3 name=n3>");
    {
        DocBlock* f1 = &d->blocks[0];
        DocBlock* f2 = &d->blocks[1];
        DocBlock* f3 = &d->blocks[2];
        ck("U.checkbox_aria_fallback",
           f1->checkboxLabel != NULL &&
           strcmp(f1->checkboxLabel, "AL") == 0);
        ck("U.checkbox_title_fallback",
           f2->checkboxLabel != NULL &&
           strcmp(f2->checkboxLabel, "T2") == 0);
        ck("U.checkbox_label_attr",
           f3->checkboxLabel != NULL &&
           strcmp(f3->checkboxLabel, "L3") == 0);
    }
    doc_free(d);

    d = parse("<input type=checkbox checked><input type=radio name=r>");
    {
        DocBlock* cb = &d->blocks[0];
        DocBlock* rb = &d->blocks[1];
        ck("U.checkbox_radio_flags", cb->type == DB_CHECKBOX_FIELD &&
           !cb->radioFlag && cb->checkedFlag &&
           rb->type == DB_CHECKBOX_FIELD && rb->radioFlag &&
           !rb->checkedFlag &&
           strcmp(rb->checkboxLabel, "r") == 0);
    }
    doc_free(d);

    d = parse("<input type=submit><input type=button value=B>"
              "<input type=file><input type=reset><input type=image>");
    {
        ck("U.submit_labels",
           d->nBlocks == 5 &&
           d->blocks[0].type == DB_INPUT_SUBMIT &&
           strcmp(d->blocks[0].submitLabel, "Submit") == 0 &&
           strcmp(d->blocks[1].submitLabel, "B") == 0 &&
           strcmp(d->blocks[2].submitLabel, "Choose File") == 0 &&
           strcmp(d->blocks[3].submitLabel, "Reset") == 0 &&
           strcmp(d->blocks[4].submitLabel, "Submit") == 0);
    }
    doc_free(d);

    d = parse("<button> Hi </button><button type=reset>X</button>"
              "<button type=button></button>");
    {
        ck("U.button_blocks",
           d->nBlocks == 2 &&
           d->blocks[0].type == DB_INPUT_SUBMIT &&
           strcmp(d->blocks[0].submitLabel, "Hi") == 0 &&
           strcmp(d->blocks[1].submitLabel, "Button") == 0);
    }
    doc_free(d);

    d = parse("<form action=\"/s\" method=POST>"
              "<input name=in1>"
              "<input name=in2 formaction=\"/b2\" formmethod=get>"
              "</form><input name=out>");
    {
        DocBlock* i1 = &d->blocks[0];
        DocBlock* i2 = &d->blocks[1];
        DocBlock* out = &d->blocks[2];
        ck("U.form_context", i1->formAction != NULL &&
           strcmp(i1->formAction, "http://ex.com/s") == 0 &&
           i1->formMethod != NULL && strcmp(i1->formMethod, "post") == 0 &&
           i2->formAction != NULL &&
           strcmp(i2->formAction, "http://ex.com/b2") == 0 &&
           i2->formMethod != NULL && strcmp(i2->formMethod, "get") == 0 &&
           out->formAction == NULL && out->formMethod == NULL);
    }
    doc_free(d);

    d = parse("<textarea rows=4 cols=30 readonly required maxlength=99>"
              "ab\ncd</textarea>");
    {
        DocBlock* f = find_type(d, DB_INPUT_FIELD);
        ck("U.textarea_capture", f != NULL &&
           strcmp(f->inputType, "textarea") == 0 &&
           f->inName != NULL && strcmp(f->inName, "q") == 0 &&
           f->inValue != NULL && strcmp(f->inValue, "ab\ncd") == 0 &&
           f->fieldWidth == 30 && f->fieldRows == 4 &&
           f->readonlyFlag && f->requiredFlag && f->maxlength == 99);
    }
    doc_free(d);

    d = parse("<textarea name=ta disabled>x</textarea>");
    {
        DocBlock* f = find_type(d, DB_INPUT_FIELD);
        ck("U.textarea_disabled", f != NULL &&
           strcmp(f->inName, "ta") == 0 && f->disabledFlag &&
           f->readonlyFlag && f->blockInert);
    }
    doc_free(d);

    d = parse("<select name=s multiple><option>a"
              "<option value=v2 selected><option disabled selected=c>d"
              "</select>");
    {
        DocBlock* sel = find_type(d, DB_SELECT_FIELD);
        ck("U.select_basic", sel != NULL && sel->nOptions == 3 &&
           sel->multipleFlag && sel->selectedIndex == 2 &&
           strcmp(sel->options[0].value, "a") == 0 &&
           strcmp(sel->options[1].value, "v2") == 0 &&
           sel->options[2].disabled);
    }
    doc_free(d);

    d = parse("<select><optgroup label=\"G\"><option>a"
              "<option>b</optgroup><option>c</select>");
    {
        DocBlock* sel = find_type(d, DB_SELECT_FIELD);
        ck("U.select_optgroup", sel != NULL && sel->nOptions == 4 &&
           sel->options[0].group && sel->options[0].disabled &&
           strcmp(sel->options[0].text, "G") == 0 &&
           strcmp(sel->options[1].text, "a") == 0 &&
           strcmp(sel->options[2].text, "b") == 0 &&
           strcmp(sel->options[3].text, "c") == 0);
    }
    doc_free(d);

    d = parse("<select><p>not an option</p></select>");
    ck("U.select_empty_skip",
       d != NULL && find_type(d, DB_SELECT_FIELD) == NULL);
    doc_free(d);

    d = parse("<select><option label=\"Lab\" value=vv>Text</option>"
              "</select>");
    {
        DocBlock* sel = find_type(d, DB_SELECT_FIELD);
        ck("U.select_label_wins", sel != NULL && sel->nOptions == 1 &&
           strcmp(sel->options[0].text, "Lab") == 0 &&
           strcmp(sel->options[0].value, "vv") == 0);
    }
    doc_free(d);
}

/* ── V. bordered boxes ────────────────────────────────────────────────── */

static void case_boxes(void) {
    DocDocument* d = parse("<fieldset disabled><legend>LG</legend>"
                           "<input name=i></fieldset>");
    {
        int opens = 0, closes = 0;
        DocBlock* openBlk = NULL;
        for (size_t i = 0; i < d->nBlocks; i++) {
            if (d->blocks[i].type == DB_BOX_OPEN) {
                opens++;
                if (openBlk == NULL) openBlk = &d->blocks[i];
            }
            if (d->blocks[i].type == DB_BOX_CLOSE) closes++;
        }
        DocBlock* inp = find_type(d, DB_INPUT_FIELD);
        ck("V.fieldset_legend", opens == 1 && closes == 1 &&
           openBlk != NULL && openBlk->boxLabel != NULL &&
           strcmp(openBlk->boxLabel, "LG") == 0 &&
           openBlk->toggleKey == NULL);
        ck("V.fieldset_disables_inner", inp != NULL && inp->disabledFlag);
    }
    doc_free(d);

    d = parse("<details open><summary>  S   u </summary>BODY</details>");
    {
        DocBlock* openBlk = find_type(d, DB_BOX_OPEN);
        DocBlock* closeBlk = find_type(d, DB_BOX_CLOSE);
        ck("V.details_open", openBlk != NULL && closeBlk != NULL &&
           openBlk->toggleKey != NULL &&
           strcmp(openBlk->toggleKey, "d1") == 0 &&
           openBlk->toggleOpen == 1 && closeBlk->toggleOpen == 1 &&
           openBlk->boxLabel != NULL &&
           strcmp(openBlk->boxLabel, "> S u") == 0 &&
           closeBlk->toggleKey != NULL &&
           strcmp(closeBlk->toggleKey, "d1") == 0);
        /* summary label excluded from body */
        int sawBody = 0;
        for (size_t i = 0; i < d->nBlocks; i++)
            for (size_t j = 0; j < d->blocks[i].nInlines; j++)
                if (txt_is(&d->blocks[i].inlines[j], "BODY")) sawBody = 1;
        ck("V.details_body_rendered", sawBody);
    }
    doc_free(d);

    d = parse("<details><summary>S2</summary>HIDDEN</details>"
              "<details><summary>S3</summary>H3</details>");
    {
        DocBlock* b1 = find_type(d, DB_BOX_OPEN);
        int sawHidden = 0;
        for (size_t i = 0; i < d->nBlocks; i++)
            for (size_t j = 0; j < d->blocks[i].nInlines; j++)
                if (txt_is(&d->blocks[i].inlines[j], "HIDDEN")) sawHidden = 1;
        ck("V.details_closed_default", b1 != NULL && b1->toggleOpen == 0 &&
           !sawHidden);
        ck("V.details_counter_seq",
           d->nBlocks >= 4 && d->blocks[2].type == DB_BOX_OPEN &&
           d->blocks[2].toggleKey != NULL &&
           strcmp(d->blocks[2].toggleKey, "d2") == 0);
    }
    doc_free(d);

    {
        const char* html =
            "<details><summary>A</summary>BODY1</details>"
            "<details open><summary>B</summary>BODY2</details>";
        DocDetailsOverride ovr[2] = {
            {"d1", 1},
            {"d2", 0},
        };
        DocParseOpts po;
        po.detailsOverrides = ovr;
        po.nOverrides = 2;
        DocDocument* dd =
            doc_parse_opts(html, BASE, PLUTO_MODE_RAW_HTML, &po);
        int sawBody1 = 0, sawBody2 = 0;
        for (size_t i = 0; i < dd->nBlocks; i++) {
            for (size_t j = 0; j < dd->blocks[i].nInlines; j++) {
                if (txt_is(&dd->blocks[i].inlines[j], "BODY1")) sawBody1 = 1;
                if (txt_is(&dd->blocks[i].inlines[j], "BODY2")) sawBody2 = 1;
            }
        }
        ck("V.details_override_open_close",
           sawBody1 && !sawBody2 &&
           dd->blocks[0].toggleOpen == 1 &&
           dd->blocks[2].toggleOpen == 0);
        doc_free(dd);
    }

    d = parse("<dialog><p>never</p></dialog>");
    /* nothing rendered -> the empty-page notice is the only block */
    ck("V.dialog_closed_hidden", d != NULL && d->nBlocks == 1 &&
       find_type(d, DB_BOX_OPEN) == NULL &&
       txt_is(inl(&d->blocks[0], 0), "(Empty Web Page)") &&
       inl(&d->blocks[0], 0)->italic);
    doc_free(d);

    d = parse("<dialog open>DTEXT</dialog>");
    {
        /* parity quirk: the pending paragraph flushes AFTER box_close */
        ck("V.dialog_open_pair", d != NULL && d->nBlocks == 3 &&
           d->blocks[0].type == DB_BOX_OPEN &&
           (d->blocks[0].boxLabel == NULL ||
            d->blocks[0].boxLabel[0] == '\0') &&
           d->blocks[1].type == DB_BOX_CLOSE &&
           txt_is(inl(&d->blocks[2], 0), "DTEXT"));
    }
    doc_free(d);

    d = parse("<fieldset><div><fieldset>inner</fieldset></div></fieldset>");
    {
        int opens = 0;
        for (size_t i = 0; i < d->nBlocks; i++)
            if (d->blocks[i].type == DB_BOX_OPEN) opens++;
        ck("V.fieldset_nests", opens == 2);
    }
    doc_free(d);
}

/* ── W. media / metadata branches ─────────────────────────────────────── */

static void case_media_misc(void) {
    DocDocument* d =
        parse("<video src=\"v.mp4\" width=999 height=999 title=\"T\">"
              "</video>");
    {
        DocBlock* ph = find_type(d, DB_PLACEHOLDER);
        ck("W.video_placeholder", ph != NULL &&
           strcmp(ph->phTag, "video") == 0 &&
           strcmp(ph->boxLabel, "T") == 0 &&
           ph->width == 360 && ph->height == 120);
    }
    doc_free(d);

    d = parse("<video><source src=\"s.mp4\"></video>");
    {
        DocBlock* ph = find_type(d, DB_PLACEHOLDER);
        ck("W.source_fallback", ph != NULL &&
           strcmp(ph->boxLabel, "[video: s.mp4]") == 0);
    }
    doc_free(d);

    d = parse("<canvas></canvas>");
    {
        DocBlock* ph = find_type(d, DB_PLACEHOLDER);
        ck("W.canvas_no_src", ph != NULL &&
           strcmp(ph->boxLabel, "[canvas]") == 0 &&
           ph->width == 160 && ph->height == 60);
    }
    doc_free(d);

    d = parse("<iframe src=\"/f\"></iframe><embed>");
    {
        DocBlock* ph = find_type(d, DB_PLACEHOLDER);
        DocBlock* ph2 = (d != NULL && d->nBlocks > 1) ? &d->blocks[1] : NULL;
        ck("W.iframe_href_embed", ph != NULL &&
           ph->phHref != NULL &&
           strcmp(ph->phHref, "http://ex.com/f") == 0 &&
           ph2 != NULL && ph2->type == DB_PLACEHOLDER &&
           strcmp(ph2->phTag, "embed") == 0);
    }
    doc_free(d);

    d = parse("<progress value=7 max=0 title=P></progress>"
              "<meter min=1 low=2 high=9 optimum=4 max=10 value=3></meter>");
    {
        ck("W.progress_clamp", d != NULL && d->nBlocks == 2 &&
           d->blocks[0].type == DB_METER &&
           d->blocks[0].mValue == 7 &&
           d->blocks[0].mMax == 1 && d->blocks[0].mHigh == 1 &&
           d->blocks[0].boxLabel != NULL &&
           strcmp(d->blocks[0].boxLabel, "P") == 0);
        DocBlock* m = &d->blocks[1];
        ck("W.meter_fields", m->type == DB_METER && m->mValue == 3 &&
           m->mMax == 10 && m->mMin == 1 && m->mLow == 2 &&
           m->mHigh == 9 && m->mOptimum == 4);
    }
    doc_free(d);

    d = parse("<datalist id=langs><option value=cpp>C++"
              "<option>Lua</datalist>");
    {
        /* parity quirk: the DOM builder drops <option> without a
         * <select> ancestor, so the list keeps its id but no entries */
        ck("W.datalist_meta", d != NULL && d->nDatalists == 1 &&
           d->datalists[0].id != NULL &&
           strcmp(d->datalists[0].id, "langs") == 0 &&
           d->datalists[0].nOpts == 0 &&
           find_type(d, DB_SELECT_FIELD) == NULL);
    }
    doc_free(d);

    d = parse("<map name=\"#m\"><area coords=\"1,2 3\" href=\"/a\" alt=A>"
              "<area shape=circle href=\"javascript:x\"></map>");
    {
        ck("W.map_areas", d != NULL && d->nMaps == 1 &&
           d->maps[0].name != NULL && strcmp(d->maps[0].name, "m") == 0 &&
           d->maps[0].nRegions == 2 &&
           strcmp(d->maps[0].regions[0].shape, "rect") == 0 &&
           d->maps[0].regions[0].nCoords == 3 &&
           d->maps[0].regions[0].coords[0] == 1 &&
           d->maps[0].regions[0].coords[2] == 3 &&
           d->maps[0].regions[0].href != NULL &&
           strcmp(d->maps[0].regions[0].href,
                  "http://ex.com/a") == 0 &&
           strcmp(d->maps[0].regions[1].shape, "circle") == 0 &&
           d->maps[0].regions[1].href == NULL);
    }
    doc_free(d);

    d = parse("<fencedframe width=999 height=-3></fencedframe>");
    {
        DocBlock* ph = find_type(d, DB_PLACEHOLDER);
        ck("W.fencedframe", ph != NULL &&
           strcmp(ph->phTag, "fencedframe") == 0 &&
           strcmp(ph->boxLabel, "[fencedframe]") == 0 &&
           ph->width == 360 && ph->height == -3);
    }
    doc_free(d);

    d = parse("<template><p>TPL</p></template>");
    ck("W.template_inert", d != NULL && d->nBlocks == 1 &&
       txt_is(inl(&d->blocks[0], 0), "(Empty Web Page)") &&
       inl(&d->blocks[0], 0)->italic);
    doc_free(d);

    d = parse("<col><colgroup span=2></colgroup><track>"
              "<param name=a value=b><frameset><frame></frameset>"
              "<menuitem>M</menuitem>");
    ck("W.voids_silent", d != NULL && d->nBlocks == 1 &&
       find_type(d, DB_TABLE) == NULL &&
       txt_is(inl(&d->blocks[0], 0), "(Empty Web Page)"));
    doc_free(d);
}

/* ── X. svg / MathML ──────────────────────────────────────────────────── */

static void case_svg_math(void) {
    DocDocument* d =
        parse("<svg width=50 height=20><circle cx=\"5\"/></svg>");
    {
        DocBlock* img = find_type(d, DB_IMAGE);
        ck("X.svg_serialize", img != NULL && img->imgIsSvg == 1 &&
           img->svgXml != NULL &&
           strstr(img->svgXml, "<circle cx=\"5\"/>") != NULL &&
           img->width == 50 && img->height == 20);
    }
    doc_free(d);

    d = parse("<svg viewBox=\"10 20 300 150\"></svg>");
    {
        DocBlock* img = find_type(d, DB_IMAGE);
        /* parity quirk: attribute keys are lowercased by the tokenizer in
         * BOTH implementations, so the "viewBox" lookup never matches and
         * the fallback sizes stay at the defaults */
        ck("X.svg_viewbox_quirk", img != NULL &&
           img->width == 120 && img->height == 40);
    }
    doc_free(d);

    d = parse("<svg width=999 height=999></svg><svg></svg>");
    {
        DocBlock* i1 = find_type(d, DB_IMAGE);
        DocBlock* i2 = (d != NULL && d->nBlocks > 1) ? &d->blocks[1] : NULL;
        ck("X.svg_defaults_clamp", i1 != NULL &&
           i1->width == 360 && i1->height == 180 &&
           i2 != NULL && i2->imgIsSvg == 1 &&
           i2->width == 120 && i2->height == 40);
    }
    doc_free(d);

    d = parse("<svg role=img aria-label=\"Icon\"></svg>"
              "<svg role=img title=\"Ti\"></svg><svg><desc>d</desc></svg>");
    {
        DocBlock* i1 = find_type(d, DB_IMAGE);
        DocBlock* i2 = (d != NULL && d->nBlocks > 1) ? &d->blocks[1] : NULL;
        DocBlock* i3 = (d != NULL && d->nBlocks > 2) ? &d->blocks[2] : NULL;
        ck("X.svg_alt_rules", i1 != NULL &&
           i1->alt != NULL && strcmp(i1->alt, "Icon") == 0 &&
           i2 != NULL && i2->alt != NULL && strcmp(i2->alt, "Ti") == 0 &&
           i3 != NULL && i3->alt != NULL && i3->alt[0] == '\0');
    }
    doc_free(d);

    d = parse("<math><mfrac><mi>a</mi><mn>b</mn></mfrac></math>"
              "<math><msup><mi>x</mi><mn>2</mn></msup></math>"
              "<math><msub><mi>y</mi><mn>1</mn></msub></math>"
              "<math><msubsup><mi>z</mi><mn>1</mn><mn>2</mn></msubsup></math>"
              "<math><msqrt><mi>w</mi></msqrt></math>"
              "<math><mroot><mi>8</mi><mn>3</mn></mroot></math>");
    {
        int ok = d != NULL && d->nBlocks == 6;
        if (ok) {
            /* parity quirk: mroot emits "^(1/" and the closing paren
             * BEFORE walking the last child, exactly like the source */
            const char* e[] = {"a / b", "x^2", "y_1",
                               "z_1^2", "sqrt(w)", "sqrt(8^(1/)3)"};
            for (int i = 0; i < 6 && ok; i++) {
                DocBlock* b = &d->blocks[i];
                ok = b->type == DB_MATH && b->codeText != NULL &&
                     strcmp(b->codeText, e[i]) == 0;
            }
        }
        ck("X.math_linearized", ok);
    }
    doc_free(d);

    d = parse("<math><mfenced open=\"[\" close=\"]\" separators=\";\">"
              "<mi>a</mi><mi>b</mi></mfenced></math>");
    {
        DocBlock* b = find_type(d, DB_MATH);
        ck("X.mfenced_seps", b != NULL && b->codeText != NULL &&
           strcmp(b->codeText, "[a;b]") == 0);
    }
    doc_free(d);

    d = parse("<math><mi> a </mi> <mo>+</mo>\n<mi>b</mi></math>");
    {
        DocBlock* b = find_type(d, DB_MATH);
        ck("X.math_ws_collapse", b != NULL && b->codeText != NULL &&
           strcmp(b->codeText, "a + b") == 0);
    }
    doc_free(d);

    d = parse("<math><mtext></mtext></math>");
    ck("X.math_empty_skipped",
       d != NULL && find_type(d, DB_MATH) == NULL);
    doc_free(d);
}

/* ── Y. misc parity ───────────────────────────────────────────────────── */

static void case_misc_parity(void) {
    DocDocument* d = parse("<address>ad</address><hgroup>hg</hgroup>");
    ck("Y.address_hgroup_paragraphs", d != NULL && d->nBlocks == 2 &&
       d->blocks[0].type == DB_PARAGRAPH &&
       txt_is(inl(&d->blocks[0], 0), "ad") &&
       d->blocks[1].type == DB_PARAGRAPH &&
       txt_is(inl(&d->blocks[1], 0), "hg"));
    doc_free(d);

    d = parse("<figure><figcaption>OnlyCap</figcaption></figure>");
    {
        DocBlock* p = find_type(d, DB_PARAGRAPH);
        ck("Y.figure_caption_only_para", p != NULL &&
           p->align != NULL && strcmp(p->align, "center") == 0 &&
           p->nInlines == 1 && inl(p, 0)->italic &&
           txt_is(inl(p, 0), "OnlyCap") &&
           find_type(d, DB_IMAGE) == NULL);
    }
    doc_free(d);

    d = parse("<h2 style=\"margin: 4 2\">t</h2>");
    {
        DocBlock* h = find_type(d, DB_HEADING);
        /* 2-value shorthand: top/bottom from value #1, sides from #2 */
        ck("Y.heading_css_spacing", h != NULL &&
           h->spacingTop == 2 && h->spacingBottom == 2 && h->indent == 1);
    }
    doc_free(d);

    d = parse("<p>a</p><div hidden>b<div><span inert>c</span></div></div>"
              "<p>d</p><span inert>e<span>f</span></span>");
    {
        int ok = d != NULL;
        /* hidden subtree skipped entirely; inert flags only inside spans */
        if (ok) {
            int sawC = 0;
            for (size_t i = 0; i < d->nBlocks; i++)
                for (size_t j = 0; j < d->blocks[i].nInlines; j++)
                    if (txt_is(&d->blocks[i].inlines[j], "c")) sawC = 1;
            DocBlock* last = &d->blocks[d->nBlocks - 1];
            ok = !sawC && d->nBlocks == 3 &&
                 txt_is(inl(last, 0), "e") && inl(last, 0)->inert &&
                 txt_is(inl(last, 1), "f") && inl(last, 1)->inert;
        }
        ck("Y.inert_scope_restored", ok);
    }
    doc_free(d);
}

/* ── entry ────────────────────────────────────────────────────────────── */

int selftest_document_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;

    case_empty();
    case_reader_gate();
    case_headings();
    case_paragraphs();
    case_formats();
    case_breaks();
    case_pre();
    case_span();
    case_quote();
    case_links();
    case_lists();
    case_dl();
    case_figure();
    case_images();
    case_visibility();
    case_meta();
    case_base();
    case_caps();
    case_tables();
    case_forms();
    case_boxes();
    case_media_misc();
    case_svg_math();
    case_misc_parity();

    *passed = s_pass;
    *failed = s_fail;
    PLUTO_LOG("[P11] document selftests done: %d passed, %d failed",
              s_pass, s_fail);
    return s_fail;
}


