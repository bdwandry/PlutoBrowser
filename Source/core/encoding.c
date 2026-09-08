/*
 * PlutoBrowser — encoding.c
 * Charset detection & conversion to UTF-8 (port of Source/core/encoding.lua).
 * See encoding.h. Reference values (CP1252 table, precedence order, charset
 * aliases, 1024-byte meta scan window) are preserved exactly.
 */
#include <string.h>

#include "core/encoding.h"
#include "util/strbuf.h"

extern void pluto_free(void *p);

/* ── windows-1252 code points for bytes 0x80-0x9F ──────────────────────────── */
/* (bytes outside this range decode as Latin-1: byte N -> U+00NN) */
static const unsigned int CP1252[0x20] = {
    0x20AC, 0x0081, 0x201A, 0x0192, /* 80-83 */
    0x201E, 0x2026, 0x2020, 0x2021, /* 84-87 */
    0x02C6, 0x2030, 0x0160, 0x2039, /* 88-8B */
    0x0152, 0x008D, 0x017D, 0x008F, /* 8C-8F */
    0x0090, 0x2018, 0x2019, 0x201C, /* 90-93 */
    0x201D, 0x2022, 0x2013, 0x2014, /* 94-97 */
    0x02DC, 0x2122, 0x0161, 0x203A, /* 98-9B */
    0x0153, 0x009D, 0x017E, 0x0178  /* 9C-9F */
};

/* ── UTF-8 encoding of a single code point (mirrors Lua utf8Encode) ────────── */
static void utf8_encode(StrBuf *out, unsigned int cp)
{
    if (cp < 0x80)
    {
        strbuf_appendf(out, "%c", (char)cp);
    }
    else if (cp < 0x800)
    {
        strbuf_appendf(out, "%c%c",
                       (char)(0xC0 + (cp / 0x40)),
                       (char)(0x80 + (cp % 0x40)));
    }
    else if (cp < 0x10000)
    {
        strbuf_appendf(out, "%c%c%c",
                       (char)(0xE0 + (cp / 0x1000)),
                       (char)(0x80 + ((cp / 0x40) % 0x40)),
                       (char)(0x80 + (cp % 0x40)));
    }
    else if (cp < 0x110000)
    {
        strbuf_appendf(out, "%c%c%c%c",
                       (char)(0xF0 + (cp / 0x40000)),
                       (char)(0x80 + ((cp / 0x1000) % 0x40)),
                       (char)(0x80 + ((cp / 0x40) % 0x40)),
                       (char)(0x80 + (cp % 0x40)));
    }
    else
    {
        strbuf_appendf(out, "?");
    }
}

/* windows-1252 / iso-8859-1 / ascii byte string -> UTF-8 */
static char *from_single_byte(const unsigned char *data, size_t len)
{
    StrBuf sb;
    strbuf_init(&sb);
    for (size_t i = 0; i < len; i++)
    {
        unsigned int cp;
        unsigned char b = data[i];
        if (b < 0x80)
        {
            cp = b;
        }
        else if (b >= 0xA0)
        {
            cp = b; /* Latin-1 range */
        }
        else
        {
            cp = CP1252[b - 0x80];
        }
        utf8_encode(&sb, cp);
    }
    return sb.data; /* ownership transfers to caller */
}

