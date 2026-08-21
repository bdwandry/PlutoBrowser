// mem.h — central allocator wrapper for PlutoBrowser.
//
// Routes all heap allocation through playdate->system->realloc when the API
// pointer is installed (device + simulator), falling back to libc malloc so
// the same modules can later run in a host-side test harness.

#ifndef PLUTO_MEM_H
#define PLUTO_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlaydateAPI;

// Install the Playdate API pointer used for allocation. Call once at boot
// before any module allocates. Safe to call again (re-install).
void mem_init(struct PlaydateAPI* playdate);

void* pluto_malloc(size_t size);
void* pluto_realloc(void* ptr, size_t newSize);
void  pluto_free(void* ptr);

char* pluto_strdup(const char* s);            // NUL-terminated convenience
char* pluto_strndup(const char* s, size_t n); // always NUL-terminates

#ifdef __cplusplus
}
#endif

#endif // PLUTO_MEM_H
