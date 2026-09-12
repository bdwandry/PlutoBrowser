/*
 * PlutoBrowser — image_decoder.h
 * Port of Source/render/image_decoder.lua (reference, 308 lines).
 *
 * Lua → C function map:
 *   decodeRawImageData(data,url,onDone)  → imgdec_decode_raw (static; the
 *       task/HTTP layers play the coroutine/callback roles)
 *   processNextImage()                   → process_next (static)
 *   ImageDecoder.clearCache()            → imgdec_clear_cache()
 *   ImageDecoder.update()                → imgdec_update()  (call once per frame)
 *   ImageDecoder.enqueue(src)            → imgdec_enqueue()
 *   ImageDecoder.evict(src)              → imgdec_evict()
 *   ImageDecoder.isDecoded(src)          → imgdec_is_decoded()
 *   ImageDecoder.getImage(src)           → imgdec_get_image()
 *   ImageDecoder.isCached(src)           → imgdec_is_cached()
 *   ImageDecoder.draw(...)               → imgdec_draw()
 *   ImageDecoder._testDecode(data)       → imgdec_test_decode()
 *
 * Preserved semantics:
 *   - Format dispatch by magic: FFD8 JPEG, \x89PNG, RIFF..WEBP, GIF87a/89a
 *     → async decode via the cooperative task scheduler (isDecoding flag);
 *     "BM" BMP, ICO/CUR (reserved 0, type 1/2), SVG (<svg/<?xml in the first
 *     200 bytes, lowercased) → synchronous pcall-style decode (NULL on error).
 *   - Sequential download queue with dedupe; images only download while the
 *     main page is NOT loading (HttpClient.isLoading gate); 16ms timer
 *     re-schedule between queue items; cache url → bitmap | false; enqueue
 *     skipped when cached; draw() of a nil-cached src enqueues it.
 *   - update() stall recovery: a download whose HTTP client went idle (cancel
 *     via navigation) or a decode task that is no longer running (cancelled)
 *     releases its busy flag, exactly like the reference.
 *   - draw() placeholder card: white round-rect + black border, hatch lines
 *     every 10px, camera icon, alt text (truncated to 23+"...") when h > 30,
 *     selected state draws a 2px round rect. Cached images drawScaled to fit,
 *     centered; max box 360×180, min 40×20.
 *
 * C design note: the reference spread large JPEG/PNG/GIF/WebP decodes across
 * frames via coroutine yields INSIDE the decode loops. The C decoders keep
 * the yield sites as documented call-site comments (the task layer owns the
 * frame budget) and decode monolithically inside one task step — the same
 * architecture the P24–P29 batteries verified on the physical device. The
 * observable contract (isDecoding flag, completion callback, queue pacing,
 * stall recovery) is preserved exactly.
 */
#ifndef PLUTO_IMAGE_DECODER_H
#define PLUTO_IMAGE_DECODER_H

#include "pd_api.h"

/* Initialize (stores the API pointer). Call once at boot. */
void imgdec_init(PlaydateAPI *pd);

/* Drop every cached image/queue entry and reset the busy flags. */
void imgdec_clear_cache(void);

/* Pump the download queue / recover stalls. Call once per frame. */
void imgdec_update(void);

/* Enqueue a URL for download+decode (deduped; skipped when cached). */
void imgdec_enqueue(const char *src);

/* Evict one cache entry (frees the bitmap). NULL-safe. */
void imgdec_evict(const char *src);

/* 1 when a successfully decoded bitmap is cached for src. */
int imgdec_is_decoded(const char *src);

/* The cached bitmap for src, or NULL (failed/absent). */
LCDBitmap *imgdec_get_image(const char *src);

/* 1 when src has ANY cache entry (decoded or failed). */
int imgdec_is_cached(const char *src);

/* Placeholder/cached-image painter (see header docs above).
 * altText/href may be NULL; isSelected nonzero draws the focus ring. */
void imgdec_draw(int x, int y, int w, int h, const char *altText,
                 const char *href, int isSelected, const char *src);

/* Test hook: run a buffer through the format dispatch synchronously and
 * return the decoded bitmap (or NULL). Mirrors ImageDecoder._testDecode. */
LCDBitmap *imgdec_test_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_IMAGE_DECODER_H */
