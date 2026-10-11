/* Start-up show, two independent options:
 *
 * GLIDULATOR_SPLASH (logo, once per process, right after the first
 * grSstWinOpen, like 3dfx's grSstWinOpen):
 *   gl1       the Glidulator logo animation (default): moonlit sea,
 *             tentacle, wordmark
 *   off       nothing
 *   original  3dfx's own splash animation (spinning 3Dfx logo on the
 *             power shield), read from an original Glide 2.x GLIDE2X.OVL
 *             or glide2x.dll named in GLIDULATOR_SPLASH_FILE
 *             (core/splash_orig.c); falls back to gl1 if that file cannot
 *             be used.
 *   crt       older spelling of GLIDULATOR_CRT=1 without a logo.
 *
 * GLIDULATOR_CRT=1: analog monitor emulation on every switch to and from
 * the Voodoo (core/crt.c); the logo then starts with the sync-in.
 *
 * Glide is back in its initial state after the logo. The animations draw
 * through the normal Glide functions and output path (FSAA and the video
 * filter apply), with trace counting and frame capture muted. */

#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include "glide_internal.h"

enum { LOGO_OFF, LOGO_GL1, LOGO_ORIGINAL };

static int g_logo;
static int g_crt;
static int g_logo_done;
static int g_crt_first; /* sync-in on the next presented frame */

static int str_eq_nocase(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char c = *a >= 'A' && *a <= 'Z' ? (char) (*a - 'A' + 'a') : *a;
        if (c != *b)
            return 0;
    }
    return !*a && !*b;
}

/* Read at every grSstWinOpen. */
void glide_splash_config(void) {
    char cfg[16];
    g_logo = LOGO_GL1;
    g_crt  = 0;
    if (platform_get_config("GLIDULATOR_SPLASH", cfg, sizeof cfg)) {
        if (str_eq_nocase(cfg, "off") || cfg[0] == '0')
            g_logo = LOGO_OFF;
        else if (str_eq_nocase(cfg, "original"))
            g_logo = LOGO_ORIGINAL;
        else if (str_eq_nocase(cfg, "crt")) {
            g_logo = LOGO_OFF;
            g_crt  = 1;
        }
    }
    /* Without a clock (DOS without TSC, e.g. DOSBox) the animation would
     * run unpaced, frame by frame in software: skip the logo there. */
    if (!platform_time_us())
        g_logo = LOGO_OFF;
    if (platform_get_config("GLIDULATOR_CRT", cfg, sizeof cfg))
        g_crt = cfg[0] == '1' || str_eq_nocase(cfg, "on");
}

int glide_splash_crt(void) {
    return g_crt;
}

/* --- shared helpers ------------------------------------------------- */

/* Show the finished frame of an animation; t_us = time since its start
 * (drives the monitor sync-in). */
void glide_splash_present(uint64_t t_us) {
    glide_mt_idle();
    const uint16_t *fb = glide_fsaa_output();
    if (g_crt && t_us < GLIDE_CRT_IN_US)
        fb = glide_crt_apply(fb, (float) t_us / (float) GLIDE_CRT_IN_US);
    glide_blit_output(fb);
}

/* --- GL-1 logo ------------------------------------------------------
 * A night sea: the moon low above the water on the left, its reflection a
 * column of glittering streaks. In front of the moon a tentacle rises out
 * of the water, a silhouette with a moonlit rim, sways and sends out
 * rings of waves; its reflection wobbles below it. Above, the GLIDULATOR
 * wordmark in chrome with a glint running along it, and the motto.
 *
 * All 2D on a 640x480 virtual screen (with a margin, scaled to the Glide
 * resolution), drawn back to front without depth buffer or blending:
 * things fade in by mixing towards the colour behind them. */

#define GL1_US   4100000u
#define GL1_STEP 16667u

typedef struct {
    float sx, sy, ox, oy; /* virtual screen -> real screen */
} view;

