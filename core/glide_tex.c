/*
 * Textures: memory queries, downloads (all formats, aspect ratios, mipmap
 * chains), grTexSource and texture modes. See core/glide_internal.h for the
 * principle (registers = g_voodoo.params).
 *
 * Memory layout = the hardware's, as voodoo_recalc_tex12()
 * (core/rasterizer/vid_voodoo_texture.c) derives it from texBaseAddr, tLOD
 * and tformat: starting at texBaseAddr, ALL levels from LOD 0 (256 texels)
 * follow each other, each densely packed (width x height x 1 or 2 bytes).
 * A texture whose largest level is smaller than 256 therefore does not
 * start at texBaseAddr; 3dfx's Glide (_grTexCalcBaseAddress) subtracts the
 * size of the missing larger levels from the start address in
 * grTexSource(). So does this code, and downloads write from the start
 * address the application gave, exactly where the rasterizer reads.
 */

#include "glide_internal.h"
#include "glidulator/trace.h"

#include "vid_voodoo_texture.h"

#include <math.h>
#include <string.h>

/* --- Sizes --------------------------------------------------------- */

static int tex_bytes_per_texel(GrTextureFormat_t fmt) {
    return fmt < GR_TEXFMT_16BIT ? 1 : 2;
}

/* Width/height of a level: lod 0 = 256 texels along the long edge, the
 * aspect ratio shortens the other one (min. 1 texel). For 8x1..2x1, S
 * (width) is the long edge. */
static void tex_level_dims(GrLOD_t lod, GrAspectRatio_t aspect, FxU32 *w, FxU32 *h) {
    FxU32 s = 256u >> lod;
    *w = *h = s;
    if (aspect < GR_ASPECT_1x1)
        *h = s >> (GR_ASPECT_1x1 - aspect);
    else if (aspect > GR_ASPECT_1x1)
        *w = s >> (aspect - GR_ASPECT_1x1);
    if (!*w)
        *w = 1;
    if (!*h)
        *h = 1;
}

static FxU32 tex_level_bytes(GrLOD_t lod, GrAspectRatio_t aspect, GrTextureFormat_t fmt) {
    FxU32 w, h;
    tex_level_dims(lod, aspect, &w, &h);
    return w * h * (FxU32) tex_bytes_per_texel(fmt);
}

/* Sum of levels from..to-1 (unrounded): offset of a level. */
FxU32 glide_tex_levels_bytes(GrLOD_t from, GrLOD_t to, GrAspectRatio_t aspect, GrTextureFormat_t fmt) {
    FxU32 total = 0;
    for (GrLOD_t lod = from; lod < to; lod++)
        total += tex_level_bytes(lod, aspect, fmt);
    return total;
}

static int tex_params_valid(GrLOD_t small_lod, GrLOD_t large_lod, GrAspectRatio_t aspect, GrTextureFormat_t fmt) {
    return large_lod >= GR_LOD_256 && small_lod <= GR_LOD_1 && large_lod <= small_lod &&
           aspect >= GR_ASPECT_8x1 && aspect <= GR_ASPECT_1x8 && fmt >= 0 && fmt <= GR_TEXFMT_RSVD2;
}

/* Sum over large_lod..small_lod (large_lod is the larger texture, i.e. the
 * SMALLER number); with an odd/even split only every second level (even LOD
 * number = EVEN). Rounded up to 8 bytes: Glide allocates texture memory in
 * 8-byte units. 0 for invalid parameters. */
FxU32 glide_tex_mem_required(GrLOD_t small_lod, GrLOD_t large_lod, GrAspectRatio_t aspect,
                             GrTextureFormat_t fmt, FxU32 even_odd) {
    if (!tex_params_valid(small_lod, large_lod, aspect, fmt))
        return 0;
    FxU32 total = 0;
    for (GrLOD_t lod = large_lod; lod <= small_lod; lod++) {
        FxU32 mask = (lod & 1) ? GR_MIPMAPLEVELMASK_ODD : GR_MIPMAPLEVELMASK_EVEN;
        if (even_odd & mask)
            total += tex_level_bytes(lod, aspect, fmt);
    }
    return (total + 7u) & ~7u;
}

