/* Host-side SVG renderer test: runs the REAL svg.c against a fake Playdate
 * graphics API that implements a small 1-bit software rasterizer, renders an
 * SVG file, and writes the decoded bitmap as a 24-bit BMP so the output can
 * be inspected visually (sips -s format png out.bmp --out out.png).
 *
 * Build (macOS host):
 *   cc -o /tmp/svgtest tests/svg_fill_host_test.c Source/render/decoders/svg.c \
 *     Source/util/strbuf.c -I. -ISource -I$PLAYDATE_SDK_PATH/C_API
 * Run:
 *   /tmp/svgtest test_pattern.svg /tmp/svg_out.bmp [maxW] [maxH]
 *
 * Fake pattern semantics (to exercise svg.c's runtime self-calibration):
 *   PLUTO_FAKE_ONE_SIDED=1 → pattern 0-bits are no-ops (no erase)
 *   PLUTO_FAKE_WHITE_BITS=1 → pattern data bit 1 paints WHITE (not black)
 * Default: opaque, data bit 1 = black pixel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include "pd_api.h"

/* ---- logger/pluto_mem shims (decoders link against these) ---- */
void logger_stack_touch(void) {}
unsigned logger_stack_peak(void) { return 0; }
unsigned logger_stack_depth(void) { return 0; }
void logger_error_at(const char *file, int line, const char *fmt, ...)
{
    (void)file; (void)line; (void)fmt;
}
void logger_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[svg-log] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
void *pluto_mem_realloc(void *p, size_t n) { return realloc(p, n); }
void pluto_free(void *p) { free(p); }
void *pluto_mem_sdk_realloc(void *p, size_t n) { return realloc(p, n); }

#include "render/decoders/svg.h"

/* ---- Fake LCDBitmap: plain 1-bit plane shaped like the SDK's ---- */
typedef struct FakeBitmap
{
    int w, h, rb;
    uint8_t *data; /* 1 = white, 0 = black (SDK plane convention) */
} FakeBitmap;

static FakeBitmap *g_ctx[8];
static int g_ctxN = 0;

#define BIT_WHITE 1

static int fake_oneseited = 0;
static int fake_whitebits = 0;

static void fake_init_env(void)
{
    fake_oneseited = getenv("PLUTO_FAKE_ONE_SIDED") ? 1 : 0;
    fake_whitebits = getenv("PLUTO_FAKE_WHITE_BITS") ? 1 : 0;
}

static inline void fake_pset(FakeBitmap *fb, int x, int y, int white)
{
    if (!fb || x < 0 || y < 0 || x >= fb->w || y >= fb->h)
        return;
    uint8_t *b = &fb->data[(size_t)y * fb->rb + (x >> 3)];
    if (white)
        *b |= (uint8_t)(0x80 >> (x & 7));
    else
        *b &= (uint8_t)~(0x80 >> (x & 7));
}

static int fake_paint_pixel(FakeBitmap *fb, int x, int y, LCDColor color)
{
    if ((uintptr_t)color <= (uintptr_t)3)
    {
        /* solid: 0 = black, 1 = white, 2 = clear (no-op), 3 = XOR (no-op) */
        if (color == (LCDColor)kColorClear)
            return 0;
        fake_pset(fb, x, y, color == (LCDColor)kColorWhite);
        return 1;
    }
    const uint8_t *pat = (const uint8_t *)color;
    int maskBit = (pat[8 + (y & 7)] >> (7 - (x & 7))) & 1;
    if (!maskBit)
        return 0;
    int dataBit = (pat[y & 7] >> (7 - (x & 7))) & 1;
    if (fake_whitebits)
        dataBit = !dataBit;
    if (fake_oneseited && !dataBit)
        return 0; /* 0-bits no-op */
    fake_pset(fb, x, y, !dataBit); /* data 1 → black pixel (plane 0) */
    return 1;
}

static LCDBitmap *fake_newBitmap(int width, int height, LCDColor bg)
{
    FakeBitmap *fb = (FakeBitmap *)calloc(1, sizeof(FakeBitmap));
    if (!fb)
        return NULL;
    fb->w = width;
    fb->h = height;
    fb->rb = (width + 7) / 8;
    fb->data = (uint8_t *)calloc((size_t)fb->rb * height, 1);
    if (!fb->data)
    {
        free(fb);
        return NULL;
    }
    if (bg == (LCDColor)kColorWhite)
        memset(fb->data, 0xff, (size_t)fb->rb * height);
    return (LCDBitmap *)fb;
}

static void fake_getBitmapData(LCDBitmap *bitmap, int *w, int *h, int *rb,
                               uint8_t **mask, uint8_t **data)
{
    FakeBitmap *fb = (FakeBitmap *)bitmap;
    if (w) *w = fb->w;
    if (h) *h = fb->h;
    if (rb) *rb = fb->rb;
    if (mask) *mask = NULL;
    if (data) *data = fb->data;
}

