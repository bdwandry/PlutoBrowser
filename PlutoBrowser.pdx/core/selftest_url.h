// selftest_url.h — P03 core/url self-test harness.
//
// Every expectation below was captured by running the REAL CometBrowser
// Source/core/url.lua under host Lua (parity oracle), so these are ground-
// truth values, not re-derivations. Runs at boot; logs "[P03] PASS/FAIL".

#ifndef PLUTO_SELFTEST_URL_H
#define PLUTO_SELFTEST_URL_H

#ifdef __cplusplus
extern "C" {
#endif

void selftest_url_run(int* outPass, int* outFail);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_SELFTEST_URL_H
