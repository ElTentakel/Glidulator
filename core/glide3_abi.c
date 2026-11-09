/*
 * Glide 3.x entry points (glide3x.dll), on top of the Glide 2 implementation.
 *
 * Built only with -DGLIDULATOR_GLIDE3: glide2x.h then renames every Glide 2
 * function to g2_*, and targets/glide_dll/glide3x.def exports the g3_*
 * functions below under 3dfx's Glide 3 names. Most Glide 3 functions mean the
 * same as their Glide 2 namesakes and are forwarded unchanged; this file only
 * handles what Glide 3 changed:
 *   - free vertex layout (grVertexLayout): every vertex is converted into a
 *     Glide 2 GrVertex before it reaches the triangle setup;
 *   - texture sizes as log2 values (GR_LOD_LOG2_*, GR_ASPECT_LOG2_*);
 *   - fog modes, grGet/grGetString queries, grEnable/grDisable,
 *     grQueryResolutions, contexts, vertex arrays, changed signatures.
 * Glide 3 values whose names exist in Glide 2 with other numbers are G3_*.
 *
 * Window coordinates only (GR_WINDOW_COORDS, what the THRASH renderers of
 * the NFS games use); clip coordinates are logged and drawn as window
 * coordinates for now.
 */

#ifdef GLIDULATOR_GLIDE3

#include "glidulator/glide2x.h"
#include "glidulator/trace.h"
#include "glidulator/platform.h"

#include "glide_internal.h"

#include <string.h>

typedef FxU32 GrContext_t;


typedef struct {
    GrScreenResolution_t resolution;
    GrScreenRefresh_t    refresh;
    int                  numColorBuffers;
    int                  numAuxBuffers;
} GrResolution;

/* grVertexLayout parameters */
#define G3_PARAM_XY      0x01
#define G3_PARAM_Z       0x02
#define G3_PARAM_W       0x03
#define G3_PARAM_Q       0x04
#define G3_PARAM_FOG_EXT 0x05
#define G3_PARAM_A       0x10
#define G3_PARAM_RGB     0x20
#define G3_PARAM_PARGB   0x30
#define G3_PARAM_ST0     0x40
#define G3_PARAM_ST1     0x41
#define G3_PARAM_ST2     0x42
#define G3_PARAM_Q0      0x50
#define G3_PARAM_Q1      0x51
#define G3_PARAM_Q2      0x52

/* grCoordinateSpace */
#define G3_WINDOW_COORDS 0
#define G3_CLIP_COORDS   1

/* grEnable/grDisable */
#define G3_AA_ORDERED          1
#define G3_ALLOW_MIPMAP_DITHER 2
#define G3_PASSTHRU            3
#define G3_SHAMELESS_PLUG      4
#define G3_VIDEO_SMOOTHING     5

/* grDrawVertexArray modes */
#define G3_POINTS                  0
#define G3_LINE_STRIP              1
#define G3_LINES                   2
#define G3_POLYGON                 3
#define G3_TRIANGLE_STRIP          4
#define G3_TRIANGLE_FAN            5
#define G3_TRIANGLES               6
#define G3_TRIANGLE_STRIP_CONTINUE 7
#define G3_TRIANGLE_FAN_CONTINUE   8

/* grFogMode */
#define G3_FOG_DISABLE                    0
#define G3_FOG_WITH_TABLE_ON_FOGCOORD_EXT 1
#define G3_FOG_WITH_TABLE_ON_Q            2
#define G3_FOG_WITH_ITERATED_Z            3
#define G3_FOG_WITH_ITERATED_ALPHA_EXT    4
#define G3_FOG_MODE_MASK                  0xff

/* grGet */
#define G3_BITS_DEPTH                  0x01
#define G3_BITS_RGBA                   0x02
#define G3_FIFO_FULLNESS               0x03
#define G3_FOG_TABLE_ENTRIES           0x04
#define G3_GAMMA_TABLE_ENTRIES         0x05
#define G3_GLIDE_STATE_SIZE            0x06
#define G3_GLIDE_VERTEXLAYOUT_SIZE     0x07
#define G3_IS_BUSY                     0x08
#define G3_LFB_PIXEL_PIPE              0x09
#define G3_MAX_TEXTURE_SIZE            0x0a
#define G3_MAX_TEXTURE_ASPECT_RATIO    0x0b
#define G3_MEMORY_FB                   0x0c
#define G3_MEMORY_TMU                  0x0d
#define G3_MEMORY_UMA                  0x0e
#define G3_NUM_BOARDS                  0x0f
#define G3_NON_POWER_OF_TWO_TEXTURES   0x10
#define G3_NUM_FB                      0x11
#define G3_NUM_SWAP_HISTORY_BUFFER     0x12
#define G3_NUM_TMU                     0x13
#define G3_PENDING_BUFFERSWAPS         0x14
#define G3_REVISION_FB                 0x15
#define G3_REVISION_TMU                0x16
#define G3_STATS_LINES                 0x17
#define G3_STATS_PIXELS_AFUNC_FAIL     0x18
#define G3_STATS_PIXELS_CHROMA_FAIL    0x19
#define G3_STATS_PIXELS_DEPTHFUNC_FAIL 0x1a
#define G3_STATS_PIXELS_IN             0x1b
#define G3_STATS_PIXELS_OUT            0x1c
#define G3_STATS_PIXELS                0x1d
#define G3_STATS_POINTS                0x1e
#define G3_STATS_TRIANGLES_IN          0x1f
#define G3_STATS_TRIANGLES_OUT         0x20
#define G3_STATS_TRIANGLES             0x21
#define G3_SWAP_HISTORY                0x22
#define G3_SUPPORTS_PASSTHRU           0x23
#define G3_TEXTURE_ALIGN               0x24
#define G3_VIDEO_POSITION              0x25
#define G3_VIEWPORT                    0x26
#define G3_WDEPTH_MIN_MAX              0x27
#define G3_ZDEPTH_MIN_MAX              0x28
#define G3_VERTEX_PARAMETER            0x29
#define G3_BITS_GAMMA                  0x2a

