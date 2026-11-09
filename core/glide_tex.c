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
#include "glidulator/capture.h"
#include "glidulator/trace.h"
#include "glidulator/platform.h"

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

/* --- Number of TMUs ----------------------------------------------- */

int g_num_tmus = 1;

int glide_tmus_read_cfg(void) {
    char cfg[16];
#ifdef GLIDULATOR_GLIDE3
    /* glide3x.dll: Glide 3 renderers are written for the Voodoo2, two TMUs
     * unless GLIDULATOR_TMUS=1 */
    g_num_tmus = 2;
    if (platform_get_config("GLIDULATOR_TMUS", cfg, sizeof cfg) && cfg[0] == '1' && !cfg[1])
        g_num_tmus = 1;
#else
    /* Windows: two TMUs by default (Unreal-engine games draw see-through
     * surfaces wrongly with one, and lightmapped games save half their
     * triangles); DOS: one. The second costs 3 MB there (2 MB texture
     * memory and the first piece of its texture pool, which grows by need
     * up to 8 MB); DOS games that use it are rare. GLIDULATOR_TMUS=1|2
     * overrides; a Rush (GLIDULATOR_BOARD=rush) always has one. */
#    ifdef _WIN32
    g_num_tmus = 2;
#    else
    g_num_tmus = 1;
#    endif
    if (platform_get_config("GLIDULATOR_TMUS", cfg, sizeof cfg) && (cfg[0] == '1' || cfg[0] == '2') && !cfg[1])
        g_num_tmus = cfg[0] - '0';
    if (platform_get_config("GLIDULATOR_BOARD", cfg, sizeof cfg) && (cfg[0] | 0x20) == 'r')
        g_num_tmus = 1;
#endif
    return g_num_tmus;
}

static int tmu_ok(GrChipID_t tmu) {
    return tmu >= GR_TMU0 && tmu < g_num_tmus;
}

/* Odd/even level split (GR_MIPMAPLEVELMASK_EVEN/ODD): only with two TMUs.
 * With one, the levels stay in the unsplit layout, see level_selected(). */
static int split_used(FxU32 even_odd) {
    return g_num_tmus > 1 && (even_odd == GR_MIPMAPLEVELMASK_EVEN || even_odd == GR_MIPMAPLEVELMASK_ODD);
}

/* Offset of level lod from the texture's start address. Split: only the
 * levels of the same parity lie before it (3dfx's _grMipMapOffset_Tsplit,
 * the hardware's layout with tLOD LOD_SPLIT, voodoo_recalc_tex12()). */
static FxU32 level_offset(GrLOD_t large_lod, GrLOD_t lod, GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                          FxU32 even_odd) {
    if (!split_used(even_odd))
        return glide_tex_levels_bytes(large_lod, lod, aspect, fmt);
    FxU32 total = 0;
    for (GrLOD_t l = large_lod + ((large_lod ^ lod) & 1); l < lod; l += 2)
        total += tex_level_bytes(l, aspect, fmt);
    return total;
}