static void fake_freeBitmap(LCDBitmap *bitmap)
{
    FakeBitmap *fb = (FakeBitmap *)bitmap;
    if (fb)
    {
        free(fb->data);
        free(fb);
    }
}

static void fake_pushContext(LCDBitmap *b) { g_ctx[g_ctxN++] = (FakeBitmap *)b; }
static void fake_popContext(void) { if (g_ctxN > 0) g_ctxN--; }

static void fake_fillRect(int x, int y, int w, int h, LCDColor color)
{
    FakeBitmap *fb = g_ctx[g_ctxN - 1];
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fake_paint_pixel(fb, xx, yy, color);
}

static void fake_clearBitmap(LCDBitmap *bitmap, LCDColor bg)
{
    FakeBitmap *fb = (FakeBitmap *)bitmap;
    memset(fb->data, bg == (LCDColor)kColorWhite ? 0xff : 0x00,
           (size_t)fb->rb * fb->h);
}

static void fake_drawLine(int x1, int y1, int x2, int y2, int width, LCDColor color)
{
    FakeBitmap *fb = g_ctx[g_ctxN - 1];
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    int x = x1, y = y1;
    for (;;)
    {
        for (int t = 0; t < width; t++)
        {
            if (dx >= -dy)
                fake_paint_pixel(fb, x, y + t, color);
            else
                fake_paint_pixel(fb, x + t, y, color);
        }
        if (x == x2 && y == y2)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

static void fake_drawRect(int x, int y, int width, int height, LCDColor color)
{
    fake_drawLine(x, y, x + width - 1, y, 1, color);
    fake_drawLine(x, y + height - 1, x + width - 1, y + height - 1, 1, color);
    fake_drawLine(x, y, x, y + height - 1, 1, color);
    fake_drawLine(x + width - 1, y, x + width - 1, y + height - 1, 1, color);
}

static void fake_drawRoundRect(int x, int y, int width, int height, int radius,
                               int lineWidth, LCDColor color)
{
    (void)radius; /* test approximation: square corners */
    fake_drawRect(x, y, width, height, color);
}

static void fake_drawEllipse(int x, int y, int width, int height, int lineWidth,
                             float startAngle, float endAngle, LCDColor color)
{
    FakeBitmap *fb = g_ctx[g_ctxN - 1];
    (void)startAngle; (void)endAngle; /* full ellipse only (all uses here) */
    (void)lineWidth;                  /* 1px ring */
    if (width <= 0 || height <= 0)
        return;
    double rx = width / 2.0, ry = height / 2.0;
    double cx = x + rx, cy = y + ry;
    double edge = 1.5 / (rx > ry ? rx : ry);
    for (int yy = y - 1; yy <= y + height + 1; yy++)
        for (int xx = x - 1; xx <= x + width + 1; xx++)
        {
            double dxx = (xx - cx) / rx, dyy = (yy - cy) / ry;
            double d = sqrt(dxx * dxx + dyy * dyy);
            if (d <= 1.0 && d >= 1.0 - edge)
                fake_paint_pixel(fb, xx, yy, color);
        }
}

static void fake_fillEllipse(int x, int y, int width, int height,
                             float startAngle, float endAngle, LCDColor color)
{
    FakeBitmap *fb = g_ctx[g_ctxN - 1];
    (void)startAngle; (void)endAngle;
    if (width <= 0 || height <= 0)
        return;
    double rx = width / 2.0, ry = height / 2.0;
    double cx = x + rx, cy = y + ry;
    for (int yy = y; yy < y + height; yy++)
        for (int xx = x; xx < x + width; xx++)
        {
            double dxx = (xx - cx) / rx, dyy = (yy - cy) / ry;
            if (dxx * dxx + dyy * dyy <= 1.0)
                fake_paint_pixel(fb, xx, yy, color);
        }
}

static void fake_fillPolygon(int nPoints, int *coords, LCDColor color,
                             LCDPolygonFillRule fillrule)
{
    FakeBitmap *fb = g_ctx[g_ctxN - 1];
    if (nPoints < 3 || !coords)
        return;
    int minX = fb->w, minY = fb->h, maxX = 0, maxY = 0;
    for (int i = 0; i < nPoints; i++)
    {
        if (coords[2 * i] < minX) minX = coords[2 * i];
        if (coords[2 * i] > maxX) maxX = coords[2 * i];
        if (coords[2 * i + 1] < minY) minY = coords[2 * i + 1];
        if (coords[2 * i + 1] > maxY) maxY = coords[2 * i + 1];
    }
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > fb->w - 1) maxX = fb->w - 1;
    if (maxY > fb->h - 1) maxY = fb->h - 1;
    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++)
        {
            int inside = 0;
            if (fillrule == kPolygonFillEvenOdd)
            {
                int crossings = 0;
                for (int i = 0, j = nPoints - 1; i < nPoints; j = i++)
                {
                    int xi = coords[2 * i], yi = coords[2 * i + 1];
                    int xj = coords[2 * j], yj = coords[2 * j + 1];
                    if ((yi > y) != (yj > y))
                    {
                        double t = (double)(y - yi) / (double)(yj - yi);
                        double xx = xi + t * (double)(xj - xi);
                        if (xx > x)
                            crossings++;
                    }
                }
                inside = crossings & 1;
            }
            else
            {
                /* nonzero winding */
                int wind = 0;
                for (int i = 0, j = nPoints - 1; i < nPoints; j = i++)
                {
                    int xi = coords[2 * i], yi = coords[2 * i + 1];
                    int xj = coords[2 * j], yj = coords[2 * j + 1];
                    if ((yi > y) != (yj > y))
                    {
                        double t = (double)(y - yi) / (double)(yj - yi);
                        double xx = xi + t * (double)(xj - xi);
                        if (xx > x)
                            wind += (yj > yi) ? 1 : -1;
                    }
                }
                inside = wind != 0;
            }
            if (inside)
                fake_paint_pixel(fb, x, y, color);
        }
}

