// main.c — PlutoBrowser application skeleton (Phase P01–P04)
//
// C port of CometBrowser Source/main.lua. This phase establishes the event
// handler, the update loop, boot logging, and the P02–P04 self-test
// diagnostics; the full state machine, input handling, navigation, and
// rendering pipeline arrive in later phases (MASTER_TODO P32/P33).

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pd_api.h"

#include "core/constants.h"
#include "core/cookie_jar.h"
#include "core/encoding.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "core/selftest_encoding.h"
#include "core/selftest_http.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "html/selftest_tokenizer.h"
#include "html/selftest_dom.h"
#include "html/selftest_document.h"
#include "html/selftest_readability.h"
#include "render/style.h"
#include "render/selftest_style.h"
#include "render/link_manager.h"
#include "render/selftest_link_manager.h"
#include "render/decoders/selftest_decoders.h"
#include "render/decoders/selftest_inflate.h"
#include "render/decoders/png.h"
#include "render/decoders/selftest_png.h"
#include "render/decoders/selftest_png_fixtures.h"
#include "render/decoders/bmp.h"
#include "render/decoders/selftest_bmp.h"
#include "render/decoders/selftest_bmp_fixtures.h"
#include "render/decoders/gif.h"
#include "render/decoders/selftest_gif.h"
#include "render/decoders/selftest_jpeg.h"
#include "render/decoders/jpeg.h"
#include "render/decoders/selftest_jpeg_fixtures.h"
#include "render/decoders/webp.h"
#include "render/decoders/selftest_webp.h"
#include "render/decoders/selftest_webp_fixtures.h"
#include "render/decoders/ico.h"
#include "render/decoders/selftest_ico.h"
#include "render/decoders/selftest_ico_fixtures.h"
#include "render/decoders/svg.h"
#include "render/decoders/selftest_svg.h"
#include "render/decoders/selftest_svg_fixtures.h"
#include "render/image_decoder.h"
#include "render/selftest_image_decoder.h"
#include "render/cloud_layout.h"
#include "render/selftest_cloud_layout.h"
#include "ui/chrome.h"
#include "ui/hud.h"
#include "ui/selftest_ui.h"
#include "ui/home_page.h"
#include "ui/selftest_home_page.h"
#include "ui/address_bar.h"
#include "ui/selftest_address_bar.h"
#include "ui/error_page.h"
#include "ui/bookmarks_page.h"
#include "ui/history_page.h"
#include "ui/selftest_pages.h"
#include "render/decoders/selftest_gif_fixtures.h"
#include "html/document.h"
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
static int s_ttPass = -1;
static int s_ttFail = -1;
static int s_dmPass = -1;
static int s_dmFail = -1;
static int s_dcPass = -1;
static int s_rzFail = -1;
static int s_rzPass = -1;
static int s_syFail = -1;
static int s_syPass = -1;
static int s_lmFail = -1;
static int s_lmPass = -1;
static int s_dc15Fail = -1;
static int s_dc15Pass = -1;
static int s_inFail = -1;
static int s_inPass = -1;
static int s_pngFail = -1;
static int s_pngPass = -1;
static int s_bmFail = -1;
static int s_bmPass = -1;
static int s_giFail = -1;
static int s_giPass = -1;
static int s_jpFail = -1;
static int s_jpPass = -1;
static int s_wpFail = -1;
static int s_wpPass = -1;
static int s_icPass = -1;
static int s_icFail = -1;
static int s_svPass = -1;
static int s_svFail = -1;
static int s_idPass = -1;
static int s_idFail = -1;
/* P25 end-to-end: sequential image downloads from a localhost server */
#define P25_BASE "http://127.0.0.1:8765"
static const char* kP25Png = P25_BASE "/t.png";
static const char* kP25Bmp = P25_BASE "/t.bmp";
static const char* kP25Bad = P25_BASE "/nope.png";
static int s_p25Stage = 0;          /* 0 idle, 1 downloading, 2 done */
static unsigned s_p25T0Ms = 0;
static unsigned s_p25WaitFrames = 0;
static int s_p25Resolved = -1;      /* HUD: -1 pending, 0 fail, 1 ok */
/* P26 cloud layout visual: parse/build a fixture once, then sweep scroll */
static int s_clPass = -1;
static int s_clFail = -1;
static int s_clBuilt = 0;
static int s_clScroll = 0;
/* P27 chrome/hud visual: rotate through showcase scenarios */
static int s_uiPass = -1;
static int s_uiFail = -1;
static int s_p27Scenario = 0;
/* P28 home page visual + scripted input walk */
static int s_hpPass = -1;
static int s_hpFail = -1;
/* P29 address bar: selftest counts + armed-window demo state */
static int s_abPass = -1;
static int s_abFail = -1;
/* P30 pages: selftest counts + showcase rotation */
static int s_p30Pass = -1;
static int s_p30Fail = -1;
static int s_p30Slot = 0;

