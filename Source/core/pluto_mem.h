/*
 * pluto_mem — heap telemetry + guarded allocation (SW1/SW3a foundation).
 *
 * ONE funnel for every app+engine allocation: call sites that used
 * pluto_pd()->system->realloc call pluto_mem_realloc() instead. That is the
 * exact same SDK call underneath (zero behavior change), wrapped with:
 *   - live byte counter + all-time peak + last allocation stamp,
 *   - a SOFT BUDGET: requests that would exceed pluto_mem_budget() fail
 *     (return NULL) BEFORE the SDK heap is touched — the engines already
 *     treat NULL as OOM and fail cleanly ("script skipped, page continues").
 *
 * The budget defaults to disabled (0) so this module is pure telemetry
 * until SW3 turns it into the automatic RAM-vs-disk placement policy.
 *
 * Source/js stays STOCK: the engines reach this funnel through the same
 * hooks they already used (muJS js_alloc, Duktape heap fns, QuickJS
 * JSMallocFunctions) — only OUR call sites change, plus XS via the
 * c_malloc/c_realloc route-through defines in xs_platform.h.
 */
#ifndef PLUTO_MEM_H
#define PLUTO_MEM_H

#include <stddef.h>

/* Route one allocation through the SDK with accounting.
 * Semantics match realloc exactly: ptr==NULL + n>0 = alloc; n==0 = free
 * (returns NULL); ptr!=NULL + n>0 = resize. NULL return = refused or OOM. */
void *pluto_mem_realloc(void *ptr, size_t n);

/* Live bytes currently held through the funnel. */
unsigned long pluto_mem_live(void);

/* All-time high-water mark (bytes). Never decreases until reset. */
unsigned long pluto_mem_peak(void);

/* Reset the peak to the current live value (e.g. at page-attach). */
void pluto_mem_peak_reset(void);

/* Size of the largest single allocation ever seen through the funnel. */
unsigned long pluto_mem_peak_alloc(void);

/* Soft budget in bytes; 0 disables enforcement (pure telemetry). */
void pluto_mem_set_budget(unsigned long bytes);
unsigned long pluto_mem_budget(void);

/* SW3a: bytes between the live heap and the soft budget — the headroom a NEW
 * allocation may safely claim before the heap-pressure policy (jsext auto
 * RAM→disk placement) should spill instead. Returns the full budget when the
 * gate is disabled (budget 0), so callers see "unlimited" headroom and keep
 * the legacy pure-RAM behavior in host tests. */
unsigned long pluto_mem_headroom_bytes(void);

/* Count of allocations refused by the budget (diagnostic). */
unsigned long pluto_mem_refusals(void);

#endif /* PLUTO_MEM_H */
