// selftest_storage.c — P04 storage + cookie-jar self-tests (see header).
//
// Ground truth source: p04_oracle.lua output (real storage.lua /
// cookie_jar.lua under host Lua 5.5, clock shimmed so nowSeconds()==0
// during the cookie phase). Sections mirror the oracle:
//   MAKE_TS / PARSE_DATE / SET_COOKIE(scNN) / STORE_GET / STORAGE

#include "selftest_storage.h"

#include <stdio.h>
#include <string.h>

#include "../core/logger.h"
#include "../util/dynarray.h"
#include "../util/luanum.h"
#include "../util/strbuf.h"
#include "constants.h"
#include "cookie_jar.h"
#include "storage.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P04] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P04] FAIL %s", name);
    }
}

// ------------------------------------------------------------ clock shim ----

static double s_clock = 0.0;

static double test_now(void)
{
    return s_clock;
}

// ------------------------------------------------------------- helpers ----

static void check_ts(const char* name, double y, double mo, double d,
                     double h, double mi, double s, double want)
{
    double got = cj_make_timestamp(y, mo, d, h, mi, s);
    if (got != want) {
        PLUTO_ERROR("[P04]   ts(%g,%g,%g,%g,%g,%g)=%f want %f", y, mo, d, h,
                    mi, s, got, want);
    }
    st_check(got == want, name);
}

static void check_date(const char* input, double want, int wantOk,
                       const char* name)
{
    double got = 0;
    int ok = cj_parse_date(input, &got);
    if (ok != wantOk || (ok && got != want)) {
        PLUTO_ERROR("[P04]   parseDate('%s') ok=%d val=%f want ok=%d %f",
                    input, ok, got, wantOk, want);
    }
    st_check(ok == wantOk && (!ok || got == want), name);
}

