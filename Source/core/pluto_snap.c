/*
 * pluto_snap — SW6 rendered-snapshot cache (the "proxy on device").
 *
 * After a page renders successfully, its FINAL WALK OUTPUT — blocks, links,
 * tables, maps, datalists, title/baseUrl/metaRefresh (everything
 * layout_build consumes) — is serialized to the persistent spill store
 * (pluto_spill_store_*, the SW4 family). A revisit of the same URL within
 * the TTL loads the snapshot instead of re-fetching/re-parsing/re-running
 * scripts: network 0, parse 0, engine 0, layout from restored blocks.
 *
 * WHY serialize walk output instead of raw HTML:
 *   - document_rewalk proves layout consumes ONLY blocks/links/tables (plus
 *     title/baseUrl): the walk output is a self-sufficient render model.
 *   - The walk output IS the post-JS state (JS-mutated DOM, executed
 *     document.write, form values...), so a snapshot replays exactly what
 *     the user saw — "the server is the device's own past work".
 *   - Restored strings land in a fresh doc arena, mirroring the walker's
 *     memory model: document_free frees walk arrays + arena unchanged.
 *   - CSS: layout does not consume doc->_css (style bits are pre-baked into
 *     blocks/inlines by the walker at parse/rewalk time).
 *
 * CACHE-KEY + FAMILIES: the store keys on url hash + mode + version salt.
 * A 12-byte header (magic "PLS1", version, epoch, mode, counts; TTL checked
 * at load) validates every entry; a key collision (SW4 bytecode family or
 * another snapshot) fails the magic check and falls through to the classic
 * network path — the cache only speeds up, never breaks.
 *
 * INVALIDATION: TTL (age check at load), explicit invalidate (hard reload),
 * and a store-count LRU sweep (oldest-slot-first, same policy family as
 * SW4). Read handles are released with pluto_spill_store_close(); entries
 * are never deleted implicitly while a page is live.
 *
 * Source/js stays stock; engines never see this layer.
 */
#include "pluto_snap.h"
#include "pluto_spill.h"
#include "pluto_mem.h"
#include "html/document.h"
#include "core/logger.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Funnel allocation (SW1): every heap touch in this module goes through the
 * pluto_mem funnel — same convention as document.c/strbuf.c/etc. */
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p) pluto_mem_realloc((p), 0)
#define PLUTO_REALLOC(p, n) pluto_mem_realloc((p), (n))

/* ── byte writer (little-endian; same discipline as the token stream) ────── */

typedef struct
{
    SpillFile h;
    int err;   /* sticky write error */
} SnapWriter;

static void w_u8(SnapWriter *w, unsigned v)
{
    unsigned char b = (unsigned char)(v & 0xFF);
    if (pluto_spill_write(w->h, &b, 1) != 0)
    {
        w->err = 1;
    }
}

static void w_i32(SnapWriter *w, long v)
{
    unsigned char b[4];
    b[0] = (unsigned char)((unsigned long)v & 0xFF);
    b[1] = (unsigned char)(((unsigned long)v >> 8) & 0xFF);
    b[2] = (unsigned char)(((unsigned long)v >> 16) & 0xFF);
    b[3] = (unsigned char)(((unsigned long)v >> 24) & 0xFF);
    if (pluto_spill_write(w->h, &b, 4) != 0)
    {
        w->err = 1;
    }
}

/* length-prefixed string (len u16 + bytes, no NUL; NULL = len 0) */
static void w_str(SnapWriter *w, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > 65000)
    {
        len = 65000;
    }
    w_i32(w, (long)len);
    if (len > 0)
    {
        if (pluto_spill_write(w->h, s, len) != 0)
        {
            w->err = 1;
        }
    }
}

static void w_bytes(SnapWriter *w, const void *p, long n)
{
    if (n > 0)
    {
        if (pluto_spill_write(w->h, p, (size_t)n) != 0)
        {
            w->err = 1;
        }
    }
}

/* ── byte reader ─────────────────────────────────────────────────────────── */

typedef struct
{
    const unsigned char *buf; /* whole-file RAM image (bulk-read once) */
    long off;
    long size;
    int err;
} SnapReader;

/* Decoding reads the in-RAM image: the whole file was bulk-read at open
 * time. On device, per-field spill reads would mean open+seek+read+close
 * per field (thousands of times per page) — flash latency stacks up to a
 * multi-second main-thread stall, which trips the Playdate watchdog. The
 * bulk read converts the entire decode into one flash transaction. */
static long r_bytes_raw(SnapReader *r, void *dst, long n)
{
    if (r->err || n <= 0)
    {
        return n > 0 ? 0 : n;
    }
    if (!r->buf || r->off + n > r->size)
    {
        r->err = 1;
        return 0;
    }
    memcpy(dst, r->buf + r->off, (size_t)n);
    r->off += n;
    return n;
}

static long r_i32(SnapReader *r)
{
    unsigned char b[4];
    if (r_bytes_raw(r, b, 4) != 4)
    {
        return 0;
    }
    return (long)((unsigned long)b[0] | ((unsigned long)b[1] << 8) |
                  ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24));
}

