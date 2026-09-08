/*
 * PlutoBrowser — home_page.c
 * Start page / speed dial (port of Source/ui/home_page.lua). See home_page.h.
 * All geometry, navigation quirks (2-column grid with row wrap, settings
 * button at index 0), crank scrolling (×1.5, invertCrank flips), smooth
 * scroll (0.3 factor, 0.5 snap), auto-scroll-to-selection, comet logo,
 * address bar pill, and per-card oscillating marquee (speed 50, dwell 1s)
 * preserved from the reference.
 * BF7 fix (user-requested): the marquee's clip rect no longer shaves the
 * bottom off scrolling glyphs — its height now comes from the actual font
 * height instead of the reference's hardcoded 15px. Horizontal scrolling
 * behavior is unchanged.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui/home_page.h"
#include "core/constants.h"
#include "core/storage.h"
#include "render/style.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

/* PDButtons bits */
#define BTN_LEFT (1 << 0)
#define BTN_RIGHT (1 << 1)
#define BTN_UP (1 << 2)
#define BTN_DOWN (1 << 3)
#define BTN_A (1 << 5)

static int g_selectedIndex = 0;
static float g_scrollY = 0;
static float g_targetScrollY = 0;

/* marquee state per card (Lua keyed "t<i>"/"d<i>"; we track per index) */
typedef struct
{
    unsigned int startMs;
} MarqueeState;
static MarqueeState g_marquee[64]; /* 2 per card up to 32 cards */

static unsigned int now_ms(void)
{
    return pluto_pd()->system->getCurrentTimeMilliseconds();
}

void home_page_reset(void)
{
    g_selectedIndex = 0;
    g_scrollY = 0;
    g_targetScrollY = 0;
    memset(g_marquee, 0, sizeof(g_marquee));
}

int home_page_selected_index(void)
{
    return g_selectedIndex;
}

int home_page_scroll_y(void)
{
    return (int)g_scrollY;
}

static int bookmark_count(void)
{
    return storage_bookmark_count();
}

/* Oscillating marquee, faithful to drawMarquee(): speed 50 px/s, dwell 1s.
 * BF7: measures with the SAME font role that draws (the reference passed the
 * font object itself; the C port previously measured BODY for both lines),
 * and the clip height now derives from the font's real height (+2 safety)
 * — the hardcoded 15px shaved glyph bottoms off the taller system font. */
static void draw_marquee(const char *text, int x, int y, int maxW,
                         LCDFont *font, PlutoFontRole role, int slot)
{
    PlaydateAPI *pd = pluto_pd();
    pd->graphics->setFont(font);
    int tw = style_get_text_width(role, text);
    if (tw <= maxW)
    {
        pd->graphics->drawText(text, strlen(text), kUTF8Encoding, x, y);
        return;
    }

    int range = tw - maxW;
    if (slot < 0 || slot >= 64)
    {
        slot = 0;
    }
    if (g_marquee[slot].startMs == 0)
    {
        g_marquee[slot].startMs = now_ms();
    }
    float elapsed = (float)(now_ms() - g_marquee[slot].startMs) / 1000.0f;
    float speed = 50.0f;
    float dwell = 1.0f;
    float travel = (float)range / speed;
    float cycle = 2.0f * (dwell + travel);
    float t = elapsed - ((int)(elapsed / cycle)) * cycle;
    float offset;
    if (t < dwell)
    {
        offset = 0;
    }
    else if (t < dwell + travel)
    {
        offset = (t - dwell) * speed;
    }
    else if (t < dwell + travel + dwell)
    {
        offset = (float)range;
    }
    else
    {
        offset = (float)range - (t - dwell - travel - dwell) * speed;
    }

    /* BF7: clip to the full font height so descenders (g, y, p, &) are
     * never cut at the bottom while the text scrolls horizontally. */
    int clipH = pd->graphics->getFontHeight(font) + 2;
    pd->graphics->setClipRect(x, y, maxW, clipH);
    pd->graphics->drawText(text, strlen(text), kUTF8Encoding,
                           x - (int)offset, y);
    pd->graphics->clearClipRect();
}

