// selftest_tasks.h — P05 scheduler verification suite entry point.
#ifndef PLUTO_SELFTEST_TASKS_H
#define PLUTO_SELFTEST_TASKS_H

// Requires tasks_init(pd) beforehand. Logs "[P05] ..." lines to pluto.log.
void selftest_tasks_run(int* outPass, int* outFail);

#endif // PLUTO_SELFTEST_TASKS_H
