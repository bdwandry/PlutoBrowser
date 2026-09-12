/*
 * PlutoBrowser — history_page.h
 * Browsing history view (port of Source/ui/history_page.lua).
 */
#ifndef PLUTO_HISTORY_PAGE_H
#define PLUTO_HISTORY_PAGE_H

void history_page_open(void);

/* Feed pushed-button mask. Returns history URL to open (caller frees via
 * pluto_free), "close" (caller frees), or NULL. */
char *history_page_handle_input(unsigned int pushed);

/* Draw. crankChange scrolls the list. */
void history_page_draw(float crankChange);

int history_page_selected_index(void);

#endif /* PLUTO_HISTORY_PAGE_H */
