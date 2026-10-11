/*
 * Board status and queries (grSstScreenWidth/Height, grSstStatus,
 * grSstIsBusy, grSstVRetraceOn, grSstVideoLine, grBufferNumPending), Y origin
 * (grSstOrigin), state save/restore (grGlideGetState/SetState),
 * grDisableAllEffects, statistics (grTriStats/grSstPerfStats) and
 * lines/points (grDrawLine/grDrawPoint).
 *
 * There is no asynchronous hardware: the "board" is never busy and no
 * buffer swap is ever pending. Games still poll these and wait in loops,
 * so the answers must report "done", and the vertical retrace must
 * toggle, or a game waiting for retrace would hang forever.
 */

#include "glide_internal.h"
#include "glidulator/trace.h"

#include "vid_voodoo_texture.h"

#include "glidulator/platform.h"

#include <math.h>
#include <string.h>

FxU32 GR_EXPORT GLIDE_API grSstScreenWidth(void) {
    GLIDE_TRACE("grSstScreenWidth");
    return g_win_open ? (FxU32) g_width : 0;
}

FxU32 GR_EXPORT GLIDE_API grSstScreenHeight(void) {
    GLIDE_TRACE("grSstScreenHeight");
    return g_win_open ? (FxU32) g_height : 0;
}

/* No real display scan: the retrace flag toggles with every query, so both
 * "while (!grSstVRetraceOn());" and the following
 * "while (grSstVRetraceOn());" end after one call. */
static FxU32 g_vretrace_toggle;

static int next_vretrace(void) {
    return (int) (++g_vretrace_toggle & 1);
}

FxBool GR_EXPORT GLIDE_API grSstVRetraceOn(void) {
    GLIDE_TRACE("grSstVRetraceOn");
    return next_vretrace() ? FXTRUE : FXFALSE;
}

/* Current scanline: advances with every query (like a moving beam) so
 * loops waiting for a line terminate. */
FxU32 GR_EXPORT GLIDE_API grSstVideoLine(void) {
    GLIDE_TRACE("grSstVideoLine");
    static FxU32 line;
    FxU32        lines = g_win_open ? (FxU32) g_height : 480;
    line = (line + 1) % lines;
    return line;
}

/* Voodoo Graphics status register (SST-1 status): bits 0..5 free PCI FIFO
 * entries, bit 6 = 1 outside vertical retrace, bits 7..9 FBI/TREX/SST busy,
 * bits 10..11 displayed buffer, bits 12..27 free memory FIFO entries, bits
 * 28..30 pending buffer swaps. Reported: FIFOs empty, nothing busy,
 * nothing pending. */
#define SST_FIFO_FREE    0x3fu
#define SST_NO_VRETRACE  (1u << 6)
#define SST_MEMFIFO_FREE (0xffffu << 12)

FxU32 GR_EXPORT GLIDE_API grSstStatus(void) {
    GLIDE_TRACE("grSstStatus");
    FxU32 status = SST_FIFO_FREE | SST_MEMFIFO_FREE;
    if (!next_vretrace())
        status |= SST_NO_VRETRACE;
    return status;
}

FxBool GR_EXPORT GLIDE_API grSstIsBusy(void) {
    GLIDE_TRACE("grSstIsBusy");
    return FXFALSE;
}

int GR_EXPORT GLIDE_API grBufferNumPending(void) {
    GLIDE_TRACE("grBufferNumPending");
    return 0;
}

/* GR_ORIGIN_LOWER_LEFT: Y counts from the bottom; the hardware (and the
 * rasterizer) mirrors every line on write, while the clip window and
 * vertex coordinates stay in Glide coordinates. As in Glide,
 * grLfbWriteRegion/ReadRegion always work from the top left. */
void glide_set_origin(GrOriginLocation_t origin) {
    if (origin == GR_ORIGIN_LOWER_LEFT)
        g_voodoo.params.fbzMode |= FBZ_Y_ORIGIN;
    else
        g_voodoo.params.fbzMode &= ~FBZ_Y_ORIGIN;
}

