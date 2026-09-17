/*
 * PlutoBrowser — storage.c
 * Persistent storage (port of Source/core/storage.lua). See storage.h.
 * Datastore is Lua-only, so persistence is a custom sectioned text file
 * written through pd->file with the exact same observable semantics:
 * defaults on first load (and save), history dedupe-to-top with 50 cap,
 * bookmark dedupe→title-update, remove-by-index, save after every mutation.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "core/storage.h"
#include "core/constants.h"
#include "core/logger.h"
#include "core/cookie_jar.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);
extern void pluto_free(void *p);

#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_pd()->system->realloc((p), 0)

#define DATA_FILENAME "comet_browser_data"

/* ── in-memory state (mirrors Storage.* tables) ──────────────────────────── */

static StoredBookmark *g_bookmarks[64];
static int g_bookmarkCount = 0;
static StoredHistoryItem *g_history[HISTORY_MAX];
static int g_historyCount = 0;

typedef struct
{
    char key[32];
    char sval[128];
    int ival;
    int isInt;
} Setting;

static Setting g_settings[16] = {
    {"searchEngine", "", 1, 1},
    {"mode", "", 1, 1},       /* Constants.MODE_RAW_HTML = "html" (Lua default) */
    {"autoReader", "", 0, 1}, /* false */
    {"fontSize", "medium", 0, 0},
    {"imageMode", "viewport", 0, 0}, /* string name; Lua stores Constants names */
    {"invertCrank", "", 0, 1}, /* false */
    {"showFps", "", 0, 1}, /* false — FPS overlay off by default */
    {"displayFps", "", 30, 1}, /* display refresh target: 30 or 50 fps (Playdate max) */
    {"jsEnabled", "", 1, 1}, /* JavaScript execution: 0=Off 1=Inline 2=Full */
    {"jsEngine", "", 0, 1} /* JavaScript engine: 0=muJS 1=Duktape 2=QuickJS (only used when jsEnabled != 0) */
};
static int g_settingCount = 10;

static int g_haveDefaults = 0;

/* ── list helpers ────────────────────────────────────────────────────────── */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)PLUTO_MALLOC(n);
    if (p)
    {
        memcpy(p, s, n);
    }
    return p;
}

const StoredBookmark *storage_bookmark_at(int i)
{
    return (i >= 0 && i < g_bookmarkCount) ? g_bookmarks[i] : NULL;
}

int storage_bookmark_count(void)
{
    return g_bookmarkCount;
}

const StoredHistoryItem *storage_history_at(int i)
{
    return (i >= 0 && i < g_historyCount) ? g_history[i] : NULL;
}

int storage_history_count(void)
{
    return g_historyCount;
}

/* ── settings helpers ────────────────────────────────────────────────────── */

static Setting *find_setting(const char *key)
{
    for (int i = 0; i < g_settingCount; i++)
    {
        if (strcmp(g_settings[i].key, key) == 0)
        {
            return &g_settings[i];
        }
    }
    return NULL;
}

int storage_setting_int(const char *key)
{
    Setting *s = find_setting(key);
    return (s && s->isInt) ? s->ival : 0;
}

void storage_set_setting_int(const char *key, int value)
{
    Setting *s = find_setting(key);
    if (s && s->isInt)
    {
        s->ival = value;
    }
}

const char *storage_setting_str(const char *key)
{
    Setting *s = find_setting(key);
    return (s && !s->isInt) ? s->sval : NULL;
}

void storage_set_setting_str(const char *key, const char *value)
{
    Setting *s = find_setting(key);
    if (s && !s->isInt && value)
    {
        snprintf(s->sval, sizeof(s->sval), "%s", value);
    }
}

/* ── escaping: \ → \\ , | → \p , tab → \t , CR/LF → \n \r , other <0x20 → \xHH ── */

/* Line-scratch hoisted to BSS: storage_load nests ~2.5KB of these locals
 * under the game task, and with the entities-decode chain beneath
 * unescape_to it overflowed the 61.8KB task stack on device (errorlog
 * "stack overflow in task gameTask", 2026-09-12). Same pattern as
 * cookie_jar.c. Single-threaded cooperative tasks: safe to share. */
static char g_stRaw[900];
static char g_stKey[64];
static char g_stVal[512];
static char g_stName[128];
static char g_stValue[400];
static char g_stDomain[256];
static char g_stPath[256];

