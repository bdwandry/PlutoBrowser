#ifndef PLUTO_HTML_READABILITY_H
#define PLUTO_HTML_READABILITY_H

#include "html/document.h"
#include "html/tokenizer.h"

/* C port of Source/html/readability.lua (Readability.distill).
 *
 * Consumes a token list and returns a reader-mode DocDocument:
 *   blocks[0] = DB_READER_HEADER {readerHost, readerTitle, readingTime}
 *   blocks[1] = h1 heading with the bold page title inline
 *   blocks[2] = hr
 *   remaining = selected article blocks (paragraph fragments merged)
 * isReaderMode=1, title/baseUrl/rawHtml filled, readerWords/readerTime set.
 *
 * Faithful parity notes (verified against the Lua original):
 *  - STRIP_TAGS (script/style/noscript/svg/nav/footer/aside/header/iframe)
 *    toggle stripDepth; nav/header/footer/aside therefore NEVER reach the
 *    nav-container push branch — it is dead code in the source and omitted.
 *  - Containers are table.sort()ed by score DESC and then walked in that
 *    (sorted) order despite the "document order" comment; ties here break
 *    toward earlier document index for determinism (Lua sort is unstable).
 *  - parse("") host is "blank", so the header shows "BLANK"; the Lua
 *    `or "WEB PAGE"` fallback is unreachable and kept only as a comment.
 */
DocDocument* readability_distill(HttTokens* tokens, const char* rawTitle,
                                 const char* baseUrl);

#endif
