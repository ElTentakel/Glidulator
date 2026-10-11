/* VESA/VGA output, portable part; see video.h. No BIOS/DPMI code here,
 * everything goes through vid_bios. */

#include "video.h"
#include "../common/output_rect.h"

#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include <string.h>

/* --- VBE structures ------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* ModeAttributes (VBE 3.0, Abschnitt 4.4) */
#define MA_SUPPORTED 0x0001
#define MA_GRAPHICS  0x0010
#define MA_LFB       0x0080
#define MM_DIRECT    6

int vid_parse_mode_info(const uint8_t *mib, int vbe3, vid_mode *out) {
    uint16_t attr = rd16(mib + 0);
    if ((attr & (MA_SUPPORTED | MA_GRAPHICS | MA_LFB)) != (MA_SUPPORTED | MA_GRAPHICS | MA_LFB))
        return 0;
    if (mib[27] != MM_DIRECT)
        return 0;

    vid_mode m;
    memset(&m, 0, sizeof m);
    m.width     = rd16(mib + 18);
    m.height    = rd16(mib + 20);
    m.bpp       = mib[25];
    m.pitch     = rd16(mib + 16);
    m.phys_base = rd32(mib + 40);
    if (vbe3 && rd16(mib + 50))
        m.pitch = rd16(mib + 50); /* LinBytesPerScanLine: what counts for the LFB */
    m.r_size = mib[31];
    m.r_pos  = mib[32];
    m.g_size = mib[33];
    m.g_pos  = mib[34];
    m.b_size = mib[35];
    m.b_pos  = mib[36];

    if (m.bpp != 15 && m.bpp != 16 && m.bpp != 24 && m.bpp != 32)
        return 0;
    if (!m.r_size && !m.g_size && !m.b_size) {
        /* Some VBE-1.2-style BIOSes leave the masks empty; then the usual
         * layout applies. */
        if (m.bpp == 15) {
            m.r_size = 5; m.r_pos = 10; m.g_size = 5; m.g_pos = 5; m.b_size = 5; m.b_pos = 0;
        } else if (m.bpp == 16) {
            m.r_size = 5; m.r_pos = 11; m.g_size = 6; m.g_pos = 5; m.b_size = 5; m.b_pos = 0;
        } else {
            m.r_size = 8; m.r_pos = 16; m.g_size = 8; m.g_pos = 8; m.b_size = 8; m.b_pos = 0;
        }
    }
    if (!m.r_size || !m.g_size || !m.b_size || m.r_size > 8 || m.g_size > 8 || m.b_size > 8)
        return 0;
    if (!m.width || !m.height || !m.phys_base || m.pitch < m.width * ((m.bpp + 7) / 8))
        return 0;
    m.mode = 0;
    *out   = m;
    return 1;
}

static int is_rgb565(const vid_mode *m) {
    return m->bpp == 16 && m->r_size == 5 && m->r_pos == 11 && m->g_size == 6 && m->g_pos == 5 &&
           m->b_size == 5 && m->b_pos == 0;
}

/* Smaller = better: 565 is a plain line copy, 32 bit gives aligned
 * memory accesses, 24 bit is the most expensive. */
static int g_prefer_deep; /* video filter: 32/24 bit first */

void vid_prefer_deep(int on) {
    g_prefer_deep = on;
}

static int format_rank(const vid_mode *m) {
    if (g_prefer_deep)
        return m->bpp == 32 ? 0 : m->bpp == 24 ? 1 : is_rgb565(m) ? 2 : 3;
    if (is_rgb565(m))
        return 0;
    if (m->bpp == 32)
        return 1;
    if (m->bpp == 15 || m->bpp == 16)
        return 2;
    return 3;
}

int vid_select_mode(const vid_mode *modes, int n, int w, int h) {
    int  best      = -1;
    long best_area = 0;
    int  best_rank = 0;
    for (int i = 0; i < n; i++) {
        const vid_mode *m = &modes[i];
        if (m->width < w || m->height < h)
            continue;
        long area = (long) m->width * m->height;
        int  rank = format_rank(m);
        if (best < 0 || area < best_area || (area == best_area && rank < best_rank)) {
            best      = i;
            best_area = area;
            best_rank = rank;
        }
    }
    return best;
}

