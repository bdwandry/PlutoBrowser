/*
 * PlutoBrowser — jsenv.c
 * Basic JavaScript support (muJS/ES5). See jsenv.h for the design contract.
 *
 * muJS facts this file relies on:
 *   - js_ploadstring compiles without executing and returns non-zero on a
 *     SyntaxError instead of longjmp-ing (report hook gets the message).
 *   - js_pcall runs the loaded chunk with the error contained; the error
 *     value is left on the stack top.
 *   - js_setlimit(runlimit, memlimit): runlimit counts statements/back-edges
 *     and throws a catchable "script ran too long"; memlimit caps each
 *     individual allocation size (single-shot; "out of memory" thrown).
 *   - Native functions registered with js_newcfunction + js_defglobal can
 *     push their return values; js_pop(J, 1) in the wrapper discards them.
 *
 * document.write semantics: append-only capture. After all scripts run, the
 * CALLER feeds env result `output` back through document_parse and appends
 * the produced blocks (see document.c: doc_append_script_html).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "jsenv.h"
#include "mujs.h"
#include "../core/logger.h"

extern PlaydateAPI *pluto_pd(void);
#define JMalloc(n) pluto_pd()->system->realloc(NULL, (n))
#define JFree(p) pluto_pd()->system->realloc((p), 0)

/* ── muJS allocator: route through the SDK so all engine memory is pooled ── */
static void *js_alloc(void *actx, void *ptr, int size)
{
    (void)actx;
    return pluto_pd()->system->realloc(ptr, (size > 0) ? (unsigned)size : 0u);
}

/* ── Error reporting hook ────────────────────────────────────────────────── */
struct JsEnv
{
    js_State *J;
    char *output;      /* document.write capture */
    size_t outLen;
    size_t outCap;
    int scriptsRun;
    int scriptsErr;
    char lastError[128];
    const char *baseUrl; /* borrowed; exposed as location.href */
};

static void js_report_cb(js_State *J, const char *message)
{
    struct JsEnv *env = (struct JsEnv *)js_getcontext(J);
    if (env && message)
    {
        snprintf(env->lastError, sizeof(env->lastError), "%s", message);
    }
}

/* ── document.write / writeln ────────────────────────────────────────────── */
static void dw_append(struct JsEnv *env, const char *s)
{
    if (!s)
    {
        return;
    }
    size_t n = strlen(s);
    if (env->outLen + n + 1 > JSENV_MAX_OUTPUT)
    {
        if (env->outLen < JSENV_MAX_OUTPUT)
        {
            size_t room = JSENV_MAX_OUTPUT - env->outLen;
            memcpy(env->output + env->outLen, s, room);
            env->outLen = JSENV_MAX_OUTPUT;
            env->output[env->outLen] = '\0';
        }
        return; /* silently drop the rest (limit reached) */
    }
    memcpy(env->output + env->outLen, s, n);
    env->outLen += n;
    env->output[env->outLen] = '\0';
}

static void js_document_write(js_State *J)
{
    struct JsEnv *env = (struct JsEnv *)js_getcontext(J);
    int top = js_gettop(J);
    for (int i = 1; i < top; i++) /* args live at 1..top-1 */
    {
        if (env)
        {
            dw_append(env, js_tostring(J, i));
        }
    }
    js_pushundefined(J);
}

static void js_document_writeln(js_State *J)
{
    struct JsEnv *env = (struct JsEnv *)js_getcontext(J);
    int top = js_gettop(J);
    for (int i = 1; i < top; i++) /* args live at 1..top-1 */
    {
        if (env)
        {
            dw_append(env, js_tostring(J, i));
        }
    }
    if (env)
    {
        dw_append(env, "\n");
    }
    js_pushundefined(J);
}

/* ── console.log / warn / error ──────────────────────────────────────────── */
static void js_console_log(js_State *J)
{
    int top = js_gettop(J);
    char line[192];
    size_t off = 0;
    line[0] = '\0';
    for (int i = 1; i <= top && off < sizeof(line) - 1; i++)
    {
        const char *s = js_tostring(J, i);
        if (s)
        {
            size_t n = strlen(s);
            if (off + n >= sizeof(line) - 2)
            {
                n = sizeof(line) - 2 - off;
            }
            memcpy(line + off, s, n);
            off += n;
            if (i < top)
            {
                line[off++] = ' ';
            }
            line[off] = '\0';
        }
    }
    logger_log("[js] %s", line);
    js_pushundefined(J);
}