char *home_page_handle_input(unsigned int pushed, void (*settingsCallback)(void))
{
    int count = bookmark_count();
    int isOnSettingsBtn = (g_selectedIndex == 0);

    if (pushed & BTN_DOWN)
    {
        if (isOnSettingsBtn)
        {
            if (count > 0)
            {
                g_selectedIndex = 1;
            }
        }
        else if (g_selectedIndex + 2 <= count)
        {
            g_selectedIndex += 2;
        }
        else if (g_selectedIndex < count)
        {
            g_selectedIndex += 1;
        }
    }
    else if (pushed & BTN_UP)
    {
        if (!isOnSettingsBtn)
        {
            if (g_selectedIndex <= 2)
            {
                g_selectedIndex = 0;
            }
            else if (g_selectedIndex - 2 >= 1)
            {
                g_selectedIndex -= 2;
            }
        }
    }
    else if (pushed & BTN_RIGHT)
    {
        if (!isOnSettingsBtn && g_selectedIndex % 2 == 1 && g_selectedIndex + 1 <= count)
        {
            g_selectedIndex++;
        }
    }
    else if (pushed & BTN_LEFT)
    {
        if (!isOnSettingsBtn && g_selectedIndex % 2 == 0 && g_selectedIndex > 1)
        {
            g_selectedIndex--;
        }
    }

    if (pushed & BTN_A)
    {
        if (isOnSettingsBtn)
        {
            if (settingsCallback)
            {
                settingsCallback();
            }
            return NULL;
        }
        const StoredBookmark *bm = storage_bookmark_at(g_selectedIndex - 1);
        if (bm)
        {
            size_t n = strlen(bm->url) + 1;
            char *out = (char *)pluto_pd()->system->realloc(NULL, n);
            if (out)
            {
                memcpy(out, bm->url, n);
            }
            return out;
        }
    }

    return NULL;
}

