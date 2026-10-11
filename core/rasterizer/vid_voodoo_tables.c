/*
 * GLIDULATOR: the six pixel format lookup tables (rgb332, ai44, rgb565,
 * argb1555, argb4444, ai88) that vid_voodoo_render.c/vid_voodoo_texture.c
 * expect as `extern rgba8_t ...[]` (see vid_voodoo_common.h). 86Box fills
 * them in voodoo_init() together with the whole device setup (PCI, SLI
 * threads, ...), which is not ported, so the filling code is extracted
 * into a standalone function here. Logic unchanged from 86Box
 * (vid_voodoo.c), see ATTRIBUTION.md.
 */

#include "voodoo_compat.h"
#include "vid_voodoo_common.h"

rgba8_t rgb332[0x100];
rgba8_t ai44[0x100];
rgba8_t rgb565[0x10000];
rgba8_t argb1555[0x10000];
rgba8_t argb4444[0x10000];
rgba8_t ai88[0x10000];

/* Debug/statistics counter that vid_voodoo_render.c expects as
 * `extern int tris` (see vid_voodoo_render.h); defined in vid_voodoo.c in
 * the original. */
int tris = 0;

void
voodoo_init_color_tables(void)
{
    int c;

    for (c = 0; c < 0x100; c++) {
        rgb332[c].r = c & 0xe0;
        rgb332[c].g = (c << 3) & 0xe0;
        rgb332[c].b = (c << 6) & 0xc0;
        rgb332[c].r = rgb332[c].r | (rgb332[c].r >> 3) | (rgb332[c].r >> 6);
        rgb332[c].g = rgb332[c].g | (rgb332[c].g >> 3) | (rgb332[c].g >> 6);
        rgb332[c].b = rgb332[c].b | (rgb332[c].b >> 2);
        rgb332[c].b = rgb332[c].b | (rgb332[c].b >> 4);
        rgb332[c].a = 0xff;

        ai44[c].a = (c & 0xf0) | ((c & 0xf0) >> 4);
        ai44[c].r = (c & 0x0f) | ((c & 0x0f) << 4);
        ai44[c].g = ai44[c].b = ai44[c].r;
    }

    for (c = 0; c < 0x10000; c++) {
        rgb565[c].r = (c >> 8) & 0xf8;
        rgb565[c].g = (c >> 3) & 0xfc;
        rgb565[c].b = (c << 3) & 0xf8;
        rgb565[c].r |= (rgb565[c].r >> 5);
        rgb565[c].g |= (rgb565[c].g >> 6);
        rgb565[c].b |= (rgb565[c].b >> 5);
        rgb565[c].a = 0xff;

        argb1555[c].r = (c >> 7) & 0xf8;
        argb1555[c].g = (c >> 2) & 0xf8;
        argb1555[c].b = (c << 3) & 0xf8;
        argb1555[c].r |= (argb1555[c].r >> 5);
        argb1555[c].g |= (argb1555[c].g >> 5);
        argb1555[c].b |= (argb1555[c].b >> 5);
        argb1555[c].a = (c & 0x8000) ? 0xff : 0;

        argb4444[c].a = (c >> 8) & 0xf0;
        argb4444[c].r = (c >> 4) & 0xf0;
        argb4444[c].g = c & 0xf0;
        argb4444[c].b = (c << 4) & 0xf0;
        argb4444[c].a |= (argb4444[c].a >> 4);
        argb4444[c].r |= (argb4444[c].r >> 4);
        argb4444[c].g |= (argb4444[c].g >> 4);
        argb4444[c].b |= (argb4444[c].b >> 4);

        ai88[c].a = (c >> 8);
        ai88[c].r = c & 0xff;
        ai88[c].g = c & 0xff;
        ai88[c].b = c & 0xff;
    }
}
