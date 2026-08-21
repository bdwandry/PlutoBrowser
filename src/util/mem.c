// mem.c — allocator wrapper implementation.
//
// All PlutoBrowser heap traffic funnels through here so we can (a) route to
// playdate->system->realloc on device/simulator and (b) swap in a host
// allocator for future out-of-sim test harnesses without touching modules.

#include "mem.h"

#include <stdlib.h>
#include <string.h>

#include "../core/constants.h"
#include "../core/logger.h"
#include "pd_api.h"

static struct PlaydateAPI* g_pd = NULL;

void mem_init(struct PlaydateAPI* playdate)
{
    g_pd = playdate;
}

void* pluto_malloc(size_t size)
{
    if (g_pd != NULL && g_pd->system != NULL && g_pd->system->realloc != NULL) {
        return g_pd->system->realloc(NULL, size);
    }
    return malloc(size);
}

void* pluto_realloc(void* ptr, size_t newSize)
{
    if (g_pd != NULL && g_pd->system != NULL && g_pd->system->realloc != NULL) {
        return g_pd->system->realloc(ptr, newSize);
    }
    return realloc(ptr, newSize);
}

void pluto_free(void* ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (g_pd != NULL && g_pd->system != NULL && g_pd->system->realloc != NULL) {
        g_pd->system->realloc(ptr, 0);
        return;
    }
    free(ptr);
}

char* pluto_strdup(const char* s)
{
    if (s == NULL) {
        return NULL;
    }
    return pluto_strndup(s, strlen(s));
}

char* pluto_strndup(const char* s, size_t n)
{
    char* copy;
    if (s == NULL) {
        return NULL;
    }
    copy = (char*)pluto_malloc(n + 1);
    if (copy == NULL) {
        PLUTO_ERROR("OOM strndup %u", (unsigned)n);
        return NULL;
    }
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}
