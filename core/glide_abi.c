/*
 * Glide entry points for initialisation, board queries, window open/close,
 * clear/swap, triangles, polygons and clipped polygons.
 *
 * The triangle setup (Y sorting, plane equations for all interpolated
 * values, fixed-point conversion) is Glidulator's own: 86Box receives
 * finished register writes from a real Glide driver and never has to
 * derive them. It produces exactly the setup registers the Voodoo's setup
 * unit would, then hands the triangle to the rasterizer (directly, or
 * through the worker queue in core/render_mt.c).
 */

#include "glidulator/glide2x.h"
#include "glidulator/trace.h"
#include "glidulator/capture.h"
#include "glidulator/glide_state.h"
#include "glidulator/platform.h"

#include "glide_internal.h"
#include "glidulator/renderer.h"

#include "vid_voodoo_render.h"
#include "vid_voodoo_texture.h"

#include <math.h>
#include <string.h>

struct glide_state {
    int core_count;
};

/* See core/glide_internal.h. */
voodoo_t   g_voodoo;
int        g_width, g_height;
int        g_win_open = 0;
static int g_inited   = 0;

/* Depth (aux) buffer directly after the colour buffer in fb_mem, as on the
 * real board (rasterizer: aux_mem = fb_mem + aux_offset). */
uint32_t g_aux_offset;
void    *g_lfb_shadow;

int   g_clip_left, g_clip_right, g_clip_top, g_clip_bottom;
FxU32 g_tris_processed, g_tris_drawn;

static GrErrorCallbackFnc_t g_error_callback = 0;

/* texture_cache[tmu][c].data must be non-NULL and large enough for all mip
 * levels of a 256x256 texture for EVERY slot c: voodoo_half_triangle()
 * computes &texture_cache[0][tex_entry[0]].data[texture_offset[c]]
 * unconditionally for all LOD levels, and voodoo_use_texture() increments
 * texture_last_removed before its first comparison, so even the first
 * upload lands in slot 1. All TEX_CACHE_MAX slots of TMU0 are allocated as
 * 86Box's voodoo_init() does; TMU1 is unused (no dual TMUs) but needs slot 0
 * for the same pointer arithmetic. */
#define TEX_CACHE_SLOT_WORDS (256 * 256 + 256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2)

/* Buffers a grLfbLock() pointer can point into (colour/depth buffer,
 * shadow buffer). On a Voodoo that pointer addresses the board and stays
 * valid forever, and some games keep writing through it after
 * grLfbUnlock() or even after grSstWinClose() (Uprising on exit). Freed
 * heap memory would then be overwritten, corrupting the heap the DLL shares
 * with the game, which crashes later in an unrelated HeapFree. So these
 * buffers survive grSstWinClose(), are reused by the next grSstWinOpen()
 * and only released by glide_shutdown() (DLL detach). */
typedef struct {
    void  *p;
    size_t size;
} kept_buf;

static kept_buf g_keep_fb, g_keep_shadow;

static void *keep_alloc(kept_buf *k, size_t n) {
    if (k->p && k->size >= n)
        return k->p;
    platform_free(k->p);
    k->p    = platform_alloc(n);
    k->size = k->p ? n : 0;
    return k->p;
}

glide_state *glide_init(void) {
    glide_state *gs = (glide_state *) platform_alloc(sizeof(glide_state));
    if (!gs)
        return NULL;
    gs->core_count = platform_core_count();
    grGlideInit();
    return gs;
}

void glide_shutdown(glide_state *gs) {
    platform_free(g_keep_fb.p);
    platform_free(g_keep_shadow.p);
    g_keep_fb.p = g_keep_shadow.p = NULL;
    g_keep_fb.size = g_keep_shadow.size = 0;
    platform_free(gs);
}

void GR_EXPORT GLIDE_API grGlideInit(void) {
    GLIDE_TRACE("grGlideInit");
#ifdef GLIDULATOR_DEBUG
    /* Code and data addresses: lets tools/dosvm/ovladdr.py map fault
     * addresses from the log to functions via the map file */
    glide_trace_note("Address grGlideInit", (unsigned long) (uintptr_t) grGlideInit, 0);
    glide_trace_note("Address g_voodoo (data)/size", (unsigned long) (uintptr_t) &g_voodoo, sizeof g_voodoo);
#endif
    if (g_inited)
        return;
    voodoo_init_color_tables();
    g_inited = 1;
}

/* "2.3": the version string the 3dfx retail runtime returns (documented by
 * MGA-Glide against the retail OVL). */
void GR_EXPORT GLIDE_API grGlideGetVersion(char *version) {
    GLIDE_TRACE("grGlideGetVersion");
    const char *v = "2.3";
    size_t      n = strlen(v);
    memcpy(version, v, n + 1);
}

