/*
 * PlutoBrowser — qjs_pthread_stubs.c
 * No-op POSIX-threading primitives for the STOCK vendored QuickJS on the
 * single-threaded Playdate (Source/js/QuickJS — never modified).
 *
 * Why this exists: quickjs.c enables CONFIG_ATOMICS (its Atomics.*
 * intrinsics) and uses a pthread mutex + condition variables there, plus a
 * static mutex guarding JS_NewClassID. The Playdate runtime is strictly
 * single-threaded (one cooperative game task, no preemption), so:
 *   - a static-initialized "0xFFFFFFFF"-style mutex is never contended;
 *     lock/unlock are no-ops;
 *   - condvars can never be waited on in practice: CONFIG_ATOMICS is only
 *     reachable through Atomics.wait()/Atomics.notify(), which require a
 *     SharedArrayBuffer — and QuickJS exposes no way to create one. The
 *     waiting paths still fail safely (nonzero return = error) so a future
 *     caller can never hang the browser task.
 * These are OUR symbols (pluto_qjs_pthread_*), wired to the engine via the
 * -D map in the Makefile; the vendored sources are not touched.
 */
#include "../core/logger.h"

int pluto_qjs_pthread_mutex_lock(void *m)
{
    (void)m;
    return 0; /* single-threaded: never contended */
}

int pluto_qjs_pthread_mutex_unlock(void *m)
{
    (void)m;
    return 0;
}

int pluto_qjs_pthread_cond_init(void *c, const void *a)
{
    (void)c;
    (void)a;
    return 0;
}

int pluto_qjs_pthread_cond_destroy(void *c)
{
    (void)c;
    return 0;
}

int pluto_qjs_pthread_cond_signal(void *c)
{
    (void)c;
    return 0;
}

int pluto_qjs_pthread_cond_wait(void *c, void *m)
{
    (void)m;
    logger_log("[js] Atomics.wait blocked (no threads) — failing safely");
    return 1; /* nonzero = error; never blocks the browser task */
}

int pluto_qjs_pthread_cond_timedwait(void *c, void *m, const void *t)
{
    (void)c;
    (void)m;
    (void)t;
    logger_log("[js] Atomics.wait timedwait (no threads) — failing safely");
    return 1; /* ETIMEDOUT-style failure, never blocks */
}

/* ── 64-bit atomics + clock stubs ──────────────────────────────────────────
 * The Cortex-M7 toolchain (arm gcc 9.2, no libatomic) cannot resolve the
 * __atomic_*_8 builtins, and newlib's clock_gettime has no implementation.
 * Every reference lives in js_atomics_op / js_atomics_wait — the
 * Atomics.* intrinsics that REQUIRE a SharedArrayBuffer, which QuickJS
 * provides no way to create. Unreachable in practice; they fail loudly
 * and safely if ever reached.
 */
#include <stdint.h>

/* Exact GCC libfunc ABI signatures — the same contract libatomic fulfills,
 * minus the *_1 variants (8-byte aligned loads/stores need no helper). The
 * signatures must match the compiler built-ins exactly or GCC emits
 * -Wbuiltin-declaration-mismatch for every one of them. */
#define PLUTO_QJS_ATOMIC_RMW(name)                                  \
    unsigned long long name(volatile void *p,                       \
                            unsigned long long v, int memorder)     \
    {                                                               \
        (void)p;                                                    \
        (void)v;                                                    \
        (void)memorder;                                             \
        logger_log("[js] 64-bit Atomics op reached (no SAB support) — failing safely"); \
        return 0;                                                   \
    }

PLUTO_QJS_ATOMIC_RMW(__atomic_fetch_add_8)
PLUTO_QJS_ATOMIC_RMW(__atomic_fetch_sub_8)
PLUTO_QJS_ATOMIC_RMW(__atomic_fetch_and_8)
PLUTO_QJS_ATOMIC_RMW(__atomic_fetch_or_8)
PLUTO_QJS_ATOMIC_RMW(__atomic_fetch_xor_8)
PLUTO_QJS_ATOMIC_RMW(__atomic_exchange_8)

unsigned long long __atomic_load_8(const volatile void *p, int memorder)
{
    (void)p;
    (void)memorder;
    logger_log("[js] 64-bit Atomics load reached (no SAB support) — failing safely");
    return 0;
}

void __atomic_store_8(volatile void *p, unsigned long long v, int memorder)
{
    (void)p;
    (void)v;
    (void)memorder;
    logger_log("[js] 64-bit Atomics store reached (no SAB support) — failing safely");
}

_Bool __atomic_compare_exchange_8(volatile void *p, void *expected,
                                  unsigned long long desired, _Bool weak,
                                  int success_memorder,
                                  int failure_memorder)
{
    (void)p;
    (void)expected;
    (void)desired;
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    logger_log("[js] 64-bit Atomics CAS reached (no SAB support) — failing safely");
    return 0; /* false = "no exchange happened" */
}

int pluto_qjs_clock_gettime(int clk, void *ts)
{
    (void)clk;
    (void)ts;
    return -1; /* error: only used by the unreachable Atomics.wait path */
}
