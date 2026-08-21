// main.c — PlutoBrowser application skeleton (Phase P01–P04)
//
// C port of CometBrowser Source/main.lua. This phase establishes the event
// handler, the update loop, boot logging, and the P02–P04 self-test
// diagnostics; the full state machine, input handling, navigation, and
// rendering pipeline arrive in later phases (MASTER_TODO P32/P33).

#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "core/constants.h"
#include "core/cookie_jar.h"
#include "core/encoding.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "core/selftest_encoding.h"
#include "core/selftest_http.h"
#include "core/selftest_storage.h"
#include "core/selftest_tasks.h"
#include "core/selftest_url.h"
#include "core/storage.h"
#include "core/tasks.h"
#include "html/entities.h"
#include "util/mem.h"
#include "util/selftest_util.h"

static PlaydateAPI* pd = NULL;
static unsigned int s_frame = 0;
static LCDFont* s_fontBody = NULL;
static int s_selfPass = -1;
static int s_selfFail = -1;
static int s_urlPass = -1;
static int s_urlFail = -1;
static int s_stPass = -1;
static int s_stFail = -1;
static int s_tkPass = -1;
static int s_tkFail = -1;
static int s_enPass = -1;
static int s_enFail = -1;
static int s_hcPass = -1;
static int s_hcFail = -1;

// ── P07 benchmark (MASTER_TODO §4.10): fetch assets from the bitmaps host
// and log URL / bytes / download ms / KB per second. Log-only: failures are
// expected whenever the simulator has no network or the access prompt blocks.
static const char* BENCH_URLS[] = {
    "https://wiesmann.codiferes.net/share/bitmaps/",
    "http://wiesmann.codiferes.net/share/bitmaps/test_image_4c.png",
};
#define BENCH_NURLS (int)(sizeof(BENCH_URLS) / sizeof(BENCH_URLS[0]))
static int s_benchIdx = -1;
static int s_benchDone = 0;
static unsigned s_benchStartMs = 0;

static void bench_start_current(void);

static void bench_on_success(void* ud, int status, const StrMap* headers,
                             const char* body, size_t bodyLen,
                             const char* url)
{
    (void)ud;
    (void)headers;
    unsigned ms = pd->system->getCurrentTimeMilliseconds() - s_benchStartMs;
    PLUTO_LOG("[P07] bench OK %s status=%d bytes=%u ms=%u kbps=%u", url,
              status, (unsigned)bodyLen, ms,
              ms > 0 ? (unsigned)((bodyLen * 1000u) / (ms * 1024u)) : 0);
    s_benchIdx++;
    bench_start_current();
}

static void bench_on_error(void* ud, const char* msg)
{
    (void)ud;
    PLUTO_LOG("[P07] bench FAIL %s (%s)", BENCH_URLS[s_benchIdx],
              msg ? msg : "?");
    s_benchIdx++;
    bench_start_current();
}

static void bench_start_current(void)
{
    if (s_benchIdx >= BENCH_NURLS) {
        if (!s_benchDone) {
            PLUTO_LOG("[P07] benchmark done");
            s_benchDone = 1;
        }
        return;
    }
    PlutoHttpCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.onSuccess = bench_on_success;
    cbs.onError = bench_on_error;
    s_benchStartMs = pd->system->getCurrentTimeMilliseconds();
    PLUTO_LOG("[P07] bench start %s", BENCH_URLS[s_benchIdx]);
    // Return value intentionally ignored: synchronous rejections still fire
    // onError (which advances the chain).
    hc_get(BENCH_URLS[s_benchIdx], &cbs);
}

static void draw_placeholder(void)
{
    // Temporary P01/P02 placeholder screen: proves the loop runs, fonts load,
    // and shows the util self-test summary.
    pd->graphics->clear(kColorWhite);

    if (s_fontBody != NULL) {
        pd->graphics->setFont(s_fontBody);
    }
    // C API text drawing: black via kDrawModeCopy (default); white text later
    // via setDrawMode(kDrawModeFillWhite), mirroring Lua setImageDrawMode.

    const char* title = "PlutoBrowser";
    pd->graphics->drawText(title, strlen(title), kASCIIEncoding, 150, 80);

    char buf[96];
    int n = snprintf(buf, sizeof(buf), "C port boot OK - frame %u", s_frame);
    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
        }
        pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 120, 110);
    }

    if (s_selfPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P02 util selftests: %d passed, %d failed",
                     s_selfPass, s_selfFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 140);
        }
    }

    if (s_urlPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P03 url selftests: %d passed, %d failed",
                     s_urlPass, s_urlFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 155);
        }
    }

    if (s_stPass >= 0) {
        n = snprintf(buf, sizeof(buf),
                     "P04 storage/cookies: %d passed, %d failed",
                     s_stPass, s_stFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 170);
        }
    }

    if (s_tkPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P05 tasks: %d passed, %d failed",
                     s_tkPass, s_tkFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 185);
        }
    }

    const char* hint = "Phase P07 raw TCP HTTP client";
    pd->graphics->drawText(hint, strlen(hint), kASCIIEncoding, 100, 210);
}

