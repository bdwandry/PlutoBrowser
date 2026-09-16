/*
 * PlutoBrowser — settings_page.c
 * Settings menu overlay (port of Source/ui/settings_page.lua).
 * Staged-changes semantics preserved exactly: open() snapshots storage into
 * the staged copy; left/right adjust ONLY the staged copy; A saves (applies
 * staged → storage, fires onChange) unless the row is the Clear Cookies
 * action (executes immediately); B discards. Animation: 300ms ease-out cubic
 * box scale from center, contents clipped, drawn only past t>0.4.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui/settings_page.h"
#include "core/constants.h"
#include "core/storage.h"
#include "core/logger.h"
#include "render/style.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

#define BTN_UP (1 << 2)
#define BTN_DOWN (1 << 3)
#define BTN_LEFT (1 << 0)
#define BTN_RIGHT (1 << 1)
#define BTN_A (1 << 5)
#define BTN_B (1 << 4)

#define ANIM_DURATION_MS 300
#define BORDER 10
#define BOX_RADIUS 10
#define BOX_X (BORDER)
#define BOX_Y (BORDER + 10)
#define BOX_W (SCREEN_WIDTH - BORDER * 2)
#define BOX_H (SCREEN_HEIGHT - BORDER - BOX_Y)
#define CENTER_X (SCREEN_WIDTH / 2)
#define CENTER_Y (BOX_Y + BOX_H / 2)

#define OPTION_COUNT 8

/* ── Scrolling list geometry ──────────────────────────────────────────────
 * The panel is fixed-size; the row list scrolls under it as the browser
 * grows more settings. Rows live between the title (24px) and the footer
 * (18px); SETTINGS_ROW_PITCH is the per-row stride. */
#define SETTINGS_TITLE_H 24
#define SETTINGS_FOOTER_H 18
#define SETTINGS_ITEM_H 26
#define SETTINGS_ROW_PITCH (SETTINGS_ITEM_H + 2)

static int g_isOpen = 0;
static int g_selectedIndex = 1;
static int g_scrollOffset = 0;   /* px scrolled down from the top of the list */
static float g_crankAccum = 0.0f; /* accumulated crank degrees → row steps */
static int g_previousState = 0;
static unsigned int g_animStartMs = 0;
static void (*g_onChangeCallback)(void) = NULL;
static void (*g_clearCookiesCb)(void) = NULL;

/* staged copy (mirrors the Lua `staged` table) */
static struct
{
    int searchEngine;             /* 1..SEARCH_ENGINE_COUNT */
    int mode;                     /* BrowseMode */
    int invertCrank;              /* 0/1 */
    char imageMode[16];           /* persisted name */
    int showFps;                  /* 0/1 — FPS overlay */
    int displayFps;               /* display refresh target: 30 or 50 fps */
    int jsEnabled;                /* 0/1 — JavaScript execution (muJS) */
} g_staged;

void settings_page_set_onchange_callback(void (*fn)(void));

/* Row labels (optionIndex 1..OPTION_COUNT). Single source of truth for both
 * the renderer and settings_page_label(). */
static const char *const k_settingsLabels[OPTION_COUNT] = {
    "Search Engine", "Browse Mode", "Invert Crank", "Image Mode",
    "Display FPS", "Show FPS", "Javascript Execution", "Clear Cookies"};

const char *settings_page_label(int optionIndex)
{
    if (optionIndex < 1 || optionIndex > OPTION_COUNT)
    {
        return "";
    }
    return k_settingsLabels[optionIndex - 1];
}

void settings_page_set_onchange_callback(void (*fn)(void))
{
    g_onChangeCallback = fn;
}

int settings_page_is_open(void)
{
    return g_isOpen;
}

int settings_page_selected_index(void)
{
    return g_selectedIndex;
}

/* ── Scroll geometry helpers ────────────────────────────────────────────── */

/* Height available for the row list inside the fixed panel. */
static int settings_list_height(void)
{
    return BOX_H - 20 /* panel inner margins */ - SETTINGS_TITLE_H -
           SETTINGS_FOOTER_H;
}

