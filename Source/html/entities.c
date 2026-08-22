// entities.c — HTML entity decoder & UTF-8 sanitizer
// (C port of html/entities.lua).
//
// Ground truth: host-Lua oracle p06_oracle.lua on the REAL entities.lua.
// Verified quirks preserved:
//   - fast path: text without '&' and without bytes >=0x80 is returned
//     VERBATIM (raw control bytes included)
//   - numeric/named passes only run when '&' present; fixed UTF-8 cleanup +
//     transliteration only run when a high byte was present
//   - NAMED table: Lua constructor duplicate keys resolve LAST-WINS
//     ("not" -> "not "); values like " deg"/"in "/"not " keep exact spacing;
//     &frac12; etc. contain digits and NEVER match %a+ -> stay verbatim
//   - unknown names become " name "; case-sensitive (&AMP; -> " AMP ")
//   - decimal branch specials include 0x201A/0x201E; hex branch does NOT
//     (source asymmetry): &#x201a; -> " "
//   - step 4: literal UTF-8 sequence rewrites applied sequentially
//   - step 5 final loop: printable 32..126 + \n\r\t kept; lead C2..DF with a
//     following byte decodes cp=((b%0x20)*64)+(c%0x40) through T (or " ");
//     lead E0..EF with two more bytes -> " " advance 3; anything else ->
//     " " advance 1 (truncated tails therefore widen)
//   - Tasks.yieldCheck() once per input byte of the final loop
#include <stdio.h>
#include <string.h>

#include "../core/tasks.h"
#include "../util/mem.h"
#include "../util/strbuf.h"
#include "entities.h"

static struct PlaydateAPI* s_pd = NULL;

void entities_init(struct PlaydateAPI* pd) { s_pd = pd; }

// ------------------------------------------------ named entities ----

typedef struct NamedEnt {
    const char* name;
    const char* value;
} NamedEnt;

