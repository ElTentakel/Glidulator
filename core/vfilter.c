/* Video output filter of the Voodoo Graphics ("Voodoo look"),
 * GLIDULATOR_VOODOO_FILTER.
 *
 * The real board sends its 16-bit image (RGB565 with 4x4 dithering)
 * through a filter in the video output: neighbouring pixels that differ by
 * only a few steps (the dither pattern) are averaged, larger jumps (edges)
 * stay. 3dfx's driver enables it by default with videoFilterRgbThreshold =
 * 0x080408 (R 8, G 4, B 8; sst1InitVideo, environment
 * SST_VIDEO_FILTER_THRESHOLD). The result has more than 16 bits of colour,
 * so it goes to platform_blit32() as XRGB8888.
 *
 * Re-implementation of 86Box's filter (vid_voodoo_display.c,
 * voodoo_generate_filter_v1/voodoo_filterline_v1, Sarah Walker, GPL; see
 * core/rasterizer/ATTRIBUTION.md): per channel a table [pixel][neighbour]
 * -> average with the difference limited to the threshold, four passes per
 * line (three with the left, one with the right neighbour). 86Box's extra
 * "purple line" (R/B + 4 on every second line) is an emulator effect, not
 * documented hardware behaviour, and is left out.
 *
 * GLIDULATOR_VOODOO_FILTER=0 (default) | 1 (3dfx threshold) | hex value
 * RRGGBB (own threshold, e.g. 180c18). */

#include "glidulator/platform.h"

#include "glide_internal.h"

static uint8_t (*g_tab)[256][256]; /* [0] red, [1] green, [2] blue */
static uint32_t *g_out;
static uint8_t   g_line[2][2048 * 3];
static int       g_on;

static int hexval(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Table as voodoo_generate_filter_v1 builds it: difference limited to
 * +-cap, result = pixel + difference/2 (the condition there is always
 * true). */
static void make_table(uint8_t t[256][256], int cap) {
    for (int g = 0; g < 256; g++)
        for (int h = 0; h < 256; h++) {
            int d = h - g;
            if (d > cap)
                d = cap;
            if (d < -cap)
                d = -cap;
            int v = g + d / 2;
            t[g][h] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

int glide_vfilter_open(int width, int height) {
    char     cfg[16];
    unsigned thr = 0;
    g_on         = 0;
    g_out        = 0;
    g_tab        = 0;
    if (!platform_get_config("GLIDULATOR_VOODOO_FILTER", cfg, sizeof cfg) || (cfg[0] == '0' && !cfg[1]))
        return 1;
    if (cfg[0] == '1' && !cfg[1])
        thr = 0x080408;
    else {
        const char *p = cfg;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        for (; *p && hexval(*p) >= 0; p++)
            thr = thr << 4 | (unsigned) hexval(*p);
        if (*p || !thr)
            return 1; /* unparsable: off */
    }
    if (width > 2048)
        return 1;
    g_tab = (uint8_t(*)[256][256]) platform_alloc(3 * 256 * 256);
    g_out = (uint32_t *) platform_alloc((size_t) width * (size_t) height * 4);
    if (!g_tab || !g_out) {
        glide_vfilter_close();
        return 0;
    }
    make_table(g_tab[0], (int) ((thr >> 16) & 0xff));
    make_table(g_tab[1], (int) ((thr >> 8) & 0xff));
    make_table(g_tab[2], (int) (thr & 0xff));
    g_on = 1;
    return 1;
}

void glide_vfilter_close(void) {
    platform_free(g_tab);
    platform_free(g_out);
    g_tab = 0;
    g_out = 0;
    g_on  = 0;
}

int glide_vfilter_enabled(void) {
    return g_on;
}

/* One line: a = pixels, b = intermediate result (fil/fil3 in 86Box) */
static void filter_line(const uint16_t *src, uint32_t *dst, int w) {
    uint8_t *a = g_line[0], *b = g_line[1];
    uint8_t(*t)[256][256] = g_tab;
    for (int x = 0; x < w; x++) {
        unsigned p   = src[x];
        a[x * 3]     = (uint8_t) (((p >> 11) & 31) << 3);
        a[x * 3 + 1] = (uint8_t) (((p >> 5) & 63) << 2);
        a[x * 3 + 2] = (uint8_t) ((p & 31) << 3);
        b[x * 3]     = a[x * 3];
        b[x * 3 + 1] = a[x * 3 + 1];
        b[x * 3 + 2] = a[x * 3 + 2];
    }
    for (int x = 1; x < w; x++)
        for (int c = 0; c < 3; c++)
            b[x * 3 + c] = t[c][a[x * 3 + c]][a[(x - 1) * 3 + c]];
    for (int x = 1; x < w; x++)
        for (int c = 0; c < 3; c++)
            a[x * 3 + c] = t[c][b[x * 3 + c]][b[(x - 1) * 3 + c]];
    for (int x = 1; x < w; x++)
        for (int c = 0; c < 3; c++)
            b[x * 3 + c] = t[c][a[x * 3 + c]][a[(x - 1) * 3 + c]];
    for (int x = 0; x < w - 1; x++)
        for (int c = 0; c < 3; c++)
            a[x * 3 + c] = t[c][b[x * 3 + c]][b[(x + 1) * 3 + c]];
    /* as in 86Box, the last pixel keeps its value after the second pass */
    for (int x = 0; x < w; x++)
        dst[x] = (uint32_t) a[x * 3] << 16 | (uint32_t) a[x * 3 + 1] << 8 | a[x * 3 + 2];
}

void glide_blit_output(const uint16_t *fb) {
    glide_blit_output_keep(fb, 0, 0, 0, 0);
}

/* Leave rectangle kx/ky/kw/kh unfiltered (OSD box: text stays sharp) */
void glide_blit_output_keep(const uint16_t *fb, int kx, int ky, int kw, int kh) {
    if (!g_on) {
        platform_blit(fb, g_width, g_height, g_width * 2);
        return;
    }
    for (int y = 0; y < g_height; y++)
        filter_line(fb + (size_t) y * (size_t) g_width, g_out + (size_t) y * (size_t) g_width, g_width);
    for (int y = ky; y < ky + kh && y < g_height; y++)
        for (int x = kx; x < kx + kw && x < g_width; x++) {
            unsigned p = fb[(size_t) y * (size_t) g_width + (size_t) x];
            g_out[(size_t) y * (size_t) g_width + (size_t) x] =
                (uint32_t) (((p >> 11) & 31) << 3) << 16 | (uint32_t) (((p >> 5) & 63) << 2) << 8 | (p & 31) << 3;
        }
    platform_blit32(g_out, g_width, g_height, g_width * 4);
}
