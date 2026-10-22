/*
 * PlutoBrowser — pluto_times_stub.c
 * newlib `clock()` (used by TEMP CSS stall instrumentation in
 * Source/html/css.c) lowers to the `_times` syscall, which the Playdate
 * firmware does not implement (link failure). Stub it: return -1 (error)
 * so clock() reports unavailable. Nothing else in the tree calls
 * times()/clock(); the instrumentation values are only read on the
 * simulator (real libc there).
 *
 * DEVICE ONLY on real body: the guard below keeps `_times` for the device
 * build and compiles the TU to NOTHING under the simulator compiler —
 * plain `make` feeds this TU to the simulator dylib too, where a defined
 * `_times` would shadow the real macOS libc symptom; the instrumentation
 * values are read on the simulator through the REAL libc. REMOVE with the
 * CSS instrumentation if clock() use goes away.
 */
#ifdef TARGET_PLAYDATE
#include <sys/times.h>

clock_t _times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}
#endif /* TARGET_PLAYDATE */

