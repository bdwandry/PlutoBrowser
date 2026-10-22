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
#include "core/netmon.h"
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
#include "html/css.h" /* Phase 1.5: stylesheet-scan API + caps */
#include "html/jsbridge.h"
#include "html/jsbridge_internal.h" /* Forms-Phase 3: the shared validity layer */
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
/* R30l: live script-prefetch byte counter (fed by html/jsext.c via
 * pluto_ui_net_progress) — drives the loading screen's "Fetching scripts"
 * line. The main-HTML counter above finishes in one frame (402B gzip), so
 * the 502716-byte bundle previously NEVER moved any visible counter
 * (device run 14: the user always saw "0 bytes being downloaded"). */
static int progressNetCur = 0;
static int progressNetTotal = 0;
static int progressTotal = 0;
static int isRendering = 0;
static int pendingNavUrlSet = 0;              /* Lua pendingNavUrl */
static int pendingFlipUnload = 0;             /* settings flip: unload old page
                                               * before the deferred reload */
static char pendingNavUrl[640] = "";
static int navigatingHistory = 0;             /* Lua navigatingHistory */
static int mouseX = 200, mouseY = 100;        /* HTML-mode virtual cursor */
#define MOUSE_SPEED 4                          /* Lua mouseSpeed */
/* ── Form keyboard target key ─────────────────────────────────────────────── */
/* LayoutItem pointers die on every layout rebuild (React re-renders fire a
 * rewalk per mutation), but the Playdate keyboard lives across frames: a
 * keystroke written through the stale pointer lands in whatever reused the
 * freed memory — on multi-box pages, deterministically the FIRST box. So
 * the keyboard remembers WHAT was opened (form action + input name +
 * ordinal among its twins) and re-resolves to the live item AND the live
 * DOM node on every keystroke and on commit. DOM write-through also makes
 * typed text survive rebuilds (new items are built from the value attr). */
typedef struct
{
    char form[256];
    char name[64];
    int ordinal;
    int valid;
} FormKey;
static FormKey g_formKey;
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

static size_t pluto_load_local_file(const char *fileUrl);

/* navigate_to: full port of main.lua runNavigation/executeNavigation. */
static void navigate_to(const char *urlString);
static void render_body(const char *body, const char *url, int addToHistory);
static void toggle_details(const char *dkey);
static void activate_form_block(const LayoutItem *item);
static void submit_form(const char *formAction, const LayoutItem *inputBlock);
/* Forms-Phase 6: the driver's dialog tap maps a DomNode onto its layout
 * item (defined with the form item helpers, used early by the phase). */
static const LayoutItem *form_item_by_node(DomNode *node);
static void open_keyboard_for_input(const LayoutItem *item);
static void form_kb_did_hide(void *ud);
/* Form keyboard target key (defined with the keyboard code, used early by
 * the formclick autotest). */
static void form_key_snapshot(const LayoutItem *item);
static void form_key_write(const char *text, int commit);
static void form_kb_ok_commit(const char *text);
static const char *form_item_value(const LayoutItem *item);
static void form_kb_will_hide(int okButtonPressed, void *ud);
static void form_kb_text_changed(void *ud);
static DomNode *form_node_ancestor(DomNode *n);
/* Forms-Phase 4: the form API host hooks (registered at boot) + the
 * implicit-submission chain the keyboard's Enter commits ride. The
 * bridge rides the call so mid-parse scripts (a battery page running
 * its inline form API calls before g_pageJs exists) still dispatch
 * through their own machine. */
static int pluto_host_form_reset(JsBridge *b, void *formNode);
static int pluto_host_form_wire(JsBridge *b, void *formNode,
                                void *submitterNode, int wantGate,
                                int wantSubmitEvent);
static void pluto_form_enter_commit(void);
static int form_gate_general(JsBridge *b, DomNode *formNode, DomNode *submitterNode);
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
#if defined(PLUTO_ENGINE_FLIP_TEST)
static void engine_flip_test_tick(void);
#endif
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

/* Cached Website setting: 1 = snapshot behavior (load/serve, as before);
 * anything else = Off (default): every new-site click fetches + renders a
 * fresh copy — the snapshot LOAD is skipped. The save flow and spill
 * staging keep running normally in both modes (big-site rendering is
 * untouched); the page being left is purged from disk (see navigate_to).
 * Live read (same pattern as invertCrank/mode) so it applies immediately. */
static int cache_off(void)
{
    return storage_setting_int("cacheWebsite") != 1;
}

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

/* ── Phase 1.5: in-flight stylesheet fetch collector ──
 * The HTTP client is single-flight and the render task is the only
 * starter during this stage, so file-statics are safe (same pattern the
 * nav task uses for its own collector). The client hands a RAM body on
 * success (spill lane is skipped for CSS — bodies are tiny vs the spill
 * threshold); bad status or error = sheet skipped (the page renders
 * without it; containment by absence). */
static char *g_cssBody = NULL; /* captured body (heap, transferred to rt) */
static size_t g_cssLen = 0;
static int g_cssStatus = 0;
static int g_cssDone = 0;
static int g_cssErr = 0;
static int g_cssInFlight = 0; /* 1 = the done-flag has an owning slot */

static void css_ext_on_success(int status, char **headerKeys,
                               char **headerVals, int headerCount,
                               const char *body, size_t bodyLen,
                               const char *url)
{
    (void)headerKeys;
    (void)headerVals;
    (void)headerCount;
    (void)url;
    g_cssStatus = status;
    if (status >= 200 && status < 300 && body && bodyLen > 0)
    {
        /* Copy ≤ the page cap (bodies beyond the envelope trim — a
         * truncated sheet still parses the rules that made it). */
        size_t want = bodyLen;
        if (want > PLUTO_CSS_EXT_TOTAL_MAX)
        {
            want = PLUTO_CSS_EXT_TOTAL_MAX;
        }
        char *copy = (char *)pluto_mem_realloc(NULL, want + 1);
        if (copy)
        {
            memcpy(copy, body, want);
            copy[want] = '\0';
            g_cssBody = copy;
            g_cssLen = want;
        }
        /* else: allocation refused → sheet skipped via the error lane. */
    }
    g_cssDone = 1;
}

static void css_ext_on_spill(int status, SpillFile spill, size_t bodyLen,
                             const char *url)
{
    (void)status;
    (void)spill;
    /* ≥ the client's spill threshold for a stylesheet is unusual; the
     * disk lane stays script-only in v1 (96KB-class bodies never hit it). */
    (void)bodyLen;
    (void)url;
    logger_log("[css-ext] sheet dropped: disk-spill body (v1 limit)");
    g_cssErr = 1;
    g_cssDone = 1;
}

static void css_ext_on_error(const char *message)
{
    (void)message;
    g_cssErr = 1;
    g_cssDone = 1;
}

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
    /* ── Phase 1.5: external <link rel="stylesheet"> fetch stage ──
     * cssPhase 0 = not started, 1 = scanned → lists ready, 2 = fetching
     * (_indices advance as sheets settle), 3 = done (bodies already moved
     * onto rt->doc). Runs BEFORE the script prefetch so the stylesheet
     * lane stays independent of the JS setting (JS-Off pages get CSS). */
    int cssPhase;
    int cssUrlN;                         /* hrefs resolved & queued */
    int cssIdx;                          /* current fetch slot */
    size_t cssBytes;                     /* RAM envelope (PLUTO_CSS_EXT_TOTAL_MAX) */
    char *cssUrls[PLUTO_CSS_EXT_MAX];    /* resolved, still owned here */
    char *cssBody[PLUTO_CSS_EXT_MAX];    /* captured bodies */
    size_t cssLen[PLUTO_CSS_EXT_MAX];
} RenderTask;

/* JS bridge of the LIVE page (kept across the parse→layout→done sequence
 * for click-event dispatch). The bridge is owned by the DocParseResult:
 * every teardown path closes it BEFORE document_free frees the doc (and
 * the live DOM the engine points into). */
static JsBridge *g_pageJs = NULL;

/* Close the old page's bridge + doc, then adopt the new one. Callers must
 * already hold the new doc in rt->doc. */
/* R56f: native form fallback state (the definitions live near
 * form_node_ancestor below); page_swap_doc resets the card at the page
 * boundary — the pointer must not survive document_free into the next
 * page (use-after-free on the first fallback tap after a navigation). */
static DomNode *g_fbCard;
static DomResult *g_fbDom;
enum
{
    FB_BODY_CAP = 1600,
    FB_SAVED_MAX = 16
};
static char g_fbBody[FB_BODY_CAP];
static int g_fbInFlight;
static DomNode *g_fbSaved[FB_SAVED_MAX];
static int g_fbSavedCount;
static int g_fbSummaryOn;
/* Forms-Phase 6: WHICH form owns the sent-card summary. The R56g
 * "Send another" restore stays scoped to that form — another form's
 * submit while the summary shows is its own submit (browser truth),
 * not a card dismissal. */
static DomNode *g_fbSummaryForm;

static void page_swap_doc(RenderTask *rt)
{
    if (rt->rewalkDoc)
    {
        /* JS-mutation re-walk: the doc was updated in place — keep it (and
         * its live engine) alive; nothing to swap. */
        currentDoc = rt->rewalkDoc;
        rt->rewalkDoc = NULL;
        g_pageJs = currentDoc->_jsbridge;
        /* R27: the rewalk IS the post-eval truth — snapshot HERE too.
         * This branch previously early-returned before the save block,
         * so a deferred snapshot never materialized (the revisit then
         * served whatever stale entry was already on disk). Same
         * eval_pending gate as the fresh-render path below. */
        if (!currentDoc->parseError && currentUrlObj &&
            currentUrlObj->normalized[0])
        {
            if (g_pageJs && jsbridge_eval_pending(g_pageJs))
            {
                logger_log("[snap] save deferred (rewalk): eval still running");
            }
            else
            {
                unsigned long now = 0;
                uint32_t ms = 0;
                now = pd->system->getSecondsSinceEpoch(&ms);
                pluto_snap_save(currentDoc, currentUrlObj->normalized,
                                (int)currentBrowseMode, now);
                pluto_snap_lru_sweep(SNAP_MAX_ENTRIES);
            }
        }
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
        /* R56f: the fallback's state lives in the OLD tree — drop it
         * BEFORE any later tap could deref freed memory (the card is
         * re-created lazily on the next fallback submit). Also kill the
         * in-flight flag: the POST's callbacks carry the tree-identity
         * guard and will self-log on the new page. */
        g_fbCard = NULL;
        g_fbDom = NULL;
        g_fbInFlight = 0;
        g_fbSummaryOn = 0;
        g_fbSummaryForm = NULL;
        g_fbSavedCount = 0;
        /* Page boundary: re-base the funnel's live counter to the tracked
         * truth — drift across this page's engine churn would otherwise
         * shrink the next page's usable budget (device flip-crash, 2026-09-22). */
        pluto_mem_resync_live();
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
     * truth (toggle states / JS mutations included).
     * R27 GATE: never snapshot while the engine's background eval (XS NR
     * bundler split) is still running — the walker would capture the
     * PRE-JS DOM (device run 5: blocks=1 poisoned snapshot saved 1s into
     * the 61s eval, then served by the revisit). The split's completion
     * triggers a rewalk (page_rewalk_now), which lands here with the
     * eval done and snapshots the real page. */
    if (currentDoc && !currentDoc->parseError && currentUrlObj &&
        currentUrlObj->normalized[0])
    {
        if (g_pageJs && jsbridge_eval_pending(g_pageJs))
        {
            logger_log("[snap] save deferred: bundle eval still running");
            /* R27: the entry on disk (if any) is now KNOWN-stale — this
             * render's walk predates the JS. Invalidate so a revisit can
             * never serve a poisoned pre-eval frame (device run 5 cached
             * blocks=1; the healing also applies to snapshots written by
             * older builds still on the device). */
            pluto_snap_invalidate(currentUrlObj->normalized,
                                  (int)currentBrowseMode);
        }
        else
        {
            unsigned long now = 0;
            uint32_t ms = 0;
            now = pd->system->getSecondsSinceEpoch(&ms);
            pluto_snap_save(currentDoc, currentUrlObj->normalized,
                            (int)currentBrowseMode, now);
            pluto_snap_lru_sweep(SNAP_MAX_ENTRIES);
            /* R30p: the idle-page engine close is DISABLED — it was part of
             * the run-18 crash batch and never got runtime coverage in any
             * environment. jsbridge_is_idle() stays available for a future
             * one-change-at-a-time device A/B. */
        }
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
    /* R15: retry admission-deferred giant scripts once per frame — the
     * moment memory frees (download buffers released, page machinery
     * settled), the compile goes through. A successful run mutates the DOM
     * like any script and re-renders through page_rewalk_now below. */
    int deferredMutations = 0;
    jsbridge_deferred_pump(g_pageJs, &deferredMutations);
    mutations |= deferredMutations;
    /* R20: per-frame ENGINE pump — resumes a time-sliced bundler split
     * (XS-NR) one wall-clock budget per frame so the SDK run loop keeps
     * getting update() returns (device watchdog fires at a 10s stall). */
    int engineMutations = 0;
    jsbridge_engine_pump(g_pageJs, nowMs, &engineMutations);
    mutations |= engineMutations;
    /* R54: per-frame script stage — js_doc_attach (FULL mode) parks the
     * page's script plan in document order and this runs ONE entry per
     * frame. The old sync attach loop executed every script's
     * parse+compile+run inside one update(); the device breached the 10s
     * watchdog mid-lexer (E1 2026-10-18 12:02:50). */
    int queueMutations = 0;
    jsbridge_script_queue_pump(g_pageJs, &queueMutations);
    mutations |= queueMutations;
    /* Roadmap #3: deliver settled XHR/fetch completions through the SAME
     * engine bracket + re-render path (the HTTP client was pumped earlier
     * this frame by http_update; completions it settled are pending here). */
    int xhrMutations = 0;
    (void)jsbridge_xhr_pump(g_pageJs, &xhrMutations);
    /* API-P10: the async module lane — deliveries (already settled this
     * frame by http_update) drain through the standing module bracket,
     * the transport head starts the next wire fetch while the client is
     * idle, watchdogs age parked fetches, and __p5dynq tickets settle. */
    jsbridge_modules_pump(g_pageJs, nowMs);
    /* API-P8: worker stub error deliveries — the same frame-later analog
     * of a failed script load; never fires inside the constructing script. */
    int wkMutations = 0;
    (void)jsbridge_worker_pump(g_pageJs, &wkMutations);
    xhrMutations = xhrMutations || wkMutations;
    /* innerHTML wrapper assignments (XS (No Recursion)): the shared
     * engines never set the latch, so the extra take is a no-op for
     * them. Swap the layout to the fresh subtree (fallback restore swaps
     * head/body wholesale) before the user's next tap hits stale blocks. */
    if (jsbridge_take_dom_dirty(g_pageJs))
    {
        xhrMutations = 1;
    }
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
 *   phase 2: navigate back. Cached Website On → the revisit must come
 *            from the snapshot fast path (g_snapFastPathHits >= 1).
 *            Off (default) → the revisit must fetch + render FRESH
 *            (hits stay 0).
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
    /* Phase 2: On → the revisit must render from the snapshot.
     * Off → the revisit must fetch + render fresh (no fast path). */
    if (storage_setting_int("cacheWebsite") == 1)
    {
        if (g_snapFastPathHits >= 1 && currentState == STATE_PAGE &&
            currentDoc &&
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
                       "(hits=%d, state=%d)", g_snapFastPathHits,
                       currentState);
            g_snapTestPhase = 3;
        }
        return;
    }
    if (g_snapFastPathHits == 0 && g_snapTestFrames > 30 &&
        currentState == STATE_PAGE && currentDoc &&
        strncmp(currentDoc->baseUrl, "about:acidtest", 14) == 0)
    {
        logger_log("[snap-autotest] PASS: cache off, revisit fetched fresh "
                   "(hits=%d, blocks=%d, title=%.40s)",
                   g_snapFastPathHits, currentDoc->blockCount, pageTitle);
        g_snapTestPhase = 3;
    }
    else if (g_snapFastPathHits != 0 || g_snapTestFrames > 600)
    {
        logger_log("[snap-autotest] FAIL: cache off but snapshot served "
                   "(hits=%d, state=%d)", g_snapFastPathHits, currentState);
        g_snapTestPhase = 3;
    }
}
#endif

#if defined(PLUTO_NR_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): prove the SW6 snapshot fast
 * path for the deep-CSR page the NR engine just rendered. The boot nav
 * bypasses the snapshot (the watchdog run must exercise the engine); once
 * that render completes, this tick re-navigates to the SAME normalized URL
 * and requires the revisit to come from the snapshot — network 0, parse 0,
 * JS eval 0.
 *   phase 0: wait for the live render (save happened in page_swap_doc;
 *            fast-path hits must still be 0 — the boot bypassed the cache).
 *   phase 1: revisit must be served from the fast path (hits >= 1).
 * PASS/FAIL lands in pluto.log via [nr-autotest] lines. */
#if defined(PLUTO_NR_AUTOTEST) && !defined(TARGET_PLAYDATE)
/* MEMWATCH (sim-only): hold the SIM to the DEVICE's memory envelope for the
 * revisit path and log per-phase RAM telemetry, so the host run can be
 * judged against physical-device caps (6.5MB funnel budget inside the
 * ~7.5MB usable app pool, 61.8KB fixed stack). The FIRST visit must NOT be
 * budgeted: the sim eval legitimately scales to host-64 (peak ~86MB) and a
 * device-scale refusal mid-eval would fabricate an OOM that the physical
 * device (5.45MB peak, proven) never hits. The REVISIT — the path that must
 * actually fit the device — runs fully budgeted: any refusal logged here is
 * a real would-be device failure. */
static unsigned long g_memWatchSavedBudget = 0;
static int g_memWatchActive = 0;
static void memwatch_arm(const char *phase)
{
    if (!g_memWatchActive)
    {
        g_memWatchSavedBudget = pluto_mem_budget();
        g_memWatchActive = 1;
    }
    /* R43: keep the revisit arm in step with the boot envelope (7.5MB).
     * The revisit's snapshot render uses ~1.3MB, so this only matters if a
     * revisit ever re-evals; a stale 6.5 here would contradict boot. */
    pluto_mem_set_budget(7UL * 1024UL * 1024UL + 512UL * 1024UL);
    logger_log("[memwatch] ARM phase=%s budget=7.5MB (device envelope) "
               "live=%luKB peak=%luKB refusals=%lu",
               phase, pluto_mem_live() / 1024, pluto_mem_peak() / 1024,
               pluto_mem_refusals());
}
static void memwatch_log(const char *phase)
{
    logger_log("[memwatch] %s live=%luKB peak=%luKB bigAlloc=%luKB "
               "refusals=%lu stackPeak=%uB/61800B",
               phase, pluto_mem_live() / 1024, pluto_mem_peak() / 1024,
               pluto_mem_peak_alloc() / 1024, pluto_mem_refusals(),
               logger_stack_peak());
}
static void memwatch_disarm(void)
{
    if (g_memWatchActive)
    {
        pluto_mem_set_budget(g_memWatchSavedBudget);
        g_memWatchActive = 0;
        logger_log("[memwatch] DISARM budget restored to %luKB",
                   g_memWatchSavedBudget / 1024);
    }
}
#else
#define memwatch_arm(p) ((void)0)
#define memwatch_log(p) ((void)0)
#define memwatch_disarm() ((void)0)
#endif

static int g_nrSnapPhase = 0;
static unsigned g_nrSnapFrames = 0;
static int g_nrSoakDone = 0;   /* R47 soak: completed nav cycles */
static int g_nrSoakRetries = 0; /* R47: boot-nav error-page retries */
static int g_nrSoakBypass = 0;  /* R47b: bypass snapshot every cycle
                                 * (force full fetch + eval per nav) */
static int g_nrSoakTotal = 0;  /* R47 soak: cycles requested (0 = off) */
static char g_nrSoakDocUrl[648] = ""; /* doc base at the START of this cycle */
static char g_nrSnapUrl[648] = "";
static char g_nrSnapDocUrl[648] = ""; /* the LIVE doc's baseUrl (may differ
                         * from the nav URL when the server redirected —
                         * bryanwandrych.com http->https) */
static void nr_revisit_autotest_tick(void)
{
    if (g_nrSnapPhase == 3 || isRendering)
    {
        return;
    }
    ++g_nrSnapFrames;
    if (g_nrSnapFrames < 30)
    {
        return; /* settle ~0.5s per phase */
    }
    /* R-home phase 5: the cycle ended on an about:/file: probe page —
     * give the page 30s of rest (late module tickets, resting renders),
     * then navigate home so the device is back on the home screen. */
    if (g_nrSnapPhase == 5)
    {
        if (g_nrSnapFrames >= 1500)
        {
            logger_log("[nr-autotest] probe rest done (1500 frames) — "
                       "navigating home");
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
            g_nrSnapPhase = 3;
        }
        return;
    }
    if (g_nrSnapPhase == 0)
    {
        /* R47: state=3 is the ERROR page (network dead zone — run 41's
         * boot nav timed out after 60s on WiFi). That is not an engine
         * failure: retry the nav (snapshot keeps the offline cycles
         * meaningful) instead of declaring FAIL and idling forever. */
        if (currentState == STATE_ERROR)
        {
            if (g_nrSoakRetries < 3)
            {
                g_nrSoakRetries++;
                logger_log("[nr-autotest] boot nav hit the error page — "
                           "retrying (%d/3)", g_nrSoakRetries);
                g_nrSnapFrames = 0;
                pendingNavUrlSet = 1;
                snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                         g_nrSnapUrl[0] ? g_nrSnapUrl : "http://bryanwandrych.com");
                return;
            }
            logger_log("[nr-autotest] FAIL: boot nav stuck on the error "
                       "page after %d retries (network, not engine)",
                       g_nrSoakRetries);
            g_nrSnapPhase = 3;
            return;
        }
        if (currentState != STATE_PAGE || !currentDoc || !currentUrlObj ||
            !currentUrlObj->normalized[0])
        {
            if (g_nrSnapFrames > 3600)
            {
                logger_log("[nr-autotest] FAIL: first render never completed "
                           "(state=%d)", currentState);
                g_nrSnapPhase = 3;
            }
            return;
        }
        /* R27: wait for the background eval (bundler split) to finish —
         * the DOM is only the POST-JS truth after the last segment. The
         * device race walked at eval second 1 and snapshotted blocks=1. */
        if (g_pageJs && jsbridge_eval_pending(g_pageJs))
        {
            if (g_nrSnapFrames > 7200)
            {
                logger_log("[nr-autotest] FAIL: bundle eval never completed");
                g_nrSnapPhase = 3;
            }
            return;
        }
        if (g_snapFastPathHits != 0)
        {
            logger_log("[nr-autotest] FAIL: fast path fired on the boot visit "
                       "(hits=%d) — bypass lost", g_snapFastPathHits);
            g_nrSnapPhase = 3;
            return;
        }
        /* about: probe pages (about:cssprobe, about:jsext, about:javascript)
         * are snap-bypassed BY DESIGN (snap_skip_url — every visit must
         * freshly parse+render), so the snapshot-revisit phases below can
         * never see a hit. End the autotest here for probe URLs: the probe
         * page itself is the assertion (its own rows + the [css-skip] tally). */
        if (strncmp(currentUrlObj->normalized, "about:", 6) == 0)
        {
            logger_log("[nr-autotest] about: probe page — snapshot-revisit "
                       "phases skipped (snap_skip_url by design)");
            /* R-home (2026-10-22): don't end on the probe page. Arm a
             * phase-5 rest (30s — post-battery module tickets land in
             * that window) then navigate home so the device is back on
             * the home screen after every probe cycle. */
            g_nrSnapPhase = 5;
            g_nrSnapFrames = 0;
            return;
        }
        if (strncmp(currentUrlObj->normalized, "file:", 5) == 0)
        {
            /* file: fixtures (P5 module fixture pages, P10's async-lane
             * page) are local test documents: the row text is the
             * assertion, and the async module chain lands AFTER first
             * render — a revisit would either serve the pre-chain
             * snapshot with no JS eval or tear the page down mid-fetch.
             * Same design as the about: probe skip. */
            logger_log("[nr-autotest] file: fixture page — snapshot-revisit "
                       "phases skipped (local test page)");
            /* R-home: same deferred return as the about: probe lane. */
            g_nrSnapPhase = 5;
            g_nrSnapFrames = 0;
            return;
        }
        snprintf(g_nrSnapUrl, sizeof(g_nrSnapUrl), "%s",
                 currentUrlObj->normalized);
        snprintf(g_nrSnapDocUrl, sizeof(g_nrSnapDocUrl), "%s",
                 currentDoc->baseUrl);
        /* R47 soak: each cycle compares against the doc base recorded for
         * THAT cycle (redirect can differ per nav). Cycle 1 uses the same
         * value as the boot baseline. */
        snprintf(g_nrSoakDocUrl, sizeof(g_nrSoakDocUrl), "%s",
                 currentDoc->baseUrl);
        memwatch_log("first-render");
        memwatch_arm("revisit");
        logger_log("[nr-autotest] first render live (blocks=%d jsRan=%d "
                   "errs=%d), snap key=%s doc=%s — revisiting",
                   currentDoc->blockCount, currentDoc->jsRan,
                   currentDoc->jsErrors, g_nrSnapUrl, g_nrSnapDocUrl);
        g_nrSnapPhase = 1;
        g_nrSnapFrames = 0;
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", g_nrSnapUrl);
        return;
    }
    /* Phase 1: the revisit must render from the snapshot (no JS eval).
     * Compare the restored doc's baseUrl against the LIVE doc's baseUrl
     * recorded in phase 0 — the snapshot key is the normalized NAV url,
     * which can differ from the doc base (http->https redirect).
     * R47b bypass cycles take a DIFFERENT contract: snapBypass skipped
     * the snapshot, so success = a live full-eval render of the SAME doc
     * (jsRan=1, blocks>0, no JS errors, same baseUrl). */
    if (g_nrSoakBypass)
    {
        /* R47d: the PASS must observe the POST-JS doc, not the pre-React
         * placeholder. Run 44's judge fired 2s into the 40s eval (the
         * page attaches and the walker paints blocks=1 BEFORE the split
         * starts mutating the DOM), so cycles 2-5 "PASSed" on an empty
         * frame. Gate on eval-pending==0: the split is over, the mount
         * completed, and the final rewalk has landed. */
        if (g_pageJs && jsbridge_eval_pending(g_pageJs))
        {
            return; /* full eval still in flight — keep waiting */
        }
        if (currentState == STATE_PAGE && currentDoc && currentDoc->jsRan == 1 &&
            currentDoc->blockCount > 1 && currentDoc->jsErrors == 0 &&
            strcmp(currentDoc->baseUrl, g_nrSoakDocUrl) == 0)
        {
            logger_log("[nr-autotest] PASS: soak bypass cycle (full eval) "
                       "(blocks=%d jsRan=%d errs=%d)",
                       currentDoc->blockCount, currentDoc->jsRan,
                       currentDoc->jsErrors);
            memwatch_log("revisit-done");
            memwatch_disarm();
            g_nrSnapPhase = 3;
            if (g_nrSoakTotal > 0 && ++g_nrSoakDone < g_nrSoakTotal)
            {
                logger_log("[nr-autotest] soak cycle %d/%d done — re-navigating "
                           "(bypass)", g_nrSoakDone, g_nrSoakTotal);
                snapBypass = 1; /* next cycle: full fetch + full eval again */
                g_nrSnapPhase = 1;
                g_nrSnapFrames = 0;
                snprintf(g_nrSoakDocUrl, sizeof(g_nrSoakDocUrl), "%s",
                         currentDoc->baseUrl);
                pendingNavUrlSet = 1;
                snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", g_nrSnapUrl);
                return;
            }
            if (g_nrSoakTotal > 0)
            {
                logger_log("[nr-autotest] SOAK COMPLETE: %d/%d bypass cycles, "
                           "no crash", g_nrSoakDone, g_nrSoakTotal);
                /* Return the device to a friendly state: the soak ends on
                 * a page screen with a dead engine, so navigate home. */
                pendingNavUrlSet = 1;
                snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
                logger_log("[nr-autotest] navigating home");
            }
        }
        else if (g_nrSnapFrames > 7200)
        {
            /* R47c: a failed fetch (WiFi drop — run 43 cycle 2: the HTML
             * loaded but both ext scripts died on "Connection failed")
             * renders empty WITHOUT touching the engine. That is a
             * network miss, not the formerly-crashing path: retry the
             * nav before declaring FAIL. */
            if (g_nrSoakRetries < 2)
            {
                g_nrSoakRetries++;
                logger_log("[nr-autotest] bypass cycle did not complete a "
                           "full eval (state=%d blocks=%d jsRan=%d) — "
                           "retrying (%d/2)",
                           currentState,
                           currentDoc ? currentDoc->blockCount : -1,
                           currentDoc ? currentDoc->jsRan : -1,
                           g_nrSoakRetries);
                g_nrSnapPhase = 1;
                g_nrSnapFrames = 0;
                snapBypass = 1;
                pendingNavUrlSet = 1;
                snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                         g_nrSnapUrl);
                return;
            }
            logger_log("[nr-autotest] FAIL: bypass cycle did not complete a "
                       "full eval after %d retries (state=%d blocks=%d "
                       "jsRan=%d errs=%d)",
                       g_nrSoakRetries, currentState,
                       currentDoc ? currentDoc->blockCount : -1,
                       currentDoc ? currentDoc->jsRan : -1,
                       currentDoc ? currentDoc->jsErrors : -1);
            memwatch_disarm();
            g_nrSnapPhase = 3;
            /* R-home: end every cycle on the home page. */
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
            logger_log("[nr-autotest] navigating home");
        }
        return;
    }
    if (g_snapFastPathHits >= 1 && currentState == STATE_PAGE && currentDoc &&
        strcmp(currentDoc->baseUrl, g_nrSoakDocUrl) == 0)
    {
        logger_log("[nr-autotest] PASS: revisit from snapshot (hits=%d "
                   "blocks=%d jsRan=%d) — JS eval skipped",
                   g_snapFastPathHits, currentDoc->blockCount,
                   currentDoc->jsRan);
        memwatch_log("revisit-done");
        memwatch_disarm();
        g_nrSnapPhase = 3;
        /* R47 soak (PLUTO_NR_SOAK=n): the formerly-crashing edge was the
         * SECOND navigation — one render + one revisit proves less than
         * n consecutive cycles. After each PASS, re-arm the same nav and
         * count the cycle; stop after n or on any FAIL. Cycles 2+ also
         * exercise repeated snapshot save/load + funnel resync drift.
         * R47b (PLUTO_NR_SOAK_BYPASS): re-arming snapBypass every cycle
         * forces the FULL path each time — network fetch + 502KB bundle
         * parse/split + React mount + boundary compacts + fresh snapshot
         * save — the slow, memory-heavy path that only ran twice before.
         * The phase-1 check below is bypassed to match: cycles are judged
         * by the live render itself (blocks>0, no errors), not by a hit. */
        if (g_nrSoakBypass)
        {
            snapBypass = 1; /* next nav re-downloads + re-evals everything */
            logger_log("[nr-autotest] soak bypass armed — cycle %d will "
                       "full-fetch + full-eval", g_nrSoakDone + 1);
        }
        if (g_nrSoakTotal > 0 && ++g_nrSoakDone < g_nrSoakTotal)
        {
            logger_log("[nr-autotest] soak cycle %d/%d done — re-navigating",
                       g_nrSoakDone, g_nrSoakTotal);
            g_nrSnapPhase = 1;
            g_nrSnapFrames = 0;
            snprintf(g_nrSoakDocUrl, sizeof(g_nrSoakDocUrl), "%s",
                     currentDoc->baseUrl);
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", g_nrSnapUrl);
            return;
        }
        if (g_nrSoakTotal > 0)
        {
            logger_log("[nr-autotest] SOAK COMPLETE: %d/%d cycles, no crash",
                       g_nrSoakDone, g_nrSoakTotal);
            /* Return the device to a friendly state: the soak ends on
             * a page screen with a dead engine, so navigate home. */
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
            logger_log("[nr-autotest] navigating home");
        }
        /* R-home (2026-10-22): standard single-cycle runs end the same
         * way — navigate home after the PASS so the device never lingers
         * on a test page (the deploy-collect flow unmounts right after). */
        if (g_nrSoakTotal == 0)
        {
            pendingNavUrlSet = 1;
            snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
            logger_log("[nr-autotest] navigating home");
        }
    }
    else if (g_nrSnapPhase != 5 && g_nrSnapFrames > 600)
    {
        logger_log("[nr-autotest] FAIL: revisit missed the snapshot "
                   "(hits=%d state=%d baseUrl=%.80s)",
                   g_snapFastPathHits, currentState,
                   currentDoc ? currentDoc->baseUrl : "(none)");
        memwatch_disarm();
        g_nrSnapPhase = 3;
        /* R-home: end every cycle on the home page, FAIL included. */
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "about:home");
        logger_log("[nr-autotest] navigating home");
    }
}

