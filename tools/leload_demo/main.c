/*
 * Loads the real, Open Watcom-built GLIDE2X.OVL dynamically, the same code
 * path a DOS game takes (name lookup in the LE export table, no link-time
 * dependency), and calls it through the real Glide 2.x ABI.
 *
 * Portable thanks to tools/leload/leload.c (MIT, MGA-Glide), which uses
 * callbacks for memory and file access. Must run as a 32-bit process (gcc
 * -m32): the OVL's LE fixups are 32-bit addresses used directly as
 * pointers, and Watcom builds the exports with __stdcall, which does not
 * exist on x86-64.
 */

#include "../leload/leload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void *demo_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    memset(p, 0, n);
    return p;
}

/* le_io.free gets no size, which munmap() needs. A short-lived test
 * program simply does not free. */
static void demo_free(void *p) {
    (void) p;
}

static void *demo_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t) n);
    if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t) n;
    return buf;
}

static void demo_log(const char *msg) {
    fprintf(stderr, "leload: %s\n", msg);
}

static const le_io g_io = { demo_alloc, demo_free, demo_read_file, demo_log };

/* Own minimal type declarations instead of including glide2x.h: keeps the
 * set of types the test relies on explicit. The layout MUST match
 * glide2x.h (generated from abi/types.h.frag, verified against real
 * games). */
typedef unsigned int FxU32;
typedef int          FxI32;
typedef FxI32         FxBool;
typedef FxU32         GrColor_t;
typedef unsigned char GrAlpha_t;
typedef FxI32         GrAspectRatio_t;
typedef FxI32         GrTextureFormat_t;
typedef FxI32         GrLOD_t;
typedef FxU32         GrScreenResolution_t;
typedef FxU32         GrScreenRefresh_t;
typedef FxI32         GrColorFormat_t;
typedef FxI32         GrOriginLocation_t;
typedef FxI32         GrChipID_t;

#define GR_RESOLUTION_320x240 0x1
#define GR_REFRESH_60Hz       0x0
#define GR_COLORFORMAT_ARGB   0x0
#define GR_ORIGIN_UPPER_LEFT  0x0
#define GR_LOD_32             0x3
#define GR_ASPECT_1x1         0x3
#define GR_TEXFMT_RGB_565     0xa
#define GR_TMU0               0x0
#define GR_MIPMAPLEVELMASK_BOTH 0x3

typedef struct {
    float sow, tow, oow;
} GrTmuVertex;

typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex tmuvtx[2];
} GrVertex;

typedef struct {
    GrLOD_t           smallLod;
    GrLOD_t           largeLod;
    GrAspectRatio_t   aspectRatio;
    GrTextureFormat_t format;
    void             *data;
} GrTexInfo;

/* --- Simulated BIOS for the VESA/VGA output -------------------------
 * The layout MUST match platform/dos_ovl/video.h (vid_rmregs/vid_bios/
 * vid_mode, deliberately cdecl there). Simulates a VBE 2.0 card with a few
 * modes (or, with vbe=0, plain VGA without VESA) including 1 MB of
 * "real-mode memory", LFB and DAC. */
typedef struct {
    unsigned int   edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} vid_rmregs;

#define CDECL __attribute__((cdecl))
typedef struct {
    void (CDECL *int10)(vid_rmregs *r);
    void *(CDECL *real_ptr)(unsigned short seg, unsigned short off);
    void *(CDECL *map_phys)(unsigned int phys, unsigned int size);
    void (CDECL *outb)(unsigned short port, unsigned char value);
    unsigned short dosbuf_seg;
    /* Write-combining (wc_dos.c): none here, Linux has no MTRRs to set from
     * user space. */
    int (CDECL *wc_on)(unsigned int phys, unsigned int need, unsigned int vram);
    void (CDECL *wc_off)(void);
} vid_bios;

typedef struct {
    int            width, height, pitch, bpp;
    unsigned char  r_size, r_pos, g_size, g_pos, b_size, b_pos;
    unsigned int   phys_base;
    unsigned short mode;
} vid_mode;

