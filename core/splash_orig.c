/* GLIDULATOR_SPLASH=original: 3dfx's splash animation (spinning 3Dfx logo
 * in front of the power shield), with the model read from an original
 * Glide 2.x binary (GLIDULATOR_SPLASH_FILE = GLIDE2X.OVL or glide2x.dll)
 * and drawn by this file through Glidulator's Glide functions. No 3dfx
 * data is part of Glidulator.
 *
 * Locating the data: 3dfx's Voodoo Graphics Glide 2.x carries the splash as
 * initialised data, in this order: per object (3) a vertex array (x y z,
 * normal, s t; 8 floats) followed by its face array (3 vertex indices,
 * material, edge flags; 5 ints), then the object tables (vertex pointers,
 * vertex counts, face pointers, face counts), the frame count, one 4x4
 * matrix per frame and object, and three textures, each a Gu3dfInfo
 * (1056 bytes) followed by its texel data. Compilers pad between the groups
 * (MSVC DLL: zero dwords, Watcom OVL: none) and the pointers are either
 * absolute (DLL) or unrelocated object offsets (OVL), so the search does
 * not use fixed offsets or pointer values: it finds the table block by its
 * internal consistency (pointer differences = array sizes) and derives the
 * array positions from the counts. Every array is validated before use; a
 * file that does not match is rejected (fall back to the GL-1 logo).
 *
 * Rendering follows the documented behaviour of the 3dfx splash (Glide 2.x
 * sources): projection with a 480/640 aspect viewport, lower-left origin,
 * W buffer, per-vertex diffuse light from (-1,-1,-1), material colours,
 * textured "3D" letters, a specular pass with normal-mapped highlight
 * texture, the logo's projected shadow on the shield, fog-table fades, 30
 * frames per second. */

#include "glidulator/platform.h"
#include "glidulator/trace.h"

#include "glide_internal.h"

#define FILE_MAX   (1024 * 1024)
#define OBJS       3
#define VERT_BYTES 32
#define FACE_BYTES 20
#define INFO_BYTES 1056 /* sizeof(Gu3dfInfo) in 32-bit Glide 2.x */
#define FRAME_US   33333u

typedef struct {
    float x, y, z, nx, ny, nz, s, t;
} sp_vert;

typedef struct {
    int32_t v[3], mat, aa;
} sp_face;

typedef struct {
    const uint8_t *raw; /* Gu3dfInfo */
    GrTexInfo      info;
    FxU32          addr;
    int            ncc;
} sp_tex;

static const sp_vert *g_vert[OBJS];
static const sp_face *g_face[OBJS];
static int            g_nv[OBJS], g_nf[OBJS];
static const float   *g_mat; /* [frame][obj][16] */
static int            g_frames;
static sp_tex         g_tex[3]; /* text, hilite, shadow */
static float         *g_xv, *g_xn; /* transformed vertices/normals */
static float          g_vp[4];
static int            g_pass, g_phong, g_prev_mat;
static const sp_tex  *g_last_tex;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static float rdf(const uint8_t *p) {
    union {
        uint32_t u;
        float    f;
    } c;
    c.u = rd32(p);
    return c.f;
}

static int finite_small(float f, float lim) {
    return f == f && f > -lim && f < lim;
}

static int valid_verts(const uint8_t *p, int n) {
    int unit = 0;
    for (int i = 0; i < n; i++, p += VERT_BYTES) {
        float f[8];
        for (int k = 0; k < 8; k++) {
            f[k] = rdf(p + 4 * k);
            if (!finite_small(f[k], 100000.0f))
                return 0;
        }
        float l = f[3] * f[3] + f[4] * f[4] + f[5] * f[5];
        if (l > 0.9f && l < 1.1f)
            unit++;
        if (!finite_small(f[6], 4096.0f) || !finite_small(f[7], 4096.0f)) /* texel units, may wrap */
            return 0;
    }
    return unit * 10 >= n * 9;
}

static int valid_faces(const uint8_t *p, int n, int nv) {
    for (int i = 0; i < n; i++, p += FACE_BYTES) {
        for (int k = 0; k < 3; k++)
            if (rd32(p + 4 * k) >= (uint32_t) nv)
                return 0;
        if (rd32(p + 12) > 4 || rd32(p + 16) > 7)
            return 0;
    }
    return 1;
}

static int is_matrix(const uint8_t *p) {
    for (int k = 0; k < 16; k++)
        if (!finite_small(rdf(p + 4 * k), 1.0e6f))
            return 0;
    return rdf(p + 12) == 0.0f && rdf(p + 28) == 0.0f && rdf(p + 44) == 0.0f && rdf(p + 60) == 1.0f;
}

