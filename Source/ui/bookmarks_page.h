#ifndef PLUTO_UI_BOOKMARKS_PAGE_H
#define PLUTO_UI_BOOKMARKS_PAGE_H

#include <stddef.h>

#include "pd_api.h"
#include "ui/list_core.h"

// C port of CometBrowser Source/ui/bookmarks_page.lua (BookmarksPage).
// Row list of Storage.bookmarks(): 34px rows, title/url clips, crank
// scroll x2, selection inversion, B closes, A opens the row URL.

void     bm_init_pd(PlaydateAPI* pd);
void     bm_open(void);
LpAction bm_handle_input(LrButton btn, char* outUrl, size_t cap);
void     bm_draw(double crankChange);

int      bm_selected_index(void);
double   bm_scroll_y(void);

#endif
