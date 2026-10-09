/*
 * PlutoBrowser — jsbridge_bundler.h (R18: general-purpose bundler monolith
 * splitter)
 *
 * Modern bundlers (webpack/rollup-style production output) ship a page's
 * entire script as ONE giant IIFE:
 *
 *   (()=>{var e={5513:(e,t,n)=>{...},7950:(e,t,n)=>{...},...},t={};
 *         function n(r){...}...i.render(...,...)})()
 *
 * No engine can compile that as a single unit on the device (parse
 * working-set + admission budget scale with WHOLE-file size), yet the file
 * decomposes into a few hundred SELF-CONTAINED top-level statements whose
 * individual footprints are tiny (the R16 chunked path proves the shape
 * works — it just cannot split a file that is ONE statement).
 *
 * R18 detects the bundler STRUCTURE (never a site: no URLs, no names, no
 * allowlists) and emits an execution PLAN over [start,end) spans of the
 * original source:
 *
 *   seg 0      : `var <m>={};`  (fresh module registry)
 *   seg 1..k   : `<m>[<key>]=<value>;` for each module-map entry — the
 *                registry survives across engine brackets, so modules can
 *                require each other exactly as authored
 *   seg k+1..  : the remaining tail (runtime bootstrap + app entry) wrapped
 *                in its own IIFE; a leading `,decl=0` repairs a `var`
 *                declarator that the map used to continue
 *
 * The tail is emitted as ONE segment on purpose: QuickJS on device parses
 * 253KB of tail within its own probed stack budget (qexp experiment,
 * MASTER_TODO R18). Engines that still refuse it (ES5 parsers, XS NR
 * parser-memory cap) containedly skip that one segment and every other
 * segment still runs.
 *
 * The plan is two spans of indices into a small struct; maxSpans bounds the
 * device allocation (a 500KB bundle ≈ 2000 spans ≈ 32KB). Callers that get
 * PLUTO_BUNDLER_NO_MAP simply run the script unsplit — this code only ever
 * ADDS a path, it is a no-op for anything that is not a bundler monolith.
 */
#ifndef PLUTO_JSBRIDGE_BUNDLER_H
#define PLUTO_JSBRIDGE_BUNDLER_H

#include <stddef.h>
#include <stdint.h>

/* Hard cap on emitted spans. webpack production bundles of real apps stay
 * far below this (bryanwandrych.com: 95 modules + 1 tail). A bundle above
 * the cap is simply not split (falls back to the unsplit path). */
#ifndef PLUTO_BUNDLER_MAX_SPANS
#define PLUTO_BUNDLER_MAX_SPANS 2048
#endif

/* A bundler monolith must look like `(()=>{var <id>={` within the first
 * bytes (skipping a possible license comment), and the registry var must
 * hold at least this many entries before we commit to splitting. */
#define PLUTO_BUNDLER_MIN_ENTRIES 8

/* R20b: target tail slice size in source bytes. The tail (bootstrap +
 * entry) is emitted as several wrapped statement slices of ~this size so
 * no single execution unit dominates a frame (device watchdog: 10s
 * without an update() return; measured ~12KB/s engine throughput makes
 * a 6KB slice comfortably sub-second). #ifndef so host tests can tune
 * it for small fixtures. */
#ifndef PLUTO_BUNDLER_TAIL_STEP
#define PLUTO_BUNDLER_TAIL_STEP (6u * 1024u)
#endif

/* R26f: hard ceiling on a single tail slice. The R20b slicer tries to
 * cut every ~tailStep bytes, but a slice can only END at a depth-1
 * statement boundary — and the last such boundary in a web bundle's
 * tail is routinely followed by one very long multi-statement region
 * (React's JSX tree: `const of=function(){return(0,Ne.jsx)(we,{children:
 * (0,Ne.jsx)(rf,{children:[...]})})},i.render(...)` spans 200KB+ with
 * NOTHING cuttable — its commas are all inside the render tree, its
 * statements are all `}`-ASI-adjacent with no `;` at all). Without a
 * ceiling the final slice silently absorbs ~all of the tail (measured:
 * 252656B of 253KB) and dies at parse. With the ceiling, an oversized
 * slice is FORCE-CUT at the last boundary BEFORE the ceiling — multiple
 * consecutive top-level statements, semantically identical under the
 * same ordering guarantee as R20b — or, if NO boundary exists in the
 * whole remainder (a single monster statement), the slice stays whole
 * and its segment skips containedly (pre-R26f behavior). Must be ≥
 * TAIL_STEP. #ifndef so host tests can tune it. */
