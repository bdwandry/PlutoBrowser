#ifndef PLUTO_UI_CHROME_H
#define PLUTO_UI_CHROME_H

#include <stddef.h>

#include "../core/url.h"

struct PlaydateAPI;

/* C port of Source/ui/chrome.lua (Chrome).
 *
 * Faithful parity notes:
 *  - cometAnimFrame is a module static incremented once per draw call;
 *    loading dots use frame % 12 (three dot layouts at <4, <8, else).
 *  - pageTitle is accepted but UNUSED in the Lua body; kept for parity.
 *  - Host display rules: default "CometBrowser"; scheme=="about" ->
 *    "about:" .. (host or "home"); else non-empty host wins. Longer than
 *    28 chars -> first 25 + "...".
 *  - Badge "[READ]"/"[WEB]" only when a non-about URL is present and not
 *    loading; right edge sits at SCREEN_WIDTH - badgeW - 62.
 *  - Progress: known total -> min(1, cur/tot); otherwise indeterminate
 *    sweep ((frame*3) % 100)/100. Bar drawn over the separator row.
 *  - Time falls back to "--:--" when the clock read fails (Lua pcall).
 */

void chrome_init(struct PlaydateAPI* pd);

/* Pure helpers mirroring Chrome.draw internals (host-testable). */
void   ch_display_host(const PlutoUrl* urlObj, char* out, size_t cap);
double ch_progress_ratio(long progressCur, long progressTot,
                         int animFrame);
int    ch_comet_group(int animFrame);   /* 0 (<4), 1 (<8), 2 (else) */

/* device/sim only: paints the top bar; host builds no-op */
void ch_draw(const PlutoUrl* urlObj, const char* pageTitle,
             int isLoading, long progressCur, long progressTot,
             int isReaderMode, const char* backendLabel);

#endif
