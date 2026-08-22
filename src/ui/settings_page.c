// settings_page.c — P31: C port of CometBrowser Source/ui/settings_page.lua.
//
// Five rows: Search Engine (cycle NAMES), Browse Mode (toggle), Invert
// Crank (toggle), Image Mode (cycle NAMES order), Clear Cookies (action,
// runs immediately). Save writes staged -> Storage.settings + save() +
// onChange; Cancel discards. Box animates from center, ease-out-cubic.

#include "ui/settings_page.h"

#include <stdio.h>
#include <string.h>

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define SP_HAS_PD 1
#endif

#include "../core/constants.h"
#include "../core/cookie_jar.h"
#include "../core/logger.h"
#include "../core/storage.h"
#include "../render/style.h"

#define SP_ANIM_DURATION_MS 300
#define SP_BORDER 10
#define SP_BOX_RADIUS 10

static PlaydateAPI* s_pd = NULL;

static int s_isOpen = 0;
static int s_selectedIndex = 1;
static char s_prevState[32];
static unsigned s_animStartMs = 0;
static SpOnChangeFn s_onChange = NULL;

/* Staged settings: only applied to Storage on Save (A). */
static struct {
    int searchEngine;
    int mode;
    int invertCrank;
    int imageMode; // PlutoImageMode
} s_staged;

void sp_init_pd(PlaydateAPI* pd) { s_pd = pd; }

void sp_set_on_change(SpOnChangeFn cb) { s_onChange = cb; }

int sp_is_open(void) { return s_isOpen; }
int sp_selected_index(void) { return s_selectedIndex; }
const char* sp_previous_state(void) { return s_prevState; }

const char* sp_staged_engine_name(void) {
    int idx = s_staged.searchEngine;
    if (idx < 1 || idx > PLUTO_SEARCH_ENGINE_COUNT) return "DuckDuckGo";
    return PLUTO_SEARCH_ENGINES[idx - 1].name;
}

const char* sp_staged_mode_label(void) {
    return (s_staged.mode == PLUTO_MODE_RAW_HTML) ? "HTML" : "Reader";
}

const char* sp_staged_invert_label(void) {
    return s_staged.invertCrank ? "On" : "Off";
}

const char* sp_staged_image_label(void) {
    /* Lua: IMAGE_MODE_LABELS[mode] or "Render All" */
    const char* label =
        pluto_image_mode_label((PlutoImageMode)s_staged.imageMode);
    return (label != NULL) ? label : "Render All";
}

void sp_open(const char* prevState) {
    s_isOpen = 1;
    s_selectedIndex = 1;
    snprintf(s_prevState, sizeof(s_prevState), "%s",
             prevState ? prevState : "");

#ifdef SP_HAS_PD
    if (s_pd != NULL) {
        s_animStartMs = s_pd->system->getCurrentTimeMilliseconds();
    }
#endif

    PlutoSettings* st = storage_settings();
    /* Lua snapshot defaults: searchEngine or 1, mode or MODE_READER,
     * invertCrank or false, imageMode or IMAGE_MODE_ALL */
    s_staged.searchEngine = st ? st->searchEngine : 1;
    if (s_staged.searchEngine < 1 ||
        s_staged.searchEngine > PLUTO_SEARCH_ENGINE_COUNT) {
        s_staged.searchEngine = 1;
    }
    s_staged.mode = st ? st->mode : (int)PLUTO_MODE_READER;
    s_staged.invertCrank = st ? st->invertCrank : 0;
    s_staged.imageMode = st ? st->imageMode : (int)PLUTO_IMAGE_MODE_ALL;

    PLUTO_LOG("[P31] SettingsPage.open() previousState=%s", s_prevState);
}

void sp_close(void) {
    s_isOpen = 0;
    PLUTO_LOG("[P31] SettingsPage.close()");
}

/* Row 1: Search Engine cycle — Lua ((v-2+n)%n)+1 / (v%n)+1 */
static void cycle_engine(int dir) {
    const int n = PLUTO_SEARCH_ENGINE_COUNT;
    int v = s_staged.searchEngine;
    if (dir < 0) {
        v = ((v - 2 + n) % n) + 1;
    } else {
        v = (v % n) + 1;
    }
    s_staged.searchEngine = v;
}

