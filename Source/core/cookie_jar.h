/*
 * PlutoBrowser — cookie_jar.h
 * RFC 6265 session cookie management (port of Source/core/cookie_jar.lua).
 *
 * Semantics preserved exactly from the Lua reference:
 *   - parseSetCookie: name/value validation, domain rejection, path default,
 *     secure/httpOnly/samesite, max-age, expires (3 date formats), delete
 *     semantics for Max-Age<=0 / past Expires.
 *   - store: dedupe by (hostOnly,domain,path,name); identical value+expires
 *     is a no-op; deletes remove; 300-cookie cap; save after every change.
 *   - getHeader: domain/path/secure match + lazy prune of expired entries.
 *   - prune/clear/count.
 *
 * Storage coupling: the Lua module persisted via Storage.cookies + save().
 * In C the jar owns its list and calls an injectable save hook (wired to the
 * storage module in Phase 9) so this phase stays independent.
 */
#ifndef PLUTO_COOKIE_JAR_H
#define PLUTO_COOKIE_JAR_H

#include <stddef.h>

#define COOKIE_JAR_MAX 300

typedef struct Cookie
{
    char *name;        /* heap; never NULL for a stored cookie */
    char *value;       /* heap; may be "" */
    char *domain;      /* heap; dot-stripped */
    int hostOnly;
    char *path;        /* heap; starts with '/' */
    int secure;
    int httpOnly;
    char samesite[8];  /* "" | "lax" | "strict" | "none" */
    long long expires; /* epoch seconds; -1 = session cookie */
    int deleteFlag;    /* set by parse for Max-Age<=0 / past Expires */
} Cookie;

/* Inject persistence: storage module assigns this in Phase 9 (may be NULL). */
void cookie_jar_set_save_hook(void (*fn)(void));

/* Parse a raw Set-Cookie value received from `host`. Returns a heap cookie
 * (caller frees with cookie_jar_free_cookie) or NULL if it must be ignored.
 * Sets cookie->deleteFlag for Max-Age<=0 / past Expires. */
Cookie *cookie_jar_parse_set_cookie(const char *host, const char *raw);

/* Free a cookie returned by parse. */
void cookie_jar_free_cookie(Cookie *c);

/* Store (or delete) a cookie received from `host`. */
void cookie_jar_store(const char *host, const char *raw);

/* Process all Set-Cookie values from one response. */
void cookie_jar_process_set_cookies(const char *host, char **list, int count);

/* Build the Cookie request header value for host/path/ssl, or "" when none
 * apply. Expired cookies are pruned lazily (save hook fires if pruned).
 * Writes into buf (cap bytes); always NUL-terminated. Returns buf. */
char *cookie_jar_get_header(const char *host, const char *path, int isSsl,
                            char *buf, size_t cap);

/* Drop expired/malformed cookies (called once at startup in the Lua ref). */
void cookie_jar_prune(void);

/* Remove all cookies. */
void cookie_jar_clear(void);

/* Number of stored cookies. */
int cookie_jar_count(void);

/* Test support: direct access to the stored list (index < count). */
const Cookie *cookie_jar_get(int index);

#endif /* PLUTO_COOKIE_JAR_H */
