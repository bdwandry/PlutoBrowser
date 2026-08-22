// json.c — minimal JSON parser, writer, and builders.

#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

#define JSON_MAX_DEPTH 64

// ---------------------------------------------------------------- shared --

void json_free(JsonValue* v)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    if (v->type == JSON_STRING) {
        pluto_free(v->str);
    } else if (v->type == JSON_ARRAY || v->type == JSON_OBJECT) {
        for (i = 0; i < v->count; i++) {
            json_free(v->items[i]);
            if (v->keys != NULL) {
                pluto_free(v->keys[i]);
            }
        }
        pluto_free(v->items);
        pluto_free(v->keys);
    }
    pluto_free(v);
}

const JsonValue* json_obj_get(const JsonValue* obj, const char* key)
{
    size_t i;
    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

const JsonValue* json_arr_get(const JsonValue* arr, size_t i)
{
    if (arr == NULL || arr->type != JSON_ARRAY || i >= arr->count) {
        return NULL;
    }
    return arr->items[i];
}

size_t json_arr_count(const JsonValue* v)
{
    if (v == NULL || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) {
        return 0;
    }
    return v->count;
}

int json_is_null(const JsonValue* v)
{
    return v == NULL || v->type == JSON_NULL;
}

double json_num(const JsonValue* v, double dflt)
{
    if (v == NULL || v->type != JSON_NUMBER) {
        return dflt;
    }
    return v->number;
}

int json_bool_val(const JsonValue* v, int dflt)
{
    if (v == NULL || v->type != JSON_BOOL) {
        return dflt;
    }
    return v->boolean;
}

const char* json_str(const JsonValue* v, size_t* lenOut)
{
    if (v == NULL || v->type != JSON_STRING) {
        if (lenOut != NULL) *lenOut = 0;
        return NULL;
    }
    if (lenOut != NULL) *lenOut = v->strLen;
    return v->str;
}

// ---------------------------------------------------------------- parser --

typedef struct {
    const char* p;    // cursor
    const char* end;
    const char* start; // input start (for error offsets)
    char err[128];
    int depth;
} JParser;

static void jerr(JParser* jp, const char* msg)
{
    if (jp->err[0] == '\0') {
        snprintf(jp->err, sizeof(jp->err), "%s at offset %u", msg,
                 (unsigned)(jp->p - jp->start));
    }
}

static void jskip_ws(JParser* jp)
{
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            jp->p++;
        } else {
            break;
        }
    }
}

static int j_hex4(JParser* jp, unsigned* out)
{
    int i;
    unsigned v = 0;
    for (i = 0; i < 4; i++) {
        char c;
        if (jp->p >= jp->end) {
            jerr(jp, "truncated \\u escape");
            return 0;
        }
        c = *jp->p++;
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else { jerr(jp, "bad hex digit in \\u escape"); return 0; }
    }
    *out = v;
    return 1;
}

static int j_utf8_encode(StrBuf* sb, unsigned cp)
{
    if (cp < 0x80) {
        return sb_append_char(sb, (char)cp);
    }
    if (cp < 0x800) {
        return sb_append_char(sb, (char)(0xC0 | (cp >> 6))) &&
               sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp < 0x10000) {
        return sb_append_char(sb, (char)(0xE0 | (cp >> 12))) &&
               sb_append_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
    return sb_append_char(sb, (char)(0xF0 | (cp >> 18))) &&
           sb_append_char(sb, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           sb_append_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
}

// Parses a JSON string body starting after the opening quote.
static char* j_parse_string_raw(JParser* jp, size_t* outLen)
{
    StrBuf sb;
    sb_init(&sb);
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p;
        if (c == '"') {
            jp->p++;
            *outLen = sb.len;
            return sb_detach(&sb);
        }
        if (c == '\\') {
            jp->p++;
            if (jp->p >= jp->end) {
                jerr(jp, "truncated escape");
                break;
            }
            {
                char e = *jp->p++;
                switch (e) {
                    case '"':  if (!sb_append_char(&sb, '"')) goto oom; break;
                    case '\\': if (!sb_append_char(&sb, '\\')) goto oom; break;
                    case '/':  if (!sb_append_char(&sb, '/')) goto oom; break;
                    case 'b':  if (!sb_append_char(&sb, '\b')) goto oom; break;
                    case 'f':  if (!sb_append_char(&sb, '\f')) goto oom; break;
                    case 'n':  if (!sb_append_char(&sb, '\n')) goto oom; break;
                    case 'r':  if (!sb_append_char(&sb, '\r')) goto oom; break;
                    case 't':  if (!sb_append_char(&sb, '\t')) goto oom; break;
                    case 'u': {
                        unsigned cp;
                        if (!j_hex4(jp, &cp)) break;
                        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
                            unsigned lo;
                            if (jp->p + 1 < jp->end && jp->p[0] == '\\' &&
                                jp->p[1] == 'u') {
                                jp->p += 2;
                                if (!j_hex4(jp, &lo)) break;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (lo - 0xDC00);
                                } else {
                                    // unpaired: emit replacement char
                                    cp = 0xFFFD;
                                    jp->p -= 2;
                                }
                            } else {
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            cp = 0xFFFD; // unpaired low surrogate
                        }
                        if (!j_utf8_encode(&sb, cp)) goto oom;
                        break;
                    }
                    default:
                        jerr(jp, "bad escape character");
                        goto fail;
                }
            }
            continue;
        }
        if (c < 0x20) {
            jerr(jp, "control character in string");
            break;
        }
        if (!sb_append_char(&sb, (char)c)) goto oom;
        jp->p++;
    }
    if (jp->err[0] == '\0') {
        jerr(jp, "unterminated string");
    }
fail:
    sb_free(&sb);
    return NULL;
oom:
    jerr(jp, "out of memory");
    sb_free(&sb);
    return NULL;
}

