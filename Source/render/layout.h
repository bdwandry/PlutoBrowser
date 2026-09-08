/*
 * PlutoBrowser — layout.h
 * Port of Source/render/layout.lua (reference, 1472 lines).
 *
 * Lua → C function map (one-to-one):
 *   (local) normalizeAlign            → layout_normalize_align()   (test seam)
 *   (local) toRoman                   → layout_to_roman()          (test seam)
 *   (local) toAlpha                   → layout_to_alpha()          (test seam)
 *   (local) orderedMarker             → layout_ordered_marker()    (test seam)
 *   (local) tabAdvance                → layout_tab_advance()       (internal)
 *   (local) expandTabColumns          → layout_expand_tab_columns() (test seam)
 *   (local) breakLines                → layout_break_lines()       (internal)
 *   (local) emitFlow                  → layout_emit_flow()         (internal)
 *   Layout.build(doc)                 → layout_build(doc)
 *   Layout.draw(scrollY)              → layout_draw(scrollY)
 *   Layout.evictOffscreen(scrollY)    → layout_evict_offscreen(scrollY)
 *   Layout.evictHoveredImage(cur)     → layout_evict_hovered_image(cur)
 *   Layout.showOnDemandOverlay(...)   → layout_show_on_demand_overlay(...)
 *   Layout.clearOnDemandOverlay()     → layout_clear_on_demand_overlay()
 *   Layout.drawOnDemandOverlay()      → layout_draw_on_demand_overlay()
 *   Layout.handleOnDemandInput()      → layout_handle_on_demand_input()
 *   Layout.renderItems/totalHeight    → layout_item_at()/layout_get_total_height()
 *
 * Render items are a tagged struct (Lua used open tables). Strings are
 * BORROWED from the parsed document (arena) — layout_clear() frees only
 * layout-owned memory, so free the document first, then the layout, or call
 * layout_clear() before document_free().
 *
 * Link rects: the reference attached per-rect metadata (isImage/src/alt,
 * isFormInput/inputBlock, isToggle/toggleKey/toggleOpen, inert) to the rect
 * tables passed to LinkManager.addLinkRect. The C port passes an equivalent
 * LayoutRectAux blob through lm_add_link_rect_ex(); main.c (navigation) reads
 * it back with lm_rect_aux().
 */
#ifndef PLUTO_LAYOUT_H
#define PLUTO_LAYOUT_H

#include "pd_api.h"
#include "html/document.h"
#include "render/link_manager.h"

/* ── Render item types (Lua item.type strings) ───────────────────────────── */
typedef enum
{
    LRI_TEXT = 0,
    LRI_READER_BADGE,
    LRI_LINE,
    LRI_QUOTE_BAR,
    LRI_CODE_BOX,
    LRI_TABLE_BOX,
    LRI_IMAGE,
    LRI_INPUT_FIELD,
    LRI_INPUT_SUBMIT,
    LRI_CHECKBOX_FIELD,
    LRI_SELECT_FIELD,
    LRI_HIDDEN_FIELD,
    LRI_PLACEHOLDER,
    LRI_METER,
    LRI_BOX_FRAME
} LayoutItemType;

/* Per-rect metadata the reference attached to LinkManager rect tables
 * (LMRectAux — defined in link_manager.h). inputItem points at the
 * LayoutItem owned by the layout (inputBlock). */
typedef LMRectAux LayoutRectAux;

/* ── Render item (union-style tagged struct) ─────────────────────────────── */
typedef struct LayoutItem
{
    LayoutItemType type;
    int x, y, w, h;

    /* LRI_TEXT */
    const char *text;    /* borrowed (word text / bullet / label) */
    LCDFont *font;       /* NULL → body font at draw time (Lua gfx.getFont) */
    unsigned flags;      /* DOC_INF_* copied from the inline */
    int sub, sup;        /* baseline shift flags (dy = 3 / -4) */
    int bold, italic, underline, code, small, big, mark, strike;
    int invert;          /* opts.invert from the block */
    const char *href;    /* borrowed or NULL */
    int anchorIndex;     /* 0 = none */

    /* LRI_LINE */
    int x1, y1, x2, y2;

    /* LRI_QUOTE_BAR: x, y1, y2 (h unused) */

    /* LRI_READER_BADGE */
    const char *host;    /* borrowed */
    const char *readingTime;

    /* LRI_CODE_BOX */
    char **lines;        /* borrowed from block */
    int lineCount;

    /* LRI_TABLE_BOX */
    const DocTable *table;
    const char *caption; /* borrowed */
    int border;          /* 1 = draw border (Lua border ~= false) */

    /* LRI_IMAGE */
    const char *alt, *src; /* borrowed */
    const char *imgHref;   /* block.href */
    LCDBitmap *img;        /* usually NULL (decoded images come from cache) */

    /* LRI_INPUT_FIELD / LRI_INPUT_SUBMIT / LRI_CHECKBOX_FIELD / LRI_SELECT_FIELD /
     * LRI_HIDDEN_FIELD (shared string fields, all borrowed from the block) */
    const char *name, *value, *placeholder, *label;
    const char *formAction, *formMethod;
    const char *inputType;
    int disabled, readonly, required;
    int maxlength;      /* -1 = Lua nil */
    int radio, checked;
    int selectedIndex;  /* 1-based */
    DocOption **options; /* borrowed */
    int optionCount;

    /* LRI_METER */
    double mValue, mMin, mMax, mLow, mHigh, mOptimum;

    /* LRI_BOX_FRAME (uses x1..y2 above) */
    const char *toggleKey; /* borrowed or NULL */
    int toggleOpen;

    /* Back-pointer to the source DocBlock for input items (Lua items alias
     * the block table; form interaction mutates block.value/checked/
     * selectedIndex through it). Borrowed; NULL for non-input items. */
    void *block;

    /* Owned live value for input fields (layout_set_input_value). Heap copy
     * replacing the borrowed block->value while the keyboard is open; freed
     * in layout_clear(). NULL when the item still shows the block value. */
    char *ownedValue;
} LayoutItem;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
void layout_init(PlaydateAPI *pd);
void layout_clear(void); /* frees item array + overlay/requested state */

