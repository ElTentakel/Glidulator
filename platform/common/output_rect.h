/* Where a Glide image (w x h) goes on a display (cw x ch); shared by the
 * outputs (Windows, DOS OVL), tested in targets/glide_test_linux.
 *
 * The Voodoo showed every Glide resolution full-screen on a 4:3 monitor,
 * including 640x400 and 320x200. "aspect" therefore keeps 4:3 (for square
 * display pixels) and fills the rest black, so the picture stays
 * undistorted on 16:9 monitors. */

#ifndef GLIDULATOR_OUTPUT_RECT_H
#define GLIDULATOR_OUTPUT_RECT_H

enum {
    OUT_SCALE_ASPECT = 0, /* largest 4:3 rectangle, centred (default) */
    OUT_SCALE_STRETCH,    /* whole display */
    OUT_SCALE_INTEGER,    /* largest integer factor, centred */
    OUT_SCALE_NONE        /* 1:1, centred */
};

/* "aspect", "stretch", "integer", "none" -> OUT_SCALE_*, else -1. */
static inline int output_scale_parse(const char *s) {
    static const char *const names[] = { "aspect", "stretch", "integer", "none" };
    for (int i = 0; i < 4; i++) {
        const char *a = s, *b = names[i];
        while (*a && (*a | 0x20) == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return -1;
}

/* GLIDULATOR_DISPLAY: "1280x720" or "1280x720x16" -> width/height/bits
 * (0 = not given); "glide"/"desktop" etc. leave everything 0. */
static inline void output_display_parse(const char *s, int *w, int *h, int *bpp) {
    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3 && *s >= '0' && *s <= '9'; i++) {
        while (*s >= '0' && *s <= '9')
            v[i] = v[i] * 10 + (*s++ - '0');
        if (*s != 'x' && *s != 'X')
            break;
        s++;
    }
    *w   = v[0];
    *h   = v[1];
    *bpp = v[2];
}

static inline void output_rect(int mode, int w, int h, int cw, int ch, int *x, int *y, int *rw, int *rh) {
    int k;
    switch (mode) {
    case OUT_SCALE_STRETCH:
        *rw = cw;
        *rh = ch;
        break;
    case OUT_SCALE_INTEGER:
    case OUT_SCALE_NONE:
        k = mode == OUT_SCALE_NONE ? (w <= cw && h <= ch) : (cw / w < ch / h ? cw / w : ch / h);
        if (k >= 1) {
            *rw = w * k;
            *rh = h * k;
            break;
        }
        /* does not even fit 1:1: like aspect */
        /* fall through */
    default:
        if (cw * 3 >= ch * 4) { /* display wider than 4:3: borders left/right */
            *rh = ch;
            *rw = ch * 4 / 3;
        } else {
            *rw = cw;
            *rh = cw * 3 / 4;
        }
        break;
    }
    *x = (cw - *rw) / 2;
    *y = (ch - *rh) / 2;
}

#endif /* GLIDULATOR_OUTPUT_RECT_H */
