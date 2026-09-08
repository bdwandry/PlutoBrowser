/*
 * PlutoBrowser — main.c
 * Native C reimplementation of CometBrowser (Lua reference).
 *
 * Phase 0: eventHandler + update callback scaffold.
 * - Boots, initializes the file logger (pluto.log), clears screen, logs boot.
 * - The full state machine arrives in later phases.
 */
#include <stdio.h>
#include "pd_api.h"

#ifdef TARGET_SIMULATOR
#include <stdlib.h>
#endif

#include "core/logger.h"
#include "core/encoding.h"
#include "core/cookie_jar.h"
#include "core/http_client.h"
#include "core/storage.h"
#include "core/constants.h"
#include "render/style.h"
#include "render/link_manager.h"
#include "render/layout.h"
#include "render/image_decoder.h"
#include "render/cloud_layout.h"
#include "util/json.h"
#include "render/decoders/scale.h"
#include "render/decoders/dither.h"
#include "render/decoders/inflate.h"
#include "render/decoders/png.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/svg.h"
#include "render/decoders/webp.h"
#include "render/decoders/webp-internal.h"
#include "render/decoders/webp_vp8_data.h"
#include "render/decoders/bmp.h"
#include "render/decoders/gif.h"
#include "render/decoders/ico.h"
#include "ui/chrome.h"
#include "ui/hud.h"
#include "ui/error_page.h"
#include "ui/home_page.h"
#include "ui/bookmarks_page.h"
#include "ui/history_page.h"
#include "ui/settings_page.h"
#include "ui/address_bar.h"
#include "html/entities.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "html/document.h"
#include "html/readability.h"
#include "keyboard/keyboard.h"
#include "core/constants.h"
#include "core/url.h"
#include "core/tasks.h"
#include "util/pdtimer.h"
#include "util/strbuf.h"
#include "util/strutil.h"

static PlaydateAPI *pd = NULL;

/* The Raphcal keyboard port reads this global (declared in keyboard.h). */
PlaydateAPI *playdate = NULL;

/* Accessor used by util/ modules for the SDK allocator (system->realloc). */
PlaydateAPI *pluto_pd(void);
PlaydateAPI *pluto_pd(void)
{
    return pd;
}

/* Allocator mirror for buffers returned by util/ helpers. */
void pluto_free(void *p);
void pluto_free(void *p)
{
    if (pd)
    {
        pd->system->realloc(p, 0);
    }
}

void *pluto_realloc(void *p, size_t n);

/* Shared strdup on the same allocator (nav history, details keys). */
static char *pluto_strdup(const char *s)
{
    if (!s)
    {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = (char *)pluto_realloc(NULL, n);
    if (d)
    {
        memcpy(d, s, n);
    }
    return d;
}

/* Shared realloc mirror (pluto_free's counterpart). */
void *pluto_realloc(void *p, size_t n)
{
    if (!pd)
    {
        return NULL;
    }
    return pd->system->realloc(p, n);
}

/* Forward declaration (implementation below). */
static int updateFrame(void *userdata);





/* Shared keyboard instance (address bar + page form inputs use it). */
static PDKeyboard *g_kb = NULL;

/* Accessor for modules that need the shared keyboard instance (address bar). */
PDKeyboard *app_keyboard(void)
{
    return g_kb;
}

/* ── Browser state machine (Phase 12; port of main.lua control flow) ─────── */
static BrowserState currentState = STATE_HOME;
static BrowserState g_lastLoggedState = STATE_HOME;
static BrowserState settingsPrevState = STATE_HOME;
static UrlParsed *currentUrlObj = NULL;   /* heap; NULL = about:home */
static int currentBrowseMode = 1;         /* MODE_RAW_HTML default */
static int bHoldActive = 0;
static int bHoldUsedDir = 0;
static int bNotPressedFrames = 0;
static unsigned int bHoldStartMs = 0;
static float crankVelocity = 0;
static int skipInputFrames = 0;
static unsigned int g_lastFrameAtMs = 0; /* system-menu-close detection (Lua lastFrameAtMs) */

/* ── P-INT: live page/app state (port of main.lua locals) ────────────────── */
static DocParseResult *currentDoc = NULL;     /* owned; layout borrows from it */
static char pageTitle[256] = "CometBrowser Start Page";
static int scrollY = 0;
static int targetScrollY = 0;
static int progressCurrent = 0;
static int progressTotal = 0;
static int isRendering = 0;
static int pendingNavUrlSet = 0;              /* Lua pendingNavUrl */
static char pendingNavUrl[640] = "";
static int navigatingHistory = 0;             /* Lua navigatingHistory */
static int mouseX = 200, mouseY = 100;        /* HTML-mode virtual cursor */
#define MOUSE_SPEED 4                          /* Lua mouseSpeed */
static const LayoutItem *formInputItem = NULL;/* Lua activeInputField */
static int formKeyboardOpen = 0;              /* Lua keyboardOpen */

/* detailsOpenSet (Lua map keyed "d1".."dN"): C stores the doc's d-key list in
 * parse order + an open/closed override array; document_parse takes it as a
 * positional detailsOpen[] (index i-1 for key d<i>). */
static char *detailsKeys[64];
static int detailsOpenState[64];
static int detailsKeyCount = 0;

/* Navigation history stack (port of navHistory/historyIndex, MAX_HISTORY=30). */
#define MAX_HISTORY 30
static char *navHistory[MAX_HISTORY];
static int navHistoryCount = 0;
static int historyIndex = 0; /* 1-based (Lua parity); 0 = empty */

static void push_history(const char *urlString)
{
    if (!urlString || !urlString[0])
    {
        return;
    }
    if (historyIndex < navHistoryCount)
    {
        /* Navigating somewhere new clears the forward entries. */
        for (int i = historyIndex; i < navHistoryCount; i++)
        {
            pluto_free(navHistory[i]);
        }
        navHistoryCount = historyIndex;
    }
    if (navHistoryCount > 0 &&
        strcmp(navHistory[navHistoryCount - 1], urlString) == 0)
    {
        return; /* Lua: top-of-stack duplicate is dropped */
    }
    if (navHistoryCount < MAX_HISTORY)
    {
        navHistory[navHistoryCount++] = pluto_strdup(urlString);
        historyIndex = navHistoryCount;
    }
    else
    {
        /* Lua: table.remove(navHistory, 1) then insert; index shifts down. */
        pluto_free(navHistory[0]);
        memmove(&navHistory[0], &navHistory[1], sizeof(char *) * (MAX_HISTORY - 1));
        navHistory[MAX_HISTORY - 1] = pluto_strdup(urlString);
        historyIndex = MAX_HISTORY;
    }
}

static const char *go_back(void)
{
    if (historyIndex > 1)
    {
        historyIndex--;
        navigatingHistory = 1;
        return navHistory[historyIndex - 1];
    }
    return NULL;
}

static const char *go_forward(void)
{
    if (historyIndex < navHistoryCount)
    {
        historyIndex++;
        navigatingHistory = 1;
        return navHistory[historyIndex - 1];
    }
    return NULL;
}

/* detailsOpenSet key bookkeeping: find or add "d<N>", return index. */
static int details_key_index(const char *dkey)
{
    if (!dkey)
    {
        return -1;
    }
    for (int i = 0; i < detailsKeyCount; i++)
    {
        if (strcmp(detailsKeys[i], dkey) == 0)
        {
            return i;
        }
    }
    if (detailsKeyCount < 64)
    {
        detailsKeys[detailsKeyCount] = pluto_strdup(dkey);
        detailsOpenState[detailsKeyCount] = 0;
        return detailsKeyCount++;
    }
    return -1;
}

static void details_keys_clear(void)
{
    for (int i = 0; i < detailsKeyCount; i++)
    {
        pluto_free(detailsKeys[i]);
    }
    detailsKeyCount = 0;
}

/* Diagnostic counters exposed in logs. */
static unsigned int frameCount = 0;

/* navigate_to: full port of main.lua runNavigation/executeNavigation. */
static void navigate_to(const char *urlString);
static void render_body(const char *body, const char *url, int addToHistory);
static void toggle_details(const char *dkey);
static void activate_form_block(const LayoutItem *item);
static void submit_form(const char *formAction, const LayoutItem *inputBlock);
static void open_keyboard_for_input(const LayoutItem *item);
static void form_kb_did_hide(void *ud);
static void form_kb_will_hide(int okButtonPressed, void *ud);
static void form_kb_text_changed(void *ud);
static void form_set_block_value(const LayoutItem *item, const char *text);
static int app_svg_decoder(const char *xml, int w, int h, void **outBitmap);
static void http_on_progress(int cur, int total);
static void http_on_success(int status, char **headerKeys, char **headerVals,
                            int headerCount, const char *body, size_t bodyLen,
                            const char *url);
static void http_on_error(const char *message);
static void update_system_menu(void);
static void settings_on_change(void);
static void view_menu_callback(void *ud);
static void meta_refresh_cb(void *ud);

static void go_home(void)
{
    pendingNavUrlSet = 1;
    snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
}

static void menu_home_page(void *ud)
{
    (void)ud;
    go_home();
}

/* Address-bar submit target (Lua: function(newUrl) navigateTo(newUrl) end). */
static void ab_submit_goto(const char *finalUrl, void *ud)
{
    (void)ud;
    navigate_to(finalUrl);
}

static void menu_settings(void *ud)
{
    (void)ud;
    settings_page_open((int)currentState);
    currentState = STATE_SETTINGS;
    logger_log("main: menu Settings (state=%d)", (int)currentState);
}

/* Home-page settings button: same behavior, signature for the page API. */
static void home_settings_opened(void)
{
    settings_page_open((int)currentState);
    currentState = STATE_SETTINGS;
    logger_log("main: home Settings opened (state=%d)", (int)currentState);
}

static void menu_history(void *ud)
{
    (void)ud;
    history_page_open();
    currentState = STATE_HISTORY;
}

static void menu_clear_cookies(void *ud)
{
    (void)ud;
    cookie_jar_clear();
    logger_log("main: menu Clear Cookies");
}

static void settings_cleared_cookies(void)
{
    cookie_jar_clear();
}

/* Reader/HTML "View" options item (Lua: addOptionsMenuItem("View", ...)). */
static PDMenuItem *g_viewMenuItem = NULL;
static const char *k_viewOptions[2] = { "Reader", "HTML" };

static void view_menu_callback(void *ud)
{
    (void)ud;
    if (!g_viewMenuItem || !pd)
    {
        return;
    }
    int v = pd->system->getMenuItemValue(g_viewMenuItem);
    int newMode = (v == 0) ? MODE_READER : MODE_RAW_HTML;
    if (newMode != currentBrowseMode)
    {
        currentBrowseMode = newMode;
        storage_set_setting_int("mode", currentBrowseMode);
        storage_save();
        if (currentDoc && currentDoc->rawHtml && currentDoc->rawHtml[0] &&
            currentUrlObj)
        {
            render_body(currentDoc->rawHtml, currentUrlObj->normalized, 0);
        }
        else if (currentUrlObj)
        {
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                     currentUrlObj->normalized);
        }
    }
}

