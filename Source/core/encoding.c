// encoding.c — charset detection & conversion (C port of core/encoding.lua).
//
// Ground truth: host-Lua oracle p06_oracle.lua on the REAL encoding.lua.
// Verified quirks preserved:
//   - charset="utf-8" (QUOTED value in a Content-Type HEADER) fails the Lua
//     pattern -> NULL (meta scan then applies per precedence)
//   - normalizeCharset("euc-jp") strips to "eucjp"; shift_jis/eucjp/gbk/big5
//     dispatch through single-byte best-effort
//   - "utf16" (no endianness) normalizes but is NOT dispatched -> verbatim
//   - UTF-16LE with one trailing stray byte decodes to ""
//   - lone high surrogate CONSUMES the next unit before emitting "?"
//   - bytes <0x80 pass through raw (incl. 0x00/0x7F); 0x80-0x9F CP1252 table;
//     >=0xA0 Latin-1
//   - meta scan: Lua pattern '<meta[^>]*charset...' uses GREEDY [^>]*, so a
//     tag with duplicate charset= attributes resolves to the LAST one
//     (occurrences tried right-to-left within the tag span).
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "../util/mem.h"
#include "../util/strbuf.h"
#include "encoding.h"

static struct PlaydateAPI* s_pd = NULL;

void encoding_init(struct PlaydateAPI* pd) { s_pd = pd; }

static int ascii_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

static int ascii_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// ------------------------------------------------------- utf8Encode ----

static void utf8_encode_cp(StrBuf* out, unsigned cp)
{
    if (cp < 0x80) {
        sb_append_char(out, (char)cp);
    } else if (cp < 0x800) {
        sb_append_char(out, (char)(0xC0 + cp / 0x40));
        sb_append_char(out, (char)(0x80 + (cp % 0x40)));
    } else if (cp < 0x10000) {
        sb_append_char(out, (char)(0xE0 + cp / 0x1000));
        sb_append_char(out, (char)(0x80 + ((cp / 0x40) % 0x40)));
        sb_append_char(out, (char)(0x80 + (cp % 0x40)));
    } else if (cp < 0x110000) {
        sb_append_char(out, (char)(0xF0 + cp / 0x40000));
        sb_append_char(out, (char)(0x80 + ((cp / 0x1000) % 0x40)));
        sb_append_char(out, (char)(0x80 + ((cp / 0x40) % 0x40)));
        sb_append_char(out, (char)(0x80 + (cp % 0x40)));
    } else {
        sb_append_char(out, '?');
    }
}

// ---------------------------------------------------- CP1252 table ----

static unsigned cp1252_codepoint(unsigned char b)
{
    switch (b) {
    case 0x80: return 0x20AC; case 0x81: return 0x0081;
    case 0x82: return 0x201A; case 0x83: return 0x0192;
    case 0x84: return 0x201E; case 0x85: return 0x2026;
    case 0x86: return 0x2020; case 0x87: return 0x2021;
    case 0x88: return 0x02C6; case 0x89: return 0x2030;
    case 0x8A: return 0x0160; case 0x8B: return 0x2039;
    case 0x8C: return 0x0152; case 0x8D: return 0x008D;
    case 0x8E: return 0x017D; case 0x8F: return 0x008F;
    case 0x90: return 0x0090; case 0x91: return 0x2018;
    case 0x92: return 0x2019; case 0x93: return 0x201C;
    case 0x94: return 0x201D; case 0x95: return 0x2022;
    case 0x96: return 0x2013; case 0x97: return 0x2014;
    case 0x98: return 0x02DC; case 0x99: return 0x2122;
    case 0x9A: return 0x0161; case 0x9B: return 0x203A;
    case 0x9C: return 0x0153; case 0x9D: return 0x009D;
    case 0x9E: return 0x017E; case 0x9F: return 0x0178;
    default: return b;
    }
}

static void from_single_byte(const char* data, size_t len, StrBuf* out)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)data[i];
        unsigned cp;
        if (b < 0x80 || b >= 0xA0) {
            cp = b;
        } else {
            cp = cp1252_codepoint(b);
        }
        utf8_encode_cp(out, cp);
    }
}

// --------------------------------------------------------- UTF-16 ----