/* grGetString */
#define G3_EXTENSION 0xa0
#define G3_HARDWARE  0xa1
#define G3_RENDERER  0xa2
#define G3_VENDOR    0xa3
#define G3_VERSION   0xa4

#define G3_QUERY_ANY ((FxU32) -1)

/* --- State ---------------------------------------------------------- */

typedef struct {
    FxI32 offset;
    FxU32 on;
} g3_param;

/* What grGlideGetVertexLayout/SetVertexLayout copy */
typedef struct {
    g3_param xy, z, w, q, fog, a, rgb, pargb, st[3], qt[3];
} g3_layout;

typedef struct {
    GrState   g2; /* everything the Glide 2 implementation keeps */
    g3_layout layout;
    FxU32     coord_space;
    FxI32     vp_x, vp_y, vp_w, vp_h;
    float     depth_n, depth_f;
} g3_state;

void GLIDE_API g3_grGlideGetState(void *state);
void GLIDE_API g3_grGlideSetState(const void *state);

static g3_layout g_layout;
static FxU32     g_coord_space = G3_WINDOW_COORDS;
static FxI32     g_vp_x, g_vp_y, g_vp_w, g_vp_h;
static float     g_depth_n, g_depth_f = 1.0f;

/* Last two vertices of a strip/fan, for the *_CONTINUE modes */
static GrVertex g_strip_v[2];
static int      g_strip_odd;

/* Glide 3 fills TMU1 from its own ST1/Q1 when they are enabled; the Glide 2
 * setup takes those only with the matching STW hints. */
static void update_stw_hint(void) {
    FxU32 hint = GR_STWHINT_ST_DIFF_TMU1;
    if (g_layout.qt[0].on)
        hint |= GR_STWHINT_W_DIFF_TMU0;
    if (g_layout.qt[1].on)
        hint |= GR_STWHINT_W_DIFF_TMU1;
    g_glide_mute++;
    g2_grHints(GR_HINT_STWHINT, hint);
    g_glide_mute--;
}

static inline float vf(const void *v, FxI32 off) {
    float f;
    memcpy(&f, (const uint8_t *) v + off, sizeof f);
    return f;
}

/* Glide 3 vertex -> Glide 2 GrVertex (window coordinates: the same meaning,
 * only the place in memory differs). */
static void to_g2(const void *v, GrVertex *o) {
    const g3_layout *l = &g_layout;
    o->x   = vf(v, l->xy.offset);
    o->y   = vf(v, l->xy.offset + 4);
    o->z   = 0.0f;
    o->ooz = l->z.on ? vf(v, l->z.offset) : 0.0f;
    o->oow = l->q.on ? vf(v, l->q.offset) : 1.0f;
    if (l->pargb.on) {
        FxU32 c;
        memcpy(&c, (const uint8_t *) v + l->pargb.offset, sizeof c);
        o->a = (float) (c >> 24);
        o->r = (float) ((c >> 16) & 0xff);
        o->g = (float) ((c >> 8) & 0xff);
        o->b = (float) (c & 0xff);
    } else {
        o->r = l->rgb.on ? vf(v, l->rgb.offset) : 0.0f;
        o->g = l->rgb.on ? vf(v, l->rgb.offset + 4) : 0.0f;
        o->b = l->rgb.on ? vf(v, l->rgb.offset + 8) : 0.0f;
        o->a = l->a.on ? vf(v, l->a.offset) : 0.0f;
    }
    for (int t = 0; t < GLIDE_NUM_TMU; t++) {
        /* TMU1 without its own ST1 gets TMU0's, as on the board */
        const g3_param *st = (l->st[t].on || t == 0) ? &l->st[t] : &l->st[0];
        if (st->on) {
            o->tmuvtx[t].sow = vf(v, st->offset);
            o->tmuvtx[t].tow = vf(v, st->offset + 4);
        } else {
            o->tmuvtx[t].sow = o->tmuvtx[t].tow = 0.0f;
        }
        o->tmuvtx[t].oow = l->qt[t].on ? vf(v, l->qt[t].offset) : o->oow;
    }
}

/* Glide 3 log2 encodings -> Glide 2 */
static GrLOD_t lod_g2(GrLOD_t log2) {
    return GR_LOD_1 - log2; /* LOG2_256 = 8 -> GR_LOD_256 = 0 */
}

static GrAspectRatio_t aspect_g2(GrAspectRatio_t log2) {
    return GR_ASPECT_1x1 - log2; /* LOG2_8x1 = 3 -> GR_ASPECT_8x1 = 0 */
}

static int lod_g3(int lod) {
    return GR_LOD_1 - lod;
}

static int aspect_g3(int aspect) {
    return GR_ASPECT_1x1 - aspect;
}

static GrTexInfo *info_g2(const GrTexInfo *in, GrTexInfo *out) {
    if (!in)
        return NULL;
    out->smallLod    = lod_g2(in->smallLod);
    out->largeLod    = lod_g2(in->largeLod);
    out->aspectRatio = aspect_g2(in->aspectRatio);
    out->format      = in->format;
    out->data        = in->data;
    return out;
}

static void log_once(const char *what, unsigned long a) {
#ifdef GLIDULATOR_TRACE
    glide_trace_note(what, a, 0);
#else
    (void) what;
    (void) a;
#endif
}


#ifdef GLIDULATOR_TRACE
/* Logs the first 24 distinct value tuples of a call site */
static void log_distinct(uint32_t *seen, int *nseen, const char *what, uint32_t a, uint32_t b) {
    uint32_t h = (a * 2654435761u) ^ b;
    for (int i = 0; i < *nseen; i++)
        if (seen[i] == h)
            return;
    if (*nseen >= 24)
        return;
    seen[(*nseen)++] = h;
    glide_trace_note(what, a, b);
}
#define LOG_DISTINCT(what, a, b)                                   \
    do {                                                           \
        static uint32_t seen_[24];                                 \
        static int      nseen_;                                    \
        log_distinct(seen_, &nseen_, what, (uint32_t) (a), (uint32_t) (b)); \
    } while (0)
#else
#define LOG_DISTINCT(what, a, b) ((void) 0)
#endif

