// bookmarks_page.c — P30: C port of CometBrowser Source/ui/bookmarks_page.lua.

#include "ui/bookmarks_page.h"

#include <stdio.h>
#include <string.h>

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define BM_HAS_PD 1
#endif

#include "../core/constants.h"
#include "../core/logger.h"
#include "../core/storage.h"
#include "../render/style.h"
#include "../util/dynarray.h"

static int s_selectedIndex = 1;
static double s_scrollY = 0.0;

void bm_open(void) {
    s_selectedIndex = 1;
    s_scrollY = 0.0;
}

int bm_selected_index(void) { return s_selectedIndex; }
double bm_scroll_y(void) { return s_scrollY; }

LpAction bm_handle_input(LrButton btn, char* outUrl, size_t cap) {
    DynArray* bms = storage_bookmarks();
    int count = bms ? (int)bms->count : 0;

    if (count == 0) {
        if (btn == LR_BTN_B) return LP_ACT_CLOSE;
        return LP_ACT_NONE;
    }

    if (btn == LR_BTN_DOWN || btn == LR_BTN_UP) {
        lr_nav(&s_selectedIndex, btn, count);
        return LP_ACT_NONE;
    }

    if (btn == LR_BTN_A) {
        PlutoSavedBookmark* bm =
            da_get(bms, (size_t)s_selectedIndex - 1);
        if (bm != NULL) {
            if (outUrl != NULL && cap > 0) {
                snprintf(outUrl, cap, "%s", bm->url);
            }
            return LP_ACT_OPEN;
        }
        return LP_ACT_NONE;
    }

    if (btn == LR_BTN_B) return LP_ACT_CLOSE;
    return LP_ACT_NONE;
}

#ifdef BM_HAS_PD

static PlaydateAPI* s_pd = NULL;

void bm_init_pd(PlaydateAPI* pd) { s_pd = pd; }

void bm_draw(double crankChange) {
    if (s_pd == NULL) return;

    DynArray* bms = storage_bookmarks();
    int count = bms ? (int)bms->count : 0;

    int hsz = 0, hlh = 0;
    PlutoFont* fontH = style_get_heading_font(2, &hsz, &hlh);
    int bsz = 0;
    PlutoFont* fontB = style_get_body_font(1, 0, &bsz);
    PlutoFont* fontS = style_get_ui_small_font();

    lr_scroll(&s_scrollY, crankChange);

    int startY = PLUTO_CONTENT_Y + 10 - (int)s_scrollY;

    
    s_pd->graphics->setFont(fontH);
    s_pd->graphics->drawText("BOOKMARKS & FAVORITES", 21,
                             kASCIIEncoding, 16, startY);
    s_pd->graphics->drawLine(16, startY + 18, PLUTO_SCREEN_WIDTH - 16,
                             startY + 18, 1, kColorBlack);

    int itemY = startY + 26;
    const int itemH = 34;

    static char clipped[PLUTO_BM_URL_MAX];

    if (count == 0) {
        s_pd->graphics->setFont(fontB);
        s_pd->graphics->drawText(
            "No bookmarks saved yet. Use Menu to add bookmarks.", 50,
            kASCIIEncoding, 16, itemY);
        return;
    }

    for (int i = 1; i <= count; i++) {
        int isSel = (i == s_selectedIndex);
        int drawY = itemY + (i - 1) * (itemH + 4);

        if (!lr_row_visible((float)drawY, (float)itemH)) continue;

        PlutoSavedBookmark* bm = da_get(bms, (size_t)i - 1);
        if (bm == NULL) continue;

        if (isSel) {
            s_pd->graphics->fillRoundRect(
                16.0f, (float)drawY, (float)(PLUTO_SCREEN_WIDTH - 32),
                (float)itemH, 4, kColorBlack);
            s_pd->graphics->setDrawMode(kDrawModeFillWhite);
        } else {
            s_pd->graphics->fillRoundRect(
                16.0f, (float)drawY, (float)(PLUTO_SCREEN_WIDTH - 32),
                (float)itemH, 4, kColorWhite);
            s_pd->graphics->drawRoundRect(16, drawY,
                                          PLUTO_SCREEN_WIDTH - 32,
                                          itemH, 4, 1, kColorBlack);
            s_pd->graphics->setDrawMode(kDrawModeCopy);
        }

        /* title = bm.title or bm.url */
        const char* rawTitle =
            (bm->title[0] != '\0') ? bm->title : bm->url;
        lr_clip_title(clipped, sizeof(clipped), rawTitle);
        s_pd->graphics->setFont(fontB);
        s_pd->graphics->drawText(clipped, strlen(clipped),
                                 kUTF8Encoding, 24, drawY + 3);

        lr_clip_url(clipped, sizeof(clipped), bm->url);
        s_pd->graphics->setFont(fontS);
        s_pd->graphics->drawText(clipped, strlen(clipped),
                                 kUTF8Encoding, 24, drawY + 18);

        s_pd->graphics->setDrawMode(kDrawModeCopy);
    }
}

#else /* host build */

void bm_init_pd(PlaydateAPI* pd) { (void)pd; }
void bm_draw(double crankChange) { (void)crankChange; }

#endif
