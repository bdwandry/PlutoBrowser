// link_manager.c — C port of Source/render/link_manager.lua (LinkManager).
//
// Hyperlink focus & interaction manager. See link_manager.h for notes.

#include "render/link_manager.h"

#include <stdlib.h>
#include <string.h>

#include "core/constants.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_LM_PD 1
#endif

/* ── state ────────────────────────────────────────────────────────────── */

static LmLink* s_links = NULL;
static size_t s_nLinks = 0, s_capLinks = 0;
static size_t s_selected = 0;   /* 0 == none, else 1-based */

#ifdef PLUTO_LM_PD
#include "pd_api.h"
static PlaydateAPI* s_pd = NULL;

void lm_init(struct PlaydateAPI* pd) { s_pd = pd; }
#endif

static void lm_free_link(LmLink* l) {
    pluto_free(l->href);
    pluto_free(l->text);
    pluto_free(l->rects);
    memset(l, 0, sizeof(*l));
}

void lm_clear(void) {
    for (size_t i = 0; i < s_nLinks; i++) lm_free_link(&s_links[i]);
    pluto_free(s_links);
    s_links = NULL;
    s_nLinks = s_capLinks = 0;
    s_selected = 0;
}

void lm_clear_selection(void) { s_selected = 0; }

/* ── addLinkRect ──────────────────────────────────────────────────────── */

void lm_add_link_rect(const char* href, const char* text, LmRect rect,
                      long anchorIndex) {
    if (href == NULL || href[0] == '\0') return;

    /* Lua merge condition: same href AND same text AND
     *   (newAnchor == nil OR storedAnchor == newAnchor).
     * Stored text is never nil (insert stores text or href), so a nil
     * `text` argument can never match -> starts a new link. */
    if (s_nLinks > 0 && text != NULL) {
        LmLink* last = &s_links[s_nLinks - 1];
        int anchorOk =
            (anchorIndex < 0) ||
            (last->hasAnchor && last->anchorIndex == anchorIndex);
        if (last->href != NULL && strcmp(last->href, href) == 0 &&
            last->text != NULL && strcmp(last->text, text) == 0 &&
            anchorOk) {
            if (last->nRects == last->capRects) {
                size_t nc = last->capRects ? last->capRects * 2 : 4;
                LmRect* nr =
                    (LmRect*)pluto_realloc(last->rects, nc * sizeof(LmRect));
                if (nr == NULL) return;
                last->rects = nr;
                last->capRects = nc;
            }
            last->rects[last->nRects++] = rect;
            return;
        }
    }

    if (s_nLinks == s_capLinks) {
        size_t nc = s_capLinks ? s_capLinks * 2 : 8;
        LmLink* ns = (LmLink*)pluto_realloc(s_links, nc * sizeof(LmLink));
        if (ns == NULL) return;
        s_links = ns;
        s_capLinks = nc;
    }

    LmLink* l = &s_links[s_nLinks];
    memset(l, 0, sizeof(*l));
    l->href = pluto_strdup(href);
    l->text = pluto_strdup((text != NULL) ? text : href);
    if (l->href == NULL || l->text == NULL) {
        lm_free_link(l);
        return;
    }
    l->hasAnchor = (anchorIndex >= 0);
    if (l->hasAnchor) l->anchorIndex = anchorIndex;

    l->capRects = 4;
    l->nRects = 1;
    l->rects = (LmRect*)pluto_malloc(4 * sizeof(LmRect));
    if (l->rects == NULL) {
        lm_free_link(l);
        return;
    }
    l->rects[0] = rect;
    l->primaryRect = rect;
    s_nLinks++;
}

void lm_add_link(const char* href, int x, int y, int w, int h) {
    if (href == NULL) return;
    LmRect r = { x, y, w, h };
    lm_add_link_rect(href, NULL, r, -1);
}

void lm_add_form_input(int x, int y, int w, int h, void* inputBlock) {
    if (s_nLinks == s_capLinks) {
        size_t nc = s_capLinks ? s_capLinks * 2 : 8;
        LmLink* ns = (LmLink*)pluto_realloc(s_links, nc * sizeof(LmLink));
        if (ns == NULL) return;
        s_links = ns;
        s_capLinks = nc;
    }
    LmLink* l = &s_links[s_nLinks];
    memset(l, 0, sizeof(*l));
    /* Lua stores href=nil / text=nil for these links; hit-testing and
     * selection work purely on the rect. */
    l->isFormInput = 1;
    l->inputBlock = inputBlock;
    l->capRects = 4;
    l->nRects = 1;
    l->rects = (LmRect*)pluto_malloc(4 * sizeof(LmRect));
    if (l->rects == NULL) return;
    l->rects[0].x = x;
    l->rects[0].y = y;
    l->rects[0].w = w;
    l->rects[0].h = h;
    l->primaryRect = l->rects[0];
    s_nLinks++;
}

size_t lm_get_count(void) { return s_nLinks; }

size_t lm_selected_index(void) { return s_selected; }