/* Base address = where LOD 0 would be (3dfx's _grTexCalcBaseAddress). */
static FxU32 base_address(FxU32 start, GrLOD_t large_lod, GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                          FxU32 even_odd) {
    if (!split_used(even_odd))
        return start - glide_tex_levels_bytes(GR_LOD_256, large_lod, aspect, fmt);
    /* first level of the selected parity */
    if ((even_odd == GR_MIPMAPLEVELMASK_EVEN) == ((large_lod & 1) != 0))
        large_lod++;
    FxU32 total = 0;
    for (GrLOD_t l = large_lod & 1; l < large_lod; l += 2)
        total += tex_level_bytes(l, aspect, fmt);
    return start - total;
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

/* Copy bytes into tex_mem[tmu] and invalidate the texture cache for every
 * 1 KB page touched, as 86Box's voodoo_tex_writel() does per hardware write
 * (texture_present + flush_texture_cache). Otherwise the cache would keep
 * showing the old texture after a reload to the same address. */
static void tex_mem_write(int tmu, FxU32 addr, const void *src, FxU32 n) {
    if (!g_win_open || !src || !n)
        return;
#ifdef GLIDULATOR_TRACE
    uint64_t t0 = glide_trace_cycles();
#endif
    /* at most two pieces: the texture memory wraps at texture_mask */
    const uint8_t *s    = (const uint8_t *) src;
    FxU32          size = g_voodoo.texture_mask + 1;
    FxU32          left = n > size ? size : n;
    FxU32          pos  = addr & g_voodoo.texture_mask;
    if (n > size) { /* only the last bytes survive, as with byte-wise writes */
        s += n - size;
        pos = (addr + n - size) & g_voodoo.texture_mask;
    }
    while (left) {
        FxU32 k = size - pos < left ? size - pos : left;
        memcpy(&g_voodoo.tex_mem[tmu][pos], s, k);
        s += k;
        left -= k;
        pos = 0;
    }
#ifdef GLIDULATOR_TRACE
    uint64_t t1 = glide_trace_cycles();
    g_perf_tex_copy_cyc += t1 - t0;
    g_perf_tex_bytes += n;
    g_perf_tex_calls++;
#endif
    flush_texture_range(&g_voodoo, addr, n > size ? size : n, tmu);
#ifdef GLIDULATOR_TRACE
    g_perf_tex_flush_cyc += glide_trace_cycles() - t1;
#endif
}

/* An odd/even split (texture spread over two TMUs) does not exist with one
 * TMU; only the requested levels are loaded, in the unsplit layout. That is
 * correct for programs that use the split only on multi-TMU boards. With
 * two TMUs the levels are packed as on the board (level_offset()). */
static int level_selected(GrLOD_t lod, FxU32 even_odd) {
    return (even_odd & ((lod & 1) ? GR_MIPMAPLEVELMASK_ODD : GR_MIPMAPLEVELMASK_EVEN)) != 0;
}

void GR_EXPORT GLIDE_API grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexDownloadMipMap");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_DOWNLOAD_MIPMAP, tmu, startAddress, evenOdd, info ? (uint32_t) info->smallLod : 0, info ? (uint32_t) info->largeLod : 0, info ? (uint32_t) info->aspectRatio : 0, info ? (uint32_t) info->format : 0);
    if (!tmu_ok(tmu) || !info || !info->data)
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
    const uint8_t *src = (const uint8_t *) info->data;
    for (GrLOD_t lod = info->largeLod; lod <= info->smallLod; lod++) {
        FxU32 n = tex_level_bytes(lod, info->aspectRatio, info->format);
        if (level_selected(lod, evenOdd))
            tex_mem_write(tmu, startAddress + level_offset(info->largeLod, lod, info->aspectRatio, info->format, evenOdd),
                          src, n);
        src += n;
    }
}

void GR_EXPORT GLIDE_API grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
                                                  GrAspectRatio_t aspectRatio, GrTextureFormat_t format, FxU32 evenOdd,
                                                  void *data) {
    GLIDE_TRACE("grTexDownloadMipMapLevel");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_DOWNLOAD_LEVEL, tmu, startAddress, thisLod, largeLod, aspectRatio, format, evenOdd);
    if (!tmu_ok(tmu) || !tex_params_valid(thisLod, largeLod, aspectRatio, format) || !level_selected(thisLod, evenOdd))
        return;
    FxU32 offset = level_offset(largeLod, thisLod, aspectRatio, format, evenOdd);
    tex_mem_write(tmu, startAddress + offset, data, tex_level_bytes(thisLod, aspectRatio, format));
}

/* Rows start..end (inclusive) of one level; data points at row start (as
 * in 3dfx's Glide, which reads data sequentially). */
