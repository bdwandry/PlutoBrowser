#include "html/selftest_tokenizer.h"
#include "html/tokenizer.h"
#include "core/logger.h"
#include "core/tasks.h"
#include "util/mem.h"
#include <string.h>
#include <stdlib.h>

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P08] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P08] FAIL %s", name); }
}

static void cks(const char* name, const char* got, const char* exp) {
    ck(name, got != NULL && exp != NULL && strcmp(got, exp) == 0);
}

static int tok_is_text(const HttToken* t, const char* exp) {
    return t != NULL && t->type == HTT_TEXT && t->text != NULL &&
           t->textLen == strlen(exp) && memcmp(t->text, exp, t->textLen) == 0;
}

static int tok_is_tag(const HttToken* t, const char* name, int closing, int self) {
    return t != NULL && t->type == HTT_TAG && t->name != NULL &&
           strcmp(t->name, name) == 0 && t->isClosing == closing &&
           t->isSelfClosing == self;
}

static const char* attr_str(const HttToken* t, const char* key) {
    void* v = sm_get(t->attrs, key);
    return (v == NULL || v == HT_ATTR_TRUE) ? NULL : (const char*)v;
}

static int attr_bool(const HttToken* t, const char* key) {
    return sm_get(t->attrs, key) == HT_ATTR_TRUE;
}

#define T(tks, i) (&(tks)->items[i])

static void case_basic(void) {
    HttTokens* t = htt_tokenize("hello world", 11);
    ck("A.nonnull", t != NULL);
    if (!t) return;
    cks("A.title_default", t->pageTitle, "Web Page");
    ck("A.count_1", t->count == 1);
    ck("A.text", tok_is_text(T(t, 0), "hello world"));
    htt_free(t);

    t = htt_tokenize("", 0);
    ck("B.empty_count0", t != NULL && t->count == 0);
    if (t) { cks("B.empty_title", t->pageTitle, "Web Page"); htt_free(t); }

    const char* entIn = "a&amp;b &lt;i&gt;";
    t = htt_tokenize(entIn, strlen(entIn));
    ck("C.entity_decode", t && t->count == 1 && tok_is_text(T(t, 0), "a&b <i>"));
    htt_free(t);
}

static void case_tags(void) {
    HttTokens* t = htt_tokenize("<p>hi</p>", 9);
    ck("D.count_3", t && t->count == 3);
    if (!t) return;
    ck("D.tag_p_open", tok_is_tag(T(t, 0), "p", 0, 0));
    ck("D.text_hi", tok_is_text(T(t, 1), "hi"));
    ck("D.tag_p_close_self", tok_is_tag(T(t, 2), "p", 1, 1));
    htt_free(t);

    t = htt_tokenize("<br/><br /><hr></p>", 19);
    ck("E.count_4", t && t->count == 4);
    if (!t) return;
    ck("E.br_slash", tok_is_tag(T(t, 0), "br", 0, 1));
    ck("E.br_space_slash", tok_is_tag(T(t, 1), "br", 0, 1));
    ck("E.hr_plain", tok_is_tag(T(t, 2), "hr", 0, 0));
    ck("E.close_p_self", tok_is_tag(T(t, 3), "p", 1, 1));
    htt_free(t);

    t = htt_tokenize("<my:tag data-x=1></my:tag>", 26);
    ck("F.colon_names", t && t->count == 2 &&
       tok_is_tag(T(t, 0), "my:tag", 0, 0) && tok_is_tag(T(t, 1), "my:tag", 1, 1));
    if (t && t->count == 2)
        cks("F.attr_datax", attr_str(T(t, 0), "data-x"), "1");
    htt_free(t);

    t = htt_tokenize("<DIV CLASS=\"X\">Y</DIV>", 22);
    ck("G.lowered_names", t && t->count == 3 &&
       tok_is_tag(T(t, 0), "div", 0, 0) && tok_is_tag(T(t, 2), "div", 1, 1));
    if (t && t->count == 3) {
        cks("G.attr_val_case_kept", attr_str(T(t, 0), "class"), "X");
        ck("G.text_Y", tok_is_text(T(t, 1), "Y"));
    }
    htt_free(t);

    t = htt_tokenize("A<b>B</b>C", 10);
    ck("H.interleave", t && t->count == 5 && tok_is_text(T(t, 0), "A") &&
       tok_is_tag(T(t, 1), "b", 0, 0) && tok_is_text(T(t, 2), "B") &&
       tok_is_tag(T(t, 3), "b", 1, 1) && tok_is_text(T(t, 4), "C"));
    htt_free(t);

    t = htt_tokenize("3 < 4 > 2", 9);
    ck("I.lt_space_gt_quirk", t && t->count == 3 && tok_is_text(T(t, 0), "3 ") &&
       tok_is_tag(T(t, 1), "4", 0, 0) && tok_is_text(T(t, 2), " 2"));
    htt_free(t);

    t = htt_tokenize("A<>B<?php echo ?>C", 18);
    ck("J.junk_dropped", t && t->count == 3 && tok_is_text(T(t, 0), "A") &&
       tok_is_text(T(t, 1), "B") && tok_is_text(T(t, 2), "C"));
    htt_free(t);

    t = htt_tokenize("<!DOCTYPE html><p>x", 19);
    ck("K.doctype_vanishes", t && t->count == 2 &&
       tok_is_tag(T(t, 0), "p", 0, 0) && tok_is_text(T(t, 1), "x"));
    htt_free(t);

    t = htt_tokenize("<p>ok<div unfinished", 20);
    ck("L.broken_tail_dropped", t && t->count == 2 &&
       tok_is_tag(T(t, 0), "p", 0, 0) && tok_is_text(T(t, 1), "ok"));
    htt_free(t);
}

