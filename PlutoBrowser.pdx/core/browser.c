// browser.c -- [P32] C port of CometBrowser Source/main.lua.
//
// Application state machine: home/loading/page/error/bookmarks/history/
// settings, the navigateTo -> executeNavigation -> runNavigation flow,
// cooperative renderBody task, history stack (cap 30), <details> toggling,
// form activation/submission, reader-mode input with crank physics,
// loading/error/list/settings branches and the updateFrame composition
// order. The Lua playdate.system menu is rendered as a custom pause
// overlay (the C SDK has no menu-items API); kEventPause/kEventResume map
// to the hardware Menu button. HTML-mode virtual mouse cursor lands in
// P33 -- until then HTML mode scrolls by crank exactly like reader mode.

#include "core/browser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/constants.h"
#include "core/cookie_jar.h"
#include "core/encoding.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "core/storage.h"
#include "core/tasks.h"
#include "core/url.h"
#include "html/document.h"
#include "render/image_decoder.h"
#include "render/layout.h"
#include "render/link_manager.h"
#include "render/style.h"
#include "ui/address_bar.h"
#include "ui/bookmarks_page.h"
#include "ui/chrome.h"
#include "ui/error_page.h"
#include "ui/history_page.h"
#include "ui/home_page.h"
#include "ui/hud.h"
#include "ui/settings_page.h"
#include "util/mem.h"
#include "util/strbuf.h"
#include "util/strmap.h"

#include "pd_api.h"
#include "keyboard.h"

#define BR_MAX_HISTORY 30

/* ── engine state (module singleton, like every other module) ─────────── */

typedef struct {
    char key[24];
    int  open;
} BrDetailsEntry;

typedef struct BrRenderCtx {
    char* body;
    char* url;
    int   addHist;
    int   stage;
    struct DocDocument* doc;
} BrRenderCtx;

typedef struct BrToggleCtx {
    char* rawHtml;
    char* baseUrl;
    int   mode;
    char  key[24];
    struct DocDocument* doc;
} BrToggleCtx;

static struct {
    int      state;
    PlutoUrl curUrl;
    int      hasUrl;
    char     pageTitle[128];
    int      browseMode;

    double scrollY, targetScrollY, crankVelocity;

    long progressCur, progressTot;
    int  isRendering;

    char navHistory[BR_MAX_HISTORY][PLUTO_URL_NORMALIZED_MAX];
    int  nHistory, hIdx;

    char pendingNav[PLUTO_URL_NORMALIZED_MAX];
    int  hasPendingNav;

    struct LItem* activeInputField;
    int keyboardOpen;
    int skipInputFrames;

    int      bHoldActive, bHoldUsedDir, bNotPressedFrames;
    unsigned bHoldStartMs;
    int      navigatingHistory;

    /* mouse cursor state (driven in P33; kept for composition parity) */
    int   mouseX, mouseY;
    float mouseSpeed;

    BrDetailsEntry details[32];
    size_t         nDetails;

    double metaDeadlineMs;
    char   metaUrl[PLUTO_URL_NORMALIZED_MAX];
    int    hasMetaRefresh;

    unsigned lastFrameMs;
    int      haveLastFrame;

    struct DocDocument* currentDoc;

    /* custom pause-menu overlay */
    int menuOpen, menuSel;
} s_br;

static struct PlaydateAPI* s_pd = NULL;
static BrInputFn           s_inputFn   = NULL;
static void*               s_inputUd   = NULL;
static BrClockFn           s_clockFn   = NULL;
static int (*s_updateTrampoline)(void*) = NULL;
static PDKeyboard*         s_kb        = NULL;
static char                s_kbText[512];

/* hover-image tracking (IMAGE_MODE_HOVER) */
static char* s_hoveredImageSrc = NULL;

/* forward decls mirroring main.lua's local functions */
static void run_navigation(const char* urlString);
static void render_body(const char* body, const char* url, int addToHistory);
static void toggle_details(const char* dkey);
static void menu_rebuild(void);
static void target_scroll_step(const BrInput* in, int isHtmlMode,
                               float crankChange);
static void page_a_click(const BrInput* in, int isHtmlMode);

/* ── tiny helpers ─────────────────────────────────────────────────────── */

static unsigned br_now_ms(void) {
    return s_clockFn ? s_clockFn()
                     : (unsigned)s_pd->system->getCurrentTimeMilliseconds();
}

static void str_copy(char* dst, size_t cap, const char* src) {
    if (dst == NULL || cap == 0) return;
    size_t i = 0;
    if (src != NULL)
        while (i + 1 < cap && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void trim_in_place(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t' ||
           s[start] == '\r' || s[start] == '\n')
        start++;
    if (start > 0) memmove(s, s + start, n - start + 1);
}

static void item_set_value(struct LItem* it, const char* v) {
    if (!it) return;
    pluto_free(it->value);
    it->value = pluto_strdup(v ? v : "");
}

/* ── history stack (pushHistory/goBack/goForward, cap 30) ─────────────── */

static void push_history(const char* urlString) {
    /* navigating somewhere new clears the future */
    if (s_br.hIdx < s_br.nHistory - 1) s_br.nHistory = s_br.hIdx + 1;

    if (s_br.nHistory > 0 &&
        !strcmp(s_br.navHistory[s_br.nHistory - 1], urlString))
        return;

    if (s_br.nHistory < BR_MAX_HISTORY)
        str_copy(s_br.navHistory[s_br.nHistory++],
                 PLUTO_URL_NORMALIZED_MAX, urlString);
    else {
        memmove(s_br.navHistory, s_br.navHistory + 1,
                sizeof(s_br.navHistory[0]) * (BR_MAX_HISTORY - 1));
        str_copy(s_br.navHistory[BR_MAX_HISTORY - 1],
                 PLUTO_URL_NORMALIZED_MAX, urlString);
    }
    s_br.hIdx = s_br.nHistory - 1;
}

static int go_back_url(char* out, size_t cap) {
    if (s_br.hIdx <= 0) return 0;
    s_br.hIdx--;
    s_br.navigatingHistory = 1;
    str_copy(out, cap, s_br.navHistory[s_br.hIdx]);
    return 1;
}

static int go_forward_url(char* out, size_t cap) {
    if (s_br.hIdx >= s_br.nHistory - 1) return 0;
    s_br.hIdx++;
    s_br.navigatingHistory = 1;
    str_copy(out, cap, s_br.navHistory[s_br.hIdx]);
    return 1;
}

int br_go_back(void)    { char u[PLUTO_URL_NORMALIZED_MAX];
                          return go_back_url(u, sizeof(u)); }
int br_go_forward(void) { char u[PLUTO_URL_NORMALIZED_MAX];
                          return go_forward_url(u, sizeof(u)); }

/* ── details-open set ─────────────────────────────────────────────────── */

static BrDetailsEntry* details_find(const char* key) {
    for (size_t i = 0; i < s_br.nDetails; i++)
        if (!strcmp(s_br.details[i].key, key)) return &s_br.details[i];
    return NULL;
}

static int details_toggle(const char* key) {
    BrDetailsEntry* e = details_find(key);
    if (e) { e->open = !e->open; return e->open; }
    if (s_br.nDetails >= sizeof(s_br.details) / sizeof(s_br.details[0]))
        return 0;
    e = &s_br.details[s_br.nDetails++];
    str_copy(e->key, sizeof(e->key), key);
    e->open = 1;
    return 1;
}

static size_t details_build_overrides(const DocDetailsOverride** out) {
    static DocDetailsOverride ov[32];
    size_t n = 0;
    for (size_t i = 0; i < s_br.nDetails && n < 32; i++) {
        ov[n].key  = s_br.details[i].key;
        ov[n].open = s_br.details[i].open;
        n++;
    }
    *out = ov;
    return n;
}

/* ── keyboard (vendored C lib; same wiring as address_bar.c) ──────────── */

static const char* kb_text_snapshot(void) {
    if (s_pd == NULL || s_kb == NULL) return s_kbText;
    char* text = NULL;
    unsigned int count = 0;
    keyboardApi.getText(s_kb, &text, &count);
    if (text != NULL) {
        str_copy(s_kbText, sizeof(s_kbText), count > 0 ? text : "");
        pluto_free(text);
    } else {
        s_kbText[0] = '\0';
    }
    return s_kbText;
}

/* keyboardDidHideCallback: commit text into the focused field, honouring
 * maxlength truncation exactly like the Lua handler. */
static void keyboard_finalize(const char* entered) {
    s_br.keyboardOpen = 0;
    s_br.skipInputFrames = 2;
    if (entered == NULL) entered = "";
    if (s_br.activeInputField != NULL) {
        struct LItem* f = s_br.activeInputField;
        if (f->maxlength > 0 && (int)strlen(entered) > f->maxlength) {
            char clipped[512];
            size_t n = (size_t)f->maxlength;
            if (n >= sizeof(clipped)) n = sizeof(clipped) - 1;
            memcpy(clipped, entered, n);
            clipped[n] = '\0';
            item_set_value(f, clipped);
        } else {
            item_set_value(f, entered);
        }
    }
    s_br.activeInputField = NULL;
}

static void kb_will_hide(int okButtonPressed, void* userdata) {
    (void)okButtonPressed;   /* Lua commits text on cancel too */
    (void)userdata;
    str_copy(s_kbText, sizeof(s_kbText), kb_text_snapshot());
    keyboard_finalize(s_kbText);
}

void br_keyboard_finalize(const char* text) {
    keyboard_finalize(text);
}

int br_keyboard_open(void) { return s_br.keyboardOpen; }

static int s_keyboardEnabled = 1;

static void open_keyboard_for_input(struct LItem* inputBlock) {
    if (inputBlock == NULL) return;
    s_br.activeInputField = inputBlock;
    s_br.keyboardOpen = 1;
    s_kbText[0] = '\0';
    if (s_keyboardEnabled && s_pd != NULL && s_kb != NULL) {
        keyboardApi.setKeyboardWillHideCallback(s_kb, kb_will_hide, NULL);
        keyboardApi.show(s_kb, inputBlock->value ? inputBlock->value : "",
                         inputBlock->value ? strlen(inputBlock->value) : 0u);
    }
}

void br_set_keyboard_enabled(int on) { s_keyboardEnabled = on; }

void br_reset_for_tests(void) {
    tasks_cancel_all();
    int kbEnabled = s_keyboardEnabled;
    memset(&s_br, 0, sizeof(s_br));
    s_br.mouseX = 200;
    s_br.mouseY = 120;
    s_br.mouseSpeed = 4.0f;
    s_br.state = PLUTO_STATE_HOME;
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "CometBrowser");
    s_keyboardEnabled = kbEnabled;
}

