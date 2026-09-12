/*
 * PlutoBrowser — address_bar.c
 * Address Bar & Web Search Controller (port of Source/ui/address_bar.lua).
 *
 * Preserved behavior (Lua reference):
 *  - open(): isOpen=true, keyboardShown=false, prefill = currentUrl unless
 *    it matches ^about: (empty otherwise).
 *  - launchKeyboard(): no-op if not open or already shown or B held; installs
 *    willHide/textChanged, show(inputText).
 *  - willHide(submitted): submitted + trimmed text non-empty →
 *    isSearchQuery ? buildSearchUrl(engine.url, text) : parse(text).normalized;
 *    close; skipInputFrames=2; fire onSubmit(finalUrl). Cancel OR empty
 *    submit → close; skipInputFrames=2; no callback.
 *  - cancel(): hide keyboard if shown, close, drop submit callback.
 *  - drawOverlay(): white rounded box, black border; compact rect
 *    (10,6,380x48) when keyboard hidden, tall (4,4,192x232) when shown;
 *    clipped label "Enter URL or Search:" (body-bold) + input text (mono);
 *    text wraps when keyboard shown (14px line height), single line otherwise.
 */
#include "address_bar.h"

#include <string.h>

#include "pd_api.h"
#include "core/constants.h"
#include "core/url.h"
#include "core/storage.h"
#include "render/style.h"
#include "keyboard/keyboard.h"
#include "core/logger.h"

extern PlaydateAPI *playdate; /* keyboard port contract */

void pluto_free(void *p); /* SDK realloc(0) wrapper (defined in main.c) */

/* Shared keyboard instance (defined in main.c, Phase 13). */
PDKeyboard *app_keyboard(void);

/* ── Lua-reference state ──────────────────────────────────────────────────── */
static int g_isOpen = 0;
static int g_keyboardShown = 0;
static char g_inputText[512] = "";
static AddressBarSubmitFn g_onSubmit = NULL;
static void *g_onSubmitUserdata = NULL;

/* Lua global skipInputFrames is owned by main; the bar requests frames. */
static int g_pendingSkipFrames = 0;

static void ab_close(void)
{
    g_isOpen = 0;
    g_keyboardShown = 0;
}

/* ── Submit routing (Lua willHide submitted branch) ─────────────────────────
 * Shared by the real keyboard willHide callback and the test hook. */
