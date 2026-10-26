/*
 * PlutoBrowser — sdisco.h
 * Script-Derived Submission Discovery (the general, zero-site-data form
 * route for script-dead pages).
 *
 * WHY THIS EXISTS
 *   On the device a page's own JS often cannot run (measured, R56: the
 *   react-dom factory's codegen demand exceeds the physical pool — the
 *   engine aborts containedly and the page keeps the SSR DOM). A browser
 *   with dead scripts cannot know where a form with NO action= attribute
 *   submits — Chrome itself would reload the page. The R56g answer was a
 *   per-site DATA file (form_sites.txt): general machinery, but the site
 *   knowledge lived in a deployed file — per-site hard-coding by another
 *   name, and it silently vanished on any redeploy that forgot the copy
 *   (the 2026-10-25 regression: Send email fell back to the legacy static
 *   GET and reloaded the page).
 *
 * WHAT THIS IS
 *   The browser ALREADY downloads every <script src> the page names and
 *   holds every inline body — the submission endpoint, method, content
 *   type, and payload key names the page's own handlers use are IN THOSE
 *   BYTES. A preload-scanner-style pass (the same speculative-parse idea
 *   real browsers use for resource discovery) lifts the candidate
 *   submissions out at download time:
 *
 *     fetch("https://api.example.com/send",{method:"POST",
 *       headers:{"Content-Type":"application/json"},
 *       body:JSON.stringify({email:e,subject:s,message:m})})
 *     xhr.open("POST","/api/send")
 *
 *   → a page-scoped table of endpoints. NOTHING here knows any site: the
 *   records come from whatever page is loaded, live in a fixed BSS table
 *   (bounded, no heap growth), and die at the page boundary. When a
 *   script-dead form submit has no HTML route (no action=), the fallback
 *   scores these endpoints against the form's OWN fields (payload key ↔
 *   field-name suffix match) and submits to the page's own discovered
 *   endpoint — the same request the page's script would have sent.
 *
 * MEMORY SHAPE (the disk/RAM contract)
 *   Bulk page data (the scripts themselves) is already disk-spilled when
 *   big (SW2b) — the spill lane is scanned straight off disk in bounded
 *   windows, never materialized. The discovery index is the only new
 *   state: a fixed BSS table (~2KB worst case), no allocation, no growth
 *   with script size.
 */
#ifndef PLUTO_SDISCO_H
#define PLUTO_SDISCO_H

#include <stddef.h>
#include "core/pluto_spill.h" /* SpillFile */

/* One discovered submission endpoint (page-scoped, script-derived). */
typedef struct
{
    char url[224];   /* raw target as written (relative resolved by the
                      * consumer against the document base — classic-script
                      * fetch() semantics) */
    int methodPost;  /* a POST method was named (fetch options / open()) */
    char contentType[48]; /* Content-Type literal in the fetch headers,
                           * "" = none named (consumer default applies) */
    char keys[8][24];     /* JSON.stringify({...}) payload keys, in order */
    int keyCount;
} SdiscoEndpoint;

/* Drop every record (page boundary — call from page_swap_doc). */
void sdisco_reset(void);

/* Scan one script body held in RAM (external small-body delivery, inline
 * bodies, local test fixtures). Safe to call on any bytes; only fetch/XHR
 * POST shapes produce records. */
void sdisco_scan_script(const char *src, size_t len);

/* Scan one script body held on disk (SW2b spill delivery) in bounded
 * windows — the body never materializes in RAM. */
void sdisco_scan_spill(SpillFile h, long len);

/* The discovered table (never NULL-safe-failed: count first). */
int sdisco_count(void);
const SdiscoEndpoint *sdisco_at(int i);

#endif /* PLUTO_SDISCO_H */