/* ── form activation + submission ─────────────────────────────────────── */

static void navigate_to(const char* urlString);   /* fwd */

void br_navigate_to(const char* url) { navigate_to(url); }

static void add_pair(StrBuf* pairs, const char** seen, int* nSeen,
                     const char* k, const char* v) {
    if (k == NULL || k[0] == '\0') return;
    for (int i = 0; i < *nSeen; i++)
        if (!strcmp(seen[i], k)) return;
    if (*nSeen < 64) seen[(*nSeen)++] = k;
    StrBuf ek, ev;
    sb_init(&ek); sb_init(&ev);
    url_encode(k, &ek);
    url_encode(v ? v : "", &ev);
    if (pairs->len > 0) sb_append_str(pairs, "&");
    sb_append_str(pairs, ek.data ? ek.data : "");
    sb_append_str(pairs, "=");
    sb_append_str(pairs, ev.data ? ev.data : "");
    sb_free(&ek); sb_free(&ev);
}

/* submitForm(formAction, inputBlock): query built from the CURRENT render
 * items exactly as the Lua version does (hidden/text match by formAction,
 * checked checkboxes send value-or-"on", selects send the active option,
 * dedupe by name, clicked submit button appended per HTML spec, GET
 * assembly onto formAction with ?/& separator). */
static void submit_form(const char* formActionIn, struct LItem* submitBtn) {
    const char* fallback = s_br.hasUrl ? s_br.curUrl.normalized : "";
    const char* formAction =
        (formActionIn && formActionIn[0] != '\0') ? formActionIn : fallback;
    if (!strcmp(formAction, "#")) return;

    const char* seen[64];
    int nSeen = 0;
    StrBuf q;
    sb_init(&q);

    for (int i = 0; i < layout_item_count(); i++) {
        const LItem* it = layout_item_at(i);
        if (it->disabled) continue;          /* never submitted */
        int actionMatch = it->formAction &&
                          !strcmp(it->formAction, formAction);
        switch (it->type) {
        case LIT_HIDDEN_FIELD:
        case LIT_INPUT_FIELD:
            if (actionMatch) add_pair(&q, seen, &nSeen, it->name, it->value);
            break;
        case LIT_CHECKBOX_FIELD:
            if (actionMatch && it->checked)
                add_pair(&q, seen, &nSeen, it->name,
                         (it->value && it->value[0]) ? it->value : "on");
            break;
        case LIT_SELECT_FIELD: {
            if (!actionMatch) break;
            const DocSelectOpt* opts = (const DocSelectOpt*)it->options;
            if (it->selectedIndex >= 1 &&
                (size_t)it->selectedIndex <= it->nOptions) {
                const DocSelectOpt* opt =
                    &opts[it->selectedIndex - 1];
                if (!opt->disabled && !opt->group)
                    add_pair(&q, seen, &nSeen, it->name,
                             (opt->value && opt->value[0]) ? opt->value
                                                           : opt->text);
            }
            break;
        }
        default:
            break;
        }
    }

    /* no fields matched by formAction: collect all named visible inputs */
    if (q.len == 0) {
        for (int i = 0; i < layout_item_count(); i++) {
            const LItem* it = layout_item_at(i);
            if (it->disabled) continue;
            if ((it->type == LIT_HIDDEN_FIELD ||
                 it->type == LIT_INPUT_FIELD) &&
                it->name && it->name[0])
                add_pair(&q, seen, &nSeen, it->name, it->value);
        }
    }

    /* the clicked submit button's name=value always rides along */
    if (submitBtn && !submitBtn->disabled &&
        submitBtn->name && submitBtn->name[0])
        add_pair(&q, seen, &nSeen, submitBtn->name, submitBtn->value);

    const char* sep = strchr(formAction, '?') ? "&" : "?";
    char target[2 * PLUTO_URL_NORMALIZED_MAX];
    snprintf(target, sizeof(target), "%s%s%s", formAction,
             q.len > 0 ? sep : "", q.len > 0 ? (q.data ? q.data : "") : "");
    sb_free(&q);
    navigate_to(target);
}

/* activateFormBlock(block): text input / submit / checkbox-radio toggle /
 * dropdown cycle. `block` is a live render item (Layout.renderItems in
 * Lua; LItem copies here -- mutations persist until the next build). */
static void activate_form_block(struct LItem* blk) {
    if (blk == NULL || blk->disabled) return;

    switch (blk->type) {
    case LIT_INPUT_FIELD:
        layout_set_selected_input(blk);
        open_keyboard_for_input(blk);
        break;

    case LIT_INPUT_SUBMIT:
        submit_form(blk->formAction, blk);
        break;

    case LIT_CHECKBOX_FIELD:
        if (blk->radio) {
            for (int i = 0; i < layout_item_count(); i++) {
                LItem* it = layout_item_mutable(i);
                if (it != blk && it->type == LIT_CHECKBOX_FIELD &&
                    it->radio && it->name && blk->name &&
                    !strcmp(it->name, blk->name))
                    it->checked = 0;
            }
            blk->checked = 1;
        } else {
            blk->checked = !blk->checked;
        }
        layout_set_selected_input(blk);
        break;

    case LIT_SELECT_FIELD: {
        size_t n = blk->nOptions;
        if (n > 0) {
            const DocSelectOpt* opts = (const DocSelectOpt*)blk->options;
            int tries = (int)n + 1;
            while (tries > 0) {
                blk->selectedIndex = (blk->selectedIndex % (int)n) + 1;
                const DocSelectOpt* opt =
                    &opts[blk->selectedIndex - 1];
                if (!opt->disabled && !opt->group) break;
                tries--;
            }
        }
        layout_set_selected_input(blk);
        break;
    }

    default:
        break;
    }
}

void br_activate_item(struct LItem* it) { activate_form_block(it); }

/* ── custom pause menu (playdate.system menu replacement) ─────────────── */

enum { BR_MI_HOME = 0, BR_MI_VIEW, BR_MI_SETTINGS, BR_MI_HISTORY,
       BR_MI_COOKIES };

static struct {
    int present[5];
    int viewIndex;          /* row index of the View options entry */
    int count;
} s_menu;

