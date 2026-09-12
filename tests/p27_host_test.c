/* P27 host-side harness (macOS, NOT part of the Playdate build).
 * Compiles webp.c + webp_vp8_data.c against a faked PlaydateAPI and runs
 * the Huffman builder on the same vectors the Lua oracle saw, dumping
 * "idx bits value" lines identical to the harness format.
 * Usage: p27host <vector.txt>
 * vector.txt: "rootbits <n>" / "lengths <n...>" directive pairs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_api.h"

/* ---- Fake Playdate API (same pattern as p25_host_test.c) ---- */
static void *host_realloc(void *p, size_t n) { return realloc(p, n); }
static struct playdate_sys g_fakeSys;
static PlaydateAPI g_fakeApi;
static int g_fakeInit = 0;
PlaydateAPI *pluto_pd(void)
{
    if (!g_fakeInit)
    {
        memset(&g_fakeSys, 0, sizeof(g_fakeSys));
        memset(&g_fakeApi, 0, sizeof(g_fakeApi));
        g_fakeSys.realloc = host_realloc;
        g_fakeApi.system = &g_fakeSys;
        g_fakeInit = 1;
    }
    return &g_fakeApi;
}

#include "render/decoders/webp.h"
#include "render/decoders/webp-internal.h"

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s vector.txt\n", argv[1] ? argv[0] : "p27host"); return 2; }

    /* P28/P29 mode: argument is a .webp file -> full decode (VP8L, VP8+alpha,
     * or animated) via webp_decode_raw and dump "w h ck f0 f1 f2 f3".
     * Format matches the Lua oracle exactly: for VP8 files _testDecodeRaw
     * returns the flat rgb BYTE array, so ck/first4 run over the first w*h
     * bytes; for VP8L/animation it returns ARGB words over w*h pixels. */
    if (strstr(argv[1], ".webp"))
    {
        size_t dlen = 0;
        uint8_t *data = read_file(argv[1], &dlen);
        if (!data) { fprintf(stderr, "cannot read\n"); return 2; }
        int w = 0, h = 0;
        int isVP8 = 0;
        {
            /* Detect a simple lossy file (VP8 chunk, no VP8X/ANMF). */
            if (dlen >= 16 && memcmp(data + 8, "WEBP", 4) == 0)
            {
                size_t pos = 12;
                while (pos + 8 <= dlen)
                {
                    /* _testDecodeRaw returns rgb bytes whenever parseWebP
                     * finds a VP8 chunk (VP8X/ALPH don't change that); only
                     * an ANMF (animation) yields ARGB word arrays. */
                    if (memcmp(data + pos, "VP8 ", 4) == 0) { isVP8 = 1; break; }
                    if (memcmp(data + pos, "ANMF", 4) == 0) { isVP8 = 0; break; }
                    uint32_t csz = data[pos+4] | (data[pos+5]<<8) | (data[pos+6]<<16);
                    pos += 8 + csz + (csz & 1);
                }
            }
        }
        if (isVP8)
        {
            /* Mirror _testDecodeRaw's VP8 branch: rgb bytes, w, h, alpha. */
            const char *cid = NULL; const uint8_t *payload = NULL; size_t payloadLen = 0;
            const uint8_t *alphaData = NULL; size_t alphaLen = 0;
            uint8_t *alpha = NULL;
            uint8_t *rgb = NULL;
            int kind = webp_parse_container(data, dlen, &cid, &payload, &payloadLen,
                                            &alphaData, &alphaLen);
            if (kind == 2)
                rgb = vp8_decode_payload(payload, payloadLen, alphaData, alphaLen, &w, &h, &alpha);
            if (alpha) free(alpha);
            free(data);
            if (!rgb) { printf("NULL\n"); return 1; }
            uint32_t ck = 0;
            for (int i = 0; i < w * h; i++) ck += rgb[i];
            printf("%d %d %08x %02x %02x %02x %02x\n", w, h, ck,
                   rgb[0], rgb[1], rgb[2], rgb[3]);
            if (getenv("DUMP_ALL"))
            {
                fprintf(stderr, "all ");
                for (int i = 0; i < w * h * 3; i++)
                    fprintf(stderr, "%02x", rgb[i]);
                fprintf(stderr, "\n");
            }
            {
                fprintf(stderr, "rgb32:");
                for (int i = 0; i < 32 && i < w * h * 3; i++)
                    fprintf(stderr, " %02x", rgb[i]);
                fprintf(stderr, "\n");
            }
            free(rgb);
            return 0;
        }
        uint32_t *pix = webp_decode_raw(data, dlen, &w, &h);
        free(data);
        if (!pix) { printf("NULL\n"); return 1; }
        uint32_t ck = 0;
        for (int i = 0; i < w * h; i++) ck += pix[i];
        printf("%d %d %08x %08x %08x %08x %08x\n", w, h, ck, pix[0], pix[1], pix[2], pix[3]);
        free(pix);
        return 0;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    static uint32_t table[WEBP_HUFFMAN_TABLE_MAX];
    static uint8_t lens[4096];
    char line[8192];
    int rootbits = 8;
    while (fgets(line, sizeof(line), f))
    {
        char op[16];
        if (sscanf(line, "%15s", op) != 1) continue;
        if (strcmp(op, "rootbits") == 0)
        {
            sscanf(line, "%*s %d", &rootbits);
        }
        else if (strcmp(op, "lengths") == 0)
        {
            int n = 0;
            char *p = line;
            while (*p && *p != '\n')
            {
                if (*p >= '0' && *p <= '9')
                {
                    long v = strtol(p, &p, 10);
                    lens[n++] = (uint8_t)v;
                }
                else
                {
                    p++;
                }
            }
            memset(table, 0, sizeof(table));
            int used = webp_build_huffman_table(lens, n, rootbits, table);
            if (used <= 0)
            {
                printf("TABLE nil\n");
            }
            else
            {
                /* Determine max written index by scanning for entries the
                 * builder touched: replicate fills exactly `used` entries
                 * contiguously? No — two-level layouts write sparse root
                 * entries + second-level blocks. The Lua harness dumps
                 * [0..maxKey]; replicate the same by tracking all indices
                 * the builder could touch: root [0,1<<rootbits) plus the
                 * whole span it reported. For comparison safety we dump
                 * [0..used) — entries beyond the last written are zero in
                 * both implementations (C memset; Lua dumps nil->0). */
                printf("TABLE %d\n", used);
                for (int i = 0; i < used; i++)
                {
                    printf("%d %u %u\n", i, table[i] >> 16, table[i] & 0xFFFFu);
                }
            }
            fflush(stdout);
        }
    }
    fclose(f);
    return 0;
}