/* --- Initialisation, windows, contexts ------------------------------- */

void GLIDE_API g3_grGlideInit(void) {
    g2_grGlideInit();
}

void GLIDE_API g3_grGlideShutdown(void) {
    g2_grGlideShutdown();
}

void GLIDE_API g3_grSstSelect(int which_sst) {
    g2_grSstSelect(which_sst);
}

GrContext_t GLIDE_API g3_grSstWinOpen(FxU32 hWnd, GrScreenResolution_t res, GrScreenRefresh_t refresh,
                                      GrColorFormat_t color_format, GrOriginLocation_t origin, int nColBuffers,
                                      int nAuxBuffers) {
    GLIDE_TRACE("g3 grSstWinOpen");
    if (!g2_grSstWinOpen(hWnd, res, refresh, color_format, origin, nColBuffers, nAuxBuffers))
        return 0;
    memset(&g_layout, 0, sizeof g_layout);
    g_coord_space = G3_WINDOW_COORDS;
    g_vp_x = g_vp_y = 0;
    g_vp_w          = g_width;
    g_vp_h          = g_height;
    g_depth_n       = 0.0f;
    g_depth_f       = 1.0f;
    update_stw_hint();
    {
        /* 3dfx's Glide 3 splash module (GLIDULATOR_SPLASH=original with a
         * 3dfxSpl*.dll, core/splash.c): it uses the Glide 3 API like a
         * program, so the program then gets this state again */
        static g3_state before;
        g3_grGlideGetState(&before);
        /* the module refuses fewer than 2 colour and 1 aux buffers;
         * Glidulator always has them */
        if (glide_splash_module_run(hWnd, g_width, g_height, nColBuffers < 2 ? 2 : nColBuffers,
                                    nAuxBuffers < 1 ? 1 : nAuxBuffers, (int) color_format)) {
            glide_splash_cleanup();
            g3_grGlideSetState(&before);
        }
    }
    return 1; /* one context, see grSelectContext */
}

FxBool GLIDE_API g3_grSstWinClose(GrContext_t context) {
    (void) context;
    g2_grSstWinClose();
    return FXTRUE;
}

FxBool GLIDE_API g3_grSelectContext(GrContext_t context) {
    GLIDE_TRACE("grSelectContext");
    return context != 0 && g_win_open;
}

void GLIDE_API g3_grSstOrigin(GrOriginLocation_t origin) {
    g2_grSstOrigin(origin);
}

FxI32 GLIDE_API g3_grQueryResolutions(const GrResolution *tmpl, GrResolution *output) {
    GLIDE_TRACE("grQueryResolutions");
    /* What grSstWinOpen accepts (core/glide_abi.c), at one refresh rate
     * (the display decides it anyway), 2-3 colour and 0-1 aux buffers */
    static const GrScreenResolution_t res[] = {
        GR_RESOLUTION_320x200, GR_RESOLUTION_320x240, GR_RESOLUTION_400x256, GR_RESOLUTION_512x384,
        GR_RESOLUTION_640x400, GR_RESOLUTION_640x480, GR_RESOLUTION_800x600};
    FxI32 size = 0;
    if (!tmpl)
        return 0;
    for (unsigned i = 0; i < sizeof res / sizeof res[0]; i++) {
        if ((FxU32) tmpl->resolution != G3_QUERY_ANY && (FxU32) tmpl->resolution != (FxU32) res[i])
            continue;
        if ((FxU32) tmpl->refresh != G3_QUERY_ANY && (FxU32) tmpl->refresh != (FxU32) GR_REFRESH_60Hz)
            continue;
        for (int col = 2; col <= 3; col++) {
            if ((FxU32) tmpl->numColorBuffers != G3_QUERY_ANY && tmpl->numColorBuffers != col)
                continue;
            for (int aux = 0; aux <= 1; aux++) {
                if ((FxU32) tmpl->numAuxBuffers != G3_QUERY_ANY && tmpl->numAuxBuffers != aux)
                    continue;
                size += (FxI32) sizeof(GrResolution);
                if (output) {
                    output->resolution      = res[i];
                    output->refresh         = GR_REFRESH_60Hz;
                    output->numColorBuffers = col;
                    output->numAuxBuffers   = aux;
                    output++;
                }
            }
        }
    }
    return size;
}

FxBool GLIDE_API g3_grReset(FxU32 what) {
    GLIDE_TRACE("grReset");
    if (what == G3_STATS_TRIANGLES || what == G3_STATS_POINTS || what == G3_STATS_LINES || what == G3_STATS_PIXELS) {
        g2_grResetTriStats();
        return FXTRUE;
    }
    return FXFALSE;
}

GrProc GLIDE_API g3_grGetProcAddress(char *procName) {
    GLIDE_TRACE("grGetProcAddress");
    (void) procName; /* no extensions offered (grGetString(GR_EXTENSION) is empty) */
    return NULL;
}

/* --- Queries --------------------------------------------------------- */

#define GET1(v)                    \
    do {                           \
        if (plength != 4)          \
            return 0;              \
        params[0] = (FxI32) (v);   \
        return 4;                  \
    } while (0)

