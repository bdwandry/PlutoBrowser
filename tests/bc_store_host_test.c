/* SW4 host test: the named bytecode store family in pluto_spill.
 * Verifies (1) create→write→finish→(find)→read-back byte-exact, (2) keys
 * survive pluto_spill_reset() (the SESSION family dies, the store does not),
 * (3) recreate-in-place on the same key, (4) enumeration + delete-at +
 * invalidate-all, (5) miss on an unknown key.
 * Build & run (repo root):
 *   cc -o /tmp/bcstore tests/bc_store_host_test.c Source/core/pluto_mem.c \
 *     -DPLUTO_SPILL_HOST -I. -ISource -fsanitize=address,undefined && /tmp/bcstore
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/pluto_spill.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                          \
    do                                             \
    {                                              \
        if (cond)                                  \
        {                                          \
            printf("PASS: %s\n", name);            \
            g_pass++;                              \
        }                                          \
        else                                       \
        {                                          \
            printf("FAIL: %s\n", name);            \
            g_fail++;                              \
        }                                          \
    } while (0)

#define KEY_A 0x12345678UL
#define KEY_B 0x9ABCDEF0UL
#define KEY_C 0xDEADBEEFUL /* left on disk for the phase-2 relaunch scan */

static char payloadA[70000]; /* bytecode-blob-ish size (>64KB) */
static char payloadB[1000];
static char payloadC[321];

