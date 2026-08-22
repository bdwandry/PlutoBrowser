// cloud_layout.c — C port of Source/render/cloud_layout.lua (CloudLayout).
//
// See cloud_layout.h for parity notes. Source of truth is the Lua module;
// every branch below mirrors it in order.

#include "render/cloud_layout.h"

#include <stdlib.h>
#include <string.h>

#include "core/constants.h"
#include "core/logger.h"
#include "render/image_decoder.h"
#include "render/link_manager.h"
#include "render/style.h"
#include "util/json.h"
#include "util/mem.h"

#if defined(TARGET_SIMULATOR) || defined(TARGET_PLAYDATE)
#define PLUTO_CL_PD 1
#endif

/* ── state ────────────────────────────────────────────────────────────── */

static ClItem* s_items = NULL;
static size_t s_nItems = 0, s_capItems = 0;
static double s_totalHeight = 0.0;
static ClItem* s_selectedInputItem = NULL;   /* nil in current Lua source */

#ifdef PLUTO_CL_PD
#include "pd_api.h"
static PlaydateAPI* s_pd = NULL;

void cl_init(struct PlaydateAPI* pd) { s_pd = pd; }
#else
void cl_init(struct PlaydateAPI* pd) { (void)pd; }
#endif

/* ── item helpers ─────────────────────────────────────────────────────── */

static char* dup_str(const char* s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char* d = pluto_malloc(n);
    if (d != NULL) memcpy(d, s, n);
    return d;
}

void cl_free_items(void) {
    for (size_t i = 0; i < s_nItems; i++) {
        ClItem* it = &s_items[i];
        pluto_free(it->text);
        pluto_free(it->src);
        pluto_free(it->alt);
        pluto_free(it->inputType);
        pluto_free(it->name);
        pluto_free(it->value);
        pluto_free(it->placeholder);
        pluto_free(it->formAction);
        pluto_free(it->label);
    }
    pluto_free(s_items);
    s_items = NULL;
    s_nItems = s_capItems = 0;
    s_selectedInputItem = NULL;
}

double cl_total_height(void) { return s_totalHeight; }

int cl_item_count(void) { return (int)s_nItems; }

const ClItem* cl_item_at(int i) {
    if (i < 0 || (size_t)i >= s_nItems) return NULL;
    return &s_items[i];
}

const ClItem* cl_selected_input(void) { return s_selectedInputItem; }

/* ── parse ────────────────────────────────────────────────────────────── */

static double num_field(const JsonValue* obj, const char* key, double dflt) {
    return json_num(json_obj_get(obj, key), dflt);
}

static const char* str_field(const JsonValue* obj, const char* key) {
    const JsonValue* v = json_obj_get(obj, key);
    return (v != NULL && v->type == JSON_STRING) ? v->str : NULL;
}

struct ClDoc {
    char* title;                      /* owned; never NULL after parse */
    struct JsonValue* root;           /* owned parsed JSON */
    const struct JsonValue* elements; /* borrowed from root; array or NULL */
    int parseFailed;
    double totalHeight;
};

ClDoc* cl_parse(const char* jsonStr, size_t len, const char* baseUrl) {
    /* baseUrl is accepted but unused in the Lua source too. */
    (void)baseUrl;

    ClDoc* doc = pluto_calloc(1, sizeof(ClDoc));
    if (doc == NULL) return NULL;

    char err[128];
    doc->root = json_parse(jsonStr ? jsonStr : "", jsonStr ? len : 0, err);

    /* Lua wraps decode + field access in pcall: any failure yields
     * { title="Parse Error", elements={}, totalHeight=240 }. A decoded
     * non-table hits the same path (data.title would raise). */
    if (doc->root == NULL || doc->root->type != JSON_OBJECT) {
        if (doc->root != NULL) {
            json_free(doc->root);
            doc->root = NULL;
        }
        doc->parseFailed = 1;
        doc->title = dup_str("Parse Error");
        if (doc->title == NULL) {
            pluto_free(doc);
            return NULL;
        }
        doc->totalHeight = 240.0;
        PLUTO_LOG("[P26] CloudLayout.parse failed: %s",
                  err[0] != '\0' ? err : "payload is not a JSON object");
        return doc;
    }

    const char* t = str_field(doc->root, "title");
    doc->title = dup_str(t != NULL ? t : "Cloud Page");

    const JsonValue* els = json_obj_get(doc->root, "elements");
    doc->elements = (els != NULL && els->type == JSON_ARRAY) ? els : NULL;

    doc->totalHeight = num_field(doc->root, "totalHeight", 240.0);

    if (doc->title == NULL) {
        json_free(doc->root);
        pluto_free(doc);
        return NULL;
    }
    return doc;
}

