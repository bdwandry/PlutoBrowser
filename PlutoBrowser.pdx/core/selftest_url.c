// selftest_url.c — P03 core/url self-tests (see header).
//
// Ground truth captured from the Lua original (url.lua) via host lua:
//   PARSE   : 14 fixtures incl. about:, userinfo colon quirk, ":0"/":" ports
//   ENCODE  : 8 fixtures incl. UTF-8, \n -> %0D%0A, %2550
//   DECODE  : 6 fixtures incl. invalid-escape passthrough
//   ISSEARCH: 14 inputs incl. "   " -> query, "HTTP://X.COM" -> URL (dot rule)
//   UNWRAP  : 3 fixtures (DDG /l/ uddg=)
//   RESOLVE : 16 fixtures incl. dot-segment pops, mailto: NOT special,
//             uppercase absolute passthrough

#include "selftest_url.h"

#include <stdio.h>
#include <string.h>

#include "../core/logger.h"
#include "../util/strbuf.h"
#include "url.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P03] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P03] FAIL %s", name);
    }
}

// ---------------------------------------------------------------- parse ----

typedef struct {
    const char* name;
    const char* url;
    const char* norm;
    const char* scheme;
    const char* host;
    int         port;
    const char* path;
    const char* query;
    const char* hash;
    const char* fullPath;
    int         ssl;
} ParseCase;