// Vendored keyboard lib (vendor/keyboard) resolves the SDK handle through
// this global symbol; every PlutoBrowser module keeps its own s_pd static.
PlaydateAPI* playdate = NULL;
/* Fixture mirroring the cloud payload shape served on MODE_OPERA_DS. */
static const char* kClFixture =
    "{\"title\":\"Cloud Demo\",\"totalHeight\":500,\"elements\":["
    "{\"type\":\"text\",\"x\":8,\"y\":4,\"w\":384,\"h\":20,"
    "\"text\":\"Hello Cloud\",\"font\":\"large\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":30,\"w\":200,\"h\":14,"
    "\"text\":\"bold line\",\"font\":\"bold\"},"
    "{\"type\":\"text\",\"x\":8,\"y\":50,\"w\":200,\"h\":14,"
    "\"text\":\"code line\",\"font\":\"mono\"},"
    "{\"type\":\"image\",\"x\":8,\"y\":90,\"w\":64,\"h\":64,"
    "\"src\":\"img/logo.png\"},"
    "{\"type\":\"link\",\"x\":8,\"y\":160,\"w\":100,\"h\":14,"
    "\"href\":\"https://example.com/a\"},"
    "{\"type\":\"input\",\"x\":8,\"y\":180,\"w\":180,\"h\":22,"
    "\"inputType\":\"email\",\"name\":\"email\","
    "\"placeholder\":\"you@example.com\"},"
    "{\"type\":\"submit\",\"x\":196,\"y\":180,\"w\":60,\"h\":22,"
    "\"label\":\"Go\",\"formAction\":\"/search\"}"
    "]}";
