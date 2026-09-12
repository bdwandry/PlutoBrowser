/*
 * PlutoBrowser — scale.c
 * Port of Source/render/decoders/scale.lua (reference, 85 lines).
 * Shared integer box-filter downscaler for the image decoders.
 * See scale.h for the Lua→C map and preserved semantics.
 */
#include <stdlib.h>
#include <string.h>
#include "render/decoders/scale.h"

struct ScaleAccum
{
    int srcW, srcH;
    int boxW, boxH, targetW, targetH;

    int *accum;    /* targetW column sums for the current output row */
    int filled;    /* source rows accumulated for the current output row */

    uint8_t **out; /* emitted rows (targetW bytes each) */
    int outCap;
    int oy;        /* rows emitted */
    int divisor;   /* boxW * boxH */
};

void scale_box_sizes(int srcW, int srcH, int maxW, int maxH,
                     int *boxW, int *boxH, int *targetW, int *targetH)
{
    /* Lua: boxW = max(1, ceil(srcW / max(1, maxW))) */
    int mw = maxW > 1 ? maxW : 1;
    int mh = maxH > 1 ? maxH : 1;
    int bw = (srcW + mw - 1) / mw; /* ceil */
    int bh = (srcH + mh - 1) / mh;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    /* Lua: targetW = max(1, floor(srcW / boxW)) */
    int tw = srcW / bw;
    int th = srcH / bh;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (boxW) *boxW = bw;
    if (boxH) *boxH = bh;
    if (targetW) *targetW = tw;
    if (targetH) *targetH = th;
}

ScaleAccum *scale_accum_new(int srcW, int srcH, int maxW, int maxH)
{
    ScaleAccum *a = (ScaleAccum *)calloc(1, sizeof(ScaleAccum));
    if (!a)
    {
        return NULL;
    }
    a->srcW = srcW;
    a->srcH = srcH;
    scale_box_sizes(srcW, srcH, maxW, maxH,
                    &a->boxW, &a->boxH, &a->targetW, &a->targetH);
    a->divisor = a->boxW * a->boxH;
    a->accum = (int *)calloc((size_t)a->targetW, sizeof(int));
    if (!a->accum)
    {
        free(a);
        return NULL;
    }
    return a;
}

void scale_accum_add_row(ScaleAccum *acc, const uint8_t *row)
{
    if (!acc || !row)
    {
        return; /* Lua: if not row then return end */
    }

    if (acc->boxW == 1)
    {
        /* Fast path: 1:1 horizontal, just accumulate. */
        for (int i = 0; i < acc->targetW; i++)
        {
            acc->accum[i] += row[i];
        }
    }
    else
    {
        int x = 0; /* Lua x is 1-based over row[k]; C is 0-based */
        for (int oc = 0; oc < acc->targetW; oc++)
        {
            int xEnd = acc->srcW < x + acc->boxW ? acc->srcW : x + acc->boxW;
            int s = 0;
            for (int k = x; k < xEnd; k++)
            {
                s += row[k];
            }
            acc->accum[oc] += s;
            x += acc->boxW;
        }
    }

    acc->filled++;
    if (acc->filled >= acc->boxH)
    {
        /* Emit one output row (Lua: floor(sum/divisor + 0.5) — half-up). */
        uint8_t *r = (uint8_t *)malloc((size_t)acc->targetW);
        if (r)
        {
            for (int i = 0; i < acc->targetW; i++)
            {
                r[i] = (uint8_t)((acc->accum[i] / acc->divisor) + ((acc->accum[i] % acc->divisor) * 2 >= acc->divisor ? 1 : 0));
            }
            if (acc->oy == acc->outCap)
            {
                int newCap = acc->outCap > 0 ? acc->outCap * 2 : 64;
                uint8_t **grown = (uint8_t **)realloc(acc->out, (size_t)newCap * sizeof(uint8_t *));
                if (!grown)
                {
                    free(r);
                    return;
                }
                acc->out = grown;
                acc->outCap = newCap;
            }
            acc->out[acc->oy++] = r;
        }
        memset(acc->accum, 0, (size_t)acc->targetW * sizeof(int));
        acc->filled = 0;
    }
}

uint8_t **scale_accum_finish(ScaleAccum *acc, int *outCount, int *outWidth)
{
    if (!acc)
    {
        if (outCount) *outCount = 0;
        if (outWidth) *outWidth = 0;
        return NULL;
    }

    if (acc->filled > 0)
    {
        /* Trailing partial block: divisor = boxW * filled (Lua parity). */
        int d = acc->boxW * acc->filled;
        uint8_t *r = (uint8_t *)malloc((size_t)acc->targetW);
        if (r)
        {
            for (int i = 0; i < acc->targetW; i++)
            {
                r[i] = (uint8_t)((acc->accum[i] / d) + ((acc->accum[i] % d) * 2 >= d ? 1 : 0));
            }
            if (acc->oy == acc->outCap)
            {
                int newCap = acc->outCap > 0 ? acc->outCap * 2 : 64;
                uint8_t **grown = (uint8_t **)realloc(acc->out, (size_t)newCap * sizeof(uint8_t *));
                if (grown)
                {
                    acc->out = grown;
                    acc->outCap = newCap;
                }
            }
            if (acc->oy < acc->outCap)
            {
                acc->out[acc->oy++] = r;
            }
            else
            {
                free(r);
            }
        }
        acc->filled = 0;
    }

    if (outCount) *outCount = acc->oy;
    if (outWidth) *outWidth = acc->targetW;
    return acc->out;
}

void scale_accum_free(ScaleAccum *acc)
{
    if (!acc)
    {
        return;
    }
    for (int i = 0; i < acc->oy; i++)
    {
        free(acc->out[i]);
    }
    free(acc->out);
    free(acc->accum);
    free(acc);
}
