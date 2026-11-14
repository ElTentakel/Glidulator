/* setenv despite -std=c99 */
#define _POSIX_C_SOURCE 200112L

/* Glide 3 test harness: core/ + core/glide3_abi.c + platform/linux, built
 * with -DGLIDULATOR_GLIDE3 like glide3x.dll (Glide 2 functions renamed to
 * g2_*). Checks what the Glide 3 layer itself does:
 *   - scenes of the Glide 2 harness (targets/glide_test_linux) drawn again
 *     through the Glide 3 API must match their golden images in
 *     tests/golden/ pixel for pixel (vertex layout, log2 texture sizes,
 *     vertex arrays, changed signatures);
 *   - grGet/grGetString/grQueryResolutions, viewport, state save/restore;
 *   - vertex array modes (strip/fan/polygon/continue, pointer arrays)
 *     against each other, fog modes against the Glide 2 path.
 * Golden images are only read here, never written. On a mismatch the
 * rendered frame is saved as build/linux_g3/<name>.png. */

#ifndef GLIDULATOR_GLIDE3
#error "build with -DGLIDULATOR_GLIDE3 (make test)"
#endif
#include "glidulator/glide2x.h"
#include "glidulator/glide_state.h"

#include <png.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Glide 3 API (core/glide3_abi.c; exported under these names without
 *     the g3_ prefix by glide3x.dll) ------------------------------------- */

typedef FxU32 GrContext_t;

typedef struct {
    GrScreenResolution_t resolution;
    GrScreenRefresh_t    refresh;
    int                  numColorBuffers;
    int                  numAuxBuffers;
} GrResolution;

void GLIDE_API        g3_grGlideInit(void);
GrContext_t GLIDE_API g3_grSstWinOpen(FxU32 hWnd, GrScreenResolution_t res, GrScreenRefresh_t refresh,
                                      GrColorFormat_t color_format, GrOriginLocation_t origin, int nColBuffers,
                                      int nAuxBuffers);
FxBool GLIDE_API      g3_grSstWinClose(GrContext_t context);
FxBool GLIDE_API      g3_grSelectContext(GrContext_t context);
FxI32 GLIDE_API       g3_grQueryResolutions(const GrResolution *tmpl, GrResolution *output);
FxU32 GLIDE_API       g3_grGet(FxU32 pname, FxU32 plength, FxI32 *params);
const char *GLIDE_API g3_grGetString(FxU32 pname);
void GLIDE_API        g3_grViewport(FxI32 x, FxI32 y, FxI32 width, FxI32 height);
void GLIDE_API        g3_grVertexLayout(FxU32 param, FxI32 offset, FxU32 mode);
void GLIDE_API        g3_grGlideGetVertexLayout(void *layout);
void GLIDE_API        g3_grGlideSetVertexLayout(const void *layout);
void GLIDE_API        g3_grGlideGetState(void *state);
void GLIDE_API        g3_grGlideSetState(const void *state);
void GLIDE_API        g3_grDrawTriangle(const void *a, const void *b, const void *c);
void GLIDE_API        g3_grDrawVertexArray(FxU32 mode, FxU32 count, void *pointers);
void GLIDE_API        g3_grDrawVertexArrayContiguous(FxU32 mode, FxU32 count, void *pointers, FxU32 stride);
void GLIDE_API        g3_grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU32 depth);
void GLIDE_API        g3_grBufferSwap(FxU32 swap_interval);
void GLIDE_API        g3_grAlphaBlendFunction(GrAlphaBlendFnc_t rs, GrAlphaBlendFnc_t rd, GrAlphaBlendFnc_t as,
                                              GrAlphaBlendFnc_t ad);
void GLIDE_API        g3_grColorCombine(GrCombineFunction_t f, GrCombineFactor_t fa, GrCombineLocal_t l,
                                        GrCombineOther_t o, FxBool inv);
void GLIDE_API        g3_grConstantColorValue(GrColor_t value);
void GLIDE_API        g3_grCullMode(GrCullMode_t mode);
void GLIDE_API        g3_grDepthBufferFunction(GrCmpFnc_t function);
void GLIDE_API        g3_grDepthBufferMode(GrDepthBufferMode_t mode);
void GLIDE_API        g3_grDepthMask(FxBool mask);
void GLIDE_API        g3_grDitherMode(GrDitherMode_t mode);
void GLIDE_API        g3_grFogColorValue(GrColor_t fogcolor);
void GLIDE_API        g3_grFogMode(GrFogMode_t mode);
void GLIDE_API        g3_grFogTable(const GrFog_t ft[]);
float GLIDE_API       g3_guFogTableIndexToW(int i);
FxU32 GLIDE_API       g3_grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax, GrAspectRatio_t aspect,
                                              GrTextureFormat_t fmt);
FxU32 GLIDE_API       g3_grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo *info);
void GLIDE_API        g3_grTexDownloadTable(GrTexTable_t type, void *data);
void GLIDE_API        g3_grTexSource(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info);
void GLIDE_API        g3_grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minf, GrTextureFilterMode_t magf);
void GLIDE_API        g3_grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info);
void GLIDE_API        g3_grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                                                  GrLOD_t largeLod, GrAspectRatio_t aspect, GrTextureFormat_t format,
                                                  FxU32 evenOdd, void *data);
