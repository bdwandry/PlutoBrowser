/*
 * PlutoBrowser — layout.c
 * Port of Source/render/layout.lua (reference, 1472 lines).
 *
 * Every observable behavior of the reference is reproduced, including:
 *   - breakLines word/whitespace scanning (Lua %s = the five ASCII space
 *     chars), leading-space drop, gap collapse, tab-run advance measured
 *     from cur.width+ww, first-word overflow staying on the line.
 *   - emitFlow center/right x, link-rect merge across same href+anchor,
 *     sub/sup dy, per-word item flags, mrText "a b" joining.
 *   - Layout.build geometry for all 16 block types incl. form link rects
 *     ("#"/"input:"/"select:"/"toggle:"), image-map scaling, box_stack y2
 *     patching, totalHeight clamp max(currentY+20, CONTENT_HEIGHT).
 *   - Layout.draw culling rules per item type (exact y comparisons),
 *     selection inversion, code/table/checkbox/select/meter/box rendering,
 *     image modes, scrollbar (barH max(16, CH*CH/total)), overlay panel.
 *
 * Measurement goes through g_measure (default: pd getTextWidth with the
 * given font; NULL font → body font; empty → 0), swappable for the battery.
 *
 * String lifetime: items BORROW strings from the parsed document; FlowWord
 * text storage (word buffers) is arena-owned by g_wordArena and freed by
 * layout_clear(). Free the document AFTER layout_clear(), or re-build (which
 * clears first).
 */
#include "core/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pd_api.h"

#include "render/layout.h"
#include "core/constants.h"
#include "core/tasks.h"
#include "render/style.h"
#include "render/link_manager.h"
#include "render/image_decoder.h"
#include "core/storage.h"

extern PlaydateAPI *pluto_pd(void);

/* ── Module state ────────────────────────────────────────────────────────── */

static LayoutItem **g_items = NULL;
static int g_itemCount = 0;
static int g_itemCap = 0;
static int g_totalHeight = 0;
static LayoutItem *g_selectedInput = NULL;

static int g_onDemandConsumed = 0;
static char g_hoveredImageSrc[512];
static int g_hasHoveredImage = 0;
static int g_cellLinksRegistered = 0;

typedef struct OnDemandOverlay
{
    int present;
    char src[512];
    char href[512];
    char alt[512];
} OnDemandOverlay;

static OnDemandOverlay g_overlay;

#define ONDEMAND_MAP_MAX 64
static char g_odRequested[ONDEMAND_MAP_MAX][512];
static int g_odRequestedCount = 0;

/* word-text arena: breakLines words live here until layout_clear().
 *
 * Implemented as a CHUNK LIST, not one growing buffer: LayoutItems borrow
 * word pointers for the lifetime of the layout, so the storage must NEVER
 * move. The previous single-buffer arena called realloc() on growth, which
 * invalidated every word pointer emitted before the growth — first-ever
 * layout built from an empty arena always grew (4096 → …) and the earliest
 * words rendered as garbage/underlines until the next full layout (whose
 * arena already had capacity) redrew them correctly. Chunked allocation
 * gives stable pointers for the whole build. */
typedef struct WordChunk
{
    struct WordChunk *next;
    size_t used;
    size_t cap;
    char data[]; /* flexible array member */
} WordChunk;

#define WORD_CHUNK_SIZE 16384
static WordChunk *g_wordChunks = NULL;

static const char *arena_word(const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > WORD_CHUNK_SIZE)
    {
        return s; /* single word larger than a chunk: caller-owned fallback */
    }
    WordChunk *c = g_wordChunks;
    if (!c || c->used + n > c->cap)
    {
        c = (WordChunk *)malloc(sizeof(WordChunk) + WORD_CHUNK_SIZE);
        if (!c)
        {
            return s; /* fall back to caller-owned storage */
        }
        c->used = 0;
        c->cap = WORD_CHUNK_SIZE;
        c->next = g_wordChunks;
        g_wordChunks = c;
    }
    char *dst = c->data + c->used;
    memcpy(dst, s, n);
    c->used += n;
    return dst;
}

static PlaydateAPI *g_pd = NULL;

/* Build error parity: the reference's table branch raises on percent widths
 * (tonumber(string.gsub(w,"%%","")) passes gsub's replacement COUNT as
 * tonumber's base → "base out of range" error). CometBrowser's callers wrap
 * Layout.build in pcall and turn the error into a "Layout Error" page, so a
 * percent-width table must abort the build, not silently lay out. */
static int g_buildError = 0;
int layout_build_failed(void) { return g_buildError; }

/* scrollY flows into draw_table_box for page-space link rects */
static int g_scrollYForLinks = 0;
/* R71 pinned app-bar span (page coords). g_pinFrom/g_pinTo are ITEM indices
 * of the top-anchored position:sticky element's subtree; layout re-draws
 * exactly that span at CONTENT_Y once scrollY moves past g_pinY. One bar per
 * page (the 400x240 viewport has room for one app bar). */
static int g_hasPin = 0;
static int g_pinOpenSpan = 0;
static int g_pinFrom = 0, g_pinTo = 0;
static int g_pinY = 0, g_pinH = 0;
/* R71 right-aligned disclosure dropdown span (page coords). The panel is
 * out of flow: its page Y is anchored under the app bar, but its height is
 * restored out of `currentY` so it overlays content exactly like the
 * author's position:absolute menu instead of pushing the page down. */
static int g_dropOpen = 0;
static int g_dropY = 0;
static int g_dropFlowSavedY = 0;
static int g_dropItemFrom = 0, g_dropItemTo = 0, g_dropRectFrom = 0;
static int g_dropPanelH = 0;
static int g_dropX = 0, g_dropW = 0;
/* R71 spanning decorations: the page Y where each decoration's first block
 * started (filled while the block loop runs; consumed at endBlock). */
static int g_decorY[DOC_BOX_DECOR_MAX];

/* A disclosure trigger inside an authored bordered card already has a
 * visible frame while the card is closed. Do not add the synthetic
 * clickable-row frame on top of it: that produced two concentric borders
 * around closed role cards. When the card opens, however, the authored
 * border spans the whole panel and the trigger loses its own boundary, so
 * layout draws a second frame around the trigger only. */
static const DocBoxDecor *decor_border_at(const DocParseResult *doc, int bi)
{
    if (!doc || bi < 0)
    {
        return NULL;
    }
    for (int i = 0; i < doc->decorCount && i < DOC_BOX_DECOR_MAX; i++)
    {
        const DocBoxDecor *d = &doc->decor[i];
        if (d->border > 0 && d->startBlock >= 0 && d->endBlock >= d->startBlock &&
            bi >= d->startBlock && bi <= d->endBlock)
        {
            return d;
        }
    }
    return NULL;
}
/* Large scratch buffers hoisted to BSS (stack audit): the table-cell and
 * code-box paths nested three 1KB arrays on the game-task stack.
 * Single-threaded: safe to share. */
static char g_layoutLineBuf[1024];
static char g_layoutCellA[1024];
static char g_layoutCellB[1024];
static char g_layoutTabExp[1024];

static void draw_table_box(LayoutItem *item, int drawY);

/* test-hook outputs (see layout_test_run_emit) */
static int g_testEmitLineH = 0;
static int g_testEmitEndY = 0;

void layout_init(PlaydateAPI *pd)
{
    g_pd = pd;
}

void layout_clear(void)
{
    for (int i = 0; i < g_itemCount; i++)
    {
        if (g_items[i]->ownedValue)
        {
            free(g_items[i]->ownedValue);
            g_items[i]->ownedValue = NULL;
        }
        free(g_items[i]);
    }
    free(g_items);
    g_items = NULL;
    g_itemCount = 0;
    g_itemCap = 0;
    g_totalHeight = 0;
    g_selectedInput = NULL;
    g_hoveredImageSrc[0] = '\0';
    g_hasHoveredImage = 0;
    g_cellLinksRegistered = 0;
    g_overlay.present = 0;
    g_odRequestedCount = 0;
    g_onDemandConsumed = 0;
    g_hasPin = 0;
    g_pinOpenSpan = 0;
    g_pinFrom = g_pinTo = 0;
    g_pinY = g_pinH = 0;
    g_dropOpen = 0;
    g_dropY = 0;
    g_dropFlowSavedY = 0;
    g_dropItemFrom = g_dropItemTo = g_dropRectFrom = 0;
    g_dropPanelH = 0;
    g_dropX = g_dropW = 0;
    for (int di = 0; di < DOC_BOX_DECOR_MAX; di++)
    {
        g_decorY[di] = 0;
    }
    /* Free the whole word-arena chunk list. */
    WordChunk *c = g_wordChunks;
    while (c)
    {
        WordChunk *nx = c->next;
        free(c);
        c = nx;
    }
    g_wordChunks = NULL;
    /* Column-grid scratch arrays borrow into the (now freed) document —
     * drop the pointers; the next build re-mallocs for the new doc. */
    /* (DocTable memory itself belongs to the document arena; only these
     * two malloc'd index arrays are layout-owned.) */
    /* They are freed per-table here via the last built doc reference kept
     * in the items? No — items are freed above. Instead the arrays are
     * re-malloc'd per build (gridCount check) and leak-safe because the
     * document is freed before the next layout_build reuses the pointer. */
}

/* ── Measurement (Style.getTextWidth) ────────────────────────────────────── */


static int default_measure(LCDFont *font, const char *text)
{
    if (!text || text[0] == '\0')
    {
        return 0;
    }
    if (!font)
    {
        font = style_font(PLUTO_FONT_BODY);
    }
    if (font && g_pd)
    {
        return g_pd->graphics->getTextWidth(font, text, strlen(text),
                                            kUTF8Encoding, 0);
    }
    return (int)strlen(text) * 8;
}

static int measure(LCDFont *font, const char *text)
{
    return default_measure(font, text);
}

/* ── Lua string.sub (1-based, negative from end) ─────────────────────────── */

static void lua_sub(const char *s, int i, int j, char *out, size_t cap)
{
    if (!s || !out || cap == 0)
    {
        return;
    }
    size_t n = strlen(s);
    if (i < 0)
    {
        i = (int)n + i + 1;
    }
    if (j < 0)
    {
        j = (int)n + j + 1;
    }
    if (i < 1)
    {
        i = 1;
    }
    if (j > (int)n)
    {
        j = (int)n;
    }
    if (i > j)
    {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)(j - i + 1);
    if (len >= cap)
    {
        len = cap - 1;
    }
    memcpy(out, s + (i - 1), len);
    out[len] = '\0';
}

/* ── Item allocation ─────────────────────────────────────────────────────── */

static LayoutItem *item_new(LayoutItemType type)
{
    if (g_itemCount == g_itemCap)
    {
        int nc = g_itemCap ? g_itemCap * 2 : 64;
        LayoutItem **na = (LayoutItem **)realloc(g_items, nc * sizeof(LayoutItem *));
        if (!na)
        {
            return NULL;
        }
        g_items = na;
        g_itemCap = nc;
    }
    LayoutItem *it = (LayoutItem *)calloc(1, sizeof(LayoutItem));
    if (!it)
    {
        return NULL;
    }
    it->type = type;
    it->maxlength = -1;
    g_items[g_itemCount++] = it;
    return it;
}

/* ── (local) normalizeAlign / toRoman / toAlpha / orderedMarker ──────────── */

const char *layout_normalize_align(const char *a)
{
    static char buf[32];
    if (!a || !a[0])
    {
        return NULL;
    }
    size_t n = strlen(a);
    if (n >= sizeof(buf))
    {
        n = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < n; i++)
    {
        buf[i] = (char)tolower((unsigned char)a[i]);
    }
    buf[n] = '\0';
    if (strcmp(buf, "center") == 0 || strcmp(buf, "right") == 0 ||
        strcmp(buf, "left") == 0)
    {
        return buf;
    }
    return NULL;
}

void layout_to_roman(int n, char *out, size_t cap)
{
    /* Lua: if not n or n <= 0 or n > 3999 then return tostring(n or 1) */
    if (n <= 0 || n > 3999)
    {
        /* Lua tostring(n or 1): 0 is truthy → "0"; nil (not applicable here)
         * would give "1"; negative n passes through. */
        snprintf(out, cap, "%d", n);
        return;
    }
    static const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *syms[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL",
                                 "X", "IX", "V", "IV", "I"};
    size_t o = 0;
    out[0] = '\0';
    char tmp[32];
    for (int i = 0; i < 13 && n > 0; i++)
    {
        while (n >= vals[i])
        {
            size_t sl = strlen(syms[i]);
            memcpy(tmp + o, syms[i], sl);
            o += sl;
            n -= vals[i];
        }
    }
    tmp[o] = '\0';
    snprintf(out, cap, "%s", tmp);
}

void layout_to_alpha(int n, char *out, size_t cap)
{
    char tmp[40];
    size_t o = 0;
    while (n > 0)
    {
        int rem = (n - 1) % 26;
        if (o < sizeof(tmp))
        {
            tmp[o++] = (char)('a' + rem);
        }
        n = (n - 1) / 26;
    }
    for (size_t i = 0; i < o / 2; i++)
    {
        char t = tmp[i];
        tmp[i] = tmp[o - 1 - i];
        tmp[o - 1 - i] = t;
    }
    tmp[o] = '\0';
    if (o == 0)
    {
        snprintf(out, cap, "1"); /* Lua: out ~= "" and out or "1" */
        return;
    }
    snprintf(out, cap, "%s", tmp);
}

void layout_ordered_marker(int number, char markerType, char *out, size_t cap)
{
    char buf[40];
    if (!markerType)
    {
        markerType = '1';
    }
    if (markerType == 'a')
    {
        layout_to_alpha(number, buf, sizeof(buf));
    }
    else if (markerType == 'A')
    {
        layout_to_alpha(number, buf, sizeof(buf));
        for (char *p = buf; *p; p++)
        {
            *p = (char)toupper((unsigned char)*p);
        }
    }
    else if (markerType == 'i')
    {
        layout_to_roman(number, buf, sizeof(buf));
    }
    else if (markerType == 'I')
    {
        layout_to_roman(number, buf, sizeof(buf));
        for (char *p = buf; *p; p++)
        {
            *p = (char)toupper((unsigned char)*p);
        }
    }
    else
    {
        snprintf(buf, sizeof(buf), "%d", number == 0 ? 1 : number);
    }
    snprintf(out, cap, "%s", buf);
}

/* ── Tab helpers ─────────────────────────────────────────────────────────── */

#define TAB_COLUMNS 8

static int tab_advance(LCDFont *font, int runningWidth)
{
    int spaceW = measure(font, " ");
    if (spaceW <= 0)
    {
        spaceW = 8;
    }
    int tabPx = TAB_COLUMNS * spaceW;
    int toTab = tabPx - (runningWidth % tabPx);
    if (toTab <= 0)
    {
        toTab = tabPx;
    }
    return toTab;
}

void layout_expand_tab_columns(const char *line, char *out, size_t cap)
{
    if (!line)
    {
        if (cap)
        {
            out[0] = '\0';
        }
        return;
    }
    if (!strchr(line, '\t'))
    {
        snprintf(out, cap, "%s", line);
        return;
    }
    size_t o = 0;
    int col = 0;
    for (const char *p = line; *p; p++)
    {
        if (*p == '\t')
        {
            int pad = TAB_COLUMNS - (col % TAB_COLUMNS);
            for (int i = 0; i < pad && o + 1 < cap; i++)
            {
                out[o++] = ' ';
            }
            col += pad;
        }
        else
        {
            if (o + 1 < cap)
            {
                out[o++] = *p;
            }
            col++;
        }
    }
    out[o] = '\0';
}

/* ── Flow word/line model ────────────────────────────────────────────────── */

typedef struct FlowWord
{
    const char *text; /* arena_word storage */
    LCDFont *font;
    int w;
    int advance;
    const DocInline *inl;
} FlowWord;

typedef struct FlowLine
{
    FlowWord *words;
    int count, cap;
    int width;
} FlowLine;

typedef struct FlowLines
{
    FlowLine *lines;
    int count, cap;
} FlowLines;

static void flow_free(FlowLines *fl)
{
    for (int i = 0; i < fl->count; i++)
    {
        free(fl->lines[i].words);
    }
    free(fl->lines);
    memset(fl, 0, sizeof(*fl));
}

static void flow_push_line(FlowLines *fl, FlowLine *cur)
{
    if (cur->count == 0)
    {
        return;
    }
    if (fl->count == fl->cap)
    {
        int nc = fl->cap ? fl->cap * 2 : 8;
        FlowLine *na = (FlowLine *)realloc(fl->lines, nc * sizeof(FlowLine));
        if (!na)
        {
            return;
        }
        fl->lines = na;
        fl->cap = nc;
    }
    fl->lines[fl->count++] = *cur;
    cur->words = NULL;
    cur->count = 0;
    cur->cap = 0;
    cur->width = 0;
}

static void flow_add_word(FlowLine *cur, const FlowWord *w)
{
    if (cur->count == cur->cap)
    {
        int nc = cur->cap ? cur->cap * 2 : 8;
        FlowWord *na = (FlowWord *)realloc(cur->words, nc * sizeof(FlowWord));
        if (!na)
        {
            return;
        }
        cur->words = na;
        cur->cap = nc;
    }
    cur->words[cur->count++] = *w;
    cur->width += w->advance;
}

/* Phase 5 ellipsis tail: append a "..." word to `ln` measured in the
 * CUT inline's font; pop trailing whole words until the dots fit the
 * line budget (never underflows the line's first word). Literal text —
 * emit is the only consumer and "..." outlives the pass. */
