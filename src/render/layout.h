#ifndef PLUTO_RENDER_LAYOUT_H
#define PLUTO_RENDER_LAYOUT_H

#include <stddef.h>

struct PlaydateAPI;
struct DocDocument;
struct LmRect;

/* C port of Source/render/layout.lua (Layout) -- P26B.
 *
 * Block layout engine: walks DocDocument blocks and flattens them into
 * positioned render items (text runs with per-word wrapping, code boxes,
 * tables, form controls, images, boxes/meters/placeholders), registers
 * link hitboxes in LinkManager, then paints them scrolled (draw()).
 *
 * Faithful parity notes:
 *  - build(): currentY starts CONTENT_Y+8; marginX = CONTENT_MARGIN+2;
 *    maxWidth = CONTENT_TEXT_WIDTH-4; totalHeight = max(currentY+20,
 *    CONTENT_HEIGHT). Empty/absent blocks -> totalHeight = CONTENT_HEIGHT.
 *  - Word wrap drops leading whitespace, collapses whitespace runs to one
 *    space width, tab-containing runs advance to the next 8-column tab
 *    stop measured from the running line width.
 *  - emitFlow merges consecutive words sharing href+anchorIndex into ONE
 *    LinkManager rect spanning the gap spaces; sub/sup shift y by +3/-4.
 *  - Headings level<=2 append an underline "line" item (+6 y).
 *  - Table cell links register on FIRST draw that paints a table box
 *    (Layout.cellLinksRegistered); their rect y is rowY+scrollY (page
 *    space). The Lua source reads `inl.inert` after its inline loop has
 *    ended -- a nil global lookup -- so inert is ALWAYS false there; the
 *    port replicates that exactly.
 *  - draw() honors imageMode for image items: DISABLED box "[Image Off]",
 *    ONDEMAND tap-prompt/hatching states, HOVER only when hovered,
 *    VIEWPORT enqueue-when-visible, ALL always via ImageDecoder.draw or
 *    pre-decoded bitmap scaled to fit.
 *  - On-demand overlay input returns view/unload/link/cancel actions and
 *    raises the one-frame onDemandConsumed flag.
 *
 * Lifetime contract: items BORROW strings from the document they were
 * built from (and own a few synthesized ones like bullets/truncated
 * labels). Call layout_free_items() before freeing that document; the
 * next layout_build() does this implicitly.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void PlutoFont;

enum {
    LIT_TEXT = 0,
    LIT_READER_BADGE,
    LIT_LINE,
    LIT_QUOTE_BAR,
    LIT_CODE_BOX,
    LIT_TABLE_BOX,
    LIT_IMAGE,
    LIT_INPUT_FIELD,
    LIT_INPUT_SUBMIT,
    LIT_CHECKBOX_FIELD,
    LIT_SELECT_FIELD,
    LIT_PLACEHOLDER,
    LIT_METER,
    LIT_BOX_FRAME,
    LIT_HIDDEN_FIELD
};

/* rows/caption of table items borrow the document's DocTableRow array
 * (declared in html/document.h; kept opaque here via void* accessors) */
struct DocTableRow;

typedef struct LItem {
    int type;
    int x, y, w, h;

    /* text / generic label */
    char* text;              /* owned */
    PlutoFont* font;
    unsigned char bold, italic, underline, code, smallFlag, bigFlag;
    unsigned char mark, strike, invert;
    char* href;              /* owned or NULL */
    long anchorIndex;        /* -1 when none */

    /* reader_badge */
    char* host;              /* owned */
    char* readingTime;       /* owned */

    /* line / quote_bar / box_frame */
    int x1, y1, x2, y2;

    /* code_box */
    char** lines;            /* owned array of owned strings */
    size_t nLines;

    /* table_box */
    struct DocTableRow* rows;   /* borrowed from doc */
    size_t nRows;
    char* caption;           /* owned copy or NULL */
    int border;

    /* image */
    char* alt;               /* owned or NULL */
    char* src;               /* owned or NULL */
    void* img;               /* borrowed LCDBitmap* or NULL */

    /* form controls */
    char* inputType;         /* owned ("text"/"textarea"/"password"...) */
    char* name;              /* owned (input default "q", checkbox "") */
    char* value;             /* owned ("" allowed) */
    char* placeholder;       /* owned ("" allowed) */
    char* formAction;        /* owned ("" allowed) */
    char* formMethod;        /* owned ("get") */
    char* label;             /* owned (submit/checkbox/placeholder) */
    int disabled, readonly, required;
    int maxlength;           /* -1 unset */
    int radio, checked;
    int fieldWidth, fieldRows; /* -1 unset (mirrored at build time only) */

    /* select_field */
    void* options;           /* borrowed DocSelectOpt* */
    size_t nOptions;
    int selectedIndex;

    /* meter */
    double mValue, mMax, mMin, mLow, mHigh, mOptimum;

    char* toggleKey;         /* owned or NULL */
    int toggleOpen;
} LItem;

void layout_init(struct PlaydateAPI* pd);

/* Lua Layout.build(doc): resets state, clears LinkManager, flattens
 * blocks into render items. NULL doc / zero blocks -> empty page height. */
void layout_build(const struct DocDocument* doc);

/* free all items + reset derived state (called by next build too) */
void layout_free_items(void);

int          layout_item_count(void);
const LItem* layout_item_at(int i);      /* NULL out of range */
double       layout_total_height(void);

/* Lua Layout.selectedInputItem (borrowed pointer to a rendered item) */
const LItem* layout_selected_input(void);
void         layout_set_selected_input(const LItem* it);

/* on-demand overlay state (Lua Layout.onDemandOverlay / onDemandConsumed /
 * onDemandRequested) */
int layout_has_on_demand_overlay(void);
void layout_show_on_demand_overlay(const char* src, const char* href,
                                   const char* alt);
void layout_clear_on_demand_overlay(void);
const char* layout_od_src(void);
const char* layout_od_href(void);
const char* layout_od_alt(void);
int  layout_on_demand_consumed(void);
void layout_clear_on_demand_consumed(void);
int  layout_on_demand_requested(const char* src);

/* handleOnDemandInput(pressed): pass the just-pressed button bitmask
 * (playdate kButtonA/kButtonB values). Consumes overlay on action and
 * raises onDemandConsumed for one frame. */
enum {
    PLUTO_KBUTTON_A = 0x1,
    PLUTO_KBUTTON_B = 0x2
};

typedef enum {
    OD_NONE = 0,
    OD_VIEW,
    OD_UNLOAD,
    OD_LINK,
    OD_CANCEL
} PlutoOdAction;

PlutoOdAction layout_handle_on_demand_input(unsigned int pressed);

/* device/sim painters; no-ops on host builds (pd == NULL) */
void layout_draw(int scrollY);
void layout_evict_offscreen(int scrollY);
void layout_evict_hovered_image(const char* currentHoveredSrc);
void layout_draw_on_demand_overlay(void);

/* hover tracking used by hover image mode (main loop owns the value) */
const char* layout_hovered_image_src(void);
void        layout_set_hovered_image_src(const char* src);

/* test hooks: ordered-list marker helpers */
char* layout_ordered_marker(int number, const char* markerType); /* owned */
char* layout_expand_tab_columns(const char* line);               /* owned */

#ifdef __cplusplus
}
#endif

#endif // PLUTO_RENDER_LAYOUT_H
