#ifndef PLUTO_RENDER_DECODERS_SELFTEST_JPEG_H
#define PLUTO_RENDER_DECODERS_SELFTEST_JPEG_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_jpeg_set_pd(struct PlaydateAPI* pd);

int selftest_jpeg_run(int* passed, int* failed);

#endif