static void update_system_menu(void)
{
    pd->system->removeAllMenuItems();
    g_viewMenuItem = NULL;
    pd->system->addMenuItem("Home-Page", menu_home_page, NULL);
    if (currentState == STATE_PAGE && currentUrlObj &&
        strcmp(currentUrlObj->scheme, "about") != 0)
    {
        int initial = (currentBrowseMode == MODE_READER) ? 0 : 1;
        g_viewMenuItem = pd->system->addOptionsMenuItem(
            "View", k_viewOptions, 2, view_menu_callback, NULL);
        pd->system->setMenuItemValue(g_viewMenuItem, initial);
    }
    pd->system->addMenuItem("Settings", menu_settings, NULL);
    pd->system->addMenuItem("History", menu_history, NULL);
    pd->system->addMenuItem("Clear Cookies", menu_clear_cookies, NULL);
}

/* ── renderBody (port): parse + Layout.build as a cooperative task ───────── */
static void render_body(const char *body, const char *url, int addToHistory);

/* renderBody task state (task-private continuation). */
typedef struct RenderTask
{
    char *body;        /* malloc'd copy of the html to parse */
    char url[512];
    int addToHistory;
    int isToggle;      /* details-toggle re-render: restore scroll, no history */
    int prevScroll;
    int prevTarget;
    DocParseResult *doc; /* set by the parse step, consumed by done */
} RenderTask;

/* Static error buffer: tasks fire onError(data) with the task's data pointer
 * (see tasks.c), so the real message travels through this side channel. */
static char g_renderErrMsg[192];

static int render_step(TaskCtx *ctx)
{
    RenderTask *rt = (RenderTask *)ctx->data;
    if (!rt->doc)
    {
        rt->doc = (DocParseResult *)malloc(sizeof(DocParseResult));
        if (!rt->doc)
        {
            snprintf(g_renderErrMsg, sizeof(g_renderErrMsg),
                     "Parse Error: out of memory");
            return -1;
        }
        memset(rt->doc, 0, sizeof(DocParseResult));
        tasks_report_progress(0.05f);

        /* detailsOpen overrides for toggle re-renders. */
        DocParseOpts opts;
        memset(&opts, 0, sizeof(opts));
        if (rt->isToggle && detailsKeyCount > 0)
        {
            opts.detailsOpen = detailsOpenState;
            opts.detailsOpenCount = detailsKeyCount;
        }
        opts.svgDecoder = app_svg_decoder;

        int rc = document_parse(rt->body, rt->url, currentBrowseMode,
                                rt->isToggle ? &opts : NULL, rt->doc);
        if (rc != 0 || rt->doc->parseError)
        {
            snprintf(g_renderErrMsg, sizeof(g_renderErrMsg),
                     "Parse Error: parse failed");
            return -1;
        }
        tasks_report_progress(0.6f);
        return 1; /* more work: layout next tick */
    }

    /* Layout step. */
    layout_build(rt->doc);
    if (layout_build_failed())
    {
        snprintf(g_renderErrMsg, sizeof(g_renderErrMsg),
                 "Layout Error: invalid table width (percent not supported)");
        return -1;
    }
    tasks_report_progress(1.0f);
    return 0;
}

