/*
 * pluto_mem — heap telemetry + guarded allocation (SW1/SW3a foundation).
 * See pluto_mem.h for the contract.
 *
 * NOTE on recursion: the logger never allocates through this funnel
 * (logger_log writes through the SDK's own file plumbing), so accounting
 * calls made here cannot re-enter.
 */
#include "pluto_mem.h"
#include <string.h>

/* Call the SDK allocator DIRECTLY — never via main.c's pluto_realloc (which
 * routes INTO this funnel; that would recurse infinitely). The host suites
 * provide their own pluto_mem_sdk_realloc via the fake PD API. */
extern void *pluto_mem_sdk_realloc(void *p, size_t n);

/* ── accounting state ──────────────────────────────────────────────────────── */
static unsigned long mem_live = 0;
static unsigned long mem_peak = 0;
static unsigned long mem_peak_alloc = 0;
static unsigned long mem_refusals = 0;
static unsigned long mem_budget = 0; /* 0 = telemetry only */

/* Live-delta accounting for realloc is size-based: we cannot know the old
 * block's true size portably, so callers' FREE (n==0) and realloc paths
 * track deltas by having the funnel remember the last size it handed out
 * per pointer is NOT portable either — instead we account conservatively:
 * allocations add n, frees subtract nothing (relying on the caller-visible
 * live number being an upper bound) — NO: that drifts upward forever.
 *
 * Correct portable approach: wrap with explicit size tracking at the two
 * call shapes we control. realloc(ptr, n) on a tracked pointer returns the
 * delta (n - old_n); free(ptr, 0) subtracts the pointer's tracked size.
 * A tiny open-addressing table keyed by pointer holds tracked sizes.
 * Overhead: 24B entry per live allocation, table sized for our scale. */
#define MEMTRACK_SLOTS 2048 /* power of two */

typedef struct
{
    void *ptr;
    size_t size;
} MemTrack;

static MemTrack track[MEMTRACK_SLOTS];

static unsigned track_slot(const void *p)
{
    unsigned h = (unsigned)(((unsigned long)p) >> 4);
    return h & (MEMTRACK_SLOTS - 1);
}

static MemTrack *track_find(const void *p)
{
    unsigned i = track_slot(p);
    for (int probe = 0; probe < 8; probe++)
    {
        MemTrack *e = &track[(i + probe) & (MEMTRACK_SLOTS - 1)];
        if (e->ptr == p)
        {
            return e;
        }
        if (e->ptr == NULL)
        {
            return NULL; /* empty slot stops the probe chain */
        }
    }
    return NULL; /* not found (foreign pointer: host tests, stack, etc.) */
}

static MemTrack *track_insert(void *p, size_t size)
{
    unsigned i = track_slot(p);
    for (int probe = 0; probe < 8; probe++)
    {
        MemTrack *e = &track[(i + probe) & (MEMTRACK_SLOTS - 1)];
        if (e->ptr == NULL)
        {
            e->ptr = p;
            e->size = size;
            return e;
        }
    }
    return NULL; /* table pressure: skip tracking this block (still counted) */
}

static void track_remove(const void *p)
{
    MemTrack *e = track_find(p);
    if (e)
    {
        e->ptr = NULL;
        e->size = 0;
    }
}

void *pluto_mem_realloc(void *ptr, size_t n)
{
    /* Budget gate (SW3a): refuse BEFORE the SDK heap is touched. A refusal
     * is a clean NULL the engines/callers already handle as OOM. Frees and
     * shrinks are never refused. */
    if (mem_budget && n)
    {
        MemTrack *old = ptr ? track_find(ptr) : NULL;
        unsigned long projected = mem_live - (old ? old->size : 0) + n;
        if (projected > mem_budget)
        {
            mem_refusals++;
            return NULL;
        }
    }

    /* Free FIRST: realloc(p, 0) frees and typically returns NULL, so the
     * NULL check below must not skip the accounting. */
    if (n == 0)
    {
        (void)pluto_mem_sdk_realloc(ptr, 0);
        MemTrack *old = ptr ? track_find(ptr) : NULL;
        if (old && old->size)
        {
            mem_live -= old->size;
        }
        track_remove(ptr);
        return NULL;
    }

    void *np = pluto_mem_sdk_realloc(ptr, n);

    if (!np)
    {
        return NULL; /* SDK OOM (or refusal) — old block untouched, tracked */
    }

    if (!ptr)
    {
        /* Fresh allocation. */
        mem_live += n;
        if (n > mem_peak_alloc)
        {
            mem_peak_alloc = n;
        }
        track_insert(np, n);
    }
    else
    {
        /* Resize: find the old tracked size, adjust live. */
        MemTrack *old = track_find(ptr);
        size_t oldSize = (old && old->size) ? old->size : 0;
        if (n >= oldSize)
        {
            mem_live += (n - oldSize);
        }
        else
        {
            mem_live -= (oldSize - n);
        }
        if (old)
        {
            if (np != ptr)
            {
                old->ptr = np; /* moved: retarget the entry */
            }
            old->size = n;
        }
        else
        {
            track_insert(np, n); /* untracked before (foreign) — track now */
        }
    }

    if (mem_live > mem_peak)
    {
        mem_peak = mem_live;
    }
    return np;
}

/* calloc: allocate + zero (XS tables require zeroed memory). */
void *pluto_mem_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    void *p = pluto_mem_realloc(NULL, total);
    if (p)
    {
        memset(p, 0, total);
    }
    return p;
}

unsigned long pluto_mem_live(void) { return mem_live; }
unsigned long pluto_mem_peak(void) { return mem_peak; }
unsigned long pluto_mem_peak_alloc(void) { return mem_peak_alloc; }
unsigned long pluto_mem_refusals(void) { return mem_refusals; }
void pluto_mem_set_budget(unsigned long bytes) { mem_budget = bytes; }
unsigned long pluto_mem_budget(void) { return mem_budget; }
unsigned long pluto_mem_headroom_bytes(void)
{
    if (!mem_budget)
    {
        return (unsigned long)-1 / 2; /* gate disabled = effectively unlimited */
    }
    return (mem_live < mem_budget) ? (mem_budget - mem_live) : 0;
}

void pluto_mem_peak_reset(void)
{
    mem_peak = mem_live;
}
