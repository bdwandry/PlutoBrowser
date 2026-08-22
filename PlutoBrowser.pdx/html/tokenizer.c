#include "html/tokenizer.h"
#include "html/entities.h"
#include "core/tasks.h"
#include "util/mem.h"
#include <ctype.h>
#include <string.h>

#define HTT_MAX_HTML_SIZE 262144

static int htt_key_char(int c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':';
}

static int htt_unquoted_val_char(int c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' ||
           c == '.' || c == '/' || c == '?' || c == '#';
}

static void htt_put_attr(StrMap* attrs, const char* lkey, char* decoded) {
    if (decoded == NULL) return;
    if (sm_get(attrs, lkey) != NULL) {
        pluto_free(decoded);
        return;
    }
    sm_put(attrs, lkey, decoded);
}

static StrMap* htt_parse_attrs(const char* s, size_t n) {
    StrMap* attrs = sm_create(8);
    if (attrs == NULL) return NULL;
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        while (st < n && isspace((unsigned char)s[st])) st++;
        if (st >= n) break;
        i = st;
        size_t ke = st;
        while (ke < n && htt_key_char((unsigned char)s[ke])) ke++;
        if (ke == st) { i = st + 1; continue; }
        size_t keyLen = ke - st;
        char* key = pluto_strndup(s + st, keyLen);
        if (key == NULL) break;
        for (char* p = key; *p; p++) *p = (char)tolower((unsigned char)*p);

        size_t j = ke;
        while (j < n && isspace((unsigned char)s[j])) j++;
        int hasEq = (j < n && s[j] == '=');
        if (hasEq) {
            size_t vs = ke + 1;
            while (vs < n && isspace((unsigned char)s[vs])) vs++;
            if (vs >= n) {
                htt_put_attr(attrs, key, pluto_strdup(""));
                pluto_free(key);
                break;
            }
            char c = s[vs];
            if (c == '"' || c == '\'') {
                const char* q0 = s + vs + 1;
                const char* qe = memchr(q0, c, n - (vs + 1));
                if (qe == NULL) {
                    pluto_free(key);
                    break;
                }
                size_t vLen = (size_t)(qe - q0);
                char* raw = pluto_strndup(q0, vLen);
                size_t dLen = 0;
                char* dec = raw ? entities_decode(raw, vLen, &dLen) : NULL;
                pluto_free(raw);
                htt_put_attr(attrs, key, dec);
                i = (size_t)(qe - s) + 1;
            } else {
                size_t ve = vs;
                while (ve < n && htt_unquoted_val_char((unsigned char)s[ve])) ve++;
                size_t vLen = ve - vs;
                char* raw = pluto_strndup(s + vs, vLen);
                size_t dLen = 0;
                char* dec = raw ? entities_decode(raw, vLen, &dLen) : NULL;
                pluto_free(raw);
                htt_put_attr(attrs, key, dec);
                i = ve;
            }
        } else {
            if (sm_get(attrs, key) == NULL)
                sm_put(attrs, key, HT_ATTR_TRUE);
            i = ke;
        }
        pluto_free(key);
    }
    return attrs;
}

static long htt_find_tag_end(const char* s, size_t n, size_t startIdx) {
    size_t i = startIdx;
    while (i < n) {
        const char* m = memchr(s + i, '>', n - i);
        const char* dq = memchr(s + i, '"', n - i);
        const char* sq = memchr(s + i, '\'', n - i);
        const char* hit = m;
        if (hit == NULL || (dq != NULL && dq < hit)) hit = dq;
        if (hit == NULL || (sq != NULL && sq < hit)) hit = sq;
        if (hit == NULL) return -1;
        char b = *hit;
        if (b == '>') return (long)(hit - s);
        const char* close = memchr(hit + 1, b, n - (size_t)(hit - s) - 1);
        if (close == NULL) return -1;
        i = (size_t)(close - s) + 1;
    }
    return -1;
}

