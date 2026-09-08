/*
 * PlutoBrowser — tasks.h
 * Cooperative task scheduler (port of Source/core/tasks.lua).
 *
 * Lua reference semantics preserved:
 *   - Tasks.run(fn, onComplete, onError): schedule work that runs a slice
 *     per frame; onComplete(result) or onError(message) when done.
 *   - Tasks.yieldCheck(): called from hot loops; withholds the frame when the
 *     500ms budget is spent (Lua yielded the coroutine; C just returns and the
 *     task resumes next update — identical observable frame behavior).
 *   - Tasks.reportProgress(f): monotonic fraction in [0,1].
 *   - Tasks.getProgress / isRunning / cancelAll.
 *
 * C design (no coroutines): a task is a step function
 *     int step(TaskCtx *ctx)  — returns 1 while more work remains, 0 when done.
 * The task owns its continuation via ctx->data and a position/phase field, so
 * large parses decode "a little each frame" exactly like the Lua version.
 */
#ifndef PLUTO_TASKS_H
#define PLUTO_TASKS_H

#include "pd_api.h"

#define TASKS_MAX 8

typedef struct TaskCtx TaskCtx;

/* Step function: return 0 when the task is finished, non-zero to continue.
 * ctx->data is the task's private state (allocated/freed by the task). */
typedef int (*TaskStepFn)(TaskCtx *ctx);

/* Completion callbacks (mirror onComplete/onError in Lua). */
typedef void (*TaskDoneFn)(void *result, void *userdata);
typedef void (*TaskErrorFn)(const char *message, void *userdata);

struct TaskCtx
{
    int active;
    TaskStepFn step;
    TaskDoneFn onDone;
    TaskErrorFn onError;
    void *userdata;
    void *data;            /* task-private continuation state */
    unsigned int frameStartMs;
    PlaydateAPI *pd;
};

void tasks_init(PlaydateAPI *pd);

/* Schedule a task. initialData becomes ctx->data (task-private state; the
 * task allocates/frees it — it is also passed to onDone as the result).
 * Returns 0 on success, -1 when the queue is full. */
int tasks_run(TaskStepFn step, void *initialData, TaskDoneFn onDone, TaskErrorFn onError, void *userdata);

/* Called from hot loops of heavy work. When the 500ms frame budget is spent,
 * sets ctx->yieldRequested = 1; the caller checks it (or the convenience
 * return value) and returns non-zero from its step function to pause until
 * the next frame. Returns 1 when the task should yield now. */
int tasks_yield_check(TaskCtx *ctx);

/* Report monotonic progress fraction [0,1]; fractional regressions ignored. */
void tasks_report_progress(float f);

float tasks_get_progress(void);

int tasks_is_running(void);

/* Drop all in-flight tasks (navigation away mid-render). */
void tasks_cancel_all(void);

/* Drive the head task for one frame; call once per update. */
void tasks_update(void);

#endif /* PLUTO_TASKS_H */