static JsonValue* j_parse_value(JParser* jp);

static JsonValue* j_parse_number(JParser* jp)
{
    char buf[64];
    size_t n = 0;
    const char* start = jp->p;
    char* endp;
    double d;
    JsonValue* v;

    // Sign, digits, fraction, and exponent chars are all collected verbatim
    // so strtod sees the full token.
    while (jp->p < jp->end &&
           ((*jp->p >= '0' && *jp->p <= '9') || *jp->p == '.' ||
            *jp->p == 'e' || *jp->p == 'E' || *jp->p == '+' || *jp->p == '-')) {
        if (n + 1 < sizeof(buf)) {
            buf[n++] = *jp->p;
        } else {
            jerr(jp, "number too long");
            return NULL;
        }
        jp->p++;
    }
    if (n == 0) {
        jerr(jp, "bad number");
        return NULL;
    }
    buf[n] = '\0';
    d = strtod(buf, &endp);
    if (endp == buf) {
        jp->p = start;
        jerr(jp, "bad number");
        return NULL;
    }
    v = json_new_number(d);
    if (v == NULL) {
        jerr(jp, "out of memory");
    }
    return v;
}

static int j_lit(JParser* jp, const char* word)
{
    size_t n = strlen(word);
    if ((size_t)(jp->end - jp->p) >= n && memcmp(jp->p, word, n) == 0) {
        jp->p += n;
        return 1;
    }
    return 0;
}

static int j_grow(JsonValue* v)
{
    if (v->count < v->cap) {
        return 1;
    }
    {
        size_t nc = (v->cap == 0) ? 8 : v->cap * 2;
        JsonValue** ni = (JsonValue**)pluto_realloc(v->items, nc * sizeof(JsonValue*));
        char** nk = NULL;
        if (ni == NULL) {
            return 0;
        }
        v->items = ni;
        if (v->type == JSON_OBJECT) {
            nk = (char**)pluto_realloc(v->keys, nc * sizeof(char*));
            if (nk == NULL) {
                return 0;
            }
            v->keys = nk;
        }
        v->cap = nc;
    }
    return 1;
}