static void fill_fake_hwconfig(GrHwConfiguration *hwconfig) {
    memset(hwconfig, 0, sizeof(*hwconfig));
    hwconfig->num_sst                                  = 1;
    hwconfig->SSTs[0].type                              = 0; /* GR_SSTTYPE_VOODOO, see abi/constants.tsv */
    hwconfig->SSTs[0].sstBoard.VoodooConfig.fbRam        = 4; /* MB, like a 4 MB Voodoo Graphics board */
    hwconfig->SSTs[0].sstBoard.VoodooConfig.fbiRev       = 1;
    hwconfig->SSTs[0].sstBoard.VoodooConfig.nTexelfx     = 1;
    hwconfig->SSTs[0].sstBoard.VoodooConfig.sliDetect    = FXFALSE;
    hwconfig->SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRev = 1;
    hwconfig->SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRam = 2;
}

/* Both report one Voodoo Graphics board: games call grSstQueryHardware()
 * right after grGlideInit() and quit immediately if no board is found. */
FxBool GR_EXPORT GLIDE_API grSstQueryHardware(GrHwConfiguration *hwconfig) {
    GLIDE_TRACE("grSstQueryHardware");
    if (hwconfig)
        fill_fake_hwconfig(hwconfig);
    return FXTRUE;
}

FxBool GR_EXPORT GLIDE_API grSstQueryBoards(GrHwConfiguration *hwconfig) {
    GLIDE_TRACE("grSstQueryBoards");
    if (hwconfig)
        fill_fake_hwconfig(hwconfig);
    return FXTRUE;
}

void GR_EXPORT GLIDE_API grSstSelect(int which_sst) {
    GLIDE_TRACE("grSstSelect");
    (void) which_sst; /* one board only, nothing to select */
    platform_prepare_window();
}

void GR_EXPORT GLIDE_API grSstIdle(void) {
    GLIDE_TRACE("grSstIdle");
    /* No-op: Glide callers use it before touching the framebuffer, and every
     * such path (LFB, clear, swap) already waits for the render workers. */
}

void GR_EXPORT GLIDE_API grRenderBuffer(GrBuffer_t buffer) {
    GLIDE_TRACE("grRenderBuffer");
    (void) buffer; /* there is a single framebuffer, see grSstWinOpen() */
}

void GR_EXPORT GLIDE_API grErrorSetCallback(GrErrorCallbackFnc_t fnc) {
    GLIDE_TRACE("grErrorSetCallback");
    g_error_callback = fnc; /* stored only: Glidulator reports no errors through it */
}

void GR_EXPORT GLIDE_API grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx, FxU32 maxy) {
    GLIDE_TRACE("grClipWindow");
#ifdef GLIDULATOR_DEBUG
    {
        static int logged;
        if (logged++ < 20) {
            glide_trace_note("grClipWindow min x/y", (unsigned long) minx, (unsigned long) miny);
            glide_trace_note("  max x/y", (unsigned long) maxx, (unsigned long) maxy);
        }
    }
#endif
    g_clip_left   = (int) minx;
    g_clip_top    = (int) miny;
    g_clip_right  = (int) maxx;
    g_clip_bottom = (int) maxy;
}

static void free_voodoo_resources(void) {
    glide_mt_stop(); /* no worker may draw into freed buffers */
    glide_osd_close();
    g_lfb_shadow    = NULL; /* both stay allocated, see kept_buf */
    g_voodoo.fb_mem = NULL;
    glide_fsaa_close();
    glide_vfilter_close();
    glide_crt_close();
    platform_free(g_voodoo.tex_mem[0]);
    for (int c = 0; c < TEX_CACHE_MAX; c++)
        platform_free(g_voodoo.texture_cache[0][c].data);
    platform_free(g_voodoo.texture_cache[1][0].data);
}

/* Glide state right after grSstWinOpen (also restored after the start-up
 * animation). */
static void initial_state(GrOriginLocation_t origin) {
    g_clip_left = g_clip_top = 0;
    g_clip_right  = g_width;
    g_clip_bottom = g_height;
    glide_modes_reset();
    glide_tex_reset();
    glide_lfb_reset();
    glide_gutex_reset();
    glide_fog_reset();
    glide_set_origin(origin); /* no trace entry: not an application call */
    g_tris_processed = g_tris_drawn = 0;
}

void GR_EXPORT GLIDE_API grSstWinClose(void) {
    GLIDE_TRACE("grSstWinClose");
    if (g_win_open) {
        glide_splash_winclose(); /* monitor emulation, before the switch back */
        platform_display_close();
        free_voodoo_resources();
        g_win_open = 0;
    }
}

void GR_EXPORT GLIDE_API grGlideShutdown(void) {
    GLIDE_TRACE("grGlideShutdown");
#ifdef GLIDULATOR_TRACE
    glide_trace_dump("grGlideShutdown");
#endif
    grSstWinClose();
    g_inited = 0;
}

/* GR_RESOLUTION_* -> pixel size. Only the resolutions a Voodoo Graphics
 * actually supports; anything else fails grSstWinOpen(). */
