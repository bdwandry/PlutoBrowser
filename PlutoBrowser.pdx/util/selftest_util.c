// selftest_util.c — P02 util-layer self-tests (see header).
//
// Pattern tests use the exact Lua patterns found in CometBrowser sources
// (url.lua, tokenizer.lua, document.lua, svg.lua, cookie_jar.lua,
// http_client.lua, encoding.lua) so parity regressions surface here.

#include "selftest_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../core/logger.h"
#include "dynarray.h"
#include "json.h"
#include "luapattern.h"
#include "mem.h"
#include "strbuf.h"
#include "strmap.h"

static int st_pass = 0;
static int st_fail = 0;

// Literal-with-length: compile-time exact sizes for patterns/subjects
// (eliminates hand-counted length bugs).
#define PLIT(lit) (lit), (sizeof(lit) - 1)

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P02] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P02] FAIL %s", name);
    }
}

// Copy capture i into a NUL-terminated scratch buffer for comparisons.
static int cap_str(const char* subj, const LPCap caps[], int ncaps, int i,
                   char* out, size_t outSz)
{
    size_t cs, cl;
    if (!lp_cap_span(subj, caps, ncaps, i, &cs, &cl)) {
        return 0;
    }
    if (cl + 1 > outSz) {
        cl = outSz - 1;
    }
    memcpy(out, subj + cs, cl);
    out[cl] = '\0';
    return 1;
}

// ---------------------------------------------------------------- mem ----

static void test_mem(void)
{
    char* p = (char*)pluto_malloc(64);
    st_check(p != NULL, "mem.malloc");
    memset(p, 'x', 64);

    {
        char* q = (char*)pluto_realloc(p, 256);
        st_check(q != NULL && q[63] == 'x', "mem.realloc_preserves");
        pluto_free(q);
    }

    {
        char* d = pluto_strdup("hello");
        st_check(d != NULL && strcmp(d, "hello") == 0, "mem.strdup");
        pluto_free(d);
    }

    {
        const char* src = "abcdef";
        char* n = pluto_strndup(src + 2, 3); // "cde"
        st_check(n != NULL && strlen(n) == 3 && n[2] == 'e', "mem.strndup");
        pluto_free(n);
    }

    pluto_free(NULL); // must be a no-op
    st_check(1, "mem.free_null");
}

// ------------------------------------------------------------- strbuf ----

static void test_strbuf(void)
{
    StrBuf sb;
    sb_init(&sb);
    st_check(sb.len == 0 && sb.data == NULL, "strbuf.init");

    st_check(sb_append_str(&sb, "abc") && sb.len == 3, "strbuf.append_str");
    st_check(sb_append_char(&sb, '!') && sb.len == 4 &&
             memcmp(sb.data, "abc!", 4) == 0, "strbuf.append_char");

    // embedded NUL support
    st_check(sb_append(&sb, "a\0b", 3) && sb.len == 7 &&
             sb.data[5] == '\0' && sb.data[6] == 'b', "strbuf.embedded_nul");

    sb_clear(&sb);
    st_check(sb.len == 0 && sb.cap >= 7, "strbuf.clear_keeps_cap");

    st_check(sb_printf(&sb, "%d-%s-%02X", 42, "ok", 255) &&
             sb.len == 8 && memcmp(sb.data, "42-ok-FF", 8) == 0,
             "strbuf.printf");

    {
        StrBuf big;
        int i;
        sb_init(&big);
        for (i = 0; i < 500; i++) {
            sb_append_char(&big, 'a');
        }
        st_check(big.len == 500 && big.data[499] == 'a', "strbuf.grow_500");
        sb_free(&big);
    }

    {
        char* detached = sb_detach(&sb);
        st_check(detached != NULL && strcmp(detached, "42-ok-FF") == 0,
                 "strbuf.detach");
        pluto_free(detached);
        st_check(sb.data == NULL && sb.len == 0, "strbuf.detach_resets");
    }

    sb_free(&sb);
    st_check(1, "strbuf.free_idempotent");
}

// ------------------------------------------------------------ dynarray ----

typedef struct {
    int x;
    char tag;
} PtPair;