/* Next nonzero dword at or after *o (at most 3 zero dwords skipped). */
static int next_dw(const uint8_t *d, size_t n, size_t *o, uint32_t *v) {
    for (int skip = 0; skip < 4; skip++, *o += 4) {
        if (*o + 4 > n)
            return 0;
        *v = rd32(d + *o);
        if (*v) {
            *o += 4;
            return 1;
        }
    }
    return 0;
}

/* Texture header at p: plausible Gu3dfHeader and mem_required matching the
 * levels; returns the texel byte count (0 = no). */
static FxU32 tex_header(const uint8_t *p, size_t avail) {
    if (avail < INFO_BYTES)
        return 0;
    uint32_t w = rd32(p), h = rd32(p + 4), sl = rd32(p + 8), ll = rd32(p + 12), ar = rd32(p + 16), fm = rd32(p + 20);
    if (w < 1 || w > 256 || h < 1 || h > 256 || sl > GR_LOD_1 || ll > sl || ar > GR_ASPECT_1x8 || fm > GR_TEXFMT_AP_88)
        return 0;
    FxU32 bytes = glide_tex_levels_bytes((GrLOD_t) ll, (GrLOD_t) sl + 1, (GrAspectRatio_t) ar, (GrTextureFormat_t) fm);
    FxU32 mem   = rd32(p + INFO_BYTES - 4);
    if (!bytes || mem < bytes || mem > bytes + 16 || avail < INFO_BYTES + mem)
        return 0;
    return mem;
}

static int parse(const uint8_t *d, size_t n) {
    for (size_t x = 0; x + 64 <= n; x += 4) {
        uint32_t p[3], cnt[3], q[3], fc[3], frames;
        size_t   o  = x;
        int      ok = 1;
        for (int k = 0; k < 3 && ok; k++)
            ok = next_dw(d, n, &o, &p[k]);
        for (int k = 0; k < 3 && ok; k++)
            ok = next_dw(d, n, &o, &cnt[k]);
        for (int k = 0; k < 3 && ok; k++)
            ok = next_dw(d, n, &o, &q[k]);
        for (int k = 0; k < 3 && ok; k++)
            ok = next_dw(d, n, &o, &fc[k]);
        if (!ok || !next_dw(d, n, &o, &frames))
            continue;
        if (rd32(d + x) != p[0] || frames < 2 || frames > 1000)
            continue;
        for (int k = 0; k < 3 && ok; k++)
            ok = cnt[k] < 8192 && fc[k] < 8192 && q[k] - p[k] == cnt[k] * VERT_BYTES;
        if (!ok || p[1] - p[0] != cnt[0] * VERT_BYTES + fc[0] * FACE_BYTES ||
            p[2] - p[1] != cnt[1] * VERT_BYTES + fc[1] * FACE_BYTES)
            continue;
        /* geometry ends right before the tables (up to 12 bytes padding) */
        size_t span = (size_t) (p[2] - p[0]) + cnt[2] * VERT_BYTES + fc[2] * FACE_BYTES;
        size_t v0   = 0;
        for (size_t pad = 0; pad <= 12; pad += 4)
            if (x >= span + pad && valid_verts(d + x - span - pad, (int) cnt[0])) {
                v0 = x - span - pad;
                break;
            }
        if (!v0)
            continue;
        for (int k = 0; k < 3 && ok; k++) {
            size_t vo = v0 + (p[k] - p[0]);
            ok        = valid_verts(d + vo, (int) cnt[k]) &&
                 valid_faces(d + vo + cnt[k] * VERT_BYTES, (int) fc[k], (int) cnt[k]);
            g_vert[k] = (const sp_vert *) (const void *) (d + vo);
            g_face[k] = (const sp_face *) (const void *) (d + vo + cnt[k] * VERT_BYTES);
            g_nv[k]   = (int) cnt[k];
            g_nf[k]   = (int) fc[k];
        }
        if (!ok)
            continue;
        /* matrices after the frame count */
        while (o + 64 <= n && !is_matrix(d + o) && o < x + 96)
            o += 4;
        size_t m = o;
        int    count = 0;
        while (o + 64 <= n && is_matrix(d + o)) {
            o += 64;
            count++;
        }
        if (count < (int) frames * OBJS)
            continue;
        g_mat    = (const float *) (const void *) (d + m);
        g_frames = (int) frames;
        /* three textures */
        for (int k = 0; k < 3 && ok; k++) {
            FxU32  mem = 0;
            size_t end = o + 64;
            while (o < end && o < n && !(mem = tex_header(d + o, n - o)))
                o += 4;
            if (!mem) {
                ok = 0;
                break;
            }
            const uint8_t *r     = d + o;
            g_tex[k].raw         = r;
            g_tex[k].info.smallLod    = (GrLOD_t) rd32(r + 8);
            g_tex[k].info.largeLod    = (GrLOD_t) rd32(r + 12);
            g_tex[k].info.aspectRatio = (GrAspectRatio_t) rd32(r + 16);
            g_tex[k].info.format      = (GrTextureFormat_t) rd32(r + 20);
            g_tex[k].info.data        = (void *) (r + INFO_BYTES);
            g_tex[k].ncc = g_tex[k].info.format == GR_TEXFMT_YIQ_422 || g_tex[k].info.format == GR_TEXFMT_AYIQ_8422;
            o += INFO_BYTES + mem;
        }
        if (ok)
            return 1;
    }
    return 0;
}

