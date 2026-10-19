/*
 * PlutoBrowser — jsbridge_bundler.c (R18: general-purpose bundler monolith
 * splitter — see jsbridge_bundler.h for the design contract)
 *
 * Streaming lexical scan, O(n), zero allocation. Recognizes the webpack/
 * rollup production shape
 *
 *     [license comment] (()=>{var <m>={<key>:<expr>,...},<rest>...})()
 *
 * purely structurally: IIFE arrow head, first statement `var <ident> = {`,
 * >= PLUTO_BUNDLER_MIN_ENTRIES `key:expr` pairs at map level, tail after the
 * map close. Anything else — non-arrow IIFE, `!function`, map with getters
 * or spread at map level, truncated file — returns NO_MAP and the caller
 * runs the script unsplit. This file never sees a site name, URL, or
 * allowlist: it fires on STRUCTURE, so every bundler-built site benefits.
 *
 * The lexer mirrors the R16 chunk scanner's state machine (comments,
 * strings, templates with nested ${}, regex-vs-division disambiguation) so
 * the two splitters agree on what "top level" means.
 */
#include <string.h>

#include "core/logger.h"
#include "core/pluto_mem.h"
#include "jsbridge_bundler.h"

/* lexical states (mirrors xcs_* in jsbridge_xs_nr.c) */
enum
{
    BRAW = 0,   /* raw code */
    BLC,        /* line comment */
    BBC,        /* block comment */
    BSTR,       /* ' or " string */
    BTPL,       /* ` template */
    BRE         /* regex literal */
};

typedef struct
{
    const char *src;
    size_t len;
    size_t pos;
    int state;
    char quote;
    int reBracket; /* regex: inside [...] ( / is literal there ) */
} BLex;

static int blex_isidchar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static void blex_init(BLex *x, const char *src, size_t len, size_t pos)
{
    memset(x, 0, sizeof(*x));
    x->src = src;
    x->len = len;
    x->pos = pos;
}

static unsigned char bleek(BLex *x)
{
    if (x->pos >= x->len)
        return 0;
    return (unsigned char)x->src[x->pos];
}

/* Advance one char INSIDE the current non-raw state. Returns 0 at EOF. */
static int blex_step(BLex *x)
{
    if (x->pos >= x->len)
        return 0;
    unsigned char c = (unsigned char)x->src[x->pos];
    switch (x->state)
    {
    case BLC:
        if (c == '\n')
            x->state = BRAW;
        x->pos++;
        break;
    case BBC:
        if (c == '*' && x->pos + 1 < x->len && x->src[x->pos + 1] == '/')
        {
            x->pos += 2;
            x->state = BRAW;
        }
        else
            x->pos++;
        break;
    case BSTR:
        if (c == '\\')
            x->pos += 2;
        else if ((char)c == x->quote)
        {
            x->pos++;
            x->state = BRAW;
        }
        else
            x->pos++;
        break;
    case BTPL:
        if (c == '\\')
            x->pos += 2;
        else if (c == '`')
        {
            x->pos++;
            x->state = BRAW;
        }
        else
            x->pos++; /* ${ } stays inside the template literal token
                         * stream for brace-counting purposes ONLY IF the
                         * template is an operand — webpack minified output
                         * uses plain strings; a template with ${} whose
                         * braces unbalance the map-level count simply fails
                         * the structural checks and NO_MAPs (safe). */
        break;
    case BRE:
        if (c == '\\')
            x->pos += 2;
        else if (c == '[')
        {
            x->reBracket = 1;
            x->pos++;
        }
        else if (c == ']')
        {
            x->reBracket = 0;
            x->pos++;
        }
        else if (c == '/' && !x->reBracket)
        {
            x->pos++;
            x->state = BRAW;
        }
        else
            x->pos++;
        break;
    default: /* BRAW handled by callers */
        x->pos++;
        break;
    }
    return x->pos <= x->len;
}

/* Run the lexer from a raw position until it re-enters raw state (i.e.
 * consume one comment/string/template/regex token). pos must sit on the
 * token's opening char. Returns 1 ok, 0 = unterminated (EOF). */
static int blex_skip_token(BLex *x)
{
    unsigned char c = bleek(x);
    if (c == '/')
    {
        unsigned char d = (x->pos + 1 < x->len) ? (unsigned char)x->src[x->pos + 1] : 0;
        if (d == '/')
        {
            x->state = BLC;
            x->pos += 2;
        }
        else if (d == '*')
        {
            x->state = BBC;
            x->pos += 2;
        }
        else
        {
            x->state = BRE;
            x->reBracket = 0;
            x->pos++;
        }
    }
    else if (c == '"' || c == '\'')
    {
        x->state = BSTR;
        x->quote = (char)c;
        x->pos++;
    }
    else if (c == '`')
    {
        x->state = BTPL;
        x->pos++;
    }
    else
        return 0; /* not a token opener */
    while (x->state != BRAW)
    {
        if (x->pos >= x->len)
            return 0; /* unterminated — structural surprise */
        blex_step(x);
    }
    return 1;
}

/* Skip whitespace + comments from x->pos (raw). Returns 1 if pos advanced
 * to a significant char (or EOF), 0 on unterminated comment. */
/* R20b: whitespace+comments skip WITHOUT moving the cursor — returns the
 * position of the first significant byte at/after p (p itself when no
 * trivia is pending). The tail slicer uses this to prove a candidate cut
 * lands exactly where a fresh statement starts (never mid-construct). */
static size_t blex_space_at(const char *src, size_t len, size_t p)
{
    for (;;)
    {
        while (p < len && (src[p] == ' ' || src[p] == '\t' ||
                           src[p] == '\r' || src[p] == '\n'))
            p++;
        if (p + 1 < len && src[p] == '/' && src[p + 1] == '*')
        {
            size_t i = p + 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
                i++;
            if (i + 1 >= len)
                return p; /* unterminated: let the caller treat as ambiguous */
            p = i + 2;
            continue;
        }
        return p;
    }
}

/* R26f: does the statement starting at/after `at` carry a payload that a
 * depth-1 comma cut would break? var/let/const declarator lists and
 * return/throw/yield expressions continue to their terminating `;` —
 * a `,` inside them is NOT a statement separator. Everything else at
 * depth 1 (assignments, calls, IIFEs) splits cleanly: a top-level comma
 * sequence `A,B,C` rewrites as `A; B; C` with identical evaluation
 * order and no value consumer. Returns 1 = hazardous (no comma cuts). */