/* Max scroll offset: the bottom of the LAST row must sit at the list bottom.
 * Both edges are in content space (measured from the panel's inner top). */
static int settings_max_scroll(void)
{
    int contentBottom = SETTINGS_TITLE_H +
                        (OPTION_COUNT - 1) * SETTINGS_ROW_PITCH +
                        SETTINGS_ITEM_H;
    int windowBottom = SETTINGS_TITLE_H + settings_list_height();
    return contentBottom > windowBottom ? contentBottom - windowBottom : 0;
}

/* Clamp the scroll so the SELECTED row is always fully visible (keeps D-pad
 * and crank scrolling consistent). */
static int settings_clamped_scroll(void)
{
    int maxScroll = settings_max_scroll();
    int scroll = g_scrollOffset;
    if (scroll > maxScroll)
    {
        scroll = maxScroll;
    }
    if (scroll < 0)
    {
        scroll = 0;
    }
    /* keep the selected row fully on-screen */
    int selTop = SETTINGS_TITLE_H + (g_selectedIndex - 1) * SETTINGS_ROW_PITCH;
    int selBottom = selTop + SETTINGS_ITEM_H;
    int listBottom = SETTINGS_TITLE_H + settings_list_height();
    int minScroll = selBottom - listBottom;
    int maxForSel = selTop - SETTINGS_TITLE_H;
    if (minScroll > maxForSel)
    {
        minScroll = maxForSel;
    }
    if (scroll < minScroll)
    {
        scroll = minScroll;
    }
    if (scroll > maxForSel)
    {
        scroll = maxForSel;
    }
    return scroll;
}

void settings_page_apply_crank(float crankChange)
{
    if (!g_isOpen || crankChange == 0.0f)
    {
        return;
    }
    /* ~18deg of crank = one row, with sub-row remainder carried over. */
    const float degPerRow = 18.0f;
    g_crankAccum += crankChange;
    int rows = (int)(g_crankAccum / degPerRow);
    if (rows != 0)
    {
        g_crankAccum -= (float)rows * degPerRow;
    }
    while (rows > 0)
    {
        if (g_selectedIndex < OPTION_COUNT)
        {
            g_selectedIndex++;
        }
        rows--;
    }
    while (rows < 0)
    {
        if (g_selectedIndex > 1)
        {
            g_selectedIndex--;
        }
        rows++;
    }
}

int settings_page_previous_state(void)
{
    return g_previousState;
}

void settings_page_open(int prevState)
{
    g_isOpen = 1;
    g_selectedIndex = 1;
    g_scrollOffset = 0;
    g_crankAccum = 0.0f;
    g_previousState = prevState;
    g_animStartMs = pluto_pd()->system->getCurrentTimeMilliseconds();

    g_staged.searchEngine = storage_setting_int("searchEngine");
    g_staged.mode = storage_setting_int("mode");
    g_staged.invertCrank = storage_setting_int("invertCrank");
    const char *im = storage_setting_str("imageMode");
    snprintf(g_staged.imageMode, sizeof(g_staged.imageMode), "%s",
             im ? im : IMAGE_MODE_NAMES[IMAGE_MODE_VIEWPORT]);
    g_staged.showFps = storage_setting_int("showFps");
    g_staged.displayFps = storage_setting_int("displayFps");
    if (g_staged.displayFps != 50)
    {
        g_staged.displayFps = 30; /* only 30 or 50 are valid (Playdate max = 50) */
    }
    g_staged.jsEnabled = storage_setting_int("jsEnabled");
    if (g_staged.jsEnabled < 0 || g_staged.jsEnabled > 2)
    {
        g_staged.jsEnabled = 1; /* Off/Inline/Full — out-of-range → Inline */
    }

    /* Settings-panel audit line: logs the exact on-screen label + staged
     * value of the JavaScript row so simulator/device pluto.log runs can
     * verify the row set without human eyes on the panel. */
    logger_log("SETTINGS: open label7='%s' staged7='%s'",
               settings_page_label(7), settings_page_staged_value(7));
}