static void render_done(void *result, void *userdata)
{
    RenderTask *rt = (RenderTask *)userdata;
    (void)result;
    if (!rt)
    {
        return;
    }
    isRendering = 0;

    /* Free the previous doc AFTER the new build (layout borrows strings). */
    if (currentDoc)
    {
        document_free(currentDoc);
        free(currentDoc);
    }
    currentDoc = rt->doc;
    rt->doc = NULL;

    snprintf(pageTitle, sizeof(pageTitle), "%s",
             currentDoc->title[0] ? currentDoc->title
             : (currentUrlObj ? currentUrlObj->host : "Web Page"));
    if (rt->isToggle)
    {
        scrollY = rt->prevScroll;
        targetScrollY = rt->prevTarget;
    }
    else
    {
        scrollY = 0;
        targetScrollY = 0;
    }
    crankVelocity = 0;
    currentState = STATE_PAGE;
    if (rt->addToHistory)
    {
        storage_add_history(pageTitle, rt->url);
    }
    update_system_menu();

    /* Enqueue images per the image mode setting (Lua: IMAGE_MODE_ALL). */
    {
        const char *im = storage_setting_str("imageMode");
        int imgMode = IMAGE_MODE_ALL;
        for (int m = 0; m < IMAGE_MODE_COUNT; m++)
        {
            if (im && strcmp(im, IMAGE_MODE_NAMES[m]) == 0)
            {
                imgMode = m;
                break;
            }
        }
        if (imgMode == IMAGE_MODE_ALL && currentDoc)
        {
            for (int bi = 0; bi < currentDoc->blockCount; bi++)
            {
                DocBlock *blk = currentDoc->blocks[bi];
                if (blk->type == DOC_BLOCK_IMAGE && blk->src && blk->src[0])
                {
                    imgdec_enqueue(blk->src);
                }
            }
        }
    }

    /* Auto-redirect for <meta http-equiv="refresh">. */
    if (currentDoc && currentDoc->metaRefresh.present && currentDoc->metaRefresh.delay >= 0)
    {
        const char *redirectUrl = currentDoc->metaRefresh.url[0]
                                      ? currentDoc->metaRefresh.url
                                      : rt->url;
        int redirectDelay = (int)(currentDoc->metaRefresh.delay * 1000.0f);
        if (redirectDelay < 0)
        {
            redirectDelay = 0;
        }
        /* Capture the URL in a heap holder for the timer callback. */
        char *urlCopy = pluto_strdup(redirectUrl);
        if (urlCopy)
        {
            pdtimer_perform_after_delay(pd, (unsigned int)redirectDelay,
                                        meta_refresh_cb, urlCopy);
        }
    }

    pluto_free(rt->body);
    free(rt);
}

static void render_error(const char *message, void *userdata)
{
    /* tasks.c passes the task's data pointer as "message" — ignore it and use
     * the static error buffer set by render_step. */
    (void)message;
    RenderTask *rt = (RenderTask *)userdata;
    isRendering = 0;
    if (rt)
    {
        if (rt->isToggle)
        {
            /* Lua: undo the toggle flip on error. */
            if (detailsKeyCount > 0)
            {
                detailsOpenState[detailsKeyCount - 1] =
                    !detailsOpenState[detailsKeyCount - 1];
            }
        }
        if (rt->body)
        {
            pluto_free(rt->body);
        }
        if (rt->doc)
        {
            document_free(rt->doc);
            free(rt->doc);
        }
        free(rt);
    }
    error_page_show(g_renderErrMsg, currentUrlObj ? currentUrlObj->normalized : "");
    currentState = STATE_ERROR;
    snprintf(pageTitle, sizeof(pageTitle), "Render Error");
    update_system_menu();
}

static void render_body(const char *body, const char *url, int addToHistory)
{
    isRendering = 1;
    progressCurrent = 0;
    progressTotal = 0;
    snprintf(pageTitle, sizeof(pageTitle), "Rendering...");
    currentState = STATE_LOADING;

    RenderTask *rt = (RenderTask *)calloc(1, sizeof(RenderTask));
    if (!rt)
    {
        isRendering = 0;
        return;
    }
    rt->body = pluto_strdup(body ? body : "");
    snprintf(rt->url, sizeof(rt->url), "%s", url ? url : "");
    rt->addToHistory = addToHistory;
    rt->isToggle = 0;
    tasks_run(render_step, rt, render_done, render_error, rt);
}

static void meta_refresh_cb(void *ud)
{
    char *urlCopy = (char *)ud;
    if (currentState == STATE_PAGE && urlCopy)
    {
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", urlCopy);
    }
    pluto_free(urlCopy);
}

/* SVG decoder hook for document_parse (production path). */
static int app_svg_decoder(const char *xml, int w, int h, void **outBitmap)
{
    *outBitmap = svg_decode(xml, w, h);
    return *outBitmap ? 0 : -1;
}

/* runNavigation + executeNavigation (port). */
static void navigate_to(const char *urlString)
{
    if (!urlString || !urlString[0])
    {
        return;
    }

    /* Any still-running parse/render of a previous page must be dropped;
     * its onComplete would otherwise fire later and hijack the new page. */
    tasks_cancel_all();
    isRendering = 0;
    /* Drop the previous page's layout (borrows doc strings) before the doc
     * is freed on the next render_done. */
    layout_clear();

    logger_log("navigate_to: %s", urlString);

    char urlBuf[648];
    char *unwrapped = url_unwrap_redirect(urlString);
    char *trimmed = strutil_trim_dup(unwrapped ? unwrapped : urlString);
    pluto_free(unwrapped);
    snprintf(urlBuf, sizeof(urlBuf), "%s", trimmed ? trimmed : urlString);
    pluto_free(trimmed);

    /* Reset per-navigation state (Lua runNavigation). The old layout items
     * (with their owned values) are freed by layout_clear during the render
     * task, so no form-value cleanup is needed here. */
    formInputItem = NULL;
    formKeyboardOpen = 0;
    imgdec_clear_cache();

    if (strcmp(urlBuf, "about:home") == 0)
    {
        currentState = STATE_HOME;
        if (!currentUrlObj)
        {
            currentUrlObj = (UrlParsed *)malloc(sizeof(UrlParsed));
        }
        url_parse("about:home", currentUrlObj);
        snprintf(pageTitle, sizeof(pageTitle), "CometBrowser Start Page");
        if (!navigatingHistory)
        {
            push_history("about:home");
        }
        navigatingHistory = 0;
        scrollY = 0;
        targetScrollY = 0;
        home_page_reset();
        update_system_menu();
        return;
    }

    if (strncmp(urlBuf, "about:", 6) == 0)
    {
        /* Other about: pages go through the normal load path (http_client
         * serves about:acidtest etc. internally). */
    }
    else if (strncmp(urlBuf, "http://", 7) != 0 && strncmp(urlBuf, "https://", 8) != 0)
    {
        /* No scheme: search query or bare host (Lua parity). */
        if (url_is_search_query(urlBuf))
        {
            int engine = storage_setting_int("searchEngine");
            if (engine < 0 || engine >= SEARCH_ENGINE_COUNT)
            {
                engine = 0;
            }
            char *full = url_build_search_url(SEARCH_ENGINES[engine].url, urlBuf);
            if (full)
            {
                snprintf(urlBuf, sizeof(urlBuf), "%s", full);
                pluto_free(full);
            }
        }
        else
        {
            char withScheme[648 + 16];
            snprintf(withScheme, sizeof(withScheme), "https://%s", urlBuf);
            snprintf(urlBuf, sizeof(urlBuf), "%s", withScheme);
        }
    }

    if (!currentUrlObj)
    {
        currentUrlObj = (UrlParsed *)malloc(sizeof(UrlParsed));
    }
    url_parse(urlBuf, currentUrlObj);
    currentState = STATE_LOADING;
    progressCurrent = 0;
    progressTotal = 0;
    snprintf(pageTitle, sizeof(pageTitle), "Loading...");
    if (!navigatingHistory)
    {
        push_history(urlBuf);
    }
    navigatingHistory = 0;
    update_system_menu();

    HttpCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.onProgress = http_on_progress;
    cbs.onSuccess = http_on_success;
    cbs.onError = http_on_error;
    http_get(urlBuf, &cbs);
}

/* ── HTTP callbacks (port of the HttpClient.get handler table) ──────────── */
static void http_on_progress(int cur, int total)
{
    progressCurrent = cur;
    progressTotal = total;
}

static void http_on_success(int status, char **headerKeys, char **headerVals,
                            int headerCount, const char *body, size_t bodyLen,
                            const char *url)
{
    (void)status;
    const char *contentType = NULL;
    for (int i = 0; i < headerCount; i++)
    {
        if (headerKeys && headerKeys[i] && headerVals &&
            strcmp(headerKeys[i], "content-type") == 0)
        {
            contentType = headerVals[i];
            break;
        }
    }
    /* Lua: body = Encoding.toUtf8(body, headers["content-type"]). */
    char *utf8 = encoding_to_utf8((const unsigned char *)body,
                                  body ? strlen(body) : 0, contentType);
    render_body(utf8 ? utf8 : (body ? body : ""), url, 1);
    if (utf8)
    {
        pluto_free(utf8);
    }
}

