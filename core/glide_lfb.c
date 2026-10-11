/*
 * Direct framebuffer access (Glide "LFB"): grLfbWriteRegion,
 * grLfbReadRegion, grLfbLock/Unlock and their formats. Used for loading
 * screens, menus, HUDs and videos, i.e. everything a game paints itself
 * instead of drawing triangles.
 *
 * The framebuffer is RGB565 colour plus 16-bit depth (aux buffer after
 * it), see grSstWinOpen(). Front and back buffer are the same memory, so
 * writes to the FRONT buffer are shown immediately, since by definition
 * it is visible.
 *
 * As in Glide 2.x, writes bypass the pixel pipeline (no depth test, no
 * blending); pixelPipeline=FXTRUE in grLfbLock is ignored.
 *
 * FSAA (core/fsaa.c): writes go to all planes (identical values resolve
 * to exactly the same pixel, so GUIs stay sharp), reads come from plane 0.
 * Write locks then always use the shadow buffer, and grLfbUnlock() applies
 * only changed pixels; otherwise every lock would overwrite the
 * anti-aliasing of the whole frame.
 */

#include "glide_internal.h"
#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include <string.h>

static GrColorFormat_t g_lfb_color_format = GR_COLORFORMAT_ARGB;

/* A lock whose write format is not RGB565 writes into the shadow buffer
 * (4 bytes per pixel suffice for every format); grLfbUnlock() converts to
 * RGB565. */
static int              g_shadow_active;
static GrLfbWriteMode_t g_shadow_mode;
static GrBuffer_t       g_shadow_buffer;

static uint16_t *color_buf(void) {
    return (uint16_t *) g_voodoo.fb_mem;
}

static uint16_t *aux_buf(void) {
    return (uint16_t *) (g_voodoo.fb_mem + g_aux_offset);
}

/* Pixel i into every FSAA plane (just the one without FSAA). */
static void set_col(size_t i, uint16_t v) {
    if (g_aa_mask)
        glide_aa_mark(i); /* edge smoothing skips this pixel */
    for (int k = 0; k < g_fsaa_n; k++)
        ((uint16_t *) (g_voodoo.fb_mem + (size_t) k * g_fsaa_plane_bytes))[i] = v;
}

static void set_aux(size_t i, uint16_t v) {
    for (int k = 0; k < g_fsaa_n; k++)
        ((uint16_t *) (g_voodoo.fb_mem + (size_t) k * g_fsaa_plane_bytes + g_aux_offset))[i] = v;
}