/* ── no-op sinks (alert/confirm/prompt/setTimeout family) ────────────────── */
static void js_noop(js_State *J)
{
    js_pushundefined(J);
}

/* location.replace: recorded but not followed (no navigation from JS yet) */
static void js_location_replace(js_State *J)
{
    js_pushundefined(J);
}

/* ── Environment construction ────────────────────────────────────────────── */
static void define_global_fns(struct JsEnv *env, const char *baseUrl)
{
    js_State *J = env->J;

    /* Bare window object (identity alias for `globalThis`-style access). */
    js_pushglobal(J);
    js_setglobal(J, "window");

    /* document object: write/writeln only. */
    js_newobject(J);
    {
        js_newcfunction(J, js_document_write, "document.write", 0);
        js_setproperty(J, -2, "write");
        js_newcfunction(J, js_document_writeln, "document.writeln", 0);
        js_setproperty(J, -2, "writeln");
    }
    js_setglobal(J, "document");

    /* navigator */
    js_newobject(J);
    {
        js_pushstring(J, "PlutoBrowser/1.0 (Playdate; muJS ES5)");
        js_setproperty(J, -2, "userAgent");
        js_pushstring(J, "PlutoBrowser");
        js_setproperty(J, -2, "appCodeName");
        js_pushstring(J, "1.0");
        js_setproperty(J, -2, "appVersion");
    }
    js_setglobal(J, "navigator");

    /* console */
    js_newobject(J);
    {
        js_newcfunction(J, js_console_log, "console.log", 0);
        js_setproperty(J, -2, "log");
        js_newcfunction(J, js_console_log, "console.warn", 0);
        js_setproperty(J, -2, "warn");
        js_newcfunction(J, js_console_log, "console.error", 0);
        js_setproperty(J, -2, "error");
    }
    js_setglobal(J, "console");

    /* location: href is a plain string (muJS props are not accessors);
     * replace() is accepted and ignored. */
    js_newobject(J);
    {
        js_pushstring(J, baseUrl ? baseUrl : "");
        js_setproperty(J, -2, "href");
        js_newcfunction(J, js_location_replace, "location.replace", 1);
        js_setproperty(J, -2, "replace");
    }
    js_setglobal(J, "location");

    /* no-op sinks so scripts don't crash calling them */
    js_newcfunction(J, js_noop, "alert", 1);
    js_setglobal(J, "alert");
    js_newcfunction(J, js_noop, "confirm", 1);
    js_setglobal(J, "confirm");
    js_newcfunction(J, js_noop, "prompt", 1);
    js_setglobal(J, "prompt");
    js_newcfunction(J, js_noop, "setTimeout", 2);
    js_setglobal(J, "setTimeout");
    js_newcfunction(J, js_noop, "setInterval", 2);
    js_setglobal(J, "setInterval");
    js_newcfunction(J, js_noop, "clearTimeout", 1);
    js_setglobal(J, "clearTimeout");
    js_newcfunction(J, js_noop, "clearInterval", 1);
    js_setglobal(J, "clearInterval");
    js_newcfunction(J, js_noop, "requestAnimationFrame", 1);
    js_setglobal(J, "requestAnimationFrame");

    (void)env;
}

/* ── Script extraction (mirrors the tokenizer's <script> skip rules) ─────── */
static const char *find_str(const char *hay, const char *end, const char *needle,
                            size_t nlen)
{
    if (nlen == 0 || (size_t)(end - hay) < nlen)
    {
        return NULL;
    }
    for (const char *p = hay; p + nlen <= end; p++)
    {
        if (*p == needle[0] && memcmp(p, needle, nlen) == 0)
        {
            return p;
        }
    }
    return NULL;
}

static int ci_has_prefix(const char *s, size_t maxlen, const char *prefix)
{
    size_t n = strlen(prefix);
    if (maxlen < n)
    {
        return 0;
    }
    for (size_t i = 0; i < n; i++)
    {
        char a = s[i];
        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (a != prefix[i])
        {
            return 0;
        }
    }
    return 1;
}

/* Copy a case-insensitive attribute value out of a tag interior span.
 * Returns length written (0 = not found). value gets NUL-terminated. */