static void apply_view_mode(int newMode) {
    s_br.browseMode = newMode;
    storage_settings()->mode = newMode;
    storage_save();
    if (s_br.hasUrl) {
        if (s_br.currentDoc != NULL && s_br.currentDoc->rawHtml != NULL &&
            s_br.currentDoc->rawHtml[0] != '\0')
            render_body(s_br.currentDoc->rawHtml,
                        s_br.curUrl.normalized, 0);
        else
            navigate_to(s_br.curUrl.normalized);
    }
}

static void menu_activate(int row);

static void menu_rebuild(void) {
    /* rows are frozen while the popup is open: a page landing underneath
     * (br_boot's auto-launch, background loads) must not shift indices */
    if (s_br.menuOpen) return;
    memset(s_menu.present, 0, sizeof(s_menu.present));
    s_menu.count = 0;
    s_menu.present[BR_MI_HOME]     = 1;
    s_menu.present[BR_MI_SETTINGS] = 1;   /* Lua: always-added entries */
    s_menu.present[BR_MI_HISTORY]  = 1;
    s_menu.present[BR_MI_COOKIES]  = 1;
    s_menu.viewIndex = -1;
    int row = 0;
    for (int kind = 0; kind < 5; kind++) {
        if (!s_menu.present[kind]) continue;
        if (kind == BR_MI_VIEW) continue;          /* counted below */
        row++;
    }
    if (s_br.state == PLUTO_STATE_PAGE && s_br.hasUrl &&
        strcmp(s_br.curUrl.scheme, "about") != 0) {
        s_menu.present[BR_MI_VIEW] = 1;
        s_menu.viewIndex = 1;                      /* right after Home */
        row++;
    }
    s_menu.count = row;
}

static void menu_action_home(void) {
    str_copy(s_br.pendingNav, sizeof(s_br.pendingNav), "about:home");
    s_br.hasPendingNav = 1;
}

static void menu_action_settings(void) {
    sp_open(pluto_state_name((PlutoState)s_br.state));
    s_br.state = PLUTO_STATE_SETTINGS;
}

static void menu_action_history(void) {
    hi_open();
    s_br.state = PLUTO_STATE_HISTORY;
}

static void menu_action_cookies(void) { cj_clear(); }

static void menu_activate(int row) {
    /* Lua parity: picking a system-menu item dismisses the menu (the SDK
     * closes it and a kEventResume follows); our custom overlay mirrors
     * the observable result */
    s_br.menuOpen = 0;
    int idx = 0;
    for (int kind = 0; kind < 5; kind++) {
        if (!s_menu.present[kind]) continue;
        if (idx == row) {
            switch (kind) {
            case BR_MI_HOME:     menu_action_home(); break;
            case BR_MI_SETTINGS: menu_action_settings(); break;
            case BR_MI_HISTORY:  menu_action_history(); break;
            case BR_MI_COOKIES:  menu_action_cookies(); break;
            case BR_MI_VIEW: {
                int next = (s_br.browseMode == PLUTO_MODE_READER)
                               ? PLUTO_MODE_RAW_HTML
                               : PLUTO_MODE_READER;
                apply_view_mode(next);
                break;
            }
            default: break;
            }
            return;
        }
        idx++;
    }
}

static void menu_cycle_view(int dir) {
    int next = s_br.browseMode +
               (dir > 0 ? 1 : -1);
    if (next < 0) next = PLUTO_MODE_RAW_HTML;
    if (next > PLUTO_MODE_RAW_HTML) next = PLUTO_MODE_READER;
    apply_view_mode(next);
}

void br_on_pause(void)  { menu_rebuild(); s_br.menuSel = 0;
                          s_br.menuOpen = 1; }
void br_on_resume(void) { s_br.menuOpen = 0; }
int  br_menu_open(void) { return s_br.menuOpen; }
int  br_menu_sel(void)  { return s_br.menuSel; }
int  br_mouse_x(void)   { return s_br.mouseX; }
int  br_mouse_y(void)   { return s_br.mouseY; }
void br_set_mouse_for_tests(int x, int y) {
    s_br.mouseX = x; s_br.mouseY = y;
}

/* ── Native system menu (Lua main.lua parity) ──────────────────────────
 * The C_API DOES expose playdate->system menu items (the old custom
 * overlay premise was wrong): Home-Page / View(Reader|HTML) / Settings /
 * History / Clear Cookies, rebuilt whenever the View row's presence or
 * the current mode changes. Callbacks only stage work; it executes in
 * the next br_frame() after the system menu resumes the game. */

static PDMenuItem* s_viewItem = NULL;

static void mi_home_cb(void* ud)    { (void)ud; menu_action_home(); }
static void mi_settings_cb(void* ud){ (void)ud; menu_action_settings();
                                      br_system_menu_refresh(); }
static void mi_history_cb(void* ud) { (void)ud; menu_action_history();
                                      br_system_menu_refresh(); }
static void mi_cookies_cb(void* ud) { (void)ud; cj_clear(); }

static void mi_view_cb(void* ud) {
    (void)ud;
    int v = s_pd->system->getMenuItemValue(s_viewItem);
    /* Lua: options are { "Reader", "HTML" } */
    apply_view_mode(v == 0 ? PLUTO_MODE_READER : PLUTO_MODE_RAW_HTML);
}

void br_system_menu_refresh(void) {
    if (s_pd == NULL) return;
    s_pd->system->removeAllMenuItems();
    s_pd->system->addMenuItem("Home-Page", mi_home_cb, NULL);
    s_viewItem = NULL;
    if (s_br.state == PLUTO_STATE_PAGE && s_br.hasUrl &&
        strcmp(s_br.curUrl.scheme, "about") != 0) {
        static const char* opts[2] = { "Reader", "HTML" };
        s_viewItem = s_pd->system->addOptionsMenuItem(
            "View", opts, 2, mi_view_cb, NULL);
        /* Lua: initial selection reflects the active browse mode */
        s_pd->system->setMenuItemValue(
            s_viewItem,
            s_br.browseMode == PLUTO_MODE_READER ? 0 : 1);
    }
    s_pd->system->addMenuItem("Settings", mi_settings_cb, NULL);
    s_pd->system->addMenuItem("History", mi_history_cb, NULL);
    s_pd->system->addMenuItem("Clear Cookies", mi_cookies_cb, NULL);
}

static void menu_draw(void) {
    if (!s_br.menuOpen) return;
    const char* viewLabel =
        (s_br.browseMode == PLUTO_MODE_READER) ? "Reader" : "HTML";

    int rowH = 26;
    int w = 190;
    int x = PLUTO_SCREEN_WIDTH - w - 6;
    int y = 6;
    int h = s_menu.count * rowH + 12;
    s_pd->graphics->fillRect(x, y, w, h, kColorWhite);
    s_pd->graphics->drawRect(x, y, w, h, kColorBlack);

    PlutoFont* body = style_get_body_font(0, 0, NULL);
    s_pd->graphics->setFont((LCDFont*)body);

    int row = 0;
    for (int kind = 0; kind < 5; kind++) {
        if (!s_menu.present[kind]) continue;
        int ry = y + 6 + row * rowH;
        int selected = (row == s_br.menuSel);
        if (selected) {
            s_pd->graphics->fillRect(x + 2, ry, w - 4, rowH - 2,
                                     kColorBlack);
            s_pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        const char* label = "";
        switch (kind) {
        case BR_MI_HOME:     label = "Home-Page"; break;
        case BR_MI_VIEW:     label = "View"; break;
        case BR_MI_SETTINGS: label = "Settings"; break;
        case BR_MI_HISTORY:  label = "History"; break;
        case BR_MI_COOKIES:  label = "Clear Cookies"; break;
        default: break;
        }
        s_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                 x + 10, ry + 5);
        if (kind == BR_MI_VIEW) {
            s_pd->graphics->drawText(viewLabel, strlen(viewLabel),
                                     kUTF8Encoding, x + w - 70, ry + 5);
        }
        if (selected) s_pd->graphics->setDrawMode(kDrawModeCopy);
        row++;
    }
}