LmLink* lm_get_selected_link(void) {
    if (s_selected >= 1 && s_selected <= s_nLinks)
        return &s_links[s_selected - 1];
    return NULL;
}

/* ── findInitialSelection ─────────────────────────────────────────────── */

long lm_find_initial_selection(int currentScrollY) {
    int viewTop = currentScrollY;
    int viewBottom = currentScrollY + PLUTO_CONTENT_HEIGHT;
    int viewCenter = viewTop + PLUTO_CONTENT_HEIGHT / 2;

    long bestIndex = 0;
    long bestDist = 0;
    int haveBest = 0;

    /* Pass 1: primary-rect center inside viewport */
    for (size_t i = 0; i < s_nLinks; i++) {
        LmLink* l = &s_links[i];
        int cy = l->primaryRect.y + l->primaryRect.h / 2;
        if (cy >= viewTop && cy <= viewBottom) {
            long d = cy > viewCenter ? cy - viewCenter : viewCenter - cy;
            if (!haveBest || d < bestDist) {
                bestDist = d;
                bestIndex = (long)i + 1;
                haveBest = 1;
            }
        }
    }
    if (bestIndex != 0) return bestIndex;

    /* Pass 2: nothing in view -- closest anywhere on the page */
    for (size_t i = 0; i < s_nLinks; i++) {
        LmLink* l = &s_links[i];
        int cy = l->primaryRect.y + l->primaryRect.h / 2;
        long d = cy > viewCenter ? cy - viewCenter : viewCenter - cy;
        if (!haveBest || d < bestDist) {
            bestDist = d;
            bestIndex = (long)i + 1;
            haveBest = 1;
        }
    }
    return bestIndex;
}

/* ── selectNext / selectPrev ──────────────────────────────────────────── */

LmLink* lm_select_next(int currentScrollY) {
    if (s_nLinks == 0) {
        s_selected = 0;
        return NULL;
    }
    if (s_selected == 0) {
        long idx = lm_find_initial_selection(currentScrollY);
        if (idx != 0) {
            s_selected = (size_t)idx;
            return &s_links[s_selected - 1];
        }
        s_selected = 0;
        return NULL;
    }
    s_selected++;
    if (s_selected > s_nLinks) s_selected = 1;
    return &s_links[s_selected - 1];
}

LmLink* lm_select_prev(int currentScrollY) {
    if (s_nLinks == 0) {
        s_selected = 0;
        return NULL;
    }
    if (s_selected == 0) {
        long idx = lm_find_initial_selection(currentScrollY);
        if (idx != 0) {
            s_selected = (size_t)idx;
            return &s_links[s_selected - 1];
        }
        s_selected = 0;
        return NULL;
    }
    s_selected--;
    if (s_selected < 1) s_selected = s_nLinks;
    return &s_links[s_selected - 1];
}

/* ── drawSelectedHighlight ────────────────────────────────────────────── */

void lm_draw_selected_highlight(int scrollY) {
    LmLink* link = lm_get_selected_link();
    if (link == NULL) return;
#ifndef PLUTO_LM_PD
    (void)scrollY;
#else
    if (s_pd == NULL) return;
    s_pd->graphics->setClipRect(0, PLUTO_CONTENT_Y,
                                PLUTO_CONTENT_WIDTH, PLUTO_CONTENT_HEIGHT);
    for (size_t i = 0; i < link->nRects; i++) {
        LmRect* r = &link->rects[i];
        int drawY = r->y - scrollY;
        if (drawY + r->h >= PLUTO_CONTENT_Y && drawY <= PLUTO_SCREEN_HEIGHT) {
            /* Lua: setColor(black); drawRoundRect(x,y,w,h,3) ->
             * C API carries color/lineWidth explicitly */
            s_pd->graphics->drawRoundRect(r->x - 2, drawY - 1,
                                          r->w + 4, r->h + 2, 3,
                                          1, kColorBlack);
            s_pd->graphics->fillRect(r->x - 4, drawY + r->h / 2 - 2,
                                     2, 5, kColorBlack);
        }
    }
    s_pd->graphics->clearClipRect();
#endif
}

/* ── isHighlighted / getHoveredLink ───────────────────────────────────── */

int lm_is_highlighted(const char* href, int x, int y) {
    LmLink* sel = lm_get_selected_link();
    if (sel == NULL || href == NULL) return 0;
    if (sel->href != NULL && strcmp(sel->href, href) == 0) {
        for (size_t i = 0; i < sel->nRects; i++) {
            if (sel->rects[i].x == x && sel->rects[i].y == y) return 1;
        }
    }
    return 0;
}

LmLink* lm_get_hovered_link(int screenX, int pageY) {
    for (size_t i = 0; i < s_nLinks; i++) {
        LmLink* link = &s_links[i];
        for (size_t k = 0; k < link->nRects; k++) {
            LmRect* r = &link->rects[k];
            int w = (r->w >= 0) ? r->w : 60;
            int h = (r->h >= 0) ? r->h : 14;
            if (screenX >= r->x && screenX <= r->x + w &&
                pageY >= r->y && pageY <= r->y + h)
                return link;
        }
    }
    return NULL;
}
