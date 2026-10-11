/* setenv/unsetenv despite -std=c99 */
#define _POSIX_C_SOURCE 200112L

/* Main test harness: core/ + platform/linux rendering into memory. Every
 * test draws with the real Glide API, checks concrete pixels and some write
 * PNGs to tests/golden/ (which must stay unchanged; see `git status`). The
 * PNG output is a test tool only (libpng); core/, platform/ and the real
 * targets do not depend on it. */

#include "glidulator/glide2x.h"
#include "glidulator/glide_state.h"
#include "glidulator/config.h"
#include "glidulator/platform.h"
#include "glidulator/renderer.h"
#include "../../platform/common/output_rect.h"
#include "../../core/glide_internal.h" /* g_fsaa_n, glide_fsaa_frame (test 19) */

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t *g_last_frame;
static int             g_last_w, g_last_h;

/* g_copy_frame: copy instead of pointer (the OSD is only inside the
 * frame during platform_blit()). */
static int      g_copy_frame;
static uint16_t g_frame_copy[800 * 600];

/* video filter (test 20): last 32-bit image */
static uint32_t g_frame32[800 * 600];
static int      g_have32;

void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    (void) stride_bytes;
    if (w * h <= 800 * 600)
        memcpy(g_frame32, pixels, (size_t) w * (size_t) h * 4);
    g_have32  = 1;
    g_last_w  = w;
    g_last_h  = h;
}

/* test 22: every g_dump_every-th presented frame as <g_dump_prefix>NN.png */
static const char *g_dump_prefix;
static int         g_dump_every, g_dump_n, g_blits;
static int         save_png(const char *path);

static void capture_blit(const void *pixels, int w, int h, int stride_bytes) {
    (void) stride_bytes;
    g_last_frame = (const uint16_t *) pixels;
    if (g_copy_frame && w * h <= 800 * 600) {
        memcpy(g_frame_copy, pixels, (size_t) w * (size_t) h * 2);
        g_last_frame = g_frame_copy;
    }
    g_last_w     = w;
    g_last_h     = h;
    g_blits++;
    if (g_dump_prefix && g_dump_n++ % g_dump_every == 0) {
        char path[256];
        snprintf(path, sizeof path, "%s%02d.png", g_dump_prefix, g_dump_n / g_dump_every);
        save_png(path);
    }
}

/* platform/linux does not define platform_blit(); this test captures the
 * frame for verification, see capture_blit() above. */
void platform_blit(const void *pixels, int w, int h, int stride_bytes) {
    capture_blit(pixels, w, h, stride_bytes);
}

static int check_pixel(int x, int y, uint16_t expected, const char *what) {
    uint16_t got = g_last_frame[y * g_last_w + x];
    if (got != expected) {
        fprintf(stderr, "Pixel (%d,%d) %s: expected %04x, got %04x\n", x, y, what, expected, got);
        return 1;
    }
    return 0;
}

static void rgb565_to_rgb888(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (px >> 11) & 0x1f;
    uint8_t g6 = (px >> 5) & 0x3f;
    uint8_t b5 = px & 0x1f;
    *r = (uint8_t) ((r5 << 3) | (r5 >> 2));
    *g = (uint8_t) ((g6 << 2) | (g6 >> 4));
    *b = (uint8_t) ((b5 << 3) | (b5 >> 2));
}

/* Saves the frame last captured by platform_blit() as a PNG for visual
 * checks. Test tool only. */
static int save_png(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "save_png: fopen(%s) failed\n", path);
        return 1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        fclose(fp);
        return 1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32) g_last_w, (png_uint_32) g_last_h, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = (uint8_t *) malloc((size_t) g_last_w * 3);
    for (int y = 0; y < g_last_h; y++) {
        for (int x = 0; x < g_last_w; x++) {
            rgb565_to_rgb888(g_last_frame[y * g_last_w + x], &row[x * 3], &row[x * 3 + 1], &row[x * 3 + 2]);
        }
        png_write_row(png, row);
    }
    free(row);

    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    printf("  PNG saved: %s\n", path);
    return 0;
}

#define W 320
#define H 240

static int test_flat_triangle(void) {
    printf("-- Test 1: flat-shaded triangle --\n");

    if (!grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                       GR_ORIGIN_UPPER_LEFT, 1, 0)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }

    const GrColor_t clear_color = 0x00000000; /* black, 0x00RRGGBB (GR_COLORFORMAT_ARGB) */
    grBufferClear(clear_color, 0xff, 0);

    /* Completely inside the window, vertices deliberately NOT sorted by y
     * (tests the sorting in grDrawTriangle()). */
    GrVertex v0 = { 32.0f, 8.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v1 = { 8.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v2 = { 56.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    grDrawTriangle(&v1, &v0, &v2);

    grBufferSwap(1);

    if (!g_last_frame) {
        fprintf(stderr, "grBufferSwap did not call platform_blit\n");
        return 1;
    }

    int fail = 0;
    /* centroid of the triangle: clearly inside, must be red */
    fail |= check_pixel(32, 45, 0xf800, "triangle centre (red, RGB565)");
    /* points outside the triangle: clear colour (black) */
    fail |= check_pixel(2, 2, 0x0000, "top left (outside)");
    fail |= check_pixel(W - 2, 2, 0x0000, "top right (outside)");
    fail |= check_pixel(2, H - 2, 0x0000, "bottom left (outside)");

    if (fail) {
        fprintf(stderr, "Test 1 FAILED\n");
        return 1;
    }

    save_png("tests/golden/flat_triangle.png");
    printf("  ok: centre red, corners empty\n");
    return 0;
}

#define TEX_SIZE 32
#define TILE     8
#define WHITE    0xffff /* RGB565 */
#define BLUE     0x001f

static int test_textured_triangle(void) {
    printf("-- Test 2: textured triangle --\n");

    /* 32x32 checkerboard, 8x8 tiles, white/blue. */
    static uint16_t tex[TEX_SIZE * TEX_SIZE];
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int tile = (x / TILE) + (y / TILE);
            tex[y * TEX_SIZE + x] = (tile % 2 == 0) ? WHITE : BLUE;
        }
    }

    if (!grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                       GR_ORIGIN_UPPER_LEFT, 1, 0)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }

    /* GR_LOD_32 = 3 = 8-log2(32). */
    GrTexInfo info;
    info.smallLod = GR_LOD_32;
    info.largeLod = GR_LOD_32;
    info.aspectRatio = GR_ASPECT_1x1;
    info.format = GR_TEXFMT_RGB_565;
    info.data = tex;
    grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    /* Like a real application: colour = texture (Glide's default would be
     * the iterated vertex colour), point sampling for exactly checkable
     * texels. */
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);

    const GrColor_t clear_color = 0x0000ff00; /* green, differs from both tile colours */
    grBufferClear(clear_color, 0xff, 0);

    /* Right triangle (8,8)-(8,40)-(40,40), mapped 1:1 onto the 32x32 texture
     * (no rotation/shear, so the expected values are easy to verify by
     * hand). Right angle at (8,40), hypotenuse from (8,8) to (40,40). Glide
     * texture coordinates run 0..256 along the long edge regardless of the
     * texture size: pixels 8..40 = texels 0..32 = s/t 0..256. oow=1 (no
     * perspective). */
    GrVertex v0 = { 8.0f, 8.0f, 0, 255.0f, 255.0f, 255.0f, 0, 255.0f, 1.0f, { { 0.0f, 0.0f, 1.0f }, { 0, 0, 0 } } };
    GrVertex v1 = { 8.0f, 40.0f, 0, 255.0f, 255.0f, 255.0f, 0, 255.0f, 1.0f, { { 0.0f, 256.0f, 1.0f }, { 0, 0, 0 } } };
    GrVertex v2 = { 40.0f, 40.0f, 0, 255.0f, 255.0f, 255.0f, 0, 255.0f, 1.0f, { { 256.0f, 256.0f, 1.0f }, { 0, 0, 0 } } };
    grDrawTriangle(&v0, &v1, &v2);

    grBufferSwap(1);

    if (!g_last_frame) {
        fprintf(stderr, "grBufferSwap did not call platform_blit\n");
        return 1;
    }

    int fail = 0;
    /* rel=(x-8,y-8); inside the triangle rel_y > rel_x is needed (below the
     * hypotenuse) so the point is safely not on the edge. Tile index =
     * rel/8; even sum = white, odd = blue (see generation above). */
    fail |= check_pixel(12, 28, WHITE, "tile (0,2) -- white");
    fail |= check_pixel(28, 36, BLUE, "tile (2,3) -- blue");
    /* Above the hypotenuse (rel_x > rel_y): outside the triangle, must be
     * the clear colour; the texture must not bleed out. */
    fail |= check_pixel(36, 12, 0x07e0, "above the hypotenuse (outside)");

    if (fail) {
        fprintf(stderr, "Test 2 FAILED\n");
        return 1;
    }

    save_png("tests/golden/textured_triangle.png");
    printf("  ok: checkerboard sampled correctly, triangle edge holds\n");
    return 0;
}

/* Expected values computed by hand (texels * bytes, sum rounded up to 8
 * bytes). Games abort if grTexTextureMemRequired() returns 0 ("ctMemAlloc
 * Attempt to allocate zero or less bytes"). */
static int test_tex_mem_queries(void) {
    printf("-- Test 3: texture memory queries --\n");
    struct {
        GrLOD_t small, large;
        GrAspectRatio_t aspect;
        GrTextureFormat_t fmt;
        FxU32 even_odd, expected;
        const char *what;
    } cases[] = {
        { GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, GR_MIPMAPLEVELMASK_BOTH, 131072, "256x256 565" },
        { GR_LOD_1, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, GR_MIPMAPLEVELMASK_BOTH, 174768, "mipmap chain 256..1 565" },
        { GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_P_8, GR_MIPMAPLEVELMASK_BOTH, 65536, "256x256 P8" },
        { GR_LOD_256, GR_LOD_256, GR_ASPECT_8x1, GR_TEXFMT_ARGB_1555, GR_MIPMAPLEVELMASK_BOTH, 16384, "256x32 1555" },
        { GR_LOD_8, GR_LOD_8, GR_ASPECT_1x8, GR_TEXFMT_ALPHA_8, GR_MIPMAPLEVELMASK_BOTH, 8, "1x8 A8" },
        { GR_LOD_128, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, GR_MIPMAPLEVELMASK_EVEN, 131072, "EVEN only" },
        { GR_LOD_128, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, GR_MIPMAPLEVELMASK_ODD, 32768, "ODD only" },
        { GR_LOD_256, GR_LOD_1, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, GR_MIPMAPLEVELMASK_BOTH, 0, "small/large swapped" },
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        GrTexInfo info = { cases[i].small, cases[i].large, cases[i].aspect, cases[i].fmt, NULL };
        FxU32 got = grTexTextureMemRequired(cases[i].even_odd, &info);
        if (got != cases[i].expected) {
            fprintf(stderr, "grTexTextureMemRequired(%s): expected %u, got %u\n", cases[i].what,
                    (unsigned) cases[i].expected, (unsigned) got);
            fails++;
        }
    }
    if (grTexCalcMemRequired(GR_LOD_1, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565) != 174768) {
        fprintf(stderr, "grTexCalcMemRequired wrong\n");
        fails++;
    }
    FxU32 avail = grTexMaxAddress(GR_TMU0) - grTexMinAddress(GR_TMU0);
    if (avail < 2u * 1024 * 1024 - 8) {
        fprintf(stderr, "grTexMax-MinAddress = %u, expected ~2 MB (reported tmuRam)\n", (unsigned) avail);
        fails++;
    }
    if (fails)
        return 1;
    printf("  ok: memory requirements and address range match\n");
    return 0;
}


