/* DOS smoke test: builds core/ + platform/dos with DJGPP and runs on DOS
 * (or DOSBox) with the same pixel checks as targets/glide_test_linux, with
 * text output instead of PNG files (no libpng for DOS). */

#include "glidulator/glide2x.h"
#include "glidulator/glide_state.h"
#include "glidulator/platform.h"

#include <stdio.h>

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
    (void) stride_bytes; /* the video filter is not tested here */
}

static int check_pixel(int x, int y, uint16_t expected, const char *what) {
    uint16_t got = g_last_frame[y * g_last_w + x];
    if (got != expected) {
        printf("ERROR pixel (%d,%d) %s: expected %04x, got %04x\n", x, y, what, expected, got);
        return 1;
    }
    return 0;
}

#define W 320
#define H 240

int main(void) {
    printf("glidulator DOS smoke test\n");
    printf("  core_count = %d\n", platform_core_count());

    glide_state *gs = glide_init();
    if (!gs) {
        printf("glide_init failed\n");
        return 1;
    }

    if (!grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                       GR_ORIGIN_UPPER_LEFT, 1, 0)) {
        printf("grSstWinOpen failed\n");
        return 1;
    }

    grBufferClear(0x00000000, 0xff, 0);

    GrVertex v0 = { 32.0f, 8.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v1 = { 8.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v2 = { 56.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    grDrawTriangle(&v1, &v0, &v2);

    grBufferSwap(1);

    if (!g_last_frame) {
        printf("grBufferSwap did not call platform_blit\n");
        return 1;
    }

    int fail = 0;
    fail |= check_pixel(32, 45, 0xf800, "triangle centre (red, RGB565)");
    fail |= check_pixel(2, 2, 0x0000, "top left (outside)");
    fail |= check_pixel(W - 2, 2, 0x0000, "top right (outside)");
    fail |= check_pixel(2, H - 2, 0x0000, "bottom left (outside)");

    if (fail) {
        printf("triangle rendering FAILED\n");
        return 1;
    }

    grGlideShutdown();
    glide_shutdown(gs);

    printf("ok\n");
    return 0;
}