/* Row 2: Browse Mode toggle (both directions identical in Lua) */
static void toggle_mode(void) {
    s_staged.mode = (s_staged.mode == (int)PLUTO_MODE_RAW_HTML)
                        ? (int)PLUTO_MODE_READER
                        : (int)PLUTO_MODE_RAW_HTML;
}

/* Row 3: Invert Crank toggle */
static void toggle_invert(void) {
    s_staged.invertCrank = !s_staged.invertCrank;
}

/* Row 4: Image Mode cycle in IMAGE_MODE_NAMES order (enum order) */
static void cycle_image_mode(int dir) {
    const int n = (int)PLUTO_IMAGE_MODE_DISABLED + 1;
    int v = s_staged.imageMode;
    v += (dir < 0) ? -1 : 1;
    if (v < 0) v += n;
    if (v >= n) v -= n;
    s_staged.imageMode = v;
}

/* Row 5: Clear Cookies action (immediate, both directions) */
static void clear_cookies(void) {
    cj_clear();
    PLUTO_LOG("[P31] cookies cleared");
}

static SpAction save_and_close(void) {
    PlutoSettings* st = storage_settings();
    if (st != NULL) {
        st->searchEngine = s_staged.searchEngine;
        st->mode = s_staged.mode;
        st->invertCrank = s_staged.invertCrank;
        st->imageMode = s_staged.imageMode;
    }
    storage_save();
    if (s_onChange != NULL) {
        s_onChange();
    }
    PLUTO_LOG("[P31] saveAndClose: mode=%d imageMode=%d", s_staged.mode,
              s_staged.imageMode);
    sp_close();
    return SP_ACT_SAVED;
}

SpAction sp_handle_input(SpButton btn) {
    if (!s_isOpen) return SP_ACT_NONE;

    switch (btn) {
        case SP_BTN_DOWN:
            s_selectedIndex += 1; // math.min(#options, sel+1)
            if (s_selectedIndex > SP_OPTION_COUNT) {
                s_selectedIndex = SP_OPTION_COUNT;
            }
            break;
        case SP_BTN_UP:
            s_selectedIndex -= 1; // math.max(1, sel-1)
            if (s_selectedIndex < 1) s_selectedIndex = 1;
            break;
        case SP_BTN_LEFT:
            switch (s_selectedIndex) {
                case 1: cycle_engine(-1); break;
                case 2: toggle_mode(); break;
                case 3: toggle_invert(); break;
                case 4: cycle_image_mode(-1); break;
                case 5: clear_cookies(); break;
                default: break;
            }
            break;
        case SP_BTN_RIGHT:
            switch (s_selectedIndex) {
                case 1: cycle_engine(1); break;
                case 2: toggle_mode(); break;
                case 3: toggle_invert(); break;
                case 4: cycle_image_mode(1); break;
                case 5: clear_cookies(); break;
                default: break;
            }
            break;
        case SP_BTN_A:
            if (s_selectedIndex == 5) {
                clear_cookies(); // immediate action row
            } else {
                return save_and_close();
            }
            break;
        case SP_BTN_B:
            PLUTO_LOG("[P31] cancelAndClose: discarding changes");
            sp_close();
            return SP_ACT_CLOSED;
        default:
            break;
    }
    return SP_ACT_NONE;
}

#ifdef SP_HAS_PD

