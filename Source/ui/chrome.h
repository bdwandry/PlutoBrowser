/*
 * PlutoBrowser — chrome.h
 * Top chrome / navigation toolbar (port of Source/ui/chrome.lua).
 */
#ifndef PLUTO_CHROME_H
#define PLUTO_CHROME_H

#include "core/url.h"

/* Draw the 24px top bar. All parameters mirror Chrome.draw(); urlObj may be
 * NULL (shows "CometBrowser"), progressTot <= 0 animates the bar. */
void chrome_draw(const UrlParsed *urlObj, const char *pageTitle, int isLoading,
                 float progressCur, float progressTot, int isReaderMode);

#endif /* PLUTO_CHROME_H */
