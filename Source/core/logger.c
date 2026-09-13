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

void logger_init(PlaydateAPI *playdate)
{
    g_pd = playdate;
    g_seq = 0;

    SDFile *f = g_pd->file->open(LOG_PATH, kFileWrite);
    if (f)
    {
        g_pd->file->write(f, "=== PlutoBrowser crash log ===\n", 31);
        g_pd->file->write(f, "gamePath: com.bryanwandrych.plutobrowser\n", 41);
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