#ifndef PLUTO_BUNDLER_TAIL_CHUNK_MAX
#define PLUTO_BUNDLER_TAIL_CHUNK_MAX (24u * 1024u)
#endif

/* Only scripts at least this large are considered for splitting — anything
 * smaller parses fine whole on every engine. #ifndef so host test builds
 * can lower it and exercise the machinery with small fixtures. */
#ifndef PLUTO_BUNDLER_MIN_SOURCE
#define PLUTO_BUNDLER_MIN_SOURCE (32u * 1024u)
#endif

typedef enum
{
    PLUTO_BUNDLER_NO_MAP = 0, /* not a bundler monolith — run unsplit */
    PLUTO_BUNDLER_OK = 1      /* plan valid */
} PlutoBundlerStatus;

typedef struct
{
    uint32_t start; /* inclusive, into the ORIGINAL source buffer */
    uint32_t end;   /* exclusive */
} PlutoBundlerSpan;

typedef struct
{
    PlutoBundlerSpan *spans; /* caller-provided array, cap maxSpans.
                              * spans[0] is a {0,0} PLACEHOLDER — segment 0
                              * (the registry decl) is generated, not copied
                              * from the source. spans[1..entries] are the
                              * per-module `key:value` spans. */
    int maxSpans;
    int nSpans; /* filled: spans[0..nSpans-1] in execution order */
    /* registry bookkeeping (internal; emitted segments reference these) */
    char name[24];   /* registry variable identifier (copied, NUL-terminated) */
    int entries;     /* number of per-module spans (spans[1..entries]) */
    int tailIndex;   /* index of the first tail span (entries+1), or -1 */
    int tailCount;   /* number of tail spans (R20b: statement slices) */
    int tailHasComma; /* tail began with ",..." (var-declarator repair) */
    /* R26h: ENTRY UNWRAP. The last tail slice is the webpack entry arrow
     * `(()=>{ BODY })()` — one statement whose whole-function compile
     * transient exceeds every device budget. When its shape is proven,
     * the body's depth-1 statements become their own tail spans (the R20b
     * global-persistence model) and the bundler's registry/cache globals
     * move to collision-proof names (__we/__wt) so the body's own `var
     * e={} / var t` keep working as ordinary globals. */
    int entryBodyIndex; /* first tail span that is an unwrapped ENTRY BODY
                         * slice (registry rename stops here); -1 = none */
    int registryRenamed; /* 1 = registry is "__we", cache "__wt" */
    uint32_t tailStart; /* tail span in the ORIGINAL source (runtime
                         * bootstrap + entry; emitted inside its own IIFE) */
    uint32_t tailEnd;
    /* R20b: tail slice stride in source bytes. The tail (runtime bootstrap
     * + app entry) is emitted as tailCount wrapped slices of ~tailStep
     * bytes each, so no execution unit holds the device run loop for tens
     * of seconds (watchdog fires at a 10s no-update stall; measured device
     * engine throughput is ~12KB/s). Slices are cut at whole LEXED
     * statements only — a slice boundary can never land inside a literal
     * or template — and each slice is wrapped in its own IIFE, preserving
     * execution order and top-level `var` scoping within the tail. */
    uint32_t tailStep;
} PlutoBundlerPlan;

/* Scan src[0..len) for a bundler monolith and fill plan (which must carry a
 * caller-provided spans array). Lexes comments/strings/templates/regexes;
 * conservative: ANY structural surprise (truncated file, >cap entries,
 * malformed tail) returns NO_MAP and the caller runs the script unsplit.
 * Thread-safe, O(n), no allocation. */
PlutoBundlerStatus jsbridge_bundler_plan(const char *src, size_t len,
                                         PlutoBundlerPlan *plan);

/* Materialize plan segment i into dst (dstCap bytes; the caller sizes it
 * from jsbridge_bundler_segment_cap) reading the ORIGINAL source only for
 * the entry VALUE spans — everything else is generated. Returns the byte
 * length written (excluding NUL) or -1 if dstCap is too small. */
long jsbridge_bundler_emit(const char *src, size_t len,
                           const PlutoBundlerPlan *plan, int seg,
                           char *dst, size_t dstCap);

/* Worst-case materialized size of segment seg (pre-admission sizing). */
size_t jsbridge_bundler_segment_cap(const PlutoBundlerPlan *plan, int seg);

#endif /* PLUTO_JSBRIDGE_BUNDLER_H */
