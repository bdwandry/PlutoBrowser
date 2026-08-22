// error_page.c — P30: C port of CometBrowser Source/ui/error_page.lua.
//
// "Connection Failed" dialog: heading, clipped message (48->45+"...") and
// target URL (50->47+"..."), hint line, and three 100x28 buttons
// (Try Again / Search Web / Go Home) with inverted selection.

#include "ui/error_page.h"

#include <stdio.h>
#include <string.h>

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define EP_HAS_PD 1
#endif

#include "../core/constants.h"
#include "../core/url.h"
#include "../render/style.h"

static int s_selectedIndex = 1;
static char s_errorMsg[128];
static char s_failedUrl[PLUTO_URL_FULLPATH_MAX];

void ep_show(const char* errorMsg, const char* failedUrl) {
    snprintf(s_errorMsg, sizeof(s_errorMsg), "%s",
             (errorMsg != NULL && errorMsg[0] != '\0') ? errorMsg
                                                       : "Unable to load webpage");
    snprintf(s_failedUrl, sizeof(s_failedUrl), "%s",
             failedUrl ? failedUrl : "");
    s_selectedIndex = 1;
}

int ep_selected_index(void) { return s_selectedIndex; }

const char* ep_error_msg(void) { return s_errorMsg; }
const char* ep_failed_url(void) { return s_failedUrl; }

EpAction ep_handle_input(EpButton btn) {
    if (btn == EP_BTN_LEFT || btn == EP_BTN_UP) {
        s_selectedIndex -= 1; // Lua math.max(1, sel-1)
        if (s_selectedIndex < 1) s_selectedIndex = 1;
        return EP_ACT_NONE;
    }
    if (btn == EP_BTN_RIGHT || btn == EP_BTN_DOWN) {
        s_selectedIndex += 1; // Lua math.min(3, sel+1)
        if (s_selectedIndex > 3) s_selectedIndex = 3;
        return EP_ACT_NONE;
    }
    if (btn == EP_BTN_A) {
        switch (s_selectedIndex) {
            case 1: return EP_ACT_RETRY;
            case 2: return EP_ACT_SEARCH;
            case 3: return EP_ACT_HOME;
            default: break;
        }
    }
    return EP_ACT_NONE;
}

#ifdef EP_HAS_PD

static PlaydateAPI* s_pd = NULL;

void ep_init_pd(PlaydateAPI* pd) { s_pd = pd; }

/* Lua clips #msg>48 -> sub(1,45)+"..." and #u>50 -> sub(1,47)+"..." */
static void clip_ellipsis(char* out, size_t cap, const char* s,
                          size_t limit, size_t keep) {
    snprintf(out, cap, "%s", s ? s : "");
    if (strlen(out) > limit) {
        memcpy(out + keep, "...", 4);
    }
}

void ep_draw(void) {
    if (s_pd == NULL) return;

    int hsz = 0, hlh = 0;
    PlutoFont* fontH = style_get_heading_font(1, &hsz, &hlh);
    int bsz = 0;
    PlutoFont* fontB = style_get_body_font(0, 0, &bsz);
    PlutoFont* fontBold = style_get_body_font(1, 0, &bsz);
    PlutoFont* fontSmall = style_get_ui_small_font();

    static char clipped[PLUTO_URL_FULLPATH_MAX];

    int startY = PLUTO_CONTENT_Y + 16;

    s_pd->graphics->drawRoundRect(20, startY, PLUTO_SCREEN_WIDTH - 40,
                                  120, 6, 1, kColorBlack);

    s_pd->graphics->setFont(fontH);
    s_pd->graphics->drawText("Connection Failed", 17, kASCIIEncoding,
                             36, startY + 12);

    s_pd->graphics->setFont(fontBold);
    clip_ellipsis(clipped, sizeof(clipped), s_errorMsg, 48, 45);
    s_pd->graphics->drawText(clipped, strlen(clipped), kUTF8Encoding,
                             36, startY + 40);

    s_pd->graphics->setFont(fontSmall);
    clip_ellipsis(clipped, sizeof(clipped), s_failedUrl, 50, 47);
    {
        const char* prefix = "Target: ";
        char line[PLUTO_URL_FULLPATH_MAX + 16];
        snprintf(line, sizeof(line), "%s%s", prefix, clipped);
        s_pd->graphics->drawText(line, strlen(line), kUTF8Encoding,
                                 36, startY + 60);
    }

    s_pd->graphics->setFont(fontB);
    s_pd->graphics->drawText(
        "Check your Wi-Fi or try searching the web.", 42,
        kASCIIEncoding, 36, startY + 82);

    static const char* const buttons[3] = { "Try Again", "Search Web",
                                            "Go Home" };
    const int btnW = 100, btnH = 28;
    int btnY = startY + 136;
    int startX = 36;

    for (int i = 1; i <= 3; i++) {
        int btnX = startX + (i - 1) * (btnW + 16);
        int isSel = (i == s_selectedIndex);

        if (isSel) {
            s_pd->graphics->fillRoundRect((float)btnX, (float)btnY,
                                          (float)btnW, (float)btnH, 4,
                                          kColorBlack);
            s_pd->graphics->drawRoundRect(btnX + 1, btnY + 1, btnW - 2,
                                          btnH - 2, 3, 1, kColorWhite);
            s_pd->graphics->setDrawMode(kDrawModeFillWhite);
        } else {
            s_pd->graphics->fillRoundRect((float)btnX, (float)btnY,
                                          (float)btnW, (float)btnH, 4,
                                          kColorWhite);
            s_pd->graphics->drawRoundRect(btnX, btnY, btnW, btnH, 4, 1,
                                          kColorBlack);
            s_pd->graphics->setDrawMode(kDrawModeCopy);
        }

        s_pd->graphics->setFont(fontBold);
        const char* label = buttons[i - 1];
        int tw = style_get_text_width(fontBold, label);
        s_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                 btnX + (btnW - tw) / 2, btnY + 6);
        s_pd->graphics->setDrawMode(kDrawModeCopy);
    }
}

#else /* host build */

void ep_init_pd(PlaydateAPI* pd) { (void)pd; }
void ep_draw(void) {}

#endif
