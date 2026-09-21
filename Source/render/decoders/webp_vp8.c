/*
 * PlutoBrowser — webp_vp8.c
 * Port of Source/render/decoders/webp.lua lines 1007-2736 (P29):
 *   alphaUnfilter/decodeAlphaPlane, VP8 boolean decoder (BITS=24),
 *   segment/filter/quant/proba headers, DCT/WHT transforms, intra
 *   predictors (16x16/8x8/4x4), in-loop filters (normal + simple),
 *   residual/coeff decoding, macroblock reconstruction, YUV->RGB with
 *   fancy upsampling, decodeVP8Payload, parseWebP container,
 *   parseWebPAnimation + decodeAnimation, blendPixelNonPremult.
 *
 * Lua -> C map (prefix vp8_ / alpha_):
 *   vp8LoadNew/newBr        -> vp8_load_new / vp8_br_new
 *   vp8GetBit/GetSigned/    -> vp8_get_bit / vp8_get_signed /
 *   GetValue/GetSignedValue    vp8_get_value / vp8_get_signed_value
 *   parseSegmentHeader ...  -> vp8_parse_segment_header ...
 *   transformOne/WHT/...    -> vp8_transform_one / vp8_transform_wht ...
 *   dc16..hd4 (predictors)  -> vp8_dc16 ... (function pointers in tables)
 *   doFilter/doFilter2..    -> vp8_do_filter / vp8_do_filter2 ...
 *   getLargeValue/getCoeffs -> vp8_get_large_value / vp8_get_coeffs
 *   parseResiduals etc.     -> vp8_parse_residuals ...
 *   reconstructRow          -> vp8_reconstruct_row
 *   upsampleLinePair        -> vp8_upsample_line_pair
 *   finishRow               -> vp8_finish_row
 *   decodeVP8Payload        -> vp8_decode_payload
 *   alphaUnfilter           -> alpha_unfilter
 *   decodeAlphaPlane        -> alpha_decode_plane
 *   parseWebP               -> webp_parse_container
 *   parseWebPAnimation      -> webp_parse_animation
 *   decodeAnimation         -> webp_decode_animation
 *   blendPixelNonPremult    -> webp_blend_pixel_non_premult
 *
 * Parity notes:
 *   - Lua tables are 1-based; all transcribed tables (webp_vp8_data.h) are
 *     0-based C arrays. Every access applies the -1 correction explicitly.
 *   - The boolean decoder keeps value masked to 32 bits exactly like Lua.
 *   - kCat3456 rows are 0-terminated within 12 entries (2-D [4][12]).
 *   - kScan values are ABSOLUTE offsets into the 32-stride working buffer
 *     (0,4,8,12,128,...) — no BPS multiplication on top.
 *   - YBASE=64, UBASE/VBASE=32, BPS=32; yArr/uArr/vArr sizes as reference.
 *   - decodeImageStream/decodeImageData/applyInverseTransforms from P28
 *     are reused for the lossless alpha method; they live in webp.c and
 *     are declared in webp-internal.h.
 *   - Arithmetic is signed 32-bit with arithmetic shift (Lua >> on
 *     negatives == floor division; sar() reproduces it).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pd_api.h"
#include "render/decoders/webp.h"
#include "render/decoders/webp_vp8_data.h"
#include "render/decoders/webp-internal.h"
#include "render/decoders/scale.h"
#include "render/decoders/dither.h"
#include "../core/pluto_mem.h"

extern PlaydateAPI *pluto_pd(void);
#define PLUTO_MALLOC(n) pluto_mem_realloc(NULL, (n))
#define PLUTO_FREE(p)   pluto_mem_realloc((p), 0)

/* ── Constants (webp.lua 1128-1133, 2295-2300) ─────────────────────────── */
#define VP8_BPS    32
#define VP8_YBASE  64
#define VP8_UBASE  32
#define VP8_VBASE  32

static const uint16_t VP8_KSCAN[16] = {
    0, 4, 8, 12, 128, 132, 136, 140,
    256, 260, 264, 268, 384, 388, 392, 396
};
static const uint8_t VP8_KFILTER_EXTRA_ROWS[3] = { 0, 2, 8 };

#define VP8_DC_PRED 0
#define VP8_TM_PRED 1
#define VP8_V_PRED  2
#define VP8_H_PRED  3

/* ═══════════════════════════════════════════════════════════════════════════
 * P29a — alpha plane (webp.lua 1007-1121)
 * ═════════════════════════════════════════════════════════════════════════ */

static uint8_t *alpha_unfilter(uint8_t *alpha, int width, int height, int filter)
{
    for (int y = 0; y < height; y++)
    {
        int o = y * width;
        if (y == 0)
        {
            uint8_t pred = 0;
            for (int x = 0; x < width; x++)
            {
                uint8_t v = (uint8_t)((pred + alpha[o + x]) & 0xFF);
                alpha[o + x] = v;
                pred = v;
            }
        }
        else if (filter == 1)
        {
            uint8_t pred = alpha[o - width];
            for (int x = 0; x < width; x++)
            {
                uint8_t v = (uint8_t)((pred + alpha[o + x]) & 0xFF);
                alpha[o + x] = v;
                pred = v;
            }
        }
        else if (filter == 2)
        {
            int prev = o - width;
            for (int x = 0; x < width; x++)
                alpha[o + x] = (uint8_t)((alpha[prev + x] + alpha[o + x]) & 0xFF);
        }
        else if (filter == 3)
        {
            int prev = o - width;
            uint8_t top = alpha[prev];
            uint8_t top_left = top;
            uint8_t left = top;
            for (int x = 0; x < width; x++)
            {
                top = alpha[prev + x];
                int g = (int)left + (int)top - (int)top_left;
                if ((g & ~0xff) != 0)
                {
                    if (g < 0) g = 0; else g = 255;
                }
                left = (uint8_t)((alpha[o + x] + g) & 0xFF);
                top_left = top;
                alpha[o + x] = left;
            }
        }
    }
    return alpha;
}