static void menu_handle_input(const BrInput* in) {
    if (in->justPressed & BR_BTN_UP && s_br.menuSel > 0)
        s_br.menuSel--;
    if (in->justPressed & BR_BTN_DOWN &&
        s_br.menuSel < s_menu.count - 1)
        s_br.menuSel++;
    if ((in->justPressed & BR_BTN_LEFT ||
         in->justPressed & BR_BTN_RIGHT) &&
        s_menu.viewIndex == s_br.menuSel)
        menu_cycle_view(in->justPressed & BR_BTN_RIGHT ? 1 : -1);
    if (in->justPressed & BR_BTN_A)
        menu_activate(s_br.menuSel);
    if (in->justPressed & BR_BTN_B)
        s_br.menuOpen = 0;
}

/* ── navigation core ──────────────────────────────────────────────────── */

static void nav_on_progress(void* ud, int cur, int tot) {
    (void)ud;
    s_br.progressCur = cur;
    s_br.progressTot = tot;
}

static void nav_on_success(void* ud, int status, const StrMap* headers,
                           const char* body, size_t bodyLen,
                           const char* finalUrl) {
    (void)ud; (void)status; (void)headers;
    const char* resolved = (finalUrl && finalUrl[0])
                               ? finalUrl : s_br.curUrl.normalized;
    url_parse(resolved, &s_br.curUrl);
    s_br.hasUrl = 1;

    const char* contentType =
        (const char*)sm_get(headers, "content-type");
    size_t utf8Len = 0;
    char* utf8 = encoding_to_utf8(body, bodyLen, contentType, &utf8Len);
    render_body(utf8 ? utf8 : "", resolved, 1);
    pluto_free(utf8);
}

static void nav_on_error(void* ud, const char* msg) {
    (void)ud;
    ep_show(msg, s_br.curUrl.normalized);
    s_br.state    = PLUTO_STATE_ERROR;
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "Connection Error");
    menu_rebuild();
}

static void run_navigation(const char* urlString) {
    if (urlString == NULL || urlString[0] == '\0') return;

    /* drop any running parse/render of a previous page so its completion
     * cannot hijack this navigation */
    tasks_cancel_all();
    s_br.isRendering = 0;

    static char url[PLUTO_URL_INPUT_MAX * 2];
    StrBuf unwrapped;
    sb_init(&unwrapped);
    if (url_unwrap_redirect(urlString, &unwrapped) && unwrapped.data)
        str_copy(url, sizeof(url), unwrapped.data);
    else
        str_copy(url, sizeof(url), urlString);
    sb_free(&unwrapped);
    trim_in_place(url);

    s_br.activeInputField = NULL;
    if (s_br.keyboardOpen) {
        if (s_pd != NULL && s_kb != NULL) keyboardApi.hide(s_kb);
        s_br.keyboardOpen = 0;
    }
    id_clear_cache();

    if (!strcmp(url, "about:home")) {
        s_br.state = PLUTO_STATE_HOME;
        url_parse("about:home", &s_br.curUrl);
        s_br.hasUrl = 1;
        str_copy(s_br.pageTitle, sizeof(s_br.pageTitle),
                 "CometBrowser Start Page");
        if (!s_br.navigatingHistory) push_history(url);
        s_br.scrollY = s_br.targetScrollY = s_br.crankVelocity = 0;
        menu_rebuild();
        return;
    }

    if (strncmp(url, "http://", 7) != 0 &&
        strncmp(url, "https://", 8) != 0 &&
        strncmp(url, "about:", 6) != 0) {
        static char rebuilt[PLUTO_URL_INPUT_MAX * 2];
        if (url_is_search_query(url)) {
            StrBuf enc, enc2;
            sb_init(&enc);
            sb_append_str(&enc, "https://html.duckduckgo.com/html/?q=");
            sb_init(&enc2);
            url_encode(url, &enc2);
            sb_append_str(&enc, enc2.data ? enc2.data : "");
            sb_free(&enc2);
            snprintf(rebuilt, sizeof(rebuilt), "%s",
                     enc.data ? enc.data : "");
            sb_free(&enc);
        } else {
            /* https:// + input; pathological overflow truncates */
            size_t o = 0;
            const char* prefix = "https://";
            while (o < 8) { rebuilt[o] = prefix[o]; o++; }
            const char* p = url;
            while (*p != '\0' && o < sizeof(rebuilt) - 1)
                rebuilt[o++] = *p++;
            rebuilt[o] = '\0';
        }
        str_copy(url, sizeof(url), rebuilt);
    }

    url_parse(url, &s_br.curUrl);
    s_br.hasUrl = 1;
    s_br.state  = PLUTO_STATE_LOADING;
    s_br.progressCur = 0;
    s_br.progressTot = 0;
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "Loading...");
    if (!s_br.navigatingHistory) push_history(url);
    s_br.navigatingHistory = 0;
    menu_rebuild();

    PlutoHttpCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.onProgress = nav_on_progress;
    cbs.onSuccess  = nav_on_success;
    cbs.onError    = nav_on_error;
    hc_get(url, &cbs);
}

static void navigate_to(const char* urlString) {
    if (urlString == NULL) return;
    str_copy(s_br.pendingNav, sizeof(s_br.pendingNav), urlString);
    s_br.hasPendingNav = 1;
}

/* ── renderBody cooperative task ──────────────────────────────────────── */

static void render_ctx_free(void* ctx) {
    BrRenderCtx* c = (BrRenderCtx*)ctx;
    pluto_free(c->body);
    pluto_free(c->url);
    pluto_free(c);
}

static int render_step(void* ctx) {
    BrRenderCtx* c = (BrRenderCtx*)ctx;
    if (c->stage == 0) {
        c->doc = doc_parse_opts(c->body, c->url, s_br.browseMode, NULL);
        if (c->doc == NULL) {
            tasks_set_error("Parse Error: document unavailable");
            return PLUTO_TASK_ERROR;
        }
        tasks_report_progress(0.5);
        c->stage = 1;
        return PLUTO_TASK_YIELD;   /* yield: layout next slice */
    }
    layout_build(c->doc);
    tasks_report_progress(1.0);
    return PLUTO_TASK_DONE;
}

static void render_done(void* ctx, void* ud) {
    (void)ud;
    BrRenderCtx* c = (BrRenderCtx*)ctx;
    s_br.isRendering = 0;
    s_br.currentDoc  = c->doc;
    const char* t = c->doc->title;
    if (t == NULL || t[0] == '\0')
        t = (s_br.curUrl.host[0] != '\0') ? s_br.curUrl.host : "Web Page";
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), t);
    s_br.scrollY = s_br.targetScrollY = s_br.crankVelocity = 0;
    s_br.state = PLUTO_STATE_PAGE;
    if (c->addHist) storage_add_history(s_br.pageTitle, c->url);
    menu_rebuild();
    br_system_menu_refresh();   /* View row tracks PAGE+hasUrl */

    /* enqueue images per image mode setting */
    if ((storage_settings()->imageMode) == PLUTO_IMAGE_MODE_ALL) {
        for (size_t i = 0; i < c->doc->nBlocks; i++) {
            const DocBlock* b = &c->doc->blocks[i];
            if (b->type == DB_IMAGE && b->src && b->src[0])
                id_enqueue(b->src);
        }
    }

    /* <meta http-equiv=refresh> auto-redirect */
    if (c->doc->metaDelay > 0 || c->doc->metaUrl != NULL) {
        double delaySec = c->doc->metaDelay > 0 ? c->doc->metaDelay : 0;
        str_copy(s_br.metaUrl, sizeof(s_br.metaUrl),
                 c->doc->metaUrl ? c->doc->metaUrl : c->url);
        s_br.metaDeadlineMs =
            (double)br_now_ms() + delaySec * 1000.0;
        s_br.hasMetaRefresh = 1;
    }
}

static void render_fail(const char* msg, void* ud) {
    (void)msg;
    BrRenderCtx* c = (BrRenderCtx*)ud;
    if (c != NULL) {
        s_br.isRendering = 0;
        ep_show(msg, c->url);
        s_br.state = PLUTO_STATE_ERROR;
        str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "Render Error");
        menu_rebuild();
    }
}

static void render_body(const char* body, const char* url, int addToHistory) {
    s_br.isRendering = 1;
    s_br.progressCur = 0;
    s_br.progressTot = 0;
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "Rendering...");
    s_br.state = PLUTO_STATE_LOADING;

    BrRenderCtx* c = (BrRenderCtx*)pluto_malloc(sizeof(BrRenderCtx));
    memset(c, 0, sizeof(*c));
    c->body    = pluto_strdup(body ? body : "");
    c->url     = pluto_strdup(url ? url : "");
    c->addHist = addToHistory;
    c->stage   = 0;
    tasks_run(render_step, c, render_ctx_free, render_done, render_fail,
              c);   /* ctx rides as ud so fail() keeps context */
}