void GLIDE_API        g3_grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool lodBlend);

/* Glide 3 values (glide3x.h) */
#define G3_PARAM_XY    0x01
#define G3_PARAM_Z     0x02
#define G3_PARAM_Q     0x04
#define G3_PARAM_A     0x10
#define G3_PARAM_RGB   0x20
#define G3_PARAM_PARGB 0x30
#define G3_PARAM_ST0   0x40
#define G3_PARAM_Q0    0x50
#define G3_PARAM_ENABLE  1
#define G3_PARAM_DISABLE 0

#define G3_POINTS                  0
#define G3_LINE_STRIP              1
#define G3_LINES                   2
#define G3_POLYGON                 3
#define G3_TRIANGLE_STRIP          4
#define G3_TRIANGLE_FAN            5
#define G3_TRIANGLES               6
#define G3_TRIANGLE_STRIP_CONTINUE 7
#define G3_TRIANGLE_FAN_CONTINUE   8

#define G3_FOG_WITH_TABLE_ON_Q 2

#define G3_BITS_DEPTH              0x01
#define G3_BITS_RGBA               0x02
#define G3_FOG_TABLE_ENTRIES       0x04
#define G3_GLIDE_STATE_SIZE        0x06
#define G3_GLIDE_VERTEXLAYOUT_SIZE 0x07
#define G3_MAX_TEXTURE_SIZE        0x0a
#define G3_NUM_TMU                 0x13
#define G3_VIEWPORT                0x26
#define G3_VIDEO_POSITION          0x25

#define G3_HARDWARE 0xa1
#define G3_VERSION  0xa4
#define G3_QUERY_ANY ((FxU32) -1)

#define G3_LOD_LOG2_1      0
#define G3_LOD_LOG2_16     4
#define G3_LOD_LOG2_32     5
#define G3_LOD_LOG2_64     6
#define G3_LOD_LOG2_256    8
#define G3_ASPECT_LOG2_1x1 0
#define G3_ASPECT_LOG2_2x1 1

/* --- Frame capture (platform/linux leaves the output to the program) ---- */

#define W 320
#define H 240

static const uint16_t *g_last_frame;
static int             g_last_w, g_last_h;

void platform_blit(const void *pixels, int w, int h, int stride_bytes) {
    (void) stride_bytes;
    g_last_frame = (const uint16_t *) pixels;
    g_last_w     = w;
    g_last_h     = h;
}

void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    (void) pixels;
    (void) w;
    (void) h;
    (void) stride_bytes;
}

static int check_pixel(int x, int y, uint16_t expected, const char *what) {
    uint16_t got = g_last_frame[y * g_last_w + x];
    if (got != expected) {
        fprintf(stderr, "Pixel (%d,%d) %s: expected %04x, got %04x\n", x, y, what, expected, got);
        return 1;
    }
    return 0;
}

static void save_png(const char *path, const uint16_t *px, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;
    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    uint8_t    *row  = (uint8_t *) malloc((size_t) w * 3);
    if (png && info && row && !setjmp(png_jmpbuf(png))) {
        png_init_io(png, fp);
        png_set_IHDR(png, info, (png_uint_32) w, (png_uint_32) h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint16_t p = px[y * w + x];
                int      r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
                row[x * 3]     = (uint8_t) ((r << 3) | (r >> 2));
                row[x * 3 + 1] = (uint8_t) ((g << 2) | (g >> 4));
                row[x * 3 + 2] = (uint8_t) ((b << 3) | (b >> 2));
            }
            png_write_row(png, row);
        }
        png_write_end(png, info);
        printf("  PNG saved: %s\n", path);
    }
    free(row);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
}

/* Golden PNG (written by targets/glide_test_linux from RGB565 with bit
 * replication) back to RGB565: exact. */
static int load_golden(const char *path, uint16_t *out, int w, int h) {
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) {
        fprintf(stderr, "%s: %s\n", path, img.message);
        return 1;
    }
    if ((int) img.width != w || (int) img.height != h) {
        fprintf(stderr, "%s: %ux%u, expected %dx%d\n", path, img.width, img.height, w, h);
        png_image_free(&img);
        return 1;
    }
    img.format   = PNG_FORMAT_RGB;
    uint8_t *rgb = (uint8_t *) malloc(PNG_IMAGE_SIZE(img));
    if (!rgb || !png_image_finish_read(&img, NULL, rgb, 0, NULL)) {
        fprintf(stderr, "%s: read failed\n", path);
        free(rgb);
        return 1;
    }
    for (int i = 0; i < w * h; i++)
        out[i] = (uint16_t) (((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) | (rgb[i * 3 + 2] >> 3));
    free(rgb);
    return 0;
}

/* Last frame against tests/golden/<name>.png, pixel for pixel */
static int compare_golden(const char *name) {
    static uint16_t ref[W * H];
    char            path[128];
    if (!g_last_frame || g_last_w != W || g_last_h != H) {
        fprintf(stderr, "no %dx%d frame captured\n", W, H);
        return 1;
    }
    snprintf(path, sizeof path, "tests/golden/%s.png", name);
    if (load_golden(path, ref, W, H))
        return 1;
    int diff = 0, first = -1;
    for (int i = 0; i < W * H; i++)
        if (g_last_frame[i] != ref[i]) {
            if (first < 0)
                first = i;
            diff++;
        }
    if (diff) {
        fprintf(stderr, "%s: %d pixels differ, first at (%d,%d): golden %04x, got %04x\n", path, diff, first % W,
                first / W, ref[first], g_last_frame[first]);
        snprintf(path, sizeof path, "build/linux_g3/%s.png", name);
        save_png(path, g_last_frame, W, H);
        return 1;
    }
    printf("  identical to tests/golden/%s.png\n", name);
    return 0;
}

