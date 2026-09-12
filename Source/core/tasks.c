/*
 * PlutoBrowser — tasks.c
 * Cooperative scheduler (port of Source/core/tasks.lua). See tasks.h.
 */
#include <string.h>

#include "core/tasks.h"

/* Lua reference values: 500ms frame budget, clock check every 64 iterations. */
#define FRAME_BUDGET_MS 500
#define CHECK_EVERY 64

static TaskCtx g_queue[TASKS_MAX];
static int g_count = 0;
static float g_progress = 0;
static int g_gcPending = 0; /* placeholder: C frees are explicit; kept for parity */
static PlaydateAPI *g_pd = NULL;

void tasks_init(PlaydateAPI *pd)
{
    g_pd = pd;
    memset(g_queue, 0, sizeof(g_queue));
    g_count = 0;
    g_progress = 0;
}

void tasks_report_progress(float f)
{
    if (f > 1.0f)
    {
        f = 1.0f;
    }
    if (f < 0.0f)
    {
        f = 0.0f;
    }
    if (f > g_progress)
    {
        g_progress = f;
    }
}

float tasks_get_progress(void)
{
    return g_progress;
}

int tasks_is_running(void)
{
    return g_count > 0;
}

int tasks_run(TaskStepFn step, void *initialData, TaskDoneFn onDone, TaskErrorFn onError, void *userdata)
{
    if (g_count >= TASKS_MAX)
    {
        return -1;
    }
    TaskCtx *t = &g_queue[g_count];
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->step = step;
    t->data = initialData;
    t->onDone = onDone;
    t->onError = onError;
    t->userdata = userdata;
    t->pd = g_pd;
    g_count++;
    g_progress = 0;
    return 0;
}

int tasks_yield_check(TaskCtx *ctx)
{
    static int counter = 0;
    counter++;
    if (counter >= CHECK_EVERY)
    {
        counter = 0;
        if (ctx && ctx->active && ctx->pd)
        {
            if (ctx->pd->system->getCurrentTimeMilliseconds() - ctx->frameStartMs >= FRAME_BUDGET_MS)
            {
                return 1; /* caller should return from its step now */
            }
        }
    }
    return 0;
}

void tasks_cancel_all(void)
{
    if (g_count > 0)
    {
        g_count = 0;
        g_progress = 0;
        g_gcPending = 1;
    }
}

void tasks_update(void)
{
    if (g_count == 0)
    {
        /* Spread "GC" (here: nothing to collect; placeholder keeps parity
         * with the Lua module's gcPending flag) — cleared immediately. */
        g_gcPending = 0;
        return;
    }

    TaskCtx *task = &g_queue[0];
    task->frameStartMs = g_pd->system->getCurrentTimeMilliseconds();

    int keepGoing = 0;
    /* The step function returns 0 when finished, or (via yield) non-zero to
     * pause. A step that returns -1 signals an error. */
    int result = task->step(task);
    if (result > 0)
    {
        return; /* still running (yielded or more work) */
    }

    /* Finished (0) or error (-1). Capture the callback state BEFORE shifting
     * the queue down — after the shift `task` would alias the next entry. */
    TaskStepFn step = task->step;
    void *data = task->data;
    TaskDoneFn onDone = task->onDone;
    TaskErrorFn onError = task->onError;
    void *userdata = task->userdata;

    task->active = 0;
    /* shift the queue down */
    for (int i = 1; i < g_count; i++)
    {
        g_queue[i - 1] = g_queue[i];
    }
    g_count--;
    g_gcPending = 1;

    if (result < 0)
    {
        /* error path */
        g_progress = 0;
        if (onError)
        {
            onError(data ? (const char *)data : "task error", userdata);
        }
        (void)step;
        return;
    }

    /* normal completion */
    g_progress = 1;
    if (onDone)
    {
        onDone(data, userdata);
    }
}