void GR_EXPORT GLIDE_API grSstOrigin(GrOriginLocation_t origin) {
    GLIDE_TRACE("grSstOrigin");
    glide_set_origin(origin);
}

/* Per the Glide reference: alpha blending, alpha test, chroma key, depth
 * buffer, fog and stipple off. */
void GR_EXPORT GLIDE_API grDisableAllEffects(void) {
    GLIDE_TRACE("grDisableAllEffects");
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    grAlphaTestFunction(GR_CMP_ALWAYS);
    grChromakeyMode(GR_CHROMAKEY_DISABLE);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    g_voodoo.params.fogMode = 0;
    g_voodoo.params.fbzMode &= ~(uint32_t) FBZ_STIPPLE;
}

/* --- Gamma ------------------------------------------------------------ */

/* Like the Voodoo Graphics (3dfx sst1InitGammaRGB): the RAMDAC has 33
 * sample points, pow(x/255, 1/gamma) at every 8th value with the 33rd fixed
 * at 255, and interpolates linearly in between; the framebuffer itself
 * stays linear.
 *
 * Default: 3dfx's Voodoo Graphics Glide used 1.7, tuned for CRTs and too
 * bright and washed out on today's screens. Later 3dfx drivers (Voodoo 2,
 * Glide 3) used 1.3, which is the default here (GLIDE_DEFAULT_GAMMA). As
 * with the original driver, SST_GAMMA (environment or GLIDULAT.CFG, see
 * platform_get_config) overrides every value, including the
 * application's. */
#define GLIDE_DEFAULT_GAMMA 1.3f

/* "1.25" -> 1.25; 0 for garbage. Without strtod/atof (no C runtime in the
 * OVL). */
static float parse_float(const char *s) {
    float v = 0.0f, scale = 1.0f;
    int   frac = 0, any = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            any = 1;
            if (frac)
                v += (float) (*s - '0') * (scale /= 10.0f);
            else
                v = v * 10.0f + (float) (*s - '0');
        } else if ((*s == '.' || *s == ',') && !frac)
            frac = 1;
        else
            break;
    }
    return any ? v : 0.0f;
}

void glide_set_gamma(float gamma) {
    uint8_t lut[256];
    int     e[33];
    char    cfg[16];
    if (platform_get_config("SST_GAMMA", cfg, sizeof cfg) && parse_float(cfg) > 0.0f)
        gamma = parse_float(cfg);
    if (gamma <= 0.0f)
        gamma = 1.0f;
    for (int k = 0; k < 32; k++)
        e[k] = (int) (pow((double) (k << 3) / 255.0, 1.0 / (double) gamma) * 255.0 + 0.5);
    e[32] = 255;
    for (int i = 0; i < 256; i++) {
        int k  = i >> 3;
        lut[i] = (uint8_t) ((e[k] * 8 + (e[k + 1] - e[k]) * (i & 7) + 4) / 8);
    }
    /* The 33rd sample point belongs to input "256"; interpolating to it
     * would put white at 254 for some gammas. Pure white stays white. */
    lut[255] = 255;
    platform_set_gamma(lut);
}

void glide_set_default_gamma(void) {
    glide_set_gamma(GLIDE_DEFAULT_GAMMA);
}

void GR_EXPORT GLIDE_API grGammaCorrectionValue(float value) {
    GLIDE_TRACE("grGammaCorrectionValue");
    glide_set_gamma(value);
}

/* --- grGlideGetState / grGlideSetState -------------------------------- */

/* Glidulator's image inside the 312 bytes of GrState (opaque to the
 * application): the registers set by the state functions (see
 * core/glide_internal.h) plus the software state. As in Glide, texture
 * memory and palettes are not part of it. */
typedef struct {
    uint32_t       fbzMode, fbzColorPath, alphaMode, fogMode, zaColor, fogColor;
    uint32_t       color0, color1, chromaKey;
    uint32_t       textureMode, tLOD, texBaseAddr, tformat;
    int            clip_left, clip_right, clip_top, clip_bottom;
    glide_sw_state sw;
    GrTexInfo      src;
    int            src_valid;
} glide_saved_state;

