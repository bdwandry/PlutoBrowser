// scale.c — C port of Source/render/decoders/scale.lua (Scale).
//
// Shared box-filter downscaler for the image decoders: single source row +
// tiny output grid in memory, integer box sizes, no fractional math.

#include "render/decoders/scale.h"

#include <math.h>
#include <string.h>

#include "util/mem.h"

void scale_box_sizes(int srcW, int srcH, int maxW, int maxH,
                     int* boxW, int* boxH, int* targetW, int* targetH) {
    if (maxW < 1) maxW = 1;
    if (maxH < 1) maxH = 1;
    int bw = (srcW + maxW - 1) / maxW;   /* ceil(srcW/maxW) */
    int bh = (srcH + maxH - 1) / maxH;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    *boxW = bw;
    *boxH = bh;
    int tw = srcW / bw;
    int th = srcH / bh;
    *targetW = tw < 1 ? 1 : tw;
    *targetH = th < 1 ? 1 : th;
}

ScaleAccum* scale_accum_new(int srcW, int srcH, int maxW, int maxH) {
    ScaleAccum* acc = (ScaleAccum*)pluto_malloc(sizeof(ScaleAccum));
    if (acc == NULL) return NULL;
    memset(acc, 0, sizeof(*acc));

    scale_box_sizes(srcW, srcH, maxW, maxH,
                    &acc->boxW, &acc->boxH,
                    &acc->targetW, &acc->targetH);
    acc->srcW = srcW;

    acc->accum =
        (long long*)pluto_malloc(sizeof(long long) * acc->targetW);
    if (acc->accum == NULL) {
        pluto_free(acc);
        return NULL;
    }
    memset(acc->accum, 0, sizeof(long long) * acc->targetW);
    return acc;
}

void scale_accum_add_row(ScaleAccum* acc, const unsigned char* row) {
    if (acc == NULL || row == NULL) return;
    const int srcW = acc->srcW;
    long long* accum = acc->accum;

    if (acc->boxW == 1) {
        /* Fast path: 1:1 horizontal */
        for (int i = 0; i < acc->targetW; i++)
            accum[i] += row[i];
    } else {
        int x = 0;
        for (int oc = 0; oc < acc->targetW; oc++) {
            int xEnd = x + acc->boxW;          /* exclusive */
            if (xEnd > srcW) xEnd = srcW;
            long long s = 0;
            for (int k = x; k < xEnd; k++) s += row[k];
            accum[oc] += s;
            x += acc->boxW;
        }
    }

    acc->filled++;
    if (acc->filled >= acc->boxH) {
        long divisor = (long)acc->boxW * acc->boxH;
        int* r = (int*)pluto_malloc(sizeof(int) * acc->targetW);
        if (r == NULL) return;   /* drop row on OOM (Lua would throw) */
        for (int i = 0; i < acc->targetW; i++)
            r[i] = (int)floor((double)accum[i] / divisor + 0.5);
        if (acc->oy == acc->capOut) {
            int nc = acc->capOut ? acc->capOut * 2 : 8;
            int** no = (int**)pluto_realloc(acc->out,
                                            sizeof(int*) * nc);
            if (no == NULL) { pluto_free(r); return; }
            acc->out = no;
            acc->capOut = nc;
        }
        acc->out[acc->oy++] = r;
        acc->count++;
        memset(accum, 0, sizeof(long long) * acc->targetW);
        acc->filled = 0;
    }
}

int scale_accum_finish(ScaleAccum* acc, int* outTw, int* outTh) {
    if (acc == NULL) return 0;
    if (acc->filled > 0) {
        /* Trailing partial block (src height not divisible by boxH) */
        long d = (long)acc->boxW * acc->filled;
        int* r = (int*)pluto_malloc(sizeof(int) * acc->targetW);
        if (r != NULL) {
            for (int i = 0; i < acc->targetW; i++)
                r[i] = (int)floor((double)acc->accum[i] / d + 0.5);
            if (acc->oy == acc->capOut) {
                int nc = acc->capOut ? acc->capOut * 2 : 8;
                int** no = (int**)pluto_realloc(acc->out,
                                                sizeof(int*) * nc);
                if (no != NULL) { acc->out = no; acc->capOut = nc; }
            }
            if (acc->oy < acc->capOut) {
                acc->out[acc->oy++] = r;
                acc->count++;
            } else {
                pluto_free(r);
            }
        }
        acc->filled = 0;
    }
    if (outTw != NULL) *outTw = acc->targetW;
    if (outTh != NULL) *outTh = acc->targetH;
    return acc->count;
}

void scale_accum_free(ScaleAccum* acc) {
    if (acc == NULL) return;
    for (int i = 0; i < acc->oy; i++) pluto_free(acc->out[i]);
    pluto_free(acc->out);
    pluto_free(acc->accum);
    pluto_free(acc);
}
