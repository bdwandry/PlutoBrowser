/*
 * PlutoBrowser — scale.h
 * Port of Source/render/decoders/scale.lua (reference, 85 lines).
 *
 * Lua → C function map:
 *   Scale.boxSizes(srcW,srcH,maxW,maxH) → scale_box_sizes()
 *   Scale.newAccum(...)                 → scale_accum_new()
 *   acc.addRow(row)                     → scale_accum_add_row()
 *   acc.finish()                        → scale_accum_finish()
 *   (garbage collect)                   → scale_accum_free()
 *
 * Preserved semantics:
 *   - Integer box size so target ~= src / box, both clamped to maxW/maxH;
 *     target = floor(src / box) (min 1).
 *   - Streaming: only one source row + the tiny output grid are held in
 *     memory at once (~16MB RAM constraint on the Playdate).
 *   - Output rows round HALF-UP: floor(sum / divisor + 0.5).
 *   - Trailing partial row block uses divisor boxW * filledRows.
 *   - addRow(NULL) is a no-op (Lua `if not row then return end`).
 */
#ifndef PLUTO_SCALE_H
#define PLUTO_SCALE_H

#include <stdint.h>

typedef struct ScaleAccum ScaleAccum;

/* Returns boxW, boxH, targetW, targetH (target = floor(src/box), min 1). */
void scale_box_sizes(int srcW, int srcH, int maxW, int maxH,
                     int *boxW, int *boxH, int *targetW, int *targetH);

/* Creates a streaming accumulator; NULL on allocation failure. */
ScaleAccum *scale_accum_new(int srcW, int srcH, int maxW, int maxH);

/* Feed one source row of srcW grayscale bytes (0..255). NULL row = no-op.
 * Every boxH rows emits one output row internally (Lua parity). */
void scale_accum_add_row(ScaleAccum *acc, const uint8_t *row);

/* Finish: emits the trailing partial row block if any. Returns the output
 * row pointers (acc-owned, *outCount rows of *outWidth bytes) or NULL.
 * Rows stay valid until scale_accum_free. */
uint8_t **scale_accum_finish(ScaleAccum *acc, int *outCount, int *outWidth);

void scale_accum_free(ScaleAccum *acc);

#endif /* PLUTO_SCALE_H */
