/*
 * SW2d data:-URL script host test (macOS only, NOT part of the Playdate
 * build). Exercises:
 *   1  scanner marks data:-URL src with the JS_SCRIPT_DATA sentinel and
 *      locates the payload INSIDE the page HTML (span, no copy)
 *   2  index alignment: data: + normal external + inline scripts keep
 *      document order (the jsbridge re-scan must see identical slots)
 *   3  decoder: plain payload, percent-encoded payload (case-insensitive
 *      scheme, mediatype + ;base64 param), base64 payload
 *   4  decoder malformed: no comma, empty payload, dangling base64 quad,
 *      non-alphabet base64 char → NULL
 *   5  payload > 512B (would be truncated by the URL-storage path)
 *
 * Build & run: bash /tmp/build_du.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/jsbridge.h"
#include "html/jsext.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                    \
    do                                                                       \
    {                                                                        \
        if (cond) { printf("PASS %s\n", name); g_pass++; }                   \
        else      { printf("FAIL %s\n", name); g_fail++; }                   \
    } while (0)

/* data: payloads embedded in a page. A 600-byte percent-encoded payload
 * proves the span path (URL-storage path would truncate at 512). */
#define BIG_N 600
static char bigPayload[BIG_N + 1];

static void build_big_payload(void)
{
    /* 600 chars of safe script text with a marker. */
    const char *unit = "window.__d=1;";
    size_t ul = strlen(unit);
    size_t i = 0;
    while (i < BIG_N - 32)
    {
        memcpy(bigPayload + i, unit, ul);
        i += ul;
    }
    memcpy(bigPayload + i, "window.__bigDataRan=1;", 23);
    bigPayload[i + 22] = '\0';
}

int main(void)
{
    build_big_payload();

    /* ── Fixture page: inline, data:, normal ext, data: again ───────────── */
    char page[4096];
    snprintf(page, sizeof(page),
             "<html><body>\n"
             "<script>window.__first=1;</script>\n"
             "<script src=\"data:text/javascript,window.__plainRan=1;\"></script>\n"
             "<script src=\"data:application/javascript;base64,d2luZG93Ll9fYjY0UmFuPTE7\"></script>\n"
             "<script src=\"/normal-ext.js\"></script>\n"
             "<script src=\"data:text/javascript,%s\"></script>\n"
             "</body></html>\n",
             bigPayload);

    /* ── 1+2: scan via jsext_collect (the pipeline entry) ────────────────── */
    JsScriptSlot *slots = NULL;
    JsExtScript *ext = NULL;
    int slotCount = 0, extCount = 0;
    JsExtArena *arena = NULL;
    int total = jsext_collect(page, "https://ex.com/page.html", &slots,
                              &slotCount, &ext, &extCount, &arena);
    CHECK(total == 5, "scan counts 5 script elements");
    CHECK(extCount == 1, "only the normal external hits the URL table");

    int dataSlots = 0, extSlots = 0, inlineSlots = 0;
    for (int i = 0; i < total && i < slotCount; i++)
    {
        if (!slots[i].isExt)
        {
            inlineSlots++;
        }
        else if (slots[i].extIndex == JS_SCRIPT_DATA)
        {
            dataSlots++;
        }
        else if (slots[i].extIndex >= 0)
        {
            extSlots++;
        }
    }
    CHECK(inlineSlots == 1 && extSlots == 1 && dataSlots == 3,
          "slot kinds: 1 inline, 1 ext, 3 data:");
    CHECK(slots[0].extIndex == -1 || !slots[0].isExt,
          "slot order preserved (inline first)");
    CHECK(slots[1].extIndex == JS_SCRIPT_DATA &&
              slots[1].inlineLen > 0 &&
              memcmp(slots[1].inlineStart, "data:", 5) == 0,
          "data: slot points at the src VALUE span in the page");

    /* ── 3: decoder correctness ──────────────────────────────────────────── */
    char *d1 = jsext_decode_data_script("data:text/javascript,window.__plainRan=1;",
                                        strlen("data:text/javascript,window.__plainRan=1;"));
    CHECK(d1 && strcmp(d1, "window.__plainRan=1;") == 0, "decode: plain payload");
    free(d1);    char *d2 = jsext_decode_data_script(
        "DATA:APPLICATION/JAVASCRIPT;base64,d2luZG93Ll9fYjY0UmFuPTE7",
        strlen("DATA:APPLICATION/JAVASCRIPT;base64,d2luZG93Ll9fYjY0UmFuPTE7"));
    CHECK(d2 && strcmp(d2, "window.__b64Ran=1;") == 0,
          "decode: base64 + case-insensitive metadata");
    free(d2);

    char *d3 = jsext_decode_data_script("data:,window.x%3D1%3B", 21);
    CHECK(d3 && strcmp(d3, "window.x=1;") == 0, "decode: percent-escaped");
    free(d3);

    char *d4 = jsext_decode_data_script("data:,window.plus+plus=1;", 25);
    CHECK(d4 && strcmp(d4, "window.plus+plus=1;") == 0,
          "decode: '+' is literal (not form-encoding)");
    free(d4);

    /* ── 4: malformed → NULL ──────────────────────────────────────────────── */
    char *m1 = jsext_decode_data_script("data-text-javascript-nocomma", 28);
    CHECK(m1 == NULL, "malformed: no comma");
    free(m1);
    char *m2 = jsext_decode_data_script("data:,", 6);
    CHECK(m2 == NULL, "malformed: empty payload");
    free(m2);
    char *m3 = jsext_decode_data_script("data:;base64,QUJD", 17);
    CHECK(m3 && strcmp(m3, "ABC") == 0, "decode: base64 ABC");
    free(m3);
    char *m4 = jsext_decode_data_script("data:;base64,QQ", 15);
    CHECK(m4 == NULL, "malformed: dangling 2-char base64 quad");
    free(m4);
    char *m5 = jsext_decode_data_script("data:;base64,QU*D", 17);
    CHECK(m5 == NULL, "malformed: non-alphabet base64 char");
    free(m5);

    /* ── 5: the big payload decodes intact ────────────────────────────────── */
    char *big = jsext_decode_data_script(slots[4].inlineStart,
                                         slots[4].inlineLen);
    CHECK(big && strlen(big) == strlen(bigPayload) &&
              strstr(big, "window.__bigDataRan=1;") != NULL,
          "decode: 600-byte payload intact (no 512B truncation)");
    free(big);

    jsext_arena_free(arena);
    printf("dataurl suite: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
