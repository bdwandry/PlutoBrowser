// style.c — C port of Source/render/style.lua (Style).
//
// Typography & styling system. See style.h for the parity notes.

#include "render/style.h"

#include <stdio.h>
#include <string.h>

#include "core/logger.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_STYLE_PD 1
#include "pd_api.h"
#endif

typedef struct {
    PlutoFont* heading1;
    PlutoFont* heading2;
    PlutoFont* heading3;
    PlutoFont* body;
    PlutoFont* bodyBold;
    PlutoFont* mono;
    PlutoFont* small;   /* never loaded -- Lua leaves it nil */
    PlutoFont* sys;
} StyleState;

static StyleState st;

#ifdef PLUTO_STYLE_PD
static PlaydateAPI* s_pd = NULL;

static PlutoFont* load_or_null(const char* path) {
    const char* err = NULL;
    LCDFont* f = s_pd->graphics->loadFont(path, &err);
    PLUTO_LOG("[P13] font %s -> %s", path,
              (f != NULL) ? "loaded" : "MISSING (system fallback)");
    return (PlutoFont*)f;
}
#endif

void style_init(struct PlaydateAPI* pd) {
    memset(&st, 0, sizeof(st));
#ifndef PLUTO_STYLE_PD
    (void)pd;
#endif
#ifdef PLUTO_STYLE_PD
    if (pd == NULL || pd->graphics == NULL) return;
    s_pd = pd;
    /* pcall(...) per slot in the source: a failed load stays nil */
    st.heading1  = load_or_null("fonts/Roobert-20-Medium");
    st.heading2  = load_or_null("fonts/Roobert-10-Bold");
    st.heading3  = load_or_null("fonts/Roobert-10-Bold");
    st.body      = load_or_null("fonts/Roobert-11-Medium");
    st.bodyBold  = load_or_null("fonts/Roobert-10-Bold");
    st.mono      = load_or_null("fonts/Roobert-11-Mono-Condensed");

    /* Lua falls back to gfx.getFont(); main.c sets the body font as the
     * current font right after init, so it is registered via
     * style_set_system_font() from there. */
#endif
}

void style_set_system_font(PlutoFont* f) { st.sys = f; }

int style_get_text_width(PlutoFont* font, const char* text) {
    if (text == NULL || text[0] == '\0') return 0;
    PlutoFont* f = font;
    if (f == NULL) f = st.body;
    if (f == NULL) f = st.sys;
#ifdef PLUTO_STYLE_PD
    if (s_pd != NULL && f != NULL)
        return s_pd->graphics->getTextWidth((LCDFont*)f, text,
                                            strlen(text), kASCIIEncoding, 0);
#endif
    return (int)strlen(text) * 8;
}

PlutoFont* style_get_heading_font(int level, int* lineHeight, int* marginBottom) {
    PlutoFont* f;
    if (level == 1) {
        f = st.heading1;
        if (lineHeight) *lineHeight = 24;
        if (marginBottom) *marginBottom = 6;
    } else if (level == 2) {
        f = st.heading2;
        if (lineHeight) *lineHeight = 18;
        if (marginBottom) *marginBottom = 5;
    } else {
        f = st.heading3;
        if (lineHeight) *lineHeight = 16;
        if (marginBottom) *marginBottom = 4;
    }
    if (f == NULL) f = st.sys;
    return f;
}

PlutoFont* style_get_body_font(int isBold, int isCode, int* size) {
    PlutoFont* f;
    if (isCode) {
        f = st.mono;
        if (size) *size = 15;
    } else if (isBold) {
        f = st.bodyBold;
        if (size) *size = 16;
    } else {
        f = st.body;
        if (size) *size = 16;
    }
    if (f == NULL) f = st.sys;
    return f;
}

PlutoFont* style_get_inline_font(int isBold, int isCode, int isSmall,
                                 int isSub, int isSup, int isBig,
                                 int* size) {
    PlutoFont* f;
    if (isCode) {
        f = st.mono;
        if (size) *size = 15;
    } else if (isSmall || isSub || isSup) {
        f = st.small;
        if (size) *size = 14;
    } else if (isBold || isBig) {
        f = st.bodyBold;
        if (size) *size = 16;
    } else {
        f = st.body;
        if (size) *size = 16;
    }
    if (f == NULL) f = st.sys;
    return f;
}

/* Style.fontSmall or gfx.getFont(): init assigns sysFont when the slot
 * never loaded (Lua: fontSmall = fontSmall or sysFont). */
PlutoFont* style_get_small_font(void) {
    if (st.small) return st.small;
    return st.sys;
}

PlutoFont* style_get_ui_small_font(void) {
    if (st.small) return st.small;
    if (st.mono) return st.mono;
    return st.sys;
}

PlutoFont* style_get_mono_font(void) {
    if (st.mono) return st.mono;
    return st.sys;
}