/* read a w_str string; returns a fresh arena string ("" never NULL) */
static char *r_str(SnapReader *r, DocArena *arena)
{
    long len = r_i32(r);
    if (r->err || len < 0 || len > 65000)
    {
        r->err = 1;
        return doc_arena_strdup(arena, "");
    }
    char *out = doc_arena_alloc_str(arena, (size_t)len + 1);
    if (!out)
    {
        r->err = 1;
        return doc_arena_strdup(arena, "");
    }
    if (len > 0 && r_bytes_raw(r, out, len) != len)
    {
        r->err = 1;
        out[0] = '\0';
        return out;
    }
    out[len] = '\0';
    return out;
}

/* ── header ──────────────────────────────────────────────────────────────── */

#define SNAP_MAGIC_0 'P'
#define SNAP_MAGIC_1 'L'
#define SNAP_MAGIC_2 'S'
#define SNAP_MAGIC_3 '1'
#define SNAP_VERSION 1
#define SNAP_HEADER_BYTES 12

/* snap_key: 64-bit FNV-1a of url, mixed with mode + version salt. Returns 0
 * only for a NULL/empty url (never stored). */
static unsigned long snap_key(const char *url, int mode)
{
    if (!url || !url[0])
    {
        return 0;
    }
    unsigned long h = 1469598103934665603UL;
    h ^= (unsigned long)SNAP_VERSION * 0x9E3779B97F4A7C15UL;
    h ^= (unsigned long)mode << 24;
    h *= 1099511628211UL;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
    {
        h ^= (unsigned long)*p;
        h *= 1099511628211UL;
    }
    return h ? h : 1UL; /* 0 is the store's "no key" sentinel */
}

/* ── inline serialization (DocInline) ───────────────────────────────────── */

static void w_inline(SnapWriter *w, const DocInline *in)
{
    w_i32(w, in->type);
    w_i32(w, (long)in->flags);
    w_str(w, in->text);
    w_str(w, in->href);
    w_i32(w, in->anchorIndex);
}

static int r_inline(SnapReader *r, DocArena *arena, DocInline *in)
{
    memset(in, 0, sizeof(*in));
    in->type = (int)r_i32(r);
    in->flags = (unsigned)r_i32(r);
    in->text = r_str(r, arena);
    in->href = r_str(r, arena);
    in->anchorIndex = (int)r_i32(r);
    if (in->href[0] == '\0')
    {
        in->href = NULL; /* borrowed-NULL parity with the walker */
    }
    return r->err ? -1 : 0;
}

/* ── cell / row / table serialization ───────────────────────────────────── */

static void w_cell(SnapWriter *w, const DocCell *c)
{
    w_i32(w, c->inlineCount);
    w_i32(w, c->header);
    w_i32(w, c->colspan);
    w_i32(w, c->rowspan);
    w_str(w, c->abbr);
    w_str(w, c->align);
    for (int i = 0; i < c->inlineCount; i++)
    {
        w_inline(w, c->inlines[i]);
    }
}

static int r_cell(SnapReader *r, DocArena *arena, DocCell *c)
{
    memset(c, 0, sizeof(*c));
    c->inlineCount = (int)r_i32(r);
    c->header = (int)r_i32(r);
    c->colspan = (int)r_i32(r);
    c->rowspan = (int)r_i32(r);
    c->abbr = r_str(r, arena);
    c->align = r_str(r, arena);
    if (c->align[0] == '\0')
    {
        c->align = NULL;
    }
    if (c->inlineCount < 0 || c->inlineCount > DOC_MAX_INLINES)
    {
        return -1;
    }
    if (c->inlineCount > 0)
    {
        c->inlineCap = c->inlineCount;
        c->inlines = (DocInline **)PLUTO_MALLOC((size_t)c->inlineCount * sizeof(DocInline *));
        if (!c->inlines)
        {
            return -1;
        }
        for (int i = 0; i < c->inlineCount; i++)
        {
            DocInline *in = (DocInline *)doc_arena_alloc(arena, sizeof(DocInline));
            if (!in || r_inline(r, arena, in) != 0)
            {
                return -1;
            }
            c->inlines[i] = in;
        }
    }
    return r->err ? -1 : 0;
}

static void w_row(SnapWriter *w, const DocRow *row)
{
    w_i32(w, row->cellCount);
    for (int i = 0; i < row->cellCount; i++)
    {
        w_cell(w, row->cells[i]);
    }
}

static int r_row(SnapReader *r, DocArena *arena, DocRow *row)
{
    memset(row, 0, sizeof(*row));
    row->cellCount = (int)r_i32(r);
    if (row->cellCount < 0 || row->cellCount > 64)
    {
        return -1;
    }
    if (row->cellCount > 0)
    {
        row->cellCap = row->cellCount;
        row->cells = (DocCell **)PLUTO_MALLOC((size_t)row->cellCount * sizeof(DocCell *));
        if (!row->cells)
        {
            return -1;
        }
        for (int i = 0; i < row->cellCount; i++)
        {
            DocCell *c = (DocCell *)doc_arena_alloc(arena, sizeof(DocCell));
            if (!c || r_cell(r, arena, c) != 0)
            {
                return -1;
            }
            row->cells[i] = c;
        }
    }
    return r->err ? -1 : 0;
}

static void w_table(SnapWriter *w, const DocTable *t)
{
    w_i32(w, t->rowCount);
    w_i32(w, t->colCount);
    w_i32(w, t->colTotal);
    w_str(w, t->caption);
    w_str(w, t->align);
    w_i32(w, t->border);
    w_i32(w, t->width ? 1 : 0);
    if (t->width)
    {
        w_str(w, t->width);
    }
    for (int i = 0; i < t->colCount; i++)
    {
        const DocCol *c = t->cols[i];
        w_i32(w, c->span);
        w_i32(w, c->width);
        w_i32(w, c->percent);
        w_str(w, c->align);
    }
    for (int i = 0; i < t->rowCount; i++)
    {
        w_row(w, t->rows[i]);
    }
}

