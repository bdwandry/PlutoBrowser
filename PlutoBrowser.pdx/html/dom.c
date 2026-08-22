#include "html/dom.h"
#include "core/tasks.h"
#include "util/mem.h"
#include <ctype.h>
#include <string.h>

#define DOM_MAX_NODES 6000
#define DOM_SKIP_VALVE 500

static int dom_is_void(const char* t) {
    static const char* const v[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; v[i]; i++)
        if (strcmp(t, v[i]) == 0) return 1;
    return 0;
}

static int dom_is_skip(const char* t) {
    return strcmp(t, "template") == 0 || strcmp(t, "head") == 0 ||
           strcmp(t, "selectedcontent") == 0;
}

static int dom_is_block(const char* t) {
    static const char* const b[] = {
        "address", "article", "aside", "blockquote", "center", "dd",
        "details", "dialog", "dir", "div", "dl", "dt", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
        "h5", "h6", "header", "hgroup", "hr", "li", "main", "menu", "nav",
        "ol", "p", "pre", "section", "table", "ul", NULL
    };
    for (int i = 0; b[i]; i++)
        if (strcmp(t, b[i]) == 0) return 1;
    return 0;
}

typedef struct {
    DomNode** items;
    size_t n;
    size_t cap;
} DomStack;

static int ds_push(DomStack* s, DomNode* n) {
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        DomNode** ni = pluto_realloc(s->items, ncap * sizeof(DomNode*));
        if (ni == NULL) return 0;
        s->items = ni;
        s->cap = ncap;
    }
    s->items[s->n++] = n;
    return 1;
}

/* The Lua builder scans stack indices #stack..2 (index 1 = #root is never
 * touched). C equivalent: scan top..1 (index 0 = root untouched). */

static void ds_pop_to(DomStack* s, size_t idx) {
    s->n = idx;
}

static void ds_truncate(DomStack* s) {
    if (s->n > 1) s->n = 1;
}

static int dom_pop_to_tag(DomStack* s, const char* tag) {
    for (size_t i = s->n; i-- > 1; ) {
        if (strcmp(s->items[i]->tag, tag) == 0) {
            ds_pop_to(s, i);
            return 1;
        }
    }
    return 0;
}

static void dom_prepare_list_item(DomStack* s) {
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "li") == 0) { ds_pop_to(s, i); break; }
        if (strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0 ||
            strcmp(t, "menu") == 0 || strcmp(t, "dir") == 0) break;
    }
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0 ||
            strcmp(t, "menu") == 0 || strcmp(t, "dir") == 0) return;
    }
    ds_truncate(s);
}

static void dom_prepare_dt_dd(DomStack* s) {
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "dt") == 0 || strcmp(t, "dd") == 0) { ds_pop_to(s, i); break; }
        if (strcmp(t, "dl") == 0) break;
    }
    for (size_t i = s->n; i-- > 1; ) {
        if (strcmp(s->items[i]->tag, "dl") == 0) return;
    }
    ds_truncate(s);
}

static int dom_is_table_ctx(const char* t) {
    return strcmp(t, "table") == 0 || strcmp(t, "tbody") == 0 ||
           strcmp(t, "thead") == 0 || strcmp(t, "tfoot") == 0;
}

static int dom_prepare_row(DomStack* s) {
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "tr") == 0 || strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
            ds_pop_to(s, i);
            break;
        }
        if (dom_is_table_ctx(t)) break;
    }
    for (size_t i = s->n; i-- > 1; ) {
        if (dom_is_table_ctx(s->items[i]->tag)) return 1;
    }
    return 0;
}

static int dom_prepare_cell(DomStack* s) {
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0) { ds_pop_to(s, i); break; }
        if (strcmp(t, "tr") == 0) break;
    }
    for (size_t i = s->n; i-- > 1; ) {
        if (strcmp(s->items[i]->tag, "tr") == 0) return 1;
    }
    return 0;
}

static int dom_prepare_option(DomStack* s) {
    for (size_t i = s->n; i-- > 1; ) {
        const char* t = s->items[i]->tag;
        if (strcmp(t, "option") == 0) { ds_pop_to(s, i); break; }
        if (strcmp(t, "select") == 0) break;
    }
    for (size_t i = s->n; i-- > 1; ) {
        if (strcmp(s->items[i]->tag, "select") == 0) return 1;
    }
    return 0;
}

typedef struct {
    DomNode* root;
    DomStack stack;
    unsigned count;
    DomDiag diag;
} DomBuilder;

static DomNode* dom_new_elem(const char* tag, StrMap* attrs) {
    DomNode* n = pluto_malloc(sizeof(DomNode));
    if (n == NULL) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind = DOM_ELEMENT;
    n->tag = pluto_strdup(tag);
    if (n->tag == NULL) {
        pluto_free(n);
        return NULL;
    }
    n->attrs = attrs;
    return n;
}

static DomNode* dom_new_text(char* text, size_t len) {
    DomNode* n = pluto_malloc(sizeof(DomNode));
    if (n == NULL) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind = DOM_TEXT;
    n->text = text;
    n->textLen = len;
    return n;
}

static int dn_add_child(DomNode* parent, DomNode* child) {
    if (parent->nChildren == parent->capChildren) {
        size_t ncap = parent->capChildren ? parent->capChildren * 2 : 4;
        DomNode** nc = pluto_realloc(parent->children,
                                     ncap * sizeof(DomNode*));
        if (nc == NULL) return 0;
        parent->children = nc;
        parent->capChildren = ncap;
    }
    parent->children[parent->nChildren++] = child;
    return 1;
}

