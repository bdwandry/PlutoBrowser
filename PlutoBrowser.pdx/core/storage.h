// storage.h — persistent Storage (bookmarks, history, cookies, settings).
//
// C port of core/storage.lua. Data lives in DynArrays; save()/load()
// serialize to /Data/<bundle>/comet_browser_data.json via util/json,
// matching the shape playdate.datastore produces for the Lua tables.
// The cookies array is owned here and manipulated by cookie_jar.c.

#ifndef PLUTO_STORAGE_H
#define PLUTO_STORAGE_H

#include <stddef.h>

#include "../util/dynarray.h"
#include "storage_data.h"

struct PlaydateAPI;

#ifdef __cplusplus
extern "C" {
#endif

// Loads saved data or installs defaults (+ initial save). Call once at boot
// after logger/mem init. Also stores the API pointer for clock/file access.
void storage_init(struct PlaydateAPI* pd);

// Serialize all four collections to disk. Returns 1 on success.
int  storage_save(void);

// Re-read from disk, replacing in-memory state (used by tests; init calls it).
int  storage_load(void);

// Drop all state and install defaults WITHOUT touching the disk.
void storage_reset_defaults(void);

// Collections (elements: PlutoBookmark / PlutoHistoryItem / PlutoCookie).
DynArray*       storage_bookmarks(void);
DynArray*       storage_history(void);
DynArray*       storage_cookies(void);
PlutoSettings*  storage_settings(void);

// History (Storage.addHistory semantics: about:/empty skipped, dedup
// move-to-front, cap 50, auto-save).
void storage_add_history(const char* title, const char* url);

// Bookmarks. add returns 0 only when url is NULL/empty (Lua returns false);
// duplicate URL updates title only. remove takes a 1-BASED index.
int  storage_add_bookmark(const char* title, const char* url,
                          const char* desc);
int  storage_remove_bookmark(size_t index1based);
int  storage_is_bookmarked(const char* url);

// Current "HH:MM" local time into out[PLUTO_HIST_TIME_MAX]; "Recent" when
// no clock is available (Lua pcall fallback parity).
void storage_format_now(char* out, size_t outSz);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_STORAGE_H