void sp_draw(void) {
    if (!s_isOpen || s_pd == NULL) return;

    const float boxX0 = (float)SP_BORDER;
    const float boxY0 = (float)(SP_BORDER + 10);
    const float boxW = (float)(PLUTO_SCREEN_WIDTH - SP_BORDER * 2);
    const float boxH =
        (float)(PLUTO_SCREEN_HEIGHT - SP_BORDER - (SP_BORDER + 10));
    const float centerX = (float)PLUTO_SCREEN_WIDTH / 2.0f;
    const float centerY = boxY0 + boxH / 2.0f;

    unsigned elapsed =
        s_pd->system->getCurrentTimeMilliseconds() - s_animStartMs;
    float t = (float)elapsed / (float)SP_ANIM_DURATION_MS;
    if (t > 1.0f) t = 1.0f;
    /* ease-out cubic */
    t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    int curW = (int)(boxW * t);
    int curH = (int)(boxH * t);
    if (curW < 1) curW = 1;
    if (curH < 1) curH = 1;
    int curX = (int)(centerX - (float)curW / 2.0f);
    int curY = (int)(centerY - (float)curH / 2.0f);
    int r = (int)((float)SP_BOX_RADIUS * t);

    s_pd->graphics->fillRoundRect((float)curX, (float)curY,
                                  (float)curW, (float)curH, r,
                                  kColorWhite);
    s_pd->graphics->drawRoundRect(curX, curY, curW, curH, r, 1,
                                  kColorBlack);

    if (t <= 0.4f) return;

    int hsz = 0, hlh = 0;
    PlutoFont* fontH = style_get_heading_font(2, &hsz, &hlh);
    int bsz = 0;
    PlutoFont* fontBold = style_get_body_font(1, 0, &bsz);
    PlutoFont* fontSmall = style_get_ui_small_font();

    s_pd->graphics->pushContext(NULL);
    s_pd->graphics->setClipRect(curX + 2, curY + 2, curW - 4, curH - 4);

    int innerX = curX + 16;
    int innerY = curY + 10;
    int innerW = curW - 32;

    s_pd->graphics->setFont(fontH);
    s_pd->graphics->drawText("SETTINGS", 8, kASCIIEncoding, innerX,
                             innerY);
    s_pd->graphics->drawLine(innerX, innerY + 16, innerX + innerW,
                             innerY + 16, 1, kColorBlack);

    static const char* const labels[SP_OPTION_COUNT] = {
        "Search Engine", "Browse Mode", "Invert Crank", "Image Mode",
        "Clear Cookies"
    };

    int itemY = innerY + 24;
    const int itemH = 26;

    for (int i = 1; i <= SP_OPTION_COUNT; i++) {
        int iy = itemY + (i - 1) * (itemH + 4);
        int isSel = (i == s_selectedIndex);

        if (isSel) {
            s_pd->graphics->fillRoundRect(
                (float)(innerX - 4), (float)iy, (float)(innerW + 8),
                (float)itemH, 4, kColorBlack);
            s_pd->graphics->drawRoundRect(innerX - 3, iy + 1,
                                          innerW + 6, itemH - 2, 3, 1,
                                          kColorWhite);
            s_pd->graphics->setDrawMode(kDrawModeFillWhite);
        } else {
            s_pd->graphics->setDrawMode(kDrawModeCopy);
        }

        s_pd->graphics->setFont(fontBold);
        const char* label = labels[i - 1];
        s_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                 innerX + 4, iy + 5);

        /* getValue() */
        const char* val = "";
        switch (i) {
            case 1: val = sp_staged_engine_name(); break;
            case 2: val = sp_staged_mode_label(); break;
            case 3: val = sp_staged_invert_label(); break;
            case 4: val = sp_staged_image_label(); break;
            default: val = ""; break; // Clear Cookies renders ""
        }
        int isAction = (i == 5);

        if (val[0] != '\0') {
            s_pd->graphics->setFont(fontSmall);
            int valW = style_get_text_width(fontSmall, val);
            s_pd->graphics->drawText(val, strlen(val), kUTF8Encoding,
                                     innerX + innerW - valW - 4, iy + 7);
        }

        if (isSel && !isAction) {
            s_pd->graphics->setFont(fontSmall);
            int valW = (int)style_get_text_width(fontSmall, val);
            s_pd->graphics->drawText("<", 1, kASCIIEncoding,
                                     innerX + innerW - valW - 18, iy + 7);
            s_pd->graphics->drawText(">", 1, kASCIIEncoding,
                                     innerX + innerW - 2, iy + 7);
        } else if (isSel && isAction) {
            s_pd->graphics->setFont(fontSmall);
            int w = (int)style_get_text_width(fontSmall, "Press A");
            s_pd->graphics->drawText("Press A", 7, kASCIIEncoding,
                                     innerX + innerW - w - 4, iy + 7);
        }

        s_pd->graphics->setDrawMode(kDrawModeCopy);
    }

    int footerY = itemY + SP_OPTION_COUNT * (itemH + 4) + 8;
    s_pd->graphics->setFont(fontSmall);
    s_pd->graphics->drawText("(B) Cancel  *  (A) Save & Close", 30,
                             kASCIIEncoding, innerX, footerY);

    s_pd->graphics->popContext();
}

#else /* host build */

void sp_draw(void) {}

#endif
