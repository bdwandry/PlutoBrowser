#ifndef PLUTO_UI_HUD_H
#define PLUTO_UI_HUD_H

#include <stddef.h>

struct PlaydateAPI;

/* C port of Source/ui/hud.lua (Hud).
 *
 * Faithful parity notes:
 *  - draw(scrollY, totalHeight, activeLink): scrollbar only when
 *    totalHeight > CONTENT_HEIGHT; track inset 2px from right edge and
 *    2px from content top/bottom, thumb min height 12px. activeLink uses
 *    only its href in the Lua body, so the C port takes the string;
 *    bottom bar is SCREEN_HEIGHT-20.. with "-> " prefix clipped at 56.
 *  - drawHoverStatus(url): halved bold font pill bottom-left over the
 *    content area, 6px padding, label clipped at 50 chars; empty url
 *    skips entirely.
 *  - hoverFont loads fonts/Roobert-10-Bold-Halved once (Lua pcall) and
 *    falls back to gfx.getFont() -> system font here.
 */

void hud_init(struct PlaydateAPI* pd);

/* Pure helpers mirroring Hud.draw internals (host-testable). */
void hd_scrollbar_thumb(double totalHeight, int scrollY,
                        int* thumbY, int* thumbH);
void hd_format_link(const char* href, char* out, size_t cap);
void hd_format_hover(const char* url, char* out, size_t cap);

/* device/sim only: no-ops on host builds */
void hud_draw(int scrollY, double totalHeight, const char* activeHref);
void hud_draw_hover_status(const char* url);

#endif
