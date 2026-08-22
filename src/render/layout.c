// layout.c — [P26B] C port of Source/render/layout.lua (Layout).
//
// Block layout engine: walks DocDocument blocks and flattens them into
// positioned render items, registering link hitboxes in LinkManager; the
// paint half draws them scrolled inside the content viewport.
//
// Faithful parity notes are collected in render/layout.h.

#include "render/layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/constants.h"
#include "core/storage.h"
#include "core/tasks.h"
#include "html/document.h"
#include "render/image_decoder.h"
#include "render/link_manager.h"
#include "render/style.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#include "pd_api.h"
#endif

#define TAB_COLUMNS 8
#define BOX_STACK_MAX 64

static struct PlaydateAPI* s_pd = NULL;

/* ── module state (Lua Layout table fields) ───────────────────────────── */

static LItem* s_items        = NULL;
static int    s_nItems       = 0;
static int    s_capItems     = 0;
static double s_totalHeight  = 0.0;

static const LItem* s_selectedInput = NULL;   /* Layout.selectedInputItem */

static char* s_odSrc   = NULL;                /* onDemandOverlay */
static char* s_odHref  = NULL;
static char* s_odAlt   = NULL;
static int   s_odConsumed = 0;                /* onDemandConsumed */
static char** s_odRequested = NULL;           /* onDemandRequested set */
static int    s_nReq = 0, s_capReq = 0;

static char* s_hoveredImageSrc = NULL;

static int s_cellLinksRegistered = 0;

void layout_init(struct PlaydateAPI* pd) { s_pd = pd; }

/* ── item storage helpers ─────────────────────────────────────────────── */

static LItem* new_item(int type) {
    if (s_nItems == s_capItems) {
        s_capItems = s_capItems ? s_capItems * 2 : 64;
        s_items = (LItem*)pluto_realloc(s_items,
                                        sizeof(LItem) * (size_t)s_capItems);
    }
    LItem* it = &s_items[s_nItems++];
    memset(it, 0, sizeof(*it));
    it->type        = type;
    it->anchorIndex = -1;
    it->maxlength   = -1;
    it->fieldWidth  = -1;
    it->fieldRows   = -1;
    return it;
}

static void free_item(LItem* it) {
    pluto_free(it->text);
    pluto_free(it->href);
    pluto_free(it->host);
    pluto_free(it->readingTime);
    if (it->lines) {
        for (size_t i = 0; i < it->nLines; i++) pluto_free(it->lines[i]);
        pluto_free(it->lines);
    }
    pluto_free(it->caption);
    pluto_free(it->alt);
    pluto_free(it->src);
    pluto_free(it->inputType);
    pluto_free(it->name);
    pluto_free(it->value);
    pluto_free(it->placeholder);
    pluto_free(it->formAction);
    pluto_free(it->formMethod);
    pluto_free(it->label);
    pluto_free(it->toggleKey);
}

void layout_free_items(void) {
    for (int i = 0; i < s_nItems; i++) free_item(&s_items[i]);
    pluto_free(s_items);
    s_items = NULL; s_nItems = 0; s_capItems = 0;
}

int          layout_item_count(void)      { return s_nItems; }
const LItem* layout_item_at(int i)        { return (i >= 0 && i < s_nItems) ? &s_items[i] : NULL; }
double       layout_total_height(void)    { return s_totalHeight; }

const LItem* layout_selected_input(void)            { return s_selectedInput; }
void         layout_set_selected_input(const LItem* it) { s_selectedInput = it; }

const char* layout_hovered_image_src(void)              { return s_hoveredImageSrc; }
void        layout_set_hovered_image_src(const char* s) {
    pluto_free(s_hoveredImageSrc);
    s_hoveredImageSrc = (s && s[0]) ? pluto_strdup(s) : NULL;
}

/* ── text helpers ─────────────────────────────────────────────────────── */

static const char* normalize_align(const char* a) {
    if (!a) return NULL;
    if (!strcmp(a, "center")) return "center";
    if (!strcmp(a, "right"))  return "right";
    if (!strcmp(a, "left"))   return "left";
    return NULL;   /* document stores pre-lowered statics */
}

static void to_roman(int n, char* out, size_t cap) {
    static const struct { int v; const char* s; } map[] = {
        {1000,"M"},{900,"CM"},{500,"D"},{400,"CD"},{100,"C"},{90,"XC"},
        {50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"}
    };
    if (n <= 0 || n > 3999) {
        snprintf(out, cap, "%d", n);   /* Lua tostring(n): prints as-is */
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]) && k < cap - 1; i++) {
        while (n >= map[i].v && k < cap - 1) {
            size_t l = strlen(map[i].s);
            if (k + l >= cap) return;
            memcpy(out + k, map[i].s, l);
            k += l;
            n -= map[i].v;
        }
    }
    out[k] = '\0';
}

