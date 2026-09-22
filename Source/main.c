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
#include "core/pluto_mem.h"
#include "core/pluto_spill.h"
#include "core/pluto_page.h"
#include "core/pluto_snap.h"
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
#include "html/jsbridge.h"
#include "html/jsext.h"
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

/* SW1 telemetry funnel backend: the RAW SDK call (no accounting, no
 * recursion). pluto_realloc (below) wraps THIS with accounting; the funnel
 * (core/pluto_mem.c) calls this directly for the underlying allocation. */
void *pluto_mem_sdk_realloc(void *p, size_t n);
void *pluto_mem_sdk_realloc(void *p, size_t n)
{
    if (!pd)
    {
        return NULL;
    }
    return pd->system->realloc(p, n);
}

/* Shared realloc mirror (pluto_free's counterpart). Routes through the
 * SW1 telemetry funnel (core/pluto_mem.c): same SDK call underneath, plus
 * live/peak byte accounting and (when enabled) the SW3a soft budget. */
void *pluto_realloc(void *p, size_t n)
{
    return pluto_mem_realloc(p, n);
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
#if defined(PLUTO_NAV_AUTOTEST)
static void nav_autotest_tick(void);
#endif

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

/* ── SW6 snapshot cache ────────────────────────────────────────────────── */
/* SW6 fast-path telemetry: how many navigations rendered from a snapshot
 * (also consumed by the snap autotest for a deterministic PASS verdict). */
static int g_snapFastPathHits = 0;
static int snapBypass = 0; /* 1 = next navigation skips the snapshot load
                            * (hard reload: view-mode flip, settings save) */
#define SNAP_TTL_SECONDS (7UL * 24UL * 3600UL) /* 7-day freshness window */
#define SNAP_MAX_ENTRIES 8                     /* storage-pressure LRU cap */

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
        snapBypass = 1; /* hard reload: snapshot would be for the OLD mode */
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
    DocParseResult *doc;   /* set by the parse step, consumed by done */
    DocParseResult *rewalkDoc; /* re-walk this LIVE doc (JS mutation path) */
    /* ── DOC_SCRIPT_FULL: external <script src> prefetch chain ──
     * Stage 1: scan + resolve, begin the fetch session, then yield (return 1)
     * so the frame can pump http_update(); Stage 2 re-enters through the SAME
     * !rt->doc gate, pumps the session (return 1 while any file is in
     * flight), then detaches the results and falls through to the parser.
     * parseExtedDoc guards the stage boundary: once parsing starts, Stage-2
     * code must never run again (the task then behaves like a plain parse). */
    int prefetching;           /* 1 between Stage-1 begin and Stage-2 detach */
    int parseExtedDoc;         /* 1 once the Stage-1 ext scan has been consumed */
    int parsed;                /* 1 once document_parse_ex has run (parse step done) */
    JsExtFetch *fetch;         /* jsext session (owned while prefetching) */
} RenderTask;

/* JS bridge of the LIVE page (kept across the parse→layout→done sequence
 * for click-event dispatch). The bridge is owned by the DocParseResult:
 * every teardown path closes it BEFORE document_free frees the doc (and
 * the live DOM the engine points into). */
static JsBridge *g_pageJs = NULL;

/* Close the old page's bridge + doc, then adopt the new one. Callers must
 * already hold the new doc in rt->doc. */
static void page_swap_doc(RenderTask *rt)
{
    if (rt->rewalkDoc)
    {
        /* JS-mutation re-walk: the doc was updated in place — keep it (and
         * its live engine) alive; nothing to swap. */
        currentDoc = rt->rewalkDoc;
        rt->rewalkDoc = NULL;
        g_pageJs = currentDoc->_jsbridge;
        return;
    }
    if (currentDoc)
    {
        if (currentDoc->_jsbridge)
        {
            js_doc_close(currentDoc->_jsbridge);
        }
        document_free(currentDoc);
        free(currentDoc);
    }
    currentDoc = rt->doc;
    rt->doc = NULL;
    /* RUN_KEEP pages keep their engine for click events; RUN/OFF pages
     * have no bridge here. */
    g_pageJs = currentDoc->_jsbridge;
    if (g_pageJs)
    {
        logger_log("[js] live page: listeners=%d ran=%d errs=%d",
                   jsbridge_listener_count(g_pageJs), currentDoc->jsRan,
                   currentDoc->jsErrors);
    }
    /* SW6: snapshot the finished page (walk output) so a revisit within
     * the TTL skips network + parse + engine entirely. Failures are logged
     * inside pluto_snap and never affect the live render. Toggle re-renders
     * and JS-mutation rewalks snapshot too — the walk output is the current
     * truth (toggle states / JS mutations included). */
    if (currentDoc && !currentDoc->parseError && currentUrlObj &&
        currentUrlObj->normalized[0])
    {
        unsigned long now = 0;
        uint32_t ms = 0;
        now = pd->system->getSecondsSinceEpoch(&ms);
        pluto_snap_save(currentDoc, currentUrlObj->normalized,
                        (int)currentBrowseMode, now);
        pluto_snap_lru_sweep(SNAP_MAX_ENTRIES);
    }
    /* SW8: under RAM pressure, page the largest cold DOM subtrees of the
     * live page out to the spill store. Only JS-mode pages have a live
     * DomResult (dom.h paging requires one); materialization is automatic
     * at the walker/bridge touch points. Silent no-op when comfortable. */
    if (currentDoc && !currentDoc->parseError && currentDoc->_dom)
    {
        dom_page_out_under_pressure((DomResult *)currentDoc->_dom,
                                    currentDoc->baseUrl);
    }
}

/* Live re-render after a JS DOM mutation (preventDefault path): re-walk the
 * mutated live DOM into fresh blocks/links and rebuild the layout — scripts
 * do NOT re-run (no reload), so page script state is preserved. */
static void page_rewalk_now(void);

/* Dispatch a click on the source DOM node of link #linkIndex (1-based).
 * Returns 1 when a page handler ran (re-render if it called preventDefault,
 * navigate otherwise), 0 when no JS took the event. */
static int page_handle_js_click(int linkIndex)
{
    if (!g_pageJs || !currentDoc || linkIndex <= 0 ||
        linkIndex > currentDoc->linkCount)
    {
        return 0;
    }
    void *anchorNode = currentDoc->links[linkIndex - 1]->srcNode;
    if (!anchorNode)
    {
        return 0;
    }
    int rc = jsbridge_dispatch_link_click(g_pageJs, anchorNode);
    if (rc == JSB_CLICK_NONE)
    {
        return 0;
    }
    if (rc == JSB_CLICK_SUPPRESSED)
    {
        /* The handler mutated the DOM: rebuild blocks/links from the live
         * tree (scripts keep running — no reload) and redraw. */
        page_rewalk_now();
        return 1;
    }
    /* Handler ran without preventDefault: perform the default action. */
    const LMLink *lk = lm_link_at(linkIndex);
    if (lk && lk->href)
    {
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", lk->href);
    }
    return 1;
}

/* ── JS timer pump (setTimeout / setInterval delivery) ────────────────────
 * The router owns the timer table; this is the event loop. Once per frame
 * (only while a page with a live engine is showing): fire due callbacks,
 * and when one consumed DOM budget, re-render through the SAME rewalk path
 * a preventDefault click uses (scripts keep running — no reload). All caps
 * (timers/page, fires/interval, batch/frame, nested pumps) live in the
 * router — a runaway page is contained, never a lock-up. */
static void js_timers_update(void)
{
    if (!g_pageJs || isRendering || currentState != STATE_PAGE)
    {
        return; /* no live engine / mid-render / not on a page */
    }
    unsigned nowMs = pd->system->getCurrentTimeMilliseconds();
    int mutations = 0;
    (void)jsbridge_timers_pump(g_pageJs, nowMs, &mutations);
    /* Roadmap #3: deliver settled XHR/fetch completions through the SAME
     * engine bracket + re-render path (the HTTP client was pumped earlier
     * this frame by http_update; completions it settled are pending here). */
    int xhrMutations = 0;
    (void)jsbridge_xhr_pump(g_pageJs, &xhrMutations);
    if (mutations || xhrMutations)
    {
        page_rewalk_now();
    }
}

#if defined(PLUTO_JS_TIMERS_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic end-to-end proof
 * that setTimeout/setInterval actually FIRE in the running browser (not just
 * in host tests). Auto-navigates to about:javascript at boot, then:
 *   phase 0: wait for the page + live engine, arm phase 1 (one-shot fire).
 *   phase 1: wait for the suite's 50ms one-shot to rewrite its line to
 *            "timer-fired-ok (one-shot, 50ms)" and the interval's first beat.
 *   phase 2: wait ~3.2s of frames, require the interval beat counter ≥ 3.
 * PASS/FAIL lands in pluto.log via [jstimers-autotest] lines. */
static int g_timersTestPhase = 0;
static unsigned g_timersTestFrames = 0;
static int doc_inline_contains(const char *needle)
{
    if (!currentDoc)
    {
        return 0;
    }
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk || !blk->inlines)
        {
            continue;
        }
        for (int j = 0; j < blk->inlineCount; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return 1;
            }
        }
    }
    return 0;
}
/* Diagnostics: dump every rendered line mentioning timers/interval so a
 * FAIL names what the DOM actually holds (never silent about state). */