/* --- Depth buffer, mipmaps, perspective, combine ----------------------- */

static GrVertex vtx(float x, float y, float r, float g, float b, float a, float ooz, float oow, float s, float t) {
    GrVertex v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.r = r; v.g = g; v.b = b; v.a = a; v.ooz = ooz; v.oow = oow;
    v.tmuvtx[0].sow = s * oow; /* Glide expects S/W, T/W */
    v.tmuvtx[0].tow = t * oow;
    v.tmuvtx[0].oow = oow;
    return v;
}

/* Axis-aligned rectangle as a polygon (tests grDrawPolygonVertexList). */
static void quad(float x0, float y0, float x1, float y1, float r, float g, float b, float ooz) {
    GrVertex q[4];
    q[0] = vtx(x0, y0, r, g, b, 255, ooz, 1, 0, 0);
    q[1] = vtx(x1, y0, r, g, b, 255, ooz, 1, 0, 0);
    q[2] = vtx(x1, y1, r, g, b, 255, ooz, 1, 0, 0);
    q[3] = vtx(x0, y1, r, g, b, 255, ooz, 1, 0, 0);
    grDrawPolygonVertexList(4, q);
}

static int open_320x240(void) {
    if (!grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }
    return 0;
}

/* Glide Z: ooz = 65535/z, larger = nearer -> GR_CMP_GREATER. */
static int test_zbuffer(void) {
    printf("-- Test 4: Z-buffer --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grDepthBufferMode(GR_DEPTHBUFFER_ZBUFFER);
    grDepthBufferFunction(GR_CMP_GREATER);
    grDepthMask(FXTRUE);
    grBufferClear(0, 0, 0); /* colour black, depth 0 = infinitely far */

    quad(10, 10, 60, 60, 255, 0, 0, 1000);  /* red, middle depth */
    quad(30, 30, 80, 80, 0, 255, 0, 500);   /* green, further back: visible only outside red */
    quad(50, 50, 70, 70, 0, 0, 255, 2000);  /* blue, in front: covers both */
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    quad(100, 10, 120, 30, 255, 255, 255, 0); /* without depth test: always drawn */
    grBufferSwap(1);

    int fail = 0;
    fail |= check_pixel(40, 40, 0xf800, "red in front of green (overlap)");
    fail |= check_pixel(70, 40, 0x07e0, "green outside red");
    fail |= check_pixel(55, 55, 0x001f, "blue in front of red");
    fail |= check_pixel(65, 65, 0x001f, "blue in front of green");
    fail |= check_pixel(110, 20, 0xffff, "without Z test");
    if (fail) {
        fprintf(stderr, "Test 4 FAILED\n");
        return 1;
    }
    save_png("tests/golden/zbuffer.png");
    printf("  ok: depth occlusion correct\n");
    return 0;
}

/* P8 texture with palette, aspect ratio 2x1 (S twice as long), three mip
 * levels 64x32, 32x16, 16x8, each a single colour so the chosen level can
 * be read directly from the colour. */
static int test_mipmap_p8(void) {
    printf("-- Test 5: mipmaps, P8 + palette, 2x1 --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);

    static uint8_t data[64 * 32 + 32 * 16 + 16 * 8];
    memset(data, 1, 64 * 32);
    memset(data + 64 * 32, 2, 32 * 16);
    memset(data + 64 * 32 + 32 * 16, 3, 16 * 8);
    GuTexPalette pal;
    memset(&pal, 0, sizeof pal);
    pal.data[1] = 0xff0000; /* LOD 64: red */
    pal.data[2] = 0x00ff00; /* LOD 32: green */
    pal.data[3] = 0x0000ff; /* LOD 16: blue */

    GrTexInfo info;
    info.largeLod    = GR_LOD_64;
    info.smallLod    = GR_LOD_16;
    info.aspectRatio = GR_ASPECT_2x1;
    info.format      = GR_TEXFMT_P_8;
    info.data        = data;
    FxU32 addr = 4096; /* deliberately not 0: checks the base address correction */
    if (grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &info) != 64 * 32 + 32 * 16 + 16 * 8) {
        fprintf(stderr, "grTexTextureMemRequired wrong\n");
        return 1;
    }
    grTexDownloadTable(GR_TMU0, GR_TEXTABLE_PALETTE, &pal);
    grTexDownloadMipMap(GR_TMU0, addr, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, addr, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grBufferClear(0, 0, 0);

    /* s 0..256, t 0..128 (2x1). The size on screen selects the level:
     * 64x32 pixels = 1 texel/pixel (LOD 64), 32x16 = LOD 32, 16x8 = LOD 16. */
    struct { float x, y, w, h; uint16_t expect; const char *what; } q[] = {
        { 10, 10, 64, 32, 0xf800, "64x32 pixels -> LOD 64 (red)" },
        { 90, 10, 32, 16, 0x07e0, "32x16 pixels -> LOD 32 (green)" },
        { 130, 10, 16, 8, 0x001f, "16x8 pixels -> LOD 16 (blue)" },
    };
    for (int i = 0; i < 3; i++) {
        GrVertex v[4];
        v[0] = vtx(q[i].x, q[i].y, 255, 255, 255, 255, 0, 1, 0, 0);
        v[1] = vtx(q[i].x + q[i].w, q[i].y, 255, 255, 255, 255, 0, 1, 256, 0);
        v[2] = vtx(q[i].x + q[i].w, q[i].y + q[i].h, 255, 255, 255, 255, 0, 1, 256, 128);
        v[3] = vtx(q[i].x, q[i].y + q[i].h, 255, 255, 255, 255, 0, 1, 0, 128);
        grDrawPolygonVertexList(4, v);
    }
    grBufferSwap(1);

    int fail = 0;
    for (int i = 0; i < 3; i++)
        fail |= check_pixel((int) (q[i].x + q[i].w / 2), (int) (q[i].y + q[i].h / 2), q[i].expect, q[i].what);
    if (fail) {
        fprintf(stderr, "Test 5 FAILED\n");
        return 1;
    }
    save_png("tests/golden/mipmap_p8.png");
    printf("  ok: right mipmap level per size, palette and 2x1 correct\n");
    return 0;
}

/* Perspective: 32x1 texture, column x has colour x (raw RGB565, blue
 * 0..31 only). A strip from pixel 0 (oow 1, s 0) to pixel 256 (oow 0.25,
 * i.e. 4x as far, s 256). Perspective-correct, the middle of the screen
 * has s = (0.5*0 + 0.5*64)/(0.5*1 + 0.5*0.25) = 51.2 -> texel 51.2/8 = 6.4;
 * linear (wrong) it would be texel 16. */
static int test_perspective(void) {
    printf("-- Test 6: perspective-correct textures --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    static uint16_t tex[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            tex[y * 32 + x] = (uint16_t) x;
    GrTexInfo info;
    info.largeLod = info.smallLod = GR_LOD_32;
    info.aspectRatio = GR_ASPECT_1x1;
    info.format      = GR_TEXFMT_RGB_565;
    info.data        = tex;
    grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grBufferClear(0, 0, 0);

    GrVertex v[4];
    v[0] = vtx(0, 100, 255, 255, 255, 255, 0, 1.0f, 0, 0);
    v[1] = vtx(256, 100, 255, 255, 255, 255, 0, 0.25f, 256, 0);
    v[2] = vtx(256, 140, 255, 255, 255, 255, 0, 0.25f, 256, 256);
    v[3] = vtx(0, 140, 255, 255, 255, 255, 0, 1.0f, 0, 256);
    grDrawPolygonVertexList(4, v);
    grBufferSwap(1);

    int texel = g_last_frame[120 * g_last_w + 128] & 0x1f;
    printf("  texel in the picture centre: %d (perspective expects 6, linear would be 16)\n", texel);
    if (texel < 5 || texel > 7) {
        fprintf(stderr, "Test 6 FAILED\n");
        return 1;
    }
    return 0;
}

static int near565(uint16_t got, int r8, int g8, int b8) {
    int r = (got >> 11) << 3, g = ((got >> 5) & 0x3f) << 2, b = (got & 0x1f) << 3;
    return abs(r - r8) <= 9 && abs(g - g8) <= 5 && abs(b - b8) <= 9;
}

static int test_combine_blend_cull(void) {
    printf("-- Test 7: combine, alpha blending, culling --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0x00ff0000, 0, 0); /* red */
    int fail = 0;

    /* constant colour as "local": the vertex colours (white) must be ignored. */
    grConstantColorValue(0xff00ff00);
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT,
                   GR_COMBINE_OTHER_NONE, FXFALSE);
    quad(10, 10, 30, 30, 255, 255, 255, 0);

    /* Blending blue with alpha 128 over red -> about (127, 0, 128). */
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_ITERATED, FXFALSE);
    grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
    GrVertex q[4];
    q[0] = vtx(40, 10, 0, 0, 255, 128, 0, 1, 0, 0);
    q[1] = vtx(60, 10, 0, 0, 255, 128, 0, 1, 0, 0);
    q[2] = vtx(60, 30, 0, 0, 255, 128, 0, 1, 0, 0);
    q[3] = vtx(40, 30, 0, 0, 255, 128, 0, 1, 0, 0);
    grDrawPolygonVertexList(4, q);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);

    /* Culling: the same triangle once in each winding. Glide's area
     * dxAB*dyBC - dxBC*dyAB is +400 for a,b,c here (stays with
     * GR_CULL_NEGATIVE), negative for d,f,e (rejected). */
    grCullMode(GR_CULL_NEGATIVE);
    GrVertex a = vtx(80, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex b = vtx(100, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex c = vtx(80, 30, 255, 255, 255, 255, 0, 1, 0, 0);
    float area = (a.x - b.x) * (b.y - c.y) - (b.x - c.x) * (a.y - b.y);
    grDrawTriangle(&a, &b, &c); /* area > 0 -> drawn */
    GrVertex d = vtx(120, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex e = vtx(140, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex f = vtx(120, 30, 255, 255, 255, 255, 0, 1, 0, 0);
    grDrawTriangle(&d, &f, &e); /* reversed order -> area < 0 -> rejected */
    grCullMode(GR_CULL_DISABLE);
    grBufferSwap(1);

    fail |= check_pixel(20, 20, 0x07e0, "constant colour green");
    uint16_t blended = g_last_frame[20 * g_last_w + 50];
    if (!near565(blended, 127, 0, 128)) {
        fprintf(stderr, "Blending: expected ~(127,0,128), got %04x\n", blended);
        fail = 1;
    }
    if (area <= 0) {
        fprintf(stderr, "test assumption wrong: area %f\n", area);
        fail = 1;
    }
    fail |= check_pixel(84, 14, 0xffff, "positive triangle drawn");
    fail |= check_pixel(124, 14, 0xf800, "negative triangle culled");
    if (fail) {
        fprintf(stderr, "Test 7 FAILED\n");
        return 1;
    }
    save_png("tests/golden/combine_blend_cull.png");
    printf("  ok: constant colour, blending, culling\n");
    return 0;
}


/* Real triangles from the demo "Nature" (tests/data/nature_tris.inc),
 * each drawn on its own with a 128x128 texture whose texels encode their
 * own position (raw RGB565 = s<<7 | t). For every drawn pixel the sampled
 * texel must equal the exact perspective value: s = (plane sow)/(plane
 * oow) at the pixel centre, in Glide's 0..256 space -> texel s/2 for a 128
 * texture, with wrapping. Guards against texture coordinates jumping at
 * triangle edges. */
static const float g_nature_tris[][5] = {
#include "../../tests/data/nature_tris.inc"
};

static int test_demo_triangles(void) {
    printf("-- Test 8: perspective texture coordinates, real demo triangles --\n");
    if (!grSstWinOpen(0, GR_RESOLUTION_512x384, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }
    grDitherMode(GR_DITHER_DISABLE);
    static uint16_t tex[128 * 128];
    for (int t = 0; t < 128; t++)
        for (int s = 0; s < 128; s++)
            tex[t * 128 + s] = (uint16_t) ((s << 7) | t);
    GrTexInfo info;
    info.largeLod = info.smallLod = GR_LOD_128;
    info.aspectRatio = GR_ASPECT_1x1;
    info.format      = GR_TEXFMT_RGB_565;
    info.data        = tex;
    grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);

    int ntri = (int) (sizeof g_nature_tris / sizeof g_nature_tris[0]) / 3;
    int bad_tris = 0;
    for (int i = 0; i < ntri; i++) {
        const float(*d)[5] = &g_nature_tris[i * 3];
        GrVertex v[3];
        for (int k = 0; k < 3; k++) {
            memset(&v[k], 0, sizeof v[k]);
            v[k].x = d[k][0]; v[k].y = d[k][1]; v[k].oow = d[k][2];
            v[k].tmuvtx[0].sow = d[k][3]; v[k].tmuvtx[0].tow = d[k][4];
            v[k].r = v[k].g = v[k].b = v[k].a = 255.0f;
        }
        grBufferClear(0x00ffffff, 0, 0); /* 0xffff cannot be a texel (bits 14/15 free) */
        grDrawTriangle(&v[0], &v[1], &v[2]);
        grBufferSwap(1);

        /* planes for oow, sow, tow from the corner values (double). */
        double x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y, x2 = v[2].x, y2 = v[2].y;
        double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        int    pixels = 0, wrong = 0, worst = 0, wx = 0, wy = 0;
        for (int y = 0; y < g_last_h; y++) {
            for (int x = 0; x < g_last_w; x++) {
                uint16_t got = g_last_frame[y * g_last_w + x];
                if (got == 0xffff)
                    continue;
                pixels++;
                double px = x + 0.5, py = y + 0.5;
                double l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
                double l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det;
                double l0 = 1.0 - l1 - l2;
                double oow = l0 * v[0].oow + l1 * v[1].oow + l2 * v[2].oow;
                double sv  = (l0 * v[0].tmuvtx[0].sow + l1 * v[1].tmuvtx[0].sow + l2 * v[2].tmuvtx[0].sow) / oow;
                double tv  = (l0 * v[0].tmuvtx[0].tow + l1 * v[1].tmuvtx[0].tow + l2 * v[2].tmuvtx[0].tow) / oow;
                int es = ((int) floor(sv / 2.0)) & 127, et = ((int) floor(tv / 2.0)) & 127;
                int gs = got >> 7, gt = got & 127;
                int ds = abs(gs - es), dt = abs(gt - et);
                if (ds > 64) ds = 128 - ds;
                if (dt > 64) dt = 128 - dt;
                int err = ds > dt ? ds : dt;
                if (err > 1)
                    wrong++;
                if (err > worst) {
                    worst = err;
                    wx    = x;
                    wy    = y;
                }
            }
        }
        if (wrong) {
            bad_tris++;
            if (bad_tris <= 8)
                fprintf(stderr, "  triangle %2d: %d of %d pixels wrong, worst error %d texels at (%d,%d)\n",
                        i, wrong, pixels, worst, wx, wy);
        }
    }
    if (bad_tris) {
        fprintf(stderr, "Test 8 FAILED: %d of %d triangles with wrong texels\n", bad_tris, ntri);
        return 1;
    }
    printf("  ok: all %d triangles perspective-correct to the texel\n", ntri);
    return 0;
}


/* Like test 8 but with a mip chain 128..2 (as in the demo): every level
 * is a single colour encoding its number. The chosen level must match the
 * expected one (+-1): LOD = log2(max. texel step per pixel) in the 256
 * space, relative to the largest level (LOD 1 = 128). */
static int test_demo_triangles_mipmap(void) {
    printf("-- Test 9: mipmap level selection, real demo triangles --\n");
    if (!grSstWinOpen(0, GR_RESOLUTION_512x384, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }
    grDitherMode(GR_DITHER_DISABLE);
    static uint16_t tex[128 * 128 * 2];
    size_t off = 0;
    for (int lvl = 0; lvl <= 6; lvl++) { /* 128,64,...,2 */
        int n = 128 >> lvl;
        for (int i = 0; i < n * n; i++)
            tex[off + i] = (uint16_t) (lvl + 1);
        off += (size_t) n * n;
    }
    GrTexInfo info;
    info.largeLod = GR_LOD_128;
    info.smallLod = GR_LOD_2;
    info.aspectRatio = GR_ASPECT_1x1;
    info.format      = GR_TEXFMT_RGB_565;
    info.data        = tex;
    grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);

    int ntri = (int) (sizeof g_nature_tris / sizeof g_nature_tris[0]) / 3;
    int bad_tris = 0;
    for (int i = 0; i < ntri; i++) {
        const float(*d)[5] = &g_nature_tris[i * 3];
        GrVertex v[3];
        for (int k = 0; k < 3; k++) {
            memset(&v[k], 0, sizeof v[k]);
            v[k].x = d[k][0]; v[k].y = d[k][1]; v[k].oow = d[k][2];
            v[k].tmuvtx[0].sow = d[k][3]; v[k].tmuvtx[0].tow = d[k][4];
            v[k].r = v[k].g = v[k].b = v[k].a = 255.0f;
        }
        grBufferClear(0x00ffffff, 0, 0);
        grDrawTriangle(&v[0], &v[1], &v[2]);
        grBufferSwap(1);

        double x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y, x2 = v[2].x, y2 = v[2].y;
        double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        /* gradients of the planes (oow, sow, tow) in x and y. */
        double g[3][2];
        double val[3][3] = { { v[0].oow, v[1].oow, v[2].oow },
                             { v[0].tmuvtx[0].sow, v[1].tmuvtx[0].sow, v[2].tmuvtx[0].sow },
                             { v[0].tmuvtx[0].tow, v[1].tmuvtx[0].tow, v[2].tmuvtx[0].tow } };
        for (int q = 0; q < 3; q++) {
            double db = val[q][1] - val[q][0], dc = val[q][2] - val[q][0];
            g[q][0] = (db * (y2 - y0) - dc * (y1 - y0)) / det;
            g[q][1] = (dc * (x1 - x0) - db * (x2 - x0)) / det;
        }
        int pixels = 0, wrong = 0, worst_got = 0, worst_exp = 0;
        for (int y = 0; y < g_last_h; y++) {
            for (int x = 0; x < g_last_w; x++) {
                uint16_t got = g_last_frame[y * g_last_w + x];
                if (got == 0xffff)
                    continue;
                pixels++;
                double px = x + 0.5, py = y + 0.5;
                double l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
                double l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det;
                double l0 = 1.0 - l1 - l2;
                double w  = l0 * val[0][0] + l1 * val[0][1] + l2 * val[0][2];
                double sw = l0 * val[1][0] + l1 * val[1][1] + l2 * val[1][2];
                double tw = l0 * val[2][0] + l1 * val[2][1] + l2 * val[2][2];
                /* d(s)/dx = (d(sow)*oow - sow*d(oow)) / oow^2 */
                double dsdx = (g[1][0] * w - sw * g[0][0]) / (w * w), dsdy = (g[1][1] * w - sw * g[0][1]) / (w * w);
                double dtdx = (g[2][0] * w - tw * g[0][0]) / (w * w), dtdy = (g[2][1] * w - tw * g[0][1]) / (w * w);
                double rho = fmax(sqrt(dsdx * dsdx + dtdx * dtdx), sqrt(dsdy * dsdy + dtdy * dtdy));
                double lod = log2(rho);        /* 0 = 1 texel (256 space) per pixel */
                int    expect = (int) floor(lod + 0.5) - 1; /* level 0 = 128 */
                if (expect < 0) expect = 0;
                if (expect > 6) expect = 6;
                int got_lvl = (int) got - 1;
                if (abs(got_lvl - expect) > 1) {
                    wrong++;
                    worst_got = got_lvl;
                    worst_exp = expect;
                }
            }
        }
        if (wrong) {
            bad_tris++;
            if (bad_tris <= 8)
                fprintf(stderr, "  triangle %2d: %d of %d pixels wrong level (e.g. %d instead of %d)\n", i, wrong, pixels,
                        worst_got, worst_exp);
        }
    }
    if (bad_tris) {
        fprintf(stderr, "Test 9 FAILED: %d of %d triangles with wrong mipmap level\n", bad_tris, ntri);
        return 1;
    }
    printf("  ok: mipmap level correct in all %d triangles (+-1)\n", ntri);
    return 0;
}


static int test_lfb(void) {
    printf("-- Test 10: LFB access --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);
    int fail = 0;

    /* 565: 4x2 region at (10,10). */
    uint16_t r565[8] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0x1234, 0x4321, 0x0000, 0x8410 };
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 10, 10, GR_LFB_SRC_FMT_565, 4, 2, 8, r565);

    /* 888 (32 bits per pixel), default ARGB, then ABGR. */
    uint32_t c888 = 0x00ff8000; /* orange: r=ff g=80 b=00 */
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 20, 10, GR_LFB_SRC_FMT_888, 1, 1, 4, &c888);
    grLfbWriteColorFormat(GR_COLORFORMAT_ABGR);
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 21, 10, GR_LFB_SRC_FMT_888, 1, 1, 4, &c888); /* -> r=00 g=80 b=ff */
    grLfbWriteColorFormat(GR_COLORFORMAT_ARGB);

    /* 1555: pure green (g5 = 31) must give full green. */
    uint16_t c1555 = 0x83e0;
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 22, 10, GR_LFB_SRC_FMT_1555, 1, 1, 2, &c1555);

    /* 565_DEPTH: colour + depth; a triangle behind it with depth test
     * GREATER (ooz 100 < depth 1000) must not overwrite the pixel. */
    uint32_t cd = 0x03e8001fu; /* depth 1000, colour blue */
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 30, 30, GR_LFB_SRC_FMT_565_DEPTH, 1, 1, 4, &cd);
    grDepthBufferMode(GR_DEPTHBUFFER_ZBUFFER);
    grDepthBufferFunction(GR_CMP_GREATER);
    quad(25, 25, 40, 40, 255, 0, 0, 100);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);

    /* lock 565: written directly; lock 888: through the shadow buffer. */
    GrLfbInfo_t info;
    memset(&info, 0, sizeof info);
    if (!grLfbLock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
        fprintf(stderr, "grLfbLock 565 failed\n");
        return 1;
    }
    ((uint16_t *) info.lfbPtr)[50 * (info.strideInBytes / 2) + 50] = 0xabcd;
    grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
    if (!grLfbLock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_888, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
        fprintf(stderr, "grLfbLock 888 failed\n");
        return 1;
    }
    ((uint32_t *) info.lfbPtr)[60 * (info.strideInBytes / 4) + 60] = 0x0000ff00; /* green */
    grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);

    /* read back */
    uint16_t back[4];
    grLfbReadRegion(GR_BUFFER_BACKBUFFER, 10, 11, 4, 1, 8, back);
    grBufferSwap(1);

    fail |= check_pixel(10, 10, 0xf800, "565 pixel 0");
    fail |= check_pixel(13, 10, 0xffff, "565 pixel 3");
    fail |= check_pixel(11, 11, 0x4321, "565 second line");
    fail |= check_pixel(20, 10, 0xfc00, "888 ARGB orange");
    fail |= check_pixel(21, 10, 0x041f, "888 ABGR");
    fail |= check_pixel(22, 10, 0x07e0, "1555 green");
    fail |= check_pixel(30, 30, 0x001f, "565_DEPTH + Z test holds");
    fail |= check_pixel(35, 35, 0xf800, "triangle next to it drawn");
    fail |= check_pixel(50, 50, 0xabcd, "Lock 565 direct");
    fail |= check_pixel(60, 60, 0x07e0, "Lock 888 through shadow");
    fail |= check_pixel(10, 10, 0xf800, "Lock 888 leaves the rest unchanged");
    if (back[0] != 0x1234 || back[3] != 0x8410) {
        fprintf(stderr, "grLfbReadRegion: %04x %04x\n", back[0], back[3]);
        fail = 1;
    }
    if (fail) {
        fprintf(stderr, "Test 10 FAILED\n");
        return 1;
    }
    printf("  ok: write/read region of all formats, depth, lock direct and through shadow\n");
    return 0;
}