static int resolution_to_size(GrScreenResolution_t res, int *w, int *h) {
    switch (res) {
    case GR_RESOLUTION_320x200: *w = 320; *h = 200; return 1;
    case GR_RESOLUTION_320x240: *w = 320; *h = 240; return 1;
    case GR_RESOLUTION_400x256: *w = 400; *h = 256; return 1;
    case GR_RESOLUTION_512x384: *w = 512; *h = 384; return 1;
    case GR_RESOLUTION_640x400: *w = 640; *h = 400; return 1;
    case GR_RESOLUTION_640x480: *w = 640; *h = 480; return 1;
    case GR_RESOLUTION_800x600: *w = 800; *h = 600; return 1;
    default: return 0;
    }
}

/* A failing grSstWinOpen() usually makes games quit silently, so the trace
 * variants log the reason. */
static FxBool win_open_failed(const char *why) {
#ifdef GLIDULATOR_TRACE
    platform_log("grSstWinOpen failed: ");
    platform_log(why);
    platform_log("\n");
#else
    (void) why;
#endif
    return FXFALSE;
}

static void read_vsync_cfg(void); /* see grBufferSwap */

FxBool GR_EXPORT GLIDE_API grSstWinOpen(FxU32 hWnd, GrScreenResolution_t screen_resolution,
                                        GrScreenRefresh_t refresh_rate, GrColorFormat_t color_format,
                                        GrOriginLocation_t origin_location, int nColBuffers, int nAuxBuffers) {
    GLIDE_TRACE("grSstWinOpen");
    (void) hWnd;           /* output goes to Glidulator's own window/screen, see platform_blit() */
    (void) refresh_rate;   /* the display decides the refresh rate */
    /* color_format applies to GrColor_t arguments only (grBufferClear,
     * grConstantColorValue, ...); rendering is always RGB565. */
    (void) nColBuffers;    /* single colour buffer, presented at grBufferSwap() */
    (void) nAuxBuffers;    /* a depth buffer always exists */

    int width, height;
    if (!resolution_to_size(screen_resolution, &width, &height))
        return win_open_failed("resolution not supported");
#ifdef GLIDULATOR_TRACE
    glide_trace_note("grSstWinOpen", (unsigned long) width, (unsigned long) height);
#endif

    if (g_win_open) {
        /* Mark closed first: if reopening fails below, later Glide calls
         * must not touch the buffers freed here. */
        free_voodoo_resources();
        g_win_open = 0;
    }

    memset(&g_voodoo, 0, sizeof(g_voodoo));
    g_voodoo.type   = VOODOO_1;
    g_voodoo.v_disp = height;
    glide_renderer_apply(); /* render path and threads, core/renderer.c */

    size_t row_bytes = (size_t) width * 2; /* RGB565 */
    size_t fb_needed = row_bytes * (size_t) height * 2; /* colour + depth (16 bit each) */
    fb_needed *= (size_t) glide_fsaa_read_cfg();        /* FSAA: both per plane, core/fsaa.c */
    size_t fb_size   = 1;
    while (fb_size < fb_needed)
        fb_size <<= 1; /* fb_mask needs a power of two */

    g_voodoo.fb_mem = (uint8_t *) keep_alloc(&g_keep_fb, fb_size);
    if (!g_voodoo.fb_mem)
        return win_open_failed("out of memory");
    memset(g_voodoo.fb_mem, 0, fb_size);
    g_voodoo.fb_size = (int) fb_size;
    g_voodoo.fb_mask = (uint32_t) (fb_size - 1);
    glide_splash_config();
    if (!glide_fsaa_open(width, height) || !glide_vfilter_open(width, height) ||
        (glide_splash_crt() && !glide_crt_open(width, height))) {
        free_voodoo_resources();
        return win_open_failed("out of memory");
    }

    for (int c = 0; c < TEX_CACHE_MAX; c++) {
        g_voodoo.texture_cache[0][c].data = (uint32_t *) platform_alloc(TEX_CACHE_SLOT_WORDS * sizeof(uint32_t));
        if (!g_voodoo.texture_cache[0][c].data) {
            free_voodoo_resources();
            return win_open_failed("out of memory");
        }
        memset(g_voodoo.texture_cache[0][c].data, 0, TEX_CACHE_SLOT_WORDS * sizeof(uint32_t));
    }
    g_voodoo.texture_cache[1][0].data = (uint32_t *) platform_alloc(TEX_CACHE_SLOT_WORDS * sizeof(uint32_t));
    if (!g_voodoo.texture_cache[1][0].data) {
        free_voodoo_resources();
        return win_open_failed("out of memory");
    }
    memset(g_voodoo.texture_cache[1][0].data, 0, TEX_CACHE_SLOT_WORDS * sizeof(uint32_t));
    /* base = -1 marks a free slot in 86Box (voodoo_init()); a zero base would
     * pretend a real texture at address 0. */
    for (int c = 0; c < TEX_CACHE_MAX; c++)
        g_voodoo.texture_cache[0][c].base = (uint32_t) -1;
    g_voodoo.texture_cache[1][0].base = (uint32_t) -1;

    g_voodoo.tex_mem[0] = (uint8_t *) platform_alloc(TEX_MEM_BYTES);
    if (!g_voodoo.tex_mem[0]) {
        free_voodoo_resources();
        return win_open_failed("out of memory");
    }
    memset(g_voodoo.tex_mem[0], 0, TEX_MEM_BYTES);
    g_voodoo.texture_mask = TEX_MEM_BYTES - 1;

    g_lfb_shadow = keep_alloc(&g_keep_shadow, (size_t) width * (size_t) height * 4);
    if (!g_lfb_shadow) {
        free_voodoo_resources();
        return win_open_failed("out of memory");
    }

    g_width   = width;
    g_height  = height;
    g_clip_left = g_clip_top = 0;
    g_clip_right = width;
    g_clip_bottom = height;
    g_aux_offset  = (uint32_t) (row_bytes * (size_t) height);
    g_sw.color_format = color_format;
    initial_state(origin_location);
    g_win_open = 1;
    glide_set_default_gamma();         /* as 3dfx's grSstWinOpen does, see glide_sst.c */
    platform_display_prefer_deep(glide_vfilter_enabled());
    platform_display_open(width, height);
    glide_osd_open(); /* after the display: needs its size */
    read_vsync_cfg();
    /* Start-up animation (GLIDULATOR_SPLASH); it uses Glide like a
     * program, so the program then gets the initial state again (the
     * saved state also covers what initial_state() leaves alone, e.g. the
     * last texture source). */
    GrState before;
    g_glide_mute++;
    grGlideGetState(&before);
    g_glide_mute--;
    if (glide_splash_winopen()) {
        glide_mt_idle();
        memset(g_voodoo.fb_mem, 0, (size_t) g_voodoo.fb_size);
        memset(g_voodoo.tex_mem[0], 0, TEX_MEM_BYTES);
        for (int c = 0; c < TEX_CACHE_MAX; c++)
            g_voodoo.texture_cache[0][c].base = (uint32_t) -1;
        initial_state(origin_location);
        g_glide_mute++;
        grGlideSetState(&before);
        g_glide_mute--;
    }
    return FXTRUE;
}

