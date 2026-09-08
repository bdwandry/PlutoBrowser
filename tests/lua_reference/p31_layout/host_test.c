/*
 * P31 host battery: exercises the C layout helpers/breakLines/emitFlow with
 * the SAME deterministic metrics as the Lua harness (width = len*10,
 * " " = 4, font names by role) and prints identical formats for diffing
 * against /tmp/p31/lua_{helpers,break,emit}.txt.
 *
 * Fonts: the harness runs without the Playdate runtime, so style_init is
 * unavailable; instead the measure stub returns len*10 for every font and
 * font identity is compared by pointer (all roles NULL → uniform "BODY" —
 * matching the Lua stub only where the Lua harness also used BODY; the
 * break/emit vectors that exercise font selection inject real role fonts
 * via the style_font table, which IS initialized here from static storage —
 * see fake_pd below).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"
#include "render/layout.h"
#include "render/style.h"
#include "render/link_manager.h"
#include "html/document.h"

static int stub_measure(LCDFont *font, const char *text)
{
    (void)font;
    if (!text || !text[0]) return 0;
    return (int)strlen(text) * 10;
}

static const char *font_name(LCDFont *f)
{
    if (f == style_font(PLUTO_FONT_HEADING1)) return "H1";
    if (f == style_font(PLUTO_FONT_HEADING2)) return "H2";
    if (f == style_font(PLUTO_FONT_HEADING3)) return "H3";
    if (f == style_font(PLUTO_FONT_BODY)) return "BODY";
    if (f == style_font(PLUTO_FONT_BODY_BOLD)) return "BOLD";
    if (f == style_font(PLUTO_FONT_MONO)) return "MONO";
    if (f == style_font(PLUTO_FONT_SMALL)) return "SMALL";
    return "BODY";
}

static void emit_stdout(const char *s) { printf("%s\n", s); }

/* link rect capture (mirrors the Lua LinkManager stub) */
static char lastLinks[64][1024];
static int lastLinkCount = 0;

/* Expose the private style font table for host testing. The real style_init
 * needs pd->graphics->loadFont; here we install sentinel pointers so font
 * identity checks (font_name / style_get_inline_font) behave like the Lua
 * stub's distinct role names. */
extern void style_test_set_fonts(LCDFont *body, LCDFont *bold, LCDFont *mono,
                                 LCDFont *small);