static void escape_to(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 5 < cap; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\\')
        {
            out[o++] = '\\';
            out[o++] = '\\';
        }
        else if (c == '|')
        {
            out[o++] = '\\';
            out[o++] = 'p';
        }
        else if (c == '\t')
        {
            out[o++] = '\\';
            out[o++] = 't';
        }
        else if (c == '\n')
        {
            out[o++] = '\\';
            out[o++] = 'n';
        }
        else if (c == '\r')
        {
            out[o++] = '\\';
            out[o++] = 'r';
        }
        else if (c < 0x20)
        {
            o += (size_t)snprintf(out + o, cap - o, "\\x%02X", c);
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

static void unescape_to(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            if (*p == 'p')
            {
                out[o++] = '|';
            }
            else if (*p == 't')
            {
                out[o++] = '\t';
            }
            else if (*p == 'n')
            {
                out[o++] = '\n';
            }
            else if (*p == 'r')
            {
                out[o++] = '\r';
            }
            else if (*p == '\\')
            {
                out[o++] = '\\';
            }
            else if (*p == 'x' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0)
            {
                out[o++] = (char)((hexval(p[1]) << 4) | hexval(p[2]));
                p += 2;
            }
            else
            {
                out[o++] = *p; /* unknown escape: keep the char literally */
            }
        }
        else
        {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* ── line reader (the C file API has no readline) ────────────────────────── */

/* Reads one '\n'-terminated line into buf. Returns 1 on success, 0 at EOF.
 * The trailing newline is NOT stored; a NUL is appended. */
static int read_line(SDFile *f, char *buf, size_t cap)
{
    size_t o = 0;
    int c;
    char ch;
    int got = 0;
    while ((c = pluto_pd()->file->read(f, &ch, 1)) == 1)
    {
        got = 1;
        if (ch == '\n')
        {
            break;
        }
        if (o + 1 < cap)
        {
            buf[o++] = ch;
        }
    }
    buf[o] = '\0';
    return got;
}

/* ── load: parse file if present, else defaults ──────────────────────────── */

static void free_lists(void)
{
    for (int i = 0; i < g_bookmarkCount; i++)
    {
        PLUTO_FREE(g_bookmarks[i]->title);
        PLUTO_FREE(g_bookmarks[i]->url);
        PLUTO_FREE(g_bookmarks[i]->desc);
        PLUTO_FREE(g_bookmarks[i]);
    }
    g_bookmarkCount = 0;
    for (int i = 0; i < g_historyCount; i++)
    {
        PLUTO_FREE(g_history[i]->title);
        PLUTO_FREE(g_history[i]->url);
        PLUTO_FREE(g_history[i]->time);
        PLUTO_FREE(g_history[i]);
    }
    g_historyCount = 0;
}

static void load_defaults(void)
{
    free_lists();
    for (int i = 0; i < DEFAULT_BOOKMARK_COUNT; i++)
    {
        StoredBookmark *b = (StoredBookmark *)PLUTO_MALLOC(sizeof(StoredBookmark));
        if (!b)
        {
            break;
        }
        b->title = dup_str(DEFAULT_BOOKMARKS[i].title);
        b->url = dup_str(DEFAULT_BOOKMARKS[i].url);
        b->desc = dup_str(DEFAULT_BOOKMARKS[i].desc);
        g_bookmarks[g_bookmarkCount++] = b;
    }
    /* settings stay at their initializer defaults; fontSize="medium" etc. */
}

static void load_cookies_from_jar_test_shim(void)
{
    /* Cookies live in the jar (Phase 8); the file's [cookies] section is
     * written from the jar and restored into it here via the public test
     * accessor. Phase 30 integration will move this behind the save hook. */
}

/* Big scratch buffers live in BSS, not the stack: the device game-task
 * stack is small, and storage_load → storage_save → logger_log nests three
 * ~2-3KB frames (same failure mode as the P18 battery; see main.c). These
 * are safe as statics — the game is single-task and never re-enters them. */
static char g_loadLine[1024];
static char g_saveEsc[800];
static char g_saveLine[1200];

void storage_load(void)
{
    logger_stack_touch();
    /* NOTE: kFileRead reads the game BUNDLE; data files written via kFileWrite
     * live in the /Data/<bundleid> sandbox and must be read with
     * kFileReadData (this is what datastore read/write used in Lua). */
    logger_log("STORAGE: load enter");
    SDFile *f = pluto_pd()->file->open(DATA_FILENAME, kFileReadData);
    if (!f)
    {
        logger_log("STORAGE: no data file -> defaults");
        load_defaults();
        logger_log("STORAGE: defaults done (bm=%d)", g_bookmarkCount);
        g_haveDefaults = 1;
        logger_log("STORAGE: save enter");
        storage_save(); /* Lua: first run saves the defaults */
        logger_log("STORAGE: save done");
        return;
    }
    logger_log("STORAGE: file opened, parsing");

    free_lists();

    /* reset settings to defaults before applying the file */
    storage_set_setting_int("searchEngine", 1);
    storage_set_setting_int("mode", 1); /* Constants.MODE_RAW_HTML (Lua default) */
    storage_set_setting_int("autoReader", 0);
    storage_set_setting_str("fontSize", "medium");
    storage_set_setting_str("imageMode", "viewport"); /* IMAGE_MODE_VIEWPORT */
    storage_set_setting_int("showFps", 0); /* FPS overlay off by default */
    storage_set_setting_int("invertCrank", 0);
    storage_set_setting_int("displayFps", 30); /* 30 fps default per Playdate SDK */
    storage_set_setting_int("jsEnabled", 1); /* JavaScript execution On by default */
    storage_set_setting_int("jsEngine", 0); /* muJS engine by default */

    char *line = g_loadLine;
    char section[32] = "";
    while (read_line(f, line, 1024))
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#')
        {
            continue;
        }
        if (line[0] == '[')
        {
            char *end = strchr(line, ']');
            if (end)
            {
                size_t n = (size_t)(end - line - 1);
                if (n >= sizeof(section))
                {
                    n = sizeof(section) - 1;
                }
                memcpy(section, line + 1, n);
                section[n] = '\0';
            }
            continue;
        }

        if (strcmp(section, "settings") == 0 && line[0] == 'S' && line[1] == '|')
        {
            char *raw = g_stRaw;
            snprintf(raw, 600, "%s", line + 2);
            char *eq = strchr(raw, '=');
            if (eq)
            {
                *eq = '\0';
                char *key = g_stKey;
                char *val = g_stVal;
                unescape_to(raw, key, 64);
                unescape_to(eq + 1, val, 512);
                Setting *s = find_setting(key);
                if (s)
                {
                    if (s->isInt)
                    {
                        s->ival = atoi(val);
                    }
                    else
                    {
                        /* sval is 128 bytes; copy at most that many minus NUL */
                        size_t vlen = strlen(val);
                        if (vlen > sizeof(s->sval) - 1)
                        {
                            vlen = sizeof(s->sval) - 1;
                        }
                        memcpy(s->sval, val, vlen);
                        s->sval[vlen] = '\0';
                    }
                }
            }
        }
        else if (strcmp(section, "bookmarks") == 0 && line[0] == 'B' && line[1] == '|')
        {
            /* B|title|url|desc — split on unescaped pipes (escapes already
             * encode pipes, so a plain strtok-style split is safe) */
            char *raw = g_stRaw;
            snprintf(raw, sizeof(g_stRaw), "%s", line + 2);
            char *p1 = strchr(raw, '|');
            char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
            char *p3 = p2 ? strchr(p2 + 1, '|') : NULL;
            if (p1 && p2)
            {
                *p1 = '\0';
                *p2 = '\0'; /* url ends here — without this, url swallows desc */
                if (p3)
                {
                    *p3 = '\0';
                }
                StoredBookmark *b = (StoredBookmark *)PLUTO_MALLOC(sizeof(StoredBookmark));
                if (b && g_bookmarkCount < 64)
                {
                    b->title = (char *)PLUTO_MALLOC(400);
                    b->url = (char *)PLUTO_MALLOC(600);
                    b->desc = (char *)PLUTO_MALLOC(400);
                    unescape_to(raw, b->title, 400);
                    unescape_to(p1 + 1, b->url, 600);
                    unescape_to(p3 ? p3 + 1 : p2 + 1, b->desc, 400);
                    g_bookmarks[g_bookmarkCount++] = b;
                }
                else if (b)
                {
                    PLUTO_FREE(b);
                }
            }
        }
        else if (strcmp(section, "history") == 0 && line[0] == 'H' && line[1] == '|')
        {
            char *raw = g_stRaw;
            snprintf(raw, sizeof(g_stRaw), "%s", line + 2);
            /* H|time|title|url */
            char *p1 = strchr(raw, '|');
            char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
            char *p3 = p2 ? strchr(p2 + 1, '|') : NULL;
            if (p1 && p2)
            {
                *p1 = '\0';
                *p2 = '\0'; /* title ends here — without this, title swallows url */
                if (p3)
                {
                    *p3 = '\0';
                }
                StoredHistoryItem *h = (StoredHistoryItem *)PLUTO_MALLOC(sizeof(StoredHistoryItem));
                if (h && g_historyCount < HISTORY_MAX)
                {
                    h->time = (char *)PLUTO_MALLOC(16);
                    h->title = (char *)PLUTO_MALLOC(400);
                    h->url = (char *)PLUTO_MALLOC(600);
                    unescape_to(raw, h->time, 16);
                    unescape_to(p1 + 1, h->title, 400);
                    unescape_to(p3 ? p3 + 1 : p2 + 1, h->url, 600);
                    g_history[g_historyCount++] = h;
                }
                else if (h)
                {
                    PLUTO_FREE(h);
                }
            }
        }
        else if (strcmp(section, "cookies") == 0 && line[0] == 'C' && line[1] == '|')
        {
            /* C|name|value|domain|hostOnly|path|secure|httpOnly|samesite|expires */
            char *raw = g_stRaw;
            snprintf(raw, sizeof(g_stRaw), "%s", line + 2);
            char *tok[10];
            int nt = 0;
            tok[nt++] = raw;
            for (char *p = raw; *p && nt < 10; p++)
            {
                if (*p == '|')
                {
                    *p = '\0';
                    tok[nt++] = p + 1;
                }
            }
            if (nt >= 10)
            {
                Cookie c;
                memset(&c, 0, sizeof(c));
                char *name = g_stName;
                char *value = g_stValue;
                char *domain = g_stDomain;
                char *path = g_stPath;
                char samesite[8];
                unescape_to(tok[0], name, 128);
                unescape_to(tok[1], value, 400);
                unescape_to(tok[2], domain, 256);
                unescape_to(tok[4], path, 256);
                unescape_to(tok[8], samesite, sizeof(samesite));
                c.name = name;
                c.value = value;
                c.domain = domain;
                c.hostOnly = atoi(tok[3]);
                c.path = path;
                c.secure = atoi(tok[5]);
                c.httpOnly = atoi(tok[6]);
                snprintf(c.samesite, sizeof(c.samesite), "%s", samesite);
                c.expires = strtoll(tok[9], NULL, 10);
                (void)load_cookies_from_jar_test_shim;
                /* Restore into the jar via store-less direct path: the jar
                 * exposes no import yet; Phase 9 keeps cookies file-backed
                 * and the jar restores them in Phase 30 (network integration)
                 * when getHeader first runs. Until then this section is
                 * written but not re-injected. Documented in MASTER_TODO. */
            }
        }
    }
    pluto_pd()->file->close(f);
    g_haveDefaults = 0;

    /* Lua parity: a saved table without bookmarks (or empty) falls back to
     * DEFAULT_BOOKMARKS; history/cookies simply stay empty when absent. */
    if (g_bookmarkCount == 0)
    {
        for (int i = 0; i < DEFAULT_BOOKMARK_COUNT; i++)
        {
            StoredBookmark *b = (StoredBookmark *)PLUTO_MALLOC(sizeof(StoredBookmark));
            if (!b)
            {
                break;
            }
            b->title = dup_str(DEFAULT_BOOKMARKS[i].title);
            b->url = dup_str(DEFAULT_BOOKMARKS[i].url);
            b->desc = dup_str(DEFAULT_BOOKMARKS[i].desc);
            g_bookmarks[g_bookmarkCount++] = b;
        }
    }
}

/* ── save ────────────────────────────────────────────────────────────────── */

/* snprintf-append that never lets the offset pass the buffer. snprintf's
 * return is the *wanted* length, so unchecked "n += snprintf(...)" chains
 * overflow (size_t underflow) on long lines. */
static int save_append(char *dst, size_t cap, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= cap)
    {
        return off < 0 ? 0 : (int)cap - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(dst + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (w < 0)
    {
        return off;
    }
    size_t room = cap - (size_t)off - 1;
    if ((size_t)w > room)
    {
        w = (int)room;
    }
    return off + w;
}

void storage_save(void)
{
    logger_log("STORAGE: save open");
    SDFile *f = pluto_pd()->file->open(DATA_FILENAME, kFileWrite);
    if (!f)
    {
        logger_log("STORAGE: save open FAILED");
        return;
    }
    logger_log("STORAGE: save writing (bm=%d hist=%d)", g_bookmarkCount, g_historyCount);
    char *esc = g_saveEsc;
    char *line = g_saveLine;

    pluto_pd()->file->write(f, "[bookmarks]\n", 12);
    for (int i = 0; i < g_bookmarkCount; i++)
    {
        escape_to(g_bookmarks[i]->title, esc, sizeof(g_saveEsc));
        int n = save_append(line, sizeof(g_saveLine), 0, "B|%s|", esc);
        escape_to(g_bookmarks[i]->url, esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s|", esc);
        escape_to(g_bookmarks[i]->desc, esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s\n", esc);
        pluto_pd()->file->write(f, line, (unsigned int)n);
    }

    pluto_pd()->file->write(f, "[history]\n", 10);
    for (int i = 0; i < g_historyCount; i++)
    {
        escape_to(g_history[i]->time, esc, sizeof(g_saveEsc));
        int n = save_append(line, sizeof(g_saveLine), 0, "H|%s|", esc);
        escape_to(g_history[i]->title, esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s|", esc);
        escape_to(g_history[i]->url, esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s\n", esc);
        pluto_pd()->file->write(f, line, (unsigned int)n);
    }

    pluto_pd()->file->write(f, "[cookies]\n", 10);
    for (int i = 0; i < cookie_jar_count(); i++)
    {
        const Cookie *c = cookie_jar_get(i);
        if (!c || !c->name || !c->domain)
        {
            continue;
        }
        escape_to(c->name, esc, sizeof(g_saveEsc));
        int n = save_append(line, sizeof(g_saveLine), 0, "C|%s|", esc);
        escape_to(c->value ? c->value : "", esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s|", esc);
        escape_to(c->domain, esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s|%d|", esc, c->hostOnly ? 1 : 0);
        escape_to(c->path ? c->path : "/", esc, sizeof(g_saveEsc));
        n = save_append(line, sizeof(g_saveLine), n, "%s|%d|%d|%s|%lld\n",
                        esc, c->secure ? 1 : 0, c->httpOnly ? 1 : 0,
                        c->samesite, c->expires);
        pluto_pd()->file->write(f, line, (unsigned int)n);
    }

    pluto_pd()->file->write(f, "[settings]\n", 11);
    for (int i = 0; i < g_settingCount; i++)
    {
        char val[160];
        if (g_settings[i].isInt)
        {
            snprintf(val, sizeof(val), "%d", g_settings[i].ival);
        }
        else
        {
            escape_to(g_settings[i].sval, val, sizeof(val));
        }
        char keyEsc[64];
        escape_to(g_settings[i].key, keyEsc, sizeof(keyEsc));
        int n = save_append(line, sizeof(g_saveLine), 0, "S|%s=%s\n", keyEsc, val);
        pluto_pd()->file->write(f, line, (unsigned int)n);
    }

    logger_log("STORAGE: save close");
    pluto_pd()->file->close(f);
    logger_log("STORAGE: save exit");
}

/* ── init ────────────────────────────────────────────────────────────────── */

void storage_init(PlaydateAPI *pd)
{
    (void)pd; /* pluto_pd() already set by main */
    logger_log("STORAGE: init enter");
    /* fill the two settings whose defaults come from Constants */
    storage_set_setting_int("mode", 1);       /* Constants.MODE_RAW_HTML (Lua default) */
    storage_set_setting_str("imageMode", "viewport"); /* IMAGE_MODE_VIEWPORT */
    storage_set_setting_int("showFps", 0); /* FPS overlay off by default */
    storage_set_setting_int("displayFps", 30); /* 30 fps default per Playdate SDK */
    storage_set_setting_int("jsEnabled", 1); /* JavaScript execution On by default */
    storage_set_setting_int("jsEngine", 0); /* muJS engine by default */
    storage_load();
    logger_log("STORAGE: init exit");
}

/* ── bookmarks ───────────────────────────────────────────────────────────── */

int storage_add_bookmark(const char *title, const char *url, const char *desc)
{
    if (!url || url[0] == '\0')
    {
        return 0;
    }
    if (!title || title[0] == '\0')
    {
        title = url;
    }
    if (!desc)
    {
        desc = "";
    }

    for (int i = 0; i < g_bookmarkCount; i++)
    {
        if (strcmp(g_bookmarks[i]->url, url) == 0)
        {
            PLUTO_FREE(g_bookmarks[i]->title);
            g_bookmarks[i]->title = dup_str(title);
            storage_save();
            return 1;
        }
    }

    if (g_bookmarkCount >= 64)
    {
        return 0;
    }
    StoredBookmark *b = (StoredBookmark *)PLUTO_MALLOC(sizeof(StoredBookmark));
    if (!b)
    {
        return 0;
    }
    b->title = dup_str(title);
    b->url = dup_str(url);
    b->desc = dup_str(desc);
    g_bookmarks[g_bookmarkCount++] = b;
    storage_save();
    return 1;
}

int storage_remove_bookmark(int index)
{
    if (index < 0 || index >= g_bookmarkCount)
    {
        return 0;
    }
    PLUTO_FREE(g_bookmarks[index]->title);
    PLUTO_FREE(g_bookmarks[index]->url);
    PLUTO_FREE(g_bookmarks[index]->desc);
    PLUTO_FREE(g_bookmarks[index]);
    for (int i = index; i < g_bookmarkCount - 1; i++)
    {
        g_bookmarks[i] = g_bookmarks[i + 1];
    }
    g_bookmarkCount--;
    storage_save();
    return 1;
}

int storage_is_bookmarked(const char *url)
{
    if (!url)
    {
        return 0;
    }
    for (int i = 0; i < g_bookmarkCount; i++)
    {
        if (strcmp(g_bookmarks[i]->url, url) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* ── history ─────────────────────────────────────────────────────────────── */

void storage_add_history(const char *title, const char *url)
{
    if (!url || url[0] == '\0' || strncmp(url, "about:", 6) == 0)
    {
        return;
    }
    if (!title || title[0] == '\0')
    {
        title = url;
    }

    /* dedupe: remove any existing entry with this URL */
    for (int i = 0; i < g_historyCount; i++)
    {
        if (strcmp(g_history[i]->url, url) == 0)
        {
            PLUTO_FREE(g_history[i]->title);
            PLUTO_FREE(g_history[i]->url);
            PLUTO_FREE(g_history[i]->time);
            PLUTO_FREE(g_history[i]);
            for (int j = i; j < g_historyCount - 1; j++)
            {
                g_history[j] = g_history[j + 1];
            }
            g_historyCount--;
            break;
        }
    }

    /* HH:MM local time (Lua: playdate.getTime().hour/minute) */
    char timeFormatted[8] = "Recent";
    unsigned int ms = 0;
    uint32_t epoch = pluto_pd()->system->getSecondsSinceEpoch(&ms);
    int tz = pluto_pd()->system->getTimezoneOffset(); /* minutes */
    struct PDDateTime dt;
    pluto_pd()->system->convertEpochToDateTime(epoch + (uint32_t)(tz * 60), &dt);
    snprintf(timeFormatted, sizeof(timeFormatted), "%02d:%02d", dt.hour, dt.minute);

    /* shift down and insert at top (Lua table.insert(history, 1, …)) */
    if (g_historyCount < HISTORY_MAX)
    {
        for (int i = g_historyCount; i > 0; i--)
        {
            g_history[i] = g_history[i - 1];
        }
        g_historyCount++;
    }
    else
    {
        /* drop the oldest (last) to make room at the top */
        PLUTO_FREE(g_history[HISTORY_MAX - 1]->title);
        PLUTO_FREE(g_history[HISTORY_MAX - 1]->url);
        PLUTO_FREE(g_history[HISTORY_MAX - 1]->time);
        PLUTO_FREE(g_history[HISTORY_MAX - 1]);
        for (int i = HISTORY_MAX - 1; i > 0; i--)
        {
            g_history[i] = g_history[i - 1];
        }
    }
    StoredHistoryItem *h = (StoredHistoryItem *)PLUTO_MALLOC(sizeof(StoredHistoryItem));
    if (!h)
    {
        return;
    }
    h->title = dup_str(title);
    h->url = dup_str(url);
    h->time = dup_str(timeFormatted);
    g_history[0] = h;

    storage_save();
}

void storage_clear_cookies_for_test(void)
{
    cookie_jar_clear();
}

int storage_test_corrupt_recovery(void)
{
    /* Overwrite the data file with garbage. */
    SDFile *f = pluto_pd()->file->open(DATA_FILENAME, kFileWrite);
    if (f)
    {
        pluto_pd()->file->write(f, "GARBAGE!!!\nnot [a] valid section\n", 32);
        pluto_pd()->file->close(f);
    }

    /* Reload: parser finds no sections → defaults. */
    storage_load();
    int result = g_bookmarkCount;

    /* Re-save to replace the garbage with valid data. */
    storage_save();
    return result;
}
