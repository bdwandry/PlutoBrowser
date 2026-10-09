/* R20 host harness: run the REAL bryanwandrych.com repro page through the
 * full browser stack (parse → attach → XS-NR split → render) exactly like
 * the r18 host test does. Engine 4 (XS-NR). Prints DOM evidence + timing.
 * Build: /tmp/nrsplit_build.sh (ASan/UBSan, -O2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "document.h"
#include "constants.h"
#include "core/logger.h"
extern PlaydateAPI *pluto_pd(void);
#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "jsbridge_bundler.h"

/* GC stress hook: with -DmxStress=1 the engine forces a full collect on
 * every slot allocation when gxStress > 0 — maximum pressure to shake out
 * unrooted-host-slot bugs. Harmless no-op without mxStress. (The fork's
 * global is renamed gxStress_nr by xs_nr_rename.h.) */
extern int gxStress_nr;
#define gxStress gxStress_nr

static char *read_file(const char *path, size_t *outLen)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *outLen = (size_t)n;
    return buf;
}

static double now_sec(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int count_text(const DocParseResult *rd, const char *needle)
{
    int hits = 0;
    if (!rd)
        return 0;
    for (int i = 0; i < rd->blockCount && rd->blocks; i++)
    {
        const DocBlock *blk = rd->blocks[i];
        if (!blk)
            continue;
        for (int j = 0; j < blk->inlineCount && blk->inlines; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->text && strstr(in->text, needle))
            {
                hits++;
                printf("[harness] text hit: %s\n", in->text);
            }
        }
    }
    return hits;
}

int main(int argc, char **argv)
{
    if (getenv("PLUTO_GC_STRESS"))
        gxStress = 1;
    const char *path = (argc > 1) ? argv[1] : "/tmp/pluto_repro.html";
    size_t n = 0;
    char *html = read_file(path, &n);
    if (!html)
    {
        fprintf(stderr, "no html: %s\n", path);
        return 2;
    }
    printf("[harness] page %zuB\n", n);

    /* Plan shape (pure C — proves the planner + slicing on real bytes). */
    {
        const char *tag = strstr(html, "<script>");
        const char *body = tag ? tag + 8 : "";
        while (*body == '\n' || *body == '\r')
            body++;
        const char *end = strstr(body, "</script>");
        size_t blen = end ? (size_t)(end - body) : 0;
        PlutoBundlerSpan *spans = (PlutoBundlerSpan *)malloc(
            sizeof(PlutoBundlerSpan) * PLUTO_BUNDLER_MAX_SPANS);
        PlutoBundlerPlan plan;
        plan.spans = spans;
        plan.maxSpans = PLUTO_BUNDLER_MAX_SPANS;
        PlutoBundlerStatus rc = jsbridge_bundler_plan(body, blen, &plan);
        printf("[harness] plan rc=%d entries=%d nSpans=%d tailCount=%d "
               "tailIndex=%d tail=%u..%u\n",
               (int)rc, plan.entries, plan.nSpans, plan.tailCount,
               plan.tailIndex, plan.tailStart, plan.tailEnd);
        free(spans);
    }

    logger_init(pluto_pd()); /* route [js] traces to stdout like jstest */
    jsbridge_set_engine((argc > 2) ? atoi(argv[2]) : 4); /* 4=XS-NR, 2=QuickJS */
    DocParseResult *rd = calloc(1, sizeof(DocParseResult));
    double t0 = now_sec();
    int rc = document_parse_ex(html, "file://pluto_repro.html", MODE_RAW_HTML, NULL,
                               DOC_SCRIPT_RUN_KEEP, NULL, rd);
    double dt = now_sec() - t0;
    printf("[harness] parse+split+run: %.2fs jsErrors=%d lastError='%s'\n",
           dt, rd->jsErrors, rd->jsLastError[0] ? rd->jsLastError : "");
    int any = count_text(rd, "");
    int bw = count_text(rd, "Wandrych");
    int react = count_text(rd, "Project");
    printf("[harness] blocks=%d links=%d (inlines with text: %d) wandrych=%d "
           "project=%d\n",
           rd->blockCount, rd->linkCount, any, bw, react);
    int failed = rc != 0 || rd->jsErrors != 0;
    document_free(rd);
    free(rd);
    free(html);
    return failed ? 1 : 0;
}
