/* Screenshot: when the platform reports its screenshot key (DOS OVL: Print
 * Screen, platform_screenshot_key()), the frame of the next grBufferSwap
 * is saved as SHOTnnnn.BMP in the current directory: 24-bit BMP in the
 * game's render resolution, with the output gamma, without the OSD
 * (GLIDULATOR_SCREENSHOT=2: with it, core/osd.c), scaling or the video
 * filter. */

#include "glidulator/platform.h"
#include "glidulator/trace.h"
#include "glide_internal.h"

static int g_shot_next; /* first number that may still be free */

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v & 0xffffu);
    put16(p + 2, v >> 16);
}

size_t glide_screenshot_bmp(const uint16_t *fb, int w, int h, const uint8_t *lut, uint8_t *out) {
    size_t row  = ((size_t) w * 3 + 3) & ~(size_t) 3;
    size_t size = 54 + row * (size_t) h;
    if (!out)
        return size;
    for (size_t i = 0; i < 54; i++)
        out[i] = 0;
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, (uint32_t) size);
    put32(out + 10, 54);              /* pixel data offset */
    put32(out + 14, 40);              /* BITMAPINFOHEADER */
    put32(out + 18, (uint32_t) w);
    put32(out + 22, (uint32_t) h);    /* positive: bottom-up */
    put16(out + 26, 1);               /* planes */
    put16(out + 28, 24);              /* bits per pixel */
    put32(out + 34, (uint32_t) (row * (size_t) h));
    put32(out + 38, 2835);            /* 72 dpi */
    put32(out + 42, 2835);
    for (int y = 0; y < h; y++) {
        const uint16_t *src = fb + (size_t) (h - 1 - y) * (size_t) w;
        uint8_t        *dst = out + 54 + (size_t) y * row;
        for (int x = 0; x < w; x++) {
            unsigned p = src[x];
            unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            r          = (r << 3) | (r >> 2);
            g          = (g << 2) | (g >> 4);
            b          = (b << 3) | (b >> 2);
            *dst++     = lut ? lut[b] : (uint8_t) b;
            *dst++     = lut ? lut[g] : (uint8_t) g;
            *dst++     = lut ? lut[r] : (uint8_t) r;
        }
        for (size_t x = (size_t) w * 3; x < row; x++)
            *dst++ = 0;
    }
    return size;
}

/* SHOTnnnn.BMP: the first number from g_shot_next on without a file */
static int free_name(char name[13]) {
    for (; g_shot_next <= 9999; g_shot_next++) {
        int  n = g_shot_next;
        char c;
        name[0] = 'S', name[1] = 'H', name[2] = 'O', name[3] = 'T';
        for (int i = 7; i >= 4; i--, n /= 10)
            name[i] = (char) ('0' + n % 10);
        name[8] = '.', name[9] = 'B', name[10] = 'M', name[11] = 'P', name[12] = 0;
        if (platform_read_file(name, 0, &c, 1) < 0)
            return 1;
    }
    return 0;
}

void glide_screenshot_frame(const uint16_t *fb) {
    char name[13];
    if (!platform_screenshot_key() || !fb || g_width <= 0 || g_height <= 0)
        return;
    int      ok   = 0;
    size_t   size = glide_screenshot_bmp(fb, g_width, g_height, NULL, NULL);
    uint8_t *buf  = free_name(name) ? (uint8_t *) platform_alloc(size) : NULL;
    if (buf) {
        glide_screenshot_bmp(fb, g_width, g_height, glide_gamma_lut(), buf);
        ok = platform_write_file(name, buf, size) == 0;
        platform_free(buf);
        if (ok)
            g_shot_next++;
    }
#ifdef GLIDULATOR_TRACE
    glide_trace_note(ok ? "Screenshot written, number/width" : "Screenshot FAILED, number/width",
                     (unsigned long) g_shot_next - (ok ? 1 : 0), (unsigned long) g_width);
#endif
}
