// luapattern.c — Lua 5.4 pattern matcher port (see header for scope).
//
// Structure mirrors lstrlib.c: class_end / match_class / matchbracketclass /
// singlematch / max_expand / min_expand / captures / %b / %f / do_match,
// then public wrappers (find, find_plain, match, gmatch, gsub).
//
// Depth accounting is centralized in do_match(); all other helpers assume
// they run inside its bracket. Subjects and patterns carry explicit lengths
// (NUL-safe). Malformed patterns set ms->err instead of raising.

#include "luapattern.h"

#include <stdarg.h>
#include <string.h>

#include "../core/logger.h"

#define L_ESC '%'
#define MAXCCALLS 200
#define CAP_UNFINISHED ((ptrdiff_t)-1)
#define CAP_POSITION   ((ptrdiff_t)-2)

typedef struct {
    const char* src;  // subject start
    const char* send; // subject end
    const char* pend; // pattern end
    size_t level;     // number of captures (0..LP_MAXCAPTURES)
    struct {
        const char* init;
        ptrdiff_t len; // >=0 length; CAP_UNFINISHED; CAP_POSITION
    } capture[LP_MAXCAPTURES];
    int depth;
    int err;          // set on malformed pattern / depth overflow
} MatchState;

// ---- ASCII-only ctype (locale-independent, unsigned-char safe) ----

