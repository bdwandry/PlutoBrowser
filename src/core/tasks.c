// tasks.c — cooperative task scheduler (C port of Source/core/tasks.lua).
//
// Lua ground truth captured on host Lua 5.5 (p05_oracle.lua):
//   - run(): isRunning true immediately; progress reset to 0 immediately;
//     job first advances on the NEXT update(); callbacks fire during the
//     update in which the job finishes.
//   - update() resumes ONLY queue[1] (head), at most one job per frame.
//   - done path: progress forced to 1, head removed, gc scheduled,
//     onComplete(result) via pcall. error path: progress LEFT AS-IS,
//     head removed, gc scheduled, onError(tostring(err)) via pcall.
//   - yieldCheck(): shared global counter; every 64th call checks
//     (inside && now-frameStart >= 500); outside a job it never yields.
//     With +20ms per iteration a 1000-iteration loop spans 16 frames.
//   - cancelAll(): no-op on empty queue (progress untouched, no GC);
//     otherwise drops every job with NO callbacks, zeroes progress,
//     schedules GC.
//   - scheduleGC(): drains within ONE update regardless of completion
//     (pcall quirk clears `done` flag unconditionally).
//   - reportProgress clamps to [0,1], monotonic.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "../util/dynarray.h"
#include "logger.h"
#include "tasks.h"

typedef struct PlutoJob {
    PlutoTaskStepFn step;
    void* ctx;
    PlutoTaskCtxFreeFn ctxFree;
    PlutoTaskDoneFn onComplete;
    PlutoTaskFailFn onError;
    void* ud;
    char err[192];
} PlutoJob;

static struct PlaydateAPI* s_pd = NULL;
static DynArray s_queue; // PlutoJob
static int s_inside = 0;
static unsigned s_counter = 0;
static unsigned s_frameStartMs = 0;
static double s_progress = 0.0;
static int s_gcPending = 0;
static char s_errBuf[192];
static PlutoTaskClockFn s_clockFn = NULL;

unsigned tasks_default_clock(void)
{
    if (s_pd != NULL) {
        return s_pd->system->getCurrentTimeMilliseconds();
    }
    return 0;
}

static unsigned now_ms(void)
{
    if (s_clockFn != NULL) {
        return s_clockFn();
    }
    return tasks_default_clock();
}

void tasks_set_clock_fn(PlutoTaskClockFn clockFn) { s_clockFn = clockFn; }

void tasks_init(struct PlaydateAPI* pd)
{
    s_pd = pd;
    da_init(&s_queue, sizeof(PlutoJob));
    s_inside = 0;
    s_counter = 0;
    s_frameStartMs = 0;
    s_progress = 0.0;
    s_gcPending = 0;
}

void tasks_run(PlutoTaskStepFn step, void* ctx, PlutoTaskCtxFreeFn ctxFree,
               PlutoTaskDoneFn onComplete, PlutoTaskFailFn onError, void* ud)
{
    PlutoJob job;
    memset(&job, 0, sizeof(job));
    job.step = step;
    job.ctx = ctx;
    job.ctxFree = ctxFree;
    job.onComplete = onComplete;
    job.onError = onError;
    job.ud = ud;

    // Lua parity: progress resets even while another job is mid-flight.
    s_progress = 0.0;

    da_push(&s_queue, &job);
    PLUTO_LOG("[P05] task queued (queue=%d)", (int)s_queue.count);
}

int tasks_is_running(void) { return s_queue.count > 0 ? 1 : 0; }

void tasks_cancel_all(void)
{
    size_t i;
    if (s_queue.count == 0) {
        return; // Lua parity: empty queue -> full no-op
    }
    for (i = 0; i < s_queue.count; i++) {
        PlutoJob* j = (PlutoJob*)da_get(&s_queue, i);
        if (j != NULL && j->ctxFree != NULL) {
            j->ctxFree(j->ctx);
        }
    }
    s_queue.count = 0;
    s_progress = 0.0;
    s_gcPending = 1;
    PLUTO_LOG("[P05] cancelAll dropped all tasks");
}

void tasks_schedule_gc(void) { s_gcPending = 1; }

static void drain_gc(void)
{
    // Lua: up to 4 collectgarbage("step"); the pcall wrapper clears the
    // pending flag within this same update either way (verified quirk).
    // C has no GC -> log-only note (MASTER_TODO P05).
    s_gcPending = 0;
    PLUTO_LOG("[P05] gc drain (no-op in C)");
}

void tasks_update(void)
{
    if (s_queue.count > 0) {
        PlutoJob* job = (PlutoJob*)da_get(&s_queue, 0);
        PlutoJob finished;
        int st;

        s_frameStartMs = now_ms();
        s_errBuf[0] = '\0';

        s_inside = 1;
        st = job->step(job->ctx);
        s_inside = 0;

        if (st == PLUTO_TASK_YIELD) {
            // stays queued; next resume happens next frame
        } else {
            finished = *job;
            da_remove_at(&s_queue, 0);
            tasks_schedule_gc();
            if (st == PLUTO_TASK_DONE) {
                s_progress = 1.0;
                PLUTO_LOG("[P05] task done");
                if (finished.onComplete != NULL) {
                    finished.onComplete(finished.ctx, finished.ud);
                }
            } else {
                const char* msg =
                    s_errBuf[0] != '\0' ? s_errBuf : "task error";
                PLUTO_LOG("[P05] task error: %s", msg);
                if (finished.onError != NULL) {
                    finished.onError(msg, finished.ud);
                }
            }
            if (finished.ctxFree != NULL) {
                finished.ctxFree(finished.ctx);
            }
        }
    }

    if (s_gcPending) {
        drain_gc();
    }
}

int tasks_yield_check(void)
{
    s_counter++;
    if (s_counter >= PLUTO_TASK_CHECK_EVERY) {
        s_counter = 0;
        if (s_inside && now_ms() - s_frameStartMs >=
                            PLUTO_TASK_FRAME_BUDGET_MS) {
            return 1;
        }
    }
    return 0;
}

void tasks_set_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_errBuf, sizeof(s_errBuf), fmt, ap);
    va_end(ap);
}

void tasks_report_progress(double f)
{
    if (f > 1.0) {
        f = 1.0;
    } else if (f < 0.0) {
        f = 0.0;
    }
    if (f > s_progress) {
        s_progress = f;
    }
}

double tasks_get_progress(void) { return s_progress; }
