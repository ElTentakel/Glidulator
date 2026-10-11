/*
 * Fog: grFogMode, grFogColorValue, grFogTable and the table generators
 * guFogGenerateExp/Exp2/Linear plus guFogTableIndexToW.
 *
 * The rasterizer implements fog like the board (APPLY_FOG in
 * core/rasterizer/vid_voodoo_render.h); only the registers are set here:
 * fogMode, fogColor and the 64 fog table entries. The table is indexed by
 * the pixel's W depth (4-bit exponent, 2-bit mantissa of 1/oow) and
 * interpolated linearly between two entries, which is why the hardware
 * stores the difference to the next entry with each one (dfog, 6.2 fixed
 * point).
 */

#include "glide_internal.h"
#include "glidulator/trace.h"

#include <math.h>

/* fogMode bits (core/rasterizer/vid_voodoo_regs.h: FOG_ENABLE, FOG_ADD,
 * FOG_MULT, FOG_ALPHA, FOG_Z, FOG_CONSTANT). */
#define FOG_BITS (FOG_ENABLE | FOG_ADD | FOG_MULT | FOG_ALPHA | FOG_Z | FOG_CONSTANT)

/* Lower 8 bits: source of the fog amount (table by W, iterated alpha,
 * iterated Z). GR_FOG_MULT2/ADD2: do not blend with the fog colour but only
 * attenuate the colour or only add fog (for multi-pass rendering). */
void GR_EXPORT GLIDE_API grFogMode(GrFogMode_t mode) {
    GLIDE_TRACE("grFogMode");
    uint32_t fm = g_voodoo.params.fogMode & ~(uint32_t) FOG_BITS;
    switch (mode & 0xff) {
    case GR_FOG_WITH_ITERATED_ALPHA: fm |= FOG_ENABLE | FOG_ALPHA; break;
    case GR_FOG_WITH_TABLE: fm |= FOG_ENABLE; break;
    case GR_FOG_WITH_ITERATED_Z: fm |= FOG_ENABLE | FOG_Z; break;
    default: break; /* GR_FOG_DISABLE */
    }
    if (mode & GR_FOG_MULT2)
        fm |= FOG_MULT;
    if (mode & GR_FOG_ADD2)
        fm |= FOG_ADD;
    g_voodoo.params.fogMode = fm;
}

void GR_EXPORT GLIDE_API grFogColorValue(GrColor_t fogcolor) {
    GLIDE_TRACE("grFogColorValue");
    uint32_t argb             = glide_color_to_argb(fogcolor);
    g_voodoo.params.fogColor.r = (uint8_t) (argb >> 16);
    g_voodoo.params.fogColor.g = (uint8_t) (argb >> 8);
    g_voodoo.params.fogColor.b = (uint8_t) argb;
}

/* 64 entries; dfog = (next - this) * 4 (6.2 fixed point, as a byte modulo
 * 256 like the register), 0 for the last entry. */
void GR_EXPORT GLIDE_API grFogTable(const GrFog_t *ft) {
    GLIDE_TRACE("grFogTable");
    if (!ft)
        return;
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++) {
        int next                          = (i + 1 < GR_FOG_TABLE_SIZE) ? ft[i + 1] : ft[i];
        g_voodoo.params.fogTable[i].fog  = ft[i];
        g_voodoo.params.fogTable[i].dfog = (uint8_t) ((next - ft[i]) << 2);
    }
}

/* W (distance) at table index i: exponent i/4, mantissa i%4, exactly the
 * steps at which the hardware reads the table. */
float GR_EXPORT GLIDE_API guFogTableIndexToW(int i) {
    GLIDE_TRACE("guFogTableIndexToW");
    return (float) (pow(2.0, 3.0 + (double) (i >> 2)) / (double) (8 - (i & 3)));
}

static GrFog_t fog_byte(double f) {
    if (f > 1.0)
        f = 1.0;
    if (f < 0.0)
        f = 0.0;
    return (GrFog_t) (f * 255.0);
}

/* Exponential fog 1 - e^(-d*w), normalised so that the last entry is
 * 255. */
void GR_EXPORT GLIDE_API guFogGenerateExp(GrFog_t *fogtable, float density) {
    GLIDE_TRACE("guFogGenerateExp");
    double scale = 1.0 / (1.0 - exp(-(double) density * guFogTableIndexToW(GR_FOG_TABLE_SIZE - 1)));
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
        fogtable[i] = fog_byte((1.0 - exp(-(double) density * guFogTableIndexToW(i))) * scale);
}

/* As above with (d*w)^2. */
void GR_EXPORT GLIDE_API guFogGenerateExp2(GrFog_t *fogtable, float density) {
    GLIDE_TRACE("guFogGenerateExp2");
    double dl    = (double) density * guFogTableIndexToW(GR_FOG_TABLE_SIZE - 1);
    double scale = 1.0 / (1.0 - exp(-dl * dl));
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++) {
        double d = (double) density * guFogTableIndexToW(i);
        fogtable[i] = fog_byte((1.0 - exp(-d * d)) * scale);
    }
}

/* Linear between nearZ (no fog) and farZ (full fog), in W units. */
void GR_EXPORT GLIDE_API guFogGenerateLinear(GrFog_t *fogtable, float nearZ, float farZ) {
    GLIDE_TRACE("guFogGenerateLinear");
    double range = (double) farZ - (double) nearZ;
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++) {
        double w = guFogTableIndexToW(i);
        if (w > 65535.0)
            w = 65535.0;
        fogtable[i] = fog_byte(range != 0.0 ? (w - nearZ) / range : (w >= farZ ? 1.0 : 0.0));
    }
}

void glide_fog_reset(void) {
    g_voodoo.params.fogMode = 0;
    g_voodoo.params.fogColor.r = g_voodoo.params.fogColor.g = g_voodoo.params.fogColor.b = 0;
    for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
        g_voodoo.params.fogTable[i].fog = g_voodoo.params.fogTable[i].dfog = 0;
}
