// selftest_util.h — P02 util-layer self-test harness.
//
// Runs once at boot; each assertion logs a "[P02] PASS/FAIL name" line to
// pluto.log and the summary is drawn on the placeholder screen. Kept
// permanently as boot diagnostics (logging is permanent per project rules).

#ifndef PLUTO_SELFTEST_UTIL_H
#define PLUTO_SELFTEST_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

// Runs all util self-tests. Fills *outPass/*outFail with totals.
void selftest_util_run(int* outPass, int* outFail);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_SELFTEST_UTIL_H