static void test_dynarray(void)
{
    DynArray arr;
    int i;

    da_init(&arr, sizeof(PtPair));
    st_check(arr.count == 0 && arr.items == NULL, "dynarray.init");

    for (i = 0; i < 100; i++) { // forces several growths
        PtPair p;
        PtPair* slot;
        p.x = i * 3;
        p.tag = (char)('A' + (i % 26));
        slot = (PtPair*)da_push(&arr, &p);
        if (slot == NULL) {
            break;
        }
    }
    st_check(arr.count == 100, "dynarray.push_100_grow");

    {
        PtPair* e = (PtPair*)da_get(&arr, 37);
        st_check(e != NULL && e->x == 111 && e->tag == 'L', "dynarray.get_mid");
    }
    {
        PtPair* last = (PtPair*)da_last(&arr);
        st_check(last != NULL && last->x == 297, "dynarray.last");
    }

    da_pop(&arr);
    st_check(arr.count == 99 && da_last(&arr) != NULL &&
             ((PtPair*)da_last(&arr))->x == 294, "dynarray.pop");

    st_check(da_get(&arr, 9999) == NULL, "dynarray.get_oob_null");

    da_free(&arr);
    st_check(arr.items == NULL && arr.count == 0, "dynarray.free");
}

// -------------------------------------------------------------- strmap ----

static void test_strmap(void)
{
    StrMap* m = sm_create(8);
    char keybuf[32];
    int i;
    int foreachCount = 0;

    st_check(m != NULL && sm_count(m) == 0, "strmap.create");

    st_check(sm_put(m, "alpha", (void*)1) &&
             sm_get(m, "alpha") == (void*)1, "strmap.put_get");

    st_check(sm_put(m, "alpha", (void*)2) &&
             sm_count(m) == 1 && sm_get(m, "alpha") == (void*)2,
             "strmap.replace_value");

    st_check(!sm_has(m, "missing"), "strmap.has_false");
    st_check(sm_get(m, "missing") == NULL, "strmap.get_missing_null");

    // 200 keys force rehashes and collisions
    for (i = 0; i < 200; i++) {
        snprintf(keybuf, sizeof(keybuf), "key-%d", i);
        if (!sm_put(m, keybuf, (void*)(long)(i + 10))) {
            break;
        }
    }
    st_check(i == 200 && sm_count(m) == 201, "strmap.200_keys_rehash");

    st_check(sm_get(m, "key-137") == (void*)147, "strmap.get_after_rehash");
    st_check(sm_remove(m, "key-137") && !sm_has(m, "key-137") &&
             sm_count(m) == 200, "strmap.remove");
    st_check(!sm_remove(m, "key-137"), "strmap.remove_twice_false");
    st_check(sm_get(m, "alpha") == (void*)2, "strmap.survivor_intact");

    sm_foreach(m, NULL, NULL); // no-op safety
    st_check(1, "strmap.foreach_null_fn_safe");

    sm_destroy(m);
    st_check(1, "strmap.destroy");
}

// ---------------------------------------------------------- luapattern ----

static int gsub_hex_decode(void* ud, const char* subj, size_t subjLen,
                           const LPCap caps[], int ncaps, StrBuf* out)
{
    (void)ud; (void)subjLen;
    // Lua passes only the explicit captures to the function; with one
    // capture group, caps[0] is the hex pair.
    {
        size_t cs, cl;
        if (lp_cap_span(subj, caps, ncaps, 0, &cs, &cl) && cl == 2) {
            int hi = subj[cs], lo = subj[cs + 1];
            int v = 0;
            if (hi >= '0' && hi <= '9') v = hi - '0';
            else if (hi >= 'a' && hi <= 'f') v = hi - 'a' + 10;
            else if (hi >= 'A' && hi <= 'F') v = hi - 'A' + 10;
            v <<= 4;
            if (lo >= '0' && lo <= '9') v |= lo - '0';
            else if (lo >= 'a' && lo <= 'f') v |= lo - 'a' + 10;
            else if (lo >= 'A' && lo <= 'F') v |= lo - 'A' + 10;
            return sb_append_char(out, (char)v);
        }
    }
    return 0;
}