/* Fails to compile if the image does not fit into GrState. */
typedef char glide_saved_state_fits[(sizeof(glide_saved_state) <= sizeof(GrState)) ? 1 : -1];

void GR_EXPORT GLIDE_API grGlideGetState(GrState *state) {
    GLIDE_TRACE("grGlideGetState");
    glide_saved_state s;
    if (!state)
        return;
    memset(&s, 0, sizeof s);
    s.fbzMode      = g_voodoo.params.fbzMode;
    s.fbzColorPath = g_voodoo.params.fbzColorPath;
    s.alphaMode    = g_voodoo.params.alphaMode;
    s.fogMode      = g_voodoo.params.fogMode;
    s.zaColor      = g_voodoo.params.zaColor;
    s.fogColor     = ((uint32_t) g_voodoo.params.fogColor.r << 16) | ((uint32_t) g_voodoo.params.fogColor.g << 8) |
                 g_voodoo.params.fogColor.b;
    s.color0       = g_voodoo.params.color0;
    s.color1       = g_voodoo.params.color1;
    s.chromaKey    = g_voodoo.params.chromaKey;
    s.textureMode  = g_voodoo.params.textureMode[0];
    s.tLOD         = g_voodoo.params.tLOD[0];
    s.texBaseAddr  = g_voodoo.params.texBaseAddr[0];
    s.tformat      = (uint32_t) g_voodoo.params.tformat[0];
    s.clip_left    = g_clip_left;
    s.clip_right   = g_clip_right;
    s.clip_top     = g_clip_top;
    s.clip_bottom  = g_clip_bottom;
    s.sw           = g_sw;
    glide_tex_get_source(&s.src, &s.src_valid);
    memset(state, 0, sizeof *state);
    memcpy(state, &s, sizeof s);
}

void GR_EXPORT GLIDE_API grGlideSetState(const GrState *state) {
    GLIDE_TRACE("grGlideSetState");
    glide_saved_state s;
    if (!state)
        return;
    memcpy(&s, state, sizeof s);
    g_voodoo.params.fbzMode        = s.fbzMode;
    g_voodoo.params.fbzColorPath   = s.fbzColorPath;
    g_voodoo.params.alphaMode      = s.alphaMode;
    g_voodoo.params.fogMode        = s.fogMode;
    g_voodoo.params.zaColor        = s.zaColor;
    g_voodoo.params.fogColor.r     = (uint8_t) (s.fogColor >> 16);
    g_voodoo.params.fogColor.g     = (uint8_t) (s.fogColor >> 8);
    g_voodoo.params.fogColor.b     = (uint8_t) s.fogColor;
    g_voodoo.params.color0         = s.color0;
    g_voodoo.params.color1         = s.color1;
    g_voodoo.params.chromaKey      = s.chromaKey;
    g_voodoo.params.chromaKey_r    = (int) ((s.chromaKey >> 16) & 0xff);
    g_voodoo.params.chromaKey_g    = (int) ((s.chromaKey >> 8) & 0xff);
    g_voodoo.params.chromaKey_b    = (int) (s.chromaKey & 0xff);
    g_voodoo.params.textureMode[0] = s.textureMode;
    g_voodoo.params.tLOD[0]        = s.tLOD;
    g_voodoo.params.texBaseAddr[0] = s.texBaseAddr;
    g_voodoo.params.tformat[0]     = (int) s.tformat;
    g_clip_left                    = s.clip_left;
    g_clip_right                   = s.clip_right;
    g_clip_top                     = s.clip_top;
    g_clip_bottom                  = s.clip_bottom;
    g_sw                           = s.sw;
    glide_tex_set_source(&s.src, s.src_valid);
    if (g_win_open && s.src_valid)
        voodoo_recalc_tex12(&g_voodoo, 0);
}

/* --- Statistics --------------------------------------------------- */

