/* Analog monitor emulation for the start-up options (GLIDULATOR_SPLASH,
 * core/splash.c): when the display switches to the Voodoo, a CRT loses
 * sync and locks onto the new signal. The picture rolls vertically with
 * the blanking bar visible, lines are pulled sideways by the unsettled
 * horizontal sync, brightness overshoots and settles, and a little static
 * shows. Switching back plays the same in reverse.
 *
 * Everything works on the finished RGB565 image right before output, so
 * the effect needs nothing from the game. Portable integer/float code
 * without the C runtime (OVL). */

#include "glidulator/platform.h"

#include "glide_internal.h"

static uint16_t *g_crt_buf;
static int       g_crt_w, g_crt_h;
static uint32_t  g_rand = 0x2545f491u;

/* sin over the full float range, no libm (OVL): reduce to [-pi, pi] and
 * use a 7th-order polynomial; plenty for wobbles and fades. */
float glide_sinf(float x) {
    const float pi = 3.14159265f, two_pi = 6.28318531f;
    int         k  = (int) (x / two_pi);
    x -= (float) k * two_pi;
    if (x > pi)
        x -= two_pi;
    if (x < -pi)
        x += two_pi;
    /* fold into [-pi/2, pi/2] where the polynomial is accurate */
    if (x > pi / 2)
        x = pi - x;
    if (x < -pi / 2)
        x = -pi - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

float glide_cosf(float x) {
    return glide_sinf(x + 1.57079633f);
}

float glide_sqrtf(float x) {
    if (x <= 0.0f)
        return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    for (int i = 0; i < 20; i++)
        r = 0.5f * (r + x / r);
    return r;
}

static uint32_t rnd(void) {
    g_rand = g_rand * 1664525u + 1013904223u;
    return g_rand >> 8;
}

int glide_crt_open(int width, int height) {
    g_crt_buf = (uint16_t *) platform_alloc((size_t) width * (size_t) height * 2);
    g_crt_w   = width;
    g_crt_h   = height;
    return g_crt_buf != 0;
}

void glide_crt_close(void) {
    platform_free(g_crt_buf);
    g_crt_buf = 0;
}

/* p = 0: signal just switched, 1: locked. Returns the distorted image
 * (internal buffer) or src when p >= 1. */
const uint16_t *glide_crt_apply(const uint16_t *src, float p) {
    if (!g_crt_buf || p >= 1.0f)
        return src;
    if (p < 0.0f)
        p = 0.0f;
    int   w = g_crt_w, h = g_crt_h;
    float e = 1.0f - p; /* remaining disturbance */
    float e2 = e * e;
    /* Vertical hold: the picture rolls downwards and slows to a stop; the
     * blanking interval between two fields shows as a black bar. */
    int   bar   = h / 10;
    int   cycle = h + bar;
    int   roll  = (int) (e2 * e * 2.6f * (float) cycle);
    /* Brightness: dark at first, overshoot around p = 0.35, then 1. */
    float bright = p < 0.35f ? 0.25f + 0.95f * (p / 0.35f) : 1.2f - 0.2f * ((p - 0.35f) / 0.65f);
    int   gain   = (int) (bright * 256.0f);
    int   noise  = (int) (60.0f * e2);
    float amp    = 0.06f * (float) w * e2;
    float phase  = p * 37.0f;
    for (int y = 0; y < h; y++) {
        uint16_t *dst = g_crt_buf + (size_t) y * (size_t) w;
        int       sy  = (y + cycle - roll % cycle) % cycle;
        if (sy >= h) { /* blanking bar */
            for (int x = 0; x < w; x++)
                dst[x] = 0;
            continue;
        }
        /* horizontal sync: a slow bend plus line-to-line jitter */
        int dx = (int) (amp * glide_sinf((float) sy * 0.021f + phase)) +
                 (noise ? (int) (rnd() % (unsigned) (noise / 6 + 1)) - noise / 12 : 0);
        const uint16_t *row = src + (size_t) sy * (size_t) w;
        for (int x = 0; x < w; x++) {
            int sx = x - dx;
            if (sx < 0 || sx >= w) {
                dst[x] = 0;
                continue;
            }
            uint16_t c = row[sx];
            int      r = ((c >> 11) & 31) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
            int      n = noise ? (int) (rnd() % (unsigned) (noise + 1)) - noise / 3 : 0;
            r = ((r * gain) >> 8) + n;
            g = ((g * gain) >> 8) + n;
            b = ((b * gain) >> 8) + n;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            dst[x] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    return g_crt_buf;
}

/* Wait until t_us after start (the output loops of the effects run at a
 * fixed rate independent of vsync support). Without a clock
 * (platform_time_us() stays 0: DOS without TSC, e.g. DOSBox's 486) the
 * loops run unpaced instead of hanging. */
void glide_wait_until(uint64_t start, uint64_t t_us) {
    uint64_t now = platform_time_us();
    if (!now)
        return;
    while (now - start < t_us)
        now = platform_time_us();
}

#define CRT_OUT_US 350000u
#define CRT_STEP_US 16667u

/* Blocking sync-in on one image (the game's first frame). */
void glide_crt_sync_in(const uint16_t *fb) {
    uint64_t t0 = platform_time_us();
    for (uint64_t t = 0; t < GLIDE_CRT_IN_US; t += CRT_STEP_US) {
        glide_blit_output(glide_crt_apply(fb, (float) t / (float) GLIDE_CRT_IN_US));
        glide_wait_until(t0, t + CRT_STEP_US);
    }
}

/* Blocking loss of sync on the last image before the display switches
 * back. */
void glide_crt_sync_out(const uint16_t *fb) {
    uint64_t t0 = platform_time_us();
    for (uint64_t t = 0; t < CRT_OUT_US; t += CRT_STEP_US) {
        float p = 1.0f - (float) t / (float) CRT_OUT_US;
        glide_blit_output(glide_crt_apply(fb, p));
        glide_wait_until(t0, t + CRT_STEP_US);
    }
}