void vid_dest_rect(const vid_mode *m, int w, int h, int *x, int *y, int *rw, int *rh) {
    if (w <= m->width && h <= m->height) {
        int kx = m->width / w, ky = m->height / h;
        int k  = kx < ky ? kx : ky;
        *rw    = w * k;
        *rh    = h * k;
    } else {
        /* Source larger than the mode: only happens with the mode 13h
         * fallback (e.g. 320x240 or 640x480 into 320x200). Stretch to the
         * whole screen instead of cropping: mode 13h is shown 4:3 on a
         * monitor (non-square pixels), so a 4:3 source keeps its aspect. */
        *rw = m->width;
        *rh = m->height;
    }
    *x = (m->width - *rw) / 2;
    *y = (m->height - *rh) / 2;
}

void vid_fit_rect(const vid_mode *m, int fit, int w, int h, int *x, int *y, int *rw, int *rh) {
    if (w > m->width || h > m->height) {
        vid_dest_rect(m, w, h, x, y, rw, rh); /* mode 13h: stretch */
        return;
    }
    output_rect(fit, w, h, m->width, m->height, x, y, rw, rh);
}

int vid_select_exact(const vid_mode *modes, int n, int out_w, int out_h, int out_bpp) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        const vid_mode *m = &modes[i];
        if (m->width != out_w || m->height != out_h || (out_bpp && m->bpp != out_bpp))
            continue;
        if (best < 0 || format_rank(m) < format_rank(&modes[best]))
            best = i;
    }
    return best;
}

/* --- Pixels -------------------------------------------------------- */

/* RGB565 channels -> 8 bits, already through the gamma table (like the
 * Voodoo: widen 5/6 bits to 8, then the gamma RAMDAC). */
static uint8_t g_r8[32], g_g8[64], g_b8[32];
static uint8_t g_lut[256]; /* for 8-bit channels (vid_present32) */
static int     g_gamma_linear = -1; /* -1: tables not built yet */

void vid_set_gamma(const uint8_t lut[256]) {
    g_gamma_linear = 1;
    for (int i = 0; i < 256; i++)
        g_lut[i] = lut ? lut[i] : (uint8_t) i;
    for (int i = 0; i < 64; i++) {
        unsigned v5 = (unsigned) ((i & 31) << 3 | (i & 31) >> 2), v6 = (unsigned) (i << 2 | i >> 4);
        uint8_t  r = lut ? lut[v5] : (uint8_t) v5, g = lut ? lut[v6] : (uint8_t) v6;
        if (i < 32) {
            g_r8[i] = g_b8[i] = r;
            if (r != v5)
                g_gamma_linear = 0;
        }
        g_g8[i] = g;
        if (g != v6)
            g_gamma_linear = 0;
    }
}

static void gamma_init(void) {
    if (g_gamma_linear < 0)
        vid_set_gamma(NULL);
}

static uint32_t pack(const vid_mode *m, uint16_t p) {
    uint32_t r8 = g_r8[(p >> 11) & 0x1f], g8 = g_g8[(p >> 5) & 0x3f], b8 = g_b8[p & 0x1f];
    return ((r8 >> (8 - m->r_size)) << m->r_pos) | ((g8 >> (8 - m->g_size)) << m->g_pos) |
           ((b8 >> (8 - m->b_size)) << m->b_pos);
}

/* Mode 13h (further down, needs the options). */
static void vga_blit(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh, const void *src, int is32, int w,
                     int h, int src_stride);

/* Every target line is converted completely in g_line (system RAM) and
 * then copied into video memory in one go (memcpy: 32-bit accesses).
 * Without write-combining every access to the LFB is a separate bus
 * transfer, so per-pixel byte writes would cost several times as much.
 * When enlarging, each source line is converted only once. */
#define VID_LINE_MAX 8192 /* bytes: 2048 pixels x 32 bit */
static uint8_t g_line[VID_LINE_MAX];

static void convert_line(const vid_mode *m, uint8_t *out, const uint16_t *srow, int rw, uint32_t xstep) {
    uint32_t sx = 0;
    if (m->bpp == 15 || m->bpp == 16) {
        uint16_t *o = (uint16_t *) out;
        for (int i = 0; i < rw; i++, sx += xstep)
            o[i] = (uint16_t) pack(m, srow[sx >> 16]);
    } else if (m->bpp == 32) {
        uint32_t *o = (uint32_t *) out;
        for (int i = 0; i < rw; i++, sx += xstep)
            o[i] = pack(m, srow[sx >> 16]);
    } else {
        for (int i = 0; i < rw; i++, sx += xstep) {
            uint32_t v = pack(m, srow[sx >> 16]);
            out[i * 3 + 0] = (uint8_t) v;
            out[i * 3 + 1] = (uint8_t) (v >> 8);
            out[i * 3 + 2] = (uint8_t) (v >> 16);
        }
    }
}

