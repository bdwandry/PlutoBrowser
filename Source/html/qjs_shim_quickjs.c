/*
 * PlutoBrowser — qjs_shim_quickjs.c
 * Compile-time adapter TU for the STOCK vendored QuickJS
 * (Source/js/QuickJS — never modified).
 *
 * Why this file exists: QuickJS exports js_malloc/js_free/js_realloc/
 * js_strdup (its public allocator helpers via cutils.h) and muJS's internal
 * allocator wrappers use the same four names — linking both engines into
 * one browser binary fails with duplicate symbols. Renaming at compile time
 * HERE, and only for the QuickJS translation units (via these shim files),
 * gives QuickJS's helpers distinct symbols while muJS keeps its own. Neither
 * vendored engine file is modified, so both stay drop-in updatable.
 *
 * The rename is internal to the QuickJS side: libregexp/libunicode/dtoa/
 * cutils and quickjs.c are all compiled through matching shims with the
 * SAME set of #defines, so every QuickJS-internal call site and definition
 * stays consistent. muJS's translation units never see these defines.
 *
 * Upstream builds QuickJS with -D_GNU_SOURCE (its documented build
 * configuration): it enables _POSIX_THREADS (pthread types/macros in
 * newlib's pthread.h) and __BSD_VISIBLE (struct tm tm_gmtoff). We mirror
 * that HERE, scoped to these shims only, so no other TU is affected and
 * the vendored engine stays byte-identical.
 */
#define _GNU_SOURCE 1
/* Bare-metal newlib hides POSIX threading declarations unless asked,
 * and names the struct tm UTC-offset member via __TM_GMTOFF. QuickJS's
 * Atomics/Date code needs both; single-threaded Playdate gets no-op
 * primitives from qjs_pthread_stubs.c (our code, not the engine's). */
#define _POSIX_THREADS 1
#define __TM_GMTOFF tm_gmtoff
#define js_malloc pluto_qjs_malloc
#define js_free pluto_qjs_free
#define js_realloc pluto_qjs_realloc
#define js_strdup pluto_qjs_strdup

/* quickjs.h defines JS_PTR64 itself on 64-bit hosts; the Makefile forces it
 * on the 32-bit device. Undef only where the header re-defines it. */
#include <stdint.h>
#if INTPTR_MAX >= INT64_MAX
#undef JS_PTR64
#endif

/* newlib's <time.h> hides clock_gettime unless _POSIX_TIMERS is set, and the
 * command-line -Dclock_gettime rename means the engine calls our stub under
 * this name. Declare it with the POSIX signature (matching the host's own
 * declaration too) so the call is never implicit. */
#include <time.h>
int pluto_qjs_clock_gettime(clockid_t clk, struct timespec *ts);

/* Newlib has no malloc_usable_size. The default allocator (not the one this
 * bridge installs) only uses it for accounting; upstream documents returning
 * 0 as the fallback for platforms without the call. */
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    !defined(__linux__) && !defined(__GLIBC__)
#define malloc_usable_size(ptr) ((size_t)0)
#endif

/* The vendored engine is compiled with the project's -Wall set; these are
 * diagnostics in stock QuickJS code, not in PlutoBrowser's adapter. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstack-usage="
#endif
#endif

#include "../js/QuickJS/quickjs.c"
