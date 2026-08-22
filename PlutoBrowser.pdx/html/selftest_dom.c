#include "html/selftest_dom.h"
#include "html/dom.h"
#include "core/logger.h"
#include "core/tasks.h"
#include "util/mem.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int s_pass, s_fail;

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P09] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P09] FAIL %s", name); }
}

typedef struct {
    HttTokens* tk;
    DomNode* root;
    DomDiag dg;
} Pipe;

static Pipe run(const char* html) {
    Pipe p;
    memset(&p, 0, sizeof(p));
    p.tk = htt_tokenize(html, strlen(html));
    p.root = p.tk ? dom_build(p.tk, &p.dg) : NULL;
    return p;
}

static void pipe_free(Pipe* p) {
    if (p->root != NULL) dom_free(p->root);
    if (p->tk != NULL) htt_free(p->tk);
}

static int elem(DomNode* n, const char* tag) {
    return n != NULL && n->kind == DOM_ELEMENT && n->tag != NULL &&
           strcmp(n->tag, tag) == 0;
}

static int txt(DomNode* n, const char* expect) {
    return n != NULL && n->kind == DOM_TEXT && n->text != NULL &&
           n->textLen == strlen(expect) &&
           memcmp(n->text, expect, n->textLen) == 0;
}

#define K(n, i) ((n)->children[i])

static const char* attr_str(DomNode* n, const char* key) {
    if (n == NULL || n->attrs == NULL) return NULL;
    void* v = sm_get(n->attrs, key);
    return (v == NULL || v == HT_ATTR_TRUE) ? NULL : (const char*)v;
}

static int attr_bool(DomNode* n, const char* key) {
    return n != NULL && n->attrs != NULL && sm_get(n->attrs, key) == HT_ATTR_TRUE;
}

static void case_basic(void) {
    Pipe p = run("hello world");
    ck("A.root", p.root != NULL && p.root->kind == DOM_ELEMENT &&
       strcmp(p.root->tag, "#root") == 0);
    ck("A.diag", p.dg.textNodes == 1 && p.dg.elemNodes == 0 &&
       p.dg.tokensProcessed == 1 && !p.dg.maxNodesHit && p.dg.skippedDepth == 0);
    ck("A.child_text", p.root && p.root->nChildren == 1 &&
       txt(K(p.root, 0), "hello world"));
    pipe_free(&p);

    p = run("");
    ck("B.empty", p.root && p.root->nChildren == 0 && p.dg.tokensProcessed == 0);
    pipe_free(&p);

    p = run("<div><p>a</p><p>b</p></div>");
    ck("C.nested_counts", p.dg.elemNodes == 3 && p.dg.textNodes == 2 &&
       p.dg.tokensProcessed == 8);
    ck("C.structure", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "div") && K(p.root, 0)->nChildren == 2 &&
       elem(K(K(p.root, 0), 0), "p") &&
       txt(K(K(K(p.root, 0), 1), 0), "b"));
    pipe_free(&p);

    p = run("<div><b>x<i>y</div>z");
    ck("D.format_survives", p.root && p.root->nChildren == 2 &&
       txt(K(p.root, 1), "z"));
    if (p.root && p.root->nChildren == 2) {
        DomNode* b = K(p.root, 0);
        ck("D.b_nests_i", b->nChildren == 1 && elem(K(b, 0), "b") &&
           K(b, 0)->nChildren == 2 && txt(K(K(b, 0), 0), "x") &&
           elem(K(K(b, 0), 1), "i"));
    }
    pipe_free(&p);

    p = run("<p>one<p>two<div>d</div>");
    ck("E.p_implied_by_div", p.root && p.root->nChildren == 3 &&
       elem(K(p.root, 0), "p") && txt(K(K(p.root, 0), 0), "one") &&
       elem(K(p.root, 1), "p") && txt(K(K(p.root, 1), 0), "two") &&
       elem(K(p.root, 2), "div"));
    pipe_free(&p);
}

