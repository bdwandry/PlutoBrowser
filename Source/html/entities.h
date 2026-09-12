/*
 * PlutoBrowser — entities.h
 * HTML Entity Decoder & UTF-8 Sanitizer (port of Source/html/entities.lua).
 *
 * Lua reference behavior preserved exactly:
 *  - decode(text): fast path returns the input unchanged when it contains
 *    neither '&' nor any byte >= 0x80; then sequential passes mirroring the
 *    Lua gsub chain:
 *      1. decimal  numeric entities  &#123;
 *      2. hex      numeric entities  &#x1F; / &#X1F;
 *      3. named    entities          &name;  (unknown → " name ")
 *      4. UTF-8 multi-byte sequence cleanup (BOM, dashes, quotes, math…)
 *      5. transliterate 2-byte Latin sequences via the codepoint table and
 *         replace everything else non-ASCII with ' '
 *    Passes run in sequence (not one merged scan) so double-encoded input
 *    such as "&#38;amp;" cascades to "&" exactly like the reference.
 *  - encode(text): inverse escaping for embedding text in markup:
 *    & → &amp;  < → &lt;  > → &gt;  " → &quot;
 *
 * Both functions return a heap string owned by the caller (SDK allocator;
 * free with pluto_free).
 */
#ifndef PLUTO_ENTITIES_H
#define PLUTO_ENTITIES_H

/* Caller frees the result with pluto_free(). Never returns NULL except on
 * catastrophic allocation failure (empty string otherwise). */
char *entities_decode(const char *text);

/* Caller frees the result with pluto_free(). */
char *entities_encode(const char *text);

#endif /* PLUTO_ENTITIES_H */