static void http_on_error(const char *message)
{
    error_page_show(message, currentUrlObj ? currentUrlObj->normalized : "");
    currentState = STATE_ERROR;
    snprintf(pageTitle, sizeof(pageTitle), "Connection Error");
    update_system_menu();
}

/* ── Interactive <details> toggling (port of toggleDetails) ─────────────── */
static void toggle_details(const char *dkey)
{
    if (!currentDoc || !currentDoc->rawHtml || !currentDoc->rawHtml[0] || isRendering)
    {
        return;
    }
    int idx = details_key_index(dkey);
    if (idx < 0)
    {
        return;
    }
    detailsOpenState[idx] = !detailsOpenState[idx];

    int prevScroll = scrollY;
    int prevTarget = targetScrollY;
    isRendering = 1;
    progressCurrent = 0;
    progressTotal = 0;
    snprintf(pageTitle, sizeof(pageTitle), "Rendering...");
    currentState = STATE_LOADING;

    RenderTask *rt = (RenderTask *)calloc(1, sizeof(RenderTask));
    if (!rt)
    {
        isRendering = 0;
        detailsOpenState[idx] = !detailsOpenState[idx]; /* undo flip */
        return;
    }
    rt->body = pluto_strdup(currentDoc->rawHtml);
    snprintf(rt->url, sizeof(rt->url), "%s", currentDoc->baseUrl);
    rt->addToHistory = 0;
    rt->isToggle = 1;
    rt->prevScroll = prevScroll;
    rt->prevTarget = prevTarget;
    tasks_run(render_step, rt, render_done, render_error, rt);
}

/* ── Form input values ─────────────────────────────────────────────────────
 * Lua input items alias the block table, so openKeyboardForInput's
 * activeInputField.value = entered made typed text visible to BOTH the
 * renderer and the submit path. In C the LayoutItem and DocBlock are separate
 * structs, so the typed text lives on the item itself (owned heap copy set
 * through layout_set_input_value; freed by layout_clear). form_item_value()
 * therefore just reads the item's current value. */
static void form_set_block_value(const LayoutItem *item, const char *text)
{
    layout_set_input_value(item, text);
}

/* Effective value for an input item (live typed value > block value). */
static const char *form_item_value(const LayoutItem *item)
{
    return item && item->value ? item->value : "";
}

/* ── Form input keyboard (port of openKeyboardForInput) ─────────────────── */
static void open_keyboard_for_input(const LayoutItem *item)
{
    if (!item || !g_kb)
    {
        return;
    }
    formInputItem = item;
    formKeyboardOpen = 1;
    layout_set_selected_input(item); /* Lua: Layout.selectedInputItem = block */
    keyboardApi.setKeyboardWillHideCallback(g_kb, form_kb_will_hide, NULL);
    keyboardApi.setKeyboardDidHideCallback(g_kb, form_kb_did_hide, NULL);
    keyboardApi.setTextChangedCallback(g_kb, form_kb_text_changed, NULL);
    keyboardApi.show(g_kb, item->value ? item->value : "",
                     (unsigned int)(item->value ? strlen(item->value) : 0));
}

static void form_kb_will_hide(int okButtonPressed, void *ud)
{
    (void)ud;
    (void)okButtonPressed;
}

static void form_kb_did_hide(void *ud)
{
    (void)ud;
    formKeyboardOpen = 0;
    skipInputFrames = 2;
    if (formInputItem && g_kb)
    {
        char *txt = NULL;
        unsigned int n = 0;
        keyboardApi.getText(g_kb, &txt, &n);
        if (txt)
        {
            /* Lua: maxlength truncation on commit. */
            if (formInputItem->maxlength >= 0 && (int)n > formInputItem->maxlength)
            {
                txt[formInputItem->maxlength] = '\0';
            }
            form_set_block_value(formInputItem, txt);
            pluto_free(txt);
        }
    }
    formInputItem = NULL;
    if (g_kb)
    {
        keyboardApi.setKeyboardWillHideCallback(g_kb, NULL, NULL);
        keyboardApi.setKeyboardDidHideCallback(g_kb, NULL, NULL);
        keyboardApi.setTextChangedCallback(g_kb, NULL, NULL);
    }
}

static void form_kb_text_changed(void *ud)
{
    (void)ud;
    /* Lua updateFrame: live-sync keyboard text into the focused input. */
    if (formInputItem && g_kb)
    {
        char *txt = NULL;
        unsigned int n = 0;
        keyboardApi.getText(g_kb, &txt, &n);
        if (txt)
        {
            form_set_block_value(formInputItem, txt);
            pluto_free(txt);
        }
    }
}

/* ── activateFormBlock (port) ───────────────────────────────────────────── */
static void activate_form_block(const LayoutItem *item)
{
    if (!item || item->disabled)
    {
        return;
    }
    if (item->block && ((const DocBlock *)item->block)->inert)
    {
        return;
    }
    switch (item->type)
    {
    case LRI_INPUT_FIELD:
        open_keyboard_for_input(item);
        break;
    case LRI_INPUT_SUBMIT:
        submit_form(item->formAction, item);
        break;
    case LRI_CHECKBOX_FIELD:
    {
        DocBlock *block = (DocBlock *)item->block;
        if (block->radio)
        {
            for (int i = 0; i < layout_get_item_count(); i++)
            {
                const LayoutItem *it = layout_item_at(i);
                if (it && it != item && it->type == LRI_CHECKBOX_FIELD && it->radio &&
                    it->name && block->name && strcmp(it->name, block->name) == 0)
                {
                    ((DocBlock *)it->block)->checked = 0;
                }
            }
            block->checked = 1;
        }
        else
        {
            block->checked = !block->checked;
        }
        layout_set_selected_input(item);
        break;
    }
    case LRI_SELECT_FIELD:
    {
        DocBlock *block = (DocBlock *)item->block;
        int n = block->optionCount;
        if (n > 0)
        {
            int tries = n + 1;
            while (tries > 0)
            {
                block->selectedIndex = (block->selectedIndex % n) + 1;
                DocOption *opt = block->options[block->selectedIndex - 1];
                if (opt && !opt->disabled && !opt->group)
                {
                    break;
                }
                tries--;
            }
        }
        layout_set_selected_input(item);
        break;
    }
    default:
        break;
    }
}