static int r_table(SnapReader *r, DocArena *arena, DocTable *t)
{
    memset(t, 0, sizeof(*t));
    t->rowCount = (int)r_i32(r);
    t->colCount = (int)r_i32(r);
    t->colTotal = (int)r_i32(r);
    t->caption = r_str(r, arena);
    t->align = r_str(r, arena);
    if (t->align[0] == '\0')
    {
        t->align = NULL;
    }
    t->border = (int)r_i32(r);
    int hasWidth = (int)r_i32(r);
    t->width = hasWidth ? r_str(r, arena) : NULL;
    if (t->colCount < 0 || t->colCount > 64 ||
        t->colTotal < 0 || t->colTotal > 256 ||
        t->rowCount < 0 || t->rowCount > 512)
    {
        return -1;
    }
    if (t->colCount > 0)
    {
        t->colCap = t->colCount;
        t->cols = (DocCol **)PLUTO_MALLOC((size_t)t->colCount * sizeof(DocCol *));
        if (!t->cols)
        {
            return -1;
        }
        for (int i = 0; i < t->colCount; i++)
        {
            DocCol *c = (DocCol *)doc_arena_alloc(arena, sizeof(DocCol));
            if (!c)
            {
                return -1;
            }
            memset(c, 0, sizeof(*c));
            c->span = (int)r_i32(r);
            c->width = (int)r_i32(r);
            c->percent = (int)r_i32(r);
            c->align = r_str(r, arena);
            if (c->align[0] == '\0')
            {
                c->align = NULL;
            }
            t->cols[i] = c;
        }
    }
    if (t->rowCount > 0)
    {
        t->rowCap = t->rowCount;
        t->rows = (DocRow **)PLUTO_MALLOC((size_t)t->rowCount * sizeof(DocRow *));
        if (!t->rows)
        {
            return -1;
        }
        for (int i = 0; i < t->rowCount; i++)
        {
            DocRow *row = (DocRow *)doc_arena_alloc(arena, sizeof(DocRow));
            if (!row || r_row(r, arena, row) != 0)
            {
                return -1;
            }
            t->rows[i] = row;
        }
    }
    return r->err ? -1 : 0;
}

/* ── option serialization (select fields + datalists) ───────────────────── */

static void w_option(SnapWriter *w, const DocOption *o)
{
    w_str(w, o->text);
    w_str(w, o->value);
    w_i32(w, o->group);
    w_i32(w, o->selected);
    w_i32(w, o->disabled);
}

static int r_option(SnapReader *r, DocArena *arena, DocOption *o)
{
    memset(o, 0, sizeof(*o));
    o->text = r_str(r, arena);
    o->value = r_str(r, arena);
    o->group = (int)r_i32(r);
    o->selected = (int)r_i32(r);
    o->disabled = (int)r_i32(r);
    return r->err ? -1 : 0;
}

/* ── block serialization (the render model) ─────────────────────────────── */

static void w_block(SnapWriter *w, const DocBlock *b)
{
    w_i32(w, b->type);
    w_i32(w, b->level);
    w_str(w, b->align);
    w_i32(w, b->spacingTop);
    w_i32(w, b->spacingBottom);
    w_i32(w, b->indent);
    w_i32(w, b->hasSpacing);
    w_i32(w, b->invert);
    w_i32(w, b->inlineCount);
    for (int i = 0; i < b->inlineCount; i++)
    {
        w_inline(w, b->inlines[i]);
    }
    w_i32(w, b->isOrdered);
    w_i32(w, b->hasNumber);
    w_i32(w, b->number);
    w_i32(w, (long)(unsigned char)b->markerType);
    w_i32(w, b->depth);
    w_i32(w, b->dt);
    w_i32(w, b->dd);
    w_str(w, b->src);
    w_str(w, b->alt);
    w_str(w, b->usemap);
    /* width/height: Lua-parity doubles; integral values print as "N" via
     * %.0f, everything else with full precision. */
    if (b->width == (double)(long long)b->width)
    {
        w_i32(w, 1);
        w_i32(w, (long)b->width);
    }
    else
    {
        w_i32(w, 0);
        w_bytes(w, &b->width, (long)sizeof(double));
    }
    if (b->height == (double)(long long)b->height)
    {
        w_i32(w, 1);
        w_i32(w, (long)b->height);
    }
    else
    {
        w_i32(w, 0);
        w_bytes(w, &b->height, (long)sizeof(double));
    }
    w_str(w, b->caption);
    w_str(w, b->href);
    w_str(w, b->text);
    w_i32(w, b->lineCount);
    for (int i = 0; i < b->lineCount; i++)
    {
        w_str(w, b->lines[i]);
    }
    if (b->table)
    {
        w_i32(w, 1);
        w_table(w, b->table);
    }
    else
    {
        w_i32(w, 0);
    }
    w_str(w, b->name);
    w_str(w, b->value);
    w_str(w, b->placeholder);
    w_str(w, b->label);
    w_i32(w, b->fieldWidth);
    w_i32(w, b->fieldRows);
    w_i32(w, b->disabled);
    w_i32(w, b->readonly);
    w_i32(w, b->required);
    w_i32(w, b->maxlength);
    w_str(w, b->formAction);
    w_str(w, b->formMethod);
    w_i32(w, b->inert);
    w_i32(w, b->radio);
    w_i32(w, b->checked);
    w_str(w, b->inputType);
    w_i32(w, b->optionCount);
    w_i32(w, b->selectedIndex);
    w_i32(w, b->multiple);
    for (int i = 0; i < b->optionCount; i++)
    {
        w_option(w, b->options[i]);
    }
    w_str(w, b->toggleKey);
    w_i32(w, b->toggleOpen);
    w_i32(w, b->colWidth);
    w_str(w, b->ptag);
    w_str(w, b->plabel);
    w_str(w, b->phref);
    /* pwidth/pheight/meter: layout prints them via %g-family formats; the
     * snapshot stores doubles raw (8 bytes each, endian-stable on device). */
    w_bytes(w, &b->pwidth, (long)sizeof(double));
    w_bytes(w, &b->pheight, (long)sizeof(double));
    w_bytes(w, &b->mvalue, (long)sizeof(double));
    w_bytes(w, &b->mmax, (long)sizeof(double));
    w_bytes(w, &b->mmin, (long)sizeof(double));
    w_bytes(w, &b->mlow, (long)sizeof(double));
    w_bytes(w, &b->mhigh, (long)sizeof(double));
    w_bytes(w, &b->moptimum, (long)sizeof(double));
    w_str(w, b->host);
    w_str(w, b->readingTime);
}