static uint16_t g_frame_a[W * H];

static void keep_frame(uint16_t *dst) {
    memcpy(dst, g_last_frame, sizeof g_frame_a);
}

static int same_frame(const uint16_t *ref, const char *what) {
    for (int i = 0; i < W * H; i++)
        if (g_last_frame[i] != ref[i]) {
            fprintf(stderr, "%s: differs at (%d,%d): %04x instead of %04x\n", what, i % W, i / W, g_last_frame[i],
                    ref[i]);
            return 1;
        }
    return 0;
}

/* --- Vertices ------------------------------------------------------------ */

/* An application's own vertex: float colour and packed ARGB side by side,
 * so both colour layouts can draw the same vertices. */
typedef struct {
    float x, y;
    float ooz, oow;
    float r, g, b, a;
    float sow, tow, oow0;
    FxU32 argb;
} V3;

#define OFS(m) ((FxI32) offsetof(V3, m))

static V3 v3(float x, float y, float r, float g, float b, float a, float ooz, float oow, float s, float t) {
    V3 v;
    memset(&v, 0, sizeof v);
    v.x    = x;
    v.y    = y;
    v.ooz  = ooz;
    v.oow  = oow;
    v.r    = r;
    v.g    = g;
    v.b    = b;
    v.a    = a;
    v.sow  = s * oow; /* S/W, T/W as in Glide 2 */
    v.tow  = t * oow;
    v.oow0 = oow;
    v.argb = ((FxU32) a << 24) | ((FxU32) r << 16) | ((FxU32) g << 8) | (FxU32) b;
    return v;
}

/* Float RGB + A, Z, Q, ST0; Q0 off (TMU0 takes Q) */
static void layout_float(void) {
    g3_grVertexLayout(G3_PARAM_XY, OFS(x), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_Z, OFS(ooz), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_Q, OFS(oow), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_RGB, OFS(r), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_A, OFS(a), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_ST0, OFS(sow), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_Q0, 0, G3_PARAM_DISABLE);
}

/* Packed ARGB instead of RGB/A, own Q0 */
static void layout_packed(void) {
    layout_float();
    g3_grVertexLayout(G3_PARAM_PARGB, OFS(argb), G3_PARAM_ENABLE);
    g3_grVertexLayout(G3_PARAM_Q0, OFS(oow0), G3_PARAM_ENABLE);
}

static void quad3(FxU32 mode, float x0, float y0, float x1, float y1, float r, float g, float b, float ooz) {
    V3 q[4];
    q[0] = v3(x0, y0, r, g, b, 255, ooz, 1, 0, 0);
    q[1] = v3(x1, y0, r, g, b, 255, ooz, 1, 0, 0);
    q[2] = v3(x1, y1, r, g, b, 255, ooz, 1, 0, 0);
    q[3] = v3(x0, y1, r, g, b, 255, ooz, 1, 0, 0);
    g3_grDrawVertexArrayContiguous(mode, 4, q, sizeof q[0]);
}

static int open_win(int col, int aux) {
    if (!g3_grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, col,
                         aux)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }
    return 0;
}

/* --- Golden scenes through the Glide 3 API --------------------------------- */

/* Glide 2 test 1, once with float RGB and once with packed ARGB */
static int test_flat_triangle(void) {
    printf("-- G3 test 1: flat triangle, float and packed colour --\n");
    int fail = 0;
    for (int packed = 0; packed < 2; packed++) {
        if (open_win(1, 0))
            return 1;
        packed ? layout_packed() : layout_float();
        g3_grBufferClear(0x00000000, 0xff, 0);
        V3 a = v3(32, 8, 255, 0, 0, 255, 0, 1, 0, 0);
        V3 b = v3(8, 56, 255, 0, 0, 255, 0, 1, 0, 0);
        V3 c = v3(56, 56, 255, 0, 0, 255, 0, 1, 0, 0);
        g3_grDrawTriangle(&b, &a, &c);
        g3_grBufferSwap(1);
        fail |= compare_golden("flat_triangle");
    }
    return fail;
}

/* Glide 2 test 2 with log2 texture sizes */
static int test_textured_triangle(void) {
    printf("-- G3 test 2: textured triangle, log2 LOD/aspect --\n");
    static uint16_t tex[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            tex[y * 32 + x] = ((x / 8 + y / 8) % 2 == 0) ? 0xffff : 0x001f;
    if (open_win(1, 0))
        return 1;
    layout_float();
    GrTexInfo info;
    info.smallLod    = G3_LOD_LOG2_32;
    info.largeLod    = G3_LOD_LOG2_32;
    info.aspectRatio = G3_ASPECT_LOG2_1x1;
    info.format      = GR_TEXFMT_RGB_565;
    info.data        = tex;
    if (g3_grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &info) != 32 * 32 * 2) {
        fprintf(stderr, "grTexTextureMemRequired(32x32 565) wrong\n");
        return 1;
    }
    g3_grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    g3_grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    g3_grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                      GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    g3_grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    g3_grBufferClear(0x0000ff00, 0xff, 0);
    V3 a = v3(8, 8, 255, 255, 255, 255, 0, 1, 0, 0);
    V3 b = v3(8, 40, 255, 255, 255, 255, 0, 1, 0, 256);
    V3 c = v3(40, 40, 255, 255, 255, 255, 0, 1, 256, 256);
    g3_grDrawTriangle(&a, &b, &c);
    g3_grBufferSwap(1);
    return compare_golden("textured_triangle");
}

