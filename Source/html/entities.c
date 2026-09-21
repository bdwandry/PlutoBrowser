/*
 * PlutoBrowser — entities.c
 * HTML Entity Decoder & UTF-8 Sanitizer (port of Source/html/entities.lua).
 *
 * Structure mirrors the Lua reference pass-for-pass (see entities.h). The Lua
 * implementation chains several gsub passes and then a byte loop; this port
 * reproduces the same observable output, including the multi-pass cascade
 * behavior for double-encoded entities.
 */
#include "core/logger.h"
#include "entities.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"
#include "util/strbuf.h"
#include "../core/pluto_mem.h"

#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)
PlaydateAPI *pluto_pd(void);
void pluto_free(void *p);

/* ── Named entity table (Lua NAMED_ENTITIES, entries in reference order) ─── */
typedef struct
{
    const char *name;
    const char *repl;
} NamedEntity;

static const NamedEntity NAMED_ENTITIES[] = {
    { "quot", "\"" },
    { "amp", "&" },
    { "apos", "'" },
    { "lt", "<" },
    { "gt", ">" },
    { "nbsp", " " },
    { "ensp", " " }, { "emsp", " " }, { "thinsp", " " }, { "hairsp", " " }, { "zwsp", "" },
    { "NegativeMediumSpace", " " }, { "VeryThinSpace", " " }, { "ThinSpace", " " },
    { "iexcl", "!" },
    { "cent", "c" },
    { "pound", "L" },
    { "curren", "$" },
    { "yen", "Y" },
    { "brvbar", "|" },
    { "sect", "#" },
    { "uml", ".." },
    { "copy", "(c)" },
    { "ordf", "a" },
    { "laquo", "<<" },
    { "shy", "" },
    { "reg", "(R)" },
    { "macr", "-" },
    { "deg", " deg" },
    { "plusmn", "+/-" },
    { "sup2", "^2" },
    { "sup3", "^3" },
    { "minus", "-" }, { "plus", "+" }, { "times", "x" }, { "divide", "/" },
    { "radic", "sqrt" }, { "infin", "inf" }, { "ne", "!=" }, { "le", "<=" }, { "ge", ">=" },
    { "asymp", "~" }, { "sim", "~" }, { "cong", "~" }, { "sdot", "*" },
    { "in", "in " }, { "notin", "not in " }, { "sum", "sum " }, { "prod", "prod " }, { "int", "int " },
    { "part", "d" }, { "Delta", "D" }, { "pi", "pi" }, { "alpha", "alpha" },
    { "rarr", "->" }, { "larr", "<-" }, { "uarr", "^" }, { "darr", "v" },
    { "and", "and " }, { "or", "or " },
    { "prime", "'" }, { "Prime", "\"" }, { "ang", "L" }, { "perp", "_|_" },
    { "acute", "'" },
    { "micro", "u" },
    { "para", "P" },
    { "middot", "*" },
    { "cedil", "," },
    { "sup1", "^1" },
    { "ordm", "o" },
    { "raquo", ">>" },
    { "frac14", "1/4" },
    { "frac12", "1/2" },
    { "frac34", "3/4" },
    { "iquest", "?" },
    { "Agrave", "A" }, { "Aacute", "A" }, { "Acirc", "A" }, { "Atilde", "A" }, { "Auml", "A" }, { "Aring", "A" },
    { "Egrave", "E" }, { "Eacute", "E" }, { "Ecirc", "E" }, { "Euml", "E" },
    { "Igrave", "I" }, { "Iacute", "I" }, { "Icirc", "I" }, { "Iuml", "I" },
    { "Ograve", "O" }, { "Oacute", "O" }, { "Ocirc", "O" }, { "Otilde", "O" }, { "Ouml", "O" },
    { "Ugrave", "U" }, { "Uacute", "U" }, { "Ucirc", "U" }, { "Uuml", "U" },
    { "agrave", "a" }, { "aacute", "a" }, { "acirc", "a" }, { "atilde", "a" }, { "auml", "a" }, { "aring", "a" },
    { "egrave", "e" }, { "eacute", "e" }, { "ecirc", "e" }, { "euml", "e" },
    { "igrave", "i" }, { "iacute", "i" }, { "icirc", "i" }, { "iuml", "i" },
    { "ograve", "o" }, { "oacute", "o" }, { "ocirc", "o" }, { "otilde", "o" }, { "ouml", "o" },
    { "ugrave", "u" }, { "uacute", "u" }, { "ucirc", "u" }, { "uuml", "u" },
    { "mdash", " -- " },
    { "ndash", " - " },
    { "lsquo", "'" },
    { "rsquo", "'" },
    { "ldquo", "\"" },
    { "rdquo", "\"" },
    { "hellip", "..." },
    { "trade", "(TM)" },
    { "bull", "*" },
    { "euro", "EUR" },
    { "check", "[v]" },
    { "cross", "[x]" },
    /* ── WHATWG named-entity expansion (entities.html): common typography,
     * punctuation, currency, arrows, math and Greek that appear on real
     * pages. ASCII-safe approximations, matching the reference's approach. */
    /* typography / punctuation */
    { "dagger", "[x]" }, { "Dagger", "[x][x]" },
    { "lsquor", "," }, { "sbquo", "," }, { "bdquo", "\"" }, { "lsaquo", "<" },
    { "rsaquo", ">" }, { "OElig", "OE" }, { "oelig", "oe" },
    { "Scaron", "S" }, { "scaron", "s" }, { "Yuml", "Y" },
    { "circ", "^" }, { "tilde", "~" },
    /* currency */
    { "szlig", "ss" }, { "fnof", "f" },
    /* Latin Extended additions */
    { "Ntilde", "N" }, { "ntilde", "n" }, { "Ccedil", "C" }, { "ccedil", "c" },
    { "Oslash", "O" }, { "oslash", "o" }, { "AElig", "AE" }, { "aelig", "ae" },
    { "Iexcl", "!" },
    /* Greek (most common on real pages) */
    { "Alpha", "A" }, { "Beta", "B" }, { "Gamma", "G" }, { "gamma", "y" },
    { "delta", "d" }, { "Epsilon", "E" }, { "epsilon", "e" },
    { "Zeta", "Z" }, { "zeta", "z" }, { "Eta", "E" }, { "eta", "n" },
    { "Theta", "TH" }, { "theta", "th" }, { "Iota", "I" }, { "iota", "i" },
    { "Kappa", "K" }, { "kappa", "k" }, { "Lambda", "L" }, { "lambda", "l" },
    { "Mu", "M" }, { "mu", "u" }, { "Nu", "N" }, { "nu", "v" },
    { "Xi", "X" }, { "xi", "x" }, { "Omicron", "O" }, { "omicron", "o" },
    { "Pi", "Pi" }, { "rho", "p" }, { "Sigma", "S" }, { "sigma", "s" },
    { "Tau", "T" }, { "tau", "t" }, { "Upsilon", "Y" }, { "upsilon", "u" },
    { "Phi", "PH" }, { "phi", "ph" }, { "Chi", "X" }, { "chi", "x" },
    { "Psi", "PS" }, { "psi", "ps" }, { "Omega", "OM" }, { "omega", "w" },
    /* arrows */
    { "harr", "<->" }, { "crarr", "<-'" }, { "Larr", "<-" }, { "Rarr", "=>" },
    { "dArr", "=>" }, { "uArr", "^" }, { "lArr", "<=" }, { "hArr", "<=>" },
    { "rang", ">" }, { "loz", "<>" }, { "spades", "[S]" },
    { "clubs", "[C]" }, { "hearts", "[H]" }, { "diams", "[D]" },
    /* math extras */
    { "there4", ":." }, { "nsup", ">!" }, { "nsub", "!<" },
    { "sube", "<=" }, { "supe", ">=" }, { "oplus", "(+)" }, { "otimes", "(x)" },
    { "cup", "U" }, { "empty", "{}" }, { "nabla", "grad" },
    { "prop", "oc" }, { "vee", "v" }, { "wedge", "^" },
};
#define NAMED_ENTITY_COUNT (sizeof(NAMED_ENTITIES) / sizeof(NAMED_ENTITIES[0]))

