/* FSAA with sub-pixel offsets (like the Voodoo 5's "T-buffer").
 * GLIDULATOR_FSAA=2|4|auto (read at grSstWinOpen): N complete planes
 * (colour + depth) in g_voodoo.fb_mem, plane k at k * g_fsaa_plane_bytes;
 * plane 0 is laid out exactly as without FSAA. Every triangle is drawn N
 * times, each time with a sub-pixel offset added to the vertex registers
 * (12.4). Start values and gradients refer to vertex A and move with it, so
 * rasterizer and JIT stay unchanged. glide_fsaa_output() averages the
 * planes for output.
 *
 * auto: planes for 4 are allocated, 1, 2 or 4 are active (g_fsaa_n),
 * switched from the measured frame time, only at grBufferSwap
 * (glide_fsaa_frame, when all workers are done).
 *
 * Independently, GLIDULATOR_AA_POST=1: edge smoothing after rendering
 * (aa_post). Edges are detected by depth discontinuities; pixels written
 * through the LFB (mask, glide_aa_mark) are left untouched.
 *
 * Without settings N = 1: glide_fsaa_draw() just calls voodoo_triangle()
 * with unchanged params, and glide_fsaa_output() returns fb_mem. */

#include "glidulator/platform.h"
#include "glidulator/renderer.h"
#include "glidulator/trace.h"

#include "glide_internal.h"
#include "vid_voodoo_render.h"

int      g_fsaa_n = 1;
uint32_t g_fsaa_plane_bytes;
uint8_t *g_aa_mask; /* AA_POST: 1 bit per pixel, set = written through the LFB */

static int       g_planes = 1; /* allocated planes */
static int       g_auto;       /* GLIDULATOR_FSAA=auto */
static int       g_post;       /* GLIDULATOR_AA_POST=1 */
static uint16_t *g_out;        /* image to show (N > 1 or AA_POST) */
static uint16_t *g_tmp_out;    /* intermediate image for AA_POST after the resolve */
static uint64_t  g_auto_sum;   /* auto: sum of game+render times ... */
static unsigned  g_auto_frames; /* ... over this many frames */

/* Offset per plane in 1/16 pixel (x, y): 2 = diagonal, 4 = rotated grid,
 * the Voodoo 5's patterns. */
static const signed char g_pat2[2][2] = { { -4, -4 }, { 4, 4 } };
static const signed char g_pat4[4][2] = { { -6, -2 }, { 2, -6 }, { 6, 2 }, { -2, 6 } };

/* One params copy per worker (workers read the same queue entry
 * concurrently); slot GLIDE_MT_MAX for the main core during recovery (a
 * hung worker might still use its own slot). */
static voodoo_params_t g_tmp[GLIDE_MT_MAX + 1];

int glide_fsaa_read_cfg(void) {
    char cfg[8];
    g_fsaa_n = g_planes = 1;
    g_auto   = 0;
    if (platform_get_config("GLIDULATOR_FSAA", cfg, sizeof cfg)) {
        if (!cfg[1] && (cfg[0] == '2' || cfg[0] == '4'))
            g_fsaa_n = g_planes = cfg[0] - '0';
        else if ((cfg[0] == 'a' || cfg[0] == 'A') && (cfg[1] == 'u' || cfg[1] == 'U')) {
            g_auto   = 1;
            g_planes = 4; /* start at 1, then step up */
        }
    }
    g_post = platform_get_config("GLIDULATOR_AA_POST", cfg, sizeof cfg) && cfg[0] == '1' && !cfg[1];
    return g_planes;
}

int glide_fsaa_open(int width, int height) {
    g_fsaa_plane_bytes = (uint32_t) width * (uint32_t) height * 4; /* RGB565 + depth */
    g_out = g_tmp_out = 0;
    g_aa_mask         = 0;
    g_auto_sum        = 0;
    g_auto_frames     = 0;
    if (g_planes > 1 || g_post) {
        g_out = (uint16_t *) platform_alloc((size_t) width * (size_t) height * 2);
        if (!g_out)
            return 0;
    }
    if (g_post) {
        g_aa_mask = (uint8_t *) platform_alloc(((size_t) width * (size_t) height + 7) / 8);
        if (!g_aa_mask)
            return 0;
        for (size_t i = 0; i < ((size_t) width * (size_t) height + 7) / 8; i++)
            g_aa_mask[i] = 0;
        if (g_planes > 1) {
            g_tmp_out = (uint16_t *) platform_alloc((size_t) width * (size_t) height * 2);
            if (!g_tmp_out)
                return 0;
        }
    }
    return 1;
}