/* Like the board's fastfill unit: only inside the clip window, colour only
 * with the RGB write mask set, depth only with the depth write mask set
 * (grDepthMask). There is no alpha buffer. */
void GR_EXPORT GLIDE_API grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU16 depth) {
    GLIDE_TRACE("grBufferClear");
    glide_mt_idle();
    (void) alpha;
    if (!g_win_open)
        return;
    uint32_t argb   = glide_color_to_argb(color);
#ifdef GLIDULATOR_DEBUG
    glide_capture_clear(argb, depth);
#endif
    uint16_t rgb565 = (uint16_t) ((((argb >> 16) & 0xf8) << 8) | (((argb >> 8) & 0xfc) << 3) | ((argb & 0xff) >> 3));
    int      clear_rgb   = (g_voodoo.params.fbzMode & FBZ_RGB_WMASK) != 0;
    int      clear_depth = (g_voodoo.params.fbzMode & FBZ_DEPTH_WMASK) != 0;

    int x0 = g_clip_left < 0 ? 0 : g_clip_left;
    int y0 = g_clip_top < 0 ? 0 : g_clip_top;
    int x1 = g_clip_right > g_width ? g_width : g_clip_right;
    int y1 = g_clip_bottom > g_height ? g_height : g_clip_bottom;

    /* The clip window is in Glide coordinates; with GR_ORIGIN_LOWER_LEFT row 0
     * is at the bottom (as in the rasterizer: y_origin - y). */
    int flip = (g_voodoo.params.fbzMode & FBZ_Y_ORIGIN) != 0;

    for (int k = 0; k < g_fsaa_n; k++) { /* FSAA: every plane */
        uint16_t *col = (uint16_t *) (g_voodoo.fb_mem + (size_t) k * g_fsaa_plane_bytes);
        uint16_t *aux = (uint16_t *) (g_voodoo.fb_mem + (size_t) k * g_fsaa_plane_bytes + g_aux_offset);
        for (int y = y0; y < y1; y++) {
            int row = (flip ? g_height - 1 - y : y) * g_width;
            for (int x = x0; x < x1; x++) {
                if (clear_rgb)
                    col[row + x] = rgb565;
                if (clear_depth)
                    aux[row + x] = depth;
            }
            if (k == 0 && clear_rgb && g_aa_mask) { /* LFB mask (AA_POST) */
                int r = flip ? g_height - 1 - y : y;
                glide_aa_unmark_rows(x0, x1, r, r + 1);
            }
        }
    }
}

/* Vsync (GLIDULATOR_VSYNC, read at grSstWinOpen): -1 auto = as the game
 * requests (swap_interval), 0 never, 1 at least one retrace. As on the
 * Voodoo, this paces games that advance their logic per frame. Without
 * page flipping the picture can still tear. */
static int g_vsync_cfg = -1;