/* Board queries, GR_ORIGIN_LOWER_LEFT, state save/restore, lines/points,
 * triangle statistics (core/glide_sst.c). */
static int test_sst(void) {
    printf("-- Test 11: board status, origin, Get/SetState, lines/points --\n");
    if (open_320x240())
        return 1;
    int fail = 0;
    if (grSstScreenWidth() != 320 || grSstScreenHeight() != 240) {
        fprintf(stderr, "grSstScreenWidth/Height: %u x %u\n", (unsigned) grSstScreenWidth(), (unsigned) grSstScreenHeight());
        fail = 1;
    }
    /* loops waiting for vertical retrace must terminate. */
    int n = 0;
    while (!grSstVRetraceOn() && n < 10)
        n++;
    while (grSstVRetraceOn() && n < 20)
        n++;
    if (n >= 10 || grSstIsBusy() || grBufferNumPending() != 0 || (grSstStatus() & 0x380u)) {
        fprintf(stderr, "Status/Retrace: n=%d status=%08x\n", n, (unsigned) grSstStatus());
        fail = 1;
    }

    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);

    /* Save the state, change it (constant colour green instead of
     * iterated), restore: the rectangle must be iterated red again. */
    GrState saved;
    grGlideGetState(&saved);
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT,
                   GR_COMBINE_OTHER_NONE, FXFALSE);
    grConstantColorValue(0xff00ff00);
    grClipWindow(0, 0, 100, 100);
    quad(200, 10, 220, 30, 255, 0, 0, 0); /* outside the clip window */
    quad(10, 10, 20, 20, 255, 0, 0, 0);   /* green */
    grGlideSetState(&saved);
    quad(30, 10, 40, 20, 255, 0, 0, 0);   /* red again */
    quad(240, 10, 250, 20, 255, 0, 0, 0); /* full clip window again */

    /* lines and points (visible even with culling on). */
    grCullMode(GR_CULL_POSITIVE);
    GrVertex a = vtx(50, 50, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex b = vtx(60, 50, 255, 255, 255, 255, 0, 1, 0, 0);
    GrVertex c = vtx(70, 40, 0, 0, 255, 255, 0, 1, 0, 0);
    GrVertex d = vtx(70, 60, 0, 0, 255, 255, 0, 1, 0, 0);
    GrVertex p = vtx(80, 80, 255, 255, 0, 255, 0, 1, 0, 0);
    grDrawLine(&a, &b);
    grDrawLine(&d, &c);
    grDrawPoint(&p);
    grCullMode(GR_CULL_DISABLE);

    /* Origin bottom left: y=10 lands in row 240-1-10; clearing with a clip
     * window is mirrored the same way. */
    grSstOrigin(GR_ORIGIN_LOWER_LEFT);
    quad(100, 10, 110, 20, 0, 0, 255, 0);
    grClipWindow(150, 0, 160, 5);
    grBufferClear(0xffffffff, 0, 0);
    grClipWindow(0, 0, 320, 240);
    grSstOrigin(GR_ORIGIN_UPPER_LEFT);

    grResetTriStats();
    quad(0, 0, 1, 1, 0, 0, 0, 0);
    FxU32 processed = 0, drawn = 0;
    grTriStats(&processed, &drawn);
    grBufferSwap(1);

    fail |= check_pixel(210, 20, 0x0000, "clip window from state");
    fail |= check_pixel(15, 15, 0x07e0, "changed state green");
    fail |= check_pixel(35, 15, 0xf800, "restored red");
    fail |= check_pixel(245, 15, 0xf800, "clip window restored");
    fail |= check_pixel(50, 50, 0xffff, "line start");
    fail |= check_pixel(59, 50, 0xffff, "line end");
    fail |= check_pixel(55, 51, 0x0000, "line 1 pixel wide");
    fail |= check_pixel(70, 45, 0x001f, "vertical line");
    fail |= check_pixel(71, 45, 0x0000, "vertical line 1 pixel wide");
    fail |= check_pixel(80, 80, 0xffe0, "point");
    fail |= check_pixel(81, 80, 0x0000, "point 1 pixel");
    fail |= check_pixel(80, 81, 0x0000, "point 1 pixel (Y)");
    fail |= check_pixel(105, 239 - 15, 0x001f, "origin bottom left");
    fail |= check_pixel(105, 15, 0x0000, "top left not hit");
    fail |= check_pixel(155, 239 - 2, 0xffff, "clear mirrored");
    fail |= check_pixel(155, 2, 0x0000, "clear not at the top");
    if (processed != 2 || drawn != 2) {
        fprintf(stderr, "grTriStats: %u/%u\n", (unsigned) processed, (unsigned) drawn);
        fail = 1;
    }
    if (fail) {
        fprintf(stderr, "Test 11 FAILED\n");
        return 1;
    }
    printf("  ok: queries, retrace loops, origin, state, lines/points, statistics\n");
    return 0;
}

