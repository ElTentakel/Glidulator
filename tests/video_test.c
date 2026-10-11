/* Unit tests for the portable part of the DOS output
 * (platform/dos_ovl/video.c): parse the VBE ModeInfoBlock, choose the mode,
 * target rectangle, pixel conversion/scaling, RGB332 palette. The BIOS
 * sequence itself (vid_open/vid_close against a simulated BIOS) is tested
 * through the real OVL in tools/leload_demo (make ovl-demo). */

#include "../platform/dos_ovl/video.h"
#include "../platform/common/output_rect.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

static void make_mib(uint8_t mib[256], unsigned attr, int w, int h, int bpp, int pitch,
                     int rs, int rp, int gs, int gp, int bs, int bp) {
    memset(mib, 0, 256);
    put16(mib + 0, attr);
    put16(mib + 16, (unsigned) pitch);
    put16(mib + 18, (unsigned) w);
    put16(mib + 20, (unsigned) h);
    mib[25] = (uint8_t) bpp;
    mib[27] = 6;
    mib[31] = (uint8_t) rs; mib[32] = (uint8_t) rp;
    mib[33] = (uint8_t) gs; mib[34] = (uint8_t) gp;
    mib[35] = (uint8_t) bs; mib[36] = (uint8_t) bp;
    put16(mib + 40, 0); put16(mib + 42, 0xE000); /* PhysBasePtr 0xE0000000 */
}

static void test_parse(void) {
    uint8_t  mib[256];
    vid_mode m;

    make_mib(mib, 0x9B, 640, 480, 16, 1280, 5, 11, 6, 5, 5, 0);
    CHECK(vid_parse_mode_info(mib, 0, &m));
    CHECK(m.width == 640 && m.height == 480 && m.bpp == 16 && m.pitch == 1280);
    CHECK(m.phys_base == 0xE0000000u && m.g_size == 6);

    /* VBE 3: LinBytesPerScanLine takes precedence */
    put16(mib + 50, 2048);
    CHECK(vid_parse_mode_info(mib, 1, &m) && m.pitch == 2048);
    CHECK(vid_parse_mode_info(mib, 0, &m) && m.pitch == 1280);

    make_mib(mib, 0x1B, 640, 480, 16, 1280, 5, 11, 6, 5, 5, 0); /* no LFB */
    CHECK(!vid_parse_mode_info(mib, 0, &m));
    make_mib(mib, 0x9B, 640, 480, 8, 640, 0, 0, 0, 0, 0, 0); /* 8 bit */
    CHECK(!vid_parse_mode_info(mib, 0, &m));
    make_mib(mib, 0x9B, 640, 480, 16, 1000, 5, 11, 6, 5, 5, 0); /* pitch too small */
    CHECK(!vid_parse_mode_info(mib, 0, &m));

    /* empty masks -> standard layout */
    make_mib(mib, 0x9B, 320, 200, 15, 640, 0, 0, 0, 0, 0, 0);
    CHECK(vid_parse_mode_info(mib, 0, &m) && m.r_pos == 10 && m.g_size == 5);
}

static vid_mode mk(int w, int h, int bpp, int rs, int rp, int gs, int gp, int bs, int bp) {
    vid_mode m;
    memset(&m, 0, sizeof m);
    m.width = w; m.height = h; m.bpp = bpp; m.pitch = w * ((bpp + 7) / 8);
    m.r_size = (uint8_t) rs; m.r_pos = (uint8_t) rp;
    m.g_size = (uint8_t) gs; m.g_pos = (uint8_t) gp;
    m.b_size = (uint8_t) bs; m.b_pos = (uint8_t) bp;
    return m;
}

#define M565(w, h)  mk(w, h, 16, 5, 11, 6, 5, 5, 0)
#define M555(w, h)  mk(w, h, 15, 5, 10, 5, 5, 5, 0)
#define M888(w, h)  mk(w, h, 24, 8, 16, 8, 8, 8, 0)
#define MX888(w, h) mk(w, h, 32, 8, 16, 8, 8, 8, 0)

static void test_select(void) {
    vid_mode a[] = { M565(640, 480), MX888(320, 240), M555(320, 240), M565(320, 200) };
    CHECK(vid_select_mode(a, 4, 320, 240) == 1); /* smallest area, 32 before 15 */
    CHECK(vid_select_mode(a, 4, 320, 200) == 3); /* exact, 565 */
    CHECK(vid_select_mode(a, 4, 512, 384) == 0);
    CHECK(vid_select_mode(a, 4, 800, 600) == -1);
    vid_mode b[] = { M888(320, 240), M555(320, 240), M565(320, 240) };
    CHECK(vid_select_mode(b, 3, 320, 240) == 2);
}

