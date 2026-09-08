/*
 * PlutoBrowser — link_manager.c
 * Port of Source/render/link_manager.lua (reference, 181 lines).
 * See link_manager.h for the Lua→C function map and preserved quirks.
 */
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"
#include "core/constants.h"
#include "render/link_manager.h"

/* SDK API pointer (pluto_pd is defined in main.c; same allocator family). */
extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

static LMLink *g_links = NULL;
static int g_linkCount = 0;
static int g_linkCap = 0;
static int g_selectedIndex = 0; /* 0 = none (Lua nil) */

static void *lm_alloc(size_t n)
{
    PlaydateAPI *pd = pluto_pd();
    return pd ? pd->system->realloc(NULL, n) : malloc(n);
}

static void *lm_realloc(void *ptr, size_t n)
{
    PlaydateAPI *pd = pluto_pd();
    return pd ? pd->system->realloc(ptr, n) : realloc(ptr, n);
}

static char *lm_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)lm_alloc(n);
    if (d)
    {
        memcpy(d, s, n);
    }
    return d;
}

void lm_clear(void)
{
    for (int i = 0; i < g_linkCount; i++)
    {
        pluto_free(g_links[i].href);
        pluto_free(g_links[i].text);
        pluto_free(g_links[i].rects);
        pluto_free(g_links[i].aux);
    }
    g_linkCount = 0;
    g_selectedIndex = 0;
}

void lm_clear_selection(void)
{
    g_selectedIndex = 0;
}

void lm_add_link_rect(const char *href, const char *text, const LMRect *rect, int anchorIndex)
{
    lm_add_link_rect_ex(href, text, rect, anchorIndex, NULL);
}

void lm_add_link_rect_ex(const char *href, const char *text, const LMRect *rect,
                         int anchorIndex, const LMRectAux *aux)
{
    if (!href || href[0] == '\0' || !rect)
    {
        return; /* Lua: `if not href or href == "" or not rect then return end` */
    }

    if (g_linkCount > 0)
    {
        LMLink *last = &g_links[g_linkCount - 1];
        int anchorMatches = (anchorIndex == 0) || (last->anchorIndex == anchorIndex);
        /* Lua quirk: text==nil never merges (nil ~= string comparison). */
        int textMatches = (text != NULL) && last->text && strcmp(last->text, text) == 0;
        if (last->href && strcmp(last->href, href) == 0 && textMatches && anchorMatches)
        {
            if (last->rectCount == last->rectCap)
            {
                int newCap = last->rectCap > 0 ? last->rectCap * 2 : 4;
                LMRect *grown = (LMRect *)lm_realloc(last->rects,
                                                 (size_t)newCap * sizeof(LMRect));
                if (!grown)
                {
                    return; /* allocation failure: drop the rect, keep state */
                }
                last->rects = grown;
                last->rectCap = newCap;
            }
            last->rects[last->rectCount++] = *rect;
            if (last->rectCount > last->auxCap)
            {
                int newAuxCap = last->auxCap > 0 ? last->auxCap * 2 : 4;
                LMRectAux *ga = (LMRectAux *)lm_realloc(last->aux,
                                                    (size_t)newAuxCap * sizeof(LMRectAux));
                if (ga)
                {
                    last->aux = ga;
                    last->auxCap = newAuxCap;
                }
            }
            if (last->aux && last->rectCount <= last->auxCap)
            {
                int ai = last->rectCount - 1;
                if (aux)
                {
                    last->aux[ai] = *aux;
                }
                else
                {
                    memset(&last->aux[ai], 0, sizeof(LMRectAux));
                }
            }
            return;
        }
    }

    if (g_linkCount == g_linkCap)
    {
        int newCap = g_linkCap > 0 ? g_linkCap * 2 : 16;
        LMLink *grown = (LMLink *)lm_realloc(g_links,
            (size_t)newCap * sizeof(LMLink));
        if (!grown)
        {
            return;
        }
        g_links = grown;
        g_linkCap = newCap;
    }

    LMLink *l = &g_links[g_linkCount];
    memset(l, 0, sizeof(*l));
    l->index = g_linkCount + 1; /* Lua 1-based */
    l->href = lm_strdup(href);
    l->text = lm_strdup(text ? text : href); /* Lua: text or href */
    l->anchorIndex = anchorIndex;
    l->rectCap = 4;
    l->rects = (LMRect *)lm_alloc((size_t)l->rectCap * sizeof(LMRect));
    if (!l->rects || !l->href || !l->text)
    {
        /* Mirror Lua behavior: without a rect array nothing can be stored. */
        pluto_free(l->href);
        pluto_free(l->text);
        pluto_free(l->rects);
        return;
    }
    l->rects[0] = *rect;
    l->rectCount = 1;
    l->primaryRect = *rect;
    l->auxCap = 4;
    l->aux = (LMRectAux *)lm_alloc((size_t)l->auxCap * sizeof(LMRectAux));
    if (l->aux)
    {
        if (aux)
        {
            l->aux[0] = *aux;
        }
        else
        {
            memset(&l->aux[0], 0, sizeof(LMRectAux));
        }
    }
    g_linkCount++;
}

