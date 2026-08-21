#ifndef PLUTO_HTML_TOKENIZER_H
#define PLUTO_HTML_TOKENIZER_H

#include <stddef.h>
#include "util/strmap.h"

struct PlaydateAPI;

enum {
    HTT_TEXT = 0,
    HTT_TAG  = 1
};

#define HT_ATTR_TRUE ((void*)1)

typedef struct HttToken {
    int      type;
    char*    text;
    size_t   textLen;
    char*    name;
    int      isClosing;
    int      isSelfClosing;
    StrMap*  attrs;
} HttToken;

typedef struct HttTokens {
    HttToken* items;
    size_t    count;
    size_t    cap;
    char*     pageTitle;
} HttTokens;

void htt_init(struct PlaydateAPI* pd);

HttTokens* htt_tokenize(const char* html, size_t len);
void       htt_free(HttTokens* t);

#endif