static void to_alpha(int n, char* out, size_t cap) {
    char tmp[40];
    size_t k = 0;
    while (n > 0 && k < sizeof(tmp)) {
        int rem = (n - 1) % 26;
        tmp[k++] = (char)('a' + rem);
        n = (n - 1) / 26;
    }
    if (k == 0 || k >= cap) { snprintf(out, cap, "1"); return; }
    for (size_t i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    out[k] = '\0';
}

char* layout_ordered_marker(int number, const char* markerType) {
    char buf[48];
    /* callers resolve `number or 1` before this point (Lua parity) */
    if (!markerType || !markerType[0] || !strcmp(markerType, "1")) {
        snprintf(buf, sizeof(buf), "%d", number);
        return pluto_strdup(buf);
    }
    if (!strcmp(markerType, "a")) { to_alpha(number, buf, sizeof(buf)); return pluto_strdup(buf); }
    if (!strcmp(markerType, "A")) {
        to_alpha(number, buf, sizeof(buf));
        for (char* p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return pluto_strdup(buf);
    }
    if (!strcmp(markerType, "i") || !strcmp(markerType, "I")) {
        to_roman(number, buf, sizeof(buf));
        if (markerType[0] == 'i')
            for (char* p = buf; *p; p++)
                if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        return pluto_strdup(buf);
    }
    snprintf(buf, sizeof(buf), "%d", number);
    return pluto_strdup(buf);
}

/* pixel advance needed to reach the next TAB_COLUMNS stop from runningWidth */
static int tab_advance(PlutoFont* font, int runningWidth) {
    int spaceW = style_get_text_width(font, " ");
    if (spaceW <= 0) spaceW = 8;
    int tabPx = TAB_COLUMNS * spaceW;
    int toTab = tabPx - (runningWidth % tabPx);
    if (toTab <= 0) toTab = tabPx;
    return toTab;
}

char* layout_expand_tab_columns(const char* line) {
    if (!line || !strchr(line, '\t')) return pluto_strdup(line ? line : "");
    size_t need = strlen(line) * TAB_COLUMNS + 1;
    char* out = (char*)pluto_malloc(need);
    size_t k = 0;
    int col = 0;
    for (const char* p = line; *p; p++) {
        if (*p == '\t') {
            int pad = TAB_COLUMNS - (col % TAB_COLUMNS);
            for (int i = 0; i < pad; i++) out[k++] = ' ';
            col += pad;
        } else {
            out[k++] = *p;
            col++;
        }
    }
    out[k] = '\0';
    return out;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

/* ── breakLines ────────────────────────────────────────────────────────── */

typedef struct {
    const char* start;      /* slice into an inline's text */
    int len;
    PlutoFont* font;
    int w, advance;
    const DocInline* inl;
} BrkWord;

typedef struct {
    BrkWord* words;
    int nWords, capWords;
    int width;
} BrkLine;

typedef struct {
    BrkLine* lines;
    int nLines, capLines;
} BrkLines;

static void brk_push_word(BrkLine* ln, BrkWord w) {
    if (ln->nWords == ln->capWords) {
        ln->capWords = ln->capWords ? ln->capWords * 2 : 8;
        ln->words = (BrkWord*)pluto_realloc(ln->words,
                                     sizeof(BrkWord) * (size_t)ln->capWords);
    }
    ln->words[ln->nWords++] = w;
}

static void brk_push_line(BrkLines* ls, BrkLine* cur) {
    if (cur->nWords == 0) return;
    if (ls->nLines == ls->capLines) {
        ls->capLines = ls->capLines ? ls->capLines * 2 : 8;
        ls->lines = (BrkLine*)pluto_realloc(ls->lines,
                                    sizeof(BrkLine) * (size_t)ls->capLines);
    }
    ls->lines[ls->nLines++] = *cur;
    cur->words = NULL; cur->nWords = 0; cur->capWords = 0; cur->width = 0;
}

static void brk_free(BrkLines* ls) {
    for (int i = 0; i < ls->nLines; i++) pluto_free(ls->lines[i].words);
    pluto_free(ls->lines);
    ls->lines = NULL; ls->nLines = 0; ls->capLines = 0;
}

/* scratch buffer so word slices can be measured as C strings without a
 * malloc per word */
static char* s_meas = NULL;
static size_t s_measCap = 0;

static const char* meas_slice(const char* start, int len) {
    if ((size_t)len + 1 > s_measCap) {
        s_measCap = (size_t)len + 32;
        s_meas = (char*)pluto_realloc(s_meas, s_measCap);
    }
    memcpy(s_meas, start, (size_t)len);
    s_meas[len] = '\0';
    return s_meas;
}

static void brk_break(const DocInline* inlines, size_t nInlines, int maxW,
                      PlutoFont* forcedFont, int forcedBold, int forcedLH,
                      BrkLines* out, int* outLineH) {
    memset(out, 0, sizeof(*out));
    BrkLine cur;
    memset(&cur, 0, sizeof(cur));
    int firstWord = 1;
    int lineH = forcedFont ? (forcedLH > 0 ? forcedLH : 16) : 16;

    for (size_t ii = 0; ii < nInlines; ii++) {
        const DocInline* inl = &inlines[ii];
        if (inl->type == DIT_BR) {
            brk_push_line(out, &cur);
            firstWord = 1;
            continue;
        }
        PlutoFont* font;
        int lh;
        if (forcedFont) {
            font = forcedFont;
            lh   = forcedLH > 0 ? forcedLH : 16;
        } else if (forcedBold) {
            font = style_get_body_font(1, 0, NULL);
            lh   = 16;
        } else {
            font = style_get_inline_font(inl->bold, inl->code, inl->small,
                                         inl->sub, inl->sup, inl->big, NULL);
            lh   = 16;
        }
        if (lh > lineH) lineH = lh;

        const char* text = inl->text ? inl->text : "";
        int n = (int)strlen(text);
        int spaceW = style_get_text_width(font, " ");
        if (spaceW <= 0) spaceW = 8;

        int pos = 0;
        while (pos < n) {
            while (pos < n && is_space(text[pos])) pos++;
            if (pos >= n) break;

            int ws = pos;
            while (pos < n && !is_space(text[pos])) pos++;
            int wwlen = pos - ws;
            int ww = style_get_text_width(font, meas_slice(text + ws, wwlen));

            /* whitespace run following this word */
            int gap = 0;
            int we = pos;
            while (we < n && is_space(text[we])) we++;
            if (we > pos) {
                int hasTab = 0;
                for (int t = pos; t < we; t++)
                    if (text[t] == '\t') { hasTab = 1; break; }
                gap = hasTab ? tab_advance(font, cur.width + ww) : spaceW;
            }

            int aw = ww + gap;
            if (cur.width + aw > maxW && !firstWord) {
                brk_push_line(out, &cur);
                firstWord = 1;
                aw = ww;
                gap = 0;
            }
            BrkWord w = { text + ws, wwlen, font, ww, ww + gap, inl };
            brk_push_word(&cur, w);
            cur.width += aw;
            firstWord = 0;
            pos = we;
        }
    }
    brk_push_line(out, &cur);
    if (outLineH) *outLineH = lineH;
}

/* ── emitFlow ──────────────────────────────────────────────────────────── */

static int floordiv2(int a) {           /* math.floor(a / 2) for any sign */
    int q = a / 2;
    if (a % 2 != 0 && a < 0) q--;
    return q;
}

/* growable link-rect text accumulator ("a b c" across merged words) */
static char* mr_text_append(char** buf, size_t* len, size_t* cap,
                            const char* word, int wlen) {
    size_t add = (size_t)wlen + 1 /*space*/;
    if (*len + add + 1 > *cap) {
        *cap = (*len + add + 1) * 2;
        *buf = (char*)pluto_realloc(*buf, *cap);
    }
    if (*len > 0) (*buf)[(*len)++] = ' ';
    memcpy(*buf + *len, word, (size_t)wlen);
    *len += (size_t)wlen;
    (*buf)[*len] = '\0';
    return *buf;
}

/* appends positioned items for already-broken lines; returns endY */
static int emit_flow(BrkLines* ls, int lineH, int startX, int maxW,
                     const char* align, int startY, int invert, int forceBold) {
    int y = startY;
    for (int li = 0; li < ls->nLines; li++) {
        const BrkLine* ln = &ls->lines[li];
        int textW = 0;
        for (int wi = 0; wi < ln->nWords; wi++) textW += ln->words[wi].w;
        int x = startX;
        if (align && !strcmp(align, "center"))
            x = startX + floordiv2(maxW - textW);
        else if (align && !strcmp(align, "right"))
            x = startX + (maxW - textW > 0 ? maxW - textW : 0);

        /* pending link rect (merged across same-href words) */
        int haveMr = 0, mrX = 0, mrW = 0, mrInert = 0;
        long mrAnchor = -1;
        const char* mrHref = NULL;
        char* mrText = NULL;
        size_t mrLen = 0, mrCap = 0;

        for (int wi = 0; wi < ln->nWords; wi++) {
            const BrkWord* wd = &ln->words[wi];
            const DocInline* inl = wd->inl;
            int dy = inl->sub ? 3 : (inl->sup ? -4 : 0);

            LItem* it = new_item(LIT_TEXT);
            it->text        = pluto_strndup(wd->start, (size_t)wd->len);
            it->font        = wd->font;
            it->x           = x;
            it->y           = y + dy;
            it->w           = wd->w;
            it->h           = lineH;
            it->bold        = inl->bold;
            it->italic      = inl->italic;
            it->underline   = inl->underline;
            it->code        = inl->code;
            it->smallFlag   = inl->small;
            it->bigFlag     = inl->big;
            it->mark        = inl->mark;
            it->strike      = inl->strike;
            it->invert      = (unsigned char)(invert ? 1 : 0);
            it->href        = inl->href ? pluto_strdup(inl->href) : NULL;
            it->anchorIndex = inl->anchorIndex;
            if (forceBold) it->bold = 1;

            if (inl->href && haveMr && mrHref == inl->href &&
                mrAnchor == inl->anchorIndex) {
                /* same link continues: extend over the inter-word gap */
                mrW = (x + wd->w) - mrX;
                mr_text_append(&mrText, &mrLen, &mrCap, wd->start, wd->len);
            } else if (inl->href) {
                if (haveMr)
                    lm_add_link_rect(mrHref, mrText ? mrText : "",
                        (LmRect){ .x = mrX, .y = y, .w = mrW, .h = lineH,
                                  .inert = mrInert }, mrAnchor);
                pluto_free(mrText);
                mrText = NULL; mrLen = 0; mrCap = 0;
                haveMr  = 1;
                mrX     = x;
                mrW     = wd->w;
                mrHref  = inl->href;
                mrAnchor= inl->anchorIndex;
                mrText  = mr_text_append(&mrText, &mrLen, &mrCap,
                                         wd->start, wd->len);
                mrInert = inl->inert ? 1 : 0;
            } else if (haveMr) {
                lm_add_link_rect(mrHref, mrText ? mrText : "",
                    (LmRect){ .x = mrX, .y = y, .w = mrW, .h = lineH,
                              .inert = mrInert }, mrAnchor);
                pluto_free(mrText);
                mrText = NULL; mrLen = 0; mrCap = 0;
                haveMr = 0;
            }
            x += wd->advance;
        }
        if (haveMr)
            lm_add_link_rect(mrHref, mrText ? mrText : "",
                (LmRect){ .x = mrX, .y = y, .w = mrW, .h = lineH,
                          .inert = mrInert }, mrAnchor);
        pluto_free(mrText);
        y += lineH;
    }
    return y;
}

/* convenience wrapper matching the common Lua call shape */
static int flow_paragraph(const DocBlock* b, int startX, int maxW, int startY,
                          int invert, int forceBold, int forceBoldFont) {
    BrkLines ls;
    int lineH = 16;
    brk_break(b->inlines, b->nInlines, maxW,
              forceBoldFont ? style_get_body_font(1, 0, NULL) : NULL,
              forceBoldFont, 0, &ls, &lineH);
    int endY = emit_flow(&ls, lineH, startX, maxW,
                         normalize_align(b->align), startY, invert, forceBold);
    brk_free(&ls);
    return endY;
}

/* ── build ─────────────────────────────────────────────────────────────── */

static const DocMap* find_map(const DocDocument* doc, const char* name) {
    if (!doc || !name) return NULL;
    for (size_t i = 0; i < doc->nMaps; i++)
        if (doc->maps[i].name && !strcmp(doc->maps[i].name, name))
            return &doc->maps[i];
    return NULL;
}

void od_requested_clear(void) {
    for (int i = 0; i < s_nReq; i++) pluto_free(s_odRequested[i]);
    pluto_free(s_odRequested);
    s_odRequested = NULL; s_nReq = 0; s_capReq = 0;
}

static void od_requested_add(const char* src) {
    if (layout_on_demand_requested(src)) return;
    if (s_nReq == s_capReq) {
        s_capReq = s_capReq ? s_capReq * 2 : 8;
        s_odRequested = (char**)pluto_realloc(s_odRequested,
                                        sizeof(char*) * (size_t)s_capReq);
    }
    s_odRequested[s_nReq++] = pluto_strdup(src);
}

int layout_on_demand_requested(const char* src) {
    if (!src) return 0;
    for (int i = 0; i < s_nReq; i++)
        if (s_odRequested[i] && !strcmp(s_odRequested[i], src)) return 1;
    return 0;
}

static void od_requested_remove(const char* src) {
    for (int i = 0; i < s_nReq; i++) {
        if (s_odRequested[i] && !strcmp(s_odRequested[i], src)) {
            pluto_free(s_odRequested[i]);
            s_odRequested[i] = s_odRequested[--s_nReq];
            return;
        }
    }
}

int layout_has_on_demand_overlay(void) { return s_odSrc != NULL; }

void layout_show_on_demand_overlay(const char* src, const char* href,
                                   const char* alt) {
    pluto_free(s_odSrc); pluto_free(s_odHref); pluto_free(s_odAlt);
    s_odSrc  = (src  && src[0])  ? pluto_strdup(src)  : NULL;
    s_odHref = (href && href[0]) ? pluto_strdup(href) : NULL;
    s_odAlt  = alt ? pluto_strdup(alt) : pluto_strdup("Image");
}

void layout_clear_on_demand_overlay(void) {
    pluto_free(s_odSrc); pluto_free(s_odHref); pluto_free(s_odAlt);
    s_odSrc = s_odHref = s_odAlt = NULL;
}

const char* layout_od_src(void)  { return s_odSrc; }
const char* layout_od_href(void) { return s_odHref; }
const char* layout_od_alt(void)  { return s_odAlt; }

int  layout_on_demand_consumed(void)         { return s_odConsumed; }
void layout_clear_on_demand_consumed(void)   { s_odConsumed = 0; }

PlutoOdAction layout_handle_on_demand_input(unsigned int pressed) {
    if (!s_odSrc && !s_odHref && !s_odAlt) return OD_NONE;

    if (pressed & PLUTO_KBUTTON_A) {
        char* src = s_odSrc;
        s_odSrc = NULL;
        s_odConsumed = 1;
        if (src) {
            if (id_is_decoded(src)) {
                id_evict(src);
                od_requested_remove(src);
                pluto_free(src);
                return OD_UNLOAD;
            }
            od_requested_add(src);
            id_enqueue(src);
            pluto_free(src);
            return OD_VIEW;
        }
        layout_clear_on_demand_overlay();
        return OD_CANCEL;
    }
    if (pressed & PLUTO_KBUTTON_B) {
        char* href = s_odHref;
        s_odHref = NULL;
        s_odConsumed = 1;
        layout_clear_on_demand_overlay();
        if (href) { pluto_free(href); return OD_LINK; }
        return OD_CANCEL;
    }
    return OD_NONE;
}

void layout_build(const struct DocDocument* docp) {
    const DocDocument* doc = (const DocDocument*)docp;

    layout_free_items();
    s_selectedInput = NULL;
    s_cellLinksRegistered = 0;
    od_requested_clear();
    layout_clear_on_demand_overlay();
    s_odConsumed = 0;
    layout_set_hovered_image_src(NULL);
    lm_clear();

    if (!doc || doc->nBlocks == 0) {
        s_totalHeight = PLUTO_CONTENT_HEIGHT;
        return;
    }

    int currentY = PLUTO_CONTENT_Y + 8;
    int marginX  = PLUTO_CONTENT_MARGIN + 2;
    int maxWidth = PLUTO_CONTENT_TEXT_WIDTH - 4;

    LItem* boxStack[BOX_STACK_MAX];
    int sp = 0;

    size_t nB = doc->nBlocks;
    for (size_t bi = 0; bi < nB; bi++) {
        const DocBlock* b = &doc->blocks[bi];
        tasks_yield_check();
        tasks_report_progress(0.8 + 0.2 * ((double)(bi + 1) / (double)nB));

        switch (b->type) {

        case DB_READER_HEADER: {
            const int badgeH = 28;
            LItem* it = new_item(LIT_READER_BADGE);
            it->x = marginX; it->y = currentY;
            it->w = maxWidth; it->h = badgeH;
            it->host        = pluto_strdup(b->readerHost ? b->readerHost
                                                         : "WEB PAGE");
            it->readingTime = pluto_strdup(b->readingTime ? b->readingTime
                                                          : "");
            currentY += badgeH + 10;
            break;
        }

        case DB_HEADING: {
            currentY += (b->level == 1 ? 12 : 8) + b->spacingTop;
            int lh = 16, marginB = 4;
            PlutoFont* font = style_get_heading_font(b->level, &lh, &marginB);
            int indent = b->indent;

            BrkLines ls;
            brk_break(b->inlines, b->nInlines, maxWidth - indent,
                      font, 0, lh, &ls, NULL);
            int endY = emit_flow(&ls, lh, marginX + indent, maxWidth - indent,
                                 normalize_align(b->align), currentY,
                                 b->invert, 1 /*bold*/);
            brk_free(&ls);
            currentY = endY;

            if (b->level <= 2) {
                LItem* ln = new_item(LIT_LINE);
                ln->x1 = marginX;             ln->y1 = currentY + 3;
                ln->x2 = marginX + maxWidth;  ln->y2 = currentY + 3;
                currentY += 6;
            }
            currentY += marginB + b->spacingBottom;
            break;
        }

        case DB_MATH: {
            int startY = currentY + b->spacingTop;
            DocInline fake;
            memset(&fake, 0, sizeof(fake));
            fake.type  = DIT_TEXT;
            fake.italic = 1;
            fake.text  = b->codeText ? b->codeText : (char*)"";
            BrkLines ls;
            int lineH = 16;
            brk_break(&fake, 1, maxWidth - 10, NULL, 0, 0, &ls, &lineH);
            int endY = emit_flow(&ls, lineH, marginX + 5, maxWidth - 10,
                                 "center", startY, 0, 0);
            brk_free(&ls);
            currentY = endY + 10;
            break;
        }

        case DB_PARAGRAPH:
        case DB_BLOCKQUOTE: {
            int isQuote = (b->type == DB_BLOCKQUOTE);
            int indent  = b->indent;
            int blockStartX = ((isQuote ? marginX + 14 : marginX)) + indent;
            int blockMaxW   = maxWidth - (isQuote ? 18 : 0) - indent;
            int startQuoteY = currentY + b->spacingTop;

            int endY = flow_paragraph(b, blockStartX, blockMaxW, startQuoteY,
                                      b->invert, 0, 0);
            currentY = endY + 10 + b->spacingBottom;

            if (isQuote) {
                LItem* qb = new_item(LIT_QUOTE_BAR);
                qb->x  = marginX + 3;
                qb->y1 = startQuoteY;
                qb->y2 = currentY - 4;
            }
            break;
        }

        case DB_LIST_ITEM: {
            int depth = b->depth > 0 ? b->depth : 1;
            int isDt  = b->dtFlag;
            int isDd  = b->ddFlag;
            int indent       = (depth - 1) * 14;
            int bulletIndent = marginX + 4 + indent;
            int textIndent   = marginX + (isDd ? 30 : 20) + indent;
            int listMaxW     = maxWidth - (textIndent - marginX) - 4;
            const int lineH  = 18;

            if (!isDt && !isDd) {
                char* mk = layout_ordered_marker(
                    b->number >= 1 ? b->number : 1, b->markerType);
                char bullet[64];
                snprintf(bullet, sizeof(bullet), "%s.", mk);
                pluto_free(mk);
                LItem* bl = new_item(LIT_TEXT);
                bl->text = pluto_strdup(bullet);
                bl->font = style_get_body_font(1, 0, NULL);
                bl->x    = bulletIndent;
                bl->y    = currentY;
                bl->w    = 14;
                bl->h    = lineH;
                bl->bold = 1;
            }

            int endY = flow_paragraph(b, textIndent, listMaxW, currentY,
                                      0 /*invert*/, 0 /*forceBold*/,
                                      isDt /*bold font for whole item*/);
            currentY = endY + 6;
            break;
        }

        case DB_CODE_BLOCK: {
            PlutoFont* mono = style_get_mono_font();
            (void)mono;
            char** lines = b->lines;
            size_t nLines = b->nLines;
            char** owned = NULL;
            if (nLines == 0 && b->codeText) {
                /* replicate gmatch(rawText .. "\n", "(.-)\r?\n") */
                size_t cnt = 1;
                for (const char* p = b->codeText; *p; p++)
                    if (*p == '\n') cnt++;
                owned = (char**)pluto_malloc(sizeof(char*) * cnt);
                size_t k = 0;
                const char* seg = b->codeText;
                for (const char* p = b->codeText;; p++) {
                    if (*p == '\n') {
                        owned[k++] = pluto_strndup(seg, (size_t)(p - seg));
                        seg = p + 1;
                    } else if (*p == '\r' && p[1] == '\n') {
                        owned[k++] = pluto_strndup(seg, (size_t)(p - seg));
                        p++; seg = p + 1;
                    } else if (*p == '\0') {
                        owned[k++] = pluto_strdup(seg); /* implicit \n */
                        break;
                    }
                }
                lines = owned;
                nLines = k;
            } else if (nLines > 0) {
                owned = (char**)pluto_malloc(sizeof(char*) * nLines);
                for (size_t l = 0; l < nLines; l++)
                    owned[l] = pluto_strdup(lines[l] ? lines[l] : "");
                lines = owned;
            }
            int boxH = nLines * 14 + 12;
            if (boxH < 30) boxH = 30;
            LItem* it = new_item(LIT_CODE_BOX);
            it->x = marginX; it->y = currentY;
            it->w = maxWidth; it->h = boxH;
            it->lines  = lines;      /* takes ownership */
            it->nLines = nLines;
            currentY += boxH + 10;
            break;
        }

        case DB_HR:
            currentY += 6;
            {
                LItem* ln = new_item(LIT_LINE);
                ln->x1 = marginX + 20;
                ln->y1 = currentY;
                ln->x2 = marginX + maxWidth - 20;
                ln->y2 = currentY;
            }
            currentY += 10;
            break;

        case DB_IMAGE: {
            int imgW = b->width  > 0 ? b->width  : 160;
            if (imgW > maxWidth) imgW = maxWidth;
            int imgH = b->height > 0 ? b->height : 80;
            if (imgH > 160) imgH = 160;
            const char* align = normalize_align(b->align);
            int imgX = marginX;
            if (align && !strcmp(align, "center"))
                imgX = marginX + floordiv2(maxWidth - imgW);
            else if (align && !strcmp(align, "right"))
                imgX = marginX + (maxWidth - imgW > 0 ? maxWidth - imgW : 0);
            if (imgX < marginX) imgX = marginX;

            LItem* it = new_item(LIT_IMAGE);
            it->x = imgX; it->y = currentY;
            it->w = imgW; it->h = imgH;
            it->alt  = b->alt     ? pluto_strdup(b->alt)     : NULL;
            it->src  = b->src     ? pluto_strdup(b->src)     : NULL;
            it->href = b->imgHref ? pluto_strdup(b->imgHref) : NULL;
            it->img  = NULL;   /* bitmaps resolve via ImageDecoder */

            if (b->imgHref && !b->imgInert)
                lm_add_link_rect(b->imgHref,
                                 b->alt && b->alt[0] ? b->alt : "[Image Link]",
                                 (LmRect){ .x = imgX, .y = currentY,
                                           .w = imgW, .h = imgH,
                                           .isImage = 1,
                                           .src = it->src, .alt = it->alt },
                                 -1);

            if (b->usemap && b->usemap[0] && !b->blockInert) {
                const DocMap* map = find_map(doc, b->usemap);
                if (map) {
                    float sx = (b->width  > 0) ? (float)imgW / (float)b->width  : 1.0f;
                    float sy = (b->height > 0) ? (float)imgH / (float)b->height : 1.0f;
                    for (size_t r = 0; r < map->nRegions; r++) {
                        const DocAreaRegion* rg = &map->regions[r];
                        if (!rg->href) continue;
                        const char* shape =
                            (rg->shape && rg->shape[0]) ? rg->shape : "rect";
                        float cxv, cyv, cwv, chv;
                        int c0 = rg->nCoords > 0 ? rg->coords[0] : 0;
                        int c1 = rg->nCoords > 1 ? rg->coords[1] : 0;
                        int c2 = rg->nCoords > 2 ? rg->coords[2] : 0;
                        int c3 = rg->nCoords > 3 ? rg->coords[3] : c1;
                        if (!strcmp(shape, "circle")) {
                            float px = (float)c0, py = (float)c1,
                                  rad = (float)c2;
                            cxv = (float)imgX + (px - rad) * sx;
                            cyv = (float)currentY + (py - rad) * sy;
                            cwv = (rad * 2.0f) * sx;
                            chv = (rad * 2.0f) * sy;
                        } else if (!strcmp(shape, "poly")) {
                            float minX = 1e9f, minY = 1e9f;
                            float maxX = -1e9f, maxY = -1e9f;
                            size_t pairs = rg->nCoords / 2;
                            for (size_t pi = 0; pi < pairs; pi++) {
                                float px = (float)imgX +
                                    (float)rg->coords[pi * 2] * sx;
                                float py = (float)currentY +
                                    (float)rg->coords[pi * 2 + 1] * sy;
                                if (px < minX) minX = px;
                                if (px > maxX) maxX = px;
                                if (py < minY) minY = py;
                                if (py > maxY) maxY = py;
                            }
                            cxv = minX; cyv = minY;
                            cwv = maxX - minX; chv = maxY - minY;
                        } else {
                            float x1 = (float)c0, y1 = (float)c1;
                            float x2 = (rg->nCoords > 2) ? (float)c2 : x1;
                            float y2 = (rg->nCoords > 3) ? (float)c3 : y1;
                            cxv = (float)imgX + (x1 < x2 ? x1 : x2) * sx;
                            cyv = (float)currentY + (y1 < y2 ? y1 : y2) * sy;
                            cwv = fabsf(x2 - x1) * sx;
                            chv = fabsf(y2 - y1) * sy;
                        }
                        int cx = (int)cxv, cy = (int)cyv;
                        int cw = (int)cwv, ch = (int)chv;
                        if (cw < 1) cw = 1;
                        if (ch < 1) ch = 1;
                        lm_add_link_rect(rg->href,
                            (rg->alt && rg->alt[0]) ? rg->alt :
                                (b->alt && b->alt[0]) ? b->alt : "[Map Link]",
                            (LmRect){ .x = cx, .y = cy, .w = cw, .h = ch,
                                      .isImage = 1,
                                      .src = it->src,
                                      .alt = (rg->alt && rg->alt[0])
                                                 ? rg->alt : it->alt },
                            -1);
                    }
                }
            }
            currentY += imgH + 12;
            break;
        }

        case DB_TABLE: {
            size_t rowCount = b->nRows;
            int capH = (b->tableCaption && b->tableCaption[0]) ? 16 : 0;
            int tableH = rowCount * 18 + 14;
            if (tableH < 24) tableH = 24;
            tableH += capH;

            int tblW = maxWidth;
            if (b->tableWidth && b->tableWidth[0]) {
                char* end = NULL;
                double w = strtod(b->tableWidth, &end);
                if (end && *end == '\0') {
                    if (w > 0) {
                        int wi = (int)w;
                        tblW = wi < maxWidth ? wi : maxWidth;
                    }
                } else if (strchr(b->tableWidth, '%')) {
                    int pct = (int)w;
                    if (pct > 0 && pct <= 100)
                        tblW = (int)((double)maxWidth * pct / 100.0);
                }
            }
            int tblX = marginX;
            const char* align = normalize_align(b->align);
            if (align && !strcmp(align, "center"))
                tblX = marginX + floordiv2(maxWidth - tblW);
            else if (align && !strcmp(align, "right"))
                tblX = marginX + (maxWidth - tblW > 0 ? maxWidth - tblW : 0);

            LItem* it = new_item(LIT_TABLE_BOX);
            it->x = tblX; it->y = currentY;
            it->w = tblW; it->h = tableH;
            it->rows    = b->rows;              /* borrowed from doc */
            it->nRows   = b->nRows;
            it->caption = b->tableCaption
                              ? pluto_strdup(b->tableCaption) : NULL;
            it->border  = b->tableBorder ? 1 : 0;
            currentY += tableH + 10;
            break;
        }

        case DB_INPUT_FIELD: {
            /* hidden inputs arrive here (document.c maps type=hidden to
             * DB_INPUT_FIELD); Lua emits a separate "hidden_field" block
             * with no visual and no y advance -- same outcome. */
            if (b->inputType && !strcmp(b->inputType, "hidden")) {
                LItem* it = new_item(LIT_HIDDEN_FIELD);
                it->name       = pluto_strdup(b->inName ? b->inName : "");
                it->value      = pluto_strdup(b->inValue ? b->inValue : "");
                it->formAction = pluto_strdup(b->formAction
                                                  ? b->formAction : "");
                it->disabled   = b->disabledFlag ? 1 : 0;
                break;
            }
            int fieldH = 22;
            int fieldW = maxWidth - 10;
            int fw = b->fieldWidth;
            if (fw > 0) {
                int cand = fw * 8;
                if (cand < 60) cand = 60;
                fieldW = cand < maxWidth - 10 ? cand : maxWidth - 10;
            } else if (b->inputType &&
                       !strcmp(b->inputType, "textarea") && fw > 0) {
                int cand = fw * 8;
                if (cand < 80) cand = 80;
                fieldW = cand < maxWidth - 10 ? cand : maxWidth - 10;
            }
            int fieldRows =
                (b->inputType && !strcmp(b->inputType, "textarea"))
                    ? (b->fieldRows > 0 ? b->fieldRows : 2) : 1;
            if (fieldRows > 1) fieldH = 18 + fieldRows * 16;

            LItem* it = new_item(LIT_INPUT_FIELD);
            it->x = marginX; it->y = currentY;
            it->w = fieldW;  it->h = fieldH;
            it->inputType   = pluto_strdup(b->inputType ? b->inputType
                                                        : "text");
            it->name        = pluto_strdup(b->inName ? b->inName : "q");
            it->value       = pluto_strdup(b->inValue ? b->inValue : "");
            it->placeholder = pluto_strdup(b->placeholder ? b->placeholder
                                                          : "");
            it->formAction  = pluto_strdup(b->formAction ? b->formAction
                                                         : "");
            it->formMethod  = pluto_strdup(b->formMethod ? b->formMethod
                                                         : "get");
            it->disabled    = b->disabledFlag ? 1 : 0;
            it->readonly    = b->readonlyFlag ? 1 : 0;
            it->required    = b->requiredFlag ? 1 : 0;
            it->maxlength   = b->maxlength;

            if (!b->disabledFlag && !b->blockInert) {
                char label[96];
                snprintf(label, sizeof(label), "[Input: %s]",
                         it->name ? it->name : "q");
                lm_add_link_rect(it->formAction[0] ? it->formAction : "#",
                                 label,
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = fieldW, .h = fieldH,
                                           .isFormInput = 1,
                                           .inputBlock = it },
                                 -1);
            }
            currentY += fieldH + 8;
            break;
        }

        case DB_INPUT_SUBMIT: {
            const int btnH = 24;
            PlutoFont* btnFont = style_get_body_font(1, 0, NULL);
            const char* lbl = b->submitLabel ? b->submitLabel : "Submit";
            int tw = style_get_text_width(btnFont, lbl);
            int btnW = tw + 24;
            if (btnW < 50) btnW = 50;
            if (btnW > maxWidth) btnW = maxWidth;

            LItem* it = new_item(LIT_INPUT_SUBMIT);
            it->x = marginX; it->y = currentY;
            it->w = btnW;    it->h = btnH;
            it->label      = pluto_strdup(lbl);
            it->name       = b->inName  ? pluto_strdup(b->inName)  : NULL;
            it->value      = b->inValue ? pluto_strdup(b->inValue) : NULL;
            it->formAction = pluto_strdup(b->formAction ? b->formAction
                                                        : "");
            it->formMethod = pluto_strdup(b->formMethod ? b->formMethod
                                                        : "get");
            it->disabled   = b->disabledFlag ? 1 : 0;

            if (!b->disabledFlag && !b->blockInert) {
                char text[128];
                snprintf(text, sizeof(text), "[Button: %s]", lbl);
                lm_add_link_rect(it->formAction[0] ? it->formAction : "#",
                                 text,
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = btnW, .h = btnH,
                                           .isFormInput = 1,
                                           .inputBlock = it },
                                 -1);
            }
            currentY += btnH + 8;
            break;
        }

        case DB_CHECKBOX_FIELD: {
            const int boxH = 20;
            LItem* it = new_item(LIT_CHECKBOX_FIELD);
            it->x = marginX; it->y = currentY;
            it->w = boxH;    it->h = boxH;
            it->radio     = b->radioFlag ? 1 : 0;
            it->checked   = b->checkedFlag ? 1 : 0;
            it->name      = pluto_strdup(b->inName ? b->inName : "");
            it->value     = pluto_strdup(b->inValue ? b->inValue : "");
            it->label     = pluto_strdup(b->checkboxLabel
                                             ? b->checkboxLabel : "");
            it->formAction= pluto_strdup(b->formAction ? b->formAction : "");
            it->formMethod= pluto_strdup(b->formMethod ? b->formMethod
                                                       : "get");
            it->disabled  = b->disabledFlag ? 1 : 0;

            char label[96];
            const char* rawLbl = it->label ? it->label : "";
            snprintf(label, sizeof(label), "%s", rawLbl);
            size_t ll = strlen(label);
            if (ll > 30) {
                label[28] = '.'; label[29] = '.'; label[30] = '\0';
            }
            PlutoFont* font = style_get_body_font(0, 0, NULL);
            int lx = marginX + boxH + 6;
            int lw = style_get_text_width(font, label);
            if (label[0] != '\0') {
                LItem* tl = new_item(LIT_TEXT);
                tl->text = pluto_strdup(label);
                tl->font = font;
                tl->x    = lx;
                tl->y    = currentY + 2;
                tl->w    = lw;
                tl->h    = boxH;
                tl->bold = 0;
            }

            if (!b->disabledFlag && !b->blockInert) {
                char linkTxt[128];
                snprintf(linkTxt, sizeof(linkTxt), "input:%s",
                         it->name ? it->name : "q");
                lm_add_link_rect(linkTxt, it->label ? it->label : "",
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = boxH + 8 + lw, .h = boxH,
                                           .isFormInput = 1,
                                           .inputBlock = it },
                                 -1);
            }
            currentY += boxH + 8;
            break;
        }

        case DB_SELECT_FIELD: {
            const int fieldH = 22;
            LItem* it = new_item(LIT_SELECT_FIELD);
            it->x = marginX; it->y = currentY;
            it->w = maxWidth - 10; it->h = fieldH;
            it->name        = pluto_strdup(b->inName ? b->inName : "q");
            it->options      = b->options;          /* borrowed */
            it->nOptions     = b->nOptions;
            it->selectedIndex= b->selectedIndex > 0 ? b->selectedIndex : 1;
            it->formAction   = pluto_strdup(b->formAction ? b->formAction
                                                          : "");
            it->formMethod   = pluto_strdup(b->formMethod ? b->formMethod
                                                          : "get");
            it->disabled     = b->disabledFlag ? 1 : 0;

            if (!b->disabledFlag && !b->blockInert) {
                char linkTxt[128];
                snprintf(linkTxt, sizeof(linkTxt), "select:%s",
                         it->name ? it->name : "q");
                lm_add_link_rect(linkTxt, "[Select]",
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = maxWidth - 10, .h = fieldH,
                                           .isFormInput = 1,
                                           .inputBlock = it },
                                 -1);
            }
            currentY += fieldH + 8;
            break;
        }

        case DB_PLACEHOLDER: {
            int boxW = b->width  > 0 ? b->width  : maxWidth;
            if (boxW > maxWidth) boxW = maxWidth;
            int boxH = b->height > 0 ? b->height : 46;
            if (boxH > 120) boxH = 120;
            LItem* it = new_item(LIT_PLACEHOLDER);
            it->x = marginX; it->y = currentY;
            it->w = boxW;    it->h = boxH;
            const char* lbl =
                (b->boxLabel && b->boxLabel[0]) ? b->boxLabel :
                (b->phTag   && b->phTag[0])     ? b->phTag   : "Media";
            it->label = pluto_strdup(lbl);

            if (b->phHref && !b->blockInert)
                lm_add_link_rect(b->phHref,
                                 (it->label && strcmp(it->label, "Media"))
                                     ? it->label : "Embed",
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = boxW, .h = boxH },
                                 -1);
            currentY += boxH + 10;
            break;
        }

        case DB_METER: {
            const int boxH = 20;
            LItem* it = new_item(LIT_METER);
            it->x = marginX; it->y = currentY;
            it->w = maxWidth; it->h = boxH;
            it->mValue   = b->mValue;
            it->mMin     = b->mMin;
            it->mMax     = b->mMax;
            it->mLow     = b->mLow;
            it->mHigh    = b->mHigh;
            it->mOptimum = b->mOptimum;
            it->label    = pluto_strdup("");
            currentY += boxH + 10;
            break;
        }

        case DB_BOX_OPEN: {
            LItem* fr = new_item(LIT_BOX_FRAME);
            fr->x  = marginX;  fr->y  = currentY;
            fr->w  = maxWidth; fr->y2 = currentY;
            fr->label     = pluto_strdup(b->boxLabel ? b->boxLabel : "");
            fr->toggleKey = b->toggleKey ? pluto_strdup(b->toggleKey) : NULL;
            fr->toggleOpen= b->toggleOpen ? 1 : 0;
            if (sp < BOX_STACK_MAX) boxStack[sp++] = fr;
            if (fr->toggleKey) {
                char key[64];
                snprintf(key, sizeof(key), "toggle:%s", fr->toggleKey);
                lm_add_link_rect(key, fr->label ? fr->label : "",
                                 (LmRect){ .x = marginX, .y = currentY,
                                           .w = maxWidth, .h = 18,
                                           .isToggle = 1,
                                           .toggleOpen = fr->toggleOpen,
                                           .toggleKey = fr->toggleKey },
                                 -1);
            }
            currentY += 18;
            break;
        }

        case DB_BOX_CLOSE: {
            if (sp > 0) boxStack[--sp]->y2 = currentY;
            currentY += 8;
            break;
        }

        default:
            break;
        }
    }

    double th = (double)currentY + 20.0;
    s_totalHeight = th > PLUTO_CONTENT_HEIGHT ? th : (double)PLUTO_CONTENT_HEIGHT;
}