/* --- rendering ------------------------------------------------------ */

static void source(const sp_tex *t) {
    if (t == g_last_tex)
        return;
    g_last_tex = t;
    grTexSource(GR_TMU0, t->addr, GR_MIPMAPLEVELMASK_BOTH, (GrTexInfo *) &t->info);
    if (t->ncc) {
        GuNccTable ncc; /* table member of the Gu3dfInfo, copied for alignment */
        const uint8_t *s = t->raw + 24;
        uint8_t       *dst = (uint8_t *) &ncc;
        for (size_t i = 0; i < sizeof ncc; i++)
            dst[i] = s[i];
        grTexDownloadTable(GR_TMU0, GR_TEXTABLE_NCC0, &ncc);
    }
}

static const float *matrix(int frame, int obj) {
    return g_mat + ((size_t) frame * OBJS + (size_t) obj) * 16;
}

static void xform(const float *m, const float *v, float *out) {
    out[0] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
    out[1] = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13];
    out[2] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
}

static void xform_obj(int frame, int obj) {
    const float *m = matrix(frame, obj);
    for (int i = 0; i < g_nv[obj]; i++) {
        const sp_vert *v = &g_vert[obj][i];
        float          p[3], n[3];
        xform(m, &v->x, p);
        float oow = 1.0f / p[2];
        g_xv[3 * i]     = p[0] * oow * g_vp[0] + g_vp[1];
        g_xv[3 * i + 1] = p[1] * oow * g_vp[2] + g_vp[3];
        g_xv[3 * i + 2] = p[2];
        n[0] = v->nx * m[0] + v->ny * m[4] + v->nz * m[8];
        n[1] = v->nx * m[1] + v->ny * m[5] + v->nz * m[9];
        n[2] = v->nx * m[2] + v->ny * m[6] + v->nz * m[10];
        g_xn[3 * i] = n[0];
        g_xn[3 * i + 1] = n[1];
        g_xn[3 * i + 2] = n[2];
    }
}

/* Materials: 0 "3D" letters (textured), 1 "fx" (black with highlight),
 * 2 cyan, 3 white, 4 yellow parts of the shield. */
static void material(int m) {
    switch (m) {
    case 0:
        if (g_pass == 1) {
            source(&g_tex[0]);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_LOCAL_ITERATED,
                           GR_COMBINE_OTHER_TEXTURE, FXFALSE);
            g_phong = 1;
        } else {
            source(&g_tex[1]);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                           GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        }
        break;
    case 1:
        source(&g_tex[1]);
        grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                       GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        g_phong = 1;
        break;
    default:
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED,
                       GR_COMBINE_OTHER_NONE, FXFALSE);
        g_phong = 0;
        break;
    }
}

static void intensity(int m, float *k) {
    static const float tab[5][3] = { { 1.0f, 1.0f, 1.0f },
                                     { 0.125f, 0.125f, 0.125f },
                                     { 10.0f / 255.0f, 75.0f / 255.0f, 120.0f / 255.0f },
                                     { 1.0f, 1.0f, 1.0f },
                                     { 248.0f / 255.0f, 204.0f / 255.0f, 0.0f } };
    k[0] = tab[m][0];
    k[1] = tab[m][1];
    k[2] = tab[m][2];
}

/* Highlight texture coordinates from the view-space normal. */
static void env_map(GrVertex *g, const sp_face *f, const float *k) {
    for (int i = 0; i < 3; i++) {
        const float *n = &g_xn[3 * f->v[i]];
        g[i].tmuvtx[0].sow = g[i].oow * (128.0f + n[0] * 128.0f);
        g[i].tmuvtx[0].tow = g[i].oow * (128.0f + n[1] * 128.0f);
        g[i].r = k[0] * 255.0f;
        g[i].g = k[1] * 255.0f;
        g[i].b = k[2] * 255.0f;
    }
}