static void read_vsync_cfg(void) {
    char cfg[8];
    g_vsync_cfg = -1;
    if (platform_get_config("GLIDULATOR_VSYNC", cfg, sizeof cfg) && (cfg[0] == '0' || cfg[0] == '1') && !cfg[1])
        g_vsync_cfg = cfg[0] - '0';
}

void GR_EXPORT GLIDE_API grBufferSwap(int swap_interval) {
    GLIDE_TRACE("grBufferSwap");
    if (!g_win_open)
        return;
    int vsync = swap_interval < 0 ? 0 : swap_interval > 4 ? 4 : swap_interval;
    if (g_vsync_cfg == 0)
        vsync = 0;
    else if (g_vsync_cfg == 1 && !vsync)
        vsync = 1;
#ifdef GLIDULATOR_TRACE
    {
        static int last = -1000;
        if (swap_interval != last) /* log changes only */
            glide_trace_note("grBufferSwap swap_interval/Vsync", (unsigned long) swap_interval, (unsigned long) vsync);
        last = swap_interval;
    }
#endif
    glide_mt_idle(); /* every triangle of the frame is drawn */
    glide_present(vsync); /* vsync, image options, OSD, output (core/osd.c) */
#ifdef GLIDULATOR_TRACE
    glide_trace_swap();
#endif
#ifdef GLIDULATOR_DEBUG
    glide_capture_swap();
#endif
}

int GR_EXPORT GLIDE_API glidulatorDebugPeekPixel(int x, int y) {
    if (!g_win_open || x < 0 || y < 0 || x >= g_width || y >= g_height)
        return -1;
    glide_mt_idle();
    const uint16_t *px = (const uint16_t *) g_voodoo.fb_mem;
    return (int) px[y * g_width + x];
}

/* --- Triangle setup ----------------------------------------------- */

/* Fixed-point formats of the triangle setup registers, as the board stores
 * float writes (fstartR, fstartZ, fstartS, ...) and the rasterizer expects:
 *   colours  12.12  (0..255 -> * 4096; rasterizer: ir >> 12)
 *   Z        20.12  (Glide ooz 0..65535 -> * 4096; depth = z >> 12)
 *   W, S, T  x 2^32 (VOODOO_STW_FIXED_POINT_ONE; per pixel the rasterizer
 *                    computes tex_s = (S/W) * 16, see voodoo_tmu_fetch)
 * S/T are Glide texture coordinates: 0..256 along the long edge of the
 * texture REGARDLESS of its size; the hardware scales through the LOD
 * level. W is 1/w (oow); the rasterizer does the perspective division. */
#define FIX_COLOR 4096.0
#define FIX_Z     4096.0
#define FIX_STW   4294967296.0

/* Screen coordinates in x.4 fixed point (cf. vertexAx & 0xf in
 * voodoo_triangle()). */
static int32_t to_screen_fixed(float v) {
    return glide_round32((double) v * 16.0);
}

/* Geometry for the gradients: the corners already rounded to 1/16 pixel
 * (exactly the values the rasterizer walks the edges with), A = top. */
typedef struct {
    double ax, ay, bx, by, cx, cy;
    double inv_area2;
} tri_geom;

/* Plane through the three corner values: value at A and derivatives in x
 * and y, scaled to the register format. */
typedef struct {
    double start, ddx, ddy;
} plane;

static plane plane_of(const tri_geom *g, double va, double vb, double vc, double scale) {
    double db = vb - va, dc = vc - va;
    plane  p;
    p.start = va * scale;
    p.ddx   = (db * (g->cy - g->ay) - dc * (g->by - g->ay)) * g->inv_area2 * scale;
    p.ddy   = (dc * (g->bx - g->ax) - db * (g->cx - g->ax)) * g->inv_area2 * scale;
    return p;
}

static void set32(plane p, uint32_t *start, int32_t *ddx, int32_t *ddy) {
    *start = (uint32_t) glide_round32(p.start);
    *ddx   = glide_round32(p.ddx);
    *ddy   = glide_round32(p.ddy);
}

static void set64(plane p, int64_t *start, int64_t *ddx, int64_t *ddy) {
    *start = glide_round64(p.start);
    *ddx   = glide_round64(p.ddx);
    *ddy   = glide_round64(p.ddy);
}

/* Culling as in Glide (in software, with the application's vertex order):
 * area = dxAB*dyBC - dxBC*dyAB; GR_CULL_NEGATIVE rejects negative,
 * GR_CULL_POSITIVE non-negative area. */
static int culled(const GrVertex *a, const GrVertex *b, const GrVertex *c) {
    if (g_sw.cull_mode == GR_CULL_DISABLE)
        return 0;
    float area = (a->x - b->x) * (b->y - c->y) - (b->x - c->x) * (a->y - b->y);
    return (g_sw.cull_mode == GR_CULL_NEGATIVE) ? (area < 0.0f) : (area >= 0.0f);
}

void GR_EXPORT GLIDE_API grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c) {
    GLIDE_TRACE("grDrawTriangle");
    g_tris_processed++;
    if (!culled(a, b, c))
        glide_draw_triangle(a, b, c);
}

