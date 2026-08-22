#ifndef PLUTO_UI_LIST_CORE_H
#define PLUTO_UI_LIST_CORE_H

#include <stddef.h>

// Pure row-list primitives shared by BookmarksPage and HistoryPage
// (identical logic in CometBrowser's bookmarks_page.lua / history_page.lua).

// Buttons consumed by the list pages.
typedef enum {
    LR_BTN_UP,
    LR_BTN_DOWN,
    LR_BTN_A,
    LR_BTN_B
} LrButton;

typedef enum {
    LP_ACT_NONE = 0,
    LP_ACT_CLOSE,
    LP_ACT_OPEN
} LpAction;

// Lua: math.min(count, sel + 1) / math.max(1, sel - 1).
void lr_nav(int* selectedIndex, LrButton dir, int count);

// Lua: scrollY = max(0, scrollY + crankChange * 2).
void lr_scroll(double* scrollY, double crankChange);

// Row culling band from Lua draw loops.
int  lr_row_visible(float drawY, float itemH);

// Lua clips: titles #>34 -> first 31 + "...", urls #>46 -> first 43 + "...".
void lr_clip_title(char* out, size_t cap, const char* s);
void lr_clip_url(char* out, size_t cap, const char* s);

#endif