static void draw_obj(int obj) {
    static const float light[3] = { -0.57735f, -0.57735f, -0.57735f };
    float              k[3]     = { 1.0f, 1.0f, 1.0f };
    for (int fi = 0; fi < g_nf[obj]; fi++) {
        const sp_face *f = &g_face[obj][fi];
        int            m = f->mat;
        /* Material state is only switched on a change, also across
         * passes, as 3dfx's code does. */
        if (m != g_prev_mat) {
            material(m);
            intensity(m, k);
            g_prev_mat = m;
        } else
            intensity(m, k);
        if (m != 0 && g_pass == 2)
            continue;
        GrVertex g[3];
        for (int i = 0; i < 3; i++) {
            const float   *p = &g_xv[3 * f->v[i]], *n = &g_xn[3 * f->v[i]];
            const sp_vert *v = &g_vert[obj][f->v[i]];
            for (size_t b = 0; b < sizeof g[i]; b++)
                ((uint8_t *) &g[i])[b] = 0;
            g[i].x   = p[0];
            g[i].y   = p[1];
            g[i].oow = 1.0f / p[2];
            g[i].tmuvtx[0].oow = g[i].oow;
            g[i].tmuvtx[0].sow = v->s * g[i].oow;
            g[i].tmuvtx[0].tow = v->t * g[i].oow;
            float d = ((light[0] * n[0] + light[1] * n[1] + light[2] * n[2]) + 1.0f) * 127.5f;
            g[i].r  = d * k[0];
            g[i].g  = d * k[1];
            g[i].b  = d * k[2];
            g[i].a  = 255.0f;
        }
        if (g_pass == 2) {
            env_map(g, f, k);
            grDrawTriangle(&g[0], &g[1], &g[2]);
            continue;
        }
        grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
        if (m == 0) {
            source(&g_tex[0]);
            grDrawTriangle(&g[0], &g[1], &g[2]);
        } else if (m != 1)
            grDrawTriangle(&g[0], &g[1], &g[2]);
        if (g_phong && m != 0) {
            env_map(g, f, k);
            grDrawTriangle(&g[0], &g[1], &g[2]);
        }
    }
}

static void project_pt(GrVertex *g, const float *p) {
    for (size_t b = 0; b < sizeof *g; b++)
        ((uint8_t *) g)[b] = 0;
    g->oow = 1.0f / p[2];
    g->x   = p[0] * g->oow * g_vp[0] + g_vp[1];
    g->y   = p[1] * g->oow * g_vp[2] + g_vp[3];
    g->tmuvtx[0].oow = g->oow;
}

/* Shadow of the logo (object 1) on the back plane of the shield
 * (object 0): the logo's bounding quad projected from the light onto the
 * plane, multiplied into the image with the shadow texture. */