void glide_draw_triangle(const GrVertex *in_a, const GrVertex *in_b, const GrVertex *in_c) {
    if (!g_win_open)
        return;
#ifdef GLIDULATOR_DEBUG
    {
        int clip[4];
        clip[0] = g_clip_left;
        clip[1] = g_clip_right;
        clip[2] = g_clip_top;
        clip[3] = g_clip_bottom;
        glide_capture_triangle(in_a, in_b, in_c, clip);
    }
#endif

    /* Sort by Y: a = top, b = middle, c = bottom, as voodoo_triangle()
     * requires (AB then BC is the "short" side against the "long" side AC).
     * Assigned one by one: Open Watcom requires constant aggregate
     * initialisers. */
    const GrVertex *v[3];
    v[0] = in_a;
    v[1] = in_b;
    v[2] = in_c;
    if (v[0]->y > v[1]->y) { const GrVertex *t = v[0]; v[0] = v[1]; v[1] = t; }
    if (v[1]->y > v[2]->y) { const GrVertex *t = v[1]; v[1] = v[2]; v[2] = t; }
    if (v[0]->y > v[1]->y) { const GrVertex *t = v[0]; v[0] = v[1]; v[1] = t; }
    const GrVertex *a = v[0], *b = v[1], *c = v[2];

    /* Copy of the register file (core/glide_internal.h) plus the
     * per-triangle values. */
    voodoo_params_t params = g_voodoo.params;

    params.vertexAx = to_screen_fixed(a->x);
    params.vertexAy = to_screen_fixed(a->y);
    params.vertexBx = to_screen_fixed(b->x);
    params.vertexBy = to_screen_fixed(b->y);
    params.vertexCx = to_screen_fixed(c->x);
    params.vertexCy = to_screen_fixed(c->y);

    tri_geom g;
    g.ax = params.vertexAx / 16.0;
    g.ay = params.vertexAy / 16.0;
    g.bx = params.vertexBx / 16.0;
    g.by = params.vertexBy / 16.0;
    g.cx = params.vertexCx / 16.0;
    g.cy = params.vertexCy / 16.0;
    double area2 = (g.bx - g.ax) * (g.cy - g.ay) - (g.cx - g.ax) * (g.by - g.ay);
    if (fabs(area2) < 1e-9)
        return; /* degenerate after rounding to 1/16 pixel */
    g.inv_area2 = 1.0 / area2;
    g_tris_drawn++;

    /* sign selects whether edge AC lies left (sign=0) or right (sign=1) of
     * AB/BC. B left of AC means (Y down) negative area, e.g. A=(0,0),
     * B=(-5,5), C=(0,10): area2 = -50. */
    params.sign = (area2 < 0.0) ? 1 : 0;

    if (g_sw.cc_delta0) {
        /* DELTA0: constant "iterated" colour from grConstantColorValue4. */
        set32(plane_of(&g, g_sw.delta0_r, g_sw.delta0_r, g_sw.delta0_r, FIX_COLOR), &params.startR, &params.dRdX,
              &params.dRdY);
        set32(plane_of(&g, g_sw.delta0_g, g_sw.delta0_g, g_sw.delta0_g, FIX_COLOR), &params.startG, &params.dGdX,
              &params.dGdY);
        set32(plane_of(&g, g_sw.delta0_b, g_sw.delta0_b, g_sw.delta0_b, FIX_COLOR), &params.startB, &params.dBdX,
              &params.dBdY);
    } else {
        set32(plane_of(&g, a->r, b->r, c->r, FIX_COLOR), &params.startR, &params.dRdX, &params.dRdY);
        set32(plane_of(&g, a->g, b->g, c->g, FIX_COLOR), &params.startG, &params.dGdX, &params.dGdY);
        set32(plane_of(&g, a->b, b->b, c->b, FIX_COLOR), &params.startB, &params.dBdX, &params.dBdY);
    }
    set32(plane_of(&g, a->a, b->a, c->a, FIX_COLOR), &params.startA, &params.dAdX, &params.dAdY);
    set32(plane_of(&g, a->ooz, b->ooz, c->ooz, FIX_Z), &params.startZ, &params.dZdX, &params.dZdY);
    set64(plane_of(&g, a->oow, b->oow, c->oow, FIX_STW), &params.startW, &params.dWdX, &params.dWdY);

#ifdef GLIDULATOR_DEBUG
    {
        /* Log the first textured triangles with their vertex data
         * (x, y, oow, sow, tow, tmu oow, s=sow/oow, t=tow/oow). */
        static int logged;
        if ((params.fbzColorPath & FBZCP_TEXTURE_ENABLED) && logged < 40) {
            logged++;
            glide_trace_note("Triangle tLOD/texBase", (unsigned long) params.tLOD[0], (unsigned long) params.texBaseAddr[0]);
            const GrVertex *tv[3];
            tv[0] = a; /* one by one: Watcom requires constant initialisers */
            tv[1] = b;
            tv[2] = c;
            for (int k = 0; k < 3; k++) {
                float f[8];
                f[0] = tv[k]->x; f[1] = tv[k]->y; f[2] = tv[k]->oow;
                f[3] = tv[k]->tmuvtx[0].sow; f[4] = tv[k]->tmuvtx[0].tow; f[5] = tv[k]->tmuvtx[0].oow;
                f[6] = tv[k]->oow != 0.0f ? tv[k]->tmuvtx[0].sow / tv[k]->oow : 0.0f;
                f[7] = tv[k]->oow != 0.0f ? tv[k]->tmuvtx[0].tow / tv[k]->oow : 0.0f;
                glide_trace_floats("  v", f, 8);
            }
        }
    }
#endif
    if (params.fbzColorPath & FBZCP_TEXTURE_ENABLED) {
        /* The TMU's W is the vertex oow, unless the hint
         * GR_STWHINT_W_DIFF_TMU0 selects tmuvtx[0].oow. */
        int w_own = (g_sw.stw_hint & GR_STWHINT_W_DIFF_TMU0) != 0;
        double wa = w_own ? a->tmuvtx[0].oow : a->oow;
        double wb = w_own ? b->tmuvtx[0].oow : b->oow;
        double wc = w_own ? c->tmuvtx[0].oow : c->oow;
        set64(plane_of(&g, a->tmuvtx[0].sow, b->tmuvtx[0].sow, c->tmuvtx[0].sow, FIX_STW),
              &params.tmu[0].startS, &params.tmu[0].dSdX, &params.tmu[0].dSdY);
        set64(plane_of(&g, a->tmuvtx[0].tow, b->tmuvtx[0].tow, c->tmuvtx[0].tow, FIX_STW),
              &params.tmu[0].startT, &params.tmu[0].dTdX, &params.tmu[0].dTdY);
        set64(plane_of(&g, wa, wb, wc, FIX_STW), &params.tmu[0].startW, &params.tmu[0].dWdX, &params.tmu[0].dWdY);
    }

    params.clipLeft  = g_clip_left;
    params.clipRight = g_clip_right;
    params.clipLowY  = g_clip_top;
    params.clipHighY = g_clip_bottom;

    params.draw_offset   = 0;
    params.aux_offset    = g_aux_offset;
    params.row_width     = g_width * 2;
    params.aux_row_width = g_width * 2;
    params.col_tiled     = 0;
    params.aux_tiled     = 0;

    /* voodoo_use_texture() and parts of voodoo_triangle() also read the
     * params embedded in voodoo_t (the same object in 86Box, see
     * core/rasterizer/ATTRIBUTION.md), so sync it before the call. */
    g_voodoo.params = params;
    if (params.fbzColorPath & FBZCP_TEXTURE_ENABLED) {
        voodoo_use_texture(&g_voodoo, &params, 0);
        g_voodoo.params.tex_entry[0] = params.tex_entry[0];
    } else
        g_voodoo.texture_cache[0][params.tex_entry[0]].refcount++;
    /* voodoo_half_triangle() increments refcount_r[worker] for BOTH TMUs at
     * its end; refcount must rise by the same amount per triangle, or the
     * cache slot counts as in use forever (voodoo_texture_in_use). TMU0
     * without texture is handled above, TMU1 here. */
    g_voodoo.texture_cache[1][params.tex_entry[1]].refcount++;
    /* FSAA: voodoo_triangle() runs once per plane and increments refcount_r
     * every time, so refcount rises N times as well. */
    if (g_fsaa_n > 1) {
        g_voodoo.texture_cache[0][params.tex_entry[0]].refcount += g_fsaa_n - 1;
        g_voodoo.texture_cache[1][params.tex_entry[1]].refcount += g_fsaa_n - 1;
    }
    if (glide_mt_workers())
        glide_mt_submit(&params); /* workers draw, core/render_mt.c */
    else
        glide_fsaa_draw(&params, 0, 0);
}

