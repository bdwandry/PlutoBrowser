#ifndef PLUTO_RENDER_DECODERS_SELFTEST_DECODERS_H
#define PLUTO_RENDER_DECODERS_SELFTEST_DECODERS_H

struct PlaydateAPI;

int selftest_decoders_run(int* passed, int* failed);

/* simulator/device only: real-bitmap render cross-check */
void selftest_decoders_device(struct PlaydateAPI* pd);

#endif