/* ── paint helpers ─────────────────────────────────────────────────────── */

static void dt(const char* text, int x, int y) {
    s_pd->graphics->drawText(text, (size_t)strlen(text), kASCIIEncoding,
                             x, y);
}

static void dtf(PlutoFont* f, const char* text, int x, int y) {
    s_pd->graphics->setFont(f);
    dt(text, x, y);
}

/* growable scratch so composed cell texts can be built without mallocs */
static char* s_scratch = NULL;
static size_t s_scratchCap = 0;

static char* scratch_reserve(size_t n) {
    if (n + 1 > s_scratchCap) {
        s_scratchCap = n + 128;
        s_scratch = (char*)pluto_realloc(s_scratch, s_scratchCap);
    }
    return s_scratch;
}

/* collapse whitespace runs to single spaces, trim ends (Lua gsub pair) */
static const char* squash_text(const char* s) {
    if (!s) return "";
    size_t n = strlen(s);
    char* out = scratch_reserve(n);
    size_t k = 0;
    int inWs = 0;
    for (const char* p = s; *p; p++) {
        if (is_space(*p)) { inWs = 1; continue; }
        if (inWs && k > 0) out[k++] = ' ';
        inWs = 0;
        out[k++] = *p;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
    return out;
}

static void draw_bitmap_fit(struct LCDBitmap* bmp, int x, int drawY,
                            int w, int h) {
    int iw = 0, ih = 0, rb = 0;
    uint8_t* mask = NULL;
    uint8_t* dat = NULL;
    s_pd->graphics->getBitmapData(bmp, &iw, &ih, &rb, &mask, &dat);
    if (iw > 0 && ih > 0) {
        float scx = (float)w / (float)iw;
        float scy = (float)h / (float)ih;
        float scale = scx < scy ? scx : scy;
        int dw = (int)floorf((float)iw * scale);
        int dh = (int)floorf((float)ih * scale);
        int dx = x + floordiv2(w - dw);
        int dy = drawY + floordiv2(h - dh);
        s_pd->graphics->drawScaledBitmap(bmp, dx, dy, scale, scale);
    } else {
        s_pd->graphics->drawBitmap(bmp, x, drawY, kBitmapUnflipped);
    }
}

static void hatch_box(const LItem* it, int drawY) {
    s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4, kColorWhite);
    s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4, 1,
                                  kColorBlack);
    for (int hx = it->x + 4; hx <= it->x + it->w - 4; hx += 10)
        s_pd->graphics->drawLine(hx, drawY + 3, hx, drawY + it->h - 3,
                                 1, kColorBlack);
}

