// selftest_encoding.c — P06 verification suite.
//
// Every expectation mirrors observable behavior of Source/core/encoding.lua
// and Source/html/entities.lua captured via the host-Lua oracle
// (p06_oracle.lua -> p06_truth.txt): CP1252 table edges (0x81 self-map,
// 0x7F raw), UTF-16 lone-high-surrogate consuming the next unit, quoted
// header charset values FAILING the Lua pattern, meta-scan 1024-byte window,
// header-beats-meta precedence (unless header is utf-8), entity fast path,
// hex/decimal special-case asymmetry (0x201A/0x201E missing from hex),
// per-byte emoji widening, truncated-tail handling.
#include <stdio.h>
#include <string.h>

#include "../core/logger.h"
#include "../core/tasks.h"
#include "../html/entities.h"
#include "encoding.h"
#include "../util/mem.h"
#include "../util/strbuf.h"

static int st_pass = 0;
static int st_fail = 0;

static void st_check(int cond, const char* name)
{
    if (cond) {
        st_pass++;
        PLUTO_LOG("[P06] PASS %s", name);
    } else {
        st_fail++;
        PLUTO_ERROR("[P06] FAIL %s", name);
    }
}

// Compares a (ptr,len) result against a NUL-terminated expectation.
static void check_mem(const char* name, const char* got, size_t gotLen,
                      const char* exp)
{
    size_t expLen = strlen(exp);
    int ok = got != NULL && gotLen == expLen &&
             (expLen == 0 || memcmp(got, exp, expLen) == 0);
    if (!ok && got != NULL) {
        PLUTO_ERROR("[P06]   %-38s gotLen=%u expLen=%u", name,
                    (unsigned)gotLen, (unsigned)expLen);
    }
    st_check(ok, name);
}

// ------------------------------------------------------- fake clock ----
static unsigned g_now = 0;
static unsigned fake_clock(void) { return g_now; }

// --------------------------------------------- in-task decode fixture ----

typedef struct EncJob {
    int ran;
    size_t outLen;
    char first[16];
} EncJob;

static int step_entity_job(void* ctx)
{
    EncJob* j = (EncJob*)ctx;
    StrBuf in;
    char* out;
    int i;
    sb_init(&in);
    for (i = 0; i < 2000; i++) {
        sb_append_str(&in, "&amp;<");
    }
    out = entities_decode(in.data, in.len, &j->outLen);
    snprintf(j->first, sizeof(j->first), "%.6s", out ? out : "");
    pluto_free(out);
    pluto_free(in.data);
    j->ran = 1;
    return PLUTO_TASK_DONE;
}

// ---------------------------------------------------------------- tests ----

