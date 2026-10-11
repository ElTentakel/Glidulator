/*
 * Glide utility texture management (guTex*) and .3DF files (gu3df*).
 *
 * Many Glide 2 games do not manage texture memory themselves with
 * grTexMinAddress/grTexDownloadMipMap/grTexSource but through this simple
 * manager from 3dfx's Glide: guTexAllocateMemory() allocates memory like a
 * stack from the bottom (freeing is only possible all at once with
 * guTexMemReset) and returns an id (GrMipMapId_t) describing a GrMipMapInfo
 * with all texture attributes; guTexSource() selects it together with its
 * filter/clamp/mipmap mode. Everything here is built on the gr functions
 * (core/glide_tex.c), as in 3dfx's Glide.
 *
 * .3DF is the texture format of 3dfx's texus: four text header lines
 * ("3df v1.0", format name, "lod range: <small> <large>",
 * "aspect ratio: <w> <h>"), an NCC table for YIQ formats, a palette for
 * P8/AP88, then the mip levels from large to small; all multi-byte values
 * are big-endian.
 */

#include "glide_internal.h"
#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include <string.h>

/* As in 3dfx's Glide 2.x (MAX_MIPMAPS_PER_SST). */
#define GU_MAX_MIPMAPS 1024

static GrMipMapInfo g_mm[GU_MAX_MIPMAPS];
static int          g_mm_next;      /* next free id */
static FxU32        g_mm_free_base; /* next free texture address */
static GrMipMapId_t g_mm_current = GR_NULL_MIPMAP_HANDLE;

/* YIQ textures: which id currently occupies NCC table 0/1 and which table
 * is overwritten next. As in 3dfx's code the two tables alternate, so two
 * YIQ textures used in turn need no new table download. */
static GrMipMapId_t g_ncc_mmid[2];
static int          g_ncc_next;

static GrMipMapInfo *mm_get(GrMipMapId_t id) {
    if (id >= (GrMipMapId_t) g_mm_next || !g_mm[id].valid)
        return NULL;
    return &g_mm[id];
}

/* As in 3dfx's code, tmu_base_address is where LOD 0 (256 texels) would
 * be; the largest actual level starts after it. The gr functions take the
 * start address of the largest level instead. */
static FxU32 mm_start_address(const GrMipMapInfo *mm) {
    return mm->tmu_base_address + glide_tex_levels_bytes(GR_LOD_256, mm->lod_max, mm->aspect_ratio, mm->format);
}

void glide_gutex_reset(void) {
    memset(g_mm, 0, sizeof g_mm);
    g_mm_next      = 0;
    g_ncc_mmid[0] = g_ncc_mmid[1] = GR_NULL_MIPMAP_HANDLE;
    g_ncc_next     = 0;
    g_mm_free_base = 0;
    g_mm_current   = GR_NULL_MIPMAP_HANDLE;
}

void GR_EXPORT GLIDE_API guTexMemReset(void) {
    GLIDE_TRACE("guTexMemReset");
    glide_gutex_reset();
}

FxU32 GR_EXPORT GLIDE_API guTexMemQueryAvail(GrChipID_t tmu) {
    GLIDE_TRACE("guTexMemQueryAvail");
    if (tmu != GR_TMU0)
        return 0;
    return TEX_MEM_BYTES - g_mm_free_base;
}

/* LOD bias as in GrMipMapInfo.lod_bias: 4.2 fixed point (bias * 4) as a
 * signed value in an FxU32. */
static FxU32 lod_bias_to_fixed(float bias) {
    return (FxU32) glide_round32((double) bias * 4.0);
}

static float lod_bias_from_fixed(FxU32 fixed) {
    return (float) (FxI32) fixed / 4.0f;
}