static int lp_isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int lp_islower(int c)  { return c >= 'a' && c <= 'z'; }
static int lp_isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static int lp_isdigit(int c)  { return c >= '0' && c <= '9'; }
static int lp_isxdigit(int c) { return lp_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int lp_isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static int lp_iscntrl(int c)  { return (c >= 0 && c <= 0x1f) || c == 0x7f; }
static int lp_ispunct(int c)  { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
static int lp_isgraph(int c)  { return c > 32 && c < 127; }
static int lp_isalnum(int c)  { return lp_isalpha(c) || lp_isdigit(c); }

static int lp_tolower(int c)
{
    return lp_isupper(c) ? (c - 'A' + 'a') : c;
}

static int match_class(int c, int cl)
{
    int res;
    switch (lp_tolower(cl)) {
        case 'a': res = lp_isalpha(c); break;
        case 'c': res = lp_iscntrl(c); break;
        case 'd': res = lp_isdigit(c); break;
        case 'g': res = lp_isgraph(c); break;
        case 'l': res = lp_islower(c); break;
        case 'p': res = lp_ispunct(c); break;
        case 's': res = lp_isspace(c); break;
        case 'u': res = lp_isupper(c); break;
        case 'w': res = lp_isalnum(c); break;
        case 'x': res = lp_isxdigit(c); break;
        default:  return cl == c;
    }
    if (lp_isupper(cl)) {
        res = !res;
    }
    return res;
}

static void pattern_error(MatchState* ms, const char* msg)
{
    if (!ms->err) {
        PLUTO_ERROR("luapattern: %s", msg);
    }
    ms->err = 1;
}

static const char* class_end(MatchState* ms, const char* p)
{
    if (p == ms->pend) {
        pattern_error(ms, "pattern ends unexpectedly");
        return NULL;
    }
    if (*p == L_ESC) {
        p++;
        if (p == ms->pend) {
            pattern_error(ms, "malformed pattern (ends with '%')");
            return NULL;
        }
        return p + 1;
    }
    if (*p == '[') {
        if (++p == ms->pend) {
            pattern_error(ms, "malformed pattern (missing ])"); 
            return NULL;
        }
        if (*p == '^') {
            p++;
        }
        do { // look for a ']'
            if (p == ms->pend) {
                pattern_error(ms, "malformed pattern (missing ])");
                return NULL;
            }
            if (*p == L_ESC) {
                p++;
                if (p == ms->pend) {
                    pattern_error(ms, "malformed pattern (ends with '%')");
                    return NULL;
                }
            }
            p++;
        } while (p != ms->pend && *p != ']');
        if (p == ms->pend) {
            pattern_error(ms, "malformed pattern (missing ])");
            return NULL;
        }
        return p + 1;
    }
    return p + 1;
}

static int match_bracket_class(int c, const char* p, const char* ec)
{
    int sig = 1;
    if (*(p + 1) == '^') {
        sig = 0;
        p++; // skip the '^'
    }
    while (++p < ec) {
        if (*p == L_ESC) {
            p++;
            if (match_class(c, (unsigned char)*p)) {
                return sig;
            }
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            // After p+=2: *(p-2) is range start, *p is range end.
            if ((unsigned char)*(p - 2) <= c && c <= (unsigned char)*p) {
                return sig;
            }
        } else if ((unsigned char)*p == c) {
            return sig;
        }
    }
    return !sig;
}

static int single_match(MatchState* ms, const char* s, const char* p,
                        const char* ep)
{
    if (s >= ms->send) {
        return 0;
    }
    {
        int c = (unsigned char)*s;
        switch (*p) {
            case '.': return 1;
            case L_ESC: return match_class(c, (unsigned char)*(p + 1));
            case '[': return match_bracket_class(c, p, ep - 1);
            default: return (unsigned char)*p == c;
        }
    }
}

static const char* match_balance(MatchState* ms, const char* s, const char* p)
{
    // Need two argument chars (x at p, y at p+1): reject only when p+1 is
    // past the end (lstrlib: p >= pend - 1).
    if (p + 2 > ms->pend) {
        pattern_error(ms, "malformed %b pattern");
        return NULL;
    }
    if (s >= ms->send || *s != *p) {
        return NULL;
    }
    {
        int b = (unsigned char)*p;
        int e = (unsigned char)*(p + 1);
        int cont = 1;
        while (++s < ms->send) {
            if (*s == e) {
                if (--cont == 0) {
                    return s + 1;
                }
            } else if (*s == b) {
                cont++;
            }
        }
    }
    return NULL;
}

static const char* do_match(MatchState* ms, const char* s, const char* p);

static const char* max_expand(MatchState* ms, const char* s, const char* p,
                              const char* ep)
{
    ptrdiff_t i = 0;
    while (single_match(ms, s + i, p, ep)) {
        i++;
    }
    while (i >= 0) { // backtrack from greediest
        const char* res = do_match(ms, s + i, ep + 1);
        if (res != NULL) {
            return res;
        }
        i--;
    }
    return NULL;
}

static const char* min_expand(MatchState* ms, const char* s, const char* p,
                              const char* ep)
{
    while (1) {
        const char* res = do_match(ms, s, ep + 1);
        if (res != NULL) {
            return res;
        }
        if (single_match(ms, s, p, ep)) {
            s++;
        } else {
            return NULL;
        }
    }
}

static const char* do_match_inner(MatchState* ms, const char* s, const char* p);

static const char* do_match(MatchState* ms, const char* s, const char* p)
{
    const char* r;
    if (ms->depth >= MAXCCALLS) {
        pattern_error(ms, "pattern too complex");
        return NULL;
    }
    ms->depth++;
    r = do_match_inner(ms, s, p);
    ms->depth--;
    return r;
}

static const char* do_match_inner(MatchState* ms, const char* s, const char* p)
{
    while (1) {
        if (p == ms->pend) {
            return s;
        }
        switch (*p) {
            case '(': { // start capture
                const char* r;
                int isPos = (*(p + 1) == ')');
                if (ms->level >= LP_MAXCAPTURES) {
                    pattern_error(ms, "too many captures");
                    return NULL;
                }
                ms->capture[ms->level].init = s;
                ms->capture[ms->level].len = isPos ? CAP_POSITION : CAP_UNFINISHED;
                ms->level++;
                r = do_match(ms, s, p + (isPos ? 2 : 1));
                if (r == NULL) {
                    ms->level--;
                }
                return r;
            }
            case ')': { // end capture
                int l = -1;
                size_t i;
                const char* r;
                for (i = ms->level; i >= 1; i--) {
                    if (ms->capture[i - 1].len == CAP_UNFINISHED) {
                        l = (int)(i - 1);
                        break;
                    }
                }
                if (l < 0) {
                    pattern_error(ms, "invalid pattern capture");
                    return NULL;
                }
                ms->capture[l].len = s - ms->capture[l].init;
                r = do_match(ms, s, p + 1);
                if (r == NULL) {
                    ms->capture[l].len = CAP_UNFINISHED;
                }
                return r;
            }
            case '$':
                if (p + 1 == ms->pend) {
                    return (s == ms->send) ? s : NULL;
                }
                goto dflt;
            case L_ESC:
                switch (*(p + 1)) {
                    case 'b': { // balanced match
                        const char* r = match_balance(ms, s, p + 2);
                        if (r == NULL) {
                            return NULL;
                        }
                        s = r;
                        p += 4;
                        continue;
                    }
                    case 'f': { // frontier
                        const char* ep2;
                        char prev;
                        p += 2;
                        if (p == ms->pend || *p != '[') {
                            pattern_error(ms, "missing [ after %f");
                            return NULL;
                        }
                        ep2 = class_end(ms, p);
                        if (ep2 == NULL) {
                            return NULL;
                        }
                        prev = (s == ms->src) ? '\0' : *(s - 1);
                        if (!match_bracket_class((unsigned char)prev, p, ep2 - 1) &&
                            match_bracket_class(
                                (unsigned char)(s == ms->send ? '\0' : *s),
                                p, ep2 - 1)) {
                            p = ep2;
                            continue;
                        }
                        return NULL;
                    }
                    default: {
                        if (lp_isdigit((unsigned char)*(p + 1))) { // capture reference
                            int l = *(p + 1) - '1';
                            size_t len;
                            if (l < 0 || (size_t)l >= ms->level ||
                                ms->capture[l].len == CAP_UNFINISHED) {
                                pattern_error(ms, "invalid capture index");
                                return NULL;
                            }
                            len = (size_t)ms->capture[l].len;
                            if (ms->send - s >= (ptrdiff_t)len &&
                                memcmp(ms->capture[l].init, s, len) == 0) {
                                p += 2;
                                s += len;
                                continue;
                            }
                            return NULL;
                        }
                        goto dflt;
                    }
                }
            default:
            dflt: { // pattern item plus optional quantifier
                const char* ep2 = class_end(ms, p);
                if (ep2 == NULL) {
                    return NULL;
                }
                if (!single_match(ms, s, p, ep2)) {
                    if (ep2 != ms->pend &&
                        (*ep2 == '?' || *ep2 == '*' || *ep2 == '-')) {
                        p = ep2 + 1;
                        continue;
                    }
                    return NULL;
                }
                switch (ep2 != ms->pend ? *ep2 : 0) {
                    case '?': { // optional (prefer one occurrence)
                        const char* r = do_match(ms, s + 1, ep2 + 1);
                        if (r != NULL) {
                            return r;
                        }
                        p = ep2 + 1;
                        continue;
                    }
                    case '+':
                        return max_expand(ms, s + 1, p, ep2);
                    case '*':
                        return max_expand(ms, s, p, ep2);
                    case '-':
                        return min_expand(ms, s, p, ep2);
                    default:
                        s++;
                        p = ep2;
                        continue;
                }
            }
        }
    }
}

// Run one match attempt sequence starting at subject offset `start`,
// replicating Lua's find/match scan loop ('^' anchors to `start` only).
// Returns 1 with *outStart/*outEnd set to the [start,end) of the whole
// match, 0 on no-match, LP_ERR_PATTERN on bad pattern.
static int run_match(MatchState* ms, const char* s, size_t slen,
                     const char* pat, size_t patlen, size_t start,
                     const char** outStart, const char** outEnd)
{
    const char* sp = s + start;
    int anchored = 0;

    ms->src = s;
    ms->send = s + slen;
    ms->pend = pat + patlen;
    ms->level = 0;
    ms->depth = 0;
    ms->err = 0;

    if (patlen > 0 && pat[0] == '^') {
        anchored = 1;
        pat++;
    }
    do {
        const char* e;
        ms->level = 0;
        ms->depth = 0;
        ms->err = 0;
        e = do_match(ms, sp, pat);
        if (e != NULL) {
            *outStart = sp;
            *outEnd = e;
            return 1;
        }
        if (ms->err) {
            return LP_ERR_PATTERN;
        }
        sp++;
    } while (sp <= ms->send && !anchored);
    return 0;
}

// Convert engine captures to public spans. With zero explicit captures,
// caps[0] becomes the whole match (Lua push_captures behavior).
static void fill_caps(const MatchState* ms, const char* mStart,
                      const char* mEnd, LPCap caps[], int* outCount)
{
    int i;
    int n = (int)ms->level;

    if (n == 0) {
        caps[0].start = (size_t)(mStart - ms->src);
        caps[0].len = (size_t)(mEnd - mStart);
        caps[0].isPosition = 0;
        *outCount = 1;
        return;
    }
    for (i = 0; i < n; i++) {
        caps[i].isPosition = (ms->capture[i].len == CAP_POSITION);
        caps[i].start = (size_t)(ms->capture[i].init - ms->src);
        caps[i].len = (ms->capture[i].len > 0) ? (size_t)ms->capture[i].len : 0;
    }
    *outCount = n;
}

int lp_find(const char* s, size_t slen, const char* pat, size_t patlen,
            size_t init, size_t* outStart, size_t* outEnd)
{
    MatchState ms;
    const char* mStart;
    const char* end;
    int r;

    if (init > slen) {
        return 0;
    }
    r = run_match(&ms, s, slen, pat, patlen, init, &mStart, &end);
    if (r == 1) {
        *outStart = (size_t)(mStart - s);
        *outEnd = (size_t)(end - s);
        return 1;
    }
    return (r == LP_ERR_PATTERN) ? LP_ERR_PATTERN : 0;
}

int lp_find_plain(const char* s, size_t slen, const char* pat, size_t patlen,
                  size_t init, size_t* outStart, size_t* outEnd)
{
    if (patlen == 0) {
        *outStart = init;
        *outEnd = init;
        return 1;
    }
    if (init > slen || slen - init < patlen) {
        return 0;
    }
    {
        const char* last = s + slen - patlen;
        const char* p = s + init;
        for (; p <= last; p++) {
            if (*p == pat[0] && memcmp(p, pat, patlen) == 0) {
                *outStart = (size_t)(p - s);
                *outEnd = *outStart + patlen;
                return 1;
            }
        }
    }
    return 0;
}

int lp_match(const char* s, size_t slen, const char* pat, size_t patlen,
             size_t init, LPCap caps[], int* outCapCount)
{
    MatchState ms;
    const char* mStart;
    const char* end;
    int r;

    if (init > slen) {
        return LP_ERR_NOMATCH;
    }
    r = run_match(&ms, s, slen, pat, patlen, init, &mStart, &end);
    if (r == 1) {
        fill_caps(&ms, mStart, end, caps, outCapCount);
        return *outCapCount;
    }
    return (r == LP_ERR_PATTERN) ? LP_ERR_PATTERN : LP_ERR_NOMATCH;
}

void lp_gmatch_init(LPGMatch* it, const char* s, size_t slen,
                    const char* pat, size_t patlen)
{
    it->s = s;
    it->slen = slen;
    it->pat = pat;
    it->patlen = patlen;
    it->pos = 0;
    it->lastEnd = 0;
}

int lp_gmatch_next(LPGMatch* it, LPCap caps[], int* outCapCount)
{
    // Mirrors gmatch_aux: scan forward from it->pos; accept the first match
    // whose end differs from the previous match end (empty-match guard).
    // Note: unlike find/match/gsub, gmatch does NOT strip a leading '^'
    // (Lua treats it as a literal there too).
    while (it->pos <= it->slen) {
        MatchState ms;
        const char* sp = it->s + it->pos;
        const char* e;

        ms.src = it->s;
        ms.send = it->s + it->slen;
        ms.pend = it->pat + it->patlen;
        ms.level = 0;
        ms.depth = 0;
        ms.err = 0;

        e = do_match(&ms, sp, it->pat);
        if (e != NULL) {
            size_t endOff = (size_t)(e - it->s);
            if (endOff != it->lastEnd) {
                fill_caps(&ms, sp, e, caps, outCapCount);
                it->pos = endOff;
                it->lastEnd = endOff;
                return 1;
            }
        } else if (ms.err) {
            return 0;
        }
        it->pos++;
    }
    return 0;
}

int lp_cap_span(const char* subj, const LPCap caps[], int ncaps, int i,
                size_t* outStart, size_t* outLen)
{
    (void)subj;
    if (caps == NULL || i < 0 || i >= ncaps) {
        return 0;
    }
    *outStart = caps[i].start;
    *outLen = caps[i].len;
    return 1;
}

// Append template expansion for match [src, e) using ms captures.
static int append_replacement(const MatchState* ms, const char* src,
                              const char* e, const char* repl, size_t replLen,
                              StrBuf* out)
{
    size_t i = 0;
    while (i < replLen) {
        char c = repl[i];
        if (c == L_ESC && i + 1 < replLen) {
            char nx = repl[i + 1];
            if (nx == L_ESC) {
                if (!sb_append_char(out, L_ESC)) return 0;
                i += 2;
                continue;
            }
            if (nx >= '0' && nx <= '9') {
                // Lua template semantics: %0 = whole match, %1..%9 =
                // capture[N-1] (1-based numbering over the capture array).
                if (nx == '0') {
                    if (!sb_append(out, src, (size_t)(e - src))) return 0;
                } else {
                    size_t idx = (size_t)(nx - '1');
                    if (idx < ms->level) {
                        if (ms->capture[idx].len == CAP_POSITION) {
                            if (!sb_printf(out, "%u",
                                           (unsigned)(ms->capture[idx].init - ms->src)))
                                return 0;
                        } else {
                            if (!sb_append(out, ms->capture[idx].init,
                                           (size_t)ms->capture[idx].len)) return 0;
                        }
                    }
                }
                i += 2;
                continue;
            }
            if (!sb_append(out, repl + i, 2)) return 0; // unknown escape: literal
            i += 2;
            continue;
        }
        if (!sb_append_char(out, c)) return 0;
        i++;
    }
    return 1;
}

int lp_gsub(const char* s, size_t slen, const char* pat, size_t patlen,
            const char* repl, size_t replLen, LPGsubFn fn, void* userdata,
            size_t maxN, StrBuf* out)
{
    MatchState ms;
    const char* src = s;
    const char* lastmatch = NULL;
    const char* p = pat;
    size_t plen = patlen;
    int anchored = 0;
    size_t n = 0;

    if (plen > 0 && p[0] == '^') {
        anchored = 1;
        p++;
        plen--;
    }

    while (n < maxN) {
        const char* e;

        ms.src = s;
        ms.send = s + slen;
        ms.pend = p + plen;
        ms.level = 0;
        ms.depth = 0;
        ms.err = 0;

        e = do_match(&ms, src, p);
        if (e != NULL && e != lastmatch) {
            LPCap caps[LP_MAXCAPTURES];
            int ncaps;
            n++;
            fill_caps(&ms, src, e, caps, &ncaps);
            if (fn != NULL) {
                if (!fn(userdata, s, slen, caps, ncaps, out)) {
                    if (!sb_append(out, src, (size_t)(e - src))) return LP_ERR_PATTERN;
                }
            } else {
                if (!append_replacement(&ms, src, e, repl, replLen, out))
                    return LP_ERR_PATTERN;
            }
            src = e;
            lastmatch = e;
        } else if (src < s + slen) {
            if (!sb_append_char(out, *src++)) return LP_ERR_PATTERN;
        } else {
            break;
        }
        if (anchored) {
            break;
        }
    }
    if (!sb_append(out, src, (size_t)((s + slen) - src))) return LP_ERR_PATTERN;
    return (int)n;
}
