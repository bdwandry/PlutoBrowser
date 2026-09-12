/*
 * PlutoBrowser — webp_container.c
 * Port of Source/render/decoders/webp.lua lines 2737-3052 (P29):
 *   parseWebP, readLE24, parseWebPAnimation, blendPixelNonPremult,
 *   WebPDecoder.decodeAnimation, composite, WebPDecoder.decode,
 *   WebPDecoder._testDecodeRaw.
 *
 * Also defines the public entry points (webp_decode / webp_decode_raw /
 * webp_decode_animation) used by image_decoder and tests.
 */
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"
#include "render/decoders/webp.h"
#include "render/decoders/webp-internal.h"
#include "render/decoders/scale.h"
#include "render/decoders/dither.h"

extern PlaydateAPI *pluto_pd(void);
#define PLUTO_MALLOC(n) pluto_pd()->system->realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_pd()->system->realloc((p), 0)

/* ── parseWebP (webp.lua 2737-2763) ─────────────────────────────────────── */

static uint32_t rd32(const uint8_t *d, size_t pos)
{
    return (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) |
           ((uint32_t)d[pos + 2] << 16) | ((uint32_t)d[pos + 3] << 24);
}

int webp_parse_container(const uint8_t *data, size_t len,
                         const char **cid,
                         const uint8_t **payload, size_t *payloadLen,
                         const uint8_t **alphaData, size_t *alphaLen)
{
    if (len < 20) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0) return 0;
    size_t pos = 12; /* Lua pos=13 is 1-based -> 0-based 12 */
    const uint8_t *vp8Payload = NULL;
    size_t vp8Len = 0;
    int haveVp8 = 0;
    int kind = 0; /* 0 = none; 1 = VP8L; 2 = VP8 */
    while (pos + 8 <= len)
    {
        const uint8_t *c = data + pos;
        uint32_t size = rd32(data, pos + 4);
        if (memcmp(c, "ALPH", 4) == 0)
        {
            if (alphaData && pos + 8 + (size_t)size <= len)
            {
                *alphaData = data + pos + 8;
                *alphaLen = size;
            }
        }
        else if (memcmp(c, "VP8L", 4) == 0)
        {
            kind = 1;
            if (cid) *cid = "VP8L";
            if (pos + 8 + (size_t)size <= len)
            {
                vp8Payload = data + pos + 8;
                vp8Len = size;
            }
            else
            {
                vp8Payload = data + pos + 8;
                vp8Len = len - (pos + 8);
            }
            haveVp8 = 1;
            break; /* Lua returns immediately */
        }
        else if (memcmp(c, "VP8 ", 4) == 0)
        {
            kind = 2;
            if (cid) *cid = "VP8 ";
            if (pos + 8 + (size_t)size <= len)
            {
                vp8Payload = data + pos + 8;
                vp8Len = size;
            }
            else
            {
                vp8Payload = data + pos + 8;
                vp8Len = len - (pos + 8);
            }
            haveVp8 = 1;
        }
        pos += 8 + size + (size & 1);
    }
    if (!haveVp8) return 0;
    if (payload) *payload = vp8Payload;
    if (payloadLen) *payloadLen = vp8Len;
    return kind;
}

static uint32_t read_le24(const uint8_t *d, size_t pos)
{
    return (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) | ((uint32_t)d[pos + 2] << 16);
}

/* ── parseWebPAnimation (webp.lua 2773-2840) ────────────────────────────── */

