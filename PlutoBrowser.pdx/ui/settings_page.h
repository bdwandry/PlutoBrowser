#ifndef PLUTO_UI_SETTINGS_PAGE_H
#define PLUTO_UI_SETTINGS_PAGE_H

#include <stddef.h>

#include "pd_api.h"

// C port of CometBrowser Source/ui/settings_page.lua (SettingsPage).
//
// Staged-settings overlay: open() snapshots Storage.settings into a staged
// copy; LEFT/RIGHT cycle the selected option; A saves (Storage.save +
// onChange callback) unless the row is the immediate-action "Clear
// Cookies"; B discards. Draw animates an ease-out-cubic 300ms grow-from-
// center box; content appears after t>0.4.

typedef enum {
    SP_BTN_UP,
    SP_BTN_DOWN,
    SP_BTN_LEFT,
    SP_BTN_RIGHT,
    SP_BTN_A,
    SP_BTN_B
} SpButton;

typedef enum {
    SP_ACT_NONE = 0,
    SP_ACT_SAVED,   // A on a non-action row: applied + persisted + closed
    SP_ACT_CLOSED   // B: discarded + closed
} SpAction;

#define SP_OPTION_COUNT 5 // Search Engine / Browse Mode / Invert Crank /
                          // Image Mode / Clear Cookies

typedef void (*SpOnChangeFn)(void);

void sp_init_pd(PlaydateAPI* pd);
void sp_set_on_change(SpOnChangeFn cb);

void sp_open(const char* prevState);
void sp_close(void);
SpAction sp_handle_input(SpButton btn);
void sp_draw(void);

int         sp_is_open(void);
int         sp_selected_index(void);
const char* sp_previous_state(void);

/* Staged getValue() renderings (Lua strings) */
const char* sp_staged_engine_name(void);
const char* sp_staged_mode_label(void);    /* "HTML" / "Reader"      */
const char* sp_staged_invert_label(void);  /* "On" / "Off"           */
const char* sp_staged_image_label(void);   /* IMAGE_MODE_LABELS[...] */

#endif
