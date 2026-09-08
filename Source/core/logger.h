/*
 * PlutoBrowser — logger.h
 * File-based crash/event logger (port of Source/core/logger.lua).
 *
 * Phase 2 implements the full module; Phase 0 provides the minimal init/log
 * needed by main.c. The Lua reference wrote to "comet.log"; per the project
 * deployment instructions (AGENTS.md) PlutoBrowser writes "pluto.log" —
 *   Simulator: <SDK>/Disk/Data/com.bryanwandrych.plutobrowser/pluto.log
 *   Device:    /Data/com.bryanwandrych.plutobrowser/pluto.log
 * Every call opens, writes and closes the file so logs survive a hard crash,
 * exactly like the Lua implementation.
 */
#ifndef PLUTO_LOGGER_H
#define PLUTO_LOGGER_H

#include "pd_api.h"

/* Initialize (truncate) the log file and write the header line. */
void logger_init(PlaydateAPI *playdate);

/* Append a printf-style event line. */
void logger_log(const char *fmt, ...);

/* Append an ERROR line including the reporting source location. */
#define logger_error(...) logger_error_at(__FILE__, __LINE__, __VA_ARGS__)
void logger_error_at(const char *file, int line, const char *fmt, ...);

#endif /* PLUTO_LOGGER_H */