void vid_blit(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh,
              const uint16_t *src, int w, int h, int src_stride) {
    gamma_init();
    if (m->bpp == 8) {
        vga_blit(m, dst, x, y, rw, rh, src, 0, w, h, src_stride);
        return;
    }
    int      bypp  = (m->bpp + 7) / 8;
    int      fast  = is_rgb565(m) && rw == w && rh == h && g_gamma_linear;
    uint32_t xstep = ((uint32_t) w << 16) / (uint32_t) rw;
    size_t   bytes = (size_t) rw * (size_t) bypp;
    int      last  = -1;
    if (bytes > sizeof g_line)
        return; /* wider than any sensible mode */

    for (int j = 0; j < rh; j++) {
        int             sy   = (int) ((long) j * h / rh);
        const uint16_t *srow = (const uint16_t *) ((const uint8_t *) src + (size_t) sy * (size_t) src_stride);
        uint8_t        *drow = dst + (size_t) (y + j) * (size_t) m->pitch + (size_t) x * (size_t) bypp;

        if (fast) {
            memcpy(drow, srow, (size_t) w * 2);
            continue;
        }
        if (sy != last) {
            convert_line(m, g_line, srow, rw, xstep);
            last = sy;
        }
        memcpy(drow, g_line, bytes);
    }
}

/* XRGB8888 (video filter) -> target format, nearest like vid_blit. */
static void convert_line32(const vid_mode *m, uint8_t *out, const uint32_t *srow, int rw, uint32_t xstep) {
    uint32_t sx = 0;
    int      bypp = (m->bpp + 7) / 8;
    for (int i = 0; i < rw; i++, sx += xstep) {
        uint32_t q = srow[sx >> 16];
        uint32_t r = g_lut[(q >> 16) & 0xff], g = g_lut[(q >> 8) & 0xff], b = g_lut[q & 0xff];
        uint32_t v = ((r >> (8 - m->r_size)) << m->r_pos) | ((g >> (8 - m->g_size)) << m->g_pos) |
                     ((b >> (8 - m->b_size)) << m->b_pos);
        if (bypp == 2)
            ((uint16_t *) out)[i] = (uint16_t) v;
        else if (bypp == 4)
            ((uint32_t *) out)[i] = v;
        else {
            out[i * 3 + 0] = (uint8_t) v;
            out[i * 3 + 1] = (uint8_t) (v >> 8);
            out[i * 3 + 2] = (uint8_t) (v >> 16);
        }
    }
}

static void vid_blit32(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh, const uint32_t *src, int w,
                       int h, int src_stride) {
    gamma_init();
    if (m->bpp == 8) {
        vga_blit(m, dst, x, y, rw, rh, src, 1, w, h, src_stride);
        return;
    }
    int      bypp  = (m->bpp + 7) / 8;
    uint32_t xstep = ((uint32_t) w << 16) / (uint32_t) rw;
    size_t   bytes = (size_t) rw * (size_t) bypp;
    int      last  = -1;
    if (bytes > sizeof g_line)
        return;
    for (int j = 0; j < rh; j++) {
        int             sy   = (int) ((long) j * h / rh);
        const uint32_t *srow = (const uint32_t *) ((const uint8_t *) src + (size_t) sy * (size_t) src_stride);
        uint8_t        *drow = dst + (size_t) (y + j) * (size_t) m->pitch + (size_t) x * (size_t) bypp;
        if (sy != last) {
            convert_line32(m, g_line, srow, rw, xstep);
            last = sy;
        }
        memcpy(drow, g_line, bytes);
    }
}

/* Palette levels (8 bit): closer together in the dark, where games of
 * the time spend most of their pixels and a step of 51 would dither
 * coarsely. Multiples of 4 (6-bit DAC). */
static const uint8_t g_lv6[VID_VGA_R] = { 0, 36, 80, 132, 188, 252 };
static const uint8_t g_lv7[VID_VGA_G] = { 0, 28, 64, 104, 148, 196, 252 };

