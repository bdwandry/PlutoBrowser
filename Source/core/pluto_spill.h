/*
 * pluto_spill — RAM↔disk streaming storage (SW2a, the SW track's core).
 *
 * CONTRACT: disk holds BULK page data; RAM holds a bounded window. The
 * browser's RAM caps become RESIDENCY limits, not download limits — a
 * 502KB script or a 2MB page body lands on disk no matter how big, and
 * consumers read it back through pluto_spill_read() in bounded chunks.
 *
 * DESIGN
 *   - One spill DIRECTORY: Data/<bundle>/spill/ (device) or a temp dir
 *     (host builds without the SDK), created on first use.
 *   - pluto_spill_begin() opens a NEW spill file (one per resource) and
 *     returns a handle; pluto_spill_write() appends (called from the HTTP
 *     progress path with whatever the bounded network buffer holds);
 *     pluto_spill_finish() closes it and records the total size.
 *   - Reads: pluto_spill_read(h, offset, buf, len) — position-independent,
 *     so consumers (parse, engine feed) pull chunks in any order.
 *   - Naming: spill files are named by a monotonically increasing handle
 *     id (spill_<id>.bin) — content-addressing comes later (SW6 cache).
 *   - Lifecycle: pluto_spill_reset() deletes ALL spill files (called on
 *     page close / navigation); spill is EPHEMERAL working storage, not a
 *     cache. A persistent cache is a later stage (SW6) with LRU + quota.
 *   - Accounting: every byte written/read flows through the funnel-aware
 *     bounded buffer discipline — writes take what the network gave,
 *     reads copy out of a small static window; no allocation grows with
 *     resource size.
 *   - Flash wear: write-once per fetch (append-only, then read-only).
 *     reset() unlinks; no in-place rewriting, no churn.
 *
 * Source/js stays stock: engines see this layer only through OUR bridge
 * code feeding them bytes from pluto_spill_read().
 */
#ifndef PLUTO_SPILL_H
#define PLUTO_SPILL_H

#include <stddef.h>

/* Invalid handle. */
#define PLUTO_SPILL_INVALID (-1)

typedef int SpillFile; /* handle */

/* Prepare the spill directory. Safe to call repeatedly; returns 0 ok,
 * -1 on filesystem failure (callers must then fall back to RAM-only). */
int pluto_spill_init(void);

/* Begin a new spill file. Returns a handle or PLUTO_SPILL_INVALID. */
SpillFile pluto_spill_begin(void);

/* Append n bytes. Returns 0 ok, -1 on write failure. */
int pluto_spill_write(SpillFile h, const void *data, size_t n);

/* Finish and close. Returns total bytes written, or -1 on failure.
 * The handle stays valid for reads until reset. */
long pluto_spill_finish(SpillFile h);

/* Total bytes in a finished (or in-progress) spill file. */
long pluto_spill_size(SpillFile h);

/* Read len bytes at offset into buf. Returns bytes read (0 = EOF). */
long pluto_spill_read(SpillFile h, long offset, void *buf, size_t len);

/* Close + delete ONE spill file now (handle dies, slot freed). Use when a
 * consumer materialized the bytes into RAM or refused the file — orphan
 * cleanup, not a teardown. Returns 0 ok, -1 bad handle. */
int pluto_spill_discard(SpillFile h);

/* Close + delete every spill file (page teardown). All handles die. */
void pluto_spill_reset(void);

/* Total spill bytes live right now (diagnostics). */
long pluto_spill_total(void);

/* ── SW4: named bytecode store (persistent across spill_reset) ─────────
 * Session spill files die with the page (reset on navigate); compiled
 * QuickJS bytecode must SURVIVE navigation to pay off on revisits. The
 * store is a second, persistent file family: name-addressed (32-bit key
 * of source bytes + length, computed in OUR code — engines stay stock),
 * enumerated by index, deleted only by explicit invalidate or the
 * boot-time quota sweep. Same bounded-slot machinery as session spill.
 *   - store_open_create(key): open for WRITE (create/truncate); returns
 *     a handle usable with pluto_spill_write/finish/read/size/discard.
 *   - store_find(key): 1 if a stored file exists (exact key match).
 *   - store_open_read(key): open existing for READ.
 *   - store_count/store_key_at/store_delete_at: enumeration + eviction.
 *   - store_invalidate_all(): wipe the family (config/version bumps).
 */
SpillFile pluto_spill_store_open_create(unsigned long key);
int pluto_spill_store_find(unsigned long key);
SpillFile pluto_spill_store_open_read(unsigned long key);
/* SW6: release a store READ handle WITHOUT deleting the entry (the read
 * path reopens the file per read() and must not leak the reopened FILE*).
 * Session spill files have spill_finish/discard; finished store slots need
 * this to return the slot to its closed, resident state. */
void pluto_spill_store_close(SpillFile h);
/* SW6: store-table index of an exact key, or -1 (delete_by_key helper). */
int pluto_spill_store_index_of(unsigned long key);
int pluto_spill_store_count(void);
unsigned long pluto_spill_store_key_at(int idx);
int pluto_spill_store_delete_at(int idx);
void pluto_spill_store_invalidate_all(void);

#endif /* PLUTO_SPILL_H */