static int s_body, s_bold, s_mono, s_small, s_h1, s_h2, s_h3;

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "helpers";
    layout_set_measure(stub_measure);
    layout_init(NULL); /* host: no pd — measure stub covers everything */
    style_test_set_fonts_ex((LCDFont *)&s_body, (LCDFont *)&s_bold,
                            (LCDFont *)&s_mono, (LCDFont *)&s_small,
                            (LCDFont *)&s_h1, (LCDFont *)&s_h2, (LCDFont *)&s_h3);

    if (strcmp(which, "helpers") == 0)
    {
        int rs[] = {1,2,3,4,5,9,10,14,38,40,49,50,51,90,99,100,444,999,1000,1994,3999,4000,0,-5};
        for (size_t i = 0; i < sizeof(rs)/sizeof(rs[0]); i++)
        {
            char buf[48];
            layout_to_roman(rs[i], buf, sizeof(buf));
            printf("roman %d = %s\n", rs[i], buf);
        }
        int as[] = {1, 2, 25, 26, 27, 52, 53, 702, 703, 0, -1};
        for (size_t i = 0; i < sizeof(as)/sizeof(as[0]); i++)
        {
            char buf[48];
            layout_to_alpha(as[i], buf, sizeof(buf));
            printf("alpha %d = %s\n", as[i], buf);
        }
        /* Lua 5.5 ipairs stops at nil: only "1".."I" vectors are printed. */
        const char *types[] = {"1", "a", "A", "i", "I"};
        int ns[] = {1, 5, 27, 444};
        for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++)
            for (size_t j = 0; j < sizeof(ns)/sizeof(ns[0]); j++)
            {
                char buf[48];
                layout_ordered_marker(ns[j], types[t][0], buf, sizeof(buf));
                printf("marker %s %d = %s\n", types[t], ns[j], buf);
            }
        const char *ls[] = {"a\tb", "\tx", "abc", "ab\t\tcd", "a\nb"};
        for (size_t i = 0; i < sizeof(ls)/sizeof(ls[0]); i++)
        {
            char buf[64];
            layout_expand_tab_columns(ls[i], buf, sizeof(buf));
            /* Lua %q escapes: tab → \9, newline → literal backslash-newline */
            printf("expand \"");
            for (const char *p = ls[i]; *p; p++)
            {
                if (*p == '\t') printf("\\9");
                else if (*p == '\n') printf("\\\n");
                else putchar(*p);
            }
            printf("\" = \"");
            for (const char *p = buf; *p; p++)
            {
                if (*p == '\t') printf("\\9");
                else if (*p == '\n') printf("\\\n");
                else putchar(*p);
            }
            printf("\"\n");
        }
    }
    else if (strcmp(which, "break") == 0)
    {
        DocInline mk[24];
        DocInline *mp[24];          /* pointers — the API takes DocInline** */
        const char *texts[24];
        memset(mk, 0, sizeof(mk));
        memset(mp, 0, sizeof(mp));
        int cnt = 0;
#define MKP(n) (mp[(n)] = &mk[(n)])

        /* v1: single inline, wraps at 120 */
        MKP(cnt);
        texts[cnt] = "the quick brown fox jumps over the lazy dog";
        mk[cnt].type = DOC_INLINE_TEXT; mk[cnt].text = (char *)texts[cnt]; cnt++;
        printf("v1:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[0], cnt, 120,
                                     NULL, 0, 0, emit_stdout);
        int v1end = cnt;

        /* v2: mixed inline fonts */
        MKP(cnt);
        texts[cnt] = "small "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; mk[cnt].flags = DOC_INF_SMALL; cnt++;
        MKP(cnt);
        texts[cnt] = "boldcode "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt];
        mk[cnt].flags = DOC_INF_BOLD | DOC_INF_CODE; cnt++;
        MKP(cnt);
        texts[cnt] = "plain words here"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        printf("v2:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[v1end],
                                     cnt - v1end, 90, NULL, 0, 0, emit_stdout);
        int v2end = cnt;

        /* v3: whitespace collapse + br */
        MKP(cnt);
        texts[cnt] = "  spaced   out  "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        MKP(cnt);
        mk[cnt].type = DOC_INLINE_BR; cnt++;
        MKP(cnt);
        texts[cnt] = "after"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        printf("v3:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[v2end],
                                     cnt - v2end, 200, NULL, 0, 0, emit_stdout);
        int v3end = cnt;

        /* v4: tab gap */
        MKP(cnt);
        texts[cnt] = "ab\tcd ef"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        printf("v4:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[v3end],
                                     cnt - v3end, 400, NULL, 0, 0, emit_stdout);
        int v4end = cnt;

        /* v5: first-word overflow */
        MKP(cnt);
        texts[cnt] = "superlongword"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        printf("v5:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[v4end],
                                     cnt - v4end, 50, NULL, 0, 0, emit_stdout);
        int v5end = cnt;

        /* v6: fixed font + lineH 15 */
        printf("v6:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[0], v1end, 130,
                                     style_font(PLUTO_FONT_MONO), 0, 15,
                                     emit_stdout);
        /* v7: bold */
        printf("v7:"); fflush(stdout);
        layout_test_dump_break_lines((const DocInline **)&mp[0], v1end, 130,
                                     NULL, 1, 0, emit_stdout);
    }
    else if (strcmp(which, "emit") == 0)
    {
        /* links: go [to the store] now with href on the middle inlines */
        DocInline mk[8];
        DocInline *mp[8];               /* pointer array — API takes DocInline** */
        const char *texts[8];
        memset(mk, 0, sizeof(mk));
        memset(mp, 0, sizeof(mp));
        int cnt = 0;
        MKP(cnt);
        texts[cnt] = "go "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;
        MKP(cnt);
        texts[cnt] = "to "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; mk[cnt].href = (char *)"http://x"; cnt++;
        MKP(cnt);
        texts[cnt] = "the "; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; mk[cnt].href = (char *)"http://x"; cnt++;
        MKP(cnt);
        texts[cnt] = "store"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; mk[cnt].href = (char *)"http://x"; cnt++;
        MKP(cnt);
        texts[cnt] = " now"; mk[cnt].type = DOC_INLINE_TEXT;
        mk[cnt].text = (char *)texts[cnt]; cnt++;

        /* Note: dumpBreakLines outputs tag "v"; the Lua harness uses vN names.
         * The diff script normalizes both. */
        for (int mode = 0; mode < 4; mode++)
        {
            const char *align = mode == 0 ? NULL : mode == 1 ? "left"
                              : mode == 2 ? "center" : "right";
            lm_clear();
            layout_clear();          /* Lua harness rebuilds the item list per mode */
            lastLinkCount = 0;
            layout_test_run_emit((const DocInline **)&mp[0], cnt, 400, NULL, 0, 0,
                                 10, 200, align, 33);
            /* Dump emitted items (LRI_TEXT) in the Lua harness's order. */
            printf("align=%s lh=%d endY=%d items=%d\n",
                   align ? align : "SENTINEL-NIL", layout_test_get_emit_line_h(),
                   layout_test_get_emit_end_y(), layout_get_item_count());
            for (int i = 0; i < layout_get_item_count(); i++)
            {
                const LayoutItem *it = layout_item_at(i);
                if (it->type == LRI_TEXT)
                {
                    printf("  text=\"%s\" font=%s x=%d y=%d w=%d h=%d href=%s bold=%s\n",
                           it->text, font_name(it->font), it->x, it->y, it->w,
                           it->h, it->href ? it->href : "nil",
                           it->bold ? "true" : "nil");
                }
            }
            for (int i = 1; i <= lm_get_link_count(); i++)
            {
                const LMLink *l = lm_link_at(i);
                printf("  link href=%s text=%s rect={%d,%d,%d,%d} anchor=%d\n",
                       l->href, l->text, l->rects[0].x, l->rects[0].y,
                       l->rects[0].w, l->rects[0].h, l->anchorIndex);
            }
        }
        /* sub/sup dy */
        lm_clear();
        layout_clear();
        DocInline dy[2];
        DocInline *dp[2];
        memset(dy, 0, sizeof(dy));
        memset(dp, 0, sizeof(dp));
        dp[0] = &dy[0]; dp[1] = &dy[1];
        dy[0].type = DOC_INLINE_TEXT; dy[0].text = (char *)"x";
        dy[0].flags = DOC_INF_SUB;
        dy[1].type = DOC_INLINE_TEXT; dy[1].text = (char *)"y";
        dy[1].flags = DOC_INF_SUP;
        layout_test_run_emit((const DocInline **)&dp[0], 2, 400, NULL, 0, 0,
                             0, 200, NULL, 10);
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            if (it->type == LRI_TEXT)
            {
                printf("dy text=%s y=%d\n", it->text, it->y);
            }
        }
    }
    else if (strcmp(which, "build") == 0)
    {
        /* Same constructed document as the Lua build vector (22 blocks +
         * one image map). Item projection mirrors run.lua's dump.
         * NOTE: each block gets its own inline-pointer slot (ip[n]) so later
         * blocks never clobber earlier ones. */
        static DocInline *ip[24][4];
        static DocInline ti[24][4];   /* per-slot storage — no cross-block aliasing */
        DocBlock *B[24];
        static DocBlock blocks[24];
        static DocOption opts[2];
        memset(blocks, 0, sizeof(blocks));
        memset(ti, 0, sizeof(ti));
        memset(opts, 0, sizeof(opts));
        memset(ip, 0, sizeof(ip));
        int bc = 0;
#define NB(t) (blocks[bc].type = (t), B[bc] = &blocks[bc], &blocks[bc++])
#define SETI(slot, idx, txt) (ip[(slot)][(idx)] = &ti[(slot)][(idx)], ti[(slot)][(idx)].type = DOC_INLINE_TEXT, ti[(slot)][(idx)].text = (char *)(txt))
        /* 1 reader header */
        DocBlock *b = NB(DOC_BLOCK_READER_HEADER);
        b->host = "example.com"; b->readingTime = "2 min";
        /* 2 heading h1 center */
        b = NB(DOC_BLOCK_HEADING);
        b->level = 1; b->align = "center";
        b->inlines = ip[1]; b->inlineCount = 1;
        SETI(1, 0, "Title One");
        /* 3 paragraph right */
        b = NB(DOC_BLOCK_PARAGRAPH);
        b->align = "right";
        SETI(2, 1, "Plain ");
        SETI(2, 2, "words ");
        SETI(2, 3, "here.");
        b->inlines = ip[2] + 1; b->inlineCount = 3;
        /* 4 blockquote */
        b = NB(DOC_BLOCK_BLOCKQUOTE);
        SETI(3, 0, "quoted words");
        b->inlines = ip[3]; b->inlineCount = 1;
        /* 5 list ordered a.3 */
        b = NB(DOC_BLOCK_LIST_ITEM);
        b->isOrdered = 1; b->hasNumber = 1; b->number = 3; b->markerType = 'a';
        SETI(4, 0, "first item");
        b->inlines = ip[4]; b->inlineCount = 1;
        /* 6 list nested */
        b = NB(DOC_BLOCK_LIST_ITEM);
        b->depth = 2; b->hasNumber = 1;
        SETI(5, 0, "nested");
        b->inlines = ip[5]; b->inlineCount = 1;
        /* 7 dt / 8 dd */
        b = NB(DOC_BLOCK_LIST_ITEM); b->dt = 1;
        SETI(6, 0, "term");
        b->inlines = ip[6]; b->inlineCount = 1;
        b = NB(DOC_BLOCK_LIST_ITEM); b->dd = 1;
        SETI(8, 0, "definition");
        b->inlines = ip[8]; b->inlineCount = 1;
        /* 9 code block */
        b = NB(DOC_BLOCK_CODE_BLOCK);
        static char *clines[4] = { (char *)"line one", (char *)"line two", (char *)"", (char *)"line four" };
        b->text = (char *)"line one\nline two\n\nline four";
        b->lines = clines; b->lineCount = 4;
        /* 10 hr */
        NB(DOC_BLOCK_HR);
        /* 11 image */
        b = NB(DOC_BLOCK_IMAGE);
        b->width = 100; b->height = 50; b->src = (char *)"i.png"; b->alt = (char *)"pic";
        b->align = "center"; b->href = (char *)"http://i"; b->usemap = (char *)"mymap";
        /* 12 table width=300 */
        b = NB(DOC_BLOCK_TABLE);
        static DocTable tbl;
        static DocRow rows[2];
        memset(&tbl, 0, sizeof(tbl));
        memset(rows, 0, sizeof(rows));
        tbl.caption = (char *)"tbl cap"; tbl.border = 1; tbl.width = (char *)"300";
        static DocRow *rp[2] = { &rows[0], &rows[1] };
        tbl.rows = rp; tbl.rowCount = 2;
        b->table = &tbl;
        /* 13 hidden field */
        b = NB(DOC_BLOCK_HIDDEN_FIELD);
        b->name = (char *)"hf"; b->value = (char *)"v1"; b->formAction = (char *)"http://f";
        /* 14 input field */
        b = NB(DOC_BLOCK_INPUT_FIELD);
        b->inputType = (char *)"text"; b->name = (char *)"user"; b->value = (char *)"me";
        b->placeholder = (char *)"type"; b->fieldWidth = 10; b->formAction = (char *)"http://f/submit";
        b->maxlength = -1; /* Lua nil sentinel */
        /* 15 submit */
        b = NB(DOC_BLOCK_INPUT_SUBMIT);
        b->label = (char *)"Go!"; b->name = (char *)"s"; b->value = (char *)"1";
        b->formAction = (char *)"http://f/submit";
        /* 16 checkbox */
        b = NB(DOC_BLOCK_CHECKBOX_FIELD);
        b->name = (char *)"cb"; b->label = (char *)"Check me"; b->checked = 1;
        /* 17 select */
        b = NB(DOC_BLOCK_SELECT_FIELD);
        b->name = (char *)"sel"; b->selectedIndex = 2;
        opts[0].text = (char *)"one"; opts[0].value = (char *)"1";
        opts[1].text = (char *)"two"; opts[1].value = (char *)"2";
        static DocOption *op[2] = { &opts[0], &opts[1] };
        b->options = op; b->optionCount = 2;
        b->formAction = (char *)"http://f/submit";
        /* 18 placeholder */
        b = NB(DOC_BLOCK_PLACEHOLDER);
        b->pwidth = 200; b->pheight = 60; b->plabel = (char *)"Video"; b->phref = (char *)"http://e";
        /* 19 meter */
        b = NB(DOC_BLOCK_METER);
        b->mvalue = 40; b->mmin = 0; b->mmax = 100; b->label = (char *)"m";
        /* 20 box_open / 21 paragraph / 22 box_close */
        b = NB(DOC_BLOCK_BOX_OPEN);
        b->label = (char *)"Details"; b->toggleKey = (char *)"d1"; b->toggleOpen = 1;
        b = NB(DOC_BLOCK_PARAGRAPH);
        SETI(7, 0, "inner");
        b->inlines = ip[7]; b->inlineCount = 1;
        NB(DOC_BLOCK_BOX_CLOSE);

        static DocMap map;
        static DocArea area;
        static int coords[4] = { 10, 20, 110, 70 };
        memset(&map, 0, sizeof(map));
        memset(&area, 0, sizeof(area));
        static DocArea *ap[1] = { &area };
        map.name = (char *)"mymap"; map.areas = ap; map.areaCount = 1;
        area.shape = (char *)"rect"; area.coords = coords; area.coordCount = 4;
        area.href = (char *)"http://m"; area.alt = (char *)"mapa";

        DocParseResult doc;
        memset(&doc, 0, sizeof(doc));
        doc.blocks = (DocBlock **)B;
        doc.blockCount = bc;
        static DocMap *mapp[1] = { &map };
        doc.maps = mapp;
        doc.mapCount = 1;

        layout_build(&doc);
        printf("items=%d links=%d totalHeight=%d\n", layout_get_item_count(),
               lm_get_link_count(), layout_get_total_height());
        for (int i = 0; i < layout_get_item_count(); i++)
        {
            const LayoutItem *it = layout_item_at(i);
            switch (it->type)
            {
            case LRI_TEXT:
                printf("  text=\"%s\" font=%s x=%d y=%d w=%d h=%d href=%s bold=%s italic=%s\n",
                       it->text, font_name(it->font), it->x, it->y, it->w, it->h,
                       it->href ? it->href : "nil", it->bold ? "true" : "nil",
                       it->italic ? "true" : "nil");
                break;
            case LRI_READER_BADGE:
                printf("  badge x=%d y=%d w=%d h=%d host=%s rt=%s\n", it->x, it->y,
                       it->w, it->h, it->host, it->readingTime);
                break;
            case LRI_LINE:
                printf("  line x1=%d y1=%d x2=%d y2=%d\n", it->x1, it->y1, it->x2, it->y2);
                break;
            case LRI_QUOTE_BAR:
                printf("  quote_bar x=%d y1=%d y2=%d\n", it->x, it->y1, it->y2);
                break;
            case LRI_CODE_BOX:
                printf("  code_box x=%d y=%d w=%d h=%d nlines=%d line1=\"%s\"\n",
                       it->x, it->y, it->w, it->h, it->lineCount, it->lines[0]);
                break;
            case LRI_IMAGE:
                printf("  image x=%d y=%d w=%d h=%d src=%s alt=%s href=%s\n",
                       it->x, it->y, it->w, it->h, it->src ? it->src : "nil",
                       it->alt ? it->alt : "nil", it->imgHref ? it->imgHref : "nil");
                break;
            case LRI_TABLE_BOX:
                printf("  table_box x=%d y=%d w=%d h=%d cap=%s border=%s\n",
                       it->x, it->y, it->w, it->h, it->caption ? it->caption : "nil",
                       it->border ? "true" : "false");
                break;
            case LRI_HIDDEN_FIELD:
                printf("  hidden name=%s value=%s fa=%s disabled=%s\n",
                       it->name, it->value, it->formAction ? it->formAction : "nil",
                       it->disabled ? "true" : "nil");
                break;
            case LRI_INPUT_FIELD:
                printf("  input x=%d y=%d w=%d h=%d type=%s name=%s value=%s ph=%s maxlength=%s\n",
                       it->x, it->y, it->w, it->h, it->inputType, it->name, it->value,
                       it->placeholder, it->maxlength >= 0 ? "num" : "nil");
                break;
            case LRI_INPUT_SUBMIT:
                printf("  submit x=%d y=%d w=%d h=%d label=%s\n",
                       it->x, it->y, it->w, it->h, it->label);
                break;
            case LRI_CHECKBOX_FIELD:
                printf("  checkbox x=%d y=%d w=%d h=%d checked=%s label=%s\n",
                       it->x, it->y, it->w, it->h, it->checked ? "true" : "nil", it->label);
                break;
            case LRI_SELECT_FIELD:
                printf("  select x=%d y=%d w=%d h=%d name=%s sel=%d nopts=%d\n",
                       it->x, it->y, it->w, it->h, it->name, it->selectedIndex,
                       it->optionCount);
                break;
            case LRI_PLACEHOLDER:
                printf("  placeholder x=%d y=%d w=%d h=%d label=%s\n",
                       it->x, it->y, it->w, it->h, it->label);
                break;
            case LRI_METER:
                char mv[24], mn[24], mx[24];
                snprintf(mv, sizeof(mv), "%g", it->mValue);
                snprintf(mn, sizeof(mn), "%g", it->mMin);
                snprintf(mx, sizeof(mx), "%g", it->mMax);
                printf("  meter x=%d y=%d w=%d h=%d value=%s min=%s max=%s\n",
                       it->x, it->y, it->w, it->h, mv, mn, mx);
                break;
            case LRI_BOX_FRAME:
                printf("  box_frame x=%d y=%d w=%d y2=%d label=%s toggle=%s open=%s\n",
                       it->x, it->y, it->w, it->y2, it->label,
                       it->toggleKey ? it->toggleKey : "nil",
                       it->toggleOpen ? "true" : "false");
                break;
            default:
                printf("  UNHANDLED type=%d\n", (int)it->type);
                break;
            }
        }
        for (int i = 1; i <= lm_get_link_count(); i++)
        {
            const LMLink *l = lm_link_at(i);
            const LMRectAux *a = lm_rect_aux(i, 0); /* rectIndex is 0-based */
            printf("  link href=%s text=%s rect={%d,%d,%d,%d} anchor=%s isImage=%s isFormInput=%s isToggle=%s\n",
                   l->href, l->text, l->primaryRect.x, l->primaryRect.y,
                   l->primaryRect.w, l->primaryRect.h,
                   l->anchorIndex ? "1" : "nil",
                   (a && a->isImage) ? "true" : "nil",
                   (a && a->isFormInput) ? "true" : "nil",
                   (a && a->isToggle) ? "true" : "nil");
        }
        /* build2: percent-width table → reference raises (Layout Error path) */
        {
            static DocBlock blocks2[3];
            static DocTable tbl2;
            DocBlock *B2[3];
            memset(blocks2, 0, sizeof(blocks2));
            memset(&tbl2, 0, sizeof(tbl2));
            blocks2[0].type = DOC_BLOCK_PARAGRAPH;
            static DocInline tb[2];
            memset(tb, 0, sizeof(tb));
            static DocInline *tbp1[1]; tbp1[0] = &tb[0];
            tb[0].type = DOC_INLINE_TEXT; tb[0].text = (char *)"before table";
            blocks2[0].inlines = tbp1; blocks2[0].inlineCount = 1;
            B2[0] = &blocks2[0]; B2[1] = &blocks2[1]; B2[2] = &blocks2[2];
            blocks2[1].type = DOC_BLOCK_TABLE;
            tbl2.caption = (char *)"pct"; tbl2.border = 1; tbl2.width = (char *)"50%";
            static DocRow rows2[2];
            memset(rows2, 0, sizeof(rows2));
            static DocRow *rp2[2] = { &rows2[0], &rows2[1] };
            tbl2.rows = rp2; tbl2.rowCount = 2;
            blocks2[1].table = &tbl2;
            blocks2[2].type = DOC_BLOCK_PARAGRAPH;
            static DocInline *tbp2[1]; tbp2[0] = &tb[1];
            tb[1].type = DOC_INLINE_TEXT; tb[1].text = (char *)"after table";
            blocks2[2].inlines = tbp2; blocks2[2].inlineCount = 1;
            DocParseResult doc2;
            memset(&doc2, 0, sizeof(doc2));
            doc2.blocks = (DocBlock **)B2;
            doc2.blockCount = 3;
            layout_build(&doc2);
            printf("build2 error=%s (percent-width)\n", layout_build_failed() ? "true" : "false");
        }
    }
    return 0;
}