void vid_vga_palette(uint8_t pal[768]) {
    memset(pal, 0, 768);
    for (int i = 0; i < VID_VGA_R * VID_VGA_G * VID_VGA_B; i++) {
        pal[i * 3 + 0] = (uint8_t) (g_lv6[i / (VID_VGA_G * VID_VGA_B)] >> 2);
        pal[i * 3 + 1] = (uint8_t) (g_lv7[i / VID_VGA_B % VID_VGA_G] >> 2);
        pal[i * 3 + 2] = (uint8_t) (g_lv6[i % VID_VGA_B] >> 2);
    }
}

/* --- Output stages (see video.h) ---------------------------------- */

/* NULL = not implemented. */
static const vid_scale_fn g_scalers[VID_SCALE_COUNT] = {
    vid_blit, /* VID_SCALE_NEAREST */
    NULL,     /* VID_SCALE_BILINEAR */
    NULL,     /* VID_SCALE_SCALE2X */
};

static const vid_overlay_fn g_overlays[VID_OVERLAY_COUNT] = {
    NULL, /* VID_OVERLAY_NONE: deliberately empty, means "no overlay" */
    NULL, /* VID_OVERLAY_SCANLINES */
};

static vid_options g_opt = { 1, VID_SCALE_NEAREST, 0, 0, 0, OUT_SCALE_ASPECT, 1, VID_OVERLAY_NONE, 0, 0, VID_DITHER_BLUE };

void vid_default_options(vid_options *o) {
    memset(o, 0, sizeof *o);
    o->ssaa    = 1;
    o->scaler  = VID_SCALE_NEAREST;
    o->overlay = VID_OVERLAY_NONE;
    o->fit     = OUT_SCALE_ASPECT;
    o->wc      = 1;
}

void vid_get_options(vid_options *o) {
    *o = g_opt;
}

int vid_set_options(const vid_options *o) {
    if (!o || o->ssaa != 1)
        return 0; /* anti-aliasing is done in core/, not here */
    if (o->scaler < 0 || o->scaler >= VID_SCALE_COUNT || !g_scalers[o->scaler])
        return 0;
    if (o->overlay < 0 || o->overlay >= VID_OVERLAY_COUNT ||
        (o->overlay != VID_OVERLAY_NONE && !g_overlays[o->overlay]))
        return 0;
    if (o->out_w < 0 || o->out_h < 0 || o->out_bpp < 0 || (!o->out_w != !o->out_h))
        return 0;
    if (o->fit < OUT_SCALE_ASPECT || o->fit > OUT_SCALE_NONE || (o->wc != 0 && o->wc != 1))
        return 0;
    if (o->overlay_strength < 0 || o->overlay_strength > 255)
        return 0;
    if (o->vga_dither < 0 || o->vga_dither >= VID_DITHER_COUNT)
        return 0;
    g_opt = *o;
    return 1;
}

/* --- Mode 13h -------------------------------------------------------
 * 256 colours from a 16-bit picture that the Voodoo has already dithered
 * (4x4 ordered). Picking single pixels (nearest) samples that pattern
 * irregularly and shows it as coarse stripes; cutting the bits away loses
 * what the dither encodes. Instead:
 *   1. every target pixel averages the source area it covers, at least
 *      2 x 2 pixels: any 2 x 2 window of the 4x4 Bayer pattern sums to
 *      nearly its mean, so the average is the undithered colour with
 *      about 8 bits per channel;
 *   2. gamma (g_lut) on the averaged colour;
 *   3. one ordered dither to the 6x7x6 palette (levels denser in the
 *      dark, g_lv6/g_lv7) with a fixed threshold
 *      matrix in screen coordinates: stable from frame to frame (error
 *      diffusion would crawl in motion). Blue noise lays no second
 *      regular grid over the picture; Bayer and plain rounding are
 *      options for comparison. */

