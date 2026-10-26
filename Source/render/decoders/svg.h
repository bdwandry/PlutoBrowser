/*
 * PlutoBrowser — svg.h
 * SVG rasterizer (descendant of the svg.lua 451-line reference port).
 *
 * The scanner/parser stage keeps the reference's verbatim semantics:
 *   - getAttrs runs a double-quote pass then a single-quote pass; the single
 *     pass OVERWRITES duplicates (Lua table assignment order).
 *   - style wins over presentation attributes; last duplicate wins.
 *   - viewBox/width/height use string.match FIRST-MATCH-ANYWHERE, so
 *     stroke-width="4" before width="..." supplies srcW=4 (tc12 parity).
 *   - scale = min(maxW/srcW, maxH/srcH) capped at 2; target >= 20 px.
 *   - tx/ty = floor((v - viewBoxMin) * scale).
 *   - <use> splicing: href/xlink:href="#id" → first element with that id is
 *     re-serialized in place; the spliced copy now carries the use's
 *     transform/x/y and presentation attrs (the reference dropped them).
 *
 * The paint stage is a real SVG rasterizer (the reference drew every shape
 * as a 1px black outline — no fills, no gray, hence wireframe-looking
 * images):
 *   - fill/stroke: #rgb/#rrggbb, rgb()/rgba(), CSS named colors, none,
 *     inherit, currentColor, and url(#gradient) → linearGradient/
 *     radialGradient resolved to the average stop color (1-bit approximation).
 *   - every fill/stroke is dithered: luminance → 8x8 Bayer coverage →
 *     LCDPattern, matching the raster decoders' ordered-dither look.
 *     Pattern data-bit polarity is self-calibrated on first decode.
 *   - affine transforms (translate/scale/rotate/skewX/skewY/matrix) with a
 *     graphics-state stack (fill/stroke/stroke-width/fill-rule/opacity
 *     inheritance across <g>/<svg>/... containers).
 *   - true arc (A) path commands; polygon fills via fillPolygon (even-odd
 *     or nonzero per fill-rule); ellipse fills via fillEllipse. The old
 *     "ellipse aborts the whole decode" pcall-parity quirk is gone.
 *   - attribute values longer than 64 chars live in a 16KB per-tag value
 *     pool (the reference truncated path `d` data at 64 chars).
 *
 * NULL when: no "<svg", non-positive source dims, or zero drawn shapes.
 *
 * Documented deviations (C-side bounds, unreachable for realistic inputs):
 *   - attribute list capped (32 pairs), group-stack depth capped (128),
 *     path coordinate buffer capped (4096 numbers), fill polygon capped
 *     (2048 points), value pool 16KB per tag; Lua tables are unbounded.
 *   - decoding is not re-entrant (static workspaces), like every other
 *     PlutoBrowser decoder.
 */
#ifndef PLUTO_SVG_H
#define PLUTO_SVG_H

#include "pd_api.h"

/* Decode an SVG document into a new 1-bit LCDBitmap (white background),
 * dither-filled and downscaled into maxW x maxH. Returns NULL for: no
 * "<svg", non-positive source dims, or zero drawn shapes. Caller frees
 * the bitmap. */
LCDBitmap *svg_decode(const char *xml, int maxW, int maxH);

#endif /* PLUTO_SVG_H */
