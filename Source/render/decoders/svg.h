/*
 * PlutoBrowser — svg.h
 * Port of Source/render/decoders/svg.lua (reference, 451 lines).
 *
 * Lua → C function map:
 *   SVGDecoder.decode(xmlString, maxW, maxH) → svg_decode()
 *   getAttrs         → svg_get_attrs()          (internal)
 *   isHidden/hasInk  → svg_is_hidden/svg_has_ink (internal)
 *   parseStyle       → svg_parse_style()         (internal)
 *   mergeStyle       → svg_merge_style()         (internal)
 *   tokenizePathNumbers → svg_tokenize_numbers() (internal, char-level parser)
 *   tokenizePoints   → same tokenizer
 *   scanTags         → inline scan loop in svg_decode (verbatim traversal)
 *   expandUses       → svg_expand_uses()         (internal)
 *
 * Preserved semantics (verbatim, quirks included):
 *   - getAttrs runs a double-quote pass then a single-quote pass; the single
 *     pass OVERWRITES duplicates (Lua table assignment order).
 *   - isHidden/hasInk/mergeStyle style-wins-over-attr semantics.
 *   - viewBox/width/height use string.match FIRST-MATCH-ANYWHERE, so
 *     stroke-width="4" before width="..." supplies srcW=4 (tc12 parity).
 *   - scale = min(maxW/srcW, maxH/srcH) capped at 2; target >= 20 px.
 *   - tx/ty = floor((v - viewBoxMin) * scale).
 *   - The ellipse branch calls gfx.drawEllipse which DOES NOT EXIST in the
 *     SDK Lua API → error inside pcall → the whole decode returns nil.
 *     The C port reproduces this exactly (errFlag → NULL), per parity rules.
 *   - "clipPath" containers never match (scanTags lowercases the tag name but
 *     the reference compares against "clipPath") → dead container; kept dead.
 *   - Paths: M/L/H/V/Z/C(8-step)/Q(6-step)/S/T/A(line-to-endpoint) with the
 *     reference's exact segment arithmetic and odd-coordinate clamping (nil
 *     → 0).
 *   - <use> splicing: href/xlink:href="#id" → first element with that id is
 *     re-serialized as "<tag" .. attrs .. ">" and drawn in place; the id'd
 *     original (typically inside defs) stays skipped.
 *   - NULL when drawn == 0 or on the ellipse error path.
 *
 * Documented deviations (C-side bounds, unreachable for realistic inputs):
 *   - attribute list capped (32 pairs), group-stack depth capped (128),
 *     path coordinate buffer capped (4096 numbers); Lua tables are unbounded.
 *   - decoding is not re-entrant (static workspaces), like every other
 *     PlutoBrowser decoder.
 */
#ifndef PLUTO_SVG_H
#define PLUTO_SVG_H

#include "pd_api.h"

/* Decode an SVG document into a new 1-bit LCDBitmap (white background, black
 * strokes), downscaled into maxW x maxH with the reference's box rules.
 * Returns NULL for: no "<svg", non-positive source dims, zero drawn shapes,
 * or the ellipse error path (Lua pcall parity). Caller frees the bitmap. */
LCDBitmap *svg_decode(const char *xml, int maxW, int maxH);

#endif /* PLUTO_SVG_H */
