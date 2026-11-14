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
#include "glidulator/renderer.h"

#include "glide_internal.h"
#include "vfilter_kern.h"

static uint8_t (*g_tab)[256][256]; /* [0] red, [1] green, [2] blue */
static uint32_t *g_out;
/* scratch per drawer (glide_mt_parallel: workers + main core <= GLIDE_MT_MAX) */
static uint8_t   g_line[GLIDE_MT_MAX][2][2048 * 3];
static int       g_on;
static int       g_cap[3];  /* threshold per channel (r, g, b) */
static int       g_sse2;    /* vf_pass_sse2 usable */

static int hexval(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
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
    g_cap[0] = (int) ((thr >> 16) & 0xff);
    g_cap[1] = (int) ((thr >> 8) & 0xff);
    g_cap[2] = (int) (thr & 0xff);
    for (int c = 0; c < 3; c++)
        vf_make_table(g_tab[c], g_cap[c]);
    g_sse2 = VF_HAVE_SSE2 && (platform_cpu_features() & PLATFORM_CPU_SSE2);
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

/* One pass: dst[i] = step(g[i], h[i]) (vfilter_kern.h) */
static void filter_pass(uint8_t *dst, const uint8_t *g, const uint8_t *h, int n, int ch) {
#if VF_HAVE_SSE2
    if (g_sse2) {
        vf_pass_sse2(dst, g, h, n, g_cap[ch]);
        return;
    }
#endif
    vf_pass_table(dst, g, h, n, g_tab[ch]);
}

/* One channel of one line, the four passes of 86Box's filter (fil/fil3
 * there): three with the left neighbour, one with the right. a holds the
 * channel's pixels on entry and the result on exit; b is scratch. As in
 * 86Box the first pixel keeps its value in the left passes and the last
 * pixel keeps its value after the second pass. Every pass reads one buffer
 * and writes the other, so the pixels of a pass are independent. */
static void filter_channel(uint8_t *a, uint8_t *b, int ch, int w) {
    b[0] = a[0];
    filter_pass(b + 1, a + 1, a, w - 1, ch);
    filter_pass(a + 1, b + 1, b, w - 1, ch);
    uint8_t keep = a[w - 1];
    filter_pass(b + 1, a + 1, a, w - 1, ch);
    filter_pass(a, b, b + 1, w - 1, ch);
    a[w - 1] = keep;
}

/* One line. Planar per channel (r, g, b one after another in g_line[0]),
 * so each pass is a plain loop over one 64 KB table; the interleaved
 * version with a channel loop inside took 5.4 ms per 512x384 frame on an
 * i7-4790 (Watcom). Bit-identical to it. */
static void filter_line(const uint16_t *src, uint32_t *dst, int w, int drawer) {
    uint8_t *r = g_line[drawer][0], *g = r + 2048, *b = g + 2048, *tmp = g_line[drawer][1];
#if VF_HAVE_SSE2
    if (g_sse2)
        vf_split565_sse2(src, r, g, b, w);
    else
#endif
        vf_split565(src, r, g, b, w);
    filter_channel(r, tmp, 0, w);
    filter_channel(g, tmp, 1, w);
    filter_channel(b, tmp, 2, w);
#if VF_HAVE_SSE2
    if (g_sse2)
        vf_join_sse2(r, g, b, dst, w);
    else
#endif
        vf_join(r, g, b, dst, w);
}

/* Lines of one drawer (glide_mt_parallel; the lines are independent) */
static void filter_job(int drawer, void *arg) {
    const uint16_t *fb = (const uint16_t *) arg;
    for (int y = 0; y < g_height; y++)
        if (glide_mt_line_owner(y) == drawer)
            filter_line(fb + (size_t) y * (size_t) g_width, g_out + (size_t) y * (size_t) g_width, g_width, drawer);
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
    glide_mt_parallel(filter_job, (void *) fb);
    for (int y = ky; y < ky + kh && y < g_height; y++)
        for (int x = kx; x < kx + kw && x < g_width; x++) {
            unsigned p = fb[(size_t) y * (size_t) g_width + (size_t) x];
            g_out[(size_t) y * (size_t) g_width + (size_t) x] =
                (uint32_t) (((p >> 11) & 31) << 3) << 16 | (uint32_t) (((p >> 5) & 63) << 2) << 8 | (p & 31) << 3;
        }
    platform_blit32(g_out, g_width, g_height, g_width * 4);
}
