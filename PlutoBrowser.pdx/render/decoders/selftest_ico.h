#ifndef PLUTO_RENDER_DECODERS_SELFTEST_ICO_H
#define PLUTO_RENDER_DECODERS_SELFTEST_ICO_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_ico_set_pd(struct PlaydateAPI* pd);

int selftest_ico_run(int* passed, int* failed);

#endif
