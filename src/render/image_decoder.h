#ifndef PLUTO_RENDER_IMAGE_DECODER_H
#define PLUTO_RENDER_IMAGE_DECODER_H

#include <stddef.h>
#include <stdint.h>

struct PlaydateAPI;
struct LCDBitmap;

/* C port of Source/render/image_decoder.lua (ImageDecoder).
 *
 * Sequential image pipeline: a FIFO download queue drained one URL at a
 * time through HttpClient while the main page is NOT loading, decoded via
 * magic-byte dispatch, cached as url -> LCDBitmap* | failed-sentinel.
 *
 * Parity notes vs the Lua original:
 *  - JPEG/PNG/WebP/GIF decode asynchronously through the task scheduler
 *    (isDecoding gates the queue until done); BMP/ICO/SVG decode inline.
 *  - Failed decodes AND short (<9 byte) bodies AND transport errors are all
 *    cached as false so they are never re-fetched.
 *  - processNextImage skips URLs already present in cache (success OR
 *    failure) by draining them from the queue head immediately.
 *  - The Lua 16 ms re-kick timer after each completion is redundant given
 *    update() polls every frame with the same idle condition, so it is not
 *    emulated; the queue advances at the same frame boundary either way.
 *  - id_draw mirrors ImageDecoder.draw including the placeholder card,
 *    hatch pattern, camera icon, clipped alt text and selection border.
 *    href is accepted but unused (faithful to the source).
 */

void id_init(struct PlaydateAPI* pd);

void id_clear_cache(void);
void id_update(void);   /* call once per frame AFTER hc_update() */

void id_enqueue(const char* src);
void id_evict(const char* src);

int  id_is_cached(const char* src);   /* decoded or failed */
int  id_is_decoded(const char* src);  /* successfully decoded */
struct LCDBitmap* id_get_image(const char* src);

void id_draw(int x, int y, int w, int h,
             const char* altText, const char* href,
             int isSelected, const char* src);

/* Test hook (Lua _testDecode): synchronous dispatch for the inline formats
 * (BMP/ICO/SVG/garbage). Async formats return NULL immediately; pump
 * tasks_update() to drive them, but note the callback does not touch the
 * cache -- it only delivers the bitmap here. */
struct LCDBitmap* id_test_decode(const uint8_t* data, size_t len);

#endif
