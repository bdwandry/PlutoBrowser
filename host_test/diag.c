#include "html/document.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "core/constants.h"
#include <stdio.h>
#include <string.h>

int tasks_yield_check(void) { return 0; }
void tasks_report_progress(double p) { (void)p; }
void logger_log(const char* fmt, ...) { (void)fmt; }
void logger_error_loc(const char* f, int l, const char* fmt, ...) { (void)f; (void)l; (void)fmt; }

#define BASE "http://ex.com/dir/page.html"

static void dump_links(const DocDocument* d) {
    printf("  nLinks=%zu links=%s\n", d->nLinks, d->links ? "set" : "null");
    for (size_t i = 0; i < d->nLinks; i++)
        printf("   [%zu] href='%s' text='%s' target='%s'\n", i,
               d->links[i].href ? d->links[i].href : "(null)",
               d->links[i].text ? d->links[i].text : "(null)",
               d->links[i].target ? d->links[i].target : "(null)");
}

static void dump_blocks(const DocDocument* d) {
    printf("  nBlocks=%zu\n", d ? d->nBlocks : 0);
    if (d) for (size_t i = 0; i < d->nBlocks; i++)
        printf("   block[%zu] type=%d\n", i, d->blocks[i].type);
}

static void dump_code(const DocDocument* d) {
    for (size_t i = 0; i < d->nBlocks; i++) {
        const DocBlock* b = &d->blocks[i];
        if (b->type == DB_CODE_BLOCK) {
            printf("  codeText='%s' nLines=%zu\n", b->codeText ? b->codeText : "(null)", b->nLines);
            for (size_t j = 0; j < b->nLines; j++) printf("   line[%zu]='%s'\n", j, b->lines[j]);
        }
    }
}

static void dump_math(const DocDocument* d) {
    for (size_t i = 0; i < d->nBlocks; i++) {
        const DocBlock* b = &d->blocks[i];
        if (b->type == DB_MATH) printf("  math[%zu]='%s'\n", i, b->codeText ? b->codeText : "(null)");
    }
}

int main(void) {
    {   DocDocument* d = doc_parse("<pre>a\n\tb</pre>", BASE, PLUTO_MODE_RAW_HTML);
        printf("PRE a\\n\\tb:\n"); dump_blocks(d); dump_code(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<pre>l1\nl2\n\nl4</pre>", BASE, PLUTO_MODE_RAW_HTML);
        printf("PRE l1..l4:\n"); dump_blocks(d); dump_code(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<a href=\"x.html\" target=_blank>go</a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("LINK resolve_target:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<a href=\"/t\" title=\"TT\"></a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("LINK title_fallback:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<a href=\"/r\"></a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("LINK href_fallback:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<a href=\"/m\">a<b>b</b>c</a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("LINK text_accum:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<a href=\"/1\">x</a><a href=\"/2\">y</a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("LINK anchor_seq:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<figure><img src=\"/i.png\" width=10 height=10><figcaption>CAP</figcaption></figure>", BASE, PLUTO_MODE_RAW_HTML);
        printf("FIGURE caption_attached:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            if (b->type == DB_IMAGE) printf("  img src='%s' caption='%s'\n", b->src ? b->src : "(null)", b->caption ? b->caption : "(null)");
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("a<span inert>sec</span>b", BASE, PLUTO_MODE_RAW_HTML);
        printf("INERT span:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            for (size_t j = 0; j < b->nInlines; j++)
                printf("  inline[%zu] text='%s' inert=%d\n", j, b->inlines[j].text, b->inlines[j].inert);
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("<head><base href=\"http://cdn.ex/root/\"></head><body><a href=\"x.html\">l</a></body>", BASE, PLUTO_MODE_RAW_HTML);
        printf("BASE override:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<base href=\"/not/absolute\"><base href=\"http://good.ex/\"><a href=\"y\">l</a>", BASE, PLUTO_MODE_RAW_HTML);
        printf("BASE first_nonempty_wins:\n"); dump_links(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<textarea rows=4 cols=30 readonly required maxlength=99>ab\ncd</textarea>", BASE, PLUTO_MODE_RAW_HTML);
        printf("TEXTAREA capture:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            printf("  block[%zu] type=%d inputType='%s' name='%s' value='%s' rows=%d cols/width=%d maxlength=%d blocked=%d\n", i, b->type, b->inputType?b->inputType:"(null)", b->inName?b->inName:"(null)", b->inValue?b->inValue:"(null)", b->fieldRows, b->fieldWidth, b->maxlength, b->blockInert);
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("<math><mfrac><mi>a</mi><mn>b</mn></mfrac></math><math><msup><mi>x</mi><mn>2</mn></msup></math><math><msub><mi>y</mi><mn>1</mn></msub></math><math><msubsup><mi>z</mi><mn>1</mn><mn>2</mn></msubsup></math><math><msqrt><mi>w</mi></msqrt></math><math><mroot><mi>8</mi><mn>3</mn></mroot></math>", BASE, PLUTO_MODE_RAW_HTML);
        printf("MATH linearized (n=%zu):\n", d->nBlocks); dump_math(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<math><mfenced open=\"[\" close=\"]\" separators=\";\"><mi>a</mi><mi>b</mi></mfenced></math>", BASE, PLUTO_MODE_RAW_HTML);
        printf("MATH mfenced_seps:\n"); dump_math(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<math><mi> a </mi> <mo>+</mo>\n<mi>b</mi></math>", BASE, PLUTO_MODE_RAW_HTML);
        printf("MATH ws_collapse:\n"); dump_math(d); doc_free(d); }
    {   DocDocument* d = doc_parse("<figure><figcaption>OnlyCap</figcaption></figure>", BASE, PLUTO_MODE_RAW_HTML);
        printf("FIGURE caption_only_para:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            printf("  block[%zu] type=%d align='%s' nInlines=%zu\n", i, b->type, b->align?b->align:"(null)", b->nInlines);
            for (size_t j = 0; j < b->nInlines; j++)
                printf("    inl[%zu] text='%s' italic=%d\n", j, b->inlines[j].text?b->inlines[j].text:"(null)", b->inlines[j].italic);
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("<p>a</p><div hidden>b<div><span inert>c</span></div></div><p>d</p><span inert>e<span>f</span></span>", BASE, PLUTO_MODE_RAW_HTML);
        printf("INERT scope_restored (n=%zu):\n", d->nBlocks);
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            for (size_t j = 0; j < b->nInlines; j++)
                printf("  block[%zu] inl[%zu] text='%s' inert=%d\n", i, j, b->inlines[j].text?b->inlines[j].text:"(null)", b->inlines[j].inert);
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("<form action=\"/s\" method=POST><input name=in1><input name=in2 formaction=\"/b2\" formmethod=get></form><input name=out>", BASE, PLUTO_MODE_RAW_HTML);
        printf("FORM context:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            printf("  block[%zu] name='%s' fA='%s' fM='%s'\n", i, b->inName?b->inName:"(null)", b->formAction?b->formAction:"(null)", b->formMethod?b->formMethod:"(null)");
        }
        doc_free(d); }
    {   DocDocument* d = doc_parse("<textarea name=ta disabled>x</textarea>", BASE, PLUTO_MODE_RAW_HTML);
        printf("TEXTAREA disabled:\n");
        for (size_t i = 0; i < d->nBlocks; i++) {
            const DocBlock* b = &d->blocks[i];
            printf("  block[%zu] name='%s' disabled=%u readonly=%u blockInert=%d\n", i, b->inName?b->inName:"(null)", b->disabledFlag, b->readonlyFlag, (int)b->blockInert);
        }
        doc_free(d); }
    return 0;
}