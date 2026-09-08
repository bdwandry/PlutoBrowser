/*
 * PlutoBrowser — readability.h
 * Port of Source/html/readability.lua (reference, 535 lines).
 *
 * Lua → C function map:
 *   Readability.distill(tokens, rawTitle, baseUrl)
 *       → readability_distill()  (fills a DocParseResult in place)
 *   (internal) trimStr              → rd_trim   (static)
 *   (internal) wordCount            → rd_word_count (static)
 *   (internal) isBareUrlText        → rd_is_bare_url_text (static)
 *   (internal) mergeParagraphFragments → rd_merge_paragraph_fragments (static)
 *   (internal) flushBlock/commitBlock/ensureBlock/addText
 *                                   → RdState helpers (static)
 *   (internal) pushContainer/popContainer → static
 *   (internal) scoring + best-container selection → static rd_score_and_pick
 *
 * Preserved reference semantics (verified against the verbatim Lua):
 *   - STRIP_TAGS: script, style, noscript, svg, nav, footer, aside, header,
 *     iframe — depth-counted; closing tags never go below 0.
 *   - Containers: article/main push (isContent=1); nav/header/footer/aside
 *     push (isNav=1). The ROOT container (idx 1) has isNav=false,
 *     isContent=false.
 *   - addText: strip \r\n\t → ' ' outside pre; skip bare-URL anchor text;
 *     underline = (href ~= nil) on the inline.
 *   - flushBlock: hr/image/input_field/input_submit pass through (image bumps
 *     imageCount); others need non-empty trimmed text UNLESS code_block;
 *     wordCount/textLen/linkWords accumulate on the container.
 *   - Scoring: score = wordCount + imageCount*30; linkDensity>0.5 → *0.2,
 *     >0.33 → *0.6; isContent → *3; isNav → *0.05. Best = highest score
 *     (Lua table.sort is stable for equal keys in 5.4+... actually Lua sorts
 *     are NOT guaranteed stable; ties are resolved by qsort order. The
 *     reference then walks "containers" in the SORTED order — C replicates
 *     the sort + walk; tie order may differ from Lua but only among equal
 *     scores, which produce the same set). Threshold = best.score * 0.15;
 *     include non-nav containers with score >= threshold (best always).
 *   - Fallback: nothing picked → all blocks from non-nav containers
 *     (in sorted-container order, matching the Lua loop over `containers`).
 *   - mergeParagraphFragments: join paragraph→paragraph when the previous
 *     accumulated text does NOT end in .?!: (with trailing spaces), the
 *     combined length ≤ 200, and the incoming text is ≤ 40 words.
 *   - Reading time: max(1, ceil(totalWords/180)) → "%d min read (%d words)".
 *   - Doc assembly: [reader_header(host, title, readingTime),
 *     heading(level=1, bold title inline), hr, ...merged blocks].
 *   - img: src = src || data-src || first srcset entry; alt = alt || title ||
 *     "Image" (empty alt → "Image"); w/h = tonumber || 160/80; clamps
 *     360/180; skips src containing "tracking" or "beacon"; resolves via
 *     URL.resolve; carries currentHref.
 *   - C extension (no observable difference): tokens are consumed from the
 *     TokenizeResult array; blocks are appended to the DocParseResult using
 *     document.c's arena allocator — strings live in that arena, freed by
 *     document_free.
 */
#ifndef PLUTO_READABILITY_H
#define PLUTO_READABILITY_H

#include "html/document.h"
#include "html/tokenizer.h"

/* Distill `tr.tokens` into `out` (reader-mode blocks). rawTitle/baseUrl may
 * be NULL. Returns 0 on success, -1 on allocation failure (out->parseError
 * set; caller treats it as the Lua error path). */
int readability_distill(const TokenizeResult *tr, const char *rawTitle,
                        const char *baseUrl, DocParseResult *out);

/* Free the distill output (block arrays + arena). document_free also works
 * (the readability arena matches DocArena's layout); this standalone form
 * exists for harnesses that don't link document.c. */
void readability_free_result(DocParseResult *doc);

#endif /* PLUTO_READABILITY_H */