void GR_EXPORT GLIDE_API grTexDownloadMipMapLevelPartial(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                                                         GrLOD_t largeLod, GrAspectRatio_t aspectRatio,
                                                         GrTextureFormat_t format, FxU32 evenOdd, void *data, int start,
                                                         int end) {
    GLIDE_TRACE("grTexDownloadMipMapLevelPartial");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_DOWNLOAD_PARTIAL, tmu, startAddress, thisLod, largeLod, aspectRatio, format, evenOdd, (uint32_t) start | (uint32_t) end << 16);
    if (!tmu_ok(tmu) || !tex_params_valid(thisLod, largeLod, aspectRatio, format) || !level_selected(thisLod, evenOdd))
        return;
    FxU32 w, h;
    tex_level_dims(thisLod, aspectRatio, &w, &h);
    if (start < 0 || end < start || (FxU32) end >= h)
        return;
    FxU32 row    = w * (FxU32) tex_bytes_per_texel(format);
    FxU32 offset = level_offset(largeLod, thisLod, aspectRatio, format, evenOdd) + (FxU32) start * row;
    tex_mem_write(tmu, startAddress + offset, data, (FxU32) (end - start + 1) * row);
}

/* Palette for GR_TEXFMT_P_8/AP_88: 256 entries 0x00RRGGBB. Written to
 * every TMU, whichever one the call names: 3dfx's Glide broadcasts palette
 * downloads (the tmu argument only selects the NCC tables). Unreal
 * Tournament relies on it with two TMUs: it loads a palette through TMU1
 * and then draws a P8 texture of the same palette on TMU0 without loading
 * it again (otherwise: white sky, speckled facades in the city intro). */
static void palette_write(const FxU32 *entries, int start, int end) {
    if (start < 0)
        start = 0;
    if (end > 255)
        end = 255;
    for (int t = 0; t < g_num_tmus; t++) {
        for (int i = start; i <= end; i++)
            g_voodoo.palette[t][i].u = entries[i] | 0xff000000u;
        g_voodoo.palette_dirty[t] = 1;
    }
}

/* NCC table for the YIQ formats (GR_TEXFMT_YIQ_422/AYIQ_8422): the 12
 * packed register values (GuNccTable.packed_data: Y0..3, I0..3, Q0..3) go
 * into the TMU's nccTable registers; start..end are register indices 0..11
 * as in Glide. The lookup table is then recomputed, as the board (and
 * 86Box) does on a register write. */
static void ncc_write(int tmu, GrTexTable_t type, const GuNccTable *ncc, int start, int end) {
    int t = (type == GR_TEXTABLE_NCC1) ? 1 : 0;
    if (start < 0)
        start = 0;
    if (end > 11)
        end = 11;
    for (int r = start; r <= end; r++) {
        FxU32 v = ncc->packed_data[r];
        if (r < 4)
            g_voodoo.nccTable[tmu][t].y[r] = v;
        else if (r < 8)
            g_voodoo.nccTable[tmu][t].i[r - 4] = v;
        else
            g_voodoo.nccTable[tmu][t].q[r - 8] = v;
    }
    voodoo_update_ncc(&g_voodoo, tmu);
}

static void table_write(GrChipID_t tmu, GrTexTable_t type, void *data, int start, int end) {
    if (!tmu_ok(tmu) || !data)
        return;
#ifdef GLIDULATOR_TRACE
    g_perf_tex_tables++;
#endif
    if (type == GR_TEXTABLE_PALETTE) {
        palette_write(((const GuTexPalette *) data)->data, start, end);
#ifdef GLIDULATOR_DEBUG
        glide_capture_palette(tmu);
#endif
    }
    else if (type == GR_TEXTABLE_NCC0 || type == GR_TEXTABLE_NCC1)
        ncc_write(tmu, type, (const GuNccTable *) data, start, end);
}

void GR_EXPORT GLIDE_API grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type, void *data) {
    GLIDE_TRACE("grTexDownloadTable");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_DOWNLOAD_TABLE, tmu, type, 0, type == GR_TEXTABLE_PALETTE ? 255u : 11u);
    table_write(tmu, type, data, 0, type == GR_TEXTABLE_PALETTE ? 255 : 11);
}

