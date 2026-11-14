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
 * Glide 2.1.1 (glide.dll, core/glide211_abi.c) has no lock: grLfbGetWritePtr
 * hands out a pointer into the board's address space (stride 2048 bytes,
 * 4096 for 32-bit formats), and grLfbBypassMode decides whether writes go
 * through the pixel pipeline. Games write only the pixels they need (a HUD
 * over the 3D scene). Here the pointer is a shadow buffer filled with the
 * current picture plus an untouched copy of it ("pre-image"); at
 * grLfbEnd/grBufferSwap/grBufferClear only pixels that differ from the
 * pre-image are written, through the pixel pipeline if the game asked for
 * it (chroma key, alpha test, depth test, alpha blending, write masks).
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
#include "vid_voodoo_render.h" /* DEPTH_TEST, ALPHA_TEST, ALPHA_BLEND */

#include <string.h>

static GrColorFormat_t g_lfb_color_format = GR_COLORFORMAT_ARGB;

/* Alpha and depth for pipeline writes in formats without them
 * (grLfbConstantAlpha/grLfbConstantDepth). */
static int      g_lfb_const_alpha = 0xff;
static uint16_t g_lfb_const_depth;

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

/* 32-bit colour in the LFB colour format (grLfbWriteColorFormat) -> 8-bit
 * channels. */
static void unpack32(uint32_t c, uint32_t *r, uint32_t *g, uint32_t *b, uint32_t *a) {
    switch (g_lfb_color_format) {
    case GR_COLORFORMAT_ABGR: *a = c >> 24; *b = (c >> 16) & 0xff; *g = (c >> 8) & 0xff; *r = c & 0xff; break;
    case GR_COLORFORMAT_RGBA: *r = c >> 24; *g = (c >> 16) & 0xff; *b = (c >> 8) & 0xff; *a = c & 0xff; break;
    case GR_COLORFORMAT_BGRA: *b = c >> 24; *g = (c >> 16) & 0xff; *r = (c >> 8) & 0xff; *a = c & 0xff; break;
    default: *a = c >> 24; *r = (c >> 16) & 0xff; *g = (c >> 8) & 0xff; *b = c & 0xff; break; /* ARGB */
    }
}

