/* SW2a host test: pluto_spill disk-backed streaming storage.
 * Build & run (repo root):
 *   cc -o /tmp/spilltest tests/spill_host_test.c Source/core/pluto_mem.c \
 *     -DPLUTO_SPILL_HOST -I. -ISource -fsanitize=address,undefined && /tmp/spilltest
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

int main(void)
{
    system("mkdir -p /tmp/plutobrowser_spill && rm -f /tmp/plutobrowser_spill/spill_*.bin");

    CHECK(pluto_spill_init() == 0, "init ok");

    /* 1. big sequential write (1.5MB — far over any RAM cap) */
    SpillFile a = pluto_spill_begin();
    CHECK(a != PLUTO_SPILL_INVALID, "begin returns handle");
    char chunk[65536];
    for (size_t i = 0; i < sizeof(chunk); i++)
    {
        chunk[i] = (char)(i * 31 + 7);
    }
    long total = 0;
    for (int i = 0; i < 24; i++)
    { /* 24 * 64KB = 1.5MB */
        CHECK(pluto_spill_write(a, chunk, sizeof(chunk)) == 0, "write ok");
        total += (long)sizeof(chunk);
    }
    CHECK(pluto_spill_size(a) == total, "size tracks writes");
    CHECK(pluto_spill_finish(a) == total, "finish returns total");
    CHECK(pluto_spill_size(a) == total, "size persists after finish");

    /* 2. reads: head, middle, tail, full-sequence verification */
    char rbuf[4096];
    CHECK(pluto_spill_read(a, 0, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf),
          "read head");
    CHECK(memcmp(rbuf, chunk, sizeof(rbuf)) == 0, "head content matches");
    long mid = total / 2 - 2048;
    CHECK(pluto_spill_read(a, mid, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf),
          "read middle");
    /* expected byte at file offset o = (char)((o % sizeof(chunk))*31+7);
     * a 4096-byte window can span a chunk boundary, so compose it. */
    char exp[4096];
    {
        size_t off = (size_t)(mid % (long)sizeof(chunk));
        for (size_t k = 0; k < sizeof(exp); k++)
            exp[k] = (char)((off + k) % sizeof(chunk) * 31 + 7);
    }
    CHECK(memcmp(rbuf, exp, sizeof(rbuf)) == 0,
          "middle content matches (offset math)");
    long tail = total - (long)sizeof(rbuf);
    CHECK(pluto_spill_read(a, tail, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf),
          "read tail");
    {
        size_t off = (size_t)(tail % (long)sizeof(chunk));
        for (size_t k = 0; k < sizeof(exp); k++)
            exp[k] = (char)((off + k) % sizeof(chunk) * 31 + 7);
    }
    CHECK(memcmp(rbuf, exp, sizeof(rbuf)) == 0, "tail content matches");
    CHECK(pluto_spill_read(a, total + 100, rbuf, 16) == 0, "read past EOF = 0");

    /* 3. multiple live files */
    SpillFile b = pluto_spill_begin();
    CHECK(b != PLUTO_SPILL_INVALID && b != a, "second handle distinct");
    CHECK(pluto_spill_write(b, "hello", 5) == 0, "write to second file");
    CHECK(pluto_spill_finish(b) == 5, "finish second");
    char tiny[8];
    CHECK(pluto_spill_read(b, 0, tiny, 5) == 5 && memcmp(tiny, "hello", 5) == 0,
          "second file independent");
    CHECK(pluto_spill_size(a) == total, "first file unaffected");

    /* 4. read an in-progress write (flush path) */
    SpillFile c = pluto_spill_begin();
    pluto_spill_write(c, "live-data", 9);
    char live[9];
    CHECK(pluto_spill_read(c, 0, live, 9) == 9 && memcmp(live, "live-data", 9) == 0,
          "read during write (flush)");

    /* 5. accounting */
    CHECK(pluto_spill_total() == total + 5 + 9, "spill_total tracks all files");

    /* 6. reset kills everything */
    pluto_spill_reset();
    CHECK(pluto_spill_total() == 0, "reset clears accounting");
    CHECK(pluto_spill_read(a, 0, rbuf, 16) == -1, "dead handle rejected");
    SpillFile d = pluto_spill_begin();
    CHECK(d != PLUTO_SPILL_INVALID, "begin works again after reset");

    pluto_spill_reset();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