static void case_lists_tables(void) {
    Pipe p = run("<li>solo<li>two");
    ck("F.stray_li_root_siblings", p.root && p.root->nChildren == 2 &&
       elem(K(p.root, 0), "li") && elem(K(p.root, 1), "li"));
    pipe_free(&p);

    p = run("<ul><li>A<ul><li>A1</li><li>A2</li></ul></li><li>B</li></ul>");
    ck("G.list_counts", p.dg.elemNodes == 6 && p.dg.textNodes == 4 &&
       p.dg.tokensProcessed == 16);
    ck("G.list_shape", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "ul") && K(p.root, 0)->nChildren == 2);
    if (p.root && p.root->nChildren == 1) {
        DomNode* ul = K(p.root, 0);
        DomNode* liA = K(ul, 0);
        DomNode* liB = K(ul, 1);
        ck("G.inner_list", liA->nChildren == 2 && txt(K(liA, 0), "A") &&
           elem(K(liA, 1), "ul") && K(liA, 1)->nChildren == 2 &&
           K(K(liA, 1), 0)->nChildren == 1 &&
           K(K(liA, 1), 1)->nChildren == 1 &&
           txt(K(K(K(liA, 1), 0), 0), "A1") &&
           txt(K(K(K(liA, 1), 1), 0), "A2") && txt(K(liB, 0), "B"));
    }
    pipe_free(&p);

    p = run("<dl><dt>t1<dd>d1<dt>t2<dd>d2</dl>");
    ck("H.dtdd", p.root && p.root->nChildren == 1 && elem(K(p.root, 0), "dl") &&
       K(p.root, 0)->nChildren == 4 &&
       elem(K(K(p.root, 0), 0), "dt") && txt(K(K(K(p.root, 0), 1), 0), "d1") &&
       elem(K(K(p.root, 0), 3), "dd") && txt(K(K(K(p.root, 0), 3), 0), "d2"));
    pipe_free(&p);

    p = run("<table><tr><td>a</td><td>b</td></tr><tr><th>c</th></tr></table>");
    ck("I.table_counts", p.dg.elemNodes == 6 && p.dg.tokensProcessed == 15);
    ck("I.table_shape", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "table") && K(p.root, 0)->nChildren == 2);
    if (p.root && p.root->nChildren == 1) {
        DomNode* tbl = K(p.root, 0);
        ck("I.rows_cells", elem(K(tbl, 0), "tr") && K(tbl, 0)->nChildren == 2 &&
           elem(K(K(tbl, 0), 0), "td") && elem(K(K(tbl, 0), 1), "td") &&
           elem(K(tbl, 1), "tr") && K(tbl, 1)->nChildren == 1 &&
           elem(K(K(tbl, 1), 0), "th") && txt(K(K(K(tbl, 1), 0), 0), "c"));
    }
    pipe_free(&p);

    p = run("x<tr><td>lost</td></tr>y");
    ck("J.stray_tr_dropped", p.root && p.root->nChildren == 3 &&
       p.dg.elemNodes == 0 && txt(K(p.root, 0), "x") &&
       txt(K(p.root, 1), "lost") && txt(K(p.root, 2), "y"));
    pipe_free(&p);

    p = run("x<td>nope</td>y");
    ck("K.stray_td_dropped", p.root && p.root->nChildren == 3 &&
       p.dg.elemNodes == 0 && txt(K(p.root, 1), "nope"));
    pipe_free(&p);

    p = run("x<option>oy<select><option>a<option>b</select>z");
    ck("L.option_outside_dropped", p.root && p.root->nChildren == 4 &&
       p.dg.elemNodes == 3 && txt(K(p.root, 1), "oy"));
    if (p.root && p.root->nChildren == 5) {
        DomNode* sel = K(p.root, 2);
        ck("L.select_options", elem(sel, "select") && sel->nChildren == 2 &&
           elem(K(sel, 0), "option") && txt(K(K(sel, 0), 0), "a") &&
           elem(K(sel, 1), "option") && txt(K(K(sel, 1), 0), "b"));
    }
    pipe_free(&p);
}