static view  g_v;
static float g_flash; /* lightning 0..1 */
static float g_fade;  /* fade-in/out of the whole picture */
static float g_glint = -1000.0f; /* chrome glint position (virtual x) */

static unsigned mix(unsigned a, unsigned b, float t) {
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    unsigned r = 0;
    for (int s = 0; s < 24; s += 8) {
        float ca = (float) ((a >> s) & 255), cb = (float) ((b >> s) & 255);
        r |= (unsigned) (ca + (cb - ca) * t) << s;
    }
    return r;
}

static float ramp(float t, float a, float b) {
    float x = (t - a) / (b - a);
    x       = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return x * x * (3.0f - 2.0f * x); /* smoothstep */
}

#define SEA_Y   318.0f /* horizon */
#define MOON_X  176.0f
#define MOON_Y  254.0f
#define MOON_R  56.0f
#define TT_X    238.0f /* tentacle base, right of the moon, in its light */
#define TT_Y    (SEA_Y + 62.0f)
#define SKY_TOP 0x241040u
#define SKY_BOT 0x0a0418u

static unsigned sky(float y) {
    return mix(mix(SKY_TOP, SKY_BOT, (y + 40.0f) / (SEA_Y + 40.0f)), 0x9a88d0, g_flash * 0.6f);
}

/* Sky with the haze band above the horizon, as seascape() draws it. */
static unsigned behind(float y);

static unsigned water(float y) {
    return mix(mix(0x1a0e30, 0x030108, (y - SEA_Y) / 150.0f), 0x6a5ca0, g_flash * 0.4f);
}

/* Gouraud vertex on the virtual screen. */
static void vert2d(GrVertex *v, float x, float y, unsigned c) {
    for (size_t i = 0; i < sizeof *v; i++)
        ((uint8_t *) v)[i] = 0;
    v->x   = g_v.ox + g_v.sx * x;
    v->y   = g_v.oy + g_v.sy * y;
    v->oow = 1.0f;
    v->r   = (float) ((c >> 16) & 255) * g_fade;
    v->g   = (float) ((c >> 8) & 255) * g_fade;
    v->b   = (float) (c & 255) * g_fade;
    v->a   = 255.0f;
}

/* Slivers (tentacle tip) cover no pixel centre but give colour gradients
 * that overflow the rasterizer's setup: skip them. */
static void tri2d(const GrVertex *a, const GrVertex *b, const GrVertex *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area > -0.5f && area < 0.5f)
        return;
    grDrawTriangle(a, b, c);
}

static void quad2d(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, unsigned ca,
                   unsigned cb, unsigned cc, unsigned cd) {
    GrVertex a, b, c, d;
    vert2d(&a, ax, ay, ca);
    vert2d(&b, bx, by, cb);
    vert2d(&c, cx, cy, cc);
    vert2d(&d, dx, dy, cd);
    tri2d(&a, &b, &c);
    tri2d(&a, &c, &d);
}

/* Disc as a fan; drawn twice, the second fan turned by half a segment:
 * single pixels the rasterizer leaves open on a shared fan edge (seen in
 * the Linux test) are covered by the other fan. */
static void disc2d(float cx, float cy, float r, unsigned centre, unsigned edge, int twice) {
    for (int pass = 0; pass <= twice; pass++)
        for (int i = 0; i < 32; i++) {
            float    a0 = 6.2831853f * ((float) i + 0.5f * (float) pass) / 32, a1 = a0 + 6.2831853f / 32;
            GrVertex m, p, q;
            vert2d(&m, cx, cy, centre);
            vert2d(&p, cx + r * glide_cosf(a0), cy + r * glide_sinf(a0), edge);
            vert2d(&q, cx + r * glide_cosf(a1), cy + r * glide_sinf(a1), edge);
            tri2d(&m, &p, &q);
        }
}

/* Tentacle centre line and radius: rises (h 0..1) leaning towards the
 * moon, curls its tip back and sways. */
