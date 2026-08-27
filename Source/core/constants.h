// constants.h — full C port of CometBrowser Source/core/constants.lua
// Values and strings are transcribed verbatim; behavior parity is mandatory.

#ifndef PLUTO_CONSTANTS_H
#define PLUTO_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Display & Screen Geometry  (Constants.SCREEN_* / CHROME_* / CONTENT_* ...)
// ---------------------------------------------------------------------------

#define PLUTO_SCREEN_WIDTH        400
#define PLUTO_SCREEN_HEIGHT       240
#define PLUTO_CHROME_HEIGHT       24
#define PLUTO_CONTENT_Y           24   // Constants.CONTENT_Y
#define PLUTO_CONTENT_HEIGHT      216  // Constants.CONTENT_HEIGHT
#define PLUTO_CONTENT_WIDTH       400  // Constants.CONTENT_WIDTH
#define PLUTO_CONTENT_MARGIN      8    // Constants.CONTENT_MARGIN
#define PLUTO_CONTENT_TEXT_WIDTH  384  // Constants.CONTENT_TEXT_WIDTH
#define PLUTO_SCROLLBAR_WIDTH     5    // Constants.SCROLLBAR_WIDTH

// ---------------------------------------------------------------------------
// View States  (Constants.STATE_*)
// Lua stored these as strings ("home", "loading", ...); the C port uses an
// enum plus pluto_state_name() which returns the identical strings so that
// persisted data and log output match the Lua implementation.
// ---------------------------------------------------------------------------

typedef enum PlutoState {
    PLUTO_STATE_HOME      = 0,  // "home"
    PLUTO_STATE_LOADING   = 1,  // "loading"
    PLUTO_STATE_PAGE      = 2,  // "page"
    PLUTO_STATE_ERROR     = 3,  // "error"
    PLUTO_STATE_BOOKMARKS = 4,  // "bookmarks"
    PLUTO_STATE_HISTORY   = 5,  // "history"
    PLUTO_STATE_SETTINGS  = 6,  // "settings"
} PlutoState;

const char* pluto_state_name(PlutoState state);

// ---------------------------------------------------------------------------
// Browsing Modes  (Constants.MODE_READER / MODE_RAW_HTML / MODE_OPERA_DS)
// ---------------------------------------------------------------------------

typedef enum PlutoMode {
    PLUTO_MODE_READER   = 0,  // "reader"
    PLUTO_MODE_RAW_HTML = 1,  // "html"
    PLUTO_MODE_OPERA_DS = 2,  // "ds"
} PlutoMode;

const char* pluto_mode_name(PlutoMode mode);

// ---------------------------------------------------------------------------
// Search Engines  (Constants.SEARCH_ENGINES)
// ---------------------------------------------------------------------------

typedef struct PlutoSearchEngine {
    const char* name;
    const char* url;
} PlutoSearchEngine;

#define PLUTO_SEARCH_ENGINE_COUNT 4
extern const PlutoSearchEngine PLUTO_SEARCH_ENGINES[PLUTO_SEARCH_ENGINE_COUNT];

// ---------------------------------------------------------------------------
// Default Start Page Speed Dials  (Constants.DEFAULT_BOOKMARKS)
// ---------------------------------------------------------------------------

typedef struct PlutoBookmark {
    const char* title;
    const char* url;
    const char* desc;
} PlutoBookmark;

#define PLUTO_DEFAULT_BOOKMARK_COUNT 9
extern const PlutoBookmark PLUTO_DEFAULT_BOOKMARKS[PLUTO_DEFAULT_BOOKMARK_COUNT];

// ---------------------------------------------------------------------------
// User-Agent  (Constants.USER_AGENT)
// ---------------------------------------------------------------------------

#define PLUTO_USER_AGENT \
    "Mozilla/5.0 (Playdate OS 2.7; 400x240; 1-bit Mono) CometBrowser/1.0"

// ---------------------------------------------------------------------------
// Image Rendering Modes  (Constants.IMAGE_MODE_*)
// ---------------------------------------------------------------------------

typedef enum PlutoImageMode {
    PLUTO_IMAGE_MODE_ALL      = 0,  // "all"
    PLUTO_IMAGE_MODE_VIEWPORT = 1,  // "viewport"
    PLUTO_IMAGE_MODE_ONDEMAND = 2,  // "ondemand"
    PLUTO_IMAGE_MODE_HOVER    = 3,  // "hover"
    PLUTO_IMAGE_MODE_DISABLED = 4,  // "disabled"
} PlutoImageMode;

const char* pluto_image_mode_name(PlutoImageMode mode);   // IMAGE_MODE_NAMES order
const char* pluto_image_mode_label(PlutoImageMode mode);  // IMAGE_MODE_LABELS map

// ---------------------------------------------------------------------------
// Network Protocol  (Constants.PROTOCOL_*)
// ---------------------------------------------------------------------------

typedef enum PlutoProtocol {
    PLUTO_PROTOCOL_HTTP = 0,  // "http"
    PLUTO_PROTOCOL_TCP  = 1,  // "tcp"
} PlutoProtocol;

#define PLUTO_PROTOCOL_COUNT 2
const char* pluto_protocol_name(PlutoProtocol mode);      // "http"/"tcp"
const char* pluto_protocol_label(PlutoProtocol mode);     // "HTTP"/"TCP"

#ifdef __cplusplus
}
#endif

#endif // PLUTO_CONSTANTS_H
