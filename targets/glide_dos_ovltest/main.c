/*
 * OVLTEST.EXE: a real DOS/4GW program that loads GLIDE2X.OVL the way a DOS
 * game does (name lookup in the LE export table via tools/leload, MIT) and
 * renders a picture, with the REAL DPMI layer
 * (platform/dos_ovl/video_dpmi.c) instead of the simulated BIOS of
 * tools/leload_demo. Runs in DOSBox (make dosbox-test, see
 * tools/dosbox_test/run.sh), where the picture is checked by a screenshot
 * of the Xvfb display.
 *
 * Sequence: grSstWinOpen(320x240) -> draw scene -> grBufferSwap -> keep the
 * picture for <hold> seconds -> grGlideShutdown (text mode back) -> print
 * the result and write it to OVLTEST.LOG (unlike shell redirects in the
 * autoexec, a file written by the program itself reliably reaches the
 * host).
 *
 * Usage: OVLTEST [hold_seconds] [ovl_path]
 */

#include "../../platform/dos_ovl/video.h"
#include "../../tools/leload/leload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The same minimal Glide types as tools/leload_demo/main.c; the layout
 * MUST match core/include/glidulator/glide2x.h. */
typedef unsigned int FxU32;
typedef int          FxBool;

typedef struct {
    float sow, tow, oow;
} GrTmuVertex;

typedef struct {
    float       x, y, z;
    float       r, g, b;
    float       ooz;
    float       a;
    float       oow;
    GrTmuVertex tmuvtx[2];
} GrVertex;

#define GR_RESOLUTION_320x240 0x1

typedef void (__stdcall *fn_void)(void);
typedef FxBool (__stdcall *fn_grSstWinOpen)(FxU32, FxU32, FxU32, int, int, int, int);
typedef void (__stdcall *fn_grBufferClear)(FxU32, unsigned char, unsigned short);
typedef void (__stdcall *fn_grDrawTriangle)(const GrVertex *, const GrVertex *, const GrVertex *);
typedef void (__stdcall *fn_grBufferSwap)(int);
typedef int (__stdcall *fn_peek)(int, int);
typedef const vid_mode *(__stdcall *fn_setbios)(const vid_bios *, int);

/* Gu3dfInfo as in core/include/glidulator/glide2x.h (union of NCC table
 * and palette = 1024 bytes). */
typedef struct {
    FxU32 width, height;
    int   small_lod, large_lod, aspect_ratio, format;
    FxU32 table[256];
    void *data;
    FxU32 mem_required;
} Gu3dfInfo;
typedef FxBool (__stdcall *fn_gu3df)(const char *, Gu3dfInfo *);

/* .3DF test file: RGB565 256x128 (one level, 64 KB, more than one 32 KB
 * read chunk in platform_read_file), texel i = i * 7, big-endian. Tests
 * the OVL's DOS file access (INT 21h 3Dh/42h/3Fh), which does not exist on
 * Linux (make ovl-demo). */
#define TDF_TEXELS (256 * 128)

static int write_test_3df(void) {
    FILE *f = fopen("TEST.3DF", "wb");
    if (!f)
        return 0;
    fputs("3df v1.0\nrgb565\nlod range: 256 256\naspect ratio: 2 1\n", f);
    for (unsigned i = 0; i < TDF_TEXELS; i++) {
        unsigned v = (i * 7) & 0xffff;
        fputc((int) (v >> 8), f);
        fputc((int) (v & 0xff), f);
    }
    fclose(f);
    return 1;
}

static void *io_alloc(size_t n) {
    return calloc(1, n);
}

static void io_free(void *p) {
    free(p);
}

static void *io_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = io_alloc((size_t) n);
    if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t) n;
    return buf;
}

static const le_io g_io = { io_alloc, io_free, io_read_file, NULL };

static char g_log[2048];

static void say(const char *s) {
    strncat(g_log, s, sizeof g_log - strlen(g_log) - 1);
}

/* BIOS tick counter (18.2 Hz) at 0040:006C, flat-addressable under
 * DOS/4GW. */
static void wait_seconds(int s) {
    volatile const unsigned long *ticks = (volatile const unsigned long *) 0x46C;
    unsigned long start = *ticks, want = (unsigned long) s * 182 / 10;
    while (*ticks - start < want)
        ;
}

static GrVertex vtx(float x, float y, float r, float g, float b) {
    GrVertex v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.r = r; v.g = g; v.b = b; v.a = 255.0f; v.oow = 1.0f;
    return v;
}

#define RESOLVE(var, type, name)                     \
    do {                                             \
        var = (type) le_proc(m, name);               \
        if (!var) {                                  \
            printf("Export missing: %s\n", name);      \
            return 1;                                \
        }                                            \
    } while (0)