GrMipMapId_t GR_EXPORT GLIDE_API guTexAllocateMemory(GrChipID_t tmu, FxU8 odd_even_mask, int width, int height,
                                                     GrTextureFormat_t fmt, GrMipMapMode_t mm_mode,
                                                     GrLOD_t smallest_lod, GrLOD_t largest_lod, GrAspectRatio_t aspect,
                                                     GrTextureClampMode_t s_clamp_mode,
                                                     GrTextureClampMode_t t_clamp_mode,
                                                     GrTextureFilterMode_t minfilter_mode,
                                                     GrTextureFilterMode_t magfilter_mode, float lod_bias,
                                                     FxBool trilinear) {
    GLIDE_TRACE("guTexAllocateMemory");
    if (tmu != GR_TMU0 || g_mm_next >= GU_MAX_MIPMAPS)
        return GR_NULL_MIPMAP_HANDLE;
    FxU32 bytes = glide_tex_mem_required(smallest_lod, largest_lod, aspect, fmt, odd_even_mask);
    if (!bytes || g_mm_free_base + bytes > TEX_MEM_BYTES)
        return GR_NULL_MIPMAP_HANDLE; /* invalid, or texture memory full */

    GrMipMapId_t  id = (GrMipMapId_t) g_mm_next++;
    GrMipMapInfo *mm = &g_mm[id];
    memset(mm, 0, sizeof *mm);
    mm->sst              = 0;
    mm->valid            = FXTRUE;
    mm->width            = width;
    mm->height           = height;
    mm->aspect_ratio     = aspect;
    mm->format           = fmt;
    mm->mipmap_mode      = mm_mode;
    mm->magfilter_mode   = magfilter_mode;
    mm->minfilter_mode   = minfilter_mode;
    mm->s_clamp_mode     = s_clamp_mode;
    mm->t_clamp_mode     = t_clamp_mode;
    mm->lod_bias         = lod_bias_to_fixed(lod_bias);
    mm->lod_min          = smallest_lod; /* Glide naming: lod_min = smallest level */
    mm->lod_max          = largest_lod;
    mm->tmu              = tmu;
    mm->odd_even_mask    = odd_even_mask;
    mm->trilinear        = trilinear;
    mm->tmu_base_address = g_mm_free_base - glide_tex_levels_bytes(GR_LOD_256, largest_lod, aspect, fmt);
    /* The register values tLOD/tTextureMode are only set by guTexSource
     * through grTexSource & co.; they stay 0 here. */
    g_mm_free_base += bytes;
    return id;
}

/* -1 means "unchanged" for every parameter. As in 3dfx's code there is no
 * memory check (a larger texture overwrites the next one) and no effect on
 * a currently selected texture until the next guTexSource. */
FxBool GR_EXPORT GLIDE_API guTexChangeAttributes(GrMipMapId_t mmid, int width, int height, GrTextureFormat_t fmt,
                                                 GrMipMapMode_t mm_mode, GrLOD_t smallest_lod, GrLOD_t largest_lod,
                                                 GrAspectRatio_t aspect, GrTextureClampMode_t s_clamp_mode,
                                                 GrTextureClampMode_t t_clamp_mode,
                                                 GrTextureFilterMode_t minFilterMode,
                                                 GrTextureFilterMode_t magFilterMode) {
    GLIDE_TRACE("guTexChangeAttributes");
    GrMipMapInfo *mm = mm_get(mmid);
    if (!mm)
        return FXFALSE;
    GrMipMapInfo n = *mm;
    if (width != -1)
        n.width = width;
    if (height != -1)
        n.height = height;
    if (fmt != -1)
        n.format = fmt;
    if (mm_mode != -1)
        n.mipmap_mode = mm_mode;
    if (smallest_lod != -1)
        n.lod_min = smallest_lod;
    if (largest_lod != -1)
        n.lod_max = largest_lod;
    if (aspect != -1)
        n.aspect_ratio = aspect;
    if (s_clamp_mode != -1)
        n.s_clamp_mode = s_clamp_mode;
    if (t_clamp_mode != -1)
        n.t_clamp_mode = t_clamp_mode;
    if (minFilterMode != -1)
        n.minfilter_mode = minFilterMode;
    if (magFilterMode != -1)
        n.magfilter_mode = magFilterMode;
    *mm = n;
    return FXTRUE;
}

GrMipMapInfo *GR_EXPORT GLIDE_API guTexGetMipMapInfo(GrMipMapId_t mmid) {
    GLIDE_TRACE("guTexGetMipMapInfo");
    return mm_get(mmid);
}