#define FAKE_LFB_PHYS 0xE0000000u
#define FAKE_DOSBUF   0x9000
#define FAKE_LIST_OFF 0x0200 /* mode list at 9000:0200, outside the info buffer */

static unsigned char  fake_lowmem[1 << 20];
static unsigned char  fake_lfb[4 << 20];
static int            fake_vbe;          /* 0: no VESA extension */
static int            fake_cur_mode = 3; /* text mode at "start" */
static int            fake_mapped;
static int            fake_mode_sets;    /* AH=00h calls */
static int            fake_dac_index, fake_dac_writes;
static unsigned char  fake_dac[768];

/* Modes of the simulated card: 640x480x16 (565), 320x240x32, 320x200x16.
 * For 320x240 the 32-bit mode must win (smallest fitting area), although
 * 565 would be the "better" format. */
static const struct { unsigned short num, w, h; unsigned char bpp, rs, rp, gs, gp, bs, bp; } fake_modes[] = {
    { 0x111, 640, 480, 16, 5, 11, 6, 5, 5, 0 },
    { 0x10F, 320, 240, 32, 8, 16, 8, 8, 8, 0 },
    { 0x10E, 320, 200, 16, 5, 11, 6, 5, 5, 0 },
};
#define NFAKE ((int) (sizeof fake_modes / sizeof fake_modes[0]))

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
static void put32(unsigned char *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }

static void CDECL fake_int10(vid_rmregs *r) {
    unsigned       ax  = r->eax & 0xFFFF;
    unsigned char *buf = fake_lowmem + ((unsigned) r->es << 4) + (r->edi & 0xFFFF);
    if (ax == 0x0F00) {
        r->eax = 0x5000 | (unsigned) fake_cur_mode; /* AH=columns, AL=mode */
    } else if ((ax >> 8) == 0x00) {
        fake_cur_mode = ax & 0x7F;
        fake_mode_sets++;
    } else if (!fake_vbe) {
        return; /* no VESA extension: AX stays unchanged (!= 004Fh) */
    } else if (ax == 0x4F00) {
        memcpy(buf, "VESA", 4);
        put16(buf + 4, 0x0200);
        put32(buf + 14, ((unsigned) FAKE_DOSBUF << 16) | FAKE_LIST_OFF);
        unsigned char *list = fake_lowmem + (FAKE_DOSBUF << 4) + FAKE_LIST_OFF;
        for (int i = 0; i < NFAKE; i++)
            put16(list + 2 * i, fake_modes[i].num);
        put16(list + 2 * NFAKE, 0xFFFF);
        r->eax = 0x004F;
    } else if (ax == 0x4F01) {
        r->eax = 0x014F;
        for (int i = 0; i < NFAKE; i++) {
            if (fake_modes[i].num != (r->ecx & 0x1FF))
                continue;
            memset(buf, 0, 256);
            put16(buf + 0, 0x009B); /* supported, color, graphics, LFB */
            put16(buf + 16, fake_modes[i].w * ((fake_modes[i].bpp + 7) / 8));
            put16(buf + 18, fake_modes[i].w);
            put16(buf + 20, fake_modes[i].h);
            buf[25] = fake_modes[i].bpp;
            buf[27] = 6; /* direct color */
            buf[31] = fake_modes[i].rs; buf[32] = fake_modes[i].rp;
            buf[33] = fake_modes[i].gs; buf[34] = fake_modes[i].gp;
            buf[35] = fake_modes[i].bs; buf[36] = fake_modes[i].bp;
            put32(buf + 40, FAKE_LFB_PHYS);
            r->eax = 0x004F;
        }
    } else if (ax == 0x4F03) {
        r->ebx = (unsigned) fake_cur_mode;
        r->eax = 0x004F;
    } else if (ax == 0x4F02) {
        fake_cur_mode = (int) (r->ebx & 0x1FF) | ((r->ebx & 0x4000) ? 0x4000 : 0);
        r->eax = 0x004F;
    }
}

