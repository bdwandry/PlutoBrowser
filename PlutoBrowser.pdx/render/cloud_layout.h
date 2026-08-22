#ifndef PLUTO_RENDER_CLOUD_LAYOUT_H
#define PLUTO_RENDER_CLOUD_LAYOUT_H

#include <stddef.h>

struct PlaydateAPI;
struct JsonValue;

/* C port of Source/render/cloud_layout.lua (CloudLayout).
 *
 * Parses the JSON payload served on the MODE_OPERA_DS "cloud" path and
 * renders it: text blocks, images (via ImageDecoder), form input boxes,
 * submit buttons, link rects (via LinkManager) and a scrollbar.
 *
 * Faithful parity notes:
 *  - parse(jsonString, baseUrl): baseUrl is accepted but UNUSED in the
 *    Lua source; kept in the C signature for parity.
 *  - Malformed JSON parses to { title="Parse Error", elements={},
 *    totalHeight=240 }; missing fields fall back to "Cloud Page", {},
 *    240 respectively.
 *  - build() skips elements with y < 0 and offsets by
 *    CONTENT_Y + el.y. Empty/absent element lists reset totalHeight to
 *    CONTENT_HEIGHT.
 *  - Text font selection: font=="large" -> heading1, "bold" -> bodyBold,
 *    "mono" -> mono, otherwise body; each falls back to the system font
 *    when its slot is unset (Lua: `Style.fontX or gfx.getFont()`).
 *  - input/submit items also register a LinkManager entry so they are
 *    selectable; those links carry isFormInput/inputBlock instead of an
 *    href (see lm_add_form_input).
 *  - draw(scrollY) paints its own white content background, clips to the
 *    content area, draws the selected-link outline itself (black, line
 *    width 3, roundRect r3 inflated by 2px -- distinct from
 *    LinkManager.drawSelectedHighlight) and finishes with a scrollbar.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CL_TEXT = 0,
    CL_IMAGE,
    CL_INPUT,
    CL_SUBMIT
} ClItemType;

typedef struct ClItem ClItem;

struct ClItem {
    ClItemType type;
    int x, y, w, h;
    /* CL_TEXT */
    char* text;              /* owned */
    void* font;              /* LCDFont* resolved at build time */
    /* CL_IMAGE */
    char* src;               /* owned */
    char* alt;               /* owned (Lua default "Image") */
    /* CL_INPUT / CL_SUBMIT */
    char* inputType;         /* owned */
    char* name;              /* owned */
    char* value;             /* owned */
    char* placeholder;       /* owned */
    char* formAction;        /* owned */
    char* label;             /* owned */
};

typedef struct ClDoc ClDoc;

void cl_init(struct PlaydateAPI* pd);

/* Lua CloudLayout.parse(jsonString, baseUrl): parses jsonString into a
 * document. Never returns NULL except on host OOM. */
ClDoc* cl_parse(const char* jsonStr, size_t len, const char* baseUrl);
void   cl_doc_free(ClDoc* doc);

/* Test/introspection accessors mirroring the returned doc table. */
const char* cl_doc_title(const ClDoc* doc);      /* never NULL */
const struct JsonValue* cl_doc_elements(const ClDoc* doc); /* array|NULL */
double cl_doc_total_height(const ClDoc* doc);

/* Lua CloudLayout.build(doc): clears render items + LinkManager and
 * flattens doc->elements into drawable items. NULL doc is allowed. */
void   cl_build(const ClDoc* doc);

/* Lua CloudLayout.draw(scrollY). Host builds: no-op. */
void   cl_draw(int scrollY);

double cl_total_height(void);
int    cl_item_count(void);
const ClItem* cl_item_at(int i);          /* NULL when out of range */
const ClItem* cl_selected_input(void);    /* nil in current Lua source */

void cl_free_items(void);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_RENDER_CLOUD_LAYOUT_H