static void shadow(int frame) {
    static const float local[4][3] = { { -280.0f, 0.0f, -160.0f }, { -280.0f, 0.0f, 150.0f },
                                       { 280.0f, 0.0f, 150.0f }, { 280.0f, 0.0f, -160.0f } };
    static const float tc[4][2]    = { { 10.5f, 127.5f }, { 10.5f, 0.5f }, { 255.0f, 0.5f }, { 255.0f, 127.5f } };
    const float       *sm = matrix(frame, 1), *rm = matrix(frame, 0);
    float              light[3];
    light[0] = 5.0f;
    light[1] = 300.0f;
    light[2] = -1500.0f + rm[14];
    float              plane    = rm[14] - 26.0f;
    GrVertex           g[4];
    for (int i = 0; i < 4; i++) {
        float v[3], p[3];
        xform(sm, local[i], v);
        float t = (plane - light[2]) / (v[2] - light[2]);
        p[0]    = light[0] + (v[0] - light[0]) * t;
        p[1]    = light[1] + (v[1] - light[1]) * t;
        p[2]    = plane;
        project_pt(&g[i], p);
        g[i].tmuvtx[0].sow = tc[i][0] * g[i].oow;
        g[i].tmuvtx[0].tow = tc[i][1] * g[i].oow;
    }
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    source(&g_tex[2]);
    grAlphaBlendFunction(GR_BLEND_DST_COLOR, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    grDrawTriangle(&g[0], &g[1], &g[2]); /* both windings: culling is on */
    grDrawTriangle(&g[0], &g[2], &g[3]);
    grDrawTriangle(&g[0], &g[2], &g[1]);
    grDrawTriangle(&g[0], &g[3], &g[2]);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    g_prev_mat = -1; /* the combine unit changed behind the material cache */
}

static void play(void) {
    float w = (float) g_width, h = (float) g_height;
    g_vp[0] = w * (480.0f / 640.0f);
    g_vp[1] = w / 2.0f;
    g_vp[2] = h;
    g_vp[3] = h / 2.0f;

    FxU32 addr = grTexMinAddress(GR_TMU0);
    for (int k = 0; k < 3; k++) {
        g_tex[k].addr = addr;
        grTexDownloadMipMap(GR_TMU0, addr, GR_MIPMAPLEVELMASK_BOTH, &g_tex[k].info);
        addr += grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &g_tex[k].info);
    }
    g_last_tex = 0;
    g_prev_mat = -1;

    glide_set_origin(GR_ORIGIN_LOWER_LEFT);
    grAlphaTestFunction(GR_CMP_ALWAYS);
    grChromakeyMode(GR_CHROMAKEY_DISABLE);
    grConstantColorValue(0xffffffff);
    grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
    grDepthMask(FXTRUE);
    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_FUNCTION_NONE,
                 GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
    grDepthBufferFunction(GR_CMP_LEQUAL);
    grCullMode(GR_CULL_NEGATIVE);
    grFogColorValue(0);

    int      fade_in = (int) ((float) g_frames * 0.3f), fade_out = (int) ((float) g_frames * 0.2f);
    GrFog_t  fog[GR_FOG_TABLE_SIZE];
    uint64_t t0 = platform_time_us();
    for (int frame = 1; frame < g_frames; frame++) {
        int fval = -1;
        if (frame < fade_in)
            fval = 255 - (int) (255.0f * (float) (frame + 1) / (float) fade_in);
        else if (frame > g_frames - fade_out)
            fval = 255 - (int) (255.0f * (float) (g_frames - frame) / (float) fade_out);
        if (fval >= 0) {
            for (int i = 0; i < GR_FOG_TABLE_SIZE; i++)
                fog[i] = (GrFog_t) (fval < 0 ? 0 : fval);
            grFogMode(GR_FOG_WITH_TABLE);
            grFogTable(fog);
        } else
            grFogMode(GR_FOG_DISABLE);
        grBufferClear(0, 0, GR_WDEPTHVALUE_FARTHEST);

        g_pass = 1;
        grDepthMask(FXFALSE); /* shield parts without Z: avoids Z fighting with the logo */
        xform_obj(frame, 2);
        draw_obj(2);
        xform_obj(frame, 0);
        draw_obj(0);
        grDepthMask(FXTRUE);

        grDepthBufferFunction(GR_CMP_ALWAYS);
        grFogMode(GR_FOG_DISABLE);
        shadow(frame);
        if (fval >= 0)
            grFogMode(GR_FOG_WITH_TABLE);
        grDepthBufferFunction(GR_CMP_LEQUAL);

        xform_obj(frame, 1);
        draw_obj(1);
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ZERO);
        g_pass = 2;
        draw_obj(1);
        g_pass = 1;
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);

        uint64_t t = (uint64_t) (frame - 1) * FRAME_US;
        glide_splash_present(t);
        glide_wait_until(t0, t + FRAME_US);
    }
    grFogMode(GR_FOG_DISABLE);
}

/* Load, play and release; 0 if the file is missing or does not contain
 * the splash data. */
int glide_splash_original(void) {
    char path[260];
    if (!platform_get_config("GLIDULATOR_SPLASH_FILE", path, sizeof path)) {
        platform_log("Splash: GLIDULATOR_SPLASH_FILE missing, GL-1 logo\n");
        return 0;
    }
    uint8_t *buf = (uint8_t *) platform_alloc(FILE_MAX);
    if (!buf)
        return 0;
    long n  = platform_read_file(path, 0, buf, FILE_MAX);
    int  ok = n > 0 && parse(buf, (size_t) n);
    if (ok) {
        int most = g_nv[0];
        for (int k = 1; k < OBJS; k++)
            if (g_nv[k] > most)
                most = g_nv[k];
        g_xv = (float *) platform_alloc((size_t) most * 3 * sizeof(float));
        g_xn = (float *) platform_alloc((size_t) most * 3 * sizeof(float));
        ok   = g_xv && g_xn;
        if (ok)
            play();
        platform_free(g_xv);
        platform_free(g_xn);
        g_xv = g_xn = 0;
    }
    if (!ok)
        platform_log("Splash: no 3dfx splash data in GLIDULATOR_SPLASH_FILE, GL-1 logo\n");
    platform_free(buf);
    return ok;
}
