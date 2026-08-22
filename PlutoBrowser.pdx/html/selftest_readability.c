#include "html/selftest_readability.h"
#include "html/readability.h"
#include "html/document.h"
#include "html/tokenizer.h"
#include "core/constants.h"
#include "core/logger.h"
#include "util/mem.h"
#include "util/strbuf.h"
#include <string.h>
#include <stdio.h>

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P12] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P12] FAIL %s", name); }
}

#define BASE "http://ex.com/dir/page.html"

static DocDocument* reader(const char* html) {
    return doc_parse(html, BASE, PLUTO_MODE_READER);
}

/* concat all text of an inline-carrier block */
static char* blk_text(const DocBlock* b) {
    StrBuf sb;
    sb_init(&sb);
    if (b != NULL)
        for (size_t i = 0; i < b->nInlines; i++)
            if (b->inlines[i].text != NULL)
                sb_append_str(&sb, b->inlines[i].text);
    return sb_detach(&sb);
}

static const DocBlock* find_blk(const DocDocument* d, int type, size_t nth) {
    if (d == NULL) return NULL;
    size_t seen = 0;
    for (size_t i = 0; i < d->nBlocks; i++) {
        if (d->blocks[i].type == type) {
            if (seen == nth) return &d->blocks[i];
            seen++;
        }
    }
    return NULL;
}

static int doc_contains_text(const DocDocument* d, const char* needle) {
    if (d == NULL) return 0;
    for (size_t i = 0; i < d->nBlocks; i++) {
        const DocBlock* b = &d->blocks[i];
        if (b->codeText != NULL && strstr(b->codeText, needle)) return 1;
        if (b->alt != NULL && strstr(b->alt, needle)) return 1;
        for (size_t k = 0; k < b->nInlines; k++)
            if (b->inlines[k].text != NULL &&
                strstr(b->inlines[k].text, needle)) return 1;
    }
    return 0;
}

static void dump_blocks(const char* label, const DocDocument* d) {
    PLUTO_LOG("[P12] dump %s: %zu blocks", label,
              (d != NULL) ? d->nBlocks : (size_t)0);
    if (d == NULL) return;
    for (size_t i = 0; i < d->nBlocks; i++) {
        const DocBlock* b = &d->blocks[i];
        char* t = blk_text(b);
        switch (b->type) {
        case DB_READER_HEADER:
            PLUTO_LOG("[P12]   %2zu READER_HEADER host=%s title=%s rt=%s",
                      i, b->readerHost ? b->readerHost : "",
                      b->readerTitle ? b->readerTitle : "",
                      b->readingTime ? b->readingTime : "");
            break;
        case DB_HEADING:
            PLUTO_LOG("[P12]   %2zu HEADING h%d [%s]", i, b->level, t);
            break;
        case DB_HR:
            PLUTO_LOG("[P12]   %2zu HR", i);
            break;
        case DB_CODE_BLOCK:
            PLUTO_LOG("[P12]   %2zu CODE [%s]", i,
                      b->codeText ? b->codeText : "");
            break;
        case DB_IMAGE:
            PLUTO_LOG("[P12]   %2zu IMAGE %dx%d alt=%s src=%s", i,
                      b->width, b->height, b->alt ? b->alt : "",
                      b->src ? b->src : "");
            break;
        case DB_INPUT_FIELD:
            PLUTO_LOG("[P12]   %2zu INPUT type=%s name=%s ph=%s act=%s "
                      "meth=%s", i, b->inputType ? b->inputType : "",
                      b->inName ? b->inName : "",
                      b->placeholder ? b->placeholder : "",
                      b->formAction ? b->formAction : "",
                      b->formMethod ? b->formMethod : "");
            break;
        case DB_INPUT_SUBMIT:
            PLUTO_LOG("[P12]   %2zu SUBMIT label=%s act=%s meth=%s", i,
                      b->submitLabel ? b->submitLabel : "",
                      b->formAction ? b->formAction : "",
                      b->formMethod ? b->formMethod : "");
            break;
        case DB_LIST_ITEM:
            PLUTO_LOG("[P12]   %2zu LI ordered=%d num=%d [%s]", i,
                      b->isOrdered, b->number, t);
            break;
        default:
            PLUTO_LOG("[P12]   %2zu type=%d [%s]", i, b->type, t);
            break;
        }
        pluto_free(t);
    }
}

/* ── A. basics ────────────────────────────────────────────────────────── */

