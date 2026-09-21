/*
 * pluto_spill — implementation. See pluto_mem.h contract header.
 *
 * Device: the SDK file API, rooted in the game's Data sandbox when opened
 * with kFileReadData; writes use kFileWrite with explicit "spill/" prefix
 * (the SDK creates Data/<bundle>/ implicitly for data reads; the spill
 * subfolder is created via file->mkdir on first use).
 *
 * Host (unit tests / lab builds without the SDK): plain stdio in /tmp.
 * The host suites fake pluto_pd(); spill deliberately does NOT depend on
 * pluto_pd at all — it uses the PLUTO_SPILL_HOST define to pick stdio, so
 * tests exercise the identical handle/read/write logic.
 */
#include "pluto_spill.h"

#ifdef PLUTO_SPILL_HOST
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include "pd_api.h"
extern PlaydateAPI *pluto_pd(void);
#endif

#include <string.h>

#define SPILL_MAX_FILES 12   /* open files at once (bounded) */
#define SPILL_NAME_MAX 128 /* bc_<id>_<20-digit-key>.bin + dir prefix (SW4) */

typedef struct
{
    int used;   /* slot allocated */
    int open;   /* file currently open for write */
    long size;  /* total bytes written */
    char name[SPILL_NAME_MAX];
#ifdef PLUTO_SPILL_HOST
    FILE *fp;
#else
    SDFile *fp;
#endif
} SpillSlot;

static SpillSlot slots[SPILL_MAX_FILES];
static long spillTotal = 0;
static int spillReady = 0;
static int spillNextId = 1;
static int spillStoreNextId = 1; /* SW4: persistent store name counter */

/* SW4: is this slot a PERSISTENT store file (survives spill_reset)? */
static unsigned char slotIsStore[SPILL_MAX_FILES];
static unsigned long slotKeys[SPILL_MAX_FILES];

#ifdef PLUTO_SPILL_HOST
#define SPILL_DIR "/tmp/plutobrowser_spill"
#endif

int pluto_spill_init(void)
{
#if defined(PLUTO_SPILL_HOST)
    {
        /* Ensure the store/spill folder exists (device path does the same
         * via file->mkdir below). Ignore EEXIST — open() is the arbiter. */
        mkdir(SPILL_DIR, 0755);
    }
    spillReady = 1; /* stdio host test: /tmp works */
    return 0;
#elif defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
    /* Real builds with the SDK file system — device (common.mk DDEFS:
     * -DTARGET_PLAYDATE=1) AND simulator (-DTARGET_SIMULATOR=1, file API
     * rooted at the SDK Disk/ sandbox). CAUGHT BY DEVICE TESTING: the
     * first cut keyed the not-ready branch on !TARGET_SIMULATOR, which
     * swallowed the device (defines TARGET_PLAYDATE, not TARGET_SIMULATOR)
     * and silently disabled spill on hardware ("FAIL jsext-huge.js spill
     * write (no disk?)" — suite 5/7 on device, 7/7 in sim). */
    if (spillReady)
    {
        return 0;
    }
    if (pluto_pd()->file->mkdir("spill") != 0)
    {
        /* EEXIST-style failure is fine; a real failure shows up on open. */
    }
    spillReady = 1;
    return 0;
#else
    /* Detached host-suite builds (no SDK file system, fake pluto_pd):
     * report not-ready so callers fall back to RAM-only (historical
     * behavior). */
    spillReady = 0;
    return -1;
#endif
}

SpillFile pluto_spill_begin(void)
{
    if (pluto_spill_init() != 0)
    {
        return PLUTO_SPILL_INVALID;
    }
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        SpillSlot *s = &slots[i];
        if (s->used)
        {
            continue;
        }
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name),
#ifdef PLUTO_SPILL_HOST
                 SPILL_DIR "/spill_%d.bin",
#else
                 "spill/spill_%d.bin",
#endif
                 spillNextId++);
        s->used = 1;
#ifdef PLUTO_SPILL_HOST
        s->fp = fopen(s->name, "wb+");