/* Append a 16-bit value big-endian to f (.3DF data). */
static void put_be16(FILE *f, unsigned v) {
    fputc((int) ((v >> 8) & 0xff), f);
    fputc((int) (v & 0xff), f);
}

/* guTex* management and .3DF files (core/glide_gutex.c): load an RGB565
 * chain 64x32..16x8 (one colour per level, big-endian in the file),
 * allocate, download, select with guTexSource and check the mip levels as
 * in test 5; plus a P8 file with palette. */
static int test_gutex(void) {
    printf("-- Test 12: guTex texture management, .3DF files --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    int fail = 0;

    const char *path = "build/linux/test565.3df";
    FILE       *f    = fopen(path, "wb");
    if (!f)
        return 1;
    fputs("3df v1.0\nrgb565\nlod range: 16 64\naspect ratio: 2 1\n", f);
    for (int i = 0; i < 64 * 32; i++)
        put_be16(f, 0xf800);
    for (int i = 0; i < 32 * 16; i++)
        put_be16(f, 0x07e0);
    for (int i = 0; i < 16 * 8; i++)
        put_be16(f, 0x001f);
    fclose(f);

    Gu3dfInfo info;
    memset(&info, 0, sizeof info);
    if (!gu3dfGetInfo(path, &info) || info.header.width != 64 || info.header.height != 32 ||
        info.header.large_lod != GR_LOD_64 || info.header.small_lod != GR_LOD_16 ||
        info.header.aspect_ratio != GR_ASPECT_2x1 || info.header.format != GR_TEXFMT_RGB_565 ||
        info.mem_required != (64 * 32 + 32 * 16 + 16 * 8) * 2) { /* unrounded */
        fprintf(stderr, "gu3dfGetInfo: %ux%u lod %d..%d aspect %d fmt %d mem %u\n", (unsigned) info.header.width,
                (unsigned) info.header.height, info.header.large_lod, info.header.small_lod,
                info.header.aspect_ratio, info.header.format, (unsigned) info.mem_required);
        return 1;
    }
    info.data = malloc(info.mem_required);
    if (!gu3dfLoad(path, &info) || ((uint16_t *) info.data)[0] != 0xf800 ||
        ((uint16_t *) info.data)[64 * 32] != 0x07e0) {
        fprintf(stderr, "gu3dfLoad failed\n");
        return 1;
    }
    if (gu3dfGetInfo("build/linux/gibtsnicht.3df", &info)) {
        fprintf(stderr, "gu3dfGetInfo reports a missing file as read\n");
        fail = 1;
    }

    /* Memory: first a placeholder (256x256 RGB565 = 128 KB); the actual
     * texture must lie after it. */
    guTexMemReset();
    FxU32        total = guTexMemQueryAvail(GR_TMU0);
    GrMipMapId_t pad   = guTexAllocateMemory(GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 256, 256, GR_TEXFMT_RGB_565,
                                             GR_MIPMAP_DISABLE, GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1,
                                             GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP,
                                             GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR, 0.0f, FXFALSE);
    GrMipMapId_t id    = guTexAllocateMemory(GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 64, 32, info.header.format,
                                             GR_MIPMAP_NEAREST, info.header.small_lod, info.header.large_lod,
                                             info.header.aspect_ratio, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP,
                                             GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED, 0.0f,
                                             FXFALSE);
    /* tmu_base_address as in 3dfx's code: where LOD 0 would be; the 64x32
     * level starts at 131072, the 256x128 and 128x64 levels are missing
     * before it. */
    GrMipMapInfo *mm = guTexGetMipMapInfo(id);
    if (pad == GR_NULL_MIPMAP_HANDLE || id == GR_NULL_MIPMAP_HANDLE || !mm ||
        mm->tmu_base_address != 131072 - (256 * 128 + 128 * 64) * 2 ||
        guTexMemQueryAvail(GR_TMU0) != total - 131072 - info.mem_required) {
        fprintf(stderr, "guTexAllocateMemory: base/memory wrong\n");
        return 1;
    }
    /* more than the whole texture memory: must fail. */
    for (int i = 0; i < 20; i++)
        guTexAllocateMemory(GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 256, 256, GR_TEXFMT_RGB_565, GR_MIPMAP_DISABLE,
                            GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, 0, 0, 0, 0, 0.0f, FXFALSE);
    if (guTexAllocateMemory(GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 256, 256, GR_TEXFMT_RGB_565, GR_MIPMAP_DISABLE,
                            GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, 0, 0, 0, 0, 0.0f, FXFALSE) != GR_NULL_MIPMAP_HANDLE) {
        fprintf(stderr, "guTexAllocateMemory beyond the memory\n");
        fail = 1;
    }
    /* -1 = unchanged; changed attributes take effect only with guTexSource. */
    if (!guTexChangeAttributes(id, -1, -1, -1, -1, -1, -1, -1, GR_TEXTURECLAMP_WRAP, -1, -1, -1) ||
        mm->s_clamp_mode != GR_TEXTURECLAMP_WRAP || mm->t_clamp_mode != GR_TEXTURECLAMP_CLAMP ||
        mm->lod_max != GR_LOD_64 || mm->format != GR_TEXFMT_RGB_565) {
        fprintf(stderr, "guTexChangeAttributes\n");
        fail = 1;
    }
    guTexChangeAttributes(id, -1, -1, -1, -1, -1, -1, -1, GR_TEXTURECLAMP_CLAMP, -1, -1, -1);

    guTexDownloadMipMap(id, info.data, NULL);
    /* Overwrite the green level step by step: LOD 32 with white, through
     * guTexDownloadMipMapLevel with pointer advance. */
    static uint16_t white[32 * 16 + 16 * 8];
    for (int i = 0; i < 32 * 16; i++)
        white[i] = 0xffff;
    for (int i = 0; i < 16 * 8; i++)
        white[32 * 16 + i] = 0x001f;
    const void *src = white;
    guTexDownloadMipMapLevel(id, GR_LOD_32, &src);
    if (src != (const void *) (white + 32 * 16)) {
        fprintf(stderr, "guTexDownloadMipMapLevel: pointer not advanced\n");
        fail = 1;
    }
    guTexDownloadMipMapLevel(id, GR_LOD_16, &src);

    guTexSource(id);
    if (guTexGetCurrentMipMap(GR_TMU0) != id) {
        fprintf(stderr, "guTexGetCurrentMipMap\n");
        fail = 1;
    }
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grBufferClear(0, 0, 0);
    struct { float x, y, w, h; uint16_t expect; const char *what; } q[] = {
        { 10, 10, 64, 32, 0xf800, "guTex: 64x32 pixels -> LOD 64 (red, from .3DF)" },
        { 90, 10, 32, 16, 0xffff, "guTex: 32x16 pixels -> LOD 32 (white, per-level download)" },
        { 130, 10, 16, 8, 0x001f, "guTex: 16x8 pixels -> LOD 16 (blue)" },
    };
    for (int i = 0; i < 3; i++) {
        GrVertex v[4];
        v[0] = vtx(q[i].x, q[i].y, 255, 255, 255, 255, 0, 1, 0, 0);
        v[1] = vtx(q[i].x + q[i].w, q[i].y, 255, 255, 255, 255, 0, 1, 256, 0);
        v[2] = vtx(q[i].x + q[i].w, q[i].y + q[i].h, 255, 255, 255, 255, 0, 1, 256, 128);
        v[3] = vtx(q[i].x, q[i].y + q[i].h, 255, 255, 255, 255, 0, 1, 0, 128);
        grDrawPolygonVertexList(4, v);
    }
    grBufferSwap(1);
    for (int i = 0; i < 3; i++)
        fail |= check_pixel((int) (q[i].x + q[i].w / 2), (int) (q[i].y + q[i].h / 2), q[i].expect, q[i].what);
    free(info.data);

    /* P8 with palette (32-bit big-endian) and 8-bit data. */
    path = "build/linux/testp8.3df";
    f    = fopen(path, "wb");
    if (!f)
        return 1;
    fputs("3df v1.0\np8\nlod range: 8 8\naspect ratio: 1 1\n", f);
    for (int i = 0; i < 256; i++) {
        put_be16(f, 0x0012);
        put_be16(f, (unsigned) (0x3400 + i));
    }
    for (int i = 0; i < 64; i++)
        fputc(i, f);
    fclose(f);
    memset(&info, 0, sizeof info);
    static uint8_t p8[64];
    info.data = p8;
    if (!gu3dfGetInfo(path, &info) || info.mem_required != 64 || !gu3dfLoad(path, &info) ||
        info.table.palette.data[5] != 0x00123405u || p8[63] != 63) {
        fprintf(stderr, "P8-.3DF: Palette %08x, Texel %d\n", (unsigned) info.table.palette.data[5], p8[63]);
        fail = 1;
    }

    guTexMemReset();
    if (guTexGetMipMapInfo(id) || guTexMemQueryAvail(GR_TMU0) != total ||
        guTexGetCurrentMipMap(GR_TMU0) != GR_NULL_MIPMAP_HANDLE) {
        fprintf(stderr, "guTexMemReset\n");
        fail = 1;
    }
    if (fail) {
        fprintf(stderr, "Test 12 FAILED\n");
        return 1;
    }
    printf("  ok: .3DF (RGB565, P8), allocation, downloads, guTexSource, mipmap levels\n");
    return 0;
}

/* Fog (core/glide_fog.c) and YIQ/NCC textures (core/glide_tex.c). */
static FxU32 ncc_pack(int r, int g, int b) {
    return ((FxU32) (r & 0x1ff) << 18) | ((FxU32) (g & 0x1ff) << 9) | (FxU32) (b & 0x1ff);
}

static int red8(int x, int y) {
    return ((glidulatorDebugPeekPixel(x, y) >> 11) & 0x1f) << 3;
}

static int test_fog_ncc(void) {
    printf("-- Test 13: fog, YIQ/NCC textures --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);
    int fail = 0;

    /* Table fog: entry i = 4*i, fog colour white, surfaces black. At W =
     * guFogTableIndexToW(i) the fog amount must be ~4*i, checking that
     * Glide's index->W conversion matches the hardware's W depth. */
    GrFog_t ft[GR_FOG_TABLE_SIZE];
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
        ft[i] = (GrFog_t) (i * 4);
    grFogTable(ft);
    grFogColorValue(0xffffffff);
    grFogMode(GR_FOG_WITH_TABLE);
    int idx[3] = { 12, 30, 50 };
    for (int k = 0; k < 3; k++) {
        float    oow = 1.0f / guFogTableIndexToW(idx[k]);
        GrVertex q[4];
        float    x0 = 10.0f + 30.0f * (float) k;
        q[0] = vtx(x0, 10, 0, 0, 0, 255, 0, oow, 0, 0);
        q[1] = vtx(x0 + 20, 10, 0, 0, 0, 255, 0, oow, 0, 0);
        q[2] = vtx(x0 + 20, 30, 0, 0, 0, 255, 0, oow, 0, 0);
        q[3] = vtx(x0, 30, 0, 0, 0, 255, 0, oow, 0, 0);
        grDrawPolygonVertexList(4, q);
    }
    /* fog from iterated alpha: a = 128 -> half white. */
    grFogMode(GR_FOG_WITH_ITERATED_ALPHA);
    quad(110, 10, 130, 30, 0, 0, 0, 0); /* quad() sets a = 255 */
    {
        GrVertex q[4];
        q[0] = vtx(140, 10, 0, 0, 0, 128, 0, 1, 0, 0);
        q[1] = vtx(160, 10, 0, 0, 0, 128, 0, 1, 0, 0);
        q[2] = vtx(160, 30, 0, 0, 0, 128, 0, 1, 0, 0);
        q[3] = vtx(140, 30, 0, 0, 0, 128, 0, 1, 0, 0);
        grDrawPolygonVertexList(4, q);
    }
    grFogMode(GR_FOG_DISABLE);
    quad(170, 10, 190, 30, 0, 0, 0, 0); /* black without fog */

    /* guFogGenerate*: monotonically rising, ending at 255 (exp) or range. */
    GrFog_t e[GR_FOG_TABLE_SIZE], l[GR_FOG_TABLE_SIZE];
    guFogGenerateExp(e, 0.01f);
    guFogGenerateLinear(l, 10.0f, 1000.0f);
    for (int i = 1; i < GR_FOG_TABLE_SIZE; i++)
        if (e[i] < e[i - 1] || l[i] < l[i - 1])
            fail = 1;
    if (fail || e[GR_FOG_TABLE_SIZE - 1] < 254 || l[0] != 0 || l[GR_FOG_TABLE_SIZE - 1] != 255) {
        fprintf(stderr, "guFogGenerate*: e[63]=%d l[0]=%d l[63]=%d\n", e[GR_FOG_TABLE_SIZE - 1], l[0],
                l[GR_FOG_TABLE_SIZE - 1]);
        fail = 1;
    }

    /* YIQ_422 8x8, all texels 0x5a (Y index 5, I 2, Q 2). NCC0: Y = 16*k,
     * I2 = (+10, 0, -10), Q2 = (0, +20, 0) -> (90, 100, 70). NCC1 the same
     * except Y = 8*k -> (50, 60, 30). */
    static uint8_t yiq[8 * 8];
    memset(yiq, 0x5a, sizeof yiq);
    GuNccTable ncc[2];
    memset(ncc, 0, sizeof ncc);
    for (int t = 0; t < 2; t++) {
        for (int w = 0; w < 4; w++) {
            FxU32 m = (FxU32) (t ? 8 : 16);
            ncc[t].packed_data[w] = (m * (4 * w)) | ((m * (4 * w + 1)) << 8) | ((m * (4 * w + 2)) << 16) |
                                    ((m * (4 * w + 3)) << 24);
        }
        ncc[t].packed_data[4 + 2] = ncc_pack(10, 0, -10);
        ncc[t].packed_data[8 + 2] = ncc_pack(0, 20, 0);
    }
    GrTexInfo info;
    info.smallLod = info.largeLod = GR_LOD_8;
    info.aspectRatio              = GR_ASPECT_1x1;
    info.format                   = GR_TEXFMT_YIQ_422;
    info.data                     = yiq;
    grTexDownloadTable(GR_TMU0, GR_TEXTABLE_NCC0, &ncc[0]);
    grTexDownloadTable(GR_TMU0, GR_TEXTABLE_NCC1, &ncc[1]);
    grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    for (int t = 0; t < 2; t++) {
        grTexNCCTable(GR_TMU0, t ? GR_NCCTABLE_NCC1 : GR_NCCTABLE_NCC0);
        GrVertex q[4];
        float    x0 = 10.0f + 30.0f * (float) t;
        q[0] = vtx(x0, 50, 255, 255, 255, 255, 0, 1, 0, 0);
        q[1] = vtx(x0 + 20, 50, 255, 255, 255, 255, 0, 1, 256, 0);
        q[2] = vtx(x0 + 20, 70, 255, 255, 255, 255, 0, 1, 256, 256);
        q[3] = vtx(x0, 70, 255, 255, 255, 255, 0, 1, 0, 256);
        grDrawPolygonVertexList(4, q);
    }
    grBufferSwap(1);

    for (int k = 0; k < 3; k++) {
        int got = red8(20 + 30 * k, 20), want = 4 * idx[k];
        printf("  table fog index %d: red %d (expected ~%d)\n", idx[k], got, want);
        if (got < want - 12 || got > want + 12)
            fail = 1;
    }
    int full = red8(120, 20), half = red8(150, 20), none = red8(180, 20);
    if (full < 240 || half < 112 || half > 136 || none != 0) {
        fprintf(stderr, "Alpha fog: a=255 -> %d, a=128 -> %d, off -> %d\n", full, half, none);
        fail = 1;
    }
    fail |= !near565((uint16_t) glidulatorDebugPeekPixel(20, 60), 90, 100, 70);
    fail |= !near565((uint16_t) glidulatorDebugPeekPixel(50, 60), 50, 60, 30);
    if (fail) {
        fprintf(stderr, "Test 13 FAILED (NCC0 %04x, NCC1 %04x)\n", glidulatorDebugPeekPixel(20, 60),
                glidulatorDebugPeekPixel(50, 60));
        return 1;
    }
    save_png("tests/golden/fog_ncc.png");
    printf("  ok: table and alpha fog, guFogGenerate*, NCC0/NCC1 including table switch\n");
    return 0;
}

/* gu presets (guColorCombineFunction, guAlphaSource), DELTA0, NCC table
 * switching in guTexSource. Mappings per 3dfx's Glide 2.x. */
static int test_gu_presets(void) {
    printf("-- Test 14: guColorCombineFunction/guAlphaSource, DELTA0, NCC in guTexSource --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);
    int fail = 0;

    /* CCRGB: constant colour; ITRGB: vertex colour; ITRGB_DELTA0: colour
     * from grConstantColorValue4 instead of the vertex colours. */
    grConstantColorValue(0xff00ff00);
    guColorCombineFunction(GR_COLORCOMBINE_CCRGB);
    quad(10, 10, 30, 30, 255, 0, 0, 0);
    guColorCombineFunction(GR_COLORCOMBINE_ITRGB);
    quad(40, 10, 60, 30, 255, 0, 0, 0);
    grConstantColorValue4(255.0f, 0.0f, 0.0f, 255.0f);
    guColorCombineFunction(GR_COLORCOMBINE_ITRGB_DELTA0);
    quad(70, 10, 90, 30, 255, 0, 0, 0);
    guColorCombineFunction(GR_COLORCOMBINE_ONE);
    quad(100, 10, 120, 30, 0, 0, 0, 0);

    /* guAlphaSource(CC_ALPHA) + blending: constant alpha 0x80 blends white
     * half onto black. */
    guColorCombineFunction(GR_COLORCOMBINE_ITRGB);
    grConstantColorValue(0x80000000);
    guAlphaSource(GR_ALPHASOURCE_CC_ALPHA);
    grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
    quad(130, 10, 150, 30, 255, 255, 255, 0);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);

    /* Two YIQ textures through guTex*: different NCC tables (Y constant 200
     * or 40), selected alternately; each must keep its own table. */
    static uint8_t yiq[8 * 8];
    memset(yiq, 0x00, sizeof yiq); /* Y index 0, I 0, Q 0 */
    GuNccTable ncc[2];
    memset(ncc, 0, sizeof ncc);
    ncc[0].packed_data[0] = 200;
    ncc[1].packed_data[0] = 40;
    guTexMemReset();
    GrMipMapId_t ids[2];
    for (int t = 0; t < 2; t++) {
        ids[t] = guTexAllocateMemory(GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 8, 8, GR_TEXFMT_YIQ_422, GR_MIPMAP_DISABLE,
                                     GR_LOD_8, GR_LOD_8, GR_ASPECT_1x1, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP,
                                     GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED, 0.0f, FXFALSE);
        guTexDownloadMipMap(ids[t], yiq, &ncc[t]);
    }
    guColorCombineFunction(GR_COLORCOMBINE_DECAL_TEXTURE);
    guAlphaSource(GR_ALPHASOURCE_ITERATED_ALPHA);
    for (int k = 0; k < 4; k++) {
        guTexSource(ids[k & 1]);
        GrVertex q[4];
        float    x0 = 10.0f + 30.0f * (float) k;
        q[0] = vtx(x0, 50, 255, 255, 255, 255, 0, 1, 0, 0);
        q[1] = vtx(x0 + 20, 50, 255, 255, 255, 255, 0, 1, 256, 0);
        q[2] = vtx(x0 + 20, 70, 255, 255, 255, 255, 0, 1, 256, 256);
        q[3] = vtx(x0, 70, 255, 255, 255, 255, 0, 1, 0, 256);
        grDrawPolygonVertexList(4, q);
    }
    grBufferSwap(1);

    fail |= check_pixel(20, 20, 0x07e0, "CCRGB = constant colour");
    fail |= check_pixel(50, 20, 0xf800, "ITRGB = vertex colour");
    fail |= check_pixel(80, 20, 0x001f, "ITRGB_DELTA0 = grConstantColorValue4");
    fail |= check_pixel(110, 20, 0xffff, "ONE = white");
    fail |= !near565((uint16_t) glidulatorDebugPeekPixel(140, 20), 128, 128, 128);
    for (int k = 0; k < 4; k++) {
        int y = 200 - 160 * (k & 1);
        if (!near565((uint16_t) glidulatorDebugPeekPixel(20 + 30 * k, 60), y, y, y)) {
            fprintf(stderr, "YIQ texture %d: %04x (expected grey %d)\n", k, glidulatorDebugPeekPixel(20 + 30 * k, 60), y);
            fail = 1;
        }
    }
    if (fail) {
        fprintf(stderr, "Test 14 FAILED (alpha %04x)\n", glidulatorDebugPeekPixel(140, 20));
        return 1;
    }
    printf("  ok: colour/alpha presets, DELTA0, two YIQ textures with their own NCC table\n");
    return 0;
}

/* guDrawTriangleWithClip: corners far outside the board's 12.4 range
 * (+-2048 pixels); without clipping the coordinates would wrap (missing
 * surfaces, flying polygons). A huge triangle with a red gradient along x
 * must fill the screen and have the right colour everywhere. */
static int test_clip(void) {
    printf("-- Test 15: guDrawTriangleWithClip with corners far outside --\n");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);
    /* r = (x + 5000) / 10000 * 255 along x, independent of y. */
    GrVertex a = vtx(-5000, -5000, 0, 0, 0, 255, 0, 1, 0, 0);
    GrVertex b = vtx(5000, -5000, 255, 0, 0, 255, 0, 1, 0, 0);
    GrVertex c = vtx(-5000, 15000, 0, 0, 0, 255, 0, 1, 0, 0);
    /* c: x = -5000 -> r 0; the gradient is along x only if c has the same
     * colour as a and lies at the same x. */
    guDrawTriangleWithClip(&a, &b, &c);
    grBufferSwap(1);
    int fail = 0;
    for (int k = 0; k < 4; k++) {
        int   x    = 10 + 100 * k, y = 20 + 60 * k;
        float want = ((float) x + 0.5f + 5000.0f) / 10000.0f * 255.0f;
        int   got  = ((glidulatorDebugPeekPixel(x, y) >> 11) & 0x1f) << 3;
        if (x >= 320)
            continue;
        if (got < (int) want - 9 || got > (int) want + 9) {
            fprintf(stderr, "Pixel (%d,%d): red %d, expected ~%.0f\n", x, y, got, want);
            fail = 1;
        }
    }
    /* fully covered: the screen corners are drawn too (red > 0). */
    fail |= glidulatorDebugPeekPixel(0, 0) == 0 || glidulatorDebugPeekPixel(319, 239) == 0;
    if (fail) {
        fprintf(stderr, "Test 15 FAILED\n");
        return 1;
    }
    printf("  ok: screen filled, gradient after clipping correct\n");
    return 0;
}

static int test_renderer(void) {
    printf("-- Test 16: rasterizer path and GLIDULAT.CFG reader --\n");
    int  fail = 0;
    char buf[16];
    const char *cfg = "# comment\r\nSST_GAMMA=1.0\r\n  glidulator_renderer = JIT \r\nGLIDULATOR_THREADS=\n";
    fail |= !glide_config_find(cfg, "GLIDULATOR_RENDERER", buf, sizeof buf) || strcmp(buf, "JIT");
    fail |= !glide_config_find(cfg, "sst_gamma", buf, sizeof buf) || strcmp(buf, "1.0");
    fail |= glide_config_find(cfg, "GLIDULATOR_THREADS", buf, sizeof buf); /* empty */
    fail |= glide_config_find(cfg, "SST", buf, sizeof buf);                /* prefix only */
    fail |= glide_renderer_parse("JIT") != GLIDE_RENDERER_JIT || glide_renderer_parse("auto") != GLIDE_RENDERER_AUTO ||
            glide_renderer_parse("sse") != GLIDE_RENDERER_UNKNOWN;
    /* The example of the release package (package/GLIDULAT.CFG): within the
     * read limit, every setting present but commented out, and each one
     * found once its # is removed. */
    {
        static const char *names[] = {
            "GLIDULATOR_DISPLAY", "GLIDULATOR_SCALE",   "GLIDULATOR_VGA_DITHER", "GLIDULATOR_OSD",
            "GLIDULATOR_WC",      "SST_GAMMA",          "GLIDULATOR_RENDERER",   "GLIDULATOR_THREADS",
            "GLIDULATOR_SMP",     "GLIDULATOR_VSYNC",   "GLIDULATOR_FSAA",       "GLIDULATOR_AA_POST",
            "GLIDULATOR_VOODOO_FILTER", "GLIDULATOR_SPLASH", "GLIDULATOR_SPLASH_FILE", "GLIDULATOR_CRT"};
        static char text[GLIDE_CONFIG_MAX + 2];
        FILE       *f   = fopen("package/GLIDULAT.CFG", "rb");
        size_t      len = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f)
            fclose(f);
        text[len] = 0;
        fail |= !len || len >= GLIDE_CONFIG_MAX;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            fail |= glide_config_find(text, names[i], buf, sizeof buf);
        for (char *p = text; *p; p++)
            if (*p == '#' && (p == text || p[-1] == '\n') && (p[1] == 'G' || p[1] == 'S'))
                *p = ' ';
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!glide_config_find(text, names[i], buf, sizeof buf)) {
                fprintf(stderr, "package/GLIDULAT.CFG: %s missing\n", names[i]);
                fail = 1;
            }
    }
    /* An entry behind 2 KB of comments (the reader must handle more than
     * one small buffer). */
    {
        static char big[3000];
        memset(big, '#', sizeof big);
        for (size_t i = 79; i < sizeof big; i += 80)
            big[i] = '\n';
        strcpy(big + 2500, "\r\nGLIDULATOR_DISPLAY=1280x720\r\n");
        fail |= platform_write_file("build/linux/test_cfg.cfg", big, strlen(big)) ||
                !glide_config_from_file("build/linux/test_cfg.cfg", "GLIDULATOR_DISPLAY", buf, sizeof buf) ||
                strcmp(buf, "1280x720");
    }
    if (fail)
        fprintf(stderr, "configuration read wrongly\n");

    /* unavailable paths fall back to C, threads to the maximum. */
    glide_renderer_force(GLIDE_RENDERER_JIT, 64);
    if (open_320x240())
        return 1;
    int want = glide_renderer_available(GLIDE_RENDERER_JIT) ? GLIDE_RENDERER_JIT : GLIDE_RENDERER_C;
    if (glide_renderer_active() != want || glide_renderer_threads() != glide_renderer_max_threads()) {
        fprintf(stderr, "Renderer %s/%d threads, expected %s/%d\n", glide_renderer_name(glide_renderer_active()),
                glide_renderer_threads(), glide_renderer_name(want), glide_renderer_max_threads());
        fail = 1;
    }
    glide_renderer_force(GLIDE_RENDERER_AUTO, 0);
    if (open_320x240())
        return 1;
    unsigned cpu = platform_cpu_features();
    printf("  CPU:%s%s%s, path %s, %d thread(s)\n", (cpu & PLATFORM_CPU_MMX) ? " MMX" : "",
           (cpu & PLATFORM_CPU_SSE) ? " SSE" : "", (cpu & PLATFORM_CPU_SSE2) ? " SSE2" : "",
           glide_renderer_name(glide_renderer_active()), glide_renderer_threads());
    if (fail) {
        fprintf(stderr, "Test 16 FAILED\n");
        return 1;
    }
    printf("  ok: configuration read, fallback to an available path\n");
    return 0;
}