static void test_rect(void) {
    int      x, y, w, h;
    vid_mode m = M565(640, 480);
    vid_dest_rect(&m, 320, 240, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 640 && h == 480); /* 2x */
    m = M565(800, 600);
    vid_dest_rect(&m, 512, 384, &x, &y, &w, &h);
    CHECK(x == 144 && y == 108 && w == 512 && h == 384); /* 1x centred */
    vid_mode v = mk(320, 200, 8, 0, 0, 0, 0, 0, 0);
    vid_dest_rect(&v, 320, 240, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 320 && h == 200); /* stretched */
    vid_dest_rect(&v, 320, 200, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 320 && h == 200);
}

static void test_blit(void) {
    /* 2x2 source: red, green / blue, white (RGB565) */
    const uint16_t src[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    uint8_t        dst[4 * 4 * 4];

    vid_mode m = M565(2, 2);
    memset(dst, 0xAA, sizeof dst);
    vid_blit(&m, dst, 0, 0, 2, 2, src, 2, 2, 4);
    CHECK(memcmp(dst, src, 8) == 0);

    m = MX888(2, 2);
    vid_blit(&m, dst, 0, 0, 2, 2, src, 2, 2, 4);
    const uint32_t *d32 = (const uint32_t *) dst;
    CHECK(d32[0] == 0x00FF0000 && d32[1] == 0x0000FF00 && d32[2] == 0x000000FF && d32[3] == 0x00FFFFFF);

    m = M555(2, 2);
    vid_blit(&m, dst, 0, 0, 2, 2, src, 2, 2, 4);
    const uint16_t *d16 = (const uint16_t *) dst;
    CHECK(d16[0] == 0x7C00 && d16[1] == 0x03E0 && d16[2] == 0x001F && d16[3] == 0x7FFF);

    m = M888(2, 2);
    vid_blit(&m, dst, 0, 0, 2, 2, src, 2, 2, 4);
    CHECK(dst[0] == 0x00 && dst[1] == 0x00 && dst[2] == 0xFF); /* B,G,R */
    CHECK(dst[9] == 0xFF && dst[10] == 0xFF && dst[11] == 0xFF);

    /* mode 13h: pure colours stay pure in every dither variant (index
     * r * 42 + g * 6 + b), the 2x2 average mixes the four */
    {
        static const uint16_t solid[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
        static const uint8_t  idx[4]   = { 210, 36, 5, 251 };
        uint16_t              flat[16];
        m = mk(4, 4, 8, 0, 0, 0, 0, 0, 0);
        for (int k = 0; k < 4; k++)
            for (int d = 0; d < VID_DITHER_COUNT; d++) {
                vid_options o;
                vid_default_options(&o);
                o.vga_dither = d;
                CHECK(vid_set_options(&o));
                for (int i = 0; i < 16; i++)
                    flat[i] = solid[k];
                vid_blit(&m, dst, 0, 0, 4, 4, flat, 4, 4, 8);
                for (int i = 0; i < 16; i++)
                    CHECK(dst[i] == idx[k]);
            }
        vid_options o;
        vid_default_options(&o);
        o.vga_dither = VID_DITHER_OFF;
        CHECK(vid_set_options(&o));
        m = mk(2, 2, 8, 0, 0, 0, 0, 0, 0);
        vid_blit(&m, dst, 0, 0, 2, 2, src, 2, 2, 4);
        CHECK(dst[0] == dst[3] && dst[0] == 3 * 42 + 4 * 6 + 3); /* mean (128, 128, 128) rounded */
        vid_default_options(&o);
        CHECK(vid_set_options(&o));
    }

    /* 2x scaling into a 4x4 565 buffer, pitch 8 */
    m = M565(4, 4);
    vid_blit(&m, dst, 0, 0, 4, 4, src, 2, 2, 4);
    d16 = (const uint16_t *) dst;
    CHECK(d16[0] == 0xF800 && d16[1] == 0xF800 && d16[2] == 0x07E0 && d16[3] == 0x07E0);
    CHECK(d16[4] == 0xF800 && d16[8] == 0x001F && d16[15] == 0xFFFF);

    /* offset: 1x1 source at (1,1) in 2x2, the rest untouched */
    m = M565(2, 2);
    memset(dst, 0, 8);
    vid_blit(&m, dst, 1, 1, 1, 1, src + 3, 1, 1, 2);
    d16 = (const uint16_t *) dst;
    CHECK(d16[0] == 0 && d16[1] == 0 && d16[2] == 0 && d16[3] == 0xFFFF);
}

static void test_palette(void) {
    uint8_t pal[768];
    vid_vga_palette(pal);
    CHECK(pal[0] == 0 && pal[1] == 0 && pal[2] == 0);
    CHECK(pal[210 * 3] == 63 && pal[210 * 3 + 1] == 0 && pal[210 * 3 + 2] == 0);
    CHECK(pal[36 * 3 + 1] == 63 && pal[5 * 3 + 2] == 63);
    CHECK(pal[251 * 3] == 63 && pal[251 * 3 + 1] == 63 && pal[251 * 3 + 2] == 63);
    CHECK(pal[42 * 3] == 9 && pal[6 * 3 + 1] == 7 && pal[1 * 3 + 2] == 9); /* denser in the dark */
    CHECK(pal[252 * 3] == 0 && pal[255 * 3 + 2] == 0);
}

/* Mode 13h from a picture the Voodoo dithered (4x4 Bayer in RGB565, as
 * the rasterizer does): 640x480 grey ramp -> 320x200. The palette values
 * of the output, averaged over areas, must give back the grey value, and
 * no column may stand out (the stripes nearest sampling produced).
 * Without dither the area average alone must give flat colour. */
static uint16_t g_vsrc[640 * 480];
static uint8_t  g_vdst[320 * 200];

static int voodoo_dither5(int c, int x, int y) {
    static const int b[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    int v = (c * 31 * 16 + b[y & 3][x & 3] * 255) / (255 * 16);
    return v > 31 ? 31 : v;
}

static void test_vga_dither(void) {
    uint8_t  pal[768];
    vid_mode m = mk(320, 200, 8, 0, 0, 0, 0, 0, 0);
    vid_vga_palette(pal);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            int c = 16 + x * 224 / 640, v = voodoo_dither5(c, x, y);
            g_vsrc[y * 640 + x] = (uint16_t) (v << 11 | (v * 2 + (v >> 4)) << 5 | v);
        }
    for (int d = 0; d < VID_DITHER_COUNT; d++) {
        vid_options o;
        vid_default_options(&o);
        o.vga_dither = d;
        CHECK(vid_set_options(&o));
        vid_blit(&m, g_vdst, 0, 0, 320, 200, g_vsrc, 640, 480, 1280);
        int worst = 0, differ = 0;
        for (int x0 = 0; x0 < 320; x0 += 4) {
            /* 4 columns (one Bayer period) x 200 rows: mean blue value
             * against the ramp */
            long sum = 0;
            for (int y = 0; y < 200; y++)
                for (int x = x0; x < x0 + 4; x++)
                    sum += pal[g_vdst[y * 320 + x] * 3 + 2] * 255 / 63;
            int err = (int) (sum / (4 * 200)) - (16 + (x0 * 2 + 4) * 224 / 640);
            if (err < 0)
                err = -err;
            if (err > worst)
                worst = err;
        }
        for (int y = 1; y < 200; y++)
            for (int x = 0; x < 320; x++)
                differ += g_vdst[y * 320 + x] != g_vdst[x];
        printf("  VGA dither %d: largest deviation per 4 columns %d, pixels unlike line 0: %d\n", d, worst, differ);
        if (d == VID_DITHER_OFF)
            CHECK(differ < 320 * 199 / 20); /* without dither: Voodoo pattern gone; rows differ only at level edges */
        else
            CHECK(worst <= 8);
    }
    vid_options o;
    vid_default_options(&o);
    CHECK(vid_set_options(&o));
}

/* requested mode (GLIDULATOR_DISPLAY) and fitting (GLIDULATOR_SCALE). */
static void test_display_scale(void) {
    vid_mode a[4];
    memset(a, 0, sizeof a);
    a[0].width = 1280; a[0].height = 720; a[0].bpp = 32;
    a[1].width = 1280; a[1].height = 720; a[1].bpp = 16;
    a[1].r_size = 5; a[1].r_pos = 11; a[1].g_size = 6; a[1].g_pos = 5; a[1].b_size = 5;
    a[2].width = 640;  a[2].height = 480; a[2].bpp = 16;
    a[3].width = 1920; a[3].height = 1080; a[3].bpp = 32;
    CHECK(vid_select_exact(a, 4, 1280, 720, 0) == 1);  /* 565 before 32 bit */
    CHECK(vid_select_exact(a, 4, 1280, 720, 32) == 0); /* bits given */
    CHECK(vid_select_exact(a, 4, 1366, 768, 0) == -1);
    int x, y, w, h;
    vid_fit_rect(&a[0], OUT_SCALE_ASPECT, 512, 384, &x, &y, &w, &h);
    CHECK(x == 160 && y == 0 && w == 960 && h == 720);
    vid_fit_rect(&a[0], OUT_SCALE_NONE, 512, 384, &x, &y, &w, &h);
    CHECK(x == 384 && y == 168 && w == 512 && h == 384);
    vid_fit_rect(&a[0], OUT_SCALE_STRETCH, 512, 384, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 1280 && h == 720);
    vid_mode m13;
    memset(&m13, 0, sizeof m13);
    m13.width = 320; m13.height = 200; m13.bpp = 8;
    vid_fit_rect(&m13, OUT_SCALE_NONE, 640, 480, &x, &y, &w, &h); /* too large: stretch */
    CHECK(x == 0 && y == 0 && w == 320 && h == 200);
    int dw, dh, db;
    output_display_parse("1280x720", &dw, &dh, &db);
    CHECK(dw == 1280 && dh == 720 && db == 0);
    output_display_parse("1920X1080x16", &dw, &dh, &db);
    CHECK(dw == 1920 && dh == 1080 && db == 16);
    output_display_parse("glide", &dw, &dh, &db);
    CHECK(dw == 0 && dh == 0 && db == 0);
}

static void test_options(void) {
    vid_options o;
    vid_default_options(&o);
    CHECK(o.ssaa == 1 && o.scaler == VID_SCALE_NEAREST && o.overlay == VID_OVERLAY_NONE);
    CHECK(vid_set_options(&o)); /* the default must always work */

    /* not implemented -> explicitly rejected */
    vid_options x = o; x.scaler = VID_SCALE_BILINEAR;       CHECK(!vid_set_options(&x));
    x = o; x.scaler = VID_SCALE_SCALE2X;                    CHECK(!vid_set_options(&x));
    x = o; x.overlay = VID_OVERLAY_SCANLINES;               CHECK(!vid_set_options(&x));
    x = o; x.ssaa = 2;                                      CHECK(!vid_set_options(&x));
    x = o; x.out_w = 640;                                   CHECK(!vid_set_options(&x)); /* width only */
    x = o; x.fit = OUT_SCALE_NONE + 1;                      CHECK(!vid_set_options(&x));
    /* GLIDULATOR_DISPLAY/SCALE */
    x = o; x.out_w = 1280; x.out_h = 720; x.fit = OUT_SCALE_NONE; CHECK(vid_set_options(&x));
    vid_options g;
    vid_get_options(&g);
    CHECK(g.out_w == 1280 && g.out_h == 720 && g.fit == OUT_SCALE_NONE);
    CHECK(vid_set_options(&o));
    x = o; x.scaler = VID_SCALE_COUNT;                      CHECK(!vid_set_options(&x));
    x = o; x.overlay = -1;                                  CHECK(!vid_set_options(&x));
    CHECK(!vid_set_options(NULL));
}

/* Gamma table: mid grey 0x8410 (r=b=0x84, g=0x82) becomes 0x7B through a
 * table "x -> 255 - x" (green 0x82 -> 0x7D); the 565 fast path must not
 * copy any more then. NULL restores linear. */
static void test_gamma(void) {
    uint8_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = (uint8_t) (255 - i);
    const uint16_t src[1] = { 0x8410 };
    uint8_t        dst[4];
    vid_set_gamma(lut);
    vid_mode m = MX888(1, 1);
    vid_blit(&m, dst, 0, 0, 1, 1, src, 1, 1, 2);
    CHECK(*(const uint32_t *) dst == 0x007B7D7B); /* g6 0x20 -> 0x82 -> 0x7D */
    m = M565(1, 1);
    vid_blit(&m, dst, 0, 0, 1, 1, src, 1, 1, 2);
    CHECK(*(const uint16_t *) dst == 0x7BEF);
    vid_set_gamma(NULL);
    vid_blit(&m, dst, 0, 0, 1, 1, src, 1, 1, 2);
    CHECK(*(const uint16_t *) dst == 0x8410);
}

int main(void) {
    test_parse();
    test_select();
    test_rect();
    test_blit();
    test_gamma();
    test_palette();
    test_vga_dither();
    test_options();
    test_display_scale();
    if (failures) {
        fprintf(stderr, "video_test: %d errors\n", failures);
        return 1;
    }
    printf("video_test: ok\n");
    return 0;
}