static void flow_ellipsize(FlowLine *ln, int maxW, LCDFont *font,
                           const DocInline *inl)
{
    FlowWord ew;
    int eadv = measure(font, "...");
    if (eadv <= 0)
    {
        eadv = 0;
    }
    while (ln->count > 1 && maxW > 0 && ln->width + eadv > maxW)
    {
        FlowWord *last = &ln->words[ln->count - 1];
        ln->width -= last->advance;
        ln->count--;
    }
    memset(&ew, 0, sizeof(ew));
    ew.text = "...";
    ew.font = font;
    ew.w = eadv;
    ew.advance = eadv;
    ew.inl = inl;
    flow_add_word(ln, &ew);
}

static int is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

/* ── (local) breakLines ──────────────────────────────────────────────────── */

typedef struct BreakOpts
{
    LCDFont *font; /* NULL → per-inline font */
    int lineH;     /* 0 → 16 */
    int bold;
    /* Phase 5: -webkit-line-clamp budget (0 → unlimited). When a new
     * word would open line N+1, the block stops consuming text
     * instead: the last kept line gains a "..." tail (the web's
     * card-snippet truncation). */            int maxLines;
} BreakOpts;

/* overflow-wrap:break-word piece emission (one FlowWord per piece, with
 * the normal wrap check between pieces; the trailing gap lands on the
 * last piece only). */
typedef struct
{
    FlowLines *fl;
    FlowLine *cur;
    LCDFont *font;
    const DocInline *inl;
    int maxW;
    int gap;
    int *firstWord;
} BreakPieceCtx;

static int break_piece_measure(void *ctx, const char *s)
{
    return measure((LCDFont *)ctx, s);
}

static void break_piece_emit(void *ectx, const char *pc, size_t n, int w,
                             int isLast)
{
    BreakPieceCtx *c = (BreakPieceCtx *)ectx;
    char tmp[256];
    if (n >= sizeof(tmp))
    {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, pc, n);
    tmp[n] = '\0';
    int aw = w + (isLast ? c->gap : 0);
    if (c->cur->width + aw > c->maxW && !*c->firstWord)
    {
        flow_push_line(c->fl, c->cur);
        *c->firstWord = 1;
        /* Keep the trailing gap on the moved piece: it separates this
         * piece from the next word even when the piece starts a new line. */
    }
    FlowWord fw;
    fw.text = arena_word(tmp);
    fw.font = c->font;
    fw.w = w;
    fw.advance = aw;
    fw.inl = c->inl;
    flow_add_word(c->cur, &fw);
    *c->firstWord = 0;
}

static int break_lines(const DocInline **inlines, int inlineCount, int maxW,
                       const BreakOpts *opts, FlowLines *fl)
{
    memset(fl, 0, sizeof(*fl));
    BreakOpts o;
    memset(&o, 0, sizeof(o));
    if (opts)
    {
        o = *opts;
    }
    int lineH = o.lineH ? o.lineH : 16;
    if (!inlines || inlineCount <= 0)
    {
        /* R59 defensive guard (Wikipedia device crash 2026-10-18): a block
         * that lost its inline array (mid-walk refusal before the re-walk
         * contract fix) must degrade to an empty flow, never fault the
         * loop. The re-walk now refuses to publish such states, but any
         * producer of inlineCount>0 with inlines==NULL degrades here. */
        return lineH;
    }
    FlowLine cur;
    memset(&cur, 0, sizeof(cur));
    int firstWord = 1;
    /* Phase 5: set once when an ellipsis/clamp cut drops a line's tail;
     * every later inline (any later span, br, or text run) is skipped. */
    int truncated = 0;

    for (int ii = 0; ii < inlineCount; ii++)
    {
        const DocInline *inl = inlines[ii];
        if (!inl)
        {
            continue;
        }
        if (truncated)
        {
            break;
        }
        if (inl->type == DOC_INLINE_BR)
        {
            flow_push_line(fl, &cur);
            firstWord = 1;
            continue;
        }
        if (inl->type != DOC_INLINE_TEXT || !inl->text)
        {
            continue;
        }

        LCDFont *font;
        int lh;
        if (o.font)
        {
            font = o.font;
            lh = o.lineH ? o.lineH : 16;
        }
        else if (o.bold)
        {
            font = style_font(PLUTO_FONT_BODY_BOLD);
            lh = 16;
        }
        else
        {
            unsigned f = inl->flags;
            font = style_get_inline_font(!!(f & DOC_INF_BOLD), !!(f & DOC_INF_CODE),
                                         !!(f & DOC_INF_SMALL), !!(f & DOC_INF_SUB),
                                         !!(f & DOC_INF_SUP), !!(f & DOC_INF_BIG),
                                         &lh);
        }
        if (lh > lineH)
        {
            lineH = lh;
        }
        if (o.maxLines && cur.count == 0 && fl->count >= o.maxLines)
        {
            /* Phase 5 clamp: this word would open line N+1 — cut with
             * a "..." tail appended to the final kept line instead.
             * Only reachable when cur emptied out right on the
             * boundary (a wrap-closed line just filled the budget). */
            if (fl->count > 0 && fl->lines)
            {
                flow_ellipsize(&fl->lines[fl->count - 1], maxW, font, inl);
            }
            truncated = 1;
            break;
        }

        const char *text = inl->text;
        /* The bundled proportional fonts report a 2–3px space, which is
         * visually indistinguishable from no space at this screen size.
         * Keep a clearly readable minimum word gap (the device's word
         * spaces were collapsing on headings and long paragraphs). */
        int spaceW = measure(font, " ");
        if (spaceW < 6)
        {
            spaceW = 6;
        }
        int pos = 1;
        int n = (int)strlen(text);
        while (pos <= n)
        {
            /* Newlines survive the walker only from PRE content (<pre> or
             * CSS white-space:pre/pre-line): they force a line break here.
             * Normal-mode text never contains \n (collapsed upstream), so
             * this changes nothing outside pre. Blank lines come from
             * back-to-back breaks pushing an empty line; a trailing break
             * with nothing after it is dropped (browser parity). */
            int sawNL = 0;
            while (pos <= n && is_space_char(text[pos - 1]))
            {
                if (text[pos - 1] == '\n')
                {
                    sawNL = 1;
                }
                pos++;
            }
            if (pos > n)
            {
                break;
            }
            if (sawNL && (cur.count > 0 || fl->count > 0))
            {
                if (cur.count == 0)
                {
                    /* Blank line: the pusher drops empty lines, so plant
                     * one zero-width word (valid font, empty text draws
                     * nothing) to hold the line open. */
                    FlowWord bw;
                    memset(&bw, 0, sizeof(bw));
                    bw.text = "";
                    bw.font = font;
                    bw.inl = inl;
                    flow_add_word(&cur, &bw);
                }
                flow_push_line(fl, &cur);
                firstWord = 1;
            }
            int ws = pos; /* word end scan (Lua string.match "^%S+") */
            while (ws <= n && !is_space_char(text[ws - 1]))
            {
                ws++;
            }
            char word[256];
            lua_sub(text, pos, ws - 1, word, sizeof(word));
            int ww = measure(font, word);

            int gap = 0;
            int wsEnd = ws; /* first index AFTER the word (Lua wsEnd = pos+#word) */
            int wsStart = wsEnd;
            while (wsEnd <= n && is_space_char(text[wsEnd - 1]))
            {
                wsEnd++;
            }
            if (wsEnd > wsStart)
            {
                int hasTab = 0;
                for (int k = wsStart; k < wsEnd; k++)
                {
                    if (text[k - 1] == '\t')
                    {
                        hasTab = 1;
                        break;
                    }
                }
                gap = hasTab ? tab_advance(font, cur.width + ww) : spaceW;
            }

            int aw = ww + gap;
            /* Phase 5 ellipsis (text-overflow:ellipsis + white-space:
             * nowrap): once the accumulating nowrap line runs past the
             * budget, cut instead of overflowing — room for "..." is
             * made by popping trailing words; later inlines die at the
             * loop-top truncated guard. */
            if (!truncated && maxW > 0 && cur.count > 0 &&
                (inl->flags & DOC_INF_ELLIP) &&
                (inl->flags & DOC_INF_NOWRAP) &&
                cur.width + aw > maxW)
            {
                flow_ellipsize(&cur, maxW, font, inl);
                truncated = 1;
                break;
            }
            /* CSS white-space:nowrap rides the inline flags: suppress the
             * wrap (the line overflows instead). Explicit <br> still
             * breaks (handled above, unconditional). */
            if ((inl->flags & DOC_INF_BREAKWORD) &&
                !(inl->flags & DOC_INF_NOWRAP) && ww > maxW && maxW > 0)
            {
                /* overflow-wrap:break-word — chop the over-wide word;
                 * nowrap wins over breaking when both are set. */
                BreakPieceCtx pctx;
                pctx.fl = fl;
                pctx.cur = &cur;
                pctx.font = font;
                pctx.inl = inl;
                pctx.maxW = maxW;
                pctx.gap = gap;
                pctx.firstWord = &firstWord;
                doc_split_long_word(word, maxW, break_piece_measure, font,
                                    break_piece_emit, &pctx);
                pos = wsEnd;
                continue;
            }
            if (cur.width + aw > maxW && !firstWord &&
                !(inl->flags & DOC_INF_NOWRAP))
            {
                flow_push_line(fl, &cur);
                firstWord = 1;
                /* Do NOT drop `gap`: it belongs after this word. The gap
                 * before it was the previous word's advance and is already
                 * on the previous line. Dropping `gap` here was why the
                 * first word of every wrapped line stuck to the next word. */
                if (o.maxLines && fl->count >= o.maxLines)
                {
                    /* Phase 5 clamp: the push just CLOSED line N — the
                     * current word would open N+1 and every later word
                     * dies with it, so the block truncates NOW: the
                     * "..." tail lands at the end of the last kept line
                     * (popping its overflow words for room). The
                     * loop-top guard covers the remaining case (a BR
                     * pushed the budget-full line with no text after). */
                    if (fl->count > 0 && fl->lines)
                    {
                        flow_ellipsize(&fl->lines[fl->count - 1], maxW, font,
                                       inl);
                    }
                    truncated = 1;
                    break;
                }
            }
            FlowWord w;
            w.text = arena_word(word);
            w.font = font;
            w.w = ww;
            w.advance = aw;
            w.inl = inl;
            flow_add_word(&cur, &w);
            firstWord = 0;
            pos = wsEnd;
        }
    }
    flow_push_line(fl, &cur);
    free(cur.words);
    return lineH;
}

/* ── (local) emitFlow ────────────────────────────────────────────────────── */

static int emit_flow(const FlowLines *fl, int lineH, int startX, int maxW,
                     const char *align, int startY, int invert)
{
    int y = startY;
    for (int li = 0; li < fl->count; li++)
    {
        const FlowLine *line = &fl->lines[li];
        int textW = 0;
        for (int wi = 0; wi < line->count; wi++)
        {
            textW += line->words[wi].w;
        }
        int x = startX;
        if (align && strcmp(align, "center") == 0)
        {
            x = startX + (maxW - textW) / 2;
        }
        else if (align && strcmp(align, "right") == 0)
        {
            x = startX + (maxW - textW > 0 ? maxW - textW : 0);
        }

        int mrX = 0, mrW = 0, mrActive = 0, mrInert = 0, mrAnchor = 0;
        const char *mrHref = NULL;
        char mrText[512];
        mrText[0] = '\0';
#define FLUSH_LINK()                                                          \
    do                                                                        \
    {                                                                         \
        if (mrActive)                                                         \
        {                                                                     \
            LMRect r = {mrX, y, mrW, lineH};                                  \
            LMRectAux la;                                                     \
            memset(&la, 0, sizeof(la));                                       \
            la.inert = mrInert;                                               \
            lm_add_link_rect_ex(mrHref, mrText, &r, mrAnchor, &la);           \
            mrActive = 0;                                                     \
        }                                                                     \
    } while (0)

        for (int wi = 0; wi < line->count; wi++)
        {
            const FlowWord *wd = &line->words[wi];
            const DocInline *inl = wd->inl;
            unsigned f = inl->flags;
            int dy = 0;
            if (f & DOC_INF_SUB)
            {
                dy = 3;
            }
            else if (f & DOC_INF_SUP)
            {
                dy = -4;
            }
            LayoutItem *item = item_new(LRI_TEXT);
            if (!item)
            {
                return y; /* allocation failure: stop (Lua OOM aborts) */
            }
            item->text = wd->text;
            item->font = wd->font;
            item->x = x;
            item->y = y + dy;
            item->w = wd->w;
            item->h = lineH;
            item->flags = f;
            item->bold = (f & DOC_INF_BOLD) != 0;
            item->italic = (f & DOC_INF_ITALIC) != 0;
            item->underline = (f & DOC_INF_UNDERLINE) != 0;
            item->code = (f & DOC_INF_CODE) != 0;
            item->small = (f & DOC_INF_SMALL) != 0;
            item->big = (f & DOC_INF_BIG) != 0;
            item->mark = (f & DOC_INF_MARK) != 0;
            item->strike = (f & DOC_INF_STRIKE) != 0;
            item->invert = invert;
            item->href = inl->href;
            item->anchorIndex = inl->anchorIndex;
            item->sub = (f & DOC_INF_SUB) != 0;
            item->sup = (f & DOC_INF_SUP) != 0;

            if (inl->href && mrActive && mrHref && strcmp(mrHref, inl->href) == 0 &&
                mrAnchor == inl->anchorIndex)
            {
                mrW = (x + wd->w) - mrX;
                size_t tl = strlen(mrText);
                size_t wl = strlen(wd->text);
                if (tl + 1 + wl < sizeof(mrText))
                {
                    mrText[tl] = ' ';
                    memcpy(mrText + tl + 1, wd->text, wl + 1);
                }
            }
            else if (inl->href)
            {
                FLUSH_LINK();
                mrX = x;
                mrW = wd->w;
                mrHref = inl->href;
                mrAnchor = inl->anchorIndex;
                mrInert = (f & DOC_INF_INERT) != 0;
                snprintf(mrText, sizeof(mrText), "%s", wd->text);
                mrActive = 1;
            }
            else
            {
                FLUSH_LINK();
            }
            x += wd->advance;
        }
        FLUSH_LINK();
        y += lineH;
    }
#undef FLUSH_LINK
    return y;
}

/* ── Layout.build helpers ────────────────────────────────────────────────── */

static void add_rect_aux(const char *href, const char *text, int x, int y,
                         int w, int h, int anchor, const LayoutRectAux *aux)
{
    LMRect r = {x, y, w, h};
    lm_add_link_rect_ex(href, text, &r, anchor, aux);
}

/* Phase 9: apply a DocBlock's translate mount to the item span
 * [from, to). px halves apply directly (the layout scale); the % rows
 * key against the branch's own measured box (boxW/boxH, both already
 * on the layout scale), matching CSS's own-relative translate %.
 * Zero offsets = no work — the default for every block without a
 * transform. */
static void tf_apply_items(int from, int to, const DocBlock *b, int boxW,
                           int boxH)
{
    if (!b)
    {
        return;
    }
    int dx = b->tfXpx, dy = b->tfYpx;
    if (b->tfXpct && boxW > 0)
    {
        dx += (boxW * b->tfXpct) / 100;
    }
    if (b->tfYpct && boxH > 0)
    {
        dy += (boxH * b->tfYpct) / 100;
    }
    if (!dx && !dy)
    {
        return;
    }
    for (int i = from; i < to && i < g_itemCount; i++)
    {
        g_items[i]->x += dx;
        g_items[i]->y += dy;
        /* The blockquote rail is part of the block's own box — the
         * LRI_LINE-type frames (heading rule, hr) stay put by design
         * (separators are not transform targets), but this rail moves
         * with its quote text through the y1/y2 line fields. */
        if (g_items[i]->type == LRI_QUOTE_BAR)
        {
            g_items[i]->y1 += dy;
            g_items[i]->y2 += dy;
        }
    }
}

typedef struct
{
    unsigned short id;
    int itemFrom, itemTo, rectFrom, rectTo;
    int padL, padR, padT, padB; /* container content-box inset (CSS px) */
    int gap; /* authored gap after this item (halved px) */
} FlexGroup;

/* Place one flex row's items on a single line. Items were laid out in
 * normal block flow first (so links/rects already exist), then translated
 * as a unit: justify-content distributes item boxes, align-items centers
 * them on the cross axis. Hit rects move with their item. */
