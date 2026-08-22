// address_bar.c — P29: C port of CometBrowser Source/ui/address_bar.lua.
//
// Address bar controller: armed pill -> system-style on-screen keyboard
// (vendored Raphcal C port) -> submit decision (search query vs URL).
//
// Lua parity notes:
//   - open() prefills currentUrl unless it matches ^about:
//   - launchKeyboard() refuses while B is held so B+Left/Right work first,
//     and only once per open
//   - keyboardWillHide(submitted): trim whitespace; empty or cancelled just
//     closes (skipInputFrames=2); otherwise search-query vs URL.parse
//     normalized feeds onSubmitCallback
//   - cancel(): hides keyboard, clears callbacks
//   - drawOverlay(): two layouts (armed pill vs keyboard-side box with a
//     vertically-wrapped mono text)

#include "ui/address_bar.h"

#include <string.h>

#include "../core/constants.h"
#include "../core/logger.h"
#include "../core/storage.h"
#include "../core/url.h"
#include "../render/style.h"
#include "../util/mem.h"
#include "../util/strbuf.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define AB_HAS_PD 1
#endif

static PlaydateAPI* s_pd = NULL;
static PDKeyboard* s_kb = NULL;
static int (*s_updateFn)(void*) = NULL;
static void* s_updateUd = NULL;

static int s_isOpen = 0;
static int s_keyboardShown = 0;
static int s_skipInputFrames = 0;
static char s_inputText[PLUTO_URL_INPUT_MAX];

static AbSubmitFn s_onSubmit = NULL;
static void* s_submitUd = NULL;

void ab_trim(char* text) {
    if (text == NULL) return;

    /* leading */
    char* p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\v' || *p == '\f') {
        p++;
    }

    /* trailing */
    char* end = p + strlen(p);
    while (end > p) {
        char c = *(end - 1);
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
              c == '\v' || c == '\f')) {
            break;
        }
        end--;
    }

    size_t len = (size_t)(end - p);
    if (p != text) memmove(text, p, len);
    text[len] = '\0';
}

int ab_build_final_url(const char* text, char* out, size_t cap) {
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    const char* t = (text != NULL) ? text : "";
    char trimmed[PLUTO_URL_INPUT_MAX];
    snprintf(trimmed, sizeof(trimmed), "%s", t);
    ab_trim(trimmed);
    if (trimmed[0] == '\0') {
        return 0;
    }

    if (url_is_search_query(trimmed)) {
        PlutoSettings* st = storage_settings();
        int idx = st ? st->searchEngine : 1;
        if (idx < 1 || idx > PLUTO_SEARCH_ENGINE_COUNT) idx = 1;
        const PlutoSearchEngine* engine = &PLUTO_SEARCH_ENGINES[idx - 1];
        StrBuf sb;
        sb_init(&sb);
        url_build_search_url(engine->url, trimmed, &sb);
        snprintf(out, cap, "%s", sb.data ? sb.data : "");
        sb_free(&sb);
    } else {
        PlutoUrl parsed;
        url_parse(trimmed, &parsed);
        snprintf(out, cap, "%s", parsed.normalized);
    }
    return 1;
}

int ab_should_launch_keyboard(int isOpen, int keyboardShown, int bHeld) {
    if (!isOpen || keyboardShown) return 0;
    if (bHeld) return 0; // B+Left/Right navigation wins first
    return 1;
}

int ab_is_open(void) { return s_isOpen; }

int ab_keyboard_shown(void) { return s_keyboardShown; }

const char* ab_input_text(void) { return s_inputText; }

int ab_pop_input_skip(void) {
    if (s_skipInputFrames > 0) {
        s_skipInputFrames--;
        return 1;
    }
    return 0;
}

int ab_input_skip_remaining(void) { return s_skipInputFrames; }

#ifdef AB_HAS_PD

static void close_after_hide(void) {
    s_isOpen = 0;
    s_keyboardShown = 0;
    s_skipInputFrames = 2;
}

static void kb_text_changed(void* userdata) {
    (void)userdata;
    char* text = NULL;
    unsigned int count = 0;
    keyboardApi.getText(s_kb, &text, &count);
    if (text != NULL) {
        snprintf(s_inputText, sizeof(s_inputText), "%s",
                 (count > 0) ? text : "");
        pluto_free(text);
    }
}

static void kb_will_hide(int submitted, void* userdata) {
    (void)userdata;

    if (submitted && ab_build_final_url(s_inputText, s_inputText,
                                        sizeof(s_inputText))) {
        char finalUrl[PLUTO_URL_NORMALIZED_MAX];
        snprintf(finalUrl, sizeof(finalUrl), "%s", s_inputText);

        close_after_hide();
        PLUTO_LOG("[P29] submit -> %s", finalUrl);
        if (s_onSubmit != NULL) {
            s_onSubmit(finalUrl, s_submitUd);
        }
        return;
    }

    /* cancelled, or empty-after-trim submit: plain cancel path */
    close_after_hide();
}

void ab_init(PlaydateAPI* pd, int (*mainUpdate)(void*), void* updateUd) {
    s_pd = pd;
    s_updateFn = mainUpdate;
    s_updateUd = updateUd;
    s_kb = keyboardApi.newKeyboard();
    if (s_kb == NULL) {
        PLUTO_ERROR("[P29] keyboard instance allocation failed");
        return;
    }
    keyboardApi.setRefreshRate(s_kb, 30.0f);
    keyboardApi.setPlaydateUpdateCallback(s_kb, mainUpdate, updateUd);
    PLUTO_LOG("[P29] address bar ready (vendored C keyboard)");
}