GrMipMapId_t GR_EXPORT GLIDE_API guTexGetCurrentMipMap(GrChipID_t tmu) {
    GLIDE_TRACE("guTexGetCurrentMipMap");
    return tmu == GR_TMU0 ? g_mm_current : GR_NULL_MIPMAP_HANDLE;
}

static void mm_texinfo(const GrMipMapInfo *mm, GrTexInfo *info, void *data) {
    info->smallLod    = mm->lod_min;
    info->largeLod    = mm->lod_max;
    info->aspectRatio = mm->aspect_ratio;
    info->format      = mm->format;
    info->data        = data;
}

void GR_EXPORT GLIDE_API guTexSource(GrMipMapId_t id) {
    GLIDE_TRACE("guTexSource");
    GrMipMapInfo *mm = mm_get(id);
    if (!mm)
        return;
    GrTexInfo info;
    mm_texinfo(mm, &info, mm->data);
    grTexSource(mm->tmu, mm_start_address(mm), mm->odd_even_mask, &info);
    grTexFilterMode(mm->tmu, mm->minfilter_mode, mm->magfilter_mode);
    grTexClampMode(mm->tmu, mm->s_clamp_mode, mm->t_clamp_mode);
    grTexMipMapMode(mm->tmu, mm->mipmap_mode, mm->trilinear);
    grTexLodBiasValue(mm->tmu, lod_bias_from_fixed(mm->lod_bias));
    /* YIQ: select the texture's NCC table. If it is already in one of the
     * two tables, just switch; otherwise overwrite the one loaded longer
     * ago. */
    if (mm->format == GR_TEXFMT_YIQ_422 || mm->format == GR_TEXFMT_AYIQ_8422) {
        int t;
        if (g_ncc_mmid[0] == id)
            t = 0;
        else if (g_ncc_mmid[1] == id)
            t = 1;
        else {
            t = g_ncc_next;
            grTexDownloadTable(mm->tmu, t ? GR_TEXTABLE_NCC1 : GR_TEXTABLE_NCC0, &mm->ncc_table);
            g_ncc_mmid[t] = id;
            g_ncc_next    = !t;
        }
        grTexNCCTable(mm->tmu, t ? GR_NCCTABLE_NCC1 : GR_NCCTABLE_NCC0);
    }
    g_mm_current = id;
}

/* src: all levels from large to small back to back (as for
 * grTexDownloadMipMap); for YIQ formats the NCC table belongs to it. */
void GR_EXPORT GLIDE_API guTexDownloadMipMap(GrMipMapId_t mmid, const void *src, const GuNccTable *table) {
    GLIDE_TRACE("guTexDownloadMipMap");
    GrMipMapInfo *mm = mm_get(mmid);
    if (!mm || !src)
        return;
    if (table && (mm->format == GR_TEXFMT_YIQ_422 || mm->format == GR_TEXFMT_AYIQ_8422))
        mm->ncc_table = *table;
    GrTexInfo info;
    mm_texinfo(mm, &info, (void *) src);
    grTexDownloadMipMap(mm->tmu, mm_start_address(mm), mm->odd_even_mask, &info);
    mm->data = (void *) src;
}

/* One level; *src is advanced past the level so consecutive calls walk a
 * whole chain. */
void GR_EXPORT GLIDE_API guTexDownloadMipMapLevel(GrMipMapId_t mmid, GrLOD_t lod, const void **src) {
    GLIDE_TRACE("guTexDownloadMipMapLevel");
    GrMipMapInfo *mm = mm_get(mmid);
    if (!mm || !src || !*src || lod < mm->lod_max || lod > mm->lod_min)
        return;
    grTexDownloadMipMapLevel(mm->tmu, mm_start_address(mm), lod, mm->lod_max, mm->aspect_ratio, mm->format,
                             mm->odd_even_mask, (void *) *src);
    *src = (const uint8_t *) *src + glide_tex_levels_bytes(lod, lod + 1, mm->aspect_ratio, mm->format);
}

/* --- Texture combine presets -------------------------------------- */

