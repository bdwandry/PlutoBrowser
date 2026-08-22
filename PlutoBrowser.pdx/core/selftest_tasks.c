// selftest_tasks.c — P05 verification suite.
//
// Every expectation mirrors observable behavior of Source/core/tasks.lua
// captured via the host-Lua oracle (p05_oracle.lua -> p05_truth.txt):
// immediate-done timing, 500ms/64-iteration budget gating (+20ms/iter ->
// 16 frames for a 1000-iteration loop, first yield at iteration 64),
// head-only FIFO, error-path progress retention, cancelAll semantics
// incl. empty-queue no-op, monotonic clamped progress.
#include <stdio.h>
#include <string.h>

#include "../core/logger.h"
#include "../core/tasks.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P05] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P05] FAIL %s", name);
    }
}

// ---------------------------------------------------------- fake clock ----
static unsigned g_now = 0;
static unsigned fake_clock(void) { return g_now; }

// ------------------------------------------------------------- helpers ----

typedef struct ScriptCtx {
    int stepCalls;
    int yieldsBeforeDone;
    int firstYieldAtIter;
    long loopIters;
    int done;
    int cbFired;
    int errFired;
    int freed;
    char lastMsg[192];
} ScriptCtx;

static void sc_init(ScriptCtx* c)
{
    memset(c, 0, sizeof(*c));
    c->firstYieldAtIter = -1;
}

static void on_done_mark(void* ctx, void* ud)
{
    ScriptCtx* c = (ScriptCtx*)ud;
    (void)ctx;
    c->cbFired++;
}

static void on_err_capture(const char* msg, void* ud)
{
    ScriptCtx* c = (ScriptCtx*)ud;
    c->errFired++;
    snprintf(c->lastMsg, sizeof(c->lastMsg), "%s", msg);
}

static void ctx_free_mark(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    c->freed++;
}

static int step_instant(void* ctx)
{
    ((ScriptCtx*)ctx)->stepCalls++;
    return PLUTO_TASK_DONE;
}

static int step_yield_n(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    c->stepCalls++;
    if (c->stepCalls <= c->yieldsBeforeDone) {
        return PLUTO_TASK_YIELD;
    }
    c->done = 1;
    return PLUTO_TASK_DONE;
}

// Oracle section M: 1000 yieldCheck() iterations, clock +20ms per
// iteration. Ground truth: first yield at iteration 64, 16 frames total.
static int step_budget_loop(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    while (c->loopIters < 1000) {
        c->loopIters++;
        if (tasks_yield_check()) {
            if (c->firstYieldAtIter < 0) {
                c->firstYieldAtIter = (int)c->loopIters;
            }
            return PLUTO_TASK_YIELD;
        }
        g_now += 20; // wall clock advances inside the loop
    }
    c->done = 1;
    return PLUTO_TASK_DONE;
}

// Frozen-clock variant: budget gate can never fire (elapsed == frameStart).
static int step_frozen_loop(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    while (c->loopIters < 1000) {
        c->loopIters++;
        if (tasks_yield_check()) {
            c->firstYieldAtIter = (int)c->loopIters;
            return PLUTO_TASK_YIELD;
        }
    }
    c->done = 1;
    return PLUTO_TASK_DONE;
}

// Endless job for cancel tests: advances clock so the gate fires.
static int step_endless(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    while (1) {
        g_now += 5;
        if (tasks_yield_check()) {
            return PLUTO_TASK_YIELD;
        }
    }
}

static int step_err_msg(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    c->stepCalls++;
    tasks_report_progress(0.9);
    tasks_set_error("Parse Error: boom");
    return PLUTO_TASK_ERROR;
}

static int step_err_default(void* ctx)
{
    ScriptCtx* c = (ScriptCtx*)ctx;
    c->stepCalls++;
    return PLUTO_TASK_ERROR;
}

static int run_to_completion(int maxFrames)
{
    int frames = 0;
    while (tasks_is_running() && frames < maxFrames) {
        tasks_update();
        frames++;
    }
    return frames;
}

// ------------------------------------------------- FIFO order tracking ----
static const char* g_order[8];
static int g_orderN;

static void mark(const char* s)
{
    if (g_orderN < (int)(sizeof(g_order) / sizeof(g_order[0]))) {
        g_order[g_orderN++] = s;
    }
}