FxU32 GR_EXPORT GLIDE_API grTexMinAddress(GrChipID_t tmu) {
    GLIDE_TRACE("grTexMinAddress");
    (void) tmu;
    return 0;
}

/* Highest valid start address: memory size - 8. Programs compute
 * "max - min" as the available texture memory. */
FxU32 GR_EXPORT GLIDE_API grTexMaxAddress(GrChipID_t tmu) {
    GLIDE_TRACE("grTexMaxAddress");
    (void) tmu;
    return TEX_MEM_BYTES - 8;
}

FxU32 GR_EXPORT GLIDE_API grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexTextureMemRequired");
    if (!info)
        return 0;
    return glide_tex_mem_required(info->smallLod, info->largeLod, info->aspectRatio, info->format, evenOdd);
}

FxU32 GR_EXPORT GLIDE_API grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax, GrAspectRatio_t aspect,
                                               GrTextureFormat_t fmt) {
    GLIDE_TRACE("grTexCalcMemRequired");
    /* Glide's names mislead: lodmin is the SMALLEST texture (largest LOD
     * value), lodmax the largest, like smallLod/largeLod in GrTexInfo. */
    return glide_tex_mem_required(lodmin, lodmax, aspect, fmt, GR_MIPMAPLEVELMASK_BOTH);
}

/* --- Downloads ----------------------------------------------------- */

/* Copy bytes into tex_mem[0] and invalidate the texture cache for every
 * 1 KB page touched, as 86Box's voodoo_tex_writel() does per hardware write
 * (texture_present + flush_texture_cache). Otherwise the cache would keep
 * showing the old texture after a reload to the same address. */
static void tex_mem_write(FxU32 addr, const void *src, FxU32 n) {
    if (!g_win_open || !src || !n)
        return;
#ifdef GLIDULATOR_TRACE
    uint64_t t0 = platform_time_us();
#endif
    const uint8_t *s = (const uint8_t *) src;
    for (FxU32 i = 0; i < n; i++)
        g_voodoo.tex_mem[0][(addr + i) & g_voodoo.texture_mask] = s[i];

    FxU32 first = (addr & g_voodoo.texture_mask) >> TEX_DIRTY_SHIFT;
    FxU32 pages = ((addr & ((1u << TEX_DIRTY_SHIFT) - 1)) + n + (1u << TEX_DIRTY_SHIFT) - 1) >> TEX_DIRTY_SHIFT;
    FxU32 npage = (g_voodoo.texture_mask + 1) >> TEX_DIRTY_SHIFT;
    for (FxU32 p = 0; p < pages; p++) {
        FxU32 page = (first + p) % npage;
        if (g_voodoo.texture_present[0][page])
            flush_texture_cache(&g_voodoo, page << TEX_DIRTY_SHIFT, 0);
    }
#ifdef GLIDULATOR_TRACE
    g_perf_tex_us += platform_time_us() - t0;
#endif
}

/* An odd/even split (texture spread over two TMUs) does not exist with one
 * TMU; only the requested levels are loaded, in the unsplit layout. That is
 * correct for programs that use the split only on multi-TMU boards. */
static int level_selected(GrLOD_t lod, FxU32 even_odd) {
    return (even_odd & ((lod & 1) ? GR_MIPMAPLEVELMASK_ODD : GR_MIPMAPLEVELMASK_EVEN)) != 0;
}

void GR_EXPORT GLIDE_API grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexDownloadMipMap");
    if (tmu != GR_TMU0 || !info || !info->data)
        return;
    if (!tex_params_valid(info->smallLod, info->largeLod, info->aspectRatio, info->format))
        return;
#ifdef GLIDULATOR_DEBUG
    {
        static int logged;
        if (logged++ < 200) {
            glide_trace_note("Download Start/Bytes", (unsigned long) startAddress,
                             (unsigned long) glide_tex_mem_required(info->smallLod, info->largeLod, info->aspectRatio,
                                                                    info->format, GR_MIPMAPLEVELMASK_BOTH));
            glide_trace_note("  Format/LOD large*16+small", (unsigned long) info->format,
                             (unsigned long) (info->largeLod * 16 + info->smallLod));
        }
    }