static void check_parse(const ParseCase* c)
{
    PlutoUrl u;
    char bad[192];
    size_t blen = 0;

    url_parse(c->url, &u);
#define CHK(field, want)                                                   \
    do {                                                                   \
        if (strcmp(field, (want)) != 0 && blen < sizeof(bad)) {            \
            int n = snprintf(bad + blen, sizeof(bad) - blen,               \
                             " %s='%s'!= '%s'", #field, field, (want));    \
            if (n > 0) blen += (size_t)n;                                  \
        }                                                                  \
    } while (0)

    bad[0] = '\0';
    CHK(u.normalized, c->norm);
    CHK(u.scheme, c->scheme);
    CHK(u.host, c->host);
    CHK(u.path, c->path);
    CHK(u.query, c->query);
    CHK(u.hash, c->hash);
    CHK(u.fullPath, c->fullPath);
#undef CHK
    if (u.port != c->port && blen < sizeof(bad)) {
        int n = snprintf(bad + blen, sizeof(bad) - blen, " port=%d!=%d",
                         u.port, c->port);
        if (n > 0) blen += (size_t)n;
    }
    if (u.isSsl != c->ssl && blen < sizeof(bad)) {
        int n = snprintf(bad + blen, sizeof(bad) - blen, " ssl=%d!=%d",
                         u.isSsl, c->ssl);
        if (n > 0) blen += (size_t)n;
    }
    if (blen > 0) {
        PLUTO_ERROR("[P03]   detail:%s", bad);
    }
    st_check(blen == 0, c->name);
}

static void test_parse(void)
{
    static const ParseCase cases[] = {
        {"url.parse.empty", "",
         "about:blank", "about", "blank", 0, "/", "", "", "/", 1},
        {"url.parse.https_host", "https://example.com",
         "https://example.com/", "https", "example.com", 443, "/", "", "", "/",
         1},
        {"url.parse.full",
         "http://Example.COM:8080/path/page.html?a=b&c=d#frag",
         "http://example.com:8080/path/page.html?a=b&c=d#frag", "http",
         "example.com", 8080, "/path/page.html", "a=b&c=d", "frag",
         "/path/page.html?a=b&c=d#frag", 0},
        {"url.parse.no_scheme", "example.com/path?q=1#h",
         "https://example.com/path?q=1#h", "https", "example.com", 443,
         "/path", "q=1", "h", "/path?q=1#h", 1},
        {"url.parse.empty_host", "https://",
         "https:///", "https", "", 443, "/", "", "", "/", 1},
        {"url.parse.ftp_custom_port", "ftp://files.example.com:2121/pub",
         "ftp://files.example.com:2121/pub", "ftp", "files.example.com", 2121,
         "/pub", "", "", "/pub", 0},
        {"url.parse.userinfo_quirk", "http://user:pass@example.com/x",
         "http://user/x", "http", "user", 80, "/x", "", "", "/x", 0},
        {"url.parse.colon_empty_port", "https://example.com:",
         "https://example.com/", "https", "example.com", 443, "/", "", "", "/",
         1},
        {"url.parse.port_zero", "https://example.com:0/",
         "https://example.com/", "https", "example.com", 443, "/", "", "", "/",
         1},
        {"url.parse.spaces_trimmed", "  https://sp.example.com/x  ",
         "https://sp.example.com/x", "https", "sp.example.com", 443, "/x", "",
         "", "/x", 1},
        {"url.parse.about_blank", "about:blank",
         "about:blank", "about", "blank", 0, "/blank", "", "", "/blank", 1},
        {"url.parse.about_home", "about:home",
         "about:home", "about", "home", 0, "/home", "", "", "/home", 1},
        {"url.parse.case_fold", "HTTPS://UPPER.Example.COM/PaTh",
         "https://upper.example.com/PaTh", "https", "upper.example.com", 443,
         "/PaTh", "", "", "/PaTh", 1},
        {"url.parse.ip_host", "http://192.168.1.1/admin",
         "http://192.168.1.1/admin", "http", "192.168.1.1", 80, "/admin", "",
         "", "/admin", 0},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check_parse(&cases[i]);
    }

    // raw keeps the trimmed input only
    {
        PlutoUrl u;
        url_parse("  https://sp.example.com/x  ", &u);
        st_check(strcmp(u.raw, "https://sp.example.com/x") == 0, "url.parse.raw_trimmed");
    }
    // strict tonumber semantics on custom port
    {
        PlutoUrl u;
        url_parse("http://h.example:8080abc/", &u);
        st_check(u.port == 80 && strcmp(u.normalized, "http://h.example/") == 0,
                 "url.parse.junk_port_ignored");
    }
}

// ------------------------------------------------------- encode / decode ----

static void test_encode_decode(void)
{
    struct {
        const char* name;
        const char* in;
        const char* want;
    } enc[] = {
        {"enc.plain", "hello world", "hello+world"},
        {"enc.specials", "&a=b/c?", "%26a%3Db%2Fc%3F"},
        {"enc.unreserved_kept", "a-b_c.d~e", "a-b_c.d~e"},
        {"enc.plus_encoded", "a+b", "a%2Bb"},
        {"enc.utf8", "caf\xC3\xA9", "caf%C3%A9"},
        {"enc.percent_literal", "%50", "%2550"},
        {"enc.newline_crlf", "a\nb", "a%0D%0Ab"},
        {"enc.empty", "", ""},
    };
    size_t i;
    for (i = 0; i < sizeof(enc) / sizeof(enc[0]); i++) {
        StrBuf sb;
        sb_init(&sb);
        url_encode(enc[i].in, &sb);
        {
            const char* got = sb.data ? sb.data : "";
            if (strcmp(got, enc[i].want) != 0) {
                PLUTO_ERROR("[P03]   encode('%s')='%s' want '%s'",
                            enc[i].in, got, enc[i].want);
            }
            st_check(strcmp(got, enc[i].want) == 0, enc[i].name);
        }
        sb_free(&sb);
    }

    {
        struct {
            const char* name;
            const char* in;
            const char* want;
        } dec[] = {
            {"dec.plus_space", "a+b", "a b"},
            {"dec.pct_utf8", "%C3%A9", "\x01"}, // want overridden below
            {"dec.mixed", "a%20b+c", "a b c"},
            {"dec.invalid_pct_kept", "%zz", "%zz"},
            {"dec.truncated_pct_kept", "100%", "100%"},
            {"dec.empty", "", ""},
        };
        (void)0;
        for (i = 0; i < sizeof(dec) / sizeof(dec[0]); i++) {
            StrBuf sb;
            const char* want = dec[i].want;
            if (i == 1) {
                want = "\xC3\xA9"; // raw UTF-8 bytes for é
            }
            sb_init(&sb);
            url_decode(dec[i].in, &sb);
            {
                const char* got = sb.data ? sb.data : "";
                if (strcmp(got, want) != 0) {
                    PLUTO_ERROR("[P03]   decode('%s') len=%d want len=%d",
                                dec[i].in, (int)sb.len, (int)strlen(want));
                }
                st_check(strcmp(got, want) == 0, dec[i].name);
            }
            sb_free(&sb);
        }
    }

    // round-trip: decode(encode(x)) restores original bytes. Note: inputs
    // containing literal '+' do NOT round-trip in the Lua original either
    // (decode maps every '+' to space before %XX handling).
    {
        const char* src = "a b&c=d/e?f~g-h_i.j";
        StrBuf e, d;
        sb_init(&e);
        sb_init(&d);
        url_encode(src, &e);
        url_decode(e.data ? e.data : "", &d);
        st_check(d.len == strlen(src) &&
                 memcmp(d.data ? d.data : "", src, d.len) == 0,
                 "url.roundtrip");
        sb_free(&e);
        sb_free(&d);
    }
}

// ------------------------------------------------------------ isSearchQuery ----

static void test_is_search_query(void)
{
    struct {
        const char* in;   // NULL sentinel supported
        int         want;
        const char* name;
    } cases[] = {
        {NULL, 0, "isq.null_false"},
        {"", 0, "isq.empty_false"},
        {"hello world", 1, "isq.spaces_query"},
        {"example.com", 0, "isq.dot_url"},
        {"example", 1, "isq.bare_word_query"},
        {".hidden", 1, "isq.leading_dot_query"},
        {"trailing.", 1, "isq.trailing_dot_query"},
        {"192.168.1.1", 0, "isq.ip_url"},
        {"localhost", 0, "isq.localhost_url"},
        {"localhost.dev", 0, "isq.localhost_suffix_url"},
        {"https://x.com", 0, "isq.https_url"},
        {"about:blank", 0, "isq.about_url"},
        {"file:///tmp/x", 0, "isq.file_url"},
        {"   ", 1, "isq.spaces_only_is_query"},
        {"HTTP://X.COM", 0, "isq.upper_scheme_via_dot_rule"},
        {"  example.org  ", 0, "isq.trimmed_before_dot_rule"},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = url_is_search_query(cases[i].in);
        if (got != cases[i].want) {
            PLUTO_ERROR("[P03]   isSearchQuery('%s')=%d want %d",
                        cases[i].in ? cases[i].in : "(null)", got,
                        cases[i].want);
        }
        st_check(got == cases[i].want, cases[i].name);
    }
}

// ---------------------------------------------------------------- unwrap ----

static void test_unwrap_redirect(void)
{
    StrBuf out;

    sb_init(&out);
    if (!url_unwrap_redirect(
            "https://duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpage"
            "&rut=abc",
            &out)) {
        PLUTO_ERROR("[P03]   unwrap returned false");
    } else if (out.data == NULL ||
               strcmp(out.data, "https://example.com/page") != 0) {
        PLUTO_ERROR("[P03]   unwrap got '%s'", out.data ? out.data : "(null)");
    }
    st_check(out.data != NULL &&
                 strcmp(out.data, "https://example.com/page") == 0,
             "unwrap.ddg_uddg");
    sb_clear(&out);

    if (url_unwrap_redirect("https://example.com/l/?uddg=x", &out)) {
        PLUTO_ERROR("[P03]   unwrap non-ddg unexpectedly succeeded");
    }
    st_check(!url_unwrap_redirect("https://example.com/l/?uddg=x", &out),
             "unwrap.non_ddg_false");

    if (url_unwrap_redirect("https://duckduckgo.com/html/?q=x", &out)) {
        PLUTO_ERROR("[P03]   unwrap missing-uddg unexpectedly succeeded");
    }
    st_check(!url_unwrap_redirect("https://duckduckgo.com/html/?q=x", &out),
             "wrap.missing_uddg_false");
    sb_free(&out);
}

// ---------------------------------------------------------------- resolve ----

static void check_resolve(const char* base, const char* rel,
                          const char* want, const char* name)
{
    StrBuf out;
    sb_init(&out);
    url_resolve(base, rel, &out);
    {
        const char* got = out.data ? out.data : "";
        if (strcmp(got, want) != 0) {
            PLUTO_ERROR("[P03]   resolve(%s, %s)='%s' want '%s'",
                        base, rel, got, want);
        }
        st_check(strcmp(got, want) == 0, name);
    }
    sb_free(&out);
}

static void test_resolve(void)
{
    const char* B1 = "https://example.com/dir/page.html";
    const char* B2 = "http://b.com:8080/base/i.html?q=1#f";

    check_resolve(B1, "img.png", "https://example.com/dir/img.png",
                  "resolve.same_dir");
    check_resolve(B1, "sub/thing.css", "https://example.com/dir/sub/thing.css",
                  "resolve.subdir");
    check_resolve(B1, "/root.txt", "https://example.com/root.txt",
                  "resolve.root_rel");
    check_resolve(B1, "//other.org/x", "https://other.org/x",
                  "resolve.protocol_relative");
    check_resolve(B1, "#sec", "https://example.com/dir/page.html#sec",
                  "resolve.anchor");
    check_resolve(B1, "?new=1", "https://example.com/dir/page.html?new=1",
                  "resolve.query_only");

    check_resolve(B2, "rel.png", "http://b.com:8080/base/rel.png",
                  "resolve.custom_port_base");
    check_resolve(B2, "#frag2", "http://b.com:8080/base/i.html?q=1#frag2",
                  "resolve.anchor_keeps_query");
    check_resolve(B2, "?q=9", "http://b.com:8080/base/i.html?q=9",
                  "resolve.query_replaces");
    check_resolve(B2, "./up.png", "http://b.com:8080/base/up.png",
                  "resolve.dot_slash");
    check_resolve(B2, "../up2.png", "http://b.com:8080/up2.png",
                  "resolve.dotdot");
    check_resolve(B2, "../../too-far.png", "http://b.com:8080/too-far.png",
                  "resolve.dotdot_clamped_root");
    check_resolve(B2, "../../../../too-far.png", "http://b.com:8080/too-far.png",
                  "resolve.deep_beyond_root_dropped");
    check_resolve(B1, "about:home", "about:home", "resolve.about_passthrough");
    check_resolve(B1, "data:text/plain,X", "data:text/plain,X",
                  "resolve.data_passthrough");
    check_resolve(B1, "javascript:alert(1)", "javascript:alert(1)",
                  "resolve.js_passthrough");
    check_resolve(B1, "HTTPS://ABS.EXAMPLE/PATH", "HTTPS://ABS.EXAMPLE/PATH",
                  "resolve.absolute_verbatim_case");
    check_resolve(B2, "mailto:a@b.c", "http://b.com:8080/base/mailto:a@b.c",
                  "resolve.mailto_not_special");
    check_resolve(B1, "", B1, "resolve.empty_returns_base");
}

// ----------------------------------------------------------- buildSearchUrl ----

static void test_build_search_url(void)
{
    StrBuf out;
    sb_init(&out);
    url_build_search_url("https://duckduckgo.com/?q=", "cats & dogs",
                         &out);
    {
        const char* got = out.data ? out.data : "";
        const char* want =
            "https://duckduckgo.com/?q=cats+%26+dogs";
        if (strcmp(got, want) != 0) {
            PLUTO_ERROR("[P03]   searchUrl='%s' want '%s'", got, want);
        }
        st_check(strcmp(got, want) == 0, "searchurl.ddg");
    }
    sb_free(&out);
}

// ---------------------------------------------------------------- entry ----

void selftest_url_run(int* outPass, int* outFail)
{
    PLUTO_LOG("[P03] url selftests begin");
    test_parse();
    test_encode_decode();
    test_is_search_query();
    test_unwrap_redirect();
    test_resolve();
    test_build_search_url();
    PLUTO_LOG("[P03] url selftests done: %d passed, %d failed",
              st_pass, st_fail);
    if (outPass) *outPass = st_pass;
    if (outFail) *outFail = st_fail;
}