void settings_page_close(void)
{
    g_isOpen = 0;
}

static void save_and_close(char **out)
{
    storage_set_setting_int("searchEngine", g_staged.searchEngine);
    storage_set_setting_int("mode", g_staged.mode);
    storage_set_setting_int("invertCrank", g_staged.invertCrank);
    storage_set_setting_str("imageMode", g_staged.imageMode);
    storage_set_setting_int("showFps", g_staged.showFps);
    storage_set_setting_int("displayFps", g_staged.displayFps);
    storage_set_setting_int("jsEnabled", g_staged.jsEnabled);
    storage_save();
    if (g_onChangeCallback)
    {
        g_onChangeCallback();
    }
    settings_page_close();
    if (out)
    {
        *out = (char *)pluto_pd()->system->realloc(NULL, 6);
        if (*out)
        {
            strcpy(*out, "save");
        }
    }
}

static void cancel_and_close(char **out)
{
    settings_page_close();
    if (out)
    {
        *out = (char *)pluto_pd()->system->realloc(NULL, 7);
        if (*out)
        {
            strcpy(*out, "close");
        }
    }
}

const char *settings_page_staged_value(int optionIndex)
{
    static char buf[40];
    switch (optionIndex)
    {
    case 1:
    {
        int idx = g_staged.searchEngine - 1;
        if (idx < 0 || idx >= SEARCH_ENGINE_COUNT)
        {
            idx = 0;
        }
        return SEARCH_ENGINES[idx].name;
    }
    case 2:
        return g_staged.mode == 1 ? "HTML" : "Reader"; /* MODE_RAW_HTML=1 */
    case 3:
        return g_staged.invertCrank ? "On" : "Off";
    case 4:
    {
        int n = image_mode_from_name(g_staged.imageMode);
        return image_mode_label((ImageMode)(n >= 0 ? n : IMAGE_MODE_ALL));
    }
    case 5:
        return g_staged.displayFps == 50 ? "50" : "30";
    case 6:
        return g_staged.showFps ? "On" : "Off";
    case 7: /* JavaScript execution: Off / Inline / Full (0/1/2) */
        return g_staged.jsEnabled == 2   ? "Full"
               : g_staged.jsEnabled == 1 ? "Inline"
                                         : "Off";
    case 8:
        return "";
    default:
        return "";
    }
    (void)buf;
}