// Full-field Set-Cookie comparison; logs every mismatch.
static void check_sc(const char* name, const char* host, const char* raw,
                     int wantParse, const char* vName, const char* vValue,
                     const char* vDomain, int vHostOnly, const char* vPath,
                     int vSecure, int vHttpOnly, const char* vSamesite,
                     int vHasExp, double vExp, int vDel)
{
    PlutoCookie c;
    char bad[256];
    size_t blen = 0;
    int parsed;

    memset(&c, 0, sizeof(c));
    parsed = cj_parse_set_cookie(host, raw, &c);

#define SCCHK_STR(field, want)                                             \
    do {                                                                   \
        if ((want) != NULL && strcmp((field), (want)) != 0 &&              \
            blen < sizeof(bad)) {                                          \
            int n_ = snprintf(bad + blen, sizeof(bad) - blen, " %s=%s!=%s",\
                              #field, (field), (want));                    \
            if (n_ > 0) blen += (size_t)n_;                                \
        }                                                                  \
    } while (0)
#define SCCHK_INT(field, want)                                             \
    do {                                                                   \
        if ((field) != (want) && blen < sizeof(bad)) {                     \
            int n_ = snprintf(bad + blen, sizeof(bad) - blen, " %s=%d!=%d",\
                              #field, (int)(field), (int)(want));          \
            if (n_ > 0) blen += (size_t)n_;                                \
        }                                                                  \
    } while (0)

    bad[0] = '\0';
    if (!wantParse) {
        if (parsed && blen < sizeof(bad)) {
            int n_ = snprintf(bad + blen, sizeof(bad) - blen, " parsed!=REJECT");
            if (n_ > 0) blen += (size_t)n_;
        }
        st_check(!parsed, name);
        return;
    }
    if (!parsed && blen < sizeof(bad)) {
        int n_ = snprintf(bad + blen, sizeof(bad) - blen, " REJECT!=parsed");
        if (n_ > 0) blen += (size_t)n_;
        PLUTO_ERROR("[P04]   detail:%s", bad);
        st_check(0, name);
        return;
    }
    SCCHK_STR(c.name, vName);
    SCCHK_STR(c.value, vValue);
    SCCHK_STR(c.domain, vDomain);
    SCCHK_STR(c.path, vPath);
    SCCHK_INT(c.hostOnly, vHostOnly);
    SCCHK_INT(c.secure, vSecure);
    SCCHK_INT(c.httpOnly, vHttpOnly);
    if (vSamesite != NULL) {
        SCCHK_STR(c.samesite, vSamesite);
    } else if (c.samesite[0] != '\0' && blen < sizeof(bad)) {
        int n_ = snprintf(bad + blen, sizeof(bad) - blen,
                          " samesite=%s!=unset", c.samesite);
        if (n_ > 0) blen += (size_t)n_;
    }
    SCCHK_INT(c.hasExpires, vHasExp);
    SCCHK_INT(c.del, vDel);
    if (vHasExp && c.expires != vExp && blen < sizeof(bad)) {
        int n_ = snprintf(bad + blen, sizeof(bad) - blen, " exp=%f!=%f",
                          c.expires, vExp);
        if (n_ > 0) blen += (size_t)n_;
    }
#undef SCCHK_STR
#undef SCCHK_INT
    if (blen > 0) {
        PLUTO_ERROR("[P04]   detail:%s", bad);
    }
    st_check(blen == 0, name);
}

static void check_hdr(const char* host, const char* path, int ssl,
                      const char* want, const char* name)
{
    StrBuf sb;
    const char* got;
    sb_init(&sb);
    cj_get_header(host, path, ssl, &sb);
    got = sb.data ? sb.data : "";
    if (strcmp(got, want) != 0) {
        PLUTO_ERROR("[P04]   hdr(%s|%s|%d)='%s' want '%s'", host,
                    path ? path : "(null)", ssl, got, want);
    }
    st_check(strcmp(got, want) == 0, name);
    sb_free(&sb);
}

// ---------------------------------------------------------------- tests ----

static void test_make_timestamp(void)
{
    check_ts("ts.epoch1970", 1970, 1, 1, 0, 0, 0, 0.0);
    check_ts("ts.bttf2015", 2015, 10, 21, 7, 28, 0, 1445412480.0);
    check_ts("ts.leap2000feb29", 2000, 2, 29, 12, 0, 0, 951825600.0);
    check_ts("ts.neg1969", 1969, 12, 31, 23, 59, 59, -1.0);
    check_ts("ts.clamp_month0", 2020, 0, 5, 0, 0, 0, 1578182400.0);
    check_ts("ts.clamp_month13", 2020, 13, 5, 0, 0, 0, 1607126400.0);
    check_ts("ts.clamp_year_neg", -3, 1, 1, 0, 0, 0, -62167219200.0);
    check_ts("ts.clamp_day0", 2020, 3, 0, 0, 0, 0, 1583020800.0);
    check_ts("ts.floor_frac", 2015.9, 10.9, 21.9, 7, 28, 0, 1445412480.0);
    check_ts("ts.frac_hour", 2015, 10, 21, 7.5, 0, 0, 1445412600.0);
    check_ts("ts.nil_hms_zeroed", 2015, 10, 21, 0, 0, 0, 1445385600.0);
}

static void test_parse_date(void)
{
    check_date("Wed, 21 Oct 2015 07:28:00 GMT", 1445412480.0, 1,
               "date.imf_fixdate");
    check_date("Wed, 21-Oct-2015 07:28:00 GMT", 1445412480.0, 1,
               "date.rfc850_4digit");
    check_date("Sunday, 06-Nov-94 08:49:37 GMT", 784111777.0, 1,
               "date.rfc850_2digit");
    check_date("Fri, 31-Dec-69 23:59:59 GMT", 3155759999.0, 1,
               "date.rfc850_pivot69");
    check_date("Sat Feb  3 01:02:03 2001", 0.0, 0, "date.asctime_dblspace_nil");
    check_date("Sat Feb 03 01:02:03 2001", 981162123.0, 1,
               "date.asctime_single_space");
    check_date("wed, 21 oct 2015 07:28:00 GMT", 0.0, 0,
               "date.lowercase_month_nil");
    check_date("not a date", 0.0, 0, "date.garbage_nil");
    check_date("Wed, 21 Oct 2015 07:28:00 GMT extra junk", 1445412480.0, 1,
               "date.trailing_junk_ok");
    check_date("21 Oct 2015 07:28:00 GMT", 0.0, 0, "date.no_weekday_nil");
    check_date("", 0.0, 0, "date.empty_nil");
}

static void test_parse_set_cookie(void)
{
    cj_set_now_fn(test_now); // s_clock == 0 -> matches oracle nowSeconds()
    s_clock = 0.0;

    check_sc("sc01.basic", "example.com", "sid=abc123", 1, "sid", "abc123",
             "example.com", 1, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc02.attrs", "example.com", "a=b; Path=/x; Secure; HttpOnly", 1,
             "a", "b", "example.com", 1, "/x", 1, 1, NULL, 0, 0, 0);
    check_sc("sc03.domain_dot", "example.com", "a=b; Domain=.example.com", 1,
             "a", "b", "example.com", 0, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc04.domain_exact", "example.com", "a=b; Domain=example.com", 1,
             "a", "b", "example.com", 0, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc05.domain_suffix", "sub.example.com",
             "a=b; Domain=example.com", 1, "a", "b", "example.com", 0, "/",
             0, 0, NULL, 0, 0, 0);
    check_sc("sc06.domain_localhost", "localhost", "a=b; Domain=localhost", 1,
             "a", "b", "localhost", 0, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc07.domain_foreign_reject", "example.com",
             "a=b; Domain=other.com", 0, "", "", "", 0, "", 0, 0, NULL, 0, 0,
             0);
    check_sc("sc08.domain_undotted_reject", "example.com",
             "a=b; Domain=example", 0, "", "", "", 0, "", 0, 0, NULL, 0, 0,
             0);
    check_sc("sc09.path_noslash", "example.com", "a=b; Path=noslash", 1, "a",
             "b", "example.com", 1, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc10.samesite_lax", "example.com", "a=b; SameSite=Lax", 1, "a",
             "b", "example.com", 1, "/", 0, 0, "lax", 0, 0, 0);
    check_sc("sc11.samesite_weird_ignored", "example.com",
             "a=b; SameSite=weird", 1, "a", "b", "example.com", 1, "/", 0, 0,
             NULL, 0, 0, 0);
    check_sc("sc12.maxage_future", "example.com", "a=b; Max-Age=3600", 1, "a",
             "b", "example.com", 1, "/", 0, 0, NULL, 1, 3600.0, 0);
    check_sc("sc13.maxage_zero_del", "example.com", "a=b; Max-Age=0", 1, "a",
             "b", "example.com", 1, "/", 0, 0, NULL, 0, 0, 1);
    check_sc("sc14.maxage_negative_del", "example.com", "a=b; Max-Age=-5", 1,
             "a", "b", "example.com", 1, "/", 0, 0, NULL, 0, 0, 1);
    check_sc("sc15.maxage_junk_ignored", "example.com", "a=b; Max-Age=abc", 1,
             "a", "b", "example.com", 1, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc16.expires_imf", "example.com",
             "a=b; Expires=Wed, 21 Oct 2015 07:28:00 GMT", 1, "a", "b",
             "example.com", 1, "/", 0, 0, NULL, 1, 1445412480.0, 0);
    check_sc("sc17.expires_asctime", "example.com",
             "a=b; Expires=Sun Nov 06 08:49:37 1994", 1, "a", "b",
             "example.com", 1, "/", 0, 0, NULL, 1, 784111777.0, 0);
    check_sc("sc18.expires_garbage_ignored", "example.com",
             "a=b; Expires=garbage date", 1, "a", "b", "example.com", 1, "/",
             0, 0, NULL, 0, 0, 0);
    check_sc("sc19.expires_then_maxage_wins", "example.com",
             "a=b; Expires=Wed, 21 Oct 2015 07:28:00 GMT; Max-Age=7200", 1,
             "a", "b", "example.com", 1, "/", 0, 0, NULL, 1, 7200.0, 0);
    check_sc("sc20.maxage_then_expires_skipped", "example.com",
             "a=b; Max-Age=7200; Expires=Wed, 21 Oct 2015 07:28:00 GMT", 1,
             "a", "b", "example.com", 1, "/", 0, 0, NULL, 1, 7200.0, 0);

    // rejections
    {
        struct {
            const char* raw;
            const char* name;
        } rej[] = {
            {"a b=c", "rej.space_in_pair"},
            {"in valid=x", "rej.bad_name"},
            {"=novalue", "rej.no_name"},
            {"k=\"quoted\"", "rej.quote_in_value"},
            {"k=v,w", "rej.comma_in_value"},
            {"justtoken", "rej.no_equals"},
        };
        size_t i;
        for (i = 0; i < sizeof(rej) / sizeof(rej[0]); i++) {
            PlutoCookie c;
            memset(&c, 0, sizeof(c));
            st_check(!cj_parse_set_cookie("example.com", rej[i].raw, &c),
                     rej[i].name);
        }
    }

    check_sc("sc27.empty_value_ok", "example.com", "k=", 1, "k", "",
             "example.com", 1, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc32.dollar_name_ok", "example.com", "$t=k; Unknown=zzz", 1,
             "$t", "k", "example.com", 1, "/", 0, 0, NULL, 0, 0, 0);
    check_sc("sc33.empty_attrs_skipped", "example.com",
             "a=1;;; Path=/p;;", 1, "a", "1", "example.com", 1, "/p", 0, 0,
             NULL, 0, 0, 0);

    // nil/empty inputs
    {
        PlutoCookie c;
        memset(&c, 0, sizeof(c));
        st_check(!cj_parse_set_cookie("", "", &c), "rej.empty_host_raw");
        st_check(!cj_parse_set_cookie(NULL, "a=b", &c), "rej.null_host");
        st_check(!cj_parse_set_cookie("example.com", NULL, &c),
                 "rej.null_raw");
        st_check(!cj_parse_set_cookie("example.com", "   ", &c),
                 "rej.ws_only_raw");
    }
}

static void test_store_get_header(void)
{
    cj_set_now_fn(test_now);
    s_clock = 0.0;
    cj_clear();

    cj_store("example.com", "sid=a");
    cj_store("example.com", "pref=dark");
    check_hdr("example.com", "/", 1, "sid=a; pref=dark", "hdr.two_cookies");

    cj_store("example.com", "sid=b"); // replace -> moves to END
    check_hdr("example.com", "/", 1, "pref=dark; sid=b", "hdr.replace_moves_end");
    st_check(cj_count() == 2, "store.replace_count");

    cj_store("example.com", "pref=; Max-Age=0"); // delete one
    check_hdr("example.com", "/", 1, "sid=b", "hdr.after_delete");
    st_check(cj_count() == 1, "store.delete_count");

    cj_store("shop.example.com", "dcart=9; Domain=example.com");
    check_hdr("shop.example.com", "/", 1, "dcart=9", "hdr.domain_cookie");
    check_hdr("other.com", "/", 1, "", "hdr.cross_host_isolated");
    st_check(cj_count() == 2, "store.domain_count");

    cj_store("sec.example.com", "tok=t; Secure");
    check_hdr("sec.example.com", "/", 0, "dcart=9", "hdr.secure_blocked_http");
    check_hdr("sec.example.com", "/deep/x", 1, "dcart=9; tok=t",
              "hdr.secure_allowed_https");
    st_check(cj_count() == 3, "store.secure_count");

    cj_store("docs.example.com", "docsid=d; Path=/docs");
    check_hdr("docs.example.com", "/doc", 1, "dcart=9", "hdr.path_prefix_no");
    check_hdr("docs.example.com", "/docs", 1, "dcart=9; docsid=d",
              "hdr.path_exact_yes");
    check_hdr("docs.example.com", "/docs/sub/page.html", 1,
              "dcart=9; docsid=d", "hdr.path_subdir_yes");
    check_hdr("docs.example.com", "/docsfile", 1, "dcart=9",
              "hdr.path_boundary_no");
    st_check(cj_count() == 4, "store.docs_count");

    // cap 300 with oldest-first eviction (oracle: cap_first=c6 cap_last=c305)
    {
        char raw[64];
        int i;
        for (i = 1; i <= 305; i++) {
            snprintf(raw, sizeof(raw), "c%d=%d", i, i);
            cj_store("cap.example.com", raw);
        }
        st_check(cj_count() == 300, "cap.count300");
        {
            DynArray* list = storage_cookies();
            PlutoCookie* firstC = (PlutoCookie*)da_get(list, 0);
            PlutoCookie* lastC =
                (PlutoCookie*)da_get(list, list->count - 1);
            st_check(firstC != NULL && strcmp(firstC->name, "c6") == 0,
                     "cap.first_evicted_to_c6");
            st_check(lastC != NULL && strcmp(lastC->name, "c305") == 0,
                     "cap.last_is_c305");
        }
    }

    // past-Expires relative to clock 0 is FUTURE -> stored, evicting one
    cj_store("del.example.com",
             "gone=g; Expires=Wed, 01 Jan 2020 00:00:00 GMT");
    st_check(cj_count() == 300, "cap.future_expires_stored_evicts");

    cj_clear();
    st_check(cj_count() == 0, "clear.zeroes");
}

static void test_expiry_pruning_with_clock(void)
{
    DynArray* list;
    cj_set_now_fn(test_now);
    cj_clear();

    s_clock = 1000000.0;
    cj_store("h.com", "s=1; Max-Age=100"); // expires 1000100
    st_check(cj_count() == 1, "exp.stored");
    check_hdr("h.com", "/", 1, "s=1", "exp.alive_before");

    s_clock = 1000101.0;
    check_hdr("h.com", "/", 1, "", "exp.pruned_after");
    list = storage_cookies();
    st_check(list->count == 0, "exp.list_empty_after_prune");

    cj_clear();
    s_clock = 0.0;
}

static void test_storage_logic(void)
{
    DynArray *bmArr, *hArr;
    PlutoSavedBookmark* bm;
    PlutoHistoryItem* h;

    storage_reset_defaults(); // deterministic start; disk untouched
    bmArr = storage_bookmarks();
    hArr = storage_history();

    st_check(bmArr->count == 9, "st.defaults_bm9");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 0);
    st_check(bm != NULL && strcmp(bm->title, "Google") == 0 &&
                 strcmp(bm->url, "https://google.com") == 0 &&
                 strcmp(bm->desc, "Search engine") == 0,
             "st.default_bm1_fields");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 7);
    st_check(bm != NULL &&
                 strcmp(bm->title, "Motherfucking Website") == 0,
             "st.default_bm8_title");
    {
        PlutoSettings* set = storage_settings();
        st_check(set->searchEngine == 1 && set->mode == PLUTO_MODE_RAW_HTML &&
                     set->autoReader == 0 &&
                     strcmp(set->fontSize, "medium") == 0 &&
                     set->imageMode == PLUTO_IMAGE_MODE_VIEWPORT &&
                     set->invertCrank == 0 &&
                     set->protocol == PLUTO_PROTOCOL_HTTP,
                 "st.default_settings");
    }

    // history rules
    storage_add_history(NULL, "about:blank");
    storage_add_history(NULL, "");
    st_check(hArr->count == 0, "hist.about_and_empty_skipped");

    storage_add_history("Page One", "https://a.example/");
    storage_add_history("Page Two", "https://b.example/");
    st_check(hArr->count == 2, "hist.two_added");
    h = (PlutoHistoryItem*)da_get(hArr, 1);
    st_check(h != NULL && strcmp(h->title, "Page One") == 0,
             "hist.insert_front_order");
    st_check(strlen(h->time) == 5 && h->time[2] == ':', "hist.time_hhmm");

    storage_add_history(NULL, "https://a.example/"); // dedup move-to-front
    st_check(hArr->count == 2, "hist.dedup_count");
    h = (PlutoHistoryItem*)da_get(hArr, 0);
    st_check(h != NULL && strcmp(h->url, "https://a.example/") == 0 &&
                 strcmp(h->title, "https://a.example/") == 0,
             "hist.dedup_moves_front_title_defaults_url");

    {
        int i;
        char url[64];
        for (i = 1; i <= 55; i++) {
            snprintf(url, sizeof(url), "https://fill%d.example/", i);
            storage_add_history(NULL, url);
        }
    }
    st_check(hArr->count == 50, "hist.cap50");
    h = (PlutoHistoryItem*)da_get(hArr, 0);
    st_check(h != NULL && strcmp(h->url, "https://fill55.example/") == 0,
             "hist.cap_newest_first");
    h = (PlutoHistoryItem*)da_get(hArr, 49);
    st_check(h != NULL && strcmp(h->url, "https://fill6.example/") == 0,
             "hist.cap_oldest_evicted_fill1to5_gone");

    // bookmarks
    st_check(storage_is_bookmarked("https://google.com") == 1,
             "bm.is_true");
    st_check(storage_is_bookmarked("https://nope.example") == 0,
             "bm.is_false");
    st_check(storage_add_bookmark(NULL, "", NULL) == 0, "bm.add_empty_rejected");
    st_check(storage_add_bookmark("Renamed Google", "https://google.com",
                                  "new desc") == 1,
             "bm.dup_returns_true");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 0);
    st_check(bm != NULL && strcmp(bm->title, "Renamed Google") == 0 &&
                 strcmp(bm->desc, "Search engine") == 0 && bmArr->count == 9,
             "bm.dup_updates_title_only_keeps_desc");
    st_check(storage_add_bookmark("Zed", "https://zed.example/", "the end") ==
                     1,
             "bm.append_true");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 9);
    st_check(bm != NULL && strcmp(bm->title, "Zed") == 0 && bmArr->count == 10,
             "bm.appended_last");
    st_check(storage_remove_bookmark(10) == 1 && bmArr->count == 9,
             "bm.remove_valid");
    st_check(storage_remove_bookmark(0) == 0, "bm.remove_zero_invalid");
    st_check(storage_remove_bookmark(999) == 0, "bm.remove_big_invalid");

    // save / load round-trip through real JSON on disk
    st_check(storage_save() == 1, "st.save_ok");
    bmArr->count = 0;
    hArr->count = 0;
    storage_cookies()->count = 0;
    st_check(storage_load() == 1, "st.load_ok");
    st_check(bmArr->count == 9, "st.roundtrip_bm9");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 0);
    st_check(bm != NULL && strcmp(bm->title, "Renamed Google") == 0,
             "st.roundtrip_bm1");
    h = (PlutoHistoryItem*)da_get(hArr, 0);
    st_check(h != NULL && strcmp(h->url, "https://fill55.example/") == 0,
             "st.roundtrip_hist_top");
    {
        PlutoSettings* set = storage_settings();
        st_check(set->searchEngine == 1 &&
                     strcmp(set->fontSize, "medium") == 0 &&
                     set->mode == PLUTO_MODE_RAW_HTML,
                 "st.roundtrip_settings");
    }

    // empty-bookmark array in file -> defaults restored (Lua parity)
    bmArr->count = 0; // save an EMPTY bookmarks array
    st_check(storage_save() == 1, "st.save_empty_bm");
    st_check(storage_load() == 1, "st.load_empty_bm");
    st_check(bmArr->count == 9, "st.empty_bm_falls_back_to_defaults");
    bm = (PlutoSavedBookmark*)da_get(bmArr, 0);
    st_check(bm != NULL && strcmp(bm->title, "Google") == 0,
             "st.empty_bm_default_google");
}

static void test_luanum(void)
{
    double v;
    st_check(pluto_str_tonumber_strict("8080abc", &v) == 0, "num.junk_fail");
    st_check(pluto_str_tonumber_strict("", &v) == 0, "num.empty_fail");
    st_check(pluto_str_tonumber_strict("12,5", &v) == 0, "num.comma_fail");
    st_check(pluto_str_tonumber_strict(" 42 ", &v) == 1 && v == 42.0,
             "num.ws_trimmed");
    st_check(pluto_str_tonumber_strict("0x50", &v) == 1 && v == 80.0,
             "num.hex");
    st_check(pluto_str_tonumber_strict("1e3", &v) == 1 && v == 1000.0,
             "num.exponent");
}

void selftest_storage_run(int* outPass, int* outFail)
{
    PLUTO_LOG("[P04] storage+cookiejar selftests begin");
    test_make_timestamp();
    test_parse_date();
    test_parse_set_cookie();
    test_store_get_header();
    test_expiry_pruning_with_clock();
    test_storage_logic();
    test_luanum();
    PLUTO_LOG("[P04] storage+cookiejar selftests done: %d passed, %d failed",
              st_pass, st_fail);
    if (outPass) *outPass = st_pass;
    if (outFail) *outFail = st_fail;
}
