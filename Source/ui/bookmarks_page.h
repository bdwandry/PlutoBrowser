/*
 * PlutoBrowser — bookmarks_page.h
 * Bookmarks manager view (port of Source/ui/bookmarks_page.lua).
 */
#ifndef PLUTO_BOOKMARKS_PAGE_H
#define PLUTO_BOOKMARKS_PAGE_H

void bookmarks_page_open(void);

/* Feed pushed-button mask. Returns bookmark URL to open (caller frees via
 * pluto_free), "close" (caller frees), or NULL. */
char *bookmarks_page_handle_input(unsigned int pushed);

/* Draw. crankChange scrolls the list. */
void bookmarks_page_draw(float crankChange);

int bookmarks_page_selected_index(void);

#endif /* PLUTO_BOOKMARKS_PAGE_H */