/* tools/gen/bluenoise.py (void-and-cluster, 16x16, ranks 0..255) */
static const uint8_t g_blue16[16][16] = {
    { 155,  69, 141,  19, 183, 131,  67, 146,  88, 248, 186,  74, 237, 114, 208,  90 },
    {  23, 204,  97, 234,  54, 205, 173, 231,   1, 129,  26,  96, 166,  11,  62, 182 },
    { 109, 226,  37, 162, 116,  27,  99,  44, 108, 175, 211, 148,  38, 221, 139, 253 },
    {   2, 168,  80, 135, 214,  73, 254, 160, 223,  77,  52, 241, 125, 192,  84,  48 },
    { 121, 190,  58, 242,   8, 188, 127,  20, 137, 189,   9, 106,  70,  22, 158, 209 },
    { 238, 142,  29, 105, 153,  91,  56, 210,  39,  95, 229, 203, 172, 247, 102,  65 },
    {  16,  92, 225, 198,  41, 170, 235, 110, 184, 161,  63, 144,  15, 130,  40, 179 },
    { 216, 164,  72, 128, 246,  17, 140,  71,   4, 251, 120,  47,  87, 224, 199, 117 },
    { 149,  53,   6, 181,  83,  51, 194, 222,  89, 150,  30, 236, 180, 154,  61,  28 },
    { 100, 255, 206, 112, 217, 159, 122,  32, 174, 213,  76, 201, 107,   0,  81, 230 },
    { 176,  75,  34, 145,  21,  66, 232, 101,  49, 115,  12, 134,  50, 244, 195, 138 },
    {  13, 123, 191,  94, 245, 169,   7, 143, 249, 196, 157, 227,  93, 163, 118,  42 },
    { 239, 152, 228,  46, 132,  78, 207, 185,  25,  82,  64,  35, 187,  24,  68, 202 },
    {  57,  86,   3, 177, 212,  31,  98,  55, 133, 233, 171, 124, 252, 147, 220, 104 },
    { 167, 219, 113,  59, 156, 119, 240, 165, 111,  14, 215, 103,   5,  79, 178,  18 },
    { 126,  33, 193, 250,  85,  10, 218,  36, 197,  60, 151,  45, 200, 136,  43, 243 },
};

static const uint8_t g_bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

#define VGA_W_MAX 2048 /* source and target width */
static uint16_t g_acc[3][VGA_W_MAX];       /* column sums of the current target row */
static uint16_t g_x0[VGA_W_MAX], g_xn[VGA_W_MAX]; /* source columns per target column */
static uint8_t  g_thr[16][16];             /* threshold 0..254 of the active matrix */
static int      g_thr_kind = -1;
/* per channel value: index part of the level below, and how far (0..255)
 * the value lies towards the next level; the threshold decides */
static uint8_t g_base[3][256], g_frac[3][256];

static void vga_tables(int kind) {
    if (kind == g_thr_kind)
        return;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            g_thr[y][x] = kind == VID_DITHER_BAYER  ? (uint8_t) (g_bayer4[y & 3][x & 3] * 16 + 8)
                          : kind == VID_DITHER_OFF ? 127
                                                   : (uint8_t) (g_blue16[y][x] * 255 / 256);
    static const int stride[3] = { VID_VGA_G * VID_VGA_B, VID_VGA_B, 1 };
    for (int c = 0; c < 3; c++) {
        const uint8_t *lv = c == 1 ? g_lv7 : g_lv6;
        int            n  = c == 1 ? VID_VGA_G : VID_VGA_R;
        for (int v = 0; v < 256; v++) {
            int k = 0;
            while (k < n - 2 && v >= lv[k + 1])
                k++;
            int f = (v - lv[k]) * 256 / (lv[k + 1] - lv[k]);
            if (f > 255) { /* above the top level (252..255) */
                k++;
                f = 0;
            }
            g_base[c][v] = (uint8_t) (k * stride[c]);
            g_frac[c][v] = (uint8_t) f;
        }
    }
    g_thr_kind = kind;
}

/* Source range [*a, *a + *n) for target cell i of rn cells over sn source
 * cells; at least 2 wide where the source has 2. */
static void vga_span(int i, int rn, int sn, int *a, int *n) {
    int s0 = (int) ((long) i * sn / rn), s1 = (int) ((long) (i + 1) * sn / rn);
    if (s1 - s0 < 2) {
        s1 = s0 + 2 > sn ? sn : s0 + 2;
        s0 = s1 - 2 < 0 ? 0 : s1 - 2;
    }
    *a = s0;
    *n = s1 - s0;
}