static int dom_append(DomBuilder* b, DomNode* node) {
    if (b->count >= DOM_MAX_NODES) {
        b->diag.maxNodesHit = 1;
        return 0;
    }
    if (!dn_add_child(b->stack.items[b->stack.n - 1], node)) return 0;
    b->count++;
    if (node->kind == DOM_TEXT) b->diag.textNodes++;
    else if (node->kind == DOM_ELEMENT) b->diag.elemNodes++;
    return 1;
}

void dom_init(struct PlaydateAPI* pd) {
    (void)pd;
}

DomNode* dom_build(HttTokens* tokens, DomDiag* diagOut) {
    DomBuilder b;
    memset(&b, 0, sizeof(b));
    int skipDepth = 0;
    char* skipTag = NULL;

    b.root = dom_new_elem("#root", sm_create(2));
    if (b.root == NULL || b.root->attrs == NULL) {
        if (b.root != NULL) dom_free(b.root);
        return NULL;
    }
    if (!ds_push(&b.stack, b.root)) {
        dom_free(b.root);
        return NULL;
    }

    if (tokens != NULL) {
        size_t nTok = tokens->count;
        for (size_t i = 0; i < nTok; i++) {
            HttToken* tok = &tokens->items[i];
            if (tasks_yield_check()) break;
            tasks_report_progress(0.5 + 0.3 *
                ((double)(i + 1) / (double)(nTok ? nTok : 1)));
            b.diag.tokensProcessed++;
            if (b.count >= DOM_MAX_NODES) break;

            if (skipDepth > 0) {
                if (tok->type == HTT_TAG && tok->isClosing &&
                    tok->name != NULL && strcmp(tok->name, skipTag) == 0) {
                    skipDepth = 0;
                    pluto_free(skipTag);
                    skipTag = NULL;
                } else {
                    b.diag.skippedDepth++;
                    if (b.diag.skippedDepth >= DOM_SKIP_VALVE) {
                        skipDepth = 0;
                        pluto_free(skipTag);
                        skipTag = NULL;
                    }
                }
                continue;
            }

            if (tok->type == HTT_TEXT) {
                char* txt = tok->text;
                size_t len = tok->textLen;
                if (txt == NULL) {
                    txt = pluto_strdup("");
                    len = 0;
                    if (txt == NULL) goto fail;
                } else {
                    tok->text = NULL;
                }
                DomNode* n = dom_new_text(txt, len);
                if (n == NULL) {
                    pluto_free(txt);
                    goto fail;
                }
                if (!dom_append(&b, n)) pluto_free(n);
            } else {
                const char* tag = tok->name ? tok->name : "";
                if (tok->isClosing) {
                    if (!dom_is_void(tag) && !dom_is_skip(tag))
                        dom_pop_to_tag(&b.stack, tag);
                } else {
                    StrMap* attrs = tok->attrs;
                    tok->attrs = NULL;
                    if (dom_is_skip(tag)) {
                        sm_destroy(attrs);
                        skipTag = pluto_strdup(tag);
                        if (skipTag == NULL) goto fail;
                        skipDepth = 1;
                    } else if (dom_is_void(tag)) {
                        DomNode* el = dom_new_elem(tag, attrs);
                        if (el == NULL) {
                            sm_destroy(attrs);
                            goto fail;
                        }
                        if (!dom_append(&b, el)) pluto_free(el);
                    } else {
                        int doPush = 1;
                        if (dom_is_block(tag)) dom_pop_to_tag(&b.stack, "p");
                        if (strcmp(tag, "li") == 0) {
                            dom_prepare_list_item(&b.stack);
                        } else if (strcmp(tag, "dt") == 0 ||
                                   strcmp(tag, "dd") == 0) {
                            dom_prepare_dt_dd(&b.stack);
                        } else if (strcmp(tag, "tr") == 0 ||
                                   strcmp(tag, "thead") == 0 ||
                                   strcmp(tag, "tbody") == 0 ||
                                   strcmp(tag, "tfoot") == 0) {
                            doPush = dom_prepare_row(&b.stack);
                        } else if (strcmp(tag, "td") == 0 ||
                                   strcmp(tag, "th") == 0) {
                            doPush = dom_prepare_cell(&b.stack);
                        } else if (strcmp(tag, "option") == 0) {
                            doPush = dom_prepare_option(&b.stack);
                        } else if (strcmp(tag, "a") == 0) {
                            dom_pop_to_tag(&b.stack, "a");
                        }

                        if (doPush) {
                            DomNode* el = dom_new_elem(tag, attrs);
                            if (el == NULL) {
                                sm_destroy(attrs);
                                goto fail;
                            }
                            if (!dom_append(&b, el)) {
                                pluto_free(el);
                            } else if (!tok->isSelfClosing) {
                                if (!ds_push(&b.stack, el)) goto fail;
                            }
                        } else {
                            sm_destroy(attrs);
                        }
                    }
                }
            }
        }
    }

    pluto_free(skipTag);
    pluto_free(b.stack.items);
    if (diagOut != NULL) *diagOut = b.diag;
    return b.root;

fail:
    pluto_free(skipTag);
    pluto_free(b.stack.items);
    dom_free(b.root);
    return NULL;
}

void dom_free(DomNode* node) {
    if (node == NULL) return;
    for (size_t i = 0; i < node->nChildren; i++)
        dom_free(node->children[i]);
    pluto_free(node->children);
    pluto_free(node->tag);
    if (node->attrs != NULL) sm_destroy(node->attrs);
    pluto_free(node->text);
    pluto_free(node);
}
