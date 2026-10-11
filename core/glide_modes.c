/*
 * Glide state functions that set hardware registers: colour/alpha/texture
 * combine, constant colour, alpha blending and test, depth buffer,
 * dithering, culling, colour mask, chroma key, hints. See
 * core/glide_internal.h for the principle (registers = g_voodoo.params).
 *
 * The mapping of Glide parameters to register bits follows the SST-1
 * register layout as the rasterizer evaluates it (vid_voodoo_regs.h: cc_,
 * cca_, tc_ and tca_ macros, FBZ_*, alphaMode fields). Glide's constants
 * (GR_CMP_*, GR_BLEND_*, GR_COMBINE_FACTOR_*) are deliberately identical to
 * the hardware field values; Glide passes them through unchanged.
 */

#include "glide_internal.h"
#include "glidulator/trace.h"

#include <math.h>

glide_sw_state g_sw;

uint32_t glide_color_to_argb(GrColor_t c) {
    uint32_t a, r, g, b;
    switch (g_sw.color_format) {
    case GR_COLORFORMAT_ABGR:
        a = c >> 24; b = (c >> 16) & 0xff; g = (c >> 8) & 0xff; r = c & 0xff;
        break;
    case GR_COLORFORMAT_RGBA:
        r = c >> 24; g = (c >> 16) & 0xff; b = (c >> 8) & 0xff; a = c & 0xff;
        break;
    case GR_COLORFORMAT_BGRA:
        b = c >> 24; g = (c >> 16) & 0xff; r = (c >> 8) & 0xff; a = c & 0xff;
        break;
    default: /* GR_COLORFORMAT_ARGB */
        return c;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* --- Combine ---------------------------------------------------------- */

/* All four combine units (colour and alpha in fbzColorPath, texture RGB
 * and alpha in textureMode) share the same bit layout relative to their
 * start bit (vid_voodoo_regs.h):
 *   +0 zero_other  +1 sub_clocal  +2..+4 mselect  +5 reverse_blend
 *   +6 add_clocal  +7 add_alocal  +8 invert_output */
#define CMB_ZERO_OTHER (1u << 0)
#define CMB_SUB_CLOCAL (1u << 1)
#define CMB_MSELECT(f) (((uint32_t) (f) & 7u) << 2)
#define CMB_REVERSE    (1u << 5)
#define CMB_ADD_CLOCAL (1u << 6)
#define CMB_ADD_ALOCAL (1u << 7)
#define CMB_INVERT     (1u << 8)
#define CMB_MASK       0x1ffu

#define CP_COLOR_SHIFT 8  /* cc_*  in fbzColorPath */
#define CP_ALPHA_SHIFT 17 /* cca_* in fbzColorPath */
#define TM_RGB_SHIFT   12 /* tc_*  in textureMode */
#define TM_ALPHA_SHIFT 21 /* tca_* in textureMode */

/* Glide function + factor -> relative combine bits. The factor selects
 * mselect (lower 3 bits); "ONE_MINUS_*" (bit 3) means reverse_blend = 0 in
 * hardware: the rasterizer inverts the factor when reverse_blend is NOT
 * set (vid_voodoo_render.c: msel ^= 0xff). */
static uint32_t combine_bits(GrCombineFunction_t function, GrCombineFactor_t factor, FxBool invert) {
    uint32_t bits = CMB_MSELECT(factor) | ((factor & 8) ? 0 : CMB_REVERSE);
    switch (function) {
    case GR_COMBINE_FUNCTION_ZERO:
        bits = CMB_ZERO_OTHER | CMB_REVERSE; /* mselect 0 + reverse = factor 0 */
        break;
    case GR_COMBINE_FUNCTION_LOCAL:
        bits = CMB_ZERO_OTHER | CMB_REVERSE | CMB_ADD_CLOCAL;
        break;
    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        bits = CMB_ZERO_OTHER | CMB_REVERSE | CMB_ADD_ALOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
        bits |= CMB_ADD_CLOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        bits |= CMB_ADD_ALOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        bits |= CMB_SUB_CLOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
        bits |= CMB_SUB_CLOCAL | CMB_ADD_CLOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        bits |= CMB_SUB_CLOCAL | CMB_ADD_ALOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
        bits |= CMB_ZERO_OTHER | CMB_SUB_CLOCAL | CMB_ADD_CLOCAL;
        break;
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        bits |= CMB_ZERO_OTHER | CMB_SUB_CLOCAL | CMB_ADD_ALOCAL;
        break;
    default:
        break;
    }
    if (invert)
        bits |= CMB_INVERT;
    return bits;
}

#define FBZCP_RGB_SEL_MASK   (3u << 0)
#define FBZCP_A_SEL_MASK     (3u << 2)
#define FBZCP_CC_LOCAL       (1u << 4)
#define FBZCP_CCA_LOCAL_MASK (3u << 5)
#define FBZCP_CC_LOCAL_OVR   (1u << 7)

void glide_update_texture_enable(void) {
    uint32_t cp      = g_voodoo.params.fbzColorPath;
    int      uses    = ((cp & 3) == CC_LOCALSELECT_TEX) || (((cp >> 2) & 3) == A_SEL_TEX) ||
                       (((cp >> 10) & 7) == CC_MSELECT_TEX) || (((cp >> 19) & 7) == CCA_MSELECT_TEX);
    if (uses)
        g_voodoo.params.fbzColorPath |= FBZCP_TEXTURE_ENABLED;
    else
        g_voodoo.params.fbzColorPath &= ~(uint32_t) FBZCP_TEXTURE_ENABLED;
}

static uint32_t other_select(GrCombineOther_t other) {
    switch (other) {
    case GR_COMBINE_OTHER_TEXTURE: return CC_LOCALSELECT_TEX;
    case GR_COMBINE_OTHER_CONSTANT: return CC_LOCALSELECT_COLOR1;
    default: return CC_LOCALSELECT_ITER_RGB;
    }
}

void GR_EXPORT GLIDE_API grColorCombine(GrCombineFunction_t function, GrCombineFactor_t factor,
                                        GrCombineLocal_t local, GrCombineOther_t other, FxBool invert) {
    GLIDE_TRACE("grColorCombine");
    uint32_t cp = g_voodoo.params.fbzColorPath;
    cp &= ~((CMB_MASK << CP_COLOR_SHIFT) | FBZCP_RGB_SEL_MASK | FBZCP_CC_LOCAL);
    cp |= combine_bits(function, factor, invert) << CP_COLOR_SHIFT;
    cp |= other_select(other);
    if (local == GR_COMBINE_LOCAL_CONSTANT)
        cp |= FBZCP_CC_LOCAL; /* color0 instead of the iterated colour */
    g_voodoo.params.fbzColorPath = cp;
    glide_update_texture_enable();
}

void GR_EXPORT GLIDE_API grAlphaCombine(GrCombineFunction_t function, GrCombineFactor_t factor,
                                        GrCombineLocal_t local, GrCombineOther_t other, FxBool invert) {
    GLIDE_TRACE("grAlphaCombine");
    uint32_t cp = g_voodoo.params.fbzColorPath;
    cp &= ~((CMB_MASK << CP_ALPHA_SHIFT) | FBZCP_A_SEL_MASK | FBZCP_CCA_LOCAL_MASK);
    cp |= combine_bits(function, factor, invert) << CP_ALPHA_SHIFT;
    cp |= other_select(other) << 2; /* a_sel: same codes as rgb_sel */
    switch (local) {
    case GR_COMBINE_LOCAL_CONSTANT: cp |= (uint32_t) CCA_LOCALSELECT_COLOR0 << 5; break;
    case GR_COMBINE_LOCAL_DEPTH: cp |= (uint32_t) CCA_LOCALSELECT_ITER_Z << 5; break;
    default: break; /* iterated alpha */
    }
    g_voodoo.params.fbzColorPath = cp;
    glide_update_texture_enable();
}

/* With one TMU (Voodoo Graphics) the rasterizer does not evaluate the TMU
 * combine bits ("only one TMU, only sample TMU0"); they are set correctly
 * anyway so that the register image stays faithful (grGlideGetState). */
void GR_EXPORT GLIDE_API grTexCombine(GrChipID_t tmu, GrCombineFunction_t rgb_function, GrCombineFactor_t rgb_factor,
                                      GrCombineFunction_t alpha_function, GrCombineFactor_t alpha_factor,
                                      FxBool rgb_invert, FxBool alpha_invert) {
    GLIDE_TRACE("grTexCombine");
    if (tmu != GR_TMU0)
        return;
    uint32_t tm = g_voodoo.params.textureMode[0];
    tm &= ~((CMB_MASK << TM_RGB_SHIFT) | (CMB_MASK << TM_ALPHA_SHIFT));
    tm |= combine_bits(rgb_function, rgb_factor, rgb_invert) << TM_RGB_SHIFT;
    tm |= combine_bits(alpha_function, alpha_factor, alpha_invert) << TM_ALPHA_SHIFT;
    g_voodoo.params.textureMode[0] = tm;
}

/* Glide writes the value into BOTH constant colour registers: color0
 * (local constant) and color1 (GR_COMBINE_OTHER_CONSTANT). */
void GR_EXPORT GLIDE_API grConstantColorValue(GrColor_t value) {
    GLIDE_TRACE("grConstantColorValue");
    uint32_t argb                 = glide_color_to_argb(value);
    g_voodoo.params.color0        = argb;
    g_voodoo.params.color1        = argb;
}

/* As in 3dfx's Glide 2.x (SST-1, "obsolete routine"): sets NOT color0 but
 * only the colour for the DELTA0 mode of guColorCombineFunction (iterated
 * colour without gradient). Alpha is only remembered, as in 3dfx's code. */
void GR_EXPORT GLIDE_API grConstantColorValue4(float a, float r, float g, float b) {
    GLIDE_TRACE("grConstantColorValue4");
    (void) a;
    g_sw.delta0_r = r;
    g_sw.delta0_g = g;
    g_sw.delta0_b = b;
}

/* Bit 7 cc_localselect_override: texture alpha bit 7 selects between the
 * iterated colour and color0 as "local". */
void GR_EXPORT GLIDE_API grAlphaControlsITRGBLighting(FxBool enable) {
    GLIDE_TRACE("grAlphaControlsITRGBLighting");
    if (enable)
        g_voodoo.params.fbzColorPath |= FBZCP_CC_LOCAL_OVR;
    else
        g_voodoo.params.fbzColorPath &= ~FBZCP_CC_LOCAL_OVR;
}

/* --- Alpha blending and test (alphaMode) ---------------------------- */

#define AM_TEST_ENABLE  (1u << 0)
#define AM_TEST_FUNC(f) (((uint32_t) (f) & 7u) << 1)
#define AM_BLEND_ENABLE (1u << 4)

void GR_EXPORT GLIDE_API grAlphaBlendFunction(GrAlphaBlendFnc_t rgb_sf, GrAlphaBlendFnc_t rgb_df,
                                              GrAlphaBlendFnc_t alpha_sf, GrAlphaBlendFnc_t alpha_df) {
    GLIDE_TRACE("grAlphaBlendFunction");
    uint32_t am = g_voodoo.params.alphaMode & ~(0xffff00u | AM_BLEND_ENABLE);
    am |= ((uint32_t) rgb_sf & 0xf) << 8 | ((uint32_t) rgb_df & 0xf) << 12 |
          ((uint32_t) alpha_sf & 0xf) << 16 | ((uint32_t) alpha_df & 0xf) << 20;
    /* ONE/ZERO means "no blending": then do not enable it at all (saves
     * a framebuffer read per pixel). */
    if (!(rgb_sf == GR_BLEND_ONE && rgb_df == GR_BLEND_ZERO && alpha_sf == GR_BLEND_ONE && alpha_df == GR_BLEND_ZERO))
        am |= AM_BLEND_ENABLE;
    g_voodoo.params.alphaMode = am;
}

void GR_EXPORT GLIDE_API grAlphaTestFunction(GrCmpFnc_t function) {
    GLIDE_TRACE("grAlphaTestFunction");
    uint32_t am = g_voodoo.params.alphaMode & ~(AM_TEST_ENABLE | AM_TEST_FUNC(7));
    if (function != GR_CMP_ALWAYS)
        am |= AM_TEST_ENABLE | AM_TEST_FUNC(function);
    g_voodoo.params.alphaMode = am;
}

void GR_EXPORT GLIDE_API grAlphaTestReferenceValue(GrAlpha_t value) {
    GLIDE_TRACE("grAlphaTestReferenceValue");
    g_voodoo.params.alphaMode = (g_voodoo.params.alphaMode & 0x00ffffffu) | ((uint32_t) value << 24);
}

/* --- Depth buffer and other fbzMode bits ---------------------------- */

#define FBZ_CLIPPING    (1u << 0)
#define FBZ_DEPTH_FUNC(f) (((uint32_t) (f) & 7u) << 5)

void GR_EXPORT GLIDE_API grDepthBufferMode(GrDepthBufferMode_t mode) {
    GLIDE_TRACE("grDepthBufferMode");
    uint32_t m = g_voodoo.params.fbzMode & ~(uint32_t) (FBZ_DEPTH_ENABLE | FBZ_W_BUFFER | FBZ_DEPTH_SOURCE | FBZ_DEPTH_BIAS);
    switch (mode) {
    case GR_DEPTHBUFFER_ZBUFFER:
        m |= FBZ_DEPTH_ENABLE | FBZ_DEPTH_BIAS;
        break;
    case GR_DEPTHBUFFER_WBUFFER:
        m |= FBZ_DEPTH_ENABLE | FBZ_W_BUFFER | FBZ_DEPTH_BIAS;
        break;
    case GR_DEPTHBUFFER_ZBUFFER_COMPARE_TO_BIAS:
        m |= FBZ_DEPTH_ENABLE | FBZ_DEPTH_SOURCE;
        break;
    case GR_DEPTHBUFFER_WBUFFER_COMPARE_TO_BIAS:
        m |= FBZ_DEPTH_ENABLE | FBZ_W_BUFFER | FBZ_DEPTH_SOURCE;
        break;
    default: /* GR_DEPTHBUFFER_DISABLE */
        break;
    }
    g_voodoo.params.fbzMode = m;
}

void GR_EXPORT GLIDE_API grDepthBufferFunction(GrCmpFnc_t function) {
    GLIDE_TRACE("grDepthBufferFunction");
    g_voodoo.params.fbzMode = (g_voodoo.params.fbzMode & ~FBZ_DEPTH_FUNC(7)) | FBZ_DEPTH_FUNC(function);
}

void GR_EXPORT GLIDE_API grDepthMask(FxBool mask) {
    GLIDE_TRACE("grDepthMask");
    if (mask)
        g_voodoo.params.fbzMode |= FBZ_DEPTH_WMASK;
    else
        g_voodoo.params.fbzMode &= ~(uint32_t) FBZ_DEPTH_WMASK;
}

/* zaColor, lower 16 bits: depth bias, or the comparison value for
 * *_COMPARE_TO_BIAS. */
void GR_EXPORT GLIDE_API grDepthBiasLevel(FxI16 level) {
    GLIDE_TRACE("grDepthBiasLevel");
    g_voodoo.params.zaColor = (g_voodoo.params.zaColor & 0xffff0000u) | (uint16_t) level;
}

void GR_EXPORT GLIDE_API grDitherMode(GrDitherMode_t mode) {
    GLIDE_TRACE("grDitherMode");
    uint32_t m = g_voodoo.params.fbzMode & ~(uint32_t) (FBZ_DITHER | FBZ_DITHER_2x2);
    if (mode == GR_DITHER_2x2)
        m |= FBZ_DITHER | FBZ_DITHER_2x2;
    else if (mode == GR_DITHER_4x4)
        m |= FBZ_DITHER;
    g_voodoo.params.fbzMode = m;
}

/* Glide culls in software (grDrawTriangle), not in hardware; only
 * remember the mode here. */
void GR_EXPORT GLIDE_API grCullMode(GrCullMode_t mode) {
    GLIDE_TRACE("grCullMode");
    g_sw.cull_mode = mode;
}

/* There are no alpha planes: only the RGB mask has an effect. */
void GR_EXPORT GLIDE_API grColorMask(FxBool rgb, FxBool a) {
    GLIDE_TRACE("grColorMask");
    (void) a;
    if (rgb)
        g_voodoo.params.fbzMode |= FBZ_RGB_WMASK;
    else
        g_voodoo.params.fbzMode &= ~(uint32_t) FBZ_RGB_WMASK;
}

void GR_EXPORT GLIDE_API grChromakeyMode(GrChromakeyMode_t mode) {
    GLIDE_TRACE("grChromakeyMode");
    if (mode == GR_CHROMAKEY_ENABLE)
        g_voodoo.params.fbzMode |= FBZ_CHROMAKEY;
    else
        g_voodoo.params.fbzMode &= ~(uint32_t) FBZ_CHROMAKEY;
}

void GR_EXPORT GLIDE_API grChromakeyValue(GrColor_t value) {
    GLIDE_TRACE("grChromakeyValue");
    uint32_t argb               = glide_color_to_argb(value);
    g_voodoo.params.chromaKey   = argb & 0xffffff;
    g_voodoo.params.chromaKey_r = (int) ((argb >> 16) & 0xff);
    g_voodoo.params.chromaKey_g = (int) ((argb >> 8) & 0xff);
    g_voodoo.params.chromaKey_b = (int) (argb & 0xff);
}

/* Only the STW hint has an effect (which vertex fields are the TMU's
 * W/S/T, see grDrawTriangle). FIFO/FPU/LFB hints concern hardware details
 * that do not exist here. */
void GR_EXPORT GLIDE_API grHints(GrHint_t hintType, FxU32 hintMask) {
    GLIDE_TRACE("grHints");
    if (hintType == GR_HINT_STWHINT)
        g_sw.stw_hint = hintMask;
    else if (hintType == GR_HINT_ALLOW_MIPMAP_DITHER)
        g_sw.allow_lod_dither = hintMask; /* otherwise no LOD dithering, see tex_recalc() */
}

/* --- gu presets for colour/alpha combine ---------------------------- */

/* Fixed grAlphaCombine combinations; mapping checked against 3dfx's
 * Glide 2.x source (sst1/glide/src/gu.c). */
void GR_EXPORT GLIDE_API guAlphaSource(GrAlphaSource_t mode) {
    GLIDE_TRACE("guAlphaSource");
    switch (mode) {
    case GR_ALPHASOURCE_CC_ALPHA:
        grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT,
                       GR_COMBINE_OTHER_NONE, FXFALSE);
        break;
    case GR_ALPHASOURCE_ITERATED_ALPHA:
        grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED,
                       GR_COMBINE_OTHER_NONE, FXFALSE);
        break;
    case GR_ALPHASOURCE_TEXTURE_ALPHA:
        grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                       GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        break;
    case GR_ALPHASOURCE_TEXTURE_ALPHA_TIMES_ITERATED_ALPHA:
        grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_LOCAL_ITERATED,
                       GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        break;
    default: break;
    }
}