void cl_doc_free(ClDoc* doc) {
    if (doc == NULL) return;
    pluto_free(doc->title);
    if (doc->root != NULL) json_free(doc->root);
    pluto_free(doc);
}

const char* cl_doc_title(const ClDoc* doc) {
    static const char* kEmpty = "";
    return (doc != NULL && doc->title != NULL) ? doc->title : kEmpty;
}

const struct JsonValue* cl_doc_elements(const ClDoc* doc) {
    return doc != NULL ? doc->elements : NULL;
}

double cl_doc_total_height(const ClDoc* doc) {
    return doc != NULL ? doc->totalHeight : 0.0;
}

/* ── build ────────────────────────────────────────────────────────────── */

static int append_item(ClItem it) {
    if (s_nItems == s_capItems) {
        size_t nc = s_capItems ? s_capItems * 2 : 8;
        ClItem* ns = pluto_realloc(s_items, nc * sizeof(ClItem));
        if (ns == NULL) return 0;
        s_items = ns;
        s_capItems = nc;
    }
    s_items[s_nItems++] = it;
    return 1;
}

static PlutoFont* font_for_text_item(const JsonValue* el) {
    /* Lua picks Style slots with `or gfx.getFont()` fallbacks; the C style
     * getters perform that fallback internally. */
    const JsonValue* f = json_obj_get(el, "font");
    const char* fname =
        (f != NULL && f->type == JSON_STRING) ? f->str : NULL;
    int sz, lh;
    if (fname != NULL && strcmp(fname, "large") == 0)
        return style_get_heading_font(1, NULL, NULL);
    if (fname != NULL && strcmp(fname, "bold") == 0)
        return style_get_body_font(1, 0, &sz);
    if (fname != NULL && strcmp(fname, "mono") == 0)
        return style_get_body_font(0, 1, &sz);
    return style_get_body_font(0, 0, &sz);
}

static void add_element(const JsonValue* el) {
    double y = num_field(el, "y", 0.0);
    if (y < 0) return; /* Lua skips offscreen elements */

    int xOff = (int)num_field(el, "x", 0.0);
    int yOff = PLUTO_CONTENT_Y + (int)y;
    int w = (int)num_field(el, "w", 0.0);
    int h = (int)num_field(el, "h", 0.0);

    const char* type = str_field(el, "type");
    if (type == NULL) type = "";

    if (strcmp(type, "text") == 0) {
        ClItem it;
        memset(&it, 0, sizeof(it));
        it.type = CL_TEXT;
        it.x = xOff;
        it.y = yOff;
        it.w = w;
        it.h = h;
        it.text = dup_str(str_field(el, "text"));
        it.font = font_for_text_item(el);
        append_item(it);
    } else if (strcmp(type, "image") == 0) {
        ClItem it;
        memset(&it, 0, sizeof(it));
        it.type = CL_IMAGE;
        it.x = xOff;
        it.y = yOff;
        it.w = w;
        it.h = h;
        it.src = dup_str(str_field(el, "src"));
        it.alt = dup_str("Image"); /* Lua default */
        append_item(it);
    } else if (strcmp(type, "link") == 0) {
        lm_add_link(str_field(el, "href"), xOff, yOff, w, h);
    } else if (strcmp(type, "input") == 0 || strcmp(type, "submit") == 0) {
        ClItem it;
        memset(&it, 0, sizeof(it));
        it.type = (type[0] == 'i') ? CL_INPUT : CL_SUBMIT;
        it.x = xOff;
        it.y = yOff;
        it.w = w;
        it.h = h;
#define CL_DUPF(field, key) it.field = dup_str(str_field(el, key))
        CL_DUPF(inputType, "inputType");
        CL_DUPF(name, "name");
        CL_DUPF(value, "value");
        CL_DUPF(placeholder, "placeholder");
        CL_DUPF(formAction, "formAction");
        CL_DUPF(label, "label");
#undef CL_DUPF
        if (!append_item(it)) return;
        /* Lua also registers a link so inputs are selectable:
         * LinkManager.addLink({x,y,w,h,isFormInput=true,inputBlock=item}) */
        lm_add_form_input(xOff, yOff, w, h, &s_items[s_nItems - 1]);
    }
}