void selftest_encoding_run(int* outPass, int* outFail)
{
    size_t n;
    char* r;
    char body[1400];

    // main.c performs encoding_init/entities_init; tests swap the clock so
    // the P05 budget gate can never fire mid-decode (frozen clock parity).
    tasks_set_clock_fn(fake_clock);
    g_now = 5000;

    // ---- A. CP1252 single bytes (oracle A) --------------------------------
    {
        static const char b41[] = {(char)0x41};
        static const char b7f[] = {(char)0x7F};
        static const char b80[] = {(char)0x80};
        static const char b81[] = {(char)0x81};
        static const char b92[] = {(char)0x92};
        static const char b9c[] = {(char)0x9C};
        static const char b9f[] = {(char)0x9F};
        static const char ba0[] = {(char)0xA0};
        static const char be9[] = {(char)0xE9};
        static const char bff[] = {(char)0xFF};

        r = encoding_to_utf8(b41, 1, NULL, &n);
        check_mem("cp1252.A41_raw_ascii", r, n, "A");
        pluto_free(r);

        r = encoding_to_utf8(b7f, 1, NULL, &n);
        check_mem("cp1252.7f_stays_raw", r, n, "\x7F");
        pluto_free(r);

        // High-byte probes need a dispatching charset (no header/meta ->
        // verbatim, as proven by cp1252.no_header_verbatim above).
        r = encoding_to_utf8(b80, 1, "charset=windows-1252", &n);
        check_mem("cp1252.80_is_euro", r, n, "\xE2\x82\xAC");
        pluto_free(r);

        r = encoding_to_utf8(b81, 1, "charset=windows-1252", &n);
        check_mem("cp1252.81_self_map_C281", r, n, "\xC2\x81");
        pluto_free(r);

        r = encoding_to_utf8(b92, 1, "charset=windows-1252", &n);
        check_mem("cp1252.92_right_quote", r, n, "\xE2\x80\x99");
        pluto_free(r);

        r = encoding_to_utf8(b9c, 1, "charset=windows-1252", &n);
        check_mem("cp1252.9c_oe_ligature", r, n, "\xC5\x93");
        pluto_free(r);

        r = encoding_to_utf8(b9f, 1, "charset=windows-1252", &n);
        check_mem("cp1252.9f_Y_dieresis", r, n, "\xC5\xB8");
        pluto_free(r);

        r = encoding_to_utf8(ba0, 1, "charset=windows-1252", &n);
        check_mem("cp1252.a0_passthrough_C2A0", r, n, "\xC2\xA0");
        pluto_free(r);

        // No dispatch (NULL header, no meta) -> verbatim single byte.
        r = encoding_to_utf8(be9, 1, NULL, &n);
        check_mem("cp1252.no_header_verbatim", r, n, "\xE9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "text/html; charset=windows-1252", &n);
        check_mem("cp1252.e9_decodes_C3A9", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(bff, 1, "charset=windows-1252", &n);
        check_mem("cp1252.ff_decodes_C3BF", r, n, "\xC3\xBF");
        pluto_free(r);
    }

    // ---- B. UTF-16 (oracle B/B2) -------------------------------------------
    {
        static const char leA[] = {(char)0xFF, (char)0xFE, 0x41, 0x00};
        static const char lePair[] = {(char)0xFF, (char)0xFE, 0x00, (char)0xD8,
                                      0x37, (char)0xDC};
        static const char leLone[] = {(char)0xFF, (char)0xFE, 0x00, (char)0xD8,
                                      0x41, 0x00};
        static const char beMix[] = {(char)0xFE, (char)0xFF, 0x00, (char)0xE9,
                                     0x20, (char)0xAC, 0x27, 0x13};

        r = encoding_to_utf8(leA, sizeof(leA), NULL, &n);
        check_mem("utf16le.bom_A", r, n, "A");
        pluto_free(r);

        r = encoding_to_utf8(lePair, sizeof(lePair), NULL, &n);
        check_mem("utf16le.surrogate_pair_U10037", r, n,
                  "\xF0\x90\x80\xB7");
        pluto_free(r);

        // Lone high surrogate emits '?' AND consumes the next unit ('A').
        r = encoding_to_utf8(leLone, sizeof(leLone), NULL, &n);
        check_mem("utf16le.lone_high_consumes_next", r, n, "?");
        pluto_free(r);

        // BE: e-acute, euro, check mark.
        r = encoding_to_utf8(beMix, sizeof(beMix), NULL, &n);
        check_mem("utf16be.euro_check", r, n,
                  "\xC3\xA9\xE2\x82\xAC\xE2\x9C\x93");
        pluto_free(r);
    }

    // ---- C. UTF-8 BOM (oracle C) -------------------------------------------
    {
        static const char bomHello[] = {(char)0xEF, (char)0xBB, (char)0xBF,
                                        'h', 'e', 'l', 'l', 'o'};
        r = encoding_to_utf8(bomHello, sizeof(bomHello), NULL, &n);
        check_mem("utf8bom.stripped_hello", r, n, "hello");
        pluto_free(r);
    }

    // ---- D. Header charset probes (oracle D) -------------------------------
    {
        static const char be9[] = {(char)0xE9};
        static const char abc[] = {'a', 'b', 'c'};
        static const char oneA[] = {'A'};

        r = encoding_to_utf8(be9, 1, "text/html; charset=utf-8", &n);
        check_mem("hdr.utf8_verbatim_E9", r, n, "\xE9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=utf_8", &n);
        check_mem("hdr.utf_8_alias_verbatim", r, n, "\xE9");
        pluto_free(r);

        r = encoding_to_utf8(abc, 3, "charset=utf8mb4", &n);
        check_mem("hdr.utf8mb4_passthrough", r, n, "abc");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=ascii", &n);
        check_mem("hdr.ascii_verbatim", r, n, "\xE9");
        pluto_free(r);

        r = encoding_to_utf8(abc, 3, "charset=usascii", &n);
        check_mem("hdr.usascii_passthrough", r, n, "abc");
        pluto_free(r);

        // QUOTED value fails the unanchored pattern -> NULL -> verbatim.
        r = encoding_to_utf8(abc, 3, "text/plain; charset=\"utf-8\"", &n);
        check_mem("hdr.quoted_value_fails_pattern", r, n, "abc");
        pluto_free(r);

        // 'charset =' with no value -> empty capture -> NULL.
        r = encoding_to_utf8(abc, 3, "text/html; charset =", &n);
        check_mem("hdr.missing_value_fails", r, n, "abc");
        pluto_free(r);

        r = encoding_to_utf8(abc, 3, "text/plain", &n);
        check_mem("hdr.no_charset_verbatim", r, n, "abc");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=Windows-1252", &n);
        check_mem("hdr.Windows1252_decodes", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=cp1252", &n);
        check_mem("hdr.cp1252_decodes", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=latin1", &n);
        check_mem("hdr.latin1_decodes", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=ISO-8859-1", &n);
        check_mem("hdr.iso88591_decodes", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=iso8859", &n);
        check_mem("hdr.iso8859_decodes", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=Shift_JIS", &n);
        check_mem("hdr.shiftjis_besteffort", r, n, "\xC3\xA9");
        pluto_free(r);

        r = encoding_to_utf8(be9, 1, "charset=euc-jp", &n);
        check_mem("hdr.eucjp_besteffort", r, n, "\xC3\xA9");
        pluto_free(r);

        // Dispatched UTF-16 on non-BOM data -> empty string.
        r = encoding_to_utf8(oneA, 1, "charset=UTF-16LE", &n);
        check_mem("hdr.utf16le_nonbom_empty", r, n, "");
        pluto_free(r);

        // Bare utf16 (no endianness) is never dispatched -> verbatim.
        r = encoding_to_utf8(oneA, 1, "charset=utf16", &n);
        check_mem("hdr.bare_utf16_not_dispatched", r, n, "A");
        pluto_free(r);
    }

    // ---- E. In-document meta scan (oracle E) --------------------------------
    {
        static const char be9[] = {(char)0xE9};

        memset(body, 0, sizeof(body));
        strcpy(body, "<meta charset=windows-1252><p>");
        memcpy(body + strlen(body), be9, 1);
        r = encoding_to_utf8(body, strlen(body), NULL, &n);
        st_check(r != NULL && n == strlen(body) + 1 &&
                     memcmp(r + strlen(body) - 1, "\xC3\xA9", 2) == 0,
                 "meta.charset_simple_decodes");
        pluto_free(r);

        memset(body, 0, sizeof(body));
        strcpy(body, "<meta http-equiv=\"content-type\" "
                     "content=\"text/html; charset=ISO-8859-1\">x");
        memcpy(body + strlen(body), be9, 1);
        r = encoding_to_utf8(body, strlen(body), NULL, &n);
        st_check(r != NULL && n == strlen(body) + 1 &&
                     memcmp(r + strlen(body) - 1, "\xC3\xA9", 2) == 0,
                 "meta.http_equiv_content_pattern");
        pluto_free(r);

        // Beyond the first 1024 bytes the scan never looks.
        memset(body, 'x', sizeof(body));
        strcpy(body + 1100, "<meta charset=windows-1252>");
        memcpy(body + 1100 + 27, be9, 1);
        r = encoding_to_utf8(body, 1128, NULL, &n);
        st_check(r != NULL && n == 1128 &&
                     (unsigned char)r[1127] == 0xE9,
                 "meta.beyond1024_ignored");
        pluto_free(r);
    }

    // ---- F. Header vs meta precedence (oracle F) ----------------------------
    {
        static const char be9[] = {(char)0xE9};

        // Non-nil non-utf-8 header IGNORES the meta tag entirely.
        memset(body, 0, sizeof(body));
        strcpy(body, "<meta charset=utf-8>z");
        memcpy(body + strlen(body), be9, 1);
        r = encoding_to_utf8(body, strlen(body),
                             "charset=cp1252", &n);
        st_check(r != NULL && n == strlen(body) + 1 &&
                     memcmp(r + strlen(body) - 1, "\xC3\xA9", 2) == 0,
                 "prec.header_cp1252_beats_meta");
        pluto_free(r);

        // utf-8 header lets the meta override.
        memset(body, 0, sizeof(body));
        strcpy(body, "<meta charset=windows-1252>y");
        memcpy(body + strlen(body), be9, 1);
        r = encoding_to_utf8(body, strlen(body),
                             "text/html; charset=utf-8", &n);
        st_check(r != NULL && n == strlen(body) + 1 &&
                     memcmp(r + strlen(body) - 1, "\xC3\xA9", 2) == 0,
                 "prec.header_utf8_allows_meta");
        pluto_free(r);
    }

    // ---- G. Entities: fast path + named (oracle G) ---------------------------
    {
        static const char ctrl[] = {'a', '\x01', '\x02', '\x7F'};

        // No '&', no high byte -> verbatim INCLUDING control chars.
        r = entities_decode(ctrl, sizeof(ctrl), &n);
        check_mem("ent.fastpath_control_verbatim", r, n, "a\x01\x02\x7F");
        pluto_free(r);

        r = entities_decode("&lt;b&gt;", strlen("&lt;b&gt;"), &n);
        check_mem("ent.named_basics", r, n, "<b>");
        pluto_free(r);

        r = entities_decode("&copy;&reg;&trade;", strlen("&copy;&reg;&trade;"),
                            &n);
        check_mem("ent.copy_reg_trade", r, n, "(c)(R)(TM)");
        pluto_free(r);

        r = entities_decode("&deg;&not;", strlen("&deg;&not;"), &n);
        check_mem("ent.deg_not_exact_spacing", r, n, " degnot ");
        pluto_free(r);

        // '%a+' cannot cross digits -> frac12 never matches.
        r = entities_decode("&frac12;", strlen("&frac12;"), &n);
        check_mem("ent.frac12_never_matches", r, n, "&frac12;");
        pluto_free(r);

        r = entities_decode("&foobar;", strlen("&foobar;"), &n);
        check_mem("ent.unknown_name_spaced", r, n, " foobar ");
        pluto_free(r);

        r = entities_decode("&AMP;", strlen("&AMP;"), &n);
        check_mem("ent.case_sensitive", r, n, " AMP ");
        pluto_free(r);

        r = entities_decode("&amp", strlen("&amp"), &n);
        check_mem("ent.missing_semicolon_raw", r, n, "&amp");
        pluto_free(r);
    }

    // ---- H. Numeric entities (oracle H) --------------------------------------
    {
        r = entities_decode("&#65;&#160;&#8211;&#8212;",
                            strlen("&#65;&#160;&#8211;&#8212;"), &n);
        check_mem("dec.basic_nbsp_dashes", r, n, "A  -  -- ");
        pluto_free(r);

        r = entities_decode(
            "&#8216;x&#8217;&#8220;y&#8221;&#8222;&#8230;&#8226;",
            strlen("&#8216;x&#8217;&#8220;y&#8221;&#8222;&#8230;&#8226;"), &n);
        check_mem("dec.quote_family_ellipsis_bullet", r, n,
                  "'x'\"y\"\"...*");
        pluto_free(r);

        r = entities_decode(
            "&#60;&#215;&#247;&#960;&#8722;&#8730;&#8734;&#8804;&#8594;",
            strlen("&#60;&#215;&#247;&#960;&#8722;&#8730;&#8734;"
                   "&#8804;&#8594;"),
            &n);
        check_mem("dec.math_table", r, n, "<x/pi-sqrtinf<=->");
        pluto_free(r);

        r = entities_decode("&#31;&#32;&#126;&#127;",
                            strlen("&#31;&#32;&#126;&#127;"), &n);
        check_mem("dec.range_edges", r, n, "  ~ ");
        pluto_free(r);

        r = entities_decode("&#99999999999999999999;",
                            strlen("&#99999999999999999999;"), &n);
        check_mem("dec.huge_number_space", r, n, " ");
        pluto_free(r);

        // Hex branch: fewer specials than decimal (no 0x201A!).
        r = entities_decode("&#x41;&#xa0;&#x2013;",
                            strlen("&#x41;&#xa0;&#x2013;"), &n);
        check_mem("hex.basic_a0_dash", r, n, "A  - ");
        pluto_free(r);

        r = entities_decode("&#x202f;&#x2009;",
                            strlen("&#x202f;&#x2009;"), &n);
        check_mem("hex.thin_spaces", r, n, "  ");
        pluto_free(r);

        r = entities_decode("&#x201a;", strlen("&#x201a;"), &n);
        check_mem("hex.lacks_201a_asymmetry", r, n, " ");
        pluto_free(r);

        r = entities_decode("&#8218;", strlen("&#8218;"), &n);
        check_mem("dec.has_201a_quote", r, n, "'");
        pluto_free(r);
    }

    // ---- I. High-byte cleanup pipeline (oracle I) -----------------------------
    {
        r = entities_decode("caf\xC3\xA9 \xC3\x9F \xC3\xA6 \xC3\x87 "
                            "\xC3\xB1 \xC3\xB8",
                            strlen("caf\xC3\xA9 \xC3\x9F \xC3\xA6 \xC3\x87 "
                                   "\xC3\xB1 \xC3\xB8"),
                            &n);
        check_mem("clean.translit_accents", r, n, "cafe ss ae C n o");
        pluto_free(r);

        r = entities_decode("\xC5\x92 thorn \xC3\xBE",
                            strlen("\xC5\x92 thorn \xC3\xBE"), &n);
        check_mem("clean.OE_thorn", r, n, "OE thorn th");
        pluto_free(r);

        r = entities_decode("\xCB\x86", 2, &n);
        check_mem("clean.caron_unmapped_space", r, n, " ");
        pluto_free(r);

        r = entities_decode("a\xE2\x82\xAC" "b", 5, &n);
        check_mem("clean.euro_3byte_single_space", r, n, "a b");
        pluto_free(r);

        r = entities_decode("a\xF0\x9F\x98\x80" "b", 6, &n);
        check_mem("clean.emoji_widens_per_byte", r, n, "a    b");
        pluto_free(r);

        r = entities_decode("a\xC3", 2, &n);
        check_mem("clean.truncated_tail_single_space", r, n, "a ");
        pluto_free(r);

        r = entities_decode("a\xC2\xA0" "b", 4, &n);
        check_mem("clean.nbsp_fixed_seq", r, n, "a b");
        pluto_free(r);

        r = entities_decode("x\xE2\x80\x94" "y", 5, &n);
        check_mem("clean.emdash_fixed_seq", r, n, "x -- y");
        pluto_free(r);

        // Full pipeline: named+decimal passes, fixed seqs, transliteration.
        {
            const char* mixed =
                "caf\xC3\xA9 \xE2\x80\x94 r\xC3\xA9sum\xC3\xA9 "
                "&amp;&nbsp;&#33;";
            r = entities_decode(mixed, strlen(mixed), &n);
            check_mem("clean.full_pipeline_mixed", r, n,
                      "cafe  --  resume & !");
            pluto_free(r);
        }

        // Amp present but no high byte -> passes run, cleanup skipped:
        // stray control byte survives verbatim after the '&' is eaten.
        r = entities_decode("&\x01", 2, &n);
        check_mem("clean.high_absent_keeps_control", r, n, "&\x01");
        pluto_free(r);
    }

    // ---- J. Encode (oracle J) --------------------------------------------------
    {
        r = entities_encode("a&b<c>d\"e'f", strlen("a&b<c>d\"e'f"), &n);
        check_mem("encode.all_but_apostrophe", r, n,
                  "a&amp;b&lt;c&gt;d&quot;e'f");
        pluto_free(r);

        r = entities_encode(NULL, 0, &n);
        check_mem("encode.null_empty", r, n, "");
        pluto_free(r);
    }

    // ---- K. Decode inside a task under frozen clock (oracle task probe) -------
    {
        EncJob job;
        memset(&job, 0, sizeof(job));
        g_now = 12345; // frozen for the whole frame -> gate can never fire
        tasks_run(step_entity_job, &job, NULL, NULL, NULL, NULL);
        tasks_update();
        st_check(job.ran == 1 && !tasks_is_running(),
                 "task.decode_completes_one_frame");
        st_check(job.outLen == 4000, "task.decode_outlen");
        st_check(strcmp(job.first, "&<&<&<") == 0, "task.decode_output");
    }

    // ---- L. normalize_charset direct API ---------------------------------------
    {
        char* s = encoding_normalize_charset("Windows-1252");
        st_check(s != NULL && strcmp(s, "cp1252") == 0,
                 "norm.windows1252_alias");
        pluto_free(s);

        s = encoding_normalize_charset("UTF_8!!!");
        st_check(s != NULL && strcmp(s, "utf-8") == 0, "norm.utf8_alias");
        pluto_free(s);

        s = encoding_normalize_charset(NULL);
        st_check(s == NULL, "norm.null_safe");
    }

    // ---- M. Degenerate inputs ----------------------------------------------------
    {
        r = encoding_to_utf8(NULL, 0, NULL, &n);
        check_mem("degen.null_body_empty", r, n, "");
        pluto_free(r);

        r = entities_decode("", 0, &n);
        check_mem("degen.decode_empty", r, n, "");
        pluto_free(r);
    }

    PLUTO_LOG("[P06] encoding selftests done: %d passed, %d failed",
              st_pass, st_fail);
    if (outPass != NULL) {
        *outPass = st_pass;
    }
    if (outFail != NULL) {
        *outFail = st_fail;
    }
}