static void doc_dump_timer_lines(void)
{
    if (!currentDoc)
    {
        return;
    }
    int dumped = 0;
    for (int i = 0; i < currentDoc->blockCount && dumped < 5; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk || !blk->inlines)
        {
            continue;
        }
        for (int j = 0; j < blk->inlineCount && dumped < 5; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->type == DOC_INLINE_TEXT && in->text &&
                (strstr(in->text, "beat") || strstr(in->text, "timer") ||
                 strstr(in->text, "fired") || strstr(in->text, "error") ||
                 strstr(in->text, "MISS") || strstr(in->text, "banner")))
            {
                logger_log("[jstimers-autotest] dom: %.90s", in->text);
                dumped++;
            }
        }
    }
    if (dumped == 0)
    {
        logger_log("[jstimers-autotest] dom: (no timer lines found)");
    }
}
static void js_timers_autotest_tick(void)
{
    if (g_timersTestPhase == 3 || isRendering)
    {
        return;
    }
    if (strncmp(currentDoc->baseUrl, "about:javascript", 16) != 0)
    {
        return;
    }
    ++g_timersTestFrames;
    if (g_timersTestPhase == 0)
    {
        if (g_timersTestFrames < 30)
        {
            return; /* settle ~0.5s */
        }
        logger_log("[jstimers-autotest] armed (page live, engine attached)");
        g_timersTestPhase = 1;
        g_timersTestFrames = 0;
        return;
    }
    if (g_timersTestPhase == 1)
    {
        if (doc_inline_contains("timer-fired-ok"))
        {
            logger_log("[jstimers-autotest] PASS-A: one-shot setTimeout "
                       "fired and re-rendered (frame %u)",
                       g_timersTestFrames);
            g_timersTestPhase = 2;
            g_timersTestFrames = 0;
        }
        else if (g_timersTestFrames > 600) /* ~10s: 50ms never came */
        {
            logger_log("[jstimers-autotest] FAIL-A: one-shot never fired");
            doc_dump_timer_lines();
            g_timersTestPhase = 3;
        }
        return;
    }
    /* Phase 2: interval — the suite clears its 400ms interval after 5
     * beats; its FINAL write ("interval beat 5", right before clearInterval)
     * persists in the DOM. Sample for that — earlier beats are overwritten
     * within 400ms and may be gone before we look. */
    if (doc_inline_contains("interval beat 5"))
    {
        logger_log("[jstimers-autotest] PASS-B: setInterval fired "
                   "repeatedly (5 beats + self-clear, frame %u)",
                   g_timersTestFrames);
        g_timersTestPhase = 3;
    }
    else if (g_timersTestFrames > 900) /* ~30s: 5 beats of 400ms never */
    {
        logger_log("[jstimers-autotest] FAIL-B: interval never completed "
                   "5 beats");
        doc_dump_timer_lines();
        g_timersTestPhase = 3;
    }
}
#endif

#if defined(PLUTO_SNAP_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic end-to-end proof
 * of the SW6 rendered-snapshot cache. Boot navigates to about:acidtest
 * (content-rich, no live JS — a legit snapshot target). This tick then:
 *   phase 0: wait for the live render (save happened in render_done;
 *            fast-path hits must still be 0 — first visit is NOT cached).
 *   phase 1: navigate to about:home (a different, never-saved page).
 *   phase 2: navigate back and require the revisit to come from the
 *            snapshot fast path (g_snapFastPathHits >= 1).
 * PASS/FAIL lands in pluto.log via [snap-autotest] lines. */
static int g_snapTestPhase = 0;
static unsigned g_snapTestFrames = 0;
static void snap_autotest_tick(void)
{
    if (g_snapTestPhase == 3 || isRendering)
    {
        return;
    }
    ++g_snapTestFrames;
    if (g_snapTestFrames < 30)
    {
        return; /* settle ~0.5s per phase */
    }
    if (g_snapTestPhase == 0)
    {
        if (currentState != STATE_PAGE || !currentDoc ||
            strncmp(currentDoc->baseUrl, "about:acidtest", 14) != 0)
        {
            if (g_snapTestFrames > 600)
            {
                logger_log("[snap-autotest] FAIL: acidtest never rendered "
                           "(state=%d)", currentState);
                g_snapTestPhase = 3;
            }
            return;
        }
        if (g_snapFastPathHits != 0)
        {
            logger_log("[snap-autotest] FAIL: fast path fired on FIRST visit "
                       "(hits=%d)", g_snapFastPathHits);
            g_snapTestPhase = 3;
            return;
        }
        logger_log("[snap-autotest] first render live (blocks=%d), "
                   "navigating home", currentDoc->blockCount);
        g_snapTestPhase = 1;
        g_snapTestFrames = 0;
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:home");
        return;
    }
    if (g_snapTestPhase == 1)
    {
        if (currentState == STATE_HOME)
        {
            logger_log("[snap-autotest] home reached, navigating back");
            g_snapTestPhase = 2;
            g_snapTestFrames = 0;
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                     "about:acidtest");
        }
        else if (g_snapTestFrames > 600)
        {
            logger_log("[snap-autotest] FAIL: never reached about:home");
            g_snapTestPhase = 3;
        }
        return;
    }
    /* Phase 2: the revisit must render from the snapshot. */
    if (g_snapFastPathHits >= 1 && currentState == STATE_PAGE && currentDoc &&
        strncmp(currentDoc->baseUrl, "about:acidtest", 14) == 0)
    {
        logger_log("[snap-autotest] PASS: revisit served from snapshot "
                   "(hits=%d, blocks=%d, title=%.40s)",
                   g_snapFastPathHits, currentDoc->blockCount, pageTitle);
        g_snapTestPhase = 3;
    }
    else if (g_snapTestFrames > 600)
    {
        logger_log("[snap-autotest] FAIL: revisit missed the snapshot "
                   "(hits=%d, state=%d)", g_snapFastPathHits, currentState);
        g_snapTestPhase = 3;
    }
}
#endif

/* Static error buffer: tasks fire onError(data) with the task's data pointer
 * (see tasks.c), so the real message travels through this side channel. */
static char g_renderErrMsg[192];

#if defined(PLUTO_PAGE_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic end-to-end proof
 * of SW8 disk-backed DOM paging. Boot forced JS on and navigated to
 * about:acidtest. This tick:
 *   phase 0: wait for the live render (JS-mode page → live DomResult),
 *            then force the RAM budget down (sim only) and call the REAL
 *            pressure policy (render_done runs it too) until stubs exist.
 *   phase 1: page_rewalk_now() — the walker touch points must
 *            transparently materialize every stub from the spill store.
 *   phase 2: require stubs==0 after rewalk, block count identical to the
 *            pre-paging render, and zero errors. Restores the budget.
 * PASS/FAIL lands in pluto.log via [page-autotest] lines. The run forces
 * the SW3a gate (budget = live + 512K) on BOTH sim and device so the
 * device run exercises the real flash I/O path deterministically. */
static int g_pageTestPhase = 0;
static unsigned g_pageTestFrames = 0;
static int g_pageTestBlocks = 0;
static unsigned long g_pageTestSavedBudget = 0;
static void page_autotest_tick(void)
{
    if (g_pageTestPhase == 3 || isRendering)
    {
        return;
    }
    ++g_pageTestFrames;
    if (g_pageTestFrames < 30)
    {
        return; /* settle ~0.5s per phase */
    }
    if (g_pageTestPhase == 0)
    {
        if (currentState != STATE_PAGE || !currentDoc || !currentDoc->_dom ||
            strncmp(currentDoc->baseUrl, "about:acidtest", 14) != 0)
        {
            if (g_pageTestFrames > 900)
            {
                logger_log("[page-autotest] FAIL: acidtest never rendered with "
                           "a live DOM (state=%d)", currentState);
                g_pageTestPhase = 3;
            }
            return;
        }
        g_pageTestBlocks = currentDoc->blockCount;
        g_pageTestSavedBudget = pluto_mem_budget();
        /* Force the SW3a gate on BOTH sim and device: budget = live + 512K
         * → headroom 512K < the 1.5MB trigger. A fresh boot never has
         * natural pressure, and the point of the device run is to prove
         * the flash I/O path (the SW6 lesson: device disk latency). The
         * boot budget is restored in phase 2 / failure paths. */
        pluto_mem_set_budget(pluto_mem_live() + 512UL * 1024UL);
        dom_page_set_subtree_floor(1024); /* acidtest subtrees are small */
        logger_log("[page-autotest] budget=%lu (live+512K), floor=1KB "
                   "(forced pressure)", pluto_mem_budget());
        int paged = dom_page_out_under_pressure(
            (DomResult *)currentDoc->_dom, currentDoc->baseUrl);
        int stubs = dom_page_stub_count((DomResult *)currentDoc->_dom);
        logger_log("[page-autotest] paged=%d stubs=%d (blocks before=%d, "
                   "live=%lu)", paged, stubs, g_pageTestBlocks,
                   pluto_mem_live());
        if (paged == 0 || stubs == 0)
        {
            logger_log("[page-autotest] FAIL: pressure policy paged nothing "
                       "(paged=%d stubs=%d)", paged, stubs);
            pluto_mem_set_budget(g_pageTestSavedBudget);
            dom_page_set_subtree_floor(0);
            g_pageTestPhase = 3;
            return;
        }
        logger_log("[page-autotest] live=%lu bytes after paging",
                   pluto_mem_live());
        g_pageTestPhase = 1;
        g_pageTestFrames = 0;
        return;
    }
    if (g_pageTestPhase == 1)
    {
        logger_log("[page-autotest] triggering rewalk (materialization "
                   "should be transparent)");
        page_rewalk_now();
        g_pageTestPhase = 2;
        g_pageTestFrames = 0;
        return;
    }
    /* Phase 2: rewalk done — everything must be back in RAM. */
    if (currentState == STATE_PAGE && currentDoc && currentDoc->_dom)
    {
        int stubs = dom_page_stub_count((DomResult *)currentDoc->_dom);
        int blocks = currentDoc->blockCount;
        pluto_mem_set_budget(g_pageTestSavedBudget); /* restore boot budget */
        dom_page_set_subtree_floor(0);
        if (stubs == 0 && blocks == g_pageTestBlocks)
        {
            logger_log("[page-autotest] PASS: rewalk materialized the paged "
                       "DOM (stubs=0, blocks=%d == %d, live=%lu)", blocks,
                       g_pageTestBlocks, pluto_mem_live());
        }
        else
        {
            logger_log("[page-autotest] FAIL: stubs=%d blocks=%d (expected "
                       "%d)", stubs, blocks, g_pageTestBlocks);
        }
        g_pageTestPhase = 3;
    }
    else if (g_pageTestFrames > 900)
    {
        logger_log("[page-autotest] FAIL: rewalk never completed");
        pluto_mem_set_budget(g_pageTestSavedBudget);
        dom_page_set_subtree_floor(0);
        g_pageTestPhase = 3;
    }
}
#endif

#if defined(PLUTO_HOME_TEST_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic probe for the
 * home-page Test Cases section. Drives the REAL STATE_HOME input path
 * (home_page_handle_input with synthesized button masks) down the reading
 * order to the first test card, presses A, and asserts the navigation
 * opened that card's about: page. PASS/FAIL lands in pluto.log via
 * [hometest-autotest] lines. */