static uint16_t pack565(uint32_t r8, uint32_t g8, uint32_t b8) {
    return (uint16_t) (((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3));
}

/* 32-bit colour in the LFB colour format (grLfbWriteColorFormat) -> RGB565. */
static uint16_t from32(uint32_t c) {
    uint32_t r, g, b;
    switch (g_lfb_color_format) {
    case GR_COLORFORMAT_ABGR: b = (c >> 16) & 0xff; g = (c >> 8) & 0xff; r = c & 0xff; break;
    case GR_COLORFORMAT_RGBA: r = c >> 24; g = (c >> 16) & 0xff; b = (c >> 8) & 0xff; break;
    case GR_COLORFORMAT_BGRA: b = c >> 24; g = (c >> 16) & 0xff; r = (c >> 8) & 0xff; break;
    default: r = (c >> 16) & 0xff; g = (c >> 8) & 0xff; b = c & 0xff; break; /* ARGB */
    }
    return pack565(r, g, b);
}

static uint32_t to32(uint16_t p) {
    uint32_t r5 = (uint32_t) (p >> 11) & 0x1f, g6 = (uint32_t) (p >> 5) & 0x3f, b5 = (uint32_t) p & 0x1f;
    uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
    switch (g_lfb_color_format) {
    case GR_COLORFORMAT_ABGR: return 0xff000000u | (b << 16) | (g << 8) | r;
    case GR_COLORFORMAT_RGBA: return (r << 24) | (g << 16) | (b << 8) | 0xffu;
    case GR_COLORFORMAT_BGRA: return (b << 24) | (g << 16) | (r << 8) | 0xffu;
    default: return 0xff000000u | (r << 16) | (g << 8) | b;
    }
}

/* x555 / 1555 -> 565: widen green by one bit (replicate the top bit). */
static uint16_t from555(uint16_t p) {
    uint16_t g5 = (p >> 5) & 0x1f;
    return (uint16_t) (((p & 0x7c00) << 1) | (g5 << 6) | ((g5 >> 4) << 5) | (p & 0x1f));
}

static uint16_t to555(uint16_t p) {
    return (uint16_t) (0x8000 | ((p >> 1) & 0x7c00) | ((p >> 1) & 0x03e0) | (p & 0x1f));
}

static int bytes_per_pixel(GrLfbSrcFmt_t fmt) {
    switch (fmt) {
    case GR_LFB_SRC_FMT_565:
    case GR_LFB_SRC_FMT_555:
    case GR_LFB_SRC_FMT_1555:
    case GR_LFB_SRC_FMT_ZA16:
        return 2;
    case GR_LFB_SRC_FMT_888:
    case GR_LFB_SRC_FMT_8888:
    case GR_LFB_SRC_FMT_565_DEPTH:
    case GR_LFB_SRC_FMT_555_DEPTH:
    case GR_LFB_SRC_FMT_1555_DEPTH:
        return 4;
    default:
        return 0; /* RLE16 etc.: not supported */
    }
}

/* Write one pixel in the source format at (x, y). *_DEPTH: colour in the
 * lower, depth in the upper 16 bits (lfbMode formats 12..14). */
static void put_pixel(GrLfbSrcFmt_t fmt, int to_aux, int x, int y, const uint8_t *src) {
    size_t    i   = (size_t) y * (size_t) g_width + (size_t) x;
    uint16_t  v16 = (uint16_t) (src[0] | (src[1] << 8));
    uint32_t  v32 = 0;
    if (bytes_per_pixel(fmt) == 4)
        v32 = (uint32_t) src[0] | ((uint32_t) src[1] << 8) | ((uint32_t) src[2] << 16) | ((uint32_t) src[3] << 24);

    if (to_aux) { /* aux/depth buffer: only ZA16 makes sense */
        if (fmt == GR_LFB_SRC_FMT_ZA16)
            set_aux(i, v16);
        return;
    }
    switch (fmt) {
    case GR_LFB_SRC_FMT_565: set_col(i, v16); break;
    case GR_LFB_SRC_FMT_555:
    case GR_LFB_SRC_FMT_1555: set_col(i, from555(v16)); break;
    case GR_LFB_SRC_FMT_888:
    case GR_LFB_SRC_FMT_8888: set_col(i, from32(v32)); break;
    case GR_LFB_SRC_FMT_565_DEPTH:
        set_col(i, (uint16_t) v32);
        set_aux(i, (uint16_t) (v32 >> 16));
        break;
    case GR_LFB_SRC_FMT_555_DEPTH:
    case GR_LFB_SRC_FMT_1555_DEPTH:
        set_col(i, from555((uint16_t) v32));
        set_aux(i, (uint16_t) (v32 >> 16));
        break;
    case GR_LFB_SRC_FMT_ZA16: set_aux(i, v16); break;
    default: break;
    }
}

static int is_aux(GrBuffer_t b) {
    return b == GR_BUFFER_AUXBUFFER || b == GR_BUFFER_DEPTHBUFFER || b == GR_BUFFER_ALPHABUFFER;
}

static void show_if_front(GrBuffer_t b) {
    if (b == GR_BUFFER_FRONTBUFFER) {
        glide_mt_idle();
        glide_blit_output(glide_fsaa_output()); /* FSAA resolved, video filter if on */
    }
}

FxBool GR_EXPORT GLIDE_API grLfbWriteRegion(GrBuffer_t dst_buffer, FxU32 dst_x, FxU32 dst_y, GrLfbSrcFmt_t src_format,
                                            FxU32 src_width, FxU32 src_height, FxI32 src_stride, void *src_data) {
    GLIDE_TRACE("grLfbWriteRegion");
#ifdef GLIDULATOR_TRACE
    uint64_t t0 = platform_time_us();
#endif
    glide_mt_idle(); /* workers must not access the framebuffer at the same time */
    int bpp = bytes_per_pixel(src_format);
    if (!g_win_open || !src_data || !bpp)
        return FXFALSE;
    /* Clip to the screen, not to the clip window: LFB writes bypass the
     * pixel pipeline. */
    const uint8_t *src = (const uint8_t *) src_data;
    for (FxU32 j = 0; j < src_height; j++) {
        FxU32 y = dst_y + j;
        if (y >= (FxU32) g_height)
            break;
        const uint8_t *row = src + (ptrdiff_t) j * src_stride;
        for (FxU32 i = 0; i < src_width; i++) {
            FxU32 x = dst_x + i;
            if (x >= (FxU32) g_width)
                break;
            put_pixel(src_format, is_aux(dst_buffer), (int) x, (int) y, row + (size_t) i * (size_t) bpp);
        }
    }
    show_if_front(dst_buffer);
#ifdef GLIDULATOR_TRACE
    g_perf_lfb_us += platform_time_us() - t0;
#endif
    return FXTRUE;
}

/* Reads always return RGB565 (colour) or 16-bit depth (aux). */
FxBool GR_EXPORT GLIDE_API grLfbReadRegion(GrBuffer_t src_buffer, FxU32 src_x, FxU32 src_y, FxU32 src_width,
                                           FxU32 src_height, FxU32 dst_stride, void *dst_data) {
    GLIDE_TRACE("grLfbReadRegion");
    glide_mt_idle(); /* workers must not access the framebuffer at the same time */
    if (!g_win_open || !dst_data || src_x + src_width > (FxU32) g_width || src_y + src_height > (FxU32) g_height)
        return FXFALSE;
    const uint16_t *buf = is_aux(src_buffer) ? aux_buf() : color_buf();
    for (FxU32 j = 0; j < src_height; j++)
        memcpy((uint8_t *) dst_data + (size_t) j * dst_stride, buf + (size_t) (src_y + j) * (size_t) g_width + src_x,
               (size_t) src_width * 2);
    return FXTRUE;
}

/* Shadow buffer format: 32 bits for 888, 8888 and the DEPTH variants,
 * 16 bits for 555/1555 (and with FSAA for 565/ZA16). */
static int shadow_bpp(GrLfbWriteMode_t m) {
    return (m == GR_LFBWRITEMODE_555 || m == GR_LFBWRITEMODE_1555 || m == GR_LFBWRITEMODE_565 ||
            m == GR_LFBWRITEMODE_ZA16)
               ? 2
               : 4;
}

/* Pixel i (plane 0) in shadow format: prefill at lock time, comparison at
 * unlock time (FSAA: apply only what changed). */
static uint32_t shadow_of(size_t i, GrLfbWriteMode_t m) {
    const uint16_t *fb = color_buf();
    if (m == GR_LFBWRITEMODE_565)
        return fb[i];
    if (m == GR_LFBWRITEMODE_ZA16)
        return aux_buf()[i];
    if (m == GR_LFBWRITEMODE_555 || m == GR_LFBWRITEMODE_1555)
        return to555(fb[i]);
    if (m >= GR_LFBWRITEMODE_565_DEPTH)
        return fb[i] | ((uint32_t) aux_buf()[i] << 16);
    return to32(fb[i]);
}

FxBool GR_EXPORT GLIDE_API grLfbLock(GrLock_t type, GrBuffer_t buffer, GrLfbWriteMode_t writeMode,
                                     GrOriginLocation_t origin, FxBool pixelPipeline, GrLfbInfo_t *info) {
    GLIDE_TRACE("grLfbLock");
    glide_mt_idle(); /* workers must not access the framebuffer at the same time */
    (void) pixelPipeline;
    if (!g_win_open || !info)
        return FXFALSE;
    int write = (type & 1) == GR_LFB_WRITE_ONLY;
    info->origin    = origin;
    info->writeMode = writeMode;

    if (!write || (g_fsaa_n == 1 && (is_aux(buffer) || writeMode == GR_LFBWRITEMODE_565 ||
                                     writeMode == GR_LFBWRITEMODE_ANY || writeMode == GR_LFBWRITEMODE_ZA16))) {
        /* Directly into the framebuffer: RGB565 or 16-bit depth. */
        info->lfbPtr        = is_aux(buffer) ? (void *) aux_buf() : (void *) color_buf();
        info->strideInBytes = (FxU32) g_width * 2;
        if (writeMode == GR_LFBWRITEMODE_ANY)
            info->writeMode = is_aux(buffer) ? GR_LFBWRITEMODE_ZA16 : GR_LFBWRITEMODE_565;
        return FXTRUE;
    }
    if (!g_lfb_shadow)
        return FXFALSE;
    /* FSAA: 565/ZA16 and aux also through the shadow buffer (see top) */
    if (is_aux(buffer))
        writeMode = GR_LFBWRITEMODE_ZA16;
    else if (writeMode == GR_LFBWRITEMODE_ANY)
        writeMode = GR_LFBWRITEMODE_565;
    info->writeMode = writeMode;
    /* Prefill the shadow buffer with the current content so pixels the
     * game does not write stay unchanged at unlock (555/1555 lose the
     * lowest green bit, as a real 555 lock does). */
    int bpp = shadow_bpp(writeMode);
    for (int i = 0; i < g_width * g_height; i++) {
        if (bpp == 2)
            ((uint16_t *) g_lfb_shadow)[i] = (uint16_t) shadow_of((size_t) i, writeMode);
        else
            ((uint32_t *) g_lfb_shadow)[i] = shadow_of((size_t) i, writeMode);
    }
    g_shadow_active     = 1;
    g_shadow_mode       = writeMode;
    g_shadow_buffer     = buffer;
    info->lfbPtr        = g_lfb_shadow;
    info->strideInBytes = (FxU32) (g_width * bpp);
    return FXTRUE;
}

FxBool GR_EXPORT GLIDE_API grLfbUnlock(GrLock_t type, GrBuffer_t buffer) {
    GLIDE_TRACE("grLfbUnlock");
    if (!g_win_open)
        return FXFALSE;
    if ((type & 1) == GR_LFB_WRITE_ONLY && g_shadow_active) {
        int bpp = shadow_bpp(g_shadow_mode);
        for (int y = 0; y < g_height; y++)
            for (int x = 0; x < g_width; x++) {
                size_t         i   = (size_t) y * (size_t) g_width + (size_t) x;
                const uint8_t *src = (const uint8_t *) g_lfb_shadow + i * (size_t) bpp;
                if (g_fsaa_n > 1) { /* only what changed, see top */
                    uint32_t v = bpp == 2 ? *(const uint16_t *) src : *(const uint32_t *) src;
                    if (v == shadow_of(i, g_shadow_mode))
                        continue;
                }
                put_pixel((GrLfbSrcFmt_t) g_shadow_mode, 0, x, y, src);
            }
        g_shadow_active = 0;
        show_if_front(g_shadow_buffer);
    } else if ((type & 1) == GR_LFB_WRITE_ONLY) {
        show_if_front(buffer);
    }
    return FXTRUE;
}

/* Byte order of the 32-bit LFB formats (888/8888). */
void GR_EXPORT GLIDE_API grLfbWriteColorFormat(GrColorFormat_t colorFormat) {
    GLIDE_TRACE("grLfbWriteColorFormat");
    g_lfb_color_format = colorFormat;
}

/* Only relevant for writes through the pixel pipeline, which LFB
 * accesses do not use here; storing the value would have no effect. */
void GR_EXPORT GLIDE_API grLfbConstantAlpha(GrAlpha_t alpha) {
    GLIDE_TRACE("grLfbConstantAlpha");
    (void) alpha;
}

void GR_EXPORT GLIDE_API grLfbConstantDepth(FxU16 depth) {
    GLIDE_TRACE("grLfbConstantDepth");
    (void) depth;
}

void glide_lfb_reset(void) {
    g_lfb_color_format = GR_COLORFORMAT_ARGB;
    g_shadow_active    = 0;
}
