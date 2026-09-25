/* Quick ASan exercise for the iterative JSON parser + iterative json_free.
 * Builds the same deep-nesting stress that made the recursive versions
 * risky, plus strictness checks (trailing commas must FAIL) and free of
 * deep trees (the old recursive json_free overflowed on exactly this). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "util/json.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                          \
    do                                             \
    {                                              \
        if (cond)                                  \
        {                                          \
            printf("PASS: %s\n", name);            \
            g_pass++;                              \
        }                                          \
        else                                       \
        {                                          \
            printf("FAIL: %s\n", name);            \
            g_fail++;                              \
        }                                          \
    } while (0)

static void test_scalars(void)
{
    JsonValue *v;
    v = json_decode("42");
    CHECK(v && v->type == JSON_NUMBER && v->number == 42, "number");
    json_free(v);
    v = json_decode("\"hi\\u0041\"");
    CHECK(v && v->type == JSON_STRING && strcmp(v->string, "hiA") == 0,
          "string with \\uXXXX");
    json_free(v);
    v = json_decode("true");
    CHECK(v && v->type == JSON_BOOL && v->boolean == 1, "true");
    json_free(v);
    v = json_decode("null");
    CHECK(v && v->type == JSON_NULL, "null");
    json_free(v);
    v = json_decode("");
    CHECK(v == NULL, "empty is error");
    v = json_decode("  { }  ");
    CHECK(v && v->type == JSON_OBJECT && v->count == 0, "empty object");
    json_free(v);
}

static void test_objects_arrays(void)
{
    JsonValue *v = json_decode(
        "{\"a\":[1,2,{\"b\":\"c\",\"d\":false}],\"e\":null,\"f\":[[{\"g\":-1.5e2}]]}");
    CHECK(v != NULL, "nested doc parses");
    if (!v)
    {
        return;
    }
    JsonValue *a = json_get(v, "a");
    CHECK(a && a->type == JSON_ARRAY && a->count == 3, "a has 3 items");
    JsonValue *inner = a ? a->items[2] : NULL;
    CHECK(inner && inner->type == JSON_OBJECT &&
              strcmp(json_get(inner, "b")->string, "c") == 0,
          "nested object member");
    JsonValue *deep = json_get(v, "f");
    CHECK(deep && deep->items[0]->items[0] &&
              json_get(deep->items[0]->items[0], "g")->number == -150.0,
          "deep array-of-array value");
    json_free(v);
}

static void test_strictness(void)
{
    CHECK(json_decode("[1,]") == NULL, "trailing comma in array rejected");
    CHECK(json_decode("{\"a\":1,}") == NULL, "trailing comma in object rejected");
    CHECK(json_decode("{\"a\" 1}") == NULL, "missing colon rejected");
    CHECK(json_decode("[1 2]") == NULL, "missing comma rejected");
    CHECK(json_decode("{\"a\":1}}") == NULL, "trailing garbage rejected");
    CHECK(json_decode("[unquoted]") == NULL, "bareword rejected");
    CHECK(json_decode("\"unterminated") == NULL, "unterminated string rejected");
}

static void test_deep_nesting(void)
{
    /* 60 nested arrays: inside the depth limit but deep enough that the
     * old recursive free/parse chewed serious C stack per level. */
    int depth = 60;
    char *buf = (char *)malloc((size_t)depth * 2 + 4);
    char *p = buf;
    for (int i = 0; i < depth; i++)
    {
        *p++ = '[';
    }
    *p++ = '9';
    for (int i = 0; i < depth; i++)
    {
        *p++ = ']';
    }
    *p = '\0';
    JsonValue *v = json_decode(buf);
    CHECK(v != NULL, "60-deep array parses");
    json_free(v); /* iterative free must survive where recursion overflowed */
    CHECK(1, "60-deep array freed");
    free(buf);

    /* over the limit must be rejected, not crashed */
    buf = (char *)malloc(128);
    p = buf;
    for (int i = 0; i < 100; i++)
    {
        *p++ = '[';
    }
    *p = '\0';
    CHECK(json_decode(buf) == NULL, "100-deep array rejected (depth cap)");
    free(buf);
}

int main(void)
{
    test_scalars();
    test_objects_arrays();
    test_strictness();
    test_deep_nesting();
    printf("\n%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