FxU32 GLIDE_API g3_grGet(FxU32 pname, FxU32 plength, FxI32 *params) {
    GLIDE_TRACE("grGet");
    if (!params)
        return 0;
#ifdef GLIDULATOR_TRACE
    {
        static unsigned char seen[64];
        if (pname < 64 && !seen[pname]++)
            glide_trace_note("grGet pname/plength", pname, plength);
    }
#endif
    switch (pname) {
    case G3_BITS_DEPTH: GET1(16);
    case G3_BITS_RGBA:
        if (plength != 16)
            return 0;
        params[0] = 5;
        params[1] = 6;
        params[2] = 5;
        params[3] = 0;
        return 16;
    case G3_FIFO_FULLNESS:
        if (plength != 8)
            return 0;
        params[0] = 0xffff; /* empty FIFO */
        params[1] = 0xffff;
        return 8;
    case G3_FOG_TABLE_ENTRIES: GET1(GR_FOG_TABLE_SIZE);
    case G3_GAMMA_TABLE_ENTRIES: GET1(32);
    case G3_GLIDE_STATE_SIZE: GET1(sizeof(g3_state));
    case G3_GLIDE_VERTEXLAYOUT_SIZE: GET1(sizeof(g3_layout));
    case G3_IS_BUSY: GET1(FXFALSE);
    case G3_LFB_PIXEL_PIPE: GET1(FXTRUE);
    case G3_MAX_TEXTURE_SIZE: GET1(256);
    case G3_MAX_TEXTURE_ASPECT_RATIO: GET1(3);
    case G3_MEMORY_FB: GET1(4 << 20);
    case G3_MEMORY_TMU: GET1(TEX_MEM_BYTES);
    case G3_MEMORY_UMA: GET1(0);
    case G3_NUM_BOARDS: GET1(1);
    case G3_NON_POWER_OF_TWO_TEXTURES: GET1(FXFALSE);
    case G3_NUM_FB: GET1(1);
    case G3_NUM_SWAP_HISTORY_BUFFER: GET1(8);
    case G3_NUM_TMU: GET1(g_win_open ? g_num_tmus : glide_tmus_read_cfg());
    case G3_PENDING_BUFFERSWAPS: GET1(0);
    case G3_REVISION_FB: GET1(2);
    case G3_REVISION_TMU: GET1(2);
    case G3_STATS_TRIANGLES_IN: GET1(g_tris_processed);
    case G3_STATS_TRIANGLES_OUT: GET1(g_tris_drawn);
    case G3_STATS_LINES:
    case G3_STATS_POINTS:
    case G3_STATS_PIXELS_AFUNC_FAIL:
    case G3_STATS_PIXELS_CHROMA_FAIL:
    case G3_STATS_PIXELS_DEPTHFUNC_FAIL:
    case G3_STATS_PIXELS_IN:
    case G3_STATS_PIXELS_OUT: GET1(0);
    case G3_SWAP_HISTORY:
        if (plength != 32)
            return 0;
        memset(params, 0, 32);
        return 32;
    case G3_SUPPORTS_PASSTHRU: GET1(FXTRUE);
    case G3_TEXTURE_ALIGN: GET1(8);
    case G3_VIDEO_POSITION:
        /* 3dfx's Voodoo2 Glide 3 answers this only in its alpha builds:
         * always 0 bytes */
        return 0;
    case G3_VIEWPORT:
        if (plength != 16)
            return 0;
        params[0] = g_vp_x;
        params[1] = g_vp_y;
        params[2] = g_vp_w;
        params[3] = g_vp_h;
        return 16;
    case G3_WDEPTH_MIN_MAX:
        if (plength != 8)
            return 0;
        params[0] = 0;
        params[1] = 0xffff;
        return 8;
    case G3_ZDEPTH_MIN_MAX:
        if (plength != 8)
            return 0;
        params[0] = 0xffff;
        params[1] = 0;
        return 8;
    case G3_BITS_GAMMA: GET1(8);
    default: log_once("grGet unknown pname", pname); return 0;
    }
}

const char *GLIDE_API g3_grGetString(FxU32 pname) {
    GLIDE_TRACE("grGetString");
    switch (pname) {
    case G3_EXTENSION: return ""; /* no CHROMARANGE/TEXMIRROR/PALETTE6666/FOGCOORD yet */
    case G3_HARDWARE: return "Voodoo2";
    case G3_RENDERER: return "Glide";
    case G3_VENDOR: return "3Dfx Interactive";
    case G3_VERSION: return "3.03";
    default: return "ERROR";
    }
}

void GLIDE_API g3_grEnable(FxU32 mode) {
    GLIDE_TRACE("grEnable");
    if (mode == G3_ALLOW_MIPMAP_DITHER)
        g2_grHints(GR_HINT_ALLOW_MIPMAP_DITHER, FXTRUE);
}

void GLIDE_API g3_grDisable(FxU32 mode) {
    GLIDE_TRACE("grDisable");
    if (mode == G3_ALLOW_MIPMAP_DITHER)
        g2_grHints(GR_HINT_ALLOW_MIPMAP_DITHER, FXFALSE);
}

/* --- Coordinates and vertex layout ------------------------------------- */

void GLIDE_API g3_grCoordinateSpace(FxU32 mode) {
    GLIDE_TRACE("grCoordinateSpace");
    if (mode != G3_WINDOW_COORDS)
        log_once("grCoordinateSpace: clip coordinates not supported", mode);
    g_coord_space = mode;
}

void GLIDE_API g3_grViewport(FxI32 x, FxI32 y, FxI32 width, FxI32 height) {
    GLIDE_TRACE("grViewport");
    g_vp_x = x;
    g_vp_y = y;
    g_vp_w = width;
    g_vp_h = height;
}

void GLIDE_API g3_grDepthRange(float n, float f) {
    GLIDE_TRACE("grDepthRange");
    g_depth_n = n;
    g_depth_f = f;
}

void GLIDE_API g3_grVertexLayout(FxU32 param, FxI32 offset, FxU32 mode) {
    GLIDE_TRACE("grVertexLayout");
    g3_param *p;
    switch (param) {
    case G3_PARAM_XY: p = &g_layout.xy; break;
    case G3_PARAM_Z: p = &g_layout.z; break;
    case G3_PARAM_W: p = &g_layout.w; break;
    case G3_PARAM_Q: p = &g_layout.q; break;
    case G3_PARAM_FOG_EXT: p = &g_layout.fog; break;
    case G3_PARAM_A: p = &g_layout.a; break;
    case G3_PARAM_RGB: p = &g_layout.rgb; break;
    case G3_PARAM_PARGB: p = &g_layout.pargb; break;
    case G3_PARAM_ST0:
    case G3_PARAM_ST1:
    case G3_PARAM_ST2: p = &g_layout.st[param - G3_PARAM_ST0]; break;
    case G3_PARAM_Q0:
    case G3_PARAM_Q1:
    case G3_PARAM_Q2: p = &g_layout.qt[param - G3_PARAM_Q0]; break;
    default: log_once("grVertexLayout unknown param", param); return;
    }
#ifdef GLIDULATOR_TRACE
    {
        static int logged;
        if (logged++ < 64)
            glide_trace_note("grVertexLayout param/offset", param, (unsigned long) offset | (mode ? 0x10000ul : 0));
    }
#endif
    p->offset = offset;
    p->on     = mode != 0;
    /* RGB/A and PARGB exclude each other (the last enabled wins, as in
     * 3dfx's colorType) */
    if (p->on && (param == G3_PARAM_A || param == G3_PARAM_RGB))
        g_layout.pargb.on = 0;
    else if (p->on && param == G3_PARAM_PARGB)
        g_layout.rgb.on = g_layout.a.on = 0;
    if (param == G3_PARAM_Q0 || param == G3_PARAM_Q1)
        update_stw_hint();
}

