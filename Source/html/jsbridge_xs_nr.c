/*
 * PlutoBrowser — jsbridge_xs_nr.c
 * XS (Moddable) 9.5.0 engine implementation behind the JsEngineImpl
 * vtable (engine vendored STOCK under Source/js/xs_moddable — never
 * modified; compiled per-file via the Makefile with our
 * Source/html/xs_platform.h routed through XS's own XSPLATFORM hook).
 *
 * XS embedding facts this file relies on (all verified against the
 * stock 9.5.0 sources/includes):
 *   - xsCreateMachine(&creation, name, context) allocates one machine;
 *     xsDeleteMachine frees everything the page allocated. One machine
 *     per page render, like the other bridges. xsGetContext carries
 *     the JsBridge*.
 *   - Host code brackets engine calls: xsBeginHostExit/xsEndHostExit
 *     (sets exitStatus = xsNormalExit (-1) on entry; the else-branch on
 *     unwind does NOT re-call fxAbort — the plain xsBeginHost does,
 *     which would loop forever with our fxAbort = longjmp). Inside the
 *     bracket, the macro-local `the` enables the xs* API macros.
 *   - Scripts run exactly like the stock tool host: fxParseScript(the,
 *     &txStringCStream, fxStringCGetter, mxProgramFlag) then fxRunScript
 *     (the, script, mxThis, C_NULL, C_NULL, C_NULL, mxProgram.value.reference).
 *   - Script throws unwind to the enclosing xsTry; unhandled aborts
 *     unwind to the xsBeginHostExit. Both are contained and mirrored to
 *     b->lastError; the browser task never dies.
 *   - Engine aborts (OOM, C-stack overflow, metering limit, unhandled
 *     rejection, "no more keys", …) call fxAbort. The stock default
 *     printf()s and exits the PROCESS, so xs_platform.h sets
 *     mxUseDefaultAbort=0 and this file defines fxAbort: record the
 *     status + fxExitToHost (the stock ESP32 device platform's
 *     MODDEF_XS_ABORT_EXITTOHOST pattern). fxExitToHost longjmps to the
 *     OUTERMOST jump, which is always one of our xsBeginHostExit
 *     brackets → contained.
 *   - mxMetering: xsBeginMetering(machine, cb, step) bounds any script;
 *     the callback receives the bytecode-op count and returning 0
 *     aborts with XS_TOO_MUCH_COMPUTATION_EXIT (the muJS run-limit
 *     analogue; it also meters the parser and RegExp compiler).
 *   - C-stack safety: mxUseDefaultCStackLimit=0 → the HOST provides
 *     fxCStackLimit(); the engine's fxCheckCStack and the parser/
 *     RegExp parsers compare against it and abort with
 *     XS_NATIVE_STACK_OVERFLOW_EXIT before blowing the 61.8KB
 *     game-task stack on device.
 *   - Element wrappers: ONE prototype carries the accessors/methods
 *     (fxNextHostAccessorProperty + fxNextHostFunctionProperty);
 *     instances are created with fxNewHostInstance(the) while the
 *     prototype sits on the machine stack (it clones the prototype's
 *     trailing internal host slot), then get the DomNode* via
 *     fxSetHostData. Wrappers are rebuilt on demand (never cached), so
 *     GC never needs to trace them.
 *   - Listener functions are pinned with xsRemember (roots the C-side
 *     xsSlot against GC) and xsForget at close.
 *   - fxRunLoop is TOOL code (xs/tools/xst.c) — this file provides the
 *     equivalent microtask drain: fxEndJob + fxRunPromiseJobs loop,
 *     then fxCheckUnhandledRejections(machine, 1).
 *
 * DOM surface is identical to jsbridge_mujs.c / jsbridge_duktape.c /
 * jsbridge_quickjs.c (same accessors, methods, budget + write capture).
 */
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <stdarg.h>
#include "../core/pluto_mem.h"
#include "../core/pluto_spill.h"
#if !defined(TARGET_PLAYDATE)
#include <execinfo.h> /* A2: C backtrace at meter overruns (host labs only) */
#endif

#include "jsbridge.h"
#include "jsbridge_internal.h"
#include "jsbridge_bundler.h"
#include "../core/logger.h"
#include "../render/style.h"
#include "../core/tasks.h"
#include "../core/url.h"
extern void pluto_free(void *p);
#include "../util/strbuf.h"
#include "../html/dom.h"
#include "../html/webstorage.h"

/* No-recursion fork engine API — the exact include set of the canonical
 * host tool (xs/tools/xst.c) but pointed at the FORK: xsAll.h pulls
 * xsPlatform.h → our Source/html/xs_platform.h via -DINCLUDE_XSPLATFORM,
 * then the public xs.h macros. xsCommon.h includes xs_nr_rename.h first,
 * so every engine symbol used below lands on its _nr twin. */
#include "../js/xs_moddable_no_recursion/sources/xsAll.h"
#include "../js/xs_moddable_no_recursion/sources/xsScript.h"
#include "../js/xs_moddable_no_recursion/includes/xs.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_mem_realloc(NULL, (n))
#define JFree(p) pluto_mem_realloc((p), 0)

/* R13 product fix: per-parse parser-memory ceiling owned by the fork engine
 * (xsScript.c); 0 = unlimited. Set at engine boot on device. */
extern unsigned long fxNRParserTotalCap;
extern int fxNRParserCodegenCapped; /* R26g: cap covers codegen in attempt mode */

/* A2 forward decls: real queueMicrotask (host fn registered at engine
 * init; the flush runs from the xs_drain_jobs boundaries below). */
static void xs_queueMicrotask(xsMachine *the);
static int xs_flush_microtasks(JsBridge *b);

/* ── machine sizing (Playdate-sized; slots ≈ 32B, chunks are bytes) ──── */
#define XS_INIT_CHUNK (384u * 1024u)
#define XS_INC_CHUNK (192u * 1024u)
/* R30m DEVICE: 16384-slot (256KB) growth steps instead of 65536 (1MB).
 * Device run 16: the seg 407 mount died at tracked live=6327KB with only
 * 328KB of envelope left — the NEXT 1MB slot-heap step could not fit even
 * though the engine may have needed just a fraction of it. Each step
 * crossing near the wall overshoots by (step - actual need), so a 1MB step
 * wastes up to ~900KB of the 6.5MB envelope at exactly the wrong moment.
 * 256KB steps cap that waste at ~200KB. The R30c/R30i slot POOL (armed at
 * >=2.5MB headroom) already absorbs the mid-run churn that small steps
 * used to thrash on (run 4's 66 collects), so the collect-cycle cost of a
 * smaller step is paid by the pool, not the meter. Sim keeps 65536: no
 * funnel budget there, and the R30b cycle-count math still applies. */
#ifdef TARGET_PLAYDATE
#define XS_INIT_HEAP 8192u
#define XS_INC_HEAP 65536u /* R30p: back to runs-15/16-proven 1MB device steps */
#else
#define XS_INIT_HEAP 8192u
#define XS_INC_HEAP 65536u
#endif
/* R30b: slot-pool growth step. 2048 (the stock value) made the React
 * hydration pay a thrash cycle — free list empty → organic collect
 * (mark+sweep of the whole graph) → grow the step — every second of run
 * time: device run 4 measured 66 collects in 73s and died >10s into seg
 * 407 with two ~340ms collects/s. Steps scale DOWN the cycle COUNT:
 * cycles ≈ final slots / step, so the step must be large enough that one
 * page costs single-digit cycles. R30b v1 (16384) still cycled 1-2x/s at
 * 60K slots (run 5: 21 collects, died in seg 407's resume) because React's
 * transient churn empties ANY step within ~a second — what matters is not
 * refill volume per cycle but how MANY full-graph collects the whole run
 * must pay. 65536 (1MB) puts the entire run at ~6 cycles; each is one
 * 200-400ms collect in a distinct burst, far from any 10s frame. Pool
 * RAM worst case stays the pool the graph itself would occupy plus one
 * step (≈1MB transient) — well inside the 16MB device envelope. */
#define XS_INC_HEAP 65536u
/* JS value stack: 1024 slots threw "JavaScript stack overflow" before React
 * finished mounting (R14). 16384 was a provisional ceiling while the R22
 * tail appeared to recurse past 4096 — but that demand was the WC_FOR/WC_TRY
 * sync-skip fork bug (infinite-loop growth, peak 16370), not organic
 * recursion. R55b (2026-10-18), C4-cycle truth: the CURRENT site's react
 * RSC boot (turbopack chunks planned by jsbridge_bundler_plan, the
 * R-flight-data processing in the app-router scripts) aborts with slots
 * =4096 — sim log 18:57 twice per run: "script 11 aborted engine
 * (JavaScript stack overflow)" inside an ~10-frame .apply call chain with
 * below-anchor=-400B (the [cstack] anchor rows describe the C stack, NOT
 * the value stack — the slot cap was the gate). The R14-era field-proven
 * 16384 (C1 line: -DXS_STACK_COUNT=16384u carried every green C3/C4/C5
 * cycle) is the default again: 16384 slots ≈ 256KB one-time reserve at
 * machine create on ARM32 (fxAllocateSlots), inside the 7.68MB budget
 * ladder with room. The probe rows ([slotstack] overflow alloc /
 * interpreter overflow) are always-on, so a future site shape exceeding
 * this reports its real demand before any sizing decision.
 * A2 (bundler-run harness): modern framework bootstraps need a deeper
 * value stack than the legacy bootstrap sample — XS_STACK_COUNT is a
 * normal #ifndef override now; the bump harness defines a higher count
 * (-DXS_STACK_COUNT=16384). */
#ifndef XS_STACK_COUNT
#define XS_STACK_COUNT 16384u
#endif
/* A2: C-owned queueMicrotask depth (pinned fn slots — see
 * xs_flush_microtasks). The framework queues one microtask per async step;
 * browsers hold an unbounded queue, but each flush runs at most one batch
 * and deferred callbacks wait for the next machine boundary, so the slot
 * budget bounds device RAM, not fidelity. Overflow raises visibly. */
#define XS_MICRO_MAX 32
#define XS_INIT_KEYS 2048u
#define XS_INC_KEYS 256u
#define XS_NAME_MOD 127u
#define XS_SYMBOL_MOD 127u
#define XS_PARSER_BUF (16u * 1024u)
#define XS_PARSER_TBL 127u

/* ── R20: time-sliced split execution (watchdog-safe) ─────────────�
 * The device SDK watchdog reports a freeze when the run loop sees no
 * update() return for 10 seconds. Executing a ~96-segment bundle inside
 * one update() is inherently a >10s no-yield stretch, so the split runs
 * at most PLUTO_SPLIT_FRAME_BUDGET_MS of wall-clock work per frame and
 * resumes on the next frame through the engine `pump` vtable slot
 * (wired in main.c's timer-pump path, next to jsbridge_deferred_pump).
 * Budgets are checked BETWEEN segments (a segment is atomic); the
 * planner's segment cap keeps every individual segment's cost bounded. */
#ifndef PLUTO_SPLIT_FRAME_BUDGET_MS
#define PLUTO_SPLIT_FRAME_BUDGET_MS 250u
#endif
/* R27 stall-baseline instrument (sim-only builds): longest ATOMIC segment
 * run this split has seen — the seg-407 mount call is the one the device
 * watchdog kills at 10s. Reported once at split end. */
#if defined(PLUTO_SPLIT_SIM_FRAMES) && !defined(TARGET_PLAYDATE)
static unsigned long g_splitMaxRunMs = 0;
#endif
/* Inline span storage for the in-flight split plan (XsState.split). The
 * planner caps bundles at PLUTO_BUNDLER_MAX_SPANS entries, so this holds
 * any legal plan without heap ownership complexity. */
#ifndef PLUTO_SPLIT_SPANS_INLINE
#define PLUTO_SPLIT_SPANS_INLINE 2048
#endif

/* ── R22: stepwise (multi-frame) segment parse ───────────────�
 * The R20 frame budget holds BETWEEN segments; one segment is atomic.
 * That bound is honest only while every segment is small: a ~253KB
 * bootstrap tail parses in ~20s at the measured device engine rate
 * (~12.2KB/s) — far past the 10s watchdog — no matter how the plan is
 * sliced (the tail is one statement, so R20b statement slicing cannot
 * cut it). Segments above PLUTO_SPLIT_STEPWISE_MIN therefore take the
 * STEPWISE path: their parse is paused/resumed across frames through
 * the NR parser's pumpBudget trampoline gate (P_BUDGET_PAUSE in
 * xsSyntaxical.c), then hoist/bind/code and the VM run each get their
 * own frame. The per-frame parser step budget self-tunes from the
 * measured steps/ms to land near PLUTO_SPLIT_PUMP_TARGET_MS. */
#ifndef PLUTO_SPLIT_STEPWISE_MIN
#define PLUTO_SPLIT_STEPWISE_MIN (32u * 1024u)
#endif
#ifndef PLUTO_SPLIT_PUMP_TARGET_MS
#define PLUTO_SPLIT_PUMP_TARGET_MS 200u
#endif
#ifndef PLUTO_SPLIT_PUMP_BUDGET_START
#define PLUTO_SPLIT_PUMP_BUDGET_START 60000u
#endif
#ifndef PLUTO_SPLIT_PUMP_BUDGET_MIN
#define PLUTO_SPLIT_PUMP_BUDGET_MIN 5000u
#endif
#ifndef PLUTO_SPLIT_PUMP_BUDGET_MAX
#define PLUTO_SPLIT_PUMP_BUDGET_MAX 4000000u
#endif
/* Stepwise session states (XsState.split.step.state). */
#define PLUTO_STEP_PARSE 1 /* pump slices until curFrame drains */
#define PLUTO_STEP_CODE  2 /* one frame: hoist + bind + code gen */
#define PLUTO_STEP_RUN   3 /* one frame: fxRunScript + job drain */

/* Metering step (fxBeginMetering's txU8 interval; the engine checks every
 * interval << 16 bytecode units — one op = XS_CODE_METERING = 1<<16, so a
 * step of 8 = a check every 8 bytecode ops). Override with -DXS_METER_STEP=. */
#ifndef XS_METER_STEP
#define XS_METER_STEP 8
#endif

/* Run-limit: the meter callback receives the executed bytecode-op count
 * (meterIndex >> 16; one XS "meter op" = XS_CODE_METERING = 1<<16 raw
 * units) and returns 0 once it crosses XS_RUNLIMIT — same scale as the
 * muJS JSBRIDGE_RUNLIMIT statement counter (2,000,000). */
#ifndef XS_RUNLIMIT
#define XS_RUNLIMIT 2000000u
#endif

/* C-stack budget handed to the engine's stock fxCheckCStack + parser
 * guards. Device game-task stack is 61.8KB; keep the margin the QuickJS
 * bridge uses. Override via -DXS_CSTACK_LIMIT=. */
#ifdef TARGET_PLAYDATE
#define XS_CSTACK_LIMIT_DFL (36u * 1024u)
#else
/* Host/sim lab builds (-O0 + sanitizers): instrumented engine frames are
 * several-fold larger than thin -O2 ARM frames, so the 64KB budget tripped
 * "[xs] abort: native stack overflow" on the first trivial script (same
 * class as the QuickJS host budget fix). The host stack is 8MB; this is
 * only the engine's own guard bound — the device budget above is the one
 * that protects real hardware. */
#define XS_CSTACK_LIMIT_DFL (512u * 1024u)
#endif
#ifndef XS_CSTACK_LIMIT
#define XS_CSTACK_LIMIT XS_CSTACK_LIMIT_DFL
#endif

/* ── R28: RESUMABLE MOUNT (metering pause) ─────────────────
 * Device run 6 proved the RTOS watchdog kills the app when ONE script run
 * never returns to the SDK event loop for 10s — the React mount ran 111s
 * as a single atomic fxRunScript (segment boundaries feed the run loop;
 * nothing inside the mount does). The fork's metering check can now PARK
 * a run: machine registers stay mid-program in the machine, the burst
 * returns, the frame ends, and later frames resume the SAME run. Armed
 * for every ATOMIC split segment's run — segments that fit their budget
 * never pause (zero behavior change); the one long run parks. Off for
 * host harnesses (they own the synchronous one-call contract) and
 * kill-switched by -DPLUTO_NO_MOUNT_PAUSE. */
#if (defined(TARGET_PLAYDATE) || defined(PLUTO_SPLIT_SIM_FRAMES)) && \
    !defined(PLUTO_NO_MOUNT_PAUSE)
#define PLUTO_NR_MOUNT_PAUSE 1
#endif
/* Burst budget: how long ONE interpreter burst may run before the meter
 * check parks it. Device 2000ms << the 10s watchdog; the sim demo seam
 * (PLUTO_SPLIT_SIM_FRAMES) forces 2ms so the mount visibly spans frames. */
#ifndef PLUTO_MOUNT_BURST_MS
#if defined(TARGET_PLAYDATE)
#define PLUTO_MOUNT_BURST_MS 2000u
#else
#define PLUTO_MOUNT_BURST_MS 2u
#endif
#endif
/* Livelock cap (WATCHDOG_FIX_DESIGN.md §3): a mount bursting longer than
 * this is a runaway — the meter callback aborts containedly (engine
 * reset, page fails, browser lives). */
#ifndef PLUTO_MOUNT_DEADLINE_MS
#if defined(TARGET_PLAYDATE)
#define PLUTO_MOUNT_DEADLINE_MS 120000u
#else
#define PLUTO_MOUNT_DEADLINE_MS 30000u
#endif
#endif
/* R30n: in-mount GC policy — during a mount burst, when the funnel's
 * tracked headroom falls below this threshold, the meter callback runs a
 * non-compacting collect. Run 16 measured the mount death: burst 4 hit
 * "memory full" at headroom=328KB while most of the 4368→6327KB climb was
 * React's per-burst TRANSIENTS (R30m's between-burst collect only runs
 * AFTER the burst already crossed the boundary — too late when a single
 * burst's live+transients overshoots the envelope mid-run). Threshold
 * 600KB gives one 256KB heap step plus slack; the collect costs ~150-350ms
 * inside a 2000ms burst (the device had 7-15s of margin) and runs at most
 * once per 256KB of new churn. Host harnesses have no funnel budget
 * (headroom = SIZE_MAX sentinel → the 1GB check skips the branch).
 * SAFETY: this fires strictly between engine instructions (meter callback
 * = the engine's own allocation-triggered GC point), with nrNoCompact=1
 * routing to the non-compacting slot collect whose code-fixup walks are
 * off while a run is parked — the identical configuration the arm-time
 * top-up and R30m's between-burst collect already collect under. */
#define PLUTO_MOUNT_GC_HEADROOM_KB 600u
/* Minimum ms between in-mount collects: a burst can churn 256KB faster
 * than the threshold path can matter; this caps the collect rate so the
 * deadline budget is spent running, not sweeping. */
#define PLUTO_MOUNT_GC_MIN_INTERVAL_MS 250u
/* R28 device stall (2026-10-06): the RTOS watchdog kills at 10s, so the
 * 120s deadline can never fire first. The real livelock signature is a
 * pause request that stays DEFERRED past the burst end (a leaked
 * nrRunDepth defers every grant — the run never returns to the run
 * loop). Timers/XHR are gated off while a split owns the frame, so an
 * overrun this large can only be the leak. Abort containedly BEFORE the
 * watchdog. Must stay well under the 10s watchdog minus one burst. */
#ifndef PLUTO_MOUNT_OVERRUN_MS
#if defined(TARGET_PLAYDATE)
#define PLUTO_MOUNT_OVERRUN_MS 6000u
#else
#define PLUTO_MOUNT_OVERRUN_MS 1000u
#endif
#endif

static const xsCreation xs_creation = {
    XS_INIT_CHUNK,  /* initialChunkSize */
    XS_INC_CHUNK,   /* incrementalChunkSize */
    XS_INIT_HEAP,   /* initialHeapCount */
    XS_INC_HEAP,    /* incrementalHeapCount */
    XS_STACK_COUNT, /* stackCount (slots) */
    XS_INIT_KEYS,   /* initialKeyCount */
    XS_INC_KEYS,    /* incrementalKeyCount */
    XS_NAME_MOD,    /* nameModulo */
    XS_SYMBOL_MOD,  /* symbolModulo */
    XS_PARSER_BUF,  /* parserBufferSize */
    XS_PARSER_TBL,  /* parserTableModulo */
};

/* Engine-private state (b->implState). */
typedef struct
{
    xsMachine *machine;
    xsSlot elProto; /* element prototype (rooted) */
    xsSlot scriptProto; /* <script> element prototype (rooted; see build) */
    xsSlot sheetProto; /* CSSOM StyleSheet prototype (rooted; text-backed) */
    xsSlot urlProto; /* URL prototype (rooted; toString/toJSON) */
    /* Pinned listener functions; JsListener.ref holds the index.
     * xsRemember'd against GC; xsForget + release at close. */
    xsSlot fns[JSBRIDGE_LISTENERS_MAX];
    /* Pinned timer callbacks; JsTimer.ref holds the tfn[] slot index.
     * Same xsRemember/xsForget ownership contract as fns[]. */
    xsSlot tfn[JSBRIDGE_TIMERS_MAX];
    unsigned char tfnSet[JSBRIDGE_TIMERS_MAX]; /* slot holds a pin */
    /* A2: microtask queue (real queueMicrotask) — C-owned pinned fns,
     * flushed FLAT at machine boundaries (see xs_flush_microtasks). Never
     * dispatched through the promise machinery: the framework needs
     * queueMicrotask(fn) to complete synchronously WITHOUT running fn
     * nested inside its caller (a Promise-based JS shim reenters itself
     * against this engine's synchronous then dispatch). The queue is a RING
     * on PURPOSE: fxRemember repurposes the pinned slot struct's ->next as
     * the C_root chain node, so an entry must be pinned exactly once and
     * its struct NEVER copied while pinned (shifting copies corrupts the
     * pin chain — fxForget then scans a cycle forever). */
    xsSlot mtQ[XS_MICRO_MAX];
    int mtHead;  /* dequeue index (ring) */
    int mtCount; /* queued fns */
    /* A2: pending resource-load fires (replay-cached asset semantics).
     * react-dom's commit gate (suspendResource / insertStylesheetIntoRoot)
     * counts pending stylesheets and waits for the created <link>'s load
     * OR error event before unsuspending; without a bounded delivery the
     * commit stays parked on the 60s timeout and the page never reaches
     * the listener-binding stage. The replay environment models the
     * page's own assets as already cached and complete, so queued fires
     * deliver ONE synthetic 'load' per link element at the next
     * microcheckpoint. Elements stay raw DomNode pointers: the tree lives
     * in the document's arena and is never compacted while the engine is
     * attached (close resets the queue first). */
#define XS_RESFIRE_MAX 8
    DomNode *resFire[XS_RESFIRE_MAX]; /* elements awaiting a load fire */
    int resFireCount;
    /* Lazily-created dispatcher host fn (queued into mtQ; pinned once at
     * creation and re-Remember'd per enqueue via its own ring slot — the
     * re-pin is legal because each ring slot is a distinct chain node). */
    xsSlot resFireFn;
    int resFireFnSet;
    /* XHR + fetch pins (same ownership contract; fetch uses the same
     * xfn[] slots — XS has no native Promise, so only XMLHttpRequest). */
    xsSlot xfn[JSBRIDGE_XHR_MAX]; /* completion handlers / promise resolve */
    xsSlot xobj[JSBRIDGE_XHR_MAX]; /* wrapper objects (this) */
    unsigned char xfnSet[JSBRIDGE_XHR_MAX];
    unsigned char xobjSet[JSBRIDGE_XHR_MAX];
    xsSlot xhrProto; /* XMLHttpRequest prototype (rooted) */
    /* API-P8: the Worker contained-stub lane (mirror of the xhr pins —
     * the OBJECT is pinned at construct; deliveries read the CURRENT
     * onerror property off it, browser at-fire semantics). */
    xsSlot workerProto; /* Worker prototype (rooted at init) */
    xsSlot fdProto;     /* Forms-Phase 5: FormData prototype (rooted) */
    xsSlot fdiProto;    /* FormData iterator prototype (rooted) */
    xsSlot wobj[JSBRIDGE_WORKER_MAX]; /* stub wrappers (delivery `this`) */
    unsigned char wobjSet[JSBRIDGE_WORKER_MAX];
    /* API-P3: per-entry history.state pins, parallel to the bridge's
     * histUrl journal (entry i ↔ histState[i]). Slots pin against GC
     * like the listener ring; replaceState re-pins over the slot. */
    xsSlot histState[JSBRIDGE_HIST_MAX];
    unsigned char histStateSet[JSBRIDGE_HIST_MAX];
    /* Pending back-stack deliveries, latched by back/forward/go and
     * drained at the next engine-pump frame (post-run, never mid-eval):
     * popstate carries the new entry's state; hashchange fires only when
     * the entry hash actually moved (old/new URLs recorded at move
     * time). */
    unsigned char histPendPop;
    unsigned char histPendHash;
    char histFromUrl[JSBRIDGE_HIST_URL_MAX];
    char histToUrl[JSBRIDGE_HIST_URL_MAX];
    /* B3: per-node ELEMENT WRAPPER cache — the browser identity model.
     * xs_push_element used to build a FRESH wrapper per push, so a wrapper
     * react wrote its fiber key onto (node[internalInstanceKey] = fiber)
     * was invisible to the NEXT wrapper pushed for the same node, and the
     * delegated dispatch's getClosestInstanceFromNode(target) found
     * nothing. One pinned wrapper per node (xsRemember'd at insert,
     * xsForget at close) makes react's own property writes DURABLE — plain
     * browser semantics: an Element object identity is stable for the
     * document's lifetime. Fixed reserve: over-cap pushes fall back to
     * fresh wrappers (the old behavior), noted once in the trace. */
#define XS_ELW_CAP 1024
    DomNode *elwNode[XS_ELW_CAP];
    xsSlot elwWrap[XS_ELW_CAP]; /* pinned IN the slot (see st->fns note) */
    int elwCount;
    int elwOverflow; /* set once when the reserve is exhausted */
    /* R20: in-flight time-sliced bundler-monolith split (see
     * xs_run_script_split below). plan.spans points at spansBuf and the
     * source points INTO the live document's rawHtml buffer (JS source
     * spans are never mutated in place and page teardown always runs
     * js_doc_close → engine close before the doc is freed). Owned by ONE
     * bridge instance; freed only here. nSpans < 0 invalidates the plan
     * without leaking. */
    struct
    {
        int active;          /* pump has work to resume */
        int nextSeg;         /* first segment still to run */
        int okSegs;          /* admission result accounting (run so far) */
        int failSegs;
        int gcRetries;       /* R29: segments admitted after GC recovery */
        int gcTried;         /* R29: GC+resync tried since last admission */
        unsigned long deadlineMs; /* per-frame budget clock (SDK ms) */
        size_t srcLen;       /* length of the owned source copy */
        char *src;           /* OWNED JMalloc copy of the script source.
         * The multi-frame split OUTLIVES the call that started it, so it
         * cannot borrow a caller span: on-device the page is snapshotted
         * + freed right after the first pump slice (render_done), and a
         * borrowed span into the doc's rawHtml then reads freed bytes —
         * later segments parse garbage ("invalid character 0/144",
         * "missing property": CAUGHT BY DEVICE TESTING 2026-10-04; the
         * sim harness keeps the doc alive through document_free and so
         * masked it). Copied once here; freed by xs_split_invalidate. */
        PlutoBundlerSpan spansBuf[PLUTO_SPLIT_SPANS_INLINE];
        PlutoBundlerPlan plan; /* plan.spans = spansBuf (compact copy) */
    } split;
    /* R22: stepwise (multi-frame) segment inside the split — parse
     * slices resume across frames (see PLUTO_SPLIT_STEPWISE_* above).
     * The parser is HEAP-resident (a stack txParser would die between
     * frames); its chunks and segBuf live until the segment finishes or
     * the session is invalidated. `css` points at segBuf. */
    struct
    {
        int active;      /* stepwise session in flight */
        int state;       /* PLUTO_STEP_PARSE / CODE / RUN */
        int seg;         /* segment index (logs) */
        int frames;      /* slices consumed (diagnostics) */
        int errs0;       /* b->errs at segment start */
        int restarts;    /* R30k: fresh-parse restarts used (cap 2) */
        unsigned long pumpBudget;   /* parser steps allowed this frame */
        unsigned long emaStepsPerMs; /* self-tuning estimate (0 = unset) */
        unsigned long savedParserCap; /* admission-scoped cap to restore */
        unsigned long savedBudget;   /* R56: funnel floor raised across the
                                      * codegen+run window (device only);
                                      * restored at xs_step_discard */
        char *segBuf;    /* JMalloc'd segment source (owned) */
        long segLen;     /* emitted segment length */
        txParser *parser;   /* heap-resident parser (fxInitializeParser) */
        txParserJump jump;  /* armed while a parse/code slice runs */
        txScript *script;   /* produced by CODE, consumed by RUN */
        txStringCStream css;/* stream ctx for the parse (points at segBuf) */
    } step;
    /* R28: resumable mount session — one atomic split segment's RUN that
     * overran its burst budget and parked mid-program (see the engine's
     * nrPaused in xsAll.h). The machine registers parked at the meter
     * check are saved HERE because the host bracket epilogues restore
     * them to bracket-entry on the way out; they are re-applied before
     * every resume burst. The parked region stays inside the GC root
     * scan only while the->stack is parked (re-applied), so the bridge
     * owns the register lifecycle across the parked window. */
    struct
    {
        int active;  /* mount session in flight */
        int parked;  /* machine registers parked mid-run */
        int seg;     /* segment index (accounting + logs) */
        int errs0;   /* b->errs at session start */
        unsigned bursts; /* bursts consumed (diagnostics) */
        unsigned long deadlineMs; /* absolute livelock cap (SDK ms) */
        unsigned long burstEndMs; /* absolute end of the current burst */
        int deadlineAbort; /* diag: session ended via nrPauseAbort */
        txScript *script; /* OWNED program; freed at completion/abort */
        txSlot *pStack;   /* parked machine registers (saved at pause) */
        txSlot *pScope;
        txSlot *pFrame;
        txByte *pCode;
        /* R54: chunk-STABLE encoding of the parked code pointer. The R51
         * boundary COMPACT rewrites chunk->temporary and fixes up code
         * pointers reachable from the frame/jump chains — but pCode is a
         * BRIDGE-side copy, and the observed ~1-in-25 device corruption
         * (runs 36/45 E0; runs 51/52 gate catches, code=0x287b) shows the
         * raw pointer can come out stale/moved even though the capture
         * happens AFTER the compact. Storing the pointer as
         * (owning-block, offset-into-block) makes the parked code position
         * independent of any later chunk rebuild: a compaction that moves
         * the chunk updates the BLOCK's base implicitly (we re-walk
         * firstBlock at resume), and the offset within the chunk stays
         * valid because compaction relocates whole chunks with their
         * contents. At resume we re-derive the pointer from the CURRENT
         * block chain; the R48b gate still validates the derived pointer
         * before use. iCode keeps the same treatment for symmetry. */
        void *pCodeBlock; /* txBlock* owning the parked code position */
        unsigned long pCodeOffset; /* byte offset within that block's data */
        txSlot *iStack;   /* pre-mount IDLE registers (captured at burst 1).
         * After the mount COMPLETES, the bracket epilogues restore the
         * parked registers (their entry snapshots) — the->frame must not
         * keep walking the dead program frames once the script (and its
         * c_malloc'd codeBuffer) is freed: the GC sweep's code-fixup walk
         * starts at the->frame following ->next and raw-marks FUNCTION
         * slots. Restore the idle registers on every non-parked outcome. */
        txSlot *iScope;
        txSlot *iFrame;
        txByte *iCode;
    } mount;
    /* C4 (fetch shim): one-shot latch. The shim is an engine-owned script
     * that defines global `fetch`/`AbortSignal` over the engine's own
     * XMLHttpRequest + native Promise; it runs once, before the page's
     * first script, via xs_run_script's injection point. */
    unsigned char fetchShimDone;
} XsState;

/* ── R55b value-stack forensics (shared by sim + device) ────────────
 * The fork's interpreter calls this at the failing bounded push
 * (mxAllocStack underflow in xsRun.c / fxOverflow native push overflow
 * in xsAPI.c) right before the abort. want = slot count the push asked
 * for; depth = live value-stack depth (stackTop - stack) at the abort
 * point. Defined at file scope UNCONDITIONALLY (outside the
 * PLUTO_NO_STOCK_XS guard) because the fork ENGINE compiles and runs on
 * device builds too — only the hook definitions are single-binary.
 * XS_STACK_COUNT sizing reads this row, not the [cstack] anchor numbers,
 * which describe the C stack and do not apply to the value stack. */
void pluto_fork_stack_overflow_probe(long want, long depth)
{
    (void)want;
    (void)depth;
    logger_log("[slotstack] overflow alloc: want=%ld depth=%ld", want, depth);
}

/* ── host-provided platform functions (mxUseDefault* = 0) ────────────

/* These five hooks (fxAbort, fxCStackLimit, fxQueuePromiseJobs,
 * fxCreateMachinePlatform, fxDeleteMachinePlatform) are engine-wide
 * symbols: one definition per BINARY is expected by Moddable's platform
 * model, and the stock bridge (jsbridge_xs.c) defines identical ones.
 * Device builds link both bridges, so the fork's copies compile only when
 * the stock engine is absent (simulator one-shot dylib). */
#ifndef PLUTO_NO_STOCK_XS
#else

/* R37/depth=0 abort forensics: the anchor the engine's fxCStackLimit
 * hands out, kept file-global so fxAbort can log the SP distance at the
 * moment a guard fires (real deep recursion ⇒ delta ≈ the budget;
 * a cross-region/bogus compare ⇒ tiny or negative delta). */
static char *g_cstackAnchor = NULL;

#ifdef PLUTO_THROW_LOG
/* Throw-trace companion (PLUTO_THROW_LOG harness build only): the fork's
 * fxThrowMessage logging reads the RUNNING script number from here; both
 * run entrypoints keep it current. */
int g_pluto_throw_script = 0;
#endif

void fxAbort(txMachine *the, int status)
{
    /* fxAbortString is an internal (renamed to _nr) in the fork — the
     * bridge keeps its own tiny name table for the log line. R26h: aligned
     * with the fork's exit enum (xsCommon.h) — the old table mapped status
     * 1 (NOT_ENOUGH_MEMORY) to "unhandled exception", poisoning OOM logs. */
    static const char *const kAbortNames[] = {
        "debugger", "not enough memory", "JavaScript stack overflow",
        "fatal", "dead strip", "unhandled exception", "no more keys",
        "too much computation", "unhandled rejection",
        "native stack overflow", "incompatible mod",
        "parser state corruption" };
    const char *what = (status >= 0 && (unsigned)status <
                        sizeof(kAbortNames) / sizeof(kAbortNames[0]))
                           ? kAbortNames[status]
                           : "unknown";
    if (the->exitStatus == xsNormalExit)
    {
        the->exitStatus = status;
    }
    logger_log("[xs] abort: %s", what);
    {
        /* SP probe: address of this frame's local ≈ the SP at the abort.
         * below-anchor > 0 ⇒ that many bytes BELOW the fxCStackLimit
         * anchor (legit deep chain ≈ the budget value); ≤ 0 ⇒ the guard
         * compared across regions or the anchor is bogus. */
        char spProbe;
        long below = g_cstackAnchor
                         ? (long)(g_cstackAnchor - (char *)&spProbe)
                         : -1L;
        logger_log("[cstack] abort %s: sp~%p anchor=%p below-anchor=%+ldB",
                   what, (void *)&spProbe, (void *)g_cstackAnchor, below);
    }
    if (status == XS_JAVASCRIPT_STACK_OVERFLOW_EXIT && the)
    {
        /* A2 forensics: a JS-stack exhaustion is a recursion-shape question
         * — walk the frame chain (slot->next links the previous frame) and
         * log the deepest callers. Read-only (names come from the engine's
         * symbol tables), safe at abort time. */
        txSlot *f = the->frame;
        int depth = 0;
        while (f && depth < 14)
        {
            char name[96];
            name[0] = '\0';
            fxBufferFrameName(the, name, sizeof(name), f, "");
            logger_log("[cstack] overflow frame[%d] %s%s", depth,
                       name[0] ? name : "?", depth == 0 ? " <-deepest" : "");
            f = f->next;
            depth++;
        }
    }
    if (status == 11) /* XS_PUMP_CORRUPTION_EXIT — fork diagnostics */
    {
        extern long gXSNRFaultKind;
        extern void *gXSNRFaultFrame;
        extern void *gXSNRFaultChunkFirst;
        extern void *gXSNRFaultChunkCur;
        extern char gXSNRFaultSym[];
        logger_log("[xs] pump fault: kind=%ld frame=%p chunkFirst=%p chunkCur=%p sym=\"%s\"",
                   gXSNRFaultKind, gXSNRFaultFrame, gXSNRFaultChunkFirst,
                   gXSNRFaultChunkCur, gXSNRFaultSym);
    }
    fxExitToHost(the);	/* macro → fxExitToHost_nr via xs_nr_rename.h */
}

/* sanity: the fork renames internals; if this fails the include order
 * changed and the bridge would silently link against the stock engine */
_Static_assert(1, "");

/* fxCStackLimit: host-provided because xs_platform.h sets
 * mxUseDefaultCStackLimit=0 (the stock default returns C_NULL on bare
 * ARM, which disables the check entirely). The engine compares the
 * current stack pointer against this (downward growth) in fxCheckCStack,
 * the JS parser and the RegExp parser. Called per machine/parser init on
 * whichever task is running, so one formula serves sim + device. */
char *fxCStackLimit(void)
{
    static char *base = NULL;
    char probe;
    (void)probe;
    if (!base)
    {
        base = &probe;
        g_cstackAnchor = base;
        /* One line per process: the anchor the parser/machine/regex guards
         * will compare every SP against (device run Anonymous evidence:
         * without it a bogus anchor was undiagnosable). */
        logger_log("[cstack] anchor=%p budget=%uKB",
                   (void *)base, XS_CSTACK_LIMIT / 1024u);
    }
    else
    {
        g_cstackAnchor = base;
    }
    return base - XS_CSTACK_LIMIT;
}

/* fxQueuePromiseJobs: no scheduler to wake (we drain synchronously). */
void fxQueuePromiseJobs(txMachine *the)
{
    (void)the;
}

/* fxCreateMachinePlatform / fxDeleteMachinePlatform: the stock default
 * platform struct is just `void* host;` — nothing to do. */
void fxCreateMachinePlatform(txMachine *the)
{
    (void)the;
}

void fxDeleteMachinePlatform(txMachine *the)
{
    (void)the;
}

#endif /* PLUTO_NO_STOCK_XS */

/* ── bridge <-> machine plumbing ────────────────────� */

static JsBridge *bridge_of(xsMachine *the)
{
    return (JsBridge *)xsGetContext(the);
}

static XsState *state_of(JsBridge *b)
{
    return (XsState *)b->implState;
}

static xsMachine *machine_of(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    return st ? st->machine : NULL;
}

/* DOM-mutation budget, XS flavor: throws (contained) when exhausted. */
#define BUDGET_OR_THROW(b)                      \
    do                                          \
    {                                           \
        if (!budget_take(b))                    \
        {                                       \
            xsTypeError("script did too much"); \
        }                                       \
    } while (0)

/* Copy an engine string out to the SDK heap (survives GC + next calls). */
static char *to_cstring(xsMachine *the, xsSlot slot)
{
    /* xsToString materializes into engine storage; copy before reuse. */
    char *s = xsToString(slot);
    size_t n = strlen(s) + 1;
    char *out = (char *)JMalloc(n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

/* ── element wrappers ──────────────────────── */

static xsSlot xs_push_element(JsBridge *b, DomNode *node);

/* Forms-Phase 4 forward (defined with the dispatcher): the kind-0
 * bubbling click dispatch used by el.click(). */
static int xs_dispatch_element_event(JsBridge *b, const void *node,
                                     const char *type);

/* Host-data setter with call-site logging. The engine's
 * "C: xsSetHostData: not a host object" throw carries no context, so name
 * the site before it validates (host-only: the per-element path is too hot
 * for the device log). */
static void xs_set_host_data_site(xsMachine *the, const char *site, xsSlot slot,
                                  void *data)
{
#ifndef TARGET_PLAYDATE
    logger_log("[xs] setHostData %s", site);
#endif
    xsSetHostData(slot, data);
}

/* Host data is the DomNode*; xsGetHostDataIf returns NULL for non-host
 * slots (this = undefined/string/number is legal JS on wrappers' props). */
static DomNode *this_node(xsMachine *the)
{
    return (DomNode *)xsGetHostDataIf(xsThis);
}

static DomNode *arg_node(xsMachine *the, xsIntegerValue i)
{
    return (DomNode *)xsGetHostDataIf(xsArg(i));
}

/* Method/property definition helpers (defined with the prototype builder). */
static void def_fn(xsMachine *the, xsSlot obj, const char *name,
                   xsCallback fn, int length);
static void def_val(xsMachine *the, xsSlot obj, const char *name,
                    xsSlot value);

/* Element prototype: built once at init (see xs_build_el_proto), accessors
 * + methods live there; instances inherit through the prototype chain. */
static void xs_build_el_proto(JsBridge *b);
static void xs_build_xhr_proto(JsBridge *b);
static void xs_xmlhttprequest_new(xsMachine *the);
static void xs_build_worker_proto(JsBridge *b);
static void xs_worker_new(xsMachine *the);
static void xs_worker_postMessage(xsMachine *the);
static void xs_build_formdata_proto(JsBridge *b); /* Forms-Phase 5 */
static void xs_formdata_new(xsMachine *the);
static void xs_worker_terminate(xsMachine *the);

/* API-P10: dynamic-import bridge native — status-string reply protocol
 * (see jsbridge_internal.h); delegates to the shared core in jsbridge.c. */
static void xs_dyn_request(xsMachine *the);

/* ── accessors (this = element wrapper) ────────────────── */

static void xs_el_get_tagName(xsMachine *the)
{
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        /* HTML nodeName per kind: text → "#text", comment → "#comment" */
        xsResult = xsString(n && n->kind == DOM_COMMENT ? "#comment"
                            : n && n->kind == DOM_TEXT  ? "#text"
                                                        : "");
        return;
    }
    char up[32];
    size_t tl = strlen(n->tag);
    if (tl < sizeof(up))
    {
        for (size_t i = 0; i < tl; i++)
        {
            up[i] = (n->tag[i] >= 'a' && n->tag[i] <= 'z')
                        ? (char)(n->tag[i] - 32)
                        : n->tag[i];
        }
        up[tl] = '\0';
        xsResult = xsString(up);
    }
    else
    {
        xsResult = xsString(n->tag);
    }
}

static void xs_el_get_id(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *v = n ? dom_get_attr(n, "id") : NULL;
    xsResult = xsString(v ? v : "");
}

/* Forms-Phase 2: single-select live-state reader (the pickers' reporting
 * road). Two helpers, both read-only:
 *   - jsb_select_index: the browser's 0-based selectedIndex (-1 none),
 *     from the LIVE DocBlock (the tap lane's own store — the renderer
 *     draws it, the block-aware serializer submits it). A MULTIPLE
 *     select reports the FIRST selected option in order (browser
 *     shape), not the device's cycle-arrival index. No block
 *     (unrendered control) → markup fallback: the authored `selected`
 *     option, else the first option child (browser default).
 *   - the VALUE road (in xs_el_get_value): the selected option's value
 *     attribute, else its option text (browser-default option
 *     semantics — a valueless <option> option's value IS its text).
 * Phase 2 is READS ONLY: selectedIndex/option.selected writes stay in
 * later phases per the stamped roadmap. */
/* Forms-Phase 2 forwards: the select readers and the options collector
 * (both defined later in the file, along the Phase-1 helper idiom). */
static DocBlock *jsb_block_of_node(JsBridge *b, DomNode *n);
static void xs_collect_tags_into(JsBridge *b, DomNode *start, char *tag);

static int jsb_select_index(JsBridge *b, DomNode *n)
{
    const char *tag = (n && n->kind == DOM_ELEMENT) ? n->tag : NULL;
    if (!tag || strcmp(tag, "select") != 0)
    {
        return INT_MIN; /* sentinel: not a select at all */
    }
    DocBlock *blk = jsb_block_of_node(b, n);
    if (blk && blk->options && blk->optionCount > 0)
    {
        if (blk->multiple)
        {
            for (int k = 0; k < blk->optionCount; k++)
            {
                DocOption *o = blk->options[k];
                if (o && !o->group && o->selected)
                {
                    return k;
                }
            }
            return -1; /* nothing selected at all */
        }
        int si = blk->selectedIndex;
        return (si >= 1 && si <= blk->optionCount) ? si - 1 : -1;
    }
    /* markup fallback over direct <option> children (the block road
     * already covers optgroup-nested options via the walker) */
    int sel = -1, first = -1, k = 0;
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *c = n->children[i];
        if (!c || c->kind != DOM_ELEMENT || !c->tag ||
            strcmp(c->tag, "option") != 0)
        {
            continue;
        }
        if (first < 0)
        {
            first = k;
        }
        if (sel < 0 && dom_get_attr(c, "selected"))
        {
            sel = k;
        }
        k++;
    }
    return sel >= 0 ? sel : first;
}

/* nth direct <option> child of a select node (the fallback road's twin
 * of the walker's options[] order). NULL when the select has fewer. */
static DomNode *jsb_option_node_at(DomNode *sel, int idx)
{
    int k = 0;
    for (int i = 0; i < sel->childCount; i++)
    {
        DomNode *c = sel->children[i];
        if (c && c->kind == DOM_ELEMENT && c->tag &&
            strcmp(c->tag, "option") == 0)
        {
            if (k == idx)
            {
                return c;
            }
            k++;
        }
    }
    return NULL;
}

/* Forms-Phase 2: select.selectedIndex — the browser's 0-based read of
 * the LIVE pick (see jsb_select_index). Non-select elements read
 * undefined (the browser's shape has only the select carrying it). */
static void xs_el_get_selectedIndex(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    int idx = jsb_select_index(b, n);
    if (idx == INT_MIN)
    {
        xsResult = xsUndefined;
        return;
    }
    xsResult = xsInteger(idx);
}

/* Forms-Phase 2: select.options — ALL option-element wrappers below the
 * select (browsers: every <option>, including the ones optgroups
 * hold — the group headers themselves are not options). Fresh array
 * per access, like the classList family. Non-selects read undefined. */
static void xs_el_get_options(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        strcmp(n->tag, "select") != 0)
    {
        xsResult = xsUndefined;
        return;
    }
    xs_collect_tags_into(b, n, "option");
}

/* C4: form-control `value` property (react's controlled-input contract:
 * e.target.value on read, .value = "..." on write). Stored on the node's
 * `value` ATTRIBUTE — the same store dom_set_attr feeds and the block
 * walker mirrors — so re-renders that rebuild nodes keep carrying the
 * typed text across dispatches.
 * Forms-Phase 1: kind-aware for <textarea> — the browser's value begins
 * as the element's text content; once a value attribute exists it wins
 * (the serializer rule: fb_field_value reads the attr first, the DOM
 * text falls back). Read = attr-else-text; write = the attr (the live
 * typed-text store, so a later submit serializes the set value). */
static void xs_el_get_value(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    const char *v = n ? dom_get_attr(n, "value") : NULL;
#if defined(PLUTO_HYDRATE_TRACE)
    logger_log("[val] get tag=%s → '%.20s'",
               (n && n->kind == DOM_ELEMENT && n->tag) ? n->tag : "?",
               v ? v : "");
#endif
    if (n && n->tag && strcmp(n->tag, "select") == 0)
    {
        /* Forms-Phase 2: the picker reporting road — the browser's
         * select.value is the selected option's shape (value attr, else
         * the option text), never the element's own attribute. */
        int idx = jsb_select_index(b, n);
        if (idx < 0 || idx == INT_MIN)
        {
            xsResult = xsString(v ? v : "");
            return;
        }
        DocBlock *blk = jsb_block_of_node(b, n);
        if (blk && blk->options && idx < blk->optionCount && blk->options[idx])
        {
            DocOption *o = blk->options[idx];
            xsResult = xsString((o->value && o->value[0]) ? o->value
                                                          : (o->text
                                                                 ? o->text
                                                                 : ""));
            return;
        }
        DomNode *optn = jsb_option_node_at(n, idx);
        if (optn)
        {
            const char *ov = dom_get_attr(optn, "value");
            if (ov && ov[0])
            {
                xsResult = xsString(ov);
                return;
            }
            char obuf[512];
            doc_concat_node_text(optn, obuf, sizeof(obuf));
            xsResult = xsString(obuf);
            return;
        }
        xsResult = xsString(v ? v : "");
        return;
    }
    if (!v && n && n->tag && strcmp(n->tag, "textarea") == 0)
    {
        /* browser truth: a valueless textarea's value = its text content */
        char buf[512];
        doc_concat_node_text(n, buf, sizeof(buf));
        (void)b;
        xsResult = xsString(buf);
        return;
    }
    xsResult = xsString(v ? v : "");
}

static void xs_el_set_value(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
#if defined(PLUTO_HYDRATE_TRACE)
        logger_log("[val] set tag=%s '%.20s'",
                   n->kind == DOM_ELEMENT && n->tag ? n->tag : "?", v);
#endif
        int rc = dom_set_attr(b->dom, n, "value", v);
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("value assignment failed");
        }
    }
}

/* URL decomposition attributes on HTML URL-bearing elements. */
static void xs_el_get_url_part(xsMachine *the, int part)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    const char *href = n ? dom_get_attr(n, "href") : NULL;
    char *absolute = href ? url_resolve(b->doc->baseUrl, href) : NULL;
    UrlParsed u;
    int ok = absolute && url_parse(absolute, &u) == 0;
    char buf[264] = "";
    if (ok) {
        int defaultPort = (u.isSsl && u.port == 443) || (!u.isSsl && u.port == 80);
        switch (part) {
        case 0: break;
        case 1: snprintf(buf, sizeof(buf), "%s:", u.scheme); break;
        case 2: if (defaultPort) snprintf(buf, sizeof(buf), "%s", u.host); else snprintf(buf, sizeof(buf), "%s:%d", u.host, u.port); break;
        case 3: snprintf(buf, sizeof(buf), "%s", u.host); break;
        case 4: if (!defaultPort) snprintf(buf, sizeof(buf), "%d", u.port); break;
        case 5: snprintf(buf, sizeof(buf), "%s", u.path); break;
        case 6: snprintf(buf, sizeof(buf), "%s%s", u.query[0] ? "?" : "", u.query); break;
        case 7: snprintf(buf, sizeof(buf), "%s%s", u.hash[0] ? "#" : "", u.hash); break;
        }
    }
    xsResult = xsString(part == 0 && absolute ? absolute : buf);
    if (absolute) pluto_free(absolute);
}
#define XS_URL_GETTER(name, part) static void xs_el_get_##name(xsMachine *the) { xs_el_get_url_part(the, part); }
XS_URL_GETTER(href, 0)
XS_URL_GETTER(protocol, 1)
XS_URL_GETTER(host, 2)
XS_URL_GETTER(hostname, 3)
XS_URL_GETTER(port, 4)
XS_URL_GETTER(pathname, 5)
XS_URL_GETTER(search, 6)
XS_URL_GETTER(hash, 7)
#undef XS_URL_GETTER
static void xs_el_set_href(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *value = to_cstring(the, xsArg(0));
    if (!value) xsTypeError("out of memory");
    int rc = n ? dom_set_attr(b->dom, n, "href", value) : -1;
    JFree(value);
    if (rc) xsTypeError("href assignment failed");
}

/* src: the src-bearing URL attribute (script/img/iframe), mirror of the
 * href getter but backed by the "src" attribute. Like a browser's .src
 * IDL attribute, page code sees the page-base-RESOLVED absolute URL —
 * bundler loaders pass document.currentScript.src straight into
 * new URL(); getAttribute("src") remains the raw text. */
static void xs_el_get_src(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    const char *href = n ? dom_get_attr(n, "src") : NULL;
    char *absolute = href ? url_resolve(b->doc->baseUrl, href) : NULL;
    xsResult = xsString(absolute ? absolute : (href ? href : ""));
    if (absolute)
    {
        pluto_free(absolute);
    }
}

static void xs_el_set_src(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *value = to_cstring(the, xsArg(0));
    if (!value) xsTypeError("out of memory");
    int rc = n ? dom_set_attr(b->dom, n, "src", value) : -1;
    JFree(value);
    if (rc) xsTypeError("src assignment failed");
}

static void xs_el_get_textContent(xsMachine *the)
{
    DomNode *n = this_node(the);
    char buf[512];
    if (n)
    {
        doc_concat_node_text(n, buf, sizeof(buf));
        xsResult = xsString(buf);
    }
    else
    {
        xsResult = xsString("");
    }
}

/* REAL markup serializer (browser parity). Next.js's root error boundary
 * snapshots `document.documentElement.innerHTML` and re-renders
 * <html dangerouslySetInnerHTML={{__html: snapshot}}> on a hydration
 * fallback; with the old text-degraded getter the snapshot was plain body
 * text and that assignment erased head+body (the replayed hydration gap). */
static void xs_el_get_innerHTML(xsMachine *the)
{
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        xsResult = xsString("");
        return;
    }
    char *s = NULL;
    size_t slen = 0;
    int rc = dom_serialize_children_html(n, &s, &slen);
    if (rc != 0 || !s)
    {
#ifdef PLUTO_THROW_LOG
        logger_log("[dom] innerHTML serialize rc=%d tag=%s kids=%d", rc,
                   (n->tag) ? n->tag : "?", n->childCount);
#endif
        xsResult = xsString("");
        return;
    }
    xsResult = xsString(s);
    JFree(s);
}

/* getRootNode — react's fu() prefers it ("function"==typeof e.getRootNode)
 * and checks the returned node for nodeType 9/11 (document/fragment).
 * Every engine node lives in the page tree, so this is the owner document
 * (same observable as ownerDocument; detached-node semantics unsupported). */
static void xs_el_get_rootNode(xsMachine *the)
{
    (void)this_node(the);
    xsResult = xsGet(xsGlobal, xsID("document"));
}

/* attachShadow — react's route announcer (next module 62634) mounts its
 * visually-hidden aria-live host as
 *     e.attachShadow({mode:"open"}).appendChild(div)
 * then reads `e?.shadowRoot?.childNodes[0]` on later route-change reels.
 * Missing this made the WHOLE commit throw ("call: not a function" kind=0
 * undefined callee from iE/commitHookEffectListMount right after the
 * [crt] next-route-announcer + [crt] div rows), which collapsed hydration
 * into the fallback innerHTML restore with zero fibers attached — no
 * delegated handlers survived, so form submits still navigated.
 * Implementation: the open shadow root IS a real element node, held OUT
 * of the render tree (parent NULL → invisible to layout, tag scans and
 * serializer output) but backed by full Element machinery so
 * appendChild/childNodes/setHostData work on its content. `mode` is
 * ignored (an open-only platform in practice). */
static void xs_el_attachShadow(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *host = this_node(the);
    if (!host || host->kind != DOM_ELEMENT)
    {
        xsTypeError("attachShadow: element expected");
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[shadow] attach host=%s(%p)", (host->tag) ? host->tag : "?",
               (void *)host);
#endif
    DomNode *root = host->shadowRoot;
    if (!root)
    {
        root = dom_create_element(b->dom, "#shadow-root");
        if (!root)
        {
            xsTypeError("attachShadow failed");
        }
        root->shadowHost = host;
        host->shadowRoot = root;
    }
    xsResult = xs_push_element(b, root);
}

static void xs_el_get_parentNode(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the), n ? n->parent : NULL);
}

static void xs_el_get_childElementCount(xsMachine *the)
{
    DomNode *n = this_node(the);
    int c = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                c++;
            }
        }
    }
    xsResult = xsInteger(c);
}

static void xs_el_get_children(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    xsResult = xsNewArray(0);
    int k = 0;
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            if (n->children[i]->kind == DOM_ELEMENT)
            {
                xsSlot el = xs_push_element(b, n->children[i]);
                xsSetAt(xsResult, xsInteger(k++), el);
            }
        }
    }
}

static void xs_el_get_ownerDocument(xsMachine *the)
{
    xsResult = xsGet(xsGlobal, xsID("document"));
}

/* isConnected — next's announcer cleanup gates removeChild on
 * `e?.isConnected` (missing it just leaked the announcer host, but the
 * getter is cheap and observable-correct): TRUE iff the parent chain
 * reaches the document root; the shadow root (parent NULL, NOT the doc
 * root) and detached fragments report FALSE, matching real semantics
 * for the host element the announcer queries. */
/* Forms-Phase 1 shared parent-walk: the isConnected rule factored out so
 * the focus()/blur() roads share one connectedness rule (a browser only
 * focuses connected elements). Defined later with the dispatch machinery;
 * declared here so the roads above can call it. */
static int xs_dispatch_focus_event(JsBridge *b, DomNode *node,
                                   const char *type);

static int node_connected_to_dom(JsBridge *b, DomNode *n)
{
    DomNode *root = (b->dom) ? b->dom->root : NULL;
    for (DomNode *p = n; p; p = p->parent)
    {
        if (p == root)
        {
            return 1;
        }
    }
    return 0;
}

static void xs_el_get_isConnected(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    xsResult = xsBoolean(n ? node_connected_to_dom(b, n) : 0);
}

/* Forms-Phase 1: node → LIVE DocBlock reverse lookup. DocBlock stamps
 * srcNode on each rendered control, so scanning doc->blocks[] by pointer
 * identity is the general road from a JS element wrapper to its render
 * state (block->checked moves on tap; the markup `checked` attribute
 * never does). First match wins — one block per node in the standing
 * build; NULL when the node has no block (not a rendered control). */
static DocBlock *jsb_block_of_node(JsBridge *b, DomNode *n)
{
    if (!n || !b->doc)
    {
        return NULL;
    }
    for (int i = 0; i < b->doc->blockCount; i++)
    {
        DocBlock *blk = b->doc->blocks[i];
        if (blk && blk->srcNode == (void *)n)
        {
            return blk;
        }
    }
    return NULL;
}

/* Forms-Phase 1: `checked` READ on the toggles (checkbox/radio).
 * Browser truth: .checked reports the LIVE control state — a tapped
 * checkbox flips without its markup attribute moving. The live state is
 * DocBlock.checked (the same field main.c's formclick drives and the
 * reset/defaultChecked machinery compares), so the read prefers the
 * block and falls back to the markup attribute only when the node has
 * no block yet (pre-walk window). Non-toggle nodes read false —
 * browsers report false there too (only the property on controls
 * differs from undefined; the battery's `in` checks use toggles).
 * Getter-only in Phase 1: writes + radio-group exclusivity + change
 * delivery land in Phase 2, so a stray set still hits XS's
 * getter-only/no-op setter path exactly like Phase 0's baseline. */
static void xs_el_get_checked(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    int checked = 0;
    if (n && n->kind == DOM_ELEMENT)
    {
        const char *tag = n->tag;
        const char *tv = dom_get_attr(n, "type");
        if (tag && strcmp(tag, "input") == 0 && tv &&
            (strcmp(tv, "checkbox") == 0 || strcmp(tv, "radio") == 0))
        {
            DocBlock *blk = jsb_block_of_node(b, n);
            checked = blk ? blk->checked
                          : (dom_get_attr(n, "checked") != NULL);
            logger_log("[cbk] get tag=%s tv=%s blk=%d checked=%d",
                       tag, tv ? tv : "?", blk ? 1 : 0, checked);
        }
    }
    xsResult = xsBoolean(checked ? 1 : 0);
}

/* Forms-Phase 2: `checked` WRITE on the toggles — browser semantics with
 * radio-group exclusivity. The live state is DocBlock.checked (the same
 * store the tap lane drives, the renderer draws live each frame, and the
 * block-aware submit serializer reads), so the write flips the block —
 * no attribute mirror (the markup `checked` attribute stays the WALK-TIME
 * default; form.reset later restores from defaultChecked exactly like a
 * browser reverts dirty checkedness). Group exclusivity mirrors the tap
 * lane's scope: a doc-wide same-name radio group — setting a radio true
 * first unchecks every OTHER same-name radio; setting false just
 * unchecks (a browser allows unchecking via the property too). Checked
 * WRITES fire no events (no dirty-checkedness change event without an
 * activation — the same programmer-truth as a browser's .checked =).
 * Visual + layout need no re-walk: the renderer reads the block live and
 * the layout item ALIASES the block. */
/* Forms-Phase 2: pre-walk `checked` attr mirror. Page scripts run BEFORE
 * the element walker builds the blocks (js_doc_attach executes at dom
 * build; blocks appear at walk end) — a write in that window must land
 * on the MARKUP ATTRIBUTE so the walk carries it as the initial checked
 * state. Radio exclusivity rides the same doc-wide same-name group:
 * other same-name radios lose their `checked` attrs (the walker owns
 * the post-walk truth). Depth-capped in step with the parser's own
 * shape. */
static void jsb_attr_uncheck_radios(DomNode *n, const char *name,
                                    DomNode *skip, int depth)
{
    if (!n || depth > 16)
    {
        return;
    }
    if (n->kind == DOM_ELEMENT && n->tag && strcmp(n->tag, "input") == 0 &&
        n != skip)
    {
        const char *tv = dom_get_attr(n, "type");
        if (tv && strcmp(tv, "radio") == 0)
        {
            const char *nm = dom_get_attr(n, "name");
            if (nm && name && strcmp(nm, name) == 0)
            {
                dom_remove_attr(n, "checked");
            }
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        jsb_attr_uncheck_radios(n->children[i], name, skip, depth + 1);
    }
}

static void xs_el_set_checked(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        logger_log("[cbk] set: no element node");
        return;
    }
    const char *tag = n->tag;
    const char *tv = dom_get_attr(n, "type");
    if (!(tag && strcmp(tag, "input") == 0 && tv &&
          (strcmp(tv, "checkbox") == 0 || strcmp(tv, "radio") == 0)))
    {
        logger_log("[cbk] set: not a toggle tag=%s", tag ? tag : "?");
        return; /* non-toggle nodes: the property stays inert */
    }
    DocBlock *blk = jsb_block_of_node(b, n);
    int want = xsToBoolean(xsArg(0));
    if (!blk)
    {
        /* Pre-walk window (scripts run before the walker builds the
         * blocks): the write lands on the markup attribute — the walk's
         * checked/defaultChecked carrier — so the state survives into
         * the rendered page. Radio exclusivity removes the OTHER
         * same-name radios' `checked` attrs. */
        const char *nm = dom_get_attr(n, "name");
        if (want)
        {
            /* exclusivity only matters for radios (tv is the radio/checkbox
             * discriminator already validated above) */
            if (strcmp(tv, "radio") == 0 && nm)
            {
                jsb_attr_uncheck_radios(b->dom->root, nm, n, 0);
            }
            dom_set_attr(b->dom, n, "checked", "");
        }
        else
        {
            dom_remove_attr(n, "checked");
        }
        logger_log("[cbk] pre-walk mirror tag=%s tv=%s want=%d",
                   tag, tv ? tv : "?", want);
        return; /* live block picks the state up at walk end */
    }
    if (want)
    {
        if (blk->radio)
        {
            /* group exclusivity — same rule as the tap lane: every OTHER
             * same-name radio in the document unchecks first */
            for (int i = 0; i < b->doc->blockCount; i++)
            {
                DocBlock *sib = b->doc->blocks[i];
                if (sib && sib != blk && sib->radio && sib->name &&
                    blk->name && strcmp(sib->name, blk->name) == 0)
                {
                    sib->checked = 0;
                }
            }
        }
        blk->checked = 1;
    }
    else
    {
        blk->checked = 0;
    }
    logger_log("[cbk] set tag=%s want=%d blk.node=%d -> checked=%d blocks=%d",
               tag, want, (blk->srcNode == (void *)n) ? 1 : 0, blk->checked,
               b->doc ? b->doc->blockCount : -1);
}

/* Forms-Phase 1: `disabled` reflection — browser-true to the REFLECTED
 * property: .disabled reads the element's OWN disabled-attribute
 * presence (a disabled <fieldset> does NOT flip a child's .disabled —
 * that inheritance affects "actually disabled", a different concept).
 * Write-through: set/remove the attribute via the same dom store every
 * attribute road uses, budget-charged like the other setters, with the
 * R58 dirty + mutation notes so styles (:disabled) and the layout
 * mirror follow the change like any setAttribute. */
static void xs_el_get_disabled(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *v = n ? dom_get_attr(n, "disabled") : NULL;
    xsResult = xsBoolean(v ? 1 : 0);
}

static void xs_el_set_disabled(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        int on = xsToBoolean(xsArg(0));
        int rc = on ? dom_set_attr(b->dom, n, "disabled", "")
                    : dom_remove_attr(n, "disabled");
        if (rc != 0)
        {
            xsTypeError("disabled assignment failed");
        }
        b->domDirty = 1; /* R58: change of record → frame-loop re-walk */
        jsbridge_note_dom_mutation(b, n);
    }
}

/* `readOnly` reflection — the same shape as disabled against the
 * `readonly` attribute (markup is lowercase; the JS property is the
 * camelCase member per HTML IDL). */
static void xs_el_get_readOnly(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *v = n ? dom_get_attr(n, "readonly") : NULL;
    xsResult = xsBoolean(v ? 1 : 0);
}

static void xs_el_set_readOnly(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        int on = xsToBoolean(xsArg(0));
        int rc = on ? dom_set_attr(b->dom, n, "readonly", "")
                    : dom_remove_attr(n, "readonly");
        if (rc != 0)
        {
            xsTypeError("readOnly assignment failed");
        }
        b->domDirty = 1; /* R58: change of record → frame-loop re-walk */
        jsbridge_note_dom_mutation(b, n);
    }
}

/* Forms-Phase 1: the el.focus() road. Browser truth (HTML focus steps):
 * focusing the already-focused element is a no-op (no second focus
 * event), a disconnected element cannot take focus, and the latch lands
 * BEFORE the event dispatches so a handler reads consistent state.
 * This is state + event only — the engine's keyboard lane stays native
 * (programmatic focus does not open the keyboard; a browser only opens
 * its soft keyboard per platform policy, and the device's tap path owns
 * that here, untouched in this phase). The latch is a plain node
 * POINTER compare everywhere — never dereferenced — so a re-walk that
 * frees the node degrades to "already focused = no-op" at worst, not a
 * crash. */
static void xs_el_focus(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        return;
    }
    if (!node_connected_to_dom(b, n) || b->focusNode == n)
    {
        return;
    }
    b->focusNode = n;
    (void)xs_dispatch_focus_event(b, n, "focus");
}

/* el.blur(): clearing when not focused is a no-op (browser truth: blur
 * fires only on an element that actually loses focus). */
static void xs_el_blur(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || b->focusNode != n)
    {
        return;
    }
    b->focusNode = NULL;
    (void)xs_dispatch_focus_event(b, n, "blur");
}

/* Forms-Phase 3: the constraint-validation surface on the element
 * prototype. validity = a FRESH snapshot object per access (the same
 * reader culture as classList); validationMessage = the shared C layer's
 * first-failure message; willValidate = the candidate rule;
 * checkValidity()/reportValidity() evaluate the rules and fire the
 * non-bubbling `invalid` road on the offending element. reportValidity
 * has no OS balloon/report UI on this device — "reporting" IS the
 * invalid delivery (policy documented in the phase recap). Both public
 * methods return the boolean verdict unchanged. */
static void xs_push_validity(xsMachine *the, JsBridge *b, DomNode *n)
{
    JsbValidity v;
    memset(&v, 0, sizeof(v));
    if (n && n->kind == DOM_ELEMENT)
    {
        jsbridge_form_validity_eval(b, n, &v);
    }
    xsResult = xsNewObject();
    xsDefine(xsResult, xsID("valueMissing"),
             xsBoolean(v.flags & JSBV_VALUEMISSING ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("typeMismatch"),
             xsBoolean(v.flags & JSBV_TYPEMISMATCH ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("patternMismatch"),
             xsBoolean(v.flags & JSBV_PATTERNMISMATCH ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("tooLong"),
             xsBoolean(v.flags & JSBV_TOOLONG ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("tooShort"),
             xsBoolean(v.flags & JSBV_TOOSHORT ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("rangeUnderflow"),
             xsBoolean(v.flags & JSBV_RANGEUNDERFLOW ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("rangeOverflow"),
             xsBoolean(v.flags & JSBV_RANGEOVERFLOW ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("stepMismatch"), xsFalse, xsDefault);
    xsDefine(xsResult, xsID("badInput"),
             xsBoolean(v.flags & JSBV_BADINPUT ? 1 : 0), xsDefault);
    xsDefine(xsResult, xsID("customError"), xsFalse, xsDefault);
    xsDefine(xsResult, xsID("valid"), xsBoolean(v.flags ? 0 : 1), xsDefault);
}

static void xs_el_get_validity(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    xs_push_validity(the, b, this_node(the));
}

static void xs_el_get_validationMessage(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char msg[96];
    msg[0] = '\0';
    if (n && n->kind == DOM_ELEMENT)
    {
        JsbValidity v;
        memset(&v, 0, sizeof(v));
        jsbridge_form_validity_eval(b, n, &v);
        snprintf(msg, sizeof(msg), "%s", v.message);
    }
    xsResult = xsString(msg);
}

static void xs_el_get_willValidate(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    int will = (n && n->kind == DOM_ELEMENT)
                   ? jsbridge_form_will_validate(b, n)
                   : 0;
    xsResult = xsBoolean(will);
}

static void xs_el_checkValidity(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    int ok = 1;
    if (n && n->kind == DOM_ELEMENT)
    {
        JsbValidity v;
        memset(&v, 0, sizeof(v));
        jsbridge_form_validity_eval(b, n, &v);
        ok = (v.flags == 0);
        if (!ok)
        {
            (void)xs_dispatch_focus_event(b, n, "invalid");
        }
    }
    xsResult = xsBoolean(ok);
}

static void xs_el_reportValidity(xsMachine *the)
{
    /* Same evaluation + delivery as checkValidity: this device has no OS
     * report surface for the browser's interactive-check dialog, so
     * "reporting" is the invalid event alone. */
    xs_el_checkValidity(the);
}

/* ── Forms-Phase 4: the HTMLFormElement API + HTMLElement.click() ───────── *
 * The methods live on the shared element prototype (Phase 0's surface
 * shape); the browser bounds them to form elements, so a non-form
 * receiver stays inert — the standing no-op for unsupported receivers.
 * The actual chains run through the main.c host hooks: reset restores
 * walk-time defaults with no events; submit() bypasses the gate AND the
 * submit event; requestSubmit(submitter?) runs the gate (formNovalidate
 * honored) then the cancelable submit event then the wire with the
 * submitter's formAction/formMethod overrides. Frozen engines dissolve
 * the event delivery (standing boundary) and their NONE falls through
 * to the wire, matching a page with no listeners. */
static DomNode *xs_form_receiver(DomNode *n)
{
    return (n && n->kind == DOM_ELEMENT && n->tag &&
            !strcmp(n->tag, "form"))
               ? n
               : NULL;
}

/* Nearest enclosing <form> over the parent chain (bridges own the DOM
 * tree pointer; the wire/gate keys off node identity). */
static DomNode *xs_form_ancestor(DomNode *n)
{
    for (const DomNode *p = n ? n->parent : NULL; p; p = p->parent)
    {
        if (p->kind == DOM_ELEMENT && p->tag && !strcmp(p->tag, "form"))
        {
            return (DomNode *)p;
        }
    }
    return NULL;
}

/* The activation classes for el.click(): a submit button (button
 * defaulting to submit, or an input of submit/image type) or a reset
 * control. A click on either runs the form's activation unless a
 * handler prevented the click itself. */
static int xs_node_is_form_button(DomNode *n)
{
    if (!n || n->kind != DOM_ELEMENT || !n->tag)
    {
        return 0;
    }
    if (!strcmp(n->tag, "button"))
    {
        const char *t = dom_get_attr(n, "type");
        return (!t || !t[0] || !strcmp(t, "submit") || !strcmp(t, "reset"));
    }
    if (!strcmp(n->tag, "input"))
    {
        const char *t = dom_get_attr(n, "type");
        return t && (!strcmp(t, "submit") || !strcmp(t, "reset") ||
                     !strcmp(t, "image"));
    }
    return 0;
}

static void xs_form_get_elements(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!xs_form_receiver(n))
    {
        xsResult = xsUndefined;
        return;
    }
    void *buf[64]; /* scratch — 64 controls is far past any real page */
    int cnt = jsbridge_form_controls_collect(n, buf, 64);
    xsResult = xsNewArray(cnt);
    for (int i = 0; i < cnt; i++)
    {
        xsSlot el = xs_push_element(b, (DomNode *)buf[i]);
        xsSetAt(xsResult, xsInteger(i), el);
    }
}

static void xs_form_reset(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!xs_form_receiver(n))
    {
        /* Non-form receiver: undefined (the call itself stays silent —
         * the membership surface is the standing prototype shape). */
        return;
    }
    (void)jsbridge_form_host_reset(b, n); /* no events; the walk-time
                                          boolean returns to the wire */
    xsResult = xsUndefined;
}

static void xs_form_submit(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!xs_form_receiver(n) || !node_connected_to_dom(b, n))
    {
        return;
    }
    /* form.submit(): no gate, no submit event — straight to the wire. */
    (void)jsbridge_form_host_wire(b, n, NULL, 0, 0);
    xsResult = xsUndefined;
}

static void xs_form_requestSubmit(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!xs_form_receiver(n) || !node_connected_to_dom(b, n))
    {
        return;
    }
    /* requestSubmit([submitter]): browser truth — the argument, when
     * present, must be a form descendant (the spec throws
     * NotFoundError otherwise; contained as a TypeError here). A submit
     * button is the natural submitter; any descendant is accepted. */
    DomNode *submitter = NULL;
    if (xsTypeOf(xsArg(0)) != xsUndefinedType &&
        xsTypeOf(xsArg(0)) != xsNullType)
    {
        submitter = arg_node(the, 0);
        if (!submitter || xs_form_ancestor(submitter) != n)
        {
            xsTypeError("requestSubmit: submitter is not this form's "
                        "descendant");
        }
    }
    (void)jsbridge_form_host_wire(b, n, submitter, 1, 1);
    xsResult = xsUndefined;
}

/* Forms-Phase 6: the form's enumerated IDL mirrors (get + set) on the
 * shared element prototype, tag-gated like every Phase-4 road. Getting
 * returns the canonical keyword through the shared reflection helper
 * (missing/invalid → the GET / urlencoded defaults); setting writes the
 * lowercase string verbatim — the browser's own reflect shape (the
 * DEFAULT only applies on read). Non-form receivers read undefined and
 * absorb the write; non-elements too. */
static void xs_form_get_method(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsUndefined;
    if (!n || !xs_form_receiver(n))
    {
        return;
    }
    xsResult = xsString(jsbridge_form_method_idl(dom_get_attr(n, "method")));
}

static void xs_form_set_method(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || !xs_form_receiver(n))
    {
        return;
    }
    char *v = to_cstring(the, xsArg(0));
    if (!v)
    {
        xsTypeError("out of memory");
    }
    int rc = dom_set_attr(b->dom, n, "method", v);
    JFree(v);
    if (rc != 0)
    {
        xsTypeError("method assignment failed");
    }
    jsbridge_note_dom_mutation(b, n);
}

static void xs_form_get_enctype(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsUndefined;
    if (!n || !xs_form_receiver(n))
    {
        return;
    }
    xsResult =
        xsString(jsbridge_form_enctype_idl(dom_get_attr(n, "enctype")));
}

static void xs_form_set_enctype(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || !xs_form_receiver(n))
    {
        return;
    }
    char *v = to_cstring(the, xsArg(0));
    if (!v)
    {
        xsTypeError("out of memory");
    }
    int rc = dom_set_attr(b->dom, n, "enctype", v);
    JFree(v);
    if (rc != 0)
    {
        xsTypeError("enctype assignment failed");
    }
    jsbridge_note_dom_mutation(b, n);
}

/* HTMLElement.click(): a real dispatched click (kind-0 bubbling). On a
 * form-associated submit/reset button the browser's ACTIVATION follows
 * the un-prevented click — the same chain a tap rides (the submit
 * branch: gate honoring formNovalidate → cancelable submit → wire; the
 * reset branch: cancelable reset → restore). Everything else is the
 * pure dispatch (browsing-context links activate Phase 6). */
static void xs_el_click(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT || !node_connected_to_dom(b, n))
    {
        return;
    }
    int rc = xs_dispatch_element_event(b, n, "click");
    if (rc == JSB_EVENT_SUPPRESSED)
    {
        jsbridge_note_dom_mutation(b, n); /* the handler owns the page */
        return;
    }
    /* Not prevented: run the activation when the target is a submit or
     * reset button inside a form (browser truth: the click's default
     * action). Everything else: the dispatch alone. */
    DomNode *form = xs_form_ancestor(n);
    if (form && xs_node_is_form_button(n))
    {
        (void)jsbridge_form_host_wire(b, form, n, 1, 1);
    }
}

/* Element.attributes — a live snapshot Array of {name,value} objects.
 * Browser shape is a NamedNodeMap (iterable + length + [n]); React DOM's
 * AppHTML wrapper walks `document.documentElement.attributes` as
 * `n<e.attributes.length` + indexed {name,value} pairs, and clones
 * attributes on client re-render mounts, so the Array covers every
 * documented use. Value "" = boolean attribute present with no value
 * (dom_get_attr maps the tokenizer marker). Non-elements: undefined
 * (only Element owns the collection in the spec). */
static void xs_el_get_attributes(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    xsResult = xsUndefined;
    if (!n || n->kind != DOM_ELEMENT)
        return;
    xsResult = xsNewArray(n->attrCount > 0 ? n->attrCount : 0);
    for (int i = 0; i < n->attrCount; i++)
    {
        xsSlot entry = xsNewObject();
        const char *val = dom_get_attr(n, n->attrs[i].key);
        xsSet(entry, xsID("name"), xsString(n->attrs[i].key));
        xsSet(entry, xsID("value"), xsString(val ? val : ""));
        xsSetAt(xsResult, xsInteger(i), entry);
    }
    (void)b;
}

/* Form type — React's hydrate matcher validates inputs with
 * `"hidden" !== e.type`; browser default for <input> is "text",
 * <button> is "submit"; other elements have no .type. */
static void xs_el_get_type(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsUndefined;
    if (!n || n->kind != DOM_ELEMENT)
    {
        return;
    }
    const char *v = dom_get_attr(n, "type");
    if (v)
    {
        xsResult = xsString(v);
    }
    else if (n->tag && !strcmp(n->tag, "input"))
    {
        xsResult = xsString("text");
    }
    else if (n->tag && !strcmp(n->tag, "button"))
    {
        xsResult = xsString("submit");
    }
}

/* namespaceURI — browser-parity: xhtml for html-parsed nodes, svg for
 * svg tags. The hoisting path uses it to skip foreign elements. */
static void xs_el_get_namespaceURI(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsString("http://www.w3.org/1999/xhtml");
    if (n && n->kind == DOM_ELEMENT && n->tag &&
        (!strcmp(n->tag, "svg") || !strcmp(n->tag, "path")))
    {
        xsResult = xsString("http://www.w3.org/2000/svg");
    }
}

static void xs_el_get_nodeType(xsMachine *the)
{
    DomNode *n = this_node(the);
    /* A shadow-root node reports 11 (DOCUMENT_FRAGMENT), not 1 — it was
     * made as an element for the element vtable, but react's remount reel
     * (`e?.shadowRoot?.childNodes[0]`) and any generic node-type sniff
     * should see the browser kind. */
    if (n && n->shadowHost)
    {
        xsResult = xsInteger(11);
        return;
    }
    /* API-P6: a template's detached content fragment reports 11 too —
     * made as an element for the element vtable, but it IS the browser's
     * DocumentFragment surface (tpl.content.nodeType === 11). Detached is
     * part of the rule: a nested element tagged the same stays invisible
     * to this branch only by parent chain, and fragments are always
     * parent-NULL nodes owned by their template. */
    if (n && !n->parent && n->tag && strcmp(n->tag, "#template-content") == 0)
    {
        xsResult = xsInteger(11);
        return;
    }
    xsResult = xsInteger((n && n->kind == DOM_ELEMENT) ? 1
                         : (n && n->kind == DOM_COMMENT) ? 8 : 3);
}

static void xs_el_set_id(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
        int rc = dom_set_attr(b->dom, n, "id", v);
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("id assignment failed");
        }
    }
}

/* API-P9: className accessor pair — the class attribute read returns ""
 * never NULL (browser shape: mapped 1:1 onto 'class'), the write goes
 * through the shared class road (jsbridge_el_class_set — the same
 * snapshot/dirty-receipt pair the classList mutations ride, so rewalks
 * + the transition-lane note see it exactly like setAttribute('class')).
 * Closes the P6 parked gap: the battery's identity fixtures no longer
 * need the setAttribute detour. */
static void xs_el_get_className(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *v = (n && n->kind == DOM_ELEMENT)
                        ? dom_get_attr(n, "class")
                        : NULL;
    xsResult = xsString(v ? v : "");
}

static void xs_el_set_className(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
        int rc = jsbridge_el_class_set(b, n, v); /* router owns the budget */
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("className assignment failed");
        }
    }
}

static void xs_el_set_textContent(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT)
    {
        char *v = to_cstring(the, xsArg(0));
        if (!v)
        {
            xsTypeError("out of memory");
        }
        int rc = dom_set_text(b->dom, n, v);
        JFree(v);
        if (rc != 0)
        {
            xsTypeError("textContent assignment failed");
        }
    }
}

/* SW5: REAL markup assignment through the router (was textContent). */
static void xs_el_set_innerHTML(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    if (n)
    {
        logger_log("[elap] innerHTML set this=%p tag=%s kind=%d",
                   (void *)n, (n->tag) ? n->tag : "?", (int)n->kind);
    }
#endif
    if (n && n->kind == DOM_ELEMENT)
    {
        char *s = to_cstring(the, xsArg(0));
        if (!s)
        {
            xsTypeError("out of memory");
        }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
        logger_log("[elap] innerHTML len=%zu head=%.64s", strlen(s), s);
#endif
        /* Document-sized snapshots (Next.js error-boundary fallback
         * assigns documentElement.innerHTML back) need the NR cap, not
         * the shared 16KB one: a contact page snapshot is ~22KB. */
        int rc = jsbridge_el_set_inner_html_ex(b, n, s, strlen(s),
                                               (size_t)128 * 1024);
        JFree(s);
        if (rc == 0)
        {
            /* The assignment replaced/cleared live subtree children —
             * layout must re-anchor (stale blocks dispatch to DETACHED
             * nodes). The frame loop answers via jsbridge_take_dom_dirty
             * → page_rewalk_now(), the same escalation the throttled pumps
             * use for handler mutations. */
            b->domDirty = 1;
            jsbridge_note_dom_mutation(b, n); /* Phase 10: subtree replaced */
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
            logger_log("[elap] innerHTML set ok → domDirty=1");
#endif
        }
        if (rc != 0)
        {
            xsTypeError("innerHTML assignment failed");
        }
    }
}

/* ── element methods ────────────────────────� */

static void xs_el_getAttribute(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    const char *v = n ? dom_get_attr(n, k) : NULL;
    JFree(k);
    xsResult = v ? xsString(v) : xsNull;
}

static void xs_el_setAttribute(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    char *v = to_cstring(the, xsArg(1));
    if (!k || !v)
    {
        if (k)
            JFree(k);
        if (v)
            JFree(v);
        xsTypeError("out of memory");
    }
    BUDGET_OR_THROW(b);
    int rc = (!n) ? -1 : dom_set_attr(b->dom, n, k, v);
    JFree(k);
    JFree(v);
    if (rc != 0)
    {
        xsTypeError("setAttribute failed");
    }
    b->domDirty = 1; /* R58: change of record → frame-loop re-walk */
    if (rc == 0)
    {
        /* Phase 10: attribute writes flip computed style (class/aria/
         * value lanes ride the same store). Text-free writes only —
         * see jsbridge.h's note contract. */
        jsbridge_note_dom_mutation(b, n);
    }
}

static void xs_el_removeAttribute(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    if (n)
    {
        dom_remove_attr(n, k);
        b->domDirty = 1; /* R58: change of record → frame-loop re-walk */
        jsbridge_note_dom_mutation(b, n); /* Phase 10: removed attribute */
    }
    JFree(k);
}

/* React's hydration-reset commit path (ij()) unlinks attribute nodes:
 * `for (var l = t.attributes; l.length;) t.removeAttributeNode(l[0])`.
 * Our attributes shim hands back plain {name,value} records, so removing
 * by the record's name is the spec-equivalent observable behavior; the
 * record is returned as the removed Attr-like node. */
static void xs_el_removeAttributeNode(xsMachine *the)
{
    DomNode *n = this_node(the);
    if (!n || xsTypeOf(xsArg(0)) != xsReferenceType)
    {
        xsTypeError("removeAttributeNode: attribute node expected");
    }
    xsSlot nm = xsGet(xsArg(0), xsID("name"));
    char *k = to_cstring(the, nm);
    if (!k)
    {
        xsTypeError("out of memory");
    }
    dom_remove_attr(n, k);
    JFree(k);
    xsResult = xsArg(0);
}

static void xs_el_hasAttribute(xsMachine *the)
{
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    const char *v = n ? dom_get_attr(n, k) : NULL;
    JFree(k);
    xsResult = xsBoolean(v != NULL);
}

static void xs_el_hasChildNodes(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsBoolean(n && n->childCount > 0);
}

/* ── A2: replay-cached resource load events ─────────────────────────────
 * react-dom blocks an SSR commit on stylesheet resources: it inserts or
 * reuses a <link> (rel=preload|stylesheet) and waits for that element's
 * load OR error event before unsuspending (SUSPENSEY_STYLESHEET_TIMEOUT
 * is 60s — on this replay the gate never opened and the mount never
 * reached the event-binding stage). A browser dispatches those events
 * from its fetch pipeline; the offline replay models the page's own
 * assets as already fetched and complete, so link insertions and load
 * registrations queue ONE dispatcher on the microtask ring and it fires
 * every 'load' listener pinned on the element (exactly one event per
 * element, like the browser) plus the element's onload property hook
 * when a callable was assigned (the `instance._p = new Promise(resolve
 * => { instance.onload = resolve … })` resolution path). 'error' pings
 * are registered but never fired: a browser reports exactly one of the
 * two events per resource. */
static void xs_event_preventDefault(xsMachine *the); /* defined below */
static void xs_fire_pending_resource_loads(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    /* Cleared BEFORE firing: a ping may insert more resources, and those
     * enqueue their own dispatcher for the next microcheckpoint. */
    int count = st->resFireCount;
    st->resFireCount = 0;
    for (int i = 0; i < count; i++)
    {
        DomNode *el = st->resFire[i];
        st->resFire[i] = NULL;
        if (!el)
        {
            continue;
        }
        /* react registers pings BEFORE setInitialProperties, so re-check
         * the element's rel here instead of trusting the queue time. */
        const char *rel = dom_get_attr(el, "rel");
        if (!rel || (strcmp(rel, "preload") != 0 &&
                     strcmp(rel, "stylesheet") != 0))
        {
            continue;
        }
#ifdef PLUTO_THROW_LOG
        const char *href = dom_get_attr(el, "href");
        logger_log("[load] fire href=%.60s", href ? href : "-");
#endif
        /* A real dispatch passes an Event object: react's non-delegated
         * load dispatcher reads event.target (getEventTarget) and the
         * simple pings just ignore it. Built exactly like the click
         * event, with target set so fiber resolution can follow it. */
        xsSlot ev = xsNewHostObject(NULL);
        xsResult = ev; /* GC anchor: preventDefault fn allocates */
        xsDefine(ev, xsID("type"), xsString("load"), xsDefault);
        xsDefine(ev, xsID("preventDefault"),
                 xsNewHostFunction(xs_event_preventDefault, 0),
                 xsDefault);
        xsSlot w = xs_push_element(b, el); /* allocs: ev stays anchored */
        xsDefine(ev, xsID("target"), w, xsDefault); /* ev keeps w alive */
        for (int li = 0; li < b->listenerCount; li++)
        {
            JsListener *L = &b->listeners[li];
            if (L->target != el || L->kind != 1) /* load pings only */
            {
                continue;
            }
            xsSlot fn = st->fns[li];
            if (!fxIsCallable(the, &fn))
            {
                continue;
            }
#ifdef PLUTO_THROW_LOG
            logger_log("[load] ping li=%d", li);
#endif
            /* Browser dispatch semantics: ONE listener throwing never
             * cancels the event (the rest still get it, and the failure
             * surfaces to window.onerror, not to the dispatcher). */
            xsTry
            {
                /* listeners run with this = the event target, exactly
                 * like a browser-dispatched DOM event */
                xsCall2_noResult(fn, xsID("call"), w, ev);
            }
            xsCatch
            {
                logger_log("[load] ping li=%d threw (isolated)", li);
            }
        }
        xsSlot onload = xsGet(w, xsID("onload"));
        if (fxIsCallable(the, &onload))
        {
            xsTry
            {
                /* onLoad receives the event, like the browser. */
                xsCall2_noResult(onload, xsID("call"), w, ev);
            }
            xsCatch
            {
                logger_log("[load] onload hook threw (isolated)");
            }
        }
        xsResult = xsUndefined; /* drop the anchor before the next fire */
        /* The delivered load event is DOM-relevant work (an adopt pass
         * ran and the mount may have touched the head). Consume one
         * budget unit so the frame pump reports the round as a mutation
         * trigger — the browser equivalent schedules a style/layout
         * update on asset completion. Checked, not thrown: an exhausted
         * budget means the page is done regardless. */
        if (budget_take(b))
        {
            logger_log("[load] consumed budget (mutation signal)");
        }
    }
}

/* Queue one pending load fire for `el`. Deduped while pending: browsers
 * dispatch load exactly once per resource, and react registers the same
 * ping on both the load and error channel. */
static void xs_queue_resource_fire(JsBridge *b, DomNode *el)
{
    if (!b || !el)
    {
        return;
    }
    XsState *st = state_of(b);
    if (!st->machine)
    {
        return;
    }
    xsMachine *the = st->machine; /* value macros below need `the` */
    for (int i = 0; i < st->resFireCount; i++)
    {
        if (st->resFire[i] == el)
        {
            return;
        }
    }
    if (st->resFireCount >= XS_RESFIRE_MAX)
    {
        logger_log("[js] resource fire queue full");
        return;
    }
    st->resFire[st->resFireCount++] = el;
    if (!st->resFireFnSet)
    {
        st->resFireFn = xsNewHostFunction(xs_fire_pending_resource_loads, 0);
        xsRemember(st->resFireFn); /* pinned for the machine lifetime */
        st->resFireFnSet = 1;
    }
    if (st->mtCount >= XS_MICRO_MAX)
    {
        logger_log("[js] microtask queue full (load fire deferred)");
        st->resFireCount--;
        st->resFire[st->resFireCount] = NULL;
        return;
    }
    /* Same ring protocol as xs_queueMicrotask: copy the dispatcher value
     * into the tail slot and pin THE SLOT (never the source struct). */
    int idx = (st->mtHead + st->mtCount) % XS_MICRO_MAX;
    st->mtQ[idx] = st->resFireFn;
    xsRemember(st->mtQ[idx]);
    st->mtCount++;
}

/* Buffers a cached-asset completion when a stylesheet/preload link enters
 * the document (appendChild / insertBefore). Attributes may still be
 * landing when this runs; the dispatcher re-validates rel at fire time. */
static void xs_maybe_queue_asset_load(JsBridge *b, DomNode *c)
{
    if (!c || c->kind != DOM_ELEMENT || !c->tag || strcmp(c->tag, "link"))
    {
        return;
    }
    const char *rel = dom_get_attr(c, "rel");
    if (!rel || (strcmp(rel, "preload") != 0 &&
                 strcmp(rel, "stylesheet") != 0))
    {
        return;
    }
    xs_queue_resource_fire(b, c);
}

static void xs_el_appendChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    if (n && c && (!n->tag || (!c->tag && c->kind != DOM_ELEMENT) ||
                   strcmp(n->tag, "head") != 0))
    {
        /* Head-hoist rows flood; everything else is rare. */
        logger_log("[elap] append parent=%s child=%s kind=%d",
                   (n->tag) ? n->tag : "?",
                   (c->kind == DOM_ELEMENT && c->tag) ? c->tag : "#other",
                   c->kind);
    }
#endif
    if (!n || !c || dom_append_child(b->dom, n, c) != 0)
    {
        xsTypeError("appendChild failed");
    }
    /* R58: ANY live-tree mutation must reach the frame loop — the pump's
     * callBudget heuristic misses plain appendChild batches, so a
     * handler-triggered subtree swap (react setState rerenders) left the
     * layout on stale blocks with DETACHED srcNodes (the dead "Send
     * another" finding 2026-10-18). One flag per service call; the frame
     * loop takes it at most once per frame → at most one re-walk. */
    b->domDirty = 1;
    jsbridge_note_dom_mutation(b, n); /* Phase 10: subtree swap = style change */
    xs_maybe_queue_asset_load(b, c); /* react stylesheets enter via append */
    /* API-P6: browsers return the appended child — the falsy-return the
     * probe recorded is fixed here (chained mounts like react's
     * a.appendChild(b).appendChild(c) now behave like the browser). */
    xsResult = xs_push_element(b, c);
}

static void xs_el_removeChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    if (n && c)
    {
        logger_log("[elap] remove parent=%s child=%s kind=%d",
                   (n->tag) ? n->tag : "?",
                   (c->kind == DOM_ELEMENT && c->tag) ? c->tag : "#other",
                   c->kind);
    }
#endif
    if (!n || !c || dom_remove_child(n, c) != 0)
    {
        xsTypeError("removeChild failed");
    }
    b->domDirty = 1; /* R58: live-tree mutation → frame-loop re-walk */
    jsbridge_note_dom_mutation(b, n); /* Phase 10: subtree shape changed */
    /* API-P6: browsers return the REMOVED child. */
    xsResult = xs_push_element(b, c);
}

/* ── SW5: inline style string reader/writer (shared with the style object
 * accessors; same per-property semantics as the other engines) ────────── */
static void xs_style_read_prop(const DomNode *n, const char *prop, char *out,
                               size_t outsz)
{
    out[0] = '\0';
    if (!n || n->kind != DOM_ELEMENT)
    {
        return;
    }
    const char *st = dom_get_attr(n, "style");
    if (!st)
    {
        return;
    }
    size_t plen = strlen(prop);
    const char *p = st;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        const char *seg = p;
        while (*p && *p != ';')
        {
            p++;
        }
        const char *colon = memchr(seg, ':', (size_t)(p - seg));
        if (colon && (size_t)(colon - seg) == plen &&
            strncmp(seg, prop, plen) == 0)
        {
            const char *vs = colon + 1;
            const char *ve = p;
            while (vs < ve && (*vs == ' ' || *vs == '\t'))
            {
                vs++;
            }
            while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t'))
            {
                ve--;
            }
            size_t vn = (size_t)(ve - vs);
            if (vn >= outsz)
            {
                vn = outsz - 1;
            }
            memcpy(out, vs, vn);
            out[vn] = '\0';
            return;
        }
        if (*p)
        {
            p++;
        }
    }
}

static void xs_style_write_prop(JsBridge *b, DomNode *n, const char *prop,
                                const char *value)
{
    static char buf[512]; /* static: off the device game-task stack */
    size_t off = 0;
    size_t plen = strlen(prop);
    buf[0] = '\0';
    const char *st = dom_get_attr(n, "style");
    const char *p = st;
    while (st && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        const char *seg = p;
        while (*p && *p != ';')
        {
            p++;
        }
        const char *colon = memchr(seg, ':', (size_t)(p - seg));
        size_t segLen = (size_t)(p - seg);
        if (!colon || (size_t)(colon - seg) != plen ||
            strncmp(seg, prop, plen) != 0)
        {
            while (segLen > 0 && (seg[segLen - 1] == ' ' ||
                                  seg[segLen - 1] == '\t'))
            {
                segLen--;
            }
            if (segLen && off + segLen + 2 < sizeof(buf))
            {
                if (off)
                {
                    buf[off++] = ';';
                }
                memcpy(buf + off, seg, segLen);
                off += segLen;
                buf[off] = '\0';
            }
        }
        if (*p)
        {
            p++;
        }
    }
    if (value && value[0])
    {
        int n1 = snprintf(buf + off, sizeof(buf) - off, "%s%s: %s",
                          off ? ";" : "", prop, value);
        if (n1 < 0 || (size_t)n1 >= sizeof(buf) - off)
        {
            return;
        }
    }
    int sSet = dom_set_attr(b->dom, n, "style", buf);
    if (sSet == 0)
    {
        /* Phase 10: the style object flipped an inline property — the
         * classic transition trigger (react's isOpen .style opacities).
         * Note-only: the rewalk ladder stays as the pumps govern it. */
        jsbridge_note_dom_mutation(b, n);
    }
}

static const char *const XS_STYLE_PROPS[] = {
    "display", "visibility", "text-align", "font-weight", "font-style",
    "text-decoration", "color", "background"};

/* Style accessors: one function per property via the prototype loop, the
 * property name carried in a reserved own prop (XS host functions see only
 * the machine state — the getter scans xsThis's hidden "st.prop" marker).
 * Simpler + proven: one getter/setter PAIR per property, closed over by
 * name through separate C functions generated by the table below. */

#define XS_STYLE_GETTER(name, prop)                                        \
    static void xs_style_get_##name(xsMachine *the)                        \
    {                                                                      \
        char val[128];                                                     \
        xs_style_read_prop(this_node(the), prop, val, sizeof(val));         \
        xsResult = xsString(val);                                          \
    }
#define XS_STYLE_SETTER(name, prop)                                        \
    static void xs_style_set_##name(xsMachine *the)                        \
    {                                                                      \
        JsBridge *b = bridge_of(the);                                      \
        BUDGET_OR_THROW(b);                                                \
        DomNode *n = this_node(the);                                       \
        if (n && n->kind == DOM_ELEMENT)                                   \
        {                                                                  \
            char *v = to_cstring(the, xsArg(0));                           \
            if (!v)                                                        \
            {                                                              \
                xsTypeError("out of memory");                              \
            }                                                              \
            xs_style_write_prop(b, n, prop, v);                            \
            JFree(v);                                                      \
        }                                                                  \
    }
XS_STYLE_GETTER(display, "display")
XS_STYLE_SETTER(display, "display")
XS_STYLE_GETTER(visibility, "visibility")
XS_STYLE_SETTER(visibility, "visibility")
XS_STYLE_GETTER(textAlign, "text-align")
XS_STYLE_SETTER(textAlign, "text-align")
XS_STYLE_GETTER(fontWeight, "font-weight")
XS_STYLE_SETTER(fontWeight, "font-weight")
XS_STYLE_GETTER(fontStyle, "font-style")
XS_STYLE_SETTER(fontStyle, "font-style")
XS_STYLE_GETTER(textDecoration, "text-decoration")
XS_STYLE_SETTER(textDecoration, "text-decoration")
XS_STYLE_GETTER(color, "color")
XS_STYLE_SETTER(color, "color")
XS_STYLE_GETTER(background, "background")
XS_STYLE_SETTER(background, "background")

/* Fresh style host object for `node` (accessors over the walker vocab). */
static void xs_push_style(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b);
    xsSlot obj = xsNewHostObject(NULL);
    xsResult = obj; /* GC anchor: survives every allocation below */
    static const struct
    {
        const char *name;
        xsCallback get;
        xsCallback set;
    } accs[] = {
        {"display", xs_style_get_display, xs_style_set_display},
        {"visibility", xs_style_get_visibility, xs_style_set_visibility},
        {"textAlign", xs_style_get_textAlign, xs_style_set_textAlign},
        {"fontWeight", xs_style_get_fontWeight, xs_style_set_fontWeight},
        {"fontStyle", xs_style_get_fontStyle, xs_style_set_fontStyle},
        {"textDecoration", xs_style_get_textDecoration,
         xs_style_set_textDecoration},
        {"color", xs_style_get_color, xs_style_set_color},
        {"background", xs_style_get_background, xs_style_set_background},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        xsDefine(xsResult, xsID(accs[i].name), getter, xsIsGetter);
        /* getter is now linked on the anchored target, so the setter's
         * allocation below cannot sweep it */
        xsSlot setter = xsNewHostFunction(accs[i].set, 1);
        xsDefine(xsResult, xsID(accs[i].name), setter, xsIsSetter);
    }
    /* Host data = the element node: classList/style METHODS run with `this`
     * = the object itself, so this_node(the) resolves through it. */
    xs_set_host_data_site(the, "style", xsResult, node);
}

/* ── SW5 (O4): classList host object (fresh per access). Methods mutate
 * through the router; `length` is a live getter; `item(i)` the i-th token. */
static void xs_cl_add(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_add(b, n, tok);
    JFree(tok);
    if (rc != 0)
    {
        xsTypeError("classList.add failed");
    }
}

static void xs_cl_remove(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_remove(b, n, tok);
    JFree(tok);
    if (rc != 0)
    {
        xsTypeError("classList.remove failed");
    }
}

static void xs_cl_toggle(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    int rc = jsbridge_el_class_toggle(b, n, tok);
    JFree(tok);
    if (rc < 0)
    {
        xsTypeError("classList.toggle failed");
    }
    xsResult = xsBoolean(rc == 1);
}

static void xs_cl_contains(xsMachine *the)
{
    DomNode *n = this_node(the);
    char *tok = to_cstring(the, xsArg(0));
    if (!tok)
    {
        xsTypeError("out of memory");
    }
    xsResult = xsBoolean(jsbridge_el_class_has(n, tok));
    JFree(tok);
}

static void xs_cl_item(xsMachine *the)
{
    DomNode *n = this_node(the);
    int idx = (xsToInteger(xsArgc) > 0) ? (int)xsToNumber(xsArg(0)) : -1;
    const char *cls = (n && n->kind == DOM_ELEMENT) ? dom_get_attr(n, "class")
                                                    : NULL;
    int k = 0;
    const char *p = cls;
    while (p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }
        int n2 = 0;
        while (p[n2] && !strchr(" \t\n\r\f", p[n2]))
        {
            n2++;
        }
        if (k++ == idx)
        {
            char tok[64];
            int cpy = n2 < (int)sizeof(tok) - 1 ? n2 : (int)sizeof(tok) - 1;
            memcpy(tok, p, (size_t)cpy);
            tok[cpy] = '\0';
            xsResult = xsString(tok);
            return;
        }
        p += n2;
    }
    xsResult = xsNull;
}

static void xs_cl_length(xsMachine *the)
{
    DomNode *n = this_node(the);
    const char *cls = (n && n->kind == DOM_ELEMENT) ? dom_get_attr(n, "class")
                                                    : NULL;
    int k = 0;
    const char *p = cls;
    while (p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }
        k++;
        while (*p && !strchr(" \t\n\r\f", *p))
        {
            p++;
        }
    }
    xsResult = xsInteger(k);
}

static void xs_push_classlist(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b);
    xsSlot obj = xsNewHostObject(NULL);
    xsResult = obj; /* GC anchor: survives every allocation below */
    def_fn(the, xsResult, "add", xs_cl_add, 1);
    def_fn(the, xsResult, "remove", xs_cl_remove, 1);
    def_fn(the, xsResult, "toggle", xs_cl_toggle, 1);
    def_fn(the, xsResult, "contains", xs_cl_contains, 1);
    def_fn(the, xsResult, "item", xs_cl_item, 1);
    {
        xsSlot getter = xsNewHostFunction(xs_cl_length, 0);
        xsDefine(xsResult, xsID("length"), getter, xsIsGetter);
    }
    xs_set_host_data_site(the, "classList", xsResult, node); /* methods read `this` through it */
}

/* ── SW5: element method additions ───────────────────────────────────── */
static void xs_el_insertBefore(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    /* arg_node is fxGetHostDataIf-based: undefined/null/any primitive
     * yields NULL, which dom_insert_before treats as append-at-end. */
    DomNode *ref = (xsToInteger(xsArgc) > 1) ? arg_node(the, 1) : NULL;
    if (!n || !c || dom_insert_before(b->dom, n, c, ref) != 0)
    {
        xsTypeError("insertBefore failed");
    }
    b->domDirty = 1; /* R58: live-tree mutation → frame-loop re-walk */
    jsbridge_note_dom_mutation(b, n); /* Phase 10: subtree moved */
    xs_maybe_queue_asset_load(b, c); /* react stylesheet hoisting path */
    /* API-P6: browsers return the inserted child. */
    xsResult = xs_push_element(b, c);
}

/* querySelector emit: XS pushes wrappers into the array slot carried in
 * the user data (array + running index). */
typedef struct
{
    JsBridge *b;
    xsMachine *m; /* the xsSetAt macro below needs a local named `the` */
    xsSlot arr;
    int k;
} XsQsEmit;

static int qs_emit_xs(void *elp, void *ud)
{
    XsQsEmit *e = (XsQsEmit *)ud;
    xsSlot v = xs_push_element(e->b, (DomNode *)elp);
    xsMachine *the = e->m;
    xsSetAt(e->arr, xsInteger(e->k++), v);
    return 0;
}

static void xs_el_querySelectorAll(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    xsResult = xsNewArray(0);
    XsQsEmit e = {b, the, xsResult, 0};
    jsbridge_el_query_selector_all(b, n, sel, scratch, sizeof(scratch),
                                   qs_emit_xs, &e);
    JFree(sel);
}

static void xs_el_querySelector(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2: react's stylesheet hydration selector reads */
    logger_log("[qs] %.80s recv=%s", sel,
               n && n->tag ? n->tag : "#other");
#endif
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, n, sel, scratch, sizeof(scratch));
    JFree(sel);
    xsResult = xs_push_element(b, hit);
}

/* ── API-P6: Element.closest — walks UP from the element itself with the
 * SAME compound matcher as querySelector (the two can never disagree
 * about what matches); returns null when nothing matches, per browsers. */
static void xs_el_closest(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_closest(b, n, sel, scratch,
                                                  sizeof(scratch));
    logger_log("[closest] '%.60s' recv=%s -> %s", sel,
               (n && n->tag) ? n->tag : "#other",
               (hit && hit->tag) ? hit->tag : "null");
    JFree(sel);
    xsResult = xs_push_element(b, hit);
}

/* SW5 sim finding: the document object is a plain host object (host data
 * NULL), so the ELEMENT querySelector fns read this_node→NULL and return
 * null/empty at document level. Dedicated document-level fns scope to the
 * root — same pattern as the other bridges. */
static void xs_document_querySelector(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, b->dom ? b->dom->root : NULL, sel, scratch, sizeof(scratch));
    JFree(sel);
    xsResult = xs_push_element(b, hit);
}

static void xs_document_querySelectorAll(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *sel = to_cstring(the, xsArg(0));
    if (!sel)
    {
        xsTypeError("out of memory");
    }
    char scratch[JSBRIDGE_QS_SCRATCH];
    xsResult = xsNewArray(0);
    XsQsEmit e = {b, the, xsResult, 0};
    jsbridge_el_query_selector_all(b, b->dom ? b->dom->root : NULL, sel,
                                   scratch, sizeof(scratch), qs_emit_xs, &e);
    JFree(sel);
}

/* ── SW5 accessors: navigation + classList/style object getters ──────── */
static void xs_el_get_firstElementChild(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the),
                               n ? dom_first_element_child(n) : NULL);
}

static void xs_el_get_nextElementSibling(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the),
                               n ? dom_next_element_sibling(n) : NULL);
}

/* ── Node-level (incl. text) traversal — hydration parity ──
 * React's hydrate matcher starts at containerInfo.body.firstChild and
 * steps e.nextSibling across TEXT nodes (c9() in react-dom), so the
 * element-only getters above are not enough; the parsed tree keeps text
 * nodes as children. Null (not undefined) ends every walk per DOM spec. */
static DomNode *xs_el_sibling(DomNode *n, int delta)
{
    if (!n || !n->parent)
        return NULL;
    DomNode *p = n->parent;
    for (int i = 0; i < p->childCount; i++)
    {
        if (p->children[i] == n)
        {
            int j = i + delta;
            return (j >= 0 && j < p->childCount) ? p->children[j] : NULL;
        }
    }
    return NULL;
}

#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2 harness-only hydrate-walk trace */
static void walk_head(char *out, int cap, DomNode *c)
{
    if (!c)
    {
        snprintf(out, (size_t)cap, "null");
        return;
    }
    if (c->kind == DOM_ELEMENT)
        snprintf(out, (size_t)cap, "<%s>", c->tag ? c->tag : "?");
    else if (c->kind == DOM_COMMENT)
        snprintf(out, (size_t)cap, "c[%s]", c->text ? c->text : "");
    else
        snprintf(out, (size_t)cap, "t[%s]", c->text ? c->text : "");
    /* Keep the row one-line and byte-safe (fold anything > 0x7e). */
    for (char *p = out; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
            *p = '?';
}
#endif

static void xs_el_get_firstChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    DomNode *c = (n && n->childCount) ? n->children[0] : NULL;
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2 harness-only hydrate-walk trace */
    static int s_fcLog = 0;
    if (s_fcLog++ < 1500)
    {
        char head[40];
        walk_head(head, sizeof(head), c);
        logger_log("[walk] firstChild recv=%s kind=%d fx=%d out=%s",
                   n && n->kind == DOM_ELEMENT && n->tag ? n->tag : "#other",
                   n ? n->kind : -1,
                   c ? (c->kind == DOM_ELEMENT
                            ? (c->tag ? (int)c->tag[0] : 63)
                            : (c->kind == DOM_COMMENT ? 99 : 116))
                     : 0,
                   head);
    }
#endif
    xsResult = xs_push_element(b, c);
}

static void xs_el_get_lastChild(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the),
                               (n && n->childCount) ? n->children[n->childCount - 1] : NULL);
}

static void xs_el_get_nextSibling(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    DomNode *s = n ? xs_el_sibling(n, 1) : NULL;
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2 harness-only hydrate-walk trace */
    static int s_nsLog = 0;
    if (s_nsLog++ < 1500)
    {
        char head[40];
        walk_head(head, sizeof(head), s);
        logger_log("[walk] nextSibling recv=%s kind=%d from=%d out=%s",
                   n && n->kind == DOM_ELEMENT && n->tag ? n->tag : "#other",
                   n ? n->kind : -1,
                   s ? (s->kind == DOM_ELEMENT
                            ? (s->tag ? (int)s->tag[0] : 63)
                            : (s->kind == DOM_COMMENT ? 99 : 116))
                     : 0,
                   head);
    }
#endif
    xsResult = xs_push_element(b, s);
}

static void xs_el_get_previousSibling(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xs_push_element(bridge_of(the), n ? xs_el_sibling(n, -1) : NULL);
}

static void xs_el_get_childNodes(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    xsResult = xsNewArray(0);
    if (n)
    {
        for (int i = 0; i < n->childCount; i++)
        {
            xsSetAt(xsResult, xsInteger(i), xs_push_element(b, n->children[i]));
        }
    }
}

/* CharacterData.data — Text(.data alias of textContent) and Comment.
 * c9()/the hydrate boundary walk reads e.data on marker comments; the
 * tokenizer drops comments so this is only reachable for text nodes.
 * Elements have no .data in the DOM (honest undefined). */
static void xs_el_get_data(xsMachine *the)
{
    DomNode *n = this_node(the);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2 harness-only hydrate-scan trace */
    static int s_dgLog = 0;
    if (s_dgLog++ < 1500)
    {
        char head[40];
        walk_head(head, sizeof(head), n);
        logger_log("[tget] data self=%s", head);
    }
#endif
    if (n && n->kind != DOM_ELEMENT)
    {
        xsResult = xsString(n->text ? n->text : "");
    }
    else
    {
        xsResult = xsUndefined;
    }
}

/* Element.dataset — data-* attributes camelCased on a plain object.
 * React's stylesheet-hoisting path reads `o.dataset.precedence`. */
/* ── API-P6: template.content — the element's detached content fragment
 * (nodeType 11). Parsed templates hold theirs from the builder; created
 * templates materialize an EMPTY fragment on first access — the
 * browser's always-a-DocumentFragment content getter. Non-template
 * elements read undefined, like browsers. */
static void xs_el_get_content(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        strcmp(n->tag, "template") != 0)
    {
        xsResult = xsUndefined;
        return;
    }
    DomNode *frag = dom_template_content(b->dom, n);
    if (!frag)
    {
        xsTypeError("template content unavailable");
    }
    xsResult = xs_push_element(b, frag);
    logger_log("[tpl] content -> node %d (children=%d)", frag->nodeId,
               frag->childCount);
}

static void xs_el_get_dataset(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsSlot obj = xsNewObject();
    xsResult = obj;
    if (!n)
    {
        return;
    }
    for (int i = 0; i < n->attrCount; i++)
    {
        const char *k = n->attrs[i].key;
        if (strncmp(k, "data-", 5) != 0)
        {
            continue;
        }
        char camel[64];
        int ci = 0;
        for (const char *p = k + 5; *p && ci + 1 < (int)sizeof(camel); p++)
        {
            if (*p == '-' && p[1])
            {
                p++;
                camel[ci++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
            }
            else
            {
                camel[ci++] = *p;
            }
        }
        camel[ci] = '\0';
        const char *v = dom_get_attr(n, k);
        xsSet(obj, xsID(camel), xsString(v ? v : ""));
    }
}

/* CharacterData.nodeValue — React's single-text-child fast paths
 * (t_(e,t): `n.nodeValue = t` and the selection walker's
 * `p.nodeValue.length`) read/write TEXT nodes through this slot. The
 * old arena string stays owned by the document arena (wholesale free on
 * dom_free_result), so re-pointing without an explicit release is safe
 * and bounded. Elements keep the browser's null. */
static void xs_el_get_nodeValue(xsMachine *the)
{
    DomNode *n = this_node(the);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2 harness-only hydrate-claim trace */
    static int s_nvLog = 0;
    if (s_nvLog++ < 1500)
    {
        char head[40];
        walk_head(head, sizeof(head), n);
        logger_log("[tget] nodeValue self=%s", head);
    }
#endif
    if (n && n->kind != DOM_ELEMENT)
    {
        xsResult = xsString(n->text ? n->text : "");
    }
    else
    {
        xsResult = xsNull;
    }
}

static void xs_el_set_nodeValue(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *n = this_node(the);
    if (!n)
    {
        return;
    }
    if (n->kind == DOM_ELEMENT)
    {
        xs_el_set_textContent(the);
        return;
    }
    char *v = to_cstring(the, xsArg(0));
    if (!v)
    {
        xsTypeError("out of memory");
    }
    char *dup = dom_arena_dup(b->dom, v, (int)strlen(v));
    JFree(v);
    if (!dup)
    {
        xsTypeError("nodeValue assignment failed");
    }
    n->text = dup;
}

static void xs_el_get_classList(xsMachine *the)
{
    xs_push_classlist(bridge_of(the), this_node(the));
}

static void xs_el_get_style(xsMachine *the)
{
    xs_push_style(bridge_of(the), this_node(the));
}

/* Strict function-identity test for the listener dedupe: two slots alias
 * the SAME script/host function iff kind matches and the kind-defining
 * value is the same record. For code/callback instances `address` is the
 * function body pointer — two distinct closures over one shared body also
 * differ via their `closures`/scope snapshots (each mount builds its own).
 * Name-based dedupe (e.g. on ID) would be WRONG here: ids are property
 * names, not value identity. */
static int xs_same_listener_fn(xsMachine *the, const xsSlot *a, const xsSlot *b)
{
    (void)the;
    if (a->kind != b->kind)
    {
        return 0;
    }
    switch (a->kind)
    {
    case XS_CODE_KIND:
    case XS_CODE_X_KIND:
        return (a->value.code.address == b->value.code.address) &&
               (a->value.code.closures == b->value.code.closures);
    case XS_CALLBACK_KIND:
    case XS_CALLBACK_X_KIND:
        return (a->value.callback.address == b->value.callback.address) &&
               (a->value.callback.closures == b->value.callback.closures);
    case XS_HOST_FUNCTION_KIND:
        return a->value.hostFunction.builder == b->value.hostFunction.builder;
    default:
        return 0;
    }
}

static void xs_el_addEventListener(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);    DomNode *n = this_node(the);
    char *type = to_cstring(the, xsArg(0));
    if (!type)
    {
        xsTypeError("out of memory");
    }
    int isClick = (strcmp(type, "click") == 0);
    /* kind selects the delivery class: the page-interactivity class (kind 0)
     * covers click + submit/input/change — the react-dom delegation set the
     * B3 bubbling dispatch (xs_dispatch_element_event) delivers back out;
     * load/error stay the resource-completion channels react-dom parks
     * commits on (the engine delivers cached-asset loads at the next
     * microcheckpoint — see xs_queue_resource_fire). Phase 10 note: the
     * end-event classes (animationend/transitionend) are NOT kind 0 on
     * purpose — dispatch_click delivers kind-0 listeners WITHOUT an
     * exact-type filter (the react anchor only ever holds the delegation
     * click), so a kind-0 animationend would mis-deliver on every tap.
     * They get their own class (kind 4) below and ride the render-done
     * drain instead. */
    int isDispatch = isClick || strcmp(type, "submit") == 0 ||
                     strcmp(type, "input") == 0 ||
                     strcmp(type, "change") == 0;
    int kind = isDispatch ? 0 : (strcmp(type, "load") == 0 ? 1 : 2);
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2: listener registration trace (react pings) */
    {
        const char *tg = (n && n->kind == DOM_ELEMENT && n->tag) ? n->tag : "#other";
        const char *rel = n ? dom_get_attr(n, "rel") : NULL;
        const char *href = n ? dom_get_attr(n, "href") : NULL;
        logger_log("[lsn] type=%s recv=%s rel=%s href=%.60s", type, tg,
                   rel ? rel : "-", href ? href : "-");
    }
#endif
    xsSlot fn = xsArg(1);
    /* DOM-spec re-registration no-op: an addEventListener whose (type,
     * callback, target) tuple exactly matches an existing listener does
     * not grow the set — real browsers dedupe at EventTarget registration
     * before any store. react-dom re-offers the same cached closures on
     * every commit pass, which at JSBRIDGE_LISTENERS_MAX 32 blew the cap
     * with "too many event listeners" and killed the first real-hydration
     * commit. Without this, browsers would NEED removal bookkeeping we
     * don't model (element-level removeEventListener is a noop here —
     * react always re-registers through the same fiber-cached closures,
     * so identical tuples are the re-render replay themselves). A NEW
     * closure (fresh mount after unmount) is a DIFFERENT tuple and gets
     * a slot, matching real semantics. */
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *E = &b->listeners[i];
        if (E->target == n && !strcmp(E->type, type) &&
            xs_same_listener_fn(the, &st->fns[(int)(intptr_t)E->ref], &fn))
        {
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
            logger_log("[lsn] dup type=%s recv=%s (slot %d kept)", type,
                       (n && n->kind == DOM_ELEMENT && n->tag) ? n->tag
                                                               : "#other",
                       i);
#endif
            JFree(type);
            return;
        }
    }
    /* R54: DOMContentLoaded joins the accepted set with its own kind (3)
     * — dispatched ONLY at the script-queue drain (jsbridge.c calls
     * impl->script_queue_drained once the last parked script ran), the
     * browser's parsing-complete moment. No other machinery consults
     * kind 3, so there is no accidental second delivery. */
    if (strcmp(type, "DOMContentLoaded") == 0)
    {
        kind = 3;
    }
    /* Phase 10: the end-event class (animationend/transitionend) — stored
     * for delivery at the render-complete drain (the browser dispatches
     * each after the walk that completed the visible change), NOT the
     * resource channel and NOT the tap path. */
    if (strcmp(type, "animationend") == 0 ||
        strcmp(type, "transitionend") == 0)
    {
        kind = 4;
    }
    /* API-P3: the journal-event class (popstate/hashchange) — window
     * lanes, delivered by the engine's history drain at the pump frame
     * after a back/forward/go (kind 5; see xs_hist_deliver_pending). */
    if (strcmp(type, "popstate") == 0 || strcmp(type, "hashchange") == 0)
    {
        kind = 5;
    }
    /* Forms-Phase 1: the focus/blur class (kind 6) — NON-bubbling
     * (browser-true: focus and blur do not bubble; focusin/focusout
     * would). Phase 3 joins `invalid`: the constraint-validation event
     * doesn't bubble either. Delivered ONLY by the el.focus()/el.blur()
     * roads and the checkValidity()/reportValidity() surface through
     * xs_dispatch_focus_event to the target's own listeners; the tap
     * path and the B3 bubbling dispatcher never consult kind 6, so
     * registering focus/blur/invalid cannot move the standing event
     * counts. */
    if (strcmp(type, "focus") == 0 || strcmp(type, "blur") == 0 ||
        strcmp(type, "invalid") == 0)
    {
        kind = 6;
    }
    if (!isDispatch && strcmp(type, "load") != 0 && strcmp(type, "error") != 0 &&
        kind != 3 && kind != 4 && kind != 5 && kind != 6)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so
         * pages registering them don't error — the delivered classes are
         * click/submit/input/change (kind 0; B3 bubbling dispatch),
         * load/error (kinds 1/2, replay-cached asset completion), the
         * end-event class (kind 4; render-drain dispatch), the
         * journal events (kind 5; history drain dispatch), and the
         * focus/blur class (kind 6; road dispatch — Forms-Phase 1). */
        JFree(type);
        return;
    }
    if (!fxIsCallable(the, &fn))
    {
        JFree(type);
        xsTypeError("listener must be a function");
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        JFree(type);
        xsTypeError("too many event listeners");
    }
    int idx = b->listenerCount++;
    st->fns[idx] = fn;
    xsRemember(st->fns[idx]); /* pin against GC */
    JsListener *L = &b->listeners[idx];
    L->ref = (void *)(intptr_t)idx;
    L->target = n;
    L->kind = kind;
    snprintf(L->type, sizeof(L->type), "%s", type);
    JFree(type);
    if (kind == 1)
    {
        /* A load ping on an asset link = the cached-asset completion the
         * commit is waiting for; queue the fire for the next
         * microcheckpoint (the element's rel is revalidated at fire
         * time — attrs often land after addEventListener here). */
        xs_queue_resource_fire(b, n);
    }
}

/* Shared tag scan: collect element descendants of `start` (start itself
 * excluded, "*" wildcard) into a fresh array in xsResult. Used by both
 * element.getElementsByTagName and the document alias. */
static void xs_collect_tags_into(JsBridge *b, DomNode *start, char *tag)
{
    xsMachine *the = machine_of(b); /* value macros below need `the` */
    xsResult = xsNewArray(0);
    int k = 0;
    if (start)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = start;
        while (top > 0)
        {
            DomNode *cur = stack[--top];
            for (int i = 0; i < cur->childCount; i++)
            {
                DomNode *c = cur->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, tag) || !strcmp(tag, "*"))
                {
                    xsSlot el = xs_push_element(b, c);
                    xsSetAt(xsResult, xsInteger(k++), el);
                }
                if (top + 1 < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
}

static void xs_el_getElementsByTagName(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    char *tag = to_cstring(the, xsArg(0));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
    xs_collect_tags_into(b, n, tag);
    JFree(tag);
}

/* document.getElementsByTagName — the same walk rooted at the document.
 * react-dom's hoistable-resource adopt path scans the document this way
 * (case "link" / case "meta" call document.getElementsByTagName in
 * commitWhileSuspended recovery), so the doc wrapper needs the alias. */
static void xs_doc_getElementsByTagName(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = (b->dom && b->dom->root) ? b->dom->root : NULL;
    char *tag = to_cstring(the, xsArg(0));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
    xs_collect_tags_into(b, n, tag);
    JFree(tag);
}

/* Push the ONE cached wrapper for `node` (browser identity model; NULL →
 * null): instances share a rooted prototype and carry the DomNode* as host
 * data, but wrappers are now memoized per node so JS-visible object
 * identity — and any expando the page wrote (react's fiber key) — survives
 * across pushes. Wrapper == the ts->fns[] pin contract: fxRemember embeds
 * the pinned slot struct into its root chain, so the value is pinned
 * IN the cache slot, never in a local. */
static xsSlot xs_push_element(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b); /* value macros below need `the` */
    if (!node)
    {
        return xsNull;
    }
    /* A2: <script> elements route to a dedicated sub-prototype (instance →
     * scriptProto → elProto) so bundler-parity checks like
     * `document.currentScript instanceof HTMLScriptElement` behave like a
     * browser, where currentScript IS an HTMLScriptElement. Services
     * (getAttribute/login getters) still resolve through the same shared
     * prototype machinery. Synthetic currentScript and dynamically
     * created script elements both pass through here. */
    XsState *st = state_of(b);
    xsSlot proto = st->elProto;
    if (node->kind == DOM_ELEMENT && node->tag &&
        !strcmp(node->tag, "script"))
    {
        proto = st->scriptProto;
    }
    /* Backwards scan: insertion order correlates with reuse recency. */
    for (int i = st->elwCount - 1; i >= 0; i--)
    {
        if (st->elwNode[i] == node)
        {
            return st->elwWrap[i];
        }
    }
    xsSlot obj = xsNewHostInstance(proto);
    xs_set_host_data_site(the, "element", obj, node);
    if (st->elwCount < XS_ELW_CAP)
    {
        xsResult = obj; /* marked-slot anchor while the arrays settle */
        int w = st->elwCount++;
        st->elwNode[w] = node;
        st->elwWrap[w] = obj;
        xsRemember(st->elwWrap[w]);
    }
    else if (!st->elwOverflow)
    {
        st->elwOverflow = 1;
        logger_log("[xs] el wrapper cache full (%d) — new pushes are fresh",
                   XS_ELW_CAP);
    }
    return obj;
}

/* ── console (API-P4: the real log lane) ───────────────────────────────
 * Every method routes into the LOG FILE with its own level tag
 * ([console.log]/[console.info]/[console.debug]/[console.warn]/
 * [console.error]) — the file is the receipt surface (device truth);
 * the app's rendered panel NEVER changes from a console call.
 * Behavior lanes:
 *   1. printf-shaped substitution when the first arg is a string and
 *      carries tokens: %s/%d/%i/%f consume args to String; %o/%O/%j
 *      render the console shape; %c consumes its arg for nothing (no
 *      style on a 1-bit panel); %% = literal. A token with no remaining
 *      arg prints literally (Chrome), extra args append space-separated.
 *   2. console-shape rendering: strings print BARE at the top level,
 *      QUOTED nested; arrays → '[a, b, …]' (4 elements + a count tail);
 *      objects → '{k: v, …}' (6 own-enumerable keys); functions labeled
 *      '[Function]'; unprintables fall back to String(). Depth 2,
 *      bounded bytes — cycle-safe by depth construction.
 *   3. assert(…): logs 'Assertion failed: …' at the error level when the
 *      first arg is falsey — browser semantics: it does NOT throw.
 * A JS counter in the fixture must be able to MATCH the [console.*]
 * rows in the log ("top-line counts match in the log" = the phase bar).
 * Remaining boundary (documented gap, deliberately absent): count/dir/
 * group-family/time/trace — pages that touch them see the same
 * undefined-presence they'd get from an unknown method; inventing a
 * fake click-through surface is not on the table. */

/* Render one argument's console shape into the line buffer at `off`.
 * Actually appending: returns new offset. `nest < 0` = top level
 * (strings bare); `nest >= 0` = inside an object/array (strings
 * quoted). Depth bounded via `nest < 2` descent guard. */
static size_t xs_console_render_arg(xsMachine *the, xsSlot slot, int nest,
                                    char *line, size_t size, size_t off)
{
    if (off + 2 >= size)
    {
        return off;
    }
    int kind = xsTypeOf(slot);
    if (nest < 0)
    {
        if (kind == xsStringType)
        {
            char *s = to_cstring(the, slot);
            size_t n = s ? strlen(s) : 0;
            if (n > 96)
            {
                n = 96; /* per-arg cap at the top: the deepest " receipt */
            }
            if (off + n + 1 >= size)
            {
                n = size - off - 1;
            }
            memcpy(line + off, s ? s : "", n);
            off += n;
            line[off] = '\0';
            if (s)
            {
                JFree(s);
            }
            return off;
        }
    }
    switch (kind)
    {
    case xsNullType:
    case xsUndefinedType:
        off += (size_t)snprintf(line + off, size - off, "%s",
                                kind == xsNullType ? "null" : "undefined");
        return off;
    case xsStringType:
    {
        char *s = to_cstring(the, slot);
        off += (size_t)snprintf(line + off, size - off, "'%.40s'",
                                s ? s : "");
        if (s)
        {
            JFree(s);
        }
        return off;
    }
    case xsBooleanType:
        off += (size_t)snprintf(line + off, size - off, "%s",
                                xsToBoolean(slot) ? "true" : "false");
        return off;
    case xsIntegerType:
    case xsNumberType:
    {
        char *s = to_cstring(the, slot);
        off += (size_t)snprintf(line + off, size - off, "%.40s", s ? s : "");
        if (s)
        {
            JFree(s);
        }
        return off;
    }
    case xsReferenceType:
    {
        if (fxIsCallable(the, &slot))
        {
            off += (size_t)snprintf(line + off, size - off, "[Function]");
            return off;
        }
        /* host objects (element wrappers, XHR wrappers…) render as the
         * honest '[Object]' — they are not data dumps */
        if (xsGetHostDataIf(slot) != NULL)
        {
            off += (size_t)snprintf(line + off, size - off, "[Object]");
            return off;
        }
        if (nest >= 2)
        {
            off += (size_t)snprintf(line + off, size - off, "[Object]");
            return off;
        }
        /* array-like: numeric length + indexed fields */
        xsSlot lenSlot = xsGet(slot, xsID("length"));
        if (xsTypeOf(lenSlot) == xsIntegerType ||
            xsTypeOf(lenSlot) == xsNumberType)
        {
            int len = (int)xsToNumber(lenSlot);
            if (len > 0)
            {
                off += (size_t)snprintf(line + off, size - off, "[");
                int shown = len < 4 ? len : 4;
                for (int i = 0; i < shown && off + 2 < size; i++)
                {
                    /* elements are INDEX-keyed: xsGetIndex, not a string
                     * id (fxNewNameC interns plain names; array slots
                     * live behind the index flavor) */
                    xsSlot v = xsGetIndex(slot, i);
                    if (i)
                    {
                        off += (size_t)snprintf(line + off, size - off, ", ");
                    }
                    off = xs_console_render_arg(the, v, nest + 1, line, size,
                                                off);
                }
                off += (size_t)snprintf(line + off, size - off, len > shown ? ", …%d]" : "]", len);
                return off;
            }
        }
        /* plain object shape: own-key iterator (bounded reads; the
         * iterator dedupes via its visited set) */
        {
            off += (size_t)snprintf(line + off, size - off, "{");
            xsSlot it = xsEnumerate(slot);
            int shownTotal = 0;
            for (;;)
            {
                xsSlot res = xsCall0(it, xsID("next"));
                xsSlot doneS = xsGet(res, xsID("done"));
                if (xsTypeOf(doneS) != xsUndefinedType && xsToBoolean(doneS))
                {
                    break;
                }
                shownTotal++;
                if (shownTotal <= 6 && off + 4 < size)
                {
                    xsSlot key = xsGet(res, xsID("value"));
                    if (xsTypeOf(key) == xsStringType)
                    {
                        char *ks = to_cstring(the, key);
                        /* value get BY SLOT (xsGetAt): string keys
                         * canonicalize whether the thing names a plain
                         * key or an index flavor */
                        xsSlot v = ks ? xsGetAt(slot, key) : xsNull;
                        if (shownTotal > 1)
                        {
                            off += (size_t)snprintf(line + off, size - off,
                                                    ", ");
                        }
                        off += (size_t)snprintf(line + off, size - off,
                                                "%.24s: ", ks ? ks : "?");
                        off = xs_console_render_arg(the, v, nest + 1, line,
                                                    size, off);
                        if (ks)
                        {
                            JFree(ks);
                        }
                    }
                }
                if (shownTotal > 40) /* hard read cap on hostile iterators */
                {
                    break;
                }
            }
            off += (size_t)snprintf(line + off, size - off,
                                    shownTotal > 6 ? ", …}" : "}");
            return off;
        }
    }
    default:
    {
        char *s = to_cstring(the, slot);
        off += (size_t)snprintf(line + off, size - off, "%.40s", s ? s : "");
        if (s)
        {
            JFree(s);
        }
        return off;
    }
    }
}

static size_t xs_console_format_into(xsMachine *the, char *line, size_t size,
                                     size_t off, int first, int argc)
{
    if (argc > first && xsTypeOf(xsArg(first)) == xsStringType)
    {
        /* printf-shaped first argument → substitution */
        char *fmt = to_cstring(the, xsArg(first));
        if (fmt)
        {
            int argi = first + 1;
            for (const char *p = fmt; *p && off + 3 < size; p++)
            {
                if (*p != '%')
                {
                    line[off++] = *p;
                    line[off] = '\0';
                    continue;
                }
                char spec = p[1];
                if (!spec) /* trailing lone '%' */
                {
                    line[off++] = '%';
                    line[off] = '\0';
                    break;
                }
                if (spec == '%') /* literal percent */
                {
                    line[off++] = '%';
                    line[off] = '\0';
                    p++;
                    continue;
                }
                p++;
                if (argi < argc)
                {
                    spec = (spec == 'i') ? 'd' : spec;
                    if (spec != 'c') /* %c consumes its arg silently */
                    {
                        off = xs_console_render_arg(the, xsArg(argi),
                                                    (spec == 's') ? -1 : 0,
                                                    line, size, off);
                    }
                    argi++;
                }
                else
                {
                    /* token with no arg: print it literally (Chrome) */
                    off += (size_t)snprintf(line + off, size - off,
                                            "%%%c", spec);
                }
            }
            /* extra args after the format: space-separated additions */
            for (int i = argi; i < argc && off + 3 < size; i++)
            {
                off += (size_t)snprintf(line + off, size - off, " ");
                off = xs_console_render_arg(the, xsArg(i), -1, line, size,
                                            off);
            }
            JFree(fmt);
        }
    }
    else
    {
        for (int i = first; i < argc && off + 2 < size; i++)
        {
            if (i > first && off + 2 < size)
            {
                off += (size_t)snprintf(line + off, size - off, " ");
            }
            off = xs_console_render_arg(the, xsArg(i), -1, line, size, off);
        }
    }
    line[off] = '\0'; /* the helper owns the terminator */
    return off;
}

/* The [console.*] row publisher: format the args, tag the level, log. */
static void xs_console_publish(xsMachine *the, const char *level)
{
    char line[320];
    size_t off = 0;
    line[0] = '\0';
    int argc = (int)xsToInteger(xsArgc);
    off = xs_console_format_into(the, line, sizeof(line), off, 0, argc);
    (void)off;
    if (!line[0])
    {
        snprintf(line, sizeof(line), "(empty)"); /* an empty log printed */
    }
    logger_log("[console.%s] %s", level, line);
}

static void xs_console_log(xsMachine *the)
{
    xs_console_publish(the, "log");
}

static void xs_console_info(xsMachine *the)
{
    xs_console_publish(the, "info");
}

static void xs_console_debug(xsMachine *the)
{
    xs_console_publish(the, "debug");
}

static void xs_console_warn(xsMachine *the)
{
    xs_console_publish(the, "warn");
}

static void xs_console_error(xsMachine *the)
{
    xs_console_publish(the, "error");
}

static void xs_console_dir(xsMachine *the)
{
    /* dir = inspect: identical surface to log (same render, log level) */
    xs_console_publish(the, "log");
}

static void xs_console_assert(xsMachine *the)
{
    /* browser: assert TRUTHY = silent; falsey = an ERROR row ('Assertion
     * failed: ' + the remaining args through the SAME substitution lane).
     * Does NOT throw. */
    int truthy = 0;
    if (xsToInteger(xsArgc) > 0)
    {
        truthy = xsToBoolean(xsArg(0));
    }
    if (truthy)
    {
        return; /* no row — the assertion holds */
    }
    char line[320];
    memcpy(line, "Assertion failed: ", 18); /* the helper owns the NUL */
    int argc = (int)xsToInteger(xsArgc);
    xs_console_format_into(the, line, sizeof(line), 18, 1, argc);
    logger_log("[console.error] %s", line);
}

/* ── document.write capture (engine-agnostic router buffer) ─────────── */
static void dw_append(JsBridge *b, const char *s)
{
    if (!s || b->outputDropped)
    {
        return;
    }
    if (b->output.len + strlen(s) + 1 > JSBRIDGE_MAX_OUTPUT)
    {
        b->outputDropped = 1;
        return;
    }
    strbuf_append(&b->output, s);
}

static void xs_document_write(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
        dw_append(b, s);
        JFree(s);
    }
}

static void xs_document_writeln(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc; i++)
    {
        BUDGET_OR_THROW(b);
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
        dw_append(b, s);
        JFree(s);
    }
    dw_append(b, "\n");
}

/* ── document object ────────────────────────� */

static DomNode *first_element(DomNode *n, const char *tag)
{
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *c = n->children[i];
        if (c->kind == DOM_ELEMENT && !strcmp(c->tag, tag))
        {
            return c;
        }
    }
    return NULL;
}

/* Depth-first, document-order search for the first element with `tag`.
 * The DOM root is the synthetic #root element, so <html>/<head>/<body>
 * are its GRANDchildren — the direct-children-only first_element() NULLs
 * out head/body. That left document.head null (body survived only via
 * its #root fallback), and emotion's createCache does
 * `container = options.container || document.head` then
 * `container.insertBefore(...)` → TypeError on the first style insert.
 * Iterative DFS, same stack shape as find_element_by_id (device-safe:
 * no recursion), children pushed reversed so document order holds. */
static DomNode *find_first_element(JsBridge *b, const char *tag)
{
    if (!b->dom || !b->dom->root || !tag)
    {
        return NULL;
    }
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = b->dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (n->kind == DOM_ELEMENT && !strcmp(n->tag, tag))
        {
            return n;
        }
        for (int i = n->childCount - 1; i >= 0; i--)
        {
            if (top >= DOM_SEARCH_MAX_DEPTH)
            {
                break;
            }
            stack[top++] = n->children[i];
        }
    }
    return NULL;
}

static DomNode *find_element_by_id(JsBridge *b, const char *id)
{
    if (!b->dom || !b->dom->root || !id)
    {
        return NULL;
    }
    DomNode *hit = dom_find_by_id(b->dom, id);
    /* API-P6: the shared O(depth) id scan replaced the adapter-local
     * push-all-children walk (a body with 65+ element children aborted
     * that walk whole — the probe page hit exactly that and the mid-page
     * ids silently vanished). Also visits in document order now, the
     * browser order, instead of the old LIFO right-first sweep. */
    return hit;
}

static void xs_document_getElementById(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *id = to_cstring(the, xsArg(0));
    if (!id)
    {
        xsTypeError("out of memory");
    }
    DomNode *n = find_element_by_id(b, id);
    /* API-P6: every id lookup documents itself (LOG EVERYTHING rule);
     * the road is the shared O(depth) dom_find_by_id now. */
    logger_log("[getid] '%s' -> %s", id, n ? "HIT" : "NULL");
    JFree(id);
    xsResult = xs_push_element(b, n);
}

static void xs_document_createElement(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *tag = to_cstring(the, xsArg(0));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE) /* A2: who creates elements at hydration-commit time */
    logger_log("[crt] %s", tag);
#endif
    DomNode *el = dom_create_element(b->dom, tag);
    JFree(tag);
    if (!el)
    {
        xsTypeError("createElement failed");
    }
    xsResult = xs_push_element(b, el);
}

static void xs_document_createTextNode(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *txt = to_cstring(the, xsArg(0));
    if (!txt)
    {
        xsTypeError("out of memory");
    }
    DomNode *t = dom_create_text(b->dom, txt);
    JFree(txt);
    if (!t)
    {
        xsTypeError("createTextNode failed");
    }
    xsResult = xs_push_element(b, t);
}

/* document.getElementsByName — pseudo-array of elements whose name
 * attribute matches. Next.js announcer/detection paths read it. */
static void xs_doc_collect_by_name(JsBridge *b, DomNode *n, const char *namev,
                                   xsSlot arr, int *k, int depth)
{
    if (!n || *k >= 64 || depth > DOM_SEARCH_MAX_DEPTH)
    {
        return;
    }
    if (n->kind == DOM_ELEMENT)
    {
        const char *v = dom_get_attr(n, "name");
        if (v && !strcmp(v, namev))
        {
            xsMachine *the = machine_of(b); /* xsSetAt macro needs `the` */
            xsSlot el = xs_push_element(b, n);
            xsSetAt(arr, xsInteger((*k)++), el);
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        xs_doc_collect_by_name(b, n->children[i], namev, arr, k, depth + 1);
    }
}

static void xs_doc_getElementsByName(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    char *namev = to_cstring(the, xsArg(0));
    if (!namev)
    {
        xsTypeError("out of memory");
    }
    xsResult = xsNewArray(0);
    int k = 0;
    if (b->dom && b->dom->root)
    {
        xs_doc_collect_by_name(b, b->dom->root, namev, xsResult, &k, 0);
    }
    JFree(namev);
}

/* A2: document.currentScript — bundled loaders (Turbopack/webpack chunk
 * registration) read it during every <script> run to discover their own
 * chunk URL (`document.currentScript.src` / getAttribute("src")). The
 * ROUTER stamps the running script's open-tag span on the bridge around
 * each run; this synthesizes a detached <script> element carrying the
 * stamped attributes, so getAttribute walks them like any tree element.
 * Identity across reads within one run is not modeled (fresh element per
 * access) — loaders read it once per slot. Outside a script run (timers,
 * import(), page_script) the router's context is clear: null, like a
 * browser. */
static void xs_document_getCurrentScript(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    if (!b || !b->dom || !b->curTagStart || b->curTagLen < 8)
    {
        xsResult = xsNull;
        return;
    }
    /* A2 (throw trace): log the stamped open tag with every read during a
     * run so loader-parity questions ("which script is current at the
     * runtime-module registration?") are answerable from the replay log.
     * The running script index (the same counter the throw trace logs) is
     * included; with the ld+json slot counted it maps 1:1 to the run
     * sequence in the log. */
#ifdef PLUTO_THROW_LOG
    logger_log("[cs] script %d stamp: %.*s",
               g_pluto_throw_script,
               (int)(b->curTagLen > 120 ? 120 : b->curTagLen),
               b->curTagStart);
#endif
    DomNode *el = dom_create_element(b->dom, "script");
    if (!el)
    {
        xsResult = xsNull;
        return;
    }
    /* Attribute walk over the stamped "<script …>" open-tag span: parse
     * name[=value] pairs (quoted or bare values; valueless attrs store
     * ""). The tag name itself ("<script", 7 bytes after the '<') is
     * skipped — attributes start after it. Names are lowercased (HTML
     * attribute names are case-insensitive and the tree stores them
     * lowercase — getAttribute("src") then works for SRC= pages too).
     * Values keep the raw span bytes: bundler src URLs (chunk paths,
     * data: URLs) are entity-free, matching how the scanners capture
     * script-tag attributes. */
    const char *p = b->curTagStart + 1 + 7; /* after '<script' */
    const char *lim = b->curTagStart + b->curTagLen - 1; /* exclude '>' */
    int haveSrc = 0;
    while (p < lim)
    {
        while (p < lim && (*p == ' ' || *p == '\t' || *p == '\n' ||
                           *p == '\r' || *p == '/'))
        {
            p++; /* whitespace / stray self-close slash */
        }
        if (p >= lim)
        {
            break;
        }
        const char *n0 = p;
        while (p < lim && *p != '=' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r' && *p != '>' && *p != '/')
        {
            p++;
        }
        size_t nlen = (size_t)(p - n0);
        if (nlen >= 48)
        {
            p++; /* pathological name: step past it, keep scanning */
            continue;
        }
        if (nlen == 0)
        {
            continue;
        }
        char name[48];
        for (size_t k = 0; k < nlen; k++)
        {
            name[k] = (n0[k] >= 'A' && n0[k] <= 'Z') ? (char)(n0[k] + 32)
                                                     : n0[k];
        }
        name[nlen] = '\0';
        const char *v0 = NULL;
        size_t vlen = 0;
        if (p < lim && *p == '=')
        {
            p++;
            while (p < lim && (*p == ' ' || *p == '\t' || *p == '\n' ||
                               *p == '\r'))
            {
                p++;
            }
            if (p < lim && (*p == '"' || *p == '\''))
            {
                char q = *p++;
                v0 = p;
                while (p < lim && *p != q)
                {
                    p++;
                }
                vlen = (size_t)(p - v0);
                if (p < lim)
                {
                    p++; /* closing quote */
                }
            }
            else
            {
                v0 = p;
                while (p < lim && *p != ' ' && *p != '\t' && *p != '\n' &&
                       *p != '\r' && *p != '>')
                {
                    p++;
                }
                vlen = (size_t)(p - v0);
            }
        }
        char *v = NULL;
        if (v0 && vlen)
        {
            v = (char *)JMalloc(vlen + 1);
            if (v)
            {
                memcpy(v, v0, vlen);
                v[vlen] = '\0';
            }
        }
        dom_set_attr(b->dom, el, name, v ? v : "");
        if (!strcmp(name, "src"))
        {
            haveSrc = 1;
        }
        if (v)
        {
            JFree(v);
        }
    }
    /* A2: bundler loaders read currentScript.src during module-registry
     * drains that our engine executes while an inline bootstrap slot is
     * still stamped (Next.js getAssetPrefix: `new URL(
     * document.currentScript.src)` → the page's URL polyfill rejects ""). A
     * real browser resolves that read against a src-bearing chunk script
     * (the loader's own /_next/ prefix), never a bare inline tag. When the
     * stamped open tag carries no src attribute (inline slot), attach the
     * router's stamped external chunk URL (curSrcUrl) so src-based
     * asset-prefix discovery behaves like the site instead of parsing an
     * empty URL. getAttribute("src") on a genuinely src-bearing stamped tag
     * keeps the raw attribute — the fallback only fires for inline slots. */
    if (!haveSrc && b->lastExtSrcUrl)
    {
        dom_set_attr(b->dom, el, "src", b->lastExtSrcUrl);
#ifdef PLUTO_THROW_LOG
        logger_log("[cs] script %d src-fallback: %s",
                   g_pluto_throw_script, b->lastExtSrcUrl);
#endif
    }
    xsResult = xs_push_element(b, el);
}

/* document.title: walk the live tree's <title> so scripts and the chrome
 * agree (doc->title is only filled by the walker, after scripts). */
static void xs_document_getTitle(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    if (b->dom && b->dom->root)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = b->dom->root;
        while (top > 0)
        {
            DomNode *n = stack[--top];
            for (int i = 0; i < n->childCount; i++)
            {
                DomNode *c = n->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, "title"))
                {
                    char buf[256];
                    doc_concat_node_text(c, buf, sizeof(buf));
                    xsResult = xsString(buf);
                    return;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    xsResult = xsString(b->doc->title[0] ? b->doc->title : "");
}

/* ── document-level appendChild/insertBefore/removeChild (B5-follow) ──────
 * react-dom's document-container commits (insertInContainerBefore /
 * removeChildFromContainer / clearDocument's node walk) call these on the
 * document object; the wrapper previously lacked them and every call threw
 * `call: not a function` (the fallback wipe then hit missing-surface
 * errors). Browser mapping: the synthetic #root owns the document's
 * children (doctype is not represented), so these route into/out of
 * b->dom->root's children via the same node ops the element methods use. */
static void xs_document_appendChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    if (!b || !b->dom || !b->dom->root)
    {
        xsTypeError("document.appendChild: no document");
    }
    DomNode *child = arg_node(the, 0);
    if (!child)
    {
        xsTypeError("appendChild: node expected");
    }
    if (dom_append_child(b->dom, b->dom->root, child) != 0)
    {
        xsTypeError("appendChild failed");
    }
    b->domDirty = 1; /* R58: live-tree mutation → frame-loop re-walk */
    jsbridge_note_dom_mutation(b, child->parent); /* Phase 10: subtree grew */
    xsResult = xsArg(0);
}

static void xs_document_insertBefore(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    if (!b || !b->dom || !b->dom->root)
    {
        xsTypeError("document.insertBefore: no document");
    }
    DomNode *child = arg_node(the, 0);
    DomNode *ref = arg_node(the, 1);
    if (!child || (xsTypeOf(xsArg(1)) != xsUndefinedType && !ref))
    {
        xsTypeError("insertBefore: node expected");
    }
    if (dom_insert_before(b->dom, b->dom->root, child, ref) != 0)
    {
        xsTypeError("insertBefore failed");
    }
    b->domDirty = 1; /* R58: live-tree mutation → frame-loop re-walk */
    jsbridge_note_dom_mutation(b, child->parent); /* Phase 10: subtree moved */
    xsResult = xsArg(0);
}

static void xs_document_removeChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    if (!b || !b->dom || !b->dom->root)
    {
        xsTypeError("document.removeChild: no document");
    }
    DomNode *child = arg_node(the, 0);
    if (!child)
    {
        xsTypeError("removeChild: node expected");
    }
    DomNode *container = child->parent ? child->parent
                                       : b->dom->root; /* Phase 10: the
                     * element that loses the child (grabbed before the
                     * detach — dom_remove_child clears child->parent). */
    if (dom_remove_child(b->dom->root, child) != 0)
    {
        xsTypeError("removeChild failed");
    }
    b->domDirty = 1; /* R58: live-tree mutation → frame-loop re-walk */
    jsbridge_note_dom_mutation(b, container); /* Phase 10: subtree shrank */
    xsResult = xsArg(0);
}

/* body/head/documentElement/activeElement as LIVE getters. The init-time
 * def_val coding bound wrappers ONCE (stale if react swaps head/body on a
 * fallback re-render — the stale `document.body` then reports a detached
 * subtree). Same fallbacks as the old bindings: body/activeElement keep
 * the #root fallback for fragment documents; head falls back to the
 * document element so CSS-in-JS hosts (emotion `container ||
 * document.head`) always get an insertable node. */
static void xs_document_getBodyLike(xsMachine *the, const char *tag,
                                    int inactive)
{
    (void)inactive;
    JsBridge *b = bridge_of(the);
    DomNode *root = (b && b->dom) ? (DomNode *)b->dom->root : NULL;
    if (!root)
    {
        xsResult = xsNull;
        return;
    }
    DomNode *hit = find_first_element(b, tag);
    xsResult = xs_push_element(b, hit ? hit : root);
}

static void xs_document_getBody(xsMachine *the)
{
    xs_document_getBodyLike(the, "body", 0);
}

static void xs_document_getActiveElement(xsMachine *the)
{
    xs_document_getBodyLike(the, "body", 1);
}

static void xs_document_getHeadLike(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *root = (b && b->dom) ? (DomNode *)b->dom->root : NULL;
    if (!root)
    {
        xsResult = xsNull;
        return;
    }
    DomNode *head = find_first_element(b, "head");
    if (!head)
    {
        head = find_first_element(b, "html");
    }
    xsResult = xs_push_element(b, head ? head : root);
}

static void xs_document_getHead(xsMachine *the)
{
    xs_document_getHeadLike(the);
}

static void xs_document_getDocumentElement(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    DomNode *root = (b && b->dom) ? (DomNode *)b->dom->root : NULL;
    if (!root)
    {
        xsResult = xsNull;
        return;
    }
    DomNode *html = find_first_element(b, "html");
    xsResult = xs_push_element(b, html ? html : root);
}

/* R54: document.readyState — browser parse phases. While the per-frame
 * script queue (the parked attach plan) still has entries the document
 * is effectively still "loading": the sync attach loop ran every script
 * before any async boundary EVER observed the page, so reflect that
 * truth here. Fork-bundles branch on exactly this read — next.js's
 * app-router prefers DOMContentLoaded for its RSC stream closer when
 * readyState is "loading" at eval and only otherwise setTimeout()s it;
 * the callback timer (its 50ms scheduler) firing BETWEEN queued script
 * frames voided the flight buffer mid-push ("Unexpected server data:
 * missing bootstrap script." from the bundle's L()) — the throw that
 * made the contact page un-hydratable under the queue. Undefined
 * up to now: `undefined === "loading"` fell into the timer branch on
 * every build (also pre-queue), hidden only because sync windows never
 * delivered timers mid-stage. */
static void xs_document_readyState(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    xsResult = xsString((b && b->scriptQueueNext < b->scriptQueueCount)
                            ? "loading"
                            : "interactive");
}

/* document/window.addEventListener: B3 — delegated listeners register with
 * target = the live document root node so the bubbling dispatch
 * (xs_dispatch_element_event) delivers to them. react-dom's app-router
 * hydration uses the DOCUMENT as its root container and calls
 * document.addEventListener('click'|'submit'|… , handler) at commit — the
 * old silent no-op dropped every delegated registration, which is why the
 * click dispatch found no handlers (`[lsn]` showed only the non-delegated
 * load/error/invalid pings). The kind map + acceptance filter mirror
 * xs_el_addEventListener; same-node duplicate deliveries (window vs
 * document aliases) are left to the caller, like a browser. */
static void xs_doc_addEventListener(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    DomNode *target = (b->dom && b->dom->root) ? b->dom->root : NULL;
    if (!target)
    {
        return; /* no live tree: nothing to attach to */
    }
    char *type = to_cstring(the, xsArg(0));
    if (!type)
    {
        xsTypeError("out of memory");
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[lsn] type=%s recv=#doc", type);
#endif
    int isClick = (strcmp(type, "click") == 0);
    int isDispatch = isClick || strcmp(type, "submit") == 0 ||
                     strcmp(type, "input") == 0 ||
                     strcmp(type, "change") == 0;
    int kind = isDispatch ? 0 : (strcmp(type, "load") == 0 ? 1 : 2);
    /* R54: DOMContentLoaded gets its own kind (3) — dispatched ONLY at
     * the script-queue drain (impl->script_queue_drained), the browser's
     * parsing-complete moment; next.js's app-router arms its RSC stream
     * closer (M) here when readyState was "loading" at eval. */
    if (strcmp(type, "DOMContentLoaded") == 0)
    {
        kind = 3;
    }
    /* Phase 10: document-level end-events join the stored set (kind 4);
     * the drain dispatches element-targeted and document-registered
     * pairs through the same exact-type climb (the target walk reaches
     * the root, where these live). */
    if (strcmp(type, "animationend") == 0 ||
        strcmp(type, "transitionend") == 0)
    {
        kind = 4;
    }
    /* API-P3: document/window journal events (kind 5) — popstate/
     * hashchange are window lanes; this is where window.addEventListener
     * lands too, so both round into the same book (see the element-path
     * note; delivery = xs_hist_deliver_pending). */
    if (strcmp(type, "popstate") == 0 || strcmp(type, "hashchange") == 0)
    {
        kind = 5;
    }
    if (!isDispatch && strcmp(type, "load") != 0 && strcmp(type, "error") != 0 &&
        kind != 3 && kind != 4 && kind != 5)
    {
        /* Same UNSUPPORTED silence as the element path (see above). */
        JFree(type);
        return;
    }
    xsSlot fn = xsArg(1);
    if (!fxIsCallable(the, &fn))
    {
        JFree(type);
        xsTypeError("listener must be a function");
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        JFree(type);
        xsTypeError("too many event listeners");
    }
    int idx = b->listenerCount++;
    st->fns[idx] = fn;
    xsRemember(st->fns[idx]); /* pin against GC */
    JsListener *L = &b->listeners[idx];
    L->ref = (void *)(intptr_t)idx;
    L->target = target;
    L->kind = kind;
    snprintf(L->type, sizeof(L->type), "%s", type);
    JFree(type);
    /* NOTE: kind==1 on the DOCUMENT is NOT queued into the resource fire —
     * element-level load listeners own that channel; a second fire would
     * run react's asset-completion listener twice. */
}

static void xs_location_assign(xsMachine *the)
{
    char *u = to_cstring(the, xsArg(0));
    logger_log("[js] location.assign/replace ignored: %s", u ? u : "");
    if (u)
    {
        JFree(u);
    }
}

static void xs_alert(xsMachine *the)
{
    char *s = to_cstring(the, xsArg(0));
    logger_log("[js alert] %s", s ? s : "");
    if (s)
    {
        JFree(s);
    }
}

static void xs_noop(xsMachine *the)
{
    (void)the;
}

/* performance.now for the A2 bridge globals — ms from the same monotonic
 * clock the pump budgets use. React's scheduler calls this every work
 * slice; NaN is wrong (lanes math compares deltas), so return a real
 * float. Sim keeps clock_gettime's FRACTIONAL ms; the device has no
 * CLOCK_MONOTONIC in the toolchain (and clock_gettime is
 * macro-redefined by the QuickJS shim there — see the sim-frame probe in
 * the split path) so it reads the SDK's monotonic ms clock (integer ms:
 * sub-ms lane scheduling degrades gracefully). */
static void xs_performance_now(xsMachine *the)
{
    double ms = 0.0;
#if defined(TARGET_PLAYDATE)
    PlaydateAPI *pdp = pluto_pd();
    ms = pdp ? (double)pdp->system->getCurrentTimeMilliseconds() : 0.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
    xsResult = xsNumber(ms);
}

/* performance.getEntriesByType/getEntries — React 19's commit-suspend
 * path dereferences `performance.getEntriesByType` BEFORE its typeof
 * check, so the global must exist; entries themselves never exist in
 * this environment (no resource/page timing), and React degrades to
 * its navigator.connection fallback with an empty array. */
static void xs_performance_getEntriesByType(xsMachine *the)
{
    (void)the;
    xsResult = xsNewArray(0);
}

static void xs_computed_getPropertyValue(xsMachine *the)
{
    char *property = to_cstring(the, xsArg(0));
    if (!property) xsTypeError("out of memory");
    char value[128];
    xs_style_read_prop(this_node(the), property, value, sizeof(value));
    JFree(property);
    xsResult = xsString(value);
}

/* ── API-P3: the address journal ───────────────────────────────────────
 * history.pushState/replaceState write the ENGINE'S OWN book (the
 * bridge's histUrl[] ring, seeded with the doc base on first use) and
 * NEVER touch the wire: no http_* call, no pendingNav, no app-bar
 * change. Same-origin: a resolved entry whose scheme+authority head
 * differs from the current entry's throws (browser SecurityError
 * behavior); fragment/query/path-only forms are same-origin by
 * construction. Oversized URLs throw like a browser's malformed-URL
 * push. The journal feeds the LIVE location getters and the
 * back/forward/go delivery (popstate/hashchange, drained at the next
 * pump frame — never mid-eval). */

/* scheme+authority head of a journal URL for the same-origin check:
 * 'http://x:8080' head = 'http://x:8080'; about:/data: opaque tokens are
 * their whole form ('about:probe'). */
static void xs_hist_origin_of(const char *url, char *out, size_t outLen)
{
    if (!url || !url[0] || outLen == 0)
    {
        if (outLen)
        {
            out[0] = '\0';
        }
        return;
    }
    const char *auth = strstr(url, "://");
    if (auth)
    {
        const char *slash = strchr(auth + 3, '/');
        size_t n = slash ? (size_t)(slash - url) : strlen(url);
        if (n >= outLen)
        {
            n = outLen - 1;
        }
        memcpy(out, url, n);
        out[n] = '\0';
    }
    else
    {
        /* opaque token: everything up to (and excluding) an optional
         * fragment/query tail */
        size_t n = strcspn(url, "#?");
        if (n >= outLen)
        {
            n = outLen - 1;
        }
        memcpy(out, url, n);
        out[n] = '\0';
    }
}

static const char *xs_hist_current(JsBridge *b)
{
    if (b->histCount > 0 && b->histUrl[b->histIdx][0])
    {
        return b->histUrl[b->histIdx];
    }
    return (b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl : "";
}

/* Seed entry 0 with the doc base on first journal use. */
static void xs_hist_seed(JsBridge *b)
{
    if (b->histCount > 0)
    {
        return;
    }
    const char *base = (b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl : "";
    snprintf(b->histUrl[0], JSBRIDGE_HIST_URL_MAX, "%s", base);
    b->histCount = 1;
    b->histIdx = 0;
}

/* Latch a pending delivery at a journal move (from→to URLs recorded for
 * the hashchange event). hashchange fires only when the FRAGMENT field
 * itself moved (browser semantics — a query move is not a hash move). */
static void xs_hist_arm_delivery(JsBridge *b, const char *fromUrl,
                                 const char *toUrl)
{
    XsState *st = (XsState *)b->implState;
    if (!st)
    {
        return;
    }
    st->histPendPop = 1;
    if (fromUrl && toUrl)
    {
        char fh[JSBRIDGE_HIST_URL_MAX], th[JSBRIDGE_HIST_URL_MAX];
        const char *fh2 = fromUrl ? strchr(fromUrl, '#') : NULL;
        const char *th2 = toUrl ? strchr(toUrl, '#') : NULL;
        snprintf(fh, sizeof(fh), "%s", fh2 ? fh2 + 1 : "");
        snprintf(th, sizeof(th), "%s", th2 ? th2 + 1 : "");
        if (strcmp(fh, th) != 0) /* hash really moved → hashchange */
        {
            st->histPendHash = 1;
            snprintf(st->histFromUrl, JSBRIDGE_HIST_URL_MAX, "%s", fromUrl);
            snprintf(st->histToUrl, JSBRIDGE_HIST_URL_MAX, "%s", toUrl);
        }
    }
}

/* pushState(mode=1) / replaceState(mode=0) — shared body. */
static void xs_history_setState_common(xsMachine *the, int mode)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_hist_seed(b);
    /* state (arg 0) pins per-entry; absent → null (browser default) */
    xsSlot stateSlot = (xsToInteger(xsArgc) > 0) ? xsArg(0) : xsNull;

    /* url (arg 2): optional, relative-resolved; browser coerces
     * undefined to the current URL */
    char *rawUrl = NULL;
    if (xsToInteger(xsArgc) > 2)
    {
        rawUrl = to_cstring(the, xsArg(2)); /* NULL for undefined */
    }
    char abs[JSBRIDGE_HIST_URL_MAX];
    int ok = jsbridge_hist_resolve_url(b, rawUrl ? rawUrl : "", abs,
                                       sizeof(abs));
    if (rawUrl)
    {
        JFree(rawUrl);
    }
    if (!ok)
    {
        xsTypeError("history: bad url"); /* browser: malformed push throws */
    }
    /* same-origin guard: scheme+authority head must match the current
     * entry (browser throws SecurityError on cross-origin pushes) */
    char orgNew[160], orgCur[160];
    xs_hist_origin_of(abs, orgNew, sizeof(orgNew));
    xs_hist_origin_of(xs_hist_current(b), orgCur, sizeof(orgCur));
    if (strcmp(orgNew, orgCur) != 0)
    {
        xsTypeError("history: cross-origin push");
    }

    char fromUrl[JSBRIDGE_HIST_URL_MAX];
    snprintf(fromUrl, sizeof(fromUrl), "%s", xs_hist_current(b));
    if (mode) /* push: grow the book (overflow = reuse the top slot — a
               * full journal replaces the head entry, like a short
               * browser session limit; length semantics stay browser-
               * shaped: grows on push) */
    {
        if (b->histIdx < b->histCount - 1)
        {
            b->histCount = b->histIdx + 1; /* re-push truncates forward */
        }
        if (b->histCount < JSBRIDGE_HIST_MAX)
        {
            b->histIdx = b->histCount;
            b->histCount++;
        }
        else
        {
            b->histIdx = JSBRIDGE_HIST_MAX - 1;
        }
    }
    else /* replace: overwrite the current entry in place */
    {
        /* no index/count change */
    }
    snprintf(b->histUrl[b->histIdx], JSBRIDGE_HIST_URL_MAX, "%s", abs);

    /* pin the state for the entry (replace re-pins over the old slot) */
    if (st)
    {
        if (st->histStateSet[b->histIdx])
        {
            xsForget(st->histState[b->histIdx]);
        }
        st->histState[b->histIdx] = stateSlot;
        st->histStateSet[b->histIdx] = 1;
        xsRemember(st->histState[b->histIdx]);
    }

    logger_log("[js] history %s j=%d n=%d url='%.80s' (no network "
               "navigation)",
               mode ? "pushState" : "replaceState", b->histIdx, b->histCount,
               abs);
    /* pushState/replaceState do NOT fire popstate (browser core); only
     * the URL surface moves. */
    (void)fromUrl;
}

static void xs_history_pushState(xsMachine *the)
{
    xs_history_setState_common(the, 1);
}

static void xs_history_replaceState(xsMachine *the)
{
    xs_history_setState_common(the, 0);
}

/* back/forward/go: move the current index (clamped; at-boundary moves
 * are no-ops like a browser's edge of the session), then latch the
 * pending popstate/hashchange for the next pump frame. */
static void xs_history_move(xsMachine *the, int delta)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_hist_seed(b);
    int target = b->histIdx + delta;
    if (target < 0)
    {
        target = 0;
    }
    if (target > b->histCount - 1)
    {
        target = b->histCount - 1;
    }
    if (target == b->histIdx)
    {
        logger_log("[js] history go %+d at boundary (noop)", delta);
        return;
    }
    char fromUrl[JSBRIDGE_HIST_URL_MAX];
    snprintf(fromUrl, sizeof(fromUrl), "%s", xs_hist_current(b));
    b->histIdx = target;
    const char *to = xs_hist_current(b);
    logger_log("[js] history back/forward → j=%d url='%.80s' (popstate "
               "pending)",
               b->histIdx, to);
    xs_hist_arm_delivery(b, fromUrl, to);
}

static void xs_history_back(xsMachine *the)
{
    xs_history_move(the, -1);
}

static void xs_history_forward(xsMachine *the)
{
    xs_history_move(the, 1);
}

static void xs_history_go(xsMachine *the)
{
    int n = xsToInteger(xsArgc) > 0 ? xsToInteger(xsArg(0)) : 0;
    xs_history_move(the, n);
}

static int xs_hist_current_slot(XsState *st, JsBridge *b)
{
    return (st && b->histCount > 0 && b->histIdx < JSBRIDGE_HIST_MAX &&
            st->histStateSet[b->histIdx])
               ? b->histIdx
               : -1;
}

/* live getters */
static void xs_history_getState(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_hist_seed(b);
    int slot = xs_hist_current_slot(st, b);
    xsResult = (st && slot >= 0) ? st->histState[slot] : xsNull;
}

static void xs_history_getLength(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    xs_hist_seed(b);
    xsResult = xsInteger(b->histCount);
}

/* ── API-P3: LIVE location getters ────────────────────────────────────
 * Every property read re-derives from the address journal's current
 * entry (seeded from the doc base), so a scripted pushState/replaceState
 * moves the whole surface immediately — no stale bridge-time snapshot.
 * Component logic reproduces the previous one-time builder's shapes
 * exactly (origin/protocol/host/hostname/port defaulting, '/' pathname
 * fallback, '?q'/'#h' prefixes); only the source URL went live.
 * document-anchor search/hash getters (xs_el_get_search) still read
 * ELEMENT host data — these are the window/location ones. */

/* Parse helper: seeds the journal, then parses the CURRENT entry. The
 * per-getter static UrlParsed hoists the parse result off the small
 * stacks (url.c's hoist pattern). */
static int xs_location_parse_current(JsBridge *b, UrlParsed *parsed)
{
    xs_hist_seed(b);
    return url_parse(xs_hist_current(b), parsed) == 0;
}

static void xs_location_get_href(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    xs_hist_seed(b);
    xsResult = xsString(xs_hist_current(b));
}

static void xs_location_get_origin(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    char host[144], port[12];
    int ok = xs_location_parse_current(b, &parsed);
    int defaultPort = ok && ((parsed.isSsl && parsed.port == 443) ||
                             (!parsed.isSsl && parsed.port == 80));
    snprintf(port, sizeof(port), "%d", ok ? parsed.port : 0);
    snprintf(host, sizeof(host), "%s%s%s", ok ? parsed.host : "",
             ok && !defaultPort ? ":" : "", ok && !defaultPort ? port : "");
    char origin[176];
    snprintf(origin, sizeof(origin), "%s://%s", ok ? parsed.scheme : "", host);
    xsResult = xsString(ok ? origin : "null");
}

static void xs_location_get_protocol(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    int ok = xs_location_parse_current(b, &parsed);
    char component[64];
    snprintf(component, sizeof(component), "%s:", ok ? parsed.scheme : "");
    xsResult = xsString(component);
}

static void xs_location_get_host(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    char port[12];
    int ok = xs_location_parse_current(b, &parsed);
    int defaultPort = ok && ((parsed.isSsl && parsed.port == 443) ||
                             (!parsed.isSsl && parsed.port == 80));
    snprintf(port, sizeof(port), "%d", ok ? parsed.port : 0);
    char host[144];
    snprintf(host, sizeof(host), "%s%s%s", ok ? parsed.host : "",
             ok && !defaultPort ? ":" : "", ok && !defaultPort ? port : "");
    xsResult = xsString(host);
}

static void xs_location_get_hostname(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    int ok = xs_location_parse_current(b, &parsed);
    xsResult = xsString(ok ? parsed.host : "");
}

static void xs_location_get_port(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    char port[12];
    int ok = xs_location_parse_current(b, &parsed);
    int defaultPort = ok && ((parsed.isSsl && parsed.port == 443) ||
                             (!parsed.isSsl && parsed.port == 80));
    snprintf(port, sizeof(port), "%d", ok ? parsed.port : 0);
    xsResult = xsString(ok && !defaultPort ? port : "");
}

static void xs_location_get_pathname(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    int ok = xs_location_parse_current(b, &parsed);
    xsResult = xsString(ok ? parsed.path : "/");
}

static void xs_location_get_search(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    char component[264];
    int ok = xs_location_parse_current(b, &parsed);
    snprintf(component, sizeof(component), "%s%s",
             ok && parsed.query[0] ? "?" : "", ok ? parsed.query : "");
    xsResult = xsString(component);
}

static void xs_location_get_hash(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    static UrlParsed parsed;
    char component[264];
    int ok = xs_location_parse_current(b, &parsed);
    snprintf(component, sizeof(component), "%s%s",
             ok && parsed.hash[0] ? "#" : "", ok ? parsed.hash : "");
    xsResult = xsString(component);
}

static void xs_getComputedStyle(xsMachine *the)
{
    DomNode *node = xsToInteger(xsArgc) > 0 && xsTypeOf(xsArg(0)) == xsReferenceType
                        ? (DomNode *)xsGetHostData(xsArg(0)) : NULL;
    xsResult = xsNewHostObject(NULL);
    xs_set_host_data_site(the, "computedStyle", xsResult, node);
    xsDefine(xsResult, xsID("getPropertyValue"),
             xsNewHostFunction(xs_computed_getPropertyValue, 1), xsDefault);
}

/* ── remaining R17 element/document surface (parity with QuickJS bridge) ─ */
/* The device layout is one flowing column with no per-node geometry, so the
 * honest resolved rect is the origin: element math (MUI transitions, sticky
 * headers, tooltip positioning) stays well-defined and degrades to "no
 * offset" — same contract as the QuickJS bridge. */
static void xs_el_getBoundingClientRect(xsMachine *the)
{
    static const char *const fields[] = {"top", "left", "right", "bottom",
                                         "width", "height", "x", "y"};
    xsResult = xsNewObject();
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        xsDefine(xsResult, xsID(fields[i]), xsInteger(0), xsDefault);
    }
}

/* Comment nodes materialize as detached empty text nodes (never rendered);
 * React uses them as insertion markers for fragments/suspense. */
static void xs_document_createComment(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char scratch[2] = "";
    char *data = to_cstring(the, xsArg(0));
    const char *raw = data ? data : scratch; /* createComment() ok w/o args */
    DomNode *t = dom_create_comment(b->dom, raw);
    JFree(data);
    if (!t)
    {
        xsTypeError("createComment failed");
    }
    xsResult = xs_push_element(b, t);
}

/* HTML-only tree: the namespace argument is ignored (React DOM's commit
 * phase creates every element through createElementNS). */
static void xs_document_createElementNS(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    char *tag = to_cstring(the, xsArg(1));
    if (!tag)
    {
        xsTypeError("out of memory");
    }
    DomNode *el = dom_create_element(b->dom, tag);
    JFree(tag);
    if (!el)
    {
        xsTypeError("createElementNS failed");
    }
    xsResult = xs_push_element(b, el);
}

/* SPA pages retitle themselves from route effects; rewrite the <title>
 * element like a browser so the chrome and the walker agree. */
static void xs_document_setTitle(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    char *v = to_cstring(the, xsArg(0));
    if (!v)
    {
        xsTypeError("out of memory");
    }
    if (b->dom && b->dom->root)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = b->dom->root;
        while (top > 0)
        {
            DomNode *n = stack[--top];
            for (int i = 0; i < n->childCount; i++)
            {
                DomNode *c = n->children[i];
                if (c->kind != DOM_ELEMENT)
                {
                    continue;
                }
                if (!strcmp(c->tag, "title"))
                {
                    /* API-P3: browser-faithful set — REPLACES the title
                     * node's text (the old append left 'API Probe' glued
                     * to the new value and the getter read the concat).
                     * dom_set_text swaps text-node children for one node;
                     * a <title> only ever holds text. */
                    dom_set_text(b->dom, c, v);
                    top = 0; /* stop the walk */
                    break;
                }
                if (top < DOM_SEARCH_MAX_DEPTH)
                {
                    stack[top++] = c;
                }
            }
        }
    }
    snprintf(b->doc->title, sizeof(b->doc->title), "%s", v);
    JFree(v);
}

/* ── event object (click dispatch) ───────────────────� */
static void xs_event_preventDefault(xsMachine *the)
{
    bridge_of(the)->preventDef = 1;
}

/* B3: honored on OUR event objects during the bubbled walk (react's
 * synthetic wrappers around them keep their own copies of the flag).
 * Latches b->stopProp; the dispatch stops climbing past whatever node
 * the listener was attached to (later listeners on the SAME node still
 * run, like a browser). */
static void xs_event_stopPropagation(xsMachine *the)
{
    bridge_of(the)->stopProp = 1;
}

/* ── prototype + globals (runs inside xsBeginHostExit at init) ───────── */

/* Helper: define a data property `name` = function `cb` on `obj`.
 * GC contract: `obj` may exist only in the caller's C local, and C locals
 * are NOT GC roots. xsResult (the->frame[1]) IS a marked machine slot, so
 * pin the target there BEFORE xsNewHostFunction allocates (a collect here
 * would sweep the target and reuse its slot as the new function — the
 * "xsSetHostData: not a host object" crash). */
static void def_fn(xsMachine *the, xsSlot obj, const char *name,
                   xsCallback cb, int length)
{
    xsResult = obj;
    xsSlot fn = xsNewHostFunction(cb, length);
    xsDefine(xsResult, xsID(name), fn, xsDefault);
}

/* Helper: define a plain data property. Same GC contract as def_fn. */
static void def_val(xsMachine *the, xsSlot obj, const char *name,
                    xsSlot value)
{
    (void)the;
    xsResult = obj;
    xsDefine(xsResult, xsID(name), value, xsDefault);
}

/* ── CSSOM StyleSheet shim (text-backed) ─────────────────�
 * Browsers expose `.sheet` (a StyleSheet) on <style>/<link> elements, and
 * CSS-in-JS engines (jss: `e.cssRules.length` index math; emotion speedy:
 * `e.sheet` + `insertRule`) call into it unguarded during React commits.
 * With no CSSOM, `this.element.sheet` is undefined and jss's index helper
 * throws on the first dynamic style — the TypeError escapes the effect,
 * React's commit re-throws it at the root, and the whole bootstrap dies
 * (R25). Our CSSOM is STATELESS and derived from the style element's
 * text: `cssRules` splits the text into `}`-terminated rule chunks,
 * `insertRule` appends the rule to the text (so the renderer's CSS pass
 * sees it), `deleteRule` rebuilds the text without the block. */
static void xs_sheet_get_cssRules(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    xsResult = xsNewArray(0);
    if (!n || n->kind != DOM_ELEMENT || !n->text)
    {
        return;
    }
    int k = 0;
    int depth = 0;
    const char *s = n->text;
    size_t start = 0;
    for (size_t i = 0; s[i]; i++)
    {
        if (s[i] == '{')
        {
            depth++;
        }
        else if (s[i] == '}')
        {
            if (depth > 0)
            {
                depth--;
            }
            if (depth == 0)
            {
                size_t len = i + 1 - start;
                char *rule = (char *)JMalloc(len + 1);
                if (!rule)
                {
                    break;
                }
                memcpy(rule, s + start, len);
                rule[len] = 0;
                xsSetAt(xsResult, xsInteger(k++), xsString(rule));
                JFree(rule);
                start = i + 1;
            }
        }
    }
}

static void xs_sheet_get_ownerNode(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    xsResult = xs_push_element(b, n);
}

static void xs_sheet_insertRule(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        xsTypeError("insertRule: not a stylesheet");
    }
    char *rule = to_cstring(the, xsArg(0));
    if (!rule)
    {
        xsTypeError("out of memory");
    }
    txInteger index = 0; /* clamped by the caller via cssRules.length */
    if ((int)xsToInteger(xsArgc) > 1)
    {
        txInteger v = xsToInteger(xsArg(1));
        if (v > 0)
        {
            index = v;
        }
    }
    size_t oldLen = n->text ? strlen(n->text) : 0;
    size_t ruleLen = strlen(rule);
    char *nt = (char *)JMalloc(oldLen + ruleLen + 3);
    if (!nt)
    {
        JFree(rule);
        xsTypeError("out of memory");
    }
    if (oldLen)
    {
        memcpy(nt, n->text, oldLen);
        nt[oldLen] = '\n';
        memcpy(nt + oldLen + 1, rule, ruleLen);
        nt[oldLen + 1 + ruleLen] = 0;
    }
    else
    {
        memcpy(nt, rule, ruleLen);
        nt[ruleLen] = 0;
    }
    int rc = dom_set_text(b->dom, n, nt);
    JFree(nt);
    JFree(rule);
    if (rc != 0)
    {
        xsTypeError("insertRule failed");
    }
    xsResult = xsInteger(index);
}

static void xs_sheet_deleteRule(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT || !n->text)
    {
        return;
    }
    txInteger index = ((int)xsToInteger(xsArgc) > 0) ? xsToInteger(xsArg(0)) : -1;
    if (index < 0)
    {
        return;
    }
    /* Rebuild the text without the index-th `}`-terminated block. */
    size_t len = strlen(n->text);
    char *nt = (char *)JMalloc(len + 1);
    if (!nt)
    {
        xsTypeError("out of memory");
    }
    const char *s = n->text;
    size_t o = 0;
    int k = 0;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0;; i++)
    {
        int flush = 0;
        if (s[i] == '{')
        {
            depth++;
        }
        else if (s[i] == '}')
        {
            if (depth > 0)
            {
                depth--;
            }
            if (depth == 0)
            {
                flush = 1;
            }
        }
        else if (s[i] == 0)
        {
            flush = 1;
            i--; /* reprocess terminator below */
        }
        if (flush)
        {
            size_t end = (s[i + 1] == 0) ? i + 1 : i + 1; /* inclusive of '}' */
            if (k != index)
            {
                memcpy(nt + o, s + start, end - start);
                o += end - start;
            }
            k++;
            start = end;
            if (s[i + 1] == 0)
            {
                break;
            }
        }
    }
    nt[o] = 0;
    int rc = dom_set_text(b->dom, n, nt);
    JFree(nt);
    if (rc != 0)
    {
        xsTypeError("deleteRule failed");
    }
}

/* `.sheet` on elements — browsers expose it for <style>/<link>. Other
 * elements report undefined (matches the platform). */
static void xs_el_get_sheet(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    if (n && n->kind == DOM_ELEMENT && n->tag && !strcmp(n->tag, "style"))
    {
        xsSlot obj = xsNewHostInstance(state_of(b)->sheetProto);
        xs_set_host_data_site(the, "sheet", obj, n);
        xsResult = obj;
    }
    else
    {
        xsResult = xsUndefined;
    }
}

/* Element prototype: a host object carrying ONE internal host slot (the
 * "prototype holder" slot; clones inherit a fresh one for their data) +
 * the accessors/methods. Rooted via xsRemember for the machine's life. */
static void xs_build_el_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);

    st->elProto = xsNewHostObject(NULL);
    xsRemember(st->elProto); /* root for the machine's lifetime — BEFORE the
                              * build below allocates (unrooted C-field slots
                              * get swept and reused mid-build) */

    /* A2: <script> element sub-prototype (instance → scriptProto →
     * elProto). Members are resolved LIVE through elProto, so the empty
     * sub-object suffices; HTMLScriptElement.prototype is pinned to this
     * in xs_build_globals so bundler module factories' instanceof checks
     * (document.currentScript) behave like a browser. Rooted for the
     * machine's lifetime like the other engine-owned prototypes. */
    st->scriptProto = xsNewHostInstance(st->elProto);
    xsRemember(st->scriptProto);

    static const struct
    {
        const char *name;
        xsCallback get;
        xsCallback set;
    } accs[] = {
        {"tagName", xs_el_get_tagName, NULL},
        {"id", xs_el_get_id, xs_el_set_id},
        {"className", xs_el_get_className, xs_el_set_className},
        {"value", xs_el_get_value, xs_el_set_value},
        {"href", xs_el_get_href, xs_el_set_href},
        {"src", xs_el_get_src, xs_el_set_src},
        {"protocol", xs_el_get_protocol, NULL},
        {"host", xs_el_get_host, NULL},
        {"hostname", xs_el_get_hostname, NULL},
        {"port", xs_el_get_port, NULL},
        {"pathname", xs_el_get_pathname, NULL},
        {"search", xs_el_get_search, NULL},
        {"hash", xs_el_get_hash, NULL},
        {"textContent", xs_el_get_textContent, xs_el_set_textContent},
        {"isConnected", xs_el_get_isConnected, NULL},
        /* Forms-Phase 1+2: the field-life surface. checked gains the
         * write side in Phase 2 (radio-group exclusivity rides the same
         * doc-wide group the tap lane uses; writes flip the LIVE block
         * — no attribute mirror, so form.reset keeps honoring the
         * markup defaults later). selectedIndex/options are the picker
         * reporting road (reads only this phase). disabled/readOnly
         * reflect their own attributes (the REFLECTED property, not
         * fieldset inheritance). */
        {"checked", xs_el_get_checked, xs_el_set_checked},
        {"selectedIndex", xs_el_get_selectedIndex, NULL},
        {"options", xs_el_get_options, NULL},
        {"disabled", xs_el_get_disabled, xs_el_set_disabled},
        {"readOnly", xs_el_get_readOnly, xs_el_set_readOnly},
        /* Forms-Phase 6: the form's enumerated IDL mirrors (the browser
         * reflect shapes; tag-gated, aliasing encoding → enctype). */
        {"method", xs_form_get_method, xs_form_set_method},
        {"enctype", xs_form_get_enctype, xs_form_set_enctype},
        {"encoding", xs_form_get_enctype, xs_form_set_enctype},
        {"innerHTML", xs_el_get_innerHTML, xs_el_set_innerHTML},
        {"parentNode", xs_el_get_parentNode, NULL},
        {"childElementCount", xs_el_get_childElementCount, NULL},
        {"children", xs_el_get_children, NULL},
        {"attributes", xs_el_get_attributes, NULL},
        {"nodeType", xs_el_get_nodeType, NULL},
        {"nodeName", xs_el_get_tagName, NULL},
        {"ownerDocument", xs_el_get_ownerDocument, NULL},
        {"getRootNode", xs_el_get_rootNode, NULL},
        {"firstElementChild", xs_el_get_firstElementChild, NULL},
        {"nextElementSibling", xs_el_get_nextElementSibling, NULL},
        {"firstChild", xs_el_get_firstChild, NULL},
        {"lastChild", xs_el_get_lastChild, NULL},
        {"nextSibling", xs_el_get_nextSibling, NULL},
        {"previousSibling", xs_el_get_previousSibling, NULL},
        {"childNodes", xs_el_get_childNodes, NULL},
        {"data", xs_el_get_data, NULL},
        {"dataset", xs_el_get_dataset, NULL},
        {"nodeValue", xs_el_get_nodeValue, xs_el_set_nodeValue},
        {"type", xs_el_get_type, NULL},
        {"namespaceURI", xs_el_get_namespaceURI, NULL},
        {"classList", xs_el_get_classList, NULL},
        {"style", xs_el_get_style, NULL},
        {"sheet", xs_el_get_sheet, NULL},
        {"content", xs_el_get_content, NULL},
        /* Forms-Phase 3: the constraint-validation getters. */
        {"validity", xs_el_get_validity, NULL},
        {"validationMessage", xs_el_get_validationMessage, NULL},
        {"willValidate", xs_el_get_willValidate, NULL},
        /* Forms-Phase 4: the form's controls collection (tag-gated
         * inside; non-forms read undefined). */
        {"elements", xs_form_get_elements, NULL},
    };
    static const struct
    {
        const char *name;
        xsCallback fn;
        int length;
    } methods[] = {
        {"getAttribute", xs_el_getAttribute, 1},
        {"setAttribute", xs_el_setAttribute, 2},
        {"removeAttribute", xs_el_removeAttribute, 1},
        {"removeAttributeNode", xs_el_removeAttributeNode, 1},
        {"hasAttribute", xs_el_hasAttribute, 1},
        {"hasChildNodes", xs_el_hasChildNodes, 0},
        {"appendChild", xs_el_appendChild, 1},
        {"removeChild", xs_el_removeChild, 1},
        {"insertBefore", xs_el_insertBefore, 2},
        {"querySelector", xs_el_querySelector, 1},
        {"querySelectorAll", xs_el_querySelectorAll, 1},
        {"closest", xs_el_closest, 1},
        {"addEventListener", xs_el_addEventListener, 2},
        /* Forms-Phase 1: the focus/blur roads (state + non-bubbling
         * event; keyboard lane untouched). */
        {"focus", xs_el_focus, 0},
        {"blur", xs_el_blur, 0},
        /* Forms-Phase 3: the validity check/report methods. */
        {"checkValidity", xs_el_checkValidity, 0},
        {"reportValidity", xs_el_reportValidity, 0},
        /* Forms-Phase 4: the form API + the click activation road
         * (tag-gated/no-op inside; the chains ride the host hooks). */
        {"reset", xs_form_reset, 0},
        {"submit", xs_form_submit, 0},
        {"requestSubmit", xs_form_requestSubmit, 1},
        {"click", xs_el_click, 0},
        {"getElementsByTagName", xs_el_getElementsByTagName, 1},
        {"getBoundingClientRect", xs_el_getBoundingClientRect, 0},
        {"attachShadow", xs_el_attachShadow, 1},
    };

    xsSlot proto = st->elProto;
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        xsDefine(proto, xsID(accs[i].name), getter, xsIsGetter);
        if (accs[i].set)
        {
            /* getter linked on the rooted proto first — see xs_push_style */
            xsSlot setter = xsNewHostFunction(accs[i].set, 1);
            xsDefine(proto, xsID(accs[i].name), setter, xsIsSetter);
        }
    }
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        def_fn(the, proto, methods[i].name, methods[i].fn, methods[i].length);
    }

    /* CSSOM StyleSheet prototype (text-backed; see xs_el_get_sheet). */
    st->sheetProto = xsNewHostObject(NULL);
    xsRemember(st->sheetProto);
    {
        xsSlot getter = xsNewHostFunction(xs_sheet_get_cssRules, 0);
        xsDefine(st->sheetProto, xsID("cssRules"), getter, xsIsGetter);
        getter = xsNewHostFunction(xs_sheet_get_ownerNode, 0);
        xsDefine(st->sheetProto, xsID("ownerNode"), getter, xsIsGetter);
        def_fn(the, st->sheetProto, "insertRule", xs_sheet_insertRule, 2);
        def_fn(the, st->sheetProto, "deleteRule", xs_sheet_deleteRule, 1);
    }
}

/* ── timers (setTimeout / setInterval): router table + engine refs ──────── */
/* XS host functions read args via xsArg(i) and return via xsResult.
 * Registration mirrors the listener path: the callback is stored +
 * xsRemember'd (GC-pinned); JsTimer.ref = tfn[] slot index. */
static void xs_timer_setup(xsMachine *the, JsBridge *b, XsState *st,
                           JsTimerKind kind)
{
    xsSlot fn = xsArg(0);
    if (!fxIsCallable(the, &fn))
    {
        xsTypeError("timer callback must be a function");
    }
    if (b->timerCount >= JSBRIDGE_TIMERS_MAX)
    {
        xsTypeError("too many timers");
    }
    int slot = -1;
    /* Slot 0 is RESERVED: JsTimer.ref == NULL is the router's refusal
     * sentinel, and slot 0 would encode as (void*)0 — so the very first
     * timer would be rejected as "too many timers". Scan from 1. */
    for (int i = 1; i < JSBRIDGE_TIMERS_MAX; i++)
    {
        if (!st->tfnSet[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        xsTypeError("too many timers");
    }
    int delay = 0;
    if (xsToInteger(xsArgc) > 1)
    {
        delay = (int)xsToNumber(xsArg(1));
    }
    if (delay < 0)
    {
        delay = 0;
    }
    int id = jsbridge_timer_start(b, kind, (void *)(intptr_t)slot,
                                  (unsigned)delay);
    if (id == 0)
    {
        xsTypeError("too many timers");
    }
    st->tfn[slot] = fn;
    st->tfnSet[slot] = 1;
    xsRemember(st->tfn[slot]); /* pin against GC */
    xsResult = xsInteger(id);
}

static void xs_setTimeout(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    /* Boot forensics: if the page never queues ANY timer, its quiet idle is
     * a scheduling-init death, not a pump gap — the log tells them apart. */
    logger_log("[timer] setTimeout registered (argc=%d)",
               (int)xsToInteger(xsArgc));
    xs_timer_setup(the, b, st, JS_TIMER_TIMEOUT);
}

static void xs_setInterval(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_timer_setup(the, b, st, JS_TIMER_INTERVAL);
}

/* ── A2: real queueMicrotask (C-owned queue) ────────────────────────
 * Browser contract: enqueue the callback; it runs flat at the NEXT
 * microcheckpoint, never nested inside the caller. The callback is kept
 * out of the promise machinery (which this engine drives synchronously
 * when its promise is already resolved — a Promise-based shim self-
 * reenters: replayF stack resolve → queueMicrotask → … to JS-stack
 * exhaustion). Overflow raises: the queue is the device budget. */
static void xs_queueMicrotask(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    if (st->mtCount >= XS_MICRO_MAX)
    {
        xsTypeError("too many microtasks");
    }
    /* Ring tail, pinned exactly once; see the mtQ note on XsState for why
     * entries are never copied or re-pinned while the machine lives. */
    int idx = (st->mtHead + st->mtCount) % XS_MICRO_MAX;
    st->mtQ[idx] = xsArg(0);
    xsRemember(st->mtQ[idx]); /* pin against GC */
    st->mtCount++;
    /* A2 hydrate forensics: WHO queues callbacks (the machine is still
     * inside the calling JS frame here) + the queue trajectory after the
     * payload scripts. First 16 rows name callers 1:1; then every 32nd. */
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    {
        static long mtqSeq = 0;
        long seq = ++mtqSeq;
        if (seq <= 16 || (seq & 31) == 0)
        {
            char frx[110];
            int fnx = 0;
            for (txSlot *fg = the->frame;
                 fg && fnx + 1 < (int)sizeof(frx); fg = fg->next, fnx++)
            {
                char nmx[44];
                nmx[0] = '\0';
                fxBufferFrameName(the, nmx, sizeof(nmx), fg, "");
                if (!nmx[0])
                {
                    snprintf(nmx, sizeof(nmx), "?");
                }
                int wx = snprintf(frx + fnx, sizeof(frx) - (size_t)fnx,
                                  "%s%s", fnx ? "<-" : "", nmx);
                if (wx < 0 || (size_t)wx >= sizeof(frx) - (size_t)fnx)
                {
                    break;
                }
                fnx += wx;
            }
            logger_log("[mtq] seq=%ld idx=%d total=%d from=%s", seq, idx,
                       st->mtCount, frx);
        }
    }
#endif
    xsResult = xsUndefined;
}

static void xs_clear_common(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int id = (xsToInteger(xsArgc) > 0) ? (int)xsToNumber(xsArg(0)) : 0;
    int cleared = jsbridge_timer_clear(b, id);
    xsResult = xsInteger(cleared);
}

static void xs_clearTimeout(xsMachine *the)
{
    xs_clear_common(the);
}

static void xs_clearInterval(xsMachine *the)
{
    xs_clear_common(the);
}

/* ── URL constructor (WHATWG subset over the shared url parser) ──────────
 * Routers resolve navigation targets with `new URL(path, base)`; without
 * the global the identifier lookup throws during bootstrap and the tail
 * segment dies. Resolution reuses url_resolve/url_parse — the SAME parser
 * the link pipeline uses (absolute schemes, protocol-relative, `..`/`.`
 * normalization all come free).
 *
 * Engine shape: `new URL(...)` runs the host-constructor callback with
 * this === undefined (see xs_xmlhttprequest_new), so the constructor
 * parses, stores the components in malloc'd host data, and RETURNS a
 * fresh instance. Components are getters on the shared prototype reading
 * that host data (the StyleSheet-shim pattern — those getters are proven
 * inside this engine; defining eager per-instance properties from a
 * `new`-dispatched C callback is not — the callback frame's result slot
 * is interpreter-owned and was clobbered between two defines). Components
 * are read-only, matching the element URL getters; nothing on the page
 * writes them. */
typedef struct
{
    char href[512];
    char origin[176];
    char protocol[16];
    char host[144];
    char hostname[128];
    char port[12];
    char pathname[260];
    char search[260];
    char hash[132];
} XsUrlData;

static void xs_url_finalize(void *data)
{
    if (data)
    {
        JFree(data);
    }
}

static XsUrlData *xs_url_data(xsMachine *the)
{
    return (XsUrlData *)xsGetHostDataIf(xsThis);
}

/* Component getters: pure reads of the host data. A torn-off getter or
 * non-URL receiver yields "" (sheet-getter semantics). */
static void xs_url_get(xsMachine *the, size_t offset)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    XsUrlData *d = xs_url_data(the);
    xsResult = xsString(d ? (const char *)((char *)d + offset) : "");
}
#define XS_URL_GETTER(name)                                   \
    static void xs_url_get_##name(xsMachine *the)             \
    {                                                         \
        xs_url_get(the, offsetof(XsUrlData, name));           \
    }
XS_URL_GETTER(href)
XS_URL_GETTER(origin)
XS_URL_GETTER(protocol)
XS_URL_GETTER(host)
XS_URL_GETTER(hostname)
XS_URL_GETTER(port)
XS_URL_GETTER(pathname)
XS_URL_GETTER(search)
XS_URL_GETTER(hash)
#undef XS_URL_GETTER

static void xs_url_toString(xsMachine *the)
{
    XsUrlData *d = xs_url_data(the);
    xsResult = xsString(d ? d->href : "");
}

static void xs_url_constructor(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    /* Engine contract (same as xs_xmlhttprequest_new): `new URL(...)` runs
     * the callback with this === undefined — the constructor RETURNS the
     * instance via xsResult; a bare URL(...) returns one too. */
    int argc = (int)xsToInteger(xsArgc);
    if (argc < 1 || xsTypeOf(xsArg(0)) == xsUndefinedType)
    {
        xsTypeError("Invalid URL");
    }
    char *href = to_cstring(the, xsArg(0));
    if (!href)
    {
        xsTypeError("out of memory");
    }
    const char *base = b->doc->baseUrl;
    char *baseC = NULL;
    if (argc >= 2 && xsTypeOf(xsArg(1)) != xsUndefinedType &&
        xsTypeOf(xsArg(1)) != xsNullType)
    {
        baseC = to_cstring(the, xsArg(1)); /* a URL instance stringifies to href */
        if (!baseC)
        {
            JFree(href);
            xsTypeError("out of memory");
        }
        base = baseC;
    }
    char *abs = url_resolve(base, href);
    JFree(href);
    if (baseC)
    {
        JFree(baseC);
    }
    UrlParsed p;
    int ok = abs && url_parse(abs, &p) == 0;
    if (!ok)
    {
        if (abs)
        {
            pluto_free(abs);
        }
        xsTypeError("Invalid URL");
    }
    XsUrlData *d = (XsUrlData *)JMalloc(sizeof(XsUrlData));
    if (!d)
    {
        pluto_free(abs);
        xsTypeError("out of memory");
    }
    memset(d, 0, sizeof(*d));
    int defaultPort = (p.isSsl && p.port == 443) || (!p.isSsl && p.port == 80);
    snprintf(d->href, sizeof(d->href), "%s", p.normalized);
    /* WHATWG origin: opaque for about:, host[:nonDefaultPort] otherwise. */
    if (!strcmp(p.scheme, "about"))
    {
        snprintf(d->origin, sizeof(d->origin), "null");
    }
    else if (defaultPort)
    {
        snprintf(d->origin, sizeof(d->origin), "%s://%s", p.scheme, p.host);
    }
    else
    {
        snprintf(d->origin, sizeof(d->origin), "%s://%s:%d", p.scheme, p.host,
                 p.port);
    }
    snprintf(d->protocol, sizeof(d->protocol), "%s:", p.scheme);
    snprintf(d->host, sizeof(d->host), "%s", p.host);
    snprintf(d->hostname, sizeof(d->hostname), "%s", p.host);
    if (defaultPort || p.port <= 0)
    {
        d->port[0] = 0;
    }
    else
    {
        snprintf(d->port, sizeof(d->port), "%d", p.port);
    }
    snprintf(d->pathname, sizeof(d->pathname), "%s", p.path);
    snprintf(d->search, sizeof(d->search), "%s%s", p.query[0] ? "?" : "",
             p.query);
    snprintf(d->hash, sizeof(d->hash), "%s%s", p.hash[0] ? "#" : "", p.hash);
    pluto_free(abs);
    xsSlot obj = xsNewHostInstance(st->urlProto);
    xs_set_host_data_site(the, "url", obj, d);
    xsResult = obj;
}

/* ── API-P7: localStorage / sessionStorage ─────────────────────────
 * The browser's key/value stores for page scripts, translated from the
 * shared webstorage.c road (same origin rule, same caps, same
 * persistence file on every engine). `this` = the storage wrapper; its
 * host data is the WsStore* for the bound origin (local or session).
 * Browser shape: every argument coerces string, getItem misses return
 * null, length is a live getter, key(i) misses return null, setItem
 * quota-fails with the browser's QuotaExceededError road, removeItem
 * and clear are silent. Mutations log (+ save the local store's
 * section) in webstorage.c; gets log ONLY the miss (the hot-path
 * discipline mirrors the attribute road — a framework polling a value
 * every frame must not flood the log file). Destructured calls
 * (`var g = localStorage.getItem; g('k')`) throw Illegal invocation —
 * browsers throw there too (the wrapper carries the store). */
static void xs_storage_getItem(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    if (!s)
    {
        xsTypeError("Illegal invocation: storage member needs the object");
    }
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    char out[WS_VAL_MAX];
    int hit = ws_get(s, k, out, sizeof(out));
    if (!hit)
    {
        logger_log("[webstorage] get origin=%s key='%s' -> null", ws_origin(), k);
    }
    JFree(k);
    xsResult = hit ? xsString(out) : xsNull;
}

static void xs_storage_setItem(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    if (!s)
    {
        xsTypeError("Illegal invocation: storage member needs the object");
    }
    char *k = to_cstring(the, xsArg(0));
    char *v = to_cstring(the, xsArg(1));
    if (!k || !v)
    {
        JFree(k);
        JFree(v);
        xsTypeError("out of memory");
    }
    int rc = ws_set(s, k, v);
    JFree(k);
    JFree(v);
    if (!rc)
    {
        /* The browser's QuotaExceededError road (4KB here, not 5MB). */
        xsTypeError("QuotaExceededError: per-origin storage quota exceeded");
    }
}

static void xs_storage_removeItem(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    if (!s)
    {
        xsTypeError("Illegal invocation: storage member needs the object");
    }
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    ws_remove(s, k);
    JFree(k);
}

static void xs_storage_clear(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    if (!s)
    {
        xsTypeError("Illegal invocation: storage member needs the object");
    }
    ws_clear(s);
}

static void xs_storage_key(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    if (!s)
    {
        xsTypeError("Illegal invocation: storage member needs the object");
    }
    char out[WS_KEY_MAX];
    int hit = ws_key(s, (int)xsToInteger(xsArg(0)), out, sizeof(out));
    xsResult = hit ? xsString(out) : xsNull;
}

static void xs_storage_getLength(xsMachine *the)
{
    WsStore *s = (WsStore *)xsGetHostDataIf(xsThis);
    xsResult = xsInteger(s ? ws_count(s) : 0);
}

static void xs_build_globals(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);
    xsSlot glob = xsGlobal;

    /* window === globalThis */
    xsDefine(glob, xsID("window"), glob, xsDefault);
    def_val(the, glob, "self", glob);
    def_fn(the, glob, "addEventListener", xs_doc_addEventListener, 2);
    def_fn(the, glob, "removeEventListener", xs_noop, 2);
    def_fn(the, glob, "getComputedStyle", xs_getComputedStyle, 1);
    def_val(the, glob, "innerWidth", xsInteger(400));
    def_val(the, glob, "innerHeight", xsInteger(240));
    /* Inert DOM constructor globals: library code does
     * `x instanceof window.HTMLIFrameElement` (React DOM getActiveElementDeep)
     * and similar feature checks; with the constructor missing, instanceof
     * THROWS and kills the page. No browsing contexts/classes exist here, so
     * a constructor whose prototype never matches an element wrapper is
     * behaviorally exact (mirrors the QuickJS bridge list). */
    {
        static const char *const ctors[] = {
            "HTMLElement",       "HTMLInputElement",    "HTMLTextAreaElement",
            "HTMLSelectElement", "HTMLOptionElement",   "HTMLIFrameElement",
            "HTMLCanvasElement", "HTMLImageElement",    "HTMLAnchorElement",
            "HTMLScriptElement", "HTMLStyleElement",    "HTMLLinkElement",
            "HTMLHtmlElement",   "HTMLBodyElement",     "HTMLDivElement",
            "HTMLSpanElement",   "HTMLParagraphElement",
            "HTMLUListElement",  "HTMLLIElement",       "HTMLButtonElement",
            "HTMLFormElement",   "SVGElement",          "Element",
            "Node",              "Document",            "Window",
            "Event",             "CustomEvent",         "MouseEvent",
            "KeyboardEvent",     "NodeList",            "HTMLCollection",
        };
        for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++)
        {
            /* A2: HTMLScriptElement gets the script sub-prototype for
             * real instanceof parity — `document.currentScript` (the
             * synthetic element) is pushed on that prototype, so bundler
             * module factories that verify it (Next.js getAssetPrefix)
             * behave like a browser instead of throwing the "Expected
             * document.currentScript to be a <script> element"
             * InvariantError in the loader's swallow. The remaining
             * constructors stay inert (prototype never matches — mirrors
             * the QuickJS bridge for untouched feature checks). */
            xsSlot ctor = xsNewHostConstructor(
                xs_noop, 0,
                !strcmp(ctors[i], "HTMLScriptElement") ? st->scriptProto
                                                       : xsNewObject());
            def_val(the, glob, ctors[i], ctor);
        }
    }

    /* API-P4: console = the LOG lane. Every level tags its own
     * [console.*] row in the file (the receipt surface; the rendered
     * panel never changes). log/info/debug/warn/error/dir share the
     * publisher; assert adds the browser semantics (truthy silent,
     * falsey logs an error row, NEVER throws). Formatting: printf
     * substitution (%s/%d/%i/%f/%o/%O/%j/%c + %%), console-shape
     * rendering for objects/arrays (quoted nested strings, '[a, b]',
     * '{k: v}', '[Function]', depth 2). count, group-star, time and
     * trace stay ABSENT (documented boundary: an unknown method stays
     * unknown; no fake surface). */
    {
        xsSlot con = xsNewHostObject(NULL);
        xsResult = con; /* GC anchor */
        def_fn(the, con, "log", xs_console_log, 0);
        def_fn(the, con, "info", xs_console_info, 0);
        def_fn(the, con, "debug", xs_console_debug, 0);
        def_fn(the, con, "warn", xs_console_warn, 0);
        def_fn(the, con, "error", xs_console_error, 0);
        def_fn(the, con, "dir", xs_console_dir, 0);
        def_fn(the, con, "assert", xs_console_assert, 0);
        def_val(the, glob, "console", con);
    }

    /* performance — React DOM (19.x) reads performance.now() every
     * scheduler tick and performance.getEntriesByType() in the
     * commit-suspend path UNGUARDED (the
     * `0<m.imgBytes&&0===fT&&(fT=62500*(function(){if("function"==typeof
     * performance.getEntriesByType)...` IIFE dereferences the global
     * before any typeof check). With the global missing that get throws
     * "cannot coerce undefined to object" INSIDE the commit attempt,
     * React's recoverable-error machinery retries 3x and every pass
     * aborts — hydration never commits. now()/mark()/measure() cover
     * the other scheduler/profiling reads; getEntriesByType returns an
     * empty array (honest: no resource timing exists here) so React's
     * bandwidth estimate degrades to its navigator.connection fallback
     * exactly as the spec fallback chain intends. */
    {
        xsSlot perf = xsNewHostObject(NULL);
        xsResult = perf; /* GC anchor */
        def_fn(the, perf, "now", xs_performance_now, 0);
        def_fn(the, perf, "mark", xs_noop, 1);
        def_fn(the, perf, "measure", xs_noop, 0);
        def_fn(the, perf, "getEntriesByType", xs_performance_getEntriesByType, 1);
        def_fn(the, perf, "getEntries", xs_performance_getEntriesByType, 0);
        def_fn(the, perf, "clearMeasures", xs_noop, 0);
        def_fn(the, perf, "clearMarks", xs_noop, 0);
        {
            /* Device: no CLOCK_MONOTONIC in the toolchain — SDK ms clock. */
            double origin = 0.0;
#if defined(TARGET_PLAYDATE)
            PlaydateAPI *pdp = pluto_pd();
            origin = pdp ? (double)pdp->system->getCurrentTimeMilliseconds()
                         : 0.0;
#else
            struct timespec tso;
            clock_gettime(CLOCK_MONOTONIC, &tso);
            origin = (double)tso.tv_sec * 1000.0 +
                     (double)tso.tv_nsec / 1000000.0;
#endif
            def_val(the, perf, "timeOrigin", xsNumber(origin));
        }
        def_val(the, glob, "performance", perf);
    }

    /* navigator */
    {
        xsSlot nav = xsNewHostObject(NULL);
        xsResult = nav; /* GC anchor */
        def_val(the, nav, "userAgent",
                xsString("PlutoBrowser/1.0 (Playdate; XS/Moddable 9.5.0)"));
        def_val(the, nav, "appCodeName", xsString("PlutoBrowser"));
        def_val(the, nav, "appVersion", xsString("1.0"));
        def_val(the, glob, "navigator", nav);
    }

    /* API-P3: location is LIVE over the address journal — every
     * component getter re-parses the journal's current entry (seeded
     * from the doc base), so a scripted pushState/replaceState moves
     * the whole surface on the next read. The bridge-time snapshot
     * builder above is gone. assign/replace stay logged-ignored (a
     * scripted assign would be a real navigation; that's the app's
     * move, documented boundary). */
    {
        xsSlot loc = xsNewHostObject(NULL);
        xsResult = loc; /* GC anchor */
        static const struct
        {
            const char *name;
            xsCallback get;
        } locComps[] = {
            {"href", xs_location_get_href},
            {"origin", xs_location_get_origin},
            {"protocol", xs_location_get_protocol},
            {"host", xs_location_get_host},
            {"hostname", xs_location_get_hostname},
            {"port", xs_location_get_port},
            {"pathname", xs_location_get_pathname},
            {"search", xs_location_get_search},
            {"hash", xs_location_get_hash},
        };
        for (size_t lc = 0; lc < sizeof(locComps) / sizeof(locComps[0]); lc++)
        {
            xsSlot get = xsNewHostFunction(locComps[lc].get, 0);
            xsDefine(loc, xsID(locComps[lc].name), get, xsIsGetter);
        }
        def_fn(the, loc, "assign", xs_location_assign, 1);
        def_fn(the, loc, "replace", xs_location_assign, 1);
        def_val(the, glob, "location", loc);
    }

    /* API-P7: localStorage + sessionStorage — the browser's key/value
     * stores. ONE prototype carries the methods + the live length
     * getter; two instances (host data = the WsStore*) land as globals.
     * local persists per origin in the game's Data sandbox
     * (webstorage_js.txt via the storage.c file road — works on the
     * sim and the device identically); session lives in memory for the
     * browser run. Origin derivation + caps are in webstorage.c (the
     * engine-wide rule, no site anything). */
    {
        ws_bind((b->doc && b->doc->baseUrl[0]) ? b->doc->baseUrl : "");
        xsSlot sp = xsNewHostObject(NULL);
        xsResult = sp; /* GC anchor while the proto finishes */
        def_fn(the, sp, "getItem", xs_storage_getItem, 1);
        def_fn(the, sp, "setItem", xs_storage_setItem, 2);
        def_fn(the, sp, "removeItem", xs_storage_removeItem, 1);
        def_fn(the, sp, "clear", xs_storage_clear, 0);
        def_fn(the, sp, "key", xs_storage_key, 1);
        {
            xsSlot lg = xsNewHostFunction(xs_storage_getLength, 0);
            xsDefine(sp, xsID("length"), lg, xsIsGetter);
        }
        xsResult = sp; /* re-anchor before each instance alloc */
        xsSlot lo = xsNewHostInstance(sp);
        xsResult = lo;
        xsSetHostData(lo, ws_local());
        xsSlot so = xsNewHostInstance(sp);
        xsResult = so;
        xsSetHostData(so, ws_session());
        def_val(the, glob, "localStorage", lo);
        def_val(the, glob, "sessionStorage", so);
        logger_log("[webstorage] globals bound: localStorage@%p sessionStorage@%p",
                   (void *)ws_local(), (void *)ws_session());
    }

    /* URL constructor — component getters live on the prototype and read
     * the instance's host data (see xs_url_constructor for the engine-shape
     * notes). The proto is rooted in XsState like xhrProto/sheetProto and
     * carries the malloc'd-data destructor for its instances. */
    {
        st->urlProto = xsNewHostObject(xs_url_finalize);
        xsRemember(st->urlProto);
        static const struct
        {
            const char *name;
            xsCallback get;
        } comps[] = {
            {"href", xs_url_get_href},
            {"origin", xs_url_get_origin},
            {"protocol", xs_url_get_protocol},
            {"host", xs_url_get_host},
            {"hostname", xs_url_get_hostname},
            {"port", xs_url_get_port},
            {"pathname", xs_url_get_pathname},
            {"search", xs_url_get_search},
            {"hash", xs_url_get_hash},
        };
        for (size_t i = 0; i < sizeof(comps) / sizeof(comps[0]); i++)
        {
            xsSlot getter = xsNewHostFunction(comps[i].get, 0);
            xsDefine(st->urlProto, xsID(comps[i].name), getter, xsIsGetter);
        }
        def_fn(the, st->urlProto, "toString", xs_url_toString, 0);
        def_fn(the, st->urlProto, "toJSON", xs_url_toString, 0);
        xsSlot urlCtor = xsNewHostConstructor(xs_url_constructor, 1, st->urlProto);
        xsResult = urlCtor; /* GC anchor */
        def_val(the, glob, "URL", urlCtor);
    }

    /* API-P3: history = the engine's address journal. state/length are
     * live getters over the journal; pushState/replaceState write it
     * (NEVER the wire); back/forward/go move the entry cursor and latch
     * popstate/hashchange for the next pump frame. */
    {
        xsSlot history = xsNewObject();
        xsResult = history; /* GC anchor */
        {
            xsSlot getter = xsNewHostFunction(xs_history_getState, 0);
            xsDefine(history, xsID("state"), getter, xsIsGetter);
            xsSlot lengthGetter = xsNewHostFunction(xs_history_getLength, 0);
            xsDefine(history, xsID("length"), lengthGetter, xsIsGetter);
        }
        def_val(the, history, "scrollRestoration", xsString("auto"));
        def_fn(the, history, "pushState", xs_history_pushState, 3);
        def_fn(the, history, "replaceState", xs_history_replaceState, 3);
        def_fn(the, history, "go", xs_history_go, 1);
        def_fn(the, history, "back", xs_history_back, 0);
        def_fn(the, history, "forward", xs_history_forward, 0);
        def_val(the, glob, "history", history);
    }

    /* document */
    {
        xsSlot doc = xsNewHostObject(NULL);
        xsResult = doc; /* GC anchor: every allocation below can collect */
        def_fn(the, doc, "getElementById", xs_document_getElementById, 1);
        def_fn(the, doc, "createElement", xs_document_createElement, 1);
        def_fn(the, doc, "createTextNode", xs_document_createTextNode, 1);
        def_fn(the, doc, "createElementNS", xs_document_createElementNS, 2);
        def_fn(the, doc, "createComment", xs_document_createComment, 1);
        def_fn(the, doc, "querySelector", xs_document_querySelector, 1);
        def_fn(the, doc, "querySelectorAll", xs_document_querySelectorAll, 1);
        def_fn(the, doc, "getElementsByName", xs_doc_getElementsByName, 1);
        def_fn(the, doc, "getElementsByTagName", xs_doc_getElementsByTagName, 1);
        def_fn(the, doc, "write", xs_document_write, 0);
        def_fn(the, doc, "writeln", xs_document_writeln, 0);
        def_fn(the, doc, "appendChild", xs_document_appendChild, 1);
        def_fn(the, doc, "insertBefore", xs_document_insertBefore, 2);
        def_fn(the, doc, "removeChild", xs_document_removeChild, 1);
        def_fn(the, doc, "addEventListener", xs_doc_addEventListener, 2);
        def_fn(the, doc, "removeEventListener", xs_noop, 2);
        def_val(the, doc, "defaultView", glob);
        def_val(the, doc, "nodeType", xsInteger(9));
        /* C4 (input fidelity): react's ChangeEventPlugin capability-probes
         * `"oninput" in document` (browser truth) to pick the modern
         * TrackerEventPlugin path for text-input onChange dispatch.
         * Missing it, the typed 'input'/'change' envelopes fall into the
         * IE-era selectionchange/focusout branch (nU) — which never calls
         * the onChange accumulate — so controlled-input typing was
         * silently swallowed at dispatch (submitted states stayed "").
         * null is the browser-consistent value: the property exists but
         * is not an active handler until assigned. */
        def_val(the, doc, "oninput", xsNull);
        /* title as a live getter (walks the tree; see above) */
        {
            xsSlot getter = xsNewHostFunction(xs_document_getTitle, 0);
            xsDefine(doc, xsID("title"), getter, xsIsGetter);
            xsSlot titleSetter = xsNewHostFunction(xs_document_setTitle, 1);
            xsDefine(doc, xsID("title"), titleSetter, xsIsSetter);
        }
        /* A2: currentScript as a live getter — the router stamps each
         * running script's tag span (jsbridge_cur_script_stamp) and this
         * synthesizes the element (see xs_document_getCurrentScript). */
        {
            xsSlot csGetter = xsNewHostFunction(xs_document_getCurrentScript, 0);
            xsDefine(doc, xsID("currentScript"), csGetter, xsIsGetter);
        }
        /* body/head/documentElement/activeElement: LIVE getters over the
         * document-order search from #root (see xs_document_getBody*
         * above) — the init-time def_val bindings could go stale when
         * react swaps head/body on a fallback re-render. */
        {
            xsSlot gBody = xsNewHostFunction(xs_document_getBody, 0);
            xsDefine(doc, xsID("body"), gBody, xsIsGetter);
            xsSlot gActive = xsNewHostFunction(xs_document_getActiveElement, 0);
            xsDefine(doc, xsID("activeElement"), gActive, xsIsGetter);
            xsSlot gHead = xsNewHostFunction(xs_document_getHead, 0);
            xsDefine(doc, xsID("head"), gHead, xsIsGetter);
            xsSlot gHtml = xsNewHostFunction(xs_document_getDocumentElement, 0);
            xsDefine(doc, xsID("documentElement"), gHtml, xsIsGetter);
            /* R54: readyState mirrors the parked script queue (see the
             * host fn) — "loading" through the drain, "interactive"
             * after. Fork frameworks branch on it (next.js arms the RSC
             * closer via DOMContentLoaded instead of setTimeout). */
            xsSlot gReady = xsNewHostFunction(xs_document_readyState, 0);
            xsDefine(doc, xsID("readyState"), gReady, xsIsGetter);
        }
        /* R22: styleSheets — empty StyleSheetList. CSS-in-JS (react-jss/emotion)
         * walks document.styleSheets for their speedy insertRule path; with an
         * empty list the walk finds nothing and the insert throws INSIDE the
         * library's own try/catch (swallowed). Without the property, the walk
         * returns undefined/null and the caller crashes on `sheet.addRule`. */
        def_val(the, doc, "styleSheets", xsNewArray(0));
        def_val(the, glob, "document", doc);
    }

    /* alert/confirm/prompt + no-op timers (UNSUPPORTED — parity) */
    def_fn(the, glob, "alert", xs_alert, 0);
    def_fn(the, glob, "confirm", xs_noop, 0);
    def_fn(the, glob, "prompt", xs_noop, 0);
    def_fn(the, glob, "setTimeout", xs_setTimeout, 0);
    def_fn(the, glob, "requestAnimationFrame", xs_setTimeout, 0);
    def_fn(the, glob, "setInterval", xs_setInterval, 0);
    def_fn(the, glob, "clearTimeout", xs_clearTimeout, 0);
    def_fn(the, glob, "clearInterval", xs_clearInterval, 0);
    def_fn(the, glob, "queueMicrotask", xs_queueMicrotask, 1);
}

/* forward decl (defined below with the metering section) — the drain
 * re-arms this window per round; see xs_drain_jobs below. */
static txBoolean xs_meter_callback(txMachine *the, txU8 count);
extern void fzJobMeterStats(long *resets);

/* ── microtask drain (fxRunLoop parity, tool-free) ──────────────�
 * Jobs live in the reserved mxPendingJobs slot (fixed offset from
 * stackTop, addressable whenever the machine is quiescent — both drain
 * call sites are). fxRunPromiseJobs moves the chain to mxRunningJobs;
 * jobs queued *during* a job append to mxPendingJobs again, so loop
 * until the chain is empty. fxEndJob = post-drain cleanup.
 */
static void xs_drain_jobs(xsMachine *the)
{
    long rounds = 0;
#if defined(PLUTO_HYDRATE_TRACE)
    JsBridge *bP = (JsBridge *)xsGetContext(the);
    logger_log("[drain] enter pending=%d",
               mxPendingJobs.value.reference->next ? 1 : 0);
#endif
    fxEndJob(the);
    while (mxPendingJobs.value.reference->next)
    {
        fxRunPromiseJobs(the);
        fxEndJob(the);
        /* C4 livelock cap: a self-requeueing job chain (framework
         * scheduler per-slice continuations) spins this drain forever —
         * previously a sim hang at "[drain] enter pending=1" with zero
         * visibility (no meter overrun, no [jobs] heartbeat ≤256 rounds).
         * Cap the rounds like a browser's microtask budget and log the
         * surrender so the hang becomes a named, contained defer. */
        rounds++;
        if (rounds >= 1024)
        {
            char capName[96];
            capName[0] = '\0';
            fxBufferFrameName(the, capName, sizeof(capName), the->frame, "");
            logger_log("[drain] cap rounds=%ld pending-left frame=%s", rounds,
                       capName[0] ? capName : "?");
            break;
        }
        /* A2: re-arm the meter window at drain-round boundaries. A
         * statement's run and its async fallout share one window today:
         * bundler machinery (registerChunk continuations, the flight
         * client) finishes as promise jobs and pushed a 4.8KB payload
         * slot's cumulative count past its source-scaled cap (5.3M vs
         * 5.3M) → stock abort + engine reset MID-BOOT. Each drain round
         * is a task checkpoint — re-arm like the split pump's per-slice
         * re-arm. Only where metering is already armed (mount bursts
         * keep their own clock-based contract untouched). */
        if (the->meterInterval)
        {
            JsBridge *jb = (JsBridge *)xsGetContext(the);
            XsState *js = jb ? (XsState *)jb->implState : NULL;
            if (!(js && js->mount.active))
            {
                /* Same reset fxBeginMetering performs (fresh window, same
                 * callback) — done inline because xsBeginMetering the
                 * public macro is a do/while(0) host-jump bracket (each
                 * call LINKS a jump; mid-drain calls would leak chains). */
                the->meterIndex = 0;
                the->meterCount = ((txU8)XS_METER_STEP) << 16;
                the->meterInterval = ((txU8)XS_METER_STEP) << 16;
            }
        }
#ifdef PLUTO_THROW_LOG
        /* A2 forensics (harness-only): a self-rescheduling job chain
         * (framework schedulers queue a continuation per slice) spins this
         * unmetered loop forever with NO other log — a heartbeat makes the
         * hang visible; past a sanity threshold dump the frame chain so the
         * rescheduling cycle is identifiable. */
        rounds++;
        if ((rounds & 255) == 0)
        {
            char name[96];
            name[0] = '\0';
            fxBufferFrameName(the, name, sizeof(name), the->frame, "");
            logger_log("[jobs] drain round %ld pending=%d frame=%s",
                       rounds, mxPendingJobs.value.reference->next ? 1 : 0,
                       name[0] ? name : "?");
        }
        if (rounds == 1024)
        {
            txSlot *f = the->frame;
            for (int d = 0; f && d < 10; d++, f = f->next)
            {
                char nm[80];
                nm[0] = '\0';
                fxBufferFrameName(the, nm, sizeof(nm), f, "");
                logger_log("[jobs] deep frame[%d] %s", d, nm[0] ? nm : "?");
            }
        }
#else
        (void)rounds;
#endif
    }
#if defined(PLUTO_HYDRATE_TRACE)
    logger_log("[drain] done rounds=%ld", rounds);
#endif
}

/* ── metering (per-script run limit) ───────────────────
/* Per-run budget, set by each runner from the segment's admitted size.
 * Default is the flat XS_RUNLIMIT for scripts run outside the split
 * path (inline handlers, timers). The split path scales it: a 253KB
 * bootstrap tail legitimately executes orders of magnitude more ops
 * than 2M (R22 finding: the tail aborted at the flat limit with the
 * page otherwise fully rendered), while admission already bounded what
 * we accepted for the segment. A runaway script still stops at the
 * scaled bound. */
static txU8 xs_run_limit = XS_RUNLIMIT;
/* R26i: the RTOS stall detector ("Run loop stalled for more than 10
 * seconds", errorlog.txt pc/lr in fxRunID_nr) samples the game task from
 * the OS timer tick; a JS run that never returns to the SDK event loop for
 * >10s is killed even while it is making real progress. Segment execution
 * survived 61s because every segment logs a line and those SDK file writes
 * are OS-visible activity — but the final React render() runs as one
 * atomic, silent interpreter burst and was killed exactly 10s after the
 * last log line. Feed the OS from the metering hook: it fires every
 * XS_METER_STEP bytecodes of ANY script run, so gate the SDK-visible
 * logger write to ~4Hz (250ms) — matching the heartbeats the RTOS already
 * accepts (frames are 33ms; 250ms << 10s stall threshold). */
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
static unsigned long xs_last_keepalive_ms;
#define XS_KEEPALIVE_MS 250
#endif
/* R26m: on-glass progress overlay. The RTOS watchdog watches the screen —
 * the display refreshes from the framebuffer via its own DMA even while
 * the CPU is inside one long JS call, so painting fresh rows at ~4Hz
 * shows the OS (and the user) that the app is alive: a progress bar fed
 * by the split-pump segment counter plus a moving sweep that keeps
 * moving through the React mount. Direct framebuffer writes via
 * getFrame() — no graphics-context state, safe inside any engine call.
 * The counter itself is host-visible (plain int): the split pump feeds
 * it on every platform; only the drawing is device-only. */
volatile int xs_progress_hint = -1; /* current split segment, -1 = unknown */
volatile int xs_progress_total = 0; /* R26o: runnable segment count (denominator) */
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
static unsigned long xs_last_ka_log_ms;

/* 3x5 mini glyphs, 15 bits row-major MSB-first: 0-9, %, space, J, S */
static const unsigned short xs_mini_glyphs[14] = {
    0b111101101101111, 0b010110010010111, 0b111001111100111,
    0b111001111001111, 0b101101111001001, 0b111100111001111,
    0b111100111101111, 0b111001001010010, 0b111101111101111,
    0b111101111001111, 0b101001010100101, 0b000000000000000,
    0b001001001101110, 0b111100111001111,
};

static void xs_mini_text(const char *s, uint8_t *fb, int x, int y)
{
    for (; *s; s++)
    {
        unsigned g;
        if (*s >= '0' && *s <= '9')
            g = xs_mini_glyphs[*s - '0'];
        else if (*s == '%')
            g = xs_mini_glyphs[10];
        else if (*s == 'J')
            g = xs_mini_glyphs[12];
        else if (*s == 'S')
            g = xs_mini_glyphs[13];
        else
            g = xs_mini_glyphs[11];
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if ((g >> (14 - row * 3 - col)) & 1)
                    for (int dy = 0; dy < 2; dy++)
                    {
                        int px = x + col * 2;
                        uint8_t *b = fb + (y + row * 2 + dy) * LCD_ROWSIZE + (px >> 3);
                        b[0] |= 0x80 >> (px & 7);
                        b[0] |= 0x80 >> ((px + 1) & 7);
                    }
        x += 8;
    }
}

/* R26o: native loading screen uses style_font(PLUTO_FONT_BODY) for the
 * "Rendering page content... N%%" line. Declared in render/style.h (above).
 * Host harness builds don't link style.c — their shims carry a NULL stub
 * so the text repaint is skipped there (bar repaint still runs). */

static void xs_render_overlay(unsigned long now)
{
    PlaydateAPI *pd = pluto_pd();
    /* Guard for fake-API host builds (zeroed vtable): no graphics, no draw. */
    if (!pd || !pd->graphics || !pd->graphics->getFrame)
        return;
    /* R26o: paint ONLY while a split run is in flight (hint >= 0) — never
     * touch the framebuffer over a loaded page or the normal UI. */
    if (xs_progress_hint < 0)
        return;
    uint8_t *fb = pd->graphics->getFrame();
    if (!fb)
        return;
    (void)now;
    /* NATIVE loading-bar geometry (STATE_LOADING draw, main.c ~3610):
     * outline rect 24,138 352x10 — repaint it in place at ~4Hz with the
     * REAL segment progress so the bar the user watches never freezes. */
    const int bx = 24, by = 138, bw = 352, bh = 10;
    /* R30c: also erase the region ABOVE the bar where updateFrame draws the
     * "Rendering page content... N%" text and the URL line. Between bursts
     * the pipeline re-renders through STATE_LOADING and updateFrame's own
     * loaders (none of which run since frameCount does not advance — but the
     * OS blits OUR framebuffer rows as-is on the NEXT display pass, and the
     * SAME frame buffer is what updateFrame cleared+repainted BEFORE the
     * burst took control). Rows 104..137 contain the text lines that get
     * stale the moment a burst ends. Nothing outside eval windows draws in
     * this overlay: it only paints while xs_progress_hint >= 0. */
    if (pd->graphics->fillRect)
        pd->graphics->fillRect(0, 104, 400, 32, kColorWhite);
    int total = xs_progress_total > 0 ? xs_progress_total : 407;
    int p = xs_progress_hint;
    if (p > total)
        p = total;
    int pct = (p * 100) / total;
    pd->graphics->drawRect(bx, by, bw, bh, kColorBlack);
    int fillW = (bw - 2) * p / total;
    if (fillW > 0)
        pd->graphics->fillRect(bx + 1, by + 1, fillW, bh - 2, kColorBlack);
    /* live percent as mini digits inside the bar's right end: set bits draw
     * black-on-white while the fill hasn't reached them, white-on-black once
     * it has (fill behind glyphs is restored explicitly). */
    char pcts[8];
    snprintf(pcts, sizeof(pcts), "%d%%", pct);
    int filledTo = bx + 1 + fillW;
    int tx = bx + bw - 5 - 4 * (int)strlen(pcts);
    for (const char *c = pcts; *c; c++, tx += 4)
    {
        unsigned g;
        if (*c >= '0' && *c <= '9')
            g = xs_mini_glyphs[*c - '0'];
        else if (*c == '%')
            g = xs_mini_glyphs[10];
        else
            g = xs_mini_glyphs[11];
        for (int row = 0; row < 5; row++)
        {
            for (int col = 0; col < 3; col++)
            {
                int px = tx + col, py = by + 2 + row;
                uint8_t *b = fb + py * LCD_ROWSIZE + (px >> 3);
                uint8_t bit = (uint8_t)(0x80 >> (px & 7));
                if ((g >> (14 - row * 3 - col)) & 1)
                {
                    /* glyph pixel: white-on-black inside the fill,
                     * black-on-white right of it — always readable */
                    if (px < filledTo)
                        *b &= (uint8_t)~bit;
                    else
                        *b |= bit;
                }
                else if (px >= filledTo)
                {
                    *b &= (uint8_t)~bit; /* background pixel right of fill: white */
                }
            }
        }
    }
    /* sweep marker: a 2px white notch that travels the bar at ~3s/round —
     * visible on both filled and empty ground while JS grinds */
    int w = bw - 2;
    int sx = bx + 1 + (int)((now / 100) % (unsigned long)w);
    for (int y = by + 1; y < by + bh - 1; y++)
    {
        uint8_t *r0 = fb + y * LCD_ROWSIZE + (sx >> 3);
        *r0 ^= (uint8_t)(0x80 >> (sx & 7));
        uint8_t *r1 = fb + y * LCD_ROWSIZE + ((sx + 1) >> 3);
        *r1 ^= (uint8_t)(0x80 >> ((sx + 1) & 7));
    }
    /* R26o/R30c: repaint the "Rendering page content... N%" TEXT line above
     * the bar. Weak style_font: absent in host harness builds → skip. The
     * SetTimestamps call above makes this frame the displayed blit target,
     * so the fill+text below are what the user actually sees this tick. */
    if (style_font)
    {
        LCDFont *f = style_font(PLUTO_FONT_BODY);
        if (f && pd->graphics->setFont && pd->graphics->drawText &&
            pd->graphics->fillRect)
        {
            int fh = pd->graphics->getFontHeight(f);
            if (fh > 0 && fh < 40)
            {
                pd->graphics->fillRect(24, 112, 352, fh + 6, kColorWhite);
                pd->graphics->setDrawMode(kDrawModeCopy); /* chrome may have left FillWhite */
                pd->graphics->setFont(f);
                char line[48];
                snprintf(line, sizeof(line),
                         "Rendering page content... %d%%", pct);
                pd->graphics->drawText(line, strlen(line), kUTF8Encoding,
                                       24, 116);
            }
        }
    }
    /* R30c: the OS composes what WE drew into the display blit only for
     * rows we mark updated after touching raw getFrame() pixels (bar rows
     * + the digits inside them; the fill/text above go through the normal
     * draw path which tracks rows itself). Without this, some blits show a
     * partially-updated bar (the flicker at the percent text). */
    if (pd->graphics->markUpdatedRows)
        pd->graphics->markUpdatedRows(by - 1, by + bh);
}
#endif
/* R26i: ONE keepalive implementation, shared by the metering callback
 * (interpreter) and the NR fork's GC mark loop (xsMemory.c — GC is pure C
 * and runs no bytecode, so metering cannot cover it; the render-mount GC
 * stalled >10s in fxMarkInstance and was killed). SDK-visible file write
 * at ~4Hz; no-op off-device. */
/* R36 (device run 24 evidence): RTOS "stack overflow in task gameTask" at
 * seg 105/106 with NO crashlog — real C-stack exhaustion, and every engine
 * C-stack guard is inert on device (fxCStackLimit() = C_NULL). The engine
 * now calls this at every native→JS re-entry (the 4 fxRun* entries via
 * fxCheckCStackSoft) with the re-entry frame address. Log a 4KB-step
 * high-water so the device log reveals the growth pattern: continuous
 * growth inside one seg = in-seg recursion; equal-depth entries every burst
 * = healthy; creeping depth across segs/bursts = per-burst leak. Probe-only:
 * no guard behavior change this run (one behavioral change per A/B). */
#ifndef XS_STACK_TRIP_DEPTH
#define XS_STACK_TRIP_DEPTH (46UL * 1024UL)
#endif
static char* s_stack_base;          /* first reading anchor */
static long s_stack_best = -1;      /* deepest.seen so far */
static long s_stack_prev = -1;      /* last 4KB-logged reading */
static unsigned s_stack_n;
static int s_stack_overflow;

void xs_engine_stack_probe(const char* site, void* sp)
{
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
    char* s = (char*)sp;
    long depth;
    if (!s_stack_base)
        s_stack_base = s; /* first reading of the session = base */
    depth = (long)(s_stack_base - s); /* grows as C stack descends */
    if (depth < 0)
        depth = -depth; /* tolerate a low anchor */
    s_stack_n++;
    if (depth > s_stack_best)
        s_stack_best = depth;
    if ((s_stack_n & 63) == 0)
    {
        /* A2 sample row: the ≥4KB-delta gate below hides stretches of
         * shallow-but-numerous runs; naming site+n per 64 probes
         * identifies which driver re-enters the interpreter (the boot
         * drain produced >8000 unlogged "run" probes in one stretch). */
#if defined(PLUTO_HYDRATE_TRACE)
        logger_log("[stack] %s n=%u depth=%ldB best=%ldB", site, s_stack_n,
                   depth, s_stack_best);
#endif
    }
    if (depth > (long)XS_STACK_TRIP_DEPTH && !s_stack_overflow)
    {
        s_stack_overflow = 1;
        logger_log("[stack] TRIP depth=%ldB site=%s -> will ring-fence R37",
                   depth, site);
    }
    /* Log a reading whenever it moves 4KB or more since the last log —
     * catches both growth AND regressions (creep diagnosis). */
    if (s_stack_prev < 0 ||
        depth - s_stack_prev > 4096 ||
        s_stack_prev - depth > 4096)
    {
        s_stack_prev = depth;
#if defined(PLUTO_HYDRATE_TRACE)
        logger_log("[stack] %s n=%u depth=%ldB best=%ldB",
                   site, s_stack_n, depth, s_stack_best);
#endif
    }
#else
    (void)site;
    (void)sp;
#endif
}

void xs_engine_keepalive(void)
{
#if defined(TARGET_PLAYDATE)
    PlaydateAPI *pd = pluto_pd();
    unsigned long now = (unsigned long)pd->system->getCurrentTimeMilliseconds();
    if (now - xs_last_keepalive_ms >= XS_KEEPALIVE_MS)
    {
        xs_last_keepalive_ms = now;
        xs_render_overlay(now);
        void *touch = JMalloc(32);
        if (touch)
            JFree(touch);
    }
    if (now - xs_last_ka_log_ms >= 1000)
    {
        xs_last_ka_log_ms = now;
        logger_log("[js] ka t=%lu seg=%d", now, xs_progress_hint);
    }
#elif defined(TARGET_SIMULATOR)
    /* Sim: the same metering callback and GC mark loop both call this, so
     * gate with an internal call counter (no `count` in scope here), then
     * a wall clock (host ms clock differs from the SDK one). The fake-API
     * harness's zeroed vtable makes xs_render_overlay a no-op there, while
     * the app Simulator's REAL SDK displays the framebuffer — the bar is
     * visible in the sim window during the eval and the render mount. */
    {
        static unsigned long sim_call_div;
        if ((++sim_call_div & 1023) == 0)
        {
            static unsigned long sim_last_ms;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            unsigned long now = (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
            if (now - sim_last_ms >= XS_KEEPALIVE_MS)
            {
                sim_last_ms = now;
                xs_render_overlay(now);
            }
        }
    }
#endif
}

/* R30: GC instrumentation hooks (weak-declared in xsMemory.c). The ms
 * clock comes from the SDK — the REAL API in the app sim and on device,
 * the fake harness clock (0) in jstest/snaptest, so those report nothing.
 * The report lands in the pluto log; run 3's killer collect never reached
 * its report (killed mid-sweep), so the entry-side ka/sweep ticks are the
 * in-flight evidence and this line is the post-hoc split. */
unsigned long xs_engine_now_ms(void)
{
    PlaydateAPI *pd = pluto_pd();
    if (pd && pd->system && pd->system->getCurrentTimeMilliseconds)
        return (unsigned long)pd->system->getCurrentTimeMilliseconds();
    return 0;
}

void xs_engine_gc_report(unsigned long flag, unsigned long visits,
                         unsigned long markMs, unsigned long weakMs,
                         unsigned long sweepMs, unsigned long totalMs,
                         unsigned long liveSlots, unsigned long chunksSizeKB)
{
    logger_log("[gc] flag=0x%lx visits=%lu mark=%lums weak=%lums "
               "sweep=%lums total=%lums liveSlots=%lu chunks=%luKB",
               flag, visits, markMs, weakMs, sweepMs, totalMs, liveSlots,
               chunksSizeKB);
}

/* R42: engine-side report hook (weak-declared in xsRun.c). The
 * [meter-heal] line proves the dead-meter free-run theory: if it prints
 * and ka keeps beating through seg 105, runs 23-31's wedge is fixed; if
 * ka stays silent with no heal line, the wedge is a stuck native instead. */
void xs_engine_nr_report(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logger_log("%s", buf);
}

/* R32: organic slot-grow ladder report (weak hook from xsMemory.c). One
 * line per organic grow attempt: accepted rung = the step the engine will
 * take; rejected rung = the step the R31 one-bite policy would have demanded
 * and pluto_mem would have refused at this live/budget (device run-16 mode).
 * Ample-RAM mounts log exactly one ACCEPT at the R30b step (2MB sim / 1.5MB
 * device), byte-identical behavior to R31. */
void xs_ladder_report(unsigned long wantBytes, int accepted)
{
    logger_log("[mem] R32 LADDER want=%lu bytes accepted=%d "
               "live=%lu budget=%lu",
               wantBytes, accepted, pluto_mem_live(), pluto_mem_budget());
}

#ifdef PLUTO_NR_MOUNT_PAUSE
static unsigned long xs_mount_now_ms(void); /* defined below the callback */
#endif

static txBoolean xs_meter_callback(txMachine *the, txU8 count)
{
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
    xs_engine_keepalive();
#else
    (void)count;
#endif
    /* R38 probe (runs 24-27 evidence): ka (1Hz, from this callback's
     * keepalive path) STOPS for 17s at the seg-105 window while the RTOS
     * watchdog still kills gameTask — either the callback stops firing
     * (meter dead: interval==0 from an un-rearmed degrade) or it fires
     * and takes >250ms per call (GC inside the callback). 1 line per
     * 65536 callbacks names which. Cheap: one static counter. */
#ifdef TARGET_PLAYDATE
    {
        static unsigned cbProbeDiv;
        if ((++cbProbeDiv & 0xFFFF) == 0)
            logger_log("[mprobe] cb n=%u count=%lu mount=%d", cbProbeDiv,
                       (unsigned long)count,
                       (int)(((JsBridge *)xsGetContext(the)
                                  ? ((XsState *)((JsBridge *)xsGetContext(the))->implState)
                                  : NULL)
                                  ? (int)(((XsState *)((JsBridge *)xsGetContext(the))->implState)->mount.parked)
                                  : -1));
    }
#endif
#ifdef PLUTO_NR_MOUNT_PAUSE
    /* R28: inside a resumable-mount burst the callback decides between
     * continue / pause / abort. The clock is sampled on a divider: the
     * callback fires every XS_METER_STEP ops and the SDK ms clock is not
     * free; 1/64 ≈ every 512 ops is far finer than any budget matters. */
    {
        JsBridge *b = (JsBridge *)xsGetContext(the);
        XsState *st = b ? (XsState *)b->implState : NULL;
        if (st && st->mount.active)
        {
            static unsigned mountClockDiv;
            if ((++mountClockDiv & 63) == 0)
            {
                unsigned long now = xs_mount_now_ms();
                if ((long)(now - st->mount.burstEndMs) >= 0)
                {
                    unsigned long overrunMs = now - st->mount.burstEndMs;
                    if ((long)(now - st->mount.deadlineMs) >= 0 ||
                        overrunMs > (unsigned long)PLUTO_MOUNT_OVERRUN_MS)
                    {
                        /* Livelock cap: contained abort (engine reset).
                         * nrPauseAbort: the abort must survive a leaked
                         * nrRunDepth (see fxNrCheckMeteringPause) — the
                         * R28 device stall was a deferred-forever loop
                         * reaching the RTOS watchdog. The overrun arm
                         * catches it inside ONE unbroken run; the total
                         * deadline alone (120s device) never could. */
                        the->nrPauseAbort = 1;
                        the->nrPauseRequest = 1;
                        st->mount.active = 0;
                        st->mount.deadlineAbort = 1;
                        logger_log(
                            "[js] mount abort (bursts=%u overrun=%lums)",
                            st->mount.bursts, overrunMs);
                        return 0;
                    }
                    the->nrPauseRequest = 1; /* sticky: nested runs defer */
                    return 0;
                }
            }
            if (count > xs_run_limit)
            {
                /* R28 FIX: the op guard is scaled to ONE segment's source
                 * length, but a parked run's continuation executes the
                 * rest of the bundle (the React mount alone out-runs any
                 * per-segment budget — R27's abort at seg 407 was this
                 * guard clipping the mount). In mount mode the op limit
                 * is a BURST boundary, not an error: request a pause —
                 * the next burst resumes with meterIndex reset by
                 * fxBeginMetering. The mount's real bound is the burst
                 * deadline + the session livelock cap above. */
                the->nrPauseRequest = 1;
                return 0;
            }
            return 1; /* R30p: in-burst GC REVERTED — run-18 crash batch (see R30p note) */
        }
    }
#endif
    /* A2 forensics: name the overrun once before the stock abort so the
     * log shows executed meter ops vs the limit (a spin reads as a small
     * overshoot against a tiny limit; legit big-bundle work reads as a
     * large overshoot against a source-scaled limit). */
    if (count > xs_run_limit)
    {
        JsBridge *mb = (JsBridge *)xsGetContext(the);
        XsState *mst = mb ? (XsState *)mb->implState : NULL;
        char fr[160];
        int fn = 0;
        for (txSlot *ff = the->frame; ff && fn + 1 < (int)sizeof(fr);
             ff = ff->next, fn++)
        {
            char nm[40];
            nm[0] = '\0';
            fxBufferFrameName(the, nm, sizeof(nm), ff, "");
            if (!nm[0])
            {
                snprintf(nm, sizeof(nm), "?");
            }
            int w = snprintf(fr + fn, sizeof(fr) - (size_t)fn, "%s%s",
                             fn ? " <-" : "", nm);
            if (w < 0 || (size_t)w >= sizeof(fr) - (size_t)fn)
            {
                break;
            }
            fn += w;
        }
        logger_log(
            "[meter] overrun count=%lu limit=%lu pausereq=%d armed=%d "
            "runDepth=%ld%s%s frames=%s",
            (unsigned long)count, (unsigned long)xs_run_limit,
            the->nrPauseRequest, the->nrPauseArmed,
            the->nrRunDepth,
            (mst && mst->mount.active) ? " mount=active" : "",
            the->nrPauseAbort ? " abort=sticky" : "", fr);
#if !defined(TARGET_PLAYDATE) && defined(PLUTO_HYDRATE_TRACE)
        {
            void *pbt[10];
            int pn = backtrace(pbt, 10);
            logger_log("[meter] cbacktrace frames=%d -> stderr", pn);
            backtrace_symbols_fd(pbt, pn, fileno(stderr));
        }
#endif
    }
    else
    {
        /* A2: the callback fires ~every 64K meter index units; the drain's
         * per-job re-arm count shows whether jobs are being metered
         * individually at all. */
        static long row = 0;
        if ((++row & 1023) == 0)
        {
            long resets = -1;
            char ft[130];
            int ftN = 0;
            fzJobMeterStats(&resets);
            /* A2: name the running frame chain at the ramp — the abort
             * walk only saw ONE frame; the ramp rows say WHICH JS program
             * (and caller) the growing dispatches run inside. */
            ft[0] = '\0';
            for (txSlot *fg = the->frame; fg && ftN + 1 < (int)sizeof(ft);
                 fg = fg->next, ftN++)
            {
                char nm2[48];
                nm2[0] = '\0';
                fxBufferFrameName(the, nm2, sizeof(nm2), fg, "");
                if (!nm2[0])
                {
                    snprintf(nm2, sizeof(nm2), "?");
                }
                int w2 =
                    snprintf(ft + ftN, sizeof(ft) - (size_t)ftN, "%s%s",
                             ftN ? "<-" : "", nm2);
                if (w2 < 0 || (size_t)w2 >= sizeof(ft) - (size_t)ftN)
                {
                    break;
                }
                ftN += w2;
            }
#if defined(PLUTO_HYDRATE_TRACE)
            logger_log("[meter] tick count=%lu limit=%lu jobsReset=%ld "
                       "frames=%s depth=%ld",
                       (unsigned long)count, (unsigned long)xs_run_limit,
                       resets, ft, the->nrRunDepth);
#endif
        }
    }
    return count <= xs_run_limit;
}

#ifdef PLUTO_NR_MOUNT_PAUSE

/* SDK wall clock (device + sim share the Playdate API; the host harness
 * never arms the mount, so 0 is fine there). */
static unsigned long xs_mount_now_ms(void)
{
    PlaydateAPI *pd = pluto_pd();
    if (pd && pd->system && pd->system->getCurrentTimeMilliseconds)
        return (unsigned long)pd->system->getCurrentTimeMilliseconds();
    return 0;
}

extern void fxPreGrowSlots_nr(txMachine *m, txUnsigned count); /* R30c */

/* Arm ONE burst: fresh meter window + pause eligibility + budget. */
static void xs_mount_arm_burst(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st->machine;
    st->mount.burstEndMs = xs_mount_now_ms() + (unsigned long)PLUTO_MOUNT_BURST_MS;
    m->nrPauseArmed = 1;
    m->nrPauseRequest = 0;
    m->nrPauseAbort = 0;
    m->nrRunDepth = 0; /* abort longjmps skip the engine's decrements */
    /* R30c/R30d/R30f: keep the slot pool topped up BEFORE the burst
     * full-graph collect on device — run 4: 66 collects, run 5: 21; runs
     * died >10s in seg 407's resume). R30f gate: only ASK for the top-up
     * while the funnel can afford it (>= 1.5MB headroom: the 512KB the
     * pool will take + breathing room) — run 9's un-gated top-up pushed
     * the SDK heap wall into the eval ("memory full" at segs 166/195).
     * The engine re-checks free-slot count (no-op while stocked) and the
     * grow is best-effort (no abort on reserve failure).
     * R30i gate 1.5MB → 2.5MB: the FINAL segment's mount resume is the
     * single biggest allocation of the whole eval (React's client render —
     * device run 12: seg 407 parked with live 5108KB + headroom ~1.9MB,
     * so the arm-time top-up still fired and its fresh 512KB pool pushed
     * the wall into burst 3 → "memory full" → engine reset DROPPED the
     * built tree → empty render). The pool exists to spare mid-run grow
     * cycles; a mount already within 2.5MB of the wall cannot afford the
     * reservation without stealing exactly the memory its own resume is
     * about to need. Grow-on-demand still covers allocation there (one
     * bounded cycle with XS_INC_HEAP 65536).
     * R30o SHRINK 16384 → 4096: once grown, the slot pool IS RAM the mount
     * cannot use — run 16 died at headroom=328KB with the pool's dead
     * weight still parked. The R30n in-burst GC + 256KB growth steps now
     * absorb much of the churn the 1MB pool was sized for (run 4's 66
     * collects), and a smaller top-up merely falls back to grow-on-demand
     * (one bounded step cycle) — the cost is collect TIME, not memory.
     * Sim runtime is the regression canary: autotest must stay ~same and
     * collect counts must not explode. */
    if (pluto_mem_headroom_bytes() >= (2560UL * 1024UL))
    {
        fxPreGrowSlots_nr(m, 16384); /* R30p: back to runs-15/16-proven pool */
    }
    /* NOTE (R30p, device run 18): the R30o batch (pool shrink 16384→4096,
     * between-burst GC, in-burst GC, 256KB device heap steps) HARD-CRASHED
     * the device where 12+ runs on the 6.5MB-envelope + 1MB-step config
     * were stable. The batch is REVERTED here as one unit: these mechanisms
     * are engine-internal-invariant gambles that never got device-scale
     * runtime coverage (the in-burst GC fires ONLY on device; the shrink
     * and the 256KB steps change the device-only growth pattern), and they
     * shipped together — which is itself the mistake. Reintroduce ONE AT A
     * TIME, each with its own device A/B, starting never before a full
     * stable-baseline re-run on this build. */
}

/* Called INSIDE the run bracket, right after fxRunScript/fxRunID returned
 * with nrPaused: the machine registers are exactly the parked state. */
static void xs_mount_park(JsBridge *b, xsMachine *m)
{
    XsState *st = (XsState *)b->implState;
    st->mount.parked = 1;
    /* R44 (device runs 32/33 evidence): the parked React mount churns
     * ~8.8MB of parse-arena/hydration garbage over its ~197-burst life
     * (sim: peak 10.9MB → 2.1MB after the mount-finish COMPACT collect).
     * While the session is alive nrNoCompact forces every organic collect
     * down the slot-only path, so that garbage piles up as chunk
     * fragmentation and the funnel wall kills the mount at burst 13-15
     * (run 32 at the 6.5MB envelope, run 33 at 7.5MB — headroom=3KB,
     * "memory full", engine reset, EMPTY WEB PAGE). Compacting at the
     * PARK BOUNDARY is the quiescent point the R28 design already makes
     * compaction-safe: the machine registers are parked, no interpreter
     * is running, the parked region sits in the GC root scan and on the
     * sweep fixup chain (see the R28 park note in xsRun.c's mxBreak).
     * R29's crash was a COMPACT collect fired MID-BURST (inside
     * fxFindChunk while frames/jumps were half outside the interpreter) —
     * a different state; this one runs only when the run is fully parked.
     * In-run collects keep the R29 downgrade; only this boundary opts
     * back in.
     * R48 (device runs 36 AND 45 — SAME crashlog signature): the boundary
     * COMPACT is itself the trigger. Run 45: burst 19's compact completed
     * (live=6297→6297), the very next action was resume burst 20, and it
     * died at fxRunID_nr:872 (byte = *code) with mmfar=0x287b — identical
     * to run 36's burst-18 crash. Run 40 ran 27 boundary compacts clean;
     * 36 and 45 died mid-mount. The compact's chunk rebuild + code-fixup
     * pass nondeterministically leaves a CORRUPT m->code for the parked
     * C-side copies, and the next resume executes garbage. Whatever the
     * exact engine invariant is, three device A/Bs say: do not compact
     * while a mount is parked. The boundary collect therefore stays
     * NON-COMPACTING (nrNoCompact stays set — fxCollect downgrades it to
     * the full slot collect): slots are reclaimed, chunks never move, the
     * parked pointers stay valid by construction. Chunk fragmentation is
     * reclaimed at the MOUNT-FINISH compact — no parked run, idle regs,
     * proven safe in every run since R30g. Wall risk: runs 40/44/45 showed
     * the boundary compact reclaiming ~0 (live 6287→6287) — the in-burst
     * non-compact collects already carry the pressure; if the envelope is
     * still hit, the funnel aborts CONTAINEDLY (engine reset, recoverable)
     * instead of E0.
     * R51 (device runs 46+47 A/B evidence — R48 REVERTED): without the
     * boundary COMPACT the mount dies deterministically at burst 15:
     * run 47 `[xs] abort: memory full` at headroom=112KB (slot-only
     * collects free slots but never reclaim chunk fragmentation, so the
     * wall is hit every single run), and run 46's gate firing was the
     * same near-OOM state going corrupt mid-collect. The R48a "never
     * compact while parked" policy trades a rare E0 (runs 36/45: 1 in ~27
     * boundary compacts) for a guaranteed OOM (every run past burst ~14).
     * The correct trade is the ORIGINAL R30g behavior — compact at the
     * park boundary (runs 40/42: 27+ compacts clean, full mounts
     * complete) — COMBINED with the R48b validity gate + harness retry,
     * so if the rare corruption recurs it is a contained segment failure
     * with an automatic re-navigation, never an E0. The capture order
     * below (compact fixes up live m-> registers, THEN snapshot) is the
     * proven R45 ordering. */
    if (m->nrNoCompact)
    {
        unsigned long liveBefore = pluto_mem_live();
        /* R51: compact for real at the boundary — temporarily clear
         * nrNoCompact so fxCollect runs the full COMPACT path (fixes up
         * m->stack/frame/scope/code and all frame/jump code pointers
         * while the run is fully parked), then restore it so in-run
         * collects stay slot-only. */
        m->nrNoCompact = 0;
        fxCollectGarbage(m);
        m->nrNoCompact = 1;
        pluto_mem_resync_live();
        logger_log("[js] seg %d: park-boundary collect (COMPACTING, R51) "
                   "live=%luKB→%luKB (burst %u)",
                   st->mount.seg, liveBefore >> 10,
                   pluto_mem_live() >> 10, st->mount.bursts);
    }
    /* R45 (device run 35 crashlog + sim freeze evidence): capture the
     * parked pointers AFTER the compact — a compacting collect REBUILDS
     * chunks and fixes up only the machine's OWN registers (m->stack/
     * frame/scope/code are GC roots); these C-side copies captured before
     * the collect would hold pre-compact addresses, and xs_mount_reapply
     * then restores a stale pCode into the machine: the next resume burst
     * executes freed/moved bytecode (run 35: pc=fxRunID_nr XS_CODE_AWAIT
     * handler writing through a NULL generator — mmfar=0 — after bursts
     * 35-43 ran byte-identical live/liveSlots = executing garbage; the
     * 120s deadline then aborted the mount, the engine reset re-navigated,
     * and the whole cycle repeated: the looping loading screen). Order is
     * the entire fix: compact first (fixing up the live m-> registers),
     * THEN snapshot them. */
    st->mount.pStack = m->stack;
    st->mount.pScope = m->scope;
    st->mount.pFrame = m->frame;
    st->mount.pCode = m->code;
    /* R54: encode the parked code position chunk-stably. Find the block
     * whose data area contains m->code and record (block, offset). The
     * raw pCode is kept ONLY for the immediate next-burst fast path (no
     * compaction in between = pointer still valid); every resume
     * re-derives from the block chain, so a boundary compact between
     * park and resume can move the chunk without invalidating the
     * parked position. If m->code is outside every block (should not
     * happen — the interpreter only executes chunk-resident bytecode),
     * both fields are 0 and the resume falls back to the raw pointer +
     * gate validation, exactly as pre-R54. */
    {
        txBlock *blk = m->firstBlock;
        txByte *pcode = (txByte *)m->code;
        st->mount.pCodeBlock = NULL;
        st->mount.pCodeOffset = 0;
        while (blk)
        {
            txByte *lo = (txByte *)blk + sizeof(txBlock);
            txByte *hi = blk->current;
            if (pcode >= lo && pcode <= hi)
            {
                st->mount.pCodeBlock = (void *)blk;
                st->mount.pCodeOffset = (unsigned long)(pcode - lo);
                break;
            }
            blk = blk->nextBlock;
        }
        /* R50 diag: park-time fingerprint (block index + offset now). */
        {
            txBlock *b2 = m->firstBlock;
            int found = -1;
            unsigned long nblk = 0;
            while (b2)
            {
                if ((void *)b2 == st->mount.pCodeBlock)
                    found = (int)nblk;
                nblk++;
                b2 = b2->nextBlock;
            }
            logger_log("[js] seg %d: park burst %u snapshot code=%p frame=%p "
                       "in-block=%d off=%lu",
                       st->mount.seg, st->mount.bursts, (void *)st->mount.pCode,
                       (void *)st->mount.pFrame, found,
                       st->mount.pCodeOffset);
        }
    }
    /* R46c (device run 39 crashlog, pc=fxSweep xsMemory.c:2231): DO NOT
     * recapture "idle" registers here. xs_mount_park runs INSIDE the run
     * bracket right after the interpreter paused — m->stack/frame/scope/
     * code hold the PARKED state, not the idle state. The previous R46
     * recapture therefore overwrote the true idle snapshot (captured
     * post-squeeze in xs_parse_and_run_mount) with parked values; on
     * resume-completion the "restore idle regs" branch then wrote the dead
     * program's frame chain back into the machine and the mount-finish
     * fxCollectGarbage walked it (fxSweep frame walk → mmfar=0x208d).
     * The pre-mount idle capture is ALREADY chunk-stable: it happens after
     * the pre-mount squeeze (a COMPACT collect) and BEFORE nrNoCompact=1,
     * so no later compaction can invalidate it. */
}

/* R48b (device runs 36 + 45, identical crashlogs): the LAST line of
 * defense before resuming a parked run. A resume executes `byte = *code`
 * against the parked code pointer with NO further checks — if that
 * pointer is garbage, the device takes a hard fault (E0). Validate every
 * parked pointer against the machine's real memory before trusting it:
 *   - pCode must be non-NULL and lie inside a live chunk arena
 *     ([block + header, block->current) of the->firstBlock chain);
 *   - pStack/pFrame must be slots of the machine's slot stack
 *     ([stackBottom, stackTop)), pFrame above pStack (stack grows down).
 * Anything else = the parked state is corrupt: CONTAINED abort (delete
 * the session, restore idle regs, fail the segment) instead of E0.
 * Runs 36/45 crashed at the resume IMMEDIATELY after a boundary compact;
 * with R48a the boundary compact is gone, so this gate should never
 * fire — it exists so that IF corruption ever returns, the browser
 * survives and the log names it. */
static int xs_mount_parked_code_in_heap(txMachine *m, txByte *code)
{
#ifdef mxNoChunks
    return code != C_NULL;
#else
    txBlock *block;
    if (!code)
        return 0;
    block = m->firstBlock;
    while (block)
    {
        txByte *lo = (txByte *)block + sizeof(txBlock);
        txByte *hi = block->current;
        if (code >= lo && code < hi)
            return 1;
        block = block->nextBlock;
    }
    return 0;
#endif
}

/* R54: re-derive the parked code pointer from the CURRENT block chain.
 * The stored (pCodeBlock, pCodeOffset) pair survives chunk moves and
 * rebuilds; after the block is located the offset is re-applied. Returns
 * the re-derived pointer, or NULL when the block is gone (chunk freed)
 * or the offset no longer fits — the caller treats NULL as invalid
 * (contained abort via the gate, never an E0). If the block was compacted
 * into a NEW block, firstBlock still contains the same chunk bytes: the
 * walk matches the block whose current data spans the recorded offset
 * only via identity, so a moved-and-merged chunk falls back to a scan:
 * identity match first, then the offset-bounds scan. */
static txByte *xs_mount_derive_parked_code(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st->machine;
    txBlock *blk;
    if (!st->mount.pCodeBlock)
        return st->mount.pCode; /* un-encodable at park: raw fallback */
    /* Pass 1: identity — the owning block itself (no compaction since). */
    blk = m->firstBlock;
    while (blk)
    {
        if ((void *)blk == st->mount.pCodeBlock)
        {
            txByte *lo = (txByte *)blk + sizeof(txBlock);
            if (st->mount.pCodeOffset <=
                (unsigned long)(blk->current - lo))
                return lo + st->mount.pCodeOffset;
            return NULL; /* block shrank past our offset: invalid */
        }
        blk = blk->nextBlock;
    }
    /* Pass 2: the block identity vanished (chunks were rebuilt/moved).
     * The parked position lived at (block-data + offset) in a block whose
     * data spanned >= offset bytes; find any block that can still hold
     * that offset and trust the content (the gate re-validates the
     * result). This is the compaction-moved case R54 exists for. */
    blk = m->firstBlock;
    while (blk)
    {
        txByte *lo = (txByte *)blk + sizeof(txBlock);
        if (st->mount.pCodeOffset <= (unsigned long)(blk->current - lo))
            return lo + st->mount.pCodeOffset;
        blk = blk->nextBlock;
    }
    return NULL;
}

static int xs_mount_parked_valid(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st->machine;
    /* R54: re-derive BEFORE validating — pCode may be stale after a
     * boundary compact; the derived pointer is the truth. */
    st->mount.pCode = xs_mount_derive_parked_code(b);
    /* R50 diag: which sub-check failed — and the machine's view of the
     * arena, so a false positive is distinguishable from real corruption
     * in exactly one log line. */
    if (!st->mount.pStack || !st->mount.pFrame || !st->mount.pCode)
    {
        logger_log("[js] gate-r50: FAIL null (stack=%p frame=%p code=%p)",
                   (void *)st->mount.pStack, (void *)st->mount.pFrame,
                   (void *)st->mount.pCode);
        return 0;
    }
    if (st->mount.pStack < m->stackBottom ||
        st->mount.pStack >= m->stackTop)
    {
        logger_log("[js] gate-r50: FAIL stack-range (stack=%p bottom=%p "
                   "top=%p)",
                   (void *)st->mount.pStack, (void *)m->stackBottom,
                   (void *)m->stackTop);
        return 0;
    }
    if (st->mount.pFrame < m->stackBottom ||
        st->mount.pFrame >= m->stackTop)
    {
        logger_log("[js] gate-r50: FAIL frame-range (frame=%p bottom=%p "
                   "top=%p)",
                   (void *)st->mount.pFrame, (void *)m->stackBottom,
                   (void *)m->stackTop);
        return 0;
    }
    if (st->mount.pFrame < st->mount.pStack)
    {
        logger_log("[js] gate-r50: FAIL frame-below-stack (frame=%p "
                   "stack=%p)",
                   (void *)st->mount.pFrame, (void *)st->mount.pStack);
        return 0;
    }
    if (!xs_mount_parked_code_in_heap(m, st->mount.pCode))
    {
        txBlock *blk = m->firstBlock;
        unsigned long nblk = 0, nch = 0;
        while (blk)
        {
            nblk++;
            nch += (unsigned long)(blk->current - ((txByte *)blk + sizeof(txBlock)));
            blk = blk->nextBlock;
        }
        logger_log("[js] gate-r50: FAIL code-range (code=%p, %lu blocks, "
                   "%luKB chunked)",
                   (void *)st->mount.pCode, nblk, nch >> 10);
        return 0;
    }
    return 1;
}

/* After the run bracket unwound, its epilogues restored the machine
 * registers to bracket entry. Re-apply the parked state: GC during the
 * parked window scans (the->stack, stackTop] and must therefore see the
 * parked region, and the next resume burst snapshots these values. */
static void xs_mount_reapply(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st->machine;
    if (!m || !st->mount.parked)
        return;
    m->stack = st->mount.pStack;
    m->scope = st->mount.pScope;
    m->frame = st->mount.pFrame;
    m->code = st->mount.pCode;
}

static void xs_mount_clear(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    memset(&st->mount, 0, sizeof(st->mount));
    /* R29: any session end (finish/abort/invalidate/close) re-enables
     * compacting collects on the surviving machine. */
    if (st->machine)
        st->machine->nrNoCompact = 0;
}

#endif /* PLUTO_NR_MOUNT_PAUSE */

/* ── vtable entry points ─────────────────────── */

static int xs_init(JsBridge *b, const char *baseUrl)
{
    (void)baseUrl;
    XsState *st = (XsState *)JMalloc(sizeof(XsState));
    if (!st)
    {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    b->implState = st;

    xsMachine *m = xsCreateMachine(&xs_creation, "pluto-page", (void *)b);
    if (!m)
    {
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    st->machine = m;

#ifdef TARGET_PLAYDATE
    /* R13 product fix: the XS parser needs ~14B of parser memory per source
     * byte; a giant page script driven to the parser-memory ceiling starved
     * the SDK allocator (wedge/watchdog reset) before any clean OOM path
     * could run. Cap parser memory per fxParseScript so giant scripts fail
     * contained ("script too large" -> bridge catch -> engine reset) while
     * normal pages never notice. Device ceiling from R13 observation. */
    fxNRParserTotalCap = 1536UL * 1024UL; /* parser mem at 100K input ≈ 1.35MB */
#elif defined(PLUTO_NR_HOST_PARSE_CAP)
    /* R26g TEST SEAM (host harness only): emulate the device parse cap so
     * cap-dependent routing is exercisable in ASan runs. PLUTO_NR_CAP env
     * (KB) overrides the value for true-peak sweeps; PLUTO_NR_CAP_CODEGEN=1
     * additionally caps codegen (attempt-mode semantics) so the sweep
     * measures the TOTAL compile transient, not just the parse phase. */
    fxNRParserTotalCap = 1536UL * 1024UL;
    {
        const char *capEnv = getenv("PLUTO_NR_CAP");
        if (capEnv && *capEnv)
            fxNRParserTotalCap = strtoul(capEnv, NULL, 10) * 1024UL;
        if (getenv("PLUTO_NR_CAP_CODEGEN") != NULL)
            fxNRParserCodegenCapped = 1;
    }
#endif

    /* One contained bracket for ALL setup; any abort here fails init. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xs_build_el_proto(b);
        xs_build_globals(b);
        xs_build_xhr_proto(b);
        xs_build_worker_proto(b);
        xs_build_formdata_proto(b); /* Forms-Phase 5 */
        /* XMLHttpRequest constructor global (xsGlobal pattern, line 1030).
         * SW5 sim finding: xsNewHostFunction does NOT set the constructor
         * flag, so `new XMLHttpRequest()` threw XS_TYPE_ERROR ("new: not a
         * constructor") — the suite's only unguarded `new`. Use
         * xsNewHostConstructor so both call forms work (needs `the` from the
         * xsBeginHost bracket; prototype pins the instance shape). */
        xsBeginHost(m);
        {
            xsSlot xglob = xsGlobal;
            xsDefine(xglob, xsID("XMLHttpRequest"),
                     xsNewHostConstructor(xs_xmlhttprequest_new, 0, st->xhrProto), xsDefault);
            /* API-P8: the Worker contained-stub constructor (same shape —
             * the SW5 constructor-flag finding applies equally). */
            xsDefine(xglob, xsID("Worker"),
                     xsNewHostConstructor(xs_worker_new, 1, st->workerProto),
                     xsDefault);
            /* Forms-Phase 5: the FormData constructor (same shape);
             * declared arity 2 so the optional-submitter slot exists
             * (the C lane reads Arg(1)'s presence). */
            xsDefine(xglob, xsID("FormData"),
                     xsNewHostConstructor(xs_formdata_new, 2, st->fdProto),
                     xsDefault);
            logger_log("[xs] FormData defined");
            /* API-P10: the dynamic-import bridge (status-string protocol;
             * the __p5dynimport shim calls it — see jsbridge_internal.h). */
            xsDefine(xglob, xsID("__p5modRequest"),
                     xsNewHostFunction(xs_dyn_request, 2), xsDefault);
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    if (m->exitStatus != xsNormalExit)
    {
        /* aborted during setup (OOM/stack/meter): contained init failure */
        logger_log("[xs] init aborted (%s)", fxAbortString(m->exitStatus));
        xsDeleteMachine(m);
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        JFree(st);
        b->implState = NULL;
        return -1;
    }
    return 0;
}

static void xs_fail_script(JsBridge *b, int index, const char *msg)
{
    b->errs++;
    if (!b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError), "%s", msg);
    }
    logger_log("[js] script %d skipped: %s", index, msg);
}

/* ── R15: stream compile — parse straight off the disk spill handle ──────
 * The stock host API (fxParseScript(the, stream, getter, flags)) takes the
 * SOURCE GETTER as a parameter, so the engine never needs a contiguous RAM
 * copy of a giant script. The lexer reads one character per getter call and
 * keeps at most TWO positions live at once (parser->character + lookahead
 * are registers; UTF-8 continuation chars are read back-to-back — verified
 * in xsLexical.c, no re-reads of older stream positions), so a small
 * sliding window refilled from pluto_spill_read() is transparent to it.
 *
 * This removes BOTH source RAM copies from the compile-time peak (the
 * router's materialize buffer + the bridge's prefix concat buffer — ~1MB
 * for a 500KB script), which is exactly the margin between the measured
 * device compile cliff (~7.3MB live) and the fitting configuration.
 *
 * The SW5 ES5 prefix compiles as its own tiny script first — functionally
 * identical to concatenation (same global machine, same order), without
 * putting a 504KB body behind a 1.7KB buffer.
 */
typedef struct xsStreamSrc
{
    SpillFile spill;    /* disk handle (position-independent reads) */
    size_t total;       /* source length in bytes */
    size_t base;        /* stream offset of window[0] */
    size_t filled;      /* valid bytes in window */
    size_t consumed;    /* next stream position the engine will read */
    char window[1024];  /* sliding window; refills preserve lookahead */
} xsStreamSrc;

static int xs_stream_getter(void *raw)
{
    xsStreamSrc *s = (xsStreamSrc *)raw;
    /* The engine reads strictly forward: parser->character + lookahead are
     * held in registers, UTF-8 continuations are read back-to-back — the
     * stream is never re-queried for an older position. 8 trailing window
     * bytes stay put across a slide as look-behind insurance. */
    size_t pos = s->consumed;
    if (pos >= s->total)
    {
        return -1; /* C_EOF (matches stock fxStringCGetter EOF contract) */
    }
    if (pos >= s->base + s->filled)
    {
        /* Window exhausted: slide. */
        size_t keep = s->filled < 8 ? s->filled : 8;
        size_t keepOff = s->base + s->filled - keep;
        memmove(s->window, s->window + (s->filled - keep), keep);
        size_t want = sizeof(s->window) - keep;
        size_t remain = s->total - (keepOff + keep);
        if (want > remain)
        {
            want = remain;
        }
        long got = 0;
        if (want > 0)
        {
            got = pluto_spill_read(s->spill, (long)(keepOff + keep),
                                   s->window + keep, want);
            if (got < 0)
            {
                got = 0; /* I/O hiccup: treat as EOF for this refill */
            }
        }
        s->base = keepOff;
        s->filled = keep + (size_t)got;
        if (pos >= s->base + s->filled)
        {
            return -1; /* short read at EOF */
        }
    }
    s->consumed = pos + 1;
    return (unsigned char)s->window[pos - s->base];
}

/* Shared execute tail (R15): every exit path of a parse+run bracket funnels
 * here — script throws were already counted inside, engine ABORTs fail the
 * script and tear the machine down (phantom-weight re-base per SW3a). */
static void xs_exec_finish(JsBridge *b, XsState *st, xsMachine *m, int index)
{
    if (m->exitStatus != xsNormalExit)
    {
        /* Engine ABORT (OOM / C-stack / meter / unhandled rejection):
         * the page's machine state is no longer trustworthy — fail the
         * script and tear the engine down so nothing stale survives. */
        b->errs++;
        char why[64];
        snprintf(why, sizeof(why), "%s", fxAbortString(m->exitStatus));
        bridge_take_error_text(b, why);
        logger_log("[js] script %d aborted engine (%s) — engine reset",
                   index, why);
        st->machine = NULL;
        xsDeleteMachine(m);
        /* xsDeleteMachine's stock fxDeleteMachine frees whole heap chunks
         * through c_free, but funnel size-tracking can drop those entries
         * (table pressure, 8-probe cap) and grow-shrinks subtract nothing
         * when the old entry was lost — the dead machine's bytes then stay
         * as phantom "live" weight. Re-base the counter to the tracked
         * table's sum (see the SW3a notes). */
        pluto_mem_resync_live();
    }
}

/* Compile + run a tiny source (the SW5 ES5 prefix) as its own program on
 * the page machine. Returns 0 ok, 1 script failed (contained). The stock
 * HostExit bracket makes any engine abort unwind HERE, so the helper is
 * safe to call while the caller still holds its own outer brackets. */
static int xs_run_tiny(JsBridge *b, xsMachine *m, int index,
                       const char *src, size_t len, const char *what)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !src || !len)
    {
        return 0;
    }
#ifdef PLUTO_THROW_LOG
    /* Throw-trace companion: which script number throws (extern read by
     * the fork's PLUTO_THROW_LOG fxThrowMessage/fxThrow instrumentation). */
    g_pluto_throw_script = index;
#endif
    /* No b->ran++ here: the prefix is part of ONE script's run in the other
     * engines (concatenated source), so it must not inflate ran/errs vs
     * them. Failures still surface through the caller's lastError. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
            txStringCStream stream;
            stream.buffer = (txString)src;
            stream.offset = 0;
            stream.size = len;
            txScript *script =
                fxParseScript(the, &stream, fxStringCGetter, mxProgramFlag);
            if (script)
            {
                xsVar(0) = xsUndefined;
                fxRunScript(the, script, mxThis, C_NULL, C_NULL, C_NULL,
                            mxProgram.value.reference);
                xs_drain_jobs(the);
                xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
            }
        }
        xsCatch
        {
            char msg[112];
            msg[0] = '\0';
            {
                xsSlot exc = xsException;
                if (exc.kind == XS_REFERENCE_KIND)
                {
                    xsSlot m2 = xsGet(exc, xsID("message"));
                    char *s = to_cstring(the, m2);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
                if (!msg[0])
                {
                    char *s = to_cstring(the, exc);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
            }
            logger_log("[js] %s failed: %s", what, msg[0] ? msg : "exception");
            bridge_take_error_text(b, msg[0] ? msg : "exception");
            b->errs++;
        }
    }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    xs_exec_finish(b, st, m, index);
    return st->machine == NULL;
}

/* R15 admission probe: the funnel's OS-truth probe (pluto_mem.c) — one
 * implementation shared by the router's deferred retry and every engine. */
#ifdef TARGET_PLAYDATE
#define xs_probe_grantable(w) pluto_mem_probe_grantable((unsigned long)(w))
#endif

/* ── R22: stepwise (multi-frame) segment execution ──────────────�
 * Splits a segment's parse into per-frame pump slices through the NR
 * parser's pumpBudget gate (P_BUDGET_PAUSE), then finishes hoist + bind
 * + codegen and the VM run on later frames. Needed because the R20
 * budget is checked BETWEEN segments: a ~253KB single-statement tail
 * parses in ~20s at the measured device rate — past the 10s watchdog
 * no matter how the plan is sliced. One session per bridge; the heap
 * parser, its chunks, and segBuf live until the segment finishes or
 * the split is invalidated. */

static void xs_step_discard(JsBridge *b); /* host-memory teardown (below) */

/* Tear down the stepwise session (never throws; the parser owns no VM
 * state — machine aborts are handled by the machine-reset path). */
static void xs_step_finish(JsBridge *b, XsState *st, const char *why)
{
    xs_step_discard(b);
    if (why)
        logger_log("[js] stepwise: %s", why);
}

/* Machine gone (abort reset): free everything the stepwise session
 * owns. All of it is HOST memory — parser chunks (c_malloc through the
 * mem funnel), the txParser struct and segBuf (JMalloc), and any
 * un-consumed script (c_malloc) — none of it lives in the machine heap,
 * so it stays freeable after fxDeleteMachine. */
static void xs_step_discard(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st)
        return;
    if (st->step.parser)
    {
        fxTerminateParser(st->step.parser);
        JFree(st->step.parser);
        st->step.parser = NULL;
    }
    if (st->step.script)
    {
        fxDeleteScript(st->step.script); /* only reached un-consumed */
        st->step.script = NULL;
    }
    if (st->step.segBuf)
    {
        JFree(st->step.segBuf);
        st->step.segBuf = NULL;
    }
#ifdef TARGET_PLAYDATE
    if (st->step.savedParserCap)
    {
        fxNRParserTotalCap = st->step.savedParserCap;
        st->step.savedParserCap = 0;
    }
    if (st->step.savedBudget)
    {
        /* R56: the codegen+run window's raised funnel floor ends with
         * the session — the parser tree arenas are freed above, so the
         * live base returns under the standing budget. */
        pluto_mem_set_budget(st->step.savedBudget);
        st->step.savedBudget = 0;
    }
#endif
    st->step.active = 0;
    st->step.state = 0;
}

/* Invalidate both R20 (segment pacing) and R22 (stepwise) sessions
 * from the machine-gone sites (init fail, click/xhr abort, close). */
static void xs_split_invalidate(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st)
        return;
    st->split.active = 0;
    if (st->split.src)
    {
        JFree(st->split.src); /* owned source copy (see XsState.split) */
        st->split.src = NULL;
    }
    st->split.srcLen = 0;
    st->split.plan.nSpans = -1;
    xs_step_discard(b);
#ifdef PLUTO_NR_MOUNT_PAUSE
    /* R28: the mount session dies with the split. If a run is parked, its
     * machine is gone (engine reset) or about to be — never free the
     * engine-heap script pointer here, just drop the session. */
    if (st->mount.active)
    {
        logger_log("[js] split invalidate: mount session dropped "
                   "(parked=%d script=%p bursts=%u)", st->mount.parked,
                   (void *)st->mount.script, st->mount.bursts); /* R28 diag */
        if (!st->mount.parked && st->mount.script)
            fxDeleteScript(st->mount.script);
        xs_mount_clear(b);
    }
#endif
}

/* Set up a stepwise session for one large segment; returns 0 = started
 * (pump from here), 1 = failed before any engine work (caller counts a
 * failed segment). Never returns mid-run: the first parse slice is
 * driven by xs_step_pump. */
static int xs_step_begin(JsBridge *b, xsMachine *m, int index, int seg,
                         char *segBuf, long segLen, size_t segPeak)
{
    XsState *st = (XsState *)b->implState;
    txParser *p = (txParser *)JMalloc(sizeof(txParser));
    if (!p)
        return 1;
    memset(p, 0, sizeof(*p));
    st->step.parser = p;
    st->step.segBuf = segBuf; /* ownership moves here */
    st->step.segLen = segLen;
    st->step.css.buffer = (txString)segBuf;
    st->step.css.offset = 0;
    st->step.css.size = (size_t)segLen;
    st->step.seg = seg;
    st->step.frames = 0;
    st->step.errs0 = b->errs;
    st->step.restarts = 0; /* R30k: fresh segment, fresh restart budget */
    st->step.script = NULL;
    st->step.state = PLUTO_STEP_PARSE;
    st->step.pumpBudget = PLUTO_SPLIT_PUMP_BUDGET_START;
    st->step.emaStepsPerMs = 0;
    st->step.savedParserCap = 0;
    st->step.savedBudget = 0; /* R56: codegen window not raised yet */
#ifdef TARGET_PLAYDATE
    /* R21 admission contract: scope the parser cap to what the split's
     * headroom check granted for THIS segment (same as the atomic path). */
    st->step.savedParserCap = fxNRParserTotalCap;
    if ((unsigned long)segPeak > fxNRParserTotalCap)
    {
        fxNRParserTotalCap = (unsigned long)segPeak;
        logger_log("[js] bundle %d seg %d: parser cap %luKB → %zuKB "
                   "(admission-scoped, stepwise)",
                   index, seg, st->step.savedParserCap >> 10, segPeak >> 10);
    }
#else
    (void)index;
    (void)segPeak;
#endif
    st->step.active = 1;
    logger_log("[js] bundle %d seg %d: %ldB — stepwise parse (budget "
               "%lu steps/frame)",
               index, seg, segLen, st->step.pumpBudget);
    return 0;
}

/* One pump slice: drive the parser until the step budget elapses or the
 * frame chain drains. Self-tunes pumpBudget from the measured steps/ms
 * toward PLUTO_SPLIT_PUMP_TARGET_MS. Returns 1 when the parse phase is
 * DONE (drained or failed), 0 when more slices remain. */
static int xs_step_parse_slice(JsBridge *b, xsMachine *m, int index)
{
    XsState *st = (XsState *)b->implState;
    txParser *p = st->step.parser;
    int aborted = 0; /* parser-chain longjmp in this slice */
#ifdef TARGET_PLAYDATE
    PlaydateAPI *pd = pluto_pd();
    unsigned long t0 = (unsigned long)pd->system->getCurrentTimeMilliseconds();
#else
    unsigned long t0 = 0;
#endif
    /* Parse slices run inside the machine brackets like every other
     * engine entry: an OOM abort during the parse (fxNewParserChunk's
     * c_malloc fail → fxAbort → fxExitToHost) must land somewhere —
     * the HostExit bracket is that somewhere (contained; machine reset
     * handled below). Parser REPORTS (parse errors, the parser-memory
     * cap) longjmp the PARSER's own jump chain, caught by the in-slice
     * setjmp. The two chains are independent. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            p->firstJump = &st->step.jump;
            if (c_setjmp(st->step.jump.jmp_buf) == 0)
            {
                if (p->console == NULL)
                {
                    /* First slice (fxInitializeParser sets console):
                     * initialize + prime the parse. */
                    fxInitializeParser(p, m, m->parserBufferSize,
                                       m->parserTableModulo);
                    p->firstJump = &st->step.jump;
                    /* Push-only pump discipline: with pumpRunning set,
                     * every fxParserCall* inside fxParserTree only PUSHES
                     * frames — the K_PROGRAM frame stays pending and ALL
                     * steps run in the resume slices' fxParserPump, so a
                     * budget pause can only fire where every C caller is
                     * resumable. */
                    p->pumpBudget = st->step.pumpBudget;
                    p->pumpSteps = 0;
                    p->pumpPaused = 0;
                    p->pumpRunning = 1;
                    fxParserTree(p, &st->step.css, fxStringCGetter,
                                 mxProgramFlag, NULL);
                    p->pumpRunning = 0;
                }
                else
                {
                    /* Resume slice: re-arm the budget and pump.
                     * fxParserPump manages pumpRunning itself. */
                    p->pumpPaused = 0;
                    p->pumpSteps = 0;
                    p->pumpBudget = st->step.pumpBudget;
                    fxParserPump(p);
                }
            }
            else
            {
                aborted = 1;
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    if (m->exitStatus != xsNormalExit)
    {
        /* Engine abort during the parse (OOM): same contract as the run
         * slice — tear the machine down, free the host-side session. */
        xs_exec_finish(b, st, m, index);
        xs_step_discard(b);
        return 1;
    }
    if (aborted || p->errorCount > 0)
    {
        /* Parser report (parse error / cap / lexer EOF). During the
         * prime, fxParserTree's own mxTryParser swallows the longjmp —
         * errorCount is the signal; during resume pumps the longjmp
         * lands here directly. */
        char msg[112];
        msg[0] = '\0';
        if (p->errorMessage)
            snprintf(msg, sizeof(msg), "%s", p->errorMessage);
        logger_log("[js] bundle %d seg %d stepwise parse failed: %s",
                   index, st->step.seg, msg[0] ? msg : "parse error");
        bridge_take_error_text(b, msg[0] ? msg : "parse error");
        b->errs++;
        xs_step_finish(b, st, "parse aborted");
        return 1;
    }
    if (p->curFrame != NULL)
    {
        /* Paused: tune the budget from measured throughput. */
        unsigned long steps = p->pumpSteps;
#ifdef TARGET_PLAYDATE
        unsigned long dt = (unsigned long)pd->system->getCurrentTimeMilliseconds() - t0;
#else
        unsigned long dt = 1;
#endif
        if (dt == 0)
            dt = 1;
        st->step.frames++;
        if (steps > 0 && dt > 0)
        {
            unsigned long inst = steps / dt; /* steps per ms */
            if (st->step.emaStepsPerMs == 0)
                st->step.emaStepsPerMs = inst;
            else
                st->step.emaStepsPerMs =
                    (st->step.emaStepsPerMs * 3 + inst) / 4;
            unsigned long want = st->step.emaStepsPerMs *
                                 PLUTO_SPLIT_PUMP_TARGET_MS;
            if (want < PLUTO_SPLIT_PUMP_BUDGET_MIN)
                want = PLUTO_SPLIT_PUMP_BUDGET_MIN;
            if (want > PLUTO_SPLIT_PUMP_BUDGET_MAX)
                want = PLUTO_SPLIT_PUMP_BUDGET_MAX;
            st->step.pumpBudget = want;
        }
        logger_log("[js] bundle %d seg %d: parse slice %d (%lu steps, "
                   "%lums, budget %lu)",
                   index, st->step.seg, st->step.frames, steps, dt,
                   st->step.pumpBudget);
        return 0; /* more slices remain */
    }
    /* Frame chain drained — parse complete. */
    p->firstJump = NULL;
    p->pumpBudget = 0;
    p->pumpPaused = 0;
    st->step.state = PLUTO_STEP_CODE;
    logger_log("[js] bundle %d seg %d: parse complete (%d slices)",
               index, st->step.seg, st->step.frames + 1);
    return 1;
}

/* CODE slice: hoist + bind + codegen in ONE frame (the tree walk is an
 * order of magnitude cheaper than the parse and calls no resumable
 * C loop — its own internal loops stay run-to-completion). */
static void xs_step_code_slice(JsBridge *b, xsMachine *m, int index)
{
    XsState *st = (XsState *)b->implState;
    txParser *p = st->step.parser;
    int runNext = 1; /* R56: 0 = skip the run hand-off below (restart /
                      * restart-budget-exhausted) — the umbrella forbids
                      * mid-bracket returns (a raw return would leave the
                      * machine's jump chain pinned at this dead C frame) */
    p->firstJump = &st->step.jump;
#ifdef TARGET_PLAYDATE
    /* R56c (device run 16:30 trace): free engine-heap debris from the
     * earlier atomic segments before codegen — same idiom the codegen
     * bail-restart path uses (GC, then funnel resync). Log the drop. */
    {
        unsigned long liveBefore = pluto_mem_live();
        fxCollectGarbage(m);
        pluto_mem_resync_live();
        if (pluto_mem_live() < liveBefore)
            logger_log("[js] bundle %d seg %d: code head GC "
                       "%luKB → %luKB",
                       index, st->step.seg, liveBefore >> 10,
                       pluto_mem_live() >> 10);
    }
    /* R56 codegen window (device): the admission granted the PARSE cap
     * (segPeak ≈ len*19+512KB) but the CODE slice allocates the emitted
     * code buffer + binding tables in the machine heap ON TOP of the
     * still-resident parse tree — the funnel refused react-dom's 209KB
     * codegen ~1MB short of completion at the 7.68MB budget (device
     * 2026-10-18: "abort: memory full" right after "stepwise code
     * slice"). Raise the floor for the whole code→run→teardown window:
     * budget = live + allowance, clamped to what the OS heap can truly
     * serve (progressive probe — see pluto_mem_probe_grantable), restored
     * at xs_step_discard once the tree arenas are freed. savedBudget
     * keeps the raised floor alive across the run slice's later frame.
     * Sim keeps budget=0 (telemetry only) so this is a no-op there. */
    if (!st->step.savedBudget)
    {
        /* R56d (device [mem] trace 16:47 cycle): the progressive probe
         * said "grantable 2044KB" yet the funnel then served 2.75MB in
         * 128KB chunks before refusing at the 8192KB budget — single-
         * block probe allocation hits fragmentation that granular chunk
         * growth does not. The codegen grows steadily ~128KB/2.2s and
         * wanted ≥2.6MB total. Use a FIXED 3.5MB allowance: if the OS
         * pool's true wall is hit first the SDK malloc fails on its own
         * and the abort unwinds to this function's umbrella just as
         * containedly as a budget refusal (no probe, no self-deception). */
        unsigned long allowance = 3584UL * 1024UL;
        unsigned long raised;
        st->step.savedBudget = pluto_mem_budget();
        raised = pluto_mem_live() + allowance;
        if (raised < st->step.savedBudget)
            raised = st->step.savedBudget;
        pluto_mem_set_budget(raised);
        logger_log("[js] bundle %d seg %d: codegen window budget "
                   "%luKB → %luKB (allowance %luKB fixed)",
                   index, st->step.seg, st->step.savedBudget >> 10,
                   pluto_mem_budget() >> 10, allowance >> 10);
    }
#endif
    /* R57 measure: the device only revealed the codegen demand via
     * refusals; on host log the TRUE funnel-live delta across this slice
     * so the react-dom compile cost is a measured number (decision input
     * for squeezing it onto the device pool). */
#ifndef TARGET_PLAYDATE
    unsigned long liveCodeStart = pluto_mem_live();
#endif
    /* R56 umbrella: codegen allocates from the machine heap THROUGH the
     * funnel (c_malloc → pluto_mem_realloc); a budget refusal there is a
     * MACHINE abort (fxAbort → fxExitToHost), which walks the machine's
     * OWN jump chain — not the parser jump this function arms below.
     * Without a live HostExit bracket that walk derefs the->firstJump ==
     * C_NULL (device 2026-10-18: bus fault at bfar=0x5c, the jump->
     * nextJump offset, mid react-dom 209KB codegen). Arm the outermost
     * bracket exactly like the parse slices do; a machine abort lands
     * after xsEndHostExit, contained, and takes the same reset contract
     * (xs_exec_finish + discard). Body indentation inside is unchanged
     * to keep the R30k machinery diff minimal. */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
    if (c_setjmp(st->step.jump.jmp_buf) == 0)
    {
        if (p->errorCount == 0)
        {
            fxParserHoist(p);
            fxParserBind(p);
            st->step.script = fxParserCode(p);
            /* R30k: fxParserCode returns C_NULL SILENTLY when any of its
             * four SDK-heap allocations fails mid-codegen (script struct,
             * codeBuffer, symbolsBuffer, hostsBuffer → bail → C_NULL). The
             * run slice then throws the misleading "invalid script"
             * (fxRunScript's NULL guard), the segment fails, and every
             * later module requiring its exports dies — device run 12
             * revisit: seg 68's 118KB react-dom codegen bailed, 304 segs
             * dropped, page rendered as the placeholder.
             * R30k CORRECTION (device run 13): re-running fxParserCode on
             * the SAME parser is NOT safe — the first attempt already ran
             * hoist/bind and bailed mid-build, and the rebuilt script came
             * out with a broken top-level binding (react-dom's eager
             * top-level `new g(...)` inside a .split(" ").forEach surfaced
             * as "ReferenceError: get g: not initialized yet" two requires
             * later, at seg 105's require chain). The only clean retry is
             * a FRESH parse: free the parser (its chunk arenas hold the
             * first attempt's partial codegen allocations too), keep the
             * owned segBuf/css, and requeue the session at
             * PLUTO_STEP_PARSE. The collect first frees engine-heap arenas
             * so the fresh parse+codegen fits; the pump yields so the
             * restart parses across frames like a first attempt. */
            if (!st->step.script && m)
            {
                unsigned long liveBefore = pluto_mem_live();
                fxCollectGarbage(m);
                pluto_mem_resync_live();
                logger_log("[js] bundle %d seg %d: codegen alloc bail "
                           "(live=%luKB→%luKB) — restarts used=%d",
                           index, st->step.seg, liveBefore >> 10,
                           pluto_mem_live() >> 10, st->step.restarts);
                if (st->step.restarts < 2)
                {
                    txParser *np;
                    fxTerminateParser(p);
                    JFree(p);
                    st->step.parser = NULL;
                    np = (txParser *)JMalloc(sizeof(txParser));
                    if (np)
                    {
                        memset(np, 0, sizeof(*np));
                        st->step.parser = np;
                        st->step.state = PLUTO_STEP_PARSE;
                        st->step.frames = 0;
                        st->step.pumpBudget = PLUTO_SPLIT_PUMP_BUDGET_START;
                        st->step.emaStepsPerMs = 0;
                        st->step.restarts++;
                        st->step.css.offset = 0; /* stream rewinds for the fresh parse */
                        /* segBuf/css/savedParserCap/errs0 stay valid: the
                         * same source re-parses under the same cap scope. */
                        logger_log("[js] bundle %d seg %d: codegen restart "
                                   "queued — fresh parse from owned buffer",
                                   index, st->step.seg);
                        /* step.active stays 1 with state=PARSE: the pump
                         * yields and the caller re-queues the frame.
                         * R56: return through the umbrella tail instead of
                         * a raw mid-bracket return. */
                        runNext = 0;
                        goto codegen_done;
                    }
                    logger_log("[js] bundle %d seg %d: restart parser alloc "
                               "failed — failing contained",
                               index, st->step.seg);
                }
                bridge_take_error_text(b, "codegen alloc failed");
                b->errs++;
                xs_step_finish(b, st, "codegen restart budget exhausted");
                runNext = 0; /* parser was freed by discard — no tail p-touch */
                goto codegen_done;
            }
        }
        else
        {
            /* R30i: a parser error that never longjmp'd (fxReportParserError
             * records errorCount before its console-gated longjmp) must not
             * skip codegen silently — that produced the NULL script whose
             * run surfaced as "invalid script". Make it loud. */
            logger_log("[js] bundle %d seg %d: codegen SKIPPED, parser "
                       "errorCount=%d",
                       index, st->step.seg, p->errorCount);
        }
        if (runNext)
        {
            p->firstJump = NULL;
            st->step.state = PLUTO_STEP_RUN;
        }
codegen_done: ;
    }
    else
    {
        /* Contained abort (cap/codegen error): count as segment failure,
         * same accounting as the atomic path's engine-reset contract —
         * but the parser is host memory, so the machine stays alive. */
        char msg[112];
        msg[0] = '\0';
        if (p->errorMessage)
            snprintf(msg, sizeof(msg), "%s", p->errorMessage);
        logger_log("[js] bundle %d seg %d stepwise code failed: %s",
                   index, st->step.seg, msg[0] ? msg : "code error");
        bridge_take_error_text(b, msg[0] ? msg : "code error");
        b->errs++;
        /* Tail-dependency rule (R20b): a failed tail slice invalidates
         * the remaining tail slices — handled by the split pump on its
         * next pass through segErr accounting. */
        xs_step_finish(b, st, "code aborted");
        } /* c_setjmp catch ends */
        } /* R56 Host wrapper (xsBeginHost) */
        xsEndHost(m);
    } /* R56 Metering wrapper (xsBeginMetering) */
    xsEndMetering(m);
    xsEndHostExit(m);
    if (m->exitStatus != xsNormalExit)
    {
        /* R56: machine abort mid-codegen (funnel OOM refusal → fxAbort)
         * lands HERE containedly — same reset contract as the parse
         * slice's abort path (fail script, delete machine, free the
         * session host memory; also restores the R56 budget window). */
        xs_exec_finish(b, st, m, index);
        xs_step_discard(b);
        return;
    }
#ifndef TARGET_PLAYDATE
    logger_log("[js] R57 measure: codegen slice funnel delta %luKB "
               "(live %luKB → %luKB)",
               (pluto_mem_live() - liveCodeStart) >> 10,
               liveCodeStart >> 10, pluto_mem_live() >> 10);
#endif
}

/* RUN slice: bracket + run + drain in ONE frame — the R20 contract
 * (an executing bundle segment stays atomic per frame). The script was
 * produced by the CODE slice; fxRunScript consumes it (deletes on both
 * success and throw). R57 measure: host-only funnel-live delta across
 * the run (the factory EXECUTION's demand — object graphs etc.). */
static void xs_step_run_slice(JsBridge *b, xsMachine *m, int index)
{
    XsState *st = (XsState *)b->implState;
    txScript *script = st->step.script;
#ifndef TARGET_PLAYDATE
    unsigned long liveRunStart = pluto_mem_live();
#endif
    st->step.script = NULL; /* fxRunScript consumes it */
    /* Same admission-scaled runaway guard as the atomic path: the tail
     * segment legitimately runs the whole app bootstrap. */
    xs_run_limit = (txU8)XS_RUNLIMIT + (txU8)st->step.segLen * 256u;
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                xsVar(0) = xsUndefined;
                fxRunScript(the, script, mxThis, C_NULL, C_NULL, C_NULL,
                            mxProgram.value.reference);
                xs_drain_jobs(the);
                xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
            }
            xsCatch
            {
                /* Script-level throw: contained, surfaced like the other
                 * bridges render Error values (same as xs_parse_and_run). */
                char msg[112];
                msg[0] = '\0';
                {
                    xsSlot exc = xsException;
                    if (exc.kind == XS_REFERENCE_KIND)
                    {
                        xsSlot m2 = xsGet(exc, xsID("message"));
                        char *s = to_cstring(the, m2);
                        if (s)
                        {
                            snprintf(msg, sizeof(msg), "%s", s);
                            JFree(s);
                        }
                    }
                    if (!msg[0])
                    {
                        char *s = to_cstring(the, exc);
                        if (s)
                        {
                            snprintf(msg, sizeof(msg), "%s", s);
                            JFree(s);
                        }
                    }
                }
                logger_log("[js] bundle %d seg %d failed: %s", index,
                           st->step.seg, msg[0] ? msg : "exception");
                /* R26h diag: name the exact failing call via error.stack */
                if (xsException.kind == XS_REFERENCE_KIND)
                {
                    xsSlot sk = xsGet(xsException, xsID("stack"));
                    char *s = to_cstring(the, sk);
                    if (s)
                    {
                        char head[160];
                        snprintf(head, sizeof(head), "%s", s);
                        for (int ci = 0; head[ci]; ci++)
                            if (head[ci] == '\n')
                                head[ci] = '|';
                        logger_log("[js] seg %d stack: %s", st->step.seg, head);
                        JFree(s);
                    }
                }
                bridge_take_error_text(b, msg[0] ? msg : "exception");
                b->errs++;
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    xs_run_limit = (txU8)XS_RUNLIMIT; /* restore default for other runners */
    /* Same abort contract as the atomic path: tear the machine down on
     * a non-normal exitStatus (this bracket is the outermost one on the
     * pump call path — nothing else would catch it). */
    xs_exec_finish(b, st, m, index);
    /* Engine abort → machine reset path (xs_exec_finish contract); the
     * stepwise session owns only host memory, which stays freeable. */
    if (st->machine != m)
    {
        xs_step_discard(b); /* machine already deleted by xs_exec_finish */
        return;
    }
#ifndef TARGET_PLAYDATE
    logger_log("[js] R57 measure: run slice funnel delta %luKB "
               "(live %luKB → %luKB)",
               (pluto_mem_live() - liveRunStart) >> 10,
               liveRunStart >> 10, pluto_mem_live() >> 10);
#endif
    xs_step_finish(b, st, NULL);
}

/* Per-frame stepwise driver (called from xs_split_pump's budget yield
 * AND from xs_pump_split while a stepwise session is in flight).
 * Returns 1 when no stepwise work remains, 0 when the session yielded
 * to a later frame. On HOST the loop runs every slice to completion in
 * this one call (no watchdog; the R20/R22 harness contract is one
 * synchronous document_parse_ex) — the pause/resume machinery still
 * executes per slice, so the resume path is fully exercised. */
static int xs_step_pump(JsBridge *b, int index)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st ? st->machine : NULL;
    if (!st || !m || !st->step.active)
        return 1;
    int done = 0;
    do
    {
        switch (st->step.state)
        {
        case PLUTO_STEP_PARSE:
            if (!xs_step_parse_slice(b, m, index))
            {
#ifdef TARGET_PLAYDATE
                return 0; /* yielded — resume next frame */
#else
                continue; /* host: next slice immediately */
#endif
            }
            if (!st->step.active)
                return 1; /* parse failed — session already finished */
            /* fall through */
        case PLUTO_STEP_CODE:
            logger_log("[js] bundle %d seg %d: stepwise code slice", index,
                       st->step.seg); /* R26e: device trace */
            xs_step_code_slice(b, m, index);
            if (!st->step.active)
                return 1; /* aborted in codegen */
            if (st->step.state == PLUTO_STEP_PARSE)
                return 0; /* R30k: codegen restart queued — the session
                           * still owns the segment; fresh parse resumes
                           * next frame and the caller re-queues the frame. */
            /* fall through */
        case PLUTO_STEP_RUN:
            logger_log("[js] bundle %d seg %d: stepwise run slice", index,
                       st->step.seg); /* R26e: device trace */
            xs_step_run_slice(b, m, index);
            if (st->machine != m)
                return 1; /* engine abort tore the machine down */
            done = 1;
            break;
        default:
            xs_step_finish(b, st, "bad state");
            return 1;
        }
    } while (!done);
    return 1; /* segment fully executed */
}

/* R22e: stepwise-segment accounting, shared by the TWO completion sites
 * (the begin branch's inline completion and the resume completion at the
 * top of xs_split_pump). A completed stepwise segment is counted exactly
 * once, wherever its RUN slice happened to land. Uses the session's own
 * seg/errs0 snapshot (both survive xs_step_discard). */
static void xs_split_step_account(JsBridge *b, int index, int seg)
{
    XsState *st = (XsState *)b->implState;
    if (b->errs > st->step.errs0)
    {
        st->split.failSegs++;
        /* R20b/R26f tail dependency rule: a failed tail slice invalidates
         * the whole remaining tail — later slices bind identifiers the
         * earlier ones created (minified single-letter top-level names).
         * R26f: drop the tailCount>1 gate — with the single-tail FALLBACK
         * (a monster statement the slicer could not cut), nextSeg was left
         * pointing AT the dead slice and the budget loop re-considered it
         * on every subsequent frame (observed: 2 re-begins of a 253KB
         * parse under the emulated cap before the plan exhausted). */
        if (st->split.plan.tailIndex >= 0 && seg >= st->split.plan.tailIndex)
        {
            int lastSeg = st->split.plan.nSpans - 1;
            int remaining = lastSeg - seg;
            st->split.failSegs += remaining;
            st->split.nextSeg = lastSeg + 1;
            logger_log("[js] bundle %d: tail slice %d/%d failed — dropping "
                       "remaining %d tail slices (ordered dependency)",
                       index, seg - st->split.plan.tailIndex + 1,
                       st->split.plan.tailCount, remaining);
        }
    }
    else
    {
        st->split.okSegs++;
    }
}

/* R15 core: parse + run one script body inside fresh brackets (bracket
 * semantics identical to the pre-R15 inline code — Metering outside, Host
 * inside, HostExit outermost). `streamCtx`/`getter` feed fxParseScript
 * (buffer stream or the R15 disk-window stream); `execPeak` is a
 * device-only admission hint: when nonzero, the compile is skipped
 * CONTAINEDLY if the machine can't plausibly allocate its peak footprint
 * (parse + bytecode growth). */
static void xs_parse_and_run(JsBridge *b, xsMachine *m, int index,
                             void *streamCtx, txGetter getter, size_t len,
                             size_t execPeak, const char *what)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
#ifdef PLUTO_THROW_LOG
    /* Throw-trace companion: which script number throws (extern read by
     * the fork's PLUTO_THROW_LOG fxThrowMessage/fxThrow instrumentation). */
    g_pluto_throw_script = index;
#endif
#ifdef TARGET_PLAYDATE
    if (execPeak)
    {
        size_t got = xs_probe_grantable(execPeak);
        if (got < execPeak)
        {
            logger_log("[js] %s %d not admissible now (probe %zuKB < need "
                       "%zuKB, live %luKB) — skipped, will retry as memory "
                       "frees",
                       what, index, got >> 10, execPeak >> 10,
                       pluto_mem_live() >> 10);
            b->errs++;
            return;
        }
        logger_log("[js] %s %d admitted: probe %zuKB >= need %zuKB",
                   what, index, got >> 10, execPeak >> 10);
    }
#else
    (void)execPeak;
#endif
    /* Scale the runaway guard with the admitted source size (R22: the
     * 253KB bootstrap tail legitimately executes far more than the flat
     * 2M-op budget; admission already bounded the segment). */
    xs_run_limit = (txU8)XS_RUNLIMIT + (txU8)len * 256u;
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
            txScript *script =
                fxParseScript(the, streamCtx, getter, mxProgramFlag);
            if (script)
            {
                xsVar(0) = xsUndefined;
                fxRunScript(the, script, mxThis, C_NULL, C_NULL, C_NULL,
                            mxProgram.value.reference);
                xs_drain_jobs(the);
                xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
            }
        }
        xsCatch
        {
            /* Script-level throw: contained. Surface "message: text"
             * like the other bridges render Error values. */
            char msg[112];
            msg[0] = '\0';
            {
                /* mxException holds the thrown value at this point. */
                xsSlot exc = xsException;
                if (exc.kind == XS_REFERENCE_KIND)
                {
                    xsSlot m2 = xsGet(exc, xsID("message"));
                    char *s = to_cstring(the, m2);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
                if (!msg[0])
                {
                    char *s = to_cstring(the, exc);
                    if (s)
                    {
                        snprintf(msg, sizeof(msg), "%s", s);
                        JFree(s);
                    }
                }
            }
            logger_log("[js] %s %d failed: %s", what, index,
                       msg[0] ? msg : "exception");
            /* R26h diag: name the exact failing call via error.stack */
            if (xsException.kind == XS_REFERENCE_KIND)
            {
                xsSlot sk = xsGet(xsException, xsID("stack"));
                char *s = to_cstring(the, sk);
                if (s)
                {
                    char head[160];
                    snprintf(head, sizeof(head), "%s", s);
                    for (int ci = 0; head[ci]; ci++)
                        if (head[ci] == '\n')
                            head[ci] = '|';
                    logger_log("[js] seg %d stack: %s", index, head);
                    JFree(s);
                }
            }
            bridge_take_error_text(b, msg[0] ? msg : "exception");
            b->errs++;
        }
    }
        xsEndHost(m);
    }
    /* Per-script meter budget, like the other bridges' run limits. */
    xsEndMetering(m);
    xsEndHostExit(m);
    xs_run_limit = (txU8)XS_RUNLIMIT; /* restore default for other runners */
    xs_exec_finish(b, st, m, index);
}

#ifdef PLUTO_NR_MOUNT_PAUSE

/* R28: shared mount-run error surfacing — mirrors xs_parse_and_run's
 * xsCatch body (message + stack line + bridge error text). */
static void xs_mount_error_text(JsBridge *b, xsMachine *the, const char *what,
                                int index)
{
    char msg[112];
    msg[0] = '\0';
    if (xsException.kind == XS_REFERENCE_KIND)
    {
        xsSlot m2 = xsGet(xsException, xsID("message"));
        char *s = to_cstring(the, m2);
        if (s)
        {
            snprintf(msg, sizeof(msg), "%s", s);
            JFree(s);
        }
    }
    if (!msg[0])
    {
        char *s = to_cstring(the, xsException);
        if (s)
        {
            snprintf(msg, sizeof(msg), "%s", s);
            JFree(s);
        }
    }
    logger_log("[js] %s %d failed: %s", what, index,
               msg[0] ? msg : "exception");
    if (xsException.kind == XS_REFERENCE_KIND)
    {
        xsSlot sk = xsGet(xsException, xsID("stack"));
        char *s = to_cstring(the, sk);
        if (s)
        {
            char head[160];
            snprintf(head, sizeof(head), "%s", s);
            for (int ci = 0; head[ci]; ci++)
                if (head[ci] == '\n')
                    head[ci] = '|';
            logger_log("[js] seg %d stack: %s", index, head);
            JFree(s);
        }
    }
    bridge_take_error_text(b, msg[0] ? msg : "exception");
    b->errs++;
}

/* R28: run (burst 1) of a mount-eligible segment: parse + arm the meter
 * pause + fxRunScript. Returns 1 = finished inside this burst (completed
 * or contained-failed; the caller does the normal segment accounting),
 * 0 = PARKED (the session owns the segment; xs_split_pump's mount branch
 * resumes it on later frames via xs_mount_resume). */
static int xs_parse_and_run_mount(JsBridge *b, xsMachine *m, int index,
                                  txStringCStream *css, size_t len, int seg)
{
    XsState *st = (XsState *)b->implState;
    xs_run_limit = (txU8)XS_RUNLIMIT + (txU8)len * 256u;
    /* R28 diag: catches a memset over a LIVE parked session (the only way
     * the pump can reach the seg loop while a mount is still parked). */
    if (st->mount.active)
        logger_log("[js] bundle %d seg %d: R28-BUG session init over LIVE "
                   "mount (seg %d parked=%d)!!!", index, seg, st->mount.seg,
                   st->mount.parked);
    memset(&st->mount, 0, sizeof(st->mount));
    st->mount.active = 1;
    /* R30l: pre-mount squeeze — the mount resume is the single biggest
     * allocation of the whole eval (React's client render; run 12/14 died
     * "memory full" at bursts 2/3 with live ~5.1MB). Run ONE full collect
     * while compaction is still allowed (nrNoCompact is set right after)
     * to drop any parse/eval-era garbage, and log the truth so the next
     * device run shows exactly how far from the wall burst 1 starts. */
    {
        unsigned long live0 = pluto_mem_live();
        unsigned long head = pluto_mem_headroom_bytes();
        unsigned long gc0 = xs_mount_now_ms();
        fxCollectGarbage(m);
        pluto_mem_resync_live();
        /* Watchdog attribution: a compact collect on a multi-MB heap can
         * take seconds on the main thread. The logger is synchronous, so
         * a >3s collect lands on disk even if the OS kills us at 10s. */
        unsigned long gcMs = xs_mount_now_ms() - gc0;
        /* Host harnesses run without a funnel budget (headroom = SIZE_MAX
         * sentinel) — print "unbounded" instead of 9e15 KB. */
        if (head > (1024UL * 1024UL * 1024UL))
        {
            logger_log("[js] mount seg %d: pre-mount squeeze "
                       "live=%luKB->%luKB headroom=unbounded",
                       seg, live0 >> 10, pluto_mem_live() >> 10);
        }
        else
        {
            logger_log("[js] mount seg %d: pre-mount squeeze "
                       "live=%luKB->%luKB headroom=%luKB",
                       seg, live0 >> 10, pluto_mem_live() >> 10, head >> 10);
        }
        if (gcMs > 3000UL)
        {
            logger_log("[watchdog] pre-mount collect took %lums", gcMs);
        }
    }
    /* R29: session alive → the engine must not compact (fxCollect routes
     * COMPACT collects to the non-compacting slot collect while this is
     * set; the sweep's code-fixup walks cannot handle a parked run). */
    m->nrNoCompact = 1;
    st->mount.seg = seg;
    st->mount.errs0 = b->errs;
    st->mount.bursts = 1;
    st->mount.deadlineMs = xs_mount_now_ms() + (unsigned long)PLUTO_MOUNT_DEADLINE_MS;
    /* R41: name each post-squeeze phase — runs 23-30 all froze AFTER the
     * GC report printed (meter silent, ka silent for 17s), so the wedge is
     * in one of these C-only steps. The next run's log names which. */
    logger_log("[js] seg %d: post-squeeze regs capture len=%zu", seg, len);
    /* R46 (device run 36 crashlog): capture the machine's IDLE registers
     * AFTER the pre-mount squeeze. A COMPACT collect rebuilds chunk arenas
     * and fixes up the machine's OWN register roots (m->stack/scope/frame/
     * code) — capturing before the squeeze would leave iStack/iCode holding
     * pre-compact addresses; the mount-finish restore would then write
     * freed/moved chunk memory back into the machine (run 36 device crash:
     * mmfar 0x287b at fxRunID_nr:872 — `byte = *code` on a stale chunk).
     * This capture happens BEFORE nrNoCompact=1 and before any parse/run,
     * so it is the genuine idle state AND chunk-stable: no later compaction
     * runs before the mount-finish restore (the park-boundary compact fixes
     * up the machine's own registers only, which the bracket epilogues
     * overwrite with these snapshots anyway — R46c, run 39). */
    st->mount.iStack = m->stack;
    st->mount.iScope = m->scope;
    st->mount.iFrame = m->frame;
    st->mount.iCode = m->code;
    xs_mount_arm_burst(b);
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                logger_log("[js] seg %d: parse start len=%zu", seg, len);
                txScript *script =
                    fxParseScript(the, css, fxStringCGetter, mxProgramFlag);
                logger_log("[js] seg %d: parse returned", seg);
                if (script)
                {
                    st->mount.script = script;
                    xsVar(0) = xsUndefined;
                    fxRunScript(the, script, mxThis, C_NULL, C_NULL, C_NULL,
                                mxProgram.value.reference);
                    if (m->nrPaused)
                    {
                        xs_mount_park(b, m);
                        logger_log("[js] bundle %d seg %d: mount parked "
                                   "(burst 1) — resuming across frames",
                                   index, seg);
                    }
                    else
                    {
                        /* R28 FIX: completed in one burst. The completion
                         * tail (host pull + script delete) is fxRunScript's
                         * OWN — it ran on its non-parked path, so the
                         * script is already freed here and script ptr is
                         * dangling. Only drain microtasks; never touch the
                         * script again. */
                        st->mount.script = NULL;
                        xs_drain_jobs(the);
                        xs_flush_microtasks(b); /* A2: boundary flush */
                    }
                }
            }
            xsCatch
            {
                /* R28 FIX: an exception here has already crossed
                 * fxRunScript's mxCatch, which deleted the script and
                 * rethrew. fxParseScript failures (no run yet) are the
                 * only case where the bridge still owns the script — and
                 * fxParseScript self-frees on error, leaving the ptr
                 * NULL. Never free engine-heap memory here. */
                st->mount.script = NULL;
                xs_mount_error_text(b, the, "bundle seg", index);
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    m->nrPauseArmed = 0;
    m->nrRunDepth = 0;
    xs_run_limit = (txU8)XS_RUNLIMIT; /* restore default for other runners */
    xs_exec_finish(b, st, m, index); /* abort contract: engine reset */
    if (!st->machine)
    {
        /* R28 FIX: engine ABORT (fxExitToHost longjmps straight to the
         * HostExit bracket, so neither the engine's mxCatch nor the
         * bridge's xsCatch ran). The script is host-allocated memory that
         * dies with NEITHER the machine nor the session — free it here. */
        if (st->mount.deadlineAbort)
        {
            logger_log("[js] bundle %d seg %d: mount ended via deadline "
                       "abort (bursts=%u) — engine reset",
                       index, seg, st->mount.bursts);
        }
        if (st->mount.script)
        {
            fxDeleteScript(st->mount.script);
            st->mount.script = NULL;
        }
        xs_mount_clear(b);
        return 1;
    }
    if (st->mount.parked)
    {
        xs_mount_reapply(b); /* parked regs own the machine until resume */
        return 0;
    }
    xs_mount_clear(b);
    return 1;
}

/* R28: one RESUME burst of a parked mount run. Returns 1 = the run is
 * finished (completed or contained-failed; the caller accounts the
 * segment), 0 = still parked (caller yields the frame). */
static int xs_mount_resume(JsBridge *b, int index)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st->machine;
    int done = 0;
    st->mount.bursts++;
    {
        unsigned long head = pluto_mem_headroom_bytes();
        if (head > (1024UL * 1024UL * 1024UL))
        {
            logger_log("[js] bundle %d seg %d: resume burst %u (parked=%d "
                       "active=%d) live=%luKB headroom=unbounded",
                       index, st->mount.seg, st->mount.bursts,
                       st->mount.parked, st->mount.active,
                       pluto_mem_live() >> 10);
        }
        else
        {
            logger_log("[js] bundle %d seg %d: resume burst %u (parked=%d "
                       "active=%d) live=%luKB headroom=%luKB",
                       index, st->mount.seg, st->mount.bursts,
                       st->mount.parked, st->mount.active,
                       pluto_mem_live() >> 10, head >> 10);
        }
    } /* R28 diag + R30l mem */
    /* R48b: validate the parked state BEFORE trusting it. A corrupt
     * parked pointer set must NEVER reach fxRunID's resume path
     * (byte = *code with garbage code = E0 hard fault). */
    if (!xs_mount_parked_valid(b))
    {
        logger_log("[js] bundle %d seg %d: resume burst %u — parked regs "
                   "INVALID (stack=%p frame=%p code=%p) — contained abort, "
                   "no E0",
                   index, st->mount.seg, st->mount.bursts,
                   (void *)st->mount.pStack, (void *)st->mount.pFrame,
                   (void *)st->mount.pCode);
        if (st->mount.script)
        {
            fxDeleteScript(st->mount.script);
            st->mount.script = NULL;
        }
        st->mount.parked = 0;
        st->mount.active = 0;
        /* Restore the idle regs BEFORE xs_mount_clear memsets them away.
         * iStack/iFrame are slot-stack positions (never move); iCode is
         * only read by the resume path, which always re-adopts pCode — a
         * stale idle code register is never dereferenced. */
        m->stack = st->mount.iStack;
        m->scope = st->mount.iScope;
        m->frame = st->mount.iFrame;
        m->code = st->mount.iCode;
        snprintf(b->lastError, sizeof(b->lastError),
                 "mount seg %d: parked registers corrupt (contained)",
                 st->mount.seg);
        b->errs++; /* pump accounting: counts the segment as failed */
        xs_mount_clear(b);
        return 1; /* done — the split accounts the failure and moves on */
    }
    /* Parked registers BEFORE the bracket: its entry snapshots become the
     * parked values, so the epilogues restore them correctly. */
    m->stack = st->mount.pStack;
    m->scope = st->mount.pScope;
    m->frame = st->mount.pFrame;
    m->code = st->mount.pCode;
    m->nrPaused = 0;
    xs_mount_arm_burst(b);
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                /* Adopt the parked registers AFTER the bracket pushes so
                 * the interpreter resumes at the exact parked top of the
                 * slot stack. */
                m->stack = st->mount.pStack;
                m->scope = st->mount.pScope;
                m->frame = st->mount.pFrame;
                m->code = st->mount.pCode;
                m->nrResume = 1;
                fxRunID(the, C_NULL, 0);
                if (m->nrPaused)
                {
                    xs_mount_park(b, m);
                }
                else
                {
                    /* R28 FIX: completion of the RESUMED run. fxRunScript
                     * is NOT on the C stack here (the bridge re-entered
                     * fxRunID directly), so ITS completion tail never ran:
                     * do the host pull + script free here, mirroring
                     * fxRunScript's tail verbatim (same machine state). */
                    mxPushUndefined();
                    mxPull(mxHosts);
                    if (st->mount.script->symbolsBuffer)
                        fxDeleteScript(st->mount.script);
                    st->mount.script = NULL;
                    /* R28 FIX 2: clear the STALE parked flag from burst 1 —
                     * leaving it set made the end-of-function check take
                     * the re-park branch, kept the session alive, and the
                     * next segment memset over the LIVE session. */
                    st->mount.parked = 0;
                    xs_drain_jobs(the);
                    xs_flush_microtasks(b); /* A2: boundary flush */
                    done = 1;
                }
            }
            xsCatch
            {
                /* R28 FIX: contained failure of the resumed run. The
                 * throw crossed fxRunScript's mxCatch? No — the resume
                 * path re-enters fxRunID DIRECTLY (no fxRunScript), so
                 * the engine's mxCatch never ran. The bridge owns the
                 * script on THIS path. */
                if (st->mount.script)
                {
                    fxDeleteScript(st->mount.script);
                    st->mount.script = NULL;
                }
                st->mount.parked = 0;
                xs_mount_error_text(b, the, "burst-throw", index);
                done = 1;
 }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    m->nrPauseArmed = 0;
    m->nrRunDepth = 0;
    xs_exec_finish(b, st, m, index); /* abort contract: engine reset */
    if (!st->machine)
    {
        /* R28 FIX: engine ABORT on the resume path — fxExitToHost skipped
         * xsCatch, so the bridge still owns the host-allocated script.
         * The pump's machine!=m check invalidates the split next; its
         * mount branch frees the script when parked reads 0. */
        st->mount.parked = 0;
        logger_log("[js] bundle %d seg %d: resume burst %u — engine dead",
                   index, st->mount.seg, st->mount.bursts); /* R28 diag */
        return done;
    }
    if (st->mount.parked)
    {
        xs_mount_reapply(b);
        /* R30p: the between-burst collect is REVERTED (see the R30p note at
         * xs_mount_arm_burst) — it was part of the run-18 crash batch. */
        logger_log("[js] bundle %d seg %d: resume burst %u — re-parked",
                   index, st->mount.seg, st->mount.bursts); /* R28 diag */
    }
    else
    {
        /* R28 FIX: run finished (completed or contained-throw). The
         * bracket epilogues restored their ENTRY snapshots — the PARKED
         * registers — leaving the->frame walking dead program frames
         * whose FUNCTION slots reference the just-freed codeBuffer (GC
         * code-fixup corruption). Restore the pre-mount idle registers. */
        m->stack = st->mount.iStack;
        m->scope = st->mount.iScope;
        m->frame = st->mount.iFrame;
        m->code = st->mount.iCode;
        st->mount.active = 0;
        m->nrNoCompact = 0; /* R29: session over — compaction safe again */
        logger_log("[js] bundle %d seg %d: mount finished after %u bursts",
                   index, st->mount.seg, st->mount.bursts);
        /* R30g: reclaim the mount's transients NOW. The React render mount
         * leaves the funnel at ~6.3MB (device run 10: live 6286KB right after
         * this point) and every remaining tail segment then reads
         * "needs ~512KB, headroom ~373KB — skipped" — the whole remaining
         * 85-segment tail (module resolution + bootstrap) dies on a wall of
         * skips → page renders as the placeholder → snapshot refused →
         * revisit fails. The live graph itself is smaller than that: the
         * mount's piece-wise PARSE chunks + hydration transients are
         * garbage, but slot-only collects cannot free them (they live in
         * chunk arenas). A COMPACT collect at session end (compaction re-
         * enabled on the line above) rebuilds those arenas and returns
         * real RAM; the funnel resync re-bases the counter to the tracked
         * truth. Safe here: no parked run, idle registers, GC root state
         * is exactly what a normal collect expects. */
        {
            unsigned long liveBefore = pluto_mem_live();
            fxCollectGarbage(m);
            pluto_mem_resync_live();
            logger_log("[js] bundle %d seg %d: mount-finish compact collect "
                       "live=%luKB→%luKB",
                       index, st->mount.seg, liveBefore >> 10,
                       pluto_mem_live() >> 10);
        }
    }
    return done;
}

#endif /* PLUTO_NR_MOUNT_PAUSE */

/* R18 inline-source split (defined below): plan + split-run a bundler
 * monolith whose source is already materialized in RAM. */
static int xs_try_bundler_split_src(JsBridge *b, const char *src, size_t len,
                                    int index);

/* ── C4: engine-owned `fetch` shim ────────────────────────────────────────
 * The contact page's send handler calls
 *   fetch(url, {method:'POST', headers, body, signal:AbortSignal.timeout()})
 * The engine has native Promise/async-await + a working XMLHttpRequest
 * (router HTTP), so the shim is a tiny SCRIPT (one xs_run_tiny run, before
 * the page's first script) rather than C promise plumbing. Response shape
 * covers the handler's usage: .ok (200–299), .status, .text()/.json()
 * promise-returning. The signal object's timeoutms drives a setTimeout
 * reject (the real spec abort machinery is out of scope here). */
static const char g_fetchShimSrc[] =
    "/* pluto fetch shim (engine-owned) */\n"
    "(function (g) {\n"
    "  if (typeof g.fetch === 'function') { return; }\n"
    "  if (typeof g.AbortSignal !== 'function') {\n"
    "    var AS = function AbortSignal() {};\n"
    "    AS.timeout = function (ms) {\n"
    "      var n = 0;\n"
    "      if (typeof ms === 'number' && ms === ms && ms > 0 &&\n"
    "          ms <= 1200000) {\n"
    "        n = Math.floor(ms);\n"
    "      }\n"
    "      return { aborted: false, reason: undefined, onabort: null,\n"
    "        throwIfAborted: function () { if (this.aborted) {\n"
    "            throw this.reason;\n"
    "        } },\n"
    "        addEventListener: function () {},\n"
    "        removeEventListener: function () {},\n"
    "        dispatchEvent: function () { return false; },\n"
    "        plutoTimeoutMs: n };\n"
    "    };\n"
    "    g.AbortSignal = AS;\n"
    "  }\n"
    "  g.fetch = function (url, opts) {\n"
    "    opts = opts || {};\n"
    "    var ms = 0;\n"
    "    var sig = opts.signal;\n"
    "    if (sig && typeof sig.plutoTimeoutMs === 'number') {\n"
    "      ms = sig.plutoTimeoutMs;\n"
    "    }\n"
    "    return new g.Promise(function (resolve, reject) {\n"
    "      var xhr = new XMLHttpRequest();\n"
    "      var timer = 0;\n"
    "      try {\n"
    "        xhr.open((opts.method || 'GET') + '', url + '');\n"
    "      } catch (e) { reject(e); return; }\n"
    "      var hs = opts.headers;\n"
    "      if (hs && typeof hs === 'object') {\n"
    "        for (var hk in hs) {\n"
    "          try { xhr.setRequestHeader(String(hk), String(hs[hk])); }\n"
    "          catch (hfe) {}\n"
    "        }\n"
    "      }\n"
    "      if (ms > 0) {\n"
    "        timer = setTimeout(function () {\n"
    "          timer = 0;\n"
    "          reject(new Error('TimeoutError: fetch aborted (pluto)'));\n"
    "        }, ms);\n"
    "      }\n"
    "      xhr.onload = function () {\n"
    "        if (timer) { clearTimeout(timer); timer = 0; }\n"
    "        var st = this.status || 0;\n"
    "        var tx = this.responseText || '';\n"
    "        resolve({ ok: (st >= 200 && st < 300), status: st,\n"
    "          url: String(url), redirected: false,\n"
    "          text: function () { return g.Promise.resolve(tx); },\n"
    "          json: function () {\n"
    "            try { return g.Promise.resolve(JSON.parse(tx)); }\n"
    "            catch (e) { return g.Promise.reject(e); }\n"
    "          } });\n"
    "      };\n"
    "      xhr.onerror = function () {\n"
    "        if (timer) { clearTimeout(timer); timer = 0; }\n"
    "        reject(new Error('Fetch failed'));\n"
    "      };\n"
    "      var body = opts.body;\n"
    "      if (body === undefined || body === null) { xhr.send(); }\n"
    "      else { xhr.send(String(body)); }\n"
    "    });\n"
    "  };\n"
    "})(this);\n";

static void xs_run_fetch_shim(JsBridge *b, XsState *st, int index)
{
    /* Latch BEFORE the run: a shim parse failure must not loop every
     * script (pages that never call fetch would pay forever). The
     * failure surfaces in the log + lastError from xs_run_tiny's catch. */
    st->fetchShimDone = 1;
    logger_log("[js] fetch shim: injecting (before script %d)", index);
    int rc = xs_run_tiny(b, st->machine, index, g_fetchShimSrc,
                         sizeof(g_fetchShimSrc) - 1, "fetch shim");
    logger_log("[js] fetch shim: %s", rc ? "FAILED (engine reset)"
                                         : "defined");
}

static void xs_run_script(JsBridge *b, const char *src, size_t len,
                          int index)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    if (st->split.active || st->step.active)
    {
        /* R22 re-entry guard: a time-sliced split (or its stepwise
         * segment) is mid-flight — a new top-level script cannot run on
         * the machine yet (its parse and run bracket a resumable parser
         * session). Skip like the admission path does; the split resumes
         * on the next frame and a later rewalk re-offers the script. */
        logger_log("[js] script %d skipped: bundle split still running",
                   index);
        return;
    }
    /* C4: the engine-owned fetch/AbortSignal shim runs ONCE, before the
     * first page script (send handlers call fetch at tap time, but
     * defining the globals up front keeps every page path uniform). */
    if (!st->fetchShimDone)
    {
        xs_run_fetch_shim(b, st, index);
    }
    /* A2 forensics: identify every plain-path slot body (the split path
     * already announces itself with the bundler-monolith row). */
    {
        size_t head = len < 96 ? len : 96;
        while (head > 0 && (src[head - 1] == '\n' || src[head - 1] == '\r' ||
                            src[head - 1] == ' '))
        {
            head--;
        }
        logger_log("[js] script %d plain-run %zuB: %.*s%s", index, len,
                   (int)head, src, len > head ? "…" : "");
    }
    if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
    {
        xs_fail_script(b, index, "script too large");
        return;
    }
    /* Same compile-safety gate as muJS/Duktape/QuickJS (one bar).
     * PLUTO_NR_BISECT (host offline-bisect harness ONLY): skip the RAM-path
     * pre-scan so the engine's real parser surfaces the true SyntaxError
     * position for large minified bundles — the sim's disk-stream path
     * scans per-segment and never sees this one-shot bar, so the whole-file
     * RAM scan on host masks the actual error this gate precedes. Never
     * defined on simulator/device builds (mirrors the PLUTO_CHUNK_FORCE
     * seam below). */
#if !defined(PLUTO_NR_BISECT)
    if (!jsbridge_script_compile_safe(src, len))
    {
        xs_fail_script(b, index, "script too deeply nested (compile guard)");
        return;
    }
#endif

#ifdef PLUTO_CHUNK_FORCE
    /* Test seam (sim only, like the stream path): always try the split
     * first so the inline-source segmentation is exercisable where the
     * host memory probe would always grant the whole-file compile. */
    if (xs_try_bundler_split_src(b, src, len, index))
        return;
#endif

#ifdef TARGET_PLAYDATE
    /* R18 before whole-file admission (same ordering as the stream
     * path): when the compile's estimated peak isn't grantable, a
     * bundler monolith is ONE top-level statement — no chunker can
     * help, but the module-map splitter can, and the source is already
     * materialized in RAM here (no spill handle needed). Non-monoliths
     * fall through to the whole-file admission inside xs_parse_and_run,
     * which defers as before. */
    {
        size_t earlyNeed = jsbridge_compile_need((unsigned long)len);
        if (xs_probe_grantable(earlyNeed) < earlyNeed)
        {
            if (xs_try_bundler_split_src(b, src, len, index))
                return;
        }
    }
#endif

#ifdef TARGET_PLAYDATE
    /* R54 belt+braces: this whole-file parse runs inside ONE engine
     * bracket on ONE update() frame — the device lexer costs ~1s per
     * ~30KB of minified JS and the run-loop watchdog hard-crashes (E1)
     * past 10s. Over the cap: skip containedly instead of stalling. */
    if (len >= (size_t)JSBRIDGE_ONE_FRAME_SCRIPT_MAX)
    {
        b->errs++;
        logger_log("[js] script %d skipped: %zuB monolith over the %dKB "
                   "one-frame parse cap (device cannot lex it unyielded)",
                   index, len, JSBRIDGE_ONE_FRAME_SCRIPT_MAX >> 10);
        bridge_take_error_text(b, "script over one-frame parse cap");
        return;
    }
#endif

    b->ran++;

    /* SW5 ES5 prefix + page source in ONE buffer (prefix is deterministic
     * per build and NOT part of the compile gate input). */
    const char *prefix = NULL;
    size_t plen = jsbridge_sw5_prefix(&prefix);

    /* Copy + NUL-terminate (span is followed by '</script>' in the page
     * buffer; the lexer stops at NUL like the other bridges' copy). */
    char *buf = (char *)JMalloc(plen + len + 1);
    if (!buf)
    {
        b->errs++;
        return;
    }
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, src, len);
    buf[plen + len] = '\0';
    len += plen;/* R15 admission (device): OS truth, not a fixed cap. R14 refuted the
 * fragmentation theory on hardware — a single 4MB block was grantable
 * and the compile still died by GROWING into the pool's last megabyte
 * (wall at ~7.3MB live, of which ~1MB was the two source copies that
 * the streaming path eliminates). Estimate the compile's peak footprint
 * from the measured R13/R14 trajectory (shared JSBRIDGE_COMPILE_* knobs)
 * and require that much ACTUALLY GRANTABLE memory before parsing. Denial
 * is contained AND deferred: disk-resident scripts are re-offered to the
 * router and compile the moment memory frees. Host builds skip the probe. */
#ifdef TARGET_PLAYDATE
    size_t bodyLen = len - plen;
    size_t execPeak = jsbridge_compile_need((unsigned long)bodyLen);
#else
    size_t execPeak = 0;
#endif

    /* The SW5 ES5 prefix compiles as its own tiny script first —
     * functionally identical to concatenation (same machine, same order)
     * without putting a 504KB body behind a 1.7KB buffer. */
    if (plen && xs_run_tiny(b, st->machine, index, prefix, plen, "sw5 prefix"))
    {
        JFree(buf);
        return;
    }

    xsMachine *m = st->machine;
    if (!m)
    {
        JFree(buf);
        return;
    }
    /* fxStringCGetter reads a txStringCStream — wrap the materialized
     * buffer (same contract as xs_run_tiny above). */
    txStringCStream css;
    css.buffer = buf;
    css.offset = 0;
    css.size = len;
    xs_parse_and_run(b, m, index, &css, fxStringCGetter, len, execPeak,
                     "script");
    JFree(buf);
}

/* R16: chunked parse (top-level statement segmentation). The R16 harness
 * prototype (/tmp/nrharness chunk_scan.c + xrt3.c) proved: peak parser
 * memory = largest top-level statement, NOT file size, because fxRunScript
 * frees each program's code buffer on completion (xsRun.c:5125) and
 * function bodies are copied into machine chunks (xsRun.c:2943). A
 * statement-sequence script that cannot fit whole can still run if compiled
 * in segments. Known bounded divergences (recorded in MASTER_TODO R16):
 * use-before-declaration across a boundary throws in the consuming segment;
 * a later segment's SyntaxError lets earlier segments run first.
 *
 * Splitter: streaming two-pass scan over the spill handle. Pass 1 counts
 * segments (hard-capped — the plan array is machine-heap and must not
 * crowd out the compile it enables); pass 2 re-scans and compiles+runs
 * each segment as its own mxProgramFlag program. Boundaries ONLY at
 * depth-0 ';' / block-close '}' / EOF; lexer-aware (comments, strings,
 * nested templates with ${}, regex-vs-division, ASI continuation
 * suppression); segments tile the stream losslessly (inter-statement
 * whitespace/comments ride with the NEXT segment); a statement-opening
 * string literal starts a statement, a leading bare ';' is a no-op.
 * "use strict" directives are replicated onto every later segment's head
 * so per-segment parsing sees the same prologue. A '}' close is only a
 * boundary when the next significant token doesn't continue the statement
 * (call/member tails, operators, else/catch/finally/while) — peeked
 * through the sliding window; unknown-at-window-edge suppresses
 * (conservative merge, never a corrupt split).
 *
 * Cost: one extra streaming read of the source (the scan); compile streams
 * happen per segment. Site-agnostic: no filenames, no allowlists, only
 * generic structural limits. */

/* Packing limits (R16 harness-measured): an 8KB segment peaks at ~655KB
 * parser memory on statement-heavy code — just under the R13 768KB device
 * parser cap, which bounds ANY parse containedly. */
#define XCS_SEG_MAX (8u * 1024u)
#define XCS_PRO_MAX 256
#define XCS_PLAN_MAX 65536   /* packed-segment cap (absurd = fall back) */

typedef struct
{
    uint32_t start;
    uint32_t end;
} xsChunkSpan;

/* Streaming scan state: byte-fed, O(1) — same shape as xsStreamSrc. */
typedef struct xsChunkScan
{
    SpillFile spill;
    size_t total;
    char window[1024];
    size_t wbase;   /* stream offset of window[0] */
    size_t wlen;    /* valid bytes in window */
    size_t rpos;    /* next stream position to read (absolute) */
    /* lexer state */
    int state;      /* 0 raw, 1 line-comment, 2 block-comment, 3 string,
                       4 template, 5 template-expr, 6 regex */
    unsigned char q;    /* active string quote */
    unsigned char rl;   /* regex: last char was '[' */
    int tdepth;         /* brace depth inside current ${ } */
    int tstack[16];
    int tsp;
    /* statement matcher */
    int depth;          /* ({[ nesting */
    int inStmt;         /* statement has code */
    uint32_t lastEnd;   /* absolute end of the previous segment */
    int lastValueCtx;   /* 1 = previous significant token was a value */
    int lastKwOperand;  /* previous word was return/throw */
    unsigned char prevClass;
    int segCount;       /* raw statements found so far */
    int err;            /* 0 ok, else abandon chunking (fall back whole) */
    xsChunkSpan *segs;  /* optional raw-boundary recording */
    /* raw-statement event (R16 chunked path; NULL = none) */
    void (*onRaw)(void *ctx, struct xsChunkScan *s, uint32_t start,
                  uint32_t end);
    void *eventCtx;
} xsChunkScan;

enum
{
    XCS_RAW = 0,
    XCS_LC,
    XCS_BC,
    XCS_STR,
    XCS_TPL,
    XCS_TPLX,
    XCS_RE
};

static int xcs_refill(xsChunkScan *s, size_t pos)
{
    if (pos < s->wbase + s->wlen)
        return 1;
    size_t keep = s->wlen < 8 ? s->wlen : 8;
    size_t keepOff = s->wbase + s->wlen - keep;
    memmove(s->window, s->window + (s->wlen - keep), keep);
    size_t want = sizeof(s->window) - keep;
    size_t remain = s->total - (keepOff + keep);
    if (want > remain)
        want = remain;
    long got = 0;
    if (want > 0)
        got = pluto_spill_read(s->spill, (long)(keepOff + keep),
                               s->window + keep, want);
    if (got < 0)
        got = 0;
    s->wbase = keepOff;
    s->wlen = keep + (size_t)got;
    return pos < s->wbase + s->wlen;
}

static unsigned char xcs_peek(xsChunkScan *s, size_t pos)
{
    if (pos >= s->total || !xcs_refill(s, pos))
    {
        /* Out of window (EOF/short read): never leave the drive loop
         * spinning on an unreadable position. */
        if (s->rpos <= pos)
            s->rpos = pos + 1;
        return 0;
    }
    return (unsigned char)s->window[pos - s->wbase];
}

static int xcs_isid(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

/* Does the identifier ending just before `at` match kw? Requires the
 * whole keyword to still be inside the sliding window (always true for
 * 3-6 char keywords with 8-byte keeps and forward reads). */
static int xcs_word_before(xsChunkScan *s, size_t at, const char *kw)
{
    size_t n = strlen(kw);
    if (at < n)
        return 0;
    size_t p = at - n;
    if (!xcs_refill(s, p))
        return 0;
    if (s->window[at - 1 - s->wbase] >= 0x80)
        return 0;
    return memcmp(s->window + p - s->wbase, kw, n) == 0;
}

/* Next significant token after `at` continues the statement? Comments and
 * whitespace are skipped within the window; unknown-at-edge = yes. */
static int xcs_continues(xsChunkScan *s, size_t at)
{
    size_t j = at + 1;
    for (;;)
    {
        while (j < s->total)
        {
            unsigned char c = xcs_peek(s, j);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                j++;
            else
                break;
        }
        if (j >= s->total)
            return 1;
        unsigned char c = xcs_peek(s, j);
        if (c == '/' && j + 1 < s->total)
        {
            unsigned char d = xcs_peek(s, j + 1);
            if (d == '/')
            {
                j += 2;
                while (j < s->total && xcs_peek(s, j) != '\n')
                    j++;
                continue;
            }
            if (d == '*')
            {
                j += 2;
                while (j + 1 < s->total &&
                       !(xcs_peek(s, j) == '*' && xcs_peek(s, j + 1) == '/'))
                    j++;
                j = (j + 1 < s->total) ? j + 2 : s->total;
                continue;
            }
        }
        break;
    }
    if (j >= s->total)
        return 1;
    unsigned char c = xcs_peek(s, j);
    if (c == '(' || c == '[' || c == '.' || c == '`' || c == '=' ||
        c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
        c == '<' || c == '>' || c == '&' || c == '|' || c == '^' ||
        c == '?' || c == ':' || c == ',' || c == '!' || c == '~')
        return 1;
    if (xcs_isid(c))
    {
        static const char *const kws[4] = {"else", "catch", "finally", "while"};
        size_t k = j;
        while (k < s->total && xcs_isid(xcs_peek(s, k)))
            k++;
        size_t n = k - j;
        for (int i = 0; i < 4; i++)
        {
            if (strlen(kws[i]) == n)
            {
                int ok = 1;
                for (size_t m2 = 0; m2 < n; m2++)
                    if (xcs_peek(s, j + m2) != (unsigned char)kws[i][m2])
                    {
                        ok = 0;
                        break;
                    }
                if (ok)
                    return 1;
            }
        }
    }
    return 0;
}

static void xcs_emit(xsChunkScan *s, uint32_t end)
{
    if (s->segCount >= XCS_PLAN_MAX)
    {
        s->err = 1;
        return;
    }
    if (s->segs && s->segCount < XCS_PRO_MAX) /* recording cap: only the
        leading directives are ever copied back (xcs_fill_pro) */
    {
        s->segs[s->segCount].start = s->lastEnd;
        s->segs[s->segCount].end = end;
    }
    if (s->onRaw)
        s->onRaw(s->eventCtx, s, s->lastEnd, end);
    s->segCount++;
    s->lastEnd = end;
}

static void xcs_stmt_char(xsChunkScan *s, unsigned char c, size_t at)
{
    int sig = !(c == ' ' || c == '\t' || c == '\r' || c == '\n');

    if (s->depth == 0 && !s->inStmt && sig && c != ';')
        s->inStmt = 1; /* segment start = previous end (lossless tiling);
                          leading bare ';' is a no-op, skipped */

    if (c == '{' || c == '(' || c == '[')
    {
        s->depth++;
        if (s->depth > 65536)
            s->err = 1;
    }
    else if (c == '}')
    {
        if (s->depth > 0)
            s->depth--;
        if (s->depth == 0 && s->inStmt && !xcs_continues(s, at))
        {
            xcs_emit(s, (uint32_t)(at + 1));
            s->inStmt = 0;
            s->lastValueCtx = 0;
            s->prevClass = 0;
            s->lastKwOperand = 0;
        }
    }
    else if (c == ')' || c == ']')
    {
        if (s->depth > 0)
            s->depth--; /* parens/brackets never end a top-level statement */
    }
    else if (c == ';' && s->depth == 0 && s->inStmt)
    {
        xcs_emit(s, (uint32_t)(at + 1));
        s->inStmt = 0;
        s->lastValueCtx = 0;
        s->prevClass = 0;
        s->lastKwOperand = 0;
    }

    if (sig)
    {
        if (s->lastKwOperand)
        {
            s->lastValueCtx = 0;
            s->lastKwOperand = 0;
        }
        if (c >= '0' && c <= '9')
            s->lastValueCtx = 1;
        else if (c == '+' || c == '-')
            s->lastValueCtx = (s->prevClass == c) ? 0 : 1;
        else if (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' ||
                 c == '`')
            s->lastValueCtx = 1;
        else
            s->lastValueCtx = 0;
        s->prevClass = c;
    }
}

static void xcs_raw_step(xsChunkScan *s, size_t at, int inTplX)
{
    unsigned char c = xcs_peek(s, at);

    s->rpos = at + 1; /* every path consumes at least this byte (the
                         explicit cases below may advance further) */

    switch (s->state)
    {
    case XCS_LC:
        if (c == '\n')
            s->state = inTplX ? XCS_TPLX : XCS_RAW;
        return;

    case XCS_BC:
        if (c == '*' && at + 1 < s->total && xcs_peek(s, at + 1) == '/')
        {
            s->state = inTplX ? XCS_TPLX : XCS_RAW;
            s->rpos = at + 2;
        }
        return;

    case XCS_STR:
        if (c == '\\')
            s->rpos = at + 2;
        else if (c == s->q)
        {
            s->state = inTplX ? XCS_TPLX : XCS_RAW;
            s->lastValueCtx = 1;
        }
        else if (c == '\n')
            s->state = inTplX ? XCS_TPLX : XCS_RAW; /* unterminated */
        return;

    case XCS_RE:
        if (c == '\\')
            s->rpos = at + 2;
        else if (c == '[')
            s->rl = 1;
        else if (c == ']')
            s->rl = 0;
        else if (c == '/' && !s->rl)
        {
            size_t j = at + 1;
            while (j < s->total && xcs_isid(xcs_peek(s, j)))
                j++;
            s->rpos = j;
            s->state = inTplX ? XCS_TPLX : XCS_RAW;
            s->rl = 0;
            s->lastValueCtx = 1;
            s->prevClass = 0;
        }
        else if (c == '\n')
        {
            s->state = inTplX ? XCS_TPLX : XCS_RAW;
            s->lastValueCtx = 0;
            s->prevClass = 0;
        }
        return;

    default:
        break;
    }

    /* XCS_RAW / XCS_TPLX */
    switch (c)
    {
    case '/':
        if (at + 1 < s->total && xcs_peek(s, at + 1) == '/')
        {
            s->state = XCS_LC;
            s->rpos = at + 2;
        }
        else if (at + 1 < s->total && xcs_peek(s, at + 1) == '*')
        {
            s->state = XCS_BC;
            s->rpos = at + 2;
        }
        else if (!inTplX && !s->lastValueCtx)
        {
            s->state = XCS_RE;
            s->rl = 0;
            s->rpos = at + 1;
        }
        else
        {
            if (!inTplX)
                xcs_stmt_char(s, c, at);
        }
        return;

    case '"':
    case '\'':
        s->state = XCS_STR;
        s->q = c;
        s->rpos = at + 1;
        if (!inTplX)
            s->inStmt = 1; /* statement may open with a directive literal */
        return;

    case '`':
        if (s->tsp >= 16)
        {
            s->err = 1;
            s->rpos = at + 1;
            return;
        }
        s->tstack[s->tsp++] = inTplX ? XCS_TPLX : XCS_RAW;
        s->state = XCS_TPL;
        s->rpos = at + 1;
        return;

    case '{':
        if (inTplX)
            s->tdepth++;
        else
            xcs_stmt_char(s, c, at);
        return;

    case '}':
        if (inTplX)
        {
            if (s->tdepth > 0)
                s->tdepth--;
            else
                s->state = s->tstack[--s->tsp];
        }
        else
            xcs_stmt_char(s, c, at);
        return;

    default:
        if (!inTplX)
        {
            xcs_stmt_char(s, c, at);
            if (!xcs_isid(c) && at > 0)
            {
                if (xcs_word_before(s, at, "return") ||
                    xcs_word_before(s, at, "throw"))
                    s->lastKwOperand = 1;
            }
        }
        else if (!xcs_isid(c))
        {
            if (c == ')' || c == ']' || (c >= '0' && c <= '9'))
                s->lastValueCtx = 1;
            else
                s->lastValueCtx = 0;
        }
        return;
    }
}

/* One streaming scan pass. Returns segment count (or -1 to abandon). */
static int xcs_scan(xsChunkScan *s, xsChunkSpan *segs)
{
    s->segs = segs;
    s->rpos = 0;
    s->state = XCS_RAW;
    s->tsp = 0;
    s->tdepth = 0;
    s->depth = 0;
    s->inStmt = 0;
    s->lastEnd = 0;
    s->lastValueCtx = 0;
    s->lastKwOperand = 0;
    s->prevClass = 0;
    s->segCount = 0;
    s->err = 0;
    while (s->rpos < s->total && !s->err)
    {
        size_t at = s->rpos;
        size_t prev = s->rpos; /* monotonicity guard (see below) */
        switch (s->state)
        {
        case XCS_TPL:
        {
            unsigned char c = xcs_peek(s, at);
            if (c == '\\')
                s->rpos = at + 2;
            else if (c == '`')
            {
                s->state = (s->tsp > 0) ? s->tstack[--s->tsp] : XCS_RAW;
                s->rpos = at + 1;
                s->lastValueCtx = 1;
            }
            else if (c == '$' && at + 1 < s->total && xcs_peek(s, at + 1) == '{')
            {
                if (s->tsp >= 16)
                {
                    s->err = 1;
                }
                else
                {
                    s->tstack[s->tsp++] = XCS_TPL;
                    s->state = XCS_TPLX;
                    s->tdepth = 0;
                }
                s->rpos = at + 2;
            }
            else
                s->rpos = at + 1;
            break;
        }
        case XCS_TPLX:
        case XCS_RAW:
            xcs_raw_step(s, at, s->state == XCS_TPLX);
            break;
        default:
            xcs_raw_step(s, at, 0);
            break;
        }
        if (s->rpos <= prev)
            s->rpos = at + 1; /* lookahead rewinds rpos to `at` to re-dispatch; force 1 byte */
    }
    if (s->err)
        return -1;
    if (s->inStmt)
    {
        xcs_emit(s, (uint32_t)s->total);
        s->inStmt = 0;
    }
    return s->segCount;
}

static int xcs_copy_bytes(SpillFile spill, uint32_t start, uint32_t len,
                          char *dst);

/* Directive statement? Text (minus trailing ';' and ws) is one string. */
static int xcs_is_directive(xsChunkScan *s, uint32_t a, uint32_t b)
{
    if (b - a > 160)
        return 0;
    char buf[160];
    if (b > s->total)
        return 0;
    /* Direct reads: the statement start is behind the sliding window by
     * the time xcs_on_raw fires (xcs_peek would index before window[0]). */
    if (xcs_copy_bytes(s->spill, a, b - a, buf) != 0)
        return 0;
    uint32_t lo = 0, hi = b - a;
    while (hi > lo && (buf[hi - 1] == ';' || buf[hi - 1] == ' ' ||
                       buf[hi - 1] == '\t' || buf[hi - 1] == '\r' ||
                       buf[hi - 1] == '\n'))
        hi--;
    while (lo < hi && (buf[lo] == ' ' || buf[lo] == '\t' || buf[lo] == '\r' ||
                       buf[lo] == '\n'))
        lo++;
    if (hi - lo < 2)
        return 0;
    unsigned char q = (unsigned char)buf[lo];
    if (q != '"' && q != '\'')
        return 0;
    if ((unsigned char)buf[hi - 1] != q)
        return 0;
    for (uint32_t i = lo + 1; i + 1 < hi; i++)
        if ((unsigned char)buf[i] == q)
            return 0;
    return 1;
}

/* ── Chunked execute (R16): top-level statement segmentation ───────────
 * The R16 harness prototype (/tmp/nrharness chunk_scan.c + xrt3.c) proved:
 * peak parser memory = largest compiled unit, NOT file size, because
 * fxRunScript frees each program's code buffer on completion (xsRun.c:5125)
 * and function bodies are copied into machine chunks (xsRun.c:2943).
 * Packed segment = greedy run of consecutive raw statements <= XCS_SEG_MAX.
 * An 8KB segment peaks ~655KB parser memory (R16 measurement) — inside the
 * R13 768KB parser cap that bounds any single parse containedly.
 * Two passes over the spill file share ONE greedy packer (via the scanner's
 * raw-statement event), so pass 2 reproduces pass 1 exactly:
 *   pass 1 (measure): counts segments, biggest segment, directive prologue;
 *          one OS-truth probe then covers the whole attempt ATOMICALLY
 *          (nothing has executed if it denies).
 *   pass 2 (execute): compiles+runs each packed segment as it closes — as
 *          its own mxProgramFlag program on the page machine, through
 *          xs_parse_and_run (same containment + error surfacing; the SW5
 *          prefix already ran before this call). The directive prologue is
 *          replicated onto every later segment's head.
 * Known bounded divergences (MASTER_TODO R16): use-before-declaration
 * across a segment boundary throws in the consuming segment; a later
 * segment's SyntaxError lets earlier segments run first. Site-agnostic:
 * no filenames, no allowlists, only generic structural limits. */
typedef struct xsChunkPlanner
{
    int pass;               /* 1 = measure, 2 = execute */
    /* packing state (both passes) */
    uint32_t pStart;        /* current packed segment start */
    uint32_t pEnd;          /* current packed segment end */
    int pOpen;              /* a packed segment is open */
    int pDirective;         /* current packed segment is all-directives */
    /* outputs (pass 1) */
    int nSegs;
    uint32_t maxSeg;
    int proCount;           /* leading directive RAW statements (0: none) */
    int proDecided;         /* prologue decision final (first code stmt) */
    /* pass 2 execute state */
    JsBridge *b;
    xsMachine *m;
    int index;
    int k;                  /* segments executed so far */
    int stop;               /* pass 2: a segment failed containedly */
    int errsAtStart;
    char pro[XCS_PRO_MAX];
    int proLen;
    char *segBuf;           /* one heap segment buffer, reused */
} xsChunkPlanner;

static int xcs_seg_execute(xsChunkPlanner *p, xsChunkScan *s, uint32_t start,
                           uint32_t end, int isDirective);

/* Copy `len` bytes at absolute offset `start` from the spill file into
 * dst. Position-independent direct reads — the sliding scan window has
 * always moved past a closed segment's start by the time it closes, so
 * segment copies MUST NOT go through xcs_peek (behind-window reads
 * return stale memory — the R16-integration sim bug this fixes). */
static int xcs_copy_bytes(SpillFile spill, uint32_t start, uint32_t len,
                          char *dst)
{
    uint32_t done = 0;
    while (done < len)
    {
        uint32_t want = len - done;
        if (want > 1024)
            want = 1024;
        long got = pluto_spill_read(spill, (long)(start + done),
                                    dst + done, want);
        if (got <= 0)
            return -1;
        done += (uint32_t)got;
    }
    return 0;
}

/* Raw statement closed: [start,end) is exactly one top-level statement. */
static void xcs_on_raw(void *ctx, xsChunkScan *s, uint32_t start,
                       uint32_t end)
{
    xsChunkPlanner *p = (xsChunkPlanner *)ctx;
    int allDirective = xcs_is_directive(s, start, end);

    if (!p->pOpen)
    {
        p->pStart = start;
        p->pEnd = end;
        p->pOpen = 1;
        p->pDirective = allDirective;
        if (p->pass == 1 && !p->proDecided)
        {
            /* directive prologue: decided entirely by the FIRST raw
             * statement — directive => prologue candidate; code => none */
            if (!allDirective)
            {
                /* first code statement: prologue = whatever accumulated
                 * (0 if the file opens with code) — decision final */
                p->proDecided = 1;
            }
            else if (p->proLen + (int)(end - start) + 1 <= XCS_PRO_MAX)
            {
                p->proCount++;
                p->proLen += (int)(end - start);
                if (xcs_peek(s, end - 1) != '\n')
                    p->proLen += 1;
            }
            else
            {
                p->proCount = -1; /* prologue too big — run without it */
                p->proDecided = 1;
            }
        }
        return;
    }

    /* Close the open packed segment if this statement would overflow it
     * or cross a directive-run boundary. */
    if ((uint32_t)(end - p->pStart) > XCS_SEG_MAX ||
        (allDirective && !p->pDirective) ||
        (!allDirective && p->pDirective))
    {
        uint32_t sz = p->pEnd - p->pStart;
        if (p->pass == 1)
        {
            p->nSegs++;
            if ((int)sz > (int)p->maxSeg)
                p->maxSeg = sz;
            if (p->nSegs > XCS_PLAN_MAX)
            {
                s->err = 1;
                return;
            }
        }
        else
        {
            if (xcs_seg_execute(p, s, p->pStart, p->pEnd, p->pDirective) != 0)
            {
                s->err = 1; /* contained failure — stop the scan */
                return;
            }
        }
        p->pStart = start;
        p->pEnd = end;
        p->pDirective = allDirective;
    }
    else
    {
        p->pEnd = end;
        p->pDirective = p->pDirective && allDirective;
    }
}

/* Pass-2 packed-segment close: copy through the sliding window, re-lead
 * CODE segments with the directive prologue (directive segments run
 * verbatim — they ARE the prologue), compile+run containedly.
 * Returns 0 ok, -1 stop. */
static int xcs_seg_execute(xsChunkPlanner *p, xsChunkScan *s, uint32_t start,
                           uint32_t end, int isDirective)
{
    p->k++;
    logger_log("[js] chunk %d: %uB%s", p->k, (unsigned)(end - start),
               isDirective ? " (directive)" : "");
    if (!p->segBuf)
    {
        p->segBuf = JMalloc(XCS_SEG_MAX + XCS_PRO_MAX + 8);
        if (!p->segBuf)
            return -1;
    }
    char *seg = p->segBuf;
    size_t lead = 0;
    if (p->proLen > 0 && !isDirective)
    {
        memcpy(seg, p->pro, (size_t)p->proLen);
        lead = (size_t)p->proLen;
    }
    if (lead + (size_t)(end - start) + 1 > XCS_SEG_MAX + XCS_PRO_MAX + 8)
        return -1;
    if (xcs_copy_bytes(s->spill, start, end - start, seg + lead) != 0)
        return -1;
    seg[lead + (end - start)] = 0;
    int errsBefore = p->b->errs;
    /* fxStringCGetter reads a txStringCStream (buffer/offset/size) — NOT a
     * bare char*. Passing the raw segment made the getter read the script
     * TEXT as a pointer (R16-integration sim SIGSEGV in fxStringCGetter). */
    txStringCStream css;
    css.buffer = seg;
    css.offset = 0;
    css.size = lead + (size_t)(end - start);
    xs_parse_and_run(p->b, p->m, p->index, &css, fxStringCGetter,
                     css.size, 0, "chunk");
    p->m = ((XsState *)p->b->implState)->machine;
    if (!p->m)
        return -1; /* engine aborted — containment already ran */
    if (p->b->errs != errsBefore)
    {
        p->stop = 1; /* THIS segment failed containedly (throw/abort) */
        return -1;   /* error already surfaced; stop the segment chain */
    }
    return 0;
}

/* Copy the prologue text (the first proCount raw statements) in one scan. */
static void xcs_fill_pro(xsChunkPlanner *p, SpillFile spill, size_t len)
{
    xsChunkScan s;
    memset(&s, 0, sizeof(s));
    s.spill = spill;
    s.total = len;
    int len2 = 0;
    /* proCount counts RAW leading directives; record their spans in one
     * scan (xcs_emit caps recording at XCS_PRO_MAX entries) and copy. */
    static xsChunkSpan bounds[XCS_PRO_MAX];
    s.segs = bounds;
    xcs_scan(&s, NULL);
    s.segs = NULL;
    for (int i = 0; i < s.segCount && i < p->proCount; i++)
    {
        uint32_t a = bounds[i].start, z = bounds[i].end;
        if (len2 + (int)(z - a) + 1 > XCS_PRO_MAX)
            break;
        /* Direct reads: the scan above already slid the window to EOF,
         * so every prologue offset is behind it (xcs_peek would read OOB). */
        if (xcs_copy_bytes(spill, a, z - a, p->pro + len2) != 0)
            break;
        len2 += (int)(z - a);
        if (len2 > 0 && p->pro[len2 - 1] != '\n')
            p->pro[len2++] = '\n';
    }
    p->proLen = len2;
}

/* Chunked compile+run entry. Returns 1 = handled (ok or contained
 * failure), 0 = fall back to the whole-file path. */
static int xs_run_script_chunked(JsBridge *b, SpillFile spill, size_t len,
                                 int index)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st ? st->machine : NULL;
    if (!m || len > 0xFFFFFFFFu)
        return 0;

    /* ── pass 1: measure + validate (no execution, no big allocations) ── */
    xsChunkPlanner p1;
    memset(&p1, 0, sizeof(p1));
    p1.pass = 1;
    xsChunkScan s1;
    memset(&s1, 0, sizeof(s1));
    s1.spill = spill;
    s1.total = len;
    s1.onRaw = xcs_on_raw;
    s1.eventCtx = &p1;
    xcs_scan(&s1, NULL);
    if (s1.err)
        return 0;
    if (p1.pOpen)
    {
        uint32_t sz = p1.pEnd - p1.pStart;
        p1.nSegs++;
        if ((int)sz > (int)p1.maxSeg)
            p1.maxSeg = sz;
        p1.pOpen = 0;
    }
    if (p1.nSegs < 2 || p1.maxSeg > XCS_SEG_MAX)
        return 0; /* monolith / degenerate: whole-file path (defer/fail) */
    logger_log("[js] script %d: chunked plan ok — %d segments, biggest %uB",
               index, p1.nSegs, p1.maxSeg);

#ifdef TARGET_PLAYDATE
    /* ATOMIC admission: the biggest packed segment decides; every segment
     * is <= maxSeg, so one probe covers the attempt. Nothing has run. */
    size_t segPeak = jsbridge_compile_need((unsigned long)p1.maxSeg + 64);
    if (xs_probe_grantable(segPeak) < segPeak)
    {
        logger_log("[js] script %d chunked: biggest segment %uB needs "
                   "%zuKB, not grantable — deferred",
                   index, p1.maxSeg, segPeak >> 10);
        return 0; /* whole-file path defers atomically */
    }
#endif

    /* ── pass 2: execute ── */
    xsChunkPlanner p2;
    memset(&p2, 0, sizeof(p2));
    p2.pass = 2;
    p2.b = b;
    p2.m = m;
    p2.index = index;
    p2.errsAtStart = b->errs;
    if (p1.proCount > 0)
        xcs_fill_pro(&p2, spill, len);
    xsChunkScan s2;
    memset(&s2, 0, sizeof(s2));
    s2.spill = spill;
    s2.total = len;
    s2.onRaw = xcs_on_raw;
    s2.eventCtx = &p2;
    xcs_scan(&s2, NULL);
    if (p2.pOpen)
        xcs_seg_execute(&p2, &s2, p2.pStart, p2.pEnd, p2.pDirective);
    if (p2.segBuf)
        JFree(p2.segBuf);
    logger_log("[js] script %d: chunked run complete — %d segments",
               index, p2.k);
    return 1;
}

/* ── R18: bundler-monolith split execution (general-purpose) ───────────
 * A webpack/rollup production bundle is ONE top-level statement — the R16
 * statement chunker cannot split it, and its whole-file parse peak (which
 * scales with FILE size inside one fxRunScript) exceeds the NR fork's
 * parser-memory cap. jsbridge_bundler_plan recognizes the module-map
 * STRUCTURE (never a site: no URLs, no names) and we parse+run the
 * segments in THIS machine: `var <m>={}` registry, one `<m>[key]=<value>`
 * program per module (vars land on the shared global object — the same
 * cross-program persistence the R16 chunked path relies on — so modules
 * require each other exactly as authored), then the bootstrap+entry tail
 * as its own IIFE program. Any segment too big for admission (the 118KB
 * React-DOM module, e.g.) is contained-skipped; everything smaller still
 * runs. Returns 1 = handled (split attempted), 0 = not a bundler monolith
 * (caller falls through to chunked/whole-file paths untouched). */
static int xs_split_pump(JsBridge *b, int index);

static int xs_run_script_split(JsBridge *b, const char *src, size_t len,
                               int index, const PlutoBundlerPlan *plan)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st ? st->machine : NULL;
    if (!m)
        return 1;
#ifdef TARGET_PLAYDATE
    /* R19: NO probe ladder in the split path. The per-seg ladder (~900
     * alloc/touch/free cycles per page) and even the 16-seg recalibration
     * correlated exactly with the device allocator wedging at seg ~62-65
     * (freeze right after the seg-64 probe, ~2.4MB funnel live). Admission
     * is funnel accounting; the real OOM backstop is the engines' contained
     * NULL handling (fxNewParserChunk abort → engine reset → page
     * continues) instead of an allocator-churning ladder. */
#endif
    /* R20: TIME-SLICED execution. The device watchdog reports a freeze
     * when the SDK run loop sees no update() return for 10 seconds
     * (errorlog "Run loop stalled for more than 10 seconds" — every
     * "crash" of this class symbolizes to wherever the CPU happened to
     * be at second ten, and this run's timestamps prove it: boot → seg
     * 67 in exactly 10.0s). Executing all ~96 segments inside ONE
     * update() is inherently a >10s no-yield stretch on a 180MHz board.
     * The fix is general: run segments until a per-frame time budget
     * elapses, then RETURN to the run loop and resume on the next
     * frame through the engine pump vtable (R20 pump below). */
    /* R20: compact ownership copy — the caller's plan/spans array dies
     * with this stack frame; the time-sliced session outlives it. */
    int ncopy = plan->nSpans;
    if (ncopy > PLUTO_SPLIT_SPANS_INLINE)
    {
        ncopy = PLUTO_SPLIT_SPANS_INLINE; /* planner caps at 2048 entries */
        logger_log("[js] bundle %d: plan truncated to %d segments (over "
                   "inline cap) — tail not run", index, ncopy);
    }
    memcpy(st->split.spansBuf, plan->spans,
           sizeof(PlutoBundlerSpan) * (size_t)ncopy);
    st->split.plan.spans = st->split.spansBuf;
    st->split.plan.maxSpans = ncopy;
    st->split.plan.nSpans = ncopy;
    st->split.plan.entries = plan->entries;
    st->split.plan.tailIndex = plan->tailIndex;
    st->split.plan.tailCount = plan->tailCount;
    st->split.plan.tailHasComma = plan->tailHasComma;
    /* R26h: entry-unwrap state MUST travel with the ownership copy —
     * without it the session's tail segments emit the runtime bootstrap
     * UNRENAMED (bare e/t) while the registry/cache moved to __we/__wt;
     * the first require then reads the body's `var e={}` namespace
     * instead of the registry (observed: 'call: not a function' inside
     * the require at the first body require). */
    st->split.plan.entryBodyIndex = plan->entryBodyIndex;
    st->split.plan.registryRenamed = plan->registryRenamed;
    st->split.plan.tailStart = plan->tailStart;
    st->split.plan.tailEnd = plan->tailEnd;
    st->split.plan.tailStep = plan->tailStep;
    /* R55: turbopack plan state travels with the ownership copy — the
     * src/post spans are plan FIELDS here (not spans), so a session that
     * resumes across frames would otherwise emit headless pushes. */
    st->split.plan.kind = plan->kind;
    st->split.plan.turboWrap = plan->turboWrap;
    st->split.plan.turboNestedEnd = plan->turboNestedEnd;
    st->split.plan.turboSrc0 = plan->turboSrc0;
    st->split.plan.turboSrc1 = plan->turboSrc1;
    st->split.plan.turboPost0 = plan->turboPost0;
    st->split.plan.turboPost1 = plan->turboPost1;
    memcpy(st->split.plan.name, plan->name, sizeof(st->split.plan.name));
    /* OWN the source: copy the span once for the whole multi-frame
     * session (freed by xs_split_invalidate). The split resumes across
     * frames; the caller's buffer (doc rawHtml / a materialized segment)
     * may be snapshot-freed as soon as this call returns. */
    {
        char *owned = (char *)JMalloc(len + 1);
        if (!owned)
        {
            return 1; /* copy fail = not handled; caller falls through */
        }
        memcpy(owned, src, len);
        owned[len] = 0;
        st->split.src = owned;
    }
    st->split.srcLen = len;
    st->split.okSegs = 0;
    st->split.failSegs = 0;
    st->split.nextSeg = 0;
    st->split.gcRetries = 0; /* R29 */
    st->split.gcTried = 0;
    st->split.active = 1;
    xs_progress_hint = 0; /* R26m: progress bar starts with the bundle */
    xs_progress_total = st->split.plan.nSpans - 1; /* R26o: denominator up front */
    /* First slice NOW (this frame already spent; remaining budget still
     * usable since the watchdog only counts a full 10s silent stretch). */
    (void)xs_split_pump(b, index);
    return 1; /* handled — remainder resends via the R20 per-frame pump */
}

/* R20: the per-frame split executor — also the RESUME path. Called from
 * xs_run_script_split (first slice) and xs_pump_split (once per frame
 * while st->split.active). Runs segments until the per-frame deadline
 * elapses (budget taken between segments; a single segment ALWAYS runs
 * to completion — the biggest legal segment is bounded by admission) or
 * the plan is exhausted. Returns 1 when finished (session cleared),
 * 0 when more slices remain (next frame continues). */
static int xs_split_pump(JsBridge *b, int index)
{
    XsState *st = (XsState *)b->implState;
    xsMachine *m = st ? st->machine : NULL;
    if (!st || !m || !st->split.active)
        return 1;
#ifdef PLUTO_NR_MOUNT_PAUSE
    /* R28: a parked mount run owns the frame — resume it FIRST, before
     * anything else can touch the machine (the parked registers are the
     * only live state; timers/XHR are gated off while parked). nextSeg
     * already points PAST the mount segment (the atomic branch ran it to
     * a park), so completion falls straight into the plan-exhausted tail. */
    if (st->mount.active)
    {
        int lastSegAll = st->split.plan.nSpans - 1;
        int done;
        logger_log("[js] bundle %d: pump mount branch (parked=%d)", index,
                   st->mount.parked); /* R28 diag */
        done = xs_mount_resume(b, index);
        logger_log("[js] bundle %d: pump mount branch done=%d (parked=%d)",
                   index, done, st->mount.parked); /* R28 diag */
        if (st->machine != m)
        {
            logger_log("[js] bundle %d: engine lost during mount resume",
                       index);
            xs_split_invalidate(b);
            return 1;
        }
        if (!done)
            return 0; /* still parked — next frame */
        /* Run finished: account the segment (the atomic branch skipped
         * its accounting on the park), then fall into the normal loop. */
        int seg = st->mount.seg;
        if (b->errs > st->mount.errs0)
        {
            st->split.failSegs++;
            if (st->split.plan.tailIndex >= 0 &&
                seg >= st->split.plan.tailIndex)
            {
                int remaining = lastSegAll - seg;
                st->split.failSegs += remaining;
                st->split.nextSeg = lastSegAll + 1;
                logger_log("[js] bundle %d: mount slice %d/%d failed — "
                           "skipping remaining %d tail slices",
                           index, seg, lastSegAll, remaining);
            }
        }
        else
        {
            st->split.okSegs++;
        }
        /* nextSeg == seg + 1 already; nothing to advance. */
    }
#endif
    /* R22: a stepwise segment owns the frame while its parse slices
     * across updates. Run the next slice first; when it completes, the
     * segment's accounting already happened in the branch that started
     * it — the seg loop below resumes with the NEXT segment. */
    if (st->step.active)
    {
        /* R22e: resume the in-flight segment — but NOT through the seg
         * loop below. The begin branch restored nextSeg to this segment
         * on yield (the session owns it), so re-running the loop here
         * would re-emit + re-begin the SAME segment forever: its
         * accounting (ok/fail + nextSeg++) lives only in the begin
         * branch, which early-returned on the original yield. Device
         * R26 run 3: seg 29 re-began 49 times after "parse complete".
         * Correct contract: xs_step_pump completes the segment (it
         * already did its own error accounting), then this frame ENDS —
         * the next frame's loop resumes with the segment AFTER it.
         * Host note: xs_step_pump never yields there (one call runs all
         * slices to completion), so this branch just ends the frame
         * early — the begin branch's own accounting still applies. */
        (void)xs_step_pump(b, index);
        if (st->machine != m)
            return 1; /* engine lost — split stops below via caller */
        if (!st->step.active)
        {
            /* Segment completed by the resume path: advance past it (the
             * begin branch restored nextSeg to this segment on yield) and
             * account — the loop below resumes with the NEXT segment. */
            int seg = st->step.seg;
            if (st->split.nextSeg == seg)
                st->split.nextSeg = seg + 1;
            xs_split_step_account(b, index, seg);
        }
        /* Still active → yielded mid-parse: next frame resumes here. */
        return 0;
    }
#ifdef TARGET_PLAYDATE
    PlaydateAPI *pd = pluto_pd();
    unsigned long now = (unsigned long)pd->system->getCurrentTimeMilliseconds();
#elif defined(PLUTO_SPLIT_SIM_FRAMES)
    /* R26q demo/app-sim seam: honor the per-frame budget on the Simulator
     * too (opt-in define). Without a clock here, now=0 made the budget
     * never expire — the whole plan drained inside ONE frame and the
     * loading screen never got a chance to redraw (its bar froze at the
     * stale 5%). With it, the pump yields every budget slice, frames
     * tick, and the screen repaints real progress between slices. NOT
     * set for the host test harnesses: they rely on the synchronous
     * one-call drain contract. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long now = (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
#else
    unsigned long now = 0;
#endif
    unsigned long deadline = now + PLUTO_SPLIT_FRAME_BUDGET_MS;
    st->split.deadlineMs = deadline;
    int lastSeg = st->split.plan.nSpans - 1;
    while (st->split.nextSeg <= lastSeg)
    {
        int seg = st->split.nextSeg;
        size_t cap = jsbridge_bundler_segment_cap(&st->split.plan, seg);
        st->split.nextSeg++;
        xs_progress_hint = seg; /* R26m: feed the on-glass progress bar */
        xs_progress_total = st->split.plan.nSpans - 1; /* R26o: honest denominator */
        /* R26q: feed REAL segment progress into the task system so the
         * native loading bar + percent text show true progress on every
         * platform (0.05 base = the pre-eval one-shot; 0.90 span leaves
         * headroom for the post-eval render phases). Harness builds
         * without tasks.c carry a no-op stub. */
        tasks_report_progress(0.05f + 0.90f * ((float)xs_progress_hint /
                                               (float)(xs_progress_total > 0
                                                           ? xs_progress_total
                                                           : 1)));
        if (cap == 0 || cap > JSBRIDGE_MAX_SCRIPT_SOURCE)
        {
            st->split.failSegs++;
            continue;
        }
        char *buf = (char *)JMalloc(cap + 1);
        if (!buf)
        {
            st->split.failSegs++;
            continue;
        }
        long nseg = jsbridge_bundler_emit(st->split.src, st->split.srcLen,
                                          &st->split.plan, seg, buf, cap);
        if (nseg < 0)
        {
            JFree(buf);
            st->split.failSegs++;
            continue;
        }
#ifdef TARGET_PLAYDATE
        /* R21: honest admission for the capped parser. segPeak already
         * includes the machine-growth allowance (JSBRIDGE_COMPILE_ALLOWANCE
         * inside jsbridge_compile_need) — the R19 check added a SECOND
         * 512K on top, which double-counted and skipped the 253KB
         * bootstrap tail on device run 1 ("needs 3980KB, headroom
         * 4236KB — skipped" with 95/96 segments ok). One allowance is
         * the R13-measured contract. R30f: NO free-slot credit — the pool
         * is counted as live by the funnel (its malloc already happened);
         * crediting it let segments over-admit into the SDK heap wall
         * (run 9 "memory full" at segs 166/195). Admission grows strictly
         * more conservative with the pool in place, and a missed tiny seg
         * (run 8's 565<571) degrades the page instead of crashing it. */
        size_t segPeak = jsbridge_compile_need((unsigned long)nseg);
        size_t segHeadroom = pluto_mem_headroom_bytes();
        if (segHeadroom < segPeak)
        {
            /* R29: engine-heap counters only ever grow inside the split
             * run (GC-recoverable garbage + tracked-table drift), so a
             * shortfall is not final: once per dry streak, force an
             * engine GC and re-base mem_live to the tracked truth, then
             * re-check. Any ADMISSION clears the dry streak so recovery
             * re-arms after the next growth step. (Device R28 run #2:
             * 127 segs skipped with headroom frozen at 445KB across
             * 1.5s — nothing ever recovered it. GC+resync recovers
             * garbage and drift without touching the parked-mount
             * parser heap; safe here because the pump is parked/empty —
             * no xsSlot values are live across this GC point.) */
            if (!st->split.gcTried && !st->mount.active &&
                !st->step.active)
            {
                st->split.gcTried = 1;
                unsigned long liveBefore = pluto_mem_live();
                if (m)
                    fxCollectGarbage(m);
                pluto_mem_resync_live();
                logger_log("[js] bundle %d seg %d/%d: headroom %luKB < "
                           "needs %zuKB — GC+resync live=%luKB→%luKB",
                           index, seg, lastSeg,
                           (unsigned long)(pluto_mem_headroom_bytes() >> 10),
                           segPeak >> 10, liveBefore >> 10,
                           pluto_mem_live() >> 10);
                if (pluto_mem_headroom_bytes() >= segPeak)
                {
                    st->split.gcRetries++;
                    logger_log("[js] bundle %d seg %d/%d: admitted after "
                               "GC recovery (recovered=%d)",
                               index, seg, lastSeg, st->split.gcRetries);
                    goto admitted;
                }
            }
            JFree(buf);
            st->split.failSegs++;
            logger_log("[js] bundle %d seg %d/%d needs %zuKB, headroom "
                       "%luKB — skipped", index, seg, lastSeg,
                       segPeak >> 10,
                       (unsigned long)(segHeadroom >> 10));
            continue;
        }
admitted:;
        st->split.gcTried = 0; /* R29: admitted — re-arm GC recovery */
        /* R21: scope the parser cap to what admission just granted. The
         * parser aborts CONTAINEDLY at fxNRParserTotalCap; the boot cap
         * (1.5MB) is right for normal pages but artificially blocks a
         * giant admitted segment (its parse working-set estimate is what
         * admission just checked against real headroom). Raise for this
         * parse, restore after — the funnel decision IS the bound, and
         * the contained abort at it remains the OOM backstop. */
        unsigned long savedParserCap = fxNRParserTotalCap;
        if ((unsigned long)segPeak > fxNRParserTotalCap)
        {
            fxNRParserTotalCap = (unsigned long)segPeak;
            logger_log("[js] bundle %d seg %d/%d: parser cap %luKB → %zuKB "
                       "(admission-scoped)", index, seg, lastSeg,
                       savedParserCap >> 10, segPeak >> 10);
        }
#else
        size_t segPeak = 0;
#endif
        /* R22: large segments take the STEPWISE path — their parse is
         * paused/resumed across frames (a ~253KB single-statement tail
         * parses in ~20s at the measured device rate, past the 10s
         * watchdog no matter how the plan is sliced). Small segments
         * keep the proven R20 atomic path below. Routing happens AFTER
         * admission so the funnel decision covers the segment's whole
         * multi-frame footprint and xs_step_begin scopes the parser cap
         * to the same grant. */
        if ((size_t)nseg >= PLUTO_SPLIT_STEPWISE_MIN)
        {
            if (xs_step_begin(b, m, index, seg, buf, nseg, segPeak))
            {
                JFree(buf); /* begin failed before taking ownership */
                st->split.failSegs++;
                continue;
            }
            buf = NULL; /* ownership moved into the stepwise session */
            if (!xs_step_pump(b, index))
            {
                st->split.nextSeg--; /* session owns this segment */
                return 0;            /* resume next frame */
            }
            logger_log("[js] bundle %d seg %d: stepwise seg done", index,
                       seg); /* R26e: device trace */
            if (st->machine != m)
            {
                logger_log("[js] bundle %d: engine lost after seg %d/%d "
                           "(stepwise) — stopping split",
                           index, seg, lastSeg);
                xs_split_invalidate(b);
#if defined(PLUTO_SPLIT_SIM_FRAMES) && !defined(TARGET_PLAYDATE)
                logger_log("[js] bundle %d: split run complete: %d ok, %d "
                           "failed segs (aborted; max atomic run %lums)",
                           index, st->split.okSegs, st->split.failSegs,
                           g_splitMaxRunMs);
#else
                logger_log("[js] bundle %d split run complete: %d ok, %d "
                           "failed segs (aborted)",
                           index, st->split.okSegs, st->split.failSegs);
#endif
                if (st->split.failSegs > 0 && !b->lastError[0])
                {
                    snprintf(b->lastError, sizeof(b->lastError),
                             "bundle %d: %d segment failures", index,
                             st->split.failSegs);
                }
                return 1;
            }
            xs_split_step_account(b, index, seg);
#ifdef TARGET_PLAYDATE
            return 0; /* multi-frame path: budget re-checked next frame */
#else
            continue; /* host: the split stays one synchronous call */
#endif
        }
        txStringCStream css;
        css.buffer = buf;
        css.offset = 0;
        css.size = (size_t)nseg;
        int errs0 = b->errs;
#ifdef TARGET_PLAYDATE
        /* R30h: reclaim per-segment PARSE arena garbage before big or late
         * segments. Every atomic segment parses into chunk arenas that cumulate
         * (device run 11: chunks=1712KB by seg 407); the compacting collect at
         * mount-finish reclaimed the mount's arenas but the 85 post-mount
         * segments regrow them, and the FINAL segment (React's client render —
         * the single biggest allocation of the whole eval) then hit the SDK
         * heap wall 300KB short ("memory full", funnel live 5493KB, engine
         * reset DROPPED the built tree → empty render). Compact can only run
         * between segments (never with a parked run); this is that window.
         * Gate: only when the funnel is tight (< 1.5MB headroom) so small
         * pages pay zero extra collects. */
        if (!st->mount.active && !st->step.active &&
            pluto_mem_headroom_bytes() < (1536UL * 1024UL))
        {
            unsigned long liveBefore = pluto_mem_live();
            unsigned long gc0b = xs_mount_now_ms();
            fxCollectGarbage(m);
            pluto_mem_resync_live();
            unsigned long gcMsb = xs_mount_now_ms() - gc0b;
            logger_log("[js] bundle %d seg %d/%d: pre-run compact collect "
                       "live=%luKB→%luKB",
                       index, seg, lastSeg, liveBefore >> 10,
                       pluto_mem_live() >> 10);
            if (gcMsb > 3000UL)
            {
                logger_log("[watchdog] pre-run collect took %lums "
                           "(seg %d/%d)", gcMsb, seg, lastSeg);
            }
        }
#endif
        logger_log("[js] bundle %d seg %d/%d: %ldB — running live=%luKB "
                   "[mount active=%d parked=%d]", index, seg, lastSeg, nseg,
                   (unsigned long)(pluto_mem_live() >> 10), st->mount.active,
                   st->mount.parked); /* R28 diag suffix */
#ifdef PLUTO_NR_MOUNT_PAUSE
        /* R28: RESUMABLE MOUNT — every atomic segment's run carries the
         * meter pause. A segment that fits PLUTO_MOUNT_BURST_MS completes
         * in one burst (zero behavior change); one that overruns (the
         * React mount: a single atomic fxRunScript measured at 111s on
         * device run 6) parks and resumes across frames. */
        {
            int parked = !xs_parse_and_run_mount(b, m, index, &css,
                                                 (size_t)nseg, seg);
#ifdef TARGET_PLAYDATE
            fxNRParserTotalCap = savedParserCap; /* restore boot cap */
#endif
            JFree(buf); /* parse consumed the source in burst 1 */
            if (parked)
            {
                /* nextSeg already points past this segment; the mount
                 * branch at the pump top resumes the run next frame. */
                return 0;
            }
        }
#elif defined(PLUTO_SPLIT_SIM_FRAMES) && !defined(TARGET_PLAYDATE)
        /* R27 stall-baseline instrument (sim only): measure this ATOMIC
         * segment run — seg 407's is the React mount, the exact call the
         * device watchdog kills. Device binary untouched. Uses the SDK
         * ms clock (clock_gettime is macro-redefined by the QuickJS shim
         * in this build and non-monotonic across calls). */
        {
            PlaydateAPI *pdp = pluto_pd();
            unsigned long run0 = pdp
                                     ? (unsigned long)pdp->system->getCurrentTimeMilliseconds()
                                     : 0;
            xs_parse_and_run(b, m, index, &css, fxStringCGetter,
                             (size_t)nseg, 0, "bundle seg");
            if (pdp)
            {
                unsigned long runMs =
                    (unsigned long)pdp->system->getCurrentTimeMilliseconds() -
                    run0;
                if (runMs > g_splitMaxRunMs)
                {
                    g_splitMaxRunMs = runMs;
                }
            }
        }
#else
        xs_parse_and_run(b, m, index, &css, fxStringCGetter, (size_t)nseg,
                         0, "bundle seg");
#endif
#ifdef TARGET_PLAYDATE
        fxNRParserTotalCap = savedParserCap; /* restore boot cap */
#endif
#ifndef PLUTO_NR_MOUNT_PAUSE
        JFree(buf);
#endif
        if (st->machine != m)
        {
            /* Engine ABORT tore the machine down (xs_exec_finish): stop
             * consuming the plan — cross-segment continuation on a dead
             * engine is meaningless, and remaining segments re-offer via
             * the normal script paths later. */
            logger_log("[js] bundle %d: engine lost after seg %d/%d — "
                       "stopping split", index, seg, lastSeg);
            xs_split_invalidate(b); /* clears R20 pacing + R22 stepwise */
#if defined(PLUTO_SPLIT_SIM_FRAMES) && !defined(TARGET_PLAYDATE)
            logger_log("[js] bundle %d: split run complete: %d ok, %d failed "
                       "segs (aborted; max atomic run %lums — the mount "
                       "call the device dog sees)", index, st->split.okSegs,
                       st->split.failSegs, g_splitMaxRunMs);
#else
            logger_log("[js] bundle %d split run complete: %d ok, %d failed "
                       "segs (aborted)", index, st->split.okSegs,
                       st->split.failSegs);
#endif
            if (st->split.failSegs > 0 && !b->lastError[0])
            {
                snprintf(b->lastError, sizeof(b->lastError),
                         "bundle %d: %d segment failures", index,
                         st->split.failSegs);
            }
            return 1;
        }
        int segErr = (b->errs > errs0);
        if (segErr)
            st->split.failSegs++;
        else
            st->split.okSegs++;
        /* R20b/R26f tail dependency rule: the tail's later slices routinely
         * bind identifiers the earlier slices created (minified single-
         * letter top-level names), so a failed tail slice invalidates
         * every remaining slice — ReferenceErrors there would be PURE
         * NOISE and a per-slice abort risk on a 180MHz board. Module-map
         * entries (seg < tailIndex) are independent assignments — the
         * loop continues normally over a failed entry. R26f: no
         * tailCount>1 gate — the single-tail fallback must also drop. */
        if (segErr && st->split.plan.tailIndex >= 0 &&
            seg >= st->split.plan.tailIndex)
        {
            int remaining = lastSeg - seg;
            st->split.failSegs += remaining;
            st->split.nextSeg = lastSeg + 1; /* loop exits */
            logger_log("[js] bundle %d: tail slice %d/%d failed — skipping "
                       "remaining %d tail slices (ordered dependency)",
                       index, seg - st->split.plan.tailIndex + 1,
                       st->split.plan.tailCount, remaining);
        }
#if defined(TARGET_PLAYDATE) || defined(PLUTO_SPLIT_SIM_FRAMES)
        /* Budget check BETWEEN segments (never inside one — a segment is
         * atomic; admission bounds its length). SDK ms clock wraps at
         * ~49.7 days: wrap-safe via unsigned subtraction. R26q: the sim
         * demo seam refreshes the same clock from the host wall clock so
         * the pump really yields between frames there too. */
#ifdef TARGET_PLAYDATE
        now = (unsigned long)pd->system->getCurrentTimeMilliseconds();
#else
        {
            struct timespec ts2;
            clock_gettime(CLOCK_MONOTONIC, &ts2);
            now = (unsigned long)(ts2.tv_sec * 1000UL + ts2.tv_nsec / 1000000UL);
        }
#endif
        if ((now - st->split.deadlineMs) < 0x80000000UL &&
            (long)(now - st->split.deadlineMs) >= 0)
        {
            /* Frame slice consumed: yield to the SDK run loop NOW. */
            if (st->step.active)
                return 0; /* stepwise session in flight — resume next frame */
            return 0;
        }
#endif
    }
    /* Plan exhausted — the split is done. */
    /* The R20 split is over (or never started) — any deferred source is
     * stale. Keeping a heap copy would hold the entire document hostage
     * to one stuck segment (S4, PLUTO-04); the deferred-retry path
     * re-offers the script from the router's own disk handle instead. */
    xs_split_invalidate(b);
    /* R27: report the eval completion as an ENGINE MUTATION — the frame
     * pump (main.c js_timers_update) detects it via callBudget consuming
     * one unit, then re-renders through page_rewalk_now, which re-walks
     * the POST-JS DOM and lands in page_swap_doc with eval_pending()==0,
     * snapshotting the real page (device run 5 snapshotted the pre-React
     * DOM because nothing told the pipeline the eval was still running). */
    if (b->callBudget > 0)
    {
        b->callBudget--;
    }
#if defined(PLUTO_SPLIT_SIM_FRAMES) && !defined(TARGET_PLAYDATE)
        logger_log("[js] bundle %d: split run complete: %d ok, %d failed segs "
                   "(max atomic run %lums — the mount call the device dog "
                   "sees)", index, st->split.okSegs, st->split.failSegs,
                   g_splitMaxRunMs);
#else
        logger_log("[js] bundle %d split run complete: %d ok, %d failed segs",
                   index, st->split.okSegs, st->split.failSegs);
#endif
    xs_progress_hint = -1; /* R26m: bar full during post-eval render mount */
    if (st->split.failSegs > 0 && !b->lastError[0])
    {
        snprintf(b->lastError, sizeof(b->lastError),
                 "bundle %d: %d segment failures", index,
                 st->split.failSegs);
    }
    return 1;
}

/* R20: per-frame resume hook (engine vtable `pump`). When a bundler
 * split is mid-flight, run the next time slice; when one of those
 * segments re-renders through page_rewalk_now, it re-enters scripts —
 * that re-entry arrives here through the normal run_script paths and
 * must not clobber the in-flight session (recursion guard below). */
static void xs_pump_split(JsBridge *b, unsigned nowMs)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
        return;
    if (!st->split.active && !st->step.active)
        return;
    (void)nowMs;
    (void)xs_split_pump(b, 0);
}

/* ── API-P3: journal delivery drain ────────────────────────────────────
 * back/forward/go latch pending popstate/hashchange deliveries; this
 * runs them at the NEXT engine-pump frame (post-run, never mid-eval),
 * mirroring the xhr-completion pump: metered, per-listener xsTry
 * isolation, contained aborts. popstate/hashchange are WINDOW lanes —
 * document/window registrations round into the same listener book with
 * target = the doc root; element-registered ones never fire (browser-
 * faithful). Event objects: popstate = {type, state, preventDefault,
 * stopPropagation} (state = the new entry's pinned value, snapshotted
 * before the walk — a handler may push further entries); hashchange =
 * {type, oldURL, newURL} built from the entry URLs recorded at move
 * time. The preventDef/stopProp latches are unread outside dispatches
 * and both reset at every dispatch entry, so the shared event-object
 * methods stay harmless here. */
static void xs_hist_call_listener(JsBridge *b, XsState *st,
                                  const char *what, int i, xsSlot stateVal,
                                  const char *oldUrl, const char *newUrl)
{
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                xsSlot ev = xsNewHostObject(NULL);
                xsResult = ev; /* GC anchor */
                xsDefine(xsResult, xsID("type"), xsString(what), xsDefault);
                if (oldUrl == NULL)
                {
                    /* popstate payload: stateVal is remembered (pinned),
                     * so funneling it through the bracket is GC-safe */
                    xsDefine(xsResult, xsID("state"), stateVal, xsDefault);
                }
                else
                {
                    /* hashchange payload */
                    xsDefine(xsResult, xsID("oldURL"), xsString(oldUrl), xsDefault);
                    xsDefine(xsResult, xsID("newURL"), xsString(newUrl), xsDefault);
                }
                xsDefine(xsResult, xsID("preventDefault"),
                         xsNewHostFunction(xs_event_preventDefault, 0),
                         xsDefault);
                xsDefine(xsResult, xsID("stopPropagation"),
                         xsNewHostFunction(xs_event_stopPropagation, 0),
                         xsDefault);
                xsVar(0) = ev;
                xsCall2_noResult(st->fns[i], xsID("call"), xsGlobal, xsVar(0));
                xs_drain_jobs(the);
                xs_flush_microtasks(b); /* microcheckpoint boundary */
            }
            xsCatch
            {
                b->errs++;
                bridge_take_error_text(b, "listener exception");
                logger_log("[js] %s handler failed", what);
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);

    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (m->exitStatus != xsNormalExit)
    {
        b->errs++;
        bridge_take_error_text(b, "engine abort in listener");
        logger_log("[js] %s dispatch aborted engine — engine reset", what);
        st->machine = NULL;
        xsDeleteMachine(m);
        xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
        pluto_mem_resync_live();
    }
}

static void xs_hist_deliver_pending(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    if (b->inXhr || b->inTimer || b->inClick || jsbridge_mount_parked(b))
    {
        return; /* someone owns the machine — try again next frame */
    }
    if (!st->histPendPop && !st->histPendHash)
    {
        return;
    }
    xsMachine *m = st->machine;
    /* null outside any host bracket: fxNull fills the slot directly
     * (the xsNull macro needs the bracket-local `the`) */
    xsSlot nullSlot;
    fxNull(m, &nullSlot);

    /* popstate first (browser order: entry state, then the hash lane) */
    if (st->histPendPop)
    {
        st->histPendPop = 0;
        xsSlot stateVal = nullSlot;
        if (b->histCount > 0 && b->histIdx < JSBRIDGE_HIST_MAX &&
            st->histStateSet[b->histIdx])
        {
            stateVal = st->histState[b->histIdx];
        }
        int fires = 0;
        for (int i = 0; i < b->listenerCount && st->machine; i++)
        {
            JsListener *L = &b->listeners[i];
            if (L->kind != 5 || strcmp(L->type, "popstate") != 0)
            {
                continue;
            }
            if (L->target != (b->dom ? (const void *)b->dom->root : NULL))
            {
                continue; /* element popstate never fires (window lane) */
            }
            int slot = (int)(intptr_t)L->ref;
            if (slot < 0 || slot >= JSBRIDGE_LISTENERS_MAX)
            {
                continue;
            }
            fires++;
            xs_hist_call_listener(b, st, "popstate", i, stateVal, NULL, NULL);
        }
        if (fires)
        {
            logger_log("[js] history delivery: popstate fires=%d j=%d",
                       fires, b->histIdx);
        }
    }
    /* then hashchange (only when the entry hash actually moved) */
    if (st->machine && st->histPendHash)
    {
        st->histPendHash = 0;
        int fires = 0;
        for (int i = 0; i < b->listenerCount && st->machine; i++)
        {
            JsListener *L = &b->listeners[i];
            if (L->kind != 5 || strcmp(L->type, "hashchange") != 0)
            {
                continue;
            }
            if (L->target != (b->dom ? (const void *)b->dom->root : NULL))
            {
                continue;
            }
            int slot = (int)(intptr_t)L->ref;
            if (slot < 0 || slot >= JSBRIDGE_LISTENERS_MAX)
            {
                continue;
            }
            fires++;
            xs_hist_call_listener(b, st, "hashchange", i, nullSlot,
                                  st->histFromUrl, st->histToUrl);
        }
        if (fires)
        {
            logger_log("[js] history delivery: hashchange fires=%d "
                       "'%.40s' → '%.40s'",
                       fires, st->histFromUrl, st->histToUrl);
        }
    }
}

/* API-P3 pump wrapper (engine vtable `pump`): the split resume work
 * first, then the journal delivery drain (popstate/hashchange latched
 * by back/forward/go). Riding the existing vtable slot means no new
 * per-frame plumbing in the router — jsbridge_engine_pump already runs
 * impl->pump every frame outside any run. */
static void xs_engine_pump(JsBridge *b, unsigned nowMs)
{
    xs_pump_split(b, nowMs);
    xs_hist_deliver_pending(b);
}

/* R18 core (materialized source): plan + split-run a bundler monolith
 * whose bytes are ALREADY in RAM — either freshly read from a spill
 * handle (stream path) or the inline <script> body itself (inline path;
 * a read-only span followed by '</script>' in the page buffer, which is
 * safe because the planner and emitter are strictly len-bounded).
 * Returns 1 = handled (split attempted), 0 = not a bundler monolith
 * (caller falls through to its own paths untouched). */
static int xs_try_bundler_split_src(JsBridge *b, const char *src, size_t len,
                                    int index)
{
    if (len <= PLUTO_BUNDLER_MIN_SOURCE || len > JSBRIDGE_MAX_SCRIPT_SOURCE)
        return 0;
    PlutoBundlerSpan *spans = (PlutoBundlerSpan *)JMalloc(
        sizeof(PlutoBundlerSpan) * PLUTO_BUNDLER_MAX_SPANS);
    if (!spans)
        return 0;
    PlutoBundlerPlan plan;
    plan.spans = spans;
    plan.maxSpans = PLUTO_BUNDLER_MAX_SPANS;
    int handled = 0;
    if (jsbridge_bundler_plan(src, len, &plan) == PLUTO_BUNDLER_OK)
    {
        logger_log("[js] script %d: bundler monolith (%zuB, %d modules) — "
                   "split execution", index, len, plan.entries);
        b->ran++;
        handled = xs_run_script_split(b, src, len, index, &plan);
    }
    JFree(spans);
    return handled;
}

/* R18 (stream path): materialize the disk-resident body once, then plan
 * + split from RAM (same core as the inline path). */
static int xs_try_bundler_split(JsBridge *b, SpillFile spill, size_t len,
                                int index)
{
    if (len <= PLUTO_BUNDLER_MIN_SOURCE || len > JSBRIDGE_MAX_SCRIPT_SOURCE)
        return 0;
    char *src = (char *)JMalloc(len + 1);
    if (!src)
        return 0;
    if (pluto_spill_read(spill, 0, src, len) != (long)len)
    {
        JFree(src);
        return 0;
    }
    src[len] = '\0';
    int handled = xs_try_bundler_split_src(b, src, len, index);
    JFree(src);
    return handled;
}

/* R15: streaming run — parse + execute a DISK-RESIDENT script body straight
 * off its spill handle. The source NEVER lives in RAM; the lexer pulls
 * characters through the sliding-window getter (2-char lookahead-safe), so
 * a 500KB page script costs ~1KB of read window instead of a 504KB
 * materialize buffer + a 506KB concat copy. R16: when the whole file cannot
 * be admitted but its top-level statements can, the chunked path compiles
 * it in segments (see xs_run_script_chunked above). */
static void xs_run_script_stream(JsBridge *b, SpillFile spill, size_t len,
                                 int index)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    if (st->split.active || st->step.active)
    {
        /* R22 re-entry guard (see xs_run_script). */
        logger_log("[js] script %d (stream) skipped: bundle split still "
                   "running",
                   index);
        return;
    }
    /* C4: same one-shot shim injection as xs_run_script (the stream path
     * bypasses it entirely). */
    if (!st->fetchShimDone)
    {
        xs_run_fetch_shim(b, st, index);
    }
    if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
    {
        xs_fail_script(b, index, "script too large");
        return;
    }

#ifdef PLUTO_CHUNK_FORCE
    /* Test seam (sim only, like PLUTO_NR_AUTOTEST): always try the split
     * then chunked paths first so the segmentation itself is exercisable
     * where the host memory probe would always grant the whole-file
     * compile. */
    if (xs_try_bundler_split(b, spill, len, index))
        return;
    if (xs_run_script_chunked(b, spill, len, index))
    {
        b->ran++;
        return;
    }
    /* fall through: normal whole-file path */
#endif

    /* R60: a disk-resident body over the one-frame parse cap gets the
     * contained routes instead of a monolith admission:
     *   DEVICE — whole-file admission is impossible (the lexer cannot lex
     *   64KB+ unyielded; the late R54 cap would refuse it): bundler split,
     *   statement chunker, then a contained skip (never a retry livelock).
     *   SIM — whole-file admission stays valid while the probe grants the
     *   compile (the contact page's 112KB react chunk whole-file-runs on
     *   the host heap and MUST keep doing so — the R60 regression guard):
     *   only a probe refusal (memwatch envelope test builds) diverts to
     *   splitter → chunker → deferred retry, same containment story. */
    if (len >= (size_t)JSBRIDGE_ONE_FRAME_SCRIPT_MAX)
    {
#ifdef TARGET_PLAYDATE
        if (xs_try_bundler_split(b, spill, len, index))
            return;
        if (xs_run_script_chunked(b, spill, len, index))
        {
            b->ran++; /* handled containedly by the chunked path */
            return;
        }
        b->errs++;
        logger_log("[js] script %d skipped (stream): %zuB over the %dKB "
                   "one-frame parse cap and not chunkable",
                   index, len, JSBRIDGE_ONE_FRAME_SCRIPT_MAX >> 10);
        bridge_take_error_text(b, "script over one-frame parse cap");
        return;
#else
        {
            /* Sim: with no budget armed (budget 0 = unrestricted host
             * heap) the whole-file path keeps its exact prior behavior —
             * the contact page's 112KB react chunk whole-file-runs and
             * MUST keep doing so (R60 regression guard, read on the 01:56
             * tap run). With a budget armed (memwatch envelope test
             * builds) the funnel enforces at sim scale too: OS-truth
             * probe, then divert splitter -> chunker -> deferred retry,
             * same containment story as the device. */
            if (pluto_mem_budget() != 0)
            {
                size_t r60need = jsbridge_compile_need((unsigned long)len);
                if (pluto_mem_probe_grantable((unsigned long)r60need) <
                    r60need)
                {
                    if (xs_try_bundler_split(b, spill, len, index))
                        return;
                    if (xs_run_script_chunked(b, spill, len, index))
                    {
                        b->ran++;
                        return;
                    }
                    logger_log("[js] script %d deferred (stream): %zuB over "
                               "the %dKB one-frame parse cap, probe "
                               "refused, not chunkable",
                               index, len,
                               JSBRIDGE_ONE_FRAME_SCRIPT_MAX >> 10);
                    jsbridge_deferred_offer(b, JS_ENGINE_XS_NR, spill, len);
                    b->errs++;
                    return;
                }
            }
        }
        /* fall through: probe granted the whole-file compile — run it */
#endif
    }

#ifdef TARGET_PLAYDATE
    /* Early admission check BEFORE any machine work: if the compile's
     * estimated peak isn't grantable, defer to the router (retry per frame
     * as memory frees) instead of skipping permanently. R16: before
     * deferring, try the chunked path — if the file is a statement
     * sequence whose largest segment fits, it can run NOW without waiting
     * for the whole-file footprint to become grantable. */
    {
        size_t earlyNeed = jsbridge_compile_need((unsigned long)len);
        if (xs_probe_grantable(earlyNeed) < earlyNeed)
        {
            /* R18 before R16: a bundler monolith is ONE statement, so the
             * statement chunker can't help — the module-map splitter can. */
            if (xs_try_bundler_split(b, spill, len, index))
                return;
            if (xs_run_script_chunked(b, spill, len, index))
            {
                b->ran++; /* handled containedly by the chunked path */
                return;
            }
            logger_log("[js] script %d deferred (stream): probe < need %zuKB, "
                       "live %luKB",
                       index, earlyNeed >> 10, pluto_mem_live() >> 10);
            b->errs++;
            jsbridge_deferred_offer(b, JS_ENGINE_XS_NR, spill, len);
            return;
        }
    }
#endif

#ifdef TARGET_PLAYDATE
    /* R54 belt+braces: the deferred retry lands HERE when memory frees —
     * straight into this whole-file bracket on ONE update() frame. The
     * device lexer costs ~1s per ~30KB of minified JS; the run-loop
     * watchdog hard-crashes (E1) past 10s (2026-10-18 log showed the
     * 112594B react chunk would one day breach it via exactly this
     * path). Over the cap: skip containedly; giant bundles belong on the
     * planner/split path. */
    if (len >= (size_t)JSBRIDGE_ONE_FRAME_SCRIPT_MAX)
    {
        b->errs++;
        logger_log("[js] script %d skipped (stream): %zuB monolith over "
                   "the %dKB one-frame parse cap (device cannot lex it "
                   "unyielded)",
                   index, len, JSBRIDGE_ONE_FRAME_SCRIPT_MAX >> 10);
        bridge_take_error_text(b, "script over one-frame parse cap");
        return;
    }
#endif

    b->ran++;

    const char *prefix = NULL;
    size_t plen = jsbridge_sw5_prefix(&prefix);

    /* SW5 prefix as its own tiny program (identical global machine). */
    if (plen && xs_run_tiny(b, st->machine, index, prefix, plen, "sw5 prefix"))
    {
        return;
    }

    xsMachine *m = st->machine;
    if (!m)
    {
        return;
    }
    xsStreamSrc ssrc;
    memset(&ssrc, 0, sizeof(ssrc));
    ssrc.spill = spill;
    ssrc.total = len;
    /* Prime the window once here; the getter slides it from then on. */
    size_t want = sizeof(ssrc.window);
    if (want > len)
    {
        want = len;
    }
    long gotR = pluto_spill_read(spill, 0, ssrc.window, want);
    if (gotR <= 0)
    {
        logger_log("[js] script %d failed: stream read failed", index);
        bridge_take_error_text(b, "stream read failed");
        b->errs++;
        return;
    }
    ssrc.filled = (size_t)gotR;

    /* Admission hint mirrors the RAM path (shared R15 knobs). The early
     * check above already gated the common denial; this re-probe inside
     * the core covers the between-checks window (cheap, and contained). */
#ifdef TARGET_PLAYDATE
    size_t execPeak = jsbridge_compile_need((unsigned long)len);
#else
    size_t execPeak = 0;
#endif
    xs_parse_and_run(b, m, index, &ssrc, xs_stream_getter, len, execPeak,
                     "script");
}

static int xs_dispatch_click(JsBridge *b, const void *anchorNode)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return JSB_CLICK_NONE;
    }
    int fired = 0;
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        if (L->target != anchorNode || L->kind != 0) /* clicks only */
        {
            continue;
        }
        fired = 1;
        b->preventDef = 0;
        b->inClick = 1;

        xsMachine *m = st->machine;
        xsBeginHostExit(m);
        xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
        {
            xsBeginHost(m);
            {
                xsVars(1);
                xsTry
                {
                    /* fresh wrapper for the target + event object */
                    xsVar(0) = xs_push_element(b, (DomNode *)L->target);
                    xsSlot ev = xsNewHostObject(NULL);
                    xsResult = ev; /* GC anchor: preventDefault fn allocates */
                    xsDefine(xsResult, xsID("type"), xsString("click"), xsDefault);
                    xsDefine(xsResult, xsID("preventDefault"),
                             xsNewHostFunction(xs_event_preventDefault, 0),
                             xsDefault);
                    xsCall2_noResult(st->fns[i], xsID("call"), xsVar(0), ev);
                    xs_drain_jobs(the);
                    xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
                }
                xsCatch
                {
                    b->errs++;
                    bridge_take_error_text(b, "listener exception");
                    logger_log("[js] click handler failed");
                }
            }
            xsEndHost(m);
        }
        xsEndMetering(m);
        xsEndHostExit(m);

        b->inClick = 0;
        b->doc->jsErrors = b->errs;
        snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                 b->lastError);
        if (m->exitStatus != xsNormalExit)
        {
            b->errs++;
            bridge_take_error_text(b, "engine abort in click handler");
            logger_log("[js] click dispatch aborted engine — engine reset");
            st->machine = NULL;
            xsDeleteMachine(m);
            xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
            pluto_mem_resync_live(); /* counter re-base (see run_script) */
            return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
        }
        if (b->preventDef)
        {
            return JSB_CLICK_SUPPRESSED;
        }
    }
    return fired ? JSB_CLICK_NAVIGATE : JSB_CLICK_NONE;
}

/* ── B3: element-event dispatch with browser bubble semantics ──────────────
 * `node` is the event target: listeners registered ON it fire first (browser
 * order), then listeners on each ancestor up the tree — react's delegation
 * registers at the root container, so the climb is what delivers. Only the
 * page-interactivity class (kind 0 = click/submit/input/change) answers;
 * load/error resources run through the resource fire instead. Event object
 * follows the click template (target REQUIRED — react's non-delegated
 * dispatcher reads event.target) plus currentTarget per ancestor,
 * cancelable/bubbles/defaultPrevented, and stopPropagation. Any
 * preventDefault() suppresses the caller's default action (sticky: later
 * listeners still run, browser order), and stopPropagation() stops the
 * climb (same-node listeners still run, browser order). Per-listener xsTry
 * isolates faults exactly like the click path. Returns a
 * JsBridgeEventResult. */
static int xs_dispatch_element_event(JsBridge *b, const void *node,
                                     const char *type)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !node || !type)
    {
        return JSB_EVENT_NONE;
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    {
        DomNode *tn = (DomNode *)node;
        logger_log("[dev] type=%s recv=%s", type,
                   (tn->kind == DOM_ELEMENT && tn->tag) ? tn->tag : "#other");
    }
#endif
    int fired = 0;
    b->preventDef = 0; /* sticky: an early preventDefault wins, browser-style */
    b->stopProp = 0;
    b->inClick = 1; /* same re-entrancy guard as dispatch_click */
    xsMachine *m = st->machine;
    /* Phase 10: the delivery class follows the dispatched type — the
     * page-interactivity class (kind 0) for the react delegation set,
     * the end-event class (kind 4) for animationend/transitionend,
     * Forms-Phase 3: the non-bubbling class (kind 6) for `invalid` —
     * delivered target-only (browser-true: invalid does not bubble),
     * so the climb below ends after the target node. */
    int evKind = (strcmp(type, "animationend") == 0 ||
                  strcmp(type, "transitionend") == 0)
                     ? 4
                     : (strcmp(type, "invalid") == 0)
                           ? 6
                           : 0;
    int noClimb = (evKind == 6);

    DomNode *t = (DomNode *)node;
    while (t)
    {
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
        {
            int m0 = 0;
            for (int i = 0; i < b->listenerCount; i++)
            {
                if (b->listeners[i].target == t &&
                    !strcmp(b->listeners[i].type, type))
                {
                    m0++;
                }
            }
            logger_log("[devb] step t=%s type=%s pairs=%d total=%d",
                       (t->kind == DOM_ELEMENT && t->tag) ? t->tag
                                                           : "#other",
                       type, m0, b->listenerCount);
        }
#endif
        for (int i = 0; i < b->listenerCount; i++)
        {
            JsListener *L = &b->listeners[i];
            if (L->target != t || L->kind != evKind || // delivery class
                strcmp(L->type, type) != 0)            /* B3: exact type route */
            {
                continue;
            }
            fired = 1;
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
            logger_log("[dev] fire i=%d tgt=%s type=%s", i,
                       (t->kind == DOM_ELEMENT && t->tag) ? t->tag : "#other",
                       type);
#endif

            xsBeginHostExit(m);
            xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
            {
                xsBeginHost(m);
                {
                    xsVars(2);
                    xsTry
                    {
                        /* fresh wrappers: the ORIGINAL target (0) + the
                         * listening node (1) + the event object */
                        xsVar(0) = xs_push_element(b, (DomNode *)node);
                        xsVar(1) = xs_push_element(b, t);
                        xsSlot ev = xsNewHostObject(NULL);
                        xsResult = ev; /* GC anchor while defining */
                        xsDefine(xsResult, xsID("type"), xsString(type),
                                 xsDefault);
                        xsDefine(xsResult, xsID("target"), xsVar(0), xsDefault);
                        xsDefine(xsResult, xsID("currentTarget"), xsVar(1),
                                 xsDefault);
                        xsDefine(xsResult, xsID("cancelable"), xsTrue,
                                 xsDefault);
                        /* kind 6 classes (invalid) report bubbles:false,
                         * browser-true for the non-bubbling event. */
                        xsDefine(xsResult, xsID("bubbles"),
                                 noClimb ? xsFalse : xsTrue, xsDefault);
                        xsDefine(xsResult, xsID("defaultPrevented"), xsFalse,
                                 xsDefault);
                        xsDefine(xsResult, xsID("preventDefault"),
                                 xsNewHostFunction(xs_event_preventDefault, 0),
                                 xsDefault);
                        xsDefine(xsResult, xsID("stopPropagation"),
                                 xsNewHostFunction(xs_event_stopPropagation, 0),
                                 xsDefault);
                        xsCall2_noResult(st->fns[i], xsID("call"), xsVar(1),
                                         ev);
                        xs_drain_jobs(the);
                        xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
                    }
                    xsCatch
                    {
                        b->errs++;
                        bridge_take_error_text(b, "listener exception");
                        logger_log("[js] %s handler failed", type);
                    }
                }
                xsEndHost(m);
            }
            xsEndMetering(m);
            xsEndHostExit(m);

            b->doc->jsErrors = b->errs;
            snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                     b->lastError);
            if (m->exitStatus != xsNormalExit)
            {
                b->errs++;
                bridge_take_error_text(b, "engine abort in event handler");
                logger_log("[js] %s dispatch aborted engine — engine reset",
                           type);
                st->machine = NULL;
                xsDeleteMachine(m);
                xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
                pluto_mem_resync_live(); /* counter re-base (see run_script) */
                return fired ? JSB_EVENT_NAVIGATE : JSB_EVENT_NONE;
            }
        }
        if (b->stopProp)
        {
            break; /* browser semantics: same-node listeners ran, climb ends */
        }
        if (noClimb)
        {
            break; /* kind 6 classes (invalid) never bubble */
        }
        t = t->parent; /* bubbling: target → ancestors */
    }

    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (!fired)
    {
        return JSB_EVENT_NONE;
    }
    return b->preventDef ? JSB_EVENT_SUPPRESSED : JSB_EVENT_NAVIGATE;
}

/* Forms-Phase 1: the NON-bubbling focus/blur dispatcher — the no-climb
 * sibling of the B3 dispatcher above. Browser truth: focus and blur do
 * NOT bubble (focusin/focusout do), so delivery targets ONLY kind-6
 * listeners of the exact type on the given node — no ancestor walk, no
 * stopPropagation consultation, nothing else on the tree can see it.
 * Event object matches the observable FocusEvent surface: type/target/
 * currentTarget + bubbles:false/cancelable:false (neither event is
 * cancelable per HTML; preventDefault is provided for shape parity but
 * has no effect here). Per-listener metering/host brackets mirror the
 * B3 dispatcher exactly; the tap path never routes through here. */
static int xs_dispatch_focus_event(JsBridge *b, DomNode *node,
                                   const char *type)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !node || !type)
    {
        return JSB_EVENT_NONE;
    }
    int fired = 0;
    b->preventDef = 0;
    b->stopProp = 0;
    b->inClick = 1; /* same re-entrancy guard as the B3 dispatcher */
    xsMachine *m = st->machine;
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        if (L->target != node || L->kind != 6 || /* focus/blur class */
            strcmp(L->type, type) != 0)          /* exact type route */
        {
            continue;
        }
        fired = 1;
        xsBeginHostExit(m);
        xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
        {
            xsBeginHost(m);
            {
                xsVars(1);
                xsTry
                {
                    /* fresh wrapper: the focused target (0) + event */
                    xsVar(0) = xs_push_element(b, node);
                    xsSlot ev = xsNewHostObject(NULL);
                    xsResult = ev; /* GC anchor while defining */
                    xsDefine(xsResult, xsID("type"), xsString(type),
                             xsDefault);
                    xsDefine(xsResult, xsID("target"), xsVar(0), xsDefault);
                    xsDefine(xsResult, xsID("currentTarget"), xsVar(0),
                             xsDefault);
                    xsDefine(xsResult, xsID("cancelable"), xsFalse,
                             xsDefault);
                    xsDefine(xsResult, xsID("bubbles"), xsFalse, xsDefault);
                    xsDefine(xsResult, xsID("defaultPrevented"), xsFalse,
                             xsDefault);
                    xsDefine(xsResult, xsID("preventDefault"),
                             xsNewHostFunction(xs_event_preventDefault, 0),
                             xsDefault);
                    xsDefine(xsResult, xsID("stopPropagation"),
                             xsNewHostFunction(xs_event_stopPropagation, 0),
                             xsDefault);
                    xsCall2_noResult(st->fns[i], xsID("call"), xsVar(0),
                                     ev);
                    xs_drain_jobs(the);
                    xs_flush_microtasks(b); /* A2: microcheckpoint boundary */
                }
                xsCatch
                {
                    b->errs++;
                    bridge_take_error_text(b, "listener exception");
                    logger_log("[js] %s handler failed", type);
                }
            }
            xsEndHost(m);
        }
        xsEndMetering(m);
        xsEndHostExit(m);

        b->doc->jsErrors = b->errs;
        snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
                 b->lastError);
        if (m->exitStatus != xsNormalExit)
        {
            b->errs++;
            bridge_take_error_text(b, "engine abort in event handler");
            logger_log("[js] %s dispatch aborted engine — engine reset",
                       type);
            st->machine = NULL;
            xsDeleteMachine(m);
            xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
            pluto_mem_resync_live(); /* counter re-base (see run_script) */
            return fired ? JSB_EVENT_NAVIGATE : JSB_EVENT_NONE;
        }
    }

    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (!fired)
    {
        return JSB_EVENT_NONE;
    }
    /* NOT suppressible: focus/blur carry cancelable:false, so delivery
     * is always "handlers ran" — there is no native action to suppress
     * at this layer (the keyboard lane is native and untouched). */
    return JSB_EVENT_NAVIGATE;
}

/* Timer vtable: release one pinned callback (fnRef = tfn[] slot). */
static void xs_clear_timer_ref(JsBridge *b, void *fnRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return;
    }
    int slot = (int)(intptr_t)fnRef;
    if (slot < 0 || slot >= JSBRIDGE_TIMERS_MAX || !st->tfnSet[slot])
    {
        return;
    }
    xsBeginHostExit(st->machine);
    xsForget(st->tfn[slot]);
    xsEndHostExit(st->machine);
    st->tfnSet[slot] = 0;
}

/* Invoke one pinned callback. Returns 0 ok, 1 contained error, -1 abort. */
/* C4 (async fidelity): interleaved microcheckpoint. The two job queues
 * feed each other — a native-promise continuation can call
 * queueMicrotask (React's setState flush does exactly that: our drain
 * ran the await resumption, setState landed React's flush callback in
 * the injected ring, and the drain→flush→drain pass left it STRANDED
 * until an unrelated timer happened to fire 47s later). Alternate
 * native-jobs and injected-ring drains until BOTH are empty, bounded
 * like a browser's microtask checkpoint budget. */
static void xs_boundary_microtasks(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    for (int pass = 0; pass < 16; pass++)
    {
        xs_drain_jobs(st->machine);
        if (xs_flush_microtasks(b) == 0)
        {
            return; /* both queues empty (the drain ran first) */
        }
    }
    logger_log("[drain] boundary cap 16 passes");
}

static int xs_run_timer_ref(JsBridge *b, void *fnRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return 1;
    }
#ifdef PLUTO_NR_MOUNT_PAUSE
    if (st->mount.active && st->mount.parked)
    {
        /* A parked mount owns the machine: its frames sit mid-stack and
         * machine registers hold the parked state. A timer callback here
         * would run concurrently with a half-mounted React tree — skip;
         * the mount resumes on this very frame via the engine pump. */
        return 1;
    }
#endif
    int slot = (int)(intptr_t)fnRef;
    if (slot < 0 || slot >= JSBRIDGE_TIMERS_MAX || !st->tfnSet[slot])
    {
        return 1;
    }
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            xsTry
            {
                xsVar(0) = st->tfn[slot];
                xsCall0_noResult(xsVar(0), xsID("call"));
            }
            xsCatch
            {
                b->errs++;
                bridge_take_error_text(b, "timer callback exception");
                logger_log("[js] timer handler failed");
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (m->exitStatus != xsNormalExit)
    {
        b->errs++;
        bridge_take_error_text(b, "engine abort in timer callback");
        logger_log("[js] timer dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    /* C4 (async fidelity): match the other machine boundaries — a
     * timer callback that settles native promises (async/await
     * continuations, framework schedulers) must have those jobs run
     * BEFORE the next tick, exactly like the run/script/click paths'
     * xs_drain_jobs call sites — AND the queues interleave (see the
     * helper's note): promise continuations queue ring callbacks and
     * ring callbacks queue promise jobs. */
    xs_boundary_microtasks(b);
    return 0;
}

/* ── A2: flush the microtask queue FLAT ─────────────────────────────
 * One batch per boundary: each callback STARTS only after the previous
 * one RETURNED (browser microcheckpoint). Callbacks enqueued while the
 * batch runs continue in this same drain — flat, queue-ordered — so the
 * stack never accumulates; the per-flush cap exists for device appetite
 * and defers the tail to the next machine boundary (logged). Called at
 * the promise-job drain boundaries (script-run ends; after each timer /
 * XHR callback fire) so microtasks precede the next timer, like a
 * browser. Returns run count, or -1 on engine abort. */
static int xs_flush_microtasks(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || st->mtCount == 0)
    {
        return 0;
    }
#ifdef PLUTO_NR_MOUNT_PAUSE
    if (st->mount.active && st->mount.parked)
    {
        return 0; /* parked mount owns the machine (same rule as timers) */
    }
#endif
    xsMachine *m = st->machine;
    int run = 0;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(1);
            /* Ring dequeue: front index NEVER shifts; entries are pinned
             * exactly once at enqueue and forgotten exactly once here (a
             * struct copy of a pinned node corrupts the cRoot chain — the
             * fork's fxRemember/fxForget use the slot struct itself as the
             * chain node). The head advances only AFTER the callback and
             * its unpin, so a callback's own enqueue always lands on a
             * free tail slot — this drained entry still counts while it
             * runs. Callbacks enqueued mid-drain join the flat queue,
             * like a browser's microcheckpoint. */
            while (st->mtCount > 0 && run < 256)
            {
                int idx = st->mtHead;
                run++;
                /* A2: each microtask callback is one browser microtask —
                 * a fresh meter window per callback. Without this, ONE
                 * flush call accumulates the boot's whole async fall-out
                 * (~2500 callbacks, ~21M bytecodes) against the flush's
                 * single window and trips the source-scaled abort
                 * MID-BOOT (the stock window is cumulative from
                 * meterIndex). Same sticky-pause guards as the fork's
                 * per-job re-arm: mount bursts and split bursts keep
                 * their own pause contracts. A single runaway callback
                 * still aborts inside ITS window — CONTAINED by this
                 * xsCatch (job-level failure, drain continues). */
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
                if ((run & 15) == 1)
                {
                    logger_log("[mt0] run=%d idx=%d interval=%lu count=%lu",
                               run, idx, (unsigned long)m->meterInterval,
                               (unsigned long)(m->meterIndex >> 16));
                }
#endif
                if (m->meterInterval && !m->nrPauseRequest &&
                    !m->nrPauseAbort)
                {
                    m->meterIndex = 0;
                    m->meterCount = m->meterInterval;
                }
                xsTry
                {
                    xsVar(0) = st->mtQ[idx]; /* var pins beyond the unpin */
                    xsCall0_noResult(xsVar(0), xsID("call"));
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
                    /* A2: per-callback count probe — separates "one fat
                     * callback" from "the re-arm is trumped upstream". */
                    if ((run & 7) == 3)
                    {
                        logger_log("[mt] run=%d count=%lu pending=%d",
                                   run,
                                   (unsigned long)(m->meterIndex >> 16),
                                   st->mtCount);
                    }
#endif
                }
                xsCatch
                {
                    b->errs++;
                    bridge_take_error_text(b, "microtask callback exception");
                    logger_log("[js] microtask handler failed");
                }
                xsForget(st->mtQ[idx]); /* unpinned once consumed */
                st->mtHead = (st->mtHead + 1) % XS_MICRO_MAX;
                st->mtCount--;
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (st->mtCount > 0)
    {
        logger_log("[js] microtasks deferred to next boundary (%d left)",
                   st->mtCount);
    }
    if (m->exitStatus != xsNormalExit)
    {
        b->errs++;
        bridge_take_error_text(b, "engine abort in microtask drain");
        logger_log("[js] microtask dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    return run;
}

static void xs_close(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st)
    {
        return;
    }
    if (st->machine)
    {
        xsMachine *m = st->machine;
        xsBeginHostExit(m);
        {
            for (int i = 0; i < b->listenerCount && i < JSBRIDGE_LISTENERS_MAX;
                 i++)
            {
                xsForget(st->fns[i]);
            }
            xsForget(st->elProto);
            xsForget(st->sheetProto);
            xsForget(st->urlProto);
            for (int i = 0; i < JSBRIDGE_TIMERS_MAX; i++)
            {
                if (st->tfnSet[i])
                {
                    xsForget(st->tfn[i]);
                    st->tfnSet[i] = 0;
                }
            }
            for (int i = 0; i < st->mtCount; i++)
            {
                xsForget(st->mtQ[(st->mtHead + i) % XS_MICRO_MAX]);
            }
            st->mtHead = 0;
            st->mtCount = 0;
            /* B3: element wrapper cache pins (one per cached node). */
            for (int i = 0; i < st->elwCount; i++)
            {
                xsForget(st->elwWrap[i]);
            }
            st->elwCount = 0;
            st->elwOverflow = 0;
        }
        xsEndHostExit(m);
        /* R28: invalidate the split/stepwise/mount sessions BEFORE the
         * machine dies. xs_split_invalidate frees a still-owned mount
         * script with fxDeleteScript — between bursts the mount is exactly
         * that (active, not parked, script owned) — and that pointer lives
         * in the engine heap, so the free must run while the machine is
         * alive. Everything else it releases is host memory. */
        xs_split_invalidate(b); /* any in-flight split/stepwise/mount is dead */
        xsDeleteMachine(m); /* frees wrappers, scripts, pinned slots */
        st->machine = NULL;
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
    }
    JFree(st);
    b->implState = NULL;
}


/* ── XMLHttpRequest (async HTTP → JS callbacks) ──────────────�
 * Same thin-glue contract as the other bridges (router owns everything):
 * wrapper = host instance of a rooted prototype (host data = public request
 * id); live state reads are prototype accessors over the router table;
 * onload/onerror/onreadystatechange are pinned at send into xfn[]/xobj[]
 * (xsRemember'd; JsHttpRequest.fnRef = xfn slot, objRef = xobj slot — both
 * indexes, NO +1: slot 0 of the PINS is usable because the router's NULL
 * rule applies to the ref POINTER, and slots here are small ints; encode
 * slot+1 anyway to stay uniform with the other engines). Caps + budgets
 * live in the router (jsbridge.c). No native Promise in XS → no fetch(). */
/* API-P10: dynamic-import bridge native — delegates to the shared core
 * (jsbridge_dyn_request); returns the status STRING the shim parses. */
static void xs_dyn_request(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    const char *spec = xsToString(xsArg(0));
    const char *base = xsToString(xsArg(1));
    char out[JSBRIDGE_MOD_KEY_MAX + 24];
    jsbridge_dyn_request(b, spec ? spec : "", base ? base : "", out,
                         sizeof(out));
    xsResult = xsString(out);
}

static void xs_xhr_open(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
#if defined(PLUTO_HYDRATE_TRACE)
    {
        int ac = xsToInteger(xsArgc);
        logger_log("[xhr] open argc=%d a0.kind=%s", ac,
                   xsTypeOf(xsArg(0)) == xsUndefinedType ? "undef"
                   : (xsTypeOf(xsArg(0)) == xsReferenceType ? "ref"
                                                            : "prim"));
    }
#endif
    const char *method = xsToString(xsArg(0));
    const char *url = xsToString(xsArg(1));
#if defined(PLUTO_HYDRATE_TRACE)
    logger_log("[xhr] open method='%.24s' url='%.60s'", method ? method : "",
               url ? url : "");
#endif
    int id = jsbridge_xhr_open(b, method, url);
    if (id == 0)
    {
        xsTypeError("%s", b->lastError[0] ? b->lastError : "xhr open failed");
    }
    xs_set_host_data_site(the, "xhrOpen", xsThis, (void *)(intptr_t)id);
    xsResult = xsUndefined;
}

static int xs_xhr_id(xsMachine *the)
{
    return (int)(intptr_t)xsGetHostDataIf(xsThis);
}

/* API-P2: xhr.setRequestHeader(name, value) — the fetch shim's header
 * lane (also real-XHR-callable). Content-Type rides the wire on POST;
 * other headers are recorded (documented wire boundary). Invalid states
 * throw with the bridge's error text, browser-faithful ordering. */
static void xs_xhr_set_request_header(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int id = xs_xhr_id(the);
    if (id <= 0)
    {
        xsTypeError("xhr: setRequestHeader before open");
    }
    const char *name = xsToString(xsArg(0));
    const char *value = xsToString(xsArg(1));
    if (!jsbridge_xhr_set_header(b, id, name, value))
    {
        xsTypeError("%s", b->lastError[0] ? b->lastError
                                          : "xhr: header refused");
        b->lastError[0] = '\0';
    }
    xsResult = xsUndefined;
}

static void xs_xhr_send(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    int id = xs_xhr_id(the);
    if (id <= 0)
    {
        xsTypeError("xhr: send before open");
    }
    int fslot = -1, oslot = -1;
    for (int i = 0; i < JSBRIDGE_XHR_MAX; i++)
    {
        if (!st->xfnSet[i] && fslot < 0)
            fslot = i;
        if (!st->xobjSet[i] && oslot < 0)
            oslot = i;
    }
    if (fslot < 0 || oslot < 0)
    {
        xsTypeError("xhr: pin slots full");
    }
    /* Completion handler: onload → onerror → onreadystatechange. */
    static const char *const names[] = {"onload", "onerror",
                                        "onreadystatechange"};
    xsSlot fn;
    int found = 0;
    for (int i = 0; i < 3 && !found; i++)
    {
        xsSlot v = xsGet(xsThis, xsID(names[i]));
        if (fxIsCallable(the, &v))
        {
            fn = v;
            found = 1;
        }
    }
    if (!found)
    {
        xsTypeError("xhr: no onload/onerror/onreadystatechange handler");
    }
    st->xfn[fslot] = fn;
    st->xfnSet[fslot] = 1;
    xsRemember(st->xfn[fslot]);
    st->xobj[oslot] = xsThis;
    st->xobjSet[oslot] = 1;
    xsRemember(st->xobj[oslot]);
    /* C4: optional request body (the fetch shim's POST JSON). Anything
     * that is not undefined coerces to the body string; the router
     * deep-copies it before any other XS call can touch the scratch. */
    const char *reqBody = NULL;
    size_t reqBodyLen = 0;
    if (xsToInteger(xsArgc) > 0 && xsTypeOf(xsArg(0)) != xsUndefinedType)
    {
        reqBody = xsToString(xsArg(0));
        reqBodyLen = strlen(reqBody);
    }
    if (reqBody)
    {
        jsbridge_xhr_send_body(b, id, reqBody, reqBodyLen,
                               (void *)(intptr_t)(fslot + 1),
                               (void *)(intptr_t)(oslot + 1));
    }
    else
    {
        jsbridge_xhr_send(b, id, (void *)(intptr_t)(fslot + 1),
                          (void *)(intptr_t)(oslot + 1));
    }
    xsResult = xsUndefined;
}

static void xs_xhr_abort(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    jsbridge_xhr_abort(b, xs_xhr_id(the));
    xsResult = xsUndefined;
}

static void xs_xhr_get_readyState(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsInteger(r ? r->state : 0);
}

static void xs_xhr_get_status(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsInteger((r && r->state >= JS_XHR_DONE) ? r->status : 0);
}

static void xs_xhr_get_responseText(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsString((r && r->body) ? r->body : "");
}

static void xs_xhr_get_responseURL(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const JsHttpRequest *r = jsbridge_xhr_get(b, xs_xhr_id(the));
    xsResult = xsString((r && r->url[0]) ? r->url : "");
}

static void xs_build_xhr_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);
    st->xhrProto = xsNewHostObject(NULL);
    xsRemember(st->xhrProto); /* root BEFORE the build allocates */
    def_fn(the, st->xhrProto, "open", xs_xhr_open, 2);
    def_fn(the, st->xhrProto, "send", xs_xhr_send, 0);
    def_fn(the, st->xhrProto, "setRequestHeader", xs_xhr_set_request_header,
           2);
    def_fn(the, st->xhrProto, "abort", xs_xhr_abort, 0);
    struct
    {
        const char *name;
        xsCallback get;
    } accs[] = {
        {"readyState", xs_xhr_get_readyState},
        {"status", xs_xhr_get_status},
        {"responseText", xs_xhr_get_responseText},
        {"response", xs_xhr_get_responseText},
        {"responseURL", xs_xhr_get_responseURL},
    };
    for (size_t i = 0; i < sizeof(accs) / sizeof(accs[0]); i++)
    {
        xsSlot getter = xsNewHostFunction(accs[i].get, 0);
        xsDefine(st->xhrProto, xsID(accs[i].name), getter, xsIsGetter);
    }
}

/* API-P8: the Worker contained-stub prototype (methods live here;
 * onerror/onmessage materialize as proto-null values, so a fresh stub
 * reads `null` for both before the page assigns — browser shape). */
static void xs_build_worker_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);
    st->workerProto = xsNewHostObject(NULL);
    xsRemember(st->workerProto); /* root BEFORE the build allocates */
    def_fn(the, st->workerProto, "postMessage", xs_worker_postMessage, 1);
    def_fn(the, st->workerProto, "terminate", xs_worker_terminate, 0);
    def_val(the, st->workerProto, "onerror", xsNull);
    def_val(the, st->workerProto, "onmessage", xsNull);
}

/* Constructor global: new XMLHttpRequest() / XHR() → fresh wrapper. */
static void xs_xmlhttprequest_new(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xsSlot obj = xsNewHostInstance(st->xhrProto);
    xs_set_host_data_site(the, "xhrNew", obj, (void *)(intptr_t)0);
    xsResult = obj;
}

/* ── Forms-Phase 5: FormData (constructor class, host-instance shape) ───
 * Proto-built like the XHR/Worker lane (rooted prototype slots); every
 * instance is a host instance whose host data = the bridge-owned
 * FormDataEntry (the page-long fixed store — see jsbridge_internal.h);
 * iterators are their own host-instance lane with the FormDataIter as
 * host data. The C pair ops all live on the shared core (jsbridge.c) —
 * ONE rulebook feeding both the JS surface and the route-standard POST
 * body. Typed throws use the family xsTypeError (contained). Strings
 * cross the engine boundary via to_cstring (freed after each use). */
static FormDataEntry *xs_formdata_entry(xsMachine *the)
{
    FormDataEntry *e = (FormDataEntry *)xsGetHostDataIf(xsThis);
    return (e && e->used) ? e : NULL;
}

static void xs_formdata_append(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 2)
    {
        xsTypeError("FormData.append: name and value required");
    }
    char *n = to_cstring(the, xsArg(0));
    char *v = to_cstring(the, xsArg(1));
    jsbridge_form_data_append(e, n ? n : "", v ? v : "");
    if (n)
    {
        JFree(n);
    }
    if (v)
    {
        JFree(v);
    }
    xsResult = xsUndefined;
}

static void xs_formdata_get(xsMachine *the)
{
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 1)
    {
        xsTypeError("FormData.get: name required");
    }
    char *n = to_cstring(the, xsArg(0));
    const char *v = jsbridge_form_data_get(e, n ? n : "");
    if (n)
    {
        JFree(n);
    }
    xsResult = v ? xsString(v) : xsNull;
}

static void xs_formdata_getAll(xsMachine *the)
{
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 1)
    {
        xsTypeError("FormData.getAll: name required");
    }
    char *n = to_cstring(the, xsArg(0));
    int hits = 0;
    xsResult = xsNewArray(0);
    for (int i = 0; i < e->npairs; i++)
    {
        if (strcmp(e->pairs[i].name, n ? n : "") == 0)
        {
            xsSetAt(xsResult, xsInteger(hits++),
                    xsString(e->pairs[i].value));
        }
    }
    if (n)
    {
        JFree(n);
    }
}

static void xs_formdata_has(xsMachine *the)
{
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 1)
    {
        xsTypeError("FormData.has: name required");
    }
    char *n = to_cstring(the, xsArg(0));
    int has = jsbridge_form_data_has(e, n ? n : "");
    if (n)
    {
        JFree(n);
    }
    xsResult = has ? xsTrue : xsFalse;
}

static void xs_formdata_set(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 2)
    {
        xsTypeError("FormData.set: name and value required");
    }
    char *n = to_cstring(the, xsArg(0));
    char *v = to_cstring(the, xsArg(1));
    (void)jsbridge_form_data_set(e, n ? n : "", v ? v : "");
    if (n)
    {
        JFree(n);
    }
    if (v)
    {
        JFree(v);
    }
    xsResult = xsUndefined;
}

static void xs_formdata_delete(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = xs_formdata_entry(the);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if ((int)xsToInteger(xsArgc) < 1)
    {
        xsTypeError("FormData.delete: name required");
    }
    char *n = to_cstring(the, xsArg(0));
    int removed = jsbridge_form_data_delete(e, n ? n : "");
    if (n)
    {
        JFree(n);
    }
    xsResult = removed ? xsTrue : xsFalse;
}

/* Shared iterator factory: entries/keys/values walk the same bridge
 * FormDataIter (mode 0/1/2); the result object reads {value, done}. */
static void xs_formdata_walk(xsMachine *the, int mode)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    FormDataEntry *e = xs_formdata_entry(the);
    int full = 0;
    FormDataIter *it;
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    it = jsbridge_form_data_iter(b, e, &full);
    if (!it)
    {
        xsTypeError(full ? "FormData: too many iterators this page"
                         : "FormData: iterator unavailable");
    }
    it->mode = (unsigned char)mode;
    xsResult = xsNewHostInstance(st->fdiProto);
    xs_set_host_data_site(the, "fditer", xsResult, it);
}

static void xs_formdata_entries(xsMachine *the)
{
    xs_formdata_walk(the, 0);
}

static void xs_formdata_keys(xsMachine *the)
{
    xs_formdata_walk(the, 1);
}

static void xs_formdata_values(xsMachine *the)
{
    xs_formdata_walk(the, 2);
}

static void xs_fditer_next(xsMachine *the)
{
    FormDataIter *it = (FormDataIter *)xsGetHostDataIf(xsThis);
    xsResult = xsNewObject();
    if (it && it->entry && it->idx < it->entry->npairs)
    {
        const FdPair *p = &it->entry->pairs[it->idx++];
        if (it->mode == 1)
        {
            xsSet(xsResult, xsID("value"), xsString(p->name));
        }
        else if (it->mode == 2)
        {
            xsSet(xsResult, xsID("value"), xsString(p->value));
        }
        else
        {
            xsSlot arr = xsNewArray(2);
            xsSetAt(arr, xsInteger(0), xsString(p->name));
            xsSetAt(arr, xsInteger(1), xsString(p->value));
            xsSet(xsResult, xsID("value"), arr);
        }
        xsSet(xsResult, xsID("done"), xsFalse);
    }
    else
    {
        xsSet(xsResult, xsID("value"), xsUndefined);
        xsSet(xsResult, xsID("done"), xsTrue);
    }
}

static void xs_formdata_foreach(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    FormDataEntry *e = xs_formdata_entry(the);
    int argc = (int)xsToInteger(xsArgc);
    if (!e)
    {
        xsTypeError("FormData: receiver is not a FormData");
    }
    if (argc < 1)
    {
        xsTypeError("FormData.forEach: callback required");
    }
    for (int i = 0; i < e->npairs; i++)
    {
        /* fn.call(thisArg, value, name) — callback slots re-read per
         * round like every other house delivery loop (slot stability);
         * a bare forEach(fn) passes undefined as the receiver (Arg(1)
         * exists only when the page passed a thisArg — keyed off
         * xsArgc, per the fxCheckArg actual-arg rule). Containment
         * rides the page's own eval bracket, exactly like every
         * callback delivery; a throw stops forEach. */
        xsCall3_noResult(xsArg(0), xsID("call"),
                         argc > 1 ? xsArg(1) : xsUndefined,
                         xsString(e->pairs[i].value),
                         xsString(e->pairs[i].name));
    }
    xsResult = xsUndefined;
}

static void xs_formdata_new(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    /* fxCheckArg validates against the ACTUAL call's argument count (the
     * declared constructor length is only Function.length metadata), so
     * every optional-slot read is guarded by xsArgc first — the house
     * getComputedStyle idiom. A bare `new FormData()` is an EMPTY
     * instance (browser truth: the form argument became optional). */
    int argc = (int)xsToInteger(xsArgc);
    DomNode *form = argc > 0 ? arg_node(the, 0) : NULL;
    DomNode *sub = (argc > 1 && xsTypeOf(xsArg(1)) != xsUndefinedType &&
                    xsTypeOf(xsArg(1)) != xsNullType)
                       ? arg_node(the, 1)
                       : NULL;
    FormDataEntry *e;
    int err = 0;
    e = jsbridge_form_data_construct(b, form, sub, &err);
    if (!e)
    {
        xsTypeError(err == 1 ? "FormData: parameter is not of type "
                               "HTMLFormElement"
                    : err == 2 ? "FormData: submitter is not this form's "
                                 "descendant"
                               : "FormData: too many FormData objects "
                                 "this page");
    }
    xsResult = xsNewHostInstance(st->fdProto);
    xs_set_host_data_site(the, "formdata", xsResult, e);
}

static void xs_build_formdata_proto(JsBridge *b)
{
    xsMachine *the = machine_of(b);
    XsState *st = state_of(b);
    st->fdProto = xsNewHostObject(NULL);
    xsRemember(st->fdProto); /* root BEFORE the build allocates */
    def_fn(the, st->fdProto, "append", xs_formdata_append, 2);
    def_fn(the, st->fdProto, "get", xs_formdata_get, 1);
    def_fn(the, st->fdProto, "getAll", xs_formdata_getAll, 1);
    def_fn(the, st->fdProto, "has", xs_formdata_has, 1);
    def_fn(the, st->fdProto, "set", xs_formdata_set, 2);
    def_fn(the, st->fdProto, "delete", xs_formdata_delete, 1);
    def_fn(the, st->fdProto, "entries", xs_formdata_entries, 0);
    def_fn(the, st->fdProto, "keys", xs_formdata_keys, 0);
    def_fn(the, st->fdProto, "values", xs_formdata_values, 0);
    def_fn(the, st->fdProto, "forEach", xs_formdata_foreach, 2);

    st->fdiProto = xsNewHostObject(NULL);
    xsRemember(st->fdiProto);
    def_fn(the, st->fdiProto, "next", xs_fditer_next, 0);
}

/* ── API-P8: Worker (the contained-stub lane; see the JsWorkerStub note) ──
 * Same thin-glue contract as XMLHttpRequest: wrapper = host instance of a
 * rooted prototype, host data = the stub's public id (0 = the documented
 * delivery-less cap shape). The OBJECT is pinned at construct (wobj[]/
 * wobjSet[], xsRemember); the delivery reads the CURRENT onerror off it —
 * browser at-fire semantics (pages may (re)assign handlers any time).
 * The worker SCRIPT never compiles or runs: postMessage absorbs+logs,
 * terminate cancels/quiets, and onerror fires ONCE at the next frame
 * with ev = {message: stub message}. */
static void xs_worker_postMessage(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int id = (int)(intptr_t)xsGetHostDataIf(xsThis);
    jsbridge_worker_post(b, id);
    xsResult = xsUndefined;
}

static void xs_worker_terminate(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    int id = (int)(intptr_t)xsGetHostDataIf(xsThis);
    jsbridge_worker_terminate(b, id);
    xsResult = xsUndefined;
}

/* Constructor global: new Worker(url) / Worker(url) → stub wrapper. */
static void xs_worker_new(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    const char *url = xsToString(xsArg(0)); /* recorded, never resolved */
    int id = jsbridge_worker_open(b, url);
    if (id <= 0)
    {
        /* Documented cap: a shape-complete delivery-less wrapper. */
        xsSlot obj = xsNewHostInstance(st->workerProto);
        xs_set_host_data_site(the, "workerNew", obj, (void *)(intptr_t)0);
        xsResult = obj;
        return;
    }
    int slot = -1;
    for (int i = 0; i < JSBRIDGE_WORKER_MAX; i++)
    {
        if (!st->wobjSet[i])
        {
            slot = i;
            break;
        }
    }
    if (slot >= 0)
    {
        st->wobj[slot] = xsNewHostInstance(st->workerProto);
        st->wobjSet[slot] = 1;
        xsRemember(st->wobj[slot]);
        /* objRef = wobj slot+1 (uniform with the xhr pin encoding —
         * slot 0 stays usable because the router's NULL rule applies
         * to the ref POINTER). */
        jsbridge_worker_pin(b, id, (void *)(intptr_t)(slot + 1));
        xs_set_host_data_site(the, "workerNew", st->wobj[slot],
                              (void *)(intptr_t)id);
        xsResult = st->wobj[slot];
        return;
    }
    /* Pin slots full: a tracked stub with no object can never deliver —
     * the delivery is cancelled and the page gets a delivery-less shape. */
    jsbridge_worker_terminate(b, id);
    logger_log("[worker] no pin slot — stub #%d delivery-less (engine pin cap)",
               id);
    xsSlot obj = xsNewHostInstance(st->workerProto);
    xs_set_host_data_site(the, "workerNew", obj, (void *)(intptr_t)0);
    xsResult = obj;
}

/* XHR vtable: release the pinned completion + wrapper (slot inert). */
static void xs_clear_xhr_refs(JsBridge *b, void *fnRef, void *objRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
    {
        return;
    }
    int fslot = (int)(intptr_t)fnRef - 1;
    int oslot = (int)(intptr_t)objRef - 1;
    xsBeginHostExit(st->machine);
    if (fslot >= 0 && fslot < JSBRIDGE_XHR_MAX && st->xfnSet[fslot])
    {
        xsForget(st->xfn[fslot]);
        st->xfnSet[fslot] = 0;
    }
    if (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX && st->xobjSet[oslot])
    {
        xsForget(st->xobj[oslot]);
        st->xobjSet[oslot] = 0;
    }
    xsEndHostExit(st->machine);
}

/* Invoke the pinned completion: fn.call(this, responseText) — the same
 * bracket pattern as xs_run_timer_ref (metering + contained exception +
 * abort → engine reset). */
static int xs_run_xhr_ref(JsBridge *b, void *fnRef, void *objRef,
                          const JsHttpRequest *r)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !fnRef)
    {
        return 1;
    }
#ifdef PLUTO_NR_MOUNT_PAUSE
    if (st->mount.active && st->mount.parked)
    {
        /* Parked mount owns the machine (see xs_run_timer_ref). The
         * completion stays pinned in the bridge queue; the pump retries
         * after the mount finishes. */
        return 1;
    }
#endif
    int fslot = (int)(intptr_t)fnRef - 1;
    int oslot = (int)(intptr_t)objRef - 1;
    if (fslot < 0 || fslot >= JSBRIDGE_XHR_MAX || !st->xfnSet[fslot])
    {
        return 1;
    }
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(3);
            xsTry
            {
                xsVar(0) = st->xfn[fslot];
                xsVar(1) = (oslot >= 0 && oslot < JSBRIDGE_XHR_MAX &&
                            st->xobjSet[oslot])
                               ? st->xobj[oslot]
                               : xsNull;
                xsVar(2) = xsString((r && r->body) ? r->body : "");
                xsCall2_noResult(xsVar(0), xsID("call"), xsVar(1), xsVar(2));
            }
            xsCatch
            {
                if (!b->lastError[0])
                {
                    bridge_take_error_text(b, xsToString(xsException));
                }
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (m->exitStatus != xsNormalExit)
    {
        b->errs++;
        bridge_take_error_text(b, "engine abort in xhr callback");
        logger_log("[js] xhr dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    /* C4 (async fidelity): an onload that resolves the fetch promise
     * queues the page's `await fetch(...)` continuation as a NATIVE
     * promise job; the continuation's setState then queues React's
     * flush via queueMicrotask. Drain AND flush alternate until both
     * are empty (see the helper's note) — one flat pass left React's
     * callback stranded in the ring for 47s. */
    xs_boundary_microtasks(b);
    return 0;
}

/* API-P8: release one worker's pinned wrapper (slot inert; same bracket
 * as xs_clear_xhr_refs). */
static void xs_clear_worker_refs(JsBridge *b, void *objRef)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !objRef)
    {
        return;
    }
    int oslot = (int)(intptr_t)objRef - 1;
    xsBeginHostExit(st->machine);
    if (oslot >= 0 && oslot < JSBRIDGE_WORKER_MAX && st->wobjSet[oslot])
    {
        xsForget(st->wobj[oslot]);
        st->wobjSet[oslot] = 0;
    }
    xsEndHostExit(st->machine);
}

/* Delivery: worker.onerror(ev) with `this` = the pinned wrapper and
 * ev = {message}. The CURRENT onerror property is consulted at fire time
 * (browser at-fire semantics — pages reassign handlers freely); a stub
 * without a callable onerror delivers nothing. Same metering + contained
 * exception + abort → engine reset bracket as xs_run_xhr_ref. */
static int xs_run_worker_ref(JsBridge *b, void *objRef, const JsWorkerStub *w)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine || !objRef || !w)
    {
        return 1;
    }
#ifdef PLUTO_NR_MOUNT_PAUSE
    if (st->mount.active && st->mount.parked)
    {
        /* Parked mount owns the machine (see xs_run_xhr_ref). The pin
         * stays held; the pump retries after the mount finishes. */
        return 1;
    }
#endif
    int oslot = (int)(intptr_t)objRef - 1;
    if (oslot < 0 || oslot >= JSBRIDGE_WORKER_MAX || !st->wobjSet[oslot])
    {
        return 1;
    }
    xsMachine *m = st->machine;
    b->inClick = 1; /* reuse the dispatch re-entrancy guard */
    xsBeginHostExit(m);
    xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
    {
        xsBeginHost(m);
        {
            xsVars(3);
            xsTry
            {
                xsVar(0) = xsGet(st->wobj[oslot], xsID("onerror"));
                if (fxIsCallable(m, &xsVar(0)))
                {
                    xsVar(1) = st->wobj[oslot];
                    xsVar(2) = xsNewHostObject(NULL);
                    xsSlot msg = xsString(w->message);
                    xsDefine(xsVar(2), xsID("message"), msg, xsDefault);
                    xsCall2_noResult(xsVar(0), xsID("call"), xsVar(1),
                                     xsVar(2));
                    logger_log("[worker] onerror delivered #%d present=1",
                               w->id);
                }
                else
                {
                    logger_log("[worker] onerror delivered #%d present=0 "
                               "(silent — unhandled worker error)",
                               w->id);
                }
            }
            xsCatch
            {
                if (!b->lastError[0])
                {
                    bridge_take_error_text(b, xsToString(xsException));
                }
            }
        }
        xsEndHost(m);
    }
    xsEndMetering(m);
    xsEndHostExit(m);
    b->inClick = 0;
    b->doc->jsErrors = b->errs;
    snprintf(b->doc->jsLastError, sizeof(b->doc->jsLastError), "%s",
             b->lastError);
    if (m->exitStatus != xsNormalExit)
    {
        b->errs++;
        bridge_take_error_text(b, "engine abort in worker callback");
        logger_log("[js] worker dispatch aborted engine — engine reset");
        xsDeleteMachine(m);
        st->machine = NULL;
        xs_split_invalidate(b); /* R20 pacing + R22 stepwise are dead */
        pluto_mem_resync_live(); /* counter re-base (see run_script) */
        return -1;
    }
    /* A worker error callback can build page objects and settle promises
     * like any completion (see the xhr note). */
    xs_boundary_microtasks(b);
    return 0;
}

/* R27: 1 while the bundler split (or its stepwise session) is still
 * executing — the render pipeline's walker/snapshot must wait for this
 * to clear so they observe the POST-JS DOM (device run 5 walked at seg 1
 * and snapshotted the pre-React DOM: blocks=1). */
static int xs_eval_pending(JsBridge *b)
{
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
        return 0;
    return (st->split.active || st->step.active
#ifdef PLUTO_NR_MOUNT_PAUSE
            || st->mount.active
#endif
            )
               ? 1
               : 0;
}

/* R28: router pumps gate on this while the page is still LOADING — a
 * parked mount run mid-program, or any split still consuming its plan
 * (timers/XHR/deferred scripts must not run against a half-built React
 * tree; they wait — nothing is dropped). This also matches the R27
 * ordering contract: with the mount now completing MID-split, React's
 * timers would otherwise fire between bursts and blow the per-script
 * runaway guard while the split still owns the frame. */
static int xs_mount_parked_query(JsBridge *b)
{
#ifdef PLUTO_NR_MOUNT_PAUSE
    XsState *st = (XsState *)b->implState;
    if (!st || !st->machine)
        return 0;
    if (st->mount.active && st->mount.parked)
        return 1;
    if (st->split.active || st->step.active)
        return 1;
    return 0;
#else
    (void)b;
    return 0;
#endif
}

/* ── R54: DOMContentLoaded at the script-queue drain ─────────────────
 * Browser parse sequence: inline scripts evaluate while readyState is
 * "loading"; DOMContentLoaded fires once AFTER the last of them. The
 * per-frame parked queue reproduces exactly that span: readyState reads
 * "loading" through the drain (see xs_document_readyState) and THIS
 * dispatch is the drain moment the caller (jsbridge_script_queue_pump)
 * invokes via impl->script_queue_drained. Without it the fork-framework
 * branch is broken: next.js's app-router arms its RSC stream closer
 * (M: closes the flight stream + voids the push buffer) on DCL, AND its
 * fallback `setTimeout(M)` (50ms scheduler timer) firing BETWEEN queued
 * script frames voided the buffer mid-push — "Unexpected server data:
 * missing bootstrap script." on the page's last three flight scripts
 * (the sim A/B: byte-sync drain = green; per-frame drain = 3 throws).
 * Modeling: an Event object of type DOMContentLoaded dispatched to every
 * registered document-level DCL listener (kind 3), one bracket, isolated
 * throws, then the C4 boundary drain so the closer's resolution cascade
 * (flight stream completion → hydrateRoot) starts here — the parked
 * mount machinery owns any further frames, exactly like post-DCL
 * hydration in a browser. */
static void xs_script_queue_drained(JsBridge *b)
{
    if (!b || !b->implState)
    {
        return;
    }
    XsState *st = (XsState *)b->implState;
    if (!st->machine)
    {
        return;
    }
    xsMachine *m = st->machine; /* value macros need `the` == m */
    int fired = 0;
    for (int i = 0; i < b->listenerCount; i++)
    {
        JsListener *L = &b->listeners[i];
        if (L->kind != 3 || strcmp(L->type, "DOMContentLoaded") != 0)
        {
            continue;
        }
        xsSlot fn = st->fns[(int)(intptr_t)L->ref];
        if (!fxIsCallable(m, &fn))
        {
            continue;
        }
        xsBeginHostExit(m);
        xsBeginMetering(m, xs_meter_callback, XS_METER_STEP);
        {
            xsBeginHost(m);
            {
                xsVars(2);
                xsTry
                {
                    /* Event object built exactly like the load dispatch
                     * (target = the document; react's closer only reads
                     * nothing from it, but a real dispatch passes one). */
                    xsVar(0) = xsNewHostObject(NULL);
                    xsDefine(xsVar(0), xsID("type"),
                             xsString("DOMContentLoaded"), xsDefault);
                    xsDefine(xsVar(0), xsID("preventDefault"),
                             xsNewHostFunction(xs_event_preventDefault, 0),
                             xsDefault);
                    DomNode *root =
                        (b->dom && b->dom->root) ? b->dom->root : NULL;
                    xsVar(1) = root ? xs_push_element(b, root) : xsNull;
                    xsDefine(xsVar(0), xsID("target"), xsVar(1),
                             xsDefault);
                    xsCall2_noResult(fn, xsID("call"), xsVar(1),
                                     xsVar(0));
                    fired++;
                    /* Boundary drain INSIDE the dispatch bracket: the
                     * closer's o.close() resolves the flight consumer's
                     * queued jobs; the hydrateRoot continuation must
                     * begin in this same checkpoint (like every script
                     * bracket since C4) — further work parks via R28
                     * and resumes through the engine pump. */
                    xs_drain_jobs(m);
                    xs_flush_microtasks(b);
                }
                xsCatch
                {
                    char msg[112];
                    msg[0] = '\0';
                    {
                        xsSlot exc = xsException;
                        if (exc.kind == XS_REFERENCE_KIND)
                        {
                            xsSlot m2 = xsGet(exc, xsID("message"));
                            char *s2 = to_cstring(m, m2);
                            if (s2)
                            {
                                snprintf(msg, sizeof(msg), "%s", s2);
                                JFree(s2);
                            }
                        }
                    }
                    logger_log("[dcl] DOMContentLoaded listener threw "
                               "(isolated): %s",
                               msg[0] ? msg : "exception");
                    bridge_take_error_text(b, msg[0] ? msg : "exception");
                    b->errs++;
                }
            }
            xsEndHost(m);
        }
        xsEndMetering(m);
        xsEndHostExit(m);
        if (st->machine == NULL)
        {
            /* the bracket reset the machine (abort path) — job done */
            logger_log("[dcl] dispatch ended in engine reset (ran=%d)", fired);
            return;
        }
    }
#if defined(PLUTO_THROW_LOG) && defined(PLUTO_HYDRATE_TRACE)
    logger_log("[dcl] DOMContentLoaded fired (listeners=%d)", fired);
#endif
}

const JsEngineImpl js_engine_xs_nr = {
    xs_init,
    xs_run_script,
    xs_run_script_stream,
    xs_dispatch_click,
    xs_clear_timer_ref,
    xs_run_timer_ref,
    xs_run_xhr_ref,
    xs_clear_xhr_refs,
    xs_close,
    xs_engine_pump, /* R20 split resume + API-P3 journal deliveries */
    xs_eval_pending, /* R27: walker/snapshot wait-gate */
    xs_mount_parked_query, /* R28: router pump gate while parked */
    "XS (No Recursion)",
    xs_dispatch_element_event, /* B2: click/input/change/submit bubbling */
    xs_script_queue_drained, /* R54: DOMContentLoaded at queue drain */
    xs_run_worker_ref,       /* API-P8: worker error delivery */
    xs_clear_worker_refs /* API-P8: release the pinned wrapper */};

/* ── R20 host-harness seam ──────────────────────�
 * Direct entry to the split path for host tests (nrsplit harness);
 * mirrors what run_script does for a materialized inline body. No
 * device caller; dead-stripped by --gc-sections on device builds. */
int xs_nr_host_try_split(JsBridge *b, const char *src, size_t len, int index)
{
    return xs_try_bundler_split_src(b, src, len, index);
}