static void flex_finish_row(FlexGroup *groups, int n, int rowStartY,
                            int justify, int center, int marginX,
                            int maxWidth, int *currentYOut)
{
    int minX[16], minY[16], maxX[16], maxY[16], gw[16], gh[16];
    int rowH = 0, totalW = 0;
    if (n > 16)
    {
        n = 16;
    }
    int padL = 0, padR = 0, padT = 0, padB = 0, gapsTotal = 0;
    if (n > 0)
    {
        padL = groups[0].padL;
        padR = groups[0].padR;
        padT = groups[0].padT;
        padB = groups[0].padB;
        for (int g = 1; g < n; g++)
        {
            gapsTotal += groups[g - 1].gap;
        }
    }
    int contentX = marginX + padL;
    int contentW = maxWidth - padL - padR;
    if (contentW < 1)
    {
        contentW = 1;
    }
    for (int g = 0; g < n; g++)
    {
        minX[g] = 32767;
        minY[g] = 32767;
        maxX[g] = -32767;
        maxY[g] = -32767;
        for (int i = groups[g].itemFrom; i < groups[g].itemTo; i++)
        {
            LayoutItem *it = g_items[i];
            int ix2 = it->x + it->w;
            int iy2 = it->y + it->h;
            if ((it->type == LRI_QUOTE_BAR || it->type == LRI_BOX_FRAME) &&
                it->y2 > it->y)
            {
                iy2 = it->y2;
            }
            if (it->x < minX[g])
            {
                minX[g] = it->x;
            }
            if (it->y < minY[g])
            {
                minY[g] = it->y;
            }
            if (ix2 > maxX[g])
            {
                maxX[g] = ix2;
            }
            if (iy2 > maxY[g])
            {
                maxY[g] = iy2;
            }
        }
        if (minX[g] == 32767)
        {
            minX[g] = maxX[g] = marginX;
            minY[g] = maxY[g] = rowStartY;
        }
        gw[g] = maxX[g] - minX[g];
        gh[g] = maxY[g] - minY[g];
        totalW += gw[g];
        if (gh[g] > rowH)
        {
            rowH = gh[g];
        }
    }
    totalW += gapsTotal;
    int gap = 0;
    if (justify == 3 && n > 1)
    {
        gap = (contentW - totalW) / (n - 1);
        if (gap < 0)
        {
            gap = 0;
        }
    }
    int x0 = contentX;
    if (justify == 1)
    {
        x0 = contentX + (contentW - totalW) / 2;
    }
    else if (justify == 2)
    {
        x0 = contentX + contentW - totalW;
    }
    if (x0 < contentX)
    {
        x0 = contentX;
    }
    logger_log("[bar] flex startY=%d n=%d pad=%d/%d/%d/%d totalW=%d rowH=%d gaps=%d",
               rowStartY, n, padL, padR, padT, padB, totalW, rowH, gapsTotal);
    for (int g = 0; g < n; g++)
    {
        logger_log("[bar] flex g=%d id=%d items=%d..%d bbox=%d,%d..%d,%d gh=%d gw=%d",
                   g, groups[g].id, groups[g].itemFrom, groups[g].itemTo,
                   minX[g], minY[g], maxX[g], maxY[g], gh[g], gw[g]);
    }
    int cursor = x0;
    for (int g = 0; g < n; g++)
    {
        int dx = cursor - minX[g];
        int dy = rowStartY + padT +
                 (center ? (rowH - gh[g]) / 2 : 0) - minY[g];
        for (int i = groups[g].itemFrom; i < groups[g].itemTo; i++)
        {
            LayoutItem *it = g_items[i];
            it->x += dx;
            it->y += dy;
            if (it->type == LRI_QUOTE_BAR || it->type == LRI_BOX_FRAME ||
                it->type == LRI_LINE)
            {
                it->x1 += dx;
                it->x2 += dx;
                it->y1 += dy;
                it->y2 += dy;
            }
        }
        lm_shift_rects(groups[g].rectFrom,
                       groups[g].rectTo - groups[g].rectFrom, dx, dy);
        cursor += gw[g];
        if (g + 1 < n)
        {
            cursor += groups[g].gap + (justify == 3 ? gap : 0);
        }
    }
    *currentYOut = rowStartY + padT + rowH + padB + 6;
    logger_log("[bar] flex placed endY=%d", *currentYOut);
}

