// hud.c — C port of Source/ui/hud.lua (Hud).
//
// Floating scrollbar + active-link bottom bar + hover status pill.
// See hud.h for parity notes.

#include "ui/hud.h"

#include <stdio.h>
#include <string.h>

#include "../core/constants.h"
#include "../core/url.h"
#include "../render/style.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_HD_PD 1
#endif

#ifdef PLUTO_HD_PD
#include "pd_api.h"
static PlaydateAPI* s_pd = NULL;
static LCDFont* s_hoverFont = NULL;

void hud_init(struct PlaydateAPI* pd) {
    s_pd = pd;
    if (s_pd == NULL) return;
    /* Lua: pcall(font.new("fonts/Roobert-10-Bold-Halved")) or getFont() */
    const char* err = NULL;
    s_hoverFont =
        s_pd->graphics->loadFont("fonts/Roobert-10-Bold-Halved", &err);
}
#else
void hud_init(struct PlaydateAPI* pd) { (void)pd; }
#endif

/* ── pure helpers ─────────────────────────────────────────────────────── */

void hd_scrollbar_thumb(double totalHeight, int scrollY,
                        int* thumbY, int* thumbH) {
    int trackX = PLUTO_SCREEN_WIDTH - PLUTO_SCROLLBAR_WIDTH - 2;
    int trackY = PLUTO_CONTENT_Y + 2;
    int trackH = PLUTO_CONTENT_HEIGHT - 4;
    (void)trackX;

    if (!(totalHeight > (double)PLUTO_CONTENT_HEIGHT)) {
        *thumbY = -1;
        *thumbH = -1;
        return;
    }
    int th = (int)(trackH *
                   ((double)PLUTO_CONTENT_HEIGHT / totalHeight));
    if (th < 12) th = 12;
    double maxScroll = totalHeight - (double)PLUTO_CONTENT_HEIGHT;
    double ratio = (double)scrollY / maxScroll;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    *thumbY = trackY + (int)((double)(trackH - th) * ratio);
    *thumbH = th;
}

/* Shared clip rule: limit -> first (limit-3) chars + "...". */
static void clip_label(const char* src, size_t limit, char* out,
                       size_t cap) {
    if (src == NULL) src = "";
    size_t len = strlen(src);
    if (len <= limit) {
        snprintf(out, cap, "%s", src);
        return;
    }
    size_t keep = limit - 3;
    if (keep + 3 >= cap) keep = (cap > 4) ? cap - 4 : 0;
    memcpy(out, src, keep);
    memcpy(out + keep, "...", 3);
    out[keep + 3] = '\0';
}

void hd_format_link(const char* href, char* out, size_t cap) {
    char buf[PLUTO_URL_FULLPATH_MAX];
    snprintf(buf, sizeof(buf), "-> %s", href ? href : "");
    clip_label(buf, 56, out, cap);
}

void hd_format_hover(const char* url, char* out, size_t cap) {
    clip_label(url ? url : "", 50, out, cap);
}

/* ── drawing ──────────────────────────────────────────────────────────── */

void hud_draw(int scrollY, double totalHeight, const char* activeHref) {
#ifndef PLUTO_HD_PD
    (void)scrollY;
    (void)totalHeight;
    (void)activeHref;
#else
    if (s_pd == NULL) return;
    PlaydateAPI* pd = s_pd;

    if (totalHeight > (double)PLUTO_CONTENT_HEIGHT) {
        int trackX = PLUTO_SCREEN_WIDTH - PLUTO_SCROLLBAR_WIDTH - 2;
        int trackY = PLUTO_CONTENT_Y + 2;
        int trackH = PLUTO_CONTENT_HEIGHT - 4;

        pd->graphics->drawRect(trackX, trackY, PLUTO_SCROLLBAR_WIDTH,
                               trackH, kColorBlack);

        int thumbY, thumbH;
        hd_scrollbar_thumb(totalHeight, scrollY, &thumbY, &thumbH);
        pd->graphics->fillRect(trackX + 1, thumbY,
                               PLUTO_SCROLLBAR_WIDTH - 2, thumbH,
                               kColorBlack);
    }

    if (activeHref != NULL && activeHref[0] != '\0') {
        int hudH = 20;
        int hudY = PLUTO_SCREEN_HEIGHT - hudH;

        pd->graphics->fillRect(0, hudY, PLUTO_SCREEN_WIDTH, hudH,
                               kColorBlack);
        pd->graphics->drawLine(0, hudY, PLUTO_SCREEN_WIDTH, hudY, 1,
                               kColorWhite);

        pd->graphics->setDrawMode(kDrawModeFillWhite);
        PlutoFont* font = style_get_ui_small_font();
        pd->graphics->setFont((LCDFont*)font);

        char linkText[PLUTO_URL_FULLPATH_MAX];
        hd_format_link(activeHref, linkText, sizeof(linkText));
        pd->graphics->drawText(linkText, strlen(linkText),
                               kASCIIEncoding, 8, hudY + 3);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
#endif
}

void hud_draw_hover_status(const char* url) {
#ifndef PLUTO_HD_PD
    (void)url;
#else
    if (s_pd == NULL) return;
    if (url == NULL || url[0] == '\0') return;
    PlaydateAPI* pd = s_pd;

    LCDFont* font = (s_hoverFont != NULL)
                        ? s_hoverFont
                        : (LCDFont*)style_get_ui_small_font();
    int fontH = pd->graphics->getFontHeight(font);
    char label[PLUTO_URL_INPUT_MAX];
    hd_format_hover(url, label, sizeof(label));
    int tw = style_get_text_width((PlutoFont*)font, label);
    int pad = 6;
    int barW = tw + pad * 2;
    if (barW > PLUTO_SCREEN_WIDTH) barW = PLUTO_SCREEN_WIDTH;
    int barH = fontH + 4;
    int barX = 0;
    int barY = PLUTO_CONTENT_Y + PLUTO_CONTENT_HEIGHT - barH;

    pd->graphics->fillRect(barX, barY, barW, barH, kColorBlack);

    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont(font);
    pd->graphics->drawText(label, strlen(label), kASCIIEncoding,
                           barX + pad, barY + 2);
    pd->graphics->setDrawMode(kDrawModeCopy);
#endif
}
