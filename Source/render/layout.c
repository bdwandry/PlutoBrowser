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

/* word-text arena: breakLines words live here until layout_clear() */
static char *g_wordArena = NULL;
static size_t g_wordArenaUsed = 0;
static size_t g_wordArenaCap = 0;

static const char *arena_word(const char *s)
{
    size_t n = strlen(s) + 1;
    if (g_wordArenaUsed + n > g_wordArenaCap)
    {
        size_t nc = g_wordArenaCap ? g_wordArenaCap * 2 : 4096;
        while (nc < g_wordArenaUsed + n)
        {
            nc *= 2;
        }
        char *na = (char *)realloc(g_wordArena, nc);
        if (!na)
        {
            return s; /* fall back to caller-owned storage */
        }
        g_wordArena = na;
        g_wordArenaCap = nc;
    }
    char *dst = g_wordArena + g_wordArenaUsed;
    memcpy(dst, s, n);
    g_wordArenaUsed += n;
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
    g_wordArenaUsed = 0;
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
} BreakOpts;

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
    FlowLine cur;
    memset(&cur, 0, sizeof(cur));
    int firstWord = 1;

    for (int ii = 0; ii < inlineCount; ii++)
    {
        const DocInline *inl = inlines[ii];
        if (!inl)
        {
            continue;
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

        const char *text = inl->text;
        int spaceW = measure(font, " ");
        if (spaceW <= 0)
        {
            spaceW = 8;
        }
        int pos = 1;
        int n = (int)strlen(text);
        while (pos <= n)
        {
            while (pos <= n && is_space_char(text[pos - 1]))
            {
                pos++;
            }
            if (pos > n)
            {
                break;
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
            if (cur.width + aw > maxW && !firstWord)
            {
                flow_push_line(fl, &cur);
                firstWord = 1;
                aw = ww;
                gap = 0;
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

void layout_build(DocParseResult *doc)
{
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

    for (int bi = 0; bi < doc->blockCount; bi++)
    {
        DocBlock *block = doc->blocks[bi];
        if (!block)
        {
            continue;
        }
        /* Tasks.yieldCheck() call site: outside a task context the C port's
         * budget check is a no-op (build runs inside the navigation task on
         * device, which owns the frame budget). */
        tasks_report_progress(0.8f + 0.2f * ((float)(bi + 1) / (float)doc->blockCount));

        /* 1. Reader Header Banner */
        if (block->type == DOC_BLOCK_READER_HEADER)
        {
            int badgeH = 28;
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
            currentY += marginB + block->spacingBottom;
        }
        /* 3. MathML linearized formulas */
        else if (block->type == DOC_BLOCK_MATH)
        {
            int startY = currentY + block->spacingTop;
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
            int startQuoteY = currentY + block->spacingTop;
            const char *align = layout_normalize_align(block->align);
            BreakOpts bo;
            memset(&bo, 0, sizeof(bo));
            FlowLines fl;
            int lineH = break_lines((const DocInline **)block->inlines,
                                    block->inlineCount, blockMaxW, &bo, &fl);
            emit_flow(&fl, lineH, blockStartX, blockMaxW, align, startQuoteY,
                      block->invert);
            currentY = startQuoteY + lineH * fl.count + 10 + block->spacingBottom;
            flow_free(&fl);
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
        }
        /* 4. Lists (ul / ol / dl) */
        else if (block->type == DOC_BLOCK_LIST_ITEM)
        {
            int depth = block->depth ? block->depth : 1;
            int isDt = block->dt;
            int isDd = block->dd;
            int indent = (depth - 1) * 14;
            int bulletIndent = marginX + 4 + indent;
            int textIndent = marginX + (isDd ? 30 : 20) + indent;
            int listMaxW = maxWidth - (textIndent - marginX) - 4;
            int lineH = 18;

            if (!isDt && !isDd)
            {
                char bulletStr[48];
                if (block->isOrdered)
                {
                    char marker[40];
                    layout_ordered_marker(block->number, block->markerType,
                                          marker, sizeof(marker));
                    snprintf(bulletStr, sizeof(bulletStr), "%s.", marker);
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
            FlowLines fl;
            int lh = break_lines((const DocInline **)block->inlines,
                                 block->inlineCount, listMaxW, &bo, &fl);
            emit_flow(&fl, lh, textIndent, listMaxW, align, currentY, 0);
            currentY = currentY + lh * fl.count + 6;
            flow_free(&fl);
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
                char buf[1024];
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
                    if (bo2 + 1 < sizeof(buf))
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
            currentY += boxH + 10;
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

            currentY += imgH + 12;
        }
        /* 8. Tables */
        else if (block->type == DOC_BLOCK_TABLE && block->table)
        {
            const DocTable *tbl = block->table;
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
                    if (strchr(wstr, '%'))
                    {
                        g_buildError = 1; /* reference raises here */
                        return;
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
            currentY += tableH + 10;
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
            it->checked = block->checked;
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
            it->selectedIndex = block->selectedIndex ? block->selectedIndex : 1;
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
            currentY += boxH + 10;
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
    }

    int th = currentY + 20;
    g_totalHeight = th > CONTENT_HEIGHT ? th : CONTENT_HEIGHT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 2: state accessors, Layout.draw, eviction, on-demand overlay.
 * ═══════════════════════════════════════════════════════════════════════════ */

int layout_get_total_height(void) { return g_totalHeight; }
int layout_get_item_count(void) { return g_itemCount; }

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
                int cw = colW * span;
                char txt[1024];
                size_t to = 0;
                txt[0] = '\0';
                for (int ii = 0; ii < cell->inlineCount; ii++)
                {
                    const DocInline *inl = cell->inlines[ii];
                    if (inl && inl->text)
                    {
                        size_t tl = strlen(inl->text);
                        if (to + tl < sizeof(txt) - 1)
                        {
                            strcat(txt + to, inl->text);
                            to += tl;
                        }
                    }
                }
                /* gsub("%s+", " ") + trim: collapse then trim in place */
                {
                    char out[1024];
                    size_t o = 0;
                    int inWs = 0;
                    for (const char *p = txt; *p; p++)
                    {
                        if (is_space_char(*p))
                        {
                            if (!inWs && o < sizeof(out) - 1)
                            {
                                out[o++] = ' ';
                            }
                            inWs = 1;
                        }
                        else
                        {
                            if (o < sizeof(out) - 1)
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
                int maxChars = cw / 7;
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
                int tx = cellX;
                if (cellAlign && strcmp(cellAlign, "center") == 0)
                {
                    tx = cellX + (cw - tw) / 2;
                }
                else if (cellAlign && strcmp(cellAlign, "right") == 0)
                {
                    tx = cellX + (cw - tw > 0 ? cw - tw : 0);
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
            }
        }
        rowY += 18;
        if (rowY < drawY + item->h - 4)
        {
            g_pd->graphics->drawLine(item->x, rowY - 2, item->x + item->w, rowY - 2,
                                     1, kColorBlack);
        }
    }
    g_cellLinksRegistered = 1;
}


void layout_draw(int scrollY)
{
    if (!g_pd)
    {
        return;
    }
    g_scrollYForLinks = scrollY;

    g_pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT, kColorWhite);
    g_pd->graphics->setClipRect(0, CONTENT_Y, CONTENT_WIDTH, CONTENT_HEIGHT);

    for (int i = 0; i < g_itemCount; i++)
    {
        LayoutItem *item = g_items[i];
        int drawY;
        switch (item->type)
        {
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
                        char expanded[1024];
                        layout_expand_tab_columns(item->lines[li], expanded, sizeof(expanded));
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
                    if (item->checked)
                    {
                        int r3 = item->h / 2 - 3;
                        fill_circle_at(cx, cy, r3, kColorBlack);
                    }
                }
                else
                {
                    g_pd->graphics->drawRoundRect(item->x, drawY, item->h, item->h, 3, 1, kColorBlack);
                    if (item->checked)
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
                if (item->options && item->selectedIndex >= 1 &&
                    item->selectedIndex <= item->optionCount)
                {
                    const DocOption *opt = item->options[item->selectedIndex - 1];
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
                g_pd->graphics->drawLine(item->x, y, item->x + item->w, y, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x, y2, item->x + item->w, y2, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x, y, item->x, y2, 1, kColorBlack);
                g_pd->graphics->drawLine(item->x + item->w, y, item->x + item->w, y2,
                                         1, kColorBlack);
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
    }

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
    if (g_hasHoveredImage && (!currentHoveredSrc ||
                              strcmp(g_hoveredImageSrc, currentHoveredSrc) != 0))
    {
        imgdec_evict(g_hoveredImageSrc);
        g_hasHoveredImage = 0;
        g_hoveredImageSrc[0] = '\0';
    }
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