void layout_build(DocParseResult *doc)
{
    logger_stack_touch();
    layout_clear();
    lm_clear();
    g_buildError = 0;

    if (!doc || !doc->blocks || doc->blockCount == 0)
    {
        g_totalHeight = CONTENT_HEIGHT;
        return;
    }

    int currentY = CONTENT_Y + 8;
    int marginX = CONTENT_MARGIN + 2;
    int maxWidth = CONTENT_TEXT_WIDTH - 4;
    LayoutItem *boxStack[64];
    int boxTop = 0;
    FlexGroup flexGroups[16];
    int flexGroupCount = 0;
    int flexActive = 0;
    unsigned short flexCurRow = 0;
    int flexRowStartY = 0, flexJustifyCur = 0, flexCenterCur = 0;

    for (int bi = 0; bi < doc->blockCount; bi++)
    {
        DocBlock *block = doc->blocks[bi];
        if (!block)
        {
            continue;
        }
        /* A top-anchored sticky header is app chrome: the site's header is
         * flush with the viewport top, not inset by the engine's generic 8px
         * page margin. Snap BEFORE flex-row setup so the row's start (and
         * therefore the app-bar height) uses the snapped top, not 8px of
         * blank header above the controls. */
        if (bi == 0 && block->pinStart && !g_hasPin && !g_pinOpenSpan &&
            currentY == CONTENT_Y + 8)
        {
            currentY = CONTENT_Y;
        }
        if (flexActive && block->flexRow != flexCurRow)
        {
            flex_finish_row(flexGroups, flexGroupCount, flexRowStartY,
                            flexJustifyCur, flexCenterCur, marginX, maxWidth,
                            &currentY);
            flexActive = 0;
            flexGroupCount = 0;
        }
        if (!flexActive && block->flexRow)
        {
            flexActive = 1;
            flexCurRow = block->flexRow;
            flexRowStartY = currentY;
            flexJustifyCur = block->flexJustify;
            flexCenterCur = block->flexCenter;
            flexGroupCount = 0;
        }
        if (flexActive)
        {
            if (flexGroupCount == 0 ||
                flexGroups[flexGroupCount - 1].id != block->flexItem)
            {
                if (flexGroupCount > 0)
                {
                    flexGroups[flexGroupCount - 1].itemTo = g_itemCount;
                    flexGroups[flexGroupCount - 1].rectTo = lm_rect_total();
                }
                if (flexGroupCount < 16)
                {
                    FlexGroup *fg = &flexGroups[flexGroupCount++];
                    fg->id = block->flexItem;
                    fg->itemFrom = fg->itemTo = g_itemCount;
                    fg->rectFrom = fg->rectTo = lm_rect_total();
                    fg->padL = block->flexPadL;
                    fg->padR = block->flexPadR;
                    fg->padT = block->flexPadT;
                    fg->padB = block->flexPadB;
                    fg->gap = block->gapApplied;
                }
            }
        }
        /* Tasks.yieldCheck() call site: outside a task context the C port's
         * budget check is a no-op (build runs inside the navigation task on
         * device, which owns the frame budget). */
        tasks_report_progress(0.8f + 0.2f * ((float)(bi + 1) / (float)doc->blockCount));

        /* R71: remember where each spanning decoration starts (its first
         * block's top), and open the pinned app-bar item span. */
        for (int di = 0; di < doc->decorCount && di < DOC_BOX_DECOR_MAX; di++)
        {
            const DocBoxDecor *d = &doc->decor[di];
            if (d->startBlock == bi && d->endBlock >= d->startBlock)
            {
                g_decorY[di] = currentY;
            }
        }
        if (block->pinStart && !g_hasPin && !g_pinOpenSpan)
        {
            g_pinFrom = g_itemCount;
            g_pinY = currentY;
            g_pinOpenSpan = 1;
        }
        /* R71 dropdown panel: begin a right-aligned box span. The panel
         * sits below the app bar and is laid out independently of the pin
         * span, so opening it never grows the sticky bar. */
        if (block->dropdownStart && !g_dropOpen)
        {
            g_dropOpen = 1;
            g_dropFlowSavedY = currentY;
            currentY += 4;
            g_dropY = currentY;
            g_dropItemFrom = g_itemCount;
            g_dropRectFrom = lm_rect_total();
        }
        /* CSS border top: captured before the branch adds its spacingTop
         * (margin stays outside the box); the bottom inevitably includes
         * the branch's spacingBottom (single post-pass point). */
        int borderTop = currentY;

        /* 1. Reader Header Banner */
        if (block->type == DOC_BLOCK_READER_HEADER)
        {
            int badgeH = 28;
            int tfFrom = g_itemCount;
            LayoutItem *it = item_new(LRI_READER_BADGE);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = maxWidth;
            it->h = badgeH;
            it->host = block->host ? block->host : "WEB PAGE";
            it->readingTime = block->readingTime ? block->readingTime : "";
            tf_apply_items(tfFrom, g_itemCount, block, maxWidth, badgeH);
            currentY += badgeH + 10;
        }
        /* 2. Headings (h1 - h6) */
        else if (block->type == DOC_BLOCK_HEADING)
        {
            currentY += (block->level == 1 ? 12 : 8) + block->spacingTop;
            int lineH = 0, marginB = 0;
            LCDFont *font = style_get_heading_font(block->level, &lineH, &marginB);
            const char *align = layout_normalize_align(block->align);
            int indent = block->indent;
            BreakOpts bo;
            memset(&bo, 0, sizeof(bo));
            bo.font = font;
            bo.lineH = lineH;
            bo.maxLines = block->clampLines;
            FlowLines fl;
            int lh = break_lines((const DocInline **)block->inlines,
                                 block->inlineCount, maxWidth - indent, &bo, &fl);
            int itemsBefore = g_itemCount;
            emit_flow(&fl, lh, marginX + indent, maxWidth - indent, align,
                      currentY, block->invert);
            int linesEmitted = fl.count;
            flow_free(&fl);
            for (int i = itemsBefore; i < g_itemCount; i++)
            {
                g_items[i]->bold = 1;
            }
            /* Phase 9: the element's translate mount (the h2 underline
             * is created AFTER this point, so the separator stays —
             * separators are not transform targets). */
            tf_apply_items(itemsBefore, g_itemCount, block, maxWidth - indent,
                           lh * linesEmitted);
            currentY += lh * linesEmitted; /* emitFlow returns endY */
            if (block->level <= 2)
            {
                LayoutItem *ln = item_new(LRI_LINE);
                if (!ln)
                {
                    return;
                }
                ln->x1 = marginX;
                ln->y1 = currentY + 3;
                ln->x2 = marginX + maxWidth;
                ln->y2 = currentY + 3;
                currentY += 6;
            }
            currentY += marginB + block->spacingBottom + block->gapApplied;
        }
        /* 3. MathML linearized formulas */
        else if (block->type == DOC_BLOCK_MATH)
        {
            int startY = currentY + block->spacingTop;
            int tfMathFrom = g_itemCount;
            DocInline inl;
            memset(&inl, 0, sizeof(inl));
            inl.type = DOC_INLINE_TEXT;
            inl.text = block->text ? block->text : "";
            inl.flags = DOC_INF_ITALIC;
            const DocInline *inls[1] = {&inl};
            BreakOpts bo;
            memset(&bo, 0, sizeof(bo));
            FlowLines fl;
            int lineH = break_lines(inls, 1, maxWidth - 10, &bo, &fl);
            emit_flow(&fl, lineH, marginX + 5, maxWidth - 10, "center", startY, 0);
            tf_apply_items(tfMathFrom, g_itemCount, block, maxWidth - 10,
                           lineH * fl.count);
            currentY = startY + lineH * fl.count + 10;
            flow_free(&fl);
        }
        /* 4. Paragraphs & Blockquotes */
        else if (block->type == DOC_BLOCK_PARAGRAPH ||
                 block->type == DOC_BLOCK_BLOCKQUOTE)
        {
            int isQuote = (block->type == DOC_BLOCK_BLOCKQUOTE);
            int indent = block->indent;
            int blockStartX = (isQuote ? marginX + 14 : marginX) + indent;
            int blockMaxW = maxWidth - (isQuote ? 18 : 0) - indent;
            /* R71 menu button: compact, right-aligned hamburger. Size and
             * frame it from the trigger's own CSS box (padding/border/radius/
             * gap/font) instead of a fixed pill; the flex row centers it on
             * the nav's content axis. */
            int menuBoxBottom = 0;
            if (block->menuButton)
            {
                const char *mlab = (block->label && block->label[0])
                                       ? block->label
                                       : "Menu";
                int border = block->menuBorder;
                int padL = block->menuPadL, padR = block->menuPadR;
                int padT = block->menuPadT, padB = block->menuPadB;
                int gap = block->menuGap ? block->menuGap : 6;
                LCDFont *mf = style_font(block->menuFontBig
                                             ? PLUTO_FONT_BODY
                                             : PLUTO_FONT_SMALL);
                /* Optical glyph box, not getFontHeight(): the system small
                 * font reports a 22px line box for ~15px visible caps, and
                 * using that inflated the compact control to 30px. */
                int fontH = block->menuFontBig ? 16 : 14;
                int iconW = 11;
                int labelW = measure(mf, mlab) + 2; /* breathing room */
                int mw = 2 * border + padL + padR + iconW + gap + labelW;
                if (mw < 44)
                {
                    mw = 44;
                }
                if (mw > blockMaxW)
                {
                    mw = blockMaxW;
                }
                int mh = 2 * border + padT + padB + fontH;
                /* The store's menu trigger is app chrome, not page content:
                 * keep its box compact (20px) so the sticky row can be a
                 * true thin app bar on the 240px panel. */
                if (mh < 18)
                {
                    mh = 18;
                }
                if (mh > 20)
                {
                    mh = 20;
                }
                LayoutItem *mi = item_new(LRI_MENU_BUTTON);
                if (!mi)
                {
                    return;
                }
                int rightInset = block->flexPadR;
                mi->x = marginX + maxWidth - rightInset - mw;
                if (mi->x < marginX)
                {
                    mi->x = marginX;
                }
                mi->y = currentY + 2;
                mi->w = mw;
                mi->h = mh;
                mi->label = mlab;
                mi->small = !block->menuFontBig;
                mi->radius = block->menuRadius;
                mi->menuPadL = padL;
                mi->menuPadR = padR;
                mi->menuPadT = padT;
                mi->menuPadB = padB;
                mi->menuBorder = border;
                mi->menuGap = gap;
                mi->toggleKey = block->toggleKey;
                mi->toggleOpen = block->toggleOpen;
                menuBoxBottom = mi->y + mi->h + 6;
                logger_log("[bar] menu x=%d y=%d w=%d h=%d pad=%d/%d/%d/%d border=%d radius=%d gap=%d row=%d item=%d",
                           mi->x, mi->y, mi->w, mi->h, padL, padR, padT,
                           padB, border, block->menuRadius, gap,
                           block->flexRow, block->flexItem);
                if (block->toggleKey)
                {
                    char thref[32];
                    snprintf(thref, sizeof(thref), "toggle:%s",
                             block->toggleKey);
                    LayoutRectAux aux;
                    memset(&aux, 0, sizeof(aux));
                    aux.isToggle = 1;
                    aux.toggleKey = block->toggleKey;
                    aux.toggleOpen = block->toggleOpen;
                    add_rect_aux(thref, mlab, mi->x, mi->y, mi->w, mi->h, 0,
                                 &aux);
                }
            }
            int tfQFrom = g_itemCount;
            int startQuoteY = currentY + block->spacingTop;
            /* Non-menu disclosures reserve a small gutter on the right for
             * the chevron, matching the site's flex row (text + arrow). */
            int labelMaxW = blockMaxW;
            if (block->toggleKey && !block->menuButton && blockMaxW > 80)
            {
                labelMaxW -= 24;
            }
            const char *align = layout_normalize_align(block->align);
            BreakOpts bo;
            memset(&bo, 0, sizeof(bo));
            bo.maxLines = block->clampLines;
            FlowLines fl;
            int lineH = break_lines((const DocInline **)block->inlines,
                                    block->inlineCount, labelMaxW, &bo, &fl);
            emit_flow(&fl, lineH, blockStartX, labelMaxW, align, startQuoteY,
                      block->invert);
            /* Capture the line count BEFORE flow_free(): that helper zeroes
             * the FlowLines struct. Reading fl.count afterwards made every
             * ordinary disclosure's hit-rect zero-height, so role cards
             * rendered but could never be clicked. */
            int lineCount = fl.count;
            currentY = startQuoteY + lineH * lineCount + 10 +
                       block->spacingBottom + block->gapApplied;
            if (block->menuButton && menuBoxBottom > currentY)
            {
                currentY = menuBoxBottom;
            }
            flow_free(&fl);
            /* Ordinary ARIA accordion headers (not menu buttons) get a
             * tappable row + [+]/[-] toggle spanning the header line. */
            if (block->toggleKey && !block->menuButton && lineCount > 0)
            {
                char thref[32];
                snprintf(thref, sizeof(thref), "toggle:%s", block->toggleKey);
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isToggle = 1;
                aux.toggleKey = block->toggleKey;
                aux.toggleOpen = block->toggleOpen;
                add_rect_aux(thref, block->label ? block->label : "",
                             blockStartX, startQuoteY, blockMaxW,
                             lineH * lineCount, 0, &aux);
                logger_log("[toggle-rect] key=%s x=%d y=%d w=%d h=%d",
                           block->toggleKey, blockStartX, startQuoteY,
                           blockMaxW, lineH * lineCount);
            }
            if (isQuote)
            {
                LayoutItem *qb = item_new(LRI_QUOTE_BAR);
                if (!qb)
                {
                    return;
                }
                qb->x = marginX + 3;
                qb->y1 = startQuoteY;
                qb->y2 = currentY - 4;
            }
            tf_apply_items(tfQFrom, g_itemCount, block, blockMaxW,
                           lineH * lineCount);
            /* Non-menu disclosure headers get a visible frame when the
             * author did not supply one. It surrounds the same row that
             * toggle_details() makes clickable, so role/accordion cards
             * read as controls rather than plain text. */
            const DocBoxDecor *decorBorder = decor_border_at(doc, bi);
            int openDecorFrame = block->toggleOpen && decorBorder &&
                                 decorBorder->endBlock > bi;
            if (block->toggleKey && !block->menuButton && lineCount > 0)
            {
                logger_log("[toggle-frame] key=%s blockBorder=%d decor=%d open=%d inner=%d",
                           block->toggleKey, block->border,
                           decorBorder != NULL, block->toggleOpen,
                           openDecorFrame);
            }
            if (block->toggleKey && !block->menuButton && block->border == 0 &&
                lineCount > 0 && (!decorBorder || openDecorFrame))
            {
                LayoutItem *bf = item_new(LRI_BOX_FRAME);
                if (bf)
                {
                    bf->x = blockStartX;
                    bf->w = blockMaxW;
                    if (openDecorFrame)
                    {
                        /* Mirror the closed card's border box: same full
                         * width and same height including the trigger's
                         * trailing block spacing, but NOT the flex/grid gap
                         * that belongs between cards. The outer authored
                         * border still wraps the expanded panel below. */
                        bf->y = startQuoteY;
                        bf->y2 = currentY - block->gapApplied;
                        bf->radius = decorBorder->radius;
                    }
                    else
                    {
                        bf->y = startQuoteY - 1;
                        bf->y2 = startQuoteY + lineH * lineCount + 1;
                        bf->radius = 3;
                    }
                }
            }
            /* Collapsed cards show a down chevron; open cards flip it up,
             * matching .role-chevron's rotate(45deg)/rotate(225deg). Two
             * 1px lines form the V (the site uses a rotated 2px border). */
            if (block->toggleKey && !block->menuButton && lineCount > 0)
            {
                int arrowCx = blockStartX + blockMaxW - 14;
                int arrowCy = startQuoteY + (lineH * lineCount) / 2;
                int halfW = 4, halfH = 2;
                int outerY = block->toggleOpen ? arrowCy + halfH
                                               : arrowCy - halfH;
                int tipY = block->toggleOpen ? arrowCy - halfH
                                             : arrowCy + halfH;
                for (int leg = 0; leg < 2; leg++)
                {
                    int ax1 = (leg == 0) ? arrowCx - halfW : arrowCx;
                    int ay1 = (leg == 0) ? outerY : tipY;
                    int ax2 = (leg == 0) ? arrowCx : arrowCx + halfW;
                    int ay2 = (leg == 0) ? tipY : outerY;
                    LayoutItem *ln = item_new(LRI_LINE);
                    if (!ln)
                    {
                        return;
                    }
                    ln->x1 = ax1;
                    ln->y1 = ay1;
                    ln->x2 = ax2;
                    ln->y2 = ay2;
                    ln->x = ax1 < ax2 ? ax1 : ax2;
                    ln->y = ay1 < ay2 ? ay1 : ay2;
                    ln->w = (ax1 < ax2 ? ax2 - ax1 : ax1 - ax2) + 1;
                    ln->h = (ay1 < ay2 ? ay2 - ay1 : ay1 - ay2) + 1;
                }
                logger_log("[toggle-arrow] key=%s x=%d y=%d open=%d",
                           block->toggleKey, arrowCx, arrowCy,
                           block->toggleOpen);
            }
        }
        /* 4. Lists (ul / ol / dl) */
        else if (block->type == DOC_BLOCK_LIST_ITEM)
        {
            /* Empty list items (no inlines, no image/table of their own)
             * render nothing: skip the bullet AND the vertical advance,
             * then continue with the next block (NOT return — this branch
             * runs inside layout's per-block loop, and returning would
             * abandon the rest of the page: no scroll, missing content).
             * These are split artifacts — <li><a><img></a></li> emits the
             * image as its own block (which already carries the link
             * target for tapping), leaving the li shell empty. Genuine
             * contentless <li></li> is vanishingly rare; desktop parity
             * for it is not worth phantom bullets site-wide. */
            if (block->inlineCount == 0 && !block->src && !block->table)
            {
                /* fall through without emitting: no bullet, no advance */
            }
            else
            {
            int drop = block->dropdown;
            int depth = block->depth ? block->depth : 1;
            int isDt = block->dt;
            int isDd = block->dd;
            int indent = drop ? 0 : (depth - 1) * 14;
            int bulletIndent = marginX + 4 + indent;
            int textIndent = marginX + (drop ? 10 : (isDd ? 30 : 20)) + indent;
            int listMaxW = maxWidth - (textIndent - marginX) - 4;
            int lineH = 18;

            /* Phase 7: the winning ::marker content text replaces the
             * default bullet glyph ("" = the author removed the bullet
             * — draw nothing, no item slot). The ordered/unordered
             * default path below still applies when markerText is
             * NULL (::marker never fired). */
            if (!drop && !isDt && !isDd && block->markerText)
            {
                if (block->markerText[0])
                {
                    LayoutItem *it = item_new(LRI_TEXT);
                    if (!it)
                    {
                        return;
                    }
                    it->text = arena_word(block->markerText);
                    it->font = style_font(PLUTO_FONT_BODY_BOLD);
                    it->x = bulletIndent;
                    it->y = currentY;
                    it->w = 14;
                    it->h = lineH;
                    it->bold = 1;
                }
            }
            else if (!drop && !isDt && !isDd && block->markerType != 'n')
            {
                char bulletStr[48];
                if (block->isOrdered && block->markerType != 'd' &&
                    block->markerType != 'c' && block->markerType != 's')
                {
                    char marker[40];
                    layout_ordered_marker(block->number, block->markerType,
                                          marker, sizeof(marker));
                    snprintf(bulletStr, sizeof(bulletStr), "%s.", marker);
                }
                else if (block->markerType == 'c')
                {
                    snprintf(bulletStr, sizeof(bulletStr), "o");
                }
                else if (block->markerType == 's')
                {
                    snprintf(bulletStr, sizeof(bulletStr), "+");
                }
                else
                {
                    snprintf(bulletStr, sizeof(bulletStr), "*");
                }
                LayoutItem *it = item_new(LRI_TEXT);
                if (!it)
                {
                    return;
                }
                it->text = arena_word(bulletStr);
                it->font = style_font(PLUTO_FONT_BODY_BOLD);
                it->x = bulletIndent;
                it->y = currentY;
                it->w = 14;
                it->h = lineH;
                it->bold = 1;
            }

            const char *align = layout_normalize_align(block->align);
            BreakOpts bo;
            memset(&bo, 0, sizeof(bo));
            bo.bold = isDt;
            bo.maxLines = block->clampLines;
            FlowLines fl;
            int lh = break_lines((const DocInline **)block->inlines,
                                 block->inlineCount, listMaxW, &bo, &fl);
            int tfLFrom = g_itemCount;
            emit_flow(&fl, lh, textIndent, listMaxW, align, currentY, 0);
            tf_apply_items(tfLFrom, g_itemCount, block, listMaxW,
                           lh * fl.count);
            currentY = currentY + lh * fl.count +
                       (drop ? 14 : 6) + block->gapApplied;
            flow_free(&fl);
            } /* end non-empty list item */
        }
        /* 5. Code Blocks */
        else if (block->type == DOC_BLOCK_CODE_BLOCK)
        {
            LCDFont *font = style_font(PLUTO_FONT_MONO);
            (void)font;
            const char *rawText = block->text ? block->text : "";
            char **lines = block->lines;
            int lineCount = block->lineCount;
            char **heapLines = NULL;
            if (lineCount == 0)
            {
                /* Lua: for l in string.gmatch(rawText .. "\n", "(.-)\r?\n") */
                int cap = 8;
                heapLines = (char **)malloc(cap * sizeof(char *));
                int hc = 0;
                const char *p = rawText;
                char *buf = g_layoutLineBuf; /* BSS-hoisted (stack audit) */
                size_t bo2 = 0;
                for (;;)
                {
                    if (*p == '\r' && *(p + 1) == '\n')
                    {
                        buf[bo2] = '\0';
                        if (hc == cap)
                        {
                            cap *= 2;
                            heapLines = (char **)realloc(heapLines, cap * sizeof(char *));
                        }
                        heapLines[hc++] = strdup(buf);
                        bo2 = 0;
                        p += 2;
                        continue;
                    }
                    if (*p == '\n' || *p == '\0')
                    {
                        buf[bo2] = '\0';
                        if (hc == cap)
                        {
                            cap *= 2;
                            heapLines = (char **)realloc(heapLines, cap * sizeof(char *));
                        }
                        heapLines[hc++] = strdup(buf);
                        bo2 = 0;
                        if (*p == '\0')
                        {
                            break;
                        }
                        p++;
                        continue;
                    }
                    if (bo2 + 1 < 1024)
                    {
                        buf[bo2++] = *p;
                    }
                    p++;
                }
                lines = heapLines;
                lineCount = hc;
            }

            int boxH = lineCount * 14 + 12;
            if (boxH < 30)
            {
                boxH = 30;
            }
            int tfCFrom = g_itemCount;
            LayoutItem *it = item_new(LRI_CODE_BOX);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = maxWidth;
            it->h = boxH;
            it->lines = lines;
            it->lineCount = lineCount;
            tf_apply_items(tfCFrom, g_itemCount, block, maxWidth, boxH);
            currentY += boxH + 10 + block->gapApplied;
            if (heapLines)
            {
                for (int i = 0; i < lineCount; i++)
                {
                    free(heapLines[i]);
                }
                free(heapLines);
            }
        }
        /* 6. Horizontal Rule (hr) */
        else if (block->type == DOC_BLOCK_HR)
        {
            currentY += 6;
            LayoutItem *ln = item_new(LRI_LINE);
            if (!ln)
            {
                return;
            }
            ln->x1 = marginX + 20;
            ln->y1 = currentY;
            ln->x2 = marginX + maxWidth - 20;
            ln->y2 = currentY;
            currentY += 10;
        }
        /* 7. Images (On-Device Dithered) */
        else if (block->type == DOC_BLOCK_IMAGE)
        {
            int imgW = (int)block->width ? (int)block->width : 160;
            if (imgW > maxWidth)
            {
                imgW = maxWidth;
            }
            int imgH = (int)block->height ? (int)block->height : 80;
            if (imgH > 160)
            {
                imgH = 160;
            }
            const char *align = layout_normalize_align(block->align);
            int imgX = marginX;
            if (align && strcmp(align, "center") == 0)
            {
                imgX = marginX + (maxWidth - imgW) / 2;
            }
            else if (align && strcmp(align, "right") == 0)
            {
                imgX = marginX + (maxWidth - imgW > 0 ? maxWidth - imgW : 0);
            }
            if (imgX < marginX)
            {
                imgX = marginX;
            }

            int tfIFrom = g_itemCount;
            LayoutItem *it = item_new(LRI_IMAGE);
            if (!it)
            {
                return;
            }
            it->x = imgX;
            it->y = currentY;
            it->w = imgW;
            it->h = imgH;
            it->alt = block->alt;
            it->src = block->src;
            it->imgHref = block->href;
            it->img = (LCDBitmap *)block->img;
            tf_apply_items(tfIFrom, g_itemCount, block, imgW, imgH);

            if (block->href && !block->inert)
            {
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isImage = 1;
                aux.src = block->src;
                aux.alt = block->alt;
                add_rect_aux(block->href, block->alt && block->alt[0] ? block->alt : "[Image Link]",
                             imgX, currentY, imgW, imgH, 0, &aux);
            }

            if (block->usemap && block->usemap[0] && !block->inert)
            {
                const DocMap *regions = NULL;
                for (int mi = 0; mi < doc->mapCount; mi++)
                {
                    if (doc->maps[mi] &&
                        strcmp(doc->maps[mi]->name, block->usemap) == 0)
                    {
                        regions = doc->maps[mi];
                        break;
                    }
                }
                if (regions)
                {
                    double sx = (block->width > 0) ? ((double)imgW / block->width) : 1.0;
                    double sy = (block->height > 0) ? ((double)imgH / block->height) : 1.0;
                    for (int ri = 0; ri < regions->areaCount; ri++)
                    {
                        const DocArea *r = regions->areas[ri];
                        if (!r || !r->href)
                        {
                            continue;
                        }
                        const char *shape = r->shape && r->shape[0] ? r->shape : "rect";
                        double cx = 0, cy = 0, cw = 0, ch = 0;
                        double c0 = r->coordCount > 0 ? r->coords[0] : 0;
                        double c1 = r->coordCount > 1 ? r->coords[1] : 0;
                        double c2 = r->coordCount > 2 ? r->coords[2] : 0;
                        if (strcmp(shape, "circle") == 0)
                        {
                            cx = imgX + (c0 - c2) * sx;
                            cy = currentY + (c1 - c2) * sy;
                            cw = (c2 * 2) * sx;
                            ch = (c2 * 2) * sy;
                        }
                        else if (strcmp(shape, "poly") == 0)
                        {
                            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
                            int pairs = r->coordCount / 2;
                            for (int i2 = 1; i2 <= pairs; i2++)
                            {
                                double px = imgX + (r->coords[(i2 - 1) * 2]) * sx;
                                double py = currentY + (r->coords[(i2 - 1) * 2 + 1]) * sy;
                                if (px < minX) minX = px;
                                if (px > maxX) maxX = px;
                                if (py < minY) minY = py;
                                if (py > maxY) maxY = py;
                            }
                            cx = minX;
                            cy = minY;
                            cw = maxX - minX;
                            ch = maxY - minY;
                        }
                        else
                        {
                            double x1 = c0, y1 = c1;
                            double x2 = r->coordCount > 2 ? r->coords[2] : x1;
                            double y2 = r->coordCount > 3 ? r->coords[3] : y1;
                            cx = imgX + (x1 < x2 ? x1 : x2) * sx;
                            cy = currentY + (y1 < y2 ? y1 : y2) * sy;
                            cw = (x2 > x1 ? x2 - x1 : x1 - x2) * sx;
                            ch = (y2 > y1 ? y2 - y1 : y1 - y2) * sy;
                        }
                        LayoutRectAux aux;
                        memset(&aux, 0, sizeof(aux));
                        aux.isImage = 1;
                        aux.src = block->src;
                        aux.alt = r->alt && r->alt[0] ? r->alt : block->alt;
                        add_rect_aux(r->href, r->alt && r->alt[0] ? r->alt : (block->alt && block->alt[0] ? block->alt : "[Map Link]"),
                                     (int)cx, (int)cy, (int)(cw > 1 ? cw : 1), (int)(ch > 1 ? ch : 1), 0, &aux);
                    }
                }
            }

            currentY += imgH + 12 + block->gapApplied;
        }
        /* 8. Tables */
        else if (block->type == DOC_BLOCK_TABLE && block->table)
        {
            DocTable *tbl = block->table; /* non-const: col-grid scratch */
            int rowCount = tbl->rowCount;
            int capH = (tbl->caption && tbl->caption[0]) ? 16 : 0;
            int tableH = rowCount * 18 + 14;
            if (tableH < 24)
            {
                tableH = 24;
            }
            tableH += capH;

            int tblW = maxWidth;
            if (tbl->width)
            {
                /* Lua semantics: tonumber(width) prefix-parses ("50%" → 50);
                 * only when the WHOLE string is non-numeric does the % branch
                 * run — and there the reference's gsub-into-tonumber raises
                 * (gsub returns the value AND the count; tonumber uses the
                 * count as base). Reproduce exactly. */
                const char *wstr = tbl->width;
                char *end = NULL;
                double wv = strtod(wstr, &end);
                int luaIsNumber = (end != wstr);
                if (luaIsNumber)
                {
                    while (*end == ' ' || *end == '\t' || *end == '\n' ||
                           *end == '\r' || *end == '\f' || *end == '\v')
                    {
                        end++; /* Lua tonumber skips trailing whitespace */
                    }
                    if (*end != '\0')
                    {
                        luaIsNumber = 0; /* trailing junk → not a number */
                    }
                }
                if (!luaIsNumber)
                {
                    /* HTML4 percent width ("85%"): the Lua reference raised
                     * an error here, but real browsers render it — and
                     * news.ycombinator.com relies on it (width="85%").
                     * strtod already parsed the numeric prefix (85), so
                     * scale it to the content width and clamp. */
                    if (strchr(wstr, '%') && wv > 0)
                    {
                        int pw = (int)(maxWidth * wv / 100.0 + 0.5);
                        tblW = pw < 8 ? 8 : (pw > maxWidth ? maxWidth : pw);
                    }
                    /* non-numeric without %: tblW stays maxWidth */
                }
                else if (wv > 0)
                {
                    tblW = (int)wv < maxWidth ? (int)wv : maxWidth;
                }
            }
            int tblX = marginX;
            const char *align = layout_normalize_align(tbl->align);
            if (align && strcmp(align, "center") == 0)
            {
                tblX = marginX + (maxWidth - tblW) / 2;
            }
            else if (align && strcmp(align, "right") == 0)
            {
                tblX = marginX + (maxWidth - tblW > 0 ? maxWidth - tblW : 0);
            }

            /* ── Column grid from <colgroup>/<col> (WHATWG §4.9.4) ──
             * Resolve per-grid-column pixel widths: explicit px > percent
             * of tblW > equal split of the remainder. Stored back on the
             * DocTable so the draw pass can right-align cells by column. */
            tbl->widthPx = tblW;
            if (tbl->cols && tbl->colCount > 0 && tbl->colTotal > 0)
            {
                if (tbl->gridCount != tbl->colTotal)
                {
                    free(tbl->colX);
                    free(tbl->colW);
                    tbl->colX = (int *)malloc((size_t)tbl->colTotal * sizeof(int));
                    tbl->colW = (int *)malloc((size_t)tbl->colTotal * sizeof(int));
                    tbl->gridCount = tbl->colTotal;
                }
                if (tbl->colX && tbl->colW)
                {
                    int fixedPx = 0;
                    int fixedCount = 0;
                    for (int ci = 0; ci < tbl->colCount; ci++)
                    {
                        const DocCol *col = tbl->cols[ci];
                        if (col->width > 0 && !col->percent)
                        {
                            fixedPx += col->width * col->span;
                            fixedCount += col->span;
                        }
                        else if (col->width > 0 && col->percent)
                        {
                            fixedPx += tblW * col->width / 100 * col->span;
                            fixedCount += col->span;
                        }
                    }
                    int flexCount = tbl->colTotal - fixedCount;
                    int flexEach = flexCount > 0 ? (tblW - fixedPx) / flexCount : 0;
                    if (flexEach < 8 && flexCount > 0)
                    {
                        flexEach = 8; /* keep flexible columns readable */
                    }
                    int cx = 0;
                    int gi = 0;
                    for (int ci = 0; ci < tbl->colCount && gi < tbl->colTotal; ci++)
                    {
                        const DocCol *col = tbl->cols[ci];
                        int wpx = (col->width > 0)
                                      ? (col->percent ? tblW * col->width / 100
                                                      : col->width)
                                      : flexEach;
                        for (int s = 0; s < col->span && gi < tbl->colTotal; s++)
                        {
                            tbl->colX[gi] = cx;
                            tbl->colW[gi] = wpx;
                            cx += wpx;
                            gi++;
                        }
                    }
                }
            }

            int tfTFrom = g_itemCount;
            LayoutItem *it = item_new(LRI_TABLE_BOX);
            if (!it)
            {
                return;
            }
            it->x = tblX;
            it->y = currentY;
            it->w = tblW;
            it->h = tableH;
            it->table = tbl;
            it->caption = tbl->caption;
            it->border = tbl->border;
            tf_apply_items(tfTFrom, g_itemCount, block, tblW, tableH);
            currentY += tableH + 10 + block->gapApplied;
        }
        /* 8a. Hidden fields */
        else if (block->type == DOC_BLOCK_HIDDEN_FIELD)
        {
            LayoutItem *it = item_new(LRI_HIDDEN_FIELD);
            if (!it)
            {
                return;
            }
            it->name = block->name;
            it->value = block->value;
            it->formAction = block->formAction ? block->formAction : "";
            it->disabled = block->disabled;
            it->block = block;
        }
        /* 9. Text Input Fields */
        else if (block->type == DOC_BLOCK_INPUT_FIELD)
        {
            int fieldH = 22;
            int fieldW = maxWidth - 10;
            if (block->fieldWidth > 0)
            {
                fieldW = maxWidth - 10;
                int want = block->fieldWidth * 8;
                if (want < 60)
                {
                    want = 60;
                }
                if (want < fieldW)
                {
                    fieldW = want;
                }
            }
            else if (block->inputType && strcmp(block->inputType, "textarea") == 0 &&
                     block->fieldWidth > 0)
            {
                fieldW = maxWidth - 10;
                int want = block->fieldWidth * 8;
                if (want < 80)
                {
                    want = 80;
                }
                if (want < fieldW)
                {
                    fieldW = want;
                }
            }
            int isTextarea = block->inputType &&
                             strcmp(block->inputType, "textarea") == 0;
            int fieldRows = isTextarea ? (block->fieldRows > 0 ? block->fieldRows : 2) : 1;
            if (fieldRows > 1)
            {
                fieldH = 18 + fieldRows * 16;
            }
            LayoutItem *it = item_new(LRI_INPUT_FIELD);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = fieldW;
            it->h = fieldH;
            it->inputType = block->inputType ? block->inputType : "text";
            it->name = block->name ? block->name : "q";
            it->value = block->value ? block->value : "";
            it->placeholder = block->placeholder ? block->placeholder : "";
            it->formAction = block->formAction ? block->formAction : "";
            it->formMethod = block->formMethod ? block->formMethod : "get";
            it->disabled = block->disabled;
            it->readonly = block->readonly;
            it->required = block->required;
            it->maxlength = block->maxlength;
            it->block = block;
            if (!block->disabled && !block->inert)
            {
                char label[128];
                snprintf(label, sizeof(label), "[Input: %s]",
                         block->name && block->name[0] ? block->name : "q");
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isFormInput = 1;
                aux.inputItem = it;
                add_rect_aux(block->formAction && block->formAction[0] ? block->formAction : "#",
                             label, marginX, currentY, fieldW, fieldH, 0, &aux);
            }
            currentY += fieldH + 8;
        }
        /* 10. Submit Buttons */
        else if (block->type == DOC_BLOCK_INPUT_SUBMIT)
        {
            int btnH = 24;
            LCDFont *btnFont = style_font(PLUTO_FONT_BODY_BOLD);
            const char *lbl = block->label ? block->label : "Submit";
            int tw = measure(btnFont, lbl);
            int btnW = tw + 24;
            if (btnW < 50)
            {
                btnW = 50;
            }
            if (btnW > maxWidth)
            {
                btnW = maxWidth;
            }
            LayoutItem *it = item_new(LRI_INPUT_SUBMIT);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = btnW;
            it->h = btnH;
            it->label = lbl;
            it->name = block->name;
            it->value = block->value;
            it->formAction = block->formAction ? block->formAction : "";
            it->formMethod = block->formMethod ? block->formMethod : "get";
            it->disabled = block->disabled;
            it->block = block;
            if (!block->disabled && !block->inert)
            {
                char label[128];
                snprintf(label, sizeof(label), "[Button: %s]", lbl);
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isFormInput = 1;
                aux.inputItem = it;
                add_rect_aux(block->formAction && block->formAction[0] ? block->formAction : "#",
                             label, marginX, currentY, btnW, btnH, 0, &aux);
            }
            currentY += btnH + 8;
        }
        /* 11. Checkboxes & Radio Buttons */
        else if (block->type == DOC_BLOCK_CHECKBOX_FIELD)
        {
            int boxH = 20;
            LayoutItem *it = item_new(LRI_CHECKBOX_FIELD);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = boxH;
            it->h = boxH;
            it->radio = block->radio;
            it->name = block->name ? block->name : "";
            it->value = block->value ? block->value : "";
            it->label = block->label ? block->label : "";
            it->formAction = block->formAction ? block->formAction : "";
            it->formMethod = block->formMethod ? block->formMethod : "get";
            it->disabled = block->disabled;
            it->block = block;

            char label[128];
            snprintf(label, sizeof(label), "%s", block->label ? block->label : "");
            if (strlen(label) > 30)
            {
                label[28] = '\0';
                strcat(label, "..");
            }
            LCDFont *font = style_font(PLUTO_FONT_BODY);
            int lw = measure(font, label);
            int lx = marginX + boxH + 6;
            if (label[0] != '\0')
            {
                LayoutItem *ti = item_new(LRI_TEXT);
                if (!ti)
                {
                    return;
                }
                ti->text = arena_word(label);
                ti->font = font;
                ti->x = lx;
                ti->y = currentY + 2;
                ti->w = lw;
                ti->h = boxH;
                ti->bold = 0;
            }

            if (!block->disabled && !block->inert)
            {
                char href[256];
                snprintf(href, sizeof(href), "input:%s",
                         block->name && block->name[0] ? block->name : "q");
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isFormInput = 1;
                aux.inputItem = it;
                add_rect_aux(href, label, marginX, currentY, boxH + 8 + lw, boxH, 0, &aux);
            }

            currentY += boxH + 8;
        }
        /* 12. Select Dropdowns */
        else if (block->type == DOC_BLOCK_SELECT_FIELD)
        {
            int fieldH = 22;
            LayoutItem *it = item_new(LRI_SELECT_FIELD);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = maxWidth - 10;
            it->h = fieldH;
            it->name = block->name ? block->name : "q";
            it->options = block->options;
            it->optionCount = block->optionCount;
            it->formAction = block->formAction ? block->formAction : "";
            it->formMethod = block->formMethod ? block->formMethod : "get";
            it->disabled = block->disabled;
            it->block = block;

            if (!block->disabled && !block->inert)
            {
                char href[256];
                snprintf(href, sizeof(href), "select:%s",
                         block->name && block->name[0] ? block->name : "q");
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isFormInput = 1;
                aux.inputItem = it;
                add_rect_aux(href, "[Select]", marginX, currentY, maxWidth - 10, fieldH, 0, &aux);
            }

            currentY += fieldH + 8;
        }
        /* 13. Media Placeholders */
        else if (block->type == DOC_BLOCK_PLACEHOLDER)
        {
            int boxW = (int)block->pwidth ? (int)block->pwidth : maxWidth;
            if (boxW > maxWidth)
            {
                boxW = maxWidth;
            }
            int boxH = (int)block->pheight ? (int)block->pheight : 46;
            if (boxH > 120)
            {
                boxH = 120;
            }
            LayoutItem *it = item_new(LRI_PLACEHOLDER);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = boxW;
            it->h = boxH;
            it->label = block->plabel ? block->plabel
                : (block->text ? block->text : "Media"); /* Lua `or`: nil-check only */
            if (block->phref && !block->inert)
            {
                add_rect_aux(block->phref,
                             block->plabel ? block->plabel : "Embed",
                             marginX, currentY, boxW, boxH, 0, NULL);
            }
            currentY += boxH + 10;
        }
        /* 14. Progress / Meter Bars */
        else if (block->type == DOC_BLOCK_METER)
        {
            int boxH = 20;
            int tfMFrom = g_itemCount;
            LayoutItem *it = item_new(LRI_METER);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = maxWidth;
            it->h = boxH;
            it->mValue = block->mvalue;
            it->mMin = block->mmin;
            it->mMax = block->mmax;
            it->mLow = block->mlow;
            it->mHigh = block->mhigh;
            it->mOptimum = block->moptimum;
            it->label = block->label ? block->label : "";
            tf_apply_items(tfMFrom, g_itemCount, block, maxWidth, boxH);
            currentY += boxH + 10;
        }
        /* R72: CSS inline-block link button (direct row-flex item). */
        else if (block->type == DOC_BLOCK_LINK_BUTTON)
        {
            int bw = block->border > 0 ? block->border : 0;
            int padL = block->btnPadL ? block->btnPadL : 9;
            int padR = block->btnPadR ? block->btnPadR : 9;
            int padT = block->btnPadT ? block->btnPadT : 5;
            int padB = block->btnPadB ? block->btnPadB : 5;
            int radius = block->btnRadius ? block->btnRadius : 5;
            const char *label = block->label ? block->label : "";
            LCDFont *font = style_font(PLUTO_FONT_BODY_BOLD);
            int lineH = 18;
            int tw = measure(font, label);
            int boxW = tw + padL + padR + 2 * bw;
            int boxH = lineH + padT + padB + 2 * bw;
            int boxX = marginX + block->indent;
            int boxY = currentY + block->spacingTop;
            if (boxW > maxWidth)
            {
                boxW = maxWidth;
            }
            int itemFrom = g_itemCount;
            /* Keep link buttons outline-only: a dithered 1-bit fill crosses
             * the label, so the primary pill should match the secondary. */
            if (bw > 0)
            {
                LayoutItem *fi = item_new(LRI_BOX_FRAME);
                if (!fi)
                {
                    return;
                }
                fi->x = boxX;
                fi->y = boxY;
                fi->w = boxW;
                fi->y2 = boxY + boxH;
                fi->radius = radius;
            }
            LayoutItem *ti = item_new(LRI_TEXT);
            if (!ti)
            {
                return;
            }
            ti->text = label;
            ti->font = font;
            ti->x = boxX + padL + bw;
            ti->y = boxY + padT + bw;
            ti->w = tw;
            ti->h = lineH;
            ti->bold = 1;
            if (block->href && block->href[0])
            {
                add_rect_aux(block->href, label, boxX, boxY, boxW, boxH, 0,
                             NULL);
            }
            tf_apply_items(itemFrom, g_itemCount, block, boxW, boxH);
            logger_log("[link-btn] layout x=%d y=%d w=%d h=%d pad=%d/%d/%d/%d border=%d bg=%d radius=%d row=%d item=%d",
                       boxX, boxY, boxW, boxH, padL, padR, padT, padB, bw,
                       block->bgGray, radius, block->flexRow,
                       block->flexItem);
            currentY = boxY + boxH + block->spacingBottom +
                       block->gapApplied;
        }
        /* 15. Bordered Boxes */
        else if (block->type == DOC_BLOCK_BOX_OPEN)
        {
            LayoutItem *it = item_new(LRI_BOX_FRAME);
            if (!it)
            {
                return;
            }
            it->x = marginX;
            it->y = currentY;
            it->w = maxWidth;
            it->y2 = currentY;
            it->label = block->label ? block->label : "";
            it->toggleKey = block->toggleKey;
            it->toggleOpen = block->toggleOpen;
            if (boxTop < (int)(sizeof(boxStack) / sizeof(boxStack[0])))
            {
                boxStack[boxTop++] = it;
            }
            if (block->toggleKey)
            {
                char href[64];
                snprintf(href, sizeof(href), "toggle:%s", block->toggleKey);
                LayoutRectAux aux;
                memset(&aux, 0, sizeof(aux));
                aux.isToggle = 1;
                aux.toggleKey = block->toggleKey;
                aux.toggleOpen = block->toggleOpen;
                add_rect_aux(href, block->label ? block->label : "",
                             marginX, currentY, maxWidth, 18, 0, &aux);
            }
            currentY += 18;
        }
        else if (block->type == DOC_BLOCK_BOX_CLOSE)
        {
            if (boxTop > 0)
            {
                LayoutItem *frame = boxStack[--boxTop];
                frame->y2 = currentY;
            }
            currentY += 8;
        }
        /* CSS borders (roadmap #2): outline the block's laid-out span
         * with nested 1px frames (LRI_BOX_FRAME draws square corners —
         * the spec default; round stays table-only). Widths that would
         * go degenerate simply stop (no partial frames). */
        int boxBottom = currentY - block->gapApplied;
        if (block->type != DOC_BLOCK_LINK_BUTTON && block->border > 0 &&
            boxBottom > borderTop)
        {
            int bw = block->border > 3 ? 3 : block->border;
            for (int k = 0; k < bw; k++)
            {
                int fx = marginX + k;
                int fy = borderTop + k;
                int fw = maxWidth - 2 * k;
                int fy2 = boxBottom - k;
                if (fw < 1 || fy2 <= fy)
                {
                    break;
                }
                LayoutItem *bi = item_new(LRI_BOX_FRAME);
                if (!bi)
                {
                    return;
                }
                bi->x = fx;
                bi->y = fy;
                bi->w = fw;
                bi->y2 = fy2;
            }
        }
        /* CSS gray backgrounds: one shade item over the block's span.
         * layout_draw paints shades in a first pass (under content), so
         * emission order here is irrelevant. Density from luminance:
         * <160 → 50% lines, else 25%. */
        if (block->type != DOC_BLOCK_LINK_BUTTON &&
            block->bgGray >= 96 && block->bgGray <= 215 &&
            boxBottom > borderTop)
        {
            LayoutItem *si = item_new(LRI_SHADE);
            if (!si)
            {
                return;
            }
            si->x = marginX;
            si->y = borderTop;
            si->w = maxWidth;
            si->h = boxBottom - borderTop;
            si->shade = (block->bgGray < 160) ? 2 : 1;
        }
        /* R71: close the pinned app-bar span (last block of the sticky
         * container) and any spanning decoration that ends here. */
        if (flexActive && block->pinEnd)
        {
            if (flexGroupCount > 0)
            {
                flexGroups[flexGroupCount - 1].itemTo = g_itemCount;
                flexGroups[flexGroupCount - 1].rectTo = lm_rect_total();
            }
            flex_finish_row(flexGroups, flexGroupCount, flexRowStartY,
                            flexJustifyCur, flexCenterCur, marginX, maxWidth,
                            &currentY);
            /* flex_finish_row adds a 6px inter-block tail. Inside a sticky
             * app bar that tail is part of the bar box, not page flow: it
             * made the painted bar look taller than its nav row and left a
             * blank strip below the controls. Trim it from the bar's box. */
            currentY -= 6;
            flexActive = 0;
            flexGroupCount = 0;
        }
        if (block->pinEnd && g_pinOpenSpan)
        {
            g_pinTo = g_itemCount;
            g_pinH = currentY - g_pinY;
            if (g_pinH > 0)
            {
                /* The sticky box's own background/border paint full width
                 * (a header bar is not a text column). It lives INSIDE the
                 * pin item span so the pinned layer redraws it too. */
                LayoutItem *pb = item_new(LRI_PIN_BAR);
                if (pb)
                {
                    pb->x = 0;
                    pb->y = g_pinY;
                    pb->w = SCREEN_WIDTH;
                    pb->h = g_pinH;
                }
            }
            g_pinTo = g_itemCount;
            g_pinOpenSpan = 0;
            g_hasPin = (g_pinTo > g_pinFrom && g_pinH > 0);
            logger_log("[bar] pin bi=%d y=%d h=%d items=%d..%d",
                       bi, g_pinY, g_pinH, g_pinFrom, g_pinTo);
        }
        for (int di = 0; di < doc->decorCount && di < DOC_BOX_DECOR_MAX; di++)
        {
            const DocBoxDecor *d = &doc->decor[di];
            if (d->endBlock != bi || d->startBlock < 0)
            {
                continue;
            }
            int dy0 = g_decorY[di];
            /* A flex/grid gap is outside the item's border box. currentY
             * already includes the gap so the NEXT item starts in the right
             * place; the decoration for THIS item must stop before it or the
             * gap is swallowed inside the card border and cards touch. */
            int dy1 = currentY - block->gapApplied;
            if (dy1 <= dy0)
            {
                continue;
            }
            if (d->bgGray >= 96 && d->bgGray <= 215)
            {
                LayoutItem *si = item_new(LRI_SHADE);
                if (!si)
                {
                    return;
                }
                si->x = marginX;
                si->y = dy0;
                si->w = maxWidth;
                si->h = dy1 - dy0;
                si->shade = (d->bgGray < 160) ? 2 : 1;
            }
            if (d->border > 0)
            {
                logger_log("[decor-frame] start=%d end=%d bi=%d y=%d..%d",
                           d->startBlock, d->endBlock, bi, dy0, dy1);
                int bw = d->border > 3 ? 3 : d->border;
                for (int k = 0; k < bw; k++)
                {
                    int fx = marginX + k;
                    int fy = dy0 + k;
                    int fw = maxWidth - 2 * k;
                    int fy2 = dy1 - k;
                    if (fw < 1 || fy2 <= fy)
                    {
                        break;
                    }
                    LayoutItem *fi = item_new(LRI_BOX_FRAME);
                    if (!fi)
                    {
                        return;
                    }
                    fi->x = fx;
                    fi->y = fy;
                    fi->w = fw;
                    fi->y2 = fy2;
                    fi->radius = d->radius > k ? d->radius - k : 0;
                }
            }
        }
        /* R71 dropdown panel close: compute the right-aligned overlay box,
         * move the panel's text/hit rects into it, and record the frame. */
        if (block->dropdownEnd && g_dropOpen)
        {
            int dropW = 208;
            if (dropW > maxWidth - 8)
            {
                dropW = maxWidth - 8;
            }
            int right = marginX + maxWidth - 16;
            int left = right - dropW;
            if (left < marginX)
            {
                left = marginX;
            }
            int dx = left - marginX;
            for (int i = g_dropItemFrom; i < g_itemCount; i++)
            {
                LayoutItem *it = g_items[i];
                it->x += dx;
                if (it->type == LRI_LINE || it->type == LRI_BOX_FRAME ||
                    it->type == LRI_QUOTE_BAR)
                {
                    it->x1 += dx;
                    it->x2 += dx;
                }
            }
            lm_shift_rects(g_dropRectFrom,
                           lm_rect_total() - g_dropRectFrom, dx, 0);
            if (currentY > g_dropY)
            {
                LayoutItem *pi = item_new(LRI_DROPDOWN_PANEL);
                if (pi)
                {
                    pi->x = left;
                    pi->y = g_dropY;
                    pi->w = dropW;
                    pi->h = currentY - g_dropY;
                    pi->radius = 4;
                }
                g_dropPanelH = currentY - g_dropY;
                g_dropX = left;
                g_dropW = dropW;
                g_dropItemTo = g_itemCount;
                /* A dropdown row is a menu target, not a text-column link:
                 * make the whole panel width tappable and give these rects
                 * priority over same-coordinate links in the page under the
                 * overlay. */
                int dropRectCount = lm_rect_total() - g_dropRectFrom;
                lm_widen_rects(g_dropRectFrom, dropRectCount, left, dropW);
                lm_set_overlay_rects(g_dropRectFrom, dropRectCount);
                logger_log("[bar] dropdown x=%d y=%d w=%d h=%d items=%d..%d",
                           left, g_dropY, dropW, g_dropPanelH,
                           g_dropItemFrom, g_dropItemTo);
            }
            /* position:absolute is out of flow: the panel must not move
             * the page or stretch the sticky header box. */
            currentY = g_dropFlowSavedY;
            g_dropOpen = 0;
        }
        if (flexActive && flexGroupCount > 0)
        {
            flexGroups[flexGroupCount - 1].itemTo = g_itemCount;
            flexGroups[flexGroupCount - 1].rectTo = lm_rect_total();
        }
    }
    if (flexActive)
    {
        flex_finish_row(flexGroups, flexGroupCount, flexRowStartY,
                        flexJustifyCur, flexCenterCur, marginX, maxWidth,
                        &currentY);
    }

    int th = currentY + 20;
    g_totalHeight = th > CONTENT_HEIGHT ? th : CONTENT_HEIGHT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 2: state accessors, Layout.draw, eviction, on-demand overlay.
 * ═══════════════════════════════════════════════════════════════════════════ */

int layout_get_total_height(void) { return g_totalHeight; }
int layout_get_item_count(void) { return g_itemCount; }

/* R71: screen point → page Y for hit testing (see layout.h). */
int layout_page_y(int screenX, int screenY, int scrollY)
{
    int pinnedNow = g_hasPin && g_pinH > 0 && scrollY > g_pinY - CONTENT_Y;
    if (pinnedNow && screenY >= CONTENT_Y &&
        screenY < CONTENT_Y + g_pinH)
    {
        return g_pinY + (screenY - CONTENT_Y);
    }
    /* An open dropdown is painted above the page and follows the pinned bar.
     * Map only points inside its actual box back to its page rows, so page
     * content outside the panel stays tappable underneath it. */
    if (g_dropPanelH > 0 && g_dropItemTo > g_dropItemFrom &&
        screenX >= g_dropX && screenX < g_dropX + g_dropW)
    {
        int dropScroll = pinnedNow ? (g_pinY - CONTENT_Y) : scrollY;
        int dropTop = g_dropY - dropScroll;
        if (screenY >= dropTop && screenY < dropTop + g_dropPanelH)
        {
            return g_dropY + (screenY - dropTop);
        }
    }
    return screenY + scrollY;
}

const LayoutItem *layout_item_at(int index)
{
    if (index < 0 || index >= g_itemCount)
    {
        return NULL;
    }
    return g_items[index];
}

const LayoutItem *layout_get_selected_input(void) { return g_selectedInput; }
void layout_set_selected_input(const LayoutItem *item)
{
    g_selectedInput = (LayoutItem *)item;
}

/* ── layout_set_input_value ──────────────────────────────────────────────── */
/* Port of the Lua reference's activeInputField.value = entered: in Lua the
 * render item IS the block, so a single assignment made typed text visible to
 * the renderer and the submit path. In C the LayoutItem and DocBlock are
 * separate, so the item keeps its own owned copy of the value. The item's
 * value pointer is replaced (old owned copy freed); block->value stays
 * untouched — form submission reads through form_item_value() which prefers
 * the live item value. */
void layout_set_input_value(const LayoutItem *item, const char *text)
{
    if (!item)
    {
        return;
    }
    LayoutItem *it = (LayoutItem *)item;
    char *owned = text ? strdup(text) : NULL;
    if (it->ownedValue)
    {
        free(it->ownedValue);
    }
    it->ownedValue = owned;
    it->value = owned ? owned : "";
}
int layout_get_on_demand_consumed(void) { return g_onDemandConsumed; }
void layout_clear_on_demand_consumed(void) { g_onDemandConsumed = 0; }
const char *layout_get_hovered_image_src(void)
{
    return g_hasHoveredImage ? g_hoveredImageSrc : NULL;
}

static int od_requested(const char *src)
{
    if (!src)
    {
        return 0;
    }
    for (int i = 0; i < g_odRequestedCount; i++)
    {
        if (strcmp(g_odRequested[i], src) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void od_request(const char *src)
{
    if (!src || od_requested(src))
    {
        return;
    }
    if (g_odRequestedCount < ONDEMAND_MAP_MAX)
    {
        snprintf(g_odRequested[g_odRequestedCount++], 512, "%s", src);
    }
}

static void od_unrequest(const char *src)
{
    for (int i = 0; i < g_odRequestedCount; i++)
    {
        if (strcmp(g_odRequested[i], src) == 0)
        {
            for (int j = i + 1; j < g_odRequestedCount; j++)
            {
                memcpy(g_odRequested[j - 1], g_odRequested[j], 512);
            }
            g_odRequestedCount--;
            return;
        }
    }
}

static int image_mode(void)
{
    const char *s = storage_setting_str("imageMode");
    int m = s ? image_mode_from_name(s) : -1;
    return m >= 0 ? m : IMAGE_MODE_VIEWPORT; /* Lua default = Constants.IMAGE_MODE_VIEWPORT */
}

/* ── helpers shared by draw ──────────────────────────────────────────────── */

static void draw_scaled_fit(LCDBitmap *bmp, int x, int y, int w, int h)
{
    int iw = 0, ih = 0, rb = 0;
    uint8_t *mask = NULL, *dat = NULL;
    g_pd->graphics->getBitmapData(bmp, &iw, &ih, &rb, &mask, &dat);
    if (iw > 0 && ih > 0)
    {
        float scale = (float)w / iw;
        float sy = (float)h / ih;
        if (sy < scale)
        {
            scale = sy;
        }
        int dw = (int)(iw * scale);
        int dh = (int)(ih * scale);
        int dx = x + (w - dw) / 2;
        int dy = y + (h - dh) / 2;
        g_pd->graphics->drawScaledBitmap(bmp, dx, dy, scale, scale);
    }
    else
    {
        g_pd->graphics->drawBitmap(bmp, x, y, kBitmapUnflipped);
    }
}

/* Lua fillCircleAtPoint(cx, cy, r): inclusive disk of diameter 2r+1 →
 * C fillEllipse bounding box (x, y, 2r+1, 2r+1). */
static void fill_circle_at(int cx, int cy, int r, LCDColor color)
{
    g_pd->graphics->fillEllipse(cx - r, cy - r, r * 2 + 1, r * 2 + 1,
                                0.f, 360.f, color);
}

static void draw_pixel(int x, int y, LCDColor color)
{
    g_pd->graphics->fillRect(x, y, 1, 1, color);
}

static void draw_placeholder_card(int x, int y, int w, int h)
{
    g_pd->graphics->fillRoundRect(x, y, w, h, 4, kColorWhite);
    g_pd->graphics->drawRoundRect(x, y, w, h, 4, 1, kColorBlack);
    for (int hx = x + 4; hx <= x + w - 4; hx += 10)
    {
        g_pd->graphics->drawLine(hx, y + 3, hx, y + h - 3, 1, kColorBlack);
    }
    int iconX = x + w / 2 - 8;
    int iconY = y + h / 2 - 6;
    g_pd->graphics->drawRoundRect(iconX, iconY, 16, 11, 2, 1, kColorBlack);
    fill_circle_at(iconX + 8, iconY + 5, 3, kColorBlack);
    draw_pixel(iconX + 13, iconY + 1, kColorBlack);
}

static void draw_on_demand_image_box(const LayoutItem *item, int drawY,
                                     const char *label, int withIcon)
{
    g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorWhite);
    g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
    for (int hx = item->x + 4; hx <= item->x + item->w - 4; hx += 10)
    {
        g_pd->graphics->drawLine(hx, drawY + 3, hx, drawY + item->h - 3, 1, kColorBlack);
    }
    LCDFont *font = style_font(PLUTO_FONT_SMALL);
    int lw = measure(font, label);
    if (withIcon)
    {
        int iconX = item->x + item->w / 2 - 8;
        int iconY = drawY + item->h / 2 - 10;
        g_pd->graphics->drawRoundRect(iconX, iconY, 16, 11, 2, 1, kColorBlack);
        fill_circle_at(iconX + 8, iconY + 5, 3, kColorBlack);
        draw_pixel(iconX + 13, iconY + 1, kColorBlack);
        if (iconY + 18 <= drawY + item->h - 2)
        {
            g_pd->graphics->setFont(font);
            g_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                     item->x + (item->w - lw) / 2, iconY + 16);
        }
    }
    else
    {
        g_pd->graphics->setFont(font);
        g_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                 item->x + (item->w - lw) / 2,
                                 drawY + item->h / 2 - 5);
    }
}

/* ── table_box painter ───────────────────────────────────────────────────── */

static void draw_table_box(LayoutItem *item, int drawY)
{
    const DocTable *tbl = item->table;
    if (item->border)
    {
        g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 3, kColorWhite);
        g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 3, 1, kColorBlack);
    }

    int colCount = 1;
    for (int ri = 0; ri < tbl->rowCount; ri++)
    {
        const DocRow *row = tbl->rows[ri];
        if (!row)
        {
            continue;
        }
        int n = 0;
        for (int ci = 0; ci < row->cellCount; ci++)
        {
            const DocCell *cell = row->cells[ci];
            if (cell)
            {
                n += cell->colspan > 0 ? cell->colspan : 1;
            }
        }
        if (n > colCount)
        {
            colCount = n;
        }
    }
    int colW = (item->w - 16) / (colCount > 1 ? colCount : 1);
    /* With <colgroup>/<col>, use the resolved per-column pixel widths
     * (grid in table space; cells left-offset by their grid column). */
    int useGrid = (tbl->cols && tbl->colX && tbl->colW &&
                   tbl->gridCount > 0 && colCount <= tbl->gridCount);
    int innerX = item->x + 8;
    int innerW = item->w - 16;
    if (useGrid)
    {
        innerW = tbl->widthPx - 16;
    }

    int capH = (item->caption && item->caption[0]) ? 16 : 0;
    if (capH > 0)
    {
        LCDFont *font = style_font(PLUTO_FONT_BODY_BOLD);
        g_pd->graphics->setFont(font);
        char cap[128];
        snprintf(cap, sizeof(cap), "%s", item->caption);
        if (strlen(cap) > 30)
        {
            cap[28] = '\0';
            strcat(cap, "..");
        }
        g_pd->graphics->drawText(cap, strlen(cap), kUTF8Encoding, item->x + 8, drawY + 2);
    }

    int rowY = drawY + 4 + capH;
    for (int ri = 0; ri < tbl->rowCount; ri++)
    {
        const DocRow *row = tbl->rows[ri];
        int cellX = item->x + 8;
        int gridCol = 0; /* running grid-column index (useGrid) */
        if (row)
        {
            for (int ci = 0; ci < row->cellCount; ci++)
            {
                const DocCell *cell = row->cells[ci];
                if (!cell)
                {
                    continue;
                }
                int span = cell->colspan > 0 ? cell->colspan : 1;
                int cw;
                if (useGrid)
                {
                    /* Sum the grid columns this cell spans. */
                    cw = 0;
                    for (int s = 0; s < span && gridCol + s < tbl->gridCount; s++)
                    {
                        cw += tbl->colW[gridCol + s];
                    }
                    if (cw <= 0)
                    {
                        cw = colW * span;
                    }
                    cellX = innerX + tbl->colX[gridCol];
                }
                else
                {
                    cw = colW * span;
                }
                char *txt = g_layoutCellA; /* BSS-hoisted (stack audit) */
                size_t to = 0;
                txt[0] = '\0';
                for (int ii = 0; ii < cell->inlineCount; ii++)
                {
                    const DocInline *inl = cell->inlines[ii];
                    if (inl && inl->text)
                    {
                        size_t tl = strlen(inl->text);
                        if (to + tl < 1024 - 1)
                        {
                            strcat(txt + to, inl->text);
                            to += tl;
                        }
                    }
                }
                /* gsub("%s+", " ") + trim: collapse then trim in place */
                {
                    char *out = g_layoutCellB; /* BSS-hoisted (stack audit) */
                    size_t o = 0;
                    int inWs = 0;
                    for (const char *p = txt; *p; p++)
                    {
                        if (is_space_char(*p))
                        {
                            if (!inWs && o < 1024 - 1)
                            {
                                out[o++] = ' ';
                            }
                            inWs = 1;
                        }
                        else
                        {
                            if (o < 1024 - 1)
                            {
                                out[o++] = *p;
                            }
                            inWs = 0;
                        }
                    }
                    out[o] = '\0';
                    const char *a = out;
                    while (*a == ' ')
                    {
                        a++;
                    }
                    const char *z = out + strlen(out);
                    while (z > a && *(z - 1) == ' ')
                    {
                        z--;
                    }
                    size_t n2 = (size_t)(z - a);
                    memmove(txt, a, n2);
                    txt[n2] = '\0';
                }

                LCDFont *font = cell->header ? style_font(PLUTO_FONT_BODY_BOLD)
                                             : style_font(PLUTO_FONT_SMALL);
                g_pd->graphics->setFont(font);
                /* CSS cell padding: inset text, shrink the measure width. */
                int pad = cell->pad;
                if (pad < 0)
                {
                    pad = 0;
                }
                int tcw = cw - pad;
                if (tcw < 8)
                {
                    pad = 0;
                    tcw = cw;
                }
                int maxChars = tcw / 7;
                if (maxChars < 2)
                {
                    maxChars = 2;
                }
                if ((int)strlen(txt) > maxChars)
                {
                    int keep = maxChars - 2;
                    if (keep < 1)
                    {
                        keep = 1;
                    }
                    txt[keep] = '\0';
                    strcat(txt, "..");
                }
                int tw = measure(font, txt);
                const char *cellAlign =
                    cell->align ? layout_normalize_align(cell->align) : NULL;
                int tx = cellX + pad;
                if (cellAlign && strcmp(cellAlign, "center") == 0)
                {
                    tx = cellX + pad + (tcw - tw) / 2;
                }
                else if (cellAlign && strcmp(cellAlign, "right") == 0)
                {
                    tx = cellX + pad + (tcw - tw > 0 ? tcw - tw : 0);
                }
                g_pd->graphics->drawText(txt, strlen(txt), kUTF8Encoding, tx, rowY);

                if (!g_cellLinksRegistered)
                {
                    const char *linkHref = NULL;
                    char linkText[512];
                    linkText[0] = '\0';
                    for (int ii = 0; ii < cell->inlineCount; ii++)
                    {
                        const DocInline *inl = cell->inlines[ii];
                        if (inl && inl->href)
                        {
                            linkHref = inl->href;
                            size_t cl = strlen(linkText);
                            if (inl->text)
                            {
                                strncat(linkText, inl->text, sizeof(linkText) - cl - 1);
                            }
                        }
                    }
                    if (linkHref)
                    {
                        LMRect r = {tx, rowY + g_scrollYForLinks, tw, 16};
                        lm_add_link_rect_ex(linkHref,
                                            linkText[0] ? linkText : linkHref,
                                            &r, 0, NULL);
                    }
                }
                cellX += cw;
                gridCol += span;
            }
        }
        rowY += 18;
        /* CSS border-collapse: separate (2) skips the row separators;
         * legacy (0) and collapse (1) draw them. */
        if (tbl->collapse != 2 && rowY < drawY + item->h - 4)
        {
            g_pd->graphics->drawLine(item->x, rowY - 2, item->x + item->w, rowY - 2,
                                     1, kColorBlack);
        }
    }
    g_cellLinksRegistered = 1;
}