/* ── <details> toggling ───────────────────────────────────────────────── */

static void toggle_ctx_free(void* ctx) {
    BrToggleCtx* c = (BrToggleCtx*)ctx;
    pluto_free(c->rawHtml);
    pluto_free(c->baseUrl);
    pluto_free(c);
}

static int toggle_step(void* ctx) {
    BrToggleCtx* c = (BrToggleCtx*)ctx;
    const DocDetailsOverride* ovs = NULL;
    size_t nOv = details_build_overrides(&ovs);
    DocParseOpts opts;
    opts.detailsOverrides = ovs;
    opts.nOverrides = nOv;
    c->doc = doc_parse_opts(c->rawHtml, c->baseUrl, c->mode, &opts);
    if (c->doc == NULL) {
        tasks_set_error("Parse Error: document unavailable");
        return PLUTO_TASK_ERROR;
    }
    layout_build(c->doc);
    return PLUTO_TASK_DONE;
}

static void toggle_done(void* ctx, void* ud) {
    (void)ud;
    BrToggleCtx* c = (BrToggleCtx*)ctx;
    s_br.isRendering = 0;
    if (s_br.currentDoc != NULL) doc_free(s_br.currentDoc);
    s_br.currentDoc = c->doc;
    c->doc = NULL;   /* ownership moved */
    /* scroll preserved across a toggle */
}

static void toggle_fail(const char* msg, void* ud) {
    (void)msg;
    BrToggleCtx* c = (BrToggleCtx*)ud;
    s_br.isRendering = 0;
    if (c != NULL && c->key[0] != '\0')
        details_toggle(c->key);   /* flip back on failure */
}

static void toggle_details(const char* dkey) {
    if (s_br.currentDoc == NULL || s_br.currentDoc->rawHtml == NULL ||
        s_br.isRendering)
        return;
    details_toggle(dkey);

    BrToggleCtx* c = (BrToggleCtx*)pluto_malloc(sizeof(BrToggleCtx));
    memset(c, 0, sizeof(*c));
    c->rawHtml = pluto_strdup(s_br.currentDoc->rawHtml);
    c->baseUrl = pluto_strdup(s_br.currentDoc->baseUrl);
    c->mode    = s_br.browseMode;
    str_copy(c->key, sizeof(c->key), dkey);
    s_br.isRendering = 1;
    tasks_run(toggle_step, c, toggle_ctx_free, toggle_done, toggle_fail,
              c);
}

/* ── address-bar submit bridge ────────────────────────────────────────── */

static void ab_submit_bridge(const char* finalUrl, void* ud) {
    (void)ud;
    navigate_to(finalUrl);
}

static void open_address_bar_prefilled(const char* preset) {
    ab_open(preset, ab_submit_bridge, NULL);
    ab_launch_keyboard();
}

/* ── boot ─────────────────────────────────────────────────────────────── */

static void home_settings_cb(void) {
    PLUTO_LOG("[P32] HomePage.settingsCallback: opening settings");
    sp_open(pluto_state_name(PLUTO_STATE_HOME));
    s_br.state = PLUTO_STATE_SETTINGS;
}

static int s_appliedProtocol = -1; // hc backend pref applied from Settings

static void settings_changed_cb(void) {
    int mode = storage_settings()->mode;
    int protocol = storage_settings()->protocol;
    enum HcBackend pref = (protocol == PLUTO_PROTOCOL_TCP)
                              ? HC_BACKEND_TCP
                              : HC_BACKEND_HTTP;
    hc_set_backend_pref(pref);
    PLUTO_LOG("[P32] SettingsPage.onChangeCallback: mode=%d protocol=%d",
              mode, protocol);
    s_br.browseMode = mode;
    if (s_br.hasUrl) {
        /* Protocol switch needs a fresh download over the new backend; a
         * mode change just re-renders the cached doc from the raw HTML. */
        if (protocol != s_appliedProtocol) {
            s_appliedProtocol = protocol;
            navigate_to(s_br.curUrl.normalized);
            return;
        }
        if (s_br.currentDoc != NULL &&
            s_br.currentDoc->rawHtml != NULL &&
            s_br.currentDoc->rawHtml[0] != '\0')
            render_body(s_br.currentDoc->rawHtml,
                        s_br.curUrl.normalized, 0);
        else
            navigate_to(s_br.curUrl.normalized);
    }
}

void br_init(struct PlaydateAPI* pd) {
    memset(&s_br, 0, sizeof(s_br));
    s_pd = pd;
    s_br.mouseX = 200;
    s_br.mouseY = 120;
    s_br.mouseSpeed = 4.0f;
    if (pd != NULL && s_kb == NULL) {
        s_kb = keyboardApi.newKeyboard();
        if (s_kb != NULL) {
            keyboardApi.setRefreshRate(s_kb, 30.0f);
            if (s_updateTrampoline != NULL)
                keyboardApi.setPlaydateUpdateCallback(
                    s_kb, s_updateTrampoline, NULL);
        }
    }
}

void br_set_update_trampoline(int (*fn)(void*)) {
    s_updateTrampoline = fn;
    if (s_pd != NULL && s_kb != NULL && fn != NULL)
        keyboardApi.setPlaydateUpdateCallback(s_kb, fn, NULL);
}

void br_set_input_source(BrInputFn fn, void* ud) {
    s_inputFn = fn;
    s_inputUd = ud;
}

void br_set_clock_fn(BrClockFn fn) { s_clockFn = fn; }

static void default_input(BrInput* out, void* ud) {
    (void)ud;
    memset(out, 0, sizeof(*out));
    if (s_pd == NULL) return;
    PDButtons cur = 0, pushed = 0;
    s_pd->system->getButtonState(&cur, &pushed, NULL);
    if (cur & kButtonLeft)   out->held |= BR_BTN_LEFT;
    if (cur & kButtonRight)  out->held |= BR_BTN_RIGHT;
    if (cur & kButtonUp)     out->held |= BR_BTN_UP;
    if (cur & kButtonDown)   out->held |= BR_BTN_DOWN;
    if (cur & kButtonB)      out->held |= BR_BTN_B;
    if (cur & kButtonA)      out->held |= BR_BTN_A;
    if (pushed & kButtonLeft)  out->justPressed |= BR_BTN_LEFT;
    if (pushed & kButtonRight) out->justPressed |= BR_BTN_RIGHT;
    if (pushed & kButtonUp)    out->justPressed |= BR_BTN_UP;
    if (pushed & kButtonDown)  out->justPressed |= BR_BTN_DOWN;
    if (pushed & kButtonB)     out->justPressed |= BR_BTN_B;
    if (pushed & kButtonA)     out->justPressed |= BR_BTN_A;
    out->crankChange = s_pd->system->getCrankChange();
    out->nowMs = br_now_ms();
}

void br_boot(void) {
    cj_prune();
    s_br.state       = PLUTO_STATE_HOME;
    s_br.browseMode  = storage_settings()->mode;
    str_copy(s_br.pageTitle, sizeof(s_br.pageTitle), "CometBrowser");
    s_appliedProtocol = storage_settings()->protocol;
    hc_set_backend_pref(s_appliedProtocol == PLUTO_PROTOCOL_TCP
                            ? HC_BACKEND_TCP
                            : HC_BACKEND_HTTP);
    hp_set_settings_callback(home_settings_cb);
    sp_set_on_change(settings_changed_cb);
    menu_rebuild();
    br_system_menu_refresh();
    /* Auto-launch google.com on boot for device crash testing. */
    br_navigate_to("https://google.com");
}

/* ── loading screen (mirrors main.lua STATE_LOADING branch) ───────────── */