/* Convex polygons as a triangle fan around the first vertex, as 3dfx's
 * Glide does (gdraw.c). "Planar" only means the application guarantees a
 * common plane; it makes no difference here. */
void GR_EXPORT GLIDE_API grDrawPolygon(int nverts, const int *ilist, const GrVertex *vlist) {
    GLIDE_TRACE("grDrawPolygon");
    for (int i = 1; i + 1 < nverts; i++)
        grDrawTriangle(&vlist[ilist[0]], &vlist[ilist[i]], &vlist[ilist[i + 1]]);
}

void GR_EXPORT GLIDE_API grDrawPolygonVertexList(int nverts, const GrVertex *vlist) {
    GLIDE_TRACE("grDrawPolygonVertexList");
    for (int i = 1; i + 1 < nverts; i++)
        grDrawTriangle(&vlist[0], &vlist[i], &vlist[i + 1]);
}

void GR_EXPORT GLIDE_API grDrawPlanarPolygon(int nverts, const int *ilist, const GrVertex *vlist) {
    GLIDE_TRACE("grDrawPlanarPolygon");
    grDrawPolygon(nverts, ilist, vlist);
}

void GR_EXPORT GLIDE_API grDrawPlanarPolygonVertexList(int nverts, const GrVertex *vlist) {
    GLIDE_TRACE("grDrawPlanarPolygonVertexList");
    grDrawPolygonVertexList(nverts, vlist);
}

