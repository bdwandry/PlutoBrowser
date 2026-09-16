/* BF14 host-side harness (macOS, NOT part of the Playdate build).
 * Compiles the REAL Source/ui/home_page.c against faked storage/logger/style
 * APIs (same pattern as tests/p27_host_test.c) and drives the crank /
 * D-pad / A-button navigation without a graphics vtable (home_page_draw is
 * never called; home_page_update_scroll is driven directly).
 *
 * Usage: bf13_home_crank_host [bookmarkCount]   (default 10, exit 0 = PASS)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "core/storage.h"
#include "core/http_client.h"
#include "render/style.h"

/* ---- Fake Playdate API (p27_host_test.c pattern) ---- */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
static struct playdate_sys g_fakeSys;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;
PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeSys.realloc = host_realloc;
        g_fakeApi.system = &g_fakeSys;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}
void pluto_free(void *p) { free(p); }

/* ---- Fakes for home_page.c dependencies ---- */
static int g_bmCount = 10;
static int g_invertCrank = 0;
static int g_testCount = 5; /* Test Cases directory size (argv[2] = 0 disables) */

void storage_init(PlaydateAPI *pd) { (void)pd; }
int storage_bookmark_count(void) { return g_bmCount; }
const StoredBookmark *storage_bookmark_at(int index)
{
    static StoredBookmark bm;
    static char buf[64];
    if (index < 0 || index >= g_bmCount) return NULL;
    snprintf(buf, sizeof(buf), "BM %d", index + 1);
    bm.title = bm.url = bm.desc = buf;
    return &bm;
}
int storage_setting_int(const char *key)
{
    if (strcmp(key, "invertCrank") == 0) return g_invertCrank;
    return 0;
}
/* Fake internal-page directory (production returns the 5 about: pages). */
int http_test_pages(const HttpTestPage **entries)
{
    static HttpTestPage pages[5];
    static char names[5][32];
    static int init = 0;
    if (!init)
    {
        const char *n[5] = { "about:home", "about:blank", "about:acidtest",
                             "about:javascript", "about:jsext" };
        for (int i = 0; i < 5; i++)
        {
            snprintf(names[i], sizeof(names[i]), "%s", n[i]);
            pages[i].name = names[i];
            pages[i].title = names[i];
        }
        init = 1;
    }
    if (entries) *entries = pages;
    return g_testCount > 5 ? 5 : g_testCount;
}
void logger_init(PlaydateAPI *pd) { (void)pd; }
void logger_log(const char *fmt, ...) { (void)fmt; }
void logger_error_at(const char *f, int l, const char *fmt, ...) { (void)f; (void)l; (void)fmt; }
LCDFont *style_font(PlutoFontRole role) { (void)role; return NULL; }
int style_get_text_width(PlutoFontRole role, const char *text) { (void)role; (void)text; return 0; }
void style_init(PlaydateAPI *pd) { (void)pd; }

/* settings open: flip a flag instead of real navigation */
static int g_settingsOpened = 0;
static void open_settings(void) { g_settingsOpened = 1; }

#include "ui/home_page.h"

/* Every crank magnitude below is expressed as a multiple of
 * HOME_CRANK_STEP_PX (45° — see home_page.h), so the tests stay in sync
 * if the production step is tuned again. */

/* crank total degrees in one call (sub-steps accumulate internally);
 * update_scroll then runs 60 simulated frames so the 0.3 easing + 0.5 snap
 * settle exactly like the device loop (draw() calls it every frame) */