uint8_t *alpha_decode_plane(const uint8_t *alphaPayload, size_t alphaLen,
                            int width, int height)
{
    if (!alphaPayload || alphaLen < 2) return NULL;
    int h0 = alphaPayload[0];
    int method = h0 & 0x03;
    int filter = (h0 >> 2) & 0x03;
    if (filter > 3) return NULL;
    if (method > 1 || ((h0 >> 4) & 0x03) > 1 || ((h0 >> 6) & 0x03) != 0) return NULL;
    int total = width * height;
    if (total < 1 || total > WEBP_MAX_PIXELS) return NULL;

    uint8_t *alpha = NULL;
    if (method == 0)
    {
        if ((int)alphaLen - 1 < total) return NULL;
        alpha = (uint8_t *)PLUTO_MALLOC((size_t)total);
        if (!alpha) return NULL;
        for (int i = 0; i < total; i++)
            alpha[i] = alphaPayload[1 + i];
    }
    else
    {
        WebPBitReader br;
        webp_br_init(&br, alphaPayload, (int)alphaLen, 2);
        WebPDecodeCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        if (!vp8l_decode_image_stream_pub(width, height, 1, &br, &ctx))
        {
            vp8l_ctx_free_pub(&ctx);
            return NULL;
        }
        int txs = ctx.transformXsize;
        int tys = ctx.transformYsize;
        if (txs < 1 || tys < 1 || txs * tys > WEBP_MAX_PIXELS)
        {
            vp8l_ctx_free_pub(&ctx);
            return NULL;
        }
        uint32_t *data = (uint32_t *)PLUTO_MALLOC(sizeof(uint32_t) * (size_t)(txs * tys));
        if (!data)
        {
            vp8l_ctx_free_pub(&ctx);
            return NULL;
        }
        memset(data, 0, sizeof(uint32_t) * (size_t)(txs * tys));
        if (!vp8l_decode_image_data_pub(&br, &ctx, data, txs, tys))
        {
            PLUTO_FREE(data);
            vp8l_ctx_free_pub(&ctx);
            return NULL;
        }
        if (br.eos)
        {
            PLUTO_FREE(data);
            vp8l_ctx_free_pub(&ctx);
            return NULL;
        }
        int owned = 0;
        uint32_t *argb = vp8l_apply_inverse_transforms_pub(&ctx, data, tys, &owned);
        vp8l_ctx_free_pub(&ctx);
        if (!argb)
        {
            PLUTO_FREE(data);
            return NULL;
        }
        alpha = (uint8_t *)PLUTO_MALLOC((size_t)total);
        if (!alpha)
        {
            if (owned) PLUTO_FREE(argb);
            return NULL;
        }
        for (int i = 0; i < total; i++)
            alpha[i] = (uint8_t)((argb[i] >> 8) & 0xFF);
        if (owned) PLUTO_FREE(argb);
        PLUTO_FREE(data);
    }
    if (filter == 0) return alpha;
    return alpha_unfilter(alpha, width, height, filter);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29b — boolean decoder + headers (webp.lua 1160-1379)
 * ═════════════════════════════════════════════════════════════════════════ */

static int VP8_LOG2[256]; /* [1..255]; built once */

static void vp8_log2_init(void)
{
    static int inited = 0;
    if (inited) return;
    for (int i = 1; i <= 255; i++)
    {
        int v = i, n = 0;
        while (v > 1) { v >>= 1; n++; }
        VP8_LOG2[i] = n;
    }
    inited = 1;
}

static void vp8_load_new(VP8BoolBr *br)
{
    const uint8_t *payload = br->payload;
    if (br->p < br->max1)
    {
        uint32_t b0 = payload[br->p];
        uint32_t b1 = payload[br->p + 1];
        uint32_t b2 = payload[br->p + 2];
        br->value = ((br->value << 24) | (b0 << 16) | (b1 << 8) | b2) & 0xFFFFFFFFu;
        br->bits += 24;
        br->p += 3;
    }
    else if (br->p < br->end1)
    {
        br->value = ((br->value << 8) | payload[br->p]) & 0xFFFFFFFFu;
        br->bits += 8;
        br->p += 1;
    }
    else if (br->eof == 0)
    {
        br->value = (br->value << 8) & 0xFFFFFFFFu;
        br->bits += 8;
        br->eof = 1;
    }
    else
    {
        br->bits = 0;
    }
}

static void vp8_br_new(VP8BoolBr *br, const uint8_t *payload, int start1, int size)
{
    br->payload = payload;
    br->p = start1;
    br->end1 = start1 + size;
    br->max1 = start1 + size - 3;
    br->range = 254;
    br->value = 0;
    br->bits = -8;
    br->eof = 0;
    vp8_load_new(br);
}

static int vp8_get_bit(VP8BoolBr *br, int prob)
{
    if (br->bits < 0) vp8_load_new(br);
    int pos = br->bits;
    int range = br->range;
    int split = (range * prob) >> 8;
    uint32_t value = br->value >> pos;
    int bit = 0;
    if (value > (uint32_t)split)
    {
        bit = 1;
        range = range - split;
        br->value = (br->value - (((uint32_t)split + 1u) << pos)) & 0xFFFFFFFFu;
    }
    else
    {
        range = split + 1;
    }
    int shift = 7 - VP8_LOG2[range];
    range = range << shift;
    br->bits -= shift;
    br->range = range - 1;
    return bit;
}

static int vp8_get_signed(VP8BoolBr *br, int v)
{
    if (br->bits < 0) vp8_load_new(br);
    int pos = br->bits;
    int split = br->range >> 1;
    uint32_t value = br->value >> pos;
    int mask;
    if (value > (uint32_t)split) mask = -1; else mask = 0;
    br->bits -= 1;
    br->range = (br->range + mask) | 1;
    br->value = (br->value - ((((uint32_t)split + 1u) & (uint32_t)mask) << pos)) & 0xFFFFFFFFu;
    return (v ^ mask) - mask;
}

static int vp8_get_value(VP8BoolBr *br, int bits)
{
    int v = 0;
    for (int i = bits - 1; i >= 0; i--)
        v |= vp8_get_bit(br, 128) << i;
    return v;
}

static int vp8_get_signed_value(VP8BoolBr *br, int bits)
{
    int value = vp8_get_value(br, bits);
    if (vp8_get_bit(br, 128) != 0) return -value;
    return value;
}

/* ── Headers ─────────────────────────────────────────────────────────────── */

static int vp8_parse_segment_header(VP8BoolBr *br, VP8Dec *dec)
{
    VP8SegmentHdr *hdr = &dec->segmentHdr;
    hdr->useSegment = vp8_get_bit(br, 128);
    if (hdr->useSegment != 0)
    {
        hdr->updateMap = vp8_get_bit(br, 128);
        if (vp8_get_bit(br, 128) != 0)
        {
            hdr->absoluteDelta = vp8_get_bit(br, 128);
            for (int s = 0; s < 4; s++)
            {
                if (vp8_get_bit(br, 128) != 0)
                    hdr->quantizer[s] = vp8_get_signed_value(br, 7);
                else
                    hdr->quantizer[s] = 0;
            }
            for (int s = 0; s < 4; s++)
            {
                if (vp8_get_bit(br, 128) != 0)
                    hdr->filterStrength[s] = vp8_get_signed_value(br, 6);
                else
                    hdr->filterStrength[s] = 0;
            }
        }
        if (hdr->updateMap != 0)
        {
            for (int s = 0; s < 3; s++)
            {
                if (vp8_get_bit(br, 128) != 0)
                    dec->probaSegments[s] = vp8_get_value(br, 8);
                else
                    dec->probaSegments[s] = 255;
            }
        }
    }
    else
    {
        hdr->updateMap = 0;
    }
    return br->eof == 0;
}

static int vp8_parse_filter_header(VP8BoolBr *br, VP8Dec *dec)
{
    VP8FilterHdr *hdr = &dec->filterHdr;
    hdr->simple = vp8_get_bit(br, 128);
    hdr->level = vp8_get_value(br, 6);
    hdr->sharpness = vp8_get_value(br, 3);
    hdr->useLfDelta = vp8_get_bit(br, 128);
    if (hdr->useLfDelta != 0)
    {
        if (vp8_get_bit(br, 128) != 0)
        {
            for (int i = 0; i < 4; i++)
            {
                if (vp8_get_bit(br, 128) != 0)
                    hdr->refLfDelta[i] = vp8_get_signed_value(br, 6);
            }
            for (int i = 0; i < 4; i++)
            {
                if (vp8_get_bit(br, 128) != 0)
                    hdr->modeLfDelta[i] = vp8_get_signed_value(br, 6);
            }
        }
    }
    if (hdr->level == 0)
        dec->filterType = 0;
    else if (hdr->simple != 0)
        dec->filterType = 1;
    else
        dec->filterType = 2;
    return br->eof == 0;
}

static int vp8_clip_q(int v, int m)
{
    if (v < 0) return 0;
    if (v > m) return m;
    return v;
}

static void vp8_parse_quant(VP8BoolBr *br, VP8Dec *dec)
{
    int baseQ0 = vp8_get_value(br, 7);
    int dqy1dc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dqy2dc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dqy2ac = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dquvdc = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    int dquvac = (vp8_get_bit(br, 128) != 0) ? vp8_get_signed_value(br, 4) : 0;
    VP8SegmentHdr *hdr = &dec->segmentHdr;
    for (int i = 0; i < 4; i++)
    {
        int q;
        if (hdr->useSegment != 0)
        {
            q = hdr->quantizer[i];
            if (hdr->absoluteDelta == 0) q = q + baseQ0;
        }
        else
        {
            if (i > 0)
            {
                dec->dqm[i] = dec->dqm[0];
                continue;
            }
            q = baseQ0;
        }
        VP8QuantInfo *m = &dec->dqm[i];
        m->y1[0] = vp8_dc_table[vp8_clip_q(q + dqy1dc, 127)];
        m->y1[1] = vp8_ac_table[vp8_clip_q(q, 127)];
        m->y2[0] = vp8_dc_table[vp8_clip_q(q + dqy2dc, 127)] * 2;
        m->y2[1] = (vp8_ac_table[vp8_clip_q(q + dqy2ac, 127)] * 101581) >> 16;
        if (m->y2[1] < 8) m->y2[1] = 8;
        m->uv[0] = vp8_dc_table[vp8_clip_q(q + dquvdc, 117)];
        m->uv[1] = vp8_ac_table[vp8_clip_q(q + dquvac, 127)];
    }
}

static void vp8_parse_proba(VP8BoolBr *br, VP8Dec *dec)
{
    for (int t = 0; t < 4; t++)
    {
        for (int b = 0; b < 8; b++)
        {
            for (int c = 0; c < 3; c++)
            {
                for (int p = 0; p < 11; p++)
                {
                    int ci = ((t * 8 + b) * 3 + c) * 11 + p;
                    if (vp8_get_bit(br, vp8_coeffs_update_proba[ci]) != 0)
                        dec->proba[ci] = (uint8_t)vp8_get_value(br, 8);
                    else
                        dec->proba[ci] = vp8_coeffs_proba0[ci];
                }
            }
        }
    }
    dec->useSkipProba = vp8_get_bit(br, 128);
    if (dec->useSkipProba != 0)
        dec->skipP = vp8_get_value(br, 8);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29c — transforms (webp.lua 1381-1504)
 * ═════════════════════════════════════════════════════════════════════════ */

static int vp8_sar(int v, int n)
{
    if (v >= 0) return v >> n;
    return ~((~v) >> n);
}

static int vp8_mul1(int a) { return vp8_sar(a * 20091, 16) + a; }
static int vp8_mul2(int a) { return vp8_sar(a * 35468, 16); }

static uint8_t vp8_clip8(int v)
{
    if ((v & ~0xff) == 0) return (uint8_t)v;
    if (v < 0) return 0;
    return 255;
}

static int8_t vp8_ksclip1(int v)
{
    if (v < -128) return -128;
    if (v > 127) return 127;
    return (int8_t)v;
}

static int8_t vp8_ksclip2(int v)
{
    if (v < -16) return -16;
    if (v > 15) return 15;
    return (int8_t)v;
}

static int vp8_kabs0(int v)
{
    if (v < 0) return -v;
    return v;
}

static void vp8_transform_one(const int16_t *in_, int inOff, uint8_t *buf, int dst)
{
    int t[16];
    for (int i = 0; i < 4; i++)
    {
        int a = in_[inOff + i] + in_[inOff + 8 + i];
        int b = in_[inOff + i] - in_[inOff + 8 + i];
        int c = vp8_mul2(in_[inOff + 4 + i]) - vp8_mul1(in_[inOff + 12 + i]);
        int d = vp8_mul1(in_[inOff + 4 + i]) + vp8_mul2(in_[inOff + 12 + i]);
        t[i * 4] = a + d;
        t[i * 4 + 1] = b + c;
        t[i * 4 + 2] = b - c;
        t[i * 4 + 3] = a - d;
    }
    for (int i = 0; i < 4; i++)
    {
        int o = dst + i * VP8_BPS;
        int dc = t[i] + 4;
        int a = dc + t[i + 8];
        int b = dc - t[i + 8];
        int c = vp8_mul2(t[i + 4]) - vp8_mul1(t[i + 12]);
        int d = vp8_mul1(t[i + 4]) + vp8_mul2(t[i + 12]);
        buf[o] = vp8_clip8(buf[o] + vp8_sar(a + d, 3));
        buf[o + 1] = vp8_clip8(buf[o + 1] + vp8_sar(b + c, 3));
        buf[o + 2] = vp8_clip8(buf[o + 2] + vp8_sar(b - c, 3));
        buf[o + 3] = vp8_clip8(buf[o + 3] + vp8_sar(a - d, 3));
    }
}

static void vp8_transform_ac3(const int16_t *in_, int inOff, uint8_t *buf, int dst)
{
    int a = in_[inOff] + 4;
    int c4 = vp8_mul2(in_[inOff + 4]);
    int d4 = vp8_mul1(in_[inOff + 4]);
    int c1 = vp8_mul2(in_[inOff + 1]);
    int d1 = vp8_mul1(in_[inOff + 1]);
#define VP8_STORE2(y, dcval)                                                    \
    do                                                                          \
    {                                                                           \
        int o = dst + (y) * VP8_BPS;                                            \
        buf[o] = vp8_clip8(buf[o] + vp8_sar((dcval) + d1, 3));                  \
        buf[o + 1] = vp8_clip8(buf[o + 1] + vp8_sar((dcval) + c1, 3));          \
        buf[o + 2] = vp8_clip8(buf[o + 2] + vp8_sar((dcval) - c1, 3));          \
        buf[o + 3] = vp8_clip8(buf[o + 3] + vp8_sar((dcval) - d1, 3));          \
    } while (0)
    VP8_STORE2(0, a + d4);
    VP8_STORE2(1, a + c4);
    VP8_STORE2(2, a - c4);
    VP8_STORE2(3, a - d4);
#undef VP8_STORE2
}

static void vp8_transform_dc(const int16_t *in_, int inOff, uint8_t *buf, int dst)
{
    int v = vp8_sar(in_[inOff] + 4, 3);
    for (int j = 0; j < 4; j++)
    {
        int o = dst + j * VP8_BPS;
        buf[o] = vp8_clip8(buf[o] + v);
        buf[o + 1] = vp8_clip8(buf[o + 1] + v);
        buf[o + 2] = vp8_clip8(buf[o + 2] + v);
        buf[o + 3] = vp8_clip8(buf[o + 3] + v);
    }
}

static void vp8_transform_uv(const int16_t *in_, int inOff, uint8_t *buf, int dst)
{
    vp8_transform_one(in_, inOff, buf, dst);
    vp8_transform_one(in_, inOff + 16, buf, dst + 4);
    vp8_transform_one(in_, inOff + 32, buf, dst + 4 * VP8_BPS);
    vp8_transform_one(in_, inOff + 48, buf, dst + 4 * VP8_BPS + 4);
}

static void vp8_transform_dcuv(const int16_t *in_, int inOff, uint8_t *buf, int dst)
{
    if (in_[inOff] != 0) vp8_transform_dc(in_, inOff, buf, dst);
    if (in_[inOff + 16] != 0) vp8_transform_dc(in_, inOff + 16, buf, dst + 4);
    if (in_[inOff + 32] != 0) vp8_transform_dc(in_, inOff + 32, buf, dst + 4 * VP8_BPS);
    if (in_[inOff + 48] != 0) vp8_transform_dc(in_, inOff + 48, buf, dst + 4 * VP8_BPS + 4);
}

static int32_t vp8_wht_tmp[16];

static void vp8_transform_wht(const int16_t *in_, int inOff, int16_t *out, int outOff)
{
    int32_t *t = vp8_wht_tmp;
    for (int i = 0; i < 4; i++)
    {
        int32_t a0 = in_[inOff + i] + in_[inOff + 12 + i];
        int32_t a1 = in_[inOff + 4 + i] + in_[inOff + 8 + i];
        int32_t a2 = in_[inOff + 4 + i] - in_[inOff + 8 + i];
        int32_t a3 = in_[inOff + i] - in_[inOff + 12 + i];
        t[i] = a0 + a1;
        t[8 + i] = a0 - a1;
        t[4 + i] = a3 + a2;
        t[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++)
    {
        int32_t dc = t[i * 4] + 3;
        int32_t a0 = dc + t[i * 4 + 3];
        int32_t a1 = t[i * 4 + 1] + t[i * 4 + 2];
        int32_t a2 = t[i * 4 + 1] - t[i * 4 + 2];
        int32_t a3 = dc - t[i * 4 + 3];
        out[outOff] = (int16_t)vp8_sar((int)a0 + (int)a1, 3);
        out[outOff + 16] = (int16_t)vp8_sar((int)a3 + (int)a2, 3);
        out[outOff + 32] = (int16_t)vp8_sar((int)a0 - (int)a1, 3);
        out[outOff + 48] = (int16_t)vp8_sar((int)a3 - (int)a2, 3);
        outOff += 64;
    }
}

static void vp8_do_transform(uint32_t bits, const int16_t *coeffs, int inOff,
                             uint8_t *buf, int dst)
{
    int top = (int)(bits >> 30);
    if (top == 3)
        vp8_transform_one(coeffs, inOff, buf, dst);
    else if (top == 2)
        vp8_transform_ac3(coeffs, inOff, buf, dst);
    else if (top == 1)
        vp8_transform_dc(coeffs, inOff, buf, dst);
}

static void vp8_do_uv_transform(uint32_t bits, const int16_t *coeffs, int inOff,
                                uint8_t *buf, int dst)
{
    if ((bits & 0xff) != 0)
    {
        if ((bits & 0xaa) != 0)
            vp8_transform_uv(coeffs, inOff, buf, dst);
        else
            vp8_transform_dcuv(coeffs, inOff, buf, dst);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29d — intra predictions (webp.lua 1505-1802)
 * ═════════════════════════════════════════════════════════════════════════ */

static int vp8_avg2(int a, int b) { return (a + b + 1) >> 1; }
static int vp8_avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }

static void vp8_true_motion(uint8_t *buf, int dst, int size)
{
    int topBase = dst - VP8_BPS;
    uint8_t topleft = buf[topBase - 1];
    for (int y = 0; y < size; y++)
    {
        uint8_t left = buf[dst - 1 + y * VP8_BPS];
        int r = dst + y * VP8_BPS;
        for (int x = 0; x < size; x++)
            buf[r + x] = vp8_clip8((int)buf[topBase + x] + (int)left - (int)topleft);
    }
}

static void vp8_dc16(uint8_t *buf, int dst)
{
    int dc = 16;
    for (int j = 0; j < 16; j++)
        dc += buf[dst - 1 + j * VP8_BPS] + buf[dst - VP8_BPS + j];
    int v = dc >> 5;
    for (int j = 0; j < 16; j++)
        memset(buf + dst + j * VP8_BPS, v, 16);
}

static void vp8_dc16_no_top(uint8_t *buf, int dst)
{
    int dc = 8;
    for (int j = 0; j < 16; j++) dc += buf[dst - 1 + j * VP8_BPS];
    int v = dc >> 4;
    for (int j = 0; j < 16; j++)
        memset(buf + dst + j * VP8_BPS, v, 16);
}

static void vp8_dc16_no_left(uint8_t *buf, int dst)
{
    int dc = 8;
    for (int i = 0; i < 16; i++) dc += buf[dst - VP8_BPS + i];
    int v = dc >> 4;
    for (int j = 0; j < 16; j++)
        memset(buf + dst + j * VP8_BPS, v, 16);
}

static void vp8_dc16_no_top_left(uint8_t *buf, int dst)
{
    for (int j = 0; j < 16; j++)
        memset(buf + dst + j * VP8_BPS, 128, 16);
}

static void vp8_ve16(uint8_t *buf, int dst)
{
    int top = dst - VP8_BPS;
    for (int j = 0; j < 16; j++)
        memcpy(buf + dst + j * VP8_BPS, buf + top, 16);
}

static void vp8_he16(uint8_t *buf, int dst)
{
    for (int j = 0; j < 16; j++)
    {
        uint8_t v = buf[dst - 1 + j * VP8_BPS];
        memset(buf + dst + j * VP8_BPS, v, 16);
    }
}

static void vp8_tm8(uint8_t *buf, int dst);
static void vp8_tm16(uint8_t *buf, int dst);
static void vp8_tm4(uint8_t *buf, int dst);

typedef void (*VP8PredFn)(uint8_t *buf, int dst);

static void vp8_tm8(uint8_t *buf, int dst) { vp8_true_motion(buf, dst, 8); }
static void vp8_tm16(uint8_t *buf, int dst) { vp8_true_motion(buf, dst, 16); }

static const VP8PredFn VP8_PRED_LUMA16[7] = {
    vp8_dc16, vp8_tm16, vp8_ve16, vp8_he16,
    vp8_dc16_no_top, vp8_dc16_no_left, vp8_dc16_no_top_left
};

static void vp8_dc8uv(uint8_t *buf, int dst)
{
    int dc0 = 8;
    for (int i = 0; i < 8; i++)
        dc0 += buf[dst - VP8_BPS + i] + buf[dst - 1 + i * VP8_BPS];
    int v = dc0 >> 4;
    for (int j = 0; j < 8; j++)
        memset(buf + dst + j * VP8_BPS, v, 8);
}

static void vp8_dc8uv_no_top(uint8_t *buf, int dst)
{
    int dc0 = 4;
    for (int i = 0; i < 8; i++) dc0 += buf[dst - 1 + i * VP8_BPS];
    int v = dc0 >> 3;
    for (int j = 0; j < 8; j++)
        memset(buf + dst + j * VP8_BPS, v, 8);
}

static void vp8_dc8uv_no_left(uint8_t *buf, int dst)
{
    int dc0 = 4;
    for (int i = 0; i < 8; i++) dc0 += buf[dst - VP8_BPS + i];
    int v = dc0 >> 3;
    for (int j = 0; j < 8; j++)
        memset(buf + dst + j * VP8_BPS, v, 8);
}

static void vp8_dc8uv_no_top_left(uint8_t *buf, int dst)
{
    for (int j = 0; j < 8; j++)
        memset(buf + dst + j * VP8_BPS, 128, 8);
}

static void vp8_ve8uv(uint8_t *buf, int dst)
{
    int top = dst - VP8_BPS;
    for (int j = 0; j < 8; j++)
        memcpy(buf + dst + j * VP8_BPS, buf + top, 8);
}

static void vp8_he8uv(uint8_t *buf, int dst)
{
    for (int j = 0; j < 8; j++)
    {
        uint8_t v = buf[dst - 1 + j * VP8_BPS];
        memset(buf + dst + j * VP8_BPS, v, 8);
    }
}

static const VP8PredFn VP8_PRED_CHROMA8[7] = {
    vp8_dc8uv, vp8_tm8, vp8_ve8uv, vp8_he8uv,
    vp8_dc8uv_no_top, vp8_dc8uv_no_left, vp8_dc8uv_no_top_left
};

static void vp8_dc4(uint8_t *buf, int dst)
{
    int dc = 4;
    for (int i = 0; i < 4; i++)
        dc += buf[dst - VP8_BPS + i] + buf[dst - 1 + i * VP8_BPS];
    dc >>= 3;
    for (int j = 0; j < 4; j++)
        memset(buf + dst + j * VP8_BPS, dc, 4);
}

static void vp8_ve4(uint8_t *buf, int dst)
{
    int top = dst - VP8_BPS;
    uint8_t v0 = (uint8_t)vp8_avg3(buf[top - 1], buf[top], buf[top + 1]);
    uint8_t v1 = (uint8_t)vp8_avg3(buf[top], buf[top + 1], buf[top + 2]);
    uint8_t v2 = (uint8_t)vp8_avg3(buf[top + 1], buf[top + 2], buf[top + 3]);
    uint8_t v3 = (uint8_t)vp8_avg3(buf[top + 2], buf[top + 3], buf[top + 4]);
    for (int j = 0; j < 4; j++)
    {
        int r = dst + j * VP8_BPS;
        buf[r] = v0; buf[r + 1] = v1; buf[r + 2] = v2; buf[r + 3] = v3;
    }
}

static void vp8_he4(uint8_t *buf, int dst)
{
    int a = buf[dst - 1 - VP8_BPS];
    int b = buf[dst - 1];
    int c = buf[dst - 1 + VP8_BPS];
    int d = buf[dst - 1 + 2 * VP8_BPS];
    int e = buf[dst - 1 + 3 * VP8_BPS];
    uint8_t w0 = (uint8_t)vp8_avg3(a, b, c);
    uint8_t w1 = (uint8_t)vp8_avg3(b, c, d);
    uint8_t w2 = (uint8_t)vp8_avg3(c, d, e);
    uint8_t w3 = (uint8_t)vp8_avg3(d, e, e);
    /* Rows are written in the reference's order: row3 first. */
    memset(buf + dst + 3 * VP8_BPS, w3, 4);
    memset(buf + dst + 2 * VP8_BPS, w2, 4);
    memset(buf + dst + 1 * VP8_BPS, w1, 4);
    memset(buf + dst, w0, 4);
}

/* rd4 (Down-Right): rows (C+R*32 layout, R0 = top):
 *   R0: avg3(a,x,i) avg3(b,a,x) avg3(c,b,a) avg3(d,c,b)
 *   R1: avg3(x,i,j) avg3(a,x,i) avg3(b,a,x) avg3(c,b,a)
 *   R2: avg3(i,j,k) avg3(x,i,j) avg3(a,x,i) avg3(b,a,x)
 *   R3: avg3(j,k,l) avg3(i,j,k) avg3(x,i,j) avg3(a,x,i) */
static void vp8_rd4(uint8_t *buf, int dst)
{
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    buf[dst + 3] = (uint8_t)vp8_avg3(d, c, b);
    buf[dst + 2] = (uint8_t)vp8_avg3(c, b, a);
    buf[dst + 1] = (uint8_t)vp8_avg3(b, a, x);
    buf[dst + 0] = (uint8_t)vp8_avg3(a, x, i);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(c, b, a);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg3(b, a, x);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(a, x, i);
    buf[dst + VP8_BPS + 0] = (uint8_t)vp8_avg3(x, i, j);
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)vp8_avg3(b, a, x);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)vp8_avg3(a, x, i);
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg3(x, i, j);
    buf[dst + 2 * VP8_BPS + 0] = (uint8_t)vp8_avg3(i, j, k);
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)vp8_avg3(a, x, i);
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)vp8_avg3(x, i, j);
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)vp8_avg3(i, j, k);
    buf[dst + 3 * VP8_BPS + 0] = (uint8_t)vp8_avg3(j, k, l);
}