/* ── [nr-autotest-tap]: sim autotest, first stamped submit block ─────────
 * After the boot page settles (hydration fallback IS allowed to run its
 * course — phases wait past any innerHTML restore + induced rewalk), find
 * the first layout item of type LRI_INPUT_SUBMIT whose block carries a
 * stamped srcNode (input[type=submit|button]/button inside <form>) and
 * drive the EXACT path a real tap uses: activate_form_block() → B2 click
 * dispatch → B5 submit branch. PASS evidence = [form] dispatch rows +
 * in-place handling (no navigation change) + jsErrors unchanged; the
 * device was never touched — SIM ONLY (user gate for device deploys). */
#if defined(PLUTO_NR_AUTOTEST_TAP)
/* The render-result sample rows print only the FIRST THREE blocks, so a
 * pass that hinges on text ANYWHERE on the page needs a whole-array scan:
 * concat each block's inline texts (same source of truth the renderer
 * prints) and substring-search. First hit returns its text for the log. */
static int nr_tap_blocks_contain(const char *needle, const char **hit)
{
    if (!currentDoc || !currentDoc->blocks || !needle)
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
        for (int ii = 0; ii < blk->inlineCount; ii++)
        {
            const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                                ? blk->inlines[ii]->text
                                : "";
            if (t && strstr(t, needle))
            {
                if (hit)
                {
                    *hit = t;
                }
                return 1;
            }
        }
    }
    return 0;
}

static int g_nrTapPhase = 0; /* 0=settle, 1=scan+tap, 2=observe, 3=done */
static unsigned g_nrTapFrames = 0;
static int g_nrTapErrsBefore = 0;
static char g_nrTapUrlBefore[128];
static int g_nrTapBlocksBefore = -1; /* C5: block count at tap time */
static DomNode *g_nrTapAnyNode = NULL; /* C5: clicked (formless) button node */
static char g_nrTapAriaPre[12];       /* C5: aria-expanded pre-tap value */

#if defined(PLUTO_NR_AUTOTEST_TYPE)
/* C4 typed-send: the contact handler only POSTs once its validation
 * passes (email regex, subject >= 2 chars, message >= 10), so the tap
 * autotest gains a typing pass before the submit: three form_key_write
 * commits. One write per tick — each commit's dispatched events run
 * react's onChange → state set → re-render in the microtask pump, and the
 * next tick re-scans a FRESH layout (stale item pointers would dangle).
 * The honeypot input (placeholder "Company (leave blank)", aria-hidden) is
 * skipped explicitly: its handler branch pretends-sent WITHOUT a fetch. */
static const char *const g_nrTypeTexts[3] = {
    "autotest@example.com",   /* passes the page's email regex */
    "Sim autotest subject",   /* >= 2 chars */
    "Hello from the PlutoBrowser simulator autotest message.", /* >= 10 */
};
static int g_nrTapField = 0;          /* next field to fill (typed order) */
static char g_nrTapSubmitAction[128]; /* captured at the submit scan */
/* The nth form-owned typeable field in layout (document) order that
 * matches the tapped submit's formAction (a <form> stamps all members
 * with resolve_href(action-or-"") — identical strings for same-form
 * items; formless inputs keep the OUTER w->formAction which FORM_END
 * restores). Scans fresh on every call; form_key_snapshot derives its
 * (form, name, ordinal) key from the SAME live layout, so this scan and
 * the write's resolution stay in sync. */
static const LayoutItem *nr_type_field_item(int nth)
{
    int seen = 0;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (!it || it->type != LRI_INPUT_FIELD || !it->name)
        {
            continue;
        }
        const char *fa = it->formAction ? it->formAction : "";
        if (strcmp(fa, g_nrTapSubmitAction) != 0)
        {
            continue; /* not a member of the tapped form */
        }
        if (it->placeholder && strstr(it->placeholder, "leave blank"))
        {
            continue; /* honeypot: fake-sent exit, no POST */
        }
        if (seen == nth)
        {
            return it;
        }
        seen++;
    }
    return NULL;
}
#endif /* PLUTO_NR_AUTOTEST_TYPE */

#if defined(PLUTO_NR_AUTOTEST_TAP2)
/* TAP2 (Send-another regression, 2026-10-18): after the C4 summary, re-run
 * the SAME tap path on react's FORMLESS "Send another" <button> and require
 * the form fields to come back in the LIVE DOM (an input under a <form>).
 * Catches the R58 finding: a setState flush that swaps the summary subtree
 * WITHOUT a re-walk leaves stale layout blocks + detached srcNodes (every
 * later tap dispatches to a dead node). Also closes the LOOP: after the
 * restore it re-enters phase 1 (re-type + re-send) for up to
 * g_nrTap2Cycle full rotations — proving send ↔ Send-another can repeat
 * indefinitely. */
static int g_nrTap2BlocksBefore;
static int g_nrTap2Cycle;

static int nr_tap_scan_form_field(const DomNode *n, int depth)
{
    int i;
    if (!n || n->kind != DOM_ELEMENT || !n->tag || depth > 8)
    {
        return 0;
    }
    if ((!strcmp(n->tag, "input") || !strcmp(n->tag, "textarea") ||
         !strcmp(n->tag, "select")) &&
        form_node_ancestor((DomNode *)n))
    {
        return 1;
    }
    if (!strcmp(n->tag, "script") || !strcmp(n->tag, "style") ||
        !strcmp(n->tag, "svg") || !strcmp(n->tag, "button"))
    {
        return 0;
    }
    for (i = 0; i < n->childCount; i++)
    {
        if (nr_tap_scan_form_field(n->children[i], depth + 1))
        {
            return 1;
        }
    }
    return 0;
}

static int nr_tap_form_fields_back(void)
{
    if (!currentDoc || !currentDoc->_dom)
    {
        return 0;
    }
    return nr_tap_scan_form_field(((DomResult *)currentDoc->_dom)->root, 0);
}
#endif /* PLUTO_NR_AUTOTEST_TAP2 */

static void nr_autotest_tap_tick(void)
{
    if (g_nrTapPhase == 3 || isRendering)
    {
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc || !g_pageJs)
    {
        g_nrTapFrames = 0; /* boot nav still in flight */
        return;
    }
    ++g_nrTapFrames;
    if (g_nrTapPhase == 0)
    {
        if (g_pageJs && jsbridge_eval_pending(g_pageJs))
        {
            if (g_nrTapFrames > 3600)
            {
                logger_log("[nr-autotest-tap] FAIL: eval never completed");
                g_nrTapPhase = 3;
            }
            return;
        }
        if (g_nrTapFrames < 360)
        {
            return; /* settle: hydration fallback + innerHTML rewalk land */
        }
        g_nrTapPhase = 1;
        g_nrTapFrames = 0;
        snprintf(g_nrTapUrlBefore, sizeof(g_nrTapUrlBefore), "%s",
                 currentDoc->baseUrl);
        g_nrTapErrsBefore = currentDoc->jsErrors;
        logger_log("[nr-autotest-tap] phase 1: scanning for stamped submit "
                   "block (url=%.80s)", g_nrTapUrlBefore);
    }
    if (g_nrTapPhase == 1)
    {
#if defined(PLUTO_NR_AUTOTEST_TYPE)
        if (g_nrTapSubmitAction[0] == '\0')
        {
            /* Resolve the submit's formAction so the typed field scan can
             * match same-form items (same predicate as the tap scan
             * below). */
            const LayoutItem *sub = NULL;
            for (int i = 0; i < layout_get_item_count(); i++)
            {
                const LayoutItem *it = layout_item_at(i);
                if (it && it->type == LRI_INPUT_SUBMIT && it->block &&
                    ((const DocBlock *)it->block)->srcNode &&
                    form_node_ancestor(
                        (DomNode *)((const DocBlock *)it->block)->srcNode))
                {
                    sub = it;
                    break;
                }
            }
            if (!sub)
            {
                logger_log("[nr-autotest-tap] FAIL: no stamped submit for "
                           "the type pass");
                g_nrTapPhase = 3;
                return;
            }
            snprintf(g_nrTapSubmitAction, sizeof(g_nrTapSubmitAction), "%s",
                     sub->formAction ? sub->formAction : "");
            logger_log("[nr-autotest-tap] type: submit formAction='%.70s'",
                       g_nrTapSubmitAction);
        }
        if (g_nrTapField <
            (int)(sizeof(g_nrTypeTexts) / sizeof(g_nrTypeTexts[0])))
        {
            const LayoutItem *f = nr_type_field_item(g_nrTapField);
            if (!f)
            {
                logger_log("[nr-autotest-tap] FAIL: field %d not found on "
                           "the tapped form (typed=%d)",
                           g_nrTapField, g_nrTapField);
                g_nrTapPhase = 3;
                return;
            }
            logger_log("[nr-autotest-tap] type[%d]: inputType=%s "
                       "placeholder='%.16s'",
                       g_nrTapField, f->inputType ? f->inputType : "?",
                       f->placeholder ? f->placeholder : "");
            form_key_snapshot(f);
            form_key_write(g_nrTypeTexts[g_nrTapField],
                           1 /* commit → input + change */);
            g_nrTapField++;
            return; /* next frame: fresh scan after the re-render settles */
        }
#endif
        /* Target the FORM submit control: the first LRI_INPUT_SUBMIT whose
         * stamped srcNode has a <form> ancestor (header/nav buttons carry
         * stamps too — the site's nav toggle sorts before the contact
         * form in document order, and a formless button never submits).
         * C5 (PLUTO_NR_AUTOTEST_ANYBUTTON): take the FIRST stamped button
         * regardless of a form ancestor — the nav/toggle class; its
         * click must re-render in place without a reload. */
        const LayoutItem *target = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_SUBMIT && it->block &&
                ((const DocBlock *)it->block)->srcNode
#if !defined(PLUTO_NR_AUTOTEST_ANYBUTTON)
                && form_node_ancestor(
                    (DomNode *)((const DocBlock *)it->block)->srcNode)
#endif
            )
            {
                target = it;
                break;
            }
        }
        if (!target)
        {
            logger_log("[nr-autotest-tap] FAIL: no stamped LRI_INPUT_SUBMIT "
                       "block on the page");
            g_nrTapPhase = 3;
            return;
        }
        DomNode *src = (DomNode *)((const DocBlock *)target->block)->srcNode;
        logger_log("[nr-autotest-tap] tap: submit block tag=%s index=%d "
                   "blocksTotal=%d",
                   (src && src->tag) ? src->tag : "?",
                   (int)layout_get_item_count(),
                   currentDoc ? currentDoc->blockCount : -1);
#if defined(PLUTO_NR_AUTOTEST_ANYBUTTON)
        /* C5: capture the clicked node + its aria-expanded pre-value —
         * the site's nav toggle flips aria-expanded/data-open via react
         * state (an ATTRIBUTE change; the menu stays structural). */
        g_nrTapAnyNode = src;
        const char *pv = dom_get_attr(src, "aria-expanded");
        snprintf(g_nrTapAriaPre, sizeof(g_nrTapAriaPre), "%s",
                 pv ? pv : "-");
        logger_log("[nr-autotest-tap] c5 tap aria-expanded='%s' blocks=%d",
                   g_nrTapAriaPre, currentDoc ? currentDoc->blockCount : -1);
#endif
        activate_form_block(target);
        g_nrTapBlocksBefore = currentDoc ? currentDoc->blockCount : -1; /* C5 */
        g_nrTapPhase = 2;
        g_nrTapFrames = 0;
        return;
    }
    if (g_nrTapPhase == 2)
    {
#if defined(PLUTO_NR_AUTOTEST_ANYBUTTON)
        /* C5: the stamped (formless) button is pure click dispatch — no
         * submit run, no typed fields. Browser-faithful acceptance = the
         * click's REACTIVE effect lands: the site's nav toggle flips
         * aria-expanded (react state → attribute update; the menu itself
         * never changes structure, so blockCount is the FALLBACK tell —
         * open/close lists). ~8s budget (240 pd-frames) mirrors the C4
         * wait; polls name the values every 60. */
        int nowBlocks = currentDoc ? currentDoc->blockCount : -1;
        const char *ariaNow =
            g_nrTapAnyNode ? dom_get_attr(g_nrTapAnyNode, "aria-expanded")
                           : NULL;
        const char *ariaCur = (ariaNow && ariaNow[0]) ? ariaNow : "-";
        int ariaFlipped =
            g_nrTapAnyNode && strcmp(g_nrTapAriaPre, ariaCur) != 0 &&
            strcmp(g_nrTapAriaPre, "-") != 0;
        if (g_nrTapFrames < 240)
        {
            if ((g_nrTapFrames % 60) == 0)
            {
                logger_log("[nr-autotest-tap] c5poll blocks=%d (was %d) "
                           "aria='%s' (was '%s') frames=%u",
                           nowBlocks, g_nrTapBlocksBefore, ariaCur,
                           g_nrTapAriaPre, g_nrTapFrames);
            }
            if (ariaFlipped)
            {
                logger_log("[nr-autotest-tap] C5 RESULT inPlace=yes "
                           "blocks=%d (was %d) clickReactive=yes "
                           "aria-expanded '%s'->'%s' jsErrors=%d",
                           nowBlocks, g_nrTapBlocksBefore, g_nrTapAriaPre,
                           ariaCur, currentDoc ? currentDoc->jsErrors : -1);
                g_nrTapPhase = 3;
                return;
            }
            if (g_nrTapBlocksBefore > 0 && nowBlocks > 0 &&
                nowBlocks != g_nrTapBlocksBefore)
            {
                logger_log("[nr-autotest-tap] C5 RESULT inPlace=yes "
                           "blocks=%d (was %d) clickReactive=yes "
                           "aria-unread jsErrors=%d",
                           nowBlocks, g_nrTapBlocksBefore,
                           currentDoc ? currentDoc->jsErrors : -1);
                g_nrTapPhase = 3;
                return;
            }
            return;
        }
        logger_log("[nr-autotest-tap] C5 RESULT inPlace=yes blocks=%d (was "
                   "%d) clickReactive=no aria='%s' (was '%s') jsErrors=%d",
                   nowBlocks, g_nrTapBlocksBefore, ariaCur, g_nrTapAriaPre,
                   currentDoc ? currentDoc->jsErrors : -1);
        g_nrTapPhase = 3;
        return;
#endif
#if defined(PLUTO_NR_AUTOTEST_TYPE)
        /* C4: wait until the send state settles (the handler's await of
         * the fetch POST resolves into the sent/error cards) or ~20s
         * runs out (pd frames ~30/s — 'Sending…' remains the fingerprint
         * of a stuck wire). */
        /* C4 (15:20 cycle): the real sent card's fingerprint block is
         * "Sent. Your message is on its way to bwandrych@…" — the
         * imagined "Send another" button never existed in the render. */
        int sentCard = nr_tap_blocks_contain("Sent.", NULL);
        int errState = nr_tap_blocks_contain("Something went wrong", NULL);
        if (!sentCard && !errState && g_nrTapFrames < 600)
        {
            if ((g_nrTapFrames % 150) == 0)
            {
                const char *sh = NULL;
                nr_tap_blocks_contain("Sending", &sh);
                logger_log("[nr-autotest-tap] sendingCard=%d frames=%u",
                           sh ? 1 : 0, g_nrTapFrames);
            }
            return;
        }
#endif
        if (g_nrTapFrames < 90)
        {
            return; /* let the dispatched submit/handlers finish */
        }
        int same = currentDoc && strcmp(currentDoc->baseUrl,
                                        g_nrTapUrlBefore) == 0;
        /* C3 textual proof: the suppressed submit must leave the form's
         * inline validation message in the rendered blocks (contact page:
         * "…a valid email address"). A miss is honest: errCard=0 with the
         * rest of the RESULT row still reported. */
        const char *errHit = NULL;
        int errCard = nr_tap_blocks_contain("valid email", &errHit);
        logger_log("[nr-autotest-tap] RESULT inPlace=%s jsErrors=%d (was %d) "
                   "blocks=%d errCard=%d"
#if defined(PLUTO_NR_AUTOTEST_TYPE)
                   " sentCard=%d errState=%d"
#endif
                   ,
                   same ? "yes" : "no",
                   currentDoc ? currentDoc->jsErrors : -1,
                   g_nrTapErrsBefore,
                   currentDoc ? currentDoc->blockCount : -1,
                   errCard
#if defined(PLUTO_NR_AUTOTEST_TYPE)
                   ,
                   sentCard, errState
#endif
        );
        if (errCard)
        {
            logger_log("[nr-autotest-tap] errCard text='%.60s'", errHit);
        }
#if defined(PLUTO_NR_AUTOTEST_TYPE)
        {
            const char *hit = NULL;
            if (sentCard && nr_tap_blocks_contain("Sent.", &hit))
            {
                logger_log("[nr-autotest-tap] sentCard text='%.60s'", hit);
            }
            if (errState && nr_tap_blocks_contain("Something went wrong",
                                                  &hit))
            {
                logger_log("[nr-autotest-tap] errState text='%.60s'", hit);
            }
        }
#endif
#if defined(PLUTO_NR_AUTOTEST_TAP2) && defined(PLUTO_NR_AUTOTEST_TYPE)
        if (sentCard)
        {
            /* TAP2: don't stop at the summary — prove the "Send another"
             * tap restores the fields, then loop back for another send. */
            g_nrTapPhase = 4;
            g_nrTapFrames = 0;
            logger_log("[nr-autotest-tap] TAP2 armed: summary up; settling "
                       "90 frames before the Send-another tap");
            return;
        }
#endif
        g_nrTapPhase = 3;
    }
#if defined(PLUTO_NR_AUTOTEST_TAP2)
    if (g_nrTapPhase == 4)
    {
        if (g_nrTapFrames < 90)
        {
            return; /* settle: let the summary's own re-walk land */
        }
        if (nr_tap_form_fields_back())
        {
            logger_log("[nr-autotest-tap] TAP2 skip: fields already back "
                       "pre-tap");
            g_nrTapPhase = 3;
            return;
        }
        g_nrTapPhase = 5;
        g_nrTapFrames = 0;
        logger_log("[nr-autotest-tap] TAP2: scanning for the summary's "
                   "formless Send-another button");
    }
    if (g_nrTapPhase == 5)
    {
        /* The summary's button is the BOTTOM-MOST formless stamped submit
         * (header nav toggles stamp too but sort first in document order;
         * the footer's GitHub/LinkedIn entries are anchors, not buttons).
         * "Send another" replaces the removed form, so it sits below the
         * sent text and the From/Subject rows. */
        const LayoutItem *target = NULL;
        int targetBlock = -1;
        int i;
        for (i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_SUBMIT && it->block &&
                ((const DocBlock *)it->block)->srcNode)
            {
                /* blocks[] is a pointer array — find its slot index */
                const DocBlock *blk = (const DocBlock *)it->block;
                int blockIdx = -1;
                int k;
                for (k = 0; currentDoc && currentDoc->blocks &&
                            k < currentDoc->blockCount;
                     k++)
                {
                    if ((const void *)currentDoc->blocks[k] ==
                        (const void *)blk)
                    {
                        blockIdx = k;
                    }
                }
                DomNode *src = (DomNode *)blk->srcNode;
#if defined(TARGET_SIMULATOR)
                /* SIM: the summary is react's OWN markup — "Send another"
                 * mounts OUTSIDE any form; the nav toggle stamps formless
                 * too but sorts FIRST in document order, so the BOTTOM-MOST
                 * formless stamped submit is the one to press. */
                if (!form_node_ancestor(src) && blockIdx > targetBlock)
                {
                    target = it;
                    targetBlock = blockIdx;
                }
#else
                /* DEVICE: the fallback rebuilt the summary INSIDE the
                 * <form> (its own card + injected button rides the form's
                 * subtree — that's exactly why the restore branch triggers
                 * on rc=0 submit), so the FIRST form-attached submit IS
                 * the Send-another; the original send button was detached
                 * along with the fields. */
                if (form_node_ancestor(src))
                {
                    target = it;
                    break;
                }
#endif
            }
        }
        if (!target)
        {
            if (g_nrTapFrames < 30)
            {
                return; /* layout may still be settling */
            }
            logger_log("[nr-autotest-tap] TAP2 FAIL: no formless stamped "
                       "button found in the summary layout");
            g_nrTapPhase = 3;
            return;
        }
        {
            DomNode *src =
                (DomNode *)((const DocBlock *)target->block)->srcNode;
            logger_log("[nr-autotest-tap] TAP2 tap: formless button tag=%s "
                       "node=%p block=%d blocks=%d cycle=%d",
                       (src && src->tag) ? src->tag : "?", (void *)src,
                       targetBlock, currentDoc ? currentDoc->blockCount : -1,
                       g_nrTap2Cycle);
        }
        g_nrTap2BlocksBefore = currentDoc ? currentDoc->blockCount : -1;
        activate_form_block(target);
        g_nrTapPhase = 6;
        g_nrTapFrames = 0;
        return;
    }
    if (g_nrTapPhase == 6)
    {
        int blocksNow = currentDoc ? currentDoc->blockCount : -1;
        if (nr_tap_form_fields_back())
        {
            logger_log("[nr-autotest-tap] TAP2 RESULT cycle=%d restored=yes "
                       "jsErrors=%d blocks=%d (was %d) frames=%u",
                       g_nrTap2Cycle,
                       currentDoc ? currentDoc->jsErrors : -1, blocksNow,
                       g_nrTap2BlocksBefore, g_nrTapFrames);
            if (g_nrTap2Cycle < 2)
            {
                /* LOOP proof: re-type + re-send; the next cycle re-arms
                 * the summary tap through identical machinery. */
                g_nrTap2Cycle++;
                g_nrTapField = 0;
                g_nrTapPhase = 1;
                g_nrTapFrames = 0;
                logger_log("[nr-autotest-tap] TAP2: looping back to phase 1 "
                           "(cycle %d: re-type + re-send)", g_nrTap2Cycle);
                return;
            }
            logger_log("[nr-autotest-tap] TAP2 RESULT loop=%d send<->restore "
                       "cycles closed — infinite loop proven",
                       g_nrTap2Cycle);
            g_nrTapPhase = 3;
            return;
        }
        if (g_nrTapFrames < 240)
        {
            if ((g_nrTapFrames % 60) == 0)
            {
                logger_log("[nr-autotest-tap] TAP2 poll frames=%u blocks=%d "
                           "(was %d)",
                           g_nrTapFrames, blocksNow, g_nrTap2BlocksBefore);
            }
            return;
        }
        logger_log("[nr-autotest-tap] TAP2 RESULT cycle=%d restored=no "
                   "blocks=%d (was %d) jsErrors=%d",
                   g_nrTap2Cycle, blocksNow, g_nrTap2BlocksBefore,
                   currentDoc ? currentDoc->jsErrors : -1);
        g_nrTapPhase = 3;
    }
#endif /* PLUTO_NR_AUTOTEST_TAP2 */
}
#endif /* PLUTO_NR_AUTOTEST_TAP */
#endif /* PLUTO_NR_AUTOTEST */

#if defined(PLUTO_EV_AUTOTEST)
/* [ev-test]: API-P1 event-battery driver (sim only; build with URL-mode
 * navigated to about:apiprobe). The probe's Events battery registers real
 * handlers on its own fixture (#evf/#evi/#evb + document-level rows); this
 * machine then drives the REAL pipeline: one form_key_write commit (input +
 * change dispatch), then activate_form_block on the probe's stamped submit
 * button (the tap → click-dispatch → form-submit-dispatch chain, B5
 * activation order). Verdict = the page's own counters painted into the
 * "apisev:" sum row (parsed from the re-walked blocks) + doc-pointer
 * identity: the form's submit handler calls preventDefault() so the
 * pipeline must branch at JSB_EVENT_SUPPRESSED into the live re-walk
 * (sameDoc) instead of submit_form's reload (newDoc — a reload rebuilds
 * the doc from markup, counters zeroed, field value reverted to "seed").
 * No engine change: this driver only OBSERVES the event lanes API-P1
 * verifies. */
static int g_evPhase = 0; /* 0=settle, 1=type, 2=press, 3=verdict, 4=done */
static unsigned g_evFrames = 0;
static int g_evErrsBefore = 0;
static DocParseResult *g_evDocBefore = NULL;
static int g_evBlocksBefore = -1;

static void ev_type_pass(void)
{
    const LayoutItem *f = NULL;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->type == LRI_INPUT_FIELD && it->name && it->inputType &&
            strcmp(it->inputType, "text") == 0)
        {
            f = it;
            break;
        }
    }
    if (!f)
    {
        logger_log("[ev-test] FAIL: no text field item on the probe page");
        g_evPhase = 4;
        return;
    }
    logger_log("[ev-test] type: name='%s' inputType=%s placeholder='%.16s'",
               f->name, f->inputType, f->placeholder ? f->placeholder : "");
    form_key_snapshot(f);
    form_key_write("pluto", 1 /* commit → input + change dispatch */);
    g_evPhase = 2;
    g_evFrames = 0;
    /* next tick: fresh scan after the dispatched handlers' re-walk settles */
}

static void ev_press_pass(void)
{
    const LayoutItem *target = NULL;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->type == LRI_INPUT_SUBMIT && it->block &&
            ((const DocBlock *)it->block)->srcNode &&
            form_node_ancestor(
                (DomNode *)((const DocBlock *)it->block)->srcNode))
        {
            target = it;
            break;
        }
    }
    if (!target)
    {
        logger_log("[ev-test] FAIL: no stamped form submit button");
        g_evPhase = 4;
        return;
    }
    DomNode *src = (DomNode *)((const DocBlock *)target->block)->srcNode;
    g_evDocBefore = currentDoc;
    g_evBlocksBefore = currentDoc->blockCount;
    g_evErrsBefore = currentDoc->jsErrors;
    logger_log("[ev-test] press: submitting via activate_form_block "
               "tag=%s blocks=%d",
               src->tag ? src->tag : "?", g_evBlocksBefore);
    activate_form_block(target); /* click + submit dispatch (B5 order) */
    g_evPhase = 3;
    g_evFrames = 0;
}

/* Scan the live blocks for the battery's sum row and parse its counters. */
static int ev_blocks_parse_sum(int *nin, int *nchg, int *nsub, int *nbub,
                               int *nbtn, int *ndoc)
{
    if (!currentDoc || !currentDoc->blocks)
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
        char text[160];
        size_t off = 0;
        text[0] = '\0';
        for (int ii = 0; ii < blk->inlineCount && off < sizeof(text) - 1;
             ii++)
        {
            const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                                ? blk->inlines[ii]->text
                                : "";
            size_t tl = strlen(t);
            if (tl > sizeof(text) - 1 - off)
            {
                tl = sizeof(text) - 1 - off;
            }
            memcpy(text + off, t, tl);
            off += tl;
        }
        text[off] = '\0';
        const char *p = strstr(text, "apisev: input=");
        if (!p)
        {
            continue;
        }
        if (sscanf(p, "apisev: input=%d change=%d submit=%d bub=%d "
                       "btn=%d doc=%d",
                   nin, nchg, nsub, nbub, nbtn, ndoc) == 6)
        {
            return 1;
        }
        return 0;
    }
    return 0;
}

static void ev_verdict(void)
{
    int nin = -1, nchg = -1, nsub = -1, nbub = -1, nbtn = -1, ndoc = -1;
    int found = ev_blocks_parse_sum(&nin, &nchg, &nsub, &nbub, &nbtn, &ndoc);
    int sameDoc = currentDoc == g_evDocBefore;
    int all = found && nin >= 1 && nchg >= 1 && nsub >= 1 && nbub >= 1 &&
              nbtn >= 1 && ndoc >= 1;
    logger_log("[ev-test] RESULT inPlace=%s jsErrors=%d (before %d) "
               "blocks=%d (was %d) found=%d input=%d change=%d submit=%d "
               "bub=%d btn=%d doc=%d",
               sameDoc ? "sameDoc" : "newDoc",
               currentDoc ? currentDoc->jsErrors : -1, g_evErrsBefore,
               currentDoc ? currentDoc->blockCount : -1, g_evBlocksBefore,
               found, nin, nchg, nsub, nbub, nbtn, ndoc);
    if (all && sameDoc)
    {
        logger_log("[ev-test] PASS: input+change+submit all delivered, "
                   "bubbling rows counted at document level, pd honored "
                   "(suppress re-walk, no reload)");
    }
    else if (!sameDoc)
    {
        logger_log("[ev-test] FAIL: doc replaced after submit press — "
                   "preventDefault did not own the default action");
    }
    else
    {
        logger_log("[ev-test] FAIL: counters incomplete (found=%d)", found);
    }
    g_evPhase = 4;
}

static void ev_autotest_tick(void)
{
    if (g_evPhase == 4 || isRendering)
    {
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc || !g_pageJs)
    {
        g_evFrames = 0; /* boot nav still in flight */
        return;
    }
    ++g_evFrames;
    if (g_evPhase == 0)
    {
        if (jsbridge_eval_pending(g_pageJs))
        {
            if (g_evFrames > 3600)
            {
                logger_log("[ev-test] FAIL: eval never completed");
                g_evPhase = 4;
            }
            return;
        }
        if (g_evFrames < 360)
        {
            return; /* settle: the battery scripts finish their pumps */
        }
        g_evPhase = 1;
        g_evFrames = 0;
        g_evErrsBefore = currentDoc->jsErrors;
        logger_log("[ev-test] phase 1: typing one commit into the probe "
                   "field (url=%.70s blocks=%d)",
                   currentDoc->baseUrl, currentDoc->blockCount);
        return;
    }
    if (g_evPhase == 1)
    {
        ev_type_pass();
        return;
    }
    if (g_evPhase == 2)
    {
        ev_press_pass();
        return;
    }
    if (g_evPhase == 3)
    {
        if (g_evFrames < 120)
        {
            return; /* let the submit handlers' re-walks land */
        }
        ev_verdict();
    }
}
#endif /* PLUTO_EV_AUTOTEST */

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

