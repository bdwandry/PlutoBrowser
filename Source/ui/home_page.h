/*
 * PlutoBrowser — home_page.h
 * Start page / speed dial (port of Source/ui/home_page.lua).
 */
#ifndef PLUTO_HOME_PAGE_H
#define PLUTO_HOME_PAGE_H

#include "pd_api.h"

/* State: selectedIndex 0 = Settings button, 1..count = bookmark cards. */
void home_page_reset(void);

/* Feed button edge masks (from getButtonState). settingsCallback fires when
 * A is pressed on the Settings button. Returns bookmark URL to navigate to,
 * or NULL (caller frees with pluto_free). */
char *home_page_handle_input(unsigned int pushed, void (*settingsCallback)(void));

/* Draw. crankChange is the raw crank delta (invertCrank flips the direction). */
void home_page_draw(float crankChange);

/* Test accessors. */
int home_page_selected_index(void);
int home_page_scroll_y(void);

#endif /* PLUTO_HOME_PAGE_H */