static JsonValue* j_parse_value(JParser* jp)
{
    JsonValue* v;

    jskip_ws(jp);
    if (jp->p >= jp->end) {
        jerr(jp, "unexpected end of input");
        return NULL;
    }
    if (++jp->depth > JSON_MAX_DEPTH) {
        jerr(jp, "nesting too deep");
        jp->depth--;
        return NULL;
    }

    switch (*jp->p) {
        case '{': {
            jp->p++;
            v = json_new_object();
            if (v == NULL) { jerr(jp, "out of memory"); break; }
            jskip_ws(jp);
            if (jp->p < jp->end && *jp->p == '}') {
                jp->p++;
                break;
            }
            while (jp->p < jp->end) {
                size_t klen;
                char* key;
                JsonValue* val;
                jskip_ws(jp);
                if (jp->p >= jp->end || *jp->p != '"') {
                    jerr(jp, "expected object key");
                    json_free(v);
                    v = NULL;
                    break;
                }
                jp->p++;
                key = j_parse_string_raw(jp, &klen);
                if (key == NULL) {
                    json_free(v);
                    v = NULL;
                    break;
                }
                jskip_ws(jp);
                if (jp->p >= jp->end || *jp->p != ':') {
                    jerr(jp, "expected ':' after object key");
                    pluto_free(key);
                    json_free(v);
                    v = NULL;
                    break;
                }
                jp->p++;
                val = j_parse_value(jp);
                if (val == NULL) {
                    pluto_free(key);
                    json_free(v);
                    v = NULL;
                    break;
                }
                if (!j_grow(v)) {
                    jerr(jp, "out of memory");
                    pluto_free(key);
                    json_free(val);
                    json_free(v);
                    v = NULL;
                    break;
                }
                v->keys[v->count] = key;
                v->items[v->count] = val;
                v->count++;
                jskip_ws(jp);
                if (jp->p < jp->end && *jp->p == ',') {
                    jp->p++;
                    continue;
                }
                if (jp->p < jp->end && *jp->p == '}') {
                    jp->p++;
                    break;
                }
                jerr(jp, "expected ',' or '}' in object");
                json_free(v);
                v = NULL;
                break;
            }
            break;
        }
        case '[': {
            jp->p++;
            v = json_new_array();
            if (v == NULL) { jerr(jp, "out of memory"); break; }
            jskip_ws(jp);
            if (jp->p < jp->end && *jp->p == ']') {
                jp->p++;
                break;
            }
            while (jp->p < jp->end) {
                JsonValue* item = j_parse_value(jp);
                if (item == NULL) {
                    json_free(v);
                    v = NULL;
                    break;
                }
                if (!j_grow(v)) {
                    jerr(jp, "out of memory");
                    json_free(item);
                    json_free(v);
                    v = NULL;
                    break;
                }
                v->items[v->count++] = item;
                jskip_ws(jp);
                if (jp->p < jp->end && *jp->p == ',') {
                    jp->p++;
                    continue;
                }
                if (jp->p < jp->end && *jp->p == ']') {
                    jp->p++;
                    break;
                }
                jerr(jp, "expected ',' or ']' in array");
                json_free(v);
                v = NULL;
                break;
            }
            break;
        }
        case '"': {
            size_t slen;
            char* s;
            jp->p++;
            s = j_parse_string_raw(jp, &slen);
            if (s == NULL) {
                v = NULL;
                break;
            }
            v = json_new_null();
            if (v == NULL) {
                pluto_free(s);
                jerr(jp, "out of memory");
                break;
            }
            v->type = JSON_STRING;
            v->str = s;
            v->strLen = slen;
            break;
        }
        case 't':
            if (j_lit(jp, "true")) {
                v = json_new_bool(1);
                if (v == NULL) jerr(jp, "out of memory");
            } else {
                jerr(jp, "bad literal");
                v = NULL;
            }
            break;
        case 'f':
            if (j_lit(jp, "false")) {
                v = json_new_bool(0);
                if (v == NULL) jerr(jp, "out of memory");
            } else {
                jerr(jp, "bad literal");
                v = NULL;
            }
            break;
        case 'n':
            if (j_lit(jp, "null")) {
                v = json_new_null();
                if (v == NULL) jerr(jp, "out of memory");
            } else {
                jerr(jp, "bad literal");
                v = NULL;
            }
            break;
        default:
            if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) {
                v = j_parse_number(jp);
            } else {
                jerr(jp, "unexpected character");
                v = NULL;
            }
            break;
    }

    jp->depth--;
    return v;
}

JsonValue* json_parse(const char* text, size_t len, char err[128])
{
    JParser jp;
    JsonValue* v;

    if (text == NULL) {
        if (err) snprintf(err, 128, "null input");
        return NULL;
    }
    jp.p = text;
    jp.end = text + len;
    jp.start = text;
    jp.err[0] = '\0';
    jp.depth = 0;

    v = j_parse_value(&jp);
    if (v != NULL) {
        jskip_ws(&jp);
        if (jp.p != jp.end) {
            jerr(&jp, "trailing data after value");
            json_free(v);
            v = NULL;
        }
    }
    if (err != NULL) {
        snprintf(err, 128, "%s", jp.err);
    }
    return v;
}

// ---------------------------------------------------------------- writer --

