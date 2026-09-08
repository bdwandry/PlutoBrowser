/*
 * PlutoBrowser — cloud_layout.c
 * Port of Source/render/cloud_layout.lua (reference, 152 lines).
 * See cloud_layout.h for the mapping and preserved semantics.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "render/cloud_layout.h"
#include "render/link_manager.h"
#include "render/style.h"
#include "render/image_decoder.h"
#include "core/constants.h"
#include "util/json.h"

extern PlaydateAPI *pluto_pd(void);

/* ── Module state (Lua CloudLayout.renderItems / totalHeight / selectedInputItem) ── */

typedef struct CloudItem
{
    char type[8];      /* "text" | "image" | "input" | "submit" */
    int x, y, w, h;
    char *text;        /* text: the string; input: value; heap copies */
    char *placeholder;
    char *label;       /* submit */
    char *src;         /* image */
    LCDFont *font;     /* text: resolved font pointer */
    char *inputType;   /* input/submit passthrough for the input handler */
    char *name;
    char *formAction;
} CloudItem;

static CloudItem *g_items = NULL;
static int g_itemCount = 0;
static int g_itemCap = 0;
static int g_totalHeight = 0;
static int g_selectedInputIndex = -1; /* Lua selectedInputItem; index into g_items */

static char *dup_str(const char *s)
{
    if (!s)
    {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
    {
        memcpy(d, s, n);
    }
    return d;
}

static void items_reset(void)
{
    for (int i = 0; i < g_itemCount; i++)
    {
        free(g_items[i].text);
        free(g_items[i].placeholder);
        free(g_items[i].label);
        free(g_items[i].src);
        free(g_items[i].inputType);
        free(g_items[i].name);
        free(g_items[i].formAction);
    }
    g_itemCount = 0;
    g_selectedInputIndex = -1;
}

static CloudItem *item_new(const char *type, int x, int y, int w, int h)
{
    if (g_itemCount == g_itemCap)
    {
        int nc = g_itemCap ? g_itemCap * 2 : 16;
        CloudItem *ni = (CloudItem *)realloc(g_items, (size_t)nc * sizeof(CloudItem));
        if (!ni)
        {
            return NULL;
        }
        g_items = ni;
        g_itemCap = nc;
    }
    CloudItem *it = &g_items[g_itemCount++];
    memset(it, 0, sizeof(*it));
    snprintf(it->type, sizeof(it->type), "%s", type ? type : "");
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
    return it;
}

/* ── parse ─────────────────────────────────────────────────────────────────── */

static int elem_from_json(const JsonValue *o, CloudElement *el)
{
    memset(el, 0, sizeof(*el));
    const char *s;
    double num;

    if (!json_as_string(json_get(o, "type"), &s))
    {
        return 0; /* Lua would index el.type as a non-string → decode error path */
    }
    el->type = dup_str(s);

    /* Numeric fields: Lua would throw on non-numbers when doing arithmetic
     * (el.y >= 0 etc.), which is a parse/pcall-level failure. Treat missing
     * numbers as an error too (the cloud producer always emits them). */
    if (!json_as_number(json_get(o, "x"), &num))
    {
        return 0;
    }
    el->x = (int)num;
    if (!json_as_number(json_get(o, "y"), &num))
    {
        return 0;
    }
    el->y = (int)num;
    if (!json_as_number(json_get(o, "w"), &num))
    {
        return 0;
    }
    el->w = (int)num;
    if (!json_as_number(json_get(o, "h"), &num))
    {
        return 0;
    }
    el->h = (int)num;

    if (json_as_string(json_get(o, "text"), &s))
    {
        el->text = dup_str(s);
    }
    if (json_as_string(json_get(o, "font"), &s))
    {
        el->font = dup_str(s);
    }
    if (json_as_string(json_get(o, "src"), &s))
    {
        el->src = dup_str(s);
    }
    if (json_as_string(json_get(o, "href"), &s))
    {
        el->href = dup_str(s);
    }
    if (json_as_string(json_get(o, "inputType"), &s))
    {
        el->inputType = dup_str(s);
    }
    if (json_as_string(json_get(o, "name"), &s))
    {
        el->name = dup_str(s);
    }
    if (json_as_string(json_get(o, "value"), &s))
    {
        el->value = dup_str(s);
    }
    if (json_as_string(json_get(o, "placeholder"), &s))
    {
        el->placeholder = dup_str(s);
    }
    if (json_as_string(json_get(o, "formAction"), &s))
    {
        el->formAction = dup_str(s);
    }
    if (json_as_string(json_get(o, "label"), &s))
    {
        el->label = dup_str(s);
    }
    return 1;
}

CloudDoc *cloud_parse(const char *jsonString, const char *baseUrl)
{
    (void)baseUrl; /* accepted for signature parity; the reference ignores it */

    JsonValue *root = json_decode(jsonString);
    if (!root || root->type != JSON_OBJECT)
    {
        json_free(root);
        return NULL;
    }

    CloudDoc *doc = (CloudDoc *)calloc(1, sizeof(CloudDoc));
    if (!doc)
    {
        json_free(root);
        return NULL;
    }

    /* Lua: title = data.title or "Cloud Page" (null/absent → fallback). */
    const char *s;
    double num;
    doc->title = dup_str(json_as_string(json_get(root, "title"), &s) ? s : "Cloud Page");

    JsonValue *elements = json_get(root, "elements");
    if (elements && elements->type == JSON_ARRAY && elements->count > 0)
    {
        doc->elements = (CloudElement *)calloc(elements->count, sizeof(CloudElement));
        if (!doc->elements)
        {
            json_free(root);
            cloud_free_doc(doc);
            return NULL;
        }
        for (size_t i = 0; i < elements->count; i++)
        {
            CloudElement el;
            if (!elem_from_json(elements->items[i], &el))
            {
                /* Malformed element → whole parse fails (Lua pcall path). */
                json_free(root);
                cloud_free_doc(doc);
                return NULL;
            }
            doc->elements[doc->elementCount++] = el;
        }
    }

    if (json_as_number(json_get(root, "totalHeight"), &num))
    {
        doc->totalHeight = (int)num;
    }
    else
    {
        doc->totalHeight = 240; /* Lua `or 240` */
    }

    json_free(root);
    return doc;
}

CloudDoc *cloud_parse_error_doc(void)
{
    CloudDoc *doc = (CloudDoc *)calloc(1, sizeof(CloudDoc));
    if (!doc)
    {
        return NULL;
    }
    doc->title = dup_str("Parse Error");
    doc->totalHeight = 240;
    return doc;
}

void cloud_free_doc(CloudDoc *doc)
{
    if (!doc)
    {
        return;
    }
    free(doc->title);
    for (int i = 0; i < doc->elementCount; i++)
    {
        CloudElement *el = &doc->elements[i];
        free(el->type);
        free(el->text);
        free(el->font);
        free(el->src);
        free(el->href);
        free(el->inputType);
        free(el->name);
        free(el->value);
        free(el->placeholder);
        free(el->formAction);
        free(el->label);
    }
    free(doc->elements);
    free(doc);
}

/* ── build ─────────────────────────────────────────────────────────────────── */

void cloud_build(const CloudDoc *doc)
{
    items_reset();
    lm_clear();

    if (!doc || doc->elementCount == 0)
    {
        g_totalHeight = CONTENT_HEIGHT;
        return;
    }

    g_totalHeight = doc->totalHeight;

    for (int i = 0; i < doc->elementCount; i++)
    {
        const CloudElement *el = &doc->elements[i];
        if (el->y < 0)
        {
            continue; /* Lua: skip anything placed above the page */
        }
        int yOffset = CONTENT_Y + el->y;

        if (strcmp(el->type, "text") == 0)
        {
            /* Lua font chain: named style font → gfx.getFont() fallback. */
            LCDFont *font = style_font(PLUTO_FONT_BODY);
            if (el->font && strcmp(el->font, "large") == 0)
            {
                font = style_font(PLUTO_FONT_HEADING1);
            }
            else if (el->font && strcmp(el->font, "bold") == 0)
            {
                font = style_font(PLUTO_FONT_BODY_BOLD);
            }
            else if (el->font && strcmp(el->font, "mono") == 0)
            {
                font = style_font(PLUTO_FONT_MONO);
            }

            CloudItem *it = item_new("text", el->x, yOffset, el->w, el->h);
            if (!it)
            {
                return;
            }
            it->text = dup_str(el->text);
            it->font = font;
        }
        else if (strcmp(el->type, "image") == 0)
        {
            CloudItem *it = item_new("image", el->x, yOffset, el->w, el->h);
            if (!it)
            {
                return;
            }
            it->src = dup_str(el->src);
            /* Lua hardcodes alt = "Image" for cloud images. */
        }
        else if (strcmp(el->type, "link") == 0)
        {
            /* Lua goes through LinkManager.addLink, which records the rect
             * with the href as both href and text — isFormInput/inputBlock
             * do not exist on this path (reference quirk, preserved). */
            lm_add_link(el->href, el->x, yOffset, el->w, el->h);
        }
        else if (strcmp(el->type, "input") == 0 || strcmp(el->type, "submit") == 0)
        {
            CloudItem *it = item_new(el->type, el->x, yOffset, el->w, el->h);
            if (!it)
            {
                return;
            }
            it->inputType = dup_str(el->inputType);
            it->name = dup_str(el->name);
            it->text = dup_str(el->value);
            it->placeholder = dup_str(el->placeholder);
            it->formAction = dup_str(el->formAction);
            it->label = dup_str(el->label);

            /* The Lua item carries these extra fields; the rect records the
             * item pointer so the input handler can find it (same mechanism
             * as Layout's inputBlock). The link rect itself is added with
             * href == nil → addLinkRect returns immediately... but the Lua
             * code still calls it, and addLinkRect with a nil href is a
             * no-op. Reproduced: we add the rect via the aux API with href
             * NULL and the item attached — lm_add_link_rect_ex ignores NULL
             * hrefs exactly like the Lua guard. */
            LMRect rect = {el->x, yOffset, el->w, el->h};
            LMRectAux aux;
            memset(&aux, 0, sizeof(aux));
            aux.isFormInput = 1;
            aux.inputItem = it;
            lm_add_link_rect_ex(NULL, NULL, &rect, 0, &aux);
        }
        /* else: silently ignored (Lua if/elseif chain) */
    }
}

/* ── draw ──────────────────────────────────────────────────────────────────── */

void cloud_draw(int scrollY)
{
    PlaydateAPI *pd = pluto_pd();
    if (!pd)
    {
        return;
    }

    /* Background */
    pd->graphics->fillRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT, kColorWhite);

    pd->graphics->pushContext(NULL);
    pd->graphics->setClipRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_HEIGHT);

    for (int i = 0; i < g_itemCount; i++)
    {
        CloudItem *item = &g_items[i];
        int drawY = item->y - scrollY;
        if (drawY + item->h < CONTENT_Y || drawY > SCREEN_HEIGHT)
        {
            continue; /* only draw if visible */
        }
        if (strcmp(item->type, "text") == 0)
        {
            pd->graphics->setFont(item->font);
            const char *txt = item->text ? item->text : "";
            pd->graphics->drawTextInRect(txt, strlen(txt), kUTF8Encoding,
                                         item->x, drawY, item->w + 10, item->h + 10,
                                         kWrapWord, kAlignTextLeft);
        }
        else if (strcmp(item->type, "image") == 0)
        {
            imgdec_draw(item->x, drawY, item->w, item->h, "Image", NULL, 0, item->src);
        }
        else if (strcmp(item->type, "input") == 0)
        {
            pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 3, kColorWhite);
            pd->graphics->drawRoundRect(item->x, drawY, item->w, item->h, 3, 1, kColorBlack);

            pd->graphics->setFont(style_font(PLUTO_FONT_BODY));
            /* Lua: (item.value and item.value ~= "") and item.value or item.placeholder */
            const char *txt;
            if (item->text && item->text[0] != '\0')
            {
                txt = item->text;
            }
            else
            {
                txt = item->placeholder ? item->placeholder : "";
            }
            pd->graphics->drawTextInRect(txt, strlen(txt), kUTF8Encoding,
                                         item->x + 4, drawY + 4, item->w - 8, item->h - 8,
                                         kWrapWord, kAlignTextLeft);
        }
        else if (strcmp(item->type, "submit") == 0)
        {
            pd->graphics->fillRoundRect(item->x, drawY, item->w, item->h, 3, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
            pd->graphics->setFont(style_font(PLUTO_FONT_BODY_BOLD));
            const char *lbl = item->label ? item->label : "";
            int lw = style_get_text_width(PLUTO_FONT_BODY_BOLD, lbl);
            pd->graphics->drawText(lbl, strlen(lbl), kUTF8Encoding,
                                   item->x + (item->w - lw) / 2,
                                   drawY + (item->h - 14) / 2);
            pd->graphics->setDrawMode(kDrawModeCopy);
        }
    }

    /* Link highlight ring */
    int selIdx = lm_get_selected_index();
    if (selIdx > 0)
    {
        const LMLink *selLink = lm_link_at(selIdx);
        if (selLink)
        {
            int selY = selLink->primaryRect.y - scrollY;
            if (selY + selLink->primaryRect.h >= CONTENT_Y &&
                selY <= SCREEN_HEIGHT)
            {
                pd->graphics->drawRoundRect(selLink->primaryRect.x - 2, selY - 2,
                                            selLink->primaryRect.w + 4,
                                            selLink->primaryRect.h + 4,
                                            3, 3, kColorBlack);
                /* Lua's setLineWidth(1) restore is implicit: C passes the
                 * width per call, so nothing further is needed. */
            }
        }
    }

    pd->graphics->popContext();

    /* Scrollbar */
    int maxScroll = g_totalHeight - CONTENT_HEIGHT;
    if (maxScroll < 0)
    {
        maxScroll = 0;
    }
    if (maxScroll > 0)
    {
        int sbH = (int)((CONTENT_HEIGHT / (float)g_totalHeight) * CONTENT_HEIGHT);
        if (sbH < 20)
        {
            sbH = 20; /* Lua math.max(20, …) */
        }
        int sbY = CONTENT_Y + (int)(((float)scrollY / maxScroll) * (CONTENT_HEIGHT - sbH));
        pd->graphics->fillRect(SCREEN_WIDTH - SCROLLBAR_WIDTH, sbY,
                               SCROLLBAR_WIDTH, sbH, kColorBlack);
    }
}

/* ── accessors ─────────────────────────────────────────────────────────────── */

int cloud_total_height(void)
{
    return g_totalHeight;
}

int cloud_item_count(void)
{
    return g_itemCount;
}

void cloud_clear(void)
{
    items_reset();
    g_totalHeight = 0;
}