int main(int argc, char **argv)
{
    /* Phase 2 ("scan"): a FRESH process sees only what phase 1 left on
     * disk — the exact relaunch condition. The boot scan must re-register
     * the store file and find() must hit it. */
    if (argc > 1 && strcmp(argv[1], "scan") == 0)
    {
        memset(payloadC, 0x5C, sizeof(payloadC)); /* refil: fresh statics here */
        int found = pluto_spill_store_find(KEY_C);
        CHECK(found, "relaunch scan re-registers KEY_C from disk");
        if (found)
        {
            SpillFile r = pluto_spill_store_open_read(KEY_C);
            char back[321];
            CHECK(r != PLUTO_SPILL_INVALID, "scan slot open-read");
            CHECK(pluto_spill_size(r) == (long)sizeof(payloadC),
                  "scan slot size from stat");
            memset(back, 0, sizeof(back));
            CHECK(pluto_spill_read(r, 0, back, sizeof(back)) ==
                      (long)sizeof(back),
                  "scan slot readable");
            CHECK(memcmp(back, payloadC, sizeof(payloadC)) == 0,
                  "scan slot byte-exact");
        }
        printf("bc_store suite: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail != 0;
    }

    system("mkdir -p /tmp/plutobrowser_spill && rm -f /tmp/plutobrowser_spill/*");
    for (size_t i = 0; i < sizeof(payloadA); i++)
    {
        payloadA[i] = (char)(i * 13 + 5);
    }
    memset(payloadB, 0xAB, sizeof(payloadB));

    CHECK(pluto_spill_init() == 0, "init ok");

    /* 1. create + write + finish + find */
    SpillFile w = pluto_spill_store_open_create(KEY_A);
    CHECK(w != PLUTO_SPILL_INVALID, "store create KEY_A");
    CHECK(pluto_spill_write(w, payloadA, sizeof(payloadA)) == 0,
          "store write 70KB");
    CHECK(pluto_spill_finish(w) == (long)sizeof(payloadA), "store finish");
    CHECK(pluto_spill_store_find(KEY_A), "store find KEY_A after finish");
    CHECK(!pluto_spill_store_find(KEY_B), "store miss unknown KEY_B");

    /* 2. read back byte-exact across the 64KB chunk boundary */
    SpillFile r = pluto_spill_store_open_read(KEY_A);
    CHECK(r != PLUTO_SPILL_INVALID, "store open-read KEY_A");
    static char back[sizeof(payloadA)];
    long got = 0;
    while (got < (long)sizeof(payloadA))
    {
        long g = pluto_spill_read(r, got, back + got, 8192);
        if (g <= 0)
        {
            break;
        }
        got += g;
    }
    CHECK(got == (long)sizeof(payloadA), "store read full length");
    CHECK(memcmp(back, payloadA, sizeof(payloadA)) == 0,
          "store read byte-exact (chunk-boundary crossing)");

    /* 3. session files die with reset; store entries survive */
    SpillFile sess = pluto_spill_begin();
    CHECK(sess != PLUTO_SPILL_INVALID, "session begin");
    CHECK(pluto_spill_write(sess, "tmp", 3) == 0, "session write");
    pluto_spill_finish(sess);
    pluto_spill_reset();
    CHECK(pluto_spill_store_find(KEY_A),
          "store survives spill_reset (session file gone)");
    r = pluto_spill_store_open_read(KEY_A);
    CHECK(r != PLUTO_SPILL_INVALID, "store still readable after reset");
    char probe[16];
    CHECK(pluto_spill_read(r, 0, probe, sizeof(probe)) == sizeof(probe),
          "store read after reset ok");
    CHECK(memcmp(probe, payloadA, sizeof(probe)) == 0, "bytes intact");

    /* 4. recreate-in-place with different content */
    w = pluto_spill_store_open_create(KEY_A);
    CHECK(w != PLUTO_SPILL_INVALID, "recreate KEY_A in place");
    CHECK(pluto_spill_write(w, payloadB, sizeof(payloadB)) == 0, "rewrite");
    CHECK(pluto_spill_finish(w) == (long)sizeof(payloadB), "rewrite finish");
    r = pluto_spill_store_open_read(KEY_A);
    CHECK(pluto_spill_size(r) == (long)sizeof(payloadB), "replaced size");
    memset(back, 0, sizeof(back));
    CHECK(pluto_spill_read(r, 0, back, sizeof(payloadB)) ==
              (long)sizeof(payloadB), "replaced read");
    CHECK(memcmp(back, payloadB, sizeof(payloadB)) == 0, "replaced content");

    /* 5. enumeration + delete-at + invalidate-all */
    w = pluto_spill_store_open_create(KEY_B);
    CHECK(w != PLUTO_SPILL_INVALID, "store create KEY_B");
    CHECK(pluto_spill_write(w, payloadB, sizeof(payloadB)) == 0, "B write");
    pluto_spill_finish(w);
    CHECK(pluto_spill_store_count() == 2, "count == 2 (A + B)");
    int sawA = 0, sawB = 0;
    for (int i = 0; i < 12; i++)
    {
        unsigned long k = pluto_spill_store_key_at(i);
        if (k == KEY_A)
        {
            sawA++;
        }
        if (k == KEY_B)
        {
            sawB++;
        }
    }
    CHECK(sawA == 1 && sawB == 1, "enumerate returns both keys exactly once");
    CHECK(pluto_spill_store_delete_at(
              pluto_spill_store_open_read(KEY_A)) == 0,
          "delete KEY_A");
    CHECK(!pluto_spill_store_find(KEY_A), "KEY_A gone");
    CHECK(pluto_spill_store_count() == 1, "count == 1 after delete");
    pluto_spill_store_invalidate_all();
    CHECK(pluto_spill_store_count() == 0, "invalidate-all clears the store");
    CHECK(!pluto_spill_store_find(KEY_B), "KEY_B gone after invalidate");

    /* 6. leave ONE file behind for the phase-2 relaunch scan (same proc
     * never sees the scan — statics are already warm; KEY_C only proves
     * its disk worth in the second process). */
    memset(payloadC, 0x5C, sizeof(payloadC));
    w = pluto_spill_store_open_create(KEY_C);
    CHECK(w != PLUTO_SPILL_INVALID, "store create KEY_C (for scan)");
    CHECK(pluto_spill_write(w, payloadC, sizeof(payloadC)) == 0, "C write");
    CHECK(pluto_spill_finish(w) == (long)sizeof(payloadC), "C finish");

    printf("bc_store suite: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