#define HT_BTN_A (1u << 5)
static int g_homeTestPhase = 0; /* 0=drive home, 1=await page, 2=done */
static unsigned g_homeTestFrames = 0;
static int g_homeTestPresses = 0;
static void home_test_autotest_tick(void)
{
    if (g_homeTestPhase == 2 || isRendering)
    {
        return;
    }
    ++g_homeTestFrames;
    if (g_homeTestFrames < 60)
    {
        return; /* let boot + first home render settle ~1s */
    }
    if (g_homeTestPhase == 0)
    {
        if (currentState != STATE_HOME)
        {
            return;
        }
        /* Target the 3rd test card (about:acidtest, index bmCount+3):
         * card 1 is about:home, which special-cases back to STATE_HOME (not
         * a page render), so it cannot verify the navigation path. Steer
         * with the CRANK (one reading-order step per call, clamps at the
         * last selectable item — no skipped indices, always terminates). */
        int target = 1 + storage_bookmark_count() + 2;
        if (home_page_selected_index() != target && g_homeTestPresses < 40)
        {
            home_page_handle_crank(HOME_CRANK_STEP_PX);
            g_homeTestPresses++;
            return;
        }
        if (home_page_selected_index() != target)
        {
            logger_log("[hometest-autotest] FAIL: never reached test card 3 "
                       "(sel=%d target=%d)",
                       home_page_selected_index(), target);
            g_homeTestPhase = 2;
            return;
        }
        logger_log("[hometest-autotest] on test card 3 after %d crank "
                   "step(s), pressing A",
                   g_homeTestPresses);
        char *u = home_page_handle_input(HT_BTN_A, NULL);
        if (u)
        {
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", u);
            pluto_free(u);
            logger_log("[hometest-autotest] A opened a URL, awaiting page");
        }
        else
        {
            logger_log("[hometest-autotest] FAIL: A on first test card "
                       "returned no URL");
            g_homeTestPhase = 2;
        }
        g_homeTestPhase = u ? 1 : 2;
        return;
    }
    /* Phase 1: verify the navigation landed on the 3rd test page. */
    if (currentState == STATE_PAGE && currentDoc)
    {
        const HttpTestPage *pages = NULL;
        http_test_pages(&pages);
        const char *want = (pages && pages[2].name) ? pages[2].name
                                                    : "about:acidtest";
        if (currentDoc->baseUrl && strcmp(currentDoc->baseUrl, want) == 0)
        {
            logger_log("[hometest-autotest] PASS: Test Cases card opened %s "
                       "(%u frames)",
                       want, g_homeTestFrames);
        }
        else
        {
            logger_log("[hometest-autotest] FAIL: landed on %s, wanted %s",
                       currentDoc->baseUrl ? currentDoc->baseUrl : "(null)",
                       want);
        }
        g_homeTestPhase = 2;
    }
}
#endif

#if defined(PLUTO_JS_CLICK_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic repro for the
 * about:javascript "Event Details → Click me" hang. After the suite page
 * renders, dispatch the click on the page's #clickme anchor through the
 * same page_handle_js_click path the A-button uses, then verify the
 * triggered re-render actually completes (state returns to PAGE, not stuck
 * at 60%). PASS/FAIL lands in pluto.log via [jsclick-autotest] lines. */
static int g_jsClickTestPhase = 0; /* 0=waiting for page, 1=clicked, 2=done */
static unsigned g_jsClickTestFrames = 0;
#ifdef PLUTO_JS_CLICK_AUTOTEST_OFF
/* Optional: force the JS setting Off for a negative (no-engine) control run. */
#define PLUTO_JS_CLICK_AUTOTEST_FORCE_OFF 1
#endif
static void js_click_autotest_tick(void)
{
    if (g_jsClickTestPhase == 2 || isRendering)
    {
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc)
    {
        return;
    }
    int onSuite = strncmp(currentDoc->baseUrl, "about:javascript", 16) == 0;
    if (g_jsClickTestPhase == 0)
    {
        if (!onSuite)
        {
            return;
        }
        if (++g_jsClickTestFrames < 30)
        {
            return; /* let the page settle ~0.5s */
        }
        int idx = 0;
        for (int i = 0; i < currentDoc->linkCount; i++)
        {
            const char *h = currentDoc->links[i]->href;
            if (h && strcmp(h, "https://example.com/blocked") == 0)
            {
                idx = i + 1;
                break;
            }
        }
        if (idx <= 0)
        {
            logger_log("[jsclick-autotest] FAIL: #clickme link not found");
            g_jsClickTestPhase = 2;
            return;
        }
        logger_log("[jsclick-autotest] clicking Event demo link %d", idx);
        int rc = page_handle_js_click(idx);
        logger_log("[jsclick-autotest] dispatch rc=%d (1=handler ran)", rc);
        g_jsClickTestPhase = (rc == 1) ? 1 : 2;
        if (g_jsClickTestPhase == 2)
        {
#ifdef PLUTO_JS_CLICK_AUTOTEST_FORCE_OFF
            logger_log("[jsclick-autotest] OK: no handler ran (JS Off "
                       "control behaved as expected)");
#else
            logger_log("[jsclick-autotest] FAIL: click handler did not run");
#endif
        }
        return;
    }
    /* Phase 1: the click-triggered re-render landed back on the page. */
    ++g_jsClickTestFrames;
    if (!onSuite)
    {
        logger_log("[jsclick-autotest] FAIL: navigated away (unexpected "
                   "default action)");
        g_jsClickTestPhase = 2;
        return;
    }
    logger_log("[jsclick-autotest] PASS: re-render completed, %u frames "
               "since page load",
               g_jsClickTestFrames);
    g_jsClickTestPhase = 2;
}
#endif

#if defined(PLUTO_FIELDTEST_AUTOTEST)
/* SW0 BENCHMARK MATRIX (SIMULATOR-ONLY): reads tests/fieldtest_urls.txt from the
 * project root — one site per line: "<engine 0|1|2> <url> [| <criterion>]".
 * The criterion is a keyword that MUST appear in the rendered document text
 * (case-insensitive; scanned across blocks AND table cells). PASS/FAIL is
 * logged per site and tallied; the final line is the matrix score:
 *   [fieldtest] done (N sites): PASS p / FAIL f — matrix p/N
 * Lines without a criterion PASS if any text renders (with a warning).
 * One retry per site on a network-error page (transient sim -16 flake).
 * Never defined for device builds. */
#define FIELDTEST_MAX_SITES 24
#define FIELDTEST_EXPECTED_SITES 20 /* resume only if the list matches */
#define FIELDTEST_FRAMES_PER_SITE 450 /* 15s @30fps */
static char ftUrl[FIELDTEST_MAX_SITES][512];
static int ftEngine[FIELDTEST_MAX_SITES];
static char ftCrit[FIELDTEST_MAX_SITES][128];
static unsigned char ftRetried[FIELDTEST_MAX_SITES];
static int ftCount = 0;
static int ftIndex = -1;
static unsigned ftFrames = 0;
static int ftPhase = 0; /* 0=boot-wait, 1=running, 2=done */
static int ftPass = 0, ftFail = 0;
/* Checkpoint file: macOS's AppKit automatic termination can kill the sim
 * mid-matrix (long idle stretches); state persists so a supervisor relaunch
 * RESUMES instead of restarting - the run stays one logical session. */
#define FIELDTEST_STATE_PATH \
    "/Users/bwandrych/Developer/PlaydateSDK/Disk/Data/" \
    "com.bryanwandrych.plutobrowser/fieldtest_state.txt"
static void fieldtest_state_save(void)
{
    FILE *f = fopen(FIELDTEST_STATE_PATH, "w");
    if (!f)
    {
        return;
    }
    fprintf(f, "%d %d %d %d\n", ftCount, ftIndex, ftPass, ftFail);
    fclose(f);
}
static int fieldtest_state_load(void)
{
    FILE *f = fopen(FIELDTEST_STATE_PATH, "r");
    if (!f)
    {
        return 0;
    }
    int count = 0, index = -1, pass = 0, fail = 0;
    if (fscanf(f, "%d %d %d %d", &count, &index, &pass, &fail) != 4 ||
        count != FIELDTEST_EXPECTED_SITES || index < 0 ||
        index >= FIELDTEST_MAX_SITES)
    {
        fclose(f);
        return 0;
    }
    fclose(f);
    ftIndex = index;
    ftPass = pass;
    ftFail = fail;
    return 1;
}
static void fieldtest_state_clear(void)
{
    remove(FIELDTEST_STATE_PATH);
}
/* Append-mode history: pluto.log truncates at boot, but a matrix now spans
 * multiple sim sessions (checkpoint resume) — this file survives and
 * accumulates every per-site verdict across relaunches. */
#define FIELDTEST_HIST_PATH \
    "/Users/bwandrych/Developer/PlaydateSDK/Disk/Data/" \
    "com.bryanwandrych.plutobrowser/fieldtest_history.txt"