void GLIDE_API g3_grGlideGetVertexLayout(void *layout) {
    if (layout)
        memcpy(layout, &g_layout, sizeof g_layout);
}

void GLIDE_API g3_grGlideSetVertexLayout(const void *layout) {
    if (!layout)
        return;
    memcpy(&g_layout, layout, sizeof g_layout);
    update_stw_hint();
}

void GLIDE_API g3_grGlideGetState(void *state) {
    g3_state *s = (g3_state *) state;
    if (!s)
        return;
    g2_grGlideGetState(&s->g2);
    s->layout      = g_layout;
    s->coord_space = g_coord_space;
    s->vp_x        = g_vp_x;
    s->vp_y        = g_vp_y;
    s->vp_w        = g_vp_w;
    s->vp_h        = g_vp_h;
    s->depth_n     = g_depth_n;
    s->depth_f     = g_depth_f;
}

void GLIDE_API g3_grGlideSetState(const void *state) {
    const g3_state *s = (const g3_state *) state;
    if (!s)
        return;
    g2_grGlideSetState(&s->g2);
    g_layout      = s->layout;
    g_coord_space = s->coord_space;
    g_vp_x        = s->vp_x;
    g_vp_y        = s->vp_y;
    g_vp_w        = s->vp_w;
    g_vp_h        = s->vp_h;
    g_depth_n     = s->depth_n;
    g_depth_f     = s->depth_f;
    update_stw_hint();
}

/* --- Drawing ----------------------------------------------------------- */

void GLIDE_API g3_grDrawPoint(const void *pt) {
    GrVertex v;
    if (!pt)
        return;
    to_g2(pt, &v);
    g2_grDrawPoint(&v);
}

void GLIDE_API g3_grDrawLine(const void *v1, const void *v2) {
    GrVertex a, b;
    if (!v1 || !v2)
        return;
    to_g2(v1, &a);
    to_g2(v2, &b);
    g2_grDrawLine(&a, &b);
}

void GLIDE_API g3_grDrawTriangle(const void *a, const void *b, const void *c) {
    GrVertex va, vb, vc;
    if (!a || !b || !c)
        return;
    to_g2(a, &va);
    to_g2(b, &vb);
    to_g2(c, &vc);
    g2_grDrawTriangle(&va, &vb, &vc);
}

/* Without edge antialiasing (as the Glide 2 path): the triangle itself */
void GLIDE_API g3_grAADrawTriangle(const void *a, const void *b, const void *c, FxBool ab, FxBool bc, FxBool ca) {
    GLIDE_TRACE("g3 grAADrawTriangle");
    (void) ab;
    (void) bc;
    (void) ca;
    g3_grDrawTriangle(a, b, c);
}

static const void *array_vertex(void *pointers, FxU32 i, FxU32 stride) {
    if (stride)
        return (const uint8_t *) pointers + (size_t) i * stride;
    return ((void *const *) pointers)[i];
}

/* stride 0: pointers is an array of vertex pointers (grDrawVertexArray),
 * otherwise the vertices themselves, stride bytes apart. Strips keep the
 * winding of the first triangle (every second one swapped), as the board
 * does with its strip cull sign. */
static void draw_array(FxU32 mode, FxU32 count, void *pointers, FxU32 stride) {
    GrVertex v[3];
    if (!pointers)
        return;
    switch (mode) {
    case G3_POINTS:
        for (FxU32 i = 0; i < count; i++) {
            to_g2(array_vertex(pointers, i, stride), &v[0]);
            g2_grDrawPoint(&v[0]);
        }
        break;
    case G3_LINES:
        for (FxU32 i = 0; i + 1 < count; i += 2) {
            to_g2(array_vertex(pointers, i, stride), &v[0]);
            to_g2(array_vertex(pointers, i + 1, stride), &v[1]);
            g2_grDrawLine(&v[0], &v[1]);
        }
        break;
    case G3_LINE_STRIP:
        for (FxU32 i = 0; i + 1 < count; i++) {
            to_g2(array_vertex(pointers, i, stride), &v[0]);
            to_g2(array_vertex(pointers, i + 1, stride), &v[1]);
            g2_grDrawLine(&v[0], &v[1]);
        }
        break;
    case G3_TRIANGLES:
        for (FxU32 i = 0; i + 2 < count; i += 3) {
            to_g2(array_vertex(pointers, i, stride), &v[0]);
            to_g2(array_vertex(pointers, i + 1, stride), &v[1]);
            to_g2(array_vertex(pointers, i + 2, stride), &v[2]);
            g2_grDrawTriangle(&v[0], &v[1], &v[2]);
        }
        break;
    case G3_TRIANGLE_STRIP:
    case G3_TRIANGLE_STRIP_CONTINUE: {
        FxU32 i = 0;
        if (mode == G3_TRIANGLE_STRIP) {
            if (count < 3)
                return;
            to_g2(array_vertex(pointers, 0, stride), &g_strip_v[0]);
            to_g2(array_vertex(pointers, 1, stride), &g_strip_v[1]);
            g_strip_odd = 0;
            i = 2;
        }
        for (; i < count; i++) {
            to_g2(array_vertex(pointers, i, stride), &v[2]);
            if (g_strip_odd)
                g2_grDrawTriangle(&g_strip_v[1], &g_strip_v[0], &v[2]);
            else
                g2_grDrawTriangle(&g_strip_v[0], &g_strip_v[1], &v[2]);
            g_strip_v[0] = g_strip_v[1];
            g_strip_v[1] = v[2];
            g_strip_odd ^= 1;
        }
        break;
    }
    case G3_TRIANGLE_FAN:
    case G3_POLYGON:
    case G3_TRIANGLE_FAN_CONTINUE: {
        FxU32 i = 0;
        if (mode != G3_TRIANGLE_FAN_CONTINUE) {
            if (count < 3)
                return;
            to_g2(array_vertex(pointers, 0, stride), &g_strip_v[0]);
            to_g2(array_vertex(pointers, 1, stride), &g_strip_v[1]);
            i = 2;
        }
        for (; i < count; i++) {
            to_g2(array_vertex(pointers, i, stride), &v[2]);
            g2_grDrawTriangle(&g_strip_v[0], &g_strip_v[1], &v[2]);
            g_strip_v[1] = v[2];
        }
        break;
    }
    default: log_once("grDrawVertexArray unknown mode", mode); break;
    }
}

