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
 * BF14 fix (user-requested): the crank now SCROLLS AND SELECTS together.
 * Previously crankChange only accumulated g_targetScrollY while the
 * selection was clamped to D-pad rules, so the highlight could get stuck:
 * scrolling down never selected the bottom-row bookmark (the scroll ran
 * past it, and odd counts leave the last row's second cell empty), and
 * once the free scroll pushed a selected card off the top the Settings
 * button was unreachable by crank. Now the crank moves the selection in
 * READING ORDER (Settings = index 0, then every card 1..count) and the
 * scroll target follows the selection, clamped to the real content
 * bottom. Cranking past the final card free-scrolls into the footer (also
 * clamped); every bookmark is selectable and Settings is always reachable
 * scrolling back up.
 * BF14b (user feedback): step eased 18° -> 25°; BF14c: -> 90° (a quarter
 * turn); BF14d (user request "I want 45 degrees"): -> **45° per bookmark**
 * (half a quarter turn). HOME PAGE only; page scrolling elsewhere is
 * untouched. Keep the define in home_page.h so the host test drives the
 * exact production value.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui/home_page.h"
#include "core/constants.h"
#include "core/storage.h"
#include "core/logger.h"
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

/* BF14 crank-selection state. The crank moves the selection through the
 * bookmark list in READING ORDER (Settings = index 0, then every card
 * 1..count) so every bookmark is crank-selectable, and the scroll target
 * follows the selection. g_crankTarget anchors the selection index for the
 * current crank gesture so down-then-up returns to the exact item you came
 * from; any button push ends the gesture (handle_input resets it). */
#define HOME_CRANK_STEP_PX 45.0f /* BF14d: one bookmark per 45° of crank
                                  * travel (user request "I want 45 degrees";
                                  * history 18 -> 25 -> 90 -> 45). Reading
                                  * order crosses a 2-card grid row every
                                  * 90°. Defined in home_page.h so the host
                                  * test stays in sync with production. */
static float g_crankFrac = 0.0f; /* sub-step remainder, crank degrees */
static int g_crankTarget = -1;   /* selection anchor; -1 = no gesture */

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
    g_crankFrac = 0.0f;
    g_crankTarget = -1;
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

/* ── BF14: crank-driven selection + bounded scroll ─────────────────────── */

/* Absolute (unscrolled) Y of the first pixel BELOW all home content.
 * draw() builds the same chain: startY = CONTENT_Y+4 → settings row
 * (+100+22) → section header (+14) → grid (+24) → rows*(46+8) → footer
 * header (+12) → footer list (+76+4) → bottom margin (8). Keep in sync. */
static int home_content_bottom(int count)
{
    int rows = (count + 1) / 2; /* grid rows used by cards */
    return CONTENT_Y + 4 + 172 + rows * (46 + 8) + 80 + 8;
}

/* Crank → selection (BF14). One bookmark step per HOME_CRANK_STEP_PX
 * degrees of crank travel (45° after BF14d — see
 * home_page.h). Reading
 * order walks Settings = index 0 then every card, so the highlight passes
 * through every bookmark; the scroll target follows the selection (see
 * home_page_update_scroll). Cranking past the final card free-scrolls
 * into the footer. invertCrank flips the direction (as the old scroll
 * did). */
void home_page_handle_crank(float crankChange)
{
    if (crankChange == 0.0f)
    {
        return;
    }

    int count = bookmark_count();
    if (count < 0)
    {
        count = 0;
    }

    float dir = storage_setting_int("invertCrank") ? -1.0f : 1.0f;
    g_crankFrac += crankChange * dir;

    int steps = (int)(g_crankFrac / HOME_CRANK_STEP_PX); /* truncate to 0 */
    g_crankFrac -= (float)steps * HOME_CRANK_STEP_PX;
    if (steps == 0)
    {
        return;
    }

    if (g_crankTarget < 0)
    {
        /* New gesture: anchor at the current selection (defensively clamped
         * to the live count). */
        g_crankTarget = g_selectedIndex > count ? count : g_selectedIndex;
    }
    g_crankTarget += steps;

    if (g_crankTarget > count)
    {
        g_crankTarget = count;
        if (count > 0 && g_selectedIndex == count)
        {
            /* Already on the final card: keep scrolling into the footer at
             * the grid-row pitch; update_scroll clamps to content bottom. */
            g_targetScrollY += (float)steps * (46 + 8);
            logger_log("home: crank freescroll tgt=%d", (int)g_targetScrollY);
            return;
        }
    }
    if (g_crankTarget < 0)
    {
        g_crankTarget = 0; /* top: Settings button */
    }

    if (g_crankTarget != g_selectedIndex)
    {
        g_selectedIndex = g_crankTarget;
        logger_log("home: crank sel=%d count=%d", g_selectedIndex, count);
    }
}

/* End the crank gesture without moving the selection (BF14): called when a
 * B-hold starts on the home page so the next crank re-anchors from wherever
 * the selection ended up. */
void home_page_end_crank_gesture(void)
{
    g_crankTarget = -1;
    g_crankFrac = 0.0f;
}

/* Scroll target follows the selection (BF14), clamped to the content, then
 * eased. Split out of draw() so the host test can drive it without a
 * graphics vtable. */
void home_page_update_scroll(void)
{
    int count = bookmark_count();

    /* Auto-scroll to keep the selected item visible */
    if (g_selectedIndex == 0)
    {
        g_targetScrollY = 0;
    }
    else if (count > 0)
    {
        int row = (g_selectedIndex - 1) / 2;
        /* BF14: CONTENT_Y+4+160 matches cardsStartY = startY+160 exactly
         * (was +12+148 — 4px stale after the BF10 layout shift). */
        int selectedAbsY = CONTENT_Y + 4 + 160 + row * (46 + 8);
        int displayY = (int)(selectedAbsY - g_targetScrollY);
        if (displayY > SCREEN_HEIGHT - 46)
        {
            /* BF14: was SCREEN_HEIGHT-40 — the whole 46px card must fit so
             * the bottom-row bookmark is selected AND fully visible. */
            g_targetScrollY = (float)(selectedAbsY - SCREEN_HEIGHT + 46);
        }
        else if (displayY < CONTENT_Y + 10)
        {
            float t = (float)(selectedAbsY - CONTENT_Y - 10);
            g_targetScrollY = t > 0 ? t : 0;
        }
    }

    /* BF14: never scroll past the end of the content. Previously the free
     * crank scroll could run arbitrarily far below the last row, which is
     * how the selection ended up stranded away from the view. */
    {
        int maxScroll = home_content_bottom(count) - SCREEN_HEIGHT;
        if (maxScroll < 0)
        {
            maxScroll = 0;
        }
        if (g_targetScrollY > (float)maxScroll)
        {
            g_targetScrollY = (float)maxScroll;
        }
        if (g_targetScrollY < 0)
        {
            g_targetScrollY = 0;
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

    if (pushed != 0)
    {
        /* BF14: any button press ends the crank gesture; the next crank
         * starts fresh from wherever the selection ended up. (pushed is
         * the full button mask, so B/address-bar presses are covered.) */
        g_crankTarget = -1;
        g_crankFrac = 0.0f;
    }

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

    /* BF14: the crank moves the SELECTION (Settings button included) —
     * main.c feeds crank deltas to home_page_handle_crank() alongside the
     * button handling; draw() only follows the selection with the scroll
     * target, clamped to the real content bottom. update_scroll is a
     * separate function so the host test can drive it without a graphics
     * vtable. */
    (void)crankChange; /* selection handled by the caller since BF14 */
    home_page_update_scroll();

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
    /* BF11: one long line was cut off at the right edge — hints now listed
     * under an underlined "Buttons to Press:" header, each bulleted. */
    pd->graphics->setFont(fontSmall);
    const char *footerHdr = "Buttons to Press:";
    pd->graphics->drawText(footerHdr, strlen(footerHdr), kUTF8Encoding, 24,
                           bottomY);
    int hdrW = style_get_text_width(PLUTO_FONT_SMALL, footerHdr);
    int hdrH = pd->graphics->getFontHeight(fontSmall);
    pd->graphics->drawLine(24, bottomY + hdrH + 2, 24 + hdrW,
                           bottomY + hdrH + 2, 1, kColorBlack);
    const char *footerA = "(A) Open";
    const char *footerB = "(B) Search/URL";
    const char *footerC = "Menu: Settings";
    pd->graphics->drawText(footerA, strlen(footerA), kUTF8Encoding, 34,
                           bottomY + 32);
    pd->graphics->drawText(footerB, strlen(footerB), kUTF8Encoding, 34,
                           bottomY + 52);
    pd->graphics->drawText(footerC, strlen(footerC), kUTF8Encoding, 34,
                           bottomY + 72);
    /* square bullets aligned with each line's vertical center */
    pd->graphics->fillRect(24, bottomY + 36, 4, 4, kColorBlack);
    pd->graphics->fillRect(24, bottomY + 56, 4, 4, kColorBlack);
    pd->graphics->fillRect(24, bottomY + 76, 4, 4, kColorBlack);
}
