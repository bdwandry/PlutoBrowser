/*
 * PlutoBrowser — history_page.c
 * Browsing history view (port of Source/ui/history_page.lua).
 * Same list mechanics as bookmarks_page but backed by storage history
 * (title 34-char / URL 46-char truncation, empty-state message).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui/history_page.h"
#include "core/constants.h"
#include "core/storage.h"
#include "render/style.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

#define BTN_UP (1 << 2)
#define BTN_DOWN (1 << 3)
#define BTN_A (1 << 5)
#define BTN_B (1 << 4)

static int g_selectedIndex = 1;
static float g_scrollY = 0;

void history_page_open(void)
{
    g_selectedIndex = 1;
    g_scrollY = 0;
}

int history_page_selected_index(void)
{
    return g_selectedIndex;
}

static char *dup_url(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = (char *)pluto_pd()->system->realloc(NULL, n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

char *history_page_handle_input(unsigned int pushed)
{
    int count = storage_history_count();
    if (count == 0)
    {
        if (pushed & BTN_B)
        {
            return dup_url("close");
        }
        return NULL;
    }

    if (pushed & BTN_DOWN)
    {
        g_selectedIndex = (g_selectedIndex + 1 <= count) ? g_selectedIndex + 1 : count;
    }
    else if (pushed & BTN_UP)
    {
        g_selectedIndex = (g_selectedIndex - 1 >= 1) ? g_selectedIndex - 1 : 1;
    }

    if (pushed & BTN_A)
    {
        const StoredHistoryItem *sel = storage_history_at(g_selectedIndex - 1);
        if (sel)
        {
            return dup_url(sel->url);
        }
    }
    else if (pushed & BTN_B)
    {
        return dup_url("close");
    }

    return NULL;
}

void history_page_draw(float crankChange)
{
    PlaydateAPI *pd = pluto_pd();
    LCDFont *fontH = style_font(PLUTO_FONT_HEADING2);
    LCDFont *fontB = style_font(PLUTO_FONT_BODY_BOLD);
    LCDFont *fontS = style_font(PLUTO_FONT_SMALL);

    if (crankChange != 0.0f)
    {
        g_scrollY += crankChange * 2.0f;
        if (g_scrollY < 0)
        {
            g_scrollY = 0;
        }
    }

    int startY = CONTENT_Y + 10 - (int)g_scrollY;

    pd->graphics->setFont(fontH);
    const char *title = "BROWSING HISTORY";
    pd->graphics->drawText(title, strlen(title), kUTF8Encoding, 16, startY);
    pd->graphics->drawLine(16, startY + 18, SCREEN_WIDTH - 16, startY + 18, 1,
                           kColorBlack);

    int itemY = startY + 26;
    int itemH = 34;
    int count = storage_history_count();

    if (count == 0)
    {
        pd->graphics->setFont(fontB);
        const char *empty = "No browsing history recorded yet.";
        pd->graphics->drawText(empty, strlen(empty), kUTF8Encoding, 16, itemY);
        return;
    }

    for (int i = 1; i <= count; i++)
    {
        const StoredHistoryItem *h = storage_history_at(i - 1);
        if (!h)
        {
            continue;
        }
        int isSel = (i == g_selectedIndex);
        int drawY = itemY + (i - 1) * (itemH + 4);

        if (drawY + itemH >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
        {
            if (isSel)
            {
                pd->graphics->fillRoundRect(16, drawY, SCREEN_WIDTH - 32, itemH,
                                            4, kColorBlack);
                pd->graphics->setDrawMode(kDrawModeFillWhite);
            }
            else
            {
                pd->graphics->fillRoundRect(16, drawY, SCREEN_WIDTH - 32, itemH,
                                            4, kColorWhite);
                pd->graphics->drawRoundRect(16, drawY, SCREEN_WIDTH - 32, itemH,
                                            4, 1, kColorBlack);
                pd->graphics->setDrawMode(kDrawModeCopy);
            }

            pd->graphics->setFont(fontB);
            const char *t = h->title ? h->title : h->url;
            char titleBuf[48];
            if (strlen(t) > 34)
            {
                snprintf(titleBuf, sizeof(titleBuf), "%.31s...", t);
            }
            else
            {
                snprintf(titleBuf, sizeof(titleBuf), "%s", t);
            }
            pd->graphics->drawText(titleBuf, strlen(titleBuf), kUTF8Encoding, 24,
                                   drawY + 3);

            pd->graphics->setFont(fontS);
            const char *u = h->url ? h->url : "";
            char urlBuf[64];
            if (strlen(u) > 46)
            {
                snprintf(urlBuf, sizeof(urlBuf), "%.43s...", u);
            }
            else
            {
                snprintf(urlBuf, sizeof(urlBuf), "%s", u);
            }
            pd->graphics->drawText(urlBuf, strlen(urlBuf), kUTF8Encoding, 24,
                                   drawY + 18);

            pd->graphics->setDrawMode(kDrawModeCopy);
        }
    }
}