static void vga_blit(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh, const void *src, int is32, int w,
                     int h, int src_stride) {
    if (w > VGA_W_MAX || rw > VGA_W_MAX || rw > (int) sizeof g_line)
        return;
    vga_tables(g_opt.vga_dither);
    for (int i = 0; i < rw; i++) {
        int a, n;
        vga_span(i, rw, w, &a, &n);
        g_x0[i] = (uint16_t) a;
        g_xn[i] = (uint16_t) n;
    }
    for (int j = 0; j < rh; j++) {
        int y0, yn;
        vga_span(j, rh, h, &y0, &yn);
        memset(g_acc, 0, sizeof g_acc);
        for (int k = 0; k < yn; k++) {
            const uint8_t *row = (const uint8_t *) src + (size_t) (y0 + k) * (size_t) src_stride;
            if (is32) {
                const uint32_t *p = (const uint32_t *) row;
                for (int c = 0; c < w; c++) {
                    g_acc[0][c] = (uint16_t) (g_acc[0][c] + ((p[c] >> 16) & 0xff));
                    g_acc[1][c] = (uint16_t) (g_acc[1][c] + ((p[c] >> 8) & 0xff));
                    g_acc[2][c] = (uint16_t) (g_acc[2][c] + (p[c] & 0xff));
                }
            } else {
                const uint16_t *p = (const uint16_t *) row;
                for (int c = 0; c < w; c++) {
                    unsigned r = (p[c] >> 11) & 31, g = (p[c] >> 5) & 63, b = p[c] & 31;
                    g_acc[0][c] = (uint16_t) (g_acc[0][c] + (r << 3 | r >> 2));
                    g_acc[1][c] = (uint16_t) (g_acc[1][c] + (g << 2 | g >> 4));
                    g_acc[2][c] = (uint16_t) (g_acc[2][c] + (b << 3 | b >> 2));
                }
            }
        }
        const uint8_t *thr = g_thr[(y + j) & 15];
        for (int i = 0; i < rw; i++) {
            unsigned sr = 0, sg = 0, sb = 0, a = g_x0[i], n = g_xn[i];
            for (unsigned c = a; c < a + n; c++) {
                sr += g_acc[0][c];
                sg += g_acc[1][c];
                sb += g_acc[2][c];
            }
            unsigned cnt = n * (unsigned) yn, half = cnt / 2, t = thr[(x + i) & 15];
            unsigned r = g_lut[(sr + half) / cnt], g = g_lut[(sg + half) / cnt], b = g_lut[(sb + half) / cnt];
            g_line[i] = (uint8_t) (g_base[0][r] + (g_frac[0][r] > t) * (VID_VGA_G * VID_VGA_B) + g_base[1][g] +
                                   (g_frac[1][g] > t) * VID_VGA_B + g_base[2][b] + (g_frac[2][b] > t));
        }
        memcpy(dst + (size_t) (y + j) * (size_t) m->pitch + (size_t) x, g_line, (size_t) rw);
    }
}

/* --- State machine -------------------------------------------------- */

#define MAX_MODES 128

static const vid_bios *g_bios;
static int             g_active;
static vid_mode        g_mode;
static uint8_t        *g_fb;
static uint8_t         g_prev_mode = 3;

/* Static instead of on the stack: the OVL runs on the game's stack, whose
 * size is unknown (~3.5 KB would be needed here otherwise). */
static uint16_t nums[MAX_MODES];
static vid_mode modes[MAX_MODES];

/* DPMI 0.9 has no unmapping (0801h is DPMI 1.0), so reuse the last
 * mapped LFB window instead of creating a new one at every
 * grSstWinOpen(). */
static uint32_t g_mapped_phys, g_mapped_size;
static uint8_t *g_mapped_ptr;

static void int10(vid_rmregs *r) {
    g_bios->int10(r);
}

static void regs_clear(vid_rmregs *r) {
    memset(r, 0, sizeof *r);
}

static uint8_t *map_lfb(uint32_t phys, uint32_t size) {
    if (g_mapped_ptr && g_mapped_phys == phys && g_mapped_size >= size)
        return g_mapped_ptr;
    uint8_t *p = (uint8_t *) g_bios->map_phys(phys, size);
    if (p) {
        g_mapped_phys = phys;
        g_mapped_size = size;
        g_mapped_ptr  = p;
    }
    return p;
}

#ifdef GLIDULATOR_TRACE
/* Trace variants: every usable VBE mode of the board into GLIDULAT.LOG
 * (the values GLIDULATOR_DISPLAY accepts), six per line. */
static char *put_num(char *p, unsigned v) {
    char t[10];
    int  k = 0;
    do
        t[k++] = (char) ('0' + v % 10);
    while ((v /= 10) && k < 9);
    while (k)
        *p++ = t[--k];
    return p;
}

