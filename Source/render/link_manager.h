/*
 * PlutoBrowser — link_manager.h
 * Port of Source/render/link_manager.lua (reference, 181 lines).
 *
 * Lua → C function map (one-to-one):
 *   LinkManager.clear()                 → lm_clear()
 *   LinkManager.clearSelection()        → lm_clear_selection()
 *   LinkManager.addLinkRect(h,t,r,ai)   → lm_add_link_rect()   [ai 0 = Lua nil]
 *   LinkManager.getCount()              → lm_get_count()
 *   LinkManager.getSelectedLink()       → lm_get_selected_link() (+ _index)
 *   LinkManager.selectNext(scrollY)     → lm_select_next()
 *   LinkManager.selectPrev(scrollY)     → lm_select_prev()
 *   LinkManager.drawSelectedHighlight() → lm_draw_selected_highlight()
 *   LinkManager.isHighlighted(h,x,y)    → lm_is_highlighted()
 *   LinkManager.addLink(link)           → lm_add_link() (flat fields)
 *   LinkManager.getHoveredLink(x,pageY) → lm_get_hovered_link()
 *   (local) findInitialSelection        → lm_find_initial_selection() (static)
 *
 * Preserved reference quirks (verified against the Lua source):
 *   - Merge requires the PASSED text to be non-nil and equal to the stored
 *     text: `lastLink.text == text` with text=nil never matches (nil ≠
 *     string), so addLinkRect(href, nil, ...) always creates a new link.
 *   - anchorIndex merge is skipped only when the PASSED anchorIndex is nil
 *     (C 0 = Lua nil): pass 0 to ignore, pass N to require last.anchorIndex==N.
 *   - getHoveredLink hit-tests use raw w/h: `(r.w or 60)` fires only on
 *     nil in Lua, and C rects always carry numeric values (0 hit-tests as 0).
 *   - selectNext/Prev with no selection pick the link closest to the
 *     viewport center (two passes: in-view first, then nearest anywhere).
 */
#ifndef PLUTO_LINK_MANAGER_H
#define PLUTO_LINK_MANAGER_H

#include <stdbool.h>

typedef struct
{
    int x, y, w, h;
} LMRect;

/* Per-rect metadata the Lua reference attached to rect tables (isImage/src,
 * isFormInput/inputBlock, isToggle/toggleKey/toggleOpen, inert). inputItem
 * points at a LayoutItem owned by the layout module. */
typedef struct LMRectAux
{
    int isImage;
    const char *src;
    const char *alt;
    int isFormInput;
    const void *inputItem;
    int isToggle;
    const char *toggleKey;
    int toggleOpen;
    int inert;
} LMRectAux;

typedef struct
{
    int index;      /* 1-based, Lua parity */
    char *href;     /* heap copy */
    char *text;     /* heap copy; Lua `text or href` */
    int anchorIndex; /* 0 = none (Lua nil) */
    int rectCount;
    LMRect *rects;  /* heap-grown (Lua table.insert parity) */
    int rectCap;
    LMRectAux *aux;  /* parallel metadata per rect (NULL entries when absent) */
    int auxCap;
    LMRect primaryRect; /* first rect */
} LMLink;

void lm_clear(void);
void lm_clear_selection(void);
void lm_add_link_rect(const char *href, const char *text, const LMRect *rect, int anchorIndex);
/* Same, with per-rect metadata (Lua rect table extras). aux may be NULL;
 * strings are borrowed (must outlive the link table — layout strings do). */
void lm_add_link_rect_ex(const char *href, const char *text, const LMRect *rect,
                         int anchorIndex, const LMRectAux *aux);
/* Metadata for a link's rect (index into the link's rect array); NULL if absent. */
const LMRectAux *lm_rect_aux(int linkIndex, int rectIndex);
/* 1-based access for tests/navigation; NULL if out of range. */
const LMLink *lm_link_at(int linkIndex);
int lm_get_link_count(void);
int lm_get_count(void);
int lm_get_selected_index(void); /* 0 = none */
const LMLink *lm_get_selected_link(void); /* NULL = none */
const LMLink *lm_select_next(int currentScrollY);
const LMLink *lm_select_prev(int currentScrollY);
void lm_draw_selected_highlight(int scrollY);
bool lm_is_highlighted(const char *href, int x, int y);
void lm_add_link(const char *href, int x, int y, int w, int h);
const LMLink *lm_get_hovered_link(int screenX, int pageY);

#endif /* PLUTO_LINK_MANAGER_H */
