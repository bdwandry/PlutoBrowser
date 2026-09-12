/*
 * P19e host battery: run readability_distill over the shared cases and print
 * the same record format as the Lua harness (run.lua) for byte diffing.
 * Usage: host_test <dummy> <outfile>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pd_api.h"
#include "html/tokenizer.h"
#include "html/readability.h"
#include "html/document.h"
#include "core/url.h"
#include "core/tasks.h"

/* Shared cases (same file the Lua harness reads). */
typedef struct
{
    const char *name;
    const char *baseUrl;
    const char *html;
} RCase;

static const RCase CASES[] = {
#include "p19e_cases_inc.h"
};

/* Host shims: a fake PlaydateAPI whose system->realloc forwards to malloc.
 * tokenizer.c and friends call pluto_pd()->system->realloc directly. */
static void *host_realloc_shim(void *p, size_t n)
{
    return realloc(p, n);
}
static struct playdate_sys g_hostSys;
static PlaydateAPI g_hostPd;
void *pluto_realloc(void *p, size_t n)
{
    return realloc(p, n);
}
void pluto_free(void *p)
{
    free(p);
}
PlaydateAPI *pluto_pd(void)
{
    if (!g_hostPd.system)
    {
        memset(&g_hostSys, 0, sizeof(g_hostSys));
        g_hostSys.realloc = host_realloc_shim;
        g_hostPd.system = &g_hostSys;
    }
    return &g_hostPd;
}

static void esc_print(FILE *f, const char *s)
{
    for (const char *p = s ? s : ""; *p; p++)
    {
        switch (*p)
        {
        case '\\':
            fputs("\\\\", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            fputc(*p, f);
        }
    }
}

static void ser_inline(FILE *f, const DocInline *i)
{
    fputs(i->type == DOC_INLINE_TEXT ? "text" : (i->type == DOC_INLINE_BR ? "br" : "wbr"), f);
    if (i->text)
    {
        fputs("|text=", f);
        esc_print(f, i->text);
    }
    if (i->flags & DOC_INF_BOLD)
    {
        fputs("|bold=t", f);
    }
    if (i->flags & DOC_INF_ITALIC)
    {
        fputs("|italic=t", f);
    }
    if (i->flags & DOC_INF_UNDERLINE)
    {
        fputs("|underline=t", f);
    }
    if (i->flags & DOC_INF_CODE)
    {
        fputs("|code=t", f);
    }
    if (i->href)
    {
        fputs("|href=", f);
        esc_print(f, i->href);
    }
}

static void ser_block(FILE *f, const DocBlock *b)
{
    static const char *TN[] = {
        "paragraph", "heading", "blockquote", "list_item", "image", "hr",
        "code_block", "table", "hidden_field", "checkbox_field", "input_field",
        "input_submit", "select_field", "box_open", "box_close", "placeholder",
        "meter", "math", "reader_header"};
    fputs("B ", f);
    fputs(TN[b->type], f);
    if (b->type == DOC_BLOCK_HEADING)
    {
        fprintf(f, "|level=%d", b->level);
    }
    if (b->type == DOC_BLOCK_LIST_ITEM)
    {
        if (b->isOrdered)
        {
            fputs("|isOrdered=true", f);
        }
        fprintf(f, "|number=%d", b->number);
    }
    if (b->type == DOC_BLOCK_IMAGE)
    {
        fputs("|src=", f);
        esc_print(f, b->src);
        fputs("|alt=", f);
        esc_print(f, b->alt);
        /* Lua tostring of a number: integral prints as "N". */
        char nb[32];
        if (b->width == (long long)b->width)
        {
            snprintf(nb, sizeof(nb), "%lld", (long long)b->width);
        }
        else
        {
            snprintf(nb, sizeof(nb), "%g", b->width);
        }
        fprintf(f, "|width=%s", nb);
        if (b->height == (long long)b->height)
        {
            snprintf(nb, sizeof(nb), "%lld", (long long)b->height);
        }
        else
        {
            snprintf(nb, sizeof(nb), "%g", b->height);
        }
        fprintf(f, "|height=%s", nb);
        if (b->href)
        {
            fputs("|href=", f);
            esc_print(f, b->href);
        }
    }
    if (b->type == DOC_BLOCK_INPUT_FIELD)
    {
        fputs("|inputType=", f);
        esc_print(f, b->inputType);
        fputs("|name=", f);
        esc_print(f, b->name);
        fputs("|value=", f);
        esc_print(f, b->value);
        fputs("|placeholder=", f);
        esc_print(f, b->placeholder);
        fputs("|formAction=", f);
        esc_print(f, b->formAction);
        fputs("|formMethod=", f);
        esc_print(f, b->formMethod);
    }
    if (b->type == DOC_BLOCK_INPUT_SUBMIT)
    {
        fputs("|label=", f);
        esc_print(f, b->label);
        fputs("|formAction=", f);
        esc_print(f, b->formAction);
        fputs("|formMethod=", f);
        esc_print(f, b->formMethod);
    }
    if (b->type == DOC_BLOCK_READER_HEADER)
    {
        fputs("|host=", f);
        esc_print(f, b->host);
        fputs("|title=", f);
        esc_print(f, b->text);
        fputs("|readingTime=", f);
        esc_print(f, b->readingTime);
    }
    if (b->type == DOC_BLOCK_CODE_BLOCK)
    {
        fputs("|text=", f);
        esc_print(f, b->text);
    }
    if (b->inlineCount > 0)
    {
        for (int i = 0; i < b->inlineCount; i++)
        {
            fputs(" {", f);
            ser_inline(f, b->inlines[i]);
            fputc('}', f);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <dummy> <outfile>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[2], "w");
    if (!f)
    {
        perror("open");
        return 2;
    }

    for (size_t ci = 0; ci < sizeof(CASES) / sizeof(CASES[0]); ci++)
    {
        const RCase *c = &CASES[ci];
        TokenizeResult tr;
        if (tokenizer_tokenize(c->html, &tr) != 0)
        {
            fprintf(f, "== %s\nTOKENIZE-FAIL\n", c->name);
            continue;
        }
        DocParseResult doc;
        memset(&doc, 0, sizeof(doc));
        int rc = readability_distill(&tr, "Web Page", c->baseUrl, &doc);
        fprintf(f, "== %s\n", c->name);
        if (rc != 0)
        {
            fprintf(f, "DISTILL-FAIL\n");
            tokenizer_free_result(&tr);
            continue;
        }
        fputs("title=", f);
        esc_print(f, doc.title);
        fputs("|base=", f);
        esc_print(f, doc.baseUrl);
        fprintf(f, "|reader=%s|words=%d|time=", doc.isReaderMode ? "true" : "false",
                doc.readingTimeWords);
        esc_print(f, doc.readingTimeStr);
        fputc('\n', f);
        for (int i = 0; i < doc.blockCount; i++)
        {
            ser_block(f, doc.blocks[i]);
            fputc('\n', f);
        }
        readability_free_result(&doc);
        tokenizer_free_result(&tr);
    }
    fclose(f);
    printf("wrote %s\n", argv[2]);
    return 0;
}
