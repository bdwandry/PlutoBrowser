/*
 * PlutoBrowser — address_bar.h
 * Address Bar & Web Search Controller (port of Source/ui/address_bar.lua).
 *
 * Lua reference behavior:
 *  - open(currentUrl, onSubmit): marks the bar open, pre-fills input with the
 *    current URL unless it starts with "about:", keyboard NOT shown yet.
 *  - launchKeyboard(): shows the keyboard (guarded against B held), installs
 *    willHide/textChanged callbacks; on submit trims whitespace, routes search
 *    queries through the configured search engine, else normalizes as a URL,
 *    sets skipInputFrames=2, fires the submit callback.
 *  - cancel(): hides keyboard if shown, clears callbacks.
 *  - drawOverlay(): compact box (10,6,380x48) when keyboard hidden, tall box
 *    (4,4,192x232) alongside the keyboard, label + mono text with wrapping.
 *
 * C port notes:
 *  - onSubmit is a function pointer + userdata carried per-open.
 *  - skipInputFrames lives in main.c; the submit path exposes
 *    address_bar_consume_skip_frames() so main can arm its own counter
 *    (Lua's global skipInputFrames = 2).
 *  - The keyboard port's update-callback takeover is honored: while the
 *    keyboard is visible, our updateFrame runs from inside keyboardUpdate.
 */
#ifndef PLUTO_ADDRESS_BAR_H
#define PLUTO_ADDRESS_BAR_H

#include <stdint.h>

typedef void (*AddressBarSubmitFn)(const char *finalUrl, void *userdata);

void address_bar_init(void);

/* Lua: AddressBar.open(currentUrl, onSubmit). */
void address_bar_open(const char *currentUrl, AddressBarSubmitFn onSubmit,
                      void *userdata);

/* Lua: AddressBar.launchKeyboard(). No-op unless open and not shown;
 * refuses while B is held (Lua parity). */
void address_bar_launch_keyboard(void);

/* Lua: AddressBar.cancel(). */
void address_bar_cancel(void);

/* Lua: AddressBar.drawOverlay(). Call after page/chrome drawing. */
void address_bar_draw_overlay(void);

int address_bar_is_open(void);
int address_bar_keyboard_shown(void);

/* Current input text (live-synced from the keyboard while shown). */
const char *address_bar_input_text(void);

/* Test hook: routes text through the real submit path (trim → search-vs-URL
 * routing → callback) without the raw keyboard, which P13 verified. */
void address_bar_test_submit(const char *text);

/* Lua copies keyboard text into inputText on every textChanged callback.
 * The C keyboard exposes getText(); the harness/main loop calls this each
 * frame while the keyboard is shown (Lua did it via callback). */
void address_bar_sync_text(void);

/* Lua sets the global skipInputFrames = 2 when the keyboard will hide;
 * returns how many frames main should skip (and clears it). */
int address_bar_consume_skip_frames(void);

#endif /* PLUTO_ADDRESS_BAR_H */