#define SEA_SEG 28
static void tentacle_line(float t, float h, float *px, float *py, float *pr) {
    float x = TT_X, y = TT_Y, len = 222.0f * h / SEA_SEG;
    float curl = 1.15f + 0.3f * glide_sinf(t * 1.6f);
    for (int i = 0; i <= SEA_SEG; i++) {
        float s = (float) i / SEA_SEG;
        px[i]   = x;
        py[i]   = y;
        pr[i]   = 16.0f * (1.0f - s) + 1.2f;
        float a = -1.5707963f - 0.42f + 0.22f * glide_sinf(t * 2.2f - s * 3.2f) + curl * s * s * 2.6f;
        x += glide_cosf(a) * len;
        y += glide_sinf(a) * len;
    }
}

/* Tube outline along the centre line; normals per point (averaged over
 * both neighbouring segments), so the quads share their edges and the
 * outside of a bend has no gaps. */
static void strip2d(const float *px, const float *py, const float *pr, float grow, unsigned c) {
    float nx[SEA_SEG + 1], ny[SEA_SEG + 1];
    for (int i = 0; i <= SEA_SEG; i++) {
        int   a = i > 0 ? i - 1 : 0, b = i < SEA_SEG ? i + 1 : SEA_SEG;
        float dx = px[b] - px[a], dy = py[b] - py[a], l = glide_sqrtf(dx * dx + dy * dy);
        nx[i] = l > 0.01f ? -dy / l : 0.0f;
        ny[i] = l > 0.01f ? dx / l : 0.0f;
    }
    for (int i = 0; i < SEA_SEG; i++) {
        float r0 = pr[i] + grow, r1 = pr[i + 1] + grow;
        quad2d(px[i] + nx[i] * r0, py[i] + ny[i] * r0, px[i + 1] + nx[i + 1] * r1, py[i + 1] + ny[i + 1] * r1,
               px[i + 1] - nx[i + 1] * r1, py[i + 1] - ny[i + 1] * r1, px[i] - nx[i] * r0, py[i] - ny[i] * r0, c, c,
               c, c);
    }
}

static unsigned haze(void) {
    return mix(0x34204e, 0x9a88d0, g_flash * 0.6f);
}

static unsigned behind(float y) {
    if (y >= SEA_Y)
        return water(y);
    if (y > SEA_Y - 50.0f)
        return mix(sky(SEA_Y - 50.0f), haze(), (y - (SEA_Y - 50.0f)) / 50.0f);
    return sky(y);
}