int main(int argc, char **argv) {
    int         hold = argc > 1 ? atoi(argv[1]) : 5;
    const char *path = argc > 2 ? argv[2] : "GLIDE2X.OVL";
    char        err[128], line[160];

    le_module *m = le_load(&g_io, path, err, sizeof err);
    if (!m) {
        printf("le_load(%s): %s\n", path, err);
        return 1;
    }

    fn_void           grGlideInit, grGlideShutdown;
    fn_grSstWinOpen   grSstWinOpen;
    fn_grBufferClear  grBufferClear;
    fn_grDrawTriangle grDrawTriangle;
    fn_grBufferSwap   grBufferSwap;
    fn_peek           peek;
    fn_setbios        setbios;
    RESOLVE(grGlideInit, fn_void, "_GRGLIDEINIT@0");
    RESOLVE(grGlideShutdown, fn_void, "_GRGLIDESHUTDOWN@0");
    RESOLVE(grSstWinOpen, fn_grSstWinOpen, "_GRSSTWINOPEN@28");
    RESOLVE(grBufferClear, fn_grBufferClear, "_GRBUFFERCLEAR@12");
    RESOLVE(grDrawTriangle, fn_grDrawTriangle, "_GRDRAWTRIANGLE@12");
    RESOLVE(grBufferSwap, fn_grBufferSwap, "_GRBUFFERSWAP@4");
    RESOLVE(peek, fn_peek, "_GLIDULATORDEBUGPEEKPIXEL@8");
    RESOLVE(setbios, fn_setbios, "_GLIDULATORDEBUGSETVIDEOBIOS@8");
    fn_gu3df gu3dfGetInfo, gu3dfLoad;
    RESOLVE(gu3dfGetInfo, fn_gu3df, "_GU3DFGETINFO@8");
    RESOLVE(gu3dfLoad, fn_gu3df, "_GU3DFLOAD@8");

    grGlideInit();
    if (!grSstWinOpen(0, GR_RESOLUTION_320x240, 0, 0, 0, 2, 0)) {
        printf("grSstWinOpen failed\n");
        return 1;
    }

    /* Test picture: dark blue background, a flat red triangle on the left,
     * a Gouraud triangle on the right (corners red/green/blue).
     * tools/dosbox_test/check_screenshot.py checks exactly these colours. */
    grBufferClear(0x00000040, 0xff, 0);
    GrVertex a = vtx(40, 20, 255, 0, 0), b = vtx(20, 220, 255, 0, 0), c = vtx(150, 220, 255, 0, 0);
    grDrawTriangle(&a, &b, &c);
    GrVertex d = vtx(235, 20, 255, 0, 0), e = vtx(170, 220, 0, 255, 0), f = vtx(300, 220, 0, 0, 255);
    grDrawTriangle(&d, &e, &f);
    grBufferSwap(1);

    const vid_mode *vm = setbios(NULL, 0); /* query only */
    if (vm)
        sprintf(line, "Mode 0x%03x %dx%dx%d pitch %d\n", vm->mode, vm->width, vm->height, vm->bpp, vm->pitch);
    else
        sprintf(line, "NO output active\n");
    say(line);
    sprintf(line, "peek bg=%04x red=%04x\n", peek(5, 5), peek(60, 180));
    say(line);

    {
        Gu3dfInfo info;
        int       bad = 0;
        memset(&info, 0, sizeof info);
        if (!write_test_3df() || !gu3dfGetInfo("TEST.3DF", &info)) {
            say("3df: GetInfo failed\n");
        } else {
            info.data = malloc(info.mem_required);
            if (!info.data || !gu3dfLoad("TEST.3DF", &info)) {
                say("3df: Load failed\n");
            } else {
                const unsigned short *t = (const unsigned short *) info.data;
                for (unsigned i = 0; i < TDF_TEXELS; i++)
                    if (t[i] != (unsigned short) (i * 7))
                        bad++;
                sprintf(line, "3df %lux%lu mem=%lu wrong=%d\n", (unsigned long) info.width,
                        (unsigned long) info.height, (unsigned long) info.mem_required, bad);
                say(line);
            }
            free(info.data);
        }
    }

    /* Vsync: 60x grBufferSwap(1) must take about one second (60-70 Hz),
     * 60x grBufferSwap(0) almost nothing, measured with the BIOS counter
     * (55 ms). */
    {
        volatile const unsigned long *ticks = (volatile const unsigned long *) 0x46C;
        unsigned long                 t0    = *ticks;
        for (int i = 0; i < 60; i++)
            grBufferSwap(1);
        unsigned long t1 = *ticks;
        for (int i = 0; i < 60; i++)
            grBufferSwap(0);
        unsigned long t2 = *ticks;
        sprintf(line, "vsync 60x: %lu ms, without: %lu ms\n", (t1 - t0) * 10000 / 182, (t2 - t1) * 10000 / 182);
        say(line);
    }

    wait_seconds(hold);
    grGlideShutdown();
    le_unload(m);

    say("OVLTEST END\n");
    fputs(g_log, stdout);
    FILE *lf = fopen("OVLTEST.LOG", "w");
    if (lf) {
        fputs(g_log, lf);
        fclose(lf);
    }
    /* Leave text mode visible briefly: DOSBox adapts its window only with
     * the next rendered frame; without a pause "exit" ends DOSBox before
     * the restored text mode becomes visible. */
    wait_seconds(2);
    return 0;
}
