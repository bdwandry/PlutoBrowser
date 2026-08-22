#ifndef PLUTO_CORE_SELFTEST_BROWSER_H
#define PLUTO_CORE_SELFTEST_BROWSER_H

/* [P32] selftests for the browser engine; results land in pluto.log */

#ifdef __cplusplus
extern "C" {
#endif

struct PlaydateAPI;
void selftest_browser_run(struct PlaydateAPI* pd, int* pass, int* fail);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_CORE_SELFTEST_BROWSER_H