static void ab_route_submit(const char *rawText)
{
    static char trimmed[512]; /* hoisted: device gameTask stack is tiny */
    snprintf(trimmed, sizeof(trimmed), "%s", rawText ? rawText : "");

    /* Lua: gsub("^%s*(.-)%s*$", "%1") — trim surrounding whitespace. */
    char *s = trimmed;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
    {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    {
        e--;
    }
    *e = '\0';

    g_pendingSkipFrames = 2; /* Lua: skipInputFrames = 2 on every exit path */

    if (s[0] == '\0')
    {
        /* Empty submit: close, no callback (Lua parity). */
        ab_close();
        g_onSubmit = NULL;
        g_onSubmitUserdata = NULL;
        return;
    }

    static char finalUrl[768]; /* hoisted: device gameTask stack is tiny */
    if (url_is_search_query(s))
    {
        /* searchEngine is stored 1-based (Lua table parity: settings page
         * clamps 1..N and indexes Constants.SEARCH_ENGINES[searchEngine]). */
        int engineIdx = storage_setting_int("searchEngine");
        if (engineIdx < 1 || engineIdx > SEARCH_ENGINE_COUNT)
        {
            engineIdx = 1;
        }
        char *built = url_build_search_url(SEARCH_ENGINES[engineIdx - 1].url, s);
        if (built)
        {
            snprintf(finalUrl, sizeof(finalUrl), "%s", built);
            pluto_free(built);
        }
        else
        {
            finalUrl[0] = '\0';
        }
    }
    else
    {
        static UrlParsed parsed; /* hoisted: device gameTask stack is tiny */
        memset(&parsed, 0, sizeof(parsed));
        if (url_parse(s, &parsed) == 0) /* 0 = success (was inverted: URL submits never fired) */
        {
            snprintf(finalUrl, sizeof(finalUrl), "%s", parsed.normalized);
        }
        else
        {
            finalUrl[0] = '\0';
        }
    }

    ab_close();

    if (finalUrl[0] != '\0')
    {
        logger_log("AddressBar submit: %s", finalUrl);
        if (g_onSubmit)
        {
            AddressBarSubmitFn cb = g_onSubmit;
            void *ud = g_onSubmitUserdata;
            g_onSubmit = NULL;
            g_onSubmitUserdata = NULL;
            cb(finalUrl, ud);
        }
    }
}

/* ── Keyboard callbacks (Lua installed the same way in launchKeyboard) ────── */

static void ab_keyboard_will_hide(int submitted, void *ud)
{
    (void)ud;
    if (submitted)
    {
        char *txt = NULL;
        unsigned int n = 0;
        keyboardApi.getText(app_keyboard(), &txt, &n);
        char buf[512] = "";
        if (txt)
        {
            snprintf(buf, sizeof(buf), "%s", txt);
            pluto_free(txt);
        }
        ab_route_submit(buf);
        return;
    }

    /* Cancel: close + skip frames, no callback (Lua parity). */
    g_pendingSkipFrames = 2;
    ab_close();
    g_onSubmit = NULL;
    g_onSubmitUserdata = NULL;
}

static void ab_keyboard_text_changed(void *ud)
{
    (void)ud;
    /* Lua: AddressBar.inputText = playdate.keyboard.text or "". */
    char *txt = NULL;
    unsigned int n = 0;
    keyboardApi.getText(app_keyboard(), &txt, &n);
    if (txt)
    {
        snprintf(g_inputText, sizeof(g_inputText), "%s", txt);
        pluto_free(txt);
    }
    else
    {
        g_inputText[0] = '\0';
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void address_bar_init(void)
{
    g_isOpen = 0;
    g_keyboardShown = 0;
    g_inputText[0] = '\0';
    g_onSubmit = NULL;
    g_onSubmitUserdata = NULL;
    g_pendingSkipFrames = 0;
}

void address_bar_open(const char *currentUrl, AddressBarSubmitFn onSubmit,
                      void *userdata)
{
    g_isOpen = 1;
    g_keyboardShown = 0;
    g_onSubmit = onSubmit;
    g_onSubmitUserdata = userdata;

    const char *initial = "";
    if (currentUrl && strncmp(currentUrl, "about:", 6) != 0)
    {
        initial = currentUrl;
    }
    snprintf(g_inputText, sizeof(g_inputText), "%s", initial);
    /* Keyboard is NOT shown here; it launches on B release (Lua parity). */
}

void address_bar_launch_keyboard(void)
{
    if (!g_isOpen || g_keyboardShown)
    {
        return;
    }
    /* Don't show keyboard while B is held (lets B+Left/Right work first). */
    PDButtons current = 0, pushed = 0, released = 0;
    playdate->system->getButtonState(&current, &pushed, &released);
    if (current & (1 << 4)) /* kButtonB */
    {
        return;
    }

    g_keyboardShown = 1;

    /* The keyboard port requires the app update callback to be armed before
     * show() (Phase 13 contract; main owns it and arms it at boot). */
    PDKeyboard *kb = app_keyboard();
    keyboardApi.setKeyboardWillHideCallback(kb, ab_keyboard_will_hide, NULL);
    keyboardApi.setTextChangedCallback(kb, ab_keyboard_text_changed, NULL);
    keyboardApi.show(kb, g_inputText, (unsigned int)strlen(g_inputText));
}

void address_bar_cancel(void)
{
    PDKeyboard *kb = app_keyboard();
    if (g_keyboardShown && keyboardApi.isVisible(kb))
    {
        keyboardApi.hide(kb);
    }
    ab_close();
    g_onSubmit = NULL;
    g_onSubmitUserdata = NULL;
}

int address_bar_is_open(void)
{
    return g_isOpen;
}

int address_bar_keyboard_shown(void)
{
    return g_keyboardShown;
}

void address_bar_sync_text(void)
{
    if (!g_keyboardShown)
    {
        return;
    }
    char *txt = NULL;
    unsigned int n = 0;
    keyboardApi.getText(app_keyboard(), &txt, &n);
    if (txt)
    {
        snprintf(g_inputText, sizeof(g_inputText), "%s", txt);
        pluto_free(txt);
    }
}

int address_bar_consume_skip_frames(void)
{
    int v = g_pendingSkipFrames;
    g_pendingSkipFrames = 0;
    return v;
}

const char *address_bar_input_text(void)
{
    return g_inputText;
}

/* Test hook: routes text through the real submit path without the keyboard
 * (P13 already verified raw keyboard input end-to-end). */
void address_bar_test_submit(const char *text)
{
    ab_route_submit(text);
}

/* ── drawOverlay (Lua geometry preserved) ─────────────────────────────────── */

void address_bar_draw_overlay(void)
{
    if (!g_isOpen)
    {
        return;
    }

    PlaydateAPI *pd = playdate;
    int boxX, boxY, boxW, boxH;
    if (g_keyboardShown)
    {
        boxX = 4;
        boxY = 4;
        boxW = 192;
        boxH = SCREEN_HEIGHT - 8; /* 232 */
    }
    else
    {
        boxX = 10;
        boxY = 6;
        boxW = SCREEN_WIDTH - 20; /* 380 */
        boxH = 48;
    }

    pd->graphics->fillRoundRect(boxX, boxY, boxW, boxH, 6, kColorWhite);
    pd->graphics->drawRoundRect(boxX, boxY, boxW, boxH, 6, 1, kColorBlack);

    /* Clip to box interior. */
    pd->graphics->pushContext(NULL);
    pd->graphics->setClipRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4);

    const int innerX = boxX + 10;
    const int innerY = boxY + 10;
    const int innerW = boxW - 20;

    LCDFont *bodyBold = style_font(PLUTO_FONT_BODY_BOLD);
    pd->graphics->setFont(bodyBold);
    pd->graphics->drawText("Enter URL or Search:", strlen("Enter URL or Search:"),
                           kASCIIEncoding, innerX, innerY);

    const char *txt = g_inputText;
    LCDFont *mono = style_font(PLUTO_FONT_MONO);
    pd->graphics->setFont(mono);

    if (g_keyboardShown)
    {
        /* Wrap text to fill the box vertically (Lua char-loop parity). */
        int lineY = innerY + 22;
        const int lineHeight = 14;
        size_t len = strlen(txt);
        size_t lineStart = 0;
        char lineBuf[256];

        for (size_t i = 0; i < len; i++)
        {
            size_t testLen = i - lineStart + 1;
            if (testLen >= sizeof(lineBuf))
            {
                testLen = sizeof(lineBuf) - 1; /* safety: never overflow */
            }
            memcpy(lineBuf, txt + lineStart, testLen);
            lineBuf[testLen] = '\0';

            int tw = style_get_text_width(PLUTO_FONT_MONO, lineBuf);
            if (tw > innerW && i > lineStart)
            {
                pd->graphics->drawText(lineBuf, strlen(lineBuf), kASCIIEncoding,
                                       innerX, lineY);
                lineY += lineHeight;
                lineStart = i;
                if (lineY > boxY + boxH - 14)
                {
                    break;
                }
            }
        }
        if (lineStart < len && lineY <= boxY + boxH - 14)
        {
            const char *rest = txt + lineStart;
            pd->graphics->drawText(rest, strlen(rest), kASCIIEncoding, innerX, lineY);
        }
    }
    else
    {
        pd->graphics->drawText(txt, strlen(txt), kASCIIEncoding, innerX, innerY + 22);
    }

    pd->graphics->popContext();
}