static int gsub_pct_encode(void* ud, const char* subj, size_t subjLen,
                           const LPCap caps[], int ncaps, StrBuf* out)
{
    (void)ud; (void)ncaps;
    {
        size_t cs, cl;
        if (lp_cap_span(subj, caps, ncaps, 0, &cs, &cl) && cl == 1) {
            return sb_printf(out, "%%%02X", (unsigned char)subj[cs]);
        }
    }
    return 0;
}

static void test_luapattern(void)
{
    char b[256];
    LPCap caps[LP_MAXCAPTURES];
    int nc;
    size_t s0, s1;

    // -- trim: used in url.lua/cookie_jar.lua/address_bar.lua/layout.lua
    // Lua: ("  hello world  "):match("^%s*(.-)%s*$") -> "hello world" (1 cap)
    st_check(lp_match(PLIT("  hello world  "), PLIT("^%s*(.-)%s*$"), 0,
                      caps, &nc) == 1 &&
             cap_str("  hello world  ", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "hello world") == 0, "lpat.trim_spaces");
    st_check(lp_match(PLIT(""), PLIT("^%s*(.-)%s*$"), 0, caps, &nc) == 1 &&
             cap_str("", caps, nc, 0, b, sizeof(b)) && b[0] == '\0',
             "lpat.trim_empty");
    st_check(lp_match(PLIT("   "), PLIT("^%s*(.-)%s*$"), 0, caps, &nc) == 1 &&
             cap_str("   ", caps, nc, 0, b, sizeof(b)) && b[0] == '\0',
             "lpat.trim_only_spaces");

    // -- URL scheme split: url.lua
    // Lua: ("https://example.com/path?q=1"):match("^([a-zA-Z][%w+%-%.]*)://(.*)$")
    st_check(lp_match(PLIT("https://example.com/path?q=1"),
                      PLIT("^([a-zA-Z][%w+%-%.]*)://(.*)$"), 0,
                      caps, &nc) == 2 &&
             cap_str("https://example.com/path?q=1", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "https") == 0 &&
             cap_str("https://example.com/path?q=1", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "example.com/path?q=1") == 0, "lpat.scheme_split");
    st_check(lp_match(PLIT("example.com/no-scheme"),
                      PLIT("^([a-zA-Z][%w+%-%.]*)://(.*)$"), 0,
                      caps, &nc) == LP_ERR_NOMATCH, "lpat.scheme_no_match");

    // -- domain heuristic: readability.lua:36. NOTE (verified vs Lua 5.5):
    // the trailing '?' after the capture group is an optional LITERAL '?'
    // character in Lua patterns (quantifiers never apply to groups), so this
    // pattern only matches strings whose tail is "...<set-char><nonspace>*?"
    // — e.g. "example.com/a?" matches with capture "/a"; bare domains and
    // normal paths do NOT match. We assert exact Lua behavior.
    st_check(lp_find(PLIT("example.com/page?q=x"),
                     PLIT("^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*([/%?][^%s]*)?$"),
                     0, &s0, &s1) == 0, "lpat.host_path_no_match_lua");
    st_check(lp_find(PLIT("example.com"),
                     PLIT("^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*([/%?][^%s]*)?$"),
                     0, &s0, &s1) == 0, "lpat.host_bare_no_match_lua");
    st_check(lp_find(PLIT("not a host"),
                     PLIT("^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*([/%?][^%s]*)?$"),
                     0, &s0, &s1) == 0, "lpat.host_reject_spaces");
    st_check(lp_find(PLIT("example.com/a?"),
                     PLIT("^[a-zA-Z0-9][a-zA-Z0-9%-%.]*%.[a-zA-Z][a-zA-Z0-9%-]*([/%?][^%s]*)?$"),
                     0, &s0, &s1) == 1 && s0 == 0 && s1 == 14,
             "lpat.host_trailing_qmark_lua");

    // -- attribute scanning: svg.lua getAttrs double-quote form
    {
        LPGMatch it;
        const char* subj = "<svg width=\"32\" viewBox=\"0 0 8 8\">";
        size_t slen = strlen(subj);
        lp_gmatch_init(&it, subj, slen, PLIT("([%w%:-]+)%s*=%s*\"([^\"]*)\""));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "width") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "32") == 0, "lpat.attr_dquote_1");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "viewBox") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "0 0 8 8") == 0, "lpat.attr_dquote_2");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.attr_dquote_done");
    }

    // -- single-quote attributes: svg.lua getAttrs second pass
    {
        LPGMatch it;
        const char* subj = "<line x1='0' y1='-8.264'>";
        size_t slen = strlen(subj);
        lp_gmatch_init(&it, subj, slen, PLIT("([%w%:-]+)%s*=%s*'([^']*)'"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "x1") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "0") == 0, "lpat.attr_squote_1");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "-8.264") == 0, "lpat.attr_squote_2");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.attr_squote_done");
    }

    // -- SVG path command splitting: svg.lua
    // Lua gmatch yields M=[10 20] L=[30 40] Z=[]
    {
        LPGMatch it;
        const char* subj = "M10 20L30 40Z";
        lp_gmatch_init(&it, subj, strlen(subj),
                       PLIT("([a-zA-Z])%s*([^a-zA-Z]*)"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "M") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "10 20") == 0, "lpat.pathcmd_M");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "L") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "30 40") == 0, "lpat.pathcmd_L");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "Z") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 b[0] == '\0', "lpat.pathcmd_Z_empty_arg");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.pathcmd_done");
    }

    // -- entities: encoding.lua
    {
        LPGMatch it;
        const char* subj = "&amp;&lt;&#65;";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("&(%a+);"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "amp") == 0, "lpat.entity_named_amp");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "lt") == 0, "lpat.entity_named_lt");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.entity_named_skips_dec");
    }
    st_check(lp_match(PLIT("&#65;"), PLIT("&#(%d+);"), 0, caps, &nc) == 1 &&
             cap_str("&#65;", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "65") == 0, "lpat.entity_decimal");
    {
        LPGMatch it;
        const char* subj = "&#x41;&#X2B;";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("&#[xX](%x+);"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "41") == 0, "lpat.entity_hex_lower_x");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "2B") == 0, "lpat.entity_hex_upper_X");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.entity_hex_done");
    }

    // -- charset sniff: http_client.lua
    st_check(lp_match(PLIT("text/html; charset=UTF-8"),
                      PLIT("charset%s*=%s*([%w%+%-_%.]+)"), 0, caps, &nc) == 1 &&
             cap_str("text/html; charset=UTF-8", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "UTF-8") == 0, "lpat.charset_utf8");

    // -- status line: http_client.lua
    st_check(lp_match(PLIT("HTTP/1.1 301 Moved Permanently"),
                      PLIT("HTTP/%d+%.%d+ (%d+)"), 0, caps, &nc) == 1 &&
             cap_str("HTTP/1.1 301 Moved Permanently", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "301") == 0, "lpat.http_status");

    // -- meta refresh: document.lua
    st_check(lp_match(PLIT("5; URL=http://x.y/z"),
                      PLIT("^(%d+%.?%d*)%s*;%s*[Uu][Rr][Ll]=%s*(.+)$"), 0,
                      caps, &nc) == 2 &&
             cap_str("5; URL=http://x.y/z", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "5") == 0 &&
             cap_str("5; URL=http://x.y/z", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "http://x.y/z") == 0, "lpat.meta_refresh_url_upper");
    st_check(lp_match(PLIT("0.5;url=/rel"),
                      PLIT("^(%d+%.?%d*)%s*;%s*[Uu][Rr][Ll]=%s*(.+)$"), 0,
                      caps, &nc) == 2 &&
             cap_str("0.5;url=/rel", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "0.5") == 0 &&
             cap_str("0.5;url=/rel", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "/rel") == 0, "lpat.meta_refresh_frac");

    // -- viewBox numbers incl negatives: svg.lua
    st_check(lp_match(PLIT(" 0 -8.264 16 16 "),
                      PLIT("^%s*([%-%d%.]+)%s+([%-%d%.]+)%s+([%-%d%.]+)%s+([%-%d%.]+)"),
                      0, caps, &nc) == 4 &&
             cap_str(" 0 -8.264 16 16 ", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "0") == 0 &&
             cap_str(" 0 -8.264 16 16 ", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "-8.264") == 0 &&
             cap_str(" 0 -8.264 16 16 ", caps, nc, 3, b, sizeof(b)) &&
             strcmp(b, "16") == 0, "lpat.viewbox_four_caps");

    // -- tag scan: tokenizer.lua. NOTE: "</div>" does NOT match because '/'
    // is not in [%w%:-] (verified vs Lua: exactly 2 iterations).
    {
        LPGMatch it;
        const char* subj = "<div id=\"a\"><br/></div>";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("<([%w%:-]+)([^>]*)>"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "div") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, " id=\"a\"") == 0, "lpat.tag_div_attrs");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "br") == 0 &&
                 cap_str(subj, caps, nc, 1, b, sizeof(b)) &&
                 strcmp(b, "/") == 0, "lpat.tag_br_selfclose");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.tag_close_not_matched");
    }

    // -- css property: document.lua parseStyle.
    // Lua truth: ([^:]+) grabs "color " INCLUDING the space before ':'
    // (greedy class stops at ':' only); value side trims via (.-)%s*$.
    st_check(lp_match(PLIT(" color : red "),
                      PLIT("^%s*([^:]+)%s*:%s*(.-)%s*$"), 0, caps, &nc) == 2 &&
             cap_str(" color : red ", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "color ") == 0 &&
             cap_str(" color : red ", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "red") == 0, "lpat.css_prop_lua_truth");

    // -- cookie pair: cookie_jar.lua
    st_check(lp_match(PLIT("SID=abc; Path=/"),
                      PLIT("^%s*([^=;%s]+)%s*=%s*(.*)$"), 0, caps, &nc) == 2 &&
             cap_str("SID=abc; Path=/", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "SID") == 0 &&
             cap_str("SID=abc; Path=/", caps, nc, 1, b, sizeof(b)) &&
             strcmp(b, "abc; Path=/") == 0, "lpat.cookie_pair");

    // -- token split: settings_page.lua "[^%s,]+"
    {
        LPGMatch it;
        const char* subj = "a, bb,ccc";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("[^%s,]+"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "a") == 0, "lpat.tokens_a");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "bb") == 0, "lpat.tokens_bb");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "ccc") == 0, "lpat.tokens_ccc");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.tokens_done");
    }

    // -- lazy vs greedy
    st_check(lp_match(PLIT("<a><b>"), PLIT("<(.-)>"), 0, caps, &nc) == 1 &&
             cap_str("<a><b>", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "a") == 0, "lpat.lazy_dash");
    st_check(lp_match(PLIT("<a><b>"), PLIT("<(.*)>"), 0, caps, &nc) == 1 &&
             cap_str("<a><b>", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "a><b") == 0, "lpat.greedy_star");

    // -- end anchor with class set: layout.lua "[%.%?!%:]%s*$"
    st_check(lp_find(PLIT("end."), PLIT("[%.%?!%:]%s*$"), 0, &s0, &s1) == 1 &&
             s0 == 3, "lpat.endpunct_true");
    st_check(lp_find(PLIT("end"), PLIT("[%.%?!%:]%s*$"), 0, &s0, &s1) == 0,
             "lpat.endpunct_false");

    // -- literal escapes (find reports actual match start)
    st_check(lp_find(PLIT("a.b"), PLIT("%."), 0, &s0, &s1) == 1 && s0 == 1,
             "lpat.escape_dot");
    st_check(lp_find(PLIT("50%"), PLIT("%%"), 0, &s0, &s1) == 1 && s0 == 2,
             "lpat.escape_percent");
    st_check(lp_find(PLIT("a-b"), PLIT("a%-b"), 0, &s0, &s1) == 1 && s0 == 0,
             "lpat.escape_dash_outside_set");

    // -- percent-hex decode via function replacement: url.lua decode
    // Lua passes ONLY explicit captures to fn -> caps[0] is the hex pair.
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("a%20b+c"), PLIT("%%(%x%x)"), NULL, 0,
                            gsub_hex_decode, NULL, 1000000, &out);
            st_check(n == 1 && out.len == 5 &&
                     memcmp(out.data, "a b+c", 5) == 0, "lpat.gsub_fn_hexdecode");
        }
        sb_free(&out);
    }

    // -- encode via function replacement: url.lua encode
    // Space is in the allowed set (encoded separately as '+'), so only '!'
    // becomes %21. Verified vs Lua: "a b~c-d_e.f%21".
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("a b~c-d_e.f!"), PLIT("([^%w %-%_%.%~])"),
                            NULL, 0, gsub_pct_encode, NULL, 1000000, &out);
            st_check(n == 1 && out.len == 14 &&
                     memcmp(out.data, "a b~c-d_e.f%21", 14) == 0,
                     "lpat.gsub_fn_encode");
        }
        sb_free(&out);
    }

    // -- digits find with init offset: ("a1b2c3"):find("%d+", 4) -> 4 (1-based)
    st_check(lp_find(PLIT("a1b2c3"), PLIT("%d+"), 3, &s0, &s1) == 1 &&
             s0 == 3 && s1 == 4, "lpat.find_init_offset");

    // -- plain find
    st_check(lp_find_plain(PLIT("xxaby"), PLIT("ab"), 0, &s0, &s1) == 1 &&
             s0 == 2 && s1 == 4, "lpat.find_plain");
    st_check(lp_find_plain(PLIT("xxaby"), PLIT("az"), 0, &s0, &s1) == 0,
             "lpat.find_plain_miss");

    // -- gsub template "%1" trim (address_bar.lua form)
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("  hi  "), PLIT("^%s*(.-)%s*$"), "%1", 2,
                            NULL, NULL, 1000000, &out);
            st_check(n == 1 && out.len == 2 &&
                     memcmp(out.data, "hi", 2) == 0, "lpat.gsub_template_trim");
        }
        sb_free(&out);
    }

    // -- gsub literal newline normalization: url.lua "\n" -> "\r\n"
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("a\nb"), PLIT("\n"), "\r\n", 2, NULL, NULL,
                            1000000, &out);
            st_check(n == 1 && out.len == 4 &&
                     memcmp(out.data, "a\r\nb", 4) == 0, "lpat.gsub_crlf");
        }
        sb_free(&out);
    }

    // -- gsub count + full replace
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("aaa"), PLIT("a"), "b", 1, NULL, NULL,
                            1000000, &out);
            st_check(n == 3 && out.len == 3 &&
                     memcmp(out.data, "bbb", 3) == 0, "lpat.gsub_count3");
        }
        sb_free(&out);
    }

    // -- empty-match gsub semantics: ("abc"):gsub("x*", "-") -> "-a-b-c-", 4
    {
        StrBuf out;
        sb_init(&out);
        {
            int n = lp_gsub(PLIT("abc"), PLIT("x*"), "-", 1, NULL, NULL,
                            1000000, &out);
            st_check(n == 4 && out.len == 7 &&
                     memcmp(out.data, "-a-b-c-", 7) == 0, "lpat.gsub_empty_matches");
        }
        sb_free(&out);
    }

    // -- negated class runs: ("%A+") on "ab--cd" -> "--"
    {
        LPGMatch it;
        const char* subj = "ab--cd";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("%A+"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "--") == 0, "lpat.negclass_A");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.negclass_done");
    }

    // -- set with leading ] : "[]]" matches "]"
    st_check(lp_find(PLIT("]"), PLIT("[]]"), 0, &s0, &s1) == 1 && s0 == 0,
             "lpat.set_leading_bracket");

    // -- NUL bytes inside subject
    {
        const char subj[] = {'a', '\0', 'b'};
        LPGMatch it;
        lp_gmatch_init(&it, subj, 3, PLIT("%a+"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 caps[0].start == 0 && caps[0].len == 1,
                 "lpat.nul_subject_first");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 caps[0].start == 2 && caps[0].len == 1,
                 "lpat.nul_subject_second");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.nul_subject_done");
    }

    // -- position captures: ("abc"):match("()bc()") -> 2, 4 (Lua 1-based)
    st_check(lp_match(PLIT("abc"), PLIT("()bc()"), 0, caps, &nc) == 2 &&
             caps[0].isPosition && caps[0].start == 1 &&
             caps[1].isPosition && caps[1].start == 3, "lpat.position_capture");

    // -- balanced match %b(): ("(a(b)c)d"):match("%b()") -> "(a(b)c)"
    st_check(lp_match(PLIT("(a(b)c)d"), PLIT("%b()"), 0, caps, &nc) == 1 &&
             cap_str("(a(b)c)d", caps, nc, 0, b, sizeof(b)) &&
             strcmp(b, "(a(b)c)") == 0, "lpat.balance");

    // -- frontier %f[%w]%w+: THE quick fox
    {
        LPGMatch it;
        const char* subj = "THE (quick) fox";
        lp_gmatch_init(&it, subj, strlen(subj), PLIT("%f[%w]%w+"));
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "THE") == 0, "lpat.frontier_THE");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "quick") == 0, "lpat.frontier_quick");
        st_check(lp_gmatch_next(&it, caps, &nc) &&
                 cap_str(subj, caps, nc, 0, b, sizeof(b)) &&
                 strcmp(b, "fox") == 0, "lpat.frontier_fox");
        st_check(!lp_gmatch_next(&it, caps, &nc), "lpat.frontier_done");
    }

    // -- optional quantifier ? on a single char class
    st_check(lp_find(PLIT("color"), PLIT("colou?r"), 0, &s0, &s1) == 1,
             "lpat.opt_without_u");
    st_check(lp_find(PLIT("colour"), PLIT("colou?r"), 0, &s0, &s1) == 1,
             "lpat.opt_with_u");

    // -- anchored IP: "^%d+%.%d+%.%d+%.%d+$"
    st_check(lp_find(PLIT("192.168.0.1"), PLIT("^%d+%.%d+%.%d+%.%d+$"), 0,
                     &s0, &s1) == 1 && s0 == 0 && s1 == 11, "lpat.ip_anchor_ok");
    st_check(lp_find(PLIT("192.168.0.1x"), PLIT("^%d+%.%d+%.%d+%.%d+$"), 0,
                     &s0, &s1) == 0, "lpat.ip_anchor_trailing_fail");

    // -- malformed pattern returns LP_ERR_PATTERN (deviation from raise)
    st_check(lp_find(PLIT("abc"), PLIT("["), 0, &s0, &s1) == LP_ERR_PATTERN,
             "lpat.malformed_set_err");
}

