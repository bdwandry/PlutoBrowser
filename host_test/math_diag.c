#include <stdio.h>
#include <string.h>
#include "core/constants.h"
#include "html/document.h"

#define BASE "http://ex.com/"

static void dump_math(const char* html) {
    DocDocument* d = doc_parse(html, BASE, PLUTO_MODE_RAW_HTML);
    printf("HTML: %s\n", html);
    if (d == NULL) { printf("  NULL\n"); return; }
    for (size_t i = 0; i < d->nBlocks; i++) {
        DocBlock* b = &d->blocks[i];
        if (b->type == DB_MATH)
            printf("  MATH[%zu]: '%s'\n", i, b->codeText ? b->codeText : "(null)");
        else
            printf("  OTHER[%zu] type=%d\n", i, (int)b->type);
    }
    doc_free(d);
    printf("\n");
}

int main(void) {
    dump_math("<math><mfrac><mi>a</mi><mn>b</mn></mfrac></math>"
              "<math><msup><mi>x</mi><mn>2</mn></msup></math>"
              "<math><msub><mi>y</mi><mn>1</mn></msub></math>"
              "<math><msubsup><mi>z</mi><mn>1</mn><mn>2</mn></msubsup></math>"
              "<math><msqrt><mi>w</mi></msqrt></math>"
              "<math><mroot><mi>8</mi><mn>3</mn></mroot></math>");
    dump_math("<math><mfenced open=\"[\" close=\"]\" separators=\";\">"
              "<mi>a</mi><mi>b</mi></mfenced></math>");
    dump_math("<math><mi> a </mi> <mo>+</mo>\n<mi>b</mi></math>");
    return 0;
}