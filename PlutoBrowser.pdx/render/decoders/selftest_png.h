#ifndef PLUTO_RENDER_DECODERS_SELFTEST_PNG_H
#define PLUTO_RENDER_DECODERS_SELFTEST_PNG_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_png_set_pd(struct PlaydateAPI* pd);

int selftest_png_run(int* passed, int* failed);

#endif
