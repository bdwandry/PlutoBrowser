// home_page.c — C port of Source/ui/home_page.lua (HomePage).
//
// Start page / speed dial: logo header, address-bar prompt pill,
// selectable Settings button, 2-column bookmark grid with marquee text,
// crank scroll + auto-scroll-to-selection. See home_page.h for parity
// notes.

#include "ui/home_page.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/constants.h"
#include "../core/logger.h"
#include "../core/storage.h"
#include "../render/style.h"
#include "../util/dynarray.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_HP_PD 1
#endif

#ifdef PLUTO_HP_PD
#include "pd_api.h"
static PlaydateAPI* s_pd = NULL;

void hp_init(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void hp_init(struct PlaydateAPI* pd) { (void)pd; }
#endif

/* ── state ────────────────────────────────────────────────────────────── */

static int s_selectedIndex = 0;   /* 0 = Settings button */
static double s_scrollY = 0.0;
static double s_targetScrollY = 0.0;
static void (*s_settingsCb)(void) = NULL;

/* Per-card marquee start timestamps keyed by "t<i>"/"d<i>". */
#define HP_MARQUEE_CAP 64
typedef struct {
    char key[16];
    unsigned long startMs;
} HpMarquee;
static HpMarquee s_marquees[HP_MARQUEE_CAP];
static int s_nMarquees = 0;

void hp_reset(void) {
    s_selectedIndex = 0;
    s_scrollY = 0.0;
    s_targetScrollY = 0.0;
    s_nMarquees = 0; /* Lua: marqueeState = {} */
}

void hp_set_settings_callback(void (*fn)(void)) { s_settingsCb = fn; }

int hp_selected_index(void) { return s_selectedIndex; }

void hp_set_selected_index(int idx) {
    if (idx >= 0) s_selectedIndex = idx;
}

/* ── pure math seams ──────────────────────────────────────────────────── */

void hp_grid_nav(HpButton btn, int* selectedIndex, int count) {
    int sel = *selectedIndex;

    switch (btn) {
    case HP_BTN_DOWN:
        if (sel == 0) {
            if (count > 0) *selectedIndex = 1;
        } else if (sel + 2 <= count) {
            *selectedIndex = sel + 2;
        } else if (sel < count) {
            *selectedIndex = sel + 1;
        }
        break;
    case HP_BTN_UP:
        if (sel == 0) break; /* already at top */
        if (sel <= 2) *selectedIndex = 0;
        else if (sel - 2 >= 1) *selectedIndex = sel - 2;
        break;
    case HP_BTN_RIGHT:
        if (sel != 0 && sel % 2 == 1 && sel + 1 <= count)
            *selectedIndex = sel + 1;
        break;
    case HP_BTN_LEFT:
        if (sel != 0 && sel % 2 == 0 && sel > 1)
            *selectedIndex = sel - 1;
        break;
    default:
        break;
    }
}

double hp_autoscroll_target(int selectedIndex, int count,
                            double targetScrollY) {
    if (selectedIndex == 0) return 0.0;
    if (count <= 0) return targetScrollY;

    int row = (selectedIndex - 1) / 2;
    double selectedAbsY =
        PLUTO_CONTENT_Y + 12 + 148 + (double)row * (46.0 + 8.0);
    double displayY = selectedAbsY - targetScrollY;
    if (displayY > (double)PLUTO_SCREEN_HEIGHT - 40.0)
        return selectedAbsY - (double)PLUTO_SCREEN_HEIGHT + 40.0;
    if (displayY < (double)PLUTO_CONTENT_Y + 10.0) {
        double t = selectedAbsY - (double)PLUTO_CONTENT_Y - 10.0;
        return t > 0.0 ? t : 0.0;
    }
    return targetScrollY;
}

double hp_marquee_offset(double elapsedSec, double textW, double maxW) {
    double range = textW - maxW;
    if (range <= 0.0) return 0.0;

    const double speed = 50.0;
    const double dwell = 1.0;
    double travel = range / speed;
    double cycle = 2.0 * (dwell + travel);
    double t = fmod(elapsedSec, cycle);
    if (t < dwell)
        return 0.0;
    if (t < dwell + travel)
        return (t - dwell) * speed;
    if (t < dwell + travel + dwell)
        return range;
    return range - (t - dwell - travel - dwell) * speed;
}

double hp_smooth_scroll(double cur, double target) {
    cur = cur + (target - cur) * 0.3;
    if (fabs(target - cur) < 0.5) cur = target;
    return cur > 0.0 ? cur : 0.0;
}

/* ── storage accessors ────────────────────────────────────────────────── */

int hp_bookmark_count(void) {
    DynArray* bms = storage_bookmarks();
    return bms ? (int)bms->count : 0;
}

/* ── input ────────────────────────────────────────────────────────────── */

HpAction hp_handle_input(HpButton btn, char* outUrl, size_t cap) {
    int count = hp_bookmark_count();

    if (btn == HP_BTN_A) {
        if (s_selectedIndex == 0) {
            /* Lua calls settingsCallback() then returns nil */
            if (s_settingsCb != NULL) s_settingsCb();
            return HP_ACT_SETTINGS;
        }
        DynArray* bms = storage_bookmarks();
        PlutoSavedBookmark* bm =
            (bms != NULL) ? da_get(bms, (size_t)s_selectedIndex - 1)
                          : NULL;
        if (bm != NULL) {
            if (outUrl != NULL && cap > 0)
                snprintf(outUrl, cap, "%s", bm->url);
            return HP_ACT_OPEN_URL;
        }
        return HP_ACT_NONE;
    }

    hp_grid_nav(btn, &s_selectedIndex, count);
    return HP_ACT_NONE;
}

/* ── draw ─────────────────────────────────────────────────────────────── */

#ifdef PLUTO_HP_PD

#define HP_TAU 6.2831853f
static void pd_fill_circle(int cx, int cy, int r, LCDColor color) {
    s_pd->graphics->fillEllipse(cx - r, cy - r, r * 2, r * 2, 0.0f,
                                HP_TAU, color);
}

/* Marquee start timestamp for a key (Lua marqueeState[key]). */
static unsigned long marquee_start(const char* key) {
    for (int i = 0; i < s_nMarquees; i++)
        if (strcmp(s_marquees[i].key, key) == 0)
            return s_marquees[i].startMs;
    if (s_nMarquees < HP_MARQUEE_CAP) {
        HpMarquee* m = &s_marquees[s_nMarquees++];
        snprintf(m->key, sizeof(m->key), "%s", key);
        m->startMs = s_pd->system->getCurrentTimeMilliseconds();
        return m->startMs;
    }
    return s_pd->system->getCurrentTimeMilliseconds(); /* table full */
}

/* Clipped oscillating text inside a maxW-wide box (Lua drawMarquee). */
static void draw_marquee(const char* text, int x, int y, int maxW,
                         PlutoFont* font, const char* key) {
    int tw = style_get_text_width(font, text);
    if (tw <= maxW) {
        s_pd->graphics->drawText(text, strlen(text), kUTF8Encoding,
                                 x, y);
        return;
    }

    unsigned long nowMs = s_pd->system->getCurrentTimeMilliseconds();
    unsigned long startMs = marquee_start(key);
    double elapsed = (double)(nowMs - startMs) / 1000.0;
    if (elapsed < 0.0) elapsed = 0.0;
    double offset =
        hp_marquee_offset(elapsed, (double)tw, (double)maxW);

    s_pd->graphics->setClipRect(x, y, maxW, 15);
    s_pd->graphics->drawText(text, strlen(text), kUTF8Encoding,
                             x - (int)offset, y);
    s_pd->graphics->clearClipRect();
}

void hp_draw(double crankChange) {
    if (s_pd == NULL) return;
    PlaydateAPI* pd = s_pd;

    DynArray* bookmarks = storage_bookmarks();
    int count = bookmarks ? (int)bookmarks->count : 0;
    int sz, lh;
    PlutoFont* fontHeading = style_get_heading_font(1, NULL, NULL);
    PlutoFont* fontBold = style_get_body_font(1, 0, &sz);
    PlutoFont* fontBody = style_get_body_font(0, 0, &sz);
    PlutoFont* fontSmall = style_get_ui_small_font();

    if (crankChange != 0.0) {
        int dir = storage_settings()->invertCrank ? -1 : 1;
        s_targetScrollY += crankChange * 1.5 * dir;
        if (s_targetScrollY < 0.0) s_targetScrollY = 0.0;
    }

    /* Auto-scroll to keep the selected item visible */
    s_targetScrollY =
        hp_autoscroll_target(s_selectedIndex, count, s_targetScrollY);

    /* Smooth scroll toward target */
    s_scrollY = hp_smooth_scroll(s_scrollY, s_targetScrollY);

    int startY = PLUTO_CONTENT_Y + 12 - (int)s_scrollY;

    /* 1. Logo header */
    pd->graphics->fillRect(0, startY - 4, PLUTO_SCREEN_WIDTH, 54,
                           kColorBlack);

    /* Comet icon (pixel art: nucleus + tail) */
    int cx = 30;
    int cy = startY + 16;
    for (int i = 0; i <= 12; i++) {
        if (i % 2 == 0)
            pd->graphics->drawLine(cx - i * 3, cy - i,
                                   cx - i * 3 - 4, cy - i + 2, 1,
                                   kColorWhite);
    }
    pd_fill_circle(cx, cy, 7, kColorWhite);
    pd_fill_circle(cx, cy, 4, kColorBlack);
    pd_fill_circle(cx - 1, cy - 2, 2, kColorWhite);

    /* Title text */
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont((LCDFont*)fontHeading);
    pd->graphics->drawText("COMET BROWSER", 13, kASCIIEncoding, 48,
                           startY + 4);
    pd->graphics->setFont((LCDFont*)fontSmall);
    pd->graphics->drawText("The Web on Playdate", 18, kASCIIEncoding, 50,
                           startY + 32);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* Address bar prompt pill */
    pd->graphics->fillRoundRect(20, startY + 56, PLUTO_SCREEN_WIDTH - 40,
                                24, 4, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont((LCDFont*)fontBold);
    pd->graphics->drawText("Press (B) to Type URL or Search Web", 36,
                           kASCIIEncoding, 32, startY + 60);
    pd->graphics->setDrawMode(kDrawModeCopy);

    /* Settings button (selectable, below address bar with spacing) */
    int settingsBtnY = startY + 88;
    int settingsBtnH = 22;
    int isSettingsSelected = (s_selectedIndex == 0);
    int settingsBtnW = PLUTO_SCREEN_WIDTH - 40;
    int settingsBtnX = 20;

    pd->graphics->drawLine(20, settingsBtnY, PLUTO_SCREEN_WIDTH - 20,
                           settingsBtnY, 1, kColorBlack);

    if (isSettingsSelected) {
        pd->graphics->fillRoundRect(settingsBtnX, settingsBtnY + 4,
                                    settingsBtnW, settingsBtnH, 4,
                                    kColorBlack);
        pd->graphics->drawRoundRect(settingsBtnX + 1, settingsBtnY + 5,
                                    settingsBtnW - 2, settingsBtnH - 2,
                                    3, 1, kColorWhite);
        pd->graphics->setDrawMode(kDrawModeFillWhite);
    } else {
        pd->graphics->fillRoundRect(settingsBtnX, settingsBtnY + 4,
                                    settingsBtnW, settingsBtnH, 4,
                                    kColorWhite);
        pd->graphics->drawRoundRect(settingsBtnX, settingsBtnY + 4,
                                    settingsBtnW, settingsBtnH, 4,
                                    1, kColorBlack);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }

    pd->graphics->setFont((LCDFont*)fontBold);
    const char* settingsLabel = "Settings";
    int settingsLabelW =
        style_get_text_width(fontBold, settingsLabel);
    pd->graphics->drawText(
        settingsLabel, strlen(settingsLabel), kUTF8Encoding,
        settingsBtnX + (settingsBtnW - settingsLabelW) / 2,
        settingsBtnY + 9);
    pd->graphics->setDrawMode(kDrawModeCopy);

    pd->graphics->drawLine(20, settingsBtnY + settingsBtnH + 6,
                           PLUTO_SCREEN_WIDTH - 20,
                           settingsBtnY + settingsBtnH + 6, 1,
                           kColorBlack);

    /* Speed Dial section title */
    int gridStartY = settingsBtnY + settingsBtnH + 14;
    pd->graphics->setFont((LCDFont*)fontBold);
    pd->graphics->drawText("SPEED DIAL / BOOKMARKS", 21, kASCIIEncoding,
                           20, gridStartY);
    pd->graphics->drawLine(20, gridStartY + 16, PLUTO_SCREEN_WIDTH - 20,
                           gridStartY + 16, 1, kColorBlack);

    /* Speed dial 2-column grid */
    const int cardW = 172, cardH = 46, gapX = 16, gapY = 8;
    int cardsStartY = gridStartY + 24;

    for (int i = 1; i <= count; i++) {
        PlutoSavedBookmark* bm =
            (PlutoSavedBookmark*)da_get(bookmarks, (size_t)i - 1);
        if (bm == NULL) continue;

        int col = (i - 1) % 2;
        int row = (i - 1) / 2;
        int cardX = 20 + col * (cardW + gapX);
        int cardY = cardsStartY + row * (cardH + gapY);
        int isSelected = (i == s_selectedIndex);

        if (isSelected) {
            pd->graphics->fillRoundRect(cardX, cardY, cardW, cardH, 5,
                                        kColorBlack);
            pd->graphics->drawRoundRect(cardX + 1, cardY + 1,
                                        cardW - 2, cardH - 2, 4, 1,
                                        kColorWhite);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        } else {
            pd->graphics->fillRoundRect(cardX, cardY, cardW, cardH, 5,
                                        kColorWhite);
            pd->graphics->drawRoundRect(cardX, cardY, cardW, cardH, 5,
                                        1, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeCopy);
        }

        int textAreaW = cardW - 16;
        char key[16];

        const char* title = bm->title[0] != '\0' ? bm->title : bm->url;
        pd->graphics->setFont((LCDFont*)fontBold);
        snprintf(key, sizeof(key), "t%d", i);
        draw_marquee(title, cardX + 8, cardY + 6, textAreaW, fontBold,
                     key);

        const char* desc = bm->desc[0] != '\0' ? bm->desc : bm->url;
        pd->graphics->setFont((LCDFont*)fontSmall);
        snprintf(key, sizeof(key), "d%d", i);
        draw_marquee(desc, cardX + 8, cardY + 24, textAreaW, fontSmall,
                     key);

        pd->graphics->setDrawMode(kDrawModeCopy);
    }

    int bottomY =
        cardsStartY + ((count + 1) / 2) * (cardH + gapY) + 12;
    pd->graphics->setFont((LCDFont*)fontSmall);
    {
        const char* hint =
            "(A) Open  \xE2\x80\xA2  (B) Search/URL  \xE2\x80\xA2  "
            "Menu: Settings";
        pd->graphics->drawText(hint, strlen(hint), kUTF8Encoding, 24,
                               bottomY);
    }

    (void)fontBody;
}

#else /* host build */

void hp_draw(double crankChange) { (void)crankChange; }

#endif