static void centered_label(const LItem* it, int drawY, const char* lbl) {
    PlutoFont* f = style_get_small_font();
    int lw = style_get_text_width(f, lbl);
    dtf(f, lbl, it->x + floordiv2(it->w - lw),
        drawY + floordiv2(it->h - 10));
}

/* ── table_box / image painters ────────────────────────────────────────── */

static void table_box_paint(const LItem* it, int scrollY) {
    int drawY = it->y - scrollY;
    if (!(drawY + it->h >= PLUTO_CONTENT_Y && drawY <= PLUTO_SCREEN_HEIGHT))
        return;

    if (it->border) {   /* Lua: border ~= false */
        s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 3,
                                      kColorWhite);
        s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 3, 1,
                                      kColorBlack);
    }

    int colCount = 1;
    for (size_t r = 0; r < it->nRows; r++) {
        const DocTableRow* row = &it->rows[r];
        int n = 0;
        for (size_t c = 0; c < row->nCells; c++)
            n += row->cells[c].colspan >= 1 ? row->cells[c].colspan : 1;
        if (n > colCount) colCount = n;
    }
    double colW = ((double)(it->w - 16)) /
                  (double)(colCount > 1 ? colCount : 1);

    int capH = 0;
    if (it->caption && it->caption[0]) capH = 16;
    if (capH > 0) {
        PlutoFont* f = style_get_body_font(1, 0, NULL);
        char cap[96];
        snprintf(cap, sizeof(cap), "%s", it->caption);
        size_t cl = strlen(cap);
        if (cl > 30) { cap[28]='.'; cap[29]='.'; cap[30]='\0'; }
        dtf(f, cap, it->x + 8, drawY + 2);
    }

    int rowY = drawY + 4 + capH;
    for (size_t r = 0; r < it->nRows; r++) {
        const DocTableRow* row = &it->rows[r];
        int cellX = it->x + 8;
        for (size_t c = 0; c < row->nCells; c++) {
            const DocTableCell* cell = &row->cells[c];
            int span = cell->colspan >= 1 ? cell->colspan : 1;
            double cw = colW * (double)span;

            /* concat all inline texts, collapse ws runs, trim */
            size_t need = 1;
            for (size_t i = 0; i < cell->nInlines; i++)
                need += cell->inlines[i].text ? strlen(cell->inlines[i].text)
                                              : 0;
            char* raw = scratch_reserve(need);
            size_t k = 0;
            for (size_t i = 0; i < cell->nInlines; i++) {
                const char* t = cell->inlines[i].text;
                if (t) { size_t tl = strlen(t); memcpy(raw + k, t, tl); k += tl; }
            }
            raw[k] = '\0';
            const char* txt = squash_text(raw);

            PlutoFont* font = cell->isHeader
                ? style_get_body_font(1, 0, NULL) : style_get_small_font();
            s_pd->graphics->setFont(font);
            int maxChars = (int)floor(cw / 7.0);
            if (maxChars < 2) maxChars = 2;
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", txt);
            size_t tlen = strlen(buf);
            if ((int)tlen > maxChars) {
                /* Lua: sub(txt, 1, math.max(1, maxChars - 2)) .. ".." */
                int keep = maxChars - 2;
                if (keep < 1) keep = 1;
                if (keep > 125) keep = 125;
                buf[keep] = '.'; buf[keep+1] = '.'; buf[keep+2] = '\0';
            }
            int tw = style_get_text_width(font, buf);
            const char* cellAlign =
                cell->align ? cell->align : "left";
            int tx = cellX;
            if (!strcmp(cellAlign, "center"))
                tx = cellX + floordiv2((int)cw - tw);
            else if (!strcmp(cellAlign, "right"))
                tx = cellX + ((int)cw - tw > 0 ? (int)cw - tw : 0);
            dt(buf, tx, rowY);

            /* collect link: LAST href wins, texts concatenated */
            const char* linkHref = NULL;
            char linkText[160];
            size_t ltLen = 0;
            for (size_t i = 0; i < cell->nInlines; i++) {
                if (cell->inlines[i].href) {
                    linkHref = cell->inlines[i].href;
                    const char* t = cell->inlines[i].text
                                        ? cell->inlines[i].text : "";
                    size_t tl2 = strlen(t);
                    if (ltLen + tl2 < sizeof(linkText)) {
                        memcpy(linkText + ltLen, t, tl2);
                        ltLen += tl2;
                    }
                }
            }
            linkText[ltLen] = '\0';
            /* Lua reads `inl.inert` after its inline loop has ended -- a
             * nil global lookup -- so inert is ALWAYS false here. */
            if (linkHref && !s_cellLinksRegistered)
                lm_add_link_rect(linkHref,
                                 linkText[0] ? linkText : linkHref,
                                 (LmRect){ .x = tx, .y = rowY + scrollY,
                                           .w = tw, .h = 16 },
                                 -1);

            cellX += (int)cw;
        }
        rowY += 18;
        if (rowY < drawY + it->h - 4) {
            s_pd->graphics->drawLine(it->x, rowY - 2, it->x + it->w, rowY - 2,
                                     1, kColorBlack);
        }
    }
    s_cellLinksRegistered = 1;
}

