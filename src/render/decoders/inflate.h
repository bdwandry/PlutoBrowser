#ifndef PLUTO_RENDER_DECODERS_INFLATE_H
#define PLUTO_RENDER_DECODERS_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* C port of Source/render/decoders/inflate.lua (Inflate).
 *
 * Pure DEFLATE/zlib decompressor. LSB-first bit stream, canonical Huffman
 * tables, stored/fixed/dynamic blocks. A zlib CMF/FLG header is detected
 * and skipped (including preset dictionary); raw deflate streams decode
 * as-is. Faithful quirks preserved:
 *  - btype==3 blocks fall through silently (loop re-reads headers)
 *  - match copies that reach before the start of output emit 0 bytes
 *  - truncated input returns whatever was decoded so far
 */

/* One-shot: decodes data[0..len) into a freshly pluto_malloc'd buffer.
 * Returns bytes written and sets *out; returns -1 only when len < 2
 * (mirrors Lua nil). Output may be shorter than expected on truncation. */
long inflate_decompress(const uint8_t* data, size_t len,
                        uint8_t** out, size_t* outLen);

typedef struct InflateStream InflateStream;

/* Streaming decoder keeping a sliding window so callers can consume
 * output in chunks (PNG row streaming). NULL on OOM / len < 2. */
InflateStream* inflate_stream_new(const uint8_t* data, size_t len);

/* Copies up to n decoded bytes into buf; returns count, 0 == end of
 * stream (Lua nil). */
size_t inflate_stream_read(InflateStream* s, uint8_t* buf, size_t n);

void inflate_stream_free(InflateStream* s);

#endif