#if defined(PLUTO_REWALK_OOM_AUTOTEST)
/* TEMPORARY (sim autotest): prove the R59 re-walk CONTRACT — a walk that
 * dies mid-build (funnel refusal mid-run — Wikipedia device crash
 * 2026-10-18) must publish NOTHING and report failure instead of letting
 * the renderer lay out a half-freed doc (break_lines faulted on a reused
 * arena value as a block's inline array). Sequence: force the memory
 * funnel strait, re-walk directly (expect rc=-1 with the doc left empty
 * and clean, NO crash), then re-walk under the restored budget (expect
 * rc=0 with blocks) and hand the page to the normal render pipeline.
 * PASS/FAIL lands in pluto.log as [rewalk-oom] lines. */
extern int g_rewalkTestForceWalkError; /* document.c test seam */
static int g_rewalkOomPhase = 0; /* 0=strait test, 1=verify, 3=done */
static unsigned g_rewalkOomFrames = 0;
static unsigned long g_rewalkOomSavedBudget = 0;
static void rewalk_oom_autotest_tick(void)
{
    if (g_rewalkOomPhase == 3)
    {
        return;
    }
    ++g_rewalkOomFrames;
    if (g_rewalkOomFrames < 30)
    {
        return; /* settle ~0.5s */
    }
    if (g_rewalkOomPhase == 0)
    {
        if (currentState != STATE_PAGE || !currentDoc || !currentDoc->_dom)
        {
            if (g_rewalkOomFrames > 900)
            {
                logger_log("[rewalk-oom] FAIL: no live rendered page to "
                           "rewalk (state=%d)", currentState);
                g_rewalkOomPhase = 3;
            }
            return;
        }
        /* Hold the page pipeline like a real re-render does, so no frame
         * draws with stale/freed layout strings while the seam runs. */
        isRendering = 1;
        currentState = STATE_LOADING;
        g_rewalkOomSavedBudget = pluto_mem_budget();
        pluto_mem_set_budget(pluto_mem_live() + 3UL * 1024UL);
        int rc = document_rewalk(currentDoc);
        pluto_mem_set_budget(g_rewalkOomSavedBudget);
        if (rc == 0)
        {
            /* The strait deferred both refusals on this small doc —
             * engage the seam to drive the REAL failure branch. */
            logger_log("[rewalk-oom] budget strait deferred (rc=0); "
                       "engaging the forced-error seam");
            g_rewalkTestForceWalkError = 1;
            rc = document_rewalk(currentDoc);
            g_rewalkTestForceWalkError = 0;
        }
        if (rc != 0)
        {
            logger_log("[rewalk-oom] phase 0: failed-walk contract rc=%d, "
                       "doc left blocks=%d parseError=%d",
                       rc, currentDoc->blockCount, currentDoc->parseError);
            /* Recovery under the restored budget: re-walk must succeed and
             * the normal pipeline re-renders the healed doc. */
            rc = document_rewalk(currentDoc);
            int blocks = currentDoc->blockCount;
            int perr = currentDoc->parseError;
            isRendering = 0;
            page_rewalk_now(); /* full re-walk + layout + swap via tasks */
            if (rc == 0 && blocks > 0 && !perr)
            {
                logger_log("[rewalk-oom] recovery rewalk rc=0 blocks=%d — "
                           "PASS (fix verified, handed to the pipeline)",
                           blocks);
            }
            else
            {
                logger_log("[rewalk-oom] FAIL: recovery rewalk rc=%d "
                           "blocks=%d parseError=%d", rc, blocks, perr);
            }
            g_rewalkOomPhase = 1; /* verify the pipeline re-render landed */
        }
        else
        {
            isRendering = 0;
            page_rewalk_now(); /* hygiene re-render; nothing failed */
            g_rewalkOomPhase = 3;
        }
        g_rewalkOomFrames = 0;
        return;
    }
    /* Phase 1: the hand-off re-render completed and the page is back. */
    if (currentState == STATE_PAGE && currentDoc && currentDoc->blockCount > 0)
    {
        logger_log("[rewalk-oom] PASS end-to-end: page re-rendered after a "
                   "failed walk (blocks=%d, live=%lu)", currentDoc->blockCount,
                   pluto_mem_live());
        g_rewalkOomPhase = 3;
    }
    else if (g_rewalkOomFrames > 900)
    {
        logger_log("[rewalk-oom] FAIL: pipeline re-render never landed "
                   "(state=%d)", currentState);
        g_rewalkOomPhase = 3;
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
        /* Target the LAST test card (about:cssprobe, index bmCount+5):
         * the full test-card sweep, with the PASS row naming the opened
         * page. Card 1 is about:home, which special-cases back to
         * STATE_HOME (not a page render), so it cannot verify the
         * navigation path. Steer with the CRANK (one reading-order step
         * per call, clamps at the last selectable item — no skipped
         * indices, always terminates). */
        int target = 1 + storage_bookmark_count() + 6;
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
    /* Phase 1: verify the navigation landed on the LAST test page. */
    if (currentState == STATE_PAGE && currentDoc)
    {
        const HttpTestPage *pages = NULL;
        http_test_pages(&pages);
        const char *want = (pages && pages[6].name) ? pages[6].name
                                                    : "about:apiprobe";
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

#if defined(PLUTO_FORMCLICK_AUTOTEST)
/* TEMPORARY (autotest builds, sim + device): prove the about:acidtest
 * "Interactive Controls" section is really clickable. After the acidtest
 * page renders, drive the same activate_form_block() path a tap uses:
 *   radio: click the unchecked "ship" option, require group exclusivity
 *          (exactly one checked, and it is the clicked one);
 *   select: click the "size" dropdown, require selectedIndex to advance.
 * PASS/FAIL lands in pluto.log via [formclick-autotest] lines. */
static int g_formClickTestPhase = 0; /* 0=nav, 1=radio, 2=select, 3=focus, 4=twins, 5=done */
static unsigned g_formClickTestFrames = 0;
static int g_twinsStaged = 0;
static void formclick_autotest_tick(void)
{
    if (g_formClickTestPhase == 5 || isRendering)
    {
        return;
    }
    if (g_formClickTestPhase == 0)
    {
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:acidtest", 14) == 0)
        {
            g_formClickTestPhase = 1;
            g_formClickTestFrames = 0;
            return;
        }
        if (++g_formClickTestFrames == 1)
        {
            navigate_to("about:acidtest");
        }
        else if (g_formClickTestFrames > 600)
        {
            logger_log("[formclick-autotest] FAIL: never reached "
                       "about:acidtest");
            g_formClickTestPhase = 3;
        }
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc ||
        strncmp(currentDoc->baseUrl, "about:acidtest", 14) != 0)
    {
        return;
    }
    if (++g_formClickTestFrames < 30)
    {
        return; /* let the page settle ~0.5s */
    }
    if (g_formClickTestPhase == 1)
    {
        const LayoutItem *target = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_CHECKBOX_FIELD && it->radio &&
                it->name && strcmp(it->name, "ship") == 0 && it->block &&
                !((const DocBlock *)it->block)->checked)
            {
                target = it;
                break;
            }
        }
        if (!target)
        {
            logger_log("[formclick-autotest] FAIL: no unchecked ship "
                       "radio found");
            g_formClickTestPhase = 3;
            return;
        }
        activate_form_block(target);
        int checkedCount = 0;
        int targetChecked = 0;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_CHECKBOX_FIELD && it->radio &&
                it->name && strcmp(it->name, "ship") == 0 &&
                it->block && ((const DocBlock *)it->block)->checked)
            {
                checkedCount++;
                if (it->block == target->block)
                {
                    targetChecked = 1;
                }
            }
        }
        if (checkedCount == 1 && targetChecked)
        {
            logger_log("[formclick-autotest] PASS: radio click selected, "
                       "group exclusive");
        }
        else
        {
            logger_log("[formclick-autotest] FAIL: radio group state "
                       "checked=%d target=%d",
                       checkedCount, targetChecked);
        }
        g_formClickTestPhase = 2;
        return;
    }
    /* Phase 2: click the size dropdown, require selectedIndex to advance. */
    if (g_formClickTestPhase == 2)
    {
    const LayoutItem *sel = NULL;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->type == LRI_SELECT_FIELD && it->name &&
            strcmp(it->name, "size") == 0)
        {
            sel = it;
            break;
        }
    }
    if (!sel)
    {
        logger_log("[formclick-autotest] FAIL: size select not found");
        g_formClickTestPhase = 3;
        return;
    }
    {
        DocBlock *b = (DocBlock *)sel->block;
        int before = b ? b->selectedIndex : -1;
        int n = b ? b->optionCount : 0;
        activate_form_block(sel);
        int after = b ? b->selectedIndex : -2;
        int want = (n > 0) ? ((before % n) + 1) : before;
        if (n > 0 && after == want && after != before)
        {
            logger_log("[formclick-autotest] PASS: select click %d -> %d",
                       before, after);
        }
        else
        {
            logger_log("[formclick-autotest] FAIL: select index %d -> %d "
                       "(want %d, n=%d)",
                       before, after, want, n);
        }
    }
    g_formClickTestPhase = 3;
    return;
    } /* end phase 2 */
    /* Phase 3: every interactive control must be a cursor stop. Walk the
     * same link table the D-pad uses and require focusable entries for
     * both ship radios and the size dropdown. */
    if (g_formClickTestPhase == 3)
    {
    const int total = lm_get_link_count();
    int shipStops = 0;
    int sizeStops = 0;
    for (int i = 1; i <= total; i++)
    {
        const LMRectAux *aux = lm_rect_aux(i, 0);
        if (aux && aux->isFormInput && aux->inputItem)
        {
            const LayoutItem *it = (const LayoutItem *)aux->inputItem;
            if (it->name && strcmp(it->name, "ship") == 0)
            {
                shipStops++;
            }
            else if (it->name && strcmp(it->name, "size") == 0)
            {
                sizeStops++;
            }
        }
    }
    if (shipStops == 2 && sizeStops == 1)
    {
        logger_log("[formclick-autotest] PASS: cursor stops ship=%d "
                   "size=%d (of %d links)",
                   shipStops, sizeStops, total);
    }
    else
    {
        logger_log("[formclick-autotest] FAIL: cursor stops ship=%d "
                   "(want 2) size=%d (want 1) of %d links",
                   shipStops, sizeStops, total);
    }
    g_formClickTestPhase = 4;
    return;
    } /* end phase 3 */
    /* Phase 4: twin text fields (same name, the contact-page shape). Type
     * into twin 0, force a full rebuild mid-session, then type into twin 1
     * through the keyboard key path. Each twin must keep its own text —
     * the stale-pointer bug put twin 1's keystrokes into twin 0. */
    if (g_formClickTestPhase == 4)
    {
        const LayoutItem *w0 = NULL;
        const LayoutItem *w1 = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_FIELD && it->name &&
                strcmp(it->name, "who") == 0)
            {
                if (!w0)
                {
                    w0 = it;
                }
                else if (!w1)
                {
                    w1 = it;
                }
            }
        }
        if (!w0 || !w1)
        {
            logger_log("[formclick-autotest] FAIL: twin who fields not "
                       "found");
            g_formClickTestPhase = 5;
            return;
        }
        if (!g_twinsStaged)
        {
            form_key_snapshot(w0);
            form_key_write("AAA-first", 1 /* commit → input + change */);
            page_rewalk_now(); /* rebuild everything under the keyboard */
            g_twinsStaged = 1;
            g_formClickTestFrames = 0;
            return;
        }
        if (currentState != STATE_PAGE || ++g_formClickTestFrames < 30)
        {
            return;
        }
        /* Re-resolve twin 1 AFTER the rebuild and type through the key. */
        const LayoutItem *u1 = NULL;
        int seen = 0;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_FIELD && it->name &&
                strcmp(it->name, "who") == 0 && seen++ == 1)
            {
                u1 = it;
                break;
            }
        }
        if (!u1)
        {
            logger_log("[formclick-autotest] FAIL: twin 1 gone after "
                       "rebuild");
            g_formClickTestPhase = 5;
            return;
        }
        form_key_snapshot(u1);
        form_key_write("BBB-second", 1 /* commit → input + change */);
        const LayoutItem *v0 = NULL;
        const LayoutItem *v1 = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_FIELD && it->name &&
                strcmp(it->name, "who") == 0)
            {
                if (!v0)
                {
                    v0 = it;
                }
                else if (!v1)
                {
                    v1 = it;
                }
            }
        }
        const char *s0 = v0 ? form_item_value(v0) : "?";
        const char *s1 = v1 ? form_item_value(v1) : "?";
        if (v0 && v1 && strcmp(s0, "AAA-first") == 0 &&
            strcmp(s1, "BBB-second") == 0)
        {
            logger_log("[formclick-autotest] PASS: twins keep own text "
                       "across rebuild ('%s' / '%s')", s0, s1);
        }
        else
        {
            logger_log("[formclick-autotest] FAIL: twin text cross-talk "
                       "'%s' / '%s'", s0, s1);
        }
        g_formClickTestPhase = 5;
        return;
    }
}
#endif

#if defined(PLUTO_FORMVAL_AUTOTEST)
/* Forms-Phase 3 driver (simulator-only): prove the NATIVE validation
 * layer on the about:formval real page — no JS validation logic there,
 * the page's script only paints an invalid counter.
 *   phase 1: tap submit on an EMPTY required field → the gate must
 *            BLOCK (URL stays about:formval, nothing on the wire) and
 *            the non-bubbling invalid event must fire once (the painted
 *            counter reads invalidcount=1 after a re-walk);
 *   phase 2: type through the real keyboard lane (form_key_snapshot +
 *            form_key_write — the tap/keyboard path a user rides);
 *   phase 3: tap submit again → the submit must now GO (the standard
 *            GET wire navigates to about:formprobe — Forms-Phase 6
 *            makes the query the browser-true form data set: the typed
 *            pair(s) once, no legacy re-append);
 *   phase 4: the dialog-method hold (Forms-Phase 6) — tap the battery
 *            page's #fpdlg (method="dialog") submit button through the
 *            SAME activation lane: with no ancestor dialog element the
 *            submission is the spec's no-op and the URL must not move.
 * PASS/FAIL lands in pluto.log via [formval-autotest] lines. */
static int g_formvalPhase = 0; /* 0=nav, 1=blocked, 2=type, 3=pass, 4=done */
static unsigned g_formvalFrames = 0;
static char g_formvalHoldUrl[640] = ""; /* Forms-Phase 6: the hold snapshot */

/* Forms-Phase 6: a bounded DOM walk matching an element's own id
 * attribute (the same walk shape the Phase-4 helpers use). The fpdlg
 * submit button is the dialog-hold tap target on the battery page. */
static DomNode *formval_find_node_by_id(const char *wantId)
{
    DomResult *dom = currentDoc ? (DomResult *)currentDoc->_dom : NULL;
    if (!dom || !dom->root || !wantId)
    {
        return NULL;
    }
    DomNode *stack[32];
    int top = 0;
    stack[top++] = dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (!n)
        {
            continue;
        }
        if (n->kind == DOM_ELEMENT)
        {
            const char *id = dom_get_attr(n, "id");
            if (id && strcmp(id, wantId) == 0)
            {
                return n;
            }
        }
        for (int i = n->childCount - 1; i >= 0 && top < 32; i--)
        {
            if (n->children[i])
            {
                stack[top++] = n->children[i];
            }
        }
    }
    return NULL;
}
static void formval_autotest_tick(void)
{
    if (g_formvalPhase == 4 || isRendering)
    {
        return;
    }
    if (g_formvalPhase == 0)
    {
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formval", 13) == 0)
        {
            g_formvalPhase = 1;
            g_formvalFrames = 0;
            return;
        }
        if (++g_formvalFrames == 1)
        {
            navigate_to("about:formval");
        }
        else if (g_formvalFrames > 600)
        {
            logger_log("[formval-autotest] FAIL: never reached about:formval");
            g_formvalPhase = 4;
        }
        return;
    }
    if (currentState != STATE_PAGE || !currentDoc)
    {
        return;
    }
    /* phases 1..21 ride the formval page; phases 31..33 are the POST-nav
     * verdicts (the submit navigated away — run them wherever we are) */
    if (g_formvalPhase != 31 && g_formvalPhase != 32 && g_formvalPhase != 33 &&
        strncmp(currentDoc->baseUrl, "about:formval", 13) != 0)
    {
        return;
    }
    if (++g_formvalFrames < 30)
    {
        return; /* let the page settle ~0.5s */
    }
    if (g_formvalPhase == 1)
    {
        const LayoutItem *submit = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_SUBMIT)
            {
                submit = it;
                break;
            }
        }
        if (!submit)
        {
            logger_log("[formval-autotest] FAIL: submit item not found");
            g_formvalPhase = 4;
            return;
        }
        logger_log("[formval-autotest] tap submit (empty required)");
        activate_form_block(submit);
        g_formvalFrames = 0;
        g_formvalPhase = 2;
        return;
    }
    if (g_formvalPhase == 2)
    {
        /* Verdict for the blocked tap: URL unchanged + the page painted
         * invalidcount=1 (re-walk so the laid text shows the update). */
        int urlHeld =
            currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formval", 13) == 0 &&
            !strchr(currentDoc->baseUrl, '?');
        if (!urlHeld)
        {
            logger_log("[formval-autotest] FAIL: empty submit left the page "
                       "(url=%s)", currentDoc ? currentDoc->baseUrl : "?");
            g_formvalPhase = 4;
            return;
        }
        page_rewalk_now();
        /* the painted-text scan runs after the next settle window */
        g_formvalFrames = 0;
        g_formvalPhase = 21;
        return;
    }
    if (g_formvalPhase == 21)
    {
        int painted = 0;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_TEXT && it->text &&
                strncmp(it->text, "invalidcount=", 13) == 0)
            {
                if (strcmp(it->text, "invalidcount=1") == 0)
                {
                    painted = 1;
                }
            }
        }
        if (painted)
        {
            logger_log("[formval-autotest] PASS: blocked at the native gate, "
                       "URL unchanged, invalid painted 1");
        }
        else
        {
            logger_log("[formval-autotest] FAIL: invalid count not painted "
                       "(expected invalidcount=1)");
        }
        /* phase 2 proper: type through the real keyboard lane */
        const LayoutItem *user = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_FIELD && it->name &&
                strcmp(it->name, "user") == 0)
            {
                user = it;
                break;
            }
        }
        if (!user)
        {
            logger_log("[formval-autotest] FAIL: user field not found");
            g_formvalPhase = 4;
            return;
        }
        form_key_snapshot(user);
        form_key_write("Pluto", 1 /* commit → input + change */);
        page_rewalk_now(); /* rebuild under the live attr value */
        g_formvalFrames = 0;
        g_formvalPhase = 3;
        return;
    }
    if (g_formvalPhase == 3)
    {
        const LayoutItem *submit = NULL;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_INPUT_SUBMIT)
            {
                submit = it;
                break;
            }
        }
        if (!submit)
        {
            logger_log("[formval-autotest] FAIL: submit item gone");
            g_formvalPhase = 4;
            return;
        }
        logger_log("[formval-autotest] tap submit (typed value)");
        activate_form_block(submit);
        g_formvalFrames = 0;
        g_formvalPhase = 31;
        return;
    }
    if (g_formvalPhase == 31)
    {
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formprobe", 15) == 0)
        {
            logger_log("[formval-autotest] PASS: valid submit navigated the "
                       "GET wire (url=%s)", currentDoc->baseUrl);
        }
        else
        {
            logger_log("[formval-autotest] FAIL: valid submit did not "
                       "navigate (url=%s)",
                       currentDoc ? currentDoc->baseUrl : "?");
        }
        /* Forms-Phase 6: now the dialog-method hold on the battery page
         * the GET landed on. */
        g_formvalPhase = 32;
        g_formvalFrames = 0;
        return;
    }
    if (g_formvalPhase == 32)
    {
        if (!currentDoc ||
            strncmp(currentDoc->baseUrl, "about:formprobe", 15) != 0)
        {
            if (++g_formvalFrames > 600)
            {
                logger_log("[formval-autotest] FAIL: the dialog tap never "
                           "found the battery page (url=%s)",
                           currentDoc ? currentDoc->baseUrl : "?");
                g_formvalPhase = 4;
            }
            return;
        }
        if (++g_formvalFrames < 30)
        {
            return; /* let the battery page settle */
        }
        DomNode *btn = formval_find_node_by_id("fpdlgb");
        if (!btn)
        {
            logger_log("[formval-autotest] FAIL: #fpdlgb button not found");
            g_formvalPhase = 4;
            return;
        }
        const LayoutItem *it = form_item_by_node(btn);
        if (!it)
        {
            logger_log("[formval-autotest] FAIL: dialog submit item not "
                       "found in the layout");
            g_formvalPhase = 4;
            return;
        }
        snprintf(g_formvalHoldUrl, sizeof(g_formvalHoldUrl), "%s",
                 currentDoc->baseUrl);
        logger_log("[formval-autotest] tap method=dialog submit (hold "
                   "expected, url=%s)", g_formvalHoldUrl);
        activate_form_block(it);
        g_formvalFrames = 0;
        g_formvalPhase = 33;
        return;
    }
    if (g_formvalPhase == 33)
    {
        /* The hold verdict: 30 settle frames, no navigation, URL
         * byte-identical (the node-id lookup rides the layout re-walk
         * the document-level click listener can paint — both sides of
         * the build parity are safe to compare). */
        if (currentDoc && strcmp(currentDoc->baseUrl, g_formvalHoldUrl) == 0)
        {
            logger_log("[formval-autotest] PASS: method=dialog held the "
                       "page (url=%s unchanged)", currentDoc->baseUrl);
        }
        else
        {
            logger_log("[formval-autotest] FAIL: method=dialog left the "
                       "page (url=%s, was %s)",
                       currentDoc ? currentDoc->baseUrl : "?",
                       g_formvalHoldUrl);
        }
        g_formvalPhase = 4;
        return;
    }
}
#endif

#if defined(PLUTO_FORMENTER_AUTOTEST)
/* Forms-Phase 4 driver (simulator-only): the Enter/reset/submit-API
 * battery on the about:formval real page. Each flavor asserts right
 * where a browser would:
 *   phase 1: requestSubmit-flavor (gate+event+wire) on the empty
 *            required field → the native gate BLOCKS (URL held,
 *            invalidcount=1 painted after the re-walk);
 *   phase 2: submit() flavor (no gate, no event) on the SAME state →
 *            the wire navigates despite the invalid field — the
 *            two flavors diverge, exactly like a browser;
 *   phase 3: navigate back to about:formval (fresh walk);
 *   phase 4: type "Pluto" through the real keyboard lane — NO re-walk
 *            in between (the block's walk-time default stays "");
 *   phase 5: form.reset() (the host hook) → the typed value returns
 *            to "" (block + value attribute + focused echo restored,
 *            no events);
 *   phase 6: retype via the OK-commit lane (form_kb_ok_commit — the
 *            exact did_hide journey) → the Enter chain rides the
 *            default submit button (click → gate → submit → wire);
 *   phase 7: the navigation landed on about:formprobe?user=EnterValid
 *            (&age= — Forms-Phase 6 makes the query the browser-true
 *            form data set; the old legacy walk skipped the empty
 *            number pair).
 * PASS/FAIL lands in pluto.log via [forment-autotest] lines. */
static int g_formentPhase = 0; /* 0..7 as listed, 9 = done */
static unsigned g_formentFrames = 0;

/* The formval page's single form (id="fv") — found by a bounded walk. */
static DomNode *forment_find_form_fv(void)
{
    DomResult *dom = currentDoc ? (DomResult *)currentDoc->_dom : NULL;
    if (!dom || !dom->root)
    {
        return NULL;
    }
    DomNode *stack[24];
    int top = 0;
    stack[top++] = dom->root;
    while (top > 0)
    {
        DomNode *n = stack[--top];
        if (!n)
        {
            continue;
        }
        if (n->kind == DOM_ELEMENT && n->tag &&
            strcmp(n->tag, "form") == 0)
        {
            const char *id = dom_get_attr(n, "id");
            if (id && strcmp(id, "fv") == 0)
            {
                return n;
            }
        }
        for (int i = n->childCount - 1; i >= 0 && top < 24; i--)
        {
            if (n->children[i])
            {
                stack[top++] = n->children[i];
            }
        }
    }
    return NULL;
}

static const LayoutItem *forment_user_field(void)
{
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->type == LRI_INPUT_FIELD && it->name &&
            strcmp(it->name, "user") == 0)
        {
            return it;
        }
    }
    return NULL;
}

static void forment_autotest_tick(void)
{
    if (g_formentPhase == 9 || isRendering)
    {
        return;
    }
    if (g_formentPhase == 0)
    {
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formval", 13) == 0)
        {
            g_formentPhase = 1;
            g_formentFrames = 0;
            return;
        }
        if (++g_formentFrames == 1)
        {
            logger_log("[forment-autotest] navigate to about:formval");
            navigate_to("about:formval");
        }
        else if (g_formentFrames > 600)
        {
            logger_log("[forment-autotest] FAIL: never reached about:formval");
            g_formentPhase = 9;
        }
        return;
    }
    /* Phases 2/3/7 ride whichever page the wire landed on; everything
     * else drives the about:formval page itself. */
    if (g_formentPhase != 2 && g_formentPhase != 3 && g_formentPhase != 7 &&
        (currentState != STATE_PAGE || !currentDoc ||
         strncmp(currentDoc->baseUrl, "about:formval", 13) != 0))
    {
        return;
    }
    if (++g_formentFrames < 30)
    {
        return; /* let the page settle ~0.5s */
    }
    if (g_formentPhase == 1)
    {
        DomNode *form = forment_find_form_fv();
        if (!form)
        {
            logger_log("[forment-autotest] FAIL: #fv form node not found");
            g_formentPhase = 9;
            return;
        }
        logger_log("[forment-autotest] requestSubmit flavor on the empty "
                   "required field");
        (void)pluto_host_form_wire(g_pageJs, form, NULL, 1, 1);
        page_rewalk_now();
        g_formentFrames = 0;
        g_formentPhase = 11; /* assert lane (formval numbering tail) */
        return;
    }
    if (g_formentPhase == 11)
    {
        int urlHeld = currentDoc && !strchr(currentDoc->baseUrl, '?');
        int painted = 0;
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it && it->type == LRI_TEXT && it->text &&
                strcmp(it->text, "invalidcount=1") == 0)
            {
                painted = 1;
            }
        }
        if (urlHeld && painted)
        {
            logger_log("[forment-autotest] PASS: requestSubmit blocked at "
                       "the native gate (invalid painted, URL held)");
        }
        else
        {
            logger_log("[forment-autotest] %s: FAIL url=%s painted=%d",
                       "requestSubmit block", currentDoc ? currentDoc->baseUrl : "?",
                       painted);
        }
        /* The submit() flavor on the SAME state: no gate, no event. */
        DomNode *form = forment_find_form_fv();
        if (!form)
        {
            logger_log("[forment-autotest] FAIL: #fv form gone before the "
                       "submit() flavor");
            g_formentPhase = 9;
            return;
        }
        logger_log("[forment-autotest] submit() flavor — skips validation");
        (void)pluto_host_form_wire(g_pageJs, form, NULL, 0, 0);
        g_formentFrames = 0;
        g_formentPhase = 2;
        return;
    }
    if (g_formentPhase == 2)
    {
        DomResult *dom = currentDoc ? (DomResult *)currentDoc->_dom : NULL;
        (void)dom;
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formprobe?", 16) == 0)
        {
            logger_log("[forment-autotest] PASS: submit() sent the wire "
                       "despite the invalid field (url=%s) — the flavors "
                       "diverge",
                       currentDoc->baseUrl);
            g_formentPhase = 3;
            g_formentFrames = 0;
            return;
        }
        if (++g_formentFrames > 600)
        {
            logger_log("[forment-autotest] FAIL: submit() did not navigate "
                       "(url=%s)", currentDoc ? currentDoc->baseUrl : "?");
            g_formentPhase = 3;
            g_formentFrames = 0;
        }
        return;
    }
    if (g_formentPhase == 3)
    {
        logger_log("[forment-autotest] navigate back to about:formval (fresh "
                   "walk)");
        navigate_to("about:formval");
        g_formentFrames = 0;
        g_formentPhase = 4;
        return;
    }
    if (g_formentPhase == 4)
    {
        const LayoutItem *user = forment_user_field();
        if (!user)
        {
            logger_log("[forment-autotest] FAIL: user field not found");
            g_formentPhase = 9;
            return;
        }
        form_key_snapshot(user);
        form_key_write("Pluto", 1 /* commit → input + change */);
        const char *v = form_item_value(user);
        if (v && strcmp(v, "Pluto") == 0)
        {
            logger_log("[forment-autotest] Yes: typed 'Pluto' (item value "
                       "read back, no re-walk)");
            g_formentPhase = 5;
        }
        else
        {
            logger_log("[forment-autotest] FAIL: typed value not readable "
                       "(got '%s')", v ? v : "(null)");
            g_formentPhase = 9;
        }
        g_formentFrames = 0;
        return;
    }
    if (g_formentPhase == 5)
    {
        DomNode *form = forment_find_form_fv();
        if (!form)
        {
            logger_log("[forment-autotest] FAIL: #fv form gone at reset");
            g_formentPhase = 9;
            return;
        }
        logger_log("[forment-autotest] form.reset() hook");
        int changed = pluto_host_form_reset(g_pageJs, form);
        const LayoutItem *user = forment_user_field();
        const char *v = user ? form_item_value(user) : NULL;
        if (changed && v && v[0] == '\0')
        {
            logger_log("[forment-autotest] PASS: form.reset() restored the "
                       "walk-time default (typed 'Pluto' → '')");
        }
        else
        {
            logger_log("[forment-autotest] FAIL: reset state changed=%d "
                       "value='%s'",
                       changed, v ? v : "(null)");
        }
        g_formentPhase = 6;
        g_formentFrames = 0;
        return;
    }
    if (g_formentPhase == 6)
    {
        const LayoutItem *user = forment_user_field();
        if (!user)
        {
            logger_log("[forment-autotest] FAIL: user field gone at Enter");
            g_formentPhase = 9;
            return;
        }
        logger_log("[forment-autotest] OK-commit → implicit Enter chain");
        form_key_snapshot(user);
        form_kb_ok_commit("EnterValid");
        g_formentFrames = 0;
        g_formentPhase = 7;
        return;
    }
    if (g_formentPhase == 7)
    {
        if (currentDoc &&
            strncmp(currentDoc->baseUrl, "about:formprobe?", 16) == 0 &&
            strstr(currentDoc->baseUrl, "user=EnterValid"))
        {
            logger_log("[forment-autotest] PASS: Enter rode the default "
                       "button through the full chain (url=%s)",
                       currentDoc->baseUrl);
            g_formentPhase = 9;
            return;
        }
        if (++g_formentFrames > 600)
        {
            logger_log("[forment-autotest] FAIL: Enter chain did not submit "
                       "(url=%s)",
                       currentDoc ? currentDoc->baseUrl : "?");
            g_formentPhase = 9;
        }
        return;
    }
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
#ifdef PLUTO_NAV_AUTOTEST_ENGINE
/* Optional engine force for the run: 0=muJS 1=Duktape 2=QuickJS 3=XS
 * (Moddable). Storage AND the live router are set before navigation; the
 * [jsbridge] page attach / [js] close[<name>] lines prove which engine
 * binaries ran the page. Mirrors the other *_AUTOTEST_ENGINE seams. */
