/*
 * pluto_snap — SW6 rendered-snapshot cache (contract header).
 *
 * After a successful Full/Inline-mode render, the FINAL WALK OUTPUT of the
 * page (blocks/links/tables/maps/datalists + title/baseUrl/metaRefresh) is
 * serialized into the persistent spill store (pluto_spill_store_*, the SW4
 * family — a second user of the same key-addressed file machinery). A
 * revisit within the TTL loads the snapshot: network 0, parse 0, engine 0 —
 * layout builds directly from the restored walk output (document_rewalk
 * proves that path is self-sufficient). The snapshot IS the post-JS render,
 * because the walk output is captured after scripts ran.
 *
 * Every failure path returns NULL / 0 and the caller falls back to the
 * classic network render — the cache only speeds up, never breaks.
 */
#ifndef PLUTO_SNAP_H
#define PLUTO_SNAP_H

#include <stddef.h>
#include "html/document.h"

/* Serialize doc's walk output under (url, mode). Skips about: pages and
 * parse errors. Returns stored byte count, 0 on failure/skip. */
long pluto_snap_save(const DocParseResult *doc, const char *url, int mode,
                     unsigned long epochSeconds);

/* Load the snapshot for (url, mode) if present and fresh. Returns a heap
 * DocParseResult whose walk output is restored into a fresh doc arena
 * (document_free-compatible); NULL on miss/expired/corrupt. The restored
 * doc has NO live DOM/JS bridge (_dom/_jsbridge NULL): links navigate,
 * clicks fall back to plain navigation, and a reload re-attaches the real
 * pipeline. nowEpoch=0 or ttlSeconds=0 disables the age check (host tests). */
DocParseResult *pluto_snap_load(const char *url, int mode,
                                unsigned long nowEpoch,
                                unsigned long ttlSeconds);

/* Explicit invalidation (hard reload, version bump). */
void pluto_snap_invalidate(const char *url, int mode);
void pluto_snap_invalidate_all(void);

/* Storage-pressure eviction: keep at most maxEntries snapshot entries,
 * evicting oldest-registration-first. Returns entries removed. */
int pluto_snap_lru_sweep(int maxEntries);

#endif /* PLUTO_SNAP_H */