static void image_paint(const LItem* it, int scrollY) {
    int drawY = it->y - scrollY;
    if (!(drawY + it->h >= PLUTO_CONTENT_Y && drawY <= PLUTO_SCREEN_HEIGHT))
        return;

    int imgMode = storage_settings()->imageMode;

    if (imgMode == PLUTO_IMAGE_MODE_DISABLED) {
        s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4,
                                      kColorWhite);
        s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4, 1,
                                      kColorBlack);
        centered_label(it, drawY, "[Image Off]");
        return;
    }

    if (imgMode == PLUTO_IMAGE_MODE_ONDEMAND) {
        int isDecoded = it->src && id_is_decoded(it->src);
        int wasRequested = it->src && layout_on_demand_requested(it->src);

        if (isDecoded) {
            struct LCDBitmap* cached =
                (struct LCDBitmap*)id_get_image(it->src);
            if (cached)
                draw_bitmap_fit(cached, it->x, drawY, it->w, it->h);
        } else if (wasRequested) {
            hatch_box(it, drawY);
            centered_label(it, drawY, "Loading...");
        } else {
            hatch_box(it, drawY);
            int iconX = it->x + floordiv2(it->w) - 8;
            int iconY = drawY + floordiv2(it->h) - 10;
            s_pd->graphics->drawRoundRect(iconX, iconY, 16, 11, 2, 1,
                                          kColorBlack);
            s_pd->graphics->fillEllipse(iconX + 8 - 3, iconY + 5 - 3, 6, 6,
                                        0.0f, 360.0f, kColorBlack);
            s_pd->graphics->fillRect(iconX + 13, iconY + 1, 1, 1,
                                     kColorBlack);
            PlutoFont* f = style_get_small_font();
            const char* lbl = it->href ? "Tap: View/Open" : "Tap: View Image";
            int lw = style_get_text_width(f, lbl);
            if (iconY + 18 <= drawY + it->h - 2)
                dtf(f, lbl, it->x + floordiv2(it->w - lw), iconY + 16);
        }
        return;
    }

    if (imgMode == PLUTO_IMAGE_MODE_HOVER) {
        int isHovered = s_hoveredImageSrc && it->src &&
                        !strcmp(s_hoveredImageSrc, it->src);
        if (!isHovered) {
            s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4,
                                          kColorWhite);
            s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4, 1,
                                          kColorBlack);
            centered_label(it, drawY, "[Hover]");
            return;
        }
        if (it->src) id_enqueue(it->src);
    } else if (imgMode == PLUTO_IMAGE_MODE_VIEWPORT) {
        if (it->src) id_enqueue(it->src);
    }

    /* HOVER(active) / VIEWPORT / ALL share this draw chain */
    id_draw(it->x, drawY, it->w, it->h,
            it->alt, it->href, 0, it->src);
}