static void* s_webpView = NULL;  /* temporary P21 debug viewer */
static void* s_jpegView = NULL;  /* temporary P20 debug viewer */
static void* s_icoView = NULL;   /* temporary P23 debug viewer */
static void* s_svgView = NULL;   /* temporary P24 debug viewer */
static void* s_gifView = NULL;   /* temporary P19 debug viewer */
static void* s_pngView = NULL;   /* temporary P17 debug viewer bitmap */
static void* s_bmpView = NULL;   /* temporary P18 debug viewer bitmap */
static int s_dcFail = -1;

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

    if (s_dcPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P11 document: %d passed, %d failed",
                     s_dcPass, s_dcFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 200);
        }
    }

    if (s_rzPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P12 readability: %d passed, %d failed",
                     s_rzPass, s_rzFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 215);
        }
    }

    if (s_syPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P13 style: %d passed, %d failed",
                     s_syPass, s_syFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 230);
        }
    }

    if (s_lmPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P14 linkmgr: %d passed, %d failed",
                     s_lmPass, s_lmFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 245);
        }
    }

    if (s_dc15Pass >= 0) {
        n = snprintf(buf, sizeof(buf), "P15 dither/scale: %d passed, %d failed",
                     s_dc15Pass, s_dc15Fail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 275);
        }
    }

    if (s_pngPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P17 png: %d passed, %d failed",
                     s_pngPass, s_pngFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 290);
        }
    }

    if (s_bmPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P18 bmp: %d passed, %d failed",
                     s_bmPass, s_bmFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 305);
        }
    }

    if (s_giPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P19 gif: %d passed, %d failed",
                     s_giPass, s_giFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 320);
        }
    }

    if (s_jpPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P20 jpeg: %d passed, %d failed",
                     s_jpPass, s_jpFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 335);
        }
    }

    if (s_wpPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P21 webp: %d passed, %d failed",
                     s_wpPass, s_wpFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 350);
        }
    }

    if (s_icPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P23 ico: %d passed, %d failed",
                     s_icPass, s_icFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 365);
        }
    }

    if (s_svPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P24 svg: %d passed, %d failed",
                     s_svPass, s_svFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 380);
        }
    }

    if (s_p25Resolved >= 0) {
        n = snprintf(buf, sizeof(buf), "P25 img: %s",
                     s_p25Resolved ? "resolved" : "FAILED");
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 395);
        }
    }

    /* P26 cloud layout visual + HUD */
    if (s_clBuilt) {
        cl_draw(s_clScroll);
        n = snprintf(buf, sizeof(buf),
                     "P26 cloud: %d items, scroll=%d",
                     cl_item_count(), s_clScroll);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 410);
        }
    }

    if (s_clPass >= 0) {
        n = snprintf(buf, sizeof(buf), "P26 selftest: %d passed, %d failed",
                     s_clPass, s_clFail);
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) {
                n = (int)sizeof(buf) - 1;
            }
            pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding, 70, 425);
        }
    }

    /* P27 chrome/hud showcase: rotates every 60 frames through
     * ssl-web / reader-badge / loading-known-total / indeterminate /
     * about-page states. */
    {
        static PlutoUrl demo;
        memset(&demo, 0, sizeof(demo));
        int loading = 0, reader = 0;
        long cur = 0, tot = 0;

        switch (s_p27Scenario % 5) {
        case 0:
            strcpy(demo.scheme, "https");
            strcpy(demo.host, "example.com");
            demo.isSsl = 1;
            break;
        case 1:
            strcpy(demo.scheme, "https");
            strcpy(demo.host, "news.example.com");
            demo.isSsl = 1;
            reader = 1;
            break;
        case 2:
            strcpy(demo.scheme, "https");
            strcpy(demo.host, "big.example.com");
            loading = 1;
            tot = 1000;
            cur = (long)(s_frame * 7 % 1100);
            break;
        case 3:
            strcpy(demo.scheme, "http");
            strcpy(demo.host, "slow.example.net");
            loading = 1;
            break;
        default:
            strcpy(demo.scheme, "about");
            strcpy(demo.host, "home");
            break;
        }

        /* Home page slot: every 6th scenario shows the speed dial page
         * with an oscillating crank scroll; chrome+hud compose on top
         * (real frame order). */
        if ((s_p27Scenario % 6) == 5) {
            double crank = 14.0 * sin((double)s_frame * 0.04);
            hp_draw(crank);
        }

        /* P30 rotating page demos (error / bookmarks / history) drawn
         * at content level, under chrome+hud like the real browser. */
        if (!ab_is_open()) {
            int slot = s_p30Slot % 3;
            if (slot == 0) {
                ep_draw();
            } else if (slot == 1) {
                bm_draw(6.0 * sin((double)s_frame * 0.03));
            } else {
                hi_draw(6.0 * sin((double)s_frame * 0.03));
            }
        }

        ch_draw(&demo, NULL, loading, cur, tot, reader);
        hud_draw(120, 500,
                 "https://very.long.example-domain.io/some/deep/path?q=1");
        hud_draw_hover_status(
            "https://hovered.example.org/a/really/long/target/link.html");

        /* P29: address-bar overlay composes on top (Lua draws it last) */
        ab_draw_overlay();

        if (s_uiPass >= 0) {
            n = snprintf(buf, sizeof(buf),
                         "P27 selftest: %d passed, %d failed",
                         s_uiPass, s_uiFail);
            if (n > 0) {
                if ((size_t)n >= sizeof(buf)) {
                    n = (int)sizeof(buf) - 1;
                }
                pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding,
                                       70, 440);
            }
        }

        if (s_hpPass >= 0) {
            n = snprintf(buf, sizeof(buf),
                         "P28 selftest: %d passed, %d failed "
                         "(sel=%d)", s_hpPass, s_hpFail,
                         hp_selected_index());
            if (n > 0) {
                if ((size_t)n >= sizeof(buf)) {
                    n = (int)sizeof(buf) - 1;
                }
                pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding,
                                       70, 455);
            }
        }

        if (s_abPass >= 0) {
            n = snprintf(buf, sizeof(buf),
                         "P29 selftest: %d passed, %d failed%s",
                         s_abPass, s_abFail,
                         ab_keyboard_shown() ? " [kb]" :
                             (ab_is_open() ? " [armed]" : ""));
            if (n > 0) {
                if ((size_t)n >= sizeof(buf)) {
                    n = (int)sizeof(buf) - 1;
                }
                pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding,
                                       70, 470);
            }
        }

        if (s_p30Pass >= 0) {
            static const char* const slotNames[3] = { "error",
                                                      "bookmarks",
                                                      "history" };
            n = snprintf(buf, sizeof(buf),
                         "P30 selftest: %d passed, %d failed (%s)",
                         s_p30Pass, s_p30Fail,
                         slotNames[s_p30Slot % 3]);
            if (n > 0) {
                if ((size_t)n >= sizeof(buf)) {
                    n = (int)sizeof(buf) - 1;
                }
                pd->graphics->drawText(buf, (size_t)n, kASCIIEncoding,
                                       70, 485);
            }
        }
    }

    /* P25 pipeline visual: placeholder card (alt + selected border) while
     * downloading; scaled decoded bitmap once resolved */
    if (s_p25Stage == 1) {
        id_draw(238, 56, 90, 50, "Playdate camera!", NULL, 1, kP25Png);
    } else if (s_p25Stage == 2 && s_p25Resolved == 1) {
        id_draw(238, 110, 100, 70, NULL, NULL, 1, kP25Png);
    }

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
    /* temporary debug viewers: decoded bench PNG + BMP (266x200) */
    if (s_pngView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_pngView, 10, 20,
                                 kBitmapUnflipped);
    }
    if (s_bmpView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_bmpView, 124, 20,
                                 kBitmapUnflipped);
    }
    if (s_gifView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_gifView, 238, 20,
                                 kBitmapUnflipped);
    }
    if (s_jpegView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_jpegView, 10, 20,
                                 kBitmapUnflipped);
    }
    if (s_webpView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_webpView, 10, 20,
                                 kBitmapUnflipped);
    }
    if (s_icoView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_icoView, 124, 124,
                                 kBitmapUnflipped);
    }
    if (s_svgView != NULL) {
        pd->graphics->drawBitmap((LCDBitmap*)s_svgView, 124, 124,
                                 kBitmapUnflipped);
    }
