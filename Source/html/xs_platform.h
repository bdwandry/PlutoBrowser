/*
 * PlutoBrowser — Source/html/xs_platform.h
 * The XSPLATFORM header for the STOCK XS 9.5.0 engine
 * (Source/js/xs_moddable — never modified).
 *
 * HOW XS PLATFORMS WORK (verified against the stock 9.5.0 sources):
 * xs/sources/xsPlatform.h, when compiled with -DINCLUDE_XSPLATFORM,
 * does `#include XSPLATFORM` and expects THAT header to:
 *   - typedef the txS1..txU8 / c_* / C_* environment (every member it
 *     does NOT define falls back to a stock default via #ifndef);
 *   - define mxMachinePlatform (extra txMachine members);
 *   - define the mxUseDefault* switches (0 = the HOST provides the
 *     named platform function; see xs/sources/xsPlatforms.c);
 *   - include the libc headers the engine needs.
 *
 * This header is ONLY ever included by the stock engine through that
 * hook (-DINCLUDE_XSPLATFORM -DXSPLATFORM='"xs_platform.h"' in the
 * Makefile), never by PlutoBrowser's own C files.
 *
 * Every setting here keeps the vendored engine byte-identical.
 */
#ifndef __PLUTO_XSPLATFORM__
#define __PLUTO_XSPLATFORM__

/*
 * ── 1. libc environment ─────────────────────────────────────────────
 * Mirrors the stock host platform headers (mac_xs.h et al.). No fdlibm:
 * the c_exp/c_log/c_pow math hooks fall back to libc in the stock
 * platform layer, which both the sim (macOS) and the device (newlib)
 * provide. The socket/net headers are what stock tool builds include;
 * the engine only touches sockets behind mxDebug (off here).
 */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

/* Network + thread headers exist on the HOST toolchains (what the stock
 * xst/mac_xs.h-style platforms include) but NOT in the Playdate device
 * newlib. The engine only touches sockets behind mxDebug (off here) and
 * threads behind mxUsePOSIXThreads (off here), so the device build simply
 * omits them. TARGET_PLAYDATE is defined by the Playdate device build. */
#ifndef TARGET_PLAYDATE
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#endif

typedef int txSocket;
#define mxNoSocket -1

/*
 * ── 2. memory functions ─────────────────────────────────────────────
 * XS grows its heap in whole malloc'd chunks (fxAllocateChunks) via the
 * c_malloc/c_realloc/c_free macros below (stock xsPlatform.h maps them to
 * libc when not overridden). We override them HERE — in OUR platform
 * header, not the engine — to route XS chunk allocations through the same
 * SW1 telemetry funnel as everything else (core/pluto_mem.c). The engine
 * source is untouched; only the macro mapping changes.
 */
void *pluto_mem_realloc(void *ptr, size_t n);
#define c_malloc(sz)   pluto_mem_realloc(NULL, (sz))
/* calloc must ZERO: use a dedicated zeroing helper (realloc does not). */
void *pluto_mem_calloc(size_t n, size_t sz);
#define c_calloc(n, sz) pluto_mem_calloc((n), (sz))
#define c_realloc(p, sz) pluto_mem_realloc((p), (sz))
#define c_free(p)      pluto_mem_realloc((p), 0)
#define c_free_uint32(p) pluto_mem_realloc((p), 0)
#define c_malloc_uint32(sz) pluto_mem_realloc(NULL, (sz))

/*
 * ── 3. no threads, no atomics, no shared chunks ─────────────────────
 * The Playdate runs ONE JS machine per page on one thread. With
 * mxThreads == 0 (no mxUsePOSIXThreads/mxUseGCCAtomics) the waiter code
 * in xsAtomics.c compiles out; Atomics ops still exist (non-atomic on
 * a single thread) and Atomics.wait correctly throws ("main thread
 * cannot wait" semantics).
 */
/* #undef mxUsePOSIXThreads (never defined here) */
/* #undef mxUseGCCAtomics  (never defined here) */

/*
 * ── 4. the machine platform member set ──────────────────────────────
 * Mirrors the canonical host (xs/tools/xst.h): the debugger socket is
 * unused (mxDebug off), but the promise-jobs flag + rejection/script
 * scratch fields are what the host-side job drain reads (xst's
 * fxRunLoop pattern, mirrored in jsbridge_xs.c).
 */
#define mxMachinePlatform \
	void* host; \
	int promiseJobs; \
	void* rejection; \
	void *script;

/*
 * ── 5. host-provided platform functions ─────────────────────────────
 * Defining an mxUseDefault* as 0 makes the stock platform layer SKIP
 * its default definition, so the host (jsbridge_xs.c) must define it.
 * We override exactly four:
 */
#define mxUseDefaultMachinePlatform 0 /* fxCreateMachinePlatform/       */
                                      /* fxDeleteMachinePlatform: no-op */
#define mxUseDefaultAbort 0           /* fxAbort: contain engine aborts */
#define mxUseDefaultQueuePromiseJobs 0 /* fxQueuePromiseJobs: flag only */
#define mxUseDefaultCStackLimit 0     /* fxCStackLimit: real C-stack bound */

/* Everything else keeps the stock default (keys, chunks, slots, module
 * lookup, script parsing, shared chunks, debug stubs): */
#define mxUseDefaultBuildKeys 1
#define mxUseDefaultChunkAllocation 1
#define mxUseDefaultSlotAllocation 1
#define mxUseDefaultFindModule 1
#define mxUseDefaultLoadModule 1
#define mxUseDefaultParseScript 1
#define mxUseDefaultSharedChunks 1
#define mxUseDefaultDebug 1

/*
 * ── 6. feature switches (the footprint build, like stock devices) ───
 * mxNoConsole=1: the engine must not printf() diagnostics (Playdate has
 *   no console; logging goes through PlutoBrowser's logger).
 * mxMetering: enables the bytecode-op counter (xsBeginMetering) so a
 *   runaway page script is bounded like the muJS run-limit.
 * mx32bitID: 64-bit hosts default to 64-bit IDs; forcing 32-bit IDs
 *   matches the device target (32-bit ARM) so sim and device run one
 *   code shape.
 * mxDebug is intentionally NOT defined: no xsbug socket, no debug
 *   fields, no retained source strings.
 */
#define mxNoConsole 1
#define mxMetering 1
#define mx32bitID 1

/* malloc_usable_size is not portable across host + device libcs — XS
 * does not require it (it accounts requested sizes), nothing to map. */

#endif /* __PLUTO_XSPLATFORM__ */