static void seascape(float t) {
    /* corners of the real screen in virtual coordinates */
    float    x0 = -g_v.ox / g_v.sx, y0 = -g_v.oy / g_v.sy;
    float    x1 = x0 + (float) g_width / g_v.sx, y1 = y0 + (float) g_height / g_v.sy;
    quad2d(x0, y0, x1, y0, x1, SEA_Y - 50, x0, SEA_Y - 50, sky(y0), sky(y0), sky(SEA_Y - 50), sky(SEA_Y - 50));
    quad2d(x0, SEA_Y - 50, x1, SEA_Y - 50, x1, SEA_Y, x0, SEA_Y, sky(SEA_Y - 50), sky(SEA_Y - 50), haze(), haze());
    quad2d(x0, SEA_Y, x1, SEA_Y, x1, y1, x0, y1, water(SEA_Y), water(SEA_Y), water(y1), water(y1));

    /* moon: a halo fading into the sky (cut at the horizon), the disc */
    for (int i = 0; i < 32; i++) {
        float    a0 = 6.2831853f * (float) i / 32, a1 = a0 + 6.2831853f / 32;
        float    r  = MOON_R * 2.8f;
        float    ya = MOON_Y + r * glide_sinf(a0), yb = MOON_Y + r * glide_sinf(a1);
        GrVertex m, p, q;
        if (ya > SEA_Y)
            ya = SEA_Y;
        if (yb > SEA_Y)
            yb = SEA_Y;
        vert2d(&m, MOON_X, MOON_Y, 0x5a6a50);
        vert2d(&p, MOON_X + r * glide_cosf(a0), ya, behind(ya - 0.01f));
        vert2d(&q, MOON_X + r * glide_cosf(a1), yb, behind(yb - 0.01f));
        tri2d(&m, &p, &q);
    }
    disc2d(MOON_X, MOON_Y, MOON_R, 0xeef6cc, 0xb0c090, 1);

    /* reflection: streaks get wider, thicker and dimmer towards the
     * viewer, each shimmering on its own */
    for (int i = 0; i < 34; i++) {
        float    f  = (float) i / 34;
        float    y  = SEA_Y + 2.0f + 168.0f * f * glide_sqrtf(f);
        float    w  = (18.0f + 92.0f * f) * (0.5f + 0.5f * glide_sinf(t * 4.3f + (float) i * 1.7f));
        float    cx = MOON_X + 7.0f * glide_sinf(t * 2.3f + (float) i * 0.9f);
        float    th = 1.0f + 3.4f * f;
        float    a  = (1.0f - 0.45f * f) * (0.7f + 0.3f * glide_sinf(t * 7.1f + (float) i * 2.3f));
        unsigned c  = mix(water(y), 0xd8e8b0, a);
        quad2d(cx - w, y, cx + w, y, cx + w * 0.8f, y + th, cx - w * 0.8f, y + th, water(y), c, c, water(y));
        quad2d(cx - w * 0.3f, y, cx + w * 0.3f, y, cx + w * 0.3f, y + th, cx - w * 0.3f, y + th, c, c, c, c);
    }

    float h = ramp(t, 0.3f, 1.5f);
    if (h <= 0.0f)
        return;
    float px[SEA_SEG + 1], py[SEA_SEG + 1], pr[SEA_SEG + 1];
    tentacle_line(t, h, px, py, pr);

    /* its reflection: mirrored at the water line, shortened, wobbling */
    float rx[SEA_SEG + 1], ry[SEA_SEG + 1], rr[SEA_SEG + 1];
    for (int i = 0; i <= SEA_SEG; i++) {
        ry[i] = TT_Y + (TT_Y - py[i]) * 0.55f;
        rx[i] = px[i] + 4.0f * glide_sinf(ry[i] * 0.25f + t * 6.0f);
        rr[i] = pr[i] * 0.9f;
    }
    strip2d(rx, ry, rr, 0.0f, mix(water(TT_Y), 0x000000, 0.65f));

    /* rings of waves, flattened by perspective */
    for (int j = 0; j < 7; j++) {
        float age = t - (0.5f + 0.5f * (float) j);
        if (age <= 0.0f || age > 2.0f)
            continue;
        float    r = 20.0f + 80.0f * age, fade = 1.0f - age / 2.0f;
        unsigned c = mix(water(TT_Y), 0xb0c0a0, 0.7f * fade);
        for (int i = 0; i < 40; i++) {
            float a0 = 6.2831853f * (float) i / 40, a1 = a0 + 6.2831853f / 40;
            float c0 = glide_cosf(a0), s0 = glide_sinf(a0) * 0.2f, c1 = glide_cosf(a1), s1 = glide_sinf(a1) * 0.2f;
            float d  = 2.4f;
            quad2d(TT_X + r * c0, TT_Y + r * s0, TT_X + r * c1, TT_Y + r * s1, TT_X + (r + d) * c1,
                   TT_Y + (r + d) * s1 + 0.8f, TT_X + (r + d) * c0, TT_Y + (r + d) * s0 + 0.8f, c, c, c, c);
        }
    }

    /* the tentacle: moonlit rim, then the dark body over it */
    strip2d(px, py, pr, 2.2f, mix(0x7a6cae, 0xe0f0b8, g_flash));
    strip2d(px, py, pr, 0.0f, 0x0c0614);
}