/* Lua tables silently overwrite duplicate keys; resolve "not" (defined with
 * '~' then later with 'not ') the same way — last assignment wins. */
static const char *named_lookup(const char *name, size_t len)
{
    if (len == 3 && strncmp(name, "not", 3) == 0)
    {
        return "not "; /* duplicate-key overwrite parity */
    }
    for (size_t i = 0; i < NAMED_ENTITY_COUNT; i++)
    {
        if (strlen(NAMED_ENTITIES[i].name) == len &&
            strncmp(NAMED_ENTITIES[i].name, name, len) == 0)
        {
            return NAMED_ENTITIES[i].repl;
        }
    }
    return NULL;
}

/* ── MATH_CP map (shared by the numeric passes) ──────────────────────────── */
typedef struct
{
    long cp;
    const char *repl;
} MathCp;

static const MathCp MATH_CP[] = {
    { 176, "deg" }, { 177, "+/-" }, { 178, "^2" }, { 179, "^3" }, { 183, "*" },
    { 215, "x" }, { 247, "/" }, { 960, "pi" }, { 916, "D" },
    { 8706, "d" }, { 8712, "in " }, { 8719, "prod " }, { 8721, "sum " },
    { 8722, "-" }, { 8730, "sqrt" }, { 8734, "inf" }, { 8747, "int " },
    { 8776, "~" }, { 8800, "!=" }, { 8804, "<=" }, { 8805, ">=" },
    { 8592, "<-" }, { 8593, "^" }, { 8594, "->" }, { 8595, "v" },
};
#define MATH_CP_COUNT (sizeof(MATH_CP) / sizeof(MATH_CP[0]))