char *settings_page_handle_input(unsigned int pushed, void (*clearCookiesCb)(void))
{
    if (!g_isOpen)
    {
        return NULL;
    }
    g_clearCookiesCb = clearCookiesCb;

    char *result = NULL;

    if (pushed & BTN_DOWN)
    {
        if (g_selectedIndex < OPTION_COUNT)
        {
            g_selectedIndex++;
        }
    }
    else if (pushed & BTN_UP)
    {
        if (g_selectedIndex > 1)
        {
            g_selectedIndex--;
        }
    }
    else if (pushed & BTN_LEFT)
    {
        switch (g_selectedIndex)
        {
        case 1:
        {
            int n = SEARCH_ENGINE_COUNT;
            g_staged.searchEngine = ((g_staged.searchEngine - 2 + n) % n) + 1;
            break;
        }
        case 2:
            g_staged.mode = (g_staged.mode == 1) ? 0 : 1; /* RAW_HTML <-> READER */
            break;
        case 3:
            g_staged.invertCrank = !g_staged.invertCrank;
            break;
        case 4:
        {
            int cur = image_mode_from_name(g_staged.imageMode);
            if (cur < 0)
            {
                cur = IMAGE_MODE_ALL;
            }
            int idx = ((cur - 2 + IMAGE_MODE_COUNT) % IMAGE_MODE_COUNT);
            snprintf(g_staged.imageMode, sizeof(g_staged.imageMode), "%s",
                     IMAGE_MODE_NAMES[idx]);
            break;
        }
        case 5:
            g_staged.displayFps = (g_staged.displayFps == 50) ? 30 : 50;
            break;
        case 6:
            g_staged.showFps = !g_staged.showFps;
            break;
        case 7: /* Off → Full → Inline → Off (left decrements) */
            g_staged.jsEnabled = (g_staged.jsEnabled + 2) % 3;
            break;
        case 8:
            if (g_clearCookiesCb)
            {
                g_clearCookiesCb();
            }
            break;
        default:
            break;
        }
    }
    else if (pushed & BTN_RIGHT)
    {
        switch (g_selectedIndex)
        {
        case 1:
        {
            int n = SEARCH_ENGINE_COUNT;
            g_staged.searchEngine = (g_staged.searchEngine % n) + 1;
            break;
        }
        case 2:
            g_staged.mode = (g_staged.mode == 1) ? 0 : 1;
            break;
        case 3:
            g_staged.invertCrank = !g_staged.invertCrank;
            break;
        case 4:
        {
            int cur = image_mode_from_name(g_staged.imageMode);
            if (cur < 0)
            {
                cur = IMAGE_MODE_ALL;
            }
            int idx = (cur % IMAGE_MODE_COUNT) + 1;
            if (idx >= IMAGE_MODE_COUNT)
            {
                idx = 0; /* (idx % n) + 1 wraps to 1-based; names are 0-based */
            }
            snprintf(g_staged.imageMode, sizeof(g_staged.imageMode), "%s",
                     IMAGE_MODE_NAMES[idx]);
            break;
        }
        case 5:
            g_staged.displayFps = (g_staged.displayFps == 50) ? 30 : 50;
            break;
        case 6:
            g_staged.showFps = !g_staged.showFps;
            break;
        case 7: /* Off → Inline → Full → Off (right increments) */
            g_staged.jsEnabled = (g_staged.jsEnabled + 1) % 3;
            break;
        case 8:
            if (g_clearCookiesCb)
            {
                g_clearCookiesCb();
            }
            break;
        default:
            break;
        }
    }
    else if (pushed & BTN_A)
    {
        if (g_selectedIndex == 8)
        {
            /* Clear Cookies action: execute immediately */
            if (g_clearCookiesCb)
            {
                g_clearCookiesCb();
            }
        }
        else
        {
            save_and_close(&result);
        }
    }
    else if (pushed & BTN_B)
    {
        cancel_and_close(&result);
    }

    return result;
}

