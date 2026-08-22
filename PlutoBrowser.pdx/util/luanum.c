// luanum.c — see header.

#include "luanum.h"

#include <stdlib.h>

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

int pluto_str_tonumber_strict(const char* s, double* out)
{
    char* endp;
    double v;

    if (s == NULL || *s == '\0') {
        return 0;
    }
    v = strtod(s, &endp);
    if (endp == s) {
        return 0;
    }
    while (*endp != '\0' && is_ws(*endp)) {
        endp++;
    }
    if (*endp != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}