static void fieldtest_hist_log(const char *verdict, const char *url)
{
    FILE *f = fopen(FIELDTEST_HIST_PATH, "a");
    if (!f)
    {
        return;
    }
    fprintf(f, "%s %s\n", verdict, url);
    fclose(f);
}
static void fieldtest_lcase(char *s)
{
    for (; *s; s++)
    {
        if (*s >= 'A' && *s <= 'Z')
        {
            *s += 32;
        }
    }
}
static int fieldtest_text_hit(const char *text, const char *needle)
{
    char hay[256];
    snprintf(hay, sizeof(hay), "%s", text);
    fieldtest_lcase(hay);
    return strstr(hay, needle) != NULL;
}
static int fieldtest_dom_contains(const char *needle)
{
    if (!currentDoc)
    {
        return 0;
    }
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk)
        {
            continue;
        }
        if (blk->inlines)
        {
            for (int j = 0; j < blk->inlineCount; j++)
            {
                const DocInline *in = blk->inlines[j];
                if (in && in->type == DOC_INLINE_TEXT && in->text &&
                    in->text[0] && fieldtest_text_hit(in->text, needle))
                {
                    return 1;
                }
            }
        }
        if (blk->type == DOC_BLOCK_TABLE && blk->table)
        {
            const DocTable *t = blk->table;
            for (int r = 0; r < t->rowCount; r++)
            {
                const DocRow *row = t->rows[r];
                if (!row)
                {
                    continue;
                }
                for (int c = 0; c < row->cellCount; c++)
                {
                    const DocCell *cell = row->cells[c];
                    if (!cell)
                    {
                        continue;
                    }
                    for (int k = 0; k < cell->inlineCount; k++)
                    {
                        const DocInline *in = cell->inlines[k];
                        if (in && in->type == DOC_INLINE_TEXT && in->text &&
                            in->text[0] && fieldtest_text_hit(in->text, needle))
                        {
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
static int fieldtest_any_text(void)
{
    if (!currentDoc)
    {
        return 0;
    }
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk || !blk->inlines)
        {
            continue;
        }
        for (int j = 0; j < blk->inlineCount; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->type == DOC_INLINE_TEXT && in->text && in->text[0])
            {
                return 1;
            }
        }
    }
    return 0;
}
static void fieldtest_load_list(void)
{
    FILE *f = fopen("/Users/bwandrych/Desktop/PlutoBrowser/tests/fieldtest_urls.txt", "r");
    if (!f)
    {
        logger_log("[fieldtest] no fieldtest_urls.txt — seam idle");
        ftPhase = 2;
        return;
    }
    char line[768];
    while (ftCount < FIELDTEST_MAX_SITES && fgets(line, sizeof(line), f))
    {
        int eng = 0;
        char url[512];
        url[0] = '\0';
        if (sscanf(line, "%d %511[^|\r\n]", &eng, url) >= 2 && url[0] &&
            (url[0] == 'h' || url[0] == 'a'))
        {
            /* trim trailing spaces from the URL */
            size_t len = strlen(url);
            while (len > 0 && (url[len - 1] == ' ' || url[len - 1] == '\t'))
            {
                url[--len] = '\0';
            }
            char crit[128] = "";
            const char *bar = strchr(line, '|');
            if (bar)
            {
                sscanf(bar + 1, " %127[^\r\n]", crit);
            }
            snprintf(ftUrl[ftCount], sizeof(ftUrl[0]), "%s", url);
            ftEngine[ftCount] = (eng >= 0 && eng <= 3) ? eng : 0; /* 3 = XS */
            snprintf(ftCrit[ftCount], sizeof(ftCrit[0]), "%s", crit);
            ftCount++;
        }
    }
    fclose(f);
    logger_log("[fieldtest] loaded %d sites", ftCount);
    if (ftCount == 0)
    {
        ftPhase = 2;
        return;
    }
    /* Resume support: a previous sim session may have died mid-matrix
     * (macOS automatic termination). Its checkpoint seeds where to start. */
    fieldtest_state_load();
}
static void fieldtest_dump_page(void)
{
    if (!currentDoc)
    {
        logger_log("[fieldtest] dom: (no document)");
        return;
    }
    int lines = 0;
    for (int i = 0; i < currentDoc->blockCount && lines < 8; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk || !blk->inlines)
        {
            continue;
        }
        for (int j = 0; j < blk->inlineCount && lines < 8; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->type == DOC_INLINE_TEXT && in->text && in->text[0])
            {
                logger_log("[fieldtest] dom: %.70s", in->text);
                lines++;
            }
        }
    }
    if (lines == 0)
    {
        logger_log("[fieldtest] dom: (empty render)");
    }
}
static void fieldtest_score_page(void)
{
    if (ftCrit[ftIndex][0])
    {
        char needle[128];
        snprintf(needle, sizeof(needle), "%s", ftCrit[ftIndex]);
        fieldtest_lcase(needle);
        int ok = fieldtest_dom_contains(needle);
        logger_log("[fieldtest] RESULT: %s site=%s crit=%s",
                   ok ? "PASS" : "FAIL", ftUrl[ftIndex], ftCrit[ftIndex]);
        fieldtest_hist_log(ok ? "PASS" : "FAIL", ftUrl[ftIndex]);
        if (ok)
        {
            ftPass++;
        }
        else
        {
            ftFail++;
        }
    }
    else if (fieldtest_any_text())
    {
        logger_log("[fieldtest] RESULT: PASS (no criterion) site=%s",
                   ftUrl[ftIndex]);
        fieldtest_hist_log("PASS", ftUrl[ftIndex]);
        ftPass++;
    }
    else
    {
        logger_log("[fieldtest] RESULT: FAIL (empty render) site=%s",
                   ftUrl[ftIndex]);
        fieldtest_hist_log("FAIL", ftUrl[ftIndex]);
        ftFail++;
    }
}
static void fieldtest_navigate_current(void)
{
    storage_set_setting_int("jsEngine", ftEngine[ftIndex]);
    jsbridge_set_engine(ftEngine[ftIndex]);
    /* Measure the LIVE pipeline exactly like the SW0 baseline: the SW6
     * snapshot fast path would serve repeat visits from cache and change
     * what we're scoring (network/parse/engine outcome). Speed gains from
     * the cache are a separate measurement. */
    snapBypass = 1;
    ftFrames = 0;
    if (ftCrit[ftIndex][0])
    {
        logger_log("[fieldtest] === site %d/%d engine=%d %s | %s", ftIndex + 1,
                   ftCount, ftEngine[ftIndex], ftUrl[ftIndex], ftCrit[ftIndex]);
    }
    else
    {
        logger_log("[fieldtest] === site %d/%d engine=%d %s", ftIndex + 1,
                   ftCount, ftEngine[ftIndex], ftUrl[ftIndex]);
    }
    navigate_to(ftUrl[ftIndex]);
}
static void fieldtest_tick(void)
{
    if (ftPhase == 2)
    {
        return;
    }
    ftFrames++;
    if (ftPhase == 0)
    {
        if (ftFrames < 20)
        {
            return;
        }
        fieldtest_load_list();
        if (ftPhase == 2)
        {
            return;
        }
        storage_set_setting_int("jsEnabled", 2); /* Full: external scripts */
        if (ftIndex < 0)
        {
            /* fresh session: start at site 0 (fieldtest_state_load may have
             * already set ftIndex/ftPass/ftFail for a resumed session) */
            ftIndex = 0;
        }
        else
        {
            logger_log("[fieldtest] RESUMED at site %d/%d (PASS %d / FAIL %d)",
                       ftIndex + 1, ftCount, ftPass, ftFail);
        }
        ftPhase = 1;
        fieldtest_navigate_current();
        return;
    }
    /* phase 1: spend FIELDTEST_FRAMES_PER_SITE on each site */
    if (ftFrames >= FIELDTEST_FRAMES_PER_SITE)
    {
        if (currentState == STATE_ERROR && !ftRetried[ftIndex])
        {
            /* transient sim network flake (-16): one retry per site */
            ftRetried[ftIndex] = 1;
            logger_log("[fieldtest] retry (network error) %s", ftUrl[ftIndex]);
            fieldtest_navigate_current();
            return;
        }
        logger_log("[fieldtest] --- snapshot %s", ftUrl[ftIndex]);
        fieldtest_dump_page();
        fieldtest_score_page();
        ftIndex++;
        if (ftIndex >= ftCount)
        {
            logger_log("[fieldtest] done (%d sites): PASS %d / FAIL %d — "
                       "matrix %d/%d",
                       ftCount, ftPass, ftFail, ftPass, ftCount);
            fieldtest_state_clear();
            ftPhase = 2;
            return;
        }
        fieldtest_state_save();
        fieldtest_navigate_current();
    }
}
#endif /* PLUTO_FIELDTEST_AUTOTEST */

#if defined(PLUTO_CSS_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic proof that the
 * roadmap-#2 CSS engine applies <style> rules in the RUNNING browser
 * (static styling needs no JS; the JS-suite line only reports DOM checks).
 * PASS/FAIL lands in pluto.log via [css-autotest] lines. */
static int g_cssTestPhase = 0; /* 0=waiting, 1=done */
static unsigned g_cssTestFrames = 0;
static int css_doc_inline_contains(const char *needle)
{
    if (!currentDoc)
    {
        return 0;
    }
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (!blk || !blk->inlines)
        {
            continue;
        }
        for (int j = 0; j < blk->inlineCount; j++)
        {
            const DocInline *in = blk->inlines[j];
            if (in && in->type == DOC_INLINE_TEXT && in->text &&
                strstr(in->text, needle))
            {
                return 1;
            }
        }
    }
    return 0;
}
static int css_doc_any_hidden_leak(void)
{
    return css_doc_inline_contains("HIDDEN-BY-CSS") ? 1 : 0;
}
static int css_doc_center_count(void)
{
    int n = 0;
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (blk && blk->align && strcmp(blk->align, "center") == 0)
        {
            n++;
        }
    }
    return n;
}
static int css_doc_invert_count(void)
{
    int n = 0;
    for (int i = 0; i < currentDoc->blockCount; i++)
    {
        const DocBlock *blk = currentDoc->blocks[i];
        if (blk && blk->invert)
        {
            n++;
        }
    }
    return n;
}
static void css_autotest_tick(void)
{
    if (g_cssTestPhase == 1 || isRendering)
    {
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc)
    {
        return;
    }
    if (strncmp(currentDoc->baseUrl, "about:javascript", 16) != 0)
    {
        return;
    }
    ++g_cssTestFrames;
    if (g_cssTestFrames < 30)
    {
        return; /* settle ~0.5s */
    }
    int hidden = css_doc_any_hidden_leak();
    int centered = css_doc_center_count();
    int inverted = css_doc_invert_count();
    int bold = css_doc_inline_contains("CSS bold text");
    if (!hidden && centered >= 1 && inverted >= 1 && bold)
    {
        logger_log("[css-autotest] PASS: display:none hidden (no leak), "
                   "%d centered, %d inverted, bold applied (frame %u)",
                   centered, inverted, g_cssTestFrames);
    }
    else
    {
        logger_log("[css-autotest] FAIL: hiddenLeak=%d centered=%d "
                   "inverted=%d bold=%d (frame %u)",
                   hidden, centered, inverted, bold, g_cssTestFrames);
    }
    g_cssTestPhase = 1;
}
#endif

#if defined(PLUTO_NAV_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): deterministic repro for the
 * google.com navigation crash. Boots to the home page, presses A on the
 * Google speed-dial card through the REAL home-page input path, then
 * watches the navigation complete: state 2 (page rendered) with a live doc
 * is PASS; an error page for google.com FAILs. Verifies the OS is still
 * alive 10s later (the device errorlog overflow killed the task ~3s after
 * the nav). PASS/FAIL lands in pluto.log via [nav-autotest] lines. */