#else
        s->fp = pluto_pd()->file->open(s->name, kFileWrite);
#endif
        if (!s->fp)
        {
            s->used = 0;
            return PLUTO_SPILL_INVALID;
        }
        s->open = 1;
        s->size = 0;
        return i;
    }
    return PLUTO_SPILL_INVALID; /* all slots busy */
}

int pluto_spill_write(SpillFile h, const void *data, size_t n)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used || !slots[h].open ||
        !data || n == 0)
    {
        return -1;
    }
    SpillSlot *s = &slots[h];
#ifdef PLUTO_SPILL_HOST
    size_t w = fwrite(data, 1, n, s->fp);
    if (w != n)
    {
        return -1;
    }
#else
    {
        /* Device: write in bounded chunks — a single multi-megabyte
         * file->write is off the firmware's proven path (network spill
         * never exceeded small chunks) and was implicated in a hard
         * fault when the SW4 store wrote a 2.25MB bytecode blob. */
        const unsigned char *p = (const unsigned char *)data;
        size_t left = n;
        while (left > 0)
        {
            unsigned int chunk = (left > 16384u) ? 16384u : (unsigned int)left;
            int w = pluto_pd()->file->write(s->fp, p, chunk);
            if ((long)w != (long)chunk)
            {
                return -1;
            }
            p += chunk;
            left -= chunk;
        }
    }
#endif
    s->size += (long)n;
    spillTotal += (long)n;
    return 0;
}

long pluto_spill_finish(SpillFile h)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used || !slots[h].open)
    {
        return -1;
    }
    SpillSlot *s = &slots[h];
#ifdef PLUTO_SPILL_HOST
    fclose(s->fp);
#else
    pluto_pd()->file->close(s->fp);
#endif
    s->fp = NULL;
    s->open = 0;
    return s->size;
}

long pluto_spill_size(SpillFile h)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used)
    {
        return -1;
    }
    return slots[h].size;
}

long pluto_spill_read(SpillFile h, long offset, void *buf, size_t len)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used || !buf || len == 0)
    {
        return -1;
    }
    SpillSlot *s = &slots[h];
    if (offset < 0 || offset >= s->size)
    {
        return 0; /* EOF */
    }
    if (s->open)
    {
        /* Read of an in-progress write: flush first (host: fflush). */
#ifdef PLUTO_SPILL_HOST
        fflush(s->fp);
#else
        pluto_pd()->file->flush(s->fp);
#endif
    }
    else
    {
        /* Reopen read-only for finished files (bounded open time). */
#ifdef PLUTO_SPILL_HOST
        s->fp = fopen(s->name, "rb");
#else
        s->fp = pluto_pd()->file->open(s->name, kFileReadData);
#endif
        if (!s->fp)
        {
            return -1;
        }
    }
#ifdef PLUTO_SPILL_HOST
    if (fseek(s->fp, offset, SEEK_SET) != 0)
    {
        fclose(s->fp);
        s->fp = NULL;
        return -1;
    }
    long r = (long)fread(buf, 1, len, s->fp);
#else
    if (pluto_pd()->file->seek(s->fp, (int)offset, SEEK_SET) != 0)
    {
        pluto_pd()->file->close(s->fp);
        s->fp = NULL;
        return -1;
    }
    int r = pluto_pd()->file->read(s->fp, buf, (unsigned int)len);
    if (r < 0)
    {
        r = -1;
    }
#endif
    if (!s->open)
    {
        /* Close the temporary read handle. */
#ifdef PLUTO_SPILL_HOST
        fclose(s->fp);
#else
        pluto_pd()->file->close(s->fp);
#endif
        s->fp = NULL;
    }
    return r;
}

int pluto_spill_discard(SpillFile h)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used)
    {
        return -1;
    }
    SpillSlot *s = &slots[h];
    if (s->fp)
    {
#ifdef PLUTO_SPILL_HOST
        fclose(s->fp);
#else
        pluto_pd()->file->close(s->fp);
#endif
        s->fp = NULL;
    }
#ifdef PLUTO_SPILL_HOST
    remove(s->name);