#define NAV_AUTOTEST_ENGINE_VAL PLUTO_NAV_AUTOTEST_ENGINE
#endif
#ifndef NAV_AUTOTEST_ENGINE_VAL
#define NAV_AUTOTEST_ENGINE_VAL (-1)
#endif
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
        if (NAV_AUTOTEST_ENGINE_VAL >= 0)
        {
            static int navEngineApplied = 0;
            if (!navEngineApplied)
            {
                storage_set_setting_int("jsEngine", NAV_AUTOTEST_ENGINE_VAL);
                jsbridge_set_engine(NAV_AUTOTEST_ENGINE_VAL);
                if (storage_setting_int("jsEnabled") != 2)
                {
                    storage_set_setting_int("jsEnabled", 2); /* JS Full */
                    logger_log("[nav-autotest] jsEnabled bumped to 2 (Full)");
                }
                logger_log("[nav-autotest] engine forced: %d",
                           (int)NAV_AUTOTEST_ENGINE_VAL);
                navEngineApplied = 1;
            }
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

#if defined(PLUTO_NR_AUTOTEST) && !defined(TARGET_PLAYDATE)
/* Phase-1 probe verifier (sim autotest builds only): about:cssprobe v2
 * renders hide-based canaries. The [render-result] sample rows print at
 * most 24 blocks, so this scans EVERY block text and logs [cssprobe]
 * rows — absent-markers prove each Phase-1 feature MATCHED (the rule
 * hid the element), present-markers prove the control stayed visible.
 * Pass only when every check agrees. */
/* Phase 10: compose a block's text the way the k[] battery does (code
 * blocks carry .text, images print as src, everything else reads its
 * inline runs). The anim battery reuses this so the marker→block find
 * stays one idiom. */
static void cssprobe_block_text(const DocBlock *blk, char *text, size_t cap)
{
    size_t off = 0;
    text[0] = '\0';
    if (blk->type == DOC_BLOCK_CODE_BLOCK && blk->text)
    {
        snprintf(text, cap, "%.190s", blk->text);
        return;
    }
    if (blk->type == DOC_BLOCK_IMAGE && blk->src)
    {
        snprintf(text, cap, "<img src=%.40s>", blk->src);
        return;
    }
    for (int ii = 0; ii < blk->inlineCount && off < cap - 1; ii++)
    {
        const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                            ? blk->inlines[ii]->text
                            : "";
        size_t tl = strlen(t);
        if (tl > cap - 1 - off)
        {
            tl = cap - 1 - off;
        }
        memcpy(text + off, t, tl);
        off += tl;
    }
    text[off] = '\0';
}

static void cssprobe_check(DocParseResult *doc)
{
    if (!doc || strncmp(doc->title, "CSS Probe", 9) != 0)
    {
        return;
    }
    static const struct
    {
        const char *mark;
        unsigned char wantPresent;
    } k[] = {
        {"ADJ-HIT", 0},       {"ADJ-KEEP", 1},
        {"GENSIB-HIT", 0},    {"GENSIB-A", 1},
        {"ATTCARET-HIT", 0},  {"ATTCARET-KEEP", 1},
        {"ATTDOLLAR-HIT", 0}, {"ATTDOLLAR-KEEP", 1},
        {"ATTSTAR-HIT", 0},   {"ATTSTAR-KEEP", 1},
        {"ATTTILDE-HIT", 0},  {"ATTTILDE-KEEP", 1},
        {"ATTBAR-HIT", 0},    {"ATTBAR-KEEP", 1},
        {"NOT-HIT", 0},       {"NOT-KEEP", 1},
        {"STATE-HIT", 0},     {"STATE-KEEP", 1},
        {"CANARY-DISPLAY-NONE", 0},
        {"VAR-HIT", 0},       {"VAR-KEEP", 1},
        {"VARFALL-HIT", 0},
        {"MQ-HIT", 0},        {"MQ-KEEP", 1},
        {"MQEM-HIT", 0},      {"MQAND-HIT", 0},
        {"MQNOT-KEEP", 1},    {"MQOR-HIT", 0},
        {"MQCOLOR-KEEP", 1},  {"MQCONTRAST-HIT", 0},
        {"MQUNKNOWN-KEEP", 1},
        {"FIXED-HIT", 0},     {"ABS-HIT", 0},
        {"ABSOFF-HIT", 0},    {"ABSSTATIC-KEEP", 1},
        {"ZKEEP", 1},         {"REL-KEEP", 1},
        {"SROFF-HIT", 0},     {"SRTINY-HIT", 0},
        {"SRKEEP", 1},        {"OVKEEP", 1},
        {"ELLKEEP", 1},       {"CLAMPKEEP", 1},
        {"FIXGAPA", 1},       {"FIXGAPB", 1},
        {"FGAPC", 1},         {"FGAPD", 1},
        {"FXROWA", 1},        {"FXROWB", 1},
        {"FAICC", 1},         {"FAICD", 1},
        {"FXNA", 1},          {"FXSB", 1},       {"FXNC", 1},
        {"FGNPX", 1},         {"AINC", 1},
        /* Phase 7 pseudo-element + truth-state canaries: the content
         * cascade now LANDS (letters inline span PBEF/PAFT + block-lane
         *anon BCARROW), attr(href) prints, ::marker feeds the doc
         * field, and the truth states hide/stay via REAL matchers. */
        {"PSBEF-SPMID-PSAFT", 1},
        {"BCARROW", 1},
        {"BCBODY", 1},
        {"ARWDROP", 1},
        {"AROWSTRAY", 0},
        /* attr() prints via a cssprobe-LOCAL ::after canary (the
         * acidtest page's [href] canary rides its own fixture —
         * cross-page markers never asserted here) — data-url proves
         * the attr() value language beyond href, brackets prove the
         * quotes/concat grammar. */
        {" [https://probe.example/beacon]", 1},
        {"MARKORD bullet text", 1},
        {"REMPT bullet-less", 1},
        {"ROOTVARHIT", 0},
        {"ROOTKEEP", 1},
        {"CHKHIT", 0},
        {"CHKKEEP", 1},
        /* Button blocks carry their label as a form field (never block
         * inlines), so button SELF-state is unassertable by text scan —
         * the sibling-text canaries carry the :disabled/:enabled proof:
         * DAFTGONE hidden (btnd:disabled + p), EAFTEL-enabled-sibling
         * hidden (btnE:enabled + .afte), DSTKEEP visible (the
         * un-disabled boundary — the rules hide only via a REAL match).
         * BTNgone/btnkeep SELF rows removed (would never render as
         * block text). */
        {"DAFTGONE", 0},
        {"EAFTEL enabled-sibling", 0},
        {"DSTKEEP", 1},
        {"PHELDGONE", 0},
        {"FITHIT", 0},
        {"TGTHIT", 1},
        {"EMPHIT", 0},
        {"EV2NOTEMPTY", 1},
        {"EKKEEP", 1},
        /* Phase 8 interactive set: template stays inert, dialog rides
         * the open attr, the closed details keeps its body hidden (its
         * default "> Details" summary line prints), and the srcset
         * picks assert their URLs in the dedicated loops below. */
        {"TMPLJUNK", 0},   {"TMPLKEEP", 1},
        {"DLGCLOSED", 0},  {"DLGOPEN", 1},
        {"DETBODY", 0},
        /* Phase 9 transforms/units/gradients: presence rows only — the
         * translate mounts assert EXACT field values in the tf battery
         * below, the gradient hero's polarity reads its invert flag,
         * the margin repairs assert spacing, and the 120vmin media rule
         * consumes (VMINHIT hides; every clause rides KNOWN vocabulary,
         * so the [css-skip] tally stays byte-stable). */
        {"TFSHIFTX", 1},
        {"TFSHIFTY", 1},
        {"TFSHIFT pct", 1},
        {"TFSHIFTR", 1},
        {"TFSHIFTWIN", 1},
        {"VMINHIT", 0},
        {"GRADHERO", 1},
        {"MRG2REM", 1},
        {"MRG50PCT", 1},
        /* Phase 10 animation/transition lanes: the engaged mount rides
         * its registry name as a BLOCK FIELD (the an battery below
         * asserts exact); the refusal windows (unregistered name /
         * paused / infinite) keep NULL fields; the transition victim
         * asserts via the direct winner query below (no text can
         * express an arm). */
        {"ANIMLIFT", 1},
        {"ANFALLDOWN", 1},
        {"ANIMPAUSE", 1},
        {"ANIMINF", 1},
        {"TRNVICTIM", 1},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    {
        int found = 0;
        for (int bi = 0; bi < doc->blockCount && !found; bi++)
        {
            DocBlock *blk = doc->blocks[bi];
            if (!blk)
            {
                continue;
            }
            /* Block text lives in the inline runs (blk->text is for code
             * blocks only) — mirror log_render_result's composition,
             * with a bigger cap so markers farther in still match. */
            char text[192];
            size_t off = 0;
            text[0] = '\0';
            if (blk->type == DOC_BLOCK_CODE_BLOCK && blk->text)
            {
                snprintf(text, sizeof(text), "%.190s", blk->text);
            }
            else if (blk->type == DOC_BLOCK_IMAGE && blk->src)
            {
                snprintf(text, sizeof(text), "<img src=%.40s>", blk->src);
            }
            else
            {
                for (int ii = 0;
                     ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
                {
                    const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                                        ? blk->inlines[ii]->text
                                        : "";
                    size_t tl = strlen(t);
                    if (tl > sizeof(text) - 1 - off)
                    {
                        tl = sizeof(text) - 1 - off;
                    }
                    memcpy(text + off, t, tl);
                    off += tl;
                }
                text[off] = '\0';
            }
            if (strstr(text, k[i].mark))
            {
                found = 1;
            }
        }
        int ok = (found == (int)k[i].wantPresent);
        if (!ok)
        {
            fails++;
        }
        logger_log("[cssprobe] %s %s: %s",
                   k[i].mark, k[i].wantPresent ? "(present?)" : "(absent?)",
                   ok ? "OK" : "FAIL");
    }
    /* Phase 5: layout-side cut proofs. The ellipsis/line-clamp cuts are
     * FILTERS the breaker applies at paint time — by design the doc
     * objects keep the FULL text (a doc-level scan can never see the
     * cut), so these rows run the canary block's OWN inline runs through
     * the REAL breaker via the battery probe hook: the ell block must
     * stay ONE line whose width fits the bond (the uncut nowrap line
     * would overflow ~2x), and the clamp block must cap at exactly 3
     * lines. 380 = the probe paragraph's production wrap width. */
    {
        int proofFails = 0;
        for (int bi = 0; bi < doc->blockCount; bi++)
        {
            DocBlock *blk = doc->blocks[bi];
            if (!blk ||
                (blk->type != DOC_BLOCK_PARAGRAPH &&
                 blk->type != DOC_BLOCK_HEADING))
            {
                continue;
            }
            char text[192];
            size_t off = 0;
            text[0] = '\0';
            for (int ii = 0;
                 ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
            {
                const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                                    ? blk->inlines[ii]->text
                                    : "";
                size_t tl = strlen(t);
                if (tl > sizeof(text) - 1 - off)
                {
                    tl = sizeof(text) - 1 - off;
                }
                memcpy(text + off, t, tl);
                off += tl;
            }
            text[off] = '\0';
            if (off < 8)
            {
                continue;
            }
            LayoutBreakProbe pr;
            if (strstr(text, "ELLKEEP visible then") == text)
            {
                layout_test_probe_maxlines(
                    (const DocInline **)blk->inlines, blk->inlineCount, 380,
                    NULL, 0, 16, 0, &pr);
                int ok = (pr.lineCount == 1 && pr.lineWidth[0] > 0 &&
                          pr.lineWidth[0] <= 380);
                logger_log("[cssprobe] ell-clip lines=%d width=%d: %s",
                           pr.lineCount, pr.lineWidth[0], ok ? "OK" : "FAIL");
                if (!ok)
                {
                    proofFails++;
                }
            }
            else if (strstr(text, "CLAMPKEEP then filler") == text)
            {
                layout_test_probe_maxlines(
                    (const DocInline **)blk->inlines, blk->inlineCount, 380,
                    NULL, 0, 16, 3, &pr);
                int ok = (pr.lineCount == 3);
                logger_log("[cssprobe] clamp-lines lines=%d want 3: %s",
                           pr.lineCount, ok ? "OK" : "FAIL");
                if (!ok)
                {
                    proofFails++;
                }
            }
        }
        fails += proofFails;
    }
    /* Phase 6: flex/grid container-transport receipts read DIRECTLY off
     * the doc objects (walker-side data — unlike the P5 paint cuts these
     * ARE visible to a doc scan): the first gap-container child carries
     * the halved-px mount, the LAST child's slot is closed by the
     * container's CSS exit, rem scales at the 16px browser root,
     * row-gap beats the gap shorthand, gap is inert on a non-flex box
     * box (CSS-correct), and align-items:center centers child blocks
     * only inside real flex containers (align-self on its item own). */
    {
        static const struct
        {
            const char *mark;
            int gapWant;    /* DocBlock.gapApplied must equal (skip < 0) */
            int centerWant; /* 1 align=="center", 0 align unset (skip -1) */
        } fk[] = {
            {"FIXGAPA child one", 8, -1},       /* .fxg: 16px -> 8 */
            {"FIXGAPB child two", 0, -1},       /* the tail slot closes */
            {"FGAPC rem child", 4, -1},         /* .fxr: .5rem -> 4 */
            {"FGAPD rem child", 0, -1},
            {"FXROWA longhand", 6, -1},         /* .fxw: 12px -> 6 */
            {"FXROWB longhand", 0, -1},
            {"FAICC centered", -1, 1},
            {"FAICD centered", -1, 1},
            {"FXSB self-centered", -1, 1},      /* align-self: center */
            {"FXNA plain one", -1, 0},          /* gapless flex, no carry */
            {"AINC noflex", -1, 0},             /* center inert w/o flex */
            {"FGNPX gap-inert", 0, -1},
        };
        int flexFails = 0;
        for (size_t i = 0; i < sizeof(fk) / sizeof(fk[0]); i++)
        {
            int found = 0, ok = 0;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk)
                {
                    continue;
                }
                char text[192];
                size_t off = 0;
                text[0] = '\0';
                for (int ii = 0;
                     ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
                {
                    const char *t = (blk->inlines[ii] &&
                                     blk->inlines[ii]->text)
                                        ? blk->inlines[ii]->text
                                        : "";
                    size_t tl = strlen(t);
                    if (tl > sizeof(text) - 1 - off)
                    {
                        tl = sizeof(text) - 1 - off;
                    }
                    memcpy(text + off, t, tl);
                    off += tl;
                }
                text[off] = '\0';
                if (strstr(text, fk[i].mark) == text)
                {
                    found = 1;
                    ok = 1;
                    if (fk[i].gapWant >= 0 &&
                        blk->gapApplied != fk[i].gapWant)
                    {
                        ok = 0;
                    }
                    if (fk[i].centerWant >= 0)
                    {
                        int isCt =
                            blk->align && strcmp(blk->align, "center") == 0;
                        if (isCt != fk[i].centerWant)
                        {
                            ok = 0;
                        }
                    }
                    if (ok)
                    {
                        logger_log("[cssprobe] flex %s: OK (gap=%d align=%s)",
                                   fk[i].mark, blk->gapApplied,
                                   blk->align ? blk->align : "unset");
                    }
                }
            }
            if (!found || !ok)
            {
                flexFails++;
                logger_log("[cssprobe] flex %s: %s", fk[i].mark,
                           found ? "FAIL" : "MISSING");
            }
        }
        fails += flexFails;
    }
    /* Phase 7: ::marker content receipts read DIRECTLY off the li
     * blocks (same doc-field pattern as the Phase 6 mounts): the
     * winning ::marker content text rides DocBlock.markerText (the
     * drawn bullet on a custom symbol), an empty string = the author
     * removed the bullet, and a block WITHOUT ::marker rules keeps the
     * default glyph (markerText NULL). Layout draws the text (or
     * nothing) from this field — the doc is the truth here. */
    {
        static const struct
        {
            const char *liText; /* block TEXT START (the li's own) */
            const char *want;   /* markerText strcmp ("" = empty string) */
            int wantNull;       /* 1 = markerText must be NULL */
        } mlk[] = {
            {"MARKORD bullet text", "MK7", 0},
            {"REMPT bullet-less", "", 0},
        };
        int mFails = 0;
        for (size_t i = 0; i < sizeof(mlk) / sizeof(mlk[0]); i++)
        {
            int found = 0, ok = 0;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk || blk->type != DOC_BLOCK_LIST_ITEM)
                {
                    continue;
                }
                char text[192];
                size_t off = 0;
                text[0] = '\0';
                for (int ii = 0;
                     ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
                {
                    const char *t =
                        (blk->inlines[ii] && blk->inlines[ii]->text)
                            ? blk->inlines[ii]->text
                            : "";
                    size_t tl = strlen(t);
                    if (tl > sizeof(text) - 1 - off)
                    {
                        tl = sizeof(text) - 1 - off;
                    }
                    memcpy(text + off, t, tl);
                    off += tl;
                }
                text[off] = '\0';
                if (strstr(text, mlk[i].liText) == text)
                {
                    found = 1;
                    ok = (((blk->markerText == NULL) == (mlk[i].wantNull != 0)) &&
                          (mlk[i].wantNull ||
                           strcmp(blk->markerText, mlk[i].want) == 0));
                    if (ok)
                    {
                        logger_log("[cssprobe] marker %s: OK (\"%s\")",
                                   mlk[i].liText,
                                   blk->markerText ? blk->markerText : "");
                    }
                }
            }
            if (!found || !ok)
            {
                mFails++;
                logger_log("[cssprobe] marker %s: %s", mlk[i].liText,
                           found ? "FAIL" : "MISSING");
            }
        }
        fails += mFails;
    }
    /* Phase 8: interactive-set field asserts — the srcset picks, the
     * multiple-select model, and the reset/default button blocks. The
     * k[] text battery cannot see these: a picked URL is not block
     * TEXT, and button labels never render as block inlines. Same
     * bespoke-loop pattern as the marker battery above. */
    {
        static const struct
        {
            const char *wantAlt;
            const char *wantSrc;
        } rk[] = {
            {"PLAINIMG baseline", "https://probe.example/plain.png"},
            {"SRCSETW width pick", "https://probe.example/three.png"},
            {"SRCSETD density pick", "https://probe.example/van@1x.png"},
            {"SRCSETF unusable keeps src", "https://probe.example/only.png"},
        };
        int rFails = 0;
        for (size_t i = 0; i < sizeof(rk) / sizeof(rk[0]); i++)
        {
            int found = 0;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk || blk->type != DOC_BLOCK_IMAGE || !blk->alt ||
                    !blk->src)
                {
                    continue;
                }
                if (strcmp(blk->alt, rk[i].wantAlt) == 0)
                {
                    found = 1;
                    if (strcmp(blk->src, rk[i].wantSrc) != 0)
                    {
                        logger_log("[cssprobe] srcset %s: FAIL (src=%.90s)",
                                   rk[i].wantAlt, blk->src);
                        rFails++;
                    }
                    else
                    {
                        logger_log("[cssprobe] srcset %s: OK", rk[i].wantAlt);
                    }
                }
            }
            if (!found)
            {
                logger_log("[cssprobe] srcset %s: MISSING (no img block)",
                           rk[i].wantAlt);
                rFails++;
            }
        }
        fails += rFails;
        int selFails = 0;
        int selFound = 0;
        for (int bi = 0; bi < doc->blockCount && !selFound; bi++)
        {
            DocBlock *blk = doc->blocks[bi];
            if (!blk || blk->type != DOC_BLOCK_SELECT_FIELD || !blk->name ||
                strcmp(blk->name, "msel") != 0)
            {
                continue;
            }
            selFound = 1;
            int ok = (blk->optionCount == 3 && blk->multiple == 1 &&
                      blk->selectedIndex >= 1 && blk->selectedIndex <= 3);
            if (ok)
            {
                static const int wantSel[3] = {1, 0, 1};
                static const int wantDis[3] = {0, 0, 0};
                for (int k2 = 0; k2 < 3; k2++)
                {
                    DocOption *o = blk->options ? blk->options[k2] : NULL;
                    if (!o || o->selected != wantSel[k2] ||
                        o->disabled != wantDis[k2] || !o->text || !o->value)
                    {
                        ok = 0;
                        break;
                    }
                }
            }
            logger_log("[cssprobe] msel model: %s (%d opts, multiple=%d)",
                       ok ? "OK" : "FAIL", blk->optionCount,
                       blk->multiple ? 1 : 0);
            if (!ok)
            {
                selFails++;
            }
        }
        if (!selFound)
        {
            logger_log("[cssprobe] msel model: MISSING (no select block)");
            selFails++;
        }
        fails += selFails;
        static const struct
        {
            const char *wantLabel;
            const char *wantType;
        } bk[] = {
            {"RNRESET", "reset"},
            {"FMBTN", "submit"},
        };
        int bFails = 0;
        for (size_t i = 0; i < sizeof(bk) / sizeof(bk[0]); i++)
        {
            int found = 0;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk || blk->type != DOC_BLOCK_INPUT_SUBMIT ||
                    !blk->label)
                {
                    continue;
                }
                if (strcmp(blk->label, bk[i].wantLabel) == 0)
                {
                    found = 1;
                    int ok = (blk->buttonType &&
                              strcmp(blk->buttonType, bk[i].wantType) == 0);
                    logger_log("[cssprobe] button %s: %s", bk[i].wantLabel,
                               ok ? "OK" : "FAIL");
                    if (!ok)
                    {
                        bFails++;
                    }
                }
            }
            if (!found)
            {
                logger_log("[cssprobe] button %s: MISSING",
                           bk[i].wantLabel);
                bFails++;
            }
        }
        fails += bFails;
        /* The summary-less details default: a summary-less <details> prints
         * the user-agent label — stored as the box-open block's LABEL
         * (box_open_block takes the string as label, not an inline, so it
         * cannot ride the k[] text battery). */
        {
            int detOk = 0;
            for (int bi = 0; bi < doc->blockCount; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (blk && blk->type == DOC_BLOCK_BOX_OPEN && blk->label &&
                    strcmp(blk->label, "> Details") == 0)
                {
                    detOk = 1;
                    break;
                }
            }
            logger_log("[cssprobe] details default: %s",
                       detOk ? "OK" : "FAIL");
            if (!detOk)
            {
                fails++;
            }
        }
    }
    /* Phase 9: translate receipts read DIRECTLY off the doc objects
     * (same doc-field pattern as the Phase 6 mounts). Halved px mirrors
     * walk_box_px (full/2); percentages carry their own field
     * (per-mille/20 = percent-of-own-box, keyed against the element's
     * box AT DRAW); the inline style replaces the WHOLE transform
     * value (CSS: one property, one winning value — the sheet's
     * translateY never contributes under an inline translateX);
     * rotate/scale stay the 2-D policy line (census vocabulary, no
     * floor). */
    {
        static const struct
        {
            const char *mark; /* block TEXT START */
            int xpx, ypx, xpct, ypct;
        } tf[] = {
            {"TFSHIFTX", 6, 0, 0, 0},     /* 12px -> 6 half-px */
            {"TFSHIFTY", 0, -6, 0, 0},    /* -0.75rem -> -12 -> -6 */
            {"TFSHIFT pct", 0, 0, 5, -25},/* 10% / -50% */
            {"TFSHIFTR", 2, 0, 0, 0},     /* rotate skipped, 4px -> 2 */
            {"TFSHIFTWIN", 3, 0, 0, 0},   /* inline beats the sheet */
        };
        int tfFails = 0;
        for (size_t i = 0; i < sizeof(tf) / sizeof(tf[0]); i++)
        {
            int found = 0, ok = 0;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk)
                {
                    continue;
                }
                char text[192];
                size_t off = 0;
                text[0] = '\0';
                for (int ii = 0;
                     ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
                {
                    const char *t = (blk->inlines[ii] &&
                                     blk->inlines[ii]->text)
                                        ? blk->inlines[ii]->text
                                        : "";
                    size_t tl = strlen(t);
                    if (tl > sizeof(text) - 1 - off)
                    {
                        tl = sizeof(text) - 1 - off;
                    }
                    memcpy(text + off, t, tl);
                    off += tl;
                }
                text[off] = '\0';
                if (strstr(text, tf[i].mark) == text)
                {
                    found = 1;
                    ok = blk->tfXpx == tf[i].xpx &&
                         blk->tfYpx == tf[i].ypx &&
                         blk->tfXpct == tf[i].xpct &&
                         blk->tfYpct == tf[i].ypct;
                    if (ok)
                    {
                        logger_log("[cssprobe] tf %s: OK (xpx=%d ypx=%d "
                                   "xpct=%d ypct=%d)",
                                   tf[i].mark, blk->tfXpx, blk->tfYpx,
                                   blk->tfXpct, blk->tfYpct);
                    }
                    else
                    {
                        logger_log("[cssprobe] tf %s: FAIL (xpx=%d ypx=%d "
                                   "xpct=%d ypct=%d)",
                                   tf[i].mark, blk->tfXpx, blk->tfYpx,
                                   blk->tfXpct, blk->tfYpct);
                    }
                }
            }
            if (!found)
            {
                logger_log("[cssprobe] tf %s: MISSING", tf[i].mark);
                tfFails++;
            }
            else if (!ok)
            {
                tfFails++;
            }
        }
        fails += tfFails;
        /* Gradient polarity: the dark sheet gradient answers through
         * the shorthand's first-stop color token (find_color_token
         * already lifts #050505 out of linear-gradient(...) as the
         * fanned-out background-color) — the same < 130 luminance bar
         * the inline route set. */
        {
            int gradOk = 0;
            for (int bi = 0; bi < doc->blockCount; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (blk)
                {
                    char text[64];
                    size_t off = 0;
                    text[0] = '\0';
                    for (int ii = 0;
                         ii < blk->inlineCount && off < sizeof(text) - 1;
                         ii++)
                    {
                        const char *t =
                            (blk->inlines[ii] && blk->inlines[ii]->text)
                                ? blk->inlines[ii]->text
                                : "";
                        size_t tl = strlen(t);
                        if (tl > sizeof(text) - 1 - off)
                        {
                            tl = sizeof(text) - 1 - off;
                        }
                        memcpy(text + off, t, tl);
                        off += tl;
                    }
                    text[off] = '\0';
                    if (strstr(text, "GRADHERO") == text)
                    {
                        gradOk = (blk->invert == 1);
                        break;
                    }
                }
            }
            logger_log("[cssprobe] gradhero invert: %s",
                       gradOk ? "OK" : "FAIL");
            if (!gradOk)
            {
                fails++;
            }
        }
        /* Margin unit repair: 2rem top = 16 (rem scales root-16px, then
         * the halved-px lane), and a 50% LEFT margin refuses entirely —
         * the legacy walk mined 25px out of %-of-parent; % has no px
         * answer before the box exists, so honest absence (indent 0). */
        {
            static const struct
            {
                const char *mark;
                int topWant;   /* spacingTop must equal (skip < 0) */
                int leftWant;  /* indent must equal (skip < 0) */
            } mg[] = {
                {"MRG2REM", 16, -1},  /* style margin-top:2rem */
                {"MRG50PCT", -1, 0},  /* style margin-left:50% refuses */
            };
            int mgFails = 0;
            for (size_t i = 0; i < sizeof(mg) / sizeof(mg[0]); i++)
            {
                int found = 0, ok = 0;
                for (int bi = 0; bi < doc->blockCount && !found; bi++)
                {
                    DocBlock *blk = doc->blocks[bi];
                    if (!blk)
                    {
                        continue;
                    }
                    char text[64];
                    size_t off = 0;
                    text[0] = '\0';
                    for (int ii = 0;
                         ii < blk->inlineCount && off < sizeof(text) - 1;
                         ii++)
                    {
                        const char *t =
                            (blk->inlines[ii] && blk->inlines[ii]->text)
                                ? blk->inlines[ii]->text
                                : "";
                        size_t tl = strlen(t);
                        if (tl > sizeof(text) - 1 - off)
                        {
                            tl = sizeof(text) - 1 - off;
                        }
                        memcpy(text + off, t, tl);
                        off += tl;
                    }
                    text[off] = '\0';
                    if (strstr(text, mg[i].mark) == text)
                    {
                        found = 1;
                        ok = 1;
                        if (mg[i].topWant >= 0 &&
                            blk->spacingTop != mg[i].topWant)
                        {
                            ok = 0;
                        }
                        if (mg[i].leftWant >= 0 &&
                            blk->indent != mg[i].leftWant)
                        {
                            ok = 0;
                        }
                        if (ok)
                        {
                            logger_log("[cssprobe] margin %s: OK "
                                       "(top=%d indent=%d)",
                                       mg[i].mark, blk->spacingTop,
                                       blk->indent);
                        }
                        else
                        {
                            logger_log("[cssprobe] margin %s: FAIL "
                                       "(top=%d indent=%d)",
                                       mg[i].mark, blk->spacingTop,
                                       blk->indent);
                        }
                    }
                }
                if (!found)
                {
                    logger_log("[cssprobe] margin %s: MISSING", mg[i].mark);
                    mgFails++;
                }
                else if (!ok)
                {
                    mgFails++;
                }
            }
            fails += mgFails;
        }
    }
    /* Phase 2: direct-resolution rows — the engine answers RESOLVED
     * strings for the fixture's var/calc rules (a stronger assertion
     * than the hide canaries: exact byte compares). Class-only
     * selectors need no DOM chain, so the hooks stay zero and the node
     * is an inert marker (the compound match never walks it). */
    CssEngine *peng = (CssEngine *)doc->_css;
    if (peng)
    {
        CssDom qdom = {0};
        /* One marker PER query: the winning mask caches per (node,
         * lane) — reusing an address would serve this query the last
         * element's matching-bits. */
        int markerW = 0;
        int markerD = 0;
        size_t ql = 0;
        const char *wv = css_winning_value(peng, "div", "calcvictim", NULL,
                                           &qdom, (const void *)&markerW,
                                           "width", &ql);
        int wok = (wv && ql == 4 && memcmp(wv, "64px", 4) == 0);
        logger_log("[cssprobe] calc-width=%.*s: %s", (int)ql, wv ? wv : "",
                   wok ? "OK" : "FAIL");
        if (!wok)
        {
            fails++;
        }
        const char *dv = css_winning_value(peng, "div", "xfvar", NULL,
                                           &qdom, (const void *)&markerD,
                                           "display", &ql);
        int dok = (dv && ql == 4 && memcmp(dv, "none", 4) == 0);
        logger_log("[cssprobe] var-display=%.*s: %s", (int)ql, dv ? dv : "",
                   dok ? "OK" : "FAIL");
        if (!dok)
        {
            fails++;
        }
        /* Phase 10: the @keyframes registry (collected at sheet parse)
         * + the transition arm receipt. The registry must know `pf`
         * (the fixture's single @keyframes) and NOTHING about `nokey`
         * (the refusal canary whose run never completes — the walk side
         * must never engage it). The transition row reads the EXACT
         * winner span for the fixture's shorthand. */
        int kfOk = css_keyframes_known(peng, "pf", 2) &&
                   !css_keyframes_known(peng, "nokey", 5);
        logger_log("[cssprobe] keyframes pf known + nokey absent: %s",
                   kfOk ? "OK" : "FAIL");
        if (!kfOk)
        {
            fails++;
        }
        int markerT = 0;
        size_t tl = 0;
        const char *tv = css_winning_value(peng, "div", "trnvictim", NULL,
                                           &qdom, (const void *)&markerT,
                                           "transition", &tl);
        int tog = (tv && tl == 9 && memcmp(tv, "color .3s", 9) == 0);
        logger_log("[cssprobe] transition=%.*s: %s", (int)tl,
                   tv ? tv : "", tog ? "OK" : "FAIL");
        if (!tog)
        {
            fails++;
        }
    }
    /* Phase 10: the animation mount battery — find each canary's OWN
     * block by its text, then assert the walk-set animName field: the
     * engaged mount = the registry name (`pf`), the never-completing
     * windows stay NULL. Per-walk field (fresh each walk); the
     * once-per-(node,name) completion contract across re-walks lives
     * on the DOM and the jsext autotest proves the real one-time fire
     * in the running browser. */
    {
        static const struct
        {
            const char *mark;
            const char *want; /* NULL: the field must stay absent */
        } an[] = {
            {"ANIMLIFT", "pf"},
            {"ANFALLDOWN", NULL},
            {"ANIMPAUSE", NULL},
            {"ANIMINF", NULL},
        };
        int anFails = 0;
        for (size_t i = 0; i < sizeof(an) / sizeof(an[0]); i++)
        {
            int found = 0;
            const char *got = NULL;
            for (int bi = 0; bi < doc->blockCount && !found; bi++)
            {
                DocBlock *blk = doc->blocks[bi];
                if (!blk)
                {
                    continue;
                }
                char text[192];
                cssprobe_block_text(blk, text, sizeof(text));
                if (strstr(text, an[i].mark))
                {
                    found = 1;
                    got = blk->animName;
                }
            }
            int ok = 0;
            if (!found)
            {
                logger_log("[cssprobe] anim %s: MISSING", an[i].mark);
            }
            else
            {
                ok = an[i].want ? (got && strcmp(got, an[i].want) == 0)
                                : (got == NULL);
            }
            if (!ok)
            {
                anFails++;
            }
            logger_log("[cssprobe] anim %s: %s%s", an[i].mark,
                       ok ? "OK" : "FAIL",
                       an[i].want ? "" : " (field must stay absent)");
        }
        fails += anFails;
    }
    logger_log("[cssprobe] %s (%d fails)", fails ? "FAIL" : "PASS", fails);
}
#endif /* PLUTO_NR_AUTOTEST && !TARGET_PLAYDATE */