#endif

    const char* hint = "Phase P07 raw TCP HTTP client";
    pd->graphics->drawText(hint, strlen(hint), kASCIIEncoding, 100, 260);
}

// P29 manual-test submit path: real navigation lands in a later phase.
static void ab_submit_stub(const char* url, void* ud) {
    (void)ud;
    PLUTO_LOG("[P29] nav stub -> %s", url);
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

    // Image pipeline pump (main.lua ImageDecoder.update() parity).
    id_update();

    // P25 e2e: sequential image downloads from a localhost HTTP server.
    if (s_p25Stage == 0 && s_frame == 150) {
        PLUTO_LOG("[P25] e2e enqueue png/bmp/404");
        s_p25T0Ms = pd->system->getCurrentTimeMilliseconds();
        id_enqueue(kP25Png);
        id_enqueue(kP25Bmp);
        id_enqueue(kP25Bad);
        s_p25Stage = 1;
    } else if (s_p25Stage == 1) {
        if (id_is_cached(kP25Png) && id_is_cached(kP25Bmp) &&
            id_is_cached(kP25Bad)) {
            unsigned ms =
                pd->system->getCurrentTimeMilliseconds() - s_p25T0Ms;
            LCDBitmap* b = (LCDBitmap*)id_get_image(kP25Png);
            int iw = 0, ih = 0, rb = 0;
            uint8_t* mk = NULL;
            uint8_t* dt = NULL;
            if (b)
                pd->graphics->getBitmapData(b, &iw, &ih, &rb, &mk, &dt);
            PLUTO_LOG("[P25] e2e resolved in %u ms (png %dx%d, "
                      "bmp decoded=%d, 404 cached-as-failed=%d)",
                      ms, iw, ih,
                      id_is_decoded(kP25Bmp),
                      id_is_cached(kP25Bad) && !id_is_decoded(kP25Bad));
            s_p25Resolved = (b != NULL && id_is_decoded(kP25Bmp)) ? 1 : 0;
            s_p25Stage = 2;
        } else if (++s_p25WaitFrames > 900u) {
            PLUTO_ERROR("[P25] e2e TIMEOUT: png=%d bmp=%d bad=%d "
                        "(httpLoading=%d)",
                        id_is_cached(kP25Png), id_is_cached(kP25Bmp),
                        id_is_cached(kP25Bad), hc_is_loading());
            s_p25Resolved = 0;
            s_p25Stage = 2;
        }
    }

    /* P26 visual: build the cloud fixture once, then sweep scroll so
     * clipping, item painters and the scrollbar all get exercised. */
    if (s_frame == 180 && !s_clBuilt) {
        ClDoc* doc = cl_parse(kClFixture, strlen(kClFixture), NULL);
        cl_build(doc);
        cl_doc_free(doc);
        s_clBuilt = 1;
        PLUTO_LOG("[P26] fixture built: items=%d totalH=%.0f",
                  cl_item_count(), cl_total_height());
    } else if (s_clBuilt) {
        double maxScroll = cl_total_height() > 216.0
                               ? cl_total_height() - 216.0 : 0.0;
        s_clScroll += 4;
        if (maxScroll <= 0 || s_clScroll > (int)maxScroll) s_clScroll = 0;
    }

    /* P27 showcase rotation */
    if (s_frame % 60 == 0) s_p27Scenario++;

    /* P28 scripted input walk: down, down, up, down ... logging each
     * selection transition through hp_handle_input. */
    if (s_frame >= 240 && s_frame % 45 == 0) {
        static const HpButton walk[] = { HP_BTN_DOWN, HP_BTN_DOWN,
                                         HP_BTN_UP, HP_BTN_DOWN,
                                         HP_BTN_RIGHT };
        static int wi = 0;
        HpButton b = walk[wi % 5];
        wi++;
        hp_handle_input(b, NULL, 0);
        PLUTO_LOG("[P28] input walk btn=%d -> sel=%d", (int)b,
                  hp_selected_index());
    }

    /* P29 address bar demo: armed pill at frame 300 (Left/Right + B-held
     * gating testable), auto-launch at 600 unless already shown. */
    if (s_frame == 300) {
        ab_open("https://example.com/", ab_submit_stub, NULL);
        PLUTO_LOG("[P29] open (armed, prefilled)");
    }
    if (s_frame >= 600 && ab_is_open() && !ab_keyboard_shown()) {
        ab_launch_keyboard();
        if (ab_keyboard_shown()) {
            PLUTO_LOG("[P29] keyboard launched (type + submit to test)");
        }
    }
    /* skipInputFrames global: consumed once per frame like Lua's handler */
    (void)ab_pop_input_skip();

    /* Give the manual typing test a window, then clear the overlay so
     * the P30 page demos are visible. */
    if (s_frame == 900 && ab_is_open()) {
        ab_cancel();
        PLUTO_LOG("[P29] auto-cancel for P30 showcase");
    }

    /* P30 showcase: rotate pages and script a few inputs each */
    if (!ab_is_open()) {
        static int s_p30Step = 0;
        if (s_frame % 120 == 0 && s_frame > 0) s_p30Slot++;
        if (s_frame >= 960 && s_frame % 45 == 0) {
            s_p30Step++;
            int slot = s_p30Slot % 3;
            if (slot == 0) {
                static const EpButton epSeq[] = { EP_BTN_RIGHT,
                                                  EP_BTN_RIGHT,
                                                  EP_BTN_A };
                EpButton b = epSeq[s_p30Step % 3];
                EpAction act = ep_handle_input(b);
                PLUTO_LOG("[P30] error input btn=%d -> act=%d sel=%d",
                          (int)b, (int)act, ep_selected_index());
            } else if (slot == 1) {
                static const LrButton lrSeq[] = { LR_BTN_DOWN,
                                                  LR_BTN_A };
                LrButton b = lrSeq[s_p30Step % 2];
                char u[PLUTO_URL_FULLPATH_MAX];
                LpAction act = bm_handle_input(b, u, sizeof(u));
                PLUTO_LOG("[P30] bookmarks btn=%d -> act=%d url=%s",
                          (int)b, (int)act, u);
            } else {
                static const LrButton hiSeq[] = { LR_BTN_DOWN,
                                                  LR_BTN_A };
                LrButton b = hiSeq[s_p30Step % 2];
                char u[PLUTO_URL_FULLPATH_MAX];
                LpAction act = hi_handle_input(b, u, sizeof(u));
                PLUTO_LOG("[P30] history btn=%d -> act=%d url=%s",
                          (int)b, (int)act, u);
            }
        }
    }

    if (s_frame == 1) {
        PLUTO_LOG("first frame rendered");
    } else if (s_frame % 300 == 0) {
        PLUTO_LOG("heartbeat frame=%u", s_frame);
    }

    draw_placeholder();
    return 1;
}

