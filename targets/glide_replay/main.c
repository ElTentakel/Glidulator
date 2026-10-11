/*
 * Replay of a frame captured by the debug variant (see
 * core/include/glidulator/capture.h): renders CAPnn.BIN with its texture
 * memory and tables on Linux through the same pipeline (core/ +
 * rasterizer) and writes a PNG. For debugging with real software: frame by
 * frame, triangle by triangle, with per-triangle figures.
 *
 *   glide_replay <dir> <CAPnn.BIN> <out.png> [options]
 *     --list            per triangle: texture, screen box, s/t range,
 *                       texels per pixel
 *     --only N          draw only triangle N (clears stay)
 *     --upto N          draw only triangles 0..N
 *     --point           point sampling instead of the recorded filter
 *     --renderer R      render path c|jit|auto (glidulator/renderer.h)
 *     --threads N       render threads (default: auto)
 *     --compare         render the frame with every available path and
 *                       compare colour and depth buffer pixel-exactly with
 *                       the C path (exit code 2 on a difference)
 *     --difftri         draw every triangle from the same starting state
 *                       with C and with JIT, list differing ones with their
 *                       mode registers (JIT debugging)
 *     --bench N         render the frame N times, print time per run
 *                       (meaningful only without sanitizers: make bench)
 * All GLIDULATOR_* settings apply (e.g. FSAA, video filter).
 */

#include "glidulator/capture.h"
#include "glidulator/glide2x.h"
#include "glidulator/platform.h"
#include "glidulator/renderer.h"
#include "../../core/glide_internal.h"

#include "vid_voodoo_texture.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t *g_frame;
static const uint32_t *g_frame32; /* video filter: XRGB8888 */
static int             g_fw, g_fh;

void platform_blit(const void *pixels, int w, int h, int stride_bytes) {
    (void) stride_bytes;
    g_frame   = (const uint16_t *) pixels;
    g_frame32 = NULL;
    g_fw      = w;
    g_fh      = h;
}

void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    (void) stride_bytes;
    g_frame32 = pixels;
    g_fw      = w;
    g_fh      = h;
}

static void *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t) len);
    if (buf && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *n = (size_t) len;
    return buf;
}

