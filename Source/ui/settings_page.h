/*
 * PlutoBrowser — settings_page.h
 * Settings menu overlay (port of Source/ui/settings_page.lua).
 */
#ifndef PLUTO_SETTINGS_PAGE_H
#define PLUTO_SETTINGS_PAGE_H

#include "pd_api.h"

/* SettingsPage.open(prevState): snapshot staged settings, reset selection. */
void settings_page_open(int prevState);

/* SettingsPage.close(). */
void settings_page_close(void);

/* Feed pushed-button mask. Returns "save" / "close" (caller frees) or NULL.
 * clearCookiesCb fires when the Clear Cookies action is adjusted/activated. */
char *settings_page_handle_input(unsigned int pushed, void (*clearCookiesCb)(void));

/* Draw with the 300ms ease-out-cubic scale-in animation. */
void settings_page_draw(void);

/* Feed raw crank delta while the panel is open: scrolls the selection
 * (clockwise = down). The panel consumes the motion entirely — the caller
 * must zero its own crank state afterwards so the background never moves. */
void settings_page_apply_crank(float crankChange);

int settings_page_is_open(void);
int settings_page_selected_index(void);
int settings_page_previous_state(void);

/* Test support: value strings currently staged ("HTML"/"Reader", "On"/"Off",
 * engine name, image-mode label). */
const char *settings_page_staged_value(int optionIndex);

/* Register the change callback (reference SettingsPage.onChangeCallback):
 * fired by save_and_close after settings are persisted. */
void settings_page_set_onchange_callback(void (*fn)(void));

#endif /* PLUTO_SETTINGS_PAGE_H */