// Transcribed from NAMED_ENTITIES in html/entities.lua.
// Duplicate Lua keys resolved last-wins ("not", prime/Prime harmless).
static const NamedEnt NAMED[] = {
    {"quot", "\""},       {"amp", "&"},         {"apos", "'"},
    {"lt", "<"},          {"gt", ">"},          {"nbsp", " "},
    {"ensp", " "},        {"emsp", " "},        {"thinsp", " "},
    {"hairsp", " "},      {"zwsp", ""},
    {"NegativeMediumSpace", " "}, {"VeryThinSpace", " "},
    {"ThinSpace", " "},
    {"iexcl", "!"},       {"cent", "c"},        {"pound", "L"},
    {"curren", "$"},      {"yen", "Y"},         {"brvbar", "|"},
    {"sect", "#"},        {"uml", ".."},        {"copy", "(c)"},
    {"ordf", "a"},        {"laquo", "<<"},      {"not", "not "},
    {"shy", ""},          {"reg", "(R)"},       {"macr", "-"},
    {"deg", " deg"},      {"plusmn", "+/-"},    {"sup2", "^2"},
    {"sup3", "^3"},
    {"minus", "-"},       {"plus", "+"},        {"times", "x"},
    {"divide", "/"},
    {"radic", "sqrt"},    {"infin", "inf"},     {"ne", "!="},
    {"le", "<="},         {"ge", ">="},
    {"asymp", "~"},       {"sim", "~"},         {"cong", "~"},
    {"sdot", "*"},
    {"in", "in "},        {"notin", "not "},    {"sum", "sum "},
    {"prod", "prod "},    {"int", "int "},
    {"part", "d"},        {"Delta", "D"},       {"pi", "pi"},
    {"alpha", "alpha"},
    {"rarr", "->"},       {"larr", "<-"},       {"uarr", "^"},
    {"darr", "v"},
    {"and", "and "},      {"or", "or "},
    {"prime", "'"},       {"Prime", "\""},      {"ang", "L"},
    {"perp", "_|_"},
    {"acute", "'"},
    {"micro", "u"},
    {"para", "P"},
    {"middot", "*"},
    {"cedil", ","},
    {"sup1", "^1"},
    {"ordm", "o"},
    {"raquo", ">>"},
    {"frac14", "1/4"},    {"frac12", "1/2"},    {"frac34", "3/4"},
    {"iquest", "?"},
    {"Agrave", "A"}, {"Aacute", "A"}, {"Acirc", "A"}, {"Atilde", "A"},
    {"Auml", "A"},   {"Aring", "A"},
    {"Egrave", "E"}, {"Eacute", "E"}, {"Ecirc", "E"}, {"Euml", "E"},
    {"Igrave", "I"}, {"Iacute", "I"}, {"Icirc", "I"}, {"Iuml", "I"},
    {"Ograve", "O"}, {"Oacute", "O"}, {"Ocirc", "O"}, {"Otilde", "O"},
    {"Ouml", "O"},
    {"Ugrave", "U"}, {"Uacute", "U"}, {"Ucirc", "U"}, {"Uuml", "U"},
    {"agrave", "a"}, {"aacute", "a"}, {"acirc", "a"}, {"atilde", "a"},
    {"auml", "a"},   {"aring", "a"},
    {"egrave", "e"}, {"eacute", "e"}, {"ecirc", "e"}, {"euml", "e"},
    {"igrave", "i"}, {"iacute", "i"}, {"icirc", "i"}, {"iuml", "i"},
    {"ograve", "o"}, {"oacute", "o"}, {"ocirc", "o"}, {"otilde", "o"},
    {"ouml", "o"},
    {"ugrave", "u"}, {"uacute", "u"}, {"ucirc", "u"}, {"uuml", "u"},
    {"mdash", " -- "},
    {"ndash", " - "},
    {"lsquo", "'"},
    {"rsquo", "'"},
    {"ldquo", "\""},
    {"rdquo", "\""},
    {"hellip", "..."},
    {"trade", "(TM)"},
    {"bull", "*"},
    {"euro", "EUR"},
    {"check", "[v]"},
    {"cross", "[x]"},
};

static const char* named_lookup(const char* name, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (strlen(NAMED[i].name) == len &&
            memcmp(NAMED[i].name, name, len) == 0) {
            return NAMED[i].value;
        }
    }
    return NULL;
}

// --------------------------------------------------- MATH_CP map ----

static const char* math_cp_lookup(unsigned num)
{
    switch (num) {
    case 176: return "deg";   case 177: return "+/-";
    case 178: return "^2";    case 179: return "^3";
    case 183: return "*";     case 215: return "x";
    case 247: return "/";     case 960: return "pi";
    case 916: return "D";
    case 8706: return "d";    case 8712: return "in ";
    case 8719: return "prod ";
    case 8721: return "sum "; case 8722: return "-";
    case 8730: return "sqrt"; case 8734: return "inf";
    case 8747: return "int ";
    case 8776: return "~";    case 8800: return "!=";
    case 8804: return "<=";   case 8805: return ">=";
    case 8592: return "<-";   case 8593: return "^";
    case 8594: return "->";   case 8595: return "v";
    default: return NULL;
    }
}

// Decimal branch specials (&#NNN;). Hex branch differs (no 201A/201E!).
static const char* dec_special(unsigned num)
{
    switch (num) {
    case 160:
    case 8239:
    case 8201:
    case 8200: return " ";
    case 8211: return " - ";
    case 8212: return " -- ";
    case 8216:
    case 8217:
    case 8218: return "'";
    case 8220:
    case 8221:
    case 8222: return "\"";
    case 8230: return "...";
    case 8226: return "*";
    default: return NULL;
    }
}

