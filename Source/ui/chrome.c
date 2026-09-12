/*
 * PlutoBrowser — chrome.c
 * Top chrome / navigation toolbar (port of Source/ui/chrome.lua).
 * Every element preserved: black bar + white separator, SSL padlock or globe
 * icon, host/title text (28-char truncation), [READ]/[WEB] badge, animated
 * comet loading dots + progress bar (or the clock), white-fill text draw mode.
 */
#include <stdio.h>
#include <string.h>

#include "ui/chrome.h"
#include "core/constants.h"
#include "render/style.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

static int g_cometAnimFrame = 0;

/* Lua string.sub(text, 1, 25) .. "..." equivalent (byte-based, as in Lua). */
static void truncate_ellipsis(const char *in, char *out, size_t cap,
                              size_t keep)
{
    size_t len = strlen(in);
    if (len <= keep + 3)
    {
        snprintf(out, cap, "%s", in);
        return;
    }
    snprintf(out, cap, "%.*s...", (int)keep, in);
}

void chrome_draw(const UrlParsed *urlObj, const char *pageTitle, int isLoading,
                 float progressCur, float progressTot, int isReaderMode)
{
    (void)pageTitle; /* the Lua reference receives it but renders host/about */
    PlaydateAPI *pd = pluto_pd();
    g_cometAnimFrame++;

    /* Top bar background + white bottom separator line */
    pd->graphics->fillRect(0, 0, SCREEN_WIDTH, CHROME_HEIGHT, kColorBlack);
    pd->graphics->drawLine(0, CHROME_HEIGHT - 1, SCREEN_WIDTH, CHROME_HEIGHT - 1,
                           1, kColorWhite);

    /* 1. SSL lock icon or globe icon */
    int isSsl = urlObj && urlObj->isSsl;
    if (isSsl)
    {
        /* padlock: round body + shackle */
        pd->graphics->drawRoundRect(6, 6, 8, 8, 3, 1, kColorWhite);
        pd->graphics->fillRect(5, 10, 10, 8, kColorWhite);
        pd->graphics->fillEllipse(10 - 1, 14 - 1, 3, 3, 0.0f, 360.0f, kColorBlack);
    }
    else
    {
        /* globe: circle + equator + meridian */
        pd->graphics->drawEllipse(10 - 6, 13 - 6, 13, 13, 1, 0.0f, 360.0f, kColorWhite);
        pd->graphics->drawLine(4, 13, 16, 13, 1, kColorWhite);
        pd->graphics->drawLine(10, 7, 10, 19, 1, kColorWhite);
    }

    /* 2. URL / title text */
    LCDFont *font = style_font(PLUTO_FONT_BODY_BOLD);
    LCDFont *fontSmall = style_font(PLUTO_FONT_SMALL);
    pd->graphics->setFont(font);
    pd->graphics->setDrawMode(kDrawModeFillWhite);

    char displayHost[128 + 8] = "PlutoBrowser"; /* BF8 rebrand; host ≤127 + "about:" */
    if (urlObj)
    {
        if (strcmp(urlObj->scheme, "about") == 0)
        {
            snprintf(displayHost, sizeof(displayHost), "about:%s",
                     urlObj->host[0] ? urlObj->host : "home");
        }
        else if (urlObj->host[0])
        {
            snprintf(displayHost, sizeof(displayHost), "%s", urlObj->host);
        }
    }
    char hostTrunc[sizeof(displayHost)];
    truncate_ellipsis(displayHost, hostTrunc, sizeof(hostTrunc), 25);
    pd->graphics->drawText(hostTrunc, strlen(hostTrunc), kUTF8Encoding, 22, 4);

    /* 3. Reader-mode badge */
    if (urlObj && strcmp(urlObj->scheme, "about") != 0 && !isLoading)
    {
        const char *badgeText = isReaderMode ? "[READ]" : "[WEB]";
        pd->graphics->setFont(fontSmall);
        int badgeW = style_get_text_width(PLUTO_FONT_SMALL, badgeText);
        pd->graphics->drawText(badgeText, strlen(badgeText), kUTF8Encoding,
                               SCREEN_WIDTH - badgeW - 62, 6);
    }

    /* 4. Right status: loading animation or time */
    if (isLoading)
    {
        int baseX = SCREEN_WIDTH - 18;
        int baseY = 13;
        int phase = g_cometAnimFrame % 12;
        if (phase < 4)
        {
            pd->graphics->fillEllipse(baseX - 3, baseY - 3, 7, 7, 0.0f, 360.0f, kColorWhite);
        }
        else if (phase < 8)
        {
            pd->graphics->fillEllipse(baseX - 4 - 2, baseY - 2, 5, 5, 0.0f, 360.0f, kColorWhite);
            pd->graphics->fillEllipse(baseX - 1, baseY - 1, 3, 3, 0.0f, 360.0f, kColorWhite);
        }
        else
        {
            pd->graphics->fillEllipse(baseX - 8 - 1, baseY - 1, 3, 3, 0.0f, 360.0f, kColorWhite);
            pd->graphics->fillEllipse(baseX - 4 - 2, baseY - 2, 5, 5, 0.0f, 360.0f, kColorWhite);
            pd->graphics->fillEllipse(baseX - 3, baseY - 3, 7, 7, 0.0f, 360.0f, kColorWhite);
        }

        /* Progress bar */
        float pct = 0.0f;
        if (progressTot > 0.0f)
        {
            pct = progressCur / progressTot;
            if (pct > 1.0f)
            {
                pct = 1.0f;
            }
        }
        else
        {
            pct = (float)((g_cometAnimFrame * 3) % 100) / 100.0f;
        }
        int barW = (int)(SCREEN_WIDTH * pct);
        pd->graphics->fillRect(0, CHROME_HEIGHT - 2, barW, 2, kColorWhite);
    }
    else
    {
        char timeFormatted[8] = "--:--";
        unsigned int ms = 0;
        uint32_t epoch = pd->system->getSecondsSinceEpoch(&ms);
        int tz = pd->system->getTimezoneOffset();
        struct PDDateTime dt;
        pd->system->convertEpochToDateTime(epoch + (uint32_t)(tz * 60), &dt);
        snprintf(timeFormatted, sizeof(timeFormatted), "%02d:%02d", dt.hour, dt.minute);

        int timeW = style_get_text_width(PLUTO_FONT_BODY_BOLD, timeFormatted);
        pd->graphics->setFont(font);
        pd->graphics->drawText(timeFormatted, strlen(timeFormatted), kUTF8Encoding,
                               SCREEN_WIDTH - timeW - 6, 4);
    }

    pd->graphics->setDrawMode(kDrawModeCopy);
}