static int r_block(SnapReader *r, DocArena *arena, DocBlock *b)
{
    memset(b, 0, sizeof(*b));
    b->type = (int)r_i32(r);
    b->level = (int)r_i32(r);
    b->align = r_str(r, arena);
    if (b->align[0] == '\0')
    {
        b->align = NULL;
    }
    b->spacingTop = (int)r_i32(r);
    b->spacingBottom = (int)r_i32(r);
    b->indent = (int)r_i32(r);
    b->hasSpacing = (int)r_i32(r);
    b->invert = (int)r_i32(r);
    b->inlineCount = (int)r_i32(r);
    if (b->inlineCount < 0 || b->inlineCount > DOC_MAX_INLINES)
    {
        return -1;
    }
    if (b->inlineCount > 0)
    {
        b->inlineCap = b->inlineCount;
        b->inlines = (DocInline **)PLUTO_MALLOC((size_t)b->inlineCount * sizeof(DocInline *));
        if (!b->inlines)
        {
        return -1;
        }
        for (int i = 0; i < b->inlineCount; i++)
        {
            DocInline *in = (DocInline *)doc_arena_alloc(arena, sizeof(DocInline));
            if (!in || r_inline(r, arena, in) != 0)
            {
        return -1;
            }
            b->inlines[i] = in;
        }
    }
    b->isOrdered = (int)r_i32(r);
    b->hasNumber = (int)r_i32(r);
    b->number = (int)r_i32(r);
    {
        long m = r_i32(r);
        b->markerType = (char)(m & 0xFF);
    }
    b->depth = (int)r_i32(r);
    b->dt = (int)r_i32(r);
    b->dd = (int)r_i32(r);
    b->src = r_str(r, arena);
    b->alt = r_str(r, arena);
    b->usemap = r_str(r, arena);
    int integral = (int)r_i32(r);
    if (integral)
    {
        b->width = (double)r_i32(r);
    }
    else
    {
        if (r_bytes_raw(r, &b->width, (long)sizeof(double)) !=
            (long)sizeof(double))
        {
        return -1;
        }
    }
    integral = (int)r_i32(r);
    if (integral)
    {
        b->height = (double)r_i32(r);
    }
    else
    {
        if (r_bytes_raw(r, &b->height, (long)sizeof(double)) !=
            (long)sizeof(double))
        {
        return -1;
        }
    }
    b->caption = r_str(r, arena);
    b->href = r_str(r, arena);
    if (b->href[0] == '\0')
    {
        b->href = NULL;
    }
    b->text = r_str(r, arena);
    b->lineCount = (int)r_i32(r);
    if (b->lineCount < 0 || b->lineCount > 4096)
    {
        return -1;
    }
    if (b->lineCount > 0)
    {
        b->lineCap = b->lineCount;
        b->lines = (char **)PLUTO_MALLOC((size_t)b->lineCount * sizeof(char *));
        if (!b->lines)
        {
        return -1;
        }
        for (int i = 0; i < b->lineCount; i++)
        {
            b->lines[i] = r_str(r, arena);
        }
    }
    int hasTable = (int)r_i32(r);
    if (hasTable)
    {
        b->table = (DocTable *)doc_arena_alloc(arena, sizeof(DocTable));
        if (!b->table || r_table(r, arena, b->table) != 0)
        {
        return -1;
        }
    }
    b->name = r_str(r, arena);
    b->value = r_str(r, arena);
    b->placeholder = r_str(r, arena);
    b->label = r_str(r, arena);
    if (b->name[0] == '\0')
    {
        b->name = NULL;
    }
    if (b->placeholder[0] == '\0')
    {
        b->placeholder = NULL;
    }
    b->fieldWidth = (int)r_i32(r);
    b->fieldRows = (int)r_i32(r);
    b->disabled = (int)r_i32(r);
    b->readonly = (int)r_i32(r);
    b->required = (int)r_i32(r);
    b->maxlength = (int)r_i32(r);
    b->formAction = r_str(r, arena);
    if (b->formAction[0] == '\0')
    {
        b->formAction = NULL;
    }
    b->formMethod = r_str(r, arena);
    if (b->formMethod[0] == '\0')
    {
        b->formMethod = NULL;
    }
    b->inert = (int)r_i32(r);
    b->radio = (int)r_i32(r);
    b->checked = (int)r_i32(r);
    b->inputType = r_str(r, arena);
    if (b->inputType[0] == '\0')
    {
        b->inputType = NULL;
    }
    b->optionCount = (int)r_i32(r);
    b->selectedIndex = (int)r_i32(r);
    b->multiple = (int)r_i32(r);
    if (b->optionCount < 0 || b->optionCount > 4096)
    {
        return -1;
    }
    if (b->optionCount > 0)
    {
        b->optionCap = b->optionCount;
        b->options = (DocOption **)PLUTO_MALLOC((size_t)b->optionCount * sizeof(DocOption *));
        if (!b->options)
        {
        return -1;
        }
        for (int i = 0; i < b->optionCount; i++)
        {
            DocOption *o = (DocOption *)doc_arena_alloc(arena, sizeof(DocOption));
            if (!o || r_option(r, arena, o) != 0)
            {
        return -1;
            }
            b->options[i] = o;
        }
    }
    b->toggleKey = r_str(r, arena);
    if (b->toggleKey[0] == '\0')
    {
        b->toggleKey = NULL;
    }
    b->toggleOpen = (int)r_i32(r);
    b->colWidth = (int)r_i32(r);
    b->ptag = r_str(r, arena);
    b->plabel = r_str(r, arena);
    b->phref = r_str(r, arena);
    if (b->ptag[0] == '\0')
    {
        b->ptag = NULL;
    }
    if (b->plabel[0] == '\0')
    {
        b->plabel = NULL;
    }
    if (b->phref[0] == '\0')
    {
        b->phref = NULL;
    }
    if (r_bytes_raw(r, &b->pwidth, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->pheight, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->mvalue, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->mmax, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->mmin, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->mlow, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->mhigh, (long)sizeof(double)) != (long)sizeof(double) ||
        r_bytes_raw(r, &b->moptimum, (long)sizeof(double)) != (long)sizeof(double))
    {
        return -1;
    }
    b->host = r_str(r, arena);
    if (b->host[0] == '\0')
    {
        b->host = NULL;
    }
    b->readingTime = r_str(r, arena);
    if (b->readingTime[0] == '\0')
    {
        b->readingTime = NULL;
    }
    /* Layout scratch (widthPx/colX/colW/gridCount) is layout-owned: layout
     * builds it fresh per build; snapshot restores it zeroed (memset above). */
    return r->err ? -1 : 0;
}