int glide_fsaa_auto(void) {
    return g_auto;
}

int glide_aa_post_enabled(void) {
    return g_post;
}

void glide_fsaa_close(void) {
    platform_free(g_out);
    platform_free(g_tmp_out);
    platform_free(g_aa_mask);
    g_out = g_tmp_out = 0;
    g_aa_mask         = 0;
}

void glide_fsaa_draw(voodoo_params_t *params, int worker, int slot) {
    if (g_fsaa_n == 1) {
        voodoo_triangle(&g_voodoo, params, worker);
        return;
    }
    const signed char(*pat)[2] = g_fsaa_n == 2 ? g_pat2 : g_pat4;
    voodoo_params_t *p         = &g_tmp[slot];
    for (int k = 0; k < g_fsaa_n; k++) {
        *p = *params;
        p->vertexAx += pat[k][0];
        p->vertexBx += pat[k][0];
        p->vertexCx += pat[k][0];
        p->vertexAy += pat[k][1];
        p->vertexBy += pat[k][1];
        p->vertexCy += pat[k][1];
        p->draw_offset = params->draw_offset + (uint32_t) k * g_fsaa_plane_bytes;
        p->aux_offset  = params->aux_offset + (uint32_t) k * g_fsaa_plane_bytes;
        voodoo_triangle(&g_voodoo, p, worker);
    }
}

/* Per channel: the sum of the N planes divided by N, with an ordered
 * dither threshold d < N. Identical planes give exactly their value (GUI,
 * LFB); mixed edge pixels get the in-between levels as a pattern. */
static void resolve(uint16_t *dst_all) {
    const uint16_t *fb = (const uint16_t *) g_voodoo.fb_mem;
    static const unsigned char d2[2][2] = { { 0, 1 }, { 1, 0 } };
    static const unsigned char d4[2][2] = { { 0, 2 }, { 3, 1 } };
    const unsigned char(*d)[2] = g_fsaa_n == 2 ? d2 : d4;
    int      shift = g_fsaa_n == 2 ? 1 : 2;
    size_t   step  = g_fsaa_plane_bytes / 2;
    for (int y = 0; y < g_height; y++) {
        const uint16_t *src = fb + (size_t) y * (size_t) g_width;
        uint16_t       *dst = dst_all + (size_t) y * (size_t) g_width;
        for (int x = 0; x < g_width; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < g_fsaa_n; k++) {
                unsigned v = src[x + (size_t) k * step];
                r += v >> 11;
                g += (v >> 5) & 0x3f;
                b += v & 0x1f;
            }
            unsigned t = d[y & 1][x & 1];
            dst[x]     = (uint16_t) ((((r + t) >> shift) << 11) | (((g + t) >> shift) << 5) | ((b + t) >> shift));
        }
    }
}

/* --- AA_POST: edge smoothing after rendering ----------------------- */

void glide_aa_mark(size_t i) {
    g_aa_mask[i >> 3] |= (uint8_t) (1u << (i & 7));
}

/* grBufferClear: release the pixels in the rectangle (memory lines y0..y1-1) */
void glide_aa_unmark_rows(int x0, int x1, int y0, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            size_t i = (size_t) y * (size_t) g_width + (size_t) x;
            g_aa_mask[i >> 3] &= (uint8_t) ~(1u << (i & 7));
        }
}

static int masked(size_t i) {
    return (g_aa_mask[i >> 3] >> (i & 7)) & 1;
}

/* Depth discontinuity between two pixels: relative (1/32 of the larger
 * value, at least 256). Works for Z and W buffers alike (both 16 bit; only
 * the distance matters, not its direction). */
static int depth_edge(unsigned a, unsigned b) {
    unsigned d = a > b ? a - b : b - a, m = a > b ? a : b;
    unsigned t = m >> 5;
    return d > (t < 256 ? 256 : t);
}

/* 3/4 own value + 1/4 neighbour, per channel */
static uint16_t mix(uint16_t c, uint16_t n) {
    unsigned r = ((c >> 11) * 3 + (n >> 11) + 2) >> 2;
    unsigned g = (((c >> 5) & 63) * 3 + ((n >> 5) & 63) + 2) >> 2;
    unsigned b = ((c & 31) * 3 + (n & 31) + 2) >> 2;
    return (uint16_t) ((r << 11) | (g << 5) | b);
}