static int j_write_escaped(StrBuf* out, const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  if (!sb_append_str(out, "\\\"")) return 0; break;
            case '\\': if (!sb_append_str(out, "\\\\")) return 0; break;
            case '\b': if (!sb_append_str(out, "\\b")) return 0; break;
            case '\f': if (!sb_append_str(out, "\\f")) return 0; break;
            case '\n': if (!sb_append_str(out, "\\n")) return 0; break;
            case '\r': if (!sb_append_str(out, "\\r")) return 0; break;
            case '\t': if (!sb_append_str(out, "\\t")) return 0; break;
            default:
                if (c < 0x20) {
                    if (!sb_printf(out, "\\u%04x", (unsigned)c)) return 0;
                } else if (!sb_append_char(out, (char)c)) {
                    return 0;
                }
                break;
        }
    }
    return 1;
}

static int j_write_num(StrBuf* out, double d)
{
    if (d == (double)(long long)d && d > -9.0e15 && d < 9.0e15) {
        return sb_printf(out, "%lld", (long long)d);
    }
    return sb_printf(out, "%.14g", d);
}

static int j_write_inner(const JsonValue* v, StrBuf* out)
{
    size_t i;
    switch (v->type) {
        case JSON_NULL:   return sb_append_str(out, "null");
        case JSON_BOOL:   return sb_append_str(out, v->boolean ? "true" : "false");
        case JSON_NUMBER: return j_write_num(out, v->number);
        case JSON_STRING:
            if (!sb_append_char(out, '"')) return 0;
            if (!j_write_escaped(out, v->str, v->strLen)) return 0;
            return sb_append_char(out, '"');
        case JSON_ARRAY:
            if (!sb_append_char(out, '[')) return 0;
            for (i = 0; i < v->count; i++) {
                if (i > 0 && !sb_append_char(out, ',')) return 0;
                if (!j_write_inner(v->items[i], out)) return 0;
            }
            return sb_append_char(out, ']');
        case JSON_OBJECT:
            if (!sb_append_char(out, '{')) return 0;
            for (i = 0; i < v->count; i++) {
                if (i > 0 && !sb_append_char(out, ',')) return 0;
                if (!sb_append_char(out, '"')) return 0;
                if (!j_write_escaped(out, v->keys[i], strlen(v->keys[i]))) return 0;
                if (!sb_append_str(out, "\":")) return 0;
                if (!j_write_inner(v->items[i], out)) return 0;
            }
            return sb_append_char(out, '}');
    }
    return 0;
}

int json_write(const JsonValue* v, StrBuf* out)
{
    if (v == NULL) {
        return sb_append_str(out, "null");
    }
    return j_write_inner(v, out);
}

// -------------------------------------------------------------- builders --

static JsonValue* j_alloc(JsonType t)
{
    JsonValue* v = (JsonValue*)pluto_malloc(sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

JsonValue* json_new_null(void)   { return j_alloc(JSON_NULL); }
JsonValue* json_new_bool(int b)
{
    JsonValue* v = j_alloc(JSON_BOOL);
    if (v) v->boolean = b ? 1 : 0;
    return v;
}
JsonValue* json_new_number(double n)
{
    JsonValue* v = j_alloc(JSON_NUMBER);
    if (v) v->number = n;
    return v;
}
JsonValue* json_new_string_len(const char* s, size_t n)
{
    JsonValue* v = j_alloc(JSON_STRING);
    if (v == NULL) return NULL;
    v->str = (char*)pluto_malloc(n + 1);
    if (v->str == NULL) {
        pluto_free(v);
        return NULL;
    }
    memcpy(v->str, s, n);
    v->str[n] = '\0';
    v->strLen = n;
    return v;
}
JsonValue* json_new_string(const char* s)
{
    return json_new_string_len(s, strlen(s));
}
JsonValue* json_new_array(void)  { return j_alloc(JSON_ARRAY); }
JsonValue* json_new_object(void) { return j_alloc(JSON_OBJECT); }

int json_obj_set(JsonValue* obj, const char* key, JsonValue* val)
{
    size_t i;
    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) {
        json_free(val);
        return 0;
    }
    for (i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) { // replace
            json_free(obj->items[i]);
            obj->items[i] = val;
            return 1;
        }
    }
    if (!j_grow(obj)) {
        json_free(val);
        return 0;
    }
    obj->keys[obj->count] = pluto_strdup(key);
    if (obj->keys[obj->count] == NULL) {
        json_free(val);
        return 0;
    }
    obj->items[obj->count] = val;
    obj->count++;
    return 1;
}

int json_arr_append(JsonValue* arr, JsonValue* val)
{
    if (arr == NULL || arr->type != JSON_ARRAY) {
        json_free(val);
        return 0;
    }
    if (!j_grow(arr)) {
        json_free(val);
        return 0;
    }
    arr->items[arr->count++] = val;
    return 1;
}