static void log_modes(const vid_mode *m, int n) {
    char line[128];
    for (int i = 0; i < n; i += 6) {
        char *p = line;
        memcpy(p, "VBE modes:", 9);
        p += 9;
        for (int k = i; k < n && k < i + 6; k++) {
            *p++ = ' ';
            p    = put_num(p, (unsigned) m[k].width);
            *p++ = 'x';
            p    = put_num(p, (unsigned) m[k].height);
            *p++ = 'x';
            p    = put_num(p, (unsigned) m[k].bpp);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
}
#endif

static int try_vbe(int w, int h) {
    vid_rmregs r;
    uint8_t   *buf = (uint8_t *) g_bios->real_ptr(g_bios->dosbuf_seg, 0);
    uint8_t   *mib = buf + 512;

    memset(buf, 0, VID_DOSBUF_BYTES);
    memcpy(buf, "VBE2", 4); /* request VBE 2.0 fields */
    regs_clear(&r);
    r.eax = 0x4F00;
    r.es  = g_bios->dosbuf_seg;
    r.edi = 0;
    int10(&r);
    if ((r.eax & 0xFFFF) != 0x004F || memcmp(buf, "VESA", 4) != 0)
        return 0;
    uint16_t version = rd16(buf + 4);
    uint32_t vram    = (uint32_t) rd16(buf + 18) << 16; /* TotalMemory, 64 KB units */
    if (version < 0x0200)
        return 0; /* VBE 1.x: no LFB, deliberately not supported */

    /* Copy the mode list first: it may live in the same buffer. */
    uint32_t       lp    = rd32(buf + 14);
    const uint8_t *list  = (const uint8_t *) g_bios->real_ptr((uint16_t) (lp >> 16), (uint16_t) lp);
    int            nnums = 0;
    while (list && nnums < MAX_MODES) {
        uint16_t num = rd16(list + 2 * nnums);
        if (num == 0xFFFF)
            break;
        nums[nnums++] = num;
    }

    int n = 0;
    for (int i = 0; i < nnums; i++) {
        memset(mib, 0, 256);
        regs_clear(&r);
        r.eax = 0x4F01;
        r.ecx = nums[i];
        r.es  = g_bios->dosbuf_seg;
        r.edi = 512;
        int10(&r);
        if ((r.eax & 0xFFFF) != 0x004F)
            continue;
        if (vid_parse_mode_info(mib, version >= 0x0300, &modes[n])) {
            modes[n].mode = nums[i] & 0x01FF;
            n++;
        }
    }

#ifdef GLIDULATOR_TRACE
    log_modes(modes, n);
#endif
    /* GLIDULATOR_DISPLAY: exactly this mode if the board has it,
     * otherwise the smallest one the Glide image fits into. */
    int sel = g_opt.out_w ? vid_select_exact(modes, n, g_opt.out_w, g_opt.out_h, g_opt.out_bpp) : -1;
    if (sel < 0)
        sel = vid_select_mode(modes, n, w, h);
    if (sel < 0)
        return 0;
    vid_mode m    = modes[sel];
    uint32_t size = (uint32_t) m.pitch * (uint32_t) m.height;
    uint8_t *fb   = map_lfb(m.phys_base, size);
    if (!fb)
        return 0;

    regs_clear(&r);
    r.eax = 0x4F02;
    r.ebx = (uint32_t) m.mode | 0x4000; /* bit 14: linear framebuffer */
    int10(&r);
    if ((r.eax & 0xFFFF) != 0x004F)
        return 0;

    memset(fb, 0, size);
    g_mode = m;
    g_fb   = fb;
    if (g_opt.wc && g_bios->wc_on)
        g_bios->wc_on(m.phys_base, size, vram);
    return 1;
}

static int try_mode13(void) {
    vid_rmregs r;
    regs_clear(&r);
    r.eax = 0x0013;
    int10(&r);
    regs_clear(&r);
    r.eax = 0x0F00; /* read the current mode back: did it work? */
    int10(&r);
    if ((r.eax & 0x7F) != 0x13)
        return 0;

    uint8_t pal[768];
    vid_vga_palette(pal);
    g_bios->outb(0x3C8, 0); /* DAC write index */
    for (int i = 0; i < 768; i++)
        g_bios->outb(0x3C9, pal[i]);

    memset(&g_mode, 0, sizeof g_mode);
    g_mode.width  = 320;
    g_mode.height = 200;
    g_mode.pitch  = 320;
    g_mode.bpp    = 8;
    g_mode.mode   = 0x13;
    g_fb          = (uint8_t *) g_bios->real_ptr(0xA000, 0);
    memset(g_fb, 0, 320 * 200);
    return 1;
}

void vid_set_bios(const vid_bios *bios) {
    if (g_active)
        vid_close();
    g_bios        = bios;
    g_mapped_ptr  = NULL;
    g_mapped_phys = g_mapped_size = 0;
}

void vid_open(int w, int h) {
    if (g_active)
        vid_close();
    if (!g_bios || w <= 0 || h <= 0)
        return;

    vid_rmregs r;
    regs_clear(&r);
    r.eax = 0x0F00;
    int10(&r);
    g_prev_mode = (uint8_t) (r.eax & 0x7F);

    /* Fallback chain: VBE LFB -> mode 13h -> nothing (Glide calls keep
     * working, just without picture). The requested mode
     * (GLIDULATOR_DISPLAY) is chosen in try_vbe(). */
    if ((!g_opt.vga_only && try_vbe(w, h)) || try_mode13())
        g_active = 1;
#ifdef GLIDULATOR_TRACE
    if (g_active)
    {
        glide_trace_note("Video mode", (unsigned long) g_mode.width, (unsigned long) g_mode.height);
        glide_trace_note("  bits/pixel, pitch", (unsigned long) g_mode.bpp, (unsigned long) g_mode.pitch);
    }
#endif
}

void vid_present(const void *pixels, int w, int h, int stride_bytes) {
    if (!g_active || !pixels || w <= 0 || h <= 0)
        return;

    /* [1] Scale and convert into the target rectangle. */
    int x, y, rw, rh;
    vid_fit_rect(&g_mode, g_opt.fit, w, h, &x, &y, &rw, &rh);
    g_scalers[g_opt.scaler](&g_mode, g_fb, x, y, rw, rh, (const uint16_t *) pixels, w, h, stride_bytes);

    /* [2] Overlay (scanlines etc.) directly in the target format. */
    if (g_overlays[g_opt.overlay])
        g_overlays[g_opt.overlay](&g_mode, g_fb, x, y, rw, rh, g_opt.overlay_strength);
}

/* Like vid_present for an XRGB8888 source (video filter,
 * core/vfilter.c); always nearest. */
void vid_present32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    if (!g_active || !pixels || w <= 0 || h <= 0)
        return;
    int x, y, rw, rh;
    vid_fit_rect(&g_mode, g_opt.fit, w, h, &x, &y, &rw, &rh);
    vid_blit32(&g_mode, g_fb, x, y, rw, rh, pixels, w, h, stride_bytes);
    if (g_overlays[g_opt.overlay])
        g_overlays[g_opt.overlay](&g_mode, g_fb, x, y, rw, rh, g_opt.overlay_strength);
}

/* Some programs switch back to text mode themselves before grSstWinClose
 * and print their exit message. Writing to the LFB then garbles the text
 * (the LFB aliases the text planes), and setting the mode again clears it.
 * VBE: 4F03h (AH=0Fh is undefined in VBE modes); if the BIOS does not
 * answer, assume the mode is still ours. */
int vid_mode_lost(void) {
    if (!g_active)
        return 0;
    vid_rmregs r;
    regs_clear(&r);
    if (g_mode.mode == 0x13) {
        r.eax = 0x0F00;
        int10(&r);
        return (r.eax & 0x7F) != 0x13;
    }
    r.eax = 0x4F03;
    int10(&r);
    return (r.eax & 0xFFFF) == 0x004F && (r.ebx & 0x1FF) != g_mode.mode;
}

void vid_close(void) {
    if (!g_active)
        return;
    int lost = vid_mode_lost();
    if (g_bios->wc_off)
        g_bios->wc_off(); /* original MTRRs back before the mode goes */
    if (!lost) {
        vid_rmregs r;
        regs_clear(&r);
        r.eax = g_prev_mode; /* AH=00h: set mode, also after a VBE mode */
        int10(&r);
    }
#ifdef GLIDULATOR_TRACE
    else
        platform_log("Video mode already switched by the program, left as is\n");
#endif
    g_active = 0;
}

const vid_mode *vid_current(void) {
    return g_active ? &g_mode : NULL;
}