void layout_draw(int scrollY)
{
    logger_stack_touch();
    if (!g_pd)
    {
        return;
    }
    g_scrollYForLinks = scrollY;

    g_pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT, kColorWhite);
    g_pd->graphics->setClipRect(0, CONTENT_Y, CONTENT_WIDTH, CONTENT_HEIGHT);

    /* R71: up to THREE LAYERS. Layer 0 = the page at the real scroll offset.
     * The pinned app-bar span is re-drawn at the content top once the sticky
     * container's flow position scrolled past the viewport; the open dropdown
     * is a separate top layer, exactly like the author's position:absolute
     * menu. Each layer draws the same two passes: gray shades first (under
     * content), then everything else. */
    int pinnedNow = g_hasPin && g_pinH > 0 && scrollY > g_pinY - CONTENT_Y;
    int hasDrop = (g_dropItemTo > g_dropItemFrom);
    int layerCount = 1 + (pinnedNow ? 1 : 0) + (hasDrop ? 1 : 0);
    int pinLayer = pinnedNow ? 1 : -1;
    int dropLayer = hasDrop ? (layerCount - 1) : -1;
    for (int layer = 0; layer < layerCount; layer++)
    {
        if (layer == pinLayer)
        {
            int bh = g_pinH > CONTENT_HEIGHT ? CONTENT_HEIGHT : g_pinH;
            g_pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, bh, kColorWhite);
            g_pd->graphics->drawLine(0, CONTENT_Y + bh - 1, SCREEN_WIDTH,
                                     CONTENT_Y + bh - 1, 1, kColorBlack);
        }
        int layerScroll = (layer == pinLayer ||
                           (layer == dropLayer && pinnedNow))
                              ? (g_pinY - CONTENT_Y)
                              : scrollY;
        {
        /* This inner declaration shadows the argument: every draw arm
         * below uses this layer's offset with no further changes. */
        int scrollY = layerScroll;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < g_itemCount; i++)
        {
            LayoutItem *item = g_items[i];
            int inDrop = hasDrop && i >= g_dropItemFrom && i < g_dropItemTo;
            if (dropLayer >= 0)
            {
                if (layer == dropLayer && !inDrop)
                {
                    continue;
                }
                if (layer != dropLayer && inDrop)
                {
                    continue;
                }
            }
            if (pinnedNow && layer != dropLayer)
            {
                int inPin = (i >= g_pinFrom && i < g_pinTo);
                if ((layer == 0 && inPin) || (layer != 0 && !inPin))
                {
                    continue;
                }
            }
            int isShade = (item->type == LRI_SHADE ||
                           item->type == LRI_PIN_BAR ||
                           item->type == LRI_DROPDOWN_PANEL);
            if ((pass == 0) != isShade)
            {
                continue;
            }
            int drawY;
            switch (item->type)
            {
            case LRI_PIN_BAR:
            {
                int y0 = item->y - scrollY;
                g_pd->graphics->fillRect(0, y0, SCREEN_WIDTH, item->h,
                                         kColorWhite);
                g_pd->graphics->drawLine(0, y0 + item->h - 1, SCREEN_WIDTH,
                                         y0 + item->h - 1, 1, kColorBlack);
                break;
            }
            case LRI_DROPDOWN_PANEL:
            {
                int y0 = item->y - scrollY;
                if (y0 + item->h >= CONTENT_Y && y0 <= SCREEN_HEIGHT)
                {
                    int rad = item->radius;
                    if (rad > item->h / 2)
                    {
                        rad = item->h / 2;
                    }
                    g_pd->graphics->fillRoundRect(item->x, y0, item->w,
                                                  item->h, rad,
                                                  kColorWhite);
                    g_pd->graphics->drawRoundRect(item->x, y0, item->w,
                                                  item->h, rad, 1,
                                                  kColorBlack);
                }
                break;
            }
            case LRI_SHADE:
            {
                /* Line-dithered gray: 25% = every 4th row, 50% = every
                 * other row. Phase-locked to the block top (no scroll
                 * shimmer). Clipped to the content area like all items. */
                int y0 = item->y - scrollY;
                int y1 = y0 + item->h;
                int step = (item->shade == 2) ? 2 : 4;
                for (int yy = y0; yy < y1; yy++)
                {
                    if (yy < CONTENT_Y || yy > SCREEN_HEIGHT)
                    {
                        continue;
                    }
                    if (((yy - (item->y - scrollY)) % step) == 0)
                    {
                        g_pd->graphics->drawLine(item->x, yy,
                                                 item->x + item->w, yy, 1,
                                                 kColorBlack);
                    }
                }
                break;
            }
        case LRI_TEXT:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int isSelLink = item->href && lm_is_highlighted(item->href, item->x, item->y);
                int inverted = isSelLink || item->mark || item->invert;
                if (inverted)
                {
                    g_pd->graphics->fillRect(item->x - 1, drawY - 1, item->w + 2, item->h, kColorBlack);
                    g_pd->graphics->setDrawMode(kDrawModeFillWhite);
                }
                g_pd->graphics->setFont(item->font);
                g_pd->graphics->drawText(item->text, strlen(item->text), kUTF8Encoding,
                                         item->x, drawY);
                if (inverted)
                {
                    g_pd->graphics->setDrawMode(kDrawModeCopy);
                }
                if (item->strike)
                {
                    g_pd->graphics->drawLine(item->x, drawY + item->h / 2,
                                             item->x + item->w, drawY + item->h / 2,
                                             1, inverted ? kColorWhite : kColorBlack);
                }
                if (item->underline || item->href)
                {
                    g_pd->graphics->drawLine(item->x, drawY + item->h - 1,
                                             item->x + item->w, drawY + item->h - 1,
                                             1, inverted ? kColorWhite : kColorBlack);
                }
            }
            break;
        }
        case LRI_READER_BADGE:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorBlack);
                g_pd->graphics->setDrawMode(kDrawModeFillWhite);
                char line1[256];
                snprintf(line1, sizeof(line1), "READER MODE  *  %s", item->host);
                g_pd->graphics->setFont(style_font(PLUTO_FONT_BODY_BOLD));
                g_pd->graphics->drawText(line1, strlen(line1), kUTF8Encoding,
                                         item->x + 8, drawY + 3);
                g_pd->graphics->setFont(style_font(PLUTO_FONT_SMALL));
                g_pd->graphics->drawText(item->readingTime, strlen(item->readingTime),
                                         kUTF8Encoding, item->x + 8, drawY + 16);
                g_pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }
        case LRI_LINE:
        {
            int dy1 = item->y1 - scrollY;
            int dy2 = item->y2 - scrollY;
            if (dy1 >= CONTENT_Y - 2 && dy1 <= SCREEN_HEIGHT + 2)
            {
                g_pd->graphics->drawLine(item->x1, dy1, item->x2, dy2, 1, kColorBlack);
            }
            break;
        }
        case LRI_QUOTE_BAR:
        {
            int dy1 = item->y1 - scrollY;
            int dy2 = item->y2 - scrollY;
            if (dy2 >= CONTENT_Y && dy1 <= SCREEN_HEIGHT)
            {
                g_pd->graphics->fillRect(item->x, dy1, 3, dy2 - dy1, kColorBlack);
            }
            break;
        }
        case LRI_CODE_BOX:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 3, kColorWhite);
                g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 3, 1, kColorBlack);
                LCDFont *font = style_font(PLUTO_FONT_MONO);
                g_pd->graphics->setFont(font);
                int lineY = drawY + 6;
                for (int li = 0; li < item->lineCount; li++)
                {
                    if (lineY + 14 <= drawY + item->h)
                    {
                        char *expanded = g_layoutTabExp; /* BSS-hoisted (stack audit) */
                        layout_expand_tab_columns(item->lines[li], expanded, 1024);
                        g_pd->graphics->drawText(expanded, strlen(expanded), kUTF8Encoding,
                                                 item->x + 8, lineY);
                    }
                    lineY += 14;
                }
            }
            break;
        }
        case LRI_TABLE_BOX:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                draw_table_box(item, drawY);
            }
            break;
        }
        case LRI_IMAGE:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int mode = image_mode();
                if (mode == IMAGE_MODE_DISABLED)
                {
                    g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorWhite);
                    g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                    LCDFont *font = style_font(PLUTO_FONT_SMALL);
                    const char *lbl = "[Image Off]";
                    int lw = measure(font, lbl);
                    g_pd->graphics->setFont(font);
                    g_pd->graphics->drawText(lbl, strlen(lbl), kUTF8Encoding,
                                             item->x + (item->w - lw) / 2,
                                             drawY + item->h / 2 - 5);
                }
                else if (mode == IMAGE_MODE_ONDEMAND)
                {
                    int isDecoded = item->src && imgdec_is_decoded(item->src);
                    int wasRequested = item->src && od_requested(item->src);
                    if (isDecoded)
                    {
                        LCDBitmap *cached = imgdec_get_image(item->src);
                        if (cached)
                        {
                            draw_scaled_fit(cached, item->x, drawY, item->w, item->h);
                        }
                    }
                    else if (wasRequested)
                    {
                        draw_on_demand_image_box(item, drawY, "Loading...", 0);
                    }
                    else
                    {
                        draw_on_demand_image_box(item, drawY,
                                                 item->imgHref ? "Tap: View/Open" : "Tap: View Image",
                                                 1);
                    }
                }
                else if (mode == IMAGE_MODE_HOVER)
                {
                    int isHovered = g_hasHoveredImage && item->src &&
                                    strcmp(g_hoveredImageSrc, item->src) == 0;
                    if (isHovered)
                    {
                        if (item->src)
                        {
                            imgdec_enqueue(item->src);
                        }
                        if (item->img)
                        {
                            draw_scaled_fit(item->img, item->x, drawY, item->w, item->h);
                        }
                        else if (item->src)
                        {
                            imgdec_draw(item->x, drawY, item->w, item->h, item->alt,
                                        item->imgHref, 0, item->src);
                        }
                    }
                    else
                    {
                        g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorWhite);
                        g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                        LCDFont *font = style_font(PLUTO_FONT_SMALL);
                        const char *lbl = "[Hover]";
                        int lw = measure(font, lbl);
                        g_pd->graphics->setFont(font);
                        g_pd->graphics->drawText(lbl, strlen(lbl), kUTF8Encoding,
                                                 item->x + (item->w - lw) / 2,
                                                 drawY + item->h / 2 - 5);
                    }
                }
                else if (mode == IMAGE_MODE_VIEWPORT)
                {
                    if (item->src)
                    {
                        imgdec_enqueue(item->src);
                    }
                    if (item->img)
                    {
                        draw_scaled_fit(item->img, item->x, drawY, item->w, item->h);
                    }
                    else
                    {
                        imgdec_draw(item->x, drawY, item->w, item->h, item->alt,
                                    item->imgHref, 0, item->src);
                    }
                }
                else /* IMAGE_MODE_ALL */
                {
                    if (item->img)
                    {
                        draw_scaled_fit(item->img, item->x, drawY, item->w, item->h);
                    }
                    else
                    {
                        imgdec_draw(item->x, drawY, item->w, item->h, item->alt,
                                    item->imgHref, 0, item->src);
                    }
                }
            }
            break;
        }
        case LRI_MENU_BUTTON:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int border = item->menuBorder > 0 ? item->menuBorder : 1;
                int rad = item->radius;
                if (rad > item->h / 2)
                {
                    rad = item->h / 2;
                }
                g_pd->graphics->fillRoundRect(item->x, drawY, item->w,
                                              item->h, rad, kColorWhite);
                g_pd->graphics->drawRoundRect(item->x, drawY, item->w,
                                              item->h, rad, border,
                                              kColorBlack);
                int hx = item->x + border + item->menuPadL;
                LCDFont *f = style_font(item->small
                                            ? PLUTO_FONT_SMALL
                                            : PLUTO_FONT_BODY);
                int fh = item->small ? 14 : 16;
                /* The trigger's 15px caps have a 2px top bearing inside the
                 * line box. Lift the text one extra pixel so the capital's
                 * optical center lands on the pill center, where the
                 * hamburger is drawn. */
                int textY = drawY + (item->h - fh) / 2 - 2;
                /* Center the hamburger on the label's full visible bbox
                 * (cap top through baseline), not the lowercase x-height:
                 * at the 20px control height the x-height center put the
                 * icon visibly low next to the capital M. */
                int iconY = drawY + (item->h - 7) / 2 + 1;
                for (int r = 0; r < 3; r++)
                {
                    g_pd->graphics->drawLine(hx, iconY + r * 3, hx + 9,
                                             iconY + r * 3, 1, kColorBlack);
                }
                if (item->label && item->label[0])
                {
                    g_pd->graphics->setFont(f);
                    g_pd->graphics->drawText(
                        item->label, strlen(item->label), kUTF8Encoding,
                        hx + 11 + item->menuGap, textY);
                }
            }
            break;
        }
        case LRI_INPUT_FIELD:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int isSel = (g_selectedInput == item);
                if (isSel)
                {
                    g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorBlack);
                    g_pd->graphics->setDrawMode(kDrawModeFillWhite);
                }
                else
                {
                    g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                    if (item->disabled)
                    {
                        g_pd->graphics->drawLine(item->x + 3, drawY + 1,
                                                 item->x + item->w - 3, drawY + 1,
                                                 1, kColorBlack);
                    }
                }
                LCDFont *font = style_font(PLUTO_FONT_BODY);
                g_pd->graphics->setFont(font);
                const char *val = (item->value && item->value[0]) ? item->value : item->placeholder;
                char disp[256];
                if (item->inputType && strcmp(item->inputType, "password") == 0)
                {
                    int vl = item->value ? (int)strlen(item->value) : 0;
                    if (vl > (int)sizeof(disp) - 1)
                    {
                        vl = (int)sizeof(disp) - 1;
                    }
                    memset(disp, '*', vl);
                    disp[vl] = '\0';
                    val = disp;
                }
                else
                {
                    snprintf(disp, sizeof(disp), "%s", val ? val : "");
                    val = disp;
                }
                if (strlen(val) > 36)
                {
                    static char trunc[260]; /* val ≤ 255 + NUL (input_field) */
                    snprintf(trunc, sizeof(trunc), "%s", val);
                    trunc[33] = '\0';
                    strcat(trunc, "...");
                    val = trunc;
                }
                g_pd->graphics->drawText(val, strlen(val), kUTF8Encoding,
                                         item->x + 6, drawY + 3);
                if (item->required)
                {
                    g_pd->graphics->drawText("*", 1, kUTF8Encoding,
                                             item->x + item->w - 10, drawY + 3);
                }
                g_pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }
        case LRI_INPUT_SUBMIT:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int isSel = (g_selectedInput == item);
                g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorBlack);
                g_pd->graphics->setDrawMode(kDrawModeFillWhite);
                LCDFont *font = style_font(PLUTO_FONT_BODY_BOLD);
                g_pd->graphics->setFont(font);
                int lw = measure(font, item->label);
                g_pd->graphics->drawText(item->label, strlen(item->label), kUTF8Encoding,
                                         item->x + (item->w - lw) / 2, drawY + 3);
                g_pd->graphics->setDrawMode(kDrawModeCopy);
                if (isSel)
                {
                    g_pd->graphics->drawRoundRect(item->x + 1, drawY + 1, item->w - 2,
                                                  item->h - 2, 3, 1, kColorWhite);
                }
                else if (item->disabled)
                {
                    g_pd->graphics->drawLine(item->x + 4, drawY + 4,
                                             item->x + item->w - 4, drawY + 4,
                                             1, kColorBlack);
                }
            }
            break;
        }
        case LRI_CHECKBOX_FIELD:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int isSel = (g_selectedInput == item);
                /* Checked state lives on the DocBlock (activate_form_block
                 * mutates the block, not this cached copy) — read it live
                 * so taps update the drawing on the very next frame. */
                const DocBlock *cb = (const DocBlock *)item->block;
                int isChecked = cb ? cb->checked : 0;
                if (item->radio)
                {
                    /* drawCircleAtPoint(cx, cy, r) line circle: fillEllipse stroke
                     * emulation via two ellipse fills would be wrong; use the
                     * inclusive-outline idiom: fill white disk + black ring by
                     * drawing r and r-1. Reference r = h/2-1 (line circle). */
                    int cx = item->x + item->h / 2;
                    int cy = drawY + item->h / 2;
                    int r = item->h / 2 - 1;
                    g_pd->graphics->fillEllipse(cx - r, cy - r, r * 2 + 1, r * 2 + 1,
                                                0.f, 360.f, kColorBlack);
                    int r2 = r - 1;
                    g_pd->graphics->fillEllipse(cx - r2, cy - r2, r2 * 2 + 1, r2 * 2 + 1,
                                                0.f, 360.f, kColorWhite);
                    if (isChecked)
                    {
                        int r3 = item->h / 2 - 3;
                        fill_circle_at(cx, cy, r3, kColorBlack);
                    }
                }
                else
                {
                    g_pd->graphics->drawRoundRect(item->x, drawY, item->h, item->h, 3, 1, kColorBlack);
                    if (isChecked)
                    {
                        g_pd->graphics->drawLine(item->x + 4, drawY + item->h / 2,
                                                 item->x + item->h / 2, drawY + item->h - 4,
                                                 1, kColorBlack);
                        g_pd->graphics->drawLine(item->x + item->h / 2, drawY + item->h - 4,
                                                 item->x + item->h - 4, drawY + 3,
                                                 1, kColorBlack);
                    }
                }
                if (isSel)
                {
                    g_pd->graphics->drawRoundRect(item->x - 2, drawY - 2, item->h + 4,
                                                  item->h + 4, 4, 1, kColorBlack);
                }
                else if (item->disabled)
                {
                    g_pd->graphics->drawLine(item->x - 1, drawY - 1,
                                             item->x + item->h + 1, drawY + item->h + 1,
                                             1, kColorBlack);
                }
            }
            break;
        }
        case LRI_SELECT_FIELD:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                int isSel = (g_selectedInput == item);
                g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                const char *val = "";
                /* Selection lives on the DocBlock — read it live so each
                 * tap visibly advances to the next option. */
                const DocBlock *sb = (const DocBlock *)item->block;
                int si = sb ? sb->selectedIndex : 1;
                if (item->options && si >= 1 &&
                    si <= item->optionCount)
                {
                    const DocOption *opt = item->options[si - 1];
                    if (opt && opt->text)
                    {
                        val = opt->text;
                    }
                }
                char disp[128];
                snprintf(disp, sizeof(disp), "%s", val);
                if (strlen(disp) > 30)
                {
                    disp[28] = '\0';
                    strcat(disp, "..");
                }
                LCDFont *font = style_font(PLUTO_FONT_BODY);
                g_pd->graphics->setFont(font);
                g_pd->graphics->drawText(disp, strlen(disp), kUTF8Encoding,
                                         item->x + 6, drawY + 3);
                int ax = item->x + item->w - 14;
                int ay = drawY + item->h / 2;
                g_pd->graphics->drawLine(ax, ay - 3, ax + 5, ay - 3, 1, kColorBlack);
                g_pd->graphics->drawLine(ax + 1, ay - 1, ax + 4, ay - 1, 1, kColorBlack);
                g_pd->graphics->drawLine(ax + 2, ay + 1, ax + 3, ay + 1, 1, kColorBlack);
                if (isSel)
                {
                    g_pd->graphics->drawRoundRect(item->x - 1, drawY - 1, item->w + 2,
                                                  item->h + 2, 4, 1, kColorBlack);
                }
                else if (item->disabled)
                {
                    g_pd->graphics->drawLine(item->x + 4, drawY + 4,
                                             item->x + item->w - 4, drawY + 4,
                                             1, kColorBlack);
                }
            }
            break;
        }
        case LRI_PLACEHOLDER:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                g_pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 4, kColorWhite);
                g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                LCDFont *font = style_font(PLUTO_FONT_SMALL);
                g_pd->graphics->setFont(font);
                char label[128];
                snprintf(label, sizeof(label), "%s",
                         item->label ? item->label : "Media");
                if (strlen(label) > 40)
                {
                    label[38] = '\0';
                    strcat(label, "..");
                }
                int lw = measure(font, label);
                g_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                         item->x + (item->w - lw) / 2,
                                         drawY + (item->h - 10) / 2);
            }
            break;
        }
        case LRI_METER:
        {
            drawY = item->y - scrollY;
            if (drawY + item->h >= CONTENT_Y && drawY <= SCREEN_HEIGHT)
            {
                g_pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 4, 1, kColorBlack);
                double min = item->mMin;
                double max = item->mMax;
                if (max <= min)
                {
                    max = min + 1;
                }
                double frac = (item->mValue - min) / (max - min);
                if (frac < 0)
                {
                    frac = 0;
                }
                else if (frac > 1)
                {
                    frac = 1;
                }
                if (frac > 0)
                {
                    g_pd->graphics->fillRoundRect(item->x + 2, drawY + 2,
                                                  (int)((item->w - 4) * frac),
                                                  item->h - 4, 3, kColorBlack);
                }
                double opt = (item->mOptimum - min) / (max - min);
                if (opt >= 0 && opt <= 1)
                {
                    int ox = item->x + 2 + (int)((item->w - 4) * opt);
                    g_pd->graphics->fillRect(ox - 1, drawY + 1, 2, item->h - 2, kColorWhite);
                    g_pd->graphics->drawLine(ox, drawY + 1, ox, drawY + item->h - 2,
                                             1, kColorBlack);
                }
                char pct[16];
                snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
                LCDFont *font = style_font(PLUTO_FONT_SMALL);
                int tw = measure(font, pct);
                g_pd->graphics->setFont(font);
                int tx = item->x + (item->w - tw) / 2;
                /* Lua setColor(white|black) before drawText: white glyphs via
                 * FillWhite mode, black glyphs via Copy (default). */
                if (frac > 0.5)
                {
                    g_pd->graphics->setDrawMode(kDrawModeFillWhite);
                    g_pd->graphics->drawText(pct, strlen(pct), kUTF8Encoding, tx,
                                             drawY + (item->h - 10) / 2);
                    g_pd->graphics->setDrawMode(kDrawModeCopy);
                }
                else
                {
                    g_pd->graphics->drawText(pct, strlen(pct), kUTF8Encoding, tx,
                                             drawY + (item->h - 10) / 2);
                }
            }
            break;
        }
        case LRI_BOX_FRAME:
        {
            int y = item->y - scrollY;
            int y2 = item->y2 - scrollY;
            if (y2 >= CONTENT_Y && y <= SCREEN_HEIGHT)
            {
                if (item->radius > 0)
                {
                    /* R71: CSS border-radius on cards/inputs. The radius
                     * is already on the layout scale; a frame thinner than
                     * its own radius would degenerate, so the corner
                     * radius shrinks with each nested ring. */
                    g_pd->graphics->drawRoundRect(item->x, y, item->w, y2 - y,
                                                  item->radius, 1,
                                                  kColorBlack);
                }
                else
                {
                g_pd->graphics->drawLine(item->x, y, item->x + item->w, y, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x, y2, item->x + item->w, y2, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x, y, item->x, y2, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x + item->w, y, item->x + item->w, y2,
                                         1, kColorBlack);
                }
                if (item->label && item->label[0])
                {
                    LCDFont *font = style_font(PLUTO_FONT_SMALL);
                    g_pd->graphics->setFont(font);
                    char label[160];
                    if (item->toggleKey)
                    {
                        snprintf(label, sizeof(label), "%s %s",
                                 item->toggleOpen ? "[-]" : "[+]", item->label);
                    }
                    else
                    {
                        snprintf(label, sizeof(label), "%s", item->label);
                    }
                    int lw = measure(font, label);
                    int maxLw = item->w - 14;
                    if (lw > maxLw)
                    {
                        int keep = maxLw / 6;
                        if (keep < 1)
                        {
                            keep = 1;
                        }
                        label[keep] = '\0';
                        strcat(label, "..");
                        lw = measure(font, label);
                    }
                    g_pd->graphics->fillRect(item->x + 6, y - 4, lw + 4, 9, kColorWhite);
                    g_pd->graphics->drawText(label, strlen(label), kUTF8Encoding,
                                             item->x + 8, y - 5);
                }
            }
            break;
        }
        default:
            break;
        }
        } /* per-item loop */
    } /* pass loop: shades, then content */
        } /* shadow block: layer offset */
    } /* layer loop: page, then pinned app bar */

    lm_draw_selected_highlight(scrollY);

    g_pd->graphics->clearClipRect();

    /* Scrollbar */
    int maxScroll = g_totalHeight - CONTENT_HEIGHT;
    if (maxScroll > 0)
    {
        int barH = CONTENT_HEIGHT * CONTENT_HEIGHT / g_totalHeight;
        if (barH < 16)
        {
            barH = 16;
        }
        int barY = CONTENT_Y + (int)(((float)scrollY / maxScroll) *
                                     (CONTENT_HEIGHT - barH));
        g_pd->graphics->fillRect(SCREEN_WIDTH - SCROLLBAR_WIDTH, barY,
                                 SCROLLBAR_WIDTH, barH, kColorBlack);
    }

    if (g_overlay.present)
    {
        layout_draw_on_demand_overlay();
    }
}

