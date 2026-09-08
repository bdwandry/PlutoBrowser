/*
 * PlutoBrowser — style.c
 * Typography & styling (port of Source/render/style.lua). See style.h.
 * Reference behavior preserved:
 *   - heading1 = Roobert-20-Medium, heading2 = heading3 = bodyBold =
 *     Roobert-10-Bold, body = Roobert-11-Medium, mono = Roobert-11-Mono-
 *     Condensed; every load falls back to the system font on failure.
 *   - getTextWidth: empty → 0; fallback estimate len*8.
 *   - getHeadingFont: (24,6) / (18,5) / (16,4); getBodyFont: mono 15,
 *     bold 16, regular 16; getInlineFont: code 15, small/sub/sup 14,
 *     bold/big 16, regular 16.
 */
#include <string.h>

#include "render/style.h"

extern PlaydateAPI *pluto_pd(void);

static LCDFont *g_fonts[PLUTO_FONT_COUNT];    /* Lua: font.new("fonts/...") wrapped in pcall → NULL fallback here. */
static LCDFont *load_or_null(const char *path)
{
    const char *err = NULL;
    LCDFont *f = pluto_pd()->graphics->loadFont(path, &err);
    return f; /* NULL when the font failed to load (err set) */
}

static const char *system_font_path(void)
{
    /* Roobert is the Playdate system family; system fonts live in
     * /System/Fonts (SDK: Disk/System/Fonts, .pft files). */
    return "/System/Fonts/Roobert-11-Medium";
}

void style_init(PlaydateAPI *pd)
{
    (void)pd;

    g_fonts[PLUTO_FONT_HEADING1] = load_or_null("fonts/Roobert-20-Medium");
    g_fonts[PLUTO_FONT_HEADING2] = load_or_null("fonts/Roobert-10-Bold");
    g_fonts[PLUTO_FONT_HEADING3] = load_or_null("fonts/Roobert-10-Bold");
    g_fonts[PLUTO_FONT_BODY] = load_or_null("fonts/Roobert-11-Medium");
    g_fonts[PLUTO_FONT_BODY_BOLD] = load_or_null("fonts/Roobert-10-Bold");
    g_fonts[PLUTO_FONT_MONO] = load_or_null("fonts/Roobert-11-Mono-Condensed");
    /* Lua: fontSmall stays nil → system font. */
    g_fonts[PLUTO_FONT_SMALL] = load_or_null(system_font_path());

    /* Fill any failures with the system font (Lua: sysFont fallback). The C
     * API has no getFont(); loading the system Roobert path is the same
     * font. If even that fails, roles stay NULL and getTextWidth falls back
     * to the Lua reference's len*8 estimate. */
    const char *err = NULL;
    LCDFont *sys = pluto_pd()->graphics->loadFont(system_font_path(), &err);
    for (int i = 0; i < PLUTO_FONT_COUNT; i++)
    {
        if (!g_fonts[i])
        {
            g_fonts[i] = sys;
        }
    }
}

LCDFont *style_font(PlutoFontRole role)
{
    if (role < 0 || role >= PLUTO_FONT_COUNT)
    {
        return NULL;
    }
    return g_fonts[role];
}

int style_get_text_width(PlutoFontRole role, const char *text)
{
    if (!text || text[0] == '\0')
    {
        return 0;
    }
    LCDFont *font = style_font(role);
    if (font)
    {
        return pluto_pd()->graphics->getTextWidth(font, text, strlen(text),
                                                  kUTF8Encoding, 0);
    }
    size_t n = strlen(text);
    return (int)(n * 8);
}

LCDFont *style_get_heading_font(int level, int *lineHeight, int *marginTopOut)
{
    if (level == 1)
    {
        if (lineHeight)
        {
            *lineHeight = 24;
        }
        if (marginTopOut)
        {
            *marginTopOut = 6;
        }
        return style_font(PLUTO_FONT_HEADING1);
    }
    if (level == 2)
    {
        if (lineHeight)
        {
            *lineHeight = 18;
        }
        if (marginTopOut)
        {
            *marginTopOut = 5;
        }
        return style_font(PLUTO_FONT_HEADING2);
    }
    if (lineHeight)
    {
        *lineHeight = 16;
    }
    if (marginTopOut)
    {
        *marginTopOut = 4;
    }
    return style_font(PLUTO_FONT_HEADING3);
}

LCDFont *style_get_body_font(int isBold, int isCode, int *lineHeight)
{
    if (isCode)
    {
        if (lineHeight)
        {
            *lineHeight = 15;
        }
        return style_font(PLUTO_FONT_MONO);
    }
    if (lineHeight)
    {
        *lineHeight = 16;
    }
    return style_font(isBold ? PLUTO_FONT_BODY_BOLD : PLUTO_FONT_BODY);
}

LCDFont *style_get_inline_font(int isBold, int isCode, int isSmall, int isSub,
                               int isSup, int isBig, int *lineHeight)
{
    (void)isSub;
    (void)isSup;
    (void)isBig;
    if (isCode)
    {
        if (lineHeight)
        {
            *lineHeight = 15;
        }
        return style_font(PLUTO_FONT_MONO);
    }
    if (isSmall || isSub || isSup)
    {
        if (lineHeight)
        {
            *lineHeight = 14;
        }
        return style_font(PLUTO_FONT_SMALL);
    }
    if (lineHeight)
    {
        *lineHeight = 16;
    }
    return style_font((isBold || isBig) ? PLUTO_FONT_BODY_BOLD : PLUTO_FONT_BODY);
}

void style_test_set_fonts(LCDFont *body, LCDFont *bold, LCDFont *mono,
                          LCDFont *small)
{
    style_test_set_fonts_ex(body, bold, mono, small, bold, bold, bold);
}

void style_test_set_fonts_ex(LCDFont *body, LCDFont *bold, LCDFont *mono,
                             LCDFont *small, LCDFont *h1, LCDFont *h2,
                             LCDFont *h3)
{
    g_fonts[PLUTO_FONT_BODY] = body;
    g_fonts[PLUTO_FONT_BODY_BOLD] = bold;
    g_fonts[PLUTO_FONT_MONO] = mono;
    g_fonts[PLUTO_FONT_SMALL] = small;
    g_fonts[PLUTO_FONT_HEADING1] = h1;
    g_fonts[PLUTO_FONT_HEADING2] = h2;
    g_fonts[PLUTO_FONT_HEADING3] = h3;
}
