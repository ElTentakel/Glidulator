/* Shared state of the Glide implementation (core/glide_*.c), not part of
 * the public ABI.
 *
 * Principle: behave like 3dfx's own Glide driver. That driver programmed
 * the Voodoo's registers (fbzMode, fbzColorPath, alphaMode, textureMode,
 * tLOD, texBaseAddr, color0/1, zaColor, ...), and the rasterizer ported
 * from 86Box (core/rasterizer/) emulates exactly that hardware. The
 * "register file" is therefore g_voodoo.params itself: state functions
 * (grColorCombine, grDepthBufferMode, grTexSource, ...) set bits there, and
 * grDrawTriangle() copies it and adds the per-triangle values (start values
 * and gradients). */

#ifndef GLIDULATOR_GLIDE_INTERNAL_H
#define GLIDULATOR_GLIDE_INTERNAL_H

#include "glidulator/glide2x.h"

#include "voodoo_compat.h"
#include "vid_voodoo_common.h"
#include "vid_voodoo_regs.h"

/* One active board per process, as in 3dfx's Glide (the gr* functions take
 * no handle). */
extern voodoo_t g_voodoo;
extern int      g_width, g_height;
extern int      g_win_open;

/* Byte offset of the depth (aux) buffer in g_voodoo.fb_mem, directly after
 * the colour buffer. */
extern uint32_t g_aux_offset;

/* Shadow buffer for grLfbLock() with a write format other than RGB565
 * (width x height x 4 bytes, allocated in grSstWinOpen()). */
extern void *g_lfb_shadow;

/* fbzMode bit 17: Y origin at the bottom (GR_ORIGIN_LOWER_LEFT); the
 * rasterizer then mirrors every line at v_disp - 1 (86Box handles the bit,
 * vid_voodoo_regs.h just does not name it). */
#define FBZ_Y_ORIGIN (1u << 17)

/* 2 MB per TMU, what grSstQueryHardware() reports (tmuRam = 2, a typical
 * Voodoo Graphics board). Power of two: g_voodoo.texture_mask derives from
 * it. */
#define TEX_MEM_BYTES (2 * 1024 * 1024)

/* State that is not a hardware register (software state in 3dfx's driver,
 * too). */
typedef struct {
    GrColorFormat_t color_format; /* from grSstWinOpen(), for GrColor_t values */
    GrCullMode_t    cull_mode;
    GrMipMapMode_t  mipmap_mode;
    FxU32           stw_hint; /* grHints(GR_HINT_STWHINT, ...) */
    FxU32           allow_lod_dither; /* grHints(GR_HINT_ALLOW_MIPMAP_DITHER, ...) */
    /* guColorCombineFunction(..._DELTA0): iterated colour held constant at
     * grConstantColorValue4 (r/g/b 0..255), gradients 0. */
    int             cc_delta0;
    float           delta0_r, delta0_g, delta0_b;
} glide_sw_state;

extern glide_sw_state g_sw;

/* grClipWindow in Glide coordinates, i.e. counted from the bottom with
 * GR_ORIGIN_LOWER_LEFT (the hardware clips before mirroring Y). */
extern int g_clip_left, g_clip_right, g_clip_top, g_clip_bottom;

/* grTriStats(): triangles passed to grDrawTriangle, and triangles actually
 * rasterized (neither culled nor degenerate). */
extern FxU32 g_tris_processed, g_tris_drawn;

/* Rasterize a triangle without culling (grDrawTriangle after its cull test;
 * grDrawLine/grDrawPoint, which Glide never culls). */
void glide_draw_triangle(const GrVertex *a, const GrVertex *b, const GrVertex *c);

/* grGammaCorrectionValue without trace entry; glide_set_default_gamma() for
 * grSstWinOpen (1.3, overridden by SST_GAMMA, core/glide_sst.c). */
void glide_set_gamma(float gamma);
void glide_set_default_gamma(void);

/* Multi-core rendering (core/render_mt.c): n >= 2 workers on other cores;
 * glide_mt_start() returns 0 if the platform cannot start them.
 * glide_mt_idle() waits until every queued triangle is drawn; call it
 * before anything that reads or writes the framebuffer or replaces a
 * texture-cache slot. */
int  glide_mt_start(int n);
void glide_mt_stop(void);
int  glide_mt_workers(void);
void glide_mt_submit(const voodoo_params_t *params);
void glide_mt_idle(void);