static const char *math_cp_lookup(long cp)
{
    for (size_t i = 0; i < MATH_CP_COUNT; i++)
    {
        if (MATH_CP[i].cp == cp)
        {
            return MATH_CP[i].repl;
        }
    }
    return NULL;
}

/* ── Numeric codepoint → ASCII (shared tail of both numeric passes) ──────── */
static const char *cp_special_or_math(long num, int hexPass)
{
    if (hexPass)
    {
        if (num == 0xA0 || num == 0x202F || num == 0x2009)
        {
            return " ";
        }
        if (num == 0x2013)
        {
            return " - ";
        }
        if (num == 0x2014)
        {
            return " -- ";
        }
        if (num == 0x2018 || num == 0x2019)
        {
            return "'";
        }
        if (num == 0x201C || num == 0x201D)
        {
            return "\"";
        }
        if (num == 0x2026)
        {
            return "...";
        }
        if (num == 0x2022)
        {
            return "*";
        }
    }
    else
    {
        if (num == 160 || num == 8239 || num == 8201 || num == 8200)
        {
            return " ";
        }
        if (num == 8211)
        {
            return " - ";
        }
        if (num == 8212)
        {
            return " -- ";
        }
        if (num == 8216 || num == 8217 || num == 8218)
        {
            return "'";
        }
        if (num == 8220 || num == 8221 || num == 8222)
        {
            return "\"";
        }
        if (num == 8230)
        {
            return "...";
        }
        if (num == 8226)
        {
            return "*";
        }
    }
    return math_cp_lookup(num);
}