int webp_parse_animation(const uint8_t *data, size_t len, WebPAnim *out)
{
    if (len < 20) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0) return 0;
    size_t pos = 12; /* Lua pos=13 is 1-based -> 0-based 12 */
    int canvasW = 0, canvasH = 0;
    uint32_t bgcolor = 0;
    int loopCount = 0;
    int isExtended = 0;
    int seenAnim = 0;
    WebPAnimFrame *frames = NULL;
    int numFrames = 0, capFrames = 0;
    while (pos + 8 <= len)
    {
        const uint8_t *c = data + pos;
        uint32_t size = rd32(data, pos + 4);
        if (memcmp(c, "VP8X", 4) == 0)
        {
            isExtended = 1;
            size_t p = pos + 8;
            uint8_t flags = (p < len) ? data[p] : 0;
            canvasW = (int)read_le24(data, p + 4) + 1;
            canvasH = (int)read_le24(data, p + 7) + 1;
            if (canvasW < 1 || canvasH < 1 || canvasW * canvasH > WEBP_MAX_PIXELS)
            {
                PLUTO_FREE(frames);
                return 0;
            }
            if ((flags & 0x02) == 0)
            {
                PLUTO_FREE(frames);
                return 0;
            }
        }
        else if (memcmp(c, "ANIM", 4) == 0)
        {
            seenAnim = 1;
            size_t p = pos + 8;
            uint32_t a = (p < len) ? data[p] : 0;
            uint32_t r = (p + 1 < len) ? data[p + 1] : 0;
            uint32_t g = (p + 2 < len) ? data[p + 2] : 0;
            uint32_t b = (p + 3 < len) ? data[p + 3] : 0;
            bgcolor = (a << 24) | (r << 16) | (g << 8) | b;
            uint32_t l1 = (p + 4 < len) ? data[p + 4] : 0;
            uint32_t l2 = (p + 5 < len) ? data[p + 5] : 0;
            loopCount = (int)(l1 | (l2 << 8));
        }
        else if (memcmp(c, "ANMF", 4) == 0)
        {
            if (!seenAnim)
            {
                PLUTO_FREE(frames);
                return 0;
            }
            const uint8_t *pl = data + pos + 8;
            size_t plLen = (pos + 8 + (size_t)size <= len) ? size : (len - (pos + 8));
            if (plLen < 16)
            {
                PLUTO_FREE(frames);
                return 0;
            }
            WebPAnimFrame f;
            memset(&f, 0, sizeof(f));
            f.x = (int)(2 * read_le24(pl, 1));
            f.y = (int)(2 * read_le24(pl, 4));
            f.w = (int)read_le24(pl, 7) + 1;
            f.h = (int)read_le24(pl, 10) + 1;
            f.duration = (int)read_le24(pl, 13);
            uint8_t bits = pl[16];
            f.dispose = ((bits & 1) == 1);
            f.noBlend = (((bits >> 1) & 1) == 1);
            size_t ipos = 17;
            while (ipos + 8 <= plLen)
            {
                const uint8_t *ic = pl + ipos;
                uint32_t isize = rd32(pl, ipos + 4);
                if (memcmp(ic, "ALPH", 4) == 0)
                {
                    f.alpha = pl + ipos + 8;
                    f.alphaLen = isize;
                }
                else if (memcmp(ic, "VP8L", 4) == 0)
                {
                    f.cid = "VP8L";
                    f.payload = pl + ipos + 8;
                    f.payloadLen = isize;
                }
                else if (memcmp(ic, "VP8 ", 4) == 0)
                {
                    f.cid = "VP8 ";
                    f.payload = pl + ipos + 8;
                    f.payloadLen = isize;
                }
                ipos += 8 + isize + (isize & 1);
            }
            if (!f.cid || !f.payload)
            {
                PLUTO_FREE(frames);
                return 0;
            }
            if (f.x + f.w > canvasW || f.y + f.h > canvasH)
            {
                PLUTO_FREE(frames);
                return 0;
            }
            if (numFrames == capFrames)
            {
                capFrames = capFrames ? capFrames * 2 : 8;
                WebPAnimFrame *nf = (WebPAnimFrame *)PLUTO_MALLOC(sizeof(WebPAnimFrame) * (size_t)capFrames);
                if (!nf)
                {
                    PLUTO_FREE(frames);
                    return 0;
                }
                if (frames)
                {
                    memcpy(nf, frames, sizeof(WebPAnimFrame) * (size_t)numFrames);
                    PLUTO_FREE(frames);
                }
                frames = nf;
            }
            frames[numFrames++] = f;
        }
        pos += 8 + size + (size & 1);
    }
    if (!isExtended || numFrames == 0)
    {
        PLUTO_FREE(frames);
        return 0;
    }
    out->canvasW = canvasW;
    out->canvasH = canvasH;
    out->bgcolor = bgcolor;
    out->loopCount = loopCount;
    out->numFrames = numFrames;
    out->frames = frames;
    return 1;
}

