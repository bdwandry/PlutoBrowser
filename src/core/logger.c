// logger.c — full C port of CometBrowser Source/core/logger.lua
//
// Lua original behavior (source of truth):
//   LOG_PATH = "comet.log"  -> PlutoBrowser writes "pluto.log"
//   Logger.init():  kFileWrite (truncate), banner + "gamePath: <path>"
//   Logger.log(m):  "[HH:MM:SS #seq] m\n" via kFileAppend, open+close per line
//   Logger.error(m): "ERROR: m  ||  stack: where()"
// Every write is failure-tolerant (Lua wrapped each step in pcall; the C port
// checks every return value and never crashes on I/O errors).

#include "core/logger.h"

#include <stdarg.h>
#include <stdio.h>

#include "pd_api.h"

#define PLUTO_LOG_PATH "pluto.log"
#define PLUTO_LOG_LINE_MAX 1024

static PlaydateAPI* s_pd = NULL;
static int s_seq = 0;

void logger_init(PlaydateAPI* playdate)
{
    s_pd = playdate;
    s_seq = 0;

    if (s_pd == NULL) {
        return;
    }

    // Lua: pcall(function() path = tostring(playdate.getPath()) end)
    // The C API has no getPath(); the Lua fallback string was "unknown".
    const char* gamePath = "unknown";

    SDFile* f = s_pd->file->open(PLUTO_LOG_PATH, kFileWrite);
    if (f != NULL) {
        s_pd->file->write(f, "=== PlutoBrowser crash log ===\n",
                          sizeof("=== PlutoBrowser crash log ===\n") - 1);
        char line[PLUTO_LOG_LINE_MAX];
        int n = snprintf(line, sizeof(line), "gamePath: %s\n", gamePath);
        if (n > 0) {
            if ((size_t)n >= sizeof(line)) {
                n = (int)sizeof(line) - 1;
            }
            s_pd->file->write(f, line, (unsigned int)n);
        }
        s_pd->file->close(f);
    }
}

static void nowString(char* out, size_t outSize)
{
    // Lua: string.format("%02d:%02d:%02d", t.hour, t.minute, t.second)
    unsigned int ms = 0;
    unsigned int epoch = s_pd ? s_pd->system->getSecondsSinceEpoch(&ms) : 0;
    struct PDDateTime dt;
    dt.year = 0; dt.month = 0; dt.day = 0; dt.weekday = 0;
    dt.hour = 0; dt.minute = 0; dt.second = 0;
    if (s_pd != NULL) {
        s_pd->system->convertEpochToDateTime(epoch, &dt);
    }
    snprintf(out, outSize, "%02d:%02d:%02d", (int)dt.hour, (int)dt.minute, (int)dt.second);
}

void logger_log(const char* fmt, ...)
{
    s_seq++;

    char timestamp[16];
    nowString(timestamp, sizeof(timestamp));

    char msg[PLUTO_LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) {
        msg[0] = '\0';
        n = 0;
    } else if ((size_t)n >= sizeof(msg)) {
        n = (int)sizeof(msg) - 1;
    }

    char line[PLUTO_LOG_LINE_MAX + 32];
    int total = snprintf(line, sizeof(line), "[%s #%d] %.*s\n",
                         timestamp, s_seq, n, msg);
    if (total < 0) {
        return;
    }
    if ((size_t)total >= sizeof(line)) {
        total = (int)sizeof(line) - 1;
    }

    if (s_pd == NULL) {
        // Host build (no Playdate API): mirror log lines to stderr so
        // out-of-sim test harnesses can see them.
        fputs(line, stderr);
        return;
    }

    // Lua wrapped this in pcall — any file error must be swallowed.
    SDFile* f = s_pd->file->open(PLUTO_LOG_PATH, kFileAppend);
    if (f != NULL) {
        s_pd->file->write(f, line, (unsigned int)total);
        s_pd->file->close(f);
    }
}

void logger_error_loc(const char* file, int line, const char* fmt, ...)
{
    char msg[PLUTO_LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) {
        msg[0] = '\0';
        n = 0;
    } else if ((size_t)n >= sizeof(msg)) {
        n = (int)sizeof(msg) - 1;
    }

    // Lua: "ERROR: " .. msg .. "  ||  stack: " .. where()
    logger_log("ERROR: %.*s  ||  stack: %s:%d", n, msg, file, line);
}
