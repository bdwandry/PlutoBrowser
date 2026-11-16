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
#include <sys/time.h>
#include <stddef.h>
#include <time.h>
#include "pd_api.h"
#include "core/logger.h"

extern PlaydateAPI *pluto_pd(void);

/* newlib's _findenv_r() (and therefore localtime()/mktime() via tzset())
 * reads the global `environ` array. The game task's environment pointer is
 * not valid on hardware and faulted while the projects page formatted
 * GitHub card dates. Clearing it makes _findenv_r() return immediately, so
 * newlib uses its default UTC zone. Date formatting only; DOM/layout and
 * rendering are untouched. */
extern char **environ;

clock_t _times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}

void pluto_env_sanitize(void)
{
    environ = NULL;
}

/* Device probe: XS Date formatting reaches c_localtime; route it through
 * gmtime and log the call so a date-path crash is attributable. The local
 * timezone is unavailable on hardware (the environment is invalid), so UTC
 * is the safe breakdown. */
struct tm *pluto_date_gmtime(const time_t *t)
{
    logger_log("[date] c_localtime enter t=%ld", t ? (long)*t : -1L);
    struct tm *tm = gmtime(t);
    logger_log("[date] c_localtime leave tm=%p", (void *)tm);
    return tm;
}

/* XS's fxDateNow() calls c_gettimeofday() and uses the result without
 * checking the return value. newlib's device implementation is not a
 * reliable clock here and can leave the timeval uninitialised, which made
 * the projects page's Date.now()/savedAt non-finite ("Invalid Date"). The
 * Playdate system table owns the device clock, including epoch seconds. */
int pluto_date_gettimeofday(struct timeval *tp, void *tzp)
{
    (void)tzp;
    if (!tp)
    {
        return 0;
    }
    unsigned int ms = 0;
    unsigned int secs = 0;
    PlaydateAPI *pd = pluto_pd();
    if (pd && pd->system && pd->system->getSecondsSinceEpoch)
    {
        secs = pd->system->getSecondsSinceEpoch(&ms);
    }
    tp->tv_sec = (time_t)secs;
    tp->tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    logger_log("[date] gettimeofday secs=%u ms=%u", secs, ms);
    return 0;
}
#endif /* TARGET_PLAYDATE */