void webp_anim_free(WebPAnim *anim)
{
    if (anim && anim->frames)
    {
        PLUTO_FREE(anim->frames);
        anim->frames = NULL;
    }
}

/* ── blendPixelNonPremult (webp.lua 2842-2857) ──────────────────────────── */

uint32_t webp_blend_pixel_non_premult(uint32_t src, uint32_t dst)
{
    uint32_t srcA = (src >> 24) & 0xFF;
    if (srcA == 0) return dst;
    uint32_t dstA = (dst >> 24) & 0xFF;
    uint32_t dstFactorA = (dstA * (256 - srcA)) >> 8;
    uint32_t blendA = srcA + dstFactorA;
    /* Lua: scale = floor(16777216 / blendA); chan = floor(v * scale / 16777216).
     * floor(v*scale/16777216) == floor(v * floor(2^24/blendA) / 2^24): the
     * double division then floor is exact for these magnitudes, and the
     * product v*scale fits 64 bits, so integer math reproduces it exactly. */
    uint32_t scale = (uint32_t)(16777216u / blendA);
#define WEBP_CHAN(shift)                                                       \
    ((((src >> shift) & 0xFF) * srcA + ((dst >> shift) & 0xFF) * dstFactorA)   \
     * (uint64_t)scale / 16777216u)
    return ((uint32_t)WEBP_CHAN(16) << 16) | ((uint32_t)WEBP_CHAN(8) << 8)
           | (uint32_t)WEBP_CHAN(0) | (blendA << 24);
#undef WEBP_CHAN
}

/* ── decodeAnimation (webp.lua 2859-2980) ───────────────────────────────── */