/* 32-bit colour in the LFB colour format -> RGB565. */
static uint16_t from32(uint32_t c) {
    uint32_t r, g, b, a;
    unpack32(c, &r, &g, &b, &a);
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

/* One pixel through the pixel pipeline, as the board does for LFB writes
 * with lfbMode bit 8 set: chroma key, alpha test, depth test, alpha
 * blending and the write masks, in the rasterizer's order and with its
 * macros (vid_voodoo_render.c). No colour combine, fog or dithering.
 * cmask: bits of each channel the chroma key compares (16-bit sources
 * carry only 5 of them). The rasterizer's macros compare int with the
 * unsigned a_ref, which the rasterizer's own flags do not warn about. */
#if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wsign-compare"
#endif
static void put_rgba_pipeline(size_t i, int src_r, int src_g, int src_b, int src_a, int depth, int cmask) {
    const voodoo_params_t *params = &g_voodoo.params;
    if ((params->fbzMode & FBZ_CHROMAKEY) && !((src_r ^ params->chromaKey_r) & cmask) &&
        !((src_g ^ params->chromaKey_g) & cmask) && !((src_b ^ params->chromaKey_b) & cmask))
        return;
    if (params->alphaMode & 1)
        ALPHA_TEST(src_a);
    if (params->fbzMode & FBZ_DEPTH_ENABLE) {
        int old_depth = aux_buf()[i];
        DEPTH_TEST(depth);
    }
    if (params->alphaMode & (1 << 4)) {
        uint16_t dat       = color_buf()[i];
        int      dest_r    = (dat >> 8) & 0xf8, dest_g = (dat >> 3) & 0xfc, dest_b = (dat << 3) & 0xf8;
        int      dest_a    = 0xff;
        int      colbfog_r = src_r, colbfog_g = src_g, colbfog_b = src_b; /* no fog: colour before fog */
        dest_r |= dest_r >> 5;
        dest_g |= dest_g >> 6;
        dest_b |= dest_b >> 5;
        ALPHA_BLEND(src_r, src_g, src_b, src_a);
    }
    if (params->fbzMode & FBZ_RGB_WMASK)
        set_col(i, pack565((uint32_t) src_r, (uint32_t) src_g, (uint32_t) src_b));
    if ((params->fbzMode & (FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE)) == (FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE))
        set_aux(i, (uint16_t) depth);
skip_pixel:
    return;
}
#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

/* Like put_pixel(), but through the pixel pipeline: alpha from the 1555
 * bit or the 8888 alpha byte, otherwise grLfbConstantAlpha; depth from the
 * *_DEPTH formats, otherwise grLfbConstantDepth. */
static void put_pixel_pipeline(GrLfbSrcFmt_t fmt, int x, int y, const uint8_t *src) {
    size_t   i   = (size_t) y * (size_t) g_width + (size_t) x;
    uint16_t v16 = (uint16_t) (src[0] | (src[1] << 8));
    uint32_t v32 = 0, r, g, b, a = (uint32_t) g_lfb_const_alpha;
    int      depth = g_lfb_const_depth, cmask = 0xf8;
    uint16_t c565;
    if (bytes_per_pixel(fmt) == 4)
        v32 = (uint32_t) src[0] | ((uint32_t) src[1] << 8) | ((uint32_t) src[2] << 16) | ((uint32_t) src[3] << 24);
    switch (fmt) {
    case GR_LFB_SRC_FMT_565: c565 = v16; break;
    case GR_LFB_SRC_FMT_555: c565 = from555(v16); break;
    case GR_LFB_SRC_FMT_1555:
        c565 = from555(v16);
        a    = (v16 & 0x8000) ? 0xff : 0;
        break;
    case GR_LFB_SRC_FMT_888:
    case GR_LFB_SRC_FMT_8888: {
        uint32_t a32;
        unpack32(v32, &r, &g, &b, &a32);
        if (fmt == GR_LFB_SRC_FMT_8888)
            a = a32;
        put_rgba_pipeline(i, (int) r, (int) g, (int) b, (int) a, depth, 0xff);
        return;
    }
    case GR_LFB_SRC_FMT_565_DEPTH:
        c565  = (uint16_t) v32;
        depth = (int) (v32 >> 16);
        break;
    case GR_LFB_SRC_FMT_555_DEPTH:
    case GR_LFB_SRC_FMT_1555_DEPTH:
        c565  = from555((uint16_t) v32);
        depth = (int) (v32 >> 16);
        if (fmt == GR_LFB_SRC_FMT_1555_DEPTH)
            a = (v32 & 0x8000) ? 0xff : 0;
        break;
    case GR_LFB_SRC_FMT_ZA16: set_aux(i, v16); return;
    default: return;
    }
    r = (c565 >> 8) & 0xf8;
    g = (c565 >> 3) & 0xfc;
    b = (c565 << 3) & 0xf8;
    put_rgba_pipeline(i, (int) (r | (r >> 5)), (int) (g | (g >> 6)), (int) (b | (b >> 5)), (int) a, depth, cmask);
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
    glide_lfb211_sync(0); /* Glide 2.1.1 pointer writes first, in order */
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
    glide_lfb211_sync(0);
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
    glide_lfb211_sync(0);
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

/* Only used by writes through the pixel pipeline (Glide 2.1.1, see the
 * top); Glide 2.x locks here always bypass it. */
void GR_EXPORT GLIDE_API grLfbConstantAlpha(GrAlpha_t alpha) {
    GLIDE_TRACE("grLfbConstantAlpha");
    g_lfb_const_alpha = alpha;
}

void GR_EXPORT GLIDE_API grLfbConstantDepth(FxU16 depth) {
    GLIDE_TRACE("grLfbConstantDepth");
    g_lfb_const_depth = depth;
}

/* --- Glide 2.1.1: pointer access without a lock (see the top) ---------- */

/* Shadow formats; the picture copy (snap) is kept for all but DEPTH, whose
 * shadow combines colour and depth. */
enum { CLS_565, CLS_555, CLS_32, CLS_ZA16, CLS_DEPTH };

static struct {
    uint8_t           *buf;      /* what the game writes into */
    uint8_t           *pre;      /* buf as handed out, for the comparison */
    uint16_t          *snap;     /* picture rows buf/pre were made from */
    uint16_t          *rd;       /* RGB565 copy for grLfbGetReadPtr */
    size_t             bytes;    /* size of buf and pre */
    size_t             rd_bytes; /* size of rd */
    size_t             snap_px;  /* size of snap in pixels */
    int                armed;    /* an access is open */
    int                pipeline; /* write through the pixel pipeline */
    GrLfbWriteMode_t   mode;
    GrBuffer_t         buffer;
    GrOriginLocation_t origin;
    /* what snap/buf/pre were made for; snap_ok = 0: everything anew */
    int                snap_ok, snap_cls, snap_w, snap_h;
    GrOriginLocation_t snap_origin;
    GrColorFormat_t    snap_cf;
} g211;


int g_lfb211_open; /* = g211.armed, read before every triangle */

static int class211(GrLfbWriteMode_t m) {
    switch (m) {
    case GR_LFBWRITEMODE_565: return CLS_565;
    case GR_LFBWRITEMODE_555:
    case GR_LFBWRITEMODE_1555: return CLS_555;
    case GR_LFBWRITEMODE_888:
    case GR_LFBWRITEMODE_8888: return CLS_32;
    case GR_LFBWRITEMODE_ZA16: return CLS_ZA16;
    default: return CLS_DEPTH;
    }
}

/* Bytes per row of the board's LFB address space: 1024 pixels. */
static size_t stride211(GrLfbWriteMode_t mode) {
    int w = g_width > 1024 ? g_width : 1024;
    return (size_t) w * (size_t) shadow_bpp(mode);
}

/* Picture row of LFB row y (grLfbOrigin). */
static int row211(int y) {
    return g211.origin == GR_ORIGIN_LOWER_LEFT ? g_height - 1 - y : y;
}

static int alloc211(void) {
    size_t bytes = stride211(GR_LFBWRITEMODE_8888) * (size_t) g_height; /* room for every format */
    size_t px    = (size_t) g_width * (size_t) g_height;
    if (g211.bytes < bytes) {
        platform_free(g211.buf);
        platform_free(g211.pre);
        g211.buf     = (uint8_t *) platform_alloc(bytes);
        g211.pre     = (uint8_t *) platform_alloc(bytes);
        g211.bytes   = (g211.buf && g211.pre) ? bytes : 0;
        g211.snap_ok = 0;
    }
    if (g211.snap_px < px) {
        platform_free(g211.snap);
        g211.snap    = (uint16_t *) platform_alloc(px * 2);
        g211.snap_px = g211.snap ? px : 0;
        g211.snap_ok = 0;
    }
    return g211.bytes && g211.snap_px;
}

/* a == b for n bytes (n even, both 2-byte aligned). msvcrt's memcmp is a
 * byte loop on Wine (and old Windows); these rows are compared several
 * times per frame. */
static int same211(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *) a, *q = (const uint8_t *) b;
    size_t         i = 0;
    for (; i + 16 <= n; i += 16) {
        uint32_t d = 0, w1, w2;
        for (int k = 0; k < 16; k += 4) {
            memcpy(&w1, p + i + k, 4); /* aliasing-safe, compiles to a load */
            memcpy(&w2, q + i + k, 4);
            d |= w1 ^ w2;
        }
        if (d)
            return 0;
    }
    for (; i < n; i += 2)
        if (*(const uint16_t *) (p + i) != *(const uint16_t *) (q + i))
            return 0;
    return 1;
}

/* Picture plane the shadow format is made from. */
static const uint16_t *plane211(int cls) {
    return cls == CLS_ZA16 ? aux_buf() : color_buf();
}

/* Picture row py in the shadow format into dst. */
static void convert_row211(uint8_t *dst, int py, int cls) {
    size_t          i0 = (size_t) py * (size_t) g_width;
    const uint16_t *c  = plane211(cls) + i0;
    switch (cls) {
    case CLS_565:
    case CLS_ZA16: memcpy(dst, c, (size_t) g_width * 2); break;
    case CLS_555:
        for (int x = 0; x < g_width; x++)
            ((uint16_t *) dst)[x] = to555(c[x]);
        break;
    default:
        for (int x = 0; x < g_width; x++)
            ((uint32_t *) dst)[x] = shadow_of(i0 + (size_t) x, g211.mode);
        break;
    }
}

/* Bring buf and pre up to the current picture: only rows that changed
 * since they were made (snap), all of them for a new format/origin. The
 * game asks for the pointer many times per frame (Scorched Planet: per HUD
 * element), and converting the whole picture each time cost 3/4 of the
 * frame time. */
static void refresh211(void) {
    int             cls   = class211(g211.mode);
    int             cache = cls != CLS_DEPTH;
    size_t          strd  = stride211(g211.mode);
    size_t          rowb  = (size_t) g_width * (size_t) shadow_bpp(g211.mode);
    const uint16_t *plane = plane211(cls);
    int valid = cache && g211.snap_ok && g211.snap_cls == cls && g211.snap_w == g_width && g211.snap_h == g_height &&
                g211.snap_origin == g211.origin && (cls != CLS_32 || g211.snap_cf == g_lfb_color_format);
    for (int y = 0; y < g_height; y++) {
        int             py   = row211(y);
        const uint16_t *fb   = plane + (size_t) py * (size_t) g_width;
        uint16_t       *snap = g211.snap + (size_t) py * (size_t) g_width;
        uint8_t        *row  = g211.buf + (size_t) y * strd;
        if (valid && same211(snap, fb, (size_t) g_width * 2) && same211(row, g211.pre + (size_t) y * strd, rowb))
            continue; /* unchanged picture, nothing pending */
        convert_row211(row, py, cls);
        memcpy(g211.pre + (size_t) y * strd, row, rowb);
        if (cache)
            memcpy(snap, fb, (size_t) g_width * 2);
    }
    g211.snap_ok     = cache;
    g211.snap_cls    = cls;
    g211.snap_w      = g_width;
    g211.snap_h      = g_height;
    g211.snap_origin = g211.origin;
    g211.snap_cf     = g_lfb_color_format;
}

void *glide_lfb211_write_ptr(GrBuffer_t buffer, GrLfbWriteMode_t mode, int pipeline, GrOriginLocation_t origin) {
    if (!g_win_open)
        return NULL;
    if (mode == GR_LFBWRITEMODE_ANY)
        mode = GR_LFBWRITEMODE_565;
    if (is_aux(buffer))
        mode = GR_LFBWRITEMODE_ZA16;
    if (g211.armed && g211.mode == mode && g211.pipeline == pipeline && g211.origin == origin)
        return g211.buf; /* still the same access: keep what was written */
    glide_lfb211_sync(0);
    glide_mt_idle(); /* workers must not access the framebuffer at the same time */
    if (!alloc211())
        return NULL;
    g211.mode     = mode;
    g211.pipeline = pipeline;
    g211.buffer   = buffer;
    g211.origin   = origin;
    refresh211();
    g211.armed = g_lfb211_open = 1;
    return g211.buf;
}

/* grLfbBegin: a pointer the game kept from an earlier access is valid
 * again, with that access's format; buf/pre are brought up to the
 * picture first. */
void glide_lfb211_begin(void) {
    if (g211.armed || !g211.bytes || !g_win_open)
        return;
    glide_mt_idle();
    refresh211();
    g211.armed = g_lfb211_open = 1;
}

/* grLfbEnd: the writes stay pending until something needs the picture
 * (glide_lfb211_sync callers), so many small accesses per frame (one per
 * HUD element) cost one comparison of the buffer per frame, not one each.
 * The front buffer is visible, so it is written at once. */
void glide_lfb211_end(void) {
    if (g211.armed && g211.buffer == GR_BUFFER_FRONTBUFFER)
        glide_lfb211_sync(0);
}

/* Write every pixel that differs from the pre-image and set buf, pre and
 * snap of that pixel to the result (a pixel the pipeline rejected keeps
 * the picture's value). rearm: bring buf/pre up to the picture again and
 * keep the access open (the game keeps the pointer, e.g. across
 * grBufferSwap or grLfbBegin), otherwise the access ends (grLfbEnd). */
void glide_lfb211_sync(int rearm) {
    if (!g211.armed || !g_win_open) {
        g211.armed = g_lfb211_open = 0;
        return;
    }
#ifdef GLIDULATOR_TRACE
    uint64_t t0 = platform_time_us();
#endif
    glide_mt_idle();
    int             cls   = class211(g211.mode);
    size_t          strd  = stride211(g211.mode);
    int             bpp   = shadow_bpp(g211.mode);
    int             aux   = is_aux(g211.buffer);
    const uint16_t *plane = plane211(cls);
    for (int y = 0; y < g_height; y++) {
        uint8_t *row = g211.buf + (size_t) y * strd;
        uint8_t *old = g211.pre + (size_t) y * strd;
        if (same211(row, old, (size_t) g_width * (size_t) bpp))
            continue;
        int py = row211(y);
        for (int x = 0; x < g_width; x++) {
            size_t o = (size_t) x * (size_t) bpp; /* no memcmp per pixel: a library call each */
            if (bpp == 2 ? *(uint16_t *) (row + o) == *(uint16_t *) (old + o)
                         : *(uint32_t *) (row + o) == *(uint32_t *) (old + o))
                continue;
            if (g211.pipeline && !aux)
                put_pixel_pipeline((GrLfbSrcFmt_t) g211.mode, x, py, row + o);
            else
                put_pixel((GrLfbSrcFmt_t) g211.mode, aux, x, py, row + o);
            size_t   i = (size_t) py * (size_t) g_width + (size_t) x;
            uint32_t v = shadow_of(i, g211.mode);
            if (bpp == 2)
                *(uint16_t *) (row + o) = *(uint16_t *) (old + o) = (uint16_t) v;
            else
                *(uint32_t *) (row + o) = *(uint32_t *) (old + o) = v;
            if (cls != CLS_DEPTH)
                g211.snap[i] = plane[i];
        }
    }
    show_if_front(g211.buffer);
    g211.armed = g_lfb211_open = 0;
    if (rearm) {
        refresh211();
        g211.armed = g_lfb211_open = 1;
    }
#ifdef GLIDULATOR_TRACE
    g_perf_lfb_us += platform_time_us() - t0;
#endif
}

const void *glide_lfb211_read_ptr(GrBuffer_t buffer, GrOriginLocation_t origin) {
    if (!g_win_open)
        return NULL;
    glide_lfb211_sync(0); /* the game reads what it wrote */
    glide_mt_idle();
    size_t w = (size_t) (g_width > 1024 ? g_width : 1024);
    if (g211.rd_bytes < w * 2 * (size_t) g_height) {
        platform_free(g211.rd);
        g211.rd       = (uint16_t *) platform_alloc(w * 2 * (size_t) g_height);
        g211.rd_bytes = g211.rd ? w * 2 * (size_t) g_height : 0;
    }
    if (!g211.rd)
        return NULL;
    const uint16_t *src = is_aux(buffer) ? aux_buf() : color_buf();
    for (int y = 0; y < g_height; y++) {
        int py = origin == GR_ORIGIN_LOWER_LEFT ? g_height - 1 - y : y;
        memcpy(g211.rd + (size_t) y * w, src + (size_t) py * (size_t) g_width, (size_t) g_width * 2);
    }
    return g211.rd;
}

void glide_lfb_reset(void) {
    g_lfb_color_format = GR_COLORFORMAT_ARGB;
    g_shadow_active    = 0;
    g_lfb_const_alpha  = 0xff;
    g_lfb_const_depth  = 0;
    g211.armed         = 0;
    g_lfb211_open      = 0;
    g211.snap_ok       = 0; /* new picture buffers after grSstWinOpen */
}