static int test_output_osd(void) {
    printf("-- Test 17: output rectangle (GLIDULATOR_SCALE) and overlay (GLIDULATOR_OSD) --\n");
    static const struct {
        int mode, w, h, cw, ch, x, y, rw, rh;
    } cases[] = {
        { OUT_SCALE_ASPECT, 512, 384, 1280, 720, 160, 0, 960, 720 },   /* 16:9: borders left/right */
        { OUT_SCALE_ASPECT, 640, 400, 1920, 1080, 240, 0, 1440, 1080 }, /* 640x400 also as 4:3 */
        { OUT_SCALE_ASPECT, 640, 480, 1280, 1024, 0, 32, 1280, 960 },  /* 5:4: top/bottom */
        { OUT_SCALE_STRETCH, 512, 384, 1280, 720, 0, 0, 1280, 720 },
        { OUT_SCALE_INTEGER, 512, 384, 1920, 1080, 448, 156, 1024, 768 },
        { OUT_SCALE_NONE, 512, 384, 640, 480, 64, 48, 512, 384 },
        { OUT_SCALE_NONE, 800, 600, 640, 480, 0, 0, 640, 480 },        /* too large: like aspect */
    };
    int fail = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int x, y, rw, rh;
        output_rect(cases[i].mode, cases[i].w, cases[i].h, cases[i].cw, cases[i].ch, &x, &y, &rw, &rh);
        if (x != cases[i].x || y != cases[i].y || rw != cases[i].rw || rh != cases[i].rh) {
            fprintf(stderr, "Case %u: %d,%d %dx%d, expected %d,%d %dx%d\n", i, x, y, rw, rh, cases[i].x, cases[i].y,
                    cases[i].rw, cases[i].rh);
            fail = 1;
        }
    }
    fail |= output_scale_parse("Aspect") != OUT_SCALE_ASPECT || output_scale_parse("none") != OUT_SCALE_NONE ||
            output_scale_parse("fit") != -1;

    /* OSD: visible in the output, gone from the framebuffer afterwards. */
    setenv("GLIDULATOR_OSD", "1", 1);
    if (open_320x240())
        return 1;
    grBufferClear(0x00406080, 0, 0);
    uint16_t bg = (uint16_t) glidulatorDebugPeekPixel(100, 10);
    g_copy_frame = 1;
    grBufferSwap(1);
    g_copy_frame = 0;
    int white = 0, dark = 0;
    for (int y = 4; y < 24; y++)
        for (int x = 4; x < 200; x++) {
            white += g_last_frame[y * g_last_w + x] == 0xffff;
            dark += g_last_frame[y * g_last_w + x] == (uint16_t) ((bg >> 1) & 0x7bef);
        }
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 320; x++)
            if (glidulatorDebugPeekPixel(x, y) != bg) {
                fprintf(stderr, "Framebuffer (%d,%d) not restored after the overlay\n", x, y);
                fail = 1;
                y = 30;
                break;
            }
    if (white < 50 || dark < 500) {
        fprintf(stderr, "Overlay missing from the picture (%d white, %d darkened pixels)\n", white, dark);
        fail = 1;
    }
    save_png("build/linux/osd.png");
    unsetenv("GLIDULATOR_OSD");
    if (open_320x240())
        return 1;
    if (fail) {
        fprintf(stderr, "Test 17 FAILED\n");
        return 1;
    }
    printf("  ok: rectangles match, overlay visible and restored afterwards (build/linux/osd.png)\n");
    return 0;
}