int webp_decode_animation(const uint8_t *data, size_t len, WebPAnimResult *out)
{
    WebPAnim anim;
    memset(&anim, 0, sizeof(anim));
    if (!webp_parse_animation(data, len, &anim)) return 0;
    int canvasW = anim.canvasW, canvasH = anim.canvasH;
    int total = canvasW * canvasH;
    if (total < 1 || total > WEBP_MAX_PIXELS)
    {
        webp_anim_free(&anim);
        return 0;
    }

    uint32_t *curr = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)total);
    uint32_t *prevDisposed = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)total);
    uint32_t **outPix = (uint32_t **)PLUTO_MALLOC(sizeof(uint32_t *) * (size_t)anim.numFrames);
    int *outDur = (int *)PLUTO_MALLOC(sizeof(int) * (size_t)anim.numFrames);
    if (!curr || !prevDisposed || !outPix || !outDur)
    {
        if (curr) PLUTO_FREE(curr);
        if (prevDisposed) PLUTO_FREE(prevDisposed);
        if (outPix) PLUTO_FREE(outPix);
        if (outDur) PLUTO_FREE(outDur);
        webp_anim_free(&anim);
        return 0;
    }
    int outN = 0;

    const WebPAnimFrame *prevIter = NULL;
    int prevWasKey = 0;
    memset(curr, 0, sizeof(uint32_t) * (size_t)total);

    for (int i = 0; i < anim.numFrames; i++)
    {
        const WebPAnimFrame *f = &anim.frames[i];
        uint32_t *fargb = NULL;
        int fw = 0, fh = 0;
        int fOwned = 0;
        uint8_t *alphaPlane = NULL;
        if (f->cid && strcmp(f->cid, "VP8L") == 0)
        {
            fargb = webp_decode_vp8l_payload(f->payload, f->payloadLen, &fw, &fh, &fOwned);
            fOwned = 1;
        }
        else
        {
            uint8_t *rgb = vp8_decode_payload(f->payload, f->payloadLen,
                                              f->alpha, f->alphaLen, &fw, &fh, &alphaPlane);
            if (rgb)
            {
                fargb = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)(fw * fh));
                if (fargb)
                {
                    for (int j = 0; j < fw * fh; j++)
                    {
                        int o = j * 3;
                        uint32_t a = alphaPlane ? alphaPlane[j] : 0xFF;
                        fargb[j] = (a << 24) | ((uint32_t)rgb[o] << 16)
                                   | ((uint32_t)rgb[o + 1] << 8) | (uint32_t)rgb[o + 2];
                    }
                }
                PLUTO_FREE(rgb);
            }
            fOwned = 1;
            if (alphaPlane) PLUTO_FREE(alphaPlane);
        }
        if (!fargb)
        {
            /* Lua: return nil mid-way. Emit what we have? The reference
             * aborts the whole decode; mirror that. */
            PLUTO_FREE(curr);
            PLUTO_FREE(prevDisposed);
            for (int k = 0; k < outN; k++) PLUTO_FREE(outPix[k]);
            PLUTO_FREE(outPix);
            PLUTO_FREE(outDur);
            webp_anim_free(&anim);
            return 0;
        }

        int hasAlpha = (f->cid && strcmp(f->cid, "VP8L") == 0) || (f->alpha != NULL);
        int isKey;
        if (i == 0)
            isKey = 1;
        else if ((!hasAlpha || f->noBlend) && f->w == canvasW && f->h == canvasH)
            isKey = 1;
        else
            isKey = prevIter->dispose && (prevIter->w == canvasW || prevWasKey);

        if (isKey)
            memset(curr, 0, sizeof(uint32_t) * (size_t)total);
        else
            memcpy(curr, prevDisposed, sizeof(uint32_t) * (size_t)total);

        int oy = f->y * canvasW + f->x;
        for (int fy = 0; fy < f->h; fy++)
        {
            int dst = oy + fy * canvasW;
            int src = fy * f->w;
            memcpy(curr + dst, fargb + src, sizeof(uint32_t) * (size_t)f->w);
        }

        if (i > 0 && !f->noBlend && !isKey)
        {
            if (!prevIter->dispose)
            {
                for (int fy = 0; fy < f->h; fy++)
                {
                    int off = (f->y + fy) * canvasW + f->x;
                    for (int fx = 0; fx < f->w; fx++)
                    {
                        uint32_t v = curr[off + fx];
                        if (((v >> 24) & 0xFF) != 0xFF)
                            curr[off + fx] = webp_blend_pixel_non_premult(v, prevDisposed[off + fx]);
                    }
                }
            }
            else
            {
                int srcMaxX = f->x + f->w;
                int dstMaxX = prevIter->x + prevIter->w;
                int dstMaxY = prevIter->y + prevIter->h;
                for (int fy = 0; fy < f->h; fy++)
                {
                    int canvasY = f->y + fy;
                    /* up to 2 ranges, as the reference builds */
                    struct { int left, width; } ranges[2];
                    int n = 0;
                    if (canvasY < prevIter->y || canvasY >= dstMaxY ||
                        f->x >= dstMaxX || srcMaxX <= prevIter->x)
                    {
                        ranges[n].left = f->x;
                        ranges[n].width = f->w;
                        n++;
                    }
                    else
                    {
                        if (f->x < prevIter->x)
                        {
                            ranges[n].left = f->x;
                            ranges[n].width = prevIter->x - f->x;
                            n++;
                        }
                        if (srcMaxX > dstMaxX)
                        {
                            ranges[n].left = dstMaxX;
                            ranges[n].width = srcMaxX - dstMaxX;
                            n++;
                        }
                    }
                    for (int r = 0; r < n; r++)
                    {
                        int off = canvasY * canvasW + ranges[r].left;
                        for (int fx = 0; fx < ranges[r].width; fx++)
                        {
                            uint32_t v = curr[off + fx];
                            if (((v >> 24) & 0xFF) != 0xFF)
                                curr[off + fx] = webp_blend_pixel_non_premult(v, prevDisposed[off + fx]);
                        }
                    }
                }
            }
        }

        prevIter = f;
        prevWasKey = isKey;
        memcpy(prevDisposed, curr, sizeof(uint32_t) * (size_t)total);

        outPix[outN] = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)total);
        if (!outPix[outN])
        {
            PLUTO_FREE(fargb);
            PLUTO_FREE(curr);
            PLUTO_FREE(prevDisposed);
            for (int k = 0; k < outN; k++) PLUTO_FREE(outPix[k]);
            PLUTO_FREE(outPix);
            PLUTO_FREE(outDur);
            webp_anim_free(&anim);
            return 0;
        }
        memcpy(outPix[outN], curr, sizeof(uint32_t) * (size_t)total);
        outDur[outN] = f->duration;
        outN++;
        PLUTO_FREE(fargb);

        if (f->dispose)
        {
            int oy2 = f->y * canvasW + f->x;
            for (int fy = 0; fy < f->h; fy++)
            {
                int off = oy2 + fy * canvasW;
                memset(prevDisposed + off, 0, sizeof(uint32_t) * (size_t)f->w);
            }
        }
    }

    webp_anim_free(&anim);
    PLUTO_FREE(curr);
    PLUTO_FREE(prevDisposed);
    out->width = canvasW;
    out->height = canvasH;
    out->bgcolor = anim.bgcolor;
    out->loopCount = anim.loopCount;
    out->numFrames = outN;
    out->pix = outPix;
    out->durations = outDur;
    return 1;
}