const LMRectAux *lm_rect_aux(int linkIndex, int rectIndex)
{
    if (linkIndex < 1 || linkIndex > g_linkCount)
    {
        return NULL;
    }
    LMLink *l = &g_links[linkIndex - 1];
    if (rectIndex < 0 || rectIndex >= l->rectCount || !l->aux)
    {
        return NULL;
    }
    return &l->aux[rectIndex];
}

int lm_get_count(void)
{
    return g_linkCount;
}

int lm_get_selected_index(void)
{
    return g_selectedIndex;
}

const LMLink *lm_get_selected_link(void)
{
    /* Lua: selectedIndex and selectedIndex >= 1 and <= #links */
    if (g_selectedIndex >= 1 && g_selectedIndex <= g_linkCount)
    {
        return &g_links[g_selectedIndex - 1];
    }
    return NULL;
}

/* (local) findInitialSelection — two passes: in-view center, then nearest. */
static int lm_find_initial_selection(int currentScrollY)
{
    int viewTop = currentScrollY;
    int viewBottom = currentScrollY + CONTENT_HEIGHT;
    int viewCenter = viewTop + CONTENT_HEIGHT / 2;

    int bestIndex = 0;
    int bestDist = 0; /* 0 = unset (Lua math.huge sentinel) */

    /* Pass 1: links whose primary center is inside the viewport. */
    for (int i = 0; i < g_linkCount; i++)
    {
        const LMLink *l = &g_links[i];
        int cy = l->primaryRect.y + l->primaryRect.h / 2; /* Lua (h or 0)/2 */
        if (cy >= viewTop && cy <= viewBottom)
        {
            int d = cy >= viewCenter ? cy - viewCenter : viewCenter - cy;
            if (bestIndex == 0 || d < bestDist)
            {
                bestDist = d;
                bestIndex = i + 1; /* Lua 1-based */
            }
        }
    }
    if (bestIndex)
    {
        return bestIndex;
    }

    /* Pass 2: nothing in view — closest link anywhere on the page. */
    for (int i = 0; i < g_linkCount; i++)
    {
        const LMLink *l = &g_links[i];
        int cy = l->primaryRect.y + l->primaryRect.h / 2;
        int d = cy >= viewCenter ? cy - viewCenter : viewCenter - cy;
        if (bestIndex == 0 || d < bestDist)
        {
            bestDist = d;
            bestIndex = i + 1;
        }
    }
    return bestIndex;
}