static void case_inline_void(void) {
    Pipe p = run("<a href=\"1\">one<a href=\"2\">two</a>three");
    ck("M.anchor_reopen", p.root && p.root->nChildren == 3 &&
       p.dg.elemNodes == 2 && elem(K(p.root, 0), "a") &&
       strcmp(attr_str(K(p.root, 0), "href"), "1") == 0);
    if (p.root && p.root->nChildren == 3) {
        ck("M.second_a_sibling", elem(K(p.root, 1), "a") &&
           txt(K(K(p.root, 1), 0), "two") && txt(K(p.root, 2), "three"));
    }
    pipe_free(&p);

    p = run("a<br>b<img src=\"x\">c<hr></br>d");
    ck("N.voids_flat", p.root && p.root->nChildren == 7 &&
       p.dg.elemNodes == 3);
    if (p.root && p.root->nChildren == 7) {
        ck("N.sequence", txt(K(p.root, 0), "a") && elem(K(p.root, 1), "br") &&
           txt(K(p.root, 2), "b") && elem(K(p.root, 3), "img") &&
           strcmp(attr_str(K(p.root, 3), "src"), "x") == 0 &&
           txt(K(p.root, 4), "c") && elem(K(p.root, 5), "hr") &&
           txt(K(p.root, 6), "d"));
    }
    ck("N.close_br_ignored", p.dg.tokensProcessed == 8);
    pipe_free(&p);

    p = run("<div/>after<br/>end");
    ck("O.selfclose_not_pushed", p.root && p.root->nChildren == 4 &&
       p.dg.elemNodes == 2 && elem(K(p.root, 0), "div") &&
       txt(K(p.root, 1), "after") && elem(K(p.root, 2), "br") &&
       txt(K(p.root, 3), "end"));
    pipe_free(&p);
}

static void case_skip_subtree(void) {
    const char* hs =
        "<html><head><meta charset=\"utf-8\"><title>T</title></head>"
        "<body><p>vis</p></body></html>";
    Pipe p = run(hs);
    ck("P.head_skipped", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "html") && K(p.root, 0)->nChildren == 1 &&
       elem(K(K(p.root, 0), 0), "body"));
    if (p.root && p.root->nChildren == 1) {
        DomNode* body = K(K(p.root, 0), 0);
        ck("P.body_p_vis", body->nChildren == 1 && elem(K(body, 0), "p") &&
           txt(K(K(body, 0), 0), "vis"));
    }
    ck("P.diag_skipped1_proc10", p.dg.skippedDepth == 1 &&
       p.dg.tokensProcessed == 10);
    pipe_free(&p);

    p = run("a<template><p>hidden</p></template>b");
    ck("Q.template_hidden", p.root && p.root->nChildren == 2 &&
       p.dg.elemNodes == 0 && txt(K(p.root, 0), "a") &&
       txt(K(p.root, 1), "b"));
    ck("Q.skipped3", p.dg.skippedDepth == 3 && p.dg.tokensProcessed == 7);
    pipe_free(&p);
}