void GLIDE_API g3_grDrawVertexArray(FxU32 mode, FxU32 count, void *pointers) {
    GLIDE_TRACE("grDrawVertexArray");
    draw_array(mode, count, pointers, 0);
}

void GLIDE_API g3_grDrawVertexArrayContiguous(FxU32 mode, FxU32 count, void *pointers, FxU32 stride) {
    GLIDE_TRACE("grDrawVertexArrayContiguous");
    if (stride)
        draw_array(mode, count, pointers, stride);
}

/* --- Buffers ----------------------------------------------------------- */

void GLIDE_API g3_grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU32 depth) {
    g2_grBufferClear(color, alpha, (FxU16) depth);
}

void GLIDE_API g3_grBufferSwap(FxU32 swap_interval) {
    g2_grBufferSwap((int) swap_interval);
}

void GLIDE_API g3_grRenderBuffer(GrBuffer_t buffer) {
    g2_grRenderBuffer(buffer);
}

void GLIDE_API g3_grErrorSetCallback(GrErrorCallbackFnc_t fnc) {
    g2_grErrorSetCallback(fnc);
}

void GLIDE_API g3_grFinish(void) {
    GLIDE_TRACE("grFinish");
    glide_mt_idle();
}

void GLIDE_API g3_grFlush(void) {
    GLIDE_TRACE("grFlush");
}

/* --- Rendering state (same meaning as Glide 2) --------------------------- */

void GLIDE_API g3_grAlphaBlendFunction(GrAlphaBlendFnc_t rs, GrAlphaBlendFnc_t rd, GrAlphaBlendFnc_t as,
                                       GrAlphaBlendFnc_t ad) {
    g2_grAlphaBlendFunction(rs, rd, as, ad);
}

void GLIDE_API g3_grAlphaCombine(GrCombineFunction_t f, GrCombineFactor_t fa, GrCombineLocal_t l, GrCombineOther_t o,
                                 FxBool inv) {
    g2_grAlphaCombine(f, fa, l, o, inv);
}

void GLIDE_API g3_grAlphaControlsITRGBLighting(FxBool enable) {
    g2_grAlphaControlsITRGBLighting(enable);
}

void GLIDE_API g3_grAlphaTestFunction(GrCmpFnc_t function) {
    g2_grAlphaTestFunction(function);
}

void GLIDE_API g3_grAlphaTestReferenceValue(GrAlpha_t value) {
    g2_grAlphaTestReferenceValue(value);
}

void GLIDE_API g3_grChromakeyMode(GrChromakeyMode_t mode) {
    g2_grChromakeyMode(mode);
}

void GLIDE_API g3_grChromakeyValue(GrColor_t value) {
    g2_grChromakeyValue(value);
}

void GLIDE_API g3_grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx, FxU32 maxy) {
    g2_grClipWindow(minx, miny, maxx, maxy);
}

void GLIDE_API g3_grColorCombine(GrCombineFunction_t f, GrCombineFactor_t fa, GrCombineLocal_t l, GrCombineOther_t o,
                                 FxBool inv) {
    g2_grColorCombine(f, fa, l, o, inv);
}

void GLIDE_API g3_grColorMask(FxBool rgb, FxBool a) {
    g2_grColorMask(rgb, a);
}

void GLIDE_API g3_grCullMode(GrCullMode_t mode) {
    g2_grCullMode(mode);
}

void GLIDE_API g3_grConstantColorValue(GrColor_t value) {
    g2_grConstantColorValue(value);
}

void GLIDE_API g3_grDepthBiasLevel(FxI32 level) {
    g2_grDepthBiasLevel((FxI16) level);
}

void GLIDE_API g3_grDepthBufferFunction(GrCmpFnc_t function) {
    g2_grDepthBufferFunction(function);
}

void GLIDE_API g3_grDepthBufferMode(GrDepthBufferMode_t mode) {
    g2_grDepthBufferMode(mode);
}

void GLIDE_API g3_grDepthMask(FxBool mask) {
    g2_grDepthMask(mask);
}

void GLIDE_API g3_grDisableAllEffects(void) {
    g2_grDisableAllEffects();
}

void GLIDE_API g3_grStippleMode(FxU32 mode) {
    GLIDE_TRACE("grStippleMode");
    if (mode)
        log_once("grStippleMode not supported", mode);
}

void GLIDE_API g3_grStipplePattern(FxU32 pattern) {
    GLIDE_TRACE("grStipplePattern");
    (void) pattern;
}

void GLIDE_API g3_grDitherMode(GrDitherMode_t mode) {
    g2_grDitherMode(mode);
}

void GLIDE_API g3_grFogColorValue(GrColor_t fogcolor) {
    g2_grFogColorValue(fogcolor);
}

/* Glide 3 fog modes -> Glide 2 (MULT2/ADD2 bits are the same). The fog
 * coordinate extension is not offered, so FOGCOORD falls back to the table
 * on Q. */