static void crank(float deg)
{
    home_page_handle_crank(deg);
    for (int f = 0; f < 60; f++)
    {
        home_page_update_scroll();
    }
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                    \
    do                                                                       \
    {                                                                        \
        if (cond) { printf("PASS %s\n", name); g_pass++; }                   \
        else { printf("FAIL %s (sel=%d scroll=%d)\n", name,                  \
                      home_page_selected_index(), home_page_scroll_y());     \
               g_fail++; }                                                  \
    } while (0)

/* Legacy scroll geometry for reference:
 * card row pitch = 54px; last card (1-based i) abs top = 24+4+160+(row)*54,
 * row = (i-1)/2. update_scroll selects the LAST card at count (reading order). */
static int expected_max_scroll(int count)
{
    int tests = g_testCount > 5 ? 5 : g_testCount;
    int rows = (count + 1) / 2;
    int testRows = tests > 0 ? (tests + 1) / 2 : 0;
    /* BF19 layout fix mirrored: test section = +12 grid-bottom pad +24
     * header→cards gap (was a bare +54). Keep in sync with home_page.c. */
    int bottom = 24 + 4 + 172 + rows * 54 +
                 (tests > 0 ? 12 + 24 + testRows * 54 : 0) + 80 + 8; /* home_content_bottom */
    int max = bottom - 240;
    return max < 0 ? 0 : max;
}

/* The test-page fakes expose these names so A-press assertions can check
 * WHICH page a selection opens. */
#define BTN_RIGHT (1u << 1)
#define BTN_LEFT (1u << 0)
#define BTN_DOWN (1u << 3)
#define BTN_UP (1u << 2)
#define BTN_A (1u << 5)

int main(int argc, char **argv)
{
    if (argc > 1) g_bmCount = atoi(argv[1]);
    if (argc > 2) g_testCount = atoi(argv[2]);
    int count = g_bmCount;
    int tests = g_testCount > 5 ? 5 : g_testCount;
    printf("== BF14 home crank harness (count=%d tests=%d) ==\n", count, tests);

    home_page_reset();

    /* TC1: crank DOWN selects bookmarks one at a time (18 deg/step).
     * 180 degrees from Settings should land on bookmark 10. */
    crank((float)count * HOME_CRANK_STEP_PX);
    CHECK(home_page_selected_index() == count, "TC1 crank-down reaches last bookmark");

    /* TC2: scroll clamped at/under content bottom (no runaway). */
    crank(500.0f);
    CHECK(home_page_scroll_y() <= expected_max_scroll(count),
          "TC2 scroll clamped to content bottom");

    /* TC3: crank UP from the bottom returns exactly to Settings (0).
     * The overshoot must span Settings + every selectable card (bookmarks
     * + test cards) plus the sub-step remainder the previous gesture left
     * in g_crankFrac — the clamp at the top does the rest. */
    crank((float)-(count + tests + 8) * HOME_CRANK_STEP_PX); /* overshoot past the top */
    CHECK(home_page_selected_index() == 0, "TC3 crank-up returns to Settings");
    CHECK(home_page_scroll_y() == 0, "TC3b scroll back at top");

    /* TC4: down-then-up returns to the exact row you came from.
     * Fresh gesture (reset), down 3 bookmarks then up 3 -> Settings.
     * (Needs >= 3 bookmarks to be meaningful.) */
    if (count >= 3)
    {
        home_page_reset();
        crank(3.0f * HOME_CRANK_STEP_PX);
        int mid = home_page_selected_index();
        CHECK(mid == 3, "TC4a down 3 = bookmark 3");
        crank(-3.0f * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == 0, "TC4b up 3 = Settings again");
    }

    /* TC5: partial crank (under one full 45° step) must NOT move the
     * selection; 10° and 40° both stay sub-threshold. */
    crank(10.0f);
    CHECK(home_page_selected_index() == 0, "TC5 sub-threshold crank ignored");
    crank(-10.0f);
    CHECK(home_page_selected_index() == 0, "TC5b sub-threshold up ignored");
    crank(40.0f);
    CHECK(home_page_selected_index() == 0, "TC5c 40<45 still ignored");
    crank(-40.0f);
    CHECK(home_page_selected_index() == 0, "TC5d 40<45 up still ignored");

    /* TC6: settings reachable via crank even after deep free-scroll. */
    crank((float)count * HOME_CRANK_STEP_PX);      /* to last bookmark */
    crank(200.0f);             /* free-scroll into footer */
    crank((float)-(count + 8) * HOME_CRANK_STEP_PX); /* all the way back up */
    CHECK(home_page_selected_index() == 0, "TC6 Settings reachable after free-scroll");

    /* TC7: A on Settings opens settings callback. */
    home_page_reset();
    char *u = home_page_handle_input(BTN_A, open_settings);
    CHECK(g_settingsOpened == 1 && u == NULL, "TC7 A on Settings opens settings");
    if (u) pluto_free(u);

    /* TC8: odd count: last bookmark (bottom-left cell) selectable by crank. */
    if (count % 2 == 0)
    {
        g_bmCount = count + 1; /* make it odd */
        home_page_reset();
        crank((float)(count + 1) * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == count + 1,
              "TC8 odd count: bottom-left bookmark selectable");
        /* and scroll up from it reaches Settings */
        crank((float)-(count + 2) * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == 0, "TC8b odd count: back to Settings");
        g_bmCount = count;
        home_page_reset();
    }

    /* TC9: D-pad regression — DOWN from Settings -> 1; DOWN from even -> +2;
     * UP from 2 -> Settings (row wrap preserved). */
    home_page_reset();
    home_page_handle_input(BTN_DOWN, open_settings);
    CHECK(home_page_selected_index() == 1, "TC9a D-pad DOWN to bookmark 1");
    if (count >= 3)
    {
        home_page_handle_input(BTN_DOWN, open_settings);
        CHECK(home_page_selected_index() == 3, "TC9b D-pad DOWN wraps a row");
        home_page_handle_input(BTN_UP, open_settings);
        CHECK(home_page_selected_index() == 1, "TC9c D-pad UP wraps a row");
    }
    home_page_handle_input(BTN_UP, open_settings);
    CHECK(home_page_selected_index() == 0, "TC9d D-pad UP to Settings");

    /* TC10: button press ends the crank gesture (re-anchor, no jump). */
    if (count >= 6)
    {
        crank(2.0f * HOME_CRANK_STEP_PX); /* bookmark 2 via crank */
        home_page_handle_input(BTN_DOWN, open_settings); /* D-pad +2 -> 4 */
        crank(2.0f * HOME_CRANK_STEP_PX); /* new gesture from 4 -> 6 (no anchor carry-over) */
        CHECK(home_page_selected_index() == 6, "TC10 gesture ends on button press");
    }

    /* TC11: invertCrank flips direction. */
    home_page_reset();
    g_invertCrank = 1;
    crank(2.0f * HOME_CRANK_STEP_PX);
    CHECK(home_page_selected_index() == 0, "TC11 inverted crank-down stays at top");
    if (count >= 2)
    {
        crank(-2.0f * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == 2, "TC11b inverted crank-up selects down");
    }
    g_invertCrank = 0;

    /* ── Test Cases section (BF19): built-in about: pages as cards ─────── */
    if (tests > 0)
    {
        int last = 1 + count + tests - 1; /* last selectable index */

        /* TC12: D-pad DOWN walks Settings → bookmarks → test cards and
         * STOPS at the last test card (the footer is not selectable). */
        home_page_reset();
        home_page_handle_input(BTN_DOWN, open_settings); /* Settings -> bm 1 */
        if (count > 0)
        {
            int sel = 1;
            while (sel + 2 <= count)
            {
                home_page_handle_input(BTN_DOWN, open_settings);
                sel += 2;
            }
            if (sel < count)
            {
                home_page_handle_input(BTN_DOWN, open_settings);
                sel += 1;
            }
            CHECK(home_page_selected_index() == count,
                  "TC12a D-pad reaches last bookmark");
            home_page_handle_input(BTN_DOWN, open_settings);
            CHECK(home_page_selected_index() == count + 1,
                  "TC12b DOWN enters Test Cases");
        }
        else
        {
            /* No bookmarks: Settings -> DOWN lands on the first test card. */
            CHECK(home_page_selected_index() == count + 1,
                  "TC12e no bookmarks: DOWN lands on first test card");
        }
        /* Walk down through the test rows with a bounded number of presses
         * (DOWN clamps at the last card, so the loop always terminates). */
        int tsel = home_page_selected_index();
        for (int i = 0; i < tests + 2 && tsel < last; i++)
        {
            home_page_handle_input(BTN_DOWN, open_settings);
            tsel = home_page_selected_index();
        }
        home_page_handle_input(BTN_DOWN, open_settings);
        CHECK(home_page_selected_index() == last,
              "TC12c DOWN stops at last test card");
        home_page_handle_input(BTN_DOWN, open_settings);
        CHECK(home_page_selected_index() == last,
              "TC12d DOWN never selects the footer");

        /* TC13: A on a test card opens that card's about: URL. */
        home_page_reset();
        crank((float)(count + 1) * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == count + 1,
              "TC13a crank lands on first test card");
        u = home_page_handle_input(BTN_A, open_settings);
        CHECK(u != NULL && strcmp(u, "about:home") == 0,
              "TC13b A on test card 1 opens about:home");
        if (u) pluto_free(u);
        home_page_handle_input(BTN_RIGHT, open_settings);
        CHECK(home_page_selected_index() == count + 2,
              "TC13c RIGHT moves within the test row");
        u = home_page_handle_input(BTN_A, open_settings);
        CHECK(u != NULL && strcmp(u, "about:blank") == 0,
              "TC13d A on test card 2 opens about:blank");
        if (u) pluto_free(u);

        /* TC14: crank walks Settings + every bookmark + every test card. */
        home_page_reset();
        crank((float)(1 + count + tests) * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == last,
              "TC14 crank reaches the last test card");

        /* TC15: UP from the first test card returns to the bookmark grid
         * (the lone odd-row cell when the grid ends with one). */
        home_page_reset();
        crank((float)(count + 1) * HOME_CRANK_STEP_PX);
        home_page_handle_input(BTN_UP, open_settings);
        {
            /* Odd grid: the lone last-row bookmark is straight above the
             * first test card (-1). Even grid: the left-column card one row
             * higher (-2). */
            int expect = (count % 2 == 1) ? count : count - 1;
            if (expect < 0) expect = 0;
            CHECK(home_page_selected_index() == expect,
                  "TC15 UP from Test Cases re-enters the grid");
        }

        /* TC16: crank-up from the bottom crosses the seam back to Settings. */
        crank((float)-(count + tests + 4) * HOME_CRANK_STEP_PX);
        CHECK(home_page_selected_index() == 0,
              "TC16 crank-up returns to Settings");
    }

    printf("== %d PASS, %d FAIL ==\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