/* Fixed grTexCombine combinations, checked against 3dfx's Glide 2.x
 * (sst1/glide/src/digutex.c). */
void GR_EXPORT GLIDE_API grTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t fnc) {
    GLIDE_TRACE("grTexCombineFunction");
    GrCombineFunction_t f   = GR_COMBINE_FUNCTION_LOCAL;
    GrCombineFactor_t   rgb = GR_COMBINE_FACTOR_NONE, a = GR_COMBINE_FACTOR_NONE;
    FxBool              inv = FXFALSE;
    switch (fnc) {
    case GR_TEXTURECOMBINE_ZERO: f = GR_COMBINE_FUNCTION_ZERO; break;
    case GR_TEXTURECOMBINE_DECAL: f = GR_COMBINE_FUNCTION_LOCAL; break;
    case GR_TEXTURECOMBINE_OTHER: f = GR_COMBINE_FUNCTION_SCALE_OTHER; rgb = a = GR_COMBINE_FACTOR_ONE; break;
    case GR_TEXTURECOMBINE_ADD:
        f   = GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL;
        rgb = a = GR_COMBINE_FACTOR_ONE;
        break;
    case GR_TEXTURECOMBINE_MULTIPLY:
        f   = GR_COMBINE_FUNCTION_SCALE_OTHER;
        rgb = a = GR_COMBINE_FACTOR_LOCAL;
        break;
    case GR_TEXTURECOMBINE_SUBTRACT:
        f   = GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL;
        rgb = a = GR_COMBINE_FACTOR_ONE;
        break;
    case GR_TEXTURECOMBINE_DETAIL:
        f   = GR_COMBINE_FUNCTION_BLEND;
        rgb = a = GR_COMBINE_FACTOR_ONE_MINUS_DETAIL_FACTOR;
        break;
    case GR_TEXTURECOMBINE_DETAIL_OTHER:
        f   = GR_COMBINE_FUNCTION_BLEND;
        rgb = a = GR_COMBINE_FACTOR_DETAIL_FACTOR;
        break;
    case GR_TEXTURECOMBINE_TRILINEAR_ODD:
        f   = GR_COMBINE_FUNCTION_BLEND;
        rgb = a = GR_COMBINE_FACTOR_ONE_MINUS_LOD_FRACTION;
        break;
    case GR_TEXTURECOMBINE_TRILINEAR_EVEN:
        f   = GR_COMBINE_FUNCTION_BLEND;
        rgb = a = GR_COMBINE_FACTOR_LOD_FRACTION;
        break;
    case GR_TEXTURECOMBINE_ONE: f = GR_COMBINE_FUNCTION_ZERO; inv = FXTRUE; break;
    default: return;
    }
    grTexCombine(tmu, f, rgb, f, a, inv, inv);
}

void GR_EXPORT GLIDE_API guTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t fnc) {
    GLIDE_TRACE("guTexCombineFunction");
    grTexCombineFunction(tmu, fnc);
}

/* --- .3DF files --------------------------------------------------- */

#define TDF_HEADER_MAX 256 /* header: four short text lines */

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Compare the word at *p with word (case-insensitive); a blank or line
 * end must follow. On a match, *p moves past it. */
static int match_word(const char **p, const char *word) {
    const char *s = *p;
    while (*word) {
        if (lower((unsigned char) *s) != *word)
            return 0;
        s++;
        word++;
    }
    if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
        return 0;
    *p = s;
    return 1;
}