/* ── submitForm (port) ──────────────────────────────────────────────────── */
static void submit_form(const char *formAction, const LayoutItem *inputBlock)
{
    char action[512];
    if (!formAction || !formAction[0])
    {
        snprintf(action, sizeof(action), "%s",
                 currentUrlObj ? currentUrlObj->normalized : "");
    }
    else
    {
        snprintf(action, sizeof(action), "%s", formAction);
    }
    if (strcmp(action, "#") == 0)
    {
        return;
    }

    StrBuf query;
    strbuf_init(&query);
    int pairCount = 0;

    /* First-seen name dedupe (Lua `seen` table). */
    char seen[32][64];
    int seenCount = 0;

    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (!it)
        {
            continue;
        }
        const char *name = NULL;
        const char *value = NULL;
        int include = 0;
        int sameForm = it->formAction && it->formAction[0] &&
                       strcmp(it->formAction, action) == 0;

        switch (it->type)
        {
        case LRI_HIDDEN_FIELD:
        case LRI_INPUT_FIELD:
            if (!it->disabled && sameForm && it->name && it->name[0])
            {
                name = it->name;
                value = form_item_value(it);
                include = 1;
            }
            break;
        case LRI_CHECKBOX_FIELD:
            if (!it->disabled && sameForm && it->checked)
            {
                name = it->name ? it->name : "";
                value = (it->value && it->value[0]) ? it->value : "on";
                include = 1;
            }
            break;
        case LRI_SELECT_FIELD:
        {
            if (!it->disabled && sameForm)
            {
                DocBlock *b = (DocBlock *)it->block;
                int si = b->selectedIndex;
                if (si >= 1 && si <= b->optionCount)
                {
                    DocOption *opt = b->options[si - 1];
                    if (opt && !opt->disabled && !opt->group)
                    {
                        name = it->name ? it->name : "";
                        value = (opt->value && opt->value[0]) ? opt->value
                                : (opt->text ? opt->text : "");
                        include = 1;
                    }
                }
            }
            break;
        }
        default:
            break;
        }

        if (include && name && name[0])
        {
            int dup = 0;
            for (int s2 = 0; s2 < seenCount; s2++)
            {
                if (strcmp(seen[s2], name) == 0)
                {
                    dup = 1;
                    break;
                }
            }
            if (!dup)
            {
                if (pairCount > 0)
                {
                    strbuf_append(&query, "&");
                }
                char *ek = url_encode(name);
                char *ev = url_encode(value ? value : "");
                strbuf_appendf(&query, "%s=%s", ek ? ek : "", ev ? ev : "");
                pluto_free(ek);
                pluto_free(ev);
                pairCount++;
                if (seenCount < 32)
                {
                    snprintf(seen[seenCount], sizeof(seen[0]), "%s", name);
                    seenCount++;
                }
            }
        }
    }

    /* Fallback: no formAction matches → all visible named controls. */
    if (pairCount == 0)
    {
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (!it || it->disabled)
            {
                continue;
            }
            if ((it->type == LRI_HIDDEN_FIELD || it->type == LRI_INPUT_FIELD) &&
                it->name && it->name[0])
            {
                if (pairCount > 0)
                {
                    strbuf_append(&query, "&");
                }
                char *ek = url_encode(it->name);
                char *ev = url_encode(form_item_value(it));
                strbuf_appendf(&query, "%s=%s", ek ? ek : "", ev ? ev : "");
                pluto_free(ek);
                pluto_free(ev);
                pairCount++;
            }
        }
    }

    /* Always include the clicked submit button's name=value (HTML spec). */
    if (inputBlock && !inputBlock->disabled && inputBlock->name && inputBlock->name[0])
    {
        int dup = 0;
        for (int s2 = 0; s2 < seenCount; s2++)
        {
            if (strcmp(seen[s2], inputBlock->name) == 0)
            {
                dup = 1;
                break;
            }
        }
        if (!dup)
        {
            if (pairCount > 0)
            {
                strbuf_append(&query, "&");
            }
            char *ek = url_encode(inputBlock->name);
            char *ev = url_encode(inputBlock->value ? inputBlock->value : "");
            strbuf_appendf(&query, "%s=%s", ek ? ek : "", ev ? ev : "");
            pluto_free(ek);
            pluto_free(ev);
            pairCount++;
        }
    }

    char target[640];
    const char *sep = strchr(action, '?') ? "&" : "?";
    if (query.len > 0)
    {
        snprintf(target, sizeof(target), "%s%s%s", action, sep, query.data);
    }
    else
    {
        snprintf(target, sizeof(target), "%s", action);
    }
    strbuf_free(&query);

    pendingNavUrlSet = 1;
    snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", target);
}

/* ── Settings change (reference SettingsPage.onChangeCallback) ──────────── */
static void settings_on_change(void)
{
    currentBrowseMode = storage_setting_int("mode");
    if (currentBrowseMode != MODE_READER && currentBrowseMode != MODE_RAW_HTML)
    {
        currentBrowseMode = MODE_READER;
    }
    if (currentUrlObj)
    {
        if (currentDoc && currentDoc->rawHtml && currentDoc->rawHtml[0])
        {
            render_body(currentDoc->rawHtml, currentUrlObj->normalized, 0);
        }
        else
        {
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                     currentUrlObj->normalized);
        }
    }
}

