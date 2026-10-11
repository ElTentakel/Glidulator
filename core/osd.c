/* Frame output with timing and the optional on-screen display
 * (GLIDULATOR_OSD=1): frames per second, game+render and output time per
 * frame, render path, threads, Glide and screen resolution, active image
 * options. Portable (also in the OVL, no C runtime): the text is drawn
 * straight into the RGB565 image and the overwritten area is restored
 * after output, so the game never sees it (LFB reads, frames without
 * clear). */

#include "glidulator/platform.h"
#include "glidulator/renderer.h"

#include "glide_internal.h"

static const char g_osd_chars[] = "-./0123456789:>ABCDEFGHIJKLMNOPQRSTUVWXYZx+*";
/* 7 rows per character, bit 4 = left column */
static const uint8_t g_osd_font[][7] = {
    { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 }, /* - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c }, /* . */
    { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 }, /* / */
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, /* 0 */
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* 1 */
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, /* 2 */
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e }, /* 3 */
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, /* 4 */
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, /* 6 */
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, /* 8 */
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, /* 9 */
    { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 }, /* : */
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, /* > */
    { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* A */
    { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e }, /* B */
    { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }, /* C */
    { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e }, /* D */
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }, /* E */
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 }, /* F */
    { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }, /* G */
    { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* H */
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* I */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c }, /* J */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f }, /* L */
    { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, /* N */
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* O */
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 }, /* P */
    { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d }, /* Q */
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 }, /* R */
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e }, /* S */
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 }, /* V */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a }, /* W */
    { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 }, /* X */
    { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 }, /* Y */
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f }, /* Z */
    { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11 }, /* x */
    { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 }, /* + */
    { 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00 }, /* * = separator dot */
};

/* All sizes in font pixels; one font pixel is g_osd_s x g_osd_s screen
 * pixels of the Glide picture (see glide_osd_open). */
#define OSD_X       (4 * g_osd_s)
#define OSD_Y       (4 * g_osd_s)
#define OSD_PAD     (4 * g_osd_s)
#define OSD_LINE_H  (9 * g_osd_s) /* 7 pixel font + 2 spacing */
#define OSD_LINES   3 /* at most; the third only with image options */
#define OSD_CHARS   64
#define OSD_H       (OSD_LINES * OSD_LINE_H + 2 * OSD_PAD - 2 * g_osd_s)
#define OSD_UPDATE_US 500000u /* refresh values twice per second */

/* colour switches inside the text (take no space) */
#define OSD_LABEL '\1' /* grey: label */
#define OSD_VALUE '\2' /* white: value */
#define OSD_GRAY  0xa514
#define OSD_WHITE 0xffff
#define OSD_FRAME 0x6b4d

static int       g_osd_on;
static int       g_osd_s = 1; /* scale */
static uint16_t *g_osd_save; /* saved area, g_width x OSD_H */
static char      g_osd_text[OSD_LINES][OSD_CHARS + 1];
static int       g_osd_lines; /* lines in use */

/* Timing: end of the last output, sums since the last refresh. */
static uint64_t g_last_end, g_period_start, g_sum_render, g_sum_out;
static unsigned g_frames;