static int step_mark_a(void* ctx)
{
    (void)ctx;
    mark("A.step");
    return PLUTO_TASK_DONE;
}
static int step_mark_b(void* ctx)
{
    (void)ctx;
    mark("B.step");
    return PLUTO_TASK_DONE;
}
static void done_mark_a(void* ctx, void* ud)
{
    (void)ctx;
    (void)ud;
    mark("A.done");
}
static void done_mark_b(void* ctx, void* ud)
{
    (void)ctx;
    (void)ud;
    mark("B.done");
}

// ---------------------------------------------------------------- tests ----

void selftest_tasks_run(int* outPass, int* outFail)
{
    ScriptCtx a, b;
    int frames;
    double p0, p1;

    // main.c performs tasks_init(pd); tests only swap the clock.
    tasks_set_clock_fn(fake_clock);
    g_now = 1000;

    // A: run/isRunning/progress timing -------------------------------------
    sc_init(&a);
    tasks_run(step_instant, &a, NULL, NULL, NULL, NULL);
    st_check(tasks_is_running() == 1, "run.is_running_immediately");
    st_check(tasks_get_progress() == 0.0, "run.progress_reset_immediately");
    st_check(a.stepCalls == 0, "run.step_not_called_before_update");

    // B: instant completion on first update ---------------------------------
    tasks_update();
    st_check(a.cbFired == 0 && a.stepCalls == 1,
             "done.no_callback_when_null_onComplete");
    st_check(tasks_is_running() == 0 && tasks_get_progress() == 1.0,
             "done.queue_emptied_progress_forced_one");

    // C: advancing clock ground truth (oracle M) ----------------------------
    // NOTE: runs FIRST among yield-checking jobs so the shared iteration
    // counter (module-global in Lua too) starts pristine -> first gate
    // lands exactly at iteration 64.
    g_now = 0;
    sc_init(&a);
    tasks_run(step_budget_loop, &a, NULL, NULL, NULL, NULL);
    frames = run_to_completion(100);
    st_check(a.done == 1 && a.firstYieldAtIter == 64,
             "budget.first_yield_at_iter64");
    st_check(frames == 16, "budget.frames16_for_1000iters_at20ms");

    // D: frozen clock never yields (oracle B) -------------------------------
    sc_init(&a);
    tasks_run(step_frozen_loop, &a, NULL, on_done_mark, NULL, &a);
    frames = run_to_completion(5);
    st_check(frames == 1 && a.done == 1 && a.loopIters == 1000 &&
                 a.firstYieldAtIter < 0 && a.cbFired == 1,
             "budget.frozen_clock_single_frame");

    // E: yieldCheck outside any task never yields (oracle I) -----------------
    {
        int i;
        int yielded = 0;
        g_now = 999999;
        for (i = 0; i < 300; i++) {
            if (tasks_yield_check()) {
                yielded++;
            }
        }
        st_check(yielded == 0, "outside.never_yields");
    }

    // F: error path keeps task-reported progress (oracle C quirk) ------------
    // (run() wipes any pre-existing progress; the task's own 0.9 report wins.)
    g_now = 0;
    sc_init(&a);
    tasks_run(step_err_msg, &a, NULL, on_done_mark, on_err_capture, &a);
    tasks_update();
    st_check(a.errFired == 1 && a.cbFired == 0,
             "err.onError_not_onComplete");
    st_check(strcmp(a.lastMsg, "Parse Error: boom") == 0,
             "err.message_delivered_verbatim");
    st_check(tasks_get_progress() == 0.9, "err.progress_keeps_task_report");

    // G: unset error message default -----------------------------------------
    sc_init(&b);
    tasks_run(step_err_default, &b, NULL, NULL, on_err_capture, &b);
    tasks_update();
    st_check(b.errFired == 1 && strcmp(b.lastMsg, "task error") == 0,
             "err.default_message");

    // H: FIFO head-only, one job per update (oracle D) ------------------------
    g_orderN = 0;
    g_now = 0;
    tasks_run(step_mark_a, NULL, NULL, done_mark_a, NULL, NULL);
    tasks_run(step_mark_b, NULL, NULL, done_mark_b, NULL, NULL);
    st_check(tasks_is_running() == 1, "fifo.two_queued");
    tasks_update();
    st_check(g_orderN == 2 && strcmp(g_order[0], "A.step") == 0 &&
                 strcmp(g_order[1], "A.done") == 0,
             "fifo.upd1_only_head_a");
    tasks_update();
    st_check(g_orderN == 4 && strcmp(g_order[2], "B.step") == 0 &&
                 strcmp(g_order[3], "B.done") == 0,
             "fifo.upd2_then_b");
    tasks_update();
    st_check(g_orderN == 4 && !tasks_is_running(), "fifo.upd3_idle");

    // I: clamp + monotonic progress right after run() reset -------------------
    sc_init(&a);
    tasks_run(step_instant, &a, NULL, NULL, NULL, NULL);
    st_check(tasks_get_progress() == 0.0, "prog.reset_by_run");
    tasks_report_progress(-0.5);
    st_check(tasks_get_progress() == 0.0, "prog.neg_clamped");
    tasks_report_progress(0.3);
    st_check(tasks_get_progress() == 0.3, "prog.set03");
    tasks_report_progress(0.2);
    st_check(tasks_get_progress() == 0.3, "prog.regression_ignored");
    tasks_report_progress(1.5);
    st_check(tasks_get_progress() == 1.0, "prog.over_clamped");
    tasks_update(); // finish it (progress stays 1)

    // J: cancelAll on empty queue is a full no-op (oracle F) ------------------
    p0 = tasks_get_progress();
    tasks_cancel_all();
    p1 = tasks_get_progress();
    st_check(p0 == p1 && tasks_is_running() == 0,
             "cancel.empty_queue_noop");

    // K: cancelAll mid-flight drops silently, frees ctx, schedules gc ---------
    g_now = 0;
    sc_init(&a);
    tasks_run(step_endless, &a, ctx_free_mark, on_done_mark, on_err_capture,
              &a);
    tasks_update(); // starts and yields once budget hit
    st_check(tasks_is_running() == 1 && a.cbFired == 0 && a.errFired == 0,
             "cancel.job_started_yielded");
    tasks_cancel_all();
    st_check(tasks_is_running() == 0 && tasks_get_progress() == 0.0,
             "cancel.queue_cleared_progress_zeroed");
    st_check(a.cbFired == 0 && a.errFired == 0,
             "cancel.no_callbacks_fired");
    st_check(a.freed == 1, "cancel.ctx_freed");
    tasks_update(); // idle pump must be safe (drains gc note)
    st_check(tasks_is_running() == 0, "cancel.post_cancel_idle_safe");

    // L: run-during-run resets progress immediately (parity quirk) ------------
    g_now = 0;
    sc_init(&a);
    sc_init(&b);
    tasks_run(step_endless, &a, NULL, NULL, NULL, NULL);
    tasks_update(); // a yields
    tasks_run(step_instant, &b, NULL, NULL, NULL, NULL);
    st_check(tasks_get_progress() == 0.0, "run.second_run_resets_progress");
    tasks_cancel_all(); // cleanup (frees nothing; ctx are stack locals)

    // M: ctxFree on completion and error paths --------------------------------
    g_now = 0;
    sc_init(&a);
    tasks_run(step_instant, &a, ctx_free_mark, NULL, NULL, NULL);
    tasks_update();
    st_check(a.freed == 1, "free.on_complete_path");

    sc_init(&b);
    tasks_run(step_err_default, &b, ctx_free_mark, NULL, on_err_capture, &b);
    tasks_update();
    st_check(b.freed == 1, "free.on_error_path");

    // N: multi-frame yield_n job ----------------------------------------------
    g_now = 0;
    sc_init(&a);
    a.yieldsBeforeDone = 3;
    tasks_run(step_yield_n, &a, NULL, on_done_mark, NULL, &a);
    frames = run_to_completion(10);
    st_check(frames == 4 && a.stepCalls == 4 && a.cbFired == 1,
             "yieldn.four_frames_then_done");

    // O: external scheduleGC is harmless ---------------------------------------
    tasks_schedule_gc();
    tasks_update();
    st_check(!tasks_is_running(), "gc.external_schedule_safe");

    PLUTO_LOG("[P05] tasks selftests done: %d passed, %d failed", st_pass,
              st_fail);
    if (outPass != NULL) {
        *outPass = st_pass;
    }
    if (outFail != NULL) {
        *outFail = st_fail;
    }
}
