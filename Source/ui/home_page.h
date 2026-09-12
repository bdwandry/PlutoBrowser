/*
 * PlutoBrowser — home_page.h
 * Start page / speed dial (port of Source/ui/home_page.lua).
 */
#ifndef PLUTO_HOME_PAGE_H
#define PLUTO_HOME_PAGE_H

#include "pd_api.h"

/* BF14: home-page crank step — one bookmark per this many degrees of crank
 * travel. History of user feedback: 18° -> 25° -> 90° (quarter turn) ->
 * **45° (BF14d, current — user request "I want 45 degrees"). HOME PAGE
 * ONLY — page scrolling elsewhere is untouched. Defined here so
 * tests/bf14_home_crank_host_test.c drives the exact production value. */
#define HOME_CRANK_STEP_PX 45.0f

/* State: selectedIndex 0 = Settings button, 1..count = bookmark cards. */
void home_page_reset(void);

/* BF14: apply the crank delta for this frame. The crank moves the
 * SELECTION through the bookmark list (Settings button included) and the
 * scroll target follows, clamped to the content bottom — one bookmark per
 * HOME_CRANK_STEP_PX degrees (45°, half a quarter turn); cranking past the
 * final card free-scrolls into the footer. invertCrank flips the
 * direction. */
void home_page_handle_crank(float crankChange);

/* BF14: follow the selection with the scroll target (clamped to the
 * content bottom) and ease g_scrollY toward it. Called from draw(). */
void home_page_update_scroll(void);

/* BF14: end the current crank gesture (next crank re-anchors at the
 * current selection). Called when a B-hold starts on the home page. */
void home_page_end_crank_gesture(void);

/* Feed button edge masks (from getButtonState). settingsCallback fires when
 * A is pressed on the Settings button. Returns bookmark URL to navigate to,
 * or NULL (caller frees with pluto_free). */
char *home_page_handle_input(unsigned int pushed, void (*settingsCallback)(void));

/* Draw. crankChange is the raw crank delta (invertCrank flips the
 * direction; selection + scroll handled via handle_crank/update_scroll). */
void home_page_draw(float crankChange);

/* Test accessors. */
int home_page_selected_index(void);
int home_page_scroll_y(void);

#endif /* PLUTO_HOME_PAGE_H */