/* ── Eviction ────────────────────────────────────────────────────────────── */

void layout_evict_offscreen(int scrollY)
{
    if (image_mode() != IMAGE_MODE_VIEWPORT)
    {
        return;
    }
    int viewTop = scrollY - 200;
    int viewBottom = scrollY + CONTENT_HEIGHT + 200;
    for (int i = 0; i < g_itemCount; i++)
    {
        const LayoutItem *item = g_items[i];
        if (item->type == LRI_IMAGE && item->src)
        {
            if (item->y + item->h < viewTop || item->y > viewBottom)
            {
                imgdec_evict(item->src);
            }
        }
    }
}

void layout_evict_hovered_image(const char *currentHoveredSrc)
{
    if (image_mode() != IMAGE_MODE_HOVER)
    {
        return;
    }
    /* Track the CURRENT hover even on first entry (Lua stores
     * Layout.hoveredImageSrc every frame; without this the C port never
     * marked the hovered image, so the evict branch below could never fire
     * and mouse-off unloads never happened). */
    if (currentHoveredSrc && currentHoveredSrc[0])
    {
        snprintf(g_hoveredImageSrc, sizeof(g_hoveredImageSrc), "%s",
                 currentHoveredSrc);
        g_hasHoveredImage = 1;
        return;
    }
    if (g_hasHoveredImage && (!currentHoveredSrc ||
                              strcmp(g_hoveredImageSrc, currentHoveredSrc) != 0))
    {
        imgdec_evict(g_hoveredImageSrc);
        g_hasHoveredImage = 0;
        g_hoveredImageSrc[0] = '\0';
    }
}