static void draw_loading_screen(void) {
    char buf[256];

    PlutoFont* fontH = style_get_heading_font(1, NULL, NULL);
    PlutoFont* fontB = style_get_body_font(0, 0, NULL);

    s_pd->graphics->fillRect(0, PLUTO_CONTENT_Y, PLUTO_SCREEN_WIDTH,
                             PLUTO_CONTENT_HEIGHT, kColorWhite);

    s_pd->graphics->setFont((LCDFont*)fontH);
    const char* headline = s_br.isRendering ? "Rendering Web Page..."
                                            : "Loading Web Page...";
    s_pd->graphics->drawText(headline, strlen(headline), kUTF8Encoding,
                             24, 60);
    s_pd->graphics->setFont((LCDFont*)fontB);

    const char* host = s_br.hasUrl ? s_br.curUrl.normalized : "Web Request";
    char displayHost[48];
    str_copy(displayHost, sizeof(displayHost), host);
    if (strlen(displayHost) > 45) {
        displayHost[42] = '\0';
        strcat(displayHost, "...");
    }
    s_pd->graphics->drawText(displayHost, strlen(displayHost),
                             kUTF8Encoding, 24, 90);

    if (s_br.isRendering) {
        double p = tasks_get_progress();
        int pct = (int)(fmin(1.0, p) * 100.0);
        snprintf(buf, sizeof(buf), "Rendering page content... %d%%", pct);
        s_pd->graphics->drawText(buf, strlen(buf), kUTF8Encoding, 24, 116);
        s_pd->graphics->drawRect(24, 138, 352, 10, kColorBlack);
        s_pd->graphics->fillRect(24, 138,
                                 (int)(352.0 * fmin(1.0, p)), 10,
                                 kColorBlack);
    } else if (s_br.progressTot > 0) {
        int pct = (int)((double)s_br.progressCur /
                        (double)s_br.progressTot * 100.0);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        snprintf(buf, sizeof(buf),
                 "Received: %ld / %ld bytes (%d%%)",
                 s_br.progressCur, s_br.progressTot, pct);
        s_pd->graphics->drawText(buf, strlen(buf), kUTF8Encoding, 24, 116);
        s_pd->graphics->drawRect(24, 138, 352, 10, kColorBlack);
        s_pd->graphics->fillRect(24, 138, 352 * pct / 100, 10,
                                 kColorBlack);
    } else {
        snprintf(buf, sizeof(buf),
                 "Downloading on device... (%ld bytes)",
                 s_br.progressCur);
        s_pd->graphics->drawText(buf, strlen(buf), kUTF8Encoding, 24, 116);
    }

    const char* hint = "(B) Cancel  *  (Left) Back";
    s_pd->graphics->drawText(hint, strlen(hint), kUTF8Encoding, 24, 165);
}

/* ── main frame ───────────────────────────────────────────────────────── */

int br_state(void)                   { return s_br.state; }
const char* br_page_title(void)      { return s_br.pageTitle; }
const char* br_current_normalized(void) {
    return s_br.hasUrl ? s_br.curUrl.normalized : "";
}
int   br_browse_mode(void)           { return s_br.browseMode; }
double br_scroll_y(void)             { return s_br.scrollY; }
double br_target_scroll_y(void)      { return s_br.targetScrollY; }
float  br_crank_velocity(void)       { return s_br.crankVelocity; }
int    br_is_rendering(void)         { return s_br.isRendering; }
const struct DocDocument* br_current_doc(void) { return s_br.currentDoc; }

int br_progress(int* cur, int* tot) {
    if (cur) *cur = (int)s_br.progressCur;
    if (tot) *tot = (int)s_br.progressTot;
    return s_br.progressTot > 0;
}

int br_history_count(void) { return s_br.nHistory; }
int br_history_index(void) { return s_br.hIdx; }
const char* br_history_at(int i) {
    if (i < 0 || i >= s_br.nHistory) return NULL;
    return s_br.navHistory[i];
}