/* FSAA (core/fsaa.c): edges mixed, surfaces exact, LFB accesses (test 10)
 * bit-identical to the result without FSAA, one and several workers
 * bit-identical. */
static uint16_t g_fsaa_ref[320 * 240];

static int fsaa_scene(int threads) {
    glide_renderer_force(GLIDE_RENDERER_AUTO, threads);
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grBufferClear(0, 0, 0);
    GrVertex a = vtx(20.5f, 10.25f, 255, 0, 0, 255, 0, 1, 0, 0);
    GrVertex b = vtx(300.0f, 60.75f, 255, 0, 0, 255, 0, 1, 0, 0);
    GrVertex c = vtx(70.25f, 230.5f, 255, 0, 0, 255, 0, 1, 0, 0);
    for (int k = 0; k < 200; k++) /* enough to fill the workers' queue */
        grDrawTriangle(&a, &b, &c);
    g_copy_frame = 1;
    grBufferSwap(1);
    g_copy_frame = 0;
    return 0;
}

static int test_fsaa(void) {
    printf("-- Test 18: FSAA (offset sample points) --\n");
    int fail = 0;
    static const char *const levels[] = { "2", "4" };
    for (int l = 0; l < 2; l++) {
        setenv("GLIDULATOR_FSAA", levels[l], 1);
        if (fsaa_scene(1))
            return 1;
        int mixed = 0, full = 0;
        for (int i = 0; i < 320 * 240; i++) {
            uint16_t p = g_last_frame[i];
            if (p == 0xf800)
                full++;
            else if (p && (p & 0x07ff) == 0)
                mixed++;
            else if (p) {
                fprintf(stderr, "FSAA %s: unexpected colour %04x\n", levels[l], p);
                fail = 1;
                break;
            }
        }
        fail |= check_pixel(100, 80, 0xf800, "area exact");
        fail |= check_pixel(5, 200, 0x0000, "outside exact");
        if (mixed < 300 || full < 10000) {
            fprintf(stderr, "FSAA %s: %d blended edge pixels, %d full\n", levels[l], mixed, full);
            fail = 1;
        }
        memcpy(g_fsaa_ref, g_last_frame, sizeof g_fsaa_ref);
        int max = glide_renderer_max_threads();
        if (max >= 2) {
            if (fsaa_scene(max))
                return 1;
            if (memcmp(g_fsaa_ref, g_last_frame, sizeof g_fsaa_ref)) {
                fprintf(stderr, "FSAA %s: %d workers differ from single-core\n", levels[l], max);
                fail = 1;
            }
        }
        glide_renderer_force(GLIDE_RENDERER_AUTO, 0);
        printf("  FSAA %s: %d blended edge pixels, %d workers bit-identical\n", levels[l], mixed, max);
        if (l == 1)
            save_png("build/linux/fsaa4.png");
        fail |= test_lfb(); /* GUI: everything exactly as without FSAA */
    }
    unsetenv("GLIDULATOR_FSAA");
    if (open_320x240())
        return 1;
    if (fail) {
        fprintf(stderr, "Test 18 FAILED\n");
        return 1;
    }
    printf("  ok: edges blended, areas and LFB exact, multi-core identical (build/linux/fsaa4.png)\n");
    return 0;
}

