#ifndef PLUTO_UI_HOME_PAGE_H
#define PLUTO_UI_HOME_PAGE_H

#include <stddef.h>

struct PlaydateAPI;

/* C port of Source/ui/home_page.lua (HomePage).
 *
 * Faithful parity notes:
 *  - selectedIndex 0 = Settings button, 1..count = bookmark cards.
 *  - Grid nav: Down from settings -> first card (only when count>0);
 *    Down moves +2 when a full row exists below else +1; Up from the
 *    first row -> settings; Left/Right only swap columns on odd/even
 *    indices within range.
 *  - draw(crankChange): crank scroll x1.5 honoring settings.invertCrank,
 *    auto-scroll-to-selection math (viewport bands SCREEN_HEIGHT-40 /
 *    CONTENT_Y+10), smooth scroll lerp 0.3 with 0.5 snap and >=0 clamp.
 *  - Marquee: oversized card text oscillates at speed=50 px/s with
 *    dwell=1.0s holds at both ends, clipped to maxW x 15 inside its
 *    card; per-key start timestamps.
 *  - A on settings invokes settingsCallback then reports HP_ACT_SETTINGS
 *    (Lua returns nil); A on a card reports its URL.
 */

typedef enum {
    HP_BTN_UP = 0,
    HP_BTN_DOWN,
    HP_BTN_LEFT,
    HP_BTN_RIGHT,
    HP_BTN_A
} HpButton;

typedef enum {
    HP_ACT_NONE = 0,
    HP_ACT_OPEN_URL,
    HP_ACT_SETTINGS
} HpAction;

void hp_init(struct PlaydateAPI* pd);
void hp_reset(void);

/* Lua HomePage.settingsCallback */
void hp_set_settings_callback(void (*fn)(void));

/* Pure grid-navigation rules over an external selection index
 * (host-testable seam for HomePage.handleInput's d-pad branch). */
void hp_grid_nav(HpButton btn, int* selectedIndex, int count);

/* Full input step against Storage bookmarks: d-pad nav plus the A action.
 * outUrl (may be NULL) receives the bookmark URL for HP_ACT_OPEN_URL. */
HpAction hp_handle_input(HpButton btn, char* outUrl, size_t cap);

/* Pure math seams mirroring the draw() internals (host-testable). */
double hp_autoscroll_target(int selectedIndex, int count,
                            double targetScrollY);
double hp_marquee_offset(double elapsedSec, double textW, double maxW);
double hp_smooth_scroll(double cur, double target);

int  hp_bookmark_count(void);
int  hp_selected_index(void);
void hp_set_selected_index(int idx);

/* device/sim only: paints the home page; host builds no-op */
void hp_draw(double crankChange);

#endif