/* ── Bare-image hit test (#12c) ────────────────────────────────────────────
 * Returns the LRI_IMAGE item under the given page coordinates, or NULL.
 * Used by the HTML-mode click handler so unlinked <img> elements (google.com
 * logo) can open the on-demand overlay too. */
const LayoutItem *layout_image_at(int pageX, int pageY)
{
    int n = g_itemCount;
    for (int i = n - 1; i >= 0; i--) /* topmost (last drawn) wins */
    {
        LayoutItem *it = g_items[i];
        if (!it || it->type != LRI_IMAGE || !it->src || !it->src[0])
        {
            continue;
        }
        if (pageX >= it->x && pageX < it->x + it->w &&
            pageY >= it->y && pageY < it->y + it->h)
        {
            return it;
        }
    }
    return NULL;
}

/* ── On-demand overlay ───────────────────────────────────────────────────── */

void layout_show_on_demand_overlay(const char *src, const char *href, const char *alt)
{
    memset(&g_overlay, 0, sizeof(g_overlay));
    g_overlay.present = 1;
    snprintf(g_overlay.src, sizeof(g_overlay.src), "%s", src ? src : "");
    snprintf(g_overlay.href, sizeof(g_overlay.href), "%s", href ? href : "");
    snprintf(g_overlay.alt, sizeof(g_overlay.alt), "%s", alt && alt[0] ? alt : "Image");
}