/* FSAA=auto switches by frame time; AA_POST smooths depth edges and
 * leaves surfaces and LFB pixels exact. */
static int test_fsaa_auto_post(void) {
    printf("-- Test 19: FSAA auto, edge smoothing after rendering --\n");
    int fail = 0;
    setenv("GLIDULATOR_FSAA", "auto", 1);
    if (open_320x240())
        return 1;
    int seq[4];
    for (int k = 0; k < 30; k++) glide_fsaa_frame(0); /* without timer: stays 1 */
    if (g_fsaa_n != 1) {
        fprintf(stderr, "auto without time source: %d layers\n", g_fsaa_n);
        fail = 1;
    }
    for (int k = 0; k < 30; k++) glide_fsaa_frame(2000);
    seq[0] = g_fsaa_n;
    for (int k = 0; k < 30; k++) glide_fsaa_frame(2000);
    seq[1] = g_fsaa_n;
    for (int k = 0; k < 30; k++) glide_fsaa_frame(2000);
    seq[2] = g_fsaa_n;
    for (int k = 0; k < 30; k++) glide_fsaa_frame(40000);
    seq[3] = g_fsaa_n;
    if (seq[0] != 2 || seq[1] != 4 || seq[2] != 4 || seq[3] != 2) {
        fprintf(stderr, "auto: levels %d %d %d %d, expected 2 4 4 2\n", seq[0], seq[1], seq[2], seq[3]);
        fail = 1;
    }
    unsetenv("GLIDULATOR_FSAA");

    setenv("GLIDULATOR_AA_POST", "1", 1);
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_DISABLE);
    grDepthBufferMode(GR_DEPTHBUFFER_ZBUFFER);
    grDepthBufferFunction(GR_CMP_ALWAYS);
    grDepthMask(FXTRUE);
    grBufferClear(0, 0, 0);
    GrVertex a = vtx(20.5f, 10.25f, 255, 0, 0, 255, 30000, 1, 0, 0);
    GrVertex b = vtx(300.0f, 60.75f, 255, 0, 0, 255, 30000, 1, 0, 0);
    GrVertex c = vtx(70.25f, 230.5f, 255, 0, 0, 255, 30000, 1, 0, 0);
    grDrawTriangle(&a, &b, &c);
    uint16_t gui[16];
    for (int k = 0; k < 16; k++)
        gui[k] = 0x07e0; /* green bar across the edge */
    grLfbWriteRegion(GR_BUFFER_BACKBUFFER, 150, 34, GR_LFB_SRC_FMT_565, 16, 1, 32, gui);
    g_copy_frame = 1;
    grBufferSwap(1);
    g_copy_frame = 0;
    int mixed = 0;
    for (int i = 0; i < 320 * 240; i++)
        if (g_last_frame[i] && g_last_frame[i] != 0xf800 && g_last_frame[i] != 0x07e0)
            mixed++;
    fail |= check_pixel(100, 80, 0xf800, "area exact");
    fail |= check_pixel(5, 200, 0x0000, "outside exact");
    for (int k = 0; k < 16; k++)
        fail |= check_pixel(150 + k, 34, 0x07e0, "LFB pixel unchanged");
    if (mixed < 300) {
        fprintf(stderr, "AA_POST: only %d blended edge pixels\n", mixed);
        fail = 1;
    }
    save_png("build/linux/aa_post.png");
    /* Without depth (no edges detectable): picture unchanged */
    grBufferClear(0, 0, 0); /* clear depth while the mask is still on */
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grDepthMask(FXFALSE);
    grDrawTriangle(&a, &b, &c);
    g_copy_frame = 1;
    grBufferSwap(1);
    g_copy_frame = 0;
    for (int i = 0; i < 320 * 240; i++)
        if (g_last_frame[i] && g_last_frame[i] != 0xf800) {
            fprintf(stderr, "AA_POST without depth changed pixel %d (%04x)\n", i, g_last_frame[i]);
            fail = 1;
            break;
        }
    unsetenv("GLIDULATOR_AA_POST");
    if (open_320x240())
        return 1;
    if (fail) {
        fprintf(stderr, "Test 19 FAILED\n");
        return 1;
    }
    printf("  ok: auto 1->2->4->2, %d edges blended, areas/LFB exact (build/linux/aa_post.png)\n", mixed);
    return 0;
}

