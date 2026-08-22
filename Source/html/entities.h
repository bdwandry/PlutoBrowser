// entities.h — HTML entity decoder & UTF-8 sanitizer
// (C port of html/entities.lua).
//
// Phase P06. decode() mirrors the source pipeline:
//   fast path (no '&' and no byte >=0x80 -> verbatim copy),
//   1. decimal numeric entities   &#123;
//   2. hex numeric entities       &#x1F;   (asymmetric: fewer specials)
//   3. named entities             &name;   (%a+ only — &frac12; does NOT match)
//   early return when no high bytes were present,
//   4. fixed UTF-8 sequence rewrites,
//   5. transliteration map + final byte loop (2-byte cp decode, 3+ -> " ").
// Ownership: returned string is fresh pluto_malloc, NUL-terminated;
// outLen gets the real byte length.
#ifndef PLUTO_ENTITIES_H
#define PLUTO_ENTITIES_H

#include <stddef.h>

struct PlaydateAPI;

void entities_init(struct PlaydateAPI* pd);

char* entities_decode(const char* text, size_t len, size_t* outLen);
char* entities_encode(const char* text, size_t len, size_t* outLen);

#endif // PLUTO_ENTITIES_H