static void *CDECL fake_real_ptr(unsigned short seg, unsigned short off) {
    return fake_lowmem + ((unsigned) seg << 4) + off;
}

static void *CDECL fake_map_phys(unsigned int phys, unsigned int size) {
    if (phys != FAKE_LFB_PHYS || size > sizeof fake_lfb)
        return NULL;
    fake_mapped++;
    return fake_lfb;
}

static void CDECL fake_outb(unsigned short port, unsigned char v) {
    if (port == 0x3C8)
        fake_dac_index = v * 3;
    else if (port == 0x3C9 && fake_dac_index < 768) {
        fake_dac[fake_dac_index++] = v;
        fake_dac_writes++;
    }
}

static const vid_bios fake_bios = { fake_int10, fake_real_ptr, fake_map_phys, fake_outb, FAKE_DOSBUF, 0, 0 };

#define STD __attribute__((stdcall))
typedef void (STD *fn_grGlideInit)(void);
typedef void (STD *fn_grGlideShutdown)(void);
typedef FxBool (STD *fn_grSstWinOpen)(FxU32, GrScreenResolution_t, GrScreenRefresh_t,
                                      GrColorFormat_t, GrOriginLocation_t, int, int);
typedef void (STD *fn_grTexDownloadMipMap)(GrChipID_t, FxU32, FxU32, GrTexInfo *);
typedef void (STD *fn_grTexSource)(GrChipID_t, FxU32, FxU32, GrTexInfo *);
typedef void (STD *fn_grBufferClear)(GrColor_t, GrAlpha_t, unsigned short);
typedef void (STD *fn_grDrawTriangle)(const GrVertex *, const GrVertex *, const GrVertex *);
typedef void (STD *fn_grBufferSwap)(int);
typedef int (STD *fn_peek)(int, int);
typedef const vid_mode *(STD *fn_setbios)(const vid_bios *, int);
typedef void (STD *fn_grColorCombine)(FxI32, FxI32, FxI32, FxI32, FxBool);
typedef void (STD *fn_grTexFilterMode)(GrChipID_t, FxI32, FxI32);
typedef void (STD *fn_grTexClampMode)(GrChipID_t, FxI32, FxI32);
typedef FxU32 (STD *fn_grSstScreenWidth)(void);
typedef void (STD *fn_grGlideState)(void *); /* Get/SetState: GrState * (312 bytes) */
typedef float (STD *fn_guFogTableIndexToW)(int);
typedef void *(__attribute__((cdecl)) *host_alloc_fn)(size_t);
typedef void (__attribute__((cdecl)) *host_free_fn)(void *);
typedef void (STD *fn_setmem)(host_alloc_fn, host_free_fn);
typedef void (STD *fn_guFogGenerateExp)(unsigned char *, float);

#define RESOLVE(var, type, name)                                  \
    do {                                                          \
        var = (type) le_proc(m, name);                            \
        if (!var) {                                               \
            fprintf(stderr, "Export not found: %s\n", name); \
            return 1;                                             \
        }                                                         \
    } while (0)

typedef struct {
    fn_grSstScreenWidth grSstScreenWidth;
    fn_grGlideState grGlideGetState, grGlideSetState;
    fn_guFogTableIndexToW guFogTableIndexToW;
    fn_setmem setmem;
    fn_guFogGenerateExp guFogGenerateExp;
    fn_grGlideInit grGlideInit;
    fn_grGlideShutdown grGlideShutdown;
    fn_grSstWinOpen grSstWinOpen;
    fn_grBufferClear grBufferClear;
    fn_grDrawTriangle grDrawTriangle;
    fn_grBufferSwap grBufferSwap;
    fn_peek peek;
    fn_setbios setbios;
    fn_grTexDownloadMipMap grTexDownloadMipMap;
    fn_grTexSource grTexSource;
    fn_grColorCombine grColorCombine;
    fn_grTexFilterMode grTexFilterMode;
    fn_grTexClampMode grTexClampMode;
} glide_fns;