static void case_quirks(void) {
    Pipe p = run("<div><p>text<span>more<div>deep");
    ck("R.close_through_counts", p.dg.elemNodes == 4 &&
       p.dg.tokensProcessed == 7);
    ck("R.popped_through_span", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "div") && K(p.root, 0)->nChildren == 2);
    if (p.root && p.root->nChildren == 1) {
        DomNode* dv = K(p.root, 0);
        ck("R.deep_sibling_of_p", elem(K(dv, 0), "p") &&
           K(dv, 0)->nChildren == 2 && txt(K(K(dv, 0), 0), "text") &&
           elem(K(K(dv, 0), 1), "span") && txt(K(K(K(dv, 0), 1), 0), "more") &&
           elem(K(dv, 1), "div") && txt(K(K(dv, 1), 0), "deep"));
    }
    pipe_free(&p);

    p = run("<div>a</span>b</div>c");
    ck("S.unknown_close_noop", p.root && p.root->nChildren == 2 &&
       p.dg.elemNodes == 1);
    if (p.root && p.root->nChildren == 2) {
        DomNode* dv = K(p.root, 0);
        ck("S.b_inside_div", dv->nChildren == 2 && txt(K(dv, 0), "a") &&
           txt(K(dv, 1), "b") && txt(K(p.root, 1), "c"));
    }
    pipe_free(&p);

    p = run("<a class=\"x y\" href=\"/p\" disabled>t</a>");
    ck("T.attrs_kept", p.root && p.root->nChildren == 1 &&
       elem(K(p.root, 0), "a"));
    if (p.root && p.root->nChildren == 1) {
        DomNode* a = K(p.root, 0);
        ck("T.attr_values", strcmp(attr_str(a, "class"), "x y") == 0 &&
           strcmp(attr_str(a, "href"), "/p") == 0 && attr_bool(a, "disabled") &&
           sm_count(a->attrs) == 3);
        ck("T.text", txt(K(a, 0), "t"));
    }
    pipe_free(&p);

    char html[13 * 5 + 5 + 13 * 6 + 1];
    html[0] = '\0';
    for (int i = 0; i < 12; i++) strcat(html, "<div>");
    strcat(html, "core");
    for (int i = 0; i < 12; i++) strcat(html, "</div>");
    p = run(html);
    int depth = 0;
    DomNode* cur = p.root;
    while (cur != NULL && cur->nChildren == 1 &&
           K(cur, 0)->kind == DOM_ELEMENT) {
        cur = K(cur, 0);
        depth++;
    }
    ck("U.chain_depth_12", p.dg.elemNodes == 12 && depth == 12 &&
       cur != NULL && cur->nChildren == 1 && txt(K(cur, 0), "core"));
    pipe_free(&p);

    p = run("<blockquote><p>q1<p>q2</blockquote>out");
    ck("V.blockquote_ps", p.root && p.root->nChildren == 2 &&
       elem(K(p.root, 0), "blockquote"));
    if (p.root && p.root->nChildren == 2) {
        DomNode* bq = K(p.root, 0);
        ck("V.two_siblings", bq->nChildren == 2 && elem(K(bq, 0), "p") &&
           txt(K(K(bq, 0), 0), "q1") && elem(K(bq, 1), "p") &&
           txt(K(K(bq, 1), 0), "q2") && txt(K(p.root, 1), "out"));
    }
    pipe_free(&p);
}

static void case_valve_and_caps(void) {
    size_t n = strlen("<head>") + 600 * strlen("<span></span>") + 1;
    char* big = pluto_malloc(n);
    ck("W.alloc", big != NULL);
    if (big == NULL) return;
    strcpy(big, "<head>");
    for (int i = 0; i < 600; i++) strcat(big, "<span></span>");
    Pipe p = run(big);
    pluto_free(big);
    ck("W.valve_diag", p.dg.skippedDepth == 500 &&
       p.dg.tokensProcessed == 1201 && !p.dg.maxNodesHit);
    ck("W.valve_kids", p.root && p.root->nChildren == 350 &&
       elem(K(p.root, 0), "span") && elem(K(p.root, 349), "span"));
    pipe_free(&p);

    size_t m = 5 * 6100 + 1;
    char* many = pluto_malloc(m);
    ck("X.alloc", many != NULL);
    if (many == NULL) return;
    many[0] = '\0';
    for (int i = 0; i < 6100; i++) strcat(many, "<img>");
    p = run(many);
    pluto_free(many);
    ck("X.capped_at_6000", p.root && p.root->nChildren == 6000 &&
       p.dg.elemNodes == 6000 && p.dg.tokensProcessed == 6001);
    // Faithful source quirk: the loop breaks before append() can ever
    // refuse, so maxNodesHit stays false even at the cap.
    ck("X.maxhit_dead_flag_false", !p.dg.maxNodesHit);
    pipe_free(&p);
}

static void case_progress(void) {
    Pipe p = run("<div><p>x</p></div>");
    double prog = tasks_get_progress();
    ck("Y.progress_in_dom_range", p.root != NULL && prog >= 0.8 && prog <= 1.0);
    pipe_free(&p);
}

int selftest_dom_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    PLUTO_LOG("[P09] dom selftests start");
    case_basic();
    case_lists_tables();
    case_inline_void();
    case_skip_subtree();
    case_quirks();
    case_valve_and_caps();
    case_progress();
    PLUTO_LOG("[P09] dom selftests done: %d passed, %d failed", s_pass, s_fail);
    *passed = s_pass;
    *failed = s_fail;
    return s_fail;
}