/* ── link / map / datalist serialization ─────────────────────────────────── */

static void w_link(SnapWriter *w, const DocLink *l)
{
    w_str(w, l->href);
    w_str(w, l->text);
    w_str(w, l->target);
}

static int r_link(SnapReader *r, DocArena *arena, DocLink *l)
{
    memset(l, 0, sizeof(*l));
    l->href = r_str(r, arena);
    l->text = r_str(r, arena);
    l->target = r_str(r, arena);
    if (l->target[0] == '\0')
    {
        l->target = NULL;
    }
    /* srcNode stays NULL: no live DOM in a snapshot (click events fall back
     * to plain navigation; a fresh load re-attaches a live DOM). */
    return r->err ? -1 : 0;
}

static void w_map(SnapWriter *w, const DocMap *m)
{
    w_str(w, m->name);
    w_i32(w, m->areaCount);
    for (int i = 0; i < m->areaCount; i++)
    {
        const DocArea *a = m->areas[i];
        w_str(w, a->shape);
        w_i32(w, a->coordCount);
        for (int j = 0; j < a->coordCount; j++)
        {
            w_i32(w, a->coords[j]);
        }
        w_str(w, a->href);
        w_str(w, a->alt);
    }
}

static int r_map(SnapReader *r, DocArena *arena, DocMap *m)
{
    memset(m, 0, sizeof(*m));
    m->name = r_str(r, arena);
    m->areaCount = (int)r_i32(r);
    if (m->areaCount < 0 || m->areaCount > 256)
    {
        return -1;
    }
    if (m->areaCount > 0)
    {
        m->areaCap = m->areaCount;
        m->areas = (DocArea **)PLUTO_MALLOC((size_t)m->areaCount * sizeof(DocArea *));
        if (!m->areas)
        {
            return -1;
        }
        for (int i = 0; i < m->areaCount; i++)
        {
            DocArea *a = (DocArea *)doc_arena_alloc(arena, sizeof(DocArea));
            if (!a)
            {
                return -1;
            }
            memset(a, 0, sizeof(*a));
            a->shape = r_str(r, arena);
            a->coordCount = (int)r_i32(r);
            if (a->coordCount < 0 || a->coordCount > 64)
            {
                return -1;
            }
            if (a->coordCount > 0)
            {
                a->coords = (int *)PLUTO_MALLOC((size_t)a->coordCount * sizeof(int));
                if (!a->coords)
                {
                    return -1;
                }
                for (int j = 0; j < a->coordCount; j++)
                {
                    a->coords[j] = (int)r_i32(r);
                }
            }
            a->href = r_str(r, arena);
            if (a->href[0] == '\0')
            {
                a->href = NULL;
            }
            a->alt = r_str(r, arena);
            m->areas[i] = a;
        }
    }
    return r->err ? -1 : 0;
}

