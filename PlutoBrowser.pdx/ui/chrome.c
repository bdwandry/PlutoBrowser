// chrome.c — C port of Source/ui/chrome.lua (Chrome).
//
// Top navigation bar: SSL/globe icon, host display, reader badge, loading
// animation + progress bar or clock. See chrome.h for parity notes.

#include "ui/chrome.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../core/constants.h"
#include "../core/logger.h"
#include "../render/style.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_CH_PD 1
#endif

static int s_cometAnimFrame = 0;
/* The C SDK has no synchronous local-clock read (Lua playdate.getTime()
 * does); rendering uses libc localtime_r on host builds and shows the
 * Lua pcall-fallback "--:--" when no clock is available. */
static int s_haveClock = 0;

#ifdef PLUTO_CH_PD
#include "pd_api.h"
static PlaydateAPI* s_pd = NULL;

void chrome_init(struct PlaydateAPI* pd) {
    s_pd = pd;
    time_t now = time(NULL);
    s_haveClock = (now != (time_t)-1 && now > 0);
}
#else
void chrome_init(struct PlaydateAPI* pd) { (void)pd; }
#endif

/* Lua gfx.fillCircleAtPoint / drawCircleAtPoint equivalents. */
#define PLUTO_TAU 6.2831853f
static void pd_fill_circle(PlaydateAPI* pd, int cx, int cy, int r,
                           LCDColor color) {
    pd->graphics->fillEllipse(cx - r, cy - r, r * 2, r * 2, 0.0f,
                              PLUTO_TAU, color);
}
static void pd_stroke_circle(PlaydateAPI* pd, int cx, int cy, int r,
                             LCDColor color) {
    pd->graphics->drawEllipse(cx - r, cy - r, r * 2, r * 2, 1, 0.0f,
                              PLUTO_TAU, color);
}

int ch_comet_group(int animFrame) {
    int phase = animFrame % 12;
    if (phase < 4) return 0;
    if (phase < 8) return 1;
    return 2;
}

double ch_progress_ratio(long progressCur, long progressTot,
                         int animFrame) {
    if (progressTot > 0) {
        double pct = (double)progressCur / (double)progressTot;
        if (pct > 1.0) pct = 1.0;
        if (pct < 0.0) pct = 0.0; /* Lua cur<0 would go negative; draw
                                     clamps by floor of width anyway */
        return pct;
    }
    return (double)((animFrame * 3) % 100) / 100.0;
}

void ch_display_host(const PlutoUrl* urlObj, char* out, size_t cap) {
    const char* displayHost = "CometBrowser";
    char aboutBuf[PLUTO_URL_HOST_MAX + 8];

    if (urlObj != NULL) {
        if (strcmp(urlObj->scheme, "about") == 0) {
            /* "about:" .. (urlObj.host or "home") */
            snprintf(aboutBuf, sizeof(aboutBuf), "about:%s",
                     urlObj->host[0] != '\0' ? urlObj->host : "home");
            displayHost = aboutBuf;
        } else if (urlObj->host[0] != '\0') {
            displayHost = urlObj->host;
        }
    }

    size_t len = strlen(displayHost);
    if (len > 28) len = 25; /* string.sub(1,25) .. "..." */

    if (len + 3 >= cap) len = (cap > 4) ? cap - 4 : 0;
    memcpy(out, displayHost, len);
    if (strlen(displayHost) > 28) {
        memcpy(out + len, "...", 3);
        out[len + 3] = '\0';
    } else {
        out[len] = '\0';
    }
}