#endif
    /* info->data holds all levels large..small back to back. */
    const uint8_t *src  = (const uint8_t *) info->data;
    FxU32          addr = startAddress;
    for (GrLOD_t lod = info->largeLod; lod <= info->smallLod; lod++) {
        FxU32 n = tex_level_bytes(lod, info->aspectRatio, info->format);
        if (level_selected(lod, evenOdd))
            tex_mem_write(addr, src, n);
        addr += n;
        src += n;
    }
}

void GR_EXPORT GLIDE_API grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
                                                  GrAspectRatio_t aspectRatio, GrTextureFormat_t format, FxU32 evenOdd,
                                                  void *data) {
    GLIDE_TRACE("grTexDownloadMipMapLevel");
    if (tmu != GR_TMU0 || !tex_params_valid(thisLod, largeLod, aspectRatio, format) || !level_selected(thisLod, evenOdd))
        return;
    FxU32 offset = glide_tex_levels_bytes(largeLod, thisLod, aspectRatio, format);
    tex_mem_write(startAddress + offset, data, tex_level_bytes(thisLod, aspectRatio, format));
}

/* Rows start..end (inclusive) of one level; data points at row start (as
 * in 3dfx's Glide, which reads data sequentially). */
void GR_EXPORT GLIDE_API grTexDownloadMipMapLevelPartial(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                                                         GrLOD_t largeLod, GrAspectRatio_t aspectRatio,
                                                         GrTextureFormat_t format, FxU32 evenOdd, void *data, int start,
                                                         int end) {
    GLIDE_TRACE("grTexDownloadMipMapLevelPartial");
    if (tmu != GR_TMU0 || !tex_params_valid(thisLod, largeLod, aspectRatio, format) || !level_selected(thisLod, evenOdd))
        return;
    FxU32 w, h;
    tex_level_dims(thisLod, aspectRatio, &w, &h);
    if (start < 0 || end < start || (FxU32) end >= h)
        return;
    FxU32 row    = w * (FxU32) tex_bytes_per_texel(format);
    FxU32 offset = glide_tex_levels_bytes(largeLod, thisLod, aspectRatio, format) + (FxU32) start * row;
    tex_mem_write(startAddress + offset, data, (FxU32) (end - start + 1) * row);
}

/* Palette for GR_TEXFMT_P_8/AP_88: 256 entries 0x00RRGGBB. */
static void palette_write(const FxU32 *entries, int start, int end) {
    if (start < 0)
        start = 0;
    if (end > 255)
        end = 255;
    for (int i = start; i <= end; i++)
        g_voodoo.palette[0][i].u = entries[i] | 0xff000000u;
    g_voodoo.palette_dirty[0] = 1;
}

/* NCC table for the YIQ formats (GR_TEXFMT_YIQ_422/AYIQ_8422): the 12
 * packed register values (GuNccTable.packed_data: Y0..3, I0..3, Q0..3) go
 * into the TMU's nccTable registers; start..end are register indices 0..11
 * as in Glide. The lookup table is then recomputed, as the board (and
 * 86Box) does on a register write. */
static void ncc_write(GrTexTable_t type, const GuNccTable *ncc, int start, int end) {
    int t = (type == GR_TEXTABLE_NCC1) ? 1 : 0;
    if (start < 0)
        start = 0;
    if (end > 11)
        end = 11;
    for (int r = start; r <= end; r++) {
        FxU32 v = ncc->packed_data[r];
        if (r < 4)
            g_voodoo.nccTable[0][t].y[r] = v;
        else if (r < 8)
            g_voodoo.nccTable[0][t].i[r - 4] = v;
        else
            g_voodoo.nccTable[0][t].q[r - 8] = v;
    }
    voodoo_update_ncc(&g_voodoo, 0);
}