#define WORD_PX 7.5f
#define WORD_X  (320.0f - (60.0f * WORD_PX - WORD_PX) * 0.5f)
#define WORD_Y  50.0f

/* Colour of a text vertex: mixed from the colour behind it (a = 0) to the
 * text colour, plus the chrome glint, a diagonal band at g_glint. */
static void text_rgb(GrVertex *v, unsigned c, float a, float vx, float vy, int glint) {
    unsigned bg = behind(vy);
    if (glint) {
        float d = (vx + (vy - WORD_Y) * 0.55f - g_glint) / 42.0f;
        if (d < 0.0f)
            d = -d;
        if (d < 1.0f)
            c = mix(c, 0xffffff, (1.0f - d) * (1.0f - d));
    }
    c    = mix(bg, c, a);
    v->r = (float) ((c >> 16) & 255) * g_fade;
    v->g = (float) ((c >> 8) & 255) * g_fade;
    v->b = (float) (c & 255) * g_fade;
}

/* Text with the OSD font: font pixels as quads, sheared for italics,
 * colour by glyph row. */
static void text2d(const char *s, float x, float y, float px, float shear, const unsigned *row_rgb, int count,
                   float alpha, int glint) {
    if (alpha <= 0.0f)
        return;
    for (int n = 0; *s && n < count; s++, n++, x += 6 * px) {
        const uint8_t *g = glide_osd_glyph(*s);
        if (!g)
            continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col)) {
                    float    x0 = x + (float) col * px + (float) (6 - row) * px * shear, y0 = y + (float) row * px;
                    float    xs[4], ys[4]; /* no non-constant initialisers (Watcom) */
                    GrVertex q[4];
                    xs[0] = x0;
                    xs[1] = x0 + px;
                    xs[2] = x0 + px - px * shear;
                    xs[3] = x0 - px * shear;
                    ys[0] = ys[1] = y0;
                    ys[2] = ys[3] = y0 + px;
                    for (int k = 0; k < 4; k++) {
                        vert2d(&q[k], xs[k], ys[k], 0);
                        text_rgb(&q[k], row_rgb[row], alpha, xs[k], ys[k], glint);
                    }
                    tri2d(&q[0], &q[1], &q[2]);
                    tri2d(&q[0], &q[2], &q[3]);
                }
    }
}

/* Lightning: two quick flashes as the tentacle breaks the surface, an
 * echo later. */
static float lightning(float t) {
    static const float at[3] = { 0.95f, 1.12f, 3.0f }, amp[3] = { 1.0f, 0.7f, 0.45f };
    float f = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = t - at[i];
        if (d >= 0.0f && d < 0.18f)
            f += amp[i] * (1.0f - d / 0.18f) * (1.0f - d / 0.18f);
    }
    return f > 1.0f ? 1.0f : f;
}

