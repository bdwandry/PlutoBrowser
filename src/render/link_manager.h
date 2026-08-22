#ifndef PLUTO_RENDER_LINK_MANAGER_H
#define PLUTO_RENDER_LINK_MANAGER_H

#include <stddef.h>

struct PlaydateAPI;
/* device/sim only: installs the API pointer used for highlight drawing */
void lm_init(struct PlaydateAPI* pd);

/* C port of Source/render/link_manager.lua (LinkManager).
 *
 * Faithful parity notes:
 *  - addLinkRect merges into the LAST link when href+text match AND the
 *    NEW anchorIndex is -1 (Lua nil) or equals the stored one; a nil new
 *    anchor therefore merges across differing stored anchors.
 *  - findInitialSelection pass 1 = primary-rect center inside
 *    [scrollY, scrollY+CONTENT_HEIGHT], closest to viewport center;
 *    pass 2 = closest anywhere (caller scrolls it into view).
 *  - selectNext/Prev: simple wrap-around (the "skip logic" once mentioned
 *    in planning notes is not present in the current Lua source).
 *  - getHoveredLink hit-test falls back to w=60/h=14 for unset rect
 *    fields (stored here as -1).
 */

typedef struct {
    int x, y, w, h;   /* w or h == -1 means "unset" (hit-test default) */
    /* Per-rect extras carried opaquely inside Lua rect tables (layout.c
     * passes them through addLinkRect; link_manager.lua stores them
     * verbatim). All zero/NULL when absent. Strings are BORROWED from the
     * Layout render items / document and must outlive the link. */
    int   isToggle;          /* <summary> tap target */
    int   toggleOpen;
    const char* toggleKey;   /* "dN" */
    int   isImage;
    const char* src;
    const char* alt;
    int   inert;
    int   isFormInput;       /* layout.lua registers these per-rect */
    void* inputBlock;        /* borrowed LItem */
} LmRect;

typedef struct {
    char* href;       /* owned; NULL for form-input links (Lua nil) */
    char* text;       /* owned (defaults to href); NULL for form inputs */
    int   hasAnchor;
    long  anchorIndex;
    LmRect* rects;
    size_t nRects, capRects;
    LmRect primaryRect;
    /* Lua links are plain tables, so CloudLayout form-input links carry
     * two extra optional fields (both zero for regular links). */
    int   isFormInput;
    void* inputBlock;   /* borrowed; owned by CloudLayout render items */
} LmLink;

void   lm_clear(void);
void   lm_clear_selection(void);

void   lm_add_link_rect(const char* href, const char* text, LmRect rect,
                        long anchorIndex);
/* convenience wrapper mirroring LinkManager.addLink(link) */
void   lm_add_link(const char* href, int x, int y, int w, int h);

/* CloudLayout parity: form-input links carry no href/text (Lua nil) but
 * point back at their render item so later phases can activate forms
 * from a selection. Rect uses -1 "unset" semantics like everywhere else. */
void   lm_add_form_input(int x, int y, int w, int h, void* inputBlock);

size_t lm_get_count(void);
size_t lm_selected_index(void);      /* 0 == none; else 1-based */

/* returns selected link or NULL when out of range / none */
LmLink* lm_get_selected_link(void);

long    lm_find_initial_selection(int currentScrollY); /* 0 == none */

LmLink* lm_select_next(int currentScrollY);
LmLink* lm_select_prev(int currentScrollY);

/* device/sim only: draws the focus outline + side tab inside the content
 * clip rect; no-op on host builds */
void    lm_draw_selected_highlight(int scrollY);

int     lm_is_highlighted(const char* href, int x, int y);
LmLink* lm_get_hovered_link(int screenX, int pageY);

#endif