void GR_EXPORT GLIDE_API grTexDownloadTablePartial(GrChipID_t tmu, GrTexTable_t type, void *data, int start, int end) {
    GLIDE_TRACE("grTexDownloadTablePartial");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_DOWNLOAD_TABLE, tmu, type, (uint32_t) start, (uint32_t) end);
    table_write(tmu, type, data, start, end);
}

/* Which of the two NCC tables YIQ textures use (textureMode bit 5;
 * tex_recalc() leaves the bit alone). */
void GR_EXPORT GLIDE_API grTexNCCTable(GrChipID_t tmu, GrNCCTable_t table) {
    GLIDE_TRACE("grTexNCCTable");
    if (!tmu_ok(tmu))
        return;
    if (table == GR_NCCTABLE_NCC1)
        g_voodoo.params.textureMode[tmu] |= TEXTUREMODE_NCC_SEL;
    else
        g_voodoo.params.textureMode[tmu] &= ~(uint32_t) TEXTUREMODE_NCC_SEL;
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

/* Texture last selected with grTexSource per TMU: grTexMipMapMode() must
 * be able to recompute tLOD. */
static GrTexInfo g_src[GLIDE_NUM_TMU];
static int       g_src_valid[GLIDE_NUM_TMU];
static FxU32     g_src_even_odd[GLIDE_NUM_TMU];

/* GLIDULATOR_LOD_DITHER: -1 = auto (as the game asks), 0 = never, 1 = for
 * every mipmapped texture (like the "mipmap dithering" driver option). */
static int g_lod_dither_cfg = -1;

void glide_tex_read_cfg(void) {
    char cfg[8];
    g_lod_dither_cfg = -1;
    if (platform_get_config("GLIDULATOR_LOD_DITHER", cfg, sizeof cfg) && (cfg[0] == '0' || cfg[0] == '1') && !cfg[1])
        g_lod_dither_cfg = cfg[0] - '0';
}

/* tLOD odd/split bits of a TMU, as 3dfx's Glide sets them in grTexSource,
 * grTexMipMapMode and grTexCombine (two TMUs only): the level mask gives
 * LOD_SPLIT (+ LOD_ODD for the odd levels); with lodBlend, a combine of
 * "zero other, blend by LOD fraction, not reversed" (the odd pass of
 * two-pass trilinear) sets LOD_ODD too. Glide tests the mselect field
 * with a plain AND (SST_TC_BLEND_LODFRAC), so does this. */
#define TM_TC_ZERO_OTHER   (1u << 12)
#define TM_TC_BLEND_LODFRC (5u << 14)
#define TM_TC_REVERSE      (1u << 17)

static uint32_t odd_split_bits(int tmu) {
    uint32_t bits = 0, tm = g_voodoo.params.textureMode[tmu];
    if (g_num_tmus < 2)
        return 0;
    if (g_src_even_odd[tmu] == GR_MIPMAPLEVELMASK_EVEN)
        bits |= LOD_SPLIT;
    else if (g_src_even_odd[tmu] == GR_MIPMAPLEVELMASK_ODD)
        bits |= LOD_SPLIT | LOD_ODD;
    if ((tm & TEXTUREMODE_TRILINEAR) && (tm & TM_TC_ZERO_OTHER) && (tm & TM_TC_BLEND_LODFRC) && !(tm & TM_TC_REVERSE))
        bits |= LOD_ODD;
    return bits;
}

static void tex_recalc(int tmu) {
    if (!g_src_valid[tmu])
        return;
    const GrTexInfo *src  = &g_src[tmu];
    /* keep the LOD bias and grTexMultibase */
    uint32_t         tlod = g_voodoo.params.tLOD[tmu] & ((0x3fu << 12) | LOD_TMULTIBASEADDR);
    GrLOD_t          max  = (g_sw.mipmap_mode[tmu] == GR_MIPMAP_DISABLE) ? src->largeLod : src->smallLod;
    tlod |= TLOD_MIN(src->largeLod) | TLOD_MAX(max);
    if (src->aspectRatio < GR_ASPECT_1x1)
        tlod |= LOD_S_IS_WIDER | TLOD_ASPECT(GR_ASPECT_1x1 - src->aspectRatio);
    else
        tlod |= TLOD_ASPECT(src->aspectRatio - GR_ASPECT_1x1);

    uint32_t tm = g_voodoo.params.textureMode[tmu] & ~(TM_FORMAT(0xf) | TM_LODDITHER | TEXTUREMODE_TRILINEAR);
    tm |= TM_FORMAT(src->format) | TM_PERSP;
    /* As in 3dfx's Glide: LOD dithering only if the application allows it
     * with grHints(GR_HINT_ALLOW_MIPMAP_DITHER); GLIDULATOR_LOD_DITHER can
     * force it on or off. */
    GrMipMapMode_t mm = g_sw.mipmap_mode[tmu];
    if (g_lod_dither_cfg == 1 ? mm != GR_MIPMAP_DISABLE
                              : g_lod_dither_cfg < 0 && mm == GR_MIPMAP_NEAREST_DITHER && g_sw.allow_lod_dither)
        tm |= TM_LODDITHER;
    /* lodBlend: trilinear between this TMU's two nearest levels (two TMUs
     * only; with one, Glide's two-pass trilinear is not emulated) */
    if (g_num_tmus > 1 && g_sw.lod_blend[tmu])
        tm |= TEXTUREMODE_TRILINEAR;
    g_voodoo.params.textureMode[tmu] = tm;
    g_voodoo.params.tformat[tmu]     = src->format;
    g_voodoo.params.tLOD[tmu]        = tlod | odd_split_bits(tmu);

    voodoo_recalc_tex12(&g_voodoo, tmu);
}

void glide_tex_combine_changed(int tmu) {
    if (g_num_tmus < 2 || tmu < 0 || tmu >= g_num_tmus)
        return;
    uint32_t tlod = (g_voodoo.params.tLOD[tmu] & ~(uint32_t) (LOD_SPLIT | LOD_ODD)) | odd_split_bits(tmu);
    if (tlod != g_voodoo.params.tLOD[tmu]) {
        g_voodoo.params.tLOD[tmu] = tlod;
        if (g_src_valid[tmu])
            voodoo_recalc_tex12(&g_voodoo, tmu);
    }
}

void glide_tex_get_source(int tmu, GrTexInfo *info, int *valid, FxU32 *even_odd) {
    *info     = g_src[tmu];
    *valid    = g_src_valid[tmu];
    *even_odd = g_src_even_odd[tmu];
}

void glide_tex_set_source(int tmu, const GrTexInfo *info, int valid, FxU32 even_odd) {
    g_src[tmu]          = *info;
    g_src_valid[tmu]    = valid;
    g_src_even_odd[tmu] = even_odd;
}

void GR_EXPORT GLIDE_API grTexSource(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexSource");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_SOURCE, tmu, startAddress, evenOdd, info ? (uint32_t) info->smallLod : 0, info ? (uint32_t) info->largeLod : 0, info ? (uint32_t) info->aspectRatio : 0, info ? (uint32_t) info->format : 0);
    if (!tmu_ok(tmu) || !info || !tex_params_valid(info->smallLod, info->largeLod, info->aspectRatio, info->format))
        return;
#ifdef GLIDULATOR_DEBUG
    {
        static int logged;
        if (logged++ < 300)
            glide_trace_note("Source Start/LOD large*16+small", (unsigned long) startAddress,
                             (unsigned long) (info->largeLod * 16 + info->smallLod));
    }
#endif
    g_src[tmu]          = *info;
    g_src_valid[tmu]    = 1;
    g_src_even_odd[tmu] = evenOdd;
    /* Base = where LOD 0 would be; may lie "before" address 0, which is
     * fine because the rasterizer masks every address with texture_mask. */
    g_voodoo.params.texBaseAddr[tmu] =
        base_address(startAddress, info->largeLod, info->aspectRatio, info->format, evenOdd);
    tex_recalc(tmu);
}

void GR_EXPORT GLIDE_API grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minfilter_mode,
                                         GrTextureFilterMode_t magfilter_mode) {
    GLIDE_TRACE("grTexFilterMode");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_FILTER, tmu, minfilter_mode, magfilter_mode);
    if (!tmu_ok(tmu))
        return;
    uint32_t tm = g_voodoo.params.textureMode[tmu] & ~(TM_MINFILTER | TM_MAGFILTER);
    if (minfilter_mode == GR_TEXTUREFILTER_BILINEAR)
        tm |= TM_MINFILTER;
    if (magfilter_mode == GR_TEXTUREFILTER_BILINEAR)
        tm |= TM_MAGFILTER;
    g_voodoo.params.textureMode[tmu] = tm;
}