/* Glide 2 test 4: polygons through pointer arrays and contiguous fans */
static int test_zbuffer(void) {
    printf("-- G3 test 3: Z-buffer, polygon pointer arrays --\n");
    if (open_win(2, 1))
        return 1;
    layout_float();
    g3_grDitherMode(GR_DITHER_DISABLE);
    g3_grDepthBufferMode(GR_DEPTHBUFFER_ZBUFFER);
    g3_grDepthBufferFunction(GR_CMP_GREATER);
    g3_grDepthMask(FXTRUE);
    g3_grBufferClear(0, 0, 0);
    static const float q[3][8] = {
        {10, 10, 60, 60, 255, 0, 0, 1000},
        {30, 30, 80, 80, 0, 255, 0, 500},
        {50, 50, 70, 70, 0, 0, 255, 2000},
    };
    for (int i = 0; i < 3; i++) {
        V3    v[4];
        void *p[4];
        v[0] = v3(q[i][0], q[i][1], q[i][4], q[i][5], q[i][6], 255, q[i][7], 1, 0, 0);
        v[1] = v3(q[i][2], q[i][1], q[i][4], q[i][5], q[i][6], 255, q[i][7], 1, 0, 0);
        v[2] = v3(q[i][2], q[i][3], q[i][4], q[i][5], q[i][6], 255, q[i][7], 1, 0, 0);
        v[3] = v3(q[i][0], q[i][3], q[i][4], q[i][5], q[i][6], 255, q[i][7], 1, 0, 0);
        for (int k = 0; k < 4; k++)
            p[k] = &v[k];
        g3_grDrawVertexArray(G3_POLYGON, 4, p);
    }
    g3_grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    quad3(G3_TRIANGLE_FAN, 100, 10, 120, 30, 255, 255, 255, 0);
    g3_grBufferSwap(1);
    return compare_golden("zbuffer");
}

/* Glide 2 test 5: P8 mipmaps with palette (no TMU argument in Glide 3),
 * log2 sizes, levels downloaded one by one, quads as strips */
static int test_mipmap_p8(void) {
    printf("-- G3 test 4: mipmaps, P8 + palette, 2x1, strips --\n");
    if (open_win(2, 1))
        return 1;
    layout_float();
    g3_grDitherMode(GR_DITHER_DISABLE);
    static uint8_t data[64 * 32 + 32 * 16 + 16 * 8];
    memset(data, 1, 64 * 32);
    memset(data + 64 * 32, 2, 32 * 16);
    memset(data + 64 * 32 + 32 * 16, 3, 16 * 8);
    GuTexPalette pal;
    memset(&pal, 0, sizeof pal);
    pal.data[1] = 0xff0000;
    pal.data[2] = 0x00ff00;
    pal.data[3] = 0x0000ff;
    GrTexInfo info;
    info.largeLod    = G3_LOD_LOG2_64;
    info.smallLod    = G3_LOD_LOG2_16;
    info.aspectRatio = G3_ASPECT_LOG2_2x1;
    info.format      = GR_TEXFMT_P_8;
    info.data        = data;
    FxU32 addr = 4096;
    if (g3_grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &info) != sizeof data) {
        fprintf(stderr, "grTexTextureMemRequired wrong\n");
        return 1;
    }
    g3_grTexDownloadTable(GR_TEXTABLE_PALETTE, &pal);
    g3_grTexDownloadMipMapLevel(GR_TMU0, addr, G3_LOD_LOG2_64, G3_LOD_LOG2_64, G3_ASPECT_LOG2_2x1, GR_TEXFMT_P_8,
                                GR_MIPMAPLEVELMASK_BOTH, data);
    g3_grTexDownloadMipMapLevel(GR_TMU0, addr, G3_LOD_LOG2_32, G3_LOD_LOG2_64, G3_ASPECT_LOG2_2x1, GR_TEXFMT_P_8,
                                GR_MIPMAPLEVELMASK_BOTH, data + 64 * 32);
    g3_grTexDownloadMipMapLevel(GR_TMU0, addr, G3_LOD_LOG2_16, G3_LOD_LOG2_64, G3_ASPECT_LOG2_2x1, GR_TEXFMT_P_8,
                                GR_MIPMAPLEVELMASK_BOTH, data + 64 * 32 + 32 * 16);
    g3_grTexSource(GR_TMU0, addr, GR_MIPMAPLEVELMASK_BOTH, &info);
    g3_grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);
    g3_grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    g3_grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                      GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    g3_grBufferClear(0, 0, 0);
    static const float q[3][4] = {{10, 10, 64, 32}, {90, 10, 32, 16}, {130, 10, 16, 8}};
    for (int i = 0; i < 3; i++) {
        float x = q[i][0], y = q[i][1], w = q[i][2], h = q[i][3];
        V3    s[4]; /* strip order: top left, top right, bottom left, bottom right */
        s[0] = v3(x, y, 255, 255, 255, 255, 0, 1, 0, 0);
        s[1] = v3(x + w, y, 255, 255, 255, 255, 0, 1, 256, 0);
        s[2] = v3(x, y + h, 255, 255, 255, 255, 0, 1, 0, 128);
        s[3] = v3(x + w, y + h, 255, 255, 255, 255, 0, 1, 256, 128);
        g3_grDrawVertexArrayContiguous(G3_TRIANGLE_STRIP, 4, s, sizeof s[0]);
    }
    g3_grBufferSwap(1);
    return compare_golden("mipmap_p8");
}