/* ── Build (port of Layout.build) ────────────────────────────────────────── */
void layout_build(DocParseResult *doc);

/* ── Draw (port of Layout.draw) ──────────────────────────────────────────── */
void layout_draw(int scrollY);

/* ── State accessors ─────────────────────────────────────────────────────── */
int layout_get_total_height(void);

/* Battery probe: breakLines structure summary (device build has no printf
 * diffing, so the battery asserts on counts/widths instead of full dumps). */
typedef struct
{
    int lineCount; /* capped at 16 */
    int lineWordCount[16];
    int lineWidth[16];
} LayoutBreakProbe;
int layout_test_break_lines_probe(const DocInline **inlines, int inlineCount,
                                  int maxW, LCDFont *font, int bold, int lineH,
                                  LayoutBreakProbe *out);
/* 1 when the last layout_build() hit the reference's Layout Error path
 * (percent-width tables raise in the reference's table branch). */
int layout_build_failed(void);
int layout_get_item_count(void);
const LayoutItem *layout_item_at(int index); /* 0-based; NULL if out of range */

/* Set the visible text of a form input item (port of Lua's direct
 * activeInputField.value = entered mutation, which the C port must do through
 * an accessor because LayoutItem and DocBlock are separate structs). The value
 * is owned by the item and freed on layout_clear(); the renderer and
 * form_item_value() both see it immediately. Pass NULL to clear. */
void layout_set_input_value(const LayoutItem *item, const char *text);
const LayoutItem *layout_get_selected_input(void);
void layout_set_selected_input(const LayoutItem *item);
int layout_get_on_demand_consumed(void);
void layout_clear_on_demand_consumed(void);
const char *layout_get_hovered_image_src(void);

/* ── Eviction (viewport/hover image modes) ───────────────────────────────── */
void layout_evict_offscreen(int scrollY);
void layout_evict_hovered_image(const char *currentHoveredSrc);

/* ── On-demand overlay ───────────────────────────────────────────────────── */
void layout_show_on_demand_overlay(const char *src, const char *href, const char *alt);
void layout_clear_on_demand_overlay(void);
void layout_draw_on_demand_overlay(void);
/* Returns "view"/"unload"/"link"/"cancel" or NULL; btnPushed = PDButtons bits. */
const char *layout_handle_on_demand_input(unsigned int btnPushed);
int layout_has_on_demand_overlay(void);
/* The overlay's link href (Lua: Layout.onDemandOverlay.href). "" when none. */
const char *layout_on_demand_href(void);

/* ── Test seams (vector parity vs the Lua reference harness) ─────────────── */
/* Measure function: Lua Style.getTextWidth(font, text). Swappable so the
 * host battery can pin deterministic metrics; NULL restores the default
 * (pd getTextWidth, NULL font → body font, empty → 0). */
typedef int (*LayoutMeasureFn)(LCDFont *font, const char *text);
void layout_set_measure(LayoutMeasureFn fn);

/* (local) helpers exercised directly by the battery */
const char *layout_normalize_align(const char *a); /* NULL/invalid → NULL */
void layout_to_roman(int n, char *out, size_t cap);
void layout_to_alpha(int n, char *out, size_t cap);
/* Writes "<marker>" (no dot) for type '1','a','A','i','I' (0/other → '1'). */
void layout_ordered_marker(int number, char markerType, char *out, size_t cap);
/* Tab expansion for code lines: out holds ≤ cap-1 chars. */
void layout_expand_tab_columns(const char *line, char *out, size_t cap);

/* Host-battery hooks (no-ops in the app; used by tests/lua_reference/p31). */
void layout_test_dump_break_lines(const DocInline **inlines, int inlineCount,
                                  int maxW, LCDFont *font, int bold, int lineH,
                                  void (*emitLine)(const char *s));
void layout_test_run_emit(const DocInline **inlines, int inlineCount, int maxW,
                          LCDFont *font, int bold, int lineH, int startX,
                          int maxWX, const char *align, int startY);
int layout_test_get_emit_line_h(void);
int layout_test_get_emit_end_y(void);

#endif /* PLUTO_LAYOUT_H */
