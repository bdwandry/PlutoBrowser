// selftest_http.h — P07 raw TCP HTTP client verification suite entry point.
#ifndef PLUTO_SELFTEST_HTTP_H
#define PLUTO_SELFTEST_HTTP_H

// Requires hc_init(pd) beforehand (real vtable/clock are swapped out for the
// offline fake TCP + fake clock inside). Logs "[P07] ..." lines to pluto.log.
void selftest_http_run(int* outPass, int* outFail);

#endif // PLUTO_SELFTEST_HTTP_H
