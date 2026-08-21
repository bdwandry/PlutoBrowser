// selftest_storage.h — P04 storage + cookie-jar self-test harness.
//
// Expectations captured from the REAL CometBrowser Lua sources running
// under host lua with a shimmed playdate table (fixed clock, in-memory
// datastore). Runs at boot; logs "[P04] PASS/FAIL <name>" lines.

#ifndef PLUTO_SELFTEST_STORAGE_H
#define PLUTO_SELFTEST_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

void selftest_storage_run(int* outPass, int* outFail);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_SELFTEST_STORAGE_H