void settings_page_draw(void)
{
    if (!g_isOpen)
    {
        return;
    }
    PlaydateAPI *pd = pluto_pd();

    float elapsed = (float)(pd->system->getCurrentTimeMilliseconds() - g_animStartMs);
    float t = elapsed / (float)ANIM_DURATION_MS;
    if (t > 1.0f)
    {
        t = 1.0f;
    }
    /* ease-out cubic */
    float e = 1.0f - t;
    t = 1.0f - e * e * e;

    int curW = (int)(BOX_W * t);
    if (curW < 1)
    {
        curW = 1;
    }
    int curH = (int)(BOX_H * t);
    if (curH < 1)
    {
        curH = 1;
    }
    int curX = CENTER_X - curW / 2;
    int curY = CENTER_Y - curH / 2;
    int r = (int)(BOX_RADIUS * t);

    pd->graphics->fillRoundRect(curX, curY, curW, curH, r, kColorWhite);
    pd->graphics->drawRoundRect(curX, curY, curW, curH, r, 1, kColorBlack);

    if (t > 0.4f)
    {
        LCDFont *fontH = style_font(PLUTO_FONT_HEADING2);
        LCDFont *fontBold = style_font(PLUTO_FONT_BODY_BOLD);
        LCDFont *fontSmall = style_font(PLUTO_FONT_SMALL);

        pd->graphics->pushContext(NULL);
        pd->graphics->setClipRect(curX + 2, curY + 2, curW - 4, curH - 4);

        int innerX = curX + 16;
        int innerY = curY + 10;
        int innerW = curW - 32;

        pd->graphics->setFont(fontH);
        const char *title = "SETTINGS";
        pd->graphics->drawText(title, strlen(title), kUTF8Encoding, innerX, innerY);
        pd->graphics->drawLine(innerX, innerY + 16, innerX + innerW, innerY + 16,
                               1, kColorBlack);

        int itemY = innerY + SETTINGS_TITLE_H;      /* list top (after title) */
        int itemH = SETTINGS_ITEM_H;
        int itemGap = SETTINGS_ROW_PITCH - SETTINGS_ITEM_H;
        int listH = settings_list_height();
        int scroll = settings_clamped_scroll();
        g_scrollOffset = scroll;

        /* Edge arrows: indicate more rows beyond the visible window. */
        if (scroll > 0)
        { /* can scroll up */
            int ax = innerX + innerW / 2;
            int ay = itemY - 4;
            pd->graphics->fillTriangle(ax - 4, ay + 3, ax + 4, ay + 3, ax, ay - 3,
                                       kColorBlack);
        }
        if (scroll < settings_max_scroll())
        { /* can scroll down */
            int ax = innerX + innerW / 2;
            int ay = itemY + listH + 3;
            pd->graphics->fillTriangle(ax - 4, ay - 3, ax + 4, ay - 3, ax, ay + 3,
                                       kColorBlack);
        }

        /* Push a sub-context clipped to the row area so partially scrolled
         * rows cut off cleanly instead of bleeding over the title/footer. */
        pd->graphics->pushContext(NULL);
        pd->graphics->setClipRect(curX + 2, itemY, curW - 4, listH);

        for (int i = 1; i <= OPTION_COUNT; i++)
        {
            int iy = itemY + (i - 1) * (itemH + itemGap) - scroll;
            if (iy + itemH < itemY || iy > itemY + listH)
            {
                continue; /* fully outside the visible window */
            }
            int isSel = (i == g_selectedIndex);
            int isAction = (i == 8);

            if (isSel)
            {
                pd->graphics->fillRoundRect(innerX - 4, iy, innerW + 8, itemH, 4,
                                            kColorBlack);
                pd->graphics->drawRoundRect(innerX - 3, iy + 1, innerW + 6,
                                            itemH - 2, 3, 1, kColorWhite);
                pd->graphics->setDrawMode(kDrawModeFillWhite);
            }
            else
            {
                pd->graphics->setDrawMode(kDrawModeCopy);
            }

            pd->graphics->setFont(fontBold);
            pd->graphics->drawText(k_settingsLabels[i - 1],
                                   strlen(k_settingsLabels[i - 1]),
                                   kUTF8Encoding, innerX + 4, iy + 5);

            const char *val = settings_page_staged_value(i);
            if (val && val[0])
            {
                pd->graphics->setFont(fontSmall);
                int valW = style_get_text_width(PLUTO_FONT_SMALL, val);
                pd->graphics->drawText(val, strlen(val), kUTF8Encoding,
                                       innerX + innerW - valW - 4, iy + 7);
            }

            if (isSel && !isAction)
            {
                pd->graphics->setFont(fontSmall);
                int valW = val ? style_get_text_width(PLUTO_FONT_SMALL, val) : 0;
                pd->graphics->drawText("<", 1, kUTF8Encoding,
                                       innerX + innerW - valW - 18, iy + 7);
                pd->graphics->drawText(">", 1, kUTF8Encoding, innerX + innerW - 2,
                                       iy + 7);
            }
            else if (isSel && isAction)
            {
                pd->graphics->setFont(fontSmall);
                const char *pa = "Press A";
                int paW = style_get_text_width(PLUTO_FONT_SMALL, pa);
                pd->graphics->drawText(pa, strlen(pa), kUTF8Encoding,
                                       innerX + innerW - paW - 4, iy + 7);
            }

            pd->graphics->setDrawMode(kDrawModeCopy);
        }

        pd->graphics->popContext(); /* row-clip context */

        int footerY = itemY + listH + 6;
        pd->graphics->setFont(fontSmall);
        const char *footer = "(B) Cancel  *  (A) Save & Close";
        pd->graphics->drawText(footer, strlen(footer), kUTF8Encoding, innerX,
                               footerY);

        pd->graphics->clearClipRect();
        pd->graphics->popContext();
    }
}
