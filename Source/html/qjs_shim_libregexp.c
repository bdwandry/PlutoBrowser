/*
 * PlutoBrowser — qjs_shim_libregexp.c
 * Compile-time adapter TU for the STOCK vendored QuickJS regexp engine
 * (see qjs_shim_quickjs.c for the full rationale: the js_* allocator
 * renames here give QuickJS's helpers distinct symbols so muJS and QuickJS
 * can coexist in one binary; Source/js is never modified).
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

#include "../js/QuickJS/libregexp.c"
