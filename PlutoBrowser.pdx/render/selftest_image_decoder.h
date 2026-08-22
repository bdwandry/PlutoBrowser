#ifndef PLUTO_RENDER_SELFTEST_IMAGE_DECODER_H
#define PLUTO_RENDER_SELFTEST_IMAGE_DECODER_H

struct PlaydateAPI;

/* device builds: install API pointer */
void selftest_image_decoder_set_pd(struct PlaydateAPI* pd);

int selftest_image_decoder_run(int* passed, int* failed);

#endif
