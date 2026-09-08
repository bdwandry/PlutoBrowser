/*
 * PlutoBrowser — json.h
 * Minimal JSON decoder replacing the Playdate Lua SDK's built-in json.decode,
 * which has no C-API equivalent. Port of Source/render/cloud_layout.lua's
 * dependency (the reference consumes json.decode output only).
 *
 * Scope decision (documented in MASTER_TODO): the ONLY consumer in the
 * reference is CloudLayout.parse, which reads doc.title (string),
 * doc.totalHeight (number) and doc.elements (array of objects whose string
 * / number fields are copied verbatim). The decoder therefore implements the
 * full JSON grammar (objects, arrays, strings with escapes incl. \uXXXX,
 * numbers, true/false/null) but exposes a Lua-table-like tree model:
 *   JsonValue { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING,
 *               JSON_ARRAY, JSON_OBJECT }
 * Errors are reported by returning NULL — CloudLayout.parse's pcall path.
 *
 * Memory: the tree is one contiguous arena owned by the returned root; free
 * with json_free(). No allocation hooks — cloud layouts are tiny (a few KB).
 */
#ifndef PLUTO_JSON_H
#define PLUTO_JSON_H

#include <stddef.h>

typedef enum
{
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue
{
    JsonType type;
    /* bool / number / string */
    int boolean;
    double number;
    char *string; /* NULL for non-strings */
    /* array / object */
    JsonValue **items;   /* array elements or object values */
    char **keys;         /* object keys (NULL entries for arrays) */
    size_t count;
    size_t cap;
};

/* Parse `text` (NUL-terminated). Returns the root value or NULL on any
 * syntax error (trailing garbage after the root value is an error, matching
 * the Playdate decoder's strictness). */
JsonValue *json_decode(const char *text);

/* Look up `key` in an object. Returns NULL when v is not an object or the
 * key is absent. */
JsonValue *json_get(const JsonValue *v, const char *key);

/* Convenience accessors: return 1 on success. A JSON null or a type mismatch
 * leaves the out-param untouched and returns 0 (Lua's `x or default` reads
 * absent/null identically for CloudLayout's purposes). */
int json_as_string(const JsonValue *v, const char **out);
int json_as_number(const JsonValue *v, double *out);

void json_free(JsonValue *v);

#endif /* PLUTO_JSON_H */
