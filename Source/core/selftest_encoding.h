// selftest_encoding.h — P06 charset/entity verification suite entry point.
#ifndef PLUTO_SELFTEST_ENCODING_H
#define PLUTO_SELFTEST_ENCODING_H

// Requires encoding_init(pd) + entities_init(pd) beforehand.
// Logs "[P06] ..." lines to pluto.log.
void selftest_encoding_run(int* outPass, int* outFail);

#endif // PLUTO_SELFTEST_ENCODING_H
