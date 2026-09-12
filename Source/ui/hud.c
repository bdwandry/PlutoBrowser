/*
 * PlutoBrowser — hud.c
 * Floating HUD & scrollbar (port of Source/ui/hud.lua). See hud.h.
 * Geometry, thumb sizing (min 12px), scroll clamping, and text truncation
 * lengths (56 / 50 chars) preserved from the reference.
 */
#include <stdio.h>
#include <string.h>

#include "ui/hud.h"
#include "core/constants.h"
#include "render/style.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

void hud_draw(int scrollY, int totalHeight, const char *activeLinkHref)
{
    PlaydateAPI *pd = pluto_pd();

    if (totalHeight > CONTENT_HEIGHT)
    {
        int trackX = SCREEN_WIDTH - SCROLLBAR_WIDTH - 2;
        int trackY = CONTENT_Y + 2;
        int trackH = CONTENT_HEIGHT - 4;

        pd->graphics->drawRect(trackX, trackY, SCROLLBAR_WIDTH, trackH, kColorBlack);

        int thumbH = trackH * CONTENT_HEIGHT / totalHeight;
        if (thumbH < 12)
        {
            thumbH = 12;
        }
        int maxScroll = totalHeight - CONTENT_HEIGHT;
        int scrollRatioPct = clampi((int)((float)scrollY / (float)maxScroll * 1000.0f),
                                    0, 1000);
        int thumbY = trackY + (trackH - thumbH) * scrollRatioPct / 1000;

        pd->graphics->fillRect(trackX + 1, thumbY, SCROLLBAR_WIDTH - 2, thumbH,
                               kColorBlack);
    }

    if (activeLinkHref && activeLinkHref[0])
    {
        int hudH = 20;
        int hudY = SCREEN_HEIGHT - hudH;

        pd->graphics->fillRect(0, hudY, SCREEN_WIDTH, hudH, kColorBlack);
        pd->graphics->drawLine(0, hudY, SCREEN_WIDTH, hudY, 1, kColorWhite);

        pd->graphics->setDrawMode(kDrawModeFillWhite);
        pd->graphics->setFont(style_font(PLUTO_FONT_SMALL));

        char linkText[512 + 8]; /* fullPath ≤511 + "-> " */
        snprintf(linkText, sizeof(linkText), "-> %s", activeLinkHref);
        char trunc[sizeof(linkText)];
        if (strlen(linkText) > 56)
        {
            snprintf(trunc, sizeof(trunc), "%.53s...", linkText);
        }
        else
        {
            snprintf(trunc, sizeof(trunc), "%s", linkText);
        }
        pd->graphics->drawText(trunc, strlen(trunc), kUTF8Encoding, 8, hudY + 3);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
}

void hud_draw_hover_status(const char *url)
{
    if (!url || !url[0])
    {
        return;
    }
    PlaydateAPI *pd = pluto_pd();

    char label[64];
    if (strlen(url) > 50)
    {
        snprintf(label, sizeof(label), "%.47s...", url);
    }
    else
    {
        snprintf(label, sizeof(label), "%s", url);
    }

    LCDFont *font = style_font(PLUTO_FONT_SMALL);
    int fontH = pd->graphics->getFontHeight(font);
    int tw = style_get_text_width(PLUTO_FONT_SMALL, label);
    int pad = 6;
    int barW = SCREEN_WIDTH;
    if (tw + pad * 2 < barW)
    {
        barW = tw + pad * 2;
    }
    int barH = fontH + 4;
    int barX = 0;
    int barY = CONTENT_Y + CONTENT_HEIGHT - barH;

    pd->graphics->fillRect(barX, barY, barW, barH, kColorBlack);

    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->setFont(font);
    pd->graphics->drawText(label, strlen(label), kUTF8Encoding, barX + pad, barY + 2);
    pd->graphics->setDrawMode(kDrawModeCopy);
}
