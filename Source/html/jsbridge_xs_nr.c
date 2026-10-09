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
 * recursion. With the epilogue fixes the measured full-site bootstrap peak
 * is 1598 slots (fxNRStackPeakSlots), so 4096 (128KB host) carries ~2.5x
 * headroom and restores the R14 sizing. */
#define XS_STACK_COUNT 4096u
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
    xsSlot sheetProto; /* CSSOM StyleSheet prototype (rooted; text-backed) */
    xsSlot urlProto; /* URL prototype (rooted; toString/toJSON) */
    /* Pinned listener functions; JsListener.ref holds the index.
     * xsRemember'd against GC; xsForget + release at close. */
    xsSlot fns[JSBRIDGE_LISTENERS_MAX];
    /* Pinned timer callbacks; JsTimer.ref holds the tfn[] slot index.
     * Same xsRemember/xsForget ownership contract as fns[]. */
    xsSlot tfn[JSBRIDGE_TIMERS_MAX];
    unsigned char tfnSet[JSBRIDGE_TIMERS_MAX]; /* slot holds a pin */
    /* XHR + fetch pins (same ownership contract; fetch uses the same
     * xfn[] slots — XS has no native Promise, so only XMLHttpRequest). */
    xsSlot xfn[JSBRIDGE_XHR_MAX]; /* completion handlers / promise resolve */
    xsSlot xobj[JSBRIDGE_XHR_MAX]; /* wrapper objects (this) */
    unsigned char xfnSet[JSBRIDGE_XHR_MAX];
    unsigned char xobjSet[JSBRIDGE_XHR_MAX];
    xsSlot xhrProto; /* XMLHttpRequest prototype (rooted) */
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
} XsState;

/* ── host-provided platform functions (mxUseDefault* = 0) ────────────

/* These five hooks (fxAbort, fxCStackLimit, fxQueuePromiseJobs,
 * fxCreateMachinePlatform, fxDeleteMachinePlatform) are engine-wide
 * symbols: one definition per BINARY is expected by Moddable's platform
 * model, and the stock bridge (jsbridge_xs.c) defines identical ones.
 * Device builds link both bridges, so the fork's copies compile only when
 * the stock engine is absent (simulator one-shot dylib). */
#ifndef PLUTO_NO_STOCK_XS
#else

/* fxAbort: every fatal engine condition lands here (OOM, C-stack, meter
 * limit, unhandled rejection, …). Instead of the stock fprintf+exit
 * (would kill the browser), record and unwind to the OUTERMOST host
 * bracket — the stock ESP32 device platform's ABORT_EXITTOHOST pattern. */
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

/* ── accessors (this = element wrapper) ────────────────── */