void GR_EXPORT GLIDE_API grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t s_clampmode,
                                        GrTextureClampMode_t t_clampmode) {
    GLIDE_TRACE("grTexClampMode");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_CLAMP, tmu, s_clampmode, t_clampmode);
    if (!tmu_ok(tmu))
        return;
    uint32_t tm = g_voodoo.params.textureMode[tmu] & ~(uint32_t) (TEXTUREMODE_TCLAMPS | TEXTUREMODE_TCLAMPT);
    if (s_clampmode == GR_TEXTURECLAMP_CLAMP)
        tm |= TEXTUREMODE_TCLAMPS;
    if (t_clampmode == GR_TEXTURECLAMP_CLAMP)
        tm |= TEXTUREMODE_TCLAMPT;
    g_voodoo.params.textureMode[tmu] = tm;
}

/* lodBlend (trilinear) only with two TMUs, see tex_recalc(). */
void GR_EXPORT GLIDE_API grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool lodBlend) {
    GLIDE_TRACE("grTexMipMapMode");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_MIPMAP_MODE, tmu, mode, lodBlend);
    if (!tmu_ok(tmu))
        return;
    g_sw.mipmap_mode[tmu] = mode;
    g_sw.lod_blend[tmu]   = lodBlend;
    tex_recalc(tmu);
}

