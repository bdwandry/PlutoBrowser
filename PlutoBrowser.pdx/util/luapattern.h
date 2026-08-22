// luapattern.h — Lua-pattern matching engine (subset used by CometBrowser).
//
// Faithful port of the Lua 5.4 lstrlib matcher, adapted to explicit lengths
// so subjects may contain embedded NUL bytes. Supported constructs:
//   classes %a %c %d %g %l %p %s %u %w %x (+ uppercase complements), '.'
//   sets [...] with ranges, negation, embedded classes, escaped chars
//   anchors ^ $, quantifiers * + - ?, captures (...), position captures ()
//   balanced match %bxy, frontier %f[set]
//
// Deviation from Lua: malformed patterns / depth overflow return an error
// code instead of raising; callers treat as no-match.

#ifndef PLUTO_LUAPATTERN_H
#define PLUTO_LUAPATTERN_H

#include <stddef.h>

#include "strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LP_MAXCAPTURES 32

typedef struct {
    size_t start;      // byte offset into subject
    size_t len;        // byte length; 0 for position captures
    int    isPosition; // 1 if this capture is a position () capture
} LPCap;

#define LP_ERR_NOMATCH   (-1) // no match found
#define LP_ERR_PATTERN   (-2) // malformed pattern or depth overflow

// string.find semantics without the plain flag.
// init: 0-based start offset. On success fills *outStart/*outEnd as
// [start,end) offsets of the whole match and returns 1; else returns 0
// (LP_ERR_PATTERN on malformed pattern).
int lp_find(const char* s, size_t slen, const char* pat, size_t patlen,
            size_t init, size_t* outStart, size_t* outEnd);

// string.find(s, pat, init, plain=true) — literal substring search.
int lp_find_plain(const char* s, size_t slen, const char* pat, size_t patlen,
                  size_t init, size_t* outStart, size_t* outEnd);

// string.match semantics: on success returns number of captures filled
// (>=1; if the pattern has no captures, one capture spanning the whole
// match is returned, mirroring Lua), fills caps[0..*outCapCount-1].
// Returns LP_ERR_NOMATCH or LP_ERR_PATTERN on failure.
int lp_match(const char* s, size_t slen, const char* pat, size_t patlen,
             size_t init, LPCap caps[], int* outCapCount);

// string.gmatch iterator.
typedef struct {
    const char* s;
    size_t slen;
    const char* pat;
    size_t patlen;
    size_t pos;       // next scan start
    size_t lastEnd;   // end of previous match (empty-match guard)
} LPGMatch;

void lp_gmatch_init(LPGMatch* it, const char* s, size_t slen,
                    const char* pat, size_t patlen);
// Returns 1 and fills caps/outCapCount on each match; 0 when exhausted.
// Whole-match span is caps[0] when the pattern has no captures; otherwise
// caps[] holds only the explicit captures (Lua behavior).
int lp_gmatch_next(LPGMatch* it, LPCap caps[], int* outCapCount);

// Replacement callback for lp_gsub. Receives the subject plus captures
// (caps[0] = whole match when pattern has no captures). Append the
// replacement text to out and return 1; return 0 to keep the original
// match text (Lua nil/false).
typedef int (*LPGsubFn)(void* userdata, const char* subj, size_t subjLen,
                        const LPCap caps[], int ncap, StrBuf* out);

// string.gsub semantics. repl/replLen is a replacement TEMPLATE supporting
// %0..%9 and %%; pass fn != NULL to use the callback instead (template is
// ignored). maxN limits replacements (SIZE_MAX = unlimited). Returns the
// number of substitutions performed, or LP_ERR_PATTERN.
int lp_gsub(const char* s, size_t slen, const char* pat, size_t patlen,
            const char* repl, size_t replLen, LPGsubFn fn, void* userdata,
            size_t maxN, StrBuf* out);

// Extract capture i as a byte range into subj. Returns 1 on success.
int lp_cap_span(const char* subj, const LPCap caps[], int ncaps, int i,
                size_t* outStart, size_t* outLen);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_LUAPATTERN_H