static void w_datalist(SnapWriter *w, const DocDatalist *d)
{
    w_str(w, d->id);
    w_i32(w, d->optionCount);
    for (int i = 0; i < d->optionCount; i++)
    {
        w_option(w, d->options[i]);
    }
}

static int r_datalist(SnapReader *r, DocArena *arena, DocDatalist *d)
{
    memset(d, 0, sizeof(*d));
    d->id = r_str(r, arena);
    d->optionCount = (int)r_i32(r);
    if (d->optionCount < 0 || d->optionCount > 4096)
    {
        return -1;
    }
    if (d->optionCount > 0)
    {
        d->optionCap = d->optionCount;
        d->options = (DocOption **)PLUTO_MALLOC((size_t)d->optionCount * sizeof(DocOption *));
        if (!d->options)
        {
            return -1;
        }
        for (int i = 0; i < d->optionCount; i++)
        {
            DocOption *o = (DocOption *)doc_arena_alloc(arena, sizeof(DocOption));
            if (!o || r_option(r, arena, o) != 0)
            {
                return -1;
            }
            d->options[i] = o;
        }
    }
    return r->err ? -1 : 0;
}

/* ── save / load / invalidate ────────────────────────────────────────────── */

/* URLs the snapshot cache must never serve: the two live-JS suite pages
 * (every visit must actually execute their scripts — cached output would
 * silently break the JS autotests), and anything empty. about:home and
 * about:blank never reach here anyway (early returns / empty output). */
static int snap_skip_url(const char *url)
{
    if (!url || !url[0])
    {
        return 1;
    }
    if (strcmp(url, "about:javascript") == 0 ||
        strcmp(url, "about:jsext") == 0)
    {
        return 1;
    }
    return 0;
}

long pluto_snap_save(const DocParseResult *doc, const char *url, int mode,
                     unsigned long epoch)
{
    if (!doc || doc->parseError || snap_skip_url(url))
    {
        return 0;
    }
    unsigned long key = snap_key(url, mode);
    if (!key)
    {
        return 0;
    }
    SnapWriter w;
    memset(&w, 0, sizeof(w));
    w.h = pluto_spill_store_open_create(key);
    if (w.h == PLUTO_SPILL_INVALID)
    {
        return 0;
    }
    /* header: magic(4) version(1) mode(1) flags(1) reserved(1) epoch(4) */
    unsigned char hdr[SNAP_HEADER_BYTES];
    hdr[0] = SNAP_MAGIC_0;
    hdr[1] = SNAP_MAGIC_1;
    hdr[2] = SNAP_MAGIC_2;
    hdr[3] = SNAP_MAGIC_3;
    hdr[4] = SNAP_VERSION;
    hdr[5] = (unsigned char)(mode & 0xFF);
    hdr[6] = 0; /* flags */
    hdr[7] = 0; /* reserved */
    hdr[8] = (unsigned char)(epoch & 0xFF);
    hdr[9] = (unsigned char)((epoch >> 8) & 0xFF);
    hdr[10] = (unsigned char)((epoch >> 16) & 0xFF);
    hdr[11] = (unsigned char)((epoch >> 24) & 0xFF);
    if (pluto_spill_write(w.h, hdr, SNAP_HEADER_BYTES) != 0)
    {
        w.err = 1;
    }
    w_i32(&w, doc->blockCount);
    w_i32(&w, doc->linkCount);
    w_i32(&w, doc->mapCount);
    w_i32(&w, doc->datalistCount);
    {
        /* fixed-size raw fields: title[256] + baseUrl[512], zero-padded */
        unsigned char tbuf[256];
        memset(tbuf, 0, sizeof(tbuf));
        snprintf((char *)tbuf, sizeof(tbuf), "%s", doc->title);
        if (pluto_spill_write(w.h, tbuf, (long)sizeof(tbuf)) != 0)
        {
            w.err = 1;
        }
        unsigned char bbuf[512];
        memset(bbuf, 0, sizeof(bbuf));
        snprintf((char *)bbuf, sizeof(bbuf), "%s", doc->baseUrl);
        if (pluto_spill_write(w.h, bbuf, (long)sizeof(bbuf)) != 0)
        {
            w.err = 1;
        }
    }
    /* metaRefresh */
    w_i32(&w, doc->metaRefresh.present);
    {
        unsigned char mb[4];
        float delay = doc->metaRefresh.delay;
        memcpy(mb, &delay, 4);
        if (pluto_spill_write(w.h, mb, 4) != 0)
        {
            w.err = 1;
        }
        w_str(&w, doc->metaRefresh.url);
    }
    /* suppressNoscript + js telemetry (diagnostic parity on restore) */
    w_i32(&w, doc->suppressNoscript);
    w_i32(&w, doc->jsRan);
    w_i32(&w, doc->jsErrors);
    w_str(&w, doc->jsLastError);

    for (int i = 0; i < doc->blockCount && !w.err; i++)
    {
        w_block(&w, doc->blocks[i]);
    }
    for (int i = 0; i < doc->linkCount && !w.err; i++)
    {
        w_link(&w, doc->links[i]);
    }
    for (int i = 0; i < doc->mapCount && !w.err; i++)
    {
        w_map(&w, doc->maps[i]);
    }
    for (int i = 0; i < doc->datalistCount && !w.err; i++)
    {
        w_datalist(&w, doc->datalists[i]);
    }
    if (w.err)
    {
        /* Half-written entry: remove it (a create-recreate on next save
         * would truncate anyway, but a BROKEN entry must never load). */
        pluto_spill_discard(w.h);
        logger_log("[snap] save failed (write error) key=%08lx", key);
        return 0;
    }
    long total = pluto_spill_finish(w.h);
    if (total <= 0)
    {
        pluto_spill_discard(w.h);
        logger_log("[snap] save failed (finish) key=%08lx", key);
        return 0;
    }
    logger_log("[snap] saved url=%s blocks=%d links=%d bytes=%ld",
               url, doc->blockCount, doc->linkCount, total);
    return total;
}