// param renamed from `playdate` so the vendored-keyboard global below is
// reachable here (SDK header only fixes types, not names).
int eventHandler(PlaydateAPI* pdApi, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    switch (event) {
        case kEventInit:
            pd = pdApi;
            playdate = pdApi; // vendor/keyboard reads this global

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

            // P08 HTML tokenizer self-tests (oracle replay fixtures).
            htt_init(pd);
            selftest_tokenizer_run(&s_ttPass, &s_ttFail);
            if (s_ttFail > 0) {
                PLUTO_ERROR("P08 SELFTEST FAILURES: %d", s_ttFail);
            }

            // P09 DOM tree builder self-tests (tokenizer -> dom pipeline).
            dom_init(pd);
            selftest_dom_run(&s_dmPass, &s_dmFail);
            if (s_dmFail > 0) {
                PLUTO_ERROR("P09 SELFTEST FAILURES: %d", s_dmFail);
            }

            // P11 document model self-tests (dom -> blocks/links pipeline).
            doc_init(pd);
            selftest_document_run(&s_dcPass, &s_dcFail);
            if (s_dcFail > 0) {
                PLUTO_ERROR("P11 SELFTEST FAILURES: %d", s_dcFail);
            }

            // P12 readability (reader-mode distiller) self-tests.
            selftest_readability_run(&s_rzPass, &s_rzFail);
            if (s_rzFail > 0) {
                PLUTO_ERROR("P12 SELFTEST FAILURES: %d", s_rzFail);
            }

            // Lua main.lua did not call setRefreshRate -> keep SDK default.

            // P13 typography system loads every Style font (with
            // system-font fallback per slot, mirroring Style.init).
            style_init(pd);
            s_fontBody = (LCDFont*)style_get_body_font(0, 0, NULL);
            if (s_fontBody == NULL) {
                PLUTO_ERROR("style fonts unavailable (no body font)");
            }
            style_set_system_font((PlutoFont*)s_fontBody);
            lm_init(pd);

            selftest_style_run(&s_syPass, &s_syFail);
            if (s_syFail > 0) {
                PLUTO_ERROR("P13 SELFTEST FAILURES: %d", s_syFail);
            }

            selftest_link_manager_run(&s_lmPass, &s_lmFail);
            if (s_lmFail > 0) {
                PLUTO_ERROR("P14 SELFTEST FAILURES: %d", s_lmFail);
            }

            selftest_decoders_run(&s_dc15Pass, &s_dc15Fail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            selftest_decoders_device(pd);
#endif
            if (s_dc15Fail > 0) {
                PLUTO_ERROR("P15 SELFTEST FAILURES: %d", s_dc15Fail);
            }

            selftest_inflate_run(&s_inPass, &s_inFail);
            if (s_inFail > 0) {
                PLUTO_ERROR("P16 SELFTEST FAILURES: %d", s_inFail);
            }

            selftest_png_set_pd(pd);
            selftest_png_run(&s_pngPass, &s_pngFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P17 debug viewer: decoded bench PNG on screen */
            s_pngView = png_decode(pd, fx_bench, FX_BENCH_LEN,
                                   360, 200);
#endif
            if (s_pngFail > 0) {
                PLUTO_ERROR("P17 SELFTEST FAILURES: %d", s_pngFail);
            }

            selftest_bmp_set_pd(pd);
            selftest_bmp_run(&s_bmPass, &s_bmFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P18 debug viewer: decoded bench BMP */
            s_bmpView = bmp_decode(pd, fx_bmp_bench,
                                   FX_BMP_BENCH_LEN);
#endif
            if (s_bmFail > 0) {
                PLUTO_ERROR("P18 SELFTEST FAILURES: %d", s_bmFail);
            }

            selftest_gif_set_pd(pd);
            selftest_gif_run(&s_giPass, &s_giFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P19 debug viewer: decoded bench GIF */
            s_gifView = gif_decode(pd, fx_g_bench, FX_G_BENCH_LEN,
                                   360, 200);
#endif
            if (s_giFail > 0) {
                PLUTO_ERROR("P19 SELFTEST FAILURES: %d", s_giFail);
            }

            selftest_jpeg_set_pd(pd);
            selftest_jpeg_run(&s_jpPass, &s_jpFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P20 debug viewer: decoded bench JPEG */
            s_jpegView = jpeg_decode(pd, fx_j_pil_bench,
                                     FX_J_PIL_BENCH_LEN, 360, 200);
#endif
            if (s_jpFail > 0) {
                PLUTO_ERROR("P20 SELFTEST FAILURES: %d", s_jpFail);
            }

            selftest_webp_set_pd(pd);
            selftest_webp_run(&s_wpPass, &s_wpFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P21 debug viewer: decoded bench WebP */
            s_webpView = webp_decode(pd, fx_w_pil_bench,
                                     FX_W_PIL_BENCH_LEN, 360, 200);
#endif
            if (s_wpFail > 0) {
                PLUTO_ERROR("P21 SELFTEST FAILURES: %d", s_wpFail);
            }

            selftest_ico_set_pd(pd);
            selftest_ico_run(&s_icPass, &s_icFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P23 debug viewer: decoded scaled ICO */
            s_icoView = ico_decode(pd, fx_i_scaled, FX_I_I_SCALED_LEN,
                                   360, 200);
#endif
            if (s_icFail > 0) {
                PLUTO_ERROR("P23 SELFTEST FAILURES: %d", s_icFail);
            }

            selftest_svg_set_pd(pd);
            selftest_svg_run(&s_svPass, &s_svFail);
#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
            /* temporary P24 debug viewer: decoded bench SVG */
            s_svgView = svg_decode(pd, fx_s_bench, FX_S_BENCH_LEN,
                                   360, 200);
#endif
            if (s_svFail > 0) {
                PLUTO_ERROR("P24 SELFTEST FAILURES: %d", s_svFail);
            }

            id_init(pd);
            selftest_image_decoder_set_pd(pd);
            selftest_image_decoder_run(&s_idPass, &s_idFail);
            if (s_idFail > 0) {
                PLUTO_ERROR("P25 SELFTEST FAILURES: %d", s_idFail);
            }
            PLUTO_LOG("[P25] image decoder ready");

            // P26 cloud layout: parse/build/draw pipeline over JSON docs.
            cl_init(pd);
            selftest_cloud_layout_run(&s_clPass, &s_clFail);
            if (s_clFail > 0) {
                PLUTO_ERROR("P26 SELFTEST FAILURES: %d", s_clFail);
            }
            PLUTO_LOG("[P26] cloud layout ready");

            // P27 chrome + hud: top bar and floating overlays.
            chrome_init(pd);
            hud_init(pd);
            selftest_ui_run(&s_uiPass, &s_uiFail);
            if (s_uiFail > 0) {
                PLUTO_ERROR("P27 SELFTEST FAILURES: %d", s_uiFail);
            }
            PLUTO_LOG("[P27] chrome/hud ready");

            // P28 home page: speed dial grid + input rules.
            hp_init(pd);
            selftest_home_page_run(&s_hpPass, &s_hpFail);
            if (s_hpFail > 0) {
                PLUTO_ERROR("P28 SELFTEST FAILURES: %d", s_hpFail);
            }
            PLUTO_LOG("[P28] home page ready");

            // P29 address bar + vendored C keyboard.
            ab_init(pd, update, NULL);
            selftest_address_bar_run(&s_abPass, &s_abFail);
            if (s_abFail > 0) {
                PLUTO_ERROR("P29 SELFTEST FAILURES: %d", s_abFail);
            }
            PLUTO_LOG("[P29] address bar ready");

            // P30 error / bookmarks / history pages.
            ep_init_pd(pd);
            bm_init_pd(pd);
            hi_init_pd(pd);
            ep_show("DNS lookup failed",
                    "https://very.long.example-domain.io/deep/path");
            selftest_pages_run(&s_p30Pass, &s_p30Fail);
            if (s_p30Fail > 0) {
                PLUTO_ERROR("P30 SELFTEST FAILURES: %d", s_p30Fail);
            }
            PLUTO_LOG("[P30] pages ready");

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