/* Video filter: dithered grey 100/150/200 is smoothed (mean close to the
 * true value, much less spread), a hard edge stays. */
static int test_voodoo_filter(void) {
    printf("-- Test 20: Voodoo video filter (GLIDULATOR_VOODOO_FILTER) --\n");
    int fail = 0;
    setenv("GLIDULATOR_VOODOO_FILTER", "1", 1);
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_4x4);
    grBufferClear(0, 0, 0);
    quad(40, 40, 160, 200, 100.4f, 150.4f, 200.4f, 0);
    quad(160, 40, 280, 200, 255, 255, 255, 0);
    g_have32 = 0;
    grBufferSwap(1);
    if (!g_have32) {
        fprintf(stderr, "no 32-bit picture (platform_blit32)\n");
        fail = 1;
    }
    double sum = 0, sq = 0;
    int    n = 0, lo = 255, hi = 0;
    for (int y = 60; y < 180; y++)
        for (int x = 60; x < 140; x++) {
            int r = (int) ((g_frame32[y * 320 + x] >> 16) & 0xff);
            sum += r;
            sq += (double) r * r;
            n++;
            lo = r < lo ? r : lo;
            hi = r > hi ? r : hi;
        }
    double mean = sum / n, sd = sqrt(sq / n - mean * mean);
    /* Comparison: the same surface without filter (565 widened to 8 bits
     * like the filter does: r5 << 3); the filter must keep the mean */
    unsetenv("GLIDULATOR_VOODOO_FILTER");
    if (open_320x240())
        return 1;
    grDitherMode(GR_DITHER_4x4);
    grBufferClear(0, 0, 0);
    quad(40, 40, 160, 200, 100.4f, 150.4f, 200.4f, 0);
    g_copy_frame = 1;
    grBufferSwap(1);
    g_copy_frame = 0;
    double raw = 0, rsq = 0;
    for (int y = 60; y < 180; y++)
        for (int x = 60; x < 140; x++) {
            int r = (g_last_frame[y * 320 + x] >> 11) << 3;
            raw += r;
            rsq += (double) r * r;
        }
    raw /= n;
    double rsd = sqrt(rsq / n - raw * raw);
    printf("  without filter: mean %.1f, spread %.2f\n", raw, rsd);
    /* edge: left of white stays grey, right white */
    int left  = (int) ((g_frame32[100 * 320 + 156] >> 16) & 0xff);
    int right = (int) ((g_frame32[100 * 320 + 164] >> 16) & 0xff);
    printf("  red area: mean %.1f, spread %.2f (%d..%d); edge %d | %d\n", mean, sd, lo, hi, left, right);
    if (fabs(mean - raw) > 1.0 || sd > rsd / 2 || hi - lo > 6 || right < 240 || left > 120) {
        fprintf(stderr, "Video filter: values outside expectation\n");
        fail = 1;
    }
    {   /* for viewing: 32-bit image as PPM */
        FILE *f = fopen("build/linux/voodoo_filter.ppm", "wb");
        if (f) {
            fprintf(f, "P6 320 240 255\n");
            for (int i = 0; i < 320 * 240; i++) {
                uint8_t c[3] = { (uint8_t) (g_frame32[i] >> 16), (uint8_t) (g_frame32[i] >> 8), (uint8_t) g_frame32[i] };
                fwrite(c, 1, 3, f);
            }
            fclose(f);
        }
    }
    unsetenv("GLIDULATOR_VOODOO_FILTER");
    if (open_320x240())
        return 1;
    if (fail) {
        fprintf(stderr, "Test 20 FAILED\n");
        return 1;
    }
    printf("  ok: dither pattern smoothed, edge kept\n");
    return 0;
}

/* Writes through an LFB pointer after grLfbUnlock() and grSstWinClose():
 * on a Voodoo the pointer addresses the board and stays valid, and
 * Uprising writes through it on exit. Under ASan a freed buffer would be
 * reported here. */
static int test_lfb_after_close(void) {
    printf("-- Test 21: LFB pointers after grSstWinClose --\n");
    if (open_320x240())
        return 1;
    GrLfbInfo_t info565, info888;
    memset(&info565, 0, sizeof info565);
    memset(&info888, 0, sizeof info888);
    info565.size = info888.size = sizeof(GrLfbInfo_t);
    if (!grLfbLock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE,
                   &info565))
        return 1;
    grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
    if (!grLfbLock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_888, GR_ORIGIN_UPPER_LEFT, FXFALSE,
                   &info888))
        return 1;
    grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
    grSstWinClose();
    memset(info565.lfbPtr, 0x5a, (size_t) info565.strideInBytes * 240);
    memset(info888.lfbPtr, 0x5a, (size_t) info888.strideInBytes * 240);
    if (open_320x240())
        return 1;
    grBufferClear(0, 0, 0);
    grBufferSwap(1);
    return 0;
}

/* Start-up show: the animation presents frames, and afterwards Glide is in
 * exactly the state of a plain grSstWinOpen. Frames go to
 * build/linux/splash_*.png for viewing. The original 3dfx splash needs an
 * original Glide 2.x binary: GLIDULATOR_TEST_SPLASH_FILE=<path> (otherwise
 * skipped). */
static int splash_open(const char *mode, const char *crt, const char *prefix, int every, GrState *st) {
    setenv("GLIDULATOR_SPLASH", mode, 1);
    if (crt)
        setenv("GLIDULATOR_CRT", crt, 1);
    else
        unsetenv("GLIDULATOR_CRT");
    g_dump_prefix = prefix;
    g_dump_every  = every;
    g_dump_n      = 0;
    g_blits       = 0;
    int ok = grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1);
    grGlideGetState(st);
    return ok;
}

static int test_splash(void) {
    printf("-- Test 22: start animation (GLIDULATOR_SPLASH) --\n");
    int     fail = 0;
    GrState plain, after;
    memset(&plain, 0, sizeof plain);
    memset(&after, 0, sizeof after);
    if (!splash_open("off", "0", NULL, 1, &plain))
        return 1;
    grSstWinClose();

    /* The logo plays once per process: gl1, or original with a file. */
    const char *file = getenv("GLIDULATOR_TEST_SPLASH_FILE");
    const char *mode = file ? "original" : "gl1";
    if (file)
        setenv("GLIDULATOR_SPLASH_FILE", file, 1);
    {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "build/linux/splash_%s_", mode);
        if (!splash_open(mode, "1", prefix, file ? 6 : 15, &after))
            return 1;
        printf("  %s: %d frames\n", mode, g_blits);
        if (memcmp(&plain, &after, sizeof plain)) {
            fprintf(stderr, "state differs after the animation:");
            for (size_t i = 0; i < sizeof plain / 4; i++)
                if (((uint32_t *) &plain)[i] != ((uint32_t *) &after)[i])
                    fprintf(stderr, " [%zu] %08x/%08x", i, ((uint32_t *) &plain)[i], ((uint32_t *) &after)[i]);
            fprintf(stderr, "\n");
            fail = 1;
        }
        grBufferClear(0, 0, 0);
        quad(100, 100, 200, 200, 255, 0, 0, 0);
        grBufferSwap(0);
        fail |= check_pixel(150, 150, 0xf800, "after animation");
        grSstWinClose();
    }

    /* GLIDULATOR_CRT=1 without logo, and the older spelling
     * GLIDULATOR_SPLASH=crt: sync-in on the first frame, sync-out on close */
    for (int k = 0; k < 2; k++) {
        if (!splash_open(k ? "crt" : "off", k ? NULL : "1", k ? NULL : "build/linux/splash_crt_", 8, &after))
            return 1;
        grBufferClear(0, 0, 0);
        quad(100, 100, 540, 380, 60, 160, 255, 0);
        g_blits = 0;
        grBufferSwap(0);
        printf("  %s: %d frames while locking in\n", k ? "SPLASH=crt" : "CRT=1", g_blits);
        if (g_blits < 10) {
            fprintf(stderr, "crt: no lock-in\n");
            fail = 1;
        }
        g_blits = 0;
        grSstWinClose();
        if (g_blits < 10) {
            fprintf(stderr, "crt: no closing effect\n");
            fail = 1;
        }
    }

    /* Logo without the monitor effect (GLIDULATOR_CRT=0): the logo has
     * already played in this process, so only the game's frames are
     * checked: no sync-in, no sync-out. */
    if (!splash_open("gl1", "0", NULL, 1, &after))
        return 1;
    grBufferClear(0, 0, 0);
    quad(100, 100, 540, 380, 60, 160, 255, 0);
    g_blits = 0;
    grBufferSwap(0);
    if (g_blits != 1) {
        fprintf(stderr, "CRT=0: %d frames instead of 1 on the first frame\n", g_blits);
        fail = 1;
    }
    g_blits = 0;
    grSstWinClose();
    if (g_blits) {
        fprintf(stderr, "CRT=0: closing effect anyway (%d frames)\n", g_blits);
        fail = 1;
    }
    g_dump_prefix = NULL;
    unsetenv("GLIDULATOR_CRT");
    setenv("GLIDULATOR_SPLASH", "off", 1);
    unsetenv("GLIDULATOR_SPLASH_FILE");
    return fail;
}

int main(void) {
    printf("glidulator smoke test\n");
    setenv("GLIDULATOR_SPLASH", "off", 1); /* the logo is on by default; only test 22 plays it */
    printf("  core_count = %d\n", platform_core_count());

    glide_state *gs = glide_init();
    if (!gs) {
        fprintf(stderr, "glide_init failed\n");
        return 1;
    }

    if (test_flat_triangle())
        return 1;
    if (test_textured_triangle())
        return 1;
    if (test_tex_mem_queries())
        return 1;
    if (test_zbuffer() || test_mipmap_p8() || test_perspective() || test_combine_blend_cull())
        return 1;
    if (test_demo_triangles() || test_demo_triangles_mipmap())
        return 1;
    if (test_lfb() || test_sst() || test_gutex() || test_fog_ncc() || test_gu_presets() || test_clip() || test_renderer() || test_output_osd() || test_fsaa() || test_fsaa_auto_post() || test_voodoo_filter() ||
        test_lfb_after_close() || test_splash())
        return 1;

    grGlideShutdown();
    glide_shutdown(gs);

    printf("ok\n");
    return 0;
}