void webp_anim_result_free(WebPAnimResult *res)
{
    if (!res) return;
    if (res->pix)
    {
        for (int i = 0; i < res->numFrames; i++)
            if (res->pix[i]) PLUTO_FREE(res->pix[i]);
        PLUTO_FREE(res->pix);
        res->pix = NULL;
    }
    if (res->durations)
    {
        PLUTO_FREE(res->durations);
        res->durations = NULL;
    }
}

/* ── composite + WebPDecoder.decode (webp.lua 2982-3033) ────────────────── */

static int webp_composite(int gray, int a)
{
    if (a >= 255) return gray;
    if (a <= 0) return 255;
    return (gray * a + 255 * (255 - a)) / 255;
}

typedef struct WebPGrayCtx
{
    const uint32_t *pix;
    int w;
    const uint8_t *const *rows; /* scale accumulator output (via host fn) */
} WebPGrayCtx;

static uint8_t webp_out_pixel(void *ud, int x, int y)
{
    WebPGrayCtx *c = (WebPGrayCtx *)ud;
    const uint8_t *r = c->rows[y];
    if (!r) return 255;
    return r[x];
}

/* Convert an ARGB buffer to dithered LCDBitmap exactly like
 * WebPDecoder.decode's scale+dither tail. */
static LCDBitmap *webp_argb_to_bitmap(const uint32_t *pix, int w, int h,
                                      int maxW, int maxH)
{
    ScaleAccum *acc = scale_accum_new(w, h, maxW, maxH);
    int boxW = 0, boxH = 0, targetW = 0, targetH = 0;
    scale_box_sizes(w, h, maxW, maxH, &boxW, &boxH, &targetW, &targetH);
    if (!acc) return NULL;

    uint8_t *grayRow = (uint8_t *)PLUTO_MALLOC((size_t)(w ? w : 1));
    if (!grayRow)
    {
        scale_accum_free(acc);
        return NULL;
    }
    for (int y = 0; y < h; y++)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        int base = y * w;
        for (int x = 0; x < w; x++)
        {
            uint32_t argb = pix[base + x];
            int r = (int)((argb >> 16) & 0xFF);
            int g = (int)((argb >> 8) & 0xFF);
            int b = (int)(argb & 0xFF);
            int gv = dither_rgb_to_gray(r, g, b);
            grayRow[x] = (uint8_t)webp_composite(gv, (int)((argb >> 24) & 0xFF));
        }
        scale_accum_add_row(acc, grayRow);
    }
    int outCount = 0, outWidth = 0;
    uint8_t **rows = scale_accum_finish(acc, &outCount, &outWidth);
    LCDBitmap *img = NULL;
    if (rows && outCount > 0)
    {
        WebPGrayCtx gc;
        gc.pix = NULL;
        gc.w = outWidth;
        gc.rows = (const uint8_t *const *)rows;
        img = dither_to_bitmap(targetW, targetH, webp_out_pixel, &gc);
    }
    PLUTO_FREE(grayRow);
    scale_accum_free(acc);
    return img;
}

