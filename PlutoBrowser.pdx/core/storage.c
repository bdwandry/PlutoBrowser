// storage.c — persistent Storage (see header).
//
// Parity notes vs core/storage.lua:
//  - init: missing/empty bookmarks -> DEFAULT_BOOKMARKS; history/cookies
//    adopted verbatim when present; settings merged per-key.
//  - addHistory: skips NULL/""/"^about:" URLs, dedups by exact URL scanning
//    from the END (first match removed), inserts at front, caps at 50 by
//    dropping from the tail, auto-saves.
//  - addBookmark: duplicate URL updates TITLE ONLY (desc untouched).
//  - removeBookmark takes a 1-based index; out-of-range -> 0, no change.

#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "../util/dynarray.h"
#include "../util/json.h"
#include "../util/luapattern.h"
#include "../util/mem.h"
#include "../util/strbuf.h"
#include "constants.h"
#include "logger.h"

// Lua: playdate.datastore.write/read(data, "comet_browser_data.json").
// Paths are relative to the game's data directory on both sim and device.
#define STORAGE_PATH "comet_browser_data.json"

static struct PlaydateAPI* s_pd = NULL;
static DynArray s_bookmarks; // PlutoBookmark
static DynArray s_history;   // PlutoHistoryItem
static DynArray s_cookies;   // PlutoCookie
static PlutoSettings s_settings;

