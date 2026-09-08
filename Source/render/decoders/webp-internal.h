/*
 * PlutoBrowser — webp-internal.h
 * Shared internal declarations between webp.c (P27/P28: bit reader,
 * Huffman, VP8L) and webp_vp8.c / webp_container.c (P29: VP8 lossy,
 * alpha, container, animation). Not part of the public decoder API.
 */
#ifndef PLUTO_WEBP_INTERNAL_H
#define PLUTO_WEBP_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "render/decoders/webp.h"

/* WebPDecodeCtx (VP8L decode context) — shared by webp.c and the alpha
 * lossless path in webp_vp8.c. */
#define WEBP_MAX_TRANSFORMS 3

typedef struct WebPTransform
{
    int type;
    int xsize, ysize;
    int bits;
    uint32_t *data;    /* heap, sub-stream pixels or expanded color map */
} WebPTransform;

typedef struct WebPDecodeCtx
{
    uint32_t *huffmanImage;     /* heap or NULL */
    int huffmanSubsampleBits;   /* 0 when absent */
    int huffmanXsize;
    int numHtreeGroups;
    int colorCacheSize;
    uint32_t *colorCacheColors; /* heap (1<<bits) or NULL */
    int colorCacheHashShift;
    int transformXsize, transformYsize;
    WebPTransform transforms[WEBP_MAX_TRANSFORMS];
    int numTransforms;
} WebPDecodeCtx;

/* ── P28 internals reused by the alpha plane (lossless method) ───────────── */
int vp8l_decode_image_stream_pub(int xsize, int ysize, int isLevel0,
                                 WebPBitReader *br, WebPDecodeCtx *ctx);
int vp8l_decode_image_data_pub(WebPBitReader *br, WebPDecodeCtx *ctx,
                               uint32_t *data, int width, int height);
uint32_t *vp8l_apply_inverse_transforms_pub(WebPDecodeCtx *ctx, uint32_t *data,
                                            int rows, int *outOwned);
void vp8l_ctx_free_pub(WebPDecodeCtx *ctx);

/* ── P29: VP8 structures (webp.lua dec table shape) ──────────────────────── */

typedef struct VP8BoolBr
{
    const uint8_t *payload;
    int p, end1, max1;
    int range;
    uint32_t value;
    int bits;
    int eof;
} VP8BoolBr;

typedef struct VP8SegmentHdr
{
    int useSegment, updateMap, absoluteDelta;
    int quantizer[4];
    int filterStrength[4];
} VP8SegmentHdr;

typedef struct VP8FilterHdr
{
    int simple, level, sharpness, useLfDelta;
    int refLfDelta[4], modeLfDelta[4];
} VP8FilterHdr;

/* Lua m = { y1 = {dc,ac}, y2 = {dc,ac}, uv = {dc,ac} } */
typedef struct VP8QuantInfo
{
    int y1[2], y2[2], uv[2];
} VP8QuantInfo;

typedef struct VP8FilterStrength
{
    int fLimit, fIlevel, hevThresh, fInner;
} VP8FilterStrength;

typedef struct VP8Mb
{
    int16_t coeffs[384];
    uint32_t nonZeroY, nonZeroUv;
    uint8_t segment, skip, isI4x4;
    uint8_t imodes[16];
    uint8_t uvMode;
    uint8_t dither;
    int fLimit, fIlevel, hevThresh, fInner;
} VP8Mb;

typedef struct VP8MbInfo
{
    uint8_t nz;
    uint8_t nzDc;
} VP8MbInfo;

typedef struct VP8TopYuv
{
    uint8_t y[16];
    uint8_t u[8];
    uint8_t v[8];
} VP8TopYuv;

typedef struct VP8Dec
{
    int width, height, mbW, mbH;
    int filterType;
    VP8SegmentHdr segmentHdr;
    VP8FilterHdr filterHdr;
    int probaSegments[3];
    uint8_t proba[1056];
    VP8QuantInfo dqm[4];
    VP8FilterStrength fstrengths[4][2];
    int useSkipProba, skipP;
    VP8BoolBr brMain;
    VP8BoolBr *tokenBrs;
    int numPartsMinusOne;
    int extra, extraUV;
    int cacheYStride, cacheUvStride;
    uint8_t *cacheY, *cacheU, *cacheV;
    uint8_t *rgb;
    uint8_t *tmpY, *tmpU, *tmpV;
    VP8Mb *mbData;
    VP8MbInfo *mbInfo;
    uint8_t *intraT;
    uint8_t intraL[4];
    VP8TopYuv *yuvT;
    int mbX, mbY;
} VP8Dec;

void vp8_dec_free(VP8Dec *dec);

/* VP8 lossy payload decode (webp.lua decodeVP8Payload). Returns the heap
 * RGB planar buffer (3 bytes/pixel, caller frees) and optionally the alpha
 * plane (NULL when absent; caller frees). */
uint8_t *vp8_decode_payload(const uint8_t *payload, size_t payloadLen,
                            const uint8_t *alphaPayload, size_t alphaLen,
                            int *outW, int *outH, uint8_t **outAlpha);

/* Alpha plane (webp.lua alphaUnfilter/decodeAlphaPlane). */
uint8_t *alpha_decode_plane(const uint8_t *alphaPayload, size_t alphaLen,
                            int width, int height);

/* ── P29: container / animation (webp_container.c) ───────────────────────── */

/* parseWebP: returns chunk id ("VP8L"/"VP8 ") and payload pointers into
 * data (no copy); alphaData receives the ALPH chunk when present. */
int webp_parse_container(const uint8_t *data, size_t len,
                         const char **cid,
                         const uint8_t **payload, size_t *payloadLen,
                         const uint8_t **alphaData, size_t *alphaLen);

/* Animated WebP (VP8X+ANIM+ANMF). Returns 1 and fills out on success. */
typedef struct WebPAnimFrame
{
    int x, y, w, h, duration;
    int dispose, noBlend;
    const char *cid; /* "VP8L" or "VP8 " */
    const uint8_t *payload;
    size_t payloadLen;
    const uint8_t *alpha;
    size_t alphaLen;
} WebPAnimFrame;

typedef struct WebPAnim
{
    int canvasW, canvasH;
    uint32_t bgcolor;
    int loopCount;
    int numFrames;
    WebPAnimFrame *frames; /* heap, caller frees */
} WebPAnim;

int webp_parse_animation(const uint8_t *data, size_t len, WebPAnim *out);
void webp_anim_free(WebPAnim *anim);
uint32_t webp_blend_pixel_non_premult(uint32_t src, uint32_t dst);

/* decodeAnimation: returns blended canvas-size ARGB frames.
 * On success *outCount frames are returned in a heap array of heap
 * ARGB buffers (all caller-freed via webp_anim_result_free). */
typedef struct WebPAnimResult
{
    int width, height;
    uint32_t bgcolor;
    int loopCount;
    int numFrames;
    uint32_t **pix;   /* heap array of heap ARGB buffers */
    int *durations;   /* heap array */
} WebPAnimResult;

int webp_decode_animation(const uint8_t *data, size_t len, WebPAnimResult *out);
void webp_anim_result_free(WebPAnimResult *res);

/* VP8L payload decode (P28, defined in webp.c). */
uint32_t *webp_decode_vp8l_payload(const uint8_t *payload, size_t payloadLen,
                                   int *outW, int *outH, int *outOwned);

#endif /* PLUTO_WEBP_INTERNAL_H */