/* Glide 2 test 7: constant colour, blending (triangle list), culling */
static int test_combine_blend_cull(void) {
    printf("-- G3 test 5: combine, blending, culling --\n");
    if (open_win(2, 1))
        return 1;
    layout_packed();
    g3_grDitherMode(GR_DITHER_DISABLE);
    g3_grBufferClear(0x00ff0000, 0, 0);
    g3_grConstantColorValue(0xff00ff00);
    g3_grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT,
                      GR_COMBINE_OTHER_NONE, FXFALSE);
    quad3(G3_POLYGON, 10, 10, 30, 30, 255, 255, 255, 0);
    g3_grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                      GR_COMBINE_OTHER_ITERATED, FXFALSE);
    g3_grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
    V3 c0 = v3(40, 10, 0, 0, 255, 128, 0, 1, 0, 0), c1 = v3(60, 10, 0, 0, 255, 128, 0, 1, 0, 0);
    V3 c2 = v3(60, 30, 0, 0, 255, 128, 0, 1, 0, 0), c3 = v3(40, 30, 0, 0, 255, 128, 0, 1, 0, 0);
    V3 tris[6] = {c0, c1, c2, c0, c2, c3}; /* the fan of the Glide 2 polygon as a list */
    g3_grDrawVertexArrayContiguous(G3_TRIANGLES, 6, tris, sizeof tris[0]);
    g3_grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    g3_grCullMode(GR_CULL_NEGATIVE);
    V3 a = v3(80, 10, 255, 255, 255, 255, 0, 1, 0, 0), b = v3(100, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    V3 c = v3(80, 30, 255, 255, 255, 255, 0, 1, 0, 0);
    g3_grDrawTriangle(&a, &b, &c);
    V3 d = v3(120, 10, 255, 255, 255, 255, 0, 1, 0, 0), e = v3(140, 10, 255, 255, 255, 255, 0, 1, 0, 0);
    V3 f = v3(120, 30, 255, 255, 255, 255, 0, 1, 0, 0);
    g3_grDrawTriangle(&d, &f, &e);
    g3_grCullMode(GR_CULL_DISABLE);
    g3_grBufferSwap(1);
    return compare_golden("combine_blend_cull");
}

/* --- Glide 3 only ------------------------------------------------------------ */