#else
    pluto_pd()->file->unlink(s->name, 0);
#endif
    spillTotal -= s->size;
    s->used = 0;
    s->open = 0;
    s->size = 0;
    s->name[0] = '\0';
    slotIsStore[h] = 0;
    slotKeys[h] = 0;
    return 0;
}

void pluto_spill_reset(void)
{
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        SpillSlot *s = &slots[i];
        if (!s->used || slotIsStore[i])
        {
            continue; /* SW4: persistent store files survive reset */
        }
        if (s->fp)
        {
#ifdef PLUTO_SPILL_HOST
            fclose(s->fp);
#else
            pluto_pd()->file->close(s->fp);
#endif
            s->fp = NULL;
        }
#ifdef PLUTO_SPILL_HOST
        remove(s->name);
#else
        pluto_pd()->file->unlink(s->name, 0);
#endif
        spillTotal -= s->size;
        s->used = 0;
        s->open = 0;
        s->size = 0;
    }
    if (spillTotal < 0)
    {
        spillTotal = 0;
    }
    spillNextId = 1;
}

long pluto_spill_total(void) { return spillTotal; }

/* ── SW4: named bytecode store ──────────────────────────────────────────────
 * Same bounded-slot machinery, second file family: "spill/bc_<id>_<key>.bin".
 * The key rides in the NAME, so entries are found after relaunch without a
 * table file. Store files are created EXCLUDED from reset() (slotIsStore)
 * and only leave via explicit delete/invalidate. Store slots come from the
 * SAME SPILL_MAX_FILES pool — a full store leaves fewer session slots, so
 * store_count is kept small (SW4 caps it at the boot sweep). */

/* Slot of an entry with exact key, or -1. */
static int store_slot_of(unsigned long key)
{
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        if (slots[i].used && slotIsStore[i] && slotKeys[i] == key)
        {
            return i;
        }
    }
    return -1;
}

/* ── relaunch scan: re-register "bc_<id>_<key>.bin" files from disk ────────
 * Keys live in the FILENAME, so a boot-time directory scan restores the
 * store table after process restart (next power-on / sim relaunch). Claimed
 * slots are closed (open=0), sized via stat, and excluded from reset(). */
static void store_scan_claim(const char *fname, unsigned long key,
                             unsigned int fsz)
{
    if (store_slot_of(key) >= 0)
    {
        return; /* duplicate key (id bump) — keep first */
    }
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        SpillSlot *s = &slots[i];
        if (s->used)
        {
            continue;
        }
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name),
#ifdef PLUTO_SPILL_HOST
                 SPILL_DIR "/%s",
#else
                 "spill/%s",
#endif
                 fname);
        s->used = 1;
        s->open = 0;
        s->size = (long)fsz;
        slotIsStore[i] = 1;
        slotKeys[i] = key;
        return;
    }
    /* store table full — leave the file on disk unregistered */
}