static const char* htt_find_ci(const char* s, size_t n, size_t from,
                               const char* lit, size_t litLen) {
    if (litLen == 0 || n < litLen) return NULL;
    for (size_t i = from; i + litLen <= n; i++) {
        if (strncasecmp(s + i, lit, litLen) == 0) return s + i;
    }
    return NULL;
}

static char* htt_collapse_ws(const char* s, size_t n) {
    char* out = pluto_malloc(n + 1);
    if (out == NULL) return NULL;
    size_t o = 0, i = 0;
    while (i < n) {
        if (isspace((unsigned char)s[i])) {
            out[o++] = ' ';
            while (i < n && isspace((unsigned char)s[i])) i++;
        } else {
            out[o++] = s[i++];
        }
    }
    out[o] = '\0';
    return out;
}

static void htt_trim(const char** p, size_t* n) {
    const char* s = *p;
    size_t len = *n;
    size_t a = 0, b = len;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    *p = s + a;
    *n = b - a;
}

static int htt_push_token(HttTokens* tks, HttToken tok) {
    if (tks->count == tks->cap) {
        size_t ncap = tks->cap ? tks->cap * 2 : 64;
        HttToken* ni = pluto_realloc(tks->items, ncap * sizeof(HttToken));
        if (ni == NULL) return 0;
        tks->items = ni;
        tks->cap = ncap;
    }
    tks->items[tks->count++] = tok;
    return 1;
}

static int htt_push_text(HttTokens* tks, const char* s, size_t n) {
    if (n == 0) return 1;
    size_t dLen = 0;
    char* dec = entities_decode(s, n, &dLen);
    if (dec == NULL) return 0;
    HttToken tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = HTT_TEXT;
    tok.text = dec;
    tok.textLen = dLen;
    if (!htt_push_token(tks, tok)) {
        pluto_free(dec);
        return 0;
    }
    return 1;
}