static void case_basics(void) {
    DocDocument* d = reader("<p>Hello brave world</p>");
    ck("A.mode_flag", d != NULL && d->isReaderMode == 1);
    ck("A.raw_html_kept", d != NULL && d->rawHtml != NULL &&
       strcmp(d->rawHtml, "<p>Hello brave world</p>") == 0);
    ck("A.base_kept", d != NULL && d->baseUrl != NULL &&
       strcmp(d->baseUrl, BASE) == 0);
    ck("A.block_count", d != NULL && d->nBlocks == 4);
    ck("A.header_host", d != NULL && d->blocks[0].readerHost != NULL &&
       strcmp(d->blocks[0].readerHost, "EX.COM") == 0);
    ck("A.header_title_fallback", d != NULL &&
       d->blocks[0].readerTitle != NULL &&
       strcmp(d->blocks[0].readerTitle, "Web Page") == 0);
    ck("A.reading_time_math", d != NULL &&
       d->blocks[0].readingTime != NULL &&
       strcmp(d->blocks[0].readingTime, "1 min read (3 words)") == 0 &&
       d->readerWords == 3 && d->readerTime != NULL &&
       strcmp(d->readerTime, "1 min read (3 words)") == 0);
    ck("A.title_h1_bold", d != NULL && d->blocks[1].type == DB_HEADING &&
       d->blocks[1].level == 1 && d->blocks[1].nInlines == 1 &&
       d->blocks[1].inlines[0].bold &&
       strcmp(d->blocks[1].inlines[0].text, "Web Page") == 0);
    ck("A.hr_third", d != NULL && d->blocks[2].type == DB_HR);
    ck("A.paragraph_fourth", d != NULL &&
       d->blocks[3].type == DB_PARAGRAPH);
    {
        char* t = blk_text(find_blk(d, DB_PARAGRAPH, 0));
        ck("A.paragraph_text", t != NULL &&
           strcmp(t, "Hello brave world") == 0);
        pluto_free(t);
    }
    dump_blocks("basics", d);
    doc_free(d);

    /* empty input keeps the blank-document shortcut (no reader branch) */
    d = doc_parse("", BASE, PLUTO_MODE_READER);
    ck("A.empty_blank_doc", d != NULL && !d->isReaderMode &&
       d->nBlocks == 0);
    doc_free(d);

    /* direct distill fallbacks: NULL title -> Untitled Article, "" -> BLANK */
    HttTokens* toks = htt_tokenize("<p>x</p>", 7);
    DocDocument* rd =
        readability_distill(toks, NULL, "");
    ck("A.fallback_title_host", rd != NULL &&
       rd->blocks[0].readerTitle != NULL &&
       strcmp(rd->blocks[0].readerTitle, "Untitled Article") == 0 &&
       rd->blocks[0].readerHost != NULL &&
       strcmp(rd->blocks[0].readerHost, "BLANK") == 0);
    dump_blocks("fallbacks", rd);
    doc_free(rd);
    htt_free(toks);
}

/* ── B. mergeParagraphFragments ───────────────────────────────────────── */

static void case_merge(void) {
    DocDocument* d = reader(
        "<p>Welcome to Wikipedia,</p><div>the free encyclopedia</div>"
        "<p>Ends here.</p><div>new frag</div>");
    /* parity quirk: the running accumulator has no sentence-ending punct
     * until "Ends here." lands, so p1..p3 ALL merge; "new frag" then
     * stays separate. Inline texts join raw (no injected space). */
    ck("B.merged_count", d != NULL && d->nBlocks == 5);
    {
        const DocBlock* m = find_blk(d, DB_PARAGRAPH, 0);
        char* t = blk_text(m);
        ck("B.merged_text", m != NULL && t != NULL &&
           strcmp(t, "Welcome to Wikipedia,"
                     "the free encyclopediaEnds here.") == 0 &&
           m->nInlines == 3);
        pluto_free(t);
        const DocBlock* p4 = find_blk(d, DB_PARAGRAPH, 1);
        char* t4 = blk_text(p4);
        ck("B.sentence_boundary", p4 != NULL && t4 != NULL &&
           strcmp(t4, "new frag") == 0);
        pluto_free(t4);
    }
    dump_blocks("merge", d);
    doc_free(d);
}

/* ── C. bare-url anchors hidden ───────────────────────────────────────── */

static void case_bare_url(void) {
    DocDocument* d = reader(
        "<p><a href=\"/wiki/M\">en.wikipedia.org/wiki/Mesklin</a>"
        "<a href=\"https://x.org\">https://x.org</a>"
        "<a href=\"/keep\">real anchor text</a>"
        "<a href=\"/sp\">two words stay</a></p>");
    {
        const DocBlock* p = find_blk(d, DB_PARAGRAPH, 0);
        ck("C.bare_dropped_kept", p != NULL && p->nInlines == 2 &&
           p->inlines[0].text != NULL &&
           strcmp(p->inlines[0].text, "real anchor text") == 0 &&
           p->inlines[0].underline &&
           p->inlines[0].href != NULL &&
           strcmp(p->inlines[0].href, "http://ex.com/keep") == 0 &&
           p->inlines[1].text != NULL &&
           strcmp(p->inlines[1].text, "two words stay") == 0);
    }
    dump_blocks("bare_url", d);
    doc_free(d);
}