const LMLink *lm_select_next(int currentScrollY)
{
    if (g_linkCount == 0)
    {
        g_selectedIndex = 0;
        return NULL;
    }

    if (g_selectedIndex == 0)
    {
        int idx = lm_find_initial_selection(currentScrollY);
        if (idx)
        {
            g_selectedIndex = idx;
            return &g_links[idx - 1];
        }
        g_selectedIndex = 0;
        return NULL;
    }

    g_selectedIndex++;
    if (g_selectedIndex > g_linkCount)
    {
        g_selectedIndex = 1;
    }
    return &g_links[g_selectedIndex - 1];
}

const LMLink *lm_select_prev(int currentScrollY)
{
    if (g_linkCount == 0)
    {
        g_selectedIndex = 0;
        return NULL;
    }

    if (g_selectedIndex == 0)
    {
        int idx = lm_find_initial_selection(currentScrollY);
        if (idx)
        {
            g_selectedIndex = idx;
            return &g_links[idx - 1];
        }
        g_selectedIndex = 0;
        return NULL;
    }

    g_selectedIndex--;
    if (g_selectedIndex < 1)
    {
        g_selectedIndex = g_linkCount;
    }
    return &g_links[g_selectedIndex - 1];
}

void lm_draw_selected_highlight(int scrollY)
{
    const LMLink *link = lm_get_selected_link();
    if (!link)
    {
        return;
    }

    PlaydateAPI *pd = pluto_pd();
    if (!pd)
    {
        return;
    }

    pd->graphics->setClipRect(0, CONTENT_Y, CONTENT_WIDTH, CONTENT_HEIGHT);

    for (int i = 0; i < link->rectCount; i++)
    {
        const LMRect *r = &link->rects[i];
        int drawY = r->y - scrollY;
        if (drawY + r->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
        {
            pd->graphics->drawRoundRect(r->x - 2, drawY - 1, r->w + 4, r->h + 2, 3, 1,
                                        kColorBlack);
            /* Lua gfx.fillRect(r.x - 4, drawY + floor(h/2) - 2, 2, 5) —
             * gfx.fillRect fills (w+1)x(h+1) pixels; the SDK C fillRect
             * matches that same inclusive convention. */
            pd->graphics->fillRect(r->x - 4, drawY + r->h / 2 - 2, 2, 5, kColorBlack);
        }
    }

    pd->graphics->clearClipRect();
}

bool lm_is_highlighted(const char *href, int x, int y)
{
    const LMLink *sel = lm_get_selected_link();
    if (!sel || !href)
    {
        return false;
    }
    if (sel->href && strcmp(sel->href, href) == 0)
    {
        for (int i = 0; i < sel->rectCount; i++)
        {
            if (sel->rects[i].x == x && sel->rects[i].y == y)
            {
                return true;
            }
        }
    }
    return false;
}

void lm_add_link(const char *href, int x, int y, int w, int h)
{
    if (!href)
    {
        return; /* Lua: if not link or not link.href then return end */
    }
    LMRect r = { x, y, w, h };
    lm_add_link_rect(href, href, &r, 0);
}

const LMLink *lm_get_hovered_link(int screenX, int pageY)
{
    for (int i = 0; i < g_linkCount; i++)
    {
        const LMLink *link = &g_links[i];
        for (int j = 0; j < link->rectCount; j++)
        {
            const LMRect *r = &link->rects[j];
            /* Lua `(r.w or 60)` fires only on nil; C rects always carry
             * numeric w/h, so raw values are the faithful mapping (a 0
             * width hit-tests with width 0, exactly as Lua would). */
            if (screenX >= r->x && screenX <= r->x + r->w &&
                pageY >= r->y && pageY <= r->y + r->h)
            {
                return link;
            }
        }
    }
    return NULL;
}

const LMLink *lm_link_at(int linkIndex)
{
    if (linkIndex < 1 || linkIndex > g_linkCount)
    {
        return NULL;
    }
    return &g_links[linkIndex - 1];
}

int lm_get_link_count(void)
{
    return g_linkCount;
}