HttTokens* htt_tokenize(const char* html, size_t len) {
    HttTokens* tks = pluto_malloc(sizeof(HttTokens));
    if (tks == NULL) return NULL;
    memset(tks, 0, sizeof(*tks));
    tks->pageTitle = pluto_strdup("Web Page");
    if (tks->pageTitle == NULL) {
        pluto_free(tks);
        return NULL;
    }
    if (html == NULL || len == 0) return tks;

    size_t workLen = len;
    if (len > HTT_MAX_HTML_SIZE) {
        size_t winStart = HTT_MAX_HTML_SIZE > 128 ? HTT_MAX_HTML_SIZE - 128 : 0;
        const char* gt = memchr(html + winStart, '>', len - winStart);
        workLen = gt ? (size_t)(gt - html) + 1 : HTT_MAX_HTML_SIZE;
    }

    size_t pos = 0;
    while (pos < workLen) {
        if (tasks_yield_check()) break;
        tasks_report_progress(0.5 * ((double)(pos + 1) / (double)workLen));

        const char* lt = memchr(html + pos, '<', workLen - pos);
        if (lt == NULL) {
            if (!htt_push_text(tks, html + pos, workLen - pos)) goto fail;
            break;
        }
        size_t tagStart = (size_t)(lt - html);
        if (tagStart > pos) {
            if (!htt_push_text(tks, html + pos, tagStart - pos)) goto fail;
        }

        long te = htt_find_tag_end(html, workLen, tagStart + 1);
        if (te < 0) break;
        size_t tagEnd = (size_t)te;

        const char* rinP = html + tagStart + 1;
        size_t rinN = tagEnd - (tagStart + 1);
        htt_trim(&rinP, &rinN);

        char head[9];
        size_t hn = rinN < 8 ? rinN : 8;
        for (size_t k = 0; k < hn; k++)
            head[k] = (char)tolower((unsigned char)rinP[k]);
        head[hn] = '\0';

        if (rinN >= 3 && memcmp(rinP, "!--", 3) == 0) {
            const char* ce = memchr(html + tagStart, '-', workLen - tagStart);
            const char* found = NULL;
            for (const char* q = ce; q && q + 2 <= html + workLen; ) {
                if (q[0] == '-' && q[1] == '-' && q[2] == '>') { found = q; break; }
                const char* nx = memchr(q + 1, '-', (size_t)(html + workLen - (q + 1)));
                q = nx;
            }
            pos = found ? (size_t)(found - html) + 3 : tagEnd + 1;
        } else if (hn >= 6 && strncasecmp(head, "script", 6) == 0) {
            const char* sc = htt_find_ci(html, workLen, tagEnd, "</script>", 9);
            if (sc != NULL) {
                const char* gt = memchr(sc, '>', workLen - (size_t)(sc - html));
                pos = (gt ? (size_t)(gt - html) : (size_t)(sc - html)) + 1;
            } else {
                pos = workLen;
            }
        } else if (hn >= 5 && strncasecmp(head, "style", 5) == 0) {
            const char* sc = htt_find_ci(html, workLen, tagEnd, "</style>", 8);
            if (sc != NULL) {
                const char* gt = memchr(sc, '>', workLen - (size_t)(sc - html));
                pos = (gt ? (size_t)(gt - html) : (size_t)(sc - html)) + 1;
            } else {
                pos = workLen;
            }
        } else if (hn >= 5 && strncasecmp(head, "title", 5) == 0) {
            const char* tc = htt_find_ci(html, workLen, tagEnd, "</title>", 8);
            if (tc != NULL) {
                const char* tp = html + tagEnd + 1;
                size_t tn = (size_t)(tc - tp);
                char* collapsed = htt_collapse_ws(tp, tn);
                if (collapsed != NULL) {
                    const char* cp = collapsed;
                    size_t cn = strlen(collapsed);
                    htt_trim(&cp, &cn);
                    size_t dLen = 0;
                    char* dec = entities_decode(cp, cn, &dLen);
                    pluto_free(collapsed);
                    if (dec != NULL) {
                        pluto_free(tks->pageTitle);
                        tks->pageTitle = dec;
                    }
                }
                pos = (size_t)(tc - html) + 8;
            } else {
                pos = tagEnd + 1;
            }
        } else {
            int isClosing = rinN > 0 && rinP[0] == '/';
            const char* bodyP = isClosing ? rinP + 1 : rinP;
            size_t bodyN = isClosing ? rinN - 1 : rinN;
            htt_trim(&bodyP, &bodyN);

            int endsSlash = bodyN > 0 && bodyP[bodyN - 1] == '/';
            int isSelfClosing = endsSlash || isClosing;
            if (endsSlash) bodyN--;

            size_t tn = 0;
            while (tn < bodyN && htt_key_char((unsigned char)bodyP[tn])) tn++;
            if (tn > 0) {
                char* name = pluto_strndup(bodyP, tn);
                if (name == NULL) goto fail;
                for (char* p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
                StrMap* attrs = htt_parse_attrs(bodyP + tn, bodyN - tn);
                if (attrs == NULL) {
                    pluto_free(name);
                    goto fail;
                }
                HttToken tok;
                memset(&tok, 0, sizeof(tok));
                tok.type = HTT_TAG;
                tok.name = name;
                tok.isClosing = isClosing;
                tok.isSelfClosing = isSelfClosing;
                tok.attrs = attrs;
                if (!htt_push_token(tks, tok)) {
                    sm_destroy(attrs);
                    pluto_free(name);
                    goto fail;
                }
            }
            pos = tagEnd + 1;
        }
    }
    return tks;

fail:
    htt_free(tks);
    return NULL;
}

void htt_free(HttTokens* tks) {
    if (tks == NULL) return;
    for (size_t i = 0; i < tks->count; i++) {
        HttToken* t = &tks->items[i];
        if (t->type == HTT_TEXT) {
            pluto_free(t->text);
        } else {
            pluto_free(t->name);
            if (t->attrs != NULL) {
                sm_destroy(t->attrs);
            }
        }
    }
    pluto_free(tks->items);
    pluto_free(tks->pageTitle);
    pluto_free(tks);
}

void htt_init(struct PlaydateAPI* pd) {
    (void)pd;
}