static void copy_field(char* dst, size_t dstSz, const char* src)
{
    size_t n;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstSz) {
        n = dstSz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void da_insert_front(DynArray* a, const void* elem)
{
    if (da_push(a, elem) == NULL) {
        return;
    }
    // shift [0..n-2] up one slot, place new element at 0
    memmove((char*)a->items + a->elemsize, a->items,
            (a->count - 1) * a->elemsize);
    memcpy(a->items, elem, a->elemsize);
}

// ------------------------------------------------------------ defaults ----

static void install_default_bookmarks(void)
{
    size_t i;
    s_bookmarks.count = 0;
    for (i = 0; i < PLUTO_DEFAULT_BOOKMARK_COUNT; i++) {
        PlutoSavedBookmark bm;
        memset(&bm, 0, sizeof(bm));
        copy_field(bm.title, sizeof(bm.title), PLUTO_DEFAULT_BOOKMARKS[i].title);
        copy_field(bm.url, sizeof(bm.url), PLUTO_DEFAULT_BOOKMARKS[i].url);
        copy_field(bm.desc, sizeof(bm.desc), PLUTO_DEFAULT_BOOKMARKS[i].desc);
        da_push(&s_bookmarks, &bm);
    }
}

void storage_reset_defaults(void)
{
    s_history.count = 0;
    s_cookies.count = 0;
    install_default_bookmarks();

    s_settings.searchEngine = 1;
    s_settings.mode = PLUTO_MODE_RAW_HTML;      // Constants.MODE_RAW_HTML
    s_settings.autoReader = 0;
    copy_field(s_settings.fontSize, sizeof(s_settings.fontSize), "medium");
    s_settings.imageMode = PLUTO_IMAGE_MODE_VIEWPORT;
    s_settings.invertCrank = 0;
}

// ------------------------------------------------------- mode mappings ----

static const char* mode_to_str(int mode)
{
    switch ((PlutoMode)mode) {
        case PLUTO_MODE_READER:   return "reader";
        case PLUTO_MODE_RAW_HTML: return "html";
        case PLUTO_MODE_OPERA_DS: return "ds";
        default:                  return "html";
    }
}

static int mode_from_str(const char* s)
{
    if (strcmp(s, "reader") == 0) return PLUTO_MODE_READER;
    if (strcmp(s, "html") == 0)   return PLUTO_MODE_RAW_HTML;
    if (strcmp(s, "ds") == 0)     return PLUTO_MODE_OPERA_DS;
    return -1;
}

static const char* image_mode_to_str(int mode)
{
    switch ((PlutoImageMode)mode) {
        case PLUTO_IMAGE_MODE_ALL:      return "all";
        case PLUTO_IMAGE_MODE_VIEWPORT: return "viewport";
        case PLUTO_IMAGE_MODE_ONDEMAND: return "ondemand";
        case PLUTO_IMAGE_MODE_HOVER:    return "hover";
        case PLUTO_IMAGE_MODE_DISABLED: return "disabled";
        default:                        return "viewport";
    }
}

static int image_mode_from_str(const char* s)
{
    if (strcmp(s, "all") == 0)      return PLUTO_IMAGE_MODE_ALL;
    if (strcmp(s, "viewport") == 0) return PLUTO_IMAGE_MODE_VIEWPORT;
    if (strcmp(s, "ondemand") == 0) return PLUTO_IMAGE_MODE_ONDEMAND;
    if (strcmp(s, "hover") == 0)    return PLUTO_IMAGE_MODE_HOVER;
    if (strcmp(s, "disabled") == 0) return PLUTO_IMAGE_MODE_DISABLED;
    return -1;
}

// -------------------------------------------------------------- saving ----

int storage_save(void)
{
    JsonValue *root, *arr, *obj;
    StrBuf out;
    SDFile* f;
    size_t i;
    int ok = 1;

    if (s_pd == NULL) {
        return 0;
    }

    root = json_new_object();

    arr = json_new_array();
    for (i = 0; i < s_bookmarks.count && arr; i++) {
        PlutoSavedBookmark* bm = (PlutoSavedBookmark*)da_get(&s_bookmarks, i);
        obj = json_new_object();
        json_obj_set(obj, "title", json_new_string(bm->title));
        json_obj_set(obj, "url", json_new_string(bm->url));
        json_obj_set(obj, "desc", json_new_string(bm->desc));
        json_arr_append(arr, obj);
    }
    json_obj_set(root, "bookmarks", arr);

    arr = json_new_array();
    for (i = 0; i < s_history.count && arr; i++) {
        PlutoHistoryItem* h = (PlutoHistoryItem*)da_get(&s_history, i);
        obj = json_new_object();
        json_obj_set(obj, "title", json_new_string(h->title));
        json_obj_set(obj, "url", json_new_string(h->url));
        json_obj_set(obj, "time", json_new_string(h->time));
        json_arr_append(arr, obj);
    }
    json_obj_set(root, "history", arr);

    arr = json_new_array();
    for (i = 0; i < s_cookies.count && arr; i++) {
        PlutoCookie* c = (PlutoCookie*)da_get(&s_cookies, i);
        obj = json_new_object();
        json_obj_set(obj, "name", json_new_string(c->name));
        json_obj_set(obj, "value", json_new_string(c->value));
        json_obj_set(obj, "domain", json_new_string(c->domain));
        json_obj_set(obj, "hostOnly", json_new_bool(c->hostOnly));
        json_obj_set(obj, "path", json_new_string(c->path));
        json_obj_set(obj, "secure", json_new_bool(c->secure));
        json_obj_set(obj, "httpOnly", json_new_bool(c->httpOnly));
        if (c->samesite[0] != '\0') {
            json_obj_set(obj, "samesite", json_new_string(c->samesite));
        }
        if (c->hasExpires) {
            json_obj_set(obj, "expires", json_new_number(c->expires));
        }
        json_arr_append(arr, obj);
    }
    json_obj_set(root, "cookies", arr);

    obj = json_new_object();
    json_obj_set(obj, "searchEngine", json_new_number(s_settings.searchEngine));
    json_obj_set(obj, "mode", json_new_string(mode_to_str(s_settings.mode)));
    json_obj_set(obj, "autoReader", json_new_bool(s_settings.autoReader));
    json_obj_set(obj, "fontSize",
                 json_new_string(s_settings.fontSize));
    json_obj_set(obj, "imageMode",
                 json_new_string(image_mode_to_str(s_settings.imageMode)));
    json_obj_set(obj, "invertCrank", json_new_bool(s_settings.invertCrank));
    json_obj_set(root, "settings", obj);

    sb_init(&out);
    if (!json_write(root, &out)) {
        ok = 0;
    }
    json_free(root);

    if (ok) {
        f = s_pd->file->open(STORAGE_PATH, kFileWrite);
        if (f == NULL) {
            PLUTO_ERROR("storage: open(%s) failed: %s", STORAGE_PATH,
                        s_pd->file->geterr());
            ok = 0;
        } else {
            if (out.len > 0 &&
                s_pd->file->write(f, out.data, (unsigned)out.len) < 0) {
                PLUTO_ERROR("storage: write failed: %s",
                            s_pd->file->geterr());
                ok = 0;
            }
            s_pd->file->close(f);
        }
    }
    sb_free(&out);
    return ok;
}

// -------------------------------------------------------------- loading ----

static void map_bookmark(const JsonValue* o, PlutoSavedBookmark* bm)
{
    const JsonValue* v;
    memset(bm, 0, sizeof(*bm));
    v = json_obj_get(o, "title");
    if (v && json_str(v, NULL)) copy_field(bm->title, sizeof(bm->title), json_str(v, NULL));
    v = json_obj_get(o, "url");
    if (v && json_str(v, NULL)) copy_field(bm->url, sizeof(bm->url), json_str(v, NULL));
    v = json_obj_get(o, "desc");
    if (v && json_str(v, NULL)) copy_field(bm->desc, sizeof(bm->desc), json_str(v, NULL));
}

static void map_history(const JsonValue* o, PlutoHistoryItem* h)
{
    const JsonValue* v;
    memset(h, 0, sizeof(*h));
    v = json_obj_get(o, "title");
    if (v && json_str(v, NULL)) copy_field(h->title, sizeof(h->title), json_str(v, NULL));
    v = json_obj_get(o, "url");
    if (v && json_str(v, NULL)) copy_field(h->url, sizeof(h->url), json_str(v, NULL));
    v = json_obj_get(o, "time");
    if (v && json_str(v, NULL)) copy_field(h->time, sizeof(h->time), json_str(v, NULL));
}

static void map_cookie(const JsonValue* o, PlutoCookie* c)
{
    const JsonValue* v;
    size_t n;
    memset(c, 0, sizeof(*c));
    copy_field(c->path, sizeof(c->path), "/"); // default
    c->hostOnly = 1;

    v = json_obj_get(o, "name");
    if (v && json_str(v, NULL)) copy_field(c->name, sizeof(c->name), json_str(v, NULL));
    v = json_obj_get(o, "value");
    if (v && json_str(v, NULL)) copy_field(c->value, sizeof(c->value), json_str(v, NULL));
    v = json_obj_get(o, "domain");
    if (v && json_str(v, NULL)) copy_field(c->domain, sizeof(c->domain), json_str(v, NULL));
    v = json_obj_get(o, "path");
    if (v && json_str(v, NULL) && json_str(v, NULL)[0] != '\0')
        copy_field(c->path, sizeof(c->path), json_str(v, NULL));
    v = json_obj_get(o, "hostOnly");
    c->hostOnly = json_bool_val(v, 1);
    v = json_obj_get(o, "secure");
    c->secure = json_bool_val(v, 0);
    v = json_obj_get(o, "httpOnly");
    c->httpOnly = json_bool_val(v, 0);
    v = json_obj_get(o, "samesite");
    if (v && json_str(v, &n) && n > 0 && n < sizeof(c->samesite))
        copy_field(c->samesite, sizeof(c->samesite), json_str(v, NULL));
    v = json_obj_get(o, "expires");
    if (v != NULL && !json_is_null(v)) {
        c->expires = json_num(v, 0.0);
        c->hasExpires = 1;
    }
}

int storage_load(void)
{
    FileStat st;
    SDFile* f;
    char* buf;
    JsonValue* root;
    char err[128];
    int got = 0;

    if (s_pd == NULL) {
        return 0;
    }
    if (s_pd->file->stat(STORAGE_PATH, &st) != 0 || st.size == 0) {
        return 0;
    }
    f = s_pd->file->open(STORAGE_PATH, kFileRead | kFileReadData);
    if (f == NULL) {
        return 0;
    }
    buf = (char*)pluto_malloc((size_t)st.size + 1);
    if (buf == NULL) {
        s_pd->file->close(f);
        return 0;
    }
    {
        unsigned total = 0;
        while (total < (unsigned)st.size) {
            int n = s_pd->file->read(f, buf + total, (unsigned)st.size - total);
            if (n <= 0) {
                break;
            }
            total += (unsigned)n;
        }
        buf[total] = '\0';
    }
    s_pd->file->close(f);

    root = json_parse(buf, strlen(buf), err);
    pluto_free(buf);
    if (root == NULL) {
        PLUTO_ERROR("storage: parse failed (%s)", err);
        return 0;
    }

    got = 1;
    {
        const JsonValue* arr = json_obj_get(root, "bookmarks");
        if (arr != NULL && json_arr_count(arr) > 0) {
            size_t i;
            s_bookmarks.count = 0;
            for (i = 0; i < json_arr_count(arr); i++) {
                PlutoSavedBookmark bm;
                map_bookmark(json_arr_get(arr, i), &bm);
                da_push(&s_bookmarks, &bm);
            }
        } else {
            // missing OR empty -> defaults (Lua "#saved.bookmarks > 0")
            install_default_bookmarks();
        }
    }
    {
        const JsonValue* arr = json_obj_get(root, "history");
        if (arr != NULL) {
            size_t i;
            s_history.count = 0;
            for (i = 0; i < json_arr_count(arr); i++) {
                PlutoHistoryItem h;
                map_history(json_arr_get(arr, i), &h);
                da_push(&s_history, &h);
            }
        }
    }
    {
        const JsonValue* arr = json_obj_get(root, "cookies");
        if (arr != NULL) {
            size_t i;
            s_cookies.count = 0;
            for (i = 0; i < json_arr_count(arr); i++) {
                PlutoCookie c;
                map_cookie(json_arr_get(arr, i), &c);
                if (c.name[0] == '\0' || c.domain[0] == '\0') {
                    continue; // malformed; drop (prune would remove anyway)
                }
                da_push(&s_cookies, &c);
            }
        }
    }
    {
        const JsonValue* set = json_obj_get(root, "settings");
        const JsonValue* v;
        if (set != NULL) {
            double num;
            v = json_obj_get(set, "searchEngine");
            if (v != NULL && !json_is_null(v)) {
                num = json_num(v, 1.0);
                if (num >= 1.0 && num <= 4.0) {
                    s_settings.searchEngine = (int)num;
                }
            }
            v = json_obj_get(set, "mode");
            if (v != NULL && json_str(v, NULL) != NULL) {
                int m = mode_from_str(json_str(v, NULL));
                if (m >= 0) s_settings.mode = m;
            }
            v = json_obj_get(set, "autoReader");
            if (v != NULL && !json_is_null(v))
                s_settings.autoReader = json_bool_val(v, 0);
            v = json_obj_get(set, "fontSize");
            if (v != NULL && json_str(v, NULL) != NULL)
                copy_field(s_settings.fontSize, sizeof(s_settings.fontSize),
                           json_str(v, NULL));
            v = json_obj_get(set, "imageMode");
            if (v != NULL && json_str(v, NULL) != NULL) {
                int m = image_mode_from_str(json_str(v, NULL));
                if (m >= 0) s_settings.imageMode = m;
            }
            v = json_obj_get(set, "invertCrank");
            if (v != NULL && !json_is_null(v))
                s_settings.invertCrank = json_bool_val(v, 0);
        }
    }

    json_free(root);
    return got;
}

// ---------------------------------------------------------------- clock ----

void storage_format_now(char* out, size_t outSz)
{
    if (s_pd != NULL) {
        unsigned int ms = 0;
        uint32_t epoch = s_pd->system->getSecondsSinceEpoch(&ms);
        struct PDDateTime dt;
        s_pd->system->convertEpochToDateTime(epoch, &dt);
        snprintf(out, outSz, "%02d:%02d", (int)dt.hour, (int)dt.minute);
        out[outSz - 1] = '\0';
    } else {
        // playdate.getTime() unavailable -> Lua pcall fallback
        copy_field(out, outSz, "Recent");
    }
}

// ------------------------------------------------------------ collection ops ----

void storage_add_history(const char* title, const char* url)
{
    PlutoHistoryItem h;
    size_t i;
    size_t mStart, mEnd;

    if (url == NULL || url[0] == '\0' ||
        lp_find(url, strlen(url), "^about:", 7, 0, &mStart, &mEnd) == 1) {
        return;
    }
    if (title == NULL || title[0] == '\0') {
        title = url; // Lua: title = title or url
    }

    // Dedup: scan from END, remove first match (Lua parity).
    for (i = s_history.count; i-- > 0;) {
        PlutoHistoryItem* e = (PlutoHistoryItem*)da_get(&s_history, i);
        if (e != NULL && strcmp(e->url, url) == 0) {
            da_remove_at(&s_history, i);
            break;
        }
    }

    memset(&h, 0, sizeof(h));
    copy_field(h.title, sizeof(h.title), title);
    copy_field(h.url, sizeof(h.url), url);
    storage_format_now(h.time, sizeof(h.time));

    da_insert_front(&s_history, &h);

    while (s_history.count > PLUTO_HISTORY_CAP) {
        da_remove_at(&s_history, s_history.count - 1); // drop from tail
    }

    storage_save();
}

int storage_add_bookmark(const char* title, const char* url, const char* desc)
{
    PlutoSavedBookmark bm;
    size_t i;

    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    if (title == NULL || title[0] == '\0') {
        title = url;
    }
    if (desc == NULL) {
        desc = "";
    }

    for (i = 0; i < s_bookmarks.count; i++) {
        PlutoSavedBookmark* b = (PlutoSavedBookmark*)da_get(&s_bookmarks, i);
        if (b != NULL && strcmp(b->url, url) == 0) {
            copy_field(b->title, sizeof(b->title), title); // title only!
            storage_save();
            return 1;
        }
    }

    memset(&bm, 0, sizeof(bm));
    copy_field(bm.title, sizeof(bm.title), title);
    copy_field(bm.url, sizeof(bm.url), url);
    copy_field(bm.desc, sizeof(bm.desc), desc);
    da_push(&s_bookmarks, &bm);
    storage_save();
    return 1;
}

int storage_remove_bookmark(size_t index1based)
{
    if (index1based >= 1 && index1based <= s_bookmarks.count) {
        da_remove_at(&s_bookmarks, index1based - 1);
        storage_save();
        return 1;
    }
    return 0;
}

int storage_is_bookmarked(const char* url)
{
    size_t i;
    if (url == NULL) {
        return 0;
    }
    for (i = 0; i < s_bookmarks.count; i++) {
        PlutoSavedBookmark* b = (PlutoSavedBookmark*)da_get(&s_bookmarks, i);
        if (b != NULL && strcmp(b->url, url) == 0) {
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------- accessors ----

DynArray* storage_bookmarks(void) { return &s_bookmarks; }
DynArray* storage_history(void) { return &s_history; }
DynArray* storage_cookies(void) { return &s_cookies; }
PlutoSettings* storage_settings(void) { return &s_settings; }

void storage_init(struct PlaydateAPI* pd)
{
    s_pd = pd;
    da_init(&s_bookmarks, sizeof(PlutoSavedBookmark));
    da_init(&s_history, sizeof(PlutoHistoryItem));
    da_init(&s_cookies, sizeof(PlutoCookie));
    memset(&s_settings, 0, sizeof(s_settings));

    if (!storage_load()) {
        storage_reset_defaults();
        storage_save(); // Lua else-branch parity
    }
}
