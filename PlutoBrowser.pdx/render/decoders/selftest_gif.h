#ifndef PLUTO_RENDER_DECODERS_SELFTEST_GIF_H
#define PLUTO_RENDER_DECODERS_SELFTEST_GIF_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_gif_set_pd(struct PlaydateAPI* pd);

int selftest_gif_run(int* passed, int* failed);

#endif