/* ── D. STRIP_TAGS ────────────────────────────────────────────────────── */

static void case_strip(void) {
    DocDocument* d = reader(
        "<nav>NAVTEXT</nav><header>HEADERTXT</header>"
        "<footer>FOOTERTXT</footer><aside>ASIDETEXT</aside>"
        "<script>SCRIPTTXT</script><style>.x{}</style>"
        "<noscript>NOSTXT</noscript><iframe src=\"/e\">IFTXT</iframe>"
        "<svg>SVGTXT</svg>"
        "<p>kept paragraph</p>");
    ck("D.stripped_absent", d != NULL &&
       !doc_contains_text(d, "NAVTEXT") &&
       !doc_contains_text(d, "HEADERTXT") &&
       !doc_contains_text(d, "FOOTERTXT") &&
       !doc_contains_text(d, "ASIDETEXT") &&
       !doc_contains_text(d, "SCRIPTTXT") &&
       !doc_contains_text(d, ".x{}") &&
       !doc_contains_text(d, "NOSTXT") &&
       !doc_contains_text(d, "IFTXT") &&
       !doc_contains_text(d, "SVGTXT"));
    ck("D.kept_present", doc_contains_text(d, "kept paragraph"));
    dump_blocks("strip", d);
    doc_free(d);
}

/* ── E. headings / lists / pre / quote ────────────────────────────────── */

static void case_structure(void) {
    DocDocument* d = reader(
        "<h3>Title Three</h3><ul><li>Alpha item</li><li>Beta item</li></ul>"
        "<ol><li>Num one</li></ol>"
        "<pre>a   b\nline2</pre>"
        "<blockquote>quoted words</blockquote>");
    ck("E.heading_level", d != NULL);
    {
        const DocBlock* h = find_blk(d, DB_HEADING, 1); /* [0] is the h1 */
        ck("E.h3", h != NULL && h->level == 3);
        const DocBlock* l1 = find_blk(d, DB_LIST_ITEM, 0);
        const DocBlock* l2 = find_blk(d, DB_LIST_ITEM, 1);
        const DocBlock* l3 = find_blk(d, DB_LIST_ITEM, 2);
        ck("E.list_numbers", l1 != NULL && l2 != NULL && l3 != NULL &&
           !l1->isOrdered && l1->number == 1 &&
           !l2->isOrdered && l2->number == 2 &&
           l3->isOrdered && l3->number == 1);
        const DocBlock* cb = find_blk(d, DB_CODE_BLOCK, 0);
        ck("E.pre_verbatim", cb != NULL && cb->codeText != NULL &&
           strcmp(cb->codeText, "a   b\nline2") == 0);
        const DocBlock* q = find_blk(d, DB_BLOCKQUOTE, 0);
        char* qt = blk_text(q);
        ck("E.blockquote_type", q != NULL && qt != NULL &&
           strcmp(qt, "quoted words") == 0);
        pluto_free(qt);
    }
    dump_blocks("structure", d);
    doc_free(d);
}

/* ── F. images ────────────────────────────────────────────────────────── */

static void case_images(void) {
    DocDocument* d = reader(
        "<img src=\"/pic.png\" alt=\"Pic\" width=\"800\" height=\"99\">"
        "<img src=\"/tracking.gif\" alt=\"t\">"
        "<img src=\"/beacon.png\">"
        "<img srcset=\"/first.x 1x, /second.y 2x\" alt=\"ss\">"
        "<img data-src=\"/lazy.png\">"
        "<img src=\"/noalt.png\">");
    {
        const DocBlock* i0 = find_blk(d, DB_IMAGE, 0);
        ck("F.clamp_resolve_alt", i0 != NULL &&
           i0->src != NULL && strcmp(i0->src, "http://ex.com/pic.png") == 0 &&
           i0->width == 360 && i0->height == 99 &&
           i0->alt != NULL && strcmp(i0->alt, "Pic") == 0);
        ck("F.tracking_skipped",
           find_blk(d, DB_IMAGE, 1) != NULL &&
           find_blk(d, DB_IMAGE, 2) != NULL &&
           find_blk(d, DB_IMAGE, 3) != NULL &&
           find_blk(d, DB_IMAGE, 4) == NULL);
        const DocBlock* ss = find_blk(d, DB_IMAGE, 1);
        ck("F.srcset_first", ss != NULL && ss->src != NULL &&
           strcmp(ss->src, "http://ex.com/first.x") == 0 &&
           strcmp(ss->alt, "ss") == 0);
        const DocBlock* lz = find_blk(d, DB_IMAGE, 2);
        ck("F.data_src_default_alt", lz != NULL && lz->src != NULL &&
           strcmp(lz->src, "http://ex.com/lazy.png") == 0 &&
           strcmp(lz->alt, "Image") == 0);
        const DocBlock* na = find_blk(d, DB_IMAGE, 3);
        ck("F.empty_alt_becomes_image", na != NULL &&
           strcmp(na->alt, "Image") == 0 && na->imgHref == NULL);
    }
    dump_blocks("images", d);
    doc_free(d);
}