/* UTF-16 (big or little endian) byte string -> UTF-8 */
static char *from_utf16(const unsigned char *data, size_t len, int littleEndian)
{
    StrBuf sb;
    strbuf_init(&sb);
    size_t i = 0;
    for (;;)
    {
        if (i + 2 > len)
        {
            break;
        }
        unsigned int lo = data[i];
        unsigned int hi = data[i + 1];
        i += 2;
        unsigned int u = littleEndian ? (lo + hi * 256) : (hi + lo * 256);

        if (u >= 0xD800 && u <= 0xDBFF)
        {
            /* high surrogate: try to pair */
            if (i + 2 <= len)
            {
                unsigned int l2 = data[i];
                unsigned int h2 = data[i + 1];
                unsigned int u2 = littleEndian ? (l2 + h2 * 256) : (h2 + l2 * 256);
                if (u2 >= 0xDC00 && u2 <= 0xDFFF)
                {
                    i += 2;
                    unsigned int cp = 0x10000 + ((u - 0xD800) * 0x400) + (u2 - 0xDC00);
                    utf8_encode(&sb, cp);
                }
                else
                {
                    strbuf_appendf(&sb, "?");
                }
            }
            else
            {
                strbuf_appendf(&sb, "?");
            }
        }
        else if (u >= 0xDC00 && u <= 0xDFFF)
        {
            strbuf_appendf(&sb, "?");
        }
        else
        {
            utf8_encode(&sb, u);
        }
    }
    return sb.data;
}

/* Normalize a charset name to a lower-cased canonical token.
 * Writes into `out` (capacity >= 64). Returns 0 when `name` is NULL. */
static int normalize_charset(const char *name, char *out, size_t outCap)
{
    if (!name)
    {
        return 0;
    }
    /* Lua: gsub(name, "[^%w]", "") then lower — strip non-alphanumerics. */
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < outCap; p++)
    {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[o] = '\0';

    if (strcmp(out, "utf8") == 0 || strcmp(out, "utf8mb4") == 0)
    {
        strcpy(out, "utf-8");
    }
    else if (strcmp(out, "cp1252") == 0 || strcmp(out, "windows1252") == 0 ||
             strcmp(out, "latin1") == 0 || strcmp(out, "iso88591") == 0 ||
             strcmp(out, "iso8859") == 0)
    {
        strcpy(out, "cp1252");
    }
    /* utf16 / utf16le / utf16be / ascii / usascii pass through as-is;
     * shiftjis / sjis → shift_jis. */
    else if (strcmp(out, "shiftjis") == 0 || strcmp(out, "sjis") == 0)
    {
        strcpy(out, "shift_jis");
    }
    return 1;
}

/* Case-insensitive substring find (haystack, needle both NUL-terminated). */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
    {
        return NULL;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0)
    {
        return haystack;
    }
    for (const char *h = haystack; *h; h++)
    {
        size_t i = 0;
        while (i < nlen && h[i])
        {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z')
            {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b)
            {
                break;
            }
            i++;
        }
        if (i == nlen)
        {
            return h;
        }
    }
    return NULL;
}

/* Scan the first `limit` bytes of an HTML document for a charset declaration.
 * Lua used two patterns:
 *   '<meta[^>]*charset%s*=%s*["\']?([%w%+%-_%.]+)'
 *   '<meta[^>]*content%s*=%s*["\']?[^"\'>]*charset%s*=%s*["\']?([%w%+%-_%.]+)'
 * The second is subsumed by the first for our purposes (both end in the same
 * "charset = token" capture); we scan case-insensitively for "charset",
 * optional spaces, '=', optional spaces, optional quote, then the token. */