static int open_and_draw(const glide_fns *g) {
    g->grGlideInit();
    if (!g->grSstWinOpen(0, GR_RESOLUTION_320x240, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                         GR_ORIGIN_UPPER_LEFT, 1, 0)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }
    g->grBufferClear(0x00000000, 0xff, 0);

    GrVertex v0 = { 32.0f, 8.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v1 = { 8.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    GrVertex v2 = { 56.0f, 56.0f, 0, 255.0f, 0.0f, 0.0f, 0, 255.0f, 1.0f, { { 0, 0, 0 }, { 0, 0, 0 } } };
    g->grDrawTriangle(&v1, &v0, &v2);
    g->grBufferSwap(1);
    return 0;
}

static void *host_alloc(size_t n) {
    return malloc(n);
}

static void host_free(void *p) {
    free(p);
}

#define EXPECT(cond, ...)                         \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAILED: ");  \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            return 1;                             \
        }                                         \
    } while (0)

static unsigned int lfb32(int x, int y) {
    const unsigned char *p = fake_lfb + (y * 320 + x) * 4;
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned) p[3] << 24);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "build/ovl/GLIDE2X.OVL";
    char err[128];

    le_module *m = le_load(&g_io, path, err, sizeof err);
    if (!m) {
        fprintf(stderr, "le_load(%s) failed: %s\n", path, err);
        return 1;
    }
    printf("loaded: %s, module=\"%s\", flags=0x%x, %d objects, %d fixups\n",
           path, le_module_name(m), le_module_flags(m), le_object_count(m), le_fixup_count(m));

    glide_fns g;

    RESOLVE(g.grGlideInit, fn_grGlideInit, "_GRGLIDEINIT@0");
    RESOLVE(g.grGlideShutdown, fn_grGlideShutdown, "_GRGLIDESHUTDOWN@0");
    RESOLVE(g.grSstWinOpen, fn_grSstWinOpen, "_GRSSTWINOPEN@28");
    RESOLVE(g.grTexDownloadMipMap, fn_grTexDownloadMipMap, "_GRTEXDOWNLOADMIPMAP@16");
    RESOLVE(g.grTexSource, fn_grTexSource, "_GRTEXSOURCE@16");
    RESOLVE(g.grColorCombine, fn_grColorCombine, "_GRCOLORCOMBINE@20");
    RESOLVE(g.grTexFilterMode, fn_grTexFilterMode, "_GRTEXFILTERMODE@12");
    RESOLVE(g.grTexClampMode, fn_grTexClampMode, "_GRTEXCLAMPMODE@12");
    RESOLVE(g.grBufferClear, fn_grBufferClear, "_GRBUFFERCLEAR@12");
    RESOLVE(g.grDrawTriangle, fn_grDrawTriangle, "_GRDRAWTRIANGLE@12");
    RESOLVE(g.grBufferSwap, fn_grBufferSwap, "_GRBUFFERSWAP@4");
    RESOLVE(g.peek, fn_peek, "_GLIDULATORDEBUGPEEKPIXEL@8");
    RESOLVE(g.setbios, fn_setbios, "_GLIDULATORDEBUGSETVIDEOBIOS@8");
    RESOLVE(g.grSstScreenWidth, fn_grSstScreenWidth, "_GRSSTSCREENWIDTH@0");
    RESOLVE(g.grGlideGetState, fn_grGlideState, "_GRGLIDEGETSTATE@4");
    RESOLVE(g.grGlideSetState, fn_grGlideState, "_GRGLIDESETSTATE@4");
    RESOLVE(g.guFogTableIndexToW, fn_guFogTableIndexToW, "_GUFOGTABLEINDEXTOW@4");
    RESOLVE(g.guFogGenerateExp, fn_guFogGenerateExp, "_GUFOGGENERATEEXP@8");
    RESOLVE(g.setmem, fn_setmem, "_GLIDULATORDEBUGSETMEMORY@8");
    printf("all 19 exports resolved\n");
    /* The OVL otherwise allocates memory via DPMI (INT 31h), which Linux does
     * not have. */
    g.setmem(host_alloc, host_free);

    /* The only Glide function returning float: result in ST(0) as usual for
     * __stdcall (otherwise the caller reads garbage), values
     * W(i) = 2^(3 + i/4) / (8 - i%4); pow/exp from Watcom's math library. */
    {
        float         w0 = g.guFogTableIndexToW(0), w30 = g.guFogTableIndexToW(30);
        unsigned char ft[64];
        g.guFogGenerateExp(ft, 0.01f);
        printf("Fog (Watcom build): W(0)=%.4f W(30)=%.4f Exp[0]=%d Exp[32]=%d Exp[63]=%d\n", w0, w30, ft[0],
               ft[32], ft[63]);
        EXPECT(w0 == 1.0f && w30 > 170.66f && w30 < 170.67f, "guFogTableIndexToW (float return/pow)");
        EXPECT(ft[0] <= 3 && ft[32] > 100 && ft[63] >= 254, "guFogGenerateExp (exp)");
    }

    /* Run 1: VBE 2.0 card. Expected: 320x240x32 via LFB, triangle 1:1 in the
     * LFB, text mode 3 again after grGlideShutdown. */
    fake_vbe = 1;
    g.setbios(&fake_bios, 1);
    if (open_and_draw(&g))
        return 1;

    int center = g.peek(32, 45);
    int corner = g.peek(2, 2);
    printf("centre (32,45) = 0x%04x (expected 0xf800)\n", center);
    printf("corner  (2,2)   = 0x%04x (expected 0x0000)\n", corner);
    EXPECT(center == 0xf800 && corner == 0x0000, "rasterizer pixels");

    const vid_mode *vm = g.setbios(NULL, 0);
    EXPECT(vm, "no video mode set");
    printf("VBE: mode 0x%03x %dx%dx%d, BIOS mode now 0x%x\n", vm->mode, vm->width, vm->height, vm->bpp,
           fake_cur_mode);
    EXPECT(vm->mode == 0x10F && fake_cur_mode == (0x10F | 0x4000), "expected VBE mode 0x10F with LFB bit");
    printf("LFB (32,45) = 0x%08x, (2,2) = 0x%08x\n", lfb32(32, 45), lfb32(2, 2));
    EXPECT(lfb32(32, 45) == 0x00ff0000 && lfb32(2, 2) == 0, "LFB content (expected 0x00ff0000 / 0)");

    g.grGlideShutdown();
    EXPECT(fake_cur_mode == 3, "text mode not restored after grGlideShutdown (0x%x)", fake_cur_mode);
    EXPECT(g.setbios(NULL, 0) == NULL, "display still active after grGlideShutdown");

    /* Run 1b: the program switches back to text mode itself and prints its
     * exit text before grGlideShutdown. Setting the mode again would clear
     * that text: Glidulator must leave the screen alone. */
    if (open_and_draw(&g))
        return 1;
    {
        vid_rmregs r;
        memset(&r, 0, sizeof r);
        r.eax = 0x0003;
        fake_int10(&r);
    }
    int sets = fake_mode_sets;
    g.grGlideShutdown();
    printf("program switches to text mode itself: %d further mode switches\n", fake_mode_sets - sets);
    EXPECT(fake_mode_sets == sets && fake_cur_mode == 3, "mode set again after the program's own text mode");

    /* Run 2: plain VGA without VESA -> mode 13h fallback. 320x240 is
     * stretched to 320x200, red (565 0xf800) -> palette index 210 (5 * 42, 6x7x6). */
    fake_vbe = 0;
    memset(fake_lowmem, 0, sizeof fake_lowmem);
    if (open_and_draw(&g))
        return 1;
    vm = g.setbios(NULL, 0);
    EXPECT(vm && vm->mode == 0x13 && fake_cur_mode == 0x13, "expected mode 13h fallback");
    const unsigned char *vga = fake_lowmem + 0xA0000;
    int sy = 38; /* target line 38 shows source line 38 * 240 / 200 = 45 */
    printf("Mode 13h: Pixel (32,%d) = 0x%02x, (2,2) = 0x%02x, %d DAC-Writes, DAC[210] = %d/%d/%d\n",
           sy, vga[sy * 320 + 32], vga[2 * 320 + 2], fake_dac_writes, fake_dac[210 * 3], fake_dac[210 * 3 + 1],
           fake_dac[210 * 3 + 2]);
    EXPECT(vga[sy * 320 + 32] == 210 && vga[2 * 320 + 2] == 0, "VGA memory content");
    EXPECT(fake_dac_writes == 768 && fake_dac[210 * 3] == 63 && fake_dac[210 * 3 + 1] == 0,
           "RGB332 palette not set correctly");
    g.grGlideShutdown();
    EXPECT(fake_cur_mode == 3, "text mode not restored after mode 13h");

    /* Run 3: headless (NULL); Glide keeps working without BIOS calls. Plus a
     * perspective-textured strip computed by the Watcom build: the same
     * check as test 6 in targets/glide_test_linux. Catches differences of
     * the Watcom runtime that the GCC build never sees (e.g. Watcom's
     * llrint() returning 0 for values beyond 32 bits). */
    g.setbios(NULL, 1);
    if (open_and_draw(&g))
        return 1;
    EXPECT(g.setbios(NULL, 0) == NULL && g.peek(32, 45) == 0xf800, "headless operation");

    static unsigned short tex[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            tex[y * 32 + x] = (unsigned short) x;
    GrTexInfo info = { GR_LOD_32, GR_LOD_32, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, tex };
    g.grTexDownloadMipMap(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    g.grTexSource(GR_TMU0, 0, GR_MIPMAPLEVELMASK_BOTH, &info);
    g.grTexFilterMode(GR_TMU0, 0, 0);  /* point sampling */
    g.grTexClampMode(GR_TMU0, 1, 1);   /* clamp */
    g.grColorCombine(3, 8, 0, 1, 0);   /* SCALE_OTHER, ONE, ITERATED, TEXTURE */
    /* Save the state, change it (constant colour instead of texture, wrap)
     * and restore it: the strip must still be textured. */
    EXPECT(g.grSstScreenWidth() == 320, "grSstScreenWidth = %u", (unsigned) g.grSstScreenWidth());
    static unsigned char state[312];
    g.grGlideGetState(state);
    g.grColorCombine(1, 0, 1, 2, 0);   /* LOCAL, NONE, CONSTANT, OTHER_NONE */
    g.grTexClampMode(GR_TMU0, 0, 0);
    g.grGlideSetState(state);
    /* Strip x 0..256, left oow 1, right oow 0.25 (4x as far); sow/tow =
     * s*oow, s 0..256. */
    GrVertex s0 = { 0, 100, 0, 255, 255, 255, 0, 255, 1.0f, { { 0, 0, 1.0f }, { 0, 0, 0 } } };
    GrVertex s1 = { 256, 100, 0, 255, 255, 255, 0, 255, 0.25f, { { 64.0f, 0, 0.25f }, { 0, 0, 0 } } };
    GrVertex s2 = { 256, 140, 0, 255, 255, 255, 0, 255, 0.25f, { { 64.0f, 64.0f, 0.25f }, { 0, 0, 0 } } };
    GrVertex s3 = { 0, 140, 0, 255, 255, 255, 0, 255, 1.0f, { { 0, 256.0f, 1.0f }, { 0, 0, 0 } } };
    g.grDrawTriangle(&s0, &s1, &s2);
    g.grDrawTriangle(&s0, &s2, &s3);
    g.grBufferSwap(1);
    int texel = g.peek(128, 110) & 0x1f;
    printf("Perspective (Watcom build): texel in the centre %d (expected 6, linear would be 16)\n", texel);
    EXPECT(texel >= 5 && texel <= 7, "perspective texture wrong in the Watcom OVL");
    g.grGlideShutdown();

    le_unload(m);
    printf("ok -- GLIDE2X.OVL loaded dynamically and ran correctly (VBE LFB, mode 13h, headless, perspective, Get/SetState, fog)\n");
    return 0;
}
