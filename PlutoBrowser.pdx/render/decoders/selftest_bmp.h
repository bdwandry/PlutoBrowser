#ifndef PLUTO_RENDER_DECODERS_SELFTEST_BMP_H
#define PLUTO_RENDER_DECODERS_SELFTEST_BMP_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_bmp_set_pd(struct PlaydateAPI* pd);

int selftest_bmp_run(int* passed, int* failed);

#endif