void ab_open(const char* currentUrl, AbSubmitFn onSubmit, void* userdata) {
    s_isOpen = 1;
    s_keyboardShown = 0;
    s_onSubmit = onSubmit;
    s_submitUd = userdata;

    /* Lua: initialText = "" unless currentUrl exists and not ^about: */
    const char* initial = "";
    if (currentUrl != NULL &&
        strncmp(currentUrl, "about:", 6) != 0) {
        initial = currentUrl;
    }
    snprintf(s_inputText, sizeof(s_inputText), "%s", initial);
    /* Keyboard is NOT shown here; launches on B release (launchKeyboard),
     * letting Left/Right navigate back/forward while armed. */
}

void ab_launch_keyboard(void) {
    PDButtons down, pushed, released;
    s_pd->system->getButtonState(&down, &pushed, &released);
    if (!ab_should_launch_keyboard(s_isOpen, s_keyboardShown,
                                   (down & kButtonB) != 0)) {
        return;
    }
    s_keyboardShown = 1;

    /* Lua order: nil didHide + textChanged, then assign willHide and
     * textChanged, then show(prefill). */
    keyboardApi.setKeyboardDidHideCallback(s_kb, NULL, NULL);
    keyboardApi.setTextChangedCallback(s_kb, NULL, NULL);
    keyboardApi.setKeyboardWillHideCallback(s_kb, kb_will_hide, NULL);
    keyboardApi.setTextChangedCallback(s_kb, kb_text_changed, NULL);
    keyboardApi.show(s_kb, s_inputText,
                     (unsigned int)strlen(s_inputText));
}

void ab_cancel(void) {
    if (s_keyboardShown) {
        keyboardApi.hide(s_kb); // fires willHide(0) -> closes like Lua
    }
    s_isOpen = 0;
    s_keyboardShown = 0;
    s_onSubmit = NULL;
    s_submitUd = NULL;
    keyboardApi.setKeyboardWillHideCallback(s_kb, NULL, NULL);
    keyboardApi.setTextChangedCallback(s_kb, NULL, NULL);
    keyboardApi.setKeyboardDidHideCallback(s_kb, NULL, NULL);
}

void ab_draw_overlay(void) {
    if (!s_isOpen) return;

    int boxX, boxY, boxW, boxH;
    if (s_keyboardShown) {
        boxX = 4;
        boxY = 4;
        boxW = 192;
        boxH = PLUTO_SCREEN_HEIGHT - 8;
    } else {
        boxX = 10;
        boxY = 6;
        boxW = PLUTO_SCREEN_WIDTH - 20;
        boxH = 48;
    }

    s_pd->graphics->fillRoundRect(boxX, boxY, boxW, boxH, 6, kColorWhite);
    s_pd->graphics->drawRoundRect(boxX, boxY, boxW, boxH, 6, 1,
                                  kColorBlack);

    /* clip to box interior */
    s_pd->graphics->pushContext(NULL);
    s_pd->graphics->setClipRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4);

    int innerX = boxX + 10;
    int innerY = boxY + 10;
    int innerW = boxW - 20;

    int sz = 16;
    PlutoFont* font = style_get_body_font(1 /*bold*/, 0, &sz);
    if (font == NULL) font = style_get_small_font(); // gfx.getFont() chain
    s_pd->graphics->setFont(font);
    s_pd->graphics->drawText("Enter URL or Search:", strlen(
                                 "Enter URL or Search:"),
                             kASCIIEncoding, (int)innerX, (int)innerY);

    const char* txt = s_inputText;
    PlutoFont* monoFont = style_get_mono_font();
    if (monoFont == NULL) monoFont = font;
    s_pd->graphics->setFont(monoFont);

    size_t txtLen = strlen(txt);
    if (s_keyboardShown) {
        /* wrap text to fill the box vertically */
        int lineY = innerY + 22;
        const int lineHeight = 14;
        size_t lineStart = 0, i = 0;
        while (i < txtLen) {
            char ch[2] = { txt[i], '\0' };
            i++;
            /* test width of line..i */
            char testLine[PLUTO_URL_INPUT_MAX];
            size_t segLen = i - lineStart;
            if (segLen >= sizeof(testLine)) segLen = sizeof(testLine) - 1;
            memcpy(testLine, txt + lineStart, segLen);
            testLine[segLen] = '\0';

            if (style_get_text_width(monoFont, testLine) > innerW &&
                i - lineStart > 1) {
                /* flush previous segment (without the new char) */
                segLen = (i - 1) - lineStart;
                s_pd->graphics->drawText(txt + lineStart, segLen,
                                         kUTF8Encoding, innerX, lineY);
                lineY += lineHeight;
                lineStart = i - 1;
                if (lineY > boxY + boxH - 14) break;
            }
        }
        if (lineStart < txtLen && lineY <= boxY + boxH - 14) {
            s_pd->graphics->drawText(txt + lineStart, txtLen - lineStart,
                                     kUTF8Encoding, innerX, lineY);
        }
    } else {
        s_pd->graphics->drawText(txt, txtLen, kUTF8Encoding, innerX,
                                 innerY + 22);
    }

    s_pd->graphics->popContext();
}

#else /* host build */

void ab_init(PlaydateAPI* pd, int (*mainUpdate)(void*), void* updateUd) {
    (void)pd; (void)mainUpdate; (void)updateUd;
}
void ab_open(const char* currentUrl, AbSubmitFn onSubmit, void* ud) {
    (void)currentUrl; (void)onSubmit; (void)ud;
}
void ab_launch_keyboard(void) {}
void ab_cancel(void) {}
void ab_draw_overlay(void) {}

#endif