static int blx_stmt_hazardous(const char *src, size_t len, size_t at)
{
    static const char *const kw[] = {"var", "let", "const", "return",
                                     "throw", "yield"};
    size_t p = blex_space_at(src, len, at);
    if (p >= len)
        return 0;
    for (unsigned i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
    {
        size_t n = strlen(kw[i]);
        if (p + n <= len && memcmp(src + p, kw[i], n) == 0)
        {
            char nx = (p + n < len) ? src[p + n] : 0;
            int ident = (nx >= 'a' && nx <= 'z') ||
                        (nx >= 'A' && nx <= 'Z') ||
                        (nx >= '0' && nx <= '9') || nx == '_' || nx == '$';
            if (!ident)
                return 1;
        }
    }
    return 0;
}

static int blex_space(BLex *x)
{
    for (;;)
    {
        while (x->pos < x->len)
        {
            unsigned char c = (unsigned char)x->src[x->pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                x->pos++;
            else
                break;
        }
        if (x->pos >= x->len)
            return 1;
        if (x->src[x->pos] == '/')
        {
            unsigned char d = (x->pos + 1 < x->len) ? (unsigned char)x->src[x->pos + 1] : 0;
            if (d == '/' || d == '*')
            {
                if (!blex_skip_token(x))
                    return 0;
                continue;
            }
        }
        return 1;
    }
}

/* Regex-vs-division: after which preceding significant chars does a '/'
 * OPEN a regex? Conservative superset of the ES grammar's restricted
 * productions — when in doubt we treat '/' as division ONLY after an
 * identifier, ')' ']' '}' or quote (value positions), else as regex. */
static int blex_regex_possible(const char *src, size_t from)
{
    size_t p = from;
    while (p > 0)
    {
        p--;
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' ||
            c == '`')
            return 0; /* division context */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$')
        {
            /* identifier/number: division UNLESS the word is a keyword
             * that can precede an expression (return/typeof/case/in/of/
             * instanceof/new/delete/void/do/else/throw/yield/await). */
            static const char *const kws[] = {
                "return", "typeof", "case", "in", "of", "instanceof",
                "new", "delete", "void", "do", "else", "throw", "yield",
                "await"
            };
            size_t w0 = p;
            while (w0 > 0)
            {
                char w = src[w0 - 1];
                if ((w >= 'a' && w <= 'z') || (w >= 'A' && w <= 'Z') ||
                    (w >= '0' && w <= '9') || w == '_' || w == '$')
                    w0--;
                else
                    break;
            }
            size_t wl = p - w0 + 1;
            for (unsigned i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
            {
                if (strlen(kws[i]) == wl &&
                    memcmp(src + w0, kws[i], wl) == 0)
                    return 1;
            }
            return 0;
        }
        return 1; /* operators / open brackets / start: regex context */
    }
    return 1;
}

/* Advance the lexer over ONE raw-code character, transparently consuming
 * comments/strings/templates/regexes as whole tokens. Returns 0 at EOF or
 * on structural surprise (unterminated token). */
static int blex_raw_step(BLex *x)
{
    if (x->pos >= x->len)
        return 0;
    unsigned char c = (unsigned char)x->src[x->pos];
    if (c == '/' || c == '"' || c == '\'' || c == '`')
    {
        if (c == '/')
        {
            unsigned char d = (x->pos + 1 < x->len) ? (unsigned char)x->src[x->pos + 1] : 0;
            if (d == '/' || d == '*')
                return blex_skip_token(x);
            if (blex_regex_possible(x->src, x->pos))
                return blex_skip_token(x);
            x->pos++; /* division */
            return 1;
        }
        return blex_skip_token(x);
    }
    x->pos++;
    return 1;
}

/* R26h: copy [a,b) renaming the bare identifiers `e`→`__we` and `t`→`__wt`.
 * Used ONLY for the runtime tail slices when the entry was unwrapped: the
 * registry and module cache move to collision-proof names so the entry
 * body's own `var e={} / var t` (now globals after the unwrap) cannot
 * clobber them. Lexer-guarded: strings/templates/regexes/comments are
 * consumed whole by blex_raw_step; identifier boundaries exclude `.`
 * property access and ident-char neighbors (so `new`, `he`, `obj.e`,
 * `{e:` keys stay untouched). Params named e/t rename consistently with
 * their references (binding+refs move together). Returns bytes written
 * or -1 on capacity overflow. */
static long blx_copy_rename_et(const char *src, size_t len, uint32_t a,
                               uint32_t b, char *dst, size_t dstCap)
{
    size_t used = 0;
    BLex x;
    blex_init(&x, src, len, a);
    while (x.pos < b)
    {
        unsigned char c = (unsigned char)src[x.pos];
        const char *rep = NULL;
        if (c == 'e' || c == 't')
        {
            char prev = (x.pos > 0) ? src[x.pos - 1] : ' ';
            char next = (x.pos + 1 < len) ? src[x.pos + 1] : ' ';
            if (!blex_isidchar((unsigned char)prev) && prev != '.' &&
                !blex_isidchar((unsigned char)next))
                rep = (c == 'e') ? "__we" : "__wt";
        }
        if (rep)
        {
            size_t n = strlen(rep);
            if (used + n + 1 > dstCap)
                return -1;
            memcpy(dst + used, rep, n);
            used += n;
            x.pos++;
            continue;
        }
        /* one token: literals (strings/templates/regexes/comments) whole */
        size_t litStart = x.pos;
        if (!blex_raw_step(&x))
            return -1;
        size_t n = x.pos - litStart;
        if (used + n + 1 > dstCap)
            return -1;
        memcpy(dst + used, src + litStart, n);
        used += n;
    }
    dst[used] = '\0';
    return (long)used;
}

/* R26h: ENTRY UNWRAP — the last tail slice is the webpack entry arrow
 * `(()=>{ BODY })()`, one statement whose whole-function compile transient
 * (measured ~10.4MB on the 502KB bryanwandrych.com bundle: parse ~35B/B
 * + codegen) exceeds every device budget. The BODY is a statement list:
 * sliced at its depth-1 `;` boundaries into consecutive programs, its
 * locals become globals under the R20b persistence model with semantics
 * intact — closures capture global bindings (the n.d getters read
 * later-declared vars at call time), function declarations are callable
 * from later slices, execution order is preserved. The registry/cache
 * rename (registryRenamed) removes the two name collisions the unwrap
 * would otherwise create (body `var e={}` vs the registry, body `var t`
 * vs the module cache — the latter fatally: `var t,r=n(5043)` requires
 * before its own `t` declarator assigns, so a shared `t` reads undefined).
 * Returns 1 on success (spans appended, count updated), 0 on any shape
 * doubt (caller keeps the monolithic entry: admission refuses it
 * containedly, exactly the pre-R26h behavior). */
static int bundler_unwrap_entry(const char *src, size_t len,
                                PlutoBundlerPlan *plan, int entries,
                                int *countInOut)
{
    int count = *countInOut;
    int last = count - 1;
    uint32_t s = plan->spans[entries + 1 + last].start;
    uint32_t e = plan->spans[entries + 1 + last].end;
    if (e <= s || (e - s) < 64)
        return 0;
    /* shape: `(()=>{` prefix after optional trivia (6 bytes:
     * '(' '(' ')' '=' '>' '{' — minified webpack entry arrow) */
    uint32_t p = (uint32_t)blex_space_at(src, len, s);
    if (p + 6 > e || src[p] != '(' || src[p + 1] != '(' ||
        src[p + 2] != ')' || src[p + 3] != '=' || src[p + 4] != '>' ||
        src[p + 5] != '{')
        return 0;
    uint32_t bodyStart = p + 6;
    /* find the matching `}` of the arrow body (lexically whole literals) */
    BLex x;
    blex_init(&x, src, len, bodyStart);
    int depth = 1;
    uint32_t bodyClose = 0;
    while (x.pos < e)
    {
        unsigned char c = (unsigned char)src[x.pos];
        if (c == '{' || c == '(' || c == '[')
            depth++;
        else if (c == '}' || c == ')' || c == ']')
        {
            depth--;
            if (depth == 0 && c == '}')
            {
                bodyClose = (uint32_t)x.pos;
                break;
            }
        }
        if (!blex_raw_step(&x))
            return 0;
    }
    if (!bodyClose || bodyClose <= bodyStart)
        return 0;
    /* tail after the body must be exactly `)(` `)` [;] + trivia —
     * the arrow's closing paren, then the invocation `()` (live_main
     * span ends `...})()`, no trailing `;`; tolerate one if present) */
    uint32_t q = (uint32_t)blex_space_at(src, len, bodyClose + 1);
    if (q + 1 > e || src[q] != ')')
        return 0;
    q = (uint32_t)blex_space_at(src, len, q + 1);
    if (q + 2 > e || src[q] != '(' || src[q + 1] != ')')
        return 0;
    q = (uint32_t)blex_space_at(src, len, q + 2);
    if (q < e && src[q] == ';')
        q = (uint32_t)blex_space_at(src, len, q + 1);
    if (q != e)
        return 0;
    /* slice the body at its depth-1 `;` boundaries */
    uint32_t cut = bodyStart;
    int bodyCount = 0;
    int ok = 1;
    BLex y;
    blex_init(&y, src, len, bodyStart);
    int deep = 1;
    while (y.pos < bodyClose)
    {
        unsigned char c2 = (unsigned char)src[y.pos];
        if (c2 == ';' && deep == 1)
        {
            uint32_t b2 = (uint32_t)y.pos + 1;
            if (b2 > cut) /* non-empty statement flank */
            {
                if (entries + 1 + last + bodyCount >= plan->maxSpans - 2)
                {
                    ok = 0;
                    break;
                }
                bodyCount++;
                cut = b2;
            }
            y.pos++;
            continue;
        }
        if (c2 == '{' || c2 == '(' || c2 == '[')
            deep++;
        else if (c2 == '}' || c2 == ')' || c2 == ']')
        {
            deep--;
            if (deep <= 0)
            {
                ok = 0; /* escaped the body — miscount */
                break;
            }
        }
        if (!blex_raw_step(&y))
        {
            ok = 0;
            break;
        }
    }
    if (!ok || bodyCount == 0)
        return 0;
    if (cut < bodyClose)
        bodyCount++; /* final statement (no trailing `;`) */
    /* commit: overwrite the last old span with body statement 1 and append
     * the rest (the entry is the FINAL tail span — nothing to shift) */
    int slot = entries + 1 + last;
    cut = bodyStart;
    int written = 0;
    blex_init(&y, src, len, bodyStart);
    deep = 1;
    while (y.pos < bodyClose && written < bodyCount - 1)
    {
        unsigned char c2 = (unsigned char)src[y.pos];
        if (c2 == ';' && deep == 1)
        {
            uint32_t b2 = (uint32_t)y.pos + 1;
            if (b2 > cut)
            {
                plan->spans[slot + written].start = cut;
                plan->spans[slot + written].end = b2;
                written++;
                cut = b2;
            }
            y.pos++;
            continue;
        }
        if (c2 == '{' || c2 == '(' || c2 == '[')
            deep++;
        else if (c2 == '}' || c2 == ')' || c2 == ']')
            deep--;
        if (!blex_raw_step(&y))
            return 0;
    }
    if (cut < bodyClose)
    {
        plan->spans[slot + written].start = cut;
        plan->spans[slot + written].end = bodyClose;
        written++;
    }
    if (written != bodyCount)
        return 0;
    plan->entryBodyIndex = entries + 1 + last;
    plan->registryRenamed = 1;
    *countInOut = count - 1 + bodyCount;
    logger_log("[bundler] entry unwrapped: %d body statements (span %u..%u) "
               "— registry __we, cache __wt",
               bodyCount, bodyStart, bodyClose);
    return 1;
}

/* ── R55: TURBOPACK push-bundle planning ─────────────────────────────────
 * Next.js 15 turbopack chunk scripts (bryanwandrych.com ships five per
 * page) have ONE top-level statement:
 *
 *     (()=>{"use strict";(globalThis.TURBOPACK||(globalThis.TURBOPACK=[]))
 *           .push([src, id, factory, id, factory, ...])})()        [wrapped]
 *     (globalThis.TURBOPACK||(globalThis.TURBOPACK=[]))
 *           .push([src, [id, factory, ...], id, factory, ...])     [bare]
 *
 * element 0 ("src") evaluates document.currentScript for the chunk name;
 * the rest are (module id, factory) pairs — flat, or inside ONE nested
 * array as the next element, or both (the runtime walk recurses into a
 * nested array then keeps scanning top level). The chunk loader (the
 * small `turbopack-*.js` script) re-consumes every pushed array in order:
 * for each push it resolves the chunk promise by the src name and
 * registers every non-function element between successive factories as an
 * id → factory binding (throwing on a trailing run with no factory).
 *
 * Splitting therefore re-emits ONE push per WINDOW of consecutive pairs:
 * every segment repeats the src span (same chunk name) plus SELF-CONTAINED
 * whole (id, factory) pairs, so (a) prefix registration order is exactly
 * the original walk order (nested windows first, then flat), and (b) each
 * segment's registration lands in the SAME bracket as its chunk-promise
 * resolution — the invariant the monolithic push had. Cuts land only
 * after a LEXICALLY-PROVEN factory (function keyword / balanced-params
 * `(…)=>` / `ident=>`); the FINAL window must end on a proven factory
 * because the runtime throws on dangling ids (the real file would too).
 * Elements that are neither ids nor provable factories (object chunks'
 * otherChunks-like payloads) END the pair region and travel verbatim as
 * the plan's post tail, attached once to the last segment. A second
 * nested array or an unexpected element class NO_MAPs (run unsplit) —
 * conservative at every step. */

static const char kTurboHead[] =
    "(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push(";

/* Prove the raw element a..b is a function expression the runtime's
 * `"function"==typeof` test would classify as a factory: the `function`
 * keyword (anon or `function name(`), a balanced `(…)` parameter list
 * directly followed by `=>`, or an identifier directly followed by `=>`.
 * Parenthesized NON-function expressions (`(0,he.jsx)(…)`, call results)
 * are NOT provable → 0 (conservative: never a cut point). */
static int blx_prove_function(const char *src, size_t len, uint32_t a,
                              uint32_t b)
{
    size_t p = blex_space_at(src, len, a);
    if (p >= b || p >= len)
        return 0;
    unsigned char c0 = (unsigned char)src[p];
    if (c0 == 'f' && p + 8 <= b && memcmp(src + p, "function", 8) == 0)
    {
        unsigned char nx = (p + 8 < b) ? (unsigned char)src[p + 8] : 0;
        /* named = `function name(` — minifiers emit at least one space
         * or `(` directly; anything idchar-adjacent is an identifier
         * whose letters happen to spell "function". A bare `function`
         * (element ends there) is not an expression. */
        return nx == '(' || nx == ' ' || nx == '*';
    }
    if (c0 == '(')
    {
        int d = 1;
        size_t q = p + 1;
        while (q < b && d > 0)
        {
            unsigned char c = (unsigned char)src[q];
            if (c == '"' || c == '\'' || c == '`')
            {
                char qc = (char)c;
                q++;
                while (q < b && src[q] != qc)
                {
                    if (src[q] == '\\')
                        q++;
                    q++;
                }
                if (q >= b)
                    return 0;
                q++;
                continue;
            }
            if (c == '(')
                d++;
            else if (c == ')')
                d--;
            q++;
        }
        if (d != 0)
            return 0;
        q = blex_space_at(src, len, q);
        return (q + 2 <= b && src[q] == '=' && src[q + 1] == '>');
    }
    if (blex_isidchar(c0))
    {
        size_t q = p;
        while (q < b && blex_isidchar((unsigned char)src[q]))
            q++;
        q = blex_space_at(src, len, q);
        return (q + 2 <= b && src[q] == '=' && src[q + 1] == '>');
    }
    return 0;
}

/* Scan the elements of an array literal whose '[' byte is at `open`,
 * recording pair windows into plan->spans as cut. nested = 1 → pure
 * (id, factory) elements only (pulled OUT of a nested pairs array — the
 * windows emit FLAT via the emitter, R55b: the runtime's walk registers
 * nothing from a length-2 `[src,[pairs]]` arg); nested = 0 → the FIRST
 * element is the chunk src expression (recorded to plan->turboSrc0/1 and
 * never part of a window), ONE nested pairs array is allowed as the first
 * post-src element (windows recurse there first — the runtime's walk
 * order), and any other non-pair element ends the region: its leading
 * delimiter through the arg's closing ']' is the verbatim post tail. A
 * window closes after a proven-factory element once it holds ≥ tailStep
 * bytes; the FINAL window requires a proven-factory last element.
 * *closeOut = the arg's closing ']' byte. Returns 1 ok. */
static int blx_walk_turbo_pairs(const char *src, size_t len, size_t open,
                                int nested, PlutoBundlerPlan *plan,
                                int *windows, size_t *closeOut)
{
    size_t pos = open + 1;
    int haveWin = 0;
    int srcRecorded = 0;
    uint32_t winStart = 0;
    uint32_t lastElemEnd = 0;
    int lastClass = 0;
    size_t argClose = 0;
    for (;;)
    {
        size_t sig = blex_space_at(src, len, pos);
        if (sig >= len)
            return 0;
        if (src[sig] == ',')
        {
            /* tolerate a pending delimiter at the walk entry (the only
             * such path is the nested-array return below) */
            sig = blex_space_at(src, len, sig + 1);
            pos = sig;
            if (sig >= len)
                return 0;
        }
        /* lex this element to its top-level ',' or the arg's ']' */
        BLex x;
        blex_init(&x, src, len, sig);
        int d = 0;
        int atArgClose = 0;
        for (;;)
        {
            unsigned char c = (unsigned char)src[x.pos];
            if (c == '[' || c == '{' || c == '(')
            {
                d++;
            }
            else if (c == ']' || c == '}' || c == ')')
            {
                if (d == 0)
                {
                    /* element level: only ']' can appear — the arg close */
                    if (c != ']')
                        return 0;
                    argClose = x.pos;
                    atArgClose = 1;
                    break;
                }
                d--;
            }
            else if (c == ',' && d == 0)
            {
                break; /* element delimiter (x.pos sits ON it) */
            }
            if (!blex_raw_step(&x))
                return 0;
        }
        uint32_t elemEnd = (uint32_t)(atArgClose ? argClose : x.pos);
        if (elemEnd <= (uint32_t)sig)
            return 0;
        unsigned char lead = (unsigned char)src[sig];
        int cls; /* 1 = id, 2 = proven factory, 3 = nested array, 4 = other */
        if (lead >= '0' && lead <= '9')
            cls = 1;
        else if (lead == '[')
            cls = 3;
        else if (blx_prove_function(src, len, (uint32_t)sig, elemEnd))
            cls = 2;
        else
            cls = 4;
        if (!nested && !srcRecorded)
        {
            /* the src expression: verbatim span, never cut */
            plan->turboSrc0 = (uint32_t)sig;
            plan->turboSrc1 = elemEnd;
            srcRecorded = 1;
            if (atArgClose)
                return 0; /* src only — no pairs, nothing to split */
            pos = x.pos + 1;
            continue;
        }
        if (!nested && cls == 3)
        {
            /* the ONE nested pairs array (must be the first post-src
             * element — recursion preserves the runtime's walk order) */
            size_t nestedClose;
            if (plan->turboNestedEnd != 0)
                return 0; /* second nested array */
            if (!blx_walk_turbo_pairs(src, len, sig, 1, plan, windows,
                                      &nestedClose))
                return 0;
            plan->turboNestedEnd = (uint32_t)nestedClose;
            if (atArgClose)
                break;
            pos = nestedClose + 1;
            continue;
        }
        if (cls == 4)
        {
            /* pair region ends: everything from here through the arg
             * close is the verbatim post tail (top level only — nested
             * regions are strictly id/fn pairs) */
            if (nested)
                return 0;
            if (haveWin)
            {
                /* open final window: dangling id run would make BOTH the
                 * original and the split registration throw */
                if (lastClass != 2)
                    return 0;
                if (*windows >= plan->maxSpans)
                    return 0;
                plan->spans[*windows].start = winStart;
                plan->spans[*windows].end = lastElemEnd;
                (*windows)++;
                haveWin = 0;
            }
            /* haveWin==0: the previous element's budget cut already
             * recorded the final window (cuts land after proven fns,
             * so its tail is factory-terminated) */
            if (*windows == 0)
                return 0;
            /* locate the arg close beyond the post elements (verbatim,
             * balance-proving just to find the end byte) */
            {
                BLex y;
                blex_init(&y, src, len, x.pos); /* on the region-ending ',' */
                int dd = 0;
                int found = 0;
                for (;;)
                {
                    unsigned char c = (unsigned char)src[y.pos];
                    if (c == '[' || c == '{' || c == '(')
                    {
                        dd++;
                    }
                    else if (c == ']' || c == '}' || c == ')')
                    {
                        if (c == ']' && dd == 0)
                        {
                            argClose = y.pos;
                            found = 1;
                            break;
                        }
                        dd--;
                        if (dd < 0)
                            return 0;
                    }
                    if (!blex_raw_step(&y))
                        return 0;
                }
                if (!found)
                    return 0;
            }
            /* post tail = [lastElemEnd, argClose) — the leading ',' after
             * the region's final element included; cap it so the LAST
             * segment (window + post) stays parse-sized in one frame. */
            if ((size_t)argClose - (size_t)lastElemEnd >
                (size_t)plan->tailStep)
                return 0; /* post too large to re-emit whole — unsplit */
            plan->turboPost0 = lastElemEnd;
            plan->turboPost1 = (uint32_t)argClose;
            *closeOut = argClose;
            return 1;
        }
        /* pair-region element: id, proven factory, or a nested array in
         * the nested walk (illegal there) */
        if (nested && cls == 3)
            return 0;
        if (!haveWin)
        {
            winStart = (uint32_t)sig;
            haveWin = 1;
        }
        if (cls == 2 && (elemEnd - winStart) >= (uint32_t)plan->tailStep)
        {
            if (*windows >= plan->maxSpans)
                return 0;
            plan->spans[*windows].start = winStart;
            plan->spans[*windows].end = elemEnd;
            (*windows)++;
            haveWin = 0;
        }
        lastElemEnd = elemEnd;
        lastClass = cls;
        if (atArgClose)
            break;
        pos = x.pos + 1; /* past the ',' delimiter */
    }
    /* arg closed inside the pair region: finalize the open final window.
     * haveWin==0 with lastClass==2 is VALID (the last element's budget
     * cut landed exactly at the region end). A nested-only walk (no top
     * flat pairs) finalizes nothing here. */
    if (haveWin)
    {
        if (lastClass != 2)
            return 0; /* dangling id run — the runtime would throw */
        if (*windows >= plan->maxSpans)
            return 0;
        plan->spans[*windows].start = winStart;
        plan->spans[*windows].end = lastElemEnd;
        (*windows)++;
    }
    *closeOut = argClose;
    return 1;
}

/* R55 planner entry: returns 1 with the plan filled (kind=TURBOPACK),
 * 0 = not the shape (caller continues with the webpack scan). */
static int bundler_plan_turbopack(const char *src, size_t len, size_t pos,
                                  PlutoBundlerPlan *plan)
{
    const size_t headLen = sizeof(kTurboHead) - 1;
    int wrapped = 0;
    size_t p = pos;
    if (p + 6 <= len && memcmp(src + p, "(()=>{", 6) == 0)
    {
        /* wrapped: `(()=>{"use strict";HEAD[...]})()` */
        BLex x;
        blex_init(&x, src, len, p + 6);
        if (!blex_space(&x))
            return 0;
        static const char strict[] = "\"use strict\";";
        if (x.pos + sizeof(strict) - 1 > len ||
            memcmp(src + x.pos, strict, sizeof(strict) - 1) != 0)
            return 0;
        x.pos += sizeof(strict) - 1;
        if (!blex_space(&x))
            return 0;
        p = x.pos;
        wrapped = 1;
    }
    if (p + headLen > len || memcmp(src + p, kTurboHead, headLen) != 0)
        return 0;
    p += headLen;
    if (p >= len || src[p] != '[')
        return 0; /* object pushes (runtime config) don't split */
    int windows = 0;
    size_t argClose = 0;
    if (!blx_walk_turbo_pairs(src, len, p, 0, plan, &windows, &argClose))
        return 0;
    /* verify the push-call tail: `)` then wrapped `})()` / bare `;`|EOF */
    {
        BLex x;
        blex_init(&x, src, len, argClose + 1);
        if (!blex_space(&x))
            return 0;
        if (x.pos >= len || src[x.pos] != ')')
            return 0;
        x.pos++;
        if (wrapped)
        {
            if (!blex_space(&x))
                return 0;
            if (x.pos >= len || src[x.pos] != '}')
                return 0;
            x.pos++;
            /* `(()=>{ … })()`: the arrow's own `(` closes first, THEN the
             * call — the real chunk tail is `…])})();`. */
            if (!blex_space(&x))
                return 0;
            if (x.pos >= len || src[x.pos] != ')')
                return 0;
            x.pos++;
            if (!blex_space(&x))
                return 0;
            if (x.pos + 1 >= len || src[x.pos] != '(' ||
                src[x.pos + 1] != ')')
                return 0;
            x.pos += 2;
        }
    }
    if (windows < 2)
        return 0; /* a single window is not worth a plan */
    plan->kind = PLUTO_BUNDLER_KIND_TURBOPACK;
    plan->turboWrap = wrapped;
    plan->entries = windows;
    plan->nSpans = windows;
    plan->tailIndex = -1;
    plan->tailCount = 0;
    plan->tailHasComma = 0;
    plan->entryBodyIndex = -1;
    plan->registryRenamed = 0;
    plan->name[0] = '\0';
    logger_log("[bundler] turbopack plan: wrap=%d windows=%d "
               "src=%u..%u nestedEnd=%u post=%u..%u",
               wrapped, windows, plan->turboSrc0, plan->turboSrc1,
               plan->turboNestedEnd, plan->turboPost0, plan->turboPost1);
    return 1;
}

PlutoBundlerStatus jsbridge_bundler_plan(const char *src, size_t len,
                                         PlutoBundlerPlan *plan)
{
    memset(plan->spans, 0, sizeof(PlutoBundlerSpan) * (size_t)plan->maxSpans);
    plan->nSpans = 0;
    plan->name[0] = '\0';
    plan->entries = 0;
    plan->tailIndex = -1;
    plan->tailCount = 0;
    plan->tailHasComma = 0;
    plan->entryBodyIndex = -1;
    plan->registryRenamed = 0;
    plan->tailStart = 0;
    plan->tailEnd = 0;
    plan->tailStep = PLUTO_BUNDLER_TAIL_STEP;
    plan->kind = PLUTO_BUNDLER_KIND_WEBPACK; /* R55: default shape */
    plan->turboWrap = 0;
    plan->turboNestedEnd = 0;
    plan->turboSrc0 = plan->turboSrc1 = 0;
    plan->turboPost0 = plan->turboPost1 = 0;
    if (!src || len < 16)
        return PLUTO_BUNDLER_NO_MAP;
    logger_log("[bundler] plan enter len=%zu live=%luKB", len,
               pluto_mem_live() / 1024);

    size_t pos = 0;
    /* Leading trivia: whitespace and block comments in ANY order and count
     * (webpack emits `/*! license *\/` banners before the IIFE; inline
     * wrappers and minifiers may add newlines between them). Requiring the
     * comment at byte 0 exactly made the plan depend on how the script text
     * was delivered — a general structure must not care. */
    for (;;)
    {
        while (pos < len && (src[pos] == ' ' || src[pos] == '\t' ||
                             src[pos] == '\r' || src[pos] == '\n'))
            pos++;
        if (pos + 1 < len && src[pos] == '/' && src[pos + 1] == '*')
        {
            size_t i = pos + 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
                i++;
            if (i + 1 >= len)
                return PLUTO_BUNDLER_NO_MAP;
            pos = i + 2;
            continue;
        }
        break;
    }
    /* R55: turbopack push-bundle shape (Next.js 15 static chunks). Try it
     * FIRST: this head never starts with `var`, so the webpack map scan
     * below cannot claim it, and the wrapped variant shares the `(()=>{`
     * head with the webpack shape. */
    if (bundler_plan_turbopack(src, len, pos, plan))
        return PLUTO_BUNDLER_OK;
    /* IIFE arrow head: exactly `(()=>{` */
    if (pos + 6 > len || memcmp(src + pos, "(()=>{", 6) != 0)
        return PLUTO_BUNDLER_NO_MAP;
    pos += 6; /* inside the arrow body now: brace depth 1 */

    BLex x;
    blex_init(&x, src, len, pos);
    int brace = 1;

    /* first significant statement must be `var <ident> = {` */
    if (!blex_space(&x))
        return PLUTO_BUNDLER_NO_MAP;
    if (x.pos + 4 > len || memcmp(src + x.pos, "var", 3) != 0 ||
        (x.pos + 3 < len && blex_isidchar((unsigned char)src[x.pos + 3])))
        return PLUTO_BUNDLER_NO_MAP;
    x.pos += 3;
    if (!blex_space(&x))
        return PLUTO_BUNDLER_NO_MAP;
    size_t name0 = x.pos;
    while (x.pos < len && blex_isidchar((unsigned char)src[x.pos]))
        x.pos++;
    size_t name1 = x.pos;
    if (name1 == name0 || name1 - name0 >= sizeof(plan->name))
        return PLUTO_BUNDLER_NO_MAP;
    if (!blex_space(&x))
        return PLUTO_BUNDLER_NO_MAP;
    if (x.pos >= len || src[x.pos] != '=')
        return PLUTO_BUNDLER_NO_MAP;
    x.pos++;
    if (!blex_space(&x))
        return PLUTO_BUNDLER_NO_MAP;
    if (x.pos >= len || src[x.pos] != '{')
        return PLUTO_BUNDLER_NO_MAP;
    x.pos++;
    brace++; /* inside the map: map level == brace 2 */

    /* collect map entries: `KEY : VALUE` between map-level commas */
    int entries = 0;
    for (;;)
    {
        if (!blex_space(&x))
            return PLUTO_BUNDLER_NO_MAP;
        if (x.pos >= len)
            return PLUTO_BUNDLER_NO_MAP;
        if (src[x.pos] == '}' && brace == 2)
            break; /* empty map region (shouldn't happen — MIN_ENTRIES) */
        size_t key0 = x.pos;
        /* key: raw token up to a map-level ':' (ident, number, or string) */
        int sawColon = 0;
        while (x.pos < len)
        {
            unsigned char c = (unsigned char)src[x.pos];
            if (c == ':' && brace == 2)
            {
                sawColon = 1;
                break;
            }
            if (c == ',' || c == '}' || c == '{' || c == '(' || c == '[')
                return PLUTO_BUNDLER_NO_MAP; /* not a plain key */
            if (!blex_raw_step(&x))
                return PLUTO_BUNDLER_NO_MAP;
        }
        if (!sawColon)
            return PLUTO_BUNDLER_NO_MAP;
        size_t colon = x.pos;
        if (colon == key0)
            return PLUTO_BUNDLER_NO_MAP;
        x.pos++; /* ':' */
        if (!blex_space(&x))
            return PLUTO_BUNDLER_NO_MAP;
        /* value: raw-lex until map-level ',' or map-closing '}' */
        for (;;)
        {
            if (x.pos >= len)
                return PLUTO_BUNDLER_NO_MAP;
            unsigned char c = (unsigned char)src[x.pos];
            if (c == '{' || c == '(' || c == '[')
                brace++;
            else if (c == '}' || c == ')' || c == ']')
            {
                if (c == '}' && brace == 2)
                    break; /* map close — value ended just before */
                if (brace <= 2)
                    return PLUTO_BUNDLER_NO_MAP; /* would escape the map */
                brace--;
            }
            else if (c == ',' && brace == 2)
                break; /* entry separator */
            if (!blex_raw_step(&x))
                return PLUTO_BUNDLER_NO_MAP;
        }
        if (entries >= plan->maxSpans - 2)
            return PLUTO_BUNDLER_NO_MAP; /* absurd bundle — run unsplit */
        plan->spans[1 + entries].start = (uint32_t)key0;
        plan->spans[1 + entries].end = (uint32_t)x.pos;
        entries++;
        if (x.pos >= len)
            return PLUTO_BUNDLER_NO_MAP;
        if (src[x.pos] == '}')
        {
            x.pos++; /* consume map close; back to arrow-body level */
            brace--;
            break;
        }
        x.pos++; /* consume the map-level ',' */
    }

    logger_log("[bundler] map scanned: %d entries", entries);
    if (entries < PLUTO_BUNDLER_MIN_ENTRIES)
        return PLUTO_BUNDLER_NO_MAP;

    /* Registry identifier must fit plan->name[24]; longer names mean the
     * structure is not the shape we split for (or name0/name1 lexing
     * regressed) — NO_MAP is always the safe answer. */
    if (name1 <= name0 || name1 - name0 >= sizeof(plan->name))
        return PLUTO_BUNDLER_NO_MAP;

    /* tail: map close to the arrow-body close (brace 1 -> 0) */
    if (!blex_space(&x))
        return PLUTO_BUNDLER_NO_MAP;
    if (x.pos >= len)
        return PLUTO_BUNDLER_NO_MAP;
    uint32_t tail0 = (uint32_t)x.pos;
    if (src[x.pos] == ',')
        plan->tailHasComma = 1; /* `var <m>={...},t={};` — repair needed */
    int closed = 0;
    while (x.pos < len)
    {
        unsigned char c = (unsigned char)src[x.pos];
        if (c == '{' || c == '(' || c == '[')
            brace++;
        else if (c == '}' || c == ')' || c == ']')
        {
            brace--;
            if (brace == 0)
            {
                closed = 1;
                break; /* arrow body close — tail ended just before */
            }
            if (brace < 0)
                return PLUTO_BUNDLER_NO_MAP;
        }
        if (!blex_raw_step(&x))
            return PLUTO_BUNDLER_NO_MAP;
    }
    if (!closed || x.pos == 0)
        return PLUTO_BUNDLER_NO_MAP;
    uint32_t tail1 = (uint32_t)x.pos; /* exclusive: the closing '}' excluded */

    /* R20b: split the tail into whole-statement slices of ~tailStep
     * bytes. The tail is executed inside one engine bracket per slice;
     * keeping every unit under a few KB bounds each slice's run-loop
     * occupancy (device: ~12KB/s ⇒ a 6KB slice runs well under a
     * second). Boundaries land at depth-1 tokens the raw lexer proves
     * are outside any literal: at `;` (R20b), and at `,` directly
     * following `}`/`)`/`]` (R26f — webpack's dominant tail shape
     * `n.n=e=>{...},(()=>{...})(),...`; the comma is DROPPED and the
     * sequence becomes consecutive programs). A boundary can never
     * split a template literal, string, regex, or comment. State
     * flows between slices through the machine's persistent global
     * object (the same cross-program mechanism the module registry and
     * the R16 chunked path rely on), and execution order is preserved.
     * Conservative: ANY ambiguity about the first/last boundary → fall
     * back to the single-slice tail (monolithic behavior). */
    {
        uint32_t t0 = tail0;
        uint32_t t1 = tail1;
        uint32_t step = plan->tailStep;
        if (step < 512)
            step = 512; /* honor a smaller test override, but sane minimum */
        if (t1 > t0 && (uint32_t)(t1 - t0) > step)
        {
            uint32_t cut = t0;
            int count = 0;
            int ok = 1;
            int brace = 1; /* inside the arrow body */
            /* R26f: statement-kind guard — a depth-1 `,` is a separator
             * (cuttable, comma dropped) UNLESS the statement it lives in
             * carries a payload: var/let/const declarator lists and
             * return/throw/yield expressions. Recomputed at every
             * statement start. */
            int hazard = blx_stmt_hazardous(src, len, cut);
            uint32_t tslot = entries + 1; /* first tail span slot (tailIndex) */
            BLex x;
            blex_init(&x, src, len, t0);
            while (x.pos < t1)
            {
                unsigned char c = (unsigned char)src[x.pos];
                if (c == ';' && brace == 1)
                {
                    /* DEFINITE statement end: the raw lexer got here
                     * outside every literal/comment/regex, at depth 1. */
                    uint32_t b = (uint32_t)x.pos + 1;
                    if (b - cut >= step)
                    {
                        /* boundary sanity: the next significant byte
                         * (skipping trivia) may be anything except a `,`
                         * (a depth-1 `;` followed by `,` is not a
                         * statement-end shape we understand — be
                         * conservative) or dangling block-comment trivia.
                         * With the explicit `;` there is no ASI hazard:
                         * the next token always begins a fresh statement. */
                        if (b < t1)
                        {
                            uint32_t nsig =
                                (uint32_t)blex_space_at(src, len, b);
                            if (nsig == b && nsig + 1 < len &&
                                src[nsig] == '/' && src[nsig + 1] == '*')
                                ok = 0; /* unterminated comment trivia */
                            if (nsig < t1 && src[nsig] == ',')
                                ok = 0;
                        }
                        if (!ok)
                            break;
                        plan->spans[tslot + count].start = cut;
                        plan->spans[tslot + count].end = b;
                        count++;
                        if (entries + 2 + count >= plan->maxSpans - 2)
                        {
                            ok = 0; /* absurd bundle — single tail instead */
                            break;
                        }
                        cut = b;
                    }
                    x.pos++; /* consume the ';' */
                    hazard = blx_stmt_hazardous(src, len, x.pos);
                    continue;
                }
                if (c == '{' || c == '(' || c == '[')
                {
                    brace++;
                }
                else if (c == '}' || c == ')' || c == ']')
                {
                    brace--;
                }
                else if (c == ',' && brace == 1 && !hazard && ok &&
                         x.pos > cut)
                { /* non-empty left flank — the leading repair comma of a
                   * `,t={}` tail start is consumed as ordinary trivia of
                   * slice 0 (the emit re-attaches it via the var repair) */
                    /* R26f: DEFINITE sequence-element end — depth-1 `,`
                     * in a non-payload statement, e.g. webpack's
                     * `n.n=e=>{...},n.p="/",(()=>{...})(),…`. Cutting here
                     * and DROPPING the comma rewrites the top-level
                     * sequence as consecutive statements — semantically
                     * identical (no value consumer between elements; the
                     * final element is the bootstrap). NOT a boundary in
                     * payload statements (hazard guard above): var
                     * declarator chains `var a=(..)(),b=…` and
                     * return/throw/yield expressions. */
                    if (x.pos > cut)
                    {
                        uint32_t nsig =
                            (uint32_t)blex_space_at(src, len, x.pos + 1);
                        if (nsig + 1 < len && src[nsig] == '/' &&
                            src[nsig + 1] == '*')
                            ok = 0; /* unterminated comment trivia */
                        if (nsig < t1 && src[nsig] == ',')
                            ok = 0; /* not our shape — conservative */
                    }
                    if (!ok)
                        break;
                    plan->spans[tslot + count].start = cut;
                    plan->spans[tslot + count].end = (uint32_t)x.pos;
                    count++;
                    if (entries + 2 + count >= plan->maxSpans - 2)
                    {
                        ok = 0; /* absurd bundle — single tail instead */
                        break;
                    }
                    cut = (uint32_t)x.pos + 1; /* comma DROPPED */
                    x.pos++; /* consume the ',' */
                    hazard = blx_stmt_hazardous(src, len, cut);
                    continue;
                }
                if (brace <= 0)
                {
                    ok = 0; /* escaped the arrow body — miscount */
                    break;
                }
                /* step one token; strings/templates/regexes and comments
                 * are consumed WHOLE, so a `;` inside them is never seen */
                if (!blex_raw_step(&x))
                {
                    ok = 0; /* structural surprise inside the tail */
                    break;
                }
            }
            /* R26f: the trailing region (cut..t1) absorbs every statement
             * after the LAST regular boundary — in real web bundles that is
             * routinely the whole JSX bootstrap (measured: 252656B of a
             * 253KB tail — 5 tiny early statements, then the render tree
             * with NO cuttable boundary until the very end). Ceiling it:
             * force-cut at the LAST valid boundary before
             * PLUTO_BUNDLER_TAIL_CHUNK_MAX, repeating until the remainder
             * fits. A chunk with NO boundary in range stays whole (one
             * monster statement → its segment skips containedly, exactly
             * the pre-R26f monolith behavior). Multiple consecutive
             * top-level statements stay semantically identical under the
             * same ordering guarantee as R20b. */
            while (ok && count > 0 &&
                   t1 - cut > PLUTO_BUNDLER_TAIL_CHUNK_MAX)
            {
                BLex y;
                uint32_t lastB = 0;  /* boundary token position */
                uint32_t lastNext = 0; /* next chunk start */
                int deep = 1;
                int yHazard = blx_stmt_hazardous(src, len, cut);
                /* R26f: scan the WHOLE remainder — the next boundary may be
                 * far past the ceiling (a big IIFE element); cutting at the
                 * first valid boundary however far keeps every real
                 * statement separable. A remainder with NO boundary stays
                 * one chunk (single monster statement → contained skip). */
                blex_init(&y, src, len, cut);
                while (y.pos < t1)
                {
                    unsigned char c2 = (unsigned char)src[y.pos];
                    if (c2 == ';' && deep == 1 && (uint32_t)y.pos > cut)
                    {
                        lastB = (uint32_t)y.pos + 1;
                        lastNext = lastB;
                        yHazard = blx_stmt_hazardous(src, len, lastNext);
                    }
                    else if (c2 == ',' && deep == 1 && !yHazard &&
                             (uint32_t)y.pos > cut)
                    {
                        lastB = (uint32_t)y.pos;     /* comma DROPPED */
                        lastNext = (uint32_t)y.pos + 1;
                        yHazard = blx_stmt_hazardous(src, len, lastNext);
                    }
                    if (c2 == '{' || c2 == '(' || c2 == '[')
                    {
                        deep++;
                    }
                    else if (c2 == '}' || c2 == ')' || c2 == ']')
                    {
                        deep--;
                    }
                    if (deep <= 0 || !blex_raw_step(&y))
                        break;
                }
                if (lastB == 0)
                    break; /* no cuttable boundary — keep the big chunk */
                plan->spans[tslot + count].start = cut;
                plan->spans[tslot + count].end = lastB;
                count++;
                if (entries + 2 + count >= plan->maxSpans - 2)
                {
                    ok = 0;
                    break;
                }
                cut = lastNext;
            }
            if (ok && count > 0 && cut < t1)
            {
                plan->spans[tslot + count].start = cut;
                plan->spans[tslot + count].end = t1;
                count++;
            }
            if (ok && count > 0)
            {
                plan->tailCount = count;
                plan->nSpans = entries + 2 + (count - 1);
                logger_log("[bundler] tail sliced: %d statements "
                           "(step %u, %u..%u)", count, step, tail0, tail1);
            }
            else
            {
                plan->tailCount = 1; /* conservative fallback */
            }
        }
        else
        {
            plan->tailCount = 1;
        }
    }

    memcpy(plan->name, src + name0, name1 - name0);
    plan->name[name1 - name0] = '\0';
    plan->entries = entries;
    plan->tailIndex = entries + 1;
    /* tailCount was set by the R20b slicer above (≥ 1 in every path:
    * sliced count, or 1 = conservative single-tail fallback). */
    plan->tailStart = tail0;
    plan->tailEnd = tail1;
    /* spans[0] stays {0,0} (generated registry decl); tail spans recorded
     * in spans[tailIndex..tailIndex+tailCount-1] (R20b statement slices).
     * The single-tail fallback MUST fill spans[tailIndex] with the whole
     * tail region — the slicer writes tail spans itself, but this path
     * arrives with the {0,0} memset placeholder and emit/segment_cap read
     * spans[seg] for every tail segment. */
    if (plan->tailCount <= 1)
    {
        plan->spans[plan->tailIndex].start = tail0;
        plan->spans[plan->tailIndex].end = tail1;
    }
    /* nSpans covers seg 0 (registry decl) + entries module spans + the
     * tail slices (R18 set this unconditionally to entries+2; R20b makes
     * it entries+1+tailCount, identical for the single-tail shape). */
    plan->nSpans = entries + 1 + plan->tailCount;
    /* R26h: try to unwrap the entry IIFE into body statement slices. On
     * success the registry/cache move to __we/__wt (plan->name overridden
     * so seg 0 + map-entry emission follows automatically) and the last
     * tail span is replaced by the body's statement spans. */
    {
        int count = plan->tailCount;
        if (bundler_unwrap_entry(src, len, plan, entries, &count))
        {
            plan->tailCount = count;
            plan->nSpans = entries + 1 + plan->tailCount;
            strcpy(plan->name, "__we");
        }
    }
    logger_log("[bundler] plan done: entries=%d tail=%u..%u slices=%d",
               entries, plan->tailStart, plan->tailEnd, plan->tailCount);
    return PLUTO_BUNDLER_OK;
}

size_t jsbridge_bundler_segment_cap(const PlutoBundlerPlan *plan, int seg)
{
    if (seg < 0 || seg >= plan->nSpans)
        return 0;
    if (plan->kind == PLUTO_BUNDLER_KIND_TURBOPACK)
    {
        /* R55: boilerplate (`(()=>{"use strict";` + head + delimiters +
         * `])`/`})()`) plus the verbatim src span, this segment's window
         * (always flat — R55b), and the post tail on the LAST segment. */
        size_t n = (size_t)(plan->spans[seg].end - plan->spans[seg].start);
        n += (size_t)(plan->turboSrc1 - plan->turboSrc0);
        n += sizeof("(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push([");
        if (plan->turboWrap)
            n += sizeof("(()=>{\"use strict\";") + sizeof("})()") - 2;
        if (seg == plan->nSpans - 1 && plan->turboPost1 > plan->turboPost0)
            n += (size_t)(plan->turboPost1 - plan->turboPost0);
        return n + 32;
    }
    if (seg == 0)
        return sizeof(plan->name) + 8;
    if (seg <= plan->entries)
    {
        uint32_t vlen = plan->spans[seg].end - plan->spans[seg].start;
        /* `<name>[<key>]=<value>;` — key+value re-partition the span, so
         * the span bytes appear once; name appears twice. */
        return (size_t)vlen + sizeof(plan->name) * 2 + 16;
    }
    /* tail slice: IIFE wrapper + optional `var <repair>=0` + this slice.
     * R26h: pre-entry runtime slices are e/t-renamed — each bare `e`/`t`
     * grows 3 bytes (worst case: every byte an identifier char → 4×). */
    if (plan->registryRenamed && seg < plan->entryBodyIndex)
        return (size_t)(plan->spans[seg].end - plan->spans[seg].start) * 4 + 64;
    return (size_t)(plan->spans[seg].end - plan->spans[seg].start) + 64;
}

long jsbridge_bundler_emit(const char *src, size_t len,
                           const PlutoBundlerPlan *plan, int seg,
                           char *dst, size_t dstCap)
{
    if (seg < 0 || seg >= plan->nSpans)
        return -1;
    size_t used = 0;
#define EMIT_LIT(s)                                                  \
    do                                                               \
    {                                                                \
        size_t n = sizeof(s) - 1;                                    \
        if (used + n + 1 > dstCap)                                   \
            return -1;                                               \
        memcpy(dst + used, s, n);                                    \
        used += n;                                                   \
    } while (0)
#define EMIT_SPAN(a, b)                                              \
    do                                                               \
    {                                                                \
        if ((size_t)(b) > len || (size_t)(a) > (size_t)(b))          \
            return -1;                                               \
        size_t n = (size_t)(b) - (size_t)(a);                        \
        if (used + n + 1 > dstCap)                                   \
            return -1;                                               \
        memcpy(dst + used, src + (a), n);                            \
        used += n;                                                   \
    } while (0)

    if (plan->kind == PLUTO_BUNDLER_KIND_TURBOPACK)
    {
        /* R55b: re-emit ONE push registration. Every segment re-carries
         * the src expression (same chunk name each time — the runtime's
         * registerChunk resolves an already-resolved chunk promise as a
         * no-op, and the module walk starts at index 1), its window of
         * whole (id, factory) pairs — ALWAYS flat: the runtime's H
         * walker treats a length-2 arg as a CONFIG item and reads
         * `.otherChunks` on it without registering the pairs (the
         * original chunks stay length>2 by appending top-level pairs
         * after their nested array), so re-wrapping a window slice in
         * its own `[…]` would silently register nothing and reject.
         * Flat emission of the nested array's window slices preserves
         * the original registration order (the walk recurses into the
         * nested array first, then scans top level — our windows are
         * recorded in exactly that order). The wrapped form re-wraps
         * each segment in the original `(()=>{"use strict"; … })()`
         * strict-mode IIFE. */
        uint32_t a = plan->spans[seg].start;
        uint32_t b = plan->spans[seg].end;
        if (plan->turboWrap)
            EMIT_LIT("(()=>{\"use strict\";");
        EMIT_LIT("(globalThis.TURBOPACK||(globalThis.TURBOPACK=[])).push([");
        EMIT_SPAN(plan->turboSrc0, plan->turboSrc1);
        EMIT_LIT(",");
        EMIT_SPAN(a, b);
        if (seg == plan->nSpans - 1 &&
            plan->turboPost1 > plan->turboPost0)
            EMIT_SPAN(plan->turboPost0, plan->turboPost1);
        EMIT_LIT("])");
        if (plan->turboWrap)
            EMIT_LIT("})()");
        else
            EMIT_LIT(";");
        dst[used] = '\0';
        return (long)used;
    }
    if (seg == 0)
    {
        /* fresh registry: `var <name>={};` */
        EMIT_LIT("var ");
        size_t n = strlen(plan->name);
        if (used + n + 1 > dstCap)
            return -1;
        memcpy(dst + used, plan->name, n);
        used += n;
        EMIT_LIT("={};");
    }
    else if (seg <= plan->entries)
    {
        /* `<name>[<key>]=<value>;` — key/value copied VERBATIM around the
         * span's map-level ':' (numbers, idents, quoted strings all work
         * as member-expression keys). */
        uint32_t s = plan->spans[seg].start;
        uint32_t e = plan->spans[seg].end;
        uint32_t p = s;
        int depth = 0;
        int found = 0;
        while (p < e && p < len)
        {
            char c = src[p];
            if (c == '"' || c == '\'')
            { /* quoted key: skip string */
                char q = c;
                p++;
                while (p < e && p < len)
                {
                    if (src[p] == '\\')
                        p++;
                    else if (src[p] == q)
                        break;
                    p++;
                }
            }
            else if (c == '[')
                depth++;
            else if (c == ']')
                depth--;
            else if (c == ':' && depth == 0)
            {
                found = 1;
                break;
            }
            p++;
        }
        if (!found)
            return -1;
        size_t n = strlen(plan->name);
        if (used + n + 1 > dstCap)
            return -1;
        memcpy(dst + used, plan->name, n);
        used += n;
        EMIT_LIT("[");
        EMIT_SPAN(s, p);
        EMIT_LIT("]=");
        EMIT_SPAN(p + 1, e);
        EMIT_LIT(";");
    }
    else
    {
        uint32_t s = plan->spans[seg].start;
        uint32_t e = plan->spans[seg].end;
        if (plan->tailCount <= 1)
        {
            /* R18 shape (unchanged, host-proven): the whole tail inside
             * ONE IIFE — `var`/`function` declarations stay in one
             * function scope, exactly like the original arrow body. */
            EMIT_LIT("(()=>{");
            if (plan->tailHasComma)
            {
                EMIT_LIT("var __plutoSplit0=0");
            }
            EMIT_SPAN(s, e);
            EMIT_LIT("})();");
        }
        else
        {
            /* R20b sliced tail: TOP-LEVEL statement slices. Each slice
             * is a program, so its `var`/`function` declarations land on
             * the machine's persistent global object — the SAME
             * cross-program mechanism the module registry (`var <m>={}`
             * in seg 0) and the R16 chunked path rely on. That is what
             * keeps later slices seeing earlier slices' declarations
             * (minified single-letter top-level names in the tail refer
             * to each other); a per-slice IIFE would hide them. Slice 0
             * keeps the `,decl` repair (a dummy first declarator turns a
             * leading `,x={}` into a valid statement). */
            if (seg == plan->tailIndex && plan->tailHasComma)
            {
                EMIT_LIT("var __plutoSplit0=0");
            }
            if (plan->registryRenamed && seg < plan->entryBodyIndex)
            {
                /* R26h: runtime bootstrap before the unwrapped entry —
                 * copy with the registry/cache identifiers renamed to
                 * their collision-proof names. */
                long rn = blx_copy_rename_et(src, len, s, e, dst + used,
                                             dstCap - used);
                if (rn < 0)
                    return -1;
                used += (size_t)rn;
            }
            else
            {
                EMIT_SPAN(s, e);
            }
        }
    }
    dst[used] = '\0';
    return (long)used;
#undef EMIT_LIT
#undef EMIT_SPAN
}