static int updateFrame(void *userdata)
{
    (void)userdata;
    frameCount++;

    /* Clear the full framebuffer every frame (was dropped accidentally during
     * the BTEST scaffolding removal — without it, home-page scrolling smears
     * previous frames' content across the screen). */
    pd->graphics->clear((LCDColor)kColorWhite);

    /* ── button state (current/pushed/released) ── */
    unsigned int btnCurrent = 0, btnPushed = 0, btnReleased = 0;
    pd->system->getButtonState((PDButtons *)&btnCurrent, (PDButtons *)&btnPushed,
                               (PDButtons *)&btnReleased);

    float crankChange = pd->system->getCrankChange();

    /* ── pendingNavUrl processing (Lua: top of updateFrame) ── */
    if (pendingNavUrlSet)
    {
        pendingNavUrlSet = 0;
        char dest[640];
        snprintf(dest, sizeof(dest), "%s", pendingNavUrl);
        pendingNavUrl[0] = '\0';
        navigate_to(dest);
    }

    /* ── crank velocity physics (Lua parity) ── */
    {
        int keyboardActive = formKeyboardOpen || address_bar_is_open();
        if (keyboardActive)
        {
            crankChange = 0.0f;
            crankVelocity = 0.0f;
        }
        else if (crankChange != 0.0f)
        {
            if (storage_setting_int("invertCrank"))
            {
                crankVelocity -= crankChange * 1.6f;
            }
            else
            {
                crankVelocity += crankChange * 1.6f;
            }
        }
        crankVelocity *= 0.85f;
        if (crankVelocity > -0.05f && crankVelocity < 0.05f)
        {
            crankVelocity = 0.0f;
        }

        /* Real crank input drops the link selection (Lua parity). */
        if (crankChange != 0.0f)
        {
            lm_clear_selection();
        }
    }

    /* Reset B hold state outside page-browsing states (Lua parity). */
    if (currentState != STATE_HOME && currentState != STATE_PAGE)
    {
        bHoldActive = 0;
    }

    /* ── B BUTTON HOLD STATE MACHINE (port of main.lua) ──
     * B press: start tracking; B held + Left/Right: history navigation
     * (deferred to Phase 30 when history exists); B release (4 clean
     * frames): open the address bar — requires the keyboard (Phase 13/14).
     * Until the keyboard lands, B-release is logged as a navigation intent. */
    if (currentState == STATE_HOME || currentState == STATE_PAGE)
    {
        int bDown = (btnCurrent & (1 << 4)) ? 1 : 0; /* kButtonB */
        unsigned int nowMs = pd->system->getCurrentTimeMilliseconds();

        if (bDown)
        {
            bNotPressedFrames = 0;
        }
        else
        {
            bNotPressedFrames++;
        }

        if ((btnPushed & (1 << 4)) && !bHoldActive)
        {
            bHoldActive = 1;
            bHoldUsedDir = 0;
            bHoldStartMs = nowMs;
            bNotPressedFrames = 0;
        }
        if (bHoldActive && (btnCurrent & (1 << 4)))
        {
            if (!address_bar_is_open() && !formKeyboardOpen)
            {
                if (btnCurrent & (1 << 0)) /* Left: back */
                {
                    if (!bHoldUsedDir)
                    {
                        const char *prev = go_back();
                        pendingNavUrlSet = 1;
                        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                                 prev ? prev : "about:home");
                    }
                    bHoldUsedDir = 1;
                }
                else if (btnCurrent & (1 << 1)) /* Right: forward */
                {
                    if (!bHoldUsedDir)
                    {
                        const char *fwd = go_forward();
                        if (fwd)
                        {
                            pendingNavUrlSet = 1;
                            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", fwd);
                        }
                    }
                    bHoldUsedDir = 1;
                }
            }
        }
        if (bHoldActive && bNotPressedFrames >= 4)
        {
            /* Lua parity (main.lua): `not bHoldUsedDir and not AddressBar.isOpen
             * and not keyboardOpen` — a B press while a keyboard is open belongs
             * to the keyboard (backspace), never opens the address bar. */
            if (!bHoldUsedDir && !address_bar_is_open() && !formKeyboardOpen)
            {
                /* Lua: open(""|curUrl, navigateTo) then launchKeyboard(). */
                if (currentState == STATE_HOME)
                {
                    address_bar_open("", ab_submit_goto, NULL);
                }
                else
                {
                    address_bar_open(currentUrlObj ? currentUrlObj->normalized : "",
                                     ab_submit_goto, NULL);
                }
                address_bar_launch_keyboard();
                logger_log("main: B-release opened address bar (keyboard)");
            }
            bHoldActive = 0;
            bNotPressedFrames = 0;
        }
    }
    else
    {
        bNotPressedFrames = 0;
        bHoldActive = 0;
    }

    if (skipInputFrames > 0)
    {
        skipInputFrames--;
    }

    /* ── STATE MACHINE ── */
    switch (currentState)
    {
    case STATE_HOME:
    {
        if (skipInputFrames <= 0)
        {
            char *selUrl = home_page_handle_input(btnPushed, home_settings_opened);
            if (selUrl)
            {
                navigate_to(selUrl);
                pluto_free(selUrl);
            }
        }
        home_page_draw(crankChange);
        break;
    }

    case STATE_PAGE:
    {
        /* Full port of main.lua STATE_PAGE handling. */
        int totalH = layout_get_total_height();
        int maxScroll = totalH - CONTENT_HEIGHT;
        if (maxScroll < 0)
        {
            maxScroll = 0;
        }
        int isHtmlMode = (currentBrowseMode == MODE_RAW_HTML);

        if (skipInputFrames <= 0 && !formKeyboardOpen && !address_bar_is_open())
        {
            /* ── ON-DEMAND IMAGE OVERLAY ───────────────────────────────── */
            if (layout_has_on_demand_overlay())
            {
                const char *ovHref = NULL;
                const LMLink *ovLink = NULL; /* Lua: Layout.onDemandOverlay.href */
                (void)ovLink;
                ovHref = layout_on_demand_href();
                const char *ovAction =
                    layout_handle_on_demand_input(btnPushed);
                if (ovAction && strcmp(ovAction, "link") == 0 && ovHref &&
                    ovHref[0])
                {
                    pendingNavUrlSet = 1;
                    snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", ovHref);
                }
            }
            /* ── READER MODE: crank scrolls, D-Pad navigates links ─────── */
            else if (!isHtmlMode)
            {
                targetScrollY += (int)crankVelocity;

                /* A + Left = Back, A + Right = Forward */
                if (btnCurrent & (1 << 5))
                {
                    if (btnPushed & (1 << 0))
                    {
                        const char *prev = go_back();
                        pendingNavUrlSet = 1;
                        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                                 prev ? prev : "about:home");
                    }
                    else if (btnPushed & (1 << 1))
                    {
                        const char *fwd = go_forward();
                        if (fwd)
                        {
                            pendingNavUrlSet = 1;
                            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", fwd);
                        }
                    }
                }
                else
                {
                    if (btnPushed & (1 << 3)) /* DOWN */
                    {
                        const LMLink *nextLink = lm_select_next(scrollY);
                        if (nextLink)
                        {
                            int linkY = nextLink->primaryRect.y;
                            if (linkY > targetScrollY + CONTENT_HEIGHT - 50)
                            {
                                targetScrollY = linkY - CONTENT_HEIGHT + 70;
                            }
                            else if (linkY < targetScrollY + CONTENT_Y)
                            {
                                targetScrollY = linkY - CONTENT_Y - 20;
                            }
                        }
                        else
                        {
                            targetScrollY += 40;
                        }
                    }
                    else if (btnPushed & (1 << 2)) /* UP */
                    {
                        const LMLink *prevLink = lm_select_prev(scrollY);
                        if (prevLink)
                        {
                            int linkY = prevLink->primaryRect.y;
                            if (linkY < targetScrollY + CONTENT_Y)
                            {
                                targetScrollY = linkY - CONTENT_Y - 20;
                            }
                            else if (linkY > targetScrollY + CONTENT_HEIGHT - 50)
                            {
                                targetScrollY = linkY - CONTENT_HEIGHT + 70;
                            }
                        }
                        else
                        {
                            targetScrollY -= 40;
                        }
                    }
                }

                /* (A) alone = follow focused link / activate form input */
                if ((btnPushed & (1 << 5)) &&
                    !(btnCurrent & (1 << 0)) && !(btnCurrent & (1 << 1)) &&
                    !layout_get_on_demand_consumed())
                {
                    const LMLink *activeLink = lm_get_selected_link();
                    if (activeLink)
                    {
                        const LMRectAux *primary = lm_rect_aux(
                            activeLink->index, 0);
                        if (primary && primary->isToggle && primary->toggleKey)
                        {
                            toggle_details(primary->toggleKey);
                        }
                        else if (primary && primary->isFormInput &&
                                 primary->inputItem)
                        {
                            activate_form_block((const LayoutItem *)primary->inputItem);
                        }
                        else if (activeLink->href &&
                                 !(primary && primary->inert))
                        {
                            const char *im = storage_setting_str("imageMode");
                            int imgMode = IMAGE_MODE_ALL;
                            for (int m = 0; m < IMAGE_MODE_COUNT; m++)
                            {
                                if (im && strcmp(im, IMAGE_MODE_NAMES[m]) == 0)
                                {
                                    imgMode = m;
                                    break;
                                }
                            }
                            if (imgMode == IMAGE_MODE_ONDEMAND && primary &&
                                primary->isImage)
                            {
                                layout_show_on_demand_overlay(
                                    primary->src, activeLink->href, primary->alt);
                            }
                            else
                            {
                                pendingNavUrlSet = 1;
                                snprintf(pendingNavUrl, sizeof(pendingNavUrl),
                                         "%s", activeLink->href);
                            }
                        }
                    }
                }
            }
            /* ── HTML MODE: Virtual Mouse Cursor ───────────────────────── */
            else
            {
                if (btnCurrent & (1 << 5)) /* A held: history nav */
                {
                    if (btnPushed & (1 << 0))
                    {
                        const char *prev = go_back();
                        pendingNavUrlSet = 1;
                        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                                 prev ? prev : "about:home");
                    }
                    else if (btnPushed & (1 << 1))
                    {
                        const char *fwd = go_forward();
                        if (fwd)
                        {
                            pendingNavUrlSet = 1;
                            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", fwd);
                        }
                    }
                }
                else if (!bHoldActive)
                {
                    int moved = 0;
                    if (btnCurrent & (1 << 0))
                    {
                        mouseX -= MOUSE_SPEED;
                        if (mouseX < 2) { mouseX = 2; }
                        moved = 1;
                    }
                    if (btnCurrent & (1 << 1))
                    {
                        mouseX += MOUSE_SPEED;
                        if (mouseX > SCREEN_WIDTH - 2) { mouseX = SCREEN_WIDTH - 2; }
                        moved = 1;
                    }
                    if (btnCurrent & (1 << 2))
                    {
                        mouseY -= MOUSE_SPEED;
                        if (mouseY < CONTENT_Y + 2) { mouseY = CONTENT_Y + 2; }
                        moved = 1;
                    }
                    if (btnCurrent & (1 << 3))
                    {
                        mouseY += MOUSE_SPEED;
                        if (mouseY > SCREEN_HEIGHT - 2) { mouseY = SCREEN_HEIGHT - 2; }
                        moved = 1;
                    }

                    if (moved)
                    {
                        /* Lua: crank nudges mouseY while D-pad held. */
                        float crankChange = pd->system->getCrankChange();
                        if (crankChange != 0.0f)
                        {
                            mouseY += (int)(crankChange * 0.5f);
                            if (mouseY < CONTENT_Y + 2) { mouseY = CONTENT_Y + 2; }
                            if (mouseY > SCREEN_HEIGHT - 2) { mouseY = SCREEN_HEIGHT - 2; }
                        }
                        const int SCROLL_ZONE = 20;
                        if (mouseY <= CONTENT_Y + SCROLL_ZONE)
                        {
                            float strength =
                                (float)(SCROLL_ZONE - (mouseY - CONTENT_Y)) / SCROLL_ZONE;
                            targetScrollY -= (int)(3.0f * (1.0f + strength * 3.0f));
                        }
                        else if (mouseY >= SCREEN_HEIGHT - SCROLL_ZONE)
                        {
                            float strength =
                                (float)(SCROLL_ZONE - (SCREEN_HEIGHT - mouseY)) / SCROLL_ZONE;
                            targetScrollY += (int)(3.0f * (1.0f + strength * 3.0f));
                        }
                    }
                    else
                    {
                        targetScrollY += (int)crankVelocity;
                    }
                }

                /* (A) = left click: follow hovered link / activate input. */
                if ((btnPushed & (1 << 5)) &&
                    !(btnCurrent & (1 << 0)) && !(btnCurrent & (1 << 1)) &&
                    !layout_get_on_demand_consumed())
                {
                    const LMLink *hitLink = lm_get_hovered_link(mouseX, mouseY + scrollY);
                    if (hitLink)
                    {
                        const LMRectAux *primary = lm_rect_aux(hitLink->index, 0);
                        if (primary && primary->isToggle && primary->toggleKey)
                        {
                            toggle_details(primary->toggleKey);
                        }
                        else if (primary && primary->isFormInput &&
                                 primary->inputItem)
                        {
                            activate_form_block((const LayoutItem *)primary->inputItem);
                        }
                        else if (hitLink->href && !(primary && primary->inert))
                        {
                            const char *im = storage_setting_str("imageMode");
                            int imgMode = IMAGE_MODE_ALL;
                            for (int m = 0; m < IMAGE_MODE_COUNT; m++)
                            {
                                if (im && strcmp(im, IMAGE_MODE_NAMES[m]) == 0)
                                {
                                    imgMode = m;
                                    break;
                                }
                            }
                            if (imgMode == IMAGE_MODE_ONDEMAND && primary &&
                                primary->isImage)
                            {
                                layout_show_on_demand_overlay(
                                    primary->src, hitLink->href, primary->alt);
                            }
                            else
                            {
                                pendingNavUrlSet = 1;
                                snprintf(pendingNavUrl, sizeof(pendingNavUrl),
                                         "%s", hitLink->href);
                            }
                        }
                    }
                }
            }

            /* Scroll clamp + smoothing (Lua parity). */
            if (targetScrollY < 0)
            {
                targetScrollY = 0;
            }
            if (targetScrollY > maxScroll)
            {
                targetScrollY = maxScroll;
            }
            scrollY += (int)((targetScrollY - scrollY) * 0.4f);
        }

        /* Hovered-image management (IMAGE_MODE_HOVER). */
        {
            const char *im = storage_setting_str("imageMode");
            int imgMode = IMAGE_MODE_ALL;
            for (int m = 0; m < IMAGE_MODE_COUNT; m++)
            {
                if (im && strcmp(im, IMAGE_MODE_NAMES[m]) == 0)
                {
                    imgMode = m;
                    break;
                }
            }
            if (imgMode == IMAGE_MODE_HOVER)
            {
                const char *currentHoverSrc = NULL;
                if (isHtmlMode)
                {
                    const LMLink *hovered = lm_get_hovered_link(mouseX, mouseY + scrollY);
                    if (hovered)
                    {
                        const LMRectAux *pr = lm_rect_aux(hovered->index, 0);
                        if (pr && pr->isImage)
                        {
                            currentHoverSrc = pr->src;
                        }
                    }
                }
                else
                {
                    const LMLink *selLink = lm_get_selected_link();
                    if (selLink)
                    {
                        const LMRectAux *pr = lm_rect_aux(selLink->index, 0);
                        if (pr && pr->isImage)
                        {
                            currentHoverSrc = pr->src;
                        }
                    }
                }
                layout_evict_hovered_image(currentHoverSrc);
                if (currentHoverSrc)
                {
                    imgdec_enqueue(currentHoverSrc);
                }
            }
        }

        layout_draw(scrollY);
        layout_evict_offscreen(scrollY);
        {
            const LMLink *sel = lm_get_selected_link();
            hud_draw(scrollY, layout_get_total_height(),
                     sel ? sel->href : NULL);
        }
        break;
    }

    case STATE_LOADING:
    {
        /* Port of main.lua STATE_LOADING draw + input. */
        LCDFont *fontH = style_font(PLUTO_FONT_HEADING1);
        LCDFont *fontB = style_font(PLUTO_FONT_BODY);
        LCDColor white = kColorWhite;
        LCDColor black = kColorBlack;
        pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT, white);
        pd->graphics->setFont(fontH);
        pd->graphics->drawText(isRendering ? "Rendering Web Page..."
                                           : "Loading Web Page...",
                               isRendering ? strlen("Rendering Web Page...")
                                           : strlen("Loading Web Page..."),
                               kUTF8Encoding, 24, 60);
        pd->graphics->setFont(fontB);
        {
            char displayHost[512];
            snprintf(displayHost, sizeof(displayHost), "%s",
                     currentUrlObj ? currentUrlObj->normalized : "Web Request");
            if (strlen(displayHost) > 45)
            {
                displayHost[42] = '\0';
                strcat(displayHost, "...");
            }
            pd->graphics->drawText(displayHost, strlen(displayHost),
                                   kUTF8Encoding, 24, 90);
        }
        if (isRendering)
        {
            float p = tasks_get_progress();
            if (p > 1.0f) { p = 1.0f; }
            int pct = (int)(p * 100.0f);
            char line[96];
            snprintf(line, sizeof(line), "Rendering page content... %d%%", pct);
            pd->graphics->drawText(line, strlen(line), kUTF8Encoding, 24, 116);
            pd->graphics->drawRect(24, 138, 352, 10, black);
            pd->graphics->fillRect(24, 138, (int)(352.0f * p), 10, black);
        }
        else if (progressTotal > 0)
        {
            int pct = (int)((float)progressCurrent / (float)progressTotal * 100.0f);
            if (pct > 100) { pct = 100; }
            char line[96];
            snprintf(line, sizeof(line), "Received: %d / %d bytes (%d%%)",
                     progressCurrent, progressTotal, pct);
            pd->graphics->drawText(line, strlen(line), kUTF8Encoding, 24, 116);
            pd->graphics->drawRect(24, 138, 352, 10, black);
            pd->graphics->fillRect(24, 138, 352 * pct / 100, 10, black);
        }
        else
        {
            char line[96];
            snprintf(line, sizeof(line), "Downloading on device... (%d bytes)",
                     progressCurrent);
            pd->graphics->drawText(line, strlen(line), kUTF8Encoding, 24, 116);
        }
        pd->graphics->drawText("(B) Cancel  *  (Left) Back", 25, kUTF8Encoding, 24, 165);

        if (btnPushed & (1 << 4)) /* B: cancel */
        {
            http_cancel();
            go_home();
        }
        if (btnPushed & (1 << 0)) /* Left: back */
        {
            http_cancel();
            const char *prev = go_back();
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                     prev ? prev : "about:home");
        }
        break;
    }

    case STATE_ERROR:
    {
        char *action = error_page_handle_input(btnCurrent, btnPushed, btnReleased);
        if (action)
        {
            if (strcmp(action, "retry") == 0 && currentUrlObj)
            {
                navigate_to(currentUrlObj->normalized);
            }
            else if (strcmp(action, "home") == 0)
            {
                go_home();
            }
            else if (strcmp(action, "search") == 0)
            {
                /* Lua: AddressBar.open("", navigateTo) + launchKeyboard(). */
                address_bar_open("", ab_submit_goto, NULL);
                address_bar_launch_keyboard();
                logger_log("main: error page opened address bar (keyboard)");
            }
            pluto_free(action);
        }
        if (btnPushed & (1 << 0)) /* Left → back through history */
        {
            const char *prev = go_back();
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                     prev ? prev : "about:home");
        }
        error_page_draw();
        break;
    }

    case STATE_BOOKMARKS:
    {
        char *action = bookmarks_page_handle_input(btnPushed);
        if (action)
        {
            if (strcmp(action, "close") == 0)
            {
                go_home();
            }
            else
            {
                navigate_to(action);
            }
            pluto_free(action);
        }
        bookmarks_page_draw(crankChange);
        break;
    }

    case STATE_HISTORY:
    {
        char *action = history_page_handle_input(btnPushed);
        if (action)
        {
            if (strcmp(action, "close") == 0)
            {
                go_home();
            }
            else
            {
                navigate_to(action);
            }
            pluto_free(action);
        }
        history_page_draw(crankChange);
        break;
    }

    case STATE_SETTINGS:
    {
        char *action = settings_page_handle_input(btnPushed, settings_cleared_cookies);
        if (action)
        {
            int prev = settings_page_previous_state();
            settings_page_close();
            if (prev == STATE_HOME || prev == STATE_PAGE)
            {
                go_home();
            }
            else
            {
                currentState = (BrowserState)prev;
            }
            pluto_free(action);
        }
        settings_page_draw();
        break;
    }

    default:
        break;
    }

    /* ── Address bar per-frame glue (Lua parity) ──
     * 1. Arm skipInputFrames requested by the keyboard willHide callback.
     * 2. System-menu-close detection: >500ms frame gap closes the bar.
     * 3. Live-sync keyboard text into the bar (Lua textChanged callback kept
     *    g_inputText current; sync covers any path that missed it).
     * 4. drawOverlay runs after chrome (Lua: Chrome.draw then drawOverlay). */
    {
        int pending = address_bar_consume_skip_frames();
        if (pending > skipInputFrames)
        {
            skipInputFrames = pending;
        }
        if (g_lastFrameAtMs > 0 &&
            (unsigned int)(pd->system->getCurrentTimeMilliseconds() - g_lastFrameAtMs) > 500)
        {
            if (address_bar_is_open())
            {
                address_bar_cancel();
                logger_log("main: system menu close detected, address bar cancelled");
            }
            skipInputFrames = 2;
        }
        g_lastFrameAtMs = pd->system->getCurrentTimeMilliseconds();
        address_bar_sync_text();
    }

    /* ── chrome: always drawn last (Lua parity) ── */
    chrome_draw(currentUrlObj, NULL, currentState == STATE_LOADING, 0, 0,
                currentBrowseMode == 0);
    address_bar_draw_overlay();

    /* Hover status bar: show URL under cursor (like desktop browsers). */
    if (currentState == STATE_PAGE && currentBrowseMode == MODE_RAW_HTML)
    {
        const LMLink *hovLink = lm_get_hovered_link(mouseX, mouseY + scrollY);
        if (hovLink && hovLink->href)
        {
            hud_draw_hover_status(hovLink->href);
        }
    }

    /* Draw mouse cursor as the very last thing so nothing can draw over it
     * (Lua main.lua: white-filled triangle + black outline + inner line). */
    if (currentState == STATE_PAGE && currentBrowseMode == MODE_RAW_HTML)
    {
        int mx = mouseX, my = mouseY;
        pd->graphics->fillTriangle(mx, my, mx + 10, my + 4, mx + 4, my + 10,
                                   kColorWhite);
        /* Lua gfx.drawTriangle has no C equivalent: draw the 3 edges with
         * width 1 (Lua's default line width) via drawLine. */
        pd->graphics->drawLine(mx, my, mx + 10, my + 4, 1, kColorBlack);
        pd->graphics->drawLine(mx + 10, my + 4, mx + 4, my + 10, 1, kColorBlack);
        pd->graphics->drawLine(mx + 4, my + 10, mx, my, 1, kColorBlack);
        pd->graphics->drawLine(mx, my, mx + 4, my + 10, 1, kColorBlack);
    }

    /* State-transition log (first entry to each state). */
    if (currentState != g_lastLoggedState)
    {
        logger_log("state -> %d (frame %u)", (int)currentState, frameCount);
        g_lastLoggedState = currentState;
    }

    if (frameCount == 1)
    {
        logger_log("updateFrame: first frame executed");
    }


    /* Log a periodic heartbeat every 300 frames (~10s at 30fps) so logs show liveness. */
    if (frameCount % 300 == 0)
    {
        logger_log("updateFrame: heartbeat frame=%u", frameCount);
    }

    /* Phase 4: pump timers each frame. */
    pdtimer_update();

    /* Phase 24: pump the HTTP state machine each frame. */
    http_update();

    /* P30: pump the image download/decode queue each frame. */
    imgdec_update();

    /* Drive cooperative tasks (Lua Tasks.update parity). */
    tasks_update();

    /* Return non-zero to tell the system to update the display. */
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
__attribute__((noinline)) static int pluto_event_handler(PlaydateAPI *api, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    switch (event)
    {
    case kEventInit:
        pd = api;
        playdate = api; /* keyboard port global (same pointer) */
        logger_init(pd);

        logger_log("=== PlutoBrowser boot ===");
        logger_log("eventHandler: kEventInit, osversion=%u", pd->system->getSystemInfo()->osversion);

        /* Phase 14: address bar module init. */
        address_bar_init();

        pdtimer_init(pd);
        http_client_init(pd);
        imgdec_init(pd);
        tasks_init(pd);

        storage_init(pd);
        style_init(pd);
        settings_page_set_onchange_callback(settings_on_change);
        layout_init(pd);

        /* Lua boot: currentBrowseMode = Storage.settings.mode. */
        currentBrowseMode = storage_setting_int("mode");
        if (currentBrowseMode != MODE_READER && currentBrowseMode != MODE_RAW_HTML)
        {
            currentBrowseMode = MODE_READER;
        }

        /* Phase 12: boot into the home page with the system menu wired. */
        update_system_menu();
        home_page_reset();
        currentState = STATE_HOME;

        /* Keyboard instance. The port's contract:
         * setPlaydateUpdateCallback MUST be called before show() — while the
         * keyboard is visible it OWNS the system update callback and invokes
         * our update from inside its own (calling it with playdateUpdate NULL
         * would crash). */
        g_kb = keyboardApi.newKeyboard();
        keyboardApi.setPlaydateUpdateCallback(g_kb, updateFrame, NULL);
        /* Replace the Lua run loop with our native update function. */
        pd->system->setUpdateCallback(updateFrame, NULL);
        break;

    case kEventTerminate:
        logger_log("eventHandler: kEventTerminate, frames=%u", frameCount);
        break;

    default:
        break;
    }

    return 0;
}

/* Thin event shim: keeping the entry point small means every other event
 * (update ticks, terminate, lock) runs on a shallow stack — required by the
 * device's small game-task stack. */
int eventHandler(PlaydateAPI *api, PDSystemEvent event, uint32_t arg)
{
    return pluto_event_handler(api, event, arg);
}

