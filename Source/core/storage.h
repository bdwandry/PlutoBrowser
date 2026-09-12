/*
 * PlutoBrowser — storage.h
 * Persistent storage (port of Source/core/storage.lua).
 *
 * The Lua reference used playdate.datastore (Lua-only; absent from the C
 * API), so PlutoBrowser serializes to a text file itself, preserving the
 * reference's observable behavior: sections for bookmarks/history/cookies/
 * settings, load-with-defaults, 50-entry history cap with dedupe-to-top,
 * bookmark add/remove/isBookmarked semantics, save-after-every-mutation.
 *
 * Data file: "comet_browser_data" (same name as the Lua reference) inside
 * the game's data directory (C file API paths are relative to it).
 *
 * Serializer format (our own, replacing JSON):
 *   [bookmarks] / [history] / [cookies] / [settings] section headers;
 *   bookmark lines  B|title|url|desc
 *   history lines   H|time|title|url
 *   cookie lines    C|name|value|domain|hostOnly|path|secure|httpOnly|samesite|expires
 *   setting lines   S|key=value
 * Field values have tabs/newlines/escapes hex-encoded (\xHH) and literal
 * backslashes doubled, pipes encoded as \p — see storage.c.
 */
#ifndef PLUTO_STORAGE_H
#define PLUTO_STORAGE_H

#include "pd_api.h"

typedef struct StoredBookmark
{
    char *title;
    char *url;
    char *desc;
} StoredBookmark;

typedef struct StoredHistoryItem
{
    char *title;
    char *url;
    char *time;
} StoredHistoryItem;

#define HISTORY_MAX 50

void storage_init(PlaydateAPI *pd);

/* Load/save. init() already loads (defaults on first run, saving them). */
void storage_load(void);
void storage_save(void);

/* Bookmarks (mirrors Lua addBookmark/removeBookmark/isBookmarked). */
int storage_add_bookmark(const char *title, const char *url, const char *desc);
int storage_remove_bookmark(int index); /* 0-based */
int storage_is_bookmarked(const char *url);

/* History (mirrors Lua addHistory: skip empty/about: URLs, dedupe-to-top,
 * HH:MM timestamp, 50 cap). */
void storage_add_history(const char *title, const char *url);

/* Direct list access for UI + tests (index < count). */
const StoredBookmark *storage_bookmark_at(int index);
int storage_bookmark_count(void);
const StoredHistoryItem *storage_history_at(int index);
int storage_history_count(void);

/* Settings (same keys/defaults as the Lua reference). */
int storage_setting_int(const char *key);
void storage_set_setting_int(const char *key, int value);
const char *storage_setting_str(const char *key); /* NULL for number keys */
void storage_set_setting_str(const char *key, const char *value);

/* Cookie section access for core/cookie_jar (Phase 8 hook lives in the jar;
 * this API is used by the storage integration when wiring is complete). */
void storage_clear_cookies_for_test(void);

/* Test support: write garbage over the data file, reload, and return the
 * resulting bookmark count (defaults → 9). Restores the file afterwards. */
int storage_test_corrupt_recovery(void);

#endif /* PLUTO_STORAGE_H */
