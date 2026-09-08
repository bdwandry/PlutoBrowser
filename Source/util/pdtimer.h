/*
 * PlutoBrowser — pdtimer.h
 * Timer framework (Phase 4).
 *
 * The Lua reference uses CoreLibs/timer: playdate.timer.performAfterDelay(ms, fn)
 * plus playdate.timer.updateTimers() each frame. The C API has no timer
 * framework, so PlutoBrowser implements the subset of semantics CometBrowser
 * relies on:
 *   - one-shot delayed callbacks (meta refresh redirect, about: page success,
 *     image-decode re-scheduling chains)
 *   - self-rescheduling (a callback may schedule another timer)
 *   - cancellation of all pending timers (navigation resets)
 * Timers fire on the first frame update AFTER their delay has elapsed, matching
 * CoreLibs behavior closely enough for every CometBrowser call site.
 */
#ifndef PLUTO_PDTIMER_H
#define PLUTO_PDTIMER_H

#include "pd_api.h"

typedef void (*PDTimerCallback)(void *userdata);

/* One-time initialization (called from eventHandler kEventInit). */
void pdtimer_init(PlaydateAPI *pd);

/* Schedule `cb(userdata)` to fire once, at least `delayMs` from now.
 * Returns an opaque timer id (non-zero), or 0 on allocation failure. */
unsigned int pdtimer_perform_after_delay(PlaydateAPI *pd, unsigned int delayMs,
                                         PDTimerCallback cb, void *userdata);

/* Fire any due timers; call once per frame from the update loop. */
void pdtimer_update(void);

/* Cancel every pending timer (used on navigation, like the Lua flows reset). */
void pdtimer_cancel_all(void);

/* Cancel a specific timer by id. */
void pdtimer_cancel(unsigned int id);

/* Diagnostics: number of currently pending timers. */
int pdtimer_pending_count(void);

#endif /* PLUTO_PDTIMER_H */
