// storage_data.h — persisted data records shared by Storage and CookieJar.
//
// Field-for-field counterparts of the Lua tables in Storage.bookmarks /
// Storage.history / Storage.cookies / Storage.settings. Fixed buffers with
// defensive truncation replace Lua's unbounded strings.

#ifndef PLUTO_STORAGE_DATA_H
#define PLUTO_STORAGE_DATA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_BM_TITLE_MAX   128
#define PLUTO_BM_URL_MAX     512
#define PLUTO_BM_DESC_MAX    256
#define PLUTO_HIST_TIME_MAX  16
#define PLUTO_HISTORY_CAP    50

#define PLUTO_CK_NAME_MAX    64
#define PLUTO_CK_VALUE_MAX   400
#define PLUTO_CK_DOMAIN_MAX  256
#define PLUTO_CK_PATH_MAX    256
#define PLUTO_CK_SAMESITE_MAX 8 // "lax"/"strict"/"none" + NUL

// Mutable persisted-bookmark record (fixed buffers). The immutable default
// dial list is constants.h's PlutoBookmark/PLUTO_DEFAULT_BOOKMARKS.
typedef struct {
    char title[PLUTO_BM_TITLE_MAX];
    char url[PLUTO_BM_URL_MAX];
    char desc[PLUTO_BM_DESC_MAX];
} PlutoSavedBookmark;

typedef struct {
    char title[PLUTO_BM_TITLE_MAX];
    char url[PLUTO_BM_URL_MAX];
    char time[PLUTO_HIST_TIME_MAX]; // "HH:MM" or "Recent"
} PlutoHistoryItem;

// One stored cookie (CookieJar record). `samesite[0]=='\0'` means unset
// (Lua nil); hasExpires==0 means no expiry (session cookie).
typedef struct {
    char name[PLUTO_CK_NAME_MAX];
    char value[PLUTO_CK_VALUE_MAX];
    char domain[PLUTO_CK_DOMAIN_MAX];
    int  hostOnly;
    char path[PLUTO_CK_PATH_MAX];
    int  secure;
    int  httpOnly;
    char samesite[PLUTO_CK_SAMESITE_MAX];
    double expires;
    int  hasExpires;
    int  del; // transient Max-Age<=0 / past-Expires flag; never persisted
} PlutoCookie;

typedef struct {
    int  searchEngine; // default 1
    int  mode;         // PlutoBrowsingMode, default RAW_HTML ("html")
    int  autoReader;   // default 0
    char fontSize[16]; // default "medium"
    int  imageMode;    // PlutoImageMode, default VIEWPORT ("viewport")
    int  invertCrank;  // default 0
} PlutoSettings;

#ifdef __cplusplus
}
#endif

#endif // PLUTO_STORAGE_DATA_H