LCDBitmap *webp_decode(const uint8_t *data, size_t len, int maxW, int maxH)
{
    if (maxW <= 0) maxW = 360;
    if (maxH <= 0) maxH = 200;
    const char *cid = NULL;
    const uint8_t *payload = NULL;
    size_t payloadLen = 0;
    const uint8_t *alphaData = NULL;
    size_t alphaLen = 0;
    int kind = webp_parse_container(data, len, &cid, &payload, &payloadLen,
                                    &alphaData, &alphaLen);
    if (!kind) return NULL;
    const uint32_t *pix = NULL;
    uint32_t *heapPix = NULL;
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    if (kind == 1)
    {
        heapPix = webp_decode_vp8l_payload(payload, payloadLen, &w, &h, NULL);
        pix = heapPix;
    }
    else if (kind == 2)
    {
        uint8_t *alpha = NULL;
        rgb = vp8_decode_payload(payload, payloadLen, alphaData, alphaLen, &w, &h, &alpha);
        if (rgb)
        {
            heapPix = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)(w * h));
            if (heapPix)
            {
                for (int i = 0; i < w * h; i++)
                {
                    int o = i * 3;
                    uint32_t a = alpha ? alpha[i] : 0xFF;
                    heapPix[i] = (a << 24) | ((uint32_t)rgb[o] << 16)
                                 | ((uint32_t)rgb[o + 1] << 8) | (uint32_t)rgb[o + 2];
                }
            }
            pix = heapPix;
        }
        if (alpha) PLUTO_FREE(alpha);
    }
    else
    {
        WebPAnimResult anim;
        memset(&anim, 0, sizeof(anim));
        if (webp_decode_animation(data, len, &anim) && anim.numFrames > 0)
        {
            w = anim.width;
            h = anim.height;
            heapPix = anim.pix[0]; /* first frame */
            pix = heapPix;
            /* Detach the first frame; free the rest. */
            anim.pix[0] = NULL;
            webp_anim_result_free(&anim);
        }
        else
        {
            webp_anim_result_free(&anim);
        }
    }
    if (rgb) PLUTO_FREE(rgb);
    if (!pix)
    {
        if (heapPix) PLUTO_FREE(heapPix);
        return NULL;
    }
    LCDBitmap *img = webp_argb_to_bitmap(pix, w, h, maxW, maxH);
    if (heapPix) PLUTO_FREE(heapPix);
    return img;
}

/* WebPDecoder._testDecodeRaw: full ARGB dump for tests (Lua parity). */
uint32_t *webp_decode_raw(const uint8_t *data, size_t len,
                          int *outW, int *outH)
{
    const char *cid = NULL;
    const uint8_t *payload = NULL;
    size_t payloadLen = 0;
    const uint8_t *alphaData = NULL;
    size_t alphaLen = 0;
    int kind = webp_parse_container(data, len, &cid, &payload, &payloadLen,
                                    &alphaData, &alphaLen);
    if (kind)
    {
        if (kind == 1)
        {
            int owned = 0;
            uint32_t *pix = webp_decode_vp8l_payload(payload, payloadLen, outW, outH, &owned);
            return pix;
        }
        uint8_t *rgb = NULL;
        uint8_t *alpha = NULL;
        int w = 0, h = 0;
        rgb = vp8_decode_payload(payload, payloadLen, alphaData, alphaLen, &w, &h, &alpha);
        if (!rgb) return NULL;
        uint32_t *pix = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)(w * h));
        if (pix)
        {
            for (int i = 0; i < w * h; i++)
            {
                int o = i * 3;
                uint32_t a = alpha ? alpha[i] : 0xFF;
                pix[i] = (a << 24) | ((uint32_t)rgb[o] << 16)
                         | ((uint32_t)rgb[o + 1] << 8) | (uint32_t)rgb[o + 2];
            }
        }
        PLUTO_FREE(rgb);
        if (alpha) PLUTO_FREE(alpha);
        *outW = w;
        *outH = h;
        return pix;
    }
    WebPAnimResult anim;
    memset(&anim, 0, sizeof(anim));
    if (webp_decode_animation(data, len, &anim) && anim.numFrames > 0)
    {
        uint32_t *first = anim.pix[0];
        anim.pix[0] = NULL;
        *outW = anim.width;
        *outH = anim.height;
        webp_anim_result_free(&anim);
        return first;
    }
    webp_anim_result_free(&anim);
    return NULL;
}
