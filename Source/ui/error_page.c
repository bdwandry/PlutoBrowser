/*
 * PlutoBrowser — error_page.c
 * Error page display (port of Source/ui/error_page.lua). See error_page.h.
 * Behavior preserved: selection clamped 1..3, left/up decrement, right/down
 * increment, A activates retry/search/home; draw draws the rounded panel,
 * truncated message (48) and URL (50), and three 100x28 buttons with the
 * selected one filled black/inverted.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui/error_page.h"
#include "core/constants.h"
#include "render/style.h"
#include "pd_api.h"
#include "../core/pluto_mem.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

/* PDButtons enum (pd_api_sys.h): Left=1<<0 Right=1<<1 Up=1<<2 Down=1<<3
 * B=1<<4 A=1<<5. */
#define BTN_LEFT (1 << 0)
#define BTN_RIGHT (1 << 1)
#define BTN_UP (1 << 2)
#define BTN_DOWN (1 << 3)
#define BTN_A (1 << 5)

static int g_selectedIndex = 1;
static char g_errorMsg[128] = "Unable to load webpage";
static char g_failedUrl[256] = "";

void error_page_show(const char *errorMsg, const char *failedUrl)
{
    snprintf(g_errorMsg, sizeof(g_errorMsg), "%s",
             errorMsg && errorMsg[0] ? errorMsg : "Unable to load webpage");
    snprintf(g_failedUrl, sizeof(g_failedUrl), "%s",
             failedUrl ? failedUrl : "");
    g_selectedIndex = 1;
}

int error_page_selected_index(void)
{
    return g_selectedIndex;
}

const char *error_page_message(void)
{
    return g_errorMsg;
}

const char *error_page_failed_url(void)
{
    return g_failedUrl;
}

char *error_page_handle_input(unsigned int current, unsigned int pushed,
                              unsigned int released)
{
    (void)current;
    (void)released;

    if ((pushed & BTN_LEFT) || (pushed & BTN_UP))
    {
        if (g_selectedIndex > 1)
        {
            g_selectedIndex--;
        }
    }
    else if ((pushed & BTN_RIGHT) || (pushed & BTN_DOWN))
    {
        if (g_selectedIndex < 3)
        {
            g_selectedIndex++;
        }
    }

    if (pushed & BTN_A)
    {
        const char *action = g_selectedIndex == 1    ? "retry"
                             : g_selectedIndex == 2  ? "search"
                                                     : "home";
        size_t n = strlen(action) + 1;
        char *out = (char *)pluto_mem_realloc(NULL, n);
        if (out)
        {
            memcpy(out, action, n);
        }
        return out;
    }
    return NULL;
}

void error_page_draw(void)
{
    PlaydateAPI *pd = pluto_pd();
    LCDFont *fontH = style_font(PLUTO_FONT_HEADING1);
    LCDFont *fontB = style_font(PLUTO_FONT_BODY);
    LCDFont *fontBold = style_font(PLUTO_FONT_BODY_BOLD);
    LCDFont *fontSmall = style_font(PLUTO_FONT_SMALL);

    int startY = CONTENT_Y + 16;

    pd->graphics->drawRoundRect(20, startY, SCREEN_WIDTH - 40, 120, 6, 1,
                                kColorBlack);

    pd->graphics->setFont(fontH);
    const char *title = "Connection Failed";
    pd->graphics->drawText(title, strlen(title), kUTF8Encoding, 36, startY + 12);

    pd->graphics->setFont(fontBold);
    char msg[64];
    if (strlen(g_errorMsg) > 48)
    {
        snprintf(msg, sizeof(msg), "%.45s...", g_errorMsg);
    }
    else
    {
        snprintf(msg, sizeof(msg), "%s", g_errorMsg);
    }
    pd->graphics->drawText(msg, strlen(msg), kUTF8Encoding, 36, startY + 40);

    pd->graphics->setFont(fontSmall);
    char u[64];
    if (strlen(g_failedUrl) > 50)
    {
        snprintf(u, sizeof(u), "Target: %.47s...", g_failedUrl);
    }
    else
    {
        snprintf(u, sizeof(u), "Target: %s", g_failedUrl);
    }
    pd->graphics->drawText(u, strlen(u), kUTF8Encoding, 36, startY + 60);

    pd->graphics->setFont(fontB);
    const char *hint = "Check your Wi-Fi or try searching the web.";
    pd->graphics->drawText(hint, strlen(hint), kUTF8Encoding, 36, startY + 82);

    static const char *const buttons[3] = {"Try Again", "Search Web", "Go Home"};
    int btnW = 100;
    int btnH = 28;
    int btnY = startY + 136;
    int startX = 36;

    for (int i = 0; i < 3; i++)
    {
        int btnX = startX + i * (btnW + 16);
        int isSel = ((i + 1) == g_selectedIndex);

        if (isSel)
        {
            pd->graphics->fillRoundRect(btnX, btnY, btnW, btnH, 4, kColorBlack);
            pd->graphics->drawRoundRect(btnX + 1, btnY + 1, btnW - 2, btnH - 2, 3,
                                        1, kColorWhite);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        else
        {
            pd->graphics->fillRoundRect(btnX, btnY, btnW, btnH, 4, kColorWhite);
            pd->graphics->drawRoundRect(btnX, btnY, btnW, btnH, 4, 1, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeCopy);
        }

        pd->graphics->setFont(fontBold);
        int tw = style_get_text_width(PLUTO_FONT_BODY_BOLD, buttons[i]);
        pd->graphics->drawText(buttons[i], strlen(buttons[i]), kUTF8Encoding,
                               btnX + (btnW - tw) / 2, btnY + 6);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
}