/* ld4 (Down-Left):
 *   R0: avg3(a,b,c) avg3(b,c,d) avg3(c,d,e) avg3(d,e,f)
 *   R1: avg3(b,c,d) avg3(c,d,e) avg3(d,e,f) avg3(e,f,g)
 *   R2: avg3(c,d,e) avg3(d,e,f) avg3(e,f,g) avg3(f,g,h)
 *   R3: avg3(d,e,f) avg3(e,f,g) avg3(f,g,h) avg3(g,h,h) */
static void vp8_ld4(uint8_t *buf, int dst)
{
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    int e = buf[dst - VP8_BPS + 4];
    int f = buf[dst - VP8_BPS + 5];
    int g = buf[dst - VP8_BPS + 6];
    int h = buf[dst - VP8_BPS + 7];
    buf[dst] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + 1] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + 2] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + 3] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + VP8_BPS] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 2 * VP8_BPS] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)vp8_avg3(f, g, h);
    buf[dst + 3 * VP8_BPS] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)vp8_avg3(f, g, h);
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)vp8_avg3(g, h, h);
}

/* vr4 (Vertical-Right):
 *   R0: avg2(x,a)   avg2(a,b)   avg2(b,c)   avg2(c,d)
 *   R1: avg3(i,x,a) avg3(x,a,b) avg3(a,b,c) avg3(b,c,d)
 *   R2: avg3(j,i,x) avg2(a,b)   avg2(b,c)   avg2(c,d)
 *   R3: avg3(k,j,i) avg3(i,x,a) avg3(x,a,b) avg3(a,b,c) */