DocParseResult *pluto_snap_load(const char *url, int mode, unsigned long nowEpoch,
                                unsigned long ttlSeconds)
{
    unsigned char *image = NULL;
    if (!url || !url[0] || snap_skip_url(url))
    {
        return NULL;
    }
    unsigned long key = snap_key(url, mode);
    if (!key || !pluto_spill_store_find(key))
    {
        return NULL;
    }
    SpillFile h = pluto_spill_store_open_read(key);
    if (h == PLUTO_SPILL_INVALID)
    {
        return NULL;
    }
    long fsz = pluto_spill_size(h);
    if (fsz < SNAP_HEADER_BYTES)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }
    /* SW6 device fix: bulk-read the whole file once, decode from RAM.
     * Per-field spill reads on device cost open+seek+read+close each —
     * thousands of them for a real page stalled the main thread long
     * enough to trip the watchdog (see crashlog 2026-09-21). */
    image = (unsigned char *)PLUTO_MALLOC((size_t)fsz);
    if (!image)
    {
        pluto_spill_store_close(h);
        return NULL;
    }
    long got = pluto_spill_read(h, 0, image, (size_t)fsz);
    if (got != fsz)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }
    SnapReader r;
    memset(&r, 0, sizeof(r));
    r.buf = image;
    r.size = fsz;
    unsigned char hdr[SNAP_HEADER_BYTES];
    if (r_bytes_raw(&r, hdr, SNAP_HEADER_BYTES) != SNAP_HEADER_BYTES ||
        hdr[0] != SNAP_MAGIC_0 || hdr[1] != SNAP_MAGIC_1 ||
        hdr[2] != SNAP_MAGIC_2 || hdr[3] != SNAP_MAGIC_3)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL; /* not a snapshot (foreign family / corrupt) */
    }
    if (hdr[4] != SNAP_VERSION)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL; /* format version bump — fall through to network */
    }
    int snapMode = (int)hdr[5];
    unsigned long epoch = (unsigned long)hdr[8] |
                          ((unsigned long)hdr[9] << 8) |
                          ((unsigned long)hdr[10] << 16) |
                          ((unsigned long)hdr[11] << 24);
    if (snapMode != mode)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }
    if (ttlSeconds && nowEpoch && epoch &&
        nowEpoch > epoch && (nowEpoch - epoch) > ttlSeconds)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        logger_log("[snap] expired url=%s age=%lus", url, nowEpoch - epoch);
        return NULL;
    }

    long blockCount = r_i32(&r);
    long linkCount = r_i32(&r);
    long mapCount = r_i32(&r);
    long datalistCount = r_i32(&r);
    if (r.err || blockCount < 0 || blockCount > DOC_MAX_BLOCKS ||
        linkCount < 0 || linkCount > 4096 ||
        mapCount < 0 || mapCount > 128 ||
        datalistCount < 0 || datalistCount > 128)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }

    DocParseResult *doc = (DocParseResult *)PLUTO_MALLOC(sizeof(DocParseResult));
    if (!doc)
    {
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->_arena = PLUTO_MALLOC(sizeof(DocArena));
    if (!doc->_arena)
    {
        PLUTO_FREE(doc);
        pluto_spill_store_close(h);
        PLUTO_FREE(image);
        return NULL;
    }
    DocArena *arena = (DocArena *)doc->_arena;
    memset(arena, 0, sizeof(*arena));

    unsigned char tbuf[256];
    if (r_bytes_raw(&r, tbuf, (long)sizeof(tbuf)) != (long)sizeof(tbuf))
    {
    goto fail;
    }
    memcpy(doc->title, tbuf, sizeof(doc->title));
    doc->title[sizeof(doc->title) - 1] = '\0';
    {
        unsigned char bbuf[512];
        if (r_bytes_raw(&r, bbuf, (long)sizeof(bbuf)) != (long)sizeof(bbuf))
        {
    goto fail;
        }
        memcpy(doc->baseUrl, bbuf, sizeof(doc->baseUrl));
        doc->baseUrl[sizeof(doc->baseUrl) - 1] = '\0';
    }    doc->metaRefresh.present = (int)r_i32(&r);
    {
        unsigned char mb[4];
        if (r_bytes_raw(&r, mb, 4) != 4)
        {
    goto fail;
        }
        float delay;
        memcpy(&delay, mb, 4);
        doc->metaRefresh.delay = delay;
        char *murl = r_str(&r, arena);
        snprintf(doc->metaRefresh.url, sizeof(doc->metaRefresh.url), "%s", murl);
    }
    doc->suppressNoscript = (int)r_i32(&r);
    doc->jsRan = (int)r_i32(&r);
    doc->jsErrors = (int)r_i32(&r);
    {
        char *jel = r_str(&r, arena);
        snprintf(doc->jsLastError, sizeof(doc->jsLastError), "%s", jel);
    }
    if (r.err)
    {
    goto fail;
    }

    if (blockCount > 0)
    {
        doc->blockCap = (int)blockCount;
        doc->blocks = (DocBlock **)PLUTO_MALLOC((size_t)blockCount * sizeof(DocBlock *));
        if (!doc->blocks)
        {
    goto fail;
        }
        for (long i = 0; i < blockCount; i++)
        {
            DocBlock *b = (DocBlock *)doc_arena_alloc(arena, sizeof(DocBlock));
            if (!b || r_block(&r, arena, b) != 0)
            {
                goto fail;
            }
            doc->blocks[i] = b;
        }
        doc->blockCount = (int)blockCount;
    }
    if (linkCount > 0)
    {
        doc->linkCap = (int)linkCount;
        doc->links = (DocLink **)PLUTO_MALLOC((size_t)linkCount * sizeof(DocLink *));
        if (!doc->links)
        {
    goto fail;
        }
        for (long i = 0; i < linkCount; i++)
        {
            DocLink *l = (DocLink *)doc_arena_alloc(arena, sizeof(DocLink));
            if (!l || r_link(&r, arena, l) != 0)
            {
    goto fail;
            }
            doc->links[i] = l;
        }
        doc->linkCount = (int)linkCount;
    }
    if (mapCount > 0)
    {
        doc->mapCap = (int)mapCount;
        doc->maps = (DocMap **)PLUTO_MALLOC((size_t)mapCount * sizeof(DocMap *));
        if (!doc->maps)
        {
    goto fail;
        }
        for (long i = 0; i < mapCount; i++)
        {
            DocMap *m = (DocMap *)doc_arena_alloc(arena, sizeof(DocMap));
            if (!m || r_map(&r, arena, m) != 0)
            {
    goto fail;
            }
            doc->maps[i] = m;
        }
        doc->mapCount = (int)mapCount;
    }
    if (datalistCount > 0)
    {
        doc->datalistCap = (int)datalistCount;
        doc->datalists = (DocDatalist **)PLUTO_MALLOC((size_t)datalistCount * sizeof(DocDatalist *));
        if (!doc->datalists)
        {
    goto fail;
        }
        for (long i = 0; i < datalistCount; i++)
        {
            DocDatalist *d = (DocDatalist *)doc_arena_alloc(arena, sizeof(DocDatalist));
            if (!d || r_datalist(&r, arena, d) != 0)
            {
    goto fail;
            }
            doc->datalists[i] = d;
        }
        doc->datalistCount = (int)datalistCount;
    }
    if (r.err || r.off != r.size)
    {
        /* trailing garbage / short file = corrupt: refuse rather than
         * render a partially-restored page */
    goto fail;
    }
    pluto_spill_store_close(h);
    PLUTO_FREE(image);
    logger_log("[snap] hit url=%s blocks=%ld links=%ld", url, blockCount, linkCount);
    return doc;

fail:
    logger_log("[snap] load failed (corrupt entry)");
    pluto_spill_store_close(h);
    PLUTO_FREE(image);
    if (doc)
    {
        if (doc->blocks)
        {
            PLUTO_FREE(doc->blocks);
        }
        if (doc->links)
        {
            PLUTO_FREE(doc->links);
        }
        if (doc->maps)
        {
            PLUTO_FREE(doc->maps);
        }
        if (doc->datalists)
        {
            PLUTO_FREE(doc->datalists);
        }
        doc_arena_free_all(arena);
        PLUTO_FREE(doc->_arena);
        PLUTO_FREE(doc);
    }
    return NULL;
}

