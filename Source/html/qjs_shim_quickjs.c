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

#include "../js/QuickJS/quickjs.c"