static char *put_str(char *p, char *end, const char *s) {
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

static char *put_uint(char *p, char *end, unsigned v) {
    char tmp[12];
    int  n = 0;
    do
        tmp[n++] = (char) ('0' + v % 10);
    while ((v /= 10) && n < 11);
    while (n && p < end)
        *p++ = tmp[--n];
    return p;
}

/* v in tenths -> "12.3" */
static char *put_tenths(char *p, char *end, unsigned v) {
    p = put_uint(p, end, v / 10);
    p = put_str(p, end, ".");
    return put_uint(p, end, v % 10);
}

/* Like put_tenths, right-aligned to width characters (values do not jump) */
static char *put_tenths_w(char *p, char *end, unsigned v, int width) {
    char  tmp[16];
    char *q = put_tenths(tmp, tmp + sizeof tmp, v);
    for (int k = (int) (q - tmp); k < width && p < end; k++)
        *p++ = ' ';
    for (char *t = tmp; t < q && p < end; t++)
        *p++ = *t;
    return p;
}

static char *put_sep(char *p, char *end) {
    char sep[5] = { ' ', OSD_LABEL, '*', ' ', 0 };
    return put_str(p, end, sep);
}

static char *put_label(char *p, char *end, const char *s) {
    if (p < end)
        *p++ = OSD_LABEL;
    return put_str(p, end, s);
}

static char *put_value(char *p, char *end) {
    if (p < end)
        *p++ = OSD_VALUE;
    return p;
}

static void set_lines(unsigned fps10, unsigned render10, unsigned out10) {
    char *p = g_osd_text[0], *end = p + OSD_CHARS;
    int   dw, dh;
    /* 1: speed */
    p  = put_value(p, end);
    p  = put_tenths_w(p, end, fps10, 5);
    p  = put_label(p, end, " FPS");
    p  = put_sep(p, end);
    p  = put_label(p, end, "RENDER ");
    p  = put_value(p, end);
    p  = put_tenths_w(p, end, render10, 5);
    p  = put_sep(p, end);
    p  = put_label(p, end, "OUTPUT ");
    p  = put_value(p, end);
    p  = put_tenths_w(p, end, out10, 4);
    p  = put_label(p, end, " MS");
    *p = 0;

    /* 2: path, threads, resolution */
    p   = g_osd_text[1];
    end = p + OSD_CHARS;
    p   = put_value(p, end);
    p   = put_str(p, end, glide_renderer_name(glide_renderer_active()));
    p   = put_sep(p, end);
    p   = put_value(p, end);
    p   = put_uint(p, end, (unsigned) glide_renderer_threads());
    p   = put_label(p, end, glide_renderer_threads() == 1 ? " THREAD" : " THREADS");
    p   = put_sep(p, end);
    p   = put_value(p, end);
    p   = put_uint(p, end, (unsigned) g_width);
    p   = put_str(p, end, "x");
    p   = put_uint(p, end, (unsigned) g_height);
    if (platform_display_size(&dw, &dh) && (dw != g_width || dh != g_height)) {
        p = put_label(p, end, " > ");
        p = put_value(p, end);
        p = put_uint(p, end, (unsigned) dw);
        p = put_str(p, end, "x");
        p = put_uint(p, end, (unsigned) dh);
    }
    *p = 0;

    /* 3: image options, only if one is on */
    p   = g_osd_text[2];
    end = p + OSD_CHARS;
    *p  = 0;
    if (g_fsaa_n > 1 || glide_fsaa_auto()) {
        p = put_label(p, end, "FSAA ");
        p = put_value(p, end);
        if (glide_fsaa_auto())
            p = put_str(p, end, g_fsaa_n > 1 ? "AUTO " : "AUTO OFF");
        if (g_fsaa_n > 1) {
            p = put_uint(p, end, (unsigned) g_fsaa_n);
            p = put_str(p, end, "X");
        }
    }
    if (glide_aa_post_enabled()) {
        if (p != g_osd_text[2])
            p = put_sep(p, end);
        p = put_value(p, end);
        p = put_str(p, end, "POST-AA");
    }
    if (glide_vfilter_enabled()) {
        if (p != g_osd_text[2])
            p = put_sep(p, end);
        p = put_value(p, end);
        p = put_str(p, end, "VOODOO-FILTER");
    }
    *p          = 0;
    g_osd_lines = g_osd_text[2][0] ? 3 : 2;
}

const uint8_t *glide_osd_glyph(char c) {
    if (c >= 'a' && c <= 'z' && c != 'x')
        c = (char) (c - 'a' + 'A');
    for (int i = 0; g_osd_chars[i]; i++)
        if (g_osd_chars[i] == c)
            return g_osd_font[i];
    return 0; /* blank and unknown characters */
}

/* Width in pixels (colour switches do not count) */
static int text_width(void) {
    int n = 0;
    for (int l = 0; l < g_osd_lines; l++) {
        int k = 0;
        for (const char *c = g_osd_text[l]; *c; c++)
            if (*c != OSD_LABEL && *c != OSD_VALUE)
                k++;
        if (k > n)
            n = k;
    }
    return (n * 6 - 1) * g_osd_s + 2 * OSD_PAD;
}

static int box_height(void) {
    return g_osd_lines * OSD_LINE_H + 2 * OSD_PAD - 2 * g_osd_s;
}

/* Box: background darkened by half, thin frame, labels grey, values
 * white. */
static void draw(uint16_t *fb, int w) {
    int h = box_height();
    for (int y = 0; y < h; y++) {
        uint16_t *row = fb + (size_t) (OSD_Y + y) * (size_t) g_width + OSD_X;
        for (int x = 0; x < w; x++)
            row[x] = (y < g_osd_s || y >= h - g_osd_s || x < g_osd_s || x >= w - g_osd_s) ? OSD_FRAME
                                                                      : (uint16_t) ((row[x] >> 1) & 0x7bef);
    }
    for (int l = 0; l < g_osd_lines; l++) {
        uint16_t color = OSD_WHITE;
        int      col   = 0;
        for (const char *c = g_osd_text[l]; *c; c++) {
            if (*c == OSD_LABEL || *c == OSD_VALUE) {
                color = *c == OSD_LABEL ? OSD_GRAY : OSD_WHITE;
                continue;
            }
            const uint8_t *g  = glide_osd_glyph(*c);
            int            x0 = OSD_X + OSD_PAD + col * 6 * g_osd_s;
            col++;
            if (!g || x0 + 5 * g_osd_s > OSD_X + w)
                continue;
            for (int y = 0; y < 7 * g_osd_s; y++) {
                uint16_t *row = fb + (size_t) (OSD_Y + OSD_PAD + l * OSD_LINE_H + y) * (size_t) g_width + x0;
                for (int x = 0; x < 5 * g_osd_s; x++)
                    if (g[y / g_osd_s] & (0x10 >> (x / g_osd_s)))
                        row[x] = color;
            }
        }
    }
}

void glide_osd_open(void) {
    char cfg[8];
    g_osd_on = platform_get_config("GLIDULATOR_OSD", cfg, sizeof cfg) && cfg[0] == '1';
    platform_free(g_osd_save);
    g_osd_save = 0;
    /* A screen mode smaller than the Glide picture (DOS mode 13h: 640x480
     * on 320x200) shrinks the OSD with it until it is unreadable: scale
     * it by the rounded shrink factor. */
    int dw, dh;
    g_osd_s = 1;
    if (platform_display_size(&dw, &dh) && dw > 0 && dh > 0) {
        int fx = (g_width * 2 + dw) / (2 * dw), fy = (g_height * 2 + dh) / (2 * dh);
        g_osd_s = fx > fy ? fx : fy;
        if (g_osd_s < 1)
            g_osd_s = 1;
    }
    if (g_osd_on && g_height >= OSD_Y + OSD_H && g_width > OSD_X + 16)
        g_osd_save = (uint16_t *) platform_alloc((size_t) g_width * OSD_H * 2);
    g_last_end = g_period_start = platform_time_us();
    g_sum_render = g_sum_out = 0;
    g_frames                 = 0;
    set_lines(0, 0, 0);
}

void glide_osd_close(void) {
    platform_free(g_osd_save);
    g_osd_save = 0;
    g_osd_on   = 0;
}

#ifdef GLIDULATOR_TRACE
uint64_t g_perf_tex_us, g_perf_lfb_us;

/* Trace variants: one line per 100 frames into GLIDULAT.LOG on every
 * platform (the Windows DLL also writes its output details). */
static void trace_perf(uint64_t now, uint64_t render, uint64_t wait, uint64_t out) {
    static uint64_t start, sum_render, sum_wait, sum_out;
    static unsigned frames;
    char            line[128];
    char           *p = line, *end = line + sizeof line - 2;
    if (!frames)
        start = now;
    sum_render += render;
    sum_wait += wait;
    sum_out += out;
    if (++frames < 100)
        return;
    uint64_t dt = now - start;
    p           = put_str(p, end, "Frame: ");
    p           = put_tenths(p, end, dt ? (unsigned) (1000000000ull / dt) : 0);
    p           = put_str(p, end, " fps, game+render ");
    p           = put_uint(p, end, (unsigned) (sum_render / 100));
    if (sum_wait / 100 >= 50) { /* hide measuring noise without vsync */
        p = put_str(p, end, " us, Vsync ");
        p = put_uint(p, end, (unsigned) (sum_wait / 100));
    }
    p           = put_str(p, end, " us, output ");
    p           = put_uint(p, end, (unsigned) (sum_out / 100));
    if (g_perf_tex_us + g_perf_lfb_us >= 100 * 50) { /* only if significant */
        p = put_str(p, end, " us, of which textures ");
        p = put_uint(p, end, (unsigned) (g_perf_tex_us / 100));
        p = put_str(p, end, " us, LFB ");
        p = put_uint(p, end, (unsigned) (g_perf_lfb_us / 100));
    }
    p           = put_str(p, end, " us per frame\n");
    g_perf_tex_us = g_perf_lfb_us = 0;
    *p          = 0;
    platform_log(line);
    frames = 0;
    sum_render = sum_wait = sum_out = 0;
}
#endif

/* Show the frame with timing (game+render = end of the last output until
 * now; vsync = waiting for retrace; output = image options and
 * platform_blit), with the OSD if enabled. */
void glide_present(int vsync) {
    uint64_t  t0 = platform_time_us();
    if (vsync > 0)
        platform_wait_vretrace(vsync);
    uint64_t tv = platform_time_us();
    /* FSAA/AA_POST: processed image (core/fsaa.c), otherwise fb_mem itself */
    uint16_t *fb = (uint16_t *) glide_fsaa_output();
    int       w  = 0;
    glide_splash_first_frame(fb); /* monitor sync-in (GLIDULATOR_SPLASH) */
    if (g_osd_save) {
        w = text_width();
        if (w > g_width - OSD_X)
            w = g_width - OSD_X;
        for (int y = 0; y < OSD_H; y++) {
            const uint16_t *src = fb + (size_t) (OSD_Y + y) * (size_t) g_width + OSD_X;
            uint16_t       *dst = g_osd_save + (size_t) y * (size_t) g_width;
            for (int x = 0; x < w; x++)
                dst[x] = src[x];
        }
        draw(fb, w);
    }
    /* video filter if on (core/vfilter.c), except the OSD box */
    glide_blit_output_keep(fb, g_osd_save ? OSD_X : 0, OSD_Y, g_osd_save ? w : 0, box_height());
    if (g_osd_save)
        for (int y = 0; y < OSD_H; y++) {
            uint16_t       *dst = fb + (size_t) (OSD_Y + y) * (size_t) g_width + OSD_X;
            const uint16_t *src = g_osd_save + (size_t) y * (size_t) g_width;
            for (int x = 0; x < w; x++)
                dst[x] = src[x];
        }
    glide_fsaa_frame(t0 - g_last_end); /* FSAA=auto: level for the next frame */
    uint64_t t1 = platform_time_us();
#ifdef GLIDULATOR_TRACE
    trace_perf(t1, t0 - g_last_end, tv - t0, t1 - tv);
#endif
    g_sum_render += t0 - g_last_end;
    g_sum_out += t1 - tv;
    g_last_end = t1;
    g_frames++;
    if (t1 - g_period_start >= OSD_UPDATE_US) {
        uint64_t dt = t1 - g_period_start;
        if (g_osd_save)
                set_lines((unsigned) (g_frames * 10000000ull / dt), (unsigned) (g_sum_render / 100 / g_frames),
                      (unsigned) (g_sum_out / 100 / g_frames));
        g_period_start = t1;
        g_sum_render = g_sum_out = 0;
        g_frames                 = 0;
    }
}
