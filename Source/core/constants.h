/*
 * PlutoBrowser — constants.h
 * Screen geometry, view states, search engines, bookmarks, image modes.
 * Port of Source/core/constants.lua (reference values preserved exactly).
 */
#ifndef PLUTO_CONSTANTS_H
#define PLUTO_CONSTANTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Display & Screen Geometry ─────────────────────────────────────────── */
#define SCREEN_WIDTH     400
#define SCREEN_HEIGHT    240
#define CHROME_HEIGHT    24
#define CONTENT_Y        24
#define CONTENT_HEIGHT   216
#define CONTENT_WIDTH    400
#define CONTENT_MARGIN   8
#define CONTENT_TEXT_WIDTH 384
#define SCROLLBAR_WIDTH  5

/* ── View States ───────────────────────────────────────────────────────── */
typedef enum
{
    STATE_HOME = 0,
    STATE_LOADING,
    STATE_PAGE,
    STATE_ERROR,
    STATE_BOOKMARKS,
    STATE_HISTORY,
    STATE_SETTINGS
} BrowserState;

/* ── Browsing Modes (100% Pure On-Device) ──────────────────────────────── */
typedef enum
{
    MODE_READER = 0,   /* Constants.MODE_READER   = "reader" */
    MODE_RAW_HTML,     /* Constants.MODE_RAW_HTML = "html"   */
    MODE_OPERA_DS      /* Constants.MODE_OPERA_DS = "ds"     */
} BrowseMode;

/* ── Image Rendering Modes ─────────────────────────────────────────────── */
typedef enum
{
    IMAGE_MODE_ALL = 0,      /* Constants.IMAGE_MODE_ALL      = "all"      */
    IMAGE_MODE_VIEWPORT,     /* Constants.IMAGE_MODE_VIEWPORT = "viewport" */
    IMAGE_MODE_ONDEMAND,     /* Constants.IMAGE_MODE_ONDEMAND = "ondemand" */
    IMAGE_MODE_HOVER,        /* Constants.IMAGE_MODE_HOVER    = "hover"    */
    IMAGE_MODE_DISABLED,     /* Constants.IMAGE_MODE_DISABLED = "disabled" */
    IMAGE_MODE_COUNT
} ImageMode;

/* ── Search Engine entry ───────────────────────────────────────────────── */
typedef struct
{
    const char *name;
    const char *url;
} SearchEngine;

#define SEARCH_ENGINE_COUNT 4
extern const SearchEngine SEARCH_ENGINES[SEARCH_ENGINE_COUNT];

/* ── Default Start Page Speed Dials ────────────────────────────────────── */
typedef struct
{
    const char *title;
    const char *url;
    const char *desc;
} DefaultBookmark;

#define DEFAULT_BOOKMARK_COUNT 9
extern const DefaultBookmark DEFAULT_BOOKMARKS[DEFAULT_BOOKMARK_COUNT];

/* ── User-Agent (constants.lua value; HTTP client sends a shorter header
 *    string built in http_client.c exactly like the Lua reference) ──────── */
#define USER_AGENT "Mozilla/5.0 (Playdate OS 2.7; 400x240; 1-bit Mono) CometBrowser/1.0"

/* ── Image mode label/names (settings UI + persistence) ────────────────── */
extern const char *IMAGE_MODE_NAMES[IMAGE_MODE_COUNT];   /* keys stored in storage */
const char *image_mode_label(ImageMode mode);            /* Constants.IMAGE_MODE_LABELS */

/* Map a persisted name ("all"/"viewport"/...) to its enum; -1 if unknown. */
int image_mode_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PLUTO_CONSTANTS_H */
