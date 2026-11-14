/*
 * Glide 2.1.1 entry points (glide.dll): the API of the first Voodoo
 * Graphics games -- Scorched Planet, Pandemonium!, CyberGladiators,
 * MechWarrior 2 (3Dfx edition). They load glide.dll, not glide2x.dll.
 *
 * Glide 2.1.1 is Glide 2.x minus a few later additions, plus a handful of
 * functions 2.2+ dropped; everything else has the same name, signature and
 * meaning. glide.dll is the same core as glide2x.dll (targets/glide_dll/
 * glide.def = glide2x.def plus the names below, tools/abi/gen_glide211.py).
 * Added here:
 *   - grSstOpen: grSstWinOpen without a window handle;
 *   - grSstPassthruMode: VGA passthrough, nothing to switch here;
 *   - LFB access without locks: grLfbBegin/End, grLfbGetWritePtr/ReadPtr,
 *     grLfbWriteMode, grLfbBypassMode, grLfbOrigin, see core/glide_lfb.c.
 *
 * LFB writes go through the pixel pipeline unless grLfbBypassMode enabled
 * the bypass. The default is the pipeline (GR_LFBBYPASS_DISABLE): Scorched
 * Planet draws its in-game HUD (1555, alpha test drops the transparent
 * pixels) without ever calling grLfbBypassMode.
 */

#include "glidulator/glide2x.h"
#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include "glide_internal.h"

typedef FxI32 GrPassthruMode_t;

static GrLfbWriteMode_t   g_write_mode = GR_LFBWRITEMODE_565;
static GrLfbBypassMode_t  g_bypass     = GR_LFBBYPASS_DISABLE;
static GrOriginLocation_t g_lfb_origin = GR_ORIGIN_UPPER_LEFT;

FxBool GR_EXPORT GLIDE_API grSstOpen(GrScreenResolution_t screen_resolution, GrScreenRefresh_t refresh_rate,
                                     GrColorFormat_t color_format, GrOriginLocation_t origin_location,
                                     GrSmoothingMode_t smoothing_filter, int num_buffers) {
    GLIDE_TRACE("grSstOpen");
    (void) smoothing_filter; /* video output filter of the SST-1 DAC */
    g_write_mode = GR_LFBWRITEMODE_565;
    g_bypass     = GR_LFBBYPASS_DISABLE;
    g_lfb_origin = GR_ORIGIN_UPPER_LEFT;
    return grSstWinOpen(0, screen_resolution, refresh_rate, color_format, origin_location, num_buffers, 1);
}

/* On the board: the video switch between the VGA card's picture and the
 * Voodoo's. CyberGladiators plays its AVI intros on the VGA side. */
void GR_EXPORT GLIDE_API grSstPassthruMode(GrPassthruMode_t mode) {
    GLIDE_TRACE("grSstPassthruMode");
    glide_trace_note("grSstPassthruMode", (unsigned long) mode, 0);
    if (mode == GR_PASSTHRU_SHOW_VGA || mode == GR_PASSTHRU_SHOW_SST1)
        platform_show_output(mode == GR_PASSTHRU_SHOW_SST1);
}

/* Start of an LFB access (the board waited for the pipeline to drain): a
 * pointer the game kept from before is valid again. */
void GR_EXPORT GLIDE_API grLfbBegin(void) {
    GLIDE_TRACE("grLfbBegin");
    glide_lfb211_begin();
}

/* The writes stay pending until something needs the picture, see
 * core/glide_lfb.c. */
void GR_EXPORT GLIDE_API grLfbEnd(void) {
    GLIDE_TRACE("grLfbEnd");
    glide_lfb211_end();
}

/* A changed mode applies to the following writes: what was written so far
 * goes out in the old one. */
static void lfb_state_changed(void) {
    glide_lfb211_sync(0);
}

void GR_EXPORT GLIDE_API grLfbWriteMode(GrLfbWriteMode_t mode) {
    GLIDE_TRACE("grLfbWriteMode");
    if (mode != g_write_mode)
        lfb_state_changed();
    g_write_mode = mode;
}

void GR_EXPORT GLIDE_API grLfbBypassMode(GrLfbBypassMode_t mode) {
    GLIDE_TRACE("grLfbBypassMode");
    if (mode != g_bypass)
        lfb_state_changed();
    g_bypass = mode;
}

void GR_EXPORT GLIDE_API grLfbOrigin(GrOriginLocation_t origin) {
    GLIDE_TRACE("grLfbOrigin");
    if (origin != g_lfb_origin)
        lfb_state_changed();
    g_lfb_origin = origin;
}

void *GR_EXPORT GLIDE_API grLfbGetWritePtr(GrBuffer_t buffer) {
    GLIDE_TRACE("grLfbGetWritePtr");
    return glide_lfb211_write_ptr(buffer, g_write_mode, g_bypass == GR_LFBBYPASS_DISABLE, g_lfb_origin);
}

const FxU32 *GR_EXPORT GLIDE_API grLfbGetReadPtr(GrBuffer_t buffer) {
    GLIDE_TRACE("grLfbGetReadPtr");
    return (const FxU32 *) glide_lfb211_read_ptr(buffer, g_lfb_origin);
}
