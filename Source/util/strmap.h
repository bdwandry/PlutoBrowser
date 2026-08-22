// strmap.h — string-keyed hash map (string -> void*).
//
// Replaces Lua tables used as string-keyed dictionaries (attribute maps,
// entity tables, cookie jars, caches). Keys are copied on insert; values are
// caller-owned pointers passed through untouched.

#ifndef PLUTO_STRMAP_H
#define PLUTO_STRMAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StrMap StrMap;

StrMap* sm_create(size_t initialBuckets);
void    sm_destroy(StrMap* m);

// Copies key. Replaces value if key already present. 0 on OOM.
int     sm_put(StrMap* m, const char* key, void* value);
void*   sm_get(const StrMap* m, const char* key); // NULL if absent
int     sm_has(const StrMap* m, const char* key);
int     sm_remove(StrMap* m, const char* key);    // 1 if removed
size_t  sm_count(const StrMap* m);

typedef void (*SMIterFn)(const char* key, void* value, void* userdata);
void    sm_foreach(StrMap* m, SMIterFn fn, void* userdata);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_STRMAP_H