static int g_navTestPhase = 0; /* 0=home, 1=await page, 2=stable check, 3=done */
static unsigned g_navTestFrames = 0;
static void nav_autotest_tick(void)
{
    if (g_navTestPhase == 3 || isRendering)
    {
        return;
    }
    ++g_navTestFrames;
    if (g_navTestFrames < 60)
    {
        return; /* let boot + first home render settle ~1s */
    }
    if (g_navTestPhase == 0)
    {
        if (currentState != STATE_HOME)
        {
            return;
        }
        /* The Google speed-dial card is bookmark #2 (index 2): Storage
         * order is Bitmap Gallery(1), Google(2), ... Steer with crank. */
        int target = 2;
        if (home_page_selected_index() != target)
        {
            home_page_handle_crank(HOME_CRANK_STEP_PX);
            return;
        }
        logger_log("[nav-autotest] on Google card, pressing A");
        char *u = home_page_handle_input(1u << 5, NULL);
        if (u)
        {
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", u);
            pluto_free(u);
            g_navTestPhase = 1;
        }
        else
        {
            logger_log("[nav-autotest] FAIL: A on Google card returned no URL");
            g_navTestPhase = 3;
        }
        return;
    }
    if (g_navTestPhase == 1)
    {
        if (currentState == STATE_PAGE && currentDoc)
        {
            logger_log("[nav-autotest] PASS: google.com rendered (title=%.40s, "
                       "%u frames)", pageTitle, g_navTestFrames);
            g_navTestPhase = 2;
            g_navTestFrames = 0;
        }
        else if (currentState == STATE_ERROR)
        {
            logger_log("[nav-autotest] FAIL: error page for google.com");
            g_navTestPhase = 3;
        }
        else if (g_navTestFrames > 60 * 30)
        {
            logger_log("[nav-autotest] FAIL: navigation never completed "
                       "(state=%d)", (int)currentState);
            g_navTestPhase = 3;
        }
        return;
    }
    /* Phase 2: hold 10s of live frames — proves the game task survived the
     * JS-heavy page (the device overflow killed the task seconds after nav). */
    if (g_navTestFrames > 60 * 10)
    {
        logger_log("[nav-autotest] PASS: task alive 10s after render");
        g_navTestPhase = 3;
    }
}
#endif

static int render_step(TaskCtx *ctx)
{
    RenderTask *rt = (RenderTask *)ctx->data;

    /* ── JS-mutation re-walk path: rebuild blocks/links from the live tree ── */
    if (rt->rewalkDoc && !rt->parsed)
    {
        logger_log("render: rewalk start (live DOM mutation re-render)");
        int wrc = document_rewalk(rt->rewalkDoc);
        if (wrc != 0)
        {
            snprintf(g_renderErrMsg, sizeof(g_renderErrMsg),
                     "Render Error: rewalk out of memory");
            return -1;
        }
        logger_log("render: rewalk done blocks=%d links=%d",
                   rt->rewalkDoc->blockCount, rt->rewalkDoc->linkCount);
        /* Mark parsed so this branch runs exactly ONCE: the next task
         * re-entry must fall through to the layout step below. Without this
         * gate the task rewalked + yielded FOREVER — the loading screen
         * stuck at 60% (about:javascript "Click me" bug). rewalkDoc stays
         * set: page_swap_doc routes on it to keep the doc + live engine
         * alive, and the layout step below reads it. */
        rt->parsed = 1;
        tasks_report_progress(0.6f);
        return 1; /* more work: layout next tick */
    }

    if (!rt->parsed)
    {
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
        }

        /* ── Stage 2 (FULL): pump the prefetch session before parsing ────
         * Each entry pumps/downloads one tick; while any file is in flight
         * the task yields so the frame loop can call http_update(). When all
         * files settle, the session detaches its arena + ext table into
         * rt->doc and the SAME step falls through to the parse below. */
        if (rt->prefetching && rt->fetch)
        {
            if (jsext_prefetch_step(rt->fetch) != 0)
            {
                return 1; /* still downloading (http_update pumps per frame) */
            }
            rt->prefetching = 0;
            JsExtArena *arena = NULL;
            JsExtScript *ext = NULL;
            int extCount = 0;
            jsext_fetch_detach(rt->fetch, &arena, &ext, &extCount);
            rt->fetch = NULL;
            rt->doc->extScripts = ext;
            rt->doc->extScriptCount = extCount;
            rt->doc->_extArena = arena;
            logger_log("[jsext] prefetch done: %d file(s), %zu bytes",
                       extCount, jsext_last_bytes());
        }

        /* detailsOpen overrides for toggle re-renders. */
        DocParseOpts opts;
        memset(&opts, 0, sizeof(opts));
        if (rt->isToggle && detailsKeyCount > 0)
        {
            opts.detailsOpen = detailsOpenState;
            opts.detailsOpenCount = detailsKeyCount;
        }
        opts.svgDecoder = app_svg_decoder;

        /* JavaScript execution policy: Off → DOC_SCRIPT_OFF (no engine ever
         * created); Inline → RUN_KEEP (inline scripts only, files skipped —
         * the historical On path); Full → the same engine wiring PLUS the
         * prefetch chain below downloads external <script src> files, which
         * then execute in document order like inline bodies. HTML mode only
         * in every case — reader mode distills the page first. */
        DocScriptPolicy jsPolicy = DOC_SCRIPT_OFF;
        int jsSetting = storage_setting_int("jsEnabled");
        if (jsSetting == 1 && currentBrowseMode == MODE_RAW_HTML)
        {
            jsPolicy = DOC_SCRIPT_RUN_KEEP;
        }
        else if (jsSetting == 2 && currentBrowseMode == MODE_RAW_HTML)
        {
            jsPolicy = DOC_SCRIPT_FULL;
        }

        /* Full mode (Stage 1): scan the page for external script files and
         * provide them BEFORE parsing (browser-preload-scanner model — a
         * desktop browser will not run a file-based script before its bytes
         * arrive; Stage 2 above pumps the session across task re-entries).
         *   Network pages: sequential fetches through the single-flight HTTP
         * client (same machinery as the page body and images).
         *   about: pages: scripts resolve LOCALLY from jsext's built-in table
         * (deterministic tests 6–11 with zero network dependence). */
        if (jsPolicy == DOC_SCRIPT_FULL && rt->body && !rt->parseExtedDoc)
        {
            /* Consumed immediately: the fetch path returns before falling
             * through, and Stage 2 (top of step) must not re-run the scan. */
            rt->parseExtedDoc = 1;
            int isAbout = (strncmp(rt->url, "about:", 6) == 0);
            JsScriptSlot *slots = NULL;
            JsExtScript *ext = NULL;
            int slotCount = 0, extCount = 0;
            JsExtArena *arena = NULL;
            jsext_collect(rt->body, rt->url, &slots, &slotCount, &ext,
                          &extCount, &arena);
            if (extCount > 0)
            {
                logger_log("[jsext] %d external script(s) on page", extCount);
                if (isAbout)
                {
                    jsext_local_fill(arena, ext, extCount);
                    rt->doc->extScripts = ext;
                    rt->doc->extScriptCount = extCount;
                    rt->doc->_extArena = arena;
                }
                else
                {
                    rt->fetch = jsext_prefetch_begin(arena, slots, slotCount,
                                                     ext, extCount);
                    if (rt->fetch)
                    {
                        rt->prefetching = 1;
                        return 1; /* downloads pump on re-entry (Stage 2) */
                    }
                    jsext_arena_free(arena); /* begin failed */
                }
            }
            else if (arena)
            {
                jsext_arena_free(arena); /* no externals: drop scratch */
            }
        }

        int rc = document_parse_ex(rt->body, rt->url, currentBrowseMode,
                                   rt->isToggle ? &opts : NULL, jsPolicy, NULL,
                                   rt->doc);
        if (rc != 0 || rt->doc->parseError)
        {
            snprintf(g_renderErrMsg, sizeof(g_renderErrMsg),
                     "Parse Error: parse failed");
            return -1;
        }
        if (rt->doc->jsRan || rt->doc->jsErrors)
        {
            logger_log("[js] page: ran=%d errs=%d%s%s", rt->doc->jsRan,
                       rt->doc->jsErrors,
                       rt->doc->jsLastError[0] ? " last=" : "",
                       rt->doc->jsLastError[0] ? rt->doc->jsLastError : "");
        }
        rt->parsed = 1;
        tasks_report_progress(0.6f);
        return 1; /* more work: layout next tick */
    }

    /* Layout step. The JS-mutation re-walk path keeps its (already-parsed)
     * doc in rewalkDoc — page_swap_doc still routes on rewalkDoc to keep the
     * doc + live engine alive, so the layout must build from THAT doc. */
    layout_build(rt->rewalkDoc ? rt->rewalkDoc : rt->doc);
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
    int wasRewalk = (rt->rewalkDoc != NULL); /* read before page_swap_doc */

    /* Free the previous doc AFTER the new build (layout borrows strings). */
    page_swap_doc(rt);

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

    /* Auto-redirect for <meta http-equiv="refresh"> (fresh loads only — a
     * JS-mutation re-walk must not re-arm an already-scheduled redirect). */
    if (!wasRewalk && currentDoc && currentDoc->metaRefresh.present && currentDoc->metaRefresh.delay >= 0)
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
        /* FULL-mode prefetch mid-flight: abort the session (cancels any
         * in-flight script request + frees the arena) before tearing down. */
        jsext_abort_active();
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
            if (rt->doc->_jsbridge)
            {
                js_doc_close(rt->doc->_jsbridge); /* engine dies with the doc */
            }
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
    jsext_abort_active(); /* FULL-mode prefetch: kill any in-flight session */
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

    /* ── SW6 snapshot fast path: a fresh, valid snapshot for THIS url in
     * THIS view mode renders from the cached walk output — no network, no
     * parse, no engine. The restored doc has no live DOM (links navigate,
     * clicks degrade gracefully) and rawHtml is absent (view-mode flip
     * re-navigates through the full pipeline). A bypass (hard reload) or
     * any miss/expiry falls through to the classic fetch path. */
    {
        unsigned long now = 0;
        uint32_t ms = 0;
        now = pd->system->getSecondsSinceEpoch(&ms);
        DocParseResult *snap = snapBypass ? NULL
            : pluto_snap_load(urlBuf, (int)currentBrowseMode, now,
                              SNAP_TTL_SECONDS);
        snapBypass = 0;
        if (snap)
        {
            ++g_snapFastPathHits;
            logger_log("[snap] fast path: %s", urlBuf);
            /* Tear down the old page exactly as render_done would have. */
            layout_clear();
            if (currentDoc)
            {
                if (currentDoc->_jsbridge)
                {
                    js_doc_close(currentDoc->_jsbridge);
                }
                document_free(currentDoc);
                free(currentDoc);
            }
            currentDoc = snap;
            g_pageJs = NULL;
            snprintf(pageTitle, sizeof(pageTitle), "%s",
                     snap->title[0] ? snap->title
                                    : (currentUrlObj->host[0] ? currentUrlObj->host
                                                              : "Web Page"));
            currentState = STATE_PAGE;
            scrollY = 0;
            targetScrollY = 0;
            crankVelocity = 0;
            layout_build(snap);
            if (!navigatingHistory)
            {
                push_history(urlBuf);
            }
            navigatingHistory = 0;
            update_system_menu();
            return;
        }
    }

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
    logger_log("[net] http error: %s", message ? message : "(null)");
    error_page_show(message, currentUrlObj ? currentUrlObj->normalized : "");
    currentState = STATE_ERROR;
    snprintf(pageTitle, sizeof(pageTitle), "Connection Error");
    update_system_menu();
}