static void table_write(GrChipID_t tmu, GrTexTable_t type, void *data, int start, int end) {
    if (tmu != GR_TMU0 || !data)
        return;
    if (type == GR_TEXTABLE_PALETTE)
        palette_write(((const GuTexPalette *) data)->data, start, end);
    else if (type == GR_TEXTABLE_NCC0 || type == GR_TEXTABLE_NCC1)
        ncc_write(type, (const GuNccTable *) data, start, end);
}

void GR_EXPORT GLIDE_API grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type, void *data) {
    GLIDE_TRACE("grTexDownloadTable");
    table_write(tmu, type, data, 0, type == GR_TEXTABLE_PALETTE ? 255 : 11);
}

void GR_EXPORT GLIDE_API grTexDownloadTablePartial(GrChipID_t tmu, GrTexTable_t type, void *data, int start, int end) {
    GLIDE_TRACE("grTexDownloadTablePartial");
    table_write(tmu, type, data, start, end);
}

/* Which of the two NCC tables YIQ textures use (textureMode bit 5;
 * tex_recalc() leaves the bit alone). */
void GR_EXPORT GLIDE_API grTexNCCTable(GrChipID_t tmu, GrNCCTable_t table) {
    GLIDE_TRACE("grTexNCCTable");
    if (tmu != GR_TMU0)
        return;
    if (table == GR_NCCTABLE_NCC1)
        g_voodoo.params.textureMode[0] |= TEXTUREMODE_NCC_SEL;
    else
        g_voodoo.params.textureMode[0] &= ~(uint32_t) TEXTUREMODE_NCC_SEL;
}

/* --- Source and modes (texBaseAddr, tLOD, textureMode) -------------- */

#define TM_PERSP      (1u << 0)
#define TM_MINFILTER  (1u << 1)
#define TM_MAGFILTER  (1u << 2)
#define TM_LODDITHER  (1u << 4)
#define TM_FORMAT(f)  (((uint32_t) (f) & 0xfu) << 8)
#define TLOD_MIN(l)   (((uint32_t) (l) & 0xfu) << 2)  /* 4.2 fixed point */
#define TLOD_MAX(l)   (((uint32_t) (l) & 0xfu) << 8)  /* 4.2 from bit 6 */
#define TLOD_ASPECT(a) (((uint32_t) (a) & 3u) << 21)

/* Texture last selected with grTexSource: grTexMipMapMode() must be able
 * to recompute tLOD. */
static GrTexInfo g_src;
static int       g_src_valid;

static void tex_recalc(void) {
    if (!g_src_valid)
        return;
    uint32_t tlod = g_voodoo.params.tLOD[0] & (0x3fu << 12); /* keep the LOD bias */
    GrLOD_t  max  = (g_sw.mipmap_mode == GR_MIPMAP_DISABLE) ? g_src.largeLod : g_src.smallLod;
    tlod |= TLOD_MIN(g_src.largeLod) | TLOD_MAX(max);
    if (g_src.aspectRatio < GR_ASPECT_1x1)
        tlod |= LOD_S_IS_WIDER | TLOD_ASPECT(GR_ASPECT_1x1 - g_src.aspectRatio);
    else
        tlod |= TLOD_ASPECT(g_src.aspectRatio - GR_ASPECT_1x1);
    g_voodoo.params.tLOD[0] = tlod;

    uint32_t tm = g_voodoo.params.textureMode[0] & ~(TM_FORMAT(0xf) | TM_LODDITHER);
    tm |= TM_FORMAT(g_src.format) | TM_PERSP;
    /* As in 3dfx's Glide: LOD dithering only if the application allows it
     * with grHints(GR_HINT_ALLOW_MIPMAP_DITHER). */
    if (g_sw.mipmap_mode == GR_MIPMAP_NEAREST_DITHER && g_sw.allow_lod_dither)
        tm |= TM_LODDITHER;
    g_voodoo.params.textureMode[0] = tm;
    g_voodoo.params.tformat[0]     = g_src.format;

    voodoo_recalc_tex12(&g_voodoo, 0);
}

void glide_tex_get_source(GrTexInfo *info, int *valid) {
    *info  = g_src;
    *valid = g_src_valid;
}

void glide_tex_set_source(const GrTexInfo *info, int valid) {
    g_src       = *info;
    g_src_valid = valid;
}

