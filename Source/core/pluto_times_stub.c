/*
 * PlutoBrowser — pluto_times_stub.c
 * newlib `clock()` (used by TEMP CSS stall instrumentation in
 * Source/html/css.c) lowers to the `_times` syscall, which the Playdate
 * firmware does not implement (link failure). Stub it: return -1 (error)
 * so clock() reports unavailable. Nothing else in the tree calls
 * times()/clock(); the instrumentation values are only read on the
 * simulator (real libc there).
 *
 * DEVICE ONLY (Makefile ifeq device block): the simulator links the real
 * macOS libc. Same precedent as dav1d_pthread_shim.c. REMOVE with the
 * CSS instrumentation if clock() use goes away.
 */
#include <sys/times.h>

clock_t _times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}