static void gl1_frame(float t) {
    float out = 1.0f - ramp(t, 3.6f, 4.1f);
    g_fade    = ramp(t, 0.0f, 0.7f) * out;
    g_flash   = lightning(t) * out;
    seascape(t);

    static const unsigned chrome[7] = { 0xd4cee6, 0xb4acce, 0x8e84b0, 0x342850, 0x5c4e7e, 0x948ab4, 0xb0a6ca };
    static const unsigned shadow[7] = { 0x12081e, 0x12081e, 0x12081e, 0x12081e, 0x12081e, 0x12081e, 0x12081e };
    static const unsigned green[7]  = { 0xb6f05a, 0xa6e04c, 0x94d040, 0x94d040, 0x84c036, 0x74ac2c, 0x649a24 };
    static const unsigned bar[7]    = { 0x7a2aa8, 0x7a2aa8, 0x7a2aa8, 0x7a2aa8, 0x7a2aa8, 0x7a2aa8, 0x7a2aa8 };
    float word = ramp(t, 1.1f, 1.8f);
    if (word > 0.0f) {
        float dy = (1.0f - word) * -30.0f;
        g_glint  = -200.0f + 1000.0f * ramp(t, 2.2f, 3.3f);
        text2d("GLIDULATOR", WORD_X + 5.0f, WORD_Y + 5.0f + dy, WORD_PX, 0.22f, shadow, 10, word, 0);
        text2d("GLIDULATOR", WORD_X, WORD_Y + dy, WORD_PX, 0.22f, chrome, 10, word, 1);
    }
    float b = ramp(t, 1.7f, 2.2f);
    if (b > 0.0f) {
        float    w = 440.0f * b, x = 320.0f - w * 0.5f, y = WORD_Y + 7.0f * WORD_PX + 12.0f;
        GrVertex q[4];
        vert2d(&q[0], x, y, 0);
        vert2d(&q[1], x + w, y, 0);
        vert2d(&q[2], x + w, y + 3, 0);
        vert2d(&q[3], x, y + 3, 0);
        for (int k = 0; k < 4; k++)
            text_rgb(&q[k], bar[0], 1.0f, k == 1 || k == 2 ? x + w : x, y, 0);
        tri2d(&q[0], &q[1], &q[2]);
        tri2d(&q[0], &q[2], &q[3]);
    }
    int letters = (int) (ramp(t, 1.9f, 2.7f) * 22.0f + 0.5f);
    text2d("EVERY CORE A TENTACLE.", 320.0f - (132.0f * 3.0f - 3.0f) * 0.5f, WORD_Y + 7.0f * WORD_PX + 24.0f, 3.0f,
           0.15f, green, letters, 1.0f, 0);
}

static void gl1_play(void) {
    /* 640x480 layout with a margin */
    float vs = (float) g_width / 700.0f, hs = (float) g_height / 525.0f;
    float s  = vs < hs ? vs : hs;
    g_v.sx = g_v.sy = s;
    g_v.ox = ((float) g_width - 640.0f * s) * 0.5f;
    g_v.oy = ((float) g_height - 480.0f * s) * 0.5f;

    glide_set_origin(GR_ORIGIN_UPPER_LEFT);
    grCullMode(GR_CULL_DISABLE);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    grDitherMode(GR_DITHER_4x4);

    uint64_t t0 = platform_time_us();
    for (uint64_t t = 0; t < GL1_US; t += GL1_STEP) {
        gl1_frame((float) t / 1000000.0f);
        glide_splash_present(t);
        glide_wait_until(t0, t + GL1_STEP);
    }
}

/* --- entry points --------------------------------------------------- */

/* End of grSstWinOpen (display open, Glide in its initial state).
 * Returns 1 if an animation drew, then the caller restores the initial
 * state. */
int glide_splash_winopen(void) {
    if (g_logo == LOGO_OFF || g_logo_done) {
        g_crt_first = g_crt;
        return 0;
    }
    g_logo_done = 1;
    g_glide_mute++;
    int ok = g_logo == LOGO_ORIGINAL && glide_splash_original();
    if (!ok)
        gl1_play();
    g_glide_mute--;
    return 1;
}

/* glide_present(): sync-in on the game's first frame (no logo, or every
 * later grSstWinOpen). */
void glide_splash_first_frame(const uint16_t *fb) {
    if (!g_crt_first)
        return;
    g_crt_first = 0;
    glide_crt_sync_in(fb);
}

/* grSstWinClose, before the display switches back: the monitor loses
 * sync on the last image -- unless the program has already left the mode
 * itself (DOS demos that print their exit text before grSstWinClose). */
void glide_splash_winclose(void) {
    g_crt_first = 0;
    if (!g_crt)
        return;
    if (platform_display_lost()) {
        platform_log("Splash: display already switched back, no closing effect\n");
        return;
    }
    glide_mt_idle();
    glide_crt_sync_out(glide_fsaa_output());
}
