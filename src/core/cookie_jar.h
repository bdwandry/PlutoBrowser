// cookie_jar.h — RFC 6265 session cookies (C port of core/cookie_jar.lua).
//
// All decision quirks mirror the Lua original exactly, including:
//  - Max-Age / Expires processed in header order; Expires skipped once any
//    expiry is set; past expiry sets delete instead of storing.
//  - Domain attr accepted only when it equals/suffix-matches the request
//    host AND contains a dot (or is "localhost"); otherwise the whole
//    Set-Cookie is rejected.
//  - store() replaces same-key entries (moving them to the END of the list)
//    and silently no-ops when value AND expiry are unchanged.
// The clock is injectable for deterministic tests (cj_set_now_fn).

#ifndef PLUTO_COOKIE_JAR_H
#define PLUTO_COOKIE_JAR_H

#include <stddef.h>

#include "../util/strbuf.h"
#include "storage_data.h"

struct PlaydateAPI;

#ifdef __cplusplus
extern "C" {
#endif

#define CJ_MAX_COOKIES 300

typedef double (*CJNowFn)(void); // epoch seconds

void cj_init(struct PlaydateAPI* pd);

// NULL restores the playdate clock (Lua nowSeconds parity).
void cj_set_now_fn(CJNowFn fn);
double cj_now(void);

// Howard Hinnant days-from-civil -> epoch seconds, with the Lua clamps
// (year floor>=0, month clamped to [1,12], day floor>=1; h/m/s used raw,
// treated as 0 when absent callers pass 0).
double cj_make_timestamp(double year, double month, double day, double hour,
                         double minute, double second);

// RFC 6265 date parsing (IMF-fixdate / RFC850 / asctime). Returns 1 + *out.
int  cj_parse_date(const char* str, double* out);

// Parse one Set-Cookie value. Returns 1 when acceptable (check out->del),
// 0 when the header must be ignored entirely.
int  cj_parse_set_cookie(const char* host, const char* raw, PlutoCookie* out);

// Store/delete from a raw Set-Cookie value received from `host`.
void cj_store(const char* host, const char* raw);
void cj_process_set_cookies(const char* host, const char* const* list,
                            size_t n);

// Cookie: header value for a request ("name=value; ..." or ""). Lazily
// prunes expired/malformed cookies (saving when anything was dropped).
void cj_get_header(const char* host, const char* path, int isSsl,
                   StrBuf* out);

void   cj_prune(void);
void   cj_clear(void);
size_t cj_count(void);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_COOKIE_JAR_H
