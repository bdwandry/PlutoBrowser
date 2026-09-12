/*
 * PlutoBrowser — style.h
 * Typography & styling (port of Source/render/style.lua).
 *
 * Lua loaded fonts with pcall + system-font fallback; loadFont in C returns
 * NULL on failure, so the fallback chain is: named Roobert font → the system
 * font provided by pd->graphics->getFont() — the identical observable result.
 */
#ifndef PLUTO_STYLE_H
#define PLUTO_STYLE_H

#include "pd_api.h"

typedef enum
{
    PLUTO_FONT_HEADING1 = 0,
    PLUTO_FONT_HEADING2,
    PLUTO_FONT_HEADING3,
    PLUTO_FONT_BODY,
    PLUTO_FONT_BODY_BOLD,
    PLUTO_FONT_MONO,
    PLUTO_FONT_SMALL,
    PLUTO_FONT_COUNT
} PlutoFontRole;

/* Load all fonts (safe to call once at boot). */
void style_init(PlaydateAPI *pd);

/* Font for a role (never NULL after init). */
LCDFont *style_font(PlutoFontRole role);

/* Lua getTextWidth: 0 for empty text; font metrics; last-resort len*8. */
int style_get_text_width(PlutoFontRole role, const char *text);

/* Lua getHeadingFont(level): returns font; lineHeight and marginTopOut set. */
LCDFont *style_get_heading_font(int level, int *lineHeight, int *marginTopOut);

/* Lua getBodyFont(isBold, isCode): returns font; lineHeight set. */
LCDFont *style_get_body_font(int isBold, int isCode, int *lineHeight);

/* Host-test seam: install sentinel font pointers (see p31 battery). */
void style_test_set_fonts(LCDFont *body, LCDFont *bold, LCDFont *mono,
                          LCDFont *small);
void style_test_set_fonts_ex(LCDFont *body, LCDFont *bold, LCDFont *mono,
                             LCDFont *small, LCDFont *h1, LCDFont *h2,
                             LCDFont *h3);

/* Lua getInlineFont(isBold, isCode, isSmall, isSub, isSup, isBig). */
LCDFont *style_get_inline_font(int isBold, int isCode, int isSmall, int isSub,
                               int isSup, int isBig, int *lineHeight);

#endif /* PLUTO_STYLE_H */