static void skip_blanks(const char **p) {
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

static int parse_uint(const char **p, int *v) {
    skip_blanks(p);
    if (**p < '0' || **p > '9')
        return 0;
    int n = 0;
    while (**p >= '0' && **p <= '9')
        n = n * 10 + (*(*p)++ - '0');
    *v = n;
    return 1;
}

/* Skip past the next '\n'; 0 if there is none left in the buffer. */
static int next_line(const char **p, const char *end) {
    while (*p < end && **p != '\n')
        (*p)++;
    if (*p >= end)
        return 0;
    (*p)++;
    return 1;
}

static GrLOD_t size_to_lod(int size) {
    for (GrLOD_t lod = GR_LOD_256; lod <= GR_LOD_1; lod++)
        if ((256 >> lod) == size)
            return lod;
    return -1;
}

static int tdf_format(const char **p, GrTextureFormat_t *fmt) {
    static const struct {
        const char       *name;
        GrTextureFormat_t fmt;
    } names[] = {
        { "i8", GR_TEXFMT_INTENSITY_8 },         { "a8", GR_TEXFMT_ALPHA_8 },
        { "ai44", GR_TEXFMT_ALPHA_INTENSITY_44 }, { "yiq", GR_TEXFMT_YIQ_422 },
        { "rgb332", GR_TEXFMT_RGB_332 },          { "rgb565", GR_TEXFMT_RGB_565 },
        { "argb8332", GR_TEXFMT_ARGB_8332 },      { "argb1555", GR_TEXFMT_ARGB_1555 },
        { "ayiq8422", GR_TEXFMT_AYIQ_8422 },      { "argb4444", GR_TEXFMT_ARGB_4444 },
        { "ai88", GR_TEXFMT_ALPHA_INTENSITY_88 }, { "p8", GR_TEXFMT_P_8 },
        { "ap88", GR_TEXFMT_AP_88 },
    };
    skip_blanks(p);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (match_word(p, names[i].name)) {
            *fmt = names[i].fmt;
            return 1;
        }
    return 0;
}

/* Parse the header; returns the byte offset after it, 0 on error. */
static size_t tdf_parse_header(const char *filename, Gu3dfInfo *info) {
    char  buf[TDF_HEADER_MAX];
    long  n = platform_read_file(filename, 0, buf, sizeof buf);
    if (n <= 0)
        return 0;
    const char *p = buf, *end = buf + n;
    int         small, large, aw, ah;
    GrTextureFormat_t fmt;

    /* "3df v1.0": as in Glide the version is not evaluated. */
    if (!match_word(&p, "3df") || !next_line(&p, end))
        return 0;
    if (!tdf_format(&p, &fmt) || !next_line(&p, end))
        return 0;
    skip_blanks(&p);
    if (!match_word(&p, "lod") || (skip_blanks(&p), !match_word(&p, "range:")) || !parse_uint(&p, &small) ||
        !parse_uint(&p, &large) || !next_line(&p, end))
        return 0;
    skip_blanks(&p);
    if (!match_word(&p, "aspect") || (skip_blanks(&p), !match_word(&p, "ratio:")) || !parse_uint(&p, &aw) ||
        !parse_uint(&p, &ah) || !next_line(&p, end))
        return 0;

    GrLOD_t small_lod = size_to_lod(small), large_lod = size_to_lod(large);
    GrAspectRatio_t aspect;
    switch (aw * 16 + ah) {
    case 8 * 16 + 1: aspect = GR_ASPECT_8x1; break;
    case 4 * 16 + 1: aspect = GR_ASPECT_4x1; break;
    case 2 * 16 + 1: aspect = GR_ASPECT_2x1; break;
    case 1 * 16 + 1: aspect = GR_ASPECT_1x1; break;
    case 1 * 16 + 2: aspect = GR_ASPECT_1x2; break;
    case 1 * 16 + 4: aspect = GR_ASPECT_1x4; break;
    case 1 * 16 + 8: aspect = GR_ASPECT_1x8; break;
    default: return 0;
    }
    if (small_lod < 0 || large_lod < 0 || large_lod > small_lod)
        return 0;

    info->header.small_lod    = small_lod;
    info->header.large_lod    = large_lod;
    info->header.aspect_ratio = aspect;
    info->header.format       = fmt;
    info->header.width        = (FxU32) (aw >= ah ? large : large * aw / ah);
    info->header.height       = (FxU32) (ah >= aw ? large : large * ah / aw);
    info->mem_required        = glide_tex_levels_bytes(large_lod, small_lod + 1, aspect, fmt); /* unrounded, as in 3dfx's code */
    return (size_t) (p - buf);
}

FxBool GR_EXPORT GLIDE_API gu3dfGetInfo(const char *filename, Gu3dfInfo *info) {
    GLIDE_TRACE("gu3dfGetInfo");
    if (!filename || !info)
        return FXFALSE;
    return tdf_parse_header(filename, info) ? FXTRUE : FXFALSE;
}

static FxU32 be16(const uint8_t *p) {
    return (FxU32) ((p[0] << 8) | p[1]);
}

static FxU32 be32(const uint8_t *p) {
    return ((FxU32) p[0] << 24) | ((FxU32) p[1] << 16) | ((FxU32) p[2] << 8) | p[3];
}

/* NCC table: 16 Y values, 4x3 I and 4x3 Q values, 16 bits each; plus the
 * packed register format (as 3dfx's Glide stores it in packed_data). */
static void tdf_ncc(const uint8_t *p, GuNccTable *ncc) {
    for (int i = 0; i < 16; i++)
        ncc->yRGB[i] = (FxU8) be16(p + 2 * i);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) {
            ncc->iRGB[i][j] = (FxI16) (be16(p + 32 + 2 * (i * 3 + j)) & 0x1ff); /* 9 bits, as in 3dfx's code */
            ncc->qRGB[i][j] = (FxI16) (be16(p + 56 + 2 * (i * 3 + j)) & 0x1ff);
        }
    for (int i = 0; i < 4; i++) {
        ncc->packed_data[i] = (FxU32) ncc->yRGB[4 * i] | ((FxU32) ncc->yRGB[4 * i + 1] << 8) |
                              ((FxU32) ncc->yRGB[4 * i + 2] << 16) | ((FxU32) ncc->yRGB[4 * i + 3] << 24);
        ncc->packed_data[4 + i] = (((FxU32) ncc->iRGB[i][0] << 18) & 0x07fc0000u) |
                                  (((FxU32) ncc->iRGB[i][1] << 9) & 0x0003fe00u) | ((FxU32) ncc->iRGB[i][2] & 0x1ffu);
        ncc->packed_data[8 + i] = (((FxU32) ncc->qRGB[i][0] << 18) & 0x07fc0000u) |
                                  (((FxU32) ncc->qRGB[i][1] << 9) & 0x0003fe00u) | ((FxU32) ncc->qRGB[i][2] & 0x1ffu);
    }
}