static void vp8_vr4(uint8_t *buf, int dst)
{
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    buf[dst] = (uint8_t)vp8_avg2(x, a);
    buf[dst + 1] = (uint8_t)vp8_avg2(a, b);
    buf[dst + 2] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 3] = (uint8_t)vp8_avg2(c, d);
    buf[dst + VP8_BPS] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(x, a, b);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + 2 * VP8_BPS] = (uint8_t)vp8_avg3(j, i, x);
    /* Row 2 repeats row 0's first three values starting at avg2(x,a):
     * canonical VP8 VR (RFC 6386) — verified against the reference. */
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg2(x, a);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)vp8_avg2(a, b);
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 3 * VP8_BPS] = (uint8_t)vp8_avg3(k, j, i);
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)vp8_avg3(x, a, b);
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)vp8_avg3(a, b, c);
}

/* vl4 (Vertical-Left):
 *   R0: avg2(a,b)   avg2(b,c)   avg2(c,d)   avg2(d,e)
 *   R1: avg3(a,b,c) avg3(b,c,d) avg3(c,d,e) avg3(d,e,f)
 *   R2: avg2(b,c)   avg2(c,d)   avg2(d,e)   avg3(e,f,g)
 *   R3: avg3(b,c,d) avg3(c,d,e) avg3(d,e,f) avg3(f,g,h) */
static void vp8_vl4(uint8_t *buf, int dst)
{
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    int d = buf[dst - VP8_BPS + 3];
    int e = buf[dst - VP8_BPS + 4];
    int f = buf[dst - VP8_BPS + 5];
    int g = buf[dst - VP8_BPS + 6];
    int h = buf[dst - VP8_BPS + 7];
    buf[dst] = (uint8_t)vp8_avg2(a, b);
    buf[dst + 1] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 2] = (uint8_t)vp8_avg2(c, d);
    buf[dst + 3] = (uint8_t)vp8_avg2(d, e);
    buf[dst + VP8_BPS] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 2 * VP8_BPS] = (uint8_t)vp8_avg2(b, c);
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg2(c, d);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)vp8_avg2(d, e);
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)vp8_avg3(e, f, g);
    buf[dst + 3 * VP8_BPS] = (uint8_t)vp8_avg3(b, c, d);
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)vp8_avg3(c, d, e);
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)vp8_avg3(d, e, f);
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)vp8_avg3(f, g, h);
}

/* hu4 (Horizontal-Up):
 *   R0: avg2(i,j)   avg3(i,j,k) avg2(j,k)   avg3(j,k,l)
 *   R1: avg2(j,k)   avg3(j,k,l) avg2(k,l)   avg3(k,l,l)
 *   R2: avg2(k,l)   avg3(k,l,l) l           l
 *   R3: l           l           l           l */
static void vp8_hu4(uint8_t *buf, int dst)
{
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    buf[dst] = (uint8_t)vp8_avg2(i, j);
    buf[dst + 1] = (uint8_t)vp8_avg3(i, j, k);
    buf[dst + 2] = (uint8_t)vp8_avg2(j, k);
    buf[dst + 3] = (uint8_t)vp8_avg3(j, k, l);
    buf[dst + VP8_BPS] = (uint8_t)vp8_avg2(j, k);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(j, k, l);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg2(k, l);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(k, l, l);
    buf[dst + 2 * VP8_BPS] = (uint8_t)vp8_avg2(k, l);
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg3(k, l, l);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)l;
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)l;
    buf[dst + 3 * VP8_BPS] = (uint8_t)l;
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)l;
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)l;
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)l;
}

/* hd4 (Horizontal-Down):
 *   R0: avg2(i,x)   avg3(i,x,a) avg3(x,a,b) avg3(a,b,c)
 *   R1: avg2(j,i)   avg3(j,i,x) avg2(i,x)   avg3(i,x,a)
 *   R2: avg2(k,j)   avg3(k,j,i) avg2(j,i)   avg3(j,i,x)
 *   R3: avg2(l,k)   avg3(l,k,j) avg2(k,j)   avg3(k,j,i) */
static void vp8_hd4(uint8_t *buf, int dst)
{
    int i = buf[dst - 1];
    int j = buf[dst - 1 + VP8_BPS];
    int k = buf[dst - 1 + 2 * VP8_BPS];
    int l = buf[dst - 1 + 3 * VP8_BPS];
    int x = buf[dst - 1 - VP8_BPS];
    int a = buf[dst - VP8_BPS];
    int b = buf[dst - VP8_BPS + 1];
    int c = buf[dst - VP8_BPS + 2];
    buf[dst] = (uint8_t)vp8_avg2(i, x);
    buf[dst + 1] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + 2] = (uint8_t)vp8_avg3(x, a, b);
    buf[dst + 3] = (uint8_t)vp8_avg3(a, b, c);
    buf[dst + VP8_BPS] = (uint8_t)vp8_avg2(j, i);
    buf[dst + VP8_BPS + 1] = (uint8_t)vp8_avg3(j, i, x);
    buf[dst + VP8_BPS + 2] = (uint8_t)vp8_avg2(i, x);
    buf[dst + VP8_BPS + 3] = (uint8_t)vp8_avg3(i, x, a);
    buf[dst + 2 * VP8_BPS] = (uint8_t)vp8_avg2(k, j);
    buf[dst + 2 * VP8_BPS + 1] = (uint8_t)vp8_avg3(k, j, i);
    buf[dst + 2 * VP8_BPS + 2] = (uint8_t)vp8_avg2(j, i);
    buf[dst + 2 * VP8_BPS + 3] = (uint8_t)vp8_avg3(j, i, x);
    buf[dst + 3 * VP8_BPS] = (uint8_t)vp8_avg2(l, k);
    buf[dst + 3 * VP8_BPS + 1] = (uint8_t)vp8_avg3(l, k, j);
    buf[dst + 3 * VP8_BPS + 2] = (uint8_t)vp8_avg2(k, j);
    buf[dst + 3 * VP8_BPS + 3] = (uint8_t)vp8_avg3(k, j, i);
}

static void vp8_tm4(uint8_t *buf, int dst) { vp8_true_motion(buf, dst, 4); }

static const VP8PredFn VP8_PRED_LUMA4[10] = {
    vp8_dc4, vp8_tm4, vp8_ve4, vp8_he4,
    vp8_rd4, vp8_vr4, vp8_ld4, vp8_vl4, vp8_hd4, vp8_hu4
};

