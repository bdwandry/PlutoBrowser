// tasks.h — cooperative task scheduler (C port of Source/core/tasks.lua).
//
// Phase P05. Lua drives heavy work (parse/layout/decode) inside coroutines
// paused by Tasks.yieldCheck() when a frame burns its CPU budget. C has no
// coroutines, so each job is a resumable state machine: step() performs one
// slice and returns YIELD when the budget gate fires (call sites use
// tasks_yield_check() at exactly the same loop positions as Lua), DONE when
// finished, ERROR on failure.
#ifndef PLUTO_TASKS_H
#define PLUTO_TASKS_H

#include <stddef.h>

struct PlaydateAPI;

#define PLUTO_TASK_FRAME_BUDGET_MS 500u // max ms of heavy work per frame
#define PLUTO_TASK_CHECK_EVERY     64   // check clock every N iterations

typedef enum PlutoTaskStatus {
    PLUTO_TASK_DONE  = 0,
    PLUTO_TASK_YIELD = 1,
    PLUTO_TASK_ERROR = 2
} PlutoTaskStatus;

typedef int (*PlutoTaskStepFn)(void* ctx);
typedef void (*PlutoTaskCtxFreeFn)(void* ctx);
typedef void (*PlutoTaskDoneFn)(void* ctx, void* ud);
typedef void (*PlutoTaskFailFn)(const char* msg, void* ud);

// Lua: playdate.getCurrentTimeMilliseconds().
unsigned tasks_default_clock(void);

void tasks_init(struct PlaydateAPI* pd);

// Test hook: replace the wall clock (cookie_jar-style injection).
typedef unsigned (*PlutoTaskClockFn)(void);
void tasks_set_clock_fn(PlutoTaskClockFn clockFn);

// Queue a job. Parity notes (verified against tasks.lua on host Lua):
//  - progressValue resets to 0 IMMEDIATELY, even if another job is running.
//  - the job starts on the NEXT Tasks.update(), never inside run().
//  - only the HEAD job advances, one resume per update() (strict FIFO).
// ctx ownership: ctxFree(ctx) runs after onComplete/onError, or on
// cancelAll (which fires NO callbacks — cancelled jobs are dropped).
void tasks_run(PlutoTaskStepFn step, void* ctx, PlutoTaskCtxFreeFn ctxFree,
               PlutoTaskDoneFn onComplete, PlutoTaskFailFn onError, void* ud);

int  tasks_is_running(void);
void tasks_cancel_all(void);
void tasks_schedule_gc(void);

// Call once per frame from the main update loop (main.lua:602 parity).
void tasks_update(void);

// Hot-loop gate; mirrors Tasks.yieldCheck() placement exactly.
// Returns 1 when the caller must unwind its loop and return
// PLUTO_TASK_YIELD from its step function. Outside a job this NEVER
// yields (Lua: guarded by `inside`), but the shared iteration counter
// still advances (module-global in Lua too).
int tasks_yield_check(void);

// Inside a failing step: record the message delivered to onError
// (Lua: error("Parse Error: ...")). Unset errors surface as "task error".
void tasks_set_error(const char* fmt, ...);

// Monotonic [0,1] progress; fractional regressions ignored (Lua parity).
void   tasks_report_progress(double f);
double tasks_get_progress(void);

#endif // PLUTO_TASKS_H
