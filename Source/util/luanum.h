// luanum.h — Lua-semantic numeric string conversion.
//
// Mirrors Lua's tonumber(s) for decimal/hex/exponent forms: leading and
// trailing whitespace allowed, ENTIRE string must consume, else failure
// ("8080abc", "12,5" fail; " 42 ", "0x50", "1e3" succeed).

#ifndef PLUTO_LUANUM_H
#define PLUTO_LUANUM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 and fills *out on success, 0 on failure.
int pluto_str_tonumber_strict(const char* s, double* out);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_LUANUM_H