static void case_attrs(void) {
    const char* in =
        "<a href=\"x?a=1&amp;b=2\" id='s q' class=big disabled data-x>y</a>";
    HttTokens* t = htt_tokenize(in, strlen(in));
    ck("M.count_3", t && t->count == 3);
    if (!t) return;
    const HttToken* a = T(t, 0);
    ck("M.tag_a", tok_is_tag(a, "a", 0, 0));
    if (tok_is_tag(a, "a", 0, 0)) {
        cks("M.href_decoded", attr_str(a, "href"), "x?a=1&b=2");
        cks("M.id_single_quoted", attr_str(a, "id"), "s q");
        cks("M.unquoted", attr_str(a, "class"), "big");
        ck("M.boolean_disabled", attr_bool(a, "disabled"));
        ck("M.boolean_datax", attr_bool(a, "data-x"));
        ck("M.attr_count_5", sm_count(a->attrs) == 5);
    }
    ck("M.text_y", tok_is_text(T(t, 1), "y"));
    ck("M.close_a", tok_is_tag(T(t, 2), "a", 1, 1));
    htt_free(t);

    const char* dupIn = "<img src=\"1\" src=\"2\" ALT=\"A\" alt=\"B\">";
    t = htt_tokenize(dupIn, strlen(dupIn));
    ck("N.first_wins", t && t->count == 1 && tok_is_tag(T(t, 0), "img", 0, 0));
    if (t && t->count == 1) {
        cks("N.src_first", attr_str(T(t, 0), "src"), "1");
        cks("N.alt_first_lowered_key", attr_str(T(t, 0), "alt"), "A");
        ck("N.attr_count_2", sm_count(T(t, 0)->attrs) == 2);
    }
    htt_free(t);

    t = htt_tokenize("<div a.b=c d-e:f=g>", 19);
    ck("O.key_dot_quirk", t && t->count == 1 && tok_is_tag(T(t, 0), "div", 0, 0));
    if (t && t->count == 1) {
        ck("O.a_bool", attr_bool(T(t, 0), "a"));
        cks("O.b_c", attr_str(T(t, 0), "b"), "c");
        cks("O.d_e_f_g", attr_str(T(t, 0), "d-e:f"), "g");
        ck("O.attr_count_3", sm_count(T(t, 0)->attrs) == 3);
    }
    htt_free(t);

    t = htt_tokenize("x<a href =\"v\">", 14);
    ck("P.space_before_eq_quirk", t && t->count == 2 && tok_is_text(T(t, 0), "x") &&
       tok_is_tag(T(t, 1), "a", 0, 0));
    if (t && t->count == 2) {
        cks("P.href_empty", attr_str(T(t, 1), "href"), "");
        ck("P.v_bool_lost_value", attr_bool(T(t, 1), "v"));
        ck("P.attr_count_2", sm_count(T(t, 1)->attrs) == 2);
    }
    htt_free(t);

    t = htt_tokenize("<a href=\"unterminated>text</a>", 30);
    ck("Q.unterminated_quote_all_dropped", t && t->count == 0);
    htt_free(t);
}