static int vp8_check_mode(int mbX, int mbY, int mode)
{
    if (mode == VP8_DC_PRED)
    {
        if (mbX == 0)
        {
            if (mbY == 0) return 6;
            return 5;
        }
        if (mbY == 0) return 4;
        return 0;
    }
    return mode;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29e — in-loop filtering (webp.lua 1805-2078)
 * ═════════════════════════════════════════════════════════════════════════ */

static void vp8_do_filter2(uint8_t *buf, int p, int step)
{
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int a = 3 * (q0 - p0) + vp8_ksclip1(p1 - q1);
    int a1 = vp8_ksclip2(vp8_sar(a + 4, 3));
    int a2 = vp8_ksclip2(vp8_sar(a + 3, 3));
    buf[p - step] = vp8_clip8(p0 + a2);
    buf[p] = vp8_clip8(q0 - a1);
}

static void vp8_do_filter4(uint8_t *buf, int p, int step)
{
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int a = 3 * (q0 - p0);
    int a1 = vp8_ksclip2(vp8_sar(a + 4, 3));
    int a2 = vp8_ksclip2(vp8_sar(a + 3, 3));
    int a3 = vp8_sar(a1 + 1, 1);
    buf[p - 2 * step] = vp8_clip8(p1 + a3);
    buf[p - step] = vp8_clip8(p0 + a2);
    buf[p] = vp8_clip8(q0 - a1);
    buf[p + step] = vp8_clip8(q1 - a3);
}

static void vp8_do_filter6(uint8_t *buf, int p, int step)
{
    int p2 = buf[p - 3 * step];
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int q2 = buf[p + 2 * step];
    int a = vp8_ksclip1(3 * (q0 - p0) + vp8_ksclip1(p1 - q1));
    int a1 = vp8_sar(27 * a + 63, 7);
    int a2 = vp8_sar(18 * a + 63, 7);
    int a3 = vp8_sar(9 * a + 63, 7);
    buf[p - 3 * step] = vp8_clip8(p2 + a3);
    buf[p - 2 * step] = vp8_clip8(p1 + a2);
    buf[p - step] = vp8_clip8(p0 + a1);
    buf[p] = vp8_clip8(q0 - a1);
    buf[p + step] = vp8_clip8(q1 - a2);
    buf[p + 2 * step] = vp8_clip8(q2 - a3);
}

static int vp8_hev(uint8_t *buf, int p, int step, int thresh)
{
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    return (vp8_kabs0(p1 - p0) > thresh) || (vp8_kabs0(q1 - q0) > thresh);
}

static int vp8_needs_filter(uint8_t *buf, int p, int step, int t)
{
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    return (4 * vp8_kabs0(p0 - q0) + vp8_kabs0(p1 - q1)) <= t;
}

static int vp8_needs_filter2(uint8_t *buf, int p, int step, int t, int it)
{
    int p3 = buf[p - 4 * step];
    int p2 = buf[p - 3 * step];
    int p1 = buf[p - 2 * step];
    int p0 = buf[p - step];
    int q0 = buf[p];
    int q1 = buf[p + step];
    int q2 = buf[p + 2 * step];
    int q3 = buf[p + 3 * step];
    if ((4 * vp8_kabs0(p0 - q0) + vp8_kabs0(p1 - q1)) > t) return 0;
    return vp8_kabs0(p3 - p2) <= it && vp8_kabs0(p2 - p1) <= it && vp8_kabs0(p1 - p0) <= it &&
           vp8_kabs0(q3 - q2) <= it && vp8_kabs0(q2 - q1) <= it && vp8_kabs0(q1 - q0) <= it;
}

static void vp8_filter_loop26(uint8_t *buf, int p, int hstride, int vstride, int size,
                              int thresh, int ithresh, int hevThresh)
{
    int t = 2 * thresh + 1;
    for (int i = 0; i < size; i++)
    {
        if (vp8_needs_filter2(buf, p, hstride, t, ithresh))
        {
            if (vp8_hev(buf, p, hstride, hevThresh))
                vp8_do_filter2(buf, p, hstride);
            else
                vp8_do_filter6(buf, p, hstride);
        }
        p += vstride;
    }
}

static void vp8_filter_loop24(uint8_t *buf, int p, int hstride, int vstride, int size,
                              int thresh, int ithresh, int hevThresh)
{
    int t = 2 * thresh + 1;
    for (int i = 0; i < size; i++)
    {
        if (vp8_needs_filter2(buf, p, hstride, t, ithresh))
        {
            if (vp8_hev(buf, p, hstride, hevThresh))
                vp8_do_filter2(buf, p, hstride);
            else
                vp8_do_filter4(buf, p, hstride);
        }
        p += vstride;
    }
}

static void vp8_v_filter16(uint8_t *buf, int p, int stride, int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop26(buf, p, stride, 1, 16, thresh, ithresh, hevThresh);
}

static void vp8_h_filter16(uint8_t *buf, int p, int stride, int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop26(buf, p, 1, stride, 16, thresh, ithresh, hevThresh);
}

static void vp8_v_filter16i(uint8_t *buf, int p, int stride, int thresh, int ithresh, int hevThresh)
{
    for (int k = 1; k <= 3; k++)
    {
        p += 4 * stride;
        vp8_filter_loop24(buf, p, stride, 1, 16, thresh, ithresh, hevThresh);
    }
}

static void vp8_h_filter16i(uint8_t *buf, int p, int stride, int thresh, int ithresh, int hevThresh)
{
    for (int k = 1; k <= 3; k++)
    {
        p += 4;
        vp8_filter_loop24(buf, p, 1, stride, 16, thresh, ithresh, hevThresh);
    }
}

static void vp8_v_filter8(uint8_t *bufU, int u, uint8_t *bufV, int v, int stride,
                          int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop26(bufU, u, stride, 1, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop26(bufV, v, stride, 1, 8, thresh, ithresh, hevThresh);
}

static void vp8_v_filter8i(uint8_t *bufU, int u, uint8_t *bufV, int v, int stride,
                           int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop24(bufU, u + 4 * stride, stride, 1, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop24(bufV, v + 4 * stride, stride, 1, 8, thresh, ithresh, hevThresh);
}

static void vp8_h_filter8(uint8_t *bufU, int u, uint8_t *bufV, int v, int stride,
                          int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop26(bufU, u, 1, stride, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop26(bufV, v, 1, stride, 8, thresh, ithresh, hevThresh);
}

static void vp8_h_filter8i(uint8_t *bufU, int u, uint8_t *bufV, int v, int stride,
                           int thresh, int ithresh, int hevThresh)
{
    vp8_filter_loop24(bufU, u + 4, 1, stride, 8, thresh, ithresh, hevThresh);
    vp8_filter_loop24(bufV, v + 4, 1, stride, 8, thresh, ithresh, hevThresh);
}

static void vp8_simple_v_filter16(uint8_t *buf, int p, int stride, int thresh)
{
    int t = 2 * thresh + 1;
    for (int i = 0; i < 16; i++)
    {
        int pp = p + i;
        if (vp8_needs_filter(buf, pp, stride, t)) vp8_do_filter2(buf, pp, stride);
    }
}

static void vp8_simple_h_filter16(uint8_t *buf, int p, int stride, int thresh)
{
    int t = 2 * thresh + 1;
    for (int i = 0; i < 16; i++)
    {
        int pp = p + i * stride;
        if (vp8_needs_filter(buf, pp, 1, t)) vp8_do_filter2(buf, pp, 1);
    }
}

static void vp8_simple_v_filter16i(uint8_t *buf, int p, int stride, int thresh)
{
    for (int k = 1; k <= 3; k++)
    {
        p += 4 * stride;
        vp8_simple_v_filter16(buf, p, stride, thresh);
    }
}

static void vp8_simple_h_filter16i(uint8_t *buf, int p, int stride, int thresh)
{
    for (int k = 1; k <= 3; k++)
    {
        p += 4;
        vp8_simple_h_filter16(buf, p, stride, thresh);
    }
}

static void vp8_do_filter(VP8Dec *dec, int mbX, int mbY)
{
    VP8Mb *block = &dec->mbData[mbX];
    int yBps = dec->cacheYStride;
    int yDst = dec->extra * yBps + mbX * 16;
    int ilevel = block->fIlevel;
    int limit = block->fLimit;
    if (limit == 0) return;
    if (dec->filterType == 1)
    {
        if (mbX > 0) vp8_simple_h_filter16(dec->cacheY, yDst, yBps, limit + 4);
        if (block->fInner == 1) vp8_simple_h_filter16i(dec->cacheY, yDst, yBps, limit);
        if (mbY > 0) vp8_simple_v_filter16(dec->cacheY, yDst, yBps, limit + 4);
        if (block->fInner == 1) vp8_simple_v_filter16i(dec->cacheY, yDst, yBps, limit);
    }
    else
    {
        int uvBps = dec->cacheUvStride;
        int uDst = dec->extraUV * uvBps + mbX * 8;
        int vDst = uDst;
        int hevThresh = block->hevThresh;
        if (mbX > 0)
        {
            vp8_h_filter16(dec->cacheY, yDst, yBps, limit + 4, ilevel, hevThresh);
            vp8_h_filter8(dec->cacheU, uDst, dec->cacheV, vDst, uvBps, limit + 4, ilevel, hevThresh);
        }
        if (block->fInner == 1)
        {
            vp8_h_filter16i(dec->cacheY, yDst, yBps, limit, ilevel, hevThresh);
            vp8_h_filter8i(dec->cacheU, uDst, dec->cacheV, vDst, uvBps, limit, ilevel, hevThresh);
        }
        if (mbY > 0)
        {
            vp8_v_filter16(dec->cacheY, yDst, yBps, limit + 4, ilevel, hevThresh);
            vp8_v_filter8(dec->cacheU, uDst, dec->cacheV, vDst, uvBps, limit + 4, ilevel, hevThresh);
        }
        if (block->fInner == 1)
        {
            vp8_v_filter16i(dec->cacheY, yDst, yBps, limit, ilevel, hevThresh);
            vp8_v_filter8i(dec->cacheU, uDst, dec->cacheV, vDst, uvBps, limit, ilevel, hevThresh);
        }
    }
}

static void vp8_precompute_filter_strengths(VP8Dec *dec)
{
    if (dec->filterType <= 0) return;
    VP8FilterHdr *hdr = &dec->filterHdr;
    VP8SegmentHdr *segHdr = &dec->segmentHdr;
    for (int s = 0; s < 4; s++)
    {
        int baseLevel;
        if (segHdr->useSegment != 0)
        {
            baseLevel = segHdr->filterStrength[s];
            if (segHdr->absoluteDelta == 0) baseLevel += hdr->level;
        }
        else
        {
            baseLevel = hdr->level;
        }
        for (int i4x4 = 0; i4x4 < 2; i4x4++)
        {
            int level = baseLevel;
            if (hdr->useLfDelta != 0)
            {
                level += hdr->refLfDelta[0];
                if (i4x4 == 1) level += hdr->modeLfDelta[0];
            }
            if (level < 0) level = 0; else if (level > 63) level = 63;
            VP8FilterStrength *fs = &dec->fstrengths[s][i4x4];
            fs->fLimit = 0;
            fs->fIlevel = 0;
            fs->fInner = i4x4;
            fs->hevThresh = 0;
            if (level > 0)
            {
                int ilevel = level;
                if (hdr->sharpness > 0)
                {
                    if (hdr->sharpness > 4)
                        ilevel >>= 2;
                    else
                        ilevel >>= 1;
                    if (ilevel > 9 - hdr->sharpness) ilevel = 9 - hdr->sharpness;
                }
                if (ilevel < 1) ilevel = 1;
                fs->fIlevel = ilevel;
                fs->fLimit = 2 * level + ilevel;
                if (level >= 40)
                    fs->hevThresh = 2;
                else if (level >= 15)
                    fs->hevThresh = 1;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29f — coefficient decoding (webp.lua 2080-2216)
 * ═════════════════════════════════════════════════════════════════════════ */

static int vp8_get_large_value(VP8BoolBr *br, const uint8_t *proba, int p)
{
    int v;
    if (vp8_get_bit(br, proba[p + 3]) == 0)
    {
        if (vp8_get_bit(br, proba[p + 4]) == 0)
            v = 2;
        else
            v = 3 + vp8_get_bit(br, proba[p + 5]);
    }
    else if (vp8_get_bit(br, proba[p + 6]) == 0)
    {
        if (vp8_get_bit(br, proba[p + 7]) == 0)
            v = 5 + vp8_get_bit(br, 159);
        else
        {
            v = 7 + 2 * vp8_get_bit(br, 165);
            v += vp8_get_bit(br, 145);
        }
    }
    else
    {
        int bit1 = vp8_get_bit(br, proba[p + 8]);
        int bit0 = vp8_get_bit(br, proba[p + 9 + bit1]);
        int cat = 2 * bit1 + bit0;
        v = 0;
        const uint8_t *tab = vp8_cat3456[cat];
        for (int i = 0; i < VP8_CAT3456_ROW; i++)
        {
            int prob = tab[i];
            if (prob == 0) break;
            v = v + v + vp8_get_bit(br, prob);
        }
        v = v + 3 + (8 << cat);
    }
    return v;
}

/* dq = { DC, AC }; proba is a flat 0-based 4*8*3*11 table. */
static int vp8_get_coeffs(VP8BoolBr *br, const uint8_t *proba, int t, int n,
                          int ctx, const int *dq, int16_t *out, int outOff)
{
    int p = ((t * 8 + vp8_bands[n]) * 3 + ctx) * 11;
    while (n < 16)
    {
        if (vp8_get_bit(br, proba[p]) == 0) return n;
        while (vp8_get_bit(br, proba[p + 1]) == 0)
        {
            n++;
            if (n == 16) return 16;
            p = (t * 8 + vp8_bands[n]) * 33;
        }
        int v;
        if (vp8_get_bit(br, proba[p + 2]) == 0)
        {
            v = 1;
            p = (t * 8 + vp8_bands[n + 1]) * 33 + 11;
        }
        else
        {
            v = vp8_get_large_value(br, proba, p);
            p = (t * 8 + vp8_bands[n + 1]) * 33 + 22;
        }
        out[outOff + vp8_zigzag[n]] = (int16_t)(vp8_get_signed(br, v) * dq[(n > 0) ? 1 : 0]);
        n++;
    }
    return 16;
}

static int vp8_parse_residuals(VP8Dec *dec, VP8BoolBr *tokenBr)
{
    int mbX = dec->mbX;
    VP8MbInfo *mb = &dec->mbInfo[mbX + 1];
    VP8MbInfo *leftMb = &dec->mbInfo[0];
    VP8Mb *block = &dec->mbData[mbX];
    int16_t *dst = block->coeffs;
    memset(dst, 0, sizeof(int16_t) * 384);
    int nonZeroY = 0;
    uint32_t nonZeroUv = 0;
    int first;
    int outOff = 0;
    if (block->isI4x4 == 0)
    {
        int16_t dc[16] = {0};
        int ctx = mb->nzDc + leftMb->nzDc;
        int nz = vp8_get_coeffs(tokenBr, dec->proba, 1, 0, ctx, dec->dqm[block->segment].y2, dc, 0);
        mb->nzDc = (nz > 0) ? 1 : 0;
        leftMb->nzDc = mb->nzDc;
        if (nz > 1)
        {
            vp8_transform_wht(dc, 0, dst, 0);
        }
        else
        {
            int dc0 = vp8_sar(dc[0] + 3, 3);
            for (int i = 0; i < 16; i++) dst[i * 16] = (int16_t)dc0;
        }
        first = 1;
    }
    else
    {
        first = 0;
    }
    int acT = (block->isI4x4 == 0) ? 0 : 3;
    int tnz = mb->nz & 0x0f;
    int lnz = leftMb->nz & 0x0f;
    for (int y = 0; y < 4; y++)
    {
        int l = lnz & 1;
        int nzCoeffs = 0;
        for (int x = 0; x < 4; x++)
        {
            int ctx = l + (tnz & 1);
            int nz = vp8_get_coeffs(tokenBr, dec->proba, acT, first, ctx,
                                    dec->dqm[block->segment].y1, dst, outOff);
            l = (nz > first) ? 1 : 0;
            tnz = ((tnz >> 1) | (l << 7)) & 0xff;
            int code = 0;
            if (nz > 3) code = 3; else if (nz > 1) code = 2; else if (dst[outOff] != 0) code = 1;
            nzCoeffs = ((nzCoeffs << 2) | code) & 0xff;
            outOff += 16;
        }
        tnz >>= 4;
        lnz = ((lnz >> 1) | (l << 7)) & 0xff;
        nonZeroY = ((nonZeroY << 8) | nzCoeffs) & 0xffffffff;
    }
    int outTNz = tnz;
    int outLNz = lnz >> 4;
    for (int ch = 0; ch <= 2; ch += 2)
    {
        int nzCoeffs = 0;
        tnz = (mb->nz >> (4 + ch)) & 0xff;
        lnz = (leftMb->nz >> (4 + ch)) & 0xff;
        for (int y = 0; y < 2; y++)
        {
            int l = lnz & 1;
            for (int x = 0; x < 2; x++)
            {
                int ctx = l + (tnz & 1);
                int nz = vp8_get_coeffs(tokenBr, dec->proba, 2, 0, ctx,
                                        dec->dqm[block->segment].uv, dst, outOff);
                l = (nz > 0) ? 1 : 0;
                tnz = ((tnz >> 1) | (l << 3)) & 0xff;
                int code = 0;
                if (nz > 3) code = 3; else if (nz > 1) code = 2; else if (dst[outOff] != 0) code = 1;
                nzCoeffs = ((nzCoeffs << 2) | code) & 0xff;
                outOff += 16;
            }
            tnz >>= 2;
            lnz = ((lnz >> 1) | (l << 5)) & 0xff;
        }
        nonZeroUv = (nonZeroUv | ((uint32_t)nzCoeffs << (4 * ch))) & 0xffffffff;
        outTNz |= ((tnz << 4) << ch) & 0xff;
        outLNz |= ((lnz & 0xf0) << ch) & 0xff;
    }
    mb->nz = outTNz & 0xff;
    leftMb->nz = outLNz & 0xff;
    block->nonZeroY = (uint32_t)nonZeroY;
    block->nonZeroUv = nonZeroUv;
    block->dither = 0;
    return ((nonZeroY | (int)nonZeroUv) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29g — intra mode parsing (webp.lua 2217-2302)
 * ═════════════════════════════════════════════════════════════════════════ */

static void vp8_parse_intra_mode(VP8BoolBr *br, VP8Dec *dec, int mbX)
{
    int topBase = mbX * 4;
    VP8Mb *block = &dec->mbData[mbX];
    if (dec->segmentHdr.updateMap != 0)
    {
        if (vp8_get_bit(br, dec->probaSegments[0]) == 0)
            block->segment = vp8_get_bit(br, dec->probaSegments[1]);
        else
            block->segment = vp8_get_bit(br, dec->probaSegments[2]) + 2;
    }
    else
    {
        block->segment = 0;
    }
    if (dec->useSkipProba != 0)
        block->skip = vp8_get_bit(br, dec->skipP);
    block->isI4x4 = (vp8_get_bit(br, 145) == 0) ? 1 : 0;
    if (block->isI4x4 == 0)
    {
        int ymode;
        if (vp8_get_bit(br, 156) != 0)
        {
            if (vp8_get_bit(br, 128) != 0) ymode = VP8_TM_PRED; else ymode = VP8_H_PRED;
        }
        else
        {
            if (vp8_get_bit(br, 163) != 0) ymode = VP8_V_PRED; else ymode = VP8_DC_PRED;
        }
        block->imodes[0] = (uint8_t)ymode;
        for (int i = 0; i < 4; i++)
        {
            dec->intraT[topBase + i] = (uint8_t)ymode;
            dec->intraL[i] = (uint8_t)ymode;
        }
    }
    else
    {
        for (int y = 0; y < 4; y++)
        {
            int ymode = dec->intraL[y];
            for (int x = 0; x < 4; x++)
            {
                int base = (dec->intraT[topBase + x] * 10 + ymode) * 9;
                if (vp8_get_bit(br, vp8_bmodes_proba[base]) == 0)
                    ymode = 0;
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 1]) == 0)
                    ymode = 1;
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 2]) == 0)
                    ymode = 2;
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 3]) == 0)
                {
                    if (vp8_get_bit(br, vp8_bmodes_proba[base + 4]) == 0)
                        ymode = 3;
                    else if (vp8_get_bit(br, vp8_bmodes_proba[base + 5]) == 0)
                        ymode = 4;
                    else
                        ymode = 5;
                }
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 6]) == 0)
                    ymode = 6;
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 7]) == 0)
                    ymode = 7;
                else if (vp8_get_bit(br, vp8_bmodes_proba[base + 8]) == 0)
                    ymode = 8;
                else
                    ymode = 9;
                dec->intraT[topBase + x] = (uint8_t)ymode;
                block->imodes[y * 4 + x] = (uint8_t)ymode;
            }
            dec->intraL[y] = (uint8_t)ymode;
        }
    }
    if (vp8_get_bit(br, 142) == 0)
        block->uvMode = VP8_DC_PRED;
    else if (vp8_get_bit(br, 114) == 0)
        block->uvMode = VP8_V_PRED;
    else
    {
        if (vp8_get_bit(br, 183) != 0) block->uvMode = VP8_TM_PRED; else block->uvMode = VP8_H_PRED;
    }
}

