/*
 * PlutoBrowser — inflate.h
 * Port of Source/render/decoders/inflate.lua (reference, 427 lines).
 *
 * Lua → C function map:
 *   createBitStream + bs:readBits/alignByte → inf_bitstream (static)
 *   buildHuffmanTable / decodeSymbol        → inf_build_table / inf_decode_symbol
 *   getFixedTables                          → inf_fixed_tables (static, cached)
 *   Inflate.decompress(data)                → inflate_decompress()
 *   Inflate.createStream(data) + s:read(n)  → inflate_stream_new()/inflate_stream_read()
 *   Tasks.yieldCheck()                      → document deviation below
 *
 * Deviations (documented, behavior-preserving):
 *   - Output is a heap byte buffer (caller frees via free()), not a Lua
 *     string built through string.char chunking.
 *   - Tasks.yieldCheck() call sites are preserved as comments; the one-shot
 *     decompress is called from task step functions that already yield per
 *     frame at a higher level (tasks module gives the same frame budgeting).
 *   - The streaming window is a 64KB ring (Lua compacts its table at >65536
 *     entries down to the last 32768 — identical observable output).
 *   - Length/dist tables, canonical-Huffman construction (bit-reversed
 *     LSB-first codes, stable-by-symbol order), zlib header autodetect
 *     (CM=8 + (cmf*256+flg)%31==0, preset-dict skip), BFINAL/BTYPE flow,
 *     and all `or 0`/`or 1` fallbacks are preserved exactly.
 */
#ifndef PLUTO_INFLATE_H
#define PLUTO_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* One-shot decompress of a zlib-wrapped OR raw deflate stream.
 * Returns a malloc'd buffer (caller frees) and sets *outLen, or NULL.
 * Mirrors Inflate.decompress including its nil-on-short-input behavior. */
uint8_t *inflate_decompress(const uint8_t *data, size_t len, size_t *outLen);

/* SW2c: raw-deflate variants — NO zlib-container sniff. The HTTP gzip
 * path strips the gzip member header itself (gzip header parse + name
 * fields + 8-byte footer, see http_client.c) and feeds the raw deflate
 * payload here. Using the sniffing entry points on HTTP bodies would
 * false-positive a raw stream as zlib-wrapped ~1/500 of the time
 * ((cmf*256+flg)%31==0) and corrupt the output. */
uint8_t *inflate_decompress_raw(const uint8_t *data, size_t len, size_t *outLen);

/* Streaming inflate (Lua Inflate.createStream). Keeps a 64KB window;
 * read() returns up to *outLen bytes (owned by the stream until the next
 * read) or NULL at end of stream. */
typedef struct InflateStream InflateStream;

InflateStream *inflate_stream_new(const uint8_t *data, size_t len);
InflateStream *inflate_stream_new_raw(const uint8_t *data, size_t len);
const uint8_t *inflate_stream_read(InflateStream *s, size_t want, size_t *outLen);
void inflate_stream_free(InflateStream *s);

#endif /* PLUTO_INFLATE_H */
