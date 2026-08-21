// json.h — minimal JSON parser, writer, and builders.
//
// Used by CloudLayout (P26) for server layout payloads and by Storage (P04)
// for datastore serialization parity with CometBrowser's Lua tables.
// Objects preserve insertion order; duplicate keys keep both entries and
// lookups return the first match (Lua table semantics overwrite instead,
// but no producer in this project emits duplicates).

#ifndef PLUTO_JSON_H
#define PLUTO_JSON_H

#include <stddef.h>

#include "strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonType type;
    int boolean;         // JSON_BOOL
    double number;       // JSON_NUMBER
    char* str;           // JSON_STRING (owned, decoded UTF-8)
    size_t strLen;
    JsonValue** items;   // JSON_ARRAY / JSON_OBJECT values (owned)
    char** keys;         // JSON_OBJECT keys parallel to items (owned)
    size_t count;
    size_t cap;
};

// Parse text[0..len). Returns NULL on failure and fills err (if non-NULL).
JsonValue* json_parse(const char* text, size_t len, char err[128]);
void       json_free(JsonValue* v);

// Accessors (NULL/type-safe).
const JsonValue* json_obj_get(const JsonValue* obj, const char* key);
const JsonValue* json_arr_get(const JsonValue* arr, size_t i);
size_t           json_arr_count(const JsonValue* v);
int              json_is_null(const JsonValue* v);
double           json_num(const JsonValue* v, double dflt);
int              json_bool_val(const JsonValue* v, int dflt);
// Strings only; returns NULL otherwise. *lenOut optional.
const char*      json_str(const JsonValue* v, size_t* lenOut);

// Serialize compactly. Returns 0 on OOM.
int   json_write(const JsonValue* v, StrBuf* out);

// Builders. Constructors return NULL on OOM. obj_set/arr_append take
// ownership of val (freeing it on failure).
JsonValue* json_new_null(void);
JsonValue* json_new_bool(int b);
JsonValue* json_new_number(double n);
JsonValue* json_new_string_len(const char* s, size_t n);
JsonValue* json_new_string(const char* s);
JsonValue* json_new_array(void);
JsonValue* json_new_object(void);
int json_obj_set(JsonValue* obj, const char* key, JsonValue* val);
int json_arr_append(JsonValue* arr, JsonValue* val);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_JSON_H