// Hex branch specials (&#xNN;): NO 0x201A / 0x201E cases (source asymmetry).
static const char* hex_special(unsigned num)
{
    switch (num) {
    case 0xA0:
    case 0x202F:
    case 0x2009: return " ";
    case 0x2013: return " - ";
    case 0x2014: return " -- ";
    case 0x2018:
    case 0x2019: return "'";
    case 0x201C:
    case 0x201D: return "\"";
    case 0x2026: return "...";
    case 0x2022: return "*";
    default: return NULL;
    }
}

// numeric entity body shared by both branches
static void append_num_entity(StrBuf* out, unsigned long long num,
                              const char* (*special)(unsigned))
{
    const char* m;
    if (num > 0xFFFFFFFFull) {
        sb_append_char(out, ' '); // huge values land here like Lua floats
        return;
    }
    m = special((unsigned)num);
    if (m != NULL) {
        sb_append_str(out, m);
        return;
    }
    m = math_cp_lookup((unsigned)num);
    if (m != NULL) {
        sb_append_str(out, m);
        return;
    }
    if (num >= 32 && num <= 126) {
        sb_append_char(out, (char)(unsigned char)num);
        return;
    }
    sb_append_char(out, ' ');
}

// ------------------------------------------- numeric entity passes ----

// Replaces "&#<digits>;" (hexMode=0) or "&#[xX]<hexdigits>;" (hexMode=1).
// Single left-to-right pass, replacements never rescanned (gsub parity).
static int replace_numeric(char** ptext, size_t* plen, int hexMode)
{
    StrBuf out;
    const char* text = *ptext;
    size_t len = *plen;
    size_t i = 0;
    int changed = 0;

    sb_init(&out);
    while (i < len) {
        char c = text[i];
        if (c != '&') {
            sb_append_char(&out, c);
            i++;
            continue;
        }
        {
            size_t p = i + 1;
            unsigned long long value = 0;
            int digits = 0;
            int overflow = 0;
            int ok = 0;

            // Both forms share the literal '&#': & #[xX] hex | & # dec.
            if (p < len && text[p] == '#') {
                p++;
                if (!hexMode) {
                    while (p < len && text[p] >= '0' && text[p] <= '9') {
                        unsigned d = (unsigned)(text[p] - '0');
                        digits++;
                        if (value > 0xFFFFFFFFull) {
                            overflow = 1;
                        } else {
                            value = value * 10 + d;
                        }
                        p++;
                    }
                    ok = digits > 0 && p < len && text[p] == ';';
                    if (ok) {
                        p++;
                    }
                } else if (p < len &&
                           (text[p] == 'x' || text[p] == 'X')) {
                    p++;
                    while (p < len) {
                        char h = text[p];
                        unsigned d;
                        if (h >= '0' && h <= '9') {
                            d = (unsigned)(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            d = (unsigned)(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            d = (unsigned)(h - 'A' + 10);
                        } else {
                            break;
                        }
                        digits++;
                        if (value > 0xFFFFFFFFull) {
                            overflow = 1;
                        } else {
                            value = value * 16 + d;
                        }
                        p++;
                    }
                    ok = digits > 0 && p < len && text[p] == ';';
                    if (ok) {
                        p++;
                    }
                }
                // NOTE: "&#x..;" simply fails the decimal branch
                // (digits==0), exactly like the Lua pattern &#(%d+);.
            }
            if (ok) {
                append_num_entity(&out, overflow ? 0xFFFFFFFFull + 1 : value,
                                  hexMode ? hex_special : dec_special);
                i = p;
                changed = 1;
                continue;
            }
        }
        sb_append_char(&out, '&');
        i++;
    }

    if (changed) {
        pluto_free(*ptext);
        *ptext = sb_detach(&out);
        *plen = strlen(*ptext); // replacements are ASCII-only
        return 1;
    }
    sb_free(&out);
    return 0;
}

// ------------------------------------------------ named pass ----

static int replace_named(char** ptext, size_t* plen)
{
    StrBuf out;
    const char* text = *ptext;
    size_t len = *plen;
    size_t i = 0;
    int changed = 0;

    sb_init(&out);
    while (i < len) {
        char c = text[i];
        if (c != '&') {
            sb_append_char(&out, c);
            i++;
            continue;
        }
        {
            size_t p = i + 1;
            size_t start = p;
            while (p < len && ((text[p] >= 'a' && text[p] <= 'z') ||
                               (text[p] >= 'A' && text[p] <= 'Z'))) {
                p++;
            }
            if (p > start && p < len && text[p] == ';') {
                const char* v = named_lookup(text + start, p - start);
                if (v != NULL) {
                    sb_append_str(&out, v);
                } else {
                    sb_append_char(&out, ' ');
                    sb_append(&out, text + start, p - start);
                    sb_append_char(&out, ' ');
                }
                i = p + 1;
                changed = 1;
                continue;
            }
        }
        sb_append_char(&out, '&');
        i++;
    }

    if (changed) {
        pluto_free(*ptext);
        *ptext = sb_detach(&out);
        *plen = strlen(*ptext);
        return 1;
    }
    sb_free(&out);
    return 0;
}

// ------------------------------------- step 4: fixed sequences ----

typedef struct SeqRepl {
    const char* seq;   // raw bytes
    size_t seqLen;
    const char* repl;
} SeqRepl;

#define S3(a, b, c) ((const char[]) {(char)a, (char)b, (char)c})
#define S2(a, b) ((const char[]) {(char)a, (char)b})

static const SeqRepl FIXED_SEQS[] = {
    {S3(0xEF, 0xBB, 0xBF), 3, ""},        // BOM
    {S2(0xC2, 0xA0), 2, " "},             // NBSP
    {S3(0xE2, 0x80, 0x93), 3, " - "},     // en dash
    {S3(0xE2, 0x80, 0x94), 3, " -- "},    // em dash
    {S3(0xE2, 0x80, 0x98), 3, "'"},       // left single quote
    {S3(0xE2, 0x80, 0x99), 3, "'"},       // right single quote
    {S3(0xE2, 0x80, 0x9C), 3, "\""},      // left double quote
    {S3(0xE2, 0x80, 0x9D), 3, "\""},      // right double quote
    {S3(0xE2, 0x80, 0xA6), 3, "..."},     // ellipsis
    {S3(0xE2, 0x80, 0xA2), 3, "*"},       // bullet
    {S2(0xC2, 0xB7), 2, "*"},             // middle dot
    {S3(0xE2, 0x88, 0x92), 3, "-"},       // minus sign
    {S2(0xC2, 0xB1), 2, "+/-"},           // plus-minus
    {S3(0xE2, 0x88, 0x9A), 3, "sqrt"},    // square root
    {S3(0xE2, 0x88, 0x9E), 3, "inf"},     // infinity
    {S3(0xE2, 0x89, 0xA4), 3, "<="},      // less-equal
    {S3(0xE2, 0x89, 0xA5), 3, ">="},      // greater-equal
    {S3(0xE2, 0x89, 0xA0), 3, "!="},      // not-equal
    {S3(0xE2, 0x89, 0x88), 3, "~"},       // almost-equal
    {S3(0xE2, 0x88, 0x91), 3, "sum "},    // n-ary summation
    {S3(0xE2, 0x88, 0x8F), 3, "prod "},   // n-ary product
    {S3(0xE2, 0x88, 0x88), 3, "in "},     // element-of
    {S3(0xE2, 0x88, 0xA3), 3, "|"},       // divides
    {S3(0xE2, 0x86, 0x92), 3, "->"},      // right arrow
    {S3(0xE2, 0x86, 0x90), 3, "<-"},      // left arrow
    {S3(0xE2, 0x86, 0x91), 3, "^"},       // up arrow
    {S3(0xE2, 0x86, 0x93), 3, "v"},       // down arrow
};

static void apply_fixed_sequences(StrBuf* out, const char* text, size_t len)
{
    size_t nSeq = sizeof(FIXED_SEQS) / sizeof(FIXED_SEQS[0]);
    StrBuf cur;
    size_t s;

    sb_init(&cur);
    sb_append(&cur, text, len);

    for (s = 0; s < nSeq; s++) {
        const SeqRepl* r = &FIXED_SEQS[s];
        StrBuf next;
        size_t i = 0;
        sb_init(&next);
        while (i < cur.len) {
            if (cur.len - i >= r->seqLen &&
                memcmp(cur.data + i, r->seq, r->seqLen) == 0) {
                sb_append_str(&next, r->repl);
                i += r->seqLen;
            } else {
                sb_append_char(&next, cur.data[i]);
                i++;
            }
        }
        pluto_free(cur.data);
        cur = next;
    }
    sb_append_buf(out, &cur); // may alias? no — must differ
    pluto_free(cur.data);
}

// --------------------------------------- step 5: transliteration ----

static const char* translit_lookup(unsigned cp)
{
    switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        return "A";
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
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        return "a";
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

    case 0x100: return "A"; case 0x101: return "a";
    case 0x102: return "A"; case 0x103: return "a";
    case 0x104: return "A"; case 0x105: return "a";
    case 0x10C: return "C"; case 0x10D: return "c";
    case 0x10E: return "D"; case 0x10F: return "d";
    case 0x110: return "D"; case 0x111: return "d";
    case 0x112: return "E"; case 0x113: return "e";
    case 0x11A: return "E"; case 0x11B: return "e";
    case 0x11E: return "G"; case 0x11F: return "g";
    case 0x120: return "G"; case 0x121: return "g";
    case 0x124: return "H"; case 0x125: return "h";
    case 0x126: return "H"; case 0x127: return "h";
    case 0x12A: return "I"; case 0x12B: return "i";
    case 0x130: return "I"; case 0x131: return "i";
    case 0x134: return "J"; case 0x135: return "j";
    case 0x136: return "K"; case 0x137: return "k";
    case 0x138: return "k";
    case 0x13B: return "L"; case 0x13C: return "l";
    case 0x13D: return "L"; case 0x13E: return "l";
    case 0x141: return "L"; case 0x142: return "l";
    case 0x143: return "N"; case 0x144: return "n";
    case 0x145: return "N"; case 0x146: return "n";
    case 0x147: return "N"; case 0x148: return "n";
    case 0x150: return "O"; case 0x151: return "o";
    case 0x152: return "OE"; case 0x153: return "oe";
    case 0x154: return "R"; case 0x155: return "r";
    case 0x158: return "R"; case 0x159: return "r";
    case 0x15A: return "S"; case 0x15B: return "s";
    case 0x15C: return "S"; case 0x15D: return "s";
    case 0x15E: return "S"; case 0x15F: return "s";
    case 0x160: return "S"; case 0x161: return "s";
    case 0x162: return "T"; case 0x163: return "t";
    case 0x164: return "T"; case 0x165: return "t";
    case 0x166: return "T"; case 0x167: return "t";
    case 0x16A: return "U"; case 0x16B: return "u";
    case 0x16C: return "U"; case 0x16D: return "u";
    case 0x16E: return "U"; case 0x16F: return "u";
    case 0x170: return "U"; case 0x171: return "u";
    case 0x172: return "U"; case 0x173: return "u";
    case 0x174: return "W"; case 0x175: return "w";
    case 0x176: return "Y"; case 0x177: return "y";
    case 0x178: return "Y";
    case 0x179: return "Z"; case 0x17A: return "z";
    case 0x17B: return "Z"; case 0x17C: return "z";
    case 0x17D: return "Z"; case 0x17E: return "z";
    default: return NULL;
    }
}

// Final sanitize pass. Runs ONLY when the original text had high bytes.
// Mirrors the Lua byte loop exactly, including its index conditions:
//   b 192..223 needs ONE more byte; b 224..239 needs TWO more bytes;
//   truncated leads fall into the catch-all " " and the leftover
//   continuation bytes become separate " " entries.
static void utf8_cleanup_pass(char** ptext, size_t* plen)
{
    StrBuf out;
    const char* text = *ptext;
    size_t len = *plen;
    size_t i = 0;

    sb_init(&out);
    while (i < len) {
        unsigned char b = (unsigned char)text[i];

        tasks_yield_check();

        if ((b >= 32 && b <= 126) || b == 10 || b == 13 || b == 9) {
            sb_append_char(&out, (char)b);
            i += 1;
        } else if (b >= 192 && b <= 223 && i + 1 < len) {
            unsigned char cb = (unsigned char)text[i + 1];
            unsigned cp = ((b % 0x20) * 64) + (cb % 0x40);
            const char* t = translit_lookup(cp);
            if (t != NULL) {
                sb_append_str(&out, t);
            } else {
                sb_append_char(&out, ' ');
            }
            i += 2;
        } else if (b >= 224 && b <= 239 && i + 2 < len) {
            sb_append_char(&out, ' ');
            i += 3;
        } else {
            sb_append_char(&out, ' ');
            i += 1;
        }
    }

    pluto_free(*ptext);
    *ptext = sb_detach(&out);
    *plen = strlen(*ptext);
}

// ------------------------------------------------------ decode ----

char* entities_decode(const char* text, size_t len, size_t* outLen)
{
    char* buf;
    size_t blen;
    int hasAmp = 0;
    int hasHigh = 0;
    size_t i;

    if (outLen != NULL) {
        *outLen = 0;
    }
    if (text == NULL || len == 0) {
        buf = (char*)pluto_malloc(1);
        if (buf != NULL) {
            buf[0] = '\0';
        }
        return buf;
    }

    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b == '&') {
            hasAmp = 1;
        } else if (b >= 0x80) {
            hasHigh = 1;
        }
    }

    // Fast path: no '&' and no high byte -> nothing can ever decode.
    if (!hasAmp && !hasHigh) {
        buf = (char*)pluto_malloc(len + 1);
        memcpy(buf, text, len);
        buf[len] = '\0';
        if (outLen != NULL) {
            *outLen = len;
        }
        return buf;
    }

    buf = (char*)pluto_malloc(len + 1);
    memcpy(buf, text, len);
    buf[len] = '\0';
    blen = len;

    if (hasAmp) {
        replace_numeric(&buf, &blen, 0);
        replace_numeric(&buf, &blen, 1);
        replace_named(&buf, &blen);
        if (!hasHigh) {
            if (outLen != NULL) {
                *outLen = blen;
            }
            return buf;
        }
    }

    // Steps 4+5: only reachable when high bytes are present.
    {
        StrBuf staged;
        sb_init(&staged);
        apply_fixed_sequences(&staged, buf, blen);
        pluto_free(buf);
        buf = sb_detach(&staged);
        blen = strlen(buf);

        utf8_cleanup_pass(&buf, &blen);
    }

    if (outLen != NULL) {
        *outLen = blen;
    }
    return buf;
}

// ------------------------------------------------------ encode ----

char* entities_encode(const char* text, size_t len, size_t* outLen)
{
    StrBuf out;
    size_t i;

    if (outLen != NULL) {
        *outLen = 0;
    }
    if (text == NULL || len == 0) {
        char* empty = (char*)pluto_malloc(1);
        if (empty != NULL) {
            empty[0] = '\0';
        }
        return empty;
    }

    sb_init(&out);
    for (i = 0; i < len; i++) {
        char c = text[i];
        if (c == '&') {
            sb_append_str(&out, "&amp;");
        } else if (c == '<') {
            sb_append_str(&out, "&lt;");
        } else if (c == '>') {
            sb_append_str(&out, "&gt;");
        } else if (c == '"') {
            sb_append_str(&out, "&quot;");
        } else {
            sb_append_char(&out, c);
        }
    }

    {
        char* s = sb_detach(&out);
        if (outLen != NULL) {
            *outLen = strlen(s);
        }
        return s;
    }
}