/* FSAA (core/fsaa.c): g_fsaa_n active planes (1 = off), g_fsaa_plane_bytes
 * apart in g_voodoo.fb_mem. glide_fsaa_read_cfg() before fb_mem is
 * allocated (it returns the number of planes to allocate),
 * glide_fsaa_open() afterwards. glide_fsaa_draw() replaces every
 * voodoo_triangle() call; slot selects the private params copy (worker
 * number for workers, 0 for the main core, GLIDE_MT_MAX for the main core
 * while workers may still run). glide_fsaa_output() returns the image to
 * show (resolved/post-processed buffer, or fb_mem when nothing applies). */
extern int      g_fsaa_n;
extern uint32_t g_fsaa_plane_bytes;
/* GLIDULATOR_FSAA=auto: called after every frame with its game+render time
 * in us; switches g_fsaa_n. Only call while no worker is drawing. */
int glide_fsaa_frame(uint64_t render_us);
/* For the OSD: FSAA=auto configured, AA_POST on */
int glide_fsaa_auto(void);
int glide_aa_post_enabled(void);
/* GLIDULATOR_AA_POST: bitmap of pixels written through the LFB (NULL =
 * off). glide_aa_mark() sets pixel i; glide_aa_unmark_rows() clears
 * x0..x1-1 in memory lines y0..y1-1 (grBufferClear). */
extern uint8_t *g_aa_mask;
void glide_aa_mark(size_t i);
void glide_aa_unmark_rows(int x0, int x1, int y0, int y1);
int             glide_fsaa_read_cfg(void);
int             glide_fsaa_open(int width, int height);
void            glide_fsaa_close(void);
void            glide_fsaa_draw(voodoo_params_t *params, int worker, int slot);
const uint16_t *glide_fsaa_output(void);

/* Trace: time spent in texture downloads and LFB writes (us), reported in
 * the performance line. Separates Glidulator's work from the game's own
 * loading in loading screens. */
#ifdef GLIDULATOR_TRACE
extern uint64_t g_perf_tex_us, g_perf_lfb_us;
#endif

/* Voodoo video filter (GLIDULATOR_VOODOO_FILTER, core/vfilter.c): open and
 * close together with the buffers (grSstWinOpen/WinClose).
 * glide_blit_output() shows an RGB565 image, filtered through
 * platform_blit32() when the filter is on, otherwise unchanged through
 * platform_blit(). */
int  glide_vfilter_open(int width, int height);
void glide_vfilter_close(void);
int  glide_vfilter_enabled(void);
void glide_blit_output(const uint16_t *fb);
/* The same, leaving rectangle x/y/w/h (the OSD box) unfiltered */
void glide_blit_output_keep(const uint16_t *fb, int x, int y, int w, int h);

/* Analog monitor emulation (core/crt.c), buffers opened/closed with the
 * others. glide_crt_apply(): image as the monitor shows it at sync
 * progress p (0 = just switched, 1 = locked; returns src from 1 on).
 * sync_in/sync_out play the effect on one image (blocking). */
#define GLIDE_CRT_IN_US 900000u
int             glide_crt_open(int width, int height);
void            glide_crt_close(void);
const uint16_t *glide_crt_apply(const uint16_t *src, float p);
void            glide_crt_sync_in(const uint16_t *fb);
void            glide_crt_sync_out(const uint16_t *fb);
/* Busy wait until start + t_us (fixed-rate animation loops). */
void            glide_wait_until(uint64_t start, uint64_t t_us);
/* sin/cos/sqrt without libm (the OVL has no C runtime) */
float glide_sinf(float x);
float glide_cosf(float x);
float glide_sqrtf(float x);

/* Start-up show (GLIDULATOR_SPLASH, GLIDULATOR_CRT; core/splash.c,
 * splash_orig.c).
 * glide_splash_crt(): monitor emulation on.
 * glide_splash_winopen(): end of grSstWinOpen, returns 1 if an animation
 * drew (the caller then restores Glide's initial state).
 * glide_splash_first_frame(): glide_present, before the frame is shown.
 * glide_splash_winclose(): grSstWinClose, before the display closes. */
void glide_splash_config(void);
int  glide_splash_crt(void);
int  glide_splash_winopen(void);
void glide_splash_first_frame(const uint16_t *fb);
void glide_splash_winclose(void);
void glide_splash_present(uint64_t t_us);
int  glide_splash_original(void);

/* 5x7 OSD font (core/osd.c): 7 rows, bit 4 = left column; NULL = blank. */
const uint8_t *glide_osd_glyph(char c);