/* R30l: user request — log the END RESULT of every DOM render: block/link
 * counts AND the actual text that made it into the doc. Device run 14 kept
 * ending "rewalk done blocks=1 links=0" with zero visibility into WHAT that
 * one block was (the walker's "(Empty Web Page)" placeholder). This makes
 * every render outcome diagnosable from pluto.log alone. */
static void log_render_result(const char *tag, DocParseResult *doc)
{
    if (!doc)
    {
        logger_log("[render-result] %s: (no doc)", tag);
        return;
    }
#if defined(PLUTO_NR_AUTOTEST) && !defined(TARGET_PLAYDATE)
    cssprobe_check(doc);
#endif
    logger_log("[render-result] %s: blocks=%d links=%d title='%.48s'",
               tag, doc->blockCount, doc->linkCount,
               doc->title[0] ? doc->title : "");
    if (doc->blockCount == 0)
    {
        return;
    }
    int shown = doc->blockCount < 64 ? doc->blockCount : 64; /* API-P3: the
     * address battery paints deep (blocks 24+); API-P4 adds the console
     * battery (9 more rows); API-P6 adds the DOM-conveniences battery
     * (6 rows + header + fixture) — the probe dump wants the whole
     * canvas */
    for (int bi = 0; bi < shown; bi++)
    {
        DocBlock *blk = doc->blocks[bi];
        if (!blk)
        {
            continue;
        }
        char text[96];
        size_t off = 0;
        text[0] = '\0';
        if (blk->type == DOC_BLOCK_CODE_BLOCK && blk->text)
        {
            snprintf(text, sizeof(text), "%.60s", blk->text);
        }
        else if (blk->type == DOC_BLOCK_IMAGE && blk->src)
        {
            snprintf(text, sizeof(text), "<img src=%.40s>", blk->src);
        }
        else
        {
            for (int ii = 0; ii < blk->inlineCount && off < sizeof(text) - 1; ii++)
            {
                const char *t = (blk->inlines[ii] && blk->inlines[ii]->text)
                                    ? blk->inlines[ii]->text
                                    : "";
                size_t tl = strlen(t);
                if (tl > sizeof(text) - 1 - off)
                {
                    tl = sizeof(text) - 1 - off;
                }
                memcpy(text + off, t, tl);
                off += tl;
            }
            text[off] = '\0';
        }
        logger_log("[render-result]   block[%d] type=%d text='%.60s'",
                   bi, blk->type, text);
    }
}

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
        log_render_result("rewalk", rt->rewalkDoc);
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

        /* ── Stage C (Phase 1.5): external stylesheet fetch ──
         * The engine needs every sheet BEFORE the parse joins them (inline
         * <style> keeps the tight slots; <link rel="stylesheet"> fills the
         * remainder). CSS runs regardless of the JS setting — a JS-Off
         * page still gets its stylesheet. Sequential single-flight fetches
         * through the shared client; the frame loop pumps http_update()
         * while we yield the task. Caps: ≤ PLUTO_CSS_EXT_MAX sheets,
         * ≤ PLUTO_CSS_EXT_TOTAL_MAX bytes RAM total; media/type mismatches
         * and fetch failures skip that sheet only (absence, never crash). */
        if (rt->body && !rt->parsed && !rt->isToggle &&
            strncmp(rt->url, "about:", 6) != 0)
        {
            if (rt->cssPhase == 0)
            {
                /* C1: scan + resolve (no network in this step). */
                rt->cssPhase = 1;
                char hrefs[PLUTO_CSS_EXT_MAX][512];
                unsigned char mediaOk[PLUTO_CSS_EXT_MAX];
                memset(hrefs, 0, sizeof(hrefs));
                memset(mediaOk, 0, sizeof(mediaOk));
                int hn = css_ext_scan_links(rt->body, hrefs, mediaOk);
                int stored = 0;
                for (int k = 0; k < hn; k++)
                {
                    if (!mediaOk[k])
                    {
                        logger_log("[css-ext] skip %s (media)", hrefs[k]);
                        continue;
                    }
                    if (stored >= PLUTO_CSS_EXT_MAX)
                    {
                        logger_log("[css-ext] link cap reached (%d)",
                                   PLUTO_CSS_EXT_MAX);
                        break;
                    }
                    char *abs = url_resolve(rt->url, hrefs[k]);
                    if (!abs)
                    {
                        logger_log("[css-ext] resolve-fail %s", hrefs[k]);
                        continue;
                    }
                    if (strncmp(abs, "http://", 7) != 0 &&
                        strncmp(abs, "https://", 8) != 0)
                    {
                        pluto_free(abs);
                        continue;
                    }
                    rt->cssUrls[stored] = abs;
                    stored++;
                }
                rt->cssUrlN = stored;
                rt->cssIdx = 0;
                rt->cssPhase = (stored > 0) ? 2 : 3;
                if (stored > 0)
                {
                    logger_log("[css-ext] %d stylesheet link(s) to fetch",
                               stored);
                }
            }
            if (rt->cssPhase == 2)
            {
                if (g_cssInFlight && g_cssDone)
                {
                    /* Consume the finished fetch (success, error, or
                     * refused start). g_cssInFlight marks an owned slot —
                     * a stray completion arriving after a mid-fetch
                     * teardown finds it 0 and is dropped unclaimed
                     * (never misattributed to the next render task). */
                    g_cssInFlight = 0;
                    if (!g_cssErr && g_cssStatus >= 200 && g_cssStatus < 400 &&
                        g_cssBody && g_cssLen > 0)
                    {
                        logger_log("[css-ext] ok %s (%zu bytes)",
                                   rt->cssUrls[rt->cssIdx], g_cssLen);
                        rt->cssBody[rt->cssIdx] = g_cssBody;
                        rt->cssLen[rt->cssIdx] = g_cssLen;
                        g_cssBody = NULL;
                        g_cssLen = 0;
                    }
                    else
                    {
                        logger_log("[css-ext] miss %s (status=%d err=%d)",
                                   rt->cssUrls[rt->cssIdx], g_cssStatus,
                                   g_cssErr);
                        if (g_cssBody)
                        {
                            pluto_mem_realloc(g_cssBody, 0);
                            g_cssBody = NULL;
                        }
                        g_cssLen = 0;
                    }
                    rt->cssBytes += rt->cssLen[rt->cssIdx];
                    g_cssStatus = 0;
                    g_cssDone = 0;
                    g_cssErr = 0;
                    rt->cssIdx++;
                }
                if (rt->cssIdx < rt->cssUrlN &&
                    rt->cssBytes < PLUTO_CSS_EXT_TOTAL_MAX)
                {
                    if (http_is_loading())
                    {
                        return 1; /* in flight; the frame pump advances it */
                    }
                    HttpCallbacks cb;
                    memset(&cb, 0, sizeof(cb));
                    cb.onSuccess = css_ext_on_success;
                    cb.onSuccessSpill = css_ext_on_spill;
                    cb.onError = css_ext_on_error;
                    int started = http_get(rt->cssUrls[rt->cssIdx], &cb);
                    if (started)
                    {
                        g_cssInFlight = 1;
                    }
                    else
                    {
                        /* Immediate failure: onError ALREADY fired (client
                         * contract) with the labels pointing at THIS slot.
                         * Reset them here — the cursor advances past the
                         * slot and the next step starts the next URL with
                         * clean flags (no wrong-slot consumption). */
                        g_cssDone = 0;
                        g_cssErr = 0;
                        g_cssStatus = 0;
                        logger_log("[css-ext] start-fail %s",
                                   rt->cssUrls[rt->cssIdx]);
                        rt->cssIdx++; /* past it; next entry on next step */
                    }
                    return 1; /* every step yields; the pump runs per frame */
                }
                /* Queue drained: move captured bodies onto the doc result. */
                if (rt->cssIdx < rt->cssUrlN)
                {
                    logger_log("[css-ext] %d link(s) skipped: byte cap",
                               rt->cssUrlN - rt->cssIdx);
                }
                int have = 0;
                for (int k = 0; k < PLUTO_CSS_EXT_MAX; k++)
                {
                    if (rt->cssBody[k])
                    {
                        have++;
                    }
                }
                if (have > 0)
                {
                    rt->doc->cssExtBody =
                        (char **)pluto_mem_realloc(NULL, sizeof(char *) *
                                                          (size_t)have);
                    rt->doc->cssExtLen =
                        (size_t *)pluto_mem_realloc(NULL, sizeof(size_t) *
                                                           (size_t)have);
                    if (rt->doc->cssExtBody && rt->doc->cssExtLen)
                    {
                        int w = 0;
                        for (int k = 0; k < PLUTO_CSS_EXT_MAX; k++)
                        {
                            if (rt->cssBody[k])
                            {
                                rt->doc->cssExtBody[w] = rt->cssBody[k];
                                rt->doc->cssExtLen[w] = rt->cssLen[k];
                                w++;
                            }
                        }
                        rt->doc->cssExtN = w;
                        logger_log("[css-ext] sheets joined: %d (%zu bytes "
                                   "total)",
                                   w, rt->cssBytes);
                    }
                    else
                    {
                        /* Track-array alloc failed: drop the bodies (same
                         * absence containment as a fetch OOM). */
                        for (int k = 0; k < PLUTO_CSS_EXT_MAX; k++)
                        {
                            if (rt->cssBody[k])
                            {
                                pluto_mem_realloc(rt->cssBody[k], 0);
                                rt->cssBody[k] = NULL;
                            }
                        }
                    }
                }
                for (int k = 0; k < PLUTO_CSS_EXT_MAX; k++)
                {
                    if (rt->cssUrls[k])
                    {
                        pluto_free(rt->cssUrls[k]);
                        rt->cssUrls[k] = NULL;
                    }
                }
                rt->cssPhase = 3;
            }
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
            /* R30l: transfer over — hand the loading line back to the
             * parse percent (counter resets with the next page). */
            progressNetCur = 0;
            progressNetTotal = 0;
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

/* Phase 1.5: release any stylesheet stage state still owned by the task
 * (urls not yet transferred, bodies not yet moved onto the doc). */
static void render_css_free(RenderTask *rt)
{
    if (!rt)
    {
        return;
    }
    if (rt->cssPhase == 2 && g_cssInFlight)
    {
        /* Task torn down mid-fetch — same policy as the script prefetch
         * abort: cancel the request so the loading lane doesn't stall.
         * Safe for images too: the image queue defers while
         * http_is_loading() is true, so this in-flight request is ours. */
        http_cancel();
    }
    for (int k = 0; k < PLUTO_CSS_EXT_MAX; k++)
    {
        if (rt->cssUrls[k])
        {
            pluto_free(rt->cssUrls[k]);
            rt->cssUrls[k] = NULL;
        }
        if (rt->cssBody[k])
        {
            pluto_mem_realloc(rt->cssBody[k], 0);
            rt->cssBody[k] = NULL;
        }
    }
    /* Late cancel-callbacks find an unowned collector: drop leftovers. */
    if (g_cssBody)
    {
        pluto_mem_realloc(g_cssBody, 0);
        g_cssBody = NULL;
    }
    g_cssLen = 0;
    g_cssStatus = 0;
    g_cssDone = 0;
    g_cssErr = 0;
    g_cssInFlight = 0;
    rt->cssUrlN = 0;
    rt->cssIdx = 0;
    rt->cssBytes = 0;
    rt->cssPhase = 0;
}

/* ── Phase 10: end-event drain (animation + transition lanes) ──────────────
 * Runs in render_done — after a render (fresh parse or JS-mutation
 * re-walk) completed, when the visible state IS the final state a
 * browser would be mid-transitioning to:
 *
 *   animation lane: the walker enqueued nodes whose DECLARED animation
 *   (a name the @keyframes registry holds, finite count, not paused,
 *   element rendered) completed this walk. Each dispatches one
 *   animationend. Per-(node,name) dedupe lives on the DOM (document
 *   lifetime), so re-walks cannot re-engage a completed run; a NEW
 *   name on the same node is a new run and re-engages, browser-style.
 *
 *   transition lane: the bridge noted the elements the page MUTATED
 *   (attribute writes, classList, style-object writes, structural
 *   append/insert/remove; text-only writes excluded on purpose so a
 *   listener's own writes cannot loop). Each noted element whose
 *   winning style still declares a transition (non-`none` value) gets
 *   ONE transitionend — the change's end, reached immediately at the
 *   visible render of the flat renderer. */
static void p10_drain_end_events(void)
{
    if (!g_pageJs || !currentDoc)
    {
        return;
    }
    if (currentDoc->pendEndCount > 0)
    {
        int n = currentDoc->pendEndCount;
        currentDoc->pendEndCount = 0;
        for (int i = 0; i < n; i++)
        {
            void *node = currentDoc->pendEnd[i].node;
            if (node)
            {
                jsbridge_dispatch_element_event(g_pageJs, node,
                                                "animationend");
            }
        }
        logger_log("[p10] animationend lane: %d run(s) dispatched", n);
    }
    {
        void *notes[JSB_MUT_NOTES_MAX];
        int nn = jsbridge_take_mut_notes(g_pageJs, notes,
                                         (int)(sizeof(notes) /
                                               sizeof(notes[0])));
        if (nn > 0)
        {
            int fired = 0;
            for (int i = 0; i < nn; i++)
            {
                if (document_node_transition_hot(currentDoc, notes[i]))
                {
                    jsbridge_dispatch_element_event(g_pageJs, notes[i],
                                                    "transitionend");
                    fired++;
                }
            }
            logger_log("[p10] transitionend lane: %d mutation(s), "
                       "%d armed & dispatched",
                       nn, fired);
        }
    }
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
    /* R30l: what did this render actually produce? (pre-JS doc on the fresh
     * path — the post-eval truth logs at the rewalk site). */
    log_render_result(wasRewalk ? "done(rewalk)" : "done(fresh)", currentDoc);

    /* Phase 10: the end-event lanes drain here — the visible render IS
     * the completed state, so animationend/transitionend dispatch now. */
    p10_drain_end_events();

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

    render_css_free(rt);
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
        render_css_free(rt);
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
            log_render_result("render-error(discarded)", rt->doc);
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
    progressNetCur = 0; /* R30l: fresh page, fresh byte counter */
    progressNetTotal = 0;
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
     * is freed here. */
    layout_clear();

    /* R28: tear the OLD page down at navigation START — a real browser
     * unloads the previous document when navigation commits, not when the
     * next one finishes. The mount fix made deep-CSR pages genuinely
     * complete, so their engine heap stayed alive through the next load:
     * the revisit autotest then armed the 6.5MB device-envelope budget
     * over an 83MB live heap and every allocation was refused — the
     * snapshot image malloc missed the fast path (hits=0) and
     * http_client's URL-struct malloc left a zombie CONNECTING request
     * that sat silent for the whole 60s watchdog (no connect attempt,
     * then "[net] http error: Connection timed out"). On device the same
     * double-booking presses the old page's engine heap against the new
     * page's budget. Same-shaped block as page_swap_doc: free the doc,
     * close the bridge (timers, XHR, listeners, machine), re-base the
     * live counter. The fresh snapshot saved after the last rewalk stays
     * on disk — invalidate only fires from render_done while eval is
     * still pending. */
    if (currentDoc)
    {
        if (currentDoc->_jsbridge)
        {
            js_doc_close(currentDoc->_jsbridge);
        }
        document_free(currentDoc);
        free(currentDoc);
        currentDoc = NULL;
        g_pageJs = NULL;
        pluto_mem_resync_live(); /* page-boundary counter re-base */
        logger_log("[nav] old page torn down at navigation start");
    }

    logger_log("navigate_to: %s", urlString);

    /* Cached Website Off (default): the page being left must leave
     * nothing on disk — drop its snapshot now (currentUrlObj still
     * addresses the OLD page here) so the next visit fetches fresh.
     * Snapshots live key-targeted in the spill store, so in-flight
     * bodies for other sessions are untouched. On = no-op (snapshot
     * behavior resumes exactly as before). */
    if (cache_off() && currentUrlObj && currentUrlObj->normalized[0])
    {
        pluto_snap_invalidate(currentUrlObj->normalized,
                              (int)currentBrowseMode);
        logger_log("[snap] cache off: purged %s",
                   currentUrlObj->normalized);
    }

    char urlBuf[648];
    char *unwrapped = url_unwrap_redirect(urlString);
    char *trimmed = strutil_trim_dup(unwrapped ? unwrapped : urlString);
    pluto_free(unwrapped);
    snprintf(urlBuf, sizeof(urlBuf), "%s", trimmed ? trimmed : urlString);
    pluto_free(trimmed);

    /* Reset per-navigation state (Lua runNavigation). The old layout items
     * (with their owned values) are freed by layout_clear during the render
     * task, so no form-value cleanup is needed here. */
    memset(&g_formKey, 0, sizeof(g_formKey));
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
    else if (strncmp(urlBuf, "file://", 7) == 0)
    {
        /* Test seam: local Data-dir file kept as-is; the loader below
         * serves it through the identical fetch→render pipeline. */
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
        /* Load with the SAME key the save used: currentUrlObj->normalized.
         * The raw urlBuf differs whenever url_parse canonifies the input
         * (empty path -> "/", host lowercased, default port stripped) —
         * e.g. "http://bryanwandrych.com" vs the saved
         * "http://bryanwandrych.com/" computed two different FNV keys and
         * every revisit missed the cache. */
        /* Cached Website Off NEVER serves a snapshot: every navigation
         * fetches + renders fresh. On keeps the fast path exactly. */
        DocParseResult *snap = (snapBypass || cache_off()) ? NULL
            : pluto_snap_load(currentUrlObj->normalized,
                              (int)currentBrowseMode, now,
                              SNAP_TTL_SECONDS);
        snapBypass = 0;
        if (snap)
        {
            ++g_snapFastPathHits;
            logger_log("[snap] fast path: %s", currentUrlObj->normalized);
#if defined(PLUTO_NR_AUTOTEST) && !defined(TARGET_PLAYDATE)
            memwatch_log("snap-restore");
#endif
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
                pluto_mem_resync_live(); /* page-boundary counter re-base */
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
    progressNetCur = 0; /* R30l: fresh page, fresh byte counter */
    progressNetTotal = 0;
    snprintf(pageTitle, sizeof(pageTitle), "Loading...");
    if (!navigatingHistory)
    {
        push_history(urlBuf);
    }
    navigatingHistory = 0;
    update_system_menu();

    /* Test seam: file:// URLs are served locally — no network involved
     * (used to reproduce engine crashes offline when the device radio is
     * down, and to open local documents generally). */
    if (strncmp(urlBuf, "file://", 7) == 0)
    {
        if (pluto_load_local_file(urlBuf) == 0)
        {
            currentState = STATE_ERROR;
            snprintf(pageTitle, sizeof(pageTitle), "Local file error");
            logger_log("[file] load failed: %s", urlBuf);
        }
        return;
    }

    HttpCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.onProgress = http_on_progress;
    cbs.onSuccess = http_on_success;
    cbs.onError = http_on_error;
    http_get(urlBuf, &cbs);
}

/* ── HTTP callbacks (port of the HttpClient.get handler table) ──────────── */

/* file:// loader (test seam + local documents). Streams a file from the
 * Data dir through the SAME http_on_success pipeline: consumers see a
 * normal page fetch with Content-Type text/html. GENERAL PURPOSE: no
 * filename or content knowledge here — any local document loads the same
 * way a desktop browser serves local files. Returns the byte length, or
 * 0 when the file is missing/too large for RAM delivery. */
static size_t pluto_load_local_file(const char *fileUrl)
{
    /* file://<path> — strip the scheme; device Data files live at /Data
     * (sim: same relative path works under the game's Data dir). */
    const char *path = fileUrl + 7;
    while (*path == '/')
    {
        path++; /* file:///Data/x → /Data/x (device), Data/x (sim) */
    }
    SDFile *fp = pd->file->open(path, kFileReadData);
    if (!fp)
    {
        /* URL normalization appends the empty-path '/' to file: URLs
         * (file://x.html → file://x.html/), but when the referent is a
         * FILE the slashed open just ENOTDIRs. One unslashed retry keeps
         * stray-slash file URLs loadable (receipted both ways). */
        size_t plen = strlen(path);
        char trimmed[1024];
        if (plen > 1 && plen < sizeof(trimmed) && path[plen - 1] == '/')
        {
            memcpy(trimmed, path, plen + 1);
            while (plen > 0 && trimmed[plen - 1] == '/')
            {
                plen--;
            }
            trimmed[plen] = '\0';
            fp = pd->file->open(trimmed, kFileReadData);
            if (fp)
            {
                logger_log("[file] slashed path retried as '%s' (opened)",
                           trimmed);
                path = trimmed;
            }
        }
        if (!fp)
        {
            logger_log("[file] open failed: %s", path);
            return 0;
        }
    }
    pd->file->seek(fp, 0, SEEK_END);
    int flen = pd->file->tell(fp);
    pd->file->seek(fp, 0, SEEK_SET);
    if (flen <= 0 || flen > 2 * 1024 * 1024)
    {
        logger_log("[file] bad size %d: %s", flen, path);
        pd->file->close(fp);
        return 0;
    }
    char *buf = (char *)malloc((size_t)flen + 1);
    if (!buf)
    {
        pd->file->close(fp);
        return 0;
    }
    int got = pd->file->read(fp, buf, (unsigned int)flen);
    pd->file->close(fp);
    if (got != flen)
    {
        logger_log("[file] short read %d/%d: %s", got, flen, path);
        free(buf);
        return 0;
    }
    buf[flen] = '\0';
    logger_log("[file] served %s (%d bytes)", path, flen);
    char *keys[1] = { "content-type" };
    char *vals[1] = { "text/html" };
    http_on_success(200, keys, vals, 1, buf, (size_t)flen, fileUrl);
    free(buf);
    return (size_t)flen;
}

static void http_on_progress(int cur, int total)
{
    progressCurrent = cur;
    progressTotal = total;
}

/* See html/jsext.h: loading-UI byte counter for external script fetches.
 * jsext's HTTP session is separate from the page request above, so this
 * is a distinct sink (called from html/jsext.c fetch_on_progress). */
void pluto_ui_net_progress(int cur, int total)
{
    progressNetCur = cur;
    progressNetTotal = total;
    /* Log every 64KB so pluto.log alone proves real bytes moved on the
     * device (user trust: "show me more than 0 bytes"). Log the start and
     * every 64KB step even when total is unknown (chunked responses). */
    static int lastLoggedK = -1;
    int k = cur >> 6;
    if (k != lastLoggedK)
    {
        lastLoggedK = k;
        if (total > 0)
        {
            logger_log("[net] script fetch %d / %d bytes", cur, total);
        }
        else
        {
            logger_log("[net] script fetch %d bytes", cur);
        }
    }
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
    /* R46 (device run 36 + sim run evidence): a parked XS-NR mount owns
     * the machine and the DOM is a HALF-BUILT React tree. A mutation
     * reported between bursts (budget_take inside setHostData) used to
     * schedule a full rewalk EVERY FRAME: 98 rewalks rendered the
     * "(Empty Web Page)" placeholder on the sim, and on the device the
     * walker's allocations + renders ran concurrently with the parked
     * mount until burst 18 resumed stale bytecode and hard-crashed
     * (mmfar=0x287b at fxRunID_nr:872). The mount's own completion
     * consumes callBudget on the split-exhausted path, which fires the
     * ONE rewalk that sees the finished tree — skip the per-burst ones. */
    if (g_pageJs && jsbridge_mount_parked(g_pageJs))
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

/* ── Form keyboard target resolution ─────────────────────────────────────── */
/* Mirror of the walker's text-input branch (document.c): lowered type in
 * the text set (missing type = text), plus <textarea>. Hidden/submit/
 * checkbox/radio inputs become other item kinds and are never counted. */
static int form_is_text_type(const char *t)
{
    if (!t || !t[0])
    {
        return 1;
    }
    char b[32];
    size_t i;
    for (i = 0; i + 1 < sizeof(b) && t[i]; i++)
    {
        char c = t[i];
        b[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    b[i] = '\0';
    return (strcmp(b, "text") == 0 || strcmp(b, "search") == 0 ||
            strcmp(b, "email") == 0 || strcmp(b, "url") == 0 ||
            strcmp(b, "number") == 0 || strcmp(b, "password") == 0 ||
            strcmp(b, "tel") == 0 || strcmp(b, "date") == 0 ||
            strcmp(b, "time") == 0 || strcmp(b, "month") == 0 ||
            strcmp(b, "week") == 0 || strcmp(b, "datetime-local") == 0 ||
            strcmp(b, "color") == 0);
}

static const char *form_node_attr(const DomNode *n, const char *key)
{
    if (!n || !key)
    {
        return NULL;
    }
    for (int i = 0; i < n->attrCount; i++)
    {
        if (n->attrs[i].key && strcmp(n->attrs[i].key, key) == 0)
        {
            return n->attrs[i].value ? n->attrs[i].value : "";
        }
    }
    return NULL;
}

/* Snapshot the key for the item the keyboard is opening. Ordinal = index
 * among same-(form,name) text inputs in layout order (== DOM order). */
static void form_key_snapshot(const LayoutItem *item)
{
    memset(&g_formKey, 0, sizeof(g_formKey));
    if (!item)
    {
        return;
    }
    snprintf(g_formKey.form, sizeof(g_formKey.form), "%s",
             item->formAction ? item->formAction : "");
    snprintf(g_formKey.name, sizeof(g_formKey.name), "%s",
             item->name ? item->name : "q");
    int ord = 0;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (!it || it->type != LRI_INPUT_FIELD || !it->name)
        {
            continue;
        }
        const char *fa = it->formAction ? it->formAction : "";
        int isArea = it->inputType && strcmp(it->inputType, "textarea") == 0;
        if (strcmp(fa, g_formKey.form) != 0 ||
            strcmp(it->name, g_formKey.name) != 0 ||
            (!isArea && !form_is_text_type(it->inputType)))
        {
            continue;
        }
        if (it == item)
        {
            g_formKey.ordinal = ord;
            g_formKey.valid = 1;
            return;
        }
        ord++;
    }
}

/* Resolve the key to the CURRENT layout item (NULL if rebuilt away). */
static const LayoutItem *form_key_item(void)
{
    if (!g_formKey.valid)
    {
        return NULL;
    }
    int ord = 0;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (!it || it->type != LRI_INPUT_FIELD || !it->name)
        {
            continue;
        }
        const char *fa = it->formAction ? it->formAction : "";
        int isArea = it->inputType && strcmp(it->inputType, "textarea") == 0;
        if (strcmp(fa, g_formKey.form) != 0 ||
            strcmp(it->name, g_formKey.name) != 0 ||
            (!isArea && !form_is_text_type(it->inputType)))
        {
            continue;
        }
        if (ord == g_formKey.ordinal)
        {
            return it;
        }
        ord++;
    }
    return NULL;
}

/* Nearest enclosing <form> action, resolved to absolute ("" if formless).
 * Mirrors the walker's resolve_href(w, action or ""). */
static void form_node_form(char *out, size_t cap, const DomNode *n)
{
    out[0] = '\0';
    const char *base = (currentDoc && currentDoc->baseUrl) ? currentDoc->baseUrl : "";
    for (const DomNode *p = n ? n->parent : NULL; p; p = p->parent)
    {
        if (p->kind == DOM_ELEMENT && p->tag && strcmp(p->tag, "form") == 0)
        {
            const char *act = form_node_attr(p, "action");
            char *abs = url_resolve(base, act ? act : "");
            if (abs)
            {
                snprintf(out, cap, "%s", abs);
                pluto_free(abs);
            }
            return;
        }
    }
}

/* ══ R56f/R56g: NATIVE FORM FALLBACK (the script-dead submit) — GENERAL ══
 * When NO script ran for a submit (JSB_EVENT_NONE — on-device react-dom's
 * 209KB factory cannot compile: R56d device [mem] trace, ~+5.75MB codegen
 * vs a ~9.4MB pool wall) the SSR DOM still shows the REAL form with the
 * REAL typed values (dom_set_attr "value" — the keyboard/autotest writes
 * it; it survives rewalks). The browser performs the submission itself,
 * GENERICALLY: all named successful controls, HTML5-lite validation
 * (required + type=email), the HTML-standard routing (form action= →
 * GET-navigate or urlencoded POST), else a Data/form_sites.txt profile
 * (endpoint/body/card-string overrides — per-site data, not code), else
 * the byte-identical static-page GET. Cards ride the dom_create helpers
 * + dom_append_child + page_rewalk_now (the same plumbing the JS DOM
 * services use), so the autotest DOM scans see them. The POST rides the
 * http_request_ct path (caller-chosen Content-Type; the legacy callers'
 * application/json is unchanged). A live script (rc=SUPPRESSED/NAVIGATE)
 * NEVER reaches this path — the sim's hydration stays on react's own
 * handlers. */

/* The GENERIC browser strings (every site without a profile gets these;
 * a Data/form_sites.txt profile overrides per host — data, not code). */
static const char FB_TXT_SUCCESS[] = "Sent.";
static const char FB_TXT_FAIL[] = "Something went wrong.";
static const char FB_TXT_INVALID[] =
    "Please check the form fields and try again.";
static const char FB_TXT_SENDING[] = "Sending\u2026";

/* ── Site profiles: Data/form_sites.txt (deployed DATA, not code) ─────
 * One profile block per host; keys only:
 *   host=example.com
 *   endpoint=https://server.example.com/api
 *   body=json            (or "url")
 *   keys=dom-name:payload-key;dom-name2:payload-key2
 *   labels=dom-name:Shown Label;dom-name2:Label
 *   summary=name1,name2  (rows shown; empty = every submitted field)
 *   success=... failure=... invalid=... sending=...
 * A form with its own action= never consults profiles (standard HTML
 * semantics run first). The file lives in the browser's data dir, so a
 * new site is an edit, not a recompile. */
typedef struct
{
    char host[64];
    char endpoint[160];
    int bodyJson;
    char keys[128];
    char labels[128];
    char summary[96];
    char success[192];
    char failure[96];
    char invalid[96];
    char sending[24];
} FormSite;
enum
{
    FB_SITE_MAX = 4
};
static FormSite g_fbSites[FB_SITE_MAX];
static int g_fbSiteCount;
static int g_fbSitesLoaded;

static void fb_site_reset(FormSite *s)
{
    memset(s, 0, sizeof(*s));
    s->bodyJson = 1; /* modern script-driven endpoints post JSON */
    snprintf(s->success, sizeof(s->success), "%s", FB_TXT_SUCCESS);
    snprintf(s->failure, sizeof(s->failure), "%s", FB_TXT_FAIL);
    snprintf(s->invalid, sizeof(s->invalid), "%s", FB_TXT_INVALID);
    snprintf(s->sending, sizeof(s->sending), "%s", FB_TXT_SENDING);
}

static void fb_site_value(FormSite *s, const char *key, const char *val)
{
    if (!strcmp(key, "host"))
        snprintf(s->host, sizeof(s->host), "%s", val);
    else if (!strcmp(key, "endpoint"))
        snprintf(s->endpoint, sizeof(s->endpoint), "%s", val);
    else if (!strcmp(key, "body"))
        s->bodyJson = (strstr(val, "json") == NULL) ? 0 : 1;
    else if (!strcmp(key, "keys"))
        snprintf(s->keys, sizeof(s->keys), "%s", val);
    else if (!strcmp(key, "labels"))
        snprintf(s->labels, sizeof(s->labels), "%s", val);
    else if (!strcmp(key, "summary"))
        snprintf(s->summary, sizeof(s->summary), "%s", val);
    else if (!strcmp(key, "success"))
        snprintf(s->success, sizeof(s->success), "%s", val);
    else if (!strcmp(key, "failure"))
        snprintf(s->failure, sizeof(s->failure), "%s", val);
    else if (!strcmp(key, "invalid"))
        snprintf(s->invalid, sizeof(s->invalid), "%s", val);
    else if (!strcmp(key, "sending"))
        snprintf(s->sending, sizeof(s->sending), "%s", val);
    /* unknown keys ignored (forward compatibility) */
}

static void pluto_form_sites_load(void)
{
    SDFile *fp;
    int flen, got;
    char *buf;
    g_fbSitesLoaded = 1;
    fp = pd->file->open("form_sites.txt", kFileReadData);
    if (!fp)
    {
        logger_log("[fbform] no form_sites.txt — generic fallback only");
        return;
    }
    pd->file->seek(fp, 0, SEEK_END);
    flen = pd->file->tell(fp);
    pd->file->seek(fp, 0, SEEK_SET);
    if (flen <= 0 || flen > 4096)
    {
        logger_log("[fbform] form_sites.txt bad size %d", flen);
        pd->file->close(fp);
        return;
    }
    buf = (char *)malloc((size_t)flen + 1);
    if (!buf)
    {
        pd->file->close(fp);
        return;
    }
    got = pd->file->read(fp, buf, (unsigned int)flen);
    pd->file->close(fp);
    if (got != flen)
    {
        free(buf);
        return;
    }
    buf[flen] = '\0';
    {
        FormSite cur;
        int have = 0;
        char *line = buf;
        while (line && line[0])
        {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            {
                char *eq;
                char *p = line;
                while (*p == ' ' || *p == '\t' || *p == '\r')
                    p++;
                if (*p && *p != '#')
                {
                    eq = strchr(p, '=');
                    if (eq)
                    {
                        char key[16];
                        size_t klen = (size_t)(eq - p);
                        char *val = eq + 1, *vend;
                        while (klen > 0 &&
                               (p[klen - 1] == ' ' || p[klen - 1] == '\t'))
                            klen--;
                        if (klen < sizeof(key))
                        {
                            memcpy(key, p, klen);
                            key[klen] = '\0';
                            vend = val + strlen(val);
                            while (vend > val &&
                                   (vend[-1] == ' ' || vend[-1] == '\t' ||
                                    vend[-1] == '\r'))
                                *--vend = '\0';
                            if (!strcmp(key, "host"))
                            {
                                if (have && g_fbSiteCount < FB_SITE_MAX)
                                    g_fbSites[g_fbSiteCount++] = cur;
                                fb_site_reset(&cur);
                                have = 1;
                            }
                            if (have)
                                fb_site_value(&cur, key, val);
                        }
                    }
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        if (have && g_fbSiteCount < FB_SITE_MAX)
            g_fbSites[g_fbSiteCount++] = cur;
    }
    {
        int i;
        for (i = 0; i < g_fbSiteCount; i++)
            logger_log("[fbform] profile[%d] host=%s endpoint=%.60s json=%d",
                       i, g_fbSites[i].host, g_fbSites[i].endpoint,
                       g_fbSites[i].bodyJson);
    }
    logger_log("[fbform] %d site profile(s) loaded", g_fbSiteCount);
    free(buf);
}

static const FormSite *fb_site_lookup(void)
{
    int i;
    const char *host =
        (currentUrlObj && currentUrlObj->host[0]) ? currentUrlObj->host : "";
    if (!host[0])
        return NULL;
    for (i = 0; i < g_fbSiteCount; i++)
    {
        if (g_fbSites[i].host[0] && strstr(host, g_fbSites[i].host))
            return &g_fbSites[i];
    }
    return NULL;
}

/* "name:Payload;other:Key2" map lookup — bounded copy of the value after
 * the ':' matching token `name` (up to ';' or end). out = "" when absent. */
static void fb_map_get(const char *raw, const char *name, size_t nameLen,
                       char *out, size_t cap)
{
    size_t vLen;
    out[0] = '\0';
    while (raw && *raw)
    {
        const char *semi = strchr(raw, ';');
        const char *colon = strchr(raw, ':');
        size_t segLen = semi ? (size_t)(semi - raw) : strlen(raw);
        size_t colonOff = colon ? (size_t)(colon - raw) : (size_t)-1;
        if (colon && colonOff < segLen && colonOff == nameLen &&
            strncmp(raw, name, nameLen) == 0)
        {
            vLen = segLen - colonOff - 1;
            if (vLen + 1 > cap)
                vLen = cap - 1;
            memcpy(out, colon + 1, vLen);
            out[vLen] = '\0';
            return;
        }
        if (!semi)
            break;
        raw = semi + 1;
    }
}

static int fb_list_has(const char *csv, const char *name)
{
    size_t n = strlen(name);
    while (csv && *csv)
    {
        const char *comma = strchr(csv, ',');
        size_t segLen = comma ? (size_t)(comma - csv) : strlen(csv);
        const char *s = csv;
        while (segLen && (*s == ' ' || *s == '\t'))
        {
            s++;
            segLen--;
        }
        while (segLen &&
               (s[segLen - 1] == ' ' || s[segLen - 1] == '\t'))
            segLen--;
        if (segLen == n && strncmp(s, name, n) == 0)
            return 1;
        if (!comma)
            break;
        csv = comma + 1;
    }
    return 0;
}

/* case-insensitive comparison without strcasecmp (newlib-safe). */
static int fb_ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Forms-Phase 6: the standing receipt family names the resolved method
 * (the IDL keyword upper-cased); the dialog keyword rides its own road
 * this phase, so the receipt keeps three honest names. */
static const char *fb_method_log_name(const char *idl)
{
    if (idl && strcmp(idl, "dialog") == 0)
        return "DIALOG";
    if (idl && strcmp(idl, "post") == 0)
        return "POST";
    return "GET";
}

/* The HTML5 email-input rule (any browser spec shape, not site data):
 * no whitespace, one @ not at the ends, a last dot in the domain that
 * isn't first/last. */
static int fb_email_valid(const char *v)
{
    const char *p, *at, *dot;
    if (!v || !v[0])
        return 0;
    for (p = v; *p; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            return 0; /* [^\s] */
        if (*p == '@')
            break; /* first @ */
    }
    if (!*p || p == v || p[1] == 0)
        return 0;
    at = p;
    if (!at[1])
        return 0;
    dot = NULL;
    for (p = at + 1; *p; p++)
    {
        if (*p == '.')
            dot = p; /* last dot in the domain */
        else if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                 *p == '@')
            return 0;
    }
    if (!dot || dot == at + 1 || dot[1] == 0)
        return 0;
    return 1;
}

/* JSON escape (react sent JSON.stringify's output): " \ and C0 → escapes.
 * Always NUL-terminates within cap. */
static void fb_json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 7 < cap; in++)
    {
        char c = *in;
        if (c == '"' || c == '\\')
        {
            out[n++] = '\\';
            out[n++] = c;
        }
        else if (c == '\n')
        {
            out[n++] = '\\';
            out[n++] = 'n';
        }
        else if (c == '\r')
        {
            out[n++] = '\\';
            out[n++] = 'r';
        }
        else if (c == '\t')
        {
            out[n++] = '\\';
            out[n++] = 't';
        }
        else if ((unsigned char)c < 0x20)
        {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x",
                                  (unsigned char)c);
        }
        else
        {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* ── generic successful-control collection (HTML spec semantics) ───────
 * EVERY named control found within FB_WALK_MAX_DEPTH levels: name
 * (name attr, else id), type, required flag, and a GENERIC display
 * label (enclosing <label> text → placeholder → the name). Typed text
 * lives in the "value" attribute (the keyboard + autotest write-back —
 * main.c:3934); textarea falls back to its DOM text content when the
 * attr is empty; select uses its selected option's value (else the
 * first option). Spec skips: controls with no name, disabled fields,
 * unchecked checkbox/radio, and aria-hidden / "leave blank" honeypots
 * (the anti-spam convention). Live walk rows stay on for diagnosis. */
enum
{
    FB_WALK_MAX_DEPTH = 6,
    FB_FIELD_MAX = 8
};

typedef struct
{
    char name[48];
    char value[256];
    char type[16];
    char label[64];
    int required;
} FbField;
static FbField g_fbFields[FB_FIELD_MAX];
static int g_fbFieldCount;

static void fb_label_of(const DomNode *n, const char *name, char *out,
                        size_t cap)
{
    const DomNode *p = n->parent;
    int i;
    out[0] = '\0';
    if (p && p->kind == DOM_ELEMENT && p->tag &&
        strcmp(p->tag, "label") == 0)
    {
        for (i = 0; i < p->childCount; i++)
        {
            const DomNode *c = p->children[i];
            if (c->kind == DOM_TEXT && c->text && c->text[0])
            {
                snprintf(out, cap, "%s", c->text);
                return;
            }
        }
    }
    snprintf(out, cap, "%s", name);
}

static void fb_field_value(DomNode *n, const char *tag, char *out,
                           size_t cap)
{
    const char *v = form_node_attr(n, "value");
    int i;
    out[0] = '\0';
    if (v && v[0])
    {
        snprintf(out, cap, "%s", v);
        return;
    }
    if (!strcmp(tag, "textarea"))
    {
        /* HTML: a textarea's value = its DOM text content (the typed
         * text lands in the value attr first, which wins above). */
        for (i = 0; i < n->childCount; i++)
        {
            const DomNode *c = n->children[i];
            if (c->kind == DOM_TEXT && c->text && c->text[0])
            {
                snprintf(out, cap, "%s", c->text);
                return;
            }
        }
    }
    else if (!strcmp(tag, "select"))
    {
        /* the selected option's value; else the first option */
        const DomNode *first = NULL;
        for (i = 0; i < n->childCount; i++)
        {
            const DomNode *c = n->children[i];
            if (c->kind == DOM_ELEMENT && c->tag &&
                strcmp(c->tag, "option") == 0)
            {
                if (!first)
                    first = c;
                if (form_node_attr(c, "selected"))
                {
                    const char *ov = form_node_attr(c, "value");
                    snprintf(out, cap, "%s",
                             (ov && ov[0]) ? ov
                                           : (c->text && c->text[0])
                                               ? c->text
                                               : "");
                    return;
                }
            }
        }
        if (first)
        {
            const char *ov = form_node_attr(first, "value");
            snprintf(out, cap, "%s",
                     (ov && ov[0]) ? ov
                                   : (first->text && first->text[0])
                                       ? first->text
                                       : "");
        }
    }
}

static void fb_collect_fields(const DomNode *nIn, int depth)
{
    DomNode *n = (DomNode *)nIn;
    int i;
    if (!n || n->kind != DOM_ELEMENT || !n->tag ||
        depth > FB_WALK_MAX_DEPTH)
        return;
    if (!strcmp(n->tag, "input") || !strcmp(n->tag, "textarea") ||
        !strcmp(n->tag, "select"))
    {
        const char *nm = form_node_attr(n, "name");
        const char *ty = form_node_attr(n, "type");
        const char *ph = form_node_attr(n, "placeholder");
        char type[16];
        FbField *f;
        snprintf(type, sizeof(type), "%s",
                 (ty && ty[0]) ? ty
                               : strcmp(n->tag, "textarea") == 0
                                   ? "textarea"
                                   : "text");
        logger_log("[fbform]   walk d=%d %s type=%s name=%s val-probe ok",
                   depth, n->tag, type, nm ? nm : "-");
        if (!nm || !nm[0])
            nm = form_node_attr(n, "id");
        if (!nm || !nm[0])
            return; /* nameless controls are never successful */
        if (form_node_attr(n, "disabled"))
            return; /* disabled = not submitted (spec) */
        {
            const char *hid = form_node_attr(n, "aria-hidden");
            if (hid && (!hid[0] || fb_ieq(hid, "true")))
                return; /* hidden honeypot */
        }
        if (ph && strstr(ph, "leave blank"))
            return; /* the anti-spam honeypot convention */
        if (!strcmp(type, "checkbox") || !strcmp(type, "radio"))
        {
            if (!form_node_attr(n, "checked"))
                return; /* unchecked = not successful (spec) */
        }
        if (g_fbFieldCount >= FB_FIELD_MAX)
            return; /* tiny cap: log + drop beyond 8 controls */
        f = &g_fbFields[g_fbFieldCount++];
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", nm);
        fb_field_value(n, strcmp(n->tag, "select") == 0 ? "select"
                                                        : n->tag,
                       f->value, sizeof(f->value));
        snprintf(f->type, sizeof(f->type), "%s", type);
        fb_label_of(n, nm, f->label, sizeof(f->label));
        f->required = (form_node_attr(n, "required") != NULL);
        logger_log("[fbform]   field name=%s type=%s required=%d "
                   "label='%.30s' val='%.50s'",
                   f->name, f->type, f->required, f->label, f->value);
        return;
    }
    if (!strcmp(n->tag, "button") || !strcmp(n->tag, "svg") ||
        !strcmp(n->tag, "style") || !strcmp(n->tag, "script"))
        return;
    for (i = 0; i < n->childCount; i++)
        fb_collect_fields(n->children[i], depth + 1);
}

/* Reuse the card across taps (a status-node swap, like react's); virgin
 * card = create + append LAST in the form (inline, after the fields). */
static void fb_card_insert(DomResult *dom, DomNode *form, const char *text)
{
    if (g_fbCard && g_fbCard->parent == form)
    {
        dom_set_text(dom, g_fbCard, text);
    }
    else
    {
        DomNode *d = dom_create_element(dom, "div");
        if (d)
        {
            dom_set_attr(dom, d, "class", "pli-native-form-card");
            dom_set_attr(dom, d, "role", "status");
            if (dom_set_text(dom, d, text) == 0 &&
                dom_append_child(dom, form, d) == 0)
                g_fbCard = d;
        }
    }
    page_rewalk_now();
}

/* ── R56g: the post-send SUMMARY swap (replicates the sim's react view) ──
 * On 2xx react replaces the form's fields with the sent overview:
 * a label + value row per submitted field and a "Send another" button
 * (block dump 2026-10-18 22:11). The fallback mirrors that IN THE REAL
 * DOM for ANY site: the form's original children are detached and kept
 * in g_fbSaved (the arena owns them — re-appendChild restores), and the
 * card fills with the per-post summary snapshot. "Send another" taps
 * ride the normal tap path (click → submit, rc=0 = no JS) into the
 * fallback's restore branch: card off, original children re-appended,
 * values kept. The rows/texts come from the POST-time snapshot
 * (fb_summary_persist): generic browser strings, or the site profile's
 * overrides — data, not code. */
static char g_fbRowLabel[FB_FIELD_MAX][64];
static char g_fbRowValue[FB_FIELD_MAX][256];
static int g_fbRowCount;
static char g_fbTextSent[192];
static char g_fbTextFail[96];

/* Rows + texts snapshot for THIS post: profile `summary=` filters the
 * rows, `labels=`/`success=`/`failure=` rename/copy the strings. */
static void fb_summary_persist(const FormSite *site)
{
    int i;
    g_fbRowCount = 0;
    snprintf(g_fbTextSent, sizeof(g_fbTextSent), "%s",
             (site && site->success[0]) ? site->success : FB_TXT_SUCCESS);
    snprintf(g_fbTextFail, sizeof(g_fbTextFail), "%s",
             (site && site->failure[0]) ? site->failure : FB_TXT_FAIL);
    for (i = 0; i < g_fbFieldCount && g_fbRowCount < FB_FIELD_MAX; i++)
    {
        const FbField *f = &g_fbFields[i];
        char labBuf[64];
        if (site && site->summary[0] && !fb_list_has(site->summary, f->name))
            continue; /* the profile's row filter */
        fb_map_get(site ? site->labels : NULL, f->name, strlen(f->name),
                   labBuf, sizeof(labBuf));
        snprintf(g_fbRowLabel[g_fbRowCount], sizeof(g_fbRowLabel[0]), "%s",
                 (labBuf[0]) ? labBuf : f->label);
        snprintf(g_fbRowValue[g_fbRowCount], sizeof(g_fbRowValue[0]), "%s",
                 f->value);
        g_fbRowCount++;
    }
    logger_log("[fbform] summary rows=%d sent='%.50s'", g_fbRowCount,
               g_fbTextSent);
}

static void fb_summary_swap_fields(DomResult *dom, DomNode *form)
{
    int i;
    g_fbSavedCount = 0;
    for (i = 0; i < form->childCount && g_fbSavedCount < FB_SAVED_MAX; i++)
        g_fbSaved[g_fbSavedCount++] = form->children[i];
    for (i = 0; i < g_fbSavedCount; i++)
        if (g_fbSaved[i] != g_fbCard)
            dom_remove_child(form, g_fbSaved[i]);
    logger_log("[fbform] summary: hid %d child node(s) (kept card)",
               g_fbSavedCount - 1);
}

static void fb_summary_row(DomResult *dom, const char *text)
{
    DomNode *row = dom_create_element(dom, "div");
    if (row && dom_set_text(dom, row, text) == 0)
        dom_append_child(dom, g_fbCard, row);
}

static void fb_summary_build(DomResult *dom, DomNode *form)
{
    DomNode *btn;
    int i;
    (void)form;
    dom_set_text(dom, g_fbCard, g_fbTextSent);
    for (i = 0; i < g_fbRowCount; i++)
    {
        fb_summary_row(dom, g_fbRowLabel[i]);
        fb_summary_row(dom, g_fbRowValue[i]);
    }
    btn = dom_create_element(dom, "button");
    if (btn && dom_set_text(dom, btn, "Send another") == 0)
        dom_append_child(dom, g_fbCard, btn);
    else
        logger_log("[fbform] summary: Send another button MISSING (alloc)");
    g_fbSummaryOn = 1;
    /* Forms-Phase 6: the summary's owning form (the R56g restore guard
     * keys on it — scoped, not global). */
    g_fbSummaryForm = form;
    page_rewalk_now();
}

static void fb_summary_restore(DomResult *dom, DomNode *form)
{
    int i;
    logger_log("[fbform] Send another: restoring the form fields");
    if (g_fbCard)
        dom_remove_child(form, g_fbCard);
    for (i = 0; i < g_fbSavedCount; i++)
        dom_append_child(dom, form, g_fbSaved[i]);
    if (g_fbSavedCount == 0)
        logger_log("[fbform] WARNING: no saved children to restore");
    g_fbSavedCount = 0;
    g_fbSummaryOn = 0;
    g_fbSummaryForm = NULL;
    g_fbCard = NULL; /* detached; re-created on the next success */
    page_rewalk_now();
}

static void fb_on_success(int status, char **headerKeys, char **headerVals,
                          int headerCount, const char *body, size_t bodyLen,
                          const char *url)
{
    (void)headerKeys;
    (void)headerVals;
    (void)headerCount;
    (void)body;
    (void)bodyLen;
    (void)url;
    logger_log("[fbform] POST done status=%d", status);
    if (!g_fbInFlight)
        return;
    g_fbInFlight = 0;
    if (currentDoc && (DomResult *)currentDoc->_dom == g_fbDom && g_fbCard &&
        g_fbCard->parent)
    {
        DomResult *dom = (DomResult *)currentDoc->_dom;
        DomNode *form = g_fbCard->parent;
        if (status / 100 == 2)
        {
            /* R56g: 2xx = react's summary view (fields swapped out). */
            fb_summary_swap_fields(dom, form);
            fb_summary_build(dom, form);
        }
        else
        {
            fb_card_insert(dom, form, g_fbTextFail);
        }
    }
    else
        logger_log("[fbform] page changed mid-flight — card dropped");
}

static void fb_on_error(const char *message)
{
    logger_log("[fbform] POST failed: %s", message ? message : "?");
    if (!g_fbInFlight)
        return;
    g_fbInFlight = 0;
    if (currentDoc && (DomResult *)currentDoc->_dom == g_fbDom && g_fbCard &&
        g_fbCard->parent)
        fb_card_insert((DomResult *)currentDoc->_dom, g_fbCard->parent,
                       g_fbTextFail);
    else
        logger_log("[fbform] page changed mid-flight — card dropped");
}

/* application/x-www-form-urlencoded serializer (spec): space → '+',
 * letters/digits/`-_.` verbatim, everything else %XX-hex. Appends into
 * `out` at *used, always NUL-terminates, truncates at cap-4.
 * Forms-Phase 5: the byte loop now lives in the shared bridge core
 * (jsbridge_urlenc_cpy — ONE escaper feeds the standing favicon-bypass
 * routes AND the FormData wire road), wrapped here so every standing
 * caller keeps its exact shape and byte behavior. */
static void fb_urlenc_append(const char *raw, char *out, size_t cap,
                             size_t *used)
{
    jsbridge_urlenc_cpy(raw, out, cap, used);
}

/* name=value&name2=value2 over the collected fields (the DOM names are
 * the spec's keys for a standard action submission). */
static void fb_pairs_urlenc(char *out, size_t cap)
{
    size_t used = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < g_fbFieldCount; i++)
    {
        if (i && used + 1 < cap)
        {
            out[used++] = '&';
            out[used] = '\0';
        }
        fb_urlenc_append(g_fbFields[i].name, out, cap, &used);
        if (used + 1 < cap)
        {
            out[used++] = '=';
            out[used] = '\0';
        }
        fb_urlenc_append(g_fbFields[i].value, out, cap, &used);
    }
}

/* JSON.stringify-style body: {"payloadKey":"value",...}. The payload key
 * comes from the profile's keys= map (the server's contract); no profile
 * = the DOM name itself. Values stream-escape directly into the buffer. */
static void fb_pairs_json(const FormSite *site, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t u = 1; /* the '{' */
    int i;
    out[0] = '{';
    for (i = 0; i < g_fbFieldCount; i++)
    {
        const FbField *f = &g_fbFields[i];
        char keyBuf[48];
        const char *raw = f->value;
        if (u + 8 >= cap)
            break;
        if (i)
            out[u++] = ',';
        out[u++] = '"';
        /* key: characters until '"' (config-supplied payload key) */
        {
            const char *k;
            fb_map_get(site ? site->keys : NULL, f->name, strlen(f->name),
                       keyBuf, sizeof(keyBuf));
            k = (keyBuf[0]) ? keyBuf : f->name;
            while (*k && u + 4 < cap)
            {
                char c = *k++;
                if (c == '"')
                {
                    out[u++] = '\\';
                    out[u++] = '"';
                }
                else
                    out[u++] = c;
            }
        }
        out[u++] = '"';
        if (u + 2 < cap)
        {
            out[u++] = ':';
            out[u] = '\0';
        }
        /* value with the full JSON escapes */
        if (u + 2 < cap)
        {
            out[u++] = '"';
            for (; *raw && u + 7 < cap; raw++)
            {
                char c = *raw;
                if (c == '"' || c == '\\')
                {
                    out[u++] = '\\';
                    out[u++] = c;
                }
                else if (c == '\n')
                {
                    out[u++] = '\\';
                    out[u++] = 'n';
                }
                else if (c == '\r')
                {
                    out[u++] = '\\';
                    out[u++] = 'r';
                }
                else if (c == '\t')
                {
                    out[u++] = '\\';
                    out[u++] = 't';
                }
                else if ((unsigned char)c < 0x20)
                {
                    out[u++] = '\\';
                    out[u++] = 'u';
                    out[u++] = '0';
                    out[u++] = '0';
                    out[u++] = hex[((unsigned char)c >> 4) & 0xF];
                    out[u++] = hex[(unsigned char)c & 0xF];
                }
                else
                    out[u++] = c;
            }
            if (u + 1 < cap)
                out[u++] = '"';
        }
        out[u] = '\0';
    }
    if (u + 1 < cap)
        out[u++] = '}';
    out[u] = '\0';
}

/* HTML5-lite validation on the GENERIC collected fields: required-empty
 * fails, type=email fails unless it passes the spec-shaped regex. The
 * card text = the profile's invalid= (data) or the generic default. */
static int fb_fields_valid(const FormSite *site, DomResult *dom,
                           DomNode *form)
{
    int i;
    for (i = 0; i < g_fbFieldCount; i++)
    {
        const FbField *f = &g_fbFields[i];
        if (f->required && !f->value[0])
        {
            logger_log("[fbform] validation: required '%s' is empty",
                       f->name);
            fb_card_insert(dom, form,
                           (site && site->invalid[0]) ? site->invalid
                                                      : FB_TXT_INVALID);
            return 0;
        }
        if (!strcmp(f->type, "email") && f->value[0] &&
            !fb_email_valid(f->value))
        {
            logger_log("[fbform] validation: invalid email '%s' (name=%s)",
                       f->value, f->name);
            fb_card_insert(dom, form,
                           (site && site->invalid[0]) ? site->invalid
                                                      : FB_TXT_INVALID);
            return 0;
        }
    }
    return 1;
}

/* Returns 1 when the fallback OWNS the submit, 0 = no route (no form
 * action, no site profile) → the caller keeps the byte-identical legacy
 * static GET path — exactly what a script-less browser did. */
static int pluto_form_fallback(DomNode *form, const LayoutItem *item,
                               const char *actionOverride,
                               const char *methodOverride,
                               int skipValidation, DomNode *submitterNode)
{
    DomResult *dom = currentDoc ? (DomResult *)currentDoc->_dom : NULL;
    const FormSite *site;
    char ep[224];
    char ct[48];
    int methodGet = 0;
    int routeProfile = 0;
    if (!dom || !form || g_fbInFlight)
    {
        if (g_fbInFlight)
            logger_log("[fbform] submit ignored — POST already in flight");
        return 0;
    }
    /* R56g: the summary is showing — this entry comes from the "Send
     * another" button (click→submit, rc=0 = no JS). Restore the form.
     * Forms-Phase 6: scoped to the summary's OWN form — another form's
     * submit rides its own route (its method may be dialog/GET/POST). */
    if (g_fbSummaryOn && form == g_fbSummaryForm)
    {
        fb_summary_restore(dom, form);
        return 1;
    }
    (void)item; /* only the GET route navigates */
    logger_log("[fbform] form tag=%s kind=%d kids=%d attrs=%d node=%p",
               form->tag ? form->tag : "?", (int)form->kind, form->childCount,
               form->attrCount, (void *)form);
    if (!g_fbSitesLoaded)
        pluto_form_sites_load();
    site = fb_site_lookup();
    g_fbFieldCount = 0;
    fb_collect_fields(form, 0);
    if (g_fbFieldCount == 0)
    {
        logger_log("[fbform] no named control fields — old static action");
        return 0;
    }
    /* Forms-Phase 4: the submit() method road skips the profile-arc
     * required-field check too (browser truth: submit() bypasses ALL
     * constraint validation, not just the Phase-3 gate). The tap lane
     * passes skipValidation=0 — byte-identical standing behavior; its
     * empty-required case is already caught by the Phase-3 gate
     * upstream. */
    if (!skipValidation && !fb_fields_valid(site, dom, form))
        return 1; /* the validation card is up; the form stays */
    /* Forms-Phase 6: METHOD=DIALOG resolves BEFORE any route selection.
     * The spec's dict-defining description: the submission is "intended
     * to close the dialog box in which the form finds itself, if any,
     * and otherwise not submit" — the action attribute and the site
     * profile are IRRELEVANT to that intent (an actionless dialog form
     * holds, an action-bearing one also holds). The constraint gate and
     * the cancelable submit event already ran upstream (the tap lane /
     * the wire). This page shape has no ancestor <dialog>, so the
     * submission is the honest no-op: the page holds, nothing
     * navigates — the formval driver receipts the hold. */
    {
        const char *dm = (methodOverride && methodOverride[0])
                             ? methodOverride
                             : form_node_attr(form, "method");
        if (strcmp(jsbridge_form_method_idl(dm), "dialog") == 0)
        {
            const char *dact = form_node_attr(form, "action");
            logger_log("[fbform] method=dialog — no ancestor dialog "
                       "element: the page holds (action=%.80s)",
                       dact && dact[0] ? dact : "(none)");
            return 1;
        }
    }
    fb_summary_persist(site);
    /* ── ROUTE: first match wins ─────────────────────────────────────
     * a. the form has its own action → the HTML-standard submission
     *    (GET navigates action?query; POST sends urlencoded). Works for
     *    EVERY site's real HTML form with zero site knowledge.
     * b. no action, but a form_sites.txt profile matches the host →
     *    that site's endpoint + body format (data-driven).
     * c. neither → the legacy static-page GET behavior unchanged. */
    {
        char actionAbs[224];
        /* Forms-Phase 4: the submitter's stamped overrides ride the
         * route (browser truth: a submit button's formAction/formMethod
         * replace the form's own for THAT submission). The tap lane
         * passes NULL/NULL — byte-identical standing behavior. */
        const char *override = (actionOverride && actionOverride[0])
                                   ? actionOverride
                                   : NULL;
        const char *action = form_node_attr(form, "action");
        actionAbs[0] = '\0';
        const char *resolved = override ? override : action;
        if (resolved && resolved[0])
        {
            char *abs = url_resolve(
                (currentDoc && currentDoc->baseUrl) ? currentDoc->baseUrl
                                                    : "",
                resolved);
            if (abs)
            {
                snprintf(actionAbs, sizeof(actionAbs), "%s", abs);
                pluto_free(abs);
            }
        }
        if (actionAbs[0])
        {
            /* Forms-Phase 6: the effective method resolves through the
             * shared IDL reflection (the submitter's formMethod override
             * first, then the form's method attribute), so a JS read of
             * form.method and a wire route cannot disagree. The dialog
             * method already returned upstream — what reaches the route
             * block is GET or POST only. */
            const char *m =
                (methodOverride && methodOverride[0]) ? methodOverride
                                                      : form_node_attr(form, "method");
            const char *idlMethod =
                jsbridge_form_method_idl(m);
            methodGet = strcmp(idlMethod, "get") == 0;
            snprintf(ep, sizeof(ep), "%s", actionAbs);
            snprintf(ct, sizeof(ct), "%s",
                     "application/x-www-form-urlencoded");
            if (override || methodOverride)
            {
                logger_log("[fbform] submitter override action=%.80s "
                           "method=%s",
                           ep, fb_method_log_name(idlMethod));
            }
            logger_log("[fbform] route=standard action=%.80s method=%s",
                       ep, fb_method_log_name(idlMethod));
        }
        else if (site && site->endpoint[0])
        {
            snprintf(ep, sizeof(ep), "%s", site->endpoint);
            snprintf(ct, sizeof(ct), "%s", site->bodyJson
                                                ? "application/json"
                                                : "application/x-www-form-urlencoded");
            routeProfile = 1;
            logger_log("[fbform] route=profile host=%.40s endpoint=%.70s "
                       "body=%s",
                       site->host, ep, site->bodyJson ? "json" : "url");
        }
        else
        {
            logger_log("[fbform] no action, no profile — legacy static GET");
            return 0;
        }
    }
    /* Forms-Phase 6: the dialog-method hold happens BEFORE the route
     * block (see the early resolution above) — every shape (action
     * present or not, profile matched or not) holds the same way. */
    if (methodGet)
    {
        /* Forms-Phase 6: the GET submission is the browser-true query —
         * the SAME one-rulebook form data set the POST body carries
         * (the submitter pair rides a real activation), serialized with
         * the standing escaper. The double-append shape is gone: the
         * legacy fb-collector chunk (id-fallback names) no longer rides
         * the query, and this branch no longer hands the finished URL
         * to submit_form to append the layout's own copy a second time.
         * enctype is IGNORED here (the GET query is always the
         * urlencoded shape — the browser's own rule).
         * The commit rides the standing pending-nav lane (the same
         * deferred commit submit_form left for the frame loop): a GET
         * from a tap or a script chain must not free the layout/doc
         * mid-walk. An action that already carries a query joins with
         * '&' (the browser's own append); an empty data set leaves the
         * action bare. */
        char enc[FB_BODY_CAP];
        char url[640];
        size_t off = strlen(ep);
        if (off >= sizeof(url))
            off = sizeof(url) - 1;
        memcpy(url, ep, off);
        url[off] = '\0';
        jsbridge_form_data_urlenc(g_pageJs, form, submitterNode,
                                  enc, sizeof(enc));
        if (enc[0])
        {
            const char *join = strchr(url, '?') ? "&" : "?";
            snprintf(url + off, sizeof(url) - off, "%s%s", join, enc);
        }
        logger_log("[fbform] GET submit → url=%.110s", url);
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", url);
        return 1;
    }
    {
        HttpCallbacks cbs;
        int started;
        if (routeProfile && site->bodyJson)
            fb_pairs_json(site, g_fbBody, sizeof(g_fbBody));
        else if (routeProfile)
            fb_pairs_urlenc(g_fbBody, sizeof(g_fbBody));
        else
        {
            /* Forms-Phase 5: the route-standard POST body is the SAME
             * browser-true form data set the JS FormData constructor
             * builds — one collector, two roads (the parity bar). The
             * submitter pair rides a real activation (browser truth:
             * the submitter button contributes its pair); form.submit()
             * and the battery road pass NULL submitter. */
            /* Forms-Phase 6: the submitter's effective enctype picks
             * the serializer the browser would run. urlencoded stays
             * the standing road; text/plain rides the raw CRLF wire
             * (its honest Content-Type); multipart stays the
             * documented policy boundary (no file picker, no encoder —
             * the body rides urlencoded and says so on the record). */
            const char *etr = submitterNode
                                  ? form_node_attr((DomNode *)submitterNode,
                                                   "formenctype")
                                  : NULL;
            if (!etr || !etr[0])
                etr = form_node_attr(form, "enctype");
            const char *encl = jsbridge_form_enctype_idl(etr);
            int textPlain = strcmp(encl, "text/plain") == 0;
            int multipart = strcmp(encl, "multipart/form-data") == 0;
            if (textPlain)
            {
                jsbridge_form_data_plain(g_pageJs, form, submitterNode,
                                         g_fbBody, sizeof(g_fbBody));
                snprintf(ct, sizeof(ct), "%s", "text/plain;charset=UTF-8");
            }
            else
            {
                if (multipart)
                {
                    logger_log("[fbform] enctype=multipart/form-data — "
                               "policy boundary (no file picker, no "
                               "encoder): the body rides urlencoded");
                }
                jsbridge_form_data_urlenc(g_pageJs, form, submitterNode,
                                          g_fbBody, sizeof(g_fbBody));
            }
        }
        logger_log("[fbform] POST %s body=%u (%s)", ep,
                   (unsigned)strlen(g_fbBody), ct);
        if (!routeProfile)
        {
            /* Forms-Phase 5 receipt: the browser-true body bytes on the
             * record (pairs in tree order; the battery pairs this with
             * the page-side FormData recount). */
            logger_log("[fbform] POST body bytes=%s", g_fbBody);
        }
        g_fbDom = dom;
        fb_card_insert(dom, form,
                       (site && site->sending[0]) ? site->sending
                                                  : FB_TXT_SENDING);
        memset(&cbs, 0, sizeof(cbs));
        cbs.onSuccess = fb_on_success;
        cbs.onError = fb_on_error;
        started = http_request_ct("POST", ep, ct, g_fbBody, strlen(g_fbBody),
                                  &cbs);
        if (!started)
        {
            fb_on_error("http refused");
            return 1;
        }
        g_fbInFlight = 1;
    }
    return 1;
}

/* Nearest enclosing <form> element (B5: the submit event's target — browser
 * dispatches an activation click on the button, then submit on the form). */
static DomNode *form_node_ancestor(DomNode *n)
{
    int hops = 0;
    for (const DomNode *p = n ? n->parent : NULL; p; p = p->parent, hops++)
    {
        if (p->kind == DOM_ELEMENT && p->tag && strcmp(p->tag, "form") == 0)
        {
            return (DomNode *)p;
        }
    }
    /* Diagnostics: a submit tap that cannot find its <form> usually means
     * the stamped node belongs to a DETACHED subtree (fallback restore/
     * hydration replacement swapped the body) — log the parent chain so
     * the replay + sim logs answer "which tree was the stamp on?" */
    if (n)
    {
        logger_log("[form] noform ancestor: srcNode tag=%s kind=%d",
                   (n->kind == DOM_ELEMENT && n->tag) ? n->tag : "(text)",
                   (int)n->kind);
        hops = 0;
        for (const DomNode *p = n->parent; p && hops < 6; p = p->parent, hops++)
        {
            logger_log("[form] noform chain hop %d: tag=%s kind=%d kids=%d",
                       hops + 1,
                       (p->kind == DOM_ELEMENT && p->tag) ? p->tag
                                                          : "(text)",
                       (int)p->kind, p->childCount);
        }
        if (hops == 0)
        {
            logger_log("[form] noform chain: srcNode is ROOT-LEVEL "
                       "(parent=NULL — detached?)");
        }
    }
    return NULL;
}

/* Recursive DOM search (document order, depth-capped like the builder):
 * ordinal-th text input/textarea with this key. Textarea counts by
 * document order; hidden/non-text types never match. */
static DomNode *form_node_search(DomNode *n, int *ord, int depth)
{
    if (!n || depth > DOM_SEARCH_MAX_DEPTH)
    {
        return NULL;
    }
    if (n->kind == DOM_ELEMENT && n->tag)
    {
        int isArea = strcmp(n->tag, "textarea") == 0;
        int isTextInput = strcmp(n->tag, "input") == 0 &&
                          form_is_text_type(form_node_attr(n, "type"));
        if ((isArea || isTextInput))
        {
            const char *nm = form_node_attr(n, "name");
            if (!nm)
            {
                nm = "q";
            }
            if (strcmp(nm, g_formKey.name) == 0)
            {
                char fa[256];
                const char *fact = form_node_attr(n, "formaction");
                if (fact)
                {
                    const char *base = (currentDoc && currentDoc->baseUrl)
                                           ? currentDoc->baseUrl
                                           : "";
                    char *abs = url_resolve(base, fact);
                    snprintf(fa, sizeof(fa), "%s", abs ? abs : "");
                    if (abs)
                    {
                        pluto_free(abs);
                    }
                }
                else
                {
                    form_node_form(fa, sizeof(fa), n);
                }
                if (strcmp(fa, g_formKey.form) == 0)
                {
                    if (*ord == g_formKey.ordinal)
                    {
                        return n;
                    }
                    (*ord)++;
                }
            }
        }
    }
    for (int i = 0; i < n->childCount; i++)
    {
        DomNode *hit = form_node_search(n->children[i], ord, depth + 1);
        if (hit)
        {
            return hit;
        }
    }
    return NULL;
}

static DomNode *form_key_node(void)
{
    if (!g_formKey.valid || !currentDoc || !currentDoc->_dom)
    {
        return NULL;
    }
    DomResult *dom = (DomResult *)currentDoc->_dom;
    if (!dom->root)
    {
        return NULL;
    }
    int ord = 0;
    return form_node_search(dom->root, &ord, 0);
}

/* Write keystrokes to the live item (echo) AND the live DOM node
 * (survives rebuilds: the walker rebuilds block->value from the attr).
 * B4: `commit` selects the event set — 0 = live keystroke (fires "input"),
 * 1 = keyboard commit (fires "input" then "change", the browser's
 * keystroke/blur split). With a page engine attached, react's controlled
 * inputs sync state from these events; without them the value only ever
 * exists on the DOM attr and react's submit validation refuses the form. */
static void form_key_write(const char *text, int commit)
{
    if (!text)
    {
        return;
    }
    const LayoutItem *it = form_key_item();
    if (it)
    {
        layout_set_input_value(it, text);
        layout_set_selected_input(it);
    }
    if (g_formKey.valid && currentDoc && currentDoc->_dom)
    {
        DomNode *n = form_key_node();
        if (n && n->tag && (strcmp(n->tag, "input") == 0 ||
                            strcmp(n->tag, "textarea") == 0))
        {
            /* The value lands on the node BEFORE the events fire: react's
             * onChange reads target.value synchronously. textarea now
             * included — the walker rebuilds its value the same way. */
            dom_set_attr((DomResult *)currentDoc->_dom, n, "value", text);
        }
        if (g_pageJs && n && n->tag)
        {
            int r = jsbridge_dispatch_element_event(g_pageJs, n, "input");
            if (commit)
            {
                int r2 = jsbridge_dispatch_element_event(g_pageJs, n, "change");
                if (r == JSB_EVENT_NONE)
                {
                    r = r2;
                }
            }
            if (r == JSB_EVENT_SUPPRESSED)
            {
                page_rewalk_now(); /* a handler mutated the live DOM */
            }
        }
    }
}

/* ── Form input values ─────────────────────────────────────────────────────
 * Lua input items alias the block table, so openKeyboardForInput's
 * activeInputField.value = entered made typed text visible to BOTH the
 * renderer and the submit path. In C the LayoutItem and DocBlock are separate
 * structs, so the typed text lives on the item itself (owned heap copy set
 * through layout_set_input_value; freed by layout_clear). form_item_value()
 * therefore just reads the item's current value. */
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
    form_key_snapshot(item);
    formKeyboardOpen = 1;
    layout_set_selected_input(item); /* Lua: Layout.selectedInputItem = block */
    keyboardApi.setKeyboardWillHideCallback(g_kb, form_kb_will_hide, NULL);
    keyboardApi.setKeyboardDidHideCallback(g_kb, form_kb_did_hide, NULL);
    keyboardApi.setTextChangedCallback(g_kb, form_kb_text_changed, NULL);
    keyboardApi.show(g_kb, item->value ? item->value : "",
                     (unsigned int)(item->value ? strlen(item->value) : 0));
}

static int g_formKbOkayPressed = 0; /* last will_hide's OK flag (Phase 4) */
static void form_kb_will_hide(int okButtonPressed, void *ud)
{
    (void)ud;
    /* Forms-Phase 4: remember WHY the keyboard hid. OK = the Enter
     * commit → implicit submission follows the change commit (the
     * browser's Enter order). A plain dismissal (tapping elsewhere)
     * commits without submitting. */
    g_formKbOkayPressed = okButtonPressed ? 1 : 0;
}

static void form_kb_did_hide(void *ud)
{
    (void)ud;
    formKeyboardOpen = 0;
    skipInputFrames = 2;
    if (g_kb)
    {
        char *txt = NULL;
        unsigned int n = 0;
        keyboardApi.getText(g_kb, &txt, &n);
        if (txt)
        {
            /* Lua: maxlength truncation on commit (read from the live
             * item — the one at open time may be long freed). */
            const LayoutItem *it = form_key_item();
            if (it && it->maxlength >= 0 && (int)n > it->maxlength)
            {
                txt[it->maxlength] = '\0';
            }
            form_key_write(txt, 1 /* commit → input + change */);
            if (g_formKbOkayPressed)
            {
                /* Forms-Phase 4: the browser's Enter order — the commit
                 * (input+change) just ran; implicit submission follows
                 * when the field belongs to a form. textarea fields
                 * never implicitly submit (pluto_form_enter_commit
                 * checks). */
                logger_log("[form] keyboard OK commit → enter chain");
                pluto_form_enter_commit();
            }
            else
            {
                logger_log("[form] keyboard dismissed commit (no submit)");
            }
            g_formKbOkayPressed = 0;
            pluto_free(txt);
        }
    }
    memset(&g_formKey, 0, sizeof(g_formKey)); /* valid=0 */
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
    /* Lua updateFrame: live-sync keyboard text into the focused input.
     * Resolved per keystroke — never through the open-time pointer. */
    if (g_kb)
    {
        char *txt = NULL;
        unsigned int n = 0;
        keyboardApi.getText(g_kb, &txt, &n);
        if (txt)
        {
            (void)n;
            form_key_write(txt, 0 /* live keystroke → input */);
            pluto_free(txt);
        }
    }
}

/* Forms-Phase 4 (autotest lane): the keyboard's OK commit exactly as
 * form_kb_did_hide rides it — the change commit, then the Enter-implicit
 * chain (pluto_form_enter_commit checks the field's form, the textarea
 * exception and the default button). The driver calls this to exercise
 * a real user's OK press without opening the OS keyboard. */
static void form_kb_ok_commit(const char *text)
{
    form_key_write(text, 1 /* commit → input + change */);
    logger_log("[form] enter: OK commit driver lane");
    pluto_form_enter_commit();
}

/* ── activateFormBlock (port) ───────────────────────────────────────────── */
/* Phase 8: the host side of a type=reset activation (the browser's
 * form.reset()): every same-form control returns to the walk-time
 * default captured at block creation (inputs/textarea values,
 * checkbox/radio checked bits, select index + multiple accumulation).
 * Disabled controls reset too (spec: reset is not a submit filter).
 * Returns 1 when any control state changed — the only thing worth a
 * re-walk. Scope = the tapped item's formAction string, the same
 * same-form test the submit serializer uses.
 * Forms-Phase 4: text restore now reaches BOTH other stores — the
 * node's value ATTRIBUTE (the typing carrier; without this a re-walk
 * resurrects the typed text over the block restore) and the focused
 * item's owned value (the keyboard echo). Restores fire no events —
 * a browser's reset() fires none (the reset EVENT rides the button,
 * not the restore). */
static int host_form_reset(const LayoutItem *item)
{
    const char *fa = (item && item->formAction) ? item->formAction : "";
    int changed = 0;
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (!it || !it->formAction || fa[0] == '\0' ||
            strcmp(it->formAction, fa) != 0)
        {
            continue;
        }
        DocBlock *b = (DocBlock *)it->block;
        if (!b)
        {
            continue;
        }
        switch (it->type)
        {
        case LRI_HIDDEN_FIELD:
        case LRI_INPUT_FIELD:
            if (b->value != b->defaultValue)
            {
                b->value = b->defaultValue; /* arena alias of the walk value */
                /* Forms-Phase 4: the value attr carries typed text —
                 * restore it so the next re-walk keeps the default. */
                DomResult *dom = currentDoc ? (DomResult *)currentDoc->_dom
                                            : NULL;
                if (dom && b->srcNode)
                {
                    dom_set_attr(dom, (DomNode *)b->srcNode, "value",
                                 b->defaultValue ? b->defaultValue : "");
                }
                /* The focused item's owned echo follows the restore. */
                layout_set_input_value(it, b->defaultValue ? b->defaultValue
                                                           : "");
                changed = 1;
            }
            break;
        case LRI_CHECKBOX_FIELD:
            if (b->checked != b->defaultChecked)
            {
                b->checked = b->defaultChecked;
                changed = 1;
            }
            break;
        case LRI_SELECT_FIELD:
        {
            int def = (b->defaultSelIndex >= 1) ? b->defaultSelIndex : 1;
            if (b->selectedIndex != def)
            {
                b->selectedIndex = def;
                changed = 1;
            }
            if (b->multiple)
            {
                for (int k = 0; k < b->optionCount; k++)
                {
                    DocOption *o = b->options[k];
                    if (o && !o->group && o->selected != o->defaultSelected)
                    {
                        o->selected = o->defaultSelected;
                        changed = 1;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return changed;
}

static int form_gate_general(JsBridge *b, DomNode *formNode,
                             DomNode *submitterNode)
{
    if (!b || !formNode || formNode->kind != DOM_ELEMENT)
    {
        return 1; /* no bridge / no form: standing behavior */
    }
    if (dom_get_attr((DomNode *)formNode, "novalidate") ||
        (submitterNode && dom_get_attr(submitterNode, "formnovalidate")))
    {
        return 1;
    }
    static DomNode *offenders[8]; /* bss scratch — single-threaded caller */
    JsbValidity first;
    memset(&first, 0, sizeof(first));
    int n = jsbridge_form_check(b, formNode, offenders, 8, &first);
    if (n == 0)
    {
        return 1;
    }
    for (int i = 0; i < n && i < 8; i++)
    {
        /* Fire-and-forget: the block verdict is the C evaluation, not
         * the event's return. */
        (void)jsbridge_dispatch_element_event(b, offenders[i], "invalid");
    }
    logger_log("[form] submit blocked by validation (%d): %s", n,
               first.message);
    return 0;
}

/* Forms-Phase 3: the native submit gate — the browser runs constraint
 * validation on the click's default action BEFORE any submit event can
 * dispatch. novalidate on the <form> or formNovalidate on the button
 * skips it; offenders fire their non-bubbling `invalid` events (the
 * kind-6 road: the live NR engine delivers, frozen engines dissolve
 * the delivery per the standing boundary) and the submit BLOCKS at the
 * source — no submit dispatch, no wire, nothing sent. Boundary: the
 * gate rides the page's engine bridge (the rules read the same live
 * DOM as the JS surface); a page with JS runs it, a script-less page
 * keeps the standing byte-identical static behavior. */
static int form_submit_gate_pass(DomNode *btnNode)
{
    if (!g_pageJs)
    {
        return 1; /* no bridge: standing static behavior */
    }
    DomNode *form = form_node_ancestor(btnNode);
    return form_gate_general(g_pageJs, form, btnNode);
}

/* ── Forms-Phase 4: the form API host hooks ─────────────────────────────── *
 * Browser truth: HTMLFormElement.reset() restores every control to its
 * walk-time default and fires NO events; submit() skips validation AND
 * the submit event; requestSubmit([submitter]) runs the full chain —
 * the constraint gate (novalidate/formNovalidate honored), then the
 * cancelable submit event, then the wire (the submitter's formAction/
 * formMethod overrides ride along). The Enter-implicit chain reuses the
 * same wire with no submitter. All roads mark domDirty through the
 * standing rewalk road — queued render tasks, never mid-script walks. */

/* Layout item back-lookup by source node: the only item-level state the
 * wire needs is the submitter's stamped formAction/formMethod pair. */
static const LayoutItem *form_item_by_node(DomNode *node)
{
    if (!node)
    {
        return NULL;
    }
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->block && ((const DocBlock *)it->block)->srcNode == node)
        {
            return it;
        }
    }
    return NULL;
}

/* The implicit-submission default button: the FIRST submit button in
 * tree order under the form (input type=submit/image and button
 * elements whose type is submit or defaulted). Iterative stack walk —
 * no recursion on the 64KB game-task stack. */
static DomNode *form_default_button(DomNode *formNode)
{
    if (!formNode)
    {
        return NULL;
    }
    DomNode *stack[24]; /* bounded — the game-task stack is 64KB */
    int top = 0;
    for (int i = 0; i < formNode->childCount && top < 24; i++)
    {
        stack[top++] = formNode->children[i];
    }
    while (top > 0)
    {
        DomNode *c = stack[--top];
        if (!c || c->kind != DOM_ELEMENT)
        {
            continue;
        }
        if (c->tag)
        {
            if (!strcmp(c->tag, "button"))
            {
                /* <button> defaults to type=submit */
                const char *t = dom_get_attr(c, "type");
                if (!t || !t[0] || !strcmp(t, "submit"))
                {
                    return c;
                }
            }
            else if (!strcmp(c->tag, "input"))
            {
                const char *t = dom_get_attr(c, "type");
                if (t && (!strcmp(t, "submit") || !strcmp(t, "image")))
                {
                    return c;
                }
            }
            else if (!strcmp(c->tag, "form"))
            {
                continue; /* nested forms break containment */
            }
        }
        for (int j = c->childCount - 1; j >= 0; j--)
        {
            if (c->children[j] && top < 24)
            {
                stack[top++] = c->children[j];
            }
        }
    }
    return NULL;
}

/* The form.reset() hook (JS method road + the reset-button upgrade):
 * the walk-time-default restore over the form's whole scope. The block
 * road covers the walked page (taps live on blocks); a pre-walk call
 * finds no matching items and the DOM attrs already ARE the defaults —
 * a browser-true two-armed no-op. Returns 1 when anything changed. */
static int pluto_host_form_reset(JsBridge *b, void *formNode)
{
    (void)b;
    DomNode *form = (DomNode *)formNode;
    if (!form || form->kind != DOM_ELEMENT || !form->tag ||
        strcmp(form->tag, "form") != 0)
    {
        return 0;
    }
    for (int i = 0; i < layout_get_item_count(); i++)
    {
        const LayoutItem *it = layout_item_at(i);
        if (it && it->block && ((const DocBlock *)it->block)->srcNode &&
            form_node_ancestor((DomNode *)((const DocBlock *)it->block)->srcNode) ==
                form)
        {
            int changed = host_form_reset(it);
            logger_log("[form] form.reset(): %s",
                       changed ? "walk-time defaults restored"
                               : "already at defaults, nothing changed");
            return changed;
        }
    }
    logger_log("[form] form.reset(): no walked controls (pre-walk call, "
               "DOM state already defaults)");
    return 0;
}

/* The submission-chain hook. wantGate runs the constraint layer first
 * (blocked = no submit event wired, offenders fired); wantSubmitEvent
 * dispatches the cancelable submit; SUPPRESSED = a handler owns the
 * submission (rewalk, no wire). Otherwise the wire routes exactly like
 * the tap lane: pluto_form_fallback (standard action GET/POST with the
 * submitter's formAction/formMethod overrides, then the data-driven
 * site profile), and its legacy fallthrough rides submit_form (the
 * formless self-GET, the button's name=value pair included). */
static int pluto_host_form_wire(JsBridge *b, void *formNode,
                                void *submitterNode, int wantGate,
                                int wantSubmitEvent)
{
    DomNode *form = (DomNode *)formNode;
    DomNode *submitter = (DomNode *)submitterNode;
    if (!form || form->kind != DOM_ELEMENT || !form->tag ||
        strcmp(form->tag, "form") != 0)
    {
        return 0;
    }
    logger_log("[form] wire requested: form=%s submitter=%s gate=%d event=%d",
               form->tag, submitter && submitter->tag ? submitter->tag : "none",
               wantGate, wantSubmitEvent);
    /* Reset activation (the submitter is a type=reset control): the
     * browser order is the cancelable reset event on the form, then the
     * restore. The gate and the wire do not apply to resets. */
    const char *st = submitter && submitter->tag ? submitter->tag : NULL;
    int isReset = 0;
    if (st && !strcmp(st, "button") &&
        dom_get_attr(submitter, "type") &&
        !strcmp(dom_get_attr(submitter, "type"), "reset"))
    {
        isReset = 1;
    }
    else if (st && !strcmp(st, "input"))
    {
        const char *t = dom_get_attr(submitter, "type");
        if (t && !strcmp(t, "reset"))
        {
            isReset = 1;
        }
    }
    if (isReset)
    {
        if (g_pageJs)
        {
            int rr = jsbridge_dispatch_element_event(b, form, "reset");
            logger_log("[form] reset event: rc=%d", rr);
            if (rr == JSB_EVENT_SUPPRESSED)
            {
                page_rewalk_now(); /* a reset handler owns the restore */
                return 0;
            }
        }
        const LayoutItem *ri = form_item_by_node(submitter);
        int changed = ri ? host_form_reset(ri) : pluto_host_form_reset(b, form);
        if (changed)
        {
            page_rewalk_now();
        }
        logger_log("[form] reset performed (%s)",
                   changed ? "changed" : "already default");
        return changed;
    }
    /* (1) the constraint gate — browser order: validation precedes the
     * submit event on every path that honors it. */
    if (wantGate && !form_gate_general(b, form, submitter))
    {
        return 0;
    }
    /* (2) the cancelable submit event — over the whole form (the
     * page's bridge owns the delivery even mid-parse). Frozen engines
     * dissolve the delivery (standing boundary) and NONE falls through
     * to the wire, matching a page with no listeners. */
    if (wantSubmitEvent && b)
    {
        int rc = jsbridge_dispatch_element_event(b, form, "submit");
        logger_log("[form] submit dispatch: rc=%d", rc);
        if (rc == JSB_EVENT_SUPPRESSED)
        {
            page_rewalk_now(); /* onSubmit handled the submission */
            return 0;
        }
    }
    /* (3) the wire — the submitter's stamped overrides ride the route. */
    const LayoutItem *sub = form_item_by_node(submitter);
    if (sub && !sub->disabled)
    {
        int conducted =
            pluto_form_fallback(form, sub, sub->formAction, sub->formMethod,
                                !wantGate,
                                (sub && sub->block)
                                    ? ((DocBlock *)sub->block)->srcNode
                                    : NULL);
        if (conducted)
        {
            return 1;
        }
    }
    if (pluto_form_fallback(form, NULL, NULL, NULL, !wantGate, NULL))
    {
        return 1;
    }
    /* (4) legacy route: no action attr, no site profile — the browser's
     * own submission to the form's resolved action (the current URL for
     * an empty action), the submitter pair included. */
    logger_log("[form] wire legacy route: submit_form self GET");
    submit_form(sub ? sub->formAction : "", sub);
    return 1;
}

/* The Enter-implicit chain (the keyboard's OK commit). Browser truth:
 * implicit submission on a TEXT field rides the form's default button
 * when one exists (its click + activation: the click, then the gate
 * honoring its formNovalidate, then the submit event, then the wire);
 * with no submit button the form itself is submitted (gate, event,
 * wire, no submitter pair). textarea never implicitly submits. */
static void pluto_form_enter_commit(void)
{
    const LayoutItem *it = form_key_item();
    if (!it)
    {
        return;
    }
    if (it->inputType && strcmp(it->inputType, "textarea") == 0)
    {
        logger_log("[form] enter on textarea: newline, no implicit submit");
        return;
    }
    DomNode *n = form_key_node();
    if (!n)
    {
        return;
    }
    DomNode *form = form_node_ancestor(n);
    if (!form)
    {
        return; /* formless field: nothing to submit */
    }
    DomNode *btn = form_default_button(form);
    if (btn)
    {
        const LayoutItem *bi = form_item_by_node(btn);
        if (bi)
        {
            logger_log("[form] enter: implicit submission rides the default "
                       "button (tag=%s)",
                       btn->tag ? btn->tag : "?");
            activate_form_block(bi); /* the full tap chain: click → … */
            return;
        }
        /* The button exists in the DOM but is not a walked item (a
         * pre-walk commit). Run the chain minus the click dispatch. */
        logger_log("[form] enter: default button unwalked — direct chain");
        pluto_host_form_wire(g_pageJs, form, btn, 1, 1);
        return;
    }
    logger_log("[form] enter: no submit button — direct implicit submission");
    pluto_host_form_wire(g_pageJs, form, NULL, 1, 1);
}

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
    {
        /* B5: the tap becomes a real dispatched "click" on the button's
         * DOM node first, then a "submit" on the nearest <form> — the
         * browser's activation order. A handler calling preventDefault()
         * (react's onSubmit) re-walks the LIVE DOM instead of the old
         * hardcoded submit_form reload; with no listeners at all, the
         * dispatch short-circuits to NONE and submit_form runs exactly
         * as before (byte-identical static-page behavior). */
        const DocBlock *blk = (const DocBlock *)item->block;
        void *srcNode = blk ? blk->srcNode : NULL;
        /* Phase 8: a type=reset button's activation does NOT submit —
         * the browser order is click, then form.reset() (no submit). A
         * JS handler can still own the click (preventDefault = the
         * handler re-walks); with no handler the HOST performs the
         * reset: every same-form control returns to its walk-time
         * default (captured at block creation). Formless reset buttons
         * do nothing (spec). Without JS at all the host reset still
         * runs — a static page's reset is pure DOM behavior. */
        int isReset =
            (blk && blk->buttonType && strcmp(blk->buttonType, "reset") == 0);
        if (isReset && !srcNode)
        {
            logger_log("[form] reset without a source node: nothing to do");
            break;
        }
        if (isReset && !g_pageJs)
        {
            DomNode *formNode = form_node_ancestor(srcNode);
            if (formNode)
            {
                if (host_form_reset(item))
                {
                    logger_log("[form] reset performed (no JS)");
                    page_rewalk_now();
                }
            }
            else
            {
                logger_log("[form] formless reset: nothing to reset");
            }
            break;
        }
        int rc = JSB_EVENT_NONE;
        if (g_pageJs && srcNode)
        {
            rc = jsbridge_dispatch_element_event(g_pageJs, srcNode, "click");
            logger_log("[form] %s tap: click=%d node=%p tag=%s",
                       isReset ? "reset" : "submit", rc, srcNode,
                       ((DomNode *)srcNode)->tag ? ((DomNode *)srcNode)->tag : "?");
            if (rc == JSB_EVENT_SUPPRESSED)
            {
                page_rewalk_now(); /* the click handler owns the action */
                break;
            }
            if (isReset)
            {
                DomNode *formNode = form_node_ancestor(srcNode);
                if (formNode)
                {
                    /* Forms-Phase 4: the browser order is click, then
                     * the cancelable `reset` event on the form, then
                     * the restore. A handler cancelling the reset owns
                     * the form (rewalk); NONE and NAVIGATE both fall
                     * through to the host restore. */
                    int re = jsbridge_dispatch_element_event(
                        g_pageJs, formNode, "reset");
                    logger_log("[form] reset event dispatch: rc=%d", re);
                    if (re == JSB_EVENT_SUPPRESSED)
                    {
                        page_rewalk_now(); /* a reset handler owns it */
                        break;
                    }
                    if (host_form_reset(item))
                    {
                        logger_log("[form] reset performed");
                        page_rewalk_now();
                    }
                }
                else
                {
                    logger_log("[form] formless reset: nothing to reset");
                }
                break;
            }
            DomNode *formNode = form_node_ancestor(srcNode);
            if (formNode)
            {
                /* Forms-Phase 3: the native gate runs on the default
                 * action, between the click and the submit event (the
                 * browser order). Blocked = no submit dispatch. */
                if (!form_submit_gate_pass((DomNode *)srcNode))
                {
                    break;
                }
                rc = jsbridge_dispatch_element_event(g_pageJs, formNode, "submit");
                logger_log("[form] submit dispatch: rc=%d", rc);
                if (rc == JSB_EVENT_SUPPRESSED)
                {
                    page_rewalk_now(); /* onSubmit handled the submission */
                    break;
                }
                /* R56f fallback: nothing matched the event (rc=NONE) = no
                 * JS handler ran — the react engine is reset/absent (the
                 * R56d device finding). The browser performs the site's
                 * own submission now: validate the email, show the error/
                 * Sending/sent card inline, POST the page's exact JSON
                 * body. Only the email-form shape takes this path; a miss
                 * (returns 0) falls through to the byte-identical static
                 * GET below. */
                if (rc == JSB_EVENT_NONE)
                {
                    logger_log("[form] submit unhandled — native fallback "
                               "attempt");
                    if (pluto_form_fallback(formNode, item, NULL, NULL, 0,
                                            (item && item->block)
                                                ? ((DocBlock *)item->block)
                                                      ->srcNode
                                                : NULL))
                        break;
                }
            }
            else
            {
                /* Formless button (nav toggles, tab bars, …): a browser's
                 * native behavior for a formless <button type=submit> click
                 * is NOTHING — no submit runs. The click dispatch above
                 * already delivered the event (NAVIGATE = handlers ran,
                 * none prevented) so a script-driven button still works. */
                logger_log("[form] formless button: no submit run (rc=%d)",
                           rc);
                break;
            }
        }
        submit_form(item->formAction, item);
        break;
    }
    case LRI_CHECKBOX_FIELD:
    {
        DocBlock *block = (DocBlock *)item->block;
        int wasChecked = block->checked;
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
        /* Forms-Phase 2: a user toggle delivers input, then change (the
         * browser's activation order — the toggle moves the checkedness
         * first; a re-tap on an already-checked radio moves nothing and
         * fires nothing, exactly like a browser's no-op re-click). The
         * events ride the field's own source node, mirroring the
         * form_key_write contract: change only counts when input's
         * dispatch found no handler (r falls back to r2), and a handler
         * that mutated the live DOM re-walks the page. The JS .checked
         * WRITE accessor fires no events — only USER toggles do. */
        if (block->checked != wasChecked)
        {
            void *srcNode = block->srcNode;
            if (g_pageJs && srcNode)
            {
                int r =
                    jsbridge_dispatch_element_event(g_pageJs, srcNode, "input");
                int r2 =
                    jsbridge_dispatch_element_event(g_pageJs, srcNode, "change");
                if (r == JSB_EVENT_NONE)
                {
                    r = r2;
                }
                if (r == JSB_EVENT_SUPPRESSED)
                {
                    page_rewalk_now(); /* a handler mutated the live DOM */
                }
            }
        }
        break;
    }
    case LRI_SELECT_FIELD:
    {
        DocBlock *block = (DocBlock *)item->block;
        int wasSi = block->selectedIndex;
        int picked = 0; /* an accumulate actually changed the pick set */
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
                    /* Phase 8: on a multiple select the cycle ACCUMULATES
                     * the pick (each arrival marks the option selected)
                     * so the user's taps join the author's selected=
                     * truth in the submit set. No deselect UI ships in
                     * this phase — the reset button (or a JS rewrite of
                     * option.selected) is the way back. */
                    if (block->multiple)
                    {
                        if (!opt->selected)
                        {
                            opt->selected = 1;
                            picked = 1;
                        }
                    }
                    break;
                }
                tries--;
            }
        }
        layout_set_selected_input(item);
        /* Forms-Phase 2: a user pick delivers input, then change (the
         * browser's activation order — the selection moves first; when
         * the cycle arrives nowhere new and nothing got marked, the
         * state did not change and nothing fires). Same dispatch
         * contract as the checkbox lane and form_key_write: change only
         * counts when input's dispatch found no handler, and a handler
         * that mutated the live DOM re-walks the page. */
        if (g_pageJs &&
            (block->selectedIndex != wasSi || picked))
        {
            void *srcNode = block->srcNode;
            if (srcNode)
            {
                int r =
                    jsbridge_dispatch_element_event(g_pageJs, srcNode, "input");
                int r2 =
                    jsbridge_dispatch_element_event(g_pageJs, srcNode, "change");
                if (r == JSB_EVENT_NONE)
                {
                    r = r2;
                }
                if (r == JSB_EVENT_SUPPRESSED)
                {
                    page_rewalk_now(); /* a handler mutated the live DOM */
                }
            }
        }
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
            /* Checked state lives on the DocBlock (taps mutate the block,
             * not the item cache) — read it live so submit sends the
             * option the user actually picked. */
            if (!it->disabled && sameForm && it->block &&
                ((const DocBlock *)it->block)->checked)
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
                if (b->multiple)
                {
                    /* Phase 8: a multiple select submits EVERY selected
                     * option as its own name=value pair (the real
                     * application/x-www-form-urlencoded shape) — both
                     * the author's selected= truth and any tap
                     * accumulation above. The seen[] dedupe below is
                     * bypassed: the same name is SUPPOSED to repeat. */
                    for (int k = 0; k < b->optionCount; k++)
                    {
                        DocOption *opt = b->options[k];
                        if (!opt || opt->disabled || opt->group ||
                            !opt->selected)
                        {
                            continue;
                        }
                        const char *mname = it->name ? it->name : "";
                        const char *mval =
                            (opt->value && opt->value[0]) ? opt->value
                                                          : (opt->text
                                                                 ? opt->text
                                                                 : "");
                        if (mname[0])
                        {
                            char *ek = url_encode(mname);
                            char *ev = url_encode(mval ? mval : "");
                            if (pairCount > 0)
                            {
                                strbuf_append(&query, "&");
                            }
                            strbuf_appendf(&query, "%s=%s", ek ? ek : "",
                                           ev ? ev : "");
                            pluto_free(ek);
                            pluto_free(ev);
                            pairCount++;
                        }
                    }
                }
                else
                {
                    int si = b->selectedIndex;
                    if (si >= 1 && si <= b->optionCount)
                    {
                        DocOption *opt = b->options[si - 1];
                        if (opt && !opt->disabled && !opt->group)
                        {
                            name = it->name ? it->name : "";
                            value = (opt->value && opt->value[0])
                                        ? opt->value
                                        : (opt->text ? opt->text : "");
                            include = 1;
                        }
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
static float g_fpsEma = 0.0f;          /* EMA of instantaneous per-frame rate */
static unsigned int g_fpsLastMs = 0;   /* ms timestamp of the previous frame */
static int g_fpsValue = 0;             /* last computed frames-per-second */
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

/* ── Display FPS (50 default; 30 low-power; 0 = Uncapped/unrestricted rate) ─ */
/* Apply the displayFps setting to the OS display + the keyboard component's
 * key-repeat timing (which is frame-count based). Called once at boot and
 * again whenever settings are saved. */
static void apply_display_fps(void)
{
    int fps = storage_setting_int("displayFps");
    if (fps != 30 && fps != 0)
    {
        fps = 50; /* 30/50/Uncapped(0) valid; 50 is the default */
    }
    pd->display->setRefreshRate((float)fps);
    if (g_kb)
    {
        keyboardApi.setRefreshRate(g_kb, (float)fps);
    }
    if (fps == 0)
    {
        logger_log("DISPLAY: refresh rate uncapped (0 = unrestricted)");
    }
    else
    {
        logger_log("DISPLAY: refresh rate set to %d fps", fps);
    }
}

/* Engine selection lives in the router (jsbridge_set_engine); this helper
 * only names it for logs (0=muJS, 1=Duktape, 2=QuickJS, 3=XS, 4=XS-NR). */
static const char *engine_name(void)
{
    int e = storage_setting_int("jsEngine");
    return e == 1   ? "Duktape"
           : e == 2 ? "QuickJS"
           : e == 3 ? "XS (Moddable)"
           : e == 4 ? "XS (No Recursion)"
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
     * place. No page open -> nothing to reload.
     *
     * 2026-09-22 DEVICE CRASH FIX part 2 (user repro: flip the engine via
     * Settings on about:jsext): the synchronous navigate_to here ran the
     * WHOLE reload inside the settings-save call chain — the OS watchdog
     * logged "Run loop stalled for more than 10 seconds" (frame never
     * ended from the flip to the failing script compile 10s later) and
     * "stack overflow in task gameTask": parse + engine attach + script
     * compiles all nested under the menu-event stack blew the 64KB game
     * task stack. Direct boot never nests like this — its load is chunked
     * across frames at frame-loop stack depth. So DEFER the reload to the
     * frame loop via the pendingNavUrl mechanism (same as go_home and every
     * other menu action); pendingFlipUnload makes the frame loop unload the
     * old page first, preserving the part-1 memory fix. */
    if (currentUrlObj && currentUrlObj->normalized[0] &&
        strcmp(currentUrlObj->normalized, "about:home") != 0)
    {
        pendingFlipUnload = 1;
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                 currentUrlObj->normalized);
        logger_log("[settings] reload deferred to frame loop (flip unload armed)");
    }
}

#if defined(PLUTO_ENGINE_FLIP_TEST)
/* TEMPORARY (device verification of the 2026-09-22 crash fix): replays the
 * user's exact repro autonomously — about:jsext loads on the boot-forced
 * engine, then the engine is flipped through the REAL settings path
 * (storage_set_setting_int + storage_save + settings_on_change), the same
 * code the Settings panel's save button runs, with ~30s settle per engine
 * so each suite run completes before the next flip. Build:
 *   make device DEVICEDEFS="-DPLUTO_JSEXT_AUTOTEST
 *                           -DPLUTO_JSEXT_AUTOTEST_ENGINE=0
 *                           -DPLUTO_ENGINE_FLIP_TEST"
 * Expected log: three full [jsext-test] summaries (engine 0 run + a fresh
 * run per flip), [settings] old-page unload lines before each flip, and NO
 * refusals cascade / error-page / stack-overflow lines. */
static void engine_flip_test_tick(void)
{
    static int phase = 0;          /* 0=arming, 1..3 = waiting to flip */
    static unsigned frames = 0;
    static const int kFlipTo[3] = {1, 2, 3}; /* Duktape, QuickJS, XS */
    if (phase > 3)
    {
        return;
    }
    if (phase == 0)
    {
        phase = 1; /* initial nav was queued by the JSEXT_AUTOTEST block */
        frames = 0;
        return;
    }
    if (currentState != STATE_PAGE || isRendering)
    {
        frames++;
        if (frames % 300 == 0)
        {
            logger_log("[flip-test] stalled: state=%d rendering=%d phase=%d",
                       (int)currentState, (int)isRendering, phase);
        }
        return; /* settle only counts while a page is actually up */
    }
    frames++;
    if (frames % 150 == 0)
    {
        /* Memory trace during the new engine's growth: the resync should
         * keep live ~= real usage; a climb into the budget here would be a
         * genuine wall, not a phantom. */
        logger_log("[flip-test] phase=%d settle t=%ds live=%luKB refusals=%lu",
                   phase, frames / 30, pluto_mem_live() / 1024,
                   pluto_mem_refusals());
    }
    if (frames < 30u * 30u)
    {
        return;
    }
    frames = 0;
    int eng = kFlipTo[phase - 1];
    logger_log("[flip-test] settings-path flip to engine %d (live=%luKB)",
               eng, pluto_mem_live() / 1024);
    storage_set_setting_int("jsEngine", eng);
    storage_save();
    settings_on_change(); /* the exact user path, minus the UI */
    phase++;
    if (phase > 3)
    {
        logger_log("[flip-test] all flips issued; final run in flight");
    }
}
#endif




/* R30j MERGE (user request, 2026-10-06): ONE painter for the render-progress
 * screen. The eval overlay (xs_render_overlay) repaints the same bar onto the
 * same framebuffer, and STATE_PAGE's own doc draw used to paint the walker's
 * 1-block placeholder doc between overlay frames — the device showed TWO
 * competing "render pages" (placeholder text + bar, flickering percents).
 * This helper is the single source of the rendering UI: STATE_LOADING's
 * isRendering branch and STATE_PAGE's eval-pending gate both draw it, so the
 * placeholder doc NEVER paints while the engine is mid-eval. */
static void draw_render_progress_ui(void)
{
    LCDFont *fontH = style_font(PLUTO_FONT_HEADING1);
    LCDFont *fontB = style_font(PLUTO_FONT_BODY);
    LCDColor white = kColorWhite;
    LCDColor black = kColorBlack;
    pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT, white);
    pd->graphics->setFont(fontH);
    pd->graphics->drawText("Rendering Web Page...",
                           strlen("Rendering Web Page..."), kUTF8Encoding,
                           24, 60);
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
    float p = tasks_get_progress();
    if (p > 1.0f)
    {
        p = 1.0f;
    }
    int pct = (int)(p * 100.0f);
    char line[96];
    snprintf(line, sizeof(line), "Rendering page content... %d%%", pct);
    pd->graphics->drawText(line, strlen(line), kUTF8Encoding, 24, 116);
    pd->graphics->drawRect(24, 138, 352, 10, black);
    pd->graphics->fillRect(24, 138, (int)(352.0f * p), 10, black);
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
        /* Settings engine flip: fully unload the OLD page before the reload
         * so the new engine starts with the whole budget (part-1 fix), now
         * at frame-loop stack depth (part-2 fix). */
        if (pendingFlipUnload)
        {
            pendingFlipUnload = 0;
            tasks_cancel_all();
            jsext_abort_active();
            isRendering = 0;
            layout_clear();
            if (currentDoc)
            {
                if (currentDoc->_jsbridge)
                {
                    js_doc_close(currentDoc->_jsbridge);
                }
                logger_log("[mem] after engine close:");
                pluto_mem_dump_live(logger_log);
                document_free(currentDoc);
                free(currentDoc);
                currentDoc = NULL;
            }
            g_pageJs = NULL;
            pluto_mem_resync_live(); /* page boundary: counter = tracked truth
                                      * (drift made the SW3a gate refuse the
                                      * fresh engine's allocations) */
            logger_log("[settings] old page fully unloaded before reload "
                       "(live=%luKB, counter resynced)",
                       pluto_mem_live() / 1024);
            logger_log("[mem] after full unload:");
            pluto_mem_dump_live(logger_log);
        }
        navigate_to(dest);
    }

#if defined(PLUTO_JS_CLICK_AUTOTEST)
    js_click_autotest_tick();
#endif

#if defined(PLUTO_FORMCLICK_AUTOTEST)
    formclick_autotest_tick();
#endif
#if defined(PLUTO_FORMVAL_AUTOTEST)
    formval_autotest_tick();
#endif
#if defined(PLUTO_FORMENTER_AUTOTEST)
    forment_autotest_tick();
#endif

#if defined(PLUTO_JS_TIMERS_AUTOTEST)
    js_timers_autotest_tick();
#endif

#if defined(PLUTO_SNAP_AUTOTEST)
    snap_autotest_tick();
#endif

#if defined(PLUTO_NR_AUTOTEST) && !defined(PLUTO_NR_AUTOTEST_TAP)
    nr_revisit_autotest_tick();
#endif

#if defined(PLUTO_NR_AUTOTEST_TAP)
    nr_autotest_tap_tick();
#endif

#if defined(PLUTO_EV_AUTOTEST) && defined(PLUTO_NR_AUTOTEST)
    ev_autotest_tick();
#endif

#if defined(PLUTO_PAGE_AUTOTEST)
    page_autotest_tick();
#endif

#if defined(PLUTO_REWALK_OOM_AUTOTEST)
    rewalk_oom_autotest_tick();
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

#if defined(PLUTO_ENGINE_FLIP_TEST)
    engine_flip_test_tick();
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

    /* ── WiFi poll: refresh the status-icon truth every 1s and force a
     * chrome redraw when the visible level changes (user fix: the icon
     * used to stick on "disconnected" on pages that never fire a
     * network probe). Runs in EVERY state, including SETTINGS. ── */
    netmon_poll();

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

        /* R30j MERGE: while the engine's eval is still running, the walker's
         * doc is the PRE-JS placeholder ("(Empty Web Page)") — painting it
         * here is the second competing "render page" the device showed
         * (flickering bar, percent jumps). Draw the SAME progress UI the
         * overlay paints instead; the real doc takes over at the post-eval
         * rewalk. */
        if (g_pageJs && jsbridge_eval_pending(g_pageJs))
        {
            draw_render_progress_ui();
        }
        else
        {
            layout_draw(scrollY);
            layout_evict_offscreen(scrollY);
            {
                const LMLink *sel = lm_get_selected_link();
                hud_draw(scrollY, layout_get_total_height(),
                         sel ? sel->href : NULL);
            }
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
            /* R30j MERGE: single shared painter (same pixels as before —
             * heading/URL above are redrawn idempotently by the helper). */
            draw_render_progress_ui();
            /* R30l: while a big external script downloads, replace the
             * (slow-moving) parse percent with the REAL byte counter —
             * device run 14: the 502716-byte bundle never moved any
             * visible number, so the user could not trust the download.
             * Same pixels as the percent line (overdraw after a white
             * fill of its band; the bar itself stays shared).
             * NOTE: bryanwandrych.com serves chunked (no Content-Length)
             * — http_client reports total=0 — so the gate is on CUR and
             * the percent only renders when a total is actually known. */
            if (progressNetTotal > 0 || progressNetCur > 0)
            {
                int npct = progressNetTotal > 0
                               ? (int)((float)progressNetCur /
                                       (float)progressNetTotal * 100.0f)
                               : -1;
                if (npct > 100)
                {
                    npct = 100;
                }
                char nline[96];
                if (npct >= 0)
                {
                    snprintf(nline, sizeof(nline),
                             "Fetching scripts: %d / %d bytes (%d%%)",
                             progressNetCur, progressNetTotal, npct);
                }
                else
                {
                    snprintf(nline, sizeof(nline),
                             "Fetching scripts: %d bytes", progressNetCur);
                }
                pd->graphics->fillRect(24, 112, 352, 22, white);
                pd->graphics->drawText(nline, strlen(nline), kUTF8Encoding,
                                       24, 116);
            }
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



    /* Log a periodic heartbeat every 300 frames so logs show liveness. */
    if (frameCount % 300 == 0)
    {
        logger_log("updateFrame: heartbeat frame=%u fps=%d overlay=%d stackPeak=%uB/61800B heap=%luKB peak=%luKB bigAlloc=%luKB refusals=%lu",
                   frameCount, g_fpsValue, g_showFps, logger_stack_peak(),
                   pluto_mem_live() / 1024, pluto_mem_peak() / 1024,
                   pluto_mem_peak_alloc() / 1024, pluto_mem_refusals());
    }



    /* FPS sampling: exponential moving average of the instantaneous
     * per-frame rate (1000 / frame-delta-ms, alpha 0.1 ≈ last ~10 frames).
     * Unlike a fixed time window — which flickers ±2fps from single-frame
     * quantization — the EMA converges rock-solid when the OS tick locks
     * (30/50/uncapped) yet still tracks genuine dips within ~a second.
     * Zero-delta frames (sub-ms, uncapped hosts) carry no information and
     * are skipped rather than counted as infinite rate. */
    {
        unsigned int now = pd->system->getCurrentTimeMilliseconds();
        if (g_fpsLastMs == 0)
        {
            g_fpsLastMs = now;
        }
        else
        {
            unsigned int dt = now - g_fpsLastMs;
            g_fpsLastMs = now;
            if (dt > 0)
            {
                float inst = 1000.0f / (float)dt;
                if (g_fpsEma <= 0.0f)
                {
                    g_fpsEma = inst; /* snap on first real sample */
                }
                else
                {
                    g_fpsEma += (inst - g_fpsEma) * 0.1f;
                }
                g_fpsValue = (int)(g_fpsEma + 0.5f);
            }
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

        /* Forms-Phase 4: the form API host hooks — reset/submit/
         * requestSubmit/click roads run through main.c's wire. */
        jsbridge_set_form_hosts(pluto_host_form_reset, pluto_host_form_wire);

        pdtimer_init(pd);
        http_client_init(pd);
        logger_log("[netmon] boot: wifi %s (signal %d/3)",
                   netmon_state_name(), netmon_level());
        pluto_spill_init(); /* SW2b: disk-backed streaming storage dir */
        /* SW3/SW3a: enable the soft heap budget — the guarded-RAM raise.
         * Everything on the device allocates through the SW1 funnel, so this
         * one gate turns "allocate until the OS panics" into "refuse, log,
         * fall back to disk".
         * R30l (device run 16 hard numbers): the seg 407 mount (React's
         * client render — the single biggest allocation of the whole eval)
         * parked at live=4368KB headroom=2287KB, then burst 2/3/4 climbed
         * live 5904→6107→6327KB and died "memory full" at headroom=328KB —
         * React needs ~2.6MB from burst 1 and the 6.5MB envelope is ~600KB
         * short.
         * R30m REVERT (device run 17): the first and ONLY run with a 7.0MB
         * boot budget HARD-CRASHED the device (unrecoverable until a manual
         * reset) — no pluto.log, no errorlog.txt. The only delta vs runs
         * 12–16 was this +512KB of funnel allowance; a real OS-pool malloc
         * failure somewhere outside the XS contained-abort path panics
         * PlaydateOS. 6.5MB was the envelope for the pre-R42 evals (5.45MB
         * proven peak). Device run 32 (R42: the seg-105 free-run fixed)
         * measured the REAL mount trajectory: the React hydration peak
         * needs ~7MB+ on 32-bit (sim needs 10.9MB host-64; device died at
         * the 6.5MB wall with 6.7MB genuinely free per the crashlog's
         * heap-allocated watermark). Raise to 7.5MB — still leaves ~0.5MB
         * of funnel guard, and the mount-finish compact collect reclaims
         * the peak to ~2MB (sim-proven) once hydration completes.
         * DEVICE-ONLY: the simulator's app-resident watermark is ~45MB of
         * host-side allocations — enforcing a device-scale budget there would
         * refuse every large allocation and turn Duktape OOM fatal. The sim
         * stays pure telemetry (budget 0); its suites pin behavior instead. */
#ifdef TARGET_PLAYDATE
        /* R53: 8.0MB funnel budget (was 7.5MB). Device runs 48–50 evidence:
         * the R52 crash fix works (no E0, three full-render bypass cycles
         * PASSed), but every navigation leaves ~0.3–1MB of residue outside
         * the JS machine (bundler-start live climbed 1623→2115→2752→3054→
         * 4038KB across run 50's cycles), so at the old 7.5MB budget the
         * seg-407 React mount's 1.5MB slot-heap grow was refused from cycle
         * 2 on ("R32 LADDER … accepted=0" → meter overrun → contained
         * abort + engine reset, page never rendered). Run 48's crashlog
         * measured 6.7MB GENUINELY free at the old envelope, so the real
         * OS ceiling is far above 8MB; the old 7.0MB hard-crash (R30m) was
         * a different era (pre-R42 evals, higher engine peaks). 8.0MB
         * covers 5+ full-eval cycles at the measured residue rate while
         * keeping ~0.5MB of funnel guard under the observed envelope. */
        pluto_mem_set_budget(8 * 1024UL * 1024UL);
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


        /* R58b: boot auto-nav REMOVED (user request): production boots to
         * the HOME launcher — the browser never navigates anywhere on its
         * own. Test builds that need a launch-into-a-URL bring their own
         * pending-nav handoff (the autotest defines' block below). To
         * restore launch-into-the-site later: pendingNavUrlSet = 1 +
         * snprintf(pendingNavUrl, ..., "<url>") right here. */

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
    #ifdef PLUTO_JS_AUTOTEST_XS_NR
        /* Same, on the XS (NO RECURSION) fork engine (storage jsEngine=4,
         * live router=4). close[XS-NR] in the log proves which binaries
         * ran; the fork's heap-bounded walkers must survive the same page
         * that crashes the stock engines' native C-stack recursion. */
        storage_set_setting_int("jsEngine", 4);
        jsbridge_set_engine(4);
        logger_log("[js-autotest] engine forced: XS (No Recursion)");
    #endif
#endif /* TARGET_SIMULATOR guard above */

#if defined(PLUTO_IMG_AUTOTEST) && 0
        /* TEMPORARY test builds: auto-navigate to the about page. */
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s",
                 "https://bryanwandrych.com/about");
        logger_log("[img-autotest] navigating to about page");
#endif

#if defined(PLUTO_NR_AUTOTEST)
#ifndef PLUTO_NR_AUTOTEST_URL
#if defined(PLUTO_NR_AUTOTEST_TAP)
/* The tap autotest needs a stamped form-submit block: the contact page's
 * <form class="contact" noValidate><button type="submit">Send email</button>
 * is the hydrating in-place-submit target (react onSubmit + B5 branch). */
#define PLUTO_NR_AUTOTEST_URL "https://bryanwandrych.com/contact"
#else
#define PLUTO_NR_AUTOTEST_URL "https://bryanwandrych.com"
#endif
#endif
/* Optional offline repro: navigate to a local file instead of the URL
 * (test builds only; used when the device radio is down but the crash
 * must be iterated on). */
#ifdef PLUTO_NR_AUTOTEST_FILE_URL
#undef PLUTO_NR_AUTOTEST_URL
#define PLUTO_NR_AUTOTEST_URL PLUTO_NR_AUTOTEST_FILE_URL
#endif
/* Optional engine force: 0=muJS 1=Duktape 2=QuickJS 4=XS NR (default).
 * Mirrors PLUTO_NAV_AUTOTEST_ENGINE — lets one seam build drive the real
 * CSR page under every engine for like-for-like comparison. */
#ifndef PLUTO_NR_AUTOTEST_ENGINE
#define PLUTO_NR_AUTOTEST_ENGINE 4
#endif
/* R47 soak: consecutive full render+revisit cycles after boot (the edge
 * that used to die on the SECOND navigation). 0 = single cycle (default). */
#ifndef PLUTO_NR_SOAK
#define PLUTO_NR_SOAK 0
#endif
/* R47b: 1 = every soak cycle bypasses the snapshot (full fetch + full
 * eval + mount + compact each time). Requires PLUTO_NR_SOAK > 0. */
#ifndef PLUTO_NR_SOAK_BYPASS
#define PLUTO_NR_SOAK_BYPASS 0
#endif
        /* TEMPORARY (PLUTO_NR_AUTOTEST builds, sim + device): navigate
         * straight to the deep-CSR test page (bryanwandrych.com) with the
         * XS (No Recursion) engine active. This is the end-to-end crash
         * reproduction: the page's bundle depth overflows every stock
         * engine's native recursion; the fork must parse, bind, emit and
         * run it entirely from heap frames. */
        /* Full JS execution is REQUIRED for the CSR page (user rule for
         * this sweep): force jsEnabled=2 unconditionally, not just when Off. */
        if (storage_setting_int("jsEnabled") != 2)
            logger_log("[nr-autotest] jsEnabled was %d, forced to 2 (Full)", storage_setting_int("jsEnabled"));
        storage_set_setting_int("jsEnabled", 2); /* Full */
        storage_set_setting_int("jsEngine", PLUTO_NR_AUTOTEST_ENGINE);
        jsbridge_set_engine(PLUTO_NR_AUTOTEST_ENGINE);
        /* Snapshots persist across sessions (by design), so a bryanwandrych
         * page snap from any previous run would otherwise fast-path this
         * boot navigation and the watchdog test would never touch the
         * engine. Force the classic fetch+eval path for THIS navigation. */
        snapBypass = 1;
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", PLUTO_NR_AUTOTEST_URL);
        logger_log("[nr-autotest] navigating to %s on engine %d", PLUTO_NR_AUTOTEST_URL, (int)PLUTO_NR_AUTOTEST_ENGINE);
#if PLUTO_NR_SOAK > 0
        g_nrSoakTotal = PLUTO_NR_SOAK;
        logger_log("[nr-autotest] SOAK enabled: %d consecutive render+revisit cycles", g_nrSoakTotal);
#endif
#if PLUTO_NR_SOAK > 0 && PLUTO_NR_SOAK_BYPASS
        g_nrSoakBypass = 1;
        logger_log("[nr-autotest] SOAK BYPASS: every cycle full-fetches + full-evals (no snapshot)");
#endif
#if !defined(TARGET_PLAYDATE)
        /* MEMWATCH scale note: the sim eval allocates at host-64 scale
         * (~86MB peak on this page) — the DEVICE envelope is 6.5MB funnel
         * inside the ~7.5MB app pool with a 61.8KB stack, and that envelope
         * is enforced (sim-only) at the revisit via [memwatch] ARM. */
        logger_log("[memwatch] sim boot: heap scale is HOST-64 (eval peak "
                   "~86MB here; device envelope 6.5MB enforced at revisit)");
#endif
#ifdef PLUTO_QJS_PROBE_SPAN
        /* R26g device bisect: compile the baked-in span bytes from boot
         * depth in a fresh runtime BEFORE any navigation. */
        {
            extern void qjs_probe_span_compile(void);
            qjs_probe_span_compile();
            logger_log("[qjsprobe] complete");
        }
#endif
#endif

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

#if defined(PLUTO_REWALK_OOM_AUTOTEST)
        /* TEMPORARY (sim autotest): land on the acidtest page with a live
         * DomResult so the seam tick has something real to re-walk. */
        storage_set_setting_int("jsEnabled", 1);
        storage_set_setting_int("mode", 1); /* RAW_HTML: live DOM */
        currentBrowseMode = MODE_RAW_HTML;
        snapBypass = 1; /* SW6 fast path serves no live DOM — full parse */
        pendingNavUrlSet = 1;
        snprintf(pendingNavUrl, sizeof(pendingNavUrl), "%s", "about:acidtest");
        logger_log("[rewalk-oom] boot: RAW_HTML + snap bypass, navigating "
                   "to acidtest");
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
        /* NO budget pin here (2026-09-22 lesson): the suite fixtures are
         * sized against the PRODUCT DEFAULT (JSBRIDGE_EXT_PAGE_BUDGET,
         * 512KB) — see the SW3 RE-TUNE note in jsext.c. A test-only pin
         * created two worlds: seam runs passed while a real navigation to
         * about:jsext under the default failed the over-budget refusal
         * test. Seam builds differ from product ONLY in auto-navigation. */
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