/* info->data must be allocated by the caller with info->mem_required
 * bytes (as in the Glide SDK: gu3dfGetInfo, malloc, gu3dfLoad). */
FxBool GR_EXPORT GLIDE_API gu3dfLoad(const char *filename, Gu3dfInfo *data) {
    GLIDE_TRACE("gu3dfLoad");
    if (!filename || !data || !data->data)
        return FXFALSE;
    size_t offset = tdf_parse_header(filename, data);
    if (!offset)
        return FXFALSE;
    GrTextureFormat_t fmt = data->header.format;

    if (fmt == GR_TEXFMT_YIQ_422 || fmt == GR_TEXFMT_AYIQ_8422) {
        uint8_t raw[80]; /* 16 + 12 + 12 values of 16 bits */
        if (platform_read_file(filename, offset, raw, sizeof raw) != (long) sizeof raw)
            return FXFALSE;
        tdf_ncc(raw, &data->table.nccTable);
        offset += sizeof raw;
    } else if (fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88) {
        uint8_t raw[256 * 4];
        if (platform_read_file(filename, offset, raw, sizeof raw) != (long) sizeof raw)
            return FXFALSE;
        for (int i = 0; i < 256; i++)
            data->table.palette.data[i] = be32(raw + 4 * i);
        offset += sizeof raw;
    }

    /* Unrounded data size: sum of the levels. */
    FxU32 bytes = glide_tex_levels_bytes(data->header.large_lod, data->header.small_lod + 1,
                                         data->header.aspect_ratio, fmt);
    if (platform_read_file(filename, offset, data->data, bytes) != (long) bytes)
        return FXFALSE;
    if (fmt >= GR_TEXFMT_16BIT) {
        uint8_t *p = (uint8_t *) data->data;
        for (FxU32 i = 0; i + 1 < bytes; i += 2) {
            uint8_t t = p[i];
            p[i]      = p[i + 1];
            p[i + 1]  = t;
        }
    }
    return FXTRUE;
}
