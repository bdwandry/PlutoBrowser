// selftest_image_decoder.c — [P25] selftests for the image pipeline port.
//
// Exercises the magic-byte dispatch through the Lua _testDecode seam:
// inline formats (BMP/ICO/SVG) must decode synchronously, while the async
// formats (JPEG/PNG/GIF/WebP) return NULL immediately by design (their
// completion is covered by the end-to-end network check in main.c).
// Cache/queue API guards run without touching the network. Reuses the
// per-format fixture vectors from earlier phases.

#include "render/selftest_image_decoder.h"

#include <stdio.h>
#include <string.h>

#if defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#include "core/logger.h"
#include "render/image_decoder.h"
#include "render/decoders/selftest_bmp_fixtures.h"
#include "render/decoders/selftest_gif_fixtures.h"
#include "render/decoders/selftest_ico_fixtures.h"
#include "render/decoders/selftest_png_fixtures.h"
#include "render/decoders/selftest_svg_fixtures.h"
#include "util/mem.h"

static int s_pass, s_fail;

#if defined(TARGET_PLAYDATE)
static struct PlaydateAPI* s_pd = NULL;
void selftest_image_decoder_set_pd(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void selftest_image_decoder_set_pd(struct PlaydateAPI* pd) { (void)pd; }
#endif

static void ck(const char* name, int cond) {
    if (cond) { s_pass++; PLUTO_LOG("[P25] PASS %s", name); }
    else      { s_fail++; PLUTO_ERROR("[P25] FAIL %s", name); }
}

static void case_dispatch(void) {
    /* inline formats decode synchronously */
    ck("D.bmp_sync",
       id_test_decode(fx_b24, FX_B24_LEN) != NULL);
    ck("D.ico_sync",
       id_test_decode(fx_i_scaled, FX_I_I_SCALED_LEN) != NULL);
    ck("D.svg_sniff",
       id_test_decode((const uint8_t*)fx_s_rects, FX_S_RECTS_LEN) != NULL);

    /* short bodies are rejected before any signature sniffing */
    ck("G.short_body", id_test_decode((const uint8_t*)"PNG", 3) == NULL);

    /* unknown payloads fall through every branch -> nil */
    ck("G.garbage_text",
       id_test_decode((const uint8_t*)"just some text data", 19) == NULL);

    /* async formats: NULL on the synchronous path (Lua parity: _testDecode
     * does not cover them); completion is exercised via the e2e pipeline */
    ck("A.png_async_null",
       id_test_decode(fx_red, FX_RED_LEN) == NULL);
    ck("A.gif_async_null",
       id_test_decode(fx_g_basic, FX_G_BASIC_LEN) == NULL);
}

static void case_cache_api(void) {
    const char* u = "http://unit.test/a.png";

    ck("A.unknown_state",
       !id_is_cached(u) && !id_is_decoded(u) && id_get_image(u) == NULL);

    /* guard paths: empty/NULL src ignored; unknown evict is a no-op */
    id_enqueue(NULL);
    id_enqueue("");
    id_evict(NULL);
    id_evict(u);
    ck("A.guards_safe", 1);
}

int selftest_image_decoder_run(int* passed, int* failed) {
    s_pass = 0;
    s_fail = 0;
    case_dispatch();
    case_cache_api();
    PLUTO_LOG("[P25] image decoder selftests done: %d passed, %d failed",
              s_pass, s_fail);
    if (passed) *passed = s_pass;
    if (failed) *failed = s_fail;
    return s_fail;
}
