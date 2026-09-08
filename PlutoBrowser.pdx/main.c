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
#include "render/layout.h"
#include "render/selftest_layout.h"
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
#include "ui/settings_page.h"
#include "ui/selftest_settings.h"
#include "core/browser.h"
#include "core/selftest_browser.h"
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
static int s_dcFail = -1;
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
/* P26 cloud layout / P26B layout selftest counts */
static int s_clPass = -1;
static int s_lyPass = -1, s_lyFail = -1;   /* P26B layout */
static int s_clFail = -1;
/* P27 chrome/hud selftest counts */
static int s_uiPass = -1;
static int s_uiFail = -1;
/* P28 home page selftest counts */
static int s_hpPass = -1;
static int s_hpFail = -1;
/* P29 address bar selftest counts */
static int s_abPass = -1;
static int s_abFail = -1;
/* P30 pages selftest counts */
static int s_p30Pass = -1;
static int s_p30Fail = -1;
/* P31 settings page selftest counts */
static int s_spPass = -1;
static int s_spFail = -1;
/* P32 browser engine: selftest counts */
static int s_brPass = -1;
static int s_brFail = -1;

// Vendored keyboard lib (vendor/keyboard) resolves the SDK handle through
// this global symbol; every PlutoBrowser module keeps its own s_pd static.
PlaydateAPI* playdate = NULL;

