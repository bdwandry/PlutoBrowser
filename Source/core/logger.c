/*
 * PlutoBrowser — logger.c
 * File-based crash/event logger (port of Source/core/logger.lua).
 *
 * Lua reference behavior (core/logger.lua):
 *  - Logger.init(): opens comet.log for write, writes "=== CometBrowser crash log ==="
 *    and the game path, then closes.
 *  - Logger.log(msg): appends "[HH:MM:SS #seq] msg\n" (open/append/close every call).
 *  - Logger.error(msg): logs "ERROR: msg || stack: <where>".
 * C port notes:
 *  - Log file name is pluto.log (deployment rules in AGENTS.md).
 *  - playdate.getPath() has no C API equivalent; the game path is not available.
 *    We log the bundle id from pdxinfo context instead (static string).
 *  - Lua's where() has no C equivalent; error lines carry __FILE__:__LINE__.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "core/logger.h"

#define LOG_PATH "pluto.log"
#define LOG_LINE_MAX 512

static PlaydateAPI *g_pd = NULL;
static int g_seq = 0;

/* ── Stack high-water tracking ─────────────────────────────────────────
 * The device game task has a 61.8KB stack; errorlog showed an overflow
 * (storage_load → entities chain, 2026-09-12). logger_stack_touch() reads
 * the current SP and records the lowest value seen; the heartbeat logs
 * the peak. On simulator the SP is meaningless for this purpose, so the
 * simulator build keeps a fixed 0 peak. */
#if defined(TARGET_SIMULATOR) || !defined(__arm__)
void logger_stack_touch(void) {}
unsigned logger_stack_peak(void) { return 0; }
#else
static uintptr_t g_stackTop = 0;
static uintptr_t g_stackFloor = UINTPTR_MAX;
void logger_stack_touch(void)
{
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (!g_stackTop || sp > g_stackTop)
    {
        g_stackTop = sp; /* shallowest SP seen ≈ stack top */
    }
    if (sp < g_stackFloor)
    {
        g_stackFloor = sp;
    }
}
unsigned logger_stack_peak(void)
{
    return (g_stackTop && g_stackFloor != UINTPTR_MAX && g_stackTop > g_stackFloor)
               ? (unsigned)(g_stackTop - g_stackFloor)
               : 0;
}
#endif

static void nowString(char *out, size_t outLen)
{
    struct PDDateTime t;
    memset(&t, 0, sizeof(t));
    g_pd->system->convertEpochToDateTime(g_pd->system->getSecondsSinceEpoch(NULL), &t);
    snprintf(out, outLen, "%02d:%02d:%02d", (int)t.hour, (int)t.minute, (int)t.second);
}

/* Previous-run tail preservation: logger_init used to truncate the log on
 * every boot, destroying exactly the evidence a watchdog kill needs (the
 * lines written just before death). Keep the last 8KB under a marker.
 * Bounded: the old tail is re-truncated every boot, never accumulates.
 * BSS buffer: the device game-task stack is tiny, no 8KB locals. */
#define LOG_TAIL_KEEP (8 * 1024)
static char g_logTail[LOG_TAIL_KEEP];
static int g_logTailLen = 0;

static void logger_read_tail(void)
{
    g_logTailLen = 0;
    if (!g_pd || !g_pd->file)
    {
        return;
    }
    SDFile *f = g_pd->file->open(LOG_PATH, kFileReadData);
    if (!f)
    {
        return; /* first boot: no previous log */
    }
    /* Sliding window: keep only the last LOG_TAIL_KEEP bytes. */
    char chunk[1024];
    int n;
    while ((n = g_pd->file->read(f, chunk, sizeof(chunk))) > 0)
    {
        if (g_logTailLen + n <= LOG_TAIL_KEEP)
        {
            memcpy(g_logTail + g_logTailLen, chunk, (size_t)n);
            g_logTailLen += n;
        }
        else
        {
            int over = g_logTailLen + n - LOG_TAIL_KEEP;
            memmove(g_logTail, g_logTail + over, (size_t)(g_logTailLen - over));
            g_logTailLen = LOG_TAIL_KEEP - n;
            memcpy(g_logTail + g_logTailLen, chunk, (size_t)n);
            g_logTailLen = LOG_TAIL_KEEP;
        }
    }
    g_pd->file->close(f);
}

void logger_init(PlaydateAPI *playdate)
{
    g_pd = playdate;
    g_seq = 0;

    logger_read_tail();
    SDFile *f = g_pd->file->open(LOG_PATH, kFileWrite);
    if (f)
    {
        g_pd->file->write(f, "=== PlutoBrowser crash log ===\n", 31);
        g_pd->file->write(f, "gamePath: com.bryanwandrych.plutobrowser\n", 41);
        if (g_logTailLen > 0)
        {
            const char *mark = "--- previous run tail (truncated to 8KB) ---\n";
            g_pd->file->write(f, mark, (unsigned int)strlen(mark));
            g_pd->file->write(f, g_logTail, (unsigned int)g_logTailLen);
            const char *end = "--- end previous tail / new boot ---\n";
            g_pd->file->write(f, end, (unsigned int)strlen(end));
        }
        g_pd->file->close(f);
    }
}

void logger_log(const char *fmt, ...)
{
    if (!g_pd)
    {
        return;
    }

    char msg[LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char stamp[16];
    nowString(stamp, sizeof(stamp));

    static char line[LOG_LINE_MAX + 48]; /* hoisted: device gameTask stack is tiny */
    g_seq++;
    int len = snprintf(line, sizeof(line), "[%s #%d] %s\n", stamp, g_seq, msg);

    SDFile *f = g_pd->file->open(LOG_PATH, kFileAppend);
    if (f)
    {
        g_pd->file->write(f, line, (unsigned int)len);
        g_pd->file->close(f);
    }
}

void logger_error_at(const char *file, int line, const char *fmt, ...)
{
    char msg[LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    logger_log("ERROR: %s  ||  at %s:%d", msg, file, line);
}