/* forward decl used inside layout_draw's switch */
static void table_box_paint(const LItem* it, int scrollY);
static void image_paint(const LItem* it, int scrollY);

/* ── Layout.draw ───────────────────────────────────────────────────────── */

void layout_draw(int scrollY) {
    if (!s_pd) return;
    s_pd->graphics->fillRect(0, PLUTO_CONTENT_Y, PLUTO_SCREEN_WIDTH,
                             PLUTO_CONTENT_HEIGHT, kColorWhite);

    s_pd->graphics->pushContext(NULL);
    s_pd->graphics->setClipRect(0, PLUTO_CONTENT_Y, PLUTO_CONTENT_WIDTH,
                                PLUTO_CONTENT_HEIGHT);

    for (int i = 0; i < s_nItems; i++) {
        const LItem* it = &s_items[i];

        switch (it->type) {

        case LIT_TEXT: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                int isSelLink =
                    it->href && lm_is_highlighted(it->href, it->x, it->y);
                int inverted = isSelLink || it->mark || it->invert;
                if (inverted) {
                    s_pd->graphics->fillRect(it->x - 1, drawY - 1,
                                             it->w + 2, it->h, kColorBlack);
                    s_pd->graphics->setDrawMode(kDrawModeFillWhite);
                }
                s_pd->graphics->setFont(it->font);
                dt(it->text, it->x, drawY);
                if (inverted)
                    s_pd->graphics->setDrawMode(kDrawModeCopy);

                if (it->strike) {
                    s_pd->graphics->drawLine(it->x,
                                             drawY + floordiv2(it->h),
                                             it->x + it->w,
                                             drawY + floordiv2(it->h),
                                             1,
                                             inverted ? kColorWhite
                                                      : kColorBlack);
                }
                if (it->underline || it->href) {
                    s_pd->graphics->drawLine(it->x, drawY + it->h - 1,
                                             it->x + it->w,
                                             drawY + it->h - 1,
                                             1, kColorBlack);
                }
            }
            break;
        }

        case LIT_READER_BADGE: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4,
                                              kColorBlack);
                s_pd->graphics->setDrawMode(kDrawModeFillWhite);

                char head[192];
                snprintf(head, sizeof(head), "READER MODE  *  %s",
                         it->host ? it->host : "");
                dtf(style_get_body_font(1, 0, NULL), head, it->x + 8,
                    drawY + 3);
                dtf(style_get_small_font(),
                    it->readingTime ? it->readingTime : "", it->x + 8,
                    drawY + 16);

                s_pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }

        case LIT_LINE: {
            int drawY1 = it->y1 - scrollY;
            int drawY2 = it->y2 - scrollY;
            /* Lua sets no explicit color here; it inherits whatever the
             * previous painter left -- replicated literally */
            if (drawY1 >= PLUTO_CONTENT_Y - 2 &&
                drawY1 <= PLUTO_SCREEN_HEIGHT + 2)
                s_pd->graphics->drawLine(it->x1, drawY1, it->x2, drawY2,
                                         1, kColorBlack);
            break;
        }

        case LIT_QUOTE_BAR: {
            int drawY1 = it->y1 - scrollY;
            int drawY2 = it->y2 - scrollY;
            if (drawY2 >= PLUTO_CONTENT_Y && drawY1 <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->fillRect(it->x, drawY1, 3, drawY2 - drawY1,
                                         kColorBlack);
            }
            break;
        }

        case LIT_CODE_BOX: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 3,
                                              kColorWhite);
                s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 3,
                                              1, kColorBlack);
                PlutoFont* mono = style_get_mono_font();
                s_pd->graphics->setFont(mono);
                int lineY = drawY + 6;
                for (size_t l = 0; l < it->nLines; l++) {
                    if (lineY + 14 > drawY + it->h) break;
                    char* exp = layout_expand_tab_columns(
                        it->lines[l] ? it->lines[l] : "");
                    dt(exp, it->x + 8, lineY);
                    pluto_free(exp);
                    lineY += 14;
                }
            }
            break;
        }

        case LIT_TABLE_BOX:
            table_box_paint(it, scrollY);
            break;

        case LIT_IMAGE:
            image_paint(it, scrollY);
            break;

        case LIT_INPUT_FIELD: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                int isSel = (s_selectedInput == it);
                if (isSel) {
                    s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h,
                                                  4, kColorBlack);
                    s_pd->graphics->setDrawMode(kDrawModeFillWhite);
                } else if (it->disabled) {
                    s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h,
                                                  4, 1, kColorBlack);
                    s_pd->graphics->drawLine(it->x + 3, drawY + 1,
                                             it->x + it->w - 3, drawY + 1,
                                             1, kColorBlack);
                } else {
                    s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h,
                                                  4, 1, kColorBlack);
                }

                char val[128];
                if (it->value && it->value[0])
                    snprintf(val, sizeof(val), "%s", it->value);
                else
                    snprintf(val, sizeof(val), "%s",
                             it->placeholder ? it->placeholder : "");
                if (it->inputType && !strcmp(it->inputType, "password")) {
                    size_t vl = it->value ? strlen(it->value) : 0;
                    if (vl > 100) vl = 100;
                    memset(val, '*', vl);
                    val[vl] = '\0';
                }
                size_t vlen = strlen(val);
                if (vlen > 36) {
                    val[33] = '.'; val[34] = '.'; val[35] = '.';
                    val[36] = '\0';
                }
                dtf(style_get_body_font(0, 0, NULL), val, it->x + 6,
                    drawY + 3);
                if (it->required) dt("*", it->x + it->w - 10, drawY + 3);
                s_pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }

        case LIT_INPUT_SUBMIT: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                int isSel = (s_selectedInput == it);
                s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4,
                                              kColorBlack);
                s_pd->graphics->setDrawMode(kDrawModeFillWhite);

                const char* lbl = it->label ? it->label : "Submit";
                PlutoFont* bf = style_get_body_font(1, 0, NULL);
                int lw = style_get_text_width(bf, lbl);
                dtf(bf, lbl, it->x + floordiv2(it->w - lw), drawY + 3);

                if (isSel) {
                    s_pd->graphics->drawRoundRect(it->x + 1, drawY + 1,
                                                  it->w - 2, it->h - 2, 3,
                                                  1, kColorWhite);
                } else if (it->disabled) {
                    s_pd->graphics->drawLine(it->x + 4, drawY + 4,
                                             it->x + it->w - 4, drawY + 4,
                                             1, kColorBlack);
                }
                s_pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }

        case LIT_CHECKBOX_FIELD: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                int isSel = (s_selectedInput == it);
                if (it->radio) {
                    int cx = it->x + it->h / 2;
                    int cy = drawY + it->h / 2;
                    int r  = it->h / 2 - 1;
                    s_pd->graphics->drawEllipse(cx - r, cy - r, r * 2, r * 2,
                                                1, 0.0f, 360.0f,
                                                kColorBlack);
                    if (it->checked) {
                        int ir = it->h / 2 - 3;
                        s_pd->graphics->fillEllipse(cx - ir, cy - ir,
                                                    ir * 2, ir * 2,
                                                    0.0f, 360.0f,
                                                    kColorBlack);
                    }
                } else {
                    s_pd->graphics->drawRoundRect(it->x, drawY, it->h, it->h,
                                                  3, 1, kColorBlack);
                    if (it->checked) {
                        s_pd->graphics->drawLine(it->x + 4,
                                                 drawY + it->h / 2,
                                                 it->x + it->h / 2,
                                                 drawY + it->h - 4,
                                                 1, kColorBlack);
                        s_pd->graphics->drawLine(it->x + it->h / 2,
                                                 drawY + it->h - 4,
                                                 it->x + it->h - 4,
                                                 drawY + 3,
                                                 1, kColorBlack);
                    }
                }
                if (isSel) {
                    s_pd->graphics->drawRoundRect(it->x - 2, drawY - 2,
                                                  it->h + 4, it->h + 4, 4,
                                                  1, kColorBlack);
                } else if (it->disabled) {
                    s_pd->graphics->drawLine(it->x - 1, drawY - 1,
                                             it->x + it->h + 1,
                                             drawY + it->h + 1,
                                             1, kColorBlack);
                }
            }
            break;
        }

        case LIT_SELECT_FIELD: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                int isSel = (s_selectedInput == it);
                s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4,
                                              1, kColorBlack);

                char val[96];
                val[0] = '\0';
                if (it->options && it->selectedIndex >= 1 &&
                    (size_t)it->selectedIndex <= it->nOptions) {
                    const DocSelectOpt* opt =
                        &((const DocSelectOpt*)it->options)
                            [it->selectedIndex - 1];
                    snprintf(val, sizeof(val), "%s",
                             opt->text ? opt->text : "");
                }
                size_t vlen = strlen(val);
                if (vlen > 30) { val[28]='.'; val[29]='.'; val[30]='\0'; }
                dtf(style_get_body_font(0, 0, NULL), val, it->x + 6,
                    drawY + 3);

                int ax = it->x + it->w - 14;
                int ay = drawY + it->h / 2;
                s_pd->graphics->drawLine(ax, ay - 3, ax + 5, ay - 3,
                                         1, kColorBlack);
                s_pd->graphics->drawLine(ax + 1, ay - 1, ax + 4, ay - 1,
                                         1, kColorBlack);
                s_pd->graphics->drawLine(ax + 2, ay + 1, ax + 3, ay + 1,
                                         1, kColorBlack);

                if (isSel) {
                    s_pd->graphics->drawRoundRect(it->x - 1, drawY - 1,
                                                  it->w + 2, it->h + 2, 4,
                                                  1, kColorBlack);
                } else if (it->disabled) {
                    s_pd->graphics->drawLine(it->x + 4, drawY + 4,
                                             it->x + it->w - 4, drawY + 4,
                                             1, kColorBlack);
                }
            }
            break;
        }

        case LIT_PLACEHOLDER: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 4,
                                              kColorWhite);
                s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4,
                                              1, kColorBlack);

                char lbl[96];
                snprintf(lbl, sizeof(lbl), "%s",
                         it->label ? it->label : "Media");
                size_t ll = strlen(lbl);
                if (ll > 40) { lbl[38]='.'; lbl[39]='.'; lbl[40]='\0'; }
                PlutoFont* f = style_get_small_font();
                int lw = style_get_text_width(f, lbl);
                dtf(f, lbl, it->x + floordiv2(it->w - lw),
                    drawY + floordiv2(it->h - 10));
            }
            break;
        }

        case LIT_METER: {
            int drawY = it->y - scrollY;
            if (drawY + it->h >= PLUTO_CONTENT_Y &&
                drawY <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 4,
                                              1, kColorBlack);
                double min = it->mMin;
                double max = it->mMax;
                if (max <= min) max = min + 1;
                double frac = ((it->mValue) - min) / (max - min);
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                if (frac > 0) {
                    s_pd->graphics->fillRoundRect(
                        it->x + 2, drawY + 2,
                        (int)floor((it->w - 4) * frac), it->h - 4, 3,
                        kColorBlack);
                }
                if (it->mOptimum != 0.0) {   /* Lua: nil optimum -> skip */
                    double opt = (it->mOptimum - min) / (max - min);
                    if (opt >= 0 && opt <= 1) {
                        int ox = it->x + 2 + (int)floor((it->w - 4) * opt);
                        s_pd->graphics->fillRect(ox - 1, drawY + 1, 2,
                                                 it->h - 2, kColorWhite);
                        s_pd->graphics->drawLine(ox, drawY + 1, ox,
                                                 drawY + it->h - 2,
                                                 1, kColorBlack);
                    }
                }
                char pct[24];
                snprintf(pct, sizeof(pct), "%d%%",
                         (int)floor(frac * 100));
                PlutoFont* f = style_get_small_font();
                int tw = style_get_text_width(f, pct);
                int tx = it->x + floordiv2(it->w - tw);
                dtf(f, pct, tx, drawY + floordiv2(it->h - 10));
            }
            break;
        }

        case LIT_BOX_FRAME: {
            int y  = it->y - scrollY;
            int y2 = it->y2 - scrollY;
            if (y2 >= PLUTO_CONTENT_Y && y <= PLUTO_SCREEN_HEIGHT) {
                s_pd->graphics->drawLine(it->x, y, it->x + it->w, y,
                                         1, kColorBlack);
                s_pd->graphics->drawLine(it->x, y2, it->x + it->w, y2,
                                         1, kColorBlack);
                s_pd->graphics->drawLine(it->x, y, it->x, y2,
                                         1, kColorBlack);
                s_pd->graphics->drawLine(it->x + it->w, y, it->x + it->w, y2,
                                         1, kColorBlack);

                if (it->label && it->label[0]) {
                    PlutoFont* f = style_get_small_font();
                    char lbl[160];
                    if (it->toggleKey)
                        snprintf(lbl, sizeof(lbl), "%s %s",
                                 it->toggleOpen ? "[-]" : "[+]", it->label);
                    else
                        snprintf(lbl, sizeof(lbl), "%s", it->label);
                    int lw = style_get_text_width(f, lbl);
                    int maxLw = it->w - 14;
                    if (lw > maxLw) {
                        int keep = maxLw / 6;   /* math.floor(maxLw / 6) */
                        if (keep < 1) keep = 1;
                        if (keep > 140) keep = 140;
                        lbl[keep] = '.'; lbl[keep+1] = '.'; lbl[keep+2]='\0';
                        lw = style_get_text_width(f, lbl);
                    }
                    s_pd->graphics->fillRect(it->x + 6, y - 4, lw + 4, 9,
                                             kColorWhite);
                    dtf(f, lbl, it->x + 8, y - 5);
                }
            }
            break;
        }

        default:
            break;
        }
    }

    lm_draw_selected_highlight(scrollY);

    s_pd->graphics->popContext();
    s_pd->graphics->clearClipRect();

    double maxScrollD = s_totalHeight - (double)PLUTO_CONTENT_HEIGHT;
    if (maxScrollD > 0) {
        int barH = (int)((double)PLUTO_CONTENT_HEIGHT *
                         ((double)PLUTO_CONTENT_HEIGHT / s_totalHeight));
        if (barH < 16) barH = 16;
        int barY = PLUTO_CONTENT_Y +
                   (int)(((double)scrollY / maxScrollD) *
                         (double)(PLUTO_CONTENT_HEIGHT - barH));
        s_pd->graphics->fillRect(PLUTO_SCREEN_WIDTH - PLUTO_SCROLLBAR_WIDTH,
                                 barY, PLUTO_SCROLLBAR_WIDTH, barH,
                                 kColorBlack);
    }

    if (layout_has_on_demand_overlay()) layout_draw_on_demand_overlay();
}