void cl_build(const ClDoc* doc) {
    cl_free_items();
    lm_clear();

    const JsonValue* elements = (doc != NULL) ? doc->elements : NULL;
    if (elements == NULL || json_arr_count(elements) == 0) {
        s_totalHeight = PLUTO_CONTENT_HEIGHT;
        return;
    }
    s_totalHeight = doc->totalHeight;

    size_t n = json_arr_count(elements);
    for (size_t i = 0; i < n; i++) {
        const JsonValue* el = json_arr_get(elements, i);
        if (el != NULL && el->type == JSON_OBJECT) add_element(el);
    }
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void cl_draw(int scrollY) {
#ifndef PLUTO_CL_PD
    (void)scrollY;
#else
    if (s_pd == NULL) return;
    PlaydateAPI* pd = s_pd;

    /* White content background */
    pd->graphics->fillRect(0, PLUTO_CONTENT_Y, PLUTO_SCREEN_WIDTH,
                           PLUTO_CONTENT_HEIGHT, kColorWhite);

    pd->graphics->pushContext(NULL);
    pd->graphics->setClipRect(0, PLUTO_CONTENT_Y, PLUTO_SCREEN_WIDTH,
                              PLUTO_CONTENT_HEIGHT);

    for (size_t i = 0; i < s_nItems; i++) {
        const ClItem* it = &s_items[i];
        int drawY = it->y - scrollY;
        /* Lua visibility gate: bottom edge past CONTENT_Y and top edge
         * above SCREEN_HEIGHT (not CONTENT_HEIGHT). */
        if (!(drawY + it->h >= PLUTO_CONTENT_Y &&
              drawY <= PLUTO_SCREEN_HEIGHT))
            continue;

        switch (it->type) {
        case CL_TEXT: {
            pd->graphics->setFont((LCDFont*)it->font);
            const char* t = it->text ? it->text : "";
            pd->graphics->drawTextInRect(t, strlen(t), kASCIIEncoding,
                                         it->x, drawY, it->w + 10,
                                         it->h + 10, kWrapWord,
                                         kAlignTextLeft);
            break;
        }
        case CL_IMAGE:
            id_draw(it->x, drawY, it->w, it->h, it->alt, NULL, 0, it->src);
            break;
        case CL_INPUT: {
            pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 3,
                                        kColorWhite);
            pd->graphics->drawRoundRect(it->x, drawY, it->w, it->h, 3, 1,
                                        kColorBlack);
            int sz;
            pd->graphics->setFont(
                (LCDFont*)style_get_body_font(0, 0, &sz));
            const char* txt = (it->value != NULL && it->value[0] != '\0')
                                  ? it->value
                                  : it->placeholder;
            if (txt == NULL) txt = "";
            pd->graphics->drawTextInRect(txt, strlen(txt), kASCIIEncoding,
                                         it->x + 4, drawY + 4, it->w - 8,
                                         it->h - 8, kWrapWord,
                                         kAlignTextLeft);
            break;
        }
        case CL_SUBMIT: {
            pd->graphics->fillRoundRect(it->x, drawY, it->w, it->h, 3,
                                        kColorBlack);
            int sz;
            PlutoFont* bold = style_get_body_font(1, 0, &sz);
            pd->graphics->setFont((LCDFont*)bold);
            if (it->label != NULL) {
                int lw = style_get_text_width(bold, it->label);
                pd->graphics->setDrawMode(kDrawModeFillWhite);
                pd->graphics->drawText(it->label, strlen(it->label),
                                       kASCIIEncoding,
                                       it->x + (it->w - lw) / 2,
                                       drawY + (it->h - 14) / 2);
                pd->graphics->setDrawMode(kDrawModeCopy);
            }
            break;
        }
        }
    }

    /* Selected-link outline: CloudLayout draws its own (black, line width
     * 3, roundRect r3 inflated by 2px), distinct from
     * LinkManager.drawSelectedHighlight. */
    LmLink* sel = lm_get_selected_link();
    if (sel != NULL) {
        int sy = sel->primaryRect.y - scrollY;
        if (sy + sel->primaryRect.h >= PLUTO_CONTENT_Y &&
            sy <= PLUTO_SCREEN_HEIGHT) {
            pd->graphics->drawRoundRect(sel->primaryRect.x - 2, sy - 2,
                                        sel->primaryRect.w + 4,
                                        sel->primaryRect.h + 4, 3, 3,
                                        kColorBlack);
        }
    }

    pd->graphics->popContext();

    /* Scrollbar */
    double maxScroll = s_totalHeight > (double)PLUTO_CONTENT_HEIGHT
                           ? s_totalHeight - (double)PLUTO_CONTENT_HEIGHT
                           : 0.0;
    if (maxScroll > 0) {
        double sbH =
            ((double)PLUTO_CONTENT_HEIGHT / s_totalHeight) *
            (double)PLUTO_CONTENT_HEIGHT;
        if (sbH < 20) sbH = 20;
        double sbY =
            (double)PLUTO_CONTENT_Y +
            ((double)scrollY / maxScroll) *
                ((double)PLUTO_CONTENT_HEIGHT - sbH);
        pd->graphics->fillRect(
            PLUTO_SCREEN_WIDTH - PLUTO_SCROLLBAR_WIDTH, (int)sbY,
            PLUTO_SCROLLBAR_WIDTH, (int)sbH, kColorBlack);
    }
#endif
}