/* Every pixel at a depth edge is mixed with its neighbour across the edge
 * (one horizontal and one vertical neighbour at most). Simple and cheap;
 * surfaces and LFB pixels stay exact. */
static void aa_post(const uint16_t *src, uint16_t *dst) {
    const uint16_t *z = (const uint16_t *) (g_voodoo.fb_mem + g_aux_offset);
    int             w = g_width, h = g_height;
    for (int y = 0; y < h; y++) {
        const uint16_t *s  = src + (size_t) y * (size_t) w;
        uint16_t       *d  = dst + (size_t) y * (size_t) w;
        const uint16_t *zr = z + (size_t) y * (size_t) w;
        for (int x = 0; x < w; x++) {
            size_t   i = (size_t) y * (size_t) w + (size_t) x;
            uint16_t c = s[x];
            if (masked(i) || x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                d[x] = c;
                continue;
            }
            unsigned zc = zr[x];
            int      nx = 0, ny = 0;
            unsigned dl = depth_edge(zc, zr[x - 1]) && !masked(i - 1) ? 1u : 0u;
            unsigned dr = depth_edge(zc, zr[x + 1]) && !masked(i + 1) ? 1u : 0u;
            unsigned du = depth_edge(zc, zr[x - w]) && !masked(i - (size_t) w) ? 1u : 0u;
            unsigned dd = depth_edge(zc, zr[x + w]) && !masked(i + (size_t) w) ? 1u : 0u;
            if (dl)
                nx = -1;
            else if (dr)
                nx = 1;
            if (du)
                ny = -w;
            else if (dd)
                ny = w;
            if (nx)
                c = mix(c, s[x + nx]);
            if (ny)
                c = mix(c, s[x + ny]);
            d[x] = c;
        }
    }
}

const uint16_t *glide_fsaa_output(void) {
    const uint16_t *fb = (const uint16_t *) g_voodoo.fb_mem;
    if (!g_out)
        return fb;
    if (!g_post) {
        if (g_fsaa_n == 1)
            return fb;
        resolve(g_out);
        return g_out;
    }
    const uint16_t *src = fb;
    if (g_fsaa_n > 1 && g_tmp_out) {
        resolve(g_tmp_out);
        src = g_tmp_out;
    }
    aa_post(src, g_out);
    return g_out;
}

/* auto: after every frame (workers done). render_us = game+render time of
 * the frame. Averaged over 30 frames; step up when the estimated time of
 * the next level (rendering scales ~ with N) stays below 80 % of the budget
 * (1/60 s), step down immediately when the budget is exceeded. Stepping up
 * copies plane 0 into the new planes (games that draw on without
 * clearing). */
#define FSAA_BUDGET_US 16667u

int glide_fsaa_frame(uint64_t render_us) {
    if (!g_auto)
        return g_fsaa_n;
    g_auto_sum += render_us;
    if (++g_auto_frames < 30)
        return g_fsaa_n;
    uint64_t avg  = g_auto_sum / g_auto_frames;
    g_auto_sum    = 0;
    g_auto_frames = 0;
    int next     = g_fsaa_n;
    if (!avg) /* no usable timer (DOSBox without TSC): do nothing */
        return g_fsaa_n;
    if (avg > FSAA_BUDGET_US && g_fsaa_n > 1)
        next = g_fsaa_n / 2;
    else if (g_fsaa_n < g_planes && avg * 2 < FSAA_BUDGET_US * 8 / 10)
        next = g_fsaa_n * 2;
    if (next > g_fsaa_n) {
        size_t bytes = g_fsaa_plane_bytes;
        for (int k = g_fsaa_n; k < next; k++) {
            uint8_t *dst = g_voodoo.fb_mem + (size_t) k * bytes;
            for (size_t b = 0; b < bytes; b++)
                dst[b] = g_voodoo.fb_mem[b];
        }
    }
    if (next != g_fsaa_n) {
        /* refcount grew by g_fsaa_n per triangle; switching is safe because
         * all workers are done */
        g_fsaa_n = next;
#ifdef GLIDULATOR_TRACE
        glide_trace_note("FSAA auto: layers/average us", (unsigned long) g_fsaa_n, (unsigned long) avg);
#endif
    }
    return g_fsaa_n;
}