/* LOD bias: signed 4.2 fixed point in tLOD bits 12..17, range
 * -8..+7.75. */
void GR_EXPORT GLIDE_API grTexLodBiasValue(GrChipID_t tmu, float bias) {
    GLIDE_TRACE("grTexLodBiasValue");
    if (!tmu_ok(tmu))
        return;
    if (bias < -8.0f)
        bias = -8.0f;
    if (bias > 7.75f)
        bias = 7.75f;
    int32_t fixed             = glide_round32((double) bias * 4.0);
    g_voodoo.params.tLOD[tmu] = (g_voodoo.params.tLOD[tmu] & ~(0x3fu << 12)) | (((uint32_t) fixed & 0x3fu) << 12);
}

/* Glide defaults: TMU combine LOCAL (pass the texel), bilinear, wrap, no
 * mipmaps, LOD bias 0, no texture selected. */
void glide_tex_reset(void) {
    for (int t = 0; t < GLIDE_NUM_TMU; t++) {
        g_src_valid[t]                 = 0;
        g_src_even_odd[t]              = GR_MIPMAPLEVELMASK_BOTH;
        g_sw.mipmap_mode[t]            = GR_MIPMAP_DISABLE;
        g_sw.lod_blend[t]              = FXFALSE;
        g_voodoo.params.textureMode[t] = 0;
        g_voodoo.params.tLOD[t]        = 0;
        g_voodoo.params.texBaseAddr[t] = 0;
        if (t < 2) {
            g_voodoo.params.texBaseAddr1[t]  = 0;
            g_voodoo.params.texBaseAddr2[t]  = 0;
            g_voodoo.params.texBaseAddr38[t] = 0;
        }
    }
    for (int t = 0; t < g_num_tmus; t++) {
        grTexCombine(t, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_FUNCTION_LOCAL,
                     GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
        grTexFilterMode(t, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
        grTexClampMode(t, GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP);
    }
    g_voodoo.bilinear_enabled = 1;
}

/* --- Multibase: separate base addresses for LOD 256, 128, 64 and 32..1 --

   As in 3dfx's Glide: grTexMultibase sets tLOD bit 24, and
   grTexMultibaseAddress sets texBaseAddr / _1 / _2 / _3_8 to the base at
   which the range's first level lies at startAddress (the same
   computation as grTexSource, with the range's level as the large LOD).
   The rasterizer (voodoo_recalc_tex12) then switches the base per level
   and keeps adding the full-chain offset, like the board. */

void GR_EXPORT GLIDE_API grTexMultibase(GrChipID_t tmu, FxBool enable) {
    GLIDE_TRACE("grTexMultibase");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_MULTIBASE, tmu, enable);
    if (!tmu_ok(tmu))
        return;
    if (enable)
        g_voodoo.params.tLOD[tmu] |= LOD_TMULTIBASEADDR;
    else
        g_voodoo.params.tLOD[tmu] &= ~(uint32_t) LOD_TMULTIBASEADDR;
    if (g_src_valid[tmu])
        voodoo_recalc_tex12(&g_voodoo, tmu);
}

void GR_EXPORT GLIDE_API grTexMultibaseAddress(GrChipID_t tmu, GrTexBaseRange_t range, FxU32 startAddress,
                                               FxU32 evenOdd, GrTexInfo *info) {
    GLIDE_TRACE("grTexMultibaseAddress");
    GLIDE_CAPTURE_CALL(GLIDE_CALL_TEX_MULTIBASE_ADDR, tmu, range, startAddress, evenOdd);
    if (!tmu_ok(tmu) || !info || !tex_params_valid(info->smallLod, info->largeLod, info->aspectRatio, info->format))
        return;
    switch (range) {
    case GR_TEXBASE_256:
        g_voodoo.params.texBaseAddr[tmu] = base_address(startAddress, GR_LOD_256, info->aspectRatio, info->format, evenOdd);
        break;
    case GR_TEXBASE_128:
        g_voodoo.params.texBaseAddr1[tmu] = base_address(startAddress, GR_LOD_128, info->aspectRatio, info->format, evenOdd);
        break;
    case GR_TEXBASE_64:
        g_voodoo.params.texBaseAddr2[tmu] = base_address(startAddress, GR_LOD_64, info->aspectRatio, info->format, evenOdd);
        break;
    case GR_TEXBASE_32_TO_1:
        g_voodoo.params.texBaseAddr38[tmu] = base_address(startAddress, GR_LOD_32, info->aspectRatio, info->format, evenOdd);
        break;
    default: return;
    }
    if (g_src_valid[tmu])
        voodoo_recalc_tex12(&g_voodoo, tmu);
}

/* Detail texture factor per TMU (tDetail), as 3dfx's Glide: detail_max x 255
 * (8.0), lod_bias as signed 6.0, detail_scale 3.0. The rasterizer blends
 * with clamp(0, max, (bias - LOD) << scale) for GR_COMBINE_FACTOR_DETAIL. */
void GR_EXPORT GLIDE_API grTexDetailControl(GrChipID_t tmu, int lod_bias, FxU8 detail_scale, float detail_max) {
    GLIDE_TRACE("grTexDetailControl");
    if (!tmu_ok(tmu) || tmu > 1)
        return;
    if (detail_max < 0.0f)
        detail_max = 0.0f;
    else if (detail_max > 1.0f)
        detail_max = 1.0f;
    int bias = lod_bias & 0x3f;
    if (bias & 0x20)
        bias -= 0x40; /* signed 6 bit, like the register */
    g_voodoo.params.detail_max[tmu]   = (int) (detail_max * 255.0f);
    g_voodoo.params.detail_bias[tmu]  = bias;
    g_voodoo.params.detail_scale[tmu] = detail_scale & 7;
}
