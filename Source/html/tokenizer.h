/*
 * PlutoBrowser — tokenizer.h
 * Single-pass HTML tokenizer (port of Source/html/tokenizer.lua).
 *
 * Token model preserved from the Lua reference:
 *   - text tokens: decoded content (Entities.decode applied).
 *   - tag tokens:  lowercased name, isClosing, isSelfClosing, attributes.
 *     Attribute values are entity-decoded; boolean attributes are stored as
 *     the internal BOOL marker; keys are lowercased; FIRST duplicate wins.
 *   - <title> yields no token; its collapsed text becomes pageTitleOut.
 *   - comments, <script>, <style> bodies are skipped without tokens.
 *   - unterminated tags / stray '<' drop the remainder (no synthetic tokens).
 *   - input larger than 256KB is cut at a tag boundary before tokenizing.
 *
 * C storage: tokens are appended to caller-owned growable arrays. Strings are
 * NUL-terminated copies allocated with the SDK allocator (pluto_free each).
 * For 256KB pages prefer tokenizer_run(), which owns an internal string arena
 * and frees every copied string when done (device-heap friendly); the raw
 * tokenizer_next_* API remains for callers that need tokens to outlive the
 * call (document builder, Phase 17).
 */
#ifndef PLUTO_TOKENIZER_H
#define PLUTO_TOKENIZER_H

#include <stddef.h>

/* Sentinel attribute value meaning "boolean attribute present" (Lua true).
 * Any other non-NULL value is a decoded string. */
extern const char PLUTO_TOK_ATTR_TRUE[1];

typedef enum
{
    TOK_TEXT = 0,
    TOK_TAG
} TokenType;

typedef struct
{
    char *key;   /* lowercase; arena-owned */
    char *value; /* decoded value or PLUTO_TOK_ATTR_TRUE; arena-owned */
} TokenAttr;

typedef struct
{
    TokenType type;
    /* text tokens */
    char *content; /* decoded text; arena-owned */
    /* tag tokens */
    char *name;       /* lowercased tag name; arena-owned */
    int isClosing;
    int isSelfClosing;
    TokenAttr *attrs; /* heap array (free with pluto_free); strings arena-owned */
    int attrCount;
} Token;

typedef struct
{
    Token *items;
    int count;
    int cap;
} TokenList;

/* Result of a tokenize pass. pageTitle is "Web Page" unless <title> found.
 * All token strings live in an internal arena owned by the result; release
 * everything with tokenizer_free_result. */
typedef struct
{
    TokenList tokens;
    char pageTitle[256];
    void *_arena; /* internal string arena */
} TokenizeResult;

/* High-level entry: tokenize `html` fully. Returns 0 ok, -1 alloc failure. */
int tokenizer_tokenize(const char *html, TokenizeResult *out);

/* Release all storage owned by a TokenizeResult (tokens, attrs, arena). */
void tokenizer_free_result(TokenizeResult *res);

#endif /* PLUTO_TOKENIZER_H */
