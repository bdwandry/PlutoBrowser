// logger.h — full C port of CometBrowser Source/core/logger.lua
//
// File-based crash/event logger. Writes to the game's data folder:
//   Simulator: ~/Documents/Playdate DATA/com.bryanwandrych.plutobrowser/pluto.log
//   Device:    /Data/com.bryanwandrych.plutobrowser/pluto.log
// Every line is written and the file closed immediately, so the log survives
// even a hard crash (same policy as the Lua original).
//
// Diagnostic logging is PERMANENT infrastructure: these calls must never be
// removed from the codebase until the user explicitly authorizes removal in
// the final cleanup phase (MASTER_TODO P36).

#ifndef PLUTO_LOGGER_H
#define PLUTO_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

struct PlaydateAPI;

// Mirrors Logger.init(): truncates the log and writes the banner + gamePath.
void logger_init(struct PlaydateAPI* playdate);

// Mirrors Logger.log(msg): "[HH:MM:SS #seq] msg" appended per line.
void logger_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Mirrors Logger.error(msg): prefixes "ERROR: " and appends "|| stack: <loc>"
// where <loc> is the C equivalent of Lua's where() (file:line of caller).
void logger_error_loc(const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define PLUTO_LOG(...)      logger_log(__VA_ARGS__)
#define PLUTO_ERROR(...)    logger_error_loc(__FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // PLUTO_LOGGER_H