static int test_queries(void) {
    printf("-- G3 test 6: grGet, grGetString, grQueryResolutions, grViewport --\n");
    int   fail = 0;
    FxI32 p[8];
    if (open_win(2, 1))
        return 1;
#define EXPECT_GET(pname, len, want, what)                                                     \
    do {                                                                                       \
        memset(p, 0x55, sizeof p);                                                             \
        if (g3_grGet(pname, len, p) != (len) || p[0] != (want)) {                              \
            fprintf(stderr, "grGet(%s): expected %d, got %d\n", what, (int) (want), (int) p[0]); \
            fail = 1;                                                                          \
        }                                                                                      \
    } while (0)
    EXPECT_GET(G3_BITS_DEPTH, 4, 16, "BITS_DEPTH");
    EXPECT_GET(G3_FOG_TABLE_ENTRIES, 4, GR_FOG_TABLE_SIZE, "FOG_TABLE_ENTRIES");
    EXPECT_GET(G3_MAX_TEXTURE_SIZE, 4, 256, "MAX_TEXTURE_SIZE");
    EXPECT_GET(G3_NUM_TMU, 4, 2, "NUM_TMU (Voodoo2 default)");
    if (g3_grGet(G3_BITS_RGBA, 16, p) != 16 || p[0] != 5 || p[1] != 6 || p[2] != 5 || p[3] != 0) {
        fprintf(stderr, "grGet(BITS_RGBA) not 5/6/5/0\n");
        fail = 1;
    }
    /* wrong length or unknown/unanswered pname: 0 bytes, params untouched */
    p[0] = 1234;
    if (g3_grGet(G3_BITS_RGBA, 4, p) != 0 || g3_grGet(G3_BITS_DEPTH, 8, p) != 0 || g3_grGet(0x7777, 4, p) != 0 ||
        g3_grGet(G3_VIDEO_POSITION, 8, p) != 0 || p[0] != 1234) {
        fprintf(stderr, "grGet with a wrong length/pname did not return 0 untouched\n");
        fail = 1;
    }
    if (g3_grGet(G3_BITS_DEPTH, 4, NULL) != 0) {
        fprintf(stderr, "grGet(params NULL) not 0\n");
        fail = 1;
    }
    /* the state/layout sizes applications allocate */
    if (g3_grGet(G3_GLIDE_STATE_SIZE, 4, p) != 4 || p[0] < (FxI32) sizeof(GrState) ||
        g3_grGet(G3_GLIDE_VERTEXLAYOUT_SIZE, 4, p + 1) != 4 || p[1] <= 0) {
        fprintf(stderr, "grGet(GLIDE_STATE_SIZE/VERTEXLAYOUT_SIZE) implausible: %d/%d\n", (int) p[0], (int) p[1]);
        fail = 1;
    }
    /* viewport: whole window after grSstWinOpen, then what grViewport set */
    if (g3_grGet(G3_VIEWPORT, 16, p) != 16 || p[0] != 0 || p[1] != 0 || p[2] != W || p[3] != H) {
        fprintf(stderr, "grGet(VIEWPORT) after grSstWinOpen: %d,%d %dx%d\n", (int) p[0], (int) p[1], (int) p[2],
                (int) p[3]);
        fail = 1;
    }
    g3_grViewport(10, 20, 100, 50);
    if (g3_grGet(G3_VIEWPORT, 16, p) != 16 || p[0] != 10 || p[1] != 20 || p[2] != 100 || p[3] != 50) {
        fprintf(stderr, "grGet(VIEWPORT) after grViewport wrong\n");
        fail = 1;
    }
    if (strcmp(g3_grGetString(G3_VERSION), "3.03") != 0 || strcmp(g3_grGetString(G3_HARDWARE), "Voodoo2") != 0 ||
        strcmp(g3_grGetString(0x1234), "ERROR") != 0) {
        fprintf(stderr, "grGetString wrong\n");
        fail = 1;
    }
    if (!g3_grSelectContext(1) || g3_grSelectContext(0)) {
        fprintf(stderr, "grSelectContext wrong\n");
        fail = 1;
    }

    /* grQueryResolutions: 7 resolutions x 2-3 colour x 0-1 aux buffers */
    GrResolution any = {(GrScreenResolution_t) G3_QUERY_ANY, (GrScreenRefresh_t) G3_QUERY_ANY, (int) G3_QUERY_ANY,
                        (int) G3_QUERY_ANY};
    FxI32        size = g3_grQueryResolutions(&any, NULL);
    if (size != 7 * 2 * 2 * (FxI32) sizeof(GrResolution)) {
        fprintf(stderr, "grQueryResolutions(any) size %d\n", (int) size);
        fail = 1;
    } else {
        GrResolution list[28];
        memset(list, 0, sizeof list);
        if (g3_grQueryResolutions(&any, list) != size || list[27].resolution != GR_RESOLUTION_800x600 ||
            list[27].numColorBuffers != 3 || list[27].numAuxBuffers != 1) {
            fprintf(stderr, "grQueryResolutions(any) list wrong\n");
            fail = 1;
        }
    }
    GrResolution one = {GR_RESOLUTION_640x480, (GrScreenRefresh_t) G3_QUERY_ANY, 2, 1}, out;
    memset(&out, 0, sizeof out);
    if (g3_grQueryResolutions(&one, &out) != (FxI32) sizeof out || out.resolution != GR_RESOLUTION_640x480 ||
        out.numColorBuffers != 2 || out.numAuxBuffers != 1) {
        fprintf(stderr, "grQueryResolutions(640x480, 2, 1) wrong\n");
        fail = 1;
    }
    GrResolution none = {GR_RESOLUTION_640x480, (GrScreenRefresh_t) G3_QUERY_ANY, 1, 0};
    if (g3_grQueryResolutions(&none, NULL) != 0 || g3_grQueryResolutions(NULL, NULL) != 0) {
        fprintf(stderr, "grQueryResolutions(1 colour buffer / NULL) not 0\n");
        fail = 1;
    }

    /* texture sizes in log2: the same bytes as Glide 2 test 3 */
    if (g3_grTexCalcMemRequired(G3_LOD_LOG2_1, G3_LOD_LOG2_256, G3_ASPECT_LOG2_1x1, GR_TEXFMT_RGB_565) != 174768) {
        fprintf(stderr, "grTexCalcMemRequired(log2 1..256) wrong\n");
        fail = 1;
    }
#undef EXPECT_GET
    if (!fail)
        printf("  ok\n");
    return fail;
}

/* The same gouraud quad through every triangle array mode, contiguous and
 * as pointer array, and split into strip/fan + *_CONTINUE: identical
 * frames. With GR_CULL_NEGATIVE every triangle of a strip must survive (the
 * winding alternates; the board flips its cull sign). */