static int update(void* userdata)
{
    (void)userdata;
    s_frame++;

    /* [P33] Real application shell: br_frame() pumps the cooperative
     * schedulers itself (tasks/http/image inside browser.c) and composes
     * content + chrome + overlays, mirroring Lua playdate.update ->
     * browser.updateFrame(). Input/crank come from the engine's default
     * hardware provider. */
    br_frame();
    if (s_frame % 300 == 0)
        PLUTO_LOG("app frame=%u st=%d url='%s'", s_frame, br_state(),
                  br_current_normalized());
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
#ifdef TARGET_SIMULATOR
            selftest_util_run(&s_selfPass, &s_selfFail);
            if (s_selfFail > 0) {
                PLUTO_ERROR("P02 SELFTEST FAILURES: %d", s_selfFail);
            }
#endif

            // P03 core/url self-tests (permanent boot diagnostics).
#ifdef TARGET_SIMULATOR
            selftest_url_run(&s_urlPass, &s_urlFail);
            if (s_urlFail > 0) {
                PLUTO_ERROR("P03 SELFTEST FAILURES: %d", s_urlFail);
            }
#endif
            storage_init(pd);
            cj_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_storage_run(&s_stPass, &s_stFail);
            if (s_stFail > 0) {
                PLUTO_ERROR("P04 SELFTEST FAILURES: %d", s_stFail);
            }
#endif

            // P05 cooperative task scheduler init.
            tasks_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_tasks_run(&s_tkPass, &s_tkFail);
            if (s_tkFail > 0) {
                PLUTO_ERROR("P05 SELFTEST FAILURES: %d", s_tkFail);
            }
#endif

            // P06 charset conversion + HTML entities init.
            encoding_init(pd);
            entities_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_encoding_run(&s_enPass, &s_enFail);
            if (s_enFail > 0) {
                PLUTO_ERROR("P06 SELFTEST FAILURES: %d", s_enFail);
            }
#endif

            // P07 raw TCP HTTP client init.
            cj_init(pd); // idempotent: reuses the P04 storage backend
            hc_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_http_run(&s_hcPass, &s_hcFail);
            if (s_hcFail > 0) {
                PLUTO_ERROR("P07 SELFTEST FAILURES: %d", s_hcFail);
            }
#endif

            // P08 HTML tokenizer init.
            htt_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_tokenizer_run(&s_ttPass, &s_ttFail);
            if (s_tkFail > 0) {
                PLUTO_ERROR("P08 SELFTEST FAILURES: %d", s_ttFail);
            }
#endif

            // P09 DOM tree builder init.
            dom_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_dom_run(&s_dmPass, &s_dmFail);
            if (s_dmFail > 0) {
                PLUTO_ERROR("P09 SELFTEST FAILURES: %d", s_dmFail);
            }
#endif

            // P11 document model init.
            doc_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_document_run(&s_dcPass, &s_dcFail);
            if (s_dcFail > 0) {
                PLUTO_ERROR("P11 SELFTEST FAILURES: %d", s_dcFail);
            }
#endif

#ifdef TARGET_SIMULATOR
            // P12 readability (reader-mode distiller) self-tests.
            selftest_readability_run(&s_rzPass, &s_rzFail);
            if (s_rzFail > 0) {
                PLUTO_ERROR("P12 SELFTEST FAILURES: %d", s_rzFail);
            }
#endif

            // P13 typography system loads every Style font.
            style_init(pd);
            s_fontBody = (LCDFont*)style_get_body_font(0, 0, NULL);
            if (s_fontBody == NULL) {
                PLUTO_ERROR("style fonts unavailable (no body font)");
            }
            style_set_system_font((PlutoFont*)s_fontBody);
            lm_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_style_run(&s_syPass, &s_syFail);
            if (s_syFail > 0) {
                PLUTO_ERROR("P13 SELFTEST FAILURES: %d", s_syFail);
            }

            selftest_link_manager_run(&s_lmPass, &s_lmFail);
            if (s_lmFail > 0) {
                PLUTO_ERROR("P14 SELFTEST FAILURES: %d", s_lmFail);
            }

            selftest_decoders_run(&s_dc15Pass, &s_dc15Fail);
            selftest_decoders_device(pd);
            if (s_dc15Fail > 0) {
                PLUTO_ERROR("P15 SELFTEST FAILURES: %d", s_dc15Fail);
            }

            selftest_inflate_run(&s_inPass, &s_inFail);
            if (s_inFail > 0) {
                PLUTO_ERROR("P16 SELFTEST FAILURES: %d", s_inFail);
            }

            selftest_png_set_pd(pd);
            selftest_png_run(&s_pngPass, &s_pngFail);
            if (s_pngFail > 0) {
                PLUTO_ERROR("P17 SELFTEST FAILURES: %d", s_pngFail);
            }

            selftest_bmp_set_pd(pd);
            selftest_bmp_run(&s_bmPass, &s_bmFail);
            if (s_bmFail > 0) {
                PLUTO_ERROR("P18 SELFTEST FAILURES: %d", s_bmFail);
            }

            selftest_gif_set_pd(pd);
            selftest_gif_run(&s_giPass, &s_giFail);
            if (s_giFail > 0) {
                PLUTO_ERROR("P19 SELFTEST FAILURES: %d", s_giFail);
            }

            selftest_jpeg_set_pd(pd);
            selftest_jpeg_run(&s_jpPass, &s_jpFail);
            if (s_jpFail > 0) {
                PLUTO_ERROR("P20 SELFTEST FAILURES: %d", s_jpFail);
            }

            selftest_webp_set_pd(pd);
            selftest_webp_run(&s_wpPass, &s_wpFail);
            if (s_wpFail > 0) {
                PLUTO_ERROR("P21 SELFTEST FAILURES: %d", s_wpFail);
            }

            selftest_ico_set_pd(pd);
            selftest_ico_run(&s_icPass, &s_icFail);
            if (s_icFail > 0) {
                PLUTO_ERROR("P23 SELFTEST FAILURES: %d", s_icFail);
            }

            selftest_svg_set_pd(pd);
            selftest_svg_run(&s_svPass, &s_svFail);
            if (s_svFail > 0) {
                PLUTO_ERROR("P24 SELFTEST FAILURES: %d", s_svFail);
            }
#endif

            id_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_image_decoder_set_pd(pd);
            selftest_image_decoder_run(&s_idPass, &s_idFail);
            if (s_idFail > 0) {
                PLUTO_ERROR("P25 SELFTEST FAILURES: %d", s_idFail);
            }
#endif
            PLUTO_LOG("image decoder ready");

            // P26 cloud layout init.
            cl_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_cloud_layout_run(&s_clPass, &s_clFail);
            if (s_clFail > 0) {
                PLUTO_ERROR("P26 SELFTEST FAILURES: %d", s_clFail);
            }
#endif
            PLUTO_LOG("cloud layout ready");

            // P26B block layout init.
            layout_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_layout_run(&s_lyPass, &s_lyFail);
            if (s_lyFail > 0) {
                PLUTO_ERROR("P26B SELFTEST FAILURES: %d", s_lyFail);
            }
#endif
            PLUTO_LOG("layout ready");

            // P27 chrome + hud init.
            chrome_init(pd);
            hud_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_ui_run(&s_uiPass, &s_uiFail);
            if (s_uiFail > 0) {
                PLUTO_ERROR("P27 SELFTEST FAILURES: %d", s_uiFail);
            }
#endif
            PLUTO_LOG("chrome/hud ready");

            // P28 home page init.
            hp_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_home_page_run(&s_hpPass, &s_hpFail);
            if (s_hpFail > 0) {
                PLUTO_ERROR("P28 SELFTEST FAILURES: %d", s_hpFail);
            }
#endif
            PLUTO_LOG("home page ready");

            // P29 address bar + vendored C keyboard init.
            ab_init(pd, update, NULL);
#ifdef TARGET_SIMULATOR
            selftest_address_bar_run(&s_abPass, &s_abFail);
            if (s_abFail > 0) {
                PLUTO_ERROR("P29 SELFTEST FAILURES: %d", s_abFail);
            }
#endif
            PLUTO_LOG("address bar ready");

            // P30 error / bookmarks / history pages init.
            ep_init_pd(pd);
            bm_init_pd(pd);
            hi_init_pd(pd);
#ifdef TARGET_SIMULATOR
            selftest_pages_run(&s_p30Pass, &s_p30Fail);
            if (s_p30Fail > 0) {
                PLUTO_ERROR("P30 SELFTEST FAILURES: %d", s_p30Fail);
            }
#endif
            PLUTO_LOG("pages ready");

            // P31 settings overlay init.
            sp_init_pd(pd);
#ifdef TARGET_SIMULATOR
            selftest_settings_run(&s_spPass, &s_spFail);
            if (s_spFail > 0) {
                PLUTO_ERROR("P31 SELFTEST FAILURES: %d", s_spFail);
            }
#endif
            PLUTO_LOG("settings page ready");

            // P32 browser engine.
            br_init(pd);
#ifdef TARGET_SIMULATOR
            selftest_browser_run(pd, &s_brPass, &s_brFail);
            if (s_brFail > 0) {
                PLUTO_ERROR("P32 SELFTEST FAILURES: %d", s_brFail);
            }
#endif
            PLUTO_LOG("browser engine ready");

            // P33 app shell: hand control to the real browser. br_boot()
            // re-runs the Lua init tail (STATE_HOME, callbacks, menu) on a
            // clean slate after the selftest suites churned engine state;
            // seams are force-restored so the app polls real hardware even
            // if a future suite forgets its teardown.
            br_set_input_source(NULL, NULL);
            br_set_clock_fn(NULL);
            br_set_update_trampoline(update);   // vendored keyboard pump
            br_boot();
            PLUTO_LOG("[P33] app shell engaged");

            pd->system->setUpdateCallback(update, NULL);
            PLUTO_LOG("update callback registered");
            break;

        case kEventTerminate:
            PLUTO_LOG("terminate event, frames=%u", s_frame);
            break;

        /* Menu button: handled by the NATIVE system menu items built in
         * browser.c (br_system_menu_refresh) — no kEventPause hooks needed. */

        default:
            break;
    }

    return 0;
}
