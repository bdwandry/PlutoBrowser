#ifndef PLUTO_RENDER_DECODERS_SVG_H
#define PLUTO_RENDER_DECODERS_SVG_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;

/* C port of Source/render/decoders/svg.lua (SVGDecoder).
 *
 * Renders the web-icon subset of SVG (rect/circle/ellipse/line/polygon/
 * polyline/path with M L H V Z C S Q T A, g/a/symbol containers, defs skip,
 * <use> best-effort resolution, style="" override, inherited display:none /
 * visibility:hidden) into a 1-bit gray grid: 255 = white canvas, 0 = ink.
 * The software rasterizer (Bresenham lines, midpoint circles/ellipses,
 * rounded rects with quadrant arcs) is deterministic; device parity with
 * playdate.graphics primitives is approximate by design.
 *
 * svg_decode_gray returns 0 on success with (*outRows)[y][x] valid for
 * y < *outH, x < *outW and *outDrawn >= 1; -1 on reject (no "<svg",
 * nonpositive source dims, zero shapes drawn, OOM). Free with
 * svg_free_rows(). */
int svg_decode_gray(const char* data, size_t len,
                    int maxW, int maxH,
                    uint8_t*** outRows, int* outW, int* outH,
                    int* outDrawn);

void svg_free_rows(uint8_t** rows, int h);

/* device/simulator wrapper returning an LCDBitmap (NULL on failure). */
struct LCDBitmap;
struct LCDBitmap* svg_decode(struct PlaydateAPI* pd, const char* data,
                             size_t len, int maxW, int maxH);

#endif
