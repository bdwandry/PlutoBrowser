#ifndef PLUTO_RENDER_DECODERS_SELFTEST_WEBP_H
#define PLUTO_RENDER_DECODERS_SELFTEST_WEBP_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_webp_set_pd(struct PlaydateAPI* pd);

int selftest_webp_run(int* passed, int* failed);

#endif