static void from_utf16(const char* data, size_t len, int littleEndian,
                       StrBuf* out)
{
    size_t i = 0;
    while (len >= 2 && i + 1 < len) {
        unsigned lo = (unsigned char)data[i];
        unsigned hi = (unsigned char)data[i + 1];
        unsigned u;
        i += 2;
        u = littleEndian ? (lo + hi * 256) : (hi + lo * 256);

        if (u >= 0xD800 && u <= 0xDBFF) {
            unsigned llo, lhi, lo2;
            if (i + 1 >= len) {
                break; // readUnit() nil -> loop exits
            }
            llo = (unsigned char)data[i];
            lhi = (unsigned char)data[i + 1];
            i += 2;
            lo2 = littleEndian ? (llo + lhi * 256) : (lhi + llo * 256);
            if (lo2 >= 0xDC00 && lo2 <= 0xDFFF) {
                unsigned cp =
                    0x10000 + ((u - 0xD800) * 0x400) + (lo2 - 0xDC00);
                utf8_encode_cp(out, cp);
            } else {
                sb_append_char(out, '?'); // low unit already consumed!
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            sb_append_char(out, '?');
        } else {
            utf8_encode_cp(out, u);
        }
    }
}

// ------------------------------------------------- charset naming ----

char* encoding_normalize_charset(const char* name)
{
    StrBuf out;
    const char* p;
    char* s;

    if (name == NULL) {
        return NULL;
    }
    sb_init(&out);
    for (p = name; *p != '\0'; p++) {
        if (ascii_alnum((unsigned char)*p)) {
            sb_append_char(&out, (char)tolower((unsigned char)*p));
        }
    }
    s = sb_detach(&out);
    if (strcmp(s, "utf8") == 0 || strcmp(s, "utf8mb4") == 0) {
        pluto_free(s);
        return pluto_strdup("utf-8");
    }
    if (strcmp(s, "cp1252") == 0 || strcmp(s, "windows1252") == 0 ||
        strcmp(s, "latin1") == 0 || strcmp(s, "iso88591") == 0 ||
        strcmp(s, "iso8859") == 0) {
        pluto_free(s);
        return pluto_strdup("cp1252");
    }
    // utf16 / utf16le / utf16be pass through unchanged
    if (strcmp(s, "ascii") == 0 || strcmp(s, "usascii") == 0) {
        pluto_free(s);
        return pluto_strdup("ascii");
    }
    if (strcmp(s, "shiftjis") == 0 || strcmp(s, "sjis") == 0) {
        pluto_free(s);
        return pluto_strdup("shift_jis");
    }
    return s;
}

// Tries to read an unquoted token value after "charset" starting at j.
// Pattern piece: charset%s*=%s*["']?([%w%+%-_%.]+)
// Returns malloc'd token and sets *next, or NULL (no side effects).
// NOTE: a '"'/'\'' immediately before the token KILLS this alignment
// (only ONE optional quote is consumed pre-capture) — quoted header
// values therefore fail, exactly like the Lua pattern.
static char* take_charset_value(const char* s, size_t slen, size_t j,
                                size_t* next)
{
    size_t k = j;
    StrBuf tok;

    while (k < slen && ascii_ws((unsigned char)s[k])) {
        k++;
    }
    if (k >= slen || s[k] != '=') {
        return NULL;
    }
    k++;
    while (k < slen && ascii_ws((unsigned char)s[k])) {
        k++;
    }
    if (k < slen && (s[k] == '"' || s[k] == '\'')) {
        k++;
    }
    sb_init(&tok);
    while (k < slen) {
        unsigned char c = (unsigned char)s[k];
        if (!ascii_alnum(c) && c != '+' && c != '-' && c != '_' && c != '.') {
            break;
        }
        sb_append_char(&tok, (char)c);
        k++;
    }
    if (tok.len == 0) {
        sb_free(&tok);
        return NULL;
    }
    *next = k;
    return sb_detach(&tok);
}

char* encoding_charset_from_header(const char* contentType)
{
    size_t n, i, pos;
    char* lower;
    char* tok = NULL;

    if (contentType == NULL) {
        return NULL;
    }
    n = strlen(contentType);
    lower = pluto_malloc(n + 1);
    if (lower == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        lower[i] = (char)tolower((unsigned char)contentType[i]);
    }
    lower[n] = '\0';

    // Unanchored Lua match: FIRST "charset" occurrence wins.
    for (i = 0; i + 7 <= n; i++) {
        if (memcmp(lower + i, "charset", 7) == 0) {
            tok = take_charset_value(lower, n, i + 7, &pos);
            if (tok != NULL) {
                break;
            }
        }
    }
    pluto_free(lower);
    if (tok == NULL) {
        return NULL;
    }
    {
        char* norm = encoding_normalize_charset(tok);
        pluto_free(tok);
        return norm;
    }
}

char* encoding_scan_meta_charset(const char* data, size_t len)
{
    size_t limit = len < 1024 ? len : 1024;
    const char* head = data;
    size_t i;

    if (limit == 0) {
        return NULL;
    }

    for (i = 0; i + 5 <= limit; i++) {
        size_t spanStart, spanEnd, occ[64];
        size_t nOcc = 0, k;

        if (memcmp(head + i, "<meta", 5) != 0) {
            continue;
        }
        spanStart = i + 5;
        spanEnd = limit;
        for (k = spanStart; k < limit; k++) {
            if (head[k] == '>') { // [^>]* cannot cross '>'
                spanEnd = k;
                break;
            }
        }

        // Collect "charset" occurrences, try RIGHT-to-LEFT (greedy backtrack).
        for (k = spanStart; k + 7 <= spanEnd && nOcc < 64; k++) {
            if (memcmp(head + k, "charset", 7) == 0) {
                occ[nOcc++] = k;
            }
        }
        while (nOcc > 0) {
            size_t next;
            char* tok =
                take_charset_value(head, spanEnd, occ[--nOcc] + 7, &next);
            if (tok != NULL) {
                char* norm = encoding_normalize_charset(tok);
                pluto_free(tok);
                return norm;
            }
        }
    }
    return NULL;
}

// ------------------------------------------------------------ main ----

static char* copy_bytes(const char* data, size_t len, size_t* outLen)
{
    char* copy = (char*)pluto_malloc(len + 1);
    if (copy == NULL) {
        if (outLen != NULL) {
            *outLen = 0;
        }
        return NULL;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';
    if (outLen != NULL) {
        *outLen = len;
    }
    return copy;
}

static char* finish_sb(StrBuf* sb, size_t* outLen)
{
    size_t n = sb->len;
    char* s = sb_detach(sb);
    if (outLen != NULL) {
        *outLen = n;
    }
    return s;
}

char* encoding_to_utf8(const char* data, size_t len,
                       const char* contentType, size_t* outLen)
{
    char* cs = NULL;
    StrBuf out;

    if (data == NULL || len == 0) {
        return copy_bytes("", 0, outLen);
    }

    // 1. BOM
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        return copy_bytes(data + 3, len - 3, outLen);
    }
    if (len >= 2 && (unsigned char)data[0] == 0xFF &&
        (unsigned char)data[1] == 0xFE) {
        sb_init(&out);
        from_utf16(data + 2, len - 2, 1, &out);
        return finish_sb(&out, outLen);
    }
    if (len >= 2 && (unsigned char)data[0] == 0xFE &&
        (unsigned char)data[1] == 0xFF) {
        sb_init(&out);
        from_utf16(data + 2, len - 2, 0, &out);
        return finish_sb(&out, outLen);
    }

    // 2. transport charset
    cs = encoding_charset_from_header(contentType);

    // 3/4. in-document meta (only when header missing or utf-8)
    if (cs == NULL || strcmp(cs, "utf-8") == 0) {
        char* meta = encoding_scan_meta_charset(data, len);
        if (meta != NULL) {
            pluto_free(cs);
            cs = meta;
        }
    }

    if (cs == NULL) {
        return copy_bytes(data, len, outLen); // assumed UTF-8
    }
    if (strcmp(cs, "cp1252") == 0) {
        sb_init(&out);
        from_single_byte(data, len, &out);
        pluto_free(cs);
        return finish_sb(&out, outLen);
    }
    if (strcmp(cs, "utf16le") == 0) {
        sb_init(&out);
        from_utf16(data, len, 1, &out);
        pluto_free(cs);
        return finish_sb(&out, outLen);
    }
    if (strcmp(cs, "utf16be") == 0) {
        sb_init(&out);
        from_utf16(data, len, 0, &out);
        pluto_free(cs);
        return finish_sb(&out, outLen);
    }
    if (strcmp(cs, "shift_jis") == 0 || strcmp(cs, "eucjp") == 0 ||
        strcmp(cs, "gbk") == 0 || strcmp(cs, "big5") == 0) {
        sb_init(&out);
        from_single_byte(data, len, &out);
        pluto_free(cs);
        return finish_sb(&out, outLen);
    }
    // utf-8 / ascii / unknown -> verbatim
    pluto_free(cs);
    return copy_bytes(data, len, outLen);
}