static int vp8_parse_intra_mode_row(VP8BoolBr *br, VP8Dec *dec)
{
    for (int mbX = 0; mbX < dec->mbW; mbX++)
        vp8_parse_intra_mode(br, dec, mbX);
    return br->eof == 0;
}

static void vp8_init_scanline(VP8Dec *dec)
{
    VP8MbInfo *leftMb = &dec->mbInfo[0];
    leftMb->nz = 0;
    leftMb->nzDc = 0;
    for (int i = 0; i < 4; i++) dec->intraL[i] = 0;
    dec->mbX = 0;
}

static int vp8_decode_mb(VP8Dec *dec, VP8BoolBr *tokenBr)
{
    int mbX = dec->mbX;
    VP8MbInfo *leftMb = &dec->mbInfo[0];
    VP8MbInfo *mb = &dec->mbInfo[mbX + 1];
    VP8Mb *block = &dec->mbData[mbX];
    int skip = (dec->useSkipProba != 0) ? block->skip : 0;
    if (skip == 0)
    {
        skip = vp8_parse_residuals(dec, tokenBr) ? 1 : 0;
    }
    else
    {
        leftMb->nz = 0;
        mb->nz = 0;
        if (block->isI4x4 == 0)
        {
            leftMb->nzDc = 0;
            mb->nzDc = 0;
        }
        block->nonZeroY = 0;
        block->nonZeroUv = 0;
        block->dither = 0;
    }
    if (dec->filterType > 0)
    {
        VP8FilterStrength *fs = &dec->fstrengths[block->segment][block->isI4x4];
        block->fLimit = fs->fLimit;
        block->fIlevel = fs->fIlevel;
        block->hevThresh = fs->hevThresh;
        block->fInner = (fs->fInner != 0 || skip == 0) ? 1 : 0;
    }
    return tokenBr->eof == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29h — reconstruction (webp.lua 2332-2438)
 * ═════════════════════════════════════════════════════════════════════════ */

static uint8_t VP8_YARR[640];
/* Chroma work arrays get a 4-byte leading pad: the reference's uArr/vArr
 * tables are indexed down to -4 (the mbX>0 shift-copy) and -1 IS read as
 * the above-left sample by TM/edge predictors. Lua's phantom keys are
 * behaviorally real here — the pad makes them legal memory in C. */
#define VP8_UVPAD 4
static uint8_t VP8_UARR_MEM[320 + VP8_UVPAD];
static uint8_t VP8_VARR_MEM[320 + VP8_UVPAD];
#define VP8_UARR (VP8_UARR_MEM + VP8_UVPAD)
#define VP8_VARR (VP8_VARR_MEM + VP8_UVPAD)

static void vp8_reconstruct_row(VP8Dec *dec, int mbY)
{
    int mbW = dec->mbW;
    int mbH = dec->mbH;
    int yBps = dec->cacheYStride;
    int uvBps = dec->cacheUvStride;
    uint8_t *cacheY = dec->cacheY;
    uint8_t *cacheU = dec->cacheU;
    uint8_t *cacheV = dec->cacheV;
    uint8_t *yArr = VP8_YARR, *uArr = VP8_UARR, *vArr = VP8_VARR;
    for (int j = 0; j < 16; j++) yArr[VP8_YBASE + j * 32 - 1] = 129;
    for (int j = 0; j < 8; j++)
    {
        uArr[VP8_UBASE + j * 32 - 1] = 129;
        vArr[VP8_VBASE + j * 32 - 1] = 129;
    }
    if (mbY > 0)
    {
        yArr[VP8_YBASE - 33] = 129;
        uArr[VP8_UBASE - 33] = 129; /* phantom key -1, read as TM topleft */
        vArr[VP8_VBASE - 33] = 129;
    }
    else
    {
        for (int x = -1; x <= 19; x++) yArr[VP8_YBASE - 32 + x] = 127;
        for (int x = -1; x <= 7; x++)
        {
            uArr[VP8_UBASE - 32 + x] = 127;
            vArr[VP8_VBASE - 32 + x] = 127;
        }
    }
    for (int mbX = 0; mbX < mbW; mbX++)
    {
        VP8Mb *block = &dec->mbData[mbX];
        const int16_t *coeffs = block->coeffs;
        VP8TopYuv *topYuv = &dec->yuvT[mbX];
        if (mbX > 0)
        {
            for (int j = -1; j <= 15; j++)
                for (int i = 0; i < 4; i++)
                    yArr[VP8_YBASE + j * 32 - 4 + i] = yArr[VP8_YBASE + j * 32 + 12 + i];
            for (int j = -1; j <= 7; j++)
                for (int i = 0; i < 4; i++)
                {
                    /* j == -1 lands at dst -4..-1 — legal via VP8_UVPAD;
                     * index -1 is the TM above-left sample the Lua
                     * reference really does maintain through this copy. */
                    uArr[VP8_UBASE + j * 32 - 4 + i] = uArr[VP8_UBASE + j * 32 + 4 + i];
                    vArr[VP8_VBASE + j * 32 - 4 + i] = vArr[VP8_VBASE + j * 32 + 4 + i];
                }
        }
        if (mbY > 0)
        {
            for (int i = 0; i < 16; i++) yArr[VP8_YBASE - 32 + i] = topYuv->y[i];
            for (int i = 0; i < 8; i++)
            {
                uArr[VP8_UBASE - 32 + i] = topYuv->u[i];
                vArr[VP8_VBASE - 32 + i] = topYuv->v[i];
            }
        }
        if (block->isI4x4 != 0)
        {
            if (mbY > 0)
            {
                if (mbX >= mbW - 1)
                {
                    uint8_t vv = topYuv->y[15]; /* Lua y[16], 1-based */
                    for (int i = 0; i < 4; i++) yArr[VP8_YBASE - 32 + 16 + i] = vv;
                }
                else
                {
                    VP8TopYuv *nxt = &dec->yuvT[mbX + 1];
                    for (int i = 0; i < 4; i++) yArr[VP8_YBASE - 32 + 16 + i] = nxt->y[i];
                }
            }
            for (int r = 1; r <= 3; r++)
                for (int i = 0; i < 4; i++)
                    yArr[VP8_YBASE - 32 + 16 + r * 128 + i] = yArr[VP8_YBASE - 32 + 16 + i];
            uint32_t bits = block->nonZeroY;
            for (int n = 0; n < 16; n++)
            {
                int d = VP8_YBASE + VP8_KSCAN[n];
                VP8_PRED_LUMA4[block->imodes[n]](yArr, d);
                vp8_do_transform(bits, coeffs, n * 16, yArr, d);
                bits = (bits << 2) & 0xFFFFFFFFu;
            }
        }
        else
        {
            int pred = vp8_check_mode(mbX, mbY, block->imodes[0]);
            VP8_PRED_LUMA16[pred](yArr, VP8_YBASE);
            uint32_t bits = block->nonZeroY;
            if (bits != 0)
            {
                for (int n = 0; n < 16; n++)
                {
                    vp8_do_transform(bits, coeffs, n * 16, yArr, VP8_YBASE + VP8_KSCAN[n]);
                    bits = (bits << 2) & 0xFFFFFFFFu;
                }
            }
        }
        uint32_t bitsUV = block->nonZeroUv;
        int pred = vp8_check_mode(mbX, mbY, block->uvMode);
        VP8_PRED_CHROMA8[pred](uArr, VP8_UBASE);
        VP8_PRED_CHROMA8[pred](vArr, VP8_VBASE);
        vp8_do_uv_transform(bitsUV & 0xff, coeffs, 256, uArr, VP8_UBASE);
        vp8_do_uv_transform(bitsUV >> 8, coeffs, 320, vArr, VP8_VBASE);
        if (mbY < mbH - 1)
        {
            for (int i = 0; i < 16; i++) topYuv->y[i] = yArr[VP8_YBASE + 480 + i];
            for (int i = 0; i < 8; i++)
            {
                topYuv->u[i] = uArr[VP8_UBASE + 224 + i];
                topYuv->v[i] = vArr[VP8_VBASE + 224 + i];
            }
        }
        int yOut = dec->extra * yBps + mbX * 16;
        int uvOut = dec->extraUV * uvBps + mbX * 8;
        for (int j = 0; j < 16; j++)
        {
            int sr = VP8_YBASE + j * 32;
            int dr = yOut + j * yBps;
            memcpy(cacheY + dr, yArr + sr, 16);
        }
        for (int j = 0; j < 8; j++)
        {
            int sr = VP8_UBASE + j * 32;
            int dr = uvOut + j * uvBps;
            memcpy(cacheU + dr, uArr + sr, 8);
            memcpy(cacheV + dr, vArr + sr, 8);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29i — YUV->RGB upsampling + finishRow (webp.lua 2440-2602)
 * ═════════════════════════════════════════════════════════════════════════ */

static uint8_t vp8_clip_yuv(int v)
{
    if ((v & ~0x3FFF) == 0) return (uint8_t)(v >> 6);
    if (v < 0) return 0;
    return 255;
}

static void vp8_write_rgb_pixel(uint8_t *rgb, int o, int yv, int u, int v)
{
    rgb[o] = vp8_clip_yuv(((yv * 19077) >> 8) + ((v * 26149) >> 8) - 14234);
    rgb[o + 1] = vp8_clip_yuv(((yv * 19077) >> 8) - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708);
    rgb[o + 2] = vp8_clip_yuv(((yv * 19077) >> 8) + ((u * 33050) >> 8) - 17685);
}

static void vp8_upsample_line_pair(const uint8_t *yTopArr, int yTopBase, const uint8_t *yBotArr, int yBotBase,
                                   const uint8_t *uTopArr, int uTopBase, const uint8_t *vTopArr, int vTopBase,
                                   const uint8_t *uBotArr, int uBotBase, const uint8_t *vBotArr, int vBotBase,
                                   uint8_t *rgb, int width, int dstTop, int dstBot)
{
    int lastPair = (width - 1) >> 1;
    int tlU = uTopArr[uTopBase];
    int tlV = vTopArr[vTopBase];
    int lU = uBotArr[uBotBase];
    int lV = vBotArr[vBotBase];
    int o = dstTop * width * 3;
    vp8_write_rgb_pixel(rgb, o, yTopArr[yTopBase], (3 * tlU + lU + 2) >> 2, (3 * tlV + lV + 2) >> 2);
    if (yBotArr)
    {
        int ob = dstBot * width * 3;
        vp8_write_rgb_pixel(rgb, ob, yBotArr[yBotBase], (3 * lU + tlU + 2) >> 2, (3 * lV + tlV + 2) >> 2);
    }
    for (int x = 1; x <= lastPair; x++)
    {
        int tU = uTopArr[uTopBase + x];
        int tV = vTopArr[vTopBase + x];
        int cU = uBotArr[uBotBase + x];
        int cV = vBotArr[vBotBase + x];
        int avgU = tlU + tU + lU + cU + 8;
        int avgV = tlV + tV + lV + cV + 8;
        int d12U = (avgU + 2 * (tU + lU)) >> 3;
        int d03U = (avgU + 2 * (tlU + cU)) >> 3;
        int d12V = (avgV + 2 * (tV + lV)) >> 3;
        int d03V = (avgV + 2 * (tlV + cV)) >> 3;
        int o2 = o + (2 * x - 1) * 3;
        vp8_write_rgb_pixel(rgb, o2, yTopArr[yTopBase + 2 * x - 1], (d12U + tlU) >> 1, (d12V + tlV) >> 1);
        vp8_write_rgb_pixel(rgb, o2 + 3, yTopArr[yTopBase + 2 * x], (d03U + tU) >> 1, (d03V + tV) >> 1);
        if (yBotArr)
        {
            int ob = dstBot * width * 3 + (2 * x - 1) * 3;
            vp8_write_rgb_pixel(rgb, ob, yBotArr[yBotBase + 2 * x - 1], (d03U + lU) >> 1, (d03V + lV) >> 1);
            vp8_write_rgb_pixel(rgb, ob + 3, yBotArr[yBotBase + 2 * x], (d12U + cU) >> 1, (d12V + cV) >> 1);
        }
        tlU = tU; tlV = tV; lU = cU; lV = cV;
    }
    if ((width & 1) == 0)
    {
        int o2 = o + (width - 1) * 3;
        vp8_write_rgb_pixel(rgb, o2, yTopArr[yTopBase + width - 1], (3 * tlU + lU + 2) >> 2, (3 * tlV + lV + 2) >> 2);
        if (yBotArr)
        {
            int ob = dstBot * width * 3 + (width - 1) * 3;
            vp8_write_rgb_pixel(rgb, ob, yBotArr[yBotBase + width - 1], (3 * lU + tlU + 2) >> 2, (3 * lV + tlV + 2) >> 2);
        }
    }
}

static void vp8_finish_row(VP8Dec *dec, int isFirst, int isLast)
{
    int width = dec->width;
    int height = dec->height;
    int mbY = dec->mbY;
    int yStart = mbY * 16;
    int yEnd = yStart + 16;
    if (!isFirst) yStart -= dec->extra;
    if (!isLast) yEnd -= dec->extra;
    if (yEnd > height) yEnd = height;
    if (yStart < yEnd)
    {
        int rowOff = isFirst ? dec->extra : 0;
        int uvOff = isFirst ? dec->extraUV : 0;
        int yBps = dec->cacheYStride;
        int uvBps = dec->cacheUvStride;
        uint8_t *cacheY = dec->cacheY;
        uint8_t *cacheU = dec->cacheU;
        uint8_t *cacheV = dec->cacheV;
        uint8_t *rgb = dec->rgb;
        if (yStart == 0)
        {
            int yBase = rowOff * yBps;
            int uvBase = uvOff * uvBps;
            vp8_upsample_line_pair(cacheY, yBase, NULL, 0,
                                   cacheU, uvBase, cacheV, uvBase,
                                   cacheU, uvBase, cacheV, uvBase,
                                   rgb, width, 0, -1);
        }
        else
        {
            int uvBase = uvOff * uvBps;
            vp8_upsample_line_pair(dec->tmpY, 0, cacheY, rowOff * yBps,
                                   dec->tmpU, 0, dec->tmpV, 0,
                                   cacheU, uvBase, cacheV, uvBase,
                                   rgb, width, yStart - 1, yStart);
        }
        int k = 1;
        while (yStart + 2 * k < yEnd)
        {
            int yRow = rowOff + 2 * k - 1;
            int uvRow = uvOff + k - 1;
            vp8_upsample_line_pair(cacheY, yRow * yBps, cacheY, (yRow + 1) * yBps,
                                   cacheU, uvRow * uvBps, cacheV, uvRow * uvBps,
                                   cacheU, (uvRow + 1) * uvBps, cacheV, (uvRow + 1) * uvBps,
                                   rgb, width, yStart + 2 * k - 1, yStart + 2 * k);
            k++;
        }
        int curY = yStart + 2 * (k - 1) + 1;
        if (yEnd < height)
        {
            int yBase = (rowOff + curY - yStart) * yBps;
            int uvBase = (uvOff + (curY >> 1) - (yStart >> 1)) * uvBps;
            int uvWidth = (width + 1) >> 1;
            memcpy(dec->tmpY, cacheY + yBase, (size_t)width);
            memcpy(dec->tmpU, cacheU + uvBase, (size_t)uvWidth);
            memcpy(dec->tmpV, cacheV + uvBase, (size_t)uvWidth);
        }
        else
        {
            if ((yEnd & 1) == 0)
            {
                int yBase = (rowOff + yEnd - 1 - yStart) * yBps;
                int uvBase = (uvOff + ((yEnd - 1 - yStart) >> 1)) * uvBps;
                vp8_upsample_line_pair(cacheY, yBase, NULL, 0,
                                       cacheU, uvBase, cacheV, uvBase,
                                       cacheU, uvBase, cacheV, uvBase,
                                       rgb, width, yEnd - 1, -1);
            }
        }
    }
    if (!isLast)
    {
        int yBps = dec->cacheYStride;
        int uvBps = dec->cacheUvStride;
        uint8_t *cacheY = dec->cacheY;
        uint8_t *cacheU = dec->cacheU;
        uint8_t *cacheV = dec->cacheV;
        for (int k = 0; k < dec->extra; k++)
        {
            int src = (16 + k) * yBps;
            int dst = k * yBps;
            memmove(cacheY + dst, cacheY + src, (size_t)yBps);
        }
        for (int k = 0; k < dec->extraUV; k++)
        {
            int src = (8 + k) * uvBps;
            int dst = k * uvBps;
            memmove(cacheU + dst, cacheU + src, (size_t)uvBps);
            memmove(cacheV + dst, cacheV + src, (size_t)uvBps);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P29j — decodeVP8Payload (webp.lua 2603-2736)
 * ═════════════════════════════════════════════════════════════════════════ */

void vp8_dec_free(VP8Dec *dec)
{
    if (!dec) return;
    if (dec->cacheY) PLUTO_FREE(dec->cacheY);
    if (dec->cacheU) PLUTO_FREE(dec->cacheU);
    if (dec->cacheV) PLUTO_FREE(dec->cacheV);
    if (dec->rgb) PLUTO_FREE(dec->rgb);
    if (dec->tmpY) PLUTO_FREE(dec->tmpY);
    if (dec->tmpU) PLUTO_FREE(dec->tmpU);
    if (dec->tmpV) PLUTO_FREE(dec->tmpV);
    if (dec->mbData) PLUTO_FREE(dec->mbData);
    if (dec->mbInfo) PLUTO_FREE(dec->mbInfo);
    if (dec->intraT) PLUTO_FREE(dec->intraT);
    if (dec->yuvT) PLUTO_FREE(dec->yuvT);
    if (dec->tokenBrs) PLUTO_FREE(dec->tokenBrs);
    PLUTO_FREE(dec);
}

uint8_t *vp8_decode_payload(const uint8_t *payload, size_t payloadLen,
                            const uint8_t *alphaPayload, size_t alphaLen,
                            int *outW, int *outH, uint8_t **outAlpha)
{
    vp8_log2_init();
    int len = (int)payloadLen;
    if (len < 11) return NULL;
    int b1 = payload[0], b2 = payload[1], b3 = payload[2];
    int bits = b1 | (b2 << 8) | (b3 << 16);
    if ((bits & 1) != 0) return NULL;
    int profile = (bits >> 1) & 7;
    if (profile > 3) return NULL;
    if (((bits >> 4) & 1) == 0) return NULL;
    int partitionLength = bits >> 5;
    if ((int)payloadLen < 10) return NULL;
    if (payload[3] != 0x9d || payload[4] != 0x01 || payload[5] != 0x2a) return NULL;
    int width = (((int)payload[7] << 8) | payload[6]) & 0x3fff;
    int height = (((int)payload[9] << 8) | payload[8]) & 0x3fff;
    if (width < 1 || height < 1 || width * height > WEBP_MAX_PIXELS) return NULL;
    if (11 + partitionLength > len + 1) return NULL;

    VP8Dec *dec = (VP8Dec *)PLUTO_MALLOC(sizeof(VP8Dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    /* Lua: for ci = 0, 1055 do dec.proba[ci] = CoeffsProba0[ci + 1] end */
    memcpy(dec->proba, vp8_coeffs_proba0, sizeof(dec->proba));
    dec->probaSegments[0] = 255;
    dec->probaSegments[1] = 255;
    dec->probaSegments[2] = 255;
    dec->width = width;
    dec->height = height;
    dec->mbW = (width + 15) >> 4;
    dec->mbH = (height + 15) >> 4;
    dec->filterType = 0;

    int yBps = 16 * dec->mbW;
    int uvBps = 8 * dec->mbW;
    int extra = VP8_KFILTER_EXTRA_ROWS[dec->filterType];
    int extraUV = extra >> 1;
    dec->extra = extra;
    dec->extraUV = extraUV;
    dec->cacheYStride = yBps;
    dec->cacheUvStride = uvBps;
    /* Allocate the caches with the WORST-CASE extra rows (filterType 2 →
     * extra 8 / extraUV 4): dec->filterType is still 0 here and is only
     * finalized by the filter header below. (Lua's dec.cacheY is an
     * unbounded table, so the reference has no such constraint.) */
    dec->cacheY = (uint8_t *)PLUTO_MALLOC((size_t)((8 + 16) * yBps));
    dec->cacheU = (uint8_t *)PLUTO_MALLOC((size_t)((4 + 8) * uvBps));
    dec->cacheV = (uint8_t *)PLUTO_MALLOC((size_t)((4 + 8) * uvBps));
    dec->rgb = (uint8_t *)PLUTO_MALLOC((size_t)(width * height * 3));
    dec->tmpY = (uint8_t *)PLUTO_MALLOC((size_t)width);
    dec->tmpU = (uint8_t *)PLUTO_MALLOC((size_t)((width + 1) >> 1));
    dec->tmpV = (uint8_t *)PLUTO_MALLOC((size_t)((width + 1) >> 1));
    dec->mbData = (VP8Mb *)PLUTO_MALLOC(sizeof(VP8Mb) * (size_t)dec->mbW);
    dec->mbInfo = (VP8MbInfo *)PLUTO_MALLOC(sizeof(VP8MbInfo) * (size_t)(dec->mbW + 1));
    dec->intraT = (uint8_t *)PLUTO_MALLOC((size_t)(4 * dec->mbW));
    dec->yuvT = (VP8TopYuv *)PLUTO_MALLOC(sizeof(VP8TopYuv) * (size_t)(dec->mbW + 1));
    if (!dec->cacheY || !dec->cacheU || !dec->cacheV || !dec->rgb ||
        !dec->tmpY || !dec->tmpU || !dec->tmpV || !dec->mbData ||
        !dec->mbInfo || !dec->intraT || !dec->yuvT)
    {
        vp8_dec_free(dec);
        return NULL;
    }
    memset(dec->cacheY, 127, (size_t)((8 + 16) * yBps));
    memset(dec->cacheU, 127, (size_t)((4 + 8) * uvBps));
    memset(dec->cacheV, 127, (size_t)((4 + 8) * uvBps));
    memset(dec->rgb, 0, (size_t)(width * height * 3));
    for (int i = 0; i < dec->mbW; i++)
    {
        VP8Mb *block = &dec->mbData[i];
        block->segment = 0; block->skip = 0; block->isI4x4 = 0;
        block->uvMode = 0;
        block->nonZeroY = 0; block->nonZeroUv = 0; block->dither = 0;
        block->fLimit = 0; block->fIlevel = 0; block->hevThresh = 0; block->fInner = 0;
    }
    for (int i = 0; i <= dec->mbW; i++)
    {
        dec->mbInfo[i].nz = 0;
        dec->mbInfo[i].nzDc = 0;
    }
    for (int i = 0; i < 4 * dec->mbW; i++) dec->intraT[i] = 0;
    for (int i = 0; i <= dec->mbW; i++) memset(&dec->yuvT[i], 0, sizeof(VP8TopYuv));

    /* NOTE: extra/extraUV recomputed after filter header below (filterType
     * may change); buffers were allocated with the worst case (filterType 2
     * → extra 8 / extraUV 4) because level is 0 pre-header. */
    /* Lua: newBr(payload, 11, partitionLength) — 11 is 1-BASED, so the
     * 0-based start is 10. vp8_br_new takes the 0-based start. */
    vp8_br_new(&dec->brMain, payload, 10, partitionLength);

    vp8_get_bit(&dec->brMain, 128); /* colorspace */
    vp8_get_bit(&dec->brMain, 128); /* clamp_type */
    if (!vp8_parse_segment_header(&dec->brMain, dec))
    {
        vp8_dec_free(dec);
        return NULL;
    }
    if (!vp8_parse_filter_header(&dec->brMain, dec))
    {
        vp8_dec_free(dec);
        return NULL;
    }
    /* Recompute the cache offsets now that filterType is final. */
    extra = VP8_KFILTER_EXTRA_ROWS[dec->filterType];
    extraUV = extra >> 1;
    dec->extra = extra;
    dec->extraUV = extraUV;
    int nParts = 1 << vp8_get_value(&dec->brMain, 2);
    dec->numPartsMinusOne = nParts - 1;
    int last = nParts - 1;
    /* Lua sizesStart = 11 + partitionLength (1-based) → 0-based 10 + pl. */
    int sizesStart = 10 + partitionLength;
    int partStart = sizesStart + 3 * last;
    if (partStart > len) /* Lua: partStart1 > len + 1 */
    {
        vp8_dec_free(dec);
        return NULL;
    }
    dec->tokenBrs = (VP8BoolBr *)PLUTO_MALLOC(sizeof(VP8BoolBr) * (size_t)nParts);
    if (!dec->tokenBrs)
    {
        vp8_dec_free(dec);
        return NULL;
    }
    for (int p = 0; p < last; p++)
    {
        int i1 = payload[sizesStart + p * 3];
        int i2 = payload[sizesStart + p * 3 + 1];
        int i3 = payload[sizesStart + p * 3 + 2];
        int psize = i1 | (i2 << 8) | (i3 << 16);
        int remaining = len - partStart; /* 0-based of Lua len - partStart1 + 1 */
        if (psize > remaining) psize = remaining;
        vp8_br_new(&dec->tokenBrs[p], payload, partStart, psize);
        partStart += psize;
    }
    vp8_br_new(&dec->tokenBrs[last], payload, partStart, len - partStart);
    vp8_parse_quant(&dec->brMain, dec);
    vp8_get_bit(&dec->brMain, 128);
    vp8_parse_proba(&dec->brMain, dec);

    dec->mbX = 0;
        vp8_precompute_filter_strengths(dec);
    for (int mbY = 0; mbY < dec->mbH; mbY++)
    {
        /* Tasks.yieldCheck() call site (task layer owns the frame budget) */
        dec->mbY = mbY;
        VP8BoolBr *tokenBr = &dec->tokenBrs[(mbY & dec->numPartsMinusOne)];
        if (!vp8_parse_intra_mode_row(&dec->brMain, dec))
        {
            vp8_dec_free(dec);
            return NULL;
        }
        for (int mbX = 0; mbX < dec->mbW; mbX++)
        {
            dec->mbX = mbX;
            if (!vp8_decode_mb(dec, tokenBr))
            {
                vp8_dec_free(dec);
                return NULL;
            }
        }
        vp8_init_scanline(dec);
        vp8_reconstruct_row(dec, mbY);
        if (dec->filterType > 0)
        {
            for (int mbX = 0; mbX < dec->mbW; mbX++)
                vp8_do_filter(dec, mbX, mbY);
        }
        vp8_finish_row(dec, mbY == 0, mbY == dec->mbH - 1);

    }
    uint8_t *alpha = alpha_decode_plane(alphaPayload, alphaLen, width, height);
    *outW = width;
    *outH = height;
    *outAlpha = alpha;
    uint8_t *rgb = dec->rgb;
    /* Detach buffers that outlive dec. */
    dec->rgb = NULL;
    vp8_dec_free(dec);
    return rgb;
}