void home_page_draw(float crankChange)
{
    PlaydateAPI *pd = pluto_pd();
    int count = bookmark_count();
    LCDFont *fontHeading = style_font(PLUTO_FONT_HEADING1);
    LCDFont *fontBold = style_font(PLUTO_FONT_BODY_BOLD);
    LCDFont *fontSmall = style_font(PLUTO_FONT_SMALL);

    if (crankChange != 0.0f)
    {
        int dir = storage_setting_int("invertCrank") ? -1 : 1;
        g_targetScrollY += crankChange * 1.5f * dir;
        if (g_targetScrollY < 0)
        {
            g_targetScrollY = 0;
        }
    }

    /* Auto-scroll to keep the selected item visible */
    if (g_selectedIndex == 0)
    {
        g_targetScrollY = 0;
    }
    else if (count > 0)
    {
        int row = (g_selectedIndex - 1) / 2;
        int selectedAbsY = CONTENT_Y + 12 + 148 + row * (46 + 8);
        int displayY = (int)(selectedAbsY - g_targetScrollY);
        if (displayY > SCREEN_HEIGHT - 40)
        {
            g_targetScrollY = (float)(selectedAbsY - SCREEN_HEIGHT + 40);
        }
        else if (displayY < CONTENT_Y + 10)
        {
            float t = (float)(selectedAbsY - CONTENT_Y - 10);
            g_targetScrollY = t > 0 ? t : 0;
        }
    }

    /* Smooth scroll toward target */
    g_scrollY += (g_targetScrollY - g_scrollY) * 0.3f;
    if (g_targetScrollY - g_scrollY < 0.5f && g_scrollY - g_targetScrollY < 0.5f)
    {
        g_scrollY = g_targetScrollY;
    }
    if (g_scrollY < 0)
    {
        g_scrollY = 0;
    }

    int startY = CONTENT_Y + 4 - (int)g_scrollY; /* BF10: was +12 — banner sat 8px below the chrome leaving a white strip; shifting the whole page up puts the banner flush under the chrome */

    /* 1. Pluto Browser logo header (BF8: rebranded from the Lua reference's
     *    "COMET BROWSER" per user request — same 13-char layout envelope).
     * BF10: rect top returns to startY-4 (= chrome bottom after the page
     * shift) and height 54->62 so ~12px of black pad sits below the
     * subtitle (was ~0px). */
    pd->graphics->fillRect(0, startY - 4, SCREEN_WIDTH, 62, kColorBlack);

    /* comet icon: nucleus + tail */
    int cx = 30;
    int cy = startY + 16;
    for (int i = 0; i <= 12; i++)
    {
        if (i % 2 == 0)
        {
            pd->graphics->drawLine(cx - i * 3, cy - i, cx - i * 3 - 4, cy - i + 2,
                                   1, kColorWhite);
        }
    }
    pd->graphics->fillEllipse(cx - 7, cy - 7, 15, 15, 0.0f, 360.0f, kColorWhite);
    pd->graphics->fillEllipse(cx - 4, cy - 4, 9, 9, 0.0f, 360.0f, kColorBlack);
    pd->graphics->fillEllipse(cx - 1 - 2, cy - 2 - 2, 5, 5, 0.0f, 360.0f, kColorWhite);

    /* Title text */
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont(fontHeading);
    const char *title = "PLUTO BROWSER"; /* BF8: was "COMET BROWSER" */
    pd->graphics->drawText(title, strlen(title), kUTF8Encoding, 48, startY + 4);
    pd->graphics->setFont(fontSmall);
    const char *subtitle = "The Web on Playdate";
    pd->graphics->drawText(subtitle, strlen(subtitle), kUTF8Encoding, 50,
                           startY + 32);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* Address bar prompt pill — BF10: banner bottom is now startY+58, so the
     * pill moved 56->68 to leave a clear white gap below the banner. */
    pd->graphics->fillRoundRect(20, startY + 68, SCREEN_WIDTH - 40, 24, 4,
                                kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont(fontBold);
    const char *prompt = "Press (B) to Type URL or Search Web";
    pd->graphics->drawText(prompt, strlen(prompt), kUTF8Encoding, 32, startY + 72);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* Settings button */
    int settingsBtnY = startY + 100; /* BF10: +12 with the pill to keep its 8px gap */
    int settingsBtnH = 22;
    int isSettingsSelected = (g_selectedIndex == 0);
    int settingsBtnW = SCREEN_WIDTH - 40;
    int settingsBtnX = 20;

    pd->graphics->drawLine(20, settingsBtnY, SCREEN_WIDTH - 20, settingsBtnY,
                           1, kColorBlack);

    if (isSettingsSelected)
    {
        pd->graphics->fillRoundRect(settingsBtnX, settingsBtnY + 4, settingsBtnW,
                                    settingsBtnH, 4, kColorBlack);
        pd->graphics->drawRoundRect(settingsBtnX + 1, settingsBtnY + 5,
                                    settingsBtnW - 2, settingsBtnH - 2, 3, 1,
                                    kColorWhite);
        pd->graphics->setDrawMode(kDrawModeFillWhite);
    }
    else
    {
        pd->graphics->fillRoundRect(settingsBtnX, settingsBtnY + 4, settingsBtnW,
                                    settingsBtnH, 4, kColorWhite);
        pd->graphics->drawRoundRect(settingsBtnX, settingsBtnY + 4, settingsBtnW,
                                    settingsBtnH, 4, 1, kColorBlack);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }

    pd->graphics->setFont(fontBold);
    const char *settingsLabel = "Settings";
    int settingsLabelW = style_get_text_width(PLUTO_FONT_BODY_BOLD, settingsLabel);
    pd->graphics->drawText(settingsLabel, strlen(settingsLabel), kUTF8Encoding,
                           settingsBtnX + (settingsBtnW - settingsLabelW) / 2,
                           settingsBtnY + 9);
    pd->graphics->setDrawMode(kDrawModeCopy);

    pd->graphics->drawLine(20, settingsBtnY + settingsBtnH + 6,
                           SCREEN_WIDTH - 20, settingsBtnY + settingsBtnH + 6,
                           1, kColorBlack);

    /* Speed Dial section title */
    int gridStartY = settingsBtnY + settingsBtnH + 14;
    pd->graphics->setFont(fontBold);
    const char *section = "SPEED DIAL / BOOKMARKS";
    pd->graphics->drawText(section, strlen(section), kUTF8Encoding, 20, gridStartY);
    pd->graphics->drawLine(20, gridStartY + 16, SCREEN_WIDTH - 20, gridStartY + 16,
                           1, kColorBlack);

    /* Speed dial 2-column grid */
    int cardW = 172;
    int cardH = 46;
    int gapX = 16;
    int gapY = 8;
    int cardsStartY = gridStartY + 24;

    for (int i = 1; i <= count; i++)
    {
        const StoredBookmark *bm = storage_bookmark_at(i - 1);
        if (!bm)
        {
            continue;
        }
        int col = (i - 1) % 2;
        int row = (i - 1) / 2;
        int cardX = 20 + col * (cardW + gapX);
        int cardY = cardsStartY + row * (cardH + gapY);

        /* skip cards scrolled out of view (visual-only optimization; the Lua
         * version drew everything — results identical on a 1-bit screen) */
        if (cardY > SCREEN_HEIGHT || cardY + cardH < CONTENT_Y)
        {
            continue;
        }

        int isSelected = (i == g_selectedIndex);

        if (isSelected)
        {
            pd->graphics->fillRoundRect(cardX, cardY, cardW, cardH, 5, kColorBlack);
            pd->graphics->drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2,
                                        4, 1, kColorWhite);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        else
        {
            pd->graphics->fillRoundRect(cardX, cardY, cardW, cardH, 5, kColorWhite);
            pd->graphics->drawRoundRect(cardX, cardY, cardW, cardH, 5, 1,
                                        kColorBlack);
            pd->graphics->setDrawMode(kDrawModeCopy);
        }

        int textAreaW = cardW - 16;

        pd->graphics->setFont(fontBold);
        const char *t = bm->title ? bm->title : bm->url;
        draw_marquee(t, cardX + 8, cardY + 6, textAreaW, fontBold,
                     PLUTO_FONT_BODY_BOLD, i * 2 - 2);

        pd->graphics->setFont(fontSmall);
        const char *d = bm->desc ? bm->desc : bm->url;
        draw_marquee(d, cardX + 8, cardY + 24, textAreaW, fontSmall,
                     PLUTO_FONT_SMALL, i * 2 - 1);

        pd->graphics->setDrawMode(kDrawModeCopy);
    }

    int bottomY = cardsStartY + (count + 1) / 2 * (cardH + gapY) + 12;
    pd->graphics->setFont(fontSmall);
    const char *footer = "(A) Open  -  (B) Search/URL  -  Menu: Settings";
    pd->graphics->drawText(footer, strlen(footer), kUTF8Encoding, 24, bottomY);
}