/* ── G. forms ─────────────────────────────────────────────────────────── */

static void case_forms(void) {
    DocDocument* d = reader(
        "<form action=\"/search\" method=\"POST\">"
        "<input type=text name=q placeholder=Search>"
        "<input type=submit value=Go>"
        "</form>"
        "<textarea name=ta>buffered</textarea>"
        "<button type=\"button\">Click  Me </button>"
        "<input type=checkbox name=c>");
    {
        const DocBlock* f = find_blk(d, DB_INPUT_FIELD, 0);
        ck("G.input_field", f != NULL && f->inputType != NULL &&
           strcmp(f->inputType, "text") == 0 &&
           strcmp(f->inName, "q") == 0 && f->placeholder != NULL &&
           strcmp(f->placeholder, "Search") == 0 &&
           f->formAction != NULL &&
           strcmp(f->formAction, "http://ex.com/search") == 0 &&
           f->formMethod != NULL && strcmp(f->formMethod, "post") == 0);
        const DocBlock* sbm = find_blk(d, DB_INPUT_SUBMIT, 0);
        ck("G.input_submit_value_label", sbm != NULL &&
           sbm->submitLabel != NULL && strcmp(sbm->submitLabel, "Go") == 0 &&
           sbm->formAction != NULL &&
           strcmp(sbm->formAction, "http://ex.com/search") == 0);
        const DocBlock* ta = find_blk(d, DB_INPUT_FIELD, 1);
        ck("G.textarea", ta != NULL && ta->inputType != NULL &&
           strcmp(ta->inputType, "textarea") == 0 &&
           strcmp(ta->inName, "ta") == 0 &&
           ta->inValue != NULL && strcmp(ta->inValue, "buffered") == 0 &&
           ta->placeholder != NULL && ta->placeholder[0] == '\0' &&
           ta->formAction == NULL);
        const DocBlock* btn = find_blk(d, DB_INPUT_SUBMIT, 1);
        ck("G.button_label_trimmed", btn != NULL &&
           btn->submitLabel != NULL &&
           strcmp(btn->submitLabel, "Click  Me") == 0 &&
           btn->formAction == NULL && btn->formMethod != NULL &&
           strcmp(btn->formMethod, "post") == 0);
        ck("G.checkbox_ignored",
           find_blk(d, DB_CHECKBOX_FIELD, 0) == NULL &&
           find_blk(d, DB_INPUT_FIELD, 2) == NULL);
    }
    dump_blocks("forms", d);
    doc_free(d);
}

/* ── H. container scoring / sorted-order selection ────────────────────── */

static void case_selection(void) {
    DocDocument* d = reader(
        "Stray intro line."
        "<article><p>alpha bravo charlie delta echo</p></article>");
    /* article: 5 words *3 => 15 ; root: 3 words => 3 >= 15*.15=2.25 ->
     * BOTH selected, walked in SORTED order: article blocks first even
     * though the stray line came first in the document */
    {
        char* mergedTxt = blk_text(&d->blocks[3]);
        ck("H.sorted_order_walk", d != NULL && d->nBlocks == 4 &&
           mergedTxt != NULL &&
           strcmp(mergedTxt,
                  "alpha bravo charlie delta echoStray intro line.") == 0);
        pluto_free(mergedTxt);
    }
    ck("H.reader_words", d != NULL && d->readerWords == 8);
    dump_blocks("selection", d);
    doc_free(d);
}

int selftest_readability_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_basics();
    case_merge();
    case_bare_url();
    case_strip();
    case_structure();
    case_images();
    case_forms();
    case_selection();
    PLUTO_LOG("[P12] readability selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed != NULL) *passed = s_pass;
    if (failed != NULL) *failed = s_fail;
    return s_fail;
}
