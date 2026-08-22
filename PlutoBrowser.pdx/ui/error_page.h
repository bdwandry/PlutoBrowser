#ifndef PLUTO_UI_ERROR_PAGE_H
#define PLUTO_UI_ERROR_PAGE_H

#include <stddef.h>

#include "pd_api.h"

// C port of CometBrowser Source/ui/error_page.lua (ErrorPage).
//
// show() stores msg/url and resets selection to 1. handle_input mirrors
// Lua's buttonJustPressed branches: left|up -> -1 (clamp 1), right|down
// -> +1 (clamp 3), A -> retry / search / home by index.

typedef enum {
    EP_BTN_LEFT,
    EP_BTN_UP,
    EP_BTN_RIGHT,
    EP_BTN_DOWN,
    EP_BTN_A
} EpButton;

typedef enum {
    EP_ACT_NONE = 0,
    EP_ACT_RETRY,
    EP_ACT_SEARCH,
    EP_ACT_HOME
} EpAction;

void     ep_init_pd(PlaydateAPI* pd);
void     ep_show(const char* errorMsg, const char* failedUrl);
EpAction ep_handle_input(EpButton btn);
int      ep_selected_index(void);
const char* ep_error_msg(void);
const char* ep_failed_url(void);
void     ep_draw(void);

#endif
