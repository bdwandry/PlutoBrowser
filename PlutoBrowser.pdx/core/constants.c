// constants.c — full C port of CometBrowser Source/core/constants.lua
// All strings transcribed verbatim from the Lua source of truth.

#include "core/constants.h"

// ---------------------------------------------------------------------------
// View States
// ---------------------------------------------------------------------------

const char* pluto_state_name(PlutoState state)
{
    switch (state) {
        case PLUTO_STATE_HOME:      return "home";
        case PLUTO_STATE_LOADING:   return "loading";
        case PLUTO_STATE_PAGE:      return "page";
        case PLUTO_STATE_ERROR:     return "error";
        case PLUTO_STATE_BOOKMARKS: return "bookmarks";
        case PLUTO_STATE_HISTORY:   return "history";
        case PLUTO_STATE_SETTINGS:  return "settings";
    }
    return "home";
}

// ---------------------------------------------------------------------------
// Browsing Modes
// ---------------------------------------------------------------------------

const char* pluto_mode_name(PlutoMode mode)
{
    switch (mode) {
        case PLUTO_MODE_READER:   return "reader";
        case PLUTO_MODE_RAW_HTML: return "html";
        case PLUTO_MODE_OPERA_DS: return "ds";
    }
    return "reader";
}

// ---------------------------------------------------------------------------
// Search Engines
// ---------------------------------------------------------------------------

const PlutoSearchEngine PLUTO_SEARCH_ENGINES[PLUTO_SEARCH_ENGINE_COUNT] = {
    { "DuckDuckGo Lite",       "https://html.duckduckgo.com/html/?q=" },
    { "FrogFind (Fast Text)",  "http://frogfind.com/?q=" },
    { "Wiby (Classic Web)",    "https://wiby.me/?q=" },
    { "Wikipedia Search",      "https://en.wikipedia.org/wiki/Special:Search?search=" },
};

// ---------------------------------------------------------------------------
// Default Start Page Speed Dials
// ---------------------------------------------------------------------------

const PlutoBookmark PLUTO_DEFAULT_BOOKMARKS[PLUTO_DEFAULT_BOOKMARK_COUNT] = {
    { "Google",                "https://google.com",                            "Search engine" },
    { "Playdate Developer",    "https://play.date/dev",                         "Documentation & SDK" },
    { "Hacker News",           "https://news.ycombinator.com",                  "Tech news & discussion" },
    { "Wikipedia",             "https://en.wikipedia.org/wiki/Main_Page",       "Free encyclopedia" },
    { "DuckDuckGo Lite",       "https://html.duckduckgo.com/html/",             "Fast private search" },
    { "FrogFind",              "http://frogfind.com",                           "Text web for vintage devices" },
    { "Wiby Search",           "https://wiby.me",                               "Search engine for simple web" },
    { "Motherfucking Website", "https://motherfuckingwebsite.com",              "Lightweight pure HTML" },
    { "Dan Luu's Blog",        "https://danluu.com",                            "Engineering essays & blogs" },
};

// ---------------------------------------------------------------------------
// Image Rendering Modes
// ---------------------------------------------------------------------------

const char* pluto_image_mode_name(PlutoImageMode mode)
{
    // Constants.IMAGE_MODE_NAMES order — used by Settings cycling.
    switch (mode) {
        case PLUTO_IMAGE_MODE_ALL:      return "all";
        case PLUTO_IMAGE_MODE_VIEWPORT: return "viewport";
        case PLUTO_IMAGE_MODE_ONDEMAND: return "ondemand";
        case PLUTO_IMAGE_MODE_HOVER:    return "hover";
        case PLUTO_IMAGE_MODE_DISABLED: return "disabled";
    }
    return "all";
}

const char* pluto_image_mode_label(PlutoImageMode mode)
{
    // Constants.IMAGE_MODE_LABELS map.
    switch (mode) {
        case PLUTO_IMAGE_MODE_ALL:      return "Render All";
        case PLUTO_IMAGE_MODE_VIEWPORT: return "In-View Only";
        case PLUTO_IMAGE_MODE_ONDEMAND: return "On-Demand";
        case PLUTO_IMAGE_MODE_HOVER:    return "Hover";
        case PLUTO_IMAGE_MODE_DISABLED: return "Disabled";
    }
    return "Render All";
}

// ---------------------------------------------------------------------------
// Network Protocol
// ---------------------------------------------------------------------------

const char* pluto_protocol_name(PlutoProtocol mode)
{
    switch (mode) {
        case PLUTO_PROTOCOL_HTTP: return "http";
        case PLUTO_PROTOCOL_TCP:  return "tcp";
    }
    return "http";
}

const char* pluto_protocol_label(PlutoProtocol mode)
{
    switch (mode) {
        case PLUTO_PROTOCOL_HTTP: return "HTTP";
        case PLUTO_PROTOCOL_TCP:  return "TCP";
    }
    return "HTTP";
}