static void case_comments_scripts(void) {
    const char* cf = "A<!-- secret <b>x</b> -->B";
    HttTokens* t = htt_tokenize(cf, strlen(cf));
    ck("R.comment_full", t && t->count == 2 && tok_is_text(T(t, 0), "A") &&
       tok_is_text(T(t, 1), "B"));
    htt_free(t);

    t = htt_tokenize("A<!-- never closed", 18);
    ck("S.comment_unclosed_tail_after_gt", t && t->count == 1 &&
       tok_is_text(T(t, 0), "A"));
    htt_free(t);

    const char* ss = "<script type=\"t\">var a=\"</p>\"; if(a<b){}</script>tail";
    t = htt_tokenize(ss, strlen(ss));
    ck("T.script_skip", t && t->count == 1 && tok_is_text(T(t, 0), "tail"));
    htt_free(t);

    t = htt_tokenize("<SCRIPT>x<1</ScRiPt>ok", 22);
    ck("U.script_case_insensitive", t && t->count == 1 && tok_is_text(T(t, 0), "ok"));
    htt_free(t);

    t = htt_tokenize("<p>a<script>never", 17);
    ck("V.script_unclosed_swallows_rest", t && t->count == 2 &&
       tok_is_tag(T(t, 0), "p", 0, 0) && tok_is_text(T(t, 1), "a"));
    htt_free(t);

    t = htt_tokenize("<style>p{color:red}</style>Z", 28);
    ck("W.style_skip", t && t->count == 1 && tok_is_text(T(t, 0), "Z"));
    htt_free(t);

    t = htt_tokenize("<style>body{", 12);
    ck("X.style_unclosed_swallows", t && t->count == 0);
    htt_free(t);
}

static void case_title(void) {
    const char* in = "<html><head><title>  Hi   there &amp;  you </title></head></html>";
    HttTokens* t = htt_tokenize(in, strlen(in));
    cks("Y.title_decoded_collapsed", t ? t->pageTitle : NULL, "Hi there & you");
    ck("Y.title_tags_not_emitted", t && t->count == 4 &&
       tok_is_tag(T(t, 0), "html", 0, 0) && tok_is_tag(T(t, 1), "head", 0, 0) &&
       tok_is_tag(T(t, 2), "head", 1, 1) && tok_is_tag(T(t, 3), "html", 1, 1));
    htt_free(t);

    t = htt_tokenize("<title>nope", 11);
    ck("Z.title_unclosed_default", t && t->count == 1 &&
       strcmp(t->pageTitle, "Web Page") == 0 && tok_is_text(T(t, 0), "nope"));
    htt_free(t);

    const char* hijack = "<titles fake=1>x<title>Real</title>";
    t = htt_tokenize(hijack, strlen(hijack));
    ck("AA.prefix_hijack_title", t && t->count == 0 &&
       strcmp(t->pageTitle, "x<title>Real") == 0);
    htt_free(t);
}

static void case_truncation(void) {
    size_t n = 262200 + 8;
    char* big = pluto_malloc(n);
    ck("AB.alloc_big", big != NULL);
    if (!big) return;
    memset(big, 'a', 262200);
    memcpy(big + 262200, "X><p>END", 8);
    HttTokens* t = htt_tokenize(big, n);
    pluto_free(big);
    // Oracle probe: the '>' lands inside the truncation window, so the buffer
    // is cut right after it; no '<' survives, yielding ONE text token.
    ck("AC.trunc_cut_at_gt_single", t && t->count == 1);
    if (t && t->count == 1) {
        ck("AC.text_len_262202", T(t, 0)->textLen == 262202 &&
           T(t, 0)->text[262199] == 'a' && T(t, 0)->text[262200] == 'X' &&
           T(t, 0)->text[262201] == '>');
    }
    htt_free(t);

    size_t m = 300000;
    char* flat = pluto_malloc(m);
    ck("AD.alloc_flat", flat != NULL);
    if (!flat) return;
    memset(flat, 'q', m);
    t = htt_tokenize(flat, m);
    pluto_free(flat);
    ck("AE.no_gt_exact_max", t && t->count == 1 && T(t, 0)->textLen == 262144);
    htt_free(t);

    m = 262144;
    flat = pluto_malloc(m);
    if (!flat) return;
    memset(flat, 'z', m);
    t = htt_tokenize(flat, m);
    pluto_free(flat);
    ck("AF.exact_max_untouched", t && t->count == 1 && T(t, 0)->textLen == 262144);
    htt_free(t);
}

static void case_progress(void) {
    const char* in = "<a>1</a><b>2</b>";
    HttTokens* t = htt_tokenize(in, 16);
    // s_progress is a global monotonic max shared with earlier phase
    // selftests; this run only guarantees it was raised to >= our last
    // report (0.5 * 15/16).
    double p = tasks_get_progress();
    ck("AG.progress_reported", t != NULL && p >= 0.45 && p <= 1.0);
    htt_free(t);
}

int selftest_tokenizer_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    PLUTO_LOG("[P08] tokenizer selftests start");
    case_basic();
    case_tags();
    case_attrs();
    case_comments_scripts();
    case_title();
    case_truncation();
    case_progress();
    PLUTO_LOG("[P08] tokenizer selftests done: %d passed, %d failed", s_pass, s_fail);
    *passed = s_pass;
    *failed = s_fail;
    return s_fail;
}