void pluto_snap_invalidate(const char *url, int mode)
{
    unsigned long key = snap_key(url, mode);
    if (key && pluto_spill_store_find(key))
    {
        int idx = pluto_spill_store_index_of(key);
        if (idx >= 0)
        {
            pluto_spill_store_delete_at(idx);
            logger_log("[snap] invalidated url=%s", url);
        }
    }
}

void pluto_snap_invalidate_all(void)
{
    /* The spill-level sweep (not a count loop): the store table is lazily
     * scanned, so a pre-scan count is 0 and entries on disk would survive.
     * pluto_spill_store_invalidate_all force-scans first, then wipes. */
    pluto_spill_store_invalidate_all();
    logger_log("[snap] invalidated all snapshots");
}

int pluto_snap_lru_sweep(int maxEntries)
{
    int removed = 0;
    /* The store table enumerates oldest-registration-first (store_scan
     * claims files in directory order, open_create appends); deleting from
     * the front evicts the oldest entries. A registration-order LRU is a
     * good-enough approximation here — a proper access-time LRU needs a
     * metadata file, which SW6 skips to stay bounded. */
    while (pluto_spill_store_count() > maxEntries)
    {
        if (pluto_spill_store_delete_at(0) != 0)
        {
            break;
        }
        removed++;
    }
    if (removed)
    {
        logger_log("[snap] lru sweep: removed %d", removed);
    }
    return removed;
}
