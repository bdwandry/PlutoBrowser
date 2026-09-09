/*
 * PlutoBrowser — jsenv.h
 * Basic JavaScript support (muJS/ES5) behind the Settings "JavaScript" toggle.
 *
 * Design (device-safe):
 *   - Scripts are EXTRACTED from the raw HTML by jsenv (so the existing
 *     tokenizer skip-logic stays untouched) and executed in a fresh muJS
 *     state per page render.
 *   - Hard limits: one-shot run limit (statement/back-edge counter) and a
 *     single-allocation size cap. muJS converts "too much recursion" and
 *     "script ran too long" into catchable JS errors, and js_pcall contains
 *     everything else, so a hostile page cannot hang or crash the app.
 *   - Browser surface stubbed, not implemented: document.write append-only,
 *     navigator (userAgent/appCodeName), console (log/error/warn), a bare
 *     window/document object, alert/location stubs, and setTimeout discards.
 *   - document.write output is captured into a malloc'd buffer the CALLER
 *     re-parses (document_parse of the concatenated script output) — the
 *     resulting blocks are appended to the page's block list.
 *
 * No state is shared between pages: each render creates and destroys its own
 * engine, so unloading a page frees everything the scripts allocated.
 */
#ifndef PLUTO_JSENV_H
#define PLUTO_JSENV_H

#include <stddef.h>

typedef struct JsEnv JsEnv;

/* ── Config ──────────────────────────────────────────────────────────────── */
#define JSENV_MAX_SCRIPTS 64      /* <script> blocks per page */
#define JSENV_MAX_SCRIPT_BYTES (96 * 1024) /* per-script source cap */
#define JSENV_MAX_OUTPUT (192 * 1024)      /* total document.write cap */
#define JSENV_RUNLIMIT 4000000    /* one-shot statement counter */
#define JSENV_MAXALLOC (256 * 1024) /* single-allocation size cap */

/* muJS's js_dofile/js_ploadfile pull newlib stdio into the device binary
 * (undefined _read/_write syscalls). The browser only ever runs inline
 * <script> sources, so the file entry points are never built. */
#define MUJS_NO_FILE_IO 1

typedef struct
{
    char *output;   /* malloc'd document.write capture (NULL if none) */
    size_t outLen;
    int scriptsRun; /* how many scripts executed */
    int scriptsErr; /* how many failed (parse or runtime) */
    char lastError[128];
} JsEnvResult;

/* Execute all <script> bodies found in `html`. Returns a heap env or NULL
 * (out of memory). Never runs longer than the limits above; errors are
 * reported in *res, never thrown. baseUrl is exposed to scripts as
 * location.href (read-only string). */
JsEnv *jsenv_run(const char *html, const char *baseUrl, JsEnvResult *res);

/* Free the engine and the captured output. */
void jsenv_free(JsEnv *env);

#ifdef MUJS_NO_FILE_IO
typedef struct js_State js_State;
int js_ploadfile(js_State *J, const char *filename);
int js_dofile(js_State *J, const char *filename);
#endif

#endif /* PLUTO_JSENV_H */