static void xs_el_get_tagName(xsMachine *the)
{
    DomNode *n = this_node(the);
    if (!n || n->kind != DOM_ELEMENT)
    {
        xsResult = xsString("");
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

/* UNSUPPORTED (no subtree serializer): degrades to text — engine parity. */
static void xs_el_get_innerHTML(xsMachine *the)
{
    xs_el_get_textContent(the);
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

static void xs_el_get_nodeType(xsMachine *the)
{
    DomNode *n = this_node(the);
    xsResult = xsInteger((n && n->kind == DOM_ELEMENT) ? 1 : 3);
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
    if (n && n->kind == DOM_ELEMENT)
    {
        char *s = to_cstring(the, xsArg(0));
        if (!s)
        {
            xsTypeError("out of memory");
        }
        int rc = jsbridge_el_set_inner_html(b, n, s, strlen(s));
        JFree(s);
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
}

static void xs_el_removeAttribute(xsMachine *the)
{
    DomNode *n = this_node(the);
    char *k = to_cstring(the, xsArg(0));
    if (!k)
    {
        xsTypeError("out of memory");
    }
    if (n)
    {
        dom_remove_attr(n, k);
    }
    JFree(k);
}

static void xs_el_appendChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    if (!n || !c || dom_append_child(b->dom, n, c) != 0)
    {
        xsTypeError("appendChild failed");
    }
}

static void xs_el_removeChild(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    BUDGET_OR_THROW(b);
    DomNode *n = this_node(the);
    DomNode *c = arg_node(the, 0);
    if (!n || !c || dom_remove_child(n, c) != 0)
    {
        xsTypeError("removeChild failed");
    }
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
    dom_set_attr(b->dom, n, "style", buf);
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
    char scratch[JSBRIDGE_QS_SCRATCH];
    DomNode *hit = (DomNode *)jsbridge_el_query_selector_first(
        b, n, sel, scratch, sizeof(scratch));
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

static void xs_el_get_classList(xsMachine *the)
{
    xs_push_classlist(bridge_of(the), this_node(the));
}

static void xs_el_get_style(xsMachine *the)
{
    xs_push_style(bridge_of(the), this_node(the));
}

static void xs_el_addEventListener(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    DomNode *n = this_node(the);
    char *type = to_cstring(the, xsArg(0));
    if (!type)
    {
        xsTypeError("out of memory");
    }
    int isClick = (strcmp(type, "click") == 0);
    JFree(type);
    if (!isClick)
    {
        /* UNSUPPORTED event types are accepted and ignored (no-op) so
         * pages registering them don't error — only click is delivered. */
        return;
    }
    xsSlot fn = xsArg(1);
    if (!fxIsCallable(the, &fn))
    {
        xsTypeError("listener must be a function");
    }
    if (b->listenerCount >= JSBRIDGE_LISTENERS_MAX)
    {
        xsTypeError("too many event listeners");
    }
    int idx = b->listenerCount++;
    st->fns[idx] = fn;
    xsRemember(st->fns[idx]); /* pin against GC */
    JsListener *L = &b->listeners[idx];
    L->ref = (void *)(intptr_t)idx;
    L->target = n;
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
    xsResult = xsNewArray(0);
    int k = 0;
    if (n)
    {
        DomNode *stack[DOM_SEARCH_MAX_DEPTH];
        int top = 0;
        stack[top++] = n;
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
    JFree(tag);
}

/* Push a fresh wrapper for `node` (NULL → null). The prototype is a
 * rooted host object; xsNewHostInstance clones it (inheriting its
 * internal host slot for the instance's data) and we store the DomNode*
 * as host data. Wrappers are never cached → nothing for GC to trace. */
static xsSlot xs_push_element(JsBridge *b, DomNode *node)
{
    xsMachine *the = machine_of(b); /* value macros below need `the` */
    if (!node)
    {
        return xsNull;
    }
    xsSlot obj = xsNewHostInstance(state_of(b)->elProto);
    xs_set_host_data_site(the, "element", obj, node);
    return obj;
}

/* ── console ─────────────────────────── */
static void xs_console_log(xsMachine *the)
{
    char line[160];
    size_t off = 0;
    line[0] = '\0';
    int argc = (int)xsToInteger(xsArgc);
    for (int i = 0; i < argc && off < sizeof(line) - 2; i++)
    {
        char *s = to_cstring(the, xsArg(i));
        if (!s)
        {
            continue;
        }
        size_t n = strlen(s);
        if (off + n >= sizeof(line) - 2)
        {
            n = sizeof(line) - 2 - off;
        }
        memcpy(line + off, s, n);
        off += n;
        if (i < argc - 1)
        {
            line[off++] = ' ';
        }
        line[off] = '\0';
        JFree(s);
    }
    logger_log("[js] %s", line);
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
    DomNode *hit = NULL;
    DomNode *stack[DOM_SEARCH_MAX_DEPTH];
    int top = 0;
    stack[top++] = b->dom->root;
    while (top > 0 && !hit)
    {
        DomNode *n = stack[--top];
        if (n->kind == DOM_ELEMENT)
        {
            const char *v = dom_get_attr(n, "id");
            if (v && strcmp(v, id) == 0)
            {
                hit = n;
                break;
            }
        }
        if (top + n->childCount > DOM_SEARCH_MAX_DEPTH)
        {
            break;
        }
        for (int i = 0; i < n->childCount; i++)
        {
            stack[top++] = n->children[i];
        }
    }
    if (!hit && id[0] == '_' && id[1] == 'd')
    {
        hit = dom_node_by_id(b->dom, atoi(id + 2));
    }
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

/* document/window.addEventListener: UNSUPPORTED delivery (no bubbling
 * model); accepted silently so common pages don't throw — parity. */
static void xs_doc_addEventListener(xsMachine *the)
{
    (void)the;
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

static void xs_computed_getPropertyValue(xsMachine *the)
{
    char *property = to_cstring(the, xsArg(0));
    if (!property) xsTypeError("out of memory");
    char value[128];
    xs_style_read_prop(this_node(the), property, value, sizeof(value));
    JFree(property);
    xsResult = xsString(value);
}

static void xs_history_setState(xsMachine *the)
{
    if (xsToInteger(xsArgc) > 0)
        xsSet(xsThis, xsID("state"), xsArg(0));
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
    DomNode *t = dom_create_text(b->dom, "");
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
                    DomNode *t = dom_create_text(b->dom, v);
                    if (t)
                    {
                        dom_append_child(b->dom, c, t);
                    }
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

    static const struct
    {
        const char *name;
        xsCallback get;
        xsCallback set;
    } accs[] = {
        {"tagName", xs_el_get_tagName, NULL},
        {"id", xs_el_get_id, xs_el_set_id},
        {"href", xs_el_get_href, xs_el_set_href},
        {"protocol", xs_el_get_protocol, NULL},
        {"host", xs_el_get_host, NULL},
        {"hostname", xs_el_get_hostname, NULL},
        {"port", xs_el_get_port, NULL},
        {"pathname", xs_el_get_pathname, NULL},
        {"search", xs_el_get_search, NULL},
        {"hash", xs_el_get_hash, NULL},
        {"textContent", xs_el_get_textContent, xs_el_set_textContent},
        {"innerHTML", xs_el_get_innerHTML, xs_el_set_innerHTML},
        {"parentNode", xs_el_get_parentNode, NULL},
        {"childElementCount", xs_el_get_childElementCount, NULL},
        {"children", xs_el_get_children, NULL},
        {"nodeType", xs_el_get_nodeType, NULL},
        {"nodeName", xs_el_get_tagName, NULL},
        {"ownerDocument", xs_el_get_ownerDocument, NULL},
        {"firstElementChild", xs_el_get_firstElementChild, NULL},
        {"nextElementSibling", xs_el_get_nextElementSibling, NULL},
        {"classList", xs_el_get_classList, NULL},
        {"style", xs_el_get_style, NULL},
        {"sheet", xs_el_get_sheet, NULL},
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
        {"appendChild", xs_el_appendChild, 1},
        {"removeChild", xs_el_removeChild, 1},
        {"insertBefore", xs_el_insertBefore, 2},
        {"querySelector", xs_el_querySelector, 1},
        {"querySelectorAll", xs_el_querySelectorAll, 1},
        {"addEventListener", xs_el_addEventListener, 2},
        {"getElementsByTagName", xs_el_getElementsByTagName, 1},
        {"getBoundingClientRect", xs_el_getBoundingClientRect, 0},
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
    xs_timer_setup(the, b, st, JS_TIMER_TIMEOUT);
}

static void xs_setInterval(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xs_timer_setup(the, b, st, JS_TIMER_INTERVAL);
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
            def_val(the, glob, ctors[i],
                    xsNewHostConstructor(xs_noop, 0, xsNewObject()));
        }
    }

    /* console.log only — parity with the other bridges */
    {
        xsSlot con = xsNewHostObject(NULL);
        xsResult = con; /* GC anchor */
        def_fn(the, con, "log", xs_console_log, 0);
        def_fn(the, con, "warn", xs_console_log, 0);
        def_fn(the, con, "error", xs_console_log, 0);
        def_val(the, glob, "console", con);
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

    /* Location components use the shared URL parser, not page-specific
     * values. Routers and URL helpers need more than href. */
    {
        xsSlot loc = xsNewHostObject(NULL);
        xsResult = loc; /* GC anchor: xsString() below can collect */
        const char *base = b->doc->baseUrl;
        UrlParsed parsed;
        int ok = url_parse(base, &parsed) == 0;
        char host[144], origin[176], port[12], component[264];
        int defaultPort = ok && ((parsed.isSsl && parsed.port == 443) || (!parsed.isSsl && parsed.port == 80));
        snprintf(port, sizeof(port), "%d", ok ? parsed.port : 0);
        snprintf(host, sizeof(host), "%s%s%s", ok ? parsed.host : "", ok && !defaultPort ? ":" : "", ok && !defaultPort ? port : "");
        snprintf(origin, sizeof(origin), "%s://%s", ok ? parsed.scheme : "", host);
        def_val(the, loc, "href", xsString(base));
        def_val(the, loc, "origin", xsString(ok ? origin : "null"));
        snprintf(component, sizeof(component), "%s:", ok ? parsed.scheme : "");
        def_val(the, loc, "protocol", xsString(ok ? component : ""));
        def_val(the, loc, "host", xsString(host));
        def_val(the, loc, "hostname", xsString(ok ? parsed.host : ""));
        def_val(the, loc, "port", xsString(ok && !defaultPort ? port : ""));
        def_val(the, loc, "pathname", xsString(ok ? parsed.path : "/"));
        snprintf(component, sizeof(component), "%s%s", ok && parsed.query[0] ? "?" : "", ok ? parsed.query : "");
        def_val(the, loc, "search", xsString(component));
        snprintf(component, sizeof(component), "%s%s", ok && parsed.hash[0] ? "#" : "", ok ? parsed.hash : "");
        def_val(the, loc, "hash", xsString(component));
        def_fn(the, loc, "assign", xs_location_assign, 1);
        def_fn(the, loc, "replace", xs_location_assign, 1);
        def_val(the, glob, "location", loc);
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

    /* History state is stored; back/forward are inert until navigation
     * entries are available, matching the existing QuickJS bridge. */
    {
        xsSlot history = xsNewObject();
        xsResult = history; /* GC anchor */
        def_val(the, history, "state", xsNull);
        def_val(the, history, "length", xsInteger(1));
        def_val(the, history, "scrollRestoration", xsString("auto"));
        def_fn(the, history, "pushState", xs_history_setState, 3);
        def_fn(the, history, "replaceState", xs_history_setState, 3);
        def_fn(the, history, "go", xs_noop, 1);
        def_fn(the, history, "back", xs_noop, 0);
        def_fn(the, history, "forward", xs_noop, 0);
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
        def_fn(the, doc, "write", xs_document_write, 0);
        def_fn(the, doc, "writeln", xs_document_writeln, 0);
        def_fn(the, doc, "addEventListener", xs_doc_addEventListener, 2);
        def_fn(the, doc, "removeEventListener", xs_noop, 2);
        def_val(the, doc, "defaultView", glob);
        def_val(the, doc, "nodeType", xsInteger(9));
        /* title as a live getter (walks the tree; see above) */
        {
            xsSlot getter = xsNewHostFunction(xs_document_getTitle, 0);
            xsDefine(doc, xsID("title"), getter, xsIsGetter);
            xsSlot titleSetter = xsNewHostFunction(xs_document_setTitle, 1);
            xsDefine(doc, xsID("title"), titleSetter, xsIsSetter);
        }
        /* body/head/documentElement: document-order search from #root
         * (see find_first_element — these are grandchildren of the
         * synthetic root). body/activeElement keep the #root fallback
         * for fragment documents; head falls back to the document
         * element so CSS-in-JS hosts (emotion `container ||
         * document.head`) always get an insertable node. */
        DomNode *body = find_first_element(b, "body");
        def_val(the, doc, "body",
                xs_push_element(b, body ? body : b->dom->root));
        def_val(the, doc, "activeElement", xs_push_element(b, body ? body : b->dom->root));
        {
            DomNode *html = find_first_element(b, "html");
            DomNode *head = find_first_element(b, "head");
            def_val(the, doc, "documentElement",
                    xs_push_element(b, html ? html : b->dom->root));
            def_val(the, doc, "head",
                    xs_push_element(b, head ? head : (html ? html : b->dom->root)));
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
    def_fn(the, glob, "clearInterval", xs_clearInterval, 0);
    def_fn(the, glob, "requestAnimationFrame", xs_setTimeout, 0);
}

/* ── microtask drain (fxRunLoop parity, tool-free) ──────────────�
 * Jobs live in the reserved mxPendingJobs slot (fixed offset from
 * stackTop, addressable whenever the machine is quiescent — both drain
 * call sites are). fxRunPromiseJobs moves the chain to mxRunningJobs;
 * jobs queued *during* a job append to mxPendingJobs again, so loop
 * until the chain is empty. fxEndJob = post-drain cleanup.
 */
static void xs_drain_jobs(xsMachine *the)
{
    fxEndJob(the);
    while (mxPendingJobs.value.reference->next)
    {
        fxRunPromiseJobs(the);
        fxEndJob(the);
    }
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
        logger_log("[stack] %s n=%u depth=%ldB best=%ldB",
                   site, s_stack_n, depth, s_stack_best);
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
    p->firstJump = &st->step.jump;
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
                         * yields and the caller re-queues the frame. */
                        return;
                    }
                    logger_log("[js] bundle %d seg %d: restart parser alloc "
                               "failed — failing contained",
                               index, st->step.seg);
                }
                bridge_take_error_text(b, "codegen alloc failed");
                b->errs++;
                xs_step_finish(b, st, "codegen restart budget exhausted");
                return;
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
        p->firstJump = NULL;
        st->step.state = PLUTO_STEP_RUN;
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
    }
}

/* RUN slice: bracket + run + drain in ONE frame — the R20 contract
 * (an executing bundle segment stays atomic per frame). The script was
 * produced by the CODE slice; fxRunScript consumes it (deletes on both
 * success and throw). */
static void xs_step_run_slice(JsBridge *b, xsMachine *m, int index)
{
    XsState *st = (XsState *)b->implState;
    txScript *script = st->step.script;
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
        fxCollectGarbage(m);
        pluto_mem_resync_live();
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
    if (len > JSBRIDGE_MAX_SCRIPT_SOURCE)
    {
        xs_fail_script(b, index, "script too large");
        return;
    }
    /* Same compile-safety gate as muJS/Duktape/QuickJS (one bar). */
    if (!jsbridge_script_compile_safe(src, len))
    {
        xs_fail_script(b, index, "script too deeply nested (compile guard)");
        return;
    }

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
            fxCollectGarbage(m);
            pluto_mem_resync_live();
            logger_log("[js] bundle %d seg %d/%d: pre-run compact collect "
                       "live=%luKB→%luKB",
                       index, seg, lastSeg, liveBefore >> 10,
                       pluto_mem_live() >> 10);
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
        if (L->target != anchorNode)
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
    return 0;
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
static void xs_xhr_open(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    const char *method = xsToString(xsArg(0));
    const char *url = xsToString(xsArg(1));
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
    jsbridge_xhr_send(b, id, (void *)(intptr_t)(fslot + 1),
                      (void *)(intptr_t)(oslot + 1));
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

/* Constructor global: new XMLHttpRequest() / XHR() → fresh wrapper. */
static void xs_xmlhttprequest_new(xsMachine *the)
{
    JsBridge *b = bridge_of(the);
    XsState *st = state_of(b);
    xsSlot obj = xsNewHostInstance(st->xhrProto);
    xs_set_host_data_site(the, "xhrNew", obj, (void *)(intptr_t)0);
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
    xs_pump_split, /* R20: resume a time-sliced bundler split per frame */
    xs_eval_pending, /* R27: walker/snapshot wait-gate */
    xs_mount_parked_query, /* R28: router pump gate while parked */
    "XS (No Recursion)"};

/* ── R20 host-harness seam ──────────────────────�
 * Direct entry to the split path for host tests (nrsplit harness);
 * mirrors what run_script does for a materialized inline body. No
 * device caller; dead-stripped by --gc-sections on device builds. */
int xs_nr_host_try_split(JsBridge *b, const char *src, size_t len, int index)
{
    return xs_try_bundler_split_src(b, src, len, index);
}