/* ── evictions ─────────────────────────────────────────────────────────── */

void layout_evict_offscreen(int scrollY) {
    int imgMode = storage_settings()->imageMode;
    if (!s_pd || imgMode != PLUTO_IMAGE_MODE_VIEWPORT) return;

    int viewTop = scrollY - 200;
    int viewBottom = scrollY + PLUTO_CONTENT_HEIGHT + 200;

    for (int i = 0; i < s_nItems; i++) {
        const LItem* it = &s_items[i];
        if (it->type == LIT_IMAGE && it->src) {
            if (it->y + it->h < viewTop || it->y > viewBottom)
                id_evict(it->src);
        }
    }
}

void layout_evict_hovered_image(const char* currentHoveredSrc) {
    int imgMode = storage_settings()->imageMode;
    if (imgMode != PLUTO_IMAGE_MODE_HOVER) return;

    if (s_hoveredImageSrc && !currentHoveredSrc) {
        id_evict(s_hoveredImageSrc);
        pluto_free(s_hoveredImageSrc);
        s_hoveredImageSrc = NULL;
    } else if (s_hoveredImageSrc && currentHoveredSrc &&
               strcmp(s_hoveredImageSrc, currentHoveredSrc) != 0) {
        id_evict(s_hoveredImageSrc);
        pluto_free(s_hoveredImageSrc);
        s_hoveredImageSrc = NULL;
    }
}