/* --- gu*WithClip: clip polygons against the clip window -------------
 *
 * The Voodoo takes vertices as 16-bit fixed point (12.4, i.e. only
 * -2048..+2047 pixels) and drops the upper bits, as the rasterizer does.
 * A triangle with corners far off screen (typical for nearby walls and
 * floors) therefore arrives mangled. That is why Glide has the
 * gu*WithClip variants: they clip the polygon in software against the
 * clip window (Sutherland-Hodgman, edge by edge) before drawing. All vertex
 * values are interpolated linearly in screen space, which is correct
 * because Glide receives screen-linear quantities anyway (1/w, s/w, t/w,
 * z/w, colours). */

#define CLIP_MAX_VERTS 108 /* up to 100 input corners + at most one more per edge */

static GrVertex lerp_vertex(const GrVertex *a, const GrVertex *b, float t) {
    GrVertex r;
    r.x   = a->x + t * (b->x - a->x);
    r.y   = a->y + t * (b->y - a->y);
    r.z   = a->z + t * (b->z - a->z);
    r.r   = a->r + t * (b->r - a->r);
    r.g   = a->g + t * (b->g - a->g);
    r.b   = a->b + t * (b->b - a->b);
    r.ooz = a->ooz + t * (b->ooz - a->ooz);
    r.a   = a->a + t * (b->a - a->a);
    r.oow = a->oow + t * (b->oow - a->oow);
    for (int k = 0; k < GLIDE_NUM_TMU; k++) {
        r.tmuvtx[k].sow = a->tmuvtx[k].sow + t * (b->tmuvtx[k].sow - a->tmuvtx[k].sow);
        r.tmuvtx[k].tow = a->tmuvtx[k].tow + t * (b->tmuvtx[k].tow - a->tmuvtx[k].tow);
        r.tmuvtx[k].oow = a->tmuvtx[k].oow + t * (b->tmuvtx[k].oow - a->tmuvtx[k].oow);
    }
    return r;
}

/* Distance of a point to an edge, >= 0 = inside. Edges 0..3 = left, right,
 * top, bottom of the clip window. */
static float clip_dist(const GrVertex *v, int edge) {
    switch (edge) {
    case 0: return v->x - (float) g_clip_left;
    case 1: return (float) g_clip_right - v->x;
    case 2: return v->y - (float) g_clip_top;
    default: return (float) g_clip_bottom - v->y;
    }
}

/* Clip a polygon against one edge; returns the new corner count. */
static int clip_edge(const GrVertex *in, int n, GrVertex *out, int edge) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const GrVertex *p  = &in[i];
        const GrVertex *q  = &in[(i + 1) % n];
        float           dp = clip_dist(p, edge), dq = clip_dist(q, edge);
        if (dp >= 0.0f)
            out[m++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f))
            out[m++] = lerp_vertex(p, q, dp / (dp - dq)); /* intersection */
    }
    return m;
}

static void draw_clipped(int nverts, const GrVertex *vlist) {
    static GrVertex buf[2][CLIP_MAX_VERTS]; /* static: the game's stack is small */
    if (nverts < 3)
        return;
    int inside = 1;
    for (int i = 0; i < nverts && inside; i++)
        inside = vlist[i].x >= (float) g_clip_left && vlist[i].x < (float) g_clip_right &&
                 vlist[i].y >= (float) g_clip_top && vlist[i].y < (float) g_clip_bottom;
    if (inside || nverts > CLIP_MAX_VERTS - 8) {
        grDrawPolygonVertexList(nverts, vlist); /* nothing to clip (or too large) */
        return;
    }
    int n = clip_edge(vlist, nverts, buf[0], 0);
    n     = clip_edge(buf[0], n, buf[1], 1);
    n     = clip_edge(buf[1], n, buf[0], 2);
    n     = clip_edge(buf[0], n, buf[1], 3);
    if (n >= 3)
        grDrawPolygonVertexList(n, buf[1]);
}

void GR_EXPORT GLIDE_API guDrawTriangleWithClip(const GrVertex *a, const GrVertex *b, const GrVertex *c) {
    GLIDE_TRACE("guDrawTriangleWithClip");
    GrVertex tri[3];
    tri[0] = *a; /* one by one: Watcom requires constant initialisers */
    tri[1] = *b;
    tri[2] = *c;
    draw_clipped(3, tri);
}

void GR_EXPORT GLIDE_API guDrawPolygonVertexListWithClip(int nverts, const GrVertex *vlist) {
    GLIDE_TRACE("guDrawPolygonVertexListWithClip");
    draw_clipped(nverts, vlist);
}
