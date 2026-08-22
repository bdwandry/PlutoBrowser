#ifndef PLUTO_UI_HISTORY_PAGE_H
#define PLUTO_UI_HISTORY_PAGE_H

#include <stddef.h>

#include "pd_api.h"
#include "ui/list_core.h"

// C port of CometBrowser Source/ui/history_page.lua (HistoryPage).
// Same row-list layout as BookmarksPage over Storage.history().

void     hi_init_pd(PlaydateAPI* pd);
void     hi_open(void);
LpAction hi_handle_input(LrButton btn, char* outUrl, size_t cap);
void     hi_draw(double crankChange);

int      hi_selected_index(void);
double   hi_scroll_y(void);

#endif
