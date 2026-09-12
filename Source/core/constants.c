/*
 * PlutoBrowser — constants.c
 * Port of Source/core/constants.lua (values preserved exactly).
 */
#include <string.h>

#include "core/constants.h"

/* Constants.SEARCH_ENGINES */
const SearchEngine SEARCH_ENGINES[SEARCH_ENGINE_COUNT] = {
    { "DuckDuckGo Lite",         "https://html.duckduckgo.com/html/?q=" },
    { "FrogFind (Fast Text)",    "http://frogfind.com/?q=" },
    { "Wiby (Classic Web)",      "https://wiby.me/?q=" },
    { "Wikipedia Search",        "https://en.wikipedia.org/wiki/Special:Search?search=" }
};

/* Constants.DEFAULT_BOOKMARKS */
const DefaultBookmark DEFAULT_BOOKMARKS[DEFAULT_BOOKMARK_COUNT] = {
    { "Bitmap Gallery",         "https://wiesmann.codiferes.net/share/bitmaps/", "Pixel-art & bitmap gallery" },
    { "Google",                 "https://google.com",                        "Search engine" },
    { "Playdate Developer",     "https://play.date/dev",                     "Documentation & SDK" },
    { "Hacker News",            "https://news.ycombinator.com",              "Tech news & discussion" },
    { "Wikipedia",              "https://en.wikipedia.org/wiki/Main_Page",   "Free encyclopedia" },
    { "DuckDuckGo Lite",        "https://html.duckduckgo.com/html/",         "Fast private search" },
    { "FrogFind",               "http://frogfind.com",                       "Text web for vintage devices" },
    { "Wiby Search",            "https://wiby.me",                           "Search engine for simple web" },
    { "Motherfucking Website",  "https://motherfuckingwebsite.com",          "Lightweight pure HTML" },
    { "Dan Luu's Blog",         "https://danluu.com",                        "Engineering essays & blogs" }
};

/* Constants.IMAGE_MODE_NAMES (storage keys, same order as Lua array) */
const char *IMAGE_MODE_NAMES[IMAGE_MODE_COUNT] = {
    "all", "viewport", "ondemand", "hover", "disabled"
};

/* Constants.IMAGE_MODE_LABELS (settings display strings) */
const char *image_mode_label(ImageMode mode)
{
    switch (mode)
    {
    case IMAGE_MODE_ALL:      return "Render All";
    case IMAGE_MODE_VIEWPORT: return "In-View Only";
    case IMAGE_MODE_ONDEMAND: return "On-Demand";
    case IMAGE_MODE_HOVER:    return "Hover";
    case IMAGE_MODE_DISABLED: return "Disabled";
    default:                  return "Render All";
    }
}

int image_mode_from_name(const char *name)
{
    if (!name)
    {
        return -1;
    }
    for (int i = 0; i < IMAGE_MODE_COUNT; i++)
    {
        if (strcmp(IMAGE_MODE_NAMES[i], name) == 0)
        {
            return i;
        }
    }
    return -1;
}
