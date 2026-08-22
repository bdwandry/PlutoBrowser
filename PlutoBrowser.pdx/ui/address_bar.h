#ifndef PLUTO_UI_ADDRESS_BAR_H
#define PLUTO_UI_ADDRESS_BAR_H

#include <stddef.h>

#include "pd_api.h"

// The vendored C keyboard port (vendor/keyboard, Unlicense) references this
// typedef from an older SDK; it must match pd->system->setUpdateCallback's
// function shape (our main update returns int).
typedef int PDCallbackFunction(void* userdata);

#include "keyboard.h"

// C port of CometBrowser Source/ui/address_bar.lua (AddressBar).
//
// Lifecycle parity:
//   ab_open            <- AddressBar.open(currentUrl, onSubmit)
//   ab_launch_keyboard <- AddressBar.launchKeyboard (gated on B held)
//   kb_will_hide       <- playdate.keyboard.keyboardWillHideCallback
//   ab_cancel          <- AddressBar.cancel
//
// Pure helpers (unit-tested): trim + final-URL decision (search query vs
// URL.parse normalized), open-prefill rule (^about: excluded) and the
// launchKeyboard gate.

typedef void (*AbSubmitFn)(const char* finalUrl, void* userdata);

// mainUpdate/updateUd are handed to the keyboard so it can re-install the
// system update loop when its takeover ends (vendored-lib requirement).
void ab_init(PlaydateAPI* pd, int (*mainUpdate)(void*), void* updateUd);

void ab_open(const char* currentUrl, AbSubmitFn onSubmit, void* userdata);
void ab_launch_keyboard(void);
void ab_cancel(void);

int         ab_is_open(void);
int         ab_keyboard_shown(void);
const char* ab_input_text(void);
void        ab_draw_overlay(void);

// skipInputFrames global: willHide sets 2; pop once per frame.
int  ab_pop_input_skip(void);
int  ab_input_skip_remaining(void);

// ── pure helpers ──────────────────────────────────────────────────────────

// Lua string.gsub(text, "^%s*(.-)%s*$", "%1"): trims both ends in place.
void ab_trim(char* text);

// Lua submit decision. Returns 0 for empty input (cancel path). Otherwise
// writes the search-engine URL or URL.parse().normalized into out.
int ab_build_final_url(const char* text, char* out, size_t cap);

// Lua launchKeyboard gates: isOpen && !keyboardShown && !B held.
int ab_should_launch_keyboard(int isOpen, int keyboardShown, int bHeld);

#endif
