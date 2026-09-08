/*
 * PlutoBrowser — cloud_layout.h
 * Port of Source/render/cloud_layout.lua (reference, 152 lines).
 *
 * Lua → C function map:
 *   CloudLayout.parse(jsonString, baseUrl) → cloud_parse() + cloud_free_doc()
 *   CloudLayout.build(doc)                 → cloud_build()
 *   CloudLayout.draw(scrollY)              → cloud_draw()
 *   (module state renderItems)             → internal item array + accessors
 *   (module state totalHeight)             → cloud_total_height()
 *   (module state selectedInputItem)       → cloud_selected_input_item()
 *
 * Preserved reference semantics (verified against the verbatim Lua):
 *   - parse: any json.decode failure → { title="Parse Error",
 *     elements={}, totalHeight=240 }; missing fields fall back to
 *     title="Cloud Page", elements={}, totalHeight=240.
 *   - build: NULL/empty doc → items cleared, totalHeight = CONTENT_HEIGHT;
 *     elements with y < 0 are skipped; text/image/link/input/submit handled;
 *     unknown element types are silently ignored (Lua if/elseif chain).
 *     LinkManager.clear() runs first. NOTE (reference quirk): link elements
 *     go through LinkManager.addLink which records href only — the Lua
 *     addLink drops the isFormInput/inputBlock fields the caller intended,
 *     so C does exactly the same via lm_add_link().
 *   - draw: white content background, clip rect, per-item culling
 *     (drawY + h >= CONTENT_Y && drawY <= SCREEN_HEIGHT), text uses
 *     drawTextInRect w+10/h+10 kWrapWord/kAlignTextLeft (Lua default wrap =
 *     word, alignment = left), input value-or-placeholder text inset by 4,
 *     submit = black round-rect + centered FillWhite bold label, selected
 *     link gets a 3px round-rect ring inset -2/+4, then the scrollbar.
 *     C quirk parity: an input with empty value AND nil placeholder draws
 *     empty (Lua drawTextInRect(nil) would error; never occurs in practice —
 *     C draws "" which is the benign superset).
 */
#ifndef PLUTO_CLOUD_LAYOUT_H
#define PLUTO_CLOUD_LAYOUT_H

#include "pd_api.h"

/* One element of a parsed cloud document (Lua doc.elements[i] projection). */
typedef struct CloudElement
{
    char *type;         /* "text" | "image" | "link" | "input" | "submit" | other */
    int x, y, w, h;     /* Lua `el.y >= 0` gate uses y; non-numbers → parse error */
    char *text;         /* text elements */
    char *font;         /* "large" | "bold" | "mono" | anything else = body */
    char *src;          /* image elements */
    char *href;         /* link elements */
    char *inputType;    /* input/submit */
    char *name;
    char *value;
    char *placeholder;
    char *formAction;
    char *label;
} CloudElement;

typedef struct CloudDoc
{
    char *title;
    CloudElement *elements;
    int elementCount;
    int totalHeight;
} CloudDoc;

/* CloudLayout.parse: decode a cloud-layout JSON document. Returns NULL on
 * any JSON syntax error — the caller then uses cloud_parse_error_doc()
 * (the Lua "Parse Error" literal doc). baseUrl is accepted for signature
 * parity with the reference; the Lua parse() ignores it. */
CloudDoc *cloud_parse(const char *jsonString, const char *baseUrl);

/* The Lua literal `{ title = "Parse Error", elements = {}, totalHeight = 240 }`
 * as a heap doc (caller frees with cloud_free_doc). */
CloudDoc *cloud_parse_error_doc(void);

void cloud_free_doc(CloudDoc *doc);

/* CloudLayout.build: consume the doc into render items + link rects. */
void cloud_build(const CloudDoc *doc);

/* CloudLayout.draw. */
void cloud_draw(int scrollY);

/* Module state accessors (tests / main.c integration). */
int cloud_total_height(void);
int cloud_item_count(void);
void cloud_clear(void); /* items + totalHeight reset (Lua rebuild from scratch) */

#endif /* PLUTO_CLOUD_LAYOUT_H */
