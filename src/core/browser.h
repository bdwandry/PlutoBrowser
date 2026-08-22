#ifndef PLUTO_CORE_BROWSER_H
#define PLUTO_CORE_BROWSER_H

/* [P32] C port of CometBrowser Source/main.lua -- application state
 * machine, navigation flow (navigateTo/executeNavigation/runNavigation/
 * renderBody), history stack, form activation + submission, system menu,
 * reader-mode input with crank physics, loading/error/list/settings state
 * handling and the per-frame composition order.
 *
 * The C SDK exposes no menu-items API, so the Lua playdate.system menu is
 * rendered as a custom pause overlay opened from kEventPause (hardware
 * Menu button) with identical entries/labels/behavior.
 *
 * Testability seams mirror earlier phases: input snapshots + clock come
 * from injectable providers (br_set_input_source / br_set_clock_fn);
 * HTTP flows run against the P07 fake-TCP vtable in selftests. */

#include <stddef.h>

#include "core/url.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PlaydateAPI;
struct DocDocument;
struct LItem;

/* button bitmask used by the injected input provider */
enum {
    BR_BTN_LEFT  = 1u << 0,
    BR_BTN_RIGHT = 1u << 1,
    BR_BTN_UP    = 1u << 2,
    BR_BTN_DOWN  = 1u << 3,
    BR_BTN_B     = 1u << 4,
    BR_BTN_A     = 1u << 5
};

typedef struct BrInput {
    unsigned held;         /* buttons down this frame */
    unsigned justPressed;  /* rising edge this frame */
    float crankChange;     /* degrees since last frame */
    unsigned nowMs;        /* getCurrentTimeMilliseconds equivalent */
} BrInput;

typedef void (*BrInputFn)(BrInput* out, void* ud);
typedef unsigned (*BrClockFn)(void);

void br_init(struct PlaydateAPI* pd);
void br_set_input_source(BrInputFn fn, void* ud);
void br_set_clock_fn(BrClockFn fn);   /* ms like getCurrentTimeMilliseconds */
void br_set_update_trampoline(int (*fn)(void*));   /* vendor keyboard pump */

/* main.lua top-level init tail: STATE_HOME + menu + callbacks wired */
void br_boot(void);

/* navigateTo(url): stages pendingNavUrl, consumed at next br_frame */
void br_navigate_to(const char* url);

/* one full updateFrame() */
void br_frame(void);

/* kEventPause/kEventResume hooks for the custom menu overlay */
void br_on_pause(void);
void br_on_resume(void);
int  br_menu_open(void);
int  br_menu_sel(void);              /* test introspection: highlighted row */

/* introspection for tests + HUD debug */
int         br_state(void);
const char* br_page_title(void);
const char* br_current_normalized(void);
int         br_browse_mode(void);
double      br_scroll_y(void);
double      br_target_scroll_y(void);
float       br_crank_velocity(void);
int         br_is_rendering(void);
int         br_progress(int* cur, int* tot);
const struct DocDocument* br_current_doc(void);   /* borrowed, may be NULL */

/* history stack introspection (0-based) */
int         br_history_count(void);
int         br_history_index(void);
const char* br_history_at(int i);
int         br_go_back(void);    /* returns 0 when impossible */
int         br_go_forward(void);

/* keyboard-driven form input (test seam): finalize as if the on-screen
 * keyboard closed, applying maxlength truncation like keyboardDidHide */
void br_keyboard_finalize(const char* text);
int  br_keyboard_open(void);

/* activateFormBlock(block) exposed for tests: checkbox/radio toggle,
 * select cycle, submit, or open the keyboard for a text field */
void br_activate_item(struct LItem* it);

/* test seams */
void br_reset_for_tests(void);      /* clean engine slate between suites */
void br_set_keyboard_enabled(int);  /* 0: record state, never show HW kb */

#ifdef __cplusplus
}
#endif

#endif // PLUTO_CORE_BROWSER_H