static size_t tag_attr_value(const char *b, const char *e, const char *attr,
                             char *value, size_t vcap)
{
    size_t alen = strlen(attr);
    for (const char *p = b; p + alen + 1 < e; p++)
    {
        if (!ci_has_prefix(p, (size_t)(e - p), attr))
        {
            continue;
        }
        const char *q = p + alen;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
        {
            q++;
        }
        if (q >= e || *q != '=')
        {
            continue;
        }
        q++;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
        {
            q++;
        }
        if (q >= e)
        {
            return 0;
        }
        if (*q == '"' || *q == '\'')
        {
            char close = *q++;
            const char *z = memchr(q, close, (size_t)(e - q));
            if (!z)
            {
                return 0;
            }
            size_t n = (size_t)(z - q);
            if (n >= vcap)
            {
                n = vcap - 1;
            }
            memcpy(value, q, n);
            value[n] = '\0';
            return n;
        }
        /* unquoted */
        const char *z = q;
        while (z < e && *z != ' ' && *z != '\t' && *z != '\r' && *z != '\n' &&
               *z != '>')
        {
            z++;
        }
        size_t n = (size_t)(z - q);
        if (n >= vcap)
        {
            n = vcap - 1;
        }
        memcpy(value, q, n);
        value[n] = '\0';
        return n;
    }
    return 0;
}

/* Extract <script> body spans. Writes up to max spans; returns count found
 * (may exceed max — caller only executes the first max). */