static int test_vertex_arrays(void) {
    printf("-- G3 test 7: vertex array modes --\n");
    V3 tl = v3(40, 40, 255, 0, 0, 255, 0, 1, 0, 0), tr = v3(200, 50, 0, 255, 0, 255, 0, 1, 0, 0);
    V3 br = v3(210, 180, 0, 0, 255, 255, 0, 1, 0, 0), bl = v3(30, 170, 255, 255, 0, 255, 0, 1, 0, 0);
    V3 list[6] = {tl, tr, br, tl, br, bl};
    V3 fan[4]  = {tl, tr, br, bl};
    /* same diagonal tl-br and winding as the fan: (bl,tl,br), (br,tl,tr) */
    V3 strip[4] = {bl, tl, br, tr};
    struct {
        const char *what;
        FxU32       mode, count;
        V3         *v;
        FxU32       cont_mode; /* != 0: first 3 vertices, then the rest with this mode */
        int         pointers;
    } cases[] = {
        {"TRIANGLES", G3_TRIANGLES, 6, list, 0, 0},
        {"TRIANGLE_FAN", G3_TRIANGLE_FAN, 4, fan, 0, 0},
        {"POLYGON", G3_POLYGON, 4, fan, 0, 0},
        {"TRIANGLE_STRIP", G3_TRIANGLE_STRIP, 4, strip, 0, 0},
        {"TRIANGLE_STRIP pointer array", G3_TRIANGLE_STRIP, 4, strip, 0, 1},
        {"TRIANGLE_FAN pointer array", G3_TRIANGLE_FAN, 4, fan, 0, 1},
        {"TRIANGLE_STRIP + STRIP_CONTINUE", G3_TRIANGLE_STRIP, 4, strip, G3_TRIANGLE_STRIP_CONTINUE, 0},
        {"TRIANGLE_FAN + FAN_CONTINUE", G3_TRIANGLE_FAN, 4, fan, G3_TRIANGLE_FAN_CONTINUE, 0},
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (open_win(2, 1))
            return 1;
        layout_float();
        g3_grDitherMode(GR_DITHER_DISABLE);
        g3_grCullMode(GR_CULL_NEGATIVE);
        g3_grBufferClear(0, 0, 0);
        if (cases[i].pointers) {
            void *p[6];
            for (FxU32 k = 0; k < cases[i].count; k++)
                p[k] = &cases[i].v[k];
            g3_grDrawVertexArray(cases[i].mode, cases[i].count, p);
        } else if (cases[i].cont_mode) {
            g3_grDrawVertexArrayContiguous(cases[i].mode, 3, cases[i].v, sizeof(V3));
            g3_grDrawVertexArrayContiguous(cases[i].cont_mode, cases[i].count - 3, cases[i].v + 3, sizeof(V3));
        } else {
            g3_grDrawVertexArrayContiguous(cases[i].mode, cases[i].count, cases[i].v, sizeof(V3));
        }
        g3_grBufferSwap(1);
        if (i == 0) {
            /* reference: both triangles there (centre of each half) */
            if (g_last_frame[80 * W + 160] == 0 || g_last_frame[150 * W + 60] == 0) {
                fprintf(stderr, "TRIANGLES: quad not drawn completely\n");
                return 1;
            }
            keep_frame(g_frame_a);
        } else {
            fail |= same_frame(g_frame_a, cases[i].what);
        }
    }
    /* stride 0 in the contiguous call draws nothing */
    if (open_win(2, 1))
        return 1;
    layout_float();
    g3_grBufferClear(0, 0, 0);
    g3_grDrawVertexArrayContiguous(G3_TRIANGLES, 6, list, 0);
    g3_grBufferSwap(1);
    fail |= check_pixel(160, 80, 0, "grDrawVertexArrayContiguous with stride 0");
    if (!fail)
        printf("  ok: %d modes identical, strip winding kept under culling\n", (int) (sizeof cases / sizeof cases[0]));
    return fail;
}

/* grGlideGetState/SetState and Get/SetVertexLayout bring back the vertex
 * layout and the Glide 2 state. */
static int test_state(void) {
    printf("-- G3 test 8: state and vertex layout save/restore --\n");
    FxI32 size[2];
    if (open_win(2, 1))
        return 1;
    g3_grGet(G3_GLIDE_STATE_SIZE, 4, &size[0]);
    g3_grGet(G3_GLIDE_VERTEXLAYOUT_SIZE, 4, &size[1]);
    void *state = calloc(1, (size_t) size[0]), *layout = calloc(1, (size_t) size[1]);
    if (!state || !layout)
        return 1;
    int fail = 0;
    V3  a = v3(32, 8, 255, 0, 0, 255, 0, 1, 0, 0), b = v3(8, 56, 255, 0, 0, 255, 0, 1, 0, 0);
    V3  c = v3(56, 56, 255, 0, 0, 255, 0, 1, 0, 0);

    /* packed layout + green constant colour saved, then everything changed */
    layout_packed();
    g3_grConstantColorValue(0xff00ff00);
    g3_grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT,
                      GR_COMBINE_OTHER_NONE, FXFALSE);
    g3_grViewport(1, 2, 3, 4);
    g3_grGlideGetState(state);
    g3_grGlideGetVertexLayout(layout);
    g3_grVertexLayout(G3_PARAM_XY, OFS(r), G3_PARAM_ENABLE); /* garbage positions */
    g3_grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                      GR_COMBINE_OTHER_ITERATED, FXFALSE);
    g3_grViewport(0, 0, W, H);
    g3_grGlideSetState(state);
    g3_grBufferClear(0, 0xff, 0);
    g3_grDrawTriangle(&b, &a, &c);
    g3_grBufferSwap(1);
    fail |= check_pixel(32, 45, 0x07e0, "after grGlideSetState: saved layout and constant green");
    FxI32 vp[4];
    if (g3_grGet(G3_VIEWPORT, 16, vp) != 16 || vp[0] != 1 || vp[3] != 4) {
        fprintf(stderr, "grGlideSetState did not restore the viewport\n");
        fail = 1;
    }

    /* the layout alone */
    g3_grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                      GR_COMBINE_OTHER_ITERATED, FXFALSE);
    g3_grVertexLayout(G3_PARAM_XY, OFS(r), G3_PARAM_ENABLE);
    g3_grGlideSetVertexLayout(layout);
    g3_grBufferClear(0, 0xff, 0);
    g3_grDrawTriangle(&b, &a, &c);
    g3_grBufferSwap(1);
    fail |= check_pixel(32, 45, 0xf800, "after grGlideSetVertexLayout: red triangle in place");

    /* RGB after PARGB switches back to float colour (and the other way round) */
    a.argb = b.argb = c.argb = 0xff0000ff; /* packed: blue; float: still red */
    g3_grVertexLayout(G3_PARAM_RGB, OFS(r), G3_PARAM_ENABLE);
    g3_grBufferClear(0, 0xff, 0);
    g3_grDrawTriangle(&b, &a, &c);
    g3_grBufferSwap(1);
    fail |= check_pixel(32, 45, 0xf800, "RGB enabled after PARGB: float colour");
    g3_grVertexLayout(G3_PARAM_PARGB, OFS(argb), G3_PARAM_ENABLE);
    g3_grBufferClear(0, 0xff, 0);
    g3_grDrawTriangle(&b, &a, &c);
    g3_grBufferSwap(1);
    fail |= check_pixel(32, 45, 0x001f, "PARGB enabled after RGB: packed colour");
    free(state);
    free(layout);
    if (!fail)
        printf("  ok\n");
    return fail;
}

