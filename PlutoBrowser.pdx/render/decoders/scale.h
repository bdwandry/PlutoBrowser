#ifndef PLUTO_RENDER_DECODERS_SCALE_H
#define PLUTO_RENDER_DECODERS_SCALE_H

/* C port of Source/render/decoders/scale.lua (Scale).
 *
 * Streaming integer box-filter downscaler: source rows of gray values
 * (0..255) go in via addRow, downscaled rows come out of finish.
 * Rounding is floor(sum/divisor + 0.5), exactly like the Lua original.
 */

typedef struct {
    int srcW, boxW, boxH, targetW, targetH;
    long long* accum;   /* targetW column sums for the current output row */
    int filled;         /* source rows folded into the current output row */
    int** out;          /* produced output rows (each targetW entries) */
    int capOut, oy, count;
} ScaleAccum;

/* Integer box size so that target ~= src / box, both clamped.
 * Returns boxW, boxH, targetW = floor(srcW/boxW), targetH = floor(srcH/boxH). */
void scale_box_sizes(int srcW, int srcH, int maxW, int maxH,
                     int* boxW, int* boxH, int* targetW, int* targetH);

/* NULL on OOM. */
ScaleAccum* scale_accum_new(int srcW, int srcH, int maxW, int maxH);

/* row: srcW gray values 0..255 (0-based; Lua rows are 1-based arrays). */
void scale_accum_add_row(ScaleAccum* acc, const unsigned char* row);

/* Flushes any trailing partial block (srcH not divisible by boxH) and
 * returns the number of produced rows; *outTw / *outTh get the grid dims
 * (th is the computed target -- actual count can exceed it on remainders,
 * a quirk preserved from Lua). Rows live in acc->out[0..count-1]. */
int scale_accum_finish(ScaleAccum* acc, int* outTw, int* outTh);

void scale_accum_free(ScaleAccum* acc);

#endif