static int extract_scripts(const char *html, const char **starts,
                           size_t *lens, int max)
{
    const char *pos = html;
    const char *end = html + strlen(html);
    int count = 0;

    while (pos < end)
    {
        const char *lt = memchr(pos, '<', (size_t)(end - pos));
        if (!lt)
        {
            break;
        }
        const char *gt = find_str(lt, end, ">", 1);
        if (!gt)
        {
            break;
        }
        /* interior of the tag */
        const char *b = lt + 1;
        const char *e = gt;
        while (b < e && (*b == ' ' || *b == '\t'))
        {
            b++;
        }
        if (!ci_has_prefix(b, (size_t)(e - b), "script"))
        {
            pos = gt + 1;
            continue;
        }
        /* a real <script ...> open tag: next non-space after name must not
         * be a letter (else it's <scriptx ...) */
        const char *afterName = b + 6;
        if (afterName < e &&
            ((*afterName >= 'a' && *afterName <= 'z') ||
             (*afterName >= 'A' && *afterName <= 'Z')))
        {
            pos = gt + 1;
            continue;
        }

        /* find closing </script (case-insensitive per HTML spec; the real
         * world is lowercase, so scan both) */
        const char *close = find_str(gt + 1, end, "</script", 8);
        if (!close)
        {
            const char *c2 = gt + 1;
            /* case-insensitive scan */
            while (c2 + 8 <= end)
            {
                if ((c2[0] == '<' || c2[0] == '<') &&
                    (c2[1] == '/' || c2[1] == '/') &&
                    (c2[2] | 0x20) == 's' && (c2[3] | 0x20) == 'c' &&
                    (c2[4] | 0x20) == 'r' && (c2[5] | 0x20) == 'i' &&
                    (c2[6] | 0x20) == 'p' && (c2[7] | 0x20) == 't')
                {
                    close = c2;
                    break;
                }
                c2++;
            }
        }
        if (!close)
        {
            break; /* unterminated: drop (tokenizer parity) */
        }

        if (count < max)
        {
            starts[count] = gt + 1;
            lens[count] = (size_t)(close - (gt + 1));
        }
        count++;

        pos = close + 8;
        /* skip to after the '>' of </script ...> */
        while (pos < end && *pos != '>')
        {
            pos++;
        }
        if (pos < end)
        {
            pos++;
        }
    }
    return count;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
JsEnv *jsenv_run(const char *html, const char *baseUrl, JsEnvResult *res)
{
    struct JsEnv *env = (struct JsEnv *)JMalloc(sizeof(struct JsEnv));
    if (!env)
    {
        return NULL;
    }
    memset(env, 0, sizeof(*env));
    memset(res, 0, sizeof(*res));

    /* document.write capture buffer (lazily grown on first write) */
    env->outCap = 4096;
    env->output = (char *)JMalloc(env->outCap);
    if (!env->output)
    {
        JFree(env);
        return NULL;
    }
    env->output[0] = '\0';

    /* Engine. */
    js_State *J = js_newstate(js_alloc, NULL, 0);
    if (!J)
    {
        JFree(env->output);
        JFree(env);
        return NULL;
    }
    env->J = J;
    env->baseUrl = baseUrl;
    js_setcontext(J, env);
    js_setreport(J, js_report_cb);
    js_setlimit(J, JSENV_RUNLIMIT, JSENV_MAXALLOC);

    define_global_fns(env, baseUrl);

    /* Extract + execute. */
    const char *starts[JSENV_MAX_SCRIPTS];
    size_t lens[JSENV_MAX_SCRIPTS];
    int n = extract_scripts(html, starts, lens, JSENV_MAX_SCRIPTS);
    int total = (n > JSENV_MAX_SCRIPTS) ? JSENV_MAX_SCRIPTS : n;

    for (int i = 0; i < total; i++)
    {
        if (lens[i] == 0 || lens[i] > JSENV_MAX_SCRIPT_BYTES)
        {
            if (lens[i] > JSENV_MAX_SCRIPT_BYTES)
            {
                env->scriptsErr++;
                snprintf(env->lastError, sizeof(env->lastError),
                         "script %d too large", i + 1);
            }
            continue;
        }

        /* NUL-terminate the source (muJS wants a C string). */
        char *src = (char *)JMalloc(lens[i] + 1);
        if (!src)
        {
            env->scriptsErr++;
            snprintf(env->lastError, sizeof(env->lastError), "oom for script");
            continue;
        }
        memcpy(src, starts[i], lens[i]);
        src[lens[i]] = '\0';

        /* js_ploadstring compiles; js_pcall executes. Both contain errors.
         * Limits reset per script: the counters decrement monotonically, so
         * without a reset one runaway script would exhaust them for every
         * later script on the page. */
        js_setlimit(J, JSENV_RUNLIMIT, JSENV_MAXALLOC);
        env->scriptsRun++;
        if (js_ploadstring(J, "[page]", src) != 0)
        {
            env->scriptsErr++;
            js_pop(J, 1); /* error object */
        }
        else
        {
            js_pushundefined(J);
            if (js_pcall(J, 0) != 0)
            {
                env->scriptsErr++;
                if (!js_isundefined(J, -1) && !js_isnull(J, -1))
                {
                    const char *msg = js_tostring(J, -1);
                    if (msg)
                    {
                        snprintf(env->lastError, sizeof(env->lastError), "%s",
                                 msg);
                    }
                }
                js_pop(J, 1);
            }
            else
            {
                js_pop(J, 1); /* the pushed undefined return */
            }
        }
        JFree(src);

        /* GC between scripts keeps the heap bounded on device. */
        js_gc(J, 0);
    }

    /* Hand results to the caller. */
    res->output = env->output;
    res->outLen = env->outLen;
    res->scriptsRun = env->scriptsRun;
    res->scriptsErr = env->scriptsErr;
    snprintf(res->lastError, sizeof(res->lastError), "%s", env->lastError);

    /* Ownership of `output` transfers to the caller; detach from env so
     * jsenv_free doesn't free it. */
    env->output = NULL;
    env->outLen = 0;
    return env;
}

void jsenv_free(JsEnv *env)
{
    if (!env)
    {
        return;
    }
    if (env->J)
    {
        js_freestate(env->J);
        env->J = NULL;
    }
    if (env->output)
    {
        JFree(env->output);
    }
    JFree(env);
}

/* ── Device link shims ───────────────────────────────────────────────────── */
#ifdef TARGET_PLAYDATE
/* js_defaultpanic calls abort() only if no panic hook is installed; jsenv
 * always installs one, so this is a cold path. Provide it locally so the
 * toolchain's abort/_exit syscall chain isn't pulled into the binary. */
#include <stdlib.h>
void _exit(int code)
{
    (void)code;
    for (;;)
    {
        /* halted */
    }
}
void abort(void);
void abort(void)
{
    _exit(134);
}
int _write(int fd, const void *buf, unsigned len)
{
    (void)fd;
    (void)buf;
    return (int)len;
}
int _close(int fd)
{
    (void)fd;
    return 0;
}
int _fstat(int fd, void *st)
{
    (void)fd;
    (void)st;
    return 0;
}
int _isatty(int fd)
{
    (void)fd;
    return 0;
}
int _lseek(int fd, int off, int whence)
{
    (void)fd;
    (void)off;
    (void)whence;
    return 0;
}
int _read(int fd, void *buf, unsigned len)
{
    (void)fd;
    (void)buf;
    return 0;
}
int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}
int _getpid(void)
{
    return 1;
}
int _gettimeofday(void *tv, void *tz)
{
    (void)tv;
    (void)tz;
    return -1;
}
#endif