/* Fixed grColorCombine combinations (as in 3dfx's gu.c). The _DELTA0
 * variants use the colour from grConstantColorValue4 as "iterated" colour,
 * without gradient, instead of the vertex colours. */
void GR_EXPORT GLIDE_API guColorCombineFunction(GrColorCombineFnc_t fnc) {
    GLIDE_TRACE("guColorCombineFunction");
    GrCombineFunction_t f      = GR_COMBINE_FUNCTION_SCALE_OTHER;
    GrCombineFactor_t   factor = GR_COMBINE_FACTOR_LOCAL;
    GrCombineLocal_t    local  = GR_COMBINE_LOCAL_ITERATED;
    GrCombineOther_t    other  = GR_COMBINE_OTHER_TEXTURE;
    FxBool              invert = FXFALSE;
    g_sw.cc_delta0             = 0;
    switch (fnc) {
    case GR_COLORCOMBINE_ZERO:
        f = GR_COMBINE_FUNCTION_ZERO; factor = GR_COMBINE_FACTOR_NONE;
        local = GR_COMBINE_LOCAL_NONE; other = GR_COMBINE_OTHER_NONE;
        break;
    case GR_COLORCOMBINE_CCRGB:
        f = GR_COMBINE_FUNCTION_LOCAL; factor = GR_COMBINE_FACTOR_NONE;
        local = GR_COMBINE_LOCAL_CONSTANT; other = GR_COMBINE_OTHER_NONE;
        break;
    case GR_COLORCOMBINE_ITRGB_DELTA0:
        g_sw.cc_delta0 = 1;
        /* fall through */
    case GR_COLORCOMBINE_ITRGB:
        f = GR_COMBINE_FUNCTION_LOCAL; factor = GR_COMBINE_FACTOR_NONE; other = GR_COMBINE_OTHER_NONE;
        break;
    case GR_COLORCOMBINE_DECAL_TEXTURE:
        factor = GR_COMBINE_FACTOR_ONE; local = GR_COMBINE_LOCAL_NONE;
        break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_CCRGB: local = GR_COMBINE_LOCAL_CONSTANT; break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_DELTA0:
        g_sw.cc_delta0 = 1;
        break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB: break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA;
        break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA:
        factor = GR_COMBINE_FACTOR_LOCAL_ALPHA; local = GR_COMBINE_LOCAL_NONE;
        break;
    case GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA_ADD_ITRGB:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL; factor = GR_COMBINE_FACTOR_LOCAL_ALPHA;
        break;
    case GR_COLORCOMBINE_TEXTURE_ADD_ITRGB:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL; factor = GR_COMBINE_FACTOR_ONE;
        break;
    case GR_COLORCOMBINE_TEXTURE_SUB_ITRGB:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL; factor = GR_COMBINE_FACTOR_ONE;
        break;
    case GR_COLORCOMBINE_CCRGB_BLEND_ITRGB_ON_TEXALPHA:
        f = GR_COMBINE_FUNCTION_BLEND; factor = GR_COMBINE_FACTOR_TEXTURE_ALPHA;
        local = GR_COMBINE_LOCAL_CONSTANT; other = GR_COMBINE_OTHER_ITERATED;
        break;
    case GR_COLORCOMBINE_DIFF_SPEC_A:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL; factor = GR_COMBINE_FACTOR_LOCAL_ALPHA;
        break;
    case GR_COLORCOMBINE_DIFF_SPEC_B:
        f = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA;
        break;
    case GR_COLORCOMBINE_ONE:
        f = GR_COMBINE_FUNCTION_ZERO; factor = GR_COMBINE_FACTOR_NONE;
        local = GR_COMBINE_LOCAL_NONE; other = GR_COMBINE_OTHER_NONE; invert = FXTRUE;
        break;
    default: return;
    }
    grColorCombine(f, factor, local, other, invert);
}