void GLIDE_API g3_grFogMode(GrFogMode_t mode) {
    GrFogMode_t extra = mode & ~(GrFogMode_t) G3_FOG_MODE_MASK;
    GrFogMode_t g2;
    switch (mode & G3_FOG_MODE_MASK) {
    case G3_FOG_WITH_TABLE_ON_FOGCOORD_EXT:
    case G3_FOG_WITH_TABLE_ON_Q: g2 = GR_FOG_WITH_TABLE; break;
    case G3_FOG_WITH_ITERATED_Z: g2 = GR_FOG_WITH_ITERATED_Z; break;
    case G3_FOG_WITH_ITERATED_ALPHA_EXT: g2 = GR_FOG_WITH_ITERATED_ALPHA; break;
    default: g2 = GR_FOG_DISABLE; extra = 0; break;
    }
    g2_grFogMode(g2 | extra);
}

void GLIDE_API g3_grFogTable(const GrFog_t ft[]) {
#ifdef GLIDULATOR_TRACE
    /* The first distinct tables (entries 0, 8, ..., 56, 63) */
    if (ft) {
        static uint32_t seen[16];
        static int      nseen;
        uint32_t        h = 2166136261u;
        for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
            h = (h ^ ft[i]) * 16777619u;
        int known = 0;
        for (int i = 0; i < nseen; i++)
            known |= seen[i] == h;
        if (!known && nseen < 16) {
            float e[8];
            seen[nseen++] = h;
            for (int i = 0; i < 8; i++)
                e[i] = (float) ft[i * 8 + (i == 7 ? 7 : 0)];
            glide_trace_floats("grFogTable new (0,8..56,63)", e, 8);
        }
    }
#endif
    g2_grFogTable(ft);
}

void GLIDE_API g3_grLoadGammaTable(FxU32 nentries, FxU32 *red, FxU32 *green, FxU32 *blue) {
    GLIDE_TRACE("grLoadGammaTable");
    (void) red;
    (void) green;
    (void) blue;
    log_once("grLoadGammaTable not supported, entries", nentries);
}

void GLIDE_API g3_grSplash(float x, float y, float width, float height, FxU32 frame) {
    GLIDE_TRACE("grSplash");
    (void) x;
    (void) y;
    (void) width;
    (void) height;
    (void) frame;
}

/* One gamma for all channels: the output has a single table
 * (glide_set_gamma). */
void GLIDE_API g3_guGammaCorrectionRGB(float red, float green, float blue) {
    GLIDE_TRACE("guGammaCorrectionRGB");
    glide_set_gamma((red + green + blue) / 3.0f);
}

/* --- Textures ------------------------------------------------------------ */

FxU32 GLIDE_API g3_grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax, GrAspectRatio_t aspect,
                                        GrTextureFormat_t fmt) {
    return g2_grTexCalcMemRequired(lod_g2(lodmin), lod_g2(lodmax), aspect_g2(aspect), fmt);
}

FxU32 GLIDE_API g3_grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo *info) {
    GrTexInfo i2;
    if (!info)
        return 0;
    return g2_grTexTextureMemRequired(evenOdd, info_g2(info, &i2));
}

FxU32 GLIDE_API g3_grTexMinAddress(GrChipID_t tmu) {
    return g2_grTexMinAddress(tmu);
}

FxU32 GLIDE_API g3_grTexMaxAddress(GrChipID_t tmu) {
    return g2_grTexMaxAddress(tmu);
}

/* Glide 3 has no TMU argument for tables: every TMU gets them */
void GLIDE_API g3_grTexNCCTable(GrNCCTable_t table) {
    for (int t = 0; t < g_num_tmus; t++)
        g2_grTexNCCTable(t, table);
}

void GLIDE_API g3_grTexDownloadTable(GrTexTable_t type, void *data) {
    for (int t = 0; t < g_num_tmus; t++)
        g2_grTexDownloadTable(t, type, data);
}

void GLIDE_API g3_grTexDownloadTablePartial(GrTexTable_t type, void *data, int start, int end) {
    for (int t = 0; t < g_num_tmus; t++)
        g2_grTexDownloadTablePartial(t, type, data, start, end);
}

void GLIDE_API g3_grTexSource(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GrTexInfo i2;
    if (!info)
        return;
    LOG_DISTINCT("g3 grTexSource tmu*16+evenOdd / large*256+small*16+fmt", tmu * 16 + evenOdd,
                 ((uint32_t) info->largeLod & 0xf) * 256 + ((uint32_t) info->smallLod & 0xf) * 16 + info->format);
    g2_grTexSource(tmu, startAddress, evenOdd, info_g2(info, &i2));
}

void GLIDE_API g3_grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t s, GrTextureClampMode_t t) {
    g2_grTexClampMode(tmu, s, t);
}

void GLIDE_API g3_grTexCombine(GrChipID_t tmu, GrCombineFunction_t rf, GrCombineFactor_t rfa, GrCombineFunction_t af,
                               GrCombineFactor_t afa, FxBool rinv, FxBool ainv) {
    LOG_DISTINCT("g3 grTexCombine tmu*256+rf*16+rfa / af*16+afa (+inv 0x100/0x200)", tmu * 256 + rf * 16 + rfa,
                 af * 16 + afa + (rinv ? 0x100 : 0) + (ainv ? 0x200 : 0));
    g2_grTexCombine(tmu, rf, rfa, af, afa, rinv, ainv);
}

void GLIDE_API g3_grTexDetailControl(GrChipID_t tmu, int lod_bias, FxU8 detail_scale, float detail_max) {
    LOG_DISTINCT("g3 grTexDetailControl tmu*256+scale / bias*1000+max*255", tmu * 256 + detail_scale,
                 (uint32_t) (lod_bias * 1000 + (int) (detail_max * 255.0f)));
    g2_grTexDetailControl(tmu, lod_bias, detail_scale, detail_max);
}

void GLIDE_API g3_grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minf, GrTextureFilterMode_t magf) {
    g2_grTexFilterMode(tmu, minf, magf);
}

void GLIDE_API g3_grTexLodBiasValue(GrChipID_t tmu, float bias) {
    g2_grTexLodBiasValue(tmu, bias);
}