static int scan_meta_charset(const unsigned char *data, size_t len, size_t limit,
                             char *out, size_t outCap)
{
    size_t n = (limit && limit < len) ? limit : len;
    /* Copy into a NUL-terminated scratch buffer, lowercased, for ci_strstr. */
    static char head[1025];
    if (n >= sizeof(head))
    {
        n = sizeof(head) - 1;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = (char)data[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        head[i] = c;
    }
    head[n] = '\0';

    const char *p = head;
    while ((p = ci_strstr(p, "charset")) != NULL)
    {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
        {
            q++;
        }
        if (*q == '=')
        {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            {
                q++;
            }
            if (*q == '"' || *q == '\'')
            {
                q++;
            }
            /* extract token [A-Za-z0-9+-.] (plus _ per Lua class) */
            char tok[64];
            size_t t = 0;
            while (*q && t + 1 < sizeof(tok))
            {
                char c = *q;
                int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         (c >= 'A' && c <= 'Z') || c == '+' || c == '-' ||
                         c == '_' || c == '.';
                if (!ok)
                {
                    break;
                }
                tok[t++] = c;
                q++;
            }
            tok[t] = '\0';
            if (t > 0 && normalize_charset(tok, out, outCap))
            {
                return 1;
            }
        }
        p += 7;
    }
    return 0;
}

/* Extract charset from an HTTP Content-Type header value. */
static int charset_from_header(const char *contentType, char *out, size_t outCap)
{
    if (!contentType)
    {
        return 0;
    }
    const char *p = ci_strstr(contentType, "charset");
    if (!p)
    {
        return 0;
    }
    p += 7;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p != '=')
    {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p == '"' || *p == '\'')
    {
        p++;
    }
    char tok[64];
    size_t t = 0;
    while (*p && t + 1 < sizeof(tok))
    {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '_' || c == '.';
        if (!ok)
        {
            break;
        }
        tok[t++] = c;
        p++;
    }
    tok[t] = '\0';
    if (t == 0)
    {
        return 0;
    }
    return normalize_charset(tok, out, outCap);
}

int encoding_is_utf8(const unsigned char *data, size_t len)
{
    size_t i = 0;
    while (i < len)
    {
        unsigned char b = data[i];
        if (b < 0x80)
        {
            i++;
        }
        else if ((b & 0xE0) == 0xC0 && i + 1 < len)
        {
            if ((data[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        }
        else if ((b & 0xF0) == 0xE0 && i + 2 < len)
        {
            if ((data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) return 0;
            i += 3;
        }
        else if ((b & 0xF8) == 0xF0 && i + 3 < len)
        {
            if ((data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80)
            {
                return 0;
            }
            i += 4;
        }
        else
        {
            return 0;
        }
    }
    return 1;
}

char *encoding_to_utf8(const unsigned char *data, size_t len, const char *contentType)
{
    if (!data || len == 0)
    {
        /* Lua returned the (nil/empty) input unchanged; return an empty heap
         * string so the caller always owns a freeable buffer. */
        StrBuf sb;
        strbuf_init(&sb);
        return sb.data;
    }

    /* 1. Byte order mark. */
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    {
    /* UTF-8 BOM: strip it, copy the rest byte-identical. */
    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append_n(&sb, (const char *)(data + 3), len - 3);
    return sb.data;
    }
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        return from_utf16(data + 2, len - 2, 1);
    }
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
        return from_utf16(data + 2, len - 2, 0);
    }

    /* 2. Transport charset. */
    char cs[64];
    int have = charset_from_header(contentType, cs, sizeof(cs));

    /* 3/4. In-document <meta> declarations. */
    if (!have || strcmp(cs, "utf-8") == 0)
    {
        char meta[64];
        if (scan_meta_charset(data, len, 1024, meta, sizeof(meta)))
        {
            strcpy(cs, meta);
            have = 1;
        }
    }

    /* Anything we can't decode stays as-is (assumed UTF-8). */
    if (!have)
    {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_n(&sb, (const char *)data, len);
        return sb.data;
    }
    if (strcmp(cs, "utf-8") == 0 || strcmp(cs, "ascii") == 0)
    {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_n(&sb, (const char *)data, len);
        return sb.data;
    }
    if (strcmp(cs, "cp1252") == 0)
    {
        return from_single_byte(data, len);
    }
    if (strcmp(cs, "utf16le") == 0)
    {
        return from_utf16(data, len, 1);
    }
    if (strcmp(cs, "utf16be") == 0)
    {
        return from_utf16(data, len, 0);
    }

    /* Fallback for exotic encodings we cannot handle: best-effort
     * single-byte decode so non-ASCII text still shows something readable. */
    if (strcmp(cs, "shift_jis") == 0 || strcmp(cs, "eucjp") == 0 ||
        strcmp(cs, "gbk") == 0 || strcmp(cs, "big5") == 0)
    {
        return from_single_byte(data, len);
    }

    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append_n(&sb, (const char *)data, len);
    return sb.data;
}