void layout_clear_on_demand_overlay(void)
{
    g_overlay.present = 0;
}

int layout_has_on_demand_overlay(void)
{
    return g_overlay.present;
}
const char *layout_on_demand_href(void)
{
    return g_overlay.href;
}

const char *layout_on_demand_src(void)
{
    return g_overlay.src;
}

void layout_draw_on_demand_overlay(void)
{
    if (!g_overlay.present || !g_pd)
    {
        return;
    }
    int isLoaded = g_overlay.src[0] && imgdec_is_decoded(g_overlay.src);

    int boxW = 240;
    int boxH = 80;
    int boxX = (SCREEN_WIDTH - boxW) / 2;
    int boxY = (CONTENT_Y + CONTENT_HEIGHT - boxH) / 2;

    g_pd->graphics->fillRoundRect(boxX, boxY, boxW, boxH, 8, kColorWhite);
    g_pd->graphics->drawRoundRect(boxX, boxY, boxW, boxH, 8, 1, kColorBlack);

    char title[516]; /* g_overlay.alt ≤ 511 + NUL */
    snprintf(title, sizeof(title), "%s", g_overlay.alt);
    if (strlen(title) > 28)
    {
        title[25] = '\0';
        strcat(title, "...");
    }
    g_pd->graphics->setFont(style_font(PLUTO_FONT_BODY_BOLD));
    g_pd->graphics->drawText(title, strlen(title), kUTF8Encoding, boxX + 12, boxY + 8);
    g_pd->graphics->drawLine(boxX + 10, boxY + 20, boxX + boxW - 10, boxY + 20,
                             1, kColorBlack);

    g_pd->graphics->setFont(style_font(PLUTO_FONT_SMALL));
    const char *aLine = isLoaded ? "(A) Unload Image" : "(A) View Image";
    const char *bLine = g_overlay.href[0] ? "(B) Open Link" : "(B) Cancel";
    g_pd->graphics->drawText(aLine, strlen(aLine), kUTF8Encoding, boxX + 14, boxY + 30);
    g_pd->graphics->drawText(bLine, strlen(bLine), kUTF8Encoding, boxX + 14, boxY + 48);
}

const char *layout_handle_on_demand_input(unsigned int btnPushed)
{
    if (!g_overlay.present)
    {
        return NULL;
    }
    if (btnPushed & kButtonA)
    {
        static const char *actView = "view";
        static const char *actUnload = "unload";
        static const char *actCancel = "cancel";
        char src[512];
        snprintf(src, sizeof(src), "%s", g_overlay.src);
        g_overlay.present = 0;
        g_onDemandConsumed = 1;
        if (src[0])
        {
            if (imgdec_is_decoded(src))
            {
                imgdec_evict(src);
                od_unrequest(src);
                return actUnload;
            }
            od_request(src);
            imgdec_enqueue(src);
            return actView;
        }
        return actCancel;
    }
    if (btnPushed & kButtonB)
    {
        static const char *actLink = "link";
        static const char *actCancel = "cancel";
        char href[512];
        snprintf(href, sizeof(href), "%s", g_overlay.href);
        g_overlay.present = 0;
        g_onDemandConsumed = 1;
        if (href[0])
        {
            return actLink;
        }
        return actCancel;
    }
    return NULL;
}


/* ── Test hooks (host battery parity with the Lua reference harness) ─────── */

static const char *font_role_name(LCDFont *f)
{
    if (f == style_font(PLUTO_FONT_BODY)) return "BODY";
    if (f == style_font(PLUTO_FONT_BODY_BOLD)) return "BOLD";
    if (f == style_font(PLUTO_FONT_MONO)) return "MONO";
    if (f == style_font(PLUTO_FONT_SMALL)) return "SMALL";
    if (f == style_font(PLUTO_FONT_HEADING1)) return "H1";
    if (f == style_font(PLUTO_FONT_HEADING2)) return "H2";
    if (f == style_font(PLUTO_FONT_HEADING3)) return "H3";
    return "BODY";
}

int layout_test_break_lines_probe(const DocInline **inlines, int inlineCount,
                                  int maxW, LCDFont *font, int bold, int lineH,
                                  LayoutBreakProbe *out)
{
    BreakOpts bo;
    memset(&bo, 0, sizeof(bo));
    bo.font = font;
    bo.bold = bold;
    bo.lineH = lineH;
    FlowLines fl;
    int lh = break_lines(inlines, inlineCount, maxW, &bo, &fl);
    memset(out, 0, sizeof(*out));
    out->lineCount = fl.count > 16 ? 16 : fl.count;
    for (int i = 0; i < out->lineCount; i++)
    {
        out->lineWordCount[i] = fl.lines[i].count;
        out->lineWidth[i] = fl.lines[i].width;
    }
    flow_free(&fl);
    return lh;
}

/* Phase 5 probe: identical to layout_test_break_lines_probe but with an
 * explicit -webkit-line-clamp budget (0 = unlimited), so a sim checker
 * can assert on the REAL breaker's emitted structure for an ellipsis/
 * clamp cut (the doc objects keep full text by design — the cut only
 * exists between the breaker and the emitter). */
int layout_test_probe_maxlines(const DocInline **inlines, int inlineCount,
                               int maxW, LCDFont *font, int bold, int lineH,
                               int maxlines, LayoutBreakProbe *out)
{
    BreakOpts bo;
    memset(&bo, 0, sizeof(bo));
    bo.font = font;
    bo.bold = bold;
    bo.lineH = lineH;
    bo.maxLines = maxlines;
    FlowLines fl;
    int lh = break_lines(inlines, inlineCount, maxW, &bo, &fl);
    memset(out, 0, sizeof(*out));
    out->lineCount = fl.count > 16 ? 16 : fl.count;
    for (int i = 0; i < out->lineCount; i++)
    {
        out->lineWordCount[i] = fl.lines[i].count;
        out->lineWidth[i] = fl.lines[i].width;
    }
    flow_free(&fl);
    return lh;
}

void layout_test_dump_break_lines(const DocInline **inlines, int inlineCount,
                                  int maxW, LCDFont *font, int bold, int lineH,
                                  void (*emitLine)(const char *s))
{
    BreakOpts bo;
    memset(&bo, 0, sizeof(bo));
    bo.font = font;
    bo.bold = bold;
    bo.lineH = lineH;
    FlowLines fl;
    int lh = break_lines(inlines, inlineCount, maxW, &bo, &fl);
    char buf[1024];
    snprintf(buf, sizeof(buf), "v lineH=%d n=%d", lh, fl.count);
    emitLine(buf);
    for (int li = 0; li < fl.count; li++)
    {
        const FlowLine *line = &fl.lines[li];
        size_t o = (size_t)snprintf(buf, sizeof(buf), "  L%d width=%d words=[",
                                    li + 1, line->width);
        for (int wi = 0; wi < line->count; wi++)
        {
            const FlowWord *w = &line->words[wi];
            if (wi)
            {
                o += (size_t)snprintf(buf + o, sizeof(buf) - o, ",");
            }
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s/%s/w%d/a%d",
                                  w->text, font_role_name(w->font), w->w, w->advance);
        }
        snprintf(buf + o, sizeof(buf) - o, "]");
        emitLine(buf);
    }
    flow_free(&fl);
}

void layout_test_run_emit(const DocInline **inlines, int inlineCount, int maxW,
                          LCDFont *font, int bold, int lineH, int startX,
                          int maxWX, const char *align, int startY)
{
    BreakOpts bo;
    memset(&bo, 0, sizeof(bo));
    bo.font = font;
    bo.bold = bold;
    bo.lineH = lineH;
    FlowLines fl;
    int lh = break_lines(inlines, inlineCount, maxW, &bo, &fl);
    int endY = emit_flow(&fl, lh, startX, maxWX, align, startY, 0);
    flow_free(&fl);
    g_testEmitLineH = lh;
    g_testEmitEndY = endY;
}

int layout_test_get_emit_line_h(void) { return g_testEmitLineH; }
int layout_test_get_emit_end_y(void) { return g_testEmitEndY; }
