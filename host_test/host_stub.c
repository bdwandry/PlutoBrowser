#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int tasks_yield_check(void) { return 0; }
void tasks_report_progress(double p) { (void)p; }

void logger_log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout);
}
void logger_error_loc(const char* file, int line, const char* fmt, ...) {
    printf("ERROR: %s:%d ", file, line);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout);
}

int selftest_document_run(int* passed, int* failed);

int main(void) {
    int p = 0, f = 0;
    int r = selftest_document_run(&p, &f);
    printf("RESULT passed=%d failed=%d rc=%d\n", p, f, r);
    return r == 0 ? 0 : 1;
}