void GLIDE_API g3_grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GrTexInfo i2;
    if (!info)
        return;
    g2_grTexDownloadMipMap(tmu, startAddress, evenOdd, info_g2(info, &i2));
}

void GLIDE_API g3_grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
                                           GrAspectRatio_t aspect, GrTextureFormat_t format, FxU32 evenOdd,
                                           void *data) {
    g2_grTexDownloadMipMapLevel(tmu, startAddress, lod_g2(thisLod), lod_g2(largeLod), aspect_g2(aspect), format,
                                evenOdd, data);
}

FxBool GLIDE_API g3_grTexDownloadMipMapLevelPartial(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                                                    GrLOD_t largeLod, GrAspectRatio_t aspect,
                                                    GrTextureFormat_t format, FxU32 evenOdd, void *data, int start,
                                                    int end) {
    g2_grTexDownloadMipMapLevelPartial(tmu, startAddress, lod_g2(thisLod), lod_g2(largeLod), aspect_g2(aspect),
                                       format, evenOdd, data, start, end);
    return FXTRUE;
}

void GLIDE_API g3_grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool lodBlend) {
    LOG_DISTINCT("g3 grTexMipMapMode tmu / mode*16+lodBlend", tmu, mode * 16 + lodBlend);
    g2_grTexMipMapMode(tmu, mode, lodBlend);
}

void GLIDE_API g3_grTexMultibase(GrChipID_t tmu, FxBool enable) {
    LOG_DISTINCT("g3 grTexMultibase tmu / enable", tmu, enable);
    g2_grTexMultibase(tmu, enable);
}

/* GR_TEXBASE_* count the other way round in Glide 3 (256 = 3) */
void GLIDE_API g3_grTexMultibaseAddress(GrChipID_t tmu, GrTexBaseRange_t range, FxU32 startAddress, FxU32 evenOdd,
                                        GrTexInfo *info) {
    GrTexInfo i2;
    g2_grTexMultibaseAddress(tmu, GR_TEXBASE_32_TO_1 - range, startAddress, evenOdd, info_g2(info, &i2));
}

/* --- Linear frame buffer ----------------------------------------------- */

FxBool GLIDE_API g3_grLfbLock(GrLock_t type, GrBuffer_t buffer, GrLfbWriteMode_t writeMode,
                              GrOriginLocation_t origin, FxBool pixelPipeline, GrLfbInfo_t *info) {
    return g2_grLfbLock(type, buffer, writeMode, origin, pixelPipeline, info);
}

FxBool GLIDE_API g3_grLfbUnlock(GrLock_t type, GrBuffer_t buffer) {
    return g2_grLfbUnlock(type, buffer);
}

void GLIDE_API g3_grLfbConstantAlpha(GrAlpha_t alpha) {
    g2_grLfbConstantAlpha(alpha);
}

void GLIDE_API g3_grLfbConstantDepth(FxU32 depth) {
    g2_grLfbConstantDepth((FxU16) depth);
}

void GLIDE_API g3_grLfbWriteColorSwizzle(FxBool swizzleBytes, FxBool swapWords) {
    g2_grLfbWriteColorSwizzle(swizzleBytes, swapWords);
}

void GLIDE_API g3_grLfbWriteColorFormat(GrColorFormat_t colorFormat) {
    g2_grLfbWriteColorFormat(colorFormat);
}

/* Glide 3 adds pixelPipeline; the Glide 2 region write never used the
 * pixel pipeline either */
FxBool GLIDE_API g3_grLfbWriteRegion(GrBuffer_t dst_buffer, FxU32 dst_x, FxU32 dst_y, GrLfbSrcFmt_t src_format,
                                     FxU32 src_width, FxU32 src_height, FxBool pixelPipeline, FxI32 src_stride,
                                     void *src_data) {
    if (pixelPipeline)
        log_once("grLfbWriteRegion through the pixel pipeline not supported", src_format);
    return g2_grLfbWriteRegion(dst_buffer, dst_x, dst_y, src_format, src_width, src_height, src_stride, src_data);
}

FxBool GLIDE_API g3_grLfbReadRegion(GrBuffer_t src_buffer, FxU32 src_x, FxU32 src_y, FxU32 src_width,
                                    FxU32 src_height, FxU32 dst_stride, void *dst_data) {
    return g2_grLfbReadRegion(src_buffer, src_x, src_y, src_width, src_height, dst_stride, dst_data);
}

/* --- Utility functions ---------------------------------------------------- */

float GLIDE_API g3_guFogTableIndexToW(int i) {
    return g2_guFogTableIndexToW(i);
}

void GLIDE_API g3_guFogGenerateExp(GrFog_t *fogtable, float density) {
    g2_guFogGenerateExp(fogtable, density);
}

void GLIDE_API g3_guFogGenerateExp2(GrFog_t *fogtable, float density) {
    g2_guFogGenerateExp2(fogtable, density);
}

void GLIDE_API g3_guFogGenerateLinear(GrFog_t *fogtable, float nearZ, float farZ) {
    g2_guFogGenerateLinear(fogtable, nearZ, farZ);
}

/* The 3DF header holds Glide 3 log2 sizes */
static void header_g3(Gu3dfInfo *info) {
    info->header.small_lod    = lod_g3(info->header.small_lod);
    info->header.large_lod    = lod_g3(info->header.large_lod);
    info->header.aspect_ratio = aspect_g3(info->header.aspect_ratio);
}

FxBool GLIDE_API g3_gu3dfGetInfo(const char *filename, Gu3dfInfo *info) {
    if (!g2_gu3dfGetInfo(filename, info))
        return FXFALSE;
    header_g3(info);
    return FXTRUE;
}

FxBool GLIDE_API g3_gu3dfLoad(const char *filename, Gu3dfInfo *data) {
    /* gu3dfLoad reads the header itself; it comes back in Glide 2 form */
    if (!g2_gu3dfLoad(filename, data))
        return FXFALSE;
    header_g3(data);
    return FXTRUE;
}

#endif /* GLIDULATOR_GLIDE3 */