/* ── Live re-walk after JS DOM mutation (preventDefault path) ───────────── */
static void page_rewalk_now(void)
{
    if (!currentDoc || isRendering)
    {
        return;
    }
    int prevScroll = scrollY;
    int prevTarget = targetScrollY;
    isRendering = 1;
    snprintf(pageTitle, sizeof(pageTitle), "Rendering...");
    currentState = STATE_LOADING;

    RenderTask *rt = (RenderTask *)calloc(1, sizeof(RenderTask));
    if (!rt)
    {
        isRendering = 0;
        return;
    }
    rt->rewalkDoc = currentDoc; /* re-walk the LIVE tree, do not reload */
    rt->addToHistory = 0;
    rt->isToggle = 1; /* restore scroll position */
    rt->prevScroll = prevScroll;
    rt->prevTarget = prevTarget;
    tasks_run(render_step, rt, render_done, render_error, rt);
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
/* Form-submit scratch hoisted to BSS: action[512] + seen[32][64] made
 * submit_form a 3.3KB game-task frame. Single-threaded: safe to share. */
static char g_formAction[512];
static char g_formSeen[32][64];

static void submit_form(const char *formAction, const LayoutItem *inputBlock)
{
    char *action = g_formAction;
    if (!formAction || !formAction[0])
    {
        snprintf(action, 512, "%s",
                 currentUrlObj ? currentUrlObj->normalized : "");
    }
    else
    {
        snprintf(action, 512, "%s", formAction);
    }
    if (strcmp(action, "#") == 0)
    {
        return;
    }

    StrBuf query;
    strbuf_init(&query);
    int pairCount = 0;

    /* First-seen name dedupe (Lua `seen` table). */
    char (*seen)[64] = g_formSeen;
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
/* ── FPS overlay (Beta: user request) ──────────────────────────────────── */
static int g_showFps = 0;              /* cached setting; re-read on boot/save */
static unsigned int g_fpsFrames = 0;   /* frames since last sample window */
static unsigned int g_fpsWindowStart = 0; /* ms timestamp of window start */
static int g_fpsValue = 0;             /* last computed frames-per-second */
static unsigned int g_fpsLastSampleMs = 0;
static LCDFont *g_fpsFont = NULL;      /* bold body font, loaded once */

/* Draw a small bold FPS number flush in the bottom-right corner.
 * Runs AFTER every state's draw, in its own full-screen context with no
 * clip, so nothing can clip it out (the visibility bug from the first
 * attempt). 1px white backing keeps it readable over dark content. */
static void draw_fps_overlay(void)
{
    if (!g_showFps || !g_fpsFont)
    {
        return;
    }
    PlaydateAPI *pd = pluto_pd();
    char num[8];
    snprintf(num, sizeof(num), "%d", g_fpsValue);
    pd->graphics->pushContext(NULL);
    pd->graphics->clearClipRect();
    pd->graphics->setFont(g_fpsFont);
    int w = pd->graphics->getTextWidth(g_fpsFont, num, strlen(num), kUTF8Encoding, 0);
    int h = pd->graphics->getFontHeight(g_fpsFont);
    int x = LCD_COLUMNS - w;          /* flush right */
    int y = LCD_ROWS - h;             /* flush bottom, touching the border */
    pd->graphics->fillRect(x - 1, y, w + 1, h, kColorWhite);
    pd->graphics->setDrawMode(kDrawModeCopy);
    pd->graphics->drawText(num, strlen(num), kUTF8Encoding, x, y);
    pd->graphics->popContext();
}

/* ── Display FPS (Playdate: 30 fps default, 50 fps max per SDK docs) ───── */
/* Apply the displayFps setting to the OS display + the keyboard component's
 * key-repeat timing (which is frame-count based). Called once at boot and
 * again whenever settings are saved. */
static void apply_display_fps(void)
{
    int fps = storage_setting_int("displayFps");
    if (fps != 50)
    {
        fps = 30; /* only 30 or 50 are valid; 30 is the OS default */
    }
    pd->display->setRefreshRate((float)fps);
    if (g_kb)
    {
        keyboardApi.setRefreshRate(g_kb, (float)fps);
    }
    logger_log("DISPLAY: refresh rate set to %d fps", fps);
}

/* Engine selection lives in the router (jsbridge_set_engine); this helper
 * only names it for logs (0=muJS, 1=Duktape, 2=QuickJS, 3=XS). */
static const char *engine_name(void)
{
    int e = storage_setting_int("jsEngine");
    return e == 1   ? "Duktape"
           : e == 2 ? "QuickJS"
           : e == 3 ? "XS (Moddable)"
                    : "muJS";
}

static void settings_on_change(void)
{
    currentBrowseMode = storage_setting_int("mode");
    g_showFps = storage_setting_int("showFps");
    apply_display_fps();
    /* Route all subsequent page loads to the selected JS engine (0=muJS,
     * 1=Duktape). Pages never mix engines: the chosen one runs everything. */
    jsbridge_set_engine(storage_setting_int("jsEngine"));
    snapBypass = 1; /* settings may change rendering: reload, don't replay */
    logger_log("[jsbridge] engine selected: %s", engine_name());
    if (currentBrowseMode != MODE_READER && currentBrowseMode != MODE_RAW_HTML)
    {
        currentBrowseMode = MODE_READER;
    }
    /* User request: saving from a website RELOADS the current page (fresh
     * fetch + re-render with the new settings) instead of re-rendering in
     * place. No page open -> nothing to reload. */
    if (currentUrlObj && currentUrlObj->normalized[0] &&
        strcmp(currentUrlObj->normalized, "about:home") != 0)
    {
        navigate_to(currentUrlObj->normalized);
    }
}




static int updateFrame(void *userdata)
{
    (void)userdata;
    frameCount++;

    /* Stack high-water sample (frame-level floor; deep callees touch too). */
    logger_stack_touch();

    /* Clear the full framebuffer every frame (was dropped accidentally during
     * the BTEST scaffolding removal — without it, home-page scrolling smears
     * previous frames' content across the screen). */
    pd->graphics->clear((LCDColor)kColorWhite);

    /* ── button state (current/pushed/released) ── */
    unsigned int btnCurrent = 0, btnPushed = 0, btnReleased = 0;
    pd->system->getButtonState((PDButtons *)&btnCurrent, (PDButtons *)&btnPushed,
                               (PDButtons *)&btnReleased);
    /* ── KBGATE (Bug Fix #6): while a keyboard owns input (address bar or
     * form field), the app must not react to buttons AT ALL — the keyboard
     * reads the hardware itself, so an A press meant for typing previously
     * also launched bookmarks/links/cursor actions underneath. Zero the
     * app-visible button state; the keyboard's own button reader is
     * unaffected. This holds for EVERY app state (home, page, …). */
    if (formKeyboardOpen || address_bar_is_open() ||
        (g_kb && keyboardApi.isVisible(g_kb)))
    {
        btnCurrent = 0;
        btnPushed = 0;
        btnReleased = 0;
    }

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

#if defined(PLUTO_JS_CLICK_AUTOTEST)
    js_click_autotest_tick();
#endif

#if defined(PLUTO_JS_TIMERS_AUTOTEST)
    js_timers_autotest_tick();
#endif

#if defined(PLUTO_SNAP_AUTOTEST)
    snap_autotest_tick();
#endif

#if defined(PLUTO_PAGE_AUTOTEST)
    page_autotest_tick();
#endif

#if defined(PLUTO_CSS_AUTOTEST)
    css_autotest_tick();
#endif

#if defined(PLUTO_FIELDTEST_AUTOTEST)
    fieldtest_tick();
#endif

#if defined(PLUTO_NAV_AUTOTEST)
    nav_autotest_tick();
#endif

#if defined(PLUTO_HOME_TEST_AUTOTEST)
    home_test_autotest_tick();
#endif

    /* ── crank velocity physics (Lua parity) ── */
    {
        int keyboardActive = formKeyboardOpen || address_bar_is_open();
        if (currentState == STATE_SETTINGS)
        {
            /* Settings panel consumes the crank entirely: it scrolls the
             * settings list; the background page/home must not receive any
             * motion (no velocity, no link-selection clear). */
            settings_page_apply_crank(
                storage_setting_int("invertCrank") ? -crankChange : crankChange);
            crankChange = 0.0f;
            crankVelocity = 0.0f;
        }
        else if (keyboardActive)
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

    /* Reset per-frame flags (Lua: Layout.onDemandConsumed = false each
     * updateFrame). Without this the first on-demand overlay action blocks
     * every later A-click in the page state. */
    layout_clear_on_demand_consumed();

    /* ── B BUTTON HOLD STATE MACHINE (port of main.lua) ──
     * B press: start tracking; B held + Left/Right: history navigation
     * (deferred to Phase 30 when history exists); B release (4 clean
     * frames): open the address bar — requires the keyboard (Phase 13/14).
     * Until the keyboard lands, B-release is logged as a navigation intent. */
    if (currentState == STATE_HOME || currentState == STATE_PAGE)
    {
        int bDown = (btnCurrent & (1 << 4)) ? 1 : 0; /* kButtonB */
        /* BF14: a B hold ends the home-page crank gesture. */
        if (currentState == STATE_HOME && (btnPushed & (1 << 4)))
        {
            home_page_end_crank_gesture();
        }
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
            /* BF14: crank moves the selection (Settings button included);
             * draw() then follows it with the clamped scroll target. */
            home_page_handle_crank(crankChange);
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
                        else if (page_handle_js_click(activeLink->index))
                        {
                            /* A JS click handler ran (see HTML-mode branch). */
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

                /* (A) = left click: follow hovered link / activate input.
                 * On-demand images: a click on ANY image (linked or bare)
                 * opens the view/unload overlay (#12c — the Lua reference
                 * only triggered through links, so bare <img> like the
                 * google.com logo could never be loaded). */
                int odHitImage = 0;
                if ((btnPushed & (1 << 5)) &&
                    !(btnCurrent & (1 << 0)) && !(btnCurrent & (1 << 1)) &&
                    !layout_get_on_demand_consumed())
                {
                    const LMLink *hitLink = lm_get_hovered_link(mouseX, mouseY + scrollY);
                    if (!hitLink)
                    {
                        /* No link under the cursor: try a bare image hit. */
                        const LayoutItem *imgItem = layout_image_at(
                            mouseX, mouseY + scrollY);
                        if (imgItem)
                        {
                            const char *im2 = storage_setting_str("imageMode");
                            int imgMode2 = IMAGE_MODE_ALL;
                            for (int m = 0; m < IMAGE_MODE_COUNT; m++)
                            {
                                if (im2 && strcmp(im2, IMAGE_MODE_NAMES[m]) == 0)
                                {
                                    imgMode2 = m;
                                    break;
                                }
                            }
                            if (imgMode2 == IMAGE_MODE_ONDEMAND)
                            {
                                layout_show_on_demand_overlay(
                                    imgItem->src, imgItem->imgHref,
                                    imgItem->alt);
                                odHitImage = 1;
                            }
                        }
                    }
                    if (!odHitImage && hitLink)
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
                        else if (page_handle_js_click(hitLink->index))
                        {
                            /* A JS click handler ran: it may have mutated the
                             * DOM (preventDefault) or requested navigation. */
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
            pluto_spill_reset(); /* SW2b: drop aborted page's spill files */
            go_home();
        }
        if (btnPushed & (1 << 0)) /* Left: back */
        {
            http_cancel();
            pluto_spill_reset(); /* SW2b: drop aborted page's spill files */
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
            /* Lua: navigateTo("about:home") only when the previous state was
             * HOME. A page restores its own state — settings_on_change already
             * re-rendered it — so saving from a website returns to that
             * website, not the home page. */
            if (prev == STATE_HOME)
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
        logger_log("updateFrame: heartbeat frame=%u fps=%d overlay=%d stackPeak=%uB/61800B heap=%luKB peak=%luKB bigAlloc=%luKB refusals=%lu",
                   frameCount, g_fpsValue, g_showFps, logger_stack_peak(),
                   pluto_mem_live() / 1024, pluto_mem_peak() / 1024,
                   pluto_mem_peak_alloc() / 1024, pluto_mem_refusals());
    }



    /* FPS sampling: frames over a rolling 500ms window (display runs 30 or
     * 50 fps; the counter only dips below that when a frame runs long). */
    g_fpsFrames++;
    {
        unsigned int now = pd->system->getCurrentTimeMilliseconds();
        if (g_fpsWindowStart == 0)
        {
            g_fpsWindowStart = now;
            g_fpsLastSampleMs = now;
        }
        else if (now - g_fpsLastSampleMs >= 500)
        {
            unsigned int span = now - g_fpsWindowStart;
            if (span > 0)
            {
                g_fpsValue = (int)((g_fpsFrames * 1000u + span / 2) / span);
            }
            g_fpsFrames = 0;
            g_fpsWindowStart = now;
            g_fpsLastSampleMs = now;
        }
    }

    /* FPS overlay: drawn after all state rendering so nothing can clip it. */
    draw_fps_overlay();






    /* Phase 4: pump timers each frame. */
    pdtimer_update();

    /* Phase 24: pump the HTTP state machine each frame. */
    http_update();

    /* P30: pump the image download/decode queue each frame. */
    imgdec_update();

    /* Drive cooperative tasks (Lua Tasks.update parity). */
    tasks_update();

    /* Deliver due JS timers (setTimeout/setInterval); a callback that
     * mutated the DOM schedules the standard re-render via page_rewalk_now. */
    js_timers_update();

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
        logger_stack_touch(); /* baseline: SP near the game-task stack top */

        logger_log("=== PlutoBrowser boot ===");
        logger_log("eventHandler: kEventInit, osversion=%u", pd->system->getSystemInfo()->osversion);

        /* Phase 14: address bar module init. */
        address_bar_init();

        pdtimer_init(pd);
        http_client_init(pd);
        pluto_spill_init(); /* SW2b: disk-backed streaming storage dir */
        /* SW3/SW3a: enable the soft heap budget — the guarded-RAM raise.
         * Everything on the device allocates through the SW1 funnel, so this
         * one gate turns "allocate until the OS panics" into "refuse, log,
         * fall back to disk". 6.5MB against the ~7.5MB usable app pool:
         * 1MB of true headroom keeps Duktape's OOM-fatal handler out of the
         * picture and leaves room for non-funneled SDK internals.
         * DEVICE-ONLY: the simulator's app-resident watermark is ~45MB of
         * host-side allocations — enforcing a device-scale budget there would
         * refuse every large allocation and turn Duktape OOM fatal. The sim
         * stays pure telemetry (budget 0); its suites pin behavior instead. */
#ifdef TARGET_PLAYDATE
        pluto_mem_set_budget(6 * 1024UL * 1024UL + 512 * 1024UL);
#endif
        imgdec_init(pd);
        tasks_init(pd);

        storage_init(pd);
        style_init(pd);
        settings_page_set_onchange_callback(settings_on_change);
        layout_init(pd);

        /* FPS overlay: cache the setting and load the bold font once. */
        g_showFps = storage_setting_int("showFps");
        g_fpsFont = style_font(PLUTO_FONT_BODY_BOLD);
        /* Route page loads to the persisted JS engine selection (0=muJS,
         * 1=Duktape). Page loads attach per-engine bridges. */
        jsbridge_set_engine(storage_setting_int("jsEngine"));
        logger_log("[jsbridge] boot engine: %s", engine_name());

        /* Lua boot: currentBrowseMode = storage_setting_int("mode"). */
        currentBrowseMode = storage_setting_int("mode");
        if (currentBrowseMode != MODE_READER && currentBrowseMode != MODE_RAW_HTML)
        {
            currentBrowseMode = MODE_READER;
        }

        /* Phase 12: boot into the home page with the system menu wired. */
        update_system_menu();
        home_page_reset();
        currentState = STATE_HOME;

#if defined(PLUTO_SETTINGS_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): boot-time
         * settings-panel probe — open the panel (stages storage), log the
         * JavaScript row's label + staged value, cycle the row through the
         * 3-state range with the same buttons a user presses (Right/Right/
         * Left/Left returns to the start: Inline→Full→Off→Full→Inline),
         * then B-cancel (discard — storage untouched) and return home. */
        settings_page_open((int)currentState);
        currentState = STATE_SETTINGS;
        logger_log("[settings-autotest] label='%s' value='%s'",
                   settings_page_label(7), settings_page_staged_value(7));
        {
            const int btnDown = 1 << 3, btnRight = 1 << 1, btnLeft = 1 << 0,
                      btnB = 1 << 4;
            char *act = NULL;
            /* Row 7 is 6 DOWN presses away from the opening selection. */
            for (int i = 0; i < 6; i++)
            {
                act = settings_page_handle_input(btnDown,
                                                 settings_cleared_cookies);
                if (act)
                {
                    pluto_free(act);
                }
            }
            logger_log("[settings-autotest] row7 selected: label='%s'",
                       settings_page_label(7));
            act = settings_page_handle_input(btnRight,
                                                   settings_cleared_cookies);
            if (act)
            {
                pluto_free(act);
            }
            logger_log("[settings-autotest] R -> '%s'",
                       settings_page_staged_value(7));
            act = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act)
            {
                pluto_free(act);
            }
            logger_log("[settings-autotest] RR -> '%s'",
                       settings_page_staged_value(7));
            act = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act)
            {
                pluto_free(act);
            }
            logger_log("[settings-autotest] RRL -> '%s'",
                       settings_page_staged_value(7));
            act = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act)
            {
                pluto_free(act);
            }
            logger_log("[settings-autotest] RRLL -> '%s'",
                       settings_page_staged_value(7));
            act = settings_page_handle_input(btnB, settings_cleared_cookies);
            if (act)
            {
                pluto_free(act);
            }
        }

        /* Row 8 "Javascript Engine": selector probe through the REAL input
         * path, in BOTH Execution states:
         *   Inline → cycles muJS ↔ Duktape (Right then Left returns);
         *   Off    → locked 'Off', L/R no-op, mirrors row 7 live.
         * Storage's jsEnabled is snapshotted and restored; every staged
         * change here is B-cancelled (never saved).
         * NOTE: never navigate DOWN onto row 9 (Clear Cookies) — Right there
         * fires the real cookie-clear callback. Route via row 7 instead. */
        {
            const int btnDown = 1 << 3, btnUp = 1 << 2, btnRight = 1 << 1,
                      btnLeft = 1 << 0, btnB = 1 << 4;
            const int savedJs = storage_setting_int("jsEnabled");
            char *act8 = NULL;
            /* ── Inline state: row 8 must read 'muJS' and ignore L/R. ── */
            storage_set_setting_int("jsEnabled", 1);
            settings_page_open((int)STATE_HOME);
            for (int i = 0; i < 7; i++) /* row 1 → row 8 (stays off row 9) */
            {
                act8 = settings_page_handle_input(btnDown,
                                                  settings_cleared_cookies);
                if (act8)
                {
                    pluto_free(act8);
                    act8 = NULL;
                }
            }
            logger_log("[settings-autotest] row8 selected: label='%s' "
                       "value='%s' (want muJS)",
                       settings_page_label(8), settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RIGHT: '%s' (want Duktape)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RR: '%s' (want QuickJS)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RRR: '%s' "
                       "(want XS (Moddable))",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RRRL: '%s' (want QuickJS)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RRL: '%s' (want Duktape)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RRLL: '%s' (want muJS)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnB, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            /* ── Off state: row 8 must read 'Off', ignore L/R, and mirror
             * row 7 LIVE when the Execution policy changes under it. ── */
            storage_set_setting_int("jsEnabled", 0);
            settings_page_open((int)STATE_HOME);
            for (int i = 0; i < 7; i++)
            {
                act8 = settings_page_handle_input(btnDown,
                                                  settings_cleared_cookies);
                if (act8)
                {
                    pluto_free(act8);
                    act8 = NULL;
                }
            }
            logger_log("[settings-autotest] row8 selected: '%s' (want Off)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after RIGHT: '%s' (want Off)",
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row8 after LEFT: '%s' (want Off)",
                       settings_page_staged_value(8));
            /* UP to row 7 and flip Execution Off → Inline (one RIGHT): the
             * locked row 8 must follow with ZERO input on it. */
            act8 = settings_page_handle_input(btnUp, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            act8 = settings_page_handle_input(btnRight, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row7 '%s' → row8 mirror '%s' "
                       "(want Inline/muJS)",
                       settings_page_staged_value(7),
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnLeft, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            logger_log("[settings-autotest] row7 '%s' → row8 mirror '%s' "
                       "(want Off/Off)",
                       settings_page_staged_value(7),
                       settings_page_staged_value(8));
            act8 = settings_page_handle_input(btnB, settings_cleared_cookies);
            if (act8)
            {
                pluto_free(act8);
                act8 = NULL;
            }
            /* Restore the user's persisted Execution value. */
            storage_set_setting_int("jsEnabled", savedJs);
        }
        currentState = STATE_HOME;
        home_page_reset();
#endif

#if defined(TARGET_SIMULATOR) && defined(PLUTO_JS_AUTOTEST)
        /* TEMPORARY (sim-only, PLUTO_JS_AUTOTEST builds): navigate straight
         * to the JS test suite so the integration run is reproducible.
         * PLUTO_JS_AUTOTEST_OFF additionally forces the JS setting Off. */
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:javascript");
    #ifdef PLUTO_JS_AUTOTEST_OFF
        storage_set_setting_int("jsEnabled", 0);
    #endif
    #ifdef PLUTO_JS_AUTOTEST_DUKTAPE
        /* Run the suite on the DUKTAPE engine instead of muJS: select it in
         * storage AND flip the live router before navigation. The [js] lines
         * and close[Duktape] summary prove which engine binaries ran. */
        storage_set_setting_int("jsEngine", 1);
        jsbridge_set_engine(1);
        logger_log("[js-autotest] engine forced: Duktape");
    #endif
    #ifdef PLUTO_JS_AUTOTEST_QUICKJS
        /* Same, on the QUICKJS engine (storage jsEngine=2, live router=2).
         * close[QuickJS] in the log proves which binaries executed. */
        storage_set_setting_int("jsEngine", 2);
        jsbridge_set_engine(2);
        logger_log("[js-autotest] engine forced: QuickJS");
    #endif
    #ifdef PLUTO_JS_AUTOTEST_XS
        /* Same, on the XS (MODDABLE) engine (storage jsEngine=3, live
         * router=3). close[XS] in the log proves which binaries ran. */
        storage_set_setting_int("jsEngine", 3);
        jsbridge_set_engine(3);
        logger_log("[js-autotest] engine forced: XS (Moddable)");
    #endif
#endif /* TARGET_SIMULATOR guard above */

#if defined(PLUTO_JS_TIMERS_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): navigate straight to
         * the JS test suite and watch its timers actually fire in the
         * running browser (PASS/FAIL via [jstimers-autotest] log lines). */
        if (storage_setting_int("jsEnabled") == 0)
        {
            storage_set_setting_int("jsEnabled", 1);
            logger_log("[jstimers-autotest] jsEnabled was Off, bumped to 1");
        }
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:javascript");
#endif

#if defined(PLUTO_SNAP_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): navigate to the
         * content-rich about:acidtest page; snap_autotest_tick then returns
         * to it to prove the SW6 snapshot fast path end-to-end
         * ([snap-autotest] PASS/FAIL lines in the log). The store is wiped
         * first so the run is hermetic: snapshots persist across sessions
         * (by design), which would otherwise make the first visit a HIT. */
        pluto_snap_invalidate_all();
        logger_log("[snap-autotest] store invalidated for a cold run");
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:acidtest");
#endif

#if defined(PLUTO_PAGE_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): prove SW8 end-to-end.
         * JS on (muJS first), navigate to the acidtest page, then force the
         * RAM budget down so the SW8 pressure policy pages real subtrees of
         * the LIVE DOM out to the spill store. A rewalk must transparently
         * materialize everything (stub count 0) with identical block count.
         * The device build uses its real 6.5MB budget (no override). */
        storage_set_setting_int("jsEnabled", 1);
        storage_set_setting_int("mode", 1); /* RAW_HTML: JS runs in this mode */
        currentBrowseMode = MODE_RAW_HTML;
        snapBypass = 1; /* SW6 fast path serves NO live DOM — force the
                         * classic parse so the page has a live DomResult */
#ifdef PLUTO_PAGE_AUTOTEST_ENGINE
        storage_set_setting_int("jsEngine", PLUTO_PAGE_AUTOTEST_ENGINE);
#endif
        jsbridge_set_engine(storage_setting_int("jsEngine"));
        logger_log("[page-autotest] boot: engine=%d mode=%d (RAW_HTML, "
                   "snap bypass), navigating to acidtest",
                   storage_setting_int("jsEngine"), currentBrowseMode);
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:acidtest");
#endif

#if defined(PLUTO_JS_CLICK_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): navigate straight to
         * the JS test suite so the Event-demo click repro is deterministic.
         * PLUTO_JS_CLICK_AUTOTEST_OFF additionally forces the JS setting
         * Off (negative control: no engine, no listener). */
    #ifdef PLUTO_JS_CLICK_AUTOTEST_FORCE_OFF
        storage_set_setting_int("jsEnabled", 0);
    #else
        if (storage_setting_int("jsEnabled") == 0)
        {
            /* The click path needs an engine; only bump a saved Off. */
            storage_set_setting_int("jsEnabled", 1);
            logger_log("[jsclick-autotest] jsEnabled was Off, bumped to "
                       "Inline for this run");
        }
    #endif
    #ifdef PLUTO_JS_CLICK_AUTOTEST_DUKTAPE
        /* Run the suite on the DUKTAPE engine instead of muJS: select it in
         * storage AND flip the live router before navigation. The click
         * listener/dispatch then exercises the Duktape path on hardware. */
        storage_set_setting_int("jsEngine", 1);
        jsbridge_set_engine(1);
        logger_log("[jsclick-autotest] engine forced: Duktape");
    #endif
    #ifdef PLUTO_JS_CLICK_AUTOTEST_QUICKJS
        /* Same, on the QUICKJS engine (storage jsEngine=2, live router=2):
         * click listener + dispatch + preventDefault on the QuickJS path. */
        storage_set_setting_int("jsEngine", 2);
        jsbridge_set_engine(2);
        logger_log("[jsclick-autotest] engine forced: QuickJS");
    #endif
    #ifdef PLUTO_JS_CLICK_AUTOTEST_XS
        /* Same, on the XS (MODDABLE) engine (storage jsEngine=3, live
         * router=3): click listener + dispatch + preventDefault on XS. */
        storage_set_setting_int("jsEngine", 3);
        jsbridge_set_engine(3);
        logger_log("[jsclick-autotest] engine forced: XS (Moddable)");
    #endif
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:javascript");
#endif /* TARGET_SIMULATOR guard above */

#if defined(PLUTO_JSEXT_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): force the JS
         * setting to Full (2) and navigate to the external-script suite —
         * exercises the prefetch/local-fill chain + FULL execution end to
         * end; assertions land in pluto.log via [jsext] / [js] lines.
         * PLUTO_JSEXT_AUTOTEST_URL overrides the target (sim-only: a
         * loopback HTTP server for the real-network run; on device there
         * is no dev machine to serve it). */
#ifdef PLUTO_JSEXT_AUTOTEST_ENGINE
        /* Force a specific engine for the run: 1=Duktape 2=QuickJS 3=XS
         * (Moddable). Storage AND the live router are set before navigation;
         * the [jsbridge] page attach / [js] close[<name>] lines prove which
         * engine binaries ran the page. */
        storage_set_setting_int("jsEngine", PLUTO_JSEXT_AUTOTEST_ENGINE);
        jsbridge_set_engine(PLUTO_JSEXT_AUTOTEST_ENGINE);
        logger_log("[jsext-autotest] engine forced: %d", (int)PLUTO_JSEXT_AUTOTEST_ENGINE);
#endif
        storage_set_setting_int("jsEnabled", 2);
        logger_log("[jsext-autotest] armed (jsEnabled=2)"); /* device-build marker */
        pendingNavUrlSet = 1;
#ifdef PLUTO_JSEXT_AUTOTEST_URL
/* The target URL lives in tests/jsext_autotest_url.h (generated into tests/
 * before a seam build) — a -D value containing "//" is parsed as a
 * comment by the preprocessor, and shell/make quoting is too fragile:
 *   printf '#define PLUTO_JSEXT_AUTOTEST_URL_STR "%s"\n' \
 *     "http://127.0.0.1:8099/page.html" > tests/jsext_autotest_url.h
 *   make SIMDEFS="-DPLUTO_JSEXT_AUTOTEST -DPLUTO_JSEXT_AUTOTEST_URL=1"
 */
#include "../tests/jsext_autotest_url.h"
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                 PLUTO_JSEXT_AUTOTEST_URL_STR);
#else
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:jsext");
#endif
#endif

#if defined(PLUTO_NAV_AUTOTEST)
        /* TEMPORARY (autotest builds, sim + device): leave the app on the
         * home page — nav_autotest_tick() steers the Google speed-dial card
         * and presses A through the real input path (see updateFrame). */
#endif

        /* Keyboard instance. The port's contract:
         * setPlaydateUpdateCallback MUST be called before show() — while the
         * keyboard is visible it OWNS the system update callback and invokes
         * our update from inside its own (calling it with playdateUpdate NULL
         * would crash). */
        g_kb = keyboardApi.newKeyboard();
        keyboardApi.setPlaydateUpdateCallback(g_kb, updateFrame, NULL);
        /* Display refresh target: apply AFTER the keyboard exists so its
         * key-repeat timing is derived from the same rate. */
        apply_display_fps();
        /* Replace the Lua run loop with our native update function. */
        pd->system->setUpdateCallback(updateFrame, NULL);
        break;

    case kEventTerminate:
        pluto_spill_reset(); /* SW2b: no orphan spill files across sessions */
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