/* ── on-demand choice overlay ──────────────────────────────────────────── */

void layout_draw_on_demand_overlay(void) {
    if (!s_pd) return;
    if (!s_odSrc && !s_odHref && !s_odAlt) return;
    const char* ovSrc = s_odSrc;
    const char* ovHref = s_odHref;
    const char* title = s_odAlt ? s_odAlt : "Image";
    int isLoaded = ovSrc && id_is_decoded(ovSrc);

    const int boxW = 240;
    const int boxH = 80;
    int boxX = floordiv2(PLUTO_SCREEN_WIDTH - boxW);
    int boxY = floordiv2(PLUTO_CONTENT_Y + PLUTO_CONTENT_HEIGHT - boxH);
    s_pd->graphics->fillRoundRect(boxX, boxY, boxW, boxH, 8, kColorWhite);
    s_pd->graphics->drawRoundRect(boxX, boxY, boxW, boxH, 8, 1, kColorBlack);

    char ttl[96];
    snprintf(ttl, sizeof(ttl), "%s", title);
    size_t tl = strlen(ttl);
    if (tl > 28) { ttl[25]='.'; ttl[26]='.'; ttl[27]='.'; ttl[28]='\0'; }

    dtf(style_get_body_font(1, 0, NULL), ttl, boxX + 12, boxY + 8);
    s_pd->graphics->drawLine(boxX + 10, boxY + 20, boxX + boxW - 10,
                             boxY + 20, 1, kColorBlack);

    PlutoFont* fs = style_get_small_font();
    if (isLoaded)
        dtf(fs, "(A) Unload Image", boxX + 14, boxY + 30);
    else
        dtf(fs, "(A) View Image", boxX + 14, boxY + 30);
    if (ovHref)
        dtf(fs, "(B) Open Link", boxX + 14, boxY + 48);
    else
        dtf(fs, "(B) Cancel", boxX + 14, boxY + 48);
}

