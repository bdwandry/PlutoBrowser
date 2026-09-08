#include "html/tokenizer.h"
#include "html/dom.h"
#include <stdio.h>
#include <string.h>

int tasks_yield_check(void) { return 0; }
void tasks_report_progress(double p) { (void)p; }
void logger_log(const char* fmt, ...) { (void)fmt; }
void logger_error_loc(const char* f, int l, const char* fmt, ...) { (void)f; (void)l; (void)fmt; }

static void dump_node(DomNode* n, int depth) {
    if (n == NULL) return;
    for (int i = 0; i < depth; i++) printf("  ");
    if (n->kind == DOM_TEXT)
        printf("TEXT '%s'\n", n->text ? n->text : "");
    else
        printf("<%s>\n", n->tag ? n->tag : "?");
    for (size_t i = 0; i < n->nChildren; i++) dump_node(n->children[i], depth + 1);
}

int main(int argc, char** argv) {
    const char* html = argc > 1 ? argv[1] : "";
    HttTokens* t = htt_tokenize(html, strlen(html));
    printf("tokens=%zu\n", t ? t->count : 0);
    for (size_t i = 0; i < t->count; i++) {
        HttToken* tok = &t->items[i];
        if (tok->type == HTT_TEXT) printf("  TEXT '%s'\n", tok->text);
        else printf("  TAG <%s%s%s%s>\n", tok->name, tok->isClosing ? " /" : "",
                    tok->isSelfClosing ? " selfclose" : "", tok->attrs ? "" : " noattrs");
    }
    DomNode* root = dom_build(t, NULL);
    printf("DOM:\n");
    dump_node(root, 0);
    dom_free(root);
    htt_free(t);
    return 0;
}