void GR_EXPORT GLIDE_API grTriStats(FxU32 *trisProcessed, FxU32 *trisDrawn) {
    GLIDE_TRACE("grTriStats");
    if (trisProcessed)
        *trisProcessed = g_tris_processed;
    if (trisDrawn)
        *trisDrawn = g_tris_drawn;
}

void GR_EXPORT GLIDE_API grResetTriStats(void) {
    GLIDE_TRACE("grResetTriStats");
    g_tris_processed = 0;
    g_tris_drawn     = 0;
}

/* The rasterizer does not count pixels; report 0 rather than invented
 * values. */
void GR_EXPORT GLIDE_API grSstPerfStats(GrSstPerfStats_t *pStats) {
    GLIDE_TRACE("grSstPerfStats");
    if (pStats)
        memset(pStats, 0, sizeof *pStats);
}

void GR_EXPORT GLIDE_API grSstResetPerfStats(void) {
    GLIDE_TRACE("grSstResetPerfStats");
}

/* --- Lines and points --------------------------------------------- */

/* As in Glide: drawn as thin triangles, never culled. A point is the
 * square [x, x+1) x [y, y+1), which contains exactly one pixel centre. */
static void draw_quad(const GrVertex *v0, const GrVertex *v1, const GrVertex *v2, const GrVertex *v3) {
    glide_draw_triangle(v0, v1, v2);
    glide_draw_triangle(v0, v2, v3);
}

void GR_EXPORT GLIDE_API grDrawPoint(const GrVertex *pt) {
    GLIDE_TRACE("grDrawPoint");
    GrVertex q[4];
    if (!pt)
        return;
    q[0] = q[1] = q[2] = q[3] = *pt;
    q[1].x += 1.0f;
    q[2].x += 1.0f;
    q[2].y += 1.0f;
    q[3].y += 1.0f;
    draw_quad(&q[0], &q[1], &q[2], &q[3]);
}

/* A line is a band 1 pixel wide across the major axis (in Y for mostly
 * horizontal lines, otherwise in X), anchored like a point: from (x0,y) to
 * (x1,y) it covers pixels x0..x1-1 of row y. */
void GR_EXPORT GLIDE_API grDrawLine(const GrVertex *v1, const GrVertex *v2) {
    GLIDE_TRACE("grDrawLine");
    GrVertex q[4];
    float    dx, dy;
    if (!v1 || !v2)
        return;
    q[0] = q[3] = *v1;
    q[1] = q[2] = *v2;
    dx = v2->x - v1->x;
    dy = v2->y - v1->y;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        q[0].y = v1->y;
        q[1].y = v2->y;
        q[2].y = v2->y + 1.0f;
        q[3].y = v1->y + 1.0f;
    } else {
        q[0].x = v1->x;
        q[1].x = v2->x;
        q[2].x = v2->x + 1.0f;
        q[3].x = v1->x + 1.0f;
    }
    draw_quad(&q[0], &q[1], &q[2], &q[3]);
}

/* On the Voodoo Graphics, 3dfx's grSstControl only forwards the code to
 * the init layer and reports success; window resize/move and
 * activate/deactivate concern the Voodoo Rush. Some programs treat FXFALSE
 * as a failed resize and quit ("Scenarios", Operation 3DFX '98). */
FxBool GR_EXPORT GLIDE_API grSstControl(FxU32 code) {
    GLIDE_TRACE("grSstControl");
    (void) code;
    return FXTRUE;
}

/* --- fxpci --------------------------------------------------------- */

/* 3dfx's glide2x.dll also exports its PCI library, and some programs
 * import it (Nature95). Open/close always succeed; there is no PCI board
 * (the stubs in core/glide_stubs.c return FXFALSE). */
FxBool GR_EXPORT GLIDE_API pciOpen(void) {
    GLIDE_TRACE("pciOpen");
    return FXTRUE;
}

FxBool GR_EXPORT GLIDE_API pciClose(void) {
    GLIDE_TRACE("pciClose");
    return FXTRUE;
}