/* Table fog on Q: the same picture as the Glide 2 path (g2_* functions,
 * linked in as in glide3x.dll), and the amount the table says. */
static int fog_scene(int g3) {
    GrFog_t ft[GR_FOG_TABLE_SIZE];
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
        ft[i] = (GrFog_t) (i * 4);
    if (open_win(2, 1))
        return 1;
    layout_float();
    g3_grDitherMode(GR_DITHER_DISABLE);
    g3_grBufferClear(0, 0, 0);
    g3_grFogTable(ft);
    g3_grFogColorValue(0xffffffff);
    if (g3)
        g3_grFogMode(G3_FOG_WITH_TABLE_ON_Q);
    else
        g2_grFogMode(GR_FOG_WITH_TABLE);
    static const int idx[3] = {12, 30, 50};
    for (int k = 0; k < 3; k++) {
        float oow = 1.0f / g3_guFogTableIndexToW(idx[k]);
        float x0  = 10.0f + 30.0f * (float) k;
        V3    q[4];
        q[0] = v3(x0, 10, 0, 0, 0, 255, 0, oow, 0, 0);
        q[1] = v3(x0 + 20, 10, 0, 0, 0, 255, 0, oow, 0, 0);
        q[2] = v3(x0 + 20, 30, 0, 0, 0, 255, 0, oow, 0, 0);
        q[3] = v3(x0, 30, 0, 0, 0, 255, 0, oow, 0, 0);
        if (g3) {
            g3_grDrawVertexArrayContiguous(G3_POLYGON, 4, q, sizeof q[0]);
        } else {
            GrVertex g[4];
            memset(g, 0, sizeof g);
            for (int i = 0; i < 4; i++) {
                g[i].x   = q[i].x;
                g[i].y   = q[i].y;
                g[i].a   = 255;
                g[i].oow = oow;
            }
            g2_grDrawPolygonVertexList(4, g);
        }
    }
    g3_grBufferSwap(1);
    return 0;
}

static int test_fog(void) {
    printf("-- G3 test 9: table fog on Q --\n");
    if (fog_scene(0))
        return 1;
    keep_frame(g_frame_a);
    if (fog_scene(1))
        return 1;
    int fail = same_frame(g_frame_a, "FOG_WITH_TABLE_ON_Q against Glide 2 GR_FOG_WITH_TABLE");
    static const int idx[3] = {12, 30, 50};
    for (int k = 0; k < 3; k++) {
        int red = (g_last_frame[20 * W + 20 + 30 * k] >> 11) << 3;
        if (red < idx[k] * 4 - 10 || red > idx[k] * 4 + 10) {
            fprintf(stderr, "fog at table index %d: red %d, expected ~%d\n", idx[k], red, idx[k] * 4);
            fail = 1;
        }
    }
    if (!fail)
        printf("  ok: same as Glide 2, amounts from the table\n");
    return fail;
}

int main(void) {
    printf("glidulator Glide 3 test\n");
    setenv("GLIDULATOR_SPLASH", "off", 1);
    glide_state *gs = glide_init();
    if (!gs) {
        fprintf(stderr, "glide_init failed\n");
        return 1;
    }
    g3_grGlideInit();
    int fail = 0;
    fail |= test_flat_triangle();
    fail |= test_textured_triangle();
    fail |= test_zbuffer();
    fail |= test_mipmap_p8();
    fail |= test_combine_blend_cull();
    fail |= test_queries();
    fail |= test_vertex_arrays();
    fail |= test_state();
    fail |= test_fog();
    g3_grSstWinClose(1);
    g2_grGlideShutdown();
    glide_shutdown(gs);
    if (fail) {
        printf("FAILED\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
