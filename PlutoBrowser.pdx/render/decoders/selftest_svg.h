#ifndef PLUTO_RENDER_DECODERS_SELFTEST_SVG_H
#define PLUTO_RENDER_DECODERS_SELFTEST_SVG_H

struct PlaydateAPI;

/* device builds: install API pointer for benchmark timing */
void selftest_svg_set_pd(struct PlaydateAPI* pd);

int selftest_svg_run(int* passed, int* failed);

#endif
