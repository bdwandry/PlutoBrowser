// list_core.c — P30 pure helpers shared by the bookmarks/history pages.

#include "ui/list_core.h"

#include <stdio.h>
#include <string.h>

#include "../core/constants.h"

void lr_nav(int* selectedIndex, LrButton dir, int count) {
    if (selectedIndex == NULL || count <= 0) return;
    if (dir == LR_BTN_DOWN) {
        *selectedIndex += 1;
        if (*selectedIndex > count) *selectedIndex = count;
    } else if (dir == LR_BTN_UP) {
        *selectedIndex -= 1;
        if (*selectedIndex < 1) *selectedIndex = 1;
    }
}

void lr_scroll(double* scrollY, double crankChange) {
    if (scrollY == NULL) return;
    if (crankChange != 0.0) {
        double v = *scrollY + crankChange * 2.0;
        if (v < 0.0) v = 0.0;
        *scrollY = v;
    }
}

int lr_row_visible(float drawY, float itemH) {
    /* Lua: drawY + itemH >= CONTENT_Y and drawY <= SCREEN_HEIGHT */
    return (drawY + itemH >= (float)PLUTO_CONTENT_Y) &&
           (drawY <= (float)PLUTO_SCREEN_HEIGHT);
}

void lr_clip_title(char* out, size_t cap, const char* s) {
    snprintf(out, cap, "%s", s ? s : "");
    size_t len = strlen(out);
    if (len > 34) {
        memmove(out + 31, "...", 4); // first 31 bytes already in place
    }
}

void lr_clip_url(char* out, size_t cap, const char* s) {
    snprintf(out, cap, "%s", s ? s : "");
    size_t len = strlen(out);
    if (len > 46) {
        memmove(out + 43, "...", 4);
    }
}
