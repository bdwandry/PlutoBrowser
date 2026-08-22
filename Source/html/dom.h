#ifndef PLUTO_HTML_DOM_H
#define PLUTO_HTML_DOM_H

#include <stddef.h>
#include "util/strmap.h"
#include "html/tokenizer.h"

struct PlaydateAPI;

enum {
    DOM_ELEMENT = 0,
    DOM_TEXT    = 1
};

typedef struct DomNode {
    int kind;
    char* tag;              /* ELEMENT: owned, "#root" for root */
    StrMap* attrs;          /* ELEMENT: owned (transferred from token) */
    struct DomNode** children;
    size_t nChildren;
    size_t capChildren;
    char* text;             /* TEXT: owned (transferred from token) */
    size_t textLen;
} DomNode;

typedef struct DomDiag {
    unsigned textNodes;
    unsigned elemNodes;
    unsigned tokensProcessed;
    unsigned skippedDepth;
    int maxNodesHit;
} DomDiag;

void dom_init(struct PlaydateAPI* pd);

/* Builds the tree from the token stream. Takes ownership of each token's
 * attrs map / text buffer (the corresponding HttToken fields are nulled,
 * so a later htt_free() on the stream stays safe). Returns the #root node
 * or NULL on OOM. diagOut may be NULL. */
DomNode* dom_build(HttTokens* tokens, DomDiag* diagOut);

void dom_free(DomNode* node);

#endif