/* Output with timing and OSD (GLIDULATOR_OSD, core/osd.c): open after
 * platform_display_open, close with the Voodoo buffers. glide_present()
 * shows the frame in grBufferSwap; vsync > 0 first waits for that many
 * vertical retraces (platform_wait_vretrace). */
void glide_osd_open(void);
void glide_osd_close(void);
void glide_present(int vsync);

/* Screenshot (core/screenshot.c). glide_screenshot_frame(): glide_present,
 * before the OSD (GLIDULATOR_SCREENSHOT=2: after it); saves fb as SHOTnnnn.BMP if platform_screenshot_key()
 * reports a key press. glide_screenshot_bmp(): w x h RGB565 -> 24-bit BMP
 * (bottom-up, rows padded to 4 bytes) through lut (NULL = none) into out;
 * returns the file size, out = NULL only computes it. */
void   glide_screenshot_frame(const uint16_t *fb);
size_t glide_screenshot_bmp(const uint16_t *fb, int w, int h, const uint8_t *lut, uint8_t *out);

/* Output gamma table last set by glide_set_gamma() (core/glide_sst.c),
 * NULL before the first one. */
const uint8_t *glide_gamma_lut(void);

/* grSstOrigin without trace entry (for grSstWinOpen). */
void glide_set_origin(GrOriginLocation_t origin);

/* Texture last selected with grTexSource (for grGlideGet/SetState);
 * valid = 0: none. glide_tex_set_source() does NOT recompute tLOD and
 * textureMode, they are restored from the register image anyway. */
void glide_tex_get_source(GrTexInfo *info, int *valid);
void glide_tex_read_cfg(void); /* GLIDULATOR_LOD_DITHER, read by grSstWinOpen */
void glide_tex_set_source(const GrTexInfo *info, int valid);

/* Round to nearest, halves away from zero. Not lrint()/llrint(): Open
 * Watcom's llrint() returns 0 for values beyond 32 bits (e.g. 3.6e10 -> 0),
 * which broke every texture coordinate (S/T/W x 2^32) in the OVL; the
 * double -> long long cast is correct.
 * Saturated at +-2^60 (NaN -> 0): some programs send nonsense for single
 * triangles (S = -inf, T = 1e9..inf). A cast outside the int64 range is
 * undefined in C, and values just below it overflow the gradient additions
 * in the rasterizer. Real values (S/T/W x 2^32) stay far below 2^60. */
#define GLIDE_FIX_LIMIT 1152921504606846976.0 /* 2^60 */
static inline int64_t glide_round64(double v) {
    if (!(v == v))
        return 0;
    if (v > GLIDE_FIX_LIMIT)
        v = GLIDE_FIX_LIMIT;
    if (v < -GLIDE_FIX_LIMIT)
        v = -GLIDE_FIX_LIMIT;
    return (int64_t) (v >= 0.0 ? v + 0.5 : v - 0.5);
}

static inline int32_t glide_round32(double v) {
    return (int32_t) glide_round64(v);
}

/* GrColor_t in the format chosen at grSstWinOpen() -> 0xAARRGGBB (hardware
 * format of color0/color1/chromaKey). */
uint32_t glide_color_to_argb(GrColor_t c);

/* Glide's initial state in g_voodoo.params/g_sw (after grSstWinOpen). */
void glide_modes_reset(void);
void glide_tex_reset(void);
void glide_lfb_reset(void);

/* Keep FBZCP_TEXTURE_ENABLED up to date: Glide enables the TMU only when
 * the colour or alpha combine actually uses the texture. */
void glide_update_texture_enable(void);

/* Unrounded bytes of LOD levels from..to-1 (from = larger level). */
FxU32 glide_tex_levels_bytes(GrLOD_t from, GrLOD_t to, GrAspectRatio_t aspect, GrTextureFormat_t fmt);

/* Fog off, colour/table 0 (grSstWinOpen). */
void glide_fog_reset(void);

/* Empty the guTex* allocator (grSstWinOpen, guTexMemReset). */
void glide_gutex_reset(void);

/* Bytes of a mipmap chain large_lod..small_lod (see glide_tex.c). */
FxU32 glide_tex_mem_required(GrLOD_t small_lod, GrLOD_t large_lod, GrAspectRatio_t aspect,
                             GrTextureFormat_t fmt, FxU32 even_odd);

#endif /* GLIDULATOR_GLIDE_INTERNAL_H */