#ifdef PLUTO_SPILL_HOST
#include <dirent.h>
#include <sys/stat.h>
static void store_scan(void)
{
    DIR *d = opendir(SPILL_DIR);
    if (!d)
    {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        unsigned long id = 0, key = 0;
        if (sscanf(e->d_name, "bc_%lu_%lu.bin", &id, &key) != 2 || key == 0)
        {
            continue;
        }
        if (id >= spillStoreNextId)
        {
            spillStoreNextId = (unsigned int)id + 1;
        }
        char p[512];
        snprintf(p, sizeof(p), SPILL_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
        {
            store_scan_claim(e->d_name, key, (unsigned int)st.st_size);
        }
    }
    closedir(d);
}
#else
static void store_scan_listfile_cb(const char *path, void *userdata)
{
    (void)userdata;
    unsigned long id = 0, key = 0;
    if (sscanf(path, "bc_%lu_%lu.bin", &id, &key) != 2 || key == 0)
    {
        return;
    }
    if (id >= spillStoreNextId)
    {
        spillStoreNextId = (unsigned int)id + 1;
    }
    char p[128];
    snprintf(p, sizeof(p), "spill/%s", path);
    FileStat st;
    memset(&st, 0, sizeof(st));
    if (pluto_pd()->file->stat(p, &st) == 0 && !st.isdir)
    {
        store_scan_claim(path, key, st.size);
    }
}
static void store_scan(void)
{
    pluto_pd()->file->listfiles("spill", store_scan_listfile_cb, NULL, 0);
}
#endif

static int storeScanDone = 0;

SpillFile pluto_spill_store_open_create(unsigned long key)
{
    if (pluto_spill_init() != 0)
    {
        return PLUTO_SPILL_INVALID;
    }
    if (!storeScanDone)
    {
        storeScanDone = 1;
        store_scan();
    }
    int h = store_slot_of(key);
    if (h >= 0)
    {
        /* Recreate-in-place: same key replaces its file (write path re-opens
         * after a decode failure — old bytes must not be read again). */
        pluto_spill_discard(h);
    }
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        SpillSlot *s = &slots[i];
        if (s->used)
        {
            continue;
        }
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name),
#ifdef PLUTO_SPILL_HOST
                 SPILL_DIR "/bc_%d_%lu.bin",
#else
                 "spill/bc_%d_%lu.bin",
#endif
                 spillStoreNextId++, key);
        s->used = 1;
#ifdef PLUTO_SPILL_HOST
        s->fp = fopen(s->name, "wb+");
#else
        s->fp = pluto_pd()->file->open(s->name, kFileWrite);
#endif
        if (!s->fp)
        {
            s->used = 0;
            return PLUTO_SPILL_INVALID;
        }
        s->open = 1;
        s->size = 0;
        slotIsStore[i] = 1;
        slotKeys[i] = key;
        return i;
    }
    return PLUTO_SPILL_INVALID; /* all slots busy */
}

int pluto_spill_store_find(unsigned long key)
{
    if (pluto_spill_init() == 0 && !storeScanDone)
    {
        storeScanDone = 1;
        store_scan();
    }
    return store_slot_of(key) >= 0;
}

SpillFile pluto_spill_store_open_read(unsigned long key)
{
    int h = store_slot_of(key);
    if (h < 0)
    {
        return PLUTO_SPILL_INVALID;
    }
    return h;
}

void pluto_spill_store_close(SpillFile h)
{
    if (h < 0 || h >= SPILL_MAX_FILES || !slots[h].used || !slotIsStore[h])
    {
        return;
    }
    SpillSlot *s = &slots[h];
    if (s->fp && !s->open)
    {
        /* read-side handle left open by pluto_spill_read's reopen path */
#ifdef PLUTO_SPILL_HOST
        fclose(s->fp);
#else
        pluto_pd()->file->close(s->fp);
#endif
    }
    s->fp = NULL;
}

int pluto_spill_store_index_of(unsigned long key)
{
    return store_slot_of(key);
}

int pluto_spill_store_count(void)
{
    int n = 0;
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        if (slots[i].used && slotIsStore[i])
        {
            n++;
        }
    }
    return n;
}

unsigned long pluto_spill_store_key_at(int idx)
{
    if (idx < 0 || idx >= SPILL_MAX_FILES || !slots[idx].used ||
        !slotIsStore[idx])
    {
        return 0;
    }
    return slotKeys[idx];
}

int pluto_spill_store_delete_at(int idx)
{
    if (idx < 0 || idx >= SPILL_MAX_FILES || !slots[idx].used ||
        !slotIsStore[idx])
    {
        return -1;
    }
    return pluto_spill_discard(idx);
}

void pluto_spill_store_invalidate_all(void)
{
    /* The store table is populated lazily (first store_* call scans the
     * spill dir). Invalidate must see everything on disk, otherwise entries
     * not yet registered survive the sweep and are served by later loads. */
    if (pluto_spill_init() != 0)
    {
        return;
    }
    if (!storeScanDone)
    {
        storeScanDone = 1;
        store_scan();
    }
    for (int i = 0; i < SPILL_MAX_FILES; i++)
    {
        if (slots[i].used && slotIsStore[i])
        {
            pluto_spill_discard(i);
        }
    }
}
