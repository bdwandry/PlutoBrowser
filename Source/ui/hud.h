/*
 * PlutoBrowser — hud.h
 * Floating HUD & scrollbar (port of Source/ui/hud.lua).
 */
#ifndef PLUTO_HUD_H
#define PLUTO_HUD_H

/* Draw scrollbar (when content overflows) and the active-link HUD bar.
 * activeLink may be NULL. */
void hud_draw(int scrollY, int totalHeight, const char *activeLinkHref);

/* Small bottom-left status bar showing a hovered link URL (may be NULL/empty
 * → draws nothing). */
void hud_draw_hover_status(const char *url);

#endif /* PLUTO_HUD_H */