void ch_draw(const PlutoUrl* urlObj, const char* pageTitle,
             int isLoading, long progressCur, long progressTot,
             int isReaderMode) {
#ifndef PLUTO_CH_PD
    (void)urlObj;
    (void)pageTitle;
    (void)isLoading;
    (void)progressCur;
    (void)progressTot;
    (void)isReaderMode;
#else
    if (s_pd == NULL) return;
    PlaydateAPI* pd = s_pd;

    s_cometAnimFrame++;

    /* Top bar background */
    pd->graphics->fillRect(0, 0, PLUTO_SCREEN_WIDTH, PLUTO_CHROME_HEIGHT,
                           kColorBlack);

    /* White bottom separator line */
    pd->graphics->drawLine(0, PLUTO_CHROME_HEIGHT - 1, PLUTO_SCREEN_WIDTH,
                           PLUTO_CHROME_HEIGHT - 1, 1, kColorWhite);

    /* 1. SSL lock icon or globe icon */
    int isSsl = (urlObj != NULL) ? urlObj->isSsl : 0;
    if (isSsl) {
        pd->graphics->drawRoundRect(6, 6, 8, 8, 3, 1, kColorWhite);
        pd->graphics->fillRect(5, 10, 10, 8, kColorWhite);
        pd_fill_circle(pd, 10, 14, 1, kColorBlack);
    } else {
        pd_stroke_circle(pd, 10, 13, 6, kColorWhite);
        pd->graphics->drawLine(4, 13, 16, 13, 1, kColorWhite);
        pd->graphics->drawLine(10, 7, 10, 19, 1, kColorWhite);
    }

    /* 2. URL / title text */
    int sz;
    PlutoFont* font = style_get_body_font(1, 0, &sz);
    PlutoFont* fontSmall = style_get_ui_small_font();
    pd->graphics->setFont((LCDFont*)font);
    pd->graphics->setDrawMode(kDrawModeFillWhite);

    char displayHost[PLUTO_URL_HOST_MAX + 16];
    ch_display_host(urlObj, displayHost, sizeof(displayHost));
    pd->graphics->drawText(displayHost, strlen(displayHost),
                           kASCIIEncoding, 22, 4);

    /* 3. Reader mode indicator badge */
    if (urlObj != NULL && strcmp(urlObj->scheme, "about") != 0 &&
        !isLoading) {
        const char* badgeText = isReaderMode ? "[READ]" : "[WEB]";
        pd->graphics->setFont((LCDFont*)fontSmall);
        int badgeW =
            style_get_text_width(fontSmall, badgeText);
        pd->graphics->drawText(badgeText, strlen(badgeText),
                               kASCIIEncoding,
                               PLUTO_SCREEN_WIDTH - badgeW - 62, 6);
    }

    /* 4. Right status: loading animation or time */
    if (isLoading) {
        int baseX = PLUTO_SCREEN_WIDTH - 18;
        int baseY = 13;
        switch (ch_comet_group(s_cometAnimFrame)) {
        case 0:
            pd_fill_circle(pd, baseX, baseY, 3, kColorWhite);
            break;
        case 1:
            pd_fill_circle(pd, baseX - 4, baseY, 2, kColorWhite);
            pd_fill_circle(pd, baseX, baseY, 1, kColorWhite);
            break;
        default:
            pd_fill_circle(pd, baseX - 8, baseY, 1, kColorWhite);
            pd_fill_circle(pd, baseX - 4, baseY, 2, kColorWhite);
            pd_fill_circle(pd, baseX, baseY, 3, kColorWhite);
            break;
        }

        double pct =
            ch_progress_ratio(progressCur, progressTot, s_cometAnimFrame);
        int barW = (int)((double)PLUTO_SCREEN_WIDTH * pct);
        pd->graphics->fillRect(0, PLUTO_CHROME_HEIGHT - 2, barW, 2,
                               kColorWhite);
    } else {
        /* Lua: pcall(playdate.getTime()) with "--:--" fallback. The C SDK
         * exposes no synchronous local-clock read; on host builds we use
         * libc localtime_r (matches playdate.getTime()), otherwise the
         * fallback shows. */
        char timeFormatted[8];
        const char* shown = timeFormatted;
        time_t now = time(NULL);
        struct tm lt;
        if (!s_haveClock || now == (time_t)-1 ||
            localtime_r(&now, &lt) == NULL)
            shown = "--:--";
        else
            snprintf(timeFormatted, sizeof(timeFormatted), "%02u:%02u",
                     (unsigned)lt.tm_hour, (unsigned)lt.tm_min);
        int timeW = style_get_text_width(font, shown);
        pd->graphics->setFont((LCDFont*)font);
        pd->graphics->drawText(shown, strlen(shown), kASCIIEncoding,
                               PLUTO_SCREEN_WIDTH - timeW - 6, 4);
        { static int logged = 0;
          if (!logged) {
              logged = 1;
              PLUTO_LOG("[P27] chrome clock renders %s", shown);
          } }
    }

    pd->graphics->setDrawMode(kDrawModeCopy);
#endif
}