static struct playdate_graphics g_fakeGfx;
static struct playdate_sys g_fakeSys;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;

PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        fake_init_env();
        memset(&g_fakeGfx, 0, sizeof(g_fakeGfx));
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeGfx.newBitmap = fake_newBitmap;
        g_fakeGfx.freeBitmap = fake_freeBitmap;
        g_fakeGfx.getBitmapData = fake_getBitmapData;
        g_fakeGfx.clearBitmap = fake_clearBitmap;
        g_fakeGfx.pushContext = fake_pushContext;
        g_fakeGfx.popContext = fake_popContext;
        g_fakeGfx.fillRect = fake_fillRect;
        g_fakeGfx.drawRect = fake_drawRect;
        g_fakeGfx.drawRoundRect = fake_drawRoundRect;
        g_fakeGfx.drawLine = fake_drawLine;
        g_fakeGfx.drawEllipse = fake_drawEllipse;
        g_fakeGfx.fillEllipse = fake_fillEllipse;
        g_fakeGfx.fillPolygon = fake_fillPolygon;
        g_fakeApi.graphics = &g_fakeGfx;
        g_fakeApi.system = &g_fakeSys;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}

/* ---- BMP writer (24-bit, upscaled for visibility) ---- */
static int write_bmp(const char *path, FakeBitmap *fb, int up)
{
    if (up < 1)
        up = 1;
    int w = fb->w * up, h = fb->h * up;
    int rowBytes = (w * 3 + 3) & ~3;
    int imgSize = rowBytes * h;
    int fileSize = 54 + imgSize;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)&hdr[2] = (uint32_t)fileSize;
    *(uint32_t *)&hdr[10] = 54;
    *(uint32_t *)&hdr[14] = 40;
    *(int32_t *)&hdr[18] = w;
    *(int32_t *)&hdr[22] = h;
    *(uint16_t *)&hdr[26] = 1;
    *(uint16_t *)&hdr[28] = 24;
    *(uint32_t *)&hdr[34] = (uint32_t)imgSize;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t *)calloc((size_t)rowBytes, 1);
    for (int yy = h - 1; yy >= 0; yy--)
    {
        int sy = yy / up;
        for (int xx = 0; xx < w; xx++)
        {
            int sx = xx / up;
            int white = fb->data[(size_t)sy * fb->rb + (sx >> 3)] & (0x80 >> (sx & 7));
            uint8_t v = white ? 255 : 0;
            row[xx * 3] = v;
            row[xx * 3 + 1] = v;
            row[xx * 3 + 2] = v;
        }
        fwrite(row, 1, (size_t)rowBytes, f);
    }
    free(row);
    fclose(f);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s file.svg out.bmp [maxW] [maxH]\n", argv[0]);
        return 2;
    }
    int maxW = argc > 3 ? atoi(argv[3]) : 360;
    int maxH = argc > 4 ? atoi(argv[4]) : 200;
    size_t len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    LCDBitmap *img = svg_decode((const char *)data, maxW, maxH);
    if (!img)
    {
        fprintf(stderr, "svg_decode returned NULL\n");
        return 1;
    }
    int w = 0, h = 0, rb = 0;
    uint8_t *mask = NULL, *bits = NULL;
    fake_getBitmapData(img, &w, &h, &rb, &mask, &bits);
    int blacks = 0, total = w * h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (!(bits[(size_t)y * rb + (x >> 3)] & (0x80 >> (x & 7))))
                blacks++;
    fprintf(stderr, "decoded %dx%d, black coverage %.1f%% (%d/%d)\n",
            w, h, 100.0 * blacks / total, blacks, total);
    write_bmp(argv[2], (FakeBitmap *)img, 2);
    fprintf(stderr, "wrote %s\n", argv[2]);
    return 0;
}
