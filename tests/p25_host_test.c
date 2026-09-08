/* P25 host-side debug harness (macOS, NOT part of the Playdate build).
 * Compiles jpeg.c + scale.c + dither.c against a faked PlaydateAPI (real
 * struct shapes from pd_api.h; only system->realloc is implemented).
 * Usage: p25host <file.jpg> [maxW] [maxH]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "pd_api.h"

/* ---- Fake Playdate API ---- */
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
void pluto_free(void *p) { free(p); }

#include "render/decoders/scale.h"
#include "render/decoders/jpeg.h"

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
    if (argc < 2) { fprintf(stderr, "usage: %s file.jpg [maxW] [maxH]\n", argv[0]); return 2; }
    int maxW = argc > 2 ? atoi(argv[2]) : 360;
    int maxH = argc > 3 ? atoi(argv[3]) : 200;
    size_t len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    int rows = 0, w = 0;
    uint8_t **grid = jpeg_decode_gray(data, len, maxW, maxH, &rows, &w);
    if (!grid) { printf("NULL\n"); return 1; }
    printf("%d %d\n", w, rows);
    for (int y = 0; y < rows; y++)
    {
        for (int x = 0; x < w; x++)
        {
            printf("%d%s", grid[y][x], x + 1 < w ? " " : "\n");
        }
    }
    jpeg_gray_free(grid);
    free(data);
    return 0;
}