void GR_EXPORT GLIDE_API grTexSource(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexSource");
    (void) evenOdd; /* no odd/even split with one TMU, see above */
    if (tmu != GR_TMU0 || !info || !tex_params_valid(info->smallLod, info->largeLod, info->aspectRatio, info->format))
        return;
#ifdef GLIDULATOR_DEBUG
    {
        static int logged;
        if (logged++ < 300)
            glide_trace_note("Source Start/LOD large*16+small", (unsigned long) startAddress,
                             (unsigned long) (info->largeLod * 16 + info->smallLod));
    }
#endif
    g_src       = *info;
    g_src_valid = 1;
    /* Base = where LOD 0 would be; may lie "before" address 0, which is
     * fine because the rasterizer masks every address with texture_mask. */
    g_voodoo.params.texBaseAddr[0] =
        startAddress - glide_tex_levels_bytes(GR_LOD_256, info->largeLod, info->aspectRatio, info->format);
    tex_recalc();
}

void GR_EXPORT GLIDE_API grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minfilter_mode,
                                         GrTextureFilterMode_t magfilter_mode) {
    GLIDE_TRACE("grTexFilterMode");
    if (tmu != GR_TMU0)
        return;
    uint32_t tm = g_voodoo.params.textureMode[0] & ~(TM_MINFILTER | TM_MAGFILTER);
    if (minfilter_mode == GR_TEXTUREFILTER_BILINEAR)
        tm |= TM_MINFILTER;
    if (magfilter_mode == GR_TEXTUREFILTER_BILINEAR)
        tm |= TM_MAGFILTER;
    g_voodoo.params.textureMode[0] = tm;
}

void GR_EXPORT GLIDE_API grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t s_clampmode,
                                        GrTextureClampMode_t t_clampmode) {
    GLIDE_TRACE("grTexClampMode");
    if (tmu != GR_TMU0)
        return;
    uint32_t tm = g_voodoo.params.textureMode[0] & ~(uint32_t) (TEXTUREMODE_TCLAMPS | TEXTUREMODE_TCLAMPT);
    if (s_clampmode == GR_TEXTURECLAMP_CLAMP)
        tm |= TEXTUREMODE_TCLAMPS;
    if (t_clampmode == GR_TEXTURECLAMP_CLAMP)
        tm |= TEXTUREMODE_TCLAMPT;
    g_voodoo.params.textureMode[0] = tm;
}

/* lodBlend (trilinear across two TMUs) does not exist with one TMU. */
void GR_EXPORT GLIDE_API grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool lodBlend) {
    GLIDE_TRACE("grTexMipMapMode");
    (void) lodBlend;
    if (tmu != GR_TMU0)
        return;
    g_sw.mipmap_mode = mode;
    tex_recalc();
}

/* LOD bias: signed 4.2 fixed point in tLOD bits 12..17, range
 * -8..+7.75. */
void GR_EXPORT GLIDE_API grTexLodBiasValue(GrChipID_t tmu, float bias) {
    GLIDE_TRACE("grTexLodBiasValue");
    if (tmu != GR_TMU0)
        return;
    if (bias < -8.0f)
        bias = -8.0f;
    if (bias > 7.75f)
        bias = 7.75f;
    int32_t fixed           = glide_round32((double) bias * 4.0);
    g_voodoo.params.tLOD[0] = (g_voodoo.params.tLOD[0] & ~(0x3fu << 12)) | (((uint32_t) fixed & 0x3fu) << 12);
}

/* Glide defaults: TMU combine LOCAL (pass the texel), bilinear, wrap, no
 * mipmaps, LOD bias 0, no texture selected. */
void glide_tex_reset(void) {
    g_src_valid                    = 0;
    g_sw.mipmap_mode               = GR_MIPMAP_DISABLE;
    g_voodoo.params.textureMode[0] = 0;
    g_voodoo.params.tLOD[0]        = 0;
    g_voodoo.params.texBaseAddr[0] = 0;
    grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_FUNCTION_LOCAL,
                 GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
    grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP);
    g_voodoo.bilinear_enabled = 1;
}
