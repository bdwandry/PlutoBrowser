#ifndef PLUTO_RENDER_STYLE_H
#define PLUTO_RENDER_STYLE_H

/* C port of Source/render/style.lua (Style).
 *
 * Fonts are looked up at init; any missing slot falls back to the system
 * font (gfx.getFont() in Lua -- main.c installs its body font as the
 * current font, so that same font is passed here as the fallback).
 * When built WITHOUT the Playdate API (host test harness), every slot
 * stays NULL and width measuring degrades to the Lua last resort of
 * 8 px per character.
 */

struct PlaydateAPI;

/* opaque font handle (really LCDFont*) */
typedef void PlutoFont;

void style_init(struct PlaydateAPI* pd);

/* system-font fallback (call once after loading, mirrors gfx.getFont()) */
void style_set_system_font(PlutoFont* f);

/* Style.getTextWidth: nil/"" -> 0; font arg may be NULL (body fallback);
 * final fallback = 8 px per char */
int  style_get_text_width(PlutoFont* font, const char* text);

/* returns font, fills lineHeight (+size for headings):
 * level 1 -> h1 24, level 2 -> h2 18, else h3 16 */
PlutoFont* style_get_heading_font(int level, int* size, int* lineHeight);

/* code -> mono 15; bold -> bold 16; else body 16 */
PlutoFont* style_get_body_font(int isBold, int isCode, int* size);

/* precedence: code > small/sub/sup > bold/big > regular */
PlutoFont* style_get_inline_font(int isBold, int isCode, int isSmall,
                                 int isSub, int isSup, int isBig,
                                 int* size);

#endif