static int save_png(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return 1;
    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        fclose(fp);
        return 1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32) g_fw, (png_uint_32) g_fh, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_bytep row = (png_bytep) malloc((size_t) g_fw * 3);
    for (int y = 0; y < g_fh; y++) {
        for (int x = 0; x < g_fw; x++) {
            if (g_frame32) {
                uint32_t q     = g_frame32[y * g_fw + x];
                row[x * 3 + 0] = (uint8_t) (q >> 16);
                row[x * 3 + 1] = (uint8_t) (q >> 8);
                row[x * 3 + 2] = (uint8_t) q;
                continue;
            }
            uint16_t p  = g_frame[y * g_fw + x];
            uint8_t  r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
            row[x * 3 + 0] = (uint8_t) ((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (uint8_t) ((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (uint8_t) ((b5 << 3) | (b5 >> 2));
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(row);
    fclose(fp);
    return 0;
}

static GrScreenResolution_t res_of(uint32_t w, uint32_t h) {
    if (w == 320 && h == 200) return GR_RESOLUTION_320x200;
    if (w == 320 && h == 240) return GR_RESOLUTION_320x240;
    if (w == 400 && h == 256) return GR_RESOLUTION_400x256;
    if (w == 512 && h == 384) return GR_RESOLUTION_512x384;
    if (w == 640 && h == 400) return GR_RESOLUTION_640x400;
    if (w == 640 && h == 480) return GR_RESOLUTION_640x480;
    return GR_RESOLUTION_800x600;
}

static void apply_regs(const glide_capture_cmd *c, int point) {
    g_voodoo.params.fbzMode        = c->reg[CAP_FBZMODE];
    g_voodoo.params.fbzColorPath   = c->reg[CAP_FBZCOLORPATH];
    g_voodoo.params.alphaMode      = c->reg[CAP_ALPHAMODE];
    g_voodoo.params.fogMode        = c->reg[CAP_FOGMODE];
    g_voodoo.params.textureMode[0] = c->reg[CAP_TEXTUREMODE];
    if (point)
        g_voodoo.params.textureMode[0] &= ~6u;
    g_voodoo.params.tLOD[0]        = c->reg[CAP_TLOD];
    g_voodoo.params.texBaseAddr[0] = c->reg[CAP_TEXBASEADDR];
    g_voodoo.params.tformat[0]     = (c->reg[CAP_TEXTUREMODE] >> 8) & 0xf;
    g_voodoo.params.color0         = c->reg[CAP_COLOR0];
    g_voodoo.params.color1         = c->reg[CAP_COLOR1];
    g_voodoo.params.zaColor        = c->reg[CAP_ZACOLOR];
    g_voodoo.params.chromaKey      = c->reg[CAP_CHROMAKEY];
    g_voodoo.params.chromaKey_r    = (int) ((c->reg[CAP_CHROMAKEY] >> 16) & 0xff);
    g_voodoo.params.chromaKey_g    = (int) ((c->reg[CAP_CHROMAKEY] >> 8) & 0xff);
    g_voodoo.params.chromaKey_b    = (int) (c->reg[CAP_CHROMAKEY] & 0xff);
    g_sw.stw_hint                  = c->reg[CAP_STW_HINT];
    voodoo_recalc_tex12(&g_voodoo, 0);
}

/* Figures of a triangle: s/t = sow/oow, tow/oow (Glide's 256 space),
 * texels per pixel along the longest edge (size of the largest level from
 * tLOD). */
static void list_triangle(int idx, const glide_capture_cmd *c) {
    double smin = 1e9, smax = -1e9, tmin = 1e9, tmax = -1e9, xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
    for (int k = 0; k < 3; k++) {
        const GrVertex *v = &c->v[k];
        double s = v->oow ? v->tmuvtx[0].sow / v->oow : 0, t = v->oow ? v->tmuvtx[0].tow / v->oow : 0;
        smin = fmin(smin, s); smax = fmax(smax, s); tmin = fmin(tmin, t); tmax = fmax(tmax, t);
        xmin = fmin(xmin, v->x); xmax = fmax(xmax, v->x); ymin = fmin(ymin, v->y); ymax = fmax(ymax, v->y);
    }
    int large = (int) ((c->reg[CAP_TLOD] >> 2) & 0xf);
    printf("%4d tex=%s base=%08x lod=%d..%d fmt=%x  box=(%.0f,%.0f)-(%.0f,%.0f)  s=%.1f..%.1f t=%.1f..%.1f  oow=%.4f/%.4f/%.4f\n",
           idx, (c->reg[CAP_FBZCOLORPATH] & (1u << 27)) ? "ja " : "nein", c->reg[CAP_TEXBASEADDR], large,
           (int) ((c->reg[CAP_TLOD] >> 8) & 0xf), (c->reg[CAP_TEXTUREMODE] >> 8) & 0xf, xmin, ymin, xmax, ymax, smin,
           smax, tmin, tmax, c->v[0].oow, c->v[1].oow, c->v[2].oow);
    /* Colour/alpha of the first corner and the combine/blend registers. */
    printf("      rgba=%.0f/%.0f/%.0f/%.0f  fbzColorPath=%08x alphaMode=%08x fbzMode=%08x fogMode=%08x color0=%08x\n",
           c->v[0].r, c->v[0].g, c->v[0].b, c->v[0].a, c->reg[CAP_FBZCOLORPATH], c->reg[CAP_ALPHAMODE],
           c->reg[CAP_FBZMODE], c->reg[CAP_FOGMODE], c->reg[CAP_COLOR0]);
}

/* Play all commands of the frame; returns the number of triangles. */
static int render_frame(const glide_capture_header *h, const glide_capture_cmd *cmds, int list, int only, int upto,
                        int point) {
    int tri = 0;
    for (uint32_t i = 0; i < h->ncmd; i++) {
        const glide_capture_cmd *c = &cmds[i];
        apply_regs(c, point);
        if (c->kind == GLIDE_CAPTURE_CLEAR) {
            grClipWindow(0, 0, h->width, h->height);
            grBufferClear(c->reg[CAP_CLEAR_COLOR], 0, (FxU16) c->reg[CAP_CLEAR_DEPTH]);
            continue;
        }
        if (list)
            list_triangle(tri, c);
        if ((only < 0 || tri == only) && (upto < 0 || tri <= upto)) {
            grClipWindow(c->reg[CAP_CLIP_LEFT], c->reg[CAP_CLIP_TOP], c->reg[CAP_CLIP_RIGHT], c->reg[CAP_CLIP_BOTTOM]);
            grDrawTriangle(&c->v[0], &c->v[1], &c->v[2]);
        }
        tri++;
    }
    return tri;
}

/* --difftri: C against JIT per triangle (toggles use_recompiler directly:
 * glide_renderer_apply() would clear the code cache every time). */
static int diff_triangles(const glide_capture_header *h, const glide_capture_cmd *cmds, const uint8_t *start,
                          size_t fb_bytes, int point) {
    uint8_t *before = malloc(fb_bytes), *c_out = malloc(fb_bytes);
    int      tri = 0, bad = 0;
    glide_renderer_force(GLIDE_RENDERER_JIT, 1);
    glide_renderer_apply();
    if (glide_renderer_active() != GLIDE_RENDERER_JIT) {
        printf("JIT not available\n");
        return 1;
    }
    memcpy(g_voodoo.fb_mem, start, fb_bytes);
    for (uint32_t i = 0; i < h->ncmd; i++) {
        const glide_capture_cmd *c = &cmds[i];
        apply_regs(c, point);
        if (c->kind == GLIDE_CAPTURE_CLEAR) {
            grClipWindow(0, 0, h->width, h->height);
            grBufferClear(c->reg[CAP_CLEAR_COLOR], 0, (FxU16) c->reg[CAP_CLEAR_DEPTH]);
            continue;
        }
        grClipWindow(c->reg[CAP_CLIP_LEFT], c->reg[CAP_CLIP_TOP], c->reg[CAP_CLIP_RIGHT], c->reg[CAP_CLIP_BOTTOM]);
        memcpy(before, g_voodoo.fb_mem, fb_bytes);
        g_voodoo.use_recompiler = 0;
        grDrawTriangle(&c->v[0], &c->v[1], &c->v[2]);
        memcpy(c_out, g_voodoo.fb_mem, fb_bytes);
        memcpy(g_voodoo.fb_mem, before, fb_bytes);
        g_voodoo.use_recompiler = 1;
        grDrawTriangle(&c->v[0], &c->v[1], &c->v[2]);
        const uint16_t *a = (const uint16_t *) c_out, *b = (const uint16_t *) g_voodoo.fb_mem;
        size_t          n = 0, first = 0, half = (size_t) g_aux_offset / 2;
        int             maxd = 0;
        for (size_t p = 0; p < fb_bytes / 2; p++)
            if (a[p] != b[p]) {
                if (!n++)
                    first = p;
                if (p < half) {
                    int d = abs((int) (a[p] >> 11) - (int) (b[p] >> 11)) +
                            abs((int) ((a[p] >> 5) & 63) - (int) ((b[p] >> 5) & 63)) +
                            abs((int) (a[p] & 31) - (int) (b[p] & 31));
                    if (d > maxd)
                        maxd = d;
                }
            }
        if (n) {
            if (bad < (getenv("DIFFALL") ? 1000000 : 40))
                printf("triangle %4d: %6zu values, first %s (%zu,%zu), max. colour distance %d  fbzColorPath=%08x "
                       "alphaMode=%08x fbzMode=%08x fogMode=%08x textureMode=%08x tLOD=%08x\n",
                       tri, n, first < half ? "colour" : "depth", (first % half) % h->width, (first % half) / h->width,
                       maxd, c->reg[CAP_FBZCOLORPATH], c->reg[CAP_ALPHAMODE], c->reg[CAP_FBZMODE],
                       c->reg[CAP_FOGMODE], c->reg[CAP_TEXTUREMODE], c->reg[CAP_TLOD]);
            bad++;
        }
        memcpy(g_voodoo.fb_mem, c_out, fb_bytes); /* continue with the C result */
        tri++;
    }
    printf("%d of %d triangles differ\n", bad, tri);
    free(before);
    free(c_out);
    return bad ? 2 : 0;
}

/* Render with path kind (framebuffer reset to start first); reps > 1:
 * repeat and measure the time per run. */
static int run_with(int kind, int threads, const glide_capture_header *h, const glide_capture_cmd *cmds,
                    const uint8_t *start, size_t fb_bytes, int list, int only, int upto, int point, int reps) {
    glide_renderer_force(kind, threads);
    glide_renderer_apply();
    uint64_t best = (uint64_t) -1, sum = 0;
    int      tri  = 0;
    for (int r = 0; r < reps; r++) {
        for (int k = 0; k < g_fsaa_n; k++) /* FSAA (GLIDULATOR_FSAA): all planes */
            memcpy(g_voodoo.fb_mem + (size_t) k * g_fsaa_plane_bytes, start, fb_bytes);
        uint64_t t0 = platform_time_us();
        tri         = render_frame(h, cmds, list && r == 0, only, upto, point);
        glide_mt_idle(); /* render workers done (core/render_mt.c) */
        uint64_t dt = platform_time_us() - t0;
        sum += dt;
        if (dt < best)
            best = dt;
    }
    if (reps > 1)
        printf("%-4s threads %d: %d runs, mean %.2f ms, best %.2f ms (%.1f fps)\n",
               glide_renderer_name(glide_renderer_active()),
               glide_renderer_threads(), reps, sum / 1000.0 / reps, best / 1000.0, 1e6 / (double) best);
    return tri;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <dir> <CAPnn.BIN> <out.png> [--list] [--only N] [--upto N] [--point] [--renderer R] [--threads N] [--compare] [--bench N]\n", argv[0]);
        return 1;
    }
    int list = 0, only = -1, upto = -1, point = 0, compare = 0, bench = 0, difftri = 0;
    int kind = GLIDE_RENDERER_AUTO, threads = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--list"))
            list = 1;
        else if (!strcmp(argv[i], "--point"))
            point = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upto") && i + 1 < argc)
            upto = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            kind = glide_renderer_parse(argv[++i]);
            if (kind == GLIDE_RENDERER_UNKNOWN) {
                fprintf(stderr, "unknown renderer %s\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compare"))
            compare = 1;
        else if (!strcmp(argv[i], "--difftri"))
            difftri = 1;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);
    }

    char   path[1024];
    size_t n_tex, n_pal, n_cap;
    snprintf(path, sizeof path, "%s/TEXMEM.BIN", argv[1]);
    uint8_t *tex = read_all(path, &n_tex);
    snprintf(path, sizeof path, "%s/PALETTE.BIN", argv[1]);
    uint32_t *pal = read_all(path, &n_pal);
    snprintf(path, sizeof path, "%s/%s", argv[1], argv[2]);
    uint8_t *cap = read_all(path, &n_cap);
    if (!tex || !pal || !cap || n_tex != TEX_MEM_BYTES || n_cap < sizeof(glide_capture_header)) {
        fprintf(stderr, "capture incomplete in %s\n", argv[1]);
        return 1;
    }
    const glide_capture_header *h    = (const glide_capture_header *) cap;
    const glide_capture_cmd    *cmds = (const glide_capture_cmd *) (cap + sizeof *h);
    if (h->magic != GLIDE_CAPTURE_MAGIC || h->version != GLIDE_CAPTURE_VERSION ||
        n_cap < sizeof *h + h->ncmd * sizeof(glide_capture_cmd)) {
        fprintf(stderr, "no valid GCAP v%u\n", GLIDE_CAPTURE_VERSION);
        return 1;
    }

    grGlideInit();
    if (!grSstWinOpen(0, res_of(h->width, h->height), 0, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        fprintf(stderr, "grSstWinOpen(%ux%u) failed\n", h->width, h->height);
        return 1;
    }
    memcpy(g_voodoo.tex_mem[0], tex, TEX_MEM_BYTES);
    for (int i = 0; i < 256; i++)
        g_voodoo.palette[0][i].u = pal[i];
    g_voodoo.palette_dirty[0] = 1;

    /* Per-frame TEXnn.BIN/TABnn.BIN (texture memory, palette, NCC and fog
     * tables of the same frame) when present; otherwise the TEXMEM.BIN
     * written at the first recorded frame. */
    if (strlen(argv[2]) >= 5 && !strncmp(argv[2], "CAP", 3)) {
        char   nn[3] = { argv[2][3], argv[2][4], 0 };
        size_t n_t, n_b;
        snprintf(path, sizeof path, "%s/TEX%s.BIN", argv[1], nn);
        uint8_t *t2 = read_all(path, &n_t);
        snprintf(path, sizeof path, "%s/TAB%s.BIN", argv[1], nn);
        glide_capture_tables *tb = read_all(path, &n_b);
        if (t2 && tb && n_t == TEX_MEM_BYTES && n_b == sizeof *tb) {
            memcpy(g_voodoo.tex_mem[0], t2, TEX_MEM_BYTES);
            for (int i = 0; i < 256; i++)
                g_voodoo.palette[0][i].u = tb->palette[i];
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < 4; k++) {
                    g_voodoo.nccTable[0][t].y[k] = tb->ncc[t][k];
                    g_voodoo.nccTable[0][t].i[k] = tb->ncc[t][4 + k];
                    g_voodoo.nccTable[0][t].q[k] = tb->ncc[t][8 + k];
                }
            voodoo_update_ncc(&g_voodoo, 0);
            for (int i = 0; i < 64; i++) {
                g_voodoo.params.fogTable[i].fog  = tb->fog[i][0];
                g_voodoo.params.fogTable[i].dfog = tb->fog[i][1];
            }
            g_voodoo.params.fogColor.r = (uint8_t) (tb->fog_color >> 16);
            g_voodoo.params.fogColor.g = (uint8_t) (tb->fog_color >> 8);
            g_voodoo.params.fogColor.b = (uint8_t) tb->fog_color;
            printf("texture memory/tables for the frame: TEX%s.BIN, TAB%s.BIN\n", nn, nn);
        }
        free(t2);
        free(tb);
    }

    /* Starting state of the framebuffer (colour + depth), so every run
     * (--compare, --bench) starts the same. */
    size_t   fb_bytes = (size_t) g_aux_offset * 2;
    uint8_t *start    = malloc(fb_bytes);
    uint8_t *ref      = malloc(fb_bytes);
    memcpy(start, g_voodoo.fb_mem, fb_bytes);

    if (difftri) {
        int r = diff_triangles(h, cmds, start, fb_bytes, point);
        grGlideShutdown();
        return r;
    }
    int rc  = 0;
    int tri = run_with(kind, threads, h, cmds, start, fb_bytes, list, only, upto, point, bench > 1 ? bench : 1);
    if (kind >= 0 && glide_renderer_active() != kind)
        printf("renderer %s not available, using: %s\n", glide_renderer_name(kind),
               glide_renderer_name(glide_renderer_active()));
    if (compare) {
        run_with(GLIDE_RENDERER_C, 1, h, cmds, start, fb_bytes, 0, only, upto, point, 1);
        memcpy(ref, g_voodoo.fb_mem, fb_bytes);
        for (int k = 0; k < GLIDE_RENDERER_COUNT; k++) {
            if (!glide_renderer_available(k)) {
                printf("compare %-4s: not available\n", glide_renderer_name(k));
                continue;
            }
            for (int t = 1; t <= glide_renderer_max_threads(); t++) {
                run_with(k, t, h, cmds, start, fb_bytes, 0, only, upto, point, 1);
                const uint16_t *a = (const uint16_t *) ref, *b = (const uint16_t *) g_voodoo.fb_mem;
                size_t          diff = 0, first = 0;
                for (size_t p = 0; p < fb_bytes / 2; p++)
                    if (a[p] != b[p] && !diff++)
                        first = p;
                if (diff) {
                    size_t px = first % ((size_t) g_aux_offset / 2);
                    printf("compare %-4s threads %d: %zu differing values, first %s (%zu,%zu)\n",
                           glide_renderer_name(k), t, diff, first * 2 < g_aux_offset ? "colour" : "depth",
                           px % h->width, px / h->width);
                    rc = 2;
                } else
                    printf("compare %-4s threads %d: identical\n", glide_renderer_name(k), t);
            }
        }
        /* render again with the chosen path for the PNG */
        run_with(kind, threads, h, cmds, start, fb_bytes, 0, only, upto, point, 1);
    }
    grBufferSwap(1);
    printf("Frame %u: %u commands, %d triangles, %ux%u -> %s\n", h->frame, h->ncmd, tri, h->width, h->height, argv[3]);
    if (save_png(argv[3]))
        rc = 1;
    grGlideShutdown();
    free(start);
    free(ref);
    free(tex);
    free(pal);
    free(cap);
    return rc;
}