void br_frame(void) {
    if (s_pd == NULL) return;
    s_pd->graphics->clear(kColorWhite);

    BrInput in;
    if (s_inputFn != NULL) s_inputFn(&in, s_inputUd);
    else default_input(&in, NULL);

    /* detect system-menu close (>500ms frame gap): close overlays */
    if (s_br.haveLastFrame &&
        (in.nowMs - s_br.lastFrameMs) > 500) {
        if (s_br.keyboardOpen) {
            if (s_pd != NULL && s_kb != NULL) keyboardApi.hide(s_kb);
            s_br.keyboardOpen = 0;
        }
        if (ab_is_open()) ab_cancel();
        s_br.skipInputFrames = 2;
    }
    s_br.lastFrameMs = in.nowMs;
    s_br.haveLastFrame = 1;

    layout_clear_on_demand_consumed();

    /* live-sync keyboard text into the focused input field */
    if (s_br.keyboardOpen && s_br.activeInputField != NULL)
        item_set_value(s_br.activeInputField, kb_text_snapshot());

    /* staged navigation consumes one slot per frame */
    if (s_br.hasPendingNav) {
        static char dest[PLUTO_URL_NORMALIZED_MAX];
        str_copy(dest, sizeof(dest), s_br.pendingNav);
        s_br.hasPendingNav = 0;
        s_br.pendingNav[0] = '\0';
        run_navigation(dest);
    }

    /* meta refresh redirect (only while still viewing the page) */
    if (s_br.hasMetaRefresh && s_br.state == PLUTO_STATE_PAGE &&
        (double)in.nowMs >= s_br.metaDeadlineMs) {
        s_br.hasMetaRefresh = 0;
        navigate_to(s_br.metaUrl);
    }

    float crankChange = in.crankChange;

    /* keyboards own the crank */
    int keyboardActive = s_br.keyboardOpen || ab_is_open() ||
                         s_br.menuOpen;
    if (keyboardActive) {
        crankChange = 0;
        s_br.crankVelocity = 0;
    } else if (crankChange != 0) {
        if (storage_settings()->invertCrank)
            s_br.crankVelocity -= (double)crankChange * 1.6;
        else
            s_br.crankVelocity += (double)crankChange * 1.6;
    }
    s_br.crankVelocity *= 0.85;
    if (fabs(s_br.crankVelocity) < 0.05) s_br.crankVelocity = 0;

    if (crankChange != 0) lm_clear_selection();

    hc_update();
    id_update();
    tasks_update();

    /* pause-menu overlay swallows everything below */
    if (s_br.menuOpen) {
        menu_handle_input(&in);
        goto compose;
    }

    /* B hold machine only while browsing */
    if (s_br.state != PLUTO_STATE_HOME &&
        s_br.state != PLUTO_STATE_PAGE)
        s_br.bHoldActive = 0;

    if (s_br.state == PLUTO_STATE_HOME ||
        s_br.state == PLUTO_STATE_PAGE) {
        int bDown = (in.held & BR_BTN_B) != 0;

        if (bDown) s_br.bNotPressedFrames = 0;
        else       s_br.bNotPressedFrames++;

        if ((in.justPressed & BR_BTN_B) && !ab_is_open() &&
            !s_br.keyboardOpen && !s_br.bHoldActive) {
            s_br.bHoldActive = 1;
            s_br.bHoldUsedDir = 0;
            s_br.bHoldStartMs = in.nowMs;
            s_br.bNotPressedFrames = 0;
        }
        if (s_br.bHoldActive && bDown &&
            !ab_is_open() && !s_br.keyboardOpen) {
            if (in.held & BR_BTN_LEFT) {
                char prev[PLUTO_URL_NORMALIZED_MAX];
                if (!s_br.bHoldUsedDir) {
                    if (!go_back_url(prev, sizeof(prev)))
                        str_copy(prev, sizeof(prev), "about:home");
                    navigate_to(prev);
                }
                s_br.bHoldUsedDir = 1;
            } else if (in.held & BR_BTN_RIGHT) {
                char fwd[PLUTO_URL_NORMALIZED_MAX];
                if (!s_br.bHoldUsedDir) {
                    if (go_forward_url(fwd, sizeof(fwd)))
                        navigate_to(fwd);
                }
                s_br.bHoldUsedDir = 1;
            }
        }
        if (s_br.bHoldActive && s_br.bNotPressedFrames >= 4) {
            if (!s_br.bHoldUsedDir && !ab_is_open() &&
                !s_br.keyboardOpen) {
                open_address_bar_prefilled(
                    (s_br.state == PLUTO_STATE_PAGE && s_br.hasUrl)
                        ? s_br.curUrl.normalized : "");
            }
            s_br.bHoldActive = 0;
            s_br.bNotPressedFrames = 0;
        }
    } else {
        s_br.bNotPressedFrames = 0;
        s_br.bHoldActive = 0;
    }

    if (s_br.skipInputFrames > 0) s_br.skipInputFrames--;

    switch (s_br.state) {

    case PLUTO_STATE_HOME: {
        if (s_br.skipInputFrames <= 0 && !ab_is_open() &&
            !s_br.keyboardOpen) {
            char selUrl[PLUTO_URL_NORMALIZED_MAX];
            HpAction act = HP_ACT_NONE;
            if (in.justPressed & BR_BTN_UP)
                act = hp_handle_input(HP_BTN_UP, selUrl, sizeof(selUrl));
            else if (in.justPressed & BR_BTN_DOWN)
                act = hp_handle_input(HP_BTN_DOWN, selUrl,
                                      sizeof(selUrl));
            else if (in.justPressed & BR_BTN_LEFT)
                act = hp_handle_input(HP_BTN_LEFT, selUrl,
                                      sizeof(selUrl));
            else if (in.justPressed & BR_BTN_RIGHT)
                act = hp_handle_input(HP_BTN_RIGHT, selUrl,
                                      sizeof(selUrl));
            else if (in.justPressed & BR_BTN_A)
                act = hp_handle_input(HP_BTN_A, selUrl, sizeof(selUrl));

            if (act == HP_ACT_OPEN_URL) navigate_to(selUrl);
            else if (act == HP_ACT_SETTINGS) home_settings_cb();
        }
        hp_draw(crankChange);
        break;
    }

    case PLUTO_STATE_PAGE: {
        double totalH = layout_total_height();
        double maxScroll = totalH - (double)PLUTO_CONTENT_HEIGHT;
        if (maxScroll < 0) maxScroll = 0;
        int isHtmlMode = (s_br.browseMode == PLUTO_MODE_RAW_HTML);

        if (s_br.skipInputFrames <= 0 && !s_br.keyboardOpen &&
            !ab_is_open()) {

            if (layout_has_on_demand_overlay()) {
                const char* ovHref = layout_od_href();
                PlutoOdAction act =
                    layout_handle_on_demand_input(
                        (in.justPressed & BR_BTN_A ? PLUTO_KBUTTON_A : 0) |
                        (in.justPressed & BR_BTN_B ? PLUTO_KBUTTON_B : 0));
                if (act == OD_LINK && ovHref != NULL)
                    navigate_to(ovHref);
            } else {
                target_scroll_step(&in, isHtmlMode, crankChange);
            }

            /* A alone: follow focused link / activate control (both
             * modes; outside the A-held branch so the press frame
             * still sees buttonIsPressed(A) true) */
            if ((in.justPressed & BR_BTN_A) &&
                !(in.held & BR_BTN_LEFT) && !(in.held & BR_BTN_RIGHT) &&
                !layout_on_demand_consumed()) {
                page_a_click(&in, isHtmlMode);
            }

            if (s_br.targetScrollY < 0) s_br.targetScrollY = 0;
            if (s_br.targetScrollY > maxScroll)
                s_br.targetScrollY = maxScroll;
            s_br.scrollY +=
                (s_br.targetScrollY - s_br.scrollY) * 0.4;
        }

        /* hover image management (IMAGE_MODE_HOVER) */
        if ((storage_settings()->imageMode) == PLUTO_IMAGE_MODE_HOVER) {
            const char* currentHoverSrc = NULL;
            LmLink* lk = isHtmlMode
                ? lm_get_hovered_link(s_br.mouseX,
                                      (int)(s_br.mouseY + s_br.scrollY))
                : lm_get_selected_link();
            if (lk != NULL && lk->primaryRect.isImage)
                currentHoverSrc = lk->primaryRect.src;

            if (s_hoveredImageSrc != NULL &&
                (currentHoverSrc == NULL ||
                 strcmp(s_hoveredImageSrc, currentHoverSrc) != 0))
                id_evict(s_hoveredImageSrc);
            if (currentHoverSrc != NULL) id_enqueue(currentHoverSrc);

            pluto_free(s_hoveredImageSrc);
            s_hoveredImageSrc = currentHoverSrc
                ? pluto_strdup(currentHoverSrc) : NULL;
        }

        layout_draw(s_br.scrollY);
        layout_evict_offscreen(s_br.scrollY);

        LmLink* sel = lm_get_selected_link();
        hud_draw(s_br.scrollY, totalH, sel ? sel->href : NULL);
        break;
    }

    case PLUTO_STATE_LOADING: {
        draw_loading_screen();
        if (in.justPressed & BR_BTN_B) {
            hc_cancel();
            navigate_to("about:home");
        }
        if (in.justPressed & BR_BTN_LEFT) {
            hc_cancel();
            char prev[PLUTO_URL_NORMALIZED_MAX];
            if (!go_back_url(prev, sizeof(prev)))
                str_copy(prev, sizeof(prev), "about:home");
            navigate_to(prev);
        }
        break;
    }

    case PLUTO_STATE_ERROR: {
        if (!ab_is_open()) {
            EpAction act = EP_ACT_NONE;
            if (in.justPressed & (BR_BTN_LEFT | BR_BTN_UP))
                act = ep_handle_input(EP_BTN_LEFT);
            else if (in.justPressed & (BR_BTN_RIGHT | BR_BTN_DOWN))
                act = ep_handle_input(EP_BTN_RIGHT);
            else if (in.justPressed & BR_BTN_A)
                act = ep_handle_input(EP_BTN_A);

            if (act == EP_ACT_RETRY && s_br.hasUrl)
                navigate_to(s_br.curUrl.normalized);
            else if (act == EP_ACT_SEARCH)
                open_address_bar_prefilled("");
            else if (act == EP_ACT_HOME)
                navigate_to("about:home");

            if (in.justPressed & BR_BTN_LEFT) {
                char prev[PLUTO_URL_NORMALIZED_MAX];
                if (!go_back_url(prev, sizeof(prev)))
                    str_copy(prev, sizeof(prev), "about:home");
                navigate_to(prev);
            }
        }
        ep_draw();
        break;
    }

    case PLUTO_STATE_BOOKMARKS: {
        char outUrl[PLUTO_URL_NORMALIZED_MAX];
        LpAction act = LP_ACT_NONE;
        if (in.justPressed & BR_BTN_UP)
            act = bm_handle_input(LR_BTN_UP, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_DOWN)
            act = bm_handle_input(LR_BTN_DOWN, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_A)
            act = bm_handle_input(LR_BTN_A, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_B)
            act = bm_handle_input(LR_BTN_B, outUrl, sizeof(outUrl));
        if (act == LP_ACT_CLOSE) navigate_to("about:home");
        else if (act == LP_ACT_OPEN) navigate_to(outUrl);
        bm_draw(crankChange);
        break;
    }

    case PLUTO_STATE_HISTORY: {
        char outUrl[PLUTO_URL_NORMALIZED_MAX];
        LpAction act = LP_ACT_NONE;
        if (in.justPressed & BR_BTN_UP)
            act = hi_handle_input(LR_BTN_UP, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_DOWN)
            act = hi_handle_input(LR_BTN_DOWN, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_A)
            act = hi_handle_input(LR_BTN_A, outUrl, sizeof(outUrl));
        else if (in.justPressed & BR_BTN_B)
            act = hi_handle_input(LR_BTN_B, outUrl, sizeof(outUrl));
        if (act == LP_ACT_CLOSE) navigate_to("about:home");
        else if (act == LP_ACT_OPEN) navigate_to(outUrl);
        hi_draw(crankChange);
        break;
    }

    case PLUTO_STATE_SETTINGS: {
        /* draw whatever was behind the overlay */
        if (s_br.currentDoc != NULL) {
            layout_draw(s_br.scrollY);
            if (s_br.browseMode == PLUTO_MODE_RAW_HTML)
                hud_draw(s_br.scrollY, layout_total_height(),
                         lm_get_selected_link()
                             ? lm_get_selected_link()->href : NULL);
        }
        SpAction act = SP_ACT_NONE;
        if (in.justPressed & BR_BTN_UP)    act = sp_handle_input(SP_BTN_UP);
        else if (in.justPressed & BR_BTN_DOWN)
            act = sp_handle_input(SP_BTN_DOWN);
        else if (in.justPressed & BR_BTN_LEFT)
            act = sp_handle_input(SP_BTN_LEFT);
        else if (in.justPressed & BR_BTN_RIGHT)
            act = sp_handle_input(SP_BTN_RIGHT);
        else if (in.justPressed & BR_BTN_A)
            act = sp_handle_input(SP_BTN_A);
        else if (in.justPressed & BR_BTN_B)
            act = sp_handle_input(SP_BTN_B);

        if (act == SP_ACT_CLOSED || act == SP_ACT_SAVED) {
            const char* prev = sp_previous_state();
            if (prev != NULL && !strcmp(prev, "home"))
                navigate_to("about:home");
            else if (prev != NULL) {
                for (int st = 0; st <= 6; st++) {
                    const char* nm = pluto_state_name((PlutoState)st);
                    if (!strcmp(nm, prev)) {
                        s_br.state = st;
                        break;
                    }
                }
            }
        }
        sp_draw();
        break;
    }

    default:
        break;
    }

compose:
    /* top bar + floating overlays, in the source composition order */
    s_pd->graphics->clearClipRect();
    ch_draw(s_br.hasUrl ? &s_br.curUrl : NULL, s_br.pageTitle,
            s_br.state == PLUTO_STATE_LOADING,
            s_br.progressCur, s_br.progressTot,
            s_br.browseMode == PLUTO_MODE_READER,
            hc_backend_label());
    ab_draw_overlay();

    /* HTML-mode hover status bar (cursor itself arrives in P33) */
    if (s_br.state == PLUTO_STATE_PAGE &&
        s_br.browseMode == PLUTO_MODE_RAW_HTML) {
        LmLink* hov = lm_get_hovered_link(
            s_br.mouseX, (int)(s_br.mouseY + s_br.scrollY));
        if (hov != NULL && hov->href != NULL)
            hud_draw_hover_status(hov->href);
    }

    if (s_br.menuOpen) menu_draw();

    /* HTML-mode virtual cursor: white triangle + black outline, drawn as
     * the very last thing so nothing can draw over it (Lua 1008-1021) */
    if (s_br.state == PLUTO_STATE_PAGE &&
        s_br.browseMode == PLUTO_MODE_RAW_HTML &&
        !s_br.keyboardOpen && !ab_is_open()) {
        s_pd->graphics->pushContext(NULL);
        s_pd->graphics->clearClipRect();
        int mx = s_br.mouseX, my = s_br.mouseY;
        s_pd->graphics->fillTriangle(mx, my, mx + 10, my + 4,
                                     mx + 4, my + 10, kColorWhite);
        s_pd->graphics->drawLine(mx, my, mx + 10, my + 4, 1, kColorBlack);
        s_pd->graphics->drawLine(mx + 10, my + 4, mx + 4, my + 10,
                                 1, kColorBlack);
        s_pd->graphics->drawLine(mx + 4, my + 10, mx, my, 1, kColorBlack);
        s_pd->graphics->drawLine(mx, my, mx + 4, my + 10, 1, kColorBlack);
        s_pd->graphics->popContext();
    }
}