/* --- Initial state ------------------------------------------------- */

/* Glide defaults after grSstWinOpen() per the Glide 2.x reference: colour
 * and alpha iterated, no blending/alpha test/chroma key, depth buffer off
 * (function LESS, mask off), 4x4 dithering, constant colour 0xFFFFFFFF,
 * culling off. */
void glide_modes_reset(void) {
    g_sw.cull_mode = GR_CULL_DISABLE;
    g_sw.stw_hint  = 0;
    g_sw.allow_lod_dither = 0;
    g_sw.cc_delta0 = 0;
    g_sw.delta0_r = g_sw.delta0_g = g_sw.delta0_b = 0.0f;

    /* Clipping on, RGB writes on; FBZ_PARAM_ADJUST (bit 26 in
     * fbzColorPath): move start values from the vertex to the first pixel
     * centre, as 3dfx's Glide always does. */
    g_voodoo.params.fbzMode      = FBZ_CLIPPING | FBZ_RGB_WMASK;
    g_voodoo.params.fbzColorPath = FBZ_PARAM_ADJUST;
    g_voodoo.params.alphaMode    = 0;
    g_voodoo.params.fogMode      = 0;
    g_voodoo.params.zaColor      = 0;

    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_ITERATED, FXFALSE);
    grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_ITERATED, FXFALSE);
    grConstantColorValue(0xffffffff);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    grAlphaTestFunction(GR_CMP_ALWAYS);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grDepthBufferFunction(GR_CMP_LESS);
    grDepthMask(FXFALSE);
    grDitherMode(GR_DITHER_4x4);
}