// ---------------------------------------------------------------- json ----

static void test_json(void)
{
    char err[128];
    JsonValue* v;
    const JsonValue* tmp;
    size_t slen;

    {
        static const char doc[] =
            "{\"name\":\"pluto\",\"n\":-42,\"f\":2.5,\"e\":2.5e2,\"ok\":true,"
            "\"nil\":null,\"arr\":[1,\"two\",false,null,{\"deep\":\"v\"}],"
            "\"esc\":\"A\\u00e9\\ud83d\\ude00\\n\\t\\\"\"}";
        v = json_parse(doc, sizeof(doc) - 1, err);
        st_check(v != NULL, err[0] ? err : "json.parse_fixture");

        tmp = json_obj_get(v, "name");
        st_check(tmp != NULL && json_str(tmp, &slen) != NULL && slen == 5 &&
                 memcmp(json_str(tmp, NULL), "pluto", 5) == 0,
                 "json.obj_string");

        st_check(json_num(json_obj_get(v, "n"), 9999) == -42.0,
                 "json.negative_int");
        st_check(json_num(json_obj_get(v, "f"), 0) == 2.5, "json.float");
        st_check(json_num(json_obj_get(v, "e"), 0) == 250.0, "json.exponent");
        st_check(json_bool_val(json_obj_get(v, "ok"), 0) == 1, "json.bool_true");
        st_check(json_is_null(json_obj_get(v, "nil")), "json.null_value");
        st_check(json_is_null(json_obj_get(v, "absent")), "json.absent_key");

        tmp = json_obj_get(v, "arr");
        st_check(json_arr_count(tmp) == 5, "json.arr_count");
        st_check(json_num(json_arr_get(tmp, 0), 0) == 1.0, "json.arr_num");
        st_check(json_arr_get(tmp, 3) != NULL &&
                 json_is_null(json_arr_get(tmp, 3)), "json.arr_null_elem");
        {
            const JsonValue* deep = json_obj_get(json_arr_get(tmp, 4), "deep");
            st_check(deep != NULL && json_str(deep, NULL) != NULL &&
                     strcmp(json_str(deep, NULL), "v") == 0, "json.arr_nested_obj");
        }

        tmp = json_obj_get(v, "esc");
        {
            // "A" + é(2 bytes) + U+1F600(4 bytes) + \n + \t + \" = 10 bytes
            const char* es = json_str(tmp, &slen);
            st_check(es != NULL && slen == 10 &&
                     (unsigned char)es[0] == 'A' &&
                     (unsigned char)es[1] == 0xC3 && (unsigned char)es[2] == 0xA9 &&
                     (unsigned char)es[3] == 0xF0 && (unsigned char)es[4] == 0x9F &&
                     (unsigned char)es[5] == 0x98 && (unsigned char)es[6] == 0x80 &&
                     es[7] == '\n' && es[8] == '\t' && es[9] == '"' ,
                     "json.escapes_unicode_surrogate");
        }
        json_free(v);
    }

    // error cases
    v = json_parse("{},x", 4, err);
    st_check(v == NULL && err[0] != '\0', "json.trailing_data_error");
    v = json_parse("{\"a\":}", 6, err);
    st_check(v == NULL, "json.bad_value_error");
    v = json_parse("\"unterminated", 13, err);
    st_check(v == NULL, "json.unterminated_error");

    // builder + writer round-trip
    {
        JsonValue* obj = json_new_object();
        JsonValue* arr = json_new_array();
        JsonValue* inner = json_new_object();
        StrBuf w1, w2;
        JsonValue* back;

        json_arr_append(arr, json_new_number(1));
        json_arr_append(arr, json_new_number(2));
        json_obj_set(inner, "b", json_new_bool(1));
        json_arr_append(arr, inner);
        json_obj_set(obj, "a", arr);
        json_obj_set(obj, "c", json_new_string("x\"y"));

        sb_init(&w1);
        json_write(obj, &w1);
        back = json_parse(w1.data, w1.len, err);
        st_check(back != NULL, "json.roundtrip_parse");
        sb_init(&w2);
        json_write(back, &w2);
        st_check(w1.len == w2.len && memcmp(w1.data, w2.data, w1.len) == 0,
                 "json.roundtrip_stable");
        st_check(json_num(json_arr_get(json_obj_get(back, "a"), 1), 0) == 2.0,
                 "json.roundtrip_arr_value");
        {
            const JsonValue* innerBack =
                json_arr_get(json_obj_get(back, "a"), 2);
            st_check(innerBack != NULL &&
                     json_bool_val(json_obj_get(innerBack, "b"), 0) == 1,
                     "json.roundtrip_nested_bool");
        }

        // replace existing key keeps count stable
        json_obj_set(obj, "c", json_new_number(7));
        st_check(json_arr_count(obj) == 2 &&
                 json_num(json_obj_get(obj, "c"), 0) == 7.0,
                 "json.obj_set_replace");

        json_free(back);
        json_free(obj);
        sb_free(&w1);
        sb_free(&w2);
    }
}

// ---------------------------------------------------------------- entry ----

void selftest_util_run(int* outPass, int* outFail)
{
    PLUTO_LOG("[P02] util selftests begin");
    test_mem();
    test_strbuf();
    test_dynarray();
    test_strmap();
    test_luapattern();
    test_json();
    PLUTO_LOG("[P02] util selftests done: %d passed, %d failed",
              st_pass, st_fail);
    if (outPass) *outPass = st_pass;
    if (outFail) *outFail = st_fail;
}