/* reader/html scroll step extracted for clarity (Lua 694-809) */
static void target_scroll_step(const BrInput* in, int isHtmlMode,
                               float crankChange) {
    /* A + Left/Right history jumps (both modes) */
    if (in->held & BR_BTN_A) {
        char u[PLUTO_URL_NORMALIZED_MAX];
        if (in->justPressed & BR_BTN_LEFT) {
            if (!go_back_url(u, sizeof(u)))
                str_copy(u, sizeof(u), "about:home");
            navigate_to(u);
        } else if (in->justPressed & BR_BTN_RIGHT) {
            if (go_forward_url(u, sizeof(u))) navigate_to(u);
        }
        return;
    }

    if (!isHtmlMode) {
        /* READER: crank velocity drives scroll; D-pad walks links */
        s_br.targetScrollY += s_br.crankVelocity;
        if (in->justPressed & BR_BTN_DOWN) {
            LmLink* nx = lm_select_next((int)s_br.scrollY);
            if (nx != NULL) {
                int linkY = nx->primaryRect.y;
                if (linkY > s_br.targetScrollY + PLUTO_CONTENT_HEIGHT - 50)
                    s_br.targetScrollY =
                        linkY - PLUTO_CONTENT_HEIGHT + 70;
                else if (linkY < s_br.targetScrollY + PLUTO_CONTENT_Y)
                    s_br.targetScrollY = linkY - PLUTO_CONTENT_Y - 20;
            } else {
                s_br.targetScrollY += 40;
            }
        } else if (in->justPressed & BR_BTN_UP) {
            LmLink* pv = lm_select_prev((int)s_br.scrollY);
            if (pv != NULL) {
                int linkY = pv->primaryRect.y;
                if (linkY < s_br.targetScrollY + PLUTO_CONTENT_Y)
                    s_br.targetScrollY = linkY - PLUTO_CONTENT_Y - 20;
                else if (linkY >
                         s_br.targetScrollY + PLUTO_CONTENT_HEIGHT - 50)
                    s_br.targetScrollY =
                        linkY - PLUTO_CONTENT_HEIGHT + 70;
            } else {
                s_br.targetScrollY -= 40;
            }
        }
        return;
    }

    /* ── HTML MODE: virtual mouse cursor (Lua main.lua 768-808) ── */
    if (s_br.bHoldActive) return;      /* B-hold suppresses movement */
    int dpadHeld = 0;
    if (in->held & BR_BTN_LEFT) {
        s_br.mouseX -= 4;
        if (s_br.mouseX < 2) s_br.mouseX = 2;
        dpadHeld = 1;
    }
    if (in->held & BR_BTN_RIGHT) {
        s_br.mouseX += 4;
        if (s_br.mouseX > PLUTO_SCREEN_WIDTH - 2)
            s_br.mouseX = PLUTO_SCREEN_WIDTH - 2;
        dpadHeld = 1;
    }
    if (in->held & BR_BTN_UP) {
        s_br.mouseY -= 4;
        if (s_br.mouseY < PLUTO_CONTENT_Y + 2)
            s_br.mouseY = PLUTO_CONTENT_Y + 2;
        dpadHeld = 1;
    }
    if (in->held & BR_BTN_DOWN) {
        s_br.mouseY += 4;
        if (s_br.mouseY > PLUTO_SCREEN_HEIGHT - 2)
            s_br.mouseY = PLUTO_SCREEN_HEIGHT - 2;
        dpadHeld = 1;
    }

    if (dpadHeld) {
        if (crankChange != 0.0f) {
            s_br.mouseY += (int)(crankChange * 0.5f);
            if (s_br.mouseY < PLUTO_CONTENT_Y + 2)
                s_br.mouseY = PLUTO_CONTENT_Y + 2;
            if (s_br.mouseY > PLUTO_SCREEN_HEIGHT - 2)
                s_br.mouseY = PLUTO_SCREEN_HEIGHT - 2;
        }
        /* edge zones nudge scroll toward the cursor (Lua 796-803) */
        const int SCROLL_ZONE = 20;
        if (s_br.mouseY <= PLUTO_CONTENT_Y + SCROLL_ZONE) {
            double strength =
                (double)(SCROLL_ZONE -
                         (s_br.mouseY - PLUTO_CONTENT_Y)) / SCROLL_ZONE;
            s_br.targetScrollY -= 3.0 * (1.0 + strength * 3.0);
        } else if (s_br.mouseY >= PLUTO_SCREEN_HEIGHT - SCROLL_ZONE) {
            double strength =
                (double)(SCROLL_ZONE -
                         (PLUTO_SCREEN_HEIGHT - s_br.mouseY)) / SCROLL_ZONE;
            s_br.targetScrollY += 3.0 * (1.0 + strength * 3.0);
        }
    } else {
        s_br.targetScrollY += s_br.crankVelocity;
    }
}

/* A-click resolution shared by both modes (Lua 734-752 / 811-834) */
static void page_a_click(const BrInput* in, int isHtmlMode) {
    (void)in;
    LmLink* link = isHtmlMode
        ? lm_get_hovered_link(s_br.mouseX,
                              (int)(s_br.mouseY + s_br.scrollY))
        : lm_get_selected_link();
    if (link == NULL) return;
    const LmRect* primary = &link->primaryRect;

    if (primary->isToggle && primary->toggleKey != NULL) {
        toggle_details(primary->toggleKey);
    } else if (primary->isFormInput && primary->inputBlock != NULL) {
        activate_form_block((struct LItem*)primary->inputBlock);
    } else if (link->href != NULL && !primary->inert) {
        int imgMode = storage_settings()->imageMode;
        if (imgMode == PLUTO_IMAGE_MODE_ONDEMAND && primary->isImage) {
            layout_show_on_demand_overlay(primary->src, link->href,
                                          primary->alt);
        } else {
            navigate_to(link->href);
        }
    }
}