static int update(void* userdata)
{
    (void)userdata;
    s_frame++;

    // Cooperative scheduler pump (main.lua Tasks.update() parity).
    tasks_update();

    // HTTP client pump (main.lua HttpClient.update() parity) + P07 benchmark
    // kick-off once the boot self-tests are long done.
    hc_update();
    if (s_frame == 120 && !s_benchDone && s_benchIdx < 0) {
        s_benchIdx = 0;
        bench_start_current();
    }

    if (s_frame == 1) {
        PLUTO_LOG("first frame rendered");
    } else if (s_frame % 300 == 0) {
        PLUTO_LOG("heartbeat frame=%u", s_frame);
    }

    draw_placeholder();
    return 1;
}

int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    switch (event) {
        case kEventInit:
            pd = playdate;

            // Boot logging first so any later failure is captured.
            logger_init(pd);
            mem_init(pd);
            PLUTO_LOG("PlutoBrowser boot (C port of CometBrowser)");
            PLUTO_LOG("constants: screen=%dx%d chrome=%d contentY=%d contentH=%d",
                      PLUTO_SCREEN_WIDTH, PLUTO_SCREEN_HEIGHT,
                      PLUTO_CHROME_HEIGHT, PLUTO_CONTENT_Y, PLUTO_CONTENT_HEIGHT);

            // P02 util-layer self-tests (permanent boot diagnostics).
            selftest_util_run(&s_selfPass, &s_selfFail);
            if (s_selfFail > 0) {
                PLUTO_ERROR("P02 SELFTEST FAILURES: %d", s_selfFail);
            }

            // P03 core/url self-tests (permanent boot diagnostics).
            selftest_url_run(&s_urlPass, &s_urlFail);
            if (s_urlFail > 0) {
                PLUTO_ERROR("P03 SELFTEST FAILURES: %d", s_urlFail);
            }

            // P04 storage + cookie jar: init real persistence, then tests.
            storage_init(pd);
            cj_init(pd);
            selftest_storage_run(&s_stPass, &s_stFail);
            if (s_stFail > 0) {
                PLUTO_ERROR("P04 SELFTEST FAILURES: %d", s_stFail);
            }

            // P05 cooperative task scheduler self-tests (fake clock).
            tasks_init(pd);
            selftest_tasks_run(&s_tkPass, &s_tkFail);
            if (s_tkFail > 0) {
                PLUTO_ERROR("P05 SELFTEST FAILURES: %d", s_tkFail);
            }

            // P06 charset conversion + HTML entities self-tests.
            encoding_init(pd);
            entities_init(pd);
            selftest_encoding_run(&s_enPass, &s_enFail);
            if (s_enFail > 0) {
                PLUTO_ERROR("P06 SELFTEST FAILURES: %d", s_enFail);
            }

            // P07 raw TCP HTTP client self-tests (fake TCP vtable + fake
            // clock; real networking restored afterwards for the benchmark).
            cj_init(pd); // idempotent: reuses the P04 storage backend
            hc_init(pd);
            selftest_http_run(&s_hcPass, &s_hcFail);
            if (s_hcFail > 0) {
                PLUTO_ERROR("P07 SELFTEST FAILURES: %d", s_hcFail);
            }

            // Lua main.lua did not call setRefreshRate -> keep SDK default.

            const char* fontErr = NULL;
            s_fontBody = pd->graphics->loadFont("fonts/Roobert-11-Medium", &fontErr);
            if (s_fontBody == NULL) {
                PLUTO_ERROR("loadFont Roobert-11-Medium failed: %s",
                            fontErr ? fontErr : "unknown");
            } else {
                PLUTO_LOG("font loaded: fonts/Roobert-11-Medium");
            }

            pd->system->setUpdateCallback(update, NULL);
            PLUTO_LOG("update callback registered");
            break;

        case kEventTerminate:
            PLUTO_LOG("terminate event, frames=%u", s_frame);
            break;

        default:
            break;
    }

    return 0;
}