/* Append the ASCII translation of `num` to sb. 0 ok, -1 alloc failure. */
static int append_cp_translation(StrBuf *sb, long num, int hexPass)
{
    const char *m = cp_special_or_math(num, hexPass);
    if (m)
    {
        return strbuf_append(sb, m);
    }
    if (num >= 32 && num <= 126)
    {
        return strbuf_append_char(sb, (char)num);
    }
    return strbuf_append_char(sb, ' ');
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Pass 1/2/3: entity decoding (decimal, hex, named). Each pass scans once,
 * left to right, replacing the first complete &…; pattern at each position —
 * the same single-sweep-with-atomic-replacement semantics as Lua gsub.
 */
static int decode_numeric_pass(StrBuf *out, const char *text, int hexPass)
{
    const char *p = text;
    while (*p)
    {
        const char *amp = strchr(p, '&');
        if (!amp)
        {
            if (strbuf_append(out, p) != 0)
            {
                return -1;
            }
            break;
        }
        if (amp > p && strbuf_append_n(out, p, (size_t)(amp - p)) != 0)
        {
            return -1;
        }
        p = amp;
        const char *q = p + 1;
        if (*q != '#')
        {
            goto literal; /* numeric entities always start &# */
        }
        q++;
        if (hexPass)
        {
            if (*q != 'x' && *q != 'X')
            {
                goto literal;
            }
            q++;
            const char *h = q;
            while (isxdigit((unsigned char)*q))
            {
                q++;
            }
            if (q == h || *q != ';' || (q - h) > 7)
            {
                goto literal;
            }
            char hexbuf[8];
            memcpy(hexbuf, h, (size_t)(q - h));
            hexbuf[q - h] = '\0';
            if (append_cp_translation(out, strtol(hexbuf, NULL, 16), hexPass) != 0)
            {
                return -1;
            }
            p = q + 1;
            continue;
        }
        else
        {
            if (!isdigit((unsigned char)*q))
            {
                goto literal;
            }
            const char *d = q;
            while (isdigit((unsigned char)*q))
            {
                q++;
            }
            if (*q != ';' || (q - d) > 8)
            {
                goto literal;
            }
            char decbuf[10];
            memcpy(decbuf, d, (size_t)(q - d));
            decbuf[q - d] = '\0';
            if (append_cp_translation(out, strtol(decbuf, NULL, 10), hexPass) != 0)
            {
                return -1;
            }
            p = q + 1;
            continue;
        }
    literal:
        /* Not a valid entity for this pass: emit '&' and continue after it. */
        if (strbuf_append_char(out, '&') != 0)
        {
            return -1;
        }
        p++;
    }
    return 0;
}

static int decode_named_pass(StrBuf *out, const char *text)
{
    const char *p = text;
    while (*p)
    {
        const char *amp = strchr(p, '&');
        if (!amp)
        {
            if (strbuf_append(out, p) != 0)
            {
                return -1;
            }
            break;
        }
        if (amp > p && strbuf_append_n(out, p, (size_t)(amp - p)) != 0)
        {
            return -1;
        }
        p = amp;
        const char *q = p + 1;
        const char *n = q;
        while (isalpha((unsigned char)*q))
        {
            q++;
        }
        if (q == n || *q != ';' || (q - n) > 24)
        {
            if (strbuf_append_char(out, '&') != 0)
            {
                return -1;
            }
            p++;
            continue;
        }
        const char *repl = named_lookup(n, (size_t)(q - n));
        /* Lua: return NAMED_ENTITIES[name] or (" " .. name .. " ") — the
         * unknown replacement is a single " name " token (no extra space). */
        if (repl)
        {
            if (strbuf_append(out, repl) != 0)
            {
                return -1;
            }
        }
        else if (strbuf_appendf(out, " %.*s ", (int)(q - n), n) != 0)
        {
            return -1;
        }
        p = q + 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Pass 4: UTF-8 multi-byte sequence cleanup (Lua's ordered gsub list).
 */
typedef struct
{
    const char *seq;
    const char *repl;
} ByteSeq;

static const ByteSeq UTF8_CLEANUP[] = {
    { "\xef\xbb\xbf", "" },
    { "\xc2\xa0", " " },
    { "\xe2\x80\x93", " - " },
    { "\xe2\x80\x94", " -- " },
    { "\xe2\x80\x98", "'" },
    { "\xe2\x80\x99", "'" },
    { "\xe2\x80\x9c", "\"" },
    { "\xe2\x80\x9d", "\"" },
    { "\xe2\x80\xa6", "..." },
    { "\xe2\x80\xa2", "*" },
    { "\xc2\xb7", "*" },
    { "\xe2\x88\x92", "-" },
    { "\xc2\xb1", "+/-" },
    { "\xe2\x88\x9a", "sqrt" },
    { "\xe2\x88\x9e", "inf" },
    { "\xe2\x89\xa4", "<=" },
    { "\xe2\x89\xa5", ">=" },
    { "\xe2\x89\xa0", "!=" },
    { "\xe2\x89\x88", "~" },
    { "\xe2\x88\x91", "sum " },
    { "\xe2\x88\x8f", "prod " },
    { "\xe2\x88\x88", "in " },
    { "\xe2\x88\xa3", "|" },
    { "\xe2\x86\x92", "->" },
    { "\xe2\x86\x90", "<-" },
    { "\xe2\x86\x91", "^" },
    { "\xe2\x86\x93", "v" },
};
#define UTF8_CLEANUP_COUNT (sizeof(UTF8_CLEANUP) / sizeof(UTF8_CLEANUP[0]))

static int utf8_cleanup_pass(StrBuf *out, const char *text)
{
    size_t n = strlen(text);
    size_t i = 0;
    while (i < n)
    {
        int matched = 0;
        for (size_t s = 0; s < UTF8_CLEANUP_COUNT && !matched; s++)
        {
            size_t sl = strlen(UTF8_CLEANUP[s].seq);
            if (i + sl <= n && memcmp(text + i, UTF8_CLEANUP[s].seq, sl) == 0)
            {
                if (strbuf_append(out, UTF8_CLEANUP[s].repl) != 0)
                {
                    return -1;
                }
                i += sl;
                matched = 1;
            }
        }
        if (!matched)
        {
            if (strbuf_append_char(out, text[i]) != 0)
            {
                return -1;
            }
            i++;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Pass 5: transliteration + cleaning byte loop (Lua's T table / M() entries).
 * 2-byte sequences whose combined codepoint is in the table map to the table
 * string; any other high byte (or overlong 3/4-byte lead) becomes ' '.
 */
static const char *translit_lookup(unsigned int cp)
{
    switch (cp)
    {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
    case 0xC6: return "AE";
    case 0xC7: return "C";
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
    case 0xD0: return "D";
    case 0xD1: return "N";
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: return "O";
    case 0xD7: return "x";
    case 0xD8: return "O";
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
    case 0xDD: return "Y";
    case 0xDE: return "TH";
    case 0xDF: return "ss";
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
    case 0xE6: return "ae";
    case 0xE7: return "c";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
    case 0xF0: return "d";
    case 0xF1: return "n";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: return "o";
    case 0xF7: return "/";
    case 0xF8: return "o";
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
    case 0xFD: return "y";
    case 0xFE: return "th";
    case 0xFF: return "y";
    /* Lua Latin Extended-A block (0x100–0x17E) */
    case 0x100: case 0x102: case 0x104: return "A";
    case 0x101: case 0x103: case 0x105: return "a";
    case 0x10C: return "C";
    case 0x10D: return "c";
    case 0x10E: case 0x110: return "D";
    case 0x10F: case 0x111: return "d";
    case 0x112: return "E";
    case 0x113: return "e";
    case 0x11A: return "E";
    case 0x11B: return "e";
    case 0x11E: case 0x120: return "G";
    case 0x11F: case 0x121: return "g";
    case 0x124: case 0x126: return "H";
    case 0x125: case 0x127: return "h";
    case 0x12A: return "I";
    case 0x12B: return "i";
    case 0x130: return "I";
    case 0x131: return "i";
    case 0x134: return "J";
    case 0x135: return "j";
    case 0x136: return "K";
    case 0x137: case 0x138: return "k";
    case 0x13B: case 0x13D: case 0x141: return "L";
    case 0x13C: case 0x13E: case 0x142: return "l";
    case 0x143: case 0x145: case 0x147: return "N";
    case 0x144: case 0x146: case 0x148: return "n";
    case 0x150: return "O";
    case 0x151: return "o";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x154: case 0x158: return "R";
    case 0x155: case 0x159: return "r";
    case 0x15A: case 0x15C: case 0x15E: case 0x160: return "S";
    case 0x15B: case 0x15D: case 0x15F: case 0x161: return "s";
    case 0x162: case 0x164: case 0x166: return "T";
    case 0x163: case 0x165: case 0x167: return "t";
    case 0x16A: case 0x16C: case 0x16E: case 0x170: case 0x172: return "U";
    case 0x16B: case 0x16D: case 0x16F: case 0x171: case 0x173: return "u";
    case 0x174: return "W";
    case 0x175: return "w";
    case 0x176: return "Y";
    case 0x177: return "y";
    case 0x178: return "Y";
    case 0x179: case 0x17B: case 0x17D: return "Z";
    case 0x17A: case 0x17C: case 0x17E: return "z";
    default: return NULL;
    }
}

static int translit_clean_pass(StrBuf *out, const char *text)
{
    size_t n = strlen(text);
    size_t i = 0;
    while (i < n)
    {
        unsigned char b = (unsigned char)text[i];
        if ((b >= 32 && b <= 126) || b == 10 || b == 13 || b == 9)
        {
            if (strbuf_append_char(out, (char)b) != 0)
            {
                return -1;
            }
            i++;
        }
        else if (b >= 192 && b <= 223 && i < n - 1)
        {
            unsigned char c = (unsigned char)text[i + 1];
            unsigned int cp = ((b % 0x20) * 64) + (c % 0x40);
            const char *t = translit_lookup(cp);
            if (strbuf_append(out, t ? t : " ") != 0)
            {
                return -1;
            }
            i += 2;
        }
        else if (b >= 224 && b <= 239 && i + 1 < n - 1)
        {
            if (strbuf_append_char(out, ' ') != 0)
            {
                return -1;
            }
            i += 3;
        }
        else
        {
            if (strbuf_append_char(out, ' ') != 0)
            {
                return -1;
            }
            i++;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 */

char *entities_decode(const char *text)
{
    logger_stack_touch();
    if (!text || !text[0])
    {
        char *empty = (char *)PLUTO_MALLOC(1);
        if (empty)
        {
            empty[0] = '\0';
        }
        return empty;
    }

    /* Fast path: no '&' and no byte >= 0x80 → unchanged (Lua parity). */
    int hasAmp = 0, hasHigh = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        if (*p == '&')
        {
            hasAmp = 1;
        }
        else if (*p >= 0x80)
        {
            hasHigh = 1;
        }
    }
    if (!hasAmp && !hasHigh)
    {
        char *dup = (char *)PLUTO_MALLOC(strlen(text) + 1);
        if (dup)
        {
            strcpy(dup, text);
        }
        return dup;
    }

    /* Pass 1: decimal numeric entities. */
    StrBuf s1;
    if (strbuf_init(&s1) != 0)
    {
        return NULL;
    }
    if (hasAmp)
    {
        if (decode_numeric_pass(&s1, text, 0) != 0)
        {
            strbuf_free(&s1);
            return NULL;
        }
    }
    else
    {
        strbuf_append(&s1, text);
    }

    /* Pass 2: hex numeric entities. */
    StrBuf s2;
    if (strbuf_init(&s2) != 0)
    {
        strbuf_free(&s1);
        return NULL;
    }
    if (hasAmp)
    {
        if (decode_numeric_pass(&s2, s1.data, 1) != 0)
        {
            strbuf_free(&s1);
            strbuf_free(&s2);
            return NULL;
        }
        strbuf_free(&s1);
    }
    else
    {
        strbuf_append(&s2, s1.data);
        strbuf_free(&s1);
    }

    /* Pass 3: named entities. */
    StrBuf s3;
    if (strbuf_init(&s3) != 0)
    {
        strbuf_free(&s2);
        return NULL;
    }
    if (hasAmp)
    {
        if (decode_named_pass(&s3, s2.data) != 0)
        {
            strbuf_free(&s2);
            strbuf_free(&s3);
            return NULL;
        }
        strbuf_free(&s2);
        /* Lua: `if not hasHigh then return text end` — pure-ASCII input with
         * entities skips the byte cleanup. */
        if (!hasHigh)
        {
            return strbuf_detach(&s3);
        }
    }
    else
    {
        strbuf_append(&s3, s2.data);
        strbuf_free(&s2);
    }

    /* Pass 4: UTF-8 sequence cleanup. */
    StrBuf s4;
    if (strbuf_init(&s4) != 0)
    {
        strbuf_free(&s3);
        return NULL;
    }
    if (utf8_cleanup_pass(&s4, s3.data) != 0)
    {
        strbuf_free(&s3);
        strbuf_free(&s4);
        return NULL;
    }
    strbuf_free(&s3);

    /* Pass 5: transliterate + strip. */
    StrBuf s5;
    if (strbuf_init(&s5) != 0)
    {
        strbuf_free(&s4);
        return NULL;
    }
    if (translit_clean_pass(&s5, s4.data) != 0)
    {
        strbuf_free(&s4);
        strbuf_free(&s5);
        return NULL;
    }
    strbuf_free(&s4);

    return strbuf_detach(&s5);
}

char *entities_encode(const char *text)
{
    static const char *amp = "&amp;";
    static const char *lt = "&lt;";
    static const char *gt = "&gt;";
    static const char *quot = "&quot;";

    StrBuf sb;
    if (strbuf_init(&sb) != 0)
    {
        return NULL;
    }
    if (text)
    {
        for (const char *p = text; *p; p++)
        {
            const char *rep = NULL;
            switch (*p)
            {
            case '&': rep = amp; break;
            case '<': rep = lt; break;
            case '>': rep = gt; break;
            case '"': rep = quot; break;
            default: break;
            }
            if (rep)
            {
                if (strbuf_append(&sb, rep) != 0)
                {
                    strbuf_free(&sb);
                    return NULL;
                }
            }
            else if (strbuf_append_char(&sb, *p) != 0)
            {
                strbuf_free(&sb);
                return NULL;
            }
        }
    }
    return strbuf_detach(&sb);
